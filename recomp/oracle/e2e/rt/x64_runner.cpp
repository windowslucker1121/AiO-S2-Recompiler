// SPDX-License-Identifier: GPL-3.0-or-later
// The x64 half of the end-to-end oracle: maps a static AArch64 test ELF at its own addresses (guest pointer == host
// pointer), runs the LIFTED test_main on the same initial state unicorn_side.py gives Unicorn, and dumps every
// register and every writable region for comparison.
//
//   x64_runner <prog.elf> <out-prefix>      writes <out-prefix>.txt (registers) and <out-prefix>.bin (regions, in order)
//
// The lifted code is linked in: funcs.h / module_main.cpp from `python -m nxrecomp lift`. What the ABI leaves to the
// runtime (nxrecomp/ctx.h "runtime-provided") is implemented here in the smallest way that serves the tests.
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <string>
#include <nxrecomp/ctx.h>
#include "eh_rt.h"

extern "C" {
extern const NxModule nxmod_main;
}

static const uint64_t STACK = 0x7108000000ull, STACK_SIZE = 0x100000;
static const uint64_t TLS = 0x7109000000ull, TLSRO = 0x7109100000ull, TLS_SIZE = 0x10000;

static void pattern(uint8_t *out, size_t n, uint64_t seed) {       // same generator as unicorn_side.pattern()
    uint64_t s = seed;
    for (size_t i = 0; i < n; i += 8) {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        uint64_t v = s ^ (s >> 29);
        memcpy(out + i, &v, 8);
    }
}

static void *map_fixed(uint64_t addr, uint64_t size) {
    void *p = VirtualAlloc((void *)(uintptr_t)addr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (p != (void *)(uintptr_t)addr) { fprintf(stderr, "cannot map %llx..%llx (error %lu)\n", (unsigned long long)addr, (unsigned long long)(addr + size), GetLastError()); exit(2); }
    return p;
}

struct Region { uint64_t addr, size; };

extern "C" {
void call_indirect(Ctx *c, uint64_t addr) {
    if (eh_host_call(c, addr)) return;
    const NxModule *m = &nxmod_main;
    int lo = 0, hi = m->nfns - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (m->fns[mid].addr == addr) { m->fns[mid].fn(c); return; }
        if (m->fns[mid].addr < addr) lo = mid + 1; else hi = mid - 1;
    }
    fprintf(stderr, "call_indirect to %llx: not a lifted function\n", (unsigned long long)addr);
    exit(3);
}
void host_import(Ctx *c, uint32_t sym) { (void)c; fprintf(stderr, "host_import %u\n", sym); exit(3); }
void nx_icall_miss(Ctx *c, uint64_t addr, NxIc *ic) { (void)ic; call_indirect(c, addr); }
void nx_iimport_miss(Ctx *c, uint32_t id, NxIc *ic) { (void)ic; host_import(c, id); }
void guest_trap(const char *what, uint64_t addr) { fprintf(stderr, "guest_trap: %s at %llx\n", what, (unsigned long long)addr); exit(10); }
void guest_unimpl(uint64_t pc, uint32_t insn, const char *what) { fprintf(stderr, "guest_unimpl at %llx: %08x %s\n", (unsigned long long)pc, insn, what); exit(11); }
void guest_svc(Ctx *c, uint32_t n) { (void)c; fprintf(stderr, "guest_svc %u\n", n); exit(12); }
uint64_t guest_mrs(Ctx *c, uint32_t r) { (void)c; fprintf(stderr, "guest_mrs %x\n", r); exit(13); }
void guest_msr(Ctx *c, uint32_t r, uint64_t v) { (void)c; (void)v; fprintf(stderr, "guest_msr %x\n", r); exit(13); }
void stx_diag(Ctx *c, uint64_t a, uint64_t e, uint64_t s, uint64_t v, int b) { (void)c; (void)a; (void)e; (void)s; (void)v; (void)b; }
}

