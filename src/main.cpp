#include <iostream>
#include <fstream>
#include <sstream>
#include "peer.hpp"

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <torrent-file>\n";
        return 1;
    }

    std::ifstream file(argv[1], std::ios::binary);

    if (!file.is_open())
        throw std::runtime_error("failed to open file: " + std::string(argv[1]));

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string data = buffer.str();

    TorrentFile torrent = parse_torrent(data);
    print_torrent(torrent);
    std::vector<Peer> peers = contact_trackers(torrent);

    auto connected_peers = connect_to_peers(peers, torrent, generate_peer_id());
}