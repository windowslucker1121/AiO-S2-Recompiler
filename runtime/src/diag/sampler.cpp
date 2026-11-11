// SPDX-License-Identifier: GPL-3.0-or-later
// In-process sampling profiler (S34), for machines where no profiler can run without administrator rights.
// `[prof] sample_hz` (env NXRT_PROF_SAMPLE_HZ, default 0 = off): a thread suspends every registered thread (or those named in `[prof]
// sample_threads`, comma-separated) in turn, reads its context, walks
// its host stack (the same allocation-free walk as the freeze watchdog) and resumes it. A thread whose instruction pointer sits right after a
// `syscall` instruction is blocked in the kernel (a wait, a sleep, a yield); its sample gets the leaf frame `[wait]`. Every other sample is
// the thread running user code. Samples are written as folded stacks, equal stacks added up, to `[prof] sample_out` (default
// "prof_samples.folded" in the working folder), now and then and at shutdown:
//   <label>;<thread>;<outermost frame>;...;<leaf frame> <count>
// The label is the scene a script marked with `@log SCENE start NAME` (empty when none). Frames in the game exe are named from the linker map
// (lifted game functions are f_<guest address>); frames in other modules show as module!+0x<offset>. `prof_report.py` (local test script) turns the file
// into tables. Cost: a few percent of one core per 100 Hz and 10 threads; each sampled thread is stopped for a few microseconds per sample.
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "nxrt/config.h"
#include "nxrt/diag.h"
#include "nxrt/log.h"
#include "nxrt/symbols.h"
#include "stackwalk.h"

namespace nxrt {
namespace {

constexpr size_t kDepth = 32;

struct Sample {
    uint32_t thread = 0;    // index into names
    uint32_t label = 0;     // index into labels
    uint8_t waiting = 0;
    uint8_t n = 0;
    uint64_t frames[kDepth];
};

struct State {
    std::mutex ctl;
    std::thread th;
    std::atomic<bool> stop{false};
    std::mutex lm;                       // labels
    std::vector<std::string> labels{std::string()};
    std::atomic<uint32_t> label{0};
    std::vector<std::string> names;
    std::unordered_map<std::string, uint32_t> agg;     // key = raw sample bytes (thread, label, waiting, n, frames) -> count
    std::string out;
    std::vector<std::string> only;       // [prof] sample_threads: names to sample (empty = every registered thread)
    double hz = 0;
    uint64_t total = 0, waiting = 0;
    FILE* f = nullptr;
    std::unordered_map<uint64_t, std::string> cache;
};
State& st() {
    static State* s = new State;
    return *s;
}

uint32_t name_index(State& s, const std::string& n) {
    for (size_t i = 0; i < s.names.size(); i++)
        if (s.names[i] == n) return static_cast<uint32_t>(i);
    s.names.push_back(n);
    return static_cast<uint32_t>(s.names.size() - 1);
}

// True when the two bytes before `ip` are a `syscall` instruction (0F 05): the thread is inside a system call. No C++ objects here (SEH).
bool after_syscall(uint64_t ip) {
    uint8_t b[2] = {0, 0};
    SIZE_T got = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(ip - 2), b, 2, &got) || got != 2) return false;   // no fault if the page is not mapped
    return b[0] == 0x0F && b[1] == 0x05;
}

void flush_samples(State& s);

void loop() {
    State& s = st();
    const auto period = std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(1.0 / s.hz));
    auto next = std::chrono::steady_clock::now() + period;
    auto last_flush = std::chrono::steady_clock::now();
    std::vector<Sample> batch;
    while (!s.stop.load()) {
        std::this_thread::sleep_until(next);
        next += period;
        if (next < std::chrono::steady_clock::now()) next = std::chrono::steady_clock::now() + period;   // do not catch up after a stall
        std::vector<ThreadRecord> recs = diag_threads_dup();   // private handles: a thread unregistering mid-sample cannot close ours
        batch.assign(recs.size(), Sample{});
        std::vector<char> ok(recs.size(), 0);
        const uint32_t label = s.label.load(std::memory_order_relaxed);
        for (size_t i = 0; i < recs.size(); i++) {
            HANDLE h = static_cast<HANDLE>(recs[i].host_handle);
            if (!h || recs[i].host_tid == GetCurrentThreadId()) continue;
            if (!s.only.empty() && std::find(s.only.begin(), s.only.end(), recs[i].name) == s.only.end()) continue;
            if (SuspendThread(h) == static_cast<DWORD>(-1)) continue;
            CONTEXT tc{};
            tc.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
            if (GetThreadContext(h, &tc)) {
                batch[i].n = static_cast<uint8_t>(detail::capture_stack_guarded(&tc, batch[i].frames, kDepth));
                batch[i].waiting = after_syscall(tc.Rip) ? 1 : 0;
                ok[i] = batch[i].n > 0;
            }
            ResumeThread(h);
        }
        const std::vector<std::string> names = [&] {   // the names are copied: the records go away with the handles
            std::vector<std::string> n;
            for (const ThreadRecord& r : recs) n.push_back(r.name);
            return n;
        }();
        diag_threads_release(recs);
        for (size_t i = 0; i < batch.size(); i++) {
            if (!ok[i]) continue;
            batch[i].thread = name_index(s, names[i]);
            batch[i].label = label;
            const Sample& x = batch[i];
            std::string key(reinterpret_cast<const char*>(&x), offsetof(Sample, frames) + x.n * sizeof(uint64_t));
            s.agg[key]++;
            s.total++;
            s.waiting += x.waiting;
        }
        if (std::chrono::steady_clock::now() - last_flush > std::chrono::seconds(3)) {   // the run may end without a normal shutdown: write as we go
            flush_samples(s);
            last_flush = std::chrono::steady_clock::now();
        }
    }
}

