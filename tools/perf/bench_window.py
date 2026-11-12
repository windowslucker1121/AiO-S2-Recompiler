"""fps of a benchmark window: frames of [diag] frame_csv between two log markers.

bench_window.py <log> <frames.csv> --from "bench: village_walk start" --to "bench: village_walk end"
bench_window.py <log> <frames.csv> --from "bench: balcony_far ready" --skip 30 --seconds 60

Prints frames, mean fps (frames / summed intervals), 1% low (fps of the mean of the slowest 1% of frame intervals), median and worst
frame. The log's leading "[ seconds" and the CSV's t_s use the same process clock (seconds since the log opened).
"""
import argparse
import csv
import re
import statistics
import sys

ap = argparse.ArgumentParser()
ap.add_argument('log')
ap.add_argument('csv')
ap.add_argument('--from', dest='frm', required=True, help='text of the log line that opens the window (first match)')
ap.add_argument('--to', help='text of the log line that closes it (first match after --from)')
ap.add_argument('--skip', type=float, default=0, help='seconds after --from before the window opens')
ap.add_argument('--seconds', type=float, help='window length (instead of --to)')
a = ap.parse_args()

t0 = t1 = None
rx = re.compile(r'^\[\s*([\d.]+)')
with open(a.log, encoding='utf-8', errors='replace') as f:
    for ln in f:
        m = rx.match(ln)
        if not m:
            continue
        if t0 is None and a.frm in ln:
            t0 = float(m.group(1))
        elif t0 is not None and a.to and a.to in ln:
            t1 = float(m.group(1))
            break
if t0 is None:
    sys.exit(f'no log line with {a.frm!r}')
t0 += a.skip
if a.seconds:
    t1 = t0 + a.seconds
if t1 is None:
    sys.exit('the window has no end: --to not found and no --seconds')
ms = []
with open(a.csv, newline='') as f:
    for r in csv.DictReader(f):
        t = float(r['t_s'])
        if t0 < t <= t1:
            ms.append(float(r['ms']))
if len(ms) < 10:
    sys.exit(f'only {len(ms)} frames in {t0:.1f}..{t1:.1f} s')
srt = sorted(ms, reverse=True)
low = srt[:max(1, len(ms) // 100)]
print(f'window {t0:.1f}..{t1:.1f} s: {len(ms)} frames, {1000 * len(ms) / sum(ms):.1f} fps, 1% low {1000 / statistics.mean(low):.1f} fps, '
      f'median {statistics.median(ms):.2f} ms, worst {srt[0]:.1f} ms')
