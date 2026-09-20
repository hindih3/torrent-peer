#pragma once
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>
#include <array>
#include <stdexcept>
#include <random>

#include "bencode/torrent.hpp"
#include "bencode/utils.hpp"
// use TrackerAddress and parse_tracker_url from utils,
// tracker.hpp relies on them too, so they can't
// be moved until tracker_manager replaces tracker

constexpr uint64_t BITTORRENT_PROTOCOL = 0x41727101980;

enum TrackerEvent : uint8_t {
    EVENT_NONE = 0, EVENT_COMPLETED = 1, EVENT_STARTED = 2, EVENT_STOPPED = 3
};

enum class TrackerState {
    Disconnected,   // no valid connection_id; must connect
    Connecting,     // connect sent, awaiting connection_id
    Connected,      // have connection_id, ready to announce
    Announcing,     // announce sent, awaiting response
    Idle,           // announced; next announce due at next_announce
};

struct TrackerSession {
    TrackerAddress address;
    TrackerState state = TrackerState::Disconnected;

    int sockfd = -1;
    uint64_t connection_id = 0;
    uint32_t transaction_id = 0;
    uint32_t key = 0; // TODO: seeded in future send_connect; 0 until then

    std::chrono::steady_clock::time_point connected_at;  // for the 60s expiry check
    std::chrono::steady_clock::time_point next_action;  // when Idle expires
    int retries = 0;

    uint32_t interval = 0;
    uint32_t seeders  = 0;
    uint32_t leechers = 0;
};

struct AnnounceParams {
    uint64_t downloaded;
    uint64_t uploaded;
    uint64_t left;
};

class TrackerManager {
public:
    TrackerManager(const TorrentFile& torrent,
                   std::string  peer_id, uint16_t listen_port);

private:
    static std::vector<uint8_t> build_connect_request(uint32_t transaction_id);
    static std::vector<uint8_t> build_announce_request(
        const TrackerSession& t, TrackerEvent e, const std::string& peer_id,
        const std::array<uint8_t,20>& info_hash, uint16_t port, const AnnounceParams &p);

    uint32_t next_random();

    static void open_socket(TrackerSession &t);

    static void fail_backoff(TrackerSession &t);

    void send_connect(TrackerSession &tracker);

    [[nodiscard]] static bool recv_connect(TrackerSession &t);


    std::vector <TrackerSession> trackers_;
    const TorrentFile& torrent_;
    std::string peer_id_;
    uint16_t listen_port_;
    uint64_t downloaded_ = 0, left_ = 0, uploaded_ = 0;
    std::mt19937 rng_;
};