// SPDX-License-Identifier: GPL-3.0-or-later
// nx_catch_call: the catch frames of lifted functions that contain a catch clause (recomp ctx.h; ssbu-recomp's nro_catch_call is the
// model, only its name and idea are used). The lifter enters such a function as nx_catch_call(c, u_<entry>). This runs body(c, 0)
// under a setjmp frame keyed by the guest SP at entry (= the function's CFA). When the unwinder lands in that function it stores the
// landing pad in the frame and longjmps here, discarding every host frame above; the loop then calls body(c, pad), which resumes the
// lifted function at the pad. The same function may be landed in again (a try inside a loop), hence the loop.
#include "eh_internal.h"
#include "nxrt/dispatch.h"
#include "nxrt/eh.h"
#include "nxrt/log.h"
#include "nxrt/nxmodule.h"

extern "C" void nx_catch_call(Ctx* c, void (*body)(Ctx*, uint64_t)) {
    using namespace nxrt::eh::detail;
    CatchRec rec;
    rec.cfa = c->x[31];
    catch_stack().push_back(&rec);
    // State that dispatch_call / host_import set up and restore on the way out; a longjmp skips that, so put it back by hand.
    const char* const saved_hle = nxrt::hle_current_name();
    const char* const saved_in_import = c->in_import;
    const uint64_t saved_import_from = c->import_from;
    const uint32_t saved_cb_flags = nxrt::host_callback_flags();   // a host bit set inside a guest callback the exception left
    volatile uint64_t pad = 0;
    for (;;) {
        if (setjmp(rec.jb) == 0) {
            body(c, pad);
            break;
        }
        pad = rec.pad;                                   // landed here by an exception
        nxrt::hle_restore_current(saved_hle);
        c->in_import = saved_in_import;
        c->import_from = saved_import_from;
        nxrt::host_callback_restore(saved_cb_flags);
    }
    auto& st = catch_stack();
    for (size_t i = st.size(); i-- > 0;)
        if (st[i] == &rec) { st.resize(i); break; }
}

namespace nxrt::eh {

// Guest longjmp (setjmp/longjmp of the guest libc): it restores the registers setjmp saved and branches to setjmp's return address,
// which is in the middle of a lifted function, so dispatch_call finds no function there. The lifter makes every such return address
// a landing pad of its function and enters that function through nx_catch_call (nxrecomp Env.add_setjmp_pads), so the lifted function
// is still live on the host stack below us: find its catch frame (the innermost one whose CFA lies above the restored SP: frames
// entered after setjmp have their CFA at or below it) and resume it at the pad with a host longjmp, dropping every host frame between.
bool longjmp_landing(::Ctx* c, uint64_t addr) {
    using namespace detail;
    const NxModule* m = nxmodule_at(addr);
    if (!m) return false;
    bool is_pad = false;
    for (int lo = 0, hi = m->nresume - 1; lo <= hi;) {
        const int mid = (lo + hi) / 2;
        if (m->resume[mid].pad == addr) { is_pad = true; break; }
        if (m->resume[mid].pad < addr) lo = mid + 1; else hi = mid - 1;
    }
    if (!is_pad) return false;
    auto& st = catch_stack();
    const uint64_t sp = c->x[31];
    for (size_t i = st.size(); i-- > 0;) {
        if (st[i]->cfa > sp) {
            CatchRec* rec = st[i];
            rec->pad = addr;
            rec->landed = true;
            st.resize(i + 1);
            NXLOG_D("eh", "guest longjmp: resuming the catch frame (cfa 0x%llx) at 0x%llx, x0 = %llu", static_cast<unsigned long long>(rec->cfa),
                    static_cast<unsigned long long>(addr), static_cast<unsigned long long>(c->x[0]));
            longjmp(rec->jb, 1);
        }
    }
    return false;
}

}  // namespace nxrt::eh
