#include <charconv>
#include "peer_manager.hpp"

#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include "bencode/torrent.hpp"
#include "core/log.hpp"

namespace {
    constexpr size_t kMaxHalfOpen      = 20;    // concurrent dials, leaves room for inbound
    constexpr size_t kMaxCandidates    = 1000;  // bound on queued addresses
    constexpr auto   kConnectTimeout   = std::chrono::seconds(10);
    constexpr auto   kHandshakeTimeout = std::chrono::seconds(15);

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
}

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
        log(LogLevel::Warn, "listen: bind {} failed: {} (inbound peers disabled)", port, strerror(errno));
        close(fd);
        return -1;
    }
    if (listen(fd, 32) < 0) {
        log(LogLevel::Warn, "listen: {} (inbound peers disabled)", strerror(errno));
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
        log(LogLevel::Info, "listening for inbound peers on port {}", listen_port);
}

PeerManager::~PeerManager() {
    for (auto& [id, c] : conns_)   if (c.sockfd >= 0) close(c.sockfd);
    for (auto& p : inbound_)       if (p.sockfd >= 0) close(p.sockfd);
    for (auto& p : outbound_)      if (p.sockfd >= 0) close(p.sockfd);
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

        if (conns_.size() + inbound_.size() + outbound_.size() >= max_peers_) {
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

        log(LogLevel::Debug, "inbound: {}:{}", p.peer.host, p.peer.port);
        inbound_.push_back(std::move(p));
    }
}

// An inbound peer speaks first, so the order here is the mirror of
// handshake_peers(): read and verify their 68 bytes, then send ours.
// recv() is capped at exactly what is missing so that anything the peer
// pipelined behind the handshake (usually its bitfield) stays in the kernel
// buffer and is picked up by the normal read path on the next poll.
PeerManager::HandshakeResult PeerManager::advance_inbound(PendingInbound& p, std::vector<PeerEvent>& out) {
    ssize_t n = recv(p.sockfd, p.buffer.data() + p.received, 68 - p.received, 0);

    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            return HandshakeResult::Keep;
        return HandshakeResult::Drop;
    }
    if (n == 0) return HandshakeResult::Drop;   // peer hung up

    p.received += static_cast<size_t>(n);
    if (p.received < 68) return HandshakeResult::Keep;

    if (!verify_handshake(p.buffer, p.peer)) return HandshakeResult::Drop;

    const uint32_t id = promote(p.sockfd, p.peer, out);
    queue(conns_.at(id), build_handshake(torrent_, peer_id_));
    log(LogLevel::Debug, "handshake ok (inbound): {}:{}", p.peer.host, p.peer.port);

    p.sockfd = -1;   // ownership moved into conns_; caller must not close it
    return HandshakeResult::Promoted;
}

void PeerManager::expire_inbound(std::chrono::seconds timeout) {
    const auto cutoff = std::chrono::steady_clock::now() - timeout;
    for (auto it = inbound_.begin(); it != inbound_.end(); ) {
        if (it->started < cutoff) {
            log(LogLevel::Debug, "inbound {}: handshake timed out", it->peer.host);
            if (it->sockfd >= 0) close(it->sockfd);
            it = inbound_.erase(it);
        } else {
            ++it;
        }
    }
}

