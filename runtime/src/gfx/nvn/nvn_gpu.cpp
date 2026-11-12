// SPDX-License-Identifier: GPL-3.0-or-later
// The Vulkan side of the NVN back end: the render thread, submission slots (command buffer, descriptor pools, upload memory),
// the timeline semaphore, deferred destruction, barriers and the public backend API (include/nxrt/nvn_backend.h).
// Design follows smo-recomp (record on game threads, replay on one render thread) and SuperMarioWonderRecomp docs/nvn (a submit
// is replayed under one lock and never overlaps the next); the code is new.
#include "nxrt/nvn_backend.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "nxrt/config.h"
#include "nxrt/diag.h"
#include "nxrt/frame_limiter.h"
#include "nxrt/ftrace.h"
#include "nxrt/gauge.h"
#include "nxrt/log.h"
#include "nxrt/lowlat.h"
#include "nxrt/gaps.h"
#include "vk_gauges.h"
#include "nvn_capture.h"
#include "nvn_cfg.h"
#include "nvn_hazard.h"
#include "nvn_gpu_internal.h"
#include "nvn_cmdq.h"
#include "nvn_gpu_stats.h"
#include "nvn_impl.h"
#include "nvn_pipeline.h"
#include "nvn_present_pass.h"
#include "extras/output_pass.h"
#include "play/play_selftest.h"

namespace nxrt::nvn::be::gpu {
namespace {

constexpr const char* kTag = "nvn.gpu";
constexpr uint32_t kSlots = 36;                  // about three frames of submissions (the game submits ~11 times a frame): with fewer, recording waits on
                                                 // the GPU for a submission of a recent frame and the CPU cannot get frames ahead
constexpr uint32_t kCkMax = 64;        // frame trace: chunk boundary timestamps per submission (two per boundary)
constexpr uint32_t kGpMax = 32768;     // GPU pass profiler: timestamps per submission (the first scene's main submission needs ~3,000; ~16,000 with [prof] gpu_draws)
constexpr VkDeviceSize kUploadBytes = 8ull << 20;   // per slot; larger uploads get their own staging buffer

// Live-count gauges of this file (leak monitor, nxrt/gauge.h).
nxrt::GaugeCounter g_gauge_deferred("nvn.deferred_destroy");     // closures waiting for their submission to finish (defer_destroy, Frame::defer)
nxrt::GaugeCounter g_gauge_render_queue("nvn.render_queue");     // jobs queued for the render thread
nxrt::GaugeCounter g_gauge_barriers_mem("nvn.cnt.barriers_mem");   // full memory barriers recorded (cumulative; per frame = delta / frames)
nxrt::GaugeCounter g_gauge_barriers_img("nvn.cnt.barriers_img");   // image layout barriers recorded (cumulative)

struct Slot {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkCommandPool pool2 = VK_NULL_HANDLE;   // [nvn] exp_async_cull with a second queue of another family: its pool and command buffer
    VkCommandBuffer cb2 = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> split_cbs;   // command buffers frame_split continues in (from the slot's pool; reused after each pool reset)
    size_t split_used = 0;
    std::vector<VkDescriptorPool> dpools;
    size_t dpool_cur = 0;
    gfx::Buffer upload;
    VkDeviceSize upload_off = 0;
    std::vector<gfx::Buffer> extra;       // staging / overflow buffers of the submission
    uint64_t serial = 0;                  // last submission that used the slot
    bool timed = false;                   // frame trace: the submission wrote GPU timestamps (query pair index = slot index * 2)
    uint64_t cpu_t0 = 0;
    uint64_t sets_live = 0;               // descriptor sets allocated from this slot's pools since the last reset (vk.descriptor_sets)
    uint32_t gp_n = 0;                    // GPU pass profiler: timestamps written by the submission
    std::vector<std::pair<std::string, uint32_t>> gp_labels;   // label and argument of each interval (stamp i closes interval i)
    uint32_t ck_n = 0;                    // frame trace: chunk boundary timestamps written (end of a part, start of the next; cpool)
    // [nvn] alloc_batch: sets allocated ahead per layout (direct-mapped by the layout handle; a collision leaves the spare ones unused)
    struct Spare { VkDescriptorSetLayout layout = VK_NULL_HANDLE; uint32_t n = 0; VkDescriptorSet sets[7] = {}; };
    Spare spare[128];
    // [nvn] dset_recycle: the sets this slot's submission allocated (by layout), and the ones from its earlier use, handed out again
    std::vector<std::pair<VkDescriptorSetLayout, VkDescriptorSet>> used;
    std::unordered_map<VkDescriptorSetLayout, std::vector<VkDescriptorSet>> free_sets;
    uint32_t recycles = 0;
};

// [nvn] dset_recycle (default on; cpuperf_ab_what dset_recycle): see the slot reset in the frame code.
std::atomic<bool> g_dset_recycle_ab{true};
uint64_t g_dset_recycled = 0;   // statistics: sets handed out again (render thread)
bool dset_recycle_now() {
    const bool on = NX_CFG_BOOL("nvn", "dset_recycle", true);
    return on && g_dset_recycle_ab.load(std::memory_order_relaxed);
}

struct Deferred {
    uint64_t serial;
    std::function<void()> fn;
};

struct Gpu {
    gfx::Device* dev = nullptr;
    gfx::Renderer* rend = nullptr;
    FrameLimiter* limiter = nullptr;
    bool keep_aspect = false;
    bool installed_entry_points = false;
    VkSemaphore timeline = VK_NULL_HANDLE;
    // [nvn] exp_async_cull: submissions run on the device's second queue ([gfx] second_queue), each signalling tl2 = its own count; a
    // join on the first queue waits for it and signals the serial there, so the serial timeline stays in order.
    VkSemaphore tl2 = VK_NULL_HANDLE;
    uint64_t tl2_val = 0;
    uint64_t async_n = 0;
    std::atomic<uint64_t> completed{0};
    uint64_t submitted = 0;               // render thread: last serial handed to vkQueueSubmit2
    uint64_t recording = 0;               // render thread: serial of the submission being recorded (0 outside)
    std::atomic<uint64_t> last_submitted{0};
    // Serials are handed out when a submission is queued (the game thread gets its serial at once and goes on while the render thread
    // records), in queue order, so the timeline is signalled in increasing order. Under qmu.
    std::atomic<uint64_t> reserved{0};
    // Presents queued and not yet done; the game thread waits while [nvn] present_ahead of them are pending (back pressure).
    std::mutex pmu;
    std::condition_variable pcv;
    int presents_pending = 0;
    Slot slots[kSlots];
    VkQueryPool qpool = VK_NULL_HANDLE;   // frame trace: two timestamps per slot
    VkQueryPool cpool = VK_NULL_HANDLE;   // frame trace: kCkMax timestamps per slot at submission chunk boundaries (GPU idle between parts)
    VkQueryPool gpool = VK_NULL_HANDLE;   // GPU pass profiler ([prof] gpu_passes): kGpMax timestamps per slot
    uint32_t next_slot = 0;
    Counters ctr;

    // render thread
    std::thread th;
    std::mutex qmu;
    std::condition_variable qcv;
    std::deque<std::function<void()>> queue;
    bool stop = false;
    std::thread::id tid;

    // deferred destruction
    std::mutex dmu;
    std::vector<std::function<void()>> pending_destroy;   // from any thread, assigned a serial by the render thread
    std::deque<Deferred> deferred;                        // render thread

    PresentPass present;
    bool present_ok = false;
    extras::OutputPass output;                 // HDR encode and FSR 1 (S14); used instead of `present` when the extras are on
    bool output_ok = false;
    std::mutex unsupported_mu;
    std::map<std::string, uint64_t, std::less<>> unsupported;
};

Gpu* g = nullptr;
std::mutex g_install_mu;

uint64_t read_completed() {
    uint64_t v = 0;
    if (g && g->timeline) vkGetSemaphoreCounterValue(g->dev->device(), g->timeline, &v);
    if (v > g->completed.load()) g->completed.store(v);
    return g->completed.load();
}

void drain_pending_destroy() {
    std::vector<std::function<void()>> pend;
    {
        std::lock_guard<std::mutex> lk(g->dmu);
        pend.swap(g->pending_destroy);
    }
    const uint64_t s = g->recording ? g->recording : g->submitted;
    for (auto& fn : pend) g->deferred.push_back({s, std::move(fn)});
}

void retire() {
    drain_pending_destroy();
    const uint64_t done = read_completed();
    while (!g->deferred.empty() && g->deferred.front().serial <= done) {
        auto fn = std::move(g->deferred.front().fn);
        g->deferred.pop_front();
        g_gauge_deferred.sub(1);
        fn();
    }
}

thread_local const void* t_render_of = nullptr;   // the back end whose render thread this is (on_render_thread)
void render_loop() {
    g->tid = std::this_thread::get_id();
    t_render_of = g;
    log_set_thread_name("nvn-render");
    diag_register_thread("nvn-render", nullptr);   // so the freeze watchdog and the stall sampler can see its stack
    nxrt::hitch_set_render_thread();
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock<std::mutex> lk(g->qmu);
            if (!g->stop && g->queue.empty()) {
                nxrt::HitchTimer ht(nxrt::kHpRenderIdle);      // the hitch line: the render thread waited for the game
                g->qcv.wait(lk, [] { return g->stop || !g->queue.empty(); });
            }
            if (g->queue.empty()) return;   // stop requested and drained
            job = std::move(g->queue.front());
            g->queue.pop_front();
            g_gauge_render_queue.sub(1);
        }
        try {
            job();
        } catch (const std::exception& e) {
            NXLOG_E(kTag, "render job threw: %s", e.what());
        }
    }
}

