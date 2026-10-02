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
#include <thread>

#ifdef _WIN32
#include <mstcpip.h>
#include <mswsock.h>
#include <wincrypt.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

namespace protoio {

namespace detail {

namespace {

#ifdef _WIN32
// OpenSSL's default certificate locations (SSL_CTX_set_default_verify_paths)
// are directories of its own installation, which on Windows hold nothing:
// the trusted roots live in the system certificate stores. Every certificate
// of the current user's and the machine's ROOT (trusted roots) and CA
// (intermediate authorities) stores is copied into the context's store once,
// when the context is created.
//
// This copy, and not OpenSSL 3.2's "org.openssl.winstore:" store, because it
// works with any OpenSSL 3 (consumers link the one they have, e.g.
// PostgreSQL's 3.0) and does not depend on how that OpenSSL was configured.
// The limit is the same for both: a root that Windows has not downloaded yet
// (Windows fetches some roots from Windows Update the first time CryptoAPI
// needs them) is not there; see README.md, "Windows".
void addWindowsStores(X509_STORE* store) {
    const DWORD locations[] = {CERT_SYSTEM_STORE_CURRENT_USER, CERT_SYSTEM_STORE_LOCAL_MACHINE};
    for (DWORD location : locations) {
        for (const wchar_t* name : {L"ROOT", L"CA"}) {
            HCERTSTORE cs = ::CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0,
                                            location | CERT_STORE_OPEN_EXISTING_FLAG | CERT_STORE_READONLY_FLAG,
                                            name);
            if (!cs) continue;
            const CERT_CONTEXT* c = nullptr;
            while ((c = ::CertEnumCertificatesInStore(cs, c)) != nullptr) {
                if (!(c->dwCertEncodingType & X509_ASN_ENCODING)) continue;
                const unsigned char* der = c->pbCertEncoded;
                X509* x = d2i_X509(nullptr, &der, static_cast<long>(c->cbCertEncoded));
                if (!x) continue;
                // A certificate in several stores is added once; the
                // duplicate is not an error.
                X509_STORE_add_cert(store, x);
                X509_free(x);
            }
            ::CertCloseStore(cs, 0);
        }
    }
    ERR_clear_error();
}
#endif

} // namespace

SSL_CTX* tlsContext(bool verify) {
    static std::once_flag once;
    static SSL_CTX* verifying = nullptr;
    static SSL_CTX* trusting = nullptr;
    std::call_once(once, [] {
        OPENSSL_init_ssl(0, nullptr);
        verifying = SSL_CTX_new(TLS_client_method());
        // Also on Windows: the SSL_CERT_FILE and SSL_CERT_DIR variables
        // still name extra trusted certificates there.
        SSL_CTX_set_default_verify_paths(verifying);
#ifdef _WIN32
        addWindowsStores(SSL_CTX_get_cert_store(verifying));
#endif
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

void trustCertificates(const std::string& pem) {
    X509_STORE* store = SSL_CTX_get_cert_store(detail::tlsContext(true));
    BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    if (!bio) throw Error(Error::Kind::InvalidArgument, "cannot read certificates: " + detail::tlsErrorText());
    int added = 0;
    while (X509* cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr)) {
        // A certificate the store already holds is not an error.
        X509_STORE_add_cert(store, cert);
        X509_free(cert);
        ++added;
    }
    BIO_free(bio);
    ERR_clear_error();  // the end of the input is reported as an error
    if (added == 0) throw Error(Error::Kind::InvalidArgument, "no PEM certificate to trust");
}

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
#ifdef _WIN32
    detail::win::initWinsock();
#endif
    addrinfo hints{};
    hints.ai_family = family;
    hints.ai_socktype = socktype;
    if (passive) hints.ai_flags = AI_PASSIVE;
    const std::string service = std::to_string(port);
    const int r = ::getaddrinfo(host.empty() ? nullptr : host.c_str(), service.c_str(), &hints, &out.head);
    if (r != 0) throw Error(Error::Kind::NameLookup, "cannot resolve " + host + ": " + ::gai_strerror(r));
#if defined(_WIN32) || defined(__APPLE__)
    // The wildcard address: glibc answers 0.0.0.0 before ::, Windows and macOS
    // the other way round. A Windows IPv6 socket is IPv6-only by default, so
    // binding the first answer left 127.0.0.1 refused; a macOS one takes IPv4
    // peers as ::ffff:a.b.c.d. Put the IPv4 answers first, as on Linux. (Every
    // node stays in the list, so freeaddrinfo frees them all.)
    if (passive && host.empty() && out.head) {
        addrinfo *v4 = nullptr, **v4Tail = &v4, *rest = nullptr, **restTail = &rest;
        for (addrinfo* ai = out.head; ai;) {
            addrinfo* next = ai->ai_next;
            ai->ai_next = nullptr;
            if (ai->ai_family == AF_INET) { *v4Tail = ai; v4Tail = &ai->ai_next; }
            else { *restTail = ai; restTail = &ai->ai_next; }
            ai = next;
        }
        *v4Tail = rest;
        out.head = v4 ? v4 : rest;
    }
#endif
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

#ifdef _WIN32
namespace win = detail::win;

// The socket behind a descriptor; Network (ENOTSOCK) when it is none.
SOCKET native(int fd) {
    const SOCKET s = win::socketOf(fd);
    if (s == INVALID_SOCKET) netError(ENOTSOCK, "descriptor " + std::to_string(fd));
    return s;
}

int lastErrno() { return win::errnoOfWsa(::WSAGetLastError()); }

// A socket as socket(2) with SOCK_CLOEXEC makes one: not inheritable.
SOCKET newSocket(const addrinfo* ai) {
    return ::WSASocketW(ai->ai_family, ai->ai_socktype, ai->ai_protocol, nullptr, 0,
                        WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
}

void noDelay(SOCKET s) {
    const BOOL one = TRUE;
    ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof one);
}

bool isLoopback(const sockaddr* sa) {
    if (sa->sa_family == AF_INET)
        return (ntohl(reinterpret_cast<const sockaddr_in*>(sa)->sin_addr.s_addr) >> 24) == 127;
    if (sa->sa_family == AF_INET6) return IN6_IS_ADDR_LOOPBACK(&reinterpret_cast<const sockaddr_in6*>(sa)->sin6_addr);
    return false;
}

void setFlag(SOCKET s, int option) {
    const BOOL one = TRUE;
    ::setsockopt(s, SOL_SOCKET, option, reinterpret_cast<const char*>(&one), sizeof one);
}

Address nameOf(int fd, bool peer) {
    const SOCKET s = native(fd);
    sockaddr_storage ss{};
    int len = sizeof ss;
    const int rc = peer ? ::getpeername(s, reinterpret_cast<sockaddr*>(&ss), &len)
                        : ::getsockname(s, reinterpret_cast<sockaddr*>(&ss), &len);
    if (rc != 0) netError(lastErrno(), "address of socket");
    return addressOf(reinterpret_cast<sockaddr*>(&ss));
}
#else
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
#endif

} // namespace

#ifdef _WIN32
// ------------------------------------------------------------------ Windows
//
// The same contracts as the POSIX functions below, on Winsock: sockets are
// registered descriptors (platform_win.h), EINPROGRESS is WSAEWOULDBLOCK,
// accept4 and MSG_DONTWAIT do not exist (the listening and UDP sockets are
// non-blocking instead), and closing a socket does not wake a waiting thread
// (waitReady also watches the thread's Waker, which close signals).

namespace {

// One connect attempt to `ai` on a fresh socket: the connected socket, or
// INVALID_SOCKET with `err` set to the Winsock error.
SOCKET connectOnce(const addrinfo* ai, int timeoutMs, int& err) {
    SOCKET s = newSocket(ai);
    if (s == INVALID_SOCKET) {
        err = ::WSAGetLastError();
        return s;
    }
    win::setNonBlocking(s, true);
    if (isLoopback(ai->ai_addr)) {
        // Windows answers a refused connection (a RST) by sending the SYN
        // again, twice, so it fails only after about two seconds, where
        // POSIX fails at once. Those retries are switched off on the loopback
        // interface, where nothing is lost in transit; tcpConnect retries a
        // refused loopback connect itself, for a much shorter while.
        TCP_INITIAL_RTO_PARAMETERS rto{TCP_INITIAL_RTO_UNSPECIFIED_RTT, TCP_INITIAL_RTO_NO_SYN_RETRANSMISSIONS};
        DWORD got = 0;
        ::WSAIoctl(s, SIO_TCP_INITIAL_RTO, &rto, sizeof rto, nullptr, 0, &got, nullptr, nullptr);
    }
    err = 0;
    if (::connect(s, ai->ai_addr, static_cast<int>(ai->ai_addrlen)) != 0) {
        err = ::WSAGetLastError();
        if (err == WSAEWOULDBLOCK) {
            // select, not WSAPoll: it reports a failed connect reliably.
            fd_set w, x;
            FD_ZERO(&w);
            FD_ZERO(&x);
            FD_SET(s, &w);
            FD_SET(s, &x);
            timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
            const int pr = ::select(0, nullptr, &w, &x, timeoutMs < 0 ? nullptr : &tv);
            if (pr == 0) {
                err = WSAETIMEDOUT;
            } else if (pr < 0) {
                err = ::WSAGetLastError();
            } else {
                int soErr = 0;
                int len = sizeof soErr;
                ::getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soErr), &len);
                err = soErr;
            }
        }
    }
    if (err == 0) return s;
    ::closesocket(s);
    return INVALID_SOCKET;
}

} // namespace

