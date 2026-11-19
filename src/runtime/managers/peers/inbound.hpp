#pragma once
#include <sys/poll.h>
#include <span>

#include "utils.hpp"
#include "core/unique_fd.hpp"

// An accepted socket that has not finished its handshake yet. Kept out of
// conns_ so the peer-wire framer never sees the 68 handshake bytes.
struct PendingInbound {
    UniqueFd                              sock;
    Peer                                  peer;
    std::array<uint8_t, kHandshakeSize>   buffer{};
    size_t                                received = 0;
    std::chrono::steady_clock::time_point started{};
};

class Inbound {
public:
    Inbound(const HandshakeContext &ctx, uint16_t port, size_t max_peers);

    void append_fds(std::vector<pollfd>& pfds) const;               // [listen?][pending...]
    std::vector<CompletedHandshake> advance(std::span<const pollfd> fds);

    void expire();
    void accept(std::span<const pollfd> fds, size_t others);

    [[nodiscard]] size_t size()      const { return pending_.size(); }
    [[nodiscard]] bool   empty()     const { return pending_.empty() && !listen_sock_; }
    [[nodiscard]] bool   listening() const { return listen_sock_.valid(); }

private:
    HandshakeContext                ctx_;
    UniqueFd                        listen_sock_;
    size_t                          max_peers_;
    std::vector<PendingInbound>     pending_;

    HandshakeResult advance_one(PendingInbound &p);
};
