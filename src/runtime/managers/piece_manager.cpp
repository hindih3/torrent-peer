#include "piece_manager.hpp"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <limits>
#include "core/sha1.hpp"

PieceManager::PieceManager(const TorrentFile& torrent) :
    torrent_(torrent),
    piece_count_(static_cast<uint32_t>(torrent.pieces.size())),
    have_(static_cast<uint32_t>(torrent.pieces.size())),
    rng_(std::random_device{}()) {}


std::optional<BlockRequest>
PieceManager::pick_block(const Bitfield& peer_has,
                         const std::vector<uint16_t>& availability) {
    // finish in-progress pieces first
    for (auto& [index, pp] : active_)
        if (peer_has.get(index))
            if (auto r = next_missing(index, pp)) return r;

    // scan all eligible new pieces, tracking the rarest and collecting ties
    uint16_t best = std::numeric_limits<uint16_t>::max();
    std::vector<uint32_t> candidates;

    for (uint32_t i = 0; i < piece_count_; ++i) {
        if (have_.get(i) || active_.contains(i) || !peer_has.get(i)) continue;

        const uint16_t avail = availability[i];
        if (avail < best) {
            best = avail;
            candidates.clear();
        }
        if (avail == best)
            candidates.push_back(i); // tie
    }

    if (candidates.empty()) return std::nullopt;

    uint32_t index = candidates[rng_() % candidates.size()];
    return next_missing(index, activate(index));
}

BlockResult PieceManager::on_block(const Block& block) {
    if (block.piece_index >= piece_count_)          return {BlockStatus::Rejected};
    if (have_.get(block.piece_index))               return {BlockStatus::Rejected};

    auto it = active_.find(block.piece_index);
    if (it == active_.end())                        return {BlockStatus::Rejected};
    PartialPiece& pp = it->second;

    if (block.offset % kBlockSize != 0)             return {BlockStatus::Rejected};
    uint32_t b = block.offset / kBlockSize;
    if (b >= pp.states.size())                      return {BlockStatus::Rejected};
    if (block.offset + block.data.size() > pp.data.size()) return {BlockStatus::Rejected};

    std::memcpy(pp.data.data() + block.offset,
                block.data.data(), block.data.size());

    // Guard against a duplicate delivery double-counting the block.
    if (pp.states[b] != BlockState::Received) ++pp.received;
    pp.states[b] = BlockState::Received;

    if (pp.received < pp.states.size())             return {BlockStatus::Incomplete};

    if (!verify(block.piece_index, pp.data)) {
        for (auto& s : pp.states) s = BlockState::Missing;
        pp.received = 0;
        // copied, not moved: pp.data stays allocated for the retry
        return {BlockStatus::PieceFailed, {block.piece_index, pp.data}};
    }

    CompletedPiece done{block.piece_index, std::move(pp.data)};
    active_.erase(it);
    mark_have(block.piece_index);
    return {BlockStatus::PieceVerified, std::move(done)};
}

void PieceManager::release(std::span<const BlockRequest> reqs) {
    for (const auto& req : reqs) {
        auto it = active_.find(req.piece_index);
        if (it == active_.end()) continue;           // piece completed since

        PartialPiece& pp = it->second;
        const uint32_t b = req.offset / kBlockSize;
        assert(req.offset % kBlockSize == 0);        // verifying our own request
        if (b >= pp.states.size()) continue;

        if (pp.states[b] == BlockState::Requested)
            pp.states[b] = BlockState::Missing;
    }
}

std::optional<BlockRequest> PieceManager::next_missing(uint32_t index, PartialPiece& pp) const {
    for (uint32_t b = 0; b < pp.states.size(); ++b) {
        if (pp.states[b] != BlockState::Missing) continue;
        pp.states[b] = BlockState::Requested;

        uint32_t offset = b * kBlockSize;
        uint32_t length = static_cast<uint32_t>(std::min<uint64_t>(
            kBlockSize, piece_size(torrent_, index) - offset));
        return BlockRequest{.piece_index = index, .offset = offset, .length = length};
    }
    return std::nullopt;
}

bool PieceManager::is_complete() const {
    return have_count_ == piece_count_;
}

bool PieceManager::recheck(const uint32_t index, const std::vector<uint8_t>& data) {
    if (!verify(index, data)) return false;
    mark_have(index);
    return true;
}

void PieceManager::mark_have(const uint32_t index) {
    have_.set(index);
    ++have_count_;
    have_bytes_ += piece_size(torrent_, index);
}

PieceManager::PartialPiece& PieceManager::activate(uint32_t index) {
    PartialPiece pp;
    const uint64_t len = piece_size(torrent_, index);
    pp.data.resize(len);
    pp.states.resize((len + kBlockSize - 1) / kBlockSize);
    return active_.emplace(index, std::move(pp)).first->second;
}

bool PieceManager::verify(const uint32_t index, const std::vector<uint8_t>& data) const {
    const std::array<uint8_t, 20> digest = sha1(data.data(), data.size());
    const std::string& expected = torrent_.pieces[index];

    return expected.size() == 20 && std::memcmp(digest.data(), expected.data(), 20) == 0;
}