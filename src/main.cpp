#include <atomic>
#include <csignal>
#include <iostream>
#include <fstream>
#include <sstream>

#include "net/session.hpp"

static std::atomic<bool> g_shutdown{false};
extern "C" void handle_sigint(int) { g_shutdown.store(true); }

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: torrent-peer <file.torrent> [download-dir] "
                     "[--port N] [--peer host:port] [--no-tracker]\n";
        return 1;
    }

    std::filesystem::path out_dir  = "/home/hamza/CLionProjects/torrent-peer/downloads";
    uint16_t              port     = 51413;
    bool                  use_tracker = true;
    std::vector<Peer>     manual_peers;

    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--port" && i + 1 < argc) {
            port = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if (a == "--peer" && i + 1 < argc) {
            std::string hp = argv[++i];
            size_t colon = hp.rfind(':');
            if (colon == std::string::npos) { std::cerr << "--peer wants host:port\n"; return 1; }
            manual_peers.push_back({hp.substr(0, colon), hp.substr(colon + 1)});
        } else if (a == "--no-tracker") {
            use_tracker = false;
        } else if (!a.empty() && a[0] != '-') {
            out_dir = a;
        } else {
            std::cerr << "unknown option: " << a << "\n";
            return 1;
        }
    }

    try {
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

        std::vector<Peer> peers = manual_peers;
        if (use_tracker) {
            try {
                auto found = contact_trackers(torrent, peer_id, port);
                peers.insert(peers.end(), found.begin(), found.end());
            } catch (const std::exception& e) {
                std::cerr << "tracker: " << e.what() << "\n";
            }
        }

        auto sockets = tcp_connect_peers(peers);
        auto conns   = handshake_peers(sockets, torrent, peer_id);

        std::cerr << "saving to " << std::filesystem::absolute(out_dir) << "\n";
        Session session(torrent, std::move(conns), out_dir, peer_id, port);

        std::signal(SIGINT, handle_sigint);
        session.run(g_shutdown);

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "fatal: " << e.what() << "\n";
        return 1;
    }
}