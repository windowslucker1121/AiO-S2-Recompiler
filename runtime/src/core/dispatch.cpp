// SPDX-License-Identifier: GPL-3.0-or-later
// Function table + HLE thunks. Pattern from smo-recomp runtime/core/hostrt.c (open-addressing hash, hostfn thunks) and
// SuperMarioWonderRecomp runtime/src/loader.cpp (thunks, logging stub for unknown imports).
#include "nxrt/dispatch.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "nxrt/eh.h"
#include "nxrt/ftrace.h"
#include "nxrt/config.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"
#include "nxrt/gaps.h"

namespace nxrt {
namespace {

// ---- function hash: open addressing, power of two, load < 1/2. Readers never lock; a writer publishes the fn first and the
// address last (release), so a reader that sees the address sees the function. Growth builds a new table and swaps the
// pointer; retired tables stay allocated until fn_clear because a reader may still be walking one.
struct Slot {
    std::atomic<uint64_t> addr{0};
    std::atomic<GuestFn> fn{nullptr};
};
struct Table {
    explicit Table(size_t n) : mask(n - 1), slots(new Slot[n]) {}
    size_t mask;
    std::unique_ptr<Slot[]> slots;
    size_t used = 0;
};

std::mutex g_write;
std::atomic<Table*> g_table{nullptr};

// Per-thread direct-mapped cache in front of the function hash (S34). Indirect calls (virtual calls, function pointers) are the most
// frequent thing the guest does, and the hash table of 138,000 functions is 4 MiB: a lookup is a cache miss. The same few hundred
// targets repeat, so a 1,024-entry table per thread (16 KiB) answers most of them. A cache holds only functions that were found; a
// registration that replaces a function, and fn_clear, bump the epoch and every thread drops its cache before the next lookup.
std::atomic<uint32_t> g_fn_epoch{0};
struct FnCacheEntry {
    uint64_t addr;
    GuestFn fn;
};
constexpr size_t kFnCache = 1024;
thread_local FnCacheEntry t_fn_cache[kFnCache];
thread_local uint32_t t_fn_epoch = 0;
std::vector<std::unique_ptr<Table>> g_all_tables;   // owns current + retired tables
std::atomic<size_t> g_fn_count{0};

inline size_t hash_addr(uint64_t a) { return static_cast<size_t>(((a >> 2) * 0x9E3779B97F4A7C15ull) >> 24); }

Table* table_for_write() {
    Table* t = g_table.load(std::memory_order_relaxed);
    if (t && (t->used + 1) * 2 <= t->mask + 1) return t;
    size_t n = t ? (t->mask + 1) * 2 : 4096;
    auto nt = std::make_unique<Table>(n);
    if (t) {
        for (size_t i = 0; i <= t->mask; i++) {
            uint64_t a = t->slots[i].addr.load(std::memory_order_relaxed);
            if (!a) continue;
            size_t h = hash_addr(a) & nt->mask;
            while (nt->slots[h].addr.load(std::memory_order_relaxed)) h = (h + 1) & nt->mask;
            nt->slots[h].fn.store(t->slots[i].fn.load(std::memory_order_relaxed), std::memory_order_relaxed);
            nt->slots[h].addr.store(a, std::memory_order_relaxed);
            nt->used++;
        }
    }
    Table* raw = nt.get();
    g_all_tables.push_back(std::move(nt));
    g_table.store(raw, std::memory_order_release);
    return raw;
}

// host-address order for guest_function_of: rebuilt lazily when the table changed
struct ByHost {
    std::vector<std::pair<uint64_t, uint64_t>> v;   // (host fn address, guest address) sorted by host address
    size_t built_for = ~size_t(0);
};
std::mutex g_byhost_lock;
ByHost g_byhost;

// ---- HLE thunks
struct Thunk {
    std::string name;
    HleFn fn = nullptr;       // null: logging stub
    std::atomic<uint64_t> calls{0};
    std::atomic<int> ft_zone{-1};   // frame trace zone ("hle:<name>"), registered on the first slow call
    std::atomic<bool> leaf{false};  // hle_mark_leaf: an import call runs fn directly (no count, trace or import bookkeeping)
    std::atomic<bool> slow{false};  // frame trace: a timed call took 20 us or more (every later call is timed)
};
// Frame trace of HLE calls: timing every call (two clock reads, ~35k NVN calls per frame) made the traced game side ~15% slower than an
// untraced run. A thunk is timed on every call only once one of its timed calls took 20 us; before that one call in 32 per thread is
// timed. [prof] trace_hle_all = 1: every call timed (the old behaviour).
thread_local uint32_t t_hle_sample = 0;
std::mutex g_hle_lock;
std::vector<std::unique_ptr<Thunk>> g_thunks;        // index = (addr - kHleBase) / 16
std::atomic<size_t> g_thunk_count{0};                // published size (thunks are never removed outside hle_clear)
std::unordered_map<std::string, size_t> g_thunk_by_name;
thread_local const char* t_hle_name = nullptr;

// [hle] trace = <name prefix> (env NXRT_HLE_TRACE): every call of an HLE thunk whose name starts with the prefix is logged in order
// ("hle-trace #seq name(x0..x3) lr"), up to [hle] trace_limit calls (default 20000). Diagnostics for the boot: the order in which a
// game calls an API (NVN).
struct HleTrace {
    std::string prefix;
    int64_t limit = 0;
    std::atomic<int64_t> seq{0};
};
HleTrace& hle_trace() {
    static HleTrace* t = [] {
        auto* h = new HleTrace;
        h->prefix = config_get_str("hle", "trace", "");
        h->limit = config_get_int("hle", "trace_limit", 20000);
        return h;
    }();
    return *t;
}

constexpr size_t kMaxThunks = mem::kHleSize / 16;

size_t thunk_index(uint64_t addr) { return static_cast<size_t>((addr - mem::kHleBase) / 16); }

}  // namespace

void fn_register(uint64_t guest_addr, GuestFn fn) {
    if (!guest_addr || !fn) return;
    std::lock_guard<std::mutex> lk(g_write);
    Table* t = table_for_write();
    size_t h = hash_addr(guest_addr) & t->mask;
    for (;;) {
        uint64_t a = t->slots[h].addr.load(std::memory_order_relaxed);
        if (a == guest_addr) {
            t->slots[h].fn.store(fn, std::memory_order_release);
            g_fn_epoch.fetch_add(1, std::memory_order_release);      // a replaced function: the per-thread caches may hold the old one
            ic_invalidate_all();                                       // ... and so may the lifted code's inline caches
            return;
        }
        if (!a) break;
        h = (h + 1) & t->mask;
    }
    t->slots[h].fn.store(fn, std::memory_order_relaxed);
    t->slots[h].addr.store(guest_addr, std::memory_order_release);
    t->used++;
    g_fn_count.fetch_add(1, std::memory_order_relaxed);
}

GuestFn fn_lookup(uint64_t guest_addr) {
    Table* t = g_table.load(std::memory_order_acquire);
    if (!t) return nullptr;
    size_t h = hash_addr(guest_addr) & t->mask;
    for (;;) {
        uint64_t a = t->slots[h].addr.load(std::memory_order_acquire);
        if (a == guest_addr) return t->slots[h].fn.load(std::memory_order_acquire);
        if (!a) return nullptr;
        h = (h + 1) & t->mask;
    }
}

size_t fn_count() { return g_fn_count.load(std::memory_order_relaxed); }

void fn_clear() {
    std::lock_guard<std::mutex> lk(g_write);
    g_table.store(nullptr);
    g_fn_epoch.fetch_add(1, std::memory_order_release);
    g_all_tables.clear();
    g_fn_count = 0;
    ic_invalidate_all();
    std::lock_guard<std::mutex> l2(g_byhost_lock);
    g_byhost = {};
}

uint64_t guest_function_of(uint64_t host_rip) {
    std::lock_guard<std::mutex> lk(g_byhost_lock);
    size_t n = g_fn_count.load(std::memory_order_relaxed);
    if (g_byhost.built_for != n) {
        g_byhost.v.clear();
        Table* t = g_table.load(std::memory_order_acquire);
        if (t)
            for (size_t i = 0; i <= t->mask; i++) {
                uint64_t a = t->slots[i].addr.load(std::memory_order_acquire);
                GuestFn f = a ? t->slots[i].fn.load(std::memory_order_acquire) : nullptr;
                if (f) g_byhost.v.emplace_back(reinterpret_cast<uint64_t>(f), a);
            }
        std::sort(g_byhost.v.begin(), g_byhost.v.end());
        g_byhost.built_for = n;
    }
    auto& v = g_byhost.v;
    auto it = std::upper_bound(v.begin(), v.end(), std::make_pair(host_rip, ~uint64_t(0)));
    if (it == v.begin()) return 0;
    --it;
    if (host_rip - it->first > 0x400000) return 0;
    return it->second;
}

// ---- inline caches (ctx.h NxIc) --------------------------------------------------------------------------------------
// Every indirect call site and import stub of the lifted code has a static NxIc. The sites are registered here at their first fill so
// that a replaced registration (fn_register of an address that has a function already), a changed import resolution or an ab window
// can empty them all. Targets are interned per (address, function) and never freed: a thread may still be calling through one while
// another empties the caches. A site whose target changes more than kIcRefills times is left as it is (state 2): a polymorphic call
// keeps the one target it holds and every other one goes through dispatch_call, without the shared cache line being rewritten.
namespace {
constexpr uint32_t kIcRefills = 8;
std::mutex g_ic_lock;
std::vector<NxIc*> g_ic_sites;
std::unordered_map<uint64_t, NxIcTarget*> g_ic_targets;   // address -> current target (a replaced function gets a new one)
std::atomic<uint64_t> g_ic_fills{0}, g_ic_refills{0}, g_ic_frozen{0}, g_ic_invalidations{0};
std::atomic<bool> g_ic_on{true};

struct IcCfg {
    int mode = 1;          // 0 off, 1 on, 2 ab
    int64_t ab_ms = 5000;
};
const IcCfg& ic_cfg() {
    static const IcCfg cfg = [] {
        IcCfg c;
        const std::string m = config_get_str("cpu", "inline_caches", "on");
        c.mode = m == "off" || m == "0" ? 0 : m == "ab" ? 2 : 1;
        c.ab_ms = std::max<int64_t>(100, config_get_int("cpu", "ic_ab_ms", 5000));
        if (c.mode != 1) NXLOG_I("dispatch", "inline caches of the lifted code: %s", c.mode == 0 ? "off" : "ab");
        if (c.mode == 0) g_ic_on.store(false);
        if (c.mode == 2) {
            std::thread([ms = c.ab_ms] {
                for (bool on = true;; on = !on) {
                    g_ic_on.store(on, std::memory_order_relaxed);
                    if (!on) ic_invalidate_all();
                    ft::mark(on ? "ic.on" : "ic.off");
                    Sleep(static_cast<DWORD>(ms));
                }
            }).detach();
        }
        return c;
    }();
    return cfg;
}
}  // namespace

bool ic_enabled() {
    (void)ic_cfg();
    return g_ic_on.load(std::memory_order_relaxed);
}

void ic_fill(NxIc* ic, uint64_t addr, GuestFn fn) {
    if (!fn || !ic_enabled()) return;
    const uint32_t state = __atomic_load_n(&ic->state, __ATOMIC_RELAXED);
    if (state == 2) return;
    std::lock_guard<std::mutex> lk(g_ic_lock);
    if (ic->state == 2) return;
    if (ic->state == 0) {
        g_ic_sites.push_back(ic);
        __atomic_store_n(&ic->state, 1u, __ATOMIC_RELAXED);
    }
    if (ic->t) {
        if (++ic->misses > kIcRefills) {
            __atomic_store_n(&ic->state, 2u, __ATOMIC_RELAXED);
            g_ic_frozen.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        g_ic_refills.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_ic_fills.fetch_add(1, std::memory_order_relaxed);
    }
    NxIcTarget*& t = g_ic_targets[addr];
    if (!t || t->fn != fn) t = new NxIcTarget{addr, fn};
    __atomic_store_n(&ic->t, static_cast<const NxIcTarget*>(t), __ATOMIC_RELEASE);
}

void ic_invalidate_all() {
    std::lock_guard<std::mutex> lk(g_ic_lock);
    for (NxIc* ic : g_ic_sites) {
        __atomic_store_n(&ic->t, static_cast<const NxIcTarget*>(nullptr), __ATOMIC_RELEASE);
        ic->misses = 0;
        __atomic_store_n(&ic->state, 0u, __ATOMIC_RELAXED);
    }
    g_ic_sites.clear();
    g_ic_targets.clear();        // the targets themselves stay allocated (see above)
    g_ic_invalidations.fetch_add(1, std::memory_order_relaxed);
}

IcStats ic_stats() {
    std::lock_guard<std::mutex> lk(g_ic_lock);
    IcStats s;
    s.sites = g_ic_sites.size();
    s.targets = g_ic_targets.size();
    s.fills = g_ic_fills.load();
    s.refills = g_ic_refills.load();
    s.frozen = g_ic_frozen.load();
    s.invalidations = g_ic_invalidations.load();
    return s;
}

// ---- HLE thunks -----------------------------------------------------------------------------------------------------

uint64_t hle_register(const char* name, HleFn fn) {
    std::lock_guard<std::mutex> lk(g_hle_lock);
    if (g_thunks.capacity() < kMaxThunks) g_thunks.reserve(kMaxThunks);   // never reallocates: dispatch reads without the lock
    auto it = g_thunk_by_name.find(name);
    size_t i;
    if (it != g_thunk_by_name.end()) {
        i = it->second;
    } else {
        if (g_thunks.size() >= kMaxThunks) {
            NXLOG_E("dispatch", "out of HLE thunks (%zu)", kMaxThunks);
            return 0;
        }
        i = g_thunks.size();
        g_thunks.push_back(std::make_unique<Thunk>());
        g_thunks[i]->name = name;
        g_thunk_by_name.emplace(name, i);
        g_thunk_count.store(g_thunks.size(), std::memory_order_release);
    }
    g_thunks[i]->fn = fn;
    return mem::kHleBase + i * 16;
}

uint64_t hle_address(const char* name) {
    {
        std::lock_guard<std::mutex> lk(g_hle_lock);
        auto it = g_thunk_by_name.find(name);
        if (it != g_thunk_by_name.end()) return mem::kHleBase + it->second * 16;
    }
    return hle_register(name, nullptr);   // no handler: a logging stub
}

HleFn hle_lookup(const char* name) {
    std::lock_guard<std::mutex> lk(g_hle_lock);
    auto it = g_thunk_by_name.find(name);
    return it == g_thunk_by_name.end() ? nullptr : g_thunks[it->second]->fn;
}

void hle_mark_leaf(const char* name) {
    std::lock_guard<std::mutex> lk(g_hle_lock);
    auto it = g_thunk_by_name.find(name);
    if (it != g_thunk_by_name.end() && g_thunks[it->second]->fn) g_thunks[it->second]->leaf.store(true, std::memory_order_release);
}

HleFn hle_leaf_fn(uint64_t addr) {
    if (!hle_is_thunk(addr)) return nullptr;
    const Thunk& t = *g_thunks[thunk_index(addr)];
    return t.leaf.load(std::memory_order_acquire) ? t.fn : nullptr;
}

bool hle_is_thunk(uint64_t addr) {
    return addr - mem::kHleBase < mem::kHleSize && thunk_index(addr) < g_thunk_count.load(std::memory_order_acquire);
}

const char* hle_name_at(uint64_t addr) {
    if (!hle_is_thunk(addr)) return nullptr;
    return g_thunks[thunk_index(addr)]->name.c_str();
}

size_t hle_count() { return g_thunk_count.load(std::memory_order_acquire); }

size_t hle_stub_count() {
    std::lock_guard<std::mutex> lk(g_hle_lock);
    size_t n = 0;
    for (auto& t : g_thunks) n += t->fn == nullptr;
    return n;
}

uint64_t hle_calls(uint64_t addr) { return hle_is_thunk(addr) ? g_thunks[thunk_index(addr)]->calls.load() : 0; }

std::vector<HleInfo> hle_list() {
    std::lock_guard<std::mutex> lk(g_hle_lock);
    std::vector<HleInfo> v;
    for (auto& t : g_thunks) v.push_back({t->name, t->calls.load(), t->fn == nullptr});
    return v;
}

void hle_clear() {
    std::lock_guard<std::mutex> lk(g_hle_lock);
    g_thunk_count.store(0);
    g_thunks.clear();
    g_thunk_by_name.clear();
}

const char* hle_current_name() { return t_hle_name; }
void hle_restore_current(const char* name) { t_hle_name = name; }

namespace {
thread_local uint32_t t_host_cb_flags = 0;
}
uint32_t host_callback_flags() { return t_host_cb_flags; }
void host_callback_restore(uint32_t flags) { t_host_cb_flags = flags; }

// ---- dispatch ---------------------------------------------------------------------------------------------------------

void trap(const char* what, uint64_t addr) {
    NXLOG_E("guest", "GUEST TRAP: %s (0x%llx)", what, static_cast<unsigned long long>(addr));
    log_flush();
    RaiseException(0xE0000001, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    TerminateProcess(GetCurrentProcess(), 2);
    for (;;) {}
}

void dispatch_call(::Ctx* c, uint64_t addr) {
    // HLE thunks live in their own address range, away from every lifted function: no hash lookup for them
    const bool in_hle_range = addr - mem::kHleBase < mem::kHleSize;
    if (!in_hle_range) {
        const uint32_t ep = g_fn_epoch.load(std::memory_order_acquire);
        if (t_fn_epoch != ep) {
            std::memset(t_fn_cache, 0, sizeof t_fn_cache);
            t_fn_epoch = ep;
        }
        FnCacheEntry& e = t_fn_cache[((addr >> 2) ^ (addr >> 12)) & (kFnCache - 1)];
        // An empty entry is {0, nullptr}: without the fn test a guest call to address 0 would "hit" it and jump to host address 0.
        if (e.fn && e.addr == addr) {
            e.fn(c);
            return;
        }
        if (GuestFn fn = fn_lookup(addr)) {
            e.addr = addr;
            e.fn = fn;
            fn(c);
            return;
        }
    }
    if (hle_is_thunk(addr)) {
        Thunk& t = *g_thunks[thunk_index(addr)];
        // A plain increment, not a locked add: game threads call the same NVN thunks all the time, and the locked add on the shared
        // line was 1% of the worker threads' busy time. The count is diagnostics; a race may lose an increment.
        t.calls.store(t.calls.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        const char* saved = t_hle_name;
        t_hle_name = t.name.c_str();
        // [hle] trace: the line is written after the handler ran, so it carries the result (x0) next to the arguments
        const HleTrace& tr = hle_trace();
        const bool traced = !tr.prefix.empty() && t.name.compare(0, tr.prefix.size(), tr.prefix) == 0;
        uint64_t targs[4] = {};
        int64_t tseq = 0;
        if (traced) {
            tseq = const_cast<HleTrace&>(tr).seq.fetch_add(1, std::memory_order_relaxed) + 1;
            for (int i = 0; i < 4; i++) targs[i] = reg(c, i);
        }
        if (t.fn) {
            static const bool time_all = config_get_bool("prof", "trace_hle_all", false);
            if (ft::on() && (time_all || t.slow.load(std::memory_order_relaxed) || (++t_hle_sample & 31) == 0)) {
                const uint64_t t0 = ft::now_ns();
                t.fn(c);
                const uint64_t t1 = ft::now_ns();
                if (t1 - t0 >= 20000 && !t.slow.load(std::memory_order_relaxed)) t.slow.store(true, std::memory_order_relaxed);
                if (t1 - t0 >= 100000) {       // frame trace: HLE calls (services, NVN entry points) of at least 100 us
                    int z = t.ft_zone.load(std::memory_order_relaxed);
                    if (z < 0) { z = ft::zone_id(("hle:" + t.name).c_str()); t.ft_zone.store(z, std::memory_order_relaxed); }
                    ft::event(static_cast<uint16_t>(z), t0, t1, 0);
                }
            } else {
                t.fn(c);
            }
        } else {
            // No handler: the console would fault on an unresolved import. A silent 0 is a fake answer, so say so (every
            // call up to 20, then every 1000th) and return 0.
            uint64_t n = t.calls.load(std::memory_order_relaxed);
            nxrt::gap("import", "unimplemented import called", t.name);
            if (n <= 20 || n % 1000 == 0)
                NXLOG_W("hle", "UNIMPLEMENTED import %s called (call #%llu, lr=0x%llx, x0=0x%llx x1=0x%llx): returning 0",
                        t.name.c_str(), static_cast<unsigned long long>(n), static_cast<unsigned long long>(lr(c)),
                        static_cast<unsigned long long>(reg(c, 0)), static_cast<unsigned long long>(reg(c, 1)));
            reg(c, 0) = 0;
        }
        if (traced && tseq <= tr.limit)
            NXLOG_I("hle-trace", "#%lld %s(x0=0x%llx x1=0x%llx x2=0x%llx x3=0x%llx) lr=0x%llx -> 0x%llx", static_cast<long long>(tseq), t.name.c_str(),
                    static_cast<unsigned long long>(targs[0]), static_cast<unsigned long long>(targs[1]), static_cast<unsigned long long>(targs[2]),
                    static_cast<unsigned long long>(targs[3]), static_cast<unsigned long long>(lr(c)), static_cast<unsigned long long>(reg(c, 0)));
        t_hle_name = saved;
        return;
    }
    if (eh::longjmp_landing(c, addr)) return;   // does not return when it lands
    trap("indirect call to an address that is not a known function", addr);
}

}  // namespace nxrt
