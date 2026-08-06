#pragma once

#include <filesystem>
#include <vector>

#include "bencode/torrent.hpp"
#include "common.hpp"

class DiskManager {
public:

    DiskManager(const TorrentFile& torrent,
                const std::filesystem::path& download_dir = std::filesystem::current_path());

    void write_piece(const CompletedPiece& piece);

    [[nodiscard]] std::vector<uint8_t> read_block(uint32_t piece_index, uint32_t offset, uint32_t length) const;

    void sync() const;

    [[nodiscard]] uint64_t total_length() const { return total_length_; }

private:
    struct FileEntry {
        File                  file;
        std::filesystem::path path;
        uint64_t              offset = 0;
        uint64_t              length = 0;
    };

    [[nodiscard]] size_t locate(uint64_t offset) const;

    template <typename Op>
    void for_each_slice(uint64_t global_offset, uint64_t len, Op op) const;

    std::vector<FileEntry> files_;
    uint64_t               piece_length_ = 0;
    uint64_t               total_length_ = 0;
    uint32_t               piece_count_  = 0;
};