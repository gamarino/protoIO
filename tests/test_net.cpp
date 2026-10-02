// TCP, TLS and UDP over the loopback interface.
#include "protoio/error.h"
#include "protoio/net.h"
#include "protoio/stream.h"
#include "test_support.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#else
#include <cerrno>
#endif

using protoio::Error;
namespace net = protoio::net;
using namespace std::chrono_literals;

namespace {

template <typename F>
Error errorOf(F f) {
    try {
        f();
    } catch (const Error& e) {
        return e;
    }
    ADD_FAILURE() << "no Error was thrown";
    return Error(Error::Kind::InvalidArgument, "none");
}

} // namespace

TEST(Net, TcpConnectListenAcceptEcho) {
    const int listener = net::tcpListen("127.0.0.1", 0);
    const int port = net::sockName(listener).port;
    ASSERT_GT(port, 0);
    std::thread server([&] {
        auto c = net::tcpAccept(listener, 5000);
        ASSERT_TRUE(c);
        auto line = protoio::readLine(*c);
        protoio::write(*c, "echo:" + line.value_or("<none>") + "\n");
        protoio::close(*c);
    });
    const int s = net::tcpConnect("127.0.0.1", port, 2000);
    EXPECT_EQ(net::peerName(s).port, port);
    EXPECT_EQ(net::peerName(s).host, "127.0.0.1");
    protoio::write(s, "hi\n");
    EXPECT_EQ(protoio::readLine(s), "echo:hi");
    EXPECT_EQ(protoio::readLine(s), std::nullopt);
    protoio::close(s);
    server.join();
    protoio::close(listener);
}

// Why the IPv6 loopback address cannot be used on this host, or nothing when
// it can. Some hosts have no IPv6 at all; on some Windows hosts VPN or
// security software drops all traffic to ::1.
std::optional<std::string> ipv6LoopbackUnusable() {
    int probe = -1;
    try {
        probe = net::tcpListen("::1", 0);
    } catch (const Error& e) {
        return std::string("no IPv6 loopback: ") + e.what();
    }
    const int port = net::sockName(probe).port;
    std::atomic<bool> stop{false};
    std::thread t([&] {
        while (!stop.load())
            if (auto c = net::tcpAccept(probe, 50)) protoio::close(*c);
    });
    std::optional<std::string> why;
    try {
        protoio::close(net::tcpConnect("::1", port, 1000));
    } catch (const Error& e) {
        why = std::string("the IPv6 loopback address ::1 does not answer: ") + e.what();
    }
    stop = true;
    t.join();
    protoio::close(probe);
    return why;
}

TEST(Net, ConnectByName) {
    const int listener = net::tcpListen("localhost", 0);
    const net::Address bound = net::sockName(listener);
    if (bound.host == "::1") {
        if (auto why = ipv6LoopbackUnusable()) {
            protoio::close(listener);
            GTEST_SKIP() << "\"localhost\" is ::1 first here, and " << *why;
        }
    }
    std::thread server([&] { if (auto c = net::tcpAccept(listener, 5000)) protoio::close(*c); });
    const int s = net::tcpConnect("localhost", bound.port, 2000);
    protoio::close(s);
    server.join();
    protoio::close(listener);
}

// Skipped, with the reason, where ::1 cannot be used; ctest then lists the
// test as skipped, not passed (gtest_discover_tests matches "[  SKIPPED ]").
TEST(Net, ConnectOverIpv6Loopback) {
    if (auto why = ipv6LoopbackUnusable()) GTEST_SKIP() << *why;
    const int listener = net::tcpListen("::1", 0);
    const int port = net::sockName(listener).port;
    std::thread server([&] {
        auto c = net::tcpAccept(listener, 5000);
        ASSERT_TRUE(c);
        EXPECT_EQ(net::peerName(*c).host, "::1");
        protoio::write(*c, "six\n");
        protoio::close(*c);
    });
    const int s = net::tcpConnect("::1", port, 2000);
    EXPECT_EQ(protoio::readLine(s), "six");
    protoio::close(s);
    server.join();
    protoio::close(listener);
}