// Site callouts (nxrecomp/ctx.h): counted per address and printed, so a test can compare them with the number of times
// Unicorn executed that address. They never take over a function (return 0).
#include <map>
static std::map<uint64_t, uint64_t> g_hits;
extern "C" {
void nx_site_inline(Ctx *c, uint64_t addr) { (void)c; g_hits[addr]++; }
int nx_site_midhook(Ctx *c, uint64_t addr) { (void)c; g_hits[addr]++; return 0; }
int nx_hook_entry(Ctx *c, uint64_t addr) { (void)c; g_hits[addr]++; return 0; }
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: x64_runner prog.elf out-prefix\n"); return 1; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> elf((size_t)fsz);
    if (fread(elf.data(), 1, elf.size(), f) != elf.size()) return 1;
    fclose(f);
    uint64_t entry = *(uint64_t *)&elf[0x18], phoff = *(uint64_t *)&elf[0x20];
    uint16_t phentsize = *(uint16_t *)&elf[0x36], phnum = *(uint16_t *)&elf[0x38];
    uint64_t lo = ~0ull, top_file = 0, top_mem = 0;
    struct Ph { uint64_t off, vaddr, filesz, memsz; uint32_t flags; };
    std::vector<Ph> loads;
    for (int i = 0; i < phnum; i++) {
        uint8_t *p = &elf[phoff + (uint64_t)i * phentsize];
        if (*(uint32_t *)p != 1) continue;
        Ph h = { *(uint64_t *)(p + 8), *(uint64_t *)(p + 16), *(uint64_t *)(p + 32), *(uint64_t *)(p + 40), *(uint32_t *)(p + 4) };
        loads.push_back(h);
        if (h.vaddr < lo) lo = h.vaddr;
        if (h.vaddr + h.filesz > top_file) top_file = h.vaddr + h.filesz;
        if (h.vaddr + h.memsz > top_mem) top_mem = h.vaddr + h.memsz;
    }
    lo -= lo % 0x1000;
    uint64_t end = (top_mem + 0xfff) & ~0xfffull;
    map_fixed(lo, end - lo);
    for (auto &h : loads) memcpy((void *)(uintptr_t)h.vaddr, &elf[h.off], h.filesz);
    map_fixed(STACK, STACK_SIZE);
    map_fixed(TLS, TLS_SIZE);
    map_fixed(TLSRO, TLS_SIZE);
    pattern((uint8_t *)(uintptr_t)TLS, TLS_SIZE, 2);
    pattern((uint8_t *)(uintptr_t)TLSRO, TLS_SIZE, 3);

    // NX_POKE=addr:word[,addr:word...] rewrites instruction words in guest memory before the run (the dynimm site test)
    if (const char *poke = getenv("NX_POKE")) {
        std::string spec = poke;
        size_t p0 = 0;
        while (p0 < spec.size()) {
            size_t e = spec.find(',', p0);
            if (e == std::string::npos) e = spec.size();
            std::string item = spec.substr(p0, e - p0);
            size_t colon = item.find(':');
            uint64_t a = strtoull(item.c_str(), nullptr, 16);
            uint32_t w = (uint32_t)strtoull(item.c_str() + colon + 1, nullptr, 16);
            memcpy((void *)(uintptr_t)a, &w, 4);
            p0 = e + 1;
        }
    }
    std::vector<Region> regions;
    for (auto &h : loads) if (h.flags & 2) regions.push_back({ h.vaddr, h.memsz });
    regions.push_back({ STACK, STACK_SIZE });
    regions.push_back({ TLS, TLS_SIZE });
    regions.push_back({ TLSRO, TLS_SIZE });

    static Ctx c;
    memset(&c, 0, sizeof c);
    c.ex_addr = ~0ull;
    c.x[31] = STACK + STACK_SIZE - 0x100;
    c.x[30] = 0x7107ff0000ull;
    c.tpidr = TLS;
    c.tpidrro = TLSRO;
    eh_rt_init(&nxmod_main);
    void (*fn)(Ctx *) = nullptr;
    for (int i = 0; i < nxmod_main.nfns; i++) if (nxmod_main.fns[i].addr == entry) fn = nxmod_main.fns[i].fn;
    if (!fn) { fprintf(stderr, "entry %llx is not a lifted function\n", (unsigned long long)entry); return 4; }
    fn(&c);

    std::string base = argv[2];
    FILE *t = fopen((base + ".txt").c_str(), "wb");
    for (int i = 0; i < 31; i++) fprintf(t, "x%d %llx\n", i, (unsigned long long)c.x[i]);
    fprintf(t, "sp %llx\n", (unsigned long long)c.x[31]);
    for (int i = 0; i < 32; i++) fprintf(t, "v%d %016llx%016llx\n", i, (unsigned long long)c.v[i].d[1], (unsigned long long)c.v[i].d[0]);
    fprintf(t, "nzcv %x\n", (c.N << 3) | (c.Z << 2) | (c.C << 1) | c.V);
    fprintf(t, "tpidr %llx\ntpidrro %llx\n", (unsigned long long)c.tpidr, (unsigned long long)c.tpidrro);
    for (auto &h : g_hits) fprintf(t, "hit %llx %llu\n",(unsigned long long)h.first, (unsigned long long)h.second);
    fclose(t);
    FILE *b = fopen((base + ".bin").c_str(), "wb");
    for (auto &r : regions) fwrite((void *)(uintptr_t)r.addr, 1, r.size, b);
    fclose(b);
    return 0;
}
