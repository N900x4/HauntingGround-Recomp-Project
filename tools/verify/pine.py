# Minimal PCSX2 PINE client. Usage: python3 pine.py r32 ADDR [n] | rf ADDR n | w32 ADDR VAL | status | save N | load N
# PINE slot: $PINE_SLOT, default 28011 (PCSX2's default). Windows: TCP 127.0.0.1:slot; Linux: the Unix socket
# $XDG_RUNTIME_DIR/pcsx2.sock (pcsx2.sock.SLOT for other slots).
import socket, struct, sys, os
SLOT = int(os.environ.get("PINE_SLOT", "28011"))
P = os.path.join(os.environ.get("XDG_RUNTIME_DIR", "/tmp"), "pcsx2.sock" + ("" if SLOT == 28011 else ".%d" % SLOT))
def connect():
    if os.name == "nt":
        s = socket.create_connection(("127.0.0.1", SLOT)); s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1); return s
    s = socket.socket(socket.AF_UNIX); s.connect(P); return s
def call(payload):
    s = connect()
    s.sendall(struct.pack("<I", len(payload) + 4) + payload)
    hdr = b""
    while len(hdr) < 4: hdr += s.recv(4 - len(hdr))
    n = struct.unpack("<I", hdr)[0]; d = b""
    while len(d) < n - 4: d += s.recv(n - 4 - len(d))
    s.close()
    if d[0] != 0: raise SystemExit("PINE fail")
    return d[1:]
def r32(a): return struct.unpack("<I", call(bytes([2]) + struct.pack("<I", a)))[0]
def w32(a, v): call(bytes([6]) + struct.pack("<II", a, v))
def rf(a, n): return [struct.unpack("<f", struct.pack("<I", r32(a + 4 * i)))[0] for i in range(n)]
if __name__ == "__main__":
    c = sys.argv[1]
    if c == "r32":
        a = int(sys.argv[2], 16); n = int(sys.argv[3]) if len(sys.argv) > 3 else 1
        print(" ".join("%08x" % r32(a + 4 * i) for i in range(n)))
    elif c == "rf": print(rf(int(sys.argv[2], 16), int(sys.argv[3])))
    elif c == "w32": w32(int(sys.argv[2], 16), int(sys.argv[3], 16))
    elif c == "status": print(["running", "paused", "shutdown"][struct.unpack("<I", call(bytes([0xF])))[0]])
    elif c == "save": call(bytes([9, int(sys.argv[2])]))
    elif c == "load": call(bytes([0xA, int(sys.argv[2])]))
    elif c == "fiona":
        p = r32(0x44f818); print("obj %08x" % p, rf(p + 0x10, 4) if p else None)

def dump(path, base=0, size=32 << 20, batch=40000):
    """Batched 64-bit reads (PINE MsgRead64) of EE RAM into a file."""
    out = bytearray()
    for a in range(base, base + size, 8 * batch):
        n = min(batch, (base + size - a) // 8)
        req = b"".join(bytes([3]) + struct.pack("<I", a + 8 * i) for i in range(n))
        s = connect()
        s.sendall(struct.pack("<I", len(req) + 4) + req)
        hdr = b""
        while len(hdr) < 4: hdr += s.recv(4 - len(hdr))
        m = struct.unpack("<I", hdr)[0] - 4; d = b""
        while len(d) < m: d += s.recv(m - len(d))
        s.close()
        if d[0] != 0: raise SystemExit("PINE fail at %x" % a)
        out += d[1:]
    open(path, "wb").write(out)

if __name__ == "__main__" and sys.argv[1] == "dump":
    dump(sys.argv[2])
