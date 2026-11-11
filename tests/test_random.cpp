// protoIO - shared input and output for the protoCore runtimes.
// Copyright (c) 2026 Gustavo Marino <gamarino@gmail.com>. MIT licensed.
#include "protoio/random.h"
#include "protoio/error.h"
#include "internal.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <functional>
#include <limits>
#include <set>
#include <string>
#include <thread>
#include <vector>

using protoio::random::bytes;

TEST(RandomBytes, ZeroIsEmpty) {
    EXPECT_TRUE(bytes(0).empty());
}

TEST(RandomBytes, Length) {
    // Boundaries around 256 and one large draw. No size forces getrandom's
    // short reads (a single call filled 64 MiB on Linux 7.0) or
    // BCryptGenRandom's ULONG chunking (above 4 GiB, beyond maxBytes); the
    // loop that handles short reads and EINTR is driven through the
    // detail::fillRandom seam below instead.
    for (std::size_t n : {std::size_t{1}, std::size_t{16}, std::size_t{256}, std::size_t{257},
                          std::size_t{1} << 20})
        EXPECT_EQ(bytes(n).size(), n) << "n = " << n;
}

TEST(RandomBytes, TwoDrawsDiffer) {
    // Equal 32-byte draws from a working CSPRNG have probability 2^-256:
    // the test depends on no timing and no seed.
    EXPECT_NE(bytes(32), bytes(32));
}

TEST(RandomBytes, NotAllZero) {
    std::string b = bytes(4096);
    EXPECT_TRUE(std::any_of(b.begin(), b.end(), [](char c) { return c != 0; }));
}

TEST(RandomBytes, RefusesMoreThanTheLimit) {
    // A size beyond maxBytes is refused before any allocation or system call.
    try {
        bytes(protoio::random::maxBytes + 1);
        FAIL() << "no exception";
    } catch (const protoio::Error& e) {
        EXPECT_EQ(e.kind, protoio::Error::Kind::InvalidArgument);
        EXPECT_EQ(e.sysErrno, 0);
    }
    EXPECT_THROW(bytes(std::numeric_limits<std::size_t>::max()), protoio::Error);
}

TEST(RandomBytes, ConcurrentDraws) {
    // random::bytes keeps no state, so concurrent callers need no lock (the
    // ThreadSanitizer run checks it) and never share a draw.
    constexpr int kThreads = 8, kDraws = 100;
    std::vector<std::vector<std::string>> drawn(kThreads);
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t)
        threads.emplace_back([&drawn, t] {
            for (int i = 0; i < kDraws; ++i) drawn[t].push_back(bytes(16));
        });
    for (auto& th : threads) th.join();
    std::set<std::string> distinct;
    for (const auto& v : drawn) distinct.insert(v.begin(), v.end());
    EXPECT_EQ(distinct.size(), std::size_t{kThreads * kDraws});
}

// ---- the POSIX paths behind random::bytes, through protoio::detail --------

#ifndef _WIN32
using protoio::detail::fillRandom;
using protoio::detail::randomFromDevice;

namespace {

// A fake source: answers the next scripted result. A positive result writes
// that many bytes (counting up from `next`); -1 sets the scripted errno.
struct Script {
    struct Step { long result; int err; };
    std::vector<Step> steps;
    std::size_t at = 0;
    unsigned char next = 1;
    long operator()(char* buf, std::size_t n) {
        const Step s = at < steps.size() ? steps[at++] : Step{static_cast<long>(n), 0};
        if (s.result < 0) {
            errno = s.err;
            return -1;
        }
        const std::size_t k = std::min<std::size_t>(static_cast<std::size_t>(s.result), n);
        for (std::size_t i = 0; i < k; ++i) buf[i] = static_cast<char>(next++);
        return static_cast<long>(k);
    }
};

bool countsUp(const std::string& b, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i)
        if (static_cast<std::size_t>(static_cast<unsigned char>(b[i])) != i + 1) return false;
    return true;
}

} // namespace

TEST(RandomBytes, FillContinuesAfterShortReadsAndEintr) {
    Script s{{{3, 0}, {-1, EINTR}, {1, 0}, {-1, EINTR}, {-1, EINTR}, {2, 0}, {100, 0}}};
    std::string b(10, '\0');
    EXPECT_EQ(fillRandom(b.data(), b.size(), std::ref(s), "fake", false), 10u);
    EXPECT_TRUE(countsUp(b, 10));
    EXPECT_EQ(s.at, 7u);
}

TEST(RandomBytes, FillStopsForTheFallbackOnEnosysAndEperm) {
    for (int err : {ENOSYS, EPERM}) {
        Script s{{{4, 0}, {-1, err}}};
        std::string b(16, '\0');
        EXPECT_EQ(fillRandom(b.data(), b.size(), std::ref(s), "fake", true), 4u) << err;
        EXPECT_TRUE(countsUp(b, 4));
    }
}

TEST(RandomBytes, FillThrowsInternalOtherwise) {
    struct Case { Script::Step step; bool fallback; int expected; };
    const Case cases[] = {
        {{-1, ENOSYS}, false, ENOSYS},  // no fallback allowed (reading the device)
        {{-1, EPERM}, false, EPERM},
        {{-1, EFAULT}, true, EFAULT},   // not a fallback errno
        {{0, 0}, true, EIO},            // an end of file
    };
    for (const Case& c : cases) {
        Script s{{{2, 0}, c.step}};
        std::string b(8, '\0');
        try {
            fillRandom(b.data(), b.size(), std::ref(s), "fake", c.fallback);
            ADD_FAILURE() << "no exception for errno " << c.expected;
        } catch (const protoio::Error& e) {
            EXPECT_EQ(e.kind, protoio::Error::Kind::Internal);
            EXPECT_EQ(e.sysErrno, c.expected);
            EXPECT_NE(std::string(e.what()).find("fake"), std::string::npos) << e.what();
        }
    }
}

TEST(RandomBytes, DeviceFallback) {
    // The /dev/urandom source on its own: the only one on POSIX systems
    // without getrandom or arc4random_buf, and Linux's ENOSYS/EPERM fallback.
    for (std::size_t n : {std::size_t{1}, std::size_t{257}}) {
        std::string b(n, '\0');
        randomFromDevice(b.data(), n);
        EXPECT_EQ(b.size(), n);
    }
    std::string a(32, '\0'), c(32, '\0');
    randomFromDevice(a.data(), a.size());
    randomFromDevice(c.data(), c.size());
    EXPECT_NE(a, c);  // equal with probability 2^-256
}
#else
TEST(RandomBytes, DeviceFallback) {
    GTEST_SKIP() << "POSIX only: Windows draws from BCryptGenRandom";
}
#endif
