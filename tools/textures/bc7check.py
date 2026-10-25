#!/usr/bin/env python3
"""How close each BC7 .dds (bc7pack.py) is to its PNG: level 0 decoded by a small helper built from bc7enc_rdo's
bc7decomp.cpp (raw RGBA on stdout: u32 width, u32 height, pixels), then colour PSNR over the pixels that are not
transparent (alpha > 8), alpha PSNR, and the 99.9th percentile of the largest channel error. Sorted worst first.
usage: bc7check.py PNG_DIR DDS_DIR --dec PATH_TO_bc7dec [--jobs 4] [--csv OUT.csv]"""
import os, sys, struct, subprocess
from concurrent.futures import ProcessPoolExecutor
import numpy as np
from PIL import Image

def psnr(mse): return 99.0 if mse <= 1e-9 else 10 * np.log10(255.0 ** 2 / mse)

def one(args):
    dec, png, dds = args
    r = subprocess.run([dec, dds], capture_output=True)
    if r.returncode: return (os.path.basename(dds), None)
    w, h = struct.unpack_from('<II', r.stdout, 0)
    b = np.frombuffer(r.stdout, np.uint8, w * h * 4, 8).reshape(h, w, 4).astype(np.int32)
    a = np.asarray(Image.open(png).convert('RGBA')).astype(np.int32)
    if a.shape != b.shape: return (os.path.basename(dds), None)
    d = np.abs(a - b); vis = a[..., 3] > 8
    rgb = d[..., :3][vis] if vis.any() else np.zeros((1, 3))
    return (os.path.basename(dds), (psnr(float((rgb.astype(np.float64) ** 2).mean())), psnr(float((d[..., 3].astype(np.float64) ** 2).mean())),
                                     float(np.percentile(d.max(axis=2), 99.9)), int(w), int(h)))

def main():
    opt = lambda k, v=None: sys.argv[sys.argv.index(k) + 1] if k in sys.argv else v
    pd, dd, dec = sys.argv[1], sys.argv[2], opt('--dec'); jobs = int(opt('--jobs', 4)); csv = opt('--csv')
    todo = [(dec, os.path.join(pd, f[:-4] + '.png'), os.path.join(dd, f)) for f in sorted(os.listdir(dd))
            if f.endswith('.dds') and os.path.exists(os.path.join(pd, f[:-4] + '.png'))]
    with ProcessPoolExecutor(jobs) as ex: res = list(ex.map(one, todo, chunksize=8))
    bad = [n for n, r in res if r is None]; ok = sorted([(r, n) for n, r in res if r is not None])
    if csv:
        with open(csv, 'w') as f:
            f.write('file,rgb_psnr,alpha_psnr,p999_err,w,h\n')
            for r, n in ok: f.write('%s,%.2f,%.2f,%.0f,%d,%d\n' % (n, *r))
    print('%d checked, %d unreadable' % (len(ok), len(bad)))
    for r, n in ok[:15]: print('  %-30s colour %.1f dB  alpha %.1f dB  p99.9 err %3.0f  %dx%d' % (n, *r))
    for lim in (35, 38, 40): print('colour PSNR below %d dB: %d' % (lim, sum(1 for r, n in ok if r[0] < lim)))

if __name__ == '__main__':
    main()
