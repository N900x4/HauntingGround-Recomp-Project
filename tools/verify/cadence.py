"""cadence.py LOG [REFRESH_HZ]: how long each picture stayed on the screen, from the "SHOWN" lines of HG_PRESENTLOG
(the time each present reached the screen, VK_KHR_present_wait; host_sdl.c, "pacing").

- refresh period: the median interval between presents that reached the screen one after the other in the
  display-locked mode (every present is one refresh), or REFRESH_HZ if given;
- hold of a picture: from the time it first reached the screen to the time the next picture did, in refreshes
  (rounded); the histogram should be a single bar: 2 on a 120 Hz screen at 60 fps, 1 on 60 Hz;
- order: pictures (VBlank.sub) going backwards or skipped VBlanks (a picture left out) are counted;
- lat: ms from the start of the picture's VBlank on the game thread to the screen (median, p95)."""
import re, sys
from collections import Counter
s = open(sys.argv[1]).read()
what = 'SCAN' if re.search(r'^SCAN ', s, re.M) and '--shown' not in sys.argv else 'SHOWN'   # scanout times (VK_EXT_present_timing) when there are any
sys.argv = [a for a in sys.argv if a != '--shown']
S = [(float(t), int(v), int(sub), bool(i), k, float(l) if l else None)
     for t, v, sub, i, k, l in re.findall(what + r' ([\d.]+) ms vb (\d+)\.(\d)( i)? (new|rep)(?: lat ([\d.]+))?', s)]
S.sort()
print('from the %s lines' % what)
if len(S) < 10: print('no SHOWN lines'); sys.exit()
iv = sorted(b[0] - a[0] for a, b in zip(S, S[1:]))
if len(sys.argv) > 2: rp = 1000.0 / float(sys.argv[2])
else:   # the time over the refreshes counted (each interval a whole number of refreshes)
    g = iv[len(iv) // 2]
    for _ in range(3):
        k = sum(max(1, round(x / g)) for x in iv); g = sum(iv) / k
    rp = g
print('%d presents shown, %.1f s; intervals ms: median %.3f p1 %.3f p99 %.3f max %.3f; refresh period %.3f ms (%.3f Hz)' % (
    len(S), (S[-1][0] - S[0][0]) / 1000, iv[len(iv) // 2], iv[len(iv) // 100], iv[len(iv) * 99 // 100], iv[-1], rp, 1000 / rp))
# pictures: the first time each new picture was shown
P = [(t, v, sub, i, l) for t, v, sub, i, k, l in S if k == 'new']
holds = [round((b[0] - a[0]) / rp) for a, b in zip(P, P[1:])]
h = Counter(holds)
print('pictures %d; hold in refreshes: %s' % (len(P), ', '.join('%d: %d (%.2f %%)' % (k, h[k], 100.0 * h[k] / len(holds)) for k in sorted(h))))
back = sum(1 for a, b in zip(P, P[1:]) if (b[1], b[2]) < (a[1], a[2]))
same = sum(1 for a, b in zip(P, P[1:]) if (b[1], b[2]) == (a[1], a[2]))
gaps = Counter(b[1] - a[1] for a, b in zip(P, P[1:]) if b[1] > a[1])
print('out of order %d, same picture again %d; VBlank steps between pictures: %s; in-between share %.2f' % (
    back, same, dict(sorted(gaps.items())), sum(p[3] for p in P) / len(P)))
L = sorted(p[4] for p in P if p[4] is not None)
if L: print('lat ms (VBlank start on the game thread -> screen): median %.1f p5 %.1f p95 %.1f' % (L[len(L) // 2], L[len(L) // 20], L[len(L) * 19 // 20]))
sp = [(int(v), float(x)) for v, x in re.findall(r'SPEED vb (\d+):.*?([\d.]+)x real', s)]
if sp: print('speed: ' + ' '.join('%d:%.3f' % (v, x) for v, x in sp[-8:]))
for line in re.findall(r'(host: display.*|.*no picture ready.*|host: two pictures.*)', s)[:12]: print('  ' + line.strip())
