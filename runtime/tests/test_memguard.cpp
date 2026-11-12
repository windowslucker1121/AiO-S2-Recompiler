// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_framework.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <thread>
#include <vector>

#include "nxrt/memguard.h"

NXTEST_GROUP(memguard)

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::size_t kTouchBytes = 96ull << 20;  // 96 MiB
constexpr uint64_t kHeadroom = 48ull << 20;       // limit = footprint + 48 MiB

std::atomic<bool> g_trip_flag{false};
std::atomic<int> g_calls{0};

void set_flag_cb(uint64_t, uint64_t, void*) { g_trip_flag.store(true); }
void count_cb(uint64_t, uint64_t, void*) { g_calls.fetch_add(1); }

// Commits and touches kTouchBytes of private memory; returns the block (null on failure).
void* commit_and_touch() {
    void* p = VirtualAlloc(nullptr, kTouchBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (p) std::memset(p, 0xA5, kTouchBytes);
    return p;
}

}  // namespace

// A bare test process commits well under 1 MiB, so the check is that the query is live: it is non-zero and it rises by
// the size of a 4 MiB commit.
NXTEST(footprint_tracks_private_commit) {
    const uint64_t f0 = nxrt::memguard_footprint();
    std::printf("    footprint before commit: %llu bytes\n", static_cast<unsigned long long>(f0));
    CHECK(f0 > 0);

    const std::size_t kBlock = 4ull << 20;
    void* p = VirtualAlloc(nullptr, kBlock, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    REQUIRE(p != nullptr);
    std::memset(p, 0x5A, kBlock);
    const uint64_t f1 = nxrt::memguard_footprint();
    std::printf("    footprint after 4 MiB commit: %llu bytes\n", static_cast<unsigned long long>(f1));
    CHECK(f1 >= f0 + (3ull << 20));
    CHECK(f1 > (1ull << 20));

    VirtualFree(p, 0, MEM_RELEASE);
}

NXTEST(limit_gb_parsing) {
    CHECK_EQ(nxrt::memguard_limit_gb("12.5"), 12.5);
    CHECK_EQ(nxrt::memguard_limit_gb("auto"), 0.0);   // 0: derived from the machine
    CHECK_EQ(nxrt::memguard_limit_gb("abc"), 0.0);
    CHECK_EQ(nxrt::memguard_limit_gb(""), 0.0);
    CHECK_EQ(nxrt::memguard_limit_gb(nullptr), 0.0);
    CHECK_EQ(nxrt::memguard_limit_gb("-3"), 0.0);
}

// auto: the physical memory (the footprint already has the video memory taken out), never under 8 GB.
NXTEST(auto_limit_follows_physical_memory) {
    CHECK_EQ(nxrt::memguard_auto_limit(32ull << 30), 32ull << 30);
    CHECK_EQ(nxrt::memguard_auto_limit(16ull << 30), 16ull << 30);
    CHECK_EQ(nxrt::memguard_auto_limit(4ull << 30), 8ull << 30);
}

// A peak shorter than the grace time is only logged: the game is not stopped for a big temporary buffer.
NXTEST(short_peak_within_grace_does_not_trip) {
    g_trip_flag.store(false);
    const uint64_t limit = nxrt::memguard_footprint() + kHeadroom;

    nxrt::MemGuardOptions opt;
    opt.limit_bytes = limit;
    opt.poll_ms = 20;
    opt.grace_ms = 2000;
    opt.on_exceed = &set_flag_cb;
    nxrt::memguard_start(opt);

    void* p = commit_and_touch();
    REQUIRE(p != nullptr);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    CHECK(nxrt::memguard_peak() >= limit);   // seen over the limit ...
    VirtualFree(p, 0, MEM_RELEASE);
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));
    CHECK(!g_trip_flag.load());              // ... but back under it within the grace time
    CHECK(!nxrt::memguard_tripped());

    nxrt::memguard_stop();
    CHECK(!nxrt::memguard_running());
}

namespace {
std::atomic<int> g_shed_on{0}, g_shed_off{0};
uint64_t shed_cb(bool pressure) {
    (pressure ? g_shed_on : g_shed_off).fetch_add(1);
    return 0;
}
}  // namespace

