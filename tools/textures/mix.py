#!/usr/bin/env python3
"""Mix two upscales of the same textures, each model where it does best (review of 2026-10-25, docs/textures.md):
4x-PBRify_UpscalerV4 (A, faithful, smooth) on skin and on smooth areas, where 4xHDcube3 invents speckles; 4xHDcube3
(B, crisper) on edges and patterns (lace, embroidery, stone joints, text). The weight is worked out per texel on the
source texture and blended softly, so there is no seam between the two.
usage: mix.py SRC_DIR MANIFEST A_DIR B_DIR OUT_DIR [--mask MASK_DIR]   (--mask also writes the weight, white = A)

Skin is only looked for in the character textures (/O_*/, the models and their close-up heads): wood and plaster in
the stages have skin-like hues. Smooth = little local contrast in the source (the standard deviation of luma in a
5 x 5 texel window). Alpha is taken from B (both upscales use the same alpha model). Outputs are game data: keep them
outside the repository."""
import os, sys
import numpy as np
from PIL import Image, ImageFilter

FLAT_LO, FLAT_HI = 2.5, 7.0     # local luma deviation (0..255): below LO all A, above HI all B
SKIN_LO, SKIN_HI = 6.0, 12.0    # skin tones count as skin only below this local deviation (lace and embroidery are cream)

def smoothstep(e0, e1, x):
    t = np.clip((x - e0) / (e1 - e0), 0, 1); return t * t * (3 - 2 * t)

def box(x, r):   # mean over a (2r+1)^2 window, edges mirrored
    p = np.pad(x, r, mode='reflect'); c = p.cumsum(0).cumsum(1); c = np.pad(c, ((1, 0), (1, 0)))
    k = 2 * r + 1; h, w = x.shape
    return (c[k:k + h, k:k + w] - c[:h, k:k + w] - c[k:k + h, :w] + c[:h, :w]) / (k * k)

def skin(rgb):   # soft skin-tone mask in YCbCr (the usual Cb 77..127, Cr 133..173 box, with soft edges)
    r, g, b = rgb[..., 0], rgb[..., 1], rgb[..., 2]
    y = 0.299 * r + 0.587 * g + 0.114 * b
    cb = 128 - 0.168736 * r - 0.331264 * g + 0.5 * b
    cr = 128 + 0.5 * r - 0.418688 * g - 0.081312 * b
    m = smoothstep(72, 80, cb) * (1 - smoothstep(124, 132, cb)) * smoothstep(130, 138, cr) * (1 - smoothstep(170, 178, cr))
    return m * smoothstep(40, 70, y) * (r > b)

def weight(src_rgba, character):
    rgb = src_rgba[..., :3].astype(np.float32)
    y = 0.299 * rgb[..., 0] + 0.587 * rgb[..., 1] + 0.114 * rgb[..., 2]
    sd = np.sqrt(np.maximum(box(y * y, 2) - box(y, 2) ** 2, 0))
    w = 1 - smoothstep(FLAT_LO, FLAT_HI, sd)
    if character: w = np.maximum(w, skin(rgb) * (1 - smoothstep(SKIN_LO, SKIN_HI, sd)))   # skin is smooth: cream lace is not skin
    return w

def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    mask_dir = sys.argv[sys.argv.index('--mask') + 1] if '--mask' in sys.argv else None
    if mask_dir in args: args.remove(mask_dir)
    src, manifest, adir, bdir, out = args
    files = {}
    for l in open(manifest).read().splitlines()[1:]:
        f = l.split('\t'); files.setdefault(f[0], f[3])
    os.makedirs(out, exist_ok=True)
    if mask_dir: os.makedirs(mask_dir, exist_ok=True)
    n = 0; share = []
    for tid in sorted(files):
        p = os.path.join(src, tid + '.png')
        if not os.path.exists(p): continue
        s = np.asarray(Image.open(p).convert('RGBA'))
        a = np.asarray(Image.open(os.path.join(adir, tid + '.png')).convert('RGBA')).astype(np.float32)
        b = np.asarray(Image.open(os.path.join(bdir, tid + '.png')).convert('RGBA')).astype(np.float32)
        w = weight(s, files[tid].startswith('/O_'))
        wi = Image.fromarray((w * 255 + 0.5).astype(np.uint8)).resize((a.shape[1], a.shape[0]), Image.BILINEAR)
        wi = wi.filter(ImageFilter.GaussianBlur(a.shape[1] // s.shape[1]))   # about a source texel: no hard steps
        W = np.asarray(wi).astype(np.float32)[..., None] / 255
        o = a[..., :3] * W + b[..., :3] * (1 - W)
        Image.fromarray(np.dstack([np.clip(o + 0.5, 0, 255).astype(np.uint8), b[..., 3].astype(np.uint8)]), 'RGBA').save(os.path.join(out, tid + '.png'))
        if mask_dir: wi.save(os.path.join(mask_dir, tid + '.png'))
        share.append(float(W.mean())); n += 1
    print('%d textures mixed; PBRify share: mean %.2f, median %.2f' % (n, np.mean(share), np.median(share)))

if __name__ == '__main__':
    main()
