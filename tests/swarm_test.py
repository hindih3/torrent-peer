#!/usr/bin/env python3
"""Runs the real torrent-peer binary against fake peers on localhost.

Each fake peer is a seeder with a scripted behavior, to show how the client
copes with peers that stall, choke or lie. The fake peers serve the file
straight from memory; the client downloads into a temporary directory.

usage: swarm_test.py <path/to/torrent-peer> [scenario]
"""
import hashlib
import os
import random
import select
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time

# 16 full pieces of 16 blocks each, plus a 1000-byte piece, so the last piece
# and its only block are both short. 257 blocks in all, far more than the 32
# a single peer is given at once.
PIECE_LENGTH = 256 * 1024
TOTAL_LENGTH = 16 * PIECE_LENGTH + 1000
PIECE_COUNT = (TOTAL_LENGTH + PIECE_LENGTH - 1) // PIECE_LENGTH
MAX_BLOCK = 16384

PIPELINE_DEPTH = 32       # session.cpp's kPipelineDepth
GOOD_UNCHOKE_DELAY = 0.5  # lets the client fill a bad peer's pipeline first

CHOKE, UNCHOKE, BITFIELD, REQUEST, PIECE = 0, 1, 5, 6, 7
PROTOCOL = b"BitTorrent protocol"

failures = 0


def check(good, label):
    global failures
    print(("ok    " if good else "FAIL  ") + label)
    if not good:
        failures += 1


# --- test torrent -------------------------------------------------------------

def bencode(v):
    if isinstance(v, int):
        return b"i%de" % v
    if isinstance(v, str):
        v = v.encode()
    if isinstance(v, bytes):
        return b"%d:%s" % (len(v), v)
    if isinstance(v, dict):  # keys must be sorted
        return b"d" + b"".join(bencode(k) + bencode(v[k]) for k in sorted(v)) + b"e"
    raise TypeError(v)


def make_torrent():
    data = random.Random(67).randbytes(TOTAL_LENGTH)  # fixed seed: reproducible
    hashes = b"".join(hashlib.sha1(data[off:off + PIECE_LENGTH]).digest()
                      for off in range(0, TOTAL_LENGTH, PIECE_LENGTH))
    info = bencode({"name": "swarm.bin", "piece length": PIECE_LENGTH,
                    "length": TOTAL_LENGTH, "pieces": hashes})
    return data, b"d4:info" + info + b"e", hashlib.sha1(info).digest()


def piece_length(index):
    return min(PIECE_LENGTH, TOTAL_LENGTH - index * PIECE_LENGTH)


def message(msg_id, payload=b""):
    return struct.pack(">IB", 1 + len(payload), msg_id) + payload


# --- fake peers ---------------------------------------------------------------

BEHAVIOURS = {
    "Good":      "serves every request, but unchokes late so a bad peer is asked first",
    "Silent":    "takes requests and never answers them",
    "Forgetful": "ignores its first full pipeline of requests, then serves normally",
    "Choker":    "chokes the client as soon as it's asked for anything",
    "Quitter":   "hangs up as soon as it's asked for anything",
    "BadData":   "answers every request with corrupted bytes",
}


