# SPDX-License-Identifier: GPL-3.0-or-later
"""program.localize_callee_saved: x0..x8, x19..x29 and SP as locals, written back before calls and exits (only what changed since the
context was last in step), loaded at entry and after a reload; x0..x8 also after every call. Synthetic lifted text.
Run: python -m nxrecomp.test_callee_locals"""
from nxrecomp.program import localize_callee_saved

FN = '\n'.join([
    'void f_7100001000(Ctx *__restrict c) {',
    '  c->fn = 0x7100001000ULL;',
    '  NxFlags nf; nxf_load(nf, c);',
    '  { c->x[31] += (uint64_t)-32LL; uint64_t a = c->x[31] + (uint64_t)0LL; wr64(a, c->x[29]); wr64(a + 8, c->x[30]);  }',
    '  c->x[19] = (uint64_t)(0ULL | c->x[0]);',
    '  c->x[30] = 0x7100001010ULL; f_7100002000(c);',
    '  c->x[0] = (uint64_t)(c->x[19] + 8ULL);',
    '  if (c->x[0] == 0) goto L_7100001020;',
    '  c->x[20] = (uint64_t)(c->x[19]);',
    'L_7100001020: c->x[30] = 0x7100001024ULL; f_7100002000(c);',
    '  nxf_store(nf, c); guest_svc(c, 0x1); nxf_load(nf, c);',
    '  { c->x[31] += (uint64_t)32LL; }',
    '  return;',
    '}'])

L = ' x0_ = c->x[0]; x19_ = c->x[19]; x20_ = c->x[20]; x29_ = c->x[29]; sp_ = c->x[31];'


def main():
    import os
    # SP alone (NX_CALLEE_LOCALS_REGS)
    os.environ['NX_CALLEE_LOCALS_REGS'] = '31'
    d = localize_callee_saved(FN).split('\n')
    assert 'uint64_t sp_; NxFlags nf; nxf_load(nf, c); sp_ = c->x[31];' in d[2], d[2]
    assert d[4] == '  c->x[19] = (uint64_t)(0ULL | c->x[0]);', d[4]
    assert d[5] == '  c->x[30] = 0x7100001010ULL; c->x[31] = sp_; f_7100002000(c);', d[5]
    assert d[9] == 'L_7100001020: c->x[30] = 0x7100001024ULL; f_7100002000(c);', d[9]
    # the default set: every register the pass can handle
    os.environ.pop('NX_CALLEE_LOCALS_REGS', None)
    o = localize_callee_saved(FN)
    lines = o.split('\n')
    # declaration and entry loads
    assert 'uint64_t x0_, x19_, x20_, x29_, sp_; NxFlags nf; nxf_load(nf, c);' + L in lines[2], lines[2]
    # every original reference is a local
    assert 'sp_ += (uint64_t)-32LL' in lines[3] and 'wr64(a, x29_)' in lines[3], lines[3]
    assert lines[4] == '  x19_ = (uint64_t)(0ULL | x0_);', lines[4]
    # first call: SP and x19 changed since entry; x0 is loaded again after it
    assert lines[5] == '  c->x[30] = 0x7100001010ULL; c->x[19] = x19_; c->x[31] = sp_; f_7100002000(c); x0_ = c->x[0];', lines[5]
    # second call: x0 (the argument) and, on one path, x20 changed; the callee kept the rest
    assert lines[9] == 'L_7100001020: c->x[30] = 0x7100001024ULL; c->x[0] = x0_; c->x[20] = x20_; f_7100002000(c); x0_ = c->x[0];', lines[9]
    # SVC: nothing changed since the call; every local is loaded again after it
    assert lines[10] == '  nxf_store(nf, c); guest_svc(c, 0x1); nxf_load(nf, c);' + L, lines[10]
    # return: SP changed after the reload
    assert lines[12] == '  { c->x[31] = sp_; return; }', lines[12]
    # a tail call: everything changed is written before the branch, nothing after it (x0 is the callee's result there)
    t = localize_callee_saved(FN.replace('  return;', '  c->x[0] = (uint64_t)(1ULL); nxf_store(nf, c); f_7100003000(c); return;'))
    assert t.split('\n')[12] == '  x0_ = (uint64_t)(1ULL); nxf_store(nf, c); c->x[0] = x0_; c->x[31] = sp_; f_7100003000(c); return;', t.split('\n')[12]
    # a function in which the flags are not local is left alone
    plain = FN.replace('NxFlags nf; nxf_load(nf, c);', '')
    assert localize_callee_saved(plain) == plain
    print('ok')


if __name__ == '__main__':
    main()
