// SPDX-License-Identifier: GPL-3.0-or-later
// The smaller NVN2 entry points (plan goal 20; layouts and state in nvn_small.h). All of them are NVN2-only names (NVN2_REG): push constants,
// the shading rate image and palette, the exclusive scissor, the conservative raster mode, ProgramBuilder, device message buffers and dirty tile
// maps. (The fast-clear registry is in api_device.cpp, the conservative dilate in api_cmd_state.cpp, InitializeFromBuilder in api_program.cpp:
// they replace NVN1 names or need those files' objects.) Command buffer functions only record; the replay (nvn_replay.cpp) applies the records.
//
// Argument layouts are from the SDK (work/ida/sdk/sdk.elf, image offsets as in nvn2_procs.tsv), cited at each group. The ray-tracing fields of the
// ProgramBuilder (modules, recursion depths, flags) belong to the ray-tracing family and are not registered here.
#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

#include "nvn_cmd.h"
#include "nvn_gpu.h"
#include "nvn_impl.h"
#include "nxrt/log.h"

namespace nxrt::nvn {
using namespace be;
namespace {

template <class T>
T rd(const void* base, size_t off) {
    T v;
    std::memcpy(&v, static_cast<const uint8_t*>(base) + off, sizeof v);
    return v;
}
template <class T>
void wr(void* base, size_t off, T v) {
    std::memcpy(static_cast<uint8_t*>(base) + off, &v, sizeof v);
}
inline uint64_t bits_of(const void* p) { return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(p)); }
template <class T>
inline T* ptr_of(uint64_t v) { return reinterpret_cast<T*>(static_cast<uintptr_t>(v)); }

// Runs fn(list) with the command buffer locked, while a list is being recorded.
template <class F>
void with_rec(NVNcommandBuffer* c, F&& fn) {
    CommandBuffer* cb = host_of<CommandBuffer>(c);
    if (!cb) return;
    std::lock_guard<std::mutex> lk(cb->lock);
    if (!cb->recording || !cb->rec) return;
    fn(*cb->rec);
}

void emit_small(NVNcommandBuffer* c, SmallKind kind, uint32_t a, uint32_t b, uint32_t cc, uint64_t addr, const void* blob = nullptr, uint32_t blob_bytes = 0) {
    const P_Small p{static_cast<uint32_t>(kind), a, b, cc, addr};
    with_rec(c, [&](CmdList& l) { l.emit(Op::Small, p, blob, blob_bytes); });
}

void log_once(std::atomic<bool>& flag, const char* msg) {
    if (!flag.exchange(true)) NXLOG_I("nvn", "%s (first)", msg);
}

// ---- push constants -----------------------------------------------------------------------------------------------------------------
// sdk 0x5F85E0 (graphics) / 0x5F8AA0 (compute): (cmd, stage, byte offset, byte size, data). The size is clamped to 256 and cut to words, under 4
// bytes does nothing; the graphics method keeps the stage in 3 bits (a2 & 7) and the offset as a byte (a3 is `unsigned __int8`), the compute one
// puts stage << 6 words + offset >> 2 into a register address. The Deferred forms (0x5F8740 / 0x5F8E10) record the same fields with the data's
// GPU address instead of the data (the queue reads it at submit), so the replay reads it then.
// The hardware sends the words to a constant area per stage that the shaders read. Compute programs that push constants read them from
// their c[0] table, after its 0x10-byte header (ShaderControl::c0_push; the replay builds that c[0] per dispatch from SmallState::push_cs).
// Graphics push areas are kept per stage (SmallState::push) but no translated graphics shader reads one yet. Compute records carry
// kPushCompute in the stage word.
void push_inline(NVNcommandBuffer* c, uint32_t stage, int offset, int size, const void* data) {
    if (!data || size < 4) return;
    const uint32_t n = std::min<uint32_t>(static_cast<uint32_t>(size), kPushAreaBytes) & ~3u;
    emit_small(c, SmallKind::PushConstants, stage, static_cast<uint32_t>(offset), n, 0, data, n);
    static std::atomic<bool> seen_gfx{false}, seen_cs{false};
    if (stage & kPushCompute) log_once(seen_cs, "NVN2 compute push constants recorded: compute programs read them from their c[0] table");
    else log_once(seen_gfx, "NVN2 graphics push constants recorded: kept per stage in the replay state, no translated graphics shader reads them yet");
}
void push_deferred(NVNcommandBuffer* c, uint32_t stage, int offset, int size, uint64_t address) {
    if (!address || size < 4) return;
    emit_small(c, SmallKind::PushConstantsDeferred, stage, static_cast<uint32_t>(offset), std::min<uint32_t>(static_cast<uint32_t>(size), kPushAreaBytes) & ~3u, address);
}
void nvnCommandBufferPushGraphicsConstants(NVNcommandBuffer* c, int stage, int offset, int size, const void* data) {
    push_inline(c, static_cast<uint32_t>(stage) & 7u, offset, size, data);
}
void nvnCommandBufferPushGraphicsConstantsDeferred(NVNcommandBuffer* c, int stage, int offset, int size, NVNbufferAddress address) {
    push_deferred(c, static_cast<uint32_t>(stage) & 7u, offset, size, address);
}
void nvnCommandBufferPushComputeConstants(NVNcommandBuffer* c, int stage, int offset, int size, const void* data) {
    push_inline(c, (static_cast<uint32_t>(stage) & 0xFFu) | kPushCompute, offset, size, data);
}
void nvnCommandBufferPushComputeConstantsDeferred(NVNcommandBuffer* c, int stage, int offset, int size, NVNbufferAddress address) {
    push_deferred(c, (static_cast<uint32_t>(stage) & 0xFFu) | kPushCompute, offset, size, address);
}

// ---- variable rate shading ----------------------------------------------------------------------------------------------------------
// sdk 0x621200 BindShadingRateImage(cmd, texture, view): a null texture unbinds. 0x620D80 ShadingRateImagePalette(cmd, index, count, entries):
// index & 0xF, up to 16 four-bit entries (unset ones = 5, one invocation per pixel), a null array disables the palette. 0x621340
// SetShadingRatePaletteMode(cmd, mode). The replay turns a bound image with a one-valued palette into the KHR pipeline fragment shading rate.
void nvnCommandBufferBindShadingRateImage(NVNcommandBuffer* c, const NVNtexture* texture, const NVNtextureView*) {
    emit_small(c, SmallKind::BindShadingRateImage, texture ? 1u : 0u, 0, 0, bits_of(texture));
}
void nvnCommandBufferShadingRateImagePalette(NVNcommandBuffer* c, int index, int count, const int* entries) {
    const uint32_t n = entries ? static_cast<uint32_t>(std::clamp(count, 0, 16)) : 0u;
    emit_small(c, SmallKind::ShadingRatePalette, static_cast<uint32_t>(index), n, entries ? 1u : 0u, 0, entries, n * 4);
}
void nvnCommandBufferSetShadingRatePaletteMode(NVNcommandBuffer* c, int mode) {
    emit_small(c, SmallKind::ShadingRatePaletteMode, static_cast<uint32_t>(mode), 0, 0, 0);
}

// ---- exclusive scissor --------------------------------------------------------------------------------------------------------------
// sdk 0x620EA0 EnableExclusiveScissor(cmd, enable) writes the enable of all 16 viewports at once; 0x620F40 SetExclusiveScissor(cmd, x, y, w, h)
// is SetExclusiveScissors(cmd, 0, 1, {x, y, w, h}); 0x620F80 SetExclusiveScissors(cmd, first, count, rects) clamps each rectangle to 0..0xFFFE.
void nvnCommandBufferEnableExclusiveScissor(NVNcommandBuffer* c, NVNboolean enable) {
    emit_small(c, SmallKind::ExclusiveScissorEnable, enable ? 1u : 0u, 0, 0, 0);
}
void nvnCommandBufferSetExclusiveScissors(NVNcommandBuffer* c, int first, int count, const NVNrectangle* rects) {
    if (!rects || first < 0 || count < 1 || first >= static_cast<int>(kMaxViewportsSmall)) return;
    const uint32_t n = static_cast<uint32_t>(std::min<int>(count, static_cast<int>(kMaxViewportsSmall) - first));
    emit_small(c, SmallKind::ExclusiveScissors, static_cast<uint32_t>(first), n, 0, 0, rects, n * 16);
}
void nvnCommandBufferSetExclusiveScissor(NVNcommandBuffer* c, int x, int y, int w, int h) {
    const NVNrectangle r{x, y, w, h};
    nvnCommandBufferSetExclusiveScissors(c, 0, 1, &r);
}

// ---- conservative raster ------------------------------------------------------------------------------------------------------------
// sdk 0x60A030 SetConservativeRasterMode(cmd, mode, bloat): mode 0 off, 1 and 2 on (2 sets one more control bit, 0x10), bloat & 0xF quarter
// pixels. (Enable and Dilate are 0x609F30 / 0x609FA0: api_cmd_state.cpp.)
void nvnCommandBufferSetConservativeRasterMode(NVNcommandBuffer* c, int mode, int bloat) {
    emit_small(c, SmallKind::ConservativeMode, static_cast<uint32_t>(mode), static_cast<uint32_t>(bloat) & 0xFu, 0, 0);
}

// ---- ProgramBuilder -----------------------------------------------------------------------------------------------------------------
// sdk 0x5E5DE0..0x5E5F10: +8 device, +0x18 sample shading, +0x1C/+0x20 debug data hash count/array, +0x28/+0x30 shader count/array,
// +0x48/+0x50 subroutine linkage count/array. SetDefaults leaves +0..0x17 alone and clears the rest (the ray-tracing fields too, depths = 2).
constexpr size_t kPbDevice = 8, kPbSample = 0x18, kPbHashCount = 0x1C, kPbHashes = 0x20, kPbShaderCount = 0x28, kPbShaders = 0x30,
                 kPbLinkCount = 0x48, kPbLinks = 0x50, kPbTraceDepth = 0x58, kPbCallableDepth = 0x5C, kPbRtFlags = 0x60;
static_assert(kPbRtFlags + 4 <= sizeof(NVNprogramBuilder), "program builder layout fits the object");

void nvnProgramBuilderSetDefaults(NVNprogramBuilder* b) {
    if (!b) return;
    std::memset(reinterpret_cast<uint8_t*>(b) + kPbSample, 0, 0x64 - kPbSample);
    wr<uint32_t>(b, kPbTraceDepth, 2);
    wr<uint32_t>(b, kPbCallableDepth, 2);
}
void nvnProgramBuilderSetDevice(NVNprogramBuilder* b, NVNdevice* device) { wr<uint64_t>(b, kPbDevice, bits_of(device)); }
NVNdevice* nvnProgramBuilderGetDevice(const NVNprogramBuilder* b) { return ptr_of<NVNdevice>(rd<uint64_t>(b, kPbDevice)); }
void nvnProgramBuilderSetSampleShading(NVNprogramBuilder* b, int mode) { wr<uint32_t>(b, kPbSample, static_cast<uint32_t>(mode)); }
int nvnProgramBuilderGetSampleShading(const NVNprogramBuilder* b) { return static_cast<int>(rd<uint32_t>(b, kPbSample)); }
void nvnProgramBuilderSetDebugDataHashes(NVNprogramBuilder* b, int count, const void* hashes) {
    wr<uint32_t>(b, kPbHashCount, static_cast<uint32_t>(count));
    wr<uint64_t>(b, kPbHashes, bits_of(hashes));
}
const void* nvnProgramBuilderGetDebugDataHashes(const NVNprogramBuilder* b) { return ptr_of<const void>(rd<uint64_t>(b, kPbHashes)); }
void nvnProgramBuilderSetShaders(NVNprogramBuilder* b, int count, const NVNshaderData* shaders) {
    wr<uint32_t>(b, kPbShaderCount, static_cast<uint32_t>(count));
    wr<uint64_t>(b, kPbShaders, bits_of(shaders));
}
int nvnProgramBuilderGetShaderCount(const NVNprogramBuilder* b) { return static_cast<int>(rd<uint32_t>(b, kPbShaderCount)); }
const NVNshaderData* nvnProgramBuilderGetShaders(const NVNprogramBuilder* b) { return ptr_of<const NVNshaderData>(rd<uint64_t>(b, kPbShaders)); }
void nvnProgramBuilderSetSubroutineLinkage(NVNprogramBuilder* b, int count, const NVNsubroutineLinkageMapPtr* map) {
    wr<uint32_t>(b, kPbLinkCount, static_cast<uint32_t>(count));
    wr<uint64_t>(b, kPbLinks, bits_of(map));
}
int nvnProgramBuilderGetSubroutineLinkageCount(const NVNprogramBuilder* b) { return static_cast<int>(rd<uint32_t>(b, kPbLinkCount)); }
const NVNsubroutineLinkageMapPtr* nvnProgramBuilderGetSubroutineLinkage(const NVNprogramBuilder* b) {
    return ptr_of<const NVNsubroutineLinkageMapPtr>(rd<uint64_t>(b, kPbLinks));
}

// ---- device message buffers ---------------------------------------------------------------------------------------------------------
// Object (sdk 0x5DCA10..0x5DCAE0): +0 device, +8 NVNmemoryPool*, +0x10 offset, +0x14 size, cursor state at +0x18..+0x28 (nvn_small.h).
// SetDefaults clears +8..+0x28; SetStorage stores pool, offset, size; Reset, Begin, End and GetNextMessage work on the region
// (pool storage + offset). Shaders cannot write device messages on this back end, so a buffer only ever holds what the game or a test wrote.
constexpr size_t kMbDevice = 0, kMbPool = 8, kMbOffset = 0x10, kMbSize = 0x14;

// The CPU view of the region, null when the pool is not ours, has no CPU storage or the region does not fit.
uint8_t* mb_region(const NVNdeviceMessageBuffer* b, MemoryPool** pool_out = nullptr) {
    if (!b) return nullptr;
    MemoryPool* pool = host_of<MemoryPool>(ptr_of<const void>(rd<uint64_t>(b, kMbPool)));
    const int32_t off = rd<int32_t>(b, kMbOffset), size = rd<int32_t>(b, kMbSize);
    if (pool_out) *pool_out = pool;
    if (!pool || !pool->base || off < 0 || size < static_cast<int32_t>(kMsgHeaderBytes) || static_cast<uint64_t>(off) + static_cast<uint64_t>(size) > pool->size) return nullptr;
    return pool->base + off;
}

void nvnDeviceMessageBufferSetDefaults(NVNdeviceMessageBuffer* b) {
    if (b) std::memset(reinterpret_cast<uint8_t*>(b) + 8, 0, 0x20);
}
void nvnDeviceMessageBufferSetDevice(NVNdeviceMessageBuffer* b, NVNdevice* device) { wr<uint64_t>(b, kMbDevice, bits_of(device)); }
void nvnDeviceMessageBufferSetStorage(NVNdeviceMessageBuffer* b, const NVNmemoryPool* pool, int offset, int size) {
    wr<uint64_t>(b, kMbPool, bits_of(pool));
    wr<int32_t>(b, kMbOffset, offset);
    wr<int32_t>(b, kMbSize, size);
}
void nvnDeviceMessageBufferReset(NVNdeviceMessageBuffer* b) {
    if (uint8_t* region = mb_region(b)) msgbuf_write_reset_header(region, static_cast<uint32_t>(rd<int32_t>(b, kMbSize)));
}
void nvnDeviceMessageBufferBeginMessages(NVNdeviceMessageBuffer* b) {
    if (b) msgbuf_begin(reinterpret_cast<uint8_t*>(b), mb_region(b));
}
void nvnDeviceMessageBufferEndMessages(NVNdeviceMessageBuffer* b) {
    if (b) msgbuf_end(reinterpret_cast<uint8_t*>(b));
}
// 0x5DCAE0: (buffer, message, text, text size) -> a message was returned.
NVNboolean nvnDeviceMessageBufferGetNextMessage(NVNdeviceMessageBuffer* b, NVNdeviceMessage* message, char* text, size_t text_size) {
    return b && msgbuf_next(reinterpret_cast<uint8_t*>(b), message, text, text_size) ? 1 : 0;
}

// Command buffer side. ResetDeviceMessageBuffer (0x5DD9E0) writes {12, size, 0} to the region on the GPU timeline: recorded as a buffer update.
// BindDeviceMessageBuffer (0x5DD2D0: stage, index, buffer) hands the region's GPU address to a stage's slot so a shader can write messages; no
// translated shader does, so the bind records nothing. The Deferred forms (0x5DD610 / 0x5DDB60) take the GPU address of the region instead
// of the object: the Bind is the same no-op; the Reset has no size to write without reading the object, so it is a no-op as well.
void nvnCommandBufferResetDeviceMessageBuffer(NVNcommandBuffer* c, const NVNdeviceMessageBuffer* b) {
    MemoryPool* pool = nullptr;
    if (!mb_region(b, &pool)) return;
    const int32_t off = rd<int32_t>(b, kMbOffset), size = rd<int32_t>(b, kMbSize);
    uint8_t header[kMsgHeaderBytes];
    msgbuf_write_reset_header(header, static_cast<uint32_t>(size));
    P_UpdateUniformBuffer p{};
    p.address = pool->gpu_base + static_cast<uint64_t>(off);
    p.buffer_size = static_cast<uint64_t>(size);
    p.size = kMsgHeaderBytes;
    with_rec(c, [&](CmdList& l) {
        l.hold(pool);
        l.emit(Op::UpdateUniformBuffer, p, header, kMsgHeaderBytes);
    });
}
void nvnCommandBufferBindDeviceMessageBuffer(NVNcommandBuffer*, int, int, const NVNdeviceMessageBuffer*) {
    static std::atomic<bool> seen{false};
    log_once(seen, "nvnCommandBufferBindDeviceMessageBuffer: nothing on this back end writes device messages, the bind is not recorded");
}
void nvnCommandBufferBindDeviceMessageBufferDeferred(NVNcommandBuffer*, int, int, NVNbufferAddress) {}
void nvnCommandBufferResetDeviceMessageBufferDeferred(NVNcommandBuffer*, NVNbufferAddress) {}

// ---- dirty tile maps ----------------------------------------------------------------------------------------------------------------
// NVNdirtyTileMapParameters (sdk 0x5D5130..0x5D5310, offsets in nvn_small.h). The setters/getters store and return the fields; the names of the
// Inputs arguments are by position, the sdk only copies them: SetInputs(flags +0x64, level +0x28, tile width +0x5C, tile height +0x60).
constexpr size_t kDtTexture = 0, kDtView = 8, kDtRegion = 0x10, kDtLevel = 0x28, kDtColorCount = 0x2C, kDtOutAddr = 0x30, kDtOutRow = 0x38,
                 kDtOutSlice = 0x3C, kDtOutExtra = 0x40, kDtTable = 0x48, kDtTableExtra = 0x50, kDtTableC = 0x58, kDtTileW = 0x5C, kDtTileH = 0x60,
                 kDtFlags = 0x64, kDtDsMask = 0x68, kDtExpand = 0x70, kDtEnd = 0x74;
static_assert(kDtEnd <= sizeof(NVNdirtyTileMapParameters), "dirty tile map parameter layout fits the object");

void nvnDirtyTileMapParametersSetDefaults(NVNdirtyTileMapParameters* p) {
    if (!p) return;
    std::memset(p, 0, kDtEnd);
    wr<uint32_t>(p, kDtTableC, 1);
    wr<uint32_t>(p, kDtTileW, 16);
    wr<uint32_t>(p, kDtTileH, 16);
}
void nvnDirtyTileMapParametersSetTexture(NVNdirtyTileMapParameters* p, const NVNtexture* texture, const NVNtextureView* view, const NVNcopyRegion* region) {
    wr<uint64_t>(p, kDtTexture, bits_of(texture));
    wr<uint64_t>(p, kDtView, bits_of(view));
    if (region) std::memcpy(reinterpret_cast<uint8_t*>(p) + kDtRegion, region, 24);
    else std::memset(reinterpret_cast<uint8_t*>(p) + kDtRegion, 0, 24);
}
void nvnDirtyTileMapParametersGetTexture(const NVNdirtyTileMapParameters* p, const NVNtexture** texture, const NVNtextureView** view, NVNcopyRegion* region) {
    if (texture) *texture = ptr_of<const NVNtexture>(rd<uint64_t>(p, kDtTexture));
    if (view) *view = ptr_of<const NVNtextureView>(rd<uint64_t>(p, kDtView));
    if (region) std::memcpy(region, reinterpret_cast<const uint8_t*>(p) + kDtRegion, 24);
}
void nvnDirtyTileMapParametersSetInputs(NVNdirtyTileMapParameters* p, int flags, int level, int tile_width, int tile_height) {
    wr<int32_t>(p, kDtFlags, flags);
    wr<int32_t>(p, kDtLevel, level);
    wr<int32_t>(p, kDtTileW, tile_width);
    wr<int32_t>(p, kDtTileH, tile_height);
}
void nvnDirtyTileMapParametersGetInputs(const NVNdirtyTileMapParameters* p, int* flags, int* level, int* tile_width, int* tile_height) {
    if (flags) *flags = rd<int32_t>(p, kDtFlags);
    if (level) *level = rd<int32_t>(p, kDtLevel);
    if (tile_width) *tile_width = rd<int32_t>(p, kDtTileW);
    if (tile_height) *tile_height = rd<int32_t>(p, kDtTileH);
}
void nvnDirtyTileMapParametersSetOutputs(NVNdirtyTileMapParameters* p, NVNbufferAddress address, int row_stride, int slice_stride, uint64_t extra) {
    wr<uint64_t>(p, kDtOutAddr, address);
    wr<int32_t>(p, kDtOutRow, row_stride);
    wr<int32_t>(p, kDtOutSlice, slice_stride);
    wr<uint64_t>(p, kDtOutExtra, extra);
}
void nvnDirtyTileMapParametersGetOutputs(const NVNdirtyTileMapParameters* p, NVNbufferAddress* address, int* row_stride, int* slice_stride, uint64_t* extra) {
    if (address) *address = rd<uint64_t>(p, kDtOutAddr);
    if (row_stride) *row_stride = rd<int32_t>(p, kDtOutRow);
    if (slice_stride) *slice_stride = rd<int32_t>(p, kDtOutSlice);
    if (extra) *extra = rd<uint64_t>(p, kDtOutExtra);
}
void nvnDirtyTileMapParametersSetConstantColorTable(NVNdirtyTileMapParameters* p, const void* table, int count, int c, uint64_t extra) {
    wr<uint64_t>(p, kDtTable, bits_of(table));
    wr<int32_t>(p, kDtColorCount, count);
    wr<int32_t>(p, kDtTableC, c);
    wr<uint64_t>(p, kDtTableExtra, extra);
}
void nvnDirtyTileMapParametersGetConstantColorTable(const NVNdirtyTileMapParameters* p, const void** table, int* count, int* c, uint64_t* extra) {
    if (table) *table = ptr_of<const void>(rd<uint64_t>(p, kDtTable));
    if (count) *count = rd<int32_t>(p, kDtColorCount);
    if (c) *c = rd<int32_t>(p, kDtTableC);
    if (extra) *extra = rd<uint64_t>(p, kDtTableExtra);
}
void nvnDirtyTileMapParametersSetDepthStencilMask(NVNdirtyTileMapParameters* p, int depth, int stencil) {
    wr<int32_t>(p, kDtDsMask, depth);
    wr<int32_t>(p, kDtDsMask + 4, stencil);
}
void nvnDirtyTileMapParametersGetDepthStencilMask(const NVNdirtyTileMapParameters* p, int* depth, int* stencil) {
    if (depth) *depth = rd<int32_t>(p, kDtDsMask);
    if (stencil) *stencil = rd<int32_t>(p, kDtDsMask + 4);
}
void nvnDirtyTileMapParametersSetExpandFootprint(NVNdirtyTileMapParameters* p, int left, int top, int right, int bottom) {
    wr<uint8_t>(p, kDtExpand, static_cast<uint8_t>(left));
    wr<uint8_t>(p, kDtExpand + 1, static_cast<uint8_t>(top));
    wr<uint8_t>(p, kDtExpand + 2, static_cast<uint8_t>(right));
    wr<uint8_t>(p, kDtExpand + 3, static_cast<uint8_t>(bottom));
}
void nvnDirtyTileMapParametersGetExpandFootprint(const NVNdirtyTileMapParameters* p, uint8_t* left, uint8_t* top, uint8_t* right, uint8_t* bottom) {
    if (left) *left = rd<uint8_t>(p, kDtExpand);
    if (top) *top = rd<uint8_t>(p, kDtExpand + 1);
    if (right) *right = rd<uint8_t>(p, kDtExpand + 2);
    if (bottom) *bottom = rd<uint8_t>(p, kDtExpand + 3);
}

// ComputeDirtyTileMapEx (0x5D4CA0): the console compares the texture region tile by tile against the constant colours and writes a map (a bit
// or a byte per tile) of the tiles that differ. This back end does not read the texture: it reports every tile of the region as dirty, which is
// the safe answer (a consumer redoes work it could have skipped, never skips work it needed). The map is written on the GPU timeline as buffer
// updates, a row of tiles at a time (or a slice at a time when the rows are packed). One byte per tile holds 1, one bit per tile holds 1.
void nvnCommandBufferComputeDirtyTileMapEx(NVNcommandBuffer* c, const NVNdirtyTileMapParameters* params) {
    if (!params) return;
    const uint8_t* pb = reinterpret_cast<const uint8_t*>(params);
    const uint64_t out = rd<uint64_t>(pb, kDtOutAddr);
    const DirtyTileGeometry g = dirty_tile_geometry(pb);
    if (!out || !g.tiles_x || !g.tiles_y || g.row_bytes == 0) return;
    const uint32_t padded = (g.row_bytes + 3u) & ~3u;
    const bool pad_fits = g.row_stride >= padded;   // the row's last word stays inside its stride; else the 1..3 tail bytes are not written
    const uint32_t row_write = pad_fits ? padded : (g.row_bytes & ~3u);
    if (!row_write) return;
    std::vector<uint8_t> row(row_write, g.bit_per_tile ? 0xFFu : 1u);
    if (g.bit_per_tile) {   // bits past the last tile of the row stay clear
        const uint32_t tail_bits = g.tiles_x - (g.row_bytes - 1) * 8;
        if (g.row_bytes <= row_write) row[g.row_bytes - 1] = static_cast<uint8_t>((1u << tail_bits) - 1u);
        for (uint32_t i = g.row_bytes; i < row_write; ++i) row[i] = 0;
    } else {
        for (uint32_t i = g.row_bytes; i < row_write; ++i) row[i] = 0;
    }
    const bool packed = g.row_stride == row_write;   // rows follow each other: one update per slice
    std::vector<uint8_t> slice_blob;
    if (packed) {
        slice_blob.reserve(static_cast<size_t>(row_write) * g.tiles_y);
        for (uint32_t y = 0; y < g.tiles_y; ++y) slice_blob.insert(slice_blob.end(), row.begin(), row.end());
    }
    constexpr uint32_t kMaxUpdates = 1u << 14;   // a runaway region does not flood the list
    uint32_t updates = 0;
    with_rec(c, [&](CmdList& l) {
        for (uint32_t s = 0; s < g.slices && updates < kMaxUpdates; ++s) {
            const uint64_t slice_at = out + static_cast<uint64_t>(s) * g.slice_stride;
            P_UpdateUniformBuffer p{};
            if (packed) {
                p.address = slice_at;
                p.buffer_size = slice_blob.size();
                p.size = slice_blob.size();
                l.emit(Op::UpdateUniformBuffer, p, slice_blob.data(), static_cast<uint32_t>(slice_blob.size()));
                ++updates;
                continue;
            }
            for (uint32_t y = 0; y < g.tiles_y && updates < kMaxUpdates; ++y, ++updates) {
                p.address = slice_at + static_cast<uint64_t>(y) * g.row_stride;
                p.buffer_size = row.size();
                p.size = row.size();
                l.emit(Op::UpdateUniformBuffer, p, row.data(), static_cast<uint32_t>(row.size()));
            }
        }
    });
    static std::atomic<bool> seen{false};
    log_once(seen, "nvnCommandBufferComputeDirtyTileMap: every tile is reported dirty, the texture is not compared");
}
// The older form (0x5D4C30) builds the parameters from its arguments and calls Ex: (cmd, texture, view, region, level, output address, row
// stride, slice stride); the tile size is the default 16 x 16 and one byte per tile.
void nvnCommandBufferComputeDirtyTileMap(NVNcommandBuffer* c, const NVNtexture* texture, const NVNtextureView* view, const NVNcopyRegion* region,
                                         int level, NVNbufferAddress out, int row_stride, int slice_stride) {
    NVNdirtyTileMapParameters p;
    nvnDirtyTileMapParametersSetDefaults(&p);
    nvnDirtyTileMapParametersSetTexture(&p, texture, view, region);
    wr<int32_t>(&p, kDtLevel, level);
    nvnDirtyTileMapParametersSetOutputs(&p, out, row_stride, slice_stride, 0);
    nvnCommandBufferComputeDirtyTileMapEx(c, &p);
}

}  // namespace

