#include <atomic>
#include <csignal>
#include <iostream>
#include <fstream>

#include "cli/args.hpp"
#include "runtime/session.hpp"

static std::atomic<bool> g_shutdown{false};
extern "C" void handle_sigint(int) { g_shutdown.store(true); }

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

        std::signal(SIGPIPE, SIG_IGN);
        std::signal(SIGINT, handle_sigint);

        std::cerr << "saving to " << std::filesystem::absolute(args.out_dir) << '\n';
        Session session(torrent, args.manual_peers, args.out_dir, args.port, args.use_tracker);
        session.run(g_shutdown);
    } catch (const std::exception& e) {
        std::cerr << "fatal: " << e.what() << '\n';
        return 1;
    }
}