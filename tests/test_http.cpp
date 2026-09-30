// The HTTP message layer and client, against loopback servers.
#include "protoio/error.h"
#include "protoio/http.h"
#include "protoio/net.h"
#include "protoio/stream.h"
#include "test_support.h"

#include <gtest/gtest.h>

#include <mutex>
#include <sys/socket.h>

using protoio::Error;
namespace http = protoio::http;
using protoio_test::HttpServer;
using protoio_test::RawServer;
using protoio_test::Served;

namespace {

template <typename F>
Error errorOf(F f) {
    try {
        f();
    } catch (const Error& e) {
        return e;
    }
    ADD_FAILURE() << "no Error was thrown";
    return Error(Error::Kind::FileSystem, "none");
}

bool isInvalid(const Error& e) {
    return e.kind == Error::Kind::InvalidArgument && std::string(e.what()).find("invalid") != std::string::npos;
}

// A connected pair: the test writes raw bytes to `peer` and the library
// parses them from `fd`.
struct Wire {
    int fd = -1, peer = -1;
    explicit Wire(const std::string& bytes, bool closePeer = true) {
        int s[2];
        if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, s) != 0) throw std::runtime_error("socketpair");
        fd = s[0];
        peer = s[1];
        protoio::forget(fd);
        protoio::forget(peer);
        protoio::write(peer, bytes);
        if (closePeer) { protoio::close(peer); peer = -1; }
    }
    ~Wire() {
        protoio::close(fd);
        if (peer >= 0) protoio::close(peer);
    }
};

int refusalOf(const std::string& bytes, const http::Limits& limits = {}) {
    Wire w(bytes);
    try {
        http::readRequestHead(w.fd, limits);
    } catch (const http::HttpRefusal& e) {
        return e.status;
    }
    return 0;
}

} // namespace

// ------------------------------------------------------------ validation

TEST(HttpValidation, RefusesControlCharactersAndSeparators) {
    EXPECT_NO_THROW(http::validateToken("GET"));
    EXPECT_NO_THROW(http::validateToken("x-custom-header"));
    EXPECT_NO_THROW(http::validateValue("text/plain; charset=utf-8\tok"));
    EXPECT_NO_THROW(http::validateTarget("/a/b?x=1&y=%20"));
    EXPECT_TRUE(isInvalid(errorOf([] { http::validateToken("GE T", "method"); })));
    EXPECT_TRUE(isInvalid(errorOf([] { http::validateToken(""); })));
    EXPECT_TRUE(isInvalid(errorOf([] { http::validateToken("a:b"); })));
    EXPECT_TRUE(isInvalid(errorOf([] { http::validateValue("v\r\nX-Evil: yes"); })));
    EXPECT_TRUE(isInvalid(errorOf([] { http::validateValue(std::string("v\0w", 3)); })));
    EXPECT_TRUE(isInvalid(errorOf([] { http::validateValue("v\x7f"); })));
    EXPECT_TRUE(isInvalid(errorOf([] { http::validateTarget("/b HTTP/1.1\r\nX-Injected: 1"); })));
    EXPECT_TRUE(isInvalid(errorOf([] { http::validateHeaders({{"x-a", "v\nw"}}); })));
}

TEST(HttpValidation, HeaderInjectionFromTheClientNeverReachesTheWire) {
    // Port 9 (discard) is closed on loopback: had the client connected before
    // validating, these would fail with ConnectionRefused instead.
    const std::string crlf = "\r\n";
    http::Request a;
    a.url = "http://127.0.0.1:9/b HTTP/1.1" + crlf + "X-Injected: 1";
    EXPECT_TRUE(isInvalid(errorOf([&] { http::httpRequest(a); })));
    http::Request b;
    b.url = "http://127.0.0.1:9/";
    b.headers = {{"x-a", "v" + crlf + "X-Evil: yes"}};
    EXPECT_TRUE(isInvalid(errorOf([&] { http::httpRequest(b); })));
    http::Request c;
    c.method = "GE T";
    c.url = "http://127.0.0.1:9/";
    EXPECT_TRUE(isInvalid(errorOf([&] { http::httpRequest(c); })));
}

