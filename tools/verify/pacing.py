"""pacing.py LOG [FROM_VB]: present intervals and the sequence of pictures shown (from HG_PRESENTLOG lines), and the
speed from HG_SPEEDLOG lines (VBlanks from FROM_VB on, default 3000: the Castle Suite in wintest.sh).
Gate G7: median 16.68 ms (8.34 at 120 fps), p5-p95 within 16.5-16.9, no picture twice in a row, none out of order."""
import re, sys
s = open(sys.argv[1]).read()
P = [(float(a), int(b), int(c), bool(d)) for a, b, c, d in re.findall(r'PRESENT ([\d.]+) ms vb (\d+)\.(\d)( i)?', s)]
if len(P) < 3: print('no presents'); sys.exit()
d = sorted(b[0] - a[0] for a, b in zip(P, P[1:]))
print(len(P), 'intervals ms: median %.2f p5 %.2f p95 %.2f max %.2f' % (d[len(d) // 2], d[len(d) // 20], d[len(d) * 19 // 20], d[-1]))
key = lambda p: (p[1], p[2])
rep = sum(1 for a, b in zip(P, P[1:]) if key(a) == key(b))
back = sum(1 for a, b in zip(P, P[1:]) if key(b) < key(a))
print('same picture twice in a row: %d, out of order: %d, in-between share %.2f' % (rep, back, sum(p[3] for p in P) / len(P)))
print(' '.join('%d.%d%s' % (p[1] % 1000, p[2], 'i' if p[3] else '') for p in P[300:340]))
fr = int(sys.argv[2]) if len(sys.argv) > 2 else 3000
sp = [(int(v), float(x)) for v, x in re.findall(r'SPEED vb (\d+):.*?([\d.]+)x real', s)]
sp = [x for v, x in sp if v >= fr]
if sp: print('speed from VB %d: %s' % (fr, ' '.join('%.2f' % x for x in sp)))
