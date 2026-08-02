#pragma once
#include "torrent.hpp"
#include <string>
#include <vector>
#include <cstdint>

constexpr uint64_t BITTORRENT_PROTOCOL = 0x41727101980;

struct TrackerCandidate {
    int sockfd;
    std::string url;
};

struct TrackerSession {
    int sockfd;
    uint64_t connection_id;
    uint32_t transaction_id;
    std::string url;
};

struct Peer {
    std::string host;
    std::string port;
};

void print_tracker_session(const TrackerSession& session);

std::vector<uint8_t> build_connect_request(uint32_t transaction_id);

std::vector<TrackerSession> connect_trackers(const std::vector<TrackerCandidate>& candidates,
                                          uint32_t transaction_id);

std::vector<uint8_t> build_announce_request(const TrackerSession& session,
                                             const TorrentFile& torrent,
                                             const std::string& peer_id);
std::vector<Peer> parse_peers(const uint8_t* response, ssize_t n);

std::vector<Peer>  announce(const std::vector<TrackerSession>& sessions,
                    const TorrentFile& torrent,
                    const std::string& peer_id);

TrackerCandidate create_connected_socket(const std::string& url);

std::vector<Peer> contact_trackers(const TorrentFile& torrent,
                                    const std::string& peer_id);