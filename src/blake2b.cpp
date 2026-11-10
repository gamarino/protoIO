// protoIO - shared input and output for the protoCore runtimes.
// Copyright (c) 2026 Gustavo Marino <gamarino@gmail.com>. MIT licensed.
//
// BLAKE2b (RFC 7693), unkeyed and keyed, digest sizes 1..64. The digest size
// is part of the parameter block, so a size-8 digest is not a truncated
// size-64 digest; this matches hashlib.blake2b(digest_size=...).
#include "protoio/digest.h"
#include "protoio/error.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace protoio::digest {
namespace {

constexpr std::array<uint64_t, 8> kIv = {
    0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
    0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL, 0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL};

constexpr uint8_t kSigma[12][16] = {
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
    {14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3},
    {11, 8, 12, 0, 5, 2, 15, 13, 10, 14, 3, 6, 7, 1, 9, 4},
    {7, 9, 3, 1, 13, 12, 11, 14, 2, 6, 5, 10, 4, 0, 15, 8},
    {9, 0, 5, 7, 2, 4, 10, 15, 14, 1, 11, 12, 6, 8, 3, 13},
    {2, 12, 6, 10, 0, 11, 8, 3, 4, 13, 7, 5, 15, 14, 1, 9},
    {12, 5, 1, 15, 14, 13, 4, 10, 0, 7, 6, 3, 9, 2, 8, 11},
    {13, 11, 7, 14, 12, 1, 3, 9, 5, 0, 15, 4, 8, 6, 2, 10},
    {6, 15, 14, 9, 11, 3, 0, 8, 12, 2, 13, 7, 1, 4, 10, 5},
    {10, 2, 8, 4, 7, 6, 1, 5, 15, 11, 9, 14, 3, 12, 13, 0},
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
    {14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3}};

constexpr size_t kBlock = 128;

inline uint64_t rotr(uint64_t x, int n) { return (x >> n) | (x << (64 - n)); }

inline uint64_t load64(const uint8_t* p) {  // little endian, independent of host order
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}

[[noreturn]] void bad(const char* m) { throw Error(Error::Kind::InvalidArgument, m); }

} // namespace

struct Blake2b::Impl {
    std::array<uint64_t, 8> h;
    uint64_t t0 = 0, t1 = 0;  // 128-bit byte counter
    std::array<uint8_t, kBlock> buf{};
    size_t buflen = 0;
    size_t outlen;
    bool finished = false;

    Impl(int digestSize, std::string_view key) {
        if (digestSize < 1 || digestSize > 64) bad("blake2b: digest size must be 1..64");
        if (key.size() > 64) bad("blake2b: key longer than 64 bytes");
        outlen = static_cast<size_t>(digestSize);
        h = kIv;
        // Parameter block word 0: digest length, key length, fanout 1, depth 1.
        h[0] ^= 0x01010000ULL ^ (static_cast<uint64_t>(key.size()) << 8) ^ outlen;
        if (!key.empty()) {  // the key, zero padded, is the first block
            std::memcpy(buf.data(), key.data(), key.size());
            buflen = kBlock;
        }
    }

    void compress(bool last) {
        uint64_t m[16], v[16];
        for (int i = 0; i < 16; ++i) m[i] = load64(buf.data() + 8 * i);
        for (int i = 0; i < 8; ++i) { v[i] = h[i]; v[i + 8] = kIv[i]; }
        v[12] ^= t0;
        v[13] ^= t1;
        if (last) v[14] = ~v[14];
        auto g = [&](int a, int b, int c, int d, uint64_t x, uint64_t y) {
            v[a] = v[a] + v[b] + x; v[d] = rotr(v[d] ^ v[a], 32);
            v[c] = v[c] + v[d];     v[b] = rotr(v[b] ^ v[c], 24);
            v[a] = v[a] + v[b] + y; v[d] = rotr(v[d] ^ v[a], 16);
            v[c] = v[c] + v[d];     v[b] = rotr(v[b] ^ v[c], 63);
        };
        for (int r = 0; r < 12; ++r) {
            const uint8_t* s = kSigma[r];
            g(0, 4, 8, 12, m[s[0]], m[s[1]]);
            g(1, 5, 9, 13, m[s[2]], m[s[3]]);
            g(2, 6, 10, 14, m[s[4]], m[s[5]]);
            g(3, 7, 11, 15, m[s[6]], m[s[7]]);
            g(0, 5, 10, 15, m[s[8]], m[s[9]]);
            g(1, 6, 11, 12, m[s[10]], m[s[11]]);
            g(2, 7, 8, 13, m[s[12]], m[s[13]]);
            g(3, 4, 9, 14, m[s[14]], m[s[15]]);
        }
        for (int i = 0; i < 8; ++i) h[i] ^= v[i] ^ v[i + 8];
    }

    void addCount(uint64_t n) {
        t0 += n;
        if (t0 < n) ++t1;
    }
};

Blake2b::Blake2b(int digestSize, std::string_view key) : impl_(std::make_unique<Impl>(digestSize, key)) {}
Blake2b::~Blake2b() = default;
Blake2b::Blake2b(Blake2b&&) noexcept = default;
Blake2b& Blake2b::operator=(Blake2b&&) noexcept = default;

void Blake2b::update(std::string_view data) {
    Impl& s = *impl_;
    if (s.finished) bad("blake2b: update after finish");
    const auto* p = reinterpret_cast<const uint8_t*>(data.data());
    size_t n = data.size();
    while (n > 0) {
        // A full buffer is compressed only once more input arrives, so the
        // last block always reaches finish() and gets the final flag.
        if (s.buflen == kBlock) {
            s.addCount(kBlock);
            s.compress(false);
            s.buflen = 0;
        }
        size_t take = std::min(kBlock - s.buflen, n);
        std::memcpy(s.buf.data() + s.buflen, p, take);
        s.buflen += take;
        p += take;
        n -= take;
    }
}

std::string Blake2b::finish() {
    Impl& s = *impl_;
    if (s.finished) bad("blake2b: finish called twice");
    s.finished = true;
    s.addCount(s.buflen);
    std::memset(s.buf.data() + s.buflen, 0, kBlock - s.buflen);
    s.compress(true);
    std::string out(s.outlen, '\0');
    for (size_t i = 0; i < s.outlen; ++i) out[i] = static_cast<char>((s.h[i / 8] >> (8 * (i % 8))) & 0xFF);
    return out;
}

std::string blake2b(std::string_view data, int digestSize, std::string_view key) {
    Blake2b h(digestSize, key);
    h.update(data);
    return h.finish();
}

} // namespace protoio::digest
