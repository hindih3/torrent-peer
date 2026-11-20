#pragma once
#include <deque>
#include <span>
#include <string>
#include <unordered_set>
#include <vector>
#include <poll.h>

#include "handshake.hpp"
#include "core/unique_fd.hpp"

// A dialed socket that has not finished its handshake yet:
// Connecting waits for the TCP connect, Sending pushes our handshake out
// (possibly over several polls), Receiving waits for the peer's reply
struct PendingOutbound {
    enum class Phase { Connecting, Sending, Receiving } phase = Phase::Connecting;
    UniqueFd                              sock;
    Peer                                  peer;
    std::array<uint8_t, kHandshakeSize>   buffer{};       // the peer's handshake
    size_t                                received = 0;
    size_t                                sent     = 0;   // bytes of ours already sent
    std::chrono::steady_clock::time_point deadline{};
};

class Outbound {
public:
    explicit Outbound(const HandshakeContext& ctx);

    // Queue addresses to dial. Anything already queued, dialing or connected is skipped.
    void add(std::vector<Peer> peers);
    // Dial queued addresses while slots are free and the half-open cap allows
    void refill(size_t free_slots);
    // A connected peer was dropped: allow its address to be queued again
    void forget(const Peer& peer);

    void append_fds(std::vector<pollfd>& pfds) const;               // [pending...]
    std::vector<CompletedHandshake> advance(std::span<const pollfd> fds);
    void expire();

    [[nodiscard]] size_t size()  const { return pending_.size(); }
    [[nodiscard]] bool   empty() const { return pending_.empty() && candidates_.empty(); }

private:
    HandshakeContext                ctx_;
    std::vector<uint8_t>            handshake_;     // ours, built once from ctx_
    std::vector<PendingOutbound>    pending_;
    std::deque<Peer>                candidates_;    // addresses waiting for a free slot
    std::unordered_set<std::string> known_;         // "host:port" queued, dialing or connected

    bool            dial(const Peer& peer);
    HandshakeResult advance_one(PendingOutbound& p, short rev);
    HandshakeResult send_handshake(PendingOutbound& p) const;

    static std::string key(const Peer& p) { return p.host + ":" + p.port; }
};