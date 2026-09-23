#pragma once
#include <system_error>
#include <cerrno>
#include <cstdint>
#include <string>
#include <vector>

constexpr uint32_t BLOCK_SIZE = 16384;

struct Peer {
    std::string host;
    std::string port;
};

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

[[noreturn]] inline void throw_errno(const std::string& what, int err = errno) {
    throw std::system_error(err, std::generic_category(), what);
}