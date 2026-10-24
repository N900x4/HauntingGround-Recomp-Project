#!/usr/bin/env python3
"""Extract every texture on the disc with a stable id, for the HD texture work (docs/textures.md).

Reads game/DATA.CVM (made by tools/ingest.py from the user's own disc) and writes, outside the repository:
  OUT/source/<id>.png   RGBA8 (alpha 0x80 -> 255), decoded once per id
  OUT/manifest.tsv      one line per instance: id, img_crc, clut_crc, file, blob offset, entry, w, h, psm
id = <crc32 of the index bytes>_<crc32 of the CLUT bytes>, both exactly as the game uploads them (linear indices, the
1 KB CSM1 CLUT), so the runtime's HG_TEXLOG (gs.c) computes the same numbers from the image transfers it sees.
The output is game data: never commit it.   usage: texdump.py OUT"""
import os, sys, struct, zlib, collections
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'verify')); sys.path.insert(0, os.path.join(HERE, '..', 'formats'))
import cvm, tex_hg
import numpy as np
from PIL import Image

# headerless full-screen images (docs/textures.md): name -> (w, h, psm, image bytes, CLUT bytes)
BINS = {'/SYSTEM/TITLE_BACK.BIN': (640, 448, 0x14, 640 * 448 // 2, 64), '/SYSTEM/LOGO_CRI.BIN': (640, 448, 0x13, 640 * 448, 1024)}

def blobs(name, d):
    """(offset, entries) of the TEX blobs in one file"""
    ext = name.rsplit('.', 1)[-1].upper()
    if ext == 'TEX':
        t = tex_hg.parse(d, 0)
        return [(0, t)] if t else tex_hg.scan(d)
    if ext == 'PAC':
        if len(d) >= 0x28:
            o = struct.unpack_from('<I', d, 0x24)[0]   # section 9: the stage's TEX blob
            t = tex_hg.parse(d, o) if o < len(d) else None
            if t: return [(o, t)]
        return tex_hg.scan(d)
    if ext == 'TXS':
        return tex_hg.scan(d)
    return []

def main():
    out = sys.argv[1] if len(sys.argv) > 1 else sys.exit(__doc__)
    os.makedirs(os.path.join(out, 'source'), exist_ok=True)
    man = open(os.path.join(out, 'manifest.tsv'), 'w')
    man.write('id\timg_crc\tclut_crc\tfile\tblob\tentry\tw\th\tpsm\n')
    seen = set(); n = 0; per = collections.Counter()
    for name in sorted(cvm.found):
        if name in BINS:
            w, h, psm, ib, cb = BINS[name]; d = cvm.read(name)
            img, clut = d[:ib], d[ib:ib + cb]
            ent = dict(i=0, psm=psm, w=w, h=h, iq=ib // 16, cq=cb // 16, off=0, bpp=4 if psm == 0x14 else 8)
            found = [(0, [ent])]
        else:
            if name.rsplit('.', 1)[-1].upper() not in ('TEX', 'PAC', 'TXS'): continue
            d = cvm.read(name); found = blobs(name, d)
        for bo, ents in found:
            for t in ents:
                img = d[t['off']:t['off'] + t['iq'] * 16]; clut = d[t['off'] + t['iq'] * 16:t['off'] + t['iq'] * 16 + t['cq'] * 16]
                ic, cc = zlib.crc32(img), zlib.crc32(clut); tid = '%08x_%08x' % (ic, cc)
                man.write('%s\t%08x\t%08x\t%s\t0x%x\t%d\t%d\t%d\t0x%x\n' % (tid, ic, cc, name, bo, t['i'], t['w'], t['h'], t['psm']))
                n += 1; per[name.rsplit('.', 1)[-1].upper()] += 1
                if tid in seen: continue
                seen.add(tid)
                if name in BINS and t['cq'] * 16 < 1024:   # TITLE_BACK: a 16-entry CLUT, not CSM1-swizzled
                    pal = np.frombuffer(clut, np.uint8).reshape(-1, 4).copy(); pal[:, 3] = tex_hg.a80(pal[:, 3])
                    b = np.frombuffer(img, np.uint8); idx = np.empty(t['w'] * t['h'], np.uint8); idx[0::2] = b & 15; idx[1::2] = b >> 4
                    rgba = pal[idx.reshape(t['h'], t['w'])]
                else:
                    rgba = tex_hg.decode(d, t)
                Image.fromarray(rgba, 'RGBA').save(os.path.join(out, 'source', tid + '.png'))
    man.close()
    print('%d texture instances, %d unique ids (%s); manifest and PNGs in %s' % (n, len(seen), dict(per), out))

if __name__ == '__main__':
    main()
