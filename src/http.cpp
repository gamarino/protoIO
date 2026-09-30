// protoIO - shared input and output for the protoCore runtimes.
// Copyright (c) 2026 Gustavo Marino <gamarino@gmail.com>. MIT licensed.
//
// The HTTP/1.1 message layer and client. Ported from protoST's lib/http.st
// (message handling, limits, validation, redirect policy) and
// src/primitives/io_prims.cpp (chunked bodies, percent-decoding).
#include "protoio/http.h"

#include "protoio/net.h"
#include "protoio/stream.h"
#include "internal.h"

#include <algorithm>
#include <cctype>

#ifndef PROTOIO_VERSION
#define PROTOIO_VERSION "0.0.0"
#endif

namespace protoio::http {

namespace {

using detail::FdState;

char lowerChar(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = lowerChar(c);
    return out;
}

bool equalsNoCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (lowerChar(a[i]) != lowerChar(b[i])) return false;
    return true;
}

std::string trim(std::string_view s) {
    std::size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t')) --e;
    return std::string(s.substr(b, e - b));
}

// Space-separated words, empty ones dropped.
std::vector<std::string> words(std::string_view s) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && s[i] == ' ') ++i;
        const std::size_t start = i;
        while (i < s.size() && s[i] != ' ') ++i;
        if (i > start) out.emplace_back(s.substr(start, i - start));
    }
    return out;
}

// A value for an error message: quoted, control characters escaped.
std::string printable(std::string_view s) {
    static const char* hex = "0123456789abcdef";
    std::string out = "'";
    for (char ch : s.substr(0, 200)) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c == '\r') out += "\\r";
        else if (c == '\n') out += "\\n";
        else if (c < 32 || c == 127) { out += "\\x"; out += hex[c >> 4]; out += hex[c & 15]; }
        else out.push_back(ch);
    }
    if (s.size() > 200) out += "...";
    return out + "'";
}

[[noreturn]] void network(const std::string& msg) { throw Error(Error::Kind::Network, msg); }

[[noreturn]] void invalid(std::string_view what, std::string_view value) {
    throw Error(Error::Kind::InvalidArgument, "invalid " + std::string(what) + ": " + printable(value));
}

bool isSeparator(char c) {
    return std::string_view("()<>@,;:\\\"/[]?={}").find(c) != std::string_view::npos;
}

// Header lines up to the blank line. The caller holds readMutex.
Headers readHeadersLocked(FdState& st, const Limits& limits) {
    Headers out;
    std::string line;
    std::size_t count = 0;
    while (detail::takeLine(st, limits.maxLine, line) && !line.empty()) {
        if (++count > limits.maxHeaderLines)
            throw Error(Error::Kind::LineTooLong,
                        "more than " + std::to_string(limits.maxHeaderLines) + " header lines");
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        out.push_back({lower(trim(std::string_view(line).substr(0, colon))),
                       trim(std::string_view(line).substr(colon + 1))});
    }
    return out;
}

// A chunked body, decoded as bytes. The caller holds readMutex.
std::string readChunkedLocked(FdState& st, std::size_t max) {
    std::string body, line;
    for (;;) {
        if (!detail::takeLine(st, 8192, line)) network("chunked body ended early");
        const std::string hex = line.substr(0, line.find(';'));
        std::size_t size = 0, digits = 0;
        for (char c : hex) {
            if (c == ' ' || c == '\t') continue;
            const int d = (c >= '0' && c <= '9') ? c - '0'
                        : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                        : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
            if (d < 0 || ++digits > 15) network("invalid chunk size: " + printable(hex));
            size = size * 16 + static_cast<std::size_t>(d);
        }
        if (digits == 0) network("invalid chunk size: " + printable(hex));
        if (size == 0) break;
        if (max && body.size() + size > max)
            throw Error(Error::Kind::BodyTooLarge, "a body larger than " + std::to_string(max) + " bytes");
        const std::string chunk = detail::takeBytes(st, size);
        if (chunk.size() < size) network("chunked body ended early");
        body += chunk;
        detail::takeLine(st, 8192, line);  // the CRLF after the chunk
    }
    while (detail::takeLine(st, 8192, line) && !line.empty()) {}  // trailers
    return body;
}

bool bodyMethod(std::string_view method) {
    return method == "POST" || method == "PUT" || method == "PATCH";
}

// Headers the library writes itself; the caller's copies are dropped so a
// message never carries two conflicting framings.
bool framingHeader(std::string_view name) {
    return equalsNoCase(name, "content-length") || equalsNoCase(name, "connection") ||
           equalsNoCase(name, "transfer-encoding");
}

