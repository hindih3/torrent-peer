#pragma once
#include <format>
#include <random>
#include <string>

// Azureus-style peer id (BEP 20): "-TP" + one digit each of major, minor,
// patch + "0", then 12 random characters. "-TP0100-..." is version 0.1.0.
static_assert(TP_VERSION_MAJOR < 10 && TP_VERSION_MINOR < 10 && TP_VERSION_PATCH < 10,
              "peer id encodes each version component as a single digit");

inline std::string generate_peer_id() {
    static constexpr char kCharset[] =
        "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<size_t> pick(0, sizeof(kCharset) - 2);

    std::string id = std::format("-TP{}{}{}0-", TP_VERSION_MAJOR, TP_VERSION_MINOR, TP_VERSION_PATCH);
    while (id.size() < 20)
        id += kCharset[pick(rng)];
    return id;
}