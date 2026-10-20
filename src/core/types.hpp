#pragma once
#include <system_error>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <string>
#include <vector>

constexpr uint32_t kBlockSize = 16384;

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

struct PendingRequest {
    BlockRequest req{};
    std::chrono::steady_clock::time_point sent_at;
};

// pending is in send order, so the stale entries are always a prefix
inline std::vector<BlockRequest> take_stale(std::vector<PendingRequest>& pending,
                                            std::chrono::steady_clock::time_point cutoff) {
    const auto fresh = std::ranges::find_if(pending, [&](const PendingRequest& p) {
        return p.sent_at >= cutoff;
    });
    std::vector<BlockRequest> stale;
    for (auto it = pending.begin(); it != fresh; ++it) stale.push_back(it->req);
    pending.erase(pending.begin(), fresh);
    return stale;
}

[[noreturn]] inline void throw_errno(const std::string& what, int err = errno) {
    throw std::system_error(err, std::generic_category(), what);
}