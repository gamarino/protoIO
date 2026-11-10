// protoIO - shared input and output for the protoCore runtimes.
// Copyright (c) 2026 Gustavo Marino <gamarino@gmail.com>. MIT licensed.
//
// A minimal JSON reader for the CPython-generated vector files in
// tests/vectors (see tests/vectors/README.md). Test-only code.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace protoio_test {

// A parsed JSON value. Accessors throw std::runtime_error on a type mismatch
// or a missing key. Numbers keep their source text, so integers beyond 2^53
// are exact through i64()/u64() (never routed through double).
class JsonValue {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type() const { return type_; }
    bool isNull() const { return type_ == Type::Null; }
    bool isString() const { return type_ == Type::String; }
    bool isArray() const { return type_ == Type::Array; }
    bool isObject() const { return type_ == Type::Object; }

    bool boolean() const;
    const std::string& str() const;        // String payload (unescaped, UTF-8)
    double num() const;                    // Number as a double
    const std::string& numText() const;    // Number's exact source text
    int64_t i64() const;                   // Number, or decimal String, as int64_t
    uint64_t u64() const;                  // Number, or decimal String, as uint64_t
    const std::vector<JsonValue>& items() const;  // Array elements
    bool has(const std::string& key) const;       // Object member present?
    const JsonValue& operator[](const std::string& key) const;  // Object member
    const JsonValue& operator[](size_t index) const;            // Array element
    // Object members in source order.
    const std::vector<std::pair<std::string, JsonValue>>& members() const;

private:
    friend class JsonParser;
    Type type_ = Type::Null;
    bool bool_ = false;
    std::string text_;  // String payload or Number source text
    std::vector<JsonValue> items_;
    std::vector<std::pair<std::string, JsonValue>> members_;
};

// Parses a complete JSON document; throws std::runtime_error on bad input.
JsonValue parseJson(const std::string& text);

// Parses tests/vectors/<file> (PROTOIO_VECTORS_DIR) and returns the root.
// Most files are arrays; crc32.json is {"crc":[...],"combine":[...]}.
JsonValue loadVectors(const std::string& file);

// Convenience: the elements of a root that is an array.
std::vector<JsonValue> loadVectorArray(const std::string& file);

// Lowercase/uppercase hex to bytes; throws on odd length or non-hex digits.
std::string fromHex(const std::string& hex);

// Decodes a binary vector field: a hex string, or the descriptor object
// {"pattern":"seq","length":N} (byte i = i % 251).
std::string bytesField(const JsonValue& v);

} // namespace protoio_test
