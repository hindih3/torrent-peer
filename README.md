# torrent-peer

A BitTorrent client written from scratch in C++17. No external torrent libraries; the
bencode parser, UDP tracker protocol, peer wire protocol, piece assembly, and
multi-file disk layout are all implemented manually.

OpenSSL is used for SHA-1 and nothing else.

Below is an example of the client working start-to-end on an actual torrent:
```
Name: Big Buck Bunny
Announce: udp://tracker.leechers-paradise.org:6969
Piece length: 262144
Number of pieces: 1055
Length: (none)
Files:
  - path: Big Buck Bunny.en.srt/ length: 140
  - path: Big Buck Bunny.mp4/ length: 276134947
  - path: poster.jpg/ length: 310380
Comment: WebTorrent <https://webtorrent.io>
Created by: WebTorrent <https://webtorrent.io>
Creation date: 1490916601
Encoding: UTF-8
Announce list:
  tier: udp://tracker.leechers-paradise.org:6969 
  tier: udp://tracker.coppersurfer.tk:6969 
  tier: udp://tracker.opentrackr.org:1337 
  tier: udp://explodie.org:6969 
  tier: udp://tracker.empire-js.us:1337 
  tier: wss://tracker.btorrent.xyz 
  tier: wss://tracker.openwebtorrent.com 
  tier: wss://tracker.fastcast.nz 
URL list:
  https://webtorrent.io/torrents/
Info hash: dd8255ecdc7ca55fb0bbf81323d87062db1f6d1c
Total length: 276445467
connected: udp://explodie.org:6969
1 tracker(s) responded to connect
udp://explodie.org:6969: 50 peers
TCP connected: 24.171.8.210:491XX
handshake ok: 24.171.8.210:491XX
...
saving to "CLionProjects/torrent-peer/downloads"
1055/1055 pieces, 16 peers, 3.94 down / 0.00 up MiB/s
download complete in 50.5 s — seeding
```

## Status

Downloads and seeding both work. The flow kind of looks like:
parse and extract torrent metadata → connect to trackers → extract peers → peer TCP connect/handshake
→ send pipelined block requests → SHA-1 verification → disk. Once every piece is
verified the client keeps running as a seed, accepting inbound peers and serving
`request` messages, until interrupted with Ctrl+C.

## Building

Requires CMake 3.16+, a C++20 compiler, and OpenSSL development headers. The code can compile with a C++17
compiler with the removal/replacement of some secondary features like designated initializers 
and `std::format`.

```sh
# Debian/Ubuntu
sudo apt install cmake g++ libssl-dev

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

The binary lands at `build/torrent-peer`.

Entire project was developed natively on Ubuntu 24.04.4 LTS. It was also tested on a Windows WSL
environment and passed testing normally.

## Usage

```sh
./build/torrent-peer <file.torrent> [download-dir] [options]
```

`download-dir` defaults to `./downloads`.

| Option | Description |
| --- | --- |
| `--port N` | Port to listen on for inbound peers (also advertised to trackers). Default 51413. |
| `--peer host:port` | Add a peer manually. May be repeated. Combines with tracker-discovered peers. |
| `--no-tracker` | Skip tracker contact and use only `--peer` addresses. |
| `--log-level LEVEL` | `trace`, `debug`, `info` (default), `warn`, `error`, or `off`. |

The log level can also be set with the `TP_LOG` environment variable; an explicit
`--log-level` flag overrides it. `trace` prints every wire message in both
directions and is very high volume; heavily recommend redirecting it to a file:

```sh
./build/torrent-peer file.torrent --log-level trace 2> trace.log
```

Seeding outside my LAN barely got me any peers until I set up port forwarding on my router.
I went from 2 connections to 12 with this fix. Try this fix if your upload rate feels lackluster.

## How it works

```
.torrent file
     │
     ▼
┌─────────────┐   bencode/     Recursive-descent parser. Records the byte range
│   parser    │   parser.cpp   of the info dict during the parse so the info
└─────────────┘                hash can be taken over the exact original bytes.
     │
     ▼
┌─────────────┐   bencode/     Typed view of the torrent: piece hashes, piece
│ TorrentFile │   torrent.cpp  length, file layout, announce tiers, info hash.
└─────────────┘
     │
     ▼
┌─────────────┐   net/         UDP tracker protocol (BEP 15). Walks announce
│   tracker   │   tracker.cpp  tiers in order; within a tier, connects and
└─────────────┘                announces to every tracker in parallel against a
     │                         shared deadline and merges the peer sets.
     ▼
