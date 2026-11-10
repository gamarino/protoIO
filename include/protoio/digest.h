// protoIO - shared input and output for the protoCore runtimes.
// Copyright (c) 2026 Gustavo Marino <gamarino@gmail.com>. MIT licensed.
//
// Checksums and hashes, byte-compatible with CPython's zlib and hashlib.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace protoio::digest {

// zlib.crc32(data, crc): `crc` is the value of a previous call, so calls chain.
uint32_t crc32(std::string_view data, uint32_t crc = 0);
// zlib's crc32_combine: the CRC of A+B from crc32(A), crc32(B) and len(B).
// lenB == 0 answers crcA.
uint32_t crc32Combine(uint32_t crcA, uint32_t crcB, uint64_t lenB);

// Raw digest bytes (not hex).
std::string blake2b(std::string_view data, int digestSize = 64, std::string_view key = {});
std::string sha256(std::string_view data);
std::string hmacSha256(std::string_view key, std::string_view data);

// Incremental forms for large inputs.
class Blake2b {
public:
    Blake2b(int digestSize = 64, std::string_view key = {});
    ~Blake2b();
    Blake2b(Blake2b&&) noexcept;
    Blake2b& operator=(Blake2b&&) noexcept;
    void update(std::string_view data);
    std::string finish();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class Sha256 {
public:
    Sha256();
    ~Sha256();
    Sha256(Sha256&&) noexcept;
    Sha256& operator=(Sha256&&) noexcept;
    void update(std::string_view data);
    std::string finish();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace protoio::digest
