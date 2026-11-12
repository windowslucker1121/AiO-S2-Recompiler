// SPDX-License-Identifier: GPL-3.0-or-later
// Gauges and the soak sampler (declared in nxrt/gauge.h).
#include "nxrt/gauge.h"

#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <intrin.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstdio>
#include <map>
#include <mutex>
#include <thread>
#include <string>
#include <vector>

#include "nxrt/config.h"
#include "nxrt/diag.h"
#include "nxrt/log.h"

namespace nxrt {
namespace {

struct Registry {
    std::mutex m;
    std::map<std::string, std::function<int64_t()>> g;
};
Registry& reg() {
    static Registry* r = new Registry;     // never destroyed: counters in other statics may unregister at exit
    return *r;
}

// ---- frame times of the window -------------------------------------------------------------------------------------------
constexpr size_t kRing = 1 << 16;           // frames kept between two samples (enough for 6,500 fps at a 10 s window)
std::atomic<uint64_t> g_ring_n{0};
std::atomic<uint64_t> g_ring[kRing];        // QueryPerformanceCounter ticks of each presented frame

uint64_t qpc() {
    LARGE_INTEGER v;
    QueryPerformanceCounter(&v);
    return (uint64_t)v.QuadPart;
}
double qpc_freq() {
    static double f = [] { LARGE_INTEGER v; QueryPerformanceFrequency(&v); return (double)v.QuadPart; }();
    return f;
}

// ---- hitch log -------------------------------------------------------------------------------------------------------------
constexpr size_t kMaxSources = 48;
struct Source {
    const char* label = nullptr;
    uint64_t (*counter)() = nullptr;     // cumulative; shown as growth during the frame
    int64_t (*level)() = nullptr;        // instantaneous; shown when non-zero
    bool csv_only = false;               // [diag] frame_csv only (not on the hitch line)
};
Source g_src[kMaxSources];
std::atomic<size_t> g_nsrc{0};
uint64_t g_prev_val[kMaxSources];                 // the sources at the previous frame; the presenting thread only
std::atomic<bool> g_hitch_on{false};
std::atomic<uint64_t> g_hitch_ticks{0};           // a frame longer than this is a hitch
std::atomic<uint64_t> g_t0_ticks{0};

// phase timers (HitchPhase): QPC ticks summed over all threads
std::atomic<uint64_t> g_phase[kHpCount];
uint64_t g_prev_phase[kHpCount];                  // at the previous frame; the presenting thread only
std::atomic<uint64_t> g_game_present_last{0};     // QPC of the game's latest present call
std::atomic<uint64_t> g_game_gap_max{0};          // longest gap between two game present calls since the last presented frame
thread_local bool t_render_thread = false;
thread_local bool t_game_present_thread = false;  // this thread has called hitch_note_game_present

struct HitchEvent {
    double ms = 0;
    uint64_t frame = 0;
    double at_s = 0;                              // seconds since the monitor started
    int64_t val[kMaxSources] = {};
    uint64_t phase[kHpCount] = {};                // ticks of each phase inside the frame
    uint64_t game_gap = 0;                        // ticks: longest gap between the game's present calls
};
std::mutex g_ev_mu;                               // taken by the presenting thread only for a frame over the limit
std::vector<HitchEvent> g_pending;
std::atomic<uint64_t> g_hitch_total{0};           // frames over the limit since start
std::mutex g_recent_mu;                           // the logged hitch lines, for hitch_recent (the monitor thread writes, the menu reads)
std::vector<std::string> g_recent;
constexpr size_t kRecentMax = 64;

// [diag] frame_csv: every frame's row, queued by the presenting thread and written by the monitor thread
struct CsvRow {
    uint64_t frame = 0;
    double at_s = 0, ms = 0;
    int64_t val[kMaxSources] = {};
    uint64_t phase[kHpCount] = {};
    uint64_t game_gap = 0;
};
std::atomic<bool> g_csv_on{false};
std::mutex g_csv_mu;
std::vector<CsvRow> g_csv_rows;
FILE* g_csv_f = nullptr;
std::string g_csv_path;
size_t g_csv_cols = 0;                            // sources when the header was written (later ones are left out)

// [diag] frame_csv_threads (P1, W10 hitch census): CPU time per frame by thread group, from QueryThreadCycleTime of every registered thread
// (the registry is re-read every second; cycles are converted with the TSC rate measured against QPC). Columns cpu_<group>_us, plus
// cpu_guest_max_us (the busiest single guest thread) and cpu_guest_busy (guest threads with >= 50% of the frame). One sample per frame
// (taken by the first group counter of the row).
struct ThreadCpu {
    std::vector<ThreadRecord> recs;      // duplicated handles (diag_threads_dup)
    std::vector<int> group;
    std::vector<uint64_t> last;
    uint64_t refreshed = 0;              // QPC of the last registry read
    double cyc_per_us = 0;
    uint64_t cal_qpc = 0, cal_tsc = 0;
    uint64_t sum[9] = {};                // cumulative us per group; [7] guest max of the latest frame, [8] busy guest threads of the latest frame
    uint64_t frame_qpc = 0;
};
ThreadCpu g_tc;
constexpr const char* kTcNames[] = {"cpu_main_us", "cpu_guest_us", "cpu_render_us", "cpu_recorder_us", "cpu_front_us", "cpu_texlook_us", "cpu_other_us",
                                    "cpu_guest_max_us", "cpu_guest_busy"};
int tc_group(const std::string& n) {
    if (n == "main" || n.rfind("main", 0) == 0) return 0;
    if (n.rfind("guest", 0) == 0) return 1;
    if (n == "nvn-render") return 2;
    if (n == "nvn-recorder") return 3;
    if (n == "nvn-front") return 4;
    if (n == "nvn-texlook") return 5;
    return 6;
}
void tc_sample() {
    ThreadCpu& T = g_tc;
    const uint64_t now = qpc();
    const uint64_t tsc = __rdtsc();
    if (!T.cal_qpc) { T.cal_qpc = now; T.cal_tsc = tsc; }
    else if (now - T.cal_qpc > (uint64_t)qpc_freq() / 2) T.cyc_per_us = (double)(tsc - T.cal_tsc) / ((double)(now - T.cal_qpc) * 1e6 / qpc_freq());
    if (!T.refreshed || now - T.refreshed > (uint64_t)qpc_freq()) {   // re-read the registry: new threads start at their current count
        std::vector<ThreadRecord> fresh = diag_threads_dup();
        std::vector<uint64_t> last(fresh.size(), 0);
        std::vector<int> grp(fresh.size(), 6);
        for (size_t i = 0; i < fresh.size(); ++i) {
            grp[i] = tc_group(fresh[i].name);
            ULONG64 c = 0;
            if (fresh[i].host_handle) QueryThreadCycleTime(fresh[i].host_handle, &c);
            last[i] = c;
            for (size_t j = 0; j < T.recs.size(); ++j)   // the same thread as before: keep its count, so this frame's cycles are not lost
                if (T.recs[j].host_tid == fresh[i].host_tid) { last[i] = T.last[j]; break; }
        }
        diag_threads_release(T.recs);
        T.recs.swap(fresh);
        T.last.swap(last);
        T.group.swap(grp);
        T.refreshed = now;
    }
    const double frame_us = T.frame_qpc ? (double)(now - T.frame_qpc) * 1e6 / qpc_freq() : 0;
    T.frame_qpc = now;
    uint64_t gmax = 0, busy = 0;
    for (size_t i = 0; i < T.recs.size(); ++i) {
        ULONG64 c = 0;
        if (!T.recs[i].host_handle || !QueryThreadCycleTime(T.recs[i].host_handle, &c)) continue;
        const uint64_t d = c >= T.last[i] ? c - T.last[i] : 0;
        T.last[i] = c;
        if (T.cyc_per_us <= 0) continue;
        const uint64_t us = (uint64_t)((double)d / T.cyc_per_us);
        T.sum[T.group[i]] += us;
        if (T.group[i] == 1 || T.group[i] == 0) {
            gmax = std::max(gmax, us);
            if (frame_us > 0 && us * 2 >= frame_us) ++busy;
        }
    }
    T.sum[7] = gmax;
    T.sum[8] = busy;
}
template <int G> uint64_t tc_counter() {
    if (G == 0) tc_sample();   // the first column of the row samples
    return g_tc.sum[G];
}
template <int G> int64_t tc_level() { return (int64_t)g_tc.sum[G]; }

// Called for every presented frame at tick t (previous frame's tick prev_t, 0 for the first), frame number n.
void hitch_on_frame(uint64_t t, uint64_t prev_t, uint64_t n) {
    if (!g_hitch_on.load(std::memory_order_relaxed)) return;
    const size_t ns = g_nsrc.load(std::memory_order_acquire);
    uint64_t cur[kMaxSources];
    for (size_t i = 0; i < ns; i++) cur[i] = g_src[i].counter ? g_src[i].counter() : (uint64_t)g_src[i].level();
    uint64_t curp[kHpCount];
    for (int i = 0; i < kHpCount; i++) curp[i] = g_phase[i].load(std::memory_order_relaxed);
    const uint64_t gap = g_game_gap_max.exchange(0, std::memory_order_relaxed);
    const uint64_t thr = g_hitch_ticks.load(std::memory_order_relaxed);
    if (thr && prev_t && t > prev_t && t - prev_t > thr) {
        HitchEvent ev;
        ev.ms = (double)(t - prev_t) * 1000.0 / qpc_freq();
        ev.frame = n;
        ev.at_s = (double)(t - g_t0_ticks.load(std::memory_order_relaxed)) / qpc_freq();
        for (size_t i = 0; i < ns; i++) ev.val[i] = g_src[i].counter ? (int64_t)(cur[i] - g_prev_val[i]) : (int64_t)cur[i];
        for (int i = 0; i < kHpCount; i++) ev.phase[i] = curp[i] - g_prev_phase[i];
        ev.game_gap = gap;
        g_hitch_total.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lk(g_ev_mu);
        if (g_pending.size() < 256) g_pending.push_back(ev);
    }
    if (g_csv_on.load(std::memory_order_relaxed) && prev_t && t > prev_t) {
        CsvRow r;
        r.frame = n;
        r.ms = (double)(t - prev_t) * 1000.0 / qpc_freq();
        r.at_s = (double)(t - g_t0_ticks.load(std::memory_order_relaxed)) / qpc_freq();
        for (size_t i = 0; i < ns; i++) r.val[i] = g_src[i].counter ? (int64_t)(cur[i] - g_prev_val[i]) : (int64_t)cur[i];
        for (int i = 0; i < kHpCount; i++) r.phase[i] = curp[i] - g_prev_phase[i];
        r.game_gap = gap;
        std::lock_guard<std::mutex> lk(g_csv_mu);
        if (g_csv_rows.size() < 100000) g_csv_rows.push_back(r);
    }
    for (size_t i = 0; i < ns; i++) g_prev_val[i] = cur[i];
    for (int i = 0; i < kHpCount; i++) g_prev_phase[i] = curp[i];
}

struct FrameStats {
    size_t frames = 0;
    double span_s = 0, mean_fps = 0, low1_fps = 0, worst_ms = 0, median_ms = 0;
    size_t over2x = 0;
};
// dt: frame intervals in ms (sorted here).
FrameStats stats_of(std::vector<double>& dt) {
    FrameStats st;
    st.frames = dt.size();
    if (dt.empty()) return st;
    double sum = 0;
    for (double d : dt) sum += d;
    std::sort(dt.begin(), dt.end());
    st.span_s = sum / 1000.0;
    st.mean_fps = sum > 0 ? (double)dt.size() * 1000.0 / sum : 0;
    st.median_ms = dt[dt.size() / 2];
    const double p99 = dt[std::min(dt.size() - 1, (size_t)((double)dt.size() * 0.99))];
    st.low1_fps = p99 > 0 ? 1000.0 / p99 : 0;
    st.worst_ms = dt.back();
    for (double d : dt) if (d > 2.0 * st.median_ms) st.over2x++;
    return st;
}

std::string format_event(const HitchEvent& ev, size_t others) {
    char b[400];
    snprintf(b, sizeof b, "frame %llu took %.1f ms (t=%.1f s)", (unsigned long long)ev.frame, ev.ms, ev.at_s);
    std::string out = b;
    const size_t ns = g_nsrc.load(std::memory_order_acquire);
    bool any = false;
    for (size_t i = 0; i < ns; i++) {
        if (ev.val[i] == 0 || g_src[i].csv_only) continue;
        out += any ? ", " : "; during it: ";
        out += g_src[i].label;
        snprintf(b, sizeof b, " %s%lld", g_src[i].counter ? "+" : "=", (long long)ev.val[i]);
        out += b;
        any = true;
    }
    if (!any) out += "; nothing tracked (compiles, uploads, write faults) changed in it";
    // where the frame's time went (ms inside the frame). Render thread: idle + replay + present + other = the frame.
    auto ms = [&](int p) { return (double)ev.phase[p] * 1000.0 / qpc_freq(); };
    auto part = [&](std::string& s, const char* name, int p) {
        const double v = ms(p);
        if (v < 0.05) return;
        snprintf(b, sizeof b, "%s%s %.1f", s.empty() ? "" : ", ", name, v);
        s += b;
    };
    std::string rep, pre, game;
    part(rep, "gpu slot wait", kHpRenderGpuWait);
    part(rep, "submit", kHpRenderSubmit);
    part(rep, "uploads", kHpRenderUpload);
    part(rep, "pipeline creation", kHpRenderPipeline);
    part(rep, "pipeline misses", kHpRenderPipeMiss);
    part(rep, "texture images made", kHpRenderTexCreate);
    part(rep, "lock waits", kHpRenderLockWait);
    part(pre, "limiter", kHpRenderLimiter);
    part(pre, "acquire", kHpRenderAcquire);
    part(pre, "end/queue present", kHpRenderPresentEnd);
    const double other = ev.ms - ms(kHpRenderIdle) - ms(kHpRenderReplay) - ms(kHpRenderPresent);
    snprintf(b, sizeof b, "; render thread ms: idle %.1f, replay %.1f%s%s%s, present %.1f%s%s%s, other %.1f", ms(kHpRenderIdle), ms(kHpRenderReplay),
             rep.empty() ? "" : " (", rep.c_str(), rep.empty() ? "" : ")", ms(kHpRenderPresent), pre.empty() ? "" : " (", pre.c_str(), pre.empty() ? "" : ")",
             other > 0 ? other : 0.0);
    out += b;
    snprintf(b, sizeof b, "; game ms: longest gap between presents %.1f", (double)ev.game_gap * 1000.0 / qpc_freq());
    game = b;
    part(game, "present back pressure", kHpGameBackpressure);
    part(game, "GPU sync waits (all threads)", kHpGameSyncWait);
    {   // the presenting game thread's system calls (IPC = services and file reads, waits); the rest of its gap is guest code on the CPU
        std::string sv;
        part(sv, "IPC", kHpGameIpc);
        part(sv, "waits", kHpGameWait);
        const double v = ms(kHpGameSvc);
        if (v >= 0.05) {
            snprintf(b, sizeof b, ", presenting thread in system calls %.1f%s%s%s", v, sv.empty() ? "" : " (", sv.c_str(), sv.empty() ? "" : ")");
            game += b;
        }
    }
    out += game;
    std::string any_thr;
    part(any_thr, "write-fault handler", kHpFaults);
    part(any_thr, "write-watch arming", kHpWwArm);
    part(any_thr, "of faults on guarded pages", kHpFaultGuard);
    part(any_thr, "region lock waits (shared)", kHpWwSharedWait);
    part(any_thr, "region lock held exclusive", kHpWwExclHold);
    part(any_thr, "memory pool imports", kHpPoolImport);
    part(any_thr, "held-result fetches", kHpOutFetch);
    part(any_thr, "of them by guard faults", kHpOutFetchFault);
    part(any_thr, "background pipeline builds", kHpPipelineBg);
    if (!any_thr.empty()) out += "; any thread ms: " + any_thr;
    if (others) {
        snprintf(b, sizeof b, " (+%zu more slow frames in the last second)", others);
        out += b;
    }
    return out;
}

struct Monitor {
    std::mutex ctl;
    std::thread th;
    std::mutex m;
    std::condition_variable cv;
    bool stop = false;
};
Monitor& mon() {
    static Monitor* s = new Monitor;
    return *s;
}

const char* const kPhaseNames[] = {
    "r_idle", "r_replay", "r_gpu_wait", "r_submit", "r_upload", "r_pipeline", "r_lock_wait", "ww_arm", "r_present", "r_limiter",
    "r_acquire", "r_present_end", "faults", "g_backpressure", "g_sync_wait", "pool_import", "g_svc", "g_ipc", "g_wait",
    "out_fetch", "out_fetch_fault", "pipeline_bg", "r_pipe_miss", "r_tex_create", "fault_guard", "ww_shared_wait", "ww_excl_hold", "any_ipc"};
static_assert(sizeof(kPhaseNames) / sizeof(kPhaseNames[0]) == kHpCount, "a name for every HitchPhase");

void csv_flush() {
    std::vector<CsvRow> rows;
    {
        std::lock_guard<std::mutex> lk(g_csv_mu);
        rows.swap(g_csv_rows);
    }
    if (rows.empty()) return;
    if (!g_csv_f) {
        if (g_csv_path.empty()) return;
        g_csv_f = fopen(g_csv_path.c_str(), "wb");
        if (!g_csv_f) {
            NXLOG_W("diag", "frame csv: cannot open %s", g_csv_path.c_str());
            g_csv_path.clear();
            g_csv_on.store(false);
            return;
        }
        g_csv_cols = g_nsrc.load(std::memory_order_acquire);
        std::string h = "frame,t_s,ms";
        for (size_t i = 0; i < g_csv_cols; i++) { h += ","; h += g_src[i].label; }
        for (int i = 0; i < kHpCount; i++) { h += ",ms_"; h += kPhaseNames[i]; }
        h += ",ms_game_gap\n";
        fwrite(h.data(), 1, h.size(), g_csv_f);
        NXLOG_I("diag", "frame csv: every presented frame to %s (%zu sources)", g_csv_path.c_str(), g_csv_cols);
    }
    const double f = 1000.0 / qpc_freq();
    std::string line;
    char b[64];
    for (const CsvRow& r : rows) {
        snprintf(b, sizeof b, "%llu,%.3f,%.3f", (unsigned long long)r.frame, r.at_s, r.ms);
        line = b;
        for (size_t i = 0; i < g_csv_cols; i++) { snprintf(b, sizeof b, ",%lld", (long long)r.val[i]); line += b; }
        for (int i = 0; i < kHpCount; i++) { snprintf(b, sizeof b, ",%.3f", (double)r.phase[i] * f); line += b; }
        snprintf(b, sizeof b, ",%.3f\n", (double)r.game_gap * f);
        line += b;
        fwrite(line.data(), 1, line.size(), g_csv_f);
    }
    fflush(g_csv_f);
}

void monitor_run(double summary_s) {
    Monitor& s = mon();
    log_set_thread_name("hitch");
    using Clock = std::chrono::steady_clock;
    const auto period = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(summary_s > 0 ? summary_s : 1e6));
    auto next_summary = Clock::now() + period;
    auto last_line = Clock::now() - std::chrono::seconds(10);
    uint64_t last_n = g_ring_n.load(std::memory_order_acquire);
    bool seen_frames = last_n > 0;
    for (;;) {
        {
            std::unique_lock<std::mutex> lk(s.m);
            if (s.cv.wait_for(lk, std::chrono::milliseconds(100), [&] { return s.stop; })) { csv_flush(); return; }
        }
        csv_flush();
        const auto now = Clock::now();
        // at most one hitch line per second: the worst frame waiting, with the count of the others
        if (now - last_line >= std::chrono::seconds(1)) {
            HitchEvent worst;
            size_t cnt = 0;
            {
                std::lock_guard<std::mutex> lk(g_ev_mu);
                for (const auto& e : g_pending) if (cnt++ == 0 || e.ms > worst.ms) worst = e;
                g_pending.clear();
            }
            if (cnt) {
                std::string text = format_event(worst, cnt - 1);
                NXLOG_W("hitch", "%s", text.c_str());
                last_line = now;
                std::lock_guard<std::mutex> lk(g_recent_mu);
                if (g_recent.size() >= kRecentMax) g_recent.erase(g_recent.begin());
                g_recent.push_back(std::move(text));
            }
        }
        if (summary_s > 0 && now >= next_summary) {
            next_summary = now + period;
            const uint64_t n = g_ring_n.load(std::memory_order_acquire);
            const uint64_t first = (n > last_n + kRing - 1) ? n - (kRing - 1) : last_n;
            std::vector<double> dt;
            uint64_t prev = first > 0 ? g_ring[(first - 1) % kRing].load(std::memory_order_relaxed) : 0;
            for (uint64_t i = first; i < n; i++) {
                const uint64_t t = g_ring[i % kRing].load(std::memory_order_relaxed);
                if (prev) dt.push_back((double)(t - prev) * 1000.0 / qpc_freq());
                prev = t;
            }
            const uint64_t window_frames = n - last_n;
            last_n = n;
            if (n > 0) seen_frames = true;
            if (!dt.empty()) {
                const FrameStats st = stats_of(dt);
                NXLOG_I("hitch", "%llu frames in %.1f s: mean %.1f fps, 1%% low %.1f fps, worst %.1f ms, %zu over 2x the median (%.1f ms)",
                        (unsigned long long)window_frames, st.span_s, st.mean_fps, st.low1_fps, st.worst_ms, st.over2x, st.median_ms);
            } else if (seen_frames) {
                NXLOG_W("hitch", "no frame presented in the last %.1f s", summary_s);
            }
        }
    }
}

std::string frame_times_report() { return "\n" + frame_times_summary(30.0); }

struct Sampler {
    std::mutex ctl;
    std::thread th;
    std::mutex m;
    std::condition_variable cv;
    bool stop = false;
    FILE* f = nullptr;
};
Sampler& smp() {
    static Sampler* s = new Sampler;
    return *s;
}

uint32_t thread_count() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    THREADENTRY32 te;
    te.dwSize = sizeof te;
    uint32_t n = 0;
    const DWORD pid = GetCurrentProcessId();
    if (Thread32First(snap, &te)) do { if (te.th32OwnerProcessID == pid) n++; } while (Thread32Next(snap, &te));
    CloseHandle(snap);
    return n;
}

