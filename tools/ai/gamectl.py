"""Drive the game exe from an AI agent: process, window, screenshots, input, log, crash reports, scripted runs.

Game independent: the exe and log names come from recomp/games/<id>/game.toml (id: NXRT_AI_GAME, default oot). The exe is NXRT_AI_EXE or
game-build/build/release/<exe>; a run's files (log, stdout/stderr, test output, screenshots) go to NXRT_AI_RUN_DIR or work/ai_run/<id>.
mcp_server.py serves every name in TOOLS (methods of Game, the docstring is the tool description) over MCP and as a CLI.

The running exe has no live control channel (no pipe, socket or command file). The runtime's test tooling (scripts, pad and vpad input,
@peek/@poke, game probes) is configured at start through NXRT_TEST_* and answers through files (result.json, trace.jsonl, shots/), so it is
reached through launch(script=...). Live control is window messages or SendInput to the game window.
Saved states and bug bundles (states, save_state, load_state, capture_bug, bugs) are in states.py, a mixin of Game.
"""
import collections
import ctypes
import ctypes.wintypes as wt
import io
import json
import os
import re
import shutil
import subprocess
import sys
import time
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools' / 'test'))
import imgdiff  # noqa: E402  (tools/test: perceptual image diff)
import wincap  # noqa: E402  (tools/test: window enumeration and capture; makes this process DPI aware)
from PIL import Image  # noqa: E402

sys.path.insert(0, str(Path(__file__).resolve().parent))
import states  # noqa: E402  (tools/ai/states.py: saved states and bug bundles)

TOOLS = ['status', 'launch', 'restart', 'kill', 'screenshot', 'send_input', 'wait_for', 'compare', 'log', 'reports',
         'states', 'save_state', 'load_state', 'capture_bug', 'bugs']
# list parameters whose items are not only strings (route names or step dicts): the MCP schema leaves their items open
ANY_ITEMS = {'reach', 'route'}
# The game exe's exit codes (game_main.cpp, the test tooling's contract in tt.h, crash handler, watchdog, fatal).
EXIT = {0: 'pass / normal exit', 1: 'script failed or start-up error', 2: 'usage or script parse error', 3: 'crash', 4: 'crash',
        5: 'freeze or fatal error', 6: 'process ended before the script finished', 124: 'deadline (--max-seconds / wall_timeout_s)'}
LOG_LEVEL = re.compile(r'^\[\s*[\d.]+ ([EW]) ')       # runtime log prefix "[ 12.345678 E t1234 name] "

# ---- Win32 -------------------------------------------------------------------------------------------------------------------------------
u32 = ctypes.WinDLL('user32', use_last_error=True)    # own instances: argtypes here do not leak into wincap's calls
k32 = ctypes.WinDLL('kernel32', use_last_error=True)


def _decl(dll, name, res, *args):
    f = getattr(dll, name)
    f.restype, f.argtypes = res, args
    return f


class PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [('dwSize', wt.DWORD), ('cntUsage', wt.DWORD), ('th32ProcessID', wt.DWORD), ('th32DefaultHeapID', ctypes.c_size_t),
                ('th32ModuleID', wt.DWORD), ('cntThreads', wt.DWORD), ('th32ParentProcessID', wt.DWORD), ('pcPriClassBase', ctypes.c_long),
                ('dwFlags', wt.DWORD), ('szExeFile', wt.WCHAR * 260)]


class PMC(ctypes.Structure):     # PROCESS_MEMORY_COUNTERS_EX
    _fields_ = [('cb', wt.DWORD), ('PageFaultCount', wt.DWORD)] + [(n, ctypes.c_size_t) for n in (
        'PeakWorkingSetSize', 'WorkingSetSize', 'QuotaPeakPagedPoolUsage', 'QuotaPagedPoolUsage', 'QuotaPeakNonPagedPoolUsage',
        'QuotaNonPagedPoolUsage', 'PagefileUsage', 'PeakPagefileUsage', 'PrivateUsage')]


class KEYBDINPUT(ctypes.Structure):
    _fields_ = [('wVk', wt.WORD), ('wScan', wt.WORD), ('dwFlags', wt.DWORD), ('time', wt.DWORD), ('dwExtraInfo', ctypes.c_size_t)]


class MOUSEINPUT(ctypes.Structure):
    _fields_ = [('dx', wt.LONG), ('dy', wt.LONG), ('mouseData', wt.DWORD), ('dwFlags', wt.DWORD), ('time', wt.DWORD), ('dwExtraInfo', ctypes.c_size_t)]


