// SPDX-License-Identifier: GPL-3.0-or-later
// The C++ unwinder and nx_catch_call without the lifter: a hand-built NxModule (unwind rows, call sites, actions, types) describes three
// functions of a pretend program, host code plays their guest bodies, and the unwinder walks a guest stack made of host memory.
//   Fmain  [B+0x100, B+0x200)  has a try block around the call at B+0x140 (pad B+0x1a0: catch type A in slot 1, catch(...) in slot 2)
//                              and is entered through nx_catch_call
//   Fthrow [B+0x200, B+0x300)  throws: it calls the runtime from B+0x210; saves x20 and d8 in its frame
//   Fmid   [B+0x300, B+0x400)  has a cleanup pad at B+0x380 (resumed through NxResume), covering the call at B+0x340
// Guest frame layouts (AArch64 style): Fmain: CFA = sp + 32, x30 at CFA-8; Fthrow: CFA = sp + 32, x30 at CFA-8, x20 at CFA-16, d8 at CFA-24;
// Fmid: CFA = sp + 16, x30 at CFA-8.
#include <cstring>
#include <string>

#include "nxrt/dispatch.h"
#include "nxrt/eh.h"
#include "nxrt/hooks.h"
#include "nxrt/log.h"
#include "nxrt/module.h"
#include "nxrt/nxmodule.h"
#include "test_framework.h"
NXTEST_GROUP(eh)

using namespace nxrt;

