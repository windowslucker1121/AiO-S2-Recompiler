// SPDX-License-Identifier: GPL-3.0-or-later
// Memory guard: a background thread polls the process's footprint (private commit minus the video memory the process has in use) and the
// system's free memory. It is there to end a runaway (a leak, an unbounded cache) with a report before the machine starts swapping or
// allocations fail at random places, not to stop a game for a short peak:
//   - pressure (footprint past the soft line, or the system short of physical memory): one warning with the numbers, the registered
//     shedders drop what they can (memguard_add_shedder), and memguard_pressure() is true (work that would allocate a lot waits);
//   - stop: the footprint past the limit, or the system's commit nearly used up, without a break for `grace_s` seconds; then the
//     report (crash_report_path(), reason "memory limit"), and the process ends with exit code 3 (the library explains that code).
// Config ([memory]): limit_gb = auto (default: the machine's physical memory; a number = a fixed limit in GB), grace_s (10),
// soft_percent (80), poll_ms (250). Details: runtime/docs/CONFIG.md.
#pragma once
#include <cstdint>
#include <string>

namespace nxrt {

struct MemGuardOptions {
    uint64_t limit_bytes = 0;   // 0: [memory] limit_gb from the config (auto: derived from the machine)
    uint32_t poll_ms = 0;       // 0: [memory] poll_ms (default 250)
    int32_t grace_ms = -1;      // how long the footprint must stay over the limit before on_exceed; -1: [memory] grace_s (default 10 s)
    // Called once from the guard thread when the footprint stayed over the limit for the grace time (it must not call memguard_stop: that
    // joins this thread). Null = default (write the report, terminate the process with exit code 3).
    void (*on_exceed)(uint64_t footprint, uint64_t limit, void* user) = nullptr;
    void* user = nullptr;
};

uint64_t memguard_footprint();       // bytes of private commit of this process (0 if the query fails)
// Private commit includes the video memory a process has allocated (WDDM keeps a commit charge for it). The guard is about host memory, so it
// subtracts what this function returns (the GPU's dedicated usage of this process; the Vulkan device sets it). Null: no subtraction.
// Without it a game at 4K, with 5 GB of textures and render targets in video memory, ran into the 8 GB limit with only 3 GB of host memory.
void memguard_set_discount(uint64_t (*gpu_bytes)());
uint64_t memguard_footprint_host();  // memguard_footprint() minus the discount (what the guard compares with the limit)
// "auto", empty, null, zero, negative or not a number: 0 (= derive the limit from the machine); otherwise the number of GB.
double memguard_limit_gb(const char* text);
// The limit `auto` gives on a machine with `physical_bytes` of RAM: all of it (the footprint has the video memory taken out), at least 8 GB.
uint64_t memguard_auto_limit(uint64_t physical_bytes);
void memguard_start(const MemGuardOptions& opt = {});
void memguard_stop();                // joins the guard thread
bool memguard_running();
uint64_t memguard_peak();            // highest footprint seen by the guard
uint64_t memguard_limit();           // the limit in force (0: guard not running)
bool memguard_tripped();             // on_exceed has fired
bool memguard_pressure();            // the guard sees memory pressure now (see above); false while it is not running

// A shedder frees what it can when memory pressure begins (`pressure` true) and may restore its normal budgets when it ends (false).
// Returns the bytes it freed (an estimate, for the log). Called on the guard thread; must not block for long or call back into the guard.
using MemShedFn = uint64_t (*)(bool pressure);
void memguard_add_shedder(const char* name, MemShedFn fn);

}  // namespace nxrt
