// SPDX-License-Identifier: GPL-3.0-or-later
#include "test_framework.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

#include "nxrt/abi.h"
#include "nxrt/config.h"
#include "nxrt/diag.h"
#include "nxrt/kernel.h"
#include "nxrt/memory.h"
#include "nxrt/services.h"
#include "nxrt/svc.h"
#include "test_platform.h"
NXTEST_GROUP(svc_info)

namespace {

struct Result {
    uint32_t rc;
    uint64_t x1;
};

// Dispatches one info SVC through the table with a fresh context: x1..x3 are the SVC inputs as documented by Switchbrew.
Result call(uint32_t svc, uint64_t x1, uint64_t x2 = 0, uint64_t x3 = 0) {
    nxrt::svc_init();
    ::Ctx c{};
    c.x[1] = x1;
    c.x[2] = x2;
    c.x[3] = x3;
    nxrt::svc_dispatch(&c, svc);
    return {static_cast<uint32_t>(c.x[0]), c.x[1]};
}

constexpr uint32_t kSvcGetInfo = 0x29;
constexpr uint32_t kSvcGetSystemInfo = 0x6F;
constexpr uint32_t kSvcLimitValue = 0x30;
constexpr uint32_t kSvcCurrentValue = 0x31;
constexpr uint32_t kSvcPeakValue = 0x37;
constexpr uint32_t kSvcGetProcessId = 0x24;
constexpr uint32_t kSvcGetThreadId = 0x25;
constexpr uint32_t kSvcGetProcessInfo = 0x7C;
constexpr uint64_t kPseudoProcess = 0xFFFF8001;
constexpr uint64_t kPseudoThread = 0xFFFF8000;
constexpr uint64_t kBogusHandle = 0x12345;
constexpr uint32_t kInfoThreadTickCountOld = 0xF0000002u;
constexpr uint32_t kInfoThreadTickCount = 25;
constexpr uint64_t kInvalidCombination = 0xE801;
constexpr uint64_t kInvalidHandle = 0xE401;
constexpr uint64_t kTotalMemory = 0xCD500000ull;          // platform 1 application (svc_info.cpp kTotalMemoryAvailable)
constexpr uint64_t kTotalMemorySwitch2 = 0x2A0000000ull;  // platform 2 application: 10.5 GiB
constexpr uint64_t kGolden = 0x9E3779B97F4A7C15ull;
constexpr uint64_t kAllCores = 0xFFFFFFFFFFFFFFFFull;

// Used memory (GetInfo 7 and 22, GetSystemInfo 1): the heap plus the main thread's stack, which the kernel takes from the process's
// memory (the title's main.npdm, services::TitleProfile::main_thread_stack), plus the mapped modules (none in these tests).
uint64_t used_memory() { return nxrt::svc_current_heap_size() + nxrt::services::profile().main_thread_stack; }

}  // namespace

// GetInfo: InfoType 0 to 23 with their documented or chosen values.

NXTEST(info_core_and_priority_masks) {
    nxtest::ScopedPlatform sw1(nxrt::game::Platform::Switch1);   // three application cores (the platform 2 mask is checked below)
    Result r = call(kSvcGetInfo, 0);
    CHECK_EQ(r.rc, 0u);
    CHECK_EQ(r.x1, 0x7u);
    r = call(kSvcGetInfo, 1);
    CHECK_EQ(r.rc, 0u);
    CHECK_EQ(r.x1, 0x0FFFFFFFF0000000ull);
}

NXTEST(info_alias_and_heap_regions) {
    Result r = call(kSvcGetInfo, 2);
    CHECK_EQ(r.rc, 0u);
    CHECK_EQ(r.x1, nxrt::mem::kAliasBase);
    r = call(kSvcGetInfo, 3);
    CHECK_EQ(r.rc, 0u);
    CHECK_EQ(r.x1, nxrt::mem::kAliasSize);
    r = call(kSvcGetInfo, 4);
    CHECK_EQ(r.rc, 0u);
    CHECK_EQ(r.x1, nxrt::mem::kHeapBase);
    r = call(kSvcGetInfo, 5);
    CHECK_EQ(r.rc, 0u);
    CHECK_EQ(r.x1, nxrt::mem::kHeapMax);
}