TEST(HttpValidation, AnInjectedResponseHeaderIsRefusedBeforeWriting) {
    Wire w("", false);
    http::ResponseHead r;
    r.headers = {{"x-a", "v\r\nSet-Cookie: evil"}};
    EXPECT_TRUE(isInvalid(errorOf([&] { http::writeResponse(w.fd, r, "x"); })));
    http::ResponseHead bad;
    bad.reason = "OK\r\nX: y";
    EXPECT_TRUE(isInvalid(errorOf([&] { http::writeResponse(w.fd, bad, "x"); })));
    protoio::close(w.fd);
    w.fd = -1;
    EXPECT_EQ(protoio::readAll(w.peer), "");  // nothing was written
}

// --------------------------------------------------------------- parsing

TEST(HttpParsing, ReadsARequestHeadAndBody) {
    Wire w("POST /p/a%20th?x=1 HTTP/1.1\r\nHost: h\r\nX-Two:  spaced  \r\nno colon line\r\n"
           "Content-Length: 5\r\n\r\nhelloEXTRA");
    auto head = http::readRequestHead(w.fd);
    ASSERT_TRUE(head);
    EXPECT_EQ(head->method, "POST");
    EXPECT_EQ(head->target, "/p/a%20th?x=1");
    EXPECT_EQ(head->version, "HTTP/1.1");
    ASSERT_EQ(head->headers.size(), 3u);
    EXPECT_EQ(head->headers[0].name, "host");
    EXPECT_EQ(head->headers[1].name, "x-two");
    EXPECT_EQ(head->headers[1].value, "spaced");
    EXPECT_EQ(http::find(head->headers, "X-TWO"), "spaced");
    EXPECT_EQ(http::readBody(w.fd, head->headers), "hello");
}

TEST(HttpParsing, CleanEndOfStreamBeforeARequestIsNullopt) {
    Wire w("");
    EXPECT_EQ(http::readRequestHead(w.fd), std::nullopt);
    Wire w2("\r\n");
    EXPECT_EQ(http::readRequestHead(w2.fd), std::nullopt);
}

TEST(HttpParsing, LimitsAnswer400_414_431_413) {
    const std::string crlf = "\r\n";
    EXPECT_EQ(refusalOf("POST / HTTP/1.1" + crlf + "content-length: -5" + crlf + crlf), 400);
    EXPECT_EQ(refusalOf("POST / HTTP/1.1" + crlf + "content-length: 12abc" + crlf + crlf), 400);
    EXPECT_EQ(refusalOf("POST / HTTP/1.1" + crlf + "content-length: 3" + crlf + "content-length: 4" + crlf + crlf),
              400);
    EXPECT_EQ(refusalOf("GARBAGE" + crlf + crlf), 400);
    EXPECT_EQ(refusalOf("GET /" + std::string(9000, 'a') + " HTTP/1.1" + crlf + crlf), 414);
    EXPECT_EQ(refusalOf("GET / HTTP/1.1" + crlf + "x-long: " + std::string(9000, 'a') + crlf + crlf), 431);
    std::string many = "GET / HTTP/1.1" + crlf;
    for (int i = 0; i < 101; ++i) many += "x-h" + std::to_string(i) + ": v" + crlf;
    EXPECT_EQ(refusalOf(many + crlf), 431);
    EXPECT_EQ(refusalOf("POST / HTTP/1.1" + crlf + "content-length: 999999999999" + crlf + crlf), 413);
    http::Limits small;
    small.maxBody = 10;
    EXPECT_EQ(refusalOf("POST / HTTP/1.1" + crlf + "content-length: 11" + crlf + crlf, small), 413);
    EXPECT_EQ(refusalOf("POST / HTTP/1.1" + crlf + "content-length: 10" + crlf + crlf, small), 0);
}

TEST(HttpParsing, RefusalsHaveKinds) {
    Wire w("GET / HTTP/1.1\r\nx-long: " + std::string(9000, 'a') + "\r\n\r\n");
    try {
        http::readRequestHead(w.fd);
        FAIL();
    } catch (const http::HttpRefusal& e) {
        EXPECT_EQ(e.status, 431);
        EXPECT_EQ(e.kind, Error::Kind::LineTooLong);
    }
}

