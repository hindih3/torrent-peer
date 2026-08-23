#pragma once
#include "net/peer.hpp"
#include "common.hpp"
#include <vector>
#include <unordered_map>
#include <cstdint>

#define LISTEN_MARKER UINT32_MAX

struct PeerEvent {
    enum Type { Unchoke, Piece, Dropped, Request } type;
    uint32_t     peer_id;
    Block        block;    // valid when type == Piece
    BlockRequest req;      // valid when type == Request
};

enum MessageId : uint8_t {
    MSG_CHOKE = 0, MSG_UNCHOKE = 1, MSG_INTERESTED = 2, MSG_NOT_INTERESTED = 3,
    MSG_HAVE = 4, MSG_BITFIELD = 5, MSG_REQUEST = 6, MSG_PIECE = 7, MSG_CANCEL = 8
};

int build_listen_fd();
std::vector<uint8_t> build_message(uint8_t id, const std::vector<uint8_t>& payload = {});
std::vector<uint8_t> build_request(const BlockRequest& req);

class PeerManager {
public:
    explicit PeerManager(std::vector<PeerConnection> conns, uint32_t piece_count);

    std::vector<PeerEvent> poll_once(int timeout_ms);

    void send_interested_all();
    void send_to(uint32_t peer_id, const std::vector<uint8_t>& msg);
    void send_unchoke(uint32_t peer_id);
    void send_request(uint32_t peer_id, const BlockRequest& req);
    void send_bitfield(uint32_t peer_id, const Bitfield& our_have);

    void send_piece(uint32_t peer_id, uint32_t index, uint32_t begin, const std::vector<uint8_t> &data);

    void broadcast_have(uint32_t index);

    static void queue(PeerConnection &c, std::vector<uint8_t> msg);

    std::unordered_map<uint32_t, PeerConnection>& connections() { return conns_; }

    const std::vector<uint16_t>& get_piece_frequency() const { return piece_frequency_; }
    bool empty() const { return conns_.empty(); }
    size_t peer_count() const { return conns_.size(); }

private:
    std::unordered_map<uint32_t, PeerConnection> conns_;
    uint32_t next_id_ = 0;
    int listen_fd_;

    std::vector<uint16_t> piece_frequency_;

    void handle_message(uint32_t peer_id, const std::vector<uint8_t>& msg,
                        std::vector<PeerEvent>& out);
    void apply_availability(const Bitfield& bf, int delta);
};