#include "tracker_manager.hpp"

#include <fcntl.h>
#include <netdb.h>
#include <utility>
#include <vector>
#include <bits/fcntl-linux.h>

#include "common.hpp"

TrackerManager::TrackerManager(const TorrentFile& torrent,
                               std::string  peer_id, uint16_t listen_port)
                   : torrent_(torrent), peer_id_(std::move(peer_id)),
                     listen_port_(listen_port), rng_(std::random_device{}())
{
    for (auto& tier : torrent.announce_list) {
        for (auto& url : tier) {
            try {
                TrackerSession tracker;
                tracker.address = parse_tracker_url(url);
                trackers_.push_back(std::move(tracker));
            } catch (const std::exception& e) {
                log(LogLevel::Debug, "skipping tracker: {}", e.what());
            }
        }
    }
}

std::vector<uint8_t> TrackerManager::build_connect_request(const uint32_t transaction_id) {
    std::vector<uint8_t> packet(16);

    uint64_t protocol_id = htobe64(BITTORRENT_PROTOCOL);
    uint32_t action      = htonl(0);
    uint32_t txn_id      = htonl(transaction_id);

    memcpy(packet.data(),      &protocol_id, 8);
    memcpy(packet.data() + 8,  &action,      4);
    memcpy(packet.data() + 12, &txn_id,      4);

    return packet;
}

std::vector<uint8_t> TrackerManager::build_announce_request(
    const TrackerSession& t, TrackerEvent e, const std::string& peer_id,
    const std::array<uint8_t,20>& info_hash, const uint16_t port, const AnnounceParams& p) {

    if (peer_id.size() != 20)
        throw std::runtime_error("peer_id must be exactly 20 bytes");

    std::vector<uint8_t> packet(98);
    uint64_t conn_id_be = htobe64(t.connection_id);
    uint32_t action_be  = htonl(1);
    uint32_t txn_be     = htonl(t.transaction_id);
    uint64_t down_be    = htobe64(p.downloaded);
    uint64_t left_be    = htobe64(p.left);
    uint64_t up_be      = htobe64(p.uploaded);
    uint32_t event_be   = htonl(e);
    uint32_t ip         = 0;           // default IP
    uint32_t key        = htonl(t.key);
    int32_t  num_want   = htonl(-1);   // request default num of peers
    uint16_t port_be    = htons(port);

    memcpy(packet.data(),      &conn_id_be, 8);
    memcpy(packet.data() + 8,  &action_be,  4);
    memcpy(packet.data() + 12, &txn_be,     4);
    memcpy(packet.data() + 16, info_hash.data(), 20);
    memcpy(packet.data() + 36, peer_id.data(),   20);
    memcpy(packet.data() + 56, &down_be,    8);
    memcpy(packet.data() + 64, &left_be,    8);
    memcpy(packet.data() + 72, &up_be,      8);
    memcpy(packet.data() + 80, &event_be,   4);
    memcpy(packet.data() + 84, &ip,         4);
    memcpy(packet.data() + 88, &key,        4);
    memcpy(packet.data() + 92, &num_want,   4);
    memcpy(packet.data() + 96, &port_be,    2);
    return packet;
}

uint32_t TrackerManager::next_random() {
    std::uniform_int_distribution<uint32_t> dist;
    return dist(rng_);
}

void TrackerManager::open_socket(TrackerSession& t) {
    addrinfo hints{}, *res;
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    if (getaddrinfo(t.address.host.c_str(), t.address.port.c_str(),
                    &hints, &res) != 0)
        throw std::runtime_error("DNS resolution failed: " + t.address.host);

    int fd = createUDPIpv4Socket();
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);

    if (connect(fd, res->ai_addr, res->ai_addrlen) == -1) {
        freeaddrinfo(res);
        close(fd);
        throw std::runtime_error("connect failed: " + t.address.host);
    }
    freeaddrinfo(res);
    t.sockfd = fd;
}

void TrackerManager::fail_backoff(TrackerSession& t) {
    if (t.sockfd >= 0) { close(t.sockfd); t.sockfd = -1; }
    t.state = TrackerState::Disconnected;
    t.retries = std::min(t.retries + 1, 8);
    auto delay = std::chrono::seconds(15 * (1 << t.retries)); // 15·2^n
    t.next_action = std::chrono::steady_clock::now() + delay;
}

void TrackerManager::send_connect(TrackerSession& t) {
    try {
        open_socket(t);
    } catch (const std::exception& e) {
        log(LogLevel::Debug, "connect failed for {}: {}", t.address.host, e.what());
        fail_backoff(t);
        return;
    }

    t.transaction_id = next_random();
    if (t.key == 0) t.key = next_random();
    auto packet = build_connect_request(t.transaction_id);
    if (::send(t.sockfd, packet.data(), packet.size(), 0) < 0) {
        fail_backoff(t);
        return;
    }
    t.state = TrackerState::Connecting;
    t.connected_at = std::chrono::steady_clock::now();
}
