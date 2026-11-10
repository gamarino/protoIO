// protoIO - shared input and output for the protoCore runtimes.
// Copyright (c) 2026 Gustavo Marino <gamarino@gmail.com>. MIT licensed.
//
// MessagePack writer and event reader. The encodings and their order follow
// msgpack-python 1.x (packb with use_bin_type=True, unpackb with raw=False).
#include "protoio/msgpack.h"

#include "protoio/error.h"

#include <bit>
#include <vector>

namespace protoio::msgpack {

namespace {

[[noreturn]] void bad(const std::string& m) { throw Error(Error::Kind::InvalidArgument, "msgpack: " + m); }

constexpr uint64_t kMax32 = 0xffffffffu;

// Strict UTF-8, as CPython's decoder: no overlong forms, no surrogates
// (U+D800..U+DFFF), nothing above U+10FFFF.
bool validUtf8(std::string_view s) {
    const size_t n = s.size();
    size_t i = 0;
    while (i < n) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            ++i;
            continue;
        }
        size_t len;
        uint32_t cp;
        if (c >= 0xc2 && c <= 0xdf) {
            len = 2;
            cp = c & 0x1fu;
        } else if (c >= 0xe0 && c <= 0xef) {
            len = 3;
            cp = c & 0x0fu;
        } else if (c >= 0xf0 && c <= 0xf4) {
            len = 4;
            cp = c & 0x07u;
        } else {
            return false;  // continuation byte, C0/C1 (always overlong) or F5..FF
        }
        if (n - i < len) return false;
        for (size_t k = 1; k < len; ++k) {
            const unsigned char b = static_cast<unsigned char>(s[i + k]);
            if ((b & 0xc0) != 0x80) return false;
            cp = (cp << 6) | (b & 0x3fu);
        }
        if (len == 3 && (cp < 0x800 || (cp >= 0xd800 && cp <= 0xdfff))) return false;
        if (len == 4 && (cp < 0x10000 || cp > 0x10ffff)) return false;
        i += len;
    }
    return true;
}

void putBE(std::string& out, uint64_t v, int width) {
    for (int shift = (width - 1) * 8; shift >= 0; shift -= 8) out += static_cast<char>((v >> shift) & 0xff);
}

void putHead(std::string& out, unsigned char code, uint64_t v, int width) {
    out += static_cast<char>(code);
    putBE(out, v, width);
}

void checkLength(std::string_view s, const char* what) {
    if (static_cast<uint64_t>(s.size()) > kMax32) bad(std::string(what) + " longer than 2^32 - 1 bytes");
}

} // namespace

// ----------------------------------------------------------------- Writer

void Writer::nil() { out_ += '\xc0'; }

void Writer::boolean(bool v) { out_ += v ? '\xc3' : '\xc2'; }

void Writer::unsignedInteger(uint64_t v) {
    if (v < 0x80)
        out_ += static_cast<char>(v);
    else if (v <= 0xff)
        putHead(out_, 0xcc, v, 1);
    else if (v <= 0xffff)
        putHead(out_, 0xcd, v, 2);
    else if (v <= kMax32)
        putHead(out_, 0xce, v, 4);
    else
        putHead(out_, 0xcf, v, 8);
}

void Writer::integer(int64_t v) {
    if (v >= 0) {
        unsignedInteger(static_cast<uint64_t>(v));
        return;
    }
    const uint64_t bits = static_cast<uint64_t>(v);  // two's complement
    if (v >= -32)
        out_ += static_cast<char>(bits & 0xff);
    else if (v >= INT8_MIN)
        putHead(out_, 0xd0, bits, 1);
    else if (v >= INT16_MIN)
        putHead(out_, 0xd1, bits, 2);
    else if (v >= INT32_MIN)
        putHead(out_, 0xd2, bits, 4);
    else
        putHead(out_, 0xd3, bits, 8);
}

void Writer::float64(double v) { putHead(out_, 0xcb, std::bit_cast<uint64_t>(v), 8); }