// Past the soft line the shedders run (once per episode) and memguard_pressure() is true; below it again they are told so.
NXTEST(pressure_runs_shedders) {
    static bool added = false;
    if (!added) { nxrt::memguard_add_shedder("test", &shed_cb); added = true; }
    g_shed_on.store(0);
    g_shed_off.store(0);
    nxrt::MemGuardOptions opt;
    opt.limit_bytes = nxrt::memguard_footprint() + kTouchBytes * 4;   // soft line (80%) is under footprint + 96 MiB
    opt.poll_ms = 20;
    opt.grace_ms = 60000;
    nxrt::memguard_start(opt);
    void* p = nullptr;
    // commit until past the soft line
    std::vector<void*> blocks;
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    while (!nxrt::memguard_pressure() && Clock::now() < deadline) {
        if (blocks.size() < 4) blocks.push_back(commit_and_touch());
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    CHECK(nxrt::memguard_pressure());
    CHECK_EQ(g_shed_on.load(), 1);
    for (void* b : blocks) if (b) VirtualFree(b, 0, MEM_RELEASE);
    const auto d2 = Clock::now() + std::chrono::seconds(5);
    while (nxrt::memguard_pressure() && Clock::now() < d2) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(!nxrt::memguard_pressure());
    CHECK_EQ(g_shed_off.load(), 1);
    (void)p;
    nxrt::memguard_stop();
}

NXTEST(overrun_trips_callback_and_peak) {
    g_trip_flag.store(false);
    const uint64_t limit = nxrt::memguard_footprint() + kHeadroom;

    nxrt::MemGuardOptions opt;
    opt.limit_bytes = limit;
    opt.poll_ms = 20;
    opt.grace_ms = 0;
    opt.on_exceed = &set_flag_cb;
    nxrt::memguard_start(opt);

    void* p = commit_and_touch();
    REQUIRE(p != nullptr);

    const auto deadline = Clock::now() + std::chrono::seconds(5);
    while (!g_trip_flag.load() && Clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(g_trip_flag.load());
    CHECK(nxrt::memguard_tripped());
    CHECK(nxrt::memguard_peak() >= limit);

    VirtualFree(p, 0, MEM_RELEASE);
    nxrt::memguard_stop();
    CHECK(!nxrt::memguard_running());
}

NXTEST(start_stop_start_again) {
    nxrt::MemGuardOptions opt;
    opt.limit_bytes = 1ull << 60;  // never reached
    opt.poll_ms = 20;

    nxrt::memguard_start(opt);
    CHECK(nxrt::memguard_running());
    nxrt::memguard_stop();
    CHECK(!nxrt::memguard_running());

    nxrt::memguard_start(opt);
    CHECK(nxrt::memguard_running());
    nxrt::memguard_stop();
    CHECK(!nxrt::memguard_running());
}

NXTEST(stop_is_prompt_with_long_poll) {
    nxrt::MemGuardOptions opt;
    opt.limit_bytes = 1ull << 60;
    opt.poll_ms = 5000;

    nxrt::memguard_start(opt);
    CHECK(nxrt::memguard_running());
    const auto t0 = Clock::now();
    nxrt::memguard_stop();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
    CHECK_MSG(ms < 500, "stop took " + std::to_string(ms) + " ms");
    CHECK(!nxrt::memguard_running());
}

NXTEST(on_exceed_called_exactly_once) {
    g_calls.store(0);
    const uint64_t limit = nxrt::memguard_footprint() + kHeadroom;

    nxrt::MemGuardOptions opt;
    opt.limit_bytes = limit;
    opt.poll_ms = 20;
    opt.grace_ms = 0;
    opt.on_exceed = &count_cb;
    nxrt::memguard_start(opt);

    void* p = commit_and_touch();
    REQUIRE(p != nullptr);

    const auto deadline = Clock::now() + std::chrono::seconds(5);
    while (g_calls.load() == 0 && Clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    // Keep the footprint over the limit for another second; the callback must not fire again.
    std::this_thread::sleep_for(std::chrono::seconds(1));
    CHECK_EQ(g_calls.load(), 1);

    VirtualFree(p, 0, MEM_RELEASE);
    nxrt::memguard_stop();
    CHECK(!nxrt::memguard_running());
}

NXTEST(second_start_is_ignored) {
    nxrt::MemGuardOptions opt;
    opt.limit_bytes = 1ull << 60;
    opt.poll_ms = 20;

    nxrt::memguard_start(opt);
    nxrt::memguard_start(opt);  // ignored: must not spawn a second guard or crash
    CHECK(nxrt::memguard_running());
    nxrt::memguard_stop();
    CHECK(!nxrt::memguard_running());
}
