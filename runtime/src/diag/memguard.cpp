// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from SuperMarioWonderRecomp runtime/src/memguard_win.cpp (MIT, Copyright (c) 2026 BelmanteGu); see THIRD_PARTY_NOTICES.md.
#include "nxrt/game.h"
#include "nxrt/memguard.h"

#include "nxrt/config.h"
#include "nxrt/diag.h"
#include "nxrt/gauge.h"
#include "nxrt/log.h"

#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace nxrt {
namespace {

using Clock = std::chrono::steady_clock;

struct Settings {
    uint64_t limit;       // stop line for the footprint
    uint64_t soft;        // pressure line for the footprint
    bool auto_limit;      // derived from the machine
    uint32_t poll_ms;
    uint32_t grace_ms;
    void (*on_exceed)(uint64_t footprint, uint64_t limit, void* user);
    void* user;
};

std::mutex g_ctl;             // serialises start / stop / running
std::mutex g_mu;              // guards g_stop and the poll wait
std::condition_variable g_cv;
bool g_stop = false;          // guarded by g_mu
std::thread g_thread;         // guarded by g_ctl
std::atomic<uint64_t> g_peak{0};
std::atomic<uint64_t> g_limit{0};
std::atomic<bool> g_tripped{false};
std::atomic<bool> g_pressure{false};
std::atomic<uint64_t (*)()> g_discount{nullptr};

std::mutex g_shed_mu;
std::vector<std::pair<std::string, MemShedFn>> g_shedders;   // guarded by g_shed_mu

constexpr double kBytesPerGb = 1073741824.0;
constexpr uint64_t kGb = 1ull << 30;
constexpr uint64_t kMb = 1ull << 20;

uint64_t gb_to_bytes(double gb) {
    const double b = gb * kBytesPerGb;
    if (b >= static_cast<double>(std::numeric_limits<uint64_t>::max())) return std::numeric_limits<uint64_t>::max();
    return static_cast<uint64_t>(b);
}

double gbd(uint64_t b) { return static_cast<double>(b) / kBytesPerGb; }

void raise_peak(uint64_t f) {
    uint64_t p = g_peak.load();
    while (f > p && !g_peak.compare_exchange_weak(p, f)) {
    }
}

struct SysMem {
    uint64_t phys_total = 0, phys_avail = 0, commit_limit = 0, commit_avail = 0;
};

SysMem sys_mem() {
    MEMORYSTATUSEX m{};
    m.dwLength = sizeof m;
    SysMem s;
    if (GlobalMemoryStatusEx(&m)) {
        s.phys_total = m.ullTotalPhys;
        s.phys_avail = m.ullAvailPhys;
        s.commit_limit = m.ullTotalPageFile;   // the system commit limit (RAM + page files), not only the page file
        s.commit_avail = m.ullAvailPageFile;
    }
    return s;
}

struct ProcMem {
    uint64_t priv = 0, ws = 0, peak_ws = 0;
};

ProcMem proc_mem() {
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof pmc;
    ProcMem p;
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof pmc)) {
        p.priv = pmc.PrivateUsage;
        p.ws = pmc.WorkingSetSize;
        p.peak_ws = pmc.PeakWorkingSetSize;
    }
    return p;
}

// One line with every number the guard decides on (for the warnings and the report).
std::string numbers_line(uint64_t footprint) {
    const ProcMem p = proc_mem();
    const SysMem s = sys_mem();
    char b[512];
    std::snprintf(b, sizeof b,
                  "footprint %.2f GB (private commit %.2f GB minus %.2f GB of video memory), working set %.2f GB; system: %.1f of %.1f GB "
                  "physical memory free, %.1f of %.1f GB commit free",
                  gbd(footprint), gbd(p.priv), gbd(p.priv > footprint ? p.priv - footprint : 0), gbd(p.ws), gbd(s.phys_avail), gbd(s.phys_total),
                  gbd(s.commit_avail), gbd(s.commit_limit));
    return b;
}

// The commit the system has left is about to run out: allocations (ours and every other program's) start failing.
bool commit_nearly_gone(const SysMem& s, uint64_t floor_bytes) {
    if (!s.commit_limit) return false;
    return s.commit_avail < std::max<uint64_t>(floor_bytes, s.commit_limit / 50);
}

