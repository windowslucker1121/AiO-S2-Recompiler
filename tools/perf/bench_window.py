"""fps of a benchmark window: frames of [diag] frame_csv between two log markers.

bench_window.py <log> <frames.csv> --from "bench: village_walk start" --to "bench: village_walk end"
bench_window.py <log> <frames.csv> --from "bench: balcony_far ready" --skip 30 --seconds 60
bench_window.py --walk <log> <frames.csv> [<log> <frames.csv> ...]       # presets; several runs: one line each plus all runs together
bench_window.py --balcony <log> <frames.csv> [...]
bench_window.py --turns <log> <frames.csv> [...]                         # the three windows of bench/village_turns.nxs, pooled
bench_window.py --first-scene | --house <log> <frames.csv> [...]          # bench/first_scene.nxs (170 s), bench/house_turn.nxs (two turns)
bench_window.py --walk --causes <log> <frames.csv> [...]                 # plus the slow frames by cause (PLAN_LOWS 1.1)
bench_window.py --run DIR [DIR ...] --balcony                            # a run folder holding oot.log + frames.csv
bench_window.py --turns --images --run DIR                               # plus image creation per window and the run's pt_copy lines

Prints frames, mean fps (frames / summed intervals), 1% low (fps of the mean of the slowest 1% of frame intervals), median and worst
frame, and the frames over 14.3 ms and over 20 ms (the walk's targets). `--phases a,b,...` (seconds after the window opens; `--walk`
sets 0.25,0.68,5.0: start, first-sight burst, ladder, open view) adds one line per phase. With several runs a line "all" pools their
frames: the walk's 1% low is about seven frames per run, so judge it on three runs. A preset with several windows (`--turns`) prints
one line per window and pools them per run.
`--causes` sorts the frames over 14.3 ms by cause (first match; needs `[diag] frame_csv_threads` for the render-CPU part of "stall"):
  stall     recorder drain wait over 5 ms, or render-thread CPU 5 ms short of its replay with no image creation over 3 ms and no idle
  rt-block  image creation over 3 ms in the frame               fetch     held-result fetch over 2 ms with the render thread 20% idle
  game      render thread idle 25% of the frame                 upload    over 1 MB of texture uploads
  rebuild   30 or more kept sets rebuilt                        pipeline  render-thread pipeline miss / build
  rt-churn  8 or more images made                               sets      sets built over 1.6x the window median
  draws     draws over 1.15x the window median                  other
and prints, per cause, the frames over 14.3 ms, those over 20 ms and the share of the excess time (frame minus the window median).
`--images` (PLAN_LOWS M5) adds per window the images made per second (`textures_created`), the render thread's image creation ms per
second (`ms_r_tex_create`) and the frames with over 3 ms of it, and per run the last `pt_copy:` statistics line, the pools refused by
`pt_copy_max_mb` (with their sizes) and the last `tex_rt_pool` statistics line. Images made are not split into targets and assets: that
needs a frame_csv column in the runtime (not added by this tool).
The CSV's t_s counts from the hitch monitor's start (the log line "diag: hitch log", ~0.6 s after the log opened): log times are moved
onto that clock (before 2026-11-13 the window was taken 0.6 s late).
"""
import argparse
import collections
import csv
import os
import re
import statistics
import sys

PRESETS = {
    'walk': dict(windows=[('walk', 'bench: village_walk start', 'bench: village_walk end', 0.0, None)], phases='0.25,0.68,5.0'),
    'balcony': dict(windows=[('balcony', 'bench: balcony_far ready', None, 30.0, 60.0)], phases=None),
    'turns': dict(windows=[('turn1', 'bench: village_turn1 start', 'bench: village_turn1 end', 0.0, None),
                           ('turn2', 'bench: village_turn2 start', 'bench: village_turn2 end', 0.0, None),
                           ('run', 'bench: village_run start', 'bench: village_run end', 0.0, None)], phases=None),
    'first_scene': dict(windows=[('first_scene', 'bench: first_scene start', 'bench: first_scene end', 0.0, None)], phases=None),
    'house': dict(windows=[('house1', 'bench: house_turn1 start', 'bench: house_turn1 end', 0.0, None),
                           ('house2', 'bench: house_turn2 start', 'bench: house_turn2 end', 0.0, None)], phases=None),
}

ap = argparse.ArgumentParser()
ap.add_argument('files', nargs='*', help='<log> <frames.csv> pairs')
ap.add_argument('--run', nargs='+', default=[], help='run folders with oot.log and frames.csv (instead of pairs)')
ap.add_argument('--walk', action='store_true', help='preset: the walk window, phases 0.25 / 0.68 / 5.0 s')
ap.add_argument('--balcony', action='store_true', help='preset: 30..90 s after "bench: balcony_far ready"')
ap.add_argument('--turns', action='store_true', help='preset: the two camera turns and the run of bench/village_turns.nxs')
ap.add_argument('--first-scene', dest='first_scene', action='store_true', help='preset: the 170 s of bench/first_scene.nxs')
ap.add_argument('--house', action='store_true', help='preset: the two camera turns of bench/house_turn.nxs')
ap.add_argument('--from', dest='frm', help='text of the log line that opens the window (first match)')
ap.add_argument('--to', help='text of the log line that closes it (first match after --from)')
ap.add_argument('--skip', type=float, help='seconds after --from before the window opens')
ap.add_argument('--seconds', type=float, help='window length (instead of --to)')
ap.add_argument('--phases', help='phase boundaries in seconds after the window opens, comma separated')
ap.add_argument('--causes', action='store_true', help='slow frames by cause (PLAN_LOWS classes)')
ap.add_argument('--images', action='store_true', help='image creation per window, pt_copy / tex_rt_pool lines of the run (M5)')
a = ap.parse_args()

