#include "peer_manager.hpp"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <arpa/inet.h>
#include <poll.h>
#include <ranges>
#include <sys/socket.h>

#include "metainfo/torrent.hpp"
#include "core/log.hpp"

namespace {
    constexpr size_t kMaxQueuedBytes = (1 << 22);  // 4 MiB
    constexpr size_t kRegularSlots   = 7;

    constexpr auto   kKeepAliveInterval = std::chrono::seconds(60);
    constexpr auto   kIdleTimeout       = std::chrono::seconds(150);

    enum MessageId : uint8_t {
        MsgChoke = 0, MsgUnchoke = 1, MsgInterested = 2, MsgNotInterested = 3,
        MsgHave = 4, MsgBitfield = 5, MsgRequest = 6, MsgPiece = 7, MsgCancel = 8
    };

    bool extract_message(std::vector<uint8_t>& buf, std::vector<uint8_t>& out) {
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

    std::vector<BlockRequest> requests_of(const std::vector<PendingRequest>& pending) {
        std::vector<BlockRequest> out;
        out.reserve(pending.size());
        for (const auto& p : pending) out.push_back(p.req);
        return out;
    }

    std::vector<uint8_t> build_message(const uint8_t id, const std::vector<uint8_t>& payload = {}) {
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
        return build_message(MsgRequest, payload);
    }

    // drop queued piece messages for which 'match(index, begin, length)' is true.
    // The front message stays once cursor > 0: the peer already has its first
    // bytes, so removing the rest would desync the stream
    template <typename Match>
    size_t remove_queued_pieces(PeerConnection& c, Match match) {
        size_t removed = 0;
        auto it = c.write_buffer.begin() + (c.cursor > 0 ? 1 : 0);
        while (it != c.write_buffer.end()) {
            // a piece message: [len:4][id:1][index:4][begin:4][block]
            const std::vector<uint8_t>& m = *it;
            if (m.size() < 13 || m[4] != MsgPiece) { ++it; continue; }
            uint32_t i, b;
            std::memcpy(&i, m.data() + 5, 4);
            std::memcpy(&b, m.data() + 9, 4);
            if (!match(ntohl(i), ntohl(b), static_cast<uint32_t>(m.size() - 13))) { ++it; continue; }

            c.queued_bytes -= m.size();
            it = c.write_buffer.erase(it);
            ++removed;
        }
        return removed;
    }
}

PeerManager::PeerManager(const TorrentFile& torrent, std::string peer_id,
            uint16_t listen_port, const size_t max_peers)
    : torrent_(torrent),
      peer_id_(std::move(peer_id)),
      ctx_({torrent_, peer_id_, banned_}),
      inbound_(ctx_, listen_port),
      outbound_(ctx_),
      piece_count_(static_cast<uint32_t>(torrent.pieces.size())),
      max_peers_(max_peers),
      piece_frequency_(torrent.pieces.size(), 0),
      rng_(std::random_device{}()) {

    if (peer_id_.size() != 20)
        throw std::runtime_error("peer_id must be exactly 20 bytes");

    if (inbound_.listening())
        log(LogLevel::Info, "listening for inbound peers on port {}", listen_port);
}

std::vector<pollfd> PeerManager::build_fds() {
    std::vector<pollfd> pfds;

    inbound_at_ = pfds.size();
    inbound_.append_fds(pfds);
    inbound_len_ = pfds.size() - inbound_at_;

    // Connecting sockets become writable when the TCP connect finishes;
    // handshaking ones become readable when the peer's reply arrives.
    outbound_at_ = pfds.size();
    outbound_.append_fds(pfds);
    outbound_len_ = pfds.size() - outbound_at_;

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
        pfds.push_back({.fd = c.sock.get(), .events = events_mask, .revents = 0});
        ids_.push_back(id);
    }
    return pfds;
}

