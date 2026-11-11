# SPDX-License-Identifier: GPL-3.0-or-later
"""The list of every function of a loaded module (a Switch NSO, or a static AArch64 ELF), with start and end offsets,
the evidence for each boundary, and the names the module's own symbols give. Adapted from smo-recomp
tools/extract/functions.py (GPL-3.0-or-later), rewritten to work on nxrecomp.image.Image.

    python -m nxrecomp.funcs <input.nso|input.elf> [--base 0x7100000000] [--out functions.tsv]

Boundary evidence, per function (column `src`):
    E  .eh_frame FDE (exact start and length, written by the compiler)
    S  symbol of type FUNC with a size (exact start and length, written by the linker)
    B  target of a BL somewhere in the module
    R  code address stored in data (R_AARCH64_RELATIVE addend into .text)
    T  target of an unconditional B that leaves the function it is in (tail call)
The list is the union of E and S, plus B/R/T targets that no E/S function starts at. The end of a function is the
E or S end when there is one (they must agree, checked), else the next start. All offsets are module-relative
(address minus the image base; main is loaded at 0x7100000000 on the console). A static ELF may have no FDEs at all.

write_tsv writes columns start, end, size, src, end_from, symbol (hex offsets, no 0x; header line included).
"""
import argparse
import bisect
import collections
import sys

from . import eh
from .image import Image


def sext(v, n):
    return v - (1 << n) if v & (1 << (n - 1)) else v


