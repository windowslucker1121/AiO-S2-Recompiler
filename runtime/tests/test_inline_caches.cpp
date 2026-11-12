// SPDX-License-Identifier: GPL-3.0-or-later
// Inline caches of the lifted code (recomp/include/nxrecomp/ctx.h NX_ICALL / NX_IIMPORT, src/core/abi_glue.cpp nx_icall_miss /
// nx_iimport_miss, src/core/dispatch.cpp ic_fill): a site caches a lifted target and calls it directly; a changed target misses
// and is dispatched; a site whose target keeps changing is frozen and still dispatched right; a replaced registration empties
// every cache; HLE thunks are cached by an indirect call site only when marked direct (hle_mark_direct).
#include "nxrt/dispatch.h"
#include "nxrt/memory.h"
#include "test_framework.h"
NXTEST_GROUP(inline_caches)

using namespace nxrt;

namespace {
constexpr uint64_t kA = 0x7100a00000, kB = 0x7100a00010, kC = 0x7100a00020;
int g_a = 0, g_b = 0, g_c = 0, g_hle = 0;
void fn_a(::Ctx* c) { g_a++; c->x[0] = 1; }
void fn_b(::Ctx* c) { g_b++; c->x[0] = 2; }
void fn_c(::Ctx* c) { g_c++; c->x[0] = 3; }
void fn_a2(::Ctx* c) { c->x[0] = 11; }
void hle_fn(::Ctx* c) { g_hle++; c->x[0] = 42; }

// one call site, as the lifter emits it for BLR
NxIc g_site;
void site(::Ctx* c, uint64_t t) { nx_icall(c, t, &g_site); }
}  // namespace

NXTEST(site_caches_follows_freezes_and_is_emptied_on_replacement) {
    fn_register(kA, fn_a);
    fn_register(kB, fn_b);
    fn_register(kC, fn_c);
    ic_invalidate_all();
    g_site = NxIc{};
    ::Ctx c{};
    site(&c, kA);
    CHECK_EQ(c.x[0], 1u);
    REQUIRE(g_site.t != nullptr);
    CHECK_EQ(g_site.t->addr, kA);
    site(&c, kA);                                  // a hit: no miss counted
    CHECK_EQ(g_site.misses, 0u);
    CHECK_EQ(g_a, 2);
    site(&c, kB);                                  // another target: dispatched, cache follows
    CHECK_EQ(c.x[0], 2u);
    CHECK_EQ(g_site.t->addr, kB);
    for (int i = 0; i < 20; i++) site(&c, (i & 1) ? kB : kC);   // polymorphic: frozen after a few refills, still right
    CHECK_EQ(g_site.state, 2u);
    CHECK_EQ(g_b + g_c, 21);
    site(&c, kC);
    CHECK_EQ(c.x[0], 3u);
    // a replaced registration empties every cache, frozen ones included
    fn_register(kA, fn_a2);
    CHECK(g_site.t == nullptr);
    CHECK_EQ(g_site.state, 0u);
    site(&c, kA);
    CHECK_EQ(c.x[0], 11u);
    CHECK_EQ(g_site.t->addr, kA);
    fn_register(kA, fn_a);
}

NXTEST(hle_thunks_are_not_cached_by_call_sites) {
    const uint64_t h = hle_register("ic_test_hle", hle_fn);
    REQUIRE(h != 0);
    NxIc ic{};
    ::Ctx c{};
    nx_icall(&c, h, &ic);
    CHECK_EQ(c.x[0], 42u);
    CHECK(ic.t == nullptr);
    nx_icall(&c, h, &ic);
    CHECK_EQ(g_hle, 2);
}

namespace {
int g_direct = 0, g_direct2 = 0;
void direct_fn(::Ctx* c) { g_direct++; c->x[0] = 7; }
void direct_fn2(::Ctx* c) { g_direct2++; c->x[0] = 8; }
}  // namespace

// A thunk marked direct (an implemented NVN entry point) is cached by the call site; a new handler for it empties the caches and
// is not cached until it is marked again.
NXTEST(direct_hle_thunks_are_cached_and_dropped_when_replaced) {
    const uint64_t h = hle_register("ic_test_direct", direct_fn);
    REQUIRE(h != 0);
    hle_mark_direct("ic_test_direct");
    NxIc ic{};
    ::Ctx c{};
    nx_icall(&c, h, &ic);
    CHECK_EQ(c.x[0], 7u);
    REQUIRE(ic.t != nullptr);
    CHECK_EQ(ic.t->addr, h);
    nx_icall(&c, h, &ic);
    CHECK_EQ(g_direct, 2);
    CHECK_EQ(hle_register("ic_test_direct", direct_fn2), h);
    CHECK(ic.t == nullptr);
    nx_icall(&c, h, &ic);
    CHECK_EQ(c.x[0], 8u);
    CHECK(ic.t == nullptr);                        // not marked again: dispatched
    hle_mark_direct("ic_test_direct");
    nx_icall(&c, h, &ic);
    REQUIRE(ic.t != nullptr);
    nx_icall(&c, h, &ic);
    CHECK_EQ(g_direct2, 3);
}
