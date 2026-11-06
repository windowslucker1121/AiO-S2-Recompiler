# SPDX-License-Identifier: GPL-3.0-or-later
"""Compile one end-to-end test program to a static AArch64 ELF and check its EXPECT-INSN lines.

    python build_prog.py <src.c|src.cpp> <out.elf> [-O0|-O1|-O2|-O3]

Exit code 0 when it compiled, linked and every `// EXPECT-INSN: <regex>` matched a line of the disassembly.
The ELF is linked at 0x7100000000 with entry point `test_main`.
"""
import glob
import os
import re
import shlex
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def _llvm_bin():
    """LLVM_BIN, else an LLVM release unpacked under tools/llvm/ in this repository (as recomp/cmake/clang-cl.cmake), else the
    installed one."""
    if os.environ.get('LLVM_BIN'):
        return os.environ['LLVM_BIN']
    local = sorted(glob.glob(os.path.join(HERE, '..', '..', '..', 'tools', 'llvm', '*', 'bin', 'clang.exe')))
    return os.path.dirname(os.path.abspath(local[0])) if local else r'C:\Program Files\LLVM\bin'


LLVM = _llvm_bin()
BASE = 0x7100000000


def program_flags(src):
    flags, expect = [], []
    for line in open(src, encoding='utf-8'):
        m = re.match(r'\s*//\s*FLAGS:\s*(.*)', line)
        if m:
            flags += shlex.split(m.group(1))
        m = re.match(r'\s*//\s*EXPECT-INSN:\s*(.*\S)', line)
        if m:
            expect.append(m.group(1))
    return flags, expect


def compile_cmd(src, out, opt='-O2', extra=()):
    flags, _ = program_flags(src)
    cpp = src.endswith('.cpp')
    cmd = [os.path.join(LLVM, 'clang++.exe' if cpp else 'clang.exe'), '--target=aarch64-linux-gnu', opt,
           '-ffreestanding', '-nostdlib', '-fno-pic', '-fno-pie', '-fno-stack-protector',
           '-fno-asynchronous-unwind-tables', '-fno-unwind-tables', '-mno-outline-atomics', '-Wno-everything',
           '-fuse-ld=lld', '-static', '-Wl,--image-base=0x%x' % BASE, '-Wl,-e,test_main', '-Wl,--build-id=none',
           '-Wl,-z,norelro', '-I', os.path.join(HERE, 'progs')]
    if cpp:
        cmd += ['-std=c++17', '-fno-exceptions', '-fno-rtti', '-fno-threadsafe-statics']
    else:
        cmd += ['-std=gnu17']
    cmd += flags + list(extra) + [src, '-o', out]
    return cmd


def build(src, out, opt='-O2'):
    r = subprocess.run(compile_cmd(src, out, opt), capture_output=True, text=True)
    if r.returncode:
        return False, 'compile/link failed:\n' + r.stdout + r.stderr
    _, expect = program_flags(src)
    dis = subprocess.run([os.path.join(LLVM, 'llvm-objdump.exe'), '-d', '--no-show-raw-insn', out], capture_output=True, text=True).stdout
    missing = [e for e in expect if not re.search(e, dis, re.M)]
    if missing:
        return False, 'EXPECT-INSN not found in the disassembly: ' + ', '.join(missing)
    return True, 'ok (%d EXPECT-INSN matched)' % len(expect)


if __name__ == '__main__':
    opt = sys.argv[3] if len(sys.argv) > 3 else '-O2'
    ok, msg = build(sys.argv[1], sys.argv[2], opt)
    print(('PASS ' if ok else 'FAIL ') + msg)
    sys.exit(0 if ok else 1)
