#pragma once
#include "tracker.hpp"
#include <cstring>
#include <netdb.h>
#include <unistd.h>

struct PeerConnection {
    int sockfd;
    Peer peer;
    bool am_choking     = true;
    bool am_interested  = false;
    bool peer_choking   = true;
    bool peer_interested = false;
};

std::vector<uint8_t> build_handshake(const TorrentFile& torrent, const std::string& peer_id) {
    std::vector<uint8_t> handshake(68);

    handshake[0] = 19;
    memcpy(handshake.data() + 1,  "BitTorrent protocol", 19);
    memset(handshake.data() + 20, 0, 8);
    memcpy(handshake.data() + 28, torrent.info_hash.data(), 20);
    memcpy(handshake.data() + 48, peer_id.data(), 20);

    return handshake;
}

int connect_to_peer(const Peer& peer) {
    addrinfo hints{}, *res;
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(peer.host.c_str(), peer.port.c_str(), &hints, &res) != 0)
        throw std::runtime_error("DNS resolution failed: " + peer.host);

    int fd = createTCPIpv4Socket();

    if (connect(fd, res->ai_addr, res->ai_addrlen) == -1) {
        freeaddrinfo(res);
        close(fd);
        throw std::runtime_error("connect failed: " + peer.host);
    }

    freeaddrinfo(res);
    return fd;
}

