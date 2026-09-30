// Helpers shared by the protoIO test suites: temporary directories, loopback
// servers run on a thread of the test, and an in-process TLS server.
#pragma once

#include "protoio/http.h"

#include <atomic>
#include <functional>
#include <string>
#include <thread>

namespace protoio_test {

// A fresh directory under the system's temporary directory, removed with
// everything in it when the object goes away.
struct TempDir {
    std::string path;
    TempDir();
    ~TempDir();
    std::string operator/(const std::string& name) const { return path + "/" + name; }
};

// Accepts connections on 127.0.0.1 (a free port) on its own thread and calls
// `handle` with each connected descriptor, which it then closes. Stops when
// destroyed.
class RawServer {
public:
    explicit RawServer(std::function<void(int fd)> handle);
    ~RawServer();
    int port() const { return port_; }
    std::string url(const std::string& path = "/") const;

private:
    std::function<void(int)> handle_;
    int listenFd_ = -1;
    int port_ = 0;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

// An HTTP server built on the library's server-side calls: readRequestHead,
// readBody and writeResponse. The handler fills the response and answers its
// body.
struct Served {
    protoio::http::RequestHead head;
    std::string body;
};
using HttpHandler = std::function<std::string(const Served& request, protoio::http::ResponseHead& response)>;

class HttpServer {
public:
    explicit HttpServer(HttpHandler handler);
    int port() const { return raw_.port(); }
    std::string url(const std::string& path = "/") const { return raw_.url(path); }

private:
    RawServer raw_;
};

// A TLS server on 127.0.0.1 with a freshly generated self-signed certificate
// for "localhost". For each connection it completes the handshake, reads one
// line and answers "echo:<line>\n".
class TlsEchoServer {
public:
    TlsEchoServer();
    ~TlsEchoServer();
    int port() const { return raw_ ? raw_->port() : 0; }

private:
    void* ctx_ = nullptr;  // SSL_CTX*
    RawServer* raw_ = nullptr;
};

} // namespace protoio_test
