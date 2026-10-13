# gsdump.py FILE.gs: parse a PCSX2 GS dump (uncompressed) and yield its GIF transfers and vertices.
import struct, sys

def read_dump(path):
    b = open(path, "rb").read()
    assert struct.unpack_from("<I", b, 0)[0] == 0xFFFFFFFF
    hsz = struct.unpack_from("<I", b, 4)[0]
    ver, ssz = struct.unpack_from("<II", b, 8)
    p = 8 + hsz
    state = b[p:p + ssz]; p += ssz
    regs = b[p:p + 8192]; p += 8192
    packets = []
    while p < len(b):
        t = b[p]; p += 1
        if t == 0:
            path = b[p]; size = struct.unpack_from("<I", b, p + 1)[0]; p += 5
            packets.append(("xfer", path, b[p:p + size])); p += size
        elif t == 1: packets.append(("vsync", b[p])); p += 1
        elif t == 2: size = struct.unpack_from("<I", b, p)[0]; p += 4; packets.append(("fifo2", size))
        elif t == 3: packets.append(("regs", b[p:p + 8192])); p += 8192
        else: raise SystemExit("bad packet type %d at %d" % (t, p))
    return state, regs, packets

def vertices(data, path):
    """Walk GIF tags; yield (path, kind, x, y, z, adc, prim_reg_bits, xyoffset unknown) for XYZ2/XYZF2/XYZ3 writes."""
    q = 0; n = len(data) // 16; prim = None
    while q < n:
        lo, hi = struct.unpack_from("<QQ", data, q * 16); q += 1
        nloop = lo & 0x7FFF; pre = lo >> 46 & 1; flg = lo >> 58 & 3; nreg = lo >> 60 & 15 or 16
        if pre: prim = lo >> 47 & 0x7FF
        regs = [(hi >> (4 * i)) & 15 for i in range(nreg)]
        if flg == 0:
            for _ in range(nloop):
                for r in regs:
                    if q >= n: return
                    a, c = struct.unpack_from("<QQ", data, q * 16); q += 1
                    if r == 5:
                        yield ("XYZ2p", a & 0xFFFF, a >> 32 & 0xFFFF, c & 0xFFFFFFFF, c >> 47 & 1, prim)
                    elif r == 4:
                        yield ("XYZF2p", a & 0xFFFF, a >> 32 & 0xFFFF, (c >> 4) & 0xFFFFFF, c >> 47 & 1, prim)
                    elif r == 0xE:
                        addr = c & 0xFF
                        if addr == 0: prim = a & 0x7FF
                        elif addr in (5, 13): yield ("XYZ%sad" % ("2" if addr == 5 else "3"), a & 0xFFFF, a >> 16 & 0xFFFF, a >> 32, 0, prim)
        elif flg == 1:
            words = []
            total = nloop * nreg
            nq = (total + 1) // 2
            raw = data[q * 16:(q + nq) * 16]; q += nq
            for i in range(total):
                v = struct.unpack_from("<Q", raw, 8 * i)[0]; r = regs[i % nreg]
                if r in (5, 13): yield ("XYZ%sr" % ("2" if r == 5 else "3"), v & 0xFFFF, v >> 16 & 0xFFFF, v >> 32, 1 if r == 13 else 0, prim)
                elif r == 0: prim = v & 0x7FF
        else:
            q += nloop

if __name__ == "__main__":
    state, regs, packets = read_dump(sys.argv[1])
    import collections
    c = collections.Counter((p[0], p[1] if p[0] == "xfer" else None) for p in packets)
    print(c)
