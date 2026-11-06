"""Merge a re-translation into a shader cache: rows of <new>/results.tsv whose status is ok or ok-retry replace the cache's row
for the same hash, and their <hash>.spv files are copied over. Use after translating a subset again (a corpus folder with a
filtered unique.tsv, see AGENTS.md).

Usage: python -I merge_results.py <new dir> <cache dir>
"""
import os
import shutil
import sys


def rows(path):
    with open(path, encoding='utf-8') as f:
        lines = f.read().splitlines()
    return lines[0], {line.split('\t', 1)[0]: line for line in lines[1:] if line}


def main():
    new_dir, cache_dir = sys.argv[1], sys.argv[2]
    head, new = rows(os.path.join(new_dir, 'results.tsv'))
    cache_head, cache = rows(os.path.join(cache_dir, 'results.tsv'))
    if head != cache_head:
        sys.exit('results.tsv headers differ')
    taken = 0
    for h, line in new.items():
        if line.split('\t')[4] not in ('ok', 'ok-retry'):
            continue
        shutil.copyfile(os.path.join(new_dir, h + '.spv'), os.path.join(cache_dir, h + '.spv'))
        cache[h] = line
        taken += 1
    tmp = os.path.join(cache_dir, 'results.tsv.tmp')
    with open(tmp, 'w', encoding='utf-8', newline='\n') as f:
        f.write(cache_head + '\n')
        for h in sorted(cache):
            f.write(cache[h] + '\n')
    os.replace(tmp, os.path.join(cache_dir, 'results.tsv'))
    print(f'{taken} of {len(new)} rows taken; cache has {len(cache)} rows')


if __name__ == '__main__':
    main()
