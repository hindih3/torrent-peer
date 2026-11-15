#!/usr/bin/env python3
"""Runs the real torrent-peer binary against fake peers on localhost.

Each fake peer is a seeder with a scripted behavior, to show how the client
copes with peers that stall, choke or lie. The fake peers serve the file
straight from memory; the client downloads into a temporary directory.

The seeding scenarios flip the roles: the client starts with the whole file
and a fake leecher dials in and asks for it.

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

CHOKE, UNCHOKE, INTERESTED, BITFIELD, REQUEST, PIECE, CANCEL = 0, 1, 2, 5, 6, 7, 8
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
    def __init__(self, behaviour, data, corrupt, info_hash, host):
        assert behaviour in BEHAVIOURS
        self.behaviour = behaviour
        self.data = corrupt if behaviour == "BadData" else data
        self.info_hash = info_hash
        self.host = host

        # the kernel picks the port, so scenarios never fight over fixed ones
        self.listener = socket.socket()
        self.listener.bind((host, 0))
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
    # one loopback address per peer (Linux routes all of 127.0.0.0/8 to lo),
    # so the client sees distinct IPs and an IP ban hits only the peer it means
    peers = [FakePeer(b, data, corrupt, info_hash, f"127.0.0.{i + 2}")
             for i, b in enumerate(behaviours)]

    download_dir = os.path.join(work, name)
    args = [client, torrent_path, download_dir, "--no-tracker"]
    for p in peers:
        args += ["--peer", f"{p.host}:{p.port}"]

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
        print(f"      peer {p.host}:{p.port:<5} {p.behaviour:<9} {p.requests:>6} requests, "
              f"{p.served:>6} answered, {p.link}")
        if p.conn:
            p.conn.close()
        if p.listener:
            p.listener.close()
    check(sum(p.protocol_errors for p in peers) == 0, f"{name}: no malformed messages or requests")


# --- seeding scenarios --------------------------------------------------------

MIB = 1024 * 1024

FLOOD_REQUESTS = 4000             # 62.5 MiB of blocks, 15x the size of the file
FLOOD_MEMORY_LIMIT = 16 * MIB     # generous next to any sane per-peer send cap
FLOOD_SETTLE = 2.0                # seconds for the client to work through them


def all_blocks():
    return [(index, begin, min(MAX_BLOCK, piece_length(index) - begin))
            for index in range(PIECE_COUNT)
            for begin in range(0, piece_length(index), MAX_BLOCK)]


def proc_status_bytes(pid, field):
    with open(f"/proc/{pid}/status") as f:
        for line in f:
            if line.startswith(field + ":"):
                return int(line.split()[1]) * 1024  # reported in kB
    raise KeyError(field)


class FakeLeecher:
    """Dials the client, handshakes and (by default) waits to be unchoked. What
    it does after that, including whether it ever reads again, is up to the
    scenario."""

    def __init__(self, host, info_hash, rcvbuf=None):
        self.info_hash = info_hash
        self.buf = bytearray()
        self.sock = socket.socket()
        if rcvbuf is not None:  # must be set before connect to shrink the window
            self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, rcvbuf)
        self.sock.bind((host, 0))

    # We dialed, so we speak first. Requests sent before the client's unchoke
    # are ignored, so wait for it rather than guessing how long it takes.
    # wait_unchoke=False returns after the handshake, for a peer the client
    # has no slot for yet.
    def connect(self, port, timeout=5.0, wait_unchoke=True):
        self.sock.settimeout(timeout)
        try:
            self.sock.connect(("127.0.0.1", port))
            self.sock.sendall(bytes([19]) + PROTOCOL + bytes(8) + self.info_hash
                              + b"-FL0000-".ljust(20, b"-") + message(INTERESTED))
            while len(self.buf) < 68:
                self.buf += self.recv_or_eof()
            hs = self.buf[:68]
            del self.buf[:68]
            if hs[0] != 19 or hs[1:20] != PROTOCOL or hs[28:48] != self.info_hash:
                return False
            if not wait_unchoke:
                return True
            while self.read_message()[0] != UNCHOKE:
                pass
            return True
        except (OSError, EOFError):
            return False

    # Returns (id, payload) of the next message, skipping keep-alives. Bytes
    # past it stay in self.buf for the next call.
    def read_message(self):
        while True:
            while len(self.buf) < 4:
                self.buf += self.recv_or_eof()
            (length,) = struct.unpack(">I", self.buf[:4])
            while len(self.buf) < 4 + length:
                self.buf += self.recv_or_eof()
            msg = bytes(self.buf[4:4 + length])
            del self.buf[:4 + length]
            if msg:
                return msg[0], msg[1:]

    def recv_or_eof(self):
        chunk = self.sock.recv(65536)
        if not chunk:
            raise EOFError
        return chunk

    def close(self):
        self.sock.close()


def start_seeder(client, torrent_path, download_dir, data):
    """Starts the client with the whole file already on disk and waits until it
    has rechecked it and is seeding. Returns (proc, port, log_path)."""
    os.makedirs(download_dir)
    with open(os.path.join(download_dir, "swarm.bin"), "wb") as f:
        f.write(data)

    # ask the kernel for a free port, then hand it to the client
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]

    # ASan keeps freed memory in a quarantine (256 MiB by default) that would
    # swamp any memory measurement; this is ignored by non-ASan builds
    env = dict(os.environ)
    env["ASAN_OPTIONS"] = env.get("ASAN_OPTIONS", "") + ":quarantine_size_mb=0"

    log_path = download_dir + ".log"
    with open(log_path, "wb") as log:
        proc = subprocess.Popen([client, torrent_path, download_dir, "--no-tracker",
                                 "--port", str(port)], stderr=log, env=env)

    deadline = time.monotonic() + 10
    while time.monotonic() < deadline and proc.poll() is None:
        with open(log_path, "rb") as f:
            if b"seeding" in f.read():
                return proc, port, log_path
        time.sleep(0.05)
    return None, port, log_path


def stop_client(proc):
    proc.send_signal(signal.SIGINT)
    try:
        return proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        return proc.wait()


# A leecher that pipelines thousands of requests and never reads a byte. Each
# request is 17 bytes but asks for a 16 KiB block, so it costs the peer almost
# nothing to make the client queue tens of MiB for it. However the client
# defends itself (capping what it queues per peer, ignoring requests past a
# limit, or dropping the peer), its memory should stay roughly flat.
def run_write_flood(name, client, torrent_path, work, data, info_hash):
    proc, port, log_path = start_seeder(client, torrent_path, os.path.join(work, name), data)
    if proc is None:
        check(False, f"{name}: client never started seeding (log: {log_path})")
        return

    # tiny receive window so the kernel can't soak up much on our behalf
    leecher = FakeLeecher("127.0.0.2", info_hash, rcvbuf=4096)
    if not leecher.connect(port):
        check(False, f"{name}: handshake with the client failed")
        leecher.close()
        stop_client(proc)
        return

    blocks = all_blocks()
    flood = b"".join(message(REQUEST, struct.pack(">III", *blocks[k % len(blocks)]))
                     for k in range(FLOOD_REQUESTS))
    asked = FLOOD_REQUESTS * MAX_BLOCK

    before = proc_status_bytes(proc.pid, "VmRSS")
    try:
        leecher.sock.sendall(flood)
        sent = "all requests sent"
    except socket.timeout:
        sent = "client stopped reading requests"   # backpressure; fine
    except OSError:
        sent = "client hung up"                     # dropping us is fine too
    time.sleep(FLOOD_SETTLE)
    grew = proc_status_bytes(proc.pid, "VmHWM") - before

    check(grew <= FLOOD_MEMORY_LIMIT,
          f"{name}: client memory grew {grew / MIB:.1f} MiB for one peer that asked for "
          f"{asked / MIB:.1f} MiB and never read (limit {FLOOD_MEMORY_LIMIT // MIB} MiB)")
    print(f"      {sent}")

    leecher.close()
    exit_code = stop_client(proc)
    check(exit_code == 0, f"{name}: client exited cleanly (code {exit_code})")


CANCEL_BLOCKS = 256          # distinct blocks asked for and then taken back
CANCEL_SETTLE = 0.5          # seconds for the client to queue them before the cancels
PIECE_MSG = 13 + MAX_BLOCK   # a full block on the wire


# Bytes the kernel holds for one TCP connection, both ends together: the
# client's send queue plus our receive queue. That's what a cancel can no
# longer call back. Read from /proc/net/tcp, so Linux only, like VmRSS above.
def kernel_held_bytes(sock):
    def ends(addr):
        ip, port = addr
        return socket.inet_aton(ip)[::-1].hex().upper() + f":{port:04X}"
    ours, theirs = ends(sock.getsockname()), ends(sock.getpeername())
    total = 0
    with open("/proc/net/tcp") as f:
        next(f)  # header
        for line in f:
            fields = line.split()
            if {fields[1], fields[2]} == {ours, theirs}:
                tx, rx = (int(x, 16) for x in fields[4].split(":"))
                total += tx + rx
    return total


# Reads until the sentinel block arrives. The client's write queue is FIFO, so
# any cancelled block that still went out arrives before it. Every block is
# checked against the file: if a half-sent message had been pulled from the
# queue, the stream would desync and this would fail. Returns (blocks before
# the sentinel, error or None).
def read_until_sentinel(leecher, sentinel, data):
    before = 0
    try:
        while True:
            msg_id, payload = leecher.read_message()
            if msg_id != PIECE:
                continue
            index, begin = struct.unpack(">II", payload[:8])
            off = index * PIECE_LENGTH + begin
            if payload[8:] != data[off:off + len(payload) - 8]:
                return before, f"block {index}/{begin} doesn't match the file (stream desynced?)"
            if (index, begin, len(payload) - 8) == sentinel:
                return before, None
            before += 1
    except socket.timeout:
        return before, "sentinel block never arrived"
    except (OSError, EOFError):
        return before, "client hung up"


def connect_leecher(name, host, port, info_hash, **kw):
    leecher = FakeLeecher(host, info_hash, **kw)
    if leecher.connect(port):
        return leecher
    check(False, f"{name}: handshake with the client failed")
    leecher.close()
    return None


# A peer takes requests back. Two ways a cancel can catch a request:
#  - same batch: each request is followed straight away by its cancel, all in
#    one write, so the client parses both before serving anything. None of
#    them should ever be read from disk or sent. This check is exact.
#  - already queued: requests first, then (once the client has queued them)
#    cancels. Whatever the kernel has already taken can't be called back, so
#    the test measures that just before cancelling and allows exactly those
#    blocks, plus one partly in the kernel at each end of it.
# A final uncancelled request marks the end of each phase.
def run_cancel(name, client, torrent_path, work, data, info_hash):
    proc, port, log_path = start_seeder(client, torrent_path, os.path.join(work, name), data)
    if proc is None:
        check(False, f"{name}: client never started seeding (log: {log_path})")
        return

    blocks = all_blocks()
    asked, sentinel = blocks[:CANCEL_BLOCKS], blocks[CANCEL_BLOCKS]
    request = lambda b: message(REQUEST, struct.pack(">III", *b))
    cancel = lambda b: message(CANCEL, struct.pack(">III", *b))

    # 34 bytes a pair: all of it fits in one of the client's 16 KiB reads
    leecher = connect_leecher(name, "127.0.0.2", port, info_hash)
    if leecher:
        leecher.sock.sendall(b"".join(request(b) + cancel(b) for b in asked) + request(sentinel))
        leaked, error = read_until_sentinel(leecher, sentinel, data)
        check(error is None and leaked == 0,
              f"{name}: same-batch cancels: {leaked} of {len(asked)} cancelled blocks sent"
              + (f" ({error})" if error else ""))
        leecher.close()

    # tiny receive window, so most of what the client queues stays queued
    leecher = connect_leecher(name, "127.0.0.3", port, info_hash, rcvbuf=4096)
    if leecher:
        leecher.sock.sendall(b"".join(request(b) for b in asked))
        time.sleep(CANCEL_SETTLE)
        held = kernel_held_bytes(leecher.sock)
        limit = held // PIECE_MSG + 2
        leecher.sock.sendall(b"".join(cancel(b) for b in asked) + request(sentinel))
        leaked, error = read_until_sentinel(leecher, sentinel, data)
        check(error is None and leaked <= limit,
              f"{name}: queued cancels: {leaked} of {len(asked)} cancelled blocks sent, "
              f"kernel already held {held / MIB:.2f} MiB (limit {limit})"
              + (f" ({error})" if error else ""))
        # if the kernel held everything, nothing was left to cancel
        check(limit < len(asked) // 2,
              f"{name}: at least half the blocks were still cancellable")
        leecher.close()

    exit_code = stop_client(proc)
    check(exit_code == 0, f"{name}: client exited cleanly (code {exit_code})")


# Speeds are set by how fast each leecher asks, not how fast it reads: the
# client answers requests as they come, so its per-round upload count follows
# the request rate exactly and socket buffers can't blur it.
CHOKE_SLOW_RATES = [2, 4, 6, 8, 10, 12, 14]  # blocks/s, one leecher per regular slot
CHOKE_FAST_RATE = 64                          # blocks/s, 1 MiB/s
CHOKE_FAST_COUNT = 3
CHOKE_MAX_UNCHOKED = len(CHOKE_SLOW_RATES) + 1  # regular slots + the optimistic one
CHOKE_OPTIMISTIC_AT = 30     # session.cpp's kOptimisticInterval
CHOKE_DEADLINE = 50          # the rechoke after the optimistic pick lands near 40s
CHOKE_REQUEST_DEPTH = 8      # requests in flight per leecher


class PacedLeecher:
    """Wraps a connected FakeLeecher. While unchoked it requests at a fixed
    rate, cycling through the file forever so it never stops being interested,
    and it records every choke and unchoke the client sends it."""

    def __init__(self, leecher, label, rate, start, unchoked):
        self.leecher = leecher
        self.label = label
        self.rate = rate
        self.start = start
        self.choked = not unchoked
        self.events = [(time.monotonic() - start, "unchoke")] if unchoked else []
        self.blocks = all_blocks()
        self.next_block = random.randrange(len(self.blocks))
        self.outstanding = 0
        self.next_at = 0.0
        leecher.sock.setblocking(False)

    def times(self, kind):
        return [t for t, ev in self.events if ev == kind]

    def on_readable(self):
        try:
            chunk = self.leecher.sock.recv(65536)
        except BlockingIOError:
            return
        if not chunk:
            raise EOFError(f"client hung up on {self.label}")
        t = time.monotonic() - self.start
        buf = self.leecher.buf
        buf += chunk
        while len(buf) >= 4:
            (length,) = struct.unpack(">I", buf[:4])
            if len(buf) < 4 + length:
                break
            msg_id = buf[4] if length else None  # None: keep-alive
            del buf[:4 + length]
            if msg_id == CHOKE:
                self.choked = True
                self.outstanding = 0  # a choke discards our requests
                self.events.append((t, "choke"))
            elif msg_id == UNCHOKE:
                self.choked = False
                self.events.append((t, "unchoke"))
            elif msg_id == PIECE:
                self.outstanding = max(0, self.outstanding - 1)

    def tick(self, now):
        if self.choked or self.outstanding >= CHOKE_REQUEST_DEPTH or now < self.next_at:
            return
        block = self.blocks[self.next_block % len(self.blocks)]
        try:
            self.leecher.sock.send(message(REQUEST, struct.pack(">III", *block)))
        except BlockingIOError:
            return
        self.next_block += 1
        self.outstanding += 1
        self.next_at = now + 1 / self.rate


# The client seeds to more leechers than it has slots. A seeder can only rank
# peers it's already sending to, so a choked leecher stays at 0 bytes and the
# only way in is the optimistic slot. The timeline (seconds since the client
# started):
#   0    7 slow leechers connect one at a time; each takes a free regular slot
#   ~0   3 fast leechers connect and stay choked: no slot left
#   ~30  the optimistic pick has to be a fast one, the only choked, interested peers
#   ~40  rechoke: that fast leecher ranks first and keeps its slot, so the
#        slowest slow leecher drops to 8th and is choked
# Everything is read off the wire from the leechers' side, so it doesn't
# depend on the client's log format.
def run_choking(name, client, torrent_path, work, data, info_hash):
    proc, port, log_path = start_seeder(client, torrent_path, os.path.join(work, name), data)
    if proc is None:
        check(False, f"{name}: client never started seeding (log: {log_path})")
        return
    start = time.monotonic()  # the client's rechoke timers started just before this

    peers = []
    for rate in CHOKE_SLOW_RATES:
        leecher = connect_leecher(name, f"127.0.0.{len(peers) + 2}", port, info_hash)
        if leecher is None:
            break
        peers.append(PacedLeecher(leecher, f"slow {rate}/s", rate, start, unchoked=True))
    for _ in range(CHOKE_FAST_COUNT if len(peers) == len(CHOKE_SLOW_RATES) else 0):
        leecher = FakeLeecher(f"127.0.0.{len(peers) + 2}", info_hash)
        if not leecher.connect(port, wait_unchoke=False):
            check(False, f"{name}: handshake with the client failed")
            leecher.close()
            break
        peers.append(PacedLeecher(leecher, f"fast {CHOKE_FAST_RATE}/s", CHOKE_FAST_RATE,
                                  start, unchoked=False))
    if len(peers) != len(CHOKE_SLOW_RATES) + CHOKE_FAST_COUNT:
        for p in peers:
            p.leecher.close()
        stop_client(proc)
        return
    slow, fast = peers[:len(CHOKE_SLOW_RATES)], peers[len(CHOKE_SLOW_RATES):]

    most_unchoked, settle_until, error = 0, None, None
    by_sock = {p.leecher.sock: p for p in peers}
    while time.monotonic() - start < CHOKE_DEADLINE:
        r, _, _ = select.select(list(by_sock), [], [], 0.005)
        try:
            for s in r:
                by_sock[s].on_readable()
            now = time.monotonic()
            for p in peers:
                p.tick(now)
        except (EOFError, OSError) as e:
            error = str(e)
            break
        most_unchoked = max(most_unchoked, sum(not p.choked for p in peers))

        # done once a fast leecher is in and a slow one has been choked; keep
        # reading a moment so every message from that rechoke has arrived
        t = time.monotonic() - start
        if settle_until is None and any(f.events for f in fast) \
                and any(s.times("choke") for s in slow):
            settle_until = t + 1.0
        if settle_until is not None and t >= settle_until:
            break

    if error:
        check(False, f"{name}: {error}")

    picked = [f for f in fast if f.times("unchoke")]
    picked_at = min((t for f in picked for t in f.times("unchoke")), default=None)
    check(len(picked) == 1 and abs(picked_at - CHOKE_OPTIMISTIC_AT) <= 5,
          f"{name}: one fast leecher got the optimistic slot near {CHOKE_OPTIMISTIC_AT}s "
          f"({len(picked)} picked" + (f", first at {picked_at:.1f}s)" if picked else ")"))

    choked = [(s, t) for s in slow for t in s.times("choke")]
    check(len(choked) == 1 and choked[0][0].rate == min(CHOKE_SLOW_RATES)
          and picked_at is not None and choked[0][1] > picked_at,
          f"{name}: the next rechoke choked only the slowest leecher (choked: "
          + (", ".join(f"{s.label} at {t:.1f}s" for s, t in choked) or "none") + ")")

    check(len(picked) == 1 and not picked[0].choked,
          f"{name}: the fast leecher kept its slot on merit")
    check(most_unchoked <= CHOKE_MAX_UNCHOKED,
          f"{name}: at most {CHOKE_MAX_UNCHOKED} leechers unchoked at once (saw {most_unchoked})")

    for p in peers:
        history = ", ".join(f"{ev} {t:.1f}s" for t, ev in p.events) or "always choked"
        print(f"      {p.label:<11} {history}")
        p.leecher.close()
    exit_code = stop_client(proc)
    check(exit_code == 0, f"{name}: client exited cleanly (code {exit_code})")


SEED_SCENARIOS = [
    ("write-flood", run_write_flood,
     "a peer that floods requests and never reads shouldn't make the client queue without limit"),
    ("cancel", run_cancel,
     "cancelled requests shouldn't be sent, unless they're already on their way"),
    ("choking", run_choking,
     "a fast leecher let in by the optimistic slot should take the slowest leecher's slot"),
]


def main():
    if len(sys.argv) < 2:
        sys.exit("usage: swarm_test.py <path/to/torrent-peer> [scenario]")
    client = os.path.abspath(sys.argv[1])
    only = sys.argv[2] if len(sys.argv) > 2 else None
    if only is not None and only not in [s[0] for s in SCENARIOS + SEED_SCENARIOS]:
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

        for name, run, shows in SEED_SCENARIOS:
            if only is None or only == name:
                print(f"\n== {name}: {shows}")
                run(name, client, torrent_path, work, data, info_hash)

    print(f"\n{failures} failure(s)")
    sys.exit(failures != 0)


if __name__ == "__main__":
    main()