std::vector<pollfd> PeerManager::build_fds() {
    std::vector<pollfd> pfds;

    const bool listening = listen_fd_ >= 0;
    if (listening) pfds.push_back({.fd = listen_fd_, .events = POLLIN, .revents = 0});

    inbound_at_ = pfds.size();
    for (const auto& p : inbound_)
        pfds.push_back({.fd = p.sockfd, .events = POLLIN, .revents = 0});

    // Connecting sockets become writable when the TCP connect finishes;
    // handshaking ones become readable when the peer's reply arrives.
    outbound_at_ = pfds.size();
    for (const auto& o : outbound_) {
        const short ev = o.phase == PendingOutbound::Phase::Connecting ? POLLOUT : POLLIN;
        pfds.push_back({.fd = o.sockfd, .events = ev, .revents = 0});
    }

    conns_at_ = pfds.size();
    // refresh stale id list
    ids_.clear();
    ids_.reserve(conns_.size());
    for (auto& [id, c] : conns_) {
        // Only ask for POLLOUT when there is something queued. A healthy socket
        // is almost always writable, so requesting it unconditionally makes
        // poll() return instantly every iteration
        short events_mask = POLLIN;
        if (!c.write_buffer.empty()) events_mask |= POLLOUT;
        pfds.push_back({.fd = c.sockfd, .events = events_mask, .revents = 0});
        ids_.push_back(id);
    }
    return pfds;
}