int tcpConnect(const std::string& host, int port, int timeoutMs) {
    checkPort(port);
    AddrList addrs;
    resolve(host, port, SOCK_STREAM, false, addrs);
    SOCKET s = INVALID_SOCKET;
    int lastErr = ECONNREFUSED;
    for (addrinfo* ai = addrs.head; ai && s == INVALID_SOCKET; ai = ai->ai_next) {
        // Windows also refuses a connect, with a RST, while the listener's
        // backlog is full, and the client's own SYN retries are off on
        // loopback (connectOnce). So a refused loopback connect is tried
        // again after 10, 20, 40, 80 and 160 ms (about 0.3 s in all, within
        // the timeout): a listener that drains its backlog meanwhile gets the
        // connection, and a port nobody listens on is still refused quickly.
        const bool loopback = isLoopback(ai->ai_addr);
        Deadline deadline(timeoutMs);
        for (int delayMs = 10;; delayMs *= 2) {
            int err = 0;
            s = connectOnce(ai, deadline.remaining(), err);
            if (s != INVALID_SOCKET) break;
            lastErr = win::errnoOfWsa(err);
            const int left = deadline.remaining();
            if (err != WSAECONNREFUSED || !loopback || delayMs > 160 || (left >= 0 && left <= delayMs)) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
        }
    }
    if (s == INVALID_SOCKET) netError(lastErr, "cannot connect to " + host + ":" + std::to_string(port));
    win::setNonBlocking(s, false);
    noDelay(s);
    const int fd = win::registerSocket(s);
    protoio::forget(fd);
    return fd;
}