TEST(HttpParsing, ChunkedBodyIsDecodedAsBytes) {
    // "é" (C3 A9) is split across two chunks; an extension and a trailer are skipped.
    Wire w("2;ext=1\r\na\xC3\r\n1\r\n\xA9\r\n0\r\nx-trailer: t\r\n\r\nNEXT");
    const http::Headers h = {{"transfer-encoding", "chunked"}};
    EXPECT_EQ(http::readBody(w.fd, h), "a\xC3\xA9");
    EXPECT_EQ(protoio::readAll(w.fd), "NEXT");
}

TEST(HttpParsing, BodyLimitsAndMalformedBodies) {
    const http::Headers chunked = {{"transfer-encoding", "chunked"}};
    {
        Wire w("5\r\nhello\r\n5\r\nworld\r\n0\r\n\r\n");
        EXPECT_EQ(errorOf([&] { http::readBody(w.fd, chunked, 8); }).kind, Error::Kind::BodyTooLarge);
    }
    {
        Wire w("zz\r\nhello\r\n0\r\n\r\n");
        EXPECT_EQ(errorOf([&] { http::readBody(w.fd, chunked); }).kind, Error::Kind::Network);
    }
    {
        Wire w("5\r\nhel");
        EXPECT_EQ(errorOf([&] { http::readBody(w.fd, chunked); }).kind, Error::Kind::Network);
    }
    {
        Wire w("0123456789");
        EXPECT_EQ(errorOf([&] { http::readBody(w.fd, {{"content-length", "10"}}, 5); }).kind,
                  Error::Kind::BodyTooLarge);
    }
    {
        Wire w("short");
        EXPECT_EQ(errorOf([&] { http::readBody(w.fd, {{"content-length", "10"}}); }).kind, Error::Kind::Network);
    }
    {
        Wire w("anything");
        EXPECT_EQ(http::readBody(w.fd, {}), "");
    }
}

TEST(HttpParsing, ResponseHead) {
    Wire w("HTTP/1.1 404 Not Found Here\r\nContent-Type: text/plain\r\n\r\n");
    auto r = http::readResponseHead(w.fd);
    EXPECT_EQ(r.status, 404);
    EXPECT_EQ(r.reason, "Not Found Here");
    EXPECT_EQ(http::find(r.headers, "content-type"), "text/plain");
    Wire bad("SMTP ready\r\n\r\n");
    EXPECT_EQ(errorOf([&] { http::readResponseHead(bad.fd); }).kind, Error::Kind::Network);
    Wire none("");
    EXPECT_EQ(errorOf([&] { http::readResponseHead(none.fd); }).kind, Error::Kind::Network);
}

// ---------------------------------------------------------- serialisation

TEST(HttpSerialisation, ContentLengthOnlyWithABodyOrABodyMethod) {
    http::RequestHead get{"GET", "/", "HTTP/1.1", {{"host", "h"}, {"content-length", "99"}}};
    const std::string g = http::serializeRequest(get, std::nullopt);
    EXPECT_EQ(g, "GET / HTTP/1.1\r\nhost: h\r\nconnection: close\r\n\r\n");
    http::RequestHead post{"POST", "/x", "", {{"host", "h"}}};
    EXPECT_EQ(http::serializeRequest(post, std::nullopt),
              "POST /x HTTP/1.1\r\nhost: h\r\ncontent-length: 0\r\nconnection: close\r\n\r\n");
    EXPECT_EQ(http::serializeRequest(post, std::string("héllo")),
              "POST /x HTTP/1.1\r\nhost: h\r\ncontent-length: 6\r\nconnection: close\r\n\r\nhéllo");
    http::RequestHead del{"DELETE", "/x", "", {}};
    EXPECT_EQ(http::serializeRequest(del, std::nullopt).find("content-length"), std::string::npos);
}

TEST(HttpSerialisation, Response) {
    http::ResponseHead r;
    r.status = 201;
    r.headers = {{"Content-Type", "text/plain"}, {"Connection", "keep-alive"}};
    EXPECT_EQ(http::serializeResponse(r, "ok"),
              "HTTP/1.1 201 Created\r\nContent-Type: text/plain\r\ncontent-length: 2\r\nconnection: close\r\n\r\nok");
    http::ResponseHead n;
    n.status = 204;
    EXPECT_EQ(http::serializeResponse(n, "ignored"), "HTTP/1.1 204 No Content\r\nconnection: close\r\n\r\n");
    EXPECT_EQ(http::reasonPhrase(404), "Not Found");
    EXPECT_EQ(http::reasonPhrase(299), "Status");
}

