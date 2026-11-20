#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/types.hpp"

// decides which peers to ban for sending bad data. Pure bookkeeping:
// Session reports what happened, and the methods return what Session
// should act on
//
// a failed piece is kept as evidence and retried from one peer at a
// time until a retry passes. A failed retry had a single source, so
// that peer is banned and the next peer tries. A passing retry is
// compared against the evidence block by block, and every peer that
// sent a mismatching block is banned
class BanManager {
public:
    // a peer finished its handshake; its host is kept so it can
    // still be banned after it disconnects
    void on_join(uint32_t peer_id, const std::string& host);

    // record who sent a block PieceManager accepted (anything but
    // Rejected). Call before handling the piece's verdict: the block
    // that completes a piece is one of its senders
    void on_block(uint32_t peer_id, const Block& block);

    // the piece failed its hash check. If one peer sent every block,
    // return its host; otherwise keep the piece as evidence
    std::vector<std::string> on_piece_failed(const CompletedPiece& piece);

    // the piece verified. Compare it against the evidence, return the
    // hosts that sent a mismatching block, then forget the piece
    std::vector<std::string> on_piece_verified(const CompletedPiece& piece);

    // Session picked 'peer_id' to download piece 'index' on its own
    void set_retry_owner(uint32_t index, uint32_t peer_id);

    // the peer can no longer retry (dropped, choked, banned): return the
    // pieces it was retrying and forget them, for Session to reassign
    std::vector<uint32_t> release_retries(uint32_t peer_id);

private:
    struct Evidence {
        std::vector<uint8_t>  data;     // the bytes that failed the hash
        std::vector<uint32_t> senders;  // peer id per block
    };

    std::unordered_map<uint32_t, std::string>           hosts_;        // peer id -> host
    std::unordered_map<uint32_t, std::vector<uint32_t>> senders_;      // active piece -> peer id per block
    std::unordered_map<uint32_t, Evidence>              evidence_;     // piece -> its failed attempt
    std::unordered_map<uint32_t, uint32_t>              retry_owner_;  // piece -> peer retrying it
};