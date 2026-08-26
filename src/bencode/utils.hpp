#pragma once
#include <openssl/evp.h>
#include <format>
#include <iostream>
#include <array>
#include <string>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <cstring>
#include <random>
#include <stdexcept>

enum class LogLevel { Trace, Debug, Info, Warn, Error, Off };
inline LogLevel g_log_level = LogLevel::Info;

inline const char* prefix(LogLevel l) {
    switch (l) {
        case LogLevel::Trace: return "[TRACE] ";
        case LogLevel::Debug: return "[DEBUG] ";
        case LogLevel::Info:  return "[INFO]  ";
        case LogLevel::Warn:  return "[WARN]  ";
        case LogLevel::Error: return "[ERROR] ";
        default:              return "";
    }
}

template <typename... Args>
void log(const LogLevel level, std::format_string<Args...> fmt, Args&&... args) {
    if (level < g_log_level) return;
    std::cerr << prefix(level)
              << std::format(fmt, std::forward<Args>(args)...) << '\n';
}

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

inline int createUDPIpv4Socket() {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd == -1)
        throw std::runtime_error(std::string("socket: ") + strerror(errno));
    return fd;
}

inline int createTCPIpv4Socket() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1)
        throw std::runtime_error(std::string("socket: ") + strerror(errno));
    return fd;
}

inline struct sockaddr_in createIPv4Address(const char* ip, int port) {
    struct sockaddr_in address = {};

    address.sin_family = AF_INET;
    address.sin_port   = htons(port);

    if (ip == nullptr || ip[0] == '\0')
        address.sin_addr.s_addr = INADDR_ANY;
    else
        inet_pton(AF_INET, ip, &address.sin_addr);

    return address;
}

struct TrackerAddress {
    std::string host;
    std::string port;
};

inline TrackerAddress parse_tracker_url(const std::string& url) {
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

inline std::string generate_peer_id() {
    static constexpr char charset[] =
        "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

    std::mt19937 rng(std::random_device{}());
    std::string id = "-HB0002-";
    for (int i = 0; i < 12; ++i)
        id += charset[rng() % 62];
    return id;
}