// protoIO - shared input and output for the protoCore runtimes.
// Copyright (c) 2026 Gustavo Marino <gamarino@gmail.com>. MIT licensed.
#include "internal.h"

#include <cerrno>
#include <cstring>

namespace protoio {

const char* kindName(Error::Kind kind) noexcept {
    switch (kind) {
        case Error::Kind::FileNotFound: return "FileNotFound";
        case Error::Kind::FileExists: return "FileExists";
        case Error::Kind::FileSystem: return "FileSystem";
        case Error::Kind::ConnectionRefused: return "ConnectionRefused";
        case Error::Kind::ConnectionTimedOut: return "ConnectionTimedOut";
        case Error::Kind::NameLookup: return "NameLookup";
        case Error::Kind::Network: return "Network";
        case Error::Kind::Process: return "Process";
        case Error::Kind::LineTooLong: return "LineTooLong";
        case Error::Kind::BodyTooLarge: return "BodyTooLarge";
        case Error::Kind::InvalidArgument: return "InvalidArgument";
    }
    return "Unknown";
}

namespace detail {

std::string errorText(int err) {
#ifdef _WIN32
    // The C runtime knows no text for the POSIX network codes it defines.
    switch (err) {
        case EADDRINUSE: return "Address already in use";
        case EADDRNOTAVAIL: return "Cannot assign requested address";
        case EAFNOSUPPORT: return "Address family not supported by protocol";
        case EALREADY: return "Operation already in progress";
        case ECONNABORTED: return "Software caused connection abort";
        case ECONNREFUSED: return "Connection refused";
        case ECONNRESET: return "Connection reset by peer";
        case EHOSTUNREACH: return "No route to host";
        case EINPROGRESS: return "Operation now in progress";
        case EISCONN: return "Transport endpoint is already connected";
        case EMSGSIZE: return "Message too long";
        case ENETDOWN: return "Network is down";
        case ENETRESET: return "Network dropped connection on reset";
        case ENETUNREACH: return "Network is unreachable";
        case ENOBUFS: return "No buffer space available";
        case ENOTCONN: return "Transport endpoint is not connected";
        case ENOTSOCK: return "Socket operation on non-socket";
        case EOPNOTSUPP: return "Operation not supported";
        case ETIMEDOUT: return "Connection timed out";
        case EWOULDBLOCK: return "Operation would block";
        default: break;
    }
#endif
    return std::strerror(err);
}

void fileError(const std::string& path, int err, const char* action) {
    const std::string msg = std::string(action) + " " + path + ": " + errorText(err);
    if (err == ENOENT) throw Error(Error::Kind::FileNotFound, msg, err);
    if (err == EEXIST) throw Error(Error::Kind::FileExists, msg, err);
    throw Error(Error::Kind::FileSystem, msg, err);
}

void netError(int err, const std::string& what) {
    const std::string msg = what + ": " + errorText(err);
    if (err == ECONNREFUSED) throw Error(Error::Kind::ConnectionRefused, msg, err);
    if (err == ETIMEDOUT || err == EAGAIN || err == EWOULDBLOCK)
        throw Error(Error::Kind::ConnectionTimedOut, msg, err);
    throw Error(Error::Kind::Network, msg, err);
}

} // namespace detail
} // namespace protoio