// The wildcard listener ("" = every address) takes IPv4 connections on every
// platform. On Windows it used to bind :: first, which is IPv6-only there.
TEST(Net, WildcardListenerAcceptsIpv4Loopback) {
    const int listener = net::tcpListen("", 0);
    const int port = net::sockName(listener).port;
    ASSERT_GT(port, 0);
    std::thread server([&] {
        auto c = net::tcpAccept(listener, 5000);
        ASSERT_TRUE(c);
        // An IPv4 peer, not ::ffff:127.0.0.1 (macOS lists :: first too).
        EXPECT_EQ(net::peerName(*c).host, "127.0.0.1");
        protoio::write(*c, "ok\n");
        protoio::close(*c);
    });
    const int s = net::tcpConnect("127.0.0.1", port, 2000);
    EXPECT_EQ(protoio::readLine(s), "ok");
    protoio::close(s);
    server.join();
    protoio::close(listener);
}

// The wildcard listener takes IPv6 connections too, on one dual-stack socket
// where the host has IPv6. "localhost" names ::1 first on Windows and macOS,
// so a client of a server listening on every interface must not be refused
// there first (on Windows that cost each connection about 0.3 s).
TEST(Net, WildcardListenerAcceptsIpv6Loopback) {
    if (auto why = ipv6LoopbackUnusable()) GTEST_SKIP() << *why;
    const int listener = net::tcpListen("", 0);
    const int port = net::sockName(listener).port;
    ASSERT_GT(port, 0);
    std::thread server([&] {
        auto c = net::tcpAccept(listener, 5000);
        ASSERT_TRUE(c);
        EXPECT_EQ(net::peerName(*c).host, "::1");
        protoio::write(*c, "six\n");
        protoio::close(*c);
    });
    std::optional<int> s;
    try {
        s = net::tcpConnect("::1", port, 2000);
    } catch (const Error& e) {
        ADD_FAILURE() << "the wildcard listener refused ::1: " << e.what();
        protoio::close(net::tcpConnect("127.0.0.1", port, 2000));  // let the server finish
    }
    if (s) {
        EXPECT_EQ(protoio::readLine(*s), "six");
        protoio::close(*s);
    }
    server.join();
    protoio::close(listener);
}

TEST(Net, AcceptTimeoutAnswersNullopt) {
    const int listener = net::tcpListen("127.0.0.1", 0);
    const auto start = std::chrono::steady_clock::now();
    EXPECT_EQ(net::tcpAccept(listener, 100), std::nullopt);
    EXPECT_GE(std::chrono::steady_clock::now() - start, 90ms);
    protoio::close(listener);
}

TEST(Net, CompetingAcceptorsEachHonourTheirTimeout) {
    // Both threads are woken by one connection; the one that loses the race
    // must wait again for the rest of its time, not block in accept forever.
    const int listener = net::tcpListen("127.0.0.1", 0);
    const int port = net::sockName(listener).port;
    std::atomic<int> accepted{0}, timedOut{0};
    auto acceptor = [&] {
        if (auto c = net::tcpAccept(listener, 700)) { ++accepted; protoio::close(*c); }
        else ++timedOut;
    };
    std::thread a(acceptor), b(acceptor);
    std::this_thread::sleep_for(100ms);
    const int s = net::tcpConnect("127.0.0.1", port, 2000);
    a.join();
    b.join();
    EXPECT_EQ(accepted.load(), 1);
    EXPECT_EQ(timedOut.load(), 1);
    protoio::close(s);
    protoio::close(listener);
}

TEST(Net, CloseWakesABlockedAccept) {
    const int listener = net::tcpListen("127.0.0.1", 0);
    std::optional<int> got = 12345;
    std::thread t([&] { got = net::tcpAccept(listener); });
    std::this_thread::sleep_for(150ms);
    protoio::close(listener);
    t.join();
    EXPECT_EQ(got, std::nullopt);
}

