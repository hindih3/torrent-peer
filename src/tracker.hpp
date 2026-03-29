#pragma once
#include <string>
#include <vector>
#include <stdexcept>
#include <cstring>
#include <cstdint>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include "torrent.hpp"

constexpr uint64_t BITTORRENT_PROTOCOL = 0x41727101980;

std::vector<uint8_t> build_connect_request(uint32_t transaction_id) {
    std::vector<uint8_t> packet(16);
    
    uint64_t protocol_id = htobe64(0x41727101980);
    uint32_t action      = htonl(0);
    uint32_t txn_id      = htonl(transaction_id);

    memcpy(packet.data(),      &protocol_id, 8);
    memcpy(packet.data() + 8,  &action,      4);
    memcpy(packet.data() + 12, &txn_id,      4);

    return packet;
}

uint64_t send_connect(int sockfd, uint32_t transaction_id) {
    auto packet = build_connect_request(transaction_id);

    if (send(sockfd, packet.data(), packet.size(), 0) == -1)
        throw std::runtime_error(std::string("send: ") + strerror(errno));

    uint8_t response[16];
    if (recv(sockfd, response, 16, 0) == -1)
        throw std::runtime_error(std::string("recv: ") + strerror(errno));

    uint32_t action, txn_id;
    memcpy(&action,  response,     4);
    memcpy(&txn_id,  response + 4, 4);

    if (ntohl(action) != 0)
        throw std::runtime_error("Unexpected action in connect response");
    if (ntohl(txn_id) != transaction_id)
        throw std::runtime_error("Transaction ID mismatch");

    uint64_t connection_id;
    memcpy(&connection_id, response + 8, 8);
    return be64toh(connection_id);
}

std::vector<Peer> announce(int sockfd, uint64_t connection_id,
uint32_t transaction_id, const TorrentFile& torrent) {
    std::vector<uint8_t> announce_request(98);

    uint64_t conn_id = htobe64(connection_id);
    uint32_t action  = htonl(1);
    uint32_t txn_id  = htonl(transaction_id);
    std::string peer_id = generate_peer_id();
    uint64_t downloaded = htobe64(0);
    uint64_t left = htobe64(torrent.total_length);
    uint64_t uploaded = htobe64(0);
    uint32_t event = htonl(2);
    uint32_t ip = 0;
    uint32_t key = htonl(rand());
    int32_t num_want = htonl(-1);
    uint16_t port = htons(6881);

    memcpy(announce_request.data()     , &conn_id, 8);
    memcpy(announce_request.data() +  8, &action,  4);
    memcpy(announce_request.data() + 12, &txn_id,  4);
    memcpy(announce_request.data() + 16, torrent.info_hash.data(), 20);
    memcpy(announce_request.data() + 36, peer_id.data(), 20);
    memcpy(announce_request.data() + 56, &downloaded, 8);
    memcpy(announce_request.data() + 64, &left, 8);
    memcpy(announce_request.data() + 72, &uploaded, 8);
    memcpy(announce_request.data() + 80, &event, 4);
    memcpy(announce_request.data() + 84, &ip, 4);
    memcpy(announce_request.data() + 88, &key, 4);
    memcpy(announce_request.data() + 92, &num_want, 4);
    memcpy(announce_request.data() + 96, &port, 2);

    if (send(sockfd, announce_request.data(), announce_request.size(), 0) == -1)
        throw std::runtime_error(std::string("send: ") + strerror(errno));

    uint8_t response[2048];
    int response_len = recv(sockfd, response, sizeof(response), 0);
    if (response_len == -1)
        throw std::runtime_error(std::string("recv: ") + strerror(errno));

    uint32_t resp_action, resp_txn_id;
    memcpy(&resp_action, response,     4);
    memcpy(&resp_txn_id, response + 4, 4);

    if (ntohl(resp_action) != 1)
        throw std::runtime_error("Unexpected action in announce response");
    if (ntohl(resp_txn_id) != transaction_id)
        throw std::runtime_error("Transaction ID mismatch");

    std::vector<Peer> peers;
    int num_peers = (response_len - 20) / 6;

    for (int i = 0; i < num_peers; ++i) {
        uint8_t* peer_data = response + 20 + (i * 6);

        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, peer_data, ip, sizeof(ip));

        uint16_t peer_port;
        memcpy(&peer_port, peer_data + 4, 2);
        peer_port = ntohs(peer_port);

        peers.push_back({std::string(ip), peer_port});
    }

    return peers;
}

std::vector<Peer> get_peers(const TorrentFile& torrent) {
    auto [host, port] = parse_tracker_url(torrent.announce);

    struct addrinfo hints = {}, *res;
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0)
        throw std::runtime_error("Could not resolve tracker host: " + host);

    int sockfd = createUDPIpv4Socket();

    if (connect(sockfd, res->ai_addr, res->ai_addrlen) == -1) {
        freeaddrinfo(res);
        throw std::runtime_error(std::string("connect: ") + strerror(errno));
    }
    freeaddrinfo(res);

    uint32_t transaction_id = rand();

    uint64_t connection_id = send_connect(sockfd, transaction_id);
    std::vector<Peer> peers = announce(sockfd, connection_id,
                                       transaction_id, torrent);

    close(sockfd);
    return peers;
}