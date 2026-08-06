#include "piece_manager.hpp"

PieceManager::PieceManager(const TorrentFile& torrent) :
    torrent_(torrent),
    piece_count_(torrent.pieces.size()),
    piece_length_(torrent.piece_length),
    have_(torrent.pieces.size()) {}

std::optional<BlockRequest> PieceManager::pick_block(const Bitfield& peer_has) {
    auto next_missing = [&](uint32_t index, PartialPiece& pp)
        -> std::optional<BlockRequest> {
        for (uint32_t b = 0; b < pp.blocks.size(); ++b) {
            if (pp.blocks[b].state != BlockState::Missing) continue;
            pp.blocks[b].state = BlockState::Received;   // naive: mark so we don't re-pick
            uint32_t offset = b * BLOCK_SIZE;
            uint32_t length = std::min<uint64_t>(
                BLOCK_SIZE, piece_size(torrent_, index) - offset);
            return BlockRequest{index, offset, length};
        }
        return std::nullopt;
    };

    for (auto& [index, pp] : active_)
        if (peer_has.get(index))
            if (auto r = next_missing(index, pp)) return r;

    for (uint32_t i = 0; i < piece_count_; ++i) {
        if (have_.get(i) || active_.count(i) || !peer_has.get(i)) continue;
        return next_missing(i, activate(i));
    }

    return std::nullopt;
}

std::optional<CompletedPiece> PieceManager::on_block(const Block& block) {
    if (block.piece_index >= piece_count_)          return std::nullopt;
    if (have_.get(block.piece_index))               return std::nullopt;

    auto it = active_.find(block.piece_index);
    if (it == active_.end())                        return std::nullopt;
    PartialPiece& pp = it->second;

    if (block.offset % BLOCK_SIZE != 0)             return std::nullopt;
    uint32_t b = block.offset / BLOCK_SIZE;
    if (b >= pp.blocks.size())                      return std::nullopt;
    if (block.offset + block.data.size() > pp.data.size()) return std::nullopt;

    std::memcpy(pp.data.data() + block.offset,
                block.data.data(), block.data.size());

    if (pp.blocks[b].state != BlockState::Received) ++pp.received;
    pp.blocks[b].state = BlockState::Received;

    if (pp.received < pp.blocks.size())             return std::nullopt;

    if (!verify(block.piece_index, pp.data)) {
        for (auto& s : pp.blocks) s.state = BlockState::Missing;
        pp.received = 0;
        return std::nullopt;
    }

    CompletedPiece done{block.piece_index, std::move(pp.data)};
    active_.erase(it);
    have_.set(block.piece_index);
    ++have_count_;
    return done;
}
bool PieceManager::is_complete() const {
    return have_count_ == torrent_.pieces.size();
}

PieceManager::PartialPiece& PieceManager::activate(uint32_t index) {
    PartialPiece pp;
    uint64_t len = piece_size(torrent_, index);
    pp.data.resize(len);
    pp.blocks.resize((len + BLOCK_SIZE - 1) / BLOCK_SIZE);
    return active_.emplace(index, std::move(pp)).first->second;
}

bool PieceManager::verify(uint32_t index, const std::vector<uint8_t>& data) const {
    const std::array<uint8_t, 20> digest = sha1(data.data(), data.size());
    const std::string& expected = torrent_.pieces[index];

    return expected.size() == 20 && std::memcmp(digest.data(), expected.data(), 20) == 0;
}