namespace {

constexpr uint64_t B = 0x7230000000ull;
constexpr uint64_t kPadMain = B + 0x1a0, kPadMid = B + 0x380;
constexpr uint64_t kPadJmp = B + 0x154;   // Fmain: the return address of a `bl setjmp` (the lifter makes it a landing pad)

// typeinfos: kind Unknown (no vtables configured), identity by name
struct Ti { const void* vptr; const char* name; };
Ti g_ti_a = {nullptr, "1A"}, g_ti_b = {nullptr, "1B"}, g_ti_c = {nullptr, "1C"};
const uint64_t kA = reinterpret_cast<uint64_t>(&g_ti_a), kB = reinterpret_cast<uint64_t>(&g_ti_b), kC = reinterpret_cast<uint64_t>(&g_ti_c);

const NxCfiRow kRows[] = {
    {B + 0x100, 31, 1, 32, 0},   // Fmain: x30
    {B + 0x200, 31, 3, 32, 1},   // Fthrow: x30, x20, d8
    {B + 0x300, 31, 1, 16, 4},   // Fmid: x30
};
const NxCfiSaved kSaved[] = {
    {30, -8},                                   // Fmain
    {30, -8}, {20, -16}, {72, -24},             // Fthrow
    {30, -8},                                   // Fmid
};
// Fmain: catch A (filter 1) then catch(...) (filter 2, type slot 2 = 0)
const NxCallSite kSites[] = {
    {B + 0x140, 0x10, kPadMain, 1},             // Fmain: action chain #1
    {B + 0x340, 0x10, kPadMid, 0},              // Fmid: cleanup only
};
const NxAction kActions[] = {
    {1, 2},                                     // action 1: catch types[1] -> next action 2
    {2, 0},                                     // action 2: catch types[2] (catch(...)) -> end
};
// types: Fmain's table starts at 0: [A, 0]
const uint64_t kTypes[] = {kA, 0};
const NxFde kFdes[] = {
    {B + 0x100, B + 0x200, 0, 1, 0, 1, 0},
    {B + 0x200, B + 0x300, 1, 1, 1, 0, 0},
    {B + 0x300, B + 0x400, 2, 1, 1, 1, 0},
};
// the actions/types referenced by the Fmain site: filter 1 -> types[0] = A, filter 2 -> types[1] = 0
NxEh g_eh = {kFdes, 3, kRows, kSaved, kSites, kActions, kTypes};

void fn_noop(::Ctx*) {}
void cleanup_resume(::Ctx* c, uint64_t pad);
void setjmp_resume(::Ctx*, uint64_t) {}
const NxResume kResume[] = {{kPadJmp, setjmp_resume}, {kPadMid, cleanup_resume}};   // sorted by pad
const FnEntry kFns[] = {{B + 0x100, fn_noop}, {B + 0x200, fn_noop}, {B + 0x300, fn_noop}};
NxModule g_mod = {"ehtest", 20u, B, B + 0x1000, kFns, 3, nullptr, 0, kResume, 2, &g_eh};

// a guest stack in host memory
alignas(16) uint64_t g_stack[512];
uint64_t stack_top() { return reinterpret_cast<uint64_t>(g_stack + 512); }
void put(uint64_t addr, uint64_t v) { std::memcpy(reinterpret_cast<void*>(addr), &v, 8); }

// observations made by the host "guest code"
struct Seen {
    int body_calls = 0, handler_runs = 0, cleanup_runs = 0;
    uint64_t pad = 0, x0 = 0, x1 = 0, x19 = 0, x20 = 0, d8 = 0, sp = 0;
    uint64_t sp_expected = 0;
    uint64_t exc = 0;
    int thrown_kind = 0;           // what the next Fthrow throws: 0 A, 1 B (uncaught by Fmain's A handler but caught by catch(...)), 2 C foreign
    bool via_cleanup = false;
    int order = 0, cleanup_order = 0, handler_order = 0;
};
Seen g_seen;
::Ctx* g_ctx_for_cleanup = nullptr;

nxrt::eh::Thrown make_thrown(uint64_t exc, uint64_t type) {
    nxrt::eh::Thrown t;
    t.exc = exc;
    t.type = type;
    t.object = exc;
    return t;
}

// Host stand-in for Fthrow: pushes its frame under Fmain's call at B+0x144 (or Fmid's, via_cleanup), clobbers x20 and d8, raises.
uint64_t guest_throw(::Ctx* c, uint64_t ret_in_caller, uint64_t exc, uint64_t type) {
    uint64_t cfa = c->x[31];                       // caller's sp at the call
    uint64_t sp = cfa - 32;
    put(sp + 32 - 8, ret_in_caller);               // x30
    put(sp + 32 - 16, c->x[20]);                   // x20 as the caller had it
    put(sp + 32 - 24, c->v[8].d[0]);               // d8 as the caller had it
    c->x[31] = sp;
    c->x[20] = 0xdead20;
    c->v[8].d[0] = 0xdead08;
    c->x[30] = B + 0x210;                          // return address inside Fthrow
    return nxrt::eh::raise(c, make_thrown(exc, type));
}

// Fmain body: pad 0 = normal entry, else the landing pad.
void fmain_body(::Ctx* c, uint64_t pad) {
    g_seen.body_calls++;
    if (pad) {
        g_seen.handler_runs++;
        g_seen.pad = pad;
        g_seen.x0 = c->x[0];
        g_seen.x1 = c->x[1];
        g_seen.x19 = c->x[19];
        g_seen.x20 = c->x[20];
        g_seen.d8 = c->v[8].d[0];
        g_seen.sp = c->x[31];
        return;
    }
    // prologue: sp -= 32, x30 saved at CFA-8 (return to nothing: the end of the guest stack)
    const uint64_t entry_sp = c->x[31];
    put(entry_sp - 8, 0);
    c->x[31] = entry_sp - 32;
    g_seen.sp_expected = entry_sp - 32;
    c->x[19] = 0x1919;
    c->x[20] = 0x2020;
    c->v[8].d[0] = 0x0808;
    c->x[30] = B + 0x144;                          // Fmain calls Fthrow at B+0x140
    uint64_t type = g_seen.thrown_kind == 0 ? kA : g_seen.thrown_kind == 1 ? kB : 0;
    uint64_t r = guest_throw(c, B + 0x144, 0x7777, type);
    // returns only when nothing caught it
    g_seen.x0 = r;
}

void reset_world() {
    modules_unload_all();
    hle_clear();
    hooks_clear();
    nxrt::eh::set_typeinfo_vtables({});
    nxrt::eh::set_abi_autodetect();
    nxrt::eh::stats_reset();
    nxrt::eh::set_terminate_handler(nullptr);
    g_seen = Seen{};
    std::memset(g_stack, 0, sizeof g_stack);
}

bool register_module() { return NXRT_REGISTER_NXMODULE(nullptr, g_mod).ok(); }

void cleanup_resume(::Ctx* c, uint64_t pad) {
    g_seen.cleanup_runs++;
    g_seen.cleanup_order = ++g_seen.order;
    g_seen.pad = pad;
    g_seen.x1 = c->x[1];
    g_seen.x0 = c->x[0];
    // the cleanup code finishes: _Unwind_Resume from inside Fmid (return address B+0x350)
    c->x[30] = B + 0x350;
    nxrt::eh::resume(c, make_thrown(c->x[0], kB));
    g_seen.x0 = 0xbad;                              // resume returned: nothing caught
}

}  // namespace

