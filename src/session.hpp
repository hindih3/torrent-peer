#pragma once
#include <unistd.h>
#include <sys/poll.h>
#include "peer.hpp"

enum MessageId : uint8_t {
    MSG_CHOKE = 0, MSG_UNCHOKE = 1, MSG_INTERESTED = 2, MSG_NOT_INTERESTED = 3,
    MSG_HAVE = 4, MSG_BITFIELD = 5, MSG_REQUEST = 6, MSG_PIECE = 7, MSG_CANCEL = 8
};

std::vector<uint8_t> build_message(uint8_t id, const std::vector<uint8_t>& payload = {}) {
    uint32_t len = htonl(1 + payload.size());
    std::vector<uint8_t> msg(4 + 1 + payload.size());
    memcpy(msg.data(), &len, 4);
    msg[4] = id;
    if (!payload.empty())
        memcpy(msg.data() + 5, payload.data(), payload.size());
    return msg;
}

bool extract_message(std::vector<uint8_t>& buf, std::vector<uint8_t>& out) {
    if (buf.size() < 4) return false;

    uint32_t len;
    memcpy(&len, buf.data(), 4);
    len = ntohl(len);

    if (len > 1 << 20)
        throw std::runtime_error("discarded stupidly long message");

    if (buf.size() < 4 + len) return false;

    out.assign(buf.begin() + 4, buf.begin() + 4 + len);
    buf.erase(buf.begin(), buf.begin() + 4 + len);
    return true;
}

void handle_message(PeerConnection& p, const std::vector<uint8_t>& msg,
                    const TorrentFile& torrent) {
    if (msg.empty()) {
        std::cerr << p.peer.host << " keep-alive\n";
        return;
    }

    uint8_t id = msg[0];
    const uint8_t* payload = msg.data() + 1;
    size_t payload_len = msg.size() - 1;

    switch (id) {
        case MSG_CHOKE:
            p.peer_choking = true;
            std::cerr << p.peer.host << " choke\n";
            break;

        case MSG_UNCHOKE:
            p.peer_choking = false;
            std::cerr << p.peer.host << " unchoke\n";
            break;

        case MSG_INTERESTED:
            p.peer_interested = true;
            std::cerr << p.peer.host << " interested\n";
            break;

        case MSG_NOT_INTERESTED:
            p.peer_interested = false;
            std::cerr << p.peer.host << " not interested\n";
            break;

        case MSG_HAVE: {
            if (payload_len != 4) throw std::runtime_error("bad have");
            uint32_t index;
            memcpy(&index, payload, 4);
            index = ntohl(index);
            if (index >= p.piece_array.size()) throw std::runtime_error("have out of range");
            p.piece_array[index] = true;
            std::cerr << p.peer.host << " have " << index << "\n";
            break;
        }

        case MSG_BITFIELD: {
            if (const size_t expected = (torrent.pieces.size() + 7) / 8; payload_len != expected)
                throw std::runtime_error("bad bitfield length");

            size_t count = 0;
            for (size_t i = 0; i < p.piece_array.size(); i++) {
                const bool has = (payload[i / 8] >> (7 - (i % 8))) & 1;
                p.piece_array[i] = has;
                if (has) count++;
            }
            std::cerr << p.peer.host << " bitfield: " << count << "/"
                      << p.piece_array.size() << " pieces\n";
            break;
        }

        default:
            std::cerr << p.peer.host << " msg id " << (int)id
                      << " (" << payload_len << " bytes)\n";
    }
}

void message_loop(std::vector<PeerConnection>& peers, const TorrentFile& torrent) {
    for (auto& p : peers)
        p.piece_array.resize(torrent.pieces.size(), false);

    while (!peers.empty()) {
        std::vector<pollfd> pfds;
        for (const auto& p : peers)
            pfds.push_back({p.sockfd, POLLIN, 0});

        int ready = poll(pfds.data(), pfds.size(), 1000);
        if (ready <= 0) continue;

        std::vector<PeerConnection> alive;

        for (size_t i = 0; i < peers.size(); i++) {
            auto& p = peers[i];

            if (!(pfds[i].revents & POLLIN)) {
                alive.push_back(std::move(p));
                continue;
            }

            uint8_t chunk[16384];
            ssize_t n = recv(p.sockfd, chunk, sizeof(chunk), 0);

            if (n <= 0) {
                std::cerr << "dropped: " << p.peer.host << "\n";
                close(p.sockfd);
                continue;
            }

            p.recv_buffer.insert(p.recv_buffer.end(), chunk, chunk + n);

            bool ok = true;
            std::vector<uint8_t> msg;
            try {
                while (extract_message(p.recv_buffer, msg))
                    handle_message(p, msg, torrent);
            } catch (const std::exception& e) {
                std::cerr << "protocol error from " << p.peer.host << ": " << e.what() << "\n";
                close(p.sockfd);
                ok = false;
            }

            if (ok) alive.push_back(std::move(p));
        }

        peers = std::move(alive);
    }
}

