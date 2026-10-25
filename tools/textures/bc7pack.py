#!/usr/bin/env python3
"""HD replacements as BC7 (recomp/hdtex.c reads <id>.dds before <id>.png): a quarter of the video memory of RGBA, no
PNG decoding in the game, and the release's texture pack under GitHub's 2 GB per file (2.4 GB of PNGs, about 1.6 GB
of BC7 zipped). Each PNG's mip chain is made as hdtex.c makes it (each level a 2x2 box of the one before, rounded,
down to 1 x 1), each level encoded by bc7enc (richgel999/bc7enc_rdo built with BC7E, quality 6, perceptual metrics;
levels padded to whole 4 x 4 blocks by repeating the edge), and the levels written as one DDS (DX10 header,
DXGI_FORMAT_BC7_UNORM). Output is game data: keep it outside the repository.
usage: bc7pack.py IN_DIR OUT_DIR --enc PATH_TO_bc7enc [--jobs 2] [--only ID ...]
       (skips ids whose .dds is newer than the .png; prints the PSNR range of level 0)"""
import os, re, sys, struct, shutil, subprocess, tempfile
from concurrent.futures import ThreadPoolExecutor
import numpy as np
from PIL import Image

def mips(a):   # hdtex.c's mips(): floor halves, the last row/column repeated for odd sizes, (a+b+c+d+2)>>2
    out = [a]
    while a.shape[0] > 1 or a.shape[1] > 1:
        h, w = a.shape[:2]; nh, nw = max(h // 2, 1), max(w // 2, 1)
        y0 = np.arange(nh) * 2; y1 = np.minimum(y0 + 1, h - 1); x0 = np.arange(nw) * 2; x1 = np.minimum(x0 + 1, w - 1)
        s = a.astype(np.uint16)
        a = ((s[y0][:, x0] + s[y0][:, x1] + s[y1][:, x0] + s[y1][:, x1] + 2) >> 2).astype(np.uint8)
        out.append(a)
    return out

def encode_level(enc, a, tmp, tag, psnr):   # BC7 blocks of one level (padded to whole blocks); psnr: measure it (slower)
    h, w = a.shape[:2]; ph, pw = (h + 3) // 4 * 4, (w + 3) // 4 * 4
    if (ph, pw) != (h, w): a = np.pad(a, ((0, ph - h), (0, pw - w), (0, 0)), mode='edge')
    src = os.path.join(tmp, tag + '.png'); Image.fromarray(a, 'RGBA').save(src, compress_level=1)
    r = subprocess.run([enc, src, '-s'] + ([] if psnr else ['-q', '-g']), cwd=tmp, capture_output=True, text=True)
    dds = os.path.join(tmp, tag + '.dds')
    if r.returncode or not os.path.exists(dds): raise RuntimeError('bc7enc failed on %s: %s' % (tag, r.stdout[-300:] + r.stderr[-300:]))
    b = open(dds, 'rb').read(); os.remove(dds); os.remove(src)
    for f in os.listdir(tmp):
        if f.startswith(tag + '_unpacked'): os.remove(os.path.join(tmp, f))
    if b[:4] != b'DDS ' or b[84:88] != b'DX10' or struct.unpack_from('<I', b, 128)[0] not in (98, 99): raise RuntimeError('unexpected DDS from bc7enc for ' + tag)
    n = (pw // 4) * (ph // 4) * 16
    return b[148:148 + n], [float(p) for p in re.findall(r'PSNR ([\d.]+)', r.stdout)]

def dds_header(w, h, levels):
    hdr = bytearray(148); hdr[0:4] = b'DDS '
    struct.pack_into('<IIIIIII', hdr, 4, 124, 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | 0x80000, h, w, ((w + 3) // 4) * 16, 1, levels)
    struct.pack_into('<II4s', hdr, 76, 32, 0x4, b'DX10')                 # pixel format: FOURCC DX10
    struct.pack_into('<I', hdr, 108, 0x1000 | 0x8 | 0x400000)            # caps: TEXTURE | COMPLEX | MIPMAP
    struct.pack_into('<IIIII', hdr, 128, 98, 3, 0, 1, 0)                 # BC7_UNORM, TEXTURE2D, array size 1
    return bytes(hdr)

def one(enc, src, dst):
    a = np.asarray(Image.open(src).convert('RGBA'))
    tmp = tempfile.mkdtemp(prefix='bc7_', dir=os.path.dirname(dst))
    try:
        data = []; ps = []
        for k, lv in enumerate(mips(a)):
            blk, p = encode_level(enc, lv, tmp, 'l%02d' % k, k == 0); data.append(blk)
            if k == 0: ps = p
        part = dst + '.part'
        with open(part, 'wb') as f: f.write(dds_header(a.shape[1], a.shape[0], len(data))); [f.write(d) for d in data]
        os.replace(part, dst)
    finally: shutil.rmtree(tmp, ignore_errors=True)
    return ps

def main():
    opt = lambda k, d=None: sys.argv[sys.argv.index(k) + 1] if k in sys.argv else d
    ind, outd = sys.argv[1], sys.argv[2]; enc = opt('--enc'); jobs = int(opt('--jobs', 2))
    only = set(sys.argv[sys.argv.index('--only') + 1:]) if '--only' in sys.argv else None
    if not enc: sys.exit('--enc PATH_TO_bc7enc is needed')
    os.makedirs(outd, exist_ok=True)
    todo = []
    for f in sorted(os.listdir(ind)):
        m = re.fullmatch(r'([0-9a-f]{8}_[0-9a-f]{8}(?:_c\d+)?)\.png', f)
        if not m or (only and m.group(1) not in only): continue
        src, dst = os.path.join(ind, f), os.path.join(outd, m.group(1) + '.dds')
        if os.path.exists(dst) and os.path.getmtime(dst) >= os.path.getmtime(src): continue
        todo.append((src, dst))
    print('%d textures to encode' % len(todo), flush=True)
    lo = []; done = 0
    with ThreadPoolExecutor(jobs) as ex:
        for ps in ex.map(lambda t: one(enc, *t), todo):
            done += 1; lo += ps[:3]
            if done % 50 == 0: print('%d / %d' % (done, len(todo)), flush=True)
    if lo: print('level 0 PSNR (R, G, B): min %.1f dB, median %.1f dB' % (min(lo), sorted(lo)[len(lo) // 2]))

if __name__ == '__main__':
    main()
