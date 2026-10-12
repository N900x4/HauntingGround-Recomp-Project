#!/usr/bin/env python3
"""Find likely function starts missed by the start list: the first non-nop instruction after an
unconditional transfer (jr ra / j / b) + its delay slot, that no static branch/jump targets
(so it cannot be the continuation of the same function). Appends to extra_starts.txt."""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import r5900 as R
H = os.path.dirname(os.path.abspath(__file__))
blob = R.load(os.path.join(H, '../game/SLUS_210.75')); W = R.words(blob)
END = 0x3a1b80
starts = set(int(x, 16) for x in open(os.path.join(H, '../data/mystarts.txt')).read().split())
extra = set(int(x, 16) for x in open(os.path.join(H, '../data/extra_starts.txt')).read().split()) if os.path.exists(os.path.join(H, '../data/extra_starts.txt')) else set()
def s16(x): return x - 0x10000 if x & 0x8000 else x
targets = set()
import json
jt = json.load(open(os.path.join(H, '../data/jumptables.json')))['tables']
for t in jt: targets.update(t['targets'])
for i, x in enumerate(W[:(END - 0x100000) // 4]):
    m = R.decode(x); pc = 0x100000 + 4 * i
    if m in R.BRANCH: targets.add(pc + 4 + 4 * s16(x & 0xffff))
    elif m in ('j', 'jal'): targets.add((x & 0x3ffffff) << 2)
new = []
for i, x in enumerate(W[:(END - 0x100000) // 4]):
    pc = 0x100000 + 4 * i; m = R.decode(x)
    uncond = (m == 'jr') or (m == 'j') or (m == 'beq' and ((x >> 21) & 31) == 0 and ((x >> 16) & 31) == 0)
    if not uncond: continue
    a = pc + 8
    while a < END and W[(a - 0x100000) // 4] == 0: a += 4
    if a >= END or a in starts or a in extra or a in targets: continue
    new.append(a)
print(len(new), 'gap starts; e.g.', [hex(a) for a in new[:20]])
print('0x1df068 in list:', 0x1df068 in new)
open(os.path.join(H, '../data/extra_starts.txt'), 'w').write('\n'.join(hex(a) for a in sorted(extra | set(new))) + '\n')
