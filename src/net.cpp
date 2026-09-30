// protoIO - shared input and output for the protoCore runtimes.
// Copyright (c) 2026 Gustavo Marino <gamarino@gmail.com>. MIT licensed.
//
// TCP, TLS (client side) and UDP. Moved from protoST's
// src/primitives/io_prims.cpp.
#include "protoio/net.h"

#include "protoio/stream.h"
#include "internal.h"

#include <cerrno>
#include <chrono>
#include <cstring>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

namespace protoio {

namespace detail {

SSL_CTX* tlsContext(bool verify) {
    static std::once_flag once;
    static SSL_CTX* verifying = nullptr;
    static SSL_CTX* trusting = nullptr;
    std::call_once(once, [] {
        OPENSSL_init_ssl(0, nullptr);
        verifying = SSL_CTX_new(TLS_client_method());
        SSL_CTX_set_default_verify_paths(verifying);
        SSL_CTX_set_verify(verifying, SSL_VERIFY_PEER, nullptr);
        SSL_CTX_set_min_proto_version(verifying, TLS1_2_VERSION);
        trusting = SSL_CTX_new(TLS_client_method());
        SSL_CTX_set_verify(trusting, SSL_VERIFY_NONE, nullptr);
        SSL_CTX_set_min_proto_version(trusting, TLS1_2_VERSION);
    });
    return verify ? verifying : trusting;
}

std::string tlsErrorText() {
    const unsigned long e = ERR_get_error();
    if (!e) return "TLS handshake failed";
    char buf[256];
    ERR_error_string_n(e, buf, sizeof buf);
    return buf;
}

} // namespace detail

namespace net {

using detail::netError;

namespace {

struct AddrList {
    addrinfo* head = nullptr;
    AddrList() = default;
    AddrList(const AddrList&) = delete;
    AddrList& operator=(const AddrList&) = delete;
    ~AddrList() { if (head) ::freeaddrinfo(head); }
};

// Resolves host:port. Throws NameLookup.
void resolve(const std::string& host, int port, int socktype, bool passive, AddrList& out,
             int family = AF_UNSPEC) {
    addrinfo hints{};
    hints.ai_family = family;
    hints.ai_socktype = socktype;
    if (passive) hints.ai_flags = AI_PASSIVE;
    const std::string service = std::to_string(port);
    const int r = ::getaddrinfo(host.empty() ? nullptr : host.c_str(), service.c_str(), &hints, &out.head);
    if (r != 0) throw Error(Error::Kind::NameLookup, "cannot resolve " + host + ": " + ::gai_strerror(r));
}

Address addressOf(const sockaddr* sa) {
    char host[INET6_ADDRSTRLEN] = {0};
    Address a;
    if (sa->sa_family == AF_INET) {
        auto* in = reinterpret_cast<const sockaddr_in*>(sa);
        ::inet_ntop(AF_INET, &in->sin_addr, host, sizeof host);
        a.port = ntohs(in->sin_port);
    } else if (sa->sa_family == AF_INET6) {
        auto* in = reinterpret_cast<const sockaddr_in6*>(sa);
        ::inet_ntop(AF_INET6, &in->sin6_addr, host, sizeof host);
        a.port = ntohs(in->sin6_port);
    }
    a.host = host;
    return a;
}

void checkPort(int port) {
    if (port < 0 || port > 65535)
        throw Error(Error::Kind::InvalidArgument, "invalid port: " + std::to_string(port));
}

// What is left of a timeout (-1: no limit).
class Deadline {
public:
    explicit Deadline(int timeoutMs)
        : unlimited_(timeoutMs < 0),
          end_(std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs < 0 ? 0 : timeoutMs)) {}
    int remaining() const {
        if (unlimited_) return -1;
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(end_ - std::chrono::steady_clock::now());
        return left.count() > 0 ? static_cast<int>(left.count()) : 0;
    }

private:
    bool unlimited_;
    std::chrono::steady_clock::time_point end_;
};

void noDelay(int fd) {
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
}

Address nameOf(int fd, bool peer) {
    sockaddr_storage ss{};
    socklen_t len = sizeof ss;
    const int rc = peer ? ::getpeername(fd, reinterpret_cast<sockaddr*>(&ss), &len)
                        : ::getsockname(fd, reinterpret_cast<sockaddr*>(&ss), &len);
    if (rc != 0) netError(errno, "address of socket");
    return addressOf(reinterpret_cast<sockaddr*>(&ss));
}

} // namespace

int tcpConnect(const std::string& host, int port, int timeoutMs) {
    checkPort(port);
    AddrList addrs;
    resolve(host, port, SOCK_STREAM, false, addrs);
    int fd = -1;
    int lastErr = ECONNREFUSED;
    for (addrinfo* ai = addrs.head; ai; ai = ai->ai_next) {
        fd = ::socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC | SOCK_NONBLOCK, ai->ai_protocol);
        if (fd < 0) { lastErr = errno; continue; }
        int rc = ::connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (rc != 0 && errno == EINPROGRESS) {
            pollfd p{fd, POLLOUT, 0};
            int pr;
            while ((pr = ::poll(&p, 1, timeoutMs < 0 ? -1 : timeoutMs)) < 0 && errno == EINTR) {}
            if (pr <= 0) { lastErr = pr == 0 ? ETIMEDOUT : errno; ::close(fd); fd = -1; continue; }
            int soErr = 0;
            socklen_t len = sizeof soErr;
            ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soErr, &len);
            rc = soErr ? -1 : 0;
            if (soErr) errno = soErr;
        }
        if (rc == 0) break;
        lastErr = errno;
        ::close(fd);
        fd = -1;
    }
    if (fd < 0) netError(lastErr, "cannot connect to " + host + ":" + std::to_string(port));
    ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) & ~O_NONBLOCK);
    noDelay(fd);
    protoio::forget(fd);
    return fd;
}