pname = next((k for k in ('walk', 'balcony', 'turns', 'first_scene', 'house') if getattr(a, k)), None)
if pname and not (a.frm or a.to or a.skip is not None or a.seconds is not None):
    wins = PRESETS[pname]['windows']
else:
    w0 = PRESETS[pname]['windows'][0] if pname else ('window', None, None, 0.0, None)
    wins = [('window', a.frm or w0[1], a.to if a.to is not None else w0[2], a.skip if a.skip is not None else w0[3],
             a.seconds if a.seconds is not None else w0[4])]
phases = [float(x) for x in (a.phases or (PRESETS[pname]['phases'] if pname else None) or '').split(',') if x.strip()]
if not wins[0][1]:
    sys.exit('--from (or --walk / --balcony / --turns) is required')
pairs = [(a.files[i], a.files[i + 1]) for i in range(0, len(a.files) - 1, 2)]
if len(a.files) % 2:
    sys.exit('files come in <log> <frames.csv> pairs')
pairs += [(os.path.join(d, 'oot.log'), os.path.join(d, 'frames.csv')) for d in a.run]
if not pairs:
    sys.exit('no runs given')

rx = re.compile(r'^\[\s*([\d.]+)')


def windows_of(log):
    """(name, t0, t1) on the CSV clock for every window of the preset."""
    lines = []
    csv0 = 0.0
    with open(log, encoding='utf-8', errors='replace') as f:
        for ln in f:
            m = rx.match(ln)
            if not m:
                continue
            if 'diag: hitch log' in ln and not csv0:
                csv0 = float(m.group(1))
            lines.append((float(m.group(1)), ln))
    out = []
    for name, frm, to, skip, seconds in wins:
        t0 = t1 = None
        for t, ln in lines:
            if t0 is None and frm in ln:
                t0 = t
            elif t0 is not None and to and to in ln:
                t1 = t
                break
        if t0 is None:
            if len(wins) > 1:
                print(f'{log}: no log line with {frm!r}: window {name} left out')
                continue
            sys.exit(f'{log}: no log line with {frm!r}')
        t0 += skip - csv0
        if t1 is not None:
            t1 -= csv0
        if seconds:
            t1 = t0 + seconds
        if t1 is None:
            sys.exit(f'{log}: window {name} has no end: --to not found and no --seconds')
        out.append((name, t0, t1))
    return out


