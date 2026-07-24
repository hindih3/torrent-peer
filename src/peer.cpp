#include "peer.hpp"

#include <cstring>
#include <iostream>
#include <stdexcept>
#include <array>
#include <chrono>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>

namespace {
    struct PendingHandshake {
        int sockfd;
        Peer peer;
        std::array<uint8_t, 68> buffer;
        size_t received = 0;
    };
}

std::vector<uint8_t> build_handshake(const TorrentFile& torrent,
                                     const std::string& peer_id) {
    if (peer_id.size() != 20)
        throw std::runtime_error("peer_id must be exactly 20 bytes");

    std::vector<uint8_t> handshake(68);

    handshake[0] = 19;
    memcpy(handshake.data() + 1,  "BitTorrent protocol", 19);
    memset(handshake.data() + 20, 0, 8);
    memcpy(handshake.data() + 28, torrent.info_hash.data(), 20);
    memcpy(handshake.data() + 48, peer_id.data(), 20);

    return handshake;
}

std::vector<PeerSocket> tcp_connect_peers(const std::vector<Peer>& peers) {
    std::vector<PeerSocket> connected;

    std::vector<int>  fds;
    std::vector<Peer> attempted;

    for (const auto& peer : peers) {
        addrinfo hints{}, *res;
        hints.ai_family   = AF_INET;
        hints.ai_socktype = SOCK_STREAM;

        if (getaddrinfo(peer.host.c_str(), peer.port.c_str(), &hints, &res) != 0)
            continue;

        int fd = createTCPIpv4Socket();

        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);

        connect(fd, res->ai_addr, res->ai_addrlen);
        freeaddrinfo(res);

        fds.push_back(fd);
        attempted.push_back(peer);
    }

    std::vector<size_t> pending;
    for (size_t i = 0; i < fds.size(); i++)
        pending.push_back(i);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);

    while (!pending.empty()) {
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) break;

        int timeout_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             deadline - now).count();

        std::vector<pollfd> pfds;
        for (size_t idx : pending)
            pfds.push_back({fds[idx], POLLOUT, 0});

        int ready = poll(pfds.data(), pfds.size(), timeout_ms);
        if (ready <= 0) break;

        std::vector<size_t> still_pending;

        for (size_t i = 0; i < pending.size(); i++) {
            size_t idx = pending[i];

            if (!(pfds[i].revents & (POLLOUT | POLLERR | POLLHUP))) {
                still_pending.push_back(idx);
                continue;
            }

            int err = 0;
            socklen_t len = sizeof(err);
            getsockopt(fds[idx], SOL_SOCKET, SO_ERROR, &err, &len);
            if (err != 0) {
                close(fds[idx]);
                continue;
            }

            connected.push_back({fds[idx], attempted[idx]});
            std::cerr << "TCP connected: " << attempted[idx].host
                      << ":" << attempted[idx].port << "\n";
        }

        pending = std::move(still_pending);
    }

    for (size_t idx : pending)
        close(fds[idx]);

    return connected;
}

std::vector<PeerConnection> handshake_peers(const std::vector<PeerSocket>& sockets,
                                            const TorrentFile& torrent,
                                            const std::string& peer_id) {
    auto hs = build_handshake(torrent, peer_id);

    std::vector<PendingHandshake> pending;
    for (const auto& s : sockets) {
        if (send(s.sockfd, hs.data(), hs.size(), 0) == -1) {
            close(s.sockfd);
            continue;
        }
        pending.push_back({s.sockfd, s.peer, {}, 0});
    }

    std::vector<PeerConnection> verified;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);

    while (!pending.empty()) {
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) break;

        int timeout_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             deadline - now).count();

        std::vector<pollfd> pfds;
        for (const auto& p : pending)
            pfds.push_back({p.sockfd, POLLIN, 0});

        int ready = poll(pfds.data(), pfds.size(), timeout_ms);
        if (ready <= 0) break;

        std::vector<PendingHandshake> still_pending;

        for (size_t i = 0; i < pending.size(); i++) {
            auto& p = pending[i];

            if (!(pfds[i].revents & POLLIN)) {
                still_pending.push_back(p);
                continue;
            }

            ssize_t n = recv(p.sockfd, p.buffer.data() + p.received,
                             68 - p.received, 0);

            if (n <= 0) {
                close(p.sockfd);
                continue;
            }

            p.received += n;

            if (p.received < 68) {
                still_pending.push_back(p);
                continue;
            }

            if (p.buffer[0] != 19 ||
                memcmp(p.buffer.data() + 1, "BitTorrent protocol", 19) != 0 ||
                memcmp(p.buffer.data() + 28, torrent.info_hash.data(), 20) != 0) {
                close(p.sockfd);
                continue;
            }

            verified.push_back({p.sockfd, p.peer});
            std::cerr << "handshake ok: " << p.peer.host << ":" << p.peer.port << "\n";
        }

        pending = std::move(still_pending);
    }

    for (const auto& p : pending)
        close(p.sockfd);

    return verified;
}