void run(double interval_s, double t0) {
    Sampler& s = smp();
    uint64_t last_n = 0;
    for (;;) {
        {
            std::unique_lock<std::mutex> lk(s.m);
            if (s.cv.wait_for(lk, std::chrono::duration<double>(interval_s), [&] { return s.stop; })) return;
        }
        const double now = (double)qpc() / qpc_freq();
        const uint64_t n = g_ring_n.load(std::memory_order_acquire);
        std::vector<double> dt;
        const uint64_t first = (n > last_n + kRing) ? n - kRing : last_n;
        uint64_t prev = first > 0 ? g_ring[(first - 1) % kRing].load(std::memory_order_relaxed) : 0;
        for (uint64_t i = first; i < n; i++) {
            const uint64_t t = g_ring[i % kRing].load(std::memory_order_relaxed);
            if (prev) dt.push_back((double)(t - prev) * 1000.0 / qpc_freq());
            prev = t;
        }
        const uint64_t window_frames = n - last_n;
        last_n = n;
        double p50 = 0, p99 = 0, mx = 0;
        if (!dt.empty()) {
            std::sort(dt.begin(), dt.end());
            p50 = dt[dt.size() / 2];
            p99 = dt[std::min(dt.size() - 1, (size_t)(dt.size() * 0.99))];
            mx = dt.back();
        }
        PROCESS_MEMORY_COUNTERS_EX pm{};
        pm.cb = sizeof pm;
        GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pm, sizeof pm);
        DWORD handles = 0;
        GetProcessHandleCount(GetCurrentProcess(), &handles);
        FILETIME c, e, k, u;
        GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
        const double cpu_ms = (((uint64_t)k.dwHighDateTime << 32 | k.dwLowDateTime) + ((uint64_t)u.dwHighDateTime << 32 | u.dwLowDateTime)) / 10000.0;
        char buf[640];
        snprintf(buf, sizeof buf,
                 "{\"t\":%.1f,\"frame\":%llu,\"win_frames\":%llu,\"ft_p50\":%.3f,\"ft_p99\":%.3f,\"ft_max\":%.2f,\"ws\":%llu,\"private\":%llu,\"peak_ws\":%llu,"
                 "\"pagefile\":%llu,\"handles\":%lu,\"threads\":%u,\"gdi\":%lu,\"user\":%lu,\"cpu_ms\":%.0f",
                 now - t0, (unsigned long long)diag_frame_count(), (unsigned long long)window_frames, p50, p99, mx, (unsigned long long)pm.WorkingSetSize,
                 (unsigned long long)pm.PrivateUsage, (unsigned long long)pm.PeakWorkingSetSize, (unsigned long long)pm.PagefileUsage, (unsigned long)handles,
                 thread_count(), (unsigned long)GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS), (unsigned long)GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS),
                 cpu_ms);
        std::string line = buf;
        for (auto& kv : gauge_read_all()) {
            snprintf(buf, sizeof buf, ",\"%s\":%lld", kv.first.c_str(), (long long)kv.second);
            line += buf;
        }
        line += "}\n";
        fwrite(line.data(), 1, line.size(), s.f);
        fflush(s.f);
    }
}

}  // namespace

