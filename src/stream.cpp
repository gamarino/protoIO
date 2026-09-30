// protoIO - shared input and output for the protoCore runtimes.
// Copyright (c) 2026 Gustavo Marino <gamarino@gmail.com>. MIT licensed.
//
// Buffered streams over descriptors. Moved from protoST's
// src/primitives/io_prims.cpp together with the review hardening of
// 2026-09-29: per-descriptor read/write/TLS locks, a state that owns its
// descriptor, and writes that never raise SIGPIPE.
#include "protoio/stream.h"

#include "internal.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <unordered_map>

#include <pthread.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/ssl.h>

namespace protoio {

// ------------------------------------------------------------------ SIGPIPE
//
// A write to a pipe or socket whose reader is gone raises SIGPIPE, which kills
// the process. The program's own standard output keeps that behaviour (a
// filter piped into `head` should stop, as every Unix filter does); every
// other write -- a child's input, a file that is a FIFO, a TLS socket, which
// OpenSSL writes with write(2) -- blocks the signal on the writing thread
// instead, so the write answers EPIPE and becomes a catchable error.

SigpipeGuard::SigpipeGuard() {
    sigset_t block, pending;
    sigemptyset(&block);
    sigaddset(&block, SIGPIPE);
    sigpending(&pending);
    wasPending_ = sigismember(&pending, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &block, &old_);
}

SigpipeGuard::~SigpipeGuard() {
    if (!wasPending_) {
        // Consume a SIGPIPE this thread raised while it was blocked.
        sigset_t pending, only;
        sigpending(&pending);
        if (sigismember(&pending, SIGPIPE)) {
            sigemptyset(&only);
            sigaddset(&only, SIGPIPE);
            const timespec zero{0, 0};
            while (sigtimedwait(&only, nullptr, &zero) < 0 && errno == EINTR) {}
        }
    }
    pthread_sigmask(SIG_SETMASK, &old_, nullptr);
}

namespace detail {

// --------------------------------------------------------------- registry

namespace {
std::mutex g_fdMutex;
std::unordered_map<int, std::shared_ptr<FdState>> g_fds;
} // namespace

FdState::~FdState() {
    if (SSL* s = ssl.load()) SSL_free(s);
    if (closed.load()) ::close(fd);
}

std::shared_ptr<FdState> fdState(int fd) {
    std::lock_guard<std::mutex> lock(g_fdMutex);
    auto& p = g_fds[fd];
    if (!p) {
        struct stat st{};
        p = std::make_shared<FdState>(fd, ::fstat(fd, &st) == 0 && S_ISSOCK(st.st_mode));
    }
    return p;
}

// ------------------------------------------------------------------- waits

bool waitReady(int fd, short events, int timeoutMs) {
    pollfd p{fd, events, 0};
    for (;;) {
        const int r = ::poll(&p, 1, timeoutMs);
        if (r < 0 && errno == EINTR) continue;
        return r != 0;
    }
}

int sslCall(FdState& st, int (*op)(SSL*, void*), void* arg) {
    for (;;) {
        SSL* ssl = st.ssl.load();
        if (!ssl || st.closed.load()) { errno = EBADF; return -1; }
        int r, e;
        int savedErrno;
        {
            std::lock_guard<std::mutex> lock(st.sslMutex);
            ERR_clear_error();
            errno = 0;
            r = op(ssl, arg);
            savedErrno = errno;
            e = r > 0 ? SSL_ERROR_NONE : SSL_get_error(ssl, r);
        }
        if (r > 0) return r;
        if (e == SSL_ERROR_ZERO_RETURN) return 0;
        if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) {
            if (!waitReady(st.fd, e == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT, st.timeoutMs.load())) {
                errno = ETIMEDOUT;
                return -1;
            }
            continue;
        }
        if (e == SSL_ERROR_SYSCALL && r == 0) return 0;  // peer closed without close_notify
        errno = (e == SSL_ERROR_SYSCALL && savedErrno != 0) ? savedErrno : EIO;
        return -1;
    }
}

// ------------------------------------------------------------ read / write

namespace {

// Reads up to `n` bytes into `out`; answers the count, 0 at the end of the
// stream, -1 with errno set. The caller holds readMutex.
ssize_t rawRead(FdState& st, char* out, std::size_t n) {
    if (st.ssl.load()) {
        struct Buf { char* p; int n; } b{out, static_cast<int>(n)};
        return sslCall(st, [](SSL* s, void* a) {
            auto* b = static_cast<Buf*>(a);
            return SSL_read(s, b->p, b->n);
        }, &b);
    }
    if (!waitReady(st.fd, POLLIN, st.timeoutMs.load())) { errno = ETIMEDOUT; return -1; }
    for (;;) {
        const ssize_t r = ::read(st.fd, out, n);
        if (r < 0 && errno == EINTR) continue;
        return r;
    }
}

[[noreturn]] void lineTooLong(std::size_t max) {
    throw Error(Error::Kind::LineTooLong, "a line longer than " + std::to_string(max) + " bytes");
}

} // namespace

void rawWrite(FdState& st, const char* data, std::size_t size) {
    SigpipeGuard noSigpipe;
    std::size_t done = 0;
    while (done < size) {
        ssize_t w;
        if (st.ssl.load()) {
            struct Buf { const char* p; int n; } b{data + done, static_cast<int>(std::min<std::size_t>(size - done, 1 << 30))};
            const int r = sslCall(st, [](SSL* s, void* a) {
                auto* b = static_cast<Buf*>(a);
                return SSL_write(s, b->p, b->n);
            }, &b);
            if (r <= 0) netError(r == 0 ? EPIPE : errno, "TLS write");
            w = r;
        } else {
            if (!waitReady(st.fd, POLLOUT, st.timeoutMs.load())) netError(ETIMEDOUT, "write");
            w = st.isSocket ? ::send(st.fd, data + done, size - done, MSG_NOSIGNAL)
                            : ::write(st.fd, data + done, size - done);
            if (w < 0) {
                if (errno == EINTR) continue;
                if (st.isSocket) netError(errno, "write");
                fileError("descriptor " + std::to_string(st.fd), errno, "cannot write to");
            }
        }
        done += static_cast<std::size_t>(w);
    }
}

bool fill(FdState& st) {
    if (st.eof) return false;
    char chunk[65536];
    const ssize_t r = rawRead(st, chunk, sizeof chunk);
    if (r < 0) {
        if (st.isSocket || st.ssl.load()) netError(errno, "read");
        fileError("descriptor " + std::to_string(st.fd), errno, "cannot read from");
    }
    if (r == 0) { st.eof = true; return false; }
    st.buf.append(chunk, static_cast<std::size_t>(r));
    return true;
}

bool takeLine(FdState& st, std::size_t max, std::string& line) {
    std::size_t pos, scanned = 0;
    for (;;) {
        pos = st.buf.find('\n', scanned);
        if (pos != std::string::npos) break;
        scanned = st.buf.size();
        // A line of `max` bytes may still be followed by CR LF, so only more
        // than max + 1 bytes without a LF prove the line too long. The stream
        // is left where it was.
        if (max && scanned > max + 1) lineTooLong(max);
        if (!fill(st)) break;
    }
    const bool complete = pos != std::string::npos;
    std::size_t len = complete ? pos : st.buf.size();
    if (!complete && len == 0) return false;
    if (len > 0 && st.buf[len - 1] == '\r') --len;
    if (max && len > max) lineTooLong(max);
    line.assign(st.buf, 0, len);
    st.buf.erase(0, complete ? pos + 1 : st.buf.size());
    return true;
}

std::string takeBytes(FdState& st, std::size_t n) {
    while (st.buf.size() < n && fill(st)) {}
    const std::size_t take = std::min(st.buf.size(), n);
    std::string got = st.buf.substr(0, take);
    st.buf.erase(0, take);
    return got;
}

} // namespace detail

