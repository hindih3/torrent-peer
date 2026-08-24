#include "peer_manager.hpp"

#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

int build_listen_fd(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));  // reuse port on restart

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port);

    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "listen: bind " << port << " failed: " << strerror(errno)
                  << " (inbound peers disabled)\n";
        close(fd);
        return -1;
    }
    if (listen(fd, 32) < 0) {
        std::cerr << "listen: " << strerror(errno) << " (inbound peers disabled)\n";
        close(fd);
        return -1;
    }

    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    return fd;
}

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

PeerManager::PeerManager(std::vector<PeerConnection> conns,
                         const TorrentFile& torrent,
                         std::string peer_id,
                         uint16_t listen_port,
                         size_t max_peers)
    : torrent_(torrent),
      peer_id_(std::move(peer_id)),
      piece_count_(static_cast<uint32_t>(torrent.pieces.size())),
      listen_fd_(build_listen_fd(listen_port)),
      max_peers_(max_peers),
      piece_frequency_(torrent.pieces.size(), 0) {

    if (peer_id_.size() != 20)
        throw std::runtime_error("peer_id must be exactly 20 bytes");

    for (auto& c : conns) {
        uint32_t id = next_id_++;
        c.id = id;
        conns_.emplace(id, std::move(c));
    }

    if (listen_fd_ >= 0)
        std::cerr << "listening for inbound peers on port " << listen_port << "\n";
}

PeerManager::~PeerManager() {
    for (auto& [id, c] : conns_)   if (c.sockfd >= 0) close(c.sockfd);
    for (auto& p : inbound_)       if (p.sockfd >= 0) close(p.sockfd);
    if (listen_fd_ >= 0) close(listen_fd_);
}

// Drain the backlog. A single poll wakeup can cover several pending
// connections, so this loops until accept() says there is nothing left.
void PeerManager::accept_new() {
    if (listen_fd_ < 0) return;

    while (true) {
        sockaddr_in addr{};
        socklen_t   len = sizeof(addr);
        int fd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len);

        if (fd < 0) {
            if (errno == ECONNABORTED || errno == EINTR) continue;
            break;   // EAGAIN/EWOULDBLOCK: backlog drained
        }

        if (conns_.size() + inbound_.size() >= max_peers_) {
            close(fd);   // at capacity; let them retry later
            continue;
        }

        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);

        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));

        PendingInbound p;
        p.sockfd  = fd;
        p.peer    = { std::string(ip), std::to_string(ntohs(addr.sin_port)) };
        p.started = std::chrono::steady_clock::now();

        std::cerr << "inbound: " << p.peer.host << ":" << p.peer.port << "\n";
        inbound_.push_back(std::move(p));
    }
}

// An inbound peer speaks first, so the order here is the mirror of
// handshake_peers(): read and verify their 68 bytes, then send ours.
// recv() is capped at exactly what is missing so that anything the peer
// pipelined behind the handshake (usually its bitfield) stays in the kernel
// buffer and is picked up by the normal read path on the next poll.
PeerManager::InboundResult PeerManager::advance_inbound(PendingInbound& p, std::vector<PeerEvent>& out) {
    ssize_t n = recv(p.sockfd, p.buffer.data() + p.received, 68 - p.received, 0);

    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            return InboundResult::Keep;
        return InboundResult::Drop;
    }
    if (n == 0) return InboundResult::Drop;   // peer hung up

    p.received += static_cast<size_t>(n);
    if (p.received < 68) return InboundResult::Keep;

    if (p.buffer[0] != 19 ||
        std::memcmp(p.buffer.data() + 1, "BitTorrent protocol", 19) != 0) {
        std::cerr << "inbound " << p.peer.host << ": bad protocol header\n";
        return InboundResult::Drop;
    }
    if (std::memcmp(p.buffer.data() + 28, torrent_.info_hash.data(), 20) != 0) {
        std::cerr << "inbound " << p.peer.host << ": info hash mismatch\n";
        return InboundResult::Drop;
    }
    // We advertise ourselves to trackers, so trackers hand our address back to
    // us; without this a client happily connects to itself.
    if (std::memcmp(p.buffer.data() + 48, peer_id_.data(), 20) == 0) {
        std::cerr << "inbound " << p.peer.host << ": that is us, dropping\n";
        return InboundResult::Drop;
    }

    PeerConnection c;
    c.id         = next_id_++;
    c.sockfd     = p.sockfd;
    c.peer       = p.peer;
    c.has_pieces = Bitfield(piece_count_);

    // Our reply goes through the same write_buffer as everything else, so a
    // partial send is handled by the POLLOUT path rather than blocking here.
    queue(c, build_handshake(torrent_, peer_id_));

    const uint32_t id = c.id;
    conns_.emplace(id, std::move(c));
    out.push_back({PeerEvent::Joined, id, {}, {}});

    std::cerr << "handshake ok (inbound): " << p.peer.host << ":" << p.peer.port << "\n";

    p.sockfd = -1;   // ownership moved into conns_; caller must not close it
    return InboundResult::Promoted;
}

