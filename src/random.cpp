// protoIO - shared input and output for the protoCore runtimes.
// Copyright (c) 2026 Gustavo Marino <gamarino@gmail.com>. MIT licensed.
//
// Random bytes from the operating system's CSPRNG (see protoio/random.h).
#include "protoio/random.h"
#include "protoio/error.h"

#include <cerrno>
#include <cstdio>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) \
    || defined(__DragonFly__)
#define PROTOIO_RANDOM_ARC4 1
#include <stdlib.h>
#else
#include <fcntl.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/random.h>
#endif
#endif

namespace protoio::random {
namespace {

#if !defined(_WIN32) && !defined(PROTOIO_RANDOM_ARC4)
[[noreturn]] void failErrno(const char* what, int err) {
    throw Error(Error::Kind::Internal, std::string("random::bytes: ") + what + " failed", err);
}

// Fills buf from /dev/urandom: older Linux kernels and other POSIX systems.
void fillFromDevice(char* buf, std::size_t n) {
    int fd;
    do {
        fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) failErrno("open /dev/urandom", errno);
    std::size_t done = 0;
    while (done < n) {
        const ssize_t r = ::read(fd, buf + done, n - done);
        if (r > 0) {
            done += static_cast<std::size_t>(r);
        } else if (r < 0 && errno == EINTR) {
            continue;
        } else {
            const int err = r < 0 ? errno : EIO;  // end of file: the device is broken
            ::close(fd);
            failErrno("read /dev/urandom", err);
        }
    }
    ::close(fd);
}
#endif

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
    std::size_t done = 0;
    while (done < n) {
        const ssize_t r = ::getrandom(buf + done, n - done, 0);
        if (r > 0) {
            done += static_cast<std::size_t>(r);
        } else if (r < 0 && errno == EINTR) {
            continue;
        } else if (r < 0 && errno == ENOSYS) {
            fillFromDevice(buf + done, n - done);  // a kernel older than 3.17
            return;
        } else {
            failErrno("getrandom", r < 0 ? errno : EIO);
        }
    }
#else
    fillFromDevice(buf, n);
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
