"""piccmp.py REF NEW: compare two picgate.sh sets (directories with st1/ and cs/ of shown_*.ppm). Gate G10: pictures
of copy 0 (native size, real) identical; upscaled and in-between pictures within 1 level in at most 0.01 % of pixels
(the copies are not bit-reproducible between runs)."""
import os, sys, numpy as np
def ppm(p):
    with open(p, 'rb') as f:
        d = f.read()
    parts = d[:64].split()
    w, h = int(parts[1]), int(parts[2])
    return np.frombuffer(d[-w * h * 3:], np.uint8).reshape(h, w, 3)
bad = n = 0
for sub in ('st1', 'cs'):
    a, b = os.path.join(sys.argv[1], sub), os.path.join(sys.argv[2], sub)
    na, nb = sorted(os.listdir(a)), sorted(os.listdir(b))
    if na != nb: print('%s: different picture sets: only in ref %s, only in new %s' % (sub, sorted(set(na) - set(nb)), sorted(set(nb) - set(na)))); bad += 1
    for f in sorted(set(na) & set(nb)):
        x, y = ppm(os.path.join(a, f)).astype(int), ppm(os.path.join(b, f)).astype(int); n += 1
        if x.shape != y.shape: print('%s/%s: size %s vs %s' % (sub, f, x.shape, y.shape)); bad += 1; continue
        d = np.abs(x - y).max(axis=2); nd = int((d > 0).sum()); mx = int(d.max())
        exact = x.shape[1] <= 1024 and '_i' not in f
        ok = nd == 0 if exact else (mx <= 1 and nd <= d.size * 1e-4)
        if not ok or nd: print('%s/%s %dx%d: %d pixels differ, max %d%s' % (sub, f, x.shape[1], x.shape[0], nd, mx, '' if ok else '  FAIL'))
        bad += not ok
print('%d pictures compared, %d fail' % (n, bad))
