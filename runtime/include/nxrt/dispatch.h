// SPDX-License-Identifier: GPL-3.0-or-later
// Indirect-call dispatch: guest address -> lifted function (hash table), and HLE thunks (placeholder guest addresses for
// host functions: imports, run-time handed-out function pointers). The generated code's call_indirect() lands here.
// Pattern from smo-recomp runtime/core/hostrt.c (fn hash, hostfn_address) and SuperMarioWonderRecomp loader.cpp (thunks).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "nxrt/abi.h"

namespace nxrt {

// ---- lifted functions ----------------------------------------------------------------------------------------------
// Registration may happen at any time (module load); lookups are lock free and safe against concurrent registration.
void fn_register(uint64_t guest_addr, GuestFn fn);
GuestFn fn_lookup(uint64_t guest_addr);
size_t fn_count();
void fn_clear();                                  // tests only: no thread may be inside a lookup
// The guest function whose host code contains `host_rip` (nearest registered function at or below it, within 4 MiB), or 0.
// Used by the crash handler and the watchdog to turn a host address into the lifted function it belongs to.
uint64_t guest_function_of(uint64_t host_rip);

// ---- inline caches of the lifted code (recomp/include/nxrecomp/ctx.h NxIc, NX_ICALL / NX_IIMPORT) ------------------------
// [cpu] inline_caches = on (default) | off | ab: whether the miss paths (nx_icall_miss / nx_iimport_miss, abi_glue.cpp) fill a
// site's cache. ab alternates [cpu] ic_ab_ms windows (default 5000; frame-trace marks ic.on / ic.off; caches are emptied when an
// off window begins) for paired measurements in one run.
// ic_fill points `ic` at an immutable target {addr, fn} (interned, never freed) unless the site keeps changing its target (after
// a few refills it stays as it is and misses dispatch normally); it registers the site so ic_invalidate_all can empty it.
bool ic_enabled();
void ic_fill(NxIc* ic, uint64_t addr, GuestFn fn);
void ic_invalidate_all();                         // a registration was replaced, the import resolution changed, or an ab window
struct IcStats { uint64_t sites = 0, fills = 0, refills = 0, frozen = 0, targets = 0, invalidations = 0; };
IcStats ic_stats();

// ---- HLE thunks ----------------------------------------------------------------------------------------------------
// Each thunk is a 16-byte slot in [mem::kHleBase, +mem::kHleSize). Calling the address through call_indirect runs the host
// handler with the guest registers; the handler returns by writing x0.. and returning (the caller continues after the call).
uint64_t hle_register(const char* name, HleFn fn);        // makes/updates the thunk for `name`; returns its address
void install_libc_hle();                                  // the SDK's strlen / strcmp / memcpy / ... as host code (libc_hle.cpp, [hle] libc)
uint64_t hle_address(const char* name);                   // thunk for `name`; an unknown name gets a logging stub (x0 = 0)
HleFn hle_lookup(const char* name);                       // the registered handler or nullptr (stubs are not "registered")
// A leaf handler (no guest calls, no faults it must report as the import's, called often: strlen, memcpy) that an import call runs
// directly, skipping the per-thunk call count, [hle] trace and the in_import bookkeeping. The name must be registered first.
void hle_mark_leaf(const char* name);
HleFn hle_leaf_fn(uint64_t addr);                         // the handler of a leaf thunk at addr, else nullptr
bool hle_is_thunk(uint64_t addr);
const char* hle_name_at(uint64_t addr);                   // name behind a thunk address or nullptr
size_t hle_count();
size_t hle_stub_count();                                  // thunks that are logging stubs (no handler)
uint64_t hle_calls(uint64_t addr);                        // times the thunk at addr has been called
struct HleInfo { std::string name; uint64_t calls = 0; bool stub = false; };
std::vector<HleInfo> hle_list();                          // every thunk, in creation order (end-of-run report)
void hle_clear();                                         // tests only
// Name of the HLE function the calling thread is currently inside, or nullptr.
const char* hle_current_name();
void hle_restore_current(const char* name);               // after a longjmp out of an HLE handler (eh/catch_call.cpp): sets what hle_current_name() answers
// Host state of this thread tied to a guest callback the host is running (bit flags, kHostCb*). The host sets a bit around the guest
// call; a guest exception that unwinds out of the callback longjmps past that reset, so eh/catch_call.cpp saves the flags when a catch
// frame is entered and puts them back when it lands there.
enum : uint32_t { kHostCbCmdMem = 1u << 0 };              // NVN command-memory callback (gfx/nvn/api_cmdbuf.cpp)
uint32_t host_callback_flags();
void host_callback_restore(uint32_t flags);

// ---- the dispatch entry used by the ABI (abi_glue.cpp defines call_indirect / guest_trap on top of it) ----------------------
// Runs the lifted function or HLE thunk at `addr` on `c`; an unknown target is a guest trap (GUEST TRAP report, exception
// 0xE0000001, handled by the crash handler).
void dispatch_call(::Ctx* c, uint64_t addr);
[[noreturn]] void trap(const char* what, uint64_t addr);   // logs "GUEST TRAP: what (0xaddr)" and raises 0xE0000001

}  // namespace nxrt
