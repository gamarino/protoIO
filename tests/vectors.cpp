// protoIO - shared input and output for the protoCore runtimes.
// Copyright (c) 2026 Gustavo Marino <gamarino@gmail.com>. MIT licensed.
#include "vectors.h"

#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>

#ifndef PROTOIO_VECTORS_DIR
#error "PROTOIO_VECTORS_DIR must be defined by the build"
#endif

namespace protoio_test {

namespace {
[[noreturn]] void fail(const std::string& m) { throw std::runtime_error("json: " + m); }
} // namespace

bool JsonValue::boolean() const {
    if (type_ != Type::Bool) fail("not a boolean");
    return bool_;
}
const std::string& JsonValue::str() const {
    if (type_ != Type::String) fail("not a string");
    return text_;
}
double JsonValue::num() const {
    if (type_ != Type::Number) fail("not a number");
    return std::strtod(text_.c_str(), nullptr);
}
const std::string& JsonValue::numText() const {
    if (type_ != Type::Number) fail("not a number");
    return text_;
}
int64_t JsonValue::i64() const {
    if (type_ != Type::Number && type_ != Type::String) fail("not an integer");
    errno = 0;
    char* end = nullptr;
    long long v = std::strtoll(text_.c_str(), &end, 10);
    if (text_.empty() || *end != '\0' || errno == ERANGE) fail("bad int64: " + text_);
    return v;
}
uint64_t JsonValue::u64() const {
    if (type_ != Type::Number && type_ != Type::String) fail("not an integer");
    if (text_.empty() || text_[0] == '-') fail("bad uint64: " + text_);
    errno = 0;
    char* end = nullptr;
    unsigned long long v = std::strtoull(text_.c_str(), &end, 10);
    if (*end != '\0' || errno == ERANGE) fail("bad uint64: " + text_);
    return v;
}
const std::vector<JsonValue>& JsonValue::items() const {
    if (type_ != Type::Array) fail("not an array");
    return items_;
}
const std::vector<std::pair<std::string, JsonValue>>& JsonValue::members() const {
    if (type_ != Type::Object) fail("not an object");
    return members_;
}
bool JsonValue::has(const std::string& key) const {
    for (const auto& m : members()) if (m.first == key) return true;
    return false;
}
const JsonValue& JsonValue::operator[](const std::string& key) const {
    for (const auto& m : members()) if (m.first == key) return m.second;
    fail("missing key: " + key);
}
const JsonValue& JsonValue::operator[](size_t index) const {
    const auto& it = items();
    if (index >= it.size()) fail("array index out of range");
    return it[index];
}

class JsonParser {
public:
    explicit JsonParser(const std::string& s) : s_(s) {}

    JsonValue document() {
        JsonValue v = value();
        skipWs();
        if (pos_ != s_.size()) fail("trailing characters");
        return v;
    }

private:
    const std::string& s_;
    size_t pos_ = 0;

