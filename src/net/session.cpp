#include "session.hpp"

#include <chrono>
#include <iomanip>
#include <iostream>

namespace {
bool announced_complete = false;
constexpr int  kPipelineDepth  = 8;                        // requests in flight per peer
constexpr auto kRequestTimeout = std::chrono::seconds(30); // before a block goes back in the pool

int64_t ms_since(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - t).count();
}
}

Session::Session(const TorrentFile& torrent, std::vector<PeerConnection> conns,
                 const std::filesystem::path& download_dir,
                 const std::string& peer_id, uint16_t listen_port)
    : torrent_(torrent),
      disk_(torrent, download_dir),
      pieces_(torrent),
      peers_(std::move(conns), torrent, peer_id, listen_port) {}

// Everything a peer needs on arrival, whether we dialed them or they dialed us.
void Session::greet(uint32_t id) {
    peers_.send_bitfield(id, pieces_.have_bitfield());
    peers_.send_unchoke(id);
    if (!pieces_.is_complete())
        peers_.send_interested(id);
}

void Session::run(const std::atomic<bool>& shutdown) {
    for (auto& [id, c] : peers_.connections())
        greet(id);

    const auto started = std::chrono::steady_clock::now();
    auto last_report   = started;

    uint64_t down_since = 0;   // bytes downloaded since the last status line
    uint64_t up_since   = 0;   // bytes uploaded since the last status line

    while (!shutdown.load() && !peers_.empty()) {
        for (auto& ev : peers_.poll_once(1000)) {
            if (ev.type == PeerEvent::Piece) {
                down_since += ev.block.data.size();
                if (auto done = pieces_.on_block(ev.block)) {
                    disk_.write_piece(*done);
                    peers_.broadcast_have(done->index);
                }
            }

            else if (ev.type == PeerEvent::Joined) {
                greet(ev.peer_id);
            }

            else if (ev.type == PeerEvent::Request) {
                std::cerr << "REQUEST from " << ev.peer_id << " for piece "
                    << ev.req.piece_index << "\n";
                if (!pieces_.have_piece(ev.req.piece_index)) continue;
                try {
                    auto data = disk_.read_block(ev.req.piece_index, ev.req.offset,
                                                 ev.req.length);
                    peers_.send_piece(ev.peer_id, ev.req.piece_index, ev.req.offset, data);
                    up_since += data.size();
                } catch (const std::exception&) {
                    // malformed request (bad offset/length) — ignore, don't crash
                }
            }
        }

        pieces_.requeue_stale(kRequestTimeout);

        if (pieces_.is_complete() && !announced_complete) {
            disk_.sync();
            std::cerr << "download complete in "
                      << std::fixed << std::setprecision(1)
                      << ms_since(started) / 1000.0 << " s — seeding\n";
            announced_complete = true;
        }

        // Keep every unchoked peer's pipe full instead of one block per round
        // trip: at 50ms RTT a depth of 1 caps a peer at ~320 KiB/s no matter
        // how much bandwidth is available.
        const auto& availability = peers_.get_piece_frequency();
        for (auto& [id, c] : peers_.connections()) {
            if (c.peer_choking) continue;
            while (c.outstanding < kPipelineDepth) {
                auto req = pieces_.pick_block(c.has_pieces, availability);
                if (!req) break;
                peers_.send_request(id, *req);
            }
        }

        const int64_t elapsed = ms_since(last_report);
        if (elapsed >= 1000) {
            const double secs = elapsed / 1000.0;
            const double down = down_since / secs / (1024.0 * 1024.0);
            const double up   = up_since   / secs / (1024.0 * 1024.0);

            std::cerr << pieces_.completed() << "/" << pieces_.total() << " pieces, "
                      << peers_.peer_count() << " peers, "
                      << std::fixed << std::setprecision(2)
                      << down << " down / " << up << " up MiB/s\n";

            down_since = 0;
            up_since   = 0;
            last_report = std::chrono::steady_clock::now();
        }
    }

    if (pieces_.is_complete()) {
        disk_.sync();
        std::cerr << "download complete in "
                  << std::fixed << std::setprecision(1)
                  << ms_since(started) / 1000.0 << " s\n";
    } else {
        std::cerr << "ran out of peers\n";
    }
}