// protoIO - shared input and output for the protoCore runtimes.
// Copyright (c) 2026 Gustavo Marino <gamarino@gmail.com>. MIT licensed.
#include "protoio/msgpack.h"
#include "protoio/error.h"
#include "vectors.h"

#include <gtest/gtest.h>

#include <bit>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

using namespace protoio;
using protoio_test::bytesField;
using protoio_test::fromHex;
using protoio_test::JsonValue;
using protoio_test::loadVectorArray;

namespace {

template <class F>
void expectInvalid(F f) {
    try {
        f();
        FAIL() << "expected InvalidArgument";
    } catch (const Error& e) {
        EXPECT_EQ(e.kind, Error::Kind::InvalidArgument) << e.what();
    }
}

// A document nested too deep: its own kind, so a caller can name its limit.
template <class F>
void expectDepthLimit(F f) {
    try {
        f();
        FAIL() << "expected DepthLimit";
    } catch (const Error& e) {
        EXPECT_EQ(e.kind, Error::Kind::DepthLimit) << e.what();
        EXPECT_STREQ(protoio::kindName(e.kind), "DepthLimit");
    }
}

std::string hex(std::string_view s) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    for (unsigned char c : s) {
        out += digits[c >> 4];
        out += digits[c & 15];
    }
    return out;
}

std::string bitsHex(uint64_t bits) {
    char buf[17];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(bits));
    return buf;
}

// Walks a tagged vector value into Writer calls.
void writeValue(msgpack::Writer& w, const JsonValue& v) {
    const std::string& t = v["t"].str();
    if (t == "nil") {
        w.nil();
    } else if (t == "bool") {
        w.boolean(v["v"].boolean());
    } else if (t == "int") {
        if (v["v"].str()[0] == '-') {
            w.integer(v["v"].i64());
        } else {
            uint64_t u = v["v"].u64();
            if (u <= uint64_t(std::numeric_limits<int64_t>::max()))
                w.integer(int64_t(u));  // the signed entry point, non-negative
            else
                w.unsignedInteger(u);
        }
    } else if (t == "f64") {
        w.float64(std::bit_cast<double>(std::stoull(v["bits"].str(), nullptr, 16)));
    } else if (t == "str") {
        w.str(v["v"].str());
    } else if (t == "bin") {
        w.bin(v.has("v") ? fromHex(v["v"].str()) : bytesField(v));
    } else if (t == "array") {
        const auto& items = v["v"].items();
        w.beginArray(uint32_t(items.size()));
        for (const auto& item : items) writeValue(w, item);
    } else if (t == "map") {
        const auto& pairs = v["v"].items();
        w.beginMap(uint32_t(pairs.size()));
        for (const auto& kv : pairs) {
            writeValue(w, kv[0]);
            writeValue(w, kv[1]);
        }
    } else if (t == "ext") {
        w.ext(int8_t(v["type"].i64()), fromHex(v["v"].str()));
    } else {
        throw std::runtime_error("unknown tag " + t);
    }
}

// The canonical text of a tagged vector value, the same text TreeHandler
// builds from reader events.
std::string canonical(const JsonValue& v) {
    const std::string& t = v["t"].str();
    if (t == "nil") return "nil";
    if (t == "bool") return v["v"].boolean() ? "true" : "false";
    if (t == "int") return "int:" + v["v"].str();
    if (t == "f64") return "f64:" + v["bits"].str();
    if (t == "str") return "str:" + hex(v["v"].str());
    if (t == "bin") return "bin:" + hex(v.has("v") ? fromHex(v["v"].str()) : bytesField(v));
    if (t == "ext") return "ext(" + std::to_string(v["type"].i64()) + "):" + v["v"].str();
    std::string out;
    if (t == "array") {
        out = "[";
        for (const auto& item : v["v"].items()) out += canonical(item) + ",";
        return out + "]";
    }
    if (t == "map") {
        out = "{";
        for (const auto& kv : v["v"].items()) out += canonical(kv[0]) + "=" + canonical(kv[1]) + ",";
        return out + "}";
    }
    throw std::runtime_error("unknown tag " + t);
}

// Rebuilds the canonical tree text from reader events.
struct TreeHandler : msgpack::Handler {
    std::string out;
    struct Frame { uint32_t left; bool map; bool key; };
    std::vector<Frame> stack;

    void before() {
        if (!stack.empty() && stack.back().map && !stack.back().key) out += "=";
    }
    void after() {
        while (!stack.empty()) {
            Frame& f = stack.back();
            if (f.map && f.key) {  // a key was written; its value follows
                f.key = false;
                return;
            }
            out += ",";
            if (f.map) f.key = true;
            if (--f.left > 0) return;
            out += f.map ? "}" : "]";
            stack.pop_back();
        }
    }
    void scalar(const std::string& s) { before(); out += s; after(); }
    void open(uint32_t n, bool map) {
        before();
        out += map ? "{" : "[";
        if (n == 0) {
            out += map ? "}" : "]";
            after();
        } else {
            stack.push_back({n, map, true});
        }
    }

