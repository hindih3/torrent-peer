#pragma once
#include <openssl/evp.h>
#include <iomanip>

std::string sha1(const std::string& data, size_t start, size_t length) {
    unsigned char hash[20];
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha1(), nullptr);
    EVP_DigestUpdate(ctx, data.data() + start, length);
    unsigned int hash_len;
    EVP_DigestFinal_ex(ctx, hash, &hash_len);
    EVP_MD_CTX_free(ctx);
    return std::string(reinterpret_cast<char*>(hash), 20);
}