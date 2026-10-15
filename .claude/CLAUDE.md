# torrent-peer

An educational project for learning TCP/UDP networking in C++20, meant to ship
as a finished, polished BitTorrent client.

## Ground rules

- OpenSSL is the only dependency, and it is used only for SHA-1. Don't
  write crypto by hand, and don't add other libraries (Boost/Asio, networking
  or test frameworks). The sockets, poll loop, and protocol code are written
  by hand on purpose.
- Keep the README's Limitations and Known issues lists accurate when behavior
  changes.

## Working style

The author needs to be able to explain every line, so no black boxes:

- Do only what was asked. Review findings and ideas are suggestions to
  discuss, not a queue to work through.
- Default to explaining and pointing at code (file:line) over writing it.
  When code is wanted, make one small, focused change at a time and stop
  for review before the next one.
- For anything beyond a few lines, propose the approach first (what's wrong,
  the fix, alternatives) and wait for a go-ahead.
- Never touch several managers or files in one go without that agreement.

## Commands

```sh
# sanitized build + tests (what CI runs)
cmake -B build-debug -DTP_SANITIZE=ON -DTP_ASSERTIONS=ON
cmake --build build-debug -j
ctest --test-dir build-debug --output-on-failure

# naming lint; CI uses -warnings-as-errors='*'
run-clang-tidy -p build-debug -quiet -header-filter='src/.*'
```

Tests are plain executables that return non-zero on failure, with no framework.
