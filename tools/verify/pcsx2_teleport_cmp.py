# tpcmp.py EXPLORE_LOG FRAMEDIR OUTDIR [STAGE] [EVERY]: for explorer samples that have a frame dump, teleport Fiona in the
# reference PCSX2 (same stage) to the same position, capture its frame and compare with ours. Writes OUTDIR/index.tsv.
# Windows: the unattended PCSX2 of pcsx2_ref.py (start it first; frames are PCSX2's raw 640x448 screenshots, so no
# scaling). Linux: the user's PCSX2 window, found with xdotool and captured with import (window chrome cropped).
import os, re, struct, subprocess, sys, time
import numpy as np
from PIL import Image
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); import pine
WIN = os.name == "nt"
if WIN:
    import pcsx2_ref, wincap
    W = pcsx2_ref.window()
    if not W: raise SystemExit("start the reference PCSX2 first: python pcsx2_ref.py start SLOT")
else:
    os.environ["DISPLAY"] = ":0"
    W = subprocess.run(["xdotool", "search", "--name", "^Haunting Ground$"], capture_output=True, text=True).stdout.split()[0]
log, fdir, out = sys.argv[1:4]; want = int(sys.argv[4], 16) if len(sys.argv) > 4 else None; every = int(sys.argv[5]) if len(sys.argv) > 5 else 1
os.makedirs(out, exist_ok=True)
def wf(a, v): pine.w32(a, struct.unpack("<I", struct.pack("<f", v))[0])
def cross():   # the reference settings bind Cross to K
    if WIN: wincap.tap(W, 0x4B); time.sleep(0.35)
    else: subprocess.run(["xdotool", "keydown", "--window", W, "k"]); time.sleep(0.12); subprocess.run(["xdotool", "keyup", "--window", W, "k"]); time.sleep(0.35)
def capture():
    if WIN:
        f = os.path.join(out, "_ref.png"); pcsx2_ref.shot(f); return Image.open(f).convert("RGB")
    subprocess.run(["import", "-silent", "-window", W, "/tmp/_tp.png"])
    a = np.asarray(Image.open("/tmp/_tp.png").convert("RGB"))
    H = a.shape[0]; body = a[19:H - 26]; xs = np.nonzero(body.sum(axis=(0, 2)) > 0)[0]
    return Image.fromarray(body[:, xs[0]:xs[-1] + 1])
samples = []
for l in open(log, errors="replace"):
    m = re.match(r"explore: VBlank (\d+) stage ([0-9a-f]+) pos (\S+) (\S+) (\S+)", l)
    if m: samples.append((int(m.group(1)), int(m.group(2), 16), float(m.group(3)), float(m.group(4)), float(m.group(5))))
idx = open(os.path.join(out, "index.tsv"), "a"); n = 0
for k, (vb, st, x, y, z) in enumerate(samples):
    f = os.path.join(fdir, "frame_%06d.ppm" % vb)
    if not os.path.exists(f) or (want is not None and st != want) or k % every: continue
    p = pine.r32(0x44f818)
    if not p or pine.r32(p + 0x30) != st: continue
    for _ in range(2): cross()   # dismiss any text box (a monologue pauses the game and the teleport would not show)
    wf(p + 0x10, x); wf(p + 0x14, y); wf(p + 0x18, z); time.sleep(1.5)
    wf(p + 0x10, x); wf(p + 0x14, y); wf(p + 0x18, z); time.sleep(0.4)
    ref = capture().resize((640, 480), Image.LANCZOS if not WIN else Image.BILINEAR)
    ours = Image.open(f).convert("RGB").resize((640, 480), Image.BILINEAR)
    o = np.asarray(ours).astype(int); r = np.asarray(ref).astype(int); d = np.abs(o - r).max(2)
    c = Image.new("RGB", (1280, 480)); c.paste(ours, (0, 0)); c.paste(ref, (640, 0)); c.save(os.path.join(out, "vb%06d.png" % vb))
    line = "%d\t%x\t%.1f\t%.1f\t%.1f\t%.2f\t%.1f" % (vb, st, x, y, z, np.abs(o - r).mean(), (d <= 8).mean() * 100)
    idx.write(line + "\n"); idx.flush(); print(line, flush=True); n += 1
print("compared", n)