NXTEST(info_total_and_used_memory) {
    nxtest::ScopedPlatform sw1(nxrt::game::Platform::Switch1);   // the platform 1 application size (platform 2: info_switch2_total_memory)
    Result r = call(kSvcGetInfo, 6);
    CHECK_EQ(r.rc, 0u);
    CHECK_EQ(r.x1, kTotalMemory);
    uint64_t used = used_memory();
    r = call(kSvcGetInfo, 7);
    CHECK_EQ(r.rc, 0u);
    CHECK_EQ(r.x1, used);
    r = call(kSvcGetInfo, 22);   // UsedNonSystemMemorySize is the same value
    CHECK_EQ(r.rc, 0u);
    CHECK_EQ(r.x1, used);
    r = call(kSvcGetInfo, 21);   // TotalNonSystemMemorySize is the same value
    CHECK_EQ(r.rc, 0u);
    CHECK_EQ(r.x1, kTotalMemory);
}

NXTEST(info_switch2_total_memory) {
    nxtest::ScopedPlatform sw2(nxrt::game::Platform::Switch2);
    CHECK_EQ(call(kSvcGetInfo, 6).x1, kTotalMemorySwitch2);
    CHECK_EQ(call(kSvcGetInfo, 21).x1, kTotalMemorySwitch2);
    CHECK_EQ(call(kSvcGetInfo, 7).x1, used_memory());
}

// The main thread's stack counts as used: a title whose stack differs from the default changes the used size by the difference.
NXTEST(info_used_memory_follows_the_main_thread_stack) {
    nxrt::config_set("title", "main_thread_stack", "0x200000");
    nxrt::services::profile_reload();
    const uint64_t a = call(kSvcGetInfo, 7).x1;
    nxrt::config_set("title", "main_thread_stack", "0x600000");
    nxrt::services::profile_reload();
    const uint64_t b = call(kSvcGetInfo, 7).x1;
    CHECK_EQ(b - a, 0x400000ull);
    CHECK_EQ(b, nxrt::svc_current_heap_size() + 0x600000ull);
    nxrt::config_clear();
    nxrt::services::profile_reload();
}

NXTEST(info_debugger_resource_limit_idle_zero) {
    for (uint64_t t : {8ull, 9ull, 10ull}) {
        Result r = call(kSvcGetInfo, t);
        CHECK_EQ(r.rc, 0u);
        CHECK_EQ(r.x1, 0u);
    }
}

NXTEST(info_random_entropy_subtypes_distinct_and_stable) {
    uint64_t v[4] = {};
    for (uint64_t s = 0; s < 4; s++) {
        Result r = call(kSvcGetInfo, 11, 0, s);
        CHECK_EQ(r.rc, 0u);
        v[s] = r.x1;
        // Stable between calls.
        CHECK_EQ(call(kSvcGetInfo, 11, 0, s).x1, v[s]);
    }
    for (int i = 0; i < 4; i++)
        for (int j = i + 1; j < 4; j++) CHECK_MSG(v[i] != v[j], "RandomEntropy subtypes must differ");
    // Subtype 0 is the seed itself, the others differ from it by the golden-ratio step.
    uint64_t seed = static_cast<uint64_t>(nxrt::config_get_int("process", "entropy_seed", 0x1234567890ABCDEFll));
    CHECK_EQ(v[0], seed);
    CHECK_EQ(v[1], seed ^ kGolden);
}

NXTEST(info_random_entropy_bad_subtype) {
    nxtest::ScopedPlatform sw1(nxrt::game::Platform::Switch1);   // four subtypes on platform 1, eight on platform 2 (below)
    Result r = call(kSvcGetInfo, 11, 0, 4);
    CHECK_EQ(r.rc, kInvalidCombination);
    CHECK_EQ(r.x1, 0u);
}

