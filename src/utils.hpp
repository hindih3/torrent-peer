#pragma once
#include <openssl/evp.h>
#include <iomanip>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <string.h>

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

int createUDPIpv4Socket() {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd == -1)
        throw std::runtime_error(std::string("socket: ") + strerror(errno));
    return fd;
}

struct TrackerAddress {
    std::string host;
    std::string port;
};

TrackerAddress parse_tracker_url(const std::string& url) {
    if (url.substr(0, 6) != "udp://")
        throw std::runtime_error("Only UDP trackers supported: " + url);

    std::string rest = url.substr(6);

    size_t colon = rest.rfind(':');
    if (colon == std::string::npos)
        throw std::runtime_error("No port in tracker URL: " + url);

    return {
        rest.substr(0, colon),
        rest.substr(colon + 1)
    };
}

struct sockaddr_in createIPv4Address(const char* ip, int port) {
    struct sockaddr_in address = {0};

    address.sin_family = AF_INET;
    address.sin_port = htons(port);

    if (ip == NULL || ip[0] == '\0')
        address.sin_addr.s_addr = INADDR_ANY;
    else
        inet_pton(AF_INET, ip, &address.sin_addr);

    return address;
}