uint64_t run_shedders(bool pressure) {
    std::vector<std::pair<std::string, MemShedFn>> list;
    {
        std::lock_guard<std::mutex> lk(g_shed_mu);
        list = g_shedders;
    }
    uint64_t freed = 0;
    for (const auto& [name, fn] : list) {
        const uint64_t n = fn ? fn(pressure) : 0;
        freed += n;
        if (pressure && n) NXLOG_I("memory", "  %s dropped %.0f MB", name.c_str(), static_cast<double>(n) / static_cast<double>(kMb));
    }
    return freed;
}

// The memory-limit report: same file as the crash report. The core of it (reason, footprint, frame times) is written first; the gauges follow
// once read, on a helper thread because a gauge callback takes the lock of the thing it counts and that lock may be held by a stuck thread.
void write_limit_report(uint64_t footprint, uint64_t limit, const std::string& why) {
    const ProcMem pm = proc_mem();
    char b[1024];
    std::string text;
    std::snprintf(b, sizeof b, "MEMORY LIMIT EXCEEDED (reason: memory limit)\n  %s\n", why.c_str());
    text += b;
    std::snprintf(b, sizeof b, "  %s\n  peak working set %.2f GB, peak footprint %.2f GB, limit %.1f GB ([memory] limit_gb)\n", numbers_line(footprint).c_str(),
                  gbd(pm.peak_ws), gbd(g_peak.load()), gbd(limit));
    text += b;
    text += "  What helps: close other programs, or let Windows manage the page file size (it then grows when needed). The limit is set with\n"
            "  [memory] limit_gb in the game's settings file (auto = this PC's physical memory).\n";
    std::snprintf(b, sizeof b, "  presented frames: %llu\n", static_cast<unsigned long long>(diag_frame_count()));
    text += b;
    text += frame_times_summary(30.0);

    const std::string path = crash_report_path();
    auto write = [&path](const std::string& t) {
        if (path.empty()) return;
        FILE* f = std::fopen(path.c_str(), "wb");
        if (!f) return;
        std::fwrite(t.data(), 1, t.size(), f);
        std::fclose(f);
    };
    auto log_text = [](const std::string& t) {
        for (size_t pos = 0; pos < t.size();) {
            size_t eol = t.find('\n', pos);
            if (eol == std::string::npos) eol = t.size();
            NXLOG_E("memory", "%.*s", static_cast<int>(eol - pos), t.data() + pos);
            pos = eol + 1;
        }
    };
    log_text(text);
    log_flush();
    write(text);

    struct Shared {
        std::mutex m;
        std::condition_variable cv;
        bool done = false;
        std::vector<std::pair<std::string, int64_t>> gauges;
    };
    auto sh = std::make_shared<Shared>();
    std::thread([sh] {
        auto g = gauge_read_all();
        std::lock_guard<std::mutex> lk(sh->m);
        sh->gauges = std::move(g);
        sh->done = true;
        sh->cv.notify_all();
    }).detach();
    std::string gtext;
    {
        std::unique_lock<std::mutex> lk(sh->m);
        if (sh->cv.wait_for(lk, std::chrono::seconds(3), [&] { return sh->done; })) {
            gtext = "gauges:\n";
            for (const auto& kv : sh->gauges) {
                std::snprintf(b, sizeof b, "  %s = %lld\n", kv.first.c_str(), static_cast<long long>(kv.second));
                gtext += b;
            }
        } else {
            gtext = "gauges: (not read: a gauge callback did not return within 3 s)\n";
        }
    }
    log_text(gtext);
    log_flush();
    write(text + gtext);
    std::fwrite(text.data(), 1, text.size(), stderr);
    std::fflush(stderr);
}

