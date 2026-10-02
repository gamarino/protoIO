// protoIO - shared input and output for the protoCore runtimes.
// Copyright (c) 2026 Gustavo Marino <gamarino@gmail.com>. MIT licensed.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// TCP, TLS (client side) and UDP. Sockets are plain descriptors: read, write
// and close them with the functions of protoio/stream.h, which also carry the
// threading guarantees.
//
// Errors: ConnectionRefused, ConnectionTimedOut, NameLookup (a host that does
// not resolve, or does not resolve in the socket's address family) and
// Network for anything else, including TLS failures.
namespace protoio::net {

struct Address {
    std::string host;  // numeric, e.g. "127.0.0.1" or "::1"
    int port = 0;
};

// Connects to host:port, trying each address the name resolves to. With
// `timeoutMs` >= 0 each attempt is bounded. The socket is blocking,
// close-on-exec and has TCP_NODELAY set.
int tcpConnect(const std::string& host, int port, int timeoutMs = -1);

// Listens on host:port (an empty host: every interface; port 0: a free port,
// see sockName). SO_REUSEADDR is set.
int tcpListen(const std::string& host, int port, int backlog = 128);

// A connected socket, or std::nullopt when `timeoutMs` elapsed first or the
// listening socket was closed meanwhile.
std::optional<int> tcpAccept(int fd, int timeoutMs = -1);

// The local and the remote address of a socket.
Address sockName(int fd);
Address peerName(int fd);

// Upgrades a connected socket to TLS 1.2 or later, as a client. With
// `verify`, the certificate chain is checked against the system's trust store
// and the certificate must name `host`. The socket becomes non-blocking: the
// handshake and every later read and write wait in poll(2), bounded by the
// descriptor's timeout (protoio::setTimeout), and a handshake that expires
// throws ConnectionTimedOut.
void tlsConnect(int fd, const std::string& host, bool verify = true);

// Adds the PEM certificates in `pem` (one or more "BEGIN CERTIFICATE"
// blocks) to the trust store tlsConnect (and https) verifies against, in
// addition to the system's, for the rest of the process: a private
// certificate authority, or a test's own. Throws InvalidArgument when `pem`
// holds no certificate.
void trustCertificates(const std::string& pem);

// A UDP socket bound to host:port (an empty host: 0.0.0.0; port 0: a free
// port). SO_REUSEADDR and SO_BROADCAST are set.
int udpBind(const std::string& host, int port);

// Sends one datagram. `host` is resolved in the socket's own address family:
// an IPv6 destination from an IPv4 socket throws NameLookup.
void udpSend(int fd, const std::string& host, int port, std::string_view data);

struct Datagram {
    std::string data;
    std::string host;  // the sender
    int port = 0;
};

// The next datagram (at most 65536 bytes), or std::nullopt when `timeoutMs`
// elapsed first or the socket was closed meanwhile.
std::optional<Datagram> udpReceive(int fd, int timeoutMs = -1);

#ifdef _WIN32
// Windows only: the SOCKET behind a socket descriptor of the library (a
// SOCKET is not a C runtime descriptor, so the library numbers its sockets
// itself), for calls the library does not offer (setsockopt, a TLS server's
// SSL_set_fd, ...). The library still owns it: close it with protoio::close.
// Throws Network (ENOTSOCK) for a descriptor that is not one of its sockets.
std::uintptr_t nativeSocket(int fd);
#endif

} // namespace protoio::net