void gauge_register(const char* name, std::function<int64_t()> fn) {
    Registry& r = reg();
    std::lock_guard<std::mutex> lk(r.m);
    r.g[name] = std::move(fn);
}

void gauge_unregister(const char* name) {
    Registry& r = reg();
    std::lock_guard<std::mutex> lk(r.m);
    r.g.erase(name);
}

std::vector<std::pair<std::string, int64_t>> gauge_read_all() {
    Registry& r = reg();
    std::lock_guard<std::mutex> lk(r.m);
    std::vector<std::pair<std::string, int64_t>> out;
    out.reserve(r.g.size());
    for (auto& kv : r.g) out.emplace_back(kv.first, kv.second());
    return out;
}

GaugeCounter::GaugeCounter(const char* name) : name_(name) {
    gauge_register(name, [this] { return get(); });
}
GaugeCounter::~GaugeCounter() { gauge_unregister(name_.c_str()); }

void soak_note_frame() {
    const uint64_t n = g_ring_n.load(std::memory_order_relaxed);
    const uint64_t t = qpc();
    const uint64_t prev_t = n > 0 ? g_ring[(n - 1) % kRing].load(std::memory_order_relaxed) : 0;
    g_ring[n % kRing].store(t, std::memory_order_relaxed);
    g_ring_n.store(n + 1, std::memory_order_release);
    hitch_on_frame(t, prev_t, n + 1);
}

