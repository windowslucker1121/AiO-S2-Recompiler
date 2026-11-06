// SPDX-License-Identifier: GPL-3.0-or-later
// Pure logic of the smaller NVN2 features (nvn_small.h). No Vulkan here.
#include "nvn_small.h"

#include <algorithm>
#include <cmath>

#include "nvn_core.h"

namespace nxrt::nvn::be {
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

uint32_t log2_small(uint32_t v) { return v >= 4 ? 2u : (v >= 2 ? 1u : 0u); }

// The sdk's clamp of an exclusive scissor (0x620F80): negative origin trims the size, anything that starts past 0xFFFE or is empty becomes 0.
void clamp_exclusive(const int32_t in[4], int32_t out[4]) {
    for (int axis = 0; axis < 2; ++axis) {
        int64_t o = in[axis], s = in[axis + 2];
        if (o < 0) {
            s += o;
            o = 0;
        }
        if (o > 0xFFFE || s < 1) {
            out[axis] = 0;
            out[axis + 2] = 0;
            continue;
        }
        if (o + s > 0xFFFE) s = 0xFFFF - o;
        out[axis] = static_cast<int32_t>(o);
        out[axis + 2] = static_cast<int32_t>(s);
    }
    // the sdk packs x|w and y|h separately: an empty axis does not empty the other, but the rectangle is empty
    if (out[2] == 0 || out[3] == 0) out[0] = out[1] = out[2] = out[3] = 0;
}

}  // namespace

// ---- push constants -------------------------------------------------------------------------------------------------------------
bool push_write(SmallState& st, uint32_t stage, uint32_t offset, uint32_t size, const void* data) {
    const bool compute = (stage & kPushCompute) != 0;
    stage &= ~kPushCompute;
    if (stage >= kPushStages || !data) return false;
    size = std::min(size, kPushAreaBytes) & ~3u;
    if (size < 4) return false;
    offset = (offset & 0xFFu) & ~3u;
    if (offset + size > kPushAreaBytes) size = kPushAreaBytes - offset;
    std::memcpy(st.push[stage] + offset, data, size);
    if (compute) {
        std::memcpy(st.push_cs + offset, data, size);
        st.push_cs_stage = stage;
    }
    ++st.pushes;
    return true;
}

// ---- shading rate ----------------------------------------------------------------------------------------------------------------
void palette_entry_to_fragment_size(uint32_t entry, uint32_t& w, uint32_t& h) {
    switch (entry) {
        case 6: w = 2; h = 1; return;
        case 7: w = 1; h = 2; return;
        case 8: w = 2; h = 2; return;
        case 9: w = 4; h = 2; return;
        case 10: w = 2; h = 4; return;
        case 11: w = 4; h = 4; return;
        default: w = 1; h = 1; return;   // 0 no invocations, 1..5 supersampling or one per pixel
    }
}

void fit_fragment_size(uint32_t supported, uint32_t& w, uint32_t& h) {
    while (!(w == 1 && h == 1) && !((supported >> (log2_small(w) * 3 + log2_small(h))) & 1)) {
        if (w >= h) w /= 2;
        else h /= 2;
    }
}

void shading_rate_for_draw(const SmallState& st, uint32_t supported, uint32_t& w, uint32_t& h, bool* exact) {
    w = h = 1;
    if (exact) *exact = true;
    if (!st.rate_image || !st.palette_on[0]) return;
    const uint8_t e = st.palette[0][0];
    for (int i = 1; i < 16; ++i) {
        if (st.palette[0][i] != e) {
            if (exact) *exact = false;
            return;
        }
    }
    palette_entry_to_fragment_size(e, w, h);
    fit_fragment_size(supported, w, h);
}

// ---- exclusive scissor -----------------------------------------------------------------------------------------------------------
void exclusive_reduce(const int32_t s[4], const int32_t e[4], int32_t out[4], bool* exact) {
    if (exact) *exact = true;
    std::memcpy(out, s, 4 * sizeof(int32_t));
    if (s[2] <= 0 || s[3] <= 0 || e[2] <= 0 || e[3] <= 0) return;
    const int64_t sx0 = s[0], sy0 = s[1], sx1 = sx0 + s[2], sy1 = sy0 + s[3];
    const int64_t ex0 = e[0], ey0 = e[1], ex1 = ex0 + e[2], ey1 = ey0 + e[3];
    if (ex0 >= sx1 || ex1 <= sx0 || ey0 >= sy1 || ey1 <= sy0) return;   // no overlap: nothing is excluded
    const bool span_x = ex0 <= sx0 && ex1 >= sx1;
    const bool span_y = ey0 <= sy0 && ey1 >= sy1;
    if (span_x && span_y) {
        out[2] = out[3] = 0;
        return;
    }
    if (span_y) {   // E covers S's full height: cut a band off the left or the right
        if (ex0 <= sx0) { out[0] = static_cast<int32_t>(ex1); out[2] = static_cast<int32_t>(sx1 - ex1); return; }
        if (ex1 >= sx1) { out[2] = static_cast<int32_t>(ex0 - sx0); return; }
    } else if (span_x) {   // full width: cut off the top or the bottom
        if (ey0 <= sy0) { out[1] = static_cast<int32_t>(ey1); out[3] = static_cast<int32_t>(sy1 - ey1); return; }
        if (ey1 >= sy1) { out[3] = static_cast<int32_t>(ey0 - sy0); return; }
    }
    if (exact) *exact = false;
}

