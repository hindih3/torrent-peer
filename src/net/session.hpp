#pragma once
#include "download/piece_manager.hpp"
#include "download/peer_manager.hpp"
#include "download/disk_manager.hpp"

class Session {
public:
    Session(const TorrentFile& torrent, std::vector<PeerConnection> conns,
            const std::filesystem::path& download_dir);
    void run();

private:
    const TorrentFile& torrent_;
    DiskManager  disk_;
    PieceManager pieces_;
    PeerManager  peers_;
};