using detail::fdState;

// ---------------------------------------------------------------- public API

std::optional<std::string> readLine(int fd, std::size_t max) {
    auto st = fdState(fd);
    std::lock_guard<std::mutex> lock(st->readMutex);
    std::string line;
    if (!detail::takeLine(*st, max, line)) return std::nullopt;
    return line;
}

std::string readAll(int fd) {
    auto st = fdState(fd);
    std::lock_guard<std::mutex> lock(st->readMutex);
    while (detail::fill(*st)) {}
    std::string all;
    all.swap(st->buf);
    return all;
}

std::optional<std::string> readBytes(int fd, std::size_t n) {
    auto st = fdState(fd);
    std::lock_guard<std::mutex> lock(st->readMutex);
    std::string got = detail::takeBytes(*st, n);
    if (got.empty() && n > 0) return std::nullopt;
    return got;
}

std::optional<std::string> readChars(int fd, std::size_t n) {
    auto st = fdState(fd);
    std::lock_guard<std::mutex> lock(st->readMutex);
    // The byte length of the first `n` characters, or npos when the buffer
    // does not hold them all yet (a lead byte is any byte that is not
    // 10xxxxxx; its sequence length comes from its high bits).
    auto prefix = [n](const std::string& b) -> std::size_t {
        std::size_t i = 0;
        for (std::size_t c = 0; c < n; ++c) {
            if (i >= b.size()) return std::string::npos;
            const unsigned char lead = static_cast<unsigned char>(b[i]);
            const std::size_t len = lead < 0x80 ? 1 : (lead >> 5) == 0x6 ? 2 : (lead >> 4) == 0xE ? 3
                                  : (lead >> 3) == 0x1E ? 4 : 1;
            if (i + len > b.size()) return std::string::npos;
            i += len;
        }
        return i;
    };
    std::size_t take;
    while ((take = prefix(st->buf)) == std::string::npos && detail::fill(*st)) {}
    if (take == std::string::npos) take = st->buf.size();  // end of stream: what is left
    std::string got = st->buf.substr(0, take);
    st->buf.erase(0, take);
    if (got.empty() && n > 0) return std::nullopt;
    return got;
}