void small_exclusive_scissor(const SmallState& st, uint32_t i, const int32_t s[4], int32_t out[4], bool* exact) {
    if (exact) *exact = true;
    if (!st.excl_enable || i >= kMaxViewportsSmall) {
        std::memcpy(out, s, 4 * sizeof(int32_t));
        return;
    }
    exclusive_reduce(s, st.excl[i], out, exact);
}

// ---- conservative raster ---------------------------------------------------------------------------------------------------------
bool small_conservative_on(const SmallState& st) { return st.cons_legacy || st.cons_mode != 0; }

float small_conservative_extra(const SmallState& st, float device_max, float device_granularity) {
    const uint32_t q = st.cons_mode != 0 ? st.cons_mode_bloat_q : st.cons_dilate_q;
    float size = static_cast<float>(q) * 0.25f;
    size = std::min(size, std::max(0.0f, device_max));
    if (device_granularity > 0.0f) size = std::floor(size / device_granularity + 1e-4f) * device_granularity;
    return std::max(0.0f, size);
}

uint32_t dilate_to_quarters(float dilate) {
    if (!(dilate > 0.0f)) return 0;
    const float q = std::ceil(dilate * 4.0f);
    return q >= 3.0f ? 3u : static_cast<uint32_t>(q);
}

// ---- fast clear --------------------------------------------------------------------------------------------------------------------
int FastClearTables::add_color(uint32_t kind, const uint32_t v[4], uint32_t format) {
    if (format == 0 || kind > 2) return -1;
    std::lock_guard<std::mutex> lk(mu);
    for (size_t i = 0; i < colors.size(); ++i)
        if (colors[i].kind == kind && colors[i].format == format && std::memcmp(colors[i].v, v, 16) == 0) return static_cast<int>(i);
    if (colors.size() >= kMaxColors) return -1;
    Color c{kind, format, {v[0], v[1], v[2], v[3]}};
    colors.push_back(c);
    return static_cast<int>(colors.size() - 1);
}

int FastClearTables::add_depth(float value) {
    uint32_t bits;
    if (value == 0.0f) bits = 0;
    else if (value == 1.0f) bits = 0x3F800000u;
    else return -1;
    std::lock_guard<std::mutex> lk(mu);
    for (size_t i = 0; i < depth.size(); ++i)
        if (depth[i] == bits) return static_cast<int>(i);
    if (depth.size() >= kMaxDepth) return -1;
    depth.push_back(bits);
    return static_cast<int>(depth.size() - 1);
}

int FastClearTables::add_stencil(uint32_t value, uint32_t format) {
    uint32_t cap;
    if (format == 0 || format == 54) cap = kMaxStencil;
    else if (format == 53) cap = kMaxStencilShort;
    else return -1;
    value &= 0xFFu;
    std::lock_guard<std::mutex> lk(mu);
    for (size_t i = 0; i < stencil.size(); ++i)
        if (stencil[i] == value) return i < cap ? static_cast<int>(i) : -1;
    if (stencil.size() >= cap) return -1;
    stencil.push_back(value);
    return static_cast<int>(stencil.size() - 1);
}

// ---- device message buffers ------------------------------------------------------------------------------------------------------
void msgbuf_write_reset_header(uint8_t* region, uint32_t size) {
    wr<uint32_t>(region, 0, kMsgHeaderBytes);
    wr<uint32_t>(region, 4, size);
    wr<uint32_t>(region, 8, 0);
}

void msgbuf_begin(uint8_t* object, uint8_t* region) {
    if (!region) {
        wr<uint64_t>(object, 0x18, 0);
        wr<uint32_t>(object, 0x20, 0);
        wr<uint32_t>(object, 0x24, 0);
        return;
    }
    wr<uint64_t>(object, 0x18, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(region)));
    wr<uint32_t>(object, 0x20, kMsgHeaderBytes);
    const uint32_t snapshot = rd<uint32_t>(region, 8);
    wr<uint32_t>(object, 0x24, snapshot ? snapshot : rd<uint32_t>(region, 0));
}

void msgbuf_end(uint8_t* object) {
    wr<uint64_t>(object, 0x18, 0);
    wr<uint64_t>(object, 0x20, 0);
}