NXTEST(lands_in_catch_frame_with_selector_and_restored_registers) {
    reset_world();
    REQUIRE(register_module());
    ::Ctx c{};
    c.x[31] = stack_top();
    g_seen.thrown_kind = 0;
    nx_catch_call(&c, fmain_body);
    CHECK_EQ(g_seen.body_calls, 2);
    CHECK_EQ(g_seen.handler_runs, 1);
    CHECK_EQ(g_seen.pad, kPadMain);
    CHECK_EQ(g_seen.x0, 0x7777u);                  // the exception pointer
    CHECK_EQ(g_seen.x1, 1u);                       // selector: filter 1 = catch (A)
    CHECK_EQ(g_seen.x19, 0x1919u);                 // Fmain's own callee-saved registers as at the call
    CHECK_EQ(g_seen.x20, 0x2020u);                 // Fthrow clobbered it, its CFI said where the original is
    CHECK_EQ(g_seen.d8, 0x0808u);                  // low half of v8
    CHECK_EQ(g_seen.sp, g_seen.sp_expected);       // SP of Fmain's frame
    CHECK_EQ(nxrt::eh::catch_frame_depth(), 0u);
    nxrt::eh::Stats st = nxrt::eh::stats();
    CHECK_EQ(st.raised, 1u);
    CHECK_EQ(st.caught, 1u);
    CHECK_EQ(st.catch_frame_landings, 1u);
}

NXTEST(catch_all_takes_other_types_with_selector_2) {
    reset_world();
    REQUIRE(register_module());
    ::Ctx c{};
    c.x[31] = stack_top();
    g_seen.thrown_kind = 1;                        // type B: only catch(...) matches
    nx_catch_call(&c, fmain_body);
    CHECK_EQ(g_seen.handler_runs, 1);
    CHECK_EQ(g_seen.x1, 2u);
    // a foreign exception (no type) is caught by catch(...) only
    reset_world();
    REQUIRE(register_module());
    ::Ctx c2{};
    c2.x[31] = stack_top();
    g_seen.thrown_kind = 2;
    nx_catch_call(&c2, fmain_body);
    CHECK_EQ(g_seen.handler_runs, 1);
    CHECK_EQ(g_seen.x1, 2u);
}

NXTEST(no_handler_returns_end_of_stack_and_leaves_registers) {
    reset_world();
    REQUIRE(register_module());
    // Fthrow called from a function outside any try: the walk ends at the zero return address
    ::Ctx c{};
    c.x[31] = stack_top() - 64;
    put(c.x[31] - 8, 0);
    c.x[19] = 0x1919;
    uint64_t sp0 = c.x[31];
    uint64_t r = guest_throw(&c, 0, 0x55, kC);      // return address 0 in the caller frame: nothing above
    CHECK_EQ(r, nxrt::eh::kUrcEndOfStack);
    CHECK_EQ(c.x[19], 0x1919u);
    CHECK_EQ(c.x[31], sp0 - 32);                    // untouched by the failed raise (guest_throw set its own frame)
    CHECK_EQ(nxrt::eh::stats().uncaught, 1u);
    CHECK_EQ(nxrt::eh::stats().caught, 0u);
}

NXTEST(unknown_pc_ends_the_walk_without_crashing) {
    reset_world();
    REQUIRE(register_module());
    ::Ctx c{};
    c.x[31] = stack_top();
    c.x[30] = 0x1234567;                            // not in any module
    uint64_t r = nxrt::eh::raise(&c, make_thrown(1, kA));
    CHECK_EQ(r, nxrt::eh::kUrcEndOfStack);
    CHECK(nxrt::eh::guest_backtrace(&c).find("no unwind information") != std::string::npos);
}

