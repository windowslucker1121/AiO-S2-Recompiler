# SPDX-License-Identifier: GPL-3.0-or-later
"""Names guest addresses (f_<addr> in crash reports, logs and lifted code) from the dynamic symbols of the game's modules.

  tools\\.venv\\Scripts\\python tools\\ai\\symbolize.py [--game oot] [--exefs DIR] <crash.txt | addr ...>

A crash report argument is read for its `guest call stack` line; bare arguments are hex addresses (with or without f_ / 0x).
Module names and bases come from recomp/games/<id>/game.toml; the ExeFS defaults to game/<id>/plain/exefs. Names are demangled with
the repo's llvm-cxxfilt when it is there. A name is the nearest exported symbol at or below the address: main exports little,
so `main+off` alone is common.
"""
import argparse
import bisect
import glob
import os
import re
import subprocess
import sys
import tomllib

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, 'recomp'))
from nxrecomp.program import open_image          # noqa: E402


def load(game, exefs):
    with open(os.path.join(ROOT, 'recomp', 'games', game, 'game.toml'), 'rb') as f:
        man = tomllib.load(f)
    mods = []
    for m in man['module']:
        img = open_image(dict(kind='nso', path=os.path.join(exefs, m['file']), base=m['base'], name=m['name']))
        syms = sorted((img.sym_value(v) + m['base'], n) for n, t, b, sh, v, sz in img.symbols() if sh and n)
        mods.append((m['base'], m['base'] + max(len(img.mem), sum(img.bss)), m['name'], syms))
    return mods


def demangle(names):
    tool = glob.glob(os.path.join(ROOT, 'tools', 'llvm', '*', 'bin', 'llvm-cxxfilt.exe'))
    if not tool or not names:
        return names
    out = subprocess.run([tool[0]], input='\n'.join(names), capture_output=True, text=True).stdout.split('\n')
    return out[:len(names)]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--game', default=os.environ.get('NXRT_AI_GAME', 'oot'))
    ap.add_argument('--exefs')
    ap.add_argument('items', nargs='+')
    a = ap.parse_args()
    addrs = []
    for it in a.items:
        if os.path.isfile(it):
            text = open(it, encoding='utf-8', errors='replace').read()
            m = re.search(r'guest call stack[^:]*:(.*)', text)
            addrs += [int(x, 16) for x in re.findall(r'f_([0-9a-f]+)', m.group(1) if m else text)]
        else:
            addrs.append(int(re.sub(r'^(f_|0x)', '', it), 16))
    mods = load(a.game, a.exefs or os.path.join(ROOT, 'game', a.game, 'plain', 'exefs'))
    rows = []
    for x in addrs:
        where, name = '?', ''
        for lo, hi, mn, syms in mods:
            if lo <= x < hi:
                where = f'{mn}+{x - lo:#x}'
                i = bisect.bisect_right(syms, (x, '￿')) - 1
                if i >= 0:
                    name = (syms[i][1], x - syms[i][0])
        rows.append((x, where, name))
    dm = demangle([n[0] for _, _, n in rows if n])
    it = iter(dm)
    for x, where, n in rows:
        print(f'{x:#012x}  {where:20}  ' + (f'{next(it)}+{n[1]:#x}' if n else ''))


if __name__ == '__main__':
    main()
