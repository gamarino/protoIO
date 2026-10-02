#include "test_support.h"

#include "protoio/file.h"
#include "protoio/net.h"
#include "protoio/stream.h"

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <cstdlib>
#include <stdexcept>

#ifdef _WIN32
#include <random>
#else
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace protoio_test {

#ifdef _WIN32
void socketPair(int fds[2]) {
    const int listener = protoio::net::tcpListen("127.0.0.1", 0);
    const int port = protoio::net::sockName(listener).port;
    fds[0] = protoio::net::tcpConnect("127.0.0.1", port, 5000);
    std::optional<int> b = protoio::net::tcpAccept(listener, 5000);
    protoio::close(listener);
    if (!b) throw std::runtime_error("socketPair: no connection");
    fds[1] = *b;
}

TempDir::TempDir() {
    std::random_device rd;
    for (int attempt = 0; attempt < 100; ++attempt) {
        const std::string candidate = protoio::file::tempDir() + "/protoio-test-" + std::to_string(rd());
        try {
            protoio::file::mkdir(candidate);
            path = candidate;
            return;
        } catch (const protoio::Error& e) {
            if (e.kind != protoio::Error::Kind::FileExists) throw;
        }
    }
    throw std::runtime_error("cannot create a temporary directory");
}
#else
void socketPair(int fds[2]) {
#if defined(SOCK_CLOEXEC)
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) != 0) throw std::runtime_error("socketpair");
#else
    // macOS: no SOCK_CLOEXEC, and SIGPIPE is turned off per socket (needs <fcntl.h>).
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) throw std::runtime_error("socketpair");
    for (int i = 0; i < 2; ++i) {
        ::fcntl(fds[i], F_SETFD, FD_CLOEXEC);
        const int one = 1;
        ::setsockopt(fds[i], SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
        // Tests write up to 200 KB before reading, on one thread; Linux's
        // AF_UNIX buffers hold that, macOS's default few KB do not.
        const int big = 1 << 20;
        ::setsockopt(fds[i], SOL_SOCKET, SO_SNDBUF, &big, sizeof big);
        ::setsockopt(fds[i], SOL_SOCKET, SO_RCVBUF, &big, sizeof big);
    }
#endif
    protoio::forget(fds[0]);
    protoio::forget(fds[1]);
}

TempDir::TempDir() {
    std::string tmpl = protoio::file::tempDir() + "/protoio-test-XXXXXX";
    if (!::mkdtemp(tmpl.data())) throw std::runtime_error("mkdtemp failed");
    path = tmpl;
}
#endif

TempDir::~TempDir() {
    try { protoio::file::remove(path, true); } catch (...) {}
}

RawServer::RawServer(std::function<void(int)> handle) : handle_(std::move(handle)) {
    listenFd_ = protoio::net::tcpListen("127.0.0.1", 0);
    port_ = protoio::net::sockName(listenFd_).port;
    thread_ = std::thread([this] {
        while (!stop_.load()) {
            std::optional<int> c = protoio::net::tcpAccept(listenFd_, 50);
            if (!c) continue;
            try { handle_(*c); } catch (const std::exception&) {}
            protoio::close(*c);
        }
    });
}

RawServer::~RawServer() {
    stop_.store(true);
    thread_.join();
    protoio::close(listenFd_);
}

std::string RawServer::url(const std::string& path) const {
    return "http://127.0.0.1:" + std::to_string(port_) + path;
}

HttpServer::HttpServer(HttpHandler handler)
    : raw_([handler = std::move(handler)](int fd) {
          protoio::setTimeout(fd, 5000);
          protoio::http::ResponseHead response;
          std::string body;
          try {
              std::optional<protoio::http::RequestHead> head = protoio::http::readRequestHead(fd);
              if (!head) return;
              Served req{*head, protoio::http::readBody(fd, head->headers)};
              body = handler(req, response);
          } catch (const protoio::http::HttpRefusal& e) {
              response = {};
              response.status = e.status;
              body = protoio::http::reasonPhrase(e.status);
          }
          protoio::http::writeResponse(fd, response, body);
      }) {}

