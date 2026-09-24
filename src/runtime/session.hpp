#pragma once
#include <atomic>
#include "managers/piece_manager.hpp"
#include "managers/peer_manager.hpp"
#include "managers/disk_manager.hpp"
#include "managers/tracker_manager.hpp"
#include "metainfo/torrent.hpp"

class Session {
public:
    Session(const TorrentFile& torrent, std::vector<Peer> initial_peers,
        const std::filesystem::path& download_dir,
        uint16_t listen_port, bool use_trackers);
    void run(const std::atomic<bool>& shutdown);

private:
    void greet(uint32_t peer_id);

    void dispatch(const PeerEvent &ev, uint64_t &down_since, uint64_t &up_since);
    void on_piece(const PeerEvent &ev, uint64_t &down_since);
    void on_request(const PeerEvent &ev, uint64_t &up_since);

    const TorrentFile& torrent_;
    const std::string peer_id_;

    DiskManager disk_;
    PieceManager pieces_;
    PeerManager peers_;
    TrackerManager trackers_;

    bool use_trackers_;
    bool completed_  = false;

    uint64_t downloaded_ = 0;
    uint64_t uploaded_   = 0;
    uint64_t verified_   = 0;
};