void Writer::str(std::string_view utf8) {
    checkLength(utf8, "str");
    if (!validUtf8(utf8)) bad("str is not valid UTF-8");
    const uint64_t n = utf8.size();
    if (n < 32)
        out_ += static_cast<char>(0xa0 | n);
    else if (n <= 0xff)
        putHead(out_, 0xd9, n, 1);
    else if (n <= 0xffff)
        putHead(out_, 0xda, n, 2);
    else
        putHead(out_, 0xdb, n, 4);
    out_ += utf8;
}

void Writer::bin(std::string_view bytes) {
    checkLength(bytes, "bin");
    const uint64_t n = bytes.size();
    if (n <= 0xff)
        putHead(out_, 0xc4, n, 1);
    else if (n <= 0xffff)
        putHead(out_, 0xc5, n, 2);
    else
        putHead(out_, 0xc6, n, 4);
    out_ += bytes;
}

void Writer::beginArray(uint32_t n) {
    if (n < 16)
        out_ += static_cast<char>(0x90 | n);
    else if (n <= 0xffff)
        putHead(out_, 0xdc, n, 2);
    else
        putHead(out_, 0xdd, n, 4);
}

void Writer::beginMap(uint32_t n) {
    if (n < 16)
        out_ += static_cast<char>(0x80 | n);
    else if (n <= 0xffff)
        putHead(out_, 0xde, n, 2);
    else
        putHead(out_, 0xdf, n, 4);
}

void Writer::ext(int8_t type, std::string_view data) {
    checkLength(data, "ext");
    const uint64_t n = data.size();
    switch (n) {
    case 1: out_ += '\xd4'; break;
    case 2: out_ += '\xd5'; break;
    case 4: out_ += '\xd6'; break;
    case 8: out_ += '\xd7'; break;
    case 16: out_ += '\xd8'; break;
    default:
        if (n <= 0xff)
            putHead(out_, 0xc7, n, 1);
        else if (n <= 0xffff)
            putHead(out_, 0xc8, n, 2);
        else
            putHead(out_, 0xc9, n, 4);
    }
    out_ += static_cast<char>(type);
    out_ += data;
}

std::string Writer::take() {
    std::string out = std::move(out_);
    out_.clear();
    return out;
}

// ----------------------------------------------------------------- Reader

namespace {

// Decodes one top-level object without C++ recursion: open containers live on
// an explicit stack bounded by Limits::maxDepth.
class Reader {
public:
    Reader(std::string_view data, Handler& h, const Limits& limits) : d_(data), h_(h), limits_(limits) {}

    size_t run() {
        do {
            if (limits_.strictMapKeys && !stack_.empty() && stack_.back().map && stack_.back().atKey)
                checkKey();
            item();
        } while (!stack_.empty());
        return pos_;
    }

private:
    struct Frame {
        uint32_t left;  // values still to come (a map entry counts once)
        bool map;
        bool atKey;     // map: the next value is a key
    };

    std::string_view d_;
    Handler& h_;
    const Limits& limits_;
    size_t pos_ = 0;
    uint64_t elements_ = 0;
    std::vector<Frame> stack_;

    size_t remaining() const { return d_.size() - pos_; }

    void need(uint64_t n) const {
        if (n > remaining()) bad("truncated input");
    }

    uint64_t be(int width) {
        need(static_cast<uint64_t>(width));
        uint64_t v = 0;
        for (size_t i = 0; i < static_cast<size_t>(width); ++i) v = (v << 8) | static_cast<unsigned char>(d_[pos_ + i]);
        pos_ += static_cast<size_t>(width);
        return v;
    }

    std::string_view take(uint64_t n) {
        need(n);
        std::string_view s = d_.substr(pos_, static_cast<size_t>(n));
        pos_ += static_cast<size_t>(n);
        return s;
    }

    // strict_map_key: only str and bin keys, refused before any event.
    void checkKey() const {
        need(1);
        const unsigned char c = static_cast<unsigned char>(d_[pos_]);
        const bool ok = (c >= 0xa0 && c <= 0xbf) || (c >= 0xd9 && c <= 0xdb) || (c >= 0xc4 && c <= 0xc6);
        if (!ok) bad("map key is not str or bin (strict map keys)");
    }

