#pragma once
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>
#include <array>
#include <stdexcept>
#include <random>
#include <poll.h>
#include <span>

#include "bencode/torrent.hpp"
#include "bencode/utils.hpp"
#include "common.hpp"
// use TrackerAddress and parse_tracker_url from utils,
// tracker.hpp relies on them too, so they can't
// be moved until tracker_manager replaces tracker

constexpr uint64_t BITTORRENT_PROTOCOL = 0x41727101980;

enum TrackerEvent : uint8_t {
    EVENT_NONE = 0, EVENT_COMPLETED = 1, EVENT_STARTED = 2, EVENT_STOPPED = 3
};

enum Reported : uint8_t { REPORTED_NOTHING, REPORTED_STARTED, REPORTED_COMPLETED };

enum class TrackerState {
    Disconnected,   // no valid connection_id; must connect
    Connecting,     // connect sent, awaiting connection_id
    Connected,      // have connection_id, ready to announce
    Announcing,     // announce sent, awaiting response
    Idle,           // announced; next announce due at next_action
};

struct TrackerSession {
    TrackerAddress address;
    TrackerState state = TrackerState::Disconnected;

    int sockfd = -1;
    uint64_t connection_id = 0;
    uint32_t transaction_id = 0;
    uint32_t key = 0;

    // connected_at has a dual-purpose. send_connect sets it to the send time so
    // recv_connect can log the round-trip time, then recv_connect overwrites it
    // with the time the connection ID arrived, for the 60-second expiry. They
    // never conflict because the former expires the moment the latter begins
    std::chrono::steady_clock::time_point connected_at;
    std::chrono::steady_clock::time_point next_action;  // when Idle expires
    int retries = 0;

    TrackerEvent in_flight = EVENT_NONE;
    Reported     reported  = REPORTED_NOTHING;

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

    [[nodiscard]] std::vector<pollfd> build_fds() const;

    [[nodiscard]] std::vector<Peer> tick(std::span<const pollfd> pfds);
    [[nodiscard]] std::chrono::milliseconds until_next_action() const;

private:
    static std::vector<uint8_t> build_connect_request(uint32_t transaction_id);
    static std::vector<uint8_t> build_announce_request(
        const TrackerSession& t, TrackerEvent e, const std::string& peer_id,
        const std::array<uint8_t,20>& info_hash, uint16_t port, const AnnounceParams &p);

    uint32_t next_random();

    static void open_socket(TrackerSession& t);

    static void fail_backoff(TrackerSession& t);

    void send_connect(TrackerSession& tracker);
    [[nodiscard]] static bool recv_connect(TrackerSession& t);

    [[nodiscard]] TrackerEvent pending_event(const TrackerSession& t) const;

    void send_announce(TrackerSession& t);
    [[nodiscard]] bool recv_announce(TrackerSession& t, std::vector<Peer>& out);

    void advance(TrackerSession& t, std::chrono::steady_clock::time_point now);
    static void on_timeout(TrackerSession& t, std::chrono::steady_clock::time_point now);
    void drain(TrackerSession& t);

    std::vector<TrackerSession> trackers_;
    const TorrentFile& torrent_;
    std::string peer_id_;
    uint16_t listen_port_;
    uint64_t downloaded_ = 0, left_ = 0, uploaded_ = 0;
    std::vector<uint8_t> buf_ = std::vector<uint8_t>(65536);
    bool download_complete_ = false;
    std::mt19937 rng_;
    uint32_t key_;
};