uint64_t hitch_ticks() { return qpc(); }
double hitch_phase_seconds(int phase) {
    return phase >= 0 && phase < kHpCount ? static_cast<double>(g_phase[phase].load(std::memory_order_relaxed)) / qpc_freq() : 0.0;
}

void hitch_phase_add(int phase, uint64_t ticks) {
    if (phase >= 0 && phase < kHpCount) g_phase[phase].fetch_add(ticks, std::memory_order_relaxed);
}
void hitch_set_render_thread() { t_render_thread = true; }
bool hitch_on_render_thread() { return t_render_thread; }
bool hitch_on_game_present_thread() { return t_game_present_thread; }
void hitch_note_game_present() {
    t_game_present_thread = true;
    const uint64_t t = qpc();
    const uint64_t prev = g_game_present_last.exchange(t, std::memory_order_relaxed);
    if (!prev || t <= prev) return;
    const uint64_t d = t - prev;
    uint64_t m = g_game_gap_max.load(std::memory_order_relaxed);
    while (d > m && !g_game_gap_max.compare_exchange_weak(m, d, std::memory_order_relaxed)) {}
}

void hitch_add_counter(const char* label, uint64_t (*fn)()) {
    const size_t i = g_nsrc.load();
    if (i >= kMaxSources || !fn) return;
    g_src[i] = {label, fn, nullptr};
    g_nsrc.store(i + 1, std::memory_order_release);
}

