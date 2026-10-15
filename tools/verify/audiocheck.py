"""Sound regression check on an HG_WAV capture (48 kHz stereo, the SPU2's mixed output).
usage: audiocheck.py FILE.wav [--quiet]        exit status 0 = PASS, 1 = FAIL

Features (computed from the capture; no audio data is stored anywhere):
  - stuck tones: a narrow spectral peak (>= 15 dB over the median of the bins around it) between 250 Hz and 12 kHz
    that stays on the same frequency (within 2 bins of 5.9 Hz) for 8 or more consecutive seconds of sound. Others are
    listed but not counted: the opening movie's own drones at 60-160 Hz last up to 18 s, and some rooms carry a
    faint 15.73 kHz line (-62 dBFS, the NTSC line frequency, in the game's own recordings). A voice left looping one ADPCM
    block (a 28-sample period at its pitch) shows up here: in the 2026-10-15 playtests the reverb writes overwrote
    the sample banks and voices looped on garbage blocks at 180, 360, 510, 540 and 760 Hz for half a minute.
  - DC: seconds whose mean is beyond +-300 (garbage blocks decode with large offsets; the game's own sounds stay
    near 0).
  - clicks: 10 ms blocks whose largest second difference is 8x their rms and over 2000 (sharp attacks of the game's
    own sounds count too, so this is a rate: about 2-7 per 3 minutes with the fix, 11-40 before it).
Thresholds: no stuck tone, at most 2 DC seconds, at most 15 clicks per 3 minutes of sound."""
import sys, numpy as np

def load(path):
    raw = open(path, 'rb').read()
    return np.frombuffer(raw[44:], dtype='<i2').reshape(-1, 2).astype(np.float64)

def analyse(x, SR=48000):
    m = x.mean(axis=1)
    secs = len(m) // SR
    N = 8192; win = np.hanning(N); freqs = np.fft.rfftfreq(N, 1 / SR)
    peaks, dc, loud = [], [], []
    for s in range(secs):
        seg = m[s * SR:(s + 1) * SR]
        acc = np.zeros(N // 2 + 1)
        for k in range(0, SR - N + 1, N // 2):
            w = seg[k:k + N]
            acc += np.abs(np.fft.rfft((w - w.mean()) * win)) ** 2
        L = 10 * np.log10(acc + 1e-3)
        pad = np.pad(L, 40, mode='edge')
        med = np.median(np.lib.stride_tricks.sliding_window_view(pad, 81), axis=1)
        pk = (L - med >= 15) & (L >= np.roll(L, 1)) & (L >= np.roll(L, -1))
        pk[:4] = False                                     # below 23 Hz
        peaks.append(set(np.nonzero(pk)[0]))
        dc.append(seg.mean())
        loud.append(np.sqrt(((seg - seg.mean()) ** 2).mean()) > 30)
    # stuck tones: runs of consecutive sounding seconds with a peak within 2 bins
    stuck = []
    run = {}                                               # bin -> (start second, length)
    for s in range(secs):
        new = {}
        if loud[s]:
            for b in peaks[s]:
                prev = [run[p] for p in range(b - 2, b + 3) if p in run]
                start = min(p[0] for p in prev) if prev else s
                new[b] = (start, s - start + 1)
        for b, (st, ln) in run.items():
            if not any(p in new for p in range(b - 2, b + 3)) and ln >= 8: stuck.append((freqs[b], st, ln))
        run = new
    for b, (st, ln) in run.items():
        if ln >= 8: stuck.append((freqs[b], st, ln))
    # merge reports of the same tone
    stuck.sort(key=lambda t: (t[1], t[0]))
    low = [t for t in stuck if not 250 <= t[0] <= 12000]; stuck = [t for t in stuck if 250 <= t[0] <= 12000]
    dcs = [s for s in range(secs) if loud[s] and abs(dc[s]) > 300]
    d2 = np.abs(np.diff(m, 2)); blk = 480; nb = len(d2) // blk
    d2 = d2[:nb * blk].reshape(nb, blk)
    rms = np.sqrt((d2 ** 2).mean(axis=1)) + 1
    clicks = np.nonzero((d2.max(axis=1) > 8 * rms) & (d2.max(axis=1) > 2000))[0] * blk / SR
    return dict(secs=secs, sounding=int(sum(loud)), stuck=stuck, low=low, dc=dcs, maxdc=max([abs(d) for d in dc] or [0]), clicks=clicks)

def main():
    path = sys.argv[1]; quiet = '--quiet' in sys.argv
    r = analyse(load(path))
    mins = max(r['sounding'], 1) / 180
    ok = not r['stuck'] and len(r['dc']) <= 2 and len(r['clicks']) <= 15 * max(mins, 1)
    print('%s: %d s, %d s with sound' % (path, r['secs'], r['sounding']))
    print('  stuck tones (>= 8 s on one frequency): %d' % len(r['stuck']))
    for f, st, ln in r['stuck'][:20]: print('    %7.1f Hz from %d s for %d s' % (f, st, ln))
    if r['low']: print('  (outside 250 Hz-12 kHz, not counted: %s)' % ', '.join('%.0f Hz at %d s for %d s' % t for t in r['low'][:6]))
    print('  seconds with |DC| > 300: %d (largest %.0f)%s' % (len(r['dc']), r['maxdc'], ('  at ' + ' '.join(map(str, r['dc'][:15]))) if r['dc'] and not quiet else ''))
    print('  clicks: %d%s' % (len(r['clicks']), ('  at ' + ' '.join('%.2f' % c for c in r['clicks'][:15])) if len(r['clicks']) and not quiet else ''))
    print('  ' + ('PASS' if ok else 'FAIL'))
    sys.exit(0 if ok else 1)

if __name__ == '__main__':
    main()
