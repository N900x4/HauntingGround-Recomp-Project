"""Walk Fiona in the reference PCSX2 along a path of world positions by steering with the D-pad, closed loop.
(Teleporting her by writing the player object's position moves her picture but leaves the game unable to move her,
so to get through doors she has to walk.) The game's controls follow the fixed camera, which changes inside a room,
so which D-pad direction moves her which way in the world is learned from how she actually moves, and relearned.
usage: pcsx2_walk.py PATH.txt [DOOR]   PATH lines "VB X Y Z" (HG_POSLOG output of a replay of ours, in one stage);
DOOR: after the last point, keep walking into it holding Cross until the stage changes."""
import math, os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pcsx2_ref, pcsx2_route as r

DIRS = {"up": (0, 1), "down": (0, -1), "left": (-1, 0), "right": (1, 0)}
TICK = 0.1

def xz(): p = r.pos(); return p[0], p[2]

class Walker:
    def __init__(self, h):
        self.h = h; self.est = {}            # D-pad direction -> learned world (dx, dz) unit vector
        self.held = []
    def hold(self, keys):
        keys = list(keys)
        if keys == self.held: return
        r.keys(self.h, [k for k in self.held if k not in keys], False)
        r.keys(self.h, [k for k in keys if k not in self.held], True)
        self.held = keys
    def probe(self):   # hold each direction briefly and note where she goes
        for d in DIRS:
            x0, z0 = xz(); self.hold([d]); time.sleep(0.25); self.hold([]); time.sleep(0.15)
            x1, z1 = xz(); n = math.hypot(x1 - x0, z1 - z0)
            if n > 0.2: self.est[d] = ((x1 - x0) / n, (z1 - z0) / n)
    def best(self, tx, tz):
        x, z = xz(); dx, dz = tx - x, tz - z; n = math.hypot(dx, dz) or 1
        cands = []
        for a in DIRS:
            if a in self.est: cands.append(([a], self.est[a]))
        for a, b in (("up", "left"), ("up", "right"), ("down", "left"), ("down", "right")):
            if a in self.est and b in self.est:
                v = (self.est[a][0] + self.est[b][0], self.est[a][1] + self.est[b][1]); m = math.hypot(*v) or 1
                cands.append(([a, b], (v[0] / m, v[1] / m)))
        if not cands: return None
        return max(cands, key=lambda c: (c[1][0] * dx + c[1][1] * dz) / n)[0]
    def walk_to(self, tx, tz, reach=1.5, extra=(), timeout=20):
        t0 = time.time(); stuck = 0
        while time.time() - t0 < timeout:
            x, z = xz()
            if math.hypot(tx - x, tz - z) < reach: return True
            keys = self.best(tx, tz)
            if keys is None: self.probe(); continue
            self.hold(keys + list(extra)); time.sleep(TICK)
            nx, nz = xz(); moved = math.hypot(nx - x, nz - z)
            if moved > 0.05:   # learn: the held direction(s) moved her this way
                v = ((nx - x) / moved, (nz - z) / moved)
                if len(keys) == 1: self.est[keys[0]] = v
                stuck = 0
            else:
                stuck += 1
                if stuck >= 8: self.hold([]); self.est.clear(); self.probe(); stuck = 0   # camera changed or blocked
        return False

def run(path, door):
    h = pcsx2_ref.window()
    if not h: raise SystemExit("start the reference PCSX2 first")
    pts = [tuple(map(float, l.split()[1:4])) for l in open(path) if l.strip()]
    pts = [p for i, p in enumerate(pts) if i == 0 or math.hypot(p[0] - pts[i - 1][0], p[2] - pts[i - 1][2]) > 0.3]   # drop standing still
    st = r.stage(); w = Walker(h); r.focus(h, True)
    try:
        w.probe()
        for i, (x, y, z) in enumerate(pts[::3] + [pts[-1]]):
            ok = w.walk_to(x, z)
            print("point %d/%d (%.1f, %.1f): %s, she is at %s" % (i + 1, len(pts[::3]) + 1, x, z, "reached" if ok else "timed out", [round(v, 1) for v in r.pos()]), flush=True)
        if door:
            # the door is where the path ended; she came at it along the path's last stretch: stop a few units short of
            # it on that line, then push straight in, tapping Cross (a press, not a hold, opens it)
            x, y, z = pts[-1]; k = len(pts) - 1
            while k > 0 and math.hypot(pts[k][0] - x, pts[k][2] - z) < 6: k -= 1
            ux, uz = x - pts[k][0], z - pts[k][2]; m = math.hypot(ux, uz) or 1; ux, uz = ux / m, uz / m
            w.walk_to(x - ux * 5, z - uz * 5, reach=1.0)
            t0 = time.time(); n = 0
            while r.stage() == st and time.time() - t0 < 20:
                px, pz = xz(); keys = w.best(px + ux * 10, pz + uz * 10) or []
                n += 1
                w.hold(keys + (["cross"] if n % 4 < 2 else [])); time.sleep(TICK)
            print("stage %x -> %x" % (st, r.stage()), flush=True)
    finally:
        w.hold([]); r.focus(h, False)
    return r.stage() != st

if __name__ == "__main__":
    ok = run(sys.argv[1], len(sys.argv) > 2 and sys.argv[2] == "DOOR")
    sys.exit(0 if ok or len(sys.argv) <= 2 else 1)
