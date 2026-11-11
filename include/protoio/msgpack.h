// protoIO - shared input and output for the protoCore runtimes.
// Copyright (c) 2026 Gustavo Marino <gamarino@gmail.com>. MIT licensed.
//
// MessagePack, byte-compatible with msgpack-python 1.x as ProtoBase uses it:
// packb(v, use_bin_type=True) and unpackb(raw=False).
//
// protoIO does not build runtime objects, so there is no value tree: a runtime
// walks its own objects into Writer calls, and builds its own objects from the
// Handler events that read() reports.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace protoio::msgpack {

// Appends MessagePack to an internal buffer, always in the smallest encoding,
// exactly as msgpack-python chooses it. The writer does not check structure:
// after beginArray(n) the caller writes n values, after beginMap(n) 2n values
// (key, value, key, value, ...).
class Writer {
public:
    void nil();
    void boolean(bool v);
    // A non-negative value uses the unsigned family (fixint, uint8..uint64),
    // a negative one the signed family (negative fixint, int8..int64).
    void integer(int64_t v);
    void unsignedInteger(uint64_t v);
    // Always float64 (0xcb), as msgpack-python does; NaN payloads and -0.0 are
    // kept bit-exactly.
    void float64(double v);
    // Throws Error(InvalidArgument) if `utf8` is not strictly valid UTF-8
    // (overlong forms, surrogates and code points above U+10FFFF are refused)
    // or is longer than 2^32 - 1 bytes.
    void str(std::string_view utf8);
    // bin 8/16/32 (use_bin_type=True). InvalidArgument above 2^32 - 1 bytes.
    void bin(std::string_view bytes);
    void beginArray(uint32_t n);
    void beginMap(uint32_t n);
    // fixext 1/2/4/8/16 for those sizes, otherwise ext 8/16/32.
    // InvalidArgument above 2^32 - 1 bytes.
    void ext(int8_t type, std::string_view data);

    const std::string& bytes() const { return out_; }
    // Hands the buffer over and leaves the writer empty.
    std::string take();

private:
    std::string out_;
};

// Reader callbacks; the runtime implements them. The string_views point into
// the input given to read() and are valid only as long as it is.
struct Handler {
    virtual void nil() = 0;
    virtual void boolean(bool v) = 0;
    virtual void integer(int64_t v) = 0;
    // Called only for values above INT64_MAX; every other integer, signed or
    // unsigned on the wire, is reported through integer().
    virtual void unsignedInteger(uint64_t v) = 0;
    // float32 on the wire is widened to double.
    virtual void float64(double v) = 0;
    // Already validated as UTF-8 (unpackb(raw=False) refuses invalid UTF-8).
    virtual void str(std::string_view utf8) = 0;
    virtual void bin(std::string_view bytes) = 0;
    // Followed by n values (array) or n key/value pairs (map).
    virtual void beginArray(uint32_t n) = 0;
    virtual void beginMap(uint32_t n) = 0;
    virtual void ext(int8_t type, std::string_view data) = 0;
    virtual ~Handler() = default;
};

// Bounds on what read() accepts, so hostile input from disk or the network
// can neither exhaust memory nor the stack.
struct Limits {
    // Maximum nesting of arrays and maps (the top-level container is depth 1).
    // 1024 is msgpack-python 1.2's unpacker limit, and the record depth
    // ProtoBase format 4 requires every reader to accept (rc2, D60: 1022 for
    // a value plus 2 for the record envelope).
    uint32_t maxDepth = 1024;
    // Maximum total of array elements plus map entries declared across the
    // whole document (not per container).
    uint64_t maxElements = 1u << 26;
    // Accept only str and bin map keys, like msgpack-python 1.x's default
    // strict_map_key=True.
    bool strictMapKeys = true;
};

// Decodes exactly one top-level object, reporting it to `h`, and answers the
// number of bytes consumed (trailing bytes are left to the caller). Throws
// Error(InvalidArgument) on truncated or malformed input, on invalid UTF-8 in
// a str, on a non-str/bin map key under strictMapKeys, and when a limit is
// exceeded. Every declared length is checked against the remaining input
// before its event is reported. Exceptions thrown by `h` propagate.
size_t read(std::string_view data, Handler& h, const Limits& limits = {});

} // namespace protoio::msgpack