void PeerManager::expire_inbound(std::chrono::seconds timeout) {
    const auto cutoff = std::chrono::steady_clock::now() - timeout;
    for (auto it = inbound_.begin(); it != inbound_.end(); ) {
        if (it->started < cutoff) {
            std::cerr << "inbound " << it->peer.host << ": handshake timed out\n";
            if (it->sockfd >= 0) close(it->sockfd);
            it = inbound_.erase(it);
        } else {
            ++it;
        }
    }
}

std::vector<PeerEvent> PeerManager::poll_once(int timeout_ms) {
    std::vector<PeerEvent> events;

    // The fd set is three sections laid out back to back, so a single poll()
    // covers the listener, half-open inbound sockets, and live peers:
    //   [0]                          listen_fd_        (if bound)
    //   [inbound_at, conns_at)       inbound_          (handshake in progress)
    //   [conns_at, end)              conns_            (peer wire protocol)
    std::vector<pollfd> pfds;

    const bool listening = listen_fd_ >= 0;
    if (listening) pfds.push_back({listen_fd_, POLLIN, 0});

    const size_t inbound_at = pfds.size();
    for (const auto& p : inbound_)
        pfds.push_back({p.sockfd,POLLIN, 0});

    const size_t conns_at = pfds.size();
    std::vector<uint32_t> ids;
    ids.reserve(conns_.size());
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
    if (ready < 0) {
        if (errno == EINTR) return events;
        throw_errno("poll");
    }

    // Even on a timeout, sweep handshakes that have gone quiet.
    expire_inbound(std::chrono::seconds(15));
    if (ready == 0) return events;

    // 1. new connections
    if (listening && (pfds[0].revents & POLLIN))
        accept_new();

    // 2. handshakes in progress. Walked by index because advance_inbound may
    //    move an entry into conns_; survivors are rebuilt into a fresh vector
    //    rather than erased in place.
    if (!inbound_.empty()) {
        std::vector<PendingInbound> still_pending;
        still_pending.reserve(inbound_.size());

        for (size_t i = 0; i < inbound_.size(); ++i) {
            PendingInbound& p = inbound_[i];
            const short rev = pfds[inbound_at + i].revents;

            if (rev == 0) {
                still_pending.push_back(std::move(p));
                continue;
            }

            InboundResult r = InboundResult::Drop;
            if (rev & (POLLIN | POLLERR | POLLHUP))
                r = advance_inbound(p, events);

            if (r == InboundResult::Keep) {
                still_pending.push_back(std::move(p));
            } else if (r == InboundResult::Drop) {
                if (p.sockfd >= 0) close(p.sockfd);
            }
            // Promoted: fd now owned by conns_, do not close, do not keep
        }
        inbound_ = std::move(still_pending);
    }

    // 3. established peers. Note conns_ may have grown in step 2; the new
    //    entries are not in pfds this round, which is fine because their
    //    handshake reply is queued and will be flushed on the next poll.
    std::vector<uint32_t> to_drop;

    for (size_t k = 0; k < ids.size(); ++k) {
        const short rev = pfds[conns_at + k].revents;
        if (rev == 0) continue;

        uint32_t id = ids[k];
        auto it = conns_.find(id);
        if (it == conns_.end()) continue;
        PeerConnection& c = it->second;

        // Drain queued writes first so the peer stays fed.
        if (rev & POLLOUT) {
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

        if (!(rev & (POLLIN | POLLERR | POLLHUP))) continue;

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
        if (it == conns_.end()) continue;

        apply_availability(it->second.has_pieces, -1);

        close(it->second.sockfd);
        conns_.erase(it);
        events.push_back({PeerEvent::Dropped, id, {}, {}});
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
            out.push_back({PeerEvent::Unchoke, peer_id, {}, {}});
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
            ++piece_frequency_[index];
            break;
        }

        case MSG_BITFIELD: {
            if (c.got_bitfield) throw std::runtime_error("duplicate bitfield");
            c.got_bitfield = true;

            std::vector<uint8_t> raw(payload, payload + payload_len);
            c.has_pieces = Bitfield::from_bytes(raw, c.has_pieces.size());

            apply_availability(c.has_pieces, +1);
            break;
        }

        case MSG_REQUEST: {
            if (c.am_choking) break;
            if (payload_len != 12) throw std::runtime_error("bad request");
            uint32_t index, begin, length;
            std::memcpy(&index,  payload,     4);
            std::memcpy(&begin,  payload + 4, 4);
            std::memcpy(&length, payload + 8, 4);
            index = ntohl(index); begin = ntohl(begin); length = ntohl(length);

            if (length > BLOCK_SIZE) throw std::runtime_error("request too large");

            out.push_back({PeerEvent::Request, peer_id, {}, {index, begin, length}});
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

void PeerManager::send_interested(uint32_t peer_id) {
    auto it = conns_.find(peer_id);
    if (it == conns_.end()) return;
    it->second.am_interested = true;
    queue(it->second, build_message(MSG_INTERESTED));
}

void PeerManager::send_choke(uint32_t peer_id) {
    auto it = conns_.find(peer_id);
    if (it == conns_.end()) return;
    it->second.am_choking = true;
    queue(it->second, build_message(MSG_CHOKE));
}

void PeerManager::send_to(uint32_t peer_id, const std::vector<uint8_t>& msg) {
    auto it = conns_.find(peer_id);
    if (it != conns_.end())
        queue(it->second, msg);
}

void PeerManager::send_bitfield(uint32_t peer_id, const Bitfield& our_have) {
    send_to(peer_id, build_message(MSG_BITFIELD, our_have.bytes()));
}

void PeerManager::send_piece(uint32_t peer_id, uint32_t index,
                             uint32_t begin, const std::vector<uint8_t>& data) {
    std::vector<uint8_t> payload(8 + data.size());
    uint32_t i = htonl(index), b = htonl(begin);
    std::memcpy(payload.data(),     &i, 4);
    std::memcpy(payload.data() + 4, &b, 4);
    std::memcpy(payload.data() + 8, data.data(), data.size());
    send_to(peer_id, build_message(MSG_PIECE, payload));
}

void PeerManager::send_unchoke(uint32_t peer_id) {
    auto it = conns_.find(peer_id);
    if (it == conns_.end()) return;
    it->second.am_choking = false;
    send_to(peer_id, build_message(MSG_UNCHOKE));
}

void PeerManager::send_request(uint32_t peer_id, const BlockRequest& req) {
    auto it = conns_.find(peer_id);
    if (it == conns_.end()) return;
    ++it->second.outstanding;
    send_to(peer_id, build_request(req));
}

void PeerManager::broadcast_have(uint32_t index) {
    std::vector<uint8_t> payload(4);
    uint32_t idx = htonl(index);
    std::memcpy(payload.data(), &idx, 4);
    auto msg = build_message(MSG_HAVE, payload);

    for (auto& [id, c] : conns_)
        queue(c, msg);
}

void PeerManager::queue(PeerConnection& c, const std::vector<uint8_t>& msg) {
    c.write_buffer.insert(c.write_buffer.end(), msg.begin(), msg.end());
}

void PeerManager::apply_availability(const Bitfield& bf, int delta) {
    for (uint32_t i = 0; i < piece_frequency_.size(); ++i)
        if (bf.get(i)) piece_frequency_[i] += delta;
}