// pfds was built by build_fds() in this layout:
//   [inbound (listen?, pending...)][outbound_...][conns_ via ids_...]
// Each loop below reads its slice by index, so nothing may add to
// inbound_, outbound_ or conns_ until all indexed loops have finished
std::vector<PeerEvent> PeerManager::handle_events(const std::span<pollfd> pfds) {
    std::vector<PeerEvent> events;
    const auto in = pfds.subspan(inbound_at_, inbound_len_);

    // 1a. Inbound handshakes: promote each one that verified, then queue ours.
    for (auto&[sock, peer] : inbound_.advance(in)) {
        const uint32_t id = promote(std::move(sock), peer, events);
        enqueue(conns_.at(id), build_handshake(ctx_));
    }

    // 1b. Outbound handshakes: promote each one that verified. Ours already went out.
    const auto out = pfds.subspan(outbound_at_, outbound_len_);
    for (auto& [sock, peer] : outbound_.advance(out))
        promote(std::move(sock), peer, events);

    // 2. Established peers, walked via the ids_ snapshot from build_fds().
    //    Peers promoted in step 1 aren't in ids_, so they're skipped this
    //    round and polled from the next build_fds().
    std::vector<uint32_t> to_drop;
    for (size_t k = 0; k < ids_.size(); ++k) {
        assert(conns_at_ + k < pfds.size());
        const short rev = pfds[conns_at_ + k].revents;
        if (rev == 0) continue;

        uint32_t id = ids_[k];
        auto it = conns_.find(id);
        if (it == conns_.end()) continue;
        PeerConnection& c = it->second;
        assert(pfds[conns_at_ + k].fd == c.sock.get());

        // Drain queued writes first so the peer stays fed.
        if ((rev & POLLOUT) && !c.write_buffer.empty()) {
            const std::vector<uint8_t>& front = c.write_buffer.front();
            ssize_t w = ::send(c.sock.get(), front.data() + c.cursor,
                               front.size() - c.cursor, MSG_NOSIGNAL);
            if (w < 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                    log(LogLevel::Debug, "peer {} dropped: send failed: {}",
                        id, strerror(errno));
                    to_drop.push_back(id);
                    continue;
                }
            } else {
                // a partial send only moves the cursor; the message is
                // freed once, when its last byte has gone to the kernel
                c.last_sent = std::chrono::steady_clock::now();
                c.cursor += static_cast<size_t>(w);
                if (c.cursor == front.size()) {
                    c.queued_bytes -= front.size();
                    c.write_buffer.pop_front();
                    c.cursor = 0;
                }

                c.bytes_up += w;
                c.round_up += w;
            }
        }

        if (!(rev & (POLLIN | POLLERR | POLLHUP))) continue;

        uint8_t chunk[16384];
        ssize_t n = ::recv(c.sock.get(), chunk, sizeof(chunk), 0);

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
        c.bytes_down += n;
        c.round_down += n;
        c.last_received = std::chrono::steady_clock::now();

        try {
            std::vector<uint8_t> msg;
            while (extract_message(c.read_buffer, msg))
                handle_message(id, msg, events);

            // a have or bitfield may have just made this peer a seeder; queue
            // the drop, since c is still in use until step 3
            if (am_seeding_ && c.has_pieces.full()) {
                log(LogLevel::Debug, "peer {} dropped: seeder, and so are we", id);
                to_drop.push_back(id);
            }
        } catch (const std::exception& e) {
            log(LogLevel::Warn, "protocol error from {}: {}", c.peer.host, e.what());
            to_drop.push_back(id);
        }
    }

    // 3. Drop idle peers along with everyone who failed above. A peer can be
    //    listed twice; drop() ignores ids that are already gone.
    const auto now = std::chrono::steady_clock::now();
    for (const auto& [id, c] : conns_) {
        if (now - c.last_received >= kIdleTimeout) {
            log(LogLevel::Debug, "peer {} dropped: idle", id);
            to_drop.push_back(id);
        }
    }
    for (uint32_t id : to_drop)
        drop(id, events);

    // 4. Keep-alives. Anything already queued reaches the peer anyway, so only
    //    idle connections need one.
    for (auto& c : conns_ | std::views::values) {
        if (c.write_buffer.empty() && now - c.last_sent >= kKeepAliveInterval)
            send_keep_alive(c.id);
    }

    // 5. Expire handshakes and connects that took too long.
    inbound_.expire();
    outbound_.expire();

    // 6. Accept new inbound connections. This adds to inbound_, so it must
    //    come after every indexed loop above.
    inbound_.accept(in, free_slots());

    // 7. Refill any slots freed this loop. New sockets join the next build_fds().
    outbound_.refill(free_slots());

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
        case MsgChoke: {
            log(LogLevel::Trace, "peer {} <- choke", peer_id);
            c.peer_choking = true;
            // record requests before clearing pending vector
            out.push_back({
                .type = PeerEvent::Choke,
                .peer_id = peer_id,
                .block = {},
                .req = {},
                .released = requests_of(c.pending)
            });
            c.pending.clear();
            break;
        }
        case MsgUnchoke:
            log(LogLevel::Trace, "peer {} <- unchoke", peer_id);
            c.peer_choking = false;
            out.push_back({PeerEvent::Unchoke, peer_id, {}, {}});
            break;

        case MsgInterested:
            log(LogLevel::Trace, "peer {} <- interested", peer_id);
            c.peer_interested = true;
            if (c.am_choking && open_slot()) {
                log(LogLevel::Debug, "peer {} interested and a slot is free; unchoking", peer_id);
                send_unchoke(peer_id);
            }
            break;

        case MsgNotInterested:
            log(LogLevel::Trace, "peer {} <- not_interested", peer_id);
            c.peer_interested = false;
            break;

        case MsgHave: {
            if (payload_len != 4) throw std::runtime_error("bad have");
            uint32_t index;
            std::memcpy(&index, payload, 4);
            index = ntohl(index);
            if (index >= c.has_pieces.size()) throw std::runtime_error("have out of range");

            if (c.has_pieces.get(index)) {
                log(LogLevel::Debug, "peer {} piece {} already registered, ignoring", peer_id, index);
                break;
            }
            log(LogLevel::Trace, "peer {} <- have piece {}", peer_id, index);
            c.has_pieces.set(index);
            ++piece_frequency_[index];
            break;
        }

        case MsgBitfield: {
            if (c.got_bitfield) break; // dropping the peer would be valid, but this
            c.got_bitfield = true;     // lenient implementation simply ignores it

            std::vector<uint8_t> raw(payload, payload + payload_len);
            c.has_pieces = Bitfield::from_bytes(raw, c.has_pieces.size());

            log(LogLevel::Trace, "peer {} <- bitfield ({} bytes)", peer_id, payload_len);
            apply_availability(c.has_pieces, +1);
            break;
        }

        case MsgRequest: {
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

            if (length > kBlockSize) throw std::runtime_error("request too large");

            log(LogLevel::Trace, "peer {} <- request piece {} off {} len {}",
                peer_id, index, begin, length);
            out.push_back({PeerEvent::Request, peer_id, {}, {index, begin, length}});
            break;
        }

        case MsgPiece: {
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
            const auto length = static_cast<uint32_t>(block.data.size());
            const auto match = std::ranges::find_if(c.pending, [&](const PendingRequest& p) {
                return p.req.piece_index == index && p.req.offset == begin && p.req.length == length;
            });
            if (match != c.pending.end()) {
                c.pending.erase(match);
                c.snubbed = false;

                out.push_back({.type = PeerEvent::Piece, .peer_id = peer_id, .block = std::move(block)});
            } else
                log(LogLevel::Debug, "peer {} <- piece {} off {}: not pending (late or unrequested)",
                    peer_id, index, begin);
            break;
        }

        case MsgCancel: {
            if (payload_len != 12) throw std::runtime_error("bad cancel");
            uint32_t index, begin, length;
            std::memcpy(&index,  payload,     4);
            std::memcpy(&begin,  payload + 4, 4);
            std::memcpy(&length, payload + 8, 4);
            index = ntohl(index); begin = ntohl(begin); length = ntohl(length);

            if (length > kBlockSize) throw std::runtime_error("cancel too large");

            // a request from this batch hasn't reached Session yet: drop it before
            // it costs a disk read. `out` holds every peer's events for this round,
            // so match on peer_id too
            const auto pending = std::find_if(out.rbegin(), out.rend(), [&](const PeerEvent& ev) {
                return ev.type == PeerEvent::Request && ev.peer_id == peer_id
                    && ev.req.piece_index == index && ev.req.offset == begin
                    && ev.req.length == length;
            });
            if (pending != out.rend()) {
                out.erase(std::next(pending).base());
                log(LogLevel::Trace, "peer {} <- cancel piece {} off {} len {} (before read)",
                    peer_id, index, begin, length);
                break;
            }

            const bool removed = remove_queued_pieces(c, [&](uint32_t i, uint32_t b, uint32_t l) {
                return i == index && b == begin && l == length;
            }) > 0;
            log(LogLevel::Trace, "peer {} <- cancel piece {} off {} len {}{}",
                peer_id, index, begin, length, removed ? "" : " (no queued match)");
            break;
        }

        default:
            log(LogLevel::Trace, "peer {} <- unknown msg id {} ({} byte payload)",
                peer_id, id, payload_len);
            break;   // unknown
    }
}

