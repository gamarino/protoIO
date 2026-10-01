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
