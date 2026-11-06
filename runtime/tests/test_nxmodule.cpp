// SPDX-License-Identifier: GPL-3.0-or-later
// Lifted-module registry (nxmodule.h): descriptor validation and refusal, duplicate index and range checks, import resolution through
// NX_IMPORT_ID(module index, symbol), the export / override / HLE order, the logging stub, the resolution cache and the import
// bookkeeping. No ELF is mapped: the descriptors are hand-built and their own [base, text_end) range is trusted (image == nullptr).
#include <cstring>

#include "nxrt/dispatch.h"
#include "nxrt/module.h"
#include "nxrt/nxmodule.h"
#include "test_framework.h"
NXTEST_GROUP(nxmodule)

using nxrt::RegStatus;

namespace {

constexpr uint64_t kA = 0x7200000000ull;   // module A
constexpr uint64_t kB = 0x7210000000ull;   // module B
constexpr uint64_t kSpan = 0x1000;         // text_end = base + kSpan

// Lifted stand-ins: each writes a marker into x0.
void fa(::Ctx* c) { c->x[0] = 0xA1; }
void fb(::Ctx* c) { c->x[0] = 0xB1; }
void fc(::Ctx* c) { c->x[0] = 0xC1; }

// HLE handlers.
void h_alpha(::Ctx* c) { c->x[0] = 0x11; }
void h_beta(::Ctx* c) { c->x[0] = 0x22; }
void h_beta_b(::Ctx* c) { c->x[0] = 0x33; }
void h_shared(::Ctx* c) { c->x[0] = 0x77; }

void pad_fn(::Ctx* c, uint64_t pad) { c->x[0] = pad; }

// What the probe handler saw of the import bookkeeping.
const char* g_seen_in_import = nullptr;
uint64_t g_seen_from = 0;
void h_probe(::Ctx* c) {
    g_seen_in_import = c->in_import;
    g_seen_from = c->import_from;
    c->x[0] = 1;
}

// Every test starts and ends with a clean registry and HLE table.
void reset() {
    nxrt::modules_unload_all();
    nxrt::hle_clear();
}
struct Reset {
    Reset() { reset(); }
    ~Reset() { reset(); }
};

// Module A (index 0, function fa, one import "shared") and module B (index 1, function fb, export "shared", plus a
// second exported symbol "rel_sym" at kB + 0x200; ABI 3: every DynSym.value is a guest address). Used by the export and override tests.
void register_shared_pair() {
    static const FnEntry a_fns[] = {{kA + 0x100, fa}};
    static const DynSym a_syms[] = {{"shared", 0, 0}};
    static const NxModule a = {"mod_a", 0u, kA, kA + kSpan, a_fns, 1, a_syms, 1, nullptr, 0, nullptr};
    static const FnEntry b_fns[] = {{kB + 0x100, fb}};
    static const DynSym b_syms[] = {{"shared", kB + 0x100, 1}, {"rel_sym", kB + 0x200, 1}};
    static const NxModule b = {"mod_b", 1u, kB, kB + kSpan, b_fns, 1, b_syms, 2, nullptr, 0, nullptr};
    CHECK(nxrt::nxmodule_register(nullptr, &a, nxrt::kAbiVersion).ok());
    CHECK(nxrt::nxmodule_register(nullptr, &b, nxrt::kAbiVersion).ok());
}

// A descriptor that must be refused with `want`, leaving nothing registered.
void expect_refused(const char* what, const NxModule* nx, RegStatus want) {
    nxrt::RegResult r = nxrt::nxmodule_register(nullptr, nx, nxrt::kAbiVersion);
    CHECK_MSG(r.status == want, what);
    CHECK_MSG(!r.ok(), what);
    CHECK_MSG(r.functions == 0, what);
    CHECK_MSG(nxrt::nxmodules().empty(), what);
}

}  // namespace

