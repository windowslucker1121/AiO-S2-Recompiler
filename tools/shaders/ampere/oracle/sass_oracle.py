#!/usr/bin/env python3
"""Ground-truth SASS disassembly of an NVN2 (Ampere) shader corpus with NVIDIA's nvdisasm, plus an opcode census.

Input: a corpus made by tools/shaders/scripts/extract_generic.py (unique.tsv, code/<h>.bin, ctrl/<h>.bin).
For each distinct code block the program range comes from its control section (docs/AMPERE.md):
    stage = u16 ctrl+0x56 (0 vs, 1 ps, 2 gs, 3 tcs, 4 tes, 5 cs); graphics stages start with a 0x80-byte SPH v4.
    program end = u32 ctrl+0x38 (SPH + SASS, a multiple of 0x80); constant data follows at ctrl+0x40.
    start = 0x80 for graphics, 0 for compute; trailing all-zero 16-byte words are padding and are dropped.
The range is disassembled as one raw binary with `nvdisasm -b <SM> -hex` (base address 0 = first instruction after
the SPH). If nvdisasm refuses the block, each instruction is disassembled alone (with --base-address, so branch
targets stay right) and the ones it refuses are written as `.ILLEGAL <hex>` lines.

Output (--out): <h>.sass per block (instruction lines only, normalised whitespace), index.tsv (h, stage, bytes,
instructions, illegal, mode, sha256 of the .sass), census.tsv (base opcode: instruction count, blocks using it),
census_full.tsv (opcode with modifiers), summary.txt. Deterministic: sorted input, output independent of --jobs.

Usage: python -I sass_oracle.py --corpus <dir> --out <dir> [--sm SM86] [--nvdisasm <exe>] [--jobs 4]
       [--every N]  (only every Nth block in hash order: for SM comparisons)
       [--stage-every ps:20]  (only every Nth block of one stage: the reference subset, see docs/AMPERE.md)
This file is hash-locked (SHA256SUMS next to it).
"""
import argparse, collections, csv, hashlib, os, re, struct, subprocess, sys, tempfile
from concurrent.futures import ProcessPoolExecutor

NVDISASM = r'C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.3\bin\nvdisasm.exe'
LINE = re.compile(r'^\s*/\*([0-9a-f]{4,})\*/\s*(.*?)\s*;\s*/\*\s*(0x[0-9a-f]{16})\s*\*/\s*$')
CONT = re.compile(r'^\s*/\*\s*(0x[0-9a-f]{16})\s*\*/\s*$')
STAGES = {0: 'vs', 1: 'ps', 2: 'gs', 3: 'tcs', 4: 'tes', 5: 'cs'}


def low_priority():
    if os.name == 'nt':
        import ctypes
        ctypes.windll.kernel32.SetPriorityClass(ctypes.windll.kernel32.GetCurrentProcess(), 0x4000)


def program(code, ctrl):
    stage = struct.unpack_from('<H', ctrl, 0x56)[0]
    end = struct.unpack_from('<I', ctrl, 0x38)[0]
    start = 0 if stage == 5 else 0x80
    end = min(end, len(code))
    while end - 16 >= start and code[end - 16:end] == bytes(16):
        end -= 16
    return stage, code[start:end]


def run(exe, sm, data, base, tmpdir):
    p = os.path.join(tmpdir, 'b.bin')
    with open(p, 'wb') as f:
        f.write(data)
    cmd = [exe, '-b', sm, '-hex']
    if base:
        cmd += ['--base-address', hex(base)]
    r = subprocess.run(cmd + [p], capture_output=True, text=True)
    if r.returncode != 0 or 'error' in r.stderr:
        return None
    out, lines = [], r.stdout.splitlines()
    i = 0
    while i < len(lines):
        m = LINE.match(lines[i])
        if m:
            hi = ''
            if i + 1 < len(lines):
                c = CONT.match(lines[i + 1])
                if c:
                    hi = c.group(1); i += 1
            text = re.sub(r'\s+', ' ', m.group(2)).strip()
            out.append(f'/*{int(m.group(1), 16):05x}*/ {text} ; {m.group(3)} {hi}'.rstrip())
        i += 1
    return out


