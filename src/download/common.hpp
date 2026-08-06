#pragma once
#include <cstdint>
#include <unistd.h>
#include <vector>

constexpr uint32_t BLOCK_SIZE = 16384;

struct BlockRequest {
    uint32_t piece_index;
    uint32_t offset;
    uint32_t length;
};

struct Block {
    uint32_t piece_index;
    uint32_t offset;
    std::vector<uint8_t> data;
};

struct CompletedPiece {
    uint32_t index;
    std::vector<uint8_t> data;
};

struct File {
    int fd = -1;

    File() = default;
    explicit File(int f) : fd(f) {}
    ~File() { if (fd >= 0) ::close(fd); }

    File(const File&)            = delete;
    File& operator=(const File&) = delete;

    File(File&& o) noexcept : fd(o.fd) { o.fd = -1; }
    File& operator=(File&& o) noexcept {
        if (this != &o) { if (fd >= 0) ::close(fd); fd = o.fd; o.fd = -1; }
        return *this;
    }
};