"""Drive the game exe from an AI agent: process, window, screenshots, input, log, crash reports, scripted runs.

Game independent: the exe and log names come from recomp/games/<id>/game.toml (id: NXRT_AI_GAME, default oot). The exe is NXRT_AI_EXE or
game-build/build/release/<exe>; a run's files (log, stdout/stderr, test output, screenshots) go to NXRT_AI_RUN_DIR or work/ai_run/<id>.
mcp_server.py serves every name in TOOLS (methods of Game, the docstring is the tool description) over MCP and as a CLI.

Several runs at once: launch(instance="name") starts the exe as a named instance (--instance NAME --instance-dir <run dir>/<name>), which
writes everything of its own (log, reports, saves, controls.ini, pipeline cache) into that folder while it shares the game files, the shader
cache and the BC7 cache with every other run (nxrt/instance.h). The run without a name is "default" (the old layout). Every other tool
takes instance= or pid= to address one process; without them it uses the one run this server started, else the one game process running,
and asks for a name when there are several. Processes this server did not start (other sessions' runs) are found by the exe's file name and
their command line (--instance, --instance-dir, --log); kill ends them only with force=true.

The running exe has no live control channel (no pipe, socket or command file). The runtime's test tooling (scripts, pad and vpad input,
@peek/@poke, game probes) is configured at start through NXRT_TEST_* and answers through files (result.json, trace.jsonl, shots/), so it is
reached through launch(script=...). Live control is window messages or SendInput to the game window.
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

TOOLS = ['status', 'launch', 'restart', 'kill', 'screenshot', 'send_input', 'wait_for', 'compare', 'log', 'reports']
# The game exe's exit codes (game_main.cpp, the test tooling's contract in tt.h, crash handler, watchdog, fatal).
EXIT = {0: 'pass / normal exit', 1: 'script failed or start-up error', 2: 'usage or script parse error', 3: 'crash', 4: 'crash',
        5: 'freeze or fatal error', 6: 'process ended before the script finished',
        7: 'refused to start: this game folder or instance is already running, or too little free memory beside the other runs',
        124: 'deadline (--max-seconds / wall_timeout_s)'}
LOG_LEVEL = re.compile(r'^\[\s*[\d.]+ ([EW]) ')       # runtime log prefix "[ 12.345678 E t1234 name] "
DEFAULT = 'default'                                   # the run without --instance (the exe refuses it as a name)
INSTANCE_NAME = re.compile(r'[A-Za-z0-9_-][A-Za-z0-9_.-]{0,39}')    # game_main.cpp valid_instance_name
MEMORY_PER_RUN_GB = 13.0                              # game_main.cpp check_memory: [boot] memory_per_run_gb
STARTUP_CHECK_S = 1.5                                 # launch watches this long for a run that ends at once (refused, bad arguments)

# ---- Win32 -------------------------------------------------------------------------------------------------------------------------------
u32 = ctypes.WinDLL('user32', use_last_error=True)    # own instances: argtypes here do not leak into wincap's calls
k32 = ctypes.WinDLL('kernel32', use_last_error=True)
ntdll = ctypes.WinDLL('ntdll')
shell32 = ctypes.WinDLL('shell32')


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


class MEMORYSTATUSEX(ctypes.Structure):
    _fields_ = [('dwLength', wt.DWORD), ('dwMemoryLoad', wt.DWORD)] + [(n, ctypes.c_ulonglong) for n in (
        'ullTotalPhys', 'ullAvailPhys', 'ullTotalPageFile', 'ullAvailPageFile', 'ullTotalVirtual', 'ullAvailVirtual', 'ullAvailExtendedVirtual')]


class UNICODE_STRING(ctypes.Structure):
    _fields_ = [('Length', wt.USHORT), ('MaximumLength', wt.USHORT), ('Buffer', ctypes.c_void_p)]


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
MemStatus = _decl(k32, 'GlobalMemoryStatusEx', wt.BOOL, ctypes.POINTER(MEMORYSTATUSEX))
LocalFree = _decl(k32, 'LocalFree', ctypes.c_void_p, ctypes.c_void_p)
NtQueryInformationProcess = _decl(ntdll, 'NtQueryInformationProcess', ctypes.c_long, H, ctypes.c_int, ctypes.c_void_p, wt.ULONG,
                                  ctypes.POINTER(wt.ULONG))
CommandLineToArgv = _decl(shell32, 'CommandLineToArgvW', ctypes.POINTER(wt.LPWSTR), wt.LPCWSTR, ctypes.POINTER(ctypes.c_int))
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


def tree(root, procs):
    """root and every process below it (by parent pid)."""
    pids = {root}
    while more := {p for p, pp, _ in procs if pp in pids} - pids:
        pids |= more
    return pids


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


def command_line(pid):
    """The process's command line split like the C runtime does (CommandLineToArgvW), or None when it cannot be read."""
    h = OpenProcess(0x1000, False, pid)                         # PROCESS_QUERY_LIMITED_INFORMATION
    if not h:
        return None
    try:
        n = wt.ULONG(0)
        NtQueryInformationProcess(h, 60, None, 0, ctypes.byref(n))       # ProcessCommandLineInformation: the size first
        if not n.value:
            return None
        buf = ctypes.create_string_buffer(n.value)
        if NtQueryInformationProcess(h, 60, buf, n, ctypes.byref(n)) != 0:
            return None
        us = UNICODE_STRING.from_buffer(buf)
        line = ctypes.wstring_at(us.Buffer, us.Length // 2) if us.Buffer and us.Length else ''
    finally:
        CloseHandle(h)
    argc = ctypes.c_int(0)
    argv = CommandLineToArgv(line, ctypes.byref(argc))
    if not argv:
        return [line]
    try:
        return [argv[i] for i in range(argc.value)]
    finally:
        LocalFree(argv)


def memory_status():
    m = MEMORYSTATUSEX()
    m.dwLength = ctypes.sizeof(m)
    MemStatus(ctypes.byref(m))
    return {'free_gb': round(m.ullAvailPhys / 2**30, 1), 'total_gb': round(m.ullTotalPhys / 2**30, 1),
            'commit_free_gb': round(m.ullAvailPageFile / 2**30, 1), 'commit_limit_gb': round(m.ullTotalPageFile / 2**30, 1)}


def _arg(argv, name):
    """The value after `name` in an argument list (the last one wins, as in game_main), or None."""
    v = None
    for i, a in enumerate(argv or []):
        if a == name and i + 1 < len(argv):
            v = argv[i + 1]
    return v


def instance_name(name):
    """An instance argument: None (not given) stays None; '' and 'default' are the run without a name."""
    if name is None:
        return None
    n = str(name).strip()
    if n == '' or n.lower() == DEFAULT:
        return DEFAULT
    if not INSTANCE_NAME.fullmatch(n):
        raise ValueError(f'instance name {n!r}: use 1-40 letters, digits, "-", "_" or "." (not starting with ".")')
    return n


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


class Run:
    """One instance as this server launched it: the process (while it runs and after it ended), its folder, log and script files."""

    def __init__(self, instance, run_dir):
        self.instance, self.run_dir = instance, run_dir
        self.proc = self.launch_t = self.log_path = self.last_launch = None
        self.peeks = []

    def alive(self):
        return self.proc is not None and self.proc.poll() is None


class Target:
    """A game process a tool acts on: one of this server's runs (own) or a process found by the exe's file name (another session's)."""

    def __init__(self, instance, pid, pids, own=None, argv=None):
        self.instance, self.pid, self.pids, self.own, self.argv = instance, pid, sorted(pids), own, argv

    @property
    def running(self):
        return bool(self.pids)

    def label(self):
        return f'{self.instance} (pid {self.pid}{"" if self.own else ", not started by this server"})'


class Game:
    def __init__(self, game_id=None, exe=None, run_dir=None):
        self.id = game_id or os.environ.get('NXRT_AI_GAME', 'oot')
        manifest = tomllib.loads((ROOT / 'recomp' / 'games' / self.id / 'game.toml').read_text(encoding='utf-8'))
        files = manifest['files']
        self.log_name = files['log']
        self.exe = Path(exe or os.environ.get('NXRT_AI_EXE') or ROOT / 'game-build' / 'build' / 'release' / files['exe']).resolve()
        self.run_dir = Path(run_dir or os.environ.get('NXRT_AI_RUN_DIR') or ROOT / 'work' / 'ai_run' / self.id).resolve()
        self.runs = {}                    # instance name -> Run (this server's launches)
        self.last = None                  # the Run launched last
        self.log_pos = {}                 # log path -> position of log(new=true)
        # launch's memory check (game_main.cpp check_memory): GB one run needs; launch(settings={"boot.memory_per_run_gb": ...}) wins
        self.memory_per_run_gb = float(os.environ.get('NXRT_BOOT_MEMORY_PER_RUN_GB', MEMORY_PER_RUN_GB))

    # ---- finding the game ----------------------------------------------------------------------------------------------------------------
    def _is_game(self, image_name, pid):
        """A process (not started by this server) that is a run of the game: the exe's file name (any build of the game counts)."""
        return image_name.lower() == self.exe.name.lower()

    def _run_dir(self, instance):
        return self.run_dir if instance == DEFAULT else self.run_dir / instance

    def _running(self):
        """Every running game process: this server's runs (their process trees) first, then the others, with the instance their command
        line names."""
        procs, out, claimed = processes(), [], set()
        for run in self.runs.values():
            if run.alive():
                pids = tree(run.proc.pid, procs)
                claimed |= pids
                out.append(Target(run.instance, run.proc.pid, pids, own=run))
        for pid, _, name in procs:
            if pid in claimed or not self._is_game(name, pid):
                continue
            argv = command_line(pid) or []
            pids = tree(pid, procs)
            claimed |= pids
            out.append(Target(instance_name(_arg(argv, '--instance')) or DEFAULT, pid, pids, argv=argv))
        return out

    def _target(self, instance=None, pid=None, required=True):
        """The process a tool acts on. pid= or instance= name it; without them: the one run this server started, else the one game process
        running. A run of this server that has ended still answers (exit code, log, reports). None when nothing fits and not required."""
        inst, live = instance_name(instance), self._running()
        listing = lambda ts: ', '.join(t.label() for t in ts) or 'none'     # noqa: E731
        if pid is not None:
            if m := [t for t in live if int(pid) in t.pids]:
                return m[0]
            raise RuntimeError(f'pid {pid} is not a running game process (running: {listing(live)})')
        if inst is not None:
            m = [t for t in live if t.instance == inst]
            if len(m) > 1 and len(own := [t for t in m if t.own]) == 1:
                return own[0]
            if len(m) > 1:
                raise RuntimeError(f'several processes run instance {inst}: {listing(m)}; name one with pid=')
            if m:
                return m[0]
            if inst in self.runs:
                return Target(inst, self.runs[inst].proc.pid if self.runs[inst].proc else None, [], own=self.runs[inst])
            if required:
                raise RuntimeError(f'instance {inst} is not running (running: {listing(live)})')
            return None
        own = [t for t in live if t.own]
        if len(own) == 1:
            return own[0]
        if len(own) > 1 or (not own and len(live) > 1):
            raise RuntimeError(f'several game processes run: {listing(own or live)}; name one with instance= or pid=')
        if live:
            return live[0]
        if self.last:
            return Target(self.last.instance, self.last.proc.pid if self.last.proc else None, [], own=self.last)
        if required:
            raise RuntimeError('the game is not running')
        return None

    def _pids(self, t):
        """The target's processes now (a run's tree grows after the start: the exe's children, the game under a .cmd stand-in)."""
        if t is None:
            return []
        procs = processes()
        root = (t.own.proc.pid if t.own.alive() else None) if t.own else t.pid
        if root is None or not any(p == root for p, _, _ in procs):
            return []
        return sorted(tree(root, procs))

    def _window(self, t, required=True):
        """(hwnd, title, pid) of the largest visible top-level window of the target's processes."""
        area = lambda h: wincap.client_size(h)[0] * wincap.client_size(h)[1]     # noqa: E731
        best = max(((area(h), h, ti, p) for pid in self._pids(t) for h, ti, p in wincap.find_windows(pid=pid)), default=None)
        if best is None and required:
            raise RuntimeError(f'no game window for {t.label() if t else "the game"}: not running, still starting, or headless (headless runs take '
                               'screenshots with the script step @shot)')
        return best[1:] if best else None

    def _alive(self, t):
        return bool(self._pids(t))

    def _capture(self, t, method=None):
        w = self._window(t)
        img, how = wincap.capture_best(w[0], method)
        return img, how, w

    def _shot_path(self, t, kind):
        d = (t.own.run_dir if t and t.own else self.run_dir) / 'shots'
        d.mkdir(parents=True, exist_ok=True)
        now = time.time()
        return d / f'{kind}_{time.strftime("%H%M%S", time.localtime(now))}_{int(now * 1000) % 1000:03d}.png'

    def _log_path(self, t):
        if t and t.own and t.own.log_path:
            return t.own.log_path
        if t and t.argv is not None:                             # another session's run: what its command line says
            if log := _arg(t.argv, '--log'):
                return Path(log)
            if d := _arg(t.argv, '--instance-dir'):
                return Path(d) / self.log_name
            return None
        found = [p for p in (self.run_dir / self.log_name, self.exe.parent / self.log_name) if p.exists()]
        return max(found, key=lambda p: p.stat().st_mtime) if found else self.run_dir / self.log_name

    def _report_dir(self, t):
        """Where the target's crash / freeze / fatal reports go: the instance folder of a named run, next to the exe for the default run."""
        if t and t.instance != DEFAULT:
            if t.own:
                return t.own.run_dir
            if d := _arg(t.argv, '--instance-dir'):
                return Path(d)
            if g := _arg(t.argv, '--game'):
                return Path(g) / 'instances' / t.instance
            return None
        if t and t.argv:                                         # a default run of another build: next to its exe
            if path := proc_info(t.pid).get('path'):
                return Path(path).parent
        return self.exe.parent

    def _report_files(self, t):
        d = self._report_dir(t)
        out = self.run_dir if not (t and t.own) else t.own.run_dir
        return [p for p in ([d / f'{self.exe.stem}_{k}.txt' for k in ('crash', 'freeze', 'fatal')] if d else [])
                + sorted((out / 'testout').glob('*.txt')) if p.exists()]

    def _test_results(self, t):
        if not (t and t.own):
            return {}
        out, r = t.own.run_dir / 'testout', {}
        if (out / 'result.json').exists():
            r['result'] = json.loads((out / 'result.json').read_text(encoding='utf-8'))
        if (out / 'trace.jsonl').exists():
            r['trace_tail'] = _tail(out / 'trace.jsonl', 3)
        if shots := sorted(str(p) for p in (out / 'shots').glob('*.png')):
            r['shots'] = shots
        if t.own.peeks:
            r['peeks'] = [{'step': s, 'hex': f.read_text().strip() if f and f.exists() else None} for s, f in t.own.peeks]
        return r

    def _summary(self, t):
        info = [proc_info(p) for p in t.pids]
        w = self._window(t, required=False)
        lp = self._log_path(t)
        return {'instance': t.instance, 'pid': t.pid, 'started_by_this_server': bool(t.own), 'pids': t.pids,
                'uptime_s': next((i.get('uptime_s') for i in info if i['pid'] == t.pid), None),
                'working_set_mb': sum(i.get('working_set_mb', 0) for i in info), 'private_mb': sum(i.get('private_mb', 0) for i in info),
                'window': w[1] if w else None, 'log': str(lp) if lp else None}

    def _check_memory(self, others, gb):
        """The exe's own start check (game_main.cpp check_memory), before starting it: free memory minus what the other runs will still
        take must hold one more run, for physical memory and for the commit limit."""
        if not (gb > 0) or not others:
            return
        need, m = gb * 1024, memory_status()
        sizes = [self._summary(t) for t in others]
        growth = sum(max(0.0, need - s['working_set_mb']) for s in sizes) / 1024
        commit_growth = sum(max(0.0, need - s['private_mb']) for s in sizes) / 1024
        if m['free_gb'] - growth < gb or m['commit_free_gb'] - commit_growth < gb:
            raise RuntimeError(f'not enough free memory for another run: {m["free_gb"]} GB free, {growth:.1f} GB of it still to be taken by the '
                               f'{len(others)} other run(s) ({", ".join(t.label() for t in others)}); {m["commit_free_gb"]} GB commit free, '
                               f'{commit_growth:.1f} GB of it still to be taken; a run needs about {gb:g} GB. End a run first, or pass '
                               'settings={"boot.memory_per_run_gb": 0} to skip the check')

    # ---- tools ------------------------------------------------------------------------------------------------------------------------------
    def status(self, instance: str = None, pid: int = None):
        """One-call overview. Always: the exe (path, build time), free memory (physical and commit), and every running game process
        ("instances": name, pid, started by this server or not, memory, window title, log path), this server's or another session's.
        For one process (instance= or pid=; without them the one run this server started, else the one game process running): its
        processes (uptime, CPU seconds, memory), the exit code when it has ended, the window (title, client size, focused, minimised, hung =
        not answering messages), the log (path, seconds since the last write, errors and warnings among the last 400 lines, last lines),
        which crash/freeze/fatal reports exist, and the scripted-run result and last trace lines when the test tooling is on."""
        live = self._running()
        s = {'game': self.id, 'exe': str(self.exe), 'exe_built': time.ctime(self.exe.stat().st_mtime) if self.exe.exists() else 'missing',
             'running': bool(live), 'memory': memory_status(), 'instances': [self._summary(t) for t in live]}
        named = instance is not None or pid is not None
        try:
            t = self._target(instance, pid, required=named)
        except RuntimeError as e:
            if named:
                raise
            s['note'] = f'{e} for the details of one'
            return s
        if t is None:
            return s
        s['instance'] = t.instance
        s['target_running'] = t.running
        s['processes'] = [proc_info(p) for p in t.pids]
        if t.own and t.own.proc and t.own.proc.poll() is not None:
            s['exit_code'] = t.own.proc.returncode
            s['exit_meaning'] = EXIT.get(t.own.proc.returncode, 'unexpected')
        if win := self._window(t, required=False):
            h = win[0]
            s['window'] = {'title': win[1], 'size': wincap.client_size(h), 'focused': Foreground() == h, 'minimized': bool(IsIconic(h)),
                           'hung': bool(IsHung(h))}
        lp = self._log_path(t)
        if lp and lp.exists():
            lines = _tail(lp, 400)
            levels = collections.Counter(m.group(1) for m in map(LOG_LEVEL.match, lines) if m)
            s['log'] = {'path': str(lp), 'idle_s': round(time.time() - lp.stat().st_mtime, 1), 'recent_errors': levels['E'],
                        'recent_warnings': levels['W'], 'last': [ln[:300] for ln in lines[-8:]]}
        s['reports'] = [str(p) for p in self._report_files(t)]
        if r := self._test_results(t):
            s['test'] = r
        return s

    def launch(self, args: list = None, settings: dict = None, env: dict = None, script: list = None, headless: bool = False,
               wait: bool = False, timeout_s: float = 600, exe: str = None, cwd: str = None, instance: str = None):
        """(Re)start the game. instance: a name (1-40 letters, digits, - _ .) runs the exe as that named instance, beside other runs: it
        gets --instance <name> --instance-dir <run dir>/<name>, and everything it writes (log, crash/freeze/fatal reports, saves,
        controls.ini, pipeline cache, test output, screenshots) lands in that folder, while the game files, shader cache and BC7 cache
        stay shared. Omitted: the default run (run dir itself, shared saves and pipeline cache). While another session may be running the
        game, always pass an instance name. This server's previous run of the same instance is killed first; a run of that instance that
        this server did not start is never touched (the launch is refused). Also refused, before starting, when the free memory minus what
        the other runs will still take would not hold one more run (about 13 GB; settings {"boot.memory_per_run_gb": 0} skips the check).
        The exe gets `--log <run dir>/<log>` (unless args has --log), `--headless` when asked, then args, e.g. ["--max-seconds", "60",
        "--exefs", "D:/dump/exefs", "--ini", "x.ini"]. The window is asked to open without taking the focus. cwd defaults to the run dir
        (an installed exe changes to its own folder).
        settings: runtime config keys {"section.key": value} passed as NXRT_<SECTION>_<KEY> (the environment beats the ini; keys in
        runtime/docs/CONFIG.md), e.g. {"log.level": "debug", "diag.watchdog_seconds": 0, "test.enabled": 1}. env: more environment variables.
        script: lines of a test-tooling script (runtime/include/nxrt/tt_script.h: press/hold/stick/key/mouse, "keydown vpad ..." for the
        virtual SDL pad, @shot/@hash/@assert/@log/@sleep/@pass/...). Two shorthands read and write guest memory and come back in the result:
        `peek <addr> <len> [off ...]` and `poke <addr> [off ...] : <hexbytes>`.
        With a script or test.enabled the test output (result.json, trace.jsonl = per-frame state from the game probes, shots/) is in
        <run dir>/testout; test.frame_source defaults to tick (the present path, needs no game hooks; set "test.frame_source" to change it).
        wait: block until the game exits (killed after timeout_s) and return exit code, scripted-run result, peeks and the log tail.
        Without wait, a run that ends within 1.5 s (refused, bad arguments) is reported with its exit code and stderr."""
        inst = instance_name(instance) or DEFAULT
        params = {k: v for k, v in locals().items() if k not in ('self', 'inst')}
        run = self.runs.get(inst)
        if run and run.alive():
            self._kill_run(run)
        live = self._running()
        if clash := [t for t in live if t.instance == inst and not t.own]:
            hint = 'choose another instance name' if inst != DEFAULT else 'pass instance="<name>" to run beside it'
            raise RuntimeError(f'instance {inst} is already running as {clash[0].label()}: {hint} (another session\'s run is never killed '
                               'by launch; kill(pid=..., force=true) ends it)')
        e_settings = {re.sub(r'\W', '_', k.upper().replace('.', '_', 1)): v for k, v in (settings or {}).items()}
        gb = e_settings.get('BOOT_MEMORY_PER_RUN_GB', (env or {}).get('NXRT_BOOT_MEMORY_PER_RUN_GB', self.memory_per_run_gb))
        self._check_memory(live, float(gb))
        if exe:
            self.exe = Path(exe).resolve()
        if not self.exe.exists():
            raise RuntimeError(f'{self.exe} not found: build the game exe, or pass exe= (or set NXRT_AI_EXE)')
        rd = self._run_dir(inst)
        run = self.runs[inst] = Run(inst, rd)
        run.last_launch = params
        self.last = run
        args = [str(a) for a in args or []]
        out = rd / 'testout'
        shutil.rmtree(out, ignore_errors=True)                  # this server's own output of the previous run
        out.mkdir(parents=True)
        if inst != DEFAULT and '--instance' not in args:
            args = ['--instance', inst, '--instance-dir', str(rd)] + args
        if '--log' in args:
            log = Path(cwd or rd, args[args.index('--log') + 1])
        else:
            log = rd / self.log_name
            args = ['--log', str(log)] + args
        if log.exists():
            os.replace(log, log.with_name(log.name + '.prev'))
        if headless:
            args.append('--headless')
        e = dict(os.environ, SDL_WINDOW_ACTIVATE_WHEN_SHOWN='0', NXRT_TEST_OUT=str(out), NXRT_TEST_FRAME_SOURCE='tick')
        for k in ('NXRT_INSTANCE', 'NXRT_INSTANCE_DIR'):        # this server's own environment must not name an instance for every run
            e.pop(k, None)
        for k, v in (settings or {}).items():                    # CONFIG.md: NXRT_ + SECTION + _ + KEY, non-alphanumerics as _
            e['NXRT_' + re.sub(r'\W', '_', k.upper().replace('.', '_', 1))] = ('1' if v else '0') if isinstance(v, bool) else str(v)
        if script:
            e['NXRT_TEST_SCRIPT'] = str(self._write_script(run, script))
        e.update({k: str(v) for k, v in (env or {}).items()})
        si = subprocess.STARTUPINFO(dwFlags=subprocess.STARTF_USESHOWWINDOW, wShowWindow=4)    # SW_SHOWNOACTIVATE
        with open(rd / 'stdout.txt', 'wb') as so, open(rd / 'stderr.txt', 'wb') as se:
            run.proc = subprocess.Popen([str(self.exe)] + args, cwd=cwd or rd, env=e, stdout=so, stderr=se, startupinfo=si,
                                        creationflags=subprocess.CREATE_NO_WINDOW)
        run.launch_t, run.log_path = time.time() - 1, log
        self.log_pos.pop(str(log), None)
        if not wait:
            r = {'instance': inst, 'pid': run.proc.pid, 'cmd': [str(self.exe)] + args, 'log': str(log), 'run_dir': str(rd)}
            try:
                rc = run.proc.wait(STARTUP_CHECK_S)
            except subprocess.TimeoutExpired:
                return r
            err = (rd / 'stderr.txt').read_text(encoding='utf-8', errors='replace').strip()
            return {**r, 'exit_code': rc, 'meaning': EXIT.get(rc, 'unexpected'), 'stderr': err[-2000:]}
        try:
            rc = run.proc.wait(timeout_s)
        except subprocess.TimeoutExpired:
            self._kill_run(run)
            rc = None
        t = Target(inst, run.proc.pid, [], own=run)
        return {'instance': inst, 'exit_code': rc, 'meaning': EXIT.get(rc, 'killed after timeout_s' if rc is None else 'unexpected'),
                **self._test_results(t), 'reports': [str(p) for p in self._report_files(t)], 'log_tail': self.log(30, instance=inst).get('lines')}

    def _write_script(self, run, lines):
        rd = run.run_dir
        if ' ' in str(rd):
            raise RuntimeError(f'run dir {rd} has a space: @peek/@poke file arguments are space separated (set NXRT_AI_RUN_DIR)')
        out = []
        for ln in lines:
            w = ln.split(None, 1)
            kw = w[0].lower() if w else ''
            if kw in ('peek', 'poke') and len(w) == 2:
                n = len(run.peeks)
                fin, fout = rd / f'{kw}{n}.in', rd / f'peek{n}.out' if kw == 'peek' else None
                fin.write_text(w[1] + '\n', encoding='ascii')
                if fout and fout.exists():
                    fout.unlink()
                out.append(f'@peek {fin} {fout}' if fout else f'@poke {fin}')
                run.peeks.append((ln, fout))
            else:
                out.append(ln)
        p = rd / 'script.nxs'
        p.write_text('\n'.join(out) + '\n', encoding='utf-8')
        return p

    def restart(self, instance: str = None):
        """Kill this server's run of the instance (default: the one launched last) and start it again with the arguments of its last launch."""
        inst = instance_name(instance)
        run = self.runs.get(inst) if inst else self.last
        if not run:
            raise RuntimeError(f'nothing launched yet by this server{" as instance " + inst if inst else ""}: use launch')
        return self.launch(**run.last_launch)

    def _kill_run(self, run):
        pids = sorted(tree(run.proc.pid, processes())) if run.alive() else []
        if pids:
            self._end(run.proc.pid, pids)
        try:
            run.proc.wait(10)
        except subprocess.TimeoutExpired:
            pass
        return pids

    def _end(self, root, pids):
        """taskkill /T of the root (its whole tree), then wait until none of pids runs."""
        subprocess.run(['taskkill', '/F', '/T', '/PID', str(root)], capture_output=True, creationflags=subprocess.CREATE_NO_WINDOW)
        for _ in range(50):
            if not {p for p, _, _ in processes()} & set(pids):
                break
            time.sleep(0.1)
        return pids

    def kill(self, instance: str = None, pid: int = None, force: bool = False):
        """End a run of the game. No arguments: every run this server started (never another session's). instance= or pid=: that process
        and its children. A process this server did not start (another session's run, found by the exe's file name) is refused unless
        force=true."""
        if instance is None and pid is None:
            runs = [r for r in self.runs.values() if r.alive()]
            killed = {r.instance: self._kill_run(r) for r in runs}
            others = [t.label() for t in self._running() if not t.own]
            r = {'killed': sorted(p for ps in killed.values() for p in ps), 'instances': sorted(killed)}
            if others:
                r['still_running'] = others
            if self.last and self.last.proc:
                r['exit_code'] = self.last.proc.returncode
            return r
        t = self._target(instance, pid)
        if not t.running:
            return {'killed': [], 'instance': t.instance, 'note': 'not running', 'exit_code': t.own.proc.returncode if t.own else None}
        if not t.own and not force:
            raise RuntimeError(f'{t.label()}: this server did not start it, so it may be another session\'s run; pass force=true to end it anyway')
        pids = self._kill_run(t.own) if t.own else self._end(t.pid, t.pids)
        return {'killed': pids, 'instance': t.instance, 'exit_code': t.own.proc.returncode if t.own else None}

    def screenshot(self, show: bool = True, crop: str = None, save: str = None, method: str = None, instance: str = None, pid: int = None):
        """Capture a game window's client area (instance= or pid= choose the run, see status): works while it is covered or unfocused, not
        while minimised. Saves a full-size PNG (path in the result) and, with show, returns the picture (at most 1280 px wide). crop:
        "x0,y0,x1,y1" in client pixels, or fractions when written with decimal points ("0,0,0.5,0.5" = top-left quarter). method:
        printwindow | bitblt | screen (default printwindow, falling back to the others when the picture is black)."""
        t = self._target(instance, pid)
        img, how, w = self._capture(t, method)
        if crop:
            img = img.crop(_xy(*crop.split(',')[:2], img.size) + _xy(*crop.split(',')[2:], img.size))
        path = Path(save) if save else self._shot_path(t, 'shot')
        img.save(path)
        info = {'instance': t.instance, 'pid': w[2], 'path': str(path), 'size': img.size, 'method': how, 'black': wincap.is_black(img), 'title': w[1]}
        if not show:
            return info
        view = img.copy()
        view.thumbnail((1280, 1280))
        buf = io.BytesIO()
        view.save(buf, 'JPEG', quality=85)
        return buf.getvalue(), info

    def send_input(self, steps: list, method: str = 'post', instance: str = None, pid: int = None):
        """Keyboard and mouse input to a game window (instance= or pid= choose the run, see status). steps, run in order:
          key <name> [hold_ms]   press and release (hold 60 ms)     down <name> / up <name>     text <characters>
          click|rclick|mclick <x> <y> [hold_ms]     move <x> <y>     wheel <notches> [<x> <y>]     rel <dx> <dy>     wait <ms>
        <name>: a character (a, 7, /), F1..F24, numpad0..9, return, escape, space, tab, backspace, shift, ctrl, alt, left, up, right, down,
        insert, delete, home, end, pageup, pagedown (also l/r variants: lshift, rctrl, ...), or a virtual-key code 0xNN.
        <x> <y>: client pixels, or fractions when written with a decimal point ("click 0.5 0.5" = the centre).
        method post: window messages to that game window only; works covered and unfocused and never touches another window, but cannot
        make raw mouse motion (rel) and a game reading the keyboard state directly will not see it.
        method send: SendInput after bringing the game window to the front: the real input path (raw mouse for camera look); refused,
        sending nothing, when the game window is not in front."""
        t = self._target(instance, pid)
        hwnd, title, _ = self._window(t)
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
            tok = st.split()
            op, a = tok[0].lower(), tok[1:]
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
        return {'instance': t.instance, 'pid': t.pid, 'window': title, 'steps': len(steps), 'method': method}

    def wait_for(self, until: str, timeout_s: float = 60, pattern: str = None, ref: str = None, min_ssim: float = 0.9, mask: str = None,
                 instance: str = None, pid: int = None):
        """Block until a condition holds for one run (instance= or pid= choose it, see status) or timeout_s runs out. until:
          window   the game window exists            visible   the window shows a picture that is not black
          change   the picture differs from the one at the call
          image    the picture matches the reference PNG ref (SSIM >= min_ssim; mask as in compare)
          log      a line of this run's log matches the regex pattern (case-insensitive)
          exit     the game process has ended
        Returns ok, the seconds waited, the matching log line or the last capture's path; stops early when the game is not running."""
        if until not in ('window', 'visible', 'change', 'image', 'log', 'exit'):
            raise ValueError(f'unknown condition {until!r}')
        t = self._target(instance, pid)
        rx = re.compile(pattern or '.', re.I)
        lp, pos, base, img, t0 = self._log_path(t), 0, None, None, time.time()
        masks = [imgdiff._mask_arg(mask)] if mask else None
        while True:
            alive = self._alive(t)
            hit = None
            if until == 'exit':
                hit = not alive
            elif until == 'window':
                hit = alive and self._window(t, required=False) is not None
            elif until == 'log':
                if lp and lp.exists():
                    lines, pos = _read_from(lp, pos)
                    hit = next((ln for ln in lines if rx.search(ln)), None)
            elif alive and (w := self._window(t, required=False)) is not None:
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
                r = {'ok': bool(hit), 'instance': t.instance, 'waited_s': round(time.time() - t0, 1)}
                if not alive and not hit:
                    ended = t.own and t.own.proc and t.own.proc.poll() is not None
                    r['why'] = 'the game is not running' + (f' (exit code {t.own.proc.returncode})' if ended else '')
                if isinstance(hit, str):
                    r['line'] = hit
                if img is not None:
                    img.save(p := self._shot_path(t, until))
                    r['shot'] = str(p)
                return r
            time.sleep(0.3)

    def compare(self, a: str, b: str = None, mask: str = None, min_ssim: float = 0.9, instance: str = None, pid: int = None):
        """Perceptual diff (tools/test/imgdiff.py: SSIM on luma, mean absolute error, histogram correlation, fraction of changed pixels) of
        two PNGs, or of the reference PNG a and a game window now (b omitted; instance= or pid= choose the run). mask hides HUD/animated
        parts: "hud", "x0,y0,x1,y1" (fractions), or a mask PNG. passed = every metric within imgdiff's defaults, SSIM >= min_ssim. Writes a
        diff picture (red = changed)."""
        t = self._target(instance, pid) if b is None or instance is not None or pid is not None else None
        if b is None:
            img = self._capture(t)[0]
            img.save(b := str(self._shot_path(t, 'now')))
        masks = [imgdiff._mask_arg(mask)] if mask else None
        r = imgdiff.compare(a, b, masks=masks)
        Image.fromarray(imgdiff.diff_image(a, b, masks=masks)).save(diff := self._shot_path(t, 'diff'))
        return {**r.to_dict(), 'passed': r.passed({'min_ssim': min_ssim}), 'a': a, 'b': b, 'diff': str(diff)}

    def log(self, tail: int = 60, grep: str = None, new: bool = False, path: str = None, instance: str = None, pid: int = None):
        """Lines of a game log: the run's log (instance= or pid= choose it, see status; this server's launch, or what another session's
        run named with --log / --instance-dir), else the newest of <run dir>/<log> and <exe dir>/<log>. tail: the last N lines kept. grep:
        only lines matching this regex (case-insensitive; searches the whole log). new: only lines written since the previous
        log(new=true) call on that file or the launch. path: read another file (e.g. stderr.txt in the run dir)."""
        if path:
            p = Path(path)
        else:
            t = self._target(instance, pid, required=instance is not None or pid is not None)
            p = self._log_path(t)
            if p is None:
                raise RuntimeError(f'{t.label()} names no --log or --instance-dir on its command line: pass path=')
        if not p.exists():
            return {'path': str(p), 'exists': False}
        rx = re.compile(grep or '', re.I)
        if new:
            pos = self.log_pos.get(str(p), 0)
            if pos > p.stat().st_size:                           # the file was replaced
                pos = 0
            lines, self.log_pos[str(p)] = _read_from(p, pos)
            lines = [ln for ln in lines if rx.search(ln)]
        elif grep:
            with open(p, encoding='utf-8', errors='replace') as f:
                lines = [ln.rstrip('\n') for ln in f if rx.search(ln)]
        else:
            lines = _tail(p, tail, 4 << 20)
        return {'path': str(p), 'size_kb': p.stat().st_size >> 10, 'matched': len(lines), 'lines': [ln[:500] for ln in lines[-tail:]]}

    def reports(self, max_chars: int = 20000, instance: str = None, pid: int = None):
        """Crash, freeze and fatal reports of a run (instance= or pid= choose it, see status): <exe stem>_crash.txt / _freeze.txt /
        _fatal.txt next to the exe for the default run, in the instance folder (<run dir>/<name>) for a named one, and *.txt in its test
        output. new = written since this server's launch of it. Text cut at max_chars per file."""
        t = self._target(instance, pid, required=instance is not None or pid is not None)
        launch_t = t.own.launch_t if t and t.own else None
        out = [{'path': str(p), 'modified': time.ctime(p.stat().st_mtime), 'new': bool(launch_t and p.stat().st_mtime >= launch_t),
                'text': p.read_text(encoding='utf-8', errors='replace')[:max_chars]} for p in self._report_files(t)]
        return out or 'no crash, freeze or fatal report'