// Unchoke the kRegularSlots interested peers with the best rate this round,
// choke everyone else, then start a new round. While leeching, the rate is how
// fast a peer sends. While seeding, it's how fast they receive from us
void PeerManager::rechoke(const bool seeding) {
    const auto rate = [seeding](const PeerConnection* c) {
        return seeding ? c->round_up : c->round_down;
    };

    std::vector<PeerConnection*> interested;
    interested.reserve(conns_.size());
    for (auto& c : conns_ | std::views::values)
        if (c.peer_interested) interested.push_back(&c);

    // only the winners need to be in order; clamp so 'middle' never passes end()
    const size_t winners = std::min(kRegularSlots, interested.size());
    std::ranges::partial_sort(interested, interested.begin() + winners,
                              std::ranges::greater{}, rate);
    const auto top = std::span(interested).first(winners);

    for (size_t i = 0; i < top.size(); ++i)
        log(LogLevel::Trace, "rechoke: rank {} peer {} ({} bytes this round)",
            i + 1, top[i]->id, rate(top[i]));

    for (auto& c : conns_ | std::views::values) {
        const bool unchoke = c.id == optimistic_
                  || std::ranges::find(top, &c) != top.end();
        if (unchoke && c.am_choking)
            send_unchoke(c.id);
        else if (!unchoke && !c.am_choking)
            send_choke(c.id);

        c.round_down = 0;
        c.round_up   = 0;
    }
    log(LogLevel::Debug, "rechoke ({}): {} interested, top {} by {}",
    seeding ? "seeding" : "leeching", interested.size(), winners,
    seeding ? "upload" : "download");
}

