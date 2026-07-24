#pragma once
#include "torrent.hpp"
#include <string>
#include <vector>

constexpr uint64_t BITTORRENT_PROTOCOL = 0x41727101980;

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
void send_connect(const std::vector<int>& sockets, uint32_t transaction_id);
std::vector<TrackerSession> recv_connect(const std::vector<int>& sockets,
                                          uint32_t transaction_id);

std::vector<uint8_t> build_announce_request(const TrackerSession& session,
                                             const TorrentFile& torrent,
                                             const std::string& peer_id);
void send_announce(const std::vector<TrackerSession>& sessions,
                    const TorrentFile& torrent,
                    const std::string& peer_id);
std::vector<Peer> recv_announce(const std::vector<TrackerSession>& sessions);

int create_connected_socket(const std::string& url);

std::vector<Peer> contact_trackers(const TorrentFile& torrent,
                                    const std::string& peer_id);