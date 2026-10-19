#pragma once
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

// Helpers shared by more than one test.

// --- temporary directory -----------------------------------------------------

// A fresh directory under /tmp, removed with everything in it at scope exit.
struct TempDir {
    std::filesystem::path path;

    TempDir() {
        std::string tmpl = (std::filesystem::temp_directory_path() / "torrent-peer-test.XXXXXX").string();
        if (!::mkdtemp(tmpl.data())) throw std::runtime_error("mkdtemp failed");
        path = tmpl;
    }
    ~TempDir() { std::filesystem::remove_all(path); }

    TempDir(const TempDir&)            = delete;
    TempDir& operator=(const TempDir&) = delete;
};

// --- bencode builders --------------------------------------------------------

using Entries = std::map<std::string, std::string>;

inline std::string str(const std::string& s) { return std::to_string(s.size()) + ":" + s; }
inline std::string num(const int64_t v)      { return "i" + std::to_string(v) + "e"; }

inline std::string list(const std::vector<std::string>& encoded) {
    std::string out = "l";
    for (const auto& e : encoded) out += e;
    return out + "e";
}

inline std::string strings(const std::vector<std::string>& raw) {
    std::vector<std::string> encoded;
    for (const auto& s : raw) encoded.push_back(str(s));
    return list(encoded);
}

inline std::string dict(const Entries& d) {
    std::string out = "d";
    for (const auto& [k, v] : d) out += str(k) + v;
    return out + "e";
}
