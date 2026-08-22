#include "peer_manager.hpp"

#include <cstring>
#include <iostream>
#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

std::vector<uint8_t> build_message(uint8_t id, const std::vector<uint8_t>& payload) {
    uint32_t len = htonl(static_cast<uint32_t>(1 + payload.size()));
    std::vector<uint8_t> msg(5 + payload.size());
    std::memcpy(msg.data(), &len, 4);
    msg[4] = id;
    if (!payload.empty())
        std::memcpy(msg.data() + 5, payload.data(), payload.size());
    return msg;
}

std::vector<uint8_t> build_request(const BlockRequest& req) {
    std::vector<uint8_t> payload(12);
    uint32_t index  = htonl(req.piece_index);
    uint32_t offset = htonl(req.offset);
    uint32_t length = htonl(req.length);
    std::memcpy(payload.data(),     &index,  4);
    std::memcpy(payload.data() + 4, &offset, 4);
    std::memcpy(payload.data() + 8, &length, 4);
    return build_message(MSG_REQUEST, payload);
}

static bool extract_message(std::vector<uint8_t>& buf, std::vector<uint8_t>& out) {
    if (buf.size() < 4) return false;

    uint32_t len;
    std::memcpy(&len, buf.data(), 4);
    len = ntohl(len);

    if (len > (1u << 20))
        throw std::runtime_error("absurd message length");

    if (buf.size() < 4 + len) return false;

    out.assign(buf.begin() + 4, buf.begin() + 4 + len);
    buf.erase(buf.begin(), buf.begin() + 4 + len);
    return true;
}

PeerManager::PeerManager(std::vector<PeerConnection> conns) {
    for (auto& c : conns) {
        uint32_t id = next_id_++;
        c.id = id;
        conns_.emplace(id, std::move(c));
    }
}

std::vector<PeerEvent> PeerManager::poll_once(int timeout_ms) {
    std::vector<PeerEvent> events;

    std::vector<pollfd>   pfds;
    std::vector<uint32_t> ids;
    for (auto& [id, c] : conns_) {
        // Only ask for POLLOUT when there is something queued. A healthy socket
        // is almost always writable, so requesting it unconditionally makes
        // poll() return instantly every iteration
        short events_mask = POLLIN;
        if (!c.write_buffer.empty()) events_mask |= POLLOUT;
        pfds.push_back({c.sockfd, events_mask, 0});
        ids.push_back(id);
    }

    if (pfds.empty()) return events;

    int ready = poll(pfds.data(), pfds.size(), timeout_ms);
    if (ready == 0) return events;
    if (ready < 0) {
        if (errno == EINTR) return events;
        throw_errno("poll");
    }

    std::vector<uint32_t> to_drop;

    for (size_t k = 0; k < pfds.size(); ++k) {
        if (pfds[k].revents == 0) continue;

        uint32_t id = ids[k];
        auto it = conns_.find(id);
        if (it == conns_.end()) continue;
        PeerConnection& c = it->second;

        // Drain queued writes first so the peer stays fed.
        if (pfds[k].revents & POLLOUT) {
            ssize_t w = ::send(c.sockfd, c.write_buffer.data(),
                               c.write_buffer.size(), MSG_NOSIGNAL);
            if (w < 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                    to_drop.push_back(id);
                    continue;  // socket is gone; don't try to read from it
                }
            } else {
                c.write_buffer.erase(c.write_buffer.begin(),
                                     c.write_buffer.begin() + w);
            }
        }

        if (!(pfds[k].revents & (POLLIN | POLLERR | POLLHUP))) continue;

        uint8_t chunk[16384];
        ssize_t n = recv(c.sockfd, chunk, sizeof(chunk), 0);

        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
            to_drop.push_back(id);
            continue;
        }
        if (n == 0) { to_drop.push_back(id); continue; }

        c.read_buffer.insert(c.read_buffer.end(), chunk, chunk + n);

        try {
            std::vector<uint8_t> msg;
            while (extract_message(c.read_buffer, msg))
                handle_message(id, msg, events);
        } catch (const std::exception& e) {
            std::cerr << "protocol error from " << c.peer.host << ": " << e.what() << "\n";
            to_drop.push_back(id);
        }
    }

    for (uint32_t id : to_drop) {
        auto it = conns_.find(id);
        if (it != conns_.end()) {
            close(it->second.sockfd);
            conns_.erase(it);
            events.push_back({PeerEvent::Dropped, id, {}});
        }
    }

    return events;
}

void PeerManager::handle_message(uint32_t peer_id, const std::vector<uint8_t>& msg,
                                 std::vector<PeerEvent>& out) {
    auto it = conns_.find(peer_id);
    if (it == conns_.end()) return;
    PeerConnection& c = it->second;

    if (msg.empty()) return;   // keep-alive

    uint8_t id = msg[0];
    const uint8_t* payload = msg.data() + 1;
    size_t payload_len = msg.size() - 1;

    switch (id) {
        case MSG_CHOKE:
            c.peer_choking = true;
            c.outstanding  = 0;
            break;

        case MSG_UNCHOKE:
            c.peer_choking = false;
            out.push_back({PeerEvent::Unchoke, peer_id, {}});
            break;

        case MSG_INTERESTED:
            c.peer_interested = true;
            break;

        case MSG_NOT_INTERESTED:
            c.peer_interested = false;
            break;

        case MSG_HAVE: {
            if (payload_len != 4) throw std::runtime_error("bad have");
            uint32_t index;
            std::memcpy(&index, payload, 4);
            index = ntohl(index);
            if (index >= c.has_pieces.size()) throw std::runtime_error("have out of range");
            c.has_pieces.set(index);
            break;
        }

        case MSG_BITFIELD: {
            std::vector<uint8_t> raw(payload, payload + payload_len);
            c.has_pieces = Bitfield::from_bytes(raw, c.has_pieces.size());
            break;
        }

        case MSG_PIECE: {
            if (payload_len < 8) throw std::runtime_error("bad piece");
            uint32_t index, begin;
            std::memcpy(&index, payload,     4);
            std::memcpy(&begin, payload + 4, 4);
            index = ntohl(index);
            begin = ntohl(begin);

            Block block;
            block.piece_index = index;
            block.offset      = begin;
            block.data.assign(payload + 8, payload + payload_len);

            if (c.outstanding > 0) --c.outstanding;
            out.push_back({PeerEvent::Piece, peer_id, std::move(block)});
            break;
        }

        default:
            break;   // request/cancel/unknown — ignored in naive version
    }
}

void PeerManager::send_interested_all() {
    auto msg = build_message(MSG_INTERESTED);
    for (auto& [id, c] : conns_) {
        c.am_interested = true;
        queue(c, msg);
    }
}

void PeerManager::send_request(uint32_t peer_id, const BlockRequest& req) {
    auto it = conns_.find(peer_id);
    if (it == conns_.end()) return;
    queue(it->second, build_request(req));
    ++it->second.outstanding;
}

void PeerManager::broadcast_have(uint32_t index) {
    std::vector<uint8_t> payload(4);
    uint32_t idx = htonl(index);
    std::memcpy(payload.data(), &idx, 4);
    auto msg = build_message(MSG_HAVE, payload);

    for (auto& [id, c] : conns_)
        queue(c, msg);
}

void PeerManager::queue(PeerConnection& c, std::vector<uint8_t> msg) {
    c.write_buffer.insert(c.write_buffer.end(), msg.begin(), msg.end());
}