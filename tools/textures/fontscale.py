#!/usr/bin/env python3
"""HD replacements for the game's fonts (4-bit textures whose index is a glyph's coverage, 0 empty to 15 full, and
whose sub-palettes colour it: MSG_BASE.TEX holds the Latin font in 10 colours). The AI models invent detail on 8-pixel
glyphs, so fonts are scaled in index space instead. The glyph's outline (the texels whose index is not 0) is
upscaled smoothly and its edges made steep again, so the strokes keep their width and come out crisp; the index
inside it (its empty neighbours filled from it first, so edges do not darken) is upscaled and smoothed, which keeps
the average level of the original's noisy texels and so the game's colour; each sub-palette
is then looked up at the fractional index, which is what the game's own palettes do, at a higher resolution. Writes <id>.png (sub-palette 0) and
<id>_c<N>.png for every other sub-palette that is used, as hdtex.c expects.
The outline is smoothest when an upscaling model draws it from the glyphs' masks (a binary pixel font thresholded
after a plain upscale keeps its pixel steps): --masks DIR writes <id>.png masks (white glyphs on black) for
upscale.py, and --shape DIR then takes the outline from those upscales.
usage: fontscale.py OUT_DIR [--scale 4] [--masks DIR | --shape DIR] [FILE ...]   (default files: the fonts below;
output is game data: keep it outside the repository)"""
import os, sys
import numpy as np
from PIL import Image
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'verify')); sys.path.insert(0, os.path.join(HERE, '..', 'formats')); sys.path.insert(0, HERE)
import zlib, cvm, tex_hg, texdump

FONTS = ['/SUBSCR/MSG_BASE.TEX', '/SUBSCR/MSG_SUB.TEX', '/SUBSCR/MSG_END.TEX']

def gblur(a, sigma):   # separable Gaussian, edges mirrored
    r = max(1, int(3 * sigma)); k = np.exp(-0.5 * (np.arange(-r, r + 1) / sigma) ** 2); k /= k.sum()
    p = np.pad(a, r, mode='reflect')
    p = sum(k[i] * p[:, i:i + a.shape[1]] for i in range(2 * r + 1))
    return sum(k[i] * p[i:i + a.shape[0], :] for i in range(2 * r + 1))

def steep(c, lo=0.30, hi=0.70):   # the coverage's edges, steep again after the smooth upscale
    t = np.clip((c - lo) / (hi - lo), 0, 1)
    return t * t * (3 - 2 * t)

def main():
    opt = lambda k: sys.argv[sys.argv.index(k) + 1] if k in sys.argv else None
    skip = {opt(k) for k in ('--scale', '--masks', '--shape') if opt(k)}
    args = [a for a in sys.argv[1:] if not a.startswith('--') and a not in skip]
    s = int(opt('--scale') or 4); masks, shapes = opt('--masks'), opt('--shape')
    if masks: os.makedirs(masks, exist_ok=True)
    out = args[0]; files = args[1:] or FONTS
    os.makedirs(out, exist_ok=True); n = 0
    for name in files:
        d = cvm.read(name)
        for bo, ents in texdump.blobs(name, d):
            for t in ents:
                if t['psm'] != 0x14: continue
                w, h = t['w'], t['h']
                img = d[t['off']:t['off'] + t['iq'] * 16]; clut = d[t['off'] + t['iq'] * 16:t['off'] + t['iq'] * 16 + t['cq'] * 16]
                tid = '%08x_%08x' % (zlib.crc32(img), zlib.crc32(clut))
                b = np.frombuffer(img, np.uint8, w * h // 2); idx = np.empty(w * h, np.uint8); idx[0::2] = b & 15; idx[1::2] = b >> 4
                ix = idx.reshape(h, w).astype(np.float32)
                if masks:
                    Image.fromarray(np.repeat(((ix > 0) * 255).astype(np.uint8)[..., None], 3, 2), 'RGB').save(os.path.join(masks, tid + '.png')); n += 1; continue
                if shapes:   # the model's upscale of the mask (any size: resized to this one)
                    occ = np.asarray(Image.open(os.path.join(shapes, tid + '.png')).convert('L').resize((w * s, h * s), Image.LANCZOS)).astype(np.float32) / 255
                else:
                    occ = np.asarray(Image.fromarray((ix > 0).astype(np.float32), 'F').resize((w * s, h * s), Image.BICUBIC))
                shape = steep(np.clip(occ, 0, 1), 0.35, 0.65)   # (low: the next glyph's spread in the atlas)
                fill = ix.copy()   # empty texels take the mean of their non-empty neighbours (a few rings)
                for _ in range(3):
                    z = fill == 0
                    if not z.any(): break
                    p_ = np.pad(fill, 1); n_ = np.pad((fill > 0).astype(np.float32), 1)
                    sm = sum(p_[1 + dy:1 + dy + h, 1 + dx:1 + dx + w] for dy in (-1, 0, 1) for dx in (-1, 0, 1))
                    cn = sum(n_[1 + dy:1 + dy + h, 1 + dx:1 + dx + w] for dy in (-1, 0, 1) for dx in (-1, 0, 1))
                    fill = np.where(z & (cn > 0), sm / np.maximum(cn, 1), fill)
                val = np.asarray(Image.fromarray(fill, 'F').resize((w * s, h * s), Image.BICUBIC))
                c = np.clip(gblur(val, s * 0.75), 1, 15)   # fractional index inside the glyph: the noisy texels' average
                i0 = np.floor(c).astype(np.int32); i1 = np.minimum(i0 + 1, 15); f = (c - i0)[..., None]
                pal = tex_hg.unswizzle_csm1(np.frombuffer(clut, np.uint8).reshape(-1, 4)).astype(np.float32).copy()
                pal[:, 3] = tex_hg.a80(pal[:, 3].astype(np.uint8))
                for k in range(16):
                    p = pal[k * 16:k * 16 + 16]
                    if not p.any() or (k and np.array_equal(p, pal[0:16])): continue
                    rgba = p[i0] * (1 - f) + p[i1] * f
                    rgba[..., 3] *= shape
                    Image.fromarray(np.clip(rgba + 0.5, 0, 255).astype(np.uint8), 'RGBA').save(os.path.join(out, tid + ('_c%d' % k if k else '') + '.png'))
                    n += 1
    print('%d %s in %s' % (n, 'masks' if masks else 'font images', masks or out))

if __name__ == '__main__':
    main()
