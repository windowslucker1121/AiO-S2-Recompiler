#!/usr/bin/env python3
"""Ampere (NVN2) shader census of a game's shader corpus, with OUR decoder (tools/shaders/ampere/decoder, `sassdecode --census`).

Usage: python -I tools/shaders/scripts/census.py <corpus dir> --game <name> [--out-dir <dir>] [--jobs N] [--no-build] [--keep]

<corpus dir> is either layout the project uses:
  * title A corpus (tools/shaders/scripts/extract_generic.py): unique.tsv, code/<h>.bin, ctrl/<h>.bin
  * runtime dump (NXRT_NVN_DUMP_MISSING, any game): manifest.tsv, code_<h>.bin, ctrl_<h>.bin (duplicate rows folded)

Writes <out-dir>/census_<game>.tsv (default out-dir: <repo root>/work/audit) and census_<game>_summary.md.
TSV columns: opcode, modifiers, count (instructions over the distinct code blocks), blocks (distinct blocks containing it),
stage_blocks (blocks per stage), decode (ok | partial | undecoded), flag (tensor | raytracing | mesh), nvdisasm (name of an
undecoded opcode, from running NVIDIA's nvdisasm on one example word), example_blocks (smallest hashes), example_addr/lo/hi.
"""
import argparse, glob, os, re, shutil, struct, subprocess, sys, tempfile
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
SHADERS = os.path.dirname(HERE)
SASSDECODE_PROJ = os.path.join(SHADERS, 'ampere', 'decoder', 'tool', 'sassdecode.csproj')
SASSDECODE_DLL = os.path.join(SHADERS, 'build', 'bin', 'Release', 'net10.0', 'sassdecode.dll')

TENSOR = {'HMMA', 'IMMA', 'BMMA', 'DMMA', 'LDSM', 'MOVM', 'HGMMA', 'QGMMA', 'IGMMA', 'BGMMA'}
MESH = {'ISBERD', 'ISBEWR'}
STAGE_NAMES = ['vs', 'ps', 'gs', 'tcs', 'tes', 'cs', 'stage6', 'stage7+']


def repo_root():
    d = HERE
    while True:
        if os.path.isdir(os.path.join(d, 'work')) and os.path.isdir(os.path.join(d, '.weaver')):
            return d
        p = os.path.dirname(d)
        if p == d:
            return None
        d = p


def find_nvdisasm():
    if os.environ.get('NVDISASM') and os.path.exists(os.environ['NVDISASM']):
        return os.environ['NVDISASM']
    w = shutil.which('nvdisasm')
    if w:
        return w
    c = sorted(glob.glob(r'C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v*\bin\nvdisasm.exe'))
    return c[-1] if c else None


def nvdisasm_name(nv, lo, hi):
    """Mnemonic (with its dotted modifiers) nvdisasm prints for one 128-bit word, or '<illegal>'."""
    with tempfile.TemporaryDirectory() as td:
        p = os.path.join(td, 'w.bin')
        with open(p, 'wb') as f:
            f.write(struct.pack('<QQ', lo, hi))
        r = subprocess.run([nv, '-b', 'SM86', '-hex', p], capture_output=True, text=True)
    for ln in r.stdout.splitlines():
        m = re.match(r'^\s*/\*[0-9a-f]{4,}\*/\s*(.*?)\s*;\s*/\*', ln)
        if m:
            t = re.sub(r'\s+', ' ', m.group(1)).strip()
            if t.startswith('@'):
                t = t.split(' ', 1)[1] if ' ' in t else t
            return t.split(' ')[0]
    return '<illegal>'


