#!/usr/bin/env python3
"""Sony SCEI HD/BD sound bank parser + PS-ADPCM (VAG) decoder.
usage: hdbd.py BANK.HD BANK.BD OUTDIR   -> OUTDIR/<bank>_<idx>.wav + index.tsv
"""
import struct, sys, os, wave
import numpy as np

COEF = [(0,0),(60,0),(115,-52),(98,-55),(122,-60)]

def decode_vag(bd, off):
    out = []; h1 = h2 = 0; p = off; loop = None
    while p + 16 <= len(bd):
        sh = bd[p] & 0xF; f = bd[p] >> 4; flag = bd[p+1]
        if f > 4: f = 0
        c1, c2 = COEF[f]
        if flag & 4 and loop is None: loop = len(out)
        for b in bd[p+2:p+16]:
            for nib in (b & 0xF, b >> 4):
                s = nib - 16 if nib >= 8 else nib
                s = (s << 12) >> sh
                s = s + (h1*c1 + h2*c2) / 64.0
                s = max(-32768, min(32767, s))
                out.append(s); h2, h1 = h1, s
        p += 16
        if flag & 1: break          # end (1 = end, 3 = loop end)
    return np.array(out, dtype=np.float64), loop, (p-off)

def parse_hd(hd):
    head = 0x10
    assert hd[head+4:head+8] == b'daeH'
    prog_o, sset_o, smpl_o, vagi_o = struct.unpack_from('<4I', hd, head+0x14)
    def table(o):
        n = struct.unpack_from('<I', hd, o+12)[0]
        return [o + x for x in struct.unpack_from('<%dI' % (n+1), hd, o+16)]
    vagi = [struct.unpack_from('<IHBB', hd, a) for a in table(vagi_o)]
    smpl = [hd[a:a+0x2a] for a in table(smpl_o)]
    return vagi, smpl

def main(hdp, bdp, outdir):
    hd = open(hdp, 'rb').read(); bd = open(bdp, 'rb').read()
    os.makedirs(outdir, exist_ok=True)
    name = os.path.splitext(os.path.basename(hdp))[0]
    vagi, smpl = parse_hd(hd)
    rows = []
    for i, (off, rate, loopf, _) in enumerate(vagi):
        pcm, loop, nbytes = decode_vag(bd, off)
        fn = os.path.join(outdir, '%s_%02X.wav' % (name, i))
        w = wave.open(fn, 'wb'); w.setnchannels(1); w.setsampwidth(2); w.setframerate(rate)
        w.writeframes(np.clip(np.round(pcm), -32768, 32767).astype('<i2').tobytes()); w.close()
        dur = len(pcm)/rate
        # crude spectral centroid + peak
        if len(pcm) > 256:
            sp = np.abs(np.fft.rfft(pcm * np.hanning(len(pcm)))); fr = np.fft.rfftfreq(len(pcm), 1/rate)
            cen = float((sp*fr).sum()/max(sp.sum(),1e-9))
        else: cen = 0
        rows.append((i, off, nbytes, rate, loopf, loop, round(dur,3), int(np.abs(pcm).max() if len(pcm) else 0), int(cen)))
    with open(os.path.join(outdir, name + '_index.tsv'), 'w') as f:
        f.write('idx\tbd_off\tbytes\trate\tloopflag\tloop_sample\tdur_s\tpeak\tcentroid_hz\n')
        for r in rows: f.write('\t'.join(map(str, r)) + '\n')
    return rows

if __name__ == '__main__':
    for r in main(*sys.argv[1:4]): print(*r, sep='\t')
