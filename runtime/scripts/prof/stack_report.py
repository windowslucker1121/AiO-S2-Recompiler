"""Summarise <trace>.stacks (the stall sampler, [prof] stall_ms): per thread, which top frames were running or waiting during stalls.
  python stack_report.py trace.stacks [--all]   (default hides threads parked in a kernel wait / sleep)"""
import re, sys, collections
path = sys.argv[1]; show_all = '--all' in sys.argv
txt = open(path, errors='replace').read()
samples = txt.split('=== stall sample ')[1:]
if '--skip' in sys.argv:       # --skip S <trace>: drop samples in the first S seconds after the first frame of that trace file
    S = float(sys.argv[sys.argv.index('--skip') + 1]); tr = sys.argv[sys.argv.index('--skip') + 2]
    sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
    import ftrace_report as fr
    z, th, ev = fr.load(tr)
    f0 = min(e[0] for e in ev if z.get(e[3]) == 'frame')
    samples = [x for x in samples if int(re.search(r't=(\d+) ns', x).group(1)) >= f0 + S * 1e9]
WAIT = ('block_on_wake', 'wait_synchronization', 'sleep_ns', 'wait_process_wide_key', 'wait_for_address')
cnt = collections.defaultdict(collections.Counter)
guestfn = collections.defaultdict(collections.Counter)
for s in samples:
    for th in s.split('\nthread ')[1:]:
        name = th.split()[0]
        frames = re.findall(r'^\s+#\d+ (.*)$', th, re.M)
        mp = [f for f in frames if re.match(r'[\w.-]+\.exe\+0x', f)]   # frames in the game exe
        top = ' < '.join(re.sub(r'^[\w.-]+\.exe\+0x[0-9a-f]+ ', '', f)[:90] for f in mp[:4]) if mp else (frames[0] if frames else '?')
        waiting = any(w in top for w in WAIT[:3]) or ('wait' in (mp[0] if mp else '') and not show_all)
        if any(w in ' '.join(mp[:3]) for w in WAIT) and not show_all: continue
        cnt[name][top] += 1
        m = re.search(r'guest call stack: (.*)', th)
        if m: guestfn[name][' '.join(m.group(1).split(' <- ')[:6])] += 1
print('%d samples' % len(samples))
for name, c in sorted(cnt.items(), key=lambda kv: -sum(kv[1].values())):
    print('\n%s: %d samples not parked' % (name, sum(c.values())))
    for k, v in c.most_common(6): print('   %4d  %s' % (v, k))
    for k, v in guestfn[name].most_common(2): print('        guest: %4d  %s' % (v, k))
