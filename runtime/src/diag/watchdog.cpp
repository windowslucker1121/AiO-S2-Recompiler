// SPDX-License-Identifier: GPL-3.0-or-later
// Idea adapted from SuperMarioWonderRecomp runtime/src/kernel_watch.cpp (MIT, Copyright (c) 2026 BelmanteGu); see THIRD_PARTY_NOTICES.md.
// Freeze watchdog (declared in nxrt/diag.h): a thread that notices when diag_frame_tick() stops advancing and then dumps
// every registered thread's host stack and guest registers to the log and a report file. It also honours a
// "dump_threads" request file next to the exe.
#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "nxrt/config.h"
#include "nxrt/diag.h"
#include "nxrt/gauge.h"
#include "nxrt/instance.h"
#include "nxrt/dispatch.h"
#include "nxrt/kernel.h"
#include "nxrt/log.h"
#include "nxrt/symbols.h"
#include "stackwalk.h"

namespace nxrt {
namespace {

constexpr size_t kMaxFrames = 48;
using Clock = std::chrono::steady_clock;

std::atomic<uint64_t> g_frames{0};
std::atomic<uint64_t> g_progress{0};   // heartbeats that are not frames (diag_progress)
std::atomic<uint32_t> g_fires{0};
std::atomic<bool> g_running{false};

enum class Status : uint8_t { NoSuspend, NoContext, Ok };

// One registered thread as read by the dump. Phase one (suspend, read, resume) only writes into storage that was sized
// before the first SuspendThread; formatting happens after every thread is running again.
struct ThreadDump {
    ThreadRecord rec;                    // copied from the registry before any suspension
    Status status = Status::NoSuspend;
    bool have_guest = false;
    uint64_t guest_lr = 0, guest_sp = 0, guest_fp = 0;
    uint64_t cpu_user_ms = 0, cpu_kernel_ms = 0;   // CPU time the thread has used so far: two dumps tell a busy thread from a waiting one
    size_t nframes = 0;
    uint64_t frames[kMaxFrames] = {};
};

// Appends a formatted number (never a string argument: the buffer is fixed size).
void appendf(std::string& out, const char* fmt, ...) {
    char b[256];
    va_list ap;
    va_start(ap, fmt);
    int n = std::vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (n > 0) out.append(b, static_cast<size_t>(n) < sizeof b ? static_cast<size_t>(n) : sizeof b - 1);
}

// Phase one. The registry snapshot and every allocation happen before the first thread is suspended. While a thread is
// suspended nothing allocates and nothing logs, because the suspended thread may hold the heap or the log lock.
void collect_threads(std::vector<ThreadDump>& out) {
    // Private handle duplicates (made under the registry lock): a thread unregistering mid-dump cannot close or recycle ours.
    std::vector<ThreadRecord> recs = diag_threads_dup();
    struct Release {
        std::vector<ThreadRecord>& v;
        ~Release() { diag_threads_release(v); }   // runs on every path; CloseHandle does not allocate, and all threads are resumed by then
    } release{recs};
    const DWORD self = GetCurrentThreadId();
    out.clear();
    out.reserve(recs.size());
    for (ThreadRecord& r : recs) {
        if (r.host_tid == self) continue;  // the calling thread is not dumped
        out.emplace_back();
        out.back().rec = r;                // copy: recs keeps the handle so the release above closes it
    }
    for (ThreadDump& d : out) {
        HANDLE h = static_cast<HANDLE>(d.rec.host_handle);
        if (h) {
            FILETIME created, exited, kernel_t, user_t;
            if (GetThreadTimes(h, &created, &exited, &kernel_t, &user_t)) {
                d.cpu_kernel_ms = ((static_cast<uint64_t>(kernel_t.dwHighDateTime) << 32) | kernel_t.dwLowDateTime) / 10000;
                d.cpu_user_ms = ((static_cast<uint64_t>(user_t.dwHighDateTime) << 32) | user_t.dwLowDateTime) / 10000;
            }
        }
        if (h == nullptr || SuspendThread(h) == static_cast<DWORD>(-1)) {
            d.status = Status::NoSuspend;
            continue;  // not suspended, nothing to resume
        }
        CONTEXT tc{};
        tc.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        uint64_t frames[kMaxFrames];
        size_t n = 0;
        const bool got = GetThreadContext(h, &tc) != 0;
        if (got) n = detail::capture_stack(&tc, frames, kMaxFrames);
        bool have_guest = false;
        uint64_t glr = 0, gsp = 0, gfp = 0;
        if (got && d.rec.ctx) {
            ::Ctx* c = d.rec.ctx;
            glr = lr(c);
            gsp = sp(c);
            gfp = fp(c);
            have_guest = true;
        }
        ResumeThread(h);  // resumed before anything else touches the thread's data
        if (!got) {
            d.status = Status::NoContext;
            continue;
        }
        d.status = Status::Ok;
        d.nframes = n;
        std::memcpy(d.frames, frames, n * sizeof(uint64_t));
        d.have_guest = have_guest;
        d.guest_lr = glr;
        d.guest_sp = gsp;
        d.guest_fp = gfp;
    }
    for (ThreadDump& d : out) d.rec.host_handle = nullptr;   // the copies are about to dangle; phase two never needs them
}

// Phase two: text for the collected threads. Runs with every thread resumed.
std::string format_threads(const std::vector<ThreadDump>& threads) {
    std::string out;
    for (const ThreadDump& d : threads) {
        out += "thread ";
        out += d.rec.name;
        out += " tid=";
        appendf(out, "%lu", static_cast<unsigned long>(d.rec.host_tid));
        out += " guest_tid=";
        appendf(out, "%llu", static_cast<unsigned long long>(d.rec.guest_tid));
        out += '\n';
        appendf(out, "  cpu: user %llu ms, kernel %llu ms\n", static_cast<unsigned long long>(d.cpu_user_ms), static_cast<unsigned long long>(d.cpu_kernel_ms));
        if (d.status == Status::NoSuspend) {
            out += "  (could not suspend)\n";
            continue;
        }
        if (d.status == Status::NoContext) {
            out += "  (could not read thread context)\n";
            continue;
        }
        if (d.have_guest) {
            appendf(out, "  guest: lr=%016llx sp=%016llx fp=%016llx\n", static_cast<unsigned long long>(d.guest_lr),
                    static_cast<unsigned long long>(d.guest_sp), static_cast<unsigned long long>(d.guest_fp));
        } else {
            out += "  guest: (no guest context)\n";
        }
        std::string chain;
        uint64_t last = 0;
        for (size_t i = 0; i < d.nframes; i++) {
            const uint64_t g = guest_function_of(d.frames[i]);
            if (!g || g == last) continue;
            if (!chain.empty()) chain += " <- ";
            appendf(chain, "f_%llx", static_cast<unsigned long long>(g));
            last = g;
        }
        if (!chain.empty()) {
            out += "  guest call stack: ";
            out += chain;
            out += '\n';
        }
        for (size_t i = 0; i < d.nframes; i++) {
            out += "  #";
            appendf(out, "%zu ", i);
            out += symbols_describe(d.frames[i]);
            out += '\n';
        }
    }
    return out;
}

std::string dump_text() {
    std::vector<ThreadDump> threads;
    collect_threads(threads);
    std::string out = format_threads(threads);
    out += "last files opened (oldest first):\n";
    const std::vector<std::string> files = diag_recent_files();
    if (files.empty()) out += "  (none)\n";
    for (const std::string& f : files) {
        out += "  ";
        out += f;
        out += '\n';
    }
    return out;
}

// Writes each line of the text to the log at Error level (tag "watchdog").
void log_lines(const std::string& text) {
    size_t s = 0;
    while (s < text.size()) {
        size_t e = text.find('\n', s);
        if (e == std::string::npos) e = text.size();
        const std::string line = text.substr(s, e - s);
        NXLOG_E("watchdog", "%s", line.c_str());
        s = e + 1;
    }
}

void write_report(const std::string& path, const std::string& text) {
    if (path.empty()) return;
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        NXLOG_W("watchdog", "cannot write report file %s", path.c_str());
        return;
    }
    std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
}

std::mutex g_report_mu;
std::vector<std::string (*)()> g_reports;

std::string extra_reports() {
    std::vector<std::string (*)()> fns;
    {
        std::lock_guard<std::mutex> lk(g_report_mu);
        fns = g_reports;
    }
    std::string out;
    for (auto fn : fns) out += fn();
    return out;
}

// Header line, then the thread dump. Goes to the log and the report file; returns the full text for on_fire.
std::string emit_dump(const std::string& report_path, const std::string& header) {
    std::string text = header;
    text += '\n';
    text += dump_text();
    text += kernel::sync_wait_report();
    text += extra_reports();
    log_lines(text);
    write_report(report_path, text);
    return text;
}

// The exe path with its extension replaced by _freeze.txt; a named instance: <instance folder>\<exe stem>_freeze.txt (nxrt/instance.h).
std::string default_report_path() { return instance_report_path("_freeze.txt"); }

struct Control {
    std::mutex ctl;  // serialises watchdog_start / watchdog_stop
    std::mutex m;    // guards `stop`; the thread sleeps on `cv`
    std::condition_variable cv;
    bool stop = false;
    std::thread th;
    // Parameters, written by watchdog_start before the thread is created.
    double threshold_s = 0;
    uint32_t poll_ms = 250;
    std::string report_path;   // "" = no file
    std::string request_path;  // <exe dir>\dump_threads (a named instance: <instance folder>\dump_threads)
    void (*on_fire)(const char* dump, void* user) = nullptr;
    void* user = nullptr;
};

// Intentionally never destroyed: a joinable std::thread must not die during static destruction.
Control& control() {
    static Control* c = new Control;
    return *c;
}

void run_loop(Control* c) {
    log_set_thread_name("watchdog");
    Clock::time_point last_change = Clock::now();  // idle time counts from the start, then from the last tick
    uint64_t last_seen = g_frames.load(std::memory_order_relaxed);
    uint64_t last_progress = g_progress.load(std::memory_order_relaxed);
    bool fired = false;
    for (;;) {
        {
            std::unique_lock<std::mutex> lk(c->m);
            if (c->cv.wait_for(lk, std::chrono::milliseconds(c->poll_ms), [c] { return c->stop; })) break;
        }
        const Clock::time_point now = Clock::now();
        const uint64_t frames = g_frames.load(std::memory_order_relaxed);
        const uint64_t progress = g_progress.load(std::memory_order_relaxed);
        if (frames != last_seen || progress != last_progress) {
            last_seen = frames;
            last_progress = progress;
            last_change = now;
            fired = false;  // a tick (or a progress heartbeat) re-arms the watchdog
        }
        // <exe dir>\dump_threads (whichever process sees it first) or <exe dir>\dump_threads.<pid> (this process only)
        const std::string own_request = c->request_path + "." + std::to_string(GetCurrentProcessId());
        const bool own = GetFileAttributesA(own_request.c_str()) != INVALID_FILE_ATTRIBUTES;
        if (own || GetFileAttributesA(c->request_path.c_str()) != INVALID_FILE_ATTRIBUTES) {
            DeleteFileA(own ? own_request.c_str() : c->request_path.c_str());
            std::string header;
            appendf(header, "THREAD DUMP REQUESTED (frame count %llu)", static_cast<unsigned long long>(frames));
            emit_dump(c->report_path, header);
            const double resample = config_get_double("diag", "freeze_resample_s", 0.0);   // a second sample: what changed in the meantime
            if (resample > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int64_t>(resample * 1000)));
                std::string h2;
                appendf(h2, "THREAD DUMP SECOND SAMPLE %.1f s later (frame count %llu)", resample, static_cast<unsigned long long>(g_frames.load(std::memory_order_relaxed)));
                emit_dump(c->report_path, h2);
            }
        }
        const double idle_s = std::chrono::duration<double>(now - last_change).count();
        if (!fired && idle_s >= c->threshold_s) {
            fired = true;
            std::string header;
            appendf(header, "FREEZE: no frame tick for %.1f s (frame count %llu)", idle_s,
                    static_cast<unsigned long long>(frames));
            std::string text = emit_dump(c->report_path, header);
            // [diag] freeze_resample_s: a second dump after that many seconds, so a stalled thread (same place) can be told from a busy one
            const double resample = config_get_double("diag", "freeze_resample_s", 0.0);
            if (resample > 0 && g_frames.load(std::memory_order_relaxed) == frames) {
                std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int64_t>(resample * 1000)));
                if (g_frames.load(std::memory_order_relaxed) == frames) {
                    std::string h2;
                    appendf(h2, "FREEZE SECOND SAMPLE: still no frame tick %.1f s later (frame count %llu)", resample, static_cast<unsigned long long>(frames));
                    text += emit_dump(c->report_path, h2);
                }
            }
            if (c->on_fire) c->on_fire(text.c_str(), c->user);
            g_fires.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

}  // namespace

