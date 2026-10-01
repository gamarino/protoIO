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

// ------------------------------------------------------------------- pipes

bool waitPipe(int fd, short events, int timeoutMs, const std::atomic<bool>* cancelled) {
    if (!(events & POLLIN) || timeoutMs < 0) return true;
    intptr_t h;
    {
        QuietCrt quiet;
        h = ::_get_osfhandle(fd);
    }
    if (h == -1 || ::GetFileType(reinterpret_cast<HANDLE>(h)) != FILE_TYPE_PIPE) return true;
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        DWORD avail = 0;
        // A failure (the writer is gone) is answered as ready: the read that
        // follows reports the end of the stream.
        if (!::PeekNamedPipe(reinterpret_cast<HANDLE>(h), nullptr, 0, nullptr, &avail, nullptr) || avail > 0)
            return true;
        if (cancelled && cancelled->load()) return true;
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(end - std::chrono::steady_clock::now());
        if (left.count() <= 0) return false;
        ::Sleep(static_cast<DWORD>(std::min<long long>(left.count(), 5)));
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
