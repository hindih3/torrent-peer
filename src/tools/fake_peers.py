import socket, threading, time
def serve(port, mode):
    s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(('127.0.0.1', port)); s.listen(8)
    while True:
        c, _ = s.accept()
        hs = b''
        while len(hs) < 68:
            chunk = c.recv(68 - len(hs))
            if not chunk: break
            hs += chunk
        if mode == 'good':      # echo their info hash, with our own peer id
            c.sendall(hs[:48] + b'-PY0001-' + b'x'*12 + b'\x00\x00\x00\x01\x05')  # + bitfield header start
        elif mode == 'wronghash':
            c.sendall(hs[:28] + b'\xff'*20 + b'-PY0001-' + b'y'*12)
        elif mode == 'mirror':  # hand their own peer id back: looks like us
            c.sendall(hs)
        elif mode == 'silent':
            time.sleep(60)
        print(f"[peer {port}] served mode={mode}", flush=True)
        if mode != 'good': c.close()
        else: threading.Thread(target=lambda c=c: (time.sleep(30), c.close()), daemon=True).start()
for port, mode in [(7001,'good'), (7002,'wronghash'), (7003,'mirror'), (7004,'silent')]:
    threading.Thread(target=serve, args=(port, mode), daemon=True).start()
time.sleep(60)