def analyse(img):
    """(rows, stats): rows are dicts start/end/src/end_from/names, offsets module-relative."""
    ts = img.text_start
    te = img.text_start + img.text_size
    words = img.words()
    exact = {}                                        # start -> {'E': end, 'S': end}
    for f in eh.all_fdes(img)[0]:
        # a zero-length FDE says nothing about the extent (the game 2.0.0 main has three, on called code): the start
        # then comes from its callers and the end from the next start
        if f.end > f.start:
            exact.setdefault(f.start, {})['E'] = f.end
    names = collections.defaultdict(list)
    for name, typ, bind, shndx, value, size in img.symbols():
        v = img.sym_value(value)
        if shndx and ts <= v < te:
            if typ == 2 and size:
                exact.setdefault(v, {})['S'] = v + size
            if name:
                names[v].append(name)
    bl, rel, tail = set(), set(), set()
    for i, w in enumerate(words):
        if (w & 0xfc000000) == 0x94000000:
            t = ts + 4 * i + sext(w & 0x3ffffff, 26) * 4
            if ts <= t < te:
                bl.add(t)
    for off, info, addend in img.relocs():
        if (info & 0xffffffff) == 0x403 and ts <= addend < te and addend % 4 == 0:
            rel.add(addend)

    # exact functions first; disagreements between E and S are reported, E wins (it is what the unwinder uses)
    starts = sorted(exact)
    disagree = [(s, v['E'], v['S']) for s, v in exact.items() if 'E' in v and 'S' in v and v['E'] != v['S']]
    ends = {s: v.get('E', v.get('S')) for s, v in exact.items()}

    def containing(a):
        i = bisect.bisect_right(starts, a) - 1
        if i >= 0 and a < ends[starts[i]]:
            return starts[i]
        return None
    # tail calls: unconditional B whose target is outside the exact function holding the branch
    for i, w in enumerate(words):
        if (w & 0xfc000000) == 0x14000000:
            pc = ts + 4 * i
            t = pc + sext(w & 0x3ffffff, 26) * 4
            f = containing(pc)
            if f is not None and ts <= t < te and not (f <= t < ends[f]):
                tail.add(t)
    # ADR targets: a static function whose address is taken with one ADR (a callback next to its user: the game's printf-style
    # formatter gets its output function so) is named by nothing else. LLVM also points ADR at the base label of a jump table, which
    # follows the dispatching BR inside the function: so a target counts only outside exact functions and right after RET/RETAA/RETAB/B.
    adr = set()
    for i, w in enumerate(words):
        if (w & 0x9f000000) == 0x10000000:
            imm = (((w >> 5) & 0x7ffff) << 2) | ((w >> 29) & 3)
            t = ts + 4 * i + (imm - (1 << 21) if imm & (1 << 20) else imm)
            if ts < t < te and t % 4 == 0 and words[(t - ts) // 4] and containing(t) is None:
                p = words[(t - ts) // 4 - 1]
                if (p & 0xfffffc1f) == 0xd65f0000 or p in (0xd65f0bff, 0xd65f0fff) or (p & 0xfc000000) == 0x14000000:
                    adr.add(t)
    # a heuristic start strictly inside an exact function is not a function of its own: in compiled code nothing
    # calls into the middle of a function; in hand-written assembly `bl next; .word x` reads a PC-relative word
    # (nn::os::detail::UserExceptionHandler in sdk). Such addresses are kept as secondary entries, not functions.
    secondary = sorted(a for a in (bl | rel | tail) - set(starts) if containing(a) is not None)
    # the NSO header: word 0 is either `b <entry>` (rtld) or 0, word 1 the MOD0 offset; when word 0 is not a branch the
    # bytes up to the end of the MOD0 record (7 words) are data, and a relocated pointer to the module base (R at 0)
    # is not a function. Only for an NSO (img.mod0 != 0).
    # Word 1 is the MOD0 offset and the record is data wherever it is: right after the two words, the header runs to its end; SDK 22
    # modules (the game) put it after .text, and "up to the end of MOD0" then covered all of .text and dropped every start that
    # only a BL, a relocation or a tail call names (and every gap function).
    nso = img.mod0 != 0
    mod0 = img.u32(4) if nso else 0
    hdr_end = (mod0 + 0x1c if mod0 <= 0x10 else 8) if nso else 0
    header = []
    if nso and (img.u32(0) & 0xfc000000) != 0x14000000:
        header = sorted(a for a in (bl | rel | tail) if (a < hdr_end or mod0 <= a < mod0 + 0x1c) and a not in exact)
    allstarts = (set(starts) | bl | rel | tail | adr) - set(secondary) - set(header)
    order = sorted(allstarts)
    rows = []
    for k, s in enumerate(order):
        src = ''.join(c for c, st in (('E', s in exact and 'E' in exact[s]), ('S', s in exact and 'S' in exact[s]),
                                      ('B', s in bl), ('R', s in rel), ('T', s in tail), ('A', s in adr)) if st)
        nxt = order[k + 1] if k + 1 < len(order) else te
        if nso and s == 0 and (img.u32(0) & 0xfc000000) == 0x14000000:
            end, how = 4, 'nso-entry'             # `b start` followed by the MOD0 header words (data)
        elif s in ends:
            end, how = ends[s], 'exact'
        else:
            end, how = nxt, 'next'
        rows.append({'start': s, 'end': end, 'src': src, 'end_from': how, 'names': names.get(s, [])})

    # code that no function covers (no FDE, no sized symbol, never the target of a BL): hand-written SDK stubs. Each run of
    # code between two functions is cut after every ret / br / unconditional b and becomes a function of its own (src G)
    # unless it is only padding (zero words and nops). The NSO header (up to the end of the MOD0 record) is data.
    gap_rows = []
    prev_end = ts
    for r in rows + [{'start': te, 'end': te}]:
        lo, hi = max(prev_end, hdr_end if nso else ts), r['start']
        prev_end = max(prev_end, r['end'])
        a = lo
        while a < hi:
            b = a
            while b < hi:
                w = words[(b - ts) // 4]
                b += 4
                if (w & 0xfffffc1f) in (0xd65f0000, 0xd61f0000) or w in (0xd65f0bff, 0xd65f0fff) or (w & 0xfc000000) == 0x14000000:
                    break
            seg = [words[(x - ts) // 4] for x in range(a, b, 4)]
            while seg and seg[0] in (0, 0xd503201f):
                seg.pop(0)
                a += 4
            if any(w not in (0, 0xd503201f) for w in seg):
                gap_rows.append({'start': a, 'end': b, 'src': 'G', 'end_from': 'gap', 'names': names.get(a, [])})
            a = b
    rows = sorted(rows + gap_rows, key=lambda r: r['start'])

    # coverage of .text by exact functions
    covered = 0
    last = 0
    for s in starts:
        e = ends[s]
        if e > last:
            covered += e - max(s, last)
            last = e
    stats = {'text_size': img.text_size, 'functions': len(rows), 'gap_functions': len(gap_rows), 'exact_E': sum(1 for v in exact.values() if 'E' in v),
             'exact_S': sum(1 for v in exact.values() if 'S' in v),
             'exact_both': sum(1 for v in exact.values() if 'E' in v and 'S' in v),
             'E_S_end_disagree': len(disagree), 'E_S_disagree_examples': ['%x: E %x S %x' % x for x in disagree[:10]],
             'only_heuristic': sum(1 for r in rows if r['end_from'] not in ('exact', 'nso-entry')),
             'secondary_entries': len(secondary),
             'header_refs_dropped': ['%x' % a for a in header],
             'secondary_examples': ['%x in %x' % (a, containing(a)) for a in secondary[:10]],
             'bl_targets': len(bl), 'reloc_targets': len(rel), 'tail_targets': len(tail),
             'text_bytes_in_exact_functions': covered, 'named_by_symbols': sum(1 for r in rows if r['names'])}
    return rows, stats


def write_tsv(rows, path):
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write('start\tend\tsize\tsrc\tend_from\tsymbol\n')
        for r in rows:
            f.write('%x\t%x\t%d\t%s\t%s\t%s\n' % (
                r['start'], r['end'], r['end'] - r['start'], r['src'], r['end_from'], '|'.join(r['names'])))


def main(argv=None):
    ap = argparse.ArgumentParser(description='Function discovery for an NSO or a static AArch64 ELF.')
    ap.add_argument('input', help='an NSO (magic NSO0) or a static ELF64 (magic 7fELF)')
    ap.add_argument('--base', type=lambda s: int(s, 0), default=0x7100000000,
                    help='load address of an NSO (ignored for ELF, which carries its own)')
    ap.add_argument('--out', help='write the function list as TSV to this file')
    a = ap.parse_args(argv)
    with open(a.input, 'rb') as f:
        data = f.read()
    if data[:4] == b'NSO0':
        img = Image.from_nso(data, a.base)
    elif data[:4] == b'\x7fELF':
        img = Image.from_static_elf(a.input)
    else:
        sys.exit('not an NSO or an ELF file: magic %r' % data[:4])
    rows, stats = analyse(img)
    print('module: %s  base 0x%x  text 0x%x size 0x%x' % (img.name, img.base, img.text_start, img.text_size))
    for k, v in stats.items():
        print('%s: %s' % (k, v))
    print('functions: %d' % len(rows))
    if a.out:
        write_tsv(rows, a.out)
        print('wrote %s' % a.out)


if __name__ == '__main__':
    main()
