#pragma once
#include <array>
#include <stdexcept>
#include <cstdint>
#include <string>
#include <openssl/evp.h>
#include <openssl/types.h>

inline std::array<uint8_t, 20> sha1(const uint8_t* data, size_t len) {
    std::array<uint8_t, 20> hash{};
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) throw std::runtime_error("EVP_MD_CTX_new failed");
    unsigned int hash_len = 0;
    EVP_DigestInit_ex(ctx, EVP_sha1(), nullptr);
    EVP_DigestUpdate(ctx, data, len);
    EVP_DigestFinal_ex(ctx, hash.data(), &hash_len);
    EVP_MD_CTX_free(ctx);
    return hash;
}

inline std::array<uint8_t, 20> sha1(const std::string& data, size_t start, size_t length) {
    return sha1(reinterpret_cast<const uint8_t*>(data.data() + start), length);
}