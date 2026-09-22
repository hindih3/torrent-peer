#include <poll.h>
#include <fstream>
#include <iostream>
#include <sstream>
#include "download/peer_manager.hpp"

int main(int argc, char** argv) {
    g_log_level = LogLevel::Trace;
    std::ifstream f(argv[1], std::ios::binary);
    std::stringstream ss; ss << f.rdbuf();
    TorrentFile torrent = parse_torrent(ss.str());

    PeerManager peers({}, torrent, generate_peer_id(), 6881);
    peers.add_peers({
        {"127.0.0.1", "7001"},   // good handshake
        {"127.0.0.1", "7001"},   // duplicate in the same batch
        {"127.0.0.1", "7002"},   // wrong info hash
        {"127.0.0.1", "7003"},   // mirrors our peer id back
        {"127.0.0.1", "7004"},   // accepts, never replies
        {"127.0.0.1", "7005"},   // nothing listening
        {"boo-hoo-o", "7006"},   // bad address
    });
    peers.add_peers({{"127.0.0.1", "7001"}});

    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(std::stoi(argv[2]));
    while (std::chrono::steady_clock::now() < end) {
        auto pfds = peers.build_fds();
        poll(pfds.data(), pfds.size(), 1000);
        for (auto& ev : peers.handle_events(pfds))
            if (ev.type == PeerEvent::Joined)
                std::cout << "  >> Joined event, peer id " << ev.peer_id << "\n";
    }
    std::cout << "connected peers: " << peers.peer_count() << "\n";
}