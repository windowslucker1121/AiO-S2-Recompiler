// SPDX-License-Identifier: GPL-3.0-or-later
// Texture storage layout (family texture). Rules, from the builder:
//   * dimensions per target: 1D has height 1; arrays and cube arrays take their layer count from the depth field (cube = 6
//     faces, cube array = depth layer-faces); 3D has depth slices and one layer; 2D, rectangle and multisample have depth 1.
//   * levels = min(builder levels, floor(log2(largest dimension)) + 1), at least 1; multisample textures have one level.
//   * block-linear: tegra::compute_layout with GOB block height from the packaged layout (byte 0 as a log2, as smo-recomp
//     nvn_gl.c texb_layout / tex_layout does, only when it is 0..4) or else the console's rule for the level-0 block rows
//     (console_block_height: 1.5 times the rows, thresholds 16/32/64/128; the layout of the game's own TXTR data).
//     3D textures use block height 1 GOB and block depth 16 GOBs (the rule that matches the game's 3D textures, see
//     SURVEY section 3).
//     platform 2 (NVN2): the packaged layout is three 4-bit log2 fields in its first halfword: bits 0-3 block height, 4-7 block depth,
//     8-11 block width (in GOBs). The NVN2 driver's SetPackagedTextureLayout (sdk 0x5EEA00) keeps those 12 bits, and texture
//     initialization (0x5EEF48) uses them in place of its own block shape (0x5C1408: width 0, height from the rows, depth from the
//     slices, 0x633A80) for every target. Here: height and depth for 2D and non-sparse 3D textures (width is always 0 on Tegra; a
//     non-zero one is logged).
//   * storage = layer stride * layers * samples. The layer stride is the untrimmed size of a layer: tegra trims the last
//     level of a 3D texture, but the game's buffers are the untrimmed size (47 of 47 3D textures checked, SURVEY section 3).
//   * alignment = 512 * level-0 block height in GOBs, at least 0x200 (Wonder's TILING.md; 0x200 is the minimum).
//   * pitch-linear (NVN_TEXTURE_FLAGS_LINEAR or LINEAR_RENDER_TARGET, as raw_storage_class): one level, pitch = builder stride, or the row size rounded up to 32 when the
//     stride is 0; storage = pitch * rows * depth * layers * samples; alignment 0x1000.
// The rules are cross-checked in tests/gfx/test_nvn_texture.cpp against an independent GOB formula, tegra::compute_layout,
// and the survey's GPU buffer sizes. smo-recomp runtime/gfx/nvn_gl.c and tex_decode.h (GPL-3.0) were read for the packaged
// layout byte; Wonder docs/nvn/TILING.md (MIT) and runtime/src/nvn/nvn_texture_layout.cpp for the level and alignment rules.
// No code was copied.
#include "nvn_texture_layout.h"
#include "nxrt/game.h"
#include "nxrt/log.h"

#include <algorithm>
#include <mutex>
#include <set>

#include <nvn/nvn.h>

