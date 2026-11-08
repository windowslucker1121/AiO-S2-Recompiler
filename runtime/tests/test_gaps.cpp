// SPDX-License-Identifier: GPL-3.0-or-later
// Gap counters (nxrt/gaps.h): gap() and the per-call-site GapCounter land in the same store entry with the right counts.
#include "test_framework.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "nxrt/gaps.h"
NXTEST_GROUP(gaps)

namespace {
uint64_t count_of(const char* area, const char* what, const char* detail) {
    for (const nxrt::GapRecord& r : nxrt::gaps_snapshot())
        if (r.area == area && r.what == what && r.detail == detail) return r.count;
    return 0;
}
}  // namespace

NXTEST(gap_counts_every_call) {
    for (int i = 0; i < 5; ++i) nxrt::gap("test-gaps", "plain", "a");
    nxrt::gap_n("test-gaps", "plain", "a", 10);
    CHECK_EQ(count_of("test-gaps", "plain", "a"), 15u);
}

NXTEST(counter_reaches_the_store_on_snapshot) {
    static nxrt::GapCounter c("test-gaps", "hot", "b");
    for (int i = 0; i < 10000; ++i) c.hit();   // first hit, two flushes at 4096 and 8192, the rest at the snapshot
    CHECK_EQ(count_of("test-gaps", "hot", "b"), 10000u);
    for (int i = 0; i < 3; ++i) c.hit();
    CHECK_EQ(count_of("test-gaps", "hot", "b"), 10003u);
}

NXTEST(counter_and_gap_share_an_entry_across_threads) {
    static nxrt::GapCounter c("test-gaps", "shared", "c");
    std::vector<std::thread> ts;
    for (int t = 0; t < 4; ++t)
        ts.emplace_back([] { for (int i = 0; i < 5000; ++i) c.hit(); });
    for (auto& t : ts) t.join();
    nxrt::gap("test-gaps", "shared", "c");
    CHECK_EQ(count_of("test-gaps", "shared", "c"), 20001u);
}

NXTEST(counter_is_cheaper_than_gap) {   // informational: the cost per call of both paths on this machine
    using C = std::chrono::steady_clock;
    constexpr int kN = 200000;
    const auto t0 = C::now();
    for (int i = 0; i < kN; ++i) nxrt::gap("test-gaps", "timed gap", std::string("0123456789abcdef"));
    const auto t1 = C::now();
    static nxrt::GapCounter c("test-gaps", "timed counter", "0123456789abcdef");
    for (int i = 0; i < kN; ++i) c.hit();
    const auto t2 = C::now();
    const double g = std::chrono::duration<double, std::nano>(t1 - t0).count() / kN, h = std::chrono::duration<double, std::nano>(t2 - t1).count() / kN;
    std::printf("    gap() %.1f ns per call, GapCounter::hit() %.1f ns per call\n", g, h);
    CHECK_EQ(count_of("test-gaps", "timed counter", "0123456789abcdef"), static_cast<uint64_t>(kN));
}