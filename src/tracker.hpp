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
#include <poll.h>
#include "torrent.hpp"

constexpr uint64_t BITTORRENT_PROTOCOL = 0x41727101980;

struct TrackerSession {
    int sockfd;
    uint64_t connection_id;
    uint32_t transaction_id;
    std::string url;
};

void print_tracker_session(TrackerSession& session) {
    std::cout << "sockfd:           " << session.sockfd << '\n';
    std::cout << "connection_id:    " << session.connection_id << '\n';
    std::cout << "transaction_id:   " << session.transaction_id << '\n';
    std::cout << "url:              " << session.url << '\n';
}

std::vector<uint8_t> build_connect_request(uint32_t transaction_id) {
    std::vector<uint8_t> packet(16);

    uint64_t protocol_id = htobe64(BITTORRENT_PROTOCOL);
    uint32_t action      = htonl(0);
    uint32_t txn_id      = htonl(transaction_id);

    memcpy(packet.data(),      &protocol_id, 8);
    memcpy(packet.data() + 8,  &action,      4);
    memcpy(packet.data() + 12, &txn_id,      4);

    return packet;
}

void send_connect(const std::vector<int>& sockets, uint32_t transaction_id) {
    auto packet = build_connect_request(transaction_id);

    for(auto& fd : sockets) {
        send(fd, packet.data(), packet.size(), 0);
    }
}

std::vector<TrackerSession> recv_connect(const std::vector<int>& sockets, uint32_t transaction_id) {
    std::vector<pollfd> fds;

    for (int fd : sockets) {
        pollfd pfd;
        pfd.fd      = fd;
        pfd.events  = POLLIN;
        pfd.revents = 0;
        fds.push_back(pfd);
    }

    std::vector<TrackerSession> live;
    int timeout_ms = 5000;

    int ready = poll(fds.data(), fds.size(), timeout_ms);
    if (ready == -1)
        throw std::runtime_error(std::string("poll: ") + strerror(errno));
    if (ready == 0)
        return live;

    for (size_t i = 0; i < fds.size(); i++) {
        if (!(fds[i].revents & POLLIN))
            continue;

        uint8_t response[16];
        ssize_t n = recv(sockets[i], response, 16, 0);
        if (n < 16)
            continue;

        uint32_t action, txn_id;
        memcpy(&action,  response,     4);
        memcpy(&txn_id,  response + 4, 4);

        if (ntohl(action) != 0) continue;
        if (ntohl(txn_id) != transaction_id) continue;

        uint64_t connection_id;
        memcpy(&connection_id, response + 8, 8);

        TrackerSession session;
        session.sockfd         = sockets[i];
        session.connection_id  = be64toh(connection_id);
        session.transaction_id = transaction_id;
        live.push_back(session);
    }

    return live;
}

struct Peer {
    std::string host;
    std::string port;
};

std::vector<uint8_t> build_announce_request(TrackerSession& session, TorrentFile& torrent, std::string peer_id) {
    std::vector<uint8_t> announce_request(98);
    uint64_t conn_id = htobe64(session.connection_id);
    uint32_t action  = htonl(1);
    uint32_t txn_id  = htonl(session.transaction_id);
    uint64_t downloaded = htobe64(0);
    uint64_t left = htobe64(torrent.total_length);
    uint64_t uploaded = htobe64(0);
    uint32_t event = htonl(2);
    uint32_t ip = 0;
    uint32_t key = htonl(rand());
    int32_t num_want = htonl(-1);
    uint16_t port = htons(6881);

    memcpy(announce_request.data(),      &conn_id,    8);
    memcpy(announce_request.data() + 8,  &action,     4);
    memcpy(announce_request.data() + 12, &txn_id,     4);
    memcpy(announce_request.data() + 16, torrent.info_hash.data(), 20);
    memcpy(announce_request.data() + 36, peer_id.data(), 20);
    memcpy(announce_request.data() + 56, &downloaded, 8);
    memcpy(announce_request.data() + 64, &left,       8);
    memcpy(announce_request.data() + 72, &uploaded,   8);
    memcpy(announce_request.data() + 80, &event,      4);
    memcpy(announce_request.data() + 84, &ip,         4);
    memcpy(announce_request.data() + 88, &key,        4);
    memcpy(announce_request.data() + 92, &num_want,   4);
    memcpy(announce_request.data() + 96, &port,       2);

    return announce_request;
}