def classify(opcode, nv_name):
    n = (nv_name or opcode).split('.')[0]
    if n in TENSOR:
        return 'tensor'
    if n.startswith('TTU') or n.startswith('TRAX') or n.startswith('RTT'):
        return 'raytracing'
    if n in MESH:
        return 'mesh'
    return ''


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('corpus')
    ap.add_argument('--game', required=True, help='short name used in the output file names (e.g. oot)')
    ap.add_argument('--out-dir', default=None)
    ap.add_argument('--jobs', type=int, default=6)
    ap.add_argument('--no-build', action='store_true', help='skip `dotnet build` of sassdecode')
    ap.add_argument('--examples', type=int, default=5, help='example blocks listed per row (smallest hashes)')
    ap.add_argument('--all-blocks', action='store_true', help='list every block per row (large for big corpora)')
    ap.add_argument('--keep', action='store_true', help='keep the raw sassdecode output next to the results (<game>_raw.tsv, <game>_blocks.tsv)')
    a = ap.parse_args()

    corpus = os.path.abspath(a.corpus)
    root = repo_root()
    out_dir = os.path.abspath(a.out_dir or os.path.join(root or '.', 'work', 'audit'))
    os.makedirs(out_dir, exist_ok=True)

    if not a.no_build:
        r = subprocess.run(['dotnet', 'build', SASSDECODE_PROJ, '-c', 'Release', '--nologo', '-v', 'q'], capture_output=True, text=True)
        if r.returncode != 0:
            sys.exit('dotnet build failed:\n' + r.stdout[-2000:] + r.stderr[-2000:])
    if not os.path.exists(SASSDECODE_DLL):
        sys.exit('missing ' + SASSDECODE_DLL)

    raw = os.path.join(out_dir, f'{a.game}_raw.tsv')
    blk = os.path.join(out_dir, f'{a.game}_blocks.tsv')
    cmd = ['dotnet', SASSDECODE_DLL, '--census', corpus, '--out', raw, '--blocks', blk, '--jobs', str(a.jobs), '--examples', str(a.examples)]
    if a.all_blocks:
        cmd.append('--all-blocks')
    r = subprocess.run(cmd, capture_output=True, text=True)
    sys.stdout.write(r.stdout)
    if r.returncode != 0:
        sys.exit('sassdecode failed:\n' + r.stderr[-2000:])

    with open(raw, encoding='utf-8') as f:
        hdr = f.readline().rstrip('\n').split('\t')
        rows = [dict(zip(hdr, ln.rstrip('\n').split('\t'))) for ln in f]
    with open(blk, encoding='utf-8') as f:
        f.readline()
        blocks = [ln.rstrip('\n').split('\t') for ln in f]

    nv = find_nvdisasm()
    for rw in rows:
        rw['count'] = int(rw['count']); rw['blocks'] = int(rw['blocks'])
        rw['nvdisasm'] = ''
        if rw['decode'] == 'undecoded':
            if nv and rw['example_lo']:
                rw['nvdisasm'] = nvdisasm_name(nv, int(rw['example_lo'], 16), int(rw['example_hi'], 16))
            else:
                rw['nvdisasm'] = '?' if not nv else ''
        rw['flag'] = classify(rw['opcode'], rw['nvdisasm'])

    cols = ['opcode', 'modifiers', 'count', 'blocks', 'stage_blocks', 'decode', 'flag', 'nvdisasm', 'example_blocks', 'example_addr', 'example_lo', 'example_hi']
    tsv = os.path.join(out_dir, f'census_{a.game}.tsv')
    with open(tsv, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\t'.join(cols) + '\n')
        for rw in rows:
            f.write('\t'.join(str(rw[c]) for c in cols) + '\n')

    # ---- summary ----
    nblk = len(blocks)
    ninst = sum(int(b[2]) for b in blocks)
    stage_cnt = defaultdict(int)
    for b in blocks:
        stage_cnt[min(int(b[1]), 7)] += 1
    undec = [r_ for r_ in rows if r_['decode'] == 'undecoded']
    part = [r_ for r_ in rows if r_['decode'] == 'partial']
    n_undec = sum(r_['count'] for r_ in undec)
    n_part = sum(r_['count'] for r_ in part)
    blk_undec = sum(1 for b in blocks if int(b[3]) > 0)
    hi_stage = [b for b in blocks if int(b[1]) >= 6]

    def by_opcode(pred):
        d = defaultdict(lambda: [0, 0, set(), defaultdict(int)])
        for r_ in rows:
            n = (r_['nvdisasm'] or r_['opcode']).split('.')[0]
            if pred(n, r_):
                e = d[n + (' (undecoded in our decoder, ' + r_['opcode'] + ')' if r_['decode'] == 'undecoded' else '')]
                e[0] += r_['count']
                e[1] = max(e[1], r_['blocks'])           # lower bound over modifiers; exact count is per row
                e[2].update(r_['example_blocks'].split(',')[:3])
                for s in r_['stage_blocks'].split(';'):
                    if s:
                        k, v = s.split('=')
                        e[3][k] = max(e[3][k], int(v))
        return d

    def flagged(title, pred, none):
        out = [f'### {title}', '']
        d = by_opcode(pred)
        if not d:
            out += [none, '']
            return out
        out += ['| opcode | instructions | blocks (max over modifier rows) | stages (blocks, max over rows) | example blocks |', '|---|---|---|---|---|']
        for k, e in sorted(d.items()):
            out.append(f'| {k} | {e[0]} | {e[1]} | {", ".join(f"{s}={v}" for s, v in e[3].items())} | {", ".join(sorted(e[2])[:3])} |')
        out.append('')
        return out

    md = [f'# Shader census: {a.game}', '',
          f'Corpus `{corpus}`; decoder: tools/shaders/ampere/decoder (`sassdecode --census`). Counts are over the distinct code blocks, not draw occurrences.', '',
          f'- Blocks: {nblk}; instructions: {ninst}; opcode+modifier rows: {len(rows)} (`census_{a.game}.tsv`)',
          '- Blocks per stage: ' + ', '.join(f'{STAGE_NAMES[k]} {stage_cnt[k]}' for k in sorted(stage_cnt)),
          f'- Undecodable opcodes: {len(undec)} distinct, {n_undec} instructions in {blk_undec} blocks ({100.0 * n_undec / max(1, ninst):.5f}% of instructions)',
          f'- Partially decoded (known opcode, unknown modifier or form): {len(part)} rows, {n_part} instructions', '']
    md += ['## Flags', '']
    md += flagged('Tensor (HMMA / IMMA / LDSM / MOVM and kin)', lambda n, r_: n in TENSOR, 'None found.')
    md += flagged('Ray tracing (TTU*)', lambda n, r_: n.startswith('TTU') or n.startswith('TRAX') or n.startswith('RTT'), 'None found.')
    md += flagged('Mesh / task (ISBERD, ISBEWR)', lambda n, r_: n in MESH,
                  'None found.')
    md.append('ISBERD / ISBEWR read and write the inter-stage buffer: tessellation and geometry stages use them too, so the stage column decides. '
              'Blocks whose control stage value (u16 at +0x56) is above 5 are listed separately: ' +
              (f'{len(hi_stage)} blocks (stage 6: {stage_cnt[6]}, stage 7+: {stage_cnt[7]}); the stage values are inferred to be task/mesh, not confirmed. '
               f'Examples: {", ".join(sorted(b[0] for b in hi_stage)[:5])}.' if hi_stage else 'none.'))
    md.append('')
    md += ['## Undecodable opcodes', '']
    if undec:
        md += ['| opcode (12-bit) | nvdisasm name | instructions | blocks | example block | addr | words (lo hi) |', '|---|---|---|---|---|---|---|']
        for r_ in sorted(undec, key=lambda r_: -r_['count']):
            md.append(f"| {r_['opcode']} | {r_['nvdisasm'] or '-'} | {r_['count']} | {r_['blocks']} | {r_['example_blocks'].split(',')[0]} | {r_['example_addr']} | {r_['example_lo']} {r_['example_hi']} |")
        md.append('')
        md.append('The example block is the smallest hash holding the opcode; addr and words are its first occurrence there (addr = byte offset from the first instruction, after the 0x80-byte SPH of graphics stages). The nvdisasm name comes from NVIDIA nvdisasm (SM86) run on that one word.')
        md.append('')
    else:
        md += ['None: every instruction of every block decodes.', '']
    if part:
        md += ['## Partially decoded rows', '', '| opcode | modifiers | instructions | blocks | example block | addr |', '|---|---|---|---|---|---|']
        for r_ in sorted(part, key=lambda r_: -r_['count'])[:40]:
            md.append(f"| {r_['opcode']} | {r_['modifiers']} | {r_['count']} | {r_['blocks']} | {r_['example_blocks'].split(',')[0]} | {r_['example_addr']} |")
        md.append('')
    tot = defaultdict(int)
    for r_ in rows:
        tot[r_['opcode']] += r_['count']
    md += ['## Most frequent opcodes', '', '| opcode | instructions |', '|---|---|']
    for k, v in sorted(tot.items(), key=lambda kv: -kv[1])[:15]:
        md.append(f'| {k} | {v} |')
    md.append('')
    with open(os.path.join(out_dir, f'census_{a.game}_summary.md'), 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(md))
    if not a.keep:
        os.remove(raw); os.remove(blk)
    print(f'wrote {tsv} and census_{a.game}_summary.md; undecodable opcodes {len(undec)}, tensor/rt/mesh flagged rows '
          f'{sum(1 for r_ in rows if r_["flag"])}')


if __name__ == '__main__':
    main()
