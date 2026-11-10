// SPDX-License-Identifier: GPL-3.0-or-later
// [prof] gpu_stats (measurement only, default off): per-draw GPU pipeline statistics (vertex invocations, clipping primitives in/out,
// fragment shader invocations) and a precise occlusion count (samples that passed the depth/stencil tests) for every replayed draw,
// summed per draw family (fragment + pre-raster shader, pass size, depth test) and logged every [prof] gpu_stats_seconds (default 10):
// `gpu stats:` lines. Fragment invocations far above the passed samples of an opaque draw mean the fragments ran before (or without)
// the depth test rejected them (late Z); invocations far above the pass's pixel count mean overdraw. The queries do not drain the GPU
// like the timestamp stamps of [prof] gpu_draws, but they still cost a little: compare only proportions.
#include "nvn_gpu_stats.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "nxrt/config.h"
#include "nxrt/log.h"
#include "nvn_gpu_internal.h"

namespace nxrt::nvn::be::gpu::gstats {
namespace {

constexpr uint32_t kSlotsMax = 64;
constexpr uint32_t kPerSlot = 4096;
constexpr const char* kTag = "nvn.stats";

struct Acc {
    uint64_t draws = 0, vs = 0, clip_in = 0, clip_out = 0, fs = 0, samples = 0, samples_draws = 0;
};

struct SlotState {
    uint32_t n = 0;                        // queries begun in this slot's current use
    std::vector<uint32_t> label;           // per query: interned label
    std::vector<uint8_t> occ;              // per query: an occlusion query was begun too
};

bool g_tried = false, g_on = false;
VkQueryPool g_stat = VK_NULL_HANDLE, g_occ = VK_NULL_HANDLE;
SlotState g_slots[kSlotsMax];
int32_t g_active = -1;                     // query index of the open draw (render thread)
bool g_active_occ = false;
uint32_t g_cur_slot = 0;
std::unordered_map<std::string, uint32_t> g_ids;
std::vector<std::string> g_names;
std::vector<Acc> g_acc;
uint64_t g_presents0 = 0;
uint64_t g_mesh_switches = 0;
std::chrono::steady_clock::time_point g_t0;

bool init() {
    if (g_tried) return g_on;
    g_tried = true;
    if (!config_get_bool("prof", "gpu_stats", false)) return false;
    const gfx::DeviceFeatures& f = device().features();
    if (!f.pipeline_statistics_query) { NXLOG_W(kTag, "gpu_stats: the device has no pipeline statistics queries"); return false; }
    VkQueryPoolCreateInfo qi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qi.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS;
    qi.queryCount = kSlotsMax * kPerSlot;
    qi.pipelineStatistics = VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT | VK_QUERY_PIPELINE_STATISTIC_CLIPPING_INVOCATIONS_BIT |
                            VK_QUERY_PIPELINE_STATISTIC_CLIPPING_PRIMITIVES_BIT | VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT;
    if (vkCreateQueryPool(vkdev(), &qi, nullptr, &g_stat) != VK_SUCCESS) return false;
    VkQueryPoolCreateInfo oi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    oi.queryType = VK_QUERY_TYPE_OCCLUSION;
    oi.queryCount = kSlotsMax * kPerSlot;
    if (vkCreateQueryPool(vkdev(), &oi, nullptr, &g_occ) != VK_SUCCESS) g_occ = VK_NULL_HANDLE;
    g_t0 = std::chrono::steady_clock::now();
    g_on = true;
    NXLOG_I(kTag, "gpu_stats: per-draw pipeline statistics on (occlusion %s, precise %s)", g_occ ? "on" : "off",
            f.occlusion_query_precise ? "yes" : "no");
    return true;
}

void log_window() {
    const auto now = std::chrono::steady_clock::now();
    static const double every = static_cast<double>(config_get_int("prof", "gpu_stats_seconds", 10));
    const double secs = std::chrono::duration<double>(now - g_t0).count();
    if (secs < every) return;
    const uint64_t presents = counters().presents.load();
    const double frames = static_cast<double>(std::max<uint64_t>(1, presents - g_presents0));
    std::vector<uint32_t> order;
    Acc tot;
    for (uint32_t i = 0; i < g_acc.size(); ++i) {
        if (!g_acc[i].draws) continue;
        order.push_back(i);
        tot.draws += g_acc[i].draws; tot.vs += g_acc[i].vs; tot.clip_in += g_acc[i].clip_in; tot.clip_out += g_acc[i].clip_out; tot.fs += g_acc[i].fs;
        tot.samples += g_acc[i].samples;
    }
    std::sort(order.begin(), order.end(), [](uint32_t a, uint32_t b) { return g_acc[a].fs > g_acc[b].fs; });
    NXLOG_I(kTag, "gpu stats over %.1f s, %.0f frames, per frame: draws %.0f vs %.0f clip in %.0f out %.0f fs %.0f samples passed %.0f, mesh/vertex pipeline switches %.1f", secs, frames,
            tot.draws / frames, tot.vs / frames, tot.clip_in / frames, tot.clip_out / frames, tot.fs / frames, tot.samples / frames, g_mesh_switches / frames);
    static const size_t top = static_cast<size_t>(config_get_int("prof", "gpu_stats_top", 40));
    for (size_t k = 0; k < order.size() && k < top; ++k) {
        const Acc& a = g_acc[order[k]];
        NXLOG_I(kTag, "gpu stats  %-60s per frame: draws %6.1f vs %9.0f clip %8.0f/%8.0f fs %9.0f samples %9.0f (of %.1f draws) fs/sample %.2f",
                g_names[order[k]].c_str(), a.draws / frames, a.vs / frames, a.clip_in / frames, a.clip_out / frames, a.fs / frames, a.samples / frames,
                a.samples_draws / frames, a.samples ? static_cast<double>(a.fs) / static_cast<double>(a.samples) : 0.0);
    }
    for (Acc& a : g_acc) a = Acc();
    g_presents0 = presents;
    g_mesh_switches = 0;
    g_t0 = now;
}

}  // namespace

bool on() { return g_tried ? g_on : init(); }

void slot_begin(VkCommandBuffer cb, uint32_t slot) {
    if (!on() || slot >= kSlotsMax) return;
    SlotState& s = g_slots[slot];
    if (s.n) {   // the slot's previous submission has completed (acquire_slot waited for it): add its results
        std::vector<uint64_t> st(static_cast<size_t>(s.n) * 5), oc(static_cast<size_t>(s.n) * 2);
        const uint32_t base = slot * kPerSlot;
        vkGetQueryPoolResults(vkdev(), g_stat, base, s.n, st.size() * 8, st.data(), 5 * 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        if (g_occ) vkGetQueryPoolResults(vkdev(), g_occ, base, s.n, oc.size() * 8, oc.data(), 2 * 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        for (uint32_t i = 0; i < s.n; ++i) {
            const uint64_t* r = &st[static_cast<size_t>(i) * 5];
            if (!r[4]) continue;
            Acc& a = g_acc[s.label[i]];
            ++a.draws;
            a.vs += r[0];
            a.clip_in += r[1];
            a.clip_out += r[2];
            a.fs += r[3];
            if (g_occ && s.occ[i] && oc[static_cast<size_t>(i) * 2 + 1]) {
                a.samples += oc[static_cast<size_t>(i) * 2];
                ++a.samples_draws;
            }
        }
        log_window();
    }
    s.n = 0;
    s.label.clear();
    s.occ.clear();
    vkCmdResetQueryPool(cb, g_stat, slot * kPerSlot, kPerSlot);
    if (g_occ) vkCmdResetQueryPool(cb, g_occ, slot * kPerSlot, kPerSlot);
    g_cur_slot = slot;
}

void draw_begin(VkCommandBuffer cb, const char* label, bool stats_free, bool occlusion_free) {
    if (!on()) return;
    if (g_active >= 0) draw_end(cb);
    if (!stats_free) return;
    SlotState& s = g_slots[g_cur_slot];
    if (s.n >= kPerSlot) return;
    auto it = g_ids.find(label);
    if (it == g_ids.end()) {
        it = g_ids.emplace(label, static_cast<uint32_t>(g_names.size())).first;
        g_names.emplace_back(label);
        g_acc.emplace_back();
    }
    const uint32_t q = g_cur_slot * kPerSlot + s.n;
    vkCmdBeginQuery(cb, g_stat, q, 0);
    g_active_occ = g_occ && occlusion_free;
    if (g_active_occ) vkCmdBeginQuery(cb, g_occ, q, VK_QUERY_CONTROL_PRECISE_BIT);
    s.label.push_back(it->second);
    s.occ.push_back(g_active_occ ? 1 : 0);
    g_active = static_cast<int32_t>(q);
    ++s.n;
}

void draw_end(VkCommandBuffer cb) {
    if (g_active < 0) return;
    vkCmdEndQuery(cb, g_stat, static_cast<uint32_t>(g_active));
    if (g_active_occ) vkCmdEndQuery(cb, g_occ, static_cast<uint32_t>(g_active));
    g_active = -1;
    g_active_occ = false;
}

bool active() { return g_active >= 0; }

void note_mesh_switch() { if (g_on) ++g_mesh_switches; }

}  // namespace nxrt::nvn::be::gpu::gstats
