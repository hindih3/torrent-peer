#include "session.hpp"

void parse_messages(PeerConnection& conn) {
    while (true) {
        if (conn.read_buffer.size() < 4)
            return;

        uint32_t len;
        memcpy(&len, conn.read_buffer.data(), 4);
        len = ntohl(len);

        if (conn.read_buffer.size() < 4 + len)
            return;

        if (len == 0) {
            conn.read_buffer.erase(conn.read_buffer.begin(),
                                   conn.read_buffer.begin() + 4);
            std::cerr << "keep-alive from " << conn.peer.host << "\n";
            continue;
        }

        uint8_t  id          = conn.read_buffer[4];
        uint32_t payload_len  = len - 1;

        switch (id) {
            case 0: std::cerr << "choke from "          << conn.peer.host << "\n"; break;
            case 1: std::cerr << "unchoke from "        << conn.peer.host << "\n"; break;
            case 2: std::cerr << "interested from "     << conn.peer.host << "\n"; break;
            case 3: std::cerr << "not_interested from " << conn.peer.host << "\n"; break;
            case 4: std::cerr << "have from "           << conn.peer.host << "\n"; break;
            case 5: std::cerr << "bitfield from "       << conn.peer.host
                              << " (" << payload_len << " bytes)\n";               break;
            case 7: std::cerr << "piece from "          << conn.peer.host << "\n"; break;
            default: std::cerr << "msg id " << (int)id << " from "
                               << conn.peer.host << "\n";                          break;
        }

        conn.read_buffer.erase(conn.read_buffer.begin(),
                               conn.read_buffer.begin() + 4 + len);
    }
}