void send_announce(std::vector<TrackerSession>& sessions, TorrentFile& torrent) {
    std::string peer_id = generate_peer_id();
    for(auto& session : sessions) {
        auto packet = build_announce_request(session, torrent, peer_id);
        send(session.sockfd, packet.data(), packet.size(), 0);
    }
}

std::vector<Peer> recv_announce(std::vector<TrackerSession>& sessions) {
    std::vector<pollfd> fds;
    for (auto& session : sessions) {
        pollfd pfd;
        pfd.fd      = session.sockfd;
        pfd.events  = POLLIN;
        pfd.revents = 0;
        fds.push_back(pfd);
    }

    int ready = poll(fds.data(), fds.size(), 5000);
    if (ready == -1)
        throw std::runtime_error(std::string("poll: ") + strerror(errno));
    if (ready == 0)
        return {};

    for (size_t i = 0; i < fds.size(); i++) {
        if (!(fds[i].revents & POLLIN))
            continue;

        uint8_t response[2048];
        ssize_t n = recv(sessions[i].sockfd, response, 2048, 0);
        if (n < 20) continue;

        uint32_t action, txn_id;
        memcpy(&action, response,     4);
        memcpy(&txn_id, response + 4, 4);

        if (ntohl(action) != 1)                           continue;
        if (ntohl(txn_id) != sessions[i].transaction_id)  continue;

        std::vector<Peer> peers;
        int num_peers = (n - 20) / 6;
        for (int j = 0; j < num_peers; j++) {
            uint8_t* peer_data = response + 20 + (j * 6);
            char ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, peer_data, ip, sizeof(ip));
            uint16_t peer_port;
            memcpy(&peer_port, peer_data + 4, 2);
            peers.push_back({std::string(ip), std::to_string(ntohs(peer_port))});
        }
        return peers;
    }

    return {};
}

void print_peers(std::vector<Peer> peers) {
    std::cout << "Peer list:\n";
    for(auto& peer : peers) {
        std::cout << '\t' << peer.host << " : " << peer.port << '\n';
    }
}

int create_connected_socket(const std::string& url) {
    auto [host, port] = parse_tracker_url(url);
    
    addrinfo hints{}, *res;
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0)
        throw std::runtime_error("DNS resolution failed: " + host);

    int fd = createUDPIpv4Socket();

    if (connect(fd, res->ai_addr, res->ai_addrlen) == -1) {
        freeaddrinfo(res);
        close(fd);
        throw std::runtime_error("connect failed: " + host);
    }

    freeaddrinfo(res);
    return fd;
}

std::vector<Peer> contact_trackers(const TorrentFile& torrent) {
    for (const auto& tier : torrent.announce_list) {
        std::vector<int> sockets;

        for (const auto& url : tier) {
            if (url.substr(0, 6) != "udp://") {
                std::cerr << "skipping non-UDP: " << url << "\n";
                continue;
            }
            try {
                sockets.push_back(create_connected_socket(url));
                std::cerr << "connected: " << url << "\n";
            } catch (const std::exception& e) {
                std::cerr << "failed: " << url << " : " << e.what() << "\n";
            }
        }

        if (sockets.empty()) {
            std::cerr << "tier empty, skipping\n";
            continue;
        }

        uint32_t transaction_id = rand();

        send_connect(sockets, transaction_id);
        std::vector<TrackerSession> live = recv_connect(sockets, transaction_id);

        for (int fd : sockets) {
            bool kept = false;
            for (const auto& s : live)
                if (s.sockfd == fd) { kept = true; break; }
            if (!kept) close(fd);
        }

        if (live.empty()) {
            std::cerr << "no connect responses from tier\n";
            continue;
        }

        std::cerr << live.size() << " tracker(s) responded to connect\n";

        send_announce(live, const_cast<TorrentFile&>(torrent));
        std::vector<Peer> peers = recv_announce(live);

        for (const auto& s : live) close(s.sockfd);

        if (peers.empty()) {
            std::cerr << "no announce responses from tier\n";
            continue;
        }

        return peers;
    }

    throw std::runtime_error("All tiers failed");
}