bool create_slot(Slot& s) {
    gfx::Device& d = *g->dev;
    VkCommandPoolCreateInfo ci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    ci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    ci.queueFamilyIndex = d.queue_family();
    if (vkCreateCommandPool(d.device(), &ci, nullptr, &s.pool) != VK_SUCCESS) return false;
    vk_gauge_command_pools().add(1);
    VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = s.pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(d.device(), &ai, &s.cb) != VK_SUCCESS) return false;
    vk_gauge_command_buffers().add(1);
    const VkBufferUsageFlags usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                     VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                     VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |   // mesh draws: converted indirect records
                                     (d.features().buffer_device_address ? VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT : 0u);   // the pool address table (A17)
    if (d.create_buffer(kUploadBytes, usage, gfx::MemoryUse::Upload, &s.upload, "nvn upload") != VK_SUCCESS) return false;
    return true;
}

VkDescriptorPool new_dpool() {
    const VkDescriptorPoolSize sizes[] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1u << 16},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1u << 13},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u << 16},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1u << 12},
        {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1u << 10},
        {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1u << 10},
    };
    VkDescriptorPoolCreateInfo ci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    ci.maxSets = 1u << 14;
    ci.poolSizeCount = sizeof(sizes) / sizeof(sizes[0]);
    ci.pPoolSizes = sizes;
    VkDescriptorPool p = VK_NULL_HANDLE;
    if (vkCreateDescriptorPool(g->dev->device(), &ci, nullptr, &p) != VK_SUCCESS) return VK_NULL_HANDLE;
    vk_gauge_descriptor_pools().add(1);
    return p;
}

void wait_serial_blocking(uint64_t serial) {
    if (!serial || read_completed() >= serial) return;
    NXFT("gpu.wait");
    VkSemaphoreWaitInfo wi = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    wi.semaphoreCount = 1;
    wi.pSemaphores = &g->timeline;
    wi.pValues = &serial;
    VkResult r = vkWaitSemaphores(g->dev->device(), &wi, UINT64_MAX);
    if (r != VK_SUCCESS) NXLOG_E(kTag, "vkWaitSemaphores failed (%d)", static_cast<int>(r));
    if (r == VK_ERROR_DEVICE_LOST) device_lost("waiting for the GPU");
    read_completed();
}

// GPU time of the submissions (frame trace on: two timestamps per slot): span, and span less the waits between parts (paired windows).
std::atomic<uint64_t> g_gpu_exec_ns{0}, g_gpu_busy_ns{0};

