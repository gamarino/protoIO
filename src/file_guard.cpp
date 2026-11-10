// protoIO - shared input and output for the protoCore runtimes.
// Copyright (c) 2026 Gustavo Marino <gamarino@gmail.com>. MIT licensed.
//
// The end guard of protoio/file.h and the guarded file operations. Platform
// independent: they wrap the unguarded operations of file.cpp and
// file_win.cpp. Moved from protoST's BinaryFile (src/primitives/
// binfile_prims.cpp), so every runtime gets the same guard.
#include "protoio/file.h"

#include "protoio/error.h"

#include <string>

namespace protoio::file {

EndGuard::Claim::Claim(EndGuard& guard, Mode mode, std::string_view operation) : guard_(guard), mode_(mode) {
    int cur = guard_.state_.load(std::memory_order_relaxed);
    for (;;) {
        const bool available = mode_ == Mode::Exclusive ? cur == 0 : cur >= 0;
        if (!available) {
            std::string message(operation);
            message += mode_ == Mode::Exclusive
                           ? ": another operation that moves the end of the file is in progress "
                             "(append and truncate need it alone)"
                           : ": an append or a truncate is in progress on the file "
                             "(it needs the end of the file alone)";
            throw Error(Error::Kind::Conflict, message);
        }
        // On failure `cur` is reloaded and the state judged again.
        if (guard_.state_.compare_exchange_weak(cur, mode_ == Mode::Exclusive ? -1 : cur + 1,
                                                std::memory_order_acquire, std::memory_order_relaxed))
            return;
    }
}

EndGuard::Claim::~Claim() {
    if (mode_ == Mode::Exclusive) guard_.state_.store(0, std::memory_order_release);
    else guard_.state_.fetch_sub(1, std::memory_order_release);
}

std::int64_t appendData(int fd, EndGuard& guard, std::string_view data) {
    EndGuard::Claim claim(guard, EndGuard::Mode::Exclusive, "appendData");
    return appendData(fd, data);
}

void writeAt(int fd, EndGuard& guard, std::int64_t offset, std::string_view data) {
    if (offset < 0)
        throw Error(Error::Kind::InvalidArgument, "cannot write at: negative offset " + std::to_string(offset));
    // Read before any claim (see file.h): a write inside the file needs none.
    const std::int64_t end = size(fd);
    const bool extends =
        !data.empty() && (offset > end || static_cast<std::uint64_t>(end - offset) < data.size());
    if (!extends) {
        writeAt(fd, offset, data);
        return;
    }
    EndGuard::Claim claim(guard, EndGuard::Mode::Shared, "writeAt");
    writeAt(fd, offset, data);
}

void truncate(int fd, EndGuard& guard, std::int64_t newSize) {
    if (newSize < 0)
        throw Error(Error::Kind::InvalidArgument,
                    "cannot truncate descriptor " + std::to_string(fd) + ": negative size " + std::to_string(newSize));
    EndGuard::Claim claim(guard, EndGuard::Mode::Exclusive, "truncate");
    truncate(fd, newSize);
}

} // namespace protoio::file
