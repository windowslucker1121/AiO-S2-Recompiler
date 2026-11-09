"""A private shader cache for measuring a translator change without touching the shared one.

Makes DEST a copy of CACHE in which every file is a hard link (no disk space; never written through), except results.tsv (a real copy),
then translates the chosen corpus blocks (and optional runtime dumps) with HARNESS into a temporary folder and puts the results into
DEST: the hard link of a changed .spv is removed first and a new file written, so the shared cache's files stay as they were.

Usage (venv python, -I):
  python -I tools/shaders/scripts/private_cache.py --cache work/shaders/oot/spirv --dest work/gx/cache_v1 --corpus work/shaders/oot/corpus
         (--token gpt | --hashes FILE | PREFIX ...) [--dumps DIR] [--harness PATH] [--env NAME=VALUE ...] [--jobs 12]
  --dumps DIR   code_<hash>.bin / ctrl_<hash>.bin of "runtime" blocks (not in the corpus) to translate too (single mode)
  --refresh     DEST exists: only translate again and replace (the links stay)
  --batch-out DIR  use the rows of a finished --batch output (e.g. a full corpus run with NXRT_HARNESS_CFG_CHECK) instead of translating
                (only the selected blocks' ok rows are taken)
Prints the changed blocks (SPIR-V differs from CACHE) into DEST/changed.txt.
"""
import argparse
import filecmp
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
VARIANTS = ('.cm.spv', '.f16.spv')


def rows(path):
    with open(path, encoding='utf-8') as f:
        lines = f.read().splitlines()
    return lines[0], [ln for ln in lines[1:] if ln]


