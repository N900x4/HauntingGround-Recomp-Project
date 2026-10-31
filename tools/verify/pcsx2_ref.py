"""The reference PCSX2 for comparisons, run unattended on Windows: its own data folder (settings with PINE on and the
software renderer, the VM's save states and memory card), the main window hidden (-nogui) and the game window
minimized, so it stays off the desktop. Frames come from PCSX2's own screenshot (F8, raw internal resolution),
which overlays such as RivaTuner do not reach.
  python pcsx2_ref.py start SLOT      boot the disc and load save state SLOT (1 main menu, 2 opening movie, 4 stage 1,
                                      5 Castle Suite; 0 = no state, a cold boot); waits until PINE answers
  python pcsx2_ref.py shot OUT.png    one screenshot of the current frame
  python pcsx2_ref.py stop
Paths: $PCSX2_EXE, $PCSX2_DATA (the folder that holds PCSX2\\), $HG_ISO."""
import glob, os, subprocess, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pine, wincap
EXE = os.environ.get("PCSX2_EXE", r"C:\Program Files\PCSX2\pcsx2-qt.exe")
DATA = os.environ.get("PCSX2_DATA", os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "tools", "pcsx2-data"))   # (next to the repository)
ISO = os.environ.get("HG_ISO", r"D:\PS2 Games\Haunting Ground (USA)\Haunting Ground (USA).iso")
SNAPS = os.path.join(DATA, "PCSX2", "snaps")
PIDFILE = os.path.join(DATA, "pcsx2.pid")
TITLE = "Haunting Ground"
VK_F8 = 0x77
SW_SHOWMINNOACTIVE = 7

def pid():
    try: return int(open(PIDFILE).read())
    except (OSError, ValueError): return None

def window():
    return wincap.find(TITLE, pid())

def start(slot):
    stop()
    args = [EXE, "-datapath", DATA, "-nogui"]
    if slot:   # 0: boot the disc from scratch (the game then reads the memory card as on a console)
        state = os.path.join(DATA, "PCSX2", "sstates", "SLUS-21075 (901AAC09).%02d.p2s" % slot)
        if not os.path.exists(state): raise SystemExit("no save state " + state)
        args += ["-statefile", state]
    p = subprocess.Popen(args + ["--", ISO])
    open(PIDFILE, "w").write(str(p.pid))
    for _ in range(120):   # up to 60 s: the window, then PINE
        time.sleep(0.5)
        h = window()
        if h:
            wincap.u32.ShowWindow(h, SW_SHOWMINNOACTIVE)
            try:
                if pine.r32(0x44f818) is not None: return h
            except (OSError, SystemExit): pass   # not listening yet, or no game loaded yet
    raise SystemExit("PCSX2 did not come up")

def shot(out, timeout=10):
    h = window()
    if not h: raise SystemExit("PCSX2 is not running")
    before = set(glob.glob(os.path.join(SNAPS, "*.png")))
    wincap.tap(h, VK_F8)
    for _ in range(int(timeout / 0.1)):
        time.sleep(0.1)
        new = set(glob.glob(os.path.join(SNAPS, "*.png"))) - before
        if new:
            f = new.pop()
            for _ in range(100):   # PCSX2 writes the file on another thread: wait until it lets go
                try: os.replace(f, out); return out
                except PermissionError: time.sleep(0.1)
            raise SystemExit("screenshot still locked: " + f)
    raise SystemExit("no screenshot from PCSX2")

def stop():
    p = pid()
    if p: subprocess.run(["taskkill", "/PID", str(p), "/F"], capture_output=True)
    try: os.remove(PIDFILE)
    except OSError: pass

if __name__ == "__main__":
    c = sys.argv[1]
    if c == "start": start(int(sys.argv[2])); print("PCSX2 running (minimized), PINE up")
    elif c == "shot": print(shot(sys.argv[2]))
    elif c == "stop": stop()
