// SPDX-License-Identifier: GPL-3.0-or-later
// nn::os fibers on host fibers (kernel.h fibers_install). The SDK switches fibers in nn::os::detail::DispatchFiberContext(save, load):
// it pushes the callee-saved registers on the current guest stack, stores that sp in *save, loads sp from *load, pops and returns
// into the other fiber. Lifted code keeps its call frames on the host stack, so switching only the guest stack returns into the
// wrong host frames (the game's nn::async tasks crashed in nn::async::Await with every register zero). An entry hook on
// DispatchFiberContext (the game's sdk.sites names the function) replaces it: every guest fiber runs on a host fiber with its own
// host stack and its own Ctx, so a fiber resumed on another thread keeps its frames and registers; only the per-thread registers
// (TPIDR_EL0, TPIDRRO_EL0) come from the thread that resumes it.
//
// A fiber the SDK has just set up (FiberImplByHorizon::Initialize) has a saved context whose x30 has bit 0 set: its entry (x30 ^ 1)
// runs with x0 = the saved x0 (its FiberType*) and sp = just above the context. A suspended fiber is found by its context slot (the
// `save` it last switched away through: FiberType +80, or a stack-local FiberType for a thread's own context).
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "nxrt/abi.h"
#include "nxrt/config.h"
#include "nxrt/diag.h"
#include "nxrt/dispatch.h"
#include "nxrt/hooks.h"
#include "nxrt/kernel.h"
#include "nxrt/log.h"
#include "nxrt/nxmodule.h"

namespace nxrt::kernel {
namespace {

constexpr const char* kDispatchFiberContext = "_ZN2nn2os6detail20DispatchFiberContextEPPNS1_22FiberContextForHorizonES4_";
constexpr uint64_t kFrame = 240;     // the SDK's saved context below sp: x0, x19..x30, a check word, q8..q15
constexpr uint64_t kFrameX0 = 0, kFrameX30 = 96;

struct HostFiber {
    void* host = nullptr;            // the Windows fiber
    ::Ctx* ctx = nullptr;            // own for a guest fiber; the thread's Ctx for a thread's own context
    std::unique_ptr<::Ctx> own;
    uint64_t entry = 0;              // guest fiber: the SDK's fiber entry, run when the host fiber first starts
};

std::mutex g_mu;
std::unordered_map<uint64_t, HostFiber*> g_by_slot;   // context slot -> the fiber that last switched away through it
thread_local HostFiber* t_cur = nullptr;               // the fiber running on this thread (null until it first switches)
thread_local HostFiber* t_thread = nullptr;            // this thread's own context as a fiber
SIZE_T g_stack_reserve = 64u << 20;
std::atomic<uint64_t> g_switches{0}, g_made{0};          // logged at every power of two

uint64_t& guest64(uint64_t addr) { return *reinterpret_cast<uint64_t*>(static_cast<uintptr_t>(addr)); }

void WINAPI fiber_main(void* p) {
    auto* f = static_cast<HostFiber*>(p);
    diag_set_thread_ctx(f->ctx);
    dispatch_call(f->ctx, f->entry);
    guest_trap("a fiber's entry function returned", f->entry);   // nn::os fibers switch away at their end and never return
}

// The hook: x0 = save slot, x1 = load slot. Returns once another fiber switches back to this one.
int dispatch_fiber_context(::Ctx* c, uint64_t, void*) {
    const uint64_t save = c->x[0], load = c->x[1];
    HostFiber* cur = t_cur;
    if (!cur) {
        if (!t_thread) {
            t_thread = new HostFiber;   // ponytail: one per thread that ever switched fibers, never freed
            t_thread->host = IsThreadAFiber() ? GetCurrentFiber() : ConvertThreadToFiberEx(nullptr, FIBER_FLAG_FLOAT_SWITCH);
        }
        t_thread->ctx = c;
        cur = t_thread;
    }
    // What the SDK would leave: a saved context below sp with an even x30 (a suspended fiber, not a new one), its address in *save.
    const uint64_t frame = c->x[31] - kFrame;
    guest64(frame + kFrameX0) = 0;
    guest64(frame + kFrameX30) = c->x[30] & ~1ull;
    guest64(save) = frame;

    HostFiber* next = nullptr;
    const uint64_t n = ++g_switches;
    if ((n & (n - 1)) == 0) NXLOG_I("fiber", "%llu switches, %llu fibers made", static_cast<unsigned long long>(n), static_cast<unsigned long long>(g_made.load()));
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_by_slot[save] = cur;
        const uint64_t in = guest64(load);
        const uint64_t x30 = in ? guest64(in + kFrameX30) : 0;
        auto it = g_by_slot.find(load);
        if (x30 & 1) {
            // A fiber the SDK has just set up. A record still bound to its slot is a finished fiber whose FiberType was reused.
            if (it != g_by_slot.end() && it->second->own && it->second != cur) {
                DeleteFiber(it->second->host);
                delete it->second;
            }
            // ponytail: a finished fiber whose slot is never reused keeps its host stack; hook nn::os::FinalizeFiber if that adds up
            next = new HostFiber;
            ++g_made;
            next->own = std::make_unique<::Ctx>();
            std::memset(next->own.get(), 0, sizeof(::Ctx));
            next->ctx = next->own.get();
            next->ctx->x[0] = guest64(in + kFrameX0);
            next->ctx->x[31] = in + kFrame;
            next->ctx->fpcr = c->fpcr;
            next->entry = x30 ^ 1;
            next->host = CreateFiberEx(64u << 10, g_stack_reserve, FIBER_FLAG_FLOAT_SWITCH, fiber_main, next);
            if (!next->host) {
                NXLOG_E("fiber", "CreateFiberEx failed (error %lu)", GetLastError());
                guest_trap("no host fiber for a guest fiber", next->entry);
            }
            g_by_slot[load] = next;
        } else if (it != g_by_slot.end()) {
            next = it->second;
        } else {
            NXLOG_E("fiber", "switch to an unknown fiber context (slot 0x%llx -> 0x%llx)", static_cast<unsigned long long>(load),
                    static_cast<unsigned long long>(in));
            guest_trap("switch to an unknown fiber context", load);
        }
    }
    if (next != cur) {
        ::Ctx* to = next->ctx;
        to->tpidr = c->tpidr;
        to->tpidrro = c->tpidrro;
        to->ex_addr = ~0ull;
        t_cur = next;
        diag_set_thread_ctx(to);
        SwitchToFiber(next->host);
        // Back on this fiber, maybe on another thread: whoever switched here set t_cur and the per-thread registers.
    }
    c->x[0] = 0;   // what the SDK's restore pops from the saved context
    return 1;
}

}  // namespace

void fibers_install() {
    const uint64_t f = nxmodule_find_export(kDispatchFiberContext);
    if (!f) return;
    const int64_t mb = config_get_int("threads", "host_stack_mb", 64);
    g_stack_reserve = static_cast<SIZE_T>((mb < 1 ? 64 : mb) << 20);
    hook_add_entry(f, dispatch_fiber_context);
    NXLOG_I("fiber", "nn::os fibers run on host fibers (DispatchFiberContext at 0x%llx; needs an entryhook site there)",
            static_cast<unsigned long long>(f));
}

}  // namespace nxrt::kernel
