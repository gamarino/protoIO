// protoIO - shared input and output for the protoCore runtimes.
// Copyright (c) 2026 Gustavo Marino <gamarino@gmail.com>. MIT licensed.
//
// Fixed-width integers, IEEE 754 floats and base64, byte-compatible with
// CPython's struct and base64 modules.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace protoio::encoding {

// Every function throws Error(Kind::InvalidArgument) for a bad width, a value
// that does not fit the width, or a read past the end of `data`.

// Append `v` to `out` in `width` (1, 2, 4 or 8) bytes.
void putUint(std::string& out, uint64_t v, int width, bool bigEndian);
void putInt(std::string& out, int64_t v, int width, bool bigEndian);
// NaN payloads and signed zero are preserved bit-exactly.
void putFloat64(std::string& out, double v, bool bigEndian);
void putFloat32(std::string& out, float v, bool bigEndian);

// Read `width` bytes of `data` starting at `offset`.
uint64_t getUint(std::string_view data, size_t offset, int width, bool bigEndian);
int64_t getInt(std::string_view data, size_t offset, int width, bool bigEndian);
double getFloat64(std::string_view data, size_t offset, bool bigEndian);
float getFloat32(std::string_view data, size_t offset, bool bigEndian);

// RFC 4648, standard alphabet, padded, no line breaks.
std::string base64Encode(std::string_view data);
// Strict: padding required, no whitespace, no stray characters. It departs
// from CPython's b64decode in one more way: text whose last character carries
// non-zero bits that the padding drops ("ab==", "abd=") is not canonical and
// throws InvalidArgument, where CPython ignores those bits.
std::string base64Decode(std::string_view text);

} // namespace protoio::encoding