// close wakes a thread waiting in accept, in a UDP receive and in a socket
// read at once, not at the end of a polling interval: the median wake-up
// latency over several rounds stays far below the 50 ms slices the waits
// used to be cut into.
TEST(Net, CloseWakesBlockedWaitersPromptly) {
    using Clock = std::chrono::steady_clock;
    constexpr int kRounds = 9;
    auto median = [](std::vector<double> v) {
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
    };
    // Starts `wait` on a thread, closes `fd` once the thread is waiting, and
    // answers the milliseconds from the close to the end of the wait.
    auto latency = [](int fd, const std::function<void()>& wait) {
        std::atomic<Clock::time_point> woke{};
        std::thread t([&] {
            wait();
            woke = Clock::now();
        });
        std::this_thread::sleep_for(40ms);
        const auto closed = Clock::now();
        protoio::close(fd);
        t.join();
        return std::chrono::duration<double, std::milli>(woke.load() - closed).count();
    };
    std::vector<double> accept, udp, read;
    for (int i = 0; i < kRounds; ++i) {
        const int l = net::tcpListen("127.0.0.1", 0);
        accept.push_back(latency(l, [&] { EXPECT_EQ(net::tcpAccept(l), std::nullopt); }));
        const int u = net::udpBind("127.0.0.1", 0);
        udp.push_back(latency(u, [&] { EXPECT_FALSE(net::udpReceive(u).has_value()); }));
        int pair[2];
        protoio_test::socketPair(pair);
        read.push_back(latency(pair[0], [&] { EXPECT_EQ(protoio::readLine(pair[0]), std::nullopt); }));
        protoio::close(pair[1]);
    }
    // Reported, so a CI log shows the latency itself, not only a pass.
    std::printf("median wake-up after close (ms): accept %.2f, udp %.2f, read %.2f\n", median(accept), median(udp),
                median(read));
    EXPECT_LT(median(accept), 15.0);
    EXPECT_LT(median(udp), 15.0);
    EXPECT_LT(median(read), 15.0);
}

// A listener whose backlog is full for a moment does not make connects fail
// at once: on Windows, which answers such a connect with a reset, the
// library retries a refused loopback connect for a short while. Not on
// macOS, whose kernel itself resets connects beyond a full backlog
// (ECONNRESET): there the backlog given to tcpListen must be large enough.
#if !defined(__APPLE__)
TEST(Net, ConnectsSurviveABrieflyFullBacklog) {
    const int listener = net::tcpListen("127.0.0.1", 0, 1);
    const int port = net::sockName(listener).port;
    constexpr int kClients = 16;
    std::vector<int> fds(kClients, -1);
    std::vector<std::string> failures(kClients);
    std::vector<std::thread> clients;
    for (int i = 0; i < kClients; ++i) {
        clients.emplace_back([&, i] {
            try {
                fds[i] = net::tcpConnect("127.0.0.1", port, 5000);
            } catch (const Error& e) {
                failures[i] = e.what();
            }
        });
    }
    // The listener starts accepting a little later, and keeps accepting
    // until every client has its answer. (Linux completes the extra connects
    // at once and lets the server catch up later; Windows refuses them.)
    std::atomic<bool> stop{false};
    std::thread acceptor([&] {
        std::this_thread::sleep_for(50ms);
        while (!stop.load())
            if (auto c = net::tcpAccept(listener, 50)) protoio::close(*c);
    });
    for (auto& t : clients) t.join();
    stop = true;
    acceptor.join();
    for (int i = 0; i < kClients; ++i) {
        EXPECT_GE(fds[i], 0) << failures[i];
        if (fds[i] >= 0) protoio::close(fds[i]);
    }
    protoio::close(listener);
}
#endif

// A server can listen on its port again right after it stopped, while the
// connections it closed linger in TIME_WAIT.
TEST(Net, APortCanBeListenedOnAgainWhileItsClosedConnectionsLinger) {
    const int listener = net::tcpListen("127.0.0.1", 0);
    const int port = net::sockName(listener).port;
    std::thread server([&] {
        auto c = net::tcpAccept(listener, 5000);
        ASSERT_TRUE(c);
        protoio::write(*c, "bye\n");
        protoio::close(*c);  // the server closes first: its side enters TIME_WAIT
    });
    const int s = net::tcpConnect("127.0.0.1", port, 2000);
    EXPECT_EQ(protoio::readLine(s), "bye");
    EXPECT_EQ(protoio::readLine(s), std::nullopt);
    server.join();
    protoio::close(s);
    protoio::close(listener);
    int again = -1;
    EXPECT_NO_THROW(again = net::tcpListen("127.0.0.1", port));
    if (again >= 0) protoio::close(again);
}

