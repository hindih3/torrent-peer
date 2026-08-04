#include <iostream>
#include <fstream>
#include <sstream>
#include <ctime>
#include <unistd.h>
#include <sys/poll.h>

#include "session.hpp"

static void run_message_loop(std::vector<PeerConnection>& conns) {
    while (!conns.empty()) {
        std::vector<pollfd> pfds;
        for (const auto& c : conns)
            pfds.push_back({ c.sockfd, POLLIN, 0 });

        int ready = poll(pfds.data(), pfds.size(), 5000);
        if (ready == -1)
            throw std::runtime_error(std::string("poll: ") + strerror(errno));
        if (ready == 0) {
            std::cerr << "5s idle, stopping\n";
            break;                              // nobody said anything for 5s
        }

        std::vector<size_t> dead;
        for (size_t i = 0; i < conns.size(); i++) {
            if (!(pfds[i].revents & POLLIN))
                continue;

            uint8_t tmp[4096];
            ssize_t n = recv(conns[i].sockfd, tmp, sizeof(tmp), 0);
            if (n <= 0) {                       // 0 = peer closed, -1 = error
                std::cerr << "dead: " << conns[i].peer.host << "\n";
                close(conns[i].sockfd);
                dead.push_back(i);
                continue;
            }

            conns[i].read_buffer.insert(conns[i].read_buffer.end(), tmp, tmp + n);
            parse_messages(conns[i]);
        }

        // remove dead connections, back-to-front to keep indices valid
        for (auto it = dead.rbegin(); it != dead.rend(); ++it)
            conns.erase(conns.begin() + *it);
    }
}

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

        run_message_loop(connections);

        for (const auto& c : connections)
            close(c.sockfd);

    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}