// SPDX-License-Identifier: GPL-3.0-or-later
// The command list interpreter of the NVN back end (render thread). Walks recorded lists in order, keeps the GPU state the
// console keeps (it persists across lists and submissions), and turns it into Vulkan: dynamic-rendering passes opened lazily on the
// first draw or clear of a render target set, graphics pipelines keyed by the baked state, descriptor sets built from the
// translated shaders' binding tables (nvn_shaders.h), vertex and index data straight from the imported pool memory.
// Lessons applied from SuperMarioWonderRecomp docs/nvn/DEBUG.md and DEBUG-2.md (see work notes in docs/S4d.md): viewport flip by a
// negative-height viewport, a memory barrier after every pass, textures sampled by a draw are made current before the pass,
// handles never dropped by count, deferred bind arrays read at record time.
#include <atomic>
#include <algorithm>
#include <bit>
#include <source_location>
#include <filesystem>
#include "nxrt/instance.h"
#include <cstdio>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <map>
#include <set>
#include <tuple>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <shared_mutex>
#include <thread>

#include "nxrt/config.h"
#include "nxrt/frame_limiter.h"
#include "nxrt/ftrace.h"
#include "nxrt/gauge.h"
#include "nxrt/gfx_tuning.h"
#include "nxrt/log.h"
#include "nxrt/video_codec.h"
#include "nvn_decoded.h"
#include "nvn_devcmd.h"
#include "nvn_enums.h"
#include "nvn_format.h"
#include "nvn_mask_clear.h"
#include "nvn_capture.h"
#include "nvn_gpu_internal.h"
#include "nvn_arch_exp.h"
#include "nvn_hazard.h"
#include "nvn_cmdq.h"
#include "nvn_tess_gen.h"
#include "nxrt/lowlat.h"
#include "nxrt/gaps.h"
#include "nxrt/write_watch.h"
#include "extras/hdr_nvn.h"
#include "extras/fsr1_spatial.h"
#include "extras/spatial_upscaler.h"
#include "extras/temporal.h"
#include "nvn_pipeline.h"
#include "nvn_shaders.h"

namespace nxrt::nvn::be::gpu {
namespace {

constexpr const char* kTag = "nvn.replay";
constexpr int kMaxDepth = 8;

// A hash table of 64-bit keys (already hashed) for the render thread's per-set caches: open addressing, linear probing, no erase; clear()
// is O(1) (slots of an older generation count as empty). std::unordered_map allocated a node per insert and freed every node at each
// clear: ~4% of the render thread with thousands of sets per frame and the set memo cleared every few draws.
template <typename V>
struct FlatMap64 {
    struct Slot { uint64_t key = 0; uint32_t gen = 0; V val{}; };
    std::vector<Slot> s;
    uint32_t gen = 1;
    size_t n = 0;
    static size_t pos(uint64_t k, size_t mask) { return static_cast<size_t>((k * 0x9E3779B97F4A7C15ull) >> 32) & mask; }
    bool empty() const { return n == 0; }
    size_t size() const { return n; }
    void clear() {
        if (!n) return;
        n = 0;
        if (++gen == 0) { for (Slot& x : s) x.gen = 0; gen = 1; }
    }
    V* find(uint64_t k) {
        if (!n) return nullptr;
        const size_t mask = s.size() - 1;
        for (size_t i = pos(k, mask);; i = (i + 1) & mask) {
            Slot& x = s[i];
            if (x.gen != gen) return nullptr;
            if (x.key == k) return &x.val;
        }
    }
    void put(uint64_t k, const V& v) {
        if ((n + 1) * 2 > s.size()) grow();
        const size_t mask = s.size() - 1;
        for (size_t i = pos(k, mask);; i = (i + 1) & mask) {
            Slot& x = s[i];
            if (x.gen != gen) { x.key = k; x.gen = gen; x.val = v; ++n; return; }
            if (x.key == k) { x.val = v; return; }
        }
    }
    void grow() {
        std::vector<Slot> old;
        old.swap(s);
        s.assign(std::max<size_t>(64, old.size() * 2), Slot{});
        const uint32_t g = gen;
        n = 0;
        gen = 1;
        for (const Slot& x : old)
            if (x.gen == g) put(x.key, x.val);
    }
};
// [nvn] flat_set_maps (default on): g_set_cache and g_set_memo in FlatMap64 instead of std::unordered_map (cpuperf_ab_what flat_maps)
bool flat_set_maps_on();

enum Stage { kVertex = 0, kFragment = 1, kGeometry = 2, kTessCtl = 3, kTessEval = 4, kCompute = 5, kTask = 6, kMesh = 7, kStages = 8 };

struct GState {
    const ShaderInfo* stage[kStages] = {};
    struct Bind { uint64_t addr = 0, size = 0; };
    Bind vtx[16], ubo[kStages][16], ssbo[kStages][16], xfb[4];
    uint64_t tex[kStages][64] = {};
    uint64_t img[kStages][16] = {};
    Ref<TexturePool> tpool;
    Ref<SamplerPool> spool;
    VertexAttribInfo attr[16];
    uint32_t attr_count = 0;
    VertexStreamInfo stream[16];
    uint32_t stream_count = 0;
    BlendInfo blend[8];
    ColorInfo color;
    ChannelMaskInfo chmask;
    DepthStencilInfo ds;
    PolygonInfo poly;
    MultisampleInfo ms;
    float vp[16][4] = {};
    uint32_t nvp = 0;
    int32_t sc[16][4] = {};
    uint32_t nsc = 0;
    float dr[16][2] = {};
    bool depth_bounds = false;
    float db_near = 0, db_far = 1;
    uint32_t stencil_ref[2] = {0, 0}, stencil_vmask[2] = {0xff, 0xff}, stencil_wmask[2] = {0xff, 0xff};
    float blend_color[4] = {};
    float line_width = 1, point_size = 1, alpha_ref = 0;
    float bias_factor = 0, bias_units = 0, bias_clamp = 0;
    uint32_t sample_mask = 0xffffffff;
    bool rasterizer_discard = false, depth_clamp = false, prim_restart = false, render_enable = true;
    uint32_t restart_index = 0;
    // tessellation: nvnCommandBufferSetPatchSize (control points per patch; GL default 3) and the fixed levels of
    // nvnCommandBufferSetInner/OuterTessellationLevels (used by NVN only when the program has no control shader)
    uint32_t patch_size = 3;
    float tess_outer[4] = {1, 1, 1, 1}, tess_inner[2] = {1, 1};
    // render targets
    uint32_t ncolor = 0;
    Ref<Texture> color_tex[8];
    TextureViewInfo color_view[8];
    bool color_has_view[8] = {};
    uint32_t color_level[8] = {}, color_layer[8] = {}, color_layers[8] = {};   // level, first layer, layer count each colour target is attached with (trace)
    Ref<Texture> depth_tex;
    TextureViewInfo depth_view;
    bool depth_has_view = false;
    int window_origin = NVN_WINDOW_ORIGIN_MODE_LOWER_LEFT;
    int depth_mode = NVN_DEPTH_MODE_NEAR_IS_MINUS_W;
    SmallState small;   // push constants, shading rate, exclusive scissor, conservative raster (nvn_small.h)
};

GState S;

struct Target {
    Texture* tex = nullptr;
    uint32_t level = 0, layer = 0, layers = 1;
    VkFormat view_format = VK_FORMAT_UNDEFINED;
    bool operator==(const Target& o) const { return tex == o.tex && level == o.level && layer == o.layer && layers == o.layers && view_format == o.view_format; }
};

struct Pass {
    bool open = false;
    uint32_t ncolor = 0;
    Target color[8];
    Target depth;
    VkExtent2D extent = {0, 0};
    VkFormat color_fmt[8] = {};
    VkFormat depth_fmt = VK_FORMAT_UNDEFINED;
    bool has_stencil = false;
    bool depth_ro = false;      // the depth attachment is in DEPTH_STENCIL_READ_ONLY_OPTIMAL (it is also sampled by the draws of this pass)
    uint32_t samples = 1;
    float scale = 1.0f;         // resolution scale of the attachments ([gfx] render_scale): viewports and scissors are multiplied by it
};

Pass P;
// [nvn] pass_barriers = tracked (S38). A pass whose draws can only have written its attachments (no storage buffers or images, no stream-out)
// does not end with a barrier: its written attachments are remembered here, and the next command that could depend on them gets one. A
// layout change of such an image is that barrier for the image (narrow masks, set_narrow_barriers); a later pass that renders to it again in
// the same layout, and every command other than a pass or a layout change, get a full barrier first (end_pass).
struct PendingWrite { Ref<Texture> tex; VkImageLayout layout; };
std::vector<PendingWrite> g_pending;
bool g_tracked = false;         // this submission uses tracked barriers
bool g_pass_unsafe = false;     // a draw of the open pass may write memory other than its attachments
bool g_prog_writes = false;     // the bound program has storage buffer or storage image bindings
bool g_mirror_stale = false;    // a GPU write made pages of a pool copy stale while a draw was being set up: bind again at the next draw

uint32_t g_pass_draws = 0;      // draws recorded in the open pass (GPU pass profiler)
// Draws per render pass size since the last statistics line: the size with the most draws is the game's internal render size (dynamic
// resolution; logged with the statistics so fps measurements can be compared at equal sizes).
std::unordered_map<uint64_t, uint64_t> g_pass_size_draws;
uint64_t g_pass_verts = 0;      // vertices (indices for indexed draws) times instances of those draws
Frame* F = nullptr;
// a descriptor set cache keyed by a hash: FlatMap64, or std::unordered_map in the off windows of cpuperf_ab_what flat_maps (picked at clear)
struct SetMap {
    FlatMap64<VkDescriptorSet> f;
    std::unordered_map<uint64_t, VkDescriptorSet> u;
    bool flat = true;
    void clear() { f.clear(); u.clear(); flat = flat_set_maps_on(); }
    bool empty() const { return flat ? f.empty() : u.empty(); }
    VkDescriptorSet* find(uint64_t k) {
        if (flat) return f.find(k);
        auto it = u.find(k);
        return it == u.end() ? nullptr : &it->second;
    }
    void put(uint64_t k, VkDescriptorSet v) { if (flat) f.put(k, v); else u[k] = v; }
};
SetMap g_set_cache;
uint64_t g_desc_epoch = 1;      // the resolve epoch of [nvn] set_reuse (bind_sets): moves whenever a binding could resolve differently
enum BumpWhy { kBumpOp, kBumpWrite, kBumpPass, kBumpSubmit, kBumpWhyN };
uint64_t g_bump_n[kBumpWhyN] = {};   // statistics: epoch moves by cause
uint64_t g_bump_op[256] = {};        // statistics: epoch moves by command (kBumpOp)
void desc_epoch_bump(BumpWhy why) { ++g_desc_epoch; ++g_bump_n[why]; }
uint64_t g_buf_ver = 1;        // moves when a buffer binding or the program changes (out_draw_bindings)
// [nvn] cpuperf_ab = N: submission chunks, set reuse, the skipped draw buffer checks and the pass look-ahead are off in even windows of N presents and on (as
// configured) in odd ones, marked cpuperf.on / cpuperf.off in the frame trace: paired fps windows of one run (measurement only).
// [nvn] cpuperf_ab_what (comma list) picks the features the windows switch: chunks, pass_chunks, set_reuse, odb (the skipped draw buffer
// checks), lookahead; default chunks, set_reuse, odb, lookahead.
bool g_cpu_ab_on = true;
enum AbFeat : uint64_t { kAbChunks = 1, kAbPassChunks = 2, kAbSetReuse = 4, kAbOdb = 8, kAbLookahead = 16, kAbBarrierKeep = 32, kAbEpochKeep = 64, kAbResolveCache = 128, kAbOverlapCache = 256, kAbEpochDispatch = 512, kAbMemoPass = 1024, kAbBindSkip = 2048, kAbTexPark = 4096, kAbGmemRo = 8192, kAbOverlapGrow = 16384, kAbFlatMaps = 32768, kAbPoolSync = 65536, kAbIndexInc = 131072, kAbOutDelta = 262144, kAbWriteDefer = 524288, kAbSetKeep = 1048576, kAbOdbInc = 2097152, kAbChunkTime = 4194304, kAbKeepWrites = 8388608, kAbCmdThread = 16777216, kAbSkipGpt = 33554432, kAbTexLook = 67108864, kAbCmdCopies = 134217728, kAbPoolDepthIds = 268435456, kAbC0Persist = 536870912, kAbCsGmemHeld = 1ull << 30, kAbCsGmemFlush = 1ull << 31, kAbRefillCompute = 1ull << 32, kAbPreLayout = 1ull << 33, kAbOccDefer = 1ull << 34, kAbHazard = 1ull << 35 };
uint64_t ab_mask() {
    static const uint64_t m = [] {
        const std::string v = nxrt::config_get_str("nvn", "cpuperf_ab_what", "chunks,set_reuse,odb,lookahead");
        static const std::pair<const char*, uint64_t> names[] = {{"chunks", kAbChunks}, {"pass_chunks", kAbPassChunks}, {"set_reuse", kAbSetReuse}, {"odb", kAbOdb},
                                                                 {"lookahead", kAbLookahead}, {"barrier_keep", kAbBarrierKeep}, {"epoch_keep", kAbEpochKeep},
                                                                 {"resolve_cache", kAbResolveCache}, {"overlap_cache", kAbOverlapCache},
                                                                 {"epoch_dispatch", kAbEpochDispatch}, {"memo_pass", kAbMemoPass}, {"bind_skip", kAbBindSkip}, {"tex_park", kAbTexPark}, {"gmem_ro", kAbGmemRo}, {"overlap_grow", kAbOverlapGrow}, {"flat_maps", kAbFlatMaps}, {"pool_sync", kAbPoolSync}, {"index_inc", kAbIndexInc}, {"out_delta", kAbOutDelta}, {"write_defer", kAbWriteDefer}, {"set_keep", kAbSetKeep}, {"odb_inc", kAbOdbInc}, {"chunk_time", kAbChunkTime}, {"keep_writes", kAbKeepWrites}, {"cmd_thread", kAbCmdThread}, {"skip_gpt", kAbSkipGpt}, {"tex_lookahead", kAbTexLook}, {"cmd_copies", kAbCmdCopies}, {"pool_depth_ids", kAbPoolDepthIds}, {"c0_persist", kAbC0Persist}, {"cs_gmem_held", kAbCsGmemHeld}, {"cs_gmem_flush", kAbCsGmemFlush}, {"refill_compute", kAbRefillCompute}, {"pre_layout", kAbPreLayout}, {"occ_defer", kAbOccDefer}, {"hazard", kAbHazard}};
        uint64_t r = 0;
        size_t i = 0;
        while (i <= v.size()) {
            size_t j = v.find(',', i);
            if (j == std::string::npos) j = v.size();
            const std::string w = v.substr(i, j - i);
            for (const auto& [n, b] : names) if (w == n) r |= b;
            i = j + 1;
        }
        return r;
    }();
    return m;
}
// False only in an "off" window of [nvn] cpuperf_ab for a feature it switches.
inline bool ab_on(AbFeat f) { return g_cpu_ab_on || !(ab_mask() & f); }
bool flat_set_maps_on() {
    static const bool on = nxrt::config_get_bool("nvn", "flat_set_maps", true);
    return on && ab_on(kAbFlatMaps);
}
enum class SetReuse { Off, On, Verify };
SetReuse set_reuse_mode() {
    static const SetReuse m = [] {
        const std::string v = nxrt::config_get_str("nvn", "set_reuse", "on");
        return v == "verify" ? SetReuse::Verify : (v == "off" || v == "0" || v == "false") ? SetReuse::Off : SetReuse::On;
    }();
    return m;
}
VkPipeline g_bound_pipeline = VK_NULL_HANDLE;
UploadAlloc g_support;
UploadAlloc g_draw_support;     // the support buffer the next descriptor sets bind: g_support, or one with render scales (support_for_draw)
std::unordered_map<uint64_t, UploadAlloc> g_support_scaled;   // this submission's support buffers with render scales, by content
MaskClear g_mask;
bool g_mask_ready = false, g_mask_ok = false;

// What changed since the last draw. Ops set bits (mark_dirty); a draw handles and clears the groups it needs. A new submission, a
// new pass and anything that binds other pipelines sets them all (a new command buffer has no dynamic state, a new pass has other
// formats and extents).
enum : uint32_t {
    kPipe = 1, kDesc = 2, kVb = 4, kDynVp = 8, kDynRaster = 16, kDynDepth = 32, kDynStencil = 64, kDynBias = 128, kDynBlend = 256, kAllDirty = 0x1ff
};
uint32_t g_dirty = kAllDirty;
// A command may write [addr, addr + size) of pool memory: device-local copies of those pages are stale, and bindings that point at them are
// made again (vertex buffers and descriptor sets).
// The ranges of pool memory the GPU writes, with the serial of the submission that did (a ring). The replay records commands: a range written
// by a submission that has not completed yet does not hold its result in game memory, so the CPU must not read it for a decision that needs
// the data (the arguments of an indirect draw it converts, cpu_view). A range that fell out of the ring while its submission was still running
// makes every range unreadable until that submission is done.
struct GpuWrite { uint64_t lo, hi, serial; };
constexpr size_t kGpuWriteRing = 1024;
GpuWrite g_gw[kGpuWriteRing];
size_t g_gw_next = 0, g_gw_count = 0;
uint64_t g_gw_lost_serial = 0;      // the newest serial among ranges dropped from the ring
void note_gw(uint64_t addr, uint64_t size) {
    if (!size || !F) return;
    GpuWrite& last = g_gw[(g_gw_next + kGpuWriteRing - 1) % kGpuWriteRing];
    if (g_gw_count && last.lo == addr && last.hi == addr + size) { last.serial = F->serial; return; }
    GpuWrite& w = g_gw[g_gw_next];
    if (g_gw_count == kGpuWriteRing) g_gw_lost_serial = std::max(g_gw_lost_serial, w.serial);
    else ++g_gw_count;
    w = {addr, addr + size, F->serial};
    g_gw_next = (g_gw_next + 1) % kGpuWriteRing;
}
// True when no submission still in flight (the one being recorded included) may write [addr, addr + size).
bool gw_settled(uint64_t addr, uint64_t size) {
    const uint64_t done = poll_completed();
    if (g_gw_lost_serial > done) return false;
    for (size_t i = 0; i < g_gw_count; ++i)
        if (g_gw[i].serial > done && addr < g_gw[i].hi && g_gw[i].lo < addr + size) return false;
    return true;
}

// ---- uniform updates without a pass break ([nvn] ubo_update_cpu) ------------------------------------------------------------------
// nvnCommandBufferUpdateUniformBuffer is a GPU-ordered write between draws (OoT: ~800 per frame). As vkCmdUpdateBuffer it ends the
// render pass and needs two full barriers each time. Instead the replay writes the bytes into game memory when it records the update, and
// every uniform binding of such a buffer ("update range") is bound from a snapshot of its bytes taken when the draw is recorded (upload
// ring, one per version of the range). Game memory at record time then holds what the GPU would hold at that point in the queue, provided
// nothing else on the GPU writes the range while earlier work may still read it there. So an update takes this path only when
//   - no submission in flight may write the range (gpu_write_serial: GPU writes seen since it became an update range; when it became one,
//     the write ring, gw_settled), and
//   - no submission in flight reads it from game memory (direct_read_serial: bindings made before its snapshots were possible), and
//   - no compute result is held over it (out_overlaps).
// Otherwise the update is recorded as before (and counts as a GPU write of the range).
struct UpdRange { uint64_t hi = 0; uint64_t gpu_write_serial = 0, direct_read_serial = 0; uint64_t gen = 1; };
std::map<uint64_t, UpdRange> g_upd;                 // by start address
uint64_t g_upd_lo = UINT64_MAX, g_upd_hi = 0;      // bounds of all ranges (a quick filter)
struct UpdSnap { uint64_t addr, size, gen; UploadAlloc u; };
std::unordered_map<uint64_t, UpdSnap> g_upd_snaps;  // this submission's snapshots by (addr, size)
uint64_t g_upd_fast = 0, g_upd_slow = 0, g_upd_snapped = 0, g_upd_snap_hits = 0, g_upd_direct = 0;   // statistics

bool ubo_update_cpu_on() {
    static const bool on = nxrt::config_get_bool("nvn", "ubo_update_cpu", true);
    return on;
}

// The update range overlapping [addr, addr + size), or null.
UpdRange* upd_find(uint64_t addr, uint64_t size) {
    if (g_upd.empty() || addr >= g_upd_hi || addr + size <= g_upd_lo) return nullptr;
    auto it = g_upd.upper_bound(addr);
    if (it != g_upd.end() && it->first < addr + size) return &it->second;
    if (it == g_upd.begin()) return nullptr;
    --it;
    return it->second.hi > addr ? &it->second : nullptr;
}

bool serial_settled(uint64_t serial) { return serial <= completed_serial() || serial <= poll_completed(); }

// True when a range's state changed (it was not yet marked written by this submission).
bool upd_note_gpu_write(uint64_t addr, uint64_t size) {
    if (g_upd.empty() || !size || addr >= g_upd_hi || addr + size <= g_upd_lo) return false;
    bool changed = false;
    for (auto it = g_upd.upper_bound(addr + size - 1); it != g_upd.begin();) {
        --it;
        if (it->second.hi <= addr) break;
        const uint64_t s = F ? F->serial : UINT64_MAX;
        changed |= it->second.gpu_write_serial != s;
        it->second.gpu_write_serial = s;
    }
    return changed;
}

// Makes [lo, hi) an update range (merging the ones it overlaps).
UpdRange& upd_register(uint64_t lo, uint64_t hi) {
    UpdRange r;
    r.hi = hi;
    r.gpu_write_serial = gw_settled(lo, hi - lo) ? 0 : (F ? F->serial : UINT64_MAX);
    r.direct_read_serial = F ? F->serial : UINT64_MAX;   // earlier bindings read game memory, and they are not known: as if one were in flight
    auto it = g_upd.upper_bound(lo);
    if (it != g_upd.begin()) --it;
    while (it != g_upd.end() && it->first < hi) {
        if (it->second.hi > lo) {
            lo = std::min(lo, it->first);
            r.hi = std::max(r.hi, it->second.hi);
            r.gpu_write_serial = std::max(r.gpu_write_serial, it->second.gpu_write_serial);
            r.direct_read_serial = std::max(r.direct_read_serial, it->second.direct_read_serial);
            r.gen = std::max(r.gen, it->second.gen) + 1;
            it = g_upd.erase(it);
        } else {
            ++it;
        }
    }
    g_upd_lo = std::min(g_upd_lo, lo);
    g_upd_hi = std::max(g_upd_hi, r.hi);
    return g_upd[lo] = r;
}

bool host_readable(uint64_t addr, uint64_t n);   // below

// A binding other than a uniform block reads [addr, addr + size) from game memory: an update range there takes no record-time update
// until this submission is done.
void upd_note_read(uint64_t addr, uint64_t size) {
    if (g_upd.empty() || !size || addr >= g_upd_hi || addr + size <= g_upd_lo) return;
    for (auto it = g_upd.upper_bound(addr + size - 1); it != g_upd.begin();) {
        --it;
        if (it->second.hi <= addr) break;
        it->second.direct_read_serial = F ? F->serial : UINT64_MAX;
    }
}

// The update at record time. False: record it as a GPU write (do_write_memory).
bool ubo_update_cpu(uint64_t addr, uint64_t buffer_size, uint64_t offset, const uint8_t* data, uint32_t n) {
    if (!ubo_update_cpu_on() || !n) return false;
    const uint64_t lo = addr, hi = addr + std::max<uint64_t>(buffer_size, offset + n);
    UpdRange* r = upd_find(addr + offset, n);
    if (!r || r->hi < addr + offset + n) r = &upd_register(lo, hi);
    if (!serial_settled(r->gpu_write_serial) || !serial_settled(r->direct_read_serial) || out_touches(addr + offset, n) || !host_readable(addr + offset, n)) {
        ++g_upd_slow;
        return false;
    }
    std::memcpy(host_ptr(addr + offset), data, n);
    capture::note_gpu_write(addr + offset, n);
    ++r->gen;
    ++g_upd_fast;
    return true;
}

// A uniform binding [addr, addr + n) (n: the bytes bound) of an update range: the snapshot to bind (null u: bind game memory as before).
UploadAlloc ubo_snapshot(uint64_t addr, uint64_t n, VkDeviceSize align) {
    UpdRange* r = upd_find(addr, n);
    if (!r) return UploadAlloc();
    if (!serial_settled(r->gpu_write_serial) || out_touches(addr, n) || !host_readable(addr, n)) {
        r->direct_read_serial = F->serial;
        ++g_upd_direct;
        return UploadAlloc();
    }
    const uint64_t key = addr ^ (n << 48) ^ (n >> 16);
    auto it = g_upd_snaps.find(key);
    if (it != g_upd_snaps.end() && it->second.addr == addr && it->second.size == n && it->second.gen == r->gen) { ++g_upd_snap_hits; return it->second.u; }
    UploadAlloc u = F->upload(n, std::max<VkDeviceSize>(align, 256));
    if (!u) {
        r->direct_read_serial = F->serial;
        return UploadAlloc();
    }
    std::memcpy(u.ptr, host_ptr(addr), n);
    g_upd_snaps[key] = UpdSnap{addr, n, r->gen, u};
    ++g_upd_snapped;
    return u;
}

uint64_t g_bump_use[9] = {};   // statistics: epoch moves by GPU writes, by use (0-5 note_gpu_write uses, 8 update ranges)
// statistics: epoch moves by GPU writes, by call site (g_gw_site: 0 other, 1 gmem bound storage, 2 gmem pointer window, 3 draw over a held
// result, 4 graphics storage binding, 5 compute storage binding, 6 storage texel buffer)
int g_gw_site = 0;
uint64_t g_bump_site[8] = {};
void gpu_wrote(uint64_t addr, uint64_t size, int use) {
    // [nvn] set_reuse: a write changes how bindings resolve only when it makes copied pages stale or marks an update range written
    if (upd_note_gpu_write(addr, size)) { desc_epoch_bump(kBumpWrite); ++g_bump_use[8]; }
    note_gw(addr, size);
    capture::note_gpu_write(addr, size);
    if (note_gpu_write(addr, size, use)) {
        desc_epoch_bump(kBumpWrite);
        ++g_bump_use[static_cast<unsigned>(use) % 8];
        ++g_bump_site[g_gw_site & 7];
        g_dirty |= kVb | kDesc;
        g_mirror_stale = true;
    }
}
const ProgLayout* g_pl = nullptr;
VkPipeline g_pipe = VK_NULL_HANDLE;
VkPrimitiveTopology g_topo = VK_PRIMITIVE_TOPOLOGY_MAX_ENUM;
VkPrimitiveTopology g_pipe_topo = VK_PRIMITIVE_TOPOLOGY_MAX_ENUM;
bool g_restart = false, g_restart_valid = false;
uint32_t g_key_attr_count = 0;
uint8_t g_key_binding[16] = {};
// Occlusion counter (NVN_COUNTER_TYPE_SAMPLES_PASSED): ResetCounter starts a Vulkan occlusion query outside any pass; it stays active across
// the passes that follow (the console counts continuously); ReportCounter ends it and copies its 64-bit result into the report.
// [nvn] occlusion_in_pass (default on; OoT makes ~700 reset/draw/report triples per frame, and each one ended the render pass twice): only
// draws pass samples, and draws are inside passes, so the query is begun inside the pass instead: at ResetCounter when a pass is open, else
// when the next pass begins (g_occ_want), on a slot reset beforehand (runs of slots are reset whenever the replay is outside a pass anyway).
// A pass that ends ends the query with it ("part"); the next pass begins another part. The report ends the active part, and its write and the
// copy of the query result wait in g_occ_pend until the pass ends (end_pass: one barrier and one run of updates and copies for all of
// them). Only the CPU reads these reports (conditional rendering on a samples-passed report is decided at record time and renders), so no
// GPU work misses them. The report gets the result of the part with draws; when several parts have draws (rare: "split" in the stats line;
// counts cannot be added on the GPU without a shader) it gets kOccSplitCount, as if the test geometry were visible: an occluded object is
// then drawn (and stays hidden behind its occluder) rather than a visible one left out.
constexpr uint32_t kOccSlots = 4096;
constexpr uint32_t kOccResetRun = 256;
constexpr uint64_t kOccSplitCount = 1u << 20;
VkQueryPool g_occ_pool = VK_NULL_HANDLE;
bool g_occ_tried = false;
uint32_t g_occ_next = 0;             // the next slot to use (a counter; slot = value % kOccSlots)
uint32_t g_occ_reset_hi = 0;         // slots [g_occ_next, g_occ_reset_hi) are reset already and unused
int32_t g_occ_active = -1, g_occ_last = -1;
bool g_occ_in_pass = false;          // the active query was begun inside the open pass
bool g_occ_want = false;             // a reset counter is counting: the next pass begins a part
int32_t g_occ_result_slot = -1, g_occ_any_slot = -1;   // this counter's last part with draws, its last part
uint32_t g_occ_parts_drawn = 0;      // parts of this counter with draws
uint32_t g_occ_begin_draws = 0;      // g_pass_draws when the active part began
struct OccReport { uint64_t addr; uint64_t timestamp; int32_t slot; uint64_t counter = 0; };   // counter: written when slot < 0
std::vector<OccReport> g_occ_pend;   // reports made in the open pass: written when it ends
uint64_t g_occ_fast = 0, g_occ_slow = 0, g_occ_split = 0;   // statistics

bool occlusion_in_pass_on() {
    static const bool on = nxrt::config_get_bool("nvn", "occlusion_in_pass", true);
    return on;
}

// [nvn] occ_defer (cpuperf_ab_what name occ_defer): the reports wait for the end of the submission (or a reset of their slots) instead of
// the end of their pass: one barrier triple per submission instead of one per pass with reports (~45 per frame in Kokiri outside). Only the
// CPU reads them, after the submission's serial; a GPU command of the same submission that read a report word would see the old value.
bool occ_defer_on();
// The reports of the pass that just ended (outside any pass now): their {0, timestamp} words, then the query results over the counters.
void occ_flush_pending() {
    if (g_occ_pend.empty()) return;
    memory_barrier(F->cb);
    for (const OccReport& q : g_occ_pend) {
        const BufRef b = resolve_buffer(q.addr, 16);
        if (!b.ok) continue;
        struct { uint64_t counter, timestamp; } rep = {q.slot < 0 ? q.counter : 0, q.timestamp};
        vkCmdUpdateBuffer(F->cb, b.buffer, b.offset, sizeof rep, &rep);
    }
    memory_barrier(F->cb);
    for (const OccReport& q : g_occ_pend) {
        if (q.slot < 0) continue;
        const BufRef b = resolve_buffer(q.addr, 16);
        if (b.ok) vkCmdCopyQueryPoolResults(F->cb, g_occ_pool, static_cast<uint32_t>(q.slot), 1, b.buffer, b.offset, 16, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
    }
    memory_barrier(F->cb);
    g_occ_pend.clear();
}

// Outside a pass: resets the next run of slots so that queries can begin inside the passes that follow.
void occ_reset_ahead() {
    if (!g_occ_pool || !occlusion_in_pass_on() || static_cast<int32_t>(g_occ_reset_hi - g_occ_next) >= static_cast<int32_t>(kOccResetRun / 4)) return;
    occ_flush_pending();                 // copies of results must read their slots before any slot is reset again
    const uint32_t from = static_cast<int32_t>(g_occ_reset_hi - g_occ_next) > 0 ? g_occ_reset_hi : g_occ_next;
    const uint32_t to = g_occ_next + kOccResetRun;
    for (uint32_t a = from; a != to;) {
        const uint32_t slot = a % kOccSlots;
        const uint32_t n = std::min(to - a, kOccSlots - slot);
        vkCmdResetQueryPool(F->cb, g_occ_pool, slot, n);
        a += n;
    }
    g_occ_reset_hi = to;
}

// Inside the open pass: begins a part of the counting counter on a slot reset ahead. False: no such slot.
bool occ_begin_in_pass() {
    if (!P.open || static_cast<int32_t>(g_occ_reset_hi - g_occ_next) <= 0) return false;
    const uint32_t slot = g_occ_next++ % kOccSlots;
    vkCmdBeginQuery(F->cb, g_occ_pool, slot, device().features().occlusion_query_precise ? VK_QUERY_CONTROL_PRECISE_BIT : 0);
    g_occ_active = static_cast<int32_t>(slot);
    g_occ_in_pass = true;
    g_occ_want = false;
    g_occ_begin_draws = g_pass_draws;
    return true;
}

// The active in-pass part was ended (vkCmdEndQuery recorded).
void occ_part_done() {
    if (g_pass_draws > g_occ_begin_draws) {
        g_occ_result_slot = g_occ_active;
        ++g_occ_parts_drawn;
    }
    g_occ_any_slot = g_occ_active;
    g_occ_active = -1;
    g_occ_in_pass = false;
}

bool occ_ready() {
    if (g_occ_pool) return true;
    if (g_occ_tried || !device().features().occlusion_query) return false;
    g_occ_tried = true;
    VkQueryPoolCreateInfo qi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qi.queryType = VK_QUERY_TYPE_OCCLUSION;
    qi.queryCount = kOccSlots;
    if (vkCreateQueryPool(vkdev(), &qi, nullptr, &g_occ_pool) != VK_SUCCESS) g_occ_pool = VK_NULL_HANDLE;
    return g_occ_pool != VK_NULL_HANDLE;
}

uint8_t g_key_dummy_binding = 0;      // 1 + the binding that feeds the defaults of unset vertex attributes (0: none)
bool g_want_depth_ro = false;         // some texture this draw samples is the current depth attachment
bool g_vb_bound_in_cb = false;        // vkCmdBindVertexBuffers2 was called in this command buffer (dynamic strides need one)

// [nvn] trace_frame = N (NXRT_NVN_TRACE_FRAME): every draw, clear, copy and dispatch of the frame that is presented as number N is logged
// at Info ("trace ..." lines): programs, render targets, sampled textures, blend and depth state. For finding out what a frame does.
bool g_xfb_active = false;
bool g_pipe_xfb = false;             // the bound pipeline is a stream-out variant
bool g_xfb_began = false;            // vkCmdBeginTransformFeedbackEXT is in effect in the open pass
bool g_xfb_have_counter = false;     // a counter buffer holds the bytes written so far: the next begin continues after them
gfx::Buffer g_xfb_counter;
// One byte counter per feedback control buffer (NVN's Begin/End/Draw take its address): slot * 16 bytes into g_xfb_counter. vkCmdEndTransformFeedbackEXT
// writes the bytes of the section there; DrawTransformFeedback reads them (vkCmdDrawIndirectByteCountEXT). written: the slot holds a count.
constexpr uint32_t kXfbSlots = 256, kXfbSlotBytes = 16;
std::unordered_map<uint64_t, uint32_t> g_xfb_slot_of;
uint32_t g_xfb_slot_next = 0, g_xfb_cur = 0;
bool g_xfb_slot_written[kXfbSlots] = {};
// Windowed GPU counters for the primitive counters (13 = primitives generated, 14 = feedback primitives written): a ResetCounter whose window
// (up to the matching ReportCounter, in the same list) holds an indirect or feedback draw starts a query the GPU counts those draws with;
// the CPU counts the other draws exactly (count_primitives) but cannot know the primitives of a draw whose arguments the GPU holds.
// [0] pipeline statistics (input assembly primitives; a converted quad counts as its two triangles), [1] transform feedback stream 0 query.
struct CounterWindow { VkQueryPool pool = VK_NULL_HANDLE; bool tried = false; uint32_t next = 0; int32_t active = -1, last = -1; };
constexpr uint32_t kCounterSlots = 64;
CounterWindow g_cw[2];
int g_dbg_depth = 0;                  // debug labels open in this submission (PushDebugGroup / PopDebugGroup)
// Primitive counters (NVN_COUNTER_TYPE_PRIMITIVES_GENERATED = 13, TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN = 14): counted on the CPU by every
// draw (count_primitives), process-wide like the console's, zeroed by ResetCounter of their type, dropped at shutdown.
uint64_t g_prims_generated = 0, g_prims_xfb = 0;
// Conditional render (SetRenderEnableConditional): the console compares the 64-bit counter words of two reports, at the address and
// 16 bytes after it (yuzu Maxwell3D::ProcessQueryCondition: IfEqual / IfNotEqual), when the command runs. The replay runs in GPU
// order, so the counters it writes itself (ReportCounter other than samples passed, ReportValue) are known here: g_reports keeps
// them by address. kReportUnknown marks an address the GPU writes later (samples passed): a condition on it renders, as before. An
// address the replay never wrote (the game's own memory) is read from the pool.
constexpr uint64_t kReportUnknown = ~0ull;
std::unordered_map<uint64_t, uint64_t> g_reports;
nxrt::GaugeCounter g_gauge_pipeline_binds("nvn.cnt.pipeline_binds");   // graphics pipeline binds (cumulative)
VkQueryPool g_xq_pool = VK_NULL_HANDLE;     // transform feedback stream queries (counters 13 and 14)
bool g_trace = false;
struct TraceSnap { int index; int draw; std::string name; std::string desc; };
std::vector<TraceSnap> g_trace_snaps;
std::vector<int> g_trace_snap_draws;   // [nvn] trace_snap: draw numbers after which the targets are copied
bool g_trace_snap_read = false;
std::vector<std::string> g_trace_snap_fs;
bool g_trace_snap_sampled = false;           // [nvn] trace_snap_sampled = 1: a snapshot draw also copies the textures that draw samples
std::vector<Texture*> g_trace_sampled;       // textures sampled by the last traced draw
std::vector<Ref<Texture>> g_trace_targets;   // the colour and depth targets drawn to in the traced frame, in order of first use
int64_t g_trace_frame = -2;
std::vector<int64_t> g_trace_extra;   // more frames to trace
// [nvn] trace_after_fs = "67d504": the frame after the first one that draws with a fragment shader of this hash prefix is traced
// (a frame number is not stable from run to run: the load times differ).
std::string g_trigger_fs;

// Debug: [nvn] skip_fs_file = FILE (env NXRT_NVN_SKIP_FS_FILE). FILE holds fragment shader hash prefixes separated by blanks; draws whose fragment
// shader starts with one of them are not drawn. The file is read again twice a second, so it can change while the game runs.
std::vector<std::string> g_skip_fs;
int g_draw_idx = 0;   // draws begun in this frame
bool skip_fs_draw(const std::string& hash) {
    static const std::string file = nxrt::config_get_str("nvn", "skip_fs_file", "");
    if (file.empty()) return false;
    static std::chrono::steady_clock::time_point last{};
    const auto now = std::chrono::steady_clock::now();
    if (last.time_since_epoch().count() == 0 || now - last > std::chrono::milliseconds(500)) {
        last = now;
        std::vector<std::string> prev;
        prev.swap(g_skip_fs);
        std::ifstream f(file);
        std::string w;
        while (f >> w) g_skip_fs.push_back(w);
        // Measurement words: "bar" drops every full barrier, "bar:<line>" those of one source line (g_exp_bar_drop); "c:<prefix>" leaves out
        // dispatches (do_dispatch). A change of the set is marked in the frame trace ("skip:<words>") for per-window GPU times.
        int drop = 0;
        for (const std::string& p : g_skip_fs)
            if (p == "bar") drop = -1;
            else if (p.compare(0, 4, "bar:") == 0 && drop >= 0) drop = std::atoi(p.c_str() + 4);
        g_exp_bar_drop.store(drop, std::memory_order_relaxed);
        static bool marked = false;
        if (prev != g_skip_fs || !marked) {
            marked = true;
            std::string m = "skip:";
            for (const std::string& p : g_skip_fs) m += p + "_";
            if (m.size() > 120) m.resize(120);
            ft::mark(m.c_str(), static_cast<uint32_t>(g_skip_fs.size()));
        }
    }
    for (const std::string& p : g_skip_fs) {
        if (!p.empty() && p[0] == '#') {   // "#198" or "#198-210": the draw numbers of the frame (the first draw is #1, as in the trace)
            const int a = std::atoi(p.c_str() + 1);
            const size_t dash = p.find('-');
            const int b = dash == std::string::npos ? a : std::atoi(p.c_str() + dash + 1);
            if (g_draw_idx >= a && g_draw_idx <= b) return true;
        } else if (hash.compare(0, p.size(), p) == 0) return true;
    }
    return false;
}
bool g_trigger_read = false, g_trigger_seen = false, g_trigger_done = false;
int64_t g_trigger_present = -1;
int g_trace_n = 0;
std::atomic<bool> g_trace_req{false};   // backend_trace_next_frame(): trace the first whole frame after this flag is seen
#define TRACE(...) do { if (g_trace) NXLOG_I("nvn.trace", __VA_ARGS__); } while (0)

const char* tex_desc(const Texture* t, char* buf, size_t n) {
    if (!t) { std::snprintf(buf, n, "-"); return buf; }
    std::snprintf(buf, n, "t%u:%s %dx%dx%d L%u/%u%s%s", t->id, t->fmt ? t->fmt->name : "?", t->b.width, t->b.height, t->b.depth, t->levels, t->layers,
                  t->b.samples > 1 ? " ms" : "", t->b.target == NVN_TEXTURE_TARGET_3D ? " 3D" : "");
    return buf;
}

// Percentage of the texture's storage (in the game's memory, first 16 MiB) that holds a non-zero 8-byte word; -1 without storage. Trace only.
int storage_nonzero_pct(const Texture* t) {
    if (!t || !t->mem || !t->storage_size) return -1;
    const uint64_t n = std::min<uint64_t>(t->storage_size, 16ull << 20) / 8;
    if (!n) return -1;
    uint64_t nz = 0;
    for (uint64_t i = 0; i < n; ++i) {
        uint64_t w;
        std::memcpy(&w, t->mem + i * 8, 8);
        nz += w != 0;
    }
    return static_cast<int>(nz * 100 / n);
}

void log_once(const char* key, const char* msg) {   // render thread only; counted every time, logged once
    // Keys are literals: looked up by address (a per-op call costs one hash look-up and an atomic add), the text compared to catch a
    // reused address.
    struct Seen { const char* key; const char* msg; nxrt::GapCounter* gap; };
    static std::unordered_map<const char*, Seen> by_addr;
    static std::unordered_map<std::string, nxrt::GapCounter*> by_text;   // key text + msg text -> counter (logged once per text)
    auto it = by_addr.find(key);
    if (it == by_addr.end() || it->second.msg != msg || std::strcmp(it->second.key, key) != 0) {
        std::string k = std::string(key) + '\x1f' + msg;
        auto [t, fresh] = by_text.try_emplace(std::move(k), nullptr);
        if (fresh) {
            t->second = new nxrt::GapCounter("nvn", msg);   // never freed (registered)
            NXLOG_W(kTag, "%s", msg);
        }
        it = by_addr.insert_or_assign(key, Seen{key, msg, t->second}).first;
    }
    it->second.gap->hit();
}

bool first_time_key(const std::string& key) {   // render thread only
    static std::unordered_map<std::string, bool> seen;
    return seen.size() < 4096 && seen.emplace(key, true).second;
}

// ---- helpers ----------------------------------------------------------------------------------------------------------------
VkFormat scaled_for(VkFormat f) {
    switch (f) {
        case VK_FORMAT_R8_UINT: return VK_FORMAT_R8_USCALED;
        case VK_FORMAT_R8G8_UINT: return VK_FORMAT_R8G8_USCALED;
        case VK_FORMAT_R8G8B8_UINT: return VK_FORMAT_R8G8B8_USCALED;
        case VK_FORMAT_R8G8B8A8_UINT: return VK_FORMAT_R8G8B8A8_USCALED;
        case VK_FORMAT_R16_UINT: return VK_FORMAT_R16_USCALED;
        case VK_FORMAT_R16G16_UINT: return VK_FORMAT_R16G16_USCALED;
        case VK_FORMAT_R16G16B16_UINT: return VK_FORMAT_R16G16B16_USCALED;
        case VK_FORMAT_R16G16B16A16_UINT: return VK_FORMAT_R16G16B16A16_USCALED;
        case VK_FORMAT_A2B10G10R10_UINT_PACK32: return VK_FORMAT_A2B10G10R10_USCALED_PACK32;
        case VK_FORMAT_R8_SINT: return VK_FORMAT_R8_SSCALED;
        case VK_FORMAT_R8G8_SINT: return VK_FORMAT_R8G8_SSCALED;
        case VK_FORMAT_R8G8B8_SINT: return VK_FORMAT_R8G8B8_SSCALED;
        case VK_FORMAT_R8G8B8A8_SINT: return VK_FORMAT_R8G8B8A8_SSCALED;
        case VK_FORMAT_R16_SINT: return VK_FORMAT_R16_SSCALED;
        case VK_FORMAT_R16G16_SINT: return VK_FORMAT_R16G16_SSCALED;
        case VK_FORMAT_R16G16B16_SINT: return VK_FORMAT_R16G16B16_SSCALED;
        case VK_FORMAT_R16G16B16A16_SINT: return VK_FORMAT_R16G16B16A16_SSCALED;
        case VK_FORMAT_A2B10G10R10_SINT_PACK32: return VK_FORMAT_A2B10G10R10_SSCALED_PACK32;
        default: return f;
    }
}

bool is_int_format(VkFormat f) { return scaled_for(f) != f; }

VkImageViewType view_type_for(const DescriptorBinding& b) {
    const ImageShape& s = b.image;
    switch (s.dim) {
        case 0: return s.arrayed ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D;
        case 1: return s.arrayed ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
        case 2: return VK_IMAGE_VIEW_TYPE_3D;
        case 3: return s.arrayed ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : VK_IMAGE_VIEW_TYPE_CUBE;
        default: return VK_IMAGE_VIEW_TYPE_2D;
    }
}

// ---- passes ----------------------------------------------------------------------------------------------------------------
// Ends transform feedback in the open pass, remembering the bytes written in the counter buffer (a pass break must not restart the buffer).
void xfb_end_in_pass() {
    if (!g_xfb_began) return;
    VkBuffer cbuf = g_xfb_counter.buffer;
    const VkDeviceSize off = static_cast<VkDeviceSize>(g_xfb_cur) * kXfbSlotBytes;
    vkCmdEndTransformFeedbackEXT(F->cb, 0, 1, &cbuf, &off);
    g_xfb_began = false;
    g_xfb_have_counter = true;
    g_xfb_slot_written[g_xfb_cur] = true;
}

// GPU pass profiler: closes the interval since the previous stamp under this label (no-op unless [prof] gpu_passes).
void gstamp(const char* label) {
    if (gpass_on()) gpass_stamp(*F, label, 0);
}
// [prof] gpu_draws (with gpu_passes): a stamp after every draw, labelled "d <fragment shader> <vertex or mesh shader>". The stamps
// serialize the draws, so each interval is that draw's whole time without overlap: they rank draws (their sum is about the pass time).
void draw_stamp() {
    static const bool on = nxrt::config_get_bool("prof", "gpu_draws", false);
    if (!on || !gpass_on()) return;
    const ShaderInfo* fs = S.stage[kFragment];
    const ShaderInfo* vs = S.stage[kVertex] ? S.stage[kVertex] : S.stage[kMesh];
    char b[64];
    std::snprintf(b, sizeof b, "d %.8s %.8s", fs ? fs->hash.c_str() : "-", vs ? vs->hash.c_str() : "-");
    gpass_stamp(*F, b, 0);
}

// Two texture objects over one image (as shares_image further down, which this code cannot see yet).
bool same_image(Texture* a, Texture* b) { return a == b || (a && b && a->gpu && a->gpu == b->gpu); }

// to_pass: the caller only starts another pass or changes image layouts next (ensure_pass, prepare_textures); with tracked barriers the pass
// then ends without a barrier when it only wrote its attachments. Any other caller gets a full barrier, also for passes ended earlier
// without one.
// [nvn] write_defer (default on): GPU-ordered memory writes (do_write_memory: counter reports, report values, events) recorded while a
// render pass is open used to end the pass (vkCmdUpdateBuffer is not allowed inside one): ~34 per frame in OoT's first scene, each splitting
// a 1920x1080 pass and draining the GPU twice. They wait until the pass ends instead, with one pair of barriers for all of them. What reads
// them is the CPU after the submission (or the replay's own report bookkeeping, which is kept at record time); a draw of the same pass that
// read such a word in game memory would see the old value.
struct DeferredWrite { VkBuffer buf; VkDeviceSize off; uint32_t size; size_t data; };
std::vector<DeferredWrite> g_wdefer;
std::vector<uint8_t> g_wdefer_data;
uint64_t g_wdefer_n = 0, g_wdefer_flushes = 0;
void flush_deferred_writes() {
    if (g_wdefer.empty()) return;
    memory_barrier(F->cb);
    for (const DeferredWrite& w : g_wdefer) vkCmdUpdateBuffer(F->cb, w.buf, w.off, w.size, g_wdefer_data.data() + w.data);
    memory_barrier(F->cb);
    g_wdefer.clear();
    g_wdefer_data.clear();
    ++g_wdefer_flushes;
    gstamp("write memory");
}

// Which call sites end passes (the 10 s line "passes ended by": source line and the draws the pass had). Render thread.
uint64_t g_pass_end_line[8192] = {};
uint64_t g_pass_end_draws[8192] = {};
void end_pass(bool to_pass = false, std::source_location loc = std::source_location::current()) {
    if (P.open) {
        const uint32_t ln = loc.line() & 8191;
        ++g_pass_end_line[ln];
        g_pass_end_draws[ln] += g_pass_draws;
    }
    if (!P.open) {
        if (!to_pass && !g_pending.empty()) {
            memory_barrier(F->cb);
            g_pending.clear();
        }
        return;
    }
    xfb_end_in_pass();
    if (g_occ_active >= 0 && g_occ_in_pass) {      // a part begun in this pass ends with it; the next pass begins another
        vkCmdEndQuery(F->cb, g_occ_pool, static_cast<uint32_t>(g_occ_active));
        occ_part_done();
        g_occ_want = true;
    }
    vkCmdEndRendering(F->cb);
    P.open = false;
    flush_deferred_writes();
    if (g_pass_draws) g_pass_size_draws[(uint64_t{P.extent.width} << 32) | P.extent.height] += g_pass_draws;
    if (!occ_defer_on()) occ_flush_pending();
    if (g_occ_pool && g_occ_want) occ_reset_ahead();
    if (gpass_on()) {
        char b[112];
        std::snprintf(b, sizeof b, "pass %ux%u c%u:%d d%d%s", P.extent.width, P.extent.height, P.ncolor, static_cast<int>(P.color_fmt[0]), static_cast<int>(P.depth_fmt),
                      P.depth_ro ? " ro" : "");
        gpass_stamp(*F, b, g_pass_draws);
        char vb[128];
        std::snprintf(vb, sizeof vb, "gv:%s", b);
        ft::mark(vb, static_cast<uint32_t>(std::min<uint64_t>(g_pass_verts, 0xffffffffull)));
    }
    if (to_pass && g_tracked && !g_pass_unsafe) {
        auto remember = [](Texture* t, VkImageLayout l) {
            if (!t || !tex_gpu(t)) return;
            for (const PendingWrite& w : g_pending) if (w.tex.get() == t) return;
            g_pending.push_back({Ref<Texture>::share(t), l});
        };
        for (uint32_t i = 0; i < P.ncolor; ++i) remember(P.color[i].tex, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        if (!P.depth_ro) remember(P.depth.tex, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    } else {
        memory_barrier(F->cb);
        g_pending.clear();
    }
    g_pass_unsafe = false;
    g_bound_pipeline = VK_NULL_HANDLE;
    g_dirty = kAllDirty;
}

// ---- submission chunks ([nvn] submit_chunk_draws) ------------------------------------------------------------------------------------
// A frame's main submission (OoT: ~13,000 draws) used to reach the GPU only when all of it was recorded, so recording and GPU execution of
// it ran one after the other. With chunks, what is recorded so far goes to the GPU (frame_chunk, no wait) whenever enough draws and
// dispatches have been recorded since the last part, at a point where no render pass is open: between passes (ensure_pass) or between
// commands outside a pass (run_list). [nvn] submit_chunk_pass_draws (default 256) additionally ends a long pass for it (the pass then goes on with
// LOAD). Only command-buffer state is reset afterwards; barriers owed to earlier work stay owed (a barrier also orders against work
// submitted before it), and the submission's serial still signals at its very end. 0 = off.
struct ChunkCfg { uint32_t draws = 0, pass_draws = 0; uint64_t ns = 0; };
const ChunkCfg& chunk_cfg() {
    static const ChunkCfg c = [] {
        ChunkCfg k;
        k.draws = static_cast<uint32_t>(std::clamp<int64_t>(nxrt::config_get_int("nvn", "submit_chunk_draws", 256), 0, 1 << 20));
        k.pass_draws = static_cast<uint32_t>(std::clamp<int64_t>(nxrt::config_get_int("nvn", "submit_chunk_pass_draws", 256), 0, 1 << 20));
        k.ns = static_cast<uint64_t>(std::clamp<int64_t>(nxrt::config_get_int("nvn", "submit_chunk_us", 0), 0, 1000000)) * 1000;
        if (k.draws || k.pass_draws)
            NXLOG_I("nvn.replay", "submission chunks: every %u draws/dispatches between passes, %u inside a pass, %llu us", k.draws, k.pass_draws,
                    static_cast<unsigned long long>(k.ns / 1000));
        return k;
    }();
    return c;
}
uint32_t g_chunk_work = 0;    // draws and dispatches recorded since the submission began or the last chunk
uint64_t g_chunk_t0 = 0;      // ft::now_ns() then

bool chunk_due(uint32_t threshold, AbFeat feat = kAbChunks) {
    if (!threshold || !g_chunk_work || !ab_on(feat)) return false;
    if (g_chunk_work >= threshold) return true;
    const ChunkCfg& c = chunk_cfg();
    return c.ns && ab_on(kAbChunkTime) && ft::now_ns() - g_chunk_t0 >= c.ns;   // cpuperf_ab_what chunk_time: the time limit only in the on windows
}

// No pass open: sends the recorded part to the GPU. Skipped while a query is active (it cannot span command buffers) or stream-out runs.
void chunk_now() {
    if (!F || P.open || g_occ_active >= 0 || g_cw[0].active >= 0 || g_cw[1].active >= 0 || g_xfb_began) return;
    if (!frame_chunk(*F)) return;
    g_bound_pipeline = VK_NULL_HANDLE;   // a new command buffer: nothing bound, no dynamic state
    g_dirty = kAllDirty;
    g_topo = VK_PRIMITIVE_TOPOLOGY_MAX_ENUM;
    g_restart_valid = false;
    g_vb_bound_in_cb = false;
    g_chunk_work = 0;
    g_chunk_t0 = ft::now_ns();
}

// Between two commands of a list.
void chunk_at_op() {
    const ChunkCfg& c = chunk_cfg();
    if (!P.open) {
        if (chunk_due(c.draws)) chunk_now();
    } else if (chunk_due(c.pass_draws, kAbPassChunks)) {
        end_pass(true);
        chunk_now();
    }
}

// A pass is about to use `t` as an attachment in `layout`. An earlier pass that ended without a barrier wrote it, and no layout change
// (which would carry the dependency) happens in between: the two passes need a barrier.
bool pending_conflict(Texture* t, VkImageLayout layout) {
    if (!t || g_pending.empty()) return false;
    for (const PendingWrite& w : g_pending)
        if (same_image(w.tex.get(), t) && tex_layout(t) == w.layout && layout == w.layout) return true;
    return false;
}

Target make_target(Texture* t, const TextureViewInfo& v, bool has_view) {
    Target tg;
    tg.tex = t;
    if (has_view) {
        if (v.has_levels) tg.level = static_cast<uint32_t>(v.min_level);
        if (v.has_layers) {
            tg.layer = static_cast<uint32_t>(v.min_layer);
            tg.layers = std::max(1, v.num_layers);
        }
        else if (t && t->layers > 1 && t->b.target != NVN_TEXTURE_TARGET_3D) tg.layers = t->layers;   // a view without layers
        if (v.has_format) {
            const FormatInfo* fi = format_info(v.format);
            if (fi && fi->vk != VK_FORMAT_UNDEFINED) tg.view_format = fi->vk;
        }
    } else if (t && t->layers > 1 && t->b.target != NVN_TEXTURE_TARGET_3D) {
        // no view: an array or cube map target is attached with all its layers (layered rendering: a geometry shader picks the layer with
        // gl_Layer; without it the shaders draw to layer 0)
        tg.layers = t->layers;
    }
    return tg;
}

bool targets_match() {
    if (P.ncolor != S.ncolor) return false;
    for (uint32_t i = 0; i < S.ncolor; ++i)
        if (!(P.color[i] == make_target(S.color_tex[i].get(), S.color_view[i], S.color_has_view[i]))) return false;
    return P.depth == make_target(S.depth_tex.get(), S.depth_view, S.depth_has_view);
}

// The current render targets get their images (scaled when they qualify, tex_gpu_attachment). With `fix` (no pass open), as Ryujinx does
// when it binds render targets: a target whose image was made unscaled (first used another way) is scaled now, and the targets of one
// pass are brought to one scale (the lowest when one of them cannot be scaled). Returns the scale of the targets.
float attach_targets(bool fix) {
    Texture* ts[9];
    uint32_t n = 0;
    for (uint32_t i = 0; i < S.ncolor && i < 8; ++i) if (S.color_tex[i]) ts[n++] = S.color_tex[i].get();
    if (S.depth_tex) ts[n++] = S.depth_tex.get();
    float lo = 0.0f, hi = 0.0f;
    for (uint32_t i = 0; i < n; ++i) {
        if (!tex_gpu_attachment(ts[i])) continue;
        if (fix) {
            const float want = tex_scale_wanted(ts[i]);
            if (want != tex_scale(ts[i])) tex_rescale(ts[i], *F, want, "used as a render target");
        }
        const float sc = tex_scale(ts[i]);
        lo = lo == 0.0f ? sc : std::min(lo, sc);
        hi = std::max(hi, sc);
    }
    if (lo == 0.0f) return 1.0f;
    if (lo != hi && fix) {
        for (uint32_t i = 0; i < n; ++i)
            if (tex_gpu(ts[i]) && tex_scale(ts[i]) != lo) tex_rescale(ts[i], *F, lo, "a render target of the same pass cannot be scaled");
    }
    return lo;
}

// [nvn] epoch_keep_dispatch (default on): the held-result version (out_version) is in the set memo's and the draw buffer checks' epoch, so
// commands that only add or re-arm held results (dispatches, draws writing over held results) need not move the resolve epoch.
bool epoch_keep_on();
bool held_version_epoch() {
    static const bool on = nxrt::config_get_bool("nvn", "epoch_keep_dispatch", true);
    return on && epoch_keep_on() && ab_on(kAbEpochDispatch);
}

bool memo_pass_keep() {
    static const bool on = nxrt::config_get_bool("nvn", "memo_pass_keep", true);
    return on && ab_on(kAbMemoPass);
}

// [nvn] pass_pre_layout (default on; cpuperf_ab_what name pre_layout): a pass break for a texture that a draw samples while it is still in
// the attachment layout of an earlier pass. In Kokiri outside at 1080 prepare_textures ended ~45 open passes per frame, nearly all for depth
// images left in DEPTH_STENCIL_ATTACHMENT (the shadow maps, sampled through the A7 pool arrays) and some colour targets. An image that ended
// an open pass so once (g_break_prone, by its TexGpu) is put into its sampled layout when the next pass begins and it is not one of that
// pass's attachments: outside any pass, next to the pass's own layout changes. A layout change keeps the contents, so a wrong guess costs
// a transition back when the image is an attachment again, nothing else.
std::vector<Ref<Texture>> g_prev_att;              // break-prone attachments of this submission's passes not yet put back (render thread)
std::unordered_set<const void*> g_break_prone;     // images that ended an open pass for a layout change (keys only, never dereferenced)
uint64_t g_pre_layout_n = 0;                       // statistics
bool pre_layout_on() {
    static const bool on = nxrt::config_get_bool("nvn", "pass_pre_layout", true);
    return on && ab_on(kAbPreLayout);
}
bool attachment_layout(VkImageLayout l) { return l == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL || l == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL; }
// At a pass begin (before its attachments' own layout changes): every remembered image that is not an attachment of the new pass.
void pre_layout_prev_targets() {
    if (g_prev_att.empty()) return;
    if (!pre_layout_on()) { g_prev_att.clear(); return; }
    size_t keep = 0;
    for (size_t k = 0; k < g_prev_att.size(); ++k) {
        Texture* t = g_prev_att[k].get();
        if (!t || !t->gpu || !tex_gpu(t) || !attachment_layout(tex_layout(t))) continue;
        bool attached = same_image(t, S.depth_tex.get());
        for (uint32_t i = 0; i < S.ncolor && !attached; ++i) attached = same_image(t, S.color_tex[i].get());
        if (attached) { g_prev_att[keep++] = g_prev_att[k]; continue; }   // still in use: later
        tex_transition(t, *F, tex_sampled_layout(t));
        ++g_pre_layout_n;
    }
    g_prev_att.resize(keep);
}
// After a pass begins: its break-prone attachments are remembered.
void pre_layout_note(Texture* t) {
    if (!t || !t->gpu || !g_break_prone.count(t->gpu) || !pre_layout_on()) return;
    for (const Ref<Texture>& r : g_prev_att) if (same_image(r.get(), t)) return;
    if (g_prev_att.size() < 64) g_prev_att.push_back(Ref<Texture>::share(t));
}
// Starts a pass on the current render targets if none is open for them. False when there is nothing to render to.
void bind_skip_reset();   // below (bind_sets)
void pass_lookahead();   // below: settles the reads of the coming draws before the pass begins
bool ensure_pass(bool depth_ro = false) {
    if (P.open && targets_match() && P.depth_ro == depth_ro) return true;
    end_pass(true);
    if (chunk_due(chunk_cfg().draws)) chunk_now();
    if (!g_pending.empty()) {
        bool conflict = false;
        for (uint32_t i = 0; i < S.ncolor && !conflict; ++i) conflict = pending_conflict(S.color_tex[i].get(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        if (!conflict && S.depth_tex)
            conflict = pending_conflict(S.depth_tex.get(), depth_ro ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        if (conflict) {
            memory_barrier(F->cb);
            g_pending.clear();
        }
    }
    pre_layout_prev_targets();
    if (out_any()) pass_lookahead();
    if (gpass_on()) gpass_stamp(*F, "gap", 0);      // copies, dispatches, barriers and layout changes since the previous pass
    g_pass_draws = 0;
    g_pass_verts = 0;
    const float pass_scale = attach_targets(true);
    P = Pass();
    P.depth_ro = depth_ro;
    P.scale = pass_scale;
    VkRenderingAttachmentInfo colors[8] = {};
    VkRenderingAttachmentInfo depth = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    VkRenderingAttachmentInfo stencil = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    bool any = false;
    VkExtent2D extent = {UINT32_MAX, UINT32_MAX};
    P.ncolor = S.ncolor;
    for (uint32_t i = 0; i < S.ncolor; ++i) {
        colors[i] = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        Texture* t = S.color_tex[i].get();
        P.color[i] = make_target(t, S.color_view[i], S.color_has_view[i]);
        if (!t || !tex_gpu(t)) continue;
        const bool fresh = tex_layout(t) == VK_IMAGE_LAYOUT_UNDEFINED;
        tex_transition(t, *F, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        tex_mark_gpu_written(t);
        VkImageView v = tex_attachment_view(t, P.color[i].level, P.color[i].layer, P.color[i].layers, P.color[i].view_format);
        if (!v) continue;
        colors[i].imageView = v;
        colors[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colors[i].loadOp = fresh ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        colors[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        P.color_fmt[i] = tex_attachment_format(t, P.color[i].view_format);
        const VkExtent3D e = tex_image_extent(t, P.color[i].level);
        extent.width = std::min(extent.width, e.width);
        extent.height = std::min(extent.height, e.height);
        any = true;
    }
    Texture* dt = S.depth_tex.get();
    P.depth = make_target(dt, S.depth_view, S.depth_has_view);
    bool have_depth = false;
    if (dt && tex_gpu(dt)) {
        const bool fresh = tex_layout(dt) == VK_IMAGE_LAYOUT_UNDEFINED;
        const VkImageLayout dlayout = depth_ro ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        tex_transition(dt, *F, dlayout);
        if (!depth_ro) tex_mark_gpu_written(dt);
        VkImageView v = tex_attachment_view(dt, P.depth.level, P.depth.layer, P.depth.layers, P.depth.view_format);
        if (v) {
            depth.imageView = stencil.imageView = v;
            depth.imageLayout = stencil.imageLayout = dlayout;
            depth.loadOp = stencil.loadOp = (fresh && !depth_ro) ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
            depth.storeOp = stencil.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            depth.clearValue.depthStencil = {1.0f, 0};
            stencil.clearValue.depthStencil = {1.0f, 0};
            P.depth_fmt = tex_vk_format(dt);
            P.has_stencil = (tex_aspect(dt) & VK_IMAGE_ASPECT_STENCIL_BIT) != 0;
            const VkExtent3D e = tex_image_extent(dt, P.depth.level);
            extent.width = std::min(extent.width, e.width);
            extent.height = std::min(extent.height, e.height);
            have_depth = true;
            any = true;
        }
    }
    if (!any && g_xfb_active && device().features().transform_feedback) {   // a stream-out pass without attachments
        extent = {1, 1};
        any = true;
    }
    if (!any || extent.width == UINT32_MAX) return false;
    P.extent = extent;
    P.samples = std::max<uint32_t>(1, S.color_tex[0] ? static_cast<uint32_t>(S.color_tex[0]->b.samples) : 1);
    if (P.samples == 0) P.samples = 1;
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, extent};
    uint32_t layer_count = UINT32_MAX;     // layered rendering: the layers every attachment has in common
    for (uint32_t i = 0; i < S.ncolor; ++i)
        if (colors[i].imageView) layer_count = std::min(layer_count, std::max<uint32_t>(1, P.color[i].layers));
    if (have_depth) layer_count = std::min(layer_count, std::max<uint32_t>(1, P.depth.layers));
    ri.layerCount = layer_count == UINT32_MAX ? 1 : layer_count;
    uint32_t last = 0;
    for (uint32_t i = 0; i < S.ncolor; ++i)
        if (colors[i].imageView) last = i + 1;
    ri.colorAttachmentCount = S.ncolor;
    ri.pColorAttachments = colors;
    if (have_depth) {
        ri.pDepthAttachment = &depth;
        if (P.has_stencil) ri.pStencilAttachment = &stencil;
    }
    (void)last;
    gstamp("pass setup");
    if (g_occ_want) occ_reset_ahead();          // outside the pass still: a slot for the counter's part in it
    vkCmdBeginRendering(F->cb, &ri);
    bind_skip_reset();
    P.open = true;
    // attachments, layouts and feedback changed: sets resolve anew. [nvn] memo_pass_keep (default on): not needed for the set memo, whose
    // signature holds the render targets (feedback dummies) and whose epoch holds g_tex_image_gen (views), while sampled layouts are fixed per format
    if (!memo_pass_keep()) desc_epoch_bump(kBumpPass);
    if (g_occ_want) occ_begin_in_pass();
    counters().passes.fetch_add(1, std::memory_order_relaxed);
    g_bound_pipeline = VK_NULL_HANDLE;
    if (!g_break_prone.empty()) {   // pass_pre_layout
        for (uint32_t i = 0; i < S.ncolor; ++i) pre_layout_note(S.color_tex[i].get());
        if (!depth_ro) pre_layout_note(S.depth_tex.get());
    }
    return true;
}

// ---- viewport conversion -------------------------------------------------------------------------------------------------
VkRect2D scissor_rect(const int32_t* r) {
    int32_t x = r[0], y = r[1], w = r[2], h = r[3];
    if (P.scale != 1.0f) {     // the edges scale (a rectangle covering the target still covers it)
        const float sc = P.scale;
        const int32_t x1 = static_cast<int32_t>(std::ceil((static_cast<float>(x) + static_cast<float>(w)) * sc - 0.001f));
        const int32_t y1 = static_cast<int32_t>(std::ceil((static_cast<float>(y) + static_cast<float>(h)) * sc - 0.001f));
        x = static_cast<int32_t>(std::floor(static_cast<float>(x) * sc + 0.001f));
        y = static_cast<int32_t>(std::floor(static_cast<float>(y) * sc + 0.001f));
        w = x1 - x;
        h = y1 - y;
    }
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    w = std::max(0, std::min<int32_t>(w, static_cast<int32_t>(P.extent.width) - x));
    h = std::max(0, std::min<int32_t>(h, static_cast<int32_t>(P.extent.height) - y));
    return {{x, y}, {static_cast<uint32_t>(w), static_cast<uint32_t>(h)}};
}

void set_viewports_and_scissors() {
    VkCommandBuffer cb = F->cb;
    VkViewport vp[16];
    VkRect2D sc[16];
    const uint32_t nvp = S.nvp ? S.nvp : 1;
    for (uint32_t i = 0; i < nvp; ++i) {
        float x = S.vp[i][0], y = S.vp[i][1], w = S.vp[i][2], h = S.vp[i][3];
        if (!S.nvp) { x = 0; y = 0; w = static_cast<float>(P.extent.width); h = static_cast<float>(P.extent.height); }
        else if (P.scale != 1.0f) { x *= P.scale; y *= P.scale; w *= P.scale; h *= P.scale; }
        // Upper-left origin: window y is the image row from the top and NDC +y is the top edge of the rectangle (negative height).
        // Lower-left origin (the GL convention): window y 0 is memory row 0 and NDC -y is the first row, so no flip.
        const bool lower_left = S.window_origin == NVN_WINDOW_ORIGIN_MODE_LOWER_LEFT;
        vp[i] = lower_left ? VkViewport{x, y, w, h, S.dr[i][0], S.dr[i][1] == 0.0f && S.dr[i][0] == 0.0f ? 1.0f : S.dr[i][1]}
                           : VkViewport{x, y + h, w, -h, S.dr[i][0], S.dr[i][1] == 0.0f && S.dr[i][0] == 0.0f ? 1.0f : S.dr[i][1]};
        if (i < S.nsc) {
            int32_t r[4];
            bool exact = true;
            small_exclusive_scissor(S.small, i, S.sc[i], r, &exact);   // exclusive scissor (nvn_small.h): the part of the scissor it leaves, when that is one rectangle
            if (!exact) log_once("excl-scissor", "exclusive scissor leaves a hole in the scissor: one rectangle cannot express it, the draw is not clipped by it");
            sc[i] = scissor_rect(r);
        }
        else sc[i] = {{0, 0}, P.extent};
    }
    vkCmdSetViewportWithCount(cb, nvp, vp);
    vkCmdSetScissorWithCount(cb, nvp, sc);
}

// ---- descriptor sets ---------------------------------------------------------------------------------------------------------
struct TexResolved {
    Texture* tex = nullptr;
    TextureViewInfo view;
    bool has_view = false;
    SamplerInfo smp;
    bool has_sampler = false;
};

// The scale nxrt/gfx_tuning.h asks for on uniform block `idx` of this binding's shader (1 = none): only the chosen fragment shader's block.
float ufs_scale_for(const BindingDesc& d, int idx) {
    const nxrt::gfx::Tuning& t = nxrt::gfx::tuning();
    const uint64_t want = t.ufs_hash.load(std::memory_order_relaxed);
    if (!want || d.nvn_stage != kFragment || idx != t.ufs_block.load(std::memory_order_relaxed)) return 1.0f;
    const float sc = t.ufs_scale.load(std::memory_order_relaxed);
    if (sc == 1.0f || !d.shader || std::strtoull(d.shader->hash.c_str(), nullptr, 16) != want) return 1.0f;
    return sc;
}

// ubo: the stage's 16 uniform block bindings (S.ubo[stage], or the texture look-ahead's simulated ones)
uint32_t read_handle_word_in(const GState::Bind* ubo, int bank, int word) {
    // a texture handle the game wrote into uniform block (bank - 3)
    const int ub = bank - 3;
    if (ub < 0 || ub >= 16) return 0;
    const GState::Bind& b = ubo[ub];
    if (!b.addr || static_cast<uint64_t>(word) * 4 + 4 > b.size) return 0;
    uint32_t v = 0;
    std::memcpy(&v, host_ptr(b.addr) + static_cast<size_t>(word) * 4, 4);
    return v;
}
uint32_t read_handle_word(int stage, int bank, int word) { return read_handle_word_in(S.ubo[stage], bank, word); }

int storage_slot_for(int nvn_stage, int c0_offset);

// The texture look-ahead's thread (below): whether the 4 KiB page of addr holds a held compute result, asked once per page and scan
// (out_overlaps off the render thread takes the held results' lock; a scan reads tens of thousands of storage-held handles).
FlatMap64<uint8_t> g_tl_pages;   // look-ahead thread only: page -> 1 held, 2 not; cleared at each scan
bool tl_page_held(uint64_t addr) {
    const uint64_t page = addr >> 12;
    if (const uint8_t* v = g_tl_pages.find(page)) return *v == 1;
    const bool held = out_overlaps(page << 12, 4096);
    g_tl_pages.put(page, held ? 1 : 2);
    return held;
}

// The handle of a HandleExpr texture (nvn_shaders.h HandleExprOp) from what the draw binds now. Words outside a buffer read 0.
// ubo / ssbo: the stage's bindings (null: the replay's S); ssbo_ok false: a storage word makes the handle unknown (*unknown set, 0 returned).
uint32_t eval_handle_expr_in(const BindingDesc& d, const std::vector<HandleExprOp>& prog, const GState::Bind* ubo, const GState::Bind* ssbo, bool* unknown);
uint32_t eval_handle_expr(const BindingDesc& d, const std::vector<HandleExprOp>& prog) {
    return eval_handle_expr_in(d, prog, S.ubo[d.nvn_stage], S.ssbo[d.nvn_stage], nullptr);
}
uint32_t eval_handle_expr_in(const BindingDesc& d, const std::vector<HandleExprOp>& prog, const GState::Bind* ubo, const GState::Bind* ssbo, bool* unknown) {
    uint32_t st[64];
    int n = 0;
    for (const HandleExprOp& op : prog) {
        if (n >= 64) return 0;
        uint32_t v = 0;
        switch (op.kind) {
            case HandleExprOp::Const: v = op.a; break;
            case HandleExprOp::Cbuf:
                if (op.a == 1) {
                    const std::vector<uint8_t>& k = d.shader->constants;
                    if (static_cast<uint64_t>(op.b) * 4 + 4 <= k.size()) std::memcpy(&v, k.data() + static_cast<size_t>(op.b) * 4, 4);
                } else {
                    v = read_handle_word_in(ubo, static_cast<int>(op.a), static_cast<int>(op.b));
                }
                break;
            case HandleExprOp::Storage: {
                const uint64_t idx = st[--n];
                const int slot = storage_slot_for(d.nvn_stage, static_cast<int>(op.a));
                const GState::Bind& sb = ssbo[slot >= 0 && slot < 16 ? slot : 0];
                // the look-ahead (another thread): a word over a held compute result is not read (it would fault and fetch the result)
                if (unknown && slot >= 0 && slot < 16 && sb.addr && idx * 4 + 4 <= sb.size && tl_page_held(sb.addr + idx * 4)) { *unknown = true; return 0; }
                if (slot >= 0 && slot < 16 && sb.addr && idx * 4 + 4 <= sb.size) std::memcpy(&v, host_ptr(sb.addr) + idx * 4, 4);
                break;
            }
            case HandleExprOp::Inv: v = ~st[--n]; break;
            case HandleExprOp::Not: v = st[--n] == 0; break;
            case HandleExprOp::Sel: n -= 3; v = st[n] ? st[n + 1] : st[n + 2]; break;
            default: {
                const uint32_t y = st[--n], x = st[--n];
                const int32_t sx = static_cast<int32_t>(x), sy = static_cast<int32_t>(y);
                switch (op.kind) {
                    case HandleExprOp::Add: v = x + y; break;
                    case HandleExprOp::Sub: v = x - y; break;
                    case HandleExprOp::Mul: v = x * y; break;
                    case HandleExprOp::Shl: v = x << (y & 31); break;
                    case HandleExprOp::Shr: v = x >> (y & 31); break;
                    case HandleExprOp::Sar: v = static_cast<uint32_t>(sx >> (y & 31)); break;
                    case HandleExprOp::And: v = x & y; break;
                    case HandleExprOp::Or: v = x | y; break;
                    case HandleExprOp::Xor: v = x ^ y; break;
                    case HandleExprOp::Eq: v = x == y; break;
                    case HandleExprOp::Ne: v = x != y; break;
                    case HandleExprOp::Lt: v = sx < sy; break;
                    case HandleExprOp::Ltu: v = x < y; break;
                    case HandleExprOp::Le: v = sx <= sy; break;
                    case HandleExprOp::Leu: v = x <= y; break;
                    case HandleExprOp::Gt: v = sx > sy; break;
                    case HandleExprOp::Gtu: v = x > y; break;
                    case HandleExprOp::Ge: v = sx >= sy; break;
                    case HandleExprOp::Geu: v = x >= y; break;
                    case HandleExprOp::Min: v = static_cast<uint32_t>(std::min(sx, sy)); break;
                    case HandleExprOp::Minu: v = std::min(x, y); break;
                    case HandleExprOp::Max: v = static_cast<uint32_t>(std::max(sx, sy)); break;
                    case HandleExprOp::Maxu: v = std::max(x, y); break;
                    default: break;
                }
            }
        }
        st[n++] = v;
    }
    return n == 1 ? st[0] : 0;
}

// Textures resolve_texture finds: bound in c[2], a handle in a uniform block, a handle the draw computes.
bool texture_source(NvnSource s) { return s == NvnSource::SamplerBinding || s == NvnSource::HandleInUniform || s == NvnSource::HandleExpr; }

// [nvn] resolve_cache (default on): one command resolves each texture binding up to five times (samples_depth_target, prepare_textures,
// feedback_prepare, build_set, support_for_draw), each with the two pools' shared locks; the result is kept for the rest of the command
// (g_resolve_gen moves with every command run_list starts).
uint64_t g_resolve_gen = 1;
struct ResolveSlot { const BindingDesc* d = nullptr; uint64_t gen = 0; TexResolved r; };
ResolveSlot g_rslots[64];
bool resolve_cache_on() {
    static const bool on = nxrt::config_get_bool("nvn", "resolve_cache", true);
    return on;
}
TexResolved resolve_texture_uncached(const BindingDesc& d);
TexResolved resolve_texture(const BindingDesc& d) {
    if (!resolve_cache_on() || !ab_on(kAbResolveCache)) return resolve_texture_uncached(d);
    ResolveSlot& s = g_rslots[(reinterpret_cast<uintptr_t>(&d) >> 4) & 63];
    if (s.d == &d && s.gen == g_resolve_gen) return s.r;
    s.r = resolve_texture_uncached(d);
    s.d = &d;
    s.gen = g_resolve_gen;
    return s.r;
}

TexResolved resolve_texture_uncached(const BindingDesc& d) {
    TexResolved r;
    const DescriptorBinding& b = d.shader->refl.bindings[d.index];
    uint64_t handle = 0;
    if (b.nvn.source == NvnSource::SamplerBinding) {
        const int idx = b.nvn.index;
        if (idx >= 0 && idx < 64) handle = S.tex[d.nvn_stage][idx];
    } else if (b.nvn.source == NvnSource::HandleInUniform) {
        handle = read_handle_word(d.nvn_stage, b.nvn.bank, b.nvn.word);
    } else if (b.nvn.source == NvnSource::HandleExpr) {
        handle = eval_handle_expr(d, b.nvn.expr);
    }
    const uint32_t tid = handle_texture_id(handle), sid = handle_sampler_id(handle);
    if (S.tpool && static_cast<int>(tid) < S.tpool->count) {
        std::shared_lock<std::shared_mutex> lk(S.tpool->lock);
        const TexturePool::Entry& e = S.tpool->entries[tid];
        if (e.used && e.tex) {
            r.tex = e.tex.get();
            r.view = e.view;
            r.has_view = e.has_view;
        }
    }
    if (S.spool && static_cast<int>(sid) < S.spool->count) {
        std::shared_lock<std::shared_mutex> lk(S.spool->lock);
        if (S.spool->used[sid]) {
            r.smp = S.spool->entries[sid];
            r.has_sampler = true;
        }
    }
    return r;
}

ViewRequest make_request(const DescriptorBinding& b, const TexResolved& r) {
    ViewRequest q;
    q.type = view_type_for(b);
    q.depth_compare = b.image.depth;
    const Texture& t = *r.tex;
    int sw[4] = {t.b.swizzle[0], t.b.swizzle[1], t.b.swizzle[2], t.b.swizzle[3]};
    int dsmode = t.b.depth_stencil_mode;
    if (r.has_view) {
        const TextureViewInfo& v = r.view;
        if (v.has_levels) { q.base_level = static_cast<uint32_t>(v.min_level); q.level_count = static_cast<uint32_t>(v.num_levels); }
        if (v.has_layers) { q.base_layer = static_cast<uint32_t>(v.min_layer); q.layer_count = static_cast<uint32_t>(v.num_layers); }
        if (v.has_format) {
            const FormatInfo* fi = format_info(v.format);
            if (fi) q.format = fi->vk;
        }
        if (v.has_swizzle) for (int i = 0; i < 4; ++i) sw[i] = v.swizzle[i];
        if (v.has_depth_stencil_mode) dsmode = v.depth_stencil_mode;
    }
    q.swizzle.r = vk_swizzle(sw[0]);
    q.swizzle.g = vk_swizzle(sw[1]);
    q.swizzle.b = vk_swizzle(sw[2]);
    q.swizzle.a = vk_swizzle(sw[3]);
    q.depth_aspect = dsmode == NVN_TEXTURE_DEPTH_STENCIL_MODE_DEPTH;
    switch (b.image.component) {
        case ScalarType::Float: q.component = 1; break;
        case ScalarType::Sint: q.component = 2; break;
        case ScalarType::Uint: q.component = 3; break;
        default: break;
    }
    return q;
}

// The image a draw reads or writes through an image binding (nvnCommandBufferBindImage): the handle is a texture pool id.
TexResolved resolve_image(const BindingDesc& d) {
    TexResolved r;
    const DescriptorBinding& b = d.shader->refl.bindings[d.index];
    uint64_t handle = 0;
    if (b.nvn.source == NvnSource::ImageBinding) {
        const int idx = b.nvn.index;
        if (idx >= 0 && idx < 16) handle = S.img[d.nvn_stage][idx];
    } else if (b.nvn.source == NvnSource::HandleInUniform) {
        handle = read_handle_word(d.nvn_stage, b.nvn.bank, b.nvn.word);
    }
    const uint32_t tid = handle_texture_id(handle);
    if (S.tpool && static_cast<int>(tid) < S.tpool->count) {
        std::shared_lock<std::shared_mutex> lk(S.tpool->lock);
        const TexturePool::Entry& e = S.tpool->entries[tid];
        if (e.used && e.tex) {
            r.tex = e.tex.get();
            r.view = e.view;
            r.has_view = e.has_view;
        }
    }
    return r;
}

// The texture object behind a texture or image handle the game bound (texture pool id in the low bits), or null.
Texture* pool_texture(uint64_t handle) {
    const uint32_t tid = handle_texture_id(handle);
    if (!S.tpool || static_cast<int>(tid) >= S.tpool->count) return nullptr;
    std::shared_lock<std::shared_mutex> lk(S.tpool->lock);
    const TexturePool::Entry& e = S.tpool->entries[tid];
    return e.used ? e.tex.get() : nullptr;
}

// True when two texture objects are the same image: the object itself, or another object over the same storage (S4f: objects over one piece of
// memory share one image, so the one the pass renders to and the one a shader samples can be two objects).
bool shares_image(Texture* a, const Texture* b) {
    if (!a || !b) return false;
    if (a == b) return true;
    if (!tex_gpu(a)) return false;
    return a->gpu != nullptr && a->gpu == b->gpu;
}

// True when a texture the program samples is the depth attachment of the current target set: the draw then runs in a pass whose depth
// attachment is read-only (DEPTH_STENCIL_READ_ONLY_OPTIMAL serves as attachment and as sampled layout).
bool samples_depth_target(const ProgLayout& pl) {
    if (!S.depth_tex) return false;
    for (const BindingDesc& d : pl.bindings) {
        if (d.type != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) continue;
        const DescriptorBinding& b = d.shader->refl.bindings[d.index];
        if (!texture_source(b.nvn.source)) continue;
        if (shares_image(resolve_texture(d).tex, S.depth_tex.get())) return true;
    }
    return false;
}

// A colour attachment cannot be sampled in the pass that renders to it (no feedback loops): the binding gets the dummy view.
bool is_color_feedback(Texture* t) {
    if (!t) return false;
    for (uint32_t i = 0; i < S.ncolor; ++i) {
        Texture* a = S.color_tex[i].get();
        if (a && !a->gpu) tex_gpu_attachment(a);   // an attachment gets its image (or the one it shares) here, not later in ensure_pass: the check runs before the pass begins (S4g)
        if (shares_image(t, a)) return true;
    }
    return false;
}

// A draw that samples its own colour attachment (the bloom composite of some frames reads the tone-mapped picture while it writes it back, one
// pixel at a time): the console allows that, Vulkan has no such loop. The draw samples a copy of the image made just before it, which holds the
// same values a pointwise read sees on the hardware (and the right ones when the shader reads another mip level of the attachment).
struct FbClone {
    Ref<Texture> tex;
    uint64_t last = 0;                                     // serial of the last submission that used the copy
};
std::unordered_map<Texture*, FbClone> g_fb_clone;          // attachment -> its copy (same shape, no storage in game memory)
std::unordered_map<Texture*, Texture*> g_fb_use;           // attachment -> the copy the current draw samples
bool g_fb_seen = false;                                    // the last check found a feedback binding: check every draw until it goes away
std::atomic<int64_t> g_fb_clone_n{0};                      // g_fb_clone's size, mirrored for the gauge (the map itself is render thread only)

Texture* feedback_clone(Texture* t) {
    FbClone& fe = g_fb_clone[t];
    fe.last = F ? F->serial : 0;
    Ref<Texture>& c = fe.tex;
    g_fb_clone_n.store(static_cast<int64_t>(g_fb_clone.size()), std::memory_order_relaxed);
    if (c && (c->b.width != t->b.width || c->b.height != t->b.height || c->b.depth != t->b.depth || c->fmt != t->fmt || c->levels != t->levels || c->layers != t->layers ||
              (c->gpu && tex_scale(c.get()) != tex_scale(t))))
        c = Ref<Texture>();                                 // the game made another texture at this address
    if (!c) {
        Texture* n = new Texture();
        n->b = t->b;
        n->fmt = t->fmt;
        n->levels = t->levels;
        n->layers = t->layers;
        n->block_linear = t->block_linear;
        n->cpu_dirty.store(false);
        c = Ref<Texture>(n);
        static uint32_t next_id = 0x7f000000;
        n->id = next_id++;
    }
    return tex_gpu_scaled(c.get(), tex_scale(t)) ? c.get() : nullptr;
}

void feedback_prepare(const ProgLayout& pl) {
    g_fb_use.clear();
    bool any = false;
    for (const BindingDesc& d : pl.bindings) {
        if (d.type != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) continue;
        const DescriptorBinding& b = d.shader->refl.bindings[d.index];
        if (!texture_source(b.nvn.source)) continue;
        const TexResolved r = resolve_texture(d);
        if (!r.tex || !tex_gpu(r.tex) || !is_color_feedback(r.tex) || r.tex->b.samples > 1 || g_fb_use.count(r.tex)) continue;
        any = true;
        Texture* c = feedback_clone(r.tex);
        if (!c) continue;
        end_pass();
        tex_ensure_current(r.tex, *F);                                    // storage the game wrote since the image was made reaches the copy too
        tex_transition(r.tex, *F, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);   // the next pass moves it back to the attachment layout and loads it
        tex_transition(c, *F, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        std::vector<VkImageCopy> regions;
        for (uint32_t lv = 0; lv < tex_levels(r.tex); ++lv) {
            const VkExtent3D e = tex_image_extent(r.tex, lv);
            VkImageCopy rc = {};
            rc.srcSubresource = {tex_aspect(r.tex), lv, 0, tex_layers(r.tex)};
            rc.dstSubresource = {tex_aspect(c), lv, 0, tex_layers(c)};
            rc.extent = e;
            regions.push_back(rc);
        }
        vkCmdCopyImage(F->cb, tex_image(r.tex), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, tex_image(c), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, static_cast<uint32_t>(regions.size()), regions.data());
        tex_mark_gpu_written(c);
        tex_transition(c, *F, tex_sampled_layout(c));
        g_fb_use[r.tex] = c;
        counters().copies.fetch_add(1, std::memory_order_relaxed);
        gstamp("feedback copy");
    }
    g_fb_seen = any;
}

void flush_compute_out(int why = kOutGpuReader);       // compute shadows (below): results of direct compute bindings go back to the pool
void compute_out_guard(uint64_t addr, uint64_t size, int why = kOutGpuReader);
// [addr, +size) of game memory a texture upload reads: held compute results there go back first (compute_out_lazy; else all of them).
void out_texture_read(Texture* t) {
    if (!out_lazy()) { flush_compute_out(kOutTexture); return; }
    if (t && t->mem && t->cpu_dirty.load()) compute_out_guard(reinterpret_cast<uint64_t>(t->mem), t->storage_size ? t->storage_size : 4096, kOutTexture);
}

// A sampler3D reading a texture whose storage is 2D (a 2D texture the game left bound to the slot, or a 3D texture view of 2D storage): the
// console samples it as a volume one slice deep. Vulkan cannot view a 2D image as 3D, so the draw samples a 3D copy (w x h x 1, every level) of the
// texture's layer, made before the pass and refreshed once per submission (and whenever the CPU wrote the texture).
struct Vol3dCopy {
    Ref<Texture> src;                                      // keeps the key alive while the copy exists
    Ref<Texture> tex;
    uint32_t layer = 0;
    uint64_t copied = UINT64_MAX;                          // serial of the submission of the last copy
    uint64_t last = 0;                                     // serial of the last submission that used it
};
std::unordered_map<Texture*, Vol3dCopy> g_vol3d;           // 2D texture -> its 3D copy
std::unordered_map<Texture*, Texture*> g_vol3d_use;        // 2D texture -> the copy the current draw samples

bool wants_vol3d(const DescriptorBinding& b, const TexResolved& r) {
    if (!r.tex || view_type_for(b) != VK_IMAGE_VIEW_TYPE_3D) return false;
    const int target = r.tex->b.target;
    return target != NVN_TEXTURE_TARGET_3D && target != NVN_TEXTURE_TARGET_BUFFER && !target_is_multisample(target) && r.tex->b.samples <= 1 &&
           !tex_is_depth(r.tex);
}

void vol3d_prepare(const ProgLayout& pl) {
    g_vol3d_use.clear();
    for (const BindingDesc& d : pl.bindings) {
        if (d.type != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) continue;
        const DescriptorBinding& b = d.shader->refl.bindings[d.index];
        if (!texture_source(b.nvn.source)) continue;
        const TexResolved r = resolve_texture(d);
        if (!wants_vol3d(b, r) || g_vol3d_use.count(r.tex) || !tex_gpu(r.tex)) continue;
        const uint32_t layer = r.has_view && r.view.has_layers ? static_cast<uint32_t>(r.view.min_layer) : 0;
        if (layer >= tex_layers(r.tex)) continue;
        Vol3dCopy& e = g_vol3d[r.tex];
        if (e.src.get() != r.tex) e.src = Ref<Texture>::share(r.tex);
        e.last = F ? F->serial : 0;
        Ref<Texture>& c = e.tex;
        if (c && (c->b.width != r.tex->b.width || c->b.height != r.tex->b.height || c->fmt != r.tex->fmt || c->levels != r.tex->levels || e.layer != layer ||
                  (c->gpu && tex_scale(c.get()) != tex_scale(r.tex))))
            c = Ref<Texture>();
        if (!c) {
            Texture* n = new Texture();
            n->b = r.tex->b;
            n->b.target = NVN_TEXTURE_TARGET_3D;
            n->b.depth = 1;
            n->fmt = r.tex->fmt;
            n->levels = r.tex->levels;
            n->layers = 1;
            n->block_linear = r.tex->block_linear;
            n->cpu_dirty.store(false);
            static uint32_t next_id = 0x7e000000;
            n->id = next_id++;
            c = Ref<Texture>(n);
            e.layer = layer;
            e.copied = UINT64_MAX;
        }
        if (!tex_gpu_scaled(c.get(), tex_scale(r.tex))) continue;
        const uint64_t serial = F ? F->serial : 0;
        if (e.copied != serial || r.tex->cpu_dirty.load()) {
            out_texture_read(r.tex);
            end_pass();
            tex_ensure_current(r.tex, *F);
            tex_transition(r.tex, *F, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            tex_transition(c.get(), *F, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            std::vector<VkImageCopy> regions;
            for (uint32_t lv = 0; lv < tex_levels(r.tex); ++lv) {
                VkImageCopy rc = {};
                rc.srcSubresource = {tex_aspect(r.tex), lv, layer, 1};
                rc.dstSubresource = {tex_aspect(c.get()), lv, 0, 1};
                rc.extent = tex_image_extent(r.tex, lv);
                rc.extent.depth = 1;
                regions.push_back(rc);
            }
            vkCmdCopyImage(F->cb, tex_image(r.tex), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, tex_image(c.get()), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           static_cast<uint32_t>(regions.size()), regions.data());
            tex_mark_gpu_written(c.get());
            tex_transition(c.get(), *F, tex_sampled_layout(c.get()));
            tex_transition(r.tex, *F, tex_sampled_layout(r.tex));   // the same draw may sample it as 2D too
            e.copied = serial;
            counters().copies.fetch_add(1, std::memory_order_relaxed);
            gstamp("3D copy of a 2D texture");
            if (first_time_key("vol3d " + std::to_string(r.tex->id)))
                NXLOG_I(kTag, "texture %u (%s %dx%d, target %d) is sampled as 3D by %s binding %s: it gets a 3D copy one slice deep", r.tex->id,
                        r.tex->fmt ? r.tex->fmt->name : "?", r.tex->b.width, r.tex->b.height, r.tex->b.target, d.shader->hash.c_str(), b.name.c_str());
        }
        g_vol3d_use[r.tex] = c.get();
    }
}

// Makes the textures a draw samples current (uploaded, in their sampled layout) and the storage images it binds writable (GENERAL).
// Ends the pass when it has to touch images. The depth attachment itself is left to the pass.
// ---- pool arrays (A7) ---------------------------------------------------------------------------------------------------------------
// A shader whose texture handle is computed per pixel indexes a texture array and a sampler array as large as the NVN pools (texture id =
// handle & 0xfffff, sampler id = handle >> 20; nvn_pool_sets.cpp). Each (pool, array kind) gets one persistent descriptor set, partially
// bound and updated after bind: every entry starts as the stand-in of its kind (a depth comparison against it answers "lit", as the
// shaders did before the arrays existed), and the entries of textures the shader may read are written once per present (and again when
// the device created images since). Entered are the pool's textures whose image exists on the GPU and fit the array's kind; a depth
// comparison array takes depth formats only (shadow maps). Texture views and samplers of an entry rarely change, so a write seldom
// touches a descriptor that GPU work in flight reads (the layout allows updates of entries that pending work does not use).
// [nvn] pool_arrays = 0: the texture sets stay stand-ins (a depth comparison answers "lit"); the sampler sets are filled either way.
struct PoolSet {
    VkDescriptorPool dpool = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    std::vector<uint64_t> written;      // per entry: the view or sampler handle written (0 = the stand-in)
    std::vector<Ref<Texture>> tex;      // texture sets: the texture of each live entry (held: the game may free it before the next sync,
                                        // and the descriptor names its view until then)
    std::vector<uint32_t> live;         // texture sets: entries with a real view
    std::vector<uint64_t> esig;         // texture sets: per live entry, the signature its view was found for ([nvn] pool_sync_cache; 0 = none)
    // depth comparison sets ([nvn] pool_sync_depth_ids): the ids that can hold a view, i.e. whose texture has a depth format or that were
    // written (all others stay the stand-in they were made with); followed through the pool's registration log
    std::vector<uint32_t> cand;
    std::vector<uint8_t> in_cand;
    const void* cand_pool = nullptr;
    uint64_t cand_epoch = ~0ull;
    size_t cand_pos = 0;
    uint64_t synced = ~0ull;
    bool failed = false;
    // prepare_textures found every live texture current and in its sampled layout at this g_tex_state_gen (and this sync): nothing to check
    uint64_t clean_gen = ~0ull, clean_synced = ~0ull, clean_pass = ~0ull;   // clean_pass: the pass and attachments it was checked for
};
struct PoolSetKey {
    const void* owner;
    int view;
    bool depth, sampler;
    bool operator<(const PoolSetKey& o) const { return std::tie(owner, view, depth, sampler) < std::tie(o.owner, o.view, o.depth, o.sampler); }
};
std::map<PoolSetKey, PoolSet> g_pool_sets;

bool pool_arrays_on() {
    static const bool on = config_get_bool("nvn", "pool_arrays", true);
    return on;
}

PoolSet* pool_set_create(const PoolBinding& pb, PoolSet& st) {
    const VkDescriptorSetLayout layout = pool_set_layout(pb.sampler);
    if (!layout) { st.failed = true; return nullptr; }
    const uint32_t n = pb.sampler ? kPoolSamplers : kPoolTextures;
    VkDescriptorPoolSize ps = {pb.sampler ? VK_DESCRIPTOR_TYPE_SAMPLER : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, n};
    VkDescriptorPoolCreateInfo pci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    pci.maxSets = 1;
    pci.poolSizeCount = 1;
    pci.pPoolSizes = &ps;
    if (vkCreateDescriptorPool(vkdev(), &pci, nullptr, &st.dpool) != VK_SUCCESS) { st.failed = true; return nullptr; }
    VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = st.dpool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &layout;
    if (vkAllocateDescriptorSets(vkdev(), &ai, &st.set) != VK_SUCCESS) { st.failed = true; return nullptr; }
    // every entry the stand-in of its kind
    std::vector<VkDescriptorImageInfo> ii(n);
    for (VkDescriptorImageInfo& x : ii)
        x = pb.sampler ? VkDescriptorImageInfo{dummy_sampler(), VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED}
                       : VkDescriptorImageInfo{VK_NULL_HANDLE, dummy_view(pb.view, pb.depth), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = st.set;
    w.dstBinding = 0;
    w.descriptorCount = n;
    w.descriptorType = ps.type;
    w.pImageInfo = ii.data();
    vkUpdateDescriptorSets(vkdev(), 1, &w, 0, nullptr);
    st.written.assign(n, 0);
    st.tex.assign(pb.sampler ? 0 : n, Ref<Texture>());
    st.esig.assign(pb.sampler ? 0 : n, 0);
    if (pb.sampler) NXLOG_I(kTag, "pool array set: samplers (%u entries)", n);
    else NXLOG_I(kTag, "pool array set: textures, view type %d%s (%u entries)", static_cast<int>(pb.view), pb.depth ? ", depth compare" : "", n);
    return &st;
}

// [nvn] pool_sync_cache (default on): a texture entry whose texture object, image views (tex_view_stamp), view info and binding are what its
// written view was found for is kept without asking for the view again. The sync runs again whenever an image is created (several times
// per frame in Kokiri outside), and each run asked every live entry for its view: ~4% of the render thread.
bool pool_sync_cache_on() {
    static const bool on = nxrt::config_get_bool("nvn", "pool_sync_cache", true);
    return on && ab_on(kAbPoolSync);
}
// What decides an entry's view: the texture object, its image's view stamp and the request (shaders sharing the set ask with their own bindings)
uint64_t pool_entry_sig(const Texture* t, uint64_t stamp, const ViewRequest& q) {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](uint64_t v) { h = (h ^ v) * 1099511628211ull; };
    mix(reinterpret_cast<uint64_t>(t)); mix(stamp);
    mix(static_cast<uint64_t>(q.type) | (static_cast<uint64_t>(q.depth_compare) << 32) | (static_cast<uint64_t>(q.depth_aspect) << 33) |
        (static_cast<uint64_t>(q.component) << 40));
    mix(q.base_level | (static_cast<uint64_t>(q.level_count) << 32)); mix(q.base_layer | (static_cast<uint64_t>(q.layer_count) << 32));
    mix(static_cast<uint64_t>(q.format));
    mix(static_cast<uint64_t>(q.swizzle.r) | (static_cast<uint64_t>(q.swizzle.g) << 16) | (static_cast<uint64_t>(q.swizzle.b) << 32) | (static_cast<uint64_t>(q.swizzle.a) << 48));
    return h | 1;   // never 0
}
uint64_t g_psync_kept = 0, g_psync_asked = 0, g_psync_walks = 0, g_psync_walked = 0, g_psync_noimg = 0;   // statistics

// The set of this pool array, its entries brought up to date with the bound pool (once per present, or when images were created since).
struct PsMemo { const PoolBinding* pb = nullptr; const void* owner = nullptr; PoolSet* st = nullptr; };
PsMemo g_ps_memo[64];
PoolSet* pool_set_sync(const PoolBinding& pb) {
    const void* owner = pb.sampler ? static_cast<const void*>(S.spool.get()) : static_cast<const void*>(S.tpool.get());
    // the map entry of (binding, pool): remembered per binding (the key's other fields come from the binding; map nodes do not move)
    PsMemo& pm = g_ps_memo[(reinterpret_cast<uintptr_t>(&pb) >> 4) & 63];
    if (pm.pb != &pb || pm.owner != owner || !pm.st) {
        pm.pb = &pb;
        pm.owner = owner;
        pm.st = &g_pool_sets[PoolSetKey{owner, static_cast<int>(pb.view), pb.depth && !pb.sampler, pb.sampler}];
    }
    PoolSet& st = *pm.st;
    if (st.failed) return nullptr;
    if (!st.set && !pool_set_create(pb, st)) return nullptr;
    const uint64_t key = counters().presents.load(std::memory_order_relaxed) * 0x9e3779b97f4a7c15ull ^ counters().textures_created.load(std::memory_order_relaxed);
    // [nvn] pool_arrays = 0 keeps the textures stand-ins; the samplers are real either way, so a depth comparison against a stand-in (depth
    // 1.0) answers "lit" with the game's comparison function, as the shaders did before the arrays existed
    if (st.synced == key || !owner || (!pb.sampler && !pool_arrays_on())) return &st;
    st.synced = key;
    std::vector<VkDescriptorImageInfo> ii;
    std::vector<uint32_t> at;
    auto put = [&](uint32_t i, const VkDescriptorImageInfo& x, uint64_t h) {
        ii.push_back(x);
        at.push_back(i);
        st.written[i] = h;
    };
    if (pb.sampler) {
        std::shared_lock<std::shared_mutex> lk(S.spool->lock);
        const uint32_t n = std::min<uint32_t>(kPoolSamplers, static_cast<uint32_t>(std::max(0, S.spool->count)));
        for (uint32_t i = 0; i < n; ++i) {
            const VkSampler smp = (i < S.spool->used.size() && S.spool->used[i]) ? sampler_for(S.spool->entries[i]) : VK_NULL_HANDLE;
            const uint64_t h = reinterpret_cast<uint64_t>(smp);
            if (h == st.written[i]) continue;
            put(i, {smp ? smp : dummy_sampler(), VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED}, h);
        }
    } else {
        const DescriptorBinding& b = pb.shader->refl.bindings[pb.index];
        std::shared_lock<std::shared_mutex> lk(S.tpool->lock);
        const uint32_t count = static_cast<uint32_t>(std::max(0, S.tpool->count));
        if (count > kPoolTextures) log_once("pool-array-short", "a texture pool has more entries than the pool arrays (65536): later ids read the last entry");
        const uint32_t n = std::min<uint32_t>(kPoolTextures, count);
        st.live.clear();
        const bool cache = pool_sync_cache_on();
        // with the cache, only the ids ever registered (an unused entry has no view and stays the stand-in it was written as): OoT's pool
        // has tens of thousands of entries and the walk over all of them was a quarter of the sync
        // depth sets: only the ids that can hold a view (OoT: a few hundred of ~32k registered ids; the walk over all of them was ~5% of the
        // render thread in Kokiri outside, the sync runs whenever an image is made)
        static const bool depth_ids = nxrt::config_get_bool("nvn", "pool_sync_depth_ids", true);
        const bool by_cand = cache && pb.depth && depth_ids && ab_on(kAbPoolDepthIds);
        if (by_cand) {
            TexturePool& tp = *S.tpool;
            auto add = [&](uint32_t id) {
                if (id >= n || st.in_cand[id]) return;
                const TexturePool::Entry& e = tp.entries[id];
                const Texture* x = (e.used && e.tex) ? e.tex.get() : nullptr;
                if (st.written[id] || (x && x->fmt && x->fmt->aspect != kAspectColor)) { st.in_cand[id] = 1; st.cand.push_back(id); }
            };
            if (st.cand_pool != &tp || st.cand_epoch != tp.reg_log_epoch || st.in_cand.size() != st.written.size()) {
                st.cand.clear();
                st.in_cand.assign(st.written.size(), 0);
                st.cand_pool = &tp;
                st.cand_epoch = tp.reg_log_epoch;
                for (uint32_t id : tp.used_ids) add(id);
                for (uint32_t id = 0; id < n; ++id) if (st.written[id]) add(id);
                st.cand_pos = tp.reg_log.size();
            } else {
                for (size_t k = st.cand_pos; k < tp.reg_log.size(); ++k) add(tp.reg_log[k]);
                st.cand_pos = tp.reg_log.size();
            }
        } else {
            st.cand_pool = nullptr;   // walked in full: the list is made again when it is used next
        }
        const uint32_t nwalk = by_cand ? static_cast<uint32_t>(st.cand.size()) : cache ? static_cast<uint32_t>(S.tpool->used_ids.size()) : n;
        ++g_psync_walks;
        g_psync_walked += nwalk;
        static std::vector<uint32_t> prune;   // positions in st.cand of entries that left the list
        prune.clear();
        for (uint32_t w = 0; w < nwalk; ++w) {
            const uint32_t i = by_cand ? st.cand[w] : cache ? S.tpool->used_ids[w] : w;
            if (i >= n) continue;
            const TexturePool::Entry& e = S.tpool->entries[i];
            Texture* t = (e.used && e.tex) ? e.tex.get() : nullptr;
            if (cache && t && st.written[i] && st.tex[i].get() == t && st.esig[i]) {
                if (const uint64_t stamp = tex_view_stamp(t)) {
                    TexResolved r;
                    r.tex = t;
                    r.view = e.view;
                    r.has_view = e.has_view;
                    if (pool_entry_sig(t, stamp, make_request(b, r)) == st.esig[i]) { st.live.push_back(i); ++g_psync_kept; continue; }
                }
            }
            st.esig[i] = 0;
            const bool depth_entry = t && t->fmt && t->fmt->aspect != kAspectColor;
            if (t && pb.depth && (!t->fmt || t->fmt->aspect == kAspectColor)) t = nullptr;   // a depth comparison reads depth textures only
            if (t) ++g_psync_asked;
            if (t && !t->gpu) ++g_psync_noimg;
            if (t && !t->gpu) {   // no image of its own yet: one only when another object over the same storage has one (tex_gpu shares it)
                const Ref<Texture> alias = texture_registry_find_alias(t);
                if (!alias || !alias->gpu) t = nullptr;
            }
            VkImageView v = VK_NULL_HANDLE;
            ViewRequest q;
            if (t) {
                TexResolved r;
                r.tex = t;
                r.view = e.view;
                r.has_view = e.has_view;
                q = make_request(b, r);
                v = tex_sampled_view(t, q);
            }
            st.tex[i] = v ? e.tex : Ref<Texture>();
            if (v) {
                st.live.push_back(i);
                if (cache && t == e.tex.get())
                    if (const uint64_t stamp = tex_view_stamp(t)) st.esig[i] = pool_entry_sig(t, stamp, q);
            }
            const uint64_t h = reinterpret_cast<uint64_t>(v);
            if (by_cand && !depth_entry && !v) prune.push_back(w);   // no depth texture here any more: written as the stand-in, off the list
            if (h == st.written[i]) continue;
            put(i, v ? VkDescriptorImageInfo{VK_NULL_HANDLE, v, tex_sampled_layout(t)}
                     : VkDescriptorImageInfo{VK_NULL_HANDLE, dummy_view(pb.view, pb.depth), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}, h);
        }
        if (!prune.empty()) {   // positions ascending: compact the list in one pass
            size_t k = 0, pi = 0;
            for (size_t w2 = 0; w2 < st.cand.size(); ++w2) {
                if (pi < prune.size() && prune[pi] == w2) { st.in_cand[st.cand[w2]] = 0; ++pi; continue; }
                st.cand[k++] = st.cand[w2];
            }
            st.cand.resize(k);
        }
        for (uint32_t i = n; i < st.tex.size(); ++i) {                                     // a pool that shrank: stand-ins
            st.tex[i] = Ref<Texture>();
            st.esig[i] = 0;
            if (st.written[i]) put(i, {VK_NULL_HANDLE, dummy_view(pb.view, pb.depth), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}, 0);
        }
    }
    if (!ii.empty()) {
        std::vector<VkWriteDescriptorSet> ws(ii.size(), VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET});
        for (size_t k = 0; k < ii.size(); ++k) {
            ws[k].dstSet = st.set;
            ws[k].dstBinding = 0;
            ws[k].dstArrayElement = at[k];
            ws[k].descriptorCount = 1;
            ws[k].descriptorType = pb.sampler ? VK_DESCRIPTOR_TYPE_SAMPLER : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            ws[k].pImageInfo = &ii[k];
        }
        vkUpdateDescriptorSets(vkdev(), static_cast<uint32_t>(ws.size()), ws.data(), 0, nullptr);
    }
    if (g_trace) {
        std::string ids;
        for (size_t k = 0; k < st.live.size() && k < 48; ++k) {
            char t[64];
            Texture* x = st.tex[st.live[k]].get();
            std::snprintf(t, sizeof t, " %u:t%u:%s:%ux%ux%u", st.live[k], x ? x->id : 0, x && x->fmt ? x->fmt->name : "?", x ? x->b.width : 0, x ? x->b.height : 0, x ? x->b.depth : 0);
            ids += t;
        }
        TRACE("    pool array %s: %zu entries written, %zu live%s", pb.sampler ? "samplers" : "textures", ii.size(), st.live.size(), ids.c_str());
        // [nvn] trace_pool_handles = "1a60298d,..." (handles as the shaders read them): what each resolves to in the pool arrays
        static const std::string want = config_get_str("nvn", "trace_pool_handles", "");
        for (size_t at0 = 0; at0 < want.size();) {
            size_t e = want.find(',', at0);
            if (e == std::string::npos) e = want.size();
            const uint32_t h = static_cast<uint32_t>(std::strtoul(want.substr(at0, e - at0).c_str(), nullptr, 16));
            at0 = e + 1;
            const uint32_t tid = handle_texture_id(h), sid = handle_sampler_id(h);
            if (pb.sampler) {
                if (S.spool && sid < S.spool->used.size() && S.spool->used[sid]) {
                    const SamplerInfo& si = S.spool->entries[sid];
                    TRACE("    pool handle %08x: sampler %u compare mode %d func %d filter %d/%d wrap %d,%d", h, sid, si.compare_mode, si.compare_func, si.min_filter, si.mag_filter, si.wrap_s, si.wrap_t);
                } else {
                    TRACE("    pool handle %08x: sampler %u not in the pool", h, sid);
                }
            } else {
                Texture* x = tid < st.tex.size() ? st.tex[tid].get() : nullptr;
                char d[128];
                TRACE("    pool handle %08x: texture id %u %s", h, tid, x ? tex_desc(x, d, sizeof d) : "(stand-in)");
            }
        }
    }
    return &st;
}

void pool_sets_reset() {
    for (PsMemo& m : g_ps_memo) m = PsMemo{};
    for (auto& [k, st] : g_pool_sets)
        if (st.dpool) vkDestroyDescriptorPool(vkdev(), st.dpool, nullptr);   // frees the set
    g_pool_sets.clear();
}

uint64_t g_pt_breaks = 0, g_pt_from[7] = {}, g_pt_dirty = 0, g_pt_storage = 0;   // statistics: prepare_textures ending an open pass
void prepare_textures(const ProgLayout& pl) {
    vol3d_prepare(pl);
    std::vector<Texture*> need, need_storage;
    for (const BindingDesc& d : pl.bindings) {
        if (d.type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) {
            TexResolved r = resolve_image(d);
            if (!r.tex || !tex_gpu(r.tex) || !tex_has_storage(r.tex)) continue;
            if (r.tex->cpu_dirty.load() || tex_layout(r.tex) != VK_IMAGE_LAYOUT_GENERAL || tex_scale(r.tex) != 1.0f) need_storage.push_back(r.tex);
            continue;
        }
        if (d.type != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) continue;
        const DescriptorBinding& b = d.shader->refl.bindings[d.index];
        if (!texture_source(b.nvn.source)) continue;
        TexResolved r = resolve_texture(d);
        if (!r.tex) continue;
        if (!tex_gpu(r.tex)) continue;
        // owned by the pass; a compute dispatch runs outside every pass, so the graphics attachments do not own what it samples
        if (shares_image(r.tex, S.depth_tex.get()) || (d.nvn_stage != kCompute && is_color_feedback(r.tex))) continue;
        if (r.tex->cpu_dirty.load() || tex_layout(r.tex) != tex_sampled_layout(r.tex)) {
            need.push_back(r.tex);
            if (r.tex->cpu_dirty.load() && ft::on()) ft::mark(b.nvn.source == NvnSource::SamplerBinding ? "tx.dirty_bind" : "tx.dirty_uniform", r.tex->id);
        }
    }
    for (const PoolBinding& pb : pl.pool) {   // A7: what a pool array may read is in the sampled layout too (the pass's own targets excepted)
        if (pb.sampler || !pool_arrays_on()) continue;
        PoolSet* st = pool_set_sync(pb);
        if (!st) continue;
        const uint64_t gen = g_tex_state_gen.load(std::memory_order_acquire);
        const bool cs = pb.shader && pb.shader->stage == ShaderStage::Compute;
        // the entries left out below depend on the render targets: the check holds for these targets (for compute, this depth target)
        uint64_t pass_key = (cs ? 0x8000000000000001ull : 0x1234567ull) ^ reinterpret_cast<uint64_t>(S.depth_tex.get());
        if (!cs)
            for (uint32_t c = 0; c < S.ncolor; ++c) pass_key = (pass_key ^ reinterpret_cast<uint64_t>(S.color_tex[c].get())) * 0x100000001b3ull;
        if (st->clean_gen == gen && st->clean_synced == st->synced && st->clean_pass == pass_key) continue;   // nothing changed since all were ready
        bool clean = true;
        for (uint32_t i : st->live) {
            Texture* t = st->tex[i].get();
            if (!t || !tex_gpu(t)) { clean = false; continue; }
            if (!t->cpu_dirty.load() && tex_layout(t) == tex_sampled_layout(t)) continue;
            if (shares_image(t, S.depth_tex.get()) || (!cs && is_color_feedback(t))) continue;
            clean = false;
            if (t->cpu_dirty.load() || tex_layout(t) != tex_sampled_layout(t))
                if (std::find(need.begin(), need.end(), t) == need.end()) {
                    need.push_back(t);
                    char d[96];
                    TRACE("    pool array: entry %u %s to the sampled layout", i, tex_desc(t, d, sizeof d));
                }
        }
        if (clean) {
            st->clean_gen = gen;
            st->clean_synced = st->synced;
            st->clean_pass = pass_key;
        }
    }
    if (need.empty() && need_storage.empty()) return;
    for (Texture* t : need) out_texture_read(t);           // an upload reads game memory: compute results held there go back first
    for (Texture* t : need_storage) out_texture_read(t);
    if (P.open) {   // statistics (the 10 s line): what ended an open pass here
        ++g_pt_breaks;
        g_pt_storage += need_storage.size();
        for (Texture* t : need) {
            if (t->cpu_dirty.load()) { ++g_pt_dirty; continue; }
            if (attachment_layout(tex_layout(t)) && t->gpu && g_break_prone.size() < 4096) g_break_prone.insert(t->gpu);   // pass_pre_layout
            switch (tex_layout(t)) {
                case VK_IMAGE_LAYOUT_GENERAL: ++g_pt_from[0]; break;
                case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL: ++g_pt_from[1]; break;
                case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL: ++g_pt_from[2]; break;
                case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL: ++g_pt_from[3]; break;
                case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL: case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL: ++g_pt_from[4]; break;
                case VK_IMAGE_LAYOUT_UNDEFINED: ++g_pt_from[5]; break;
                default: ++g_pt_from[6]; break;
            }
        }
    }
    end_pass(true);    // layout changes and uploads follow: each carries the dependency for its own image
    counters().pass_breaks.fetch_add(1, std::memory_order_relaxed);
    for (Texture* t : need) {
        tex_ensure_current(t, *F);
        tex_transition(t, *F, tex_sampled_layout(t));
    }
    for (Texture* t : need_storage) {
        if (tex_scale(t) != 1.0f) tex_rescale(t, *F, 1.0f, "bound as a storage image");   // imageStore coordinates are not scaled (Ryujinx: ResScaleUnsupported)
        tex_ensure_current(t, *F);
        tex_transition(t, *F, VK_IMAGE_LAYOUT_GENERAL);
        tex_mark_gpu_written(t);
    }
    gstamp("uploads/layouts");
}

// SPIR-V ImageFormat of a storage image -> VkFormat (UNDEFINED: Unknown or unmapped).
VkFormat image_format_vk(uint32_t f) {
    switch (f) {
        case 1: return VK_FORMAT_R32G32B32A32_SFLOAT;
        case 2: return VK_FORMAT_R16G16B16A16_SFLOAT;
        case 3: return VK_FORMAT_R32_SFLOAT;
        case 4: return VK_FORMAT_R8G8B8A8_UNORM;
        case 5: return VK_FORMAT_R8G8B8A8_SNORM;
        case 6: return VK_FORMAT_R32G32_SFLOAT;
        case 7: return VK_FORMAT_R16G16_SFLOAT;
        case 8: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
        case 9: return VK_FORMAT_R16_SFLOAT;
        case 10: return VK_FORMAT_R16G16B16A16_UNORM;
        case 11: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
        case 12: return VK_FORMAT_R16G16_UNORM;
        case 13: return VK_FORMAT_R8G8_UNORM;
        case 14: return VK_FORMAT_R16_UNORM;
        case 15: return VK_FORMAT_R8_UNORM;
        case 16: return VK_FORMAT_R16G16B16A16_SNORM;
        case 17: return VK_FORMAT_R16G16_SNORM;
        case 18: return VK_FORMAT_R8G8_SNORM;
        case 19: return VK_FORMAT_R16_SNORM;
        case 20: return VK_FORMAT_R8_SNORM;
        case 21: return VK_FORMAT_R32G32B32A32_SINT;
        case 22: return VK_FORMAT_R16G16B16A16_SINT;
        case 23: return VK_FORMAT_R8G8B8A8_SINT;
        case 24: return VK_FORMAT_R32_SINT;
        case 25: return VK_FORMAT_R32G32_SINT;
        case 26: return VK_FORMAT_R16G16_SINT;
        case 27: return VK_FORMAT_R8G8_SINT;
        case 28: return VK_FORMAT_R16_SINT;
        case 29: return VK_FORMAT_R8_SINT;
        case 30: return VK_FORMAT_R32G32B32A32_UINT;
        case 31: return VK_FORMAT_R16G16B16A16_UINT;
        case 32: return VK_FORMAT_R8G8B8A8_UINT;
        case 33: return VK_FORMAT_R32_UINT;
        case 34: return VK_FORMAT_A2B10G10R10_UINT_PACK32;
        case 35: return VK_FORMAT_R32G32_UINT;
        case 36: return VK_FORMAT_R16G16_UINT;
        case 37: return VK_FORMAT_R8G8_UINT;
        case 38: return VK_FORMAT_R16_UINT;
        case 39: return VK_FORMAT_R8_UINT;
        default: return VK_FORMAT_UNDEFINED;
    }
}

int storage_c0_base(int nvn_stage) {   // c[0] byte offset of a graphics stage's storage descriptor slot 0 (see storage_slot_for)
    switch (nvn_stage) {
        case kVertex: return 0x110;
        case kTessCtl: return 0x210;
        case kTessEval: return 0x310;
        case kGeometry: return 0x410;
        case kFragment: return 0x510;
        case kTask: return 0x650;
        case kMesh: return 0x770;
        default: return 0x310;
    }
}

int storage_slot_for(int nvn_stage, int c0_offset) {
    // c[0] storage buffer descriptors (byte offsets, as the translator reports them): 0x110 + 0x100 * hardware stage + 0x10 * binding
    // with hardware stage 0 vertex, 3 geometry, 4 fragment; compute: 0x310 + 0x10 * binding. Checked against the translated shaders of the
    // game (vertex 0x110, geometry 0x410, fragment 0x510, compute 0x310 and 0x320).
    int base = 0;
    switch (nvn_stage) {
        case kVertex: base = 0x110; break;
        case kTessCtl: base = 0x210; break;     // tessellation: Ryujinx's hardware stage order (1, 2), not checked on a game shader
        case kTessEval: base = 0x310; break;
        case kGeometry: base = 0x410; break;
        case kFragment: base = 0x510; break;
        case kCompute: base = 0x310; break;
        // task 0x650 (four slots past the 0x110 + 0x100 * i pattern), mesh 0x770 (six past). Measured in OoT's first scene (A15): the task
        // shader dereferences a pointer from c[0] 0x670, and the game's task slot 2 is the only bound buffer of pointers. The same mesh
        // program family reads its GPU-written record buffer (count at word 3) at 0x770 where the game binds it at mesh slot 0 (depth
        // prepass, mesh a24d8406) and at 0x790 where it binds it at slot 2 (colour pass, b8b873e5); every other slot fits the same way.
        case kTask: base = 0x650; break;
        case kMesh: base = 0x770; break;
        default: return 0;
    }
    if (c0_offset < base) return 0;
    return (c0_offset - base) / 0x10 ^ (nvn_stage != kCompute ? graphics_ssbo_slot_xor() : 0);
}

// ---- compute shadows: storage and texel buffers of a dispatch in device-local memory ---------------------------------------------
// Pools are the game's own memory imported into Vulkan, so a shader reaches storage buffers and texel buffers over PCIe. Compute shaders
// that work on such buffers (the exposure histogram's atomics, the 4 MiB simulation buffers) took 9 ms each on an RTX 3080. [nvn]
// compute_shadows (default on): each storage or texel buffer range a dispatch binds (up to 16 MiB) is bound from device-local memory.
//  * Direct ([nvn] compute_direct, default on): the range is in a pool with a device-local copy (nvn_gpu_pool.cpp, the pool mirror). The
//    dispatch binds the copy itself. Pages the copy does not hold (the CPU wrote them since the GPU last used them) are copied in from the
//    pool first (mirror_fill), so only the CPU's writes cross PCIe. A range the shader may write is written in the copy; it goes back to the
//    pool once, when something other than a compute binding of the same range could look at it: before any command other than a dispatch,
//    a state change or a barrier, before a dispatch reads it another way (uniform block, indirect arguments, another range on its pages,
//    a texture upload), and at the end of the submission, before the serial the CPU waits for can signal. A run of dispatches over the
//    same buffers thus copies each result out once instead of once per dispatch.
//  * Shadow (otherwise: no copy for the pool, banned pages, a range over two chunks): the range is copied into a buffer of its own
//    before the dispatch and copied back after it when the shader can write it (and into the pool copy, when there is one).
// Game memory holds every result by the end of the submission, in queue order, as before. Ranges that overlap another range of the same
// dispatch stay in the pool.
struct Shadow { gfx::Buffer buf; VkDeviceSize size = 0; uint64_t last = 0; };
std::unordered_map<uint64_t, Shadow> g_shadows;            // by GPU address and size
struct ShadowUse { uint64_t addr; VkDeviceSize size; BufRef host; VkBuffer buf; VkDeviceSize off; bool writes; bool direct; MirrorSpan mirror; bool own = false; };   // own: in a shadow buffer
std::vector<ShadowUse> g_shadow_uses;                       // this dispatch
// Results of direct bindings still only in the pool copy: nvn_gpu_pool.cpp (out_add ... out_end_submit, "compute out").
bool g_shadowing = false;
bool g_sh_barrier = false, g_sh_copied = false;             // this dispatch: the barrier before the copies in is recorded / some copy in is
uint32_t g_sh_kinds = 0;   // this dispatch's copies in, for the gpu_passes label: 1 pages filled into the pool copy, 2 delta snapshot, 4 shadow from the pool, 8 shadow from the copy, 16 indirect arguments into c[0]
uint64_t g_sh_uses = 0, g_sh_from_mirror = 0, g_sh_bytes_mirror = 0, g_sh_bytes_pool = 0, g_sh_bytes_out = 0, g_sh_no_span = 0;   // summary line
uint64_t g_sh_held = 0;   // bound from a shadow that holds an earlier dispatch's result
uint64_t g_sh_direct = 0, g_sh_direct_filled = 0, g_sh_bytes_fill = 0, g_sh_bytes_dout = 0, g_sh_merged = 0, g_sh_flushes = 0;
constexpr VkDeviceSize kShadowMax = 16ull << 20;

bool compute_shadows_on() {
    static const bool on = nxrt::config_get_bool("nvn", "compute_shadows", true);
    return on;
}
// [nvn] compute_shadow_hold (default on): a shadow's result stays in the shadow (out_add) instead of being copied back after its dispatch.
bool shadow_hold_on() {
    static const bool on = nxrt::config_get_bool("nvn", "compute_shadow_hold", true);
    return on;
}
// [nvn] ssbo_readonly (default on): storage buffers a shader's SPIR-V never writes (DescriptorBinding::written) are not copied back after
// a dispatch and do not invalidate the pool copy.
bool ssbo_readonly_on() {
    static const bool on = nxrt::config_get_bool("nvn", "ssbo_readonly", true);
    return on;
}
// [nvn] gmem_ro_bindings (default on): a pointer shader (A17 "gmem") that does not store through pointers ("gmemw" absent) writes only the
// storage bindings its SPIR-V stores into (DescriptorBinding::written), like any other shader. Before, every storage binding of a pointer
// shader counted as written: each such draw marked its bound buffers written (pool-copy pages stale, the resolve epoch moved, every set
// resolved anew) and its sets were never reused. Stores through the page table are handled by gmem_before_op (global_writes).
bool gmem_ro_on();
inline bool storage_binding_writes(const DescriptorBinding& b, const ShaderInfo* s) {
    if (b.written || !ssbo_readonly_on()) return true;
    if (!s->refl.global_fallback) return false;
    return s->refl.global_writes || !gmem_ro_on();
}
bool gmem_ro_on() {
    static const bool on = nxrt::config_get_bool("nvn", "gmem_ro_bindings", true);
    return on && ab_on(kAbGmemRo);
}
bool cs_gmem_held_on() {
    static const bool on = nxrt::config_get_bool("nvn", "cs_gmem_held", true);
    return on && ab_on(kAbCsGmemHeld);
}
// [nvn] refill_compute (cpuperf_ab_what name refill_compute): see mirror_fill's refill_written (nvn_gpu_pool.cpp).
bool refill_compute_on() {
    static const bool on = nxrt::config_get_bool("nvn", "refill_compute", true);
    return on && ab_on(kAbRefillCompute);
}
bool occ_defer_on() {
    static const bool on = nxrt::config_get_bool("nvn", "occ_defer", true);
    return on && ab_on(kAbOccDefer);
}
bool cs_gmem_flush_on() {
    static const bool on = nxrt::config_get_bool("nvn", "cs_gmem_flush", false);
    return on && ab_on(kAbCsGmemFlush);
}
bool compute_direct_on() {
    static const bool on = nxrt::config_get_bool("nvn", "compute_direct", true);
    return on;
}

// Every result still only in the pool copy goes back to the pool (in queue order, before what is recorded next).
void flush_compute_out(int why) {
    if (!out_any()) return;
    end_pass();
    if (out_flush(F->cb, 0, UINT64_MAX, why)) {
        ++g_sh_flushes;
        gstamp("compute out");
    }
}

// [addr, addr + size) is about to be read or written some other way than a direct binding of exactly a held range: the results sharing a
// page with it go back first (a page granular test: the pool copy is filled and tracked by whole pages).
void compute_out_guard(uint64_t addr, uint64_t size, int why) {
    if (!size || !out_overlaps(addr, size)) return;
    end_pass();
    if (out_flush(F->cb, addr, size, why)) {
        ++g_sh_flushes;
        if (gpass_on()) gstamp(("compute out " + std::to_string(why)).c_str());
    }
}

// Ops after which the pending results may stay in the pool copy: state that touches no memory, barriers (the copies carry their own),
// dispatches (they check their own ranges) and calls of other lists (their ops are checked one by one). Anything else may read the pool.
bool compute_out_may_wait(Op op) {
    if (op == Op::Dispatch || op == Op::DispatchIndirect || op == Op::Barrier || op == Op::CallCommands || op == Op::PushDebugGroup || op == Op::PopDebugGroup) return true;
    return op < Op::DrawArrays && op != Op::SetRenderEnable && op != Op::SetRenderTargets;
}

// A buffer a draw reads: results it shares a page with go back first, unless the draw binds the pool copy for it (vertex, index and aligned
// uniform buffers whose pages are all current there, resolve_read).
// [nvn] draw_fill (default on): when the copy holds results in such a range but not all of it (pages the CPU rewrote, untracked pages), the
// missing pages are copied into the copy for this submission (mirror_fill) and the draw binds the copy, instead of every held result there
// going back to the pool: the results are most of the range, the missing pages a few.
bool draw_fill_on() {
    static const bool on = nxrt::config_get_bool("nvn", "draw_fill", true);
    return on;
}
uint64_t g_draw_fills = 0, g_draw_fill_bytes = 0;
// why the copy could not serve a draw's read over held results (the 10 s line): copy not allowed, no copy span (no copy for the pool, two
// chunks, over 64 MiB), the fill failed
uint64_t g_dr_fail[3] = {}, g_dr_fail_bytes[3] = {};
bool out_draw_read(uint64_t addr, uint64_t size, bool copy_ok, int why) {
    if (!addr) return false;
    if (!size) size = 65536;
    if (!(ab_on(kAbOverlapCache) ? out_overlaps_window(addr, size) : out_overlaps(addr, size))) return false;   // the same bindings are checked draw after draw: no-overlap answers are cached per index version
    if (copy_ok && out_servable(addr, size)) return true;
    const bool span = copy_ok && out_lazy() && draw_fill_on() && mirror_span(addr, size).buf;
    const int fail = !copy_ok ? 0 : (!span ? 1 : 2);
    if (span) {
        end_pass();
        memory_barrier(F->cb);                     // earlier writes to the pool there before the copies read it
        uint64_t bytes = 0;
        const bool ok = mirror_fill(F->cb, addr, size, &bytes);
        if (bytes) memory_barrier(F->cb);          // the copies before the draw reads them
        if (ok && out_servable(addr, size)) {
            ++g_draw_fills;
            g_draw_fill_bytes += bytes;
            return true;
        }
    }
    ++g_dr_fail[fail];
    g_dr_fail_bytes[fail] += size;
    compute_out_guard(addr, size, why);
    return true;
}

// The buffer for [addr, +size) that a command reads through a plain buffer binding (storage buffers, indirect arguments, copy sources): the
// pool copy when it holds a compute result there and is current for the whole range, else the pool, after the results there went back
// (flush: false inside a pass, where out_before_op has done that already).
BufRef held_or_pool(uint64_t addr, uint64_t size, int why, bool flush) {
    if (size && out_overlaps(addr, size)) {
        if (out_lazy() && out_servable(addr, size)) {
            const MirrorSpan ms = mirror_span(addr, size);
            BufRef r = resolve_buffer(addr, size);
            if (ms.buf && r.ok) {
                r.buffer = ms.buf;
                r.offset = ms.offset;
                r.avail = ms.avail;
                return r;
            }
        }
        if (flush) {
            static int logged = 0;
            if (why == kOutIndirect && logged < 16) {
                ++logged;
                const MirrorSpan ms = mirror_span(addr, size);
                NXLOG_I(kTag, "indirect arguments over a held result not servable from the copy: %llx +%llx (cs %s): lazy %d servable %d span %d valid %d",
                        (unsigned long long)addr, (unsigned long long)size, S.stage[kCompute] ? S.stage[kCompute]->hash.substr(0, 8).c_str() : "-",
                        out_lazy() ? 1 : 0, out_servable(addr, size) ? 1 : 0, ms.buf ? 1 : 0, ms.valid ? 1 : 0);
            }
            compute_out_guard(addr, size, why);
        }
    }
    return resolve_buffer(addr, size);
}

// ---- global memory (A17): coherence of shaders that reach the pools through pointers -------------------------------------------
// Shaders with the harness's "gmem" token resolve some of their memory accesses at run time through the guest page table, whose device
// addresses point at the imported pools themselves, not at the device-local pool copies (Mirror) or compute shadows the replay binds
// for ordinary storage buffers. What such a shader reaches is not known on the CPU; its pointers come from its uniform blocks (OoT's GPU
// culling 4c1e8564: c[4][i * 16 + 0xe0] and +0x80/+0xa0), so every 64-bit word of the stage's bound uniform blocks that points into an
// imported pool stands for kGmemWindow bytes from its page on:
//  (a) before such a shader runs (dispatch or draw of any stage), compute results held only in a pool copy there go back to the pool (it
//      may read them, and a later copy-back would overwrite what it writes there);
//  (b) after a shader that stores or does atomics that way ("gmemw"), those ranges count as written (gpu_wrote: pool copy pages, the
//      GPU-write ring, the capture's list).
// A write further from such a pointer, or through a pointer read from memory, is missed (gap "gmem ranges assumed from uniform pointers").
// Flushing every held result before each pointer shader instead cost 900 MB/s of copy-backs (7 fps in the first scene, 2026-11-07).
// [nvn] gmem_coherence = false turns both off.
constexpr uint64_t kGmemWindow = 64 * 1024;
constexpr uint64_t kGmemUboScanWrite = 4096;   // bytes of each uniform block searched for pointers: writers
constexpr uint64_t kGmemUboScanRead = 1024;    // readers (thousands of vertex shaders per frame)
constexpr int kGmemMaxPointers = 128;          // per op
uint64_t g_gmem_flushes = 0, g_gmem_windows = 0;
uint64_t g_csmask_n = 0, g_csmask_bits = 0;   // statistics: compute dispatches with a held-slot mask (cs_gmem_held), slots marked in them
uint64_t g_csmask_why[4] = {};                 // ... why slots were marked: no pool, virtual pool, no storage, held result
uint64_t g_gmask_stages = 0, g_gmask_marked = 0;   // graphics_c0: pointer stages, of them with a slot marked (no inline fast path)

bool gmem_coherence_on() {
    static const bool on = nxrt::config_get_bool("nvn", "gmem_coherence", true);
    return on;
}

bool host_readable(uint64_t addr, uint64_t n);   // below

void gmem_before_op(bool compute) {
    for (int st = 0; st < kStages; ++st)   // a pointer shader can reach any pool: every chunk imported, so the page table maps it
        if ((st == kCompute) == compute && S.stage[st] && S.stage[st]->refl.global_fallback) { import_pools_for_pointers(); break; }
    if (!gmem_coherence_on()) return;
    bool any = false, writes = false;
    for (int st = 0; st < kStages; ++st) {
        if ((st == kCompute) != compute) continue;
        const ShaderInfo* s = S.stage[st];
        if (!s || !s->refl.global_fallback) continue;
        any = true;
        writes |= s->refl.global_writes;
    }
    const bool held = out_any();
    if (!any || (!writes && !held)) return;
    uint64_t seen[kGmemMaxPointers];
    int n = 0;
    bool full = false;
    // every page a reader asked about in this op, matched or not (open addressing; a full table only stops remembering)
    uint64_t asked[512];
    std::memset(asked, 0, sizeof asked);
    auto asked_before = [&](uint64_t page) {
        for (uint32_t h = static_cast<uint32_t>((page >> 12) * 0x9E3779B1u) >> 23, k = 0; k < 16; ++k, h = (h + 1) & 511) {
            if (asked[h] == page) return true;
            if (!asked[h]) { asked[h] = page; return false; }
        }
        return false;
    };
    for (int st = 0; st < kStages && !full; ++st) {
        if ((st == kCompute) != compute) continue;
        const ShaderInfo* s = S.stage[st];
        if (!s || !s->refl.global_fallback) continue;
        const bool w_st = s->refl.global_writes;
        if (w_st) {
            // The stage's own storage descriptors (the c[0] table a pointer shader reads its buffer addresses from): a store or atomic
            // through them is a write of the bound range like any storage binding's. OoT's culling root writes the mesh draws' indirect
            // records so (797045e000 in the Deku Tree capture); without this the capture's write list missed them and every replay loop
            // added its counts to the previous loop's (an ever denser leaf crown).
            for (int k = 0; k < 16; ++k) {
                const GState::Bind& b = S.ssbo[st][k];
                if (!b.addr || !b.size) continue;
                const PoolHit hit = pool_lookup(b.addr);
                if (!hit.pool || hit.pool->is_virtual || !hit.pool->base) continue;
                if (out_overlaps(b.addr, b.size)) { compute_out_guard(b.addr, b.size, kOutGlobal); ++g_gmem_flushes; }
                g_gw_site = 1; gpu_wrote(b.addr, b.size, kWriteStorage); g_gw_site = 0;
            }
        }
        for (int k = 0; k < 16 && !full; ++k) {
            const GState::Bind& b = S.ubo[st][k];
            if (!b.addr) continue;
            const uint64_t len = std::min<uint64_t>(b.size ? b.size : 65536, w_st ? kGmemUboScanWrite : kGmemUboScanRead) & ~uint64_t{7};
            if (!len || !host_readable(b.addr, len)) continue;
            const uint8_t* w = static_cast<const uint8_t*>(host_ptr(b.addr));
            for (uint64_t o = 0; o < len; o += 8) {
                uint64_t v;
                std::memcpy(&v, w + o, 8);
                if (v < 0x10000 || (v >> 47)) continue;
                const uint64_t page = v & ~uint64_t{4095};
                bool dup = false;
                for (int i = 0; i < n && !dup; ++i) dup = seen[i] == page;
                if (dup) continue;
                if (!w_st) {   // a reader: only results held there matter
                    if (asked_before(page)) continue;
                    if (out_overlaps_window(page, kGmemWindow)) {
                        if (n == kGmemMaxPointers) { full = true; break; }
                        seen[n++] = page;
                        compute_out_guard(page, kGmemWindow, kOutGlobal);
                        ++g_gmem_flushes;
                    }
                    continue;
                }
                const PoolHit hit = pool_lookup(v);
                if (!hit.pool || hit.pool->is_virtual || !hit.pool->base) continue;
                if (n == kGmemMaxPointers) { full = true; break; }
                seen[n++] = page;
                const uint64_t end = std::min<uint64_t>(page + kGmemWindow, reinterpret_cast<uint64_t>(hit.pool->base) + hit.pool->size);
                if (end > page) {
                    if (out_overlaps(page, end - page)) { compute_out_guard(page, end - page, kOutGlobal); ++g_gmem_flushes; }
                    g_gw_site = 2; gpu_wrote(page, end - page, kWriteStorage); g_gw_site = 0;
                    ++g_gmem_windows;
                }
            }
        }
    }
    if (writes) {
        const ShaderInfo* s0 = S.stage[compute ? kCompute : kFragment];
        static std::unordered_map<std::string, nxrt::GapCounter*> per_shader;   // render thread; counters never freed (registered)
        static const std::string unknown = "?";
        const std::string& h = s0 ? s0->hash : unknown;
        auto it = per_shader.find(h);
        if (it == per_shader.end()) it = per_shader.emplace(h, new nxrt::GapCounter("gmem", "global memory writes: ranges assumed from uniform pointers", h)).first;
        it->second->hit();
    }
}

// The reads of a draw's bound buffers (out_before_op). About 80 bindings per draw, each a look-up among the held results under a lock: when
// no buffer binding and no program changed (g_buf_ver) and nothing moved the resolve epoch or sent results back since the last check (the
// same conditions as [nvn] set_reuse), the check would find what it found then, so it is skipped ([nvn] set_reuse = off checks every draw).
uint64_t g_odb_ver = ~0ull, g_odb_epoch = 0, g_odb_skipped = 0;
// [nvn] odb_incremental (default on): a binding whose last check found no held result over it cannot find one while the held results have
// not grown (out_grow_version: other changes only take results out). So between two growths only the bindings that changed since the last
// check (g_odb_dirty: BindBuffers, a new program or stage) and the ones that did overlap then (g_odb_hot) are checked again. A full check
// at every growth and at the start of each submission.
enum : int { kOdbVtx = 0, kOdbUbo = 1, kOdbSsbo = 2, kOdbXfb = 3 };
uint16_t g_odb_dirty[4][kStages] = {};       // [kind][stage] bit per slot (vertex and xfb: stage 0)
bool g_odb_all_dirty = true;
uint64_t g_odb_grow = ~0ull;
struct OdbHot { const GState::Bind* b; bool copy_ok; int why; };
std::vector<OdbHot> g_odb_hot;
uint64_t g_odb_full = 0, g_odb_inc = 0, g_odb_inc_checked = 0, g_odb_verify_bad = 0;   // statistics
bool odb_incremental_on() {
    static const bool on = [] { const std::string v = nxrt::config_get_str("nvn", "odb_incremental", "on"); return v != "off" && v != "0" && v != "false"; }();
    return on && ab_on(kAbOdbInc);
}
void odb_mark(int kind, int stage, uint32_t slot) { if (stage < kStages && slot < 16) g_odb_dirty[kind][stage] |= static_cast<uint16_t>(1u << slot); }
void out_draw_bindings() {
    const bool memo = set_reuse_mode() != SetReuse::Off && ab_on(kAbOdb);
    if (memo && g_odb_ver == g_buf_ver && g_odb_epoch == (g_desc_epoch * 0x100000001b3ull + out_bytes_total()) * 31 + out_version()) { ++g_odb_skipped; return; }
    const uint64_t grow = out_grow_version();
    if (odb_incremental_on() && !g_odb_all_dirty && grow == g_odb_grow) {
        ++g_odb_inc;
        const VkDeviceSize ualign = device().properties().limits.minUniformBufferOffsetAlignment;
        static std::vector<OdbHot> hot;
        hot.clear();
        auto check = [&](const GState::Bind& b, bool copy_ok, int why) {
            ++g_odb_inc_checked;
            if (out_draw_read(b.addr, b.size, copy_ok, why)) hot.push_back({&b, copy_ok, why});
        };
        // the bindings that overlapped at the last check (and are not checked below as changed)
        for (const OdbHot& h : g_odb_hot) {
            const GState::Bind* b = h.b;
            int kind, st; uint32_t slot;
            if (b >= S.vtx && b < S.vtx + 16) { kind = kOdbVtx; st = 0; slot = static_cast<uint32_t>(b - S.vtx); }
            else if (b >= &S.xfb[0] && b < &S.xfb[0] + 4) { kind = kOdbXfb; st = 0; slot = static_cast<uint32_t>(b - &S.xfb[0]); }
            else if (b >= &S.ubo[0][0] && b < &S.ubo[0][0] + kStages * 16) { kind = kOdbUbo; st = static_cast<int>((b - &S.ubo[0][0]) / 16); slot = static_cast<uint32_t>((b - &S.ubo[0][0]) % 16); }
            else { kind = kOdbSsbo; st = static_cast<int>((b - &S.ssbo[0][0]) / 16); slot = static_cast<uint32_t>((b - &S.ssbo[0][0]) % 16); }
            if (g_odb_dirty[kind][st] & (1u << slot)) continue;
            if (kind != kOdbVtx && kind != kOdbXfb && (st == kCompute || !S.stage[st])) continue;
            check(*b, h.copy_ok, h.why);
        }
        for (uint32_t m = g_odb_dirty[kOdbVtx][0]; m; m &= m - 1) check(S.vtx[std::countr_zero(m)], true, kOutDrawBuffer);
        for (int st = 0; st < kStages; ++st) {
            if (st == kCompute || !S.stage[st]) continue;
            for (uint32_t m = g_odb_dirty[kOdbUbo][st]; m; m &= m - 1) {
                const GState::Bind& b = S.ubo[st][std::countr_zero(m)];
                check(b, ualign && (b.addr % ualign) == 0, kOutDrawBuffer);
            }
            for (uint32_t m = g_odb_dirty[kOdbSsbo][st]; m; m &= m - 1) check(S.ssbo[st][std::countr_zero(m)], true, kOutStorageXfb);
        }
        for (uint32_t m = g_odb_dirty[kOdbXfb][0]; m; m &= m - 1) check(S.xfb[std::countr_zero(m)], false, kOutStorageXfb);
        static const bool verify = nxrt::config_get_str("nvn", "odb_incremental", "on") == "verify";
        if (verify) {   // every binding not checked now must have no held result over it (a pure query; logged when it fails)
            auto skipped = [&](const GState::Bind& b) {
                if (!b.addr) return;
                for (const OdbHot& h : hot) if (h.b == &b) return;
                if (out_overlaps(b.addr, b.size ? b.size : 65536)) {
                    ++g_odb_verify_bad;
                    if (g_odb_verify_bad < 20) NXLOG_W(kTag, "odb_incremental verify: binding %llx+%llx has a held result but was not checked", (unsigned long long)b.addr, (unsigned long long)b.size);
                }
            };
            for (const GState::Bind& b : S.vtx) skipped(b);
            for (int st = 0; st < kStages; ++st) {
                if (st == kCompute || !S.stage[st]) continue;
                for (const GState::Bind& b : S.ubo[st]) skipped(b);
                for (const GState::Bind& b : S.ssbo[st]) skipped(b);
            }
            for (const GState::Bind& b : S.xfb) skipped(b);
        }
        g_odb_hot.swap(hot);
        std::memset(g_odb_dirty, 0, sizeof g_odb_dirty);
        g_odb_grow = out_grow_version();   // the checks may have sent results back (no growth), never added any
        g_odb_ver = g_buf_ver;
        g_odb_epoch = (g_desc_epoch * 0x100000001b3ull + out_bytes_total()) * 31 + out_version();
        return;
    }
    ++g_odb_full;
    g_odb_hot.clear();
    const VkDeviceSize ualign0 = device().properties().limits.minUniformBufferOffsetAlignment;
    // out_draw_read is true for a binding over a held result (and for those only): checked again at every draw until the next full check
    for (const GState::Bind& b : S.vtx) if (out_draw_read(b.addr, b.size, true, kOutDrawBuffer)) g_odb_hot.push_back({&b, true, kOutDrawBuffer});
    for (int st = 0; st < kStages; ++st) {
        if (st == kCompute || !S.stage[st]) continue;
        for (const GState::Bind& b : S.ubo[st]) {
            const bool ok = ualign0 && (b.addr % ualign0) == 0;
            if (out_draw_read(b.addr, b.size, ok, kOutDrawBuffer)) g_odb_hot.push_back({&b, ok, kOutDrawBuffer});
        }
        for (const GState::Bind& b : S.ssbo[st]) if (out_draw_read(b.addr, b.size, true, kOutStorageXfb)) g_odb_hot.push_back({&b, true, kOutStorageXfb});
    }
    for (const GState::Bind& b : S.xfb) if (out_draw_read(b.addr, b.size, false, kOutStorageXfb)) g_odb_hot.push_back({&b, false, kOutStorageXfb});
    std::memset(g_odb_dirty, 0, sizeof g_odb_dirty);
    g_odb_all_dirty = false;
    g_odb_grow = grow;
    g_odb_ver = g_buf_ver;
    g_odb_epoch = (g_desc_epoch * 0x100000001b3ull + out_bytes_total()) * 31 + out_version();   // after the checks: a result they sent back moved out_bytes_total already
}

// ---- look-ahead before a pass ([nvn] pass_lookahead) -----------------------------------------------------------------------------
// A draw that reads buffers over held compute results gets them copied into the pool copy (draw_fill) or back to the pool first
// (out_draw_read), and both end the open render pass (~10 pass breaks per frame in the first scene). Before a pass begins, the commands
// that follow in the current list (and the lists it calls) are scanned up to the first one that ends the pass anyway (anything other than
// state binds, draws, counters and uniform updates: render target changes, dispatches, copies, clears, barriers), with the buffer bindings
// those commands make tracked on the side, and the reads of their draws are settled now, outside the pass. The draws then find their
// ranges servable or no longer held and keep the pass open. Each distinct range is checked once per scan. Off by default: paired A/B in the first
// scene and on a Kokiri walk showed no gain (33.8 vs 34.7 fps, 28.7 vs 29.4): the bytes copied stay the same, only the pass breaks move.
const RecordReader* g_rd_cur[kMaxDepth + 2] = {};   // the reader of the command being run, per list depth (run_list)
int g_rd_depth = -1;
uint64_t g_la_scans = 0, g_la_settled = 0;            // statistics

bool pass_lookahead_on() {
    static const bool on = nxrt::config_get_bool("nvn", "pass_lookahead", false);   // off: no fps gain measured (2026-11-08), costs scan time
    return on;
}

struct LookSim {
    GState::Bind vtx[16], ubo[kStages][16], ssbo[kStages][16], xfb[4];
    bool present[kStages];
};

void la_read(std::unordered_set<uint64_t>& seen, uint64_t addr, uint64_t size, bool copy_ok, int why) {
    if (!addr) return;
    if (!size) size = 65536;
    const uint64_t key = (addr * 0x9e3779b97f4a7c15ull) ^ size ^ (copy_ok ? 1ull << 63 : 0);
    if (!seen.insert(key).second) return;
    if (!out_overlaps(addr, size) || (copy_ok && out_servable(addr, size))) return;
    out_draw_read(addr, size, copy_ok, why);
    ++g_la_settled;
}

bool op_keeps_sets(Op op);   // below
// False: a command that ends the pass was reached (stop scanning).
bool la_scan(RecordReader rd, int depth, LookSim& sim, std::unordered_set<uint64_t>& seen, int& budget) {
    const VkDeviceSize ualign = device().properties().limits.minUniformBufferOffsetAlignment;
    while (rd.next()) {
        if (--budget < 0) return false;
        const Op op = rd.op();
        switch (op) {
            case Op::BindProgram: {
                const auto& p = rd.payload<P_BindProgram>();
                const uint32_t bits = p.stages | ((p.stages & ((1u << kCompute) - 1)) ? ((1u << kTask) | (1u << kMesh)) : 0u);
                for (int st = 0; st < kStages; ++st)
                    if (bits & (1u << st)) sim.present[st] = p.program && p.program->stage[st];
                break;
            }
            case Op::BindBuffers: {
                const auto& p = rd.payload<P_BindBuffers>();
                const NVNbufferRange* rg = reinterpret_cast<const NVNbufferRange*>(rd.blob<P_BindBuffers>());
                for (uint32_t i = 0; i < p.count; ++i) {
                    const uint32_t k = p.first + i;
                    if (p.kind == kBufVertex && k < 16) sim.vtx[k] = {rg[i].address, rg[i].size};
                    else if (p.kind == kBufUniform && p.stage < kStages && k < 16) sim.ubo[p.stage][k] = {physical_buffer_address(rg[i].address, rg[i].size), rg[i].size};
                    else if (p.kind == kBufStorage && p.stage < kStages && k < 16) sim.ssbo[p.stage][k] = {physical_buffer_address(rg[i].address, rg[i].size), rg[i].size};
                    else if (p.kind == kBufXfb && k < 4) sim.xfb[k] = {rg[i].address, rg[i].size};
                }
                break;
            }
            case Op::DrawArrays: case Op::DrawElements: case Op::DrawIndirect: case Op::DrawMeshTasks: {
                for (const GState::Bind& b : sim.vtx) la_read(seen, b.addr, b.size, true, kOutDrawBuffer);
                for (int st = 0; st < kStages; ++st) {
                    if (st == kCompute || !sim.present[st]) continue;
                    for (const GState::Bind& b : sim.ubo[st]) la_read(seen, b.addr, b.size, ualign && (b.addr % ualign) == 0, kOutDrawBuffer);
                    for (const GState::Bind& b : sim.ssbo[st]) la_read(seen, b.addr, b.size, true, kOutStorageXfb);
                }
                for (const GState::Bind& b : sim.xfb) la_read(seen, b.addr, b.size, false, kOutStorageXfb);
                if (op == Op::DrawElements) {
                    const auto& p = rd.payload<P_DrawElements>();
                    const uint32_t isz = index_size_bytes(static_cast<int>(p.type));
                    la_read(seen, p.index_address, static_cast<uint64_t>(p.count) * (isz ? isz : 4), true, kOutDrawBuffer);
                } else if (op == Op::DrawIndirect) {
                    const auto& p = rd.payload<P_DrawIndirect>();
                    const bool indexed = p.type != 0xffffffffu;
                    const uint64_t cmd = indexed ? 20 : 16;
                    const uint64_t stride = p.stride ? static_cast<uint64_t>(p.stride) : cmd;
                    const uint64_t n = p.multi && p.count_address ? std::max<uint64_t>(1, p.max_count) : 1;
                    la_read(seen, p.indirect_address, stride * (n - 1) + cmd, true, kOutIndirect);
                    if (p.multi && p.count_address) la_read(seen, p.count_address, 4, true, kOutIndirect);
                } else if (op == Op::DrawMeshTasks) {
                    const auto& p = rd.payload<P_DrawMesh>();
                    if (p.kind >= 2) {
                        const uint64_t rec = p.kind == 3 ? 12 : 8, stride = p.stride ? static_cast<uint64_t>(p.stride) : rec;
                        const uint64_t n = p.multi ? std::max<uint64_t>(1, p.max_count) : 1;
                        la_read(seen, p.indirect, stride * (n - 1) + rec, true, kOutIndirect);
                        if (p.multi && p.count_address) la_read(seen, p.count_address, 4, true, kOutIndirect);
                    }
                }
                break;
            }
            case Op::CallCommands: {
                if (depth >= kMaxDepth) return false;
                const auto& p = rd.payload<P_CallCommands>();
                const uint64_t* h = reinterpret_cast<const uint64_t*>(rd.blob<P_CallCommands>());
                for (uint32_t i = 0; i < p.count; ++i) {
                    Ref<CmdList> l = handle_lookup(h[i]);
                    if (l && !la_scan(RecordReader(*l), depth + 1, sim, seen, budget)) return false;
                }
                break;
            }
            default:
                if (!op_keeps_sets(op)) return false;
                break;
        }
    }
    return true;
}

// ensure_pass, before the pass begins (no pass open).
void pass_lookahead() {
    if (!pass_lookahead_on() || g_rd_depth < 0 || !g_rd_cur[g_rd_depth] || !out_lazy() || !out_any() || !ab_on(kAbLookahead)) return;
    static LookSim sim;
    std::memcpy(sim.vtx, S.vtx, sizeof sim.vtx);
    std::memcpy(sim.ubo, S.ubo, sizeof sim.ubo);
    std::memcpy(sim.ssbo, S.ssbo, sizeof sim.ssbo);
    std::memcpy(sim.xfb, S.xfb, sizeof sim.xfb);
    for (int st = 0; st < kStages; ++st) sim.present[st] = S.stage[st] != nullptr;
    static std::unordered_set<uint64_t> seen;
    seen.clear();
    int budget = 4096;
    ++g_la_scans;
    la_scan(*g_rd_cur[g_rd_depth], g_rd_depth, sim, seen, budget);
}
const uint8_t* cpu_view(uint64_t addr, uint64_t size);   // below

// The bytes from index_address an indexed indirect draw reads: max over its draws of (first index + count) * index size, from the
// arguments (and draw count) when the CPU can read them (not a held result, not written by a submission that has not completed).
// Otherwise the GPU writes them too and nobody knows the count here: everything from index_address to the end of its pool.
uint64_t indirect_index_span(const P_DrawIndirect& p) {
    const uint32_t isz0 = index_size_bytes(static_cast<int>(p.type));
    const uint64_t isz = isz0 ? isz0 : 4;
    const uint64_t stride = p.stride ? static_cast<uint64_t>(p.stride) : 20;
    const auto whole = [&] { const BufRef b = resolve_buffer(p.index_address, 1); return b.ok && b.avail ? static_cast<uint64_t>(b.avail) : uint64_t{1} << 20; };
    uint64_t n = 1;
    if (p.multi && p.count_address) {
        const uint8_t* c = out_overlaps(p.count_address, 4) ? nullptr : cpu_view(p.count_address, 4);
        if (!c) return whole();
        uint32_t v = 0;
        std::memcpy(&v, c, 4);
        n = std::min<uint64_t>(v, p.max_count);
    }
    if (!n) return 0;
    const uint64_t args = stride * (n - 1) + 20;
    const uint8_t* a = out_overlaps(p.indirect_address, args) ? nullptr : cpu_view(p.indirect_address, args);
    if (!a) return whole();
    uint64_t end = 0;
    for (uint64_t i = 0; i < n; ++i) {
        uint32_t v[3];
        std::memcpy(v, a + i * stride, sizeof v);   // count, instance count, first index
        if (v[0] && v[1]) end = std::max(end, (static_cast<uint64_t>(v[2]) + v[0]) * isz);
    }
    return end;
}

// [nvn] compute_out_lazy: before op, the results it may read or overwrite through the pool go back (instead of all of them before any
// command that is not a dispatch). Ops that only touch images, state and syncs leave them in the copy; a fence's serial is the
// submission's, and out_end_submit decides at its end.
void out_before_op(const RecordReader& r) {
    if (!out_any()) return;
    switch (r.op()) {
        case Op::Dispatch: case Op::DispatchIndirect: case Op::Barrier: case Op::CallCommands: case Op::PushDebugGroup: case Op::PopDebugGroup:
        case Op::ClearColor: case Op::ClearDepthStencil: case Op::ClearTexture: case Op::CopyTextureToTexture: case Op::Downsample:
        case Op::DrawTexture: case Op::DiscardColor: case Op::DiscardDepthStencil: case Op::ResolveDepth: case Op::FenceSync: case Op::WaitSync:
        case Op::ResetCounter: case Op::WaitEvent: case Op::SetRenderEnable: case Op::Small:
            return;
        case Op::CopyBufferToTexture: case Op::CopyTextureToBuffer:    // do_copy_buffer_texture: its own range (it knows the size)
            return;
        case Op::DrawArrays:
        case Op::DrawElements: {
            out_draw_bindings();
            if (r.op() == Op::DrawElements) {
                const auto& p = r.payload<P_DrawElements>();
                const uint32_t isz = index_size_bytes(static_cast<int>(p.type));
                out_draw_read(p.index_address, static_cast<uint64_t>(p.count) * (isz ? isz : 4), true, kOutDrawBuffer);
            }
            return;
        }
        case Op::DrawIndirect: {
            out_draw_bindings();
            const auto& p = r.payload<P_DrawIndirect>();
            const bool indexed = p.type != 0xffffffffu;
            const uint64_t cmd = indexed ? 20 : 16;
            const uint64_t stride = p.stride ? static_cast<uint64_t>(p.stride) : cmd;
            const uint64_t n = p.multi && p.count_address ? std::max<uint64_t>(1, p.max_count) : 1;
            out_draw_read(p.indirect_address, stride * (n - 1) + cmd, true, kOutIndirect);   // do_draw_indirect binds the copy for them
            if (p.multi && p.count_address) out_draw_read(p.count_address, 4, true, kOutIndirect);
            if (indexed) compute_out_guard(p.index_address, indirect_index_span(p), kOutIndirect);   // the draw binds the pool for its indices
            return;
        }
        case Op::ClearBuffer: {
            const auto& p = r.payload<P_ClearBuffer>();
            compute_out_guard(p.address, p.size, kOutBufferWrite);
            return;
        }
        case Op::CopyBufferToBuffer: {
            const auto& p = r.payload<P_CopyBufferToBuffer>();
            out_draw_read(p.src, p.size, true, kOutBufferRead);       // the copy reads the pool copy (held_or_pool)
            compute_out_guard(p.dst, p.size, kOutBufferWrite);
            return;
        }
        case Op::UpdateUniformBuffer: {
            const auto& p = r.payload<P_UpdateUniformBuffer>();
            compute_out_guard(p.address + static_cast<uint64_t>(p.offset), p.size, kOutBufferWrite);
            return;
        }
        case Op::ReportCounter: compute_out_guard(r.payload<P_Counter>().address, 32, kOutBufferWrite); return;
        case Op::ReportValue: compute_out_guard(r.payload<P_ReportValue>().address, 32, kOutBufferWrite); return;
        case Op::SignalEvent: {
            const auto& p = r.payload<P_Event>();
            if (p.event && p.event->pool && p.event->pool->base)
                compute_out_guard(p.event->pool->gpu_base + static_cast<uint64_t>(p.event->offset), 4, kOutEvent);
            return;
        }
        case Op::DrawXfb: flush_compute_out(kOutIndirect); return;
        case Op::DrawMeshTasks: {
            out_draw_bindings();
            const auto& p = r.payload<P_DrawMesh>();
            if (p.kind >= 2) {
                const uint64_t rec = p.kind == 3 ? 12 : 8, stride = p.stride ? static_cast<uint64_t>(p.stride) : rec;
                const uint64_t n = p.multi ? std::max<uint64_t>(1, p.max_count) : 1;
                out_draw_read(p.indirect, stride * (n - 1) + rec, true, kOutIndirect);
                if (p.multi && p.count_address) out_draw_read(p.count_address, 4, true, kOutIndirect);
            }
            return;
        }
        default:
            if (r.op() < Op::DrawArrays) return;    // state
            flush_compute_out(kOutOther);           // feedback begin/end and the rest: everything first
            return;
    }
}

void sh_barrier_once() {
    if (g_sh_barrier) return;
    memory_barrier(F->cb);       // earlier writes to these ranges (any command) before the copies read them
    g_sh_barrier = true;
}

// The buffer a dispatch binds for [addr, addr + size): the pool copy, the shadow (its copy-in recorded now), or `host` when it cannot have
// either.
BufRef shadow_of(uint64_t addr, VkDeviceSize size, const BufRef& host, bool writes) {
    if (!g_shadowing || !host.ok || size == 0) return host;
    for (ShadowUse& u : g_shadow_uses) {
        if (u.addr == addr && u.size == size) {           // the same range bound twice: one binding
            u.writes |= writes;
            BufRef r = host; r.buffer = u.buf; r.offset = u.off; r.avail = size;
            return r;
        }
        if (addr < u.addr + u.size && u.addr < addr + size) {   // overlapping ranges stay coherent in the pool
            compute_out_guard(addr, size, kOutDispatch);
            return host;
        }
    }
    // a result an earlier dispatch left in the pool copy: this dispatch binds it there again (the pages are not valid for the tracker
    // until the result goes back, so mirror_span would not say so)
    {
        VkBuffer ob = VK_NULL_HANDLE;
        VkDeviceSize oo = 0;
        bool in_mirror = true;
        if (out_exact(addr, size, &ob, &oo, &in_mirror)) {
            ++g_sh_uses;
            ++(in_mirror ? g_sh_direct : g_sh_held);
            if (writes && in_mirror && out_presnap(addr, size, ob, oo, F->cb)) { g_sh_copied = true; g_sh_kinds |= 2; }   // a result sent back already: snapshot again
            g_shadow_uses.push_back({addr, size, host, ob, oo, writes, true, MirrorSpan{ob, oo, true}, !in_mirror});
            BufRef r = host; r.buffer = ob; r.offset = oo; r.avail = size;
            return r;
        }
    }
    if (!out_lazy()) compute_out_guard(addr, size, kOutDispatch);
    if (size > kShadowMax) { compute_out_guard(addr, size, kOutDispatch); return host; }
    if (out_overlaps_shadow(addr, size)) compute_out_guard(addr, size, kOutDispatch);   // the pool copy and the pool lack a shadow's held result
    const MirrorSpan ms = mirror_span(addr, size);    // pages holding a compute result count as current there (compute_out_lazy)
    if (ms.buf && compute_direct_on()) {
        bool ok = ms.valid;
        if (!ok) {
            sh_barrier_once();
            uint64_t filled = 0;
            ok = mirror_fill(F->cb, addr, size, &filled, refill_compute_on());
            if (filled) { g_sh_copied = true; g_sh_bytes_fill += filled; g_sh_kinds |= 1; }
            if (ok) ++g_sh_direct_filled;
        }
        if (ok) {
            ++g_sh_uses;
            ++g_sh_direct;
            if (writes && out_presnap(addr, size, ms.buf, ms.offset, F->cb)) { g_sh_copied = true; g_sh_kinds |= 2; }   // out_delta: the range before the dispatch
            g_shadow_uses.push_back({addr, size, host, ms.buf, ms.offset, writes, true, ms});
            BufRef r = host; r.buffer = ms.buf; r.offset = ms.offset; r.avail = size;
            return r;
        }
    }
    compute_out_guard(addr, size, kOutDispatch);      // the shadow reads the pool: held results there go back first
    const uint64_t key = addr ^ (static_cast<uint64_t>(size) << 40) ^ (static_cast<uint64_t>(size) >> 24);
    Shadow& sh = g_shadows[key];
    if (!sh.buf || sh.size != size) {
        if (sh.buf) { gfx::Buffer old = sh.buf; forget_buffer_views(old.buffer); defer_destroy([old]() mutable { device().destroy_buffer(old); }); sh.buf = gfx::Buffer{}; }
        const VkBufferUsageFlags usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT |
                                         VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (device().create_buffer(size, usage, gfx::MemoryUse::GpuOnly, &sh.buf, "nvn compute shadow") != VK_SUCCESS) {
            sh.buf = gfx::Buffer{};
            g_shadows.erase(key);
            return host;
        }
        sh.size = size;
    }
    sh.last = F->serial;
    sh_barrier_once();
    // The device-local pool copy, when it holds the range, is the cheap source (video memory to video memory instead of over PCIe); after a
    // dispatch that writes the range, the result goes to both, so the next dispatch finds the copy valid again (mirror_span_written).
    ++g_sh_uses;
    if (!ms.buf) {
        ++g_sh_no_span;
        static int logged = 0;
        if (logged < 24 && first_time_key("shadow-nospan " + std::to_string(addr) + "/" + std::to_string(size))) {
            ++logged;
            const PoolHit hit = pool_lookup(addr);
            NXLOG_I(kTag, "compute shadow without a pool copy span: %llx +%llx (cs %s, writes %d): pool %llx size %llx flags %x virtual %d gpu %d, offset %llx (chunk %llu..%llu)",
                    (unsigned long long)addr, (unsigned long long)size, S.stage[kCompute] ? S.stage[kCompute]->hash.substr(0, 8).c_str() : "-", writes ? 1 : 0,
                    hit.pool ? (unsigned long long)reinterpret_cast<uint64_t>(hit.pool->base) : 0ull, hit.pool ? (unsigned long long)hit.pool->size : 0ull,
                    hit.pool ? hit.pool->flags : 0u, hit.pool ? (int)hit.pool->is_virtual : -1, hit.pool && hit.pool->gpu ? 1 : 0,
                    (unsigned long long)hit.offset, (unsigned long long)(hit.offset >> 26), (unsigned long long)((hit.offset + size - 1) >> 26));
        }
    }
    if (ms.valid) { ++g_sh_from_mirror; g_sh_bytes_mirror += size; } else g_sh_bytes_pool += size;
    VkBufferCopy c = {ms.valid ? ms.offset : host.offset, 0, size};
    vkCmdCopyBuffer(F->cb, ms.valid ? ms.buf : host.buffer, sh.buf.buffer, 1, &c);
    g_sh_copied = true;
    g_sh_kinds |= ms.valid ? 8 : 4;
    g_shadow_uses.push_back({addr, size, host, sh.buf.buffer, 0, writes, false, ms, true});
    BufRef r = host;
    r.buffer = sh.buf.buffer;
    r.offset = 0;
    r.avail = size;
    return r;
}

// After the dispatch: the written shadows go back to the pool; written direct ranges wait in g_out_pending. Shadows unused for a while
// are freed.
void shadows_after_dispatch() {
    bool any = false;
    for (const ShadowUse& u : g_shadow_uses) {
        if (!u.writes) continue;
        if (u.direct || (u.own && shadow_hold_on())) {
            // written again before it went back: one copy out for both. A shadow's result is held in the shadow the same way until something
            // else reads the pool there or the submission ends (out_end_submit), so dispatches over the same range share one copy in and out.
            if (out_add(u.addr, u.size, u.buf, u.off, u.host, !u.own)) ++g_sh_merged;
            continue;
        }
        if (!any) { memory_barrier(F->cb); any = true; }
        VkBufferCopy c = {0, u.host.offset, u.size};
        vkCmdCopyBuffer(F->cb, u.buf, u.host.buffer, 1, &c);
        g_sh_bytes_out += u.size;
        if (u.mirror.buf) {
            VkBufferCopy mc = {0, u.mirror.offset, u.size};
            vkCmdCopyBuffer(F->cb, u.buf, u.mirror.buf, 1, &mc);
            mirror_span_written(u.addr, u.size);
        }
    }
    if (any) gstamp("shadow out");
    g_shadow_uses.clear();
    g_shadowing = false;
    static uint64_t last_sweep = 0;
    if (F->serial - last_sweep > 600) {
        last_sweep = F->serial;
        NXLOG_I("nvn.replay", "compute shadows: %llu bound (%llu from the pool copy, %llu with no copy), in from the copy %llu MiB, in from the pool %llu MiB, out %llu MiB; "
                "direct to the pool copy %llu (%llu after filling pages, in %llu MiB, out %llu MiB in %llu flushes, %llu writes merged), %llu from a shadow holding a result",
                (unsigned long long)g_sh_uses, (unsigned long long)g_sh_from_mirror, (unsigned long long)g_sh_no_span, (unsigned long long)(g_sh_bytes_mirror >> 20),
                (unsigned long long)(g_sh_bytes_pool >> 20), (unsigned long long)(g_sh_bytes_out >> 20), (unsigned long long)g_sh_direct,
                (unsigned long long)g_sh_direct_filled, (unsigned long long)(g_sh_bytes_fill >> 20), (unsigned long long)(out_bytes_total() >> 20),
                (unsigned long long)g_sh_flushes, (unsigned long long)g_sh_merged, (unsigned long long)g_sh_held);
        uint64_t spans = 0, pages = 0;
        page_table_stats(&spans, &pages);
        NXLOG_I("nvn.replay", "global memory: %llu held-result flushes for pointer shaders, %llu write windows marked; page table %llu spans of 2 MiB, %llu pages; "
                "graphics pointer stages %llu, %llu with a slot marked; "
                "compute slot masks (cs_gmem_held) %llu, %.2f slots each (no pool %llu, virtual %llu, no storage %llu, held result %llu)",
                (unsigned long long)g_gmem_flushes, (unsigned long long)g_gmem_windows, (unsigned long long)spans, (unsigned long long)pages,
                (unsigned long long)g_gmask_stages, (unsigned long long)g_gmask_marked, (unsigned long long)g_csmask_n, g_csmask_n ? static_cast<double>(g_csmask_bits) / static_cast<double>(g_csmask_n) : 0.0,
                (unsigned long long)g_csmask_why[0], (unsigned long long)g_csmask_why[1], (unsigned long long)g_csmask_why[2], (unsigned long long)g_csmask_why[3]);
        for (auto it = g_shadows.begin(); it != g_shadows.end();) {
            if (F->serial - it->second.last > 600) {
                gfx::Buffer old = it->second.buf;
                forget_buffer_views(old.buffer);
                defer_destroy([old]() mutable { device().destroy_buffer(old); });
                it = g_shadows.erase(it);
            } else {
                ++it;
            }
        }
    }
}

Texture* g_su_tex = nullptr;                 // scene upscaler override (S14), see scene_upscale_for_draw
VkImageView g_su_view = VK_NULL_HANDLE;
Ref<Texture> g_tsr_depth;                   // temporal upscaling: the depth target of the G-buffer draw that gave this frame's camera
uint64_t g_tsr_frame = 0;                    // temporal upscaling: tone-map draws so far (the jitter phase of the frame being drawn)
// Temporal upscaling: a copy of a vertex slot 0 camera block with this frame's jitter added (extras::temporal_jitter_block). False: left alone.
bool tsr_jitter(uint32_t* words, size_t n_words) {
    Texture* d = S.depth_tex.get();
    if (!d || !tex_gpu(d)) return false;
    const VkExtent3D e = tex_image_extent(d, 0);
    return extras::temporal_jitter_block(words, n_words, e.width, e.height, g_tsr_frame);
}

// True when `n` bytes at `addr` lie in a memory pool backed by host memory, so a trace may print them: a virtual (sparse) pool's addresses
// (0x2000_0000_0000 and up in OoT: storage buffer 8 of compute 54ecc4b4) are not mapped on the host, and reading them faulted.
bool host_readable(uint64_t addr, uint64_t n) {
    const PoolHit h = pool_lookup(addr);
    return h.pool && !h.pool->is_virtual && h.pool->base && h.offset + n <= h.pool->size;
}

// The work-group counts of the dispatch being recorded (do_dispatch), for the c[0] header.
uint32_t g_dispatch_groups[3] = {1, 1, 1};
VkDescriptorBufferInfo g_c0_made = {};   // the c[0] header compute_c0 made for the dispatch being recorded (buffer null: none)

// c[0] of an NVN2 compute program, as the driver lays it out per dispatch (apply_nvn2_compute_table): a 0x10-byte header (word 0 zero, words 1-3
// the dispatch's work-group counts x, y, z), the push constant area (ShaderControl::c0_push bytes of SmallState::push_cs), the uniform block and
// storage buffer descriptors {address, size} of the bound compute slots, then their texture and image handles. The shader reaches the
// descriptors and handles through its bindings (the translator maps them); the copy keeps any plain read of those words right.
// The header words: OoT's sky-lighting pass 16a42858 (spherical harmonics of the sky panorama, dispatched 1x1x1 with 32x32 invocations) reads
// c[0] 0x4 as the row width in groups and 0x4 * 0x8 as the sample count; left zero, it divided by zero, its inf/NaN went into the ambient term of
// every material and whole frames came out white (A12). False: not an NVN2 compute program, or no room.
// The pool address table of this frame (A17, nvn_gpu_pool.cpp): 32-byte entries {guest address, size, device address, 0} sorted by guest
// address, in the upload ring, remade when the frame or the set of imports changes. Shaders that resolve global memory at run time find its
// device address in c[0] at kPoolTableC0 (u64) and the entry count at kPoolTableC0 - 8 (written by graphics_c0 and fill_compute_c0).
constexpr size_t kPoolTableC0 = 0xff8;
// The guest page table (nvn_gpu_pool.cpp page_table_words; AmpereGlobal.EmitPageWalk), for shaders with the "gpt" token: c[0] 0xfe0 the level
// shifts, 0xfe4 the root entry count, 0xfe8 the table's device address. The sorted import table above only for older translations (no "gpt").
constexpr size_t kPageTableC0 = 0xfe0;
// One u32 per stage (AmpereGlobal.SlotMaskC0): bit k = storage descriptor slot k is checked before the page table. Index 0 compute, a graphics
// stage (storage_c0_base(stage) >> 8) & 7.
constexpr size_t kGmemSlotMaskC0 = 0xfc0;
void pool_table_words(uint8_t* c0, bool old_table) {
    {
        uint32_t w[2] = {0, 0};
        uint64_t a = 0;
        page_table_words(&w[0], &w[1], &a);
        std::memcpy(c0 + kPageTableC0, w, 8);
        std::memcpy(c0 + kPageTableC0 + 8, &a, 8);
    }
    if (!old_table && archexp::gmem_window_on()) archexp::gmem_window_words(c0 + 0xff0);   // E3 (measurement only)
    if (!old_table) return;
    static Frame* frame = nullptr;
    static uint64_t gen = ~0ull, addr = 0;
    static uint32_t count = 0;
    if (!device().features().buffer_device_address) return;
    const uint64_t now = pool_address_table_generation();
    if (frame != F || gen != now) {
        frame = F;
        gen = now;
        addr = 0;
        count = 0;
        static std::vector<PoolAddress> table;
        pool_address_table(table);
        // the shaders' search covers 2^16 entries (AmpereGlobal.SearchSteps); past that, accesses to the last ranges would read 0
        if (table.size() > 65536 && first_time_key("pool-table-size"))
            NXLOG_W(kTag, "pool address table has %zu entries, the shaders search 65536: global accesses to the last ranges read 0", table.size());
        if (!table.empty()) {
            UploadAlloc u = F->upload(32ull * table.size(), 32);
            if (u) {
                uint64_t* w = reinterpret_cast<uint64_t*>(u.ptr);
                for (size_t i = 0; i < table.size(); ++i) { w[4 * i] = table[i].guest; w[4 * i + 1] = table[i].size; w[4 * i + 2] = table[i].dev; w[4 * i + 3] = 0; }
                static VkBuffer buf = VK_NULL_HANDLE;
                static uint64_t buf_addr = 0;
                if (u.buffer != buf) { buf = u.buffer; buf_addr = device().buffer_address(buf); }
                if (buf_addr) { addr = buf_addr + u.offset; count = static_cast<uint32_t>(table.size()); }
            }
        }
    }
    std::memcpy(c0 + kPoolTableC0, &addr, 8);
    std::memcpy(c0 + kPoolTableC0 - 8, &count, 4);
}

constexpr VkDeviceSize kComputeC0Bytes = 0x1000;
// [nvn] cs_gmem_held (default on; cpuperf_ab_what name cs_gmem_held): a compute pointer shader (A17 "gmem") that does not store through
// pointers checks only the storage slots whose binding may differ from the pool before the page table: a result held in the pool copy or a
// shadow (out_overlaps), a virtual pool. As graphics_c0 does for draws. The other slots are bound to the pool copy or a shadow made from the
// pool or from a current copy, so the page table (the pool) holds the same bytes there; gmem_before_op has sent held results near the
// shader's uniform pointers back already. Before, every slot was checked for every 32-bit pointer access: OoT's culling/HiZ chain
// (a17259b2 x6, a68b51f9, d89e74b6, 39a1f760, 5d8324bd: 11 storage slots each) took ~0.33 ms per dispatch at 1080 (Kokiri outside, ~3.3 ms
// per frame in the submission the game's main thread waits on), ~0.04 ms with the slot loop skipped. Shaders that store or do atomics
// through pointers ("gmemw", OoT's culling root 4c1e8564) keep every slot: their stores must reach the binding that is copied back.
bool cs_gmem_held_on();
bool fill_compute_c0(const ShaderControl& c, uint8_t* p, size_t n, bool old_table = true, bool held_mask = false) {
    if (!c.nvn2 || c.stage != 5 || c.c0_table > n) return false;
    std::memset(p, 0, n);
    std::memcpy(p + 4, g_dispatch_groups, 12);
    if (c.c0_push) std::memcpy(p + 0x10, S.small.push_cs, std::min<uint32_t>(c.c0_push, kPushAreaBytes));
    size_t off = 0x10 + c.c0_push;
    auto put64 = [&](uint64_t v) { std::memcpy(p + off, &v, 8); };
    auto desc = [&](const GState::Bind* b) {
        put64(b ? b->addr : 0);
        const uint32_t size = b ? static_cast<uint32_t>(std::min<uint64_t>(b->size, 0xffffffffu)) : 0;
        std::memcpy(p + off + 8, &size, 4);
        off += 0x10;
    };
    for (int i = 0; i < c.c0_ubos; ++i) desc(i < 16 ? &S.ubo[kCompute][i] : nullptr);
    for (int i = 0; i < c.c0_ssbos; ++i) desc(i < 16 ? &S.ssbo[kCompute][i] : nullptr);
    for (int i = 0; i < c.c0_textures; ++i, off += 8) put64(i < 64 ? S.tex[kCompute][i] : 0);
    for (int i = 0; i < c.c0_images; ++i, off += 8) put64(i < 16 ? S.img[kCompute][i] : 0);
    if (n >= kPoolTableC0 + 8 && off <= kGmemSlotMaskC0) {
        // compute: every bound storage descriptor before the page table (compute shadows and virtual pools are bound, not in the table), or
        // with held_mask only the slots whose binding may differ from the pool (cs_gmem_held_on)
        uint32_t all = ~0u;
        static const bool exp_none = nxrt::config_get_bool("nvn", "exp_cs_gmem_none", false);   // measurement only: no slot checked (wrong reads of held results)
        if (held_mask && exp_none) all = 0;
        else if (held_mask) {
            all = 0;
            for (int k = 0; k < 16 && k < c.c0_ssbos; ++k) {
                const GState::Bind& b = S.ssbo[kCompute][k];
                if (!b.addr) continue;
                const PoolHit hit = pool_lookup(b.addr);
                const int why = !hit.pool ? 0 : hit.pool->is_virtual ? 1 : !hit.pool->base ? 2 : out_overlaps(b.addr, b.size ? b.size : 1) ? 3 : -1;
                if (why >= 0) { all |= 1u << k; ++g_csmask_why[why]; }
            }
            {
                static int logged = 0;
                if (logged < 40 && all && first_time_key("csmask " + std::to_string(c.c0_table) + " " + std::to_string(all))) {
                    ++logged;
                    std::string sl;
                    for (int k = 0; k < 16 && k < c.c0_ssbos; ++k) {
                        const GState::Bind& b = S.ssbo[kCompute][k];
                        char t[64];
                        std::snprintf(t, sizeof t, " %d:%llx+%llx%s", k, (unsigned long long)b.addr, (unsigned long long)b.size, (all >> k) & 1 ? "*" : "");
                        sl += t;
                    }
                    NXLOG_I(kTag, "cs_gmem_held: cs %s mask %x:%s", S.stage[kCompute] ? S.stage[kCompute]->hash.substr(0, 8).c_str() : "-", all, sl.c_str());
                }
            }
            ++g_csmask_n;
            g_csmask_bits += static_cast<uint64_t>(std::popcount(all));
        }
        std::memcpy(p + kGmemSlotMaskC0, &all, 4);
    }
    if (n >= kPoolTableC0 + 8 && off <= kPageTableC0) pool_table_words(p, old_table);
    return true;
}
// c[0] of a graphics draw whose shader resolves global memory at run time (ShaderReflection::global_fallback, A17): the static driver constants
// (driver_constants_buffer: the agl point size at 0x610) with the storage descriptors {address, size} of every graphics stage's bound slots,
// where the NVN driver keeps them (0x110 + 0x100 * hardware stage + 0x10 * slot; task 0x650, mesh 0x770; storage_slot_for). 4 KiB per descriptor
// set built for such a shader; other draws keep the shared static buffer.
constexpr VkDeviceSize kGraphicsC0Bytes = 0x1000;
// The tables of this submission by content: draws that bind the same storage buffers share one (no new upload, and the descriptor set cache
// finds the set again).
std::unordered_map<uint64_t, UploadAlloc> g_c0_tables;
uint64_t g_ubo_unaligned_n = 0, g_ubo_unaligned_bytes = 0;
uint64_t g_c0_made_n = 0, g_c0_reused_n = 0;
alignas(16) uint8_t g_c0_scratch[kGraphicsC0Bytes];
// The table in g_c0_scratch was built for this draw's sets (bind_sets clears it): set_signature and build_set share it.
int g_c0_cached = -1;            // -1 none, else old_table
uint64_t g_c0_cached_h = 0;
bool g_c0_last_persist = false;  // the last graphics_c0 bound a persistent slot (build_set: a kept set may hold it)
uint64_t graphics_c0_build_raw(bool old_table);
// Builds this draw's graphics c[0] table into g_c0_scratch and returns its content hash.
uint64_t graphics_c0_build(bool old_table) {
    if (g_c0_cached == (old_table ? 1 : 0)) return g_c0_cached_h;
    g_c0_cached_h = graphics_c0_build_raw(old_table);
    g_c0_cached = old_table ? 1 : 0;
    return g_c0_cached_h;
}
uint64_t graphics_c0_build_raw(bool old_table) {
    uint8_t* const scratch = g_c0_scratch;
    UploadAlloc u;
    u.ptr = scratch;
    // the static driver constants, read once: their buffer is upload memory (write-combined), slow to read from every draw
    alignas(16) static uint8_t consts[kGraphicsC0Bytes];
    static bool have_consts = false;
    if (!have_consts) {
        VkDeviceSize range = 0;
        if (driver_constants_buffer(&range) && driver_constants_ptr()) std::memcpy(consts, driver_constants_ptr(), static_cast<size_t>(kGraphicsC0Bytes));
        have_consts = true;
    }
    std::memcpy(u.ptr, consts, static_cast<size_t>(kGraphicsC0Bytes));
    static const int kStagesWithStorage[] = {kVertex, kTessCtl, kTessEval, kGeometry, kFragment, kTask, kMesh};
    // The slots a pointer shader checks before the page table (AmpereGlobal.SlotMaskC0): those whose binding may differ from the pool, a
    // result held in the copy (build_set binds the copy then) or a virtual pool (not in the table). Others go straight to the table.
    uint32_t mask_of[kStages] = {};
    for (int st : kStagesWithStorage) {
        if (!S.stage[st] || !S.stage[st]->refl.global_fallback) continue;
        for (int k = 0; k < 16; ++k) {
            const GState::Bind& b = S.ssbo[st][k];
            if (!b.addr) continue;
            const PoolHit hit = pool_lookup(b.addr);
            if (!hit.pool || hit.pool->is_virtual || !hit.pool->base || out_overlaps(b.addr, b.size ? b.size : 1)) mask_of[st] |= 1u << k;
        }
        ++g_gmask_stages;
        if (mask_of[st]) ++g_gmask_marked;
    }
    for (int st : kStagesWithStorage) {
        const int base = storage_c0_base(st);
        for (int k = 0; k < 16; ++k) {
            const GState::Bind& b = S.ssbo[st][k];
            const uint64_t addr = b.addr;
            const uint32_t size = b.addr ? static_cast<uint32_t>(std::min<uint64_t>(b.size, 0xffffffffu)) : 0;
            std::memcpy(u.ptr + base + 0x10 * k, &addr, 8);
            std::memcpy(u.ptr + base + 0x10 * k + 8, &size, 4);
        }
        const uint32_t smask = archexp::gmem_no_slots() ? 0u : mask_of[st];   // E3: [nvn] exp_gmem_no_slots (measurement only)
        std::memcpy(u.ptr + kGmemSlotMaskC0 + 4 * ((base >> 8) & 7), &smask, 4);
    }
    pool_table_words(u.ptr, old_table);
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < kGraphicsC0Bytes; i += 8) {
        uint64_t w;
        std::memcpy(&w, scratch + i, 8);
        h = (h ^ w) * 1099511628211ull;
        h ^= h >> 29;
    }
    return h;
}

// [nvn] c0_persist (default on; cpuperf_ab_what c0_persist): the graphics c[0] tables of pointer shaders live in a persistent buffer, one
// slot per distinct content, written once and never changed, so a descriptor set that binds one can be reused (memo) and kept (set_keep)
// like any read-only set: the table's content hash is part of the set's signature. Before, every such set was built for every draw (~1,000
// per frame in Kokiri outside). When the buffer is full a new one starts (an era, also in the signature, so no set of an older era is found
// again); the buffer of the era before the previous one is destroyed after the GPU is done with it (OoT's first scene fills an era in ~100 s).
constexpr size_t kC0Slots = 8192;            // 32 MiB per era
struct C0Store {
    gfx::Buffer buf, prev;
    std::unordered_map<uint64_t, uint32_t> slot;   // content hash -> slot
    std::vector<uint8_t> copy;                      // CPU copy of every slot (collision check; the buffer is write-combined)
    uint32_t used = 0;
    int eras = 0;
    bool failed = false;
};
C0Store g_c0s;
uint64_t g_c0_era = 0, g_c0_persist_hit = 0, g_c0_persist_new = 0, g_c0_collide = 0;   // statistics
bool c0_persist_on() {
    static const bool on = nxrt::config_get_bool("nvn", "c0_persist", true);
    return on && !g_c0s.failed && ab_on(kAbC0Persist);
}
// The persistent slot of the table in g_c0_scratch (hash h); false: none (store full or failed: use the upload ring).
bool c0_persist_slot(uint64_t h, VkDescriptorBufferInfo& bi) {
    C0Store& c = g_c0s;
    auto it = c.slot.find(h);
    if (it != c.slot.end()) {
        if (std::memcmp(c.copy.data() + static_cast<size_t>(it->second) * kGraphicsC0Bytes, g_c0_scratch, kGraphicsC0Bytes) == 0) {
            ++g_c0_persist_hit;
            bi = {c.buf.buffer, static_cast<VkDeviceSize>(it->second) * kGraphicsC0Bytes, kGraphicsC0Bytes};
            return true;
        }
        ++g_c0_collide;
        return false;   // another content under the same hash: this draw uses the upload ring
    }
    if (!c.buf.buffer || c.used == kC0Slots) {
        gfx::Buffer nb;
        if (device().create_buffer(kC0Slots * kGraphicsC0Bytes, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, gfx::MemoryUse::Upload, &nb,
                                   "nvn persistent graphics c0 tables") != VK_SUCCESS || !nb.mapped) {
            c.failed = true;
            NXLOG_W(kTag, "c0_persist: no buffer for the graphics c[0] tables: they stay in the upload ring");
            return false;
        }
        if (c.prev.buffer) {   // two eras old: no submission in flight binds it (eras last minutes), and its kept sets are never found again
            gfx::Buffer old = c.prev;
            defer_destroy([old]() mutable { device().destroy_buffer(old); });
        }
        c.prev = c.buf;
        c.buf = nb;
        c.slot.clear();
        c.copy.assign(kC0Slots * kGraphicsC0Bytes, 0);
        c.used = 0;
        ++c.eras;
        ++g_c0_era;
        if (c.eras > 1) NXLOG_I(kTag, "c0_persist: graphics c[0] table buffer full: era %d", c.eras);
    }
    const uint32_t k = c.used++;
    std::memcpy(static_cast<uint8_t*>(c.buf.mapped) + static_cast<size_t>(k) * kGraphicsC0Bytes, g_c0_scratch, kGraphicsC0Bytes);
    std::memcpy(c.copy.data() + static_cast<size_t>(k) * kGraphicsC0Bytes, g_c0_scratch, kGraphicsC0Bytes);
    c.slot.emplace(h, k);
    ++g_c0_persist_new;
    bi = {c.buf.buffer, static_cast<VkDeviceSize>(k) * kGraphicsC0Bytes, kGraphicsC0Bytes};
    return true;
}

bool graphics_c0(VkDescriptorBufferInfo& bi, bool old_table) {
    const VkDeviceSize align = device().properties().limits.minUniformBufferOffsetAlignment;
    uint8_t* const scratch = g_c0_scratch;
    const uint64_t h = graphics_c0_build(old_table);
    g_c0_last_persist = false;
    if (c0_persist_on() && c0_persist_slot(h, bi)) { g_c0_last_persist = true; return true; }
    auto it = g_c0_tables.find(h);
    if (it != g_c0_tables.end()) {
        ++g_c0_reused_n;
        bi = {it->second.buffer, it->second.offset, kGraphicsC0Bytes};
        return true;
    }
    UploadAlloc up = F->upload(kGraphicsC0Bytes, std::max<VkDeviceSize>(align, 256));
    if (!up) return false;
    std::memcpy(up.ptr, scratch, static_cast<size_t>(kGraphicsC0Bytes));
    g_c0_tables.emplace(h, up);
    ++g_c0_made_n;
    bi = {up.buffer, up.offset, kGraphicsC0Bytes};
    return true;
}

bool compute_c0(const ShaderInfo& s, VkDescriptorBufferInfo& bi) {
    if (!s.control.nvn2 || s.control.stage != 5) return false;
    const VkDeviceSize align = device().properties().limits.minUniformBufferOffsetAlignment;
    UploadAlloc u = F->upload(kComputeC0Bytes, std::max<VkDeviceSize>(align, 256));
    if (!u || !fill_compute_c0(s.control, u.ptr, static_cast<size_t>(kComputeC0Bytes), s.refl.global_fallback && !s.refl.global_pt,
                               s.refl.global_fallback && s.refl.global_pt && !s.refl.global_writes && cs_gmem_held_on())) return false;
    bi = {u.buffer, u.offset, kComputeC0Bytes};
    g_c0_made = bi;   // do_dispatch: an indirect dispatch's GPU-written counts are copied into words 1-3 of this header
    return true;
}

uint64_t g_sets_built = 0, g_sets_cached = 0;   // statistics: descriptor sets resolved, of them found in this submission's cache
// ---- [nvn] set_keep: what a kept set's bindings depended on (recorded by build_set, checked by keep_valid below) ----------------------
struct KDep {
    uint8_t kind = 0;          // 0 nothing to check, 1 uniform block (the copy-or-pool decision), 2 storage range (no held result), 3 texture, 4 storage image
    bool texel = false;        // kind 3: a texel buffer: its range is checked like kind 2 too
    bool writes = false;       // kind 2: the shader may write it: a found set notes the GPU write as build_set does (gpu_wrote)
    ReadDep rd;
    uint64_t addr = 0, range = 0, note = 0;   // kinds 2 and 3 (texel): the range bound (held-result check) and the bytes upd_note_read is told
    const BindingDesc* d = nullptr;           // kind 3
    const void* pool = nullptr;               // kinds 2 and 3 (texel): the pool the address resolved in (kind 1: rd.pool)
    Texture* tex = nullptr;
    uint32_t tex_id = 0;
    uint64_t stamp = 0, vh = 0, sh = 0;       // view stamp of the image, hashes of the view info and the sampler info
    uint8_t has_view = 0, has_sampler = 0;
};
struct BVal { uint64_t binding, a, b, c; };   // set_reuse = verify: what build_set wrote for each binding (keep verify names the one that differs)
std::vector<BVal> g_bvals;
bool g_bvals_on = false;
std::unordered_map<VkDescriptorSet, std::vector<BVal>> g_ds_bvals;   // set_reuse = verify: what each set of the submission was built with
struct KeepRec {
    std::vector<KDep> deps;
    const char* bad = nullptr;  // why the set cannot be kept (null: it can)
    uint64_t content = 0;       // the content key build_set computed
    VkDescriptorSet ds = VK_NULL_HANDLE;
};
KeepRec* g_krec = nullptr;      // build_set records into it while bind_sets builds a set it may keep
uint64_t g_last_set_key = 0;    // the content key of the last set build_set resolved
std::unordered_map<VkDescriptorSet, uint64_t> g_set_content;   // set_reuse = verify: the content key of each kept set
VkDescriptorSet keep_alloc(VkDescriptorSetLayout layout);   // below: a set of the persistent pools
uint64_t bytes_hash(const void* p, size_t n) {
    uint64_t h = 1469598103934665603ull;
    const uint8_t* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}
// The support buffer of sets without render scales: the defaults, in a buffer of its own that outlives submissions (a kept set binds it; the
// per-submission g_support in the upload ring held the same bytes).
gfx::Buffer g_support_keep;
VkBuffer support_keep_buffer() {
    if (!g_support_keep.buffer) {
        if (device().create_buffer(sizeof(SupportBuffer), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, gfx::MemoryUse::Upload, &g_support_keep,
                                   "nvn support buffer") != VK_SUCCESS)
            return VK_NULL_HANDLE;
        const SupportBuffer sb = support_buffer_defaults();
        std::memcpy(g_support_keep.mapped, &sb, sizeof sb);
    }
    return g_support_keep.buffer;
}

VkDescriptorSet build_set(const ProgLayout& pl, uint32_t set) {
    // Scratch arrays kept across calls (render thread only; nothing here calls build_set again): about one set per draw, and allocating
    // them each time was a tenth of the render thread. Reserved to the binding count first, so the pointers into them stay valid.
    static std::vector<VkWriteDescriptorSet> writes;
    static std::vector<VkDescriptorBufferInfo> binfo;
    static std::vector<VkDescriptorImageInfo> iinfo;
    static std::vector<VkBufferView> tinfo;
    writes.clear(); binfo.clear(); iinfo.clear(); tinfo.clear();
    const size_t nb = pl.bindings.size();
    writes.reserve(nb);
    binfo.reserve(nb);
    iinfo.reserve(nb);
    tinfo.reserve(nb);
    uint64_t key = 1469598103934665603ull + set;
    auto mix = [&](uint64_t v) { key = (key ^ v) * 1099511628211ull; };
    g_bvals_on = set_reuse_mode() == SetReuse::Verify;
    g_bvals.clear();
    mix(reinterpret_cast<uint64_t>(pl.layout));
    for (const BindingDesc& d : pl.bindings) {
        if (d.set != set) continue;
        const DescriptorBinding& b = d.shader->refl.bindings[d.index];
        VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstBinding = d.binding;
        w.descriptorCount = 1;
        w.descriptorType = d.type;
        KDep* kd = nullptr;
        if (g_krec) { g_krec->deps.emplace_back(); kd = &g_krec->deps.back(); }
        auto kbad = [&](const char* why) { if (g_krec && !g_krec->bad) g_krec->bad = why; };
        if (d.type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || d.type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
            VkDescriptorBufferInfo bi = {dummy_buffer(), 0, 65536};
            switch (b.nvn.source) {
                case NvnSource::SupportBuffer:
                    if (g_draw_support) {
                        bi = VkDescriptorBufferInfo{g_draw_support.buffer, g_draw_support.offset, sizeof(SupportBuffer)};
                        kbad("support buffer with render scales");
                    } else if (VkBuffer kb = support_keep_buffer()) {
                        bi = VkDescriptorBufferInfo{kb, 0, sizeof(SupportBuffer)};
                    } else {
                        bi = VkDescriptorBufferInfo{g_support.buffer, g_support.offset, sizeof(SupportBuffer)};
                        kbad("support buffer");
                    }
                    break;
                case NvnSource::DriverConstants: {
                    if (d.nvn_stage == kCompute && compute_c0(*d.shader, bi)) break;   // a compute table with push constants: built for this dispatch
                    if (d.nvn_stage != kCompute && d.shader->refl.global_fallback && graphics_c0(bi, !d.shader->refl.global_pt)) {   // storage descriptors for the A17 fallback
                        if (!g_c0_last_persist) kbad("graphics c[0] in the upload ring");
                        break;
                    }
                    VkDeviceSize range = 0;
                    VkBuffer buf = driver_constants_buffer(&range);
                    if (buf) bi = {buf, 0, range};
                    break;
                }
                case NvnSource::ConstantData: {
                    VkDeviceSize range = 0;
                    VkBuffer buf = constant_data_buffer(d.shader, &range);
                    if (buf) bi = {buf, 0, range};
                    break;
                }
                case NvnSource::UniformBlock: {
                    const int idx = b.nvn.index;
                    if (idx >= 0 && idx < 16 && S.ubo[d.nvn_stage][idx].addr) {
                        const GState::Bind& bind = S.ubo[d.nvn_stage][idx];
                        if (d.nvn_stage == kCompute || !out_lazy()) {                   // draws: checked before the op (out_before_op)
                            const VkDeviceSize ua = device().properties().limits.minUniformBufferOffsetAlignment;
                            out_draw_read(bind.addr, bind.size ? bind.size : 65536, ua && (bind.addr % ua) == 0, kOutDrawBuffer);   // resolve_read serves the copy
                        }
                        BufRef br = resolve_read_dep(bind.addr, bind.size, kReadUniform, kd ? &kd->rd : nullptr);
                        if (kd) {
                            kd->kind = 1;
                            kd->addr = bind.addr;
                            kd->range = bind.size ? bind.size : 65536;
                            if (!kd->rd.keep) kbad("uniform block without a copy chunk");
                            if (upd_find(bind.addr, bind.size ? bind.size : 65536)) kbad("uniform block in an update range");
                            if (!br.ok) kbad("uniform block outside a pool");   // a pool registered later may cover it
                        }
                        const VkDeviceSize align = device().properties().limits.minUniformBufferOffsetAlignment;
                        const float ufs = ufs_scale_for(d, idx);
                        const bool tsr = d.nvn_stage == kVertex && idx == 0 && bind.size >= 272 && extras::temporal_wanted() && S.depth_tex;
                        UploadAlloc snap;
                        if (br.ok && !g_upd.empty() && ufs == 1.0f && !tsr)
                            snap = ubo_snapshot(bind.addr, std::min<VkDeviceSize>({bind.size ? bind.size : 65536, br.avail, 65536}), align);
                        if (snap) {
                            bi = {snap.buffer, snap.offset, std::min<VkDeviceSize>({bind.size ? bind.size : 65536, br.avail, 65536})};
                        } else if (br.ok && (ufs != 1.0f || tsr)) {
                            // the game's scale on some floats of this block (nxrt/gfx_tuning.h): bound from a patched copy in the upload ring
                            const VkDeviceSize n = std::min<VkDeviceSize>({bind.size ? bind.size : 65536, br.avail, 65536});
                            UploadAlloc u = F->upload(n, std::max<VkDeviceSize>(align, 256));
                            if (u) {
                                std::memcpy(u.ptr, host_ptr(bind.addr), n);
                                if (tsr && n >= 272) tsr_jitter(reinterpret_cast<uint32_t*>(u.ptr), static_cast<size_t>(n / 4));   // temporal upscaling
                                const uint32_t mask = ufs != 1.0f ? nxrt::gfx::tuning().ufs_mask.load(std::memory_order_relaxed) : 0u;
                                for (uint32_t i = 0; i < 32; ++i) {
                                    if (!(mask & (1u << i)) || (i + 1) * 4 > n) continue;
                                    float v;
                                    std::memcpy(&v, static_cast<uint8_t*>(u.ptr) + i * 4, 4);
                                    v *= ufs;
                                    std::memcpy(static_cast<uint8_t*>(u.ptr) + i * 4, &v, 4);
                                }
                                bi = {u.buffer, u.offset, n};
                            }
                        } else if (br.ok && (br.offset % align) == 0) {
                            bi = {br.buffer, br.offset, std::min<VkDeviceSize>({bind.size ? bind.size : 65536, br.avail, 65536})};
                        } else if (br.ok) {
                            // unaligned binding: copy into the upload ring
                            kbad("unaligned uniform block");
                            const VkDeviceSize n = std::min<VkDeviceSize>(bind.size ? bind.size : 65536, 65536);
                            ++g_ubo_unaligned_n; g_ubo_unaligned_bytes += n;
                            UploadAlloc u = F->upload(n, std::max<VkDeviceSize>(align, 256));
                            if (u) {
                                std::memcpy(u.ptr, host_ptr(bind.addr), n);
                                bi = {u.buffer, u.offset, n};
                            }
                        }
                    }
                    break;
                }
                case NvnSource::StorageBuffer: {
                    const int slot = b.nvn.index >= 0 ? b.nvn.index : storage_slot_for(d.nvn_stage, b.nvn.c0_offset);   // NVN2 compute: index from the c[0] table
                    if (g_trace) {
                        const bool bound = slot >= 0 && slot < 16 && S.ssbo[d.nvn_stage][slot].addr;
                        const uint64_t a = bound ? S.ssbo[d.nvn_stage][slot].addr : 0, sz = bound ? S.ssbo[d.nvn_stage][slot].size : 0;
                        std::string words;
                        if (const uint8_t* p = bound && host_readable(a, std::min<uint64_t>(sz, 64)) ? host_ptr(a) : nullptr)
                            for (int k = 0; k < 16 && static_cast<uint64_t>(k) * 4 < sz; ++k) { char w[12]; std::snprintf(w, sizeof w, " %08x", reinterpret_cast<const uint32_t*>(p)[k]); words += w; }
                        TRACE("    ssbo %s stage %d slot %d (c0 0x%x) %s %llx/%llu:%s", b.name.c_str(), d.nvn_stage, slot, b.nvn.c0_offset, bound ? "bound" : "NOT BOUND",
                              static_cast<unsigned long long>(a), static_cast<unsigned long long>(sz), words.c_str());
                        // A pointer shader (A17): where the 64-bit pointers among those words lead (pool, page table), since a page the table
                        // does not map reads 0 in the shader.
                        if (d.shader && d.shader->refl.global_fallback && bound && host_readable(a, std::min<uint64_t>(sz, 64)))
                            for (int k = 0; k + 1 < 16 && static_cast<uint64_t>(k + 2) * 4 <= sz; k += 2) {
                                uint64_t v;
                                std::memcpy(&v, host_ptr(a) + k * 4, 8);
                                if (v < (1ull << 32) || v >= (1ull << 47)) continue;
                                const PoolHit h = pool_lookup(v);
                                const uint64_t dv = page_table_lookup(v);
                                TRACE("      ptr word %d: %llx -> %s pool %llx+0x%llx (%llu MiB%s), page table %s%llx", k, static_cast<unsigned long long>(v),
                                      h.pool ? "in" : "in no", static_cast<unsigned long long>(h.pool ? reinterpret_cast<uint64_t>(h.pool->base) : 0),
                                      static_cast<unsigned long long>(h.offset), static_cast<unsigned long long>(h.pool ? h.pool->size >> 20 : 0),
                                      h.pool && h.pool->is_virtual ? ", virtual" : "", dv ? "" : "UNMAPPED ", static_cast<unsigned long long>(dv));
                            }
                    }
                    if (slot >= 0 && slot < 16 && S.ssbo[d.nvn_stage][slot].addr) {
                        const GState::Bind& bind = S.ssbo[d.nvn_stage][slot];
                        // the shader may write the binding (its SPIR-V stores or atomics into it; a pointer shader that stores through
                        // pointers, A17 gmemw, counts as writing everything it binds: storage_binding_writes)
                        const bool writes = storage_binding_writes(b, d.shader);
                        BufRef br = resolve_buffer(bind.addr, bind.size);
                        upd_note_read(bind.addr, bind.size ? bind.size : br.avail);
                        // The console's storage access is a raw load at the descriptor's address: nothing stops a shader at the bound size.
                        // [nvn] storage_min_range (bytes) widens small bindings to that much of the memory behind them (diagnosis switch).
                        static const VkDeviceSize min_range = static_cast<VkDeviceSize>(std::max<int64_t>(0, nxrt::config_get_int("nvn", "storage_min_range", 0)));
                        bi = {br.buffer, br.offset, std::min<VkDeviceSize>(std::max<VkDeviceSize>(bind.size ? bind.size : br.avail, min_range), br.avail)};
                        if (kd) {
                            kd->kind = 2;
                            kd->addr = bind.addr;
                            kd->range = bi.range;
                            kd->note = bind.size ? bind.size : br.avail;
                            kd->pool = pool_lookup(bind.addr).pool;
                            kd->writes = writes;
                            if (g_shadowing) kbad("storage buffer of a shadowed dispatch");
                            if (!br.ok) kbad("storage buffer outside a pool");   // a pool registered later may cover it
                        }
                        if (!g_shadowing && br.ok && out_lazy() && out_overlaps_window(bind.addr, bi.range)) {   // a draw over a held compute result: the copy
                            const BufRef hb = held_or_pool(bind.addr, bi.range, kOutStorageXfb, false);
                            kbad("storage buffer over a held result");
                            if (hb.buffer != br.buffer) {
                                bi.buffer = hb.buffer;
                                bi.offset = hb.offset;
                                if (writes) {
                                    g_gw_site = 3; gpu_wrote(bind.addr, bi.range, kWriteStorage); g_gw_site = 0;
                                    out_add(bind.addr, bi.range, hb.buffer, hb.offset, br);
                                    if (!held_version_epoch()) desc_epoch_bump(kBumpWrite);   // the held result may have grown: readers resolve anew (out_version moved then)   // what the draw writes there is held like a dispatch's
                                }
                                break;
                            }
                        }
                        if (g_shadowing && br.ok) {                                  // before gpu_wrote: the shadow reads the pool copy while valid
                            const BufRef sh = shadow_of(bind.addr, bi.range, br, writes);
                            bi.buffer = sh.buffer;
                            bi.offset = sh.offset;
                        }
                        if (br.ok && writes) { g_gw_site = d.nvn_stage == kCompute ? 5 : 4; gpu_wrote(bind.addr, bi.range, kWriteStorage); g_gw_site = 0; }   // the shader may write it
                    }
                    break;
                }
                case NvnSource::UniformAsStorage: {
                    // a compute shader that reads uniform block n through its c[0] descriptor: the block's buffer as a storage buffer
                    const int idx = b.nvn.index;
                    if (idx >= 0 && idx < 16 && S.ubo[d.nvn_stage][idx].addr) {
                        const GState::Bind& bind = S.ubo[d.nvn_stage][idx];
                        const BufRef br0 = resolve_buffer(bind.addr, bind.size);
                        const VkDeviceSize range = std::min<VkDeviceSize>(bind.size ? bind.size : br0.avail, br0.avail);
                        const BufRef br = held_or_pool(bind.addr, range, kOutDispatch, true);
                        upd_note_read(bind.addr, range);
                        bi = {br.buffer, br.offset, range};
                    }
                    break;
                }
                default:
                    kbad("buffer of an unknown source");
                    note_unsupported("shader buffer binding of an unknown NVN source");
                    if (first_time_key("unknown-buffer " + d.shader->hash + " " + b.name))
                        NXLOG_W(kTag, "shader %s binding %s (set %u binding %u, %s, c0 offset 0x%x, sb slot %d): unknown NVN source, the 64 KiB dummy buffer is bound",
                                d.shader->hash.c_str(), b.name.c_str(), d.set, d.binding, d.type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ? "storage" : "uniform",
                                b.nvn.c0_offset, b.nvn.sb_slot);
                    break;
            }
            binfo.push_back(bi);
            w.pBufferInfo = &binfo.back();
            mix(reinterpret_cast<uint64_t>(bi.buffer)); mix(bi.offset); mix(bi.range);
            if (g_bvals_on) g_bvals.push_back({d.binding, reinterpret_cast<uint64_t>(bi.buffer), bi.offset, bi.range});
        } else if (d.type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
            VkDescriptorImageInfo ii = {dummy_sampler(), dummy_view(view_type_for(b), b.image.depth), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            TexResolved r = resolve_texture(d);
            if (kd) {
                kd->kind = 3;
                kd->d = &d;
                kd->tex = r.tex;
                kd->tex_id = r.tex ? r.tex->id : 0;
                kd->has_view = r.has_view;
                kd->has_sampler = r.has_sampler;
                kd->vh = r.has_view ? bytes_hash(&r.view, sizeof r.view) : 0;
                kd->sh = r.has_sampler ? bytes_hash(&r.smp, sizeof r.smp) : 0;
                if (!g_fb_use.empty() || !g_vol3d_use.empty() || g_su_tex) kbad("feedback, 3D copy or upscaled scene");
            }
            if (r.tex) {
                const auto fb = g_fb_use.find(r.tex);
                if (fb != g_fb_use.end()) r.tex = fb->second;      // a feedback draw samples the copy made before it
            }
            if (r.tex && wants_vol3d(b, r)) {                      // a sampler3D over 2D storage samples the 3D copy (vol3d_prepare)
                const auto vc = g_vol3d_use.find(r.tex);
                if (vc != g_vol3d_use.end()) {
                    r.tex = vc->second;
                    r.view.has_layers = false;                     // the copy is the chosen layer, one slice deep
                }
            }
            // [nvn] debug_null_format = "BPTC_UFLOAT" or "RGBA8:256x64": every sampled texture of that NVN format (and size) gets the dummy view
            // (term isolation, debugging only)
            static const std::string null_fmt = nxrt::config_get_str("nvn", "debug_null_format", "");
            if (r.tex && !null_fmt.empty() && r.tex->fmt) {
                const std::string sized = std::string(r.tex->fmt->name) + ":" + std::to_string(r.tex->b.width) + "x" + std::to_string(r.tex->b.height);
                if (null_fmt == r.tex->fmt->name || null_fmt == sized) r.tex = nullptr;
            }
            // a compute dispatch is never inside the graphics pass: a colour attachment of the last pass is an ordinary texture to it
            const bool feedback = r.tex && d.nvn_stage != kCompute && is_color_feedback(r.tex);
            if (r.tex && tex_gpu(r.tex) && !feedback) {
                VkImageView v = tex_sampled_view(r.tex, make_request(b, r));
                if (v) {
                    ii.imageView = v;
                    ii.imageLayout = tex_sampled_layout(r.tex);
                    if (r.has_sampler) ii.sampler = sampler_for(r.smp);
                    if (r.tex == g_su_tex && g_su_view) {            // the upscaled scene replaces the small one for this draw (S14)
                        ii.imageView = g_su_view;
                        ii.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
                    }
                } else if (first_time_key("dummy-view " + d.shader->hash + " " + b.name + " " + std::to_string(r.tex->id))) {
                    NXLOG_W(kTag, "sampled texture %u (%s, target %d, %dx%dx%d) has no view of the shader's type %d: shader %s binding %s (NVN slot %d, set %u binding %u) gets the dummy view",
                            r.tex->id, r.tex->fmt ? r.tex->fmt->name : "?", r.tex->b.target, r.tex->b.width, r.tex->b.height, r.tex->b.depth,
                            static_cast<int>(view_type_for(b)), d.shader->hash.c_str(), b.name.c_str(), b.nvn.index, d.set, d.binding);
                }
            } else if (feedback) {
                log_once("color-feedback", "a texture is sampled while it is a colour attachment of the pass: the dummy texture is bound");
                static int reported = 0;
                if (reported < 40) {   // which draw: the texture, the attachment's subresource, the sampled view, the shaders (S4g)
                    ++reported;
                    char tb[96];
                    uint32_t lvl = 0, lay = 0, nlay = 0;
                    for (uint32_t i = 0; i < S.ncolor; ++i) if (S.color_tex[i].get() == r.tex) { lvl = S.color_level[i]; lay = S.color_layer[i]; nlay = S.color_layers[i]; }
                    NXLOG_I("nvn.replay", "feedback: %s binding %s vs %s fs %s | attachment level %u layer %u layers %u | sampled view levels %d+%d layers %d+%d", tex_desc(r.tex, tb, sizeof tb),
                            b.name.c_str(), S.stage[kVertex] ? S.stage[kVertex]->hash.c_str() : "-", S.stage[kFragment] ? S.stage[kFragment]->hash.c_str() : "-", lvl, lay, nlay,
                            r.has_view && r.view.has_levels ? r.view.min_level : -1, r.has_view && r.view.has_levels ? r.view.num_levels : -1,
                            r.has_view && r.view.has_layers ? r.view.min_layer : -1, r.has_view && r.view.has_layers ? r.view.num_layers : -1);
                }
            }
            if (kd) {
                kd->stamp = tex_view_stamp(r.tex);   // after the view: tex_gpu above may have made the image
                if (feedback) kbad("colour feedback");
                if (!null_fmt.empty()) kbad("debug_null_format");
            }
            iinfo.push_back(ii);
            w.pImageInfo = &iinfo.back();
            mix(reinterpret_cast<uint64_t>(ii.imageView));
            mix(reinterpret_cast<uint64_t>(ii.sampler));
            mix(static_cast<uint64_t>(ii.imageLayout));
            if (g_bvals_on) g_bvals.push_back({d.binding, reinterpret_cast<uint64_t>(ii.imageView), reinterpret_cast<uint64_t>(ii.sampler), static_cast<uint64_t>(ii.imageLayout)});
        } else if (d.type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) {
            const VkFormat shader_fmt = image_format_vk(b.image.format);
            VkDescriptorImageInfo ii = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL};
            TexResolved r = resolve_image(d);
            if (kd) {
                kd->kind = 4;
                kd->d = &d;
                kd->tex = r.tex;
                kd->tex_id = r.tex ? r.tex->id : 0;
                kd->has_view = r.has_view;
                kd->vh = r.has_view ? bytes_hash(&r.view, sizeof r.view) : 0;
            }
            if (r.tex && tex_gpu(r.tex) && tex_has_storage(r.tex)) {
                ViewRequest q = make_request(b, r);
                q.swizzle = {};
                if (shader_fmt != VK_FORMAT_UNDEFINED) q.format = shader_fmt;
                q.format = tex_storage_format(r.tex, q.format);   // an sRGB image is stored to through its UNORM view (raw bits, as on the console)
                ii.imageView = tex_sampled_view(r.tex, q);
            }
            if (!ii.imageView) {
                if (r.tex)
                    NXLOG_W(kTag, "storage image: texture %u (%s %dx%dx%d, flags 0x%x) has no storage view (gpu %d, storage usage %d)", r.tex->id,
                            r.tex->fmt ? r.tex->fmt->name : "?", r.tex->b.width, r.tex->b.height, r.tex->b.depth, static_cast<unsigned>(r.tex->b.flags),
                            tex_gpu(r.tex) ? 1 : 0, tex_has_storage(r.tex) ? 1 : 0);
                log_once("storage-image-dummy", "a storage image binding has no usable texture: a dummy storage image is bound");
                ii.imageView = dummy_storage_view(view_type_for(b), shader_fmt);
            }
            if (kd) kd->stamp = tex_view_stamp(r.tex);   // after the view
            iinfo.push_back(ii);
            w.pImageInfo = &iinfo.back();
            mix(reinterpret_cast<uint64_t>(ii.imageView));
        } else if (d.type == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER || d.type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) {
            // NVN BUFFER-target textures (samplerBuffer / imageBuffer): a texel buffer view over the pool memory that holds the texels
            const bool storage = d.type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
            const VkFormat shader_fmt = storage ? image_format_vk(b.image.format) : VK_FORMAT_UNDEFINED;
            const int component = storage ? 0 : b.image.component == ScalarType::Float ? 1 : b.image.component == ScalarType::Sint ? 2
                                  : b.image.component == ScalarType::Uint ? 3 : 0;
            TexResolved r = storage ? resolve_image(d) : resolve_texture(d);
            uint64_t addr = 0, bytes = 0;
            VkFormat vfmt = VK_FORMAT_UNDEFINED;
            VkBufferView v = r.tex ? tex_buffer_view(r.tex, r.has_view ? &r.view : nullptr, component, shader_fmt, storage, &addr, &bytes, &vfmt) : VK_NULL_HANDLE;
            // a 2D/3D texture the game left in a samplerBuffer slot (e.g. KTGL's 8x8 default texture 3 in the foliage wind shader's
            // interaction buffers): read as a run of texels over its storage, as the console does, instead of the dummy
            if (v && r.tex->b.target != NVN_TEXTURE_TARGET_BUFFER && first_time_key("texel-nonbuf " + d.shader->hash + " " + b.name + " " + std::to_string(r.tex->id)))
                NXLOG_I(kTag, "texture %u (%s %dx%dx%d, target %d) is read as a texel buffer by %s binding %s: it gets a buffer view over its storage (%llu bytes)",
                        r.tex->id, r.tex->fmt ? r.tex->fmt->name : "?", r.tex->b.width, r.tex->b.height, r.tex->b.depth, r.tex->b.target,
                        d.shader->hash.c_str(), b.name.c_str(), static_cast<unsigned long long>(bytes));
            if (v && g_shadowing) {                                        // before gpu_wrote: the shadow reads the pool copy while valid
                const BufRef host = resolve_buffer(addr, bytes);
                const BufRef sh = shadow_of(addr, bytes, host, storage);
                if (sh.buffer != host.buffer)
                    if (VkBufferView sv = make_buffer_view(sh.buffer, sh.offset, bytes, vfmt)) v = sv;   // offset: non-zero in the pool copy
            }
            if (v && !g_shadowing && out_lazy() && out_overlaps(addr, bytes)) {   // a draw reads a held compute result as texels: from the copy
                const MirrorSpan ms = out_servable(addr, bytes) ? mirror_span(addr, bytes) : MirrorSpan{};
                VkBufferView mv = ms.buf ? make_buffer_view(ms.buf, ms.offset, bytes, vfmt) : VK_NULL_HANDLE;
                if (mv) v = mv;
                else log_once("out-texel", "a draw reads a compute result held in the pool copy as a texel buffer the copy cannot serve: it reads the pool");
            }
            if (kd) {
                if (storage || g_shadowing) kbad("storage texel buffer");
                if (v && !g_shadowing && out_lazy() && out_overlaps(addr, bytes)) kbad("texel buffer over a held result");
                kd->kind = 3;
                kd->texel = true;
                kd->d = &d;
                kd->tex = r.tex;
                kd->tex_id = r.tex ? r.tex->id : 0;
                kd->has_view = r.has_view;
                kd->vh = r.has_view ? bytes_hash(&r.view, sizeof r.view) : 0;
                kd->stamp = tex_view_stamp(r.tex);
                kd->addr = addr;
                kd->range = bytes;
                kd->pool = addr ? pool_lookup(addr).pool : nullptr;
            }
            if (v && storage) { g_gw_site = 6; gpu_wrote(addr, bytes, kWriteStorage); g_gw_site = 0; }   // an image buffer may be written by the shader
            if (!v) {
                if (r.tex && first_time_key("texel-buffer " + d.shader->hash + " " + b.name + " " + std::to_string(r.tex->id)))
                    NXLOG_W(kTag, "texel buffer: texture %u (%s, target %d, %dx%dx%d, storage %llu bytes at %p) has no buffer view: shader %s binding %s (NVN slot %d, set %u binding %u)",
                            r.tex->id, r.tex->fmt ? r.tex->fmt->name : "?", r.tex->b.target, r.tex->b.width, r.tex->b.height, r.tex->b.depth,
                            static_cast<unsigned long long>(r.tex->storage_size), static_cast<void*>(r.tex->mem), d.shader->hash.c_str(), b.name.c_str(),
                            b.nvn.index, d.set, d.binding);
                kbad("texel buffer without a view");
                log_once("texel-buffer-dummy", "a texel buffer binding has no usable buffer texture: a dummy buffer view is bound");
                v = dummy_buffer_view(shader_fmt, storage);
            }
            if (!v) {
                note_unsupported("texel buffer descriptor without any view (the binding stays unwritten)");
                continue;
            }
            tinfo.push_back(v);
            w.pTexelBufferView = &tinfo.back();
            mix(reinterpret_cast<uint64_t>(v));
        } else {
            kbad("descriptor of an unhandled type");
            note_unsupported("descriptor of an unhandled type (the binding stays unwritten)");
            continue;
        }
        writes.push_back(w);
    }
    ++g_sets_built;
    g_last_set_key = key;
    if (g_krec && !g_krec->bad) {   // a set to keep: from the persistent pools, never one of the submission's
        g_krec->content = key;
        VkDescriptorSet ds = keep_alloc(pl.sets[set]);
        if (ds) {
            for (auto& w : writes) w.dstSet = ds;
            vkUpdateDescriptorSets(vkdev(), static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
            g_krec->ds = ds;
            if (set_reuse_mode() == SetReuse::Verify) g_set_content[ds] = key;
            return ds;
        }
        g_krec->bad = "persistent pool full";
    }
    if (VkDescriptorSet* hit = g_set_cache.find(key)) { ++g_sets_cached; return *hit; }
    VkDescriptorSet ds = F->alloc_set(pl.sets[set]);
    if (!ds) return VK_NULL_HANDLE;
    for (auto& w : writes) w.dstSet = ds;
    vkUpdateDescriptorSets(vkdev(), static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    g_set_cache.put(key, ds);
    if (g_bvals_on) g_ds_bvals[ds] = g_bvals;
    return ds;
}

// ---- resolution scale: the support buffer's render_scale for a draw or dispatch (Ryujinx TextureBindingsManager.UpdateRenderScale) ----
// The translator scales texelFetch / imageLoad coordinates by render_scale[1 + i] and divides textureSize and gl_FragCoord.xy by the scale
// of texture i and of the render target ([0]). Texture i is the shader's i-th texture descriptor: the harness numbers single textures
// with one counter per shader (binding = stage slot * 128 + counter, + 64 for texel buffers), images follow the textures. Vertex shaders
// read their scales after the fragment shader's (frag_scale_count). Storage images are never scaled (tex_rescale), so images read 1.
void support_for_draw(const ProgLayout& pl, bool compute) {
    g_draw_support = UploadAlloc();
    if (render_scale() == 1.0f) return;
    float sc[73];
    for (float& v : sc) v = 1.0f;
    int frag_count = 0;
    bool any = false;
    if (!compute && P.scale != 1.0f) { sc[0] = P.scale; any = true; }
    // the fragment shader's textures first, then the vertex shader's after them
    const ShaderInfo* fs = compute ? nullptr : S.stage[kFragment];
    const ShaderInfo* vs = compute ? S.stage[kCompute] : S.stage[kVertex];
    auto tex_count = [](const ShaderInfo* sh) {
        int n = 0;
        for (const DescriptorBinding& b : sh->refl.bindings)
            if (b.count == 1 && (b.type == DescriptorType::CombinedImageSampler || b.type == DescriptorType::SampledImage || b.type == DescriptorType::UniformTexelBuffer ||
                                 b.type == DescriptorType::StorageImage || b.type == DescriptorType::StorageTexelBuffer))
                ++n;
        return n;
    };
    if (fs) frag_count = tex_count(fs);
    // the native size of the render target, for the fragment rule below
    Texture* rt = S.ncolor && S.color_tex[0] ? S.color_tex[0].get() : S.depth_tex.get();
    for (const BindingDesc& d : pl.bindings) {
        if (d.type != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) continue;
        const DescriptorBinding& b = d.shader->refl.bindings[d.index];
        if (b.count != 1 || b.binding >= 640) continue;
        const bool is_fs = d.shader == fs;
        if (!is_fs && d.shader != vs) continue;
        if (!texture_source(b.nvn.source)) continue;
        TexResolved r = resolve_texture(d);
        if (!r.tex) continue;
        const auto fb = g_fb_use.find(r.tex);
        if (fb != g_fb_use.end()) r.tex = fb->second;
        float s = tex_scale(r.tex);
        if (s == 1.0f) continue;
        const int idx = static_cast<int>(b.binding % 64u);
        const int slot = is_fs ? 1 + idx : (compute ? 1 + idx : 1 + frag_count + idx);
        if (slot >= 73) continue;
        if (is_fs && rt) {
            // Ryujinx: a texture whose size is the render target's size times one factor on both axes interpolates the scaled pixels
            // with gl_FragCoord (a negative scale tells TexelFetchScale to)
            const float rw = static_cast<float>(rt->b.width), rh = static_cast<float>(rt->b.height);
            if (r.tex->b.width > 0 && r.tex->b.height > 0 && rw / static_cast<float>(r.tex->b.width) == rh / static_cast<float>(r.tex->b.height)) s = -s;
        }
        sc[slot] = s;
        any = true;
    }
    if (!any) return;
    uint64_t key = 1469598103934665603ull + static_cast<uint64_t>(frag_count);
    for (float v : sc) { uint32_t u; std::memcpy(&u, &v, 4); key = (key ^ u) * 1099511628211ull; }
    auto it = g_support_scaled.find(key);
    if (it != g_support_scaled.end()) { g_draw_support = it->second; return; }
    UploadAlloc u = F->upload(sizeof(SupportBuffer), 256);
    if (!u) return;
    SupportBuffer sb = support_buffer_defaults();
    sb.frag_scale_count[0] = frag_count;
    for (int i = 0; i < 73; ++i) sb.render_scale[i][0] = sc[i];
    std::memcpy(u.ptr, &sb, sizeof sb);
    g_support_scaled[key] = u;
    g_draw_support = u;
}

// ---- descriptor set reuse ([nvn] set_reuse) ---------------------------------------------------------------------------------------
// build_set resolves every binding of a set (pool lookups, held results, mirror state, texture views) before it can look the set up in
// g_set_cache, and OoT binds the same resources again for most draws. Graphics sets that only read get a signature of their raw inputs
// (the GState words their bindings read, the texture handles, the support buffer); with the same signature and nothing changed since the
// signature was taken, the set built then is bound again without any resolve. "Nothing changed" is the resolve epoch: it moves with every
// command other than state binds and draws (copies, dispatches, uniform updates, barriers, ...), every new pass (attachments, layouts,
// feedback), every GPU write the replay notes, texture state changes (g_tex_state_gen) and compute results going back (out_bytes_total).
// Sets with bindings the shader writes, the A17 graphics c[0] table, feedback or 3D copies, the scene upscaler and traces always resolve.
// `verify`: resolve anyway and log a set that differs from the reused one (diagnosis).

// Per program layout: which of its four sets may be reused (bit n = set n), worked out once.
std::unordered_map<const ProgLayout*, uint8_t> g_reuse_masks;   // cleared at replay shutdown (layouts are freed with the pipelines)
uint8_t set_reuse_mask(const ProgLayout& pl) {
    // keyed by the layout and gmem_ro_on() (its paired windows change which storage bindings count as written)
    const bool c0p = c0_persist_on();
    const ProgLayout* mk = reinterpret_cast<const ProgLayout*>(reinterpret_cast<uintptr_t>(&pl) | (gmem_ro_on() ? 1u : 0u) | (c0p ? 2u : 0u));
    auto it = g_reuse_masks.find(mk);
    if (it != g_reuse_masks.end()) return it->second;
    uint8_t ok = 0xf;
    for (const BindingDesc& d : pl.bindings) {
        const DescriptorBinding& b = d.shader->refl.bindings[d.index];
        bool bad = false;
        switch (d.type) {
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER: case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
                if (b.nvn.source == NvnSource::DriverConstants) bad = d.shader->refl.global_fallback && (d.nvn_stage == kCompute || !c0p);   // graphics_c0: per draw unless persistent
                else if (b.nvn.source == NvnSource::StorageBuffer) bad = storage_binding_writes(b, d.shader);
                else bad = b.nvn.source != NvnSource::SupportBuffer && b.nvn.source != NvnSource::ConstantData && b.nvn.source != NvnSource::UniformBlock;
                break;
            case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER: case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
                bad = !texture_source(b.nvn.source);
                break;
            default: bad = true; break;   // storage images and storage texel buffers are written
        }
        if (bad && d.set < 4) ok &= static_cast<uint8_t>(~(1u << d.set));
    }
    g_reuse_masks.emplace(mk, ok);
    return ok;
}

// [nvn] set_keep_diag (diagnosis): variants of the signature with one kind of input left out (bit 0 uniform block addresses, 1 storage
// addresses, 2 texture handles, 3 the pools and targets), so the misses of the kept-set table can be put down to the input that changed.
uint64_t g_sig_var[4] = {};
bool g_sig_var_on = false;
uint64_t g_keep_sig = 0;   // set_signature: the kept-set table's key of the same set
// [nvn] set_keep: the sets that may be kept although the memo may not reuse them (bit n = set n): storage buffers the shader writes and storage
// images (a kept set found valid notes the writes again, keep_valid). Not: the per-draw graphics c[0] of pointer shaders, storage texel buffers.
std::unordered_map<const ProgLayout*, uint8_t> g_keep_masks;
uint8_t set_keep_mask(const ProgLayout& pl) {
    const bool c0p = c0_persist_on();
    const ProgLayout* mk = reinterpret_cast<const ProgLayout*>(reinterpret_cast<uintptr_t>(&pl) | (c0p ? 2u : 0u));
    auto it = g_keep_masks.find(mk);
    if (it != g_keep_masks.end()) return it->second;
    uint8_t ok = 0xf;
    for (const BindingDesc& d : pl.bindings) {
        const DescriptorBinding& b = d.shader->refl.bindings[d.index];
        bool bad = false;
        switch (d.type) {
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER: case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
                if (b.nvn.source == NvnSource::DriverConstants) bad = d.shader->refl.global_fallback && (d.nvn_stage == kCompute || !c0p);
                else bad = b.nvn.source != NvnSource::SupportBuffer && b.nvn.source != NvnSource::ConstantData && b.nvn.source != NvnSource::UniformBlock &&
                           b.nvn.source != NvnSource::StorageBuffer;
                break;
            case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER: case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
                bad = !texture_source(b.nvn.source);
                break;
            case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
                bad = b.nvn.source != NvnSource::ImageBinding && b.nvn.source != NvnSource::HandleInUniform;
                break;
            default: bad = true; break;
        }
        if (bad && d.set < 4) ok &= static_cast<uint8_t>(~(1u << d.set));
    }
    g_keep_masks.emplace(mk, ok);
    return ok;
}

// The signature of set `set` of `pl`: everything its bindings read before resolving.
uint64_t set_signature(const ProgLayout& pl, uint32_t set) {
    if (g_sig_var_on) {
        for (int v = 0; v < 4; ++v) {
            uint64_t k = 0x9e3779b97f4a7c15ull ^ (reinterpret_cast<uint64_t>(pl.layout) * 31 + set) ^ static_cast<uint64_t>(v + 1) * 0x51ed27ull;
            auto mx = [&](uint64_t x) { k = (k ^ x) * 1099511628211ull; };
            mx(reinterpret_cast<uint64_t>(&pl));
            if (v != 3) {
                mx(reinterpret_cast<uint64_t>(S.tpool.get())); mx(reinterpret_cast<uint64_t>(S.spool.get()));
                mx(S.ncolor);
                for (uint32_t i = 0; i < S.ncolor && i < 8; ++i) mx(reinterpret_cast<uint64_t>(S.color_tex[i].get()));
                mx(reinterpret_cast<uint64_t>(S.depth_tex.get()));
            }
            for (const BindingDesc& d : pl.bindings) {
                if (d.set != set) continue;
                const DescriptorBinding& b = d.shader->refl.bindings[d.index];
                mx(d.binding);
                switch (b.nvn.source) {
                    case NvnSource::UniformBlock:
                        if (b.nvn.index >= 0 && b.nvn.index < 16) { const GState::Bind& u = S.ubo[d.nvn_stage][b.nvn.index]; if (v != 0) mx(u.addr); mx(u.size); }
                        break;
                    case NvnSource::StorageBuffer: {
                        const int slot = b.nvn.index >= 0 ? b.nvn.index : storage_slot_for(d.nvn_stage, b.nvn.c0_offset);
                        if (slot >= 0 && slot < 16) { if (v != 1) mx(S.ssbo[d.nvn_stage][slot].addr); mx(S.ssbo[d.nvn_stage][slot].size); }
                        break;
                    }
                    case NvnSource::SamplerBinding: if (v != 2 && b.nvn.index >= 0 && b.nvn.index < 64) mx(S.tex[d.nvn_stage][b.nvn.index]); break;
                    case NvnSource::HandleInUniform: if (v != 2) mx(read_handle_word(d.nvn_stage, b.nvn.bank, b.nvn.word)); break;
                    case NvnSource::HandleExpr: if (v != 2) mx(eval_handle_expr(d, b.nvn.expr)); break;
                    default: break;
                }
            }
            g_sig_var[v] = k;
        }
    }
    uint64_t key = 0x9e3779b97f4a7c15ull ^ (reinterpret_cast<uint64_t>(pl.layout) * 31 + set);
    auto mix = [&](uint64_t v) { key = (key ^ v) * 1099511628211ull; };
    mix(reinterpret_cast<uint64_t>(&pl));   // two programs may share a pipeline layout but read other state
    mix(reinterpret_cast<uint64_t>(g_draw_support.buffer)); mix(g_draw_support.offset);
    mix(reinterpret_cast<uint64_t>(S.tpool.get())); mix(reinterpret_cast<uint64_t>(S.spool.get()));
    if (memo_pass_keep()) {   // a sampled colour attachment of these targets gets the dummy view (is_color_feedback)
        mix(S.ncolor);
        for (uint32_t i = 0; i < S.ncolor && i < 8; ++i) mix(reinterpret_cast<uint64_t>(S.color_tex[i].get()));
        mix(reinterpret_cast<uint64_t>(S.depth_tex.get()));
    }
    // The bindings' raw inputs (bk) apart from the update-range generations (ug): the kept-set table ([nvn] set_keep) is keyed by the
    // program and bk alone (g_keep_sig). Pools, targets and update ranges are checked when a kept set is found (keep_valid).
    uint64_t bk = 0xcbf29ce484222325ull, ug = 0;
    auto mixb = [&](uint64_t v) { bk = (bk ^ v) * 1099511628211ull; };
    for (const BindingDesc& d : pl.bindings) {
        if (d.set != set) continue;
        const DescriptorBinding& b = d.shader->refl.bindings[d.index];
        mixb(d.binding);
        switch (b.nvn.source) {
            case NvnSource::UniformBlock:
                if (b.nvn.index >= 0 && b.nvn.index < 16) {
                    const GState::Bind& u = S.ubo[d.nvn_stage][b.nvn.index];
                    mixb(u.addr); mixb(u.size);
                    if (const UpdRange* ur = u.addr ? upd_find(u.addr, u.size ? u.size : 65536) : nullptr) ug = (ug ^ ur->gen ^ d.binding) * 1099511628211ull + 1;   // a record-time update: a new snapshot
                }
                break;
            case NvnSource::StorageBuffer: {
                const int slot = b.nvn.index >= 0 ? b.nvn.index : storage_slot_for(d.nvn_stage, b.nvn.c0_offset);
                if (slot >= 0 && slot < 16) { mixb(S.ssbo[d.nvn_stage][slot].addr); mixb(S.ssbo[d.nvn_stage][slot].size); }
                break;
            }
            case NvnSource::SamplerBinding:
                if (b.nvn.index >= 0 && b.nvn.index < 64) mixb(S.tex[d.nvn_stage][b.nvn.index]);
                break;
            case NvnSource::ImageBinding:   // storage images (kept sets only: the memo never reuses them)
                if (b.nvn.index >= 0 && b.nvn.index < 16) mixb(S.img[d.nvn_stage][b.nvn.index]);
                break;
            case NvnSource::HandleInUniform: mixb(read_handle_word(d.nvn_stage, b.nvn.bank, b.nvn.word)); break;
            case NvnSource::HandleExpr: mixb(eval_handle_expr(d, b.nvn.expr)); break;
            case NvnSource::DriverConstants:   // a pointer shader's graphics c[0] in its persistent slot: the table's content and the era
                if (d.nvn_stage != kCompute && d.shader->refl.global_fallback && c0_persist_on()) { mixb(graphics_c0_build(!d.shader->refl.global_pt)); mixb(g_c0_era); }
                break;
            default: break;   // support buffer, constant data, driver constants: fixed (or in the signature above)
        }
    }
    mix(bk);
    mix(ug);
    g_keep_sig = ((0x9e3779b97f4a7c15ull ^ (reinterpret_cast<uint64_t>(pl.layout) * 31 + set)) * 1099511628211ull ^ reinterpret_cast<uint64_t>(&pl)) * 1099511628211ull ^ bk;
    return key;
}

SetMap g_set_memo;   // signature -> set, valid for g_set_memo_epoch
uint64_t g_set_memo_epoch = 0;
uint64_t g_sets_reused = 0, g_sets_verify_bad = 0;          // statistics
uint64_t g_memo_clr[3] = {}, g_memo_last[3] = {};            // statistics: memo clears by the resolve epoch, texture state, results sent back

// ---- persistent, validated descriptor sets ([nvn] set_keep) ---------------------------------------------------------------------------
// The set memo above lives for one resolve epoch, and the epoch moves ~120 times per frame in Kokiri (compute storage writes, results sent
// back, texture image events, commands), so ~80% of OoT's read-only sets were resolved again every time, although ~70% of its draws come from
// command lists recorded once and submitted every frame with the same raw inputs. A kept set lives across submissions in persistent pools,
// keyed by the same signature, together with what each of its bindings depended on when it was resolved (KDep). Instead of being dropped
// when anything changes, it is checked when it is found: the pools still those (pool registry generation), each uniform block's copy-or-pool
// decision the same (read_dep_check: the copy's page state, without the pool look-up), no held compute result over its storage and texel
// ranges (only asked when the held results may have grown), each texture the same object with the same image views (tex_view_stamp) and
// the same view and sampler. Sets with anything per submission (upload-ring snapshots, held copies, scaled support buffers, update ranges)
// are not kept. An entry that keeps failing its check is no longer kept (it was rebuilt every frame). Entries unused for kKeepAge presents
// are dropped; when the pools hold kKeepMaxPools pools, everything is dropped and the pools are released once the GPU is done with them
// (a new era). `set_reuse = verify` also resolves every kept set found valid and compares the content keys.
struct KeepEntry {
    VkDescriptorSet ds = VK_NULL_HANDLE;
    const ProgLayout* pl = nullptr;
    uint32_t set = 0;
    uint64_t pool_gen = 0, grow = 0, content = 0;
    uint64_t reg_gen = 0;       // pool registry generation at the last check: a pool registered since may now hold the bound addresses
    uint32_t last_used = 0, last_fail = 0;
    uint16_t fails = 0;
    uint64_t smp_tune = 0;      // anisotropy and LOD bias of the samplers when it was built (sampler_for)
    bool dead = false;          // failed too often, or could not be kept: built per submission until retry_at
    uint32_t retry_at = 0;
    std::vector<KDep> deps;
    std::vector<BVal> bvals;    // set_reuse = verify only
};
constexpr uint32_t kKeepAge = 600;          // presents without a use before an entry is dropped
constexpr size_t kKeepMaxPools = 6;          // 16k sets each
constexpr uint16_t kKeepMaxFails = 6;
std::unordered_map<uint64_t, KeepEntry> g_keep;
std::vector<VkDescriptorPool> g_keep_pools;
bool g_keep_full = false;
uint64_t smp_tune_now() {
    const float bias = nxrt::gfx::tuning().lod_bias.load(std::memory_order_relaxed);
    uint32_t b = 0;
    std::memcpy(&b, &bias, 4);
    return static_cast<uint64_t>(static_cast<uint32_t>(nxrt::gfx::tuning().anisotropy.load(std::memory_order_relaxed))) << 32 ^ b;
}
uint64_t g_keep_sets_era = 0, g_keep_eras = 0, g_keep_sweep_at = 0;
uint64_t g_keep_hit = 0, g_keep_new = 0, g_keep_rebuilt = 0, g_keep_dead_n = 0, g_keep_bad_n = 0, g_keep_verify_bad = 0, g_keep_evicted = 0;
uint64_t g_keep_fail[5] = {};                // pool generation, uniform decision, held result, texture, layout
std::unordered_map<std::string, uint64_t> g_keep_bad_why;   // statistics: why sets were not kept
uint64_t g_var_new = 0, g_var_none = 0, g_var_hit[4] = {};   // set_keep_diag
uint64_t g_keep_first = 0;   // statistics: signatures seen for the first time (not kept yet)
uint64_t g_set_why[4] = {};   // statistics: sets that may not be reused, by the first reason: per-draw c[0] only, written storage only, both, other
void set_bad_why(const ProgLayout& pl, uint32_t set) {
    bool c0 = false, wr = false, other = false;
    for (const BindingDesc& d : pl.bindings) {
        if (d.set != set) continue;
        const DescriptorBinding& b = d.shader->refl.bindings[d.index];
        switch (d.type) {
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER: case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
                if (b.nvn.source == NvnSource::DriverConstants) c0 |= d.shader->refl.global_fallback;
                else if (b.nvn.source == NvnSource::StorageBuffer) wr |= storage_binding_writes(b, d.shader);
                else other |= b.nvn.source != NvnSource::SupportBuffer && b.nvn.source != NvnSource::ConstantData && b.nvn.source != NvnSource::UniformBlock;
                break;
            case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER: case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER: other |= !texture_source(b.nvn.source); break;
            default: other = true; break;
        }
    }
    ++g_set_why[other ? 3 : (c0 && wr) ? 2 : c0 ? 0 : 1];
}
uint64_t g_set_paths[4] = {};   // statistics: sets built outside the memo and the kept sets: compute, reuse off for the draw, a set that may not be reused, other

bool set_keep_on() {
    static const bool on = nxrt::config_get_bool("nvn", "set_keep", true);
    return on && ab_on(kAbSetKeep) && out_lazy() && memo_pass_keep();
}

// [nvn] set_keep_writes (default off): sets with storage buffers the shader writes, or storage images, are kept too (cpuperf_ab_what keep_writes).
// Off: no gain in Kokiri (paired, replay -0.1 ms): most such sets bind per-frame storage addresses and never repeat.
bool keep_writes_on() {
    static const bool on = nxrt::config_get_bool("nvn", "set_keep_writes", false);
    return on && ab_on(kAbKeepWrites);
}

void keep_reset(bool release) {
    g_keep.clear();
    g_set_content.clear();
    if (release) for (VkDescriptorPool p : g_keep_pools) keep_dpool_release(p);
    g_keep_pools.clear();
    g_keep_sets_era = 0;
}

VkDescriptorSet keep_alloc(VkDescriptorSetLayout layout) {
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (g_keep_pools.empty()) {
            const VkDescriptorPool p = keep_dpool_new();
            if (!p) return VK_NULL_HANDLE;
            g_keep_pools.push_back(p);
        }
        VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = g_keep_pools.back();
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &layout;
        VkDescriptorSet s = VK_NULL_HANDLE;
        if (vkAllocateDescriptorSets(vkdev(), &ai, &s) == VK_SUCCESS) { ++g_keep_sets_era; return s; }
        if (g_keep_pools.size() >= kKeepMaxPools) { g_keep_full = true; return VK_NULL_HANDLE; }   // full: the next submission starts a new era (keep_begin_submit)
        const VkDescriptorPool p = keep_dpool_new();
        if (!p) return VK_NULL_HANDLE;
        g_keep_pools.push_back(p);
    }
    return VK_NULL_HANDLE;
}

// Once per submission: a new era when the pools are full, the sweep of old entries every few hundred presents.
void keep_begin_submit() {
    static const bool diag = nxrt::config_get_bool("nvn", "set_keep_diag", false);
    g_sig_var_on = diag;
    const uint64_t now = counters().presents.load(std::memory_order_relaxed);
    if (g_keep_full) {
        g_keep_full = false;
        ++g_keep_eras;
        keep_reset(true);
        NXLOG_I(kTag, "set_keep: persistent descriptor pools full: new era %llu (entries dropped, pools released after the GPU)", static_cast<unsigned long long>(g_keep_eras));
    }
    if (now >= g_keep_sweep_at) {
        g_keep_sweep_at = now + 300;
        for (auto it = g_keep.begin(); it != g_keep.end();) {
            if (now > it->second.last_used + kKeepAge) { it = g_keep.erase(it); ++g_keep_evicted; }
            else ++it;
        }
    }
}

// The check of a kept set (see above). 0: valid; else 1 + the kind of failure (g_keep_fail).
int keep_valid(KeepEntry& e) {
    if (e.pool_gen != pool_unregister_generation()) return 1;
    if (e.smp_tune != smp_tune_now()) return 5;
    if (const uint64_t rg = pool_registry_generation(); rg != e.reg_gen) {   // a pool registered over an address (aliasing pools): resolved elsewhere now
        for (const KDep& k : e.deps) {
            const void* p = k.kind == 1 ? k.rd.pool : (k.kind == 2 || (k.kind == 3 && k.texel)) ? k.pool : nullptr;
            const uint64_t a = k.addr;
            if ((k.kind == 1 || k.kind == 2 || (k.kind == 3 && k.texel)) && a && pool_lookup(a).pool != p) return 1;
        }
        e.reg_gen = rg;
    }
    const uint64_t grow = out_grow_version();
    const bool ask_held = grow != e.grow;
    for (const KDep& k : e.deps) {
        switch (k.kind) {
            case 1:
                if (!g_upd.empty() && upd_find(k.addr, k.range)) return 2;   // an update range now: bound from snapshots
                if (!read_dep_check(k.rd, kReadUniform)) return 2;
                break;
            case 2:
                if (ask_held && out_overlaps_window(k.addr, k.range)) return 3;
                upd_note_read(k.addr, k.note);
                break;
            case 3: {
                const TexResolved r = resolve_texture(*k.d);
                if (r.tex != k.tex || (r.tex && r.tex->id != k.tex_id) || r.has_view != (k.has_view != 0)) return 4;
                if (r.has_view && bytes_hash(&r.view, sizeof r.view) != k.vh) return 4;
                if (!k.texel && (r.has_sampler != (k.has_sampler != 0) || (r.has_sampler && bytes_hash(&r.smp, sizeof r.smp) != k.sh))) return 4;
                if (tex_view_stamp(r.tex) != k.stamp) return 4;
                if (!k.texel && is_color_feedback(r.tex)) return 4;   // an attachment of the targets now (the key leaves the targets out)
                if (k.texel && k.addr && ask_held && out_overlaps(k.addr, k.range)) return 3;
                break;
            }
            case 4: {
                const TexResolved r = resolve_image(*k.d);
                if (r.tex != k.tex || (r.tex && r.tex->id != k.tex_id) || r.has_view != (k.has_view != 0)) return 4;
                if (r.has_view && bytes_hash(&r.view, sizeof r.view) != k.vh) return 4;
                if (tex_view_stamp(r.tex) != k.stamp) return 4;
                break;
            }
            default: break;
        }
    }
    for (const KDep& k : e.deps)   // what build_set does for a storage binding the shader may write: the GPU writes the range
        if (k.kind == 2 && k.writes) { g_gw_site = 4; gpu_wrote(k.addr, k.range, kWriteStorage); g_gw_site = 0; }
    e.grow = grow;
    return 0;
}

// A kept set for set `set` of `pl` with signature `sig`, checked; or one built now (kept when it can be). Null: build it the usual way.
VkDescriptorSet keep_lookup_or_build(const ProgLayout& pl, uint32_t set, uint64_t sig, SetReuse mode) {
    const uint32_t now = static_cast<uint32_t>(counters().presents.load(std::memory_order_relaxed));
    auto it = g_keep.find(sig);
    if (g_sig_var_on) {   // set_keep_diag: which left-out input would have found an entry made in an earlier present
        static std::unordered_map<uint64_t, uint32_t> seen[4];
        static uint32_t cleared = 0;
        if (now - cleared > 600) { for (auto& m : seen) m.clear(); cleared = now; }
        if (it == g_keep.end()) {
            ++g_var_new;
            bool any = false;
            for (int v = 0; v < 4; ++v) {
                auto f = seen[v].find(g_sig_var[v]);
                if (f != seen[v].end() && f->second < now) { ++g_var_hit[v]; any = true; }
            }
            if (!any) ++g_var_none;
        }
        for (int v = 0; v < 4; ++v) seen[v][g_sig_var[v]] = now;
    }
    if (it != g_keep.end() && (it->second.pl != &pl || it->second.set != set)) it = g_keep.end();   // a signature collision: not this set
    if (it != g_keep.end()) {
        KeepEntry& e = it->second;
        if (e.dead && now < e.retry_at) { e.last_used = now; return VK_NULL_HANDLE; }
        if (e.dead) { g_keep.erase(it); it = g_keep.end(); }   // time to try again
    }
    if (it != g_keep.end()) {
        KeepEntry& e = it->second;
        const int why = keep_valid(e);
        if (why == 0) {
            e.last_used = now;
            ++g_keep_hit;
            if (mode == SetReuse::Verify) {
                const VkDescriptorSet fresh = build_set(pl, set);
                if (fresh && g_last_set_key != e.content) {
                    ++g_keep_verify_bad;
                    if (g_keep_verify_bad < 20 || (g_keep_verify_bad & 1023) == 0) {
                        NXLOG_W(kTag, "set_keep verify: set %u of vs %s fs %s resolves differently from the kept one (%llu so far)", set,
                                S.stage[kVertex] ? S.stage[kVertex]->hash.c_str() : "-", S.stage[kFragment] ? S.stage[kFragment]->hash.c_str() : "-",
                                static_cast<unsigned long long>(g_keep_verify_bad));
                        for (size_t i = 0; i < g_bvals.size() && i < e.bvals.size(); ++i) {
                            const BVal& x = e.bvals[i];
                            const BVal& y = g_bvals[i];
                            if (x.a == y.a && x.b == y.b && x.c == y.c) continue;
                            const KDep* k = i < e.deps.size() ? &e.deps[i] : nullptr;
                            NXLOG_W(kTag, "  binding %llu: kept %llx %llx %llx, now %llx %llx %llx; dep kind %d state %d addr %llx range %llx tex %u", (unsigned long long)x.binding,
                                    (unsigned long long)x.a, (unsigned long long)x.b, (unsigned long long)x.c, (unsigned long long)y.a, (unsigned long long)y.b, (unsigned long long)y.c,
                                    k ? k->kind : -1, k ? k->rd.state : -1, (unsigned long long)(k ? k->addr : 0), (unsigned long long)(k ? k->range : 0), k ? k->tex_id : 0u);
                        }
                    }
                    return fresh;
                }
            }
            return e.ds;
        }
        ++g_keep_fail[(why - 1) % 5];
        if (now - e.last_fail > 300) e.fails = 0;   // failures far apart (an area change, a new pool): not a set that fails every frame
        e.last_fail = now;
        if (++e.fails > kKeepMaxFails) { e.dead = true; e.retry_at = now + 600; e.deps.clear(); e.deps.shrink_to_fit(); ++g_keep_dead_n; e.last_used = now; return VK_NULL_HANDLE; }
    }
    if (it == g_keep.end()) {   // kept from its second sighting in a later present on: a set made for one frame only is never kept
        static FlatMap64<uint32_t> seen;
        static uint32_t cleared = 0;
        if (now - cleared > 600) { seen.clear(); cleared = now; }
        uint32_t* s = seen.find(sig);
        if (!s || *s == now) {
            if (!s) seen.put(sig, now);
            ++g_keep_first;
            return VK_NULL_HANDLE;
        }
    }
    KeepRec rec;
    g_krec = &rec;
    VkDescriptorSet ds = build_set(pl, set);
    g_krec = nullptr;
    if (rec.bad || !rec.ds) {
        ++g_keep_bad_n;
        if (rec.bad) ++g_keep_bad_why[rec.bad];
        if (rec.bad && !g_keep_full) {   // not recorded again: this signature is built per submission while it is in use
            KeepEntry& d = it != g_keep.end() ? it->second : g_keep[sig];
            d.pl = &pl;
            d.set = set;
            d.dead = true;
            d.retry_at = now + 120;
            d.deps.clear();
            d.last_used = now;
        }
        return ds;
    }
    KeepEntry& e = it != g_keep.end() ? it->second : g_keep[sig];
    if (it != g_keep.end()) ++g_keep_rebuilt; else ++g_keep_new;
    e.ds = rec.ds;
    e.pl = &pl;
    e.set = set;
    e.pool_gen = pool_unregister_generation();
    e.smp_tune = smp_tune_now();
    e.reg_gen = pool_registry_generation();
    e.grow = out_grow_version();
    e.content = rec.content;
    e.last_used = now;
    e.deps = std::move(rec.deps);
    if (mode == SetReuse::Verify) e.bvals = g_bvals;
    return ds;
}

// [nvn] bind_skip (default on): a graphics descriptor set already bound at its index with the same pipeline layout, in the same command buffer and
// pass, is not bound again (the persistent pool-array sets were bound for every draw). Cleared when a pass begins.
VkCommandBuffer g_bs_cb = VK_NULL_HANDLE;
VkPipelineLayout g_bs_layout = VK_NULL_HANDLE;
VkDescriptorSet g_bs_ds[16] = {};
uint64_t g_bs_skipped = 0, g_bs_bound = 0;   // statistics
void bind_skip_reset() { g_bs_cb = VK_NULL_HANDLE; }
void bind_graphics_set(const ProgLayout& pl, VkPipelineBindPoint bp, uint32_t set, VkDescriptorSet ds) {
    static const bool on = nxrt::config_get_bool("nvn", "bind_skip", true);
    if (bp == VK_PIPELINE_BIND_POINT_GRAPHICS && on && ab_on(kAbBindSkip) && set < 16) {
        if (g_bs_cb != F->cb || g_bs_layout != pl.layout) {
            g_bs_cb = F->cb;
            g_bs_layout = pl.layout;
            std::memset(g_bs_ds, 0, sizeof g_bs_ds);
        }
        if (g_bs_ds[set] == ds) { ++g_bs_skipped; return; }
        g_bs_ds[set] = ds;
    }
    ++g_bs_bound;
    vkCmdBindDescriptorSets(F->cb, bp, pl.layout, set, 1, &ds, 0, nullptr);
}

void bind_sets(const ProgLayout& pl, VkPipelineBindPoint bp) {
    g_c0_cached = -1;   // the state may have changed since the last draw's table
    const SetReuse mode = bp == VK_PIPELINE_BIND_POINT_GRAPHICS && ab_on(kAbSetReuse) ? set_reuse_mode() : SetReuse::Off;
    uint8_t reusable = 0;
    const bool reusable_ok = mode != SetReuse::Off && g_fb_use.empty() && g_vol3d_use.empty() && !g_su_view && !g_trace &&
                             !nxrt::gfx::tuning().ufs_hash.load(std::memory_order_relaxed) && !extras::temporal_wanted();
    if (reusable_ok) {
        reusable = set_reuse_mask(pl);
        const uint64_t tg = memo_pass_keep() ? g_tex_image_gen.load(std::memory_order_relaxed) ^ 0x5bd1e995ull : g_tex_state_gen.load(std::memory_order_relaxed), ob = out_bytes_total();
        // the pool registry: a pool registered over bound addresses (aliasing pools) resolves them elsewhere (set_reuse = verify found such stale hits)
        // and the CPU's write faults on tracked pages: a page of a pool copy the CPU wrote is no longer served by the copy, so a uniform block
        // there resolves to the pool now (set_reuse = verify found memo hits still bound to the copy after such a write)
        const uint64_t epoch = ((g_desc_epoch * 0x100000001b3ull ^ tg * 0x9e3779b97f4a7c15ull ^ ob ^ out_version() * 0xc2b2ae3d27d4eb4full) * 31 + pool_registry_generation()) * 31 +
                               mem::ww::faults();
        if (epoch != g_set_memo_epoch) {
            if (!g_set_memo.empty()) {   // statistics: what cleared a memo that held sets
                if (g_desc_epoch != g_memo_last[0]) ++g_memo_clr[0];
                else if (tg != g_memo_last[1]) ++g_memo_clr[1];
                else ++g_memo_clr[2];
            }
            g_memo_last[0] = g_desc_epoch; g_memo_last[1] = tg; g_memo_last[2] = ob;
            g_set_memo.clear();
            g_set_memo_epoch = epoch;
        }
    }
    // sets the memo may not reuse but set_keep may keep (storage writes): the same conditions as the memo's, graphics only
    const uint8_t keep_only = (mode != SetReuse::Off && reusable_ok && set_keep_on() && keep_writes_on()) ? static_cast<uint8_t>(set_keep_mask(pl) & ~reusable) : 0;
    for (uint32_t set = 0; set < 4; ++set) {
        if (!pl.has_set[set]) continue;
        VkDescriptorSet ds = VK_NULL_HANDLE;
        uint64_t sig = 0;
        if (reusable & (1u << set)) {
            sig = set_signature(pl, set);
            if (VkDescriptorSet* hit = g_set_memo.find(sig)) {
                ds = *hit;
                ++g_sets_reused;
                if (mode == SetReuse::Verify) {
                    const VkDescriptorSet fresh = build_set(pl, set);
                    const auto kc = g_set_content.find(ds);   // a kept set is another handle with the same content: compare the content keys
                    if (kc != g_set_content.end() ? (fresh && kc->second != g_last_set_key) : fresh != ds) {
                        ++g_sets_verify_bad;
                        if (g_sets_verify_bad < 20 || (g_sets_verify_bad & 1023) == 0)
                            NXLOG_W(kTag, "set_reuse verify: set %u of vs %s fs %s differs from the reused one (%llu so far)", set,
                                    S.stage[kVertex] ? S.stage[kVertex]->hash.c_str() : "-", S.stage[kFragment] ? S.stage[kFragment]->hash.c_str() : "-",
                                    static_cast<unsigned long long>(g_sets_verify_bad));
                        if (g_sets_verify_bad < 20) {   // which binding: what the reused set was built with against what resolves now
                            const auto ob = g_ds_bvals.find(ds);
                            if (ob != g_ds_bvals.end())
                                for (size_t i = 0; i < g_bvals.size() && i < ob->second.size(); ++i) {
                                    const BVal& x = ob->second[i];
                                    const BVal& y = g_bvals[i];
                                    if (x.a == y.a && x.b == y.b && x.c == y.c) continue;
                                    NXLOG_W(kTag, "  binding %llu: reused %llx %llx %llx, now %llx %llx %llx", (unsigned long long)x.binding, (unsigned long long)x.a,
                                            (unsigned long long)x.b, (unsigned long long)x.c, (unsigned long long)y.a, (unsigned long long)y.b, (unsigned long long)y.c);
                                }
                        }
                        ds = fresh;
                    }
                }
            }
        }
        if (!ds && (reusable & (1u << set)) && set_keep_on()) {
            // not put in the memo: a kept set is checked at every use (its checks see changes the memo's epoch does not: with kept sets in the
            // memo, set_reuse = verify found a burst of stale memo hits right after the area load in Kokiri)
            ds = keep_lookup_or_build(pl, set, g_keep_sig, mode);
        } else if (!ds && keep_only & (1u << set)) {   // writes storage: kept and checked, never in the memo (its writes are noted per draw)
            set_signature(pl, set);
            ds = keep_lookup_or_build(pl, set, g_keep_sig ^ 0x6b6565702d77ull, mode);
        }
        if (!ds) {
            if (bp != VK_PIPELINE_BIND_POINT_GRAPHICS) ++g_set_paths[0];
            else if (mode == SetReuse::Off || !reusable) ++g_set_paths[1];
            else if (!(reusable & (1u << set)) && !(keep_only & (1u << set))) {
                ++g_set_paths[2];
                set_bad_why(pl, set);
            }
            else ++g_set_paths[3];
            ds = build_set(pl, set);
            if (ds && (reusable & (1u << set))) g_set_memo.put(sig, ds);
        }
        if (ds) bind_graphics_set(pl, bp, set, ds);
    }
    for (const PoolBinding& pb : pl.pool) {   // A7: the pool arrays' persistent sets
        PoolSet* st = pool_set_sync(pb);
        if (st && st->set) bind_graphics_set(pl, bp, pb.set, st->set);
    }
}

// ---- draws -------------------------------------------------------------------------------------------------------------------
const ShaderInfo* tcs_now();   // the program's control shader, or the generated pass-through one (below)
// `mesh`: a mesh draw's key: task and mesh stages instead of vertex (and geometry / tessellation), no vertex input.
bool build_key(GraphicsKey& key, VkPrimitiveTopology topo, bool mesh = false) {
    std::memset(&key, 0, sizeof key);
    key.stage[0] = mesh ? nullptr : S.stage[kVertex];
    key.stage[1] = S.stage[kFragment];
    key.stage[2] = mesh ? nullptr : S.stage[kGeometry];
    if (mesh) { key.mesh[0] = S.stage[kTask]; key.mesh[1] = S.stage[kMesh]; }
    if (topo == VK_PRIMITIVE_TOPOLOGY_PATCH_LIST) {      // begin_draw checked: both tessellation stages, patches
        key.tess[0] = tcs_now();
        key.tess[1] = S.stage[kTessEval];
        key.patch_control_points = static_cast<uint8_t>(std::clamp<uint32_t>(S.patch_size, 1, 32));
    }
    const ShaderInfo* vs = key.stage[0];
    // attributes: those the vertex shader reads. An input array takes one location per element. A location the game did not set up
    // (no attribute, or a disabled one) still has to exist in the pipeline; it reads (0, 0, 0, 1) from a dummy binding.
    struct Missing { uint32_t location; ScalarType type; };
    Missing missing[32];
    uint32_t nmissing = 0;
    static const std::vector<ShaderIo> kNoInputs;
    for (const ShaderIo& in : vs ? vs->refl.inputs : kNoInputs) {
        const uint32_t elems = in.array ? in.array : 1;
        for (uint32_t e = 0; e < elems; ++e) {
            const uint32_t loc = in.location + e;
            if (loc >= 32) break;
            VkFormat f = VK_FORMAT_UNDEFINED;
            const VertexAttribInfo* a = nullptr;
            if (loc < S.attr_count && loc < 16) {
                a = &S.attr[loc];
                f = vertex_format(a->format);
            }
            if (f == VK_FORMAT_UNDEFINED) {
                if (nmissing < 32) missing[nmissing++] = {loc, in.type};
                continue;
            }
            // a float input fed by an integer attribute gets the raw bits: the integer format stays and the pipeline patches the shader
            // (spirv_int_attributes); stream-out draws use the shader's stream-out variant, which is not patched: scaled floats there
            if (in.type == ScalarType::Float && is_int_format(f) && g_xfb_active) f = scaled_for(f);
            else if ((in.type == ScalarType::Uint || in.type == ScalarType::Sint) && !is_int_format(f))
                log_once("attr-type-mismatch", "a vertex attribute with a float format feeds an integer shader input (needs a SPIR-V variant)");
            if (key.attr_count >= 16) break;
            KeyAttr& k = key.attr[key.attr_count++];
            k.location = static_cast<uint8_t>(loc);
            k.binding = static_cast<uint8_t>(a->stream & 15);
            k.offset = static_cast<uint16_t>(a->relative_offset);
            k.format = f;
            key.binding_instance[k.binding] = S.stream[k.binding].divisor > 0 ? 1 : 0;
        }
    }
    if (nmissing) {
        uint32_t used = 0;
        for (uint32_t i = 0; i < key.attr_count; ++i) used |= 1u << key.attr[i].binding;
        uint32_t free_binding = 0;
        while (free_binding < 16 && (used >> free_binding & 1)) ++free_binding;
        if (free_binding < 16) {
            key.dummy_binding = static_cast<uint8_t>(free_binding + 1);
            key.binding_instance[free_binding] = 0;
            for (uint32_t i = 0; i < nmissing && key.attr_count < 16; ++i) {
                KeyAttr& k = key.attr[key.attr_count++];
                k.location = static_cast<uint8_t>(missing[i].location);
                k.binding = static_cast<uint8_t>(free_binding);
                const bool is_float = missing[i].type != ScalarType::Uint && missing[i].type != ScalarType::Sint;
                k.offset = is_float ? 0 : 16;
                k.format = is_float ? VK_FORMAT_R32G32B32A32_SFLOAT : (missing[i].type == ScalarType::Uint ? VK_FORMAT_R32G32B32A32_UINT : VK_FORMAT_R32G32B32A32_SINT);
            }
        }
    }
    key.color_count = P.ncolor;
    if (!P.ncolor && S.stage[kFragment] && !S.stage[kFragment]->refl.outputs.empty()) key.no_out = 1;   // a depth-only pass: no outputs
    if (!key.no_out && device().features().dynamic_rendering_unused_attachments) {
        // the fragment shader may write outputs the pass has no attachment for: they are listed as unused (format UNDEFINED)
        if (const ShaderInfo* fs = S.stage[kFragment])
            for (const ShaderIo& o : fs->refl.outputs)
                for (uint32_t e = 0; e < (o.array ? o.array : 1); ++e)
                    if (o.location + e < 8 && o.location + e >= key.color_count) key.color_count = o.location + e + 1;
    }
    for (uint32_t i = 0; i < key.color_count && i < 8; ++i) {
        if (i >= P.ncolor) {
            key.color_format[i] = VK_FORMAT_UNDEFINED;
            key.write_mask[i] = 0;
            continue;
        }
        key.color_format[i] = P.color_fmt[i];
        key.blend_enable[i] = (S.color.blend_enable_mask >> i) & 1;
        const BlendInfo& b = S.blend[i];
        key.blend_src_rgb[i] = b.src_rgb ? vk_blend_factor(b.src_rgb) : VK_BLEND_FACTOR_ONE;
        key.blend_dst_rgb[i] = b.dst_rgb ? vk_blend_factor(b.dst_rgb) : VK_BLEND_FACTOR_ZERO;
        key.blend_src_a[i] = b.src_a ? vk_blend_factor(b.src_a) : VK_BLEND_FACTOR_ONE;
        key.blend_dst_a[i] = b.dst_a ? vk_blend_factor(b.dst_a) : VK_BLEND_FACTOR_ZERO;
        key.blend_op_rgb[i] = b.eq_rgb ? vk_blend_op(b.eq_rgb) : VK_BLEND_OP_ADD;
        key.blend_op_a[i] = b.eq_a ? vk_blend_op(b.eq_a) : VK_BLEND_OP_ADD;
        key.write_mask[i] = S.chmask.mask[i] & 0xf;
        if (P.color_fmt[i] == VK_FORMAT_UNDEFINED) key.write_mask[i] = 0;
        auto uses_src1 = [](VkBlendFactor f) {
            return f == VK_BLEND_FACTOR_SRC1_COLOR || f == VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR || f == VK_BLEND_FACTOR_SRC1_ALPHA ||
                   f == VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
        };
        if (key.blend_enable[i] && (uses_src1(static_cast<VkBlendFactor>(key.blend_src_rgb[i])) || uses_src1(static_cast<VkBlendFactor>(key.blend_dst_rgb[i])) ||
                                    uses_src1(static_cast<VkBlendFactor>(key.blend_src_a[i])) || uses_src1(static_cast<VkBlendFactor>(key.blend_dst_a[i]))))
            key.dual_src = 1;
    }
    key.xfb = !mesh && g_xfb_active && device().features().transform_feedback && S.stage[kVertex] && xfb_stride(S.stage[kVertex]) ? 1 : 0;
    key.depth_format = P.depth_fmt;
    key.stencil_format = P.has_stencil ? P.depth_fmt : VK_FORMAT_UNDEFINED;
    key.samples = P.samples;
    key.topology = static_cast<uint8_t>(topo);
    key.polygon_mode = static_cast<uint8_t>(vk_polygon_mode(S.poly.polygon_mode));
    if (key.polygon_mode != VK_POLYGON_MODE_FILL && !device().features().fill_mode_non_solid) key.polygon_mode = VK_POLYGON_MODE_FILL;
    key.depth_clamp = S.depth_clamp && device().features().depth_clamp;
    key.alpha_to_coverage = S.ms.alpha_to_coverage && S.ms.enable ? 1 : 0;
    key.depth_neg_one = S.depth_mode == NVN_DEPTH_MODE_NEAR_IS_MINUS_W ? 1 : 0;
    if (S.color.logic_op != 0 && S.color.logic_op != NVN_LOGIC_OP_COPY && device().features().logic_op) {
        key.logic_op_enable = 1;
        key.logic_op = static_cast<uint8_t>(vk_logic_op(S.color.logic_op));
        for (uint32_t i = 0; i < 8; ++i) key.blend_enable[i] = 0;
    }
    if (S.color.alpha_test != 0 && S.color.alpha_test != NVN_ALPHA_FUNC_ALWAYS) log_once("alpha-test", "alpha test is enabled by the game: not implemented (needs a shader variant)");
    return true;
}

// The NVN2 smaller features that are pipeline dynamic state (nvn_small.h): the conservative raster mode with its overestimation size, and the
// fragment shading rate. Every pipeline lists them when the device has them (small_dynamic_*), so they are set at the start of every pass and
// whenever one of the commands changed them (kDynRaster). Conservative raster is always overestimate: mode 2 (one more hardware control bit
// the sdk sets, 0x60A030) has no Vulkan counterpart that is known to match. The shading rate is the palette's single rate or 1x1 (shading_rate_for_draw).
void set_small_dynamic(VkCommandBuffer cb) {
    const auto& lim = device().limits_ext();
    if (small_dynamic_conservative()) {
        const bool on = small_conservative_on(S.small);
        vkCmdSetConservativeRasterizationModeEXT(cb, on ? VK_CONSERVATIVE_RASTERIZATION_MODE_OVERESTIMATE_EXT : VK_CONSERVATIVE_RASTERIZATION_MODE_DISABLED_EXT);
        vkCmdSetExtraPrimitiveOverestimationSizeEXT(cb, on ? small_conservative_extra(S.small, lim.cons_max_extra, lim.cons_granularity) : 0.0f);
    }
    if (small_dynamic_fsr()) {
        uint32_t w = 1, h = 1;
        bool exact = true;
        shading_rate_for_draw(S.small, P.samples > 1 ? 0u : lim.fsr_sizes, w, h, &exact);
        if (!exact) log_once("vrs-image", "shading rate image bound with a palette of several rates: the image is not converted, draws run at full rate");
        const VkExtent2D size = {w, h};
        const VkFragmentShadingRateCombinerOpKHR ops[2] = {VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR, VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR};
        vkCmdSetFragmentShadingRateKHR(cb, &size, ops);
    }
}

void set_dynamic_state(uint32_t dirty, VkPrimitiveTopology topo, bool indexed, int index_type) {
    VkCommandBuffer cb = F->cb;
    if (dirty & kDynVp) set_viewports_and_scissors();
    if (dirty & kDynRaster) {
        vkCmdSetCullMode(cb, vk_cull_mode(S.poly.cull_face));
        VkFrontFace ff = vk_front_face(S.poly.front_face);
        if (S.window_origin == NVN_WINDOW_ORIGIN_MODE_LOWER_LEFT) ff = ff == VK_FRONT_FACE_CLOCKWISE ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;   // unflipped viewport
        vkCmdSetFrontFace(cb, ff);
        vkCmdSetRasterizerDiscardEnable(cb, S.rasterizer_discard);
        vkCmdSetLineWidth(cb, device().features().wide_lines ? std::max(1.0f, S.line_width) : 1.0f);
        set_small_dynamic(cb);
    }
    if (topo != g_topo) {
        vkCmdSetPrimitiveTopology(cb, topo);
        g_topo = topo;
    }
    const bool restart = S.prim_restart && indexed &&
                         ((index_type == NVN_INDEX_TYPE_UNSIGNED_INT && S.restart_index == 0xffffffffu) || (index_type == NVN_INDEX_TYPE_UNSIGNED_SHORT && S.restart_index == 0xffffu) ||
                          (index_type == NVN_INDEX_TYPE_UNSIGNED_BYTE && S.restart_index == 0xffu));
    if (!g_restart_valid || restart != g_restart || (dirty & kDynRaster)) {
        vkCmdSetPrimitiveRestartEnable(cb, restart);
        g_restart = restart;
        g_restart_valid = true;
    }
    const bool have_depth = P.depth_fmt != VK_FORMAT_UNDEFINED;
    if (dirty & kDynDepth) {
        vkCmdSetDepthTestEnable(cb, have_depth && S.ds.depth_test);
        vkCmdSetDepthWriteEnable(cb, have_depth && !P.depth_ro && S.ds.depth_test && S.ds.depth_write);
        int func = S.ds.depth_func ? S.ds.depth_func : NVN_DEPTH_FUNC_LESS;
        // [nvn] debug_equal_as = 4 (LEQUAL) / 8 (ALWAYS): diagnostic, draws that test EQUAL against a depth prepass use this NVN function
        // instead, which shows colour-pass geometry the prepass does not match (A15: broken vertices, black hulls)
        static const int equal_as = static_cast<int>(nxrt::config_get_int("nvn", "debug_equal_as", 0));
        if (equal_as && func == NVN_DEPTH_FUNC_EQUAL) func = equal_as;
        vkCmdSetDepthCompareOp(cb, vk_compare_op(func));
        vkCmdSetDepthBoundsTestEnable(cb, have_depth && S.depth_bounds && device().features().depth_bounds);
        if (S.depth_bounds && device().features().depth_bounds) vkCmdSetDepthBounds(cb, S.db_near, S.db_far);
    }
    if (dirty & kDynStencil) {
        vkCmdSetStencilTestEnable(cb, P.has_stencil && S.ds.stencil_test);
        for (int f = 0; f < 2; ++f) {
            const VkStencilFaceFlags face = f == 0 ? VK_STENCIL_FACE_FRONT_BIT : VK_STENCIL_FACE_BACK_BIT;
            vkCmdSetStencilOp(cb, face, vk_stencil_op(S.ds.stencil_fail[f]), vk_stencil_op(S.ds.depth_pass[f]), vk_stencil_op(S.ds.depth_fail[f]),
                              vk_compare_op(S.ds.stencil_func[f] ? S.ds.stencil_func[f] : NVN_STENCIL_FUNC_ALWAYS));
            vkCmdSetStencilReference(cb, face, S.stencil_ref[f]);
            vkCmdSetStencilCompareMask(cb, face, S.stencil_vmask[f]);
            vkCmdSetStencilWriteMask(cb, face, P.depth_ro ? 0u : S.stencil_wmask[f]);
        }
    }
    if (dirty & kDynBias) {
        const int off = S.poly.offset_enables;
        const int mode = S.poly.polygon_mode;
        const bool bias = (mode == NVN_POLYGON_MODE_FILL && (off & NVN_POLYGON_OFFSET_ENABLE_FILL)) ||
                          (mode == NVN_POLYGON_MODE_LINE && (off & NVN_POLYGON_OFFSET_ENABLE_LINE)) ||
                          (mode == NVN_POLYGON_MODE_POINT && (off & NVN_POLYGON_OFFSET_ENABLE_POINT));
        vkCmdSetDepthBiasEnable(cb, bias);
        vkCmdSetDepthBias(cb, S.bias_units, device().features().depth_bias_clamp ? S.bias_clamp : 0.0f, S.bias_factor);
    }
    if (dirty & kDynBlend) vkCmdSetBlendConstants(cb, S.blend_color);
}

// Prepares everything a draw needs and binds it. False: skip the draw.
// [nvn] trace_min_sec = 250: trace_after_fs only starts that many seconds after the first draw (the scene to look at comes late).
bool trigger_time_ok() {
    static const auto start = std::chrono::steady_clock::now();
    static const double min_sec = static_cast<double>(nxrt::config_get_int("nvn", "trace_min_sec", 0));
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() >= min_sec;
}

// Scene upscaler (S14, src/gfx/extras/hdr_nvn.h): before a tone-map draw whose HDR scene input is smaller than its target, FSR 1 enlarges the scene
// to the target's size and the draw samples that instead. The override lives until the next draw.
// Temporal upscaling (FSR 3 / DLSS / DLAA, src/gfx/extras/temporal.h): a G-buffer draw (4+ colour targets and a depth target) offers its vertex
// uniform slot 0 as the camera block; the first valid one of the frame is kept with its depth target.
void temporal_gbuffer_note() {
    if (S.ncolor < 4 || !S.depth_tex || !S.stage[kVertex] || extras::temporal_camera_ready()) return;
    const GState::Bind& u = S.ubo[kVertex][0];
    if (!u.addr || u.size < 272) return;
    uint32_t w[68];
    std::memcpy(w, host_ptr(u.addr), sizeof w);
    tsr_jitter(w, 68);                                                    // the camera as the draw sees it (the bound copy is jittered the same way)
    extras::temporal_note_gbuffer(w, 68);
    if (extras::temporal_camera_ready()) g_tsr_depth = S.depth_tex;
}

// The tone-map draw with a temporal mode: the largest 2D colour input is the HDR scene; the SDK's result (target size) replaces it for this draw.
void temporal_for_draw(const ProgLayout& pl) {
    ++g_tsr_frame;                                                        // one tone-map draw per frame: the next frame's jitter phase
    Texture* target = S.color_tex[0].get();
    Texture* depth = g_tsr_depth.get();
    static bool told_intercept = false, told_input = false, told_depth = false;
    if (!told_intercept) {
        told_intercept = true;
        NXLOG_I(kTag, "temporal: tone-map draw %s intercepted (target %ux%u)", S.stage[kFragment]->hash.c_str(), target ? target->b.width : 0,
                target ? target->b.height : 0);
    }
    if (!extras::temporal_camera_ready()) {                              // counts the frames without a camera (warns after 120)
        extras::temporal_evaluate(nullptr, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED, {}, {}, false);
        return;
    }
    if (!tex_gpu(target) || !depth || !tex_gpu(depth)) {
        if (!told_depth) {
            told_depth = true;
            NXLOG_W(kTag, "temporal: camera found but no usable target / G-buffer depth texture at the tone-map draw");
        }
        return;
    }
    const BindingDesc* best = nullptr;
    TexResolved best_r{};
    uint64_t best_area = 0;
    for (const BindingDesc& d : pl.bindings) {
        if (d.type != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) continue;
        const DescriptorBinding& b = d.shader->refl.bindings[d.index];
        if (!texture_source(b.nvn.source)) continue;
        if (view_type_for(b) != VK_IMAGE_VIEW_TYPE_2D) continue;
        TexResolved r = resolve_texture(d);
        if (!r.tex || !tex_gpu(r.tex) || tex_is_depth(r.tex) || shares_image(r.tex, depth)) continue;
        const VkExtent3D e = tex_image_extent(r.tex, 0);
        const uint64_t area = static_cast<uint64_t>(e.width) * e.height;
        if (area > best_area) { best_area = area; best = &d; best_r = r; }
    }
    if (!best) {
        if (!told_input) {
            told_input = true;
            NXLOG_W(kTag, "temporal: the tone-map draw samples no 2D colour texture to use as the scene");
        }
        return;
    }
    const DescriptorBinding& b = best->shader->refl.bindings[best->index];
    const VkExtent3D ce = tex_image_extent(best_r.tex, 0), de = tex_image_extent(depth, 0), te = tex_image_extent(target, 0);
    const VkExtent2D render{std::min(ce.width, de.width), std::min(ce.height, de.height)};
    end_pass();                                                           // compute work cannot run inside a render pass
    tex_ensure_current(best_r.tex, *F);
    tex_transition(best_r.tex, *F, tex_sampled_layout(best_r.tex));
    tex_transition(depth, *F, tex_sampled_layout(depth));
    VkImageView src = tex_sampled_view(best_r.tex, make_request(b, best_r));
    ViewRequest dr;
    dr.depth_aspect = true;
    VkImageView dv = tex_sampled_view(depth, dr);
    if (!src || !dv) return;
    VkImageView out = extras::temporal_evaluate(&device(), F->cb, src, dv, tex_sampled_layout(depth), render, VkExtent2D{te.width, te.height},
                                                S.depth_mode == NVN_DEPTH_MODE_NEAR_IS_MINUS_W);
    g_dirty = kAllDirty;                                                  // the SDKs bind their own compute state
    if (out) {
        g_su_tex = best_r.tex;
        g_su_view = out;
    }
}

void scene_upscale_for_draw(const ProgLayout& pl) {
    g_su_tex = nullptr;
    g_su_view = VK_NULL_HANDLE;
    const ShaderInfo* fs = S.stage[kFragment];
    if (fs && extras::temporal_wanted() && S.ncolor > 0 && S.color_tex[0] && extras::hdr_is_tonemap(fs->spirv, fs->hash)) {
        temporal_for_draw(pl);
        return;
    }
    if (!fs || !extras::scene_upscale_wanted() || S.ncolor == 0 || !S.color_tex[0] || !extras::hdr_is_tonemap(fs->spirv, fs->hash)) return;
    Texture* target = S.color_tex[0].get();
    if (!tex_gpu(target)) return;
    const VkExtent3D te = tex_extent(target, 0);
    for (const BindingDesc& d : pl.bindings) {
        if (d.type != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) continue;
        const DescriptorBinding& b = d.shader->refl.bindings[d.index];
        if (!texture_source(b.nvn.source)) continue;
        if (view_type_for(b) != VK_IMAGE_VIEW_TYPE_2D) continue;
        TexResolved r = resolve_texture(d);
        if (!r.tex || !tex_gpu(r.tex) || shares_image(r.tex, S.depth_tex.get())) continue;
        const VkExtent3D se = tex_extent(r.tex, 0);
        if (se.width >= te.width && se.height >= te.height) return;          // nothing to enlarge
        extras::SpatialUpscaler* su = extras::scene_upscaler(&device(), VkExtent2D{se.width, se.height}, VkExtent2D{te.width, te.height});
        if (!su) return;
        end_pass();                                                           // compute work cannot run inside a render pass
        tex_ensure_current(r.tex, *F);
        tex_transition(r.tex, *F, tex_sampled_layout(r.tex));
        VkImageView src = tex_sampled_view(r.tex, make_request(b, r));
        if (!src) return;
        VkImageView up = su->upscale(F->cb, src, VkExtent2D{se.width, se.height}, VkExtent2D{se.width, se.height}, VkExtent2D{te.width, te.height}, true,
                                     extras::scene_upscale_prescale(), extras::upscale_sharpness());
        if (up) {
            g_su_tex = r.tex;
            g_su_view = up;
        }
        return;
    }
}

// Evaluation-only programs (NVN tessellates with the fixed SetInner/OuterTessellationLevels): a generated pass-through control shader
// (nvn_tess_gen.h), one per (vertex shader, evaluation shader, patch size). It copies each per-vertex input of the evaluation shader from the
// vertex shader output at the same location (zero where the vertex shader writes none) and gl_Position, zeroes the per-patch inputs, and reads
// the levels from push constants (pushed before each such draw, so level changes need no new pipeline). Null when an interface variable has a
// type the generator does not handle.
const ShaderInfo* g_gen_tcs = nullptr;   // the generated control shader of the current draw (null: the program has its own, or none)
const ShaderInfo* passthrough_tcs_for(const ShaderInfo* vs, const ShaderInfo* tes) {
    struct Key {
        const ShaderInfo* vs; const ShaderInfo* tes; uint32_t n;
        bool operator<(const Key& o) const { return std::tie(vs, tes, n) < std::tie(o.vs, o.tes, o.n); }
    };
    static std::map<Key, std::unique_ptr<ShaderInfo>> cache;
    const uint32_t n = std::clamp<uint32_t>(S.patch_size, 1, 32);
    const Key key{vs, tes, n};
    auto it = cache.find(key);
    if (it != cache.end()) return it->second.get();
    auto to_io = [](const ShaderIo& v, tessgen::Io& o) {
        if (v.components < 1 || v.components > 4) return false;
        if (v.type == ScalarType::Float) o.kind = tessgen::Kind::Float;
        else if (v.type == ScalarType::Sint) o.kind = tessgen::Kind::Sint;
        else if (v.type == ScalarType::Uint) o.kind = tessgen::Kind::Uint;
        else return false;
        o.location = v.location;
        o.component = v.component;
        o.components = v.components;
        return true;
    };
    std::vector<tessgen::Io> vs_out, per_vertex, per_patch;
    bool ok = true;
    for (const ShaderIo& o : vs->refl.outputs) {   // an output array (several locations) is not matched: its readers get zero
        tessgen::Io io;
        if (o.array <= 1 && to_io(o, io)) vs_out.push_back(io);
    }
    for (const ShaderIo& in : tes->refl.inputs) {
        tessgen::Io io;
        if (!to_io(in, io)) { ok = false; break; }
        (in.array > 1 ? per_vertex : per_patch).push_back(io);   // in an evaluation shader a non-array input is per patch
    }
    std::unique_ptr<ShaderInfo> si;
    if (ok) {
        const std::vector<tessgen::Copy> copies = tessgen::match(vs_out, per_vertex);
        size_t copied = 0;
        for (const tessgen::Copy& c : copies) copied += c.has_src;
        si = std::make_unique<ShaderInfo>();
        si->hash = "tcs-pass-" + vs->hash + "-" + tes->hash + "-" + std::to_string(n);
        si->stage = ShaderStage::TessControl;
        // gl_ClipDistance / gl_CullDistance: copied when the vertex shader writes them and the evaluation shader reads gl_in[]'s
        auto uses = [](const std::vector<uint32_t>& v, uint32_t b) { return std::find(v.begin(), v.end(), b) != v.end(); };
        constexpr uint32_t kBuiltInClip = 3, kBuiltInCull = 4;
        const bool clip = uses(vs->refl.builtins_used_out, kBuiltInClip) && uses(tes->refl.builtins_used_in, kBuiltInClip);
        const bool cull = uses(vs->refl.builtins_used_out, kBuiltInCull) && uses(tes->refl.builtins_used_in, kBuiltInCull);
        si->spirv = tessgen::passthrough_tcs(copies, per_patch, n, 7, 1, clip, cull);
        si->push_bytes = tessgen::kPushBytes;
        std::string err;
        if (!reflect_spirv(si->spirv.data(), si->spirv.size(), si->refl, &err)) {
            NXLOG_W(kTag, "generated control shader for %s does not reflect: %s", tes->hash.c_str(), err.c_str());
            si.reset();
        } else {
            NXLOG_I(kTag, "evaluation shader %s (vs %s): pass-through control shader, %u vertices, %zu per-vertex locations (%zu from the vertex shader, %zu zero), %zu per-patch%s%s",
                    tes->hash.c_str(), vs->hash.c_str(), n, copies.size(), copied, copies.size() - copied, per_patch.size(), clip ? ", clip distances" : "",
                    cull ? ", cull distances" : "");
        }
    } else {
        NXLOG_W(kTag, "evaluation shader %s: an input is not a 1..4 component float/int, no pass-through control shader", tes->hash.c_str());
    }
    const ShaderInfo* out = si.get();
    cache.emplace(key, std::move(si));
    return out;
}
const ShaderInfo* tcs_now() { return S.stage[kTessCtl] ? S.stage[kTessCtl] : g_gen_tcs; }

// On-demand shader translation (nvn_shaders.h): a bound program with a stage still being translated skips its draws (graphics stages bound)
// or dispatches (compute bound) until the stage is there. Counted, and logged once per 10 s while it happens.
bool g_gfx_pending = false, g_cs_pending = false;
std::string g_gfx_missing;   // the untranslated block(s) of the bound graphics program (gaps: draws made without them)
uint64_t g_pending_draws = 0, g_pending_dispatches = 0;
std::chrono::steady_clock::time_point g_pending_log_at{};

void note_pending_skip(bool dispatch) {
    ++(dispatch ? g_pending_dispatches : g_pending_draws);
    const auto now = std::chrono::steady_clock::now();
    if (g_pending_log_at.time_since_epoch().count() == 0) g_pending_log_at = now;
    if (now - g_pending_log_at < std::chrono::seconds(10)) return;
    ShaderCache& c = shader_cache();
    NXLOG_I(kTag, "shaders: skipped %llu draws and %llu dispatches in the last %.0f s waiting for on-demand translation (%zu blocks queued, %llu translated this run)",
            static_cast<unsigned long long>(g_pending_draws), static_cast<unsigned long long>(g_pending_dispatches),
            std::chrono::duration<double>(now - g_pending_log_at).count(), c.translator_pending(), static_cast<unsigned long long>(c.translator_done()));
    g_pending_draws = g_pending_dispatches = 0;
    g_pending_log_at = now;
}

void upscaler_note_draw(const ShaderInfo* fs);

// `mesh`: a DrawMeshTasks draw (task/mesh program, no vertex input): the mesh stage takes the vertex stage's place below.
bool g_skip_gpt_raster = false;
bool begin_draw(int mode, bool indexed, int index_type, VkPrimitiveTopology* topo_out, bool mesh = false) {
    g_skip_gpt_raster = false;
    if (g_gfx_pending) { note_pending_skip(false); return false; }
    static bool last_mesh = false;
    if (mesh != last_mesh) { g_dirty |= kPipe | kDesc | kVb; last_mesh = mesh; }
    if (!g_gfx_missing.empty())   // a stage the translator could not do: skipped (no vertex / mesh shader) or drawn without it (no fragment shader)
        nxrt::gap("draw", S.stage[mesh ? kMesh : kVertex] ? "drawn without an untranslated stage" : mesh ? "skipped: untranslated mesh stage" : "skipped: untranslated vertex stage",
                  g_gfx_missing);
    const ShaderInfo* vs = S.stage[mesh ? kMesh : kVertex];
    if (!vs) { log_once(mesh ? "draw-no-ms" : "draw-no-vs", mesh ? "mesh draw without a mesh shader skipped" : "draw without a vertex shader skipped"); return false; }
    if (mesh && (S.stage[kGeometry] || S.stage[kTessCtl] || S.stage[kTessEval])) { note_unsupported("mesh draw with geometry or tessellation stages"); return false; }
    if (!S.render_enable) return false;
    ++g_draw_idx;
    if (S.stage[kFragment] && skip_fs_draw(S.stage[kFragment]->hash)) return false;
    // [nvn] skip_gpt (measurement only: the picture loses those draws): draws whose pre-raster stage walks the page table are left out; as a
    // cpuperf_ab_what feature it gives paired windows of their GPU cost
    static const int64_t skip_gpt = nxrt::config_get_int("nvn", "skip_gpt", 0);   // 1: left out, 2: no fragments (pre-raster cost only)
    if (skip_gpt && ab_on(kAbSkipGpt))
        for (int st : {static_cast<int>(kVertex), static_cast<int>(kTask), static_cast<int>(kMesh)})
            if (S.stage[st] && S.stage[st]->refl.global_pt) {
                if (skip_gpt == 1) return false;
                g_skip_gpt_raster = true;
            }
    if (!g_skip_fs.empty()) {   // skip_fs_file words "v:<prefix>" (vertex, task or mesh shader) and "gpt" (a pre-raster stage walks the page table)
        for (const std::string& p : g_skip_fs) {
            const bool gpt = p == "gpt";
            if (!gpt && p.compare(0, 2, "v:") != 0) continue;
            for (int st : {static_cast<int>(kVertex), static_cast<int>(kTask), static_cast<int>(kMesh)}) {
                const ShaderInfo* s = S.stage[st];
                if (s && (gpt ? s->refl.global_pt : s->hash.compare(0, p.size() - 2, p, 2, std::string::npos) == 0)) return false;
            }
        }
    }
    gmem_before_op(false);   // A17: held results back, ranges its pointers name written
    upscaler_note_draw(S.stage[kFragment]);   // [nvn] upscaler_dst = fs:...: where the replaced upscaler writes
    {   // [nvn] trace_fs_draws = PREFIX (NXRT_NVN_TRACE_FS_DRAWS): trace the draws with this fragment shader where they happen, in any frame, at
        // most [nvn] trace_fs_draws_max (default 8): the bindings and uniform words of a draw that takes the GPU down in the frame it first appears in.
        static const std::string trace_fs = nxrt::config_get_str("nvn", "trace_fs_draws", "");
        static int64_t trace_fs_left = nxrt::config_get_int("nvn", "trace_fs_draws_max", 8);
        static bool traced_here = false;
        if (traced_here) { g_trace = false; traced_here = false; }
        if (!trace_fs.empty() && !g_trace && trace_fs_left > 0 && S.stage[kFragment] && S.stage[kFragment]->hash.compare(0, trace_fs.size(), trace_fs) == 0) {
            g_trace = traced_here = true;
            --trace_fs_left;
            NXLOG_I("nvn.trace", "---- draw %d of present %llu (trace_fs_draws) ----", g_draw_idx, static_cast<unsigned long long>(counters().presents.load()));
        }
    }
    // Tessellation: Vulkan runs it only with both shaders and a patch list. NVN also allows an evaluation shader alone (fixed levels from
    // SetInner/OuterTessellationLevels): those draws get a generated pass-through control shader (passthrough_tcs_for).
    if (!mesh && (S.stage[kTessCtl] || S.stage[kTessEval] || mode == NVN_DRAW_PRIMITIVE_PATCHES)) {
        if (mode != NVN_DRAW_PRIMITIVE_PATCHES) { note_unsupported("tessellation draw: tessellation program drawn without patches"); return false; }
        if (!S.stage[kTessEval]) { note_unsupported("tessellation draw: patches without a tessellation evaluation shader"); return false; }
        if (!S.stage[kTessCtl]) {
            const ShaderInfo* gen = passthrough_tcs_for(vs, S.stage[kTessEval]);
            if (!gen) { note_unsupported("tessellation draw: evaluation shader without a control shader, no pass-through could be made"); return false; }
            if (gen != g_gen_tcs) g_dirty |= kPipe;
            g_gen_tcs = gen;
        }
    } else {
        g_gen_tcs = nullptr;
    }
    if (g_dirty & (kPipe | kDesc)) {
        const ShaderInfo* stages[kStages] = {vs, S.stage[kFragment], S.stage[kGeometry], tcs_now(), S.stage[kTessEval], nullptr, nullptr, nullptr};
        const ShaderInfo* mstages[kStages] = {nullptr, S.stage[kFragment], nullptr, nullptr, nullptr, nullptr, S.stage[kTask], S.stage[kMesh]};
        g_pl = prog_layout(mesh ? mstages : stages);
    }
    if (!g_trigger_fs.empty() && !g_trigger_done && trigger_time_ok() && S.stage[kFragment] && S.stage[kFragment]->hash.compare(0, g_trigger_fs.size(), g_trigger_fs) == 0) {
        g_trigger_seen = true;
        g_trigger_present = static_cast<int64_t>(counters().presents.load());
    }
    const ProgLayout* pl = g_pl;
    if (!pl) return false;
    if (extras::temporal_wanted()) temporal_gbuffer_note();
    if (g_dirty & (kDesc | kPipe)) {
        g_prog_writes = false;
        for (const BindingDesc& d : pl->bindings) g_prog_writes |= d.type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER || d.type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        attach_targets(false);
        g_want_depth_ro = samples_depth_target(*pl);
        prepare_textures(*pl);
        if (g_trace) {   // [nvn] trace_dump_tex: also the block-compressed level 0 as the GPU holds it, to compare with the prepared data
            static const std::string dump_dir = nxrt::config_get_str("nvn", "trace_dump_tex", "");
            static std::unordered_map<uint32_t, bool> raw_done;
            if (!dump_dir.empty()) {
                for (const BindingDesc& d : pl->bindings) {
                    if (d.type != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) continue;
                    const TexResolved r = resolve_texture(d);
                    if (!r.tex || !raw_done.emplace(r.tex->id, true).second) continue;
                    end_pass();
                    const int ri = snapshot_record_raw(r.tex, *F);
                    if (ri >= 0) g_trace_snaps.push_back({ri, g_trace_n, dump_dir + "/gpu_t" + std::to_string(r.tex->id) + ".bin", "raw"});
                }
            }
        }
        scene_upscale_for_draw(*pl);
    }
    if (g_fb_seen || (g_dirty & (kDesc | kPipe))) feedback_prepare(*pl);
    if (!ensure_pass(g_want_depth_ro)) { log_once("draw-no-target", "draw without a usable render target skipped"); return false; }
    if (g_prog_writes || g_xfb_active) g_pass_unsafe = true;
    const VkPrimitiveTopology topo = mesh ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST : vk_topology(mode);
    if ((g_dirty & kPipe) || topo != g_pipe_topo || !g_pipe) {
        GraphicsKey key;
        build_key(key, topo, mesh);
        g_pipe = graphics_pipeline(key, *pl);
        g_pipe_xfb = key.xfb != 0;
        g_pipe_topo = topo;
        g_key_attr_count = key.attr_count;
        g_key_dummy_binding = key.dummy_binding;
        for (uint32_t i = 0; i < key.attr_count; ++i) g_key_binding[i] = key.attr[i].binding;
    }
    if (!g_pipe) return false;
    // a draw inside a stream-out section that has no stream-out variant and nothing to render to has no effect
    if (g_xfb_active && !g_pipe_xfb && !P.ncolor && !S.depth_tex) return false;
    if (g_trace) {
        char a[96], b[96], c[96];
        std::string line;
        auto remember = [](Texture* t) {
            static const bool dump = nxrt::config_get_bool("nvn", "trace_targets", true);   // false: no PNG of every target (frame ranges)
            if (!t || !dump) return;
            for (const Ref<Texture>& r : g_trace_targets) if (r.get() == t) return;
            g_trace_targets.push_back(Ref<Texture>::share(t));
        };
        for (uint32_t i = 0; i < P.ncolor; ++i) remember(S.color_tex[i].get());
        remember(S.depth_tex.get());
        for (uint32_t i = 0; i < P.ncolor; ++i) { line += tex_desc(S.color_tex[i].get(), a, sizeof a); line += " | "; }
        TRACE("draw #%d vs %s fs %s gs %s mode %d idx %d | rt %s ds %s%s | blend %u mask %x depth t%d w%d f%d cull %d | ubo0 %llx/%llu",
              ++g_trace_n, S.stage[kVertex] ? S.stage[kVertex]->hash.c_str() : "-", S.stage[kFragment] ? S.stage[kFragment]->hash.c_str() : "-",
              S.stage[kGeometry] ? S.stage[kGeometry]->hash.c_str() : "-", mode, indexed ? 1 : 0, line.c_str(), tex_desc(S.depth_tex.get(), b, sizeof b),
              P.depth_ro ? " (ro)" : "", S.color.blend_enable_mask, S.chmask.mask[0], S.ds.depth_test ? 1 : 0, S.ds.depth_write ? 1 : 0, S.ds.depth_func,
              S.poly.cull_face, static_cast<unsigned long long>(S.ubo[kVertex][0].addr), static_cast<unsigned long long>(S.ubo[kVertex][0].size));
        TRACE("    poly mode %d offset %x factor %g units %g clamp %g | depth mode %d origin %d clamp %d", S.poly.polygon_mode, S.poly.offset_enables, S.bias_factor, S.bias_units,
              S.bias_clamp, S.depth_mode, S.window_origin, S.depth_clamp ? 1 : 0);
        if (S.color.blend_enable_mask)
            TRACE("    blend rt0 rgb %d,%d,%d a %d,%d,%d (nvn enums src,dst,eq) constant %g %g %g %g", S.blend[0].src_rgb, S.blend[0].dst_rgb, S.blend[0].eq_rgb, S.blend[0].src_a,
                  S.blend[0].dst_a, S.blend[0].eq_a, S.blend_color[0], S.blend_color[1], S.blend_color[2], S.blend_color[3]);
        // [nvn] trace_ubo_bytes = N (NXRT_NVN_TRACE_UBO_BYTES, default 768, at most 65536): how many leading bytes of each uniform block the trace prints
        static const uint64_t ubo_bytes = static_cast<uint64_t>(std::clamp<int64_t>(nxrt::config_get_int("nvn", "trace_ubo_bytes", 768), 16, 65536));
        for (int stg : mesh ? std::initializer_list<int>{kTask, kMesh, kFragment} : std::initializer_list<int>{kVertex, kFragment})
        for (uint32_t slot = 0; slot < 16; ++slot) {    // the first words of every uniform block bound to the vertex and the fragment stage
            const GState::Bind& ub = S.ubo[stg][slot];
            if (!ub.addr) continue;
            BufRef ur = resolve_buffer(ub.addr, std::min<uint64_t>(ub.size, ubo_bytes));
            std::string words;
            uint32_t first = 0;
            const uint32_t nwords = ur.ok ? static_cast<uint32_t>(std::min<uint64_t>(ubo_bytes, ub.size) / 4) : 0;
            const uint32_t* w = reinterpret_cast<const uint32_t*>(ub.addr);
            for (uint32_t i = 0; i < nwords; ++i) {   // lines of 128 words (log lines end at 2047 characters): "+<first word>:" after the first
                char t[16];
                std::snprintf(t, sizeof t, " %08x", w[i]);
                words += t;
                if ((i + 1) % 128 == 0 && i + 1 < nwords) {
                    TRACE("    %s ubo%u %llx/%llu +%u:%s", stg == kVertex ? "vs" : stg == kTask ? "ts" : stg == kMesh ? "ms" : "fs", slot, static_cast<unsigned long long>(ub.addr), static_cast<unsigned long long>(ub.size), first, words.c_str());
                    words.clear();
                    first = i + 1;
                }
            }
            TRACE("    %s ubo%u %llx/%llu%s +%u:%s", stg == kVertex ? "vs" : stg == kTask ? "ts" : stg == kMesh ? "ms" : "fs", slot, static_cast<unsigned long long>(ub.addr), static_cast<unsigned long long>(ub.size), ur.ok ? "" : " (outside every pool)", first, words.c_str());
        }
        for (int stg : mesh ? std::initializer_list<int>{kTask, kMesh, kFragment} : std::initializer_list<int>{kVertex, kFragment})
        for (uint32_t slot = 0; slot < 16; ++slot) {    // the storage buffers bound to the vertex and the fragment stage, and their first words
            const GState::Bind& sb = S.ssbo[stg][slot];
            if (!sb.addr) continue;
            BufRef sr = resolve_buffer(sb.addr, std::min<uint64_t>(sb.size, 256));
            std::string words;
            if (sr.ok) {
                const uint32_t* w = reinterpret_cast<const uint32_t*>(sb.addr);
                for (uint32_t i = 0; i < 64 && i * 4 < sb.size; ++i) { char t[16]; std::snprintf(t, sizeof t, " %08x", w[i]); words += t; }
            }
            TRACE("    %s ssbo%u %llx/%llu%s:%s", stg == kVertex ? "vs" : stg == kTask ? "ts" : stg == kMesh ? "ms" : "fs", slot, static_cast<unsigned long long>(sb.addr), static_cast<unsigned long long>(sb.size), sr.ok ? "" : " (outside every pool)", words.c_str());
        }
        TRACE("    viewport %u: %g %g %g %g depth %g..%g scissor %u: %d %d %d %d", S.nvp, S.vp[0][0], S.vp[0][1], S.vp[0][2], S.vp[0][3], S.dr[0][0], S.dr[0][1], S.nsc,
              S.sc[0][0], S.sc[0][1], S.sc[0][2], S.sc[0][3]);
        for (uint32_t i = 0; i < P.ncolor; ++i)
            if (Texture* ct = S.color_tex[i].get()) {
                const FormatInfo* vf = (S.color_has_view[i] && S.color_view[i].has_format) ? format_info(S.color_view[i].format) : nullptr;
                TRACE("    rt%u t%u mem %p storage %llu cpu-dirty %d view level %u layer %u layers %u format %s", i, ct->id, static_cast<void*>(ct->mem),
                      static_cast<unsigned long long>(ct->storage_size), ct->cpu_dirty.load() ? 1 : 0, S.color_level[i], S.color_layer[i], S.color_layers[i],
                      vf && vf->name ? vf->name : "-");
            }
        g_trace_sampled.clear();
        for (const BindingDesc& d : pl->bindings) {   // texel buffers (light lists and other BUFFER-target textures): the view the draw gets
            if (d.type != VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER && d.type != VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) continue;
            const bool storage = d.type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
            const TexResolved r = storage ? resolve_image(d) : resolve_texture(d);
            if (!r.tex) { TRACE("    texel set%u b%u -> (no texture)", d.set, d.binding); continue; }
            uint64_t addr = 0, bytes = 0;
            const VkBufferView v = tex_buffer_view(r.tex, r.has_view ? &r.view : nullptr, 0, VK_FORMAT_UNDEFINED, storage, &addr, &bytes);
            std::string words;
            if (r.tex->mem) {
                const uint32_t* w = reinterpret_cast<const uint32_t*>(r.tex->mem);
                const size_t nw = std::min<size_t>(32, static_cast<size_t>(r.tex->b.width) * (r.tex->fmt ? r.tex->fmt->bytes : 4) / 4);
                char wb[12];
                for (size_t i = 0; i < nw; ++i) { std::snprintf(wb, sizeof wb, " %08x", w[i]); words += wb; }
                const size_t all = static_cast<size_t>(r.tex->b.width) * (r.tex->fmt ? r.tex->fmt->bytes : 4) / 4;
                size_t nz = 0;
                for (size_t i = 0; i < all; ++i) nz += w[i] != 0;
                std::snprintf(wb, sizeof wb, " |nz %zu", nz);
                words = std::string(wb) + "/" + std::to_string(all) + words;
            }
            TRACE("    texel set%u b%u -> t%u %s width %d @%p view %s addr %llx bytes %llu view-format %s:%s", d.set, d.binding, r.tex->id, r.tex->fmt ? r.tex->fmt->name : "?", r.tex->b.width,
                  static_cast<void*>(r.tex->mem), v ? "ok" : "NONE (dummy)", static_cast<unsigned long long>(addr), static_cast<unsigned long long>(bytes),
                  (r.has_view && r.view.has_format) ? "set" : "-", words.c_str());
        }
        for (const BindingDesc& d : pl->bindings) {
            if (d.type != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER && d.type != VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) continue;
            const TexResolved r = d.type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ? resolve_image(d) : resolve_texture(d);
            if (r.tex) g_trace_sampled.push_back(r.tex);
            if (r.tex) {
                static const std::string dump_dir = nxrt::config_get_str("nvn", "trace_dump_tex", "");
                if (!dump_dir.empty()) trace_dump_prepared(r.tex, dump_dir);
            }
            char smp[200] = "", vw[160] = "";
            if (r.has_sampler)
                std::snprintf(smp, sizeof smp, " | sampler min %d mag %d wrap %d,%d,%d lod %g..%g bias %g aniso %g cmp %d func %d", r.smp.min_filter, r.smp.mag_filter, r.smp.wrap_s, r.smp.wrap_t,
                              r.smp.wrap_r, r.smp.lod_min, r.smp.lod_max, r.smp.lod_bias, r.smp.max_anisotropy, r.smp.compare_mode, r.smp.compare_func);
            if (r.has_view) {
                // the view's own format by name (a view naming the sRGB partner of its texture's format, or the reverse, changes the decode)
                const FormatInfo* vf = r.view.has_format ? format_info(r.view.format) : nullptr;
                char vfn[48] = "";
                if (r.view.has_format) std::snprintf(vfn, sizeof vfn, " format %s", vf && vf->name ? vf->name : "?");
                std::snprintf(vw, sizeof vw, " | view levels %d+%d layers %d+%d%s", r.view.has_levels ? r.view.min_level : -1, r.view.has_levels ? r.view.num_levels : -1,
                              r.view.has_layers ? r.view.min_layer : -1, r.view.has_layers ? r.view.num_layers : -1, vfn);
            }
            if (r.tex) {
                const int* sw = (r.has_view && r.view.has_swizzle) ? r.view.swizzle : r.tex->b.swizzle;
                const size_t n = std::strlen(vw);
                std::snprintf(vw + n, sizeof vw - n, " swz %d%d%d%d", sw[0], sw[1], sw[2], sw[3]);
            }
            TRACE("    %s set%u b%u %s -> %s @%p%s mem %d%%%s%s%s", d.type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ? "image" : "tex", d.set, d.binding,
                  d.shader->refl.bindings[d.index].name.c_str(), tex_desc(r.tex, c, sizeof c), r.tex ? static_cast<void*>(r.tex->mem) : nullptr, r.has_sampler ? "" : " (no sampler)", storage_nonzero_pct(r.tex),
                  r.tex && r.tex->cpu_dirty.load() ? " cpu-dirty" : "", smp, vw);
        }
    }
    if (g_xfb_active && !mesh) {
        static std::unordered_map<std::string, bool> seen;
        if (seen.emplace(S.stage[kVertex]->hash, true).second)
            NXLOG_I("nvn.xfb", "draw under transform feedback: vs %s mode %d attrs %u xfb0 %llx/%llu", S.stage[kVertex]->hash.c_str(), mode, S.attr_count,
                    static_cast<unsigned long long>(S.xfb[0].addr), static_cast<unsigned long long>(S.xfb[0].size));
    }
    if (g_pipe != g_bound_pipeline) {
        xfb_end_in_pass();     // the pipeline cannot change while feedback is active; the byte counter carries the position over
        vkCmdBindPipeline(F->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pipe);
        g_gauge_pipeline_binds.add(1);
        g_bound_pipeline = g_pipe;
    }
    if (g_xfb_active && g_pipe_xfb && !g_xfb_began && device().features().transform_feedback && S.xfb[0].addr) {
        BufRef xb = resolve_buffer(S.xfb[0].addr, S.xfb[0].size);
        if (xb.ok) {
            gpu_wrote(S.xfb[0].addr, S.xfb[0].size ? S.xfb[0].size : xb.avail, kWriteXfb);
            if (!g_xfb_counter) {
                if (device().create_buffer(kXfbSlots * kXfbSlotBytes, VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_COUNTER_BUFFER_BIT_EXT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, gfx::MemoryUse::Upload,
                                           &g_xfb_counter, "nvn xfb counter") != VK_SUCCESS)
                    g_xfb_counter = gfx::Buffer();
            }
            const VkDeviceSize size = std::min<VkDeviceSize>(S.xfb[0].size ? S.xfb[0].size : xb.avail, xb.avail) & ~static_cast<VkDeviceSize>(3);
            vkCmdBindTransformFeedbackBuffersEXT(F->cb, 0, 1, &xb.buffer, &xb.offset, &size);
            VkBuffer cbuf = g_xfb_counter.buffer;
            const VkDeviceSize coff = static_cast<VkDeviceSize>(g_xfb_cur) * kXfbSlotBytes;
            if (g_xfb_have_counter && cbuf) vkCmdBeginTransformFeedbackEXT(F->cb, 0, 1, &cbuf, &coff);
            else vkCmdBeginTransformFeedbackEXT(F->cb, 0, 0, nullptr, nullptr);
            g_xfb_began = true;
        } else {
            note_unsupported("transform feedback into an address outside every pool");
        }
    }
    set_dynamic_state(g_dirty, topo, indexed, index_type);
    if (g_dirty & kDesc) {
        support_for_draw(*pl, false);
        bind_sets(*pl, VK_PIPELINE_BIND_POINT_GRAPHICS);
    }
    if (pl->push_bytes && g_gen_tcs) {
        // the pass-through control shader's tessellation levels (NVN's fixed SetInner/OuterTessellationLevels), every draw: cheap, and
        // the layout may have changed since the last push
        tessgen::TessPush push = {};
        std::memcpy(push.outer, S.tess_outer, sizeof push.outer);
        std::memcpy(push.inner, S.tess_inner, sizeof push.inner);
        vkCmdPushConstants(F->cb, pl->layout, pl->push_stages, 0, std::min<uint32_t>(pl->push_bytes, sizeof push), &push);
        static std::set<const ShaderInfo*> logged;
        if (logged.insert(g_gen_tcs).second)
            NXLOG_I(kTag, "tessellation draw: evaluation shader %s drawn with pass-through control shader %s, patch size %u, levels %g %g %g %g / %g %g",
                    S.stage[kTessEval]->hash.c_str(), g_gen_tcs->hash.c_str(), static_cast<unsigned>(std::clamp<uint32_t>(S.patch_size, 1, 32)),
                    S.tess_outer[0], S.tess_outer[1], S.tess_outer[2], S.tess_outer[3], S.tess_inner[0], S.tess_inner[1]);
    }
    if (!mesh && (g_dirty & (kVb | kPipe))) {
        for (uint32_t i = 0; i < g_key_attr_count; ++i) {
            const uint32_t binding = g_key_binding[i];
            bool done = false;
            for (uint32_t j = 0; j < i; ++j) if (g_key_binding[j] == binding) done = true;
            if (done) continue;
            if (g_key_dummy_binding && binding == static_cast<uint32_t>(g_key_dummy_binding - 1)) {
                VkBuffer dummy = dummy_buffer();
                VkDeviceSize off = kDummyAttrFloatOffset, size = 32, stride = 0;
                vkCmdBindVertexBuffers2(F->cb, binding, 1, &dummy, &off, &size, &stride);
                g_vb_bound_in_cb = true;
                continue;
            }
            const GState::Bind& vb = S.vtx[binding];
            BufRef br = vb.addr ? resolve_read(vb.addr, vb.size, kReadVertex) : BufRef{dummy_buffer(), 0, 65536, false};
            if (vb.addr) upd_note_read(vb.addr, vb.size ? vb.size : br.avail);
            VkDeviceSize size = vb.size ? std::min<VkDeviceSize>(vb.size, br.avail) : br.avail;
            VkDeviceSize stride = static_cast<VkDeviceSize>(S.stream[binding].stride);
            vkCmdBindVertexBuffers2(F->cb, binding, 1, &br.buffer, &br.offset, &size, &stride);
            g_vb_bound_in_cb = true;
        }
        if (!g_vb_bound_in_cb) {
            // the pipeline's vertex binding strides are dynamic: a command buffer that never bound a vertex buffer must still bind one
            VkBuffer dummy = dummy_buffer();
            VkDeviceSize off = 0, size = 65536, stride = 0;
            vkCmdBindVertexBuffers2(F->cb, 0, 1, &dummy, &off, &size, &stride);
            g_vb_bound_in_cb = true;
        }
    }
    ++g_chunk_work;
    g_dirty = g_mirror_stale ? (kVb | kDesc) : 0;     // a storage buffer of this draw made copied pages stale: the next draw binds again
    g_mirror_stale = false;
    if (g_skip_gpt_raster) {   // [nvn] skip_gpt = 2: the pre-raster stage runs, no fragment (an empty scissor); the next draw sets them again
        g_skip_gpt_raster = false;
        const VkRect2D none = {{0, 0}, {0, 0}};
        VkRect2D sc[16];
        const uint32_t n = S.nvp ? S.nvp : 1;
        for (uint32_t i = 0; i < n; ++i) sc[i] = none;
        vkCmdSetScissorWithCount(F->cb, n, sc);
        g_dirty |= kDynVp;
    }
    *topo_out = topo;
    if (checkpoints_on()) {   // [gfx] device_diagnostics: which draw the GPU was at when the device was lost
        char t[128];
        std::snprintf(t, sizeof t, "draw %d mode %d%s vs %s fs %s", g_draw_idx, mode, indexed ? " indexed" : "", vs->hash.c_str(),
                      S.stage[kFragment] ? S.stage[kFragment]->hash.c_str() : "-");
        checkpoint(*F, t);
    }
    return true;
}


// [nvn] trace_snap = "253,300": after those draws of the traced frame the pass is closed and the colour targets and the depth target are
// copied (png after the frame: snap_d<draw>_t<id>_<format>.png).
void trace_snap_after_draw() {
    if (!g_trace) return;
    if (!g_trace_snap_read) {
        g_trace_snap_read = true;
        g_trace_snap_sampled = nxrt::config_get_int("nvn", "trace_snap_sampled", 0) != 0;
        {   // [nvn] trace_snap_fs = "f77f9d,e72811": every draw whose fragment shader hash starts with one of these
            const std::string fl = nxrt::config_get_str("nvn", "trace_snap_fs", "");
            size_t k = 0;
            while (k < fl.size()) {
                size_t j = fl.find(',', k);
                if (j == std::string::npos) j = fl.size();
                if (j > k) g_trace_snap_fs.push_back(fl.substr(k, j - k));
                k = j + 1;
            }
        }
        const std::string list = nxrt::config_get_str("nvn", "trace_snap", "");
        size_t i = 0;
        while (i < list.size()) {
            size_t j = list.find(',', i);
            if (j == std::string::npos) j = list.size();
            if (j > i) {   // a draw number or a range "a-b"
                const std::string item = list.substr(i, j - i);
                const size_t dash = item.find('-');
                const int a = std::atoi(item.c_str());
                const int b = dash == std::string::npos ? a : std::atoi(item.c_str() + dash + 1);
                for (int d = a; d <= b && d - a < 100000; ++d) g_trace_snap_draws.push_back(d);
            }
            i = j + 1;
        }
    }
    bool hit = false;
    for (int d : g_trace_snap_draws) hit |= d == g_trace_n;
    if (!hit && S.stage[kFragment] && !g_trace_snap_fs.empty()) {
        for (const std::string& pre : g_trace_snap_fs) hit |= S.stage[kFragment]->hash.compare(0, pre.size(), pre) == 0;
    }
    if (!hit) return;
    end_pass();
    auto take = [&](Texture* t) {
        if (!t) return;
        const int idx = snapshot_record(t, *F);
        if (idx < 0) return;
        char buf[96];
        g_trace_snaps.push_back({idx, g_trace_n, std::string("snap_d") + std::to_string(g_trace_n) + "_t" + std::to_string(t->id) + "_" + (t->fmt ? t->fmt->name : "x") + ".png", tex_desc(t, buf, sizeof buf)});
    };
    for (uint32_t i = 0; i < S.ncolor; ++i) take(S.color_tex[i].get());
    take(S.depth_tex.get());
    if (g_trace_snap_sampled) {
        for (Texture* t : g_trace_sampled) take(t);
    }
    // [nvn] trace_snap_ssbo = 1: the storage buffers bound to the vertex and fragment stages too, as the GPU holds them after the draw
    // (snapb_d<draw>_<stage>s<slot>_<address>.bin, at most [nvn] trace_snap_ssbo_max_kb each, default 1024): buffers that compute passes write never reach the CPU's view.
    static const bool snap_ssbo = nxrt::config_get_bool("nvn", "trace_snap_ssbo", false);
    static const uint64_t snap_max = static_cast<uint64_t>(std::max<int64_t>(4, nxrt::config_get_int("nvn", "trace_snap_ssbo_max_kb", 1024))) * 1024;
    if (snap_ssbo)
        for (int st : {kVertex, kFragment})
            for (int k = 0; k < 16; ++k) {
                const GState::Bind& b = S.ssbo[st][k];
                if (!b.addr) continue;
                const uint64_t n = std::min<uint64_t>(b.size ? b.size : 65536, snap_max);
                const BufRef r = held_or_pool(b.addr, n, kOutStorageXfb, true);
                if (!r.buffer) continue;
                const int idx = snapshot_record_buffer(r.buffer, r.offset, std::min<VkDeviceSize>(n, r.avail ? r.avail : n), *F);
                if (idx < 0) continue;
                char name[96];
                std::snprintf(name, sizeof name, "snapb_d%d_%cs%d_%llx.bin", g_trace_n, st == kVertex ? 'v' : 'f', k, static_cast<unsigned long long>(b.addr));
                g_trace_snaps.push_back({idx, g_trace_n, name, "raw"});
            }
}

// Counts a draw for the primitive counters, before the replay decides whether to skip it: the console generates the primitives of every
// draw it runs, and a draw of a feedback section also writes them (14). A draw with rendering disabled is not run by the console: not counted.
void count_primitives(uint32_t mode, uint32_t count, uint32_t instances) {
    if (!S.render_enable) return;
    const uint64_t per = mode == NVN_DRAW_PRIMITIVE_PATCHES ? count / std::max<uint32_t>(1, S.patch_size) : primitives_for(static_cast<int>(mode), count);   // patches: input patches
    const uint64_t prims = per * std::max<uint32_t>(1, instances);
    g_prims_generated += prims;
    if (g_xfb_active) g_prims_xfb += prims;
}

// Quads, quad strips, polygons and line loops, which Vulkan has no topology for, as the triangle (or line strip) index list of the same vertices.
// `in` holds the vertex indices of the draw in order (for DrawArrays: first, first + 1, ...). Shared by the direct and the indirect draws.
std::vector<uint32_t> convert_prim_indices(PrimConversion conv, const std::vector<uint32_t>& in) {
    std::vector<uint32_t> idx;
    const size_t n = in.size();
    switch (conv) {
        case PrimConversion::Quads:
            for (size_t q = 0; q + 3 < n; q += 4) idx.insert(idx.end(), {in[q], in[q + 1], in[q + 2], in[q], in[q + 2], in[q + 3]});
            break;
        case PrimConversion::QuadStrip:
            for (size_t q = 0; q + 3 < n; q += 2) idx.insert(idx.end(), {in[q], in[q + 1], in[q + 2], in[q + 2], in[q + 1], in[q + 3]});
            break;
        case PrimConversion::Polygon:
            for (size_t k = 1; k + 1 < n; ++k) idx.insert(idx.end(), {in[0], in[k], in[k + 1]});
            break;
        case PrimConversion::LineLoop:
            idx = in;
            if (!in.empty()) idx.push_back(in[0]);
            break;
        default: idx = in; break;
    }
    return idx;
}

// Binds an index list of 32-bit indices from the upload buffer. False: no room.
bool bind_u32_indices(const std::vector<uint32_t>& idx) {
    UploadAlloc u = F->upload(idx.size() * 4, 4);
    if (!u) return false;
    std::memcpy(u.ptr, idx.data(), idx.size() * 4);
    vkCmdBindIndexBuffer(F->cb, u.buffer, u.offset, VK_INDEX_TYPE_UINT32);
    return true;
}

void do_draw_arrays(const P_DrawArrays& p) {
    count_primitives(p.mode, p.count, p.instance_count);
    VkPrimitiveTopology topo;
    if (!begin_draw(static_cast<int>(p.mode), false, 0, &topo)) { counters().draws_skipped.fetch_add(1); return; }
    const PrimConversion conv = prim_conversion(static_cast<int>(p.mode));
    if (conv == PrimConversion::None) {
        TRACE("    vertices %u instances %u first %u", p.count, p.instance_count, p.first);
        vkCmdDraw(F->cb, p.count, std::max<uint32_t>(1, p.instance_count), p.first, p.base_instance);
    } else {
        // QUADS / QUAD_STRIP / POLYGON / LINE_LOOP: generate an index buffer
        std::vector<uint32_t> in(p.count);
        for (uint32_t k = 0; k < p.count; ++k) in[k] = p.first + k;
        const std::vector<uint32_t> idx = convert_prim_indices(conv, in);
        if (idx.empty()) return;
        if (!bind_u32_indices(idx)) return;
        vkCmdDrawIndexed(F->cb, static_cast<uint32_t>(idx.size()), std::max<uint32_t>(1, p.instance_count), 0, 0, p.base_instance);
    }
    counters().draws.fetch_add(1, std::memory_order_relaxed);
    g_pass_draws++;
    draw_stamp();
    g_pass_verts += static_cast<uint64_t>(p.count) * std::max<uint32_t>(1, p.instance_count);
    trace_snap_after_draw();
}

void do_draw_elements(const P_DrawElements& p) {
    count_primitives(p.mode, p.count, p.instance_count);
    VkPrimitiveTopology topo;
    if (!begin_draw(static_cast<int>(p.mode), true, static_cast<int>(p.type), &topo)) { counters().draws_skipped.fetch_add(1); return; }
    if (g_trace) {   // the indices and the first attributes of the first vertices
        const uint32_t isz0 = index_size_bytes(static_cast<int>(p.type));
        const uint8_t* ix = host_ptr(p.index_address);
        TRACE("    indexed: count %u type %d instances %u base_vertex %d base_instance %u index address %llx", p.count, static_cast<int>(p.type), p.instance_count, static_cast<int>(p.base_vertex),
              p.base_instance, static_cast<unsigned long long>(p.index_address));
        for (uint32_t i = 0; ix && i < std::min<uint32_t>(p.count, 12); ++i) {
            uint32_t v = 0;
            if (isz0 == 1) v = ix[i]; else if (isz0 == 2) { uint16_t t; std::memcpy(&t, ix + i * 2, 2); v = t; } else std::memcpy(&v, ix + i * 4, 4);
            std::string a;
            for (uint32_t k = 0; k < S.attr_count && k < 3; ++k) {
                const VertexAttribInfo& at = S.attr[k];
                const auto& st = S.stream[at.stream];
                const GState::Bind& vb = S.vtx[at.stream];
                if (!vb.addr) continue;
                const float* f = reinterpret_cast<const float*>(vb.addr + static_cast<uint64_t>(static_cast<int64_t>(v) + p.base_vertex) * st.stride + at.relative_offset);
                char t[96]; std::snprintf(t, sizeof t, " a%u(fmt %d)=(%g %g %g)", k, at.format, f[0], f[1], f[2]);
                a += t;
            }
            TRACE("      index %u:%s", v, a.c_str());
        }
    }
    if (g_trace) {   // bounds of the first attributes over all indices of the draw (half and float formats decoded), to see what part of a texture a mesh maps
        const uint32_t isz1 = index_size_bytes(static_cast<int>(p.type));
        const uint8_t* ix = host_ptr(p.index_address);
        if (ix && p.count) {
            std::string a;
            for (uint32_t k = 0; k < S.attr_count && k < 3; ++k) {
                const VertexAttribInfo& at = S.attr[k];
                const auto& st = S.stream[at.stream];
                const GState::Bind& vb = S.vtx[at.stream];
                if (!vb.addr) continue;
                float lo[4] = {1e30f, 1e30f, 1e30f, 1e30f}, hi[4] = {-1e30f, -1e30f, -1e30f, -1e30f};
                int nc = at.format == 41 ? 4 : at.format == 34 ? 3 : at.format == 17 ? 2 : at.format == 22 ? 2 : 0;
                for (uint32_t i = 0; nc && i < p.count && i < 20000; ++i) {
                    uint32_t v = 0;
                    if (isz1 == 1) v = ix[i]; else if (isz1 == 2) { uint16_t t; std::memcpy(&t, ix + i * 2, 2); v = t; } else std::memcpy(&v, ix + i * 4, 4);
                    const uint8_t* ve = reinterpret_cast<const uint8_t*>(vb.addr + static_cast<uint64_t>(static_cast<int64_t>(v) + p.base_vertex) * st.stride + at.relative_offset);
                    for (int c = 0; c < nc; ++c) {
                        float f;
                        if (at.format == 41 || at.format == 17) {
                            uint16_t h; std::memcpy(&h, ve + c * 2, 2);
                            const int e = (h >> 10) & 31, m = h & 1023; const float sg = (h & 0x8000) ? -1.f : 1.f;
                            f = e == 0 ? sg * m * (1.f / 16777216.f) : e == 31 ? sg * 65504.f : sg * (1.f + m / 1024.f) * std::ldexp(1.f, e - 15);
                        } else std::memcpy(&f, ve + c * 4, 4);
                        lo[c] = std::min(lo[c], f); hi[c] = std::max(hi[c], f);
                    }
                }
                char t[200];
                if (nc) std::snprintf(t, sizeof t, " a%u[%g..%g, %g..%g, %g..%g, %g..%g]", k, lo[0], hi[0], lo[1], hi[1], nc > 2 ? lo[2] : 0.f, nc > 2 ? hi[2] : 0.f, nc > 3 ? lo[3] : 0.f, nc > 3 ? hi[3] : 0.f);
                else std::snprintf(t, sizeof t, " a%u(fmt %d: not decoded)", k, at.format);
                a += t;
            }
            TRACE("    bounds:%s", a.c_str());
        }
    }
    const uint32_t isz = index_size_bytes(static_cast<int>(p.type));
    const PrimConversion conv = prim_conversion(static_cast<int>(p.mode));
    const uint8_t* src = host_ptr(p.index_address);
    if (conv != PrimConversion::None || isz == 1) {
        // widen to 32 bit and/or convert the primitive
        std::vector<uint32_t> in(p.count);
        for (uint32_t i = 0; i < p.count; ++i) {
            uint32_t v = 0;
            if (isz == 1) v = src[i];
            else if (isz == 2) { uint16_t t; std::memcpy(&t, src + i * 2, 2); v = t; }
            else std::memcpy(&v, src + i * 4, 4);
            in[i] = v;
        }
        const std::vector<uint32_t> idx = convert_prim_indices(conv, in);
        if (idx.empty()) return;
        if (!bind_u32_indices(idx)) return;
        vkCmdDrawIndexed(F->cb, static_cast<uint32_t>(idx.size()), std::max<uint32_t>(1, p.instance_count), 0, p.base_vertex, p.base_instance);
    } else {
        BufRef br = resolve_read(p.index_address, static_cast<uint64_t>(p.count) * isz, kReadIndex);
        upd_note_read(p.index_address, static_cast<uint64_t>(p.count) * isz);
        vkCmdBindIndexBuffer(F->cb, br.buffer, br.offset, vk_index_type(static_cast<int>(p.type)));
        vkCmdDrawIndexed(F->cb, p.count, std::max<uint32_t>(1, p.instance_count), 0, p.base_vertex, p.base_instance);
    }
    counters().draws.fetch_add(1, std::memory_order_relaxed);
    g_pass_draws++;
    draw_stamp();
    g_pass_verts += static_cast<uint64_t>(p.count) * std::max<uint32_t>(1, p.instance_count);
    trace_snap_after_draw();
}

// The bytes at a GPU address as the CPU sees them, when they are current: inside a pool, and not a range a submission that has not completed
// may write (gw_settled). Null otherwise. For decisions that need data the GPU would otherwise hold (indirect arguments, draw counts).
// Compute results held in the device copy ([nvn] compute_out_lazy) are newer than the pool: they are fetched back first (out_fetch: a
// completed submission's result, or one whose copy-back is recorded but has not run); one the submission being recorded writes cannot be
// read (null). Without the fetch an unguarded held result would be read stale, and a guarded one through a fault.
const uint8_t* cpu_view(uint64_t addr, uint64_t size) {
    if (!addr || !size) return nullptr;
    const PoolHit hit = pool_lookup(addr);
    if (!hit.pool || hit.pool->is_virtual || !hit.pool->base || hit.offset + size > hit.pool->size) return nullptr;
    if (!gw_settled(addr, size)) return nullptr;
    if (out_any()) {
        out_fetch(addr, size, false);
        if (out_overlaps(addr, size)) return nullptr;
    }
    return host_ptr(addr);
}

// An indirect draw Vulkan cannot take as it is (quads, quad strips, polygons and line loops have no topology; 8-bit indices need
// VK_EXT_index_type_uint8): each draw's arguments (and its indices) are read by the CPU and the draw is made as a direct indexed draw of
// the converted 32-bit index list, like DrawArrays / DrawElements. That needs the arguments in memory the CPU can read: arguments a compute
// shader or a copy wrote and no completed submission has handed back are not (skipped, logged). A multi-draw reads its count the same way.
void draw_indirect_cpu(const P_DrawIndirect& p, PrimConversion conv, uint32_t isz, bool indexed) {
    static bool noted = false;
    if (!noted) {
        noted = true;
        NXLOG_I("nvn.replay", "replay: indirect draw by CPU index conversion (first): mode %u, %s, index size %u, %s", p.mode, indexed ? "indexed" : "arrays", isz,
                p.multi ? "multi-draw" : "single");
    }
    const uint64_t cmd_size = indexed ? 20 : 16;
    const uint64_t stride = p.stride > 0 ? static_cast<uint64_t>(p.stride) : cmd_size;
    uint32_t n = 1;
    if (p.multi && p.count_address) {
        const uint8_t* c = cpu_view(p.count_address, 4);
        if (!c) { note_unsupported("indirect draw conversion: the draw count is not readable by the CPU (written by the GPU)"); counters().draws_skipped.fetch_add(1); return; }
        std::memcpy(&n, c, 4);
        n = std::min(n, p.max_count);
    }
    bool drew = false;
    for (uint32_t i = 0; i < n; ++i) {
        const uint8_t* a = cpu_view(p.indirect_address + static_cast<uint64_t>(i) * stride, cmd_size);
        if (!a) { note_unsupported("indirect draw conversion: the arguments are not readable by the CPU (written by the GPU)"); break; }
        uint32_t v[5] = {};
        std::memcpy(v, a, cmd_size);
        const uint32_t count = v[0], instances = v[1];
        if (!count || !instances) continue;
        std::vector<uint32_t> in(count);
        int32_t base_vertex = 0;
        uint32_t base_instance = v[3];
        if (indexed) {
            base_vertex = static_cast<int32_t>(v[3]);
            base_instance = v[4];
            const uint8_t* src = cpu_view(p.index_address + static_cast<uint64_t>(v[2]) * isz, static_cast<uint64_t>(count) * isz);
            if (!src) { note_unsupported("indirect draw conversion: the indices are not readable by the CPU (written by the GPU)"); break; }
            for (uint32_t k = 0; k < count; ++k) {
                uint32_t x = 0;
                if (isz == 1) x = src[k];
                else if (isz == 2) { uint16_t t; std::memcpy(&t, src + k * 2, 2); x = t; }
                else std::memcpy(&x, src + k * 4, 4);
                in[k] = x;
            }
        } else {
            for (uint32_t k = 0; k < count; ++k) in[k] = v[2] + k;
        }
        count_primitives(p.mode, count, instances);
        const std::vector<uint32_t> idx = convert_prim_indices(conv, in);
        if (idx.empty() || !bind_u32_indices(idx)) continue;
        vkCmdDrawIndexed(F->cb, static_cast<uint32_t>(idx.size()), instances, 0, base_vertex, base_instance);
        drew = true;
    }
    if (drew) {
        counters().draws.fetch_add(1, std::memory_order_relaxed);
        g_pass_draws++;
        draw_stamp();
    } else {
        counters().draws_skipped.fetch_add(1);
    }
    trace_snap_after_draw();
}

void do_draw_indirect(const P_DrawIndirect& p) {
    const bool indexed = p.type != 0xffffffffu;
    VkPrimitiveTopology topo;
    if (!begin_draw(static_cast<int>(p.mode), indexed, indexed ? static_cast<int>(p.type) : 0, &topo)) { counters().draws_skipped.fetch_add(1); return; }
    const PrimConversion conv = prim_conversion(static_cast<int>(p.mode));
    const uint32_t isz = indexed ? index_size_bytes(static_cast<int>(p.type)) : 0;
    if (conv != PrimConversion::None || (indexed && isz == 1 && !device().features().index_type_uint8)) {
        draw_indirect_cpu(p, conv, isz, indexed);
        return;
    }
    // GPU path. The primitives of these draws are not known to the CPU: the primitive counters see them through a query window (ResetCounter).
    static bool noted_indirect = false, noted_u8 = false;
    if (!noted_indirect) { noted_indirect = true; NXLOG_I("nvn.replay", "replay: indirect draw (first): mode %u, %s%s", p.mode, indexed ? "indexed" : "arrays", p.multi ? ", multi-draw" : ""); }
    if (indexed && isz == 1 && !noted_u8) { noted_u8 = true; NXLOG_I("nvn.replay", "replay: indirect draw with 8-bit indices (first): VK_EXT_index_type_uint8"); }
    const uint64_t cmd_size = indexed ? 20 : 16;
    const uint64_t n_cmds = p.multi && p.count_address ? std::max<uint64_t>(1, p.max_count) : 1;
    BufRef ib = held_or_pool(p.indirect_address, (p.stride ? static_cast<uint64_t>(p.stride) : cmd_size) * (n_cmds - 1) + cmd_size, kOutIndirect, false);
    if (indexed) {
        BufRef xb = resolve_buffer(p.index_address, 4);
        vkCmdBindIndexBuffer(F->cb, xb.buffer, xb.offset, vk_index_type(static_cast<int>(p.type)));
    }
    if (p.multi && p.count_address) {
        BufRef cb = held_or_pool(p.count_address, 4, kOutIndirect, false);
        const uint32_t stride = static_cast<uint32_t>(p.stride ? p.stride : cmd_size);
        if (indexed) vkCmdDrawIndexedIndirectCount(F->cb, ib.buffer, ib.offset, cb.buffer, cb.offset, p.max_count, stride);
        else vkCmdDrawIndirectCount(F->cb, ib.buffer, ib.offset, cb.buffer, cb.offset, p.max_count, stride);
    } else if (indexed) {
        vkCmdDrawIndexedIndirect(F->cb, ib.buffer, ib.offset, 1, 20);
    } else {
        vkCmdDrawIndirect(F->cb, ib.buffer, ib.offset, 1, 16);
    }
    counters().draws.fetch_add(1, std::memory_order_relaxed);
    g_pass_draws++;
    draw_stamp();
    trace_snap_after_draw();
}

// DrawTransformFeedback(mode, control): draws what the feedback section begun with this control buffer wrote, as many vertices as it wrote
// (vkCmdDrawIndirectByteCountEXT over the section's byte counter, divided by the stride of the vertex stream that reads the feedback buffer). The game
// binds the feedback buffer as an ordinary vertex buffer. The feedback writes are ordered before the vertex fetch by ending the pass first.
// Mesh draws (NVN2 task/mesh programs, VK_EXT_mesh_shader). The work-group count goes to the task stage when the program has one, else to the
// mesh stage. Vulkan has no first work group: a draw with first != 0 is drawn from 0 (counted as a gap). Indirect records stay on the GPU (the
// game's culling compute writes them): {x, y, z} records (the 3D forms) go to vkCmdDrawMeshTasksIndirectEXT as they are; {count, first} records
// (DrawMeshTasksIndirect; VK_NV_mesh_shader's layout, which NVN2's API mirrors: inferred, the driver is not in the game) get their count copied
// into {count, 1, 1} records in the upload ring first (outside the pass).
void do_draw_mesh(const P_DrawMesh& p) {
    if (!device().features().mesh_shader || (S.stage[kTask] && !device().features().task_shader)) {
        nxrt::gap("draw", "mesh draw: the device has no VK_EXT_mesh_shader"); counters().draws_skipped.fetch_add(1); return;
    }
    VkPrimitiveTopology topo;
    if (p.kind <= 1) {
        uint32_t x = p.kind == 0 ? p.y : p.x, y = p.kind == 0 ? 1 : p.y, z = p.kind == 0 ? 1 : p.z;
        if (p.kind == 0 && p.x) nxrt::gap("draw", "mesh draw with a first work group other than 0 (drawn from 0)");
        if (!x || !y || !z) return;
        if (!begin_draw(NVN_DRAW_PRIMITIVE_TRIANGLES, false, 0, &topo, true)) { counters().draws_skipped.fetch_add(1); return; }
        const uint32_t lim = S.stage[kTask] ? device().limits_ext().task_max_groups : device().limits_ext().mesh_max_groups;
        TRACE("    mesh work groups %u x %u x %u (task %s mesh %s)", x, y, z, S.stage[kTask] ? S.stage[kTask]->hash.c_str() : "-", S.stage[kMesh]->hash.c_str());
        vkCmdDrawMeshTasksEXT(F->cb, lim ? std::min(x, lim) : x, y, z);
    } else {
        const uint32_t rec = p.kind == 3 ? 12 : 8;
        const uint64_t stride = p.stride ? static_cast<uint64_t>(p.stride) : rec;
        const uint32_t n = p.multi ? std::max<uint32_t>(1, std::min<uint32_t>(p.max_count, 4096)) : 1;
        const BufRef ib = held_or_pool(p.indirect, stride * (n - 1) + rec, kOutIndirect, false);
        if (!ib.ok) { nxrt::gap("draw", "mesh indirect draw: arguments outside every pool"); counters().draws_skipped.fetch_add(1); return; }
        VkBuffer buf = ib.buffer;
        VkDeviceSize off = ib.offset;
        uint32_t vstride = static_cast<uint32_t>(stride);
        if (p.kind == 2) {   // {count, first} -> {count, 1, 1}
            UploadAlloc u = F->upload(12ull * n, 16);
            if (!u) { counters().draws_skipped.fetch_add(1); return; }
            uint32_t* w = reinterpret_cast<uint32_t*>(u.ptr);
            for (uint32_t i = 0; i < n; ++i) { w[3 * i] = 0; w[3 * i + 1] = 1; w[3 * i + 2] = 1; }
            std::vector<VkBufferCopy> regions(n);
            for (uint32_t i = 0; i < n; ++i) regions[i] = {ib.offset + stride * i, u.offset + 12ull * i, 4};
            end_pass();
            memory_barrier(F->cb);
            vkCmdCopyBuffer(F->cb, ib.buffer, u.buffer, n, regions.data());
            memory_barrier(F->cb);
            buf = u.buffer;
            off = u.offset;
            vstride = 12;
        }
        if (g_trace) {   // a traced frame: the source records and what the draw reads (snapm_d<draw>_<address>_src/_draw.bin)
            end_pass();
            char nm[96];
            const int rs = snapshot_record_buffer(ib.buffer, ib.offset, std::min<uint64_t>(64, ib.avail), *F);
            std::snprintf(nm, sizeof nm, "snapm_d%d_%llx_src.bin", g_trace_n + 1, static_cast<unsigned long long>(p.indirect));
            if (rs >= 0) g_trace_snaps.push_back({rs, g_trace_n + 1, nm, "raw"});
            const int rd = snapshot_record_buffer(buf, off, 12ull * n, *F);
            std::snprintf(nm, sizeof nm, "snapm_d%d_%llx_draw.bin", g_trace_n + 1, static_cast<unsigned long long>(p.indirect));
            if (rd >= 0) g_trace_snaps.push_back({rd, g_trace_n + 1, nm, "raw"});
        }
        if (!begin_draw(NVN_DRAW_PRIMITIVE_TRIANGLES, false, 0, &topo, true)) { counters().draws_skipped.fetch_add(1); return; }
        TRACE("    mesh indirect draw: kind %u, %u record(s) at %llx stride %llu%s (task %s mesh %s)", p.kind, n, static_cast<unsigned long long>(p.indirect),
              static_cast<unsigned long long>(stride), p.count_address ? ", count from the GPU" : "", S.stage[kTask] ? S.stage[kTask]->hash.c_str() : "-", S.stage[kMesh]->hash.c_str());
        if (p.multi && p.count_address) {
            const BufRef cb = held_or_pool(p.count_address, 4, kOutIndirect, false);
            vkCmdDrawMeshTasksIndirectCountEXT(F->cb, buf, off, cb.buffer, cb.offset, n, n > 1 || vstride >= 12 ? vstride : 12);
        } else {
            vkCmdDrawMeshTasksIndirectEXT(F->cb, buf, off, n, n > 1 ? vstride : 12);
        }
    }
    counters().draws.fetch_add(1, std::memory_order_relaxed);
    g_pass_draws++;
    draw_stamp();
    trace_snap_after_draw();
}

void do_draw_xfb(const P_DrawXfb& p) {
    static bool noted = false;
    if (!noted) { noted = true; NXLOG_I("nvn.xfb", "replay: transform feedback draw (first): mode %u control %llx", p.mode, static_cast<unsigned long long>(p.control)); }
    if (!S.render_enable) return;
    if (!device().features().transform_feedback) { note_unsupported("DrawTransformFeedback (the device has no VK_EXT_transform_feedback)"); counters().draws_skipped.fetch_add(1); return; }
    const auto it = g_xfb_slot_of.find(p.control);
    if (!g_xfb_counter || it == g_xfb_slot_of.end() || !g_xfb_slot_written[it->second]) {
        TRACE("DrawTransformFeedback control %llx: the section wrote nothing", static_cast<unsigned long long>(p.control));   // zero bytes: zero vertices
        counters().draws_skipped.fetch_add(1);
        return;
    }
    if (prim_conversion(static_cast<int>(p.mode)) != PrimConversion::None) { note_unsupported("DrawTransformFeedback of a quad/polygon/line-loop primitive"); return; }
    const uint32_t slot = it->second;
    end_pass();
    memory_barrier(F->cb);   // the section's writes (feedback buffer, counter) before the vertex fetch and the indirect read
    VkPrimitiveTopology topo;
    if (!begin_draw(static_cast<int>(p.mode), false, 0, &topo)) { counters().draws_skipped.fetch_add(1); return; }
    uint32_t stride = g_key_attr_count ? S.stream[g_key_binding[0]].stride : 0;
    for (uint32_t b = 0; !stride && b < 16; ++b) stride = S.stream[b].stride;
    if (!stride) { note_unsupported("DrawTransformFeedback with no vertex stream stride"); counters().draws_skipped.fetch_add(1); return; }
    vkCmdDrawIndirectByteCountEXT(F->cb, 1, 0, g_xfb_counter.buffer, static_cast<VkDeviceSize>(slot) * kXfbSlotBytes, 0, stride);
    counters().draws.fetch_add(1, std::memory_order_relaxed);
    g_pass_draws++;
    draw_stamp();
    trace_snap_after_draw();
}

std::string g_cs_missing;   // the untranslated block(s) of the bound compute program ("cs -" dispatches in traces, the upscaler override)

void do_copy_texture_texture(const P_CopyTexTex& p);

// The game's own upscaler replaced on the PC (game modules set these as defaults, game::Info::config_defaults). OoT renders the 3D scene at a
// low internal size and builds the output with a chain of compute dispatches, one of them a small neural network on the Switch 2's tensor
// cores; on a PC GPU that is neither needed nor cheap to emulate, and the chain reads its resources through the per-program c[0] table the
// runtime does not know (A10). [nvn] upscaler_skip = program hash prefixes (comma list): those dispatches are left out. [nvn] upscaler_replace
// = the program whose dispatch starts the chain: it becomes a linearly filtered scaled copy from [nvn] upscaler_src to [nvn] upscaler_dst.
// Source: texN / imgN (a compute binding; default tex0) or depth_rt = colour target 0 of the latest draw that had a depth target (the 3D
// scene: OoT's last scene draw renders into the HDR target the network pass reads). Destination: imgN (a compute image binding) or
// fs:<hash prefix>:texN = the texture the LAST draw of the previous frame with that fragment shader sampled at binding N (the frame's final
// composite reads the upscaler's output; earlier draws with the same shader in a frame do not count). Empty upscaler_replace: off.
// [nvn] upscaler_fallback (OoT default on): the copy stands in only while a pass of the chain cannot run (its pipeline is still building in
// the background, its shader is waiting for translation or untranslated); once every pass runs, the game's own chain does (A10/A12: the
// chain gives the sharper, anti-aliased picture at ~2 ms of GPU time on an RTX 4080). The fragment shader's network pipeline fea98fe7 takes
// 2-3 minutes to build the first time: without the fallback the 3D scene would stay black that long.
struct UpscalerOverride {
    std::vector<std::string> skip;
    std::string replace, dst_fs;
    bool src_img = false, src_depth_rt = false;
    bool fallback = false;
    int src = 0, dst = 0;
};

const UpscalerOverride& upscaler_override() {
    static const UpscalerOverride u = [] {
        UpscalerOverride o;
        o.replace = nxrt::config_get_str("nvn", "upscaler_replace", "");
        // "off" (or none / 0) turns a game module's default off: an empty environment variable does not exist on Windows
        if (o.replace == "off" || o.replace == "none" || o.replace == "0") {
            NXLOG_I(kTag, "upscaler override: off ([nvn] upscaler_replace = %s)", o.replace.c_str());
            o.replace.clear();
            return o;
        }
        std::string list = nxrt::config_get_str("nvn", "upscaler_skip", "");
        for (size_t at = 0; at < list.size();) {
            size_t end = list.find_first_of(", ", at);
            if (end == std::string::npos) end = list.size();
            if (end > at) o.skip.push_back(list.substr(at, end - at));
            at = end + 1;
        }
        auto slot = [](const std::string& s, bool* img) {
            if (img) *img = s.rfind("img", 0) == 0;
            const size_t d = s.find_last_not_of("0123456789");
            return d + 1 < s.size() ? std::atoi(s.c_str() + d + 1) : 0;
        };
        const std::string src = nxrt::config_get_str("nvn", "upscaler_src", "tex0");
        o.src_depth_rt = src == "depth_rt";
        o.src = slot(src, &o.src_img);
        const std::string dst = nxrt::config_get_str("nvn", "upscaler_dst", "img0");
        if (dst.rfind("fs:", 0) == 0) {
            const size_t colon = dst.find(':', 3);
            o.dst_fs = dst.substr(3, colon == std::string::npos ? std::string::npos : colon - 3);
            o.dst = colon == std::string::npos ? 0 : slot(dst.substr(colon + 1), nullptr);
        } else {
            o.dst = slot(dst, nullptr);
        }
        o.fallback = nxrt::config_get_bool("nvn", "upscaler_fallback", false);
        if (!o.replace.empty())
            NXLOG_I(kTag, "upscaler override: %zu pass(es) left out, %s becomes a scaled copy %s -> %s%s", o.skip.size(), o.replace.c_str(),
                    src.c_str(), dst.c_str(), o.fallback ? " while a pass of the chain cannot run (upscaler_fallback); the game's chain otherwise" : "");
        return o;
    }();
    return u;
}

std::atomic<bool> g_upscaler_on{true};  // the F1 menu's live switch (nvn_backend.h set_upscaler_replace_enabled); off: the game's own passes run
uint64_t g_upscaler_dst_handle = 0;    // upscaler_dst = fs:...: what the last such draw of the previous frame sampled
uint64_t g_upscaler_dst_pending = 0;   // ... and of this frame so far (promoted once a present has passed, upscaler_frame_check)
uint64_t g_upscaler_pending_present = 0;
Ref<Texture> g_last_depth_rt;          // upscaler_src = depth_rt: colour target 0 of the latest draw with a depth target

// A present since the pending destination was noted: it becomes the destination of the frames that follow.
void upscaler_frame_check() {
    const uint64_t pr = counters().presents.load(std::memory_order_relaxed);
    if (pr == g_upscaler_pending_present) return;
    if (g_upscaler_dst_pending) g_upscaler_dst_handle = g_upscaler_dst_pending;
    g_upscaler_dst_pending = 0;
    g_upscaler_pending_present = pr;
}

// begin_draw: remember the scene target and the texture the upscaler's output goes to.
void upscaler_note_draw(const ShaderInfo* fs) {
    const UpscalerOverride& u = upscaler_override();
    if (u.replace.empty() || !g_upscaler_on.load(std::memory_order_relaxed)) return;
    upscaler_frame_check();
    if (u.src_depth_rt && S.depth_tex && S.color_tex[0] && S.color_tex[0].get() != g_last_depth_rt.get()) g_last_depth_rt = S.color_tex[0];
    if (u.dst_fs.empty() || !fs || fs->hash.compare(0, u.dst_fs.size(), u.dst_fs) != 0) return;
    if (const uint64_t h = S.tex[kFragment][u.dst & 63]) g_upscaler_dst_pending = h;
}


uint64_t g_chain_broken_present = ~0ull;   // upscaler_fallback: the present count when a pass of the chain last could not run

// upscaler_fallback: can the bound compute program run now? Asking for its pipeline starts a background build that has not started yet.
bool chain_pass_ready() {
    const ShaderInfo* cs = S.stage[kCompute];
    if (!cs || g_cs_pending) return false;
    const ShaderInfo* stages[kStages] = {nullptr, nullptr, nullptr, nullptr, nullptr, cs, nullptr, nullptr};
    const ProgLayout* pl = prog_layout(stages);
    return pl && compute_pipeline(cs, *pl) != VK_NULL_HANDLE;
}

// True when the dispatch belongs to the replaced upscaler (and is handled: left out, or turned into the copy).
bool upscaler_dispatch(const std::string& hash) {
    const UpscalerOverride& u = upscaler_override();
    if (u.replace.empty() || hash.empty() || !g_upscaler_on.load(std::memory_order_relaxed)) return false;
    auto matches = [&](const std::string& prefix) { return !prefix.empty() && hash.compare(0, prefix.size(), prefix) == 0; };
    bool in_chain = matches(u.replace);
    for (const std::string& s : u.skip) in_chain = in_chain || matches(s);
    if (!in_chain) return false;
    if (u.fallback) {
        // the copy stands in for this frame and the next after a pass of the chain could not run; the game's chain runs otherwise
        const uint64_t pr = counters().presents.load(std::memory_order_relaxed);
        if (!chain_pass_ready()) {
            if (g_chain_broken_present == ~0ull || g_chain_broken_present + 1 < pr)
                NXLOG_I(kTag, "upscaler fallback: %s cannot run yet (present %llu): the scaled copy stands in for the game's chain", hash.c_str(),
                        static_cast<unsigned long long>(pr));
            g_chain_broken_present = pr;
        }
        const bool copy_mode = g_chain_broken_present != ~0ull && g_chain_broken_present + 1 >= pr;
        static uint64_t resumed_at = ~0ull;    // the present the chain last took over again (logged once per fallback period)
        if (!copy_mode) {
            if (g_chain_broken_present != ~0ull && resumed_at != g_chain_broken_present) {
                resumed_at = g_chain_broken_present;
                NXLOG_I(kTag, "upscaler fallback: every pass of the chain runs again (present %llu, the copy stood in last at present %llu)",
                        static_cast<unsigned long long>(pr), static_cast<unsigned long long>(g_chain_broken_present));
            }
            return false;
        }
    }
    if (matches(u.replace)) {
        upscaler_frame_check();
        Texture* src = u.src_depth_rt ? g_last_depth_rt.get() : pool_texture(u.src_img ? S.img[kCompute][u.src & 15] : S.tex[kCompute][u.src & 63]);
        Texture* dst = pool_texture(u.dst_fs.empty() ? S.img[kCompute][u.dst & 15] : g_upscaler_dst_handle);
        if (!src || !dst || same_image(src, dst)) {
            if (first_time_key("upscaler-unbound"))
                NXLOG_W(kTag, "upscaler override: %s: no source or output yet (%p, %p): left out", hash.c_str(), static_cast<void*>(src), static_cast<void*>(dst));
            return true;
        }
        P_CopyTexTex p = {};
        p.src = src;
        p.dst = dst;
        p.flags = NVN_COPY_FLAGS_LINEAR_FILTER_BIT;
        char a[96], b[96];
        TRACE("    upscaler override: copy %s -> %s", tex_desc(src, a, sizeof a), tex_desc(dst, b, sizeof b));
        if (first_time_key("upscaler-copy")) NXLOG_I(kTag, "upscaler override: first copy %s -> %s", tex_desc(src, a, sizeof a), tex_desc(dst, b, sizeof b));
        do_copy_texture_texture(p);
        return true;
    }
    for (const std::string& s : u.skip)
        if (matches(s)) return true;
    return false;
}

// [nvn] pass_copy = <hash prefix>:<src>:<dst>[,...]: each dispatch of such a compute program becomes a linearly filtered scaled copy from its
// binding src (texN / imgN) to its image binding dst (imgN), whatever else the upscaler override does. For passes whose translation is known
// to be wrong, so the picture stays usable (OoT: the network variants f90d726d / 02dcb888 the game picks at internal scales 0.75 / 0.667).
struct PassCopy {
    std::string prefix;
    bool src_img = false;
    int src = 0, dst = 0;
};

const std::vector<PassCopy>& pass_copies() {
    static const std::vector<PassCopy> v = [] {
        std::vector<PassCopy> out;
        const std::string list = nxrt::config_get_str("nvn", "pass_copy", "");
        for (size_t at = 0; at < list.size();) {
            size_t end = list.find_first_of(", ", at);
            if (end == std::string::npos) end = list.size();
            const std::string item = list.substr(at, end - at);
            at = end + 1;
            const size_t c1 = item.find(':'), c2 = c1 == std::string::npos ? c1 : item.find(':', c1 + 1);
            if (c2 == std::string::npos) continue;
            PassCopy p;
            p.prefix = item.substr(0, c1);
            const std::string s = item.substr(c1 + 1, c2 - c1 - 1), d = item.substr(c2 + 1);
            p.src_img = s.rfind("img", 0) == 0;
            p.src = std::atoi(s.c_str() + 3);   // "texN" / "imgN"
            p.dst = std::atoi(d.c_str() + 3);
            if (p.prefix.empty() || d.rfind("img", 0) != 0) continue;
            NXLOG_I(kTag, "pass copy: %s becomes a scaled copy %s -> %s ([nvn] pass_copy)", p.prefix.c_str(), s.c_str(), d.c_str());
            out.push_back(p);
        }
        return out;
    }();
    return v;
}

bool pass_copy_dispatch(const std::string& hash) {
    const auto& v = pass_copies();
    if (v.empty() || hash.empty()) return false;
    for (const PassCopy& p : v) {
        if (hash.compare(0, p.prefix.size(), p.prefix) != 0) continue;
        Texture* src = pool_texture(p.src_img ? S.img[kCompute][p.src & 15] : S.tex[kCompute][p.src & 63]);
        Texture* dst = pool_texture(S.img[kCompute][p.dst & 15]);
        if (!src || !dst || same_image(src, dst)) return true;
        P_CopyTexTex c = {};
        c.src = src;
        c.dst = dst;
        c.flags = NVN_COPY_FLAGS_LINEAR_FILTER_BIT;
        char a[96], b[96];
        TRACE("    pass copy: %s -> %s", tex_desc(src, a, sizeof a), tex_desc(dst, b, sizeof b));
        if (first_time_key(("pass-copy-" + p.prefix).c_str()))
            NXLOG_I(kTag, "pass copy: first copy for %s: %s -> %s", hash.c_str(), tex_desc(src, a, sizeof a), tex_desc(dst, b, sizeof b));
        do_copy_texture_texture(c);
        return true;
    }
    return false;
}

// Statistics: dispatches recorded right after another one with no barrier between them but the one after every dispatch (the game
// ordered nothing between them; memory_barriers_here() after the last dispatch).
uint64_t g_disp_end_mark = ~0ull;
uint64_t g_disp_n = 0, g_disp_after = 0, g_disp_after_ind = 0;

void do_dispatch(uint32_t x, uint32_t y, uint32_t z, uint64_t indirect) {
    ++g_disp_n;
    if (!P.open && memory_barriers_here() == g_disp_end_mark) {   // nothing barriered since the last dispatch's own barrier: the game ordered nothing between them
        ++g_disp_after;
        if (indirect) ++g_disp_after_ind;
    }
    const ShaderInfo* cs = S.stage[kCompute];
    TRACE("dispatch %u %u %u indirect %llx cs %s%s%s", x, y, z, static_cast<unsigned long long>(indirect), cs ? cs->hash.c_str() : "-",
          !cs && !g_cs_missing.empty() ? " missing " : "", !cs ? g_cs_missing.c_str() : "");
    if (g_trace) {   // what the game bound for compute, by NVN slot (independent of how the shader finds it in c[0]): textures, images, storage
        char a[96];
        for (int k = 0; k < 64; ++k)
            if (S.tex[kCompute][k]) TRACE("    cs bound tex%d %llx -> %s", k, static_cast<unsigned long long>(S.tex[kCompute][k]), tex_desc(pool_texture(S.tex[kCompute][k]), a, sizeof a));
        for (int k = 0; k < 16; ++k)
            if (S.img[kCompute][k]) TRACE("    cs bound img%d %llx -> %s", k, static_cast<unsigned long long>(S.img[kCompute][k]), tex_desc(pool_texture(S.img[kCompute][k]), a, sizeof a));
        for (int k = 0; k < 16; ++k)
            if (S.ssbo[kCompute][k].addr) TRACE("    cs bound ssbo%d %llx/%llu", k, static_cast<unsigned long long>(S.ssbo[kCompute][k].addr), static_cast<unsigned long long>(S.ssbo[kCompute][k].size));
        for (int k = 0; k < 16; ++k)
            if (S.ubo[kCompute][k].addr) TRACE("    cs bound ubo%d %llx/%llu", k, static_cast<unsigned long long>(S.ubo[kCompute][k].addr), static_cast<unsigned long long>(S.ubo[kCompute][k].size));
        if (cs && cs->control.nvn2) {   // the program's c[0] table (apply_nvn2_compute_table) and the compute push area it reads
            const ShaderControl& c = cs->control;
            std::string words;
            for (uint32_t i = 0; i * 4 < c.c0_push; ++i) { char t[12]; uint32_t w; std::memcpy(&w, S.small.push_cs + 4 * i, 4); std::snprintf(t, sizeof t, " %08x", w); words += t; }
            TRACE("    cs c0 table 0x%x: push 0x%x (%u words read, last compute push stage %u) ubos %u ssbos %u textures %u images %u%s%s", c.c0_table, c.c0_push, c.c0_push_words,
                  S.small.push_cs_stage, c.c0_ubos, c.c0_ssbos, c.c0_textures, c.c0_images, words.empty() ? "" : "; push words:", words.c_str());
        }
    }
    if (pass_copy_dispatch(cs ? cs->hash : g_cs_missing)) return;   // [nvn] pass_copy
    if (upscaler_dispatch(cs ? cs->hash : g_cs_missing)) return;   // [nvn] upscaler_replace: the game's upscaler, replaced
    if (g_cs_pending) { note_pending_skip(true); return; }
    if (cs && !g_skip_fs.empty())   // measurement only: skip_fs_file words "c:<prefix>" leave out dispatches of that compute shader
        for (const std::string& p : g_skip_fs)
            if (p.size() > 2 && p.compare(0, 2, "c:") == 0 && cs->hash.compare(0, p.size() - 2, p, 2, std::string::npos) == 0) return;
    if (!cs) { if (!g_cs_missing.empty()) nxrt::gap("dispatch", "skipped: untranslated compute shader", g_cs_missing); return; }
    static const bool debug_skip = config_get_bool("nvn", "debug_skip_compute", false);   // diagnosis: no dispatch runs
    if (debug_skip) return;
    const ShaderInfo* stages[kStages] = {nullptr, nullptr, nullptr, nullptr, nullptr, cs, nullptr, nullptr};
    const ProgLayout* pl = prog_layout(stages);
    if (!pl) return;
    // A storage buffer whose c[0] descriptor maps to no NVN binding would be the shared 64 KiB dummy, which the shader may write (it also
    // holds the default vertex attributes) or index out of range. NVN2 compute programs find theirs through the program's c[0] table
    // (apply_nvn2_compute_table: header, push constants, uniform block and storage buffer descriptors); a descriptor outside it is a layout
    // not understood yet. [nvn] skip_unmapped_dispatch (default on) leaves such dispatches out as a fallback.
    static const bool skip_unmapped = config_get_bool("nvn", "skip_unmapped_dispatch", true);
    if (skip_unmapped)
        for (const BindingDesc& d : pl->bindings)
            if (d.type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER && d.shader->refl.bindings[d.index].nvn.source == NvnSource::Unknown) {
                if (first_time_key("unmapped-dispatch " + cs->hash))
                    NXLOG_W(kTag, "compute shader %s: storage buffer %s (c0 offset 0x%x) maps to no NVN binding (c[0] table 0x%x: push 0x%x, %u ubos, %u ssbos): "
                            "its dispatches are left out", cs->hash.c_str(), d.shader->refl.bindings[d.index].name.c_str(), d.shader->refl.bindings[d.index].nvn.c0_offset,
                            cs->control.c0_table, cs->control.c0_push, cs->control.c0_ubos, cs->control.c0_ssbos);
                note_unsupported("dispatch with an unmapped storage buffer");
                return;
            }
    prepare_textures(*pl);
    end_pass();
    gmem_before_op(true);   // A17: held results back, ranges its pointers name written
    // [nvn] cs_gmem_flush: a compute pointer shader that only reads through pointers gets the results held over its storage bindings sent
    // back first, so no slot needs checking before the page table (cs_gmem_held: mask 0, the shader's inline fast path).
    if (cs->refl.global_fallback && cs->refl.global_pt && !cs->refl.global_writes && cs_gmem_held_on() && cs_gmem_flush_on() && out_any())
        for (int k = 0; k < 16 && k < cs->control.c0_ssbos; ++k) {
            const GState::Bind& b = S.ssbo[kCompute][k];
            if (b.addr && b.size) compute_out_guard(b.addr, b.size, kOutGlobal);
        }
    if (g_trace) {   // the compute bindings: buffers (source, slot, address), texel buffers and textures, and the first words of each uniform block
        for (const BindingDesc& d : pl->bindings) {
            const DescriptorBinding& b = d.shader->refl.bindings[d.index];
            if (d.type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || d.type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
                const int slot = b.nvn.source == NvnSource::StorageBuffer ? (b.nvn.index >= 0 ? b.nvn.index : storage_slot_for(d.nvn_stage, b.nvn.c0_offset)) : b.nvn.index;
                const bool ssbo = b.nvn.source == NvnSource::StorageBuffer;
                const GState::Bind* bd = (slot >= 0 && slot < 16) ? (ssbo ? &S.ssbo[d.nvn_stage][slot] : &S.ubo[d.nvn_stage][slot]) : nullptr;
                std::string words;
                if (bd && bd->addr && host_readable(bd->addr, std::min<uint64_t>(bd->size, 512))) {
                    const uint32_t* w = reinterpret_cast<const uint32_t*>(host_ptr(bd->addr));
                    for (uint32_t i = 0; i < 128 && i * 4 < bd->size; ++i) { char t[16]; std::snprintf(t, sizeof t, " %08x", w[i]); words += t; }
                }
                TRACE("    cs b%u %s src %d slot %d c0 %d addr %llx/%llu:%s", d.binding, b.name.c_str(), static_cast<int>(b.nvn.source), slot, b.nvn.c0_offset,
                      bd ? static_cast<unsigned long long>(bd->addr) : 0ull, bd ? static_cast<unsigned long long>(bd->size) : 0ull, words.c_str());
            } else {
                const bool img = d.type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE || d.type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
                const TexResolved r = img ? resolve_image(d) : resolve_texture(d);
                char c[96];
                std::string words;
                if (r.tex && r.tex->mem && host_readable(reinterpret_cast<uint64_t>(r.tex->mem), 256)) {
                    const uint32_t* w = reinterpret_cast<const uint32_t*>(r.tex->mem);
                    for (uint32_t i = 0; i < 64; ++i) { char t[16]; std::snprintf(t, sizeof t, " %08x", w[i]); words += t; }
                }
                TRACE("    cs b%u %s type %d -> %s @%p:%s", d.binding, b.name.c_str(), static_cast<int>(d.type), tex_desc(r.tex, c, sizeof c), r.tex ? static_cast<void*>(r.tex->mem) : nullptr, words.c_str());
            }
        }
    }
    VkPipeline pipe = compute_pipeline(cs, *pl);
    if (!pipe) return;
    out_delta_block(true);   // copy-backs recorded until the dispatch must not bind the delta shader over this pipeline and its sets
    vkCmdBindPipeline(F->cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    g_bound_pipeline = VK_NULL_HANDLE;
    support_for_draw(*pl, true);
    const BufRef ind = indirect ? held_or_pool(indirect, 12, kOutIndirect, true) : BufRef{};   // arguments a dispatch wrote: from the copy
    g_shadowing = compute_shadows_on();
    g_shadow_uses.clear();
    g_sh_barrier = g_sh_copied = false;
    g_sh_kinds = 0;
    // The work-group counts for the c[0] header (fill_compute_c0). An indirect dispatch's arguments as the CPU sees them (a dispatch
    // that wrote them on the GPU this frame is not visible here: then the header holds the values of the last host copy, or 1s).
    g_dispatch_groups[0] = x; g_dispatch_groups[1] = y; g_dispatch_groups[2] = z;
    if (indirect) {
        if (host_readable(indirect, 12)) std::memcpy(g_dispatch_groups, host_ptr(indirect), 12);
        else g_dispatch_groups[0] = g_dispatch_groups[1] = g_dispatch_groups[2] = 1;
    }
    g_c0_made = {};
    bind_sets(*pl, VK_PIPELINE_BIND_POINT_COMPUTE);
    if (indirect && g_c0_made.buffer && ind.ok) {
        // The CPU's copy of the arguments is stale when a dispatch of this frame wrote them (GPU-driven culling chains: OoT's mesh draws are
        // counted by compute 328a6c18, which is itself dispatched indirectly): the header gets the counts the GPU dispatches with.
        end_pass();
        memory_barrier(F->cb);
        const VkBufferCopy rg = {ind.offset, g_c0_made.offset + 4, 12};
        vkCmdCopyBuffer(F->cb, ind.buffer, g_c0_made.buffer, 1, &rg);
        memory_barrier(F->cb);
        g_sh_kinds |= 16;
    }
    if (g_sh_copied) memory_barrier(F->cb);     // the copies into the shadows before the dispatch reads them
    if ((g_sh_copied || g_sh_kinds) && gpass_on()) {
        char k[8] = {};
        int n = 0;
        if (g_sh_kinds & 1) k[n++] = 'f';
        if (g_sh_kinds & 2) k[n++] = 'p';
        if (g_sh_kinds & 4) k[n++] = 's';
        if (g_sh_kinds & 8) k[n++] = 'm';
        if (g_sh_kinds & 16) k[n++] = 'i';
        gstamp(("shadow in " + cs->hash.substr(0, 8) + " " + k).c_str());
    }
    if (checkpoints_on()) {
        char t[128];
        std::snprintf(t, sizeof t, "dispatch cs %s %ux%ux%u%s", cs->hash.c_str(), x, y, z, indirect ? " indirect" : "");
        checkpoint(*F, t);
    }
    if (indirect && g_trace && ind.ok) {   // a traced frame: the arguments as the GPU dispatches them (snapd_<n>_<cs>.bin)
        static int nd = 0;
        static uint64_t nd_present = ~0ull;   // numbered per frame, so trace_snap_per_frame folders of several frames compare by name
        if (counters().presents.load() != nd_present) { nd_present = counters().presents.load(); nd = 0; }
        const int ri = snapshot_record_buffer(ind.buffer, ind.offset, 12, *F);
        char nm[96];
        std::snprintf(nm, sizeof nm, "snapd_%03d_%s.bin", nd++, cs->hash.substr(0, 8).c_str());
        if (ri >= 0) g_trace_snaps.push_back({ri, g_trace_n, nm, "raw"});
    }
    if (indirect) {
        vkCmdDispatchIndirect(F->cb, ind.buffer, ind.offset);
        if (gpass_on()) gstamp(("cs " + cs->hash.substr(0, 8) + " indirect").c_str());
    } else {
        vkCmdDispatch(F->cb, x, y, z);
        if (gpass_on()) gstamp(("cs " + cs->hash.substr(0, 8)).c_str());
    }
    out_delta_block(false);
    shadows_after_dispatch();
    out_presnap_end();
    memory_barrier(F->cb);
    g_disp_end_mark = memory_barriers_here();
    ++g_chunk_work;
    if (g_trace) {
        // [nvn] trace_snap_after = "4c1e8564:796bc6d880+256,796bc82000+64": in a traced frame, after each dispatch of a compute program whose
        // hash starts with the prefix, the given guest ranges as the GPU holds them (the pool buffer behind each address; a held or shadow copy
        // is not followed): snapa_<n>_<cs>_<address>.bin. For memory a shader reaches through pointers (A17), which trace_snap_ssbo misses.
        static const std::string spec = nxrt::config_get_str("nvn", "trace_snap_after", "");
        const size_t colon = spec.find(':');
        // Several prefixes separated by '|', "*" matches every compute program (A18: which dispatch of a chain first writes a varying
        // value); the numbers restart each present, so trace_snap_per_frame folders of several frames compare by name.
        bool match = false;
        for (size_t q = 0; colon != std::string::npos && q < colon && !match;) {
            size_t qe = spec.find('|', q);
            if (qe == std::string::npos || qe > colon) qe = colon;
            match = (qe - q == 1 && spec[q] == '*') || (qe > q && cs->hash.compare(0, qe - q, spec, q, qe - q) == 0);
            q = qe + 1;
        }
        if (match) {
            static int na = 0;
            static uint64_t na_present = ~0ull;
            if (counters().presents.load() != na_present) { na_present = counters().presents.load(); na = 0; }
            const int n_this = na++;
            size_t p = colon + 1;
            while (p < spec.size()) {
                size_t e = spec.find(',', p);
                if (e == std::string::npos) e = spec.size();
                const std::string item = spec.substr(p, e - p);
                p = e + 1;
                const size_t plus = item.find('+');
                uint64_t a = std::strtoull(item.c_str(), nullptr, 16);
                const uint64_t sz = plus != std::string::npos ? std::strtoull(item.c_str() + plus + 1, nullptr, 0) : 256;
                // "u<slot>@<hex offset>": the range starts at the 64-bit pointer stored in compute uniform block <slot> at that offset (as the CPU
                // holds it when the dispatch is recorded): game addresses of GPU records change from run to run, their pointer tables do not
                if (item.size() > 2 && item[0] == 'u' && item.find('@') != std::string::npos) {
                    const int slot = std::atoi(item.c_str() + 1) & 15;
                    const uint64_t off = std::strtoull(item.c_str() + item.find('@') + 1, nullptr, 16);
                    const GState::Bind& ub = S.ubo[kCompute][slot];
                    a = 0;
                    if (ub.addr && off + 8 <= ub.size && host_readable(ub.addr + off, 8)) std::memcpy(&a, host_ptr(ub.addr + off), 8);
                    TRACE("    snap after %s: %s -> %llx", cs->hash.c_str(), item.c_str(), static_cast<unsigned long long>(a));
                    if (!a) continue;
                }
                const BufRef br = resolve_buffer(a, sz);
                TRACE("    snap after %s: %llx+%llu %s", cs->hash.c_str(), static_cast<unsigned long long>(a), static_cast<unsigned long long>(sz),
                      br.ok ? "in a pool" : "IN NO POOL");
                if (!br.ok) continue;
                const int ri = snapshot_record_buffer(br.buffer, br.offset, std::min<VkDeviceSize>(sz, br.avail), *F);
                char nm[128];
                std::snprintf(nm, sizeof nm, "snapa_%03d_%s_%llx.bin", n_this, cs->hash.substr(0, 8).c_str(), static_cast<unsigned long long>(a));
                if (ri >= 0) g_trace_snaps.push_back({ri, g_trace_n, nm, "raw"});
            }
        }
    }
    g_dirty = kAllDirty;
    counters().dispatches.fetch_add(1, std::memory_order_relaxed);
}

// ---- conditional render ---------------------------------------------------------------------------------------------------------
void note_report(uint64_t address, uint64_t counter) {
    if (!address) return;
    if (g_reports.size() > 65536) g_reports.clear();   // bounded: a forgotten address reads from memory again
    g_reports[address] = counter;
}

// The counter word at a report address; false when it is not known before the GPU runs.
bool report_counter(uint64_t address, uint64_t* out) {
    auto it = g_reports.find(address);
    if (it != g_reports.end()) {
        if (it->second == kReportUnknown) return false;
        *out = it->second;
        return true;
    }
    const PoolHit hit = pool_lookup(address);
    if (!hit.pool || hit.pool->is_virtual || !hit.pool->base || hit.offset + 8 > hit.pool->size) return false;
    std::memcpy(out, host_ptr(address), 8);
    return true;
}

bool conditional_render_enabled(uint32_t mode, uint64_t address) {
    uint64_t a = 0, b = 0;
    if (!report_counter(address, &a) || !report_counter(address + 16, &b)) return true;
    if (mode == NVN_CONDITIONAL_RENDER_MODE_RENDER_IF_EQUAL) return a == b;
    if (mode == NVN_CONDITIONAL_RENDER_MODE_RENDER_IF_NOT_EQUAL) return a != b;
    return true;
}

// ---- clears -------------------------------------------------------------------------------------------------------------------
VkClearRect full_rect() {
    VkClearRect r = {};
    r.rect = S.nsc ? scissor_rect(S.sc[0]) : VkRect2D{{0, 0}, P.extent};
    r.baseArrayLayer = 0;
    r.layerCount = 1;
    return r;
}

void do_clear_color(const P_ClearColor& p) {
    if (g_trace) {   // the value (as floats for kind 0, else as words); "rect" is taken before the clear's pass opens, clamped to the previous pass's
                     // extent (a 854x480 target after a 240x135 pass shows 240x135): the "clear applied" line after it has the rectangle really cleared
        char a[96], v[96];
        if (p.kind == 0) {
            float f[4];
            std::memcpy(f, p.value, sizeof f);
            std::snprintf(v, sizeof v, "%g %g %g %g", f[0], f[1], f[2], f[3]);
        } else {
            std::snprintf(v, sizeof v, "%08x %08x %08x %08x", p.value[0], p.value[1], p.value[2], p.value[3]);
        }
        const VkClearRect cr = full_rect();
        TRACE("clear color rt%u mask %x kind %u %s value %s rect %d %d %u %u", p.target, p.mask, p.kind, tex_desc(p.target < 8 ? S.color_tex[p.target].get() : nullptr, a, sizeof a), v,
              cr.rect.offset.x, cr.rect.offset.y, cr.rect.extent.width, cr.rect.extent.height);
    }
    if (!S.render_enable) return;   // render enable gates clears as it gates draws (yuzu DrawManager::Clear)
    if (p.target >= 8 || !S.color_tex[p.target]) return;
    if (p.mask == 0) return;
    if (!ensure_pass()) return;
    if (!P.color_fmt[p.target]) return;
    if (p.mask != 0xf) {
        // vkCmdClearAttachments ignores the colour write mask: a masked clear is a draw with that mask
        if (!g_mask_ready) { g_mask_ok = g_mask.init(device()); g_mask_ready = true; }
        const VkClearRect cr = full_rect();
        if (g_mask_ok && g_mask.record(F->cb, P.color_fmt[p.target], P.ncolor, p.target, p.mask, p.kind, p.value, cr.rect, P.samples)) {
            static bool noted = false;
            if (!noted) { noted = true; NXLOG_I("nvn.replay", "replay: ClearColor with a partial channel mask (first): rt%u mask %x, drawn with that colour write mask", p.target, p.mask); }
            counters().clears.fetch_add(1, std::memory_order_relaxed);
            g_bound_pipeline = VK_NULL_HANDLE;
            g_dirty = kAllDirty;
            g_topo = VK_PRIMITIVE_TOPOLOGY_MAX_ENUM;   // binding a pipeline without these dynamic states invalidates them
            g_restart_valid = false;
            return;
        }
        note_unsupported("ClearColor with a partial channel mask (all channels are cleared)");
    }
    VkClearAttachment ca = {};
    ca.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    ca.colorAttachment = p.target;
    std::memcpy(ca.clearValue.color.uint32, p.value, 16);
    VkClearRect r = full_rect();
    // the rectangle the clear covers in the pass it opened (the "rect" of the line above is taken before that pass, with the old extent)
    TRACE("    clear applied: rect %d %d %u %u of %ux%u (scissor %d %d %d %d, %u set)", r.rect.offset.x, r.rect.offset.y, r.rect.extent.width, r.rect.extent.height,
          P.extent.width, P.extent.height, S.sc[0][0], S.sc[0][1], S.sc[0][2], S.sc[0][3], S.nsc);
    vkCmdClearAttachments(F->cb, 1, &ca, 1, &r);
    counters().clears.fetch_add(1, std::memory_order_relaxed);
}

void do_clear_depth_stencil(const P_ClearDepthStencil& p) {
    TRACE("clear depth/stencil d %g (mask %d) s %d (mask %x)", p.depth, p.depth_mask, p.stencil, p.stencil_mask);
    if (!S.render_enable) return;
    if (!S.depth_tex) return;
    if (!ensure_pass()) return;
    VkClearAttachment ca = {};
    if (p.depth_mask) ca.aspectMask |= VK_IMAGE_ASPECT_DEPTH_BIT;
    if (p.stencil_mask && P.has_stencil) ca.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
    if (!ca.aspectMask) return;
    if (p.stencil_mask && p.stencil_mask != 0xff && P.has_stencil && !P.depth_ro) {
        // vkCmdClearAttachments has no stencil write mask: a masked stencil clear is a draw with that write mask (depth goes along)
        if (!g_mask_ready) { g_mask_ok = g_mask.init(device()); g_mask_ready = true; }
        const VkClearRect cr = full_rect();
        if (g_mask_ok && g_mask.record_depth_stencil(F->cb, P.color_fmt, P.ncolor, P.depth_fmt, true, P.samples, p.depth_mask != 0, p.depth, p.stencil_mask,
                                                    static_cast<uint32_t>(p.stencil), cr.rect)) {
            static bool noted = false;
            if (!noted) { noted = true; NXLOG_I("nvn.replay", "replay: ClearDepthStencil with a partial stencil mask (first): mask %x, drawn with that stencil write mask", p.stencil_mask); }
            counters().clears.fetch_add(1, std::memory_order_relaxed);
            g_bound_pipeline = VK_NULL_HANDLE;
            g_dirty = kAllDirty;
            g_topo = VK_PRIMITIVE_TOPOLOGY_MAX_ENUM;   // binding a pipeline without these dynamic states invalidates them
            g_restart_valid = false;
            return;
        }
        note_unsupported("ClearDepthStencil with a partial stencil mask (the whole stencil is cleared)");
    }
    ca.clearValue.depthStencil = {p.depth, static_cast<uint32_t>(p.stencil)};
    VkClearRect r = full_rect();
    vkCmdClearAttachments(F->cb, 1, &ca, 1, &r);
    counters().clears.fetch_add(1, std::memory_order_relaxed);
}

// ---- copies ------------------------------------------------------------------------------------------------------------------
struct SubRes { uint32_t level = 0, layer = 0, layers = 1; };
SubRes sub_of(const TextureViewInfo& v, bool has_view) {
    SubRes s;
    if (has_view) {
        if (v.has_levels) s.level = static_cast<uint32_t>(v.min_level);
        if (v.has_layers) { s.layer = static_cast<uint32_t>(v.min_layer); s.layers = 1; }
    }
    return s;
}

void do_clear_texture(const P_ClearTexture& p) {
    Texture* t = p.tex;
    if (!t || !tex_gpu(t)) return;
    end_pass();
    SubRes s = sub_of(p.view, p.has_view);
    tex_ensure_current(t, *F);
    tex_transition(t, *F, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    tex_mark_gpu_written(t);
    if (p.has_region) {
        const VkExtent3D e = tex_extent(t, s.level);
        if (p.region.x != 0 || p.region.y != 0 || (p.region.w && static_cast<uint32_t>(p.region.w) < e.width) || (p.region.h && static_cast<uint32_t>(p.region.h) < e.height))
            note_unsupported("ClearTexture of a sub-region (the whole level is cleared)");
    }
    VkImageSubresourceRange rng = {tex_aspect(t), s.level, 1, s.layer, p.has_view && p.view.has_layers ? static_cast<uint32_t>(std::max(1, p.view.num_layers)) : tex_layers(t)};
    if (tex_is_depth(t)) {
        VkClearDepthStencilValue v = {};
        std::memcpy(&v.depth, &p.value[0], 4);
        vkCmdClearDepthStencilImage(F->cb, tex_image(t), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &rng);
    } else {
        VkClearColorValue v = {};
        std::memcpy(v.uint32, p.value, 16);
        vkCmdClearColorImage(F->cb, tex_image(t), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &rng);
    }
    counters().clears.fetch_add(1, std::memory_order_relaxed);
    gstamp("clear texture");
}

void do_copy_buffer_texture(const P_CopyBufTex& p, bool to_texture) {
    Texture* t = p.tex;
    if (t && t->b.target == NVN_TEXTURE_TARGET_3D) {
        static int logged = 0;
        if (logged < 30) {
            ++logged;
            char a[96];
            NXLOG_I("nvn.copy", "%s 3D %s region %s (%d,%d,%d %dx%dx%d) buffer %llx row %lld image %lld", to_texture ? "buffer -> texture" : "texture -> buffer", tex_desc(t, a, sizeof a),
                    p.has_region ? "given" : "whole", p.has_region ? p.region.x : 0, p.has_region ? p.region.y : 0, p.has_region ? p.region.z : 0,
                    p.has_region ? p.region.w : 0, p.has_region ? p.region.h : 0, p.has_region ? p.region.d : 0, static_cast<unsigned long long>(p.address),
                    static_cast<long long>(p.row_stride), static_cast<long long>(p.image_stride));
        }
    }
    if (!t || !tex_gpu(t)) return;
    end_pass();
    if (tex_scale(t) != 1.0f) {
        static int logged = 0;
        if (logged++ < 10) {
            char a[96];
            NXLOG_W(kTag, "%s a scaled render target %s: the copy uses NVN texel coordinates (wrong part of the image)", to_texture ? "buffer copied into" : "copy from",
                    tex_desc(t, a, sizeof a));
        }
    }
    SubRes s = sub_of(p.view, p.has_view);
    const VkExtent3D ext = tex_extent(t, s.level);
    P_CopyRegion r = p.has_region ? p.region : P_CopyRegion{0, 0, 0, static_cast<int32_t>(ext.width), static_cast<int32_t>(ext.height), static_cast<int32_t>(ext.depth)};
    const FormatInfo* fi = t->fmt;
    const uint32_t bw = fi ? fi->block_w : 1, bh = fi ? fi->block_h : 1, bpb = fi ? fi->bytes : 4;
    uint32_t rw = static_cast<uint32_t>(r.w), rh = static_cast<uint32_t>(r.h), rd = std::max<int32_t>(1, r.d);
    VkBufferImageCopy c = {};
    const uint64_t row_blocks = (rw + bw - 1) / bw;
    const uint64_t row_bytes = row_blocks * bpb;
    const uint64_t stride = p.row_stride > 0 ? static_cast<uint64_t>(p.row_stride) : row_bytes;
    c.bufferRowLength = static_cast<uint32_t>(stride / bpb * bw);
    c.bufferImageHeight = p.image_stride > 0 ? static_cast<uint32_t>(static_cast<uint64_t>(p.image_stride) / stride * bh) : 0;
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, s.level, s.layer, 1};
    if (tex_is_depth(t)) c.imageSubresource.aspectMask = tex_aspect(t) & VK_IMAGE_ASPECT_DEPTH_BIT;
    c.imageOffset = {r.x, r.y, r.z};
    c.imageExtent = {rw, rh, rd};
    if (t->b.target != NVN_TEXTURE_TARGET_3D && t->layers > 1 && !p.has_view) { c.imageSubresource.layerCount = tex_layers(t); c.imageExtent.depth = 1; }
    if (t->b.target != NVN_TEXTURE_TARGET_3D && rd > 1) { c.imageSubresource.layerCount = rd; c.imageSubresource.baseArrayLayer = s.layer + static_cast<uint32_t>(std::max(0, r.z)); c.imageOffset.z = 0; c.imageExtent.depth = 1; }
    const uint64_t buf_bytes = stride * ((rh + bh - 1) / bh) * rd;
    if (!to_texture) compute_out_guard(p.address, buf_bytes, kOutBufferWrite);     // written: held results there go back first
    BufRef br = to_texture ? held_or_pool(p.address, buf_bytes, kOutBufferRead, true) : resolve_buffer(p.address, buf_bytes);
    c.bufferOffset = br.offset;
    if (to_texture) {
        tex_ensure_current(t, *F);
        tex_transition(t, *F, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        tev(t, "copy_buffer_to_texture", p.address, p.has_view ? 1 : 0);
        for (uint32_t i = 0; i < c.imageSubresource.layerCount; ++i) tex_sparse_before_copy(t, c.imageSubresource.baseArrayLayer + i, s.level);
        tex_mark_gpu_written(t);
        vkCmdCopyBufferToImage(F->cb, br.buffer, tex_image(t), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    } else {
        tex_ensure_current(t, *F);
        tex_transition(t, *F, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        vkCmdCopyImageToBuffer(F->cb, tex_image(t), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, br.buffer, 1, &c);
        gpu_wrote(p.address, stride * ((rh + bh - 1) / bh) * rd, kWriteImageCopy);
    }
    memory_barrier(F->cb);
    counters().copies.fetch_add(1, std::memory_order_relaxed);
    gstamp(to_texture ? "copy buffer->texture" : "copy texture->buffer");
}

void do_copy_texture_texture(const P_CopyTexTex& p) {
    Texture *s = p.src, *d = p.dst;
    if (!s || !d || !tex_gpu(s) || !tex_gpu(d)) return;
    end_pass();
    const SubRes ss = sub_of(p.src_view, p.has_src_view), ds = sub_of(p.dst_view, p.has_dst_view);
    const VkExtent3D se = tex_extent(s, ss.level), de = tex_extent(d, ds.level);
    P_CopyRegion sr = p.has_src_region ? p.src_region : P_CopyRegion{0, 0, 0, static_cast<int32_t>(se.width), static_cast<int32_t>(se.height), static_cast<int32_t>(se.depth)};
    P_CopyRegion dr = p.has_dst_region ? p.dst_region : P_CopyRegion{0, 0, 0, static_cast<int32_t>(de.width), static_cast<int32_t>(de.height), static_cast<int32_t>(de.depth)};
    // scaled images: the NVN regions in image texels (x, y and the size times each image's scale, clipped to the image)
    auto scale_region = [](P_CopyRegion& r, Texture* t, uint32_t level) {
        const float sc = tex_scale(t);
        if (sc == 1.0f) return;
        const VkExtent3D ie = tex_image_extent(t, level);
        const int32_t x1 = std::min<int32_t>(static_cast<int32_t>(std::lround((r.x + r.w) * sc)), static_cast<int32_t>(ie.width));
        const int32_t y1 = std::min<int32_t>(static_cast<int32_t>(std::lround((r.y + r.h) * sc)), static_cast<int32_t>(ie.height));
        r.x = static_cast<int32_t>(std::lround(r.x * sc));
        r.y = static_cast<int32_t>(std::lround(r.y * sc));
        r.w = std::max(0, x1 - r.x);
        r.h = std::max(0, y1 - r.y);
    };
    scale_region(sr, s, ss.level);
    scale_region(dr, d, ds.level);
    if (sr.w <= 0 || sr.h <= 0 || dr.w <= 0 || dr.h <= 0) return;
    tex_ensure_current(s, *F);
    if (s != d) tex_ensure_current(d, *F);
    const bool same = s == d;
    tex_transition(s, *F, same ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    if (!same) tex_transition(d, *F, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    tev(d, "copy_texture_to_texture", reinterpret_cast<uint64_t>(s), 0);
    for (int32_t i = 0; i < std::max(1, d->b.target != NVN_TEXTURE_TARGET_3D ? dr.d : 1); ++i) tex_sparse_before_copy(d, ds.layer + static_cast<uint32_t>(i), ds.level);
    tex_mark_gpu_written(d);
    const VkImageLayout sl = same ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    const VkImageLayout dl = same ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    const VkImageAspectFlags sa = tex_aspect(s), da = tex_aspect(d);
    const bool same_size = sr.w == dr.w && sr.h == dr.h && sr.d == dr.d;
    const bool same_fmt = tex_vk_format(s) == tex_vk_format(d);
    if (same_size && same_fmt) {
        VkImageCopy c = {};
        c.srcSubresource = {sa, ss.level, ss.layer, 1};
        c.dstSubresource = {da, ds.level, ds.layer, 1};
        c.srcOffset = {sr.x, sr.y, sr.z};
        c.dstOffset = {dr.x, dr.y, dr.z};
        c.extent = {static_cast<uint32_t>(sr.w), static_cast<uint32_t>(sr.h), static_cast<uint32_t>(std::max(1, sr.d))};
        if (s->b.target != NVN_TEXTURE_TARGET_3D && sr.d > 1) { c.srcSubresource.layerCount = c.dstSubresource.layerCount = sr.d; c.extent.depth = 1; c.srcOffset.z = c.dstOffset.z = 0; }
        vkCmdCopyImage(F->cb, tex_image(s), sl, tex_image(d), dl, 1, &c);
    } else {
        VkImageBlit b = {};
        b.srcSubresource = {sa, ss.level, ss.layer, 1};
        b.dstSubresource = {da, ds.level, ds.layer, 1};
        b.srcOffsets[0] = {sr.x, sr.y, sr.z};
        b.srcOffsets[1] = {sr.x + sr.w, sr.y + sr.h, sr.z + std::max(1, sr.d)};
        b.dstOffsets[0] = {dr.x, dr.y, dr.z};
        b.dstOffsets[1] = {dr.x + dr.w, dr.y + dr.h, dr.z + std::max(1, dr.d)};
        const bool linear = (p.flags & NVN_COPY_FLAGS_LINEAR_FILTER_BIT) || (tex_scale(s) != tex_scale(d) && !tex_is_depth(s) && !tex_is_depth(d));
        vkCmdBlitImage(F->cb, tex_image(s), sl, tex_image(d), dl, 1, &b, linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    }
    memory_barrier(F->cb);
    counters().copies.fetch_add(1, std::memory_order_relaxed);
    gstamp("copy texture");
}

void do_downsample(const P_Downsample& p) {
    Texture *s = p.src, *d = p.dst;
    if (!s || !d || !tex_gpu(s) || !tex_gpu(d)) return;
    end_pass();
    if (tex_is_depth(s)) { note_unsupported("Downsample of a depth texture"); return; }
    tex_transition(s, *F, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    tex_transition(d, *F, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    tex_mark_gpu_written(d);
    VkImageResolve rs = {};
    rs.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    rs.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    if (tex_scale(s) != tex_scale(d)) log_once("downsample-scale", "Downsample between a scaled and an unscaled texture: only the common part is resolved");
    const VkExtent3D e = tex_image_extent(d, 0), es = tex_image_extent(s, 0);
    rs.extent = {std::min(e.width, es.width), std::min(e.height, es.height), 1};
    vkCmdResolveImage(F->cb, tex_image(s), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, tex_image(d), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rs);
    memory_barrier(F->cb);
    counters().copies.fetch_add(1, std::memory_order_relaxed);
    gstamp("downsample");
}

// nvnCommandBufferEvaluateDlss (api_dlss.cpp): the game's own upscale call. With [upscale] mode fsr3, dlss or dlaa and the SDK present, FSR 3 / DLSS
// runs on the game's colour, depth and motion vectors (temporal_evaluate_native) and its result is copied into the game's output rectangle;
// otherwise (or when that cannot run) the colour rectangle is scaled into the output rectangle with the bilinear filter, which is a copy when the
// two have the same size.
void do_evaluate_dlss(const P_EvaluateDlss& p) {
    cmdq::drain();   // the DLSS evaluation records through its own loader: everything queued before it first
    Texture *in = p.color.tex, *out = p.output.tex;
    if (!in || !out || in == out) { log_once("dlss-args", "EvaluateDlss without an input colour or an output texture (or both the same): skipped"); return; }
    if (!tex_gpu(in) || !tex_gpu(out)) return;
    if (tex_is_depth(in) || tex_is_depth(out)) { log_once("dlss-depthfmt", "EvaluateDlss on a depth-format colour or output texture: skipped"); return; }
    end_pass();
    const SubRes is = sub_of(p.color.view, p.color.has_view), os = sub_of(p.output.view, p.output.has_view);
    const VkExtent3D ie = tex_extent(in, is.level), oe = tex_extent(out, os.level);   // NVN texels
    const float sin_ = tex_scale(in), sout = tex_scale(out);
    auto fit = [](int32_t want, int32_t base, uint32_t have) {   // the rectangle's size: 0 is the whole texture; never past its edge
        const int32_t room = std::max<int32_t>(1, static_cast<int32_t>(have) - std::max(0, base));
        return want > 0 ? std::min(want, room) : room;
    };
    const int32_t iw = fit(p.in_w, p.color.base_x, ie.width), ih = fit(p.in_h, p.color.base_y, ie.height);
    const int32_t ow = fit(p.out_w, p.output.base_x, oe.width), oh = fit(p.out_h, p.output.base_y, oe.height);
    auto px = [](int32_t v, float sc) { return std::max<int32_t>(0, static_cast<int32_t>(std::lround(v * sc))); };
    const VkExtent2D render{static_cast<uint32_t>(std::max(1, px(iw, sin_))), static_cast<uint32_t>(std::max(1, px(ih, sin_)))};
    const VkExtent2D outx{static_cast<uint32_t>(std::max(1, px(ow, sout))), static_cast<uint32_t>(std::max(1, px(oh, sout)))};
    const VkOffset3D in_at{px(p.color.base_x, sin_), px(p.color.base_y, sin_), 0}, out_at{px(p.output.base_x, sout), px(p.output.base_y, sout), 0};

    bool done = false;
    const char* route = "plain";
    Texture *depth = p.depth.tex, *motion = p.motion.tex;
    if (extras::temporal_wanted() && depth && motion && depth != in && motion != in && tex_gpu(depth) && tex_gpu(motion)) {
        const SubRes ds = sub_of(p.depth.view, p.depth.has_view), ms = sub_of(p.motion.view, p.motion.has_view);
        auto request = [](const SubRes& s) {
            ViewRequest r;
            r.base_level = s.level;
            r.level_count = 1;
            r.base_layer = s.layer;
            r.layer_count = 1;
            r.depth_aspect = true;
            return r;
        };
        for (Texture* t : {in, depth, motion}) {
            tex_ensure_current(t, *F);
            tex_transition(t, *F, tex_sampled_layout(t));
        }
        extras::NativeTemporalFrame f;
        f.color.view = tex_sampled_view(in, request(is));
        f.color.layout = tex_sampled_layout(in);
        f.depth.view = tex_sampled_view(depth, request(ds));
        f.depth.layout = tex_sampled_layout(depth);
        f.motion.view = tex_sampled_view(motion, request(ms));
        f.motion.layout = tex_sampled_layout(motion);
        const float sd = tex_scale(depth), sm = tex_scale(motion);
        const bool mv_low_res = (p.flags & 2u) != 0;   // the network's NGX flag: motion vectors at the render size (else at the output size)
        f.color.scale[0] = f.color.scale[1] = 1.0f;
        f.color.off[0] = in_at.x;
        f.color.off[1] = in_at.y;
        f.depth.scale[0] = iw * sd / static_cast<float>(render.width);
        f.depth.scale[1] = ih * sd / static_cast<float>(render.height);
        f.depth.off[0] = px(p.depth.base_x, sd);
        f.depth.off[1] = px(p.depth.base_y, sd);
        f.motion.scale[0] = (mv_low_res ? iw : ow) * sm / static_cast<float>(render.width);
        f.motion.scale[1] = (mv_low_res ? ih : oh) * sm / static_cast<float>(render.height);
        f.motion.off[0] = px(p.motion.base_x, sm);
        f.motion.off[1] = px(p.motion.base_y, sm);
        f.render = render;
        f.out = outx;
        const float to_render_x = mv_low_res ? 1.0f : static_cast<float>(render.width) / static_cast<float>(outx.width);   // vectors in output pixels -> render pixels
        const float to_render_y = mv_low_res ? 1.0f : static_cast<float>(render.height) / static_cast<float>(outx.height);
        f.mv_scale[0] = p.mv_scale[0] * to_render_x;
        f.mv_scale[1] = p.mv_scale[1] * to_render_y;
        f.jitter_px[0] = p.jitter[0];
        f.jitter_px[1] = p.jitter[1];
        f.depth_reversed = (p.flags & 8u) != 0;
        f.reset = p.reset != 0;
        f.history_key = p.net ^ (static_cast<uint64_t>(p.serial) << 52);
        extras::NativeTemporalResult res;
        if (f.color.view && f.depth.view && f.motion.view && extras::temporal_evaluate_native(&device(), F->cb, f, &res)) {
            g_dirty = kAllDirty;                                               // the SDKs bind their own compute state
            tex_ensure_current(out, *F);
            tex_transition(out, *F, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            tex_sparse_before_copy(out, os.layer, os.level);
            tex_mark_gpu_written(out);
            VkImageBlit b = {};
            b.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            b.dstSubresource = {tex_aspect(out), os.level, os.layer, 1};
            b.srcOffsets[1] = {static_cast<int32_t>(outx.width), static_cast<int32_t>(outx.height), 1};
            b.dstOffsets[0] = out_at;
            b.dstOffsets[1] = {out_at.x + static_cast<int32_t>(outx.width), out_at.y + static_cast<int32_t>(outx.height), 1};
            vkCmdBlitImage(F->cb, res.image, VK_IMAGE_LAYOUT_GENERAL, tex_image(out), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_NEAREST);
            route = res.backend;
            done = true;
        }
    }
    if (!done) {
        tex_ensure_current(in, *F);
        tex_ensure_current(out, *F);
        tex_transition(in, *F, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        tex_transition(out, *F, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        tex_sparse_before_copy(out, os.layer, os.level);
        tex_mark_gpu_written(out);
        VkImageBlit b = {};
        b.srcSubresource = {tex_aspect(in), is.level, is.layer, 1};
        b.dstSubresource = {tex_aspect(out), os.level, os.layer, 1};
        b.srcOffsets[0] = in_at;
        b.srcOffsets[1] = {in_at.x + static_cast<int32_t>(render.width), in_at.y + static_cast<int32_t>(render.height), 1};
        b.dstOffsets[0] = out_at;
        b.dstOffsets[1] = {out_at.x + static_cast<int32_t>(outx.width), out_at.y + static_cast<int32_t>(outx.height), 1};
        const bool same = render.width == outx.width && render.height == outx.height;
        vkCmdBlitImage(F->cb, tex_image(in), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, tex_image(out), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b,
                       same ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
    }
    memory_barrier(F->cb);
    counters().copies.fetch_add(1, std::memory_order_relaxed);
    static std::string told;   // the route is logged when it changes
    if (told != route) {
        told = route;
        NXLOG_I(kTag, "EvaluateDlss: %ux%u -> %ux%u by %s", render.width, render.height, outx.width, outx.height, route);
    }
    gstamp("evaluate dlss");
}

void do_write_memory(uint64_t gpu_addr, const void* data, uint32_t size) {
    // a GPU-ordered write of up to 64 KiB into pool memory (UpdateUniformBuffer, counters, events)
    static const bool defer = nxrt::config_get_bool("nvn", "write_defer", true);
    if (P.open && defer && ab_on(kAbWriteDefer) && size <= 4096) {   // at the end of the pass (flush_deferred_writes)
        BufRef b = resolve_buffer(gpu_addr, size);
        if (!b.ok) return;
        gpu_wrote(gpu_addr, size, kWriteUpdate);
        const size_t at = g_wdefer_data.size();
        g_wdefer_data.insert(g_wdefer_data.end(), static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
        g_wdefer.push_back({b.buffer, b.offset, size, at});
        ++g_wdefer_n;
        return;
    }
    end_pass();
    memory_barrier(F->cb);
    BufRef b = resolve_buffer(gpu_addr, size);
    if (!b.ok) return;
    gpu_wrote(gpu_addr, size, kWriteUpdate);
    vkCmdUpdateBuffer(F->cb, b.buffer, b.offset, size, data);
    memory_barrier(F->cb);
    gstamp("write memory");
}

// ---- the interpreter ----------------------------------------------------------------------------------------------------------
void apply_bind_program(const P_BindProgram& p) {
    const bool pending = p.program && program_resolve_pending(p.program);
    if (p.stages & (((1u << kCompute) - 1) | (1u << kTask) | (1u << kMesh))) {
        g_gfx_pending = pending;
        g_gfx_missing = (p.program && p.program->missing[0]) ? std::string(p.program->missing) : std::string();
    }
    if (p.stages & (1u << kCompute)) g_cs_pending = pending;
    // Task and mesh (NVN stages 6 and 7) go with the graphics stages: a bind with graphics stage bits also sets or clears them, so a vertex
    // program bound after a mesh program leaves no mesh stage behind (whichever bits NVN2 gives them).
    const uint32_t bits = p.stages | ((p.stages & ((1u << kCompute) - 1)) ? ((1u << kTask) | (1u << kMesh)) : 0u);
    for (int st = 0; st < kStages; ++st)
        if (bits & (1u << st)) S.stage[st] = p.program ? p.program->stage[st] : nullptr;
    if (p.program && (p.program->stage[kTask] || p.program->stage[kMesh])) {
        static bool logged = false;
        if (!logged) { logged = true; NXLOG_I(kTag, "mesh program bound with NVN stage bits 0x%x (task %s, mesh %s)", p.stages,
                                              p.program->stage[kTask] ? p.program->stage[kTask]->hash.c_str() : "-", p.program->stage[kMesh] ? p.program->stage[kMesh]->hash.c_str() : "-"); }
    }
    if (p.stages & (1u << kCompute)) {   // also outside traces: the upscaler override names untranslated passes by it
        g_cs_missing = (p.program && !p.program->stage[kCompute]) ? std::string(p.program->missing) : std::string();
        if (!g_cs_missing.empty()) TRACE("bind program: compute block(s) %s not in the shader cache", g_cs_missing.c_str());
    }
}

void mark_dirty(const RecordReader& r) {
    switch (r.op()) {
        case Op::BindProgram: g_dirty |= kPipe | kDesc | kVb; break;
        case Op::BindBuffers: g_dirty |= r.payload<P_BindBuffers>().kind == kBufVertex ? kVb : kDesc; break;
        case Op::BindTextures: case Op::BindImages: case Op::SetTexturePool: case Op::SetSamplerPool: g_dirty |= kDesc; break;
        case Op::BindVertexAttribs: case Op::BindVertexStreams: g_dirty |= kPipe | kVb; break;
        case Op::BindBlend: case Op::BindColor: case Op::BindChannelMask: case Op::BindMultisample: case Op::SetDepthClamp: g_dirty |= kPipe; break;
        case Op::BindPolygon: g_dirty |= kPipe | kDynRaster | kDynBias; break;
        case Op::BindDepthStencil: case Op::SetDepthBounds: g_dirty |= kDynDepth | kDynStencil; break;
        case Op::SetViewports: case Op::SetScissors: case Op::SetDepthRanges: g_dirty |= kDynVp; break;
        case Op::SetStencil: g_dirty |= kDynStencil; break;
        case Op::SetBlendColor: g_dirty |= kDynBlend; break;
        case Op::SetLineWidth: case Op::SetRasterizerDiscard: case Op::SetPrimitiveRestart: g_dirty |= kDynRaster; break;
        case Op::SetPatchSize: g_dirty |= kPipe; break;   // patch control points are part of the pipeline
        case Op::SetPolygonOffsetClamp: g_dirty |= kDynBias; break;
        case Op::SetRenderTargets: g_dirty = kAllDirty; break;
        default: break;
    }
}

// ---- windowed GPU counters (see g_cw) ----------------------------------------------------------------------------------------
bool cw_ready(CounterWindow& w, bool xfb) {
    if (w.pool) return true;
    if (w.tried) return false;
    w.tried = true;
    const gfx::DeviceFeatures& f = device().features();
    if (xfb ? !(f.transform_feedback && f.transform_feedback_queries) : !f.pipeline_statistics_query) return false;
    VkQueryPoolCreateInfo qi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qi.queryType = xfb ? VK_QUERY_TYPE_TRANSFORM_FEEDBACK_STREAM_EXT : VK_QUERY_TYPE_PIPELINE_STATISTICS;
    qi.queryCount = kCounterSlots;
    if (!xfb) qi.pipelineStatistics = VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_PRIMITIVES_BIT;
    if (vkCreateQueryPool(vkdev(), &qi, nullptr, &w.pool) != VK_SUCCESS) w.pool = VK_NULL_HANDLE;
    return w.pool != VK_NULL_HANDLE;
}

void cw_end_query(CounterWindow& w, bool xfb) {
    if (w.active < 0) return;
    if (xfb) vkCmdEndQueryIndexedEXT(F->cb, w.pool, static_cast<uint32_t>(w.active), 0);
    else vkCmdEndQuery(F->cb, w.pool, static_cast<uint32_t>(w.active));
}

// Does the window that a ResetCounter of `type` opens (up to the next report or reset of that type in the same list) hold a draw whose
// primitives the CPU cannot count: an indirect draw or a DrawTransformFeedback? The record reader is copied; the list itself is not touched.
bool counter_window_has_gpu_draw(RecordReader look, uint32_t type) {
    while (look.next()) {
        const Op op = look.op();
        if ((op == Op::ReportCounter || op == Op::ResetCounter) && look.payload<P_Counter>().type == type) return false;
        if (op == Op::DrawIndirect || op == Op::DrawXfb || op == Op::DrawMeshTasks) return true;
    }
    return false;
}

void run_list(const CmdList& list, int depth);

// Splits the submission so the CPU can read [addr, addr + size): what was recorded so far runs and completes first (nvn_gpu.cpp frame_split). The
// command buffer changes, so what lived in it is ended (render pass, queries, debug labels) and everything is bound again afterwards, the way
// replay_begin_submit starts a submission. Writes noted so far are done once the GPU is; the ones of the rest of the submission are noted again.
bool split_for_read(uint64_t addr, uint64_t size) {
    static std::atomic<uint64_t> splits{0};
    const PoolHit hit = pool_lookup(addr);
    if (!F || !hit.pool || hit.pool->is_virtual || !hit.pool->base || hit.offset + size > hit.pool->size) return false;
    end_pass();
    if (out_any()) flush_compute_out(kOutOther);   // held compute results are copied back by this part
    if (g_occ_active >= 0) {                       // a query cannot outlive its command buffer: the count so far is what a report gets
        vkCmdEndQuery(F->cb, g_occ_pool, static_cast<uint32_t>(g_occ_active));
        g_occ_last = g_occ_active;
        g_occ_active = -1;
    }
    for (int k = 0; k < 2; ++k) {
        cw_end_query(g_cw[k], k == 1);
        g_cw[k].active = -1;
    }
    for (; g_dbg_depth > 0; --g_dbg_depth) vkCmdEndDebugUtilsLabelEXT(F->cb);
    if (!frame_split(*F)) return false;
    P = Pass();
    g_pending.clear();                             // everything before the split has completed: no barrier is owed for it
    g_set_cache.clear();
    g_ds_bvals.clear();
    g_bound_pipeline = VK_NULL_HANDLE;
    g_dirty = kAllDirty;
    g_pl = nullptr;
    g_pipe = VK_NULL_HANDLE;
    g_topo = g_pipe_topo = VK_PRIMITIVE_TOPOLOGY_MAX_ENUM;
    g_restart_valid = false;
    g_vb_bound_in_cb = false;
    g_fb_use.clear();
    g_key_dummy_binding = 0;
    for (size_t i = 0; i < g_gw_count; ++i)
        if (g_gw[i].serial == F->serial) g_gw[i].serial = 0;
    if (g_gw_lost_serial == F->serial) g_gw_lost_serial = 0;
    const uint64_t n = splits.fetch_add(1) + 1;
    if (n == 1 || (n & 255) == 0) NXLOG_I("nvn.replay", "replay: submission split to read device command words (%llu so far): the GPU is waited for", static_cast<unsigned long long>(n));
    return true;
}

// nvnCommandBufferSubmitDeviceCommands: the command words in the device command buffer's storage are read by the CPU, decoded (nvn_devcmd.h)
// and the draws in them run like the recorded ones. First version: the words must be readable here (written by the CPU, or by a submission
// that has completed; see cpu_view). Words the GPU writes in the submission being recorded: the submission is split first (split_for_read), the
// GPU waited for, and the words read; if that is not possible the submit is skipped and logged. VK_EXT_device_generated_commands would take the place of this read-back: docs/nvn2_device_commands.md.
void do_submit_device_commands(const P_SubmitDeviceCommands& p, int depth) {
    static bool noted = false;
    if (!p.stream_size) return;
    const uint8_t* bytes = cpu_view(p.stream_address, p.stream_size);
    if (!bytes && split_for_read(p.stream_address, p.stream_size)) bytes = cpu_view(p.stream_address, p.stream_size);
    if (!bytes) {
        note_unsupported("device commands: the command words are not readable by the CPU (written by the GPU in this submission, or outside any pool)");
        counters().draws_skipped.fetch_add(1);
        return;
    }
    std::vector<devcmd::Cmd> cmds;
    const devcmd::Stats st = devcmd::decode(bytes, static_cast<size_t>(p.stream_size), cmds);
    if (!noted) {
        noted = true;
        NXLOG_I("nvn.replay", "replay: device commands (first): %u words, %u packets, %u draws, %u not decoded (first 0x%08x 0x%08x), %u bad draw arguments; stopped at %s",
                st.words, st.packets, st.commands, st.unhandled, st.first_unhandled[0], st.first_unhandled[1], st.bad_args, devcmd::end_name(st.end));
    }
    if (st.unhandled) note_unsupported("device commands: packets that are not decoded are skipped");
    // The decoded commands become the records the API would have recorded, in a list of their own that runs here: the replay's own handling of
    // each op (dirty flags, compute results that go back first, bindings) applies to them as it does to a recorded list.
    CmdList tmp;
    for (const devcmd::Cmd& c : cmds) {
        switch (c.kind) {
            case devcmd::Kind::DrawArrays: {
                if (c.indirect && (!c.count || !c.instance_count)) break;   // an indirect draw with nothing to draw
                P_DrawArrays d{};
                d.mode = c.mode; d.first = c.first; d.count = c.count; d.base_instance = c.base_instance; d.instance_count = c.instance_count;
                tmp.emit(Op::DrawArrays, d);
                break;
            }
            case devcmd::Kind::DrawElements: {
                if (c.indirect && (!c.count || !c.instance_count)) break;
                P_DrawElements d{};
                d.mode = c.mode; d.type = c.index_type; d.count = c.count; d.instance_count = c.instance_count;
                d.base_vertex = c.base_vertex; d.base_instance = c.base_instance;
                const uint32_t isz = index_size_bytes(static_cast<int>(c.index_type));
                d.index_address = c.index_address + static_cast<uint64_t>(c.first) * (isz ? isz : 4);
                tmp.emit(Op::DrawElements, d);
                break;
            }
            case devcmd::Kind::BindVertexBuffer: case devcmd::Kind::BindUniformBuffer: {
                P_BindBuffers d{};
                d.kind = c.kind == devcmd::Kind::BindVertexBuffer ? kBufVertex : kBufUniform;
                d.stage = c.stage; d.first = c.slot; d.count = 1;
                const NVNbufferRange rg{c.address, c.size};
                tmp.emit(Op::BindBuffers, d, &rg, static_cast<uint32_t>(sizeof rg));
                break;
            }
            case devcmd::Kind::SetStencil: {
                P_Stencil d{};
                d.what = c.what; d.face = c.face; d.value = c.value;
                tmp.emit(Op::SetStencil, d);
                break;
            }
            case devcmd::Kind::SetPrimitiveRestart: {
                P_PrimitiveRestart d{c.enable, c.value};
                tmp.emit(Op::SetPrimitiveRestart, d);
                break;
            }
            case devcmd::Kind::SetScissor: {
                const P_Rects d{c.slot, 1};
                tmp.emit(Op::SetScissors, d, c.rect, static_cast<uint32_t>(sizeof c.rect));
                break;
            }
        }
    }
    run_list(tmp, depth + 1);
}

// [nvn] set_reuse: commands after which every binding resolves as before (state binds, draws, counters, debug labels). Anything else
// (copies, clears, dispatches, uniform updates, barriers, render target changes, ...) moves the resolve epoch. GPU writes of draws move it
// through gpu_wrote.
// [nvn] barrier_keep_pass (default on): a game barrier inside an open pass whose draws wrote nothing but its attachments (no storage
// buffers or images, no stream-out: g_pass_unsafe) leaves the pass open. Everything the barrier orders is then ordered anyway: the pass's
// attachment writes reach later draws of the same pass in rasterization order, a later draw that samples one of them is a feedback draw
// (its own copy, made outside the pass) or a depth read (a read-only pass of its own), and a texture that needs another layout ends the pass
// with that transition (prepare_textures); every command before the pass that writes memory (copies, fills, dispatches, memory writes)
// ends with a full barrier, and occlusion reports in the pass are written when it ends and read only by the CPU. Any other barrier ends
// the pass with a full barrier, as before (off: every barrier).
uint64_t g_bar_n = 0, g_bar_in_pass = 0, g_bar_kept = 0, g_bar_unsafe = 0;   // statistics
uint32_t g_bar_bits_kept = 0, g_bar_bits_all = 0;
void do_barrier(uint32_t bits) {
    static const bool keep = nxrt::config_get_bool("nvn", "barrier_keep_pass", true);
    ++g_bar_n;
    g_bar_bits_all |= bits;
    if (P.open) {
        ++g_bar_in_pass;
        if (g_pass_unsafe) ++g_bar_unsafe;
        else if (keep && ab_on(kAbBarrierKeep) && !g_trace) {
            ++g_bar_kept;
            g_bar_bits_kept |= bits;
            return;
        }
    }
    end_pass();
    memory_barrier(F->cb);
}

// [nvn] epoch_keep (default on): also Barrier (a kept one leaves the pass open; an honoured one ends it, and the next pass moves the
// epoch), WaitEvent (a no-op here), SetRenderTargets (new attachments take effect through the next pass, which moves the epoch) and
// ClearColor / ClearDepthStencil (vkCmdClearAttachments in the pass; feedback reads of attachments never reuse sets).
bool epoch_keep_on() {
    static const bool on = nxrt::config_get_bool("nvn", "epoch_keep", true);
    return on && ab_on(kAbEpochKeep);
}
bool op_keeps_sets(Op op) {
    switch (op) {
        case Op::Barrier: case Op::WaitEvent: case Op::SetRenderTargets: case Op::ClearColor: case Op::ClearDepthStencil:
            return epoch_keep_on();
        case Op::Dispatch: case Op::DispatchIndirect:   // what a dispatch changes for draws: its GPU writes (gpu_wrote moves the epoch) and the held results (out_version is in the memo's epoch)
            return held_version_epoch();
        default: break;
    }
    switch (op) {
        case Op::Nop: case Op::BindProgram: case Op::BindBuffers: case Op::BindTextures: case Op::BindImages: case Op::BindVertexAttribs:
        case Op::BindVertexStreams: case Op::BindBlend: case Op::BindColor: case Op::BindChannelMask: case Op::BindDepthStencil: case Op::BindPolygon:
        case Op::BindMultisample: case Op::SetTexturePool: case Op::SetSamplerPool: case Op::SetViewports: case Op::SetScissors: case Op::SetDepthRanges:
        case Op::SetDepthBounds: case Op::SetStencil: case Op::SetBlendColor: case Op::SetPointSize: case Op::SetLineWidth: case Op::SetPolygonOffsetClamp:
        case Op::SetAlphaRef: case Op::SetSampleMask: case Op::SetRasterizerDiscard: case Op::SetDepthClamp: case Op::SetPrimitiveRestart:
        case Op::SetPatchSize: case Op::SetTessLevels: case Op::SetRenderEnable: case Op::SetCoverageModulationTable: case Op::SetConservativeRaster:
        case Op::DrawArrays: case Op::DrawElements: case Op::DrawIndirect: case Op::DrawMeshTasks: case Op::ReportCounter: case Op::ResetCounter:
        case Op::PushDebugGroup: case Op::PopDebugGroup: case Op::Small: case Op::CallCommands:
        case Op::UpdateUniformBuffer:   // at record time: the snapshot generation is in the signature; recorded as a GPU write: bumped there
            return true;
        default:
            return false;
    }
}

void run_list(const CmdList& list, int depth) {
    if (depth > kMaxDepth) { log_once("depth", "CallCommands nesting too deep"); return; }
    struct RdRestore { int d; ~RdRestore() { g_rd_depth = d - 1; } } rd_restore{depth};   // pass_lookahead: back to the caller's reader
    counters().lists_replayed.fetch_add(1, std::memory_order_relaxed);
    for (RecordReader r(list); r.next();) {
        g_rd_cur[depth] = &r;
        g_rd_depth = depth;
        ++g_resolve_gen;   // resolve_cache: results hold for one command
        chunk_at_op();
        if (!op_keeps_sets(r.op())) { desc_epoch_bump(kBumpOp); ++g_bump_op[static_cast<size_t>(r.op()) & 255]; }
        mark_dirty(r);
        if (out_lazy()) out_before_op(r);                                                   // compute shadows: results back to the pool first
        else if (out_any() && !compute_out_may_wait(r.op())) flush_compute_out(kOutOther);
        switch (r.op()) {
            case Op::Nop: break;
            case Op::BindProgram: apply_bind_program(r.payload<P_BindProgram>()); ++g_buf_ver; g_odb_all_dirty = true; break;
            case Op::BindBuffers: {
                const auto& p = r.payload<P_BindBuffers>();
                const NVNbufferRange* rg = reinterpret_cast<const NVNbufferRange*>(r.blob<P_BindBuffers>());
                // set: a binding that changes moves g_buf_ver (out_draw_bindings skips its checks while it and the resolve epoch stay)
                auto set = [](GState::Bind& b, uint64_t addr, uint64_t size) {
                    if (b.addr != addr || b.size != size) { b = {addr, size}; ++g_buf_ver; }
                };
                for (uint32_t i = 0; i < p.count; ++i) {
                    const uint32_t k = p.first + i;
                    const uint64_t v_before = g_buf_ver;
                    if (p.kind == kBufVertex && k < 16) set(S.vtx[k], rg[i].address, rg[i].size);
                    // a uniform or storage buffer in a virtual (sparse) pool is bound through the physical memory mapped there (physical_buffer_address)
                    else if (p.kind == kBufUniform && p.stage < kStages && k < 16) set(S.ubo[p.stage][k], physical_buffer_address(rg[i].address, rg[i].size), rg[i].size);
                    else if (p.kind == kBufStorage && p.stage < kStages && k < 16) set(S.ssbo[p.stage][k], physical_buffer_address(rg[i].address, rg[i].size), rg[i].size);
                    else if (p.kind == kBufXfb && k < 4) set(S.xfb[k], rg[i].address, rg[i].size);
                    if (g_buf_ver != v_before)   // odb_incremental: this slot is checked again at the next draw
                        odb_mark(p.kind == kBufVertex ? kOdbVtx : p.kind == kBufUniform ? kOdbUbo : p.kind == kBufStorage ? kOdbSsbo : kOdbXfb,
                                 (p.kind == kBufUniform || p.kind == kBufStorage) ? static_cast<int>(p.stage) : 0, k);
                }
                break;
            }
            case Op::BindTextures: {
                const auto& p = r.payload<P_BindTextures>();
                const uint64_t* h = reinterpret_cast<const uint64_t*>(r.blob<P_BindTextures>());
                for (uint32_t i = 0; i < p.count; ++i)
                    if (p.stage < kStages && p.first + i < 64) S.tex[p.stage][p.first + i] = h[i];
                break;
            }
            case Op::BindImages: {
                const auto& p = r.payload<P_BindTextures>();
                const uint64_t* h = reinterpret_cast<const uint64_t*>(r.blob<P_BindTextures>());
                for (uint32_t i = 0; i < p.count; ++i)
                    if (p.stage < kStages && p.first + i < 16) S.img[p.stage][p.first + i] = h[i];
                break;
            }
            case Op::BindVertexAttribs: {
                const auto& p = r.payload<P_BindVertexAttribs>();
                const NVNvertexAttribState* a = reinterpret_cast<const NVNvertexAttribState*>(r.blob<P_BindVertexAttribs>());
                S.attr_count = std::min<uint32_t>(p.count, 16);
                for (uint32_t i = 0; i < S.attr_count; ++i) S.attr[i] = decode(&a[i]);
                break;
            }
            case Op::BindVertexStreams: {
                const auto& p = r.payload<P_BindVertexStreams>();
                const NVNvertexStreamState* a = reinterpret_cast<const NVNvertexStreamState*>(r.blob<P_BindVertexStreams>());
                S.stream_count = std::min<uint32_t>(p.count, 16);
                for (uint32_t i = 0; i < S.stream_count; ++i) S.stream[i] = decode(&a[i]);
                break;
            }
            case Op::BindBlend: {
                NVNblendState st; std::memcpy(&st, r.payload<P_RawState>().bytes, sizeof st);
                BlendInfo b = decode(&st);
                if (b.target >= 0 && b.target < 8) S.blend[b.target] = b;
                break;
            }
            case Op::BindColor: { NVNcolorState st; std::memcpy(&st, r.payload<P_RawState>().bytes, sizeof st); S.color = decode(&st); break; }
            case Op::BindChannelMask: { NVNchannelMaskState st; std::memcpy(&st, r.payload<P_RawState>().bytes, sizeof st); S.chmask = decode(&st); break; }
            case Op::BindDepthStencil: { NVNdepthStencilState st; std::memcpy(&st, r.payload<P_RawState>().bytes, sizeof st); S.ds = decode(&st); break; }
            case Op::BindPolygon: { NVNpolygonState st; std::memcpy(&st, r.payload<P_RawState>().bytes, sizeof st); S.poly = decode(&st); break; }
            case Op::BindMultisample: { NVNmultisampleState st; std::memcpy(&st, r.payload<P_RawState>().bytes, sizeof st); S.ms = decode(&st); break; }
            case Op::SetTexturePool: S.tpool = Ref<TexturePool>::share(static_cast<TexturePool*>(r.payload<P_Pool>().pool)); break;
            case Op::SetSamplerPool: S.spool = Ref<SamplerPool>::share(static_cast<SamplerPool*>(r.payload<P_Pool>().pool)); break;
            case Op::SetViewports: {
                const auto& p = r.payload<P_Rects>();
                const float* v = reinterpret_cast<const float*>(r.blob<P_Rects>());
                for (uint32_t i = 0; i < p.count && p.first + i < 16; ++i) std::memcpy(S.vp[p.first + i], v + 4 * i, 16);
                S.nvp = std::max(S.nvp, std::min<uint32_t>(p.first + p.count, 16));
                break;
            }
            case Op::SetScissors: {
                const auto& p = r.payload<P_Rects>();
                const int32_t* v = reinterpret_cast<const int32_t*>(r.blob<P_Rects>());
                for (uint32_t i = 0; i < p.count && p.first + i < 16; ++i) std::memcpy(S.sc[p.first + i], v + 4 * i, 16);
                S.nsc = std::max(S.nsc, std::min<uint32_t>(p.first + p.count, 16));
                break;
            }
            case Op::SetDepthRanges: {
                const auto& p = r.payload<P_DepthRanges>();
                const float* v = reinterpret_cast<const float*>(r.blob<P_DepthRanges>());
                for (uint32_t i = 0; i < p.count && p.first + i < 16; ++i) std::memcpy(S.dr[p.first + i], v + 2 * i, 8);
                break;
            }
            case Op::SetDepthBounds: { const auto& p = r.payload<P_DepthBounds>(); S.depth_bounds = p.enable; S.db_near = p.near_; S.db_far = p.far_; break; }
            case Op::SetStencil: {
                const auto& p = r.payload<P_Stencil>();
                for (int f = 0; f < 2; ++f) {
                    const bool hit = p.face == NVN_FACE_FRONT_AND_BACK || (f == 0 && p.face == NVN_FACE_FRONT) || (f == 1 && p.face == NVN_FACE_BACK);
                    if (!hit) continue;
                    if (p.what == 0) S.stencil_ref[f] = p.value;
                    else if (p.what == 1) S.stencil_vmask[f] = p.value;
                    else S.stencil_wmask[f] = p.value;
                }
                break;
            }
            case Op::SetBlendColor: std::memcpy(S.blend_color, r.payload<P_BlendColor>().c, 16); break;
            case Op::SetPointSize: S.point_size = r.payload<P_Float>().v; break;
            case Op::SetLineWidth: S.line_width = r.payload<P_Float>().v; break;
            case Op::SetAlphaRef: S.alpha_ref = r.payload<P_Float>().v; break;
            case Op::SetPolygonOffsetClamp: { const auto& p = r.payload<P_PolygonOffsetClamp>(); S.bias_factor = p.factor; S.bias_units = p.units; S.bias_clamp = p.clamp; break; }
            case Op::SetSampleMask: S.sample_mask = r.payload<P_U32>().v; break;
            case Op::SetRasterizerDiscard: S.rasterizer_discard = r.payload<P_U32>().v != 0; break;
            case Op::SetDepthClamp: S.depth_clamp = r.payload<P_U32>().v != 0; break;
            case Op::SetPrimitiveRestart: { const auto& p = r.payload<P_PrimitiveRestart>(); S.prim_restart = p.enable != 0; S.restart_index = p.index; break; }
            case Op::SetPatchSize: S.patch_size = r.payload<P_U32>().v; g_dirty |= kPipe; break;   // the patch size is in the pipeline key
            case Op::SetTessLevels: {
                const auto& p = r.payload<P_TessLevels>();
                if (p.outer) std::memcpy(S.tess_outer, p.v, sizeof S.tess_outer);
                else std::memcpy(S.tess_inner, p.v, sizeof S.tess_inner);
                break;
            }
            case Op::SetRenderEnable:
                if (r.size() >= sizeof(RecHeader) + sizeof(P_RenderEnableCond)) {
                    const auto& p = r.payload<P_RenderEnableCond>();
                    S.render_enable = conditional_render_enabled(p.mode, p.address);
                } else {
                    S.render_enable = r.payload<P_U32>().v != 0;
                }
                break;
            case Op::SetCoverageModulationTable: break;
            case Op::SetConservativeRaster: S.small.cons_legacy = r.payload<P_U32>().v != 0; g_dirty |= kDynRaster; break;
            case Op::Small: {
                const auto& p = r.payload<P_Small>();
                const uint32_t blob_bytes = r.size() - static_cast<uint32_t>(sizeof(RecHeader)) - static_cast<uint32_t>((sizeof(P_Small) + 7u) & ~7u);
                if (g_trace && (p.kind == static_cast<uint32_t>(SmallKind::PushConstants) || p.kind == static_cast<uint32_t>(SmallKind::PushConstantsDeferred))) {
                    const bool deferred = p.kind == static_cast<uint32_t>(SmallKind::PushConstantsDeferred);
                    const uint8_t* src = deferred ? (host_readable(p.addr, p.c) ? host_ptr(p.addr) : nullptr) : r.blob<P_Small>();
                    std::string words;
                    for (uint32_t i = 0; src && i * 4 < p.c && i < 64; ++i) { char t[12]; uint32_t w; std::memcpy(&w, src + 4 * i, 4); std::snprintf(t, sizeof t, " %08x", w); words += t; }
                    TRACE("push %s%s stage %u offset 0x%x size 0x%x:%s", (p.a & kPushCompute) ? "compute" : "graphics", deferred ? " deferred" : "", p.a & ~kPushCompute, p.b, p.c,
                          words.c_str());
                }
                small_apply(S.small, p.kind, p.a, p.b, p.c, p.addr, r.blob<P_Small>(), blob_bytes);
                g_dirty |= kDynRaster | kDynVp;
                break;
            }
            case Op::SetRenderTargets: {
                const auto& p = r.payload<P_RenderTargets>();
                S.ncolor = p.count;
                for (uint32_t i = 0; i < 8; ++i) {
                    S.color_tex[i] = i < p.count ? Ref<Texture>::share(p.color[i].tex) : Ref<Texture>();
                    if (i < p.count) { S.color_view[i] = p.color[i].view; S.color_has_view[i] = p.color[i].has_view != 0; }
                    else S.color_has_view[i] = false;
                    const TextureViewInfo& v = S.color_view[i];
                    const bool hv = S.color_has_view[i];
                    S.color_level[i] = hv && v.has_levels ? static_cast<uint32_t>(v.min_level) : 0;
                    S.color_layer[i] = hv && v.has_layers ? static_cast<uint32_t>(v.min_layer) : 0;
                    S.color_layers[i] = hv && v.has_layers ? static_cast<uint32_t>(v.num_layers) : (S.color_tex[i] ? static_cast<uint32_t>(S.color_tex[i]->layers) : 0u);
                    if (S.color_tex[i]) tev(S.color_tex[i].get(), "render_target", i, 0);
                }
                S.depth_tex = Ref<Texture>::share(p.depth.tex);
                S.depth_view = p.depth.view;
                S.depth_has_view = p.depth.has_view != 0;
                break;
            }
            case Op::DrawArrays: do_draw_arrays(r.payload<P_DrawArrays>()); break;
            case Op::DrawElements: do_draw_elements(r.payload<P_DrawElements>()); break;
            case Op::DrawIndirect: do_draw_indirect(r.payload<P_DrawIndirect>()); break;
            case Op::SubmitDeviceCommands: do_submit_device_commands(r.payload<P_SubmitDeviceCommands>(), depth); break;
            case Op::DrawXfb: do_draw_xfb(r.payload<P_DrawXfb>()); break;
            case Op::DrawMeshTasks: do_draw_mesh(r.payload<P_DrawMesh>()); break;
            case Op::PushDebugGroup:
                if (device().features().debug_labels && vkCmdBeginDebugUtilsLabelEXT) {
                    VkDebugUtilsLabelEXT l = {VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT};
                    l.pLabelName = reinterpret_cast<const char*>(r.blob<P_DebugGroup>());
                    vkCmdBeginDebugUtilsLabelEXT(F->cb, &l);
                    ++g_dbg_depth;
                }
                break;
            case Op::PopDebugGroup:
                if (g_dbg_depth > 0 && vkCmdEndDebugUtilsLabelEXT) { vkCmdEndDebugUtilsLabelEXT(F->cb); --g_dbg_depth; }   // a pop whose push was in an earlier submission has nothing to close
                break;
            case Op::DrawTexture: note_unsupported("DrawTexture"); break;
            case Op::ClearColor: do_clear_color(r.payload<P_ClearColor>()); break;
            case Op::ClearDepthStencil: do_clear_depth_stencil(r.payload<P_ClearDepthStencil>()); break;
            case Op::ClearTexture: do_clear_texture(r.payload<P_ClearTexture>()); break;
            case Op::ClearBuffer: {
                const auto& p = r.payload<P_ClearBuffer>();
                end_pass();
                BufRef b = resolve_buffer(p.address, p.size);
                if (b.ok) vkCmdFillBuffer(F->cb, b.buffer, b.offset, p.size & ~3ull, p.value);
                if (b.ok) gpu_wrote(p.address, p.size, kWriteFill);
                memory_barrier(F->cb);
                counters().clears.fetch_add(1);
                gstamp("fill buffer");
                break;
            }
            case Op::CopyBufferToBuffer: {
                const auto& p = r.payload<P_CopyBufferToBuffer>();
                end_pass();
                BufRef s = held_or_pool(p.src, p.size, kOutBufferRead, true), d = resolve_buffer(p.dst, p.size);
                if (s.ok && d.ok) {
                    VkBufferCopy c = {s.offset, d.offset, p.size};
                    vkCmdCopyBuffer(F->cb, s.buffer, d.buffer, 1, &c);
                    gpu_wrote(p.dst, p.size, kWriteCopy);
                }
                memory_barrier(F->cb);
                counters().copies.fetch_add(1);
                gstamp("copy buffer");
                break;
            }
            case Op::CopyBufferToTexture: do_copy_buffer_texture(r.payload<P_CopyBufTex>(), true); break;
            case Op::CopyTextureToBuffer: do_copy_buffer_texture(r.payload<P_CopyBufTex>(), false); break;
            case Op::CopyTextureToTexture: do_copy_texture_texture(r.payload<P_CopyTexTex>()); break;
            case Op::Downsample: do_downsample(r.payload<P_Downsample>()); break;
            case Op::EvaluateDlss: do_evaluate_dlss(r.payload<P_EvaluateDlss>()); break;
            case Op::UpdateUniformBuffer: {
                const auto& p = r.payload<P_UpdateUniformBuffer>();
                const uint8_t* data = r.blob<P_UpdateUniformBuffer>();
                const uint32_t n = static_cast<uint32_t>(p.size & ~3ull);
                if (n && !ubo_update_cpu(p.address, p.buffer_size, static_cast<uint64_t>(p.offset), data, n)) {
                    desc_epoch_bump(kBumpOp);   // recorded as a GPU write
                    do_write_memory(p.address + static_cast<uint64_t>(p.offset), data, n);
                }
                else if (n)
                    g_dirty |= kDesc;   // the uniform bindings of the range get the new snapshot
                break;
            }
            case Op::Dispatch: { const auto& p = r.payload<P_Dispatch>(); do_dispatch(p.x, p.y, p.z, 0); break; }
            case Op::DispatchIndirect: do_dispatch(0, 0, 0, r.payload<P_DispatchIndirect>().address); break;
            case Op::ReportCounter: {
                const auto& p = r.payload<P_Counter>();
                struct { uint64_t counter, timestamp; } rep = {1, timestamp_ns()};   // counters other than the ones handled below are reported as non-zero
                if (p.type == NVN_COUNTER_TYPE_SAMPLES_PASSED && occ_ready() && occlusion_in_pass_on() && (g_occ_active < 0 || g_occ_in_pass)) {
                    if (g_occ_active >= 0) {
                        vkCmdEndQuery(F->cb, g_occ_pool, static_cast<uint32_t>(g_occ_active));
                        occ_part_done();
                    }
                    const bool split = g_occ_parts_drawn > 1;
                    if (split) ++g_occ_split;
                    const int32_t slot = g_occ_result_slot >= 0 ? g_occ_result_slot : g_occ_any_slot >= 0 ? g_occ_any_slot : g_occ_last;
                    g_occ_last = slot;
                    g_occ_want = false;
                    g_occ_result_slot = g_occ_any_slot = -1;
                    g_occ_parts_drawn = 0;
                    if (p.address) {
                        g_reports[p.address] = kReportUnknown;   // the query result lands on the GPU
                        gpu_wrote(p.address, sizeof rep, kWriteUpdate);
                        g_occ_pend.push_back({p.address, rep.timestamp, split ? -1 : slot, split ? kOccSplitCount : 0});
                        if (!P.open && !occ_defer_on()) occ_flush_pending();
                    }
                    ++g_occ_fast;
                    break;
                }
                if (p.type == NVN_COUNTER_TYPE_SAMPLES_PASSED && occ_ready()) {
                    ++g_occ_slow;
                    end_pass();
                    if (g_occ_active >= 0) {
                        vkCmdEndQuery(F->cb, g_occ_pool, static_cast<uint32_t>(g_occ_active));
                        g_occ_last = g_occ_active;
                        g_occ_active = -1;
                    }
                    if (p.address) {
                        rep.counter = 0;
                        g_reports[p.address] = kReportUnknown;   // the query result lands on the GPU
                        do_write_memory(p.address, &rep, sizeof rep);
                        BufRef b = resolve_buffer(p.address, sizeof rep);
                        if (b.ok && g_occ_last >= 0) {
                            vkCmdCopyQueryPoolResults(F->cb, g_occ_pool, static_cast<uint32_t>(g_occ_last), 1, b.buffer, b.offset, 16,
                                                      VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
                            memory_barrier(F->cb);
                        }
                    }
                    break;
                }
                if (p.type == NVN_COUNTER_TYPE_PRIMITIVES_GENERATED || p.type == NVN_COUNTER_TYPE_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN) {
                    CounterWindow& cw = g_cw[p.type == NVN_COUNTER_TYPE_PRIMITIVES_GENERATED ? 0 : 1];
                    if (cw.active >= 0) {   // the GPU counted this window (it holds indirect or feedback draws): the report is the query's result
                        end_pass();
                        cw_end_query(cw, &cw == &g_cw[1]);
                        cw.last = cw.active;
                        cw.active = -1;
                        if (p.address) {
                            rep.counter = 0;
                            g_reports[p.address] = kReportUnknown;   // the result lands on the GPU
                            do_write_memory(p.address, &rep, sizeof rep);
                            BufRef b = resolve_buffer(p.address, sizeof rep);
                            if (b.ok) {
                                // the feedback query has two results (written, needed): the second would land on the timestamp word, which is written again after it
                                vkCmdCopyQueryPoolResults(F->cb, cw.pool, static_cast<uint32_t>(cw.last), 1, b.buffer, b.offset, 16, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
                                memory_barrier(F->cb);
                                if (&cw == &g_cw[1]) do_write_memory(p.address + 8, &rep.timestamp, 8);
                            }
                        }
                        break;
                    }
                    rep.counter = p.type == NVN_COUNTER_TYPE_PRIMITIVES_GENERATED ? g_prims_generated : g_prims_xfb;
                    note_report(p.address, rep.counter);
                    if (p.address) do_write_memory(p.address, &rep, sizeof rep);
                    break;
                }
                // TIMESTAMP_TOP is a timestamp too (taken when the command reaches the top of the pipe): its counter word is 0 like TIMESTAMP's
                const bool stamp = p.type == NVN_COUNTER_TYPE_TIMESTAMP || p.type == NVN_COUNTER_TYPE_TIMESTAMP_TOP;
                if (!stamp) note_unsupported("statistics counters other than samples passed, primitives generated and feedback primitives written are reported as non-zero");
                if (stamp) rep.counter = 0;
                note_report(p.address, rep.counter);
                if (p.address) do_write_memory(p.address, &rep, sizeof rep);
                break;
            }
            case Op::ReportValue: {
                const auto& p = r.payload<P_ReportValue>();
                struct { uint64_t counter, timestamp; } rep = {p.value, timestamp_ns()};
                note_report(p.address, rep.counter);
                if (p.address) do_write_memory(p.address, &rep, sizeof rep);
                break;
            }
            case Op::ResetCounter: {
                const auto& p = r.payload<P_Counter>();
                if (p.type == NVN_COUNTER_TYPE_SAMPLES_PASSED && occ_ready() && occlusion_in_pass_on() && (g_occ_active < 0 || g_occ_in_pass)) {
                    if (g_occ_active >= 0) {                      // an unreported count is dropped
                        vkCmdEndQuery(F->cb, g_occ_pool, static_cast<uint32_t>(g_occ_active));
                        g_occ_active = -1;
                        g_occ_in_pass = false;
                    }
                    g_occ_result_slot = g_occ_any_slot = g_occ_last = -1;
                    g_occ_parts_drawn = 0;
                    g_occ_want = true;
                    if (P.open && !occ_begin_in_pass()) {
                        ++g_occ_slow;
                        end_pass();                                // no slot reset ahead: one pass break to reset a run (the part begins with the next pass)
                        occ_reset_ahead();
                    } else if (!P.open) {
                        occ_reset_ahead();
                    }
                } else if (p.type == NVN_COUNTER_TYPE_SAMPLES_PASSED && occ_ready()) {
                    end_pass();
                    if (g_occ_active >= 0) vkCmdEndQuery(F->cb, g_occ_pool, static_cast<uint32_t>(g_occ_active));
                    const bool ready = static_cast<int32_t>(g_occ_reset_hi - g_occ_next) > 0;
                    const uint32_t slot = g_occ_next++ % kOccSlots;
                    if (!ready) vkCmdResetQueryPool(F->cb, g_occ_pool, slot, 1);
                    occ_reset_ahead();                     // outside the pass now: slots for the queries of the passes to come
                    vkCmdBeginQuery(F->cb, g_occ_pool, slot, device().features().occlusion_query_precise ? VK_QUERY_CONTROL_PRECISE_BIT : 0);
                    g_occ_active = static_cast<int32_t>(slot);
                    g_occ_in_pass = false;
                    g_occ_last = -1;
                } else if (p.type == NVN_COUNTER_TYPE_PRIMITIVES_GENERATED || p.type == NVN_COUNTER_TYPE_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN) {
                    const bool gen = p.type == NVN_COUNTER_TYPE_PRIMITIVES_GENERATED;
                    (gen ? g_prims_generated : g_prims_xfb) = 0;
                    CounterWindow& cw = g_cw[gen ? 0 : 1];
                    if (cw.active >= 0) { end_pass(); cw_end_query(cw, !gen); cw.active = -1; }   // an unreported window is over
                    if (counter_window_has_gpu_draw(r, p.type) && cw_ready(cw, !gen)) {
                        static bool noted = false;
                        if (!noted) { noted = true; NXLOG_I("nvn.replay", "replay: primitive counter %u counted by a GPU query (first): its window holds an indirect or feedback draw", p.type); }
                        end_pass();
                        const uint32_t slot = cw.next++ % kCounterSlots;
                        vkCmdResetQueryPool(F->cb, cw.pool, slot, 1);
                        if (gen) vkCmdBeginQuery(F->cb, cw.pool, slot, 0);
                        else vkCmdBeginQueryIndexedEXT(F->cb, cw.pool, slot, 0, 0);
                        cw.active = static_cast<int32_t>(slot);
                    }
                }
                break;
            }
            case Op::Barrier: do_barrier(r.payload<P_Barrier>().bits); break;
            case Op::FenceSync: {
                const auto& p = r.payload<P_Sync>();
                if (p.sync) { p.sync->serial.store(F->serial); p.sync->armed.store(true); }
                break;
            }
            case Op::WaitSync: break;
            case Op::SignalEvent: {
                const auto& p = r.payload<P_Event>();
                if (p.event && p.event->pool && p.event->pool->base) {
                    Event* ev = p.event;
                    uint32_t v = p.value;
                    if (p.mode != NVN_EVENT_SIGNAL_MODE_WRITE) {
                        // ADD: the GPU cannot add into memory; the sum is computed here from the value after the signals recorded so far
                        uint32_t base = 0;
                        if (ev->shadow_valid.load()) base = ev->shadow.load();
                        else std::memcpy(&base, ev->pool->base + ev->offset, 4);
                        v = base + p.value;
                    }
                    ev->shadow.store(v);
                    ev->shadow_valid.store(true);
                    do_write_memory(ev->pool->gpu_base + static_cast<uint64_t>(ev->offset), &v, 4);
                }
                break;
            }
            case Op::WaitEvent: note_unsupported("GPU-side WaitEvent"); break;
            case Op::CallCommands: {
                const auto& p = r.payload<P_CallCommands>();
                const uint64_t* h = reinterpret_cast<const uint64_t*>(r.blob<P_CallCommands>());
                for (uint32_t i = 0; i < p.count; ++i) {
                    Ref<CmdList> l = handle_lookup(h[i]);
                    if (l) run_list(*l, depth + 1);
                    else log_once("missing-handle", "CallCommands of an unknown handle skipped");
                }
                break;
            }
            case Op::DiscardColor: case Op::DiscardDepthStencil: break;
            case Op::ResolveDepth: note_unsupported("ResolveDepthBuffer"); break;
            case Op::BeginXfb: case Op::EndXfb: case Op::PauseXfb: case Op::ResumeXfb: {
                const auto& p = r.payload<P_Counter>();
                static std::unordered_map<std::string, bool> seen;
                const std::string key = std::string(op_name(r.op())) + (S.stage[kVertex] ? S.stage[kVertex]->hash : "-");
                if (seen.emplace(key, true).second)
                    NXLOG_I("nvn.xfb", "%s control %llx vs %s xfb buffer0 %llx/%llu", op_name(r.op()), static_cast<unsigned long long>(p.address),
                            S.stage[kVertex] ? S.stage[kVertex]->hash.c_str() : "-", static_cast<unsigned long long>(S.xfb[0].addr),
                            static_cast<unsigned long long>(S.xfb[0].size));
                TRACE("%s control %llx", op_name(r.op()), static_cast<unsigned long long>(p.address));
                if (!device().features().transform_feedback) {
                    note_unsupported("transform feedback (the device has no VK_EXT_transform_feedback)");
                    break;
                }
                if (r.op() == Op::BeginXfb) {
                    xfb_end_in_pass();    // a section still open in the pass ends where the new one starts
                    auto sit = g_xfb_slot_of.find(p.address);
                    if (sit == g_xfb_slot_of.end()) {
                        if (g_xfb_slot_next >= kXfbSlots) {   // all counters in use: start over (a DrawTransformFeedback of an older control buffer then draws nothing)
                            g_xfb_slot_of.clear();
                            g_xfb_slot_next = 0;
                        }
                        sit = g_xfb_slot_of.emplace(p.address, g_xfb_slot_next++).first;
                    }
                    g_xfb_cur = sit->second;
                    g_xfb_slot_written[g_xfb_cur] = false;
                    g_xfb_active = true;
                    g_xfb_have_counter = false;
                } else if (r.op() == Op::EndXfb) {
                    xfb_end_in_pass();
                    g_xfb_active = false;
                    g_xfb_have_counter = false;
                }
                break;
            }
            default: note_unsupported(op_name(r.op())); break;
        }
    }
}

// ---- texture look-ahead ([nvn] tex_lookahead) -------------------------------------------------------------------------------------
// At a camera cut or an area entry a frame first uses hundreds of textures. The background preparation (nvn_texprep.h) only starts a job
// when the game binds a texture with nvnCommandBufferBindTexture* or registers a big one; OoT's materials name theirs by handles in uniform
// blocks and handle expressions, so the render thread read, hashed and untiled (BC7 cache read) nearly all of them itself, one after the
// other, as its draws reached them (~3 ms each, 0.25-1.2 s per cut). Here a thread of its own walks every submission when the game submits
// it (the render thread replays it later: the previous frame is usually still in flight), with the bindings that matter for textures
// (programs, uniform and storage blocks, bound handles, the texture pool) simulated on the side, resolves each draw's and dispatch's
// texture bindings like resolve_texture, and names the textures the CPU wrote since their last upload to the background preparation
// (texture_bound). Nothing here touches the replay's state; a wrong guess only costs a wasted job: texprep::take checks generation and
// content hash before a result is used. Storage-held handles (HandleExpr storage words, which may be GPU results) are not followed.
struct TexLook {
    std::mutex mu;
    std::condition_variable cv;
    std::deque<std::pair<std::vector<Ref<CmdList>>, uint64_t>> q;
    std::atomic<uint64_t> replay_serial{0};   // the serial the render thread is replaying (replay_begin_submit)
    // simulated state, carried from submission to submission in submission order (scanner thread only)
    const ShaderInfo* stage[kStages] = {};
    GState::Bind ubo[kStages][16], ssbo[kStages][16];
    uint64_t tex[kStages][64] = {};
    Ref<TexturePool> tpool;
    FlatMap64<uint8_t> seen;                   // per scan: (pool, texture id) looked at
    // statistics
    uint64_t scans = 0, late = 0, cmds = 0, works = 0, resolved = 0, named = 0, unknown = 0, dropped = 0;
    uint64_t scan_ns = 0, last_log_ns = 0;
};
TexLook* g_tl = nullptr;

bool tex_lookahead_on() {
    static const bool on = nxrt::config_get_bool("nvn", "tex_lookahead", true);
    return on;
}

void tl_name_textures(TexLook& L, const uint32_t stage_bits) {
    for (int st = 0; st < kStages; ++st) {
        if (!(stage_bits & (1u << st))) continue;
        const ShaderInfo* sh = L.stage[st];
        if (!sh) continue;
        const auto& bs = sh->refl.bindings;
        for (size_t i = 0; i < bs.size(); ++i) {
            const DescriptorBinding& b = bs[i];
            if (b.type != DescriptorType::CombinedImageSampler || !texture_source(b.nvn.source)) continue;
            uint64_t handle = 0;
            if (b.nvn.source == NvnSource::SamplerBinding) {
                if (b.nvn.index >= 0 && b.nvn.index < 64) handle = L.tex[st][b.nvn.index];
            } else if (b.nvn.source == NvnSource::HandleInUniform) {
                handle = read_handle_word_in(L.ubo[st], b.nvn.bank, b.nvn.word);
            } else {
                BindingDesc d;
                d.shader = sh;
                d.index = static_cast<int>(i);
                d.nvn_stage = st;
                bool unknown = false;
                handle = eval_handle_expr_in(d, b.nvn.expr, L.ubo[st], L.ssbo[st], &unknown);
                if (unknown) { ++L.unknown; continue; }
            }
            TexturePool* tp = L.tpool.get();
            const uint32_t tid = handle_texture_id(handle);
            if (!tp || static_cast<int>(tid) >= tp->count) continue;
            const uint64_t key = (reinterpret_cast<uint64_t>(tp) * 0x9E3779B97F4A7C15ull) ^ (static_cast<uint64_t>(tid) + 1);
            if (L.seen.find(key)) continue;
            L.seen.put(key, 1);
            ++L.resolved;
            Ref<Texture> t;
            {
                std::shared_lock<std::shared_mutex> lk(tp->lock);
                if (tid < tp->entries.size()) {
                    const TexturePool::Entry& e = tp->entries[tid];
                    if (e.used && e.tex) t = e.tex;
                }
            }
            if (t && t->cpu_dirty.load(std::memory_order_relaxed) && !t->destroyed.load(std::memory_order_relaxed)) {
                ++L.named;
                texture_bound(t.get());
            }
        }
    }
}

// False: the render thread has begun this submission (stop: what is left helps little, and its objects are the replay's from now on).
bool tl_scan(TexLook& L, const CmdList& list, int depth, uint64_t serial) {
    if (depth > kMaxDepth) return true;
    uint32_t n = 0;
    for (RecordReader r(list); r.next();) {
        ++L.cmds;
        if ((++n & 255) == 0 && L.replay_serial.load(std::memory_order_relaxed) >= serial) return false;
        switch (r.op()) {
            case Op::BindProgram: {
                const auto& p = r.payload<P_BindProgram>();
                const uint32_t bits = p.stages | ((p.stages & ((1u << kCompute) - 1)) ? ((1u << kTask) | (1u << kMesh)) : 0u);
                for (int st = 0; st < kStages; ++st)
                    if (bits & (1u << st)) L.stage[st] = p.program ? p.program->stage[st] : nullptr;
                break;
            }
            case Op::BindBuffers: {
                const auto& p = r.payload<P_BindBuffers>();
                const NVNbufferRange* rg = reinterpret_cast<const NVNbufferRange*>(r.blob<P_BindBuffers>());
                for (uint32_t i = 0; i < p.count; ++i) {
                    const uint32_t k = p.first + i;
                    if (p.stage >= kStages || k >= 16) continue;
                    if (p.kind == kBufUniform) L.ubo[p.stage][k] = {physical_buffer_address(rg[i].address, rg[i].size), rg[i].size};
                    else if (p.kind == kBufStorage) L.ssbo[p.stage][k] = {physical_buffer_address(rg[i].address, rg[i].size), rg[i].size};
                }
                break;
            }
            case Op::BindTextures: {
                const auto& p = r.payload<P_BindTextures>();
                const uint64_t* h = reinterpret_cast<const uint64_t*>(r.blob<P_BindTextures>());
                for (uint32_t i = 0; i < p.count; ++i)
                    if (p.stage < kStages && p.first + i < 64) L.tex[p.stage][p.first + i] = h[i];
                break;
            }
            case Op::SetTexturePool: L.tpool = Ref<TexturePool>::share(static_cast<TexturePool*>(r.payload<P_Pool>().pool)); break;
            case Op::DrawArrays: case Op::DrawElements: case Op::DrawIndirect: case Op::DrawMeshTasks: case Op::DrawXfb:
                ++L.works;
                tl_name_textures(L, 0xffu & ~(1u << kCompute));
                break;
            case Op::Dispatch: case Op::DispatchIndirect:
                ++L.works;
                tl_name_textures(L, 1u << kCompute);
                break;
            case Op::CallCommands: {
                const auto& p = r.payload<P_CallCommands>();
                const uint64_t* h = reinterpret_cast<const uint64_t*>(r.blob<P_CallCommands>());
                for (uint32_t i = 0; i < p.count; ++i) {
                    Ref<CmdList> l = handle_lookup(h[i]);
                    if (l && !tl_scan(L, *l, depth + 1, serial)) return false;
                }
                break;
            }
            default: break;
        }
    }
    return true;
}

// The scanner reads guest memory (uniform blocks) the game may unmap: a fault ends that scan.
bool tl_scan_guarded(TexLook& L, const CmdList& list, uint64_t serial, bool& faulted) {
    __try {
        return tl_scan(L, list, 0, serial);
    } __except (1 /* EXCEPTION_EXECUTE_HANDLER (windows.h is not included here) */) {
        faulted = true;
        return false;
    }
}

void tl_main() {
    log_set_thread_name("nvn-texlook");
    TexLook& L = *g_tl;
    for (;;) {
        std::vector<Ref<CmdList>> lists;
        uint64_t serial = 0;
        {
            std::unique_lock<std::mutex> lk(L.mu);
            L.cv.wait(lk, [&] { return !L.q.empty(); });
            lists = std::move(L.q.front().first);
            serial = L.q.front().second;
            L.q.pop_front();
        }
        if (L.replay_serial.load(std::memory_order_relaxed) >= serial) { ++L.late; continue; }
        if (!ab_on(kAbTexLook)) continue;          // an "off" window of [nvn] cpuperf_ab (the flag is the render thread's; a stale read only shifts a window edge)
        const uint64_t t0 = ft::now_ns();
        static const uint16_t zid = ft::zone_id("tex.lookahead");
        ft::Scope zs(zid, 0, serial);
        L.seen.clear();
        g_tl_pages.clear();
        ++L.scans;
        for (const Ref<CmdList>& l : lists) {
            bool faulted = false;
            if (!l || !tl_scan_guarded(L, *l, serial, faulted)) {
                if (!faulted) ++L.late;
                break;
            }
        }
        const uint64_t t1 = ft::now_ns();
        L.scan_ns += t1 - t0;
        if (t1 - L.last_log_ns > 10ull * 1000000000ull) {
            L.last_log_ns = t1;
            NXLOG_I(kTag, "texture look-ahead: %llu scans (%llu overtaken by the replay, %llu dropped), %.2f ms per scan, %llu commands, %llu draws/dispatches, "
                          "%llu textures resolved, %llu named to the background preparation, %llu storage-held handles not followed",
                    static_cast<unsigned long long>(L.scans), static_cast<unsigned long long>(L.late), static_cast<unsigned long long>(L.dropped),
                    L.scans ? static_cast<double>(L.scan_ns) / static_cast<double>(L.scans) * 1e-6 : 0.0, static_cast<unsigned long long>(L.cmds),
                    static_cast<unsigned long long>(L.works), static_cast<unsigned long long>(L.resolved), static_cast<unsigned long long>(L.named),
                    static_cast<unsigned long long>(L.unknown));
        }
    }
}

}  // namespace

// The game thread, after queueing a submission: the look-ahead scans it (above).
void texlook_submit(const std::vector<Ref<CmdList>>& lists, uint64_t serial) {
    if (!tex_lookahead_on() || lists.empty()) return;
    static std::once_flag once;
    std::call_once(once, [] {
        g_tl = new TexLook();   // never destroyed: the thread runs until the process ends
        std::thread(tl_main).detach();
        NXLOG_I(kTag, "texture look-ahead on: submissions are scanned for textures to prepare in the background ([nvn] tex_lookahead)");
    });
    {
        std::lock_guard<std::mutex> lk(g_tl->mu);
        while (g_tl->q.size() >= 16) { g_tl->q.pop_front(); ++g_tl->dropped; }
        g_tl->q.emplace_back(lists, serial);
    }
    g_tl->cv.notify_one();
}

bool ab_tex_park_on() { return ab_on(kAbTexPark); }   // [nvn] cpuperf_ab_what tex_park (nvn_gpu_texture.cpp)
bool ab_overlap_grow_on() { return ab_on(kAbOverlapGrow); }
bool ab_index_inc_on() { return ab_on(kAbIndexInc); }   // [nvn] cpuperf_ab_what index_inc (nvn_gpu_pool.cpp)   // [nvn] cpuperf_ab_what overlap_grow (nvn_gpu_pool.cpp)
bool ab_out_delta_on() { return ab_on(kAbOutDelta); }  // out_delta (nvn_gpu_pool.cpp delta copy-backs)
// [display] unpaced_loading (default on): the limiter stops waiting while the game loads, since its loading code advances once per
// presented frame. Loading = presented frames with fewer than 16 draws (boot's black screen draws 3: exposure, bloom, tone map; the
// loading screen a few more) AND the game still stalling: a present gap over 100 ms in the last 1.5 s. A light screen that presents
// steadily (the title screen, a menu) is paced again within 1.5 s, so it does not run at 1000 fps; so is any frame with 16+ draws.
// A movie is never loading: while the game reads decoded pictures (nn::codec, last read under 1 s ago) the frames stay paced. A movie
// frame draws only a few quads, and OoT's players advance one picture per presented frame: unpaced, the 5-minute intro ran at ~250
// pictures/s for 20 s (and for 1.5 s after each stall) while its voice lines, started along the movie's timeline, piled up (A11).
void note_frame_for_pacing() {
    static const bool on = nxrt::config_get_bool("display", "unpaced_loading", true);
    if (!on) return;
    using clk = std::chrono::steady_clock;
    static uint64_t last_p = 0, last_d = 0;
    static int low = 0;
    static clk::time_point last_present{}, last_stall{}, low_since{};
    const uint64_t p = counters().presents.load(std::memory_order_relaxed);
    if (p == last_p) return;
    const auto now = clk::now();
    const uint64_t d = counters().draws.load(std::memory_order_relaxed) + counters().draws_skipped.load(std::memory_order_relaxed);
    const uint64_t per = (d - last_d) / (p - last_p);
    last_p = p;
    last_d = d;
    if (last_present == clk::time_point{} || now - last_present > std::chrono::milliseconds(100)) last_stall = now;
    last_present = now;
    static uint64_t movie_read = 0;
    static clk::time_point movie_seen{};
    if (const uint64_t r = nxrt::media::video_codec_stats().frames_read; r != movie_read) {
        movie_read = r;
        movie_seen = now;
    }
    const bool movie = movie_seen != clk::time_point{} && now - movie_seen < std::chrono::seconds(1);
    low = per < 16 && !movie ? low + 1 : 0;
    if (low == 1) low_since = now;
    // A loading screen presents steadily but its loading still advances once per frame (Pokopia: 14 s at 60 fps before the title), so a
    // run of light frames stays unpaced for up to [display] unpaced_loading_s (20) seconds even without stalls.
    static const double max_s = nxrt::config_get_double("display", "unpaced_loading_s", 20.0);
    const bool want = low >= 3 && (now - last_stall < std::chrono::milliseconds(1500) ||
                                   now - low_since < std::chrono::duration<double>(max_s));
    if (want != nxrt::frame_limiter_unpaced()) {
        nxrt::frame_limiter_set_unpaced(want);
        ft::mark(want ? "pace.loading" : "pace.playing", static_cast<uint32_t>(per));
        NXLOG_I(kTag, "pacing %s (present %llu, %llu draws)", want ? "off: loading" : movie ? "on: movie" : "on", static_cast<unsigned long long>(p),
                static_cast<unsigned long long>(per));
    }
    // A 3D scene starts: the first frame with 1000+ draws after 60 lighter ones (tests wait for this line; the pacing lines above only
    // come on a change, and a scene that follows a movie is paced already).
    static uint64_t light = 0;
    if (per >= 1000) {
        if (light >= 60)
            NXLOG_I(kTag, "3D scene (present %llu, %llu draws)", static_cast<unsigned long long>(p), static_cast<unsigned long long>(per));
        light = 0;
    } else {
        light++;
    }
    static bool full = false;
    if (!full && per >= 16) {
        full = true;
        NXLOG_I(kTag, "first frame with 16+ draws (present %llu, %llu draws)", static_cast<unsigned long long>(p), static_cast<unsigned long long>(per));
    }
}

void replay_begin_submit(Frame& f) {
    gpl_note_presents(counters().presents.load(std::memory_order_relaxed));   // [nvn] gpl_async defers draws only while frames are flowing
    note_frame_for_pacing();
    F = &f;
    if (g_tl) g_tl->replay_serial.store(f.serial, std::memory_order_relaxed);   // the texture look-ahead stops scanning this submission
    cmdq::begin_submission(ab_on(kAbCmdThread));   // [nvn] cmd_thread: a recorder thread makes the vkCmd* calls (nvn_cmdq.cpp)
    hazard::begin_submission(f.cb, ab_on(kAbHazard));   // [nvn] hazard: barriers where commands touch what others wrote (nvn_hazard.cpp)
    tex_poll_cpu_writes();
    if (extras::LowLatency* ll = extras::lowlat()) ll->submit_begin();     // RENDERSUBMIT_START of the frame (S14)
    if (g_trace_frame == -2) {
        // [nvn] trace_frame = "6050", "6050,9300" or a range "1590-1660": every listed presented frame is traced (the first one is the main one)
        const std::string list = nxrt::config_get_str("nvn", "trace_frame", "");
        g_trace_frame = -1;
        size_t i = 0;
        while (i < list.size()) {
            size_t j = list.find(',', i);
            if (j == std::string::npos) j = list.size();
            const std::string item = list.substr(i, j - i);
            const size_t dash = item.find('-', 1);
            const long long lo = std::atoll(item.c_str()), hi = dash == std::string::npos ? lo : std::atoll(item.c_str() + dash + 1);
            for (long long v = lo; v <= hi; ++v) {
                if (g_trace_frame < 0) g_trace_frame = v; else g_trace_extra.push_back(v);
            }
            i = j + 1;
        }
    }
    if (!g_trigger_read) {
        g_trigger_read = true;
        g_trigger_fs = nxrt::config_get_str("nvn", "trace_after_fs", "");
    }
    if (!g_trigger_fs.empty() && g_trigger_seen && !g_trigger_done && static_cast<int64_t>(counters().presents.load()) > g_trigger_present) {
        g_trigger_done = true;
        g_trace_frame = static_cast<int64_t>(counters().presents.load());
    }
    {   // [nvn] trace_sec = N (NXRT_NVN_TRACE_SEC): trace the first frame presented N seconds after the first draw (frame numbers differ between runs)
        static const double trace_sec = static_cast<double>(nxrt::config_get_int("nvn", "trace_sec", 0));
        static const auto t0 = std::chrono::steady_clock::now();
        static bool trace_sec_done = false;
        if (trace_sec > 0 && !trace_sec_done && std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() >= trace_sec) {
            trace_sec_done = true;
            g_trace_frame = static_cast<int64_t>(counters().presents.load());
        }
    }
    if (g_trace_req.exchange(false)) g_trace_frame = static_cast<int64_t>(counters().presents.load()) + 1;   // armed from a script (@shot nvntrace_*)
    g_trace = g_trace_frame >= 0 && static_cast<int64_t>(counters().presents.load()) == g_trace_frame;
    for (int64_t e : g_trace_extra) g_trace |= static_cast<int64_t>(counters().presents.load()) == e;
    if (g_trace) { g_trace_n = 0; NXLOG_I("nvn.trace", "---- frame %lld ----", static_cast<long long>(counters().presents.load())); }
    {   // S38: device-local pool copies and tracked barriers. [nvn] gpuperf_ab = N: both are off in even windows of N presents and on in odd
        // ones (the paired-window measurement of S29); without it they follow their own settings.
        static const int64_t ab = config_get_int("nvn", "gpuperf_ab", 0);
        static const bool tracked = config_get_str("nvn", "pass_barriers", "tracked") != "full";
        static int last_on = -1;
        bool on = true;
        if (ab > 0) on = ((counters().presents.load() / static_cast<uint64_t>(ab)) & 1) != 0;
        static const int64_t cab = config_get_int("nvn", "cpuperf_ab", 0);
        if (cab > 0) {
            const bool con = ((counters().presents.load() / static_cast<uint64_t>(cab)) & 1) != 0;
            if (con != g_cpu_ab_on) {   // the render thread's CPU time so far (ms) with each switch: CPU cost per window when the GPU limits the fps
                ft::mark("cpuperf.rcpu", static_cast<uint32_t>(thread_cpu_us() / 1000));
                ft::mark(con ? "cpuperf.on" : "cpuperf.off", static_cast<uint32_t>(counters().presents.load()));
            }
            g_cpu_ab_on = con;
            cmdq::set_copies(ab_on(kAbCmdCopies));
        }
        if (static_cast<int>(on) != last_on) {
            last_on = on ? 1 : 0;
            if (ab > 0) ft::mark(on ? "gpuperf.on" : "gpuperf.off", static_cast<uint32_t>(counters().presents.load()));
        }
        g_tracked = on && tracked;
        set_narrow_barriers(g_tracked);
        g_pending.clear();
        g_pass_unsafe = false;
        mirror_begin_submit(f, on);
    }
    if (archexp::gmem_window_on()) archexp::gmem_window_refresh(f);   // E3 (measurement only)
    S.window_origin = window_origin_mode();
    S.depth_mode = depth_mode();
    g_draw_idx = 0;
    g_chunk_work = 0;
    desc_epoch_bump(kBumpSubmit);     // the sets of earlier submissions came from other slots' pools
    keep_begin_submit();
    g_odb_all_dirty = true;   // odb_incremental: a full check first in every submission
    g_chunk_t0 = ft::now_ns();
    P = Pass();
    g_set_cache.clear();
    g_ds_bvals.clear();
    g_bound_pipeline = VK_NULL_HANDLE;
    g_dirty = kAllDirty;
    g_pl = nullptr;
    g_pipe = VK_NULL_HANDLE;
    g_topo = g_pipe_topo = VK_PRIMITIVE_TOPOLOGY_MAX_ENUM;
    g_restart_valid = false;
    g_vb_bound_in_cb = false;
    g_want_depth_ro = false;
    g_fb_use.clear();
    g_fb_seen = false;
    g_key_dummy_binding = 0;
    // one support buffer per submission (Ryujinx SupportBuffer: render scales 1.0, no BGRA remap)
    g_support = f.upload(sizeof(SupportBuffer), 256);
    g_support_scaled.clear();
    g_upd_snaps.clear();
    g_c0_tables.clear();
    {
        static uint64_t last_log = 0;
        if (f.serial - last_log > 600) {
            last_log = f.serial;
            NXLOG_I(kTag, "uniform updates: %llu at record time, %llu on the GPU; %zu update ranges; bindings: %llu snapshots made, %llu reused, %llu from game memory",
                    (unsigned long long)g_upd_fast, (unsigned long long)g_upd_slow, g_upd.size(), (unsigned long long)g_upd_snapped,
                    (unsigned long long)g_upd_snap_hits, (unsigned long long)g_upd_direct);
            NXLOG_I(kTag, "graphics c[0] tables: %llu made, %llu shared; unaligned uniform copies %llu (%llu KiB); draws over held results served by filling the copy %llu (%llu KiB)",
                    (unsigned long long)g_c0_made_n, (unsigned long long)g_c0_reused_n, (unsigned long long)g_ubo_unaligned_n, (unsigned long long)(g_ubo_unaligned_bytes >> 10),
                    (unsigned long long)g_draw_fills, (unsigned long long)(g_draw_fill_bytes >> 10));
            NXLOG_I(kTag, "draw reads over held results not served by the copy: copy not allowed %llu (%llu KiB), no copy span %llu (%llu KiB), fill failed %llu (%llu KiB)",
                    (unsigned long long)g_dr_fail[0], (unsigned long long)(g_dr_fail_bytes[0] >> 10), (unsigned long long)g_dr_fail[1],
                    (unsigned long long)(g_dr_fail_bytes[1] >> 10), (unsigned long long)g_dr_fail[2], (unsigned long long)(g_dr_fail_bytes[2] >> 10));
            {   // the call sites that end passes, most first (cumulative): nvn_replay.cpp line, passes, draws in them
                std::vector<std::pair<uint64_t, uint32_t>> v;
                for (uint32_t i = 0; i < 8192; ++i) if (g_pass_end_line[i]) v.emplace_back(g_pass_end_line[i], i);
                std::sort(v.rbegin(), v.rend());
                std::string s;
                for (size_t i = 0; i < v.size() && i < 14; ++i) {
                    char b[64];
                    std::snprintf(b, sizeof b, " L%u %llu (%llu draws)", v[i].second, (unsigned long long)v[i].first, (unsigned long long)g_pass_end_draws[v[i].second]);
                    s += b;
                }
                NXLOG_I(kTag, "passes ended by:%s; memory writes kept to the pass end %llu (%llu flushes)", s.c_str(), (unsigned long long)g_wdefer_n,
                        (unsigned long long)g_wdefer_flushes);
                NXLOG_I(kTag, "pass_pre_layout: %llu images put into their sampled layout at a pass begin, %zu images known to break passes",
                        (unsigned long long)g_pre_layout_n, g_break_prone.size());
                NXLOG_I(kTag, "texture work that ended an open pass: %llu breaks; textures by old layout: general %llu, color attachment %llu, depth attachment %llu, "
                        "depth read-only %llu, transfer %llu, undefined %llu, other %llu; CPU-written (uploads) %llu, storage images %llu",
                        (unsigned long long)g_pt_breaks, (unsigned long long)g_pt_from[0], (unsigned long long)g_pt_from[1], (unsigned long long)g_pt_from[2],
                        (unsigned long long)g_pt_from[3], (unsigned long long)g_pt_from[4], (unsigned long long)g_pt_from[5], (unsigned long long)g_pt_from[6],
                        (unsigned long long)g_pt_dirty, (unsigned long long)g_pt_storage);
            }
            NXLOG_I(kTag, "occlusion reports: %llu in their pass, %llu with a pass break; %llu queries ended early by a pass break (split)",
                    (unsigned long long)g_occ_fast, (unsigned long long)g_occ_slow, (unsigned long long)g_occ_split);
            NXLOG_I(kTag, "game barriers: %llu, %llu in an open pass: %llu kept the pass (barrier_keep_pass, bits seen %x), %llu ended it after storage or stream-out writes (bits of all %x)",
                    (unsigned long long)g_bar_n, (unsigned long long)g_bar_in_pass, (unsigned long long)g_bar_kept, g_bar_bits_kept, (unsigned long long)g_bar_unsafe, g_bar_bits_all);
            {
                NXLOG_I(kTag, "dispatches: %llu, %llu right after another with no barrier between but ours (%llu indirect)", (unsigned long long)g_disp_n,
                        (unsigned long long)g_disp_after, (unsigned long long)g_disp_after_ind);
                NXLOG_I(kTag, "full barriers by site:%s", memory_barrier_sites(16).c_str());
                if (hazard::enabled()) NXLOG_I(kTag, "%s", hazard::stats_line().c_str());
            }
            NXLOG_I(kTag, "graphics descriptor set binds: %llu made, %llu skipped as already bound (bind_skip)", (unsigned long long)g_bs_bound, (unsigned long long)g_bs_skipped);
            {
                std::string why;
                std::vector<std::pair<uint64_t, std::string>> v;
                for (const auto& [k, n] : g_keep_bad_why) v.emplace_back(n, k);
                std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
                for (size_t i = 0; i < v.size() && i < 6; ++i) why += (i ? ", " : "") + v[i].second + " " + std::to_string(v[i].first);
                size_t dead = 0;
                for (const auto& [k, e] : g_keep) dead += e.dead ? 1 : 0;
                NXLOG_I(kTag, "kept descriptor sets (set_keep): %llu found valid, %llu first sightings, %llu new, %llu rebuilt after a failed check (pool generation %llu, uniform decision %llu, held result %llu, "
                        "texture %llu, sampler tuning %llu), %llu stopped after failing often, %llu not keepable (%s), %llu differed (verify); %zu entries (%zu not kept), "
                        "%llu sets in %zu pools this era, %llu eras, %llu dropped by age",
                        (unsigned long long)g_keep_hit, (unsigned long long)g_keep_first, (unsigned long long)g_keep_new, (unsigned long long)g_keep_rebuilt, (unsigned long long)g_keep_fail[0],
                        (unsigned long long)g_keep_fail[1], (unsigned long long)g_keep_fail[2], (unsigned long long)g_keep_fail[3], (unsigned long long)g_keep_fail[4],
                        (unsigned long long)g_keep_dead_n, (unsigned long long)g_keep_bad_n, why.c_str(), (unsigned long long)g_keep_verify_bad, g_keep.size(), dead,
                        (unsigned long long)g_keep_sets_era, g_keep_pools.size(), (unsigned long long)g_keep_eras, (unsigned long long)g_keep_evicted);
                NXLOG_I(kTag, "sets built outside the memo and set_keep: compute %llu, reuse off for the draw %llu, a set with written storage or a per-draw c[0] %llu, reusable (first sightings, not keepable) %llu",
                        (unsigned long long)g_set_paths[0], (unsigned long long)g_set_paths[1], (unsigned long long)g_set_paths[2], (unsigned long long)g_set_paths[3]);
                NXLOG_I(kTag, "sets that may not be reused, why: per-draw c[0] only %llu, written storage only %llu, both %llu, other %llu", (unsigned long long)g_set_why[0],
                        (unsigned long long)g_set_why[1], (unsigned long long)g_set_why[2], (unsigned long long)g_set_why[3]);
                NXLOG_I(kTag, "c0_persist: %llu table slots found, %llu written (era %llu), %llu hash collisions", (unsigned long long)g_c0_persist_hit,
                        (unsigned long long)g_c0_persist_new, (unsigned long long)g_c0_era, (unsigned long long)g_c0_collide);
                if (cmdq::enabled()) {
                    uint64_t pk = 0, dr = 0, di = 0, sp = 0;
                    cmdq::stats(&pk, &dr, &di, &sp);
                    NXLOG_I(kTag, "cmd_thread: %llu calls queued, %llu drains (%llu spins), %llu calls made directly after a drain (%s)", (unsigned long long)pk, (unsigned long long)dr,
                            (unsigned long long)sp, (unsigned long long)di, cmdq::direct_top().c_str());
                }
                NXLOG_I(kTag, "draw buffer checks (odb_incremental): %llu full, %llu incremental with %llu bindings checked, %llu missed (verify)", (unsigned long long)g_odb_full,
                        (unsigned long long)g_odb_inc, (unsigned long long)g_odb_inc_checked, (unsigned long long)g_odb_verify_bad);
                if (g_sig_var_on)
                    NXLOG_I(kTag, "set_keep_diag: %llu signatures not in the table; seen in an earlier present without uniform addresses %llu, without storage addresses %llu, "
                            "without texture handles %llu, without pools and targets %llu; none of these %llu; pool registry generation %llu", (unsigned long long)g_var_new, (unsigned long long)g_var_hit[0],
                            (unsigned long long)g_var_hit[1], (unsigned long long)g_var_hit[2], (unsigned long long)g_var_hit[3], (unsigned long long)g_var_none,
                            (unsigned long long)pool_registry_generation());
            }
            NXLOG_I(kTag, "pool array texture entries: %llu kept (pool_sync_cache), %llu asked for their view; %llu walks over %llu entries, %llu without an image",
                        (unsigned long long)g_psync_kept, (unsigned long long)g_psync_asked, (unsigned long long)g_psync_walks, (unsigned long long)g_psync_walked, (unsigned long long)g_psync_noimg);

            NXLOG_I(kTag, "resolve epoch moves by GPU write site: other %llu, gmem bound storage %llu, gmem pointer windows %llu, draws over held results %llu, graphics storage %llu, compute storage %llu, storage texel %llu",
                    (unsigned long long)g_bump_site[0], (unsigned long long)g_bump_site[1], (unsigned long long)g_bump_site[2], (unsigned long long)g_bump_site[3],
                    (unsigned long long)g_bump_site[4], (unsigned long long)g_bump_site[5], (unsigned long long)g_bump_site[6]);
            {
                uint64_t pk = 0, tk = 0, st = 0, ex = 0;
                park_stats(&pk, &tk, &st, &ex);
                uint64_t ms = 0, mse = 0, msb = 0;
                park_miss_stats(&ms, &mse, &msb);
                NXLOG_I(kTag, "parked texture images (tex_park): %llu parked, %llu taken back (%llu of them stale: uploaded again), %llu expired; new images of pool textures with none parked %llu (%llu of them with a key that expired lately, %llu of 1 MiB or more)", (unsigned long long)pk, (unsigned long long)tk, (unsigned long long)st, (unsigned long long)ex, (unsigned long long)ms, (unsigned long long)mse, (unsigned long long)msb);
            }
            NXLOG_I(kTag, "submissions: %llu, parts sent ahead while recording (submit_chunk_draws): %llu; draw buffer checks skipped %llu; pass look-aheads %llu settled %llu ranges; descriptor sets resolved %llu, %llu of them already in the submission's cache; %llu bound again without resolving (set_reuse), %llu differed (verify); resolve epoch moves: commands %llu, GPU writes %llu, passes %llu; set memo cleared by the epoch %llu, texture state %llu, results sent back %llu",
                    (unsigned long long)counters().submits.load(), (unsigned long long)counters().submit_chunks.load(), (unsigned long long)g_odb_skipped, (unsigned long long)g_la_scans, (unsigned long long)g_la_settled, (unsigned long long)g_sets_built, (unsigned long long)g_sets_cached,
                    (unsigned long long)g_sets_reused, (unsigned long long)g_sets_verify_bad, (unsigned long long)g_bump_n[kBumpOp],
                    (unsigned long long)g_bump_n[kBumpWrite], (unsigned long long)g_bump_n[kBumpPass], (unsigned long long)g_memo_clr[0], (unsigned long long)g_memo_clr[1],
                    (unsigned long long)g_memo_clr[2]);
            {   // the commands that moved the resolve epoch most often
                std::vector<std::pair<uint64_t, int>> v;
                for (int i = 0; i < 256; ++i) if (g_bump_op[i]) v.emplace_back(g_bump_op[i], i);
                std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
                std::string s;
                for (size_t i = 0; i < v.size() && i < 8; ++i) s += std::string(i ? ", " : "") + op_name(static_cast<Op>(v[i].second)) + " " + std::to_string(v[i].first);
                NXLOG_I(kTag, "resolve epoch moves by command: %s; by GPU write use: %llu %llu %llu %llu %llu %llu %llu %llu, update ranges %llu", s.c_str(),
                        (unsigned long long)g_bump_use[0], (unsigned long long)g_bump_use[1], (unsigned long long)g_bump_use[2], (unsigned long long)g_bump_use[3],
                        (unsigned long long)g_bump_use[4], (unsigned long long)g_bump_use[5], (unsigned long long)g_bump_use[6], (unsigned long long)g_bump_use[7], (unsigned long long)g_bump_use[8]);
            }
            {   // the three pass sizes with the most draws
                std::vector<std::pair<uint64_t, uint64_t>> v(g_pass_size_draws.begin(), g_pass_size_draws.end());
                std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
                std::string s;
                for (size_t i = 0; i < v.size() && i < 3; ++i) {
                    char b[64];
                    std::snprintf(b, sizeof b, "%s%ux%u (%llu draws)", i ? ", " : "", static_cast<unsigned>(v[i].first >> 32), static_cast<unsigned>(v[i].first & 0xffffffffu),
                                  static_cast<unsigned long long>(v[i].second));
                    s += b;
                }
                NXLOG_I(kTag, "render size: pass sizes with the most draws since the last line: %s", s.c_str());
                g_pass_size_draws.clear();
            }
        }
    }
    g_draw_support = UploadAlloc();
    if (g_support) {
        SupportBuffer sb = support_buffer_defaults();
        std::memcpy(g_support.ptr, &sb, sizeof sb);
    }
}

void replay_lists(Frame& f, const std::vector<Ref<CmdList>>& lists) {
    F = &f;
    for (const Ref<CmdList>& l : lists)
        if (l) run_list(*l, 0);
}

void replay_request_trace() { g_trace_req.store(true); }

void trace_dump_targets() {
    // The snapshot copies are in the frame's submissions, which the GPU may still be running at present time: reading their buffers
    // before it finished gave zeros for the frame's later draws (the colour-pass mesh records "read 0" on replays, 2026-11-07).
    if (!g_trace_snaps.empty()) vkDeviceWaitIdle(device().device());
    // [nvn] trace_snap_per_frame = 1: each traced frame's snapshots go to their own folder snap_f<present>/ (a trace_frame range then keeps
    // every frame's files instead of overwriting them: frame-to-frame comparisons, A18)
    static const bool per_frame = config_get_bool("nvn", "trace_snap_per_frame", false);
    std::string prefix;
    if (per_frame && !g_trace_snaps.empty()) {
        prefix = "snap_f" + std::to_string(counters().presents.load()) + "/";
        std::error_code ec;
        std::filesystem::create_directories(nxrt::instance_file(prefix), ec);
    }
    for (const TraceSnap& sn : g_trace_snaps) {
        std::string stats;
        const bool ok = snapshot_write(sn.index, prefix + sn.name, &stats);
        NXLOG_I("nvn.trace", "snapshot after draw %d: %s -> %s: %s", sn.draw, sn.desc.c_str(), ok ? sn.name.c_str() : "(not written)", ok ? stats.c_str() : "");
    }
    g_trace_snaps.clear();
    if (g_trace_targets.empty()) return;
    std::vector<Ref<Texture>> list;
    list.swap(g_trace_targets);
    for (const Ref<Texture>& t : list) {
        char buf[96];
        std::string stats;
        const std::string png = std::string("trace_t") + std::to_string(t->id) + "_" + (t->fmt ? t->fmt->name : "x") + ".png";
        const bool ok = dump_texture(t.get(), png, &stats);
        NXLOG_I("nvn.trace", "target %s -> %s: %s", tex_desc(t.get(), buf, sizeof buf), ok ? png.c_str() : "(not dumped)", ok ? stats.c_str() : "");
    }
}

// The copies of feedback attachments are keyed by the attachment's address. A texture the game finalizes (every area load makes new render
// targets) leaves its copy behind, a full-size image each (S34 soak: 500 an hour, about 3 GB an hour of GPU memory). A copy that no submission
// has used for kFbIdleSubmits is released; one that is still in use stays (and one released too early is made again on its next use).
constexpr uint64_t kFbIdleSubmits = 600;
void fb_sweep(uint64_t serial) {
    g_fb_use.clear();       // raw pointers into the copies: none is in use between two submissions
    for (auto it = g_fb_clone.begin(); it != g_fb_clone.end();) {
        if (serial > it->second.last + kFbIdleSubmits) it = g_fb_clone.erase(it);
        else ++it;
    }
    g_fb_clone_n.store(static_cast<int64_t>(g_fb_clone.size()), std::memory_order_relaxed);
}

void replay_end_submit(Frame& f) {
    F = &f;
    if (out_any()) {         // every compute result is in game memory, or held and guarded, before the submission's serial signals
        end_pass();
        gstamp("pre-tail");   // gpu_passes: "tail" is then the end-of-submission copy-backs (and the rest after them)
        out_end_submit(F->cb, F->serial);
    }
    end_pass();
    occ_flush_pending();   // [nvn] occ_defer: the reports of the whole submission, written here once
    g_prev_att.clear();   // pass_pre_layout: no references past the submission
    if ((f.serial & 127) == 0 && !g_fb_clone.empty()) fb_sweep(f.serial);
    if ((f.serial & 127) == 0 && !g_vol3d.empty()) {   // 3D copies of 2D textures no submission used for kFbIdleSubmits are released
        g_vol3d_use.clear();
        for (auto it = g_vol3d.begin(); it != g_vol3d.end();) {
            if (f.serial > it->second.last + kFbIdleSubmits) it = g_vol3d.erase(it);
            else ++it;
        }
    }
    if (g_occ_active >= 0) {   // a query cannot outlive its command buffer: the count of an unreported counter is dropped
        vkCmdEndQuery(f.cb, g_occ_pool, static_cast<uint32_t>(g_occ_active));
        g_occ_active = -1;
    }
    for (int k = 0; k < 2; ++k) {
        cw_end_query(g_cw[k], k == 1);
        g_cw[k].active = -1;
    }
    for (; g_dbg_depth > 0; --g_dbg_depth) vkCmdEndDebugUtilsLabelEXT(f.cb);   // labels cannot outlive their command buffer either
    F = nullptr;
}

void replay_shutdown() {
    cmdq::shutdown();
    if (g_xfb_counter) device().destroy_buffer(g_xfb_counter);
    g_xfb_counter = gfx::Buffer();
    g_xfb_began = g_xfb_active = g_xfb_have_counter = false;
    g_xfb_slot_of.clear();
    g_xfb_slot_next = g_xfb_cur = 0;
    std::memset(g_xfb_slot_written, 0, sizeof g_xfb_slot_written);
    for (CounterWindow& w : g_cw) {
        if (w.pool) vkDestroyQueryPool(vkdev(), w.pool, nullptr);
        w = CounterWindow();
    }
    g_dbg_depth = 0;
    g_gw_next = g_gw_count = 0;
    g_gw_lost_serial = 0;
    if (g_occ_pool) vkDestroyQueryPool(vkdev(), g_occ_pool, nullptr);
    g_occ_pool = VK_NULL_HANDLE;
    g_occ_tried = false;
    g_occ_active = g_occ_last = -1;
    g_occ_in_pass = g_occ_want = false;
    g_occ_result_slot = g_occ_any_slot = -1;
    g_occ_parts_drawn = 0;
    g_occ_next = g_occ_reset_hi = 0;
    g_occ_pend.clear();
    g_prims_generated = g_prims_xfb = 0;
    g_mask.shutdown();
    g_mask_ready = false;
    pool_sets_reset();
    for (VkDescriptorPool p : g_keep_pools) vkDestroyDescriptorPool(vkdev(), p, nullptr);   // the device is idle here
    g_keep_pools.clear();
    keep_reset(false);
    g_keep_full = false;
    if (g_support_keep.buffer) device().destroy_buffer(g_support_keep);
    g_support_keep = gfx::Buffer();
    g_reuse_masks.clear();
    g_keep_masks.clear();
    g_set_memo.clear();
    pipeline_shutdown();
    S = GState();
    g_odb_all_dirty = true;
    ++g_resolve_gen;
    P = Pass();
    g_pending.clear();
    out_reset();
    g_set_cache.clear();
    g_fb_use.clear();
    g_fb_clone.clear();
    g_vol3d_use.clear();
    g_vol3d.clear();
    g_fb_clone_n.store(0, std::memory_order_relaxed);
}

// ---- frame capture (nvn_capture.cpp) --------------------------------------------------------------------------------------------
// The GPU state a captured frame starts from. GState is plain data apart from its Refs and the ShaderInfo pointers: the image is the raw
// bytes of a copy whose Refs and stage pointers were cleared, and those go beside it (the capture turns them into object ids).
std::shared_ptr<void> replay_state_snapshot() { return std::make_shared<GState>(S); }

uint32_t replay_state_size() { return static_cast<uint32_t>(sizeof(GState)); }

void replay_state_image(const std::shared_ptr<void>& snap, std::vector<uint8_t>& pod, ReplayStateRefs& refs) {
    GState c = *static_cast<const GState*>(snap.get());
    refs = ReplayStateRefs{};
    refs.tpool = c.tpool.get();
    refs.spool = c.spool.get();
    for (int i = 0; i < 8; ++i) refs.color[i] = c.color_tex[i].get();
    refs.depth = c.depth_tex.get();
    for (int i = 0; i < kStages; ++i) refs.stage[i] = c.stage[i];
    c.tpool = nullptr;
    c.spool = nullptr;
    for (auto& t : c.color_tex) t = nullptr;
    c.depth_tex = nullptr;
    for (auto& st : c.stage) st = nullptr;
    pod.resize(sizeof(GState));
    std::memcpy(pod.data(), static_cast<const void*>(&c), sizeof(GState));
}

bool replay_state_restore(const std::vector<uint8_t>& pod, const ReplayStateRefs& refs) {
    if (pod.size() != sizeof(GState)) return false;
    GState n;   // every Ref null, as in the image
    std::memcpy(static_cast<void*>(&n), pod.data(), sizeof(GState));
    n.tpool = Ref<TexturePool>::share(static_cast<TexturePool*>(refs.tpool));
    n.spool = Ref<SamplerPool>::share(static_cast<SamplerPool*>(refs.spool));
    for (int i = 0; i < 8; ++i) n.color_tex[i] = Ref<Texture>::share(static_cast<Texture*>(refs.color[i]));
    n.depth_tex = Ref<Texture>::share(static_cast<Texture*>(refs.depth));
    for (int i = 0; i < kStages; ++i) n.stage[i] = refs.stage[i];
    S = n;
    g_odb_all_dirty = true;
    ++g_resolve_gen;
    // the raw copy's Refs in n are released by n's destructor: they hold the references taken just above, S took its own
    g_dirty = kAllDirty;
    return true;
}

// Gauge (nxrt/gauge.h): reads the atomic mirror above, never the map. Removed before the globals are destroyed.
namespace {
struct ReplayGauges {
    ReplayGauges() {
        nxrt::gauge_register("nvn.replay.fb_clones", [] { return g_fb_clone_n.load(std::memory_order_relaxed); });
    }
    ~ReplayGauges() { nxrt::gauge_unregister("nvn.replay.fb_clones"); }
};
ReplayGauges g_replay_gauges;
}  // namespace

}  // namespace nxrt::nvn::be::gpu

namespace nxrt::nvn {
bool upscaler_replace_configured() { return !be::gpu::upscaler_override().replace.empty(); }
void set_upscaler_replace_enabled(bool on) {
    if (be::gpu::g_upscaler_on.exchange(on) != on && upscaler_replace_configured())
        NXLOG_I("nvn", "upscaler override %s (the game's own upscaler passes %s)", on ? "on" : "off", on ? "are replaced" : "run again");
}
bool upscaler_replace_enabled() { return be::gpu::g_upscaler_on.load(std::memory_order_relaxed); }
}  // namespace nxrt::nvn
