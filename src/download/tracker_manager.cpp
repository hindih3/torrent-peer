#include "tracker_manager.hpp"

#include <fcntl.h>
#include <netdb.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <utility>
#include <vector>
#include <algorithm>

#include "common.hpp"

namespace {
    std::chrono::seconds response_timeout(const TrackerSession& t) {
        return std::chrono::seconds(15 << t.retries);   // BEP 15: 15 * 2^n
    }

    uint32_t read_be32(const uint8_t* p) {
        uint32_t v;
        std::memcpy(&v, p, 4);
        return ntohl(v);
    }

    struct AnnounceReply {
        enum Kind { Ignore, Ok, Error } kind = Ignore;
        uint32_t interval = 0, leechers = 0, seeders = 0;
        std::vector<Peer> peers;
        std::string error;
    };

    // pure function that only extracts meaning. Doesn't alter sockets or state
    AnnounceReply parse_announce_reply(const uint8_t* buf, size_t n, uint32_t expect_txn) {
        AnnounceReply r;
        if (n < 8) return r;

        const uint32_t action = read_be32(buf);
        const uint32_t txn    = read_be32(buf + 4);
        if (txn != expect_txn) return r;                 // txn_id mismatch

        if (action == 3) {                               // tracker-reported error
            r.kind = AnnounceReply::Error;
            r.error.assign(reinterpret_cast<const char*>(buf + 8), n - 8);
            return r;
        }
        if (action != 1 || n < 20) return r;             // not an announce reply

        r.kind     = AnnounceReply::Ok;
        r.interval = read_be32(buf + 8);
        r.leechers = read_be32(buf + 12);
        r.seeders  = read_be32(buf + 16);

        for (size_t off = 20; off + 6 <= n; off += 6) {
            uint16_t port;
            std::memcpy(&port, buf + off + 4, 2);
            port = ntohs(port);
            if (port == 0) continue;                     // garbage entry

            char ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, buf + off, ip, sizeof ip);
            r.peers.push_back({ip, std::to_string(port)});
        }
        return r;
    }
}

TrackerManager::TrackerManager(const TorrentFile& torrent,
                               std::string peer_id, uint16_t listen_port)
    : torrent_(torrent),
      peer_id_(std::move(peer_id)),
      listen_port_(listen_port),
      left_(torrent.total_length),
      rng_(std::random_device{}()),
      key_(static_cast<uint32_t>(rng_()))
{
    log(LogLevel::Debug, "initialising tracker manager (listen port {}, {} announce tier(s))",
        listen_port_, torrent.announce_list.size());

    for (auto& tier : torrent.announce_list) {
        for (auto& url : tier) {
            try {
                TrackerSession tracker;
                tracker.address = parse_tracker_url(url);
                tracker.key = key_;
                log(LogLevel::Debug, "added tracker {}:{}",
                    tracker.address.host, tracker.address.port);
                trackers_.push_back(std::move(tracker));
            } catch (const std::exception& e) {
                log(LogLevel::Debug, "skipping tracker {}: {}", url, e.what());
            }
        }
    }

    if (trackers_.empty())
        log(LogLevel::Warn, "no usable trackers found in torrent");
    else
        log(LogLevel::Info, "loaded {} tracker(s)", trackers_.size());
}

std::vector<uint8_t> TrackerManager::build_connect_request(const uint32_t transaction_id) {
    log(LogLevel::Trace, "building connect request (txn={:#010x})", transaction_id);

    std::vector<uint8_t> packet(16);

    uint64_t protocol_id = htobe64(BITTORRENT_PROTOCOL);
    uint32_t action      = htonl(0);
    uint32_t txn_id      = htonl(transaction_id);

    memcpy(packet.data(),      &protocol_id, 8);
    memcpy(packet.data() + 8,  &action,      4);
    memcpy(packet.data() + 12, &txn_id,      4);

    return packet;
}