NXTEST(cleanup_pad_runs_before_the_handler) {
    reset_world();
    REQUIRE(register_module());
    // Fmain -> Fmid -> Fthrow. Fmid has only a cleanup pad (resumed through NxResume); Fmain catches.
    struct Body {
        static void run(::Ctx* c, uint64_t pad) {
            g_seen.body_calls++;
            if (pad) {
                g_seen.handler_runs++;
                g_seen.handler_order = ++g_seen.order;
                g_seen.x1 = c->x[1];
                return;
            }
            const uint64_t entry_sp = c->x[31];
            put(entry_sp - 8, 0);
            c->x[31] = entry_sp - 32;                // Fmain frame
            // Fmid frame: CFA = sp + 16, x30 at CFA-8, called from Fmain at B+0x144
            const uint64_t mid_cfa = c->x[31];
            put(mid_cfa - 8, B + 0x144);
            c->x[31] = mid_cfa - 16;
            // Fthrow frame under Fmid, called from Fmid at B+0x344
            const uint64_t thr_cfa = c->x[31];
            put(thr_cfa - 8, B + 0x344);
            put(thr_cfa - 16, 0);
            put(thr_cfa - 24, 0);
            c->x[31] = thr_cfa - 32;
            c->x[30] = B + 0x210;
            nxrt::eh::raise(c, make_thrown(0x4242, kB));      // B: Fmain's catch(...) takes it
            g_seen.x0 = 0xbad;
        }
    };
    ::Ctx c{};
    c.x[31] = stack_top();
    nx_catch_call(&c, Body::run);
    CHECK_EQ(g_seen.cleanup_runs, 1);
    CHECK_EQ(g_seen.handler_runs, 1);
    CHECK(g_seen.cleanup_order < g_seen.handler_order);
    CHECK_EQ(g_seen.x1, 2u);                         // catch(...) of Fmain
    CHECK_EQ(g_seen.pad, kPadMid);                   // the cleanup ran at Fmid's pad
    nxrt::eh::Stats st = nxrt::eh::stats();
    CHECK_EQ(st.cleanups, 1u);
    CHECK_EQ(st.caught, 1u);
    CHECK_EQ(st.resume_fn_landings, 1u);
    CHECK_EQ(st.resumed, 1u);
}

NXTEST(libcxxabi_header_front_end_publishes_handler_info) {
    reset_world();
    REQUIRE(register_module());
    // a fake __cxa_exception: 0x80 bytes header, the _Unwind_Exception at +0x60, the thrown object at +0x80
    alignas(16) static unsigned char exc_mem[0x100];
    std::memset(exc_mem, 0, sizeof exc_mem);
    const uint64_t hdr = reinterpret_cast<uint64_t>(exc_mem);
    const uint64_t ue = hdr + 0x60, obj = hdr + 0x80;
    put(hdr + 0x10, kA);                              // exceptionType
    put(ue, 0x434C4E47432B2B00ull);                   // exception_class "CLNGC++\0"
    struct Body {
        static void run(::Ctx* c, uint64_t pad) {
            if (pad) { g_seen.handler_runs++; g_seen.x0 = c->x[0]; g_seen.x1 = c->x[1]; return; }
            const uint64_t entry_sp = c->x[31];
            put(entry_sp - 8, 0);
            c->x[31] = entry_sp - 32;
            c->x[30] = B + 0x144;
            c->x[0] = reinterpret_cast<uint64_t>(exc_mem) + 0x60;
            nxrt::eh::unwind_raise_exception(c);              // as _Unwind_RaiseException would, from Fmain's call site
            g_seen.x0 = 0xbad;
        }
    };
    ::Ctx c{};
    c.x[31] = stack_top();
    nx_catch_call(&c, Body::run);
    CHECK_EQ(g_seen.handler_runs, 1);
    CHECK_EQ(g_seen.x0, ue);                          // the landing pad gets the _Unwind_Exception pointer
    CHECK_EQ(g_seen.x1, 1u);
    uint64_t adj = 0, temp = 0;
    int32_t sw = 0;
    std::memcpy(&adj, exc_mem + 0x58, 8);
    std::memcpy(&temp, exc_mem + 0x50, 8);
    std::memcpy(&sw, exc_mem + 0x3c, 4);
    CHECK_EQ(adj, obj);                               // adjustedPtr: what __cxa_begin_catch returns
    CHECK_EQ(temp, kPadMain);                         // catchTemp
    CHECK_EQ(sw, 1);                                  // handlerSwitchValue
}

