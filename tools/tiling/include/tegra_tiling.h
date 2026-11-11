// tegra_tiling: Tegra X1 (Maxwell) block-linear texture layout. GPL-3.0-or-later.
//
// Layout rules follow Ryujinx.Graphics.Texture (MIT, Copyright Ryujinx Team and Contributors) and the NVN/deko3d
// documentation in SuperMarioWonderRecomp docs/nvn/TILING.md (MIT); see ../README.md and ../ref/LAYOUT_NOTES.md.
//
// Everything is counted in blocks: one texel for plain formats, one BCn/ASTC block for compressed ones
// (bytes_per_block 1, 2, 4, 8, 12 or 16). A GOB is 64 bytes x 8 rows (512 bytes); a "block" of GOBs stacks
// gob_blocks_y GOBs vertically (1..32, power of two) and, for 3D textures, gob_blocks_z in depth.
//
// Linear buffer (the format of tile() input and untile() output), identical to Ryujinx's:
//   levels back to back, level 0 first, no alignment between them; inside a level all layers; inside a layer
//   z slices, then rows of align(blocks_per_row * bytes_per_block, linear_stride_align) bytes.
//   linear_stride_align = 4 reproduces Ryujinx exactly (its rows are padded to 4 bytes); 1 gives tight rows.
// Tiled buffer: layers (aligned as below), inside a layer the levels back to back, each level in GOB order.
#pragma once
#include <cstdint>
#include <vector>

namespace tegra {

struct TextureDesc {
    uint32_t width = 1, height = 1, depth = 1;  // texels of level 0; depth > 1 (or gob_blocks_z > 1) = 3D texture
    uint32_t levels = 1, layers = 1;            // 3D textures: layers must be 1
    uint32_t block_w = 1, block_h = 1;          // texels per block (1x1 plain, 4x4 BCn, 4x4..12x12 ASTC)
    uint32_t bytes_per_block = 4;
    uint32_t gob_blocks_y = 1;                  // block height in GOBs of level 0, power of two 1..32
    uint32_t gob_blocks_z = 1;                  // block depth in GOBs of level 0, power of two 1..32 (3D)
    uint32_t linear_stride_align = 4;           // 1 = tight rows, 4 = Ryujinx
    // GOB-internal byte order. false: Maxwell (platform 1): offset bits 4..8 = y0, x4, y1, y2, x5 (x = byte column).
    // true: platform 2 (T239): bits 4..8 = y0, y1, x4, y2, x5 (16-byte pieces of rows 0-3 of one 32-byte half first),
    // found on title A's GT1G RGBA8 and BC7 textures (the only order of the 120 that decodes them cleanly).
    bool gob_switch2 = false;
};

struct LevelLayout {
    uint32_t width, height, depth;      // texels
    uint32_t blocks_x, blocks_y;        // blocks
    uint32_t gob_blocks_y, gob_blocks_z;  // this level's block height / depth (they shrink on small mips)
    uint64_t linear_offset;             // from the start of the linear buffer (layer 0)
    uint64_t linear_row_stride;         // bytes per row
    uint64_t linear_slice_size;         // bytes of one layer's slice (all z) of this level
    uint64_t tiled_offset;              // from the start of a layer's tiled data
    uint64_t tiled_size;                // bytes of this level in one layer
};

struct Layout {
    std::vector<LevelLayout> levels;
    uint64_t linear_size = 0;           // total linear bytes (all levels and layers)
    uint64_t tiled_layer_stride = 0;    // distance between layers in the tiled buffer
    uint64_t tiled_size = 0;            // total tiled bytes
    bool valid = false;                 // false for bad parameters (zero sizes, non power-of-two block heights, 3D layers)
};

Layout compute_layout(const TextureDesc& d);

// Whole texture, all levels and layers. Buffers must hold at least layout.tiled_size / layout.linear_size bytes.
// untile() leaves the row padding of the linear buffer untouched; tile() zero-fills the tiled bytes no block maps to.
bool untile(const TextureDesc& d, const Layout& l, const uint8_t* tiled, uint8_t* linear);
bool tile(const TextureDesc& d, const Layout& l, const uint8_t* linear, uint8_t* tiled);

// One level of one layer (z slices included) between a tiled and a linear region, both starting at that level.
void untile_level(const TextureDesc& d, const LevelLayout& lv, const uint8_t* tiled, uint8_t* linear);
void tile_level(const TextureDesc& d, const LevelLayout& lv, const uint8_t* linear, uint8_t* tiled);

// Byte offset of the block at (block x, block y, z) inside one level's tiled data. Slow, formula-only reference used by
// the tests; untile()/tile() do not call it.
// `byte` addresses one byte inside the block (blocks of 12 bytes straddle 16-byte pieces, so they are not contiguous).
uint64_t tiled_address(const LevelLayout& lv, uint32_t bytes_per_block, uint32_t bx, uint32_t by, uint32_t z, uint32_t byte = 0);

// Default block height (GOBs) NVN picks for a texture of this many block rows: smallest power of two >= rows / 8,
// at most 16. Packaged (BNTX) textures carry their own value, which is often one step smaller: use the file's.
uint32_t default_gob_blocks_y(uint32_t block_rows);

}  // namespace tegra