std::vector<PeerEvent> PeerManager::handle_events(const std::span<pollfd> pfds) {
    std::vector<PeerEvent> events;

    // 1. new connections
    if (const bool listening = listen_fd_ >= 0; listening && (pfds[0].revents & POLLIN))
        accept_new();

    // 2. handshakes in progress. Walked by index because advance_inbound may
    //    move an entry into conns_; survivors are rebuilt into a fresh vector
    //    rather than erased in place.
    if (!inbound_.empty()) {
        std::vector<PendingInbound> still_pending;
        still_pending.reserve(inbound_.size());

        for (size_t i = 0; i < inbound_.size(); ++i) {
            PendingInbound& p = inbound_[i];
            const short rev = pfds[inbound_at_ + i].revents;

            if (rev == 0) {
                still_pending.push_back(std::move(p));
                continue;
            }

            HandshakeResult r = HandshakeResult::Drop;
            if (rev & (POLLIN | POLLERR | POLLHUP))
                r = advance_inbound(p, events);

            if (r == HandshakeResult::Keep) {
                still_pending.push_back(std::move(p));
            } else if (r == HandshakeResult::Drop) {
                if (p.sockfd >= 0) close(p.sockfd);
            }
            // Promoted: fd now owned by conns_, do not close, do not keep
        }
        inbound_ = std::move(still_pending);
    }

    if (!outbound_.empty()) {
        std::vector<PendingOutbound> still_pending;
        still_pending.reserve(outbound_.size());

        for (size_t i = 0; i < outbound_.size(); ++i) {
            PendingOutbound& p = outbound_[i];
            const short rev = pfds[outbound_at_ + i].revents;

            const HandshakeResult r = rev ? advance_outbound(p, rev, events)
                                          : HandshakeResult::Keep;
            if (r == HandshakeResult::Keep) {
                still_pending.push_back(std::move(p));
            } else if (r == HandshakeResult::Drop) {
                close(p.sockfd);
                known_.erase(key(p.peer));
            }
            // Promoted: fd owned by conns_, address stays in known_
        }
        outbound_ = std::move(still_pending);
    }

    // 3. established peers. Note conns_ may have grown in step 2; the new
    //    entries are not in pfds this round, which is fine because their
    //    handshake reply is queued and will be flushed on the next poll.
    std::vector<uint32_t> to_drop;

    for (size_t k = 0; k < ids_.size(); ++k) {
        const short rev = pfds[conns_at_ + k].revents;
        if (rev == 0) continue;

        uint32_t id = ids_[k];
        auto it = conns_.find(id);
        if (it == conns_.end()) continue;
        PeerConnection& c = it->second;

        // Drain queued writes first so the peer stays fed.
        if (rev & POLLOUT) {
            ssize_t w = ::send(c.sockfd, c.write_buffer.data(),
                               c.write_buffer.size(), MSG_NOSIGNAL);
            if (w < 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                    log(LogLevel::Debug, "peer {} dropped: send failed: {}",
                        id, strerror(errno));
                    to_drop.push_back(id);
                    continue;
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
            log(LogLevel::Debug, "peer {} dropped: recv failed: {}", id, strerror(errno));
            to_drop.push_back(id);
            continue;
        }
        if (n == 0) {
            log(LogLevel::Debug, "peer {} dropped: connection closed", id);
            to_drop.push_back(id);
            continue;
        }

        c.read_buffer.insert(c.read_buffer.end(), chunk, chunk + n);

        try {
            std::vector<uint8_t> msg;
            while (extract_message(c.read_buffer, msg))
                handle_message(id, msg, events);
        } catch (const std::exception& e) {
            log(LogLevel::Warn, "protocol error from {}: {}", c.peer.host, e.what());
            to_drop.push_back(id);
        }
    }

    for (uint32_t id : to_drop) {
        auto it = conns_.find(id);
        if (it == conns_.end()) continue;

        apply_availability(it->second.has_pieces, -1);
        known_.erase(key(it->second.peer));

        close(it->second.sockfd);
        conns_.erase(it);
        events.push_back({.type = PeerEvent::Dropped, .peer_id = id, .block = {}, .req = {}});
    }

    expire_inbound(std::chrono::seconds(15));
    expire_outbound();

    // Refill any slots freed this loop. New sockets join the next build_fds().
    dial_more();

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
            log(LogLevel::Trace, "peer {} <- choke", peer_id);
            c.peer_choking = true;
            c.outstanding  = 0;
            break;

        case MSG_UNCHOKE:
            log(LogLevel::Trace, "peer {} <- unchoke", peer_id);
            c.peer_choking = false;
            out.push_back({PeerEvent::Unchoke, peer_id, {}, {}});
            break;

        case MSG_INTERESTED:
            log(LogLevel::Trace, "peer {} <- interested", peer_id);
            c.peer_interested = true;
            break;

        case MSG_NOT_INTERESTED:
            log(LogLevel::Trace, "peer {} <- not_interested", peer_id);
            c.peer_interested = false;
            break;

        case MSG_HAVE: {
            if (payload_len != 4) throw std::runtime_error("bad have");
            uint32_t index;
            std::memcpy(&index, payload, 4);
            index = ntohl(index);
            if (index >= c.has_pieces.size()) throw std::runtime_error("have out of range");
            log(LogLevel::Trace, "peer {} <- have piece {}", peer_id, index);
            c.has_pieces.set(index);
            ++piece_frequency_[index];
            break;
        }

        case MSG_BITFIELD: {
            if (c.got_bitfield) break; // throwing and dropping the peer would be too harsh IMO
            c.got_bitfield = true;

            std::vector<uint8_t> raw(payload, payload + payload_len);
            c.has_pieces = Bitfield::from_bytes(raw, c.has_pieces.size());

            log(LogLevel::Trace, "peer {} <- bitfield ({} bytes)", peer_id, payload_len);
            apply_availability(c.has_pieces, +1);
            break;
        }

        case MSG_REQUEST: {
            if (c.am_choking) {
                log(LogLevel::Trace, "peer {} <- request while choked; ignoring", peer_id);
                break;
            }
            if (payload_len != 12) throw std::runtime_error("bad request");
            uint32_t index, begin, length;
            std::memcpy(&index,  payload,     4);
            std::memcpy(&begin,  payload + 4, 4);
            std::memcpy(&length, payload + 8, 4);
            index = ntohl(index); begin = ntohl(begin); length = ntohl(length);

            if (length > BLOCK_SIZE) throw std::runtime_error("request too large");

            log(LogLevel::Trace, "peer {} <- request piece {} off {} len {}",
                peer_id, index, begin, length);
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

            log(LogLevel::Trace, "peer {} <- piece {} off {} ({} bytes)",
                peer_id, index, begin, block.data.size());
            if (c.outstanding > 0) --c.outstanding;
            out.push_back({PeerEvent::Piece, peer_id, std::move(block)});
            break;
        }

        default:
            log(LogLevel::Trace, "peer {} <- unknown msg id {} ({} byte payload)",
                peer_id, id, payload_len);
            break;   // request/cancel/unknown
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
    log(LogLevel::Trace, "peer {} -> bitfield ({} bytes)", peer_id, our_have.bytes().size());
    send_to(peer_id, build_message(MSG_BITFIELD, our_have.bytes()));
}

void PeerManager::send_piece(uint32_t peer_id, uint32_t index,
                             uint32_t begin, const std::vector<uint8_t>& data) {
    std::vector<uint8_t> payload(8 + data.size());
    uint32_t i = htonl(index), b = htonl(begin);
    std::memcpy(payload.data(),     &i, 4);
    std::memcpy(payload.data() + 4, &b, 4);
    std::memcpy(payload.data() + 8, data.data(), data.size());
    log(LogLevel::Trace, "peer {} -> piece {} off {} ({} bytes)",
        peer_id, index, begin, data.size());
    send_to(peer_id, build_message(MSG_PIECE, payload));
}

void PeerManager::send_unchoke(uint32_t peer_id) {
    auto it = conns_.find(peer_id);
    if (it == conns_.end()) return;
    it->second.am_choking = false;
    log(LogLevel::Trace, "peer {} -> unchoke", peer_id);
    send_to(peer_id, build_message(MSG_UNCHOKE));
}

void PeerManager::send_request(uint32_t peer_id, const BlockRequest& req) {
    auto it = conns_.find(peer_id);
    if (it == conns_.end()) return;
    ++it->second.outstanding;
    log(LogLevel::Trace, "peer {} -> request piece {} off {} len {}",
        peer_id, req.piece_index, req.offset, req.length);
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

void PeerManager::add_peers(std::vector<Peer> peers) {
    size_t added = 0;
    for (auto& p : peers) {
        if (candidates_.size() >= kMaxCandidates) break;
        if (!known_.insert(key(p)).second) continue;
        candidates_.push_back(std::move(p));
        ++added;
    }
    log(LogLevel::Debug, "queued {} new peer(s), {} waiting", added, candidates_.size());
    dial_more();
}

void PeerManager::dial_more() {
    while (!candidates_.empty()
           && outbound_.size() < kMaxHalfOpen
           && conns_.size() + inbound_.size() + outbound_.size() < max_peers_) {
        Peer p = std::move(candidates_.front());
        candidates_.pop_front();
        if (!start_dial(p)) known_.erase(key(p));
    }
}

bool PeerManager::start_dial(const Peer& peer) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;

    uint16_t port = 0;
    const char* first = peer.port.data();
    const char* last  = first + peer.port.size();
    auto [end, ec] = std::from_chars(first, last, port);
    if (ec != std::errc{} || end != last || port == 0 ||
        inet_pton(AF_INET, peer.host.c_str(), &addr.sin_addr) != 1) {
        log(LogLevel::Debug, "outbound {}:{}: bad address", peer.host, peer.port);
        return false;
    }
    addr.sin_port = htons(port);

    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        log(LogLevel::Warn, "outbound socket() failed: {}", strerror(errno));
        return false;
    }

    // Non-blocking connect: EINPROGRESS is the normal answer. The result
    // arrives later as POLLOUT, and SO_ERROR says whether it worked.
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0
        && errno != EINPROGRESS) {
        const int err = errno;
        close(fd);
        log(LogLevel::Debug, "outbound {}:{}: connect failed: {}",
            peer.host, peer.port, strerror(err));
        return false;
    }

    PendingOutbound o;
    o.sockfd   = fd;
    o.peer     = peer;
    o.deadline = std::chrono::steady_clock::now() + kConnectTimeout;
    outbound_.push_back(std::move(o));

    log(LogLevel::Trace, "dialing {}:{}", peer.host, peer.port);
    return true;
}

