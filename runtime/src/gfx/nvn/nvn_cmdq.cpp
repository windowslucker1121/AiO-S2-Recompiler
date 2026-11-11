// SPDX-License-Identifier: GPL-3.0-or-later
// [nvn] cmd_thread: the render thread's vkCmd* calls are recorded by a second thread.
//
// The replay decides what to record (resolution, coherence, descriptor sets) and then makes ~15-18k vkCmd* calls per frame, about 3-4 ms of
// driver work on the render thread (E4). With this on, the volk function pointers of the vkCmd* calls are replaced by hooks: on the render
// thread a hook copies the call's arguments (and the arrays they point to) into a single-producer ring, and the recorder thread makes the
// driver call in the same order. Calls whose arguments the hooks do not copy, and every call that must see the command buffer finished or
// that touches its command pool (begin, end, submit, reset, allocate, free, wait idle), first wait until the recorder has made every queued
// call ("drain") and then run on the render thread as before. Other threads always call the driver directly.
//
// What stays correct: the recorder makes the calls in the order the render thread issued them, into the same command buffers, before any of
// those command buffers is ended or submitted. Descriptor set updates (vkUpdateDescriptorSets) still run on the render thread at once, i.e.
// before the recorder records a bind of that set, as Vulkan requires. Objects the queued calls name are only destroyed after the GPU has
// finished with them (deferred destruction), so never before the recorder has recorded the call.
#include "nvn_cmdq.h"

#include <windows.h>
#include <intrin.h>
#pragma comment(lib, "Synchronization.lib")   // WaitOnAddress / WakeByAddressSingle

#include <algorithm>
#include <atomic>
#include <mutex>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>

#include <volk.h>

#include "nvn_gpu_internal.h"
#include "nvn_hazard.h"
#include "nxrt/config.h"
#include "nxrt/diag.h"
#include "nxrt/log.h"

