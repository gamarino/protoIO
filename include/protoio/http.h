// protoIO - shared input and output for the protoCore runtimes.
// Copyright (c) 2026 Gustavo Marino <gamarino@gmail.com>. MIT licensed.
#pragma once

#include "protoio/error.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The HTTP/1.1 message layer: parsing with limits, validation, serialisation,
// URL and redirect helpers, and a blocking client. One request per connection
// ("connection: close"); no HTTP/2, no WebSockets, no TLS server.
//
// A server stays in each runtime (it dispatches to user code): the runtime
// owns the accept loop and, per connection, calls readRequestHead (which
// refuses malformed or oversized input with an HttpRefusal carrying 400, 414,
// 431 or 413), readBody, the user's handler, and writeResponse (which
// validates the response before anything is written).
namespace protoio::http {

struct Header {
    std::string name;
    std::string value;
};

// Headers in the order they were given or received. Parsed headers have
// lower-case names and trimmed values; duplicates are kept.
using Headers = std::vector<Header>;

// The value of the last header named `name` (compared without case), or
// std::nullopt. The last one wins, as a dictionary filled in order would.
std::optional<std::string> find(const Headers& headers, std::string_view name);

// Bounds on untrusted input.
struct Limits {
    std::size_t maxLine = 8192;              // a request, status or header line, in bytes
    std::size_t maxHeaderLines = 100;        // header lines per message
    std::size_t maxBody = 64 * 1024 * 1024;  // a request body (readRequestHead); 0: no limit
};

struct RequestHead {
    std::string method;
    std::string target;   // as sent, e.g. "/path?x=1"
    std::string version;  // e.g. "HTTP/1.1"; empty when the request line had none
    Headers headers;
};

struct ResponseHead {
    int status = 200;
    std::string reason;  // empty: writeResponse uses reasonPhrase(status)
    Headers headers;
};

// A request the server must refuse with `status` before its handler runs:
// 400 (malformed request line or Content-Length), 414 (request line too
// long), 431 (header line too long, or too many) or 413 (declared body over
// Limits::maxBody). Its kind is InvalidArgument, LineTooLong or BodyTooLarge.
struct HttpRefusal : Error {
    int status;
    HttpRefusal(int st, Kind k, const std::string& message) : Error(k, message), status(st) {}
};

// ------------------------------------------------------------------ parsing

// Header lines up to the blank line. A line over limits.maxLine bytes, or
// more than limits.maxHeaderLines lines, throws LineTooLong. A line without a
// colon is ignored. The end of the stream ends the headers.
Headers readHeaders(int fd, const Limits& limits = {});

// A request line and its headers, or std::nullopt when the connection closed
// (or sent an empty line) before a request began. Throws HttpRefusal as
// described above; the Content-Length, if any, is validated here.
std::optional<RequestHead> readRequestHead(int fd, const Limits& limits = {});

// A status line and its headers. Throws Network when the stream ends first
// or the status line is malformed, LineTooLong past the limits.
ResponseHead readResponseHead(int fd, const Limits& limits = {});

// The Content-Length, or std::nullopt when absent. A value that is not a
// decimal integer of at most 18 digits, or several differing values, throw
// Network ("invalid content-length").
std::optional<std::size_t> contentLength(const Headers& headers);

// The body the headers announce: chunked (when Transfer-Encoding names it;
// decoded as bytes, trailers skipped) or Content-Length bytes; empty when
// there is neither. `maxBytes` 0 means no limit; a larger body throws
// BodyTooLarge. A malformed chunk or a body that ends early throws Network.
std::string readBody(int fd, const Headers& headers, std::size_t maxBytes = 0);

// ------------------------------------------------------ validation / output

// Validation refuses anything that could forge extra headers or messages.
// Each throws InvalidArgument with a message that contains "invalid".
// A token (a method or header name): non-empty, no control characters,
// spaces or separators "()<>@,;:\"/[]?={}".
void validateToken(std::string_view token, std::string_view what = "token");
// A header value or reason phrase: no control characters other than a tab.
void validateValue(std::string_view value, std::string_view what = "header value");
// A request target: no spaces and no control characters.
void validateTarget(std::string_view target);
// Every name and value of a header list.
void validateHeaders(const Headers& headers);

// The reason phrase of a status code ("OK", "Not Found", ...; "Status" for
// codes it does not know).
std::string reasonPhrase(int status);

// The bytes of a request: the request line, the headers, a content-length
// when there is a body or the method carries one (POST, PUT, PATCH), and
// "connection: close". Content-Length and Connection headers in
// head.headers are replaced by the library's own. Validates first.
std::string serializeRequest(const RequestHead& head, const std::optional<std::string>& body);

// The bytes of a response: status line, headers, content-length (not for 1xx
// and 204, which carry no body), "connection: close", body. Validates the
// reason and the headers first.
std::string serializeResponse(const ResponseHead& head, std::string_view body);

// Validate, serialise and write in one call (see protoio::write).
void writeRequest(int fd, const RequestHead& head, const std::optional<std::string>& body);
void writeResponse(int fd, const ResponseHead& head, std::string_view body);

// ------------------------------------------------------------------ helpers

// %XX sequences decoded as bytes (so "%C3%A9" is one UTF-8 character);
// malformed escapes are kept as they are. '+' is left alone.
std::string percentDecode(std::string_view s);

// A query string ("a=1&b=x+y") as ordered name/value pairs: '+' becomes a
// space first, then each part is percent-decoded (so "%2B" stays a plus).
// A pair without '=' has an empty value.
std::vector<std::pair<std::string, std::string>> parseQuery(std::string_view query);

struct Url {
    std::string scheme;  // lower case, e.g. "http"
    std::string host;    // without brackets for an IPv6 literal
    int port = 0;        // the scheme's default (80, 443) when absent
    std::string target;  // path and query, at least "/"
};

// Parses an absolute URL "scheme://host[:port][/path][?query]". Throws
// InvalidArgument for anything else or an invalid port.
Url parseUrl(std::string_view url);

// "host", or "host:port" when the port is not the scheme's default (the
// Host header).
std::string authority(const Url& url);

// "scheme://host:port": two URLs with the same origin may share credentials.
std::string origin(const Url& url);

// The absolute URL a Location header names, resolved against the URL that
// answered it (RFC 3986, with dot segments removed). A redirect from https
// to http throws Network.
std::string resolveRedirect(std::string_view location, std::string_view baseUrl);

// ------------------------------------------------------------------- client

struct Request {
    std::string method = "GET";
    std::string url;                  // http:// or https://
    Headers headers;                  // names are sent in lower case
    std::optional<std::string> body;  // std::nullopt: no body
    int timeoutMs = 30000;            // connect, and each wait for the peer
    int maxRedirects = 5;
    std::string userAgent;            // empty: "protoIO/<version>"
};

struct Response {
    int status = 0;
    std::string reason;
    Headers headers;  // lower-case names
    std::string body;
};

// Performs a request: connects, upgrades to TLS for https (the certificate
// is verified), writes the request, reads the status line (a malformed one
// throws Network), the headers and the body (none for HEAD, 1xx, 204 and
// 304; to the end of the stream when neither a length nor chunking is
// announced), and follows redirects (301, 302, 303, 307, 308) up to
// maxRedirects: 303 turns into a GET without a body, a redirect to another
// origin drops the caller's headers for the rest of the chain, and https to
// http is refused. The request sends host, user-agent, accept and, with a
// body, a text/plain content-type, each replaceable by the caller's headers.
// Everything is validated before anything is sent. Blocks.
Response httpRequest(const Request& request);

} // namespace protoio::http
