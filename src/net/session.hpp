#pragma once
#include <atomic>
#include "download/piece_manager.hpp"
#include "download/peer_manager.hpp"
#include "download/disk_manager.hpp"

class Session {
public:
    Session(const TorrentFile& torrent, std::vector<PeerConnection> conns,
            const std::filesystem::path& download_dir,
            const std::string& peer_id, uint16_t listen_port = 6881);
    void run(const std::atomic<bool>& shutdown);

private:
    void greet(uint32_t peer_id);

    void dispatch(const PeerEvent &ev, uint64_t &down_since, uint64_t &up_since);
    void on_piece(const PeerEvent &ev, uint64_t &down_since);
    void on_request(const PeerEvent &ev, uint64_t &up_since);

    const TorrentFile& torrent_;
    DiskManager  disk_;
    PieceManager pieces_;
    PeerManager  peers_;
};