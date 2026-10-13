#!/usr/bin/env python3
"""Check SPU2 key-ons from a run (HG_KONLOG) against a BGM sequence and its bank, both read from DATA.CVM.
For each sequence note-on: the matching key-on time, the bank sample it started (Vagi index) and its transposition
in semitones from the sample's native rate.
usage: bgm_check.py KONLOG SEQ.SQ BANK.HD BANK_SPU_HALFWORD_BASE [core [start_seconds]]
   e.g. bgm_check.py kon.log /BGM/S1_NORMALA.SQ /BGM/STAGE1_BANK.HD 0x10000
"""
import os, sys, struct, math
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'formats'))
import sq, hdbd
CVM = os.path.join(os.environ.get('HG_GAME_DIR', '../game'), 'DATA.CVM')
f = open(CVM, 'rb')
def sec(n, c=1): f.seek((n + 3) * 2048); return f.read(2048 * c)
found = {}
def walk(lsn, size, path):
    d = sec(lsn, (size + 2047) // 2048); i = 0
    while i < len(d):
        l = d[i]
        if l == 0: i = (i // 2048 + 1) * 2048; continue
        ext, sz = struct.unpack_from('<I', d, i + 2)[0], struct.unpack_from('<I', d, i + 10)[0]
        fl, nl = d[i + 25], d[i + 32]; name = d[i + 33:i + 33 + nl]
        if name not in (b'\0', b'\1'):
            p = path + '/' + name.decode('latin1').split(';')[0]
            if fl & 2: walk(ext, sz, p)
            else: found[p] = (ext, sz)
        i += l
root = sec(16)[156:190]; walk(struct.unpack_from('<I', root, 2)[0], struct.unpack_from('<I', root, 10)[0], '')
def read(p): ext, sz = found[p]; return sec(ext, (sz + 2047) // 2048)[:sz]

kon_path, sq_path, hd_path, base = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4], 0)
core = int(sys.argv[5]) if len(sys.argv) > 5 else 0
vagi, _ = hdbd.parse_hd(read(hd_path))
by_addr = {base + off // 2: (i, rate) for i, (off, rate, _, _) in enumerate(vagi)}
_, ev = sq.parse(read(sq_path))
notes = [(e[1], e[2] & 15, e[3], e[4]) for e in ev if isinstance(e[2], int) and e[2] & 0xF0 == 0x90 and e[4] > 0]
kons = []
for line in open(kon_path):
    p = line.split()
    if int(p[1]) != core: continue
    a = int(p[3][4:], 16)
    if a in by_addr: kons.append((int(p[0]) / 48000, a, int(p[4][6:], 16)))
if not kons: sys.exit('no key-ons from this bank')
# sequence start: the offset that lines up the most notes with key-ons (within 10 ms)
cands = [k[0] - notes[0][0] for k in kons]
t0 = float(sys.argv[6]) if len(sys.argv) > 6 else max(cands, key=lambda c: sum(any(abs(k[0] - (n[0] + c)) < 0.01 for k in kons) for n in notes))
worst, matched = 0.0, 0
for t, ch, key, vel in notes:
    if t + t0 > kons[-1][0] + 0.05: break
    m = min(kons, key=lambda k: abs(k[0] - (t + t0)))
    dt = m[0] - (t + t0); i, rate = by_addr[m[1]]
    semis = 12 * math.log2(m[2] / (rate * 4096 / 48000))
    ok = abs(dt) < 0.01
    matched += ok; worst = max(worst, abs(dt)) if ok else worst
    print('%8.3f ch%-2d key %3d vel %3d  key-on %+.4f s  sample %2d  %+7.3f semitones%s' % (t + t0, ch, key, vel, dt, i, semis, '' if ok else '  NO MATCH'))
print('matched %d notes, worst timing error %.4f s' % (matched, worst))