namespace {

// A certificate for `key`, valid for an hour, named `cn` and signed by
// `issuerKey` under `issuer`'s name (itself when `issuer` is null).
X509* makeCertificate(EVP_PKEY* key, const char* cn, X509* issuer, EVP_PKEY* issuerKey, long serial,
                      const char* extensions[][2]) {
    X509* cert = X509_new();
    X509_set_version(cert, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(cert), serial);
    X509_gmtime_adj(X509_getm_notBefore(cert), -60);
    X509_gmtime_adj(X509_getm_notAfter(cert), 3600);
    X509_set_pubkey(cert, key);
    X509_NAME* name = X509_get_subject_name(cert);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>(cn), -1, -1, 0);
    X509_set_issuer_name(cert, issuer ? X509_get_subject_name(issuer) : name);
    X509V3_CTX v3;
    X509V3_set_ctx(&v3, issuer ? issuer : cert, cert, nullptr, nullptr, 0);
    for (int i = 0; extensions && extensions[i][0]; ++i) {
        X509_EXTENSION* ext = X509V3_EXT_conf(nullptr, &v3, extensions[i][0], extensions[i][1]);
        if (!ext) throw std::runtime_error(std::string("bad certificate extension ") + extensions[i][0]);
        X509_add_ext(cert, ext, -1);
        X509_EXTENSION_free(ext);
    }
    X509_sign(cert, issuerKey, EVP_sha256());
    return cert;
}

SSL_CTX* makeServerContext(bool caSigned, std::string& caPem) {
    EVP_PKEY* key = EVP_EC_gen("P-256");
    X509* cert = nullptr;
    X509* ca = nullptr;
    if (caSigned) {
        EVP_PKEY* caKey = EVP_EC_gen("P-256");
        const char* caExt[][2] = {{"basicConstraints", "critical,CA:TRUE"},
                                  {"keyUsage", "critical,keyCertSign,cRLSign"},
                                  {"subjectKeyIdentifier", "hash"},
                                  {nullptr, nullptr}};
        ca = makeCertificate(caKey, "protoIO test CA", nullptr, caKey, 1, caExt);
        const char* leafExt[][2] = {{"basicConstraints", "critical,CA:FALSE"},
                                    {"keyUsage", "critical,digitalSignature"},
                                    {"extendedKeyUsage", "serverAuth"},
                                    {"subjectAltName", "DNS:localhost,IP:127.0.0.1"},
                                    {"authorityKeyIdentifier", "keyid"},
                                    {nullptr, nullptr}};
        cert = makeCertificate(key, "localhost", ca, caKey, 2, leafExt);
        BIO* mem = BIO_new(BIO_s_mem());
        PEM_write_bio_X509(mem, ca);
        char* data = nullptr;
        const long n = BIO_get_mem_data(mem, &data);
        caPem.assign(data, static_cast<std::size_t>(n));
        BIO_free(mem);
        EVP_PKEY_free(caKey);
    } else {
        cert = makeCertificate(key, "localhost", nullptr, key, 1, nullptr);
    }
    SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
    SSL_CTX_use_certificate(ctx, cert);
    SSL_CTX_use_PrivateKey(ctx, key);
    X509_free(cert);
    if (ca) X509_free(ca);
    EVP_PKEY_free(key);
    return ctx;
}

} // namespace

TlsEchoServer::TlsEchoServer(bool caSigned) {
    SSL_CTX* ctx = makeServerContext(caSigned, caPem_);
    ctx_ = ctx;
    raw_ = new RawServer([ctx](int fd) {
        SSL* ssl = SSL_new(ctx);
#ifdef _WIN32
        SSL_set_fd(ssl, static_cast<int>(protoio::net::nativeSocket(fd)));
#else
        SSL_set_fd(ssl, fd);
#endif
        if (SSL_accept(ssl) == 1) {
            std::string line;
            char c;
            while (SSL_read(ssl, &c, 1) == 1 && c != '\n') line.push_back(c);
            const std::string reply = "echo:" + line + "\n";
            SSL_write(ssl, reply.data(), static_cast<int>(reply.size()));
            SSL_shutdown(ssl);
        }
        SSL_free(ssl);
    });
}

TlsEchoServer::~TlsEchoServer() {
    delete raw_;
    SSL_CTX_free(static_cast<SSL_CTX*>(ctx_));
}

} // namespace protoio_test