Slot& acquire_slot() {
    NXFT_MIN("slot.acquire", 20000);
    Slot& s = g->slots[g->next_slot];
    g->next_slot = (g->next_slot + 1) % kSlots;
    {
        nxrt::HitchTimer ht(nxrt::kHpRenderGpuWait);
        wait_serial_blocking(s.serial);
    }
    if (s.timed) {
        s.timed = false;
        uint64_t ts[2] = {0, 0};
        const uint32_t idx = static_cast<uint32_t>(&s - g->slots) * 2;
        if (vkGetQueryPoolResults(g->dev->device(), g->qpool, idx, 2, sizeof ts, ts, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && ts[1] >= ts[0]) {
            static const uint16_t z = ft::zone_id("gpu.exec");
            const double ns = static_cast<double>(ts[1] - ts[0]) * g->dev->properties().limits.timestampPeriod;
            ft::event(z, s.cpu_t0, s.cpu_t0 + static_cast<uint64_t>(ns), static_cast<uint32_t>(s.serial));
            {   // the GPU's own clock (ns) of the submission's first and last timestamp: the critical-path analysis maps it onto the CPU clock
                // through the timeline signals the game's waits see (no calibrated timestamps)
                static const uint16_t zr = ft::zone_id("gpu.raw");
                const double per = g->dev->properties().limits.timestampPeriod;
                ft::event(zr, static_cast<uint64_t>(static_cast<double>(ts[0]) * per), static_cast<uint64_t>(static_cast<double>(ts[1]) * per), static_cast<uint32_t>(s.serial));
            }
            g_gpu_exec_ns.fetch_add(static_cast<uint64_t>(ns), std::memory_order_relaxed);
            g_gpu_busy_ns.fetch_add(static_cast<uint64_t>(ns), std::memory_order_relaxed);   // less the waits for parts, below
            // The span above runs from the first part's start to the last part's end: with submission chunks it includes the time the GPU
            // waited for the render thread to send the next part. gpu.starve: the sum of those waits (a part that started before the one
            // before it ended counts 0); gpu.busy: the span without them.
            uint64_t ck[kCkMax];
            if (g->cpool && s.ck_n >= 2 &&
                vkGetQueryPoolResults(g->dev->device(), g->cpool, static_cast<uint32_t>(&s - g->slots) * kCkMax, s.ck_n, sizeof(uint64_t) * s.ck_n, ck,
                                      sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
                const double period = g->dev->properties().limits.timestampPeriod;
                uint64_t gap = 0;
                for (uint32_t i = 0; i + 1 < s.ck_n; i += 2)
                    if (ck[i + 1] > ck[i]) gap += ck[i + 1] - ck[i];
                const uint64_t gap_ns = static_cast<uint64_t>(static_cast<double>(gap) * period);
                static const uint16_t zs = ft::zone_id("gpu.starve"), zb = ft::zone_id("gpu.busy");
                ft::event(zs, s.cpu_t0, s.cpu_t0 + gap_ns, static_cast<uint32_t>(s.serial));
                ft::event(zb, s.cpu_t0, s.cpu_t0 + (static_cast<uint64_t>(ns) > gap_ns ? static_cast<uint64_t>(ns) - gap_ns : 0), s.ck_n / 2);
                g_gpu_busy_ns.fetch_sub(std::min<uint64_t>(gap_ns, static_cast<uint64_t>(ns)), std::memory_order_relaxed);
            }
        }
    }
    if (s.gp_n >= 2 && g->gpool) {      // GPU pass profiler: one event per interval between two timestamps, named gp:<label>, arg = draws
        static thread_local std::vector<uint64_t> ts; ts.resize(kGpMax);
        const uint32_t n = std::min(s.gp_n, kGpMax);
        const uint32_t base = static_cast<uint32_t>(&s - g->slots) * kGpMax;
        if (vkGetQueryPoolResults(g->dev->device(), g->gpool, base, n, sizeof(uint64_t) * n, ts.data(), sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            static std::map<std::string, uint16_t> zones;
            const double period = g->dev->properties().limits.timestampPeriod;
            uint64_t t = s.cpu_t0;
            for (uint32_t i = 1; i < n && i < s.gp_labels.size(); i++) {
                const uint64_t d = ts[i] >= ts[i - 1] ? static_cast<uint64_t>(static_cast<double>(ts[i] - ts[i - 1]) * period) : 0;
                auto it = zones.find(s.gp_labels[i].first);
                if (it == zones.end()) it = zones.emplace(s.gp_labels[i].first, ft::zone_id(("gp:" + s.gp_labels[i].first).c_str())).first;
                ft::event(it->second, t, t + d, s.gp_labels[i].second);
                t += d;
            }
        }
    }
    s.gp_n = 0;
    s.gp_labels.clear();
    retire();
    vkResetCommandPool(g->dev->device(), s.pool, 0);
    if (s.pool2) vkResetCommandPool(g->dev->device(), s.pool2, 0);
    s.split_used = 0;
    // [nvn] dset_recycle: the slot's submission has completed, so its sets are free: they go back to the slot's lists by layout and the next
    // submission in this slot takes them instead of allocating (the driver's vkAllocateDescriptorSets was ~0.5 us per set, ~800 sets per frame
    // in the village scene). A full reset now and then (and whenever recycling is off) returns sets of layouts no longer used.
    if (dset_recycle_now() && ++s.recycles % 512 != 0) {
        for (const auto& [l, d] : s.used) s.free_sets[l].push_back(d);
        s.used.clear();
    } else {
        for (VkDescriptorPool p : s.dpools) vkResetDescriptorPool(g->dev->device(), p, 0);
        for (Slot::Spare& sp : s.spare) sp.n = 0;   // freed with their pools
        vk_gauge_descriptor_sets().sub(static_cast<int64_t>(s.sets_live));   // the reset freed them
        s.sets_live = 0;
        s.dpool_cur = 0;
        s.used.clear();
        s.free_sets.clear();
        s.recycles = 0;
    }
    s.upload_off = 0;
    for (auto& b : s.extra) g->dev->destroy_buffer(b);
    s.extra.clear();
    return s;
}

}  // namespace

// ---- Frame ---------------------------------------------------------------------------------------------------------------
UploadAlloc Frame::upload(VkDeviceSize size, VkDeviceSize align) {
    Slot& s = *static_cast<Slot*>(slot);
    if (align == 0) align = 16;
    VkDeviceSize off = (s.upload_off + align - 1) & ~(align - 1);
    if (off + size <= kUploadBytes) {
        s.upload_off = off + size;
        UploadAlloc a;
        a.buffer = s.upload.buffer;
        a.offset = off;
        a.ptr = static_cast<uint8_t*>(s.upload.mapped) + off;
        a.size = size;
        return a;
    }
    return staging(size + align);
}

UploadAlloc Frame::staging(VkDeviceSize size) {
    Slot& s = *static_cast<Slot*>(slot);
    gfx::Buffer b;
    const VkBufferUsageFlags usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                     VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    if (g->dev->create_buffer(std::max<VkDeviceSize>(size, 256), usage, gfx::MemoryUse::Upload, &b, "nvn staging") != VK_SUCCESS) {
        NXLOG_E(kTag, "staging buffer of %llu bytes failed", static_cast<unsigned long long>(size));
        return {};
    }
    UploadAlloc a;
    a.buffer = b.buffer;
    a.offset = 0;
    a.ptr = static_cast<uint8_t*>(b.mapped);
    a.size = size;
    s.extra.push_back(b);
    return a;
}

UploadAlloc Frame::adopt_staging(gfx::Buffer b) {
    Slot& s = *static_cast<Slot*>(slot);
    UploadAlloc a;
    a.buffer = b.buffer;
    a.offset = 0;
    a.ptr = static_cast<uint8_t*>(b.mapped);
    a.size = b.size;
    s.extra.push_back(b);
    return a;
}

void Frame::defer(std::function<void()> fn) {
    g->deferred.push_back({serial, std::move(fn)});
    g_gauge_deferred.add(1);
}

VkDescriptorPool keep_dpool_new() { return new_dpool(); }
void keep_dpool_release(VkDescriptorPool p) {
    if (!p) return;
    defer_destroy([p] {
        vkDestroyDescriptorPool(g->dev->device(), p, nullptr);
        vk_gauge_descriptor_pools().sub(1);
    });
}

// [nvn] alloc_batch (default off; cpuperf_ab_what alloc_batch): descriptor sets are allocated eight at a time per layout and handed out one
// by one (one driver call instead of eight; ~1,000 sets per frame are allocated in village outside). Measured worse (village 1080, paired
// windows: render replay +0.30 ms per frame, 3 of 19 pairs better): the driver's cost is per set, and spare sets of a layout went unused.
std::atomic<bool> g_alloc_batch_ab{true};
void alloc_batch_enable(bool on) { g_alloc_batch_ab.store(on, std::memory_order_relaxed); }
void dset_recycle_enable(bool on) { g_dset_recycle_ab.store(on, std::memory_order_relaxed); }
uint64_t dset_recycled() { return g_dset_recycled; }
VkDescriptorSet alloc_set_new(Slot& s, VkDescriptorSetLayout layout);
VkDescriptorSet Frame::alloc_set(VkDescriptorSetLayout layout) {
    Slot& s = *static_cast<Slot*>(slot);
    if (!dset_recycle_now()) return alloc_set_new(s, layout);
    auto it = s.free_sets.find(layout);
    VkDescriptorSet ds = VK_NULL_HANDLE;
    if (it != s.free_sets.end() && !it->second.empty()) {
        ds = it->second.back();
        it->second.pop_back();
        ++g_dset_recycled;
    } else {
        ds = alloc_set_new(s, layout);
    }
    if (ds) s.used.emplace_back(layout, ds);
    return ds;
}
VkDescriptorSet alloc_set_new(Slot& s, VkDescriptorSetLayout layout) {
    const bool batch_cfg = NX_CFG_BOOL("nvn", "alloc_batch", false);
    if (batch_cfg && g_alloc_batch_ab.load(std::memory_order_relaxed)) {
        Slot::Spare& sp = s.spare[(reinterpret_cast<uintptr_t>(layout) >> 4) & 127];
        if (sp.layout == layout && sp.n) return sp.sets[--sp.n];
        for (int attempt = 0; attempt < 2; ++attempt) {
            if (s.dpool_cur >= s.dpools.size()) {
                VkDescriptorPool p = new_dpool();
                if (!p) return VK_NULL_HANDLE;
                s.dpools.push_back(p);
            }
            VkDescriptorSetLayout layouts[8];
            for (VkDescriptorSetLayout& l : layouts) l = layout;
            VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            ai.descriptorPool = s.dpools[s.dpool_cur];
            ai.descriptorSetCount = 8;
            ai.pSetLayouts = layouts;
            VkDescriptorSet sets[8] = {};
            if (vkAllocateDescriptorSets(g->dev->device(), &ai, sets) == VK_SUCCESS) {
                g->ctr.descriptor_sets.fetch_add(8, std::memory_order_relaxed);
                s.sets_live += 8;
                vk_gauge_descriptor_sets().add(8);
                sp.layout = layout;
                sp.n = 7;
                for (int i = 0; i < 7; ++i) sp.sets[i] = sets[i + 1];
                return sets[0];
            }
            ++s.dpool_cur;   // exhausted (or too fragmented for eight): the next pool; the single allocation below takes what is left
        }
    }
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (s.dpool_cur >= s.dpools.size()) {
            VkDescriptorPool p = new_dpool();
            if (!p) return VK_NULL_HANDLE;
            s.dpools.push_back(p);
        }
        VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = s.dpools[s.dpool_cur];
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &layout;
        VkDescriptorSet set = VK_NULL_HANDLE;
        const VkResult r = vkAllocateDescriptorSets(g->dev->device(), &ai, &set);
        if (r == VK_SUCCESS) {
            g->ctr.descriptor_sets.fetch_add(1, std::memory_order_relaxed);
            s.sets_live++;
            vk_gauge_descriptor_sets().add(1);
            return set;
        }
        ++s.dpool_cur;   // exhausted or fragmented: move on to the next pool (created on demand)
    }
    return VK_NULL_HANDLE;
}

// ---- general helpers ---------------------------------------------------------------------------------------------------
bool on_render_thread() {
    // a constant-initialized thread-local set by the render loop (std::this_thread::get_id is a system call; the held-result queries ask
    // millions of times per second)
    return g && t_render_of == g;
}
gfx::Device& device() { return *g->dev; }
VkDevice vkdev() { return g->dev->device(); }
Counters& counters() { return g->ctr; }
Counters* counters_if() { return g ? &g->ctr : nullptr; }
uint64_t completed_serial() { return g ? g->completed.load() : 0; }
uint64_t poll_completed() { return g ? read_completed() : 0; }
uint64_t current_serial() { return g ? (g->recording ? g->recording : g->submitted) : 0; }

void note_unsupported(const char* what) {
    {   // the gap count without the gap store's mutex and map per call (WaitEvent, per op): a counter per text, found by address
        struct Hot { const char* what; nxrt::GapCounter* gap; };
        thread_local std::unordered_map<const char*, Hot> cache;
        auto it = cache.find(what);
        if (it == cache.end() || std::strcmp(it->second.what, what) != 0) {
            static std::mutex mu;
            static std::unordered_map<std::string, nxrt::GapCounter*> by_text;   // counters never freed (registered)
            nxrt::GapCounter* gc;
            {
                std::lock_guard<std::mutex> lk(mu);
                auto [t, fresh] = by_text.try_emplace(what, nullptr);
                if (fresh) t->second = new nxrt::GapCounter("nvn", what);
                gc = t->second;
            }
            it = cache.insert_or_assign(what, Hot{what, gc}).first;
        }
        it->second.gap->hit();
    }
    if (!g) return;
    g->ctr.unsupported_ops.fetch_add(1);
    std::lock_guard<std::mutex> lk(g->unsupported_mu);
    auto u = g->unsupported.find(std::string_view(what));
    if (u == g->unsupported.end()) u = g->unsupported.emplace(what, 0).first;
    if (u->second++ == 0) NXLOG_W(kTag, "not implemented, skipped: %s", what);
}

void defer_destroy(std::function<void()> fn) {
    if (!g) {
        fn();
        return;
    }
    std::lock_guard<std::mutex> lk(g->dmu);
    g->pending_destroy.push_back(std::move(fn));
    g_gauge_deferred.add(1);
}

void run_on_render_thread(const std::function<void()>& fn) {
    if (!g) return;
    if (on_render_thread()) {
        fn();
        return;
    }
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    {
        std::lock_guard<std::mutex> lk(g->qmu);
        g->queue.push_back([&] {
            fn();
            std::lock_guard<std::mutex> l2(m);
            done = true;
            cv.notify_all();
        });
        g_gauge_render_queue.add(1);
    }
    g->qcv.notify_one();
    std::unique_lock<std::mutex> lk(m);
    cv.wait(lk, [&] { return done; });
}

bool gpass_on() { return g && g->gpool != VK_NULL_HANDLE; }

void gpass_stamp(Frame& f, const char* label, uint32_t arg) {
    if (!g || !g->gpool || !f.slot) return;
    Slot& s = *static_cast<Slot*>(f.slot);
    if (s.gp_n >= kGpMax) return;
    vkCmdWriteTimestamp2(f.cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, g->gpool, static_cast<uint32_t>(&s - g->slots) * kGpMax + s.gp_n);
    s.gp_labels.emplace_back(label, arg);
    s.gp_n++;
}

// ---- device-loss checkpoints ([gfx] device_diagnostics, VK_NV_device_diagnostic_checkpoints) ----------------------------------------
namespace {
// The latest checkpoint descriptions: the render thread writes them, the device-loss report reads them back by marker.
struct CpRec {
    std::atomic<uint64_t> id{0};
    char text[160];
};
constexpr uint64_t kCpRing = 1u << 16;
CpRec* g_cp = nullptr;
std::atomic<uint64_t> g_cp_next{0};
std::atomic<bool> g_lost_exit{false};

std::string describe_checkpoint(uint64_t id) {
    if (!g_cp || !id) return "(no record)";
    const CpRec& r = g_cp[id & (kCpRing - 1)];
    if (r.id.load(std::memory_order_acquire) != id) return "(record overwritten)";
    return std::string(r.text) + "  [newest recorded: " + std::to_string(g_cp_next.load()) + "]";
}
}  // namespace

bool checkpoints_on() { return g_cp != nullptr; }

void checkpoint(Frame& f, const char* text) {
    if (!g_cp || f.cb == VK_NULL_HANDLE) return;
    const uint64_t id = g_cp_next.fetch_add(1) + 1;
    CpRec& r = g_cp[id & (kCpRing - 1)];
    std::snprintf(r.text, sizeof r.text, "serial %llu present %llu: %s", static_cast<unsigned long long>(f.serial),
                  static_cast<unsigned long long>(counters().presents.load()), text);
    r.id.store(id, std::memory_order_release);
    vkCmdSetCheckpointNV(f.cb, reinterpret_cast<const void*>(static_cast<uintptr_t>(id)));
}

// A submission or a wait failed with the device lost: the driver's report once (Device::report_device_lost), then the process ends
// ([gfx] exit_on_device_lost, default on): every later submission fails and the window would stand still. The exit runs on a thread of
// its own, so the render thread (often the caller) can go on answering the waits of the game threads meanwhile.
void device_lost(const char* where) {
    if (!g) return;
    g->dev->report_device_lost(where);
    if (g_lost_exit.exchange(true) || !config_get_bool("gfx", "exit_on_device_lost", true)) return;
    std::thread([w = std::string(where)] {
        nxrt::play::exit_process(4, "device lost", "the GPU device was lost (" + w + "): the game cannot go on. The driver's report is in the log above.");
    }).detach();
}

namespace {
// [nvn] pass_barriers = tracked (S38): a layout change waits only for the stages that can have used the image in its old layout and blocks
// only the stages that use it in the new one. The layout says how the image was used: the replay keeps every image in the layout of its
// last use (attachments in the attachment layouts, sampled images in the read-only layouts, copies in the transfer layouts); GENERAL and
// anything else keep the full barrier.
std::atomic<bool> g_narrow_barriers{false};

constexpr VkPipelineStageFlags2 kShaderStages = VK_PIPELINE_STAGE_2_PRE_RASTERIZATION_SHADERS_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                                                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
constexpr VkPipelineStageFlags2 kDepthStages = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;

// Stages and accesses of an image's use in `layout`; writes_only: the access part names the writes only (the source half of a barrier).
void layout_scope(VkImageLayout layout, bool src, VkPipelineStageFlags2* stage, VkAccessFlags2* access) {
    switch (layout) {
        case VK_IMAGE_LAYOUT_UNDEFINED:
            // the old contents do not matter, but the memory may have belonged to something still in flight: wait for all earlier commands
            *stage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            *access = src ? VK_ACCESS_2_NONE : (VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
            return;
        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
            *stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            *access = src ? VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT : (VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
            return;
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
            *stage = kDepthStages;
            *access = src ? VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
                          : (VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
            return;
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
            *stage = kDepthStages | kShaderStages;
            *access = src ? VK_ACCESS_2_NONE : (VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            return;
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
            *stage = kShaderStages;
            *access = src ? VK_ACCESS_2_NONE : VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
            return;
        case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
            *stage = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
            *access = src ? VK_ACCESS_2_NONE : VK_ACCESS_2_TRANSFER_READ_BIT;
            return;
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
            *stage = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
            *access = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            return;
        default:
            *stage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            *access = src ? VK_ACCESS_2_MEMORY_WRITE_BIT : (VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
            return;
    }
}
}  // namespace

void set_narrow_barriers(bool on) { g_narrow_barriers.store(on, std::memory_order_relaxed); }

void image_barrier(VkCommandBuffer cb, VkImage image, VkImageLayout from, VkImageLayout to, VkImageAspectFlags aspect, uint32_t base_level,
                   uint32_t level_count, uint32_t base_layer, uint32_t layer_count) {
    g_gauge_barriers_img.add(1);
    VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    if (g_narrow_barriers.load(std::memory_order_relaxed)) {
        layout_scope(from, true, &b.srcStageMask, &b.srcAccessMask);
        layout_scope(to, false, &b.dstStageMask, &b.dstAccessMask);
    }
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {aspect, base_level, level_count, base_layer, layer_count};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cb, &di);
}

// Full barriers by call site (diagnosis: which of our barriers drain the GPU most often). Open addressing over (file pointer, line); any
// thread; a full table only stops counting new sites.
namespace {
std::atomic<uint64_t> g_bar_site_key[512];
std::atomic<uint64_t> g_bar_site_n[512];
void bar_site_count(const std::source_location& loc) {
    const uint64_t key = (reinterpret_cast<uintptr_t>(loc.file_name()) & 0xffffffffffffull) | (uint64_t{loc.line() & 0xffff} << 48);
    uint64_t h = (key * 0x9e3779b97f4a7c15ull) >> 55;
    for (int i = 0; i < 16; ++i, h = (h + 1) & 511) {
        uint64_t k = g_bar_site_key[h].load(std::memory_order_relaxed);
        if (k == 0 && g_bar_site_key[h].compare_exchange_strong(k, key, std::memory_order_relaxed)) k = key;
        if (k == key) { g_bar_site_n[h].fetch_add(1, std::memory_order_relaxed); return; }
    }
}
}  // namespace

std::string memory_barrier_sites(size_t top) {
    std::vector<std::pair<uint64_t, uint64_t>> v;
    for (size_t i = 0; i < 512; ++i)
        if (const uint64_t k = g_bar_site_key[i].load(std::memory_order_relaxed)) v.emplace_back(g_bar_site_n[i].load(std::memory_order_relaxed), k);
    std::sort(v.rbegin(), v.rend());
    std::string s;
    for (size_t i = 0; i < v.size() && i < top; ++i) {
        const char* f = reinterpret_cast<const char*>(v[i].second & 0xffffffffffffull);
        const char* b = f;
        for (const char* p = f; *p; ++p) if (*p == '/' || *p == '\\') b = p + 1;
        char t[96];
        std::snprintf(t, sizeof t, " %s:%llu %llu", b, (unsigned long long)(v[i].second >> 48), (unsigned long long)v[i].first);
        s += t;
    }
    return s;
}

void count_tracked_barrier() { g_gauge_barriers_mem.add(1); }
uint64_t barriers_mem_total() { return static_cast<uint64_t>(g_gauge_barriers_mem.get()); }
void gpu_time_totals(uint64_t* exec_ns, uint64_t* busy_ns) { *exec_ns = g_gpu_exec_ns.load(std::memory_order_relaxed); *busy_ns = g_gpu_busy_ns.load(std::memory_order_relaxed); }

thread_local uint64_t t_barriers = 0;
uint64_t memory_barriers_here() { return t_barriers; }

// Measurement only ([nvn] skip_fs_file words "bar" / "bar:<line>", nvn_replay.cpp): 0 keeps every full barrier, -1 drops all of them, a
// line number drops the barriers of that source line. The picture may break; it gives the GPU cost of the barriers at real clocks.
std::atomic<int> g_exp_bar_drop{0};
std::atomic<int> g_exp_bar_more[3] = {};   // further "bar:<line>" words

void memory_barrier(VkCommandBuffer cb, std::source_location loc) {
    cmdq::owe_clear();   // [nvn] dispatch_trust: a full barrier pays an owed one
    ++t_barriers;
    bar_site_count(loc);
    if (const int drop = g_exp_bar_drop.load(std::memory_order_relaxed); drop) {
        if (drop < 0 || static_cast<uint32_t>(drop) == loc.line()) return;
        for (const auto& m : g_exp_bar_more)
            if (static_cast<uint32_t>(m.load(std::memory_order_relaxed)) == loc.line()) return;
    }
    if (hazard::takes(cb)) { hazard::request(loc.line()); return; }   // [nvn] hazard: recorded only where a command needs it (nvn_hazard.cpp)
    g_gauge_barriers_mem.add(1);
    VkMemoryBarrier2 b = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;

    b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.memoryBarrierCount = 1;
    di.pMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cb, &di);
}

// ---- submission --------------------------------------------------------------------------------------------------------
namespace {

// [nvn] exp_async_cull (measurement only) and, with [nvn] exp_async_cull_ab, only in the "on" windows of [nvn] cpuperf_ab (paired windows)
bool async_cull_now() {
    if (!g || !g->tl2) return false;
    static const bool ab = config_get_bool("nvn", "exp_async_cull_ab", false);
    return !ab || ab_window_on();
}

// `serial`: the one reserved when the job was queued (queue_submit). 0: a submission made inside a render job (texture dumps of the trace):
// it takes the next serial when nothing queued holds one, else it signals nothing and the queue is drained after it.
uint64_t submit_on_render_thread(const std::function<void(Frame&)>& record, uint64_t serial = 0, bool q2_hint = false) {
    NXFT("replay");
    nxrt::HitchTimer ht_replay(nxrt::kHpRenderReplay);
    struct ReplayEv {   // frame trace: the same span with the serial (critical-path analysis: queued -> replayed -> signalled)
        uint64_t t0 = ft::on() ? ft::now_ns() : 0; uint64_t serial = 0;
        ~ReplayEv() { if (t0 && serial) { static const uint16_t z = ft::zone_id("replay.s"); ft::event(z, t0, ft::now_ns(), static_cast<uint32_t>(serial)); } }
    } rev;
    const auto t0 = std::chrono::steady_clock::now();
    Slot& s = acquire_slot();
    bool unsignalled = false;
    if (!serial) {
        std::lock_guard<std::mutex> lk(g->qmu);
        if (g->reserved.load() == g->submitted) serial = g->reserved.fetch_add(1) + 1;
        else { serial = g->submitted; unsignalled = true; }
    }
    g->recording = serial;
    rev.serial = serial;
    Frame f;
    f.cb = s.cb;
    f.serial = serial;
    f.slot = &s;
    // [nvn] exp_async_cull with a second queue of another family: the submission is recorded into the slot's command buffer of that family
    if (q2_hint && !unsignalled && g->tl2 && g->dev->queue2_family() != g->dev->queue_family()) {
        if (!s.pool2) {
            VkCommandPoolCreateInfo ci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            ci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
            ci.queueFamilyIndex = g->dev->queue2_family();
            if (vkCreateCommandPool(g->dev->device(), &ci, nullptr, &s.pool2) == VK_SUCCESS) {
                VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
                ai.commandPool = s.pool2;
                ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                ai.commandBufferCount = 1;
                if (vkAllocateCommandBuffers(g->dev->device(), &ai, &s.cb2) != VK_SUCCESS) s.cb2 = VK_NULL_HANDLE;
            }
        }
        if (s.cb2) { f.cb = s.cb2; f.q2 = true; }
    }
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(f.cb, &bi);
    const uint32_t qi = static_cast<uint32_t>(&s - g->slots) * 2;
    gstats::slot_begin(f.cb, static_cast<uint32_t>(&s - g->slots));   // [prof] gpu_stats (measurement only)
    if (g->gpool) {
        vkCmdResetQueryPool(f.cb, g->gpool, static_cast<uint32_t>(&s - g->slots) * kGpMax, kGpMax);
        s.gp_n = 0;
        s.gp_labels.clear();
        gpass_stamp(f, "(start)", 0);
    }
    if (g->qpool) {
        vkCmdResetQueryPool(f.cb, g->qpool, qi, 2);
        vkCmdWriteTimestamp(f.cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g->qpool, qi);
        s.timed = true;
        s.ck_n = 0;
        if (g->cpool) vkCmdResetQueryPool(f.cb, g->cpool, static_cast<uint32_t>(&s - g->slots) * kCkMax, kCkMax);
        s.cpu_t0 = ft::now_ns();
    }
    checkpoint(f, "submission start");
    const uint64_t draws0 = g->ctr.draws.load(std::memory_order_relaxed), disp0 = g->ctr.dispatches.load(std::memory_order_relaxed);
    record(f);
    checkpoint(f, "submission end");
    if (g->gpool) gpass_stamp(f, "tail", 0);
    // f.cb, not s.cb: frame_split / frame_chunk may have moved the recording on to another command buffer of the slot
    if (g->qpool) vkCmdWriteTimestamp(f.cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g->qpool, qi + 1);
    // [nvn] exp_async_cull (measurement only: no dependency on the first queue's work is kept, the picture may be wrong): a submission
    // without draws and in one command buffer runs on the second queue, beside whatever the first queue still runs.
    const bool drawless = g->ctr.draws.load(std::memory_order_relaxed) == draws0;
    if (f.q2 && !drawless) NXLOG_E(kTag, "exp_async_cull: serial %llu was recorded for the second queue but drew", static_cast<unsigned long long>(serial));
    if (f.q2 || (!unsignalled && async_cull_now() && g->dev->queue2_family() == g->dev->queue_family() && s.split_used == 0 && f.cb == s.cb && drawless &&
                 g->ctr.dispatches.load(std::memory_order_relaxed) - disp0 >= static_cast<uint64_t>(NX_CFG_INT("nvn", "exp_async_cull_min", 1)))) {
        vkEndCommandBuffer(f.cb);
        const uint64_t v = ++g->tl2_val;
        VkCommandBufferSubmitInfo cbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
        cbi.commandBuffer = f.cb;
        VkSemaphoreSubmitInfo sg = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        sg.semaphore = g->tl2;
        sg.value = v;
        sg.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        VkSubmitInfo2 si = {VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
        si.commandBufferInfoCount = 1;
        si.pCommandBufferInfos = &cbi;
        si.signalSemaphoreInfoCount = 1;
        si.pSignalSemaphoreInfos = &sg;
        VkResult r;
        {
            std::lock_guard<std::mutex> lk(g->dev->queue2_mutex());
            r = vkQueueSubmit2(g->dev->queue2(), 1, &si, VK_NULL_HANDLE);
        }
        VkSemaphoreSubmitInfo wt = sg;   // the join: the first queue waits for it and signals the serial
        VkSemaphoreSubmitInfo sig = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        sig.semaphore = g->timeline;
        sig.value = serial;
        sig.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        VkSubmitInfo2 sj = {VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
        sj.waitSemaphoreInfoCount = r == VK_SUCCESS ? 1 : 0;
        sj.pWaitSemaphoreInfos = &wt;
        sj.signalSemaphoreInfoCount = 1;
        sj.pSignalSemaphoreInfos = &sig;
        {
            std::lock_guard<std::mutex> lk(g->dev->queue_mutex());
            const VkResult rj = vkQueueSubmit2(g->dev->queue(), 1, &sj, VK_NULL_HANDLE);
            if (r == VK_SUCCESS) r = rj;
        }
        if (r != VK_SUCCESS) {
            NXLOG_E(kTag, "async submission: vkQueueSubmit2 failed (%d)", static_cast<int>(r));
            if (r == VK_ERROR_DEVICE_LOST) device_lost("NVN async submission");
        }
        if (g->async_n++ == 0) NXLOG_I(kTag, "exp_async_cull: serial %llu (%llu dispatches, no draws) ran on the second queue", static_cast<unsigned long long>(serial),
                                       static_cast<unsigned long long>(g->ctr.dispatches.load() - disp0));
        s.serial = serial;
        g->submitted = serial;
        g->last_submitted.store(serial);
        g->recording = 0;
        g->ctr.submits.fetch_add(1);
        g->ctr.submit_replay_ns.fetch_add(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count()));
        return serial;
    }
    if (!unsignalled && cmdq::queue_submit_end(g->dev->device(), f.cb, g->dev->queue(), &g->dev->queue_mutex(), g->timeline, serial)) {
        // [nvn] submit_async: the recorder ends and submits it in order (it signals the serial; a failed submit signals from the host)
        s.serial = serial;
        g->submitted = serial;
        g->last_submitted.store(serial);
        g->recording = 0;
        g->ctr.submits.fetch_add(1);
        g->ctr.submit_replay_ns.fetch_add(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count()));
        return serial;
    }
    vkEndCommandBuffer(f.cb);
    VkCommandBufferSubmitInfo cbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cbi.commandBuffer = f.cb;
    VkSemaphoreSubmitInfo sig = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    sig.semaphore = g->timeline;
    sig.value = serial;
    sig.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkSubmitInfo2 si = {VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cbi;
    si.signalSemaphoreInfoCount = unsignalled ? 0 : 1;
    si.pSignalSemaphoreInfos = &sig;
    VkResult r;
    {
        nxrt::HitchTimer ht(nxrt::kHpRenderSubmit);
        const uint64_t tl = nxrt::hitch_ticks();
        std::lock_guard<std::mutex> lk(g->dev->queue_mutex());
        nxrt::hitch_phase_add(nxrt::kHpRenderLockWait, nxrt::hitch_ticks() - tl);
        r = vkQueueSubmit2(g->dev->queue(), 1, &si, VK_NULL_HANDLE);
        if (unsignalled && r == VK_SUCCESS) vkQueueWaitIdle(g->dev->queue());   // debug path only: nothing else tells when it is done
    }
    if (unsignalled) {
        g->recording = 0;
        s.serial = 0;
        return serial;
    }
    if (r != VK_SUCCESS) {
        NXLOG_E(kTag, "vkQueueSubmit2 failed (%d): the device is lost or out of memory", static_cast<int>(r));
        if (r == VK_ERROR_DEVICE_LOST) device_lost("NVN submission");
        // keep the timeline moving so waiters do not hang: signal from the host
        VkSemaphoreSignalInfo ssi = {VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
        ssi.semaphore = g->timeline;
        ssi.value = serial;
        vkSignalSemaphore(g->dev->device(), &ssi);
    }
    s.serial = serial;
    g->submitted = serial;
    g->last_submitted.store(serial);
    g->recording = 0;
    g->ctr.submits.fetch_add(1);
    g->ctr.submit_replay_ns.fetch_add(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count()));
    return serial;
}

}  // namespace

// Ends the command buffer of the submission being recorded, runs what is recorded so far and waits for the GPU to finish it (and every earlier
// submission), then goes on recording the same submission (same serial) in a fresh command buffer of the slot. Render thread only. The caller has
// ended its render pass and queries, and must treat everything bound in the old command buffer as unbound.
bool frame_split(Frame& f) {
    if (!g || !on_render_thread() || !f.slot || f.cb == VK_NULL_HANDLE || f.q2) return false;
    Slot& s = *static_cast<Slot*>(f.slot);
    VkDevice dev = g->dev->device();
    if (s.split_used == s.split_cbs.size()) {
        VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = s.pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VkCommandBuffer nb = VK_NULL_HANDLE;
        if (vkAllocateCommandBuffers(dev, &ai, &nb) != VK_SUCCESS) return false;
        s.split_cbs.push_back(nb);
    }
    VkCommandBuffer next = s.split_cbs[s.split_used++];
    VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence = VK_NULL_HANDLE;
    if (vkCreateFence(dev, &fi, nullptr, &fence) != VK_SUCCESS) return false;
    vkEndCommandBuffer(f.cb);
    VkCommandBufferSubmitInfo cbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cbi.commandBuffer = f.cb;
    VkSubmitInfo2 si = {VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cbi;   // no timeline signal: the serial belongs to the end of the submission
    VkResult r;
    {
        std::lock_guard<std::mutex> lk(g->dev->queue_mutex());
        r = vkQueueSubmit2(g->dev->queue(), 1, &si, fence);
    }
    if (r == VK_SUCCESS) {
        if (vkWaitForFences(dev, 1, &fence, VK_TRUE, 10ull * 1000 * 1000 * 1000) != VK_SUCCESS) NXLOG_W(kTag, "frame split: the GPU did not finish within 10 s");
        wait_serial_blocking(g->submitted);   // earlier submissions too: the data the caller reads may come from any of them
    } else {
        NXLOG_E(kTag, "frame split: vkQueueSubmit2 failed (%d)", static_cast<int>(r));
        if (r == VK_ERROR_DEVICE_LOST) device_lost("NVN frame split");
    }
    vkDestroyFence(dev, fence, nullptr);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(next, &bi);
    hazard::switch_cb(f.cb, next);
    f.cb = next;
    g->ctr.submits.fetch_add(1);
    return r == VK_SUCCESS;
}

// [nvn] submit_chunk_draws: the part of the submission recorded so far goes to the GPU now, without waiting for it and without a timeline
// signal, and the recording goes on in a fresh command buffer of the slot (same serial). The GPU then runs a long submission while the rest
// of it is still being recorded. The serial's signal at the end of the last part covers every earlier part (a signal operation's first
// synchronization scope is all work submitted before it on the queue). Render thread only; the caller has ended its render pass and its
// queries and treats everything bound in the old command buffer as unbound.
bool frame_chunk(Frame& f) {
    if (!g || !f.slot || f.cb == VK_NULL_HANDLE || f.q2) return false;
    NXFT("replay.chunk");
    Slot& s = *static_cast<Slot*>(f.slot);
    VkDevice dev = g->dev->device();
    if (s.split_used == s.split_cbs.size()) {
        VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = s.pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VkCommandBuffer nb = VK_NULL_HANDLE;
        if (vkAllocateCommandBuffers(dev, &ai, &nb) != VK_SUCCESS) return false;
        vk_gauge_command_buffers().add(1);
        s.split_cbs.push_back(nb);
    }
    VkCommandBuffer next = s.split_cbs[s.split_used++];
    const bool ck = g->cpool && s.timed && s.ck_n + 2 <= kCkMax;   // frame trace: when this part ends and the next begins on the GPU
    const uint32_t ck_base = static_cast<uint32_t>(&s - g->slots) * kCkMax;
    if (ck) vkCmdWriteTimestamp(f.cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g->cpool, ck_base + s.ck_n++);
    VkResult r = VK_SUCCESS;
    if (!cmdq::queue_chunk(f.cb, next, g->dev->queue(), &g->dev->queue_mutex())) {   // [nvn] chunk_async: on the recorder thread, in order
        vkEndCommandBuffer(f.cb);
        VkCommandBufferSubmitInfo cbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
        cbi.commandBuffer = f.cb;
        VkSubmitInfo2 si = {VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
        si.commandBufferInfoCount = 1;
        si.pCommandBufferInfos = &cbi;
        {
            nxrt::HitchTimer ht(nxrt::kHpRenderSubmit);
            std::lock_guard<std::mutex> lk(g->dev->queue_mutex());
            r = vkQueueSubmit2(g->dev->queue(), 1, &si, VK_NULL_HANDLE);
        }
        if (r != VK_SUCCESS) {
            NXLOG_E(kTag, "submission chunk: vkQueueSubmit2 failed (%d)", static_cast<int>(r));
            if (r == VK_ERROR_DEVICE_LOST) device_lost("NVN submission chunk");
        }
        VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(next, &bi);
    }
    hazard::switch_cb(f.cb, next);
    if (ck) vkCmdWriteTimestamp(next, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g->cpool, ck_base + s.ck_n++);
    f.cb = next;
    g->ctr.submit_chunks.fetch_add(1, std::memory_order_relaxed);
    return r == VK_SUCCESS;
}

// Queues a submission with the next serial (in queue order). wait: until the render thread has run it (and, with gpu_done, the GPU too).
uint64_t queue_submit(std::function<void(Frame&)> record, bool wait, bool gpu_done, bool q2_hint = false) {
    if (on_render_thread()) {
        const uint64_t serial = submit_on_render_thread(record);
        if (gpu_done) { wait_serial_blocking(serial); retire(); }
        return serial;
    }
    struct Done { std::mutex m; std::condition_variable cv; bool done = false; };
    auto done = wait ? std::make_shared<Done>() : nullptr;
    uint64_t serial = 0;
    {
        std::lock_guard<std::mutex> lk(g->qmu);
        serial = g->reserved.fetch_add(1) + 1;
        g->queue.push_back([record = std::move(record), serial, done, gpu_done, q2_hint] {
            submit_on_render_thread(record, serial, q2_hint);
            if (gpu_done) { wait_serial_blocking(serial); retire(); }
            if (done) {
                std::lock_guard<std::mutex> l2(done->m);
                done->done = true;
                done->cv.notify_all();
            }
        });
        g_gauge_render_queue.add(1);
    }
    g->qcv.notify_one();
    if (done) {
        std::unique_lock<std::mutex> lk(done->m);
        done->cv.wait(lk, [&] { return done->done; });
    }
    return serial;
}

uint64_t run_frame_sync(const std::function<void(Frame&)>& fn) {
    return queue_submit(fn, true, true);
}

// ---- gpu.h ---------------------------------------------------------------------------------------------------------------
bool active() { return g != nullptr; }
void cpu_will_read(uint64_t addr, uint64_t size) {
    if (g && out_any()) out_fetch(addr, size, false);
}
bool debug_labels() { return g && g->dev && g->dev->features().debug_labels; }

namespace {
std::atomic<uint64_t> g_idle_serial{0};   // without a back end submissions still get serials and complete at once (tests, tools)
}

// [nvn] async_submit (default on): the game thread queues its command lists and goes on; the render thread records and submits them in
// order. The serial is known at once (reserved in queue order), so sync objects and waits work as before: a wait for a serial the render
// thread has not submitted yet waits on the timeline until it has (wait-before-signal).
bool async_submit() {
    const bool on = NX_CFG_BOOL("nvn", "async_submit", true);
    return on;
}

uint64_t submit(const std::vector<Ref<CmdList>>& lists) {
    if (!g) return ++g_idle_serial;
    NXFT("submit");
    bool recorded = false;
    const bool snap = capture::on_submit(lists, &recorded);   // the first submission of a frame being captured: the GPU state it starts from
    std::shared_ptr<FrontPlan> plan = async_submit() && !on_render_thread() ? front_submit(lists) : nullptr;   // [nvn] front_plan
    bool q2_hint = false;   // [nvn] exp_async_cull with a compute-family second queue: decided before recording
    if (async_cull_now() && g->dev->queue2_family() != g->dev->queue_family() && async_submit() && !on_render_thread() && !snap && !recorded) {
        uint32_t nd = 0;
        int bad = -1;
        q2_hint = submission_compute_only(lists, &nd, &bad) && nd >= static_cast<uint32_t>(NX_CFG_INT("nvn", "exp_async_cull_min", 1));
        static std::atomic<int> logged{0};
        if (nd >= 8 && !q2_hint && logged.fetch_add(1) < 8)
            NXLOG_I(kTag, "exp_async_cull: a submission with %u dispatches stays on the first queue (op %d)", nd, bad);
    }
    const uint64_t serial = queue_submit([lists, snap, recorded, plan](Frame& f) {
        if (recorded) capture::frame_submit_begin(snap);
        if (snap) capture::state_taken(replay_state_snapshot());
        replay_begin_submit(f);
        front_replay_begin(plan);
        replay_lists(f, lists);
        front_replay_end(plan);
        replay_end_submit(f);
        if (recorded) capture::frame_submit_end();
    }, !async_submit(), false, q2_hint);
    if (async_submit() && !on_render_thread()) texlook_submit(lists, serial);   // textures the coming draws use: prepared in the background meanwhile
    if (ft::on()) ft::mark("q.sub", static_cast<uint32_t>(serial));   // frame trace: when the game queued this serial (critical-path analysis)
    return serial;
}

uint64_t flush() { return g ? g->reserved.load() : g_idle_serial.load(); }
uint64_t last_serial() { return g ? g->reserved.load() : g_idle_serial.load(); }

void finish() {
    if (!g) return;
    run_on_render_thread([] {
        wait_serial_blocking(g->submitted);
        retire();
    });
}
void finish_on_render_thread() {
    if (!g) return;
    wait_serial_blocking(g->submitted);
    retire();
}

bool serial_done(uint64_t serial) {
    if (!g || !serial) return true;
    if (g->completed.load() >= serial) return true;
    return read_completed() >= serial;
}

namespace {
std::atomic<uint64_t> g_waits_begun{0}, g_waits_done{0};   // sync waits entered and left: a live graphics thread moves both
std::atomic<int> g_waiters{0};                 // game threads inside wait_serial (freeze report)
std::atomic<uint64_t> g_wait_serial{0}, g_wait_timeout{0};   // the latest wait: serial and timeout

std::string gpu_report() {
    if (!g) return {};
    uint64_t v = 0;
    if (g->timeline) vkGetSemaphoreCounterValue(g->dev->device(), g->timeline, &v);
    char b[400];
    size_t q = 0;
    bool got = false;
    if (g->qmu.try_lock()) { q = g->queue.size(); got = true; g->qmu.unlock(); }
    std::snprintf(b, sizeof b, "NVN back end: last submitted serial %llu, timeline value %llu, %d game thread(s) in a sync wait (latest wait: serial %llu, timeout %llu ns), sync waits begun %llu done %llu, submits %llu, presents %llu, render queue %s%zu\n",
                  static_cast<unsigned long long>(g->last_submitted.load()), static_cast<unsigned long long>(v), g_waiters.load(),
                  static_cast<unsigned long long>(g_wait_serial.load()), static_cast<unsigned long long>(g_wait_timeout.load()),
                  static_cast<unsigned long long>(g_waits_begun.load()), static_cast<unsigned long long>(g_waits_done.load()),
                  static_cast<unsigned long long>(g->ctr.submits.load()), static_cast<unsigned long long>(g->ctr.presents.load()), got ? "" : "(busy) ", q);
    return b;
}
}  // namespace

bool wait_serial(uint64_t serial, uint64_t timeout_ns) {
    if (!g || !serial) return true;
    if (serial_done(serial)) return true;
    if (timeout_ns == 0) return false;
    NXFT("sync.wait");
    nxrt::HitchTimer ht(nxrt::kHpGameSyncWait);
    struct WaitEv {   // frame trace: the wait with its serial
        uint64_t t0 = ft::on() ? ft::now_ns() : 0; uint64_t serial;
        ~WaitEv() { if (t0) { static const uint16_t z = ft::zone_id("sync.w"); ft::event(z, t0, ft::now_ns(), static_cast<uint32_t>(serial)); } }
    } wev{ft::on() ? ft::now_ns() : 0, serial};
    g_wait_serial.store(serial);
    g_wait_timeout.store(timeout_ns);
    struct Counted { Counted() { g_waiters++; g_waits_begun++; } ~Counted() { g_waiters--; g_waits_done++; } } counted;
    VkSemaphoreWaitInfo wi = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    wi.semaphoreCount = 1;
    wi.pSemaphores = &g->timeline;
    wi.pValues = &serial;
    const VkResult r = vkWaitSemaphores(g->dev->device(), &wi, timeout_ns);
    read_completed();
    return r == VK_SUCCESS;
}

namespace {
// The last serial queued before each of the game's presents (the end of that frame's GPU work), for the low-latency GPU wait (lowlat.h).
// Written by the game thread in present(), read by the main thread at the frame boundary (the same thread in title A).
std::atomic<uint64_t> g_frame_work[4];
std::atomic<uint64_t> g_frame_count{0};

bool gpu_wait_frames(int frames_back, uint64_t timeout_ns) {
    const uint64_t n = g_frame_count.load(std::memory_order_acquire);
    if (!g || n <= static_cast<uint64_t>(frames_back)) return true;
    const uint64_t serial = g_frame_work[(n - 1 - static_cast<uint64_t>(frames_back)) & 3].load(std::memory_order_relaxed);
    NXFT("lowlat.gpu_wait");
    return wait_serial(serial, timeout_ns);
}
}  // namespace

// [nvn] present_ahead (default 1; 0 = wait for each present): the game thread queues the present and goes on while fewer than this many
// presents are still pending on the render thread (the frame limiter and the swapchain's frame slots pace the render thread). 1 keeps the
// render thread one present behind the game at most (2 let a second finished frame wait in the queue: one more frame between input and screen).
void present(Window* window, int index) {
    if (!g || !window) return;
    capture::on_present(window, index);
    NXFT("present.call");
    nxrt::hitch_note_game_present();
    {
        const uint64_t n = g_frame_count.load(std::memory_order_relaxed);
        g_frame_work[n & 3].store(g->reserved.load(), std::memory_order_relaxed);
        g_frame_count.store(n + 1, std::memory_order_release);
    }
    static const int ahead = static_cast<int>(std::clamp<int64_t>(nxrt::config_get_int("nvn", "present_ahead", 1), 0, 3));
    if (ahead == 0 || on_render_thread()) {
        Ref<Window> keep = Ref<Window>::share(window);
        run_on_render_thread([&] { present_on_render_thread(window, index); });
        return;
    }
    {
        NXFT("present.backpressure");
        nxrt::HitchTimer ht(nxrt::kHpGameBackpressure);
        std::unique_lock<std::mutex> lk(g->pmu);
        g->pcv.wait(lk, [] { return g->presents_pending < ahead; });
        ++g->presents_pending;
    }
    Ref<Window> keep = Ref<Window>::share(window);
    {
        std::lock_guard<std::mutex> lk(g->qmu);
        g->queue.push_back([keep, index] {
            present_on_render_thread(keep.get(), index);
            std::lock_guard<std::mutex> l2(g->pmu);
            --g->presents_pending;
            g->pcv.notify_all();
        });
        g_gauge_render_queue.add(1);
    }
    g->qcv.notify_one();
}

void destroy_texture_gpu(TexGpu* t) {
    if (!t) return;
    if (!g) { tex_destroy(t); return; }
    defer_destroy([t] { tex_destroy(t); });
}

void release_texture_gpu(Texture* t) {
    if (!g || !t) return;
    Ref<Texture> keep = Ref<Texture>::share(t);
    {
        std::lock_guard<std::mutex> lk(g->qmu);
        if (g->stop) return;
        g->queue.push_back([keep] {
            Texture* x = keep.get();
            if (!x->destroyed.load() || !x->gpu) return;   // initialized again meanwhile, or never got an image
            TexGpu* img = x->gpu;
            x->gpu = nullptr;
            g_tex_image_gen.fetch_add(1, std::memory_order_acq_rel);
            if (tex_release_shared(img)) return;          // other objects still share the image: the last one parks it
            if (!tex_park(*x, img)) destroy_texture_gpu(img);
        });
        g_gauge_render_queue.add(1);
    }
    g->qcv.notify_one();
}

void destroy_pool_gpu(PoolGpu* p) {
    if (!p) return;
    if (!g) { pool_gpu_destroy(p); return; }
    defer_destroy([p] { pool_gpu_destroy(p); });
}

namespace {
std::atomic<int> g_origin{NVN_WINDOW_ORIGIN_MODE_LOWER_LEFT}, g_depth_mode{NVN_DEPTH_MODE_NEAR_IS_MINUS_W};
}
void set_window_modes(int origin_mode, int depth_mode) { g_origin.store(origin_mode); g_depth_mode.store(depth_mode); }
int window_origin_mode() { return g_origin.load(); }
int depth_mode() { return g_depth_mode.load(); }
gfx::Renderer* renderer() { return g ? g->rend : nullptr; }
FrameLimiter* limiter() { return g ? g->limiter : nullptr; }
PresentPass* present_pass() { return g && g->present_ok ? &g->present : nullptr; }
extras::OutputPass* output_pass() { return g && g->output_ok ? &g->output : nullptr; }
bool keep_aspect() { return g && g->keep_aspect; }

uint64_t timestamp_ns() {
    static const auto t0 = std::chrono::steady_clock::now();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count()) + 1000000000ull;
}

std::string unsupported_report() {
    std::string out;
    if (!g) return out;
    std::lock_guard<std::mutex> lk(g->unsupported_mu);
    for (auto& [name, n] : g->unsupported) out += name + ": " + std::to_string(n) + "\n";
    return out;
}

// ---- lifecycle -----------------------------------------------------------------------------------------------------------
namespace {

void register_counter_gauges();

bool bring_up(gfx::Device& dev, gfx::Renderer* rend, FrameLimiter* limiter, bool keep_aspect) {
    std::lock_guard<std::mutex> lk(g_install_mu);
    if (g) return g->dev == &dev;
    auto* gp = new Gpu();
    gp->dev = &dev;
    gp->rend = rend;
    gp->limiter = limiter;
    gp->keep_aspect = keep_aspect;
    g = gp;
    if (dev.features().diagnostic_checkpoints && !g_cp) {
        g_cp = new CpRec[kCpRing];
        dev.set_checkpoint_describer(describe_checkpoint);
        NXLOG_I(kTag, "device-loss checkpoints on: a marker before every draw and dispatch ([gfx] device_diagnostics)");
    }
    if (!dev.features().timeline_semaphore) {
        NXLOG_E(kTag, "the GPU has no timeline semaphores: the NVN back end cannot run");
        g = nullptr;
        delete gp;
        return false;
    }
    VkSemaphoreTypeCreateInfo tci = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    tci.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo sci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &tci};
    if (vkCreateSemaphore(dev.device(), &sci, nullptr, &gp->timeline) != VK_SUCCESS) {
        g = nullptr;
        delete gp;
        return false;
    }
    vk_gauge_semaphores().add(1);
    if (dev.queue2() && config_get_bool("nvn", "exp_async_cull", false)) {
        if (vkCreateSemaphore(dev.device(), &sci, nullptr, &gp->tl2) == VK_SUCCESS) {
            vk_gauge_semaphores().add(1);
            NXLOG_W(kTag, "exp_async_cull: submissions without draws run on the second queue (measurement only: no dependencies kept)");
        }
    }
    for (Slot& s : gp->slots)
        if (!create_slot(s)) {
            NXLOG_E(kTag, "creating a submission slot failed");
            g = nullptr;
            delete gp;
            return false;
        }
    if (ft::on()) {
        VkQueryPoolCreateInfo qi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = kSlots * 2;
        if (vkCreateQueryPool(dev.device(), &qi, nullptr, &gp->qpool) != VK_SUCCESS) gp->qpool = VK_NULL_HANDLE;
        else vk_gauge_query_pools().add(1);
        qi.queryCount = kSlots * kCkMax;
        if (vkCreateQueryPool(dev.device(), &qi, nullptr, &gp->cpool) != VK_SUCCESS) gp->cpool = VK_NULL_HANDLE;
        else vk_gauge_query_pools().add(1);
    }
    if (ft::on() && config_get_bool("prof", "gpu_passes", false)) {
        VkQueryPoolCreateInfo qi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = kSlots * kGpMax;
        if (vkCreateQueryPool(dev.device(), &qi, nullptr, &gp->gpool) != VK_SUCCESS) gp->gpool = VK_NULL_HANDLE;
        else vk_gauge_query_pools().add(1);
    }
    if (ft::on()) extras::lowlat_set_backlog_fn([]() -> uint64_t {      // latency probe: submissions the GPU has not finished
        if (!g || !g->timeline) return 0;
        uint64_t v = 0;
        vkGetSemaphoreCounterValue(g->dev->device(), g->timeline, &v);
        const uint64_t sub = g->last_submitted.load();
        return sub > v ? sub - v : 0;
    });
    extras::lowlat_set_gpu_wait_fn(&gpu_wait_frames);
    static const bool report_registered = (diag_add_report(gpu_report), true);   // the freeze dump shows the GPU state
    (void)report_registered;
    register_counter_gauges();
    gp->th = std::thread(render_loop);
    // wait for the thread id to be published
    for (int i = 0; i < 1000 && gp->tid == std::thread::id(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (rend) {
        run_on_render_thread([&] { gp->present_ok = gp->present.init(dev, rend->format()); });
        if (!gp->present_ok) NXLOG_E(kTag, "present pass failed to initialise: frames will not be shown");
        run_on_render_thread([&] { gp->output_ok = gp->output.init(dev); });
        if (!gp->output_ok) NXLOG_W(kTag, "output pass (HDR, FSR 1) failed to initialise: those extras are off");
    }
    NXLOG_I(kTag, "NVN back end up on %s (%u submission slots, timeline semaphore, host-memory import %s)", dev.properties().deviceName, kSlots,
            dev.features().external_memory_host ? "available" : "NOT available");
    return true;
}

const char* const kCounterGauges[] = {"nvn.cnt.draws", "nvn.cnt.passes", "nvn.cnt.pass_breaks", "nvn.cnt.dispatches", "nvn.cnt.clears", "nvn.cnt.copies",
                                      "nvn.cnt.submits", "nvn.cnt.descriptor_sets"};

void register_counter_gauges() {
    auto reg = [](const char* name, std::atomic<uint64_t> Counters::*m) {
        nxrt::gauge_register(name, [m]() -> int64_t { Gpu* gp = g; return gp ? static_cast<int64_t>((gp->ctr.*m).load(std::memory_order_relaxed)) : 0; });
    };
    reg("nvn.cnt.draws", &Counters::draws);
    reg("nvn.cnt.passes", &Counters::passes);
    reg("nvn.cnt.pass_breaks", &Counters::pass_breaks);
    reg("nvn.cnt.dispatches", &Counters::dispatches);
    reg("nvn.cnt.clears", &Counters::clears);
    reg("nvn.cnt.copies", &Counters::copies);
    reg("nvn.cnt.submits", &Counters::submits);
    reg("nvn.cnt.descriptor_sets", &Counters::descriptor_sets);
    // What the hitch log (nxrt/gauge.h) shows for a slow frame: growth of these during the frame, read lock-free on the presenting thread.
    static const bool hitch_registered = [] {
        nxrt::hitch_add_counter("pipelines_created", []() -> uint64_t { Gpu* gp = g; return gp ? gp->ctr.pipelines_created.load(std::memory_order_relaxed) : 0; });
        nxrt::hitch_add_counter("texture_uploads", []() -> uint64_t { Gpu* gp = g; return gp ? gp->ctr.texture_uploads.load(std::memory_order_relaxed) : 0; });
        nxrt::hitch_add_counter("texture_upload_kb", []() -> uint64_t { Gpu* gp = g; return gp ? gp->ctr.texture_upload_bytes.load(std::memory_order_relaxed) >> 10 : 0; });
        nxrt::hitch_add_counter("bc7_transcodes", []() -> uint64_t { Gpu* gp = g; return gp ? gp->ctr.bc7_transcodes.load(std::memory_order_relaxed) : 0; });
        nxrt::hitch_add_csv_counter("draws", []() -> uint64_t { Gpu* gp = g; return gp ? gp->ctr.draws.load(std::memory_order_relaxed) : 0; });
        nxrt::hitch_add_csv_counter("draws_skipped", []() -> uint64_t { Gpu* gp = g; return gp ? gp->ctr.draws_skipped.load(std::memory_order_relaxed) : 0; });
        nxrt::hitch_add_csv_counter("submits", []() -> uint64_t { Gpu* gp = g; return gp ? gp->ctr.submits.load(std::memory_order_relaxed) : 0; });
        nxrt::hitch_add_csv_counter("textures_created", []() -> uint64_t { Gpu* gp = g; return gp ? gp->ctr.textures_created.load(std::memory_order_relaxed) : 0; });
        nxrt::hitch_add_level("pipeline_compiles_in_flight", []() -> int64_t { return compiles_in_flight().load(std::memory_order_relaxed); });
        return true;
    }();
    (void)hitch_registered;
}

void tear_down() {
    std::lock_guard<std::mutex> lk(g_install_mu);
    if (!g) return;
    for (const char* n : kCounterGauges) nxrt::gauge_unregister(n);
    Gpu* gp = g;
    run_on_render_thread([&] {
        wait_serial_blocking(gp->submitted);
        retire();
        replay_shutdown();
        tex_shutdown();
        pool_shutdown();
        while (!gp->deferred.empty()) {
            auto fn = std::move(gp->deferred.front().fn);
            gp->deferred.pop_front();
            g_gauge_deferred.sub(1);
            fn();
        }
        drain_pending_destroy();
        for (auto& d : gp->deferred) { d.fn(); g_gauge_deferred.sub(1); }
        gp->deferred.clear();
        gp->present.shutdown();
        gp->output.shutdown();
    });
    {
        std::lock_guard<std::mutex> ql(gp->qmu);
        gp->stop = true;
    }
    gp->qcv.notify_all();
    if (gp->th.joinable()) gp->th.join();
    gfx::Device& d = *gp->dev;
    vkDeviceWaitIdle(d.device());
    for (Slot& s : gp->slots) {
        for (auto& b : s.extra) d.destroy_buffer(b);
        vk_gauge_descriptor_sets().sub(static_cast<int64_t>(s.sets_live));
        s.sets_live = 0;
        for (VkDescriptorPool p : s.dpools) {
            vkDestroyDescriptorPool(d.device(), p, nullptr);
            vk_gauge_descriptor_pools().sub(1);
        }
        d.destroy_buffer(s.upload);
        if (s.pool) {
            vkDestroyCommandPool(d.device(), s.pool, nullptr);
            vk_gauge_command_pools().sub(1);
            if (s.cb) vk_gauge_command_buffers().sub(1);
        }
        if (s.pool2) vkDestroyCommandPool(d.device(), s.pool2, nullptr);
    }
    if (gp->qpool) { vkDestroyQueryPool(d.device(), gp->qpool, nullptr); vk_gauge_query_pools().sub(1); }
    if (gp->cpool) { vkDestroyQueryPool(d.device(), gp->cpool, nullptr); vk_gauge_query_pools().sub(1); }
    if (gp->gpool) { vkDestroyQueryPool(d.device(), gp->gpool, nullptr); vk_gauge_query_pools().sub(1); }
    if (gp->timeline) { vkDestroySemaphore(d.device(), gp->timeline, nullptr); vk_gauge_semaphores().sub(1); }
    if (gp->tl2) { vkDestroySemaphore(d.device(), gp->tl2, nullptr); vk_gauge_semaphores().sub(1); }
    g = nullptr;
    delete gp;
}

}  // namespace

bool init_for_test(gfx::Device& dev, gfx::Renderer* renderer) { return bring_up(dev, renderer, nullptr, false); }
void shutdown_for_test() { tear_down(); }

}  // namespace nxrt::nvn::be::gpu

namespace nxrt::nvn {

bool install_backend(const BackendDesc& desc) {
    if (!desc.device) return false;
    if (!be::gpu::bring_up(*desc.device, desc.renderer, desc.limiter, desc.keep_aspect)) return false;
    if (desc.register_entry_points) {
        install();
        register_all_impls();
        NXLOG_I("nvn", "NVN back end installed: %zu of %zu entry points implemented", implemented_count(), entry_count());
    }
    return true;
}

void shutdown_backend() { be::gpu::tear_down(); }

bool prepare_shaders(const ShaderPrepareOptions& o, ShaderPrepareResult* r) {
    ShaderPrepareResult local;
    ShaderPrepareResult& out = r ? *r : local;
    if (!be::gpu::active()) { out = ShaderPrepareResult{}; out.error = "the graphics back end is not running"; return false; }
    return be::gpu::gpl_prepare(o, &out);
}
bool backend_installed() { return be::gpu::active(); }
void backend_flush_caches() {
    if (be::gpu::active()) be::gpu::flush_caches();
}

void backend_trace_next_frame() {
    if (be::gpu::active()) be::gpu::replay_request_trace();
}

BackendStats backend_stats() {
    BackendStats s;
    if (!be::gpu::active()) return s;
    auto& c = be::gpu::counters();
    s.submits = c.submits; s.lists_replayed = c.lists_replayed; s.draws = c.draws; s.draws_skipped = c.draws_skipped;
    s.dispatches = c.dispatches; s.clears = c.clears; s.copies = c.copies; s.presents = c.presents;
    s.passes = c.passes; s.pass_breaks = c.pass_breaks; s.pipelines_created = c.pipelines_created;
    s.pipeline_cache_hits = c.pipeline_cache_hits; s.descriptor_sets = c.descriptor_sets;
    s.textures_created = c.textures_created; s.texture_uploads = c.texture_uploads; s.texture_upload_bytes = c.texture_upload_bytes;
    s.bc7_transcodes = c.bc7_transcodes; s.textures_failed = c.textures_failed; s.pools_imported = c.pools_imported;
    s.unresolved_addresses = c.unresolved_addresses; s.missing_handles = be::handles_missing(); s.unsupported_ops = c.unsupported_ops;
    s.submit_replay_ns = c.submit_replay_ns;
    return s;
}

std::string backend_unsupported_report() { return be::gpu::unsupported_report(); }

}  // namespace nxrt::nvn

// CPU time of the calling thread in microseconds (kernel + user), for the paired A/B windows ([nvn] cpuperf_ab). Last in the file: windows.h
// would otherwise define min/max over the code above.
#include <windows.h>
namespace nxrt::nvn::be::gpu {
uint64_t thread_cpu_us() {
    FILETIME c{}, e{}, k{}, u{};
    if (!GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u)) return 0;
    auto v = [](const FILETIME& f) { return (static_cast<uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
    return (v(k) + v(u)) / 10;
}
}  // namespace nxrt::nvn::be::gpu
