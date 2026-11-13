// SPDX-License-Identifier: GPL-3.0-or-later
// [nvn] render_cpus = auto (src/gfx/nvn/nvn_cpu_pick.h): the processors of the render and recorder threads on synthetic topologies.
#include <cstdint>
#include <vector>

#include "nvn_cpu_pick.h"
#include "test_framework.h"
NXTEST_GROUP(gfx_nvn_cpu_pick)

using namespace nxrt::nvn::be::cmdq;

namespace {

// n cores of `smt` hardware threads each, numbered consecutively from processor `first`
std::vector<CpuCore> cores_of(int n, int smt, int eff = 0, int first = 0, int llc = -1) {
    std::vector<CpuCore> v;
    for (int i = 0; i < n; ++i) {
        CpuCore c;
        for (int t = 0; t < smt; ++t) c.mask |= 1ull << (first + i * smt + t);
        c.eff = eff;
        c.llc = llc;
        v.push_back(c);
    }
    return v;
}
// the console core model as the kernel picks it under soft affinity: six ideal processors, the pool without core 0
CpuGuest guest(std::initializer_list<int> ideal, uint64_t pool) {
    CpuGuest g;
    g.model = true;
    bool first = true;
    for (int p : ideal) {
        g.ideal |= 1ull << p;
        if (first) g.main = 1ull << p;
        first = false;
    }
    g.pool = pool;
    return g;
}

}  // namespace

NXTEST(eight_cores_smt_take_the_free_core_and_the_main_cores_second_thread) {
    // the development machine: guest ideals on cores 1..6 (12, 10, 8, 6, 4, 2), core 7 free; N1: the recorder off core 0
    const CpuPick p = pick_render_cpus(cores_of(8, 2), guest({12, 10, 8, 6, 4, 2}, 0xfffc));
    CHECK(p.render == 14);
    CHECK(p.recorder == 13);
    const CpuPick old = pick_render_cpus(cores_of(8, 2), guest({12, 10, 8, 6, 4, 2}, 0xfffc), true);
    CHECK(old.render == 14);
    CHECK(old.recorder == 1);
}

NXTEST(recorder_sibling_follows_the_least_busy_core) {
    uint64_t busy[64] = {};
    for (int b = 0; b < 16; ++b) busy[b] = 900;
    busy[4] = 100;   // console core 4's ideal processor (core 2) idles most
    busy[5] = 50;
    CHECK(pick_recorder_sibling(cores_of(8, 2), guest({12, 10, 8, 6, 4, 2}, 0xfffc), busy) == 5);
    CHECK(pick_recorder_sibling(cores_of(8, 1), guest({7, 6, 5, 4, 3, 2}, 0xfe), busy) == -1);   // no second hardware threads
}

NXTEST(never_a_guest_ideal_core) {
    const std::vector<CpuCore> c = cores_of(8, 2);
    const CpuGuest g = guest({12, 10, 8, 6, 4, 2}, 0xfffc);
    const CpuPick p = pick_render_cpus(c, g);
    CHECK(!(g.ideal >> p.render & 1));
    CHECK(!(g.ideal >> p.recorder & 1));
    CHECK(p.render / 2 != p.recorder / 2);   // two physical cores
}

NXTEST(no_smt_and_one_free_core_does_not_pin) {
    // 8 cores without SMT: core 1 is free, but core 0 has no second hardware thread for the recorder
    const CpuPick p = pick_render_cpus(cores_of(8, 1), guest({7, 6, 5, 4, 3, 2}, 0xfe));
    CHECK(p.render == -1);
    CHECK(p.recorder == -1);
}

NXTEST(no_smt_two_free_cores_pin_both) {
    const CpuPick p = pick_render_cpus(cores_of(10, 1), guest({9, 8, 7, 6, 5, 4}, 0x3fe));
    CHECK(p.render == 1);
    CHECK(p.recorder == 2);
}

NXTEST(six_cores_all_taken_by_the_guests_do_not_pin) {
    const CpuPick p = pick_render_cpus(cores_of(6, 2), guest({10, 8, 6, 4, 2, 0}, 0xfff));
    CHECK(p.render == -1);
}

NXTEST(fewer_than_four_cores_do_not_pin) {
    CpuGuest g;
    const CpuPick p = pick_render_cpus(cores_of(3, 2), g);
    CHECK(p.render == -1);
}

NXTEST(hybrid_never_takes_an_efficiency_core) {
    // 8 performance cores with SMT (class 1, processors 0..15) and 16 efficiency cores (class 0, 16..31); guests on P cores 1..6
    std::vector<CpuCore> c = cores_of(8, 2, 1);
    const std::vector<CpuCore> e = cores_of(16, 1, 0, 16);
    c.insert(c.end(), e.begin(), e.end());
    const CpuPick p = pick_render_cpus(c, guest({12, 10, 8, 6, 4, 2}, 0xfffffffc));
    CHECK(p.render == 14);
    CHECK(p.recorder == 13);
}

NXTEST(hybrid_with_every_performance_core_taken_does_not_pin) {
    // 6 performance cores (all guest ideals or core 0) and 8 efficiency cores free: no pinning onto efficiency cores
    std::vector<CpuCore> c = cores_of(6, 2, 1);
    const std::vector<CpuCore> e = cores_of(8, 1, 0, 12);
    c.insert(c.end(), e.begin(), e.end());
    const CpuPick p = pick_render_cpus(c, guest({10, 8, 6, 4, 2, 11}, 0xffffc));
    CHECK(p.render == -1);
    CHECK(p.recorder == -1);
}

NXTEST(two_ccds_prefer_the_main_threads_cache) {
    // 16 cores in two last-level caches (cores 0..7 and 8..15); guests on cores 1..6 of the first, core 7 and the second CCD free
    std::vector<CpuCore> c = cores_of(8, 2, 0, 0, 0);
    const std::vector<CpuCore> d = cores_of(8, 2, 0, 16, 1);
    c.insert(c.end(), d.begin(), d.end());
    const CpuPick p = pick_render_cpus(c, guest({12, 10, 8, 6, 4, 2}, 0xfffffffc));
    CHECK(p.render == 14);       // core 7: the main thread's cache
    CHECK(p.recorder == 16);     // then the other cache's first core
}

NXTEST(without_the_core_model_only_large_machines_pin) {
    CpuGuest none;
    CHECK(pick_render_cpus(cores_of(4, 2), none).render == -1);
    const CpuPick p = pick_render_cpus(cores_of(8, 2), none);
    CHECK(p.render == 2);
    CHECK(p.recorder == 4);
}
