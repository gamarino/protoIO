// TCP, TLS and UDP over the loopback interface.
#include "protoio/error.h"
#include "protoio/net.h"
#include "protoio/stream.h"
#include "test_support.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

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

TEST(Net, ConnectByNameAndOverIpv6Loopback) {
#ifdef _WIN32
    // Some Windows hosts (VPN or security software) drop all traffic to ::1,
    // where "localhost" resolves first; nothing can connect there.
    try {
        const int probe = net::tcpListen("::1", 0);
        const int port = net::sockName(probe).port;
        std::thread t([&] { if (auto c = net::tcpAccept(probe, 3000)) protoio::close(*c); });
        try {
            protoio::close(net::tcpConnect("::1", port, 1000));
        } catch (const Error&) {
            protoio::close(probe);  // wakes the acceptor
            t.join();
            GTEST_SKIP() << "the IPv6 loopback address ::1 does not answer on this host";
        }
        t.join();
        protoio::close(probe);
    } catch (const Error& e) {
        GTEST_SKIP() << "no IPv6 loopback: " << e.what();
    }
#endif
    const int listener = net::tcpListen("localhost", 0);
    const int port = net::sockName(listener).port;
    std::thread server([&] { if (auto c = net::tcpAccept(listener, 5000)) protoio::close(*c); });
    const int s = net::tcpConnect("localhost", port, 2000);
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
        protoio::write(*c, "ok\n");
        protoio::close(*c);
    });
    const int s = net::tcpConnect("127.0.0.1", port, 2000);
    EXPECT_EQ(protoio::readLine(s), "ok");
    protoio::close(s);
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
