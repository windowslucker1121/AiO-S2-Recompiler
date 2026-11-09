// SPDX-License-Identifier: GPL-3.0-or-later
// Host half of the known-answer tests (run_ka.py, oracle/ka). The runtime-provided functions of nxrecomp/ctx.h, the guest
// memory windows (VirtualAlloc at the address the case names), the expectations the generated cases call, and one result
// line per case: PASS, FAIL, FPSR-NOT-MODELLED or SKIP. Cases from ScarletNX tests/a64_ported_cases.h (GPL-3.0-or-later,
// originally rEden's and dynarmic's A64 tests); the expectations follow ScarletNX's a64_ported_run.inc.

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <nxrecomp/ctx.h>

typedef bool (*KaFn)(Ctx *);
extern "C" void ka_run_all(void);                 // generated: gen/ka_cases.cpp

namespace {

const uint64_t KA_WINDOW = 0x4000;                // the ported runner's guest memory window
const uint64_t KA_GRAIN = 0x10000;                // VirtualAlloc's allocation granularity: a window is mapped in whole grains
const char CAT_MAP[] = "window cannot be mapped at its address on this host";

struct Map { uint64_t lo, hi; };
Map maps[64];
int nmaps;

bool case_fpcr;                                   // the case's script sets FPCR to a non-zero value
bool win_on;                                      // a MEMORY op has set the current window
uint64_t win_base;
uint64_t cur_pc;                                  // the address the last RUN went to

Ctx ka_ctx;
Ctx want;                                         // MARK's state, kept across cases as the ported runner keeps it
uint8_t want_win[KA_WINDOW];

// the result of the case that runs
char msg[4096];
size_t msg_len;
int hard_fails, soft_fails;                       // soft: an FPSR expectation, which the lifter does not model
bool skipped;
char skip_reason[300];
unsigned long long traps;                         // undefined-instruction, SVC and BRK traps in the current RUN
char last_trap[160];

#define LL(x) ((unsigned long long)(x))

void add_msg(bool soft, const char *fmt, ...) {
    char buf[320];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (soft) ++soft_fails; else ++hard_fails;
    size_t n = strlen(buf);
    if (msg_len + n + 3 >= sizeof msg) return;
    if (msg_len) { memcpy(msg + msg_len, "; ", 2); msg_len += 2; }
    memcpy(msg + msg_len, buf, n);
    msg_len += n;
    msg[msg_len] = 0;
}

bool in_window(uint64_t a, uint64_t size) {
    return win_on && a >= win_base && a - win_base < KA_WINDOW && size <= KA_WINDOW - (a - win_base);
}

uint8_t *at(uint64_t a) { return (uint8_t *)(uintptr_t)a; }

uint64_t vhi(const V128 *v) { return v->d[1]; }
uint64_t vlo(const V128 *v) { return v->d[0]; }

} // namespace