// platform 2 profile: six application cores (mask 0x3f) and eight RandomEntropy subtypes / core numbers (platform 1: three cores, four subtypes).
NXTEST(info_switch2_core_mask_entropy_subtypes_and_core_numbers) {
    nxtest::ScopedPlatform sw2(nxrt::game::Platform::Switch2);
    Result r = call(kSvcGetInfo, 0);
    CHECK_EQ(r.rc, 0u);
    CHECK_EQ(r.x1, 0x3Full);
    uint64_t v[8] = {};
    for (uint64_t s = 0; s < 8; s++) {
        r = call(kSvcGetInfo, 11, 0, s);
        CHECK_EQ(r.rc, 0u);
        v[s] = r.x1;
    }
    for (int i = 0; i < 8; i++)
        for (int j = i + 1; j < 8; j++) CHECK_MSG(v[i] != v[j], "RandomEntropy subtypes must differ");
    r = call(kSvcGetInfo, 11, 0, 8);
    CHECK_EQ(r.rc, kInvalidCombination);
    CHECK_EQ(call(kSvcGetInfo, kInfoThreadTickCount, kPseudoThread, 5).rc, 0u);
    CHECK_EQ(call(kSvcGetInfo, kInfoThreadTickCount, kPseudoThread, 8).rc, kInvalidCombination);
}

NXTEST(info_aslr_and_stack_regions) {
    Result r = call(kSvcGetInfo, 12);
    CHECK_EQ(r.rc, 0u);
    CHECK_EQ(r.x1, 0x0008000000ull);
    r = call(kSvcGetInfo, 13);
    CHECK_EQ(r.rc, 0u);
    CHECK_EQ(r.x1, 0x7FF8000000ull);
    r = call(kSvcGetInfo, 14);
    CHECK_EQ(r.rc, 0u);
    CHECK_EQ(r.x1, nxrt::mem::kStackBase);
    r = call(kSvcGetInfo, 15);
    CHECK_EQ(r.rc, 0u);
    CHECK_EQ(r.x1, nxrt::mem::kStackSize);
}

NXTEST(info_system_resource_and_program_id_override) {
    // SystemResourceSize is the title's (main.npdm; oot 0x3000000, which turns on nn::os's address manager), and 0 without one.
    nxrt::config_clear();
    nxrt::services::profile_reload();
    Result r = call(kSvcGetInfo, 16);
    CHECK_EQ(r.rc, 0u);
    CHECK_EQ(r.x1, nxrt::services::profile().system_resource_size);
    nxrt::config_set("title", "system_resource_size", "0");
    nxrt::services::profile_reload();
    CHECK_EQ(call(kSvcGetInfo, 16).x1, 0u);
    nxrt::config_set("title", "system_resource_size", "0x1000000");
    nxrt::services::profile_reload();
    CHECK_EQ(call(kSvcGetInfo, 16).x1, 0x1000000u);
    nxrt::config_clear();
    nxrt::services::profile_reload();
    r = call(kSvcGetInfo, 17);
    CHECK_EQ(r.rc, 0u);
    CHECK_EQ(r.x1, 0u);
    // ProgramId comes from the config; restore the default afterwards so other cases see 0.
    nxrt::config_set("process", "program_id", "0x010012101468C000");
    r = call(kSvcGetInfo, 18);
    CHECK_EQ(r.rc, 0u);
    CHECK_EQ(r.x1, 0x010012101468C000ull);
    nxrt::config_set("process", "program_id", "0");
}

NXTEST(info_user_exception_context_page) {
    nxrt::mem::init();
    Result a = call(kSvcGetInfo, 20);
    CHECK_EQ(a.rc, 0u);
    CHECK(a.x1 != 0);
    CHECK_EQ(a.x1 % nxrt::mem::kPage, 0u);
    CHECK(nxrt::mem::is_mapped(a.x1, nxrt::mem::kPage));
    // Made once: the same page on the next call.
    CHECK_EQ(call(kSvcGetInfo, 20).x1, a.x1);
}

NXTEST(info_application_flags) {
    Result r = call(kSvcGetInfo, 23);
    CHECK_EQ(r.rc, 0u);
    CHECK_EQ(r.x1, 1u);
}

NXTEST(info_thread_tick_count_bad_handle) {
    Result r = call(kSvcGetInfo, kInfoThreadTickCountOld, kBogusHandle, 0xFFFFFFFFFFFFFFFFull);
    CHECK_EQ(r.rc, kInvalidHandle);
    CHECK_EQ(r.x1, 0u);
    // The newer id (25) takes the same path.
    CHECK_EQ(call(kSvcGetInfo, 25, kBogusHandle, 0).rc, kInvalidHandle);
}