int tcpListen(const std::string& host, int port, int backlog) {
    checkPort(port);
    AddrList addrs;
    resolve(host, port, SOCK_STREAM, true, addrs);
    SOCKET s = INVALID_SOCKET;
    int lastErr = EADDRNOTAVAIL;
    for (addrinfo* ai = addrs.head; ai; ai = ai->ai_next) {
        s = newSocket(ai);
        if (s == INVALID_SOCKET) { lastErr = lastErrno(); continue; }
        // No SO_REUSEADDR: on Windows it lets a second socket bind a port in
        // use, and without it a port whose old connections linger in
        // TIME_WAIT can already be listened on again. SO_EXCLUSIVEADDRUSE:
        // no other socket may bind the port while this one has it, not even
        // one that sets SO_REUSEADDR (which would take the port over).
        setFlag(s, SO_EXCLUSIVEADDRUSE);
        if (::bind(s, ai->ai_addr, static_cast<int>(ai->ai_addrlen)) == 0 && ::listen(s, backlog) == 0) {
            win::setNonBlocking(s, true);  // see tcpAccept
            break;
        }
        lastErr = lastErrno();
        ::closesocket(s);
        s = INVALID_SOCKET;
    }
    if (s == INVALID_SOCKET) netError(lastErr, "cannot listen on port " + std::to_string(port));
    const int fd = win::registerSocket(s);
    protoio::forget(fd);
    return fd;
}

