// protoIO - shared input and output for the protoCore runtimes.
// Copyright (c) 2026 Gustavo Marino <gamarino@gmail.com>. MIT licensed.
//
// SHA-256 and HMAC-SHA256 on OpenSSL's EVP interfaces (OpenSSL 3.0+).
#include "protoio/digest.h"
#include "protoio/error.h"

#include <openssl/evp.h>
#include <openssl/core_names.h>
#include <openssl/params.h>

namespace protoio::digest {
namespace {

[[noreturn]] void fail(Error::Kind k, const char* m) { throw Error(k, m); }

struct MdCtxDeleter { void operator()(EVP_MD_CTX* c) const { EVP_MD_CTX_free(c); } };
struct MacDeleter { void operator()(EVP_MAC* m) const { EVP_MAC_free(m); } };
struct MacCtxDeleter { void operator()(EVP_MAC_CTX* c) const { EVP_MAC_CTX_free(c); } };

} // namespace

struct Sha256::Impl {
    std::unique_ptr<EVP_MD_CTX, MdCtxDeleter> ctx{EVP_MD_CTX_new()};
    bool finished = false;
    Impl() {
        if (!ctx || EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) != 1)
            fail(Error::Kind::Internal, "sha256: OpenSSL initialisation failed");
    }
};

Sha256::Sha256() : impl_(std::make_unique<Impl>()) {}
Sha256::~Sha256() = default;
Sha256::Sha256(Sha256&&) noexcept = default;
Sha256& Sha256::operator=(Sha256&&) noexcept = default;

void Sha256::update(std::string_view data) {
    if (impl_->finished) fail(Error::Kind::InvalidArgument, "sha256: update after finish");
    if (EVP_DigestUpdate(impl_->ctx.get(), data.data(), data.size()) != 1)
        fail(Error::Kind::Internal, "sha256: OpenSSL update failed");
}

std::string Sha256::finish() {
    if (impl_->finished) fail(Error::Kind::InvalidArgument, "sha256: finish called twice");
    impl_->finished = true;
    unsigned char out[EVP_MAX_MD_SIZE];
    unsigned int n = 0;
    if (EVP_DigestFinal_ex(impl_->ctx.get(), out, &n) != 1)
        fail(Error::Kind::Internal, "sha256: OpenSSL finalisation failed");
    return std::string(reinterpret_cast<char*>(out), n);
}

std::string sha256(std::string_view data) {
    Sha256 h;
    h.update(data);
    return h.finish();
}

std::string hmacSha256(std::string_view key, std::string_view data) {
    std::unique_ptr<EVP_MAC, MacDeleter> mac{EVP_MAC_fetch(nullptr, "HMAC", nullptr)};
    if (!mac) fail(Error::Kind::Internal, "hmac: HMAC not available in OpenSSL");
    std::unique_ptr<EVP_MAC_CTX, MacCtxDeleter> ctx{EVP_MAC_CTX_new(mac.get())};
    if (!ctx) fail(Error::Kind::Internal, "hmac: OpenSSL context allocation failed");
    char digestName[] = "SHA256";
    OSSL_PARAM params[2] = {
        OSSL_PARAM_construct_utf8_string(OSSL_MAC_PARAM_DIGEST, digestName, 0),
        OSSL_PARAM_construct_end()};
    unsigned char out[EVP_MAX_MD_SIZE];
    size_t n = 0;
    // EVP_MAC_init treats a null key pointer as "no key set" and fails, but an
    // empty key is valid HMAC input; a default-constructed string_view has a
    // null data(), so hand OpenSSL a non-null pointer with length 0.
    static const unsigned char kNoKey[1] = {0};
    const unsigned char* keyPtr = key.empty() ? kNoKey : reinterpret_cast<const unsigned char*>(key.data());
    if (EVP_MAC_init(ctx.get(), keyPtr, key.size(), params) != 1 ||
        EVP_MAC_update(ctx.get(), reinterpret_cast<const unsigned char*>(data.data()), data.size()) != 1 ||
        EVP_MAC_final(ctx.get(), out, &n, sizeof out) != 1)
        fail(Error::Kind::Internal, "hmac: OpenSSL computation failed");
    return std::string(reinterpret_cast<char*>(out), n);
}

} // namespace protoio::digest
