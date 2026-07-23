#include <iostream>
#include <fstream>
#include <sstream>
#include <ctime>
#include <unistd.h>

#include "peer.hpp"

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: torrent-peer <file.torrent>\n";
        return 1;
    }

    srand(time(nullptr));

    std::ifstream file(argv[1], std::ios::binary);
    if (!file) {
        std::cerr << "could not open: " << argv[1] << "\n";
        return 1;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();

    try {
        TorrentFile torrent = parse_torrent(buffer.str());
        print_torrent(torrent);

        std::string peer_id = generate_peer_id();

        std::vector<Peer> peers = contact_trackers(torrent, peer_id);
        std::cerr << peers.size() << " peers from tracker\n";

        std::vector<PeerSocket> sockets = tcp_connect_peers(peers);
        std::cerr << sockets.size() << " TCP connections established\n";

        std::vector<PeerConnection> connections =
            handshake_peers(sockets, torrent, peer_id);
        std::cerr << connections.size() << " peers handshaked\n";

        if (connections.empty()) {
            std::cerr << "no peers available\n";
            return 1;
        }

        for (const auto& c : connections)
            close(c.sockfd);

    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}