void watchdog_start(const WatchdogOptions& opt) {
    Control& c = control();
    std::lock_guard<std::mutex> g(c.ctl);
    if (c.th.joinable()) return;  // already running: ignored
    if (opt.seconds < 0) return;  // disabled
    const double secs = opt.seconds > 0 ? opt.seconds : config_get_double("diag", "watchdog_seconds", 10.0);
    if (!(secs > 0)) return;      // disabled (also for a NaN value)
    c.threshold_s = secs;
    c.poll_ms = opt.poll_ms ? opt.poll_ms : 1;
    c.report_path = opt.report_path ? std::string(opt.report_path) : default_report_path();
    c.request_path = instance_request_dir() + "\\dump_threads";     // next to the exe, or in the instance folder
    c.on_fire = opt.on_fire;
    c.user = opt.user;
    {
        std::lock_guard<std::mutex> lk(c.m);
        c.stop = false;
    }
    c.th = std::thread(run_loop, &c);
    g_running.store(true);
    NXLOG_I("watchdog", "started: threshold %.1f s, poll %u ms, report file %s", secs, c.poll_ms,
            c.report_path.empty() ? "(none)" : c.report_path.c_str());
}

void watchdog_stop() {
    Control& c = control();
    std::lock_guard<std::mutex> g(c.ctl);
    if (!c.th.joinable()) return;
    {
        std::lock_guard<std::mutex> lk(c.m);
        c.stop = true;
    }
    c.cv.notify_all();
    c.th.join();
    g_running.store(false);
}

bool watchdog_running() { return g_running.load(); }

namespace {
std::atomic<void (*)()> g_frame_listener{nullptr};
}
void diag_set_frame_listener(void (*fn)()) { g_frame_listener.store(fn); }
void diag_progress() { g_progress.fetch_add(1, std::memory_order_relaxed); }
void diag_frame_tick_nolisten() { g_frames.fetch_add(1, std::memory_order_relaxed); }
void diag_frame_tick() {
    g_frames.fetch_add(1, std::memory_order_relaxed);
    soak_note_frame();
    if (auto fn = g_frame_listener.load(std::memory_order_acquire)) fn();
}

uint64_t diag_frame_count() { return g_frames.load(std::memory_order_relaxed); }

uint32_t watchdog_fire_count() { return g_fires.load(std::memory_order_relaxed); }

std::string diag_thread_dump() { return dump_text(); }

void diag_add_report(std::string (*fn)()) {
    std::lock_guard<std::mutex> lk(g_report_mu);
    g_reports.push_back(fn);
}

}  // namespace nxrt
