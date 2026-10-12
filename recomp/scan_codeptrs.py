#!/usr/bin/env python3
"""Find function-pointer constants built in code (lui+addiu/ori) that point into .text but are not in the
start list (e.g. ctor/dtor callbacks passed to MW __construct_array). Prints candidates -> extra_starts.txt"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import r5900 as R
H = os.path.dirname(os.path.abspath(__file__))
blob = R.load(os.path.join(H, '../game/SLUS_210.75')); W = R.words(blob)
starts = set(int(x, 16) for x in open(os.path.join(H, '../data/mystarts.txt')).read().split())
def s16(x): return x - 0x10000 if x & 0x8000 else x
lui = {}; found = {}
for i, w in enumerate(W[:(0x469460 - 0x100000) // 4]):
    a = 0x100000 + 4 * i
    if 0x3a1b80 <= a < 0x464480: continue
    if a in starts: lui = {}
    m = R.decode(w); rs = (w >> 21) & 31; rt = (w >> 16) & 31
    if m == 'lui': lui[rt] = (w & 0xffff) << 16; continue
    if m in ('addiu', 'ori') and rs in lui:
        v = (lui[rs] + s16(w & 0xffff)) & 0xffffffff if m == 'addiu' else lui[rs] | (w & 0xffff)
        if 0x100000 <= v < 0x3a1b80 and v % 4 == 0 and v not in starts:
            found.setdefault(v, []).append(a)
    if m in ('jr', 'j', 'jal', 'jalr'): pass
def wd(a): return W[(a - 0x100000) // 4]
good = []
for v, refs in sorted(found.items()):
    prev = wd(v - 8)
    looks = prev == 0x03e00008 or R.decode(wd(v - 8)) == 'j' or wd(v - 4) == 0 and wd(v - 8) == 0
    good.append((v, looks, refs))
print(len(found), 'candidates;', sum(1 for g in good if g[1]), 'preceded by jr ra/j/nop-padding')
for v, l, refs in good[:30]: print(hex(v), l, [hex(r) for r in refs[:3]])
open(os.path.join(H, '../data/extra_starts.txt'), 'w').write('\n'.join(hex(v) for v, l, r in good if l) + '\n')
