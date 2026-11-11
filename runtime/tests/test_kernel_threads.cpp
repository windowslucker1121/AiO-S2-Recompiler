// SPDX-License-Identifier: GPL-3.0-or-later
// Guest threads and the kernel pieces around them: thread objects and TLS, thread start and exit, the thread SVCs, the console core
// model and priority bands, the system tick, the handle table, OutputDebugString and svcBreak, unknown-SVC bookkeeping, event SVCs.
// Every case starts from kernel_reset + fn_clear + svc_init + mem::shutdown/init and ends with a kernel_reset (which joins its threads).
// Guest threads never run CHECK (the failure counter is thread-local): they store results in atomics and the test thread checks them
// after the join. Every wait has a 20 s deadline: a hung guest thread ends the run with exit code 3 instead of hanging CTest.
#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "nxrt/abi.h"
#include "nxrt/config.h"
#include "nxrt/diag.h"
#include "nxrt/dispatch.h"
#include "nxrt/kernel.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"
#include "nxrt/module.h"
#include "nxrt/runtime.h"
#include "nxrt/svc.h"
#include "test_framework.h"
#include "test_platform.h"
NXTEST_GROUP(kernel_threads)

namespace k = nxrt::kernel;
namespace mem = nxrt::mem;

namespace {

// ---- helpers --------------------------------------------------------------------------------------------------------------------

constexpr int64_t kMs = 1000000;

int64_t mono_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::atomic<uint64_t> g_seq{0};
uint64_t next_addr() { return 0x7100010000ull + 16 * ++g_seq; }      // fake guest code address, never dereferenced
uint64_t next_stack_top() { return 0x7C00100000ull + 0x10000 * ++g_seq; }  // fake guest stack top, never touched by host code

bool in_kernel_region(uint64_t a, uint64_t n) {
    return a >= mem::kKernelBase && n <= mem::kKernelSize && a - mem::kKernelBase <= mem::kKernelSize - n;
}

void begin() {
    k::kernel_reset();
    nxrt::fn_clear();
    nxrt::svc_init();
    nxrt::modules_unload_all();
    nxrt::mem::shutdown();
    nxrt::mem::init();
    nxrt::config_clear();
}

void end_case() { k::kernel_reset(); }

// Runs one SVC on a register set and returns its result code (w0).
uint32_t svc_rc(::Ctx* c, uint32_t n) {
    nxrt::svc_dispatch(c, n);
    return static_cast<uint32_t>(c->x[0]);
}

// A guest thread running `fn` with x0 = arg, prio in HOS units, core as ThreadParams::core (-2 = default).
uint32_t make_thread(nxrt::GuestFn fn, uint64_t arg, int prio, int core, const char* name) {
    k::ThreadParams p;
    p.entry = next_addr();
    nxrt::fn_register(p.entry, fn);
    p.arg = arg;
    p.stack_top = next_stack_top();
    p.prio = prio;
    p.core = core;
    p.name = name;
    return k::thread_create(p);
}

std::shared_ptr<k::KThread> thread_of(uint32_t h) { return k::handle_as<k::KThread>(h); }

// Host-side wait for a guest thread with the 20 s deadline. A timeout ends the run (exit code 3) instead of hanging.
bool join_guest(uint32_t h, const char* what) {
    const bool ok = k::thread_join(h, 20000);
    if (!ok) {
        std::printf("DEADLINE: guest thread %s did not finish in 20 s, aborting the run\n", what);
        std::fflush(stdout);
        std::_Exit(3);
    }
    return ok;
}

// A 20 s deadline for waits the test thread does itself without a timeout of its own (see the event and svcExitThread cases).
class Deadline {
public:
    explicit Deadline(const char* what)
        : what_(what), th_([this] {
              std::unique_lock<std::mutex> lk(m_);
              if (!cv_.wait_for(lk, std::chrono::seconds(20), [this] { return done_; })) {
                  std::printf("DEADLINE: %s did not finish in 20 s, aborting the run\n", what_);
                  std::fflush(stdout);
                  std::_Exit(3);
              }
          }) {}
    ~Deadline() {
        {
            std::lock_guard<std::mutex> lk(m_);
            done_ = true;
        }
        cv_.notify_all();
        th_.join();
    }

private:
    const char* what_;
    std::mutex m_;
    std::condition_variable cv_;
    bool done_ = false;
    std::thread th_;
};

// Log capture through nxrt::log_set_sink. The sink only stores lines; no CHECK runs inside it.
struct LogLine {
    nxrt::LogLevel level;
    std::string text;
};
std::mutex g_log_mu;
std::vector<LogLine> g_log;
nxrt::LogLevel g_saved_level = nxrt::LogLevel::Info;

void log_sink(nxrt::LogLevel l, const char* line, void*) {
    std::lock_guard<std::mutex> g(g_log_mu);
    g_log.push_back({l, line});
}

void capture_begin() {
    {
        std::lock_guard<std::mutex> g(g_log_mu);
        g_log.clear();
    }
    g_saved_level = nxrt::log_level();
    nxrt::log_set_level(nxrt::LogLevel::Info);
    nxrt::log_set_sink(log_sink, nullptr);
}

void capture_end() {
    nxrt::log_set_sink(nullptr, nullptr);
    nxrt::log_set_level(g_saved_level);
}

std::vector<LogLine> captured() {
    std::lock_guard<std::mutex> g(g_log_mu);
    return g_log;
}

size_t count_lines(const std::vector<LogLine>& v, const std::string& needle) {
    size_t n = 0;
    for (const auto& l : v) n += l.text.find(needle) != std::string::npos;
    return n;
}

// True when a line contains `needle`; with `level` >= 0 that line must also have that level.
bool has_line(const std::vector<LogLine>& v, const std::string& needle, int level = -1) {
    for (const auto& l : v)
        if (l.text.find(needle) != std::string::npos && (level < 0 || static_cast<int>(l.level) == level)) return true;
    return false;
}

// Console-model helpers.
int bit_of(uint64_t m) { return m ? __builtin_ctzll(m) : -1; }

// Expected band table, written from the scheduling model (six bands).
int band_active(int nn) {
    if (nn <= 4) return THREAD_PRIORITY_TIME_CRITICAL;
    if (nn <= 8) return THREAD_PRIORITY_HIGHEST;
    if (nn <= 15) return THREAD_PRIORITY_ABOVE_NORMAL;
    if (nn == 16) return THREAD_PRIORITY_NORMAL;
    if (nn == 17) return THREAD_PRIORITY_BELOW_NORMAL;
    return THREAD_PRIORITY_LOWEST;
}

// Expected table without the model, as sched.cpp has it.
int band_inactive(int nn) {
    if (nn <= 4) return THREAD_PRIORITY_HIGHEST;
    if (nn <= 15) return THREAD_PRIORITY_ABOVE_NORMAL;
    if (nn == 16) return THREAD_PRIORITY_NORMAL;
    if (nn <= 27) return THREAD_PRIORITY_BELOW_NORMAL;
    return THREAD_PRIORITY_LOWEST;
}

// Guest function bodies.

void fn_idle(::Ctx*) {}

std::atomic<uint64_t> g_arg_seen{0};
std::atomic<int> g_ran{0};
std::atomic<int> g_ctx_ok{0};
std::atomic<uint32_t> g_tid_seen{0};
k::KThread* g_expect_thread = nullptr;

void fn_identity(::Ctx* c) {
    g_arg_seen = c->x[0];
    g_tid_seen = GetCurrentThreadId();
    const bool ok = nxrt::diag_thread_ctx() == c && k::current_thread() == g_expect_thread && k::current_ctx() == c;
    g_ctx_ok = ok ? 1 : 0;
    g_ran = 1;
}

// Release gate for threads that must stay alive while the test inspects them.
std::atomic<bool> g_release{false};
void fn_wait_release(::Ctx*) {
    while (!g_release.load()) Sleep(1);
}

std::atomic<int> g_mid_reached{0};
std::atomic<int> g_after_exit{0};
void fn_exit_mid(::Ctx* c) {
    g_mid_reached = 1;
    nxrt::svc_dispatch(c, 0x0A);   // svcExitThread: nothing below may run
    g_after_exit = 1;
}

std::atomic<uint64_t> g_svc_arg{0};
void fn_svc_child(::Ctx* c) {
    g_svc_arg = c->x[0];
    g_ran = 1;
}

std::atomic<int> g_many{0};
void fn_many(::Ctx*) { g_many.fetch_add(1); }

// Console-model probe: samples the host processor every 1 ms while it spins.
struct Probe {
    int64_t spin_ns = 200 * kMs;
    uint64_t sched_core = 99, svc_core = 99;
    std::vector<std::pair<int64_t, uint32_t>> samples;   // (monotonic ns, host processor number)
};

void fn_probe(::Ctx* c) {
    Probe* p = reinterpret_cast<Probe*>(c->x[0]);
    p->sched_core = static_cast<uint64_t>(k::sched_current_core());
    nxrt::svc_dispatch(c, 0x10);                         // GetCurrentProcessorNumber
    p->svc_core = c->x[0];
    const int64_t end = mono_ns() + p->spin_ns;
    int64_t next = 0;
    for (;;) {
        const int64_t t = mono_ns();
        if (t >= end) break;
        if (t >= next) {
            PROCESSOR_NUMBER pn{};
            GetCurrentProcessorNumberEx(&pn);
            p->samples.push_back({t, pn.Number});
            next = t + kMs;
        }
    }
}

std::atomic<int64_t> g_dt5{-1}, g_dt0{-1}, g_dtm1{-1}, g_dtm2{-1};
void fn_sleep_svc(::Ctx* c) {
    auto timed = [&](int64_t ns) {
        c->x[0] = static_cast<uint64_t>(ns);
        const int64_t t0 = mono_ns();
        nxrt::svc_dispatch(c, 0x0B);       // SleepThread
        return mono_ns() - t0;
    };
    g_dt5 = timed(5 * kMs);
    g_dt0 = timed(0);
    g_dtm1 = timed(-1);
    g_dtm2 = timed(-2);
}

std::atomic<uint64_t> g_pseudo_id{0};
std::atomic<uint32_t> g_pseudo_rc{0xFFFF};
std::atomic<int> g_pseudo_same{0};
void fn_pseudo(::Ctx* c) {
    c->x[1] = k::kPseudoCurrentThread;
    nxrt::svc_dispatch(c, 0x25);           // GetThreadId(pseudo handle)
    g_pseudo_rc = static_cast<uint32_t>(c->x[0]);
    g_pseudo_id = c->x[1];
    auto self = k::handle_get(k::kPseudoCurrentThread);
    g_pseudo_same = (self && self.get() == k::current_thread()) ? 1 : 0;
}

void fn_return_now(::Ctx*) {}
void fn_svc_exit_now(::Ctx* c) { nxrt::svc_dispatch(c, 0x0A); }

// Signals an event from a guest thread after 30 ms; t_signal is the monotonic time of the signal.
struct SignalCtx {
    uint32_t hw = 0;
    std::atomic<int64_t> t_signal{0};
};
void fn_signaler(::Ctx* c) {
    auto* sc = reinterpret_cast<SignalCtx*>(c->x[0]);
    k::sleep_ns(30 * kMs);
    sc->t_signal = mono_ns();
    ::Ctx s{};
    s.x[0] = sc->hw;
    nxrt::svc_dispatch(&s, 0x11);          // SignalEvent
}

// Host functions behind HLE and import names of the fixture (tests/data/fixture_info.txt).
int g_host_data_value = 0x1234;
void host_hle_add(::Ctx* c) { c->x[0] = static_cast<uint32_t>(c->x[0] + c->x[1]); }

}  // namespace