void appendHeaders(std::string& out, const Headers& headers) {
    for (const Header& h : headers) {
        if (framingHeader(h.name)) continue;
        out += h.name;
        out += ": ";
        out += h.value;
        out += "\r\n";
    }
}

int defaultPort(std::string_view scheme) { return scheme == "https" ? 443 : 80; }

std::string hostText(const std::string& host) {
    return host.find(':') != std::string::npos ? "[" + host + "]" : host;
}

// "/a/b/../c" -> "/a/c"; a query is kept as it is.
std::string removeDotSegments(std::string_view full) {
    const std::size_t q = full.find('?');
    const std::string_view path = full.substr(0, q);
    const std::string_view query = q == std::string_view::npos ? std::string_view() : full.substr(q);
    std::vector<std::string_view> out;
    std::size_t i = 0;
    while (i <= path.size()) {
        const std::size_t slash = std::min(path.find('/', i), path.size());
        const std::string_view seg = path.substr(i, slash - i);
        if (seg == "..") {
            if (!out.empty()) out.pop_back();
        } else if (!seg.empty() && seg != ".") {
            out.push_back(seg);
        }
        i = slash + 1;
    }
    std::string result;
    for (std::string_view seg : out) { result += '/'; result += seg; }
    const auto endsWith = [&](std::string_view suffix) {
        return path.size() >= suffix.size() && path.substr(path.size() - suffix.size()) == suffix;
    };
    if (out.empty() || endsWith("/") || endsWith("/.") || endsWith("/..") || path == "." || path == "..")
        result += '/';
    return result + std::string(query);
}

bool hasScheme(std::string_view s) {
    const std::size_t colon = s.find("://");
    if (colon == std::string_view::npos || colon == 0 || !std::isalpha(static_cast<unsigned char>(s[0])))
        return false;
    for (std::size_t i = 0; i < colon; ++i) {
        const char c = s[i];
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '+' && c != '-' && c != '.') return false;
    }
    return true;
}

void setHeader(Headers& headers, const std::string& name, const std::string& value) {
    for (Header& h : headers)
        if (h.name == name) { h.value = value; return; }
    headers.push_back({name, value});
}

struct Closer {
    int fd;
    ~Closer() { protoio::close(fd); }
};

} // namespace

// ------------------------------------------------------------------ parsing

std::optional<std::string> find(const Headers& headers, std::string_view name) {
    for (auto it = headers.rbegin(); it != headers.rend(); ++it)
        if (equalsNoCase(it->name, name)) return it->value;
    return std::nullopt;
}

Headers readHeaders(int fd, const Limits& limits) {
    auto st = detail::fdState(fd);
    std::lock_guard<std::mutex> lock(st->readMutex);
    return readHeadersLocked(*st, limits);
}

std::optional<RequestHead> readRequestHead(int fd, const Limits& limits) {
    auto st = detail::fdState(fd);
    std::lock_guard<std::mutex> lock(st->readMutex);
    std::string line;
    try {
        if (!detail::takeLine(*st, limits.maxLine, line)) return std::nullopt;
    } catch (const Error& e) {
        if (e.kind != Error::Kind::LineTooLong) throw;
        throw HttpRefusal(414, Error::Kind::LineTooLong, "request line too long");
    }
    if (line.empty()) return std::nullopt;
    std::vector<std::string> w = words(line);
    if (w.size() < 2 || w.size() > 3)
        throw HttpRefusal(400, Error::Kind::InvalidArgument, "malformed request line: " + printable(line));
    RequestHead head;
    try {
        validateToken(w[0], "method");
        validateTarget(w[1]);
    } catch (const Error& e) {
        throw HttpRefusal(400, Error::Kind::InvalidArgument, e.what());
    }
    head.method = w[0];
    head.target = w[1];
    if (w.size() == 3) head.version = w[2];
    try {
        head.headers = readHeadersLocked(*st, limits);
    } catch (const Error& e) {
        if (e.kind != Error::Kind::LineTooLong) throw;
        throw HttpRefusal(431, Error::Kind::LineTooLong, e.what());
    }
    std::optional<std::size_t> length;
    try {
        length = contentLength(head.headers);
    } catch (const Error& e) {
        throw HttpRefusal(400, Error::Kind::InvalidArgument, e.what());
    }
    if (length && limits.maxBody && *length > limits.maxBody)
        throw HttpRefusal(413, Error::Kind::BodyTooLarge,
                          "a body of " + std::to_string(*length) + " bytes (limit " +
                              std::to_string(limits.maxBody) + ")");
    return head;
}

