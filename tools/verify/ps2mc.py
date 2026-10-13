#!/usr/bin/env python3
"""ps2mc.py CARD.ps2 [OUTDIR]: list a raw PS2 memory card image (PCSX2 .ps2, 528-byte pages with ECC) and,
with OUTDIR, extract every save directory into OUTDIR/<dir>/<file> (the layout of the runtime's HG_MEMCARD_DIR/port0)."""
import os, struct, sys

class Card:
    def __init__(self, path):
        self.b = open(path, "rb").read()
        sb = self.b[:340]
        assert sb[:28] == b"Sony PS2 Memory Card Format ", "not a PS2 memory card image"
        self.page_len, self.ppc = struct.unpack_from("<HH", sb, 0x28)
        self.alloc_off, self.alloc_end, self.root = struct.unpack_from("<III", sb, 0x34)
        self.ifc = struct.unpack_from("<32I", sb, 0x50)
        self.raw_page = self.page_len + (16 if len(self.b) % (self.page_len + 16) == 0 else 0)
        self.csize = self.page_len * self.ppc

    def cluster(self, n):
        out = b""
        for p in range(n * self.ppc, (n + 1) * self.ppc):
            out += self.b[p * self.raw_page:p * self.raw_page + self.page_len]
        return out

    def fat(self, n):
        per = self.csize // 4
        ind = n // per
        ind_cl = self.ifc[ind // per]
        fat_cl = struct.unpack_from("<I", self.cluster(ind_cl), 4 * (ind % per))[0]
        return struct.unpack_from("<I", self.cluster(fat_cl), 4 * (n % per))[0]

    def chain(self, first):
        n = first
        while True:
            yield n
            e = self.fat(n)
            if not e & 0x80000000 or (e & 0x7FFFFFFF) == 0x7FFFFFFF: return
            n = e & 0x7FFFFFFF

    def read(self, first, size):
        data = b"".join(self.cluster(self.alloc_off + c) for c in self.chain(first))
        return data[:size]

    def entries(self, first, count):
        raw = self.read(first, count * 512)
        for i in range(count):
            e = raw[i * 512:(i + 1) * 512]
            mode, length = struct.unpack_from("<HxxI", e, 0)
            cl = struct.unpack_from("<I", e, 0x10)[0]
            name = e[0x40:0x60].split(b"\0")[0].decode("ascii", "replace")
            yield mode, length, cl, name

def main():
    c = Card(sys.argv[1]); out = sys.argv[2] if len(sys.argv) > 2 else None
    root_len = next(c.entries(c.root, 1))[1]
    for mode, length, cl, name in c.entries(c.root, root_len):
        if not mode & 0x8000 or name in (".", ".."): continue
        print("%s/ (%d entries)" % (name, length))
        if not mode & 0x20: continue
        for m2, l2, c2, n2 in c.entries(cl, length):
            if not m2 & 0x8000 or n2 in (".", ".."): continue
            print("   %-32s %7d bytes" % (n2, l2))
            if out and m2 & 0x10:
                os.makedirs(os.path.join(out, name), exist_ok=True)
                open(os.path.join(out, name, n2), "wb").write(c.read(c2, l2))

if __name__ == "__main__":
    main()