std::optional<int> tcpAccept(int fd, int timeoutMs) {
    auto st = detail::fdState(fd);  // held: a concurrent close cannot free the socket under us
    const SOCKET ls = native(fd);
    Deadline deadline(timeoutMs);
    for (;;) {
        if (!detail::waitReady(*st, POLLIN, deadline.remaining())) return std::nullopt;
        if (st->closed.load()) return std::nullopt;
        const SOCKET c = ::accept(ls, nullptr, nullptr);
        if (c != INVALID_SOCKET) {
            win::setNonBlocking(c, false);  // an accepted socket inherits the listener's mode
            noDelay(c);
            const int cfd = win::registerSocket(c);
            protoio::forget(cfd);
            return cfd;
        }
        if (st->closed.load()) return std::nullopt;
        const int e = ::WSAGetLastError();
        if (e == WSAEWOULDBLOCK || e == WSAEINTR || e == WSAECONNRESET) continue;
        netError(win::errnoOfWsa(e), "accept");
    }
}

Address sockName(int fd) { return nameOf(fd, false); }

Address peerName(int fd) { return nameOf(fd, true); }

std::uintptr_t nativeSocket(int fd) { return static_cast<std::uintptr_t>(native(fd)); }

void tlsConnect(int fd, const std::string& host, bool verify) {
    auto st = detail::fdState(fd);
    const SOCKET s = native(fd);
    std::string failure;
    int err = 0;
    {
        std::lock_guard<std::mutex> rlock(st->readMutex);
        std::lock_guard<std::mutex> wlock(st->writeMutex);
        SSL* ssl = st->ssl.load() ? nullptr : SSL_new(detail::tlsContext(verify));
        if (!ssl) {
            failure = st->ssl.load() ? "the connection already uses TLS" : detail::tlsErrorText();
        } else {
            // OpenSSL takes a Windows socket as an int (socket handles fit in 32 bits).
            SSL_set_fd(ssl, static_cast<int>(s));
            SSL_set_tlsext_host_name(ssl, host.c_str());
            if (verify) SSL_set1_host(ssl, host.c_str());
            win::setNonBlocking(s, true);
            st->ssl.store(ssl);
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
    SOCKET s = INVALID_SOCKET;
    int lastErr = EADDRNOTAVAIL;
    for (addrinfo* ai = addrs.head; ai; ai = ai->ai_next) {
        s = newSocket(ai);
        if (s == INVALID_SOCKET) { lastErr = lastErrno(); continue; }
        setFlag(s, SO_REUSEADDR);
        setFlag(s, SO_BROADCAST);
        if (::bind(s, ai->ai_addr, static_cast<int>(ai->ai_addrlen)) == 0) break;
        lastErr = lastErrno();
        ::closesocket(s);
        s = INVALID_SOCKET;
    }
    if (s == INVALID_SOCKET) netError(lastErr, "cannot bind UDP port " + std::to_string(port));
    // Windows answers a datagram that drew an ICMP "port unreachable" with
    // WSAECONNRESET on the next receive; POSIX ignores it, and so do we.
    BOOL report = FALSE;
    DWORD got = 0;
    ::WSAIoctl(s, SIO_UDP_CONNRESET, &report, sizeof report, nullptr, 0, &got, nullptr, nullptr);
    win::setNonBlocking(s, true);  // see udpReceive
    const int fd = win::registerSocket(s);
    protoio::forget(fd);
    return fd;
}

void udpSend(int fd, const std::string& host, int port, std::string_view data) {
    checkPort(port);
    auto st = detail::fdState(fd);  // held: a concurrent close cannot free the socket under us
    const SOCKET s = native(fd);
    // Resolve to the socket's own address family: an IPv4 socket cannot send
    // to an IPv6 address.
    sockaddr_storage own{};
    int ownLen = sizeof own;
    const int family = ::getsockname(s, reinterpret_cast<sockaddr*>(&own), &ownLen) == 0 ? own.ss_family
                                                                                        : AF_UNSPEC;
    AddrList addrs;
    resolve(host, port, SOCK_DGRAM, false, addrs, family);
    Deadline deadline(st->timeoutMs.load());
    for (;;) {
        const int n = ::sendto(s, data.data(), static_cast<int>(data.size()), 0, addrs.head->ai_addr,
                               static_cast<int>(addrs.head->ai_addrlen));
        if (n != SOCKET_ERROR) return;
        const int e = ::WSAGetLastError();
        if (e == WSAEWOULDBLOCK && !st->closed.load()) {  // the socket is non-blocking: wait, bounded
            if (!detail::waitReady(*st, POLLOUT, deadline.remaining())) netError(ETIMEDOUT, "send to " + host);
            continue;
        }
        netError(win::errnoOfWsa(e), "send to " + host);
    }
}

std::optional<Datagram> udpReceive(int fd, int timeoutMs) {
    auto st = detail::fdState(fd);  // held: a concurrent close cannot free the socket under us
    const SOCKET s = native(fd);
    Deadline deadline(timeoutMs);
    Datagram d;
    d.data.resize(65536);
    sockaddr_storage ss{};
    for (;;) {
        if (!detail::waitReady(*st, POLLIN, deadline.remaining()) || st->closed.load())
            return std::nullopt;
        int len = sizeof ss;
        // The socket is non-blocking: when another thread took the datagram
        // first, wait again for what is left of the time.
        const int n = ::recvfrom(s, d.data.data(), static_cast<int>(d.data.size()), 0,
                                 reinterpret_cast<sockaddr*>(&ss), &len);
        if (n != SOCKET_ERROR) {
            d.data.resize(static_cast<std::size_t>(n));
            break;
        }
        if (st->closed.load()) return std::nullopt;
        const int e = ::WSAGetLastError();
        if (e == WSAEWOULDBLOCK || e == WSAEINTR || e == WSAECONNRESET) continue;
        netError(win::errnoOfWsa(e), "receive");
    }
    const Address from = addressOf(reinterpret_cast<sockaddr*>(&ss));
    d.host = from.host;
    d.port = from.port;
    return d;
}

#else

int tcpConnect(const std::string& host, int port, int timeoutMs) {
    checkPort(port);
    AddrList addrs;
    resolve(host, port, SOCK_STREAM, false, addrs);
    int fd = -1;
    int lastErr = ECONNREFUSED;
    for (addrinfo* ai = addrs.head; ai; ai = ai->ai_next) {
        fd = detail::newSocket(ai->ai_family, ai->ai_socktype, ai->ai_protocol, true);
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
        fd = detail::newSocket(ai->ai_family, ai->ai_socktype, ai->ai_protocol, false);
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
        if (!detail::waitReady(*st, POLLIN, deadline.remaining())) return std::nullopt;
        if (st->closed.load()) return std::nullopt;
        const int c = detail::acceptSocket(fd);
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
        fd = detail::newSocket(ai->ai_family, ai->ai_socktype, ai->ai_protocol, false);
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
    while ((n = ::sendto(fd, data.data(), data.size(), detail::kNoSigpipe, addrs.head->ai_addr,
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
        if (!detail::waitReady(*st, POLLIN, deadline.remaining()) || st->closed.load()) return std::nullopt;
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
#endif

} // namespace net
} // namespace protoio
