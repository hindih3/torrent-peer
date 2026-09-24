#include <atomic>
#include <csignal>
#include <iostream>
#include <fstream>
#include <sstream>

#include "cli/args.hpp"
#include "runtime/session.hpp"

static std::atomic<bool> g_shutdown{false};
extern "C" void handle_sigint(int) { g_shutdown.store(true); }


// #FIXME find somewhere to put this function instead of pasting it in every main
static std::string generate_peer_id() {
    static constexpr char charset[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    std::mt19937 rng(std::random_device{}());
    std::string id = "-HB0010-";
    for (int i = 0; i < 12; ++i)
        id += charset[rng() % 62];
    return id;
}

int main(int argc, char** argv) {
    args args;
    try {
        args = parse_args(argc, argv);
    } catch (const std::invalid_argument& e) {
        std::cerr << e.what() << '\n' << kUsage;
        return 1;
    }

    try {
        TorrentFile torrent = parse_torrent(read_file(args.torrent_path));
        print_torrent(torrent, true);

        std::string peer_id = generate_peer_id();

        std::signal(SIGPIPE, SIG_IGN);
        std::signal(SIGINT, handle_sigint);

        std::cerr << "saving to " << std::filesystem::absolute(args.out_dir) << '\n';
        Session session(torrent, args.manual_peers, args.out_dir, peer_id, args.port, args.use_tracker);
        session.run(g_shutdown);
    } catch (const std::exception& e) {
        std::cerr << "fatal: " << e.what() << '\n';
        return 1;
    }
}