bool atEnd(int fd) {
    auto st = fdState(fd);
    std::lock_guard<std::mutex> lock(st->readMutex);
    return st->buf.empty() && !detail::fill(*st);
}

void write(int fd, std::string_view data) {
    if (fd == 1 || fd == 2) {
        // Through the C stream, so the order with the runtime's own printing
        // (which goes through stdout) is kept.
        std::FILE* f = fd == 1 ? stdout : stderr;
        std::fwrite(data.data(), 1, data.size(), f);
        if (fd == 2 || (!data.empty() && data.back() == '\n')) std::fflush(f);
        return;
    }
    auto st = fdState(fd);
    std::lock_guard<std::mutex> lock(st->writeMutex);
    detail::rawWrite(*st, data.data(), data.size());
}

void flush(int fd) {
    if (fd == 1) std::fflush(stdout);
    if (fd == 2) std::fflush(stderr);
}

void close(int fd) {
    if (fd <= 2) { std::fflush(nullptr); return; }
    std::shared_ptr<detail::FdState> st;
    {
        std::lock_guard<std::mutex> lock(detail::g_fdMutex);
        auto it = detail::g_fds.find(fd);
        if (it != detail::g_fds.end()) { st = it->second; detail::g_fds.erase(it); }
    }
    if (!st) {
        // Never read from or written to through a state: nobody else holds it.
        struct stat sst{};
        if (::fstat(fd, &sst) == 0 && S_ISSOCK(sst.st_mode)) ::shutdown(fd, SHUT_RDWR);
        ::close(fd);
        return;
    }
    if (st->closed.exchange(true)) return;
    if (SSL* ssl = st->ssl.load()) {
        // One attempt at close_notify; the socket is non-blocking.
        SigpipeGuard noSigpipe;
        std::lock_guard<std::mutex> lock(st->sslMutex);
        SSL_shutdown(ssl);
    }
    // shutdown wakes a thread blocked in accept, poll or read on this socket,
    // which a bare close does not. The descriptor is closed by the state's
    // destructor, when its last user lets it go.
    if (st->isSocket) ::shutdown(fd, SHUT_RDWR);
}

void setTimeout(int fd, int ms) {
    fdState(fd)->timeoutMs.store(ms < 0 ? -1 : ms);
}

void forget(int fd) {
    std::lock_guard<std::mutex> lock(detail::g_fdMutex);
    detail::g_fds.erase(fd);
}

} // namespace protoio
