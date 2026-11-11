#!/usr/bin/env python3
"""Program list (programs.bin) of the shader preparation from the platform shader archives (BFSHA/BNSH, NVN2): which vertex, fragment and
geometry blocks are used together, so `<game>.exe --prepare-shaders` can compile the pipeline parts of every program before the first
start (runtime/src/gfx/nvn/nvn_gpl.cpp gpl_prepare, format read_programs_file).

    python -I bnsh_programs.py --romfs <romfs> --out <shader_cache>/programs.bin [--cache <spirv dir>] [--self-test]

Method (read from the game's files, no format spec): inside a 'BNSH' region every stage of a program has a 24-byte record
{u64 control offset, u64 code offset, u32 code size, u32 control size} (offsets from the 'BNSH' magic), and
the records of one program follow each other 0x40 bytes apart. A run of records 0x40 apart is one program; its stages come from the
control section (u16 +0x56: 0 vertex, 1 fragment, 2 geometry; 6/7 are other stages, skipped). Block keys are the first 16 hex digits of
SHA-1 of the code area (the runtime's key). Only runs with a vertex and a fragment stage are written; with --cache, only programs
whose blocks all have a translated .spv there.
"""
import argparse, array, hashlib, os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import extract_generic as eg   # zstd frames and the romfs dictionaries

CTRL_MAGIC = b'\x34\x12\x76\x98'
RECORD_STRIDE = 0x40


def finds(d, pat):
    out, i = [], d.find(pat)
    while i >= 0:
        out.append(i)
        i = d.find(pat, i + 1)
    return out


def programs_in(d):
    """Every program of every BNSH region of d: [{stage: code key}]."""
    bnsh = [b for b in finds(d, b'BNSH') if b % 0x10 == 0]
    ctrls = [h for h in finds(d, CTRL_MAGIC) if h + 0x58 <= len(d) and struct.unpack_from('<I', d, h + 4)[0] == 0xc9]
    out = []
    for k, bs in enumerate(bnsh):
        be = bnsh[k + 1] if k + 1 < len(bnsh) else len(d)
        want = {h - bs: h for h in ctrls if bs <= h < be}
        if not want:
            continue
        region = memoryview(d)[bs:be]
        n8 = len(region) // 8
        arr = array.array('Q')
        arr.frombytes(region[:n8 * 8].tobytes())
        recs = []   # (record offset, stage, key)
        for i, v in enumerate(arr):
            if v not in want or i + 2 >= n8:
                continue
            h = want[v]
            code_off = arr[i + 1]
            code_size, ctrl_size = struct.unpack_from('<II', region, (i + 2) * 8)
            if code_size != struct.unpack_from('<I', d, h + 0x44)[0] or ctrl_size != struct.unpack_from('<I', d, h + 0x30)[0]:
                continue
            if not code_size or code_off + code_size > len(region):
                continue
            key = hashlib.sha1(region[code_off:code_off + code_size]).hexdigest()[:16]
            recs.append((i * 8, struct.unpack_from('<H', d, h + 0x56)[0], key))
        run = {}
        last = None
        for off, st, key in recs:
            if last is None or off - last != RECORD_STRIDE:
                if run:
                    out.append(run)
                run = {}
            run[st] = key
            last = off
        if run:
            out.append(run)
    return out


SHADER_FOLDERS = ('Shader', 'Layout2Shader', 'Effect')   # models, the UI (Layout2), effects (vsetb files hold BNSH too)


def archives(romfs):
    for top in SHADER_FOLDERS:
        for dp, _, fs in os.walk(os.path.join(romfs, top)):
            for f in sorted(fs):
                if '.bfsha' in f or '.vsetb' in f or f.endswith('.sarc.zs') or f.endswith('.bnsh'):
                    yield os.path.join(dp, f)


def load(path):
    b = open(path, 'rb').read()
    if b[:4] == b'\x28\xb5\x2f\xfd':
        r = eg.zstd_frame(memoryview(b), 0)
        b = r[0] if r else b''
    return b


def write_programs(path, progs):
    with open(path, 'wb') as f:
        f.write(b'MPRPRG01' + struct.pack('<II', len(progs), 0))
        for vs, fs, gs in progs:
            f.write(struct.pack('<QQQ', int(vs, 16), int(fs, 16), int(gs, 16) if gs else 0))


def self_test():
    # Two programs (vertex+fragment, then fragment alone) in one BNSH region, records 0x40 apart within a program.
    d = bytearray(0x2000)
    d[0:4] = b'BNSH'
    def block(ctrl, code, stage, rec, code_bytes):
        struct.pack_into('<II', d, ctrl, 0x98761234, 0xc9)
        struct.pack_into('<I', d, ctrl + 0x30, 0xd8)
        struct.pack_into('<I', d, ctrl + 0x44, len(code_bytes))
        struct.pack_into('<H', d, ctrl + 0x56, stage)
        d[code:code + len(code_bytes)] = code_bytes
        struct.pack_into('<QQII', d, rec, ctrl, code, len(code_bytes), 0xd8)
    block(0x400, 0x1000, 0, 0x100, b'\x01' * 16)
    block(0x500, 0x1100, 1, 0x140, b'\x02' * 16)
    block(0x600, 0x1200, 1, 0x300, b'\x03' * 16)
    p = programs_in(bytes(d))
    k = lambda b: hashlib.sha1(b * 16).hexdigest()[:16]
    assert p == [{0: k(b'\x01'), 1: k(b'\x02')}, {1: k(b'\x03')}], p
    print('self-test ok')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--romfs')
    ap.add_argument('--out')
    ap.add_argument('--cache', help='spirv folder: keep only programs whose blocks were translated')
    ap.add_argument('--self-test', action='store_true')
    a = ap.parse_args()
    if a.self_test:
        return self_test()
    if not a.romfs or not a.out:
        ap.error('--romfs and --out are required')
    eg.DICTS.update(eg.load_dicts([os.path.join(a.romfs, 'Pack')]))
    seen, progs, other = set(), [], 0
    for path in archives(a.romfs):
        n0 = len(progs)
        for run in programs_in(load(path)):
            if 0 not in run or 1 not in run:
                other += 1
                continue
            t = (run[0], run[1], run.get(2))
            if t not in seen:
                seen.add(t)
                progs.append(t)
        print(f'{os.path.relpath(path, a.romfs)}: {len(progs) - n0} new programs', flush=True)
    if a.cache:
        have = {f[:16] for f in os.listdir(a.cache) if f.endswith('.spv')}
        n = len(progs)
        progs = [p for p in progs if all(h in have for h in p if h)]
        print(f'{n - len(progs)} programs dropped: a block is not translated')
    write_programs(a.out, progs)
    print(f'{len(progs)} programs written to {a.out} ({other} runs without a vertex and a fragment stage skipped)')


if __name__ == '__main__':
    main()
