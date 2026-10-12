"""Haunting Ground TEX-blob parser/decoder (used by .TEX files, PAC texture sections, etc).
Blob layout (all little-endian, offsets relative to blob start):
  0x00 u32 count, 12 bytes zero
  0x10 + i*16 entry: u32 psm (0x13 PSMT8, 0x14 PSMT4, 0x00 PSMCT32 ...), u16 w, u16 h,
                     u16 img_qwc (image size /16), u16 clut_qwc (0x40 = 1024 B = 256x32bit CLUT),
                     u32 data_off (relative to the entry itself)
  data: w*h (PSMT8) or w*h/2 (PSMT4, low nibble first) linear indices, followed by clut_qwc*16 bytes CLUT
        CLUT = 256 RGBA8888 entries in CSM1 order (blocks of 8 entries 8..15 <-> 16..23 swapped),
        alpha 0x80 = opaque.
"""
import struct, numpy as np
PSM = {0x00:'PSMCT32',0x01:'PSMCT24',0x02:'PSMCT16',0x0a:'PSMCT16S',0x13:'PSMT8',0x14:'PSMT4',0x1b:'PSMT8H',0x24:'PSMT4HL',0x2c:'PSMT4HH'}

def parse(d, base=0, strict=True):
    """Return list of entry dicts or None if not a plausible TEX blob."""
    if base+16 > len(d): return None
    n = struct.unpack_from('<I', d, base)[0]
    if not (1 <= n <= 512) or d[base+4:base+16] != b'\0'*12: return None
    out = []
    for i in range(n):
        e = base + 0x10 + i*16
        if e+16 > len(d): return None
        psm, w, h, iq, cq, off = struct.unpack_from('<IHHHHI', d, e)
        if psm not in PSM or not (w and h) or w > 1024 or h > 1024: return None
        bpp = {0x13:8,0x14:4,0x00:32,0x01:24,0x02:16,0x0a:16}.get(psm, 8)
        if strict and iq*16 != w*h*bpp//8: return None
        o = e + off
        if o + iq*16 + cq*16 > len(d): return None
        out.append(dict(i=i, psm=psm, w=w, h=h, iq=iq, cq=cq, off=o, bpp=bpp))
    return out

def unswizzle_csm1(pal):
    p = pal.reshape(-1, 4, 8, 4).copy() if len(pal) % 32 == 0 else None
    if p is None: return pal
    p[:, [1, 2]] = p[:, [2, 1]]
    return p.reshape(-1, 4)

def a80(a):  # PS2 alpha: 0x80 = 1.0
    return np.minimum(255, a.astype(np.int32) * 255 // 128).astype(np.uint8)

def decode(d, t, csa=0):
    w, h, o = t['w'], t['h'], t['off']
    if t['psm'] in (0x13, 0x14):
        if t['psm'] == 0x13:
            idx = np.frombuffer(d, np.uint8, w*h, o).reshape(h, w)
        else:
            b = np.frombuffer(d, np.uint8, w*h//2, o)
            idx = np.empty(w*h, np.uint8); idx[0::2] = b & 15; idx[1::2] = b >> 4
            idx = idx.reshape(h, w) + csa*16
        po = o + t['iq']*16
        pal = np.frombuffer(d, np.uint8, t['cq']*16, po).reshape(-1, 4)
        pal = unswizzle_csm1(pal).copy(); pal[:, 3] = a80(pal[:, 3])
        return pal[idx]
    if t['psm'] == 0x00:
        px = np.frombuffer(d, np.uint8, w*h*4, o).reshape(h, w, 4).copy(); px[..., 3] = a80(px[..., 3]); return px
    if t['psm'] in (0x02, 0x0a):
        v = np.frombuffer(d, '<u2', w*h, o).astype(np.uint32)
        r = (v & 31) << 3; g = ((v >> 5) & 31) << 3; b = ((v >> 10) & 31) << 3; a = np.where(v >> 15, 255, 0)
        return np.stack([r, g, b, a], -1).astype(np.uint8).reshape(h, w, 4)
    raise NotImplementedError(PSM[t['psm']])

def scan(d, step=16):
    """Find TEX blobs embedded anywhere in a container (16-byte aligned)."""
    hits = []; o = 0
    while o < len(d) - 32:
        t = parse(d, o)
        if t: hits.append((o, t)); o += 16 + max(x['off'] - o + x['iq']*16 + x['cq']*16 for x in t) - 16; o = (o + 15) & ~15
        else: o += step
    return hits
