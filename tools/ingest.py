#!/usr/bin/env python3
"""Disc ingestion: open your own PS2 ISO (or a .zip containing one) read-only, walk ISO9660, identify the game
by SYSTEM.CNF + ELF hash, locate DATA.CVM, and optionally copy the two files the build needs into OUTDIR.
usage: ingest.py <disc.iso|disc.zip> [OUTDIR]   (e.g. ../game)"""
import sys, struct, hashlib, zipfile, io, time
KNOWN = {"3b374d53a499d2c17b205274ee9eb34280768f294f970ebf6ae6731f6a2dacb8": "Haunting Ground NTSC-U SLUS-21075 v1.01"}
SECTOR = 2048
def open_src(path):
    if path.lower().endswith('.zip'):
        z = zipfile.ZipFile(path); n = [i for i in z.namelist() if i.lower().endswith('.iso')][0]
        return z.open(n), n            # stream; seekable in py3.7+ for stored/deflated (slow seeks)
    return open(path, 'rb'), path
def rd(f, lba, n): f.seek(lba*SECTOR); return f.read(n)
def walk(f, lba, size, prefix=''):
    data = rd(f, lba, size); off = 0
    while off < len(data):
        l = data[off]
        if l == 0: off = (off//SECTOR + 1)*SECTOR; continue
        rec = data[off:off+l]; elba, esz = struct.unpack_from('<I', rec, 2)[0], struct.unpack_from('<I', rec, 10)[0]
        flags = rec[25]; nl = rec[32]; name = rec[33:33+nl]
        if name not in (b'\x00', b'\x01'):
            nm = name.decode('ascii', 'replace').split(';')[0]
            yield (prefix+nm, elba, esz, bool(flags & 2))
            if flags & 2: yield from walk(f, elba, esz, prefix+nm+'/')
        off += l
def copy_out(f, lba, size, dst):
    f.seek(lba*SECTOR); left = size
    with open(dst, 'wb') as o:
        while left:
            b = f.read(min(left, 1 << 24)); o.write(b); left -= len(b)

def main(path, outdir=None):
    t0 = time.time(); f, inner = open_src(path)
    pvd = rd(f, 16, SECTOR); assert pvd[1:6] == b'CD001', 'not ISO9660'
    vol = pvd[40:72].decode().strip(); vsz = struct.unpack_from('<I', pvd, 80)[0]
    root = pvd[156:156+34]; rlba, rsz = struct.unpack_from('<I', root, 2)[0], struct.unpack_from('<I', root, 10)[0]
    files = list(walk(f, rlba, rsz))
    print(f"source: {inner}\nvolume: {vol}  sectors: {vsz} ({vsz*SECTOR/1e9:.2f} GB)")
    for p, l, s, d in files: print(f"  {'DIR ' if d else '    '}{p:40s} lba {l:7d} size {s:,}")
    ent = {p.upper(): (l, s) for p, l, s, d in files}
    cnf = rd(f, *ent['SYSTEM.CNF']).decode(errors='replace'); print("SYSTEM.CNF:", cnf.replace('\r\n', ' | ').strip())
    boot = cnf.split('BOOT2')[1].split('\\')[1].split(';')[0].strip()
    l, s = ent[boot.upper()]; elf = rd(f, l, s); h = hashlib.sha256(elf).hexdigest()
    print(f"boot ELF {boot}: {s:,} B sha256 {h} -> {KNOWN.get(h, 'UNKNOWN (unsupported version)')}")
    l, s = ent['DATA.CVM']; hdr = rd(f, l, 64)
    print(f"DATA.CVM header magic {hdr[:4]!r}; lba {l}; size {s:,}")
    if outdir:
        if h not in KNOWN: sys.exit('refusing to extract: unsupported or modified disc')
        import os; os.makedirs(outdir, exist_ok=True)
        open(os.path.join(outdir, boot), 'wb').write(elf)
        copy_out(f, *ent['DATA.CVM'], os.path.join(outdir, 'DATA.CVM'))
        print(f"copied {boot} and DATA.CVM to {outdir}")
    print(f"elapsed {time.time()-t0:.2f}s")
if __name__ == '__main__': main(*sys.argv[1:3])
