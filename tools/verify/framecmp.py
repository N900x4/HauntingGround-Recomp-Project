# cmp.py OURS.ppm REF.png [out.png]: compare a 640x448 frame of ours with PCSX2's output (stretched to 640x480)
import sys, numpy as np
from PIL import Image
o8 = Image.open(sys.argv[1]).convert("RGB"); r = np.asarray(Image.open(sys.argv[2]).convert("RGB")).astype(int)
o = np.asarray(o8.resize((r.shape[1], r.shape[0]), Image.BILINEAR)).astype(int)
d = np.abs(o - r).max(axis=2)
print("%s: mean abs %.2f, within 8 %.1f%%, >32 %.2f%% | mean RGB ours %s ref %s" % (sys.argv[1].split("/")[-1], np.abs(o - r).mean(),
      (d <= 8).mean() * 100, (d > 32).mean() * 100, o.reshape(-1, 3).mean(0).round(1), r.reshape(-1, 3).mean(0).round(1)))
if len(sys.argv) > 3:
    Image.fromarray(np.concatenate([np.concatenate([o, r], 1), np.concatenate([np.repeat(np.clip(d * 4, 0, 255)[..., None], 3, 2), (o + r) // 2], 1)], 0).astype(np.uint8)).save(sys.argv[3])