def stats(ms):
    srt = sorted(ms, reverse=True)
    low = srt[:max(1, len(ms) // 100)]
    return (f'{len(ms)} frames, {1000 * len(ms) / sum(ms):.1f} fps, 1% low {1000 / statistics.mean(low):.1f} fps, '
            f'median {statistics.median(ms):.2f} ms, worst {srt[0]:.1f} ms, over 14.3 ms {sum(m > 14.3 for m in ms)}, '
            f'over 20 ms {sum(m > 20.0 for m in ms)}')


def fv(r, c):
    try:
        return float(r.get(c) or 0)
    except ValueError:
        return 0.0


CAUSES = ('stall', 'rt-block', 'fetch', 'game', 'upload', 'rebuild', 'pipeline', 'rt-churn', 'sets', 'draws', 'other')


def cause(r, med):
    m = fv(r, 'ms')
    short = fv(r, 'ms_r_replay') - fv(r, 'cpu_render_us') / 1000 if 'cpu_render_us' in r else 0.0
    if fv(r, 'ring_drain_us') > 5000 or (short > 5 and fv(r, 'ms_r_tex_create') < 3 and fv(r, 'ms_r_idle') < 1):
        return 'stall'
    if fv(r, 'ms_r_tex_create') >= 3:
        return 'rt-block'
    if fv(r, 'ms_out_fetch') >= 2 and fv(r, 'ms_r_idle') >= 0.2 * m:
        return 'fetch'
    if fv(r, 'ms_r_idle') >= 0.25 * m:
        return 'game'
    if fv(r, 'texture_upload_kb') >= 1024:
        return 'upload'
    if fv(r, 'keep_rebuilt') >= 30:
        return 'rebuild'
    if max(fv(r, 'ms_r_pipe_miss'), fv(r, 'ms_r_pipeline')) >= 1.5 or (
            fv(r, 'pipelines_created') >= 1 and fv(r, 'ms_r_replay') > med['ms_r_replay'] + 5):
        return 'pipeline'
    if fv(r, 'textures_created') >= 8:
        return 'rt-churn'
    if fv(r, 'sets_built') >= 1.6 * med['sets_built']:
        return 'sets'
    if fv(r, 'draws') >= 1.15 * med['draws']:
        return 'draws'
    return 'other'


class Causes:
    def __init__(self):
        self.n, self.n20, self.exc = collections.Counter(), collections.Counter(), collections.Counter()

    def add(self, rows):
        med = {c: statistics.median(fv(r, c) for r in rows) for c in ('ms', 'draws', 'sets_built', 'ms_r_replay')}
        for r in rows:
            m = fv(r, 'ms')
            if m <= 14.3:
                continue
            c = cause(r, med)
            self.n[c] += 1
            self.exc[c] += m - med['ms']
            if m > 20:
                self.n20[c] += 1

    def line(self):
        tot = sum(self.exc.values()) or 1
        return '  causes (over 14.3 / over 20 / excess share): ' + ', '.join(
            f'{c} {self.n[c]}/{self.n20[c]}/{100 * self.exc[c] / tot:.0f}%' for c in CAUSES if self.n[c]) if self.n else '  causes: none over 14.3 ms'


def image_line(sel, t0, t1):
    s = max(t1 - t0, 1e-6)
    made = sum(fv(r, 'textures_created') for r in sel)
    ms = sum(fv(r, 'ms_r_tex_create') for r in sel)
    return (f'  images: {made / s:.0f} made per second, image creation {ms / s:.1f} ms per second, '
            f'{sum(fv(r, "ms_r_tex_create") > 3 for r in sel)} frames over 3 ms of it')


def run_image_lines(log):
    last_pt = last_rt = None
    refused = collections.OrderedDict()
    rxr = re.compile(r'pt_copy: pool (\S+) \((\d+) KiB\) is read through pointers but larger than pt_copy_max_mb')
    with open(log, encoding='utf-8', errors='replace') as f:
        for ln in f:
            if 'pt_copy: ' in ln and ' pools, ' in ln and 'read from the copy' in ln:
                last_pt = ln.split('] ', 1)[-1].strip()
            elif 'render targets from parked images of the same layout' in ln:
                last_rt = ln.split('] ', 1)[-1].strip()
            m = rxr.search(ln)
            if m:
                refused[m.group(1)] = int(m.group(2)) >> 10
    out = [f'  {last_pt or "pt_copy: no statistics line"}']
    if refused:
        out.append('  pt_copy refused (over pt_copy_max_mb): ' + ', '.join(f'{k} {v} MiB' for k, v in refused.items()))
    if last_rt:
        out.append(f'  {last_rt}')
    return out


def phase_lines(rows, acc=None):
    edges = [0.0] + phases + [float('inf')]
    for k in range(len(edges) - 1):
        pm = [m for t, m in rows if edges[k] < t <= edges[k + 1]]
        if acc is not None:
            acc[k] += pm
        hi = 'end' if edges[k + 1] == float('inf') else f'{edges[k + 1]:.2f}'
        print(f'  phase {edges[k]:.2f}..{hi} s: ' + (stats(pm) if pm else 'no frames'))


allms = []
allph = [[] for _ in range(len(phases) + 1)]
allc = Causes()
perwin = collections.OrderedDict()
for log, csvf in pairs:
    ws = windows_of(log)
    with open(csvf, newline='') as f:
        rows_all = list(csv.DictReader(f))
    runms = []
    runc = Causes()
    for name, t0, t1 in ws:
        sel = [r for r in rows_all if t0 < float(r['t_s']) <= t1]
        if len(sel) < 10:
            sys.exit(f'{csvf}: only {len(sel)} frames in {t0:.1f}..{t1:.1f} s ({name})')
        rows = [(float(r['t_s']) - t0, float(r['ms'])) for r in sel]
        ms = [m for _, m in rows]
        runms += ms
        perwin.setdefault(name, []).extend(ms)
        tag = f'{name} ' if len(wins) > 1 else ''
        print(f'{tag}window {t0:.1f}..{t1:.1f} s: {stats(ms)}')
        if phases:
            phase_lines(rows, allph)
        if a.images:
            print(image_line(sel, t0, t1))
        if a.causes:
            runc.add(sel)
            allc.add(sel)
    allms += runms
    if len(ws) > 1:
        print(f'run {log}: {len(ws)} windows: {stats(runms)}')
    if a.causes:
        print(runc.line())
    if a.images:
        for ln in run_image_lines(log):
            print(ln)
if len(pairs) > 1 or len(wins) > 1:
    if len(wins) > 1 and len(pairs) > 1:
        for name, ms in perwin.items():
            print(f'all {name}: {stats(ms)}')
    print(f'all {len(pairs)} runs: {stats(allms)}')
    if phases and len(pairs) > 1:
        edges = [0.0] + phases + [float('inf')]
        for k in range(len(edges) - 1):
            hi = 'end' if edges[k + 1] == float('inf') else f'{edges[k + 1]:.2f}'
            print(f'  phase {edges[k]:.2f}..{hi} s: ' + (stats(allph[k]) if allph[k] else 'no frames'))
    if a.causes and len(pairs) > 1:
        print(allc.line())
