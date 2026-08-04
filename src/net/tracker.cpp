#include "tracker.hpp"
#include <chrono>
#include <stdexcept>
#include <cstring>
#include <iostream>
#include <endian.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <poll.h>
#include <unordered_map>
#include <algorithm>

void print_tracker_session(const TrackerSession& session) {
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

std::vector<TrackerSession> connect_trackers(const std::vector<TrackerCandidate>& candidates,
                                          const uint32_t transaction_id) {

    auto packet = build_connect_request(transaction_id);

    for (const auto&[sockfd, url] : candidates)
        send(sockfd, packet.data(), packet.size(), 0);

    std::vector<TrackerSession> connected_trackers;

    std::vector<pollfd> fds;
    for (const TrackerCandidate& candidate : candidates) {
        pollfd pfd{};
        pfd.fd     = candidate.sockfd;
        pfd.events = POLLIN;
        fds.push_back(pfd);
    }

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    size_t responded = 0;
    while (responded < candidates.size()) {

        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) break;
        int timeout_ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();

        int ready = poll(fds.data(), fds.size(), timeout_ms);

        if (ready == 0) break;
        if (ready == -1) throw std::runtime_error(std::string("poll: ") + strerror(errno));

        for (size_t i = 0; i < fds.size(); i++) {
            if (!(fds[i].revents & POLLIN))
                continue;

            uint8_t response[16];
            ssize_t n = recv(candidates[i].sockfd, response, 16, 0);
            if (n < 16)
                continue;


            uint32_t action, txn_id;
            memcpy(&action, response,     4);
            memcpy(&txn_id, response + 4, 4);

            if (ntohl(action) != 0)              continue;
            if (ntohl(txn_id) != transaction_id) continue;

            uint64_t connection_id;
            memcpy(&connection_id, response + 8, 8);

            TrackerSession session;
            session.sockfd = candidates[i].sockfd;
            session.connection_id  = be64toh(connection_id);
            session.transaction_id = transaction_id;
            session.url = candidates[i].url;

            connected_trackers.push_back(session);

            fds[i].fd = -1;
            responded++;
        }
    }
    return connected_trackers;
}

std::vector<uint8_t> build_announce_request(const TrackerSession& session,
                                             const TorrentFile& torrent,
                                             const std::string& peer_id) {
    if (peer_id.size() != 20)
        throw std::runtime_error("peer_id must be exactly 20 bytes");

    std::vector<uint8_t> announce_request(98);

    uint64_t conn_id    = htobe64(session.connection_id);
    uint32_t action     = htonl(1);
    uint32_t txn_id     = htonl(session.transaction_id);
    uint64_t downloaded = htobe64(0);
    uint64_t left       = htobe64(torrent.total_length);
    uint64_t uploaded   = htobe64(0);
    uint32_t event      = htonl(2);
    uint32_t ip         = 0;
    uint32_t key        = htonl(rand());
    int32_t  num_want   = htonl(-1);
    uint16_t port       = htons(6881);

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

std::vector<Peer> parse_peers(const uint8_t* response, ssize_t n) {
    std::vector<Peer> out;
    int num_peers = (n - 20) / 6;
    for (int j = 0; j < num_peers; j++) {
        const uint8_t* peer_data = response + 20 + j * 6;

        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, peer_data, ip, sizeof(ip));

        uint16_t peer_port;
        memcpy(&peer_port, peer_data + 4, 2);

        out.push_back({ std::string(ip), std::to_string(ntohs(peer_port)) });
    }
    return out;
}

std::vector<Peer> announce(const std::vector<TrackerSession>& sessions,
                           const TorrentFile& torrent,
                           const std::string& peer_id) {
    std::unordered_map<std::string, Peer> peers;

    for (const auto& session : sessions) {
        auto packet = build_announce_request(session, torrent, peer_id);
        send(session.sockfd, packet.data(), packet.size(), 0);
    }

    std::vector<pollfd> fds;
    for (const TrackerSession& session : sessions) {
        pollfd pfd{};
        pfd.fd     = session.sockfd;
        pfd.events = POLLIN;
        fds.push_back(pfd);
    }

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    size_t responded = 0;

    while (responded < sessions.size()) {
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) break;

        int timeout_ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();

        int ready = poll(fds.data(), fds.size(), timeout_ms);
        if (ready == 0) break;
        if (ready == -1) throw std::runtime_error(std::string("poll: ") + strerror(errno));

        for (size_t i = 0; i < fds.size(); i++) {
            if (!(fds[i].revents & POLLIN))
                continue;

            uint8_t response[2048];
            ssize_t n = recv(sessions[i].sockfd, response, sizeof(response), 0);
            if (n < 20) continue;

            uint32_t action, txn_id;
            memcpy(&action, response,     4);
            memcpy(&txn_id, response + 4, 4);

            if (ntohl(action) != 1)                           continue;
            if (ntohl(txn_id) != sessions[i].transaction_id)  continue;

            std::vector<Peer> tracker_peers = parse_peers(response, n);
            std::cerr << sessions[i].url << ": " << tracker_peers.size() << " peers\n";

            for (const Peer& p : parse_peers(response, n))
                peers[p.host + ":" + p.port] = p;

            fds[i].fd = -1;
            responded++;
        }
    }

    std::vector<Peer> peer_pool;
    peer_pool.reserve(peers.size());
    std::transform(peers.begin(), peers.end(), std::back_inserter(peer_pool),
                   [](const auto& peer) { return peer.second; });
    return peer_pool;
}

TrackerCandidate create_connected_socket(const std::string& url) {
    auto [host, port] = parse_tracker_url(url);   // throws on non-UDP

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
    return { .sockfd = fd, .url = url };
}

std::vector<Peer> contact_trackers(const TorrentFile& torrent,
                                   const std::string& peer_id) {
    if (peer_id.size() != 20)
        throw std::runtime_error("peer_id must be exactly 20 bytes");

    for (const auto& tier : torrent.announce_list) {
        std::vector<TrackerCandidate> candidates;

        for (const auto& url : tier) {
            try {
                candidates.push_back(create_connected_socket(url));
                std::cerr << "connected: " << url << "\n";
            } catch (const std::exception& e) {
                std::cerr << "failed: " << url << " : " << e.what() << "\n";
            }
        }

        if (candidates.empty()) {
            std::cerr << "tier empty, skipping\n";
            continue;
        }

        uint32_t transaction_id = rand();

        std::vector<TrackerSession> sessions = connect_trackers(candidates, transaction_id);

        for (const auto& c : candidates) {
            bool kept = false;
            for (const auto& s : sessions)
                if (s.sockfd == c.sockfd) { kept = true; break; }
            if (!kept) close(c.sockfd);
        }

        if (sessions.empty()) {
            std::cerr << "no connect responses from tier\n";
            continue;
        }

        std::cerr << sessions.size() << " tracker(s) responded to connect\n";

        std::vector<Peer> peers = announce(sessions, torrent, peer_id);

        for (const auto& s : sessions)
            close(s.sockfd);

        if (peers.empty()) {
            std::cerr << "no announce responses from tier\n";
            continue;
        }

        return peers;
    }

    throw std::runtime_error("All tiers failed");
}