void guard_loop(Settings s) {
    auto next_log = Clock::now() + std::chrono::seconds(30);
    uint64_t logged_f = 0;                       // footprint of the last log line (a rise of 1 GB or more logs at once)
    bool warned = false;
    Clock::time_point over_since{};              // the stop condition holds since then (default: it does not)
    bool over = false;
    uint64_t over_peak = 0;
    std::unique_lock<std::mutex> lk(g_mu);
    for (;;) {
        // Returns true as soon as g_stop is set (memguard_stop notifies), so stop is prompt.
        if (g_cv.wait_for(lk, std::chrono::milliseconds(s.poll_ms), [] { return g_stop; })) return;
        lk.unlock();

        const uint64_t f = memguard_footprint_host();
        raise_peak(f);
        const SysMem sys = sys_mem();
        const auto now = Clock::now();

        if (now >= next_log || f >= logged_f + kGb) {
            if (f >= logged_f + kGb && logged_f)
                NXLOG_I("memory", "footprint rose by %.2f GB: %s", gbd(f - logged_f), numbers_line(f).c_str());
            else
                NXLOG_I("memory", "footprint %.2f GB (peak %.2f GB, limit %.1f GB%s)", gbd(f), gbd(g_peak.load()), gbd(s.limit),
                        s.auto_limit ? ", auto" : "");
            next_log = now + std::chrono::seconds(30);
            logged_f = f;
        } else if (f + kGb < logged_f) {
            logged_f = f;                        // fell back: the next rise of 1 GB is reported again
        }

        // Pressure: past the soft line, or the system short of physical memory. Shedders drop what they can; restored once it eases.
        const bool phys_low = sys.phys_total && sys.phys_avail < sys.phys_total / 25;
        const bool phys_ok = !sys.phys_total || sys.phys_avail > sys.phys_total / 12;
        if (!g_pressure.load() && (f > s.soft || phys_low)) {
            g_pressure.store(true);
            NXLOG_W("memory", "memory pressure (%s): %s; caches are trimmed and big writes wait", f > s.soft ? "footprint past the soft line" : "little physical memory free",
                    numbers_line(f).c_str());
            const uint64_t freed = run_shedders(true);
            NXLOG_I("memory", "memory pressure: %.0f MB dropped", static_cast<double>(freed) / static_cast<double>(kMb));
        } else if (g_pressure.load() && f < s.soft / 20 * 19 && phys_ok) {
            g_pressure.store(false);
            NXLOG_I("memory", "memory pressure over: %s", numbers_line(f).c_str());
            run_shedders(false);
        }
        if (!warned && f > s.limit / 4 * 3) {
            warned = true;
            NXLOG_W("memory", "footprint %.2f GB is past 75%% of the %.1f GB limit ([memory] limit_gb): a footprint that stays over the limit for "
                              "%.0f s stops the game with a report", gbd(f), gbd(s.limit), s.grace_ms / 1000.0);
        }

        // Stop: over the limit, or the system's commit about to run out while this process holds much of it, for the whole grace time. A
        // short peak (a big temporary buffer, a burst of pipeline builds) only logs. Commit all but gone: no waiting, allocations fail now.
        const bool commit_low = commit_nearly_gone(sys, 512 * kMb) && f > s.limit / 2;
        const bool commit_gone = sys.commit_limit && sys.commit_avail < 256 * kMb && f > s.limit / 2;
        const bool cond = f > s.limit || commit_low;
        if (cond) {
            if (!over) {
                over = true;
                over_since = now;
                over_peak = f;
                NXLOG_W("memory", "%s: %s; the game stops if this lasts %.0f s", f > s.limit ? "footprint over the limit" : "the system's commit is nearly used up",
                        numbers_line(f).c_str(), s.grace_ms / 1000.0);
            }
            over_peak = std::max(over_peak, f);
        } else if (over) {
            over = false;
            NXLOG_I("memory", "back under the limit after %.1f s (peak %.2f GB): %s",
                    std::chrono::duration<double>(now - over_since).count(), gbd(over_peak), numbers_line(f).c_str());
        }
        const bool expired = over && now - over_since >= std::chrono::milliseconds(s.grace_ms);
        if ((expired || commit_gone) && !g_tripped.exchange(true)) {
            char why[512];
            if (f > s.limit)
                std::snprintf(why, sizeof why, "footprint %.2f GB stayed over the %.1f GB limit for %.1f s (peak %.2f GB)", gbd(f), gbd(s.limit),
                              std::chrono::duration<double>(now - over_since).count(), gbd(over_peak));
            else
                std::snprintf(why, sizeof why, "the system's commit is used up (%.2f of %.1f GB free) and this game holds %.2f GB of it", gbd(sys.commit_avail),
                              gbd(sys.commit_limit), gbd(f));
            if (s.on_exceed) {
                s.on_exceed(f, s.limit, s.user);
            } else {
                NXLOG_E("memory", "%s: stopping", why);
                write_limit_report(f, s.limit, why);
                std::fflush(nullptr);
                TerminateProcess(GetCurrentProcess(), 3);
            }
        }

        lk.lock();
    }
}

}  // namespace