// ---------------------------------------------------------------- the generated cases call these
extern "C" {

void ka_run(const char *name, KaFn fn) {
    bool faulted = false;
    DWORD code = 0;
    memset(&ka_ctx, 0, sizeof ka_ctx);
    ka_ctx.ex_addr = ~0ull;
    msg[0] = 0;
    msg_len = 0;
    hard_fails = soft_fails = 0;
    skipped = false;
    traps = 0;
    last_trap[0] = 0;
    win_on = false;
    case_fpcr = false;
    cur_pc = 0;
    __try {
        fn(&ka_ctx);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        faulted = true;
        code = GetExceptionCode();
    }
    if (faulted)
        add_msg(false, "host fault 0x%08lx: a lifted instruction touched memory the case does not map", (unsigned long)code);
    if (skipped)
        printf("SKIP %s: %s\n", name, skip_reason);
    else if (hard_fails && case_fpcr)
        printf("FPCR-NOT-HONOURED %s: %s\n", name, msg);
    else if (hard_fails)
        printf("FAIL %s: %s\n", name, msg);
    else if (soft_fails)
        printf("FPSR-NOT-MODELLED %s: %s\n", name, msg);
    else
        printf("PASS %s\n", name);
    fflush(stdout);
}

// the case's script sets a non-zero FPCR (emitted first in such a case)
void ka_case_fpcr(void) { case_fpcr = true; }

void ka_skip(const char *name, const char *reason) {
    printf("SKIP %s: %s\n", name, reason);
    fflush(stdout);
}

void ka_script_error(const char *what) { add_msg(false, "script: %s", what); }

void ka_lifter_gap(const char *insn, const char *why) { add_msg(false, "lifter: %s: %s", insn, why); }

// Maps the window [base, base + KA_WINDOW) at its own address (rounded out to whole grains). A window that is already
// inside a mapping of this run is reused; one VirtualAlloc refuses (an address beyond the user range, or one overlapping
// another mapping) skips the case.
bool ka_map(uint64_t base, uint64_t size) {
    uint64_t lo = base & ~(KA_GRAIN - 1);
    uint64_t hi = (base + size + KA_GRAIN - 1) & ~(KA_GRAIN - 1);
    for (int i = 0; i < nmaps; ++i)
        if (lo >= maps[i].lo && hi <= maps[i].hi) return true;
    void *p = VirtualAlloc((LPVOID)(uintptr_t)lo, (SIZE_T)(hi - lo), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!p) {
        skipped = true;
        snprintf(skip_reason, sizeof skip_reason, "%s (window at 0x%llx: VirtualAlloc error %lu)", CAT_MAP, LL(base),
                 (unsigned long)GetLastError());
        return false;
    }
    if (nmaps < 64) {
        maps[nmaps].lo = lo;
        maps[nmaps].hi = hi;
        ++nmaps;
    }
    return true;
}

// MEMORY: the window starts at base; each byte is its low address byte (zeros = 0) as in the ported runner.
void ka_memory(uint64_t base, int zeros) {
    uint8_t *p = at(base);
    win_on = true;
    win_base = base;
    for (uint64_t i = 0; i < KA_WINDOW; ++i) p[i] = zeros ? 0 : (uint8_t)(base + i);
}

void ka_smem(uint64_t addr, const char *text) {
    size_t n = strlen(text) + 1;
    if (!in_window(addr, n)) {
        add_msg(false, "the script: text at 0x%llx is outside the memory window", LL(addr));
        return;
    }
    memcpy(at(addr), text, n);
}

void ka_run_begin(void) { traps = 0; }

void ka_run_end(uint64_t pc) {
    cur_pc = pc;
    if (traps) add_msg(false, "undefined-instruction, SVC or BRK traps: got %llu (last: %s), expected 0", traps, last_trap);
    traps = 0;
}

void ka_check_x(Ctx *c, unsigned n, uint64_t a, uint64_t mask) {
    uint64_t got = c->x[n] & mask;
    if (got != a) add_msg(false, "X%u & %llx is %llx, expected %llx", n, LL(mask), LL(got), LL(a));
}

// an X register compared under the QC bit after an mrs of FPSR: the lifter does not set QC, so an FPSR expectation
void ka_check_x_qc(Ctx *c, unsigned n, uint64_t a, uint64_t mask) {
    uint64_t got = c->x[n] & mask;
    if (got != a) add_msg(true, "QC via X%u & %llx is %llx, expected %llx", n, LL(mask), LL(got), LL(a));
}

// PRESET_NCE: interpreter_visitor.cpp's starting registers and memory, with the window at base (the ported runner's
// window at address 0 moved there; the addresses in the case are moved with it by run_ka.py)
void ka_preset(Ctx *c, uint64_t base) {
    uint8_t *p = at(base);
    win_on = true;
    win_base = base;
    for (uint32_t i = 0; i < KA_WINDOW; ++i) p[i] = (uint8_t)(0x80u | ((uint32_t)(i * 0x9E3779B1u) >> 25));
    for (unsigned i = 0; i < 31; ++i) c->x[i] = 0xDEAD000000000000ull | i;
    c->x[0] = base + 0x1000;
    c->x[1] = base + 0x1800;
    c->x[2] = 0x30;
    c->x[31] = base + 0x2EE0;
    for (unsigned i = 0; i < 32; ++i) {
        c->v[i].d[0] = 0x5555000000000000ull | i;
        c->v[i].d[1] = 0xAAAA000000000000ull | i;
    }
}

void ka_check_v(Ctx *c, unsigned n, uint64_t lo, uint64_t hi) {
    if (vlo(&c->v[n]) != lo || vhi(&c->v[n]) != hi)
        add_msg(false, "V%u is %016llx:%016llx (high:low), expected %016llx:%016llx", n, LL(vhi(&c->v[n])), LL(vlo(&c->v[n])),
                LL(hi), LL(lo));
}

void ka_check_pc(uint64_t a) {
    if (cur_pc != a) add_msg(false, "PC is %llx, expected %llx", LL(cur_pc), LL(a));
}

void ka_check_fpsr(Ctx *c, uint64_t a, uint64_t mask) {
    uint64_t got = c->fpsr & mask;
    if (got != a) add_msg(true, "FPSR & %llx is %llx, expected %llx", LL(mask), LL(got), LL(a));
}

void ka_check_nzcv(Ctx *c, uint64_t a) {
    uint64_t got = get_nzcv(c);
    if (got != a) add_msg(false, "NZCV is %llx, expected %llx", LL(got), LL(a));
}

void ka_check_mem64(uint64_t a, uint64_t v) {
    if (!in_window(a, 8)) {
        add_msg(false, "the 64 bits at %llx are outside the memory window", LL(a));
        return;
    }
    uint64_t got;
    memcpy(&got, at(a), 8);
    if (got != v) add_msg(false, "the 64 bits at %llx are %llx, expected %llx", LL(a), LL(got), LL(v));
}

void ka_check_mem_same(uint64_t a, uint64_t b, unsigned n) {
    if (!in_window(a, n) || !in_window(b, n)) {
        add_msg(false, "%u bytes at %llx against %llx: outside the memory window", n, LL(a), LL(b));
        return;
    }
    if (memcmp(at(a), at(b), n)) add_msg(false, "%u bytes at %llx differ from those at %llx", n, LL(a), LL(b));
}

// MARK: the state as it is now is the one expected, until the WANT steps change it.
void ka_mark(Ctx *c) {
    want = *c;
    if (win_on) memcpy(want_win, at(win_base), KA_WINDOW);
    else memset(want_win, 0, sizeof want_win);
}

void ka_want_x(unsigned n, uint64_t v) { want.x[n] = v; }

void ka_want_v(unsigned n, uint64_t lo, uint64_t hi) {
    want.v[n].d[0] = lo;
    want.v[n].d[1] = hi;
}

void ka_want_mem(uint64_t a, unsigned n, uint64_t v) {
    if (n > 8 || !in_window(a, n)) {
        add_msg(false, "the script: expectation at %llx is outside the memory window", LL(a));
        return;
    }
    memcpy(want_win + (a - win_base), &v, n);
}

// SAME: every X, SP, every V and the whole window as expected. As the ported runner, it reports the first mismatch of each.
void ka_check_same(Ctx *c) {
    bool same = true;
    for (unsigned n = 0; n < 32 && same; ++n) {
        same = c->x[n] == want.x[n];
        if (!same && n == 31) add_msg(false, "SP is %llx, expected %llx", LL(c->x[n]), LL(want.x[n]));
        else if (!same) add_msg(false, "X%u is %llx, expected %llx", n, LL(c->x[n]), LL(want.x[n]));
    }
    for (unsigned n = 0; n < 32 && same; ++n) {
        same = memcmp(&c->v[n], &want.v[n], 16) == 0;
        if (!same)
            add_msg(false, "V%u is %016llx:%016llx (high:low), expected %016llx:%016llx", n, LL(vhi(&c->v[n])), LL(vlo(&c->v[n])),
                    LL(vhi(&want.v[n])), LL(vlo(&want.v[n])));
    }
    for (uint64_t k = 0; k < KA_WINDOW && same && win_on; ++k) {
        same = at(win_base)[k] == want_win[k];
        if (!same)
            add_msg(false, "the byte at %llx is %02x, expected %02x", LL(win_base + k), at(win_base)[k], want_win[k]);
    }
}

// ---------------------------------------------------------------- the runtime-provided functions (nxrecomp/ctx.h)

void call_indirect(Ctx *c, uint64_t addr) {
    (void)c;
    add_msg(false, "call_indirect to %llx is not modelled by the known-answer tests", LL(addr));
}

void host_import(Ctx *c, uint32_t sym) {
    (void)c;
    add_msg(false, "host_import %u is not modelled by the known-answer tests", sym);
}
void nx_icall_miss(Ctx *c, uint64_t addr, NxIc *ic) { (void)ic; call_indirect(c, addr); }
void nx_iimport_miss(Ctx *c, uint32_t id, NxIc *ic) { (void)ic; host_import(c, id); }

void guest_trap(const char *what, uint64_t addr) {
    ++traps;
    snprintf(last_trap, sizeof last_trap, "%s at %llx", what, LL(addr));
}

void guest_unimpl(uint64_t pc, uint32_t insn, const char *what) {
    add_msg(false, "lifter: unimplemented %08x at %llx (%s)", insn, LL(pc), what);
}

void guest_svc(Ctx *c, uint32_t number) {
    (void)c;
    ++traps;
    snprintf(last_trap, sizeof last_trap, "svc #%u", number);
}

uint64_t guest_mrs(Ctx *c, uint32_t sysreg) {
    (void)c;
    add_msg(false, "system register %x read is not modelled by the known-answer tests", sysreg);
    return 0;
}

void guest_msr(Ctx *c, uint32_t sysreg, uint64_t value) {
    (void)c;
    (void)value;
    add_msg(false, "system register %x write is not modelled by the known-answer tests", sysreg);
}

void stx_diag(Ctx *c, uint64_t a, uint64_t expected, uint64_t seen, uint64_t v, int bits) {
    (void)c; (void)a; (void)expected; (void)seen; (void)v; (void)bits;
}

void nx_catch_call(Ctx *c, void (*body)(Ctx *, uint64_t)) { body(c, 0); }

void nx_site_inline(Ctx *c, uint64_t addr) { (void)c; (void)addr; }

int nx_site_midhook(Ctx *c, uint64_t addr) { (void)c; (void)addr; return 0; }

int nx_hook_entry(Ctx *c, uint64_t addr) { (void)c; (void)addr; return 0; }

} // extern "C"

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    ka_run_all();
    return 0;
}