// give one random choked, interested peer a slot until the next pick
void PeerManager::optimistic_unchoke() {
    std::vector<uint32_t> candidates;
    for (const auto& c : conns_ | std::views::values)
        if (c.peer_interested && c.am_choking) candidates.push_back(c.id);
    if (candidates.empty()) return;   // keep the current one

    std::uniform_int_distribution<size_t> dist(0, candidates.size() - 1);
    optimistic_ = candidates[dist(rng_)];
    send_unchoke(*optimistic_);

    log(LogLevel::Debug, "optimistic unchoke: peer {} (from {} candidate(s))",
    *optimistic_, candidates.size());
}

void PeerManager::send_keep_alive(uint32_t peer_id) {
    auto it = conns_.find(peer_id);
    if (it == conns_.end()) return;

    // Keep-alive is just a 4-byte zero
    // length prefix; it has no message ID.
    enqueue(it->second, std::vector<uint8_t>(4, 0));
}

void PeerManager::send_interested(uint32_t peer_id) {
    auto it = conns_.find(peer_id);
    if (it == conns_.end()) return;
    it->second.am_interested = true;
    enqueue(it->second, build_message(MsgInterested));
}

void PeerManager::send_choke(uint32_t peer_id) {
    auto it = conns_.find(peer_id);
    if (it == conns_.end()) return;
    it->second.am_choking = true;
    enqueue(it->second, build_message(MsgChoke));
    const size_t dropped = remove_queued_pieces(it->second, [](uint32_t, uint32_t, uint32_t) { return true; });
    log(LogLevel::Trace, "peer {} -> choke ({} queued piece(s) dropped)", peer_id, dropped);
}

void PeerManager::send_to(uint32_t peer_id, std::vector<uint8_t> msg) {
    auto it = conns_.find(peer_id);
    if (it != conns_.end())
        enqueue(it->second, std::move(msg));
}

void PeerManager::send_bitfield(uint32_t peer_id, const Bitfield& our_have) {
    log(LogLevel::Trace, "peer {} -> bitfield ({} bytes)", peer_id, our_have.bytes().size());
    send_to(peer_id, build_message(MsgBitfield, our_have.bytes()));
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
    send_to(peer_id, build_message(MsgPiece, payload));
}

void PeerManager::send_unchoke(uint32_t peer_id) {
    auto it = conns_.find(peer_id);
    if (it == conns_.end()) return;
    it->second.am_choking = false;
    log(LogLevel::Trace, "peer {} -> unchoke", peer_id);
    send_to(peer_id, build_message(MsgUnchoke));
}