class INPUT(ctypes.Structure):
    class _U(ctypes.Union):
        _fields_ = [('ki', KEYBDINPUT), ('mi', MOUSEINPUT)]
    _anonymous_ = ('u',)
    _fields_ = [('type', wt.DWORD), ('u', _U)]


H = wt.HANDLE
Snapshot = _decl(k32, 'CreateToolhelp32Snapshot', H, wt.DWORD, wt.DWORD)
ProcFirst = _decl(k32, 'Process32FirstW', wt.BOOL, H, ctypes.POINTER(PROCESSENTRY32W))
ProcNext = _decl(k32, 'Process32NextW', wt.BOOL, H, ctypes.POINTER(PROCESSENTRY32W))
CloseHandle = _decl(k32, 'CloseHandle', wt.BOOL, H)
OpenProcess = _decl(k32, 'OpenProcess', H, wt.DWORD, wt.BOOL, wt.DWORD)
ImageName = _decl(k32, 'QueryFullProcessImageNameW', wt.BOOL, H, wt.DWORD, wt.LPWSTR, ctypes.POINTER(wt.DWORD))
ProcTimes = _decl(k32, 'GetProcessTimes', wt.BOOL, H, *[ctypes.POINTER(wt.FILETIME)] * 4)
ProcMem = _decl(k32, 'K32GetProcessMemoryInfo', wt.BOOL, H, ctypes.POINTER(PMC), wt.DWORD)
PostMessage = _decl(u32, 'PostMessageW', wt.BOOL, wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM)
Foreground = _decl(u32, 'GetForegroundWindow', wt.HWND)
SetForeground = _decl(u32, 'SetForegroundWindow', wt.BOOL, wt.HWND)
ToTop = _decl(u32, 'BringWindowToTop', wt.BOOL, wt.HWND)
ShowWindow = _decl(u32, 'ShowWindow', wt.BOOL, wt.HWND, ctypes.c_int)
IsIconic = _decl(u32, 'IsIconic', wt.BOOL, wt.HWND)
IsHung = _decl(u32, 'IsHungAppWindow', wt.BOOL, wt.HWND)
WindowThread = _decl(u32, 'GetWindowThreadProcessId', wt.DWORD, wt.HWND, ctypes.POINTER(wt.DWORD))
AttachInput = _decl(u32, 'AttachThreadInput', wt.BOOL, wt.DWORD, wt.DWORD, wt.BOOL)
MapVk = _decl(u32, 'MapVirtualKeyW', wt.UINT, wt.UINT, wt.UINT)
VkScan = _decl(u32, 'VkKeyScanW', ctypes.c_short, wt.WCHAR)
ClientToScreen = _decl(u32, 'ClientToScreen', wt.BOOL, wt.HWND, ctypes.POINTER(wt.POINT))
SetCursorPos = _decl(u32, 'SetCursorPos', wt.BOOL, ctypes.c_int, ctypes.c_int)
SendInput = _decl(u32, 'SendInput', wt.UINT, wt.UINT, ctypes.POINTER(INPUT), ctypes.c_int)


def processes():
    """[(pid, parent pid, exe file name)] of every process."""
    snap, e, out = Snapshot(2, 0), PROCESSENTRY32W(), []     # TH32CS_SNAPPROCESS
    e.dwSize = ctypes.sizeof(e)
    ok = ProcFirst(snap, ctypes.byref(e))
    while ok:
        out.append((e.th32ProcessID, e.th32ParentProcessID, e.szExeFile))
        ok = ProcNext(snap, ctypes.byref(e))
    CloseHandle(snap)
    return out


def proc_info(pid):
    h = OpenProcess(0x1000 | 0x0010, False, pid)               # PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ
    if not h:
        return {'pid': pid}
    try:
        buf, n = ctypes.create_unicode_buffer(32768), wt.DWORD(32768)
        ft = [wt.FILETIME() for _ in range(4)]
        ProcTimes(h, *[ctypes.byref(f) for f in ft])
        sec = lambda f: (f.dwHighDateTime << 32 | f.dwLowDateTime) / 1e7     # noqa: E731  (100 ns units)
        m = PMC()
        m.cb = ctypes.sizeof(m)
        ProcMem(h, ctypes.byref(m), m.cb)
        return {'pid': pid, 'path': buf.value if ImageName(h, 0, buf, ctypes.byref(n)) else None,
                'uptime_s': round(time.time() + 11644473600 - sec(ft[0]), 1), 'cpu_s': round(sec(ft[2]) + sec(ft[3]), 1),
                'working_set_mb': m.WorkingSetSize >> 20, 'private_mb': m.PrivateUsage >> 20}
    finally:
        CloseHandle(h)


