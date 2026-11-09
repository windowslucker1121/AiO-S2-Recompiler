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

#include <atomic>
#include <cstdint>
#include <cstring>
#include <new>
#include <string>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>

#include <volk.h>

#include "nvn_gpu_internal.h"
#include "nxrt/config.h"
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

void recorder_loop() {
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
            h->fn(g_ring + at + sizeof(PacketHdr));
            tail += h->size;
        }
        g_tail.store(tail, std::memory_order_release);
    }
}

// Room for a packet of `payload` bytes; the returned pointer is the payload. commit() publishes it.
uint64_t g_wr = 0;   // producer's write position (== g_head between packets)
uint8_t* reserve(ExecFn fn, size_t payload) {
    const size_t size = (sizeof(PacketHdr) + payload + 15) & ~size_t{15};
    for (;;) {
        size_t at = static_cast<size_t>(g_wr % kRingBytes);
        const size_t need = (at + size > kRingBytes) ? (kRingBytes - at) + size : size;
        if (g_wr + need - g_tail.load(std::memory_order_acquire) <= kRingBytes) {
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
        _mm_pause();   // full: the recorder makes room
    }
}
void commit() {
    g_head.store(g_wr, std::memory_order_release);
    ++g_packets;
    if (g_sleeping.load(std::memory_order_seq_cst)) WakeByAddressSingle(&g_head);
}

void drain_now() {
    if (g_tail.load(std::memory_order_acquire) == g_wr) return;
    ++g_drains;
    if (g_sleeping.load(std::memory_order_seq_cst)) WakeByAddressSingle(&g_head);
    while (g_tail.load(std::memory_order_acquire) != g_wr) { _mm_pause(); ++g_drain_spins; }
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
    static void install() { if (*Slot && !orig) { orig = *Slot; *Slot = &call; } }
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

template <typename P> void hook(P& slot, P& orig, P fn) { if (slot && !orig) { orig = slot; slot = fn; } }

void install_all() {
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
    // everything else the replay records: drained first, then called directly
    NXQ_DIRECT(vkCmdPipelineBarrier); NXQ_DIRECT(vkCmdCopyImageToBuffer); NXQ_DIRECT(vkCmdCopyImageToBuffer2); NXQ_DIRECT(vkCmdCopyBufferToImage);
    NXQ_DIRECT(vkCmdCopyBufferToImage2); NXQ_DIRECT(vkCmdCopyImage); NXQ_DIRECT(vkCmdBlitImage); NXQ_DIRECT(vkCmdResolveImage);
    NXQ_DIRECT(vkCmdClearColorImage); NXQ_DIRECT(vkCmdClearDepthStencilImage); NXQ_DIRECT(vkCmdBindVertexBuffers);
    NXQ_DIRECT(vkCmdBeginTransformFeedbackEXT); NXQ_DIRECT(vkCmdEndTransformFeedbackEXT); NXQ_DIRECT(vkCmdBindTransformFeedbackBuffersEXT);
    hook(vkCmdBeginDebugUtilsLabelEXT, o_bdl, &h_bdl); NXQ_DIRECT(vkCmdSetFragmentShadingRateKHR); NXQ_DIRECT(vkCmdSetCheckpointNV);
    NXQ_DIRECT(vkCmdPushDescriptorSetKHR); NXQ_DIRECT(vkCmdSetVertexInputEXT); NXQ_DIRECT(vkCmdSetSampleMaskEXT);
    NXQ_DIRECT(vkCmdSetColorBlendEnableEXT); NXQ_DIRECT(vkCmdSetColorBlendEquationEXT); NXQ_DIRECT(vkCmdSetColorWriteMaskEXT);
    NXQ_DIRECT(vkCmdSetPolygonModeEXT); NXQ_DIRECT(vkCmdSetDepthClampEnableEXT); NXQ_DIRECT(vkCmdSetLogicOpEnableEXT);
    NXQ_DIRECT(vkCmdSetAlphaToCoverageEnableEXT); NXQ_DIRECT(vkCmdSetRasterizationSamplesEXT); NXQ_DIRECT(vkCmdSetDepthClipEnableEXT);
    NXQ_DIRECT(vkCmdExecuteCommands);
    // the command buffers, their pools and the queues as a whole
    NXQ_DIRECT(vkBeginCommandBuffer); NXQ_DIRECT(vkEndCommandBuffer); NXQ_DIRECT(vkResetCommandBuffer); NXQ_DIRECT(vkResetCommandPool);
    NXQ_DIRECT(vkAllocateCommandBuffers); NXQ_DIRECT(vkFreeCommandBuffers); NXQ_DIRECT(vkTrimCommandPool); NXQ_DIRECT(vkDestroyCommandPool);
    NXQ_DIRECT(vkQueueSubmit); NXQ_DIRECT(vkQueueSubmit2); NXQ_DIRECT(vkQueueWaitIdle); NXQ_DIRECT(vkDeviceWaitIdle); NXQ_DIRECT(vkQueuePresentKHR);
}

}  // namespace

bool enabled() {
    static const bool on = config_get_bool("nvn", "cmd_thread", false);
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
