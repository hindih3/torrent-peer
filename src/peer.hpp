#pragma once
#include <string>
#include <vector>
#include <stdexcept>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/select.h>
#include "tracker.hpp"

// REDUNDANT. USE CONNECT_WITH_TIMEOUT
int connect_to_peer(const Peer& peer) {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd == -1)
        throw std::runtime_error(std::string("socket: ") + strerror(errno));

    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

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

int connect_with_timeout(const Peer& peer, int timeout_ms) {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd == -1) throw std::runtime_error("socket error");

    int flags = fcntl(sockfd, F_GETFL, 0);
    fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(peer.port);
    inet_pton(AF_INET, peer.ip.c_str(), &addr.sin_addr);

    int res = connect(sockfd, (struct sockaddr*)&addr, sizeof(addr));
    if (res == 0) {
        fcntl(sockfd, F_SETFL, flags);
        return sockfd;
    } else if (errno != EINPROGRESS) {
        close(sockfd);
        throw std::runtime_error("connect error");
    }

    fd_set writefds;
    FD_ZERO(&writefds);
    FD_SET(sockfd, &writefds);

    struct timeval tv;
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    res = select(sockfd + 1, nullptr, &writefds, nullptr, &tv);
    if (res <= 0) {
        close(sockfd);
        throw std::runtime_error("connect timed out");
    }

    int so_error;
    socklen_t len = sizeof(so_error);
    getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &so_error, &len);
    if (so_error != 0) {
        close(sockfd);
        throw std::runtime_error("connect failed");
    }

    fcntl(sockfd, F_SETFL, flags);
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

void recv_all(int sockfd, uint8_t* buffer, size_t len) {
    size_t total = 0;
    while (total < len) {
        int n = recv(sockfd, buffer + total, len - total, 0);
        if (n <= 0)
            throw std::runtime_error("recv failed");
        total += n;
    }
}

void receive_handshake(int sockfd, const TorrentFile& torrent) {
    uint8_t response[68];

    recv_all(sockfd, response, 68);

    // DEBUG
    std::cout << "Handshake bytes: ";
    for (int i = 0; i < 68; i++)
        printf("%02x ", response[i]);
    std::cout << "\n";

    if (response[0] != 0x13)
        throw std::runtime_error("Invalid protocol length");

    std::string protocol(reinterpret_cast<char*>(response + 1), 19);
    if (protocol != "BitTorrent protocol")
        throw std::runtime_error("Invalid protocol string");

    if (memcmp(response + 28, torrent.info_hash.data(), 20) != 0)
        throw std::runtime_error("Info hash mismatch");

    // DEBUG
    std::cout << "Peer ID: " << std::string((char*)response + 48, 20) << "\n";

    std::cout << "Handshake successful!\n";
}

bool do_handshake(const Peer& peer, const TorrentFile& torrent) {
    try {
        int sockfd = connect_with_timeout(peer, 1500);
        send_handshake(sockfd, torrent);
        receive_handshake(sockfd, torrent);
        close(sockfd);
        return true;
    } catch (const std::exception& e) {
        std::cerr << peer.ip << ":" << peer.port 
                  << " failed: " << e.what() << "\n";
        return false;
    }
}