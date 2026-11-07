#!/usr/bin/env python3
"""A small shader-preparation test set: a random sample of a programs.bin and the shader cache entries its programs need, copied into a
folder of its own (so a test preparation never writes the real cache's pipeline_cache.bin or prepare.state).

    python -I programs_subset.py --programs <cache>/../programs.bin --cache <cache spirv dir> --out <dir> [--count 1152] [--seed 1]

Writes <out>/programs.bin (the sample, format of read_programs_file: "MPRPRG01", u32 count, u32 0, then {u64 vs, fs, gs}) and
<out>/spirv/ (the .spv files of the sample's blocks and their results.tsv rows). Run the preparation on it with
    NXRT_NVN_SHADER_CACHE=<out>/spirv NXRT_NVN_SHADER_TRANSLATOR=off <game>.exe --game <empty dir> --prepare-shaders --programs <out>/programs.bin
and a fresh NXRT_NVN_SPIRV_SALT per run, so the driver's own disk cache cannot answer (every part is compiled again).
"""
import argparse, os, random, shutil, struct


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--programs', required=True)
    ap.add_argument('--cache', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--count', type=int, default=1152)
    ap.add_argument('--seed', type=int, default=1)
    a = ap.parse_args()
    data = open(a.programs, 'rb').read()
    if data[:8] != b'MPRPRG01':
        raise SystemExit(f'{a.programs}: not a program list')
    n = struct.unpack_from('<I', data, 8)[0]
    progs = [struct.unpack_from('<QQQ', data, 16 + 24 * i) for i in range(n)]
    rows, header = {}, None
    with open(os.path.join(a.cache, 'results.tsv'), encoding='utf-8', newline='') as f:
        for line in f:
            if header is None:
                header = line
                continue
            rows[line.split('\t', 1)[0]] = line
    usable = [p for p in progs if all(h == 0 or f'{h:016x}' in rows for h in p)]
    random.Random(a.seed).shuffle(usable)
    pick = usable[:a.count]
    spirv = os.path.join(a.out, 'spirv')
    os.makedirs(spirv, exist_ok=True)
    hashes = sorted({f'{h:016x}' for p in pick for h in p if h})
    for h in hashes:
        src = os.path.join(a.cache, h + '.spv')
        if os.path.isfile(src):
            shutil.copyfile(src, os.path.join(spirv, h + '.spv'))
    with open(os.path.join(spirv, 'results.tsv'), 'w', encoding='utf-8', newline='') as f:
        f.write(header)
        f.write(''.join(rows[h] for h in hashes))
    with open(os.path.join(a.out, 'programs.bin'), 'wb') as f:
        f.write(b'MPRPRG01' + struct.pack('<II', len(pick), 0))
        f.write(b''.join(struct.pack('<QQQ', *p) for p in pick))
    print(f'{len(pick)} of {n} programs ({len(usable)} with every block in the cache), {len(hashes)} blocks -> {a.out}')


if __name__ == '__main__':
    main()
