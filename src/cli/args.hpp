#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "core/types.hpp"

struct args {
    std::filesystem::path torrent_path;
    std::filesystem::path out_dir = "downloads";
    uint16_t port = 51413;
    bool use_tracker = true;
    std::vector<Peer> manual_peers;
};

args parse_args(int argc, char* argv[]);

std::string read_file(const std::filesystem::path& p);

inline constexpr const char* kUsage =
    "usage: torrent-peer <file.torrent> [download-dir] "
    "[--port N] [--peer host:port] [--no-tracker] "
    "[--log-level trace|debug|info|warn|error|off]\n";