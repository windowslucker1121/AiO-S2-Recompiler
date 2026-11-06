"""Stand-in for the game exe with a real window, for test_gamectl.py (stdlib + ctypes only).

  fake_window.py [--log FILE] [--pos X,Y] [--size W,H] [--topmost] [--exit-after S] [any other game argument, ignored]

Paints its client area one colour (grey; key R / G / B turns it red / green / blue, a click white) and writes what it receives to the log:
its arguments, the NXRT_* / SDL_* environment, keys (vk, scan code, extended bit), characters, clicks (client x y), wheel deltas.
--topmost: a magenta always-on-top window (covers the other one). --exit-after S: exit 0 after S seconds; with NXRT_TEST_OUT set it then
writes result.json and a trace line there, and answers every `@peek IN OUT` line of NXRT_TEST_SCRIPT with 8 bytes, like the runtime.
Save data and bug capture, like the runtime: logs "save data in '<NXRT_FS_SAVE_DIR>'" and "fake: title"; key S saves a slot (committed
file + an uncommitted work folder), Return logs "fake: loaded slot <text>" or "fake: new game"; a request file FAKE_EXE_DIR/capture_bug.<pid>
gets a bundle under NXRT_BUG_DIR (note.txt, env.txt, save/, bug.json last).
"""
import ctypes
import re
import shutil
import time
import ctypes.wintypes as wt
import json
import os
import sys
from pathlib import Path

u32, g32, k32 = ctypes.WinDLL('user32'), ctypes.WinDLL('gdi32'), ctypes.WinDLL('kernel32')
u32.SetProcessDPIAware()
LRESULT = ctypes.c_ssize_t
WNDPROC = ctypes.WINFUNCTYPE(LRESULT, wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM)
u32.DefWindowProcW.argtypes = [wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM]
u32.DefWindowProcW.restype = LRESULT
u32.CreateWindowExW.restype = wt.HWND
u32.CreateWindowExW.argtypes = [wt.DWORD, wt.LPCWSTR, wt.LPCWSTR, wt.DWORD, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                                wt.HWND, wt.HMENU, wt.HINSTANCE, wt.LPVOID]
u32.BeginPaint.restype = wt.HDC
u32.BeginPaint.argtypes = u32.EndPaint.argtypes = [wt.HWND, ctypes.c_void_p]
u32.GetClientRect.argtypes = [wt.HWND, ctypes.POINTER(wt.RECT)]
u32.FillRect.argtypes = [wt.HDC, ctypes.POINTER(wt.RECT), wt.HBRUSH]
u32.InvalidateRect.argtypes = [wt.HWND, ctypes.c_void_p, wt.BOOL]
u32.ShowWindow.argtypes = [wt.HWND, ctypes.c_int]
u32.SetTimer.argtypes = [wt.HWND, ctypes.c_size_t, wt.UINT, ctypes.c_void_p]
g32.CreateSolidBrush.restype = wt.HBRUSH
g32.DeleteObject.argtypes = [wt.HGDIOBJ]


class WNDCLASSW(ctypes.Structure):
    _fields_ = [('style', wt.UINT), ('lpfnWndProc', WNDPROC), ('cbClsExtra', ctypes.c_int), ('cbWndExtra', ctypes.c_int), ('hInstance', wt.HINSTANCE),
                ('hIcon', wt.HICON), ('hCursor', wt.HANDLE), ('hbrBackground', wt.HBRUSH), ('lpszMenuName', wt.LPCWSTR), ('lpszClassName', wt.LPCWSTR)]


class PAINTSTRUCT(ctypes.Structure):
    _fields_ = [('hdc', wt.HDC), ('fErase', wt.BOOL), ('rcPaint', wt.RECT), ('fRestore', wt.BOOL), ('fIncUpdate', wt.BOOL), ('rgbReserved', ctypes.c_byte * 32)]


def arg(name, default=None):
    return sys.argv[sys.argv.index(name) + 1] if name in sys.argv[:-1] else default


