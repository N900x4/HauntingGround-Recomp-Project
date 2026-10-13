"""usage: sfdaudio_check.py STREAM.raw /PATH.SFD  -> where the SFD's ADX track appears in an HG_ADMAWAV capture and how it matches"""
import os
REPO = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')
GAME = os.environ.get('HG_GAME_DIR', os.path.join(REPO, 'game'))
import sys, numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); sys.path.insert(0, os.path.join(REPO, 'tools', 'formats'))
import cvm, adx
cap = np.frombuffer(open(sys.argv[1], 'rb').read(), dtype=np.int16).reshape(-1, 2).astype(float)
pcm, rate, ch = adx.decode(adx.sfd_audio(cvm.read(sys.argv[2]))); ref = pcm.astype(float)
m = 12000; x = ref[:m, 0]
N = 1 << int(np.ceil(np.log2(len(cap) + m)))
cc = np.fft.irfft(np.fft.rfft(cap[:, 0], N) * np.conj(np.fft.rfft(x, N)), N)[:len(cap) - m]
start = int(np.argmax(np.abs(cc)))
L = min(len(cap) - start, len(ref))
y = cap[start:start + L]; r = ref[:L]
same = np.all(y == r, axis=1)
print('%s: %.1f s of audio at %d Hz; found at %.3f s of the stream; %.1f s compared; %.4f%% of samples identical; first difference at %s'
      % (sys.argv[2], len(ref) / rate, rate, start / 48000, L / 48000, 100 * same.mean(), ('%.3f s' % (np.argmin(same) / 48000)) if not same.all() else 'none'))
