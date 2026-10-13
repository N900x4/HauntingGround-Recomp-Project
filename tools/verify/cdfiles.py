"""usage: cdfiles.py run.log -> the DATA.CVM files read, in order (consecutive reads of one file merged), with the VBlank seen last"""
import os, sys, re, bisect
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cvm
ext = sorted(((e + 3) * 2048, e * 2048 + 3 * 2048 + s, p) for p, (e, s) in cvm.found.items())
starts = [x[0] for x in ext]
vb = 0; last = None
for line in open(sys.argv[1], errors='replace'):
    m = re.search(r'VBlank (\d+)', line)
    if m: vb = int(m.group(1))
    m = re.search(r'CVM offset 0x([0-9a-f]+)', line)
    if not m: continue
    off = int(m.group(1), 16); i = bisect.bisect_right(starts, off) - 1
    name = ext[i][2] if i >= 0 and off < ext[i][1] else '?%x' % off
    if name != last: print('VB~%6d  %s' % (vb, name)); last = name