class FakePeer:
    def __init__(self, behaviour, data, corrupt, info_hash):
        assert behaviour in BEHAVIOURS
        self.behaviour = behaviour
        self.data = corrupt if behaviour == "BadData" else data
        self.info_hash = info_hash

        # the kernel picks the port, so scenarios never fight over fixed ones
        self.listener = socket.socket()
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen(1)
        self.port = self.listener.getsockname()[1]

        self.conn = None
        self.inbuf = bytearray()
        self.outbuf = bytearray()
        self.handshaken = False
        self.unchoke_at = None
        self.choked_client = False

        # what the peer saw, for the report
        self.requests = 0
        self.served = 0
        self.protocol_errors = 0  # anything a well-behaved client shouldn't send
        self.link = "never connected"

    def sock(self):
        return self.conn or self.listener

    def accept(self):
        self.conn, _ = self.listener.accept()
        self.conn.setblocking(False)
        self.listener.close()  # one client per peer
        self.listener = None
        self.link = "still connected"

    def hang_up(self, by_client):
        self.conn.close()
        self.conn = None
        self.link = "client hung up" if by_client else "we hung up"

    def on_readable(self):
        try:
            chunk = self.conn.recv(65536)
        except BlockingIOError:
            return
        except OSError:
            chunk = b""
        if not chunk:
            self.hang_up(by_client=True)
            return
        self.inbuf += chunk
        self.handle_input()

    def on_writable(self):
        try:
            sent = self.conn.send(self.outbuf)
        except BlockingIOError:
            return
        except OSError:
            self.hang_up(by_client=True)
            return
        del self.outbuf[:sent]

    def on_tick(self, now):
        if self.conn and self.unchoke_at is not None and now >= self.unchoke_at:
            self.outbuf += message(UNCHOKE)
            self.unchoke_at = None

    # The client dialed us, so it speaks first. Check its handshake, then
    # answer with ours and a bitfield that has every piece.
    def answer_handshake(self):
        hs = self.inbuf[:68]
        del self.inbuf[:68]
        if hs[0] != 19 or hs[1:20] != PROTOCOL or hs[28:48] != self.info_hash:
            return False

        peer_id = f"-FP0000-{self.port}".encode().ljust(20, b"-")
        self.outbuf += bytes([19]) + PROTOCOL + bytes(8) + self.info_hash + peer_id

        nbytes = (PIECE_COUNT + 7) // 8
        bits = bytearray(b"\xff" * nbytes)
        bits[-1] &= (0xFF << (nbytes * 8 - PIECE_COUNT)) & 0xFF  # spare bits stay 0
        self.outbuf += message(BITFIELD, bytes(bits))

        self.handshaken = True
        delay = GOOD_UNCHOKE_DELAY if self.behaviour == "Good" else 0
        self.unchoke_at = time.monotonic() + delay
        return True

    # Frames whatever has arrived. Only requests matter; the client's
    # bitfield, unchoke, interested and have messages are read and ignored.
    def handle_input(self):
        if not self.handshaken:
            if len(self.inbuf) < 68:
                return
            if not self.answer_handshake():
                self.protocol_errors += 1
                self.hang_up(by_client=False)
                return

        while len(self.inbuf) >= 4:
            (length,) = struct.unpack(">I", self.inbuf[:4])
            if len(self.inbuf) < 4 + length:
                break
            msg = bytes(self.inbuf[4:4 + length])
            del self.inbuf[:4 + length]
            if msg and msg[0] == REQUEST:
                if length == 13:
                    self.on_request(*struct.unpack(">III", msg[1:]))
                else:
                    self.protocol_errors += 1

        # Both react to the first batch of requests as a whole, so none of
        # them is ever answered.
        if self.requests and self.behaviour == "Choker" and not self.choked_client:
            self.outbuf += message(CHOKE)
            self.choked_client = True
        if self.requests and self.behaviour == "Quitter":
            self.hang_up(by_client=False)

    def on_request(self, index, begin, length):
        self.requests += 1
        if not (index < PIECE_COUNT and 0 < length <= MAX_BLOCK
                and begin + length <= piece_length(index)):
            self.protocol_errors += 1
            return

        answer = (self.behaviour in ("Good", "BadData")
                  or (self.behaviour == "Forgetful" and self.requests > PIPELINE_DEPTH))
        if answer:
            off = index * PIECE_LENGTH + begin
            self.outbuf += message(PIECE, struct.pack(">II", index, begin)
                                   + self.data[off:off + length])
            self.served += 1


# --- scenarios ----------------------------------------------------------------