void hitch_add_level(const char* label, int64_t (*fn)()) {
    const size_t i = g_nsrc.load();
    if (i >= kMaxSources || !fn) return;
    g_src[i] = {label, nullptr, fn};
    g_nsrc.store(i + 1, std::memory_order_release);
}

void hitch_add_csv_counter(const char* label, uint64_t (*fn)()) {
    const size_t i = g_nsrc.load();
    if (i >= kMaxSources || !fn) return;
    g_src[i] = {label, fn, nullptr, true};
    g_nsrc.store(i + 1, std::memory_order_release);
}

void hitch_start() {
    if (!config_get_bool("diag", "hitch_log", true)) return;
    Monitor& s = mon();
    std::lock_guard<std::mutex> g(s.ctl);
    if (s.th.joinable()) return;
    const double summary_s = config_get_double("diag", "hitch_summary_s", 10.0);
    const double hitch_ms = config_get_double("diag", "hitch_ms", 50.0);
    g_t0_ticks.store(qpc());
    g_hitch_ticks.store(hitch_ms > 0 ? (uint64_t)(hitch_ms / 1000.0 * qpc_freq()) : 0);
    {
        std::lock_guard<std::mutex> lk(g_ev_mu);
        g_pending.clear();
    }
    {
        const size_t ns = g_nsrc.load(std::memory_order_acquire);
        for (size_t i = 0; i < ns; i++) g_prev_val[i] = g_src[i].counter ? g_src[i].counter() : (uint64_t)g_src[i].level();
        for (int i = 0; i < kHpCount; i++) g_prev_phase[i] = g_phase[i].load(std::memory_order_relaxed);
    }
    {   // [diag] frame_csv: opened at the first flush with rows (the back end registers its sources when the GPU comes up)
        g_csv_path = config_get_str("diag", "frame_csv", "");
        if (!g_csv_path.empty()) {
            g_csv_on.store(true);
            static const bool cpu_registered = (hitch_add_csv_counter("proc_cpu_us", []() -> uint64_t {   // CPU time of all threads
                FILETIME c, e, k, u;
                if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) return 0;
                return ((((uint64_t)k.dwHighDateTime << 32) | k.dwLowDateTime) + (((uint64_t)u.dwHighDateTime << 32) | u.dwLowDateTime)) / 10;
            }), true);
            (void)cpu_registered;
            if (config_get_bool("diag", "frame_csv_threads", false)) {
                static const bool tc_registered = [] {
                    hitch_add_csv_counter(kTcNames[0], tc_counter<0>);
                    hitch_add_csv_counter(kTcNames[1], tc_counter<1>);
                    hitch_add_csv_counter(kTcNames[2], tc_counter<2>);
                    hitch_add_csv_counter(kTcNames[3], tc_counter<3>);
                    hitch_add_csv_counter(kTcNames[4], tc_counter<4>);
                    hitch_add_csv_counter(kTcNames[5], tc_counter<5>);
                    hitch_add_csv_counter(kTcNames[6], tc_counter<6>);
                    const size_t i = g_nsrc.load();   // levels, csv only
                    if (i + 2 <= kMaxSources) {
                        g_src[i] = {kTcNames[7], nullptr, tc_level<7>, true};
                        g_src[i + 1] = {kTcNames[8], nullptr, tc_level<8>, true};
                        g_nsrc.store(i + 2, std::memory_order_release);
                    }
                    return true;
                }();
                (void)tc_registered;
            }
        }
    }
    g_hitch_on.store(true);
    s.stop = false;
    s.th = std::thread(monitor_run, summary_s);
    static const bool report_registered = (diag_add_report(frame_times_report), true);   // the freeze dump shows the last 30 s of frame times
    (void)report_registered;
    NXLOG_I("diag", "hitch log: frame summary every %.0f s, frames over %.0f ms logged (at most one line per second)", summary_s, hitch_ms);
}