int tcpListen(const std::string& host, int port, int backlog) {
    checkPort(port);
    AddrList addrs;
    resolve(host, port, SOCK_STREAM, true, addrs);
    int fd = -1, lastErr = EADDRNOTAVAIL;
    for (addrinfo* ai = addrs.head; ai; ai = ai->ai_next) {
        fd = ::socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol);
        if (fd < 0) { lastErr = errno; continue; }
        int one = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        if (::bind(fd, ai->ai_addr, ai->ai_addrlen) == 0 && ::listen(fd, backlog) == 0) {
            ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);  // see tcpAccept
            break;
        }
        lastErr = errno;
        ::close(fd);
        fd = -1;
    }
    if (fd < 0) netError(lastErr, "cannot listen on port " + std::to_string(port));
    protoio::forget(fd);
    return fd;
}

std::optional<int> tcpAccept(int fd, int timeoutMs) {
    auto st = detail::fdState(fd);  // held: a concurrent close cannot free the number under us
    // The listening socket is non-blocking (tcpListen): when another thread
    // takes the connection first, accept answers EAGAIN and this one waits
    // again for what is left of its time, instead of blocking in accept.
    Deadline deadline(timeoutMs);
    for (;;) {
        if (!detail::waitReady(fd, POLLIN, deadline.remaining())) return std::nullopt;
        if (st->closed.load()) return std::nullopt;
        const int c = ::accept4(fd, nullptr, nullptr, SOCK_CLOEXEC);
        if (c >= 0) {
            noDelay(c);
            protoio::forget(c);
            return c;
        }
        if (st->closed.load()) return std::nullopt;
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK || errno == ECONNABORTED) continue;
        netError(errno, "accept");
    }
}

Address sockName(int fd) { return nameOf(fd, false); }

Address peerName(int fd) { return nameOf(fd, true); }

