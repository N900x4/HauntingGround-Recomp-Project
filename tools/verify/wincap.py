"""Windows window capture and key taps for the PCSX2 comparison tools (the Linux versions used xdotool and import).
find(title, pid=None) -> hwnd; grab(hwnd) -> PIL image of the client area, taken with PrintWindow(PW_RENDERFULLCONTENT),
which also works while other windows cover it; tap(hwnd, vk) posts a key press and release to the window.
usage: python wincap.py "Haunting Ground" out.png   (one capture, for testing)"""
import ctypes, ctypes.wintypes as W, sys, time
from PIL import Image
u32, g32 = ctypes.windll.user32, ctypes.windll.gdi32
u32.SetProcessDPIAware()
PW_RENDERFULLCONTENT = 2
WM_KEYDOWN, WM_KEYUP = 0x0100, 0x0101

def find(title, pid=None):
    found = []
    @ctypes.WINFUNCTYPE(W.BOOL, W.HWND, W.LPARAM)
    def cb(h, _):
        n = u32.GetWindowTextLengthW(h); b = ctypes.create_unicode_buffer(n + 1); u32.GetWindowTextW(h, b, n + 1)
        if b.value == title and u32.IsWindowVisible(h):
            p = W.DWORD(); u32.GetWindowThreadProcessId(h, ctypes.byref(p))
            if pid is None or p.value == pid: found.append(h)
        return True
    u32.EnumWindows(cb, 0)
    return found[0] if found else None

class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [("biSize", W.DWORD), ("biWidth", W.LONG), ("biHeight", W.LONG), ("biPlanes", W.WORD), ("biBitCount", W.WORD),
                ("biCompression", W.DWORD), ("biSizeImage", W.DWORD), ("biXPelsPerMeter", W.LONG), ("biYPelsPerMeter", W.LONG),
                ("biClrUsed", W.DWORD), ("biClrImportant", W.DWORD)]

def grab(hwnd):
    r = W.RECT(); u32.GetClientRect(hwnd, ctypes.byref(r)); w, h = r.right, r.bottom
    wdc = u32.GetDC(hwnd); dc = g32.CreateCompatibleDC(wdc); bmp = g32.CreateCompatibleBitmap(wdc, w, h); old = g32.SelectObject(dc, bmp)
    try:
        if not u32.PrintWindow(hwnd, dc, PW_RENDERFULLCONTENT | 1):   # 1 = PW_CLIENTONLY
            raise OSError("PrintWindow failed")
        bi = BITMAPINFOHEADER(ctypes.sizeof(BITMAPINFOHEADER), w, -h, 1, 32, 0, 0, 0, 0, 0, 0)
        buf = ctypes.create_string_buffer(w * h * 4)
        g32.GetDIBits(dc, bmp, 0, h, buf, ctypes.byref(bi), 0)
        return Image.frombuffer("RGB", (w, h), buf, "raw", "BGRX", 0, 1)
    finally:
        g32.SelectObject(dc, old); g32.DeleteObject(bmp); g32.DeleteDC(dc); u32.ReleaseDC(hwnd, wdc)

def tap(hwnd, vk, hold=0.12):
    u32.PostMessageW(hwnd, WM_KEYDOWN, vk, 1); time.sleep(hold); u32.PostMessageW(hwnd, WM_KEYUP, vk, 0xC0000001)

if __name__ == "__main__":
    h = find(sys.argv[1])
    if not h: raise SystemExit("no window titled %r" % sys.argv[1])
    im = grab(h); im.save(sys.argv[2]); print(im.size)
