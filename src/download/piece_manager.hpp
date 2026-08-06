#pragma once
#include "bencode/torrent.hpp"
#include "common.hpp"
#include <optional>
#include <unordered_map>
#include <vector>

class PieceManager {
public:
    explicit PieceManager(const TorrentFile& torrent);

    // given a peer's bitfield, return the next block to request from them,
    // or nullopt if they have nothing we still need
    std::optional<BlockRequest> pick_block(const Bitfield& peer_has);

    // file an arrived block; if it completes and verifies a piece,
    // return it (moved out) for the caller to write to disk, else nullopt
    std::optional<CompletedPiece> on_block(const Block& block);

    // whole-torrent exit condition
    bool is_complete() const;

private:
    enum class BlockState { Missing, Received };

    struct BlockSlot {
        BlockState state = BlockState::Missing;
    };

    struct PartialPiece {
        std::vector<uint8_t>   data;
        std::vector<BlockSlot> blocks;
        uint32_t               received = 0;
    };

    const TorrentFile& torrent_;
    uint32_t piece_count_;
    uint32_t piece_length_;

    Bitfield have_;
    uint32_t have_count_ = 0;

    std::unordered_map<uint32_t, PartialPiece> active_;

    PartialPiece& activate(uint32_t index);
    bool verify(uint32_t index, const std::vector<uint8_t>& data) const;
};