log = open(arg('--log', os.devnull), 'a', encoding='utf-8', buffering=1)
log.write(f'argv {sys.argv[1:]}\n')
for k in sorted(os.environ):
    if k.startswith(('NXRT_', 'SDL_')):
        log.write(f'env {k}={os.environ[k]}\n')

SAVE = Path(os.environ.get('NXRT_FS_SAVE_DIR') or Path(os.environ.get('TEMP', '.')) / 'fake_game_save')
SLOT = SAVE / 'account' / 'user' / 'slot.sav'
saves = [0]
log.write(f"fs: fsp-srv: RomFS from 'romfs', save data in '{SAVE}'\n")
log.write('boot: built from commit fake0123 with uncommitted changes\n')
log.write('fake: title\n')


def save_slot():
    saves[0] += 1
    SLOT.parent.mkdir(parents=True, exist_ok=True)
    SLOT.write_text(f'saved {saves[0]}')
    (SAVE / 'account' / 'user.meta').write_text('size=1024\n')
    work = SAVE / 'account' / f'user.work.{os.getpid()}'           # what a running game has besides the committed state
    work.mkdir(parents=True, exist_ok=True)
    (work / 'slot.sav').write_text('uncommitted')
    log.write(f'fake: saved {saves[0]}\n')


def answer_bug_request():
    req = Path(os.environ.get('FAKE_EXE_DIR', '.')) / f'capture_bug.{os.getpid()}'
    if not req.exists():
        return
    text = req.read_text(encoding='utf-8')
    req.unlink()
    opts = dict(re.findall(r'^@(\w+) ?(.*)$', text, re.M))
    note = '\n'.join(ln for ln in text.splitlines() if not ln.startswith('@')).strip()
    d = Path(os.environ['NXRT_BUG_DIR']) / (time.strftime('%Y%m%d-%H%M%S') + (f'-{opts["name"]}' if opts.get('name') else ''))
    d.mkdir(parents=True)
    log.write(f'bug: capture {d} ({opts.get("origin", "request")})\n')
    if note:
        (d / 'note.txt').write_text(note + '\n')
    (d / 'env.txt').write_text(''.join(f'{k}={v}\n' for k, v in os.environ.items() if k.startswith(('NXRT_', 'SDL_'))))
    if SAVE.exists():
        shutil.copytree(SAVE, d / 'save', ignore=shutil.ignore_patterns('*.work.*'))
    (d / 'bug.json').write_text(json.dumps({'format': 1, 'complete': True, 'origin': opts.get('origin', 'request'),
                                            'state': os.environ.get('NXRT_AI_STATE', ''), 'trace_frame': 'trace' in opts,
                                            'build': {'commit': 'fake0123', 'exe_sha256': ''}}))
    log.write(f'bug: bundle complete {d}\n')


topmost = '--topmost' in sys.argv
colour = [0xFF00FF if topmost else 0x404040]          # COLORREF 0x00BBGGRR
KEY_COLOURS = {ord('R'): 0x0000FF, ord('G'): 0x00FF00, ord('B'): 0xFF0000}


def finish():
    out = os.environ.get('NXRT_TEST_OUT')
    if out and os.environ.get('NXRT_TEST_SCRIPT'):
        for line in Path(os.environ['NXRT_TEST_SCRIPT']).read_text().splitlines():
            log.write(f'script {line}\n')
            if line.startswith('@peek '):
                Path(line.split()[2]).write_text('0102030405060708\n')
        (Path(out) / 'result.json').write_text(json.dumps({'verdict': 'pass', 'exit_code': 0, 'frames': 1}))
        (Path(out) / 'trace.jsonl').write_text('{"trace":1,"source":"fake"}\n{"f":0,"h":"0"}\n')
    u32.PostQuitMessage(0)


