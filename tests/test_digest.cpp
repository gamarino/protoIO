// protoIO - shared input and output for the protoCore runtimes.
// Copyright (c) 2026 Gustavo Marino <gamarino@gmail.com>. MIT licensed.
#include "protoio/digest.h"
#include "protoio/error.h"
#include "vectors.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <random>

using namespace protoio::digest;
using protoio_test::bytesField;
using protoio_test::fromHex;
using protoio_test::loadVectorArray;
using protoio_test::loadVectors;

TEST(Crc32, MatchesCPython) {
    auto root = loadVectors("crc32.json");
    const auto& rows = root["crc"].items();
    ASSERT_GT(rows.size(), 5u);
    for (const auto& r : rows) {
        std::string data = bytesField(r["data"]);
        uint32_t init = static_cast<uint32_t>(r["init"].u64());
        EXPECT_EQ(crc32(data, init), static_cast<uint32_t>(r["crc"].u64())) << "size " << data.size();
    }
}

TEST(Crc32, ChainsLikeZlib) {
    EXPECT_EQ(crc32(""), 0u);
    EXPECT_EQ(crc32("123456789"), 0xCBF43926u);
    EXPECT_EQ(crc32("6789", crc32("12345")), 0xCBF43926u);
}

TEST(Crc32Combine, MatchesCPython) {
    auto root = loadVectors("crc32.json");
    const auto& rows = root["combine"].items();
    ASSERT_GT(rows.size(), 3u);
    for (const auto& r : rows) {
        std::string a = fromHex(r["a_hex"].str()), b = fromHex(r["b_hex"].str());
        uint32_t ca = static_cast<uint32_t>(r["crc_a"].u64()), cb = static_cast<uint32_t>(r["crc_b"].u64());
        EXPECT_EQ(crc32(a), ca);
        EXPECT_EQ(crc32Combine(ca, cb, r["len_b"].u64()), static_cast<uint32_t>(r["combined"].u64()));
        EXPECT_EQ(crc32Combine(ca, cb, b.size()), static_cast<uint32_t>(r["combined"].u64()));
    }
}

TEST(Crc32Combine, ZeroLengthAnswersCrcA) {
    EXPECT_EQ(crc32Combine(0xDEADBEEFu, 0x12345678u, 0), 0xDEADBEEFu);
}

TEST(Crc32Combine, EqualsConcatenation) {
    std::mt19937_64 rng(42);
    std::string buf(1 << 20, '\0');
    for (auto& c : buf) c = static_cast<char>(rng());
    for (int i = 0; i < 100; ++i) {
        size_t cut = rng() % (buf.size() + 1);
        std::string_view a(buf.data(), cut), b(buf.data() + cut, buf.size() - cut);
        EXPECT_EQ(crc32Combine(crc32(a), crc32(b), b.size()), crc32(buf)) << "cut " << cut;
    }
}

TEST(Crc32, UnalignedAndShortTails) {
    std::string buf(100, '\0');
    for (size_t i = 0; i < buf.size(); ++i) buf[i] = static_cast<char>(i * 7 + 1);
    for (size_t off = 0; off < 9; ++off)
        for (size_t len = 0; len < 40; ++len) {
            std::string_view v(buf.data() + off, len);
            uint32_t bytewise = 0;
            for (char c : v) bytewise = crc32(std::string_view(&c, 1), bytewise);
            EXPECT_EQ(crc32(v), bytewise);
        }
}

namespace {
std::string hexOf(const std::string& b) {
    static const char* d = "0123456789abcdef";
    std::string o;
    for (unsigned char c : b) { o += d[c >> 4]; o += d[c & 15]; }
    return o;
}
}