NXTEST(register_ok_and_call) {
    Reset guard;
    static const FnEntry fns[] = {{kA + 0x100, fa}, {kA + 0x200, fb}, {kA + 0x300, fc}};
    static const NxModule nx = {"alpha_mod", 0u, kA, kA + kSpan, fns, 3, nullptr, 0, nullptr, 0, nullptr};

    nxrt::RegResult r = nxrt::nxmodule_register(nullptr, &nx, nxrt::kAbiVersion);
    CHECK(r.ok());
    CHECK_EQ(r.status, RegStatus::Ok);
    CHECK_EQ(r.functions, 3u);

    // the functions are callable through the call table and visible to fn_lookup
    CHECK(nxrt::fn_lookup(kA + 0x100) == fa);
    CHECK(nxrt::fn_lookup(kA + 0x300) == fc);
    ::Ctx c{};
    call_indirect(&c, kA + 0x200);
    CHECK_EQ(c.x[0], 0xB1u);
    ::Ctx c2{};
    call_indirect(&c2, kA + 0x100);
    CHECK_EQ(c2.x[0], 0xA1u);

    // lookups
    CHECK(nxrt::nxmodule_by_index(0) == &nx);
    CHECK(nxrt::nxmodule_by_name("alpha_mod") == &nx);
    CHECK(nxrt::nxmodule_at(kA + 0x100) == &nx);
    CHECK(nxrt::nxmodule_at(kB) == nullptr);
    auto all = nxrt::nxmodules();
    CHECK_EQ(all.size(), 1u);
    CHECK(all.size() == 1 && all[0] == &nx);
}

NXTEST(abi_mismatch_refused) {
    Reset guard;
    static const FnEntry fns[] = {{kA + 0x100, fa}};
    static const NxModule nx = {"abi_mod", 0u, kA, kA + kSpan, fns, 1, nullptr, 0, nullptr, 0, nullptr};

    for (uint32_t v : {2u, 4u, 0u}) {
        nxrt::RegResult r = nxrt::nxmodule_register(nullptr, &nx, v);
        CHECK_EQ(r.status, RegStatus::AbiMismatch);
        CHECK(!r.ok());
        CHECK_EQ(r.functions, 0u);
        CHECK(nxrt::fn_lookup(kA + 0x100) == nullptr);
        CHECK(nxrt::nxmodule_by_index(0) == nullptr);
        CHECK(nxrt::nxmodules().empty());
    }

    // the same descriptor registers with the runtime's own ABI version
    nxrt::RegResult ok = nxrt::nxmodule_register(nullptr, &nx, nxrt::kAbiVersion);
    CHECK(ok.ok());
    CHECK_EQ(ok.functions, 1u);
    CHECK(nxrt::nxmodule_by_index(0) == &nx);
    reset();

    // the old API always uses kAbiVersion: it registers and returns the function count, and 0 for a refused descriptor
    CHECK_EQ(nxrt::module_register_nxmodule(nullptr, &nx), 1u);
    CHECK(nxrt::nxmodule_by_index(0) == &nx);
    reset();
    CHECK_EQ(nxrt::module_register_nxmodule(nullptr, nullptr), 0u);
}

NXTEST(macro_registers) {
    Reset guard;
    static const FnEntry fns[] = {{kA + 0x100, fa}, {kA + 0x200, fb}};
    static const NxModule nx = {"macro_mod", 0u, kA, kA + kSpan, fns, 2, nullptr, 0, nullptr, 0, nullptr};

    nxrt::RegResult r = NXRT_REGISTER_NXMODULE(nullptr, nx);
    CHECK(r.ok());
    CHECK_EQ(r.functions, 2u);
    CHECK(nxrt::nxmodule_by_name("macro_mod") == &nx);
}

NXTEST(duplicate_index_refused) {
    Reset guard;
    static const FnEntry first_fns[] = {{kA + 0x100, fa}};
    static const NxModule first = {"first", 7u, kA, kA + kSpan, first_fns, 1, nullptr, 0, nullptr, 0, nullptr};
    static const FnEntry second_fns[] = {{kB + 0x100, fb}};
    static const NxModule second = {"second", 7u, kB, kB + kSpan, second_fns, 1, nullptr, 0, nullptr, 0, nullptr};

    CHECK(nxrt::nxmodule_register(nullptr, &first, nxrt::kAbiVersion).ok());
    nxrt::RegResult r = nxrt::nxmodule_register(nullptr, &second, nxrt::kAbiVersion);
    CHECK_EQ(r.status, RegStatus::DuplicateIndex);
    CHECK(!r.ok());
    CHECK_EQ(r.functions, 0u);

    // the first module is intact
    CHECK(nxrt::nxmodule_by_index(7) == &first);
    CHECK(nxrt::nxmodule_by_name("first") == &first);
    CHECK(nxrt::nxmodule_by_name("second") == nullptr);
    CHECK_EQ(nxrt::nxmodules().size(), 1u);
    CHECK(nxrt::fn_lookup(kA + 0x100) == fa);
    CHECK(nxrt::fn_lookup(kB + 0x100) == nullptr);
}

