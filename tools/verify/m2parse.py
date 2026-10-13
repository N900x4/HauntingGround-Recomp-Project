"""MPEG-2 video slice parser (no reconstruction) using the VLC tables from recomp/ipu.c, to check that every slice
of a stream parses exactly up to the next start code. usage: m2parse.py /CVM/PATH.SFD [max_pictures]"""
import sys, re, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cvm
src = open('/home/kali/ps2-work/HauntingGround-Recomp-Project/recomp/ipu.c').read()
def table(name):
    t = src[src.index('static const Vlc %s[]' % name):]; t = t[:t.index('VLC_END')]
    return {c: (int(a), int(b)) for c, a, b in re.findall(r'\{"([01]+)",(-?\w+),(-?\d+)\}', t.replace('0x', '').replace('-1,0', '-1,0'))} if False else \
           {c: (int(a, 0), int(b)) for c, a, b in re.findall(r'\{"([01]+)",(-?(?:0x)?[0-9a-fA-F]+),(-?\d+)\}', t)}
T = {n: table(n) for n in ['t_mbai', 't_mbt_i', 't_mbt_p', 't_mbt_b', 't_mc', 't_dmv', 't_cbp', 't_dcl', 't_dcc', 't_dct0', 't_dct1_short']}
T['t_dct1'] = dict(T['t_dct1_short']); T['t_dct1'].update({c: v for c, v in T['t_dct0'].items() if len(c) >= 14})

def demux_video(d):
    out = bytearray(); i = 0
    while i + 4 <= len(d):
        if d[i:i+3] != b'\0\0\1': i += 1; continue
        sc = d[i+3]
        if sc == 0xBA:                               # pack header (MPEG-1: 12 bytes, MPEG-2: 14 + stuffing)
            if (d[i+4] >> 6) == 1: i += 14 + (d[i+13] & 7)
            else: i += 12
            continue
        if sc == 0xB9: i += 4; continue
        if sc >= 0xBB:
            ln = struct.unpack_from('>H', d, i + 4)[0]; p = i + 6
            if sc == 0xE0:
                while d[p] == 0xFF: p += 1
                if d[p] & 0xC0 == 0x40: p += 2
                if d[p] & 0xF0 == 0x20: p += 5
                elif d[p] & 0xF0 == 0x30: p += 10
                elif d[p] == 0x0F: p += 1
                elif d[p] & 0xC0 == 0x80: p += 3 + d[p + 2]   # MPEG-2 PES header
                out += d[p:i + 6 + ln]
            i += 6 + ln; continue
        i += 1
    return bytes(out)

class Bits:
    def __init__(s, data, pos=0): s.d = data; s.p = pos
    def peek(s, n):
        v = 0
        for k in range(n):
            q = s.p + k; byte = s.d[q >> 3] if (q >> 3) < len(s.d) else 0
            v = (v << 1) | ((byte >> (7 - (q & 7))) & 1)
        return v
    def get(s, n): v = s.peek(n); s.p += n; return v
    def vlc(s, t):
        for L in range(1, 19):
            c = format(s.peek(L), '0%db' % L)
            if c in t: s.p += L; return t[c]
        raise ValueError('no code at bit %d: %s' % (s.p, format(s.peek(24), '024b')))

def parse(data, maxpic):
    starts = [m.start() for m in re.finditer(b'\x00\x00\x01', data)]
    seq = {}; pic = {}; npic = 0; nslices = 0; bad = 0
    for k, st in enumerate(starts):
        code = data[st + 3]; nxt = starts[k + 1] if k + 1 < len(starts) else len(data)
        b = Bits(data, (st + 4) * 8)
        if code == 0xB3: b.get(12); b.get(12); b.get(4); b.get(4); b.get(18); b.get(1); b.get(10); b.get(1); seq['mpeg2'] = False
        elif code == 0xB5:
            ext = b.get(4)
            if ext == 1: seq['mpeg2'] = True
            if ext == 8:
                fc = [[b.get(4), b.get(4)], [b.get(4), b.get(4)]]
                pic.update(fcode=fc, idp=b.get(2), ps=b.get(2), tff=b.get(1), fpfd=b.get(1), cmv=b.get(1), qst=b.get(1), ivf=b.get(1), alt=b.get(1))
        elif code == 0x00:
            npic += 1
            if npic > maxpic: break
            b.get(10); pct = b.get(3); b.get(16)
            pic = dict(pct=pct, fcode=[[15, 15], [15, 15]], idp=0, fpfd=1, ivf=0, qst=0, cmv=0)
        elif 1 <= code <= 0xAF:
            nslices += 1
            try:
                slice_(b, pic, seq)
                # after the last macroblock only zero stuffing may remain before the next start code
                rest_bits = nxt * 8 - b.p
                if rest_bits < 0 or (rest_bits >= 8 and b.peek(min(rest_bits, 32)) != 0) or rest_bits > 24 + 7:
                    if bad < 5: print('picture %d (type %d) slice %d: ended %d bits before the next start code' % (npic, pic['pct'], code, rest_bits))
                    bad += 1
            except ValueError as e:
                if bad < 5: print('picture %d (type %d) slice %d: %s' % (npic, pic['pct'], code, e))
                bad += 1
    print('%d pictures, %d slices, %d bad' % (min(npic, maxpic), nslices, bad))

def slice_(b, pic, seq):
    q = b.get(5)
    while b.get(1): b.get(8)
    mb = 0; pct = pic['pct']
    dc = [0, 0, 0]
    while True:
        if b.peek(23) == 0: return
        inc = 0
        while True:
            v = b.vlc(T['t_mbai'])[0]
            if v == 0x22: continue
            if v == 0x23: inc += 33; continue
            inc += v; break
        mbt = b.vlc({1: T['t_mbt_i'], 2: T['t_mbt_p'], 3: T['t_mbt_b']}[pct])[0]
        intra, pattern, back, fwd, quant = mbt & 1, mbt & 2, mbt & 4, mbt & 8, mbt & 16
        if (fwd or back) and not pic['fpfd']: b.get(2)              # frame_motion_type
        if not pic['fpfd'] and (intra or pattern): b.get(1)         # dct_type
        if quant: b.get(5)
        for r, on in ((0, fwd), (1, back)):
            if not on: continue
            for s_ in range(2):
                code = b.vlc(T['t_mc'])[0]
                if code: b.get(1)
                rs = pic['fcode'][r][s_] - 1
                if rs and code: b.get(rs)
        if intra and pic['cmv']: raise ValueError('concealment vectors not handled')
        cbp = 63 if intra else (b.vlc(T['t_cbp'])[0] if pattern else 0)
        for blk in range(6):
            if not cbp & (1 << (5 - blk)): continue
            if intra:
                size = b.vlc(T['t_dcc'] if blk >= 4 else T['t_dcl'])[0]; b.get(size)
                tab = T['t_dct1'] if pic['ivf'] else T['t_dct0']; i = 1
            else:
                tab = T['t_dct0']; i = 0
                if b.peek(1): b.get(2); i = 1
            while True:
                run, lev = b.vlc(tab)
                if run == -1: break
                if run == -2:
                    run = b.get(6); b.get(12 if seq.get('mpeg2') else 8)
                else: b.get(1)
                i += run + 1
                if i > 64: raise ValueError('coefficient overflow in block %d at bit %d' % (blk, b.p))
        mb += inc

d = cvm.read(sys.argv[1]); v = demux_video(d)
print('video elementary stream: %d bytes' % len(v))
parse(v, int(sys.argv[2]) if len(sys.argv) > 2 else 1000)
