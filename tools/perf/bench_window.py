"""fps of a benchmark window: frames of [diag] frame_csv between two log markers.

bench_window.py <log> <frames.csv> --from "bench: village_walk start" --to "bench: village_walk end"
bench_window.py <log> <frames.csv> --from "bench: balcony_far ready" --skip 30 --seconds 60
bench_window.py --walk <log> <frames.csv> [<log> <frames.csv> ...]       # presets; several runs: one line each plus all runs together
bench_window.py --balcony <log> <frames.csv> [...]

Prints frames, mean fps (frames / summed intervals), 1% low (fps of the mean of the slowest 1% of frame intervals), median and worst
frame, and the frames over 14.3 ms and over 20 ms (the walk's targets). `--phases a,b,...` (seconds after the window opens; `--walk`
sets 0.25,0.68,5.0: start, first-sight burst, ladder, open view) adds one line per phase. With several runs a line "all" pools their
frames: the walk's 1% low is about seven frames per run, so judge it on three runs.
The CSV's t_s counts from the hitch monitor's start (the log line "diag: hitch log", ~0.6 s after the log opened): log times are moved
onto that clock (before 2026-11-13 the window was taken 0.6 s late).
"""
import argparse
import csv
import re
import statistics
import sys

PRESETS = {
    'walk': dict(frm='bench: village_walk start', to='bench: village_walk end', skip=0.0, seconds=None, phases='0.25,0.68,5.0'),
    'balcony': dict(frm='bench: balcony_far ready', to=None, skip=30.0, seconds=60.0, phases=None),
}

ap = argparse.ArgumentParser()
ap.add_argument('files', nargs='+', help='<log> <frames.csv> pairs')
ap.add_argument('--walk', action='store_true', help='preset: the walk window, phases 0.25 / 0.68 / 5.0 s')
ap.add_argument('--balcony', action='store_true', help='preset: 30..90 s after "bench: balcony_far ready"')
ap.add_argument('--from', dest='frm', help='text of the log line that opens the window (first match)')
ap.add_argument('--to', help='text of the log line that closes it (first match after --from)')
ap.add_argument('--skip', type=float, help='seconds after --from before the window opens')
ap.add_argument('--seconds', type=float, help='window length (instead of --to)')
ap.add_argument('--phases', help='phase boundaries in seconds after the window opens, comma separated')
a = ap.parse_args()

pre = PRESETS['walk'] if a.walk else PRESETS['balcony'] if a.balcony else dict(frm=None, to=None, skip=0.0, seconds=None, phases=None)
frm = a.frm or pre['frm']
to = a.to if a.to is not None else pre['to']
skip = a.skip if a.skip is not None else pre['skip']
seconds = a.seconds if a.seconds is not None else pre['seconds']
phases = [float(x) for x in (a.phases or pre['phases'] or '').split(',') if x.strip()]
if not frm:
    sys.exit('--from (or --walk / --balcony) is required')
if len(a.files) % 2:
    sys.exit('files come in <log> <frames.csv> pairs')

rx = re.compile(r'^\[\s*([\d.]+)')


def window(log):
    t0 = t1 = None
    csv0 = 0.0
    with open(log, encoding='utf-8', errors='replace') as f:
        for ln in f:
            m = rx.match(ln)
            if not m:
                continue
            if 'diag: hitch log' in ln and not csv0:
                csv0 = float(m.group(1))
            if t0 is None and frm in ln:
                t0 = float(m.group(1))
            elif t0 is not None and to and to in ln:
                t1 = float(m.group(1))
                break
    if t0 is None:
        sys.exit(f'{log}: no log line with {frm!r}')
    t0 += skip - csv0
    if t1 is not None:
        t1 -= csv0
    if seconds:
        t1 = t0 + seconds
    if t1 is None:
        sys.exit(f'{log}: the window has no end: --to not found and no --seconds')
    return t0, t1


def stats(ms):
    srt = sorted(ms, reverse=True)
    low = srt[:max(1, len(ms) // 100)]
    return (f'{len(ms)} frames, {1000 * len(ms) / sum(ms):.1f} fps, 1% low {1000 / statistics.mean(low):.1f} fps, '
            f'median {statistics.median(ms):.2f} ms, worst {srt[0]:.1f} ms, over 14.3 ms {sum(m > 14.3 for m in ms)}, '
            f'over 20 ms {sum(m > 20.0 for m in ms)}')


allms = []
allph = [[] for _ in range(len(phases) + 1)]
for i in range(0, len(a.files), 2):
    log, csvf = a.files[i], a.files[i + 1]
    t0, t1 = window(log)
    rows = []
    with open(csvf, newline='') as f:
        for r in csv.DictReader(f):
            t = float(r['t_s'])
            if t0 < t <= t1:
                rows.append((t - t0, float(r['ms'])))
    if len(rows) < 10:
        sys.exit(f'{csvf}: only {len(rows)} frames in {t0:.1f}..{t1:.1f} s')
    ms = [m for _, m in rows]
    allms += ms
    print(f'window {t0:.1f}..{t1:.1f} s: {stats(ms)}')
    if phases:
        edges = [0.0] + phases + [float('inf')]
        for k in range(len(edges) - 1):
            pm = [m for t, m in rows if edges[k] < t <= edges[k + 1]]
            allph[k] += pm
            hi = 'end' if edges[k + 1] == float('inf') else f'{edges[k + 1]:.2f}'
            print(f'  phase {edges[k]:.2f}..{hi} s: ' + (stats(pm) if pm else 'no frames'))
if len(a.files) > 2:
    print(f'all {len(a.files) // 2} runs: {stats(allms)}')
    if phases:
        edges = [0.0] + phases + [float('inf')]
        for k in range(len(edges) - 1):
            hi = 'end' if edges[k + 1] == float('inf') else f'{edges[k + 1]:.2f}'
            print(f'  phase {edges[k]:.2f}..{hi} s: ' + (stats(allph[k]) if allph[k] else 'no frames'))
