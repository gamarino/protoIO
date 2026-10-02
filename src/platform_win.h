// protoIO - shared input and output for the protoCore runtimes.
// Copyright (c) 2026 Gustavo Marino <gamarino@gmail.com>. MIT licensed.
//
// Internal, Windows only: the Winsock headers and the small layer the other
// modules use where POSIX and Windows differ.
//
// Descriptors. A protoIO descriptor is an int, as on POSIX. Files and pipes
// are C runtime descriptors (_wopen, _pipe), so 0, 1 and 2 are the standard
// streams and the lowest free number is reused, as on POSIX. A SOCKET is not
// a C runtime descriptor (and _open_osfhandle on one would close it with
// CloseHandle instead of closesocket), so every socket the library creates is
// registered here under a number of its own, from kSocketBase up, a range the
// C runtime never hands out (it stops at 8192 descriptors).
#pragma once

#ifndef _WIN32
#error "platform_win.h is for Windows builds only"
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

#include <stdlib.h>

#ifndef _SSIZE_T_DEFINED
#define _SSIZE_T_DEFINED
typedef std::ptrdiff_t ssize_t;
#endif

namespace protoio::detail::win {

constexpr int kSocketBase = 1 << 16;

// The C runtime calls its invalid-parameter handler (which ends the process
// by default) for a descriptor that is not open, where POSIX answers EBADF.
// While one of these is alive, the C runtime calls on this thread answer
// EBADF too.
struct QuietCrt {
    _invalid_parameter_handler old;
    QuietCrt()
        : old(::_set_thread_local_invalid_parameter_handler(
              [](const wchar_t*, const wchar_t*, const wchar_t*, unsigned, std::uintptr_t) {})) {}
    ~QuietCrt() { ::_set_thread_local_invalid_parameter_handler(old); }
    QuietCrt(const QuietCrt&) = delete;
    QuietCrt& operator=(const QuietCrt&) = delete;
};

// WSAStartup, once per process. Every function that creates or resolves
// sockets calls it.
void initWinsock();

// The descriptor a new socket goes by. The socket is not inheritable.
int registerSocket(SOCKET s);

// The socket behind `fd`, or INVALID_SOCKET when `fd` is not a socket.
SOCKET socketOf(int fd);

// Closes a descriptor for good: closesocket (and the number is released) for
// a socket, _close for anything else.
void closeDescriptor(int fd);

// Switches a socket between blocking and non-blocking (FIONBIO).
void setNonBlocking(SOCKET s, bool on);

// The errno value matching a Winsock (WSAGetLastError) or Win32
// (GetLastError) error code, so the library reports one error model.
int errnoOfWsa(int wsaError);
int errnoOfWin32(unsigned long win32Error);

// Reads up to `n` bytes from a descriptor that is not a socket, as bytes,
// whatever the C runtime's mode for it (text mode would turn CR LF into LF
// and stop at a Ctrl+Z byte): ReadFile on its handle. A console is read with
// ReadConsoleW and answered as UTF-8; a line that starts with Ctrl+Z is the
// end of its input. Answers the count, 0 at the end of the stream, or -1 with
// errno set.
//
// With `timeoutMs` >= 0, a read from a pipe or a character device (the
// console) that is still blocked at the deadline is cancelled
// (CancelSynchronousIo, from a thread-pool timer) and fails with ETIMEDOUT:
// the thread waits in the read itself, with no polling. Reads from disk files
// never block, and ignore the timeout.
ssize_t readDescriptor(int fd, char* out, std::size_t n, int timeoutMs);

// UTF-8 <-> UTF-16, for every path, argument and environment string handed
// to the system.
std::wstring widen(const std::string& utf8);
std::string narrow(const std::wstring& utf16);

} // namespace protoio::detail::win