NXTEST(info_thread_tick_count_pseudo_handle) {
    // Pseudo handle of the calling thread: accepted even outside a guest thread, where the tick value is 0.
    Result r = call(kSvcGetInfo, kInfoThreadTickCountOld, kPseudoThread, 0xFFFFFFFFFFFFFFFFull);
    CHECK_EQ(r.rc, 0u);
}

namespace {
// Burns host CPU for about `ms` milliseconds (a CPU-bound loop, so its CPU time is about its wall time).
void burn_cpu(int ms) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    volatile uint64_t x = 0;
    while (std::chrono::steady_clock::now() < end) x = x + 1;
}
}  // namespace

NXTEST(info_thread_tick_count_pseudo_handle_counts_cpu) {
    // A guest thread (adopted on a helper host thread) burns about 20 ms of CPU: its tick count is positive and never goes down.
    std::atomic<uint32_t> adopted{0};
    std::atomic<uint32_t> rc_ok{0xFFFF};
    std::atomic<uint64_t> first{0}, second{0}, third{0};
    std::thread th([&] {
        adopted = nxrt::kernel::main_thread_adopt(0x100000);
        Result a = call(kSvcGetInfo, kInfoThreadTickCount, kPseudoThread, kAllCores);
        burn_cpu(20);
        Result b = call(kSvcGetInfo, kInfoThreadTickCount, kPseudoThread, kAllCores);
        // Windows adds a thread's CPU time at its clock tick (15.6 ms): on a busy machine 20 ms of burning can still read 0, so burn on (2 s at most).
        for (int i = 0; i < 100 && b.rc == 0 && b.x1 == 0; ++i) {
            burn_cpu(20);
            b = call(kSvcGetInfo, kInfoThreadTickCount, kPseudoThread, kAllCores);
        }
        Result c = call(kSvcGetInfo, kInfoThreadTickCount, kPseudoThread, kAllCores);
        rc_ok = (a.rc == 0 && b.rc == 0 && c.rc == 0) ? 0u : 1u;
        first = a.x1;
        second = b.x1;
        third = c.x1;
        nxrt::diag_unregister_thread();
    });
    th.join();
    CHECK(adopted.load() != 0);
    CHECK_EQ(rc_ok.load(), 0u);
    CHECK(second.load() > 0);
    CHECK(first.load() <= second.load());
    CHECK(third.load() >= second.load());
    nxrt::kernel::kernel_reset();   // forget the helper thread so later cases see no running guest thread
}

NXTEST(info_thread_tick_count_invalid_handle_any_sub) {
    // The handle is checked before the core sub-argument, so a bad handle is rejected whatever sub says.
    CHECK_EQ(call(kSvcGetInfo, kInfoThreadTickCount, kBogusHandle, kAllCores).rc, kInvalidHandle);
    CHECK_EQ(call(kSvcGetInfo, kInfoThreadTickCount, kBogusHandle, 0).rc, kInvalidHandle);
    CHECK_EQ(call(kSvcGetInfo, kInfoThreadTickCount, kBogusHandle, 9).rc, kInvalidHandle);
}

NXTEST(info_unknown_type) {
    Result r = call(kSvcGetInfo, 99);
    CHECK_EQ(r.rc, kInvalidCombination);
    CHECK_EQ(r.x1, 0u);
    r = call(kSvcGetInfo, 0x1234);
    CHECK_EQ(r.rc, kInvalidCombination);
}

// GetSystemInfo

NXTEST(system_info_total_physical_memory_per_pool) {
    Result r = call(kSvcGetSystemInfo, 0, 0, 0);
    CHECK_EQ(r.rc, 0u);
    CHECK_EQ(r.x1, 0x100000000ull);
    r = call(kSvcGetSystemInfo, 0, 0, 1);
    CHECK_EQ(r.rc, 0u);
    CHECK_EQ(r.x1, 0x20000000ull);
    r = call(kSvcGetSystemInfo, 0, 0, 4);   // no such pool
    CHECK_EQ(r.rc, kInvalidCombination);
}

