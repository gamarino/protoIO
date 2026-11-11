// protoIO - shared input and output for the protoCore runtimes.
// Copyright (c) 2026 Gustavo Marino <gamarino@gmail.com>. MIT licensed.
//
// Random bytes from the operating system's cryptographically secure
// generator, for identifiers (UUIDs), nonces and keys.
#pragma once

#include <cstddef>
#include <string>

namespace protoio::random {

// The largest draw one call accepts (1 GiB). A runtime sets its own, usually
// much smaller, bound for its language-level call.
inline constexpr std::size_t maxBytes = std::size_t{1} << 30;

// `n` bytes from the OS CSPRNG; `n == 0` answers "". No state is kept in
// protoIO, so concurrent calls need no lock and a forked child never repeats
// its parent's bytes.
//   * Linux: getrandom(2) (flags 0: waits only until the kernel pool is first
//     initialised at boot), repeated on short reads and EINTR; /dev/urandom
//     when the kernel predates getrandom (ENOSYS).
//   * macOS and the BSDs: arc4random_buf(3), which cannot fail.
//   * Windows: BCryptGenRandom with BCRYPT_USE_SYSTEM_PREFERRED_RNG.
//   * Other POSIX systems: /dev/urandom.
// `n > maxBytes` throws InvalidArgument (sysErrno 0) before any allocation.
// A failure of the system generator throws Internal with the errno (0 on
// Windows, where the message names the NTSTATUS).
std::string bytes(std::size_t n);

} // namespace protoio::random
