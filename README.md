# torrent-peer
![CI](https://github.com/hindih3/torrent-peer/actions/workflows/ci.yml/badge.svg)

**torrent-peer** is a BitTorrent client written in C++20,
with no dependencies beyond OpenSSL for SHA-1. Works end-to-end, 
downloading and seeding on live swarms.

![Big Buck Bunny download](logs/gifs/demo.gif)

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
| `--port N` | Port to listen on for inbound peers (1024–65535, also advertised to trackers). Default 51413. |
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

## Development

Naming conventions are enforced with clang-tidy's `readability-identifier-naming`
check, configured in `.clang-tidy`. CI runs it with warnings treated as errors.

```sh
cmake -B build
run-clang-tidy -p build -header-filter='src/.*'
```

Add `-fix` to rename offending identifiers in place. Names fixed by outside
tools, such as `LLVMFuzzerTestOneInput`, are excluded in `.clang-tidy`.

## Highlights

* **One Session, many managers.** The bulk of the program is accessed through
  session, an orchestrator class that controls four narrowly scoped managers:
  - `TrackerManager` UDP tracker communication
  - `PeerManager`    peer connections / wire protocol
  - `PieceManager`   piece selection and verification
  - `BanManager`     bad-data attribution and bans
  - `DiskManager`    file layout and disk I/O


* **Managers never call each other.** Session is the only code that touches more
  than one, so every interaction between the managers is written in about 250 lines.
  Take an arriving block: Session's `on_piece()` passes it to `pieces_.on_block()`, which
  returns a verified piece if that block completed one. Session hands the piece
  to `disk_.write_piece()`, then calls `peers_.broadcast_have()`.


* **Single-threaded.** All sockets and timers are handled in one `poll()` loop
  on a single thread. Multithreading doesn't make much sense in a client like
  this because most of its operations aren't CPU intensive. To demonstrate,
  downloading Sintel (about 129 MB) used 2.1s of CPU over a 35s
  download, about 6% of one core, with 18 MB peak memory. One thread means no
  locking, synchronization and other overheads that come with multithreading.
  The cost is that a blocking call like a DNS lookup or slow disk write
  holds the entire client until finished.


* **Download algorithm.** Piece rarity is calculated by summing the bitfields of
  connected peers into a frequency array. Unchoked peers receive up to 32
  pipelined block requests each. Pipelining shifts performance from latency-bound
  to bandwidth-bound, reducing a file’s download time from 154s with one request 
  to 34s with 32 (~4.5× faster).


* **Tracker state machine.** Each UDP tracker (BEP 15) runs its own
  connect/announce cycle with exponential backoff, connection-ID expiry, and
  periodic re-announce, all non-blocking inside the same loop. Announces
  carry live progress (`downloaded`, `uploaded`, `left`), and `completed` is
  sent as soon as the download finishes, but never when resuming an
  already-complete download. There is also a shutdown period where the client
  attempts to send `stopped` to connected trackers for up to 5 seconds.


* **Layered input validation.** A given file is validated in two passes:
  - BencodeParser's `parse()` only asks "is this valid bencode?" It rejects
    non-canonical integers (`i03e`, `i-0e`), unsorted or duplicate keys,
    string lengths past the end of the input, trailing data, and nesting
    deeper than 100 levels. Also fuzzed with libFuzzer under ASan and UBSan.
  - `parse_torrent()` then asks "can this be a real torrent?" It rejects unsafe
    paths (`..`, separators, NUL), piece counts that don't match the total
    length, and lengths that overflow.


* **Bans peers that send bad data, without punishing the honest ones.** A piece's
  hash only says the whole piece is wrong, not which block, so every block is
  recorded with the peer that sent it. If one peer sent all of a failing piece, it's
  banned by IP on the spot. Otherwise the failed copy is kept as evidence, and once
  a good copy of that piece verifies, it's compared block by block: only the peers
  whose blocks don't match are banned. Good peers that happened to share a piece with
  a bad one are never caught in the crossfire. In the `bad-data` test (one honest
  seeder, one peer sending corrupt blocks), the client used to finish 1 of 17 pieces
  in 30 s while transferring about 4 GiB for a 4 MiB file; it now bans the bad peer
  and finishes in under a second.


* **RAII resource ownership.** Every socket and file descriptor is wrapped in
  move-only `UniqueFd`, so nothing can leak or be closed twice. Managers
  have sole ownership of their `UniqueFds`; Session only takes their raw values
  for polling.

## Limitations

**Not supported**

- HTTP/HTTPS trackers and web seeds; only UDP trackers (BEP 15)
- Magnet links (BEP 9); a `.torrent` file is required
- DHT, PEX, the fast extension (BEP 6), and the extension protocol (BEP 10)
- Protocol encryption (MSE/PE)
- IPv6
- One torrent per process
- Choking algorithm: every peer is unchoked on connect
- Endgame mode, so the last few pieces can be slow
- `cancel` messages are ignored
- Bandwidth limits
- POSIX only (`poll`, `pwrite`, `posix_fallocate`); works under WSL

**Known issues**

- No keep-alives are sent and idle peers are never timed out
- A snubbed peer can hold one block for the 15 s request timeout (no endgame mode)
- A peer's request queue and write buffer are unbounded
- DNS resolution blocks the event loop
- One fd per file for huge torrents can potentially trigger the per-process soft limit

## References

- [BEP 3 : The BitTorrent Protocol Specification](https://www.bittorrent.org/beps/bep_0003.html)
- [BEP 15 : UDP Tracker Protocol](https://www.bittorrent.org/beps/bep_0015.html)
- [BEP 12 : Multitracker Metadata Extension](https://www.bittorrent.org/beps/bep_0012.html)