#pragma once
#include <string>
#include <vector>
#include <stdexcept>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include "tracker.hpp"

int connect_to_peer(const Peer& peer) {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd == -1)
        throw std::runtime_error(std::string("socket: ") + strerror(errno));

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(peer.port);
    inet_pton(AF_INET, peer.ip.c_str(), &addr.sin_addr);

    if (connect(sockfd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
        close(sockfd);
        throw std::runtime_error(std::string("connect: ") + strerror(errno));
    }

    return sockfd;
}

void send_handshake(int sockfd, const TorrentFile& torrent) {
    std::vector<uint8_t> handshake(68);

    std::string protocol = "BitTorrent protocol";

    handshake[0] = 0x13;
    memcpy(handshake.data() + 1,  protocol.data(), 19);
    memset(handshake.data() + 20, 0, 8);
    memcpy(handshake.data() + 28, torrent.info_hash.data(), 20);
    memcpy(handshake.data() + 48, torrent.peer_id.data(), 20);

    if (send(sockfd, handshake.data(), handshake.size(), 0) == -1)
        throw std::runtime_error(std::string("send: ") + strerror(errno));
}

void receive_handshake(int sockfd, const TorrentFile& torrent) {
    uint8_t response[68];
    int received = recv(sockfd, response, 68, 0);

    if (received != 68)
        throw std::runtime_error("Invalid handshake response");

    if (response[0] != 0x13)
        throw std::runtime_error("Invalid protocol length");

    std::string protocol(reinterpret_cast<char*>(response + 1), 19);
    if (protocol != "BitTorrent protocol")
        throw std::runtime_error("Invalid protocol string");

    if (memcmp(response + 28, torrent.info_hash.data(), 20) != 0)
        throw std::runtime_error("Info hash mismatch");

    std::cout << "Handshake successful!\n";
}

void do_handshake(const Peer& peer, const TorrentFile& torrent) {
    int sockfd = connect_to_peer(peer);
    send_handshake(sockfd, torrent);
    receive_handshake(sockfd, torrent);
    close(sockfd);
}