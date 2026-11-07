#!/usr/bin/env python3
"""Times the shader preparation of a test set (programs_subset.py) with one or more game exes: wall time, CPU time of the process (=
average busy cores) and the "prepare: done" line. Every run starts without prepare.state and pipeline_cache.bin and with a fresh
[nvn] spirv_salt, so neither the app cache nor the driver's disk cache answers.

    python -I prepare_bench.py --set <dir from programs_subset.py> --exe old=<path> --exe new=<path> [--threads 14] [--repeat 1]

Never point --set at a real shader cache: it deletes <set>/prepare.state and <set>/pipeline_cache.bin.
"""
import argparse, ctypes, os, subprocess, time, uuid
from ctypes import wintypes


def cpu_seconds(handle):
    c, e, k, u = (wintypes.FILETIME() for _ in range(4))
    ctypes.windll.kernel32.GetProcessTimes(wintypes.HANDLE(int(handle)), ctypes.byref(c), ctypes.byref(e), ctypes.byref(k), ctypes.byref(u))
    ft = lambda f: ((f.dwHighDateTime << 32) | f.dwLowDateTime) / 1e7   # noqa: E731
    return ft(k) + ft(u)


def run(name, exe, a):
    for f in ('prepare.state', 'pipeline_cache.bin'):
        p = os.path.join(a.set, f)
        if os.path.exists(p):
            os.remove(p)
    game = os.path.join(a.set, 'game')
    os.makedirs(game, exist_ok=True)
    log = os.path.join(a.set, f'{name}.log')
    env = dict(os.environ, NXRT_NVN_SHADER_CACHE=os.path.join(a.set, 'spirv'), NXRT_NVN_SHADER_TRANSLATOR='off',
               NXRT_NVN_SPIRV_SALT=a.salt or uuid.uuid4().hex[:8], NXRT_ALLOW_MULTIPLE='1')
    cmd = [exe, '--game', game, '--log', log, '--prepare-shaders', '--programs', os.path.join(a.set, 'programs.bin'), '--threads', str(a.threads)]
    t0 = time.time()
    p = subprocess.Popen(cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    last = ''
    for line in p.stdout:
        if line.startswith('prepare: done') or line.startswith('prepare: error'):
            last = line.strip()
    p.wait()
    wall = time.time() - t0
    cpu = cpu_seconds(p._handle)
    print(f'{name}: {wall:.1f} s wall, {cpu:.0f} s CPU = {cpu / wall:.1f} cores busy on average; exit {p.returncode}; {last}', flush=True)
    return wall


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--set', required=True)
    ap.add_argument('--exe', action='append', required=True, help='name=path')
    ap.add_argument('--threads', type=int, default=14)
    ap.add_argument('--repeat', type=int, default=1)
    ap.add_argument('--salt', help='one salt for every run: after a first run the driver disk cache answers (a warm preparation)')
    a = ap.parse_args()
    for r in range(a.repeat):
        for e in a.exe:
            name, path = e.split('=', 1)
            run(f'{name}_{r}', path, a)


if __name__ == '__main__':
    main()