def put(dest, name, src):
    p = os.path.join(dest, name)
    if os.path.lexists(p):
        os.unlink(p)        # drops the hard link (or old private file); the shared cache's file keeps its content
    shutil.copyfile(src, p)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--cache', required=True)
    ap.add_argument('--dest', required=True)
    ap.add_argument('--corpus', required=True)
    ap.add_argument('--token')
    ap.add_argument('--hashes')
    ap.add_argument('prefixes', nargs='*')
    ap.add_argument('--dumps')
    ap.add_argument('--harness', default=os.path.join(HERE, '..', 'build', 'bin', 'Release', 'net10.0', 'spirv_harness.exe'))
    ap.add_argument('--env', action='append', default=[])
    ap.add_argument('--jobs', type=int, default=12)
    ap.add_argument('--refresh', action='store_true')
    ap.add_argument('--batch-out', help='take the corpus rows from this finished --batch output folder instead of translating')
    a = ap.parse_args()
    cache, dest, corpus = (os.path.abspath(x) for x in (a.cache, a.dest, a.corpus))
    if os.path.normcase(cache) == os.path.normcase(dest):
        sys.exit('dest must not be the cache')

    head, cache_rows = rows(os.path.join(cache, 'results.tsv'))
    if not os.path.isdir(dest):
        if a.refresh:
            sys.exit('--refresh: no ' + dest)
        os.makedirs(dest)
        n = 0
        for e in os.scandir(cache):
            if e.is_file() and e.name != 'results.tsv':
                os.link(e.path, os.path.join(dest, e.name))
                n += 1
        shutil.copyfile(os.path.join(cache, 'results.tsv'), os.path.join(dest, 'results.tsv'))
        print(f'{n} files linked into {dest}')

    want = list(a.prefixes)
    if a.hashes:
        with open(a.hashes, encoding='utf-8') as f:
            want += f.read().split()
    if a.token:
        want += [r.split('\t')[0] for r in cache_rows if a.token in r.split('\t')[7].split()]
    want_set = set(want)
    chead, crow = rows(os.path.join(corpus, 'unique.tsv'))
    sel = [r for r in crow if r.split('\t')[0] in want_set or any(r.split('\t')[0].startswith(p) for p in a.prefixes)]
    env = dict(os.environ)
    for kv in a.env:
        k, _, v = kv.partition('=')
        env[k] = v

    tmp = tempfile.mkdtemp(prefix='pc_', dir=os.path.dirname(dest))
    sub, out = os.path.join(tmp, 'subset'), os.path.join(tmp, 'out')
    os.makedirs(sub)
    links = []
    try:
        new_rows = {}
        if sel:
            with open(os.path.join(sub, 'unique.tsv'), 'w', encoding='utf-8', newline='\n') as f:
                f.write(chead + '\n' + '\n'.join(sel) + '\n')
            for d in ('code', 'ctrl', 'glslc'):
                if os.path.isdir(os.path.join(corpus, d)):
                    link = os.path.join(sub, d)
                    subprocess.run(['cmd', '/c', 'mklink', '/J', link, os.path.join(corpus, d)], check=True, stdout=subprocess.DEVNULL)
                    links.append(link)
            if a.batch_out:
                out = os.path.abspath(a.batch_out)
            else:
                subprocess.run([os.path.abspath(a.harness), '--batch', sub, '--out', out, '--jobs', str(a.jobs), '--no-val'], check=True,
                               env=env, stdout=subprocess.DEVNULL)
            ohead, orows = rows(os.path.join(out, 'results.tsv'))
            for r in orows:
                c = r.split('\t')
                if c[4] in ('ok', 'ok-retry'):
                    new_rows[c[0]] = (r, out)
        if a.dumps:
            dump_out = os.path.join(tmp, 'dumps')
            os.makedirs(dump_out)
            by_hash = {r.split('\t')[0]: r for r in cache_rows}
            for h in want:
                code = os.path.join(a.dumps, f'code_{h}.bin')
                ctrl = os.path.join(a.dumps, f'ctrl_{h}.bin')
                if h in new_rows or not os.path.exists(code) or h not in by_hash:
                    continue
                p = subprocess.run([os.path.abspath(a.harness), code, ctrl, '-o', os.path.join(dump_out, h + '.spv')], env=env,
                                   capture_output=True, text=True)
                first = p.stdout.splitlines()[0] if p.stdout else ''
                res = [ln for ln in p.stdout.splitlines() if ln.startswith('resources: ')]
                if ' ok ' not in first or not os.path.exists(os.path.join(dump_out, h + '.spv')):
                    print(f'dump {h}: {first}')
                    continue
                c = by_hash[h].split('\t')
                c[7] = res[0][len('resources: '):] if res else c[7]
                c[6] = str(os.path.getsize(os.path.join(dump_out, h + '.spv')))
                new_rows[h] = ('\t'.join(c), dump_out)
        changed = []
        for h, (r, src) in new_rows.items():
            old = os.path.join(cache, h + '.spv')
            new = os.path.join(src, h + '.spv')
            if os.path.exists(old) and filecmp.cmp(old, new, shallow=False):
                continue
            put(dest, h + '.spv', new)
            for v in VARIANTS:
                if os.path.exists(os.path.join(src, h + v)):
                    put(dest, h + v, os.path.join(src, h + v))
            changed.append(h)
        dhead, drows = rows(os.path.join(dest, 'results.tsv'))
        rep = {h: new_rows[h][0] for h in changed}
        drows = [rep.get(r.split('\t')[0], r) for r in drows]
        with open(os.path.join(dest, 'results.tsv.tmp'), 'w', encoding='utf-8', newline='\n') as f:
            f.write(dhead + '\n' + '\n'.join(drows) + '\n')
        os.replace(os.path.join(dest, 'results.tsv.tmp'), os.path.join(dest, 'results.tsv'))
        with open(os.path.join(dest, 'changed.txt'), 'w', encoding='utf-8', newline='\n') as f:
            f.write('\n'.join(sorted(changed)) + '\n')
        print(f'{len(sel)} corpus blocks, {len(new_rows)} translated ok, {len(changed)} changed -> {dest}')
    finally:
        for link in links:
            os.rmdir(link)
        if not any(os.path.lexists(l) for l in links):
            shutil.rmtree(tmp, ignore_errors=True)
        else:
            print('links left in ' + tmp)


if __name__ == '__main__':
    main()
