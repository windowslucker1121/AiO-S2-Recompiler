# SPDX-License-Identifier: GPL-3.0-or-later
"""bootcheck: before the long translation, find what keeps a dump from booting at all (not speed, not graphics).

    python -m nxrecomp.bootcheck <exefs folder> [--json]

Errors (the build stops with the reason):
  - a module the loader needs is missing: rtld (entered first), main, sdk, main.npdm (the title facts),
  - a module is not a decrypted, readable NSO,
  - rtld does not start with `b <entry>` (the runtime enters rtld at its first word).
Warnings (the build goes on; the setup log lists them):
  - a call whose return address holds a word that is not an instruction: when the callee returns, the lifted code runs that word
    and stops on an undefined-instruction trap. `bl .+8; .word d` (rtld, sdk) is handled by the lifter and not reported.
Exit codes: 0 boots as far as these checks see, 1 errors."""
import json
import os
import sys

from .image import Image, sext

NEEDED = ('rtld', 'main', 'sdk', 'main.npdm')
BASES = {'main': 0x7100000000, 'sdk': 0x7500000000, 'rtld': 0x7700000000}
TRAP_MARKERS = (0, 0xe7ffdefe)          # udf #0 and the SDK's UDF #0xdefe after noreturn calls: deliberate traps


def is_insn(w):
    return ((w >> 25) & 0xf) > 3 or w in TRAP_MARKERS


def calls_into_data(img):
    """Addresses of BLs whose return address is a data word (the `bl .+8` idiom excluded)."""
    ws = img.words()
    lo = img.base + img.text[0]
    out = []
    for i in range(len(ws) - 1):
        w = ws[i]
        if (w & 0xfc000000) == 0x94000000 and not is_insn(ws[i + 1]) and sext(w & 0x3ffffff, 26) != 2:
            out.append(lo + 4 * i)
    return out


def check(exefs, modules=None):
    """(errors, warnings) for the ExeFS folder. modules: [(name, file, base)], default rtld / main / sdk / subsdk*."""
    errors, warnings = [], []
    for n in NEEDED:
        if not os.path.isfile(os.path.join(exefs, n)):
            errors.append('exefs/%s is missing: the game cannot start without it. Use a complete dump.' % n)
    if modules is None:
        modules = [(n, n, BASES.get(n, 0x7600000000)) for n in sorted(os.listdir(exefs)) if n in BASES or n.startswith('subsdk')]
    for name, f, base in modules:
        p = os.path.join(exefs, f)
        if not os.path.isfile(p):
            continue
        data = open(p, 'rb').read()
        if data[:4] != b'NSO0':
            errors.append('exefs/%s is not a decrypted NSO. Extract the game with your own keys first.' % f)
            continue
        try:
            img = Image.from_nso(data, base, name)
        except Exception as e:
            errors.append('exefs/%s cannot be read (%s: %s).' % (f, type(e).__name__, e))
            continue
        if name == 'rtld' and (img.u32(img.text[0]) & 0xfc000000) != 0x14000000:
            errors.append('rtld does not start with a branch to its entry (word 0x%08x): the runtime cannot enter it.' % img.u32(img.text[0]))
        bad = calls_into_data(img)
        if bad:
            warnings.append('%s: %d call(s) return into a data word and would stop on an undefined-instruction trap if reached: %s'
                            % (name, len(bad), ' '.join('0x%x' % a for a in bad[:8]) + (' ...' if len(bad) > 8 else '')))
    return errors, warnings


def main(argv=None):
    import argparse
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('exefs')
    ap.add_argument('--json', action='store_true')
    a = ap.parse_args(argv)
    errors, warnings = check(a.exefs)
    if a.json:
        print(json.dumps({'errors': errors, 'warnings': warnings}))
    else:
        for e in errors:
            print('error: ' + e)
        for w in warnings:
            print('warning: ' + w)
        print('boot check: %d error(s), %d warning(s)' % (len(errors), len(warnings)))
    return 1 if errors else 0


if __name__ == '__main__':
    sys.exit(main())