// ----------------------------------------------------------------- helpers

TEST(HttpHelpers, PercentDecodeYieldsUtf8AndKeepsPlus) {
    EXPECT_EQ(http::percentDecode("%C3%A9"), "\xC3\xA9");
    EXPECT_EQ(http::percentDecode("%C3%A9").size(), 2u);
    EXPECT_EQ(http::percentDecode("a+b"), "a+b");
    EXPECT_EQ(http::percentDecode("100%"), "100%");
    EXPECT_EQ(http::percentDecode("%zz%4"), "%zz%4");
    EXPECT_EQ(http::percentDecode("%2f%2F"), "//");
}

TEST(HttpHelpers, ParseQueryPlusIsASpaceThenDecode) {
    auto q = http::parseQuery("x=a+b&y=c%2Bd&flag&e=&=v&&k=%C3%A9");
    using P = std::pair<std::string, std::string>;
    EXPECT_EQ(q, (std::vector<P>{{"x", "a b"}, {"y", "c+d"}, {"flag", ""}, {"e", ""}, {"", "v"}, {"k", "\xC3\xA9"}}));
    EXPECT_TRUE(http::parseQuery("").empty());
}

TEST(HttpHelpers, ParseUrl) {
    auto u = http::parseUrl("HTTP://example.com");
    EXPECT_EQ(u.scheme, "http");
    EXPECT_EQ(u.host, "example.com");
    EXPECT_EQ(u.port, 80);
    EXPECT_EQ(u.target, "/");
    auto s = http::parseUrl("https://h:8443/a/b?q=1");
    EXPECT_EQ(s.port, 8443);
    EXPECT_EQ(s.target, "/a/b?q=1");
    EXPECT_EQ(http::authority(s), "h:8443");
    EXPECT_EQ(http::origin(s), "https://h:8443");
    EXPECT_EQ(http::parseUrl("https://h").port, 443);
    EXPECT_EQ(http::authority(http::parseUrl("https://h:443/")), "h");
    EXPECT_EQ(http::parseUrl("http://h?q").target, "/?q");
    auto v6 = http::parseUrl("http://[::1]:8080/x");
    EXPECT_EQ(v6.host, "::1");
    EXPECT_EQ(v6.port, 8080);
    EXPECT_EQ(http::authority(v6), "[::1]:8080");
    EXPECT_EQ(errorOf([] { http::parseUrl("example.com/x"); }).kind, Error::Kind::InvalidArgument);
    EXPECT_EQ(errorOf([] { http::parseUrl("http://h:abc/"); }).kind, Error::Kind::InvalidArgument);
    EXPECT_EQ(errorOf([] { http::parseUrl("http://h:99999/"); }).kind, Error::Kind::InvalidArgument);
    EXPECT_EQ(errorOf([] { http::parseUrl("http:///x"); }).kind, Error::Kind::InvalidArgument);
}

TEST(HttpHelpers, RedirectResolutionTable) {
    struct Case { const char* location; const char* base; const char* expected; };
    const Case cases[] = {
        // protoST tests/conformance/14-io/http_redirects.st
        {"../x", "http://h/a/b/c", "http://h/a/x"},
        {"x?q=1", "http://h/a/b/c", "http://h/a/b/x?q=1"},
        {"//g/y", "https://h/a", "https://g/y"},
        {"next", "http://127.0.0.1:8080/dir/start", "http://127.0.0.1:8080/dir/next"},
        // RFC 3986 section 5.4 style cases
        {"/abs/./p", "http://h/a/b", "http://h/abs/p"},
        {"?y", "http://h/a/b?x", "http://h/a/b?y"},
        {"./", "http://h/a/b/c", "http://h/a/b/"},
        {"..", "http://h/a/b/c", "http://h/a/"},
        {"../../../../g", "http://h/a/b/c", "http://h/g"},
        {"g/..", "http://h/a/b/c", "http://h/a/b/"},
        {"https://other/z", "http://h/a", "https://other/z"},
        {"http://h2/z", "http://h/a", "http://h2/z"},
        {"c", "http://h:8080/a/b", "http://h:8080/a/c"},
        {"c", "https://h:443/a/b", "https://h/a/c"},
    };
    for (const Case& c : cases) EXPECT_EQ(http::resolveRedirect(c.location, c.base), c.expected) << c.location;
    Error down = errorOf([] { http::resolveRedirect("http://h/x", "https://h/a"); });
    EXPECT_EQ(down.kind, Error::Kind::Network);
    // A scheme-relative location keeps https, so it is not refused.
    EXPECT_NO_THROW(http::resolveRedirect("//g/y", "https://h/a"));
}