VK = {'return': 0x0D, 'enter': 0x0D, 'escape': 0x1B, 'esc': 0x1B, 'space': 0x20, 'tab': 0x09, 'backspace': 0x08, 'shift': 0x10, 'lshift': 0xA0,
      'rshift': 0xA1, 'control': 0x11, 'ctrl': 0x11, 'lctrl': 0xA2, 'rctrl': 0xA3, 'alt': 0x12, 'menu': 0x12, 'lalt': 0xA4, 'ralt': 0xA5,
      'left': 0x25, 'up': 0x26, 'right': 0x27, 'down': 0x28, 'insert': 0x2D, 'delete': 0x2E, 'home': 0x24, 'end': 0x23, 'pageup': 0x21,
      'pagedown': 0x22, 'pause': 0x13, 'capslock': 0x14, 'lwin': 0x5B}


def vk_code(name):
    n = name.lower().replace(' ', '').replace('_', '')
    if n in VK:
        return VK[n]
    if re.fullmatch(r'f([1-9]|1\d|2[0-4])', n):
        return 0x6F + int(n[1:])
    if re.fullmatch(r'numpad\d', n):
        return 0x60 + int(n[6])
    if n.startswith('0x'):
        return int(n, 16)
    if len(name) == 1 and (v := VkScan(name) & 0xFF) != 0xFF:
        return v
    raise ValueError(f'unknown key {name!r}')


# Extended keys (scan code prefix E0): page/arrow/insert/delete block, Windows and menu keys, numpad divide, num lock, right ctrl and alt.
# MapVirtualKeyW(MAPVK_VK_TO_VSC_EX) does not report them reliably (VK_UP came back as plain 0x48 = numpad 8).
EXTENDED = {*range(0x21, 0x29), 0x2C, 0x2D, 0x2E, 0x5B, 0x5C, 0x5D, 0x6F, 0x90, 0xA3, 0xA5}


def scan_code(vk):
    return MapVk(vk, 0) & 0xFF, int(vk in EXTENDED)               # MAPVK_VK_TO_VSC


def focus(hwnd):
    """Bring the window to the front; True when it is the foreground window afterwards."""
    if IsIconic(hwnd):
        ShowWindow(hwnd, 9)                                      # SW_RESTORE
    fg = Foreground()
    if fg != hwnd:
        me, them = k32.GetCurrentThreadId(), WindowThread(fg, None) if fg else 0
        AttachInput(me, them, True)
        ToTop(hwnd)
        SetForeground(hwnd)
        AttachInput(me, them, False)
        time.sleep(0.15)
    return Foreground() == hwnd


def _read_from(path, pos):
    """Complete lines written after byte `pos`, and the position after them."""
    with open(path, 'rb') as f:
        f.seek(pos)
        data = f.read()
    cut = data.rfind(b'\n') + 1
    return data[:cut].decode('utf-8', 'replace').splitlines(), pos + cut


def _tail(path, n, max_bytes=1 << 20):
    size = path.stat().st_size
    lines = _read_from(path, max(0, size - max_bytes))[0]
    return lines[1 if size > max_bytes else 0:][-n:]            # the first line of a cut read is partial


def _xy(tx, ty, size):
    """Client pixels, or fractions of the client area when written with a decimal point."""
    return [min(int(float(t) * n), n - 1) if '.' in t else int(t) for t, n in zip((tx, ty), size)]