void tlsConnect(int fd, const std::string& host, bool verify) {
    auto st = detail::fdState(fd);
    std::string failure;
    int err = 0;
    {
        std::lock_guard<std::mutex> rlock(st->readMutex);
        std::lock_guard<std::mutex> wlock(st->writeMutex);
        SSL* ssl = st->ssl.load() ? nullptr : SSL_new(detail::tlsContext(verify));
        if (!ssl) {
            failure = st->ssl.load() ? "the connection already uses TLS" : detail::tlsErrorText();
        } else {
            SSL_set_fd(ssl, fd);
            SSL_set_tlsext_host_name(ssl, host.c_str());
            if (verify) SSL_set1_host(ssl, host.c_str());
            ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
            st->ssl.store(ssl);
            SigpipeGuard noSigpipe;
            const int rc = detail::sslCall(*st, [](SSL* s, void*) { return SSL_connect(s); }, nullptr);
            if (rc != 1) {
                const long v = SSL_get_verify_result(ssl);
                if (rc < 0 && errno == ETIMEDOUT) err = ETIMEDOUT;
                else failure = v != X509_V_OK ? std::string("TLS certificate: ") + X509_verify_cert_error_string(v)
                                              : detail::tlsErrorText();
                st->ssl.store(nullptr);
                SSL_free(ssl);
            }
        }
    }
    if (err) netError(err, "TLS handshake with " + host);
    if (!failure.empty()) throw Error(Error::Kind::Network, failure + " (" + host + ")");
}

int udpBind(const std::string& host, int port) {
    checkPort(port);
    AddrList addrs;
    resolve(host.empty() ? "0.0.0.0" : host, port, SOCK_DGRAM, true, addrs);
    int fd = -1, lastErr = EADDRNOTAVAIL;
    for (addrinfo* ai = addrs.head; ai; ai = ai->ai_next) {
        fd = ::socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol);
        if (fd < 0) { lastErr = errno; continue; }
        int one = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        ::setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
        if (::bind(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        lastErr = errno;
        ::close(fd);
        fd = -1;
    }
    if (fd < 0) netError(lastErr, "cannot bind UDP port " + std::to_string(port));
    protoio::forget(fd);
    return fd;
}

void udpSend(int fd, const std::string& host, int port, std::string_view data) {
    checkPort(port);
    auto st = detail::fdState(fd);  // held: a concurrent close cannot free the number under us
    // Resolve to the socket's own address family: an IPv4 socket cannot send
    // to an IPv6 address.
    sockaddr_storage own{};
    socklen_t ownLen = sizeof own;
    const int family = ::getsockname(fd, reinterpret_cast<sockaddr*>(&own), &ownLen) == 0 ? own.ss_family
                                                                                          : AF_UNSPEC;
    AddrList addrs;
    resolve(host, port, SOCK_DGRAM, false, addrs, family);
    ssize_t n;
    while ((n = ::sendto(fd, data.data(), data.size(), MSG_NOSIGNAL, addrs.head->ai_addr,
                         addrs.head->ai_addrlen)) < 0 && errno == EINTR) {}
    if (n < 0) netError(errno, "send to " + host);
}

std::optional<Datagram> udpReceive(int fd, int timeoutMs) {
    auto st = detail::fdState(fd);  // held: a concurrent close cannot free the number under us
    Deadline deadline(timeoutMs);
    Datagram d;
    d.data.resize(65536);
    sockaddr_storage ss{};
    for (;;) {
        if (!detail::waitReady(fd, POLLIN, deadline.remaining()) || st->closed.load()) return std::nullopt;
        socklen_t len = sizeof ss;
        // MSG_DONTWAIT: when another thread took the datagram first, wait
        // again for what is left of the time instead of blocking here.
        const ssize_t n = ::recvfrom(fd, d.data.data(), d.data.size(), MSG_DONTWAIT,
                                     reinterpret_cast<sockaddr*>(&ss), &len);
        if (n >= 0) {
            d.data.resize(static_cast<std::size_t>(n));
            break;
        }
        if (st->closed.load()) return std::nullopt;
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
        netError(errno, "receive");
    }
    const Address from = addressOf(reinterpret_cast<sockaddr*>(&ss));
    d.host = from.host;
    d.port = from.port;
    return d;
}

} // namespace net
} // namespace protoio