void PeerManager::send_request(uint32_t peer_id, const BlockRequest& req) {
    auto it = conns_.find(peer_id);
    if (it == conns_.end()) return;
    it->second.pending.push_back({.req = req, .sent_at = std::chrono::steady_clock::now()});
    log(LogLevel::Trace, "peer {} -> request piece {} off {} len {}",
        peer_id, req.piece_index, req.offset, req.length);
    send_to(peer_id, build_request(req));
}

void PeerManager::broadcast_have(uint32_t index) {
    std::vector<uint8_t> payload(4);
    uint32_t idx = htonl(index);
    std::memcpy(payload.data(), &idx, 4);
    auto msg = build_message(MsgHave, payload);

    for (auto &c: conns_ | std::views::values)
        enqueue(c, msg);   // copy: every peer needs its own
}

void PeerManager::enqueue(PeerConnection& c, std::vector<uint8_t> msg) {
    c.queued_bytes += msg.size();
    c.write_buffer.push_back(std::move(msg));
}

void PeerManager::apply_availability(const Bitfield& bf, int delta) {
    for (uint32_t i = 0; i < piece_frequency_.size(); ++i)
        if (bf.get(i)) piece_frequency_[i] += delta;
}

// a regular slot is free: fewer than kRegularSlots peers are unchoked,
// not counting the optimistic one, which has a slot of its own
bool PeerManager::open_slot() const {
    size_t unchoked = 0;
    for (const auto& c : conns_ | std::views::values)
        if (!c.am_choking && c.id != optimistic_) ++unchoked;
    return unchoked < kRegularSlots;
}

// Subtract the peer's pieces from the availability counts used by rarest-first,
// forget its address so it can be dialed again, and report a Dropped event.
void PeerManager::drop(uint32_t id, std::vector<PeerEvent>& out) {
    auto it = conns_.find(id);
    if (it == conns_.end()) return;

    apply_availability(it->second.has_pieces, -1);
    outbound_.forget(it->second.peer);

    // record pending requests before dropping peer, so
    // PieceManager is able to know what requests to reset
    out.push_back({
        .type = PeerEvent::Dropped,
        .peer_id = id,
        .block = {},
        .req = {},
        .released = requests_of(it->second.pending)
    });
    conns_.erase(it);
}

std::vector<PeerEvent> PeerManager::ban(const std::string& host) {
    if (!banned_.insert(host).second) return {};    // already banned

    std::vector<uint32_t> ids;
    for (const auto& [id, c] : conns_)
        if (c.peer.host == host) ids.push_back(id);

    std::vector<PeerEvent> events;
    for (uint32_t id : ids)
        drop(id, events);
    log(LogLevel::Debug, "banned {}: dropped {} connection(s)", host, ids.size());
    return events;
}

std::vector<PeerEvent> PeerManager::on_download_complete() {
    set_seeding();
    std::vector<uint32_t> ids;
    for (const auto &c: conns_ | std::views::values)
        if (c.has_pieces.full()) ids.push_back(c.id);
    std::vector<PeerEvent> events;
    for (uint32_t id : ids)
        drop(id, events);
    log(LogLevel::Debug, "dropped {} seeders", ids.size());
    return events;
}

bool PeerManager::can_queue(uint32_t peer_id) const {
    auto it = conns_.find(peer_id);
    if (it == conns_.end()) return false;   // dropped or banned earlier in this batch
    return it->second.queued_bytes < kMaxQueuedBytes;
}

void PeerManager::add_peers(std::vector<Peer> peers) {
    outbound_.add(std::move(peers));
    outbound_.refill(free_slots());
}

// slots left under max_peers_, counting connected peers and both handshake queues
size_t PeerManager::free_slots() const {
    const size_t used = conns_.size() + inbound_.size() + outbound_.size();
    return used < max_peers_ ? max_peers_ - used : 0;
}

uint32_t PeerManager::promote(UniqueFd sock, const Peer& peer, std::vector<PeerEvent>& out) {
    PeerConnection c;
    c.id         = next_id_++;
    c.sock       = std::move(sock);
    c.peer       = peer;
    c.has_pieces = Bitfield(piece_count_);

    auto now = std::chrono::steady_clock::now();
    c.last_sent     = now;
    c.last_received = now;

    const uint32_t id = c.id;
    conns_.emplace(id, std::move(c));
    out.push_back({.type = PeerEvent::Joined, .peer_id = id, .block = {}, .req = {}});
    return id;
}