#include <csignal>
#include <iostream>
#include <fstream>
#include <sstream>
#include <ctime>
#include <unistd.h>
#include <sys/poll.h>

#include "download/disk_manager.hpp"
#include "net/session.hpp"

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: torrent-peer <file.torrent>\n";
        return 1;
    }
    const std::filesystem::path out_dir = (argc >= 3) ? argv[2] : std::filesystem::current_path();

    std::ifstream file(argv[1], std::ios::binary);
    if (!file) {
        std::cerr << "could not open: " << argv[1] << "\n";
        return 1;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();

    TorrentFile torrent = parse_torrent(buffer.str());
    print_torrent(torrent, true);

    std::string peer_id = generate_peer_id();
    signal(SIGPIPE, SIG_IGN);

    auto peers    = contact_trackers(torrent, peer_id);
    auto sockets  = tcp_connect_peers(peers);
    auto conns    = handshake_peers(sockets, torrent, peer_id);

    if (conns.empty()) { std::cerr << "no peers\n"; return 1; }

    Session session(torrent, std::move(conns), out_dir);
    std::cerr << "saving to " << std::filesystem::absolute(out_dir) << "\n";
    session.run();
    return 0;
}