    void nil() override { scalar("nil"); }
    void boolean(bool v) override { scalar(v ? "true" : "false"); }
    void integer(int64_t v) override { scalar("int:" + std::to_string(v)); }
    void unsignedInteger(uint64_t v) override {
        EXPECT_GT(v, uint64_t(std::numeric_limits<int64_t>::max()));
        scalar("int:" + std::to_string(v));
    }
    void float64(double v) override { scalar("f64:" + bitsHex(std::bit_cast<uint64_t>(v))); }
    void str(std::string_view s) override { scalar("str:" + hex(s)); }
    void bin(std::string_view s) override { scalar("bin:" + hex(s)); }
    void beginArray(uint32_t n) override { open(n, false); }
    void beginMap(uint32_t n) override { open(n, true); }
    void ext(int8_t type, std::string_view data) override {
        scalar("ext(" + std::to_string(int(type)) + "):" + hex(data));
    }
};

// Counts events and keeps the last scalar, for the hand-written cases.
struct CountingHandler : msgpack::Handler {
    int events = 0, bins = 0, containers = 0;
    double lastFloat = 0;
    int8_t extType = 0;
    std::string extData;

    void nil() override { ++events; }
    void boolean(bool) override { ++events; }
    void integer(int64_t) override { ++events; }
    void unsignedInteger(uint64_t) override { ++events; }
    void float64(double v) override { ++events; lastFloat = v; }
    void str(std::string_view) override { ++events; }
    void bin(std::string_view) override { ++events; ++bins; }
    void beginArray(uint32_t) override { ++events; ++containers; }
    void beginMap(uint32_t) override { ++events; ++containers; }
    void ext(int8_t type, std::string_view data) override {
        ++events;
        extType = type;
        extData = std::string(data);
    }
};

msgpack::Limits lenientKeys() {
    msgpack::Limits l;
    l.strictMapKeys = false;  // the vectors include a map with int keys
    return l;
}

} // namespace

TEST(Msgpack, Writer_MatchesCPython) {
    auto vectors = loadVectorArray("msgpack.json");
    ASSERT_GT(vectors.size(), 60u);
    for (const auto& v : vectors) {
        SCOPED_TRACE(v["description"].str());
        msgpack::Writer w;
        writeValue(w, v["value"]);
        EXPECT_EQ(hex(w.bytes()), v["bytes"].str());
        std::string taken = w.take();
        EXPECT_EQ(hex(taken), v["bytes"].str());
        EXPECT_TRUE(w.bytes().empty());
    }
}

TEST(Msgpack, Reader_MatchesCPython) {
    auto vectors = loadVectorArray("msgpack.json");
    for (const auto& v : vectors) {
        SCOPED_TRACE(v["description"].str());
        std::string bytes = fromHex(v["bytes"].str());
        TreeHandler h;
        size_t used = msgpack::read(bytes, h, lenientKeys());
        EXPECT_EQ(used, bytes.size());
        EXPECT_TRUE(h.stack.empty());
        EXPECT_EQ(h.out, canonical(v["value"]));
    }
}

TEST(Msgpack, Reader_Truncated) {
    auto vectors = loadVectorArray("msgpack.json");
    for (const auto& v : vectors) {
        SCOPED_TRACE(v["description"].str());
        std::string bytes = fromHex(v["bytes"].str());
        for (size_t n = 0; n < bytes.size(); ++n) {
            CountingHandler h;
            try {
                msgpack::read(std::string_view(bytes).substr(0, n), h, lenientKeys());
                ADD_FAILURE() << "prefix of " << n << " bytes was accepted";
                break;
            } catch (const Error& e) {
                if (e.kind != Error::Kind::InvalidArgument) {
                    ADD_FAILURE() << "prefix of " << n << ": " << e.what();
                    break;
                }
            }
        }
    }
}

TEST(Msgpack, Reader_HugeDeclaredLength) {
    // bin32 declaring 4 GiB - 1 with 10 bytes behind it: refused before any
    // event (and so before anything the size of the declared length exists).
    std::string data = std::string("\xc6\xff\xff\xff\xff", 5) + std::string(10, 'x');
    CountingHandler h;
    expectInvalid([&] { msgpack::read(data, h); });
    EXPECT_EQ(h.bins, 0);
    EXPECT_EQ(h.events, 0);

    // The same for str32, ext32, array32 and map32.
    for (const std::string_view head :
         {std::string_view("\xdb\xff\xff\xff\xff", 5), std::string_view("\xc9\xff\xff\xff\xff\x01", 6),
          std::string_view("\xdd\xff\xff\xff\xff", 5), std::string_view("\xdf\xff\xff\xff\xff", 5)}) {
        std::string d = std::string(head.data(), head.size()) + std::string(10, '\x00');
        CountingHandler c;
        expectInvalid([&] { msgpack::read(d, c); });
        EXPECT_EQ(c.events, 0) << hex(head);
    }
    // A map needs at least two bytes per entry: 3 entries cannot fit in 5 bytes.
    CountingHandler m;
    expectInvalid([&] { msgpack::read(std::string_view("\x83\x00\x00\x00\x00\x00", 6), m); });
    EXPECT_EQ(m.events, 0);
}

