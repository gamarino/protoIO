// protoIO - shared input and output for the protoCore runtimes.
// Copyright (c) 2026 Gustavo Marino <gamarino@gmail.com>. MIT licensed.
//
// The wall clock in UTC (see protoio/clock.h).
#include "protoio/clock.h"
#include "protoio/error.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <cerrno>
#include <ctime>
#endif

namespace protoio::clock {

std::int64_t utcMicroseconds() {
#if defined(_WIN32)
    // A FILETIME counts 100-nanosecond intervals since 1601-01-01T00:00:00Z.
    FILETIME ft;
    ::GetSystemTimePreciseAsFileTime(&ft);
    const std::int64_t ticks = static_cast<std::int64_t>(
        (static_cast<std::uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime);
    constexpr std::int64_t kUnixEpochTicks = INT64_C(116444736000000000);
    const std::int64_t sinceEpoch = ticks - kUnixEpochTicks;
    // Floor division, so an instant before 1970 rounds down as on POSIX.
    return sinceEpoch >= 0 ? sinceEpoch / 10 : -((-sinceEpoch + 9) / 10);
#else
    struct timespec ts;
    if (::clock_gettime(CLOCK_REALTIME, &ts) != 0)
        throw Error(Error::Kind::Internal, "clock::utcMicroseconds: clock_gettime failed", errno);
    // tv_nsec is in [0, 1e9) also before 1970, so this rounds down.
    return static_cast<std::int64_t>(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
#endif
}

} // namespace protoio::clock
