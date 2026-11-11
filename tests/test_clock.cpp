// protoIO - shared input and output for the protoCore runtimes.
// Copyright (c) 2026 Gustavo Marino <gamarino@gmail.com>. MIT licensed.
#include "protoio/clock.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>

using protoio::clock::utcMicroseconds;

namespace {

std::int64_t systemMicros() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace

TEST(UtcMicroseconds, AfterYear2026) {
    // 2026-01-01T00:00:00Z in microseconds: a clock answering seconds or
    // milliseconds would be far below it.
    EXPECT_GT(utcMicroseconds(), INT64_C(1767225600000000));
}

TEST(UtcMicroseconds, BracketedBySystemClock) {
    // Two independent reads of the OS wall clock through
    // std::chrono::system_clock, in microseconds, taken around the call bound
    // it: a clock in another unit or from another epoch falls outside
    // [before, after]. No elapsed time is asserted. std::time() is not used
    // for the bounds: on Linux it reads the clock as of the last timer tick,
    // so near a second boundary it can answer the previous second.
    std::int64_t before = systemMicros();
    std::int64_t us = utcMicroseconds();
    std::int64_t after = systemMicros();
    EXPECT_LE(before, us);
    EXPECT_LE(us, after);
}
