// SPDX-License-Identifier: GPL-3.0-or-later
// Kernel-level synchronisation tests (src/kernel/kernel_sync.cpp): ArbitrateLock/Unlock, WaitProcessWideKey/SignalProcessWideKey,
// WaitForAddress/SignalToAddress, WaitSynchronization/CancelSynchronization.
// The guest SDK's mutex and condition variable (libnx mutex.c / condvar.c; Horizon's nn::os uses the same protocol) is ported below as
// GMutex and GCond, so the tests drive the kernel the way guest code does. Rule used throughout: CHECK is per host thread, so worker
// threads never CHECK. They store results in atomics and the test thread checks them after joining.
// Timing rule: sleeps and timeouts are only ever asserted as lower bounds (with 1 ms slack for the Windows timer) and upper bounds
// that are generous enough for a loaded machine.
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <string>
#include <memory>
#include <random>
#include <thread>
#include <vector>

#include "nxrt/abi.h"
#include "nxrt/dispatch.h"
#include "nxrt/kernel.h"
#include "nxrt/memory.h"
#include "nxrt/svc.h"
#include "test_framework.h"
#include "test_platform.h"

NXTEST_GROUP(kernel_sync)

namespace ks = nxrt::kernel;

namespace {

using Clock = std::chrono::steady_clock;
constexpr int64_t kMs = 1000000;                   // nanoseconds per millisecond
constexpr double kTimerSlackMs = 1.0;              // Windows may wake a timed wait a tick early against steady_clock
constexpr uint32_t kWaitMask = ks::kHandleWaitMask;
constexpr uint64_t kEntryAddr = 0x7100010000ull;   // guest address of the thread entry trampoline (a host function, never run as code)
constexpr uint64_t kStackTop = 0x7C00100000ull;    // fake guest stack top; the host functions never touch the guest stack

double ms_since(Clock::time_point t0) { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); }
void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
uint64_t addr_of(const void* p) { return reinterpret_cast<uint64_t>(p); }
bool slept_at_least(double ms_elapsed, double want_ms) { return ms_elapsed >= want_ms - kTimerSlackMs; }

[[noreturn]] void watchdog_abort(const char* test) {
    std::printf("    WATCHDOG: %s did not finish within 20 s; aborting the test run\n", test);
    std::fflush(stdout);
    std::_Exit(3);
}

// Joins host threads, but no longer than 20 s overall. A deadlock ends the whole run with exit code 3 instead of hanging CTest.
void join_host(const char* test, std::vector<std::thread>& ts) {
    std::vector<HANDLE> hs;
    for (auto& t : ts) hs.push_back(static_cast<HANDLE>(t.native_handle()));
    if (!hs.empty() && WaitForMultipleObjects(static_cast<DWORD>(hs.size()), hs.data(), TRUE, 20000) == WAIT_TIMEOUT) watchdog_abort(test);
    for (auto& t : ts) t.join();
}

void join_guest(const char* test, uint32_t handle) {
    if (!ks::thread_join(handle, 20000)) watchdog_abort(test);
}

// Polls pred on the test thread for up to ms milliseconds. Returns the final pred() value.
template <class F>
bool wait_until(F pred, int ms = 5000) {
    const auto limit = Clock::now() + std::chrono::milliseconds(ms);
    while (!pred()) {
        if (Clock::now() > limit) return false;
        sleep_ms(1);
    }
    return true;
}

// Mutex tag: the guest thread's handle (what the SDK reads from its thread vars); a fixed fake tag for plain host threads.
uint32_t my_tag() {
    if (ks::KThread* t = ks::current_thread()) return t->handle;
    static std::atomic<uint32_t> next{0x30001};
    thread_local uint32_t tag = next.fetch_add(0x10);
    return tag;
}