TEST(Blake2b, MatchesCPython) {
    auto rows = loadVectorArray("blake2b.json");
    ASSERT_GT(rows.size(), 10u);
    for (const auto& r : rows) {
        std::string data = bytesField(r["data"]), key = fromHex(r["key"].str());
        int size = static_cast<int>(r["size"].u64());
        EXPECT_EQ(hexOf(blake2b(data, size, key)), r["digest"].str())
            << "size " << size << " data " << data.size() << " key " << key.size();
        Blake2b h(size, key);  // byte-at-a-time must agree as well
        for (char c : data) h.update(std::string_view(&c, 1));
        EXPECT_EQ(hexOf(h.finish()), r["digest"].str());
    }
}

TEST(Blake2b, Rfc7693Abc) {
    EXPECT_EQ(hexOf(blake2b("abc")),
              "ba80a53f981c4d0d6a2797b69f12f6e94c212f14685ac4b74b12bb6fdbffa2d1"
              "7d87c5392aab792dc252d5de4533cc9518d38aa8dbf1925ab92386edd4009923");
}

TEST(Blake2b, Size8_IsNotTruncated512) {
    EXPECT_NE(blake2b("abc", 8), blake2b("abc", 64).substr(0, 8));
    EXPECT_EQ(blake2b("abc", 8).size(), 8u);
}

TEST(Blake2b, Incremental_EqualsOneShot) {
    std::mt19937_64 rng(7);
    std::string buf(1 << 20, '\0');
    for (auto& c : buf) c = static_cast<char>(rng());
    for (size_t len : {size_t(0), size_t(128), size_t(256), buf.size()}) {
        std::string_view all(buf.data(), len);
        Blake2b h(32, "k");
        for (size_t off = 0; off < len;) {
            size_t n = std::min<size_t>(1 + rng() % 300, len - off);
            h.update(all.substr(off, n));
            off += n;
        }
        EXPECT_EQ(h.finish(), blake2b(all, 32, "k")) << "len " << len;
    }
}

TEST(Blake2b, BadParams) {
    using protoio::Error;
    auto kind = [](auto f) {
        try { f(); } catch (const Error& e) { return e.kind; }
        return Error::Kind::Network;
    };
    EXPECT_EQ(kind([] { blake2b("x", 0); }), Error::Kind::InvalidArgument);
    EXPECT_EQ(kind([] { blake2b("x", 65); }), Error::Kind::InvalidArgument);
    EXPECT_EQ(kind([] { blake2b("x", 64, std::string(65, 'k')); }), Error::Kind::InvalidArgument);
    EXPECT_EQ(kind([] { Blake2b(-1); }), Error::Kind::InvalidArgument);
    EXPECT_NO_THROW(blake2b("x", 64, std::string(64, 'k')));
}

TEST(Blake2b, FinishTwiceOrUpdateAfterFinishThrows) {
    using protoio::Error;
    Blake2b h;
    h.update("a");
    h.finish();
    EXPECT_THROW(h.finish(), Error);
    EXPECT_THROW(h.update("b"), Error);
}

TEST(Sha256, MatchesCPython) {
    auto rows = loadVectorArray("sha256_hmac.json");
    int n = 0;
    for (const auto& r : rows) {
        if (!r.has("sha256")) continue;
        ++n;
        std::string data = bytesField(r["data"]);
        EXPECT_EQ(hexOf(sha256(data)), r["sha256"].str());
        Sha256 h;
        for (size_t i = 0; i < data.size(); i += 7) h.update(std::string_view(data).substr(i, 7));
        EXPECT_EQ(hexOf(h.finish()), r["sha256"].str());
    }
    EXPECT_GT(n, 3);
}

TEST(Sha256, FinishTwiceOrUpdateAfterFinishThrows) {
    Sha256 h;
    h.finish();
    EXPECT_THROW(h.finish(), protoio::Error);
    EXPECT_THROW(h.update("b"), protoio::Error);
}

TEST(Hmac, MatchesCPython) {
    auto rows = loadVectorArray("sha256_hmac.json");
    int n = 0;
    for (const auto& r : rows) {
        if (!r.has("hmac")) continue;
        ++n;
        EXPECT_EQ(hexOf(hmacSha256(fromHex(r["key"].str()), bytesField(r["data"]))), r["hmac"].str());
    }
    EXPECT_GT(n, 3);
}