// ------------------------------------------------------------------ client

TEST(HttpClient, GetAgainstALoopbackServer) {
    HttpServer server([](const Served& req, http::ResponseHead& res) {
        res.headers = {{"Content-Type", "text/plain"}, {"X-Method", req.head.method}};
        return "you asked for " + req.head.target;
    });
    http::Request r;
    r.url = server.url("/hello?x=1");
    auto resp = http::httpRequest(r);
    EXPECT_EQ(resp.status, 200);
    EXPECT_EQ(resp.reason, "OK");
    EXPECT_EQ(resp.body, "you asked for /hello?x=1");
    EXPECT_EQ(http::find(resp.headers, "x-method"), "GET");
    EXPECT_EQ(resp.headers.front().name, "content-type");  // lower-case names
}

TEST(HttpClient, PostSendsTheBody) {
    HttpServer server([](const Served& req, http::ResponseHead&) {
        return req.head.method + ":" + req.body + ":" + http::find(req.head.headers, "content-type").value_or("-");
    });
    http::Request r;
    r.method = "POST";
    r.url = server.url();
    r.body = "data é";
    EXPECT_EQ(http::httpRequest(r).body, "POST:data é:text/plain; charset=utf-8");
    r.headers = {{"Content-Type", "application/json"}};
    r.body = "{}";
    EXPECT_EQ(http::httpRequest(r).body, "POST:{}:application/json");
}

TEST(HttpClient, GetHasNoContentLengthPostDoesAndTheUserAgentNamesTheVersion) {
    std::mutex m;
    std::vector<http::Headers> seen;
    HttpServer server([&](const Served& req, http::ResponseHead& res) {
        std::lock_guard<std::mutex> lock(m);
        seen.push_back(req.head.headers);
        res.status = 204;
        return std::string();
    });
    http::Request get;
    get.url = server.url();
    EXPECT_EQ(http::httpRequest(get).status, 204);
    http::Request post;
    post.method = "POST";
    post.url = server.url();
    post.body = "x";
    http::httpRequest(post);
    std::lock_guard<std::mutex> lock(m);
    ASSERT_EQ(seen.size(), 2u);
    EXPECT_EQ(http::find(seen[0], "content-length"), std::nullopt);
    EXPECT_EQ(http::find(seen[1], "content-length"), "1");
    EXPECT_EQ(http::find(seen[0], "user-agent"), "protoIO/0.1.0");
    EXPECT_EQ(http::find(seen[0], "host"), "127.0.0.1:" + std::to_string(server.port()));
    EXPECT_EQ(http::find(seen[0], "accept"), "*/*");
}

TEST(HttpClient, ChunkedResponseWithACharacterSplitAcrossChunks) {
    RawServer server([](int fd) {
        http::readRequestHead(fd);
        protoio::write(fd, "HTTP/1.1 200 OK\r\ntransfer-encoding: chunked\r\n\r\n"
                           "1\r\n\xC3\r\n2\r\n\xA9!\r\n0\r\n\r\n");
    });
    http::Request r;
    r.url = server.url();
    EXPECT_EQ(http::httpRequest(r).body, "\xC3\xA9!");
}

TEST(HttpClient, BodyToTheEndWithoutLengthAndNoBodyForHead) {
    RawServer server([](int fd) {
        auto head = http::readRequestHead(fd);
        protoio::write(fd, "HTTP/1.0 200 OK\r\nx: y\r\n\r\nuntil the end");
        (void) head;
    });
    http::Request r;
    r.url = server.url();
    EXPECT_EQ(http::httpRequest(r).body, "until the end");
    r.method = "HEAD";
    EXPECT_EQ(http::httpRequest(r).body, "");
}

TEST(HttpClient, MalformedStatusLineIsNetwork) {
    RawServer server([](int fd) {
        http::readRequestHead(fd);
        protoio::write(fd, "garbage\r\n\r\n");
    });
    http::Request r;
    r.url = server.url();
    Error e = errorOf([&] { http::httpRequest(r); });
    EXPECT_EQ(e.kind, Error::Kind::Network);
    EXPECT_NE(std::string(e.what()).find("malformed status line"), std::string::npos);
}

