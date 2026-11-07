// SPDX-License-Identifier: GPL-3.0-or-later
// Guest/host object binding and the memory pool registry (GPU address -> pool).
#include <algorithm>
#include <map>
#include <shared_mutex>

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
}  // namespace

void pool_register(MemoryPool* p) {
    std::unique_lock<std::shared_mutex> lk(g_pool_mu);
    g_pools[p->gpu_base] = p;
}

void pool_unregister(MemoryPool* p) {
    std::unique_lock<std::shared_mutex> lk(g_pool_mu);
    auto it = g_pools.find(p->gpu_base);
    if (it != g_pools.end() && it->second == p) g_pools.erase(it);
}

void pool_collect(std::vector<Ref<MemoryPool>>& out) {
    std::shared_lock<std::shared_mutex> lk(g_pool_mu);
    for (auto& [base, p] : g_pools) out.push_back(Ref<MemoryPool>::share(p));
}

PoolHit pool_lookup(uint64_t gpu_address) {
    std::shared_lock<std::shared_mutex> lk(g_pool_mu);
    auto it = g_pools.upper_bound(gpu_address);
    if (it == g_pools.begin()) return {};
    --it;
    MemoryPool* p = it->second;
    if (gpu_address >= p->gpu_base + p->size) return {};
    return {p, gpu_address - p->gpu_base};
}

}  // namespace nxrt::nvn::be