NXTEST(overlapping_range_refused) {
    Reset guard;
    static const FnEntry first_fns[] = {{kA + 0x100, fa}};
    static const NxModule first = {"first", 1u, kA, kA + kSpan, first_fns, 1, nullptr, 0, nullptr, 0, nullptr};
    static const FnEntry over_fns[] = {{kA + 0x800, fb}};
    static const NxModule over = {"overlap", 2u, kA + 0x800, kA + 0x2000, over_fns, 1, nullptr, 0, nullptr, 0, nullptr};

    CHECK(nxrt::nxmodule_register(nullptr, &first, nxrt::kAbiVersion).ok());
    nxrt::RegResult r = nxrt::nxmodule_register(nullptr, &over, nxrt::kAbiVersion);
    CHECK_EQ(r.status, RegStatus::DuplicateRange);
    CHECK(!r.ok());
    CHECK_EQ(r.functions, 0u);
    CHECK(nxrt::nxmodule_by_index(2) == nullptr);
    CHECK(nxrt::fn_lookup(kA + 0x800) == nullptr);

    // a range that only touches the first one (starts at its text_end) does not overlap
    static const FnEntry next_fns[] = {{kA + kSpan + 0x100, fc}};
    static const NxModule next = {"next", 3u, kA + kSpan, kA + 2 * kSpan, next_fns, 1, nullptr, 0, nullptr, 0, nullptr};
    CHECK(nxrt::nxmodule_register(nullptr, &next, nxrt::kAbiVersion).ok());
}

NXTEST(invalid_descriptors_refused) {
    Reset guard;
    static const FnEntry one_fn[] = {{kA + 0x100, fa}};
    static const FnEntry unsorted[] = {{kA + 0x200, fa}, {kA + 0x100, fb}};
    static const FnEntry out_of_range[] = {{kA + kSpan, fa}};          // == text_end: outside [base, text_end)
    static const FnEntry null_fn[] = {{kA + 0x100, nullptr}};

    static const NxModule null_name = {nullptr, 0u, kA, kA + kSpan, one_fn, 1, nullptr, 0, nullptr, 0, nullptr};
    static const NxModule end_below_base = {"end_below_base", 0u, kA + kSpan, kA, one_fn, 1, nullptr, 0, nullptr, 0, nullptr};
    static const NxModule neg_fns = {"neg_fns", 0u, kA, kA + kSpan, one_fn, -1, nullptr, 0, nullptr, 0, nullptr};
    static const NxModule unsorted_m = {"unsorted", 0u, kA, kA + kSpan, unsorted, 2, nullptr, 0, nullptr, 0, nullptr};
    static const NxModule outside_m = {"outside", 0u, kA, kA + kSpan, out_of_range, 1, nullptr, 0, nullptr, 0, nullptr};
    static const NxModule null_fn_m = {"null_fn", 0u, kA, kA + kSpan, null_fn, 1, nullptr, 0, nullptr, 0, nullptr};
    static const NxModule null_table = {"null_table", 0u, kA, kA + kSpan, nullptr, 2, nullptr, 0, nullptr, 0, nullptr};
    static const NxModule big_index = {"big_index", 4096u, kA, kA + kSpan, one_fn, 1, nullptr, 0, nullptr, 0, nullptr};

    expect_refused("null name", &null_name, RegStatus::Invalid);
    expect_refused("text_end below base", &end_below_base, RegStatus::Invalid);
    expect_refused("negative function count", &neg_fns, RegStatus::Invalid);
    expect_refused("function table not sorted", &unsorted_m, RegStatus::Invalid);
    expect_refused("function address outside the range", &outside_m, RegStatus::Invalid);
    expect_refused("null function pointer", &null_fn_m, RegStatus::Invalid);
    expect_refused("null table with a non-zero length", &null_table, RegStatus::Invalid);
    expect_refused("module index 4096", &big_index, RegStatus::Invalid);
    expect_refused("null descriptor", nullptr, RegStatus::Invalid);
}

