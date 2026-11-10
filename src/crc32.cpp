// protoIO - shared input and output for the protoCore runtimes.
// Copyright (c) 2026 Gustavo Marino <gamarino@gmail.com>. MIT licensed.
//
// CRC-32 (reflected polynomial 0xEDB88320), compatible with zlib.crc32.
#include "protoio/digest.h"

#include <array>

namespace protoio::digest {
namespace {

constexpr uint32_t kPoly = 0xEDB88320u;

// kTables[0] is the classic byte table; kTables[k][i] is the CRC of byte i
// followed by k zero bytes, which lets eight bytes be folded per step.
constexpr std::array<std::array<uint32_t, 256>, 8> makeTables() {
    std::array<std::array<uint32_t, 256>, 8> t{};
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int b = 0; b < 8; ++b) c = (c & 1u) ? (c >> 1) ^ kPoly : c >> 1;
        t[0][i] = c;
    }
    for (uint32_t i = 0; i < 256; ++i)
        for (int k = 1; k < 8; ++k) t[k][i] = (t[k - 1][i] >> 8) ^ t[0][t[k - 1][i] & 0xFFu];
    return t;
}
constexpr auto kTables = makeTables();

// GF(2) 32x32 matrices, one column word per input bit.
using Matrix = std::array<uint32_t, 32>;

uint32_t gfApply(const Matrix& m, uint32_t v) {
    uint32_t sum = 0;
    for (int i = 0; v != 0; v >>= 1, ++i)
        if (v & 1u) sum ^= m[i];
    return sum;
}

Matrix square(const Matrix& m) {
    Matrix r{};
    for (int i = 0; i < 32; ++i) r[i] = gfApply(m, m[i]);
    return r;
}

} // namespace

uint32_t crc32(std::string_view data, uint32_t crc) {
    uint32_t c = ~crc;
    const auto* p = reinterpret_cast<const unsigned char*>(data.data());
    size_t n = data.size();
    // Byte loads (not word loads) keep this correct on any endianness and alignment.
    while (n >= 8) {
        uint32_t lo = c ^ (uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24);
        c = kTables[7][lo & 0xFFu] ^ kTables[6][(lo >> 8) & 0xFFu] ^ kTables[5][(lo >> 16) & 0xFFu] ^
            kTables[4][lo >> 24] ^ kTables[3][p[4]] ^ kTables[2][p[5]] ^ kTables[1][p[6]] ^ kTables[0][p[7]];
        p += 8;
        n -= 8;
    }
    while (n--) c = kTables[0][(c ^ *p++) & 0xFFu] ^ (c >> 8);
    return ~c;
}

uint32_t crc32Combine(uint32_t crcA, uint32_t crcB, uint64_t lenB) {
    if (lenB == 0) return crcA;
    // Shifting crcA past lenB zero bytes is a linear map over GF(2). Start
    // from the operator for one zero bit and square it to reach 2^k bits,
    // applying the squares selected by the bits of the length in bits.
    Matrix op{};
    op[0] = kPoly;
    for (int i = 1; i < 32; ++i) op[i] = 1u << (i - 1);
    op = square(op); // 2 bits
    op = square(op); // 4 bits
    // op now shifts by 4 bits; the first squaring in the loop gives 1 byte, then 2, 4, ...
    uint64_t len = lenB;
    do {
        op = square(op);
        if (len & 1u) crcA = gfApply(op, crcA);
        len >>= 1;
    } while (len != 0);
    return crcA ^ crcB;
}

} // namespace protoio::digest