NXTEST(dependent_exception_uses_the_primary_type_and_object) {
    reset_world();
    REQUIRE(register_module());
    alignas(16) static unsigned char prim_mem[0x100], dep_mem[0x100];
    std::memset(prim_mem, 0, sizeof prim_mem);
    std::memset(dep_mem, 0, sizeof dep_mem);
    const uint64_t prim_obj = reinterpret_cast<uint64_t>(prim_mem) + 0x80;
    put(prim_obj - 0x70, kA);                         // the primary's exceptionType
    const uint64_t dep_ue = reinterpret_cast<uint64_t>(dep_mem) + 0x60;
    put(dep_ue - 0x58, prim_obj);                     // primaryException
    put(dep_ue, 0x434C4E47432B2B01ull);               // dependent class
    struct Body {
        static void run(::Ctx* c, uint64_t pad) {
            if (pad) { g_seen.handler_runs++; g_seen.x1 = c->x[1]; return; }
            const uint64_t entry_sp = c->x[31];
            put(entry_sp - 8, 0);
            c->x[31] = entry_sp - 32;
            c->x[30] = B + 0x144;
            c->x[0] = reinterpret_cast<uint64_t>(dep_mem) + 0x60;
            nxrt::eh::unwind_raise_exception(c);
            g_seen.x0 = 0xbad;
        }
    };
    ::Ctx c{};
    c.x[31] = stack_top();
    nx_catch_call(&c, Body::run);
    CHECK_EQ(g_seen.handler_runs, 1);
    CHECK_EQ(g_seen.x1, 1u);                          // type A found through the primary
    uint64_t adj = 0;
    std::memcpy(&adj, dep_mem + 0x58, 8);
    CHECK_EQ(adj, prim_obj);
}

NXTEST(foreign_class_exception_matches_only_catch_all) {
    reset_world();
    REQUIRE(register_module());
    alignas(16) static unsigned char f_mem[0x100];
    std::memset(f_mem, 0, sizeof f_mem);
    const uint64_t ue = reinterpret_cast<uint64_t>(f_mem) + 0x60;
    put(reinterpret_cast<uint64_t>(f_mem) + 0x10, kA);   // a type is there but the class says foreign
    put(ue, 0x4a415641006a6176ull);                   // some other vendor
    struct Body {
        static void run(::Ctx* c, uint64_t pad) {
            if (pad) { g_seen.handler_runs++; g_seen.x1 = c->x[1]; return; }
            const uint64_t entry_sp = c->x[31];
            put(entry_sp - 8, 0);
            c->x[31] = entry_sp - 32;
            c->x[30] = B + 0x144;
            c->x[0] = reinterpret_cast<uint64_t>(f_mem) + 0x60;
            nxrt::eh::unwind_raise_exception(c);
            g_seen.x0 = 0xbad;
        }
    };
    ::Ctx c{};
    c.x[31] = stack_top();
    nx_catch_call(&c, Body::run);
    CHECK_EQ(g_seen.handler_runs, 1);
    CHECK_EQ(g_seen.x1, 2u);                          // not the typed catch(A) although the bytes say A
}