NXTEST(system_info_used_physical_memory_and_pid_range) {
    uint64_t used = used_memory();
    Result r = call(kSvcGetSystemInfo, 1, 0, 0);
    CHECK_EQ(r.rc, 0u);
    CHECK_EQ(r.x1, used);
    r = call(kSvcGetSystemInfo, 1, 0, 2);   // System pool: not used by the application
    CHECK_EQ(r.rc, 0u);
    CHECK_EQ(r.x1, 0u);
    r = call(kSvcGetSystemInfo, 2, 0, 0);
    CHECK_EQ(r.rc, 0u);
    CHECK_EQ(r.x1, 0x51u);
    r = call(kSvcGetSystemInfo, 2, 0, 1);
    CHECK_EQ(r.rc, 0u);
    CHECK_EQ(r.x1, 0x51u);
}

NXTEST(system_info_unknown_type) {
    Result r = call(kSvcGetSystemInfo, 7, 0, 0);
    CHECK_EQ(r.rc, kInvalidCombination);
    CHECK_EQ(r.x1, 0u);
}

// Resource limits

NXTEST(resource_limit_values) {
    Result r = call(kSvcLimitValue, 0x1234, 0);   // limit of PhysicalMemoryMax
    CHECK_EQ(r.rc, 0u);
    CHECK_EQ(r.x1, kTotalMemory);
    CHECK_EQ(call(kSvcLimitValue, 0x1234, 1).x1, 0x200u);   // ThreadCountMax
    CHECK_EQ(call(kSvcLimitValue, 0x1234, 2).x1, 0x600u);   // EventCountMax
    CHECK_EQ(call(kSvcLimitValue, 0x1234, 3).x1, 0x80u);    // TransferMemoryCountMax
    CHECK_EQ(call(kSvcLimitValue, 0x1234, 4).x1, 0x80u);    // SessionCountMax
    CHECK_EQ(call(kSvcLimitValue, 0x1234, 5).rc, kInvalidCombination);
}

NXTEST(resource_current_values) {
    Result r = call(kSvcCurrentValue, 0x1234, 0);
    CHECK_EQ(r.rc, 0u);
    CHECK_EQ(r.x1, nxrt::svc_current_heap_size());
    // Thread count: started, not yet exited guest threads, at least 1. No guest thread runs in this group, so it is 1.
    CHECK_EQ(call(kSvcCurrentValue, 0x1234, 1).x1, 1u);
    CHECK_EQ(call(kSvcCurrentValue, 0x1234, 2).x1, 0u);
    CHECK_EQ(call(kSvcCurrentValue, 0x1234, 5).rc, kInvalidCombination);
}

NXTEST(resource_current_thread_count_at_least_one) {
    // The test thread is not a guest thread, so the count can be 0 here; the floor of 1 applies.
    Result r = call(kSvcCurrentValue, 0x1234, 1);
    CHECK_EQ(r.rc, 0u);
    CHECK(r.x1 >= 1);
}

NXTEST(resource_peak_values) {
    // Peak is not tracked yet: it equals the current value for each resource.
    Result r = call(kSvcPeakValue, 0x1234, 0);
    CHECK_EQ(r.rc, 0u);
    CHECK_EQ(r.x1, nxrt::svc_current_heap_size());
    CHECK_EQ(call(kSvcPeakValue, 0x1234, 1).x1, 1u);
    CHECK_EQ(call(kSvcPeakValue, 0x1234, 6).rc, kInvalidCombination);
}

// Process and thread ids and process info

NXTEST(process_id_pseudo_handle) {
    Result r = call(kSvcGetProcessId, kPseudoProcess);
    CHECK_EQ(r.rc, 0u);
    CHECK_EQ(r.x1, 0x51u);
    r = call(kSvcGetProcessId, kBogusHandle);
    CHECK_EQ(r.rc, kInvalidHandle);
}

NXTEST(thread_id_invalid_handle) {
    // The success path needs thread objects, which do not exist on this branch yet, so only the failure is checked here.
    Result r = call(kSvcGetThreadId, kBogusHandle);
    CHECK_EQ(r.rc, kInvalidHandle);
    CHECK_EQ(r.x1, 0u);
}

NXTEST(process_info_state_and_bad_type) {
    Result r = call(kSvcGetProcessInfo, kPseudoProcess, 0);   // ProcessState: Started
    CHECK_EQ(r.rc, 0u);
    CHECK_EQ(r.x1, 2u);
    r = call(kSvcGetProcessInfo, kPseudoProcess, 1);
    CHECK_EQ(r.rc, kInvalidCombination);
    CHECK_EQ(r.x1, 0u);
}
