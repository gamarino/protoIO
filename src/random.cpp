// protoIO - shared input and output for the protoCore runtimes.
// Copyright (c) 2026 Gustavo Marino <gamarino@gmail.com>. MIT licensed.
//
// Random bytes from the operating system's CSPRNG (see protoio/random.h).
#include "protoio/random.h"
#include "internal.h"

#include <cerrno>
#include <cstdio>
#include <string>

#if defined(_WIN32)
#include <bcrypt.h>
#else
#include <fcntl.h>
#include <unistd.h>
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) \
    || defined(__DragonFly__)
#define PROTOIO_RANDOM_ARC4 1
#include <stdlib.h>
#elif defined(__linux__)
#include <sys/random.h>
#endif
#endif

#ifndef _WIN32
namespace protoio::detail {

namespace {
[[noreturn]] void failRandom(const char* what, int err) {
    throw Error(Error::Kind::Internal, std::string("random::bytes: ") + what + " failed", err);
}
} // namespace

std::size_t fillRandom(char* buf, std::size_t n, const RandomCall& call, const char* what,
                       bool fallbackAllowed) {
    std::size_t done = 0;
    while (done < n) {
        const long r = call(buf + done, n - done);
        if (r > 0) {
            done += static_cast<std::size_t>(r);
            continue;
        }
        if (r == 0) failRandom(what, EIO);  // an end of file: the source is broken
        const int err = errno;
        if (err == EINTR) continue;
        if (fallbackAllowed && (err == ENOSYS || err == EPERM)) return done;
        failRandom(what, err);
    }
    return done;
}

void randomFromDevice(char* buf, std::size_t n) {
    int fd;
    do {
        fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) failRandom("open /dev/urandom", errno);
    struct Closer {
        int fd;
        ~Closer() { ::close(fd); }
    } closer{fd};
    fillRandom(buf, n,
               [fd](char* p, std::size_t len) { return static_cast<long>(::read(fd, p, len)); },
               "read /dev/urandom", false);
}

} // namespace protoio::detail
#endif

namespace protoio::random {
namespace {

void fill(char* buf, std::size_t n) {
#if defined(_WIN32)
    // BCryptGenRandom takes a ULONG length: maxBytes fits, but stay general.
    while (n > 0) {
        const ULONG chunk = n > 0xFFFFFFFFu ? 0xFFFFFFFFu : static_cast<ULONG>(n);
        const NTSTATUS status = ::BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(buf), chunk,
                                                  BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        if (!BCRYPT_SUCCESS(status)) {
            char msg[64];
            std::snprintf(msg, sizeof msg, "random::bytes: BCryptGenRandom failed (0x%08lx)",
                          static_cast<unsigned long>(status));
            throw Error(Error::Kind::Internal, msg, 0);
        }
        buf += chunk;
        n -= chunk;
    }
#elif defined(PROTOIO_RANDOM_ARC4)
    ::arc4random_buf(buf, n);
#elif defined(__linux__)
    // getrandom(2), flags 0. ENOSYS (a kernel older than 3.17) and EPERM (a
    // seccomp profile that does not know the call, as some container
    // runtimes have) finish from /dev/urandom, as CPython's os.urandom does.
    const std::size_t done = detail::fillRandom(
        buf, n,
        [](char* p, std::size_t len) { return static_cast<long>(::getrandom(p, len, 0)); },
        "getrandom", true);
    if (done < n) detail::randomFromDevice(buf + done, n - done);
#else
    detail::randomFromDevice(buf, n);
#endif
}

} // namespace

std::string bytes(std::size_t n) {
    if (n > maxBytes)
        throw Error(Error::Kind::InvalidArgument,
                    "random::bytes: " + std::to_string(n) + " bytes asked, more than the limit of "
                        + std::to_string(maxBytes));
    std::string out(n, '\0');
    if (n > 0) fill(out.data(), n);
    return out;
}

} // namespace protoio::random