NXTEST(import_by_module_index_and_symbol) {
    Reset guard;
    // module A (index 0): symbols 0 "alpha" and 1 "beta", both imports
    static const FnEntry a_fns[] = {{kA + 0x100, fa}};
    static const DynSym a_syms[] = {{"alpha", 0, 0}, {"beta", 0, 0}};
    static const NxModule a = {"mod_a", 0u, kA, kA + kSpan, a_fns, 1, a_syms, 2, nullptr, 0, nullptr};
    // module B (index 1): symbol 0 "beta_b", an import
    static const FnEntry b_fns[] = {{kB + 0x100, fb}};
    static const DynSym b_syms[] = {{"beta_b", 0, 0}};
    static const NxModule b = {"mod_b", 1u, kB, kB + kSpan, b_fns, 1, b_syms, 1, nullptr, 0, nullptr};
    CHECK(nxrt::nxmodule_register(nullptr, &a, nxrt::kAbiVersion).ok());
    CHECK(nxrt::nxmodule_register(nullptr, &b, nxrt::kAbiVersion).ok());

    nxrt::hle_register("alpha", h_alpha);
    nxrt::hle_register("beta", h_beta);
    nxrt::hle_register("beta_b", h_beta_b);

    ::Ctx c{};
    c.x[0] = 0;
    host_import(&c, NX_IMPORT_ID(0, 0));
    CHECK_EQ(c.x[0], 0x11u);
    c.x[0] = 0;
    host_import(&c, NX_IMPORT_ID(0, 1));
    CHECK_EQ(c.x[0], 0x22u);
    c.x[0] = 0;
    host_import(&c, NX_IMPORT_ID(1, 0));   // symbol 0 of module 1, not symbol 0 of module 0
    CHECK_EQ(c.x[0], 0x33u);

    CHECK(std::strcmp(nxrt::import_name(NX_IMPORT_ID(0, 0)), "alpha") == 0);
    CHECK(std::strcmp(nxrt::import_name(NX_IMPORT_ID(0, 1)), "beta") == 0);
    CHECK(std::strcmp(nxrt::import_name(NX_IMPORT_ID(1, 0)), "beta_b") == 0);
    CHECK(std::strcmp(nxrt::import_name(NX_IMPORT_ID(7, 0)), "") == 0);      // module not registered
    CHECK(std::strcmp(nxrt::import_name(NX_IMPORT_ID(0, 5)), "") == 0);      // symbol out of range
    CHECK(std::strcmp(nxrt::import_name(NX_IMPORT_ID(1, 1)), "") == 0);      // symbol out of range for module 1
}

NXTEST(import_resolves_to_other_module_export) {
    Reset guard;
    register_shared_pair();
    nxrt::hle_register("shared", h_shared);
    const NxModule* b = nxrt::nxmodule_by_index(1);
    CHECK(b != nullptr);

    // module A's import "shared" runs module B's lifted function, not the HLE handler of that name
    ::Ctx c{};
    c.x[0] = 0;
    host_import(&c, NX_IMPORT_ID(0, 0));
    CHECK_EQ(c.x[0], 0xB1u);

    CHECK(nxrt::nxmodule_find_export("shared") == kB + 0x100);
    CHECK(nxrt::nxmodule_find_export("shared", b) == 0);
    CHECK(nxrt::nxmodule_find_export("no_such_symbol") == 0);

    // absolute and module-relative symbol values
    CHECK(nxrt::nxmodule_symbol_address(b, 0) == kB + 0x100);
    CHECK(nxrt::nxmodule_symbol_address(b, 1) == kB + 0x200);   // value 0x200 < base: rebased
    CHECK(nxrt::nxmodule_symbol_address(nxrt::nxmodule_by_index(0), 0) == 0);   // an import is not defined
    CHECK(nxrt::nxmodule_symbol_address(b, 2) == 0);            // out of range
}

