// protoIO - shared input and output for the protoCore runtimes.
// Copyright (c) 2026 Gustavo Marino <gamarino@gmail.com>. MIT licensed.
//
// The wall clock in UTC.
#pragma once

#include <cstdint>

namespace protoio::clock {

// Microseconds since 1970-01-01T00:00:00Z (the Unix epoch, leap seconds not
// counted), from the system's wall clock: clock_gettime(CLOCK_REALTIME) on
// POSIX, GetSystemTimePreciseAsFileTime on Windows. It is not monotonic: it
// follows the system clock when it is set or adjusted, so it dates events and
// must not measure durations (use a monotonic clock for that). The resolution
// is the system's, at best one microsecond. A failure of the system call,
// practically unreachable, throws Internal with the errno.
std::int64_t utcMicroseconds();

} // namespace protoio::clock
