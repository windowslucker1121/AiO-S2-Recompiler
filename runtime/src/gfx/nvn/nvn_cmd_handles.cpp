// SPDX-License-Identifier: GPL-3.0-or-later
// NVNcommandHandle table and the command-memory lifetime rule. On the console a handle is valid until the game records over
// the command memory the list used (or frees the memory pool). The back end keeps the same rule with an estimate of the
// memory a list uses (nvn_cmd_lifecycle in api_cmdbuf.cpp): a list is dropped when a newer list occupies an overlapping
// range of command memory or when its pool is finalized. Finalizing the command buffer does not end its lists: the command
// words are in the game's memory, not the buffer object's. Animal Crossing initializes a command buffer per worker job each
// frame, records one list, finalizes the buffer and only then has the frame's primary list call that handle. Only a list
// without a region in the game's command memory goes with its buffer: one in its buffer's overflow ring (api_cmdbuf.cpp: the
// memory the out-of-memory callback would have handed over), which nothing could overlap once the buffer is gone, or one
// without a region at all. There is no count limit: the game calls some lists (character draw lists, the map) long after
// recording them. nvnDeviceFinalizeCommandHandle drops a handle at once (title A finalizes the previous one at each
// BeginRecording).
// The rule is taken from SuperMarioWonderRecomp docs/nvn/DEBUG.md #26, DEBUG-2 #90, #91 (MIT); the code is new.
#include <atomic>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

#include "nxrt/config.h"
#include "nxrt/gauge.h"
#include "nxrt/log.h"
#include "nvn_cmd.h"
#include "nvn_cfg.h"