NXTEST(nested_catch_frames_unwind_to_the_right_one) {
    reset_world();
    REQUIRE(register_module());
    // Fmain (outer activation, catches B with catch(...)) -> Fmid (inner activation through nx_catch_call, a cleanup pad) -> Fthrow.
    // Both are catch frames; the unwinder lands in the inner one (cleanup, selector 0), whose pad code resumes the unwinding, then in the
    // outer one (selector 2), discarding the inner record.
    struct Inner {
        static void run(::Ctx* c, uint64_t pad) {
            if (pad) {                                   // Fmid's cleanup code, then _Unwind_Resume
                g_seen.cleanup_runs++;
                g_seen.cleanup_order = ++g_seen.order;
                g_seen.x1 = c->x[1];
                CHECK_EQ(nxrt::eh::catch_frame_depth(), 2u);
                c->x[30] = B + 0x350;
                nxrt::eh::resume(c, make_thrown(c->x[0], kB));
                g_seen.x0 = 0xbad;
                return;
            }
            const uint64_t cfa = c->x[31];               // Fmid's CFA = Fmain's sp
            put(cfa - 8, B + 0x144);
            c->x[31] = cfa - 16;
            guest_throw(c, B + 0x344, 0x99, kB);         // Fthrow under Fmid
            g_seen.x0 = 0xbad;
        }
    };
    struct Outer {
        static void run(::Ctx* c, uint64_t pad) {
            if (pad) {
                g_seen.handler_runs++;
                g_seen.handler_order = ++g_seen.order;
                g_seen.pad = pad;
                g_seen.x0 = c->x[0];
                g_seen.x1 = c->x[1];
                return;
            }
            const uint64_t entry_sp = c->x[31];
            put(entry_sp - 8, 0);
            c->x[31] = entry_sp - 32;
            nx_catch_call(c, Inner::run);                 // the inner activation: entry sp = Fmain's sp
            g_seen.x0 = 0xbad;
        }
    };
    ::Ctx c{};
    c.x[31] = stack_top();
    nx_catch_call(&c, Outer::run);
    CHECK_EQ(g_seen.cleanup_runs, 1);
    CHECK_EQ(g_seen.handler_runs, 1);
    CHECK(g_seen.cleanup_order < g_seen.handler_order);
    CHECK_EQ(g_seen.pad, kPadMain);
    CHECK_EQ(g_seen.x0, 0x99u);
    CHECK_EQ(g_seen.x1, 2u);
    CHECK_EQ(nxrt::eh::catch_frame_depth(), 0u);
    CHECK_EQ(nxrt::eh::stats().catch_frame_landings, 2u);
}

NXTEST(same_function_can_be_landed_in_twice) {
    reset_world();
    REQUIRE(register_module());
    struct Body {
        static void run(::Ctx* c, uint64_t pad) {
            g_seen.body_calls++;
            if (pad) {
                g_seen.handler_runs++;
                if (g_seen.handler_runs == 1) {            // the handler's code runs the loop body again: a second throw, in the same activation
                    c->x[30] = B + 0x144;
                    guest_throw(c, B + 0x144, 0x2, kA);
                    g_seen.x0 = 0xbad;
                }
                g_seen.x1 = c->x[1];
                return;
            }
            const uint64_t entry_sp = c->x[31];
            put(entry_sp - 8, 0);
            c->x[31] = entry_sp - 32;
            c->x[30] = B + 0x144;
            guest_throw(c, B + 0x144, 0x1, kA);
            g_seen.x0 = 0xbad;
        }
    };
    ::Ctx c{};
    c.x[31] = stack_top();
    nx_catch_call(&c, Body::run);
    CHECK_EQ(g_seen.handler_runs, 2);
    CHECK_EQ(g_seen.body_calls, 3);
    CHECK_EQ(nxrt::eh::catch_frame_depth(), 0u);
}

NXTEST(terminate_handler_gets_a_backtrace) {
    reset_world();
    REQUIRE(register_module());
    static std::string seen_why;
    nxrt::eh::set_terminate_handler([](::Ctx*, const char* why) { seen_why = why; });
    ::Ctx c{};
    c.x[31] = stack_top();
    put(c.x[31] - 8, 0);
    c.x[30] = B + 0x144;                             // as if called from Fmain
    c.x[31] -= 32;
    nxrt::eh::terminate_process(&c, "unit test");
    CHECK(seen_why == "unit test");
    std::string bt = nxrt::eh::guest_backtrace(&c);
    CHECK(bt.find("#0 0x") != std::string::npos);
    CHECK(bt.find("function 0x") != std::string::npos);     // names the FDE start when no symbol is known
    nxrt::eh::set_terminate_handler(nullptr);
}

NXTEST(unwind_hle_registration) {
    reset_world();
    nxrt::eh::register_unwind_hle();
    CHECK(hle_lookup("_Unwind_RaiseException") != nullptr);
    CHECK(hle_lookup("_Unwind_Resume") != nullptr);
    CHECK(hle_lookup("_Unwind_Resume_or_Rethrow") != nullptr);
    CHECK(hle_lookup("_Unwind_DeleteException") != nullptr);
    CHECK(hle_lookup("_Unwind_ForcedUnwind") != nullptr);
    CHECK(module_symbol_overridden("_Unwind_RaiseException"));
    CHECK(!module_symbol_overridden("_Unwind_ForcedUnwind"));
    ::Ctx c{};
    hle_lookup("_Unwind_ForcedUnwind")(&c);
    CHECK_EQ(c.x[0], nxrt::eh::kUrcFatalPhase1Error);
    modules_unload_all();
}

