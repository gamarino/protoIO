// protoIO - shared input and output for the protoCore runtimes.
// Copyright (c) 2026 Gustavo Marino <gamarino@gmail.com>. MIT licensed.
#include "protoio/random.h"
#include "protoio/error.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
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
    // 257 crosses getentropy's 256-byte chunk; 1 MiB crosses getrandom's
    // short-read threshold on Linux (32 MiB - 1 per call, 256 bytes before
    // the pool is initialised) and BCryptGenRandom's ULONG argument path.
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