void hitch_stop() {
    Monitor& s = mon();
    std::lock_guard<std::mutex> g(s.ctl);
    g_hitch_on.store(false);
    if (!s.th.joinable()) return;
    {
        std::lock_guard<std::mutex> lk(s.m);
        s.stop = true;
    }
    s.cv.notify_all();
    s.th.join();
}

std::string frame_times_summary(double window_s) {
    if (!(window_s > 0)) window_s = 30.0;
    const uint64_t now = qpc();
    const uint64_t n = g_ring_n.load(std::memory_order_acquire);
    if (n == 0) return "frame times: no frame was presented yet\n";
    std::vector<uint64_t> ts;       // ticks of the frames inside the window, newest first
    const uint64_t win_ticks = (uint64_t)(window_s * qpc_freq());
    for (uint64_t k = 0; k < n && k < kRing - 1; k++) {
        const uint64_t t = g_ring[(n - 1 - k) % kRing].load(std::memory_order_relaxed);
        if (t > now || now - t > win_ticks) break;
        ts.push_back(t);
    }
    char b[256];
    const double last_age = (double)(now - g_ring[(n - 1) % kRing].load(std::memory_order_relaxed)) / qpc_freq();
    if (ts.size() < 2) {
        snprintf(b, sizeof b, "frame times, last %.0f s: %zu frame(s); the last frame was presented %.1f s ago (%llu frames in all)\n", window_s, ts.size(), last_age, (unsigned long long)n);
        return b;
    }
    std::reverse(ts.begin(), ts.end());     // oldest first
    const size_t nb = (size_t)std::ceil(window_s);
    std::vector<unsigned> fps(nb, 0);
    std::vector<double> bworst(nb, 0.0);
    std::vector<double> dt;
    struct W { double ms, ago; };
    std::vector<W> frames;
    for (size_t i = 1; i < ts.size(); i++) {
        const double d = (double)(ts[i] - ts[i - 1]) * 1000.0 / qpc_freq();
        const double ago = (double)(now - ts[i]) / qpc_freq();
        dt.push_back(d);
        frames.push_back({d, ago});
        const size_t bi = std::min(nb - 1, (size_t)ago);
        fps[bi]++;
        bworst[bi] = std::max(bworst[bi], d);
    }
    const FrameStats st = stats_of(dt);
    std::string out;
    snprintf(b, sizeof b, "frame times, last %.0f s (%llu frames in all; the last one was presented %.1f s ago):\n", window_s, (unsigned long long)n, last_age);
    out += b;
    snprintf(b, sizeof b, "  %zu frames: mean %.1f fps, 1%% low %.1f fps, worst %.1f ms, %zu over 2x the median (%.1f ms)\n", st.frames, st.mean_fps, st.low1_fps, st.worst_ms, st.over2x, st.median_ms);
    out += b;
    out += "  fps per second (oldest first):";
    for (size_t i = nb; i-- > 0;) { snprintf(b, sizeof b, " %u", fps[i]); out += b; }
    out += "\n  worst frame ms per second (oldest first):";
    for (size_t i = nb; i-- > 0;) { snprintf(b, sizeof b, " %.0f", bworst[i]); out += b; }
    out += "\n";
    std::sort(frames.begin(), frames.end(), [](const W& a, const W& c) { return a.ms > c.ms; });
    out += "  slowest frames (ms@seconds before the end):";
    for (size_t i = 0; i < frames.size() && i < 6; i++) { snprintf(b, sizeof b, " %.1f@-%.1f", frames[i].ms, frames[i].ago); out += b; }
    out += "\n";
    return out;
}

