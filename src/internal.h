// protoIO - shared input and output for the protoCore runtimes.
// Copyright (c) 2026 Gustavo Marino <gamarino@gmail.com>. MIT licensed.
//
// Internal: the descriptor registry and the helpers the modules share.
#pragma once

#include "protoio/error.h"

#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>

#ifdef _WIN32
#include "platform_win.h"
#else
#include <poll.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#ifndef _WIN32
namespace protoio::detail {
// Descriptors are created close-on-exec, and sockets never raise SIGPIPE.
// Linux (and the BSDs) do the first atomically with SOCK_CLOEXEC, accept4 and
// pipe2, and the second per call with MSG_NOSIGNAL; these are exactly the calls
// made there. macOS has none of the four: the flags are set right after the
// descriptor is created, and SIGPIPE is turned off per socket (SO_NOSIGPIPE).
#if defined(SOCK_CLOEXEC)
inline int newSocket(int family, int type, int protocol, bool nonBlocking) {
    return ::socket(family, type | SOCK_CLOEXEC | (nonBlocking ? SOCK_NONBLOCK : 0), protocol);
}
inline int acceptSocket(int fd) { return ::accept4(fd, nullptr, nullptr, SOCK_CLOEXEC); }
inline int newPipe(int fds[2]) { return ::pipe2(fds, O_CLOEXEC); }
inline constexpr int kNoSigpipe = MSG_NOSIGNAL;
#else
inline void noSigpipe(int fd) {
#ifdef SO_NOSIGPIPE
    const int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#else
    (void)fd;
#endif
}
inline int newSocket(int family, int type, int protocol, bool nonBlocking) {
    const int fd = ::socket(family, type, protocol);
    if (fd < 0) return fd;
    ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    if (nonBlocking) ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
    noSigpipe(fd);
    return fd;
}
inline int acceptSocket(int fd) {
    const int c = ::accept(fd, nullptr, nullptr);
    if (c < 0) return c;
    ::fcntl(c, F_SETFD, FD_CLOEXEC);
    // A BSD accept inherits O_NONBLOCK from the listening socket, which is
    // non-blocking (tcpListen); Linux's accept4 does not, and callers expect a
    // blocking connection.
    ::fcntl(c, F_SETFL, ::fcntl(c, F_GETFL) & ~O_NONBLOCK);
    noSigpipe(c);
    return c;
}
inline int newPipe(int fds[2]) {
    if (::pipe(fds) != 0) return -1;
    ::fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    ::fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    return 0;
}
inline constexpr int kNoSigpipe = 0;
#endif
} // namespace protoio::detail
#endif

typedef struct ssl_st SSL;
typedef struct ssl_ctx_st SSL_CTX;

namespace protoio::detail {

// ------------------------------------------------------------------ errors

// The text of an errno value (strerror, which on Windows lacks the network
// codes).
std::string errorText(int err);

// ENOENT -> FileNotFound, EEXIST -> FileExists, anything else -> FileSystem.
[[noreturn]] void fileError(const std::string& path, int err, const char* action);

// ECONNREFUSED -> ConnectionRefused; ETIMEDOUT, EAGAIN -> ConnectionTimedOut;
// anything else -> Network.
[[noreturn]] void netError(int err, const std::string& what);

// ------------------------------------------------------------- descriptors
//
// One state per descriptor (files, pipes, sockets, the standard streams): the
// read-ahead buffer line reading needs and, for a socket upgraded to TLS, its
// SSL object.
//
// Several threads may use one descriptor at once. Readers serialise on
// `readMutex` (the buffer is consumed atomically: each line goes to exactly
// one reader), writers on `writeMutex`, and the SSL object, which OpenSSL
// does not let two threads use at once, on `sslMutex`, held only around one
// non-blocking SSL call: a TLS socket is non-blocking and waits in poll(2)
// with no lock held, so a reader waiting for data never blocks a writer.
//
// The state owns the descriptor once it exists. `close` removes it from the
// registry and shuts a socket down, which wakes a thread blocked on it; the
// descriptor itself is closed, and the SSL object freed, when the last user
// lets the state go. Closing the number while another thread still waited on
// it would let the next open() reuse it under that thread.
struct FdState {
    const int fd;
    const bool isSocket;
    std::mutex readMutex;
    std::mutex writeMutex;
    std::mutex sslMutex;
    std::string buf;                  // guarded by readMutex
    bool eof = false;                 // guarded by readMutex
    std::atomic<SSL*> ssl{nullptr};
    std::atomic<int> timeoutMs{-1};   // -1: block without limit
    std::atomic<bool> closed{false};

    FdState(int d, bool sock) : fd(d), isSocket(sock) {}
    ~FdState();
    FdState(const FdState&) = delete;
    FdState& operator=(const FdState&) = delete;
};

// The state of `fd`, created on first use.
std::shared_ptr<FdState> fdState(int fd);

// Waits until `fd` is ready for `events` within `timeoutMs` (-1: no limit).
// False on timeout; true also on an error or hang-up, which the call that
// follows reports. On Windows the wait also ends once `cancelled` (the
// descriptor's `closed` flag) is set, because closing a socket there does
// not wake a thread waiting on it; POSIX ignores it.
bool waitReady(int fd, short events, int timeoutMs, const std::atomic<bool>* cancelled = nullptr);

// Reads more input into the buffer; false at the end of the stream. The
// caller holds readMutex.
bool fill(FdState& st);

// A line without its end (LF or CRLF), or false at the end of the stream. A
// line longer than `max` bytes (0: no limit) throws LineTooLong. The caller
// holds readMutex.
bool takeLine(FdState& st, std::size_t max, std::string& line);

// Up to `n` bytes from the buffer and the stream (fewer only at the end).
// The caller holds readMutex.
std::string takeBytes(FdState& st, std::size_t n);

// Writes all of `data`. The caller holds writeMutex.
void rawWrite(FdState& st, const char* data, std::size_t size);

// Runs `op` (an SSL call) once under sslMutex. Answers the call's result
// (> 0), 0 at a clean end of stream, or -1 with errno set, having waited in
// poll with no lock held whenever OpenSSL asked for more I/O.
int sslCall(FdState& st, int (*op)(SSL*, void*), void* arg);

// The OpenSSL client context: verifying peers or trusting them.
SSL_CTX* tlsContext(bool verify);

// The text of the first queued OpenSSL error.
std::string tlsErrorText();

} // namespace protoio::detail
