#pragma once
#include "metainfo/torrent.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <random>
#include <span>
#include <unordered_map>
#include <vector>

#include "core/types.hpp"
#include "core/bitfield.hpp"

enum class BlockStatus { Incomplete, Rejected,  PieceVerified, PieceFailed};

struct BlockResult {
    BlockStatus status = BlockStatus::Incomplete;
    CompletedPiece piece{}; // filled on Verified / Failed
};

class PieceManager {
public:
    explicit PieceManager(const TorrentFile& torrent);

    // Given a peer's bitfield, return the next block to request using
    // a rarest-first algorithm, or nullopt if there's nothing we still
    // need. Pieces whose owner differs from 'tag' are skipped.
    std::optional<BlockRequest> pick_block(const Bitfield& peer_has,
                                           const std::vector<uint16_t>& availability,
                                           uint32_t tag);

    // file an arrived block; if it completes and verifies a piece,
    // return it (moved out) for the caller to write to disk, else nullopt
    BlockResult on_block(const Block& block);

    // return given-up requests to the pool: each block still Requested goes
    // back to Missing; blocks already Received, or in pieces that have since
    // completed, are left alone. Relies on every Requested block being in
    // exactly one peer's pending list (breaks with endgame mode)
    void release(std::span<const BlockRequest> reqs);

    // reserve an active piece for 'owner', or open it to everyone again
    // with nullopt. Does nothing if the piece isn't active
    void set_owner(uint32_t index, std::optional<uint32_t> owner);

    bool is_complete() const;
    bool recheck(uint32_t index, const std::vector<uint8_t>& data);

    const Bitfield& have_bitfield() const { return have_; }
    bool have_piece(uint32_t index) const { return have_.get(index); }
    uint64_t bytes_left() const { return torrent_.total_length - have_bytes_; }
    uint32_t completed()  const { return have_count_; }
    uint32_t total()      const { return piece_count_; }
    size_t   active()     const { return active_.size(); }

private:
    enum class BlockState { Missing, Requested, Received };

    struct PartialPiece {
        std::vector<uint8_t> data;
        std::vector<BlockState> states;
        uint32_t received = 0;

        // opaque tag set by the caller. When filled, pick_block()
        // only offers this piece to a caller passing the same tag
        std::optional<uint32_t> owner;
    };

    std::optional<BlockRequest> next_missing(uint32_t index, PartialPiece& pp) const;

    const TorrentFile& torrent_;
    uint32_t piece_count_;

    Bitfield have_;
    uint64_t have_bytes_ = 0;
    uint32_t have_count_ = 0;
    std::mt19937 rng_;

    std::unordered_map<uint32_t, PartialPiece> active_;

    PartialPiece& activate(uint32_t index);
    void mark_have(uint32_t index);
    bool verify(uint32_t index, const std::vector<uint8_t>& data) const;
};