def disasm_block(job):
    h, code_path, ctrl_path, exe, sm, out = job
    code = open(code_path, 'rb').read()
    ctrl = open(ctrl_path, 'rb').read()
    stage, prog = program(code, ctrl)
    n = len(prog) // 16
    with tempfile.TemporaryDirectory() as td:
        lines = run(exe, sm, prog, 0, td) if prog else []
        mode, illegal = 'block', 0
        if lines is None or len(lines) != n:
            mode, lines = 'single', []
            for i in range(n):
                ins = prog[i * 16:(i + 1) * 16]
                one = run(exe, sm, ins, i * 16, td)
                if one and len(one) == 1:
                    lines.append(one[0])
                else:
                    illegal += 1
                    lines.append(f'/*{i * 16:05x}*/ .ILLEGAL ; {ins.hex()}')
    text = '\n'.join(lines) + '\n'
    with open(os.path.join(out, h + '.sass'), 'w', newline='\n') as f:
        f.write(text)
    ops = collections.Counter()
    full = collections.Counter()
    for ln in lines:
        body = ln.split('*/', 1)[1].split(';', 1)[0].strip()
        if body.startswith('@'):
            body = body.split(' ', 1)[1] if ' ' in body else body
        op = body.split(' ', 1)[0]
        full[op] += 1
        ops[op.split('.', 1)[0]] += 1
    return h, STAGES.get(stage, str(stage)), len(prog), n, illegal, mode, hashlib.sha256(text.encode()).hexdigest(), ops, full


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--corpus', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--sm', default='SM86')
    ap.add_argument('--nvdisasm', default=NVDISASM)
    ap.add_argument('--jobs', type=int, default=4)
    ap.add_argument('--every', type=int, default=1)
    ap.add_argument('--stage-every', action='append', default=[], metavar='STAGE:N',
                    help='keep only every Nth block of STAGE (hash order), e.g. ps:20')
    a = ap.parse_args()
    low_priority()
    os.makedirs(a.out, exist_ok=True)
    rows = list(csv.DictReader(open(os.path.join(a.corpus, 'unique.tsv'), newline=''), delimiter='\t'))
    rows.sort(key=lambda r: r['code_sha1_16'])
    rows = rows[::a.every]
    for spec in a.stage_every:
        st, n = spec.split(':')
        keep, i = [], 0
        for r in rows:
            if r['stage'] == st:
                if i % int(n) == 0:
                    keep.append(r)
                i += 1
            else:
                keep.append(r)
        rows = keep
    jobs = [(r['code_sha1_16'], os.path.join(a.corpus, 'code', r['code_sha1_16'] + '.bin'),
             os.path.join(a.corpus, 'ctrl', r['first_ctrl'] + '.bin'), a.nvdisasm, a.sm, a.out) for r in rows]
    ops_n, ops_b, full_n, full_b = (collections.Counter() for _ in range(4))
    idx = []
    with ProcessPoolExecutor(a.jobs, initializer=low_priority) as ex:
        for h, st, nb, n, ill, mode, sha, ops, full in ex.map(disasm_block, jobs, chunksize=16):
            idx.append(f'{h}\t{st}\t{nb}\t{n}\t{ill}\t{mode}\t{sha}')
            ops_n.update(ops); ops_b.update(ops.keys()); full_n.update(full); full_b.update(full.keys())
    with open(os.path.join(a.out, 'index.tsv'), 'w', newline='\n') as f:
        f.write('code_sha1_16\tstage\tprogram_bytes\tinstructions\tillegal\tmode\tsass_sha256\n' + '\n'.join(idx) + '\n')
    for name, n, b in (('census.tsv', ops_n, ops_b), ('census_full.tsv', full_n, full_b)):
        with open(os.path.join(a.out, name), 'w', newline='\n') as f:
            f.write('opcode\tinstructions\tblocks\n')
            for op, c in sorted(n.items(), key=lambda kv: (-kv[1], kv[0])):
                f.write(f'{op}\t{c}\t{b[op]}\n')
    tot = sum(ops_n.values())
    ill = sum(int(l.split('\t')[4]) for l in idx)
    single = sum(l.split('\t')[5] == 'single' for l in idx)
    s = (f'sm {a.sm}  blocks {len(idx)}  instructions {tot}  illegal {ill}  blocks needing per-instruction mode {single}\n'
         f'distinct base opcodes {len(ops_n)}  distinct opcode+modifier forms {len(full_n)}\n')
    open(os.path.join(a.out, 'summary.txt'), 'w', newline='\n').write(s)
    print(s, end='')


if __name__ == '__main__':
    main()
