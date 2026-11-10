// protoIO - shared input and output for the protoCore runtimes.
// Copyright (c) 2026 Gustavo Marino <gamarino@gmail.com>. MIT licensed.
#include "protoio/encoding.h"
#include "protoio/error.h"
#include "vectors.h"

#include <gtest/gtest.h>

#include <bit>

using namespace protoio;
using namespace protoio::encoding;
using protoio_test::fromHex;
using protoio_test::loadVectorArray;

namespace {
template <class F>
void expectInvalid(F f) {
    try {
        f();
        FAIL() << "expected InvalidArgument";
    } catch (const Error& e) {
        EXPECT_EQ(e.kind, Error::Kind::InvalidArgument);
    }
}
} // namespace

TEST(Packing, MatchesCPython) {
    auto vectors = loadVectorArray("packing.json");
    ASSERT_GT(vectors.size(), 50u);
    for (const auto& v : vectors) {
        const std::string kind = v["kind"].str();
        int width = int(v["width"].num());
        bool be = v["big_endian"].boolean();
        std::string bytes = fromHex(v["bytes"].str());
        SCOPED_TRACE(kind + " width=" + std::to_string(width) + " be=" + std::to_string(be) +
                     " bytes=" + v["bytes"].str());
        std::string out = "x";  // put appends
        if (kind == "u") {
            uint64_t x = v["value"].u64();
            putUint(out, x, width, be);
            EXPECT_EQ(out.substr(1), bytes);
            EXPECT_EQ(getUint("x" + bytes, 1, width, be), x);
        } else if (kind == "i") {
            int64_t x = v["value"].i64();
            putInt(out, x, width, be);
            EXPECT_EQ(out.substr(1), bytes);
            EXPECT_EQ(getInt(bytes, 0, width, be), x);
        } else if (kind == "f64") {
            uint64_t bits = std::stoull(v["value"].str(), nullptr, 16);
            putFloat64(out, std::bit_cast<double>(bits), be);
            EXPECT_EQ(out.substr(1), bytes);
            EXPECT_EQ(std::bit_cast<uint64_t>(getFloat64(bytes, 0, be)), bits);
        } else if (kind == "f32") {
            uint32_t bits = uint32_t(std::stoull(v["value"].str(), nullptr, 16));
            putFloat32(out, std::bit_cast<float>(bits), be);
            EXPECT_EQ(out.substr(1), bytes);
            EXPECT_EQ(std::bit_cast<uint32_t>(getFloat32(bytes, 0, be)), bits);
        } else {
            FAIL() << "unknown kind " << kind;
        }
    }
}

TEST(Packing, OutOfRange) {
    std::string out;
    expectInvalid([&] { putUint(out, 256, 1, false); });
    expectInvalid([&] { putInt(out, 128, 1, true); });
    expectInvalid([&] { putInt(out, -129, 1, true); });
    expectInvalid([&] { putUint(out, 1, 3, false); });
    expectInvalid([&] { getUint("ab", 1, 2, false); });
    expectInvalid([&] { getFloat64("1234567", 0, false); });
    expectInvalid([&] { getUint("ab", 5, 1, false); });
    EXPECT_TRUE(out.empty());
}

TEST(Base64, MatchesCPython) {
    auto vectors = loadVectorArray("base64.json");
    ASSERT_GT(vectors.size(), 20u);
    for (const auto& v : vectors) {
        std::string data = fromHex(v["data"].str());
        SCOPED_TRACE(v["text"].str());
        EXPECT_EQ(base64Encode(data), v["text"].str());
        EXPECT_EQ(base64Decode(v["text"].str()), data);
    }
}

TEST(Base64Decode, RejectsBadInput) {
    expectInvalid([] { base64Decode("abc"); });
    expectInvalid([] { base64Decode("ab!="); });
    expectInvalid([] { base64Decode("=abc"); });
    expectInvalid([] { base64Decode("ab=c"); });
    expectInvalid([] { base64Decode("a==="); });
    expectInvalid([] { base64Decode("ab\n="); });
}

TEST(Json, ReaderBasics) {
    auto v = protoio_test::parseJson(
        R"({"a":[1,-2,3.5e2,true,false,null],"s":"q\"\\\/\b\f\n\r\té😀","big":"18446744073709551615","n":9007199254740993})");
    EXPECT_EQ(v["a"].items().size(), 6u);
    EXPECT_EQ(v["a"][1].i64(), -2);
    EXPECT_DOUBLE_EQ(v["a"][2].num(), 350.0);
    EXPECT_TRUE(v["a"][3].boolean());
    EXPECT_TRUE(v["a"][5].isNull());
    EXPECT_EQ(v["s"].str(), "q\"\\/\b\f\n\r\t\xc3\xa9\xf0\x9f\x98\x80");
    EXPECT_EQ(v["big"].u64(), UINT64_MAX);
    EXPECT_EQ(v["n"].i64(), 9007199254740993LL);  // beyond 2^53, exact
    EXPECT_THROW(protoio_test::parseJson("[1,]"), std::runtime_error);
    EXPECT_THROW(protoio_test::parseJson("{} x"), std::runtime_error);
    EXPECT_EQ(protoio_test::bytesField(protoio_test::parseJson(R"({"pattern":"seq","length":253})"))[252],
              char(1));
}
