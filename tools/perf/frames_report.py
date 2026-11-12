# SPDX-License-Identifier: GPL-3.0-or-later
"""frames_report.py <frames.csv> ([diag] frame_csv) [t0=60] [t1=1e9]: fps, 1% low, frames over 10/12/15/20 ms, worst; medians of replay / idle / sync wait;
the 4-frame cycle: frames grouped by their draw-count rank within each 4-frame block (heaviest first), mean ms per rank."""
import sys, csv, statistics as st
path = sys.argv[1]; t0 = float(sys.argv[2]) if len(sys.argv) > 2 else 60; t1 = float(sys.argv[3]) if len(sys.argv) > 3 else 1e9
rows = [r for r in csv.DictReader(open(path)) if t0 <= float(r['t_s']) < t1]
if not rows: print('no frames'); sys.exit()
ms = [float(r['ms']) for r in rows]
s = sorted(ms); low = s[-max(1, len(s) // 100):]
n = len(ms)
def c(th): return sum(m > th for m in ms)
print(f'{n} frames: {1000*n/sum(ms):.1f} fps, 1% low {1000/(sum(low)/len(low)):.1f}, >10 {100*c(10)/n:.1f}%, >12 {100*c(12)/n:.1f}%, '
      f'>15 {100*c(15)/n:.2f}% ({c(15)}), >20 {c(20)}, >33 {c(33.4)}, worst {s[-1]:.1f} ms, median {st.median(ms):.2f}')
def med(k):
    try: return st.median(float(r[k]) for r in rows)
    except Exception: return float('nan')
def mean(k):
    try: return st.mean(float(r[k]) for r in rows)
    except Exception: return float('nan')
print(f'replay median {med("ms_r_replay"):.2f} mean {mean("ms_r_replay"):.2f}; render idle mean {mean("ms_r_idle"):.2f}; game sync wait mean {mean("ms_g_sync_wait"):.2f}; '
      f'draws mean {mean("draws"):.0f}; pipelines created {sum(float(r["pipelines_created"]) for r in rows):.0f}')
# 4-frame cycle by draw count rank
rank = [[], [], [], []]
for i in range(0, n - 3, 4):
    blk = rows[i:i + 4]
    order = sorted(range(4), key=lambda j: -float(blk[j]['draws']))
    for k, j in enumerate(order): rank[k].append(float(blk[j]['ms']))
print('ms by draw rank in 4-frame blocks: ' + ' '.join(f'{st.mean(v):.2f}' for v in rank if v))