ResponseHead readResponseHead(int fd, const Limits& limits) {
    auto st = detail::fdState(fd);
    std::lock_guard<std::mutex> lock(st->readMutex);
    std::string line;
    if (!detail::takeLine(*st, limits.maxLine, line)) network("no response");
    const std::vector<std::string> w = words(line);
    bool ok = w.size() >= 2 && w[0].rfind("HTTP/", 0) == 0 && w[1].size() == 3 &&
              std::all_of(w[1].begin(), w[1].end(), [](char c) { return c >= '0' && c <= '9'; });
    if (!ok) network("malformed status line: " + printable(line));
    ResponseHead head;
    head.status = std::stoi(w[1]);
    const std::size_t codeAt = line.find(w[1], w[0].size());
    head.reason = trim(std::string_view(line).substr(codeAt + w[1].size()));
    head.headers = readHeadersLocked(*st, limits);
    return head;
}

std::optional<std::size_t> contentLength(const Headers& headers) {
    std::optional<std::size_t> result;
    for (const Header& h : headers) {
        if (!equalsNoCase(h.name, "content-length")) continue;
        const std::string& v = h.value;
        if (v.empty() || v.size() > 18 || !std::all_of(v.begin(), v.end(), [](char c) { return c >= '0' && c <= '9'; }))
            network("invalid content-length: " + printable(v));
        const std::size_t n = std::stoull(v);
        if (result && *result != n) network("invalid content-length: several differing values");
        result = n;
    }
    return result;
}

std::string readBody(int fd, const Headers& headers, std::size_t maxBytes) {
    auto st = detail::fdState(fd);
    std::lock_guard<std::mutex> lock(st->readMutex);
    const std::optional<std::string> te = find(headers, "transfer-encoding");
    if (te && lower(*te).find("chunked") != std::string::npos) return readChunkedLocked(*st, maxBytes);
    const std::optional<std::size_t> length = contentLength(headers);
    if (!length || *length == 0) return std::string();
    if (maxBytes && *length > maxBytes)
        throw Error(Error::Kind::BodyTooLarge, "a body of " + std::to_string(*length) + " bytes (limit " +
                                                   std::to_string(maxBytes) + ")");
    std::string body = detail::takeBytes(*st, *length);
    if (body.size() < *length)
        network("body ended early: " + std::to_string(body.size()) + " of " + std::to_string(*length) + " bytes");
    return body;
}

// ------------------------------------------------------ validation / output

void validateToken(std::string_view token, std::string_view what) {
    if (token.empty()) throw Error(Error::Kind::InvalidArgument, "invalid " + std::string(what) + ": empty");
    for (char ch : token) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c <= 32 || c >= 127 || isSeparator(ch)) invalid(what, token);
    }
}

void validateValue(std::string_view value, std::string_view what) {
    for (char ch : value) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if ((c < 32 && c != '\t') || c == 127) invalid(what, value);
    }
}

void validateTarget(std::string_view target) {
    if (target.empty()) throw Error(Error::Kind::InvalidArgument, "invalid URL: empty target");
    for (char ch : target) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c <= 32 || c == 127) invalid("URL", target);
    }
}

void validateHeaders(const Headers& headers) {
    for (const Header& h : headers) {
        validateToken(h.name, "header name");
        validateValue(h.value, "header value");
    }
}

std::string reasonPhrase(int status) {
    switch (status) {
        case 100: return "Continue";
        case 101: return "Switching Protocols";
        case 200: return "OK";
        case 201: return "Created";
        case 202: return "Accepted";
        case 204: return "No Content";
        case 206: return "Partial Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 303: return "See Other";
        case 304: return "Not Modified";
        case 307: return "Temporary Redirect";
        case 308: return "Permanent Redirect";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 409: return "Conflict";
        case 410: return "Gone";
        case 411: return "Length Required";
        case 413: return "Content Too Large";
        case 414: return "URI Too Long";
        case 415: return "Unsupported Media Type";
        case 422: return "Unprocessable Content";
        case 429: return "Too Many Requests";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout";
        default: return "Status";
    }
}

std::string serializeRequest(const RequestHead& head, const std::optional<std::string>& body) {
    validateToken(head.method, "method");
    validateTarget(head.target);
    validateHeaders(head.headers);
    // The request line always names HTTP/1.1, the version this layer speaks.
    std::string out = head.method + " " + head.target + " HTTP/1.1\r\n";
    appendHeaders(out, head.headers);
    // A Content-Length only when there is a body, or the method carries one.
    if (body || bodyMethod(head.method))
        out += "content-length: " + std::to_string(body ? body->size() : 0) + "\r\n";
    out += "connection: close\r\n\r\n";
    if (body) out += *body;
    return out;
}