    void skipWs() {
        while (pos_ < s_.size() &&
               (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' || s_[pos_] == '\r'))
            ++pos_;
    }
    char peek() {
        if (pos_ >= s_.size()) fail("unexpected end");
        return s_[pos_];
    }
    void expect(char c) {
        if (peek() != c) fail(std::string("expected '") + c + "'");
        ++pos_;
    }
    void literal(const char* word) {
        for (const char* p = word; *p; ++p) expect(*p);
    }

    JsonValue value() {
        skipWs();
        JsonValue v;
        char c = peek();
        if (c == '{') {
            ++pos_;
            v.type_ = JsonValue::Type::Object;
            skipWs();
            if (peek() == '}') { ++pos_; return v; }
            for (;;) {
                skipWs();
                std::string key = string();
                skipWs();
                expect(':');
                JsonValue member = value();
                v.members_.emplace_back(std::move(key), std::move(member));
                skipWs();
                if (peek() == ',') { ++pos_; continue; }
                expect('}');
                return v;
            }
        }
        if (c == '[') {
            ++pos_;
            v.type_ = JsonValue::Type::Array;
            skipWs();
            if (peek() == ']') { ++pos_; return v; }
            for (;;) {
                v.items_.push_back(value());
                skipWs();
                if (peek() == ',') { ++pos_; continue; }
                expect(']');
                return v;
            }
        }
        if (c == '"') {
            v.type_ = JsonValue::Type::String;
            v.text_ = string();
            return v;
        }
        if (c == 't') { literal("true"); v.type_ = JsonValue::Type::Bool; v.bool_ = true; return v; }
        if (c == 'f') { literal("false"); v.type_ = JsonValue::Type::Bool; return v; }
        if (c == 'n') { literal("null"); return v; }
        if (c == '-' || (c >= '0' && c <= '9')) {
            size_t start = pos_;
            while (pos_ < s_.size() &&
                   (s_[pos_] == '-' || s_[pos_] == '+' || s_[pos_] == '.' || s_[pos_] == 'e' ||
                    s_[pos_] == 'E' || (s_[pos_] >= '0' && s_[pos_] <= '9')))
                ++pos_;
            v.type_ = JsonValue::Type::Number;
            v.text_ = s_.substr(start, pos_ - start);
            return v;
        }
        fail(std::string("unexpected character '") + c + "'");
    }

    unsigned hex4() {
        if (pos_ + 4 > s_.size()) fail("short \\u escape");
        unsigned v = 0;
        for (int i = 0; i < 4; ++i) {
            char c = s_[pos_++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= unsigned(c - '0');
            else if (c >= 'a' && c <= 'f') v |= unsigned(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= unsigned(c - 'A' + 10);
            else fail("bad \\u escape");
        }
        return v;
    }
    static void appendUtf8(std::string& out, unsigned cp) {
        if (cp < 0x80) out += char(cp);
        else if (cp < 0x800) { out += char(0xC0 | (cp >> 6)); out += char(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) {
            out += char(0xE0 | (cp >> 12)); out += char(0x80 | ((cp >> 6) & 0x3F));
            out += char(0x80 | (cp & 0x3F));
        } else {
            out += char(0xF0 | (cp >> 18)); out += char(0x80 | ((cp >> 12) & 0x3F));
            out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F));
        }
    }

    std::string string() {
        expect('"');
        std::string out;
        for (;;) {
            char c = peek();
            ++pos_;
            if (c == '"') return out;
            if (c != '\\') { out += c; continue; }
            char e = peek();
            ++pos_;
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    unsigned cp = hex4();
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        if (pos_ + 2 > s_.size() || s_[pos_] != '\\' || s_[pos_ + 1] != 'u')
                            fail("lone high surrogate");
                        pos_ += 2;
                        unsigned lo = hex4();
                        if (lo < 0xDC00 || lo > 0xDFFF) fail("bad low surrogate");
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        fail("lone low surrogate");
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default: fail("bad escape");
            }
        }
    }
};

JsonValue parseJson(const std::string& text) { return JsonParser(text).document(); }

JsonValue loadVectors(const std::string& file) {
    std::string path = std::string(PROTOIO_VECTORS_DIR) + "/" + file;
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open vector file: " + path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return parseJson(ss.str());
}

std::vector<JsonValue> loadVectorArray(const std::string& file) {
    return loadVectors(file).items();
}

std::string fromHex(const std::string& hex) {
    if (hex.size() % 2 != 0) fail("odd hex length");
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        fail("bad hex digit");
    };
    std::string out;
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2)
        out += char((nib(hex[i]) << 4) | nib(hex[i + 1]));
    return out;
}

std::string bytesField(const JsonValue& v) {
    if (v.isString()) return fromHex(v.str());
    if (v.isObject() && v["pattern"].str() == "seq") {
        size_t n = size_t(v["length"].u64());
        std::string out(n, '\0');
        for (size_t i = 0; i < n; ++i) out[i] = char(i % 251);
        return out;
    }
    fail("unsupported binary field");
}

} // namespace protoio_test