class Game(states.StateTools):
    def __init__(self, game_id=None, exe=None, run_dir=None):
        self.id = game_id or os.environ.get('NXRT_AI_GAME', 'oot')
        manifest = tomllib.loads((ROOT / 'recomp' / 'games' / self.id / 'game.toml').read_text(encoding='utf-8'))
        files = manifest['files']
        self.log_name = files['log']
        self.exe = Path(exe or os.environ.get('NXRT_AI_EXE') or ROOT / 'game-build' / 'build' / 'release' / files['exe']).resolve()
        self.run_dir = Path(run_dir or os.environ.get('NXRT_AI_RUN_DIR') or ROOT / 'work' / 'ai_run' / self.id).resolve()
        self.proc = self.launch_t = self.log_path = self.last_launch = None
        self.log_pos, self.peeks = 0, []

    # ---- finding the game ----------------------------------------------------------------------------------------------------------------
    def _pids(self):
        """The game's processes: the tree this object started while it runs, else every process with the exe's file name."""
        procs = processes()
        if self.proc and self.proc.poll() is None:
            pids = {self.proc.pid}
            while more := {p for p, pp, _ in procs if pp in pids} - pids:
                pids |= more
            return sorted(pids)
        return [p for p, _, n in procs if n.lower() == self.exe.name.lower()]

    def _window(self, required=True):
        """(hwnd, title, pid) of the largest visible top-level window of the game's processes."""
        area = lambda h: wincap.client_size(h)[0] * wincap.client_size(h)[1]     # noqa: E731
        best = max(((area(h), h, t, p) for pid in self._pids() for h, t, p in wincap.find_windows(pid=pid)), default=None)
        if best is None and required:
            raise RuntimeError('no game window: not running, still starting, or headless (headless runs take screenshots with the script step @shot)')
        return best[1:] if best else None

    def _capture(self, method=None):
        w = self._window()
        img, how = wincap.capture_best(w[0], method)
        return img, how, w

    def _shot_path(self, kind):
        (self.run_dir / 'shots').mkdir(parents=True, exist_ok=True)
        t = time.time()
        return self.run_dir / 'shots' / f'{kind}_{time.strftime("%H%M%S", time.localtime(t))}_{int(t * 1000) % 1000:03d}.png'

    def _log_path(self):
        if self.log_path:
            return self.log_path
        found = [p for p in (self.run_dir / self.log_name, self.exe.parent / self.log_name) if p.exists()]
        return max(found, key=lambda p: p.stat().st_mtime) if found else self.run_dir / self.log_name

    def _report_files(self):
        return [p for p in [self.exe.parent / f'{self.exe.stem}_{k}.txt' for k in ('crash', 'freeze', 'fatal')]
                + sorted((self.run_dir / 'testout').glob('*.txt')) if p.exists()]

    def _test_results(self):
        out, r = self.run_dir / 'testout', {}
        if (out / 'result.json').exists():
            r['result'] = json.loads((out / 'result.json').read_text(encoding='utf-8'))
        if (out / 'trace.jsonl').exists():
            r['trace_tail'] = _tail(out / 'trace.jsonl', 3)
        if shots := sorted(str(p) for p in (out / 'shots').glob('*.png')):
            r['shots'] = shots
        if self.peeks:
            r['peeks'] = [{'step': s, 'hex': f.read_text().strip() if f and f.exists() else None} for s, f in self.peeks]
        return r

    # ---- tools ------------------------------------------------------------------------------------------------------------------------------
    def status(self):
        """One-call overview: exe (path, build time), processes (pid, uptime, CPU seconds, memory), the exit code when the game this server
        started has ended, the window (title, client size, focused, minimised, hung = not answering messages), the log (path, seconds since
        the last write, errors and warnings among the last 400 lines, last lines), which crash/freeze/fatal reports exist, and the
        scripted-run result and last trace lines (the game probes' state) when the test tooling is on."""
        pids, win = self._pids(), self._window(required=False)
        s = {'game': self.id, 'exe': str(self.exe), 'exe_built': time.ctime(self.exe.stat().st_mtime) if self.exe.exists() else 'missing',
             'running': bool(pids), 'processes': [proc_info(p) for p in pids]}
        if self.proc and self.proc.poll() is not None:
            s['exit_code'] = self.proc.returncode
            s['exit_meaning'] = EXIT.get(self.proc.returncode, 'unexpected')
        if win:
            h = win[0]
            s['window'] = {'title': win[1], 'size': wincap.client_size(h), 'focused': Foreground() == h, 'minimized': bool(IsIconic(h)),
                           'hung': bool(IsHung(h))}
        lp = self._log_path()
        if lp.exists():
            lines = _tail(lp, 400)
            levels = collections.Counter(m.group(1) for m in map(LOG_LEVEL.match, lines) if m)
            s['log'] = {'path': str(lp), 'idle_s': round(time.time() - lp.stat().st_mtime, 1), 'recent_errors': levels['E'],
                        'recent_warnings': levels['W'], 'last': [ln[:300] for ln in lines[-8:]]}
        s['reports'] = [str(p) for p in self._report_files()]
        if t := self._test_results():
            s['test'] = t
        return s

    def launch(self, args: list = None, settings: dict = None, env: dict = None, script: list = None, headless: bool = False,
               wait: bool = False, timeout_s: float = 600, exe: str = None, cwd: str = None):
        """(Re)start the game; a running instance is killed first. The exe gets `--log <run dir>/<log>` (unless args has --log),
        `--headless` when asked, then args, e.g. ["--max-seconds", "60", "--exefs", "D:/dump/exefs", "--ini", "x.ini"]. The window is asked
        to open without taking the focus. cwd defaults to the run dir (an installed exe changes to its own folder).
        settings: runtime config keys {"section.key": value} passed as NXRT_<SECTION>_<KEY> (the environment beats the ini; keys in
        runtime/docs/CONFIG.md), e.g. {"log.level": "debug", "diag.watchdog_seconds": 0, "test.enabled": 1}. env: more environment variables.
        script: lines of a test-tooling script (runtime/include/nxrt/tt_script.h: press/hold/stick/key/mouse, "keydown vpad ..." for the
        virtual SDL pad, @shot/@hash/@assert/@log/@sleep/@pass/...). Two shorthands read and write guest memory and come back in the result:
        `peek <addr> <len> [off ...]` and `poke <addr> [off ...] : <hexbytes>`.
        With a script or test.enabled the test output (result.json, trace.jsonl = per-frame state from the game probes, shots/) is in
        <run dir>/testout; test.frame_source defaults to tick (the present path, needs no game hooks; set "test.frame_source" to change it).
        wait: block until the game exits (killed after timeout_s) and return exit code, scripted-run result, peeks and the log tail."""
        self.last_launch = {k: v for k, v in locals().items() if k != 'self'}
        self.kill()
        self.run_dir.mkdir(parents=True, exist_ok=True)
        (self.run_dir / 'launch.json').write_text(json.dumps(self.last_launch, indent=1, default=str), encoding='utf-8')
        if exe:
            self.exe = Path(exe).resolve()
        if not self.exe.exists():
            raise RuntimeError(f'{self.exe} not found: build the game exe, or pass exe= (or set NXRT_AI_EXE)')
        rd, args = self.run_dir, [str(a) for a in args or []]
        out = rd / 'testout'
        shutil.rmtree(out, ignore_errors=True)                  # this server's own output of the previous run
        out.mkdir(parents=True)
        if '--log' in args:
            log = Path(cwd or rd, args[args.index('--log') + 1])
        else:
            log = rd / self.log_name
            args = ['--log', str(log)] + args
        if log.exists():
            os.replace(log, log.with_name(log.name + '.prev'))
        if headless:
            args.append('--headless')
        e = dict(os.environ, SDL_WINDOW_ACTIVATE_WHEN_SHOWN='0', NXRT_TEST_OUT=str(out), NXRT_TEST_FRAME_SOURCE='tick',
                 NXRT_BUG_DIR=str(self._bugs_dir()))       # bug bundles where bugs() looks (settings {"bug.dir": ...} beats it)
        for k, v in (settings or {}).items():                    # CONFIG.md: NXRT_ + SECTION + _ + KEY, non-alphanumerics as _
            e['NXRT_' + re.sub(r'\W', '_', k.upper().replace('.', '_', 1))] = ('1' if v else '0') if isinstance(v, bool) else str(v)
        self.peeks = []
        if script:
            e['NXRT_TEST_SCRIPT'] = str(self._write_script(script))
        e.update({k: str(v) for k, v in (env or {}).items()})
        si = subprocess.STARTUPINFO(dwFlags=subprocess.STARTF_USESHOWWINDOW, wShowWindow=4)    # SW_SHOWNOACTIVATE
        with open(rd / 'stdout.txt', 'wb') as so, open(rd / 'stderr.txt', 'wb') as se:
            self.proc = subprocess.Popen([str(self.exe)] + args, cwd=cwd or rd, env=e, stdout=so, stderr=se, startupinfo=si,
                                         creationflags=subprocess.CREATE_NO_WINDOW)
        self.launch_t, self.log_path, self.log_pos = time.time() - 1, log, 0
        if not wait:
            return {'pid': self.proc.pid, 'cmd': [str(self.exe)] + args, 'log': str(log), 'run_dir': str(rd)}
        try:
            rc = self.proc.wait(timeout_s)
        except subprocess.TimeoutExpired:
            self.kill()
            rc = None
        return {'exit_code': rc, 'meaning': EXIT.get(rc, 'killed after timeout_s' if rc is None else 'unexpected'), **self._test_results(),
                'reports': [str(p) for p in self._report_files()], 'log_tail': self.log(30).get('lines')}

    def _write_script(self, lines):
        rd = self.run_dir
        if ' ' in str(rd):
            raise RuntimeError(f'run dir {rd} has a space: @peek/@poke file arguments are space separated (set NXRT_AI_RUN_DIR)')
        out = []
        for ln in lines:
            w = ln.split(None, 1)
            kw = w[0].lower() if w else ''
            if kw in ('peek', 'poke') and len(w) == 2:
                n = len(self.peeks)
                fin, fout = rd / f'{kw}{n}.in', rd / f'peek{n}.out' if kw == 'peek' else None
                fin.write_text(w[1] + '\n', encoding='ascii')
                if fout and fout.exists():
                    fout.unlink()
                out.append(f'@peek {fin} {fout}' if fout else f'@poke {fin}')
                self.peeks.append((ln, fout))
            else:
                out.append(ln)
        p = rd / 'script.nxs'
        p.write_text('\n'.join(out) + '\n', encoding='utf-8')
        return p

    def restart(self):
        """Kill the game and start it again with the arguments of the last launch (also of one made by an earlier CLI call: launch.json)."""
        last = self._last_launch()
        if not last:
            raise RuntimeError('nothing launched yet: use launch')
        return self.launch(**last)

    def kill(self):
        """End the game: the process tree this server started, or every running process with the exe's file name."""
        pids = self._pids()
        for pid in pids:
            subprocess.run(['taskkill', '/F', '/T', '/PID', str(pid)], capture_output=True, creationflags=subprocess.CREATE_NO_WINDOW)
        if self.proc:
            try:
                self.proc.wait(10)
            except subprocess.TimeoutExpired:
                pass
        for _ in range(50):
            if not self._pids():
                break
            time.sleep(0.1)
        return {'killed': pids, 'exit_code': self.proc.returncode if self.proc else None}

    def screenshot(self, show: bool = True, crop: str = None, save: str = None, method: str = None):
        """Capture the game window's client area: works while it is covered or unfocused, not while minimised. Saves a full-size PNG
        (path in the result) and, with show, returns the picture (at most 1280 px wide). crop: "x0,y0,x1,y1" in client pixels, or fractions
        when written with decimal points ("0,0,0.5,0.5" = top-left quarter). method: printwindow | bitblt | screen (default printwindow,
        falling back to the others when the picture is black)."""
        img, how, w = self._capture(method)
        if crop:
            img = img.crop(_xy(*crop.split(',')[:2], img.size) + _xy(*crop.split(',')[2:], img.size))
        path = Path(save) if save else self._shot_path('shot')
        img.save(path)
        info = {'path': str(path), 'size': img.size, 'method': how, 'black': wincap.is_black(img), 'title': w[1]}
        if not show:
            return info
        view = img.copy()
        view.thumbnail((1280, 1280))
        buf = io.BytesIO()
        view.save(buf, 'JPEG', quality=85)
        return buf.getvalue(), info

    def send_input(self, steps: list, method: str = 'post'):
        """Keyboard and mouse input to the game window. steps, run in order:
          key <name> [hold_ms]   press and release (hold 60 ms)     down <name> / up <name>     text <characters>
          click|rclick|mclick <x> <y> [hold_ms]     move <x> <y>     wheel <notches> [<x> <y>]     rel <dx> <dy>     wait <ms>
        <name>: a character (a, 7, /), F1..F24, numpad0..9, return, escape, space, tab, backspace, shift, ctrl, alt, left, up, right, down,
        insert, delete, home, end, pageup, pagedown (also l/r variants: lshift, rctrl, ...), or a virtual-key code 0xNN.
        <x> <y>: client pixels, or fractions when written with a decimal point ("click 0.5 0.5" = the centre).
        method post: window messages to the game window only; works covered and unfocused and never touches another window, but cannot
        make raw mouse motion (rel) and a game reading the keyboard state directly will not see it.
        method send: SendInput after bringing the game window to the front: the real input path (raw mouse for camera look); refused,
        sending nothing, when the game window is not in front."""
        hwnd, title, _ = self._window()
        send = method == 'send'
        if send and not focus(hwnd):
            raise RuntimeError('the game window could not be brought to the front; nothing was sent')
        size = wincap.client_size(hwnd)

        def put(*inputs):
            if Foreground() != hwnd:
                raise RuntimeError('the game window lost the front; input stopped')
            arr = (INPUT * len(inputs))(*inputs)
            SendInput(len(inputs), arr, ctypes.sizeof(INPUT))

        def key(vk, down):
            sc, ext = scan_code(vk)
            if send:
                return put(INPUT(type=1, ki=KEYBDINPUT(0, sc, 0x8 | ext | (0 if down else 0x2), 0, 0)))    # KEYEVENTF_SCANCODE
            alt = vk in (0x12, 0xA4, 0xA5)
            PostMessage(hwnd, (0x104 if alt else 0x100) + (0 if down else 1), vk, 1 | sc << 16 | ext << 24 | (0 if down else 0xC0000000))

        def mouse(x, y, flags=0, data=0, msg=None, wparam=0):
            if send:
                p = wt.POINT(x, y)
                ClientToScreen(hwnd, ctypes.byref(p))
                if Foreground() != hwnd:
                    raise RuntimeError('the game window lost the front; input stopped')
                SetCursorPos(p.x, p.y)
                if flags:
                    put(INPUT(type=0, mi=MOUSEINPUT(0, 0, data & 0xFFFFFFFF, flags, 0, 0)))
            else:
                PostMessage(hwnd, msg, wparam, (y & 0xFFFF) << 16 | x & 0xFFFF)

        for st in steps:
            t = st.split()
            op, a = t[0].lower(), t[1:]
            if op == 'wait':
                time.sleep(float(a[0]) / 1000)
            elif op in ('key', 'down', 'up'):
                hold = 60
                if op == 'key' and len(a) > 1 and a[-1].isdigit():
                    a, hold = a[:-1], int(a[-1])
                vk = vk_code(' '.join(a))
                if op != 'up':
                    key(vk, True)
                if op == 'key':
                    time.sleep(hold / 1000)
                if op != 'down':
                    key(vk, False)
            elif op == 'text':
                for ch in st.split(None, 1)[1]:
                    if send:
                        put(*(INPUT(type=1, ki=KEYBDINPUT(0, ord(ch), 0x4 | up, 0, 0)) for up in (0, 0x2)))    # KEYEVENTF_UNICODE
                    else:
                        PostMessage(hwnd, 0x102, ord(ch), 1)        # WM_CHAR
            elif op in ('click', 'rclick', 'mclick'):
                x, y = _xy(a[0], a[1], size)
                b = 'lrm'['crm'.index(op[0])]
                down_flag, mk, msg = {'l': (0x2, 0x1, 0x201), 'r': (0x8, 0x2, 0x204), 'm': (0x20, 0x10, 0x207)}[b]
                mouse(x, y, msg=0x200)
                mouse(x, y, down_flag, msg=msg, wparam=mk)
                time.sleep((int(a[2]) if len(a) > 2 else 50) / 1000)
                mouse(x, y, down_flag << 1, msg=msg + 1)
            elif op == 'move':
                mouse(*_xy(a[0], a[1], size), msg=0x200)
            elif op == 'wheel':
                x, y = _xy(a[1], a[2], size) if len(a) > 2 else (size[0] // 2, size[1] // 2)
                delta = int(a[0]) * 120
                if send:
                    mouse(x, y, 0x800, delta)
                else:
                    p = wt.POINT(x, y)
                    ClientToScreen(hwnd, ctypes.byref(p))
                    PostMessage(hwnd, 0x20A, (delta & 0xFFFF) << 16, (p.y & 0xFFFF) << 16 | p.x & 0xFFFF)
            elif op == 'rel':
                if not send:
                    raise ValueError('rel (raw mouse motion) needs method=send')
                put(INPUT(type=0, mi=MOUSEINPUT(int(a[0]), int(a[1]), 0, 0x1, 0, 0)))
            else:
                raise ValueError(f'unknown step {st!r}')
        return {'window': title, 'steps': len(steps), 'method': method}

    def wait_for(self, until: str, timeout_s: float = 60, pattern: str = None, ref: str = None, min_ssim: float = 0.9, mask: str = None,
                 new: bool = False):
        """Block until a condition holds or timeout_s runs out. until:
          window   the game window exists            visible   the window shows a picture that is not black
          change   the picture differs from the one at the call
          image    the picture matches the reference PNG ref (SSIM >= min_ssim; mask as in compare)
          log      a line of this run's log matches the regex pattern (case-insensitive); new: only lines written after this call
          exit     the game process has ended
        Returns ok, the seconds waited, the matching log line or the last capture's path; stops early when the game is not running."""
        if until not in ('window', 'visible', 'change', 'image', 'log', 'exit'):
            raise ValueError(f'unknown condition {until!r}')
        rx = re.compile(pattern or '.', re.I)
        lp, base, img, t0 = self._log_path(), None, None, time.time()
        pos = lp.stat().st_size if new and lp.exists() else 0
        masks = [imgdiff._mask_arg(mask)] if mask else None
        while True:
            alive = bool(self._pids())
            hit = None
            if until == 'exit':
                hit = not alive
            elif until == 'window':
                hit = self._window(required=False) is not None
            elif until == 'log':
                if lp.exists():
                    lines, pos = _read_from(lp, pos)
                    hit = next((ln for ln in lines if rx.search(ln)), None)
            elif (w := self._window(required=False)) is not None:
                img = wincap.capture_best(w[0])[0]
                if until == 'visible':
                    hit = not wincap.is_black(img)
                elif until == 'change':
                    base = img if base is None else base
                    hit = imgdiff.compare(base, img, metrics=('changed_frac',)).changed_frac > 0.002   # ponytail: fixed threshold, a parameter if games need one
                else:
                    hit = imgdiff.compare(ref, img, masks=masks, metrics=('ssim',)).ssim >= min_ssim
            done = bool(hit) or time.time() - t0 > timeout_s or (not alive and until != 'exit')
            if done:
                r = {'ok': bool(hit), 'waited_s': round(time.time() - t0, 1)}
                if not alive and not hit:
                    r['why'] = 'the game is not running' + (f' (exit code {self.proc.returncode})' if self.proc and self.proc.poll() is not None else '')
                if isinstance(hit, str):
                    r['line'] = hit
                if img is not None:
                    img.save(p := self._shot_path(until))
                    r['shot'] = str(p)
                return r
            time.sleep(0.3)

    def compare(self, a: str, b: str = None, mask: str = None, min_ssim: float = 0.9):
        """Perceptual diff (tools/test/imgdiff.py: SSIM on luma, mean absolute error, histogram correlation, fraction of changed pixels) of
        two PNGs, or of the reference PNG a and the game window now (b omitted). mask hides HUD/animated parts: "hud", "x0,y0,x1,y1"
        (fractions), or a mask PNG. passed = every metric within imgdiff's defaults, SSIM >= min_ssim. Writes a diff picture (red = changed)."""
        if b is None:
            img = self._capture()[0]
            img.save(b := str(self._shot_path('now')))
        masks = [imgdiff._mask_arg(mask)] if mask else None
        r = imgdiff.compare(a, b, masks=masks)
        Image.fromarray(imgdiff.diff_image(a, b, masks=masks)).save(diff := self._shot_path('diff'))
        return {**r.to_dict(), 'passed': r.passed({'min_ssim': min_ssim}), 'a': a, 'b': b, 'diff': str(diff)}

    def log(self, tail: int = 60, grep: str = None, new: bool = False, path: str = None):
        """Lines of the game log (this server's last launch, else the newest of <run dir>/<log> and <exe dir>/<log>). tail: the last N
        lines kept. grep: only lines matching this regex (case-insensitive; searches the whole log). new: only lines written since the
        previous log(new=true) call or the launch. path: read another file (e.g. stderr.txt in the run dir)."""
        p = Path(path) if path else self._log_path()
        if not p.exists():
            return {'path': str(p), 'exists': False}
        rx = re.compile(grep or '', re.I)
        if new:
            if self.log_pos > p.stat().st_size:                  # the file was replaced
                self.log_pos = 0
            lines, self.log_pos = _read_from(p, self.log_pos)
            lines = [ln for ln in lines if rx.search(ln)]
        elif grep:
            with open(p, encoding='utf-8', errors='replace') as f:
                lines = [ln.rstrip('\n') for ln in f if rx.search(ln)]
        else:
            lines = _tail(p, tail, 4 << 20)
        return {'path': str(p), 'size_kb': p.stat().st_size >> 10, 'matched': len(lines), 'lines': [ln[:500] for ln in lines[-tail:]]}

    def reports(self, max_chars: int = 20000):
        """Crash, freeze and fatal reports: <exe stem>_crash.txt / _freeze.txt / _fatal.txt next to the exe and *.txt in the test output.
        new = written since this server's last launch. Text cut at max_chars per file."""
        out = [{'path': str(p), 'modified': time.ctime(p.stat().st_mtime), 'new': bool(self.launch_t and p.stat().st_mtime >= self.launch_t),
                'text': p.read_text(encoding='utf-8', errors='replace')[:max_chars]} for p in self._report_files()]
        return out or 'no crash, freeze or fatal report'
