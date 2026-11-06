// SPDX-License-Identifier: GPL-3.0-or-later
// The smaller NVN2 GPU features (plan goal 20): push constants, variable rate shading, exclusive scissor, conservative raster, fast-clear
// registration, ProgramBuilder, device message buffers and dirty tile maps. This header holds the plain-data side: the layouts of the NVN2-only
// objects, the replay state of the recorded commands (Op::Small, nvn_cmd.h) and the pure helpers the entry points (api_small.cpp) and the replay
// (nvn_replay.cpp) share. Nothing here includes Vulkan or the host objects of nvn_core.h, so the unit tests exercise it without a GPU.
//
// Where a layout was unknown it was read from the SDK (work/ida/sdk/sdk.elf, image base 0x7500000000; addresses below are image offsets, the
// ones in src/gfx/nvn/nvn2_procs.tsv). The notes at each item say what was read and what was decided.
#pragma once
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

namespace nxrt::nvn::be {

// ---- NVN2-only objects in guest memory --------------------------------------------------------------------------------------------
// Sizes are what the sdk touches (docs/nvn2_abi.md section 3); the objects are accessed by offset, never as C++ members.
struct NVNprogramBuilder { uint8_t reserved[0x80]; };           // SetDefaults writes to +0x64
struct NVNdeviceMessageBuffer { uint8_t reserved[0x40]; };      // SetDefaults clears +8..+0x28; Begin/GetNext keep a cursor at +0x18..+0x28
struct NVNdirtyTileMapParameters { uint8_t reserved[0x80]; };   // getters to +0x74
// What nvnDeviceMessageBufferGetNextMessage fills in (sdk 0x5DCAE0: +0 id, +4 kind, +8..+0x17 four argument words).
struct NVNdeviceMessage { int32_t id; int32_t kind; uint32_t args[4]; };

// ---- recorded state commands (Op::Small) ------------------------------------------------------------------------------------------
// One record kind carries the whole family so the replay switch has one case; P_Small (nvn_cmd.h) = {kind, a, b, c, addr}.
enum class SmallKind : uint32_t {
    PushConstants = 1,        // a stage, b byte offset, c byte size; blob = the data
    PushConstantsDeferred,    // a stage, b byte offset, c byte size; addr = GPU address of the data, read when the list is replayed
    BindShadingRateImage,     // a = 1 when an image is bound (0 unbinds); addr = the image's identity (never dereferenced)
    ShadingRatePalette,       // a palette index, b entry count, c = 1 when enabled (0: the sdk's "no palette"); blob = b x uint32 entries
    ShadingRatePaletteMode,   // a mode
    ExclusiveScissorEnable,   // a enable
    ExclusiveScissors,        // a first, b count; blob = count x {x, y, w, h} int32
    ConservativeMode,         // a mode (0 off, 1, 2), b extra bloat in quarter pixels (0..15)
    ConservativeDilate,       // b extra bloat in quarter pixels (0..3)
};

constexpr uint32_t kPushStages = 9;        // NVN2 has 9 stage slots (mask 0x1FF, docs/nvn2_abi.md section 5)
constexpr uint32_t kPushAreaBytes = 256;   // the sdk clamps a push to 256 bytes (0x100) and works in 32-bit words
constexpr uint32_t kPushCompute = 0x80000000u;   // in P_Small::a of a push record: it came from a compute push (the stage is in the low bits)
constexpr uint32_t kMaxViewportsSmall = 16;

// The replay's copy of this family's GPU state. It persists across lists like the other GPU state (nvn_replay.cpp GState).
struct SmallState {
    // push constants: one 256-byte area per stage; `pushes` counts the writes (the draw path could upload only on change)
    uint8_t push[kPushStages][kPushAreaBytes] = {};
    uint64_t pushes = 0;
    // what nvnCommandBufferPushComputeConstants(Deferred) wrote, whatever stage number the game passed (also kept in push[stage]): the
    // area NVN2 compute programs read from their c[0] table (ShaderControl::c0_push). push_cs_stage = the stage of the last compute push.
    uint8_t push_cs[kPushAreaBytes] = {};
    uint32_t push_cs_stage = 0;
    // shading rate: the image identity (0 none), the palette mode, and per palette index 16 four-bit entries (NV palette numbering)
    uint64_t rate_image = 0;
    int32_t rate_palette_mode = 0;
    bool palette_on[16] = {};
    uint8_t palette[16][16] = {};
    // exclusive scissor: one enable for all viewports (sdk 0x620EA0 writes the 16 enables at once) and a rectangle per viewport
    bool excl_enable = false;
    int32_t excl[kMaxViewportsSmall][4] = {};
    // conservative raster: the NVN1 pair (enable, dilate) and the NVN2 mode call; both end in the same hardware control
    bool cons_legacy = false;
    int32_t cons_mode = 0;
    uint32_t cons_mode_bloat_q = 0;     // quarter pixels, from SetConservativeRasterMode
    uint32_t cons_dilate_q = 0;         // quarter pixels, from SetConservativeRasterDilate
};

// Applies one Op::Small record to the state. `blob` has `blob_bytes` valid bytes after the payload. A malformed record changes nothing.
void small_apply(SmallState& st, uint32_t kind, uint32_t a, uint32_t b, uint32_t c, uint64_t addr, const uint8_t* blob, uint32_t blob_bytes);

// ---- push constants ---------------------------------------------------------------------------------------------------------------
// Writes `size` bytes at byte `offset` of `stage`'s area the way the sdk does: size clamped to 256 and cut to whole words (under 4 bytes is
// ignored), offset kept to a byte and to a word (the graphics path sends it as a byte, the compute path shifts it right by 2), and nothing
// past the end of the area. The caller masks the stage (graphics keeps 3 bits, sdk 0x5F85E0: a2 & 7; compute passes it whole); a stage past
// the 9 slots is refused. False when nothing was written.
bool push_write(SmallState& st, uint32_t stage, uint32_t offset, uint32_t size, const void* data);

// ---- shading rate -----------------------------------------------------------------------------------------------------------------
// NVN2's shading rate image is the NV model: a palette maps the image's 4-bit value to a rate. The entries use the VK_NV_shading_rate_image
// numbering: 0 no invocations, 1..5 = 16/8/4/2/1 invocations per pixel, 6 = 1 per 2x1, 7 = 1 per 1x2, 8 = 1 per 2x2, 9 = 1 per 4x2,
// 10 = 1 per 2x4, 11 = 1 per 4x4 (sdk 0x620D80 fills the unset entries with 5 = one invocation per pixel).
constexpr uint8_t kPaletteOnePerPixel = 5;
// KHR fragment size (width, height in pixels) of a palette entry; supersampling entries and "no invocations" become 1x1 (full rate).
void palette_entry_to_fragment_size(uint32_t entry, uint32_t& w, uint32_t& h);
// The largest size not above (w, h) that the device supports. `supported` has bit (log2(w) * 3 + log2(h)) set for each supported size
// (w, h in 1, 2, 4); 1x1 is always allowed.
void fit_fragment_size(uint32_t supported, uint32_t& w, uint32_t& h);
// The pipeline fragment shading rate the replay sets: the palette entry's size when an image is bound and every entry of palette 0 is the same
// (then any image value gives that rate, so it is exact); 1x1 otherwise. `exact` is false when an image is bound but its values would pick
// different rates (the image is not read: the draw runs at full rate).
void shading_rate_for_draw(const SmallState& st, uint32_t supported, uint32_t& w, uint32_t& h, bool* exact = nullptr);

// ---- exclusive scissor ------------------------------------------------------------------------------------------------------------
// Removes the exclusive rectangle E from the scissor S when the result is again one rectangle (E misses S, covers it, or covers it across
// one axis from one side). `exact` false: E is a hole in S (or a corner), which one scissor cannot express; S is returned unchanged.
// Rectangles are {x, y, w, h}.
void exclusive_reduce(const int32_t s[4], const int32_t e[4], int32_t out[4], bool* exact = nullptr);
// The scissor of viewport `i` after the exclusive scissor (unchanged when disabled).
void small_exclusive_scissor(const SmallState& st, uint32_t i, const int32_t s[4], int32_t out[4], bool* exact = nullptr);

// ---- conservative raster ------------------------------------------------------------------------------------------------------------
bool small_conservative_on(const SmallState& st);
// The extra primitive overestimation size in pixels for the state, bounded by the device (max, granularity as VkPhysicalDeviceConservativeRasterizationPropertiesEXT).
float small_conservative_extra(const SmallState& st, float device_max, float device_granularity);
// The quarter pixels the NVN1 dilate argument means: ceil(dilate * 4) clamped to 0..3 (sdk 0x609FA0).
uint32_t dilate_to_quarters(float dilate);

// ---- fast clear registration --------------------------------------------------------------------------------------------------------
// The console keeps a small table of clear values the hardware can clear without touching memory (sdk 0x6176A0 and friends; a device has 31
// colours, 2 depth values (0.0 and 1.0 only) and 6 stencil values, 5 for one depth-stencil format). Vulkan has no such table, so this only
// keeps the same registry: the same values are accepted, the same ones refused, and each gets a stable index (-1 = refused).
struct FastClearTables {
    static constexpr uint32_t kMaxColors = 31, kMaxDepth = 2, kMaxStencil = 6, kMaxStencilShort = 5;
    std::mutex mu;
    struct Color { uint32_t kind, format; uint32_t v[4]; };
    std::vector<Color> colors;
    std::vector<uint32_t> depth;     // float bit patterns of 0.0 / 1.0
    std::vector<uint32_t> stencil;
    // kind 0 float, 1 int, 2 uint; v = the four 32-bit words. Format 0 (none) is refused.
    int add_color(uint32_t kind, const uint32_t v[4], uint32_t format);
    int add_depth(float value);
    // `format` is the NVNformat the stencil index call passes (0 or 54 = DEPTH24_STENCIL8: 6 entries, 53: 5 entries; other values refused).
    int add_stencil(uint32_t value, uint32_t format);
};

// ---- device message buffers ---------------------------------------------------------------------------------------------------------
// Layout of the object (sdk 0x5DCA10..0x5DCAE0): +0 device, +8 NVNmemoryPool*, +0x10 offset in the pool, +0x14 size, +0x18 cursor (CPU pointer
// to the region while a read pass is open), +0x20 next read offset, +0x24 end offset. The region starts with {u32 end of data = 12 after a reset,
// u32 size, u32 count/snapshot = 0}; messages follow from byte 12: {u32 length, u16 type, i16 id, u32 args[4], NUL ended text at +0x18}.
constexpr uint32_t kMsgHeaderBytes = 12;
constexpr uint32_t kMsgRecordHeader = 0x18;
// Writes the reset header {12, size, 0} into `region` (>= 12 bytes).
void msgbuf_write_reset_header(uint8_t* region, uint32_t size);
// Starts a read pass over `region` (null: unreadable). Sets cursor, read offset and end like nvnDeviceMessageBufferBeginMessages.
void msgbuf_begin(uint8_t* object, uint8_t* region);
void msgbuf_end(uint8_t* object);
// The next message: false when none. `text` may be null. The kind is the record's raw type value (the sdk maps it through a table that
// nothing here produces messages for).
bool msgbuf_next(uint8_t* object, NVNdeviceMessage* msg, char* text, uint64_t text_size);

// ---- dirty tile maps ------------------------------------------------------------------------------------------------------------------
// Layout of NVNdirtyTileMapParameters (sdk 0x5D5130..0x5D5310): +0 texture, +8 view, +0x10 region {x,y,z,w,h,d} (24 bytes), +0x28 inputs.b,
// +0x2C constant colour count, +0x30 output address, +0x38 output row stride, +0x3C output slice stride, +0x40 outputs.d, +0x48 colour table
// address, +0x50 colour table.d, +0x58 colour table.c, +0x5C inputs.c, +0x60 inputs.d, +0x64 inputs.a, +0x68/+0x6C depth-stencil mask,
// +0x70..+0x73 expand footprint. The defaults (+0x58 = 1, +0x5C = 16, +0x60 = 16) make the tile 16 x 16 pixels; bit 0 of +0x64 selects one
// bit per tile (else one byte).
struct DirtyTileGeometry {
    uint32_t tiles_x = 0, tiles_y = 0, slices = 0;
    uint32_t row_bytes = 0;      // bytes of tile map per row of tiles (the sdk's default row stride)
    uint32_t row_stride = 0;     // as given, or row_bytes when 0
    uint32_t slice_stride = 0;   // as given, or row_stride * tiles_y when 0
    bool bit_per_tile = false;
};
DirtyTileGeometry dirty_tile_geometry(const uint8_t* params);

}  // namespace nxrt::nvn::be
