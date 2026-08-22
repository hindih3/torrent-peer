#include "session.hpp"

#include <chrono>
#include <iomanip>
#include <iostream>

namespace {
constexpr int  kPipelineDepth  = 8;                        // requests in flight per peer
constexpr auto kRequestTimeout = std::chrono::seconds(30); // before a block goes back in the pool

int64_t ms_since(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - t).count();
}
}

Session::Session(const TorrentFile& torrent, std::vector<PeerConnection> conns,
                 const std::filesystem::path& download_dir)
    : torrent_(torrent),
      disk_(torrent, download_dir),
      pieces_(torrent),
      peers_(std::move(conns), torrent.pieces.size()) {}

void Session::run() {
    peers_.send_interested_all();

    const auto started = std::chrono::steady_clock::now();
    auto last_report   = started;

    uint64_t bytes_since = 0;
    uint64_t bytes_total = 0;

    // Single writer for the status line, so the final render can't drift out of
    // sync with the periodic one.
    auto report = [&](double mib_per_s) {
        const double pct = pieces_.total()
            ? 100.0 * pieces_.completed() / pieces_.total()
            : 0.0;

        std::cerr << '\r'
                  << pieces_.completed() << '/' << pieces_.total()
                  << " pieces (" << std::fixed << std::setprecision(1) << pct << "%)  "
                  << peers_.peer_count() << " peers  "
                  << std::setprecision(2) << mib_per_s << " MiB/s"
                  << "\033[K" << std::flush;   // erase leftovers from a longer previous line
    };

    while (!pieces_.is_complete() && !peers_.empty()) {
        for (auto& ev : peers_.poll_once(1000)) {
            if (ev.type == PeerEvent::Piece) {
                bytes_since += ev.block.data.size();
                bytes_total += ev.block.data.size();
                if (auto done = pieces_.on_block(ev.block)) {
                    disk_.write_piece(*done);
                    peers_.broadcast_have(done->index);
                }
            }
        }

        pieces_.requeue_stale(kRequestTimeout);

        // Keep every unchoked peer's pipe full instead of one block per round
        // trip: at 50ms RTT a depth of 1 caps a peer at ~320 KiB/s no matter
        // how much bandwidth is available.
        for (auto& [id, c] : peers_.connections()) {
            if (c.peer_choking) continue;
            while (c.outstanding < kPipelineDepth) {
                auto req = pieces_.pick_block(c.has_pieces, peers_.get_piece_frequency());
                if (!req) break;
                peers_.send_request(id, *req);
            }
        }

        const int64_t elapsed = ms_since(last_report);
        if (elapsed >= 1000) {
            report(bytes_since / (elapsed / 1000.0) / (1024.0 * 1024.0));
            bytes_since = 0;
            last_report = std::chrono::steady_clock::now();
        }
    }

    // The loop exits the instant the last piece verifies, which is almost never
    // accurate. Render once more so the line left on screen is
    // the real end state. Close with the session average: an instantaneous rate
    // measured over the few stray milliseconds since the last report would be
    // meaningless
    const int64_t total_ms = ms_since(started);
    report(total_ms ? bytes_total / (total_ms / 1000.0) / (1024.0 * 1024.0) : 0.0);
    std::cerr << '\n';

    if (pieces_.is_complete()) {
        disk_.sync();
        std::cerr << "download complete in "
          << std::fixed << std::setprecision(1)
          << ms_since(started) / 1000.0 << " s\n";
    } else {
        std::cerr << "ran out of peers\n";
    }
}