void register_api_small() {
    NVN2_REG(nvnCommandBufferPushGraphicsConstants);
    NVN2_REG(nvnCommandBufferPushGraphicsConstantsDeferred);
    NVN2_REG(nvnCommandBufferPushComputeConstants);
    NVN2_REG(nvnCommandBufferPushComputeConstantsDeferred);
    NVN2_REG(nvnCommandBufferBindShadingRateImage);
    NVN2_REG(nvnCommandBufferShadingRateImagePalette);
    NVN2_REG(nvnCommandBufferSetShadingRatePaletteMode);
    NVN2_REG(nvnCommandBufferEnableExclusiveScissor);
    NVN2_REG(nvnCommandBufferSetExclusiveScissor);
    NVN2_REG(nvnCommandBufferSetExclusiveScissors);
    NVN2_REG(nvnCommandBufferSetConservativeRasterMode);
    NVN2_REG(nvnProgramBuilderSetDefaults);
    NVN2_REG(nvnProgramBuilderSetDevice);
    NVN2_REG(nvnProgramBuilderGetDevice);
    NVN2_REG(nvnProgramBuilderSetSampleShading);
    NVN2_REG(nvnProgramBuilderGetSampleShading);
    NVN2_REG(nvnProgramBuilderSetDebugDataHashes);
    NVN2_REG(nvnProgramBuilderGetDebugDataHashes);
    NVN2_REG(nvnProgramBuilderSetShaders);
    NVN2_REG(nvnProgramBuilderGetShaderCount);
    NVN2_REG(nvnProgramBuilderGetShaders);
    NVN2_REG(nvnProgramBuilderSetSubroutineLinkage);
    NVN2_REG(nvnProgramBuilderGetSubroutineLinkageCount);
    NVN2_REG(nvnProgramBuilderGetSubroutineLinkage);
    NVN2_REG(nvnDeviceMessageBufferSetDefaults);
    NVN2_REG(nvnDeviceMessageBufferSetDevice);
    NVN2_REG(nvnDeviceMessageBufferSetStorage);
    NVN2_REG(nvnDeviceMessageBufferReset);
    NVN2_REG(nvnDeviceMessageBufferBeginMessages);
    NVN2_REG(nvnDeviceMessageBufferEndMessages);
    NVN2_REG(nvnDeviceMessageBufferGetNextMessage);
    NVN2_REG(nvnCommandBufferResetDeviceMessageBuffer);
    NVN2_REG(nvnCommandBufferBindDeviceMessageBuffer);
    NVN2_REG(nvnCommandBufferBindDeviceMessageBufferDeferred);
    NVN2_REG(nvnCommandBufferResetDeviceMessageBufferDeferred);
    NVN2_REG(nvnDirtyTileMapParametersSetDefaults);
    NVN2_REG(nvnDirtyTileMapParametersSetTexture);
    NVN2_REG(nvnDirtyTileMapParametersGetTexture);
    NVN2_REG(nvnDirtyTileMapParametersSetInputs);
    NVN2_REG(nvnDirtyTileMapParametersGetInputs);
    NVN2_REG(nvnDirtyTileMapParametersSetOutputs);
    NVN2_REG(nvnDirtyTileMapParametersGetOutputs);
    NVN2_REG(nvnDirtyTileMapParametersSetConstantColorTable);
    NVN2_REG(nvnDirtyTileMapParametersGetConstantColorTable);
    NVN2_REG(nvnDirtyTileMapParametersSetDepthStencilMask);
    NVN2_REG(nvnDirtyTileMapParametersGetDepthStencilMask);
    NVN2_REG(nvnDirtyTileMapParametersSetExpandFootprint);
    NVN2_REG(nvnDirtyTileMapParametersGetExpandFootprint);
    NVN2_REG(nvnCommandBufferComputeDirtyTileMapEx);
    NVN2_REG(nvnCommandBufferComputeDirtyTileMap);
}

}  // namespace nxrt::nvn
