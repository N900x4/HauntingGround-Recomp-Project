#!/usr/bin/env python3
"""Upscale extracted textures (texdump.py's source/<id>.png) with a spandrel model, as chaiNNer does, on the GPU.
Run with chaiNNer's bundled Python (it has torch + spandrel), e.g.
  ~/Applications/chaiNNer-0.26.0/chaiNNer-linux-x64/python/python/bin/python3.11 upscale.py MODEL.pth SRC_DIR OUT_DIR [IDS.txt] [--alpha ALPHA.pth]

Colour: transparent texels first take the colour of the nearest opaque one (no dark fringes at cut-out edges), the
page is mirror-padded by 16 texels (no edge smear), upscaled whole (no tiles, no seams) and cropped. Alpha: upscaled
separately (with --alpha, an alpha model such as 4xHDcube-Alpha, else Lanczos). The output is RGBA PNG; alpha is
written as decoded (255 = opaque; the runtime scales it back to the GS's 0x80). Outputs are game data: keep them
outside the repository."""
import os, sys, time
import numpy as np, torch, spandrel
from PIL import Image

PAD = 16

def load_model(path):
    m = spandrel.ModelLoader().load_from_file(path).cuda().eval()
    dt = torch.float16 if m.supports_half else torch.float32   # (spandrel's DAT keeps a float32 attention mask: no bfloat16)
    return m.to(dt), dt

@torch.inference_mode()
def run(m, dt, rgb):   # HxWx3 float 0..1 -> (H*s)x(W*s)x3
    x = np.pad(rgb, ((PAD, PAD), (PAD, PAD), (0, 0)), mode='reflect')
    t = torch.from_numpy(x).permute(2, 0, 1)[None].to('cuda', dt)
    y = m(t).float().clamp(0, 1)[0].permute(1, 2, 0).cpu().numpy()
    s = m.scale
    return y[PAD * s:y.shape[0] - PAD * s, PAD * s:y.shape[1] - PAD * s]

def fill_transparent(rgba):
    """colour of the nearest opaque texel for fully transparent ones (a jump flood on the grid)"""
    a = rgba[..., 3]; rgb = rgba[..., :3].astype(np.float32)
    if a.min() > 0: return rgb
    h, w = a.shape
    src = np.where(a[..., None] > 0, np.stack(np.mgrid[0:h, 0:w], -1), -1)   # nearest opaque texel's coordinates
    k = 1 << int(np.ceil(np.log2(max(h, w))))
    while k >= 1:
        for dy in (-k, 0, k):
            for dx in (-k, 0, k):
                sh = np.roll(np.roll(src, dy, 0), dx, 1)
                ok = sh[..., 0] >= 0
                cur = src[..., 0] >= 0
                yy, xx = np.mgrid[0:h, 0:w]
                dn = (sh[..., 0] - yy) ** 2 + (sh[..., 1] - xx) ** 2
                dc = np.where(cur, (src[..., 0] - yy) ** 2 + (src[..., 1] - xx) ** 2, 1 << 30)
                take = ok & (dn < dc)
                src[take] = sh[take]
        k //= 2
    if (src[..., 0] < 0).any(): return rgb
    return rgb[src[..., 0], src[..., 1]]

def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    alpha_model = sys.argv[sys.argv.index('--alpha') + 1] if '--alpha' in sys.argv else None
    if alpha_model in args: args.remove(alpha_model)
    model, src, out = args[0], args[1], args[2]
    ids = [l.split('\t')[0].strip() for l in open(args[3])] if len(args) > 3 else [f[:-4] for f in sorted(os.listdir(src)) if f.endswith('.png')]
    os.makedirs(out, exist_ok=True)
    m, dt = load_model(model); am = load_model(alpha_model) if alpha_model else None
    t0 = time.time(); n = 0
    for tid in ids:
        im = np.asarray(Image.open(os.path.join(src, tid + '.png')).convert('RGBA'))
        rgb = fill_transparent(im) / 255.0
        y = run(m, dt, rgb)
        a = im[..., 3]
        if a.min() == 255:
            A = np.full(y.shape[:2], 255, np.uint8)
        elif am:
            ay = run(am[0], am[1], np.repeat(a[..., None] / 255.0, 3, 2).astype(np.float32))
            A = np.clip(ay.mean(2) * 255 + 0.5, 0, 255).astype(np.uint8)
        else:
            A = np.asarray(Image.fromarray(a).resize((y.shape[1], y.shape[0]), Image.LANCZOS))
        Image.fromarray(np.dstack([np.clip(y * 255 + 0.5, 0, 255).astype(np.uint8), A]), 'RGBA').save(os.path.join(out, tid + '.png'))
        n += 1
    torch.cuda.synchronize()
    print('%d textures in %.1f s with %s (%s)' % (n, time.time() - t0, os.path.basename(model), dt))

if __name__ == '__main__':
    main()
