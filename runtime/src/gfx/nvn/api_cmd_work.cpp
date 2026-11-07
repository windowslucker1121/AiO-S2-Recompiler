// SPDX-License-Identifier: GPL-3.0-or-later
// Recorded work commands (S4d, family cmd_work): draws, clears, copies, downsample, uniform buffer updates, counters,
// barriers, fences and events, compute dispatch, DrawTexture and discards. Each entry point appends one record to the CmdList
// that the command buffer is recording (nvn_cmd.h); nothing runs on the GPU here. Host objects that a payload points to
// (Texture, Sync, Event) are held by the list. Transform feedback commands are recorded (replayed with VK_EXT_transform_feedback).
// Debug groups are recorded when the back end has VK_EXT_debug_utils; debug markers and the ZCull save/restore record nothing.
// Payload layouts are the P_* structs of nvn_cmd.h. Only the NVN header and the NVN enum values are used for the arguments.
#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>

#include "nvn_cmd.h"
#include "nvn_gpu.h"
#include "nvn_impl.h"
#include "nxrt/log.h"

namespace nxrt::nvn {
using namespace be;
namespace {

constexpr uint32_t kMaxUniformUpdate = 65536;
constexpr uint32_t kNoIndexType = 0xFFFFFFFFu;     // P_DrawIndirect::type for DrawArrays

// A payload with every byte zero (the padding of a record is then deterministic).
template <class P>
P zeroed() {
    P p;
    std::memset(static_cast<void*>(&p), 0, sizeof p);
    return p;
}

// Locks the buffer and returns it when it is recording; `lk` keeps the lock until the caller returns. Null otherwise.
CommandBuffer* begin_record(NVNcommandBuffer* cmd, std::unique_lock<std::mutex>& lk) {
    CommandBuffer* cb = host_of<CommandBuffer>(cmd);
    if (!cb) return nullptr;
    lk = std::unique_lock<std::mutex>(cb->lock);
    return (cb->recording && cb->rec) ? cb : nullptr;
}

void set_view(const NVNtextureView* v, TextureViewInfo& out, uint32_t& has) {
    has = v ? 1u : 0u;
    out = v ? decode(v) : TextureViewInfo{};
}

void set_region(const NVNcopyRegion* r, P_CopyRegion& out, uint32_t& has) {
    has = r ? 1u : 0u;
    out = P_CopyRegion{};
    if (!r) return;
    out.x = r->xoffset;
    out.y = r->yoffset;
    out.z = r->zoffset;
    out.w = r->width;
    out.h = r->height;
    out.d = r->depth;
}

// Four 32-bit words of a colour (float, int or uint bit patterns, copied as they are).
void color_words(const void* src, uint32_t out[4]) { std::memcpy(out, src, 4 * sizeof(uint32_t)); }

// DrawTexture regions: four floats x0, y0, x1, y1 (a null region reads as zeros).
void region_floats(const NVNdrawTextureRegion* r, float out[4]) {
    if (r) std::memcpy(out, r, 4 * sizeof(float));
    else std::memset(out, 0, 4 * sizeof(float));
}

// ---- draws -----------------------------------------------------------------------------------------------------------------
void nvnCommandBufferDrawArrays(NVNcommandBuffer* cmd, NVNdrawPrimitive mode, int first, int count) {
    std::unique_lock<std::mutex> lk;
    CommandBuffer* cb = begin_record(cmd, lk);
    if (!cb) return;
    P_DrawArrays p = zeroed<P_DrawArrays>();
    p.mode = static_cast<uint32_t>(mode);
    p.first = static_cast<uint32_t>(first);
    p.count = static_cast<uint32_t>(count);
    p.base_instance = 0;
    p.instance_count = 1;
    cb->rec->emit(Op::DrawArrays, p);
    cb->rec->draw_count++;
}

void nvnCommandBufferDrawArraysInstanced(NVNcommandBuffer* cmd, NVNdrawPrimitive mode, int first, int count, int baseInstance, int instanceCount) {
    std::unique_lock<std::mutex> lk;
    CommandBuffer* cb = begin_record(cmd, lk);
    if (!cb) return;
    P_DrawArrays p = zeroed<P_DrawArrays>();
    p.mode = static_cast<uint32_t>(mode);
    p.first = static_cast<uint32_t>(first);
    p.count = static_cast<uint32_t>(count);
    p.base_instance = static_cast<uint32_t>(baseInstance);
    p.instance_count = static_cast<uint32_t>(instanceCount);
    cb->rec->emit(Op::DrawArrays, p);
    cb->rec->draw_count++;
}

void nvnCommandBufferDrawElements(NVNcommandBuffer* cmd, NVNdrawPrimitive mode, NVNindexType type, int count, NVNbufferAddress indexBuffer) {
    std::unique_lock<std::mutex> lk;
    CommandBuffer* cb = begin_record(cmd, lk);
    if (!cb) return;
    P_DrawElements p = zeroed<P_DrawElements>();
    p.mode = static_cast<uint32_t>(mode);
    p.type = static_cast<uint32_t>(type);
    p.count = static_cast<uint32_t>(count);
    p.instance_count = 1;
    p.base_vertex = 0;
    p.base_instance = 0;
    p.index_address = indexBuffer;
    cb->rec->emit(Op::DrawElements, p);
    cb->rec->draw_count++;
}

void nvnCommandBufferDrawElementsBaseVertex(NVNcommandBuffer* cmd, NVNdrawPrimitive mode, NVNindexType type, int count, NVNbufferAddress indexBuffer, int baseVertex) {
    std::unique_lock<std::mutex> lk;
    CommandBuffer* cb = begin_record(cmd, lk);
    if (!cb) return;
    P_DrawElements p = zeroed<P_DrawElements>();
    p.mode = static_cast<uint32_t>(mode);
    p.type = static_cast<uint32_t>(type);
    p.count = static_cast<uint32_t>(count);
    p.instance_count = 1;
    p.base_vertex = baseVertex;
    p.base_instance = 0;
    p.index_address = indexBuffer;
    cb->rec->emit(Op::DrawElements, p);
    cb->rec->draw_count++;
}

void nvnCommandBufferDrawElementsInstanced(NVNcommandBuffer* cmd, NVNdrawPrimitive mode, NVNindexType type, int count, NVNbufferAddress indexBuffer, int baseVertex, int baseInstance, int instanceCount) {
    std::unique_lock<std::mutex> lk;
    CommandBuffer* cb = begin_record(cmd, lk);
    if (!cb) return;
    P_DrawElements p = zeroed<P_DrawElements>();
    p.mode = static_cast<uint32_t>(mode);
    p.type = static_cast<uint32_t>(type);
    p.count = static_cast<uint32_t>(count);
    p.instance_count = static_cast<uint32_t>(instanceCount);
    p.base_vertex = baseVertex;
    p.base_instance = static_cast<uint32_t>(baseInstance);
    p.index_address = indexBuffer;
    cb->rec->emit(Op::DrawElements, p);
    cb->rec->draw_count++;
}

void nvnCommandBufferDrawArraysIndirect(NVNcommandBuffer* cmd, NVNdrawPrimitive mode, NVNbufferAddress indirectBuffer) {
    std::unique_lock<std::mutex> lk;
    CommandBuffer* cb = begin_record(cmd, lk);
    if (!cb) return;
    P_DrawIndirect p = zeroed<P_DrawIndirect>();
    p.mode = static_cast<uint32_t>(mode);
    p.type = kNoIndexType;
    p.multi = 0;
    p.max_count = 1;
    p.stride = 0;
    p.index_address = 0;
    p.indirect_address = indirectBuffer;
    p.count_address = 0;
    cb->rec->emit(Op::DrawIndirect, p);
    cb->rec->draw_count++;
}

void nvnCommandBufferDrawElementsIndirect(NVNcommandBuffer* cmd, NVNdrawPrimitive mode, NVNindexType type, NVNbufferAddress indexBuffer, NVNbufferAddress indirectBuffer) {
    std::unique_lock<std::mutex> lk;
    CommandBuffer* cb = begin_record(cmd, lk);
    if (!cb) return;
    P_DrawIndirect p = zeroed<P_DrawIndirect>();
    p.mode = static_cast<uint32_t>(mode);
    p.type = static_cast<uint32_t>(type);
    p.multi = 0;
    p.max_count = 1;
    p.stride = 0;
    p.index_address = indexBuffer;
    p.indirect_address = indirectBuffer;
    p.count_address = 0;
    cb->rec->emit(Op::DrawIndirect, p);
    cb->rec->draw_count++;
}

void nvnCommandBufferMultiDrawArraysIndirectCount(NVNcommandBuffer* cmd, NVNdrawPrimitive mode, NVNbufferAddress indirectBuffer, NVNbufferAddress parameterBuffer, int maxDrawCount, ptrdiff_t stride) {
    std::unique_lock<std::mutex> lk;
    CommandBuffer* cb = begin_record(cmd, lk);
    if (!cb) return;
    P_DrawIndirect p = zeroed<P_DrawIndirect>();
    p.mode = static_cast<uint32_t>(mode);
    p.type = kNoIndexType;
    p.multi = 1;
    p.max_count = static_cast<uint32_t>(maxDrawCount);
    p.stride = static_cast<int64_t>(stride);
    p.index_address = 0;
    p.indirect_address = indirectBuffer;
    p.count_address = parameterBuffer;
    cb->rec->emit(Op::DrawIndirect, p);
    cb->rec->draw_count++;
}

void nvnCommandBufferMultiDrawElementsIndirectCount(NVNcommandBuffer* cmd, NVNdrawPrimitive mode, NVNindexType type, NVNbufferAddress indexBuffer, NVNbufferAddress indirectBuffer, NVNbufferAddress parameterBuffer, int maxDrawCount, ptrdiff_t stride) {
    std::unique_lock<std::mutex> lk;
    CommandBuffer* cb = begin_record(cmd, lk);
    if (!cb) return;
    P_DrawIndirect p = zeroed<P_DrawIndirect>();
    p.mode = static_cast<uint32_t>(mode);
    p.type = static_cast<uint32_t>(type);
    p.multi = 1;
    p.max_count = static_cast<uint32_t>(maxDrawCount);
    p.stride = static_cast<int64_t>(stride);
    p.index_address = indexBuffer;
    p.indirect_address = indirectBuffer;
    p.count_address = parameterBuffer;
    cb->rec->emit(Op::DrawIndirect, p);
    cb->rec->draw_count++;
}

// ---- transform feedback ---------------------------------------------------------------------------------------------------------
// DrawTransformFeedback(mode, control): draws what the feedback section that used this control buffer wrote (the replay turns it into
// vkCmdDrawIndirectByteCountEXT over the section's byte counter).
void nvnCommandBufferDrawTransformFeedback(NVNcommandBuffer* cmd, NVNdrawPrimitive mode, NVNbufferAddress control) {
    std::unique_lock<std::mutex> lk;
    CommandBuffer* cb = begin_record(cmd, lk);
    if (!cb) return;
    P_DrawXfb p = zeroed<P_DrawXfb>();
    p.mode = static_cast<uint32_t>(mode);
    p.control = control;
    cb->rec->emit(Op::DrawXfb, p);
    cb->rec->draw_count++;
}
// ---- mesh draws (NVN2 task/mesh programs) -------------------------------------------------------------------------------------
namespace {
void record_mesh(NVNcommandBuffer* cmd, const P_DrawMesh& p) {
    std::unique_lock<std::mutex> lk;
    CommandBuffer* cb = begin_record(cmd, lk);
    if (!cb) return;
    cb->rec->emit(Op::DrawMeshTasks, p);
    cb->rec->draw_count++;
}
P_DrawMesh mesh_payload(uint32_t kind) { P_DrawMesh p = zeroed<P_DrawMesh>(); p.kind = kind; p.max_count = 1; return p; }
}  // namespace
void nvnCommandBufferDrawMeshTasks(NVNcommandBuffer* cmd, int first, int count) {
    P_DrawMesh p = mesh_payload(0); p.x = static_cast<uint32_t>(first); p.y = static_cast<uint32_t>(count); record_mesh(cmd, p);
}
void nvnCommandBufferDrawMeshTasks3D(NVNcommandBuffer* cmd, int x, int y, int z) {
    P_DrawMesh p = mesh_payload(1); p.x = static_cast<uint32_t>(x); p.y = static_cast<uint32_t>(y); p.z = static_cast<uint32_t>(z); record_mesh(cmd, p);
}
void nvnCommandBufferDrawMeshTasksIndirect(NVNcommandBuffer* cmd, NVNbufferAddress indirect) {
    P_DrawMesh p = mesh_payload(2); p.indirect = indirect; record_mesh(cmd, p);
}
void nvnCommandBufferDrawMeshTasks3DIndirect(NVNcommandBuffer* cmd, NVNbufferAddress indirect) {
    P_DrawMesh p = mesh_payload(3); p.indirect = indirect; record_mesh(cmd, p);
}
void nvnCommandBufferMultiDrawMeshTasksIndirect(NVNcommandBuffer* cmd, NVNbufferAddress indirect, int drawCount, int stride) {
    P_DrawMesh p = mesh_payload(2); p.indirect = indirect; p.multi = 1; p.max_count = static_cast<uint32_t>(drawCount); p.stride = stride; record_mesh(cmd, p);
}
void nvnCommandBufferMultiDrawMeshTasks3DIndirect(NVNcommandBuffer* cmd, NVNbufferAddress indirect, int drawCount, int stride) {
    P_DrawMesh p = mesh_payload(3); p.indirect = indirect; p.multi = 1; p.max_count = static_cast<uint32_t>(drawCount); p.stride = stride; record_mesh(cmd, p);
}
void nvnCommandBufferMultiDrawMeshTasksIndirectCount(NVNcommandBuffer* cmd, NVNbufferAddress indirect, NVNbufferAddress count, int maxCount, int stride) {
    P_DrawMesh p = mesh_payload(2); p.indirect = indirect; p.multi = 1; p.count_address = count; p.max_count = static_cast<uint32_t>(maxCount); p.stride = stride;
    record_mesh(cmd, p);
}
void nvnCommandBufferMultiDrawMeshTasks3DIndirectCount(NVNcommandBuffer* cmd, NVNbufferAddress indirect, NVNbufferAddress count, int maxCount, int stride) {
    P_DrawMesh p = mesh_payload(3); p.indirect = indirect; p.multi = 1; p.count_address = count; p.max_count = static_cast<uint32_t>(maxCount); p.stride = stride;
    record_mesh(cmd, p);
}

void record_xfb(NVNcommandBuffer* cmd, Op op, NVNbufferAddress control) {
    std::unique_lock<std::mutex> lk;
    CommandBuffer* cb = begin_record(cmd, lk);
    if (!cb) return;
    P_Counter p = zeroed<P_Counter>();
    p.address = control;
    cb->rec->emit(op, p);
}
void nvnCommandBufferBeginTransformFeedback(NVNcommandBuffer* cmd, NVNbufferAddress control) { record_xfb(cmd, Op::BeginXfb, control); }
void nvnCommandBufferEndTransformFeedback(NVNcommandBuffer* cmd, NVNbufferAddress control) { record_xfb(cmd, Op::EndXfb, control); }
void nvnCommandBufferPauseTransformFeedback(NVNcommandBuffer* cmd, NVNbufferAddress control) { record_xfb(cmd, Op::PauseXfb, control); }
void nvnCommandBufferResumeTransformFeedback(NVNcommandBuffer* cmd, NVNbufferAddress control) { record_xfb(cmd, Op::ResumeXfb, control); }

// ---- clears ------------------------------------------------------------------------------------------------------------------
// kind: 0 float, 1 int, 2 uint (the colour words are the bit patterns of the four values).
void record_clear_color(NVNcommandBuffer* cmd, int index, const void* color, int mask, uint32_t kind) {
    if (!color) return;
    std::unique_lock<std::mutex> lk;
    CommandBuffer* cb = begin_record(cmd, lk);
    if (!cb) return;
    P_ClearColor p = zeroed<P_ClearColor>();
    p.target = static_cast<uint32_t>(index);
    p.kind = kind;
    p.mask = static_cast<uint32_t>(mask);
    color_words(color, p.value);
    cb->rec->emit(Op::ClearColor, p);
}

void nvnCommandBufferClearColor(NVNcommandBuffer* cmd, int index, const float* color, int mask) {
    record_clear_color(cmd, index, color, mask, 0);
}
void nvnCommandBufferClearColori(NVNcommandBuffer* cmd, int index, const int* color, int mask) {
    record_clear_color(cmd, index, color, mask, 1);
}
void nvnCommandBufferClearColorui(NVNcommandBuffer* cmd, int index, const uint32_t* color, int mask) {
    record_clear_color(cmd, index, color, mask, 2);
}

void nvnCommandBufferClearDepthStencil(NVNcommandBuffer* cmd, float depth, NVNboolean depthMask, int stencil, int stencilMask) {
    std::unique_lock<std::mutex> lk;
    CommandBuffer* cb = begin_record(cmd, lk);
    if (!cb) return;
    P_ClearDepthStencil p = zeroed<P_ClearDepthStencil>();
    p.depth = depth;
    p.depth_mask = depthMask;
    p.stencil = stencil;
    p.stencil_mask = static_cast<uint32_t>(stencilMask);
    cb->rec->emit(Op::ClearDepthStencil, p);
}

void record_clear_texture(NVNcommandBuffer* cmd, const NVNtexture* tex, const NVNtextureView* view, const NVNcopyRegion* region,
                          const void* color, int mask, uint32_t kind) {
    Texture* t = host_of<Texture>(tex);
    if (!t || !color) return;
    std::unique_lock<std::mutex> lk;
    CommandBuffer* cb = begin_record(cmd, lk);
    if (!cb) return;
    P_ClearTexture p = zeroed<P_ClearTexture>();
    p.tex = t;
    cb->rec->hold(t);
    set_view(view, p.view, p.has_view);
    p.kind = kind;
    set_region(region, p.region, p.has_region);
    p.mask = static_cast<uint32_t>(mask);
    color_words(color, p.value);
    cb->rec->emit(Op::ClearTexture, p);
}

void nvnCommandBufferClearTexture(NVNcommandBuffer* cmd, const NVNtexture* tex, const NVNtextureView* view, const NVNcopyRegion* region, const float* color, int mask) {
    record_clear_texture(cmd, tex, view, region, color, mask, 0);
}
void nvnCommandBufferClearTexturei(NVNcommandBuffer* cmd, const NVNtexture* tex, const NVNtextureView* view, const NVNcopyRegion* region, const int* color, int mask) {
    record_clear_texture(cmd, tex, view, region, color, mask, 1);
}
void nvnCommandBufferClearTextureui(NVNcommandBuffer* cmd, const NVNtexture* tex, const NVNtextureView* view, const NVNcopyRegion* region, const uint32_t* color, int mask) {
    record_clear_texture(cmd, tex, view, region, color, mask, 2);
}

void nvnCommandBufferClearBuffer(NVNcommandBuffer* cmd, NVNbufferAddress address, size_t size, uint32_t value) {
    std::unique_lock<std::mutex> lk;
    CommandBuffer* cb = begin_record(cmd, lk);
    if (!cb) return;
    P_ClearBuffer p = zeroed<P_ClearBuffer>();
    p.address = address;
    p.size = static_cast<uint64_t>(size);
    p.value = value;
    cb->rec->emit(Op::ClearBuffer, p);
}

// ---- copies ------------------------------------------------------------------------------------------------------------------
void nvnCommandBufferCopyBufferToBuffer(NVNcommandBuffer* cmd, NVNbufferAddress src, NVNbufferAddress dst, size_t size, int flags) {
    std::unique_lock<std::mutex> lk;
    CommandBuffer* cb = begin_record(cmd, lk);
    if (!cb) return;
    P_CopyBufferToBuffer p = zeroed<P_CopyBufferToBuffer>();
    p.src = src;
    p.dst = dst;
    p.size = static_cast<uint64_t>(size);
    p.flags = static_cast<uint32_t>(flags);
    cb->rec->emit(Op::CopyBufferToBuffer, p);
}

void nvnCommandBufferCopyBufferToTexture(NVNcommandBuffer* cmd, NVNbufferAddress src, const NVNtexture* tex, const NVNtextureView* view, const NVNcopyRegion* region, int flags) {
    Texture* t = host_of<Texture>(tex);
    if (!t) return;
    std::unique_lock<std::mutex> lk;
    CommandBuffer* cb = begin_record(cmd, lk);
    if (!cb) return;
    P_CopyBufTex p = zeroed<P_CopyBufTex>();
    p.address = src;
    p.tex = t;
    cb->rec->hold(t);
    set_view(view, p.view, p.has_view);
    p.flags = static_cast<uint32_t>(flags);
    set_region(region, p.region, p.has_region);
    p.row_stride = cb->copy_row_stride;
    p.image_stride = cb->copy_image_stride;
    cb->rec->emit(Op::CopyBufferToTexture, p);
}

void nvnCommandBufferCopyTextureToBuffer(NVNcommandBuffer* cmd, const NVNtexture* tex, const NVNtextureView* view, const NVNcopyRegion* region, NVNbufferAddress dst, int flags) {
    Texture* t = host_of<Texture>(tex);
    if (!t) return;
    std::unique_lock<std::mutex> lk;
    CommandBuffer* cb = begin_record(cmd, lk);
    if (!cb) return;
    P_CopyBufTex p = zeroed<P_CopyBufTex>();
    p.address = dst;
    p.tex = t;
    cb->rec->hold(t);
    set_view(view, p.view, p.has_view);
    p.flags = static_cast<uint32_t>(flags);
    set_region(region, p.region, p.has_region);
    p.row_stride = cb->copy_row_stride;
    p.image_stride = cb->copy_image_stride;
    cb->rec->emit(Op::CopyTextureToBuffer, p);
}

void nvnCommandBufferCopyTextureToTexture(NVNcommandBuffer* cmd, const NVNtexture* src, const NVNtextureView* srcView, const NVNcopyRegion* srcRegion,
                                          const NVNtexture* dst, const NVNtextureView* dstView, const NVNcopyRegion* dstRegion, int flags) {
    Texture* s = host_of<Texture>(src);
    Texture* d = host_of<Texture>(dst);
    if (!s || !d) return;
    std::unique_lock<std::mutex> lk;
    CommandBuffer* cb = begin_record(cmd, lk);
    if (!cb) return;
    P_CopyTexTex p = zeroed<P_CopyTexTex>();
    p.src = s;
    p.dst = d;
    cb->rec->hold(s);
    cb->rec->hold(d);
    set_view(srcView, p.src_view, p.has_src_view);
    set_region(srcRegion, p.src_region, p.has_src_region);
    set_view(dstView, p.dst_view, p.has_dst_view);
    set_region(dstRegion, p.dst_region, p.has_dst_region);
    p.flags = static_cast<uint32_t>(flags);
    cb->rec->emit(Op::CopyTextureToTexture, p);
}

// ---- downsample --------------------------------------------------------------------------------------------------------------
// Tiled and plain downsample share one record (P_Downsample has no field that tells them apart).
void record_downsample(NVNcommandBuffer* cmd, const NVNtexture* src, const NVNtextureView* srcView, const NVNtexture* dst, const NVNtextureView* dstView, bool with_views) {
    Texture* s = host_of<Texture>(src);
    Texture* d = host_of<Texture>(dst);
    if (!s || !d) return;
    std::unique_lock<std::mutex> lk;
    CommandBuffer* cb = begin_record(cmd, lk);
    if (!cb) return;
    P_Downsample p = zeroed<P_Downsample>();
    p.src = s;
    p.dst = d;
    cb->rec->hold(s);
    cb->rec->hold(d);
    if (with_views) {
        p.src_view = srcView ? decode(srcView) : TextureViewInfo{};
        p.dst_view = dstView ? decode(dstView) : TextureViewInfo{};
    }
    p.has_views = with_views ? 1u : 0u;
    cb->rec->emit(Op::Downsample, p);
}

void nvnCommandBufferDownsample(NVNcommandBuffer* cmd, const NVNtexture* src, const NVNtexture* dst) {
    record_downsample(cmd, src, nullptr, dst, nullptr, false);
}
void nvnCommandBufferTiledDownsample(NVNcommandBuffer* cmd, const NVNtexture* src, const NVNtexture* dst) {
    record_downsample(cmd, src, nullptr, dst, nullptr, false);
}
void nvnCommandBufferDownsampleTextureView(NVNcommandBuffer* cmd, const NVNtexture* src, const NVNtextureView* srcView, const NVNtexture* dst, const NVNtextureView* dstView) {
    record_downsample(cmd, src, srcView, dst, dstView, true);
}
void nvnCommandBufferTiledDownsampleTextureView(NVNcommandBuffer* cmd, const NVNtexture* src, const NVNtextureView* srcView, const NVNtexture* dst, const NVNtextureView* dstView) {
    record_downsample(cmd, src, srcView, dst, dstView, true);
}

// ---- uniform buffer update ----------------------------------------------------------------------------------------------------
// The data is copied into the record (blob), so the game may reuse its source right away. Sizes above 64 KiB are clamped.
void nvnCommandBufferUpdateUniformBuffer(NVNcommandBuffer* cmd, NVNbufferAddress address, size_t bufferSize, ptrdiff_t offset, size_t size, const void* data) {
    if (!data) return;
    uint32_t n = static_cast<uint32_t>(size);
    if (size > kMaxUniformUpdate) {
        NXLOG_W("nvn", "UpdateUniformBuffer of %llu bytes clamped to %u", static_cast<unsigned long long>(size), kMaxUniformUpdate);
        n = kMaxUniformUpdate;
    }
    std::unique_lock<std::mutex> lk;
    CommandBuffer* cb = begin_record(cmd, lk);
    if (!cb) return;
    P_UpdateUniformBuffer p = zeroed<P_UpdateUniformBuffer>();
    p.address = address;
    p.buffer_size = static_cast<uint64_t>(bufferSize);
    p.offset = static_cast<int64_t>(offset);
    p.size = n;
    cb->rec->emit(Op::UpdateUniformBuffer, p, data, n);
}

// ---- counters and values -------------------------------------------------------------------------------------------------------
void nvnCommandBufferReportCounter(NVNcommandBuffer* cmd, NVNcounterType type, NVNbufferAddress address) {
    std::unique_lock<std::mutex> lk;
    CommandBuffer* cb = begin_record(cmd, lk);
    if (!cb) return;
    P_Counter p = zeroed<P_Counter>();
    p.type = static_cast<uint32_t>(type);
    p.address = address;
    cb->rec->emit(Op::ReportCounter, p);
}

void nvnCommandBufferResetCounter(NVNcommandBuffer* cmd, NVNcounterType type) {
    std::unique_lock<std::mutex> lk;
    CommandBuffer* cb = begin_record(cmd, lk);
    if (!cb) return;
    P_Counter p = zeroed<P_Counter>();
    p.type = static_cast<uint32_t>(type);
    p.address = 0;
    cb->rec->emit(Op::ResetCounter, p);
}

void nvnCommandBufferReportValue(NVNcommandBuffer* cmd, uint32_t value, NVNbufferAddress address) {
    std::unique_lock<std::mutex> lk;
    CommandBuffer* cb = begin_record(cmd, lk);
    if (!cb) return;
    P_ReportValue p = zeroed<P_ReportValue>();
    p.value = value;
    p.address = address;
    cb->rec->emit(Op::ReportValue, p);
}

// ---- barriers, fences, events --------------------------------------------------------------------------------------------------
void nvnCommandBufferBarrier(NVNcommandBuffer* cmd, int bits) {
    std::unique_lock<std::mutex> lk;
    CommandBuffer* cb = begin_record(cmd, lk);
    if (!cb) return;
    P_Barrier p = zeroed<P_Barrier>();
    p.bits = static_cast<uint32_t>(bits);
    cb->rec->emit(Op::Barrier, p);
}

void nvnCommandBufferFenceSync(NVNcommandBuffer* cmd, NVNsync* sync, NVNsyncCondition condition, int flags) {
    Sync* s = host_of<Sync>(sync);
    if (!s) return;   // the game has not initialised this sync object
    std::unique_lock<std::mutex> lk;
    CommandBuffer* cb = begin_record(cmd, lk);
    if (!cb) return;
    P_Sync p = zeroed<P_Sync>();
    p.sync = s;
    cb->rec->hold(s);
    p.condition = static_cast<uint32_t>(condition);
    p.flags = static_cast<uint32_t>(flags);
    cb->rec->emit(Op::FenceSync, p);
}

void nvnCommandBufferWaitSync(NVNcommandBuffer* cmd, const NVNsync* sync) {
    Sync* s = host_of<Sync>(sync);
    if (!s) return;
    std::unique_lock<std::mutex> lk;
    CommandBuffer* cb = begin_record(cmd, lk);
    if (!cb) return;
    P_Sync p = zeroed<P_Sync>();
    p.sync = s;
    cb->rec->hold(s);
    cb->rec->emit(Op::WaitSync, p);
}

void nvnCommandBufferSignalEvent(NVNcommandBuffer* cmd, const NVNevent* event, NVNeventSignalMode mode, NVNeventSignalLocation location, int index, uint32_t value) {
    Event* e = host_of<Event>(event);
    if (!e) return;
    std::unique_lock<std::mutex> lk;
    CommandBuffer* cb = begin_record(cmd, lk);
    if (!cb) return;
    P_Event p = zeroed<P_Event>();
    p.event = e;
    cb->rec->hold(e);
    p.mode = static_cast<uint32_t>(mode);
    p.location = static_cast<uint32_t>(location);
    p.index = static_cast<uint32_t>(index);
    p.value = value;
    cb->rec->emit(Op::SignalEvent, p);
}

void nvnCommandBufferWaitEvent(NVNcommandBuffer* cmd, const NVNevent* event, NVNeventWaitMode waitMode, uint32_t value) {
    Event* e = host_of<Event>(event);
    if (!e) return;
    std::unique_lock<std::mutex> lk;
    CommandBuffer* cb = begin_record(cmd, lk);
    if (!cb) return;
    P_Event p = zeroed<P_Event>();
    p.event = e;
    cb->rec->hold(e);
    p.mode = static_cast<uint32_t>(waitMode);
    p.value = value;
    cb->rec->emit(Op::WaitEvent, p);
}

// ---- compute, DrawTexture, discards ---------------------------------------------------------------------------------------------
void nvnCommandBufferDispatchCompute(NVNcommandBuffer* cmd, int x, int y, int z) {
    std::unique_lock<std::mutex> lk;
    CommandBuffer* cb = begin_record(cmd, lk);
    if (!cb) return;
    P_Dispatch p = zeroed<P_Dispatch>();
    p.x = static_cast<uint32_t>(x);
    p.y = static_cast<uint32_t>(y);
    p.z = static_cast<uint32_t>(z);
    cb->rec->emit(Op::Dispatch, p);
}

void nvnCommandBufferDispatchComputeIndirect(NVNcommandBuffer* cmd, NVNbufferAddress address) {
    std::unique_lock<std::mutex> lk;
    CommandBuffer* cb = begin_record(cmd, lk);
    if (!cb) return;
    P_DispatchIndirect p = zeroed<P_DispatchIndirect>();
    p.address = address;
    cb->rec->emit(Op::DispatchIndirect, p);
}

void nvnCommandBufferDrawTexture(NVNcommandBuffer* cmd, NVNtextureHandle handle, const NVNdrawTextureRegion* dst, const NVNdrawTextureRegion* src) {
    std::unique_lock<std::mutex> lk;
    CommandBuffer* cb = begin_record(cmd, lk);
    if (!cb) return;
    P_DrawTexture p = zeroed<P_DrawTexture>();
    p.handle = handle;
    region_floats(dst, p.dst);
    region_floats(src, p.src);
    cb->rec->emit(Op::DrawTexture, p);
    cb->rec->draw_count++;
}

void nvnCommandBufferDiscardColor(NVNcommandBuffer* cmd, int index) {
    std::unique_lock<std::mutex> lk;
    CommandBuffer* cb = begin_record(cmd, lk);
    if (!cb) return;
    P_U32 p = zeroed<P_U32>();
    p.v = static_cast<uint32_t>(index);
    cb->rec->emit(Op::DiscardColor, p);
}

void nvnCommandBufferDiscardDepthStencil(NVNcommandBuffer* cmd) {
    std::unique_lock<std::mutex> lk;
    CommandBuffer* cb = begin_record(cmd, lk);
    if (!cb) return;
    cb->rec->emit_empty(Op::DiscardDepthStencil);
}

void nvnCommandBufferResolveDepthBuffer(NVNcommandBuffer* cmd) {
    std::unique_lock<std::mutex> lk;
    CommandBuffer* cb = begin_record(cmd, lk);
    if (!cb) return;
    cb->rec->emit_empty(Op::ResolveDepth);
}

// ---- debug groups: command buffer labels (VK_EXT_debug_utils), recorded only when the back end shows them -----------------------------
// The name is copied (the Static variants promise a string that outlives the call, the Dynamic ones do not). The Graphics / Compute
// variants are the NVN2 per-engine forms; the engine makes no difference to a label.
void push_debug_group(NVNcommandBuffer* cmd, uint32_t id, const char* name) {
    if (!name || !gpu::debug_labels()) return;
    std::unique_lock<std::mutex> lk;
    CommandBuffer* cb = begin_record(cmd, lk);
    if (!cb) return;
    P_DebugGroup p = zeroed<P_DebugGroup>();
    p.id = id;
    p.length = static_cast<uint32_t>(std::min<size_t>(std::strlen(name), 255));
    char text[256];
    std::memcpy(text, name, p.length);
    text[p.length] = 0;
    cb->rec->emit(Op::PushDebugGroup, p, text, p.length + 1);
}
void pop_debug_group(NVNcommandBuffer* cmd, uint32_t id) {
    if (!gpu::debug_labels()) return;
    std::unique_lock<std::mutex> lk;
    CommandBuffer* cb = begin_record(cmd, lk);
    if (!cb) return;
    P_DebugGroup p = zeroed<P_DebugGroup>();
    p.id = id;
    cb->rec->emit(Op::PopDebugGroup, p);
}

// ---- ZCull: accepted, nothing recorded ------------------------------------------------------------------------------------------
void nvnCommandBufferSaveZCullData(NVNcommandBuffer*, NVNbufferAddress, size_t) {}
void nvnCommandBufferRestoreZCullData(NVNcommandBuffer*, NVNbufferAddress, size_t) {}
void nvnCommandBufferPushDebugGroupStatic(NVNcommandBuffer* c, uint32_t id, const char* name) { push_debug_group(c, id, name); }
void nvnCommandBufferPushDebugGroupDynamic(NVNcommandBuffer* c, uint32_t id, const char* name) { push_debug_group(c, id, name); }
void nvnCommandBufferPushDebugGroup(NVNcommandBuffer* c, uint32_t id, const char* name) { push_debug_group(c, id, name); }
void nvnCommandBufferPopDebugGroup(NVNcommandBuffer* c) { pop_debug_group(c, 0); }
void nvnCommandBufferPopDebugGroupId(NVNcommandBuffer* c, uint32_t id) { pop_debug_group(c, id); }
// NVN2 only (not in the NVN1 table; the game fetches them by name): (cmd, id, name) and (cmd, id).
void nvnCommandBufferPushDebugGroupGraphics(NVNcommandBuffer* c, uint32_t id, const char* name) { push_debug_group(c, id, name); }
void nvnCommandBufferPushDebugGroupCompute(NVNcommandBuffer* c, uint32_t id, const char* name) { push_debug_group(c, id, name); }
void nvnCommandBufferPopDebugGroupGraphics(NVNcommandBuffer* c, uint32_t id) { pop_debug_group(c, id); }
void nvnCommandBufferPopDebugGroupCompute(NVNcommandBuffer* c, uint32_t id) { pop_debug_group(c, id); }
void nvnCommandBufferInsertDebugMarkerStatic(NVNcommandBuffer*, uint32_t, const char*) {}
void nvnCommandBufferInsertDebugMarkerDynamic(NVNcommandBuffer*, uint32_t, const char*) {}
void nvnCommandBufferInsertDebugMarker(NVNcommandBuffer*, const char*) {}
// NVN 55 names the game asks for and never calls (signatures assumed): accepted, nothing to do.
void nvnCommandBufferInsertCheckpointStatic(NVNcommandBuffer*, uint32_t, const char*) {}
void nvnCommandBufferInsertCheckpointDynamic(NVNcommandBuffer*, uint32_t, const char*) {}
void nvnCommandBufferOverrideSubtileSize(NVNcommandBuffer*, int, int) {}

}  // namespace

void register_api_cmd_work() {
    NVN_REG(nvnCommandBufferDrawArrays);
    NVN_REG(nvnCommandBufferDrawArraysInstanced);
    NVN_REG(nvnCommandBufferDrawElements);
    NVN_REG(nvnCommandBufferDrawElementsBaseVertex);
    NVN_REG(nvnCommandBufferDrawElementsInstanced);
    NVN_REG(nvnCommandBufferDrawArraysIndirect);
    NVN_REG(nvnCommandBufferDrawElementsIndirect);
    NVN_REG(nvnCommandBufferMultiDrawArraysIndirectCount);
    NVN_REG(nvnCommandBufferMultiDrawElementsIndirectCount);
    NVN_REG(nvnCommandBufferDrawTransformFeedback);
    NVN2_REG(nvnCommandBufferDrawMeshTasks);
    NVN2_REG(nvnCommandBufferDrawMeshTasks3D);
    NVN2_REG(nvnCommandBufferDrawMeshTasksIndirect);
    NVN2_REG(nvnCommandBufferDrawMeshTasks3DIndirect);
    NVN2_REG(nvnCommandBufferMultiDrawMeshTasksIndirect);
    NVN2_REG(nvnCommandBufferMultiDrawMeshTasks3DIndirect);
    NVN2_REG(nvnCommandBufferMultiDrawMeshTasksIndirectCount);
    NVN2_REG(nvnCommandBufferMultiDrawMeshTasks3DIndirectCount);
    NVN_REG(nvnCommandBufferBeginTransformFeedback);
    NVN_REG(nvnCommandBufferEndTransformFeedback);
    NVN_REG(nvnCommandBufferPauseTransformFeedback);
    NVN_REG(nvnCommandBufferResumeTransformFeedback);
    NVN_REG(nvnCommandBufferClearColor);
    NVN_REG(nvnCommandBufferClearColori);
    NVN_REG(nvnCommandBufferClearColorui);
    NVN_REG(nvnCommandBufferClearDepthStencil);
    NVN_REG(nvnCommandBufferClearTexture);
    NVN_REG(nvnCommandBufferClearTexturei);
    NVN_REG(nvnCommandBufferClearTextureui);
    NVN_REG(nvnCommandBufferClearBuffer);
    NVN_REG(nvnCommandBufferCopyBufferToBuffer);
    NVN_REG(nvnCommandBufferCopyBufferToTexture);
    NVN_REG(nvnCommandBufferCopyTextureToBuffer);
    NVN_REG(nvnCommandBufferCopyTextureToTexture);
    NVN_REG(nvnCommandBufferDownsample);
    NVN_REG(nvnCommandBufferTiledDownsample);
    NVN_REG(nvnCommandBufferDownsampleTextureView);
    NVN_REG(nvnCommandBufferTiledDownsampleTextureView);
    NVN_REG(nvnCommandBufferUpdateUniformBuffer);
    NVN_REG(nvnCommandBufferReportCounter);
    NVN_REG(nvnCommandBufferResetCounter);
    NVN_REG(nvnCommandBufferReportValue);
    NVN_REG(nvnCommandBufferBarrier);
    NVN_REG(nvnCommandBufferFenceSync);
    NVN_REG(nvnCommandBufferWaitSync);
    NVN_REG(nvnCommandBufferSignalEvent);
    NVN_REG(nvnCommandBufferWaitEvent);
    NVN_REG(nvnCommandBufferDispatchCompute);
    NVN_REG(nvnCommandBufferDispatchComputeIndirect);
    NVN_REG(nvnCommandBufferDrawTexture);
    NVN_REG(nvnCommandBufferDiscardColor);
    NVN_REG(nvnCommandBufferDiscardDepthStencil);
    NVN_REG(nvnCommandBufferResolveDepthBuffer);
    NVN_REG(nvnCommandBufferSaveZCullData);
    NVN_REG(nvnCommandBufferRestoreZCullData);
    NVN_REG(nvnCommandBufferPushDebugGroupStatic);
    NVN_REG(nvnCommandBufferPushDebugGroupDynamic);
    NVN_REG(nvnCommandBufferPushDebugGroup);
    NVN_REG(nvnCommandBufferPopDebugGroup);
    NVN_REG(nvnCommandBufferPopDebugGroupId);
    NVN2_REG(nvnCommandBufferPushDebugGroupGraphics);
    NVN2_REG(nvnCommandBufferPushDebugGroupCompute);
    NVN2_REG(nvnCommandBufferPopDebugGroupGraphics);
    NVN2_REG(nvnCommandBufferPopDebugGroupCompute);
    NVN_REG(nvnCommandBufferInsertDebugMarkerStatic);
    NVN_REG(nvnCommandBufferInsertDebugMarkerDynamic);
    NVN_REG(nvnCommandBufferInsertDebugMarker);
    NVN_REG(nvnCommandBufferInsertCheckpointStatic);
    NVN_REG(nvnCommandBufferInsertCheckpointDynamic);
    NVN_REG(nvnCommandBufferOverrideSubtileSize);
}

}  // namespace nxrt::nvn
