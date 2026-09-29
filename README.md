# torrent-peer
![CI](https://github.com/hindih3/torrent-peer/actions/workflows/ci.yml/badge.svg)

**torrent-peer** is a BitTorrent client written from scratch in C++20, with
no dependencies beyond OpenSSL for SHA-1. The bencode parser, UDP tracker
protocol, peer wire protocol, piece verification, and multi-file disk I/O
are all implemented by hand. Everything runs on a single-threaded,
non-blocking `poll` loop, where one session coordinates four narrowly scoped
managers for trackers, peers, pieces, and disk.

## Building

Requires CMake 3.16+, GCC 13+ or Clang 17+ (for `std::format`), and the
OpenSSL development headers.

```sh
# Debian/Ubuntu
sudo apt install cmake g++ libssl-dev

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

This builds the client at `build/torrent-peer`, plus two diagnostic tools,
`tracker-probe` and `peer-probe`. Without `CMAKE_BUILD_TYPE`, the build
defaults to Debug.

Developed on Ubuntu 24.04; also works under WSL2.

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

`trace` prints every wire message in both
directions and is very high volume, so redirect it to a file:

```sh
./build/torrent-peer file.torrent --log-level trace 2> trace.log
```

## Testing

Tests run under AddressSanitizer, UndefinedBehaviorSanitizer, and libstdc++'s
bounds checks:

```sh
cmake -B build-debug -DTP_SANITIZE=ON -DTP_ASSERTIONS=ON
cmake --build build-debug
ctest --test-dir build-debug --output-on-failure
```

The bencode parser also has a libFuzzer harness (requires clang). The corpus in
`tests/corpus/parser/` holds the inputs found so far:

```sh
CXX=clang++ cmake -B build-fuzz -DTP_FUZZ=ON
cmake --build build-fuzz --target fuzz-parser
./build-fuzz/fuzz-parser -max_total_time=300 tests/corpus/parser/
```

## How it works

```
     .torrent → parser → TorrentFile
                              │
                              ▼
                      ┌───────────────┐
                      │    Session    │  owns the poll() loop
                      └───────┬───────┘
        ┌─┬─────────────┬─┬───┴─────────┬─┬────────────┬─┐
        ▼ ▲             ▼ ▲             ▼ ▲            ▼ ▲
  TrackerManager    PeerManager     PieceManager   DiskManager
   UDP trackers      TCP peers      block choice    file I/O
```

## Highlights

- **One event loop with four managers.** A single-threaded `poll()` loop drives
  every socket. Session coordinates four managers (trackers, peers, pieces,
  disk) that never call each other, each with a small interface that takes
  plain inputs and returns results. Networking is I/O-bound, so one thread
  waiting on all sockets keeps up with the swarm without any locking.

- **Download strategy.** Rarest-first piece selection, based on per-piece
  availability across connected peers, with 32 block requests pipelined per
  peer so throughput isn't capped by round-trip time.

- **Multi-file disk layout.** The torrent is one contiguous byte range mapped
  onto its files. Pieces that span file boundaries are split with a binary
  search and written with `pwrite`, in any order, into space preallocated at
  startup.

- **Tracker state machine.** Each UDP tracker (BEP 15) runs its own
  connect/announce cycle with exponential backoff, connection-ID expiry, and
  periodic re-announce, all non-blocking inside the same loop.

- **Treats all network input as hostile.** Path traversal in torrent files,
  malformed bitfields, and oversized messages are rejected before they can do
  harm. The info hash is computed over the original bytes, not a
  re-encoding.

- **Strict, fuzzed bencode parser.** Rejects non-canonical integers, unsorted
  or duplicate dictionary keys, out-of-bounds string lengths, and nesting
  deeper than 100 levels. Fuzzed with libFuzzer under ASan and UBSan.

- **RAII resource ownership.** Every socket and file descriptor is owned by a
  move-only wrapper, so nothing can leak or be closed twice, and there are no
  manual `close()` calls outside it.

## Limitations

**Not supported**

- HTTP/HTTPS trackers and web seeds; only UDP trackers (BEP 15)
- Magnet links (BEP 9); a `.torrent` file is required
- DHT, PEX, the fast extension (BEP 6), and the extension protocol (BEP 10)
- Protocol encryption (MSE/PE)
- IPv6
- One torrent per process
- Choking algorithm: every interested peer is unchoked
- Endgame mode, so the last few pieces can be slow
- `cancel` messages are ignored
- Resume: restarting re-downloads from scratch
- Bandwidth limits
- POSIX only (`poll`, `pwrite`, `posix_fallocate`); works under WSL

**Known issues**

- In-flight requests aren't released on choke or disconnect; they return
  to the pool only after a timeout
- Progress isn't reported to trackers (no `completed`/`stopped` events,
  `left` is never updated)
- No keep-alives are sent and idle peers are never timed out
- A peer's request queue and write buffer are unbounded
- DNS resolution blocks the event loop

## References

- [BEP 3 : The BitTorrent Protocol Specification](https://www.bittorrent.org/beps/bep_0003.html)
- [BEP 15 : UDP Tracker Protocol](https://www.bittorrent.org/beps/bep_0015.html)
- [BEP 12 : Multitracker Metadata Extension](https://www.bittorrent.org/beps/bep_0012.html)