// ---- thread objects and TLS -------------------------------------------------------------------------------------------------------

NXTEST(thread_create_object_and_tls) {
    begin();
    const uint64_t sp = next_stack_top();
    k::ThreadParams p;
    p.entry = next_addr();
    nxrt::fn_register(p.entry, fn_idle);
    p.arg = 0x1234;
    p.stack_top = sp;
    p.prio = 30;
    p.core = 1;
    p.name = "t-one";
    const uint32_t h = k::thread_create(p);
    CHECK(h != 0);
    auto t = thread_of(h);
    REQUIRE(t);
    CHECK_EQ(t->prio, 30);
    CHECK_EQ(t->ideal_core, 1);
    CHECK(t->name == "t-one");
    CHECK_EQ(t->core_mask, 2ull);
    CHECK_EQ(t->handle, h);

    const uint32_t h2 = make_thread(fn_idle, 0, 44, 2, "t-two");
    auto t2 = thread_of(h2);
    REQUIRE(t2);
    CHECK(t->id >= 0x50);
    CHECK(t2->id >= 0x50);
    CHECK(t->id != t2->id);

    CHECK(t->tls != 0 && t->tp_block != 0);
    CHECK((t->tls & 15) == 0 && (t->tp_block & 15) == 0);
    CHECK(in_kernel_region(t->tls, 0x200));
    CHECK(in_kernel_region(t->tp_block, 16));
    CHECK(*reinterpret_cast<uint64_t*>(t->tls + 0x1F0) == t->tp_block);
    CHECK(t->ctx.tpidr == t->tp_block);
    CHECK(t->ctx.tpidrro == t->tls);
    CHECK(t->ctx.x[31] == sp);
    CHECK(t->ctx.x[0] == 0x1234ull);
    end_case();
}

NXTEST(thread_start_runs_entry_on_host_thread) {
    begin();
    g_ran = 0;
    g_ctx_ok = 0;
    g_arg_seen = 0;
    const uint32_t h = make_thread(fn_identity, 0xABCD, 44, 0, "identity");
    REQUIRE(h);
    auto t = thread_of(h);
    REQUIRE(t);
    g_expect_thread = t.get();
    CHECK_EQ(k::thread_start(h), k::kOk);
    CHECK(join_guest(h, "identity"));
    CHECK_EQ(g_ran.load(), 1);
    CHECK_EQ(g_arg_seen.load(), 0xABCDull);
    CHECK_EQ(g_ctx_ok.load(), 1);
    CHECK(g_tid_seen.load() != GetCurrentThreadId());
    CHECK(g_tid_seen.load() == t->host_tid);
    CHECK(t->exited);
    CHECK(k::thread_join(h, 1000));
    CHECK_EQ(k::thread_start(h), k::kInvalidState);            // a second start is refused
    CHECK_EQ(k::thread_start(0xDEADu), k::kInvalidHandle);     // bad handle
    g_expect_thread = nullptr;
    end_case();
}