namespace nxrt::nvn::be::cmdq {
namespace {

constexpr const char* kTag = "nvn.cmdq";

// ---- the ring (one producer: the render thread; one consumer: the recorder) --------------------------------------------------------
constexpr size_t kRingBytes = 16u << 20;
using ExecFn = void (*)(const uint8_t* payload);
struct PacketHdr { ExecFn fn; uint32_t size; uint32_t pad; };   // size: of the whole packet, 16-byte multiple; fn null: wrap to 0
alignas(64) uint8_t* g_ring = nullptr;
alignas(64) std::atomic<uint64_t> g_head{0};    // bytes written (producer), monotonic
alignas(64) std::atomic<uint64_t> g_tail{0};    // bytes consumed (recorder), monotonic
alignas(64) std::atomic<uint32_t> g_sleeping{0};
std::atomic<bool> g_stop{false};
std::thread g_thread;
std::atomic<bool> g_active{false};              // hooks queue the render thread's calls (set at submission begin, when the ring is empty)
uint64_t g_packets = 0, g_drains = 0, g_drain_spins = 0, g_direct = 0;   // statistics (render thread)
struct DirectName { const char* name; const uint64_t* n; };
DirectName g_direct_names[64];
int g_ndirect = 0;

thread_local int tl_render = -1;
inline bool queue_here() {
    if (!g_active.load(std::memory_order_relaxed)) return false;
    if (tl_render < 0) tl_render = gpu::on_render_thread() ? 1 : 0;
    return tl_render == 1;
}

// [nvn] hazard on the recorder: the look-ahead through a render pass (scan_pass / replay_scanned).
thread_local bool tl_recorder = false;
uint64_t g_rec_next = 0;      // recorder: the position after the packet it runs (replay_scanned moves it past the scanned packets)
uint64_t g_scan_end = 0;      // recorder: the position after the last packet scan_pass looked at
alignas(64) std::atomic<uint32_t> g_need{0};   // the producer waits for the recorder (a drain, a full ring): a look-ahead stops
ExecFn g_haz_fns[64];         // packets whose call goes through the hazard tracker's hooks (looked at in the dry run)
int g_nhaz = 0;
ExecFn g_er_exec = nullptr;   // vkCmdEndRendering's packet
uint64_t g_scans = 0, g_scan_stopped = 0, g_scan_waits = 0;
void mark_haz(ExecFn f, const void* orig) {
    if (hazard::is_hook(orig) && g_nhaz < 64) g_haz_fns[g_nhaz++] = f;
}
inline bool is_haz(ExecFn f) {
    for (int i = 0; i < g_nhaz; ++i) if (g_haz_fns[i] == f) return true;
    return false;
}

void recorder_loop() {
    tl_recorder = true;
    log_set_thread_name("nvn-recorder");
    diag_register_thread("nvn-recorder", nullptr);   // the sampler ([prof] sample_hz) and the freeze watchdog see its stack
    struct Unreg { ~Unreg() { diag_unregister_thread(); } } unreg;
    uint64_t tail = g_tail.load(std::memory_order_relaxed);
    for (;;) {
        uint64_t head = g_head.load(std::memory_order_acquire);
        if (head == tail) {
            for (int i = 0; i < 4000 && head == tail; ++i) {
                _mm_pause();
                head = g_head.load(std::memory_order_acquire);
            }
            if (head == tail) {
                if (g_stop.load(std::memory_order_acquire)) return;
                g_sleeping.store(1, std::memory_order_seq_cst);
                head = g_head.load(std::memory_order_seq_cst);
                if (head == tail) {
                    uint64_t seen = tail;
                    WaitOnAddress(&g_head, &seen, sizeof seen, 2);
                }
                g_sleeping.store(0, std::memory_order_relaxed);
                continue;
            }
        }
        while (tail != head) {
            const size_t at = static_cast<size_t>(tail % kRingBytes);
            const PacketHdr* h = reinterpret_cast<const PacketHdr*>(g_ring + at);
            if (!h->fn) {   // wrap marker: the rest of the ring is unused
                tail += kRingBytes - at;
                continue;
            }
            g_rec_next = tail + h->size;
            h->fn(g_ring + at + sizeof(PacketHdr));
            tail = g_rec_next;   // past a looked-ahead render pass too
            if (tail > head) head = g_head.load(std::memory_order_acquire);
        }
        g_tail.store(tail, std::memory_order_release);
    }
}

// The placeholder barrier the recorder may be holding at (ph_insert): its state word in the ring, null when none is pending.
std::atomic<uint32_t>* g_ph = nullptr;
PFN_vkCmdPipelineBarrier2 o_pb2_raw = nullptr;   // the driver's (set at install)
uint64_t g_ph_n = 0, g_ph_forced = 0;
void ph_set(uint32_t v) {   // 1 barrier, 2 nothing
    if (!g_ph) return;
    g_ph->store(v, std::memory_order_release);
    WakeByAddressSingle(g_ph);
    g_ph = nullptr;
}

// Room for a packet of `payload` bytes; the returned pointer is the payload. commit() publishes it.
uint64_t g_wr = 0;   // producer's write position (== g_head between packets)
// [nvn] cmd_publish_batch (default 32; cpuperf_ab_what cmd_batch): commit() makes the queued packets visible to the recorder only every N
// packets (or 4 KiB), and before every drain, a full ring and a placeholder. Every packet used to store the shared head word and read the
// recorder's tail word: ~20k cache-line transfers per frame between the two threads (the render thread's atomic loads in reserve()).
uint64_t g_tail_seen = 0;    // the recorder's tail as the producer last read it (only grows): the free space is at least this much
uint64_t g_pub = 0;          // what g_head holds (producer side)
uint32_t g_unpub = 0;        // packets since the last publish
uint32_t g_pub_batch = 1;    // set from the config at install, 1 in cpuperf_ab off windows
uint32_t g_pub_batch_cfg = 1;
inline void publish() {
    if (g_pub == g_wr) return;
    g_pub = g_wr;
    g_unpub = 0;
    g_head.store(g_wr, std::memory_order_release);
    if (g_sleeping.load(std::memory_order_seq_cst)) WakeByAddressSingle(&g_head);
}
uint8_t* reserve(ExecFn fn, size_t payload) {
    const size_t size = (sizeof(PacketHdr) + payload + 15) & ~size_t{15};
    for (;;) {
        size_t at = static_cast<size_t>(g_wr % kRingBytes);
        const size_t need = (at + size > kRingBytes) ? (kRingBytes - at) + size : size;
        if (g_wr + need - g_tail_seen > kRingBytes) {
            publish();   // the recorder must see everything before the producer waits for room
            g_tail_seen = g_tail.load(std::memory_order_acquire);
        }
        if (g_wr + need - g_tail_seen <= kRingBytes) {
            if (g_need.load(std::memory_order_relaxed)) g_need.store(0, std::memory_order_relaxed);
            if (at + size > kRingBytes) {   // wrap
                reinterpret_cast<PacketHdr*>(g_ring + at)->fn = nullptr;
                g_wr += kRingBytes - at;
                at = 0;
            }
            PacketHdr* h = reinterpret_cast<PacketHdr*>(g_ring + at);
            h->fn = fn;
            h->size = static_cast<uint32_t>(size);
            g_wr += size;
            return g_ring + at + sizeof(PacketHdr);
        }
        if (g_ph) { ++g_ph_forced; ph_set(1); }   // the recorder may be held at a placeholder: it becomes a barrier
        g_need.store(1, std::memory_order_release);   // a look-ahead stops
        _mm_pause();   // full: the recorder makes room
    }
}
void commit() {
    ++g_packets;
    if (++g_unpub >= g_pub_batch || g_wr - g_pub >= 4096) publish();
}

void drain_now() {
    publish();
    if (g_ph) { ++g_ph_forced; ph_set(1); }
    if (g_tail.load(std::memory_order_acquire) == g_wr) return;
    ++g_drains;
    g_need.store(1, std::memory_order_seq_cst);   // a look-ahead through a pass stops (its end may not be queued yet)
    if (g_sleeping.load(std::memory_order_seq_cst)) WakeByAddressSingle(&g_head);
    while (g_tail.load(std::memory_order_acquire) != g_wr) { _mm_pause(); ++g_drain_spins; }
    g_need.store(0, std::memory_order_relaxed);
}

// ---- hooks ----------------------------------------------------------------------------------------------------------------------
template <typename T> constexpr bool is_data_ptr_v = std::is_pointer_v<T> && std::is_const_v<std::remove_pointer_t<T>>;

// Direct: drain, then the driver call on the render thread (calls with data the hooks do not copy, and everything about command buffers
// and queues as a whole).
template <auto* Slot, typename T> struct Direct;
template <auto* Slot, typename R, typename... A>
struct Direct<Slot, R(VKAPI_PTR*)(A...)> {
    static inline R(VKAPI_PTR* orig)(A...) = nullptr;
    static inline uint64_t n = 0;
    static R VKAPI_PTR call(A... a) {
        if (queue_here()) { drain_now(); ++g_direct; ++n; }
        return orig(a...);
    }
    static void install(const char* name) {
        if (*Slot && !orig) { orig = *Slot; *Slot = &call; if (g_ndirect < 64) g_direct_names[g_ndirect++] = {name, &n}; }
    }
};

// Queued: all arguments by value (handles and scalars; no pointer to data).
template <auto* Slot, typename T> struct Queued;
template <auto* Slot, typename... A>
struct Queued<Slot, void(VKAPI_PTR*)(A...)> {
    static_assert(!(is_data_ptr_v<A> || ...), "a queued call takes no pointer to data");
    using Args = std::tuple<A...>;
    static inline void(VKAPI_PTR* orig)(A...) = nullptr;
    static void exec(const uint8_t* p) { std::apply(orig, *reinterpret_cast<const Args*>(p)); }
    static void VKAPI_PTR call(A... a) {
        if (!queue_here()) return orig(a...);
        new (reserve(&exec, sizeof(Args))) Args(a...);
        commit();
    }
    static void install() { if (*Slot && !orig) { orig = *Slot; *Slot = &call; mark_haz(&exec, reinterpret_cast<const void*>(orig)); } }
};

#define NXQ_DIRECT(name) Direct<&name, PFN_##name>::install(#name)
#define NXQ_QUEUED(name) Queued<&name, PFN_##name>::install()

// ---- queued calls with arrays (the hot ones): the arrays are copied after the arguments ----------------------------------------------
// A small bump writer over a payload.
struct Wr {
    uint8_t* p;
    template <typename T> T* put(const T* src, size_t n) {
        p = reinterpret_cast<uint8_t*>((reinterpret_cast<uintptr_t>(p) + alignof(T) - 1) & ~(uintptr_t{alignof(T)} - 1));
        T* d = reinterpret_cast<T*>(p);
        if (n) std::memcpy(d, src, sizeof(T) * n);
        p += sizeof(T) * n;
        return d;
    }
};
template <typename T> constexpr size_t room(size_t n) { return sizeof(T) * n + alignof(T); }

PFN_vkCmdBindDescriptorSets o_bds = nullptr;
struct BdsArgs { VkCommandBuffer cb; VkPipelineBindPoint bp; VkPipelineLayout layout; uint32_t first, n, ndyn; const VkDescriptorSet* sets; const uint32_t* dyn; };
void x_bds(const uint8_t* p) { const BdsArgs& a = *reinterpret_cast<const BdsArgs*>(p); o_bds(a.cb, a.bp, a.layout, a.first, a.n, a.sets, a.ndyn, a.dyn); }
void VKAPI_PTR h_bds(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout layout, uint32_t first, uint32_t n, const VkDescriptorSet* sets,
                     uint32_t ndyn, const uint32_t* dyn) {
    if (!queue_here()) return o_bds(cb, bp, layout, first, n, sets, ndyn, dyn);
    uint8_t* p = reserve(&x_bds, sizeof(BdsArgs) + room<VkDescriptorSet>(n) + room<uint32_t>(ndyn));
    BdsArgs* a = reinterpret_cast<BdsArgs*>(p);
    Wr w{p + sizeof(BdsArgs)};
    *a = {cb, bp, layout, first, n, ndyn, w.put(sets, n), w.put(dyn, ndyn)};
    commit();
}

PFN_vkCmdBindVertexBuffers2 o_bvb2 = nullptr;
struct Bvb2Args { VkCommandBuffer cb; uint32_t first, n; const VkBuffer* b; const VkDeviceSize *off, *size, *stride; };
void x_bvb2(const uint8_t* p) { const Bvb2Args& a = *reinterpret_cast<const Bvb2Args*>(p); o_bvb2(a.cb, a.first, a.n, a.b, a.off, a.size, a.stride); }
void VKAPI_PTR h_bvb2(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkBuffer* b, const VkDeviceSize* off, const VkDeviceSize* size, const VkDeviceSize* stride) {
    if (!queue_here()) return o_bvb2(cb, first, n, b, off, size, stride);
    uint8_t* p = reserve(&x_bvb2, sizeof(Bvb2Args) + room<VkBuffer>(n) + 3 * room<VkDeviceSize>(n));
    Bvb2Args* a = reinterpret_cast<Bvb2Args*>(p);
    Wr w{p + sizeof(Bvb2Args)};
    const VkBuffer* cb_ = w.put(b, n);
    const VkDeviceSize* co = w.put(off, n);
    const VkDeviceSize* cs = size ? w.put(size, n) : nullptr;
    const VkDeviceSize* ct = stride ? w.put(stride, n) : nullptr;
    *a = {cb, first, n, cb_, co, cs, ct};
    commit();
}

PFN_vkCmdPushConstants o_pc = nullptr;
struct PcArgs { VkCommandBuffer cb; VkPipelineLayout layout; VkShaderStageFlags st; uint32_t off, size; const void* data; };
void x_pc(const uint8_t* p) { const PcArgs& a = *reinterpret_cast<const PcArgs*>(p); o_pc(a.cb, a.layout, a.st, a.off, a.size, a.data); }
void VKAPI_PTR h_pc(VkCommandBuffer cb, VkPipelineLayout layout, VkShaderStageFlags st, uint32_t off, uint32_t size, const void* data) {
    if (!queue_here()) return o_pc(cb, layout, st, off, size, data);
    uint8_t* p = reserve(&x_pc, sizeof(PcArgs) + room<uint8_t>(size) + 16);
    PcArgs* a = reinterpret_cast<PcArgs*>(p);
    uint8_t* d = reinterpret_cast<uint8_t*>((reinterpret_cast<uintptr_t>(p + sizeof(PcArgs)) + 15) & ~uintptr_t{15});
    std::memcpy(d, data, size);
    *a = {cb, layout, st, off, size, d};
    commit();
}

PFN_vkCmdSetViewport o_vp = nullptr;
struct VpArgs { VkCommandBuffer cb; uint32_t first, n; const VkViewport* v; };
void x_vp(const uint8_t* p) { const VpArgs& a = *reinterpret_cast<const VpArgs*>(p); o_vp(a.cb, a.first, a.n, a.v); }
void VKAPI_PTR h_vp(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkViewport* v) {
    if (!queue_here()) return o_vp(cb, first, n, v);
    uint8_t* p = reserve(&x_vp, sizeof(VpArgs) + room<VkViewport>(n));
    VpArgs* a = reinterpret_cast<VpArgs*>(p);
    Wr w{p + sizeof(VpArgs)};
    *a = {cb, first, n, w.put(v, n)};
    commit();
}
PFN_vkCmdSetScissor o_sc = nullptr;
struct ScArgs { VkCommandBuffer cb; uint32_t first, n; const VkRect2D* r; };
void x_sc(const uint8_t* p) { const ScArgs& a = *reinterpret_cast<const ScArgs*>(p); o_sc(a.cb, a.first, a.n, a.r); }
void VKAPI_PTR h_sc(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkRect2D* r) {
    if (!queue_here()) return o_sc(cb, first, n, r);
    uint8_t* p = reserve(&x_sc, sizeof(ScArgs) + room<VkRect2D>(n));
    ScArgs* a = reinterpret_cast<ScArgs*>(p);
    Wr w{p + sizeof(ScArgs)};
    *a = {cb, first, n, w.put(r, n)};
    commit();
}
PFN_vkCmdSetViewportWithCount o_vpc = nullptr;
struct VpcArgs { VkCommandBuffer cb; uint32_t n; const VkViewport* v; };
void x_vpc(const uint8_t* p) { const VpcArgs& a = *reinterpret_cast<const VpcArgs*>(p); o_vpc(a.cb, a.n, a.v); }
void VKAPI_PTR h_vpc(VkCommandBuffer cb, uint32_t n, const VkViewport* v) {
    if (!queue_here()) return o_vpc(cb, n, v);
    uint8_t* p = reserve(&x_vpc, sizeof(VpcArgs) + room<VkViewport>(n));
    VpcArgs* a = reinterpret_cast<VpcArgs*>(p);
    Wr w{p + sizeof(VpcArgs)};
    *a = {cb, n, w.put(v, n)};
    commit();
}
PFN_vkCmdSetScissorWithCount o_scc = nullptr;
struct SccArgs { VkCommandBuffer cb; uint32_t n; const VkRect2D* r; };
void x_scc(const uint8_t* p) { const SccArgs& a = *reinterpret_cast<const SccArgs*>(p); o_scc(a.cb, a.n, a.r); }
void VKAPI_PTR h_scc(VkCommandBuffer cb, uint32_t n, const VkRect2D* r) {
    if (!queue_here()) return o_scc(cb, n, r);
    uint8_t* p = reserve(&x_scc, sizeof(SccArgs) + room<VkRect2D>(n));
    SccArgs* a = reinterpret_cast<SccArgs*>(p);
    Wr w{p + sizeof(SccArgs)};
    *a = {cb, n, w.put(r, n)};
    commit();
}

PFN_vkCmdSetBlendConstants o_bc = nullptr;
struct BcArgs { VkCommandBuffer cb; float c[4]; };
void x_bc(const uint8_t* p) { const BcArgs& a = *reinterpret_cast<const BcArgs*>(p); o_bc(a.cb, a.c); }
void VKAPI_PTR h_bc(VkCommandBuffer cb, const float c[4]) {
    if (!queue_here()) return o_bc(cb, c);
    BcArgs* a = reinterpret_cast<BcArgs*>(reserve(&x_bc, sizeof(BcArgs)));
    a->cb = cb;
    std::memcpy(a->c, c, sizeof a->c);
    commit();
}

PFN_vkCmdCopyBuffer o_cpb = nullptr;
struct CpbArgs { VkCommandBuffer cb; VkBuffer s, d; uint32_t n; const VkBufferCopy* r; };
void x_cpb(const uint8_t* p) { const CpbArgs& a = *reinterpret_cast<const CpbArgs*>(p); o_cpb(a.cb, a.s, a.d, a.n, a.r); }
void VKAPI_PTR h_cpb(VkCommandBuffer cb, VkBuffer s, VkBuffer d, uint32_t n, const VkBufferCopy* r) {
    if (!queue_here()) return o_cpb(cb, s, d, n, r);
    uint8_t* p = reserve(&x_cpb, sizeof(CpbArgs) + room<VkBufferCopy>(n));
    CpbArgs* a = reinterpret_cast<CpbArgs*>(p);
    Wr w{p + sizeof(CpbArgs)};
    *a = {cb, s, d, n, w.put(r, n)};
    commit();
}

PFN_vkCmdClearAttachments o_cla = nullptr;
struct ClaArgs { VkCommandBuffer cb; uint32_t na, nr; const VkClearAttachment* a; const VkClearRect* r; };
void x_cla(const uint8_t* p) { const ClaArgs& a = *reinterpret_cast<const ClaArgs*>(p); o_cla(a.cb, a.na, a.a, a.nr, a.r); }
void VKAPI_PTR h_cla(VkCommandBuffer cb, uint32_t na, const VkClearAttachment* at, uint32_t nr, const VkClearRect* r) {
    if (!queue_here()) return o_cla(cb, na, at, nr, r);
    uint8_t* p = reserve(&x_cla, sizeof(ClaArgs) + room<VkClearAttachment>(na) + room<VkClearRect>(nr));
    ClaArgs* a = reinterpret_cast<ClaArgs*>(p);
    Wr w{p + sizeof(ClaArgs)};
    const VkClearAttachment* ca = w.put(at, na);
    const VkClearRect* cr = w.put(r, nr);
    *a = {cb, na, nr, ca, cr};
    commit();
}

PFN_vkCmdUpdateBuffer o_upd = nullptr;
struct UpdArgs { VkCommandBuffer cb; VkBuffer b; VkDeviceSize off, size; const void* data; };
void x_upd(const uint8_t* p) { const UpdArgs& a = *reinterpret_cast<const UpdArgs*>(p); o_upd(a.cb, a.b, a.off, a.size, a.data); }
void VKAPI_PTR h_upd(VkCommandBuffer cb, VkBuffer b, VkDeviceSize off, VkDeviceSize size, const void* data) {
    if (!queue_here() || size > 65536) { if (queue_here()) { drain_now(); ++g_direct; } return o_upd(cb, b, off, size, data); }
    uint8_t* p = reserve(&x_upd, sizeof(UpdArgs) + static_cast<size_t>(size) + 16);
    UpdArgs* a = reinterpret_cast<UpdArgs*>(p);
    uint8_t* d = reinterpret_cast<uint8_t*>((reinterpret_cast<uintptr_t>(p + sizeof(UpdArgs)) + 15) & ~uintptr_t{15});
    std::memcpy(d, data, static_cast<size_t>(size));
    *a = {cb, b, off, size, d};
    commit();
}

// vkCmdPipelineBarrier2: the dependency info and its three arrays (no pNext anywhere, else direct).
PFN_vkCmdPipelineBarrier2 o_pb2 = nullptr;
struct Pb2Args { VkCommandBuffer cb; VkDependencyInfo di; };
void x_pb2(const uint8_t* p) { const Pb2Args& a = *reinterpret_cast<const Pb2Args*>(p); o_pb2(a.cb, &a.di); }
void VKAPI_PTR h_pb2(VkCommandBuffer cb, const VkDependencyInfo* di) {
    if (!queue_here()) return o_pb2(cb, di);
    bool chained = di->pNext != nullptr;
    for (uint32_t i = 0; i < di->memoryBarrierCount && !chained; ++i) chained = di->pMemoryBarriers[i].pNext != nullptr;
    for (uint32_t i = 0; i < di->bufferMemoryBarrierCount && !chained; ++i) chained = di->pBufferMemoryBarriers[i].pNext != nullptr;
    for (uint32_t i = 0; i < di->imageMemoryBarrierCount && !chained; ++i) chained = di->pImageMemoryBarriers[i].pNext != nullptr;
    if (chained) { drain_now(); ++g_direct; return o_pb2(cb, di); }
    uint8_t* p = reserve(&x_pb2, sizeof(Pb2Args) + room<VkMemoryBarrier2>(di->memoryBarrierCount) + room<VkBufferMemoryBarrier2>(di->bufferMemoryBarrierCount) +
                                     room<VkImageMemoryBarrier2>(di->imageMemoryBarrierCount));
    Pb2Args* a = reinterpret_cast<Pb2Args*>(p);
    Wr w{p + sizeof(Pb2Args)};
    a->cb = cb;
    a->di = *di;
    a->di.pMemoryBarriers = w.put(di->pMemoryBarriers, di->memoryBarrierCount);
    a->di.pBufferMemoryBarriers = w.put(di->pBufferMemoryBarriers, di->bufferMemoryBarrierCount);
    a->di.pImageMemoryBarriers = w.put(di->pImageMemoryBarriers, di->imageMemoryBarrierCount);
    commit();
}

// The placeholder barrier ([nvn] hazard): the recorder waits here until the render thread has decided (ph_resolve), then records a full
// memory barrier or nothing.
struct PhArgs { VkCommandBuffer cb; std::atomic<uint32_t> state; };
void full_barrier(VkCommandBuffer cb) {
    VkMemoryBarrier2 b = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.memoryBarrierCount = 1;
    di.pMemoryBarriers = &b;
    o_pb2(cb, &di);
}
void x_ph(const uint8_t* p) {
    PhArgs& a = *reinterpret_cast<PhArgs*>(const_cast<uint8_t*>(p));
    uint32_t s = a.state.load(std::memory_order_acquire);
    for (int i = 0; s == 0 && i < 20000; ++i) { _mm_pause(); s = a.state.load(std::memory_order_acquire); }
    while (s == 0) {
        uint32_t zero = 0;
        WaitOnAddress(&a.state, &zero, sizeof zero, 1);
        s = a.state.load(std::memory_order_acquire);
    }
    if (s == 1) full_barrier(a.cb);
}

// vkCmdBeginRendering: the info, its colour attachments and the depth / stencil attachments (no pNext anywhere, else direct).
PFN_vkCmdBeginRendering o_br = nullptr;
struct BrArgs { VkCommandBuffer cb; VkRenderingInfo ri; };
void x_br(const uint8_t* p) { const BrArgs& a = *reinterpret_cast<const BrArgs*>(p); o_br(a.cb, &a.ri); }
void VKAPI_PTR h_br(VkCommandBuffer cb, const VkRenderingInfo* ri) {
    if (!queue_here()) return o_br(cb, ri);
    bool chained = ri->pNext != nullptr || (ri->pDepthAttachment && ri->pDepthAttachment->pNext) || (ri->pStencilAttachment && ri->pStencilAttachment->pNext);
    for (uint32_t i = 0; i < ri->colorAttachmentCount && !chained; ++i) chained = ri->pColorAttachments[i].pNext != nullptr;
    if (chained) { drain_now(); ++g_direct; return o_br(cb, ri); }
    uint8_t* p = reserve(&x_br, sizeof(BrArgs) + room<VkRenderingAttachmentInfo>(ri->colorAttachmentCount + 2));
    BrArgs* a = reinterpret_cast<BrArgs*>(p);
    Wr w{p + sizeof(BrArgs)};
    a->cb = cb;
    a->ri = *ri;
    a->ri.pColorAttachments = w.put(ri->pColorAttachments, ri->colorAttachmentCount);
    a->ri.pDepthAttachment = ri->pDepthAttachment ? w.put(ri->pDepthAttachment, 1) : nullptr;
    a->ri.pStencilAttachment = ri->pStencilAttachment ? w.put(ri->pStencilAttachment, 1) : nullptr;
    commit();
}

// vkCmdBeginDebugUtilsLabelEXT: the label and its name (the game's debug groups, many per frame).
PFN_vkCmdBeginDebugUtilsLabelEXT o_bdl = nullptr;
struct BdlArgs { VkCommandBuffer cb; VkDebugUtilsLabelEXT l; };
void x_bdl(const uint8_t* p) { const BdlArgs& a = *reinterpret_cast<const BdlArgs*>(p); o_bdl(a.cb, &a.l); }
void VKAPI_PTR h_bdl(VkCommandBuffer cb, const VkDebugUtilsLabelEXT* l) {
    if (!queue_here()) return o_bdl(cb, l);
    if (l->pNext) { drain_now(); ++g_direct; return o_bdl(cb, l); }
    const size_t n = l->pLabelName ? std::strlen(l->pLabelName) + 1 : 0;
    uint8_t* p = reserve(&x_bdl, sizeof(BdlArgs) + n);
    BdlArgs* a = reinterpret_cast<BdlArgs*>(p);
    a->cb = cb;
    a->l = *l;
    if (n) {
        char* s = reinterpret_cast<char*>(p + sizeof(BdlArgs));
        std::memcpy(s, l->pLabelName, n);
        a->l.pLabelName = s;
    }
    commit();
}

// vkCmdSetFragmentShadingRateKHR: the fragment size and the two combiner ops (set with the dynamic state of most draws).
PFN_vkCmdSetFragmentShadingRateKHR o_fsr = nullptr;
struct FsrArgs { VkCommandBuffer cb; VkExtent2D size; VkFragmentShadingRateCombinerOpKHR ops[2]; };
void x_fsr(const uint8_t* p) { const FsrArgs& a = *reinterpret_cast<const FsrArgs*>(p); o_fsr(a.cb, &a.size, a.ops); }
void VKAPI_PTR h_fsr(VkCommandBuffer cb, const VkExtent2D* size, const VkFragmentShadingRateCombinerOpKHR ops[2]) {
    if (!queue_here()) return o_fsr(cb, size, ops);
    FsrArgs* a = reinterpret_cast<FsrArgs*>(reserve(&x_fsr, sizeof(FsrArgs)));
    a->cb = cb;
    a->size = *size;
    a->ops[0] = ops[0];
    a->ops[1] = ops[1];
    commit();
}

// [nvn] cmd_thread_copies, switched by cpuperf_ab (`cmd_copies`): off makes these calls wait for the recorder as before.
std::atomic<bool> g_copies_q{true};
inline bool q_copy() {
    if (!queue_here()) return false;
    if (!g_copies_q.load(std::memory_order_relaxed)) { drain_now(); ++g_direct; return false; }
    return true;
}
// Image copies, blits, resolves and clears (texture uploads, render-target copies): their region arrays are copied. They waited for the
// recorder ("drain") before, which made the render thread wait for every queued call at each texture upload.
PFN_vkCmdCopyBufferToImage o_cbti = nullptr;
struct CbtiArgs { VkCommandBuffer cb; VkBuffer b; VkImage i; VkImageLayout l; uint32_t n; const VkBufferImageCopy* r; };
void x_cbti(const uint8_t* p) { const CbtiArgs& a = *reinterpret_cast<const CbtiArgs*>(p); o_cbti(a.cb, a.b, a.i, a.l, a.n, a.r); }
void VKAPI_PTR h_cbti(VkCommandBuffer cb, VkBuffer b, VkImage i, VkImageLayout l, uint32_t n, const VkBufferImageCopy* r) {
    if (!q_copy()) return o_cbti(cb, b, i, l, n, r);
    uint8_t* p = reserve(&x_cbti, sizeof(CbtiArgs) + room<VkBufferImageCopy>(n));
    CbtiArgs* a = reinterpret_cast<CbtiArgs*>(p);
    Wr w{p + sizeof(CbtiArgs)};
    *a = {cb, b, i, l, n, w.put(r, n)};
    commit();
}
PFN_vkCmdCopyImageToBuffer o_citb = nullptr;
struct CitbArgs { VkCommandBuffer cb; VkImage i; VkImageLayout l; VkBuffer b; uint32_t n; const VkBufferImageCopy* r; };
void x_citb(const uint8_t* p) { const CitbArgs& a = *reinterpret_cast<const CitbArgs*>(p); o_citb(a.cb, a.i, a.l, a.b, a.n, a.r); }
void VKAPI_PTR h_citb(VkCommandBuffer cb, VkImage i, VkImageLayout l, VkBuffer b, uint32_t n, const VkBufferImageCopy* r) {
    if (!q_copy()) return o_citb(cb, i, l, b, n, r);
    uint8_t* p = reserve(&x_citb, sizeof(CitbArgs) + room<VkBufferImageCopy>(n));
    CitbArgs* a = reinterpret_cast<CitbArgs*>(p);
    Wr w{p + sizeof(CitbArgs)};
    *a = {cb, i, l, b, n, w.put(r, n)};
    commit();
}
PFN_vkCmdCopyImage o_ci = nullptr;
struct CiArgs { VkCommandBuffer cb; VkImage s; VkImageLayout sl; VkImage d; VkImageLayout dl; uint32_t n; const VkImageCopy* r; };
void x_ci(const uint8_t* p) { const CiArgs& a = *reinterpret_cast<const CiArgs*>(p); o_ci(a.cb, a.s, a.sl, a.d, a.dl, a.n, a.r); }
void VKAPI_PTR h_ci(VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n, const VkImageCopy* r) {
    if (!q_copy()) return o_ci(cb, s, sl, d, dl, n, r);
    uint8_t* p = reserve(&x_ci, sizeof(CiArgs) + room<VkImageCopy>(n));
    CiArgs* a = reinterpret_cast<CiArgs*>(p);
    Wr w{p + sizeof(CiArgs)};
    *a = {cb, s, sl, d, dl, n, w.put(r, n)};
    commit();
}
PFN_vkCmdBlitImage o_bi = nullptr;
struct BiArgs { VkCommandBuffer cb; VkImage s; VkImageLayout sl; VkImage d; VkImageLayout dl; uint32_t n; const VkImageBlit* r; VkFilter f; };
void x_bi(const uint8_t* p) { const BiArgs& a = *reinterpret_cast<const BiArgs*>(p); o_bi(a.cb, a.s, a.sl, a.d, a.dl, a.n, a.r, a.f); }
void VKAPI_PTR h_bi(VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n, const VkImageBlit* r, VkFilter f) {
    if (!q_copy()) return o_bi(cb, s, sl, d, dl, n, r, f);
    uint8_t* p = reserve(&x_bi, sizeof(BiArgs) + room<VkImageBlit>(n));
    BiArgs* a = reinterpret_cast<BiArgs*>(p);
    Wr w{p + sizeof(BiArgs)};
    *a = {cb, s, sl, d, dl, n, w.put(r, n), f};
    commit();
}
PFN_vkCmdResolveImage o_ri = nullptr;
struct RiArgs { VkCommandBuffer cb; VkImage s; VkImageLayout sl; VkImage d; VkImageLayout dl; uint32_t n; const VkImageResolve* r; };
void x_ri(const uint8_t* p) { const RiArgs& a = *reinterpret_cast<const RiArgs*>(p); o_ri(a.cb, a.s, a.sl, a.d, a.dl, a.n, a.r); }
void VKAPI_PTR h_ri(VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n, const VkImageResolve* r) {
    if (!q_copy()) return o_ri(cb, s, sl, d, dl, n, r);
    uint8_t* p = reserve(&x_ri, sizeof(RiArgs) + room<VkImageResolve>(n));
    RiArgs* a = reinterpret_cast<RiArgs*>(p);
    Wr w{p + sizeof(RiArgs)};
    *a = {cb, s, sl, d, dl, n, w.put(r, n)};
    commit();
}
PFN_vkCmdClearColorImage o_cci = nullptr;
struct CciArgs { VkCommandBuffer cb; VkImage i; VkImageLayout l; VkClearColorValue c; uint32_t n; const VkImageSubresourceRange* r; };
void x_cci(const uint8_t* p) { const CciArgs& a = *reinterpret_cast<const CciArgs*>(p); o_cci(a.cb, a.i, a.l, &a.c, a.n, a.r); }
void VKAPI_PTR h_cci(VkCommandBuffer cb, VkImage i, VkImageLayout l, const VkClearColorValue* c, uint32_t n, const VkImageSubresourceRange* r) {
    if (!q_copy()) return o_cci(cb, i, l, c, n, r);
    uint8_t* p = reserve(&x_cci, sizeof(CciArgs) + room<VkImageSubresourceRange>(n));
    CciArgs* a = reinterpret_cast<CciArgs*>(p);
    Wr w{p + sizeof(CciArgs)};
    *a = {cb, i, l, *c, n, w.put(r, n)};
    commit();
}
PFN_vkCmdClearDepthStencilImage o_cdsi = nullptr;
struct CdsiArgs { VkCommandBuffer cb; VkImage i; VkImageLayout l; VkClearDepthStencilValue c; uint32_t n; const VkImageSubresourceRange* r; };
void x_cdsi(const uint8_t* p) { const CdsiArgs& a = *reinterpret_cast<const CdsiArgs*>(p); o_cdsi(a.cb, a.i, a.l, &a.c, a.n, a.r); }
void VKAPI_PTR h_cdsi(VkCommandBuffer cb, VkImage i, VkImageLayout l, const VkClearDepthStencilValue* c, uint32_t n, const VkImageSubresourceRange* r) {
    if (!q_copy()) return o_cdsi(cb, i, l, c, n, r);
    uint8_t* p = reserve(&x_cdsi, sizeof(CdsiArgs) + room<VkImageSubresourceRange>(n));
    CdsiArgs* a = reinterpret_cast<CdsiArgs*>(p);
    Wr w{p + sizeof(CdsiArgs)};
    *a = {cb, i, l, *c, n, w.put(r, n)};
    commit();
}
PFN_vkCmdBindVertexBuffers o_bvb = nullptr;
struct BvbArgs { VkCommandBuffer cb; uint32_t first, n; const VkBuffer* b; const VkDeviceSize* off; };
void x_bvb(const uint8_t* p) { const BvbArgs& a = *reinterpret_cast<const BvbArgs*>(p); o_bvb(a.cb, a.first, a.n, a.b, a.off); }
void VKAPI_PTR h_bvb(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkBuffer* b, const VkDeviceSize* off) {
    if (!q_copy()) return o_bvb(cb, first, n, b, off);
    uint8_t* p = reserve(&x_bvb, sizeof(BvbArgs) + room<VkBuffer>(n) + room<VkDeviceSize>(n));
    BvbArgs* a = reinterpret_cast<BvbArgs*>(p);
    Wr w{p + sizeof(BvbArgs)};
    const VkBuffer* cb_ = w.put(b, n);
    *a = {cb, first, n, cb_, w.put(off, n)};
    commit();
}
// Extended dynamic state 3 with arrays: one element type per call, copied.
template <auto* Slot, auto* Orig, typename E>
struct Arr3 {
    struct Args { VkCommandBuffer cb; uint32_t first, n; const E* v; };
    static void exec(const uint8_t* p) { const Args& a = *reinterpret_cast<const Args*>(p); (*Orig)(a.cb, a.first, a.n, a.v); }
    static void VKAPI_PTR call(VkCommandBuffer cb, uint32_t first, uint32_t n, const E* v) {
        if (!q_copy()) return (*Orig)(cb, first, n, v);
        uint8_t* p = reserve(&exec, sizeof(Args) + room<E>(n));
        Args* a = reinterpret_cast<Args*>(p);
        Wr w{p + sizeof(Args)};
        *a = {cb, first, n, w.put(v, n)};
        commit();
    }
};
PFN_vkCmdSetColorBlendEnableEXT o_cbe = nullptr;
PFN_vkCmdSetColorBlendEquationEXT o_cbq = nullptr;
PFN_vkCmdSetColorWriteMaskEXT o_cwm = nullptr;
PFN_vkCmdSetSampleMaskEXT o_smk = nullptr;
struct SmkArgs { VkCommandBuffer cb; VkSampleCountFlagBits s; const VkSampleMask* m; };
void x_smk(const uint8_t* p) { const SmkArgs& a = *reinterpret_cast<const SmkArgs*>(p); o_smk(a.cb, a.s, a.m); }
void VKAPI_PTR h_smk(VkCommandBuffer cb, VkSampleCountFlagBits s, const VkSampleMask* m) {
    if (!q_copy()) return o_smk(cb, s, m);
    const size_t n = (static_cast<size_t>(s) + 31) / 32;
    uint8_t* p = reserve(&x_smk, sizeof(SmkArgs) + room<VkSampleMask>(n));
    SmkArgs* a = reinterpret_cast<SmkArgs*>(p);
    Wr w{p + sizeof(SmkArgs)};
    *a = {cb, s, m ? w.put(m, n) : nullptr};
    commit();
}

// vkCmdPushDescriptorSetKHR (the delta copy-back shader, ~25 per frame): the writes and their buffer / image / texel-view arrays are copied
// (no pNext anywhere, else it waits for the recorder as before).
PFN_vkCmdPushDescriptorSetKHR o_pds = nullptr;
struct PdsArgs { VkCommandBuffer cb; VkPipelineBindPoint bp; VkPipelineLayout layout; uint32_t set, n; const VkWriteDescriptorSet* w; };
void x_pds(const uint8_t* p) { const PdsArgs& a = *reinterpret_cast<const PdsArgs*>(p); o_pds(a.cb, a.bp, a.layout, a.set, a.n, a.w); }
// [nvn] cmd_thread_pds (default on; cpuperf_ab_what pds_q): queued without the image copies of cmd_thread_copies (those made the recorder
// fall behind in the first scene); each direct one waited for the whole ring (~20 per frame in the village scene).
std::atomic<bool> g_pds_q{true};
void VKAPI_PTR h_pds(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout layout, uint32_t set, uint32_t n, const VkWriteDescriptorSet* w) {
    if (!queue_here()) return o_pds(cb, bp, layout, set, n, w);
    if (!g_pds_q.load(std::memory_order_relaxed)) { drain_now(); ++g_direct; return o_pds(cb, bp, layout, set, n, w); }
    size_t extra = room<VkWriteDescriptorSet>(n);
    for (uint32_t i = 0; i < n; ++i) {
        if (w[i].pNext) { drain_now(); ++g_direct; return o_pds(cb, bp, layout, set, n, w); }
        const uint32_t c = w[i].descriptorCount;
        if (w[i].pBufferInfo) extra += room<VkDescriptorBufferInfo>(c);
        if (w[i].pImageInfo) extra += room<VkDescriptorImageInfo>(c);
        if (w[i].pTexelBufferView) extra += room<VkBufferView>(c);
    }
    uint8_t* p = reserve(&x_pds, sizeof(PdsArgs) + extra);
    PdsArgs* a = reinterpret_cast<PdsArgs*>(p);
    Wr wr{p + sizeof(PdsArgs)};
    VkWriteDescriptorSet* cw = wr.put(w, n);
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t c = w[i].descriptorCount;
        if (w[i].pBufferInfo) cw[i].pBufferInfo = wr.put(w[i].pBufferInfo, c);
        if (w[i].pImageInfo) cw[i].pImageInfo = wr.put(w[i].pImageInfo, c);
        if (w[i].pTexelBufferView) cw[i].pTexelBufferView = wr.put(w[i].pTexelBufferView, c);
    }
    *a = {cb, bp, layout, set, n, cw};
    commit();
}

// [nvn] cmd_thread_desc (default on; cpuperf_ab_what cmd_desc): vkUpdateDescriptorSets of the render thread is queued as well. The
// recorder makes the update before any later queued bind of that set, which is all Vulkan asks (sets are only bound after their update;
// sets are freed or their pools reset only with the ring drained: those calls drain first). Writes with pNext, descriptor copies and very
// large updates (the 65,536-entry pool-array sets at creation) run directly after a drain.
std::atomic<bool> g_desc_q{true};
PFN_vkUpdateDescriptorSets o_uds = nullptr;
struct UdsArgs { VkDevice dev; uint32_t n; const VkWriteDescriptorSet* w; };
void x_uds(const uint8_t* p) { const UdsArgs& a = *reinterpret_cast<const UdsArgs*>(p); o_uds(a.dev, a.n, a.w, 0, nullptr); }
uint64_t g_uds_queued = 0, g_uds_direct = 0;   // statistics (render thread)
void VKAPI_PTR h_uds(VkDevice dev, uint32_t n, const VkWriteDescriptorSet* w, uint32_t nc, const VkCopyDescriptorSet* c) {
    if (!queue_here()) return o_uds(dev, n, w, nc, c);
    bool direct = nc != 0 || !g_desc_q.load(std::memory_order_relaxed);
    size_t extra = room<VkWriteDescriptorSet>(n);
    for (uint32_t i = 0; i < n && !direct; ++i) {
        if (w[i].pNext) { direct = true; break; }
        const uint32_t k = w[i].descriptorCount;
        switch (w[i].descriptorType) {   // only the array the type uses is read (the others may be stale pointers)
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER: case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC: case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
                extra += room<VkDescriptorBufferInfo>(k); break;
            case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER: case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
                extra += room<VkBufferView>(k); break;
            case VK_DESCRIPTOR_TYPE_SAMPLER: case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER: case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
            case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE: case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
                extra += room<VkDescriptorImageInfo>(k); break;
            default: direct = true; break;
        }
    }
    if (direct || extra > (1u << 20)) { drain_now(); ++g_direct; ++g_uds_direct; return o_uds(dev, n, w, nc, c); }
    uint8_t* p = reserve(&x_uds, sizeof(UdsArgs) + extra);
    UdsArgs* a = reinterpret_cast<UdsArgs*>(p);
    Wr wr{p + sizeof(UdsArgs)};
    VkWriteDescriptorSet* cw = wr.put(w, n);
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t k = w[i].descriptorCount;
        cw[i].pBufferInfo = nullptr; cw[i].pImageInfo = nullptr; cw[i].pTexelBufferView = nullptr;
        switch (w[i].descriptorType) {
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER: case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC: case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
                cw[i].pBufferInfo = wr.put(w[i].pBufferInfo, k); break;
            case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER: case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
                cw[i].pTexelBufferView = wr.put(w[i].pTexelBufferView, k); break;
            default:
                cw[i].pImageInfo = wr.put(w[i].pImageInfo, k); break;
        }
    }
    *a = {dev, n, cw};
    commit();
    ++g_uds_queued;
}