NXTEST(override_symbol_beats_module_export) {
    Reset guard;
    register_shared_pair();
    nxrt::hle_register("shared", h_shared);
    nxrt::module_override_symbol("shared");   // set before the first resolution of the import

    ::Ctx c{};
    c.x[0] = 0;
    host_import(&c, NX_IMPORT_ID(0, 0));
    CHECK_EQ(c.x[0], 0x77u);
}

NXTEST(override_after_first_resolution_invalidates_cache) {
    Reset guard;
    register_shared_pair();
    nxrt::hle_register("shared", h_shared);

    ::Ctx c{};
    host_import(&c, NX_IMPORT_ID(0, 0));       // resolves to module B's function and caches it
    CHECK(c.x[0] != 0x77u);
    nxrt::module_override_symbol("shared");    // found by the first version of this test: the cache kept the old target
    c.x[0] = 0;
    host_import(&c, NX_IMPORT_ID(0, 0));
    CHECK_EQ(c.x[0], 0x77u);
}

NXTEST(unknown_import_falls_to_logging_stub) {
    Reset guard;
    static const FnEntry a_fns[] = {{kA + 0x100, fa}};
    static const DynSym a_syms[] = {{"nobody_defines_this", 0, 0}};
    static const NxModule a = {"mod_a", 0u, kA, kA + kSpan, a_fns, 1, a_syms, 1, nullptr, 0, nullptr};
    CHECK(nxrt::nxmodule_register(nullptr, &a, nxrt::kAbiVersion).ok());

    uint64_t t = nxrt::import_resolve(NX_IMPORT_ID(0, 0));
    CHECK(t != 0);
    CHECK(nxrt::hle_is_thunk(t));
    CHECK(nxrt::hle_stub_count() >= 1);

    ::Ctx c{};
    c.x[0] = 5;
    host_import(&c, NX_IMPORT_ID(0, 0));   // returns, with x0 = 0 from the stub
    CHECK_EQ(c.x[0], 0u);
    CHECK_EQ(nxrt::hle_calls(t), 1u);
}

NXTEST(import_resolve_invalid_ids_return_zero) {
    Reset guard;
    static const FnEntry a_fns[] = {{kA + 0x100, fa}};
    static const DynSym a_syms[] = {{"alpha", 0, 0}};
    static const NxModule a = {"mod_a", 0u, kA, kA + kSpan, a_fns, 1, a_syms, 1, nullptr, 0, nullptr};
    CHECK(nxrt::nxmodule_register(nullptr, &a, nxrt::kAbiVersion).ok());

    CHECK_EQ(nxrt::import_resolve(NX_IMPORT_ID(0, 1)), 0u);    // symbol index out of range
    CHECK_EQ(nxrt::import_resolve(NX_IMPORT_ID(9, 0)), 0u);    // module index not registered
    CHECK(nxrt::import_resolve(NX_IMPORT_ID(0, 0)) != 0);   // the valid id still resolves
}

NXTEST(resolution_cache_invalidated_by_registration) {
    Reset guard;
    static const FnEntry a_fns[] = {{kA + 0x100, fa}};
    static const DynSym a_syms[] = {{"late", 0, 0}};
    static const NxModule a = {"mod_a", 0u, kA, kA + kSpan, a_fns, 1, a_syms, 1, nullptr, 0, nullptr};
    CHECK(nxrt::nxmodule_register(nullptr, &a, nxrt::kAbiVersion).ok());

    // nothing defines "late" yet: the import resolves to a logging stub thunk
    uint64_t before = nxrt::import_resolve(NX_IMPORT_ID(0, 0));
    CHECK(before != 0);
    CHECK(nxrt::hle_is_thunk(before));

    // module B defines "late" after the first resolution: the cached answer must be replaced
    static const FnEntry b_fns[] = {{kB + 0x100, fb}};
    static const DynSym b_syms[] = {{"late", kB + 0x100, 1}};
    static const NxModule b = {"mod_b", 1u, kB, kB + kSpan, b_fns, 1, b_syms, 1, nullptr, 0, nullptr};
    CHECK(nxrt::nxmodule_register(nullptr, &b, nxrt::kAbiVersion).ok());

    CHECK(nxrt::import_resolve(NX_IMPORT_ID(0, 0)) == kB + 0x100);
    ::Ctx c{};
    c.x[0] = 0;
    host_import(&c, NX_IMPORT_ID(0, 0));
    CHECK_EQ(c.x[0], 0xB1u);
}

