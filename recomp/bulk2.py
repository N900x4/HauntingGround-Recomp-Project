#!/usr/bin/env python3
"""bootgate bulk translator: every .text function (data/mystarts.txt) + the 143 static ctors
(0x464480-0x469460, from the ctor table at 0x469460) -> NPARTS C files + a dispatch table.
usage: bulk2.py OUTDIR [NPARTS]   (run from this directory)"""
import sys, os, json, time, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rec2 as rec, r5900 as R
HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, '..', 'data')
EXE = os.path.join(HERE, '..', 'game', 'SLUS_210.75')
out = sys.argv[1]; nparts = int(sys.argv[2]) if len(sys.argv) > 2 else 16
os.makedirs(out, exist_ok=True)
blob = R.load(EXE); W = R.words(blob)
text = sorted(int(x, 16) for x in open(os.path.join(DATA, 'mystarts.txt')).read().split())
extra = [int(x, 16) for x in open(os.path.join(DATA, 'extra_starts.txt')).read().split()] if os.path.exists(os.path.join(DATA, 'extra_starts.txt')) else []
text = sorted(set(text) | set(e for e in extra if e < 0x3a1b80))
ctors = sorted(set(W[(a - 0x100000) // 4] for a in range(0x469460, 0x46969c, 4)))
bounds = [(s, text[i + 1] if i + 1 < len(text) else 0x3a1b80) for i, s in enumerate(text)]
bounds += [(s, ctors[i + 1] if i + 1 < len(ctors) else 0x469460) for i, s in enumerate(ctors)]
starts = [s for s, _ in bounds]
jt = {t['jr']: t['targets'] for t in json.load(open(os.path.join(DATA, 'jumptables.json')))['tables']}
u = rec.Unit(blob, None, jt, set(starts))
t0 = time.time()
hdr = ['#include "ee_runtime.h"'] + [f'void {rec.fname(s)}(Ctx *ctx);' for s in starts]
open(os.path.join(out, 'protos.h'), 'w').write('\n'.join(hdr) + '\n')
per = (len(bounds) + nparts - 1) // nparts
ends = [0] * len(bounds)
for p in range(nparts):
    parts = ['#include "protos.h"']
    for i in range(p * per, min(len(bounds), (p + 1) * per)):
        s, e = bounds[i]; parts.append(u.func(s, e, i)); ends[i] = u.last_end
    open(os.path.join(out, f'part{p:02d}.c'), 'w').write('\n'.join(parts) + '\n')
tab = ['#include "protos.h"', f'const int rec_nfuncs = {len(bounds)};', 'const RecEntry rec_table[] = {']
tab += [f'  {{0x{s:x}u, {rec.fname(s)}}},' for s in starts] + ['  {0, 0}', '};']
tab += ['const uint32_t rec_func_end[] = {'] + [f'  0x{e:x}u,' for e in ends] + ['};']
open(os.path.join(out, 'table.c'), 'w').write('\n'.join(tab) + '\n')
print(f'{len(bounds)} functions ({len(ctors)} ctors), {sum(e - s for s, e in bounds) // 4} insns, {time.time() - t0:.1f}s; '
      f'unimplemented sites {len(u.unimpl)}: {collections.Counter(m for a, m in u.unimpl).most_common(12)}')