// ---- static TLS ---------------------------------------------------------------------------------------------------------------------

NXTEST(static_tls_image_in_thread_block) {
    begin();
    nxrt::module_provide_data("hle_data_import", reinterpret_cast<uint64_t>(&g_host_data_value));
    nxrt::hle_register("hle_add", host_hle_add);
    std::string err;
    nxrt::Module* m = nxrt::module_load_file(NXRT_TEST_DATA_DIR "/fixture_aarch64.elf", {}, &err);
    CHECK_MSG(m != nullptr, err);
    REQUIRE(m);
    CHECK(nxrt::module_link_all());

    const nxrt::TlsInfo& tpl = nxrt::module_tls_template();
    CHECK(tpl.present());
    CHECK_EQ(tpl.file_size, 0x20ull);
    CHECK_EQ(tpl.mem_size, 0x40ull);
    CHECK_EQ(tpl.tp_offset, 16ull);

    const uint32_t h = make_thread(fn_idle, 0, 44, 0, "tls");
    auto t = thread_of(h);
    REQUIRE(t);
    const uint8_t* block = reinterpret_cast<const uint8_t*>(t->tp_block);
    const uint8_t* image = block + tpl.tp_offset;
    CHECK(std::memcmp(image, reinterpret_cast<const void*>(tpl.init_addr), static_cast<size_t>(tpl.file_size)) == 0);
    CHECK_EQ(*reinterpret_cast<const int32_t*>(image), 7);                 // tls_a
    const int32_t tls_b_expect[4] = {1, 2, 3, 4};
    CHECK(std::memcmp(image + 0x10, tls_b_expect, sizeof tls_b_expect) == 0);  // tls_b
    // Everything else in the 4 KiB block (the TCB and the .tbss part) is zero. The block is 0x1000 bytes (thread.cpp: at least 0x1000).
    bool rest_zero = true;
    for (uint64_t i = 0; i < 0x1000; i++) {
        if (i >= tpl.tp_offset && i < tpl.tp_offset + tpl.file_size) continue;
        if (block[i]) rest_zero = false;
    }
    CHECK(rest_zero);
    CHECK(*reinterpret_cast<uint64_t*>(t->tls + 0x1F0) == t->tp_block);
    nxrt::modules_unload_all();
    end_case();
}

// ---- thread SVCs: priority, core mask, create/start/close ----------------------------------------------------------------------------

NXTEST(priority_and_core_mask_svcs) {
    begin();
    const uint32_t h = make_thread(fn_idle, 0, 44, 0, "pm");
    REQUIRE(h);

    ::Ctx g{};
    g.x[1] = h;                                              // GetThreadPriority: handle in w1, priority back in w1
    CHECK_EQ(svc_rc(&g, 0x0C), k::kOk);
    CHECK_EQ(g.x[1], 44ull);

    ::Ctx s{};
    s.x[0] = h;
    s.x[1] = 50;
    CHECK_EQ(svc_rc(&s, 0x0D), k::kOk);                      // SetThreadPriority
    ::Ctx g2{};
    g2.x[1] = h;
    svc_rc(&g2, 0x0C);
    CHECK_EQ(g2.x[1], 50ull);

    ::Ctx bp{};
    bp.x[0] = h;
    bp.x[1] = 64;
    CHECK_EQ(svc_rc(&bp, 0x0D), k::kInvalidPriority);
    ::Ctx g3{};
    g3.x[1] = h;
    svc_rc(&g3, 0x0C);
    CHECK_EQ(g3.x[1], 50ull);                                // unchanged after the refused call

    ::Ctx m{};
    m.x[2] = h;                                              // GetThreadCoreMask: handle in w2, ideal in w1, mask in x2
    CHECK_EQ(svc_rc(&m, 0x0E), k::kOk);
    CHECK_EQ(m.x[1], 0ull);
    CHECK_EQ(m.x[2], 1ull);

    ::Ctx sm{};
    sm.x[0] = h;
    sm.x[1] = 1;                                             // ideal core 1
    sm.x[2] = 6;                                             // cores 1 and 2
    CHECK_EQ(svc_rc(&sm, 0x0F), k::kOk);
    ::Ctx m2{};
    m2.x[2] = h;
    svc_rc(&m2, 0x0E);
    CHECK_EQ(m2.x[1], 1ull);
    CHECK_EQ(m2.x[2], 6ull);

    ::Ctx z{};
    z.x[0] = h;
    z.x[1] = 1;
    z.x[2] = 0;                                              // empty mask
    CHECK_EQ(svc_rc(&z, 0x0F), k::kInvalidCombination);

    ::Ctx c7{};
    c7.x[0] = h;
    c7.x[1] = 8;                                             // ideal core 8: the SVC takes core numbers 0..7 (platform 2 has six application cores)
    c7.x[2] = 1;
    CHECK_EQ(svc_rc(&c7, 0x0F), k::kInvalidCoreId);

    ::Ctx keep{};
    keep.x[0] = h;
    keep.x[1] = 0xFFFFFFFDull;                               // ideal -3: keep the ideal core (1, which is in the new mask)
    keep.x[2] = 3;                                           // cores 0 and 1
    CHECK_EQ(svc_rc(&keep, 0x0F), k::kOk);
    ::Ctx m3{};
    m3.x[2] = h;
    svc_rc(&m3, 0x0E);
    CHECK_EQ(m3.x[1], 1ull);
    CHECK_EQ(m3.x[2], 3ull);

    ::Ctx bad{};
    bad.x[0] = 0xDEADull;
    bad.x[1] = 0;
    bad.x[2] = 1;
    CHECK_EQ(svc_rc(&bad, 0x0F), k::kInvalidHandle);
    ::Ctx badp{};
    badp.x[0] = 0xDEADull;
    badp.x[1] = 5;
    CHECK_EQ(svc_rc(&badp, 0x0D), k::kInvalidHandle);
    ::Ctx badg{};
    badg.x[1] = 0xDEADull;
    CHECK_EQ(svc_rc(&badg, 0x0C), k::kInvalidHandle);
    ::Ctx badm{};
    badm.x[2] = 0xDEADull;
    CHECK_EQ(svc_rc(&badm, 0x0E), k::kInvalidHandle);
    end_case();
}

