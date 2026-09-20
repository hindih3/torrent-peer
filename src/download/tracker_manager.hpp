#pragma once
#include <chrono>
#include <cstdint>
#include <string>

#include "bencode/torrent.hpp"

constexpr uint64_t BITTORRENT_PROTOCOL = 0x41727101980;

enum class TrackerState {
    Disconnected,   // no valid connection_id; must connect
    Connecting,     // connect sent, awaiting connection_id
    Connected,      // have connection_id, ready to announce
    Announcing,     // announce sent, awaiting response
    Idle,           // announced; next announce due at next_announce
};

struct TrackerSession {
    std::string url;
    TrackerState state = TrackerState::Disconnected;

    int sockfd = -1;
    uint64_t connection_id = 0;
    uint32_t transaction_id = 0;
    std::chrono::steady_clock::time_point connected_at;  // for the 60s expiry check
    std::chrono::steady_clock::time_point next_announce;  // when Idle expires
    int retries = 0;

    uint32_t interval = 0;
    uint32_t seeders  = 0;
    uint32_t leechers = 0;
};

class TrackerManager {
public:

private:
    std::vector <TrackerSession> trackers_;
    const TorrentFile& torrent_;
    std::string peer_id_;
    uint16_t listen_port_;
    uint64_t downloaded_ = 0, uploaded_ = 0, left_ = 0;
};