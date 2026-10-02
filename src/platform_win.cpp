// protoIO - shared input and output for the protoCore runtimes.
// Copyright (c) 2026 Gustavo Marino <gamarino@gmail.com>. MIT licensed.
//
// The Windows layer: Winsock start-up, the socket descriptor table, error
// code mapping, pipe waits and UTF-8 conversion. See platform_win.h.
#include "platform_win.h"

#include "protoio/error.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstring>
#include <mutex>
#include <system_error>
#include <unordered_map>

#include <io.h>
#include <stdlib.h>

namespace protoio::detail::win {

// -------------------------------------------------------------------- Winsock

void initWinsock() {
    static std::once_flag once;
    std::call_once(once, [] {
        WSADATA data;
        const int rc = ::WSAStartup(MAKEWORD(2, 2), &data);
        if (rc != 0) throw Error(Error::Kind::Network, "WSAStartup failed", errnoOfWsa(rc));
    });
}

// ------------------------------------------------------------- socket table

namespace {

std::mutex g_sockMutex;
std::unordered_map<int, SOCKET> g_socks;  // guarded by g_sockMutex
int g_nextSock = kSocketBase;             // guarded by g_sockMutex

} // namespace

int registerSocket(SOCKET s) {
    ::SetHandleInformation(reinterpret_cast<HANDLE>(s), HANDLE_FLAG_INHERIT, 0);
    std::lock_guard<std::mutex> lock(g_sockMutex);
    // Numbers are handed out in increasing order and reused only after a
    // wrap-around, so a stale number rarely names a new socket.
    for (;;) {
        const int fd = g_nextSock;
        g_nextSock = g_nextSock == INT_MAX ? kSocketBase : g_nextSock + 1;
        if (g_socks.emplace(fd, s).second) return fd;
    }
}

SOCKET socketOf(int fd) {
    if (fd < kSocketBase) return INVALID_SOCKET;
    std::lock_guard<std::mutex> lock(g_sockMutex);
    auto it = g_socks.find(fd);
    return it == g_socks.end() ? INVALID_SOCKET : it->second;
}

void closeDescriptor(int fd) {
    SOCKET s = INVALID_SOCKET;
    if (fd >= kSocketBase) {
        std::lock_guard<std::mutex> lock(g_sockMutex);
        auto it = g_socks.find(fd);
        if (it != g_socks.end()) {
            s = it->second;
            g_socks.erase(it);
        }
    }
    if (s != INVALID_SOCKET) {
        ::closesocket(s);
        return;
    }
    QuietCrt quiet;
    ::_close(fd);
}

void setNonBlocking(SOCKET s, bool on) {
    u_long mode = on ? 1 : 0;
    ::ioctlsocket(s, FIONBIO, &mode);
}

// ----------------------------------------------------------------- errors

int errnoOfWsa(int e) {
    switch (e) {
        case 0: return 0;
        case WSAEINTR: return EINTR;
        case WSAEBADF: return EBADF;
        case WSAEACCES: return EACCES;
        case WSAEFAULT: return EFAULT;
        case WSAEINVAL: return EINVAL;
        case WSAEMFILE: return EMFILE;
        case WSAEWOULDBLOCK: return EWOULDBLOCK;
        case WSAEINPROGRESS: return EINPROGRESS;
        case WSAEALREADY: return EALREADY;
        case WSAENOTSOCK: return ENOTSOCK;
        case WSAEDESTADDRREQ: return EDESTADDRREQ;
        case WSAEMSGSIZE: return EMSGSIZE;
        case WSAEPROTOTYPE: return EPROTOTYPE;
        case WSAENOPROTOOPT: return ENOPROTOOPT;
        case WSAEPROTONOSUPPORT: return EPROTONOSUPPORT;
        case WSAEOPNOTSUPP: return EOPNOTSUPP;
        case WSAEAFNOSUPPORT: return EAFNOSUPPORT;
        case WSAEADDRINUSE: return EADDRINUSE;
        case WSAEADDRNOTAVAIL: return EADDRNOTAVAIL;
        case WSAENETDOWN: return ENETDOWN;
        case WSAENETUNREACH: return ENETUNREACH;
        case WSAENETRESET: return ENETRESET;
        case WSAECONNABORTED: return ECONNABORTED;
        case WSAECONNRESET: return ECONNRESET;
        case WSAENOBUFS: return ENOBUFS;
        case WSAEISCONN: return EISCONN;
        case WSAENOTCONN: return ENOTCONN;
        case WSAESHUTDOWN: return EPIPE;  // a write after shutdown, as POSIX reports it
        case WSAETIMEDOUT: return ETIMEDOUT;
        case WSAECONNREFUSED: return ECONNREFUSED;
        case WSAELOOP: return ELOOP;
        case WSAENAMETOOLONG: return ENAMETOOLONG;
        case WSAEHOSTUNREACH: return EHOSTUNREACH;
        case WSAENOTEMPTY: return ENOTEMPTY;
        case WSANOTINITIALISED: return EINVAL;
        default: return EIO;
    }
}

int errnoOfWin32(unsigned long e) {
    switch (e) {
        case ERROR_SUCCESS: return 0;
        case ERROR_BROKEN_PIPE:
        case ERROR_NO_DATA: return EPIPE;
        case ERROR_DIRECTORY: return ENOTDIR;
        default: break;
    }
    const std::error_condition c = std::system_category().default_error_condition(static_cast<int>(e));
    return c.category() == std::generic_category() ? c.value() : EIO;
}

// --------------------------------------------------------- descriptor reads

namespace {

// A real handle to the calling thread (GetCurrentThread answers a pseudo
// handle, which means "the caller" to whichever thread uses it), for
// CancelSynchronousIo from the timer thread.
HANDLE currentThread() {
    struct Own {
        HANDLE h = nullptr;
        Own() {
            ::DuplicateHandle(::GetCurrentProcess(), ::GetCurrentThread(), ::GetCurrentProcess(), &h, 0, FALSE,
                              DUPLICATE_SAME_ACCESS);
        }
        ~Own() {
            if (h) ::CloseHandle(h);
        }
    };
    thread_local Own own;
    return own.h;
}

// While alive, cancels the calling thread's blocking I/O once `timeoutMs`
// have passed. The timer callback cancels again (every millisecond) until the
// I/O is over, which covers a deadline that falls just before the I/O call
// starts; the destructor stops the timer and waits for a running callback,
// so no later I/O of the thread can be cancelled by it.
class IoDeadline {
public:
    explicit IoDeadline(int timeoutMs) {
        if (timeoutMs < 0) return;
        shared_.thread = currentThread();
        if (!shared_.thread) return;
        timer_ = ::CreateThreadpoolTimer(&IoDeadline::onTimer, &shared_, nullptr);
        if (!timer_) return;
        // A relative due time: negative, in 100 ns units.
        ULARGE_INTEGER due;
        due.QuadPart = static_cast<ULONGLONG>(-(static_cast<LONGLONG>(timeoutMs) * 10000));
        FILETIME ft;
        ft.dwLowDateTime = due.LowPart;
        ft.dwHighDateTime = due.HighPart;
        ::SetThreadpoolTimer(timer_, &ft, 0, 0);
    }
    ~IoDeadline() {
        if (!timer_) return;
        shared_.done.store(true);
        ::SetThreadpoolTimer(timer_, nullptr, 0, 0);
        ::WaitForThreadpoolTimerCallbacks(timer_, TRUE);
        ::CloseThreadpoolTimer(timer_);
    }
    bool expired() const { return shared_.fired.load(); }
    IoDeadline(const IoDeadline&) = delete;
    IoDeadline& operator=(const IoDeadline&) = delete;

private:
    struct Shared {
        HANDLE thread = nullptr;
        std::atomic<bool> fired{false};
        std::atomic<bool> done{false};
    };

