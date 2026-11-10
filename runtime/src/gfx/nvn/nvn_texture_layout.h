// SPDX-License-Identifier: GPL-3.0-or-later
// Storage layout of an NVN texture, computed from its builder (family texture, S4d). Sizes come from the Maxwell
// block-linear rules in tools/tiling (tegra::compute_layout); the rules were checked against the GPU buffer sizes of the
// game's textures.
#pragma once
#include <cstdint>

#include "nvn_decoded.h"
#include "nvn_format.h"
#include "tegra_tiling.h"

namespace nxrt::nvn::be {

struct TexLayout {
    tegra::TextureDesc desc;     // what tegra::untile_level / tile_level take (block-linear textures)
    tegra::Layout layout;        // per-level offsets and sizes of one layer (block-linear textures)
    uint32_t levels = 0, layers = 0;
    bool block_linear = true;    // false: pitch-linear (NVN_TEXTURE_FLAGS_LINEAR)
    uint64_t pitch = 0;          // pitch-linear only: bytes per row
    uint64_t storage_size = 0;   // bytes of the texture's storage: all levels, layers and samples
    uint64_t alignment = 0;      // storage alignment the layout implies
    const FormatInfo* fmt = nullptr;
    bool valid = false;          // false: unknown format, bad size, a target without a layout (buffer), or a bad layout
};

// The shape of a 64 KiB sparse tile of a texture in texels (width, height, depth): the standard sparse block shapes (64 KiB of
// blocks: 2D 256x256 / 256x128 / 128x128 / 128x64 / 64x64 blocks for 1 / 2 / 4 / 8 / 16 byte blocks; 3D 64x32x32 / 32x32x32 /
// 32x32x16 / 32x16x16 / 16x16x16), times the block size of the format. False: no sparse tiles for this target or format.
bool sparse_tile_shape(int nvn_target, const FormatInfo* fmt, int out[3]);

// The layout of the texture the builder describes. Never fails on a zero-filled builder: it returns valid = false.
TexLayout compute_texture_layout(const TextureBuilderInfo& b);

// Level-0 block height in GOBs of a block-linear 2D texture with that many block rows, when the game gives no packaged layout.
uint32_t console_block_height(uint32_t block_rows);

}  // namespace nxrt::nvn::be
