"""Drive the reference PCSX2 (pcsx2_ref.py) along a route of rooms: at each step, teleport Fiona next to a door or exit,
hold buttons until the stage number changes, then go on. Doors are taken from a replay of ours (HG_POSLOG output).
usage: pcsx2_route.py STEPS.txt   (start PCSX2 first: python pcsx2_ref.py start SLOT)
STEPS.txt lines:  door STAGE X Y Z KEYS FRAMES   teleport (in stage STAGE) and hold KEYS (e.g. up+cross) for FRAMES
                                                 VBlanks' worth of time, until the stage changes
                  goto STAGE X Y Z               teleport inside STAGE and wait for the camera to settle
                  shot OUT.png                   PCSX2 screenshot
Keys follow the reference settings: arrows = D-pad, K = Cross, L = Circle, Return = Start."""
import os, struct, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pine, pcsx2_ref, wincap
VK = {"up": 0x26, "down": 0x28, "left": 0x25, "right": 0x27, "cross": 0x4B, "circle": 0x4C, "start": 0x0D}
WM_KEYDOWN, WM_KEYUP = 0x0100, 0x0101

def player(): return pine.r32(0x44f818)
def stage():
    p = player(); return pine.r32(p + 0x30) if p else None
def pos():
    p = player(); return [struct.unpack("<f", struct.pack("<I", pine.r32(p + 0x10 + 4 * i)))[0] for i in range(3)]
def teleport(x, y, z):
    p = player()
    for i, v in enumerate((x, y, z)): pine.w32(p + 0x10 + 4 * i, struct.unpack("<I", struct.pack("<f", v))[0])
EXTENDED = {0x25, 0x26, 0x27, 0x28}   # arrow keys: without the extended flag they read as the numpad's
# PCSX2 reads pad keys only while its game window has focus (hotkeys such as F8 work regardless), so walking through
# a door brings the window forward for a moment and sends real key events (keybd_event), then minimizes it again.
def focus(h, on):
    if on:
        wincap.u32.ShowWindow(h, 9)                                        # SW_RESTORE
        wincap.u32.keybd_event(0x12, 0, 0, 0); wincap.u32.keybd_event(0x12, 0, 2, 0)   # an Alt tap lifts the foreground lock
        wincap.u32.SetForegroundWindow(h); time.sleep(0.3)
    else: wincap.u32.ShowWindow(h, 7)                                      # SW_SHOWMINNOACTIVE
def keys(h, names, down):
    for n in names:
        vk = VK[n]; flags = (1 if vk in EXTENDED else 0) | (0 if down else 2)   # KEYEVENTF_EXTENDEDKEY, KEYEVENTF_KEYUP
        wincap.u32.keybd_event(vk, wincap.u32.MapVirtualKeyW(vk, 0), flags, 0)

def run(steps):
    h = pcsx2_ref.window()
    if not h: raise SystemExit("start the reference PCSX2 first: python pcsx2_ref.py start SLOT")
    for line in open(steps):
        a = line.split()
        if not a or a[0].startswith("#"): continue
        if a[0] == "door":
            st, x, y, z, ks, n = int(a[1], 16), float(a[2]), float(a[3]), float(a[4]), a[5].split("+"), int(a[6])
            if stage() != st: raise SystemExit("expected stage %x, PCSX2 is in %x" % (st, stage()))
            teleport(x, y, z); time.sleep(0.3)
            focus(h, True); keys(h, ks, True)
            t0 = time.time()
            while time.time() - t0 < n / 59.94 + 5 and stage() == st: time.sleep(0.05)
            keys(h, ks, False); focus(h, False)
            if stage() == st: raise SystemExit("no stage change at the door in stage %x" % st)
            print("stage %x -> %x" % (st, stage()), flush=True); time.sleep(4)   # let the room load
        elif a[0] == "goto":
            st = int(a[1], 16)
            if stage() != st: raise SystemExit("expected stage %x, PCSX2 is in %x" % (st, stage()))
            teleport(float(a[2]), float(a[3]), float(a[4])); time.sleep(1.5); teleport(float(a[2]), float(a[3]), float(a[4])); time.sleep(1.0)
            print("in stage %x at" % st, [round(v, 1) for v in pos()], flush=True)
        elif a[0] == "shot":
            print(pcsx2_ref.shot(a[1]), flush=True)

if __name__ == "__main__": run(sys.argv[1])