FrameTimeStats frame_time_stats(double window_s, std::vector<float>* recent_ms, size_t max_points) {
    FrameTimeStats r;
    if (recent_ms) recent_ms->clear();
    if (!(window_s > 0)) window_s = 10.0;
    const uint64_t now = qpc();
    const uint64_t n = g_ring_n.load(std::memory_order_acquire);
    r.total_frames = n;
    if (n == 0) return r;
    r.last_age_s = (double)(now - g_ring[(n - 1) % kRing].load(std::memory_order_relaxed)) / qpc_freq();
    const uint64_t win_ticks = (uint64_t)(window_s * qpc_freq());
    std::vector<double> dt;
    uint64_t newer = 0;
    for (uint64_t k = 0; k < n && k < kRing - 1; k++) {
        const uint64_t t = g_ring[(n - 1 - k) % kRing].load(std::memory_order_relaxed);
        if (t > now || now - t > win_ticks) break;
        if (newer && newer > t) dt.push_back((double)(newer - t) * 1000.0 / qpc_freq());
        newer = t;
    }
    std::reverse(dt.begin(), dt.end());     // oldest first
    if (recent_ms) {
        const size_t from = dt.size() > max_points ? dt.size() - max_points : 0;
        for (size_t i = from; i < dt.size(); i++) recent_ms->push_back((float)dt[i]);
    }
    const FrameStats st = stats_of(dt);
    r.frames = st.frames;
    r.mean_fps = st.mean_fps;
    r.low1_fps = st.low1_fps;
    r.worst_ms = st.worst_ms;
    r.median_ms = st.median_ms;
    r.over2x = st.over2x;
    return r;
}