PeerManager::HandshakeResult PeerManager::advance_outbound(PendingOutbound& p, short rev,
                                                           std::vector<PeerEvent>& out) {
    if (p.phase == PendingOutbound::Phase::Connecting) {
        int err = 0;
        socklen_t len = sizeof(err);
        if (getsockopt(p.sockfd, SOL_SOCKET, SO_ERROR, &err, &len) < 0) err = errno;
        if (err != 0) {
            log(LogLevel::Trace, "outbound {}:{}: connect failed: {}",
                p.peer.host, p.peer.port, strerror(err));
            return HandshakeResult::Drop;
        }

        const auto hs = build_handshake(torrent_, peer_id_);
        const ssize_t w = ::send(p.sockfd, hs.data(), hs.size(), MSG_NOSIGNAL);
        if (w != static_cast<ssize_t>(hs.size())) {
            log(LogLevel::Debug, "Partial handshake write to {}:{}",
                p.peer.host, p.peer.port);
        }

        p.phase    = PendingOutbound::Phase::Handshaking;
        p.deadline = std::chrono::steady_clock::now() + kHandshakeTimeout;
        return HandshakeResult::Keep;
    }

    // Handshaking: same capped read as inbound, so anything the peer sends
    // behind its handshake (usually the bitfield) stays for the normal path.
    if (!(rev & (POLLIN | POLLERR | POLLHUP))) return HandshakeResult::Keep;

    const ssize_t n = recv(p.sockfd, p.buffer.data() + p.received, 68 - p.received, 0);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            return HandshakeResult::Keep;
        return HandshakeResult::Drop;
    }
    if (n == 0) return HandshakeResult::Drop;

    p.received += static_cast<size_t>(n);
    if (p.received < 68) return HandshakeResult::Keep;

    if (!verify_handshake(p.buffer, p.peer)) return HandshakeResult::Drop;

    promote(p.sockfd, p.peer, out);
    log(LogLevel::Debug, "handshake ok (outbound): {}:{}", p.peer.host, p.peer.port);

    p.sockfd = -1;   // ownership moved into conns_
    return HandshakeResult::Promoted;
}

