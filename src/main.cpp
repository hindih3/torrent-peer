#include <iostream>
#include <fstream>
#include <sstream>
#include "torrent.hpp"
#include "tracker.hpp"

int main(int argc, char* argv[]) {
    if (argc < 2)
        throw std::runtime_error("Usage: bencode_parser <path>");

    std::ifstream file(argv[1], std::ios::binary);
    if (!file)
        throw std::runtime_error("Could not open file");

    std::ostringstream ss;
    ss << file.rdbuf();
    std::string data = ss.str();

    TorrentFile torrent = parse_torrent(data);
    print_torrent(torrent);

    std::cout << "\nFetching peers...\n";
    std::vector<Peer> peers = get_peers(torrent);

    std::cout << "Got " << peers.size() << " peers:\n";
    for (const auto& peer : peers)
        std::cout << peer.ip << ":" << peer.port << "\n";

    return 0;
}
