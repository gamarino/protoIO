// protoIO - shared input and output for the protoCore runtimes.
// Copyright (c) 2026 Gustavo Marino <gamarino@gmail.com>. MIT licensed.
#pragma once

#include <csignal>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

// Buffered streams over POSIX file descriptors (files, pipes, sockets, the
// standard streams, TLS sockets). On Windows a descriptor is a C runtime
// descriptor (files, pipes, the standard streams) or the number the library
// gives each socket it creates; see README.md, "Windows".
//
// Every function here may block. A runtime calls it inside its own "leave the
// collector's quorum" bracket; the library touches no runtime object.
//
// Threads: one descriptor may be used from several threads at once.
//   * Reads are serialised (the read-ahead buffer is consumed atomically:
//     each line goes to exactly one reader), and so are writes, independently
//     of each other: a thread waiting for input never blocks a writer.
//   * `close` wakes a thread blocked on a socket.
//   * The descriptor number is released only after its last user returns, so
//     a concurrent open() can never reuse it under a thread still waiting.
//
// The standard output and error (descriptors 1 and 2) are written through the
// C stdio streams, so the order with the runtime's own printing is kept.
namespace protoio {

// Blocks SIGPIPE on the calling thread for its lifetime and consumes a
// SIGPIPE the thread raised meanwhile, so a write to a pipe or socket whose
// reader is gone fails with EPIPE instead of ending the process. The library
// uses it on every write except to the process's own standard output, which
// keeps the default action (a filter piped into `head` stops, as every Unix
// filter does). Exposed for runtimes that write to descriptors themselves.
// Windows has no SIGPIPE (such a write simply fails): there it does nothing.
class SigpipeGuard {
public:
    SigpipeGuard();
    ~SigpipeGuard();
    SigpipeGuard(const SigpipeGuard&) = delete;
    SigpipeGuard& operator=(const SigpipeGuard&) = delete;

#ifndef _WIN32
private:
    sigset_t old_{};
    bool wasPending_ = false;
#endif
};

// A line without its end (LF or CRLF), or std::nullopt at the end of the
// stream. The last line may lack its LF. With `max` > 0, a line longer than
// `max` bytes throws LineTooLong.
std::optional<std::string> readLine(int fd, std::size_t max = 0);

// Everything up to the end of the stream (possibly empty).
std::string readAll(int fd);

// Up to `n` bytes: fewer only at the end of the stream; std::nullopt when the
// stream is at its end and `n` > 0.
std::optional<std::string> readBytes(int fd, std::size_t n);

// Up to `n` whole UTF-8 characters (a character split across two reads is
// never cut); std::nullopt when the stream is at its end and `n` > 0. At the
// end of the stream an incomplete trailing sequence is answered as it is.
std::optional<std::string> readChars(int fd, std::size_t n);

// True when no more input will come (the buffer is empty and a read answers
// end of stream). Blocks until input arrives or the stream ends.
bool atEnd(int fd);

// Writes all of `data`. Throws FileSystem (a file or pipe), Network or
// ConnectionTimedOut (a socket) on failure; never raises SIGPIPE.
void write(int fd, std::string_view data);

// Flushes the C stdio buffer of descriptors 1 and 2; a no-op for others
// (the library does not buffer output).
void flush(int fd);

// Closes the descriptor. Descriptors 0-2 are only flushed. A socket is shut
// down at once, waking any thread blocked on it; the number itself is
// released when its last user returns. A TLS session gets one close_notify
// attempt. Closing twice is harmless.
void close(int fd);

// Bounds every later wait on this descriptor (read, write, TLS handshake) to
// `ms` milliseconds; a wait that expires throws ConnectionTimedOut on a
// socket, FileSystem with ETIMEDOUT on a pipe or terminal (the console on
// Windows). -1 waits without limit (the default). On Windows a write to a
// pipe ignores it.
void setTimeout(int fd, int ms);

// Drops any state the library holds for a descriptor number that the caller
// closed by other means and the system may reuse. The library calls it for
// each descriptor it creates; a runtime needs it only for descriptors it
// opens itself (pipe(2), dup(2), ...).
void forget(int fd);

} // namespace protoio
