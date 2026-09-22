#include "session.hpp"

#include <chrono>
#include <iomanip>
#include <iostream>
#include <ranges>
#include <span>

namespace {
constexpr int  kPipelineDepth  = 8;                        // requests in flight per peer
constexpr auto kRequestTimeout = std::chrono::seconds(15); // before a block goes back in the pool

int64_t ms_since(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - t).count();
}
}

Session::Session(const TorrentFile& torrent, std::vector<Peer> initial_peers,
                 const std::filesystem::path& download_dir,
                 const std::string& peer_id, uint16_t listen_port, bool use_trackers)
    : torrent_(torrent),
      disk_(torrent, download_dir),
      pieces_(torrent),
      peers_({}, torrent, peer_id, listen_port),
      trackers_(torrent, peer_id, listen_port),
      use_trackers_(use_trackers)
{
    peers_.add_peers(std::move(initial_peers));
}

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
    const auto started = std::chrono::steady_clock::now();
    auto last_report   = started;

    uint64_t down_since = 0;   // bytes downloaded since the last status line
    uint64_t up_since   = 0;   // bytes uploaded since the last status line

    while (!shutdown.load()) {
        // One pollfd list: peers first, trackers after.
        auto pfds = peers_.build_fds();
        const size_t tracker_base = pfds.size();
        if (use_trackers_) {
            auto tfds = trackers_.build_fds();
            pfds.insert(pfds.end(), tfds.begin(), tfds.end());
        }

        // Sleep until a socket has something, or the next tracker timer is due.
        auto wait = std::chrono::milliseconds(1000);
        if (use_trackers_) wait = std::min(wait, trackers_.until_next_action());
        if (poll(pfds.data(), pfds.size(), static_cast<int>(wait.count())) < 0) {
            if (errno == EINTR) continue;
            throw_errno("poll");
        }

        std::span<pollfd> all(pfds);
        for (auto& ev : peers_.handle_events(all.first(tracker_base)))
            dispatch(ev, down_since, up_since);
        if (use_trackers_) {
            auto fresh = trackers_.tick(all.subspan(tracker_base));
            if (!fresh.empty()) peers_.add_peers(std::move(fresh));
        }

        pieces_.requeue_stale(kRequestTimeout);

        if (pieces_.is_complete() && !completed_) {
            completed_ = true;
            disk_.sync();
            log(LogLevel::Info, "download complete in {:.1f} s | seeding",
                ms_since(started) / 1000.0);
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

    disk_.sync();
    log(LogLevel::Info, "stopped after {:.1f} s ({}/{} pieces)",
        ms_since(started) / 1000.0, pieces_.completed(), pieces_.total());
}

void Session::dispatch(const PeerEvent& ev, uint64_t& down_since, uint64_t& up_since) {
    switch (ev.type) {
        case PeerEvent::Piece:   on_piece(ev, down_since); break;
        case PeerEvent::Joined:  greet(ev.peer_id);           break;
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
        log(LogLevel::Debug, "peer {} requested unavailable piece {}; ignoring",
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