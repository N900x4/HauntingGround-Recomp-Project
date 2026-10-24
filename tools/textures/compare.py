#!/usr/bin/env python3
"""Side-by-side crops for reviewing upscales: the original (4x nearest), then each upscaled folder, at output size.
usage: compare.py OUT.png SRC_DIR ID X Y W H DIR1 [DIR2 ...] [--magenta]   (X Y W H in source texels; --magenta: alpha over magenta)"""
import sys, os
from PIL import Image, ImageDraw

def main():
    out, src, tid = sys.argv[1], sys.argv[2], sys.argv[3]
    x, y, w, h = map(int, sys.argv[4:8]); dirs = [d for d in sys.argv[8:] if d != '--magenta']
    bgc = (255, 0, 255, 255) if '--magenta' in sys.argv else (24, 24, 24, 255)
    o = Image.open(os.path.join(src, tid + '.png')).convert('RGBA')
    s = 4; panels = [('original (nearest)', o.crop((x, y, x + w, y + h)).resize((w * s, h * s), Image.NEAREST))]
    for d in dirs:
        u = Image.open(os.path.join(d, tid + '.png')).convert('RGBA'); k = u.size[0] // o.size[0]
        panels.append((os.path.basename(d.rstrip('/')).replace('test_', ''), u.crop((x * k, y * k, (x + w) * k, (y + h) * k)).resize((w * s, h * s), Image.LANCZOS)))
    W = w * s; H = h * s
    sheet = Image.new('RGB', (len(panels) * (W + 8), H + 22), (24, 24, 24)); dr = ImageDraw.Draw(sheet)
    for i, (name, p) in enumerate(panels):
        bg = Image.new('RGBA', p.size, bgc); bg.alpha_composite(p)
        sheet.paste(bg.convert('RGB'), (i * (W + 8), 22)); dr.text((i * (W + 8) + 4, 4), name, fill=(255, 220, 0))
    sheet.save(out)

if __name__ == '__main__':
    main()