NXTEST(create_start_close_via_svcs) {
    begin();
    g_svc_arg = 0;
    g_ran = 0;
    const uint64_t entry = next_addr();
    nxrt::fn_register(entry, fn_svc_child);

    ::Ctx c{};
    c.x[1] = entry;                  // CreateThread: x1 entry, x2 arg, x3 stack top, w4 prio, w5 core -> x1 handle
    c.x[2] = 0x5151;
    c.x[3] = next_stack_top();
    c.x[4] = 40;
    c.x[5] = 1;
    CHECK_EQ(svc_rc(&c, 0x08), k::kOk);
    const uint32_t h = static_cast<uint32_t>(c.x[1]);
    REQUIRE(h != 0);
    auto t = thread_of(h);
    REQUIRE(t);
    CHECK_EQ(t->prio, 40);
    CHECK_EQ(t->ideal_core, 1);

    ::Ctx bp{};
    bp.x[1] = entry;
    bp.x[3] = next_stack_top();
    bp.x[4] = 99;                    // priority out of range
    bp.x[5] = 1;
    CHECK_EQ(svc_rc(&bp, 0x08), k::kInvalidPriority);
    ::Ctx bc{};
    bc.x[1] = entry;
    bc.x[3] = next_stack_top();
    bc.x[4] = 40;
    bc.x[5] = 9;                     // core out of range
    CHECK_EQ(svc_rc(&bc, 0x08), k::kInvalidCoreId);

    ::Ctx st{};
    st.x[0] = h;
    CHECK_EQ(svc_rc(&st, 0x09), k::kOk);                   // StartThread
    CHECK(join_guest(h, "svc-created"));
    CHECK_EQ(g_ran.load(), 1);
    CHECK_EQ(g_svc_arg.load(), 0x5151ull);

    ::Ctx cl{};
    cl.x[0] = h;
    CHECK_EQ(svc_rc(&cl, 0x16), k::kOk);                   // CloseHandle
    ::Ctx cl2{};
    cl2.x[0] = h;
    CHECK_EQ(svc_rc(&cl2, 0x16), k::kInvalidHandle);       // second close
    end_case();
}

// ---- thread exit ----------------------------------------------------------------------------------------------------------------------

NXTEST(svc_exit_thread_ends_guest_thread) {
    begin();
    g_mid_reached = 0;
    g_after_exit = 0;
    const uint32_t h = make_thread(fn_exit_mid, 0, 44, 0, "exit-mid");
    REQUIRE(h);
    auto t = thread_of(h);
    REQUIRE(t);
    CHECK_EQ(k::thread_start(h), k::kOk);

    uint32_t hs[1] = {h};
    ::Ctx w{};
    w.x[1] = reinterpret_cast<uint64_t>(hs);
    w.x[2] = 1;
    w.x[3] = static_cast<uint64_t>(-1);                    // infinite, guarded by the deadline below
    {
        Deadline dl("WaitSynchronization on a guest thread");
        CHECK_EQ(svc_rc(&w, 0x18), k::kOk);
    }
    CHECK_EQ(static_cast<uint32_t>(w.x[1]), 0u);
    CHECK(join_guest(h, "exit-mid"));
    CHECK_EQ(g_mid_reached.load(), 1);
    CHECK_EQ(g_after_exit.load(), 0);
    CHECK(t->exited);
    end_case();
}

NXTEST(many_threads_all_complete) {
    begin();
    g_many = 0;
    std::vector<uint32_t> hs;
    for (int i = 0; i < 40; i++) {
        const uint32_t h = make_thread(fn_many, static_cast<uint64_t>(i), 44, i % 3, "many");
        REQUIRE(h);
        hs.push_back(h);
    }
    for (uint32_t h : hs) CHECK_EQ(k::thread_start(h), k::kOk);
    for (uint32_t h : hs) CHECK(join_guest(h, "many"));
    CHECK_EQ(g_many.load(), 40);
    CHECK_EQ(k::thread_count_running(), 0ull);
    CHECK(k::threads().size() >= 40);
    end_case();
}

// ---- main thread adoption ----------------------------------------------------------------------------------------------------------------

NXTEST(main_thread_adopt_on_host_thread) {
    begin();
    std::atomic<bool> done{false};
    std::atomic<uint32_t> adopted{0};
    std::atomic<int> cur_ok{0}, stack_ok{0}, vm_ok{0}, diag_ok{0}, name_ok{0};
    std::thread th([&] {
        const uint32_t h = k::main_thread_adopt(0x100000);
        adopted = h;
        k::KThread* me = k::current_thread();
        if (me) {
            cur_ok = 1;
            const uint64_t top = me->stack_top;
            name_ok = me->name == "main" ? 1 : 0;
            stack_ok = (top > mem::kKernelBase && top <= mem::kKernelBase + mem::kKernelSize) ? 1 : 0;
            const mem::VmInfo vi = mem::vm_query(top - 0x100000);
            vm_ok = vi.state == mem::kMemStack ? 1 : 0;
            for (const auto& r : nxrt::diag_threads())
                if (r.host_tid == GetCurrentThreadId()) diag_ok = 1;
        }
        // The helper ends here: drop its diagnostics record, or the watchdog case later in --all finds a dead thread it cannot suspend.
        nxrt::diag_unregister_thread();
        done = true;
    });
    {
        Deadline dl("main_thread_adopt helper");
        while (!done.load()) Sleep(1);
    }
    th.join();
    CHECK(adopted.load() != 0);
    CHECK_EQ(cur_ok.load(), 1);
    CHECK_EQ(name_ok.load(), 1);
    CHECK_EQ(stack_ok.load(), 1);
    CHECK_EQ(vm_ok.load(), 1);
    CHECK_EQ(diag_ok.load(), 1);
    end_case();
}

// ---- console core model ----------------------------------------------------------------------------------------------------------------------

NXTEST(console_core_placement) {
    begin();
    nxrt::config_set("sched", "affinity", "strict");   // the pinned placement of [sched] affinity = strict
    k::sched_reset_for_tests();
    const k::SchedInfo si = k::sched_info();
    if (!si.active) {
        nxtest::skip("console core model not active on this machine");
        return;
    }
    DWORD_PTR proc = 0, sys = 0;
    GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys);
    for (int i = 0; i < 3; i++) {
        CHECK_EQ(__builtin_popcountll(si.host_masks[i]), 1);
        CHECK((si.host_masks[i] & proc) == si.host_masks[i]);
    }
    CHECK(si.host_masks[0] != si.host_masks[1] && si.host_masks[1] != si.host_masks[2] && si.host_masks[0] != si.host_masks[2]);

    static const char* names[3] = {"core0", "core1", "core2"};
    Probe probes[3];
    uint32_t hs[3] = {0, 0, 0};
    for (int core = 0; core < 3; core++) {
        hs[core] = make_thread(fn_probe, reinterpret_cast<uint64_t>(&probes[core]), 44, core, names[core]);
        REQUIRE(hs[core]);
        CHECK_EQ(k::thread_start(hs[core]), k::kOk);
        auto t = thread_of(hs[core]);
        REQUIRE(t);
        GROUP_AFFINITY ga{};
        CHECK(GetThreadGroupAffinity(static_cast<HANDLE>(t->host_handle), &ga) != 0);
        CHECK_EQ(ga.Group, 0);
        CHECK_EQ(static_cast<uint64_t>(ga.Mask), si.host_masks[core]);
    }
    for (int core = 0; core < 3; core++) CHECK(join_guest(hs[core], names[core]));
    for (int core = 0; core < 3; core++) {
        const int want = bit_of(si.host_masks[core]);
        CHECK(probes[core].samples.size() >= 50);
        bool all_on_own = true;
        for (const auto& s : probes[core].samples)
            if (static_cast<int>(s.second) != want) all_on_own = false;
        CHECK(all_on_own);
        CHECK_EQ(probes[core].sched_core, static_cast<uint64_t>(core));
        CHECK_EQ(probes[core].svc_core, static_cast<uint64_t>(core));
    }
    end_case();
}