NXTEST(delete_exception_calls_the_cleanup_function) {
    reset_world();
    alignas(16) static unsigned char mem[0x40];
    std::memset(mem, 0, sizeof mem);
    static uint64_t got_reason = 0, got_ue = 0;
    got_reason = got_ue = 0;
    const uint64_t fn_addr = 0x7240000040ull;
    fn_register(fn_addr, [](::Ctx* c) { got_reason = c->x[0]; got_ue = c->x[1]; });
    put(reinterpret_cast<uint64_t>(mem) + 8, fn_addr);              // _Unwind_Exception::exception_cleanup
    ::Ctx c{};
    c.x[0] = reinterpret_cast<uint64_t>(mem);
    nxrt::eh::unwind_delete_exception(&c);
    CHECK_EQ(got_reason, 1u);                                       // _URC_FOREIGN_EXCEPTION_CAUGHT
    CHECK_EQ(got_ue, reinterpret_cast<uint64_t>(mem));
    fn_clear();
}

NXTEST(older_libcxxabi_layout_is_detected_and_used) {
    reset_world();
    REQUIRE(register_module());
    // typeinfos with real kinds so that detection can tell the layouts apart
    static void* vt_class[8];
    nxrt::eh::TypeinfoVtables v;
    v.klass = reinterpret_cast<uint64_t>(vt_class);
    nxrt::eh::set_typeinfo_vtables(v);
    static Ti ti_real = {vt_class + 2, "1A"};            // same name as the module's catch type A (g_ti_a), a real class typeinfo
    // the SDK-style header: no reserve word, so exceptionType is at ue-0x58, adjustedPtr at ue-0x10; ue-0x50 holds something else
    alignas(16) static unsigned char exc_mem[0x100];
    std::memset(exc_mem, 0, sizeof exc_mem);
    const uint64_t hdr = reinterpret_cast<uint64_t>(exc_mem);
    const uint64_t ue = hdr + 0x60;
    put(ue - 0x58, reinterpret_cast<uint64_t>(&ti_real));
    put(ue - 0x50, 0x12345);                              // the destructor slot of the older layout: not a typeinfo
    put(ue, 0x434C4E47432B2B00ull);
    struct Body {
        static void run(::Ctx* c, uint64_t pad) {
            if (pad) { g_seen.handler_runs++; g_seen.x0 = c->x[0]; g_seen.x1 = c->x[1]; return; }
            const uint64_t entry_sp = c->x[31];
            put(entry_sp - 8, 0);
            c->x[31] = entry_sp - 32;
            c->x[30] = B + 0x144;
            c->x[0] = reinterpret_cast<uint64_t>(exc_mem) + 0x60;
            nxrt::eh::unwind_raise_exception(c);
            g_seen.x0 = 0xbad;
        }
    };
    ::Ctx c{};
    c.x[31] = stack_top();
    nx_catch_call(&c, Body::run);
    CHECK_EQ(g_seen.handler_runs, 1);
    CHECK_EQ(g_seen.x1, 1u);
    CHECK_EQ(nxrt::eh::abi_layout().exception_type, nxrt::eh::abi_layout_llvm_old().exception_type);
    uint64_t adj = 0, temp = 0;
    std::memcpy(&adj, exc_mem + 0x50, 8);                 // ue - 0x10
    std::memcpy(&temp, exc_mem + 0x48, 8);                // ue - 0x18
    CHECK_EQ(adj, ue + 0x20);
    CHECK_EQ(temp, kPadMain);
    nxrt::eh::set_typeinfo_vtables({});
    nxrt::eh::set_abi_autodetect();
}

