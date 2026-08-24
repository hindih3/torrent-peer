#pragma once
#include "net/peer.hpp"
#include "common.hpp"
#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

struct PeerEvent {
    enum Type { Unchoke, Piece, Dropped, Request, Joined } type;
    uint32_t     peer_id;
    Block        block;    // valid when type == Piece
    BlockRequest req{};    // valid when type == Request
};

enum MessageId : uint8_t {
    MSG_CHOKE = 0, MSG_UNCHOKE = 1, MSG_INTERESTED = 2, MSG_NOT_INTERESTED = 3,
    MSG_HAVE = 4, MSG_BITFIELD = 5, MSG_REQUEST = 6, MSG_PIECE = 7, MSG_CANCEL = 8
};

// An accepted socket that has not finished its handshake yet. Kept out of
// conns_ so the peer-wire framer never sees the 68 handshake bytes.
struct PendingInbound {
    int                                   sockfd = -1;
    Peer                                  peer;
    std::array<uint8_t, 68>               buffer{};
    size_t                                received = 0;
    std::chrono::steady_clock::time_point started{};
};

int build_listen_fd(uint16_t port);
std::vector<uint8_t> build_message(uint8_t id, const std::vector<uint8_t>& payload = {});
std::vector<uint8_t> build_request(const BlockRequest& req);

class PeerManager {
public:
    PeerManager(std::vector<PeerConnection> conns,
                const TorrentFile& torrent,
                std::string peer_id,
                uint16_t listen_port = 6881,
                size_t max_peers = 60);
    ~PeerManager();

    PeerManager(const PeerManager&)            = delete;
    PeerManager& operator=(const PeerManager&) = delete;

    std::vector<PeerEvent> poll_once(int timeout_ms);

    void send_interested_all();
    void send_interested(uint32_t peer_id);
    void send_to(uint32_t peer_id, const std::vector<uint8_t>& msg);
    void send_unchoke(uint32_t peer_id);
    void send_choke(uint32_t peer_id);
    void send_request(uint32_t peer_id, const BlockRequest& req);
    void send_bitfield(uint32_t peer_id, const Bitfield& our_have);
    void send_piece(uint32_t peer_id, uint32_t index, uint32_t begin,
                    const std::vector<uint8_t>& data);
    void broadcast_have(uint32_t index);

    static void queue(PeerConnection& c, const std::vector<uint8_t>& msg);

    std::unordered_map<uint32_t, PeerConnection>& connections() { return conns_; }

    const std::vector<uint16_t>& get_piece_frequency() const { return piece_frequency_; }

    // "empty" now means: nothing connected, nothing mid-handshake, and no way
    // for anything new to arrive. A seeder with an open listen socket is not
    // idle just because no one is talking to it right now.
    bool   empty()       const { return conns_.empty() && inbound_.empty() && listen_fd_ < 0; }
    size_t peer_count()  const { return conns_.size(); }
    bool   listening()   const { return listen_fd_ >= 0; }

private:
    enum class InboundResult { Keep, Drop, Promoted };

    std::unordered_map<uint32_t, PeerConnection> conns_;
    std::vector<PendingInbound> inbound_;

    const TorrentFile& torrent_;
    std::string        peer_id_;
    uint32_t           piece_count_;
    uint32_t           next_id_   = 0;
    int                listen_fd_ = -1;
    size_t             max_peers_;

    std::vector<uint16_t> piece_frequency_;

    void          accept_new();
    InboundResult advance_inbound(PendingInbound& p, std::vector<PeerEvent>& out);
    void          expire_inbound(std::chrono::seconds timeout);

    void handle_message(uint32_t peer_id, const std::vector<uint8_t>& msg,
                        std::vector<PeerEvent>& out);
    void apply_availability(const Bitfield& bf, int delta);
};