std::string serializeResponse(const ResponseHead& head, std::string_view body) {
    if (head.status < 100 || head.status > 999)
        throw Error(Error::Kind::InvalidArgument, "invalid status: " + std::to_string(head.status));
    const std::string reason = head.reason.empty() ? reasonPhrase(head.status) : head.reason;
    validateValue(reason, "reason phrase");
    validateHeaders(head.headers);
    // 1xx, 204 and 304 carry no body (RFC 9110 section 6.4.1).
    const bool noBody = head.status < 200 || head.status == 204 || head.status == 304;
    std::string out = "HTTP/1.1 " + std::to_string(head.status) + " " + reason + "\r\n";
    appendHeaders(out, head.headers);
    if (!noBody) out += "content-length: " + std::to_string(body.size()) + "\r\n";
    out += "connection: close\r\n\r\n";
    if (!noBody) out += body;
    return out;
}

void writeRequest(int fd, const RequestHead& head, const std::optional<std::string>& body) {
    protoio::write(fd, serializeRequest(head, body));
}

void writeResponse(int fd, const ResponseHead& head, std::string_view body) {
    protoio::write(fd, serializeResponse(head, body));
}

// ------------------------------------------------------------------ helpers

std::string percentDecode(std::string_view in) {
    auto hex = [](char c) {
        return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
             : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
    };
    std::string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '%' && i + 2 < in.size() && hex(in[i + 1]) >= 0 && hex(in[i + 2]) >= 0) {
            out.push_back(static_cast<char>(hex(in[i + 1]) * 16 + hex(in[i + 2])));
            i += 2;
        } else {
            out.push_back(in[i]);
        }
    }
    return out;
}

std::vector<std::pair<std::string, std::string>> parseQuery(std::string_view query) {
    std::vector<std::pair<std::string, std::string>> out;
    std::size_t i = 0;
    while (i <= query.size()) {
        const std::size_t amp = std::min(query.find('&', i), query.size());
        std::string pair(query.substr(i, amp - i));
        i = amp + 1;
        if (pair.empty()) continue;
        // In a query string '+' is a space, so it is replaced before decoding
        // (an encoded plus, %2B, stays a plus).
        std::replace(pair.begin(), pair.end(), '+', ' ');
        const std::size_t eq = pair.find('=');
        if (eq == std::string::npos) out.emplace_back(percentDecode(pair), std::string());
        else out.emplace_back(percentDecode(std::string_view(pair).substr(0, eq)),
                              percentDecode(std::string_view(pair).substr(eq + 1)));
    }
    return out;
}

Url parseUrl(std::string_view url) {
    if (!hasScheme(url)) throw Error(Error::Kind::InvalidArgument, "not an absolute URL: " + printable(url));
    const std::size_t sep = url.find("://");
    Url u;
    u.scheme = lower(url.substr(0, sep));
    std::string_view rest = url.substr(sep + 3);
    const std::size_t hash = rest.find('#');
    if (hash != std::string_view::npos) rest = rest.substr(0, hash);  // a fragment is never sent
    const std::size_t end = std::min(rest.find_first_of("/?"), rest.size());
    const std::string_view hostPort = rest.substr(0, end);
    std::string target(rest.substr(end));
    if (target.empty() || target[0] == '?') target = "/" + target;
    u.target = target;

    std::string_view portText;
    bool hasPort = false;
    if (!hostPort.empty() && hostPort[0] == '[') {
        const std::size_t close = hostPort.find(']');
        if (close == std::string_view::npos) throw Error(Error::Kind::InvalidArgument, "invalid host in URL: " + printable(url));
        u.host = std::string(hostPort.substr(1, close - 1));
        const std::string_view after = hostPort.substr(close + 1);
        if (!after.empty()) {
            if (after[0] != ':') throw Error(Error::Kind::InvalidArgument, "invalid host in URL: " + printable(url));
            portText = after.substr(1);
            hasPort = true;
        }
    } else {
        const std::size_t colon = hostPort.find(':');
        u.host = std::string(hostPort.substr(0, colon));
        if (colon != std::string_view::npos) { portText = hostPort.substr(colon + 1); hasPort = true; }
    }
    if (u.host.empty()) throw Error(Error::Kind::InvalidArgument, "no host in URL: " + printable(url));
    if (hasPort) {
        if (portText.empty() || portText.size() > 5 ||
            !std::all_of(portText.begin(), portText.end(), [](char c) { return c >= '0' && c <= '9'; }) ||
            std::stoi(std::string(portText)) > 65535)
            throw Error(Error::Kind::InvalidArgument, "invalid port in URL: " + printable(url));
        u.port = std::stoi(std::string(portText));
    } else {
        u.port = defaultPort(u.scheme);
    }
    return u;
}