TEST(HttpClient, HttpsVerifiesTheServerCertificate) {
    // A self-signed certificate must be refused: proof that https really
    // upgrades to TLS with verification on.
    protoio_test::TlsEchoServer server;
    http::Request r;
    r.url = "https://localhost:" + std::to_string(server.port()) + "/";
    r.timeoutMs = 5000;
    Error e = errorOf([&] { http::httpRequest(r); });
    EXPECT_EQ(e.kind, Error::Kind::Network);
    EXPECT_NE(std::string(e.what()).find("certificate"), std::string::npos) << e.what();
}

TEST(HttpClient, ConnectionRefused) {
    RawServer* gone = new RawServer([](int) {});
    const std::string url = gone->url();
    delete gone;
    http::Request r;
    r.url = url;
    EXPECT_EQ(errorOf([&] { http::httpRequest(r); }).kind, Error::Kind::ConnectionRefused);
}

TEST(HttpClient, RedirectsRelativeCrossOriginDropsHeadersSameOriginKeepsThem) {
    // B answers the authorization header it received, or "none".
    HttpServer b([](const Served& req, http::ResponseHead&) {
        return http::find(req.head.headers, "authorization").value_or("none");
    });
    const std::string bUrl = b.url("/");
    HttpServer a([&](const Served& req, http::ResponseHead& res) -> std::string {
        const std::string& t = req.head.target;
        if (t == "/dir/start") { res.status = 302; res.headers = {{"location", "next"}}; return ""; }
        if (t == "/away") { res.status = 302; res.headers = {{"location", bUrl}}; return ""; }
        if (t == "/keep") { res.status = 307; res.headers = {{"location", "/echo-auth"}}; return ""; }
        if (t == "/echo-auth") return http::find(req.head.headers, "authorization").value_or("none");
        if (t == "/see-other") { res.status = 303; res.headers = {{"location", "/method"}}; return ""; }
        if (t == "/method")
            return req.head.method + " body=" + req.body + " cl=" +
                   http::find(req.head.headers, "content-length").value_or("none");
        if (t == "/loop") { res.status = 302; res.headers = {{"location", "/loop"}}; return "looping"; }
        return t;
    });
    http::Request r1;
    r1.url = a.url("/dir/start");
    EXPECT_EQ(http::httpRequest(r1).body, "/dir/next");

    http::Request r2;
    r2.url = a.url("/away");
    r2.headers = {{"Authorization", "secret"}};
    EXPECT_EQ(http::httpRequest(r2).body, "none");

    http::Request r3;
    r3.url = a.url("/keep");
    r3.headers = {{"Authorization", "secret"}};
    EXPECT_EQ(http::httpRequest(r3).body, "secret");

    http::Request r4;
    r4.method = "POST";
    r4.url = a.url("/see-other");
    r4.body = "payload";
    EXPECT_EQ(http::httpRequest(r4).body, "GET body= cl=none");

    http::Request r5;
    r5.url = a.url("/loop");
    r5.maxRedirects = 2;
    auto last = http::httpRequest(r5);
    EXPECT_EQ(last.status, 302);
    EXPECT_EQ(last.body, "looping");
}

TEST(HttpClient, ServerSideLimitsOverTheWire) {
    HttpServer server([](const Served&, http::ResponseHead&) { return std::string("ok"); });
    auto statusOf = [&](const std::string& text) {
        const int s = protoio::net::tcpConnect("127.0.0.1", server.port(), 2000);
        protoio::setTimeout(s, 5000);
        protoio::write(s, text);
        auto head = http::readResponseHead(s);
        protoio::close(s);
        return head.status;
    };
    const std::string crlf = "\r\n";
    EXPECT_EQ(statusOf("POST / HTTP/1.1" + crlf + "content-length: -5" + crlf + crlf), 400);
    EXPECT_EQ(statusOf("GET / HTTP/1.1" + crlf + "x-long: " + std::string(9000, 'a') + crlf + crlf), 431);
    EXPECT_EQ(statusOf("POST / HTTP/1.1" + crlf + "content-length: 999999999999" + crlf + crlf), 413);
    EXPECT_EQ(statusOf("GET / HTTP/1.1" + crlf + crlf), 200);
}
