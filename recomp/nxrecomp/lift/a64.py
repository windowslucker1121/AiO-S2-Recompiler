# SPDX-License-Identifier: GPL-3.0-or-later
"""Tier 1 of the instruction lifter: AArch64 word -> one C/C++ statement, by bit-field decoding.

This is class Fn of smo-recomp tools/lifter/recomp.py (GPL-3.0-or-later; itself adapted from ssbu-recomp's lifter
and from the instruction translator of ssbu-recomp, GPL-3.0-or-later), with these changes:
  * the module-level ELF globals are gone (the program level lives in program.py and works on an Image);
  * TPIDRRO_EL0 is its own register (c->tpidrro), as in ScarletNX (GPL-3.0-or-later); FPCR / FPSR / NZCV are stored
    and read back; other system registers go to guest_mrs / guest_msr (runtime-provided, see nxrecomp/ctx.h);
  * exclusive pairs (LDXP / LDAXP / STXP / STLXP) incl. 128-bit;
  * an instruction tier 1 does not know raises Unimpl and is handed to tier 2 (lift/tier2.py, Capstone-driven,
    ported from SuperMarioWonderRecomp, MIT) by lift.Lifter.

Every guest function becomes `void f_<addr>(Ctx *c)` working on the register context of nxrecomp/ctx.h. Guest memory
is mapped at its real address, so guest pointers are host pointers.
"""
import struct

def bits(w, hi, lo):
    return (w >> lo) & ((1 << (hi - lo + 1)) - 1)


def sext(v, n):
    return v - (1 << n) if v & (1 << (n - 1)) else v


class Unimpl(Exception):
    pass


# ---------------------------------------------------------------- operands
def X(n):
    return '0ULL' if n == 31 else 'c->x[%d]' % n


def XS(n):
    return 'c->x[%d]' % n            # register 31 is SP here


def W(n):
    return '0U' if n == 31 else '(uint32_t)c->x[%d]' % n


def WS(n):
    return '(uint32_t)c->x[%d]' % n


def R(n, sf, sp=False):
    if sf:
        return XS(n) if sp else X(n)
    return WS(n) if sp else W(n)


def SET(n, sf, expr, sp=False):
    if n == 31 and not sp:
        return '(void)(%s);' % expr
    return 'c->x[%d] = (%s)(%s);' % (n, 'uint64_t' if sf else 'uint32_t', expr)


def T(sf):
    return 'uint64_t' if sf else 'uint32_t'


# A scalar write of `t` to the low lane of v<rd>, the rest of the register cleared: built as one V128 and stored once (ctx.h
# vmk*), not zeroed and then patched (two stores that a later 16-byte read cannot forward from).
_VMK = {'f[0]': 'vmkf', 'df[0]': 'vmkd', 'w[0]': 'vmk32', 'd[0]': 'vmk64', 'h[0]': 'vmk32'}


def vput(rd, field):
    return 'c->v[%d] = %s(t);' % (rd, _VMK[field])


def decode_bitmasks(n, imms, immr, datasize):
    v = (n << 6) | (~imms & 0x3f)
    length = v.bit_length() - 1
    if length < 1:
        raise Unimpl('bitmask')
    levels = (1 << length) - 1
    s, r = imms & levels, immr & levels
    esize = 1 << length
    d = (s - r) & levels
    welem = (1 << (s + 1)) - 1
    telem = (1 << (d + 1)) - 1
    welem = ((welem >> r) | (welem << (esize - r))) & ((1 << esize) - 1)
    wmask = tmask = 0
    for i in range(0, datasize, esize):
        wmask |= welem << i
        tmask |= telem << i
    return wmask & ((1 << datasize) - 1), tmask & ((1 << datasize) - 1)


SHIFT = ['lsl', 'lsr', 'asr', 'ror']


def shifted(expr, kind, amount, sf):
    if amount == 0:
        return expr
    if kind == 0:
        return '(%s << %d)' % (expr, amount)
    if kind == 1:
        return '(%s >> %d)' % (expr, amount)
    if kind == 2:
        return '(%s)((%s)%s >> %d)' % (T(sf), 'int64_t' if sf else 'int32_t', expr, amount)
    return 'ror%d(%s, %d)' % (64 if sf else 32, expr, amount)


def extended(n, option, shift):
    src = X(n)
    cast = ['(uint64_t)(uint8_t)', '(uint64_t)(uint16_t)', '(uint64_t)(uint32_t)', '(uint64_t)',
            '(uint64_t)(int64_t)(int8_t)', '(uint64_t)(int64_t)(int16_t)', '(uint64_t)(int64_t)(int32_t)',
            '(uint64_t)'][option]
    e = '(%s%s)' % (cast, src)
    return '(%s << %d)' % (e, shift) if shift else e