NXTEST(core_mask_0x3_stays_on_cores_0_and_1) {
    begin();
    nxrt::config_set("sched", "affinity", "strict");   // the pinned placement of [sched] affinity = strict
    k::sched_reset_for_tests();
    const k::SchedInfo si = k::sched_info();
    if (!si.active) {
        nxtest::skip("console core model not active on this machine");
        return;
    }
    Probe p;
    const uint32_t h = make_thread(fn_probe, reinterpret_cast<uint64_t>(&p), 44, 0, "mask3");
    REQUIRE(h);
    ::Ctx s{};
    s.x[0] = h;
    s.x[1] = 0;                                    // ideal core 0
    s.x[2] = 3;                                    // cores 0 and 1
    CHECK_EQ(svc_rc(&s, 0x0F), k::kOk);
    CHECK_EQ(k::thread_start(h), k::kOk);
    auto t = thread_of(h);
    REQUIRE(t);
    GROUP_AFFINITY ga{};
    GetThreadGroupAffinity(static_cast<HANDLE>(t->host_handle), &ga);
    CHECK_EQ(static_cast<uint64_t>(ga.Mask), si.host_masks[0] | si.host_masks[1]);
    CHECK(join_guest(h, "mask3"));
    const int ok0 = bit_of(si.host_masks[0]), ok1 = bit_of(si.host_masks[1]);
    bool only_01 = true;
    for (const auto& smp : p.samples)
        if (static_cast<int>(smp.second) != ok0 && static_cast<int>(smp.second) != ok1) only_01 = false;
    CHECK(only_01);
    CHECK(p.samples.size() >= 50);
    CHECK(p.sched_core == 0 || p.sched_core == 1);
    end_case();
}

NXTEST(set_core_mask_moves_running_thread) {
    begin();
    nxrt::config_set("sched", "affinity", "strict");   // the pinned placement of [sched] affinity = strict
    k::sched_reset_for_tests();
    const k::SchedInfo si = k::sched_info();
    if (!si.active) {
        nxtest::skip("console core model not active on this machine");
        return;
    }
    Probe p;
    p.spin_ns = 600 * kMs;
    const uint32_t h = make_thread(fn_probe, reinterpret_cast<uint64_t>(&p), 44, 0, "mover");
    REQUIRE(h);
    CHECK_EQ(k::thread_start(h), k::kOk);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const int64_t change = mono_ns();
    ::Ctx s{};
    s.x[0] = h;
    s.x[1] = 2;                                    // ideal core 2
    s.x[2] = 4;                                    // core 2 only
    CHECK_EQ(svc_rc(&s, 0x0F), k::kOk);
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    auto t = thread_of(h);
    REQUIRE(t);
    GROUP_AFFINITY ga{};
    GetThreadGroupAffinity(static_cast<HANDLE>(t->host_handle), &ga);
    CHECK_EQ(static_cast<uint64_t>(ga.Mask), si.host_masks[2]);
    CHECK(join_guest(h, "mover"));

    const int own0 = bit_of(si.host_masks[0]), own2 = bit_of(si.host_masks[2]);
    bool before_ok = true, after_ok = true;
    size_t late = 0;
    for (const auto& smp : p.samples) {
        if (smp.first < change && static_cast<int>(smp.second) != own0) before_ok = false;
        if (smp.first >= change + 200 * kMs) {
            late++;
            if (static_cast<int>(smp.second) != own2) after_ok = false;
        }
    }
    CHECK(before_ok);
    CHECK(after_ok);
    CHECK(late >= 10);
}

// ---- priority bands ----------------------------------------------------------------------------------------------------------------------------

NXTEST(priority_bands_active_and_inactive) {
    begin();
    const k::SchedInfo si = k::sched_info();
    if (si.active) {
        for (int nn = 0; nn <= 31; nn++) CHECK_EQ(k::sched_host_priority(28 + nn), band_active(nn));
    } else {
        std::printf("    note: console core model not active here, the active table is not checked\n");
    }
    nxrt::config_set("sched", "console_cores", "0");
    k::sched_reset_for_tests();
    CHECK(!k::sched_info().active);
    for (int nn = 0; nn <= 31; nn++) CHECK_EQ(k::sched_host_priority(28 + nn), band_inactive(nn));
    nxrt::config_clear();
    k::sched_reset_for_tests();
    end_case();
}

NXTEST(started_thread_win32_priority_follows_bands) {
    begin();
    g_release = false;
    const int prios[6] = {28 + 0, 28 + 6, 28 + 12, 28 + 16, 28 + 17, 28 + 25};
    uint32_t hs[6] = {0, 0, 0, 0, 0, 0};
    for (int i = 0; i < 6; i++) {
        hs[i] = make_thread(fn_wait_release, 0, prios[i], 0, "prio");
        CHECK(hs[i] != 0);
    }
    for (int i = 0; i < 6; i++) {
        if (!hs[i]) continue;
        CHECK_EQ(k::thread_start(hs[i]), k::kOk);
    }
    for (int i = 0; i < 6; i++) {
        auto t = thread_of(hs[i]);
        if (!t || !t->host_handle) continue;
        const int got = GetThreadPriority(static_cast<HANDLE>(t->host_handle));
        CHECK_EQ(got, k::sched_host_priority(prios[i]));
        if (k::sched_info().active) CHECK_EQ(got, band_active(prios[i] - 28));
    }
    // A running thread changes band with thread_set_priority.
    auto t0 = thread_of(hs[0]);
    if (t0) {
        CHECK(k::thread_set_priority(*t0, 28 + 17));
        CHECK_EQ(GetThreadPriority(static_cast<HANDLE>(t0->host_handle)), k::sched_host_priority(28 + 17));
    }
    g_release = true;
    for (int i = 0; i < 6; i++)
        if (hs[i]) CHECK(join_guest(hs[i], "prio"));
    end_case();
}

// ---- config switch -------------------------------------------------------------------------------------------------------------------------------

NXTEST(config_switch_console_cores) {
    begin();
    DWORD_PTR proc = 0, sys = 0;
    GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys);
    k::sched_reset_for_tests();
    const bool capable = k::sched_info().active;

    nxrt::config_set("sched", "console_cores", "0");
    k::sched_reset_for_tests();
    CHECK(!k::sched_info().active);
    g_release = false;
    const uint32_t h = make_thread(fn_wait_release, 0, 44, 0, "unpinned");
    REQUIRE(h);
    CHECK_EQ(k::thread_start(h), k::kOk);
    auto t = thread_of(h);
    REQUIRE(t);
    GROUP_AFFINITY ga{};
    GetThreadGroupAffinity(static_cast<HANDLE>(t->host_handle), &ga);
    CHECK_EQ(static_cast<uint64_t>(ga.Mask), static_cast<uint64_t>(proc));
    g_release = true;
    CHECK(join_guest(h, "unpinned"));

    nxrt::config_set("sched", "console_cores", "1");
    k::sched_reset_for_tests();
    if (capable) CHECK(k::sched_info().active);
    nxrt::config_clear();
    k::sched_reset_for_tests();
    end_case();
}

