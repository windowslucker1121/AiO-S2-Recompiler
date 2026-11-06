"""Reads what a traced frame left behind: render-target snapshots and storage-buffer snapshots, with non-finite values counted.

The runtime writes these into the run's working folder (the instance folder for a named instance) when a frame is traced
(`[nvn] trace_sec` or `trace_frame`) and snapshots are asked for (`[nvn] trace_snap`, `trace_snap_fs`, `trace_snap_sampled`,
`trace_snap_ssbo`; runtime/docs/CONFIG.md):
  snap_d<draw>_t<texture>_<format>.png      8-bit preview of a colour/depth target after that draw
  snap_..._<format>.png.bin                 the texel bytes, for float and 16-bit formats (an 8-bit PNG clamps HDR values)
  snapb_d<draw>_<v|f>s<slot>_<address>.bin  a vertex/fragment storage buffer as the GPU holds it after that draw (<= 1 MiB)
Sizes and formats of the target snapshots come from the run's log (`snapshot after draw N: tID:FORMAT WxHxD ...`). The runtime's own
stats on that log line are unreliable for R11G11B10F (one channel reads +-1e30): use this script.

usage:
  gpu_snapshots.py targets <run dir> [--draw N]          per target snapshot: % non-finite texels, median, max
  gpu_snapshots.py buffers <run dir> [--draw N]          per buffer snapshot: non-finite float words and where the first one is
  gpu_snapshots.py words <file.bin> [--offset 0x0] [--count 64] [--float]   hex (or float) words of a buffer snapshot
  gpu_snapshots.py texel <snap .png.bin> <x> <y> --run <run dir>            decoded value of one texel
"""
import argparse
import os
import re
import sys

import numpy as np

SNAP_LINE = re.compile(r"snapshot after draw (\d+): t(\d+):(\w+) (\d+)x(\d+)x\d+ .*-> (\S+\.png)")
CHANNELS = {"R32F": 1, "RG32F": 2, "RGBA32F": 4, "R16F": 1, "RG16F": 2, "RGBA16F": 4}


def snap_index(run):
    info = {}
    with open(os.path.join(run, "oot.log"), encoding="utf-8", errors="replace") as f:
        for line in f:
            m = SNAP_LINE.search(line)
            if m:
                info[m.group(6)] = (int(m.group(1)), int(m.group(2)), m.group(3), int(m.group(4)), int(m.group(5)))
    return info


def _uf(v, shift, mbits):   # unsigned small float of R11G11B10F: 5-bit exponent, no sign; exponent 31 = inf/NaN
    e, m = (v >> (shift + mbits)) & 31, (v >> shift) & ((1 << mbits) - 1)
    val = np.where(e == 0, m / (1 << mbits) * 2.0 ** -14, (1 + m / (1 << mbits)) * 2.0 ** (e.astype(np.float64) - 15))
    return np.where(e == 31, np.nan, val)


def decode(raw, fmt, w, h):
    """Texels as float64 [h*w, channels], or None for formats without a raw dump."""
    n = w * h
    if fmt == "R11G11B10F":
        v = np.frombuffer(raw[:n * 4], np.uint32)
        return np.stack([_uf(v, 0, 6), _uf(v, 11, 6), _uf(v, 22, 5)], -1)
    if fmt in CHANNELS:
        c = CHANNELS[fmt]
        dt = np.float32 if "32" in fmt else np.float16
        return np.frombuffer(raw[:n * c * np.dtype(dt).itemsize], dt).reshape(n, c).astype(np.float64)
    if fmt.startswith("DEPTH32"):
        return np.frombuffer(raw[:n * 4], np.float32).reshape(n, 1).astype(np.float64)
    return None


def cmd_targets(a):
    rows = []
    for name, (d, t, fmt, w, h) in snap_index(a.run).items():
        if a.draw is not None and d != a.draw:
            continue
        p = os.path.join(a.run, name + ".bin")
        if not os.path.exists(p):
            continue
        x = decode(open(p, "rb").read(), fmt, w, h)
        if x is None:
            continue
        bad = ~np.isfinite(x).all(-1)
        fin = x[np.isfinite(x)]
        rows.append((d, t, fmt, w, h, 100 * bad.mean(), float(np.median(fin)) if fin.size else float("nan"),
                     float(fin.max()) if fin.size else float("nan"), name))
    for r in sorted(rows):
        print("draw %5d t%-6d %-10s %5dx%-5d non-finite %6.2f%%  median %-9.4g max %-9.4g %s" % r)


def cmd_buffers(a):
    pat = re.compile(r"snapb_d(\d+)_([vf])s(\d+)_([0-9a-f]+)\.bin$")
    rows = []
    for f in os.listdir(a.run):
        m = pat.match(f)
        if not m or (a.draw is not None and int(m.group(1)) != a.draw):
            continue
        w = np.fromfile(os.path.join(a.run, f), np.uint32)
        nonfin = ((w >> 23) & 255) == 255
        first = hex(int(np.argmax(nonfin)) * 4) if nonfin.any() else "-"
        rows.append((int(m.group(1)), m.group(2), int(m.group(3)), m.group(4), w.size * 4, int(nonfin.sum()), first, f))
    for d, st, slot, addr, size, n, first, f in sorted(rows):
        print("draw %5d %ss%-2d %s %9d bytes  non-finite float words %7d  first at %s" % (d, st, slot, addr, size, n, first))
    print("(an integer or packed buffer shows 'non-finite' words too: look at them with `words` before drawing conclusions)")


def cmd_words(a):
    w = np.fromfile(a.file, np.uint32)
    start = int(a.offset, 0) // 4
    part = w[start:start + a.count]
    for k in range(0, len(part), 8):
        row = part[k:k + 8]
        text = " ".join(("%12.6g" % x) for x in row.view(np.float32)) if a.float else " ".join("%08x" % x for x in row)
        print("+0x%05x: %s" % ((start + k) * 4, text))


def cmd_texel(a):
    name = os.path.basename(a.file)[:-4]
    info = snap_index(a.run).get(name)
    if not info:
        sys.exit("no log line for %s in %s" % (name, a.run))
    d, t, fmt, w, h = info
    x = decode(open(a.file, "rb").read(), fmt, w, h)
    print(fmt, w, h, "texel", a.x, a.y, "=", x[a.y * w + a.x])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    for c in ("targets", "buffers"):
        s = sub.add_parser(c)
        s.add_argument("run")
        s.add_argument("--draw", type=int)
    s = sub.add_parser("words")
    s.add_argument("file")
    s.add_argument("--offset", default="0")
    s.add_argument("--count", type=int, default=64)
    s.add_argument("--float", action="store_true")
    s = sub.add_parser("texel")
    s.add_argument("file")
    s.add_argument("x", type=int)
    s.add_argument("y", type=int)
    s.add_argument("--run", required=True)
    a = ap.parse_args()
    {"targets": cmd_targets, "buffers": cmd_buffers, "words": cmd_words, "texel": cmd_texel}[a.cmd](a)


if __name__ == "__main__":
    main()