TEST(Msgpack, Reader_ElementLimitIsPerDocument) {
    // [[1, 2], [3]] declares 2 + 2 + 1 = 5 elements in total.
    std::string data("\x92\x92\x01\x02\x91\x03", 6);
    msgpack::Limits limits;
    limits.maxElements = 5;
    CountingHandler ok;
    EXPECT_EQ(msgpack::read(data, ok, limits), data.size());
    limits.maxElements = 4;
    CountingHandler h;
    expectInvalid([&] { msgpack::read(data, h, limits); });
    // A map entry counts once: {"a": 1} declares one element.
    limits.maxElements = 1;
    CountingHandler m;
    EXPECT_EQ(msgpack::read(std::string_view("\x81\xa1" "a" "\x01", 4), m, limits), 4u);
}

TEST(Msgpack, Reader_DepthLimit) {
    std::string deep(1100, '\x91');
    deep += '\xc0';
    CountingHandler h;
    expectDepthLimit([&] { msgpack::read(deep, h); });

    // Exactly maxDepth levels are accepted, one more is not.
    std::string at(1024, '\x91');
    at += '\xc0';
    CountingHandler ok;
    EXPECT_EQ(msgpack::read(at, ok), at.size());
    std::string over(1025, '\x91');
    over += '\x90';
    CountingHandler no;
    expectDepthLimit([&] { msgpack::read(over, no); });

    msgpack::Limits limits;
    limits.maxDepth = 2;
    CountingHandler two;
    EXPECT_EQ(msgpack::read(std::string_view("\x91\x81\xa1" "k" "\x01", 5), two, limits), 5u);
    CountingHandler three;
    expectDepthLimit([&] { msgpack::read(std::string_view("\x91\x91\x90", 3), three, limits); });
}

TEST(Msgpack, Reader_StrictMapKeys) {
    std::string data("\x81\x01\x02", 3);  // {1: 2}
    CountingHandler h;
    expectInvalid([&] { msgpack::read(data, h); });
    TreeHandler t;
    EXPECT_EQ(msgpack::read(data, t, lenientKeys()), 3u);
    EXPECT_EQ(t.out, "{int:1=int:2,}");

    // str and bin keys are accepted by default; a container key is not.
    TreeHandler s;
    EXPECT_EQ(msgpack::read(std::string_view("\x82\xa1" "a" "\x01\xc4\x01" "b" "\x02", 8), s), 8u);
    EXPECT_EQ(s.out, "{str:61=int:1,bin:62=int:2,}");
    CountingHandler c;
    expectInvalid([&] { msgpack::read(std::string_view("\x81\x90\x01", 3), c); });
    // A value of any type is fine.
    CountingHandler v;
    EXPECT_EQ(msgpack::read(std::string_view("\x81\xa1" "a" "\x81\xa1" "b" "\x90", 7), v), 7u);
}

TEST(Msgpack, Reader_Malformed) {
    CountingHandler h;
    expectInvalid([&] { msgpack::read(std::string_view("\xc1", 1), h); });  // never used
    // Invalid UTF-8 in a str (unpackb(raw=False) raises).
    expectInvalid([&] { msgpack::read(std::string_view("\xa2\xc0\x80", 3), h); });
    expectInvalid([&] { msgpack::read(std::string_view("\xa3\xed\xa0\x80", 4), h); });
    // Trailing bytes are left to the caller.
    CountingHandler t;
    EXPECT_EQ(msgpack::read(std::string_view("\x01\x02\x03", 3), t), 1u);
    EXPECT_EQ(t.events, 1);
}

TEST(Msgpack, Reader_NegativeExtType) {
    // timestamp32 (ext type -1) of the epoch.
    std::string data("\xd6\xff\x00\x00\x00\x00", 6);
    CountingHandler h;
    EXPECT_EQ(msgpack::read(data, h), 6u);
    EXPECT_EQ(h.extType, -1);
    EXPECT_EQ(h.extData, std::string(4, '\0'));
    msgpack::Writer w;
    w.ext(h.extType, h.extData);
    EXPECT_EQ(w.bytes(), data);
    // ext8 form with type -128.
    msgpack::Writer w2;
    w2.ext(-128, "abc");
    EXPECT_EQ(hex(w2.bytes()), "c70380616263");
    CountingHandler h2;
    msgpack::read(w2.bytes(), h2);
    EXPECT_EQ(h2.extType, -128);
    EXPECT_EQ(h2.extData, "abc");
}

