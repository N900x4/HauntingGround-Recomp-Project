"""A/B timing with drift check: one discarded warm-up run, then A, B, B, A, each the same headless run with
HG_SPEEDLOG; compares host work per VBlank over a steady window. A difference smaller than the A-to-A drift means
nothing.
usage: abba.py BIN_A BIN_B [MAXVB [FROM_VB]]   (run from recomp/; environment: the run's settings, e.g. HG_GSDRAW=0;
the default route is New Game -> stage 1 with movies skipped, an empty card, from VBlank 3000 to 4800; HG_ROUTE=cs: LOAD GAME
from ../memcards into the Castle Suite)"""
import os, re, subprocess, sys, tempfile, statistics
A, B = sys.argv[1], sys.argv[2]
MAXVB = sys.argv[3] if len(sys.argv) > 3 else "4800"; FROM = int(sys.argv[4]) if len(sys.argv) > 4 else 3000
def run(binary):
    env = dict(os.environ, HG_SPEEDLOG="300", HG_WINDOW="0", HG_MOVIES="skip", HG_MAXVB=MAXVB, HG_PADREC="")
    pad = "600:cross,900:cross,1200:cross,1500:cross,1800:cross,2100:cross"
    if os.environ.get("HG_ROUTE") == "cs": pad += ",2400:cross,2600:cross,2800:cross"
    else: env["HG_MEMCARD_DIR"] = tempfile.mkdtemp(prefix="abba_mc_")
    env.setdefault("HG_PAD", pad)
    out = subprocess.run([binary, "-q", "-t", "1800"], env=env, capture_output=True, text=True).stdout
    vals = [(float(m.group(2)), float(m.group(3))) for m in re.finditer(r"SPEED vb (\d+): .*?per VB [\d.]+ ms \(busy ([\d.]+).*?VU1 ([\d.]+)", out) if int(m.group(1)) >= FROM]
    if not vals: raise SystemExit("no SPEED lines from %s" % binary)
    return statistics.mean(v[0] for v in vals), statistics.mean(v[1] for v in vals)
run(A)   # warm-up, discarded
r = [run(A), run(B), run(B), run(A)]
for k, name in ((0, "EE thread (busy)"), (1, "VU1")):
    a1, b1, b2, a2 = (x[k] for x in r)
    ma, mb = (a1 + a2) / 2, (b1 + b2) / 2
    print("%s  A: %.2f %.2f ms per VBlank (drift %.2f)   B: %.2f %.2f (drift %.2f)" % (name, a1, a2, abs(a1 - a2), b1, b2, abs(b1 - b2)))
    print("%s  B vs A: %+.2f ms per VBlank (%+.1f %%); the drift is %.2f ms" % (name, mb - ma, (mb - ma) / ma * 100, max(abs(a1 - a2), abs(b1 - b2))))
