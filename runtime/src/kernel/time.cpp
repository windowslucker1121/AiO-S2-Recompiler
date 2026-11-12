// SPDX-License-Identifier: GPL-3.0-or-later
// System tick and high-resolution sleep. Pattern from smo-recomp runtime/os/os_threads.c sleep_ns and SuperMarioWonderRecomp kernel.cpp system_tick.
#include <windows.h>
#include <intrin.h>

#include <atomic>
#include <string>

#include "nxrt/kernel.h"
#include "nxrt/game.h"
#include "nxrt/tt.h"
#include "nxrt/config.h"

namespace nxrt::kernel {

uint64_t tick_hz() {
    static const uint64_t hz = game::info().platform == game::Platform::Switch2 ? kTickHzSwitch2 : kTickHzSwitch1;
    return hz;
}

uint64_t system_tick() {
    if (uint64_t vt; tt::virtual_tick(&vt)) return vt;      // deterministic mode: the virtual clock (test tooling)
    static const uint64_t freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<uint64_t>(f.QuadPart);
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    const uint64_t v = static_cast<uint64_t>(c.QuadPart);
    const uint64_t hz = tick_hz();
    return (v / freq) * hz + (v % freq) * hz / freq;     // split so the multiplication cannot overflow
}

namespace {
std::atomic<int> g_short_mode{-1};   // 0 yield, 1 pause; -1 not read yet
int short_cfg() {
    static const int m = [] {
        const std::string v = config_get_str("sched", "short_sleep", "yield");
        return v == "pause" ? 1 : 0;
    }();
    return m;
}
uint64_t qpc_hz() {
    static const uint64_t f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return static_cast<uint64_t>(x.QuadPart); }();
    return f;
}
}  // namespace
void short_sleep_ab(bool on) { g_short_mode.store(on ? short_cfg() : 0, std::memory_order_relaxed); }

// SleepThread takes nanoseconds and the console timer is good to a few microseconds. Sleep() counts whole milliseconds on a
// 1 ms or 15.6 ms grid, so the wait runs on a high-resolution waitable timer (100 ns units); Sleep() is only the fallback.
// A zero or negative span yields, as on the console.
void sleep_ns(int64_t ns) {
    if (tt::virtual_sleep(ns)) return;                      // deterministic mode: time advances, the thread only yields
    static thread_local HANDLE timer = nullptr;
    static thread_local bool no_timer = false;
    if (ns <= 0) {
        yield_host();
        return;
    }
    // Short sleeps (polls such as title A's GPU-fence wait, 100 us a round) overshoot to about half a millisecond on the timer: yield until the
    // deadline instead. Longer ones keep the timer (no busy waiting while a thread idles).
    if (ns <= 200000) {
        int mode = g_short_mode.load(std::memory_order_relaxed);
        if (mode < 0) { mode = short_cfg(); g_short_mode.store(mode, std::memory_order_relaxed); }
        if (mode == 1) {
            LARGE_INTEGER s, n;
            QueryPerformanceCounter(&s);
            const int64_t ticks = static_cast<int64_t>(static_cast<uint64_t>(ns) * qpc_hz() / 1000000000ull);
            const int64_t yield_every = static_cast<int64_t>(qpc_hz() / 40000);   // ~25 us
            int64_t next_yield = yield_every;
            for (;;) {
                for (int i = 0; i < 16; ++i) _mm_pause();
                QueryPerformanceCounter(&n);
                const int64_t d = n.QuadPart - s.QuadPart;
                if (d >= ticks) return;
                if (d >= next_yield) { yield_host(); next_yield = d + yield_every; }
            }
        }
        LARGE_INTEGER f, s, n;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&s);
        const int64_t ticks = ns * f.QuadPart / 1000000000;
        do {
            yield_host();
            QueryPerformanceCounter(&n);
        } while (n.QuadPart - s.QuadPart < ticks);
        return;
    }
    if (!timer && !no_timer) {
        timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!timer) no_timer = true;
    }
    LARGE_INTEGER due;
    due.QuadPart = -((ns + 99) / 100);
    if (!timer || !SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
        Sleep(static_cast<DWORD>((ns + 999999) / 1000000));
    else
        WaitForSingleObject(timer, INFINITE);
}

}  // namespace nxrt::kernel
