// protoIO - shared input and output for the protoCore runtimes.
// Copyright (c) 2026 Gustavo Marino <gamarino@gmail.com>. MIT licensed.
#include "protoio/encoding.h"

#include "protoio/error.h"

#include <bit>

namespace protoio::encoding {

namespace {

[[noreturn]] void bad(const std::string& m) { throw Error(Error::Kind::InvalidArgument, m); }

void checkWidth(int width) {
    if (width != 1 && width != 2 && width != 4 && width != 8)
        bad("width must be 1, 2, 4 or 8, not " + std::to_string(width));
}

void emit(std::string& out, uint64_t bits, int width, bool bigEndian) {
    for (int i = 0; i < width; ++i) {
        int shift = 8 * (bigEndian ? width - 1 - i : i);
        out += char(static_cast<unsigned char>((bits >> shift) & 0xFF));
    }
}

uint64_t fetch(std::string_view data, size_t offset, int width, bool bigEndian) {
    checkWidth(width);
    if (offset > data.size() || data.size() - offset < size_t(width))
        bad("read of " + std::to_string(width) + " bytes at offset " + std::to_string(offset) +
            " is out of bounds (size " + std::to_string(data.size()) + ")");
    uint64_t v = 0;
    for (int i = 0; i < width; ++i) {
        uint64_t b = static_cast<unsigned char>(data[offset + size_t(i)]);
        int shift = 8 * (bigEndian ? width - 1 - i : i);
        v |= b << shift;
    }
    return v;
}

} // namespace

void putUint(std::string& out, uint64_t v, int width, bool bigEndian) {
    checkWidth(width);
    if (width < 8 && (v >> (8 * width)) != 0)
        bad("value " + std::to_string(v) + " does not fit in " + std::to_string(width) +
            " unsigned byte(s)");
    emit(out, v, width, bigEndian);
}

void putInt(std::string& out, int64_t v, int width, bool bigEndian) {
    checkWidth(width);
    if (width < 8) {
        int64_t hi = (int64_t(1) << (8 * width - 1)) - 1;
        int64_t lo = -hi - 1;
        if (v < lo || v > hi)
            bad("value " + std::to_string(v) + " does not fit in " + std::to_string(width) +
                " signed byte(s)");
    }
    emit(out, static_cast<uint64_t>(v), width, bigEndian);  // two's complement
}

void putFloat64(std::string& out, double v, bool bigEndian) {
    emit(out, std::bit_cast<uint64_t>(v), 8, bigEndian);
}

void putFloat32(std::string& out, float v, bool bigEndian) {
    emit(out, std::bit_cast<uint32_t>(v), 4, bigEndian);
}

uint64_t getUint(std::string_view data, size_t offset, int width, bool bigEndian) {
    return fetch(data, offset, width, bigEndian);
}

int64_t getInt(std::string_view data, size_t offset, int width, bool bigEndian) {
    uint64_t u = fetch(data, offset, width, bigEndian);
    if (width < 8) {
        uint64_t sign = uint64_t(1) << (8 * width - 1);
        if (u & sign) u |= ~uint64_t(0) << (8 * width);  // sign-extend
    }
    return static_cast<int64_t>(u);
}

double getFloat64(std::string_view data, size_t offset, bool bigEndian) {
    return std::bit_cast<double>(fetch(data, offset, 8, bigEndian));
}

float getFloat32(std::string_view data, size_t offset, bool bigEndian) {
    return std::bit_cast<float>(static_cast<uint32_t>(fetch(data, offset, 4, bigEndian)));
}

namespace {
constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int decodeChar(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}
} // namespace

std::string base64Encode(std::string_view data) {
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 3 <= data.size(); i += 3) {
        uint32_t n = (uint32_t(uint8_t(data[i])) << 16) | (uint32_t(uint8_t(data[i + 1])) << 8) |
                     uint32_t(uint8_t(data[i + 2]));
        out += kAlphabet[(n >> 18) & 63];
        out += kAlphabet[(n >> 12) & 63];
        out += kAlphabet[(n >> 6) & 63];
        out += kAlphabet[n & 63];
    }
    size_t rest = data.size() - i;
    if (rest == 1) {
        uint32_t n = uint32_t(uint8_t(data[i])) << 16;
        out += kAlphabet[(n >> 18) & 63];
        out += kAlphabet[(n >> 12) & 63];
        out += "==";
    } else if (rest == 2) {
        uint32_t n = (uint32_t(uint8_t(data[i])) << 16) | (uint32_t(uint8_t(data[i + 1])) << 8);
        out += kAlphabet[(n >> 18) & 63];
        out += kAlphabet[(n >> 12) & 63];
        out += kAlphabet[(n >> 6) & 63];
        out += '=';
    }
    return out;
}

std::string base64Decode(std::string_view text) {
    if (text.size() % 4 != 0) bad("base64: length is not a multiple of 4");
    std::string out;
    out.reserve(text.size() / 4 * 3);
    for (size_t i = 0; i < text.size(); i += 4) {
        bool last = i + 4 == text.size();
        int v[4];
        int pad = 0;
        for (int j = 0; j < 4; ++j) {
            char c = text[i + size_t(j)];
            if (c == '=') {
                // Padding only in the last quantum, positions 2 and 3.
                if (!last || j < 2) bad("base64: misplaced padding");
                ++pad;
                v[j] = 0;
            } else {
                if (pad) bad("base64: data after padding");
                v[j] = decodeChar(c);
                if (v[j] < 0) bad("base64: invalid character");
            }
        }
        // Canonical form: unused low bits must be zero (as CPython validate=True accepts
        // them leniently, but a strict decoder rejects non-canonical text).
        if (pad == 2 && (v[1] & 0x0F)) bad("base64: non-canonical trailing bits");
        if (pad == 1 && (v[2] & 0x03)) bad("base64: non-canonical trailing bits");
        uint32_t n = (uint32_t(v[0]) << 18) | (uint32_t(v[1]) << 12) | (uint32_t(v[2]) << 6) |
                     uint32_t(v[3]);
        out += char((n >> 16) & 0xFF);
        if (pad < 2) out += char((n >> 8) & 0xFF);
        if (pad < 1) out += char(n & 0xFF);
    }
    return out;
}

} // namespace protoio::encoding