std::vector<std::string> hitch_recent(size_t max, uint64_t* total, double* threshold_ms) {
    if (total) *total = g_hitch_total.load(std::memory_order_relaxed);
    if (threshold_ms) *threshold_ms = g_hitch_on.load(std::memory_order_relaxed) ? (double)g_hitch_ticks.load(std::memory_order_relaxed) * 1000.0 / qpc_freq() : 0.0;
    std::lock_guard<std::mutex> lk(g_recent_mu);
    const size_t from = g_recent.size() > max ? g_recent.size() - max : 0;
    return std::vector<std::string>(g_recent.begin() + (std::ptrdiff_t)from, g_recent.end());
}

void soak_start() {
    const std::string path = config_get_str("diag", "soak_csv", "");
    if (path.empty()) return;
    Sampler& s = smp();
    std::lock_guard<std::mutex> g(s.ctl);
    if (s.th.joinable()) return;
    s.f = fopen(path.c_str(), "ab");
    if (!s.f) {
        NXLOG_W("diag", "soak: cannot open %s", path.c_str());
        return;
    }
    const double iv = std::max(0.5, config_get_double("diag", "soak_interval_s", 10.0));
    s.stop = false;
    const double t0 = (double)qpc() / qpc_freq();
    s.th = std::thread(run, iv, t0);
    NXLOG_I("diag", "soak sampler: %s every %.1f s", path.c_str(), iv);
}

void soak_stop() {
    Sampler& s = smp();
    std::lock_guard<std::mutex> g(s.ctl);
    if (!s.th.joinable()) return;
    {
        std::lock_guard<std::mutex> lk(s.m);
        s.stop = true;
    }
    s.cv.notify_all();
    s.th.join();
    if (s.f) { fclose(s.f); s.f = nullptr; }
}

}  // namespace nxrt
