"""Translate a subset of a corpus again and merge it into a shader cache, cleaning up after itself.

The manual route (AGENTS.md) left a subset folder (filtered unique.tsv + junctions to the corpus) and a batch output folder per try in
work/shaders. This does the same in a temporary folder and removes both when done; only the backup (when asked for) stays.

Usage (from the checkout, with the venv's python):
  python -I tools/shaders/scripts/retranslate_subset.py --corpus work/shaders/oot/corpus --cache work/shaders/oot/spirv
         (--hashes FILE | --token gpt | HASH ...) [--harness PATH] [--jobs 16] [--backup DIR] [--keep-temp] [--env NAME=VALUE ...]

  --hashes FILE   hash prefixes, whitespace-separated (a results.tsv row's first column starts with one)
  --token T       every cache row whose resources column carries the token T (e.g. gpt: the A17 pointer shaders)
  --backup DIR    copies the cache's .spv files and results.tsv into DIR before they are replaced
  --keep-temp     keep the temporary subset and output folders (to look at the batch output)
Rows only the on-demand translator made (blocks not in the corpus) are not touched; delete their rows to have the runtime translate
them again.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import merge_results  # noqa: E402


def rows(path):
    with open(path, encoding='utf-8') as f:
        lines = f.read().splitlines()
    return lines[0], [ln for ln in lines[1:] if ln]


def junction(link, target):
    if os.name == 'nt':
        subprocess.run(['cmd', '/c', 'mklink', '/J', link, target], check=True, stdout=subprocess.DEVNULL)
    else:
        os.symlink(target, link)


def unlink_dir(link):
    # a junction or symlink goes, never what it points at
    if os.path.islink(link) or (os.name == 'nt' and os.path.isdir(link)):
        try:
            os.rmdir(link)        # removes a junction (or an empty folder) without touching the target
            return
        except OSError:
            pass
    if os.path.islink(link):
        os.unlink(link)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--corpus', required=True)
    ap.add_argument('--cache', required=True)
    ap.add_argument('--hashes')
    ap.add_argument('--token')
    ap.add_argument('prefixes', nargs='*')
    ap.add_argument('--harness', default=os.path.join(HERE, '..', 'build', 'bin', 'Release', 'net10.0', 'spirv_harness.exe'))
    ap.add_argument('--jobs', type=int, default=16)
    ap.add_argument('--backup')
    ap.add_argument('--keep-temp', action='store_true')
    ap.add_argument('--env', action='append', default=[])
    a = ap.parse_args()

    corpus, cache = os.path.abspath(a.corpus), os.path.abspath(a.cache)
    want = list(a.prefixes)
    if a.hashes:
        with open(a.hashes, encoding='utf-8') as f:
            want += f.read().split()
    cache_head, cache_rows = rows(os.path.join(cache, 'results.tsv'))
    if a.token:
        want += [r.split('\t')[0] for r in cache_rows if a.token in r.split('\t')[7].split()]
    if not want:
        sys.exit('no hashes: --hashes, --token or prefixes')
    head, corpus_rows = rows(os.path.join(corpus, 'unique.tsv'))
    sel = [r for r in corpus_rows if any(r.split('\t')[0].startswith(p) for p in want)]
    print(f'{len(sel)} corpus blocks of {len(want)} requested hashes')
    if not sel:
        return

    tmp = tempfile.mkdtemp(prefix='retranslate_', dir=os.path.dirname(cache))
    sub, out = os.path.join(tmp, 'subset'), os.path.join(tmp, 'out')
    os.makedirs(sub)
    try:
        with open(os.path.join(sub, 'unique.tsv'), 'w', encoding='utf-8', newline='\n') as f:
            f.write(head + '\n' + '\n'.join(sel) + '\n')
        for d in ('code', 'ctrl', 'glslc'):
            if os.path.isdir(os.path.join(corpus, d)):
                junction(os.path.join(sub, d), os.path.join(corpus, d))
        env = dict(os.environ)
        for kv in a.env:
            k, _, v = kv.partition('=')
            env[k] = v
        subprocess.run([os.path.abspath(a.harness), '--batch', sub, '--out', out, '--jobs', str(a.jobs), '--no-val'], check=True, env=env)
        if a.backup:
            os.makedirs(a.backup, exist_ok=True)
            _, new_rows = rows(os.path.join(out, 'results.tsv'))
            for r in new_rows:
                h = r.split('\t')[0]
                for ext in ('.spv',) + merge_results.VARIANTS:
                    src = os.path.join(cache, h + ext)
                    if os.path.exists(src) and not os.path.exists(os.path.join(a.backup, h + ext)):
                        shutil.copyfile(src, os.path.join(a.backup, h + ext))
            if not os.path.exists(os.path.join(a.backup, 'results.tsv')):
                shutil.copyfile(os.path.join(cache, 'results.tsv'), os.path.join(a.backup, 'results.tsv'))
        sys.argv = ['merge_results.py', out, cache]
        merge_results.main()
    finally:
        if a.keep_temp:
            print(f'kept {tmp}')
        else:
            for d in ('code', 'ctrl', 'glslc'):
                unlink_dir(os.path.join(sub, d))
            left = [d for d in ('code', 'ctrl', 'glslc') if os.path.lexists(os.path.join(sub, d))]
            if left:   # never remove a tree that still links into the corpus
                print(f'could not remove the links {left} in {sub}: {tmp} left in place')
            else:
                shutil.rmtree(tmp, ignore_errors=True)


if __name__ == '__main__':
    main()