namespace nxrt::nvn::be {
namespace {

uint64_t div_up(uint64_t a, uint64_t b) { return (a + b - 1) / b; }
uint64_t align_up(uint64_t a, uint64_t b) { return div_up(a, b) * b; }

// NVN2 packaged layout: log2 block height, depth and width in GOBs (see the header comment).
struct PackedShape {
    uint32_t h = 0, d = 0, w = 0;
};
PackedShape nvn2_packed_shape(const TextureBuilderInfo& b) {
    PackedShape s;
    s.h = b.packaged_layout[0] & 0xfu;
    s.d = (b.packaged_layout[0] >> 4) & 0xfu;
    s.w = b.packaged_layout[1] & 0xfu;
    static std::mutex mu;
    static std::set<uint32_t> seen;   // each distinct shape once, with the texture that first had it
    const uint32_t key = s.h | (s.d << 4) | (s.w << 8) | (static_cast<uint32_t>(b.target) << 12);
    std::lock_guard<std::mutex> lk(mu);
    if (seen.insert(key).second)
        NXLOG_I("nvn.tex", "packaged texture layout (first of this kind): block height 2^%u, depth 2^%u, width 2^%u GOBs; target %d, %dx%dx%d format %d",
                s.h, s.d, s.w, b.target, b.width, b.height, b.depth, b.format);
    return s;
}


}  // namespace

// Block height in GOBs of a 2D/array texture's level 0 when the game gives no packaged layout. The game copies TXTR data that was tiled
// for the console, so the rule has to be the console's (tegra_swizzle block_height_mip0, the rule retrotool tiles with): 1.5 times the
// block rows picks 1, 2, 4, 8 or 16 GOBs at 16, 32, 64 and 128. The smaller "smallest height that covers the rows" rule of
// tegra::default_gob_blocks_y gives the same storage size for every game texture but scrambles the texels of 263 of them (a
// 36x36 BC7 button icon has 9 block rows: 1 GOB here, 2 there, and the icon comes out as a broken half circle).
uint32_t console_block_height(uint32_t block_rows) {
    const uint32_t hh = block_rows + block_rows / 2;
    return hh >= 128 ? 16 : hh >= 64 ? 8 : hh >= 32 ? 4 : hh >= 16 ? 2 : 1;
}


bool sparse_tile_shape(int nvn_target, const FormatInfo* fmt, int out[3]) {
    if (!fmt || fmt->aspect != kAspectColor || fmt->bytes == 0) return false;
    if (nvn_target == NVN_TEXTURE_TARGET_BUFFER || nvn_target == NVN_TEXTURE_TARGET_2D_MULTISAMPLE || nvn_target == NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY)
        return false;
    int w = 0, h = 0, d = 1;
    if (nvn_target == NVN_TEXTURE_TARGET_3D) {
        switch (fmt->bytes) {
            case 1: w = 64; h = 32; d = 32; break;
            case 2: w = 32; h = 32; d = 32; break;
            case 4: w = 32; h = 32; d = 16; break;
            case 8: w = 32; h = 16; d = 16; break;
            case 16: w = 16; h = 16; d = 16; break;
            default: return false;
        }
    } else {
        switch (fmt->bytes) {
            case 1: w = 256; h = 256; break;
            case 2: w = 256; h = 128; break;
            case 4: w = 128; h = 128; break;
            case 8: w = 128; h = 64; break;
            case 16: w = 64; h = 64; break;
            default: return false;
        }
    }
    out[0] = w * fmt->block_w;
    out[1] = h * fmt->block_h;
    out[2] = d;
    return true;
}

TexLayout compute_texture_layout(const TextureBuilderInfo& b) {
    TexLayout out;
    out.fmt = format_info(b.format);
    if (!out.fmt) return out;
    if (b.target == NVN_TEXTURE_TARGET_BUFFER) {
        // A buffer texture (platform 2 games make small ones at start-up): one row of width texels, tightly packed, one level. The back end shows
        // it as a 1D image, which covers reads through a texture view; a real texel buffer view is not made.
        if (b.width <= 0) return out;
        out.levels = 1;
        out.layers = 1;
        out.block_linear = false;
        out.pitch = static_cast<uint64_t>(b.width) * out.fmt->bytes;
        out.storage_size = out.pitch;
        out.alignment = 0x20;
        out.valid = true;
        return out;
    }
    if (b.width <= 0 || b.height <= 0) return out;

    // ---- shape from the target -------------------------------------------------------------------------------------------
    uint32_t w = static_cast<uint32_t>(b.width), h = static_cast<uint32_t>(b.height), d = 1, layers = 1;
    bool is3d = false, multisample = false;
    const uint32_t depth_field = b.depth > 0 ? static_cast<uint32_t>(b.depth) : 0;
    switch (b.target) {
        case NVN_TEXTURE_TARGET_1D:
            h = 1;
            break;
        case NVN_TEXTURE_TARGET_2D:
        case NVN_TEXTURE_TARGET_RECTANGLE:
            break;
        case NVN_TEXTURE_TARGET_2D_MULTISAMPLE:
            multisample = true;
            break;
        case NVN_TEXTURE_TARGET_1D_ARRAY:
            if (!depth_field) return out;
            h = 1;
            layers = depth_field;
            break;
        case NVN_TEXTURE_TARGET_2D_ARRAY:
            if (!depth_field) return out;
            layers = depth_field;
            break;
        case NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY:
            if (!depth_field) return out;
            multisample = true;
            layers = depth_field;
            break;
        case NVN_TEXTURE_TARGET_CUBEMAP:
            layers = 6;
            break;
        case NVN_TEXTURE_TARGET_CUBEMAP_ARRAY:
            if (!depth_field || depth_field % 6) return out;   // depth = layer-faces (6 per cube), as GL: title A sets 6 * cubes
            layers = depth_field;
            break;
        case NVN_TEXTURE_TARGET_3D:
            if (!depth_field) return out;
            is3d = true;
            d = depth_field;
            break;
        default:   // BUFFER and unknown targets have no texture layout
            return out;
    }

    // ---- levels and samples ----------------------------------------------------------------------------------------------
    const uint32_t maxdim = is3d ? std::max({w, h, d}) : std::max(w, h);
    uint32_t top = 1;                                  // floor(log2(maxdim)) + 1
    while ((maxdim >> top) != 0) ++top;
    const uint32_t levels = multisample ? 1 : std::min<uint32_t>(static_cast<uint32_t>(std::max(1, b.levels)), top);
    const uint64_t samples = static_cast<uint64_t>(std::max(1, b.samples));

    const uint32_t bw = out.fmt->block_w, bh = out.fmt->block_h, bpb = out.fmt->bytes;
    const uint64_t blocks_x = div_up(w, bw), blocks_y = div_up(h, bh);
    out.levels = levels;
    out.layers = layers;

    // ---- pitch-linear ----------------------------------------------------------------------------------------------------
    // LINEAR_RENDER_TARGET is pitch-linear too (the game renders its menu text into such R8 textures on the CPU: "New Game" came out in
    // 64-texel bands when untiled as block-linear)
    out.block_linear = (b.flags & (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET)) == 0;
    if (!out.block_linear) {
        if (b.stride < 0 || (b.stride > 0 && static_cast<uint64_t>(b.stride) < blocks_x * bpb)) return out;
        out.levels = 1;   // pitch-linear textures have a single level
        out.pitch = b.stride > 0 ? static_cast<uint64_t>(b.stride) : align_up(blocks_x * bpb, 32);
        out.storage_size = out.pitch * blocks_y * d * layers * samples;
        out.alignment = 0x1000;
        out.valid = true;
        return out;
    }

    // ---- block-linear ----------------------------------------------------------------------------------------------------
    tegra::TextureDesc& ds = out.desc;
    ds.width = w;
    ds.height = h;
    ds.depth = d;
    ds.levels = levels;
    ds.layers = layers;
    ds.block_w = bw;
    ds.block_h = bh;
    ds.bytes_per_block = bpb;
    ds.linear_stride_align = 1;   // tight rows in the scratch buffers of the texel access
    ds.gob_switch2 = nxrt::game::info().platform == nxrt::game::Platform::Switch2;   // platform 2 GOB byte order (tegra_tiling.h)
    int tile[3] = {0, 0, 0};
    if (is3d && (b.flags & NVN_TEXTURE_FLAGS_SPARSE) && sparse_tile_shape(b.target, out.fmt, tile) && tile[2] <= 16 && tile[1] / static_cast<int>(bh) >= 8) {
        // A sparse volume is cut into 64 KiB tiles; a tile is whole GOB blocks, so the block shape is the tile's: rows of GOBs of its height, as deep
        // as the tile (the lighting probe volumes of the game: 2 GOBs high, 16 deep for 8 and 16 byte blocks)
        ds.gob_blocks_y = static_cast<uint32_t>(tile[1] / static_cast<int>(bh)) / 8;
        ds.gob_blocks_z = static_cast<uint32_t>(tile[2]);
    } else if (is3d) {
        ds.gob_blocks_y = 1;
        ds.gob_blocks_z = 16;
        if (b.has_packaged_layout && ds.gob_switch2) {
            const PackedShape s = nvn2_packed_shape(b);
            if (s.h <= 5 && s.d <= 5) {
                ds.gob_blocks_y = 1u << s.h;
                ds.gob_blocks_z = 1u << s.d;
            }
        }
    } else {
        uint32_t gy = console_block_height(static_cast<uint32_t>(blocks_y));
        if (b.has_packaged_layout && ds.gob_switch2) {
            const PackedShape s = nvn2_packed_shape(b);   // the depth field of a 2D texture is 0 (one slice)
            if (s.h <= 5) gy = 1u << s.h;
        } else if (b.has_packaged_layout && b.packaged_layout[0] <= 4) {
            gy = 1u << b.packaged_layout[0];
        }
        ds.gob_blocks_y = gy;
        ds.gob_blocks_z = 1;
    }
    out.layout = tegra::compute_layout(ds);
    if (!out.layout.valid || out.layout.levels.empty()) return out;

    out.storage_size = out.layout.tiled_layer_stride * layers * samples;
    out.alignment = std::max<uint64_t>(0x200, 512ull * out.layout.levels[0].gob_blocks_y);
    if (b.flags & NVN_TEXTURE_FLAGS_SPARSE) {   // starts on a 64 KiB tile
        // Volumes are whole tiles. A 2D sparse texture's size is its tiled mip chain as it is: title C places sparse 512x512
        // textures (10 levels) in pools of exactly that size rounded to 4 KiB (0x156000 for RGBA8), which a 64 KiB round-up overruns.
        if (is3d) out.storage_size = align_up(out.storage_size, 0x10000);
        // The layers of a 2D sparse array start on 64 KiB tiles (a tile is mapped as one 64 KiB page); the last layer is not padded, which
        // keeps a single-layer texture at its mip chain's size. title C's terrain virtual texture caches (8192x8192, 68 layers,
        // 12 levels: 0x1556000 bytes per layer) map their tiles at 0x1560000 per layer.
        if (!is3d && layers > 1 && samples == 1) {
            const uint64_t layer = out.layout.tiled_layer_stride;
            out.layout.tiled_layer_stride = align_up(layer, 0x10000);
            out.storage_size = out.layout.tiled_layer_stride * (layers - 1) + layer;
            out.layout.tiled_size = out.storage_size;
        }
        out.alignment = std::max<uint64_t>(out.alignment, 0x10000);
    }
    out.valid = true;
    return out;
}

}  // namespace nxrt::nvn::be