std::vector<uint8_t> TrackerManager::build_announce_request(
    const TrackerSession& t, TrackerEvent e, const std::string& peer_id,
    const std::array<uint8_t,20>& info_hash, const uint16_t port, const AnnounceParams& p) {

    if (peer_id.size() != 20)
        throw std::runtime_error("peer_id must be exactly 20 bytes");

    log(LogLevel::Trace,
        "building announce request for {}:{} (txn={:#010x}, event={}, port={}, "
        "downloaded={}, left={}, uploaded={})",
        t.address.host, t.address.port, t.transaction_id,
        static_cast<uint32_t>(e), port, p.downloaded, p.left, p.uploaded);

    std::vector<uint8_t> packet(98);
    uint64_t conn_id_be = htobe64(t.connection_id);
    uint32_t action_be  = htonl(1);
    uint32_t txn_be     = htonl(t.transaction_id);
    uint64_t down_be    = htobe64(p.downloaded);
    uint64_t left_be    = htobe64(p.left);
    uint64_t up_be      = htobe64(p.uploaded);
    uint32_t event_be   = htonl(e);
    uint32_t ip         = 0;           // default IP
    uint32_t key        = htonl(t.key);
    int32_t  num_want   = htonl(-1);   // request default num of peers
    uint16_t port_be    = htons(port);

    memcpy(packet.data(),      &conn_id_be, 8);
    memcpy(packet.data() + 8,  &action_be,  4);
    memcpy(packet.data() + 12, &txn_be,     4);
    memcpy(packet.data() + 16, info_hash.data(), 20);
    memcpy(packet.data() + 36, peer_id.data(),   20);
    memcpy(packet.data() + 56, &down_be,    8);
    memcpy(packet.data() + 64, &left_be,    8);
    memcpy(packet.data() + 72, &up_be,      8);
    memcpy(packet.data() + 80, &event_be,   4);
    memcpy(packet.data() + 84, &ip,         4);
    memcpy(packet.data() + 88, &key,        4);
    memcpy(packet.data() + 92, &num_want,   4);
    memcpy(packet.data() + 96, &port_be,    2);
    return packet;
}

uint32_t TrackerManager::next_random() {
    std::uniform_int_distribution<uint32_t> dist;
    return dist(rng_);
}

void TrackerManager::open_socket(TrackerSession& t) {
    addrinfo hints{}, *res;
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    log(LogLevel::Trace, "resolving {}:{}", t.address.host, t.address.port);

    if (int rc = getaddrinfo(t.address.host.c_str(), t.address.port.c_str(),
                             &hints, &res); rc != 0)
        throw std::runtime_error("DNS resolution failed for " + t.address.host +
                                 ": " + gai_strerror(rc));

    int fd = createUDPIpv4Socket();
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);

    if (connect(fd, res->ai_addr, res->ai_addrlen) == -1) {
        // capture before close()
        const int err = errno;
        freeaddrinfo(res);
        close(fd);
        throw std::runtime_error("connect failed for " + t.address.host +
                                 ": " + std::strerror(err));
    }
    freeaddrinfo(res);
    t.sockfd = fd;
    log(LogLevel::Trace, "opened socket fd={} to {}:{}", fd, t.address.host, t.address.port);
}

void TrackerManager::fail_backoff(TrackerSession& t) {
    if (t.sockfd >= 0) { close(t.sockfd); t.sockfd = -1; }
    t.state = TrackerState::Disconnected;
    t.retries = std::min(t.retries + 1, 8);
    auto delay = std::chrono::seconds(15 * (1 << t.retries)); // 15·2^n
    t.next_action = std::chrono::steady_clock::now() + delay;

    log(LogLevel::Debug, "{}:{} backing off for {}s (failure #{})",
        t.address.host, t.address.port, delay.count(), t.retries);
    if (t.retries >= 8)
        log(LogLevel::Warn, "{}:{} keeps failing, retrying only every {}s. Might be dead",
            t.address.host, t.address.port, delay.count());
}

void TrackerManager::send_connect(TrackerSession& t) {
    if (t.sockfd < 0) {
        try {
            open_socket(t);
        } catch (const std::exception& e) {
            log(LogLevel::Debug, "connect failed for {}: {}", t.address.host, e.what());
            fail_backoff(t);
            return;
        }
    }

    t.transaction_id = next_random();
    auto packet = build_connect_request(t.transaction_id);
    if (::send(t.sockfd, packet.data(), packet.size(), 0) < 0) {
        const int err = errno;   // capture before fail_backoff() calls close()
        log(LogLevel::Debug, "send connect to {}:{} failed: {}",
            t.address.host, t.address.port, std::strerror(err));
        fail_backoff(t);
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    t.state        = TrackerState::Connecting;
    t.connected_at = now;                        // send time, for the RTT log
    t.next_action  = now + response_timeout(t);  // give up if no reply by then
    log(LogLevel::Debug, "sent connect request to {}:{} (txn={:#010x})",
        t.address.host, t.address.port, t.transaction_id);
}

// returns false only on a fatal socket error the caller must fail_backoff
// success and ignored-noise both return true (nothing for the caller to do)
bool TrackerManager::recv_connect(TrackerSession& t) {
    uint8_t buf[16];
    ssize_t n = ::recv(t.sockfd, buf, sizeof(buf), 0);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            return true;
        log(LogLevel::Debug, "recv from {}:{} failed: {}",
            t.address.host, t.address.port, std::strerror(errno));
        return false;
    }
    if (n < 16) {
        log(LogLevel::Trace, "ignoring short connect response from {}:{} ({} bytes)",
            t.address.host, t.address.port, n);
        return true;
    }
    uint32_t action, txn;
    memcpy(&action, buf,     4);   action = ntohl(action);
    memcpy(&txn,buf + 4, 4);   txn    = ntohl(txn);

    if (action != 0 || txn != t.transaction_id) {
        log(LogLevel::Trace,
            "ignoring unexpected connect response from {}:{} "
            "(action={}, txn={:#010x}, expected txn={:#010x})",
            t.address.host, t.address.port, action, txn, t.transaction_id);
        return true;
    }

    uint64_t cid; memcpy(&cid, buf + 8, 8);
    const auto now = std::chrono::steady_clock::now();
    const auto rtt = std::chrono::duration_cast<std::chrono::milliseconds>(
                         now - t.connected_at);

    t.connection_id = be64toh(cid);
    t.connected_at  = now;
    t.retries       = 0;
    t.state         = TrackerState::Connected;

    log(LogLevel::Debug, "connected to tracker {}:{} ({}ms)",
        t.address.host, t.address.port, rtt.count());
    log(LogLevel::Trace, "{}:{} connection_id={:#x}",
        t.address.host, t.address.port, t.connection_id);
    return true;
}