// A port being listened on cannot be bound by another socket, not even one
// that asks for SO_REUSEADDR (on Windows that option would let it take the
// port over; the listener sets SO_EXCLUSIVEADDRUSE).
TEST(Net, AListeningPortCannotBeTakenOver) {
    const int listener = net::tcpListen("127.0.0.1", 0);
    const int port = net::sockName(listener).port;
    EXPECT_EQ(errorOf([&] { net::tcpListen("127.0.0.1", port); }).sysErrno, EADDRINUSE);
#ifdef _WIN32
    const SOCKET thief = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ASSERT_NE(thief, INVALID_SOCKET);
    const BOOL one = TRUE;
    ::setsockopt(thief, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof one);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<u_short>(port));
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    EXPECT_EQ(::bind(thief, reinterpret_cast<sockaddr*>(&a), sizeof a), SOCKET_ERROR)
        << "a second socket took over the listening port";
    ::closesocket(thief);
#endif
    protoio::close(listener);
}

TEST(Net, ConnectionRefusedKind) {
    const int listener = net::tcpListen("127.0.0.1", 0);
    const int port = net::sockName(listener).port;
    protoio::close(listener);
    Error e = errorOf([&] { net::tcpConnect("127.0.0.1", port, 2000); });
    EXPECT_EQ(e.kind, Error::Kind::ConnectionRefused);
    EXPECT_EQ(e.sysErrno, ECONNREFUSED);
}

TEST(Net, NameLookupKind) {
    Error e = errorOf([&] { net::tcpConnect("no-such-host.invalid", 80, 2000); });
    EXPECT_EQ(e.kind, Error::Kind::NameLookup);
}

TEST(Net, TlsHandshakeWithASilentPeerTimesOut) {
    // The listener's backlog completes the TCP handshake, but nobody ever
    // answers the ClientHello.
    const int listener = net::tcpListen("127.0.0.1", 0);
    const int port = net::sockName(listener).port;
    const int s = net::tcpConnect("127.0.0.1", port, 2000);
    protoio::setTimeout(s, 300);
    const auto start = std::chrono::steady_clock::now();
    Error e = errorOf([&] { net::tlsConnect(s, "localhost", true); });
    EXPECT_EQ(e.kind, Error::Kind::ConnectionTimedOut);
    EXPECT_LT(std::chrono::steady_clock::now() - start, 3s);
    protoio::close(s);
    protoio::close(listener);
}

TEST(Net, TlsEchoWithoutVerification) {
    protoio_test::TlsEchoServer server;
    const int s = net::tcpConnect("127.0.0.1", server.port(), 2000);
    protoio::setTimeout(s, 5000);
    net::tlsConnect(s, "localhost", false);
    protoio::write(s, "secret\n");
    EXPECT_EQ(protoio::readLine(s), "echo:secret");
    EXPECT_EQ(protoio::readLine(s), std::nullopt);
    protoio::close(s);
}

TEST(Net, TlsVerificationRefusesASelfSignedCertificate) {
    protoio_test::TlsEchoServer server;
    const int s = net::tcpConnect("127.0.0.1", server.port(), 2000);
    protoio::setTimeout(s, 5000);
    Error e = errorOf([&] { net::tlsConnect(s, "localhost", true); });
    EXPECT_EQ(e.kind, Error::Kind::Network);
    EXPECT_NE(std::string(e.what()).find("certificate"), std::string::npos) << e.what();
    protoio::close(s);
}