// platform 2 has six application cores (cores 0..5, mask 0x3f); platform 1 has three, and the SVC maps what is outside them onto the last one.
NXTEST(core_mask_svcs_with_six_cores_on_switch2) {
    {
        nxtest::ScopedPlatform sw2(nxrt::game::Platform::Switch2);
        begin();
        const uint32_t h = make_thread(fn_idle, 0, 44, 0, "pm6");
        REQUIRE(h);
        ::Ctx s{};
        s.x[0] = h;
        s.x[1] = 5;                                          // ideal core 5, a core only platform 2 has
        s.x[2] = 0x20;
        CHECK_EQ(svc_rc(&s, 0x0F), k::kOk);
        ::Ctx m{};
        m.x[2] = h;
        CHECK_EQ(svc_rc(&m, 0x0E), k::kOk);
        CHECK_EQ(m.x[1], 5ull);
        CHECK_EQ(m.x[2], 0x20ull);
        ::Ctx all{};
        all.x[0] = h;
        all.x[1] = 0xFFFFFFFFull;                            // IdealCoreDontCare, all six cores
        all.x[2] = 0x3F;
        CHECK_EQ(svc_rc(&all, 0x0F), k::kOk);
        ::Ctx m2{};
        m2.x[2] = h;
        svc_rc(&m2, 0x0E);
        CHECK_EQ(m2.x[1], 0ull);
        CHECK_EQ(m2.x[2], 0x3Full);
        ::Ctx beyond{};
        beyond.x[0] = h;
        beyond.x[1] = 6;
        beyond.x[2] = 0x40;                                  // core 6 is not an application core
        CHECK_EQ(svc_rc(&beyond, 0x0F), k::kInvalidCombination);
        end_case();
    }
    nxtest::ScopedPlatform sw1(nxrt::game::Platform::Switch1);   // the test game may be a platform 2 title (oot): say which console is meant
    begin();                                                 // the same request on platform 1 (three cores): core 5 is outside the mask it can honour
    const uint32_t h = make_thread(fn_idle, 0, 44, 0, "pm3");
    REQUIRE(h);
    ::Ctx s{};
    s.x[0] = h;
    s.x[1] = 5;
    s.x[2] = 0x20;
    CHECK_EQ(svc_rc(&s, 0x0F), k::kInvalidCombination);
    end_case();
}

// ---- system tick and sleeping ------------------------------------------------------------------------------------------------------------------

// The tick rate is cached at first use (kernel::tick_hz), so the platform 2 rate is checked in a child process that picks the platform first.
NXTEST_CHILD(kernel_threads_switch2_tick) {
    nxtest::use_platform_for_process(nxrt::game::Platform::Switch2);
    if (k::tick_hz() != k::kTickHzSwitch2 || k::kTickHzSwitch2 != 31250000ull) return 10;
    const auto w0 = std::chrono::steady_clock::now();
    const uint64_t a = k::system_tick();
    Sleep(200);
    const uint64_t b = k::system_tick();
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - w0).count();
    const double hz = static_cast<double>(b - a) / secs;
    if (hz < 31250000.0 * 0.9 || hz > 31250000.0 * 1.1) return 11;     // ticks per wall second at the platform 2 rate (a platform 1 rate is 39% lower)
    nxrt::svc_init();
    ::Ctx c{};
    nxrt::svc_dispatch(&c, 0x1E);                                      // GetSystemTick answers in the same ticks
    if (c.x[0] < b || c.x[0] > k::system_tick()) return 12;
    return 0;
}

NXTEST(system_tick_runs_at_31_25_mhz_on_switch2) {
    const int rc = nxtest::run_child("kernel_threads_switch2_tick");
    CHECK_MSG(rc == 0, "child kernel_threads_switch2_tick returned " + std::to_string(rc) + " (10 tick_hz, 11 ticks per second, 12 GetSystemTick)");
}

NXTEST(system_tick_monotonic_and_rate) {
    begin();
    CHECK_EQ(k::kTickHzSwitch1, 19200000ull);
    uint64_t prev = k::system_tick();
    bool monotonic = true;
    for (int i = 0; i < 100000; i++) {
        const uint64_t v = k::system_tick();
        if (v < prev) monotonic = false;
        prev = v;
    }
    CHECK(monotonic);

    const uint64_t a = k::system_tick();
    Sleep(100);
    const uint64_t b = k::system_tick();
    const int64_t d = static_cast<int64_t>(b - a);
    CHECK(d >= 1920000 - 300000);                  // Sleep(100) never returns early: at least 100 ms - 15 ms of 19.2 MHz ticks
    CHECK(d <= 1920000 * 50);                      // and returns (a busy machine may delay the wake-up by any amount: the upper bound only catches a wrong tick rate, 50x)

    const uint64_t before = k::system_tick();
    ::Ctx c{};
    nxrt::svc_dispatch(&c, 0x1E);                  // GetSystemTick
    const uint64_t after = k::system_tick();
    CHECK(c.x[0] >= before);
    CHECK(c.x[0] <= after);
    end_case();
}

NXTEST(sleep_ns_and_sleep_svc) {
    begin();
    const int64_t t0 = mono_ns();
    k::sleep_ns(2 * kMs);
    const int64_t dt = mono_ns() - t0;
    CHECK(dt >= 1500000);
    CHECK(dt <= 2000 * kMs);                       // a real sleep returns; a loaded machine may delay the wake-up by any amount, so only a hang is caught

    // Zero and negative spans yield (SwitchToThread). On a loaded machine a yield can legitimately take a whole scheduler
    // quantum, so no latency bound is put on them: the test only asserts that every yield returns.
    int done = 0;
    for (int i = 0; i < 200; i++) {
        k::sleep_ns(0);
        ++done;
    }
    CHECK_EQ(done, 200);
    done = 0;
    for (int i = 0; i < 200; i++) {
        k::sleep_ns(-1);
        ++done;
    }
    CHECK_EQ(done, 200);

    // SleepThread from a guest thread: 5 ms sleeps are timed, the yield variants only have to return (a value >= 0 means the
    // call completed; the -1 initial value would mean it never returned).
    g_dt5 = -1;
    g_dt0 = -1;
    g_dtm1 = -1;
    g_dtm2 = -1;
    const uint32_t h = make_thread(fn_sleep_svc, 0, 44, 0, "sleeper");
    REQUIRE(h);
    CHECK_EQ(k::thread_start(h), k::kOk);
    CHECK(join_guest(h, "sleeper"));
    CHECK(g_dt5.load() >= 4 * kMs);
    CHECK(g_dt5.load() < 200 * kMs);
    CHECK(g_dt0.load() >= 0);
    CHECK(g_dtm1.load() >= 0);
    CHECK(g_dtm2.load() >= 0);
    end_case();
}

// ---- handle table ------------------------------------------------------------------------------------------------------------------------------

