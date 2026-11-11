# SPDX-License-Identifier: GPL-3.0-or-later
"""Compiles one test program (tests/lift/progs/*.c|cpp, or any S1 e2e program) to a static AArch64 ELF the way recomp/oracle/e2e does
(its build_prog.compile_cmd: freestanding, linked at 0x7100000000, entry test_main, `// FLAGS:` and `// EXPECT-INSN:` lines honoured),
with tests/lift/guest on the include path for rt2.h.

    python tools/guest_build.py <src> <out.elf> [-O0|-O1|-O2]       (from tests/, or any directory)
Exit code 0 = compiled, linked, every EXPECT-INSN regex matched."""
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..', '..'))                 # repository root
E2E = os.path.join(ROOT, 'recomp', 'oracle', 'e2e')
GUEST = os.path.abspath(os.path.join(HERE, '..', 'lift', 'guest'))
sys.path.insert(0, E2E)
import build_prog   # noqa: E402  (read-only import of S1's oracle helper)


def build(src, out, opt='-O2'):
    cmd = build_prog.compile_cmd(src, out, opt, extra=['-I', GUEST])
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        return False, 'compile/link failed:\n' + r.stdout + r.stderr
    _, expect = build_prog.program_flags(src)
    dis = subprocess.run([os.path.join(build_prog.LLVM, 'llvm-objdump.exe'), '-d', '--no-show-raw-insn', out], capture_output=True, text=True).stdout
    missing = [e for e in expect if not re.search(e, dis, re.M)]
    if missing:
        return False, 'EXPECT-INSN not found in the disassembly: ' + ', '.join(missing)
    return True, 'ok (%d EXPECT-INSN matched)' % len(expect)


if __name__ == '__main__':
    ok, msg = build(sys.argv[1], sys.argv[2], sys.argv[3] if len(sys.argv) > 3 else '-O2')
    print(('PASS ' if ok else 'FAIL ') + msg)
    sys.exit(0 if ok else 1)