    // One value has been reported: advance the open containers.
    void done() {
        while (!stack_.empty()) {
            Frame& f = stack_.back();
            if (f.map && f.atKey) {
                f.atKey = false;
                return;
            }
            if (f.map) f.atKey = true;
            if (--f.left > 0) return;
            stack_.pop_back();
        }
    }

    void signedInt(int width) {
        const uint64_t raw = be(width);
        const int shift = 64 - 8 * width;
        // Sign-extend through the unsigned value; the conversion is modular
        // in C++20.
        h_.integer(static_cast<int64_t>(raw << shift) >> shift);
        done();
    }

    void unsignedInt(int width) {
        const uint64_t v = be(width);
        if (v > static_cast<uint64_t>(INT64_MAX))
            h_.unsignedInteger(v);
        else
            h_.integer(static_cast<int64_t>(v));
        done();
    }

    void string(uint64_t n) {
        std::string_view s = take(n);
        if (!validUtf8(s)) bad("str is not valid UTF-8");
        h_.str(s);
        done();
    }

    void binary(uint64_t n) {
        h_.bin(take(n));
        done();
    }

    void extension(uint64_t n) {
        need(n + 1);
        const int8_t type = static_cast<int8_t>(static_cast<unsigned char>(d_[pos_++]));
        h_.ext(type, take(n));
        done();
    }

    void container(uint64_t n, bool map) {
        if (stack_.size() >= limits_.maxDepth) bad("nesting deeper than the depth limit");
        if (n > limits_.maxElements - elements_) bad("more elements than the element limit");
        // Every element needs at least one byte, every map entry two.
        if ((map ? 2 * n : n) > remaining()) bad("truncated input (declared length exceeds the input)");
        elements_ += n;
        const uint32_t count = static_cast<uint32_t>(n);
        if (map)
            h_.beginMap(count);
        else
            h_.beginArray(count);
        if (count == 0)
            done();
        else
            stack_.push_back({count, map, true});
    }

    void item() {
        need(1);
        const unsigned char c = static_cast<unsigned char>(d_[pos_++]);
        if (c <= 0x7f) {
            h_.integer(c);
            done();
            return;
        }
        if (c >= 0xe0) {
            h_.integer(static_cast<int8_t>(c));
            done();
            return;
        }
        if (c <= 0x8f) return container(c & 0x0fu, true);
        if (c <= 0x9f) return container(c & 0x0fu, false);
        if (c <= 0xbf) return string(c & 0x1fu);
        switch (c) {
        case 0xc0: h_.nil(); return done();
        case 0xc2: h_.boolean(false); return done();
        case 0xc3: h_.boolean(true); return done();
        case 0xc4: return binary(be(1));
        case 0xc5: return binary(be(2));
        case 0xc6: return binary(be(4));
        case 0xc7: return extension(be(1));
        case 0xc8: return extension(be(2));
        case 0xc9: return extension(be(4));
        case 0xca:
            h_.float64(static_cast<double>(std::bit_cast<float>(static_cast<uint32_t>(be(4)))));
            return done();
        case 0xcb: h_.float64(std::bit_cast<double>(be(8))); return done();
        case 0xcc: return unsignedInt(1);
        case 0xcd: return unsignedInt(2);
        case 0xce: return unsignedInt(4);
        case 0xcf: return unsignedInt(8);
        case 0xd0: return signedInt(1);
        case 0xd1: return signedInt(2);
        case 0xd2: return signedInt(4);
        case 0xd3: return signedInt(8);
        case 0xd4: return extension(1);
        case 0xd5: return extension(2);
        case 0xd6: return extension(4);
        case 0xd7: return extension(8);
        case 0xd8: return extension(16);
        case 0xd9: return string(be(1));
        case 0xda: return string(be(2));
        case 0xdb: return string(be(4));
        case 0xdc: return container(be(2), false);
        case 0xdd: return container(be(4), false);
        case 0xde: return container(be(2), true);
        case 0xdf: return container(be(4), true);
        default: bad("invalid type byte 0xc1");  // the only byte left unassigned
        }
    }
};

} // namespace

size_t read(std::string_view data, Handler& h, const Limits& limits) {
    return Reader(data, h, limits).run();
}

} // namespace protoio::msgpack