NXTEST(handle_table_add_get_close) {
    begin();
    auto a = std::make_shared<k::KEvent>();
    auto b = std::make_shared<k::KEvent>();
    auto c = std::make_shared<k::KEvent>();
    const uint32_t ha = k::handle_add(a), hb = k::handle_add(b), hc = k::handle_add(c);
    CHECK(ha >= 0x10000 && hb >= 0x10000 && hc >= 0x10000);
    CHECK(ha != hb && hb != hc && ha != hc);
    CHECK(k::handle_get(ha).get() == a.get());
    CHECK(k::handle_get(hb).get() == b.get());
    CHECK_EQ(k::handle_count(), 3ull);
    k::handle_close(ha);
    CHECK(!k::handle_get(ha));
    CHECK_EQ(k::handle_count(), 2ull);
    k::handle_close(0x12345678u);                  // unknown handle: harmless
    k::handle_close(ha);                           // already closed: harmless
    CHECK_EQ(k::handle_count(), 2ull);
    CHECK(k::handle_as<k::KEvent>(hb) != nullptr);
    CHECK(k::handle_as<k::KThread>(hb) == nullptr);   // wrong type
    CHECK(k::handle_get(hb)->type() == k::ObjType::Event);
    end_case();
}

NXTEST(handle_pseudo_current_thread) {
    begin();
    g_pseudo_rc = 0xFFFF;
    g_pseudo_id = 0;
    g_pseudo_same = 0;
    const uint32_t h = make_thread(fn_pseudo, 0, 44, 0, "pseudo");
    REQUIRE(h);
    auto t = thread_of(h);
    REQUIRE(t);
    CHECK_EQ(k::thread_start(h), k::kOk);
    CHECK(join_guest(h, "pseudo"));
    CHECK_EQ(g_pseudo_rc.load(), k::kOk);
    CHECK_EQ(g_pseudo_id.load(), t->id);
    CHECK_EQ(g_pseudo_same.load(), 1);
    // On a host thread that is not a guest thread the pseudo handle resolves to nothing.
    CHECK(k::handle_get(k::kPseudoCurrentThread) == nullptr);
    ::Ctx c{};
    c.x[1] = k::kPseudoCurrentThread;
    CHECK_EQ(svc_rc(&c, 0x25), k::kInvalidHandle);
    end_case();
}

NXTEST(handle_table_limit_0x3000) {
    begin();
    int overflow = 0;
    for (int i = 0; i < 0x3000 + 5; i++)
        if (k::handle_add(std::make_shared<k::KEvent>()) == 0) overflow++;
    CHECK_EQ(overflow, 5);
    CHECK_EQ(k::handle_count(), 0x3000ull);
    k::handles_reset();
    CHECK_EQ(k::handle_count(), 0ull);
    end_case();
}

// ---- OutputDebugString and svcBreak --------------------------------------------------------------------------------------------------------------

NXTEST(output_debug_string_svc_logs_guest_text) {
    begin();
    capture_begin();
    const char msg[] = "hello from guest";
    ::Ctx c{};
    c.x[0] = reinterpret_cast<uint64_t>(msg);
    c.x[1] = sizeof msg - 1;
    CHECK_EQ(svc_rc(&c, 0x27), k::kOk);
    CHECK(has_line(captured(), "guest: hello from guest"));

    static char big[2000];
    std::memset(big, 'B', sizeof big);
    ::Ctx d{};
    d.x[0] = reinterpret_cast<uint64_t>(big);
    d.x[1] = sizeof big;
    CHECK_EQ(svc_rc(&d, 0x27), k::kOk);
    const auto lines = captured();
    CHECK(has_line(lines, std::string(1024, 'B')));          // cut to 1024 bytes
    CHECK(!has_line(lines, std::string(1025, 'B')));

    ::Ctx e{};
    e.x[0] = 0x10;                                           // not mapped
    e.x[1] = 4;
    CHECK_EQ(svc_rc(&e, 0x27), k::kInvalidPointer);
    capture_end();
    end_case();
}

NXTEST(svc_break_notification_and_result_payload) {
    begin();
    capture_begin();
    // Reason bit 31 set: notification only, the process continues (a fatal break would end the test process).
    ::Ctx c{};
    c.x[0] = 0x80000000ull;
    c.x[1] = 0;
    c.x[2] = 0;
    c.x[30] = 0x1234;
    nxrt::svc_dispatch(&c, 0x26);
    CHECK(has_line(captured(), "svcBreak", static_cast<int>(nxrt::LogLevel::Error)));

    // Reason 4 with a Result payload: 0x2A02 is module 0x2A02 & 0x1FF = 2, description (0x2A02 >> 9) & 0x1FFF = 21.
    // Bit 31 is set so that the break stays notify-only: without it reason 4 is fatal (RaiseException + TerminateProcess).
    uint32_t result = 0x2A02;
    ::Ctx r{};
    r.x[0] = 0x80000004ull;
    r.x[1] = reinterpret_cast<uint64_t>(&result);
    r.x[2] = 4;
    nxrt::svc_dispatch(&r, 0x26);
    CHECK(has_line(captured(), "module 2, description 21"));
    capture_end();
    end_case();
}

// ---- unknown SVCs --------------------------------------------------------------------------------------------------------------------------------------

NXTEST(unknown_svc_returns_not_implemented_and_logs_args) {
    begin();
    nxrt::svc_reset_counters();
    capture_begin();
    ::Ctx c{};
    for (int i = 0; i < 8; i++) c.x[i] = 0x100 + i;
    c.x[30] = 0xABCDEF;
    nxrt::svc_dispatch(&c, 0x38);
    CHECK_EQ(static_cast<uint32_t>(c.x[0]), k::kNotImplemented);
    CHECK_EQ(nxrt::svc_unknown_total(), 1ull);
    bool found = false;
    for (const auto& rec : nxrt::svc_unknown_records()) {
        if (rec.number != 0x38) continue;
        found = true;
        CHECK_EQ(rec.count, 1ull);
        for (int i = 0; i < 8; i++) CHECK_EQ(rec.first_args[i], 0x100ull + i);
        CHECK_EQ(rec.first_lr, 0xABCDEFull);
    }
    CHECK(found);
    const auto lines = captured();
    CHECK(has_line(lines, "UNKNOWN SVC 0x38"));
    CHECK(has_line(lines, "x0=100"));
    CHECK(has_line(lines, "x7=107"));
    CHECK(has_line(lines, "lr=abcdef"));
    capture_end();
    end_case();
}

NXTEST(unknown_svc_log_limit_summary_and_names) {
    begin();
    nxrt::svc_reset_counters();
    capture_begin();
    ::Ctx c{};
    for (int i = 0; i < 120; i++) nxrt::svc_dispatch(&c, 0x38);
    CHECK_EQ(count_lines(captured(), "UNKNOWN SVC 0x38"), 100u);     // the first 100 logged one by one
    for (int i = 120; i < 1000; i++) nxrt::svc_dispatch(&c, 0x38);
    CHECK_EQ(count_lines(captured(), "UNKNOWN SVC 0x38"), 101u);     // then only every 1000th (the 1000th call)
    CHECK_EQ(nxrt::svc_unknown_total(), 1000ull);
    CHECK(nxrt::svc_summary().find("0x38") != std::string::npos);
    CHECK(std::string(nxrt::svc_name(0x01)) == "SetHeapSize");
    CHECK(std::string(nxrt::svc_name(0xFF)) == "?");
    nxrt::svc_reset_counters();
    CHECK_EQ(nxrt::svc_unknown_total(), 0ull);
    CHECK(nxrt::svc_unknown_records().empty());
    CHECK_EQ(nxrt::svc_call_count(0x38), 0ull);
    capture_end();
    end_case();
}

