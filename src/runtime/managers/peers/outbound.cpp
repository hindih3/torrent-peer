#include "outbound.hpp"

#include <cassert>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "core/log.hpp"

namespace
{
    constexpr size_t kMaxHalfOpen    = 20;   // concurrent dials, leaves room for inbound
    constexpr size_t kMaxCandidates  = 1000;    // bound on queued addresses

    constexpr auto   kConnectTimeout = std::chrono::seconds(10);
}

Outbound::Outbound(const HandshakeContext& ctx)
    : ctx_(ctx),
      handshake_(build_handshake(ctx_)) {}

void Outbound::add(std::vector<Peer> peers) {
    size_t added = 0;
    for (auto& p : peers) {
        if (candidates_.size() >= kMaxCandidates) break;
        if (!known_.insert(key(p)).second) continue;
        candidates_.push_back(std::move(p));
        ++added;
    }
    log(LogLevel::Debug, "queued {} new peer(s), {} waiting", added, candidates_.size());
}

// A failed dial never takes a slot, so only a successful one counts against free_slots.
void Outbound::refill(size_t free_slots) {
    while (!candidates_.empty() && pending_.size() < kMaxHalfOpen && free_slots > 0) {
        Peer p = std::move(candidates_.front());
        candidates_.pop_front();
        if (ctx_.banned.contains(p.host) || !dial(p))
            known_.erase(key(p));
        else
            --free_slots;
    }
}

void Outbound::forget(const Peer& peer) {
    known_.erase(key(peer));
}

// Connecting sockets become writable when the TCP connect finishes, Sending
// ones when there is room for the rest of our handshake. Receiving ones
// become readable when the peer's reply arrives.
void Outbound::append_fds(std::vector<pollfd>& pfds) const {
    for (const auto& p : pending_) {
        const short ev = p.phase == PendingOutbound::Phase::Receiving ? POLLIN : POLLOUT;
        pfds.push_back({.fd = p.sock.get(), .events = ev, .revents = 0});
    }
}

// Survivors are rebuilt into a new vector rather than erased in place,
// so pending_[i] keeps lining up with fds[i] for the whole loop.
std::vector<CompletedHandshake> Outbound::advance(const std::span<const pollfd> fds) {
    std::vector<CompletedHandshake> done;
    if (pending_.empty()) return done;

    std::vector<PendingOutbound> still_pending;
    still_pending.reserve(pending_.size());

    for (size_t i = 0; i < pending_.size(); ++i) {
        assert(i < fds.size());
        PendingOutbound& p = pending_[i];
        assert(fds[i].fd == p.sock.get());
        const short rev = fds[i].revents;

        const HandshakeResult r = rev ? advance_one(p, rev) : HandshakeResult::Keep;
        if (r == HandshakeResult::Keep)
            still_pending.push_back(std::move(p));
        else if (r == HandshakeResult::Complete)
            done.push_back({std::move(p.sock), std::move(p.peer)});  // stays in known_ until forget()
        else
            known_.erase(key(p.peer));
        // Drop: p stays in the old pending_ and closes when it's replaced below.
    }
    pending_ = std::move(still_pending);
    return done;
}

void Outbound::expire() {
    const auto now = std::chrono::steady_clock::now();
    for (auto it = pending_.begin(); it != pending_.end(); ) {
        if (now >= it->deadline) {
            log(LogLevel::Trace, "outbound {}:{}: timed out", it->peer.host, it->peer.port);
            known_.erase(key(it->peer));
            it = pending_.erase(it);
        } else {
            ++it;
        }
    }
}

bool Outbound::dial(const Peer& peer) {
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

    UniqueFd sock(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    if (!sock) {
        log(LogLevel::Warn, "outbound socket() failed: {}", strerror(errno));
        return false;
    }

    // Non-blocking connect: EINPROGRESS is the normal answer. The result
    // arrives later as POLLOUT, and SO_ERROR says whether it worked.
    if (::connect(sock.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0
        && errno != EINPROGRESS) {
        log(LogLevel::Debug, "outbound {}:{}: connect failed: {}",
            peer.host, peer.port, strerror(errno));
        return false;                               // sock closes here
    }

    pending_.push_back({
        .sock     = std::move(sock),
        .peer     = peer,
        .deadline = std::chrono::steady_clock::now() + kConnectTimeout,
    });

    log(LogLevel::Trace, "dialing {}:{}", peer.host, peer.port);
    return true;
}

HandshakeResult Outbound::advance_one(PendingOutbound& p, const short rev) {
    using Phase = PendingOutbound::Phase;

    if (p.phase == Phase::Connecting) {
        int err = 0;
        socklen_t len = sizeof(err);
        if (getsockopt(p.sock.get(), SOL_SOCKET, SO_ERROR, &err, &len) < 0) err = errno;
        if (err != 0) {
            log(LogLevel::Trace, "outbound {}:{}: connect failed: {}",
                p.peer.host, p.peer.port, strerror(err));
            return HandshakeResult::Drop;
        }

        // The handshake timeout covers both sending ours and receiving theirs.
        p.phase    = Phase::Sending;
        p.deadline = std::chrono::steady_clock::now() + kHandshakeTimeout;
        // The socket just became writable, so send now rather than waiting a poll.
    }

    if (p.phase == Phase::Sending)
        return send_handshake(p);

    // Receiving: same capped read as inbound, so anything the peer sends
    // behind its handshake (usually the bitfield) stays for the normal path.
    if (!(rev & (POLLIN | POLLERR | POLLHUP))) return HandshakeResult::Keep;

    if (const HandshakeResult r = read_handshake(p.sock.get(), p.buffer, p.received);
        r != HandshakeResult::Complete)
        return r;

    if (!verify_handshake(ctx_, p.buffer, p.peer))
        return HandshakeResult::Drop;

    log(LogLevel::Debug, "handshake ok (outbound): {}:{}", p.peer.host, p.peer.port);
    return HandshakeResult::Complete;
}

// Sends whatever is left of our handshake. A partial write keeps the socket
// in Sending, and the next POLLOUT picks up at p.sent.
HandshakeResult Outbound::send_handshake(PendingOutbound& p) const {
    const ssize_t w = ::send(p.sock.get(), handshake_.data() + p.sent,
                             handshake_.size() - p.sent, MSG_NOSIGNAL);
    if (w < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            return HandshakeResult::Keep;
        log(LogLevel::Debug, "outbound {}:{}: handshake send failed: {}",
            p.peer.host, p.peer.port, strerror(errno));
        return HandshakeResult::Drop;
    }

    p.sent += static_cast<size_t>(w);
    if (p.sent < handshake_.size()) {
        log(LogLevel::Debug, "outbound {}:{}: partial handshake write ({} of {} bytes)",
            p.peer.host, p.peer.port, p.sent, handshake_.size());
        return HandshakeResult::Keep;
    }

    p.phase = PendingOutbound::Phase::Receiving;
    return HandshakeResult::Keep;
}
