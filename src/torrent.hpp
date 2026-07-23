#pragma once
#include "parser.hpp"
#include "utils.hpp"
#include <optional>
#include <array>
#include <vector>
#include <string>
#include <cstdint>

struct FileInfo {
    int64_t length;
    std::vector<std::string> path;
};

struct TorrentFile {

    // tracker
    std::string announce;
    std::vector<std::vector<std::string>> announce_list;
    std::vector<std::string> url_list;

    // informational fields
    std::optional<std::string> comment;
    std::optional<std::string> created_by;
    std::optional<int64_t>     creation_date;
    std::optional<std::string> encoding;

    // info dict
    std::string name;
    int64_t piece_length = 0;
    std::vector<std::string> pieces;
    std::array<uint8_t, 20> info_hash{};

    // single-file
    std::optional<int64_t> length;

    // multi-file
    std::optional<std::vector<FileInfo>> files;

    uint64_t total_length = 0;
};

bool is_multifile(const TorrentFile& t);
std::vector<std::string> generate_pieces(const std::string& raw);
uint64_t calculate_total_length(const TorrentFile& t);
uint64_t piece_size(const TorrentFile& t, size_t piece_index);
TorrentFile parse_torrent(const std::string& data);
void print_torrent(const TorrentFile& t, bool flag = false);