┌─────────────┐   net/         Non-blocking connect to all peers at once, then
│  peer conn  │   peer.cpp     the 68-byte handshake. Peers that fail either
└─────────────┘                step are dropped.
     │
     ▼
┌─────────────┐   net/         Event loop. Polls peers, hands arriving blocks to
│   Session   │   session.cpp  the piece manager, writes verified pieces to disk,
└─────────────┘                serves requests from peers, and refills each
     │                         peer's request pipeline.
     ├──────────────┬──────────────────┐
     ▼              ▼                  ▼
┌───────────┐ ┌────────────┐  ┌──────────────┐
│PeerManager│ │PieceManager│  │ DiskManager  │
└───────────┘ └────────────┘  └──────────────┘
 socket I/O,   block choice,   file layout,
 message       assembly,       slice-based
 framing,      SHA-1 verify    reads & writes
 inbound
 accept

```

### Design notes

**Info hash over original bytes.** The parser records where the info dict
starts and ends in the input so the SHA-1 is computed over the exact bytes
received rather than a re-encoding.

**Inbound connections.** The client both dials peers and accepts inbound
connections on its listen port. An inbound peer speaks the handshake first
(the mirror of the outbound path).

**Request pipelining.** Each unchoked peer is kept at 8 outstanding block
requests. With one request in flight per round trip, a peer at 50 ms RTT caps
out around 320 KiB/s regardless of available bandwidth.

**Slice-based disk I/O.** The torrent is treated as one contiguous byte
range mapped onto a list of files. A piece that sits between a file boundary is
split at the boundary and written with `pwrite` to each file at the right
offset, so pieces can arrive and be written in any order. Serving a `request`
reads back through the same mapping.

**Space is reserved up front.** preallocate with posix_fallocate; on filesystems
that don't support it, fall back to ftruncate and accept a sparse file. `ftruncate`
produces a sparse file, which means a full disk surfaces as a write failure hours into
a download instead of immediately.

**Untrusted input is flagged.** File paths from the torrent are
rejected if they contain `..` or path separators to stop path traversal attacks (so
a malicious torrent can't write outside the download directory). Peer bitfields
are rejected if the length is wrong or spare bits are set, and message lengths
above 1 MiB are refused before any allocation.

**Stalled requests are recycled.** Blocks requested but not delivered within
30 seconds return to the pool so one slow peer can't hold a piece hostage.

**Leveled logging.** A small `std::format`-based logger gates output by level
(`trace`..`error`). Connection lifecycle is logged at `debug`; every wire
message in and out is logged at `trace`, with `->`/`<-` marking direction.
Default `info` keeps normal runs quiet.

## Roadmap

- [x] **Rarest-first piece selection.** Uses a frequency array to decide which
  piece to request next from each peer. Benefits swarm health.
- [x] **Seeding.** Serves `request` messages and continues seeding after the
  download completes. (`cancel` is still ignored — a served block that the peer
  no longer wants is sent anyway.)
- [ ] **Choking algorithm.** No tit-for-tat; every peer is unchoked and none
  are choked. A real choke algorithm would attract more upload demand in busy
  swarms.
- [ ] **Tracker re-announce.** The client announces once at startup and never
  refreshes, so it does not discover peers that join later and is not kept in
  the tracker's seed list.
- [ ] **Sequential piece selection.** Optional lowest-index-first mode for
  streaming media while it downloads. Off by default: it trades swarm efficiency
  for in-order availability, so it would be an explicit `--sequential` opt-in
  rather than the default rarest-first.
- [ ] **HTTP tracker support.** Only UDP trackers (BEP 15) are supported;
  `http://` and `https://` announce URLs are ignored.
- [ ] **IPv6.** Peer connections and tracker communication are IPv4 only.

## Limitations

One torrent per process. IPv4 only. No encryption, no PEX, no
fast-extension support. No `cancel` handling. Tracker announce is one-shot
(no re-announce), so long seeding sessions stop attracting new peers.

## References

- [BEP 3 : The BitTorrent Protocol Specification](https://www.bittorrent.org/beps/bep_0003.html)
- [BEP 15 : UDP Tracker Protocol](https://www.bittorrent.org/beps/bep_0015.html)
- [BEP 12 : Multitracker Metadata Extension](https://www.bittorrent.org/beps/bep_0012.html)