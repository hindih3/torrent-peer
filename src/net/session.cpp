#include "session.hpp"

#include <chrono>
#include <iomanip>
#include <iostream>
#include <ranges>
#include <thread>

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
    auto bf = pieces_.have_bitfield();
    log(LogLevel::Debug, "greet peer {} ({}/{} pieces)",
        id, pieces_.completed(), pieces_.total());

    peers_.send_bitfield(id, bf);
    peers_.send_unchoke(id);
    if (!pieces_.is_complete())
        peers_.send_interested(id);
}

void Session::run(const std::atomic<bool>& shutdown) {
    for (const auto &id: peers_.connections() | std::views::keys)
        greet(id);

    const auto started = std::chrono::steady_clock::now();
    auto last_report   = started;

    uint64_t down_since = 0;   // bytes downloaded since the last status line
    uint64_t up_since   = 0;   // bytes uploaded since the last status line
    while (!shutdown.load() && !peers_.empty()) {
        int timeout_ms = 1000;
        auto pfds = peers_.build_fds();
        if (pfds.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms));
        } else {
            int ready = poll(pfds.data(), pfds.size(), timeout_ms);
            if (ready < 0) {
                if (errno == EINTR) continue;
                throw_errno("poll");
            }
            // ready >= 0: process whatever came back
            for (auto& ev : peers_.handle_events(pfds)) {
                dispatch(ev, down_since, up_since);
            }
        }

        pieces_.requeue_stale(kRequestTimeout);

        if (pieces_.is_complete() && !announced_complete) {
            disk_.sync();
            log(LogLevel::Info, "download complete in {:.1f} | seeding",
                ms_since(started) / 1000.0);
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
        log(LogLevel::Info, "download complete in {:.1f} s", ms_since(started) / 1000.0);
    } else {
        log(LogLevel::Info, "ran out of peers");
    }
}

void Session::dispatch(const PeerEvent& ev, uint64_t& down_since, uint64_t& up_since) {
    switch (ev.type) {
        case PeerEvent::Piece:   on_piece(ev, down_since); break;
        case PeerEvent::Joined:  greet(ev.peer_id);        break;
        case PeerEvent::Request: on_request(ev, up_since); break;
        default: break;
    }
}

void Session::on_piece(const PeerEvent& ev, uint64_t& down_since) {
    down_since += ev.block.data.size();
    if (auto done = pieces_.on_block(ev.block)) {
        disk_.write_piece(*done);
        peers_.broadcast_have(done->index);
        log(LogLevel::Debug, "piece {} complete & verified ({}/{})",
            done->index, pieces_.completed(), pieces_.total());
    }
}

void Session::on_request(const PeerEvent& ev, uint64_t& up_since) {
    if (!pieces_.have_piece(ev.req.piece_index)) {
        log(LogLevel::Debug, "peer {} requested piece {} we don't have; ignoring",
            ev.peer_id, ev.req.piece_index);
        return;                       // <-- was `continue`
    }
    try {
        auto data = disk_.read_block(ev.req.piece_index, ev.req.offset, ev.req.length);
        peers_.send_piece(ev.peer_id, ev.req.piece_index, ev.req.offset, data);
        up_since += data.size();
    } catch (const std::exception& e) {
        log(LogLevel::Debug, "peer {} bad request piece {} off {} len {}: {}",
            ev.peer_id, ev.req.piece_index, ev.req.offset, ev.req.length, e.what());
    }
}