std::string authority(const Url& url) {
    if (url.port == defaultPort(url.scheme) && (url.scheme == "http" || url.scheme == "https"))
        return hostText(url.host);
    return hostText(url.host) + ":" + std::to_string(url.port);
}

std::string origin(const Url& url) {
    return url.scheme + "://" + hostText(lower(url.host)) + ":" + std::to_string(url.port);
}

std::string resolveRedirect(std::string_view locationIn, std::string_view baseUrl) {
    const Url base = parseUrl(baseUrl);
    std::string_view location = locationIn.substr(0, locationIn.find('#'));
    std::string target;
    if (hasScheme(location)) {
        target = std::string(location);
    } else if (location.rfind("//", 0) == 0) {
        target = base.scheme + ":" + std::string(location);
    } else {
        std::string basePath = base.target.substr(0, base.target.find('?'));
        std::string merged;
        if (location.rfind("/", 0) == 0) merged = std::string(location);
        else if (location.empty()) merged = base.target;
        else if (location.rfind("?", 0) == 0) merged = basePath + std::string(location);
        else merged = basePath.substr(0, basePath.rfind('/') + 1) + std::string(location);
        target = base.scheme + "://" + authority(base) + removeDotSegments(merged);
    }
    if (base.scheme == "https" && parseUrl(target).scheme == "http")
        network("refusing a redirect from https to http: " + printable(target));
    return target;
}

// ------------------------------------------------------------------- client

Response httpRequest(const Request& request) {
    std::string method = request.method;
    std::string url = request.url;
    std::optional<std::string> body = request.body;
    Headers callerHeaders = request.headers;
    const std::string userAgent = request.userAgent.empty() ? std::string("protoIO/") + PROTOIO_VERSION
                                                            : request.userAgent;
    int redirectsLeft = request.maxRedirects;
    for (;;) {
        const Url u = parseUrl(url);
        if (u.scheme != "http" && u.scheme != "https")
            throw Error(Error::Kind::InvalidArgument, "invalid URL scheme (http or https expected): " + printable(url));
        RequestHead head;
        head.method = method;
        head.target = u.target;
        head.version = "HTTP/1.1";
        setHeader(head.headers, "host", authority(u));
        setHeader(head.headers, "user-agent", userAgent);
        setHeader(head.headers, "accept", "*/*");
        if (body) setHeader(head.headers, "content-type", "text/plain; charset=utf-8");
        for (const Header& h : callerHeaders) setHeader(head.headers, lower(h.name), h.value);
        // Validated before anything is sent: a line break could forge extra
        // headers or requests.
        const std::string wire = serializeRequest(head, body);

        Response response;
        {
            const int fd = net::tcpConnect(u.host, u.port, request.timeoutMs);
            Closer closer{fd};
            protoio::setTimeout(fd, request.timeoutMs);
            if (u.scheme == "https") net::tlsConnect(fd, u.host, true);
            protoio::write(fd, wire);
            ResponseHead rh;
            try {
                rh = readResponseHead(fd);
            } catch (const Error& e) {
                if (e.kind != Error::Kind::Network) throw;
                network(std::string(e.what()) + " from " + url);
            }
            response.status = rh.status;
            response.reason = rh.reason;
            response.headers = std::move(rh.headers);
            const int s = response.status;
            if (method == "HEAD" || s == 204 || s == 304 || s < 200) {
                // No body.
            } else if (find(response.headers, "content-length") ||
                       lower(find(response.headers, "transfer-encoding").value_or("")).find("chunked") !=
                           std::string::npos) {
                response.body = readBody(fd, response.headers, 0);
            } else {
                response.body = protoio::readAll(fd);
            }
        }

        const int s = response.status;
        const std::optional<std::string> location = find(response.headers, "location");
        const bool redirect = s == 301 || s == 302 || s == 303 || s == 307 || s == 308;
        if (!redirect || !location || redirectsLeft <= 0) return response;
        const std::string next = resolveRedirect(*location, url);
        // Another origin does not get the caller's headers (Authorization,
        // cookies), for the rest of the chain.
        if (origin(parseUrl(next)) != origin(u)) callerHeaders.clear();
        if (s == 303) {
            method = "GET";
            body.reset();
        }
        url = next;
        --redirectsLeft;
    }
}

} // namespace protoio::http
