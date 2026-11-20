#pragma once
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>
#include <sys/socket.h>

#include "core/log.hpp"
#include "core/types.hpp"
#include "core/unique_fd.hpp"
#include "metainfo/torrent.hpp"

inline constexpr size_t kHandshakeSize = 68;
inline constexpr auto   kHandshakeTimeout = std::chrono::seconds(15);

enum class HandshakeResult { Keep, Drop, Complete };

struct HandshakeContext {
    const TorrentFile& torrent;
    const std::string& peer_id;
    const std::unordered_set<std::string>& banned;
};

struct CompletedHandshake {
    UniqueFd sock;
    Peer peer;
};

inline std::vector<uint8_t> build_handshake(const HandshakeContext& ctx) {
    if (ctx.peer_id.size() != 20)
        throw std::runtime_error("peer_id must be exactly 20 bytes");

    std::vector<uint8_t> handshake(kHandshakeSize);

    handshake[0] = 19;
    memcpy(handshake.data() + 1,  "BitTorrent protocol", 19);
    memset(handshake.data() + 20, 0, 8);
    memcpy(handshake.data() + 28, ctx.torrent.info_hash.data(), 20);
    memcpy(handshake.data() + 48, ctx.peer_id.data(), 20);

    return handshake;
}

// recv() is capped at exactly what is missing so that anything the peer
// pipelined behind the handshake (usually its bitfield) stays in the kernel
// buffer and is picked up by the normal read path on the next poll.
inline HandshakeResult read_handshake(const int fd, std::array<uint8_t, kHandshakeSize>& buffer,
                                      size_t& received) {
    const ssize_t n = ::recv(fd, buffer.data() + received, kHandshakeSize - received, 0);

    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            return HandshakeResult::Keep;
        return HandshakeResult::Drop;
    }
    if (n == 0) return HandshakeResult::Drop;   // peer hung up

    received += static_cast<size_t>(n);
    return received < kHandshakeSize ? HandshakeResult::Keep : HandshakeResult::Complete;
}

[[nodiscard]] inline bool verify_handshake(const HandshakeContext& ctx,
                                           const std::array<uint8_t, kHandshakeSize>& hs,
                                           const Peer& peer) {
    // catches connections that were already mid-handshake when their host got banned
    if (ctx.banned.contains(peer.host)) {
        log(LogLevel::Debug, "{}: banned, dropping", peer.host);
        return false;
    }
    if (hs[0] != 19 || std::memcmp(hs.data() + 1, "BitTorrent protocol", 19) != 0) {
        log(LogLevel::Debug, "{}: bad protocol header", peer.host);
        return false;
    }
    if (std::memcmp(hs.data() + 28, ctx.torrent.info_hash.data(), 20) != 0) {
        log(LogLevel::Debug, "{}: info hash mismatch", peer.host);
        return false;
    }
    // Trackers hand our own address back to us; without this we'd connect to ourselves.
    if (std::memcmp(hs.data() + 48, ctx.peer_id.data(), 20) == 0) {
        log(LogLevel::Debug, "{}: that is us, dropping", peer.host);
        return false;
    }
    return true;
}