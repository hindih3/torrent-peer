import socket, struct, random
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(('127.0.0.1', 6969))
names = {0: "none", 1: "completed", 2: "started", 3: "stopped"}
while True:
    data, addr = s.recvfrom(2048)
    if len(data) == 16:                                   # connect request
        _, action, txn = struct.unpack('>QII', data)
        s.sendto(struct.pack('>IIQ', 0, txn, random.getrandbits(64)), addr)
        print(f"[tracker] connect   txn={txn:#010x}", flush=True)
    elif len(data) >= 98:                                 # announce request
        _, action, txn = struct.unpack('>QII', data[:16])
        left  = struct.unpack('>Q', data[64:72])[0]
        event = struct.unpack('>I', data[80:84])[0]
        key   = struct.unpack('>I', data[88:92])[0]
        print(f"[tracker] announce  txn={txn:#010x} event={names[event]} left={left} key={key:#010x}", flush=True)
        peers = bytes([1,2,3,4]) + struct.pack('>H', 6881) + bytes([5,6,7,8]) + struct.pack('>H', 51413)
        s.sendto(struct.pack('>IIIII', 1, txn, 0, 3, 7) + peers, addr)   # interval 0 on purpose
