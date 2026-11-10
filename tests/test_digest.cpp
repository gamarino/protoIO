// protoIO - shared input and output for the protoCore runtimes.
// Copyright (c) 2026 Gustavo Marino <gamarino@gmail.com>. MIT licensed.
#include "protoio/digest.h"
#include "vectors.h"

#include <gtest/gtest.h>

#include <random>

using namespace protoio::digest;
using protoio_test::bytesField;
using protoio_test::fromHex;
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