TEST(Msgpack, Reader_Float32Widened) {
    CountingHandler h;
    EXPECT_EQ(msgpack::read(std::string_view("\xca\x3f\xc0\x00\x00", 5), h), 5u);
    EXPECT_EQ(std::bit_cast<uint64_t>(h.lastFloat), std::bit_cast<uint64_t>(1.5));
    CountingHandler n;
    msgpack::read(std::string_view("\xca\x7f\xc0\x01\x23", 5), n);  // NaN with payload
    EXPECT_TRUE(std::isnan(n.lastFloat));
    // Widened exactly: the quiet bit and the payload move to the top of the
    // 52-bit mantissa (0x400123 << 29).
    EXPECT_EQ(std::bit_cast<uint64_t>(n.lastFloat), 0x7ff8002460000000ull);
    CountingHandler z;
    msgpack::read(std::string_view("\xca\x80\x00\x00\x00", 5), z);  // -0.0f
    EXPECT_EQ(std::bit_cast<uint64_t>(z.lastFloat), std::bit_cast<uint64_t>(-0.0));
}

TEST(Msgpack, Reader_IntegerRouting) {
    // uint64 on the wire at or below INT64_MAX goes to integer().
    TreeHandler a;
    msgpack::read(std::string_view("\xcf\x7f\xff\xff\xff\xff\xff\xff\xff", 9), a);
    EXPECT_EQ(a.out, "int:9223372036854775807");
    // Non-minimal encodings that CPython also accepts decode to the value.
    TreeHandler b;
    msgpack::read(std::string_view("\xd3\x00\x00\x00\x00\x00\x00\x00\x05", 9), b);
    EXPECT_EQ(b.out, "int:5");
    TreeHandler c;
    msgpack::read(std::string_view("\xd0\x05", 2), c);
    EXPECT_EQ(c.out, "int:5");
}

TEST(Msgpack, Writer_IntegerFamiliesAgree) {
    for (uint64_t u : {0ull, 1ull, 127ull, 128ull, 255ull, 256ull, 65535ull, 65536ull, 4294967295ull,
                       4294967296ull, 9223372036854775807ull}) {
        msgpack::Writer a, b;
        a.integer(int64_t(u));
        b.unsignedInteger(u);
        EXPECT_EQ(a.bytes(), b.bytes()) << u;
    }
}

TEST(Msgpack, Writer_InvalidUtf8) {
    for (std::string_view bad : {
             std::string_view("\x80", 1),              // lone continuation
             std::string_view("\xc0\x80", 2),          // overlong NUL
             std::string_view("\xc1\xbf", 2),          // overlong
             std::string_view("\xe0\x80\x80", 3),      // overlong
             std::string_view("\xf0\x80\x80\x80", 4),  // overlong
             std::string_view("\xed\xa0\x80", 3),      // surrogate U+D800
             std::string_view("\xed\xbf\xbf", 3),      // surrogate U+DFFF
             std::string_view("\xf4\x90\x80\x80", 4),  // U+110000
             std::string_view("\xf5\x80\x80\x80", 4),  // invalid lead
             std::string_view("\xff", 1),
             std::string_view("a\xc3", 2),             // truncated sequence
             std::string_view("\xe4\xb8", 2),
             std::string_view("\xc3\x28", 2),          // bad continuation
         }) {
        msgpack::Writer w;
        expectInvalid([&] { w.str(bad); });
        EXPECT_TRUE(w.bytes().empty()) << hex(bad);
    }
    // The boundaries are accepted.
    msgpack::Writer w;
    w.str(std::string_view("\x00\x7f\xc2\x80\xdf\xbf\xe0\xa0\x80\xed\x9f\xbf\xee\x80\x80\xef\xbf\xbf"
                           "\xf0\x90\x80\x80\xf4\x8f\xbf\xbf", 26));
    EXPECT_EQ(w.bytes().size(), 27u);
}

TEST(Msgpack, Float_BitExact) {
    for (uint64_t bits : {0x7ff8000000000123ull, 0x8000000000000000ull, 0xfff800000000abcdull}) {
        msgpack::Writer w;
        w.float64(std::bit_cast<double>(bits));
        EXPECT_EQ(hex(w.bytes()), "cb" + bitsHex(bits));
        CountingHandler h;
        EXPECT_EQ(msgpack::read(w.bytes(), h), 9u);
        EXPECT_EQ(std::bit_cast<uint64_t>(h.lastFloat), bits);
    }
}