// A successful verified handshake, not only a refused one: the chain is
// built up to a certificate authority in the store tlsConnect verifies
// against, and the server's certificate must name the host. The CA is
// generated by the test and unknown to the system, so the handshake can only
// succeed through trustCertificates; before that call it must fail.
TEST(Net, TlsVerifiedHandshakeWithATrustedCertificateAuthority) {
    protoio_test::TlsEchoServer server(true);
    auto connect = [&] {
        const int s = net::tcpConnect("127.0.0.1", server.port(), 2000);
        protoio::setTimeout(s, 5000);
        return s;
    };
    int s = connect();
    Error untrusted = errorOf([&] { net::tlsConnect(s, "localhost", true); });
    EXPECT_EQ(untrusted.kind, Error::Kind::Network);
    EXPECT_NE(std::string(untrusted.what()).find("certificate"), std::string::npos) << untrusted.what();
    protoio::close(s);

    net::trustCertificates(server.caPem());
    s = connect();
    net::tlsConnect(s, "localhost", true);
    protoio::write(s, "verified\n");
    EXPECT_EQ(protoio::readLine(s), "echo:verified");
    protoio::close(s);

    // The host name is still checked against the trusted certificate.
    s = connect();
    Error wrongName = errorOf([&] { net::tlsConnect(s, "example.com", true); });
    EXPECT_EQ(wrongName.kind, Error::Kind::Network);
    EXPECT_NE(std::string(wrongName.what()).find("certificate"), std::string::npos) << wrongName.what();
    protoio::close(s);
}

TEST(Net, TrustCertificatesNeedsACertificate) {
    EXPECT_EQ(errorOf([] { net::trustCertificates("no certificate here"); }).kind, Error::Kind::InvalidArgument);
}

TEST(Net, ConcurrentReaderAndWriterOnOneTcpSocket) {
    const int listener = net::tcpListen("127.0.0.1", 0);
    const int port = net::sockName(listener).port;
    constexpr int kLines = 2000;
    std::thread server([&] {
        auto c = net::tcpAccept(listener, 5000);
        ASSERT_TRUE(c);
        // Echo every line back.
        while (auto line = protoio::readLine(*c)) protoio::write(*c, *line + "\n");
        protoio::close(*c);
    });
    const int s = net::tcpConnect("127.0.0.1", port, 2000);
    int received = 0;
    std::thread reader([&] {
        while (auto line = protoio::readLine(s)) {
            if (*line == "m" + std::to_string(received)) ++received;
            if (received == kLines) break;
        }
    });
    for (int i = 0; i < kLines; ++i) protoio::write(s, "m" + std::to_string(i) + "\n");
    reader.join();
    EXPECT_EQ(received, kLines);
    protoio::close(s);
    server.join();
    protoio::close(listener);
}

TEST(Net, UdpSendAndReceive) {
    const int a = net::udpBind("127.0.0.1", 0);
    const int b = net::udpBind("127.0.0.1", 0);
    const int portA = net::sockName(a).port;
    const int portB = net::sockName(b).port;
    net::udpSend(b, "127.0.0.1", portA, std::string("ping\0x", 6));
    auto d = net::udpReceive(a, 2000);
    ASSERT_TRUE(d);
    EXPECT_EQ(d->data, std::string("ping\0x", 6));
    EXPECT_EQ(d->host, "127.0.0.1");
    EXPECT_EQ(d->port, portB);
    EXPECT_EQ(net::udpReceive(a, 100), std::nullopt);
    protoio::close(a);
    protoio::close(b);
}

TEST(Net, UdpToAnIpv6TargetFromAnIpv4SocketIsANameLookupError) {
    const int s = net::udpBind("127.0.0.1", 0);
    Error e = errorOf([&] { net::udpSend(s, "::1", 9, "x"); });
    EXPECT_EQ(e.kind, Error::Kind::NameLookup);
    protoio::close(s);
}

TEST(Net, CloseWakesABlockedUdpReceive) {
    const int s = net::udpBind("127.0.0.1", 0);
    std::optional<net::Datagram> got = net::Datagram{"unset", "", 0};
    std::thread t([&] { got = net::udpReceive(s); });
    std::this_thread::sleep_for(150ms);
    protoio::close(s);
    t.join();
    EXPECT_EQ(got.has_value(), false);
}