bool msgbuf_next(uint8_t* object, NVNdeviceMessage* msg, char* text, uint64_t text_size) {
    const uint8_t* cursor = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(rd<uint64_t>(object, 0x18)));
    uint32_t at = rd<uint32_t>(object, 0x20);
    const uint32_t end = rd<uint32_t>(object, 0x24);
    if (!cursor || at >= end) return false;
    const uint8_t* rec = cursor + at;
    const uint32_t len = rd<uint32_t>(rec, 0);
    if (len <= kMsgRecordHeader || len > end - at) return false;   // malformed or running past the data: stop instead of looping
    if (msg) {
        msg->id = rd<int16_t>(rec, 6);
        msg->kind = rd<uint16_t>(rec, 4);
        for (int i = 0; i < 4; ++i) msg->args[i] = rd<uint32_t>(rec, 8 + 4 * i);
    }
    if (text && text_size) {
        const uint32_t room = len - kMsgRecordHeader;
        uint64_t n = 0;
        while (n < room && n + 1 < text_size && rec[kMsgRecordHeader + n]) {
            text[n] = static_cast<char>(rec[kMsgRecordHeader + n]);
            ++n;
        }
        text[n] = 0;
    }
    wr<uint32_t>(object, 0x20, at + len);
    return true;
}

// ---- dirty tile maps --------------------------------------------------------------------------------------------------------------
DirtyTileGeometry dirty_tile_geometry(const uint8_t* p) {
    DirtyTileGeometry g;
    const uint32_t w = rd<uint32_t>(p, 0x1C), h = rd<uint32_t>(p, 0x20), d = rd<uint32_t>(p, 0x24);
    uint32_t tw = rd<uint32_t>(p, 0x5C), th = rd<uint32_t>(p, 0x60);
    if (!tw) tw = 16;
    if (!th) th = 16;
    g.bit_per_tile = (rd<uint32_t>(p, 0x64) & 1) != 0;
    g.tiles_x = (w + tw - 1) / tw;
    g.tiles_y = (h + th - 1) / th;
    g.slices = d > 1 ? d : 1;
    const uint32_t unit = g.bit_per_tile ? tw * 8 : tw;
    g.row_bytes = (w + unit - 1) / unit;
    g.row_stride = rd<uint32_t>(p, 0x38);
    if (!g.row_stride) g.row_stride = g.row_bytes;
    g.slice_stride = rd<uint32_t>(p, 0x3C);
    if (!g.slice_stride) g.slice_stride = g.row_stride * g.tiles_y;
    return g;
}

// ---- the record interpreter --------------------------------------------------------------------------------------------------------
void small_apply(SmallState& st, uint32_t kind, uint32_t a, uint32_t b, uint32_t c, uint64_t addr, const uint8_t* blob, uint32_t blob_bytes) {
    switch (static_cast<SmallKind>(kind)) {
        case SmallKind::PushConstants:
            if (blob && blob_bytes >= c) push_write(st, a, b, c, blob);
            break;
        case SmallKind::PushConstantsDeferred: {
            // the data is read now, at replay: the game may write it between recording and submit. It must lie inside a memory pool.
            const uint32_t n = std::min(c, kPushAreaBytes);
            const PoolHit h = pool_lookup(addr);
            if (n && h.pool && h.offset + n <= h.pool->size) push_write(st, a, b, n, host_ptr(addr));
            break;
        }
        case SmallKind::BindShadingRateImage:
            st.rate_image = a ? addr : 0;
            break;
        case SmallKind::ShadingRatePalette: {
            const uint32_t idx = a & 0xF;
            if (!c) {
                st.palette_on[idx] = false;
                break;
            }
            const uint32_t n = std::min<uint32_t>(std::min(b, 16u), blob ? blob_bytes / 4 : 0);
            for (uint32_t i = 0; i < 16; ++i) st.palette[idx][i] = i < n ? static_cast<uint8_t>(rd<uint32_t>(blob, 4 * i) & 0xF) : kPaletteOnePerPixel;
            st.palette_on[idx] = true;
            break;
        }
        case SmallKind::ShadingRatePaletteMode:
            st.rate_palette_mode = static_cast<int32_t>(a);
            break;
        case SmallKind::ExclusiveScissorEnable:
            st.excl_enable = a != 0;
            break;
        case SmallKind::ExclusiveScissors: {
            if (!blob || a >= kMaxViewportsSmall) break;
            const uint32_t n = std::min(std::min(b, kMaxViewportsSmall - a), blob_bytes / 16);
            for (uint32_t i = 0; i < n; ++i) {
                int32_t r[4];
                std::memcpy(r, blob + 16 * i, 16);
                clamp_exclusive(r, st.excl[a + i]);
            }
            break;
        }
        case SmallKind::ConservativeMode:
            st.cons_mode = static_cast<int32_t>(a);
            st.cons_mode_bloat_q = b & 0xF;
            break;
        case SmallKind::ConservativeDilate:
            st.cons_dilate_q = std::min(b, 3u);
            break;
        default: break;
    }
}

}  // namespace nxrt::nvn::be
