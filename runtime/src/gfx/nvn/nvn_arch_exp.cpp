// SPDX-License-Identifier: GPL-3.0-or-later
// Architecture experiments E1 / E2a / E4 (nvn_arch_exp.h). Measurement only, each behind its own [nvn] key, off by default.
#include "nvn_arch_exp.h"

#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include <volk.h>

#include "nvn_core.h"
#include "nvn_gpu_internal.h"
#include "nxrt/config.h"
#include "nxrt/log.h"

namespace nxrt::nvn::be::archexp {
namespace {

constexpr const char* kTag = "nvn.exp";
constexpr uint64_t kPage = 4096;

uint64_t mix(uint64_t h, uint64_t w) {
    h ^= w + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    h *= 0xff51afd7ed558ccdull;
    return h ^ (h >> 33);
}

std::atomic<uint64_t> g_game_frame{0};

// ---- E1: list repetition -----------------------------------------------------------------------------------------------------
struct ListStats {
    std::mutex mu;
    static constexpr int kVariants = 3;            // 0 exact bytes, 1 address fields masked, 2 structure (addresses and inline data masked)
    static constexpr int kWindow = 8;              // frames looked back
    std::deque<std::array<std::unordered_set<uint64_t>, kVariants>> past;
    std::array<std::unordered_set<uint64_t>, kVariants> cur;
    uint64_t frame = 0;
    // since the last log line
    uint64_t lists = 0, bytes = 0, draws = 0, frames = 0, hash_ns = 0;
    uint64_t hit_draws[kVariants] = {}, hit_lists[kVariants] = {};
    uint64_t empty_lists = 0;
    std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
};
ListStats& ls() { static ListStats s; return s; }

// Zeroes the 8 bytes at `off` of the record's payload (address fields).
void zero_at(std::vector<uint8_t>& rec, size_t off) {
    if (off + 8 <= rec.size()) std::memset(rec.data() + off, 0, 8);
}

// The record with its address fields (and, for variant 2, inline data) zeroed. `rec` holds header + payload + blob.
void mask_record(Op op, std::vector<uint8_t>& rec, int variant) {
    const size_t P = sizeof(RecHeader);
    auto blob_at = [&](size_t payload_size) { return P + ((payload_size + 7u) & ~size_t{7}); };
    switch (op) {
    case Op::BindBuffers: {
        P_BindBuffers p;
        std::memcpy(&p, rec.data() + P, sizeof p);
        const size_t b = blob_at(sizeof(P_BindBuffers));
        for (uint32_t i = 0; i < p.count; ++i) zero_at(rec, b + i * 16);
        break;
    }
    case Op::DrawElements: zero_at(rec, P + offsetof(P_DrawElements, index_address)); break;
    case Op::DrawIndirect:
        zero_at(rec, P + offsetof(P_DrawIndirect, index_address));
        zero_at(rec, P + offsetof(P_DrawIndirect, indirect_address));
        zero_at(rec, P + offsetof(P_DrawIndirect, count_address));
        break;
    case Op::ClearBuffer: zero_at(rec, P + offsetof(P_ClearBuffer, address)); break;
    case Op::CopyBufferToBuffer:
        zero_at(rec, P + offsetof(P_CopyBufferToBuffer, src));
        zero_at(rec, P + offsetof(P_CopyBufferToBuffer, dst));
        break;
    case Op::CopyBufferToTexture:
    case Op::CopyTextureToBuffer: zero_at(rec, P + offsetof(P_CopyBufTex, address)); break;
    case Op::UpdateUniformBuffer:
        zero_at(rec, P + offsetof(P_UpdateUniformBuffer, address));
        if (variant == 2) {
            const size_t b = blob_at(sizeof(P_UpdateUniformBuffer));
            if (b < rec.size()) std::memset(rec.data() + b, 0, rec.size() - b);
        }
        break;
    case Op::DispatchIndirect: zero_at(rec, P + offsetof(P_DispatchIndirect, address)); break;
    case Op::ReportCounter:
    case Op::ResetCounter:
    case Op::BeginXfb:
    case Op::EndXfb:
    case Op::PauseXfb:
    case Op::ResumeXfb: zero_at(rec, P + offsetof(P_Counter, address)); break;
    case Op::ReportValue: zero_at(rec, P + offsetof(P_ReportValue, address)); break;
    case Op::SetRenderEnable: if (rec.size() >= P + sizeof(P_RenderEnableCond)) zero_at(rec, P + offsetof(P_RenderEnableCond, address)); break;
    case Op::DrawXfb: zero_at(rec, P + offsetof(P_DrawXfb, control)); break;
    case Op::DrawMeshTasks:
        zero_at(rec, P + offsetof(P_DrawMesh, indirect));
        zero_at(rec, P + offsetof(P_DrawMesh, count_address));
        break;
    case Op::SubmitDeviceCommands:
        zero_at(rec, P + offsetof(P_SubmitDeviceCommands, stream_address));
        zero_at(rec, P + offsetof(P_SubmitDeviceCommands, compute_address));
        break;
    case Op::Small:
        zero_at(rec, P + offsetof(P_Small, addr));
        if (variant == 2) {   // push constants and the like: inline data
            const size_t b = blob_at(sizeof(P_Small));
            if (b < rec.size()) std::memset(rec.data() + b, 0, rec.size() - b);
        }
        break;
    default: break;
    }
}

void hash_list(const CmdList& l, uint64_t out[ListStats::kVariants]) {
    uint64_t h0 = 0x12345, h1 = 0x12345, h2 = 0x12345;
    const uint8_t* d = l.bytes.data();
    const size_t n = l.bytes.size();
    for (size_t i = 0; i + 8 <= n; i += 8) { uint64_t w; std::memcpy(&w, d + i, 8); h0 = mix(h0, w); }
    std::vector<uint8_t> rec;
    for (RecordReader r(l); r.next();) {
        const uint8_t* at = &r.payload<uint8_t>() - sizeof(RecHeader);   // the record's header
        rec.assign(at, at + r.size());
        mask_record(r.op(), rec, 1);
        for (size_t i = 0; i + 8 <= rec.size(); i += 8) { uint64_t w; std::memcpy(&w, rec.data() + i, 8); h1 = mix(h1, w); }
        mask_record(r.op(), rec, 2);
        for (size_t i = 0; i + 8 <= rec.size(); i += 8) { uint64_t w; std::memcpy(&w, rec.data() + i, 8); h2 = mix(h2, w); }
    }
    out[0] = h0; out[1] = h1; out[2] = h2;
}

// ---- E2a: Flush / Invalidate contract ---------------------------------------------------------------------------------------
int kind_of(int flags) {           // 0 CPU_CACHED, 1 CPU_UNCACHED, 2 other (CPU_NO_ACCESS ...)
    const int cpu = flags & 0x7;   // NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS 1, CPU_UNCACHED 2, CPU_CACHED 4
    if (cpu & 4) return 0;
    if (cpu & 2) return 1;
    return 2;
}
const char* const kKindName[3] = {"cached", "uncached", "other"};

struct PageRec {
    uint64_t hash = 0;
    uint64_t seen_seq = 0;     // sequence number of the last look at it
    uint64_t flush_seq = 0;    // sequence number of the last flush over it
    int kind = 2;
};
struct Coherence {
    std::mutex mu;
    uint64_t seq = 1;
    std::unordered_map<uint64_t, PageRec> pages;        // by page address
    std::unordered_set<uint64_t> gpu_pages;             // pages some GPU command may have written (ever)
    std::vector<std::pair<uint64_t, int>> suspects;     // changed without a flush, judged at the next sample (GPU writes catch up)
    // invalidates: ring of {lo, hi, game frame}
    struct Inval { uint64_t lo, hi, frame; };
    std::vector<Inval> invals;
    size_t inval_pos = 0;
    // counters by kind
    uint64_t seen[3] = {}, again[3] = {}, changed[3] = {}, changed_flushed[3] = {}, unflushed[3] = {}, gpu_excluded[3] = {}, unreadable = 0;
    uint64_t flushes = 0, flush_bytes = 0, invalidates = 0, inval_bytes = 0, samples = 0, sample_ns = 0;
    uint64_t fault_read[3] = {}, fault_write[3] = {}, fault_read_inval[3] = {}, fault_write_inval[3] = {};
    std::map<uint64_t, uint64_t> unflushed_by_pool;     // pool base -> pages
    std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
};
Coherence& co() { static Coherence c; return c; }

int64_t coherence_every() {
    static const int64_t n = std::max<int64_t>(0, config_get_int("nvn", "exp_coherence", 0));
    return n;
}

void collect_ranges(const CmdList& l, std::vector<std::pair<uint64_t, uint64_t>>& out, int depth) {
    for (RecordReader r(l); r.next();) {
        switch (r.op()) {
        case Op::BindBuffers: {
            const auto& p = r.payload<P_BindBuffers>();
            const uint8_t* b = r.blob<P_BindBuffers>();
            for (uint32_t i = 0; i < p.count; ++i) {
                uint64_t a, s;
                std::memcpy(&a, b + i * 16, 8);
                std::memcpy(&s, b + i * 16 + 8, 8);
                if (a && s) out.emplace_back(a, std::min<uint64_t>(s, 64ull << 20));
            }
            break;
        }
        case Op::DrawElements: {
            const auto& p = r.payload<P_DrawElements>();
            const uint64_t isz = p.type == 0 ? 1 : p.type == 1 ? 2 : 4;
            if (p.index_address) out.emplace_back(p.index_address, uint64_t{p.count} * isz);
            break;
        }
        case Op::CallCommands: {
            if (depth > 4) break;
            const auto& p = r.payload<P_CallCommands>();
            const uint8_t* b = r.blob<P_CallCommands>();
            for (uint32_t i = 0; i < p.count; ++i) {
                uint64_t h;
                std::memcpy(&h, b + i * 8, 8);
                if (Ref<CmdList> sub = handle_lookup(h)) collect_ranges(*sub, out, depth + 1);
            }
            break;
        }
        default: break;
        }
    }
}

bool readable(uint64_t addr) {      // committed and not a guard / no-access page (held compute results are PAGE_NOACCESS)
    MEMORY_BASIC_INFORMATION mi{};
    if (!VirtualQuery(reinterpret_cast<const void*>(addr), &mi, sizeof mi)) return false;
    if (mi.State != MEM_COMMIT) return false;
    const DWORD p = mi.Protect & 0xff;
    return !(mi.Protect & PAGE_GUARD) && p != PAGE_NOACCESS && p != 0;
}

uint64_t hash_page(uint64_t addr) {
    const uint64_t* w = reinterpret_cast<const uint64_t*>(addr);
    uint64_t h = 0x9876;
    for (size_t i = 0; i < kPage / 8; ++i) h = mix(h, w[i]);
    return h;
}

void coherence_log(Coherence& c) {
    const auto now = std::chrono::steady_clock::now();
    if (now - c.last < std::chrono::seconds(10)) return;
    c.last = now;
    for (int k = 0; k < 3; ++k) {
        if (!c.seen[k] && !c.fault_read[k] && !c.fault_write[k]) continue;
        NXLOG_I(kTag, "E2a %s pools: pages looked at %llu (again %llu), changed %llu (flushed %llu, unflushed CPU writes %llu, GPU-written skipped %llu); "
                      "guard faults on held results: read %llu (after an invalidate %llu), write %llu (after an invalidate %llu)",
                kKindName[k], (unsigned long long)c.seen[k], (unsigned long long)c.again[k], (unsigned long long)c.changed[k],
                (unsigned long long)c.changed_flushed[k], (unsigned long long)c.unflushed[k], (unsigned long long)c.gpu_excluded[k],
                (unsigned long long)c.fault_read[k], (unsigned long long)c.fault_read_inval[k], (unsigned long long)c.fault_write[k],
                (unsigned long long)c.fault_write_inval[k]);
    }
    NXLOG_I(kTag, "E2a totals: samples %llu (%.2f ms each), flushes %llu (%llu MiB), invalidates %llu (%llu MiB), unreadable pages %llu, tracked pages %zu, GPU-written pages %zu",
            (unsigned long long)c.samples, c.samples ? c.sample_ns / 1e6 / c.samples : 0.0, (unsigned long long)c.flushes,
            (unsigned long long)(c.flush_bytes >> 20), (unsigned long long)c.invalidates, (unsigned long long)(c.inval_bytes >> 20),
            (unsigned long long)c.unreadable, c.pages.size(), c.gpu_pages.size());
    std::vector<std::pair<uint64_t, uint64_t>> top(c.unflushed_by_pool.begin(), c.unflushed_by_pool.end());
    std::sort(top.begin(), top.end(), [](auto& a, auto& b) { return a.second > b.second; });
    for (size_t i = 0; i < top.size() && i < 8; ++i) {
        const PoolHit h = pool_lookup(top[i].first);
        NXLOG_I(kTag, "E2a   unflushed pages in pool %llx (%llu KiB, flags 0x%x): %llu", (unsigned long long)top[i].first,
                (unsigned long long)(h.pool ? h.pool->size >> 10 : 0), h.pool ? h.pool->flags : 0, (unsigned long long)top[i].second);
    }
}

// ---- E4: emit timers ----------------------------------------------------------------------------------------------------------
enum EmitCat { kCatCmd = 0, kCatDesc, kCatSubmit, kCatCount };
struct alignas(64) EmitAcc { uint64_t ticks[kCatCount] = {}; uint64_t calls[kCatCount] = {}; };
EmitAcc g_emit;                       // render thread only
thread_local int tl_render = -1;      // -1 unknown, 0 no, 1 render thread

inline bool is_render() {
    if (tl_render < 0) tl_render = gpu::on_render_thread() ? 1 : 0;
    return tl_render == 1;
}

template <int Cat, auto* Slot, typename T> struct Hook;
template <int Cat, auto* Slot, typename R, typename... A>
struct Hook<Cat, Slot, R(VKAPI_PTR*)(A...)> {
    static inline R(VKAPI_PTR* orig)(A...) = nullptr;
    static R VKAPI_PTR call(A... a) {
        if (!is_render()) return orig(a...);
        struct T { uint64_t t0 = __rdtsc(); ~T() { g_emit.ticks[Cat] += __rdtsc() - t0; ++g_emit.calls[Cat]; } } t;
        return orig(a...);
    }
    static void install() {
        if (!*Slot || orig) return;
        orig = *Slot;
        *Slot = &call;
    }
};
#define NXEXP_HOOK(cat, name) Hook<cat, &name, PFN_##name>::install()

void install_hooks() {
    NXEXP_HOOK(kCatCmd, vkCmdBindPipeline); NXEXP_HOOK(kCatCmd, vkCmdBindDescriptorSets); NXEXP_HOOK(kCatCmd, vkCmdPushConstants);
    NXEXP_HOOK(kCatCmd, vkCmdCopyBuffer); NXEXP_HOOK(kCatCmd, vkCmdPipelineBarrier2); NXEXP_HOOK(kCatCmd, vkCmdPipelineBarrier);
    NXEXP_HOOK(kCatCmd, vkCmdDraw); NXEXP_HOOK(kCatCmd, vkCmdDrawIndexed); NXEXP_HOOK(kCatCmd, vkCmdDrawIndirect);
    NXEXP_HOOK(kCatCmd, vkCmdDrawIndexedIndirect); NXEXP_HOOK(kCatCmd, vkCmdDrawIndirectCount); NXEXP_HOOK(kCatCmd, vkCmdDrawIndexedIndirectCount);
    NXEXP_HOOK(kCatCmd, vkCmdDrawMeshTasksEXT); NXEXP_HOOK(kCatCmd, vkCmdDrawMeshTasksIndirectEXT); NXEXP_HOOK(kCatCmd, vkCmdDrawMeshTasksIndirectCountEXT);
    NXEXP_HOOK(kCatCmd, vkCmdDrawIndirectByteCountEXT);
    NXEXP_HOOK(kCatCmd, vkCmdDispatch); NXEXP_HOOK(kCatCmd, vkCmdDispatchIndirect);
    NXEXP_HOOK(kCatCmd, vkCmdBeginQuery); NXEXP_HOOK(kCatCmd, vkCmdEndQuery); NXEXP_HOOK(kCatCmd, vkCmdResetQueryPool);
    NXEXP_HOOK(kCatCmd, vkCmdCopyQueryPoolResults); NXEXP_HOOK(kCatCmd, vkCmdWriteTimestamp2); NXEXP_HOOK(kCatCmd, vkCmdWriteTimestamp);
    NXEXP_HOOK(kCatCmd, vkCmdCopyImageToBuffer); NXEXP_HOOK(kCatCmd, vkCmdCopyBufferToImage); NXEXP_HOOK(kCatCmd, vkCmdCopyImage);
    NXEXP_HOOK(kCatCmd, vkCmdBlitImage); NXEXP_HOOK(kCatCmd, vkCmdClearColorImage); NXEXP_HOOK(kCatCmd, vkCmdClearDepthStencilImage);
    NXEXP_HOOK(kCatCmd, vkCmdClearAttachments); NXEXP_HOOK(kCatCmd, vkCmdUpdateBuffer); NXEXP_HOOK(kCatCmd, vkCmdFillBuffer);
    NXEXP_HOOK(kCatCmd, vkCmdBeginRendering); NXEXP_HOOK(kCatCmd, vkCmdEndRendering);
    NXEXP_HOOK(kCatCmd, vkCmdBindVertexBuffers2); NXEXP_HOOK(kCatCmd, vkCmdBindVertexBuffers); NXEXP_HOOK(kCatCmd, vkCmdBindIndexBuffer);
    NXEXP_HOOK(kCatCmd, vkCmdSetViewport); NXEXP_HOOK(kCatCmd, vkCmdSetScissor); NXEXP_HOOK(kCatCmd, vkCmdSetViewportWithCount);
    NXEXP_HOOK(kCatCmd, vkCmdSetScissorWithCount); NXEXP_HOOK(kCatCmd, vkCmdSetStencilReference); NXEXP_HOOK(kCatCmd, vkCmdSetStencilWriteMask);
    NXEXP_HOOK(kCatCmd, vkCmdSetStencilCompareMask); NXEXP_HOOK(kCatCmd, vkCmdSetStencilOp); NXEXP_HOOK(kCatCmd, vkCmdSetStencilTestEnable);
    NXEXP_HOOK(kCatCmd, vkCmdSetBlendConstants); NXEXP_HOOK(kCatCmd, vkCmdSetFrontFace); NXEXP_HOOK(kCatCmd, vkCmdSetCullMode);
    NXEXP_HOOK(kCatCmd, vkCmdSetPrimitiveTopology); NXEXP_HOOK(kCatCmd, vkCmdSetLineWidth); NXEXP_HOOK(kCatCmd, vkCmdSetDepthBias);
    NXEXP_HOOK(kCatCmd, vkCmdSetDepthBiasEnable); NXEXP_HOOK(kCatCmd, vkCmdSetDepthBounds); NXEXP_HOOK(kCatCmd, vkCmdSetDepthTestEnable);
    NXEXP_HOOK(kCatCmd, vkCmdSetDepthWriteEnable); NXEXP_HOOK(kCatCmd, vkCmdSetDepthCompareOp); NXEXP_HOOK(kCatCmd, vkCmdSetDepthBoundsTestEnable);
    NXEXP_HOOK(kCatCmd, vkCmdSetPrimitiveRestartEnable); NXEXP_HOOK(kCatCmd, vkCmdSetRasterizerDiscardEnable);
    NXEXP_HOOK(kCatCmd, vkCmdSetFragmentShadingRateKHR);
    NXEXP_HOOK(kCatCmd, vkCmdSetColorBlendEnableEXT); NXEXP_HOOK(kCatCmd, vkCmdSetColorBlendEquationEXT); NXEXP_HOOK(kCatCmd, vkCmdSetColorWriteMaskEXT);
    NXEXP_HOOK(kCatCmd, vkCmdSetPolygonModeEXT); NXEXP_HOOK(kCatCmd, vkCmdSetDepthClampEnableEXT); NXEXP_HOOK(kCatCmd, vkCmdSetLogicOpEnableEXT);
    NXEXP_HOOK(kCatCmd, vkCmdSetAlphaToCoverageEnableEXT); NXEXP_HOOK(kCatCmd, vkCmdSetRasterizationSamplesEXT); NXEXP_HOOK(kCatCmd, vkCmdSetSampleMaskEXT);
    NXEXP_HOOK(kCatCmd, vkCmdSetVertexInputEXT); NXEXP_HOOK(kCatCmd, vkCmdSetDepthClipEnableEXT);
    NXEXP_HOOK(kCatCmd, vkCmdBeginTransformFeedbackEXT); NXEXP_HOOK(kCatCmd, vkCmdEndTransformFeedbackEXT);
    NXEXP_HOOK(kCatCmd, vkCmdBindTransformFeedbackBuffersEXT); NXEXP_HOOK(kCatCmd, vkCmdBeginDebugUtilsLabelEXT); NXEXP_HOOK(kCatCmd, vkCmdEndDebugUtilsLabelEXT);
    NXEXP_HOOK(kCatCmd, vkBeginCommandBuffer); NXEXP_HOOK(kCatCmd, vkEndCommandBuffer);
    NXEXP_HOOK(kCatDesc, vkAllocateDescriptorSets); NXEXP_HOOK(kCatDesc, vkUpdateDescriptorSets); NXEXP_HOOK(kCatDesc, vkResetDescriptorPool);
    NXEXP_HOOK(kCatDesc, vkCmdPushDescriptorSetKHR);
    NXEXP_HOOK(kCatSubmit, vkQueueSubmit); NXEXP_HOOK(kCatSubmit, vkQueueSubmit2);
}

}  // namespace

// ---- E1 ------------------------------------------------------------------------------------------------------------------------
bool lists_on() {
    static const bool on = config_get_bool("nvn", "exp_lists", false);
    return on;
}

void list_recorded(const CmdList& l) {
    if (!lists_on()) return;
    const auto t0 = std::chrono::steady_clock::now();
    uint64_t h[ListStats::kVariants];
    hash_list(l, h);
    const uint64_t ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
    ListStats& s = ls();
    std::lock_guard<std::mutex> lk(s.mu);
    s.lists++;
    s.bytes += l.bytes.size();
    s.draws += l.draw_count;
    s.hash_ns += ns;
    if (!l.draw_count) s.empty_lists++;
    for (int v = 0; v < ListStats::kVariants; ++v) {
        bool hit = s.cur[v].count(h[v]) != 0;    // the same list twice in a frame counts too
        for (const auto& f : s.past) if (!hit && f[v].count(h[v])) hit = true;
        if (hit) { s.hit_draws[v] += l.draw_count; s.hit_lists[v]++; }
        s.cur[v].insert(h[v]);
    }
}

void game_present() {
    g_game_frame.fetch_add(1, std::memory_order_relaxed);
    if (!lists_on()) return;
    ListStats& s = ls();
    std::lock_guard<std::mutex> lk(s.mu);
    s.past.push_back(std::move(s.cur));
    for (auto& c : s.cur) c.clear();
    while (s.past.size() > ListStats::kWindow) s.past.pop_front();
    s.frames++;
    const auto now = std::chrono::steady_clock::now();
    if (now - s.last < std::chrono::seconds(10) || !s.frames) return;
    s.last = now;
    auto pct = [&](uint64_t a, uint64_t b) { return b ? 100.0 * a / b : 0.0; };
    NXLOG_I(kTag, "E1 lists: %llu frames, %.1f lists / %.0f KiB / %.0f draws recorded per frame (%.2f ms hashing per frame); draws in lists seen in the "
                  "last %d frames: exact %.1f%%, addresses masked %.1f%%, structure only %.1f%% (lists: %.1f%% / %.1f%% / %.1f%%, empty lists %.1f%%)",
            (unsigned long long)s.frames, double(s.lists) / s.frames, double(s.bytes) / 1024.0 / s.frames, double(s.draws) / s.frames,
            s.hash_ns / 1e6 / s.frames, ListStats::kWindow, pct(s.hit_draws[0], s.draws), pct(s.hit_draws[1], s.draws), pct(s.hit_draws[2], s.draws),
            pct(s.hit_lists[0], s.lists), pct(s.hit_lists[1], s.lists), pct(s.hit_lists[2], s.lists), pct(s.empty_lists, s.lists));
    s.lists = s.bytes = s.draws = s.frames = s.hash_ns = s.empty_lists = 0;
    for (int v = 0; v < ListStats::kVariants; ++v) s.hit_draws[v] = s.hit_lists[v] = 0;
}

// ---- E2a -------------------------------------------------------------------------------------------------------------------------
bool coherence_on() { return coherence_every() > 0; }

void note_flush(uint64_t addr, uint64_t size) {
    if (!coherence_on() || !size) return;
    Coherence& c = co();
    std::lock_guard<std::mutex> lk(c.mu);
    c.flushes++;
    c.flush_bytes += size;
    const uint64_t s = ++c.seq;
    const uint64_t lo = addr & ~(kPage - 1), hi = (addr + size + kPage - 1) & ~(kPage - 1);
    if ((hi - lo) / kPage <= c.pages.size()) {
        for (uint64_t p = lo; p < hi; p += kPage) {
            auto it = c.pages.find(p);
            if (it != c.pages.end()) it->second.flush_seq = s;
        }
    } else {
        for (auto& [p, r] : c.pages) if (p >= lo && p < hi) r.flush_seq = s;
    }
}

void note_invalidate(uint64_t addr, uint64_t size) {
    if (!coherence_on() || !size) return;
    Coherence& c = co();
    std::lock_guard<std::mutex> lk(c.mu);
    c.invalidates++;
    c.inval_bytes += size;
    if (c.invals.size() < 4096) c.invals.push_back({addr, addr + size, g_game_frame.load(std::memory_order_relaxed)});
    else { c.invals[c.inval_pos] = {addr, addr + size, g_game_frame.load(std::memory_order_relaxed)}; c.inval_pos = (c.inval_pos + 1) % c.invals.size(); }
}

void note_gpu_write(uint64_t addr, uint64_t size) {
    if (!coherence_on() || !size || size > (1ull << 32)) return;
    Coherence& c = co();
    std::lock_guard<std::mutex> lk(c.mu);
    for (uint64_t p = addr & ~(kPage - 1); p < addr + size; p += kPage) c.gpu_pages.insert(p);
}

void note_guard_fault(uint64_t addr, int access, int pool_flags) {
    if (!coherence_on()) return;
    Coherence& c = co();
    std::lock_guard<std::mutex> lk(c.mu);
    const int k = kind_of(pool_flags);
    const uint64_t now = g_game_frame.load(std::memory_order_relaxed);
    bool inval = false;
    for (const auto& iv : c.invals) if (addr >= iv.lo && addr < iv.hi && now - iv.frame <= 2) { inval = true; break; }
    const bool write = access == 1 || access == 3;
    (write ? c.fault_write : c.fault_read)[k]++;
    if (inval) (write ? c.fault_write_inval : c.fault_read_inval)[k]++;
}

void submit_sample(const std::vector<Ref<CmdList>>& lists) {
    const int64_t every = coherence_every();
    if (every <= 0) return;
    if (g_game_frame.load(std::memory_order_relaxed) % static_cast<uint64_t>(every)) return;
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<std::pair<uint64_t, uint64_t>> ranges;
    for (const auto& l : lists) if (l) collect_ranges(*l, ranges, 0);
    std::vector<uint64_t> pages;
    for (const auto& [a, s] : ranges)
        for (uint64_t p = a & ~(kPage - 1); p < a + s; p += kPage) pages.push_back(p);
    std::sort(pages.begin(), pages.end());
    pages.erase(std::unique(pages.begin(), pages.end()), pages.end());
    // hash outside the lock (the readable check first: held compute results are guarded)
    struct Look { uint64_t page; uint64_t hash; int kind; uint64_t pool; };
    std::vector<Look> looks;
    looks.reserve(pages.size());
    uint64_t unreadable = 0;
    uint64_t region_lo = 0, region_hi = 0;
    bool region_ok = false;
    for (uint64_t p : pages) {
        const PoolHit h = pool_lookup(p);
        if (!h.pool || h.pool->is_virtual || !h.pool->base) continue;
        if (!(p >= region_lo && p < region_hi)) {
            MEMORY_BASIC_INFORMATION mi{};
            region_ok = readable(p);
            if (VirtualQuery(reinterpret_cast<const void*>(p), &mi, sizeof mi)) {
                region_lo = reinterpret_cast<uint64_t>(mi.BaseAddress);
                region_hi = region_lo + mi.RegionSize;
            } else {
                region_lo = p; region_hi = p + kPage;
            }
        }
        if (!region_ok) { ++unreadable; continue; }
        looks.push_back({p, hash_page(p), kind_of(h.pool->flags), h.pool->gpu_base});
    }
    Coherence& c = co();
    std::lock_guard<std::mutex> lk(c.mu);
    // last sample's suspects: GPU writes known by now are not CPU writes
    for (const auto& [p, k] : c.suspects) {
        if (c.gpu_pages.count(p)) { c.gpu_excluded[k]++; continue; }
        c.unflushed[k]++;
        const PoolHit h = pool_lookup(p);
        c.unflushed_by_pool[h.pool ? h.pool->gpu_base : 0]++;
    }
    c.suspects.clear();
    const uint64_t s = ++c.seq;
    for (const Look& lk2 : looks) {
        PageRec& r = c.pages[lk2.page];
        c.seen[lk2.kind]++;
        if (r.seen_seq) {
            c.again[lk2.kind]++;
            if (r.hash != lk2.hash) {
                c.changed[lk2.kind]++;
                if (r.flush_seq > r.seen_seq) c.changed_flushed[lk2.kind]++;
                else if (c.gpu_pages.count(lk2.page)) c.gpu_excluded[lk2.kind]++;
                else c.suspects.emplace_back(lk2.page, lk2.kind);
            }
        }
        r.hash = lk2.hash;
        r.seen_seq = s;
        r.kind = lk2.kind;
    }
    c.unreadable += unreadable;
    c.samples++;
    c.sample_ns += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
    coherence_log(c);
}

// ---- E4 ---------------------------------------------------------------------------------------------------------------------------
bool emit_on() {
    static const bool on = config_get_bool("nvn", "exp_emit", false);
    return on;
}

void render_frame_tick() {
    if (!emit_on()) return;
    static bool installed = false;
    static uint64_t last_cycles = 0, last_tsc = 0;
    static double tsc_per_ns = 0;
    static uint64_t frames = 0;
    static EmitAcc base;
    static uint64_t cyc_acc = 0;
    static auto t_last = std::chrono::steady_clock::now();
    static uint64_t tsc_at_last = 0;
    if (!installed) {
        installed = true;
        install_hooks();
        QueryThreadCycleTime(GetCurrentThread(), &last_cycles);
        last_tsc = tsc_at_last = __rdtsc();
        t_last = std::chrono::steady_clock::now();
        NXLOG_I(kTag, "E4: vkCmd*, descriptor and submit calls of the render thread are timed ([nvn] exp_emit)");
        return;
    }
    uint64_t cyc = 0;
    QueryThreadCycleTime(GetCurrentThread(), &cyc);
    cyc_acc += cyc - last_cycles;
    last_cycles = cyc;
    ++frames;
    const auto now = std::chrono::steady_clock::now();
    const double secs = std::chrono::duration<double>(now - t_last).count();
    if (secs < 10.0) return;
    const uint64_t tsc = __rdtsc();
    tsc_per_ns = double(tsc - tsc_at_last) / (secs * 1e9);
    auto ms = [&](uint64_t ticks) { return ticks / tsc_per_ns / 1e6 / frames; };
    const double busy = ms(cyc_acc), cmd = ms(g_emit.ticks[kCatCmd] - base.ticks[kCatCmd]), desc = ms(g_emit.ticks[kCatDesc] - base.ticks[kCatDesc]),
                 sub = ms(g_emit.ticks[kCatSubmit] - base.ticks[kCatSubmit]);
    NXLOG_I(kTag, "E4 render thread per frame (%llu frames, %.1f fps): busy %.2f ms = emit (vkCmd*) %.2f ms (%.0f calls) + descriptors %.2f ms (%.0f calls) "
                  "+ submit %.2f ms + decide (the rest) %.2f ms",
            (unsigned long long)frames, frames / secs, busy, cmd, double(g_emit.calls[kCatCmd] - base.calls[kCatCmd]) / frames, desc,
            double(g_emit.calls[kCatDesc] - base.calls[kCatDesc]) / frames, sub, busy - cmd - desc - sub);
    base = g_emit;
    frames = 0;
    cyc_acc = 0;
    t_last = now;
    tsc_at_last = tsc;
}

}  // namespace nxrt::nvn::be::archexp
