// SPDX-License-Identifier: GPL-3.0-or-later
// Guest/host object binding and the memory pool registry (GPU address -> pool).
#include <algorithm>
#include <map>
#include <shared_mutex>
#include <string>
#include <vector>

#include <windows.h>

#include "nxrt/config.h"
#include "nxrt/gauge.h"
#include "nvn_core.h"

namespace nxrt::nvn::be {

void bind_guest(void* guest, Obj* obj) {
    GuestHeader* h = static_cast<GuestHeader*>(guest);
    Obj* old = (h->magic == kGuestMagic) ? h->host : nullptr;
    h->host = obj;
    h->kind = static_cast<uint32_t>(obj->kind);
    h->magic = kGuestMagic;
    if (old) old->unref();
}

void unbind_guest(void* guest) {
    GuestHeader* h = static_cast<GuestHeader*>(guest);
    Obj* old = (h->magic == kGuestMagic) ? h->host : nullptr;
    h->host = nullptr;
    h->magic = 0;
    h->kind = 0;
    if (old) old->unref();
}

namespace {
std::shared_mutex g_pool_mu;
std::map<uint64_t, MemoryPool*> g_pools;   // gpu_base -> pool

// Gauge (nxrt/gauge.h). The callback takes g_pool_mu; it is removed before the map is destroyed.
struct PoolGauges {
    PoolGauges() {
        nxrt::gauge_register("nvn.pool_registry", [] {
            std::shared_lock<std::shared_mutex> lk(g_pool_mu);
            return static_cast<int64_t>(g_pools.size());
        });
    }
    ~PoolGauges() { nxrt::gauge_unregister("nvn.pool_registry"); }
};
PoolGauges g_pool_gauges;

// [nvn] pool_lookup_snapshot (D3, bet 1.3 step 1): pool_lookup without the registry lock. Every register / unregister publishes an immutable
// sorted snapshot (bases, ends, pools); lookups load it with one acquire and search it, starting from the thread's last hit. Old snapshots are
// freed two seconds after they were replaced (a lookup holds one for microseconds). The registry lock was 8.5% of the render thread's busy
// samples (first scene, 2026-11-08), most of it the shared lock's cache-line traffic with the game's threads. on (default) | off | ab (paired
// windows with [nvn] d3_ab, measurement only). First scene, d3_ab = 300, 11 pairs of ~7 s windows: on 35.0 / off 33.1 fps, paired median +1.9 fps.
struct PoolSnap {
    std::vector<uint64_t> base, end;
    std::vector<MemoryPool*> pool;
};
std::atomic<const PoolSnap*> g_snap{nullptr};
std::vector<std::pair<const PoolSnap*, ULONGLONG>> g_retired;   // g_pool_mu held exclusive
void publish_snapshot() {                                          // g_pool_mu held exclusive
    auto* s = new PoolSnap();
    s->base.reserve(g_pools.size()); s->end.reserve(g_pools.size()); s->pool.reserve(g_pools.size());
    for (auto& [b, p] : g_pools) { s->base.push_back(b); s->end.push_back(b + p->size); s->pool.push_back(p); }
    const ULONGLONG now = GetTickCount64();
    if (const PoolSnap* old = g_snap.exchange(s, std::memory_order_acq_rel)) g_retired.emplace_back(old, now);
    size_t keep = 0;
    for (auto& r : g_retired) { if (now - r.second > 2000) delete r.first; else g_retired[keep++] = r; }
    g_retired.resize(keep);
}
thread_local const PoolSnap* tl_snap = nullptr;
thread_local size_t tl_idx = 0;
// Per thread, the index the search found for a page, checked before it is used: the address lies in that pool and no later base is at or
// below it, i.e. exactly what the binary search returns (the render thread asks ~20k times per frame for a few hundred distinct addresses
// among ~3,300 pools; the search was 5% of its busy time).
struct PageHint { const PoolSnap* s = nullptr; uint64_t page = ~0ull; size_t idx = 0; };
thread_local PageHint tl_hint[512];
PoolHit lookup_snapshot(const PoolSnap* s, uint64_t a) {
    if (tl_snap == s && tl_idx < s->base.size() && a >= s->base[tl_idx] && a < s->end[tl_idx]) return {s->pool[tl_idx], a - s->base[tl_idx]};
    const uint64_t page = a >> 12;
    PageHint& h = tl_hint[(page ^ (page >> 9)) & 511];
    if (h.s == s && h.page == page) {
        const size_t i = h.idx;
        if (i < s->base.size() && a >= s->base[i] && a < s->end[i] && (i + 1 == s->base.size() || s->base[i + 1] > a)) {
            tl_snap = s;
            tl_idx = i;
            return {s->pool[i], a - s->base[i]};
        }
    }
    const auto it = std::upper_bound(s->base.begin(), s->base.end(), a);
    if (it == s->base.begin()) return {};
    const size_t i = static_cast<size_t>(it - s->base.begin()) - 1;
    if (a >= s->end[i]) return {};
    tl_snap = s;
    tl_idx = i;
    h.s = s; h.page = page; h.idx = i;
    return {s->pool[i], a - s->base[i]};
}
}  // namespace

std::atomic<uint64_t> g_pool_gen{0};

void pool_register(MemoryPool* p) {
    std::unique_lock<std::shared_mutex> lk(g_pool_mu);
    g_pools[p->gpu_base] = p;
    publish_snapshot();
    g_pool_gen.fetch_add(1, std::memory_order_relaxed);
}

uint64_t pool_registry_generation() { return g_pool_gen.load(std::memory_order_relaxed); }
std::atomic<uint64_t> g_pool_unreg_gen{0};
uint64_t pool_unregister_generation() { return g_pool_unreg_gen.load(std::memory_order_acquire); }

void pool_unregister(MemoryPool* p) {
    std::unique_lock<std::shared_mutex> lk(g_pool_mu);
    auto it = g_pools.find(p->gpu_base);
    if (it != g_pools.end() && it->second == p) g_pools.erase(it);
    publish_snapshot();
    g_pool_gen.fetch_add(1, std::memory_order_relaxed);
    g_pool_unreg_gen.fetch_add(1, std::memory_order_acq_rel);
}

void pool_collect(std::vector<Ref<MemoryPool>>& out) {
    std::shared_lock<std::shared_mutex> lk(g_pool_mu);
    for (auto& [base, p] : g_pools) out.push_back(Ref<MemoryPool>::share(p));
}

std::atomic<int> g_pool_snapshot_ab{1};   // [nvn] pool_lookup_snapshot = ab: the d3_ab window (nvn_arch_exp.cpp) sets it

int pool_snapshot_mode() {   // 0 off, 1 on, 2 ab
    static const int m = [] {
        const std::string v = config_get_str("nvn", "pool_lookup_snapshot", "on");
        return v == "off" || v == "0" ? 0 : v == "ab" ? 2 : 1;
    }();
    return m;
}

PoolHit pool_lookup(uint64_t gpu_address) {
    const int mode = pool_snapshot_mode();
    if (mode == 1 || (mode == 2 && g_pool_snapshot_ab.load(std::memory_order_relaxed))) {
        if (const PoolSnap* s = g_snap.load(std::memory_order_acquire)) return lookup_snapshot(s, gpu_address);
    }
    std::shared_lock<std::shared_mutex> lk(g_pool_mu);
    auto it = g_pools.upper_bound(gpu_address);
    if (it == g_pools.begin()) return {};
    --it;
    MemoryPool* p = it->second;
    if (gpu_address >= p->gpu_base + p->size) return {};
    return {p, gpu_address - p->gpu_base};
}

}  // namespace nxrt::nvn::be