void PeerManager::expire_outbound() {
    const auto now = std::chrono::steady_clock::now();
    for (auto it = outbound_.begin(); it != outbound_.end(); ) {
        if (now >= it->deadline) {
            log(LogLevel::Trace, "outbound {}:{}: timed out", it->peer.host, it->peer.port);
            close(it->sockfd);
            known_.erase(key(it->peer));
            it = outbound_.erase(it);
        } else {
            ++it;
        }
    }
}

bool PeerManager::verify_handshake(const std::array<uint8_t, 68>& hs, const Peer& peer) const {
    if (hs[0] != 19 || std::memcmp(hs.data() + 1, "BitTorrent protocol", 19) != 0) {
        log(LogLevel::Debug, "{}: bad protocol header", peer.host);
        return false;
    }
    if (std::memcmp(hs.data() + 28, torrent_.info_hash.data(), 20) != 0) {
        log(LogLevel::Debug, "{}: info hash mismatch", peer.host);
        return false;
    }
    // Trackers hand our own address back to us; without this we'd connect to ourselves.
    if (std::memcmp(hs.data() + 48, peer_id_.data(), 20) == 0) {
        log(LogLevel::Debug, "{}: that is us, dropping", peer.host);
        return false;
    }
    return true;
}

uint32_t PeerManager::promote(int fd, const Peer& peer, std::vector<PeerEvent>& out) {
    PeerConnection c;
    c.id         = next_id_++;
    c.sockfd     = fd;
    c.peer       = peer;
    c.has_pieces = Bitfield(piece_count_);

    const uint32_t id = c.id;
    conns_.emplace(id, std::move(c));
    out.push_back({.type = PeerEvent::Joined, .peer_id = id, .block = {}, .req = {}});
    return id;
}