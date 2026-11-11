// SPDX-License-Identifier: GPL-3.0-or-later
// Semantics adapted from SuperMarioWonderRecomp runtime/src/svc_process.cpp (MIT, Copyright (c) 2026 BelmanteGu); see THIRD_PARTY_NOTICES.md.
// Info SVCs (include/nxrt/svc.h): GetInfo 0x29, GetSystemInfo 0x6F, GetResourceLimitLimitValue 0x30, GetResourceLimitCurrentValue 0x31,
// GetResourceLimitPeakValue 0x37, GetProcessId 0x24, GetThreadId 0x25, GetProcessInfo 0x7C.
// AArch64 columns per Switchbrew: inputs in x1.., result in w0, outputs in x1.
#include <windows.h>

#include "nxrt/game.h"
#include "nxrt/abi.h"
#include "nxrt/config.h"
#include "nxrt/kernel.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"
#include "nxrt/services.h"
#include "nxrt/module.h"
#include "nxrt/svc.h"

namespace nxrt {
namespace {

constexpr uint32_t kInvalidCombination = 0xE801;   // kernel result 116 (InvalidCombination); not in kernel.h yet

// Values Switchbrew does not document (it gives names only) are nxrt choices; the comment on each says where a value comes from.
constexpr uint64_t kCoreMaskAppSwitch2 = 0x3f;                  // platform 2: cores 0..5 (title A NPDM ThreadInfo cores 0-5)
constexpr uint64_t kCoreMaskApp = 0x7;                          // nxrt: cores 0..2, the three application cores
constexpr uint64_t kPriorityMaskApp = 0x0FFFFFFFF0000000ull;    // nxrt: bits 28..59 = HOS priorities 0x1C..0x3B (kernel.h: applications 28..59)
constexpr uint64_t kAslrRegionAddress = 0x0008000000ull;       // chosen value (Switchbrew: no value given)
constexpr uint64_t kAslrRegionSize = 0x7FF8000000ull;           // chosen value (Switchbrew: no value given)
constexpr uint64_t kTotalMemorySwitch2 = 0x2A0000000ull;        // platform 2 application: 10.5 GiB (nxrt choice; title A alone pools ~8 GiB)
constexpr uint64_t kTotalMemoryAvailable = 0xCD500000ull;       // chosen value: ~3.2 GiB, like an application ([memory] app_memory_mb overrides)
constexpr uint64_t kEntropyDefaultSeed = 0x1234567890ABCDEFull; // chosen value
constexpr uint64_t kGoldenRatio64 = 0x9E3779B97F4A7C15ull;      // used to separate the four RandomEntropy subtypes
constexpr uint64_t kDefaultProcessId = 0x51;                    // chosen value (config [process] pid)
constexpr uint32_t kInfoThreadTickCountOld = 0xF0000002u;       // ThreadTickCount id on firmware 1.0.0-12.1.0 (Switchbrew)
constexpr uint32_t kInfoThreadTickCount = 25;                   // ThreadTickCount id on firmware 12.1.0+ (Switchbrew table)
constexpr uint64_t kProcessStateStarted = 2;                    // Switchbrew ProcessState 2 = Started (not debugged)
constexpr uint64_t kSubAllCores = 0xFFFFFFFFFFFFFFFFull;        // ThreadTickCount sub-argument: the total over all cores (Switchbrew)

}  // namespace
uint64_t svc_total_memory() {
    const int64_t mb = config_get_int("memory", "app_memory_mb", 0);
    if (mb > 0) return static_cast<uint64_t>(mb) << 20;
    // platform 2 applications get far more memory (12 GiB console): 10.5 GiB, inside nxrt's 12 GiB heap region.
    return game::info().platform == game::Platform::Switch2 ? kTotalMemorySwitch2 : kTotalMemoryAvailable;
}
namespace {
uint64_t used_memory() {
    uint64_t images = 0;
    for (Module* m : modules()) images += m->size();          // the kernel counts the mapped code/data of the process as used
    // plus the main thread's stack, which the kernel takes from the process's memory (main.npdm). This was an invented 64 MiB: the game
    // checks its HeapDef "Program" budget (137 MiB) against nn::os::QueryMemoryInfo's used size before the heap and aborted.
    return svc_current_heap_size() + services::profile().main_thread_stack + images;
}

// UserExceptionContextAddress: one committed kernel page, made on first use and again only if it has been unmapped.
uint64_t user_exception_context() {
    static uint64_t page = 0;
    if (page == 0 || !mem::is_mapped(page, mem::kPage)) page = mem::alloc_kernel(mem::kPage);
    return page;
}

// CPU time a thread has used, in system ticks. Win32 gives kernel + user time in 100 ns units; the split keeps the product in 64 bits.
uint64_t thread_cpu_ticks(HANDLE h) {
    if (!h) return 0;                                      // thread not started yet
    FILETIME created{}, exited{}, kernel_time{}, user_time{};
    if (!GetThreadTimes(h, &created, &exited, &kernel_time, &user_time)) return 0;
    auto to_u64 = [](const FILETIME& f) { return (static_cast<uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
    const uint64_t t100ns = to_u64(kernel_time) + to_u64(user_time);
    constexpr uint64_t kUnitsPerSecond = 10000000;
    const uint64_t hz = kernel::tick_hz();
    return (t100ns / kUnitsPerSecond) * hz + (t100ns % kUnitsPerSecond) * hz / kUnitsPerSecond;
}

// Returns the result code; `out` receives the value (0 on error).
uint32_t get_info(uint32_t type, uint32_t handle, uint64_t sub, uint64_t& out) {
    out = 0;
    switch (type) {
        case 0: out = game::info().platform == game::Platform::Switch2 ? kCoreMaskAppSwitch2 : kCoreMaskApp; return kernel::kOk;   // CoreMask
        case 1: out = kPriorityMaskApp; return kernel::kOk;                   // PriorityMask
        case 2: out = mem::kAliasBase; return kernel::kOk;                    // AliasRegionAddress
        case 3: out = mem::kAliasSize; return kernel::kOk;                    // AliasRegionSize
        case 4: out = mem::kHeapBase; return kernel::kOk;                     // HeapRegionAddress
        case 5: out = mem::kHeapMax; return kernel::kOk;                      // HeapRegionSize
        case 6: out = svc_total_memory(); return kernel::kOk;              // TotalMemorySize
        case 7: out = used_memory(); return kernel::kOk;                      // UsedMemorySize
        case 8: out = 0; return kernel::kOk;                                  // DebuggerAttached: never
        case 9: out = 0; return kernel::kOk;                                  // ResourceLimit: no handle
        case 10: out = 0; return kernel::kOk;                                 // IdleTickCount (nxrt: no idle accounting)
        case 11: {                                                            // RandomEntropy, subtype 0..3
            if (sub > (game::info().platform == game::Platform::Switch2 ? 7u : 3u)) return kInvalidCombination;   // platform 1: subtypes 0..3 (Switchbrew)
            uint64_t seed = static_cast<uint64_t>(config_get_int("process", "entropy_seed", static_cast<int64_t>(kEntropyDefaultSeed)));
            out = seed ^ (sub * kGoldenRatio64);
            return kernel::kOk;
        }
        case 12: out = kAslrRegionAddress; return kernel::kOk;                // AslrRegionAddress
        case 13: out = kAslrRegionSize; return kernel::kOk;                   // AslrRegionSize
        case 14: out = mem::kStackBase; return kernel::kOk;                   // StackRegionAddress
        case 15: out = mem::kStackSize; return kernel::kOk;                   // StackRegionSize
        case 16:                                                              // SystemResourceSizeTotal: main.npdm (title profile)
            // nn::os enables its virtual address memory manager (VAMM, AllocateAddressRegion) when this is non-zero, and titles that
            // declare it allocate through VAMM unconditionally: report what the title's NPDM says ([process] overrides).
            out = static_cast<uint64_t>(config_get_int("process", "system_resource_size", static_cast<int64_t>(services::profile().system_resource_size)));
            return kernel::kOk;
        case 17: out = 0; return kernel::kOk;                                 // SystemResourceSizeUsed
        case 18:                                                              // ProgramId: set by the game layer ([process] program_id)
            out = services::program_id_now();   // [process] program_id, else [title] program_id (default: the title the profile describes)
            return kernel::kOk;
        case 20:                                                              // UserExceptionContextAddress
            out = user_exception_context();
            return out ? kernel::kOk : kernel::kOutOfMemory;
        case 21: out = svc_total_memory(); return kernel::kOk;             // TotalNonSystemMemorySize (same as 6)
        case 22: out = used_memory(); return kernel::kOk;                     // UsedNonSystemMemorySize (same as 7)
        case 23: out = 1; return kernel::kOk;                                 // IsApplication
        case kInfoThreadTickCountOld:
        case kInfoThreadTickCount: {
            // ThreadTickCount(thread handle or pseudo handle of the calling thread, core): the CPU time the thread has used, in ticks.
            if (handle != kernel::kPseudoCurrentThread && !kernel::handle_get(handle)) return kernel::kInvalidHandle;
            // Core sub-argument: 0..3 names a console core, all-ones asks for the total over all cores (Switchbrew).
            if (sub != kSubAllCores && sub > 7) return kInvalidCombination;
            auto t = kernel::handle_as<kernel::KThread>(handle);
            if (!t) return kernel::kOk;                    // pseudo handle outside a guest thread: 0
            // nxrt pins each thread to its ideal core: that core reports the total, the other cores report 0.
            if (sub != kSubAllCores && static_cast<int64_t>(sub) != t->ideal_core) return kernel::kOk;
            // The pseudo handle is GetCurrentThread() (the calling thread); any other handle uses the thread's own host handle.
            out = thread_cpu_ticks(handle == kernel::kPseudoCurrentThread ? GetCurrentThread() : static_cast<HANDLE>(t->host_handle));
            return kernel::kOk;
        }
        case 28:                                                              // AliasRegionExtraSize (18.0.0+): sub 0
            // The extra alias space a process gets when created with the 18.0.0 flag; nxrt's alias region has none (mesosphere
            // returns the page table's extra size, 0 without the flag).
            if (sub != 0) return kInvalidCombination;
            out = 0;
            return kernel::kOk;
        // platform 2 kernels (switchbrew SVC, InfoType 29-33, 36-38). The remote region is an address range the SDK builds a
        // 64 KiB-aligned allocator over; nxrt keeps it in the free span below the HLE thunks.
        case 29: if (sub != 0) return kInvalidCombination; out = mem::kRemoteBase; return kernel::kOk;   // RemoteRegionAddress
        case 30: if (sub != 0) return kInvalidCombination; out = mem::kRemoteSize; return kernel::kOk;   // RemoteRegionSize
        case 31: case 32: out = 0; return kernel::kOk;                       // RemoteMemoryUsage / RemoteMemoryUsagePeak
        case 33: out = 0x1000; return kernel::kOk;                           // ProcessPageSize
        case 36: out = 1ull << 39; return kernel::kOk;                       // AddressSpaceSize (39-bit)
        case 37: case 38: out = 0; return kernel::kOk;                       // ShadowStackRegionAddress / Size: none
        default:
            NXLOG_W("svc", "svcGetInfo: unknown type %u sub %llu", type, static_cast<unsigned long long>(sub));
            return kInvalidCombination;
    }
}

void info_get_info(::Ctx* c) {
    uint64_t value = 0;
    uint32_t rc = get_info(static_cast<uint32_t>(reg(c, 1)), static_cast<uint32_t>(reg(c, 2)), reg(c, 3), value);
    reg(c, 1) = value;
    svc_ret(c, rc);
}

// GetSystemInfo: the handle (x2) is unused, the subtype (x3) selects the memory pool or the pid range bound.
uint32_t get_system_info(uint32_t type, uint64_t sub, uint64_t& out) {
    out = 0;
    switch (type) {
        case 0:                                                // TotalPhysicalMemorySize per pool
            switch (sub) {
                case 0: out = 0x100000000ull; return kernel::kOk;   // Application: 4 GiB (chosen value)
                case 1: out = 0x20000000ull; return kernel::kOk;    // Applet: 512 MiB (nxrt choice)
                case 2: out = 0x10000000ull; return kernel::kOk;    // System: 256 MiB (nxrt choice)
                case 3: out = 0x10000000ull; return kernel::kOk;    // SystemUnsafe: 256 MiB (nxrt choice)
                default: return kInvalidCombination;
            }
        case 1:                                                // UsedPhysicalMemorySize per pool: only the application pool is in use
            if (sub == 0) { out = used_memory(); return kernel::kOk; }
            if (sub <= 3) return kernel::kOk;                  // other pools: 0
            return kInvalidCombination;
        case 2:                                                // InitialProcessIdRange: subtype 0 lower bound, 1 upper bound
            // nxrt choice: the range is the single pid GetProcessId reports.
            if (sub == 0 || sub == 1) { out = kDefaultProcessId; return kernel::kOk; }
            return kInvalidCombination;
        default:
            return kInvalidCombination;
    }
}

void system_info_get(::Ctx* c) {
    uint64_t value = 0;
    uint32_t rc = get_system_info(static_cast<uint32_t>(reg(c, 1)), reg(c, 3), value);
    reg(c, 1) = value;
    svc_ret(c, rc);
}

// LimitableResource: 0 PhysicalMemoryMax, 1 ThreadCountMax, 2 EventCountMax, 3 TransferMemoryCountMax, 4 SessionCountMax.
bool resource_limit(uint64_t which, uint64_t& v) {
    switch (which) {
        case 0: v = kTotalMemoryAvailable; return true;   // chosen value
        case 1: v = 0x200; return true;                   // nxrt choice
        case 2: v = 0x600; return true;                   // nxrt choice
        case 3: v = 0x80; return true;                    // nxrt choice
        case 4: v = 0x80; return true;                    // nxrt choice
        default: return false;
    }
}

bool resource_current(uint64_t which, uint64_t& v) {
    switch (which) {
        case 0: v = svc_current_heap_size(); return true;
        // ThreadCountMax current: guest threads started and not yet exited; never below 1 (the calling thread).
        case 1: v = kernel::thread_count_running(); if (v < 1) v = 1; return true;
        case 2: case 3: case 4: v = 0; return true;
        default: return false;
    }
}

// Peak values are not tracked by nxrt yet: the peak equals the current value.
void resource_limit_value(::Ctx* c) {
    uint64_t v = 0;
    bool ok = resource_limit(reg(c, 2), v);
    reg(c, 1) = ok ? v : 0;
    svc_ret(c, ok ? kernel::kOk : kInvalidCombination);
}

void resource_current_value(::Ctx* c) {
    uint64_t v = 0;
    bool ok = resource_current(reg(c, 2), v);
    reg(c, 1) = ok ? v : 0;
    svc_ret(c, ok ? kernel::kOk : kInvalidCombination);
}

void resource_peak_value(::Ctx* c) {
    uint64_t v = 0;
    bool ok = resource_current(reg(c, 2), v);
    reg(c, 1) = ok ? v : 0;
    svc_ret(c, ok ? kernel::kOk : kInvalidCombination);
}

// GetProcessId(process handle): only the pseudo handle of the current process is accepted (no other process objects exist yet).
void get_process_id(::Ctx* c) {
    if (static_cast<uint32_t>(reg(c, 1)) != kernel::kPseudoCurrentProcess) {
        reg(c, 1) = 0;
        svc_ret(c, kernel::kInvalidHandle);
        return;
    }
    reg(c, 1) = static_cast<uint64_t>(config_get_int("process", "pid", static_cast<int64_t>(kDefaultProcessId)));
    svc_ret(c, kernel::kOk);
}

// GetThreadId(thread handle). The pseudo handle resolves to the calling guest thread through kernel::handle_get.
void get_thread_id(::Ctx* c) {
    auto t = kernel::handle_as<kernel::KThread>(static_cast<uint32_t>(reg(c, 1)));
    if (!t) {
        reg(c, 1) = 0;
        svc_ret(c, kernel::kInvalidHandle);
        return;
    }
    reg(c, 1) = t->id;
    svc_ret(c, kernel::kOk);
}

// GetProcessInfo(process handle, ProcessInfoType): type 0 is ProcessState. The handle is not checked (no process objects yet).
void get_process_info(::Ctx* c) {
    if (static_cast<uint32_t>(reg(c, 2)) == 0) {
        reg(c, 1) = kProcessStateStarted;
        svc_ret(c, kernel::kOk);
        return;
    }
    reg(c, 1) = 0;
    svc_ret(c, kInvalidCombination);
}

}  // namespace

void svc_register_info_group() {
    svc_register(0x29, info_get_info);
    svc_register(0x6F, system_info_get);
    svc_register(0x30, resource_limit_value);
    svc_register(0x31, resource_current_value);
    svc_register(0x37, resource_peak_value);
    svc_register(0x24, get_process_id);
    svc_register(0x25, get_thread_id);
    svc_register(0x7C, get_process_info);
}

}  // namespace nxrt