namespace nxrt::nvn::be {
namespace {

struct Entry {
    Ref<CmdList> list;
    uint64_t start = 0, end = 0;
};

std::shared_mutex g_mu;   // shared: the render thread's look-ups (thousands per frame); exclusive: changes
std::unordered_map<uint64_t, Entry> g_handles;
std::map<uint64_t, uint64_t> g_regions;      // start -> handle (regions never overlap among live lists)
std::atomic<uint64_t> g_next{0x1000};
std::atomic<uint64_t> g_missing{0};
std::atomic<int64_t> g_finalized{0};

// Diagnostics for "CallCommands of an unknown handle": why a handle went away (bounded; the oldest entries are forgotten).
struct Dropped {
    const char* why;
    uint64_t by;                 // the handle whose region overlapped (overlap only)
    uint64_t start, end;         // the dropped list's region
    uint64_t by_start, by_end;   // the overlapping list's region
    uint32_t draws, records;
};
std::unordered_map<uint64_t, Dropped> g_dropped;
std::atomic<uint32_t> g_miss_logged{0};

// Not in the game's command memory: no region, or a region in an overflow ring.
bool unbacked(const Entry& e) { return e.end <= e.start || is_overflow_addr(e.start); }

void drop_locked(uint64_t handle, const char* why = "?", uint64_t by = 0, uint64_t by_start = 0, uint64_t by_end = 0) {
    auto it = g_handles.find(handle);
    if (it == g_handles.end()) return;
    if (g_dropped.size() > 65536) g_dropped.clear();
    g_dropped[handle] = Dropped{why, by, it->second.start, it->second.end, by_start, by_end,
                                it->second.list ? it->second.list->draw_count : 0u,
                                it->second.list ? it->second.list->record_count : 0u};
    if (it->second.end > it->second.start) {
        auto r = g_regions.find(it->second.start);
        if (r != g_regions.end() && r->second == handle) g_regions.erase(r);
    }
    if (it->second.list) it->second.list->dropped.store(true, std::memory_order_release);   // per-thread caches of handle_lookup
    g_handles.erase(it);   // the list lives on if a replay or another list still holds it
}

// Gauges (nxrt/gauge.h). The callbacks take g_mu; they are removed before g_mu and the tables are destroyed.
struct HandleGauges {
    HandleGauges() {
        nxrt::gauge_register("nvn.handles", [] {
            std::lock_guard<std::shared_mutex> lk(g_mu);
            return static_cast<int64_t>(g_handles.size());
        });
        nxrt::gauge_register("nvn.handle_regions", [] {
            std::lock_guard<std::shared_mutex> lk(g_mu);
            return static_cast<int64_t>(g_regions.size());
        });
        // live handles whose list is not in the game's command memory (overflow ring or no region); bounded by the rings
        nxrt::gauge_register("nvn.handles_unbacked", [] {
            std::lock_guard<std::shared_mutex> lk(g_mu);
            int64_t n = 0;
            for (const auto& [h, e] : g_handles) n += unbacked(e) ? 1 : 0;
            return n;
        });
        // nvnDeviceFinalizeCommandHandle calls so far (cumulative)
        nxrt::gauge_register("nvn.handles_finalized", [] { return g_finalized.load(std::memory_order_relaxed); });
    }
    ~HandleGauges() {
        nxrt::gauge_unregister("nvn.handles");
        nxrt::gauge_unregister("nvn.handle_regions");
        nxrt::gauge_unregister("nvn.handles_unbacked");
        nxrt::gauge_unregister("nvn.handles_finalized");
    }
};
HandleGauges g_handle_gauges;

}  // namespace

uint64_t handle_register(Ref<CmdList> list) {
    const uint64_t h = g_next.fetch_add(8);
    list->handle = h;
    std::lock_guard<std::shared_mutex> lk(g_mu);
    if (list->mem_end > list->mem_start) {
        // drop every older list whose region overlaps this one
        auto it = g_regions.lower_bound(list->mem_start);
        if (it != g_regions.begin()) {
            auto prev = std::prev(it);
            auto e = g_handles.find(prev->second);
            if (e != g_handles.end() && e->second.end > list->mem_start) it = prev;
        }
        std::vector<uint64_t> drop;
        for (; it != g_regions.end() && it->first < list->mem_end; ++it) drop.push_back(it->second);
        for (uint64_t d : drop) drop_locked(d, "overlap", h, list->mem_start, list->mem_end);
        g_regions[list->mem_start] = h;
    }
    Entry e;
    e.start = list->mem_start;
    e.end = list->mem_end;
    e.list = std::move(list);
    g_handles.emplace(h, std::move(e));
    return h;
}

std::atomic<bool> g_cache_on{true};
void handle_cache_enable(bool on) { g_cache_on.store(on, std::memory_order_relaxed); }
std::atomic<uint32_t> g_cache_mask{16383};
void handle_cache_big(bool on) { g_cache_mask.store(on ? 16383u : 1023u, std::memory_order_relaxed); }

namespace {
Ref<CmdList> handle_lookup_table(uint64_t handle);
}

// The render thread and the texture look-ahead look up thousands of handles per frame, each under g_mu shared (contended by the game
// thread's registrations): a direct-mapped cache per thread. A cached list is the handle's list for as long as it is not dropped.
Ref<CmdList> handle_lookup(uint64_t handle, bool count_call) {
    const bool cfg = NX_CFG_BOOL("nvn", "handle_cache", true);
    if (!cfg || !g_cache_on.load(std::memory_order_relaxed)) return handle_lookup_table(handle);
    struct Slot { uint64_t h = 0; Ref<CmdList> l; };
    // 16k slots: handles are issued 8 apart, and the lists a frame calls span tens of thousands of handles (1024 slots thrashed: the
    // table look-up under the shared lock, contended by the game's registrations, was ~4% of the render thread in the village scene)
    thread_local Slot* cache = new Slot[16384];
    Slot& s = cache[(handle >> 3) & g_cache_mask.load(std::memory_order_relaxed)];
    if (s.h == handle && s.l && !s.l->dropped.load(std::memory_order_acquire)) {
        if (count_call) s.l->calls.fetch_add(1, std::memory_order_relaxed);
        return s.l;
    }
    Ref<CmdList> l = handle_lookup_table(handle);
    if (l) { s.h = handle; s.l = l; }
    return l;
}

namespace {
Ref<CmdList> handle_lookup_table(uint64_t handle) {
    std::shared_lock<std::shared_mutex> lk(g_mu);
    auto it = g_handles.find(handle);
    if (it == g_handles.end()) {
        g_missing.fetch_add(1);
        auto d = g_dropped.find(handle);
        const bool empty = d != g_dropped.end() && d->second.records == 0;   // an empty list: nothing is lost
        if (!empty && g_miss_logged.fetch_add(1) < 200) {
            if (d == g_dropped.end())
                NXLOG_W("nvn.handles", "unknown handle 0x%llx: %s", static_cast<unsigned long long>(handle),
                        handle >= g_next.load() || handle < 0x1000 ? "never issued" : "issued, drop not recorded");
            else
                NXLOG_W("nvn.handles", "unknown handle 0x%llx (%u records, %u draws, region %llx-%llx): dropped by %s, handle 0x%llx region %llx-%llx",
                        static_cast<unsigned long long>(handle), d->second.records, d->second.draws, static_cast<unsigned long long>(d->second.start),
                        static_cast<unsigned long long>(d->second.end), d->second.why, static_cast<unsigned long long>(d->second.by),
                        static_cast<unsigned long long>(d->second.by_start), static_cast<unsigned long long>(d->second.by_end));
        }
        return {};
    }
    it->second.list->calls.fetch_add(1, std::memory_order_relaxed);
    return it->second.list;
}
}  // namespace

void handle_release(uint64_t handle) {
    g_finalized.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::shared_mutex> lk(g_mu);
    drop_locked(handle, "finalize");
}

void handle_release_unbacked(uint64_t handle) {
    std::lock_guard<std::shared_mutex> lk(g_mu);
    auto it = g_handles.find(handle);
    if (it != g_handles.end() && unbacked(it->second)) drop_locked(handle, "unbacked release");
}

void handles_release_pool(const MemoryPool* pool) {
    if (!pool || !pool->base) return;
    const uint64_t lo = pool->gpu_base, hi = pool->gpu_base + pool->size;
    std::lock_guard<std::shared_mutex> lk(g_mu);
    std::vector<uint64_t> drop;
    for (auto& [h, e] : g_handles)
        if (e.end > e.start && e.start >= lo && e.start < hi) drop.push_back(h);
    for (uint64_t h : drop) drop_locked(h, "pool released");
}

const char* op_name(Op op) {
    static const char* const kNames[] = {
    "Nop",
    "BindProgram",
    "BindBuffers",
    "BindTextures",
    "BindImages",
    "BindVertexAttribs",
    "BindVertexStreams",
    "BindBlend",
    "BindColor",
    "BindChannelMask",
    "BindDepthStencil",
    "BindPolygon",
    "BindMultisample",
    "SetTexturePool",
    "SetSamplerPool",
    "SetViewports",
    "SetScissors",
    "SetDepthRanges",
    "SetDepthBounds",
    "SetStencil",
    "SetBlendColor",
    "SetPointSize",
    "SetLineWidth",
    "SetPolygonOffsetClamp",
    "SetAlphaRef",
    "SetSampleMask",
    "SetRasterizerDiscard",
    "SetDepthClamp",
    "SetPrimitiveRestart",
    "SetPatchSize",
    "SetTessLevels",
    "SetRenderEnable",
    "SetRenderTargets",
    "SetCoverageModulationTable",
    "SetConservativeRaster",
    "DrawArrays",
    "DrawElements",
    "DrawIndirect",
    "DrawTexture",
    "ClearColor",
    "ClearDepthStencil",
    "ClearTexture",
    "ClearBuffer",
    "CopyBufferToBuffer",
    "CopyBufferToTexture",
    "CopyTextureToBuffer",
    "CopyTextureToTexture",
    "Downsample",
    "UpdateUniformBuffer",
    "Dispatch",
    "DispatchIndirect",
    "ReportCounter",
    "ReportValue",
    "ResetCounter",
    "Barrier",
    "FenceSync",
    "WaitSync",
    "SignalEvent",
    "WaitEvent",
    "CallCommands",
    "DiscardColor",
    "DiscardDepthStencil",
    "ResolveDepth",
    "BeginXfb",
    "EndXfb",
    "PauseXfb",
    "ResumeXfb",
    "DrawXfb",
    "PushDebugGroup",
    "PopDebugGroup",
    "SubmitDeviceCommands",
    "EvaluateDlss",
    "Small",
    "DrawMeshTasks"};
    const size_t i = static_cast<size_t>(op);
    return i < sizeof(kNames) / sizeof(kNames[0]) ? kNames[i] : "?";
}

size_t handles_live() {
    std::lock_guard<std::shared_mutex> lk(g_mu);
    return g_handles.size();
}

uint64_t handles_missing() { return g_missing.load(); }

}  // namespace nxrt::nvn::be
