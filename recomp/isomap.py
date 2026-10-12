#!/usr/bin/env python3
"""List ISO9660 extents inside game/DATA.CVM (after its 3-sector CVM header) and map CD reads in a run log
(IOP: sceCdRead(lsn ...)) to file names. usage: isomap.py RUNLOG"""
import struct, sys, re, collections
ISO = '../game/DATA.CVM'; HDR = 3; VLSN = 0x1000   # HDR = CVM header sectors, skipped in sec()
f = open(ISO, 'rb')
def sec(n, c=1): f.seek((n + HDR) * 2048); return f.read(2048 * c)
pvd = sec(16); root = pvd[156:156 + 34]
files = []
def walk(lsn, size, path):
    d = sec(lsn, (size + 2047) // 2048); i = 0
    while i < len(d):
        l = d[i]
        if l == 0: i = (i // 2048 + 1) * 2048; continue
        ext, sz = struct.unpack_from('<I', d, i + 2)[0], struct.unpack_from('<I', d, i + 10)[0]
        fl = d[i + 25]; nl = d[i + 32]; name = d[i + 33:i + 33 + nl]
        if name not in (b'\0', b'\1'):
            p = path + '/' + name.decode('latin1').split(';')[0]
            if fl & 2: walk(ext, sz, p)
            else: files.append((ext, sz, p))
        i += l
walk(struct.unpack_from('<I', root, 2)[0], struct.unpack_from('<I', root, 10)[0], '')
files.sort()
import bisect
starts = [x[0] for x in files]
def name_of(isolsn):
    k = bisect.bisect_right(starts, isolsn) - 1
    if k >= 0 and isolsn < files[k][0] + (files[k][1] + 2047) // 2048: return files[k][2]
    return '(dir/meta)'
print(len(files), 'files in DATA.iso')
seen = collections.OrderedDict()
for l in open(sys.argv[1], errors='replace'):
    m = re.search(r'sceCdRead\(lsn 0x([0-9a-f]+), (\d+) sectors', l)
    if m:
        n = name_of(int(m.group(1), 16) - VLSN - HDR); seen[n] = seen.get(n, 0) + int(m.group(2))
for n, c in seen.items(): print(f'  {c:6d} sectors  {n}')
