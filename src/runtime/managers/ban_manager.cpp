#include "ban_manager.hpp"

#include <algorithm>
#include <cassert>
#include <cstring>

#include "core/log.hpp"

void BanManager::on_join(const uint32_t peer_id, const std::string& host) {
    hosts_[peer_id] = host;
}

void BanManager::on_block(const uint32_t peer_id, const Block& block) {
    // sized lazily: by the time a piece gets a verdict, every slot has been written
    auto& senders = senders_[block.piece_index];
    const uint32_t b = block.offset / kBlockSize;
    if (b >= senders.size()) senders.resize(b + 1);
    senders[b] = peer_id;
}

std::vector<std::string> BanManager::on_piece_failed(const CompletedPiece& piece) {
    // the attempt is over either way; Session picks the next owner
    retry_owner_.erase(piece.index);

    auto it = senders_.find(piece.index);
    if (it == senders_.end() || it->second.empty()) return {};
    std::vector<uint32_t> senders = std::move(it->second);
    senders_.erase(it);

    // one peer sent every block, so the bad data can only be theirs
    const uint32_t first = senders.front();
    if (std::ranges::all_of(senders, [&](uint32_t id) { return id == first; })) {
        auto h = hosts_.find(first);
        if (h == hosts_.end()) return {};
        return {h->second};
    }

    // keep the first multi-source attempt; a later one only happens when
    // a retry changed hands midway, and the next retry starts from scratch
    if (!evidence_.contains(piece.index))
        evidence_.emplace(piece.index, Evidence{piece.data, std::move(senders)});
    return {};
}

std::vector<std::string> BanManager::on_piece_verified(const CompletedPiece& piece) {
    senders_.erase(piece.index);
    retry_owner_.erase(piece.index);

    auto it = evidence_.find(piece.index);
    if (it == evidence_.end()) return {};
    const Evidence& ev = it->second;
    assert(ev.data.size() == piece.data.size());   // same piece, same length

    std::vector<std::string> bans;
    for (size_t b = 0; b < ev.senders.size(); ++b) {
        const size_t offset = b * kBlockSize;
        const size_t length = std::min<size_t>(kBlockSize, piece.data.size() - offset);
        if (std::memcmp(ev.data.data() + offset, piece.data.data() + offset, length) == 0)
            continue;

        auto h = hosts_.find(ev.senders[b]);
        if (h == hosts_.end()) continue;
        if (std::ranges::find(bans, h->second) == bans.end())
            bans.push_back(h->second);
    }

    if (bans.empty())
        log(LogLevel::Warn, "piece {} verified, but every block of its failed attempt matches",
            piece.index);

    evidence_.erase(it);
    return bans;
}

void BanManager::set_retry_owner(const uint32_t index, const uint32_t peer_id) {
    retry_owner_[index] = peer_id;
}

std::vector<uint32_t> BanManager::release_retries(const uint32_t peer_id) {
    std::vector<uint32_t> released;
    for (auto it = retry_owner_.begin(); it != retry_owner_.end();) {
        if (it->second == peer_id) {
            released.push_back(it->first);
            it = retry_owner_.erase(it);
        } else {
            ++it;
        }
    }
    return released;
}