uint32_t load_word(const uint32_t* p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }
bool cas_word(uint32_t* p, uint32_t expected, uint32_t desired) {
    return __atomic_compare_exchange_n(p, &expected, desired, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

// ---- the guest SDK's protocol (libnx mutex.c / condvar.c) ----------------------------------------------------------------
// GMutex: the word holds the owner's tag; kHandleWaitMask = "has waiters". GCond: the "has waiters" key word the kernel sets to 1
// before it releases the mutex in WaitProcessWideKey and clears again when the last waiter leaves.
struct GMutex {
    alignas(4) uint32_t word = 0;
};
struct GCond {
    alignas(4) uint32_t key = 0;
};

// Protocol errors seen by guest code (any ArbitrateLock/Unlock/WaitProcessWideKey result that the SDK would treat as fatal).
std::atomic<int> g_guest_errors{0};

void mutex_lock(GMutex& m) {
    const uint32_t tag = my_tag();
    const uint64_t a = addr_of(&m.word);
    for (;;) {
        uint32_t cur = load_word(&m.word);
        if (cur == 0) {
            if (cas_word(&m.word, 0, tag)) return;
            continue;
        }
        if ((cur & ~kWaitMask) == tag) return;       // the owner handed it to us (ArbitrateUnlock wrote our tag)
        if (!(cur & kWaitMask)) {                    // announce a waiter: set the wait bit, then sleep in the kernel
            if (!cas_word(&m.word, cur, cur | kWaitMask)) continue;
            cur |= kWaitMask;
        }
        if (ks::arbitrate_lock(cur & ~kWaitMask, a, tag) != ks::kOk) g_guest_errors++;
    }
}

void mutex_unlock(GMutex& m) {
    const uint32_t tag = my_tag();
    if (cas_word(&m.word, tag, 0)) return;           // no waiters: fast path
    if (load_word(&m.word) != (tag | kWaitMask)) {   // not ours, or the word is in an unexpected state
        g_guest_errors++;
        return;
    }
    if (ks::arbitrate_unlock(addr_of(&m.word)) != ks::kOk) g_guest_errors++;   // waiters: the kernel hands the mutex over
}

// condvarWaitTimeout: releases the mutex in the kernel; a timed-out waiter takes the mutex back itself, as the SDK does.
uint32_t cond_wait(GCond& c, GMutex& m, int64_t timeout_ns) {
    const uint32_t rc = ks::wait_process_wide_key(addr_of(&m.word), addr_of(&c.key), my_tag(), timeout_ns);
    if (rc == ks::kTimedOut) mutex_lock(m);
    else if (rc != ks::kOk) g_guest_errors++;
    return rc;
}

// condvarWake: only enters the kernel when the has-waiters word says someone waits. Caller holds the mutex.
void cond_signal(GCond& c, int32_t count) {
    if (load_word(&c.key) == 0) return;
    if (ks::signal_process_wide_key(addr_of(&c.key), count) != ks::kOk) g_guest_errors++;
}
void cond_broadcast(GCond& c) { cond_signal(c, -1); }

// ---- guest threads ----------------------------------------------------------------------------------------------------------
// A guest thread runs a host closure: its Ctx carries the closure's address in x0 (ThreadParams::arg). The closures live here until
// kernel_reset() has joined every thread.
std::deque<std::unique_ptr<std::function<void()>>> g_bodies;

void guest_entry(::Ctx* c) { (*reinterpret_cast<std::function<void()>*>(nxrt::reg(c, 0)))(); }

uint32_t guest_create(int prio, int core, std::function<void()> body) {
    g_bodies.push_back(std::make_unique<std::function<void()>>(std::move(body)));
    ks::ThreadParams p;
    p.entry = kEntryAddr;
    p.arg = addr_of(g_bodies.back().get());
    p.stack_top = kStackTop;
    p.prio = prio;
    p.core = core;
    p.name = "guest";
    const uint32_t h = ks::thread_create(p);
    CHECK_MSG(h != 0, "thread_create failed");
    return h;
}

uint32_t guest_spawn(int prio, int core, std::function<void()> body) {
    const uint32_t h = guest_create(prio, core, std::move(body));
    CHECK_MSG(ks::thread_start(h) == ks::kOk, "thread_start failed");
    return h;
}

// Scheduling isolation. A contended guest mutex hands ownership straight to the waiter (as Horizon does), so every contended unlock is
// a host thread switch and the throughput of these tests is the host's wake-up latency. The console core model pins every guest thread
// to one host processor and switches Windows' wake-up boost off, so a woken thread cannot run before the scheduler takes the processor
// from whatever else is running on it. With CPU-bound processes of normal priority around (a parallel build, other test runs) that is
// a quantum of 15 ms or more per hand-over, and 20000 hand-overs outlast the 20 s watchdog (docs/debug/180.md). The tests therefore
// run in the high priority class: even the lowest of the six guest bands then ranks above normal load. The class is raised in begin()
// and put back in end(), so it covers exactly the time the kernel is under test.
DWORD g_saved_class = 0;

void raise_priority() {
    if (g_saved_class) return;
    g_saved_class = GetPriorityClass(GetCurrentProcess());
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
}

void restore_priority() {
    if (!g_saved_class) return;
    SetPriorityClass(GetCurrentProcess(), g_saved_class);
    g_saved_class = 0;
}

// Every test starts and ends here: no thread, waiter or handle survives from the previous test.
void begin() {
    raise_priority();
    ks::kernel_reset();
    g_bodies.clear();
    nxrt::fn_clear();
    nxrt::svc_init();
    nxrt::mem::shutdown();
    nxrt::mem::init();
    nxrt::fn_register(kEntryAddr, guest_entry);
    g_guest_errors = 0;
}

void end() {
    ks::kernel_reset();
    g_bodies.clear();
    restore_priority();
}

// Event helpers: KEvent objects made directly, as the SVC layer makes them (readable handle, shared state).
uint32_t new_event(std::shared_ptr<ks::KEvent>& out, bool signaled) {
    out = std::make_shared<ks::KEvent>();
    out->state->signaled = signaled;
    const uint32_t h = ks::handle_add(out);
    CHECK_MSG(h != 0, "handle_add failed");
    return h;
}

void set_event(const std::shared_ptr<ks::KEvent>& e, bool signaled) {
    e->state->signaled = signaled;
    ks::notify_waitable_changed();
}

uint32_t wait_sync(const std::vector<uint32_t>& hs, int64_t timeout_ns, int32_t& index) {
    index = -1;
    return ks::wait_synchronization(hs.data(), static_cast<int32_t>(hs.size()), timeout_ns, &index);
}

// The guest mutex / condvar kernel-protocol test body: `threads` guest threads add to a plain counter `iters` times each under one
// GMutex. Returns the counter. The caller checks it.
uint64_t run_mutex_exclusion(const char* test, int threads, int iters) {
    GMutex m;
    uint64_t counter = 0;
    std::vector<uint32_t> hs;
    for (int t = 0; t < threads; t++) {
        hs.push_back(guest_spawn(40 + (t * 3) % 11, t % 3, [&m, &counter, iters] {
            for (int i = 0; i < iters; i++) {
                mutex_lock(m);
                counter++;
                mutex_unlock(m);
            }
        }));
    }
    for (uint32_t h : hs) join_guest(test, h);
    CHECK_EQ(load_word(&m.word), 0u);
    return counter;
}

// Two guest threads on different cores hand a turn back and forth through a condition variable; returns the rounds each completed.
struct PingPong {
    int a_rounds = 0, b_rounds = 0, turn = 0;
};

PingPong run_pingpong(const char* test, int rounds) {
    GMutex m;
    GCond c;
    int turn = 0;                 // 0: thread A's turn, 1: thread B's turn (guarded by m)
    std::atomic<int> a_rounds{0}, b_rounds{0};
    auto player = [&](int me, std::atomic<int>& done) {
        for (int i = 0; i < rounds; i++) {
            mutex_lock(m);
            while (turn != me) cond_wait(c, m, -1);
            turn = 1 - me;
            done++;
            cond_signal(c, 1);
            mutex_unlock(m);
        }
    };
    const uint32_t a = guest_spawn(44, 0, [&] { player(0, a_rounds); });
    const uint32_t b = guest_spawn(44, 1, [&] { player(1, b_rounds); });
    join_guest(test, a);
    join_guest(test, b);
    return {a_rounds.load(), b_rounds.load(), turn};
}

// One WaitForAddress / SignalToAddress scenario for signal type 2: `waiters` host threads wait (WaitIfEqual) on the word, one signal
// with `count` runs, the word and the number of woken threads are checked, then the rest are released with a plain signal.
void type2_scenario(int waiters, int32_t value, int32_t count, int32_t want_word, int want_woken) {
    alignas(4) uint32_t w = static_cast<uint32_t>(value);
    const uint64_t a = addr_of(&w);
    std::atomic<int> woken{0}, bad{0};
    std::vector<std::thread> ts;
    for (int i = 0; i < waiters; i++) {
        ts.emplace_back([&, a, value] {
            if (ks::wait_for_address(a, 2, value, -1) != ks::kOk) bad++;
            woken++;
        });
    }
    sleep_ms(100);
    CHECK_EQ(ks::signal_to_address(a, 2, value, count), ks::kOk);
    sleep_ms(100);
    CHECK_EQ(woken.load(), want_woken);
    CHECK_EQ(static_cast<int32_t>(w), want_word);
    CHECK_EQ(ks::signal_to_address(a, 0, 0, -1), ks::kOk);
    join_host("signal_to_address_type2", ts);
    CHECK_EQ(woken.load(), waiters);
    CHECK_EQ(bad.load(), 0);
}

// Address semaphore on a guest word: acquire takes one unit (waiting while the count is 0), release adds one and signals one waiter.
void sem_acquire(uint32_t* w) {
    const uint64_t a = addr_of(w);
    for (;;) {
        const uint32_t v = load_word(w);
        if (static_cast<int32_t>(v) > 0 && cas_word(w, v, v - 1)) return;
        ks::wait_for_address(a, 0, 1, -1);          // WaitIfLessThan 1: sleeps while the count is 0
    }
}

void sem_release(uint32_t* w) {
    __atomic_fetch_add(w, 1u, __ATOMIC_SEQ_CST);
    ks::signal_to_address(addr_of(w), 0, 0, 1);
}

}  // namespace

// ---- 1: immediate returns, alignment, the SVC path ------------------------------------------------------------------------

NXTEST(alignment_and_immediate_return) {
    begin();
    alignas(8) uint32_t words[4] = {0, 0, 0, 0};
    const uint64_t bad = addr_of(&words[0]) + 2;   // misaligned by 2
    const uint64_t good = addr_of(&words[1]);
    CHECK_EQ(ks::arbitrate_lock(0x55, bad, 1), ks::kInvalidAddress);
    CHECK_EQ(ks::arbitrate_unlock(bad), ks::kInvalidAddress);
    CHECK_EQ(ks::wait_process_wide_key(bad, good, 1, 0), ks::kInvalidAddress);   // misaligned mutex
    CHECK_EQ(ks::wait_process_wide_key(good, bad, 1, 0), ks::kInvalidAddress);   // misaligned key
    CHECK_EQ(ks::signal_process_wide_key(bad, 1), ks::kInvalidAddress);
    CHECK_EQ(ks::wait_for_address(bad, 2, 0, 0), ks::kInvalidAddress);
    CHECK_EQ(ks::signal_to_address(bad, 0, 0, 1), ks::kInvalidAddress);

    // The word is not (owner | wait bit): ArbitrateLock returns at once, without sleeping.
    alignas(4) uint32_t w = 0;
    CHECK_EQ(ks::arbitrate_lock(0x55, addr_of(&w), 1), ks::kOk);   // word 0
    w = 0x55;                                                      // the owner with no wait bit
    CHECK_EQ(ks::arbitrate_lock(0x55, addr_of(&w), 1), ks::kOk);
    CHECK_EQ(w, 0x55u);                                            // and the word is not touched

    // The same through the SVC dispatcher (ArbitrateLock 0x1A: w0 owner, x1 address, w2 tag).
    ::Ctx c{};
    nxrt::reg(&c, 0) = 0x55;
    nxrt::reg(&c, 1) = addr_of(&w);
    nxrt::reg(&c, 2) = 7;
    nxrt::svc_dispatch(&c, 0x1A);
    CHECK_EQ(static_cast<uint32_t>(nxrt::reg(&c, 0)), ks::kOk);
    nxrt::reg(&c, 1) = bad;
    nxrt::svc_dispatch(&c, 0x1A);
    CHECK_EQ(static_cast<uint32_t>(nxrt::reg(&c, 0)), ks::kInvalidAddress);
    nxrt::reg(&c, 0) = bad;                                        // ArbitrateUnlock 0x1B: x0 address
    nxrt::svc_dispatch(&c, 0x1B);
    CHECK_EQ(static_cast<uint32_t>(nxrt::reg(&c, 0)), ks::kInvalidAddress);
    end();
}

// ---- 2: mutual exclusion of the SDK mutex --------------------------------------------------------------------------------

NXTEST(mutex_mutual_exclusion) {
    begin();
    const uint64_t total = run_mutex_exclusion("mutex_mutual_exclusion", 8, 20000);
    CHECK_EQ(total, 8ull * 20000ull);
    CHECK_EQ(g_guest_errors.load(), 0);
    end();
}

// ---- 3: handoff order by priority -----------------------------------------------------------------------------------------

NXTEST(mutex_handoff_follows_priority) {
    begin();
    GMutex m;
    std::atomic<int> got{0};
    int order[3] = {0, 0, 0};    // written by each acquirer while it holds m; read after the joins
    mutex_lock(m);               // the test thread owns the mutex
    const int prios[3] = {50, 40, 45};
    std::vector<uint32_t> hs;
    for (int k = 0; k < 3; k++) {
        const int prio = prios[k];
        hs.push_back(guest_spawn(prio, k % 3, [&m, &got, &order, prio] {
            mutex_lock(m);
            order[got++] = prio;
            mutex_unlock(m);
        }));
        // The new waiter queues behind the previous ones: wait until the word shows waiters, then give it time to be queued.
        CHECK_MSG(wait_until([&m] { return (load_word(&m.word) & kWaitMask) != 0; }), "a waiter never announced itself");
        sleep_ms(30);
    }
    sleep_ms(100);
    CHECK_EQ(got.load(), 0);     // nobody gets it while the owner holds it
    mutex_unlock(m);
    for (uint32_t h : hs) join_guest("mutex_handoff_follows_priority", h);
    CHECK_EQ(got.load(), 3);
    // Queued in the order 50, 40, 45: priority hands over 40 (highest), 45, 50. FIFO would give 50 first.
    CHECK_EQ(order[0], 40);
    CHECK_EQ(order[1], 45);
    CHECK_EQ(order[2], 50);
    CHECK_EQ(load_word(&m.word), 0u);
    end();
}

// ---- 4: the wait-bit protocol --------------------------------------------------------------------------------------------

NXTEST(mutex_wait_bit_protocol) {
    begin();
    GMutex m;
    const uint32_t me = my_tag();
    std::atomic<bool> got1{false}, got2{false}, rel1{false}, rel2{false};
    mutex_lock(m);
    CHECK_EQ(load_word(&m.word), me);
    const uint32_t w1 = guest_spawn(40, 1, [&m, &got1, &rel1] {
        mutex_lock(m);
        got1 = true;
        while (!rel1.load()) sleep_ms(1);
        mutex_unlock(m);
    });
    CHECK_MSG(wait_until([&m] { return (load_word(&m.word) & kWaitMask) != 0; }), "first waiter never set the wait bit");
    CHECK_EQ(load_word(&m.word), me | kWaitMask);            // one waiter: owner | wait bit
    const uint32_t w2 = guest_spawn(45, 2, [&m, &got2, &rel2] {
        mutex_lock(m);
        got2 = true;
        while (!rel2.load()) sleep_ms(1);
        mutex_unlock(m);
    });
    sleep_ms(50);
    CHECK_EQ(load_word(&m.word), me | kWaitMask);            // two waiters queued: still owner | wait bit
    CHECK(!got1.load() && !got2.load());
    mutex_unlock(m);                                          // hands over to the best waiter (prio 40)
    CHECK_MSG(wait_until([&got1] { return got1.load(); }), "waiter 1 never acquired the mutex");
    CHECK_EQ(load_word(&m.word), w1 | kWaitMask);            // owner w1, and w2 still waits: the bit stays set
    CHECK(!got2.load());
    rel1 = true;                                              // w1 unlocks: hands over to w2, which is the last waiter
    CHECK_MSG(wait_until([&got2] { return got2.load(); }), "waiter 2 never acquired the mutex");
    CHECK_EQ(load_word(&m.word), w2);                        // owner w2, no waiters left: the bit is clear
    rel2 = true;
    join_guest("mutex_wait_bit_protocol", w1);
    join_guest("mutex_wait_bit_protocol", w2);
    CHECK_EQ(load_word(&m.word), 0u);                        // last owner unlocked: free
    CHECK_EQ(g_guest_errors.load(), 0);
    end();
}

// ---- 5: condvar ping-pong -------------------------------------------------------------------------------------------------

NXTEST(condvar_pingpong_round_trips) {
    begin();
    constexpr int kRounds = 20000;
    const PingPong pp = run_pingpong("condvar_pingpong_round_trips", kRounds);
    CHECK_EQ(pp.a_rounds, kRounds);
    CHECK_EQ(pp.b_rounds, kRounds);
    CHECK_EQ(pp.turn, 0);         // an even number of hand-overs
    CHECK_EQ(g_guest_errors.load(), 0);
    end();
}

// ---- 6: broadcast wakes all ------------------------------------------------------------------------------------------------

NXTEST(condvar_broadcast_wakes_all) {
    begin();
    GMutex m;
    GCond c;
    bool go = false;              // guarded by m
    std::atomic<int> entered{0}, woke{0};
    std::vector<uint32_t> hs;
    for (int i = 0; i < 6; i++) {
        hs.push_back(guest_spawn(44 + i, i % 3, [&] {
            mutex_lock(m);
            entered++;
            while (!go) cond_wait(c, m, -1);
            woke++;
            mutex_unlock(m);
        }));
    }
    CHECK_MSG(wait_until([&entered] { return entered.load() == 6; }), "not all six waiters entered");
    sleep_ms(50);                 // let them sleep in the kernel
    mutex_lock(m);
    CHECK_EQ(load_word(&c.key), 1u);      // waiters present: the has-waiters word is 1
    go = true;
    cond_broadcast(c);                    // count -1: all
    mutex_unlock(m);
    for (uint32_t h : hs) join_guest("condvar_broadcast_wakes_all", h);
    CHECK_EQ(woke.load(), 6);
    CHECK_EQ(load_word(&c.key), 0u);
    CHECK_EQ(g_guest_errors.load(), 0);
    end();
}

// ---- 7: signal one at a time ------------------------------------------------------------------------------------------------

NXTEST(condvar_signal_one_at_a_time) {
    begin();
    GMutex m;
    GCond c;
    int tokens = 0;               // guarded by m
    std::atomic<int> entered{0}, done{0};
    std::vector<uint32_t> hs;
    for (int i = 0; i < 6; i++) {
        hs.push_back(guest_spawn(44, i % 3, [&] {
            mutex_lock(m);
            entered++;
            while (tokens == 0) cond_wait(c, m, -1);
            tokens--;
            done++;
            mutex_unlock(m);
        }));
    }
    CHECK_MSG(wait_until([&entered] { return entered.load() == 6; }), "not all six waiters entered");
    sleep_ms(50);
    for (int k = 1; k <= 6; k++) {
        mutex_lock(m);
        tokens++;
        cond_signal(c, 1);
        // The has-waiters word is 1 while anybody is still waiting, and 0 after the last one has been taken off the queue.
        CHECK_EQ(load_word(&c.key), k < 6 ? 1u : 0u);
        mutex_unlock(m);
        sleep_ms(50);
        CHECK_MSG(done.load() == k, "a signal with count 1 must wake exactly one waiter");
    }
    for (uint32_t h : hs) join_guest("condvar_signal_one_at_a_time", h);
    CHECK_EQ(done.load(), 6);
    CHECK_EQ(tokens, 0);
    CHECK_EQ(g_guest_errors.load(), 0);
    end();
}

// ---- 8: WaitProcessWideKey timeout -------------------------------------------------------------------------------------------

NXTEST(wait_process_wide_key_timeout) {
    begin();
    GMutex m;
    GCond c;
    const uint32_t me = my_tag();
    // Raw kernel call: the caller holds the mutex word; a timed-out wait releases it and does not hand it back.
    m.word = me;
    const auto t0 = Clock::now();
    const uint32_t rc = ks::wait_process_wide_key(addr_of(&m.word), addr_of(&c.key), me, 30 * kMs);
    const double el = ms_since(t0);
    CHECK_EQ(rc, ks::kTimedOut);
    CHECK_MSG(slept_at_least(el, 30.0), "the timeout returned before 30 ms");
    CHECK_MSG(el < 2000.0, "the timeout took far longer than 30 ms");
    CHECK_EQ(load_word(&m.word), 0u);     // released by the call
    CHECK_EQ(load_word(&c.key), 0u);      // no waiter left: the has-waiters word is back to 0
    // Through the SDK wrapper: the timed-out waiter takes the mutex back, so the caller holds it again.
    mutex_lock(m);
    const auto t1 = Clock::now();
    const uint32_t rc2 = cond_wait(c, m, 30 * kMs);
    const double el2 = ms_since(t1);
    CHECK_EQ(rc2, ks::kTimedOut);
    CHECK_MSG(slept_at_least(el2, 30.0), "the wrapper timeout returned before 30 ms");
    CHECK_EQ(load_word(&m.word), me);     // re-locked by the SDK path
    mutex_unlock(m);
    CHECK_EQ(load_word(&m.word), 0u);
    CHECK_EQ(load_word(&c.key), 0u);
    CHECK_EQ(g_guest_errors.load(), 0);
    end();
}

// ---- 9: producer / consumer --------------------------------------------------------------------------------------------------

NXTEST(producer_consumer_exactly_once) {
    begin();
    constexpr int kItems = 30000;
    GMutex m;
    GCond not_empty;
    std::deque<int> queue;        // guarded by m
    std::vector<int> seen(kItems, 0);
    int producers_left = 2;
    int consumed = 0;
    std::vector<uint32_t> hs;
    for (int p = 0; p < 2; p++) {
        hs.push_back(guest_spawn(44, p, [&, p] {
            for (int i = 0; i < kItems / 2; i++) {
                mutex_lock(m);
                queue.push_back(p * (kItems / 2) + i);
                cond_signal(not_empty, 1);
                mutex_unlock(m);
            }
            mutex_lock(m);
            if (--producers_left == 0) cond_broadcast(not_empty);   // wake consumers that must now exit
            mutex_unlock(m);
        }));
    }
    for (int k = 0; k < 3; k++) {
        hs.push_back(guest_spawn(44, k, [&] {
            for (;;) {
                mutex_lock(m);
                while (queue.empty() && producers_left > 0) cond_wait(not_empty, m, -1);
                if (queue.empty()) {
                    mutex_unlock(m);
                    return;
                }
                const int item = queue.front();
                queue.pop_front();
                seen[static_cast<size_t>(item)]++;
                consumed++;
                mutex_unlock(m);
            }
        }));
    }
    for (uint32_t h : hs) join_guest("producer_consumer_exactly_once", h);
    CHECK_EQ(consumed, kItems);
    bool once = true;
    for (int v : seen) once = once && v == 1;
    CHECK_MSG(once, "an item was lost or consumed twice");
    CHECK_EQ(g_guest_errors.load(), 0);
    end();
}

// ---- 10: WaitForAddress conditions ----------------------------------------------------------------------------------------

NXTEST(wait_for_address_conditions) {
    begin();
    alignas(4) uint32_t w = 5;
    const uint64_t a = addr_of(&w);
    // Condition not met: InvalidState at once, the word is not touched.
    CHECK_EQ(ks::wait_for_address(a, 0, 5, 0), ks::kInvalidState);   // WaitIfLessThan: 5 < 5 is false
    CHECK_EQ(ks::wait_for_address(a, 1, 5, 0), ks::kInvalidState);   // DecrementAndWaitIfLessThan: same condition
    CHECK_EQ(w, 5u);
    CHECK_EQ(ks::wait_for_address(a, 2, 4, 0), ks::kInvalidState);   // WaitIfEqual: 5 != 4
    CHECK_EQ(w, 5u);
    // Condition met with timeout 0: TimedOut at once.
    w = 4;
    CHECK_EQ(ks::wait_for_address(a, 0, 5, 0), ks::kTimedOut);       // 4 < 5
    CHECK_EQ(ks::wait_for_address(a, 2, 4, 0), ks::kTimedOut);       // 4 == 4
    CHECK_EQ(w, 4u);                                                 // types 0 and 2 never write the word
    // Invalid arbitration type: 3 is WaitIfEqual64 (below), 4 is the first one past the table.
    CHECK_EQ(ks::wait_for_address(a, 4, 0, 0), ks::kInvalidEnum);
    CHECK_EQ(ks::wait_for_address(a, 4, 0, 30 * kMs), ks::kInvalidEnum);
    end();
}

// ---- 10b: WaitIfEqual64 (type 3, SDK 22: nn::os::WaitForAddress behind std::atomic_wait) -------------------------------------------
// The whole 64-bit x2 is compared with the doubleword at an 8-byte aligned address (mesosphere checks that alignment for this type only).
NXTEST(wait_for_address_if_equal_64) {
    begin();
    alignas(8) uint64_t d[2] = {0x0000000100000005ull, 0};
    const uint64_t a = addr_of(&d[0]);
    CHECK_EQ(ks::wait_for_address(a, 3, 0x0000000200000005ll, 0), ks::kInvalidState);   // low halves equal, high halves differ
    CHECK_EQ(ks::wait_for_address(a, 3, 0x0000000100000005ll, 0), ks::kTimedOut);       // all 64 bits equal, timeout 0
    CHECK_EQ(ks::wait_for_address(a, 3, 0x0000000100000005ll, 30 * kMs), ks::kTimedOut);
    CHECK_EQ(d[0], 0x0000000100000005ull);                                               // the doubleword is never written
    CHECK_EQ(ks::wait_for_address(a + 4, 3, 0, 0), ks::kInvalidAddress);                // 4-byte aligned is not enough for type 3
    CHECK_EQ(ks::wait_for_address(a + 4, 2, 1, 0), ks::kTimedOut);                      // ... but is for the 32-bit WaitIfEqual (the word there is 1)
    end();
}

// DecrementAndWaitIfLessThan is one atomic step: a guest thread adding to the word with its own atomics at the same time loses nothing
// (the decrement was a plain load and store, which dropped the other thread's updates in between).
NXTEST(wait_for_address_decrement_is_atomic) {
    begin();
    alignas(4) uint32_t w = 0;
    const uint64_t a = addr_of(&w);
    constexpr int kN = 200000;
    std::atomic<bool> go{false};
    std::thread inc([&] {
        while (!go.load()) {}
        for (int i = 0; i < kN; ++i) __atomic_fetch_add(&w, 1u, __ATOMIC_SEQ_CST);
    });
    int decs = 0;
    go.store(true);
    for (int i = 0; i < kN; ++i)
        if (ks::wait_for_address(a, 1, 0x7fffffff, 0) == ks::kTimedOut) decs++;   // always below INT_MAX: decrements, then times out at once
    inc.join();
    CHECK_EQ(decs, kN);
    CHECK_EQ(w, 0u);
    end();
}

// ---- 11: WaitForAddress timeouts and the decrement -----------------------------------------------------------------------

NXTEST(wait_for_address_timeout_and_decrement) {
    begin();
    alignas(4) uint32_t w = 4;
    const uint64_t a = addr_of(&w);
    auto t0 = Clock::now();
    CHECK_EQ(ks::wait_for_address(a, 0, 5, 30 * kMs), ks::kTimedOut);   // WaitIfLessThan, met, 30 ms
    CHECK_MSG(slept_at_least(ms_since(t0), 30.0), "WaitIfLessThan returned before its 30 ms timeout");
    CHECK_EQ(w, 4u);
    t0 = Clock::now();
    CHECK_EQ(ks::wait_for_address(a, 2, 4, 30 * kMs), ks::kTimedOut);   // WaitIfEqual, met, 30 ms
    CHECK_MSG(slept_at_least(ms_since(t0), 30.0), "WaitIfEqual returned before its 30 ms timeout");
    CHECK_EQ(w, 4u);
    // DecrementAndWaitIfLessThan: the word goes down by one when the thread goes to sleep.
    t0 = Clock::now();
    CHECK_EQ(ks::wait_for_address(a, 1, 5, 30 * kMs), ks::kTimedOut);
    CHECK_MSG(slept_at_least(ms_since(t0), 30.0), "DecrementAndWaitIfLessThan returned before its 30 ms timeout");
    CHECK_EQ(w, 3u);
    end();
}

// ---- 12: SignalToAddress type 0 wakes `count` ----------------------------------------------------------------------------

NXTEST(signal_to_address_wakes_count) {
    begin();
    alignas(4) uint32_t w = 0;
    const uint64_t a = addr_of(&w);
    std::atomic<int> done{0}, bad{0};
    std::vector<std::thread> ts;
    for (int i = 0; i < 3; i++) {
        ts.emplace_back([&, a] {
            if (ks::wait_for_address(a, 2, 0, -1) != ks::kOk) bad++;   // WaitIfEqual 0, no timeout
            done++;
        });
    }
    sleep_ms(100);                                                     // all three sleep on the address
    CHECK_EQ(ks::signal_to_address(a, 0, 0, 1), ks::kOk);
    sleep_ms(100);
    CHECK_EQ(done.load(), 1);                                          // count 1 wakes exactly one
    CHECK_EQ(ks::signal_to_address(a, 0, 0, -1), ks::kOk);             // -1: the rest
    CHECK_MSG(wait_until([&done] { return done.load() == 3; }), "signal -1 did not wake the other two");
    join_host("signal_to_address_wakes_count", ts);
    CHECK_EQ(bad.load(), 0);
    end();
}

// ---- 13: SignalToAddress type 1 (SignalAndIncrementIfEqual) ----------------------------------------------------------------

NXTEST(signal_to_address_increment_if_equal) {
    begin();
    alignas(4) uint32_t w = 3;
    const uint64_t a = addr_of(&w);
    std::atomic<int> woken{0};
    std::thread waiter([&, a] {
        if (ks::wait_for_address(a, 2, 3, -1) == ks::kOk) woken++;     // WaitIfEqual 3
    });
    sleep_ms(100);
    CHECK_EQ(ks::signal_to_address(a, 1, 2, 1), ks::kInvalidState);    // value 2 != word 3: nothing changes, nobody wakes
    sleep_ms(50);
    CHECK_EQ(w, 3u);
    CHECK_EQ(woken.load(), 0);
    CHECK_EQ(ks::signal_to_address(a, 1, 3, 1), ks::kOk);              // equal: word 3 -> 4 and one waiter wakes
    CHECK_MSG(wait_until([&woken] { return woken.load() == 1; }), "the matching signal did not wake the waiter");
    waiter.join();
    CHECK_EQ(w, 4u);
    CHECK_EQ(ks::signal_to_address(a, 1, 4, 1), ks::kOk);              // no waiters left: still increments
    CHECK_EQ(w, 5u);
    end();
}

// ---- 14: SignalToAddress type 2 (SignalAndModifyBasedOnWaitingThreadCountIfEqual) ---------------------------------------
// The rules below are the ones kernel_sync.cpp implements (Switchbrew names the call but does not spell the rules out, and no
// primary source for them could be fetched). The test pins the implementation's table.

NXTEST(signal_to_address_modify_by_waiting_count) {
    begin();
    type2_scenario(0, 10, 1, 11, 0);     // no waiters: value + 1
    type2_scenario(0, 10, -1, 11, 0);    // no waiters, count <= 0: value + 1
    type2_scenario(1, 10, 1, 9, 1);      // 1 waiter <= count 1: value - 1, wakes it
    type2_scenario(2, 10, 1, 10, 1);     // 2 waiters > count 1: value unchanged, wakes one
    type2_scenario(2, 10, -1, 8, 2);     // count <= 0 with waiters: value - 2, wakes all
    end();
}

// ---- 15: address semaphore ping-pong ----------------------------------------------------------------------------------------

NXTEST(address_semaphore_pingpong) {
    begin();
    alignas(4) uint32_t s_ab = 0, s_ba = 0;
    constexpr int kRounds = 10000;
    std::atomic<int> a_rounds{0}, b_rounds{0};
    const uint32_t a = guest_spawn(44, 0, [&] {
        for (int i = 0; i < kRounds; i++) {
            sem_release(&s_ab);
            sem_acquire(&s_ba);
            a_rounds++;
        }
    });
    const uint32_t b = guest_spawn(44, 1, [&] {
        for (int i = 0; i < kRounds; i++) {
            sem_acquire(&s_ab);
            sem_release(&s_ba);
            b_rounds++;
        }
    });
    join_guest("address_semaphore_pingpong", a);
    join_guest("address_semaphore_pingpong", b);
    CHECK_EQ(a_rounds.load(), kRounds);
    CHECK_EQ(b_rounds.load(), kRounds);
    CHECK_EQ(load_word(&s_ab), 0u);       // every unit released was taken
    CHECK_EQ(load_word(&s_ba), 0u);
    end();
}

// ---- 16: WaitSynchronization index and persistence ----------------------------------------------------------------------

NXTEST(wait_synchronization_index_and_persistence) {
    begin();
    std::shared_ptr<ks::KEvent> e0, e1, e2;
    const uint32_t h0 = new_event(e0, false), h1 = new_event(e1, true), h2 = new_event(e2, false);
    int32_t idx = -1;
    const std::vector<uint32_t> all = {h0, h1, h2};
    const std::vector<uint32_t> reordered = {h2, h1, h0};
    const std::vector<uint32_t> with_bad = {h0, 0xDEADu};
    uint32_t rc = wait_sync(all, 0, idx);
    CHECK_EQ(rc, ks::kOk);
    CHECK_EQ(idx, 1);                                   // only the second one is signaled
    CHECK(e1->state->signaled);                         // events are manual reset: still signaled after the wait
    rc = wait_sync(all, 0, idx);                        // and it satisfies the next wait too
    CHECK_EQ(rc, ks::kOk);
    CHECK_EQ(idx, 1);
    set_event(e2, true);                                // two signaled: the lowest index wins
    rc = wait_sync(all, 0, idx);
    CHECK_EQ(rc, ks::kOk);
    CHECK_EQ(idx, 1);
    rc = wait_sync(reordered, 0, idx);                  // the same two, reordered: index 0 is h2
    CHECK_EQ(rc, ks::kOk);
    CHECK_EQ(idx, 0);
    CHECK(e2->state->signaled);
    rc = wait_sync(with_bad, 0, idx);                   // an unknown handle
    CHECK_EQ(rc, ks::kInvalidHandle);
    CHECK_EQ(idx, -1);
    end();
}

// ---- 17b: timeouts under the Switch 2 profile --------------------------------------------------------------------------

// The system tick is 31.25 MHz on Switch 2 (19.2 MHz on Switch 1) and kernel::tick_hz caches it at first use, so this runs in a child process that
// picks the platform first. A wait timeout is counted in ticks: counted at the wrong rate it ends early (here after 61 ms of a 100 ms wait).
NXTEST_CHILD(kernel_sync_switch2_timeouts) {
    nxtest::use_platform_for_process(nxrt::game::Platform::Switch2);
    if (ks::tick_hz() != ks::kTickHzSwitch2) return 10;
    begin();
    int rc = 0;
    std::shared_ptr<ks::KEvent> e;
    const uint32_t h = new_event(e, false);
    int32_t idx = -1;
    auto t0 = Clock::now();
    if (wait_sync({h}, 100 * kMs, idx) != ks::kTimedOut) rc = 11;
    if (!slept_at_least(ms_since(t0), 100.0)) rc = 12;          // WaitSynchronization timeout
    alignas(4) uint32_t w = 4;
    t0 = Clock::now();
    if (ks::wait_for_address(addr_of(&w), 2, 4, 100 * kMs) != ks::kTimedOut) rc = 13;
    if (!slept_at_least(ms_since(t0), 100.0)) rc = 14;          // WaitForAddress timeout
    end();
    return rc;
}

NXTEST(timeouts_run_in_real_time_under_switch2) {
    const int rc = nxtest::run_child("kernel_sync_switch2_timeouts");
    CHECK_MSG(rc == 0, "child kernel_sync_switch2_timeouts returned " + std::to_string(rc) + " (10 tick rate, 11/12 WaitSynchronization, 13/14 WaitForAddress)");
}

// ---- 17: WaitSynchronization timeouts ----------------------------------------------------------------------------------

NXTEST(wait_synchronization_timeouts) {
    begin();
    std::shared_ptr<ks::KEvent> e;
    const uint32_t h = new_event(e, false);
    int32_t idx = -1;
    CHECK_EQ(wait_sync({h}, 0, idx), ks::kTimedOut);     // timeout 0 and nothing signaled
    const auto t0 = Clock::now();
    CHECK_EQ(wait_sync({h}, 30 * kMs, idx), ks::kTimedOut);
    const double el = ms_since(t0);
    CHECK_MSG(slept_at_least(el, 30.0), "a 30 ms wait returned before 30 ms");
    CHECK_MSG(el < 2000.0, "a 30 ms wait took far longer than 30 ms");
    end();
}

// ---- 18: WaitSynchronization woken by another thread ----------------------------------------------------------------------

NXTEST(wait_synchronization_cross_thread_wake) {
    begin();
    std::shared_ptr<ks::KEvent> e;
    const uint32_t h = new_event(e, false);
    std::thread setter([&e] {
        sleep_ms(20);
        set_event(e, true);
    });
    int32_t idx = -1;
    const auto t0 = Clock::now();
    const uint32_t rc = wait_sync({h}, -1, idx);
    const double el = ms_since(t0);
    setter.join();
    CHECK_EQ(rc, ks::kOk);
    CHECK_EQ(idx, 0);
    CHECK_MSG(el < 200.0, "the waiter was not woken promptly by the signal (more than 200 ms)");
    end();
}

// ---- 19: a thread handle is waitable until the thread exits ----------------------------------------------------------------

NXTEST(wait_on_thread_handle_until_exit) {
    begin();
    const uint32_t th = guest_spawn(44, 0, [] { sleep_ms(100); });
    auto t = ks::handle_as<ks::KThread>(th);
    CHECK(t != nullptr);
    CHECK(t && !t->signaled());           // still running
    int32_t idx = -1;
    const uint32_t rc = wait_sync({th}, -1, idx);
    CHECK_EQ(rc, ks::kOk);
    CHECK_EQ(idx, 0);
    CHECK(t && t->signaled());            // exited
    join_guest("wait_on_thread_handle_until_exit", th);
    end();
}

// ---- 20: CancelSynchronization releases a blocked waiter -----------------------------------------------------------------

NXTEST(cancel_releases_blocked_waiter) {
    begin();
    std::shared_ptr<ks::KEvent> e;
    const uint32_t evh = new_event(e, false);
    std::atomic<uint32_t> rc{99};
    const uint32_t wth = guest_spawn(44, 0, [&rc, evh] {
        int32_t idx = -1;
        const uint32_t hs[1] = {evh};
        rc = ks::wait_synchronization(hs, 1, -1, &idx);   // no timeout: only a cancel or the event ends it
    });
    sleep_ms(50);
    CHECK_EQ(ks::cancel_synchronization(wth), ks::kOk);
    join_guest("cancel_releases_blocked_waiter", wth);
    CHECK_EQ(rc.load(), ks::kCancelled);
    end();
}

// ---- 21: a cancel issued before the wait, invalid handles ----------------------------------------------------------------

NXTEST(cancel_pending_then_normal_wait) {
    begin();
    std::shared_ptr<ks::KEvent> e;
    const uint32_t evh = new_event(e, false);
    std::atomic<uint32_t> first{99};
    std::atomic<int> second_ms{-1};
    std::atomic<uint32_t> second{99};
    const uint32_t th = guest_create(44, 0, [&, evh] {
        int32_t idx = -1;
        const uint32_t hs[1] = {evh};
        first = ks::wait_synchronization(hs, 1, -1, &idx);     // consumes the pending cancel: Cancelled at once
        const auto t0 = Clock::now();
        second = ks::wait_synchronization(hs, 1, 100 * kMs, &idx); // a normal wait again: it times out
        second_ms = static_cast<int>(ms_since(t0));
    });
    CHECK_EQ(ks::cancel_synchronization(th), ks::kOk);         // the thread has not started: the cancel is remembered
    CHECK_EQ(ks::thread_start(th), ks::kOk);
    join_guest("cancel_pending_then_normal_wait", th);
    CHECK_EQ(first.load(), ks::kCancelled);
    CHECK_EQ(second.load(), ks::kTimedOut);
    CHECK_MSG(second_ms.load() >= 99, "the second wait returned before its 100 ms timeout");
    CHECK_EQ(ks::cancel_synchronization(0xDEADu), ks::kInvalidHandle);   // not a handle at all
    CHECK_EQ(ks::cancel_synchronization(evh), ks::kInvalidHandle);       // an event is not a thread
    end();
}

// ---- 22: many waiters on one manual-reset event --------------------------------------------------------------------------

NXTEST(many_waiters_one_signal) {
    begin();
    std::shared_ptr<ks::KEvent> e;
    const uint32_t h = new_event(e, false);
    constexpr int kWaiters = 20;
    std::atomic<int> ok{0};
    std::vector<std::thread> ts;
    for (int i = 0; i < kWaiters; i++) {
        ts.emplace_back([&, h] {
            int32_t idx = -1;
            const uint32_t hs[1] = {h};
            const uint32_t rc = ks::wait_synchronization(hs, 1, -1, &idx);
            if (rc == ks::kOk && idx == 0) ok++;
        });
    }
    sleep_ms(100);
    set_event(e, true);
    join_host("many_waiters_one_signal", ts);
    CHECK_EQ(ok.load(), kWaiters);                // one signal released all of them
    CHECK(e->state->signaled);
    end();
}

// ---- 23: stress -----------------------------------------------------------------------------------------------------------

NXTEST(stress_random_operations) {
    begin();
    constexpr int kThreads = 6;
    constexpr int kMutexes = 4, kConds = 2, kSems = 2, kEvents = 2;
    GMutex mu[kMutexes];
    uint64_t counter[kMutexes] = {};                  // each guarded by mu[i]
    GCond cv[kConds];
    alignas(4) uint32_t sem[kSems] = {0, 0};
    std::atomic<int64_t> sem_rel[kSems] = {}, sem_acq[kSems] = {};
    std::shared_ptr<ks::KEvent> evs[kEvents];
    uint32_t evh[kEvents];
    for (int k = 0; k < kEvents; k++) evh[k] = new_event(evs[k], false);
    std::vector<std::vector<uint64_t>> made(kThreads, std::vector<uint64_t>(kMutexes, 0));   // per thread, per mutex increments
    std::atomic<uint64_t> total_ops{0};
    const auto deadline = Clock::now() + std::chrono::seconds(2);
    std::vector<uint32_t> hs;
    for (int t = 0; t < kThreads; t++) {
        hs.push_back(guest_spawn(40 + t * 2, t % 3, [&, t] {
            std::mt19937 rng(static_cast<uint32_t>(1234 + t));
            auto& mine = made[static_cast<size_t>(t)];
            uint64_t ops = 0;
            while (Clock::now() < deadline) {
                ops++;
                const int r = static_cast<int>(rng() % 100);
                const int i = static_cast<int>(rng() % kMutexes);
                const int j = static_cast<int>(rng() % kConds);
                if (r < 45) {                                   // lock, counter++
                    mutex_lock(mu[i]);
                    counter[i]++;
                    mine[static_cast<size_t>(i)]++;
                    mutex_unlock(mu[i]);
                } else if (r < 60) {                            // condvar wait with a short timeout; the caller holds the mutex on return
                    mutex_lock(mu[i]);
                    cond_wait(cv[j], mu[i], static_cast<int64_t>(1 + rng() % 3) * kMs);
                    counter[i]++;
                    mine[static_cast<size_t>(i)]++;
                    mutex_unlock(mu[i]);
                } else if (r < 75) {                            // signal (count 1 or -1) under the mutex, counter++
                    mutex_lock(mu[i]);
                    cond_signal(cv[j], (rng() & 1u) ? 1 : -1);
                    counter[i]++;
                    mine[static_cast<size_t>(i)]++;
                    mutex_unlock(mu[i]);
                } else if (r < 90) {                            // address semaphores: release, or take / wait up to 1 ms
                    const int s = static_cast<int>(rng() % kSems);
                    if (rng() & 1u) {
                        sem_release(&sem[s]);
                        sem_rel[s]++;
                    } else {
                        const uint32_t v = load_word(&sem[s]);
                        if (static_cast<int32_t>(v) > 0 && cas_word(&sem[s], v, v - 1)) sem_acq[s]++;
                        else ks::wait_for_address(addr_of(&sem[s]), 0, 1, 1 * kMs);
                    }
                } else {                                        // events: wait up to 1 ms
                    int32_t idx = -1;
                    const uint32_t hs1[1] = {evh[j % kEvents]};
                    ks::wait_synchronization(hs1, 1, 1 * kMs, &idx);
                }
            }
            total_ops += ops;
        }));
    }
    for (uint32_t h : hs) join_guest("stress_random_operations", h);
    CHECK_MSG(total_ops.load() > 1000, "the stress loop ran far too few operations");
    for (int i = 0; i < kMutexes; i++) {
        uint64_t want = 0;
        for (int t = 0; t < kThreads; t++) want += made[static_cast<size_t>(t)][static_cast<size_t>(i)];
        CHECK_MSG(counter[i] == want, "a mutex-protected counter lost updates");
        CHECK_EQ(load_word(&mu[i].word), 0u);
    }
    for (int s = 0; s < kSems; s++) {
        const int64_t value = static_cast<int64_t>(static_cast<int32_t>(load_word(&sem[s])));
        CHECK_EQ(value, sem_rel[s].load() - sem_acq[s].load());
    }
    CHECK_EQ(g_guest_errors.load(), 0);
    end();
}

// ---- 24: after kernel_reset a second run works, and nothing is left behind ---------------------------------------------------

NXTEST(exclusion_twice_after_reset) {
    begin();
    CHECK_EQ(run_mutex_exclusion("exclusion_twice_after_reset", 8, 20000), 8ull * 20000ull);
    CHECK_EQ(g_guest_errors.load(), 0);
    end();
    CHECK_EQ(ks::handle_count(), 0u);       // the reset left no handle (thread, event, ...) behind
    begin();
    CHECK_EQ(run_mutex_exclusion("exclusion_twice_after_reset", 8, 20000), 8ull * 20000ull);
    CHECK_EQ(g_guest_errors.load(), 0);
    alignas(4) uint32_t w = 0;
    const auto t0 = Clock::now();
    CHECK_EQ(ks::wait_for_address(addr_of(&w), 2, 0, 20 * kMs), ks::kTimedOut);   // no stale waiter interferes
    CHECK_MSG(slept_at_least(ms_since(t0), 20.0), "a fresh wait returned before its timeout");
    CHECK_EQ(ks::signal_to_address(addr_of(&w), 0, 0, -1), ks::kOk);
    end();
}

// ---- 25: hand-overs keep their pace while other processes burn CPU on the same processors ----------------------------------------
// Regression test for docs/debug/180.md. Child processes of normal priority spin on exactly the host processors the console core
// model gives the guest threads (two per processor), then the two hand-over heavy workloads run: the condvar ping-pong and the
// 8-thread mutex. They need tens of thousands of thread switches; unless the tests run above normal priority (begin()), each switch
// waits for a scheduler quantum and the 20 s watchdog fires.

NXTEST_CHILD(kernel_sync_hog) {
    const double seconds = argc > 1 ? std::atof(argv[1]) : 60.0;
    if (argc > 0) {
        if (HANDLE ready = OpenSemaphoreA(SEMAPHORE_MODIFY_STATE, FALSE, argv[0])) {
            ReleaseSemaphore(ready, 1, nullptr);
            CloseHandle(ready);
        }
    }
    const auto until = Clock::now() + std::chrono::duration<double>(seconds);
    volatile uint64_t sink = 0;
    while (Clock::now() < until)
        for (int i = 0; i < 100000; i++) sink = sink + 1;
    return 0;
}

namespace {

// CPU-bound child processes at normal priority, pinned to the console cores' host processors. A job object ends them with the test.
class Hogs {
public:
    ~Hogs() { stop(); }

    bool start() {
        const ks::SchedInfo info = ks::sched_info();
        DWORD_PTR mask = 0;
        int count = 0;
        if (info.active) {
            for (int i = 0; i < 3; i++) mask |= static_cast<DWORD_PTR>(info.host_masks[i]);
            count = 6;
        } else {
            DWORD_PTR sys = 0;
            GetProcessAffinityMask(GetCurrentProcess(), &mask, &sys);
            count = static_cast<int>(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
        }
        job_ = CreateJobObjectA(nullptr, nullptr);
        if (!job_) return false;
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION lim{};
        lim.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &lim, sizeof lim);
        const std::string sem_name = "Local\\nxrt_ks_hog_" + std::to_string(GetCurrentProcessId());
        HANDLE ready = CreateSemaphoreA(nullptr, 0, 1000, sem_name.c_str());
        if (!ready) return false;
        char exe[MAX_PATH];
        GetModuleFileNameA(nullptr, exe, MAX_PATH);
        for (int i = 0; i < count; i++) {
            std::string cmd = std::string("\"") + exe + "\" --child kernel_sync_hog " + sem_name + " 120";
            STARTUPINFOA su{};
            su.cb = sizeof su;
            PROCESS_INFORMATION pi{};
            if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED | CREATE_NO_WINDOW | NORMAL_PRIORITY_CLASS,
                                nullptr, nullptr, &su, &pi))
                break;
            AssignProcessToJobObject(job_, pi.hProcess);
            SetProcessAffinityMask(pi.hProcess, mask);
            ResumeThread(pi.hThread);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            started_++;
        }
        // Every hog counts itself in once it runs, so the contention is real before the workload starts.
        int up = 0;
        while (up < started_ && WaitForSingleObject(ready, 10000) == WAIT_OBJECT_0) up++;
        CloseHandle(ready);
        return started_ == count && up == count;
    }

    void stop() {
        if (job_) CloseHandle(job_);       // KILL_ON_JOB_CLOSE ends every hog
        job_ = nullptr;
        started_ = 0;
    }

private:
    HANDLE job_ = nullptr;
    int started_ = 0;
};

}  // namespace

NXTEST(handoffs_survive_cpu_contention) {
    begin();
    Hogs hogs;
    CHECK_MSG(hogs.start(), "the load processes did not start");
    constexpr int kRounds = 20000;
    const PingPong pp = run_pingpong("handoffs_survive_cpu_contention", kRounds);
    CHECK_EQ(pp.a_rounds, kRounds);
    CHECK_EQ(pp.b_rounds, kRounds);
    CHECK_EQ(pp.turn, 0);
    CHECK_EQ(run_mutex_exclusion("handoffs_survive_cpu_contention", 8, 20000), 8ull * 20000ull);
    CHECK_EQ(g_guest_errors.load(), 0);
    hogs.stop();
    end();
}
