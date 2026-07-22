#pragma once
#include <openssl/evp.h>
#include <iomanip>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <string.h>
#include <stdexcept>
#include <array>

std::array<uint8_t, 20> sha1(const std::string& data, size_t start, size_t length) {
    if (start > data.size() || length > data.size() - start) {
        throw std::out_of_range("sha1: start/length out of range");
    }

    std::array<uint8_t, 20> result{};
    unsigned char hash[20];
    unsigned int hash_len = 0;

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) {
        throw std::runtime_error("sha1: failed to create EVP_MD_CTX");
    }

    if (EVP_DigestInit_ex(ctx, EVP_sha1(), nullptr) != 1 ||
        EVP_DigestUpdate(ctx, data.data() + start, length) != 1 ||
        EVP_DigestFinal_ex(ctx, hash, &hash_len) != 1) {
        EVP_MD_CTX_free(ctx);
        throw std::runtime_error("sha1: OpenSSL digest operation failed");
    }

    EVP_MD_CTX_free(ctx);

    std::copy(hash, hash + 20, result.begin());
    return result;
}


int createUDPIpv4Socket() {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd == -1)
        throw std::runtime_error(std::string("socket: ") + strerror(errno));
    return fd;
}

int createTCPIpv4Socket() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    return fd;
}

std::string generate_peer_id() {
    std::string id = "-HB0002-";
    for (int i = 0; i < 12; ++i) {
        id += "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"[rand() % 62];
    }
    return id;
}

struct TrackerAddress {
    std::string host;
    std::string port;
};

TrackerAddress parse_tracker_url(const std::string& url) {
    const std::string prefix = "udp://";
    if (url.substr(0, prefix.size()) != prefix)
        throw std::runtime_error("Only UDP trackers supported: " + url);

    std::string rest = url.substr(prefix.size());

    size_t slash_pos = rest.find('/');
    if (slash_pos != std::string::npos)
        rest = rest.substr(0, slash_pos);

    size_t colon = rest.rfind(':');
    if (colon == std::string::npos)
        throw std::runtime_error("No port in tracker URL: " + url);

    return {
        rest.substr(0, colon),
        rest.substr(colon + 1)
    };
}