# ---------------------------------------------------------------- lifter
class Fn:
    """Lifting context for one guest function (address range [entry, end))."""

    def __init__(self, entry, end, env):
        self.entry, self.end, self.env = entry, end, env
        self.has_br_fallback = False     # an unresolved `br`: the function ends in a dispatch over all its instructions
        self.targets = set()             # addresses inside the function that a goto needs a label for

    def jump(self, target):
        if self.entry <= target < self.end:
            self.targets.add(target)
            return 'goto L_%x;' % target
        return '{ %s return; }' % self.env.call(target)

    def is_data_word(self, addr):
        """True when the word at addr is not an instruction (op0 0..3, see lift), so no execution falls into it. The SDK's
        deliberate UDF #0xdefe (Halt / DebugBreak after a noreturn call) stays a trap."""
        word = getattr(self.env, 'word', None)
        if word is None:
            return False
        try:
            w = word(addr)
        except Exception:                                # outside the image (cover lifts every word at one fake pc)
            return False
        return w is not None and bits(w, 28, 25) <= 3 and w not in (0, 0xe7ffdefe)

    def lift(self, w, pc):
        op0 = bits(w, 28, 25)
        if op0 in (8, 9):
            return self.dp_imm(w, pc)
        if op0 in (10, 11):
            return self.branch(w, pc)
        if op0 in (4, 6, 12, 14):
            return self.loadstore(w, pc)
        if op0 in (5, 13):
            return self.dp_reg(w, pc)
        if op0 in (7, 15):
            return self.fpsimd(w, pc)
        # op0 0..3: reserved / unallocated / SVE. None of these is an instruction on the console's Cortex-A57
        # (ARMv8.0, no SVE): executing one raises an undefined-instruction exception, so the faithful translation is a
        # trap. the platform SDK uses this deliberately (UDF #0xdefe, the word 0xe7ffdefe, in Halt / DebugBreak paths).
        # [from smo-recomp; checked against Unicorn by the difftest's trap tests]
        return 'guest_trap("undefined instruction", 0x%xULL);' % pc

    # ---- data processing, immediate
    def dp_imm(self, w, pc):
        sf, rd, rn = bits(w, 31, 31), bits(w, 4, 0), bits(w, 9, 5)
        g = bits(w, 25, 23)
        if g in (0, 1):
            imm = sext((bits(w, 23, 5) << 2) | bits(w, 30, 29), 21)
            val = ((pc & ~0xfff) + (imm << 12)) if sf else pc + imm
            return SET(rd, 1, '0x%xULL' % (val & (2 ** 64 - 1)))
        if g == 2:
            imm = bits(w, 21, 10) << (12 if bits(w, 22, 22) else 0)
            sub, s = bits(w, 30, 30), bits(w, 29, 29)
            a = R(rn, sf, sp=True)
            if s:
                return SET(rd, sf, '%s%d(c, %s, %s, %d)' % ('adds', 64 if sf else 32, a,
                                                              ('~(%s)%dULL' % (T(sf), imm)) if sub else '%dULL' % imm, sub))
            return SET(rd, sf, '%s %s %dULL' % (a, '-' if sub else '+', imm), sp=True)
        if g == 4:
            opc = bits(w, 30, 29)
            wmask, _ = decode_bitmasks(bits(w, 22, 22), bits(w, 15, 10), bits(w, 21, 16), 64 if sf else 32)
            a = R(rn, sf)
            e = '%s %s 0x%xULL' % (a, '&|^&'[opc], wmask)
            if opc == 3:
                return SET(rd, sf, 'logic%d(c, %s)' % (64 if sf else 32, e))
            return SET(rd, sf, e, sp=True)
        if g == 5:
            opc, hw, imm = bits(w, 30, 29), bits(w, 22, 21) * 16, bits(w, 20, 5)
            if opc == 0:
                return SET(rd, sf, '~(0x%xULL)' % (imm << hw))
            if opc == 2:
                return SET(rd, sf, '0x%xULL' % (imm << hw))
            if opc == 3:
                return SET(rd, sf, '(%s & ~0x%xULL) | 0x%xULL' % (R(rd, sf), 0xffff << hw, imm << hw))
            raise Unimpl('movwide')
        if g == 6:
            opc, n, immr, imms = bits(w, 30, 29), bits(w, 22, 22), bits(w, 21, 16), bits(w, 15, 10)
            size = 64 if sf else 32
            wmask, tmask = decode_bitmasks(n, imms, immr, size)
            if opc == 3:
                raise Unimpl('bitfield3')
            dst = R(rd, sf) if opc == 1 else '0'
            top = dst if opc == 1 else ('((s >> %d) & 1 ? ~(%s)0 : 0)' % (imms, T(sf)) if opc == 0 else '0')
            return ('{ %s s = %s; %s bot = (%s & ~0x%xULL) | (ror%d(s, %d) & 0x%xULL); %s top = %s; %s }'
                    % (T(sf), R(rn, sf), T(sf), dst, wmask, size, immr, wmask, T(sf), top,
                       SET(rd, sf, '(top & ~0x%xULL) | (bot & 0x%xULL)' % (tmask, tmask))))
        if g == 7:
            rm, lsb = bits(w, 20, 16), bits(w, 15, 10)
            size = 64 if sf else 32
            if lsb == 0:
                return SET(rd, sf, R(rm, sf))
            return SET(rd, sf, '(%s >> %d) | (%s << %d)' % (R(rm, sf), lsb, R(rn, sf), size - lsb))
        raise Unimpl('dpimm%d' % g)

    # ---- branches and system
    def branch(self, w, pc):
        if bits(w, 30, 26) == 5:
            target = pc + sext(bits(w, 25, 0), 26) * 4
            if bits(w, 31, 31):
                # `bl .+8; .word d`: hand-written code (rtld of SDK 20+ modules, e.g. title D)
                # reads a PC-relative word through x30 and returns to its own caller, never to the data word. As a call, the
                # lifted code would come back and run that word (an undefined-instruction trap), so it is a tail call.
                if target == pc + 8 and self.is_data_word(pc + 4):
                    return 'c->x[30] = 0x%xULL; { %s return; }' % (pc + 4, self.env.call(target))
                return 'c->x[30] = 0x%xULL; %s' % (pc + 4, self.env.call(target))
            return self.jump(target)
        if bits(w, 31, 24) == 0x54:
            target = pc + sext(bits(w, 23, 5), 19) * 4
            return 'if (cond(c, %d)) %s' % (bits(w, 3, 0), self.jump(target))
        if bits(w, 30, 25) == 0x1a:
            target = pc + sext(bits(w, 23, 5), 19) * 4
            return 'if (%s %s 0) %s' % (R(bits(w, 4, 0), bits(w, 31, 31)), '!=' if bits(w, 24, 24) else '==', self.jump(target))
        if bits(w, 30, 25) == 0x1b:
            bit = (bits(w, 31, 31) << 5) | bits(w, 23, 19)
            target = pc + sext(bits(w, 18, 5), 14) * 4
            return 'if (((%s >> %d) & 1) %s 0) %s' % (X(bits(w, 4, 0)), bit, '!=' if bits(w, 24, 24) else '==', self.jump(target))
        if bits(w, 31, 25) == 0x6b:
            opc, rn = bits(w, 24, 21), bits(w, 9, 5)
            if opc == 2:
                return 'return;'
            if opc == 1:
                return '{ uint64_t t = %s; c->x[30] = 0x%xULL; call_indirect(c, t); }' % (X(rn), pc + 4)
            if opc == 0:
                tg = self.env.jt.get(pc)
                if tg:                                   # a resolved jump table: a switch over exactly its targets
                    return 'switch (%s) { %s default: call_indirect(c, %s); return; }' % (
                        X(rn), ' '.join('case 0x%xULL: %s' % (t, self.jump(t)) for t in tg), X(rn))
                if pc in self.env.tailbr:                # an indirect tail call (vtable / function pointer): no dispatch
                    return '{ call_indirect(c, %s); return; }' % X(rn)
                self.has_br_fallback = True
                return '{ jt = %s; goto L_dispatch; }' % X(rn)
            raise Unimpl('br%d' % opc)
        if w == 0xd503205f or w == 0xd503203f:           # wfe / yield: a spin-wait hint; the runtime may give the host thread away
            return 'nx_spin_hint(%d);' % (1 if w == 0xd503205f else 0)
        if (w & 0xfffff01f) == 0xd503201f:
            return ';'                                   # nop / hint
        if (w & 0xffe0001f) == 0xd4000001:
            return 'guest_svc(c, 0x%x);' % bits(w, 20, 5)
        if (w & 0xffe0001f) == 0xd4200000:
            return 'guest_trap("brk", 0x%xULL);' % pc
        rt = bits(w, 4, 0)
        if (w & 0xffffffe0) == 0xd53bd040:               # mrs xt, tpidr_el0
            return SET(rt, 1, 'c->tpidr')
        if (w & 0xffffffe0) == 0xd53bd060:               # mrs xt, tpidrro_el0 (ScarletNX keeps it separate)
            return SET(rt, 1, 'c->tpidrro')
        if (w & 0xffffffe0) == 0xd51bd040:               # msr tpidr_el0, xt
            return 'c->tpidr = %s;' % X(rt)
        if (w & 0xffffffe0) == 0xd53b00e0:
            return SET(rt, 1, '0x10ULL')                 # dczid_el0: DC ZVA prohibited
        if (w & 0xffffffe0) == 0xd53b4400:               # mrs xt, fpcr
            return SET(rt, 1, 'c->fpcr')
        if (w & 0xffffffe0) == 0xd53b4420:               # mrs xt, fpsr
            return SET(rt, 1, 'c->fpsr')
        if (w & 0xffffffe0) == 0xd51b4400:               # msr fpcr, xt   (stored, not honoured: docs/GAPS.md)
            return 'c->fpcr = %s & 0x07f70000ULL;' % X(rt)      # the bits a Cortex-A57 implements
        if (w & 0xffffffe0) == 0xd51b4420:               # msr fpsr, xt
            return 'c->fpsr = %s & 0xf800009fULL;' % X(rt)
        if (w & 0xffffffe0) == 0xd53b4200:               # mrs xt, nzcv
            return SET(rt, 1, 'get_nzcv(c)')
        if (w & 0xffffffe0) == 0xd51b4200:               # msr nzcv, xt
            return 'set_nzcv(c, (int)(%s >> 28));' % X(rt)
        if (w & 0xfffff0ff) in (0xd50330bf, 0xd503309f):  # dmb / dsb: x86 is stronger than AArch64 except store->load
            # CRm<1:0> is the access type: 01 = LD (loads before the barrier against later loads and stores), 10 = ST (stores
            # against stores), 11 = all. x86 (TSO) keeps LD and ST ordering in hardware, so those need only a compiler
            # barrier; the full forms also order stores against later loads, which only mfence gives.
            if bits(w, 9, 8) in (1, 2):
                return '__atomic_signal_fence(__ATOMIC_SEQ_CST);'
            return '__atomic_thread_fence(__ATOMIC_SEQ_CST);'
        if (w & 0xfffff0ff) == 0xd503305f:               # clrex: the monitor is closed
            return 'c->ex_addr = ~0ULL;'
        if (w & 0xfffff0ff) == 0xd50330df:
            return ';'                                   # isb
        # smo-recomp additions (SDK code: a data-cache maintenance loop in the audio server)
        if (w & 0xffffffe0) == 0xd53b0020:               # mrs ctr_el0: the console's Cortex-A57 reset value 0x8444C004
            return SET(bits(w, 4, 0), 1, '0x8444c004ULL')  # [doc] ARM Cortex-A57 TRM, CTR_EL0: 64-byte I and D lines
        if (w & 0xffffffe0) == 0xd50b7e20:               # dc civac: clean + invalidate a data-cache line; the host
            return ';'                                   # memory the lifted code uses is coherent: nothing to do
        if (w & 0xfff00000) == 0xd5300000:               # any other mrs: the runtime answers (counter, ID registers)
            return SET(rt, 1, 'guest_mrs(c, 0x%x)' % bits(w, 20, 5))
        if (w & 0xfff00000) == 0xd5100000:               # any other msr (register form)
            return 'guest_msr(c, 0x%x, %s);' % (bits(w, 20, 5), X(rt))
        raise Unimpl('system')

    # ---- loads and stores
    def addr_mode(self, w, scale):
        """Single-register addressing. Returns (pre, address, post)."""
        rn = bits(w, 9, 5)
        base = XS(rn)
        if bits(w, 24, 24):
            return '', '%s + %dULL' % (base, bits(w, 21, 10) << scale), ''
        if bits(w, 21, 21) == 0:
            imm, mode = sext(bits(w, 20, 12), 9), bits(w, 11, 10)
            if mode == 0:
                return '', '%s + (uint64_t)%dLL' % (base, imm), ''
            if mode == 1:
                return '', base, '%s += (uint64_t)%dLL;' % (base, imm)
            if mode == 3:
                return '%s += (uint64_t)%dLL;' % (base, imm), base, ''
            raise Unimpl('ldst-unpriv')
        if bits(w, 11, 10) == 2:
            option, s, rm = bits(w, 15, 13), bits(w, 12, 12), bits(w, 20, 16)
            if option not in (2, 3, 6, 7):
                raise Unimpl('ldst-ext')
            return '', '%s + %s' % (base, extended(rm, option, scale if s else 0)), ''
        raise Unimpl('ldst-atomic')

    def loadstore(self, w, pc):
        v = bits(w, 26, 26)
        g = bits(w, 29, 27)
        rt, rn = bits(w, 4, 0), bits(w, 9, 5)
        if g == 5:                                       # pairs
            opc, mode, load = bits(w, 31, 30), bits(w, 24, 23), bits(w, 22, 22)
            rt2 = bits(w, 14, 10)
            if v:
                size = 4 << opc
            else:
                if opc == 3 or (opc == 1 and not load):
                    raise Unimpl('pair')
                size = 8 if opc == 2 else 4
            imm = sext(bits(w, 21, 15), 7) * size
            base = XS(rn)
            pre = '%s += (uint64_t)%dLL;' % (base, imm) if mode == 3 else ''
            post = '%s += (uint64_t)%dLL;' % (base, imm) if mode == 1 else ''
            off = imm if mode in (2, 0) else 0
            body = 'uint64_t a = %s + (uint64_t)%dLL; ' % (base, off)
            if v:
                if load:
                    body += 'V128 t1 = rdv(a, %d), t2 = rdv(a + %d, %d); c->v[%d] = t1; c->v[%d] = t2;' % (size, size, size, rt, rt2)
                else:
                    body += 'wrv(a, &c->v[%d], %d); wrv(a + %d, &c->v[%d], %d);' % (rt, size, size, rt2, size)
            elif load:
                rd = 'rd%d' % (size * 8)
                if opc == 1:
                    body += 'uint64_t t1 = (uint64_t)(int64_t)(int32_t)rd32(a), t2 = (uint64_t)(int64_t)(int32_t)rd32(a + 4); '
                else:
                    body += 'uint64_t t1 = %s(a), t2 = %s(a + %d); ' % (rd, rd, size)
                body += SET(rt, 1, 't1') + ' ' + SET(rt2, 1, 't2')
            else:
                body += 'wr%d(a, %s); wr%d(a + %d, %s);' % (size * 8, R(rt, size == 8), size * 8, size, R(rt2, size == 8))
            return '{ %s %s %s }' % (pre, body, post)
        if g == 7:                                       # single register
            size, opc = bits(w, 31, 30), bits(w, 23, 22)
            if v:
                nbytes = 16 if opc & 2 else 1 << size
                if opc & 2 and size != 0:
                    raise Unimpl('ldst-v')
                scale = 4 if opc & 2 else size
                pre, a, post = self.addr_mode(w, scale)
                if opc & 1:
                    body = 'c->v[%d] = rdv(a, %d);' % (rt, nbytes)
                else:
                    body = 'wrv(a, &c->v[%d], %d);' % (rt, nbytes)
                return '{ %s uint64_t a = %s; %s %s }' % (pre, a, body, post)
            if size == 3 and opc == 2 and (bits(w, 24, 24) or not (bits(w, 21, 21) and bits(w, 11, 10) == 0)):
                return ';'                               # prfm (the same size/opc class holds the LSE atomics ldadda...: tier 2)
            pre, a, post = self.addr_mode(w, size)
            nb = 8 << size
            if opc == 0:
                body = 'wr%d(a, %s);' % (nb, R(rt, size == 3))
            elif opc == 1:
                body = SET(rt, 1, 'rd%d(a)' % nb)
            else:
                ext = '(int64_t)(int%d_t)rd%d(a)' % (nb, nb)
                body = SET(rt, 1 if opc == 2 else 0, ext)
            return '{ %s uint64_t a = %s; %s %s }' % (pre, a, body, post)
        if g == 3 and bits(w, 24, 24) == 0:              # literal
            opc = bits(w, 31, 30)
            a = '0x%xULL' % (pc + sext(bits(w, 23, 5), 19) * 4)
            if v:
                return 'c->v[%d] = rdv(%s, %d);' % (rt, a, 4 << opc)
            if opc == 3:
                return ';'
            return SET(rt, 1, ['rd32(%s)', 'rd64(%s)', '(int64_t)(int32_t)rd32(%s)'][opc] % a)
        if bits(w, 29, 24) == 8 and not v:               # exclusive / ordered
            size, o2, load, o1 = bits(w, 31, 30), bits(w, 23, 23), bits(w, 22, 22), bits(w, 21, 21)
            if o1:
                # exclusive pair (LDXP / LDAXP / STXP / STLXP): two 32-bit registers are one 64-bit access, two 64-bit
                # registers one 128-bit access. (size 0 / 1 with o2 = 0 is not allocated; o2 = 1 is CAS / CASP: tier 2.)
                if o2 or size < 2:
                    raise Unimpl('cas')
                rt2, rs = bits(w, 14, 10), bits(w, 20, 16)
                if load:
                    if size == 2:
                        return ('{ uint64_t a = %s; uint64_t v = __atomic_load_n((uint64_t *)(uintptr_t)a, __ATOMIC_ACQUIRE); '
                                'c->ex_addr = a; c->ex_val = v; %s %s }' % (XS(rn), SET(rt, 0, '(uint32_t)v'), SET(rt2, 0, '(uint32_t)(v >> 32)')))
                    return ('{ uint64_t a = %s; uint64_t lo, hi; ldxp128(c, a, &lo, &hi); %s %s }'
                            % (XS(rn), SET(rt, 1, 'lo'), SET(rt2, 1, 'hi')))
                if size == 2:
                    return SET(rs, 0, 'stxp32(c, %s, %s, %s)' % (XS(rn), R(rt, 0), R(rt2, 0)))
                return SET(rs, 0, 'stxp64(c, %s, %s, %s)' % (XS(rn), R(rt, 1), R(rt2, 1)))
            nb = 8 << size
            rs = bits(w, 20, 16)
            if o2:                                       # ldar / stlr: ordered, not exclusive -> acquire / sequentially consistent store
                # AArch64 orders STLR before a later LDAR (RCsc); x86 lets a later load pass a plain (release) store, so STLR is a
                # seq_cst store (xchg): store-then-check handshakes (job systems, sleep/wake flags) otherwise see stale values.
                if load:
                    return SET(rt, 1, '__atomic_load_n((uint%d_t *)(uintptr_t)%s, __ATOMIC_ACQUIRE)' % (nb, XS(rn)))
                return '__atomic_store_n((uint%d_t *)(uintptr_t)%s, (uint%d_t)%s, __ATOMIC_SEQ_CST);' % (nb, XS(rn), nb, R(rt, size == 3))
            if load:                                     # ldxr / ldaxr: remember address and value
                return '{ uint64_t a = %s; c->ex_addr = a; c->ex_val = rd%d(a); %s }' % (XS(rn), nb, SET(rt, 1, 'c->ex_val'))
            # stxr / stlxr: succeeds only if the value is still the one loaded (compare-and-swap)
            return SET(rs, 0, 'stx%d(c, %s, %s)' % (nb, XS(rn), R(rt, size == 3)))
        if bits(w, 31, 31) == 0 and bits(w, 29, 24) in (0x0c, 0x0d):   # AdvSIMD structure load/store
            q, post, load, rm = bits(w, 30, 30), bits(w, 23, 23), bits(w, 22, 22), bits(w, 20, 16)
            size = bits(w, 11, 10)
            if bits(w, 24, 24) == 0:
                opcode = bits(w, 15, 12)
                nb = 16 if q else 8
                nreg = {7: 1, 10: 2, 6: 3, 2: 4}.get(opcode)          # LD1/ST1: one structure, 1..4 registers
                if nreg is not None:
                    if load:
                        body = ' '.join('V128 t%d = rdv(a + %d, %d);' % (i, i * nb, nb) for i in range(nreg))
                        body += ' ' + ' '.join('c->v[%d] = t%d;' % ((rt + i) & 31, i) for i in range(nreg))
                    else:
                        body = ' '.join('wrv(a + %d, &c->v[%d], %d);' % (i * nb, (rt + i) & 31, nb) for i in range(nreg))
                    total = nb * nreg
                else:
                    nstruct = {8: 2, 4: 3, 0: 4}.get(opcode)           # LD2..LD4 / ST2..ST4: interleaved
                    if nstruct is None or (size == 3 and not q):
                        raise Unimpl('ldN')
                    body = '%s(c, a, %d, %d, %d, %d);' % ('v_ldn' if load else 'v_stn', rt, nstruct, size, q)
                    total = nb * nstruct
            else:
                opcode, s_bit = bits(w, 15, 13), bits(w, 12, 12)
                if bits(w, 21, 21) and opcode in (0, 1, 2, 3, 4, 5):
                    return self.ldst_lanes(w, rt, rn, rm, q, post, load, size, opcode, s_bit)
                if bits(w, 21, 21):
                    raise Unimpl('ldN')
                if opcode == 6 and load:
                    body = 'v_dup(&c->v[%d], rd%d(a), %d, %d);' % (rt, 8 << size, size, (16 if q else 8) >> size)
                    total = 1 << size
                else:
                    if opcode == 0:
                        es, idx = 0, (q << 3) | (s_bit << 2) | size
                    elif opcode == 2:
                        es, idx = 1, (q << 2) | (s_bit << 1) | (size >> 1)
                    elif opcode == 4 and size == 0:
                        es, idx = 2, (q << 1) | s_bit
                    elif opcode == 4 and size == 1:
                        es, idx = 3, q
                    else:
                        raise Unimpl('ldN')
                    if load:
                        body = 'vset(&c->v[%d], %d, %d, rd%d(a));' % (rt, es, idx, 8 << es)
                    else:
                        body = 'wr%d(a, vget(&c->v[%d], %d, %d));' % (8 << es, rt, es, idx)
                    total = 1 << es
            wb = ''
            if post:
                wb = '%s += %s;' % (XS(rn), ('%dULL' % total) if rm == 31 else X(rm))
            return '{ uint64_t a = %s; %s %s }' % (XS(rn), body, wb)
        raise Unimpl('ldst%d' % g)

    def ldst_lanes(self, w, rt, rn, rm, q, post, load, size, opcode, s_bit):
        """smo-recomp addition (audio DSP of the 7.3.2 SDK): LD2 / ST2 (opcode 0, 2, 4) and LD4 / ST4 (opcode 1, 3, 5), one
        element (lane) of two or four consecutive registers to or from consecutive memory."""
        nstruct = 4 if opcode & 1 else 2
        base = opcode & 6
        if base == 0:
            es, idx = 0, (q << 3) | (s_bit << 2) | size
        elif base == 2 and not size & 1:
            es, idx = 1, (q << 2) | (s_bit << 1) | (size >> 1)
        elif base == 4 and size == 0:
            es, idx = 2, (q << 1) | s_bit
        elif base == 4 and size == 1 and not s_bit:
            es, idx = 3, q
        else:
            raise Unimpl('ldN')
        nb = 1 << es
        if load:
            body = ' '.join('uint64_t e%d = rd%d(a + %d);' % (k, 8 << es, k * nb) for k in range(nstruct))
            body += ' ' + ' '.join('vset(&c->v[%d], %d, %d, e%d);' % ((rt + k) & 31, es, idx, k) for k in range(nstruct))
        else:
            body = ' '.join('wr%d(a + %d, vget(&c->v[%d], %d, %d));' % (8 << es, k * nb, (rt + k) & 31, es, idx)
                            for k in range(nstruct))
        wb = ''
        if post:
            wb = '%s += %s;' % (XS(rn), ('%dULL' % (nb * nstruct)) if rm == 31 else X(rm))
        return '{ uint64_t a = %s; %s %s }' % (XS(rn), body, wb)

    # ---- data processing, register
    def dp_reg(self, w, pc):
        sf, rd, rn, rm = bits(w, 31, 31), bits(w, 4, 0), bits(w, 9, 5), bits(w, 20, 16)
        size = 64 if sf else 32
        if bits(w, 28, 28) == 0:
            if bits(w, 24, 24) == 0:                     # logical, shifted register
                opc, n = bits(w, 30, 29), bits(w, 21, 21)
                b = shifted(R(rm, sf), bits(w, 23, 22), bits(w, 15, 10), sf)
                if n:
                    b = '~%s' % b
                e = '%s %s %s' % (R(rn, sf), '&|^&'[opc], b)
                if opc == 3:
                    return SET(rd, sf, 'logic%d(c, %s)' % (size, e))
                return SET(rd, sf, e)
            sub, s = bits(w, 30, 30), bits(w, 29, 29)
            if bits(w, 21, 21) == 0:                     # add/sub, shifted register
                b = shifted(R(rm, sf), bits(w, 23, 22), bits(w, 15, 10), sf)
                a = R(rn, sf)
                sp = False
            else:                                        # add/sub, extended register
                b = '(%s)%s' % (T(sf), extended(rm, bits(w, 15, 13), bits(w, 12, 10)))
                a = R(rn, sf, sp=True)
                sp = True
            if s:
                return SET(rd, sf, 'adds%d(c, %s, %s, %d)' % (size, a, ('~(%s)' % b) if sub else b, sub))
            return SET(rd, sf, '%s %s %s' % (a, '-' if sub else '+', b), sp=sp)
        g = bits(w, 24, 21)
        if bits(w, 24, 24):                              # three-source
            op31, o0, ra = bits(w, 23, 21), bits(w, 15, 15), bits(w, 14, 10)
            sign = '-' if o0 else '+'
            if op31 == 0:
                return SET(rd, sf, '%s %s %s * %s' % (R(ra, sf), sign, R(rn, sf), R(rm, sf)))
            if op31 == 1:
                return SET(rd, 1, '%s %s (uint64_t)((int64_t)(int32_t)%s * (int64_t)(int32_t)%s)' % (X(ra), sign, W(rn), W(rm)))
            if op31 == 5:
                return SET(rd, 1, '%s %s (uint64_t)%s * (uint64_t)%s' % (X(ra), sign, W(rn), W(rm)))
            if op31 == 2:
                return SET(rd, 1, 'smulh(%s, %s)' % (X(rn), X(rm)))
            if op31 == 6:
                return SET(rd, 1, 'umulh(%s, %s)' % (X(rn), X(rm)))
            raise Unimpl('mul3')
        if g == 0 and bits(w, 15, 10) == 0:              # adc / sbc
            sub, s = bits(w, 30, 30), bits(w, 29, 29)
            b = ('~%s' % R(rm, sf)) if sub else R(rm, sf)
            if s:
                return SET(rd, sf, 'adds%d(c, %s, %s, c->C)' % (size, R(rn, sf), b))
            return SET(rd, sf, '%s + %s + c->C' % (R(rn, sf), b))
        if g == 2:                                       # ccmn / ccmp
            sub, cc, nzcv = bits(w, 30, 30), bits(w, 15, 12), bits(w, 3, 0)
            b = ('%dULL' % rm) if bits(w, 11, 11) else R(rm, sf)
            b = ('~(%s)%s' % (T(sf), b)) if sub else '(%s)%s' % (T(sf), b)
            return ('if (cond(c, %d)) (void)adds%d(c, %s, %s, %d); else set_nzcv(c, %d);'
                    % (cc, size, R(rn, sf), b, sub, nzcv))
        if g == 4:                                       # csel family
            op, o2, cc = bits(w, 30, 30), bits(w, 10, 10), bits(w, 15, 12)
            b = R(rm, sf)
            alt = [b, '%s + 1' % b, '~%s' % b, '(%s)0 - %s' % (T(sf), b)][op * 2 + o2]
            return SET(rd, sf, 'cond(c, %d) ? %s : %s' % (cc, R(rn, sf), alt))
        if g == 6:
            opcode = bits(w, 15, 10)
            if bits(w, 30, 30) == 0:                     # two-source
                a, b = R(rn, sf), R(rm, sf)
                if opcode == 2:
                    return SET(rd, sf, '%s ? %s / %s : 0' % (b, a, b))
                if opcode == 3:
                    return SET(rd, sf, 'sdiv%d(%s, %s)' % (size, a, b))
                if opcode in (8, 9, 10, 11):
                    amt = '(%s & %d)' % (b, size - 1)
                    if opcode == 8:
                        return SET(rd, sf, '%s << %s' % (a, amt))
                    if opcode == 9:
                        return SET(rd, sf, '%s >> %s' % (a, amt))
                    if opcode == 10:
                        return SET(rd, sf, '(%s)%s >> %s' % ('int64_t' if sf else 'int32_t', a, amt))
                    return SET(rd, sf, 'ror%d(%s, %s)' % (size, a, amt))
                raise Unimpl('dp2')
            if bits(w, 20, 16) or bits(w, 29, 29):
                # opcode2 != 0 is pointer authentication (PACIA/PACIB/... AUT*, XPAC*: tier 2, x_pac.py). Only opcode2 == 0 is
                # RBIT/REV/CLZ/CLS: PACIB x30, sp (platform 2 setjmp) was lifted as REV16 of xzr.
                raise Unimpl('dp1 opcode2')
            a = R(rn, sf)
            if opcode == 0:
                return SET(rd, sf, 'rbit%d(%s)' % (size, a))
            if opcode == 1:
                return SET(rd, sf, 'rev16_%d(%s)' % (size, a))
            if opcode == 2:
                return SET(rd, sf, 'rev32_64(%s)' % a if sf else 'bswap32(%s)' % a)
            if opcode == 3 and sf:
                return SET(rd, sf, 'bswap64(%s)' % a)
            if opcode == 4:
                return SET(rd, sf, 'clz%d(%s)' % (size, a))
            if opcode == 5:
                return SET(rd, sf, 'cls%d(%s)' % (size, a))
            raise Unimpl('dp1')
        raise Unimpl('dpreg%d' % g)

    # ---- scalar floating point (advanced SIMD is handled only where needed)
    def fpsimd(self, w, pc):
        rd, rn, rm = bits(w, 4, 0), bits(w, 9, 5), bits(w, 20, 16)
        if bits(w, 30, 24) == 0x1e and bits(w, 21, 21) == 0:     # fixed-point <-> fp: the value is scaled by 2^fbits
            sf, ty, rmode, opcode, fbits = bits(w, 31, 31), bits(w, 23, 22), bits(w, 20, 19), bits(w, 18, 16), 64 - bits(w, 15, 10)
            if ty > 1 or fbits < 1 or fbits > (64 if sf else 32):
                raise Unimpl('fp16')
            f = 'df[0]' if ty else 'f[0]'
            ft = 'double' if ty else 'float'
            if rmode == 0 and opcode in (2, 3):              # SCVTF / UCVTF: one rounding, then an exact scale
                src = R(rn, sf)
                if opcode == 2:
                    src = '(%s)%s' % ('int64_t' if sf else 'int32_t', src)
                return '{ %s t = (%s)%s * (%s)%s; %s }' % (ft, ft, src, ft, repr(2.0 ** -fbits), vput(rd, f))
            if rmode == 3 and opcode in (0, 1):              # FCVTZS / FCVTZU
                return SET(rd, sf, 'f2%s%d(trunc((double)c->v[%d].%s * %s))' % ('u' if opcode else 's', 64 if sf else 32, rn, f, repr(2.0 ** fbits)))
            raise Unimpl('fcvt')
        if bits(w, 31, 24) == 0x1e and bits(w, 21, 21) == 1 or bits(w, 30, 24) == 0x1e and bits(w, 21, 21) == 1 and bits(w, 15, 10) == 0:
            ty = bits(w, 23, 22)
            if bits(w, 15, 10) == 0:                     # int <-> fp
                sf, rmode, opcode = bits(w, 31, 31), bits(w, 20, 19), bits(w, 18, 16)
                if ty == 2 and rmode == 1 and opcode in (6, 7):
                    if opcode == 6:
                        return SET(rd, 1, 'c->v[%d].d[1]' % rn)
                    return 'c->v[%d].d[1] = %s;' % (rd, X(rn))
                if ty > 1:
                    raise Unimpl('fp16')
                f = 'df[0]' if ty else 'f[0]'
                ft = 'double' if ty else 'float'
                if rmode == 0 and opcode in (2, 3):
                    src = R(rn, sf)
                    if opcode == 2:
                        src = '(%s)%s' % ('int64_t' if sf else 'int32_t', src)
                    return '{ %s t = (%s)%s; %s }' % (ft, ft, src, vput(rd, f))
                if rmode == 0 and opcode == 6:
                    return SET(rd, sf, 'c->v[%d].%s' % (rn, 'd[0]' if sf else 'w[0]'))
                if rmode == 0 and opcode == 7:
                    return '{ uint64_t t = %s; %s }' % (R(rn, sf), vput(rd, 'd[0]' if sf else 'w[0]'))
                rnd = {(3, 0): 'trunc', (3, 1): 'trunc', (0, 0): 'rint', (0, 1): 'rint', (1, 0): 'ceil', (1, 1): 'ceil',
                       (2, 0): 'floor', (2, 1): 'floor', (0, 4): 'round', (0, 5): 'round'}.get((rmode, opcode))
                if rnd is None:
                    raise Unimpl('fcvt')
                unsigned = opcode & 1
                return SET(rd, sf, 'f2%s%d(%s((double)c->v[%d].%s))' % ('u' if unsigned else 's', 64 if sf else 32, rnd, rn, f))
            if bits(w, 31, 24) == 0x1e and bits(w, 14, 10) == 0x10 and ty in (0, 1, 3):
                # FCVT to or from half precision (opcode 7 = to half; type 3 = from half, opcode 4 / 5 = to single / double)
                op = bits(w, 20, 15)
                if ty != 3 and op == 7:
                    return '{ uint16_t t = %s(c->v[%d].%s); %s }' % ('f64_to_half' if ty else 'f32_to_half', rn, 'df[0]' if ty else 'f[0]', vput(rd, 'h[0]'))
                if ty == 3 and op in (4, 5):
                    tf, tt, fn = ('df[0]', 'double', 'half_to_f64') if op == 5 else ('f[0]', 'float', 'half_to_f32')
                    return '{ %s t = %s(c->v[%d].h[0]); %s }' % (tt, fn, rn, vput(rd, tf))
            if bits(w, 31, 24) != 0x1e or ty > 1:
                raise Unimpl('fp16')
            f = 'df[0]' if ty else 'f[0]'
            ft = 'double' if ty else 'float'
            sfx = '' if ty else 'f'

            nbits = 64 if ty else 32                 # the arithmetic is exact in fpexact.h (fp_<op><bits>); setf adds no NaN fix
            def setf(expr):
                return '{ %s t = %s; %s }' % (ft, expr, vput(rd, f))
            a, b = 'c->v[%d].%s' % (rn, f), 'c->v[%d].%s' % (rm, f)
            if bits(w, 11, 10) == 2:
                op = bits(w, 15, 12)
                if op < 4:                           # FMUL, FDIV, FADD, FSUB
                    return setf('fp_%s%d(%s, %s)' % (('mul', 'div', 'add', 'sub')[op], nbits, a, b))
                if op in (4, 5, 6, 7):               # FMAX, FMIN, FMAXNM, FMINNM: exact NaN and signed-zero rules (fpexact.h); the result is an operand, no fnan
                    return '{ %s t = fp_maxmin%d(%s, %s, %d, %d); %s }' % (ft, 64 if ty else 32, a, b, op & 1, op >> 1 & 1, vput(rd, f))
                if op == 8:                          # FNMUL = FPNeg(FPMul(n, m)): the sign is flipped after the product
                    return setf('fp_neg%d(fp_mul%d(%s, %s))' % (nbits, nbits, a, b))
                raise Unimpl('fp2')
            if bits(w, 14, 10) == 0x10:
                op = bits(w, 20, 15)
                if op == 0:                          # FMOV (register): a bit copy
                    return setf(a)
                if op == 1:                          # FABS: clears the sign bit, no quieting
                    return setf('fp_abs%d(%s)' % (nbits, a))
                if op == 2:                          # FNEG: flips the sign bit, no quieting
                    return setf('fp_neg%d(%s)' % (nbits, a))
                if op == 3:
                    return setf('fp_sqrt%d(%s)' % (nbits, a))
                if op in (4, 5):
                    tf, tt = ('df[0]', 'double') if op == 5 else ('f[0]', 'float')
                    # FCVT single <-> double: a signalling NaN is quietened on the bit patterns (fp_s2d / fp_d2s, fpexact.h)
                    return '{ %s t = %s(%s); %s }' % (tt, 'fp_s2d' if op == 5 else 'fp_d2s', a, vput(rd, tf))
                # FRINTN (8) nearest-even, FRINTP (9) ceiling, FRINTM (10) floor, FRINTZ (11) towards zero,
                # FRINTA (12) nearest ties away, FRINTX (14) and FRINTI (15) the current mode = nearest-even
                frmode = {8: 0, 9: 3, 10: 2, 11: 4, 12: 1, 14: 0, 15: 0}.get(op)
                if frmode is not None:
                    return setf('fp_frint%d(%s, %d)' % (nbits, a, frmode))
                raise Unimpl('fp1')
            if bits(w, 13, 10) == 8:
                b2 = '0' if bits(w, 3, 3) else b
                return 'fcmp(c, (double)%s, (double)%s);' % (a, b2)
            if bits(w, 12, 10) == 4:
                imm8 = bits(w, 20, 13)
                sign = -1.0 if imm8 & 0x80 else 1.0
                exp = ((imm8 >> 4) & 7) ^ 4
                val = sign * (16 + (imm8 & 0xf)) / 16.0 * (2.0 ** (exp - 3))
                return setf('(%s)%r' % (ft, val))
            if bits(w, 11, 10) == 1:
                return ('if (cond(c, %d)) fcmp(c, (double)%s, (double)%s); else set_nzcv(c, %d);'
                        % (bits(w, 15, 12), a, b, bits(w, 3, 0)))
            if bits(w, 11, 10) == 3:
                return setf('cond(c, %d) ? %s : %s' % (bits(w, 15, 12), a, b))
            raise Unimpl('fp')
        if bits(w, 31, 24) == 0x1f:                      # fmadd family
            ty = bits(w, 23, 22)
            if ty > 1:
                raise Unimpl('fp16')
            f = 'df[0]' if ty else 'f[0]'
            ft = 'double' if ty else 'float'
            a, b, r = 'c->v[%d].%s' % (rn, f), 'c->v[%d].%s' % (rm, f), 'c->v[%d].%s' % (bits(w, 14, 10), f)
            o1, o0 = bits(w, 21, 21), bits(w, 15, 15)
            nbits = 64 if ty else 32
            # fp_fma(op1, op2, addend) = FPMulAdd of fpexact.h. The Arm pseudocode negates operands with FPNeg, so the
            # sign of a NaN taken from them is flipped too: FMADD a + n*m, FMSUB a - n*m, FNMADD -a - n*m, FNMSUB n*m - a.
            na, nr = 'fp_neg%d(%s)' % (nbits, a), 'fp_neg%d(%s)' % (nbits, r)
            fma = 'fp_fma%d' % nbits
            expr = ['%s(%s, %s, %s)' % (fma, a, b, r), '%s(%s, %s, %s)' % (fma, na, b, r),
                    '%s(%s, %s, %s)' % (fma, na, b, nr), '%s(%s, %s, %s)' % (fma, a, b, nr)][o1 * 2 + o0]
            return '{ %s t = %s; %s }' % (ft, expr, vput(rd, f))
        return self.simd(w, pc)

    def simd(self, w, pc):
        rd, rn, rm = bits(w, 4, 0), bits(w, 9, 5), bits(w, 20, 16)
        if (w & 0x9ff80c00) == 0x0f000400:               # movi / mvni / orr-imm (modified immediate)
            q, op, cmode = bits(w, 30, 30), bits(w, 29, 29), bits(w, 15, 12)
            imm8 = (bits(w, 18, 16) << 5) | bits(w, 9, 5)
            def vfp(double):
                sign = -1.0 if imm8 & 0x80 else 1.0
                value = sign * (16 + (imm8 & 0xf)) / 16.0 * (2.0 ** ((((imm8 >> 4) & 7) ^ 4) - 3))
                if double:
                    return struct.unpack('<Q', struct.pack('<d', value))[0]
                return struct.unpack('<I', struct.pack('<f', value))[0] * 0x100000001
            if cmode == 15:
                val = vfp(op == 1)
            elif cmode == 14 and op == 1:
                val = sum(0xff << (8 * i) for i in range(8) if imm8 & (1 << i))
            elif cmode == 14:
                val = imm8 * 0x0101010101010101
            elif cmode in (0, 2, 4, 6, 12, 13):
                v32 = imm8 << (8 * (cmode >> 1)) if cmode < 12 else ((imm8 << 8) | 0xff if cmode == 12 else (imm8 << 16) | 0xffff)
                if op:
                    v32 ^= 0xffffffff
                val = v32 * 0x100000001
            elif cmode in (8, 10):
                v16 = imm8 << (8 * ((cmode >> 1) & 1))
                if op:
                    v16 ^= 0xffff
                val = v16 * 0x0001000100010001
            else:
                raise Unimpl('simd-imm')
            return '{ c->v[%d].d[0] = 0x%xULL; c->v[%d].d[1] = %s; }' % (rd, val, rd, ('0x%xULL' % val) if q else '0')
        if (w & 0xbfe0fc00) == 0x0ea01c00 and rn == rm:  # mov vD.16b, vN.16b (orr)
            q = bits(w, 30, 30)
            return '{ V128 t = c->v[%d]; %s c->v[%d] = t; }' % (rn, '' if q else 't.d[1] = 0;', rd)
        bitwise = {0x0e201c00: 'a & b', 0x2e201c00: 'a ^ b', 0x0e601c00: 'a & ~b', 0x0ea01c00: 'a | b',
                   0x0ee01c00: 'a | ~b'}
        if (w & 0xbfe0fc00) in bitwise:                  # and / eor / bic / orr / orn
            q = bits(w, 30, 30)
            opn = bitwise[w & 0xbfe0fc00]
            body = ''.join('{ uint64_t a = c->v[%d].d[%d], b = c->v[%d].d[%d]; t.d[%d] = %s; } ' % (rn, i, rm, i, i, opn)
                           for i in range(2 if q else 1))
            return '{ V128 t; t.d[1] = 0; %s c->v[%d] = t; }' % (body, rd)
        if bits(w, 31, 31) or bits(w, 27, 25) != 7:
            raise Unimpl('simd')
        scalar, q, u = bits(w, 28, 28), bits(w, 30, 30), bits(w, 29, 29)
        if scalar and not q:
            raise Unimpl('simd')
        size, sz, a = bits(w, 23, 22), bits(w, 22, 22), bits(w, 23, 23)

        def V(n):
            return '&c->v[%d]' % n

        def fcount():
            return 1 if scalar else ((2 if sz else 4) if q else (1 if sz else 2))

        def icount(es):
            return 1 if scalar else (16 if q else 8) >> es

        if bits(w, 27, 24) == 0xe and bits(w, 21, 21):
            if bits(w, 10, 10):                          # three registers, same type
                opc = bits(w, 15, 11)
                if opc >= 0x18:
                    op = {(0, 0, 0x1a): 'F_ADD', (0, 1, 0x1a): 'F_SUB', (1, 0, 0x1b): 'F_MUL', (1, 0, 0x1f): 'F_DIV',
                          (0, 0, 0x19): 'F_MLA', (0, 1, 0x19): 'F_MLS', (0, 0, 0x1e): 'F_MAX', (0, 1, 0x1e): 'F_MIN',
                          (0, 0, 0x18): 'F_MAXNM', (0, 1, 0x18): 'F_MINNM', (1, 0, 0x1a): 'F_ADDP', (1, 1, 0x1a): 'F_ABD',
                          (0, 0, 0x1c): 'F_CMEQ', (1, 0, 0x1c): 'F_CMGE', (1, 1, 0x1c): 'F_CMGT',
                          (0, 0, 0x1f): 'F_RECPS', (0, 1, 0x1f): 'F_RSQRTS'}.get((u, a, opc))
                    if op is None or (op == 'F_ADDP' and scalar):
                        raise Unimpl('simd-f3')
                    return 'v_f3v(%s, %s, %s, %s, %d, %d);' % (V(rd), V(rn), V(rm), op, sz, fcount())
                if opc == 3 and u and size:
                    return 'v_bitsel(%s, %s, %s, %d, %d);' % (V(rd), V(rn), V(rm), size, q)
                op = {(0, 0x10): 'I_ADD', (1, 0x10): 'I_SUB', (0, 0x13): 'I_MUL', (1, 0x11): 'I_CMEQ', (0, 0x11): 'I_CMTST',
                      (0, 0x06): 'I_CMGT', (1, 0x06): 'I_CMHI', (0, 0x07): 'I_CMGE', (1, 0x07): 'I_CMHS',
                      # smo-recomp additions (audio DSP in the SDK): saturating add / subtract, multiply-accumulate (7.3.2)
                      (0, 0x01): 'I_SQADD', (1, 0x01): 'I_UQADD', (0, 0x05): 'I_SQSUB', (1, 0x05): 'I_UQSUB',
                      (0, 0x12): 'I_MLA'}.get((u, opc))
                if op is None or (op == 'I_MLA' and size == 3):
                    raise Unimpl('simd-i3')
                return 'v_i3v(%s, %s, %s, %s, %d, %d);' % (V(rd), V(rn), V(rm), op, size, icount(size))
            if bits(w, 20, 17) == 0 and bits(w, 11, 10) == 2:    # two registers, miscellaneous
                opc = bits(w, 16, 12)
                op = {(0, 1, 0x0f): 'F1_ABS', (1, 1, 0x0f): 'F1_NEG', (1, 1, 0x1f): 'F1_SQRT', (1, 1, 0x1d): 'F1_RSQRTE',
                      (0, 1, 0x1d): 'F1_RECPE', (0, 0, 0x1d): 'F1_SCVTF', (1, 0, 0x1d): 'F1_UCVTF', (0, 1, 0x1b): 'F1_CVTZS',
                      (1, 1, 0x1b): 'F1_CVTZU', (0, 1, 0x0d): 'F1_CMEQ0', (0, 1, 0x0c): 'F1_CMGT0', (1, 1, 0x0c): 'F1_CMGE0',
                      (0, 1, 0x0e): 'F1_CMLT0', (1, 1, 0x0d): 'F1_CMLE0'}.get((u, a, opc))
                # FRINTN (u0 a0 18), FRINTM (u0 a0 19), FRINTP (u0 a1 18), FRINTZ (u0 a1 19), FRINTA (u1 a0 18),
                # FRINTX (u1 a0 19), FRINTI (u1 a1 19); modes as in fp_frint32 (fpexact.h)
                frint = {(0, 0, 0x18): 0, (0, 0, 0x19): 2, (0, 1, 0x18): 3, (0, 1, 0x19): 4, (1, 0, 0x18): 1,
                         (1, 0, 0x19): 0, (1, 1, 0x19): 0}.get((u, a, opc))
                if frint is not None and not scalar:
                    return 'v_frint(%s, %s, %d, %d, %d);' % (V(rd), V(rn), frint, sz, fcount())
                if op:
                    return 'v_f1v(%s, %s, %s, %d, %d);' % (V(rd), V(rn), op, sz, fcount())
                if (u, opc) == (0, 0x12) and not scalar and size < 3:
                    return 'v_xtn(%s, %s, %d, %d);' % (V(rd), V(rn), size, q)
                op = {(1, 0x05): 'I1_NOT', (0, 0x05): 'I1_CNT', (1, 0x0b): 'I1_NEG', (0, 0x0b): 'I1_ABS',
                      (0, 0x09): 'I1_CMEQ0', (0, 0x00): 'I1_REV64'}.get((u, opc))
                if op is None or (op in ('I1_NOT', 'I1_CNT') and size):
                    raise Unimpl('simd-misc')
                return 'v_i1v(%s, %s, %s, %d, %d);' % (V(rd), V(rn), op, size, icount(size))
            if scalar and u and bits(w, 20, 17) == 8 and bits(w, 16, 12) == 0x0d and bits(w, 11, 10) == 2:
                f, ft = ('df', 'double') if sz else ('f', 'float')   # faddp scalar
                return '{ %s t = c->v[%d].%s[0] + c->v[%d].%s[1]; %s }' % (ft, rn, f, rn, f, vput(rd, f + '[0]'))
            if bits(w, 11, 10) == 0 and not scalar and size < 3:    # smo-recomp addition: three registers, different types
                acc = {0xc: 0, 0x8: 1, 0xa: 2}.get(bits(w, 15, 12))  # SMULL/UMULL, SMLAL/UMLAL, SMLSL/UMLSL (and the "2" forms)
                if acc is not None:
                    return 'v_mull(%s, %s, %s, %d, %d, %d, %d, -1);' % (V(rd), V(rn), V(rm), size, q, u, acc)
            # FMAXV (U1 sz0 0f), FMINV (U1 sz1 0f), FMAXNMV (U1 sz0 0c), FMINNMV (U1 sz1 0c): across the 4 single lanes
            if u and not bits(w, 23, 23) and not scalar and bits(w, 20, 17) == 8 and bits(w, 11, 10) == 2 \
                    and bits(w, 16, 12) in (0x0c, 0x0f):
                if not q:
                    raise Unimpl('simd-across')
                return 'v_fred32(%s, %s, %d, %d);' % (V(rd), V(rn), bits(w, 22, 22), int(bits(w, 16, 12) == 0x0c))
            # smo-recomp addition (audio DSP of the 7.3.2 SDK): ADDV, across lanes
            if not scalar and not u and bits(w, 20, 17) == 8 and bits(w, 16, 12) == 0x1b and bits(w, 11, 10) == 2:
                if size == 3 or (size == 2 and not q):
                    raise Unimpl('simd-across')
                return 'v_addv(%s, %s, %d, %d);' % (V(rd), V(rn), size, icount(size))
            raise Unimpl('simd-e')
        if bits(w, 27, 24) == 0xe:
            if bits(w, 15, 15) == 0 and bits(w, 10, 10) == 1:    # copy: dup / ins / umov / smov
                imm5, imm4 = bits(w, 20, 16), bits(w, 14, 11)
                if imm5 & 0xf == 0:
                    raise Unimpl('simd-copy')
                es = (imm5 & -imm5).bit_length() - 1
                idx = imm5 >> (es + 1)
                if scalar:
                    if u or imm4:
                        raise Unimpl('simd-copy')
                    return '{ uint64_t t = vget(%s, %d, %d); vzero(%s); vset(%s, %d, 0, t); }' % (V(rn), es, idx, V(rd), V(rd), es)
                if u:
                    return 'vset(%s, %d, %d, vget(%s, %d, %d));' % (V(rd), es, idx, V(rn), es, imm4 >> es)
                if imm4 == 0:
                    return 'v_dup(%s, vget(%s, %d, %d), %d, %d);' % (V(rd), V(rn), es, idx, es, icount(es))
                if imm4 == 1:
                    return 'v_dup(%s, %s, %d, %d);' % (V(rd), X(rn), es, icount(es))
                if imm4 == 3:
                    return 'vset(%s, %d, %d, %s);' % (V(rd), es, idx, X(rn))
                if imm4 == 7:
                    return SET(rd, 1, 'vget(%s, %d, %d)' % (V(rn), es, idx))
                if imm4 == 5:
                    return SET(rd, q, 'vsext(vget(%s, %d, %d), %d)' % (V(rn), es, idx, es))
                raise Unimpl('simd-copy')
            if bits(w, 15, 15) == 0 and bits(w, 10, 10) == 0 and not scalar:
                if u:
                    return 'v_ext(%s, %s, %s, %d, %d);' % (V(rd), V(rn), V(rm), bits(w, 14, 11), q)
                if bits(w, 11, 10) == 0:
                    return 'v_tbl(c, %d, %d, %d, %d, %d, %d);' % (rd, rn, bits(w, 14, 13) + 1, rm, q, bits(w, 12, 12))
                return 'v_perm(%s, %s, %s, %d, %d, %d);' % (V(rd), V(rn), V(rm), bits(w, 14, 12), size, icount(size))
            raise Unimpl('simd-e')
        if bits(w, 10, 10) == 0:                          # by element
            acc = {0xa: 0, 0x2: 1, 0x6: 2}.get(bits(w, 15, 12))       # smo-recomp addition: SMULL / SMLAL / SMLSL (and U, "2") by element
            if acc is not None and not scalar and size in (1, 2):
                if size == 1:
                    idx, em = (bits(w, 11, 11) << 2) | (bits(w, 21, 21) << 1) | bits(w, 20, 20), bits(w, 19, 16)
                else:
                    idx, em = (bits(w, 11, 11) << 1) | bits(w, 21, 21), bits(w, 20, 16)
                return 'v_mull(%s, %s, %s, %d, %d, %d, %d, %d);' % (V(rd), V(rn), V(em), size, q, u, acc, idx)
            # smo-recomp addition: integer MUL (U=0 opcode 1000), MLA (U=1 0000), MLS (U=1 0100) by element
            macc = {(0, 0x8): 0, (1, 0x0): 1, (1, 0x4): 2}.get((u, bits(w, 15, 12)))
            if macc is not None and not scalar and size in (1, 2):
                if size == 1:
                    idx, em = (bits(w, 11, 11) << 2) | (bits(w, 21, 21) << 1) | bits(w, 20, 20), bits(w, 19, 16)
                else:
                    idx, em = (bits(w, 11, 11) << 1) | bits(w, 21, 21), bits(w, 20, 16)
                return 'v_mul_elem(%s, %s, %s, %d, %d, %d, %d);' % (V(rd), V(rn), V(em), size, idx, macc, icount(size))
            op = {9: 'F_MUL', 1: 'F_MLA', 5: 'F_MLS'}.get(bits(w, 15, 12))
            if op is None or u or not a:
                raise Unimpl('simd-elem')
            idx = bits(w, 11, 11) if sz else (bits(w, 11, 11) << 1) | bits(w, 21, 21)
            n = 1 if scalar else ((2 if sz else 4) if q else 2)
            return 'v_f3e(%s, %s, %s, %d, %s, %d, %d);' % (V(rd), V(rn), V(bits(w, 20, 16)), idx, op, sz, n)
        immh, immhb, opc = bits(w, 22, 19), bits(w, 22, 16), bits(w, 15, 11)
        if immh == 0:
            raise Unimpl('simd-imm')
        es = immh.bit_length() - 1
        if opc == 0x14 and not scalar and es < 3:
            return 'v_shll(%s, %s, %d, %d, %d, %d);' % (V(rd), V(rn), immhb - (8 << es), es, u, q)
        if opc == 0x0a and not u:
            return 'v_shift(%s, %s, %d, %d, 0, %d);' % (V(rd), V(rn), immhb - (8 << es), es, icount(es))
        if opc == 0:
            return 'v_shift(%s, %s, %d, %d, %d, %d);' % (V(rd), V(rn), (16 << es) - immhb, es, 2 if u else 1, icount(es))
        # smo-recomp addition (audio DSP in the SDK): saturating narrowing shifts. es = DESTINATION element size; the shift is
        # 2 * esize - immh:immb. U=0 0x12/0x13 SQSHRN/SQRSHRN, U=1 0x12/0x13 UQSHRN/UQRSHRN, U=1 0x10/0x11 SQSHRUN/SQRSHRUN.
        if opc in (0x12, 0x13) or (u and opc in (0x10, 0x11)):
            if es > 2:
                raise Unimpl('simd-shift')
            src_signed = 0 if (u and opc in (0x12, 0x13)) else 1
            dst_signed = 0 if u else 1
            if scalar:                                   # the scalar forms (7.3.2 SDK): one element
                return 'v_qshrn_s(%s, %s, %d, %d, %d, %d, %d);' % (V(rd), V(rn), (16 << es) - immhb, es, opc & 1, src_signed, dst_signed)
            return 'v_qshrn(%s, %s, %d, %d, %d, %d, %d, %d);' % (V(rd), V(rn), (16 << es) - immhb, es, opc & 1, src_signed, dst_signed, q)
        raise Unimpl('simd-shift')