@WNDPROC
def wndproc(h, m, wp, lp):
    if m == 0x000F:                                      # WM_PAINT
        ps = PAINTSTRUCT()
        dc = u32.BeginPaint(h, ctypes.byref(ps))
        r = wt.RECT()
        u32.GetClientRect(h, ctypes.byref(r))
        br = g32.CreateSolidBrush(colour[0])
        u32.FillRect(dc, ctypes.byref(r), br)
        g32.DeleteObject(br)
        u32.EndPaint(h, ctypes.byref(ps))
        return 0
    if m in (0x0100, 0x0101, 0x0104, 0x0105):           # WM_(SYS)KEYDOWN / UP
        log.write(f'key{"down" if m in (0x100, 0x104) else "up"} vk=0x{wp:02x} sc=0x{(lp >> 16) & 0xFF:02x} ext={(lp >> 24) & 1}\n')
        if m == 0x0100 and wp == ord('S'):
            save_slot()
        if m == 0x0100 and wp == 0x0D:
            log.write(f'fake: loaded slot {SLOT.read_text()}\n' if SLOT.exists() else 'fake: new game\n')
        if m == 0x0100 and wp in KEY_COLOURS:
            colour[0] = KEY_COLOURS[wp]
            u32.InvalidateRect(h, None, True)
        return 0
    if m == 0x0102:                                      # WM_CHAR
        log.write(f'char {chr(wp)}\n')
        return 0
    if m in (0x0201, 0x0204, 0x0207):                    # WM_L/R/MBUTTONDOWN
        x, y = ctypes.c_short(lp & 0xFFFF).value, ctypes.c_short((lp >> 16) & 0xFFFF).value
        button = {0x201: 'l', 0x204: 'r', 0x207: 'm'}[m]
        log.write(f'{button}down {x} {y}\n')
        colour[0] = 0xFFFFFF
        u32.InvalidateRect(h, None, True)
        return 0
    if m == 0x020A:                                      # WM_MOUSEWHEEL
        log.write(f'wheel {ctypes.c_short((wp >> 16) & 0xFFFF).value}\n')
        return 0
    if m == 0x0113:                                      # WM_TIMER
        if wp == 2:
            answer_bug_request()
        else:
            finish()
        return 0
    if m == 0x0002:                                      # WM_DESTROY
        u32.PostQuitMessage(0)
        return 0
    return u32.DefWindowProcW(h, m, wp, lp)


def main():
    inst = k32.GetModuleHandleW(None)
    wc = WNDCLASSW(lpfnWndProc=wndproc, hInstance=inst, lpszClassName='fake_game_window', hCursor=u32.LoadCursorW(None, 32512))
    u32.RegisterClassW(ctypes.byref(wc))
    x, y = (int(v) for v in arg('--pos', '60,60').split(','))
    w, h = (int(v) for v in arg('--size', '320,240').split(','))
    style, ex = 0x00CF0000, (0x08000008 if topmost else 0)          # WS_OVERLAPPEDWINDOW; WS_EX_NOACTIVATE | WS_EX_TOPMOST
    r = wt.RECT(0, 0, w, h)
    u32.AdjustWindowRectEx(ctypes.byref(r), style, False, ex)
    hwnd = u32.CreateWindowExW(ex, 'fake_game_window', 'fake cover' if topmost else 'fake game window', style, x, y, r.right - r.left,
                               r.bottom - r.top, None, None, inst, None)
    u32.ShowWindow(hwnd, 4)                                          # SW_SHOWNOACTIVATE
    if (s := arg('--exit-after')) is not None:
        u32.SetTimer(hwnd, 1, int(float(s) * 1000), None)
    if not topmost:
        u32.SetTimer(hwnd, 2, 100, None)                             # bug capture request files
    msg = wt.MSG()
    while u32.GetMessageW(ctypes.byref(msg), None, 0, 0) > 0:
        u32.TranslateMessage(ctypes.byref(msg))
        u32.DispatchMessageW(ctypes.byref(msg))
    return 0


if __name__ == '__main__':
    sys.exit(main())