uint64_t memguard_footprint() { return proc_mem().priv; }

void memguard_set_discount(uint64_t (*gpu_bytes)()) { g_discount.store(gpu_bytes); }

uint64_t memguard_footprint_host() {
    const uint64_t f = memguard_footprint();
    auto fn = g_discount.load();
    const uint64_t d = fn ? fn() : 0;
    return d < f ? f - d : 0;
}

double memguard_limit_gb(const char* text) {
    if (!text) return 0.0;
    const double gb = std::atof(text);
    return (gb > 0.0 && std::isfinite(gb)) ? gb : 0.0;
}

uint64_t memguard_auto_limit(uint64_t physical_bytes) { return std::max<uint64_t>(physical_bytes, 8 * kGb); }

void memguard_start(const MemGuardOptions& opt) {
    std::lock_guard<std::mutex> ctl(g_ctl);
    if (g_thread.joinable()) return;  // already running: ignored

    Settings s{};
    s.auto_limit = false;
    if (opt.limit_bytes) {
        s.limit = opt.limit_bytes;
    } else {
        // WIP: the old fixed default stays until the auto limit is verified in game; limit_gb = auto opts in.
        const double gb = memguard_limit_gb(config_get_str("memory", "limit_gb", game::info().platform == game::Platform::Switch2 ? "16" : "8").c_str());
        if (gb > 0.0) {
            s.limit = gb_to_bytes(gb);
        } else {
            s.limit = memguard_auto_limit(sys_mem().phys_total);
            s.auto_limit = true;
        }
    }
    const double soft_pct = std::clamp(config_get_double("memory", "soft_percent", 80.0), 10.0, 100.0);
    s.soft = static_cast<uint64_t>(static_cast<double>(s.limit) * soft_pct / 100.0);
    const int64_t poll = config_get_int("memory", "poll_ms", 250);
    s.poll_ms = opt.poll_ms ? opt.poll_ms : (poll > 0 ? static_cast<uint32_t>(poll) : 250u);
    if (opt.grace_ms >= 0) {
        s.grace_ms = static_cast<uint32_t>(opt.grace_ms);
    } else {
        // WIP: 0 (stop at once, the old behaviour) until the grace time is verified in game
        const double g = config_get_double("memory", "grace_s", 0.0);
        s.grace_ms = static_cast<uint32_t>(std::clamp(std::isfinite(g) ? g : 0.0, 0.0, 3600.0) * 1000.0);
    }
    s.on_exceed = opt.on_exceed;
    s.user = opt.user;
    if (!opt.limit_bytes) {
        const SysMem m = sys_mem();
        NXLOG_I("memory", "guard: limit %.1f GB (%s), pressure from %.1f GB, a stop needs %.0f s over it; this PC: %.1f GB physical memory, "
                          "commit limit %.1f GB", gbd(s.limit), s.auto_limit ? "auto: the physical memory" : "[memory] limit_gb", gbd(s.soft),
                s.grace_ms / 1000.0, gbd(m.phys_total), gbd(m.commit_limit));
    }

    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_stop = false;
    }
    g_peak.store(0);
    g_limit.store(s.limit);
    g_tripped.store(false);
    g_pressure.store(false);
    g_thread = std::thread(guard_loop, s);
}

void memguard_stop() {
    std::lock_guard<std::mutex> ctl(g_ctl);
    if (!g_thread.joinable()) return;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_stop = true;
    }
    g_cv.notify_all();
    g_thread.join();
    g_pressure.store(false);
    g_limit.store(0);
}

bool memguard_running() {
    std::lock_guard<std::mutex> ctl(g_ctl);
    return g_thread.joinable();
}

uint64_t memguard_peak() { return g_peak.load(); }
uint64_t memguard_limit() { return g_limit.load(); }
bool memguard_tripped() { return g_tripped.load(); }
bool memguard_pressure() { return g_pressure.load(std::memory_order_relaxed); }

void memguard_add_shedder(const char* name, MemShedFn fn) {
    if (!fn) return;
    std::lock_guard<std::mutex> lk(g_shed_mu);
    g_shedders.emplace_back(name ? name : "?", fn);
}

}  // namespace nxrt
