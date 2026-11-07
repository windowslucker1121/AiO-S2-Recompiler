#!/usr/bin/env python3
"""Screenshots and remote control of a native window without the mouse (tests of the Initial Setup, runtime/apps/setup): PrintWindow
captures it even when covered, posted WM_COMMAND messages press its menu items and buttons.

    python -I winshot.py shot  "<title part>" <out.png>
    python -I winshot.py cmd   "<title part>" <command id>          # WM_COMMAND (menu item ids: setup_main.cpp enum Id)
    python -I winshot.py close "<title part>"
    python -I winshot.py list
"""
import ctypes, sys
from ctypes import wintypes

u32, g32 = ctypes.windll.user32, ctypes.windll.gdi32
u32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))   # per-monitor v2: real pixels


def windows():
    out = []
    proc = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

    def cb(h, _):
        if u32.IsWindowVisible(h):
            n = u32.GetWindowTextLengthW(h)
            b = ctypes.create_unicode_buffer(n + 1)
            u32.GetWindowTextW(h, b, n + 1)
            if b.value:
                out.append((h, b.value))
        return True
    u32.EnumWindows(proc(cb), 0)
    return out


def find(part):
    for h, t in windows():
        if part.lower() in t.lower():
            return h
    raise SystemExit(f'no window with "{part}" in its title')


def shot(h, path):
    from PIL import Image
    r = wintypes.RECT()
    u32.GetWindowRect(h, ctypes.byref(r))
    w, ht = r.right - r.left, r.bottom - r.top
    dc = u32.GetWindowDC(h)
    mem = g32.CreateCompatibleDC(dc)
    bmp = g32.CreateCompatibleBitmap(dc, w, ht)
    g32.SelectObject(mem, bmp)
    u32.PrintWindow(h, mem, 2)   # PW_RENDERFULLCONTENT

    class BIH(ctypes.Structure):
        _fields_ = [('biSize', wintypes.DWORD), ('biWidth', wintypes.LONG), ('biHeight', wintypes.LONG), ('biPlanes', wintypes.WORD),
                    ('biBitCount', wintypes.WORD), ('biCompression', wintypes.DWORD), ('biSizeImage', wintypes.DWORD),
                    ('biXPelsPerMeter', wintypes.LONG), ('biYPelsPerMeter', wintypes.LONG), ('biClrUsed', wintypes.DWORD),
                    ('biClrImportant', wintypes.DWORD)]
    bi = BIH(ctypes.sizeof(BIH), w, -ht, 1, 32, 0, 0, 0, 0, 0, 0)
    buf = ctypes.create_string_buffer(w * ht * 4)
    g32.GetDIBits(mem, bmp, 0, ht, buf, ctypes.byref(bi), 0)
    Image.frombuffer('RGBA', (w, ht), buf, 'raw', 'BGRA', 0, 1).convert('RGB').save(path)
    g32.DeleteObject(bmp)
    g32.DeleteDC(mem)
    u32.ReleaseDC(h, dc)
    print(f'{path}: {w}x{ht}')


def main():
    a = sys.argv[1:]
    if not a or a[0] == 'list':
        for h, t in windows():
            print(h, t)
    elif a[0] == 'shot':
        shot(find(a[1]), a[2])
    elif a[0] == 'cmd':
        u32.PostMessageW(find(a[1]), 0x0111, int(a[2]), 0)
    elif a[0] == 'close':
        u32.PostMessageW(find(a[1]), 0x0010, 0, 0)


if __name__ == '__main__':
    main()
