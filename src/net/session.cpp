#include "session.hpp"

Session::Session(const TorrentFile& torrent, std::vector<PeerConnection> conns,
                 const std::filesystem::path& dir)
    : torrent_(torrent),
      disk_(torrent, dir),
      pieces_(torrent),
      peers_(std::move(conns)) {}

void Session::run() {
    peers_.send_interested_all();

    auto last_report = std::chrono::steady_clock::now();
    uint64_t bytes_since = 0;

    while (!pieces_.is_complete() && !peers_.empty()) {
        for (auto& ev : peers_.poll_once(1000)) {
            if (ev.type == PeerEvent::Piece) {
                bytes_since += ev.block.data.size();
                if (auto done = pieces_.on_block(ev.block)) {
                    disk_.write_piece(*done);
                    peers_.broadcast_have(done->index);
                }
            }
        }

        pieces_.requeue_stale();

        for (auto& [id, c] : peers_.connections()) {
            if (c.peer_choking) continue;
            if (auto req = pieces_.pick_block(c.has_pieces))
                peers_.send_request(id, *req);
        }

        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           now - last_report).count();
        if (elapsed >= 1000) {
            double rate = bytes_since / (elapsed / 1000.0) / 1024.0 / 1024.0; // MiB/s
            double pct  = 100.0 * pieces_.completed() / pieces_.total();

            std::cerr << "\r"
                      << pieces_.completed() << "/" << pieces_.total()
                      << " pieces (" << std::fixed << std::setprecision(1) << pct << "%)  "
                      << peers_.peer_count() << " peers  "
                      << std::setprecision(2) << rate << " MiB/s   "
                      << std::flush;

            bytes_since = 0;
            last_report = now;
        }
    }

    std::cerr << "\n";

    if (pieces_.is_complete()) {
        disk_.sync();
        std::cerr << "download complete\n";
    } else {
        std::cerr << "ran out of peers\n";
    }
}