#pragma once
#include "tracker.hpp"
#include <vector>
#include <string>
#include <cstdint>

struct PeerSocket {
    int sockfd;
    Peer peer;
};

struct PeerConnection {
    int sockfd;
    Peer peer;
    bool am_choking      = true;
    bool am_interested   = false;
    bool peer_choking    = true;
    bool peer_interested = false;

    std::vector<bool> piece_array;
    std::vector<uint8_t> recv_buffer;
};

std::vector<uint8_t> build_handshake(const TorrentFile& torrent,
                                     const std::string& peer_id);

std::vector<PeerSocket> tcp_connect_peers(const std::vector<Peer>& peers);

std::vector<PeerConnection> handshake_peers(const std::vector<PeerSocket>& sockets,
                                            const TorrentFile& torrent,
                                            const std::string& peer_id);