    static void CALLBACK onTimer(PTP_CALLBACK_INSTANCE, void* context, PTP_TIMER) {
        auto* shared = static_cast<Shared*>(context);
        shared->fired.store(true);
        while (!shared->done.load() && !::CancelSynchronousIo(shared->thread)) ::Sleep(1);
    }

    Shared shared_;
    PTP_TIMER timer_ = nullptr;
};

bool isHighSurrogate(wchar_t c) { return c >= 0xD800 && c <= 0xDBFF; }

} // namespace

ssize_t readDescriptor(int fd, char* out, std::size_t n, int timeoutMs) {
    HANDLE h;
    {
        QuietCrt quiet;
        h = reinterpret_cast<HANDLE>(::_get_osfhandle(fd));
    }
    if (h == INVALID_HANDLE_VALUE || h == nullptr) {
        errno = EBADF;
        return -1;
    }
    DWORD consoleMode = 0;
    const bool console = ::GetConsoleMode(h, &consoleMode) != 0;
    const DWORD type = ::GetFileType(h);
    const bool mayBlock = console || type == FILE_TYPE_PIPE || type == FILE_TYPE_CHAR;
    for (;;) {
        IoDeadline deadline(mayBlock ? timeoutMs : -1);
        BOOL ok;
        DWORD got = 0;
        std::wstring units;
        if (console) {
            // Three UTF-8 bytes at most per UTF-16 unit, and room for the
            // second half of a surrogate pair read below (the caller reads
            // in 64 KiB chunks).
            if (n < 16) {
                errno = EINVAL;
                return -1;
            }
            units.resize(std::min<std::size_t>((n - 4) / 3, 4096));
            ok = ::ReadConsoleW(h, units.data(), static_cast<DWORD>(units.size()), &got, nullptr);
            // A character outside the BMP may arrive in two reads: its second
            // half is already in the console's buffer.
            if (ok && got > 0 && isHighSurrogate(units[got - 1])) {
                units.resize(got + 1);
                DWORD more = 0;
                ok = ::ReadConsoleW(h, units.data() + got, 1, &more, nullptr);
                got += more;
            }
        } else {
            ok = ::ReadFile(h, out, static_cast<DWORD>(std::min<std::size_t>(n, 1u << 30)), &got, nullptr);
        }
        const DWORD err = ok ? ERROR_SUCCESS : ::GetLastError();
        const bool expired = deadline.expired();
        if (!ok) {
            if (err == ERROR_OPERATION_ABORTED) {
                if (expired) {
                    errno = ETIMEDOUT;
                    return -1;
                }
                continue;  // cancelled by someone else (Ctrl+C on a console): read again, as POSIX does after EINTR
            }
            if (err == ERROR_BROKEN_PIPE || err == ERROR_HANDLE_EOF) return 0;  // the writer is gone
            errno = errnoOfWin32(err);
            return -1;
        }
        if (!console) return static_cast<ssize_t>(got);
        if (got == 0) return 0;
        if (units[0] == 0x1A) return 0;  // Ctrl+Z at the start of a line: the end, as the C runtime reads it
        const std::string bytes = narrow(units.substr(0, got));
        std::memcpy(out, bytes.data(), bytes.size());
        return static_cast<ssize_t>(bytes.size());
    }
}

// ----------------------------------------------------------------- strings

std::wstring widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) throw Error(Error::Kind::InvalidArgument, "invalid UTF-8 text: " + s, EILSEQ);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return std::string();
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n > 0 ? n : 0), '\0');
    if (n > 0) ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

} // namespace protoio::detail::win
