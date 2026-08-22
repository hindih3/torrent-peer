#pragma once
#include "net/peer.hpp"
#include "common.hpp"
#include <vector>
#include <unordered_map>
#include <cstdint>

struct PeerEvent {
    enum Type { Unchoke, Piece, Dropped } type;
    uint32_t peer_id;
    Block    block;   // valid only when type == Piece
};

enum MessageId : uint8_t {
    MSG_CHOKE = 0, MSG_UNCHOKE = 1, MSG_INTERESTED = 2, MSG_NOT_INTERESTED = 3,
    MSG_HAVE = 4, MSG_BITFIELD = 5, MSG_REQUEST = 6, MSG_PIECE = 7, MSG_CANCEL = 8
};

std::vector<uint8_t> build_message(uint8_t id, const std::vector<uint8_t>& payload = {});
std::vector<uint8_t> build_request(const BlockRequest& req);

class PeerManager {
public:
    explicit PeerManager(std::vector<PeerConnection> conns, uint32_t piece_count);

    std::vector<PeerEvent> poll_once(int timeout_ms);

    void send_interested_all();
    void send_request(uint32_t peer_id, const BlockRequest& req);
    void broadcast_have(uint32_t index);

    static void queue(PeerConnection &c, std::vector<uint8_t> msg);

    std::unordered_map<uint32_t, PeerConnection>& connections() { return conns_; }

    const std::vector<uint16_t>& get_piece_frequency() const { return piece_frequency_; }
    bool empty() const { return conns_.empty(); }
    size_t peer_count() const { return conns_.size(); }

private:
    std::unordered_map<uint32_t, PeerConnection> conns_;
    uint32_t next_id_ = 0;

    std::vector<uint16_t> piece_frequency_;

    void handle_message(uint32_t peer_id, const std::vector<uint8_t>& msg,
                        std::vector<PeerEvent>& out);
    void apply_availability(const Bitfield& bf, int delta);
};