NXTEST(import_counts) {
    Reset guard;
    static const FnEntry a_fns[] = {{kA + 0x100, fa}};
    static const DynSym a_syms[] = {{"alpha", 0, 0}};
    static const NxModule a = {"mod_a", 0u, kA, kA + kSpan, a_fns, 1, a_syms, 1, nullptr, 0, nullptr};
    CHECK(nxrt::nxmodule_register(nullptr, &a, nxrt::kAbiVersion).ok());
    nxrt::hle_register("alpha", h_alpha);

    // Each thread adds its calls to the shared count in steps of 4096 (nxmodule.cpp count_import: one shared atomic add per call cost a boot
    // thread 11%), so a few calls may not show yet, and 4096 calls on one thread add exactly 4096 whatever that thread counted before.
    CHECK_EQ(nxrt::import_calls(), 0u);
    ::Ctx c{};
    for (int i = 0; i < 3; i++) host_import(&c, NX_IMPORT_ID(0, 0));
    const uint64_t after3 = nxrt::import_calls();
    CHECK(after3 == 0u || after3 == 4096u);
    for (int i = 0; i < 4096; i++) host_import(&c, NX_IMPORT_ID(0, 0));
    CHECK_EQ(nxrt::import_calls(), after3 + 4096u);
    CHECK_EQ(c.x[0], 0x11u);   // the calls reached the handler

    reset();
    CHECK_EQ(nxrt::import_calls(), 0u);
    CHECK(nxrt::nxmodules().empty());
    CHECK(nxrt::nxmodule_by_index(0) == nullptr);
}

NXTEST(host_import_restores_in_import) {
    Reset guard;
    static const FnEntry a_fns[] = {{kA + 0x100, fa}};
    static const DynSym a_syms[] = {{"probe", 0, 0}};
    static const NxModule a = {"mod_a", 0u, kA, kA + kSpan, a_fns, 1, a_syms, 1, nullptr, 0, nullptr};
    CHECK(nxrt::nxmodule_register(nullptr, &a, nxrt::kAbiVersion).ok());
    nxrt::hle_register("probe", h_probe);

    ::Ctx c{};
    c.in_import = "outer";
    c.import_from = 0x1234;
    c.x[30] = 0x5008;   // the import's return address: import_from is set to x30 - 4
    g_seen_in_import = nullptr;
    g_seen_from = 0;
    host_import(&c, NX_IMPORT_ID(0, 0));

    CHECK(g_seen_in_import != nullptr && std::strcmp(g_seen_in_import, "probe") == 0);
    CHECK_EQ(g_seen_from, 0x5004u);
    CHECK(std::strcmp(c.in_import, "outer") == 0);
    CHECK_EQ(c.import_from, 0x1234u);
}

NXTEST(resume_and_eh_pointers_kept) {
    Reset guard;
    static const FnEntry fns[] = {{kA + 0x100, fa}};
    static const NxResume resume[] = {{kA + 0x110, pad_fn}, {kA + 0x180, pad_fn}};
    static const NxModule nx = {"resume_mod", 3u, kA, kA + kSpan, fns, 1, nullptr, 0, resume, 2, nullptr};
    CHECK(nxrt::nxmodule_register(nullptr, &nx, nxrt::kAbiVersion).ok());

    const NxModule* got = nxrt::nxmodule_by_index(3);
    CHECK(got == &nx);
    CHECK(got->resume == resume);
    CHECK_EQ(got->nresume, 2);
    CHECK(got->resume[1].pad == kA + 0x180);
    CHECK(got->eh == nullptr);
}
