// protoIO - shared input and output for the protoCore runtimes.
// Copyright (c) 2026 Gustavo Marino <gamarino@gmail.com>. MIT licensed.
//
// The end guard of protoio/file.h and the guarded file operations. Platform
// independent: they wrap the unguarded operations of file.cpp and
// file_win.cpp. Grown from protoST's BinaryFile end guard
// (src/primitives/binfile_prims.cpp), so every runtime gets the same guard.
#include "protoio/file.h"

#include "protoio/error.h"

#include <string>

namespace protoio::file {

namespace {

// The layout of the guard's word: two flags and two 31-bit counts. Every
// claim adds its unit and its release subtracts it, so a flag is a count
// that the conflict rules keep at 0 or 1.
constexpr std::uint64_t kAppend = 1;
constexpr std::uint64_t kTruncate = 2;
constexpr int kExtendShift = 2;
constexpr int kInsideShift = 33;
constexpr std::uint64_t kCountMask = (std::uint64_t{1} << 31) - 1;
constexpr std::uint64_t kExtendUnit = std::uint64_t{1} << kExtendShift;
constexpr std::uint64_t kInsideUnit = std::uint64_t{1} << kInsideShift;
constexpr std::uint64_t kExtendMask = kCountMask << kExtendShift;
// Two 31-bit counts fill the word exactly: Extend in bits 2..32, Inside in
// 33..63. A count would overflow (Extend into Inside, Inside out of the
// word) only with 2^31 claims held at once; a Claim is non-movable RAII,
// each held by its own call frame or object, so that is not reachable.
static_assert(kExtendShift + 31 == kInsideShift && kInsideShift + 31 == 64, "the guard's fields fill the word");

std::uint64_t unitOf(EndGuard::Use use) {
    switch (use) {
        case EndGuard::Use::Append: return kAppend;
        case EndGuard::Use::Truncate: return kTruncate;
        case EndGuard::Use::Extend: return kExtendUnit;
        case EndGuard::Use::Inside: return kInsideUnit;
    }
    return kTruncate;  // unreachable; the strictest use
}

// Whether `use` may be claimed in state `word` (the matrix in file.h).
bool available(EndGuard::Use use, std::uint64_t word) {
    switch (use) {
        case EndGuard::Use::Append: return (word & (kAppend | kTruncate | kExtendMask)) == 0;
        case EndGuard::Use::Truncate: return word == 0;
        case EndGuard::Use::Extend: return (word & (kAppend | kTruncate)) == 0;
        case EndGuard::Use::Inside: return (word & kTruncate) == 0;
    }
    return false;
}

const char* refusalOf(EndGuard::Use use) {
    switch (use) {
        case EndGuard::Use::Append:
            return ": another operation that moves the end of the file is in progress "
                   "(append and truncate need it alone)";
        case EndGuard::Use::Truncate:
            return ": another write, append or truncate is in progress on the file (truncate needs the file alone)";
        case EndGuard::Use::Extend:
            return ": an append or a truncate is in progress on the file (it needs the end of the file alone)";
        case EndGuard::Use::Inside:
            return ": a truncate is in progress on the file (it needs the file alone)";
    }
    return ": conflict";
}

} // namespace

EndGuard::Claim::Claim(EndGuard& guard, Use use, std::string_view operation) : guard_(guard), unit_(unitOf(use)) {
    std::uint64_t cur = guard_.word_.load(std::memory_order_relaxed);
    for (;;) {
        if (!available(use, cur)) throw Error(Error::Kind::Conflict, std::string(operation) + refusalOf(use));
        // On failure `cur` is reloaded and the state judged again.
        if (guard_.word_.compare_exchange_weak(cur, cur + unit_, std::memory_order_acquire, std::memory_order_relaxed))
            return;
    }
}

EndGuard::Claim::~Claim() { guard_.word_.fetch_sub(unit_, std::memory_order_release); }

EndGuard::State EndGuard::state() const noexcept {
    const std::uint64_t w = word_.load(std::memory_order_acquire);
    State s;
    s.append = (w & kAppend) != 0;
    s.truncate = (w & kTruncate) != 0;
    s.extending = static_cast<std::uint32_t>((w >> kExtendShift) & kCountMask);
    s.inside = static_cast<std::uint32_t>((w >> kInsideShift) & kCountMask);
    return s;
}

std::int64_t appendData(int fd, EndGuard& guard, std::string_view data) {
    EndGuard::Claim claim(guard, EndGuard::Use::Append, "appendData");
    return appendData(fd, data);
}

void writeAt(int fd, EndGuard& guard, std::int64_t offset, std::string_view data) {
    if (offset < 0)
        throw Error(Error::Kind::InvalidArgument, "cannot write at: negative offset " + std::to_string(offset));
    if (data.empty()) {
        writeAt(fd, offset, data);  // writes nothing; still refuses an append descriptor
        return;
    }
    EndGuard::Claim inside(guard, EndGuard::Use::Inside, "writeAt");
    // Read under the Inside claim: no guarded truncate can run, so the size
    // can only grow, and a write judged inside stays inside.
    const std::int64_t end = size(fd);
    const bool extends = offset > end || static_cast<std::uint64_t>(end - offset) < data.size();
    if (!extends) {
        writeAt(fd, offset, data);
        return;
    }
    EndGuard::Claim extend(guard, EndGuard::Use::Extend, "writeAt");
    writeAt(fd, offset, data);
}

void truncate(int fd, EndGuard& guard, std::int64_t newSize) {
    if (newSize < 0)
        throw Error(Error::Kind::InvalidArgument,
                    "cannot truncate descriptor " + std::to_string(fd) + ": negative size " + std::to_string(newSize));
    EndGuard::Claim claim(guard, EndGuard::Use::Truncate, "truncate");
    truncate(fd, newSize);
}

} // namespace protoio::file