TrackerEvent TrackerManager::pending_event(const TrackerSession& t) const {
    switch (t.reported) {
        case REPORTED_NOTHING:   return EVENT_STARTED;
        case REPORTED_STARTED:   return download_complete_ ? EVENT_COMPLETED : EVENT_NONE;
        case REPORTED_COMPLETED: return EVENT_NONE;
    }
    return EVENT_NONE;
}

void TrackerManager::send_announce(TrackerSession& t) {
    const auto now = std::chrono::steady_clock::now();

    if (now - t.connected_at >= std::chrono::seconds(60)) {
        log(LogLevel::Debug, "{}:{} connection id expired, reconnecting",
            t.address.host, t.address.port);
        t.state       = TrackerState::Disconnected;
        t.next_action = now;
        return;
    }

    const TrackerEvent event = pending_event(t);
    t.transaction_id = next_random();
    t.in_flight      = event;

    const AnnounceParams params{
        .downloaded = downloaded_,
        .uploaded   = uploaded_,
        .left       = left_,
    };
    auto packet = build_announce_request(t, event, peer_id_, torrent_.info_hash,
                                         listen_port_, params);

    if (::send(t.sockfd, packet.data(), packet.size(), 0) < 0) {
        const int err = errno;
        log(LogLevel::Debug, "send announce to {}:{} failed: {}",
            t.address.host, t.address.port, std::strerror(err));
        fail_backoff(t);
        return;
    }

    t.state       = TrackerState::Announcing;
    t.next_action = now + response_timeout(t);

    log(LogLevel::Debug, "sent announce to {}:{} (event={}, txn={:#010x}, left={})",
    t.address.host, t.address.port, static_cast<uint32_t>(event),
    t.transaction_id, left_);
}

// Returns false only when the caller must fail_backoff.
bool TrackerManager::recv_announce(TrackerSession& t, std::vector<Peer>& out) {
    while (true) {
        ssize_t n = ::recv(t.sockfd, buf_.data(), buf_.size(), 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return true;   // nothing left
            log(LogLevel::Debug, "recv from {}:{} failed: {}",
                t.address.host, t.address.port, std::strerror(errno));
            return false;
        }

        AnnounceReply r = parse_announce_reply(buf_.data(), static_cast<size_t>(n),
                                               t.transaction_id);

        if (r.kind == AnnounceReply::Ignore) {
            log(LogLevel::Trace, "ignoring {} byte datagram from {}:{}",
                n, t.address.host, t.address.port);
            continue;                                // keep draining
        }
        if (r.kind == AnnounceReply::Error) {
            log(LogLevel::Warn, "{}:{} tracker error: {}",
                t.address.host, t.address.port, r.error);
            return false;
        }

        if (t.in_flight == EVENT_STARTED)   t.reported = REPORTED_STARTED;
        if (t.in_flight == EVENT_COMPLETED) t.reported = REPORTED_COMPLETED;

        const uint32_t interval = r.interval == 0 ? 1800u : r.interval;   // std::clamp needs all
        t.interval = std::clamp(interval, 300u, 86400u);        // 3 args the same type
        t.seeders  = r.seeders;
        t.leechers = r.leechers;
        t.retries  = 0;

        t.state       = TrackerState::Idle;
        t.next_action = std::chrono::steady_clock::now() + std::chrono::seconds(t.interval);

        log(LogLevel::Info, "{}:{}: {} peers, {} seeders, {} leechers, next announce in {}s",
            t.address.host, t.address.port, r.peers.size(),
            t.seeders, t.leechers, t.interval);

        out.insert(out.end(), std::make_move_iterator(r.peers.begin()),
                              std::make_move_iterator(r.peers.end()));
        return true;
    }
}