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

void fileError(const std::string& path, int err, const char* action) {
    const std::string msg = std::string(action) + " " + path + ": " + std::strerror(err);
    if (err == ENOENT) throw Error(Error::Kind::FileNotFound, msg, err);
    if (err == EEXIST) throw Error(Error::Kind::FileExists, msg, err);
    throw Error(Error::Kind::FileSystem, msg, err);
}

void netError(int err, const std::string& what) {
    const std::string msg = what + ": " + std::strerror(err);
    if (err == ECONNREFUSED) throw Error(Error::Kind::ConnectionRefused, msg, err);
    if (err == ETIMEDOUT || err == EAGAIN || err == EWOULDBLOCK)
        throw Error(Error::Kind::ConnectionTimedOut, msg, err);
    throw Error(Error::Kind::Network, msg, err);
}

} // namespace detail
} // namespace protoio