// ---- events ----------------------------------------------------------------------------------------------------------------------------------------------

NXTEST(event_svcs_signal_reset_clear) {
    begin();
    ::Ctx c{};
    CHECK_EQ(svc_rc(&c, 0x45), k::kOk);                       // CreateEvent: w1 write handle, w2 read handle
    const uint32_t hw = static_cast<uint32_t>(c.x[1]);
    const uint32_t hr = static_cast<uint32_t>(c.x[2]);
    CHECK(hw != 0 && hr != 0 && hw != hr);

    auto wait1 = [](uint32_t h, uint64_t timeout) -> uint32_t {
        uint32_t arr[1] = {h};
        ::Ctx w{};
        w.x[1] = reinterpret_cast<uint64_t>(arr);
        w.x[2] = 1;
        w.x[3] = timeout;
        return svc_rc(&w, 0x18);
    };
    CHECK_EQ(wait1(hr, 0), k::kTimedOut);                     // unsignaled, no wait

    ::Ctx s{};
    s.x[0] = hw;
    CHECK_EQ(svc_rc(&s, 0x11), k::kOk);                       // SignalEvent
    uint32_t arr[1] = {hr};
    ::Ctx w{};
    w.x[1] = reinterpret_cast<uint64_t>(arr);
    w.x[2] = 1;
    w.x[3] = 0;
    CHECK_EQ(svc_rc(&w, 0x18), k::kOk);
    CHECK_EQ(static_cast<uint32_t>(w.x[1]), 0u);

    ::Ctx r{};
    r.x[0] = hr;
    CHECK_EQ(svc_rc(&r, 0x17), k::kOk);                       // ResetSignal
    ::Ctx r2{};
    r2.x[0] = hr;
    CHECK_EQ(svc_rc(&r2, 0x17), k::kInvalidState);            // not signaled any more

    ::Ctx s2{};
    s2.x[0] = hw;
    CHECK_EQ(svc_rc(&s2, 0x11), k::kOk);
    ::Ctx cl{};
    cl.x[0] = hw;
    CHECK_EQ(svc_rc(&cl, 0x12), k::kOk);                      // ClearEvent
    CHECK_EQ(wait1(hr, 0), k::kTimedOut);
    end_case();
}

NXTEST(event_wait_timeouts_and_cross_thread_wake) {
    begin();
    ::Ctx c{};
    CHECK_EQ(svc_rc(&c, 0x45), k::kOk);
    const uint32_t hw = static_cast<uint32_t>(c.x[1]);
    const uint32_t hr = static_cast<uint32_t>(c.x[2]);
    REQUIRE(hw && hr);

    uint32_t arr[1] = {hr};
    ::Ctx t20{};
    t20.x[1] = reinterpret_cast<uint64_t>(arr);
    t20.x[2] = 1;
    t20.x[3] = 20 * kMs;
    const int64_t t0 = mono_ns();
    CHECK_EQ(svc_rc(&t20, 0x18), k::kTimedOut);
    const int64_t waited = mono_ns() - t0;
    CHECK(waited >= 18 * kMs);                                 // 20 ms; 2 ms of margin for the clock and the wait granularity
    CHECK(waited < 2000 * kMs);

    // An infinite wait on the test thread; a guest thread signals it 30 ms later.
    SignalCtx sc;
    sc.hw = hw;
    const uint32_t hs = make_thread(fn_signaler, reinterpret_cast<uint64_t>(&sc), 44, 0, "signaler");
    REQUIRE(hs);
    CHECK_EQ(k::thread_start(hs), k::kOk);
    int64_t t_wake = 0;
    uint32_t rc_wait = 0;
    {
        Deadline dl("event wait with timeout -1");
        ::Ctx inf{};
        inf.x[1] = reinterpret_cast<uint64_t>(arr);
        inf.x[2] = 1;
        inf.x[3] = static_cast<uint64_t>(-1);
        rc_wait = svc_rc(&inf, 0x18);
        t_wake = mono_ns();
    }
    CHECK_EQ(rc_wait, k::kOk);
    CHECK(join_guest(hs, "signaler"));
    const int64_t t_sig = sc.t_signal.load();
    CHECK(t_sig != 0);
    CHECK(t_wake >= t_sig);
    CHECK(t_wake - t_sig < 100 * kMs);

    ::Ctx big{};
    big.x[1] = reinterpret_cast<uint64_t>(arr);
    big.x[2] = 0x41;                                           // more than 0x40 handles
    big.x[3] = 0;
    CHECK_EQ(svc_rc(&big, 0x18), k::kOutOfRange);
    end_case();
}

NXTEST(event_wait_invalid_handle_and_count) {
    begin();
    ::Ctx c{};
    CHECK_EQ(svc_rc(&c, 0x45), k::kOk);
    const uint32_t hr = static_cast<uint32_t>(c.x[2]);
    REQUIRE(hr);
    uint32_t arr[2] = {hr, 0xDEADu};
    ::Ctx w{};
    w.x[1] = reinterpret_cast<uint64_t>(arr);
    w.x[2] = 2;
    w.x[3] = 0;
    CHECK_EQ(svc_rc(&w, 0x18), k::kInvalidHandle);
    CHECK_EQ(static_cast<uint32_t>(w.x[1]), 0xFFFFFFFFu);      // index -1
    end_case();
}

// ---- end-of-run summary and thread object lifetime --------------------------------------------------------------------------------------------------

NXTEST(end_of_run_summary_has_svc_and_hle_lines) {
    begin();
    const std::string s = nxrt::summary();
    CHECK(s.find("svc:") != std::string::npos);
    CHECK(s.find("hle:") != std::string::npos);
    end_case();
}

NXTEST(thread_object_released_after_return) {
    begin();
    const uint32_t h = make_thread(fn_return_now, 0, 44, 0, "returns");
    REQUIRE(h);
    std::weak_ptr<k::KThread> weak;
    {
        auto t = thread_of(h);
        weak = t;
    }
    CHECK_EQ(k::thread_start(h), k::kOk);
    CHECK(join_guest(h, "returns"));
    k::kernel_reset();                                        // the kernel forgets the thread and its handle
    CHECK(weak.expired());
}

NXTEST(thread_object_released_after_svc_exit) {
    begin();
    const uint32_t h = make_thread(fn_svc_exit_now, 0, 44, 0, "svc-exits");
    REQUIRE(h);
    std::weak_ptr<k::KThread> weak;
    {
        auto t = thread_of(h);
        weak = t;
    }
    CHECK_EQ(k::thread_start(h), k::kOk);
    CHECK(join_guest(h, "svc-exits"));
    k::kernel_reset();
    CHECK(weak.expired());                                    // expected to fail: svcExitThread leaves the thread's shared_ptr behind
}
