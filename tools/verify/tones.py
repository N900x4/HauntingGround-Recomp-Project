"""Find beep-like events in a 48 kHz stereo WAV (HG_WAV output): short stretches where one narrow frequency dominates
the spectrum and stands out from what came just before.
usage: tones.py FILE.wav [PAD.txt]   with an input recording, each event also lists the button changes near it."""
import sys, struct, numpy as np
wav = sys.argv[1]
raw = open(wav, 'rb').read(); data = np.frombuffer(raw[44:], dtype='<i2').reshape(-1, 2).astype(np.float32).mean(axis=1)
SR = 48000; N = 2048; HOP = 1024
win = np.hanning(N).astype(np.float32)
frames = (len(data) - N) // HOP
spec = np.empty((frames, N // 2), np.float32)
for i in range(frames):
    spec[i] = np.abs(np.fft.rfft(data[i * HOP:i * HOP + N] * win))[:N // 2]
freqs = np.fft.rfftfreq(N, 1 / SR)[:N // 2]
band = (freqs > 300) & (freqs < 6000)
peak = spec[:, band].max(axis=1); total = spec[:, band].sum(axis=1) + 1e-9
pk_bin = spec[:, band].argmax(axis=1); pk_f = freqs[band][pk_bin]
tonal = peak / total                       # share of the band's energy in the strongest bin
energy = 20 * np.log10(peak + 1)
# a beep: tonal (narrow), louder than the second before it, and the peak frequency steady for >= 3 frames
bg = np.array([np.median(energy[max(0, i - 47):max(1, i - 2)]) for i in range(frames)])
cand = (tonal > 0.08) & (energy > bg + 12)
events = []; i = 0
while i < frames:
    if cand[i]:
        j = i
        while j + 1 < frames and cand[j + 1] and abs(pk_f[j + 1] - pk_f[i]) < 60: j += 1
        if j - i + 1 >= 3: events.append((i, j))
        i = j + 1
    else: i += 1
pad = []
if len(sys.argv) > 2:
    pad = [(int(l.split()[0]), l.split()[1]) for l in open(sys.argv[2]) if l[:1].isdigit()]
print('%d beep-like events in %.1f s of audio' % (len(events), len(data) / SR))
for a, b in events:
    t0, t1 = a * HOP / SR, (b * HOP + N) / SR; vb = int(t0 * 59.94)
    near = [p for p in pad if vb - 12 <= p[0] <= vb + 3]
    print('%7.2f s (VBlank ~%5d) %4.0f ms  %5.0f Hz  %+.0f dB over the second before%s' % (
        t0, vb, (t1 - t0) * 1000, pk_f[a], energy[a:b + 1].max() - bg[a],
        ('   buttons ' + ' '.join('%d:%s' % p for p in near)) if near else ''))