NXTEST(uncaught_report_calls_what_of_std_exception_types) {
    reset_world();
    REQUIRE(register_module());
    static void* vt_class[8];
    static void* vt_si[8];
    nxrt::eh::TypeinfoVtables v;
    v.klass = reinterpret_cast<uint64_t>(vt_class);
    v.si_class = reinterpret_cast<uint64_t>(vt_si);
    nxrt::eh::set_typeinfo_vtables(v);
    struct TiSi { const void* vptr; const char* name; const void* base; };
    static Ti ti_exception = {vt_class + 2, "St9exception"};
    static TiSi ti_mine = {vt_si + 2, "N4game5ErrorE", &ti_exception};
    // the thrown object: a vptr to a three-slot vtable whose third slot (what) is a lifted function returning a string
    static const char what_text[] = "out of cheese";
    static int what_calls = 0;
    what_calls = 0;
    const uint64_t what_addr = 0x7240000100ull;
    fn_register(what_addr, [](::Ctx* c) { what_calls++; c->x[0] = reinterpret_cast<uint64_t>(what_text); });
    static uint64_t vtable[3];
    vtable[2] = what_addr;
    alignas(16) static unsigned char exc_mem[0x100];
    std::memset(exc_mem, 0, sizeof exc_mem);
    const uint64_t hdr = reinterpret_cast<uint64_t>(exc_mem);
    const uint64_t ue = hdr + 0x60, obj = hdr + 0x80;
    put(ue - 0x50, reinterpret_cast<uint64_t>(&ti_mine));
    put(ue, 0x434C4E47432B2B00ull);
    put(obj, reinterpret_cast<uint64_t>(vtable));         // the object's vptr
    ::Ctx c{};
    c.x[31] = stack_top() - 64;
    put(c.x[31] - 8, 0);
    c.x[30] = 0;                                          // nothing above: no handler
    c.x[0] = ue;
    nxrt::eh::unwind_raise_exception(&c);
    CHECK_EQ(c.x[0], nxrt::eh::kUrcEndOfStack);
    CHECK_EQ(what_calls, 1);                              // the report asked the exception what it was
    fn_clear();
    nxrt::eh::set_typeinfo_vtables({});
}

// Guest setjmp/longjmp: Fmain called setjmp (return address kPadJmp), then an inner function with a catch frame of its own runs the
// guest longjmp, which restores Fmain's SP and x0 = 1 and branches to kPadJmp. dispatch_call finds no function there and resumes
// Fmain's catch frame at the pad, skipping the inner frame (its CFA is below the restored SP).
NXTEST(guest_longjmp_resumes_the_setjmp_caller_at_its_pad) {
    reset_world();
    REQUIRE(register_module());
    struct Inner {
        static void run(::Ctx* c, uint64_t pad) {
            if (pad) { g_seen.x0 = 0xbad; return; }   // must not be landed in
            c->x[31] = g_seen.sp_expected;              // the guest longjmp: registers as setjmp saved them, x0 = the value
            c->x[0] = 1;
            nxrt::dispatch_call(c, kPadJmp);
            g_seen.x1 = 0xbad;                          // dispatch_call returned: not landed
        }
    };
    struct Body {
        static void run(::Ctx* c, uint64_t pad) {
            g_seen.body_calls++;
            if (pad) {
                g_seen.pad = pad;
                g_seen.x0 = c->x[0];
                g_seen.sp = c->x[31];
                return;
            }
            const uint64_t entry_sp = c->x[31];
            c->x[31] = entry_sp - 32;                   // Fmain's frame: setjmp saved this SP
            g_seen.sp_expected = c->x[31];
            c->x[31] = entry_sp - 96;                   // a deeper function entered through its own catch frame
            nx_catch_call(c, Inner::run);
            g_seen.x1 = 0xbad;
        }
    };
    ::Ctx c{};
    c.x[31] = stack_top();
    nx_catch_call(&c, Body::run);
    CHECK_EQ(g_seen.body_calls, 2);
    CHECK_EQ(g_seen.pad, kPadJmp);
    CHECK_EQ(g_seen.x0, 1u);
    CHECK_EQ(g_seen.x1, 0u);
    CHECK_EQ(g_seen.sp, g_seen.sp_expected);
    CHECK_EQ(nxrt::eh::catch_frame_depth(), 0u);
    // an address that is no landing pad is not taken for a longjmp target
    ::Ctx c2{};
    c2.x[31] = stack_top();
    CHECK(!nxrt::eh::longjmp_landing(&c2, B + 0x158));
}
