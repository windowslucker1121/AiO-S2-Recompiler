// SPDX-License-Identifier: GPL-3.0-or-later
// [nvn] render_cpus = auto (nvn_cmdq.cpp apply_render_pin): which logical processors the render thread and the recorder thread are pinned
// to, from the machine's topology and the console core model's choice for the guest threads (kernel sched_info). Pure, so it is tested on
// synthetic topologies (tests/gfx/test_nvn_cpu_pick.cpp).
#pragma once
#include <cstdint>
#include <vector>

namespace nxrt::nvn::be::cmdq {

struct CpuCore {
    uint64_t mask = 0;   // its logical processors (processor group 0)
    int eff = 0;         // efficiency class: on hybrid CPUs the performance cores have the higher one
    int llc = -1;        // last-level cache it shares (index into the machine's L3 list), -1 unknown
};
struct CpuGuest {
    bool model = false;      // the console core model is on with soft affinity (ideal processors + a pool)
    uint64_t ideal = 0;      // the guest threads' ideal processors (every console core's)
    uint64_t main = 0;       // console core 0's (the game's main thread)
    uint64_t pool = 0;       // processors every guest thread may use
};
struct CpuPick {
    int render = -1, recorder = -1;   // -1: not pinned
    const char* why = "";
};

inline int cpu_lowest(uint64_t m) {
    for (int b = 0; b < 64; ++b) if (m >> b & 1) return b;
    return -1;
}

// N1 (PLAN_LOWS): a place for the recorder when only one fast core is free: the second hardware thread of a guest core (a fast core, not
// core 0, that holds an ideal processor of the console core model, with a hardware thread that is no ideal processor). With `busy` (non-idle
// microseconds per logical processor over a recent window, 64 entries) the core with the least busy pair; without it the main guest
// thread's core (its thread waits for the GPU and its workers for much of a frame; logical 13 on the development machine). -1: none.
inline int pick_recorder_sibling(const std::vector<CpuCore>& cores, const CpuGuest& g, const uint64_t* busy = nullptr) {
    if (!g.model || !g.ideal) return -1;
    int best = cores.empty() ? 0 : cores[0].eff;
    for (const CpuCore& c : cores) if (c.eff > best) best = c.eff;
    int pick = -1;
    uint64_t pick_score = ~uint64_t{0};
    for (const CpuCore& c : cores) {
        if (c.eff != best || (c.mask & 1) || !(c.mask & g.ideal)) continue;
        const uint64_t sib = c.mask & ~g.ideal;
        if (!sib) continue;
        const int s = cpu_lowest(sib);
        uint64_t score;
        if (busy) {
            score = 0;
            for (int b = 0; b < 64; ++b) if (c.mask >> b & 1) score += busy[b];
        } else {
            score = (g.main && (c.mask & g.main)) ? 0 : 1;
        }
        if (score < pick_score) { pick_score = score; pick = s; }
    }
    return pick;
}

// Rules: only the fastest cores (never efficiency cores of a hybrid CPU); never core 0 (interrupts) for the render thread; never a core
// the guest threads prefer (an ideal processor of the console core model); with a shared last-level cache of the main guest thread's
// core known, cores that share it first (X3D / several CCDs: the render thread works on the game's data). The recorder gets a second such
// core, or else (N1) the second hardware thread of a guest core (pick_recorder_sibling; core 0 runs most interrupts and DPCs of the
// machine: 9,200 interrupts and 6,800 DPCs a second on the development machine), or else the second hardware thread of core 0 when the
// guest pool leaves it out. Anything else: no pinning. `legacy`: the rule before N1 (core 0's second hardware thread first).
inline CpuPick pick_render_cpus(const std::vector<CpuCore>& cores, const CpuGuest& g, bool legacy = false) {
    CpuPick p;
    if (cores.size() < 4) { p.why = "fewer than 4 cores"; return p; }
    int best = cores[0].eff;
    for (const CpuCore& c : cores) if (c.eff > best) best = c.eff;
    uint64_t core0 = 0;
    for (const CpuCore& c : cores) if (c.mask & 1) core0 = c.mask;
    int main_llc = -1;
    for (const CpuCore& c : cores) if (g.main && (c.mask & g.main)) main_llc = c.llc;
    size_t nfast = 0;
    for (const CpuCore& c : cores) if (c.eff == best) ++nfast;
    std::vector<const CpuCore*> free;   // fast cores without guest preference, not core 0; the main thread's cache first
    for (int pass = 0; pass < 2; ++pass)
        for (const CpuCore& c : cores) {
            if (c.eff != best || (c.mask & 1) || !c.mask || (g.model && (c.mask & g.ideal))) continue;
            const bool first = main_llc < 0 || c.llc == main_llc;
            if (first == (pass == 0)) free.push_back(&c);
        }
    if (!g.model && nfast < 6) { p.why = "no console core model and fewer than 6 fast cores"; return p; }
    if (free.empty()) { p.why = "every fast core is a guest core"; return p; }
    p.render = cpu_lowest(free[0]->mask);
    if (free.size() >= 2) {
        p.recorder = cpu_lowest(free[1]->mask);
        p.why = "two free fast cores";
    } else if (const int sib = legacy ? -1 : pick_recorder_sibling(cores, g); sib >= 0) {
        p.recorder = sib;
        p.why = "one free fast core, recorder on the second hardware thread of the main guest thread's core";
    } else if ((core0 & 2) && !(g.pool & 2) && (!g.model || !(g.ideal & 2))) {
        p.recorder = 1;   // the second hardware thread of core 0
        p.why = "one free fast core, recorder on core 0's second hardware thread";
    } else {
        p.render = -1;
        p.why = "no second place for the recorder";
    }
    return p;
}

}  // namespace nxrt::nvn::be::cmdq
