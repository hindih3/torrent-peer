#include "inbound.hpp"

#include <cassert>
#include <fcntl.h>
#include <arpa/inet.h>
#include <netinet/in.h>

namespace {
    UniqueFd build_listen_fd(uint16_t port) {
        UniqueFd sock(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
        if (!sock) {
            log(LogLevel::Warn, "listen: socket failed: {} (inbound peers disabled)",
                strerror(errno));
            return {};
        }

        int yes = 1;
        // reuse port on restart
        setsockopt(sock.get(), SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port        = htons(port);

        if (::bind(sock.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            log(LogLevel::Warn, "listen: bind {} failed: {} (inbound peers disabled)",
                port, strerror(errno));
            return {};
        }
        if (::listen(sock.get(), 32) < 0) {
            log(LogLevel::Warn, "listen: {} (inbound peers disabled)", strerror(errno));
            return {};
        }
        return sock;
    }
}

Inbound::Inbound(const HandshakeContext &ctx, uint16_t port, size_t max_peers)
    : ctx_(ctx),
      listen_sock_(build_listen_fd(port)),
      max_peers_(max_peers) {}

void Inbound::append_fds(std::vector<pollfd>& pfds) const {
    if (listen_sock_)
        pfds.push_back({.fd = listen_sock_.get(), .events = POLLIN, .revents = 0});
    for (const auto& p : pending_)
        pfds.push_back({.fd = p.sock.get(), .events = POLLIN, .revents = 0});
}

// Survivors are rebuilt into a new vector rather than erased in place,
// so pending_[i] keeps lining up with its pollfd for the whole loop.
std::vector<CompletedHandshake> Inbound::advance(const std::span<const pollfd> fds) {
    std::vector<CompletedHandshake> done;
    if (pending_.empty()) return done;

    const size_t first = listen_sock_ ? 1 : 0;   // fds is [listen?][pending...]
    std::vector<PendingInbound> still_pending;
    still_pending.reserve(pending_.size());

    for (size_t i = 0; i < pending_.size(); ++i) {
        assert(first + i < fds.size());
        PendingInbound& p = pending_[i];
        assert(fds[first + i].fd == p.sock.get());
        const short rev = fds[first + i].revents;
        if (rev == 0) {
            still_pending.push_back(std::move(p));
            continue;
        }

        HandshakeResult r = HandshakeResult::Drop;
        if (rev & (POLLIN | POLLERR | POLLHUP))
            r = advance_one(p);

        if (r == HandshakeResult::Keep)
            still_pending.push_back(std::move(p));
        else if (r == HandshakeResult::Complete)
            done.push_back({std::move(p.sock), std::move(p.peer)});
        // Drop: p stays in the old pending_ and closes when it's replaced below.
    }
    pending_ = std::move(still_pending);
    return done;
}

void Inbound::expire() {
    const auto cutoff = std::chrono::steady_clock::now() - kHandshakeTimeout;
    for (auto it = pending_.begin(); it != pending_.end(); ) {
        if (it->started < cutoff) {
            log(LogLevel::Debug, "inbound {}: handshake timed out", it->peer.host);
            it = pending_.erase(it);
        } else {
            ++it;
        }
    }
}

// Drain the backlog. A single poll wakeup can cover several pending
// connections, so this loops until accept() says there is nothing left.
void Inbound::accept(const std::span<const pollfd> fds, const size_t others) {
    if (!listen_sock_ || !(fds[0].revents & POLLIN)) return;

    while (true) {
        sockaddr_in addr{};
        socklen_t   len = sizeof(addr);
        UniqueFd sock(::accept(listen_sock_.get(), reinterpret_cast<sockaddr*>(&addr), &len));

        if (!sock) {
            if (errno == ECONNABORTED || errno == EINTR) continue;
            break;   // EAGAIN/EWOULDBLOCK: backlog drained
        }

        if (others + pending_.size() >= max_peers_) {
            continue;  // sock closes here: at capacity, let them retry later
        }

        int flags = fcntl(sock.get(), F_GETFL, 0);
        fcntl(sock.get(), F_SETFL, flags | O_NONBLOCK);

        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));

        Peer peer{std::string(ip), std::to_string(ntohs(addr.sin_port))};
        if (ctx_.banned.contains(peer.host)) continue;  // sock closes here: banned for bad data
        log(LogLevel::Debug, "inbound: {}:{}", peer.host, peer.port);

        pending_.push_back({
            .sock    = std::move(sock),
            .peer    = std::move(peer),
            .started = std::chrono::steady_clock::now()
        });
    }
}

// An inbound peer speaks first: read and verify their handshake here,
// and PeerManager sends ours once it has promoted the connection.
HandshakeResult Inbound::advance_one(PendingInbound& p) {
    if (const HandshakeResult r = read_handshake(p.sock.get(), p.buffer, p.received);
        r != HandshakeResult::Complete)
        return r;

    if (!verify_handshake(ctx_, p.buffer, p.peer))
        return HandshakeResult::Drop;

    log(LogLevel::Debug, "handshake ok (inbound): {}:{}", p.peer.host, p.peer.port);
    return HandshakeResult::Complete;
}