template <typename P> void hook(P& slot, P& orig, P fn) { if (slot && !orig) { orig = slot; slot = fn; } }

void install_all() {
    hazard::install_cmd_hooks();   // [nvn] hazard: behind these hooks, so the recorder thread runs the tracking
    // queued, by value
    NXQ_QUEUED(vkCmdBindPipeline); NXQ_QUEUED(vkCmdDraw); NXQ_QUEUED(vkCmdDrawIndexed); NXQ_QUEUED(vkCmdDrawIndirect);
    NXQ_QUEUED(vkCmdDrawIndexedIndirect); NXQ_QUEUED(vkCmdDrawIndirectCount); NXQ_QUEUED(vkCmdDrawIndexedIndirectCount);
    NXQ_QUEUED(vkCmdDrawMeshTasksEXT); NXQ_QUEUED(vkCmdDrawMeshTasksIndirectEXT); NXQ_QUEUED(vkCmdDrawMeshTasksIndirectCountEXT);
    NXQ_QUEUED(vkCmdDrawIndirectByteCountEXT); NXQ_QUEUED(vkCmdDispatch); NXQ_QUEUED(vkCmdDispatchIndirect);
    NXQ_QUEUED(vkCmdBeginQuery); NXQ_QUEUED(vkCmdEndQuery); NXQ_QUEUED(vkCmdResetQueryPool); NXQ_QUEUED(vkCmdCopyQueryPoolResults);
    NXQ_QUEUED(vkCmdBeginQueryIndexedEXT); NXQ_QUEUED(vkCmdEndQueryIndexedEXT);
    NXQ_QUEUED(vkCmdWriteTimestamp2); NXQ_QUEUED(vkCmdWriteTimestamp); NXQ_QUEUED(vkCmdFillBuffer); NXQ_QUEUED(vkCmdEndRendering);
    NXQ_QUEUED(vkCmdBindIndexBuffer); NXQ_QUEUED(vkCmdSetStencilReference); NXQ_QUEUED(vkCmdSetStencilWriteMask);
    NXQ_QUEUED(vkCmdSetStencilCompareMask); NXQ_QUEUED(vkCmdSetStencilOp); NXQ_QUEUED(vkCmdSetStencilTestEnable); NXQ_QUEUED(vkCmdSetFrontFace);
    NXQ_QUEUED(vkCmdSetCullMode); NXQ_QUEUED(vkCmdSetPrimitiveTopology); NXQ_QUEUED(vkCmdSetLineWidth); NXQ_QUEUED(vkCmdSetDepthBias);
    NXQ_QUEUED(vkCmdSetDepthBiasEnable); NXQ_QUEUED(vkCmdSetDepthBounds); NXQ_QUEUED(vkCmdSetDepthTestEnable); NXQ_QUEUED(vkCmdSetDepthWriteEnable);
    NXQ_QUEUED(vkCmdSetDepthCompareOp); NXQ_QUEUED(vkCmdSetDepthBoundsTestEnable); NXQ_QUEUED(vkCmdSetPrimitiveRestartEnable);
    NXQ_QUEUED(vkCmdSetRasterizerDiscardEnable); NXQ_QUEUED(vkCmdSetConservativeRasterizationModeEXT);
    NXQ_QUEUED(vkCmdSetExtraPrimitiveOverestimationSizeEXT); NXQ_QUEUED(vkCmdEndDebugUtilsLabelEXT);
    // with arrays, copied
    hook(vkCmdBindDescriptorSets, o_bds, &h_bds); hook(vkCmdBindVertexBuffers2, o_bvb2, &h_bvb2); hook(vkCmdPushConstants, o_pc, &h_pc);
    hook(vkCmdSetViewport, o_vp, &h_vp); hook(vkCmdSetScissor, o_sc, &h_sc); hook(vkCmdSetViewportWithCount, o_vpc, &h_vpc);
    hook(vkCmdSetScissorWithCount, o_scc, &h_scc); hook(vkCmdSetBlendConstants, o_bc, &h_bc); hook(vkCmdCopyBuffer, o_cpb, &h_cpb);
    hook(vkCmdClearAttachments, o_cla, &h_cla); hook(vkCmdUpdateBuffer, o_upd, &h_upd); hook(vkCmdPipelineBarrier2, o_pb2, &h_pb2);
    hook(vkCmdBeginRendering, o_br, &h_br);
    // [nvn] cmd_thread_copies (default off): image copies/clears and push descriptors queued too. Paired windows at 1080: village outside replay
    // -0.56 ms (8/8) but fps +0.1; first scene replay +0.8 / +0.5 ms and fps -1.5 / -0.4 (median, two runs): the recorder falls behind there
    static const bool more = config_get_bool("nvn", "cmd_thread_copies", false);
    if (more) {
        hook(vkCmdCopyBufferToImage, o_cbti, &h_cbti); hook(vkCmdCopyImageToBuffer, o_citb, &h_citb); hook(vkCmdCopyImage, o_ci, &h_ci);
        hook(vkCmdBlitImage, o_bi, &h_bi); hook(vkCmdResolveImage, o_ri, &h_ri); hook(vkCmdClearColorImage, o_cci, &h_cci);
        hook(vkCmdClearDepthStencilImage, o_cdsi, &h_cdsi); hook(vkCmdBindVertexBuffers, o_bvb, &h_bvb);
        if (vkCmdSetColorBlendEnableEXT) { o_cbe = vkCmdSetColorBlendEnableEXT; vkCmdSetColorBlendEnableEXT = &Arr3<&vkCmdSetColorBlendEnableEXT, &o_cbe, VkBool32>::call; }
        if (vkCmdSetColorBlendEquationEXT) { o_cbq = vkCmdSetColorBlendEquationEXT; vkCmdSetColorBlendEquationEXT = &Arr3<&vkCmdSetColorBlendEquationEXT, &o_cbq, VkColorBlendEquationEXT>::call; }
        if (vkCmdSetColorWriteMaskEXT) { o_cwm = vkCmdSetColorWriteMaskEXT; vkCmdSetColorWriteMaskEXT = &Arr3<&vkCmdSetColorWriteMaskEXT, &o_cwm, VkColorComponentFlags>::call; }
        hook(vkCmdSetSampleMaskEXT, o_smk, &h_smk); hook(vkCmdPushDescriptorSetKHR, o_pds, &h_pds);
        NXQ_QUEUED(vkCmdSetPolygonModeEXT); NXQ_QUEUED(vkCmdSetDepthClampEnableEXT); NXQ_QUEUED(vkCmdSetLogicOpEnableEXT);
        NXQ_QUEUED(vkCmdSetAlphaToCoverageEnableEXT); NXQ_QUEUED(vkCmdSetRasterizationSamplesEXT); NXQ_QUEUED(vkCmdSetDepthClipEnableEXT);
    } else {
        NXQ_DIRECT(vkCmdCopyImageToBuffer); NXQ_DIRECT(vkCmdCopyBufferToImage); NXQ_DIRECT(vkCmdCopyImage); NXQ_DIRECT(vkCmdBlitImage);
        NXQ_DIRECT(vkCmdResolveImage); NXQ_DIRECT(vkCmdClearColorImage); NXQ_DIRECT(vkCmdClearDepthStencilImage); NXQ_DIRECT(vkCmdBindVertexBuffers);
        NXQ_DIRECT(vkCmdSetSampleMaskEXT); NXQ_DIRECT(vkCmdSetColorBlendEnableEXT); NXQ_DIRECT(vkCmdSetColorBlendEquationEXT);
        NXQ_DIRECT(vkCmdSetColorWriteMaskEXT); NXQ_DIRECT(vkCmdSetPolygonModeEXT); NXQ_DIRECT(vkCmdSetDepthClampEnableEXT);
        NXQ_DIRECT(vkCmdSetLogicOpEnableEXT); NXQ_DIRECT(vkCmdSetAlphaToCoverageEnableEXT); NXQ_DIRECT(vkCmdSetRasterizationSamplesEXT);
        NXQ_DIRECT(vkCmdSetDepthClipEnableEXT);
        if (config_get_bool("nvn", "cmd_thread_pds", true)) hook(vkCmdPushDescriptorSetKHR, o_pds, &h_pds);
        else NXQ_DIRECT(vkCmdPushDescriptorSetKHR);
    }
    // everything else the replay records: drained first, then called directly
    NXQ_DIRECT(vkCmdPipelineBarrier); NXQ_DIRECT(vkCmdCopyImageToBuffer2); NXQ_DIRECT(vkCmdCopyBufferToImage2);
    NXQ_DIRECT(vkCmdBeginTransformFeedbackEXT); NXQ_DIRECT(vkCmdEndTransformFeedbackEXT); NXQ_DIRECT(vkCmdBindTransformFeedbackBuffersEXT);
    hook(vkCmdBeginDebugUtilsLabelEXT, o_bdl, &h_bdl); hook(vkCmdSetFragmentShadingRateKHR, o_fsr, &h_fsr); NXQ_DIRECT(vkCmdSetCheckpointNV);
    NXQ_DIRECT(vkCmdSetVertexInputEXT);
    NXQ_DIRECT(vkCmdExecuteCommands);
    // descriptor set updates queued (cmd_thread_desc); freeing sets and resetting or destroying their pools wait for the queued updates
    static const bool desc = config_get_bool("nvn", "cmd_thread_desc", true);
    if (desc) {
        hook(vkUpdateDescriptorSets, o_uds, &h_uds);
        NXQ_DIRECT(vkResetDescriptorPool); NXQ_DIRECT(vkFreeDescriptorSets); NXQ_DIRECT(vkDestroyDescriptorPool);
    }
    // the command buffers, their pools and the queues as a whole
    NXQ_DIRECT(vkBeginCommandBuffer); NXQ_DIRECT(vkEndCommandBuffer); NXQ_DIRECT(vkResetCommandBuffer); NXQ_DIRECT(vkResetCommandPool);
    NXQ_DIRECT(vkAllocateCommandBuffers); NXQ_DIRECT(vkFreeCommandBuffers); NXQ_DIRECT(vkTrimCommandPool); NXQ_DIRECT(vkDestroyCommandPool);
    NXQ_DIRECT(vkQueueSubmit); NXQ_DIRECT(vkQueueSubmit2); NXQ_DIRECT(vkQueueWaitIdle); NXQ_DIRECT(vkDeviceWaitIdle); NXQ_DIRECT(vkQueuePresentKHR);
    o_pb2_raw = o_pb2;
    // the packets the hazard look-ahead runs dry (their calls go through the tracker's hooks)
    mark_haz(&x_bds, reinterpret_cast<const void*>(o_bds)); mark_haz(&x_bvb2, reinterpret_cast<const void*>(o_bvb2));
    mark_haz(&x_cpb, reinterpret_cast<const void*>(o_cpb)); mark_haz(&x_upd, reinterpret_cast<const void*>(o_upd));
    mark_haz(&x_pb2, reinterpret_cast<const void*>(o_pb2)); mark_haz(&x_br, reinterpret_cast<const void*>(o_br));
    mark_haz(&x_uds, reinterpret_cast<const void*>(o_uds));
    if (more) {
        mark_haz(&x_cbti, reinterpret_cast<const void*>(o_cbti)); mark_haz(&x_citb, reinterpret_cast<const void*>(o_citb));
        mark_haz(&x_ci, reinterpret_cast<const void*>(o_ci)); mark_haz(&x_bi, reinterpret_cast<const void*>(o_bi));
        mark_haz(&x_ri, reinterpret_cast<const void*>(o_ri)); mark_haz(&x_cci, reinterpret_cast<const void*>(o_cci));
        mark_haz(&x_cdsi, reinterpret_cast<const void*>(o_cdsi)); mark_haz(&x_bvb, reinterpret_cast<const void*>(o_bvb));
    }
    mark_haz(&x_pds, reinterpret_cast<const void*>(o_pds));   // queued with cmd_thread_pds or cmd_thread_copies
    g_er_exec = &Queued<&vkCmdEndRendering, PFN_vkCmdEndRendering>::exec;
}

}  // namespace