std::string frame_name(uint64_t addr, std::unordered_map<uint64_t, std::string>& cache) {
    auto it = cache.find(addr);
    if (it != cache.end()) return it->second;
    const SymbolInfo si = symbols_lookup(addr);
    std::string n;
    if (si.from_map && !si.symbol.empty()) n = si.symbol;
    else if (si.module != "?" && !si.module.empty()) {
        char b[32];
        std::snprintf(b, sizeof b, "!+0x%llx", static_cast<unsigned long long>(si.offset & ~0xfffull));   // 4 KiB buckets: no symbols for other modules
        n = si.module + b;
    } else n = "unknown";
    for (char& c : n)
        if (c == ';' || c == ' ' || c == '\n') c = '_';
    cache[addr] = n;
    return n;
}

void flush_samples(State& s) {
    if (s.agg.empty()) return;
    if (!s.f) {
        s.f = std::fopen(s.out.c_str(), "wb");
        if (!s.f) {
            NXLOG_W("prof", "cannot write %s", s.out.c_str());
            s.agg.clear();
            return;
        }
        std::fprintf(s.f, "# sampling profiler, %.0f Hz\n", s.hz);
    }
    for (const auto& kv : s.agg) {
        Sample x{};
        std::memcpy(&x, kv.first.data(), kv.first.size());
        std::string line;
        {
            std::lock_guard<std::mutex> lk(s.lm);
            line = s.labels[x.label];
        }
        line += ';';
        line += s.names[x.thread];
        for (int i = static_cast<int>(x.n) - 1; i >= 0; i--) {
            line += ';';
            line += frame_name(x.frames[i], s.cache);
        }
        if (x.waiting) line += ";[wait]";
        std::fprintf(s.f, "%s %u\n", line.c_str(), kv.second);
    }
    s.agg.clear();
    std::fprintf(s.f, "# samples %llu, waiting %llu\n", static_cast<unsigned long long>(s.total), static_cast<unsigned long long>(s.waiting));
    std::fflush(s.f);
}

}  // namespace

void prof_sampler_label(const char* label) {
    State& s = st();
    std::lock_guard<std::mutex> lk(s.lm);
    uint32_t idx = 0;
    if (label && *label) {
        for (size_t i = 0; i < s.labels.size(); i++)
            if (s.labels[i] == label) idx = static_cast<uint32_t>(i);
        if (idx == 0) {
            s.labels.push_back(label);
            idx = static_cast<uint32_t>(s.labels.size() - 1);
        }
    }
    s.label.store(idx);
}

void prof_sampler_start() {
    const double hz = config_get_double("prof", "sample_hz", 0.0);
    if (hz <= 0) return;
    State& s = st();
    std::lock_guard<std::mutex> g(s.ctl);
    if (s.th.joinable()) return;
    s.hz = std::min(hz, 2000.0);
    s.out = config_get_str("prof", "sample_out", "prof_samples.folded");
    s.only.clear();
    const std::string only = config_get_str("prof", "sample_threads", "");     // comma-separated thread names, e.g. main,nvn-render
    for (size_t a = 0; a < only.size();) {
        size_t b = only.find(',', a);
        if (b == std::string::npos) b = only.size();
        if (b > a) s.only.push_back(only.substr(a, b - a));
        a = b + 1;
    }
    s.stop = false;
    s.th = std::thread(loop);
    NXLOG_I("prof", "sampling profiler: %.0f Hz, output %s", s.hz, s.out.c_str());
}

void prof_sampler_stop() {
    State& s = st();
    std::lock_guard<std::mutex> g(s.ctl);
    if (!s.th.joinable()) return;
    s.stop = true;
    s.th.join();
    flush_samples(s);
    if (s.f) {
        std::fclose(s.f);
        s.f = nullptr;
    }
    NXLOG_I("prof", "sampling profiler: samples written to %s", s.out.c_str());
}

}  // namespace nxrt
