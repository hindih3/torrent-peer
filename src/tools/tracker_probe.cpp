// Drives TrackerManager on its own: no peers, no disk, no Session.
#include <poll.h>
#include <cerrno>
#include <chrono>
#include <fstream>
#include <iostream>
#include <sstream>

#include "bencode/torrent.hpp"
#include "bencode/utils.hpp"
#include "download/tracker_manager.hpp"

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: tracker_probe <file.torrent> [seconds]\n";
        return 1;
    }
    try {
        const auto run_for = std::chrono::seconds(argc > 2 ? std::stoi(argv[2]) : 60);
        g_log_level = LogLevel::Debug;

        std::ifstream file(argv[1], std::ios::binary);
        if (!file) throw std::runtime_error("cannot open torrent file");
        std::stringstream buffer;
        buffer << file.rdbuf();
        TorrentFile torrent = parse_torrent(buffer.str());

        // Deliberately broken trackers, to exercise both failure paths:
        //   127.0.0.1:9    nothing listening -> ICMP refused -> fail_backoff
        //   10.255.255.1   usually unroutable, silent -> deadline -> on_timeout
        torrent.announce_list.push_back({"udp://127.0.0.1:9/announce",
                                         "udp://10.255.255.1:6969/announce"});

        TrackerManager trackers(torrent, generate_peer_id(), 51413);

        const auto deadline = std::chrono::steady_clock::now() + run_for;
        size_t total = 0;
        while (std::chrono::steady_clock::now() < deadline) {
            auto pfds = trackers.build_fds();
            const auto wait = std::min(std::chrono::milliseconds(1000),
                                       trackers.until_next_action());
            if (poll(pfds.data(), pfds.size(), static_cast<int>(wait.count())) < 0
                && errno != EINTR)
                throw std::runtime_error("poll failed");

            for (const auto& p : trackers.tick(pfds)) {
                ++total;
                std::cout << "  peer " << p.host << ":" << p.port << "\n";
            }
        }
        std::cout << total << " peers received in " << run_for.count() << "s\n";
    } catch (const std::exception& e) {
        std::cerr << "fatal: " << e.what() << "\n";
        return 1;
    }
}