bool enabled() {
    static const bool on = config_get_bool("nvn", "cmd_thread", true);
    return on;
}

void begin_submission(bool on) {
    if (!enabled()) return;
    static bool installed = false;
    if (!installed) {
        installed = true;
        g_ring = static_cast<uint8_t*>(VirtualAlloc(nullptr, kRingBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        if (!g_ring) { NXLOG_W(kTag, "no memory for the command ring: cmd_thread stays off"); return; }
        install_all();
        g_pub_batch_cfg = static_cast<uint32_t>(std::clamp<int64_t>(config_get_int("nvn", "cmd_publish_batch", 32), 1, 4096));
        g_pub_batch = g_pub_batch_cfg;
        NXLOG_I(kTag, "cmd_thread: vkCmd* calls of the render thread are recorded by the nvn-recorder thread (16 MiB ring)");
    }
    if (!g_ring) return;
    if (!g_thread.joinable()) {   // the first time, and again after a shutdown (tests bring the back end up and down)
        g_stop.store(false, std::memory_order_release);
        g_thread = std::thread(recorder_loop);
        SetThreadDescription(g_thread.native_handle(), L"nvn-recorder");
    }
    drain_now();       // switching only with nothing queued
    g_active.store(on, std::memory_order_relaxed);
}

void set_copies(bool on) { g_copies_q.store(on, std::memory_order_relaxed); }
void set_pds(bool on) { g_pds_q.store(on, std::memory_order_relaxed); }

// [nvn] chunk_async (default on; cpuperf_ab_what chunk_async): a submission chunk (frame_chunk: end the command buffer, submit it without a
// fence or signal, begin the next one) is one packet the recorder runs in order, instead of a drain of the ring and the three calls on the
// render thread. The render thread never waits for a chunk's submission (no fence, no signal): later waits on the queue go through calls
// that drain first (the end of the submission, splits, presents).
namespace {
struct ChunkArgs { VkCommandBuffer old, next; VkQueue q; std::mutex* mu; };
std::atomic<uint64_t> g_chunk_fail{0};
void x_chunk(const uint8_t* p) {
    const ChunkArgs& a = *reinterpret_cast<const ChunkArgs*>(p);
    vkEndCommandBuffer(a.old);   // the recorder thread: the hooks call the driver directly
    VkCommandBufferSubmitInfo cbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cbi.commandBuffer = a.old;
    VkSubmitInfo2 si = {VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cbi;
    VkResult r;
    {
        std::lock_guard<std::mutex> lk(*a.mu);
        r = vkQueueSubmit2(a.q, 1, &si, VK_NULL_HANDLE);
    }
    if (r != VK_SUCCESS && g_chunk_fail.fetch_add(1) < 4) NXLOG_E(kTag, "submission chunk (recorder): vkQueueSubmit2 failed (%d)", static_cast<int>(r));
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(a.next, &bi);
}
}  // namespace
std::atomic<bool> g_chunk_async{true};
void set_chunk_async(bool on) { g_chunk_async.store(on, std::memory_order_relaxed); }
bool queue_chunk(VkCommandBuffer old, VkCommandBuffer next, VkQueue q, std::mutex* mu) {
    static const bool cfg = config_get_bool("nvn", "chunk_async", true);
    if (!cfg || !g_chunk_async.load(std::memory_order_relaxed) || !queuing()) return false;
    if (g_ph) ph_set(1);   // a hazard placeholder still open in `old` becomes a barrier before the buffer ends
    new (reserve(&x_chunk, sizeof(ChunkArgs))) ChunkArgs{old, next, q, mu};
    commit();
    publish();
    return true;
}
void set_desc(bool on) { g_desc_q.store(on, std::memory_order_relaxed); }
void apply_render_priority(bool on) {
    static const std::string mode = config_get_str("nvn", "render_priority", "highest");
    int p = THREAD_PRIORITY_NORMAL;
    if (on) p = mode == "time_critical" ? THREAD_PRIORITY_TIME_CRITICAL : mode == "above_normal" ? THREAD_PRIORITY_ABOVE_NORMAL : mode == "normal" ? THREAD_PRIORITY_NORMAL : THREAD_PRIORITY_HIGHEST;
    static int last = -100;
    if (p == last) return;
    last = p;
    SetThreadPriority(GetCurrentThread(), p);
    if (g_thread.joinable()) SetThreadPriority(g_thread.native_handle(), p);
}
void set_publish_batch(bool on) { g_pub_batch = on ? g_pub_batch_cfg : 1; }
void desc_stats(uint64_t* queued, uint64_t* direct) { *queued = g_uds_queued; *direct = g_uds_direct; }

bool queuing() { return g_ring && queue_here(); }
bool on_recorder() { return tl_recorder; }

// The recorder, inside vkCmdBeginRendering's packet (the hazard tracker's h_br): the packets after it up to the pass's vkCmdEndRendering (or
// the first one that decides the pass's barrier) are run dry (the tracker's hooks track them, no driver call), waiting for the render thread to queue them. Stops early when the render
// thread waits for the recorder (g_need).
void scan_pass() {
    ++g_scans;
    hazard::set_thread_mode(1);
    uint64_t pos = g_rec_next;
    bool done = false;
    for (;;) {
        const uint64_t head = g_head.load(std::memory_order_acquire);
        while (pos != head) {
            const size_t at = static_cast<size_t>(pos % kRingBytes);
            const PacketHdr* h = reinterpret_cast<const PacketHdr*>(g_ring + at);
            if (!h->fn) { pos += kRingBytes - at; continue; }
            const ExecFn fn = h->fn;
            if (is_haz(fn) || fn == g_er_exec) fn(g_ring + at + sizeof(PacketHdr));
            pos += h->size;
            if (fn == g_er_exec || !hazard::scan_open()) { done = true; break; }   // the pass's end, or its barrier is decided
        }
        if (done) break;
        if (g_need.load(std::memory_order_acquire)) { ++g_scan_stopped; break; }
        ++g_scan_waits;
        bool more = false;
        for (int i = 0; i < 4000; ++i) {
            _mm_pause();
            if (g_head.load(std::memory_order_acquire) != pos || g_need.load(std::memory_order_relaxed)) { more = true; break; }
        }
        if (!more) {
            g_sleeping.store(1, std::memory_order_seq_cst);
            uint64_t seen = pos;
            if (g_head.load(std::memory_order_seq_cst) == pos && !g_need.load(std::memory_order_seq_cst)) WaitOnAddress(&g_head, &seen, sizeof seen, 1);
            g_sleeping.store(0, std::memory_order_relaxed);
        }
    }
    hazard::set_thread_mode(0);
    g_scan_end = pos;
}
// Then (after the barrier, if any, and vkCmdBeginRendering): the looked-at packets for real, without tracking.
void replay_scanned() {
    hazard::set_thread_mode(2);
    uint64_t pos = g_rec_next;
    while (pos != g_scan_end) {
        const size_t at = static_cast<size_t>(pos % kRingBytes);
        const PacketHdr* h = reinterpret_cast<const PacketHdr*>(g_ring + at);
        if (!h->fn) { pos += kRingBytes - at; continue; }
        h->fn(g_ring + at + sizeof(PacketHdr));
        pos += h->size;
    }
    hazard::set_thread_mode(0);
    g_rec_next = g_scan_end;
}
void scan_stats(uint64_t* scans, uint64_t* stopped, uint64_t* waits) { *scans = g_scans; *stopped = g_scan_stopped; *waits = g_scan_waits; }

// A call made by the recorder in order with the queued ones (or now when nothing is queued).
struct PostArgs { void (*fn)(VkCommandBuffer); VkCommandBuffer cb; };
void x_post(const uint8_t* p) { const PostArgs& a = *reinterpret_cast<const PostArgs*>(p); a.fn(a.cb); }
void post(void (*fn)(VkCommandBuffer), VkCommandBuffer cb) {
    if (!queuing()) return fn(cb);
    new (reserve(&x_post, sizeof(PostArgs))) PostArgs{fn, cb};
    commit();
}
struct PostNArgs { void (*fn)(const uint64_t*, uint32_t); uint32_t n; };
void x_postn(const uint8_t* p) { const PostNArgs& a = *reinterpret_cast<const PostNArgs*>(p); a.fn(reinterpret_cast<const uint64_t*>(p + sizeof(PostNArgs)), a.n); }
bool post_words(void (*fn)(const uint64_t*, uint32_t), const uint64_t* w, uint32_t n) {
    if (!queuing() || n > 4096) return false;
    uint8_t* p = reserve(&x_postn, sizeof(PostNArgs) + 8 * size_t{n});
    new (p) PostNArgs{fn, n};
    std::memcpy(p + sizeof(PostNArgs), w, 8 * size_t{n});
    commit();
    return true;
}

bool ph_insert(VkCommandBuffer cb) {
    if (!queuing() || !o_pb2) return false;
    if (g_ph) ph_set(1);
    PhArgs* a = reinterpret_cast<PhArgs*>(reserve(&x_ph, sizeof(PhArgs)));
    a->cb = cb;
    new (&a->state) std::atomic<uint32_t>(0);
    g_ph = &a->state;
    ++g_ph_n;
    commit();
    publish();   // the recorder holds at the placeholder until it is decided
    return true;
}
void ph_resolve(bool barrier) { ph_set(barrier ? 1 : 2); }
bool ph_pending() { return g_ph != nullptr; }
void ph_stats(uint64_t* inserted, uint64_t* forced) { *inserted = g_ph_n; *forced = g_ph_forced; }

void raw_barrier(VkCommandBuffer cb, const VkDependencyInfo* di) {
    if (o_pb2_raw) o_pb2_raw(cb, di);
    else vkCmdPipelineBarrier2(cb, di);
}

void drain() {
    if (g_ring && g_active.load(std::memory_order_relaxed) && gpu::on_render_thread()) drain_now();
}

void stats(uint64_t* packets, uint64_t* drains, uint64_t* direct, uint64_t* spins) {
    *packets = g_packets; *drains = g_drains; *direct = g_direct; *spins = g_drain_spins;
}

std::string direct_top() {   // the calls that drained most often
    std::string s;
    for (int k = 0; k < 6; ++k) {
        int best = -1;
        for (int i = 0; i < g_ndirect; ++i)
            if (*g_direct_names[i].n && (best < 0 || *g_direct_names[i].n > *g_direct_names[best].n) && s.find(g_direct_names[i].name) == std::string::npos) best = i;
        if (best < 0) break;
        s += std::string(k ? ", " : "") + g_direct_names[best].name + " " + std::to_string(*g_direct_names[best].n);
    }
    return s;
}

void shutdown() {
    if (!g_thread.joinable()) return;
    drain_now();
    g_active.store(false);
    g_stop.store(true, std::memory_order_release);
    WakeByAddressSingle(&g_head);
    g_thread.join();
}

}  // namespace nxrt::nvn::be::cmdq