# Choke and disconnect limits sit far below the client's 15s request timeout,
# so passing means the requests were handed back without waiting for it.
SCENARIOS = [
    ("baseline",   ["Good"],              5, "one honest seeder, for reference"),
    ("choke",      ["Good", "Choker"],    5, "a choke should hand that peer's requests to another peer at once"),
    ("disconnect", ["Good", "Quitter"],   5, "a disconnect should hand that peer's requests to another peer at once"),
    ("timeout",    ["Forgetful"],        30, "timed-out requests should free the peer's pipeline slots"),
    ("silent",     ["Good", "Silent"],   30, "a peer that never answers costs one request timeout (until endgame mode)"),
    ("bad-data",   ["Good", "BadData"],  30, "a peer sending corrupt blocks shouldn't keep being given the same pieces"),
]


def run_scenario(name, behaviours, limit, shows, client, torrent_path, work, data, corrupt, info_hash):
    print(f"\n== {name}: {shows}")
    peers = [FakePeer(b, data, corrupt, info_hash) for b in behaviours]

    download_dir = os.path.join(work, name)
    args = [client, torrent_path, download_dir, "--no-tracker"]
    for p in peers:
        args += ["--peer", f"127.0.0.1:{p.port}"]

    started = time.monotonic()
    proc = subprocess.Popen(args, stderr=subprocess.PIPE)
    log = b""
    finished_after = None
    exited_early = False

    while time.monotonic() - started < limit:
        readable = [proc.stderr] + [p.sock() for p in peers if p.sock()]
        writable = [p.conn for p in peers if p.conn and p.outbuf]
        r, w, _ = select.select(readable, writable, [], 0.05)

        if proc.stderr in r:
            chunk = os.read(proc.stderr.fileno(), 4096)
            if not chunk:
                exited_early = True
                break
            log += chunk

        for p in peers:
            if p.listener and p.listener in r:
                p.accept()
                continue
            if p.conn and p.conn in w:
                p.on_writable()
            if p.conn and p.conn in r:
                p.on_readable()

        now = time.monotonic()
        for p in peers:
            p.on_tick(now)

        if b"download complete" in log:
            finished_after = now - started
            break

    # SIGINT is the client's normal way out (main.cpp's handler)
    proc.send_signal(signal.SIGINT)
    try:
        exit_code = proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        exit_code = proc.wait()

    if finished_after is not None:
        check(finished_after <= limit, f"{name}: finished in {finished_after:.2f}s (limit {limit}s)")
        with open(os.path.join(download_dir, "swarm.bin"), "rb") as f:
            check(f.read() == data, f"{name}: downloaded file matches")
    else:
        why = "client exited before finishing" if exited_early else f"not finished after {limit}s"
        check(False, f"{name}: {why}")
        print("      client log ends with:")
        for line in log.decode(errors="replace").splitlines()[-3:]:
            print("      " + line)
    check(exit_code == 0, f"{name}: client exited cleanly (code {exit_code})")

    for p in peers:
        print(f"      peer :{p.port:<5} {p.behaviour:<9} {p.requests:>6} requests, "
              f"{p.served:>6} answered, {p.link}")
        if p.conn:
            p.conn.close()
        if p.listener:
            p.listener.close()
    check(sum(p.protocol_errors for p in peers) == 0, f"{name}: no malformed messages or requests")


def main():
    if len(sys.argv) < 2:
        sys.exit("usage: swarm_test.py <path/to/torrent-peer> [scenario]")
    client = os.path.abspath(sys.argv[1])
    only = sys.argv[2] if len(sys.argv) > 2 else None
    if only is not None and only not in [s[0] for s in SCENARIOS]:
        sys.exit(f"unknown scenario: {only}")

    data, torrent, info_hash = make_torrent()
    corrupt = data.translate(bytes(range(255, -1, -1)))  # every byte flipped

    with tempfile.TemporaryDirectory(prefix="torrent-peer-test.") as work:
        torrent_path = os.path.join(work, "swarm.torrent")
        with open(torrent_path, "wb") as f:
            f.write(torrent)

        for name, behaviours, limit, shows in SCENARIOS:
            if only is None or only == name:
                run_scenario(name, behaviours, limit, shows, client, torrent_path,
                             work, data, corrupt, info_hash)

    print(f"\n{failures} failure(s)")
    sys.exit(failures != 0)


if __name__ == "__main__":
    main()
