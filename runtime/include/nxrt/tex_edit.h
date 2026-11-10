// SPDX-License-Identifier: GPL-3.0-or-later
// Texture pixel edit (S45): lets the host change the pixels of game textures that are ASTC in the game and become BC7 (or RGBA8) on the PC,
// whole mip chains included. Used by game modules that recolour textures. Unlike tex_override.h (small BC7 UI icons,
// replaced as a whole), the host sees each level of the game's own image as RGBA8 and edits it in place; the runtime re-encodes it.
//
// How it runs (src/gfx/nvn/nvn_tex_edit.cpp):
//   * prepare_texture asks `match` for every 2D ASTC texture (one layer) it prepares; a non-zero answer is the host's id of that texture.
//   * The edited data is made once per (texture content, generation) and kept in memory, so uploading the same texture again costs a copy. It never
//     goes into the BC7 disk cache, which stays the game's own look.
//   * texture_edit_changed (after the host's answers changed, e.g. a new colour): a worker thread makes the new data of every live texture that
//     matches (or was edited before) and asks for each to be uploaded again once its data is ready, so the render thread does not wait for the encode.
// All callbacks may run on any thread (render thread, background texture jobs, the worker) and must be thread safe.
#pragma once
#include <cstddef>
#include <cstdint>

namespace nxrt::gfx {

struct TexEditReq {
    uint32_t width = 0, height = 0;   // level 0, texels
    uint32_t levels = 0;
    uint32_t block_w = 0, block_h = 0;   // ASTC block footprint
    bool srgb = false;
    const uint8_t* tiled = nullptr;   // the texture's storage in the game's memory exactly as the game wrote it (block-linear)
    size_t tiled_size = 0;
};

struct TexEditHooks {
    // Cheap: the host's id of this texture when it wants to edit it now, 0 to leave it alone.
    uint64_t (*match)(const TexEditReq& req, void* user) = nullptr;
    // The answer generation: data made under another generation is not used again.
    uint64_t (*generation)(void* user) = nullptr;
    // Edits one level in place: w x h straight-alpha RGBA8, sRGB-encoded when the texture is sRGB, rows as stored. Called for level 0 first, then
    // 1, 2, ... of the same texture, on one thread.
    void (*edit)(uint64_t id, uint32_t level, uint32_t w, uint32_t h, uint8_t* rgba, void* user) = nullptr;
    void* user = nullptr;
};

void set_texture_edit(const TexEditHooks* hooks);   // nullptr removes the hooks (the edited textures are uploaded again as the game's)
// The answers changed: the matching textures (and those edited before) get new data in the background and are then uploaded again. Any thread.
void texture_edit_changed();

struct TexEditStats {
    uint64_t edits = 0;          // edited data made (any thread)
    uint64_t reuses = 0;         // uploads served from the kept data
    uint64_t rebuilds = 0;       // texture_edit_changed passes finished
    double last_edit_ms = 0;     // the last edit: decode, host edit, encode of the whole chain
    double last_rebuild_ms = 0;  // the last pass of texture_edit_changed: all textures, before they are uploaded again
    uint32_t last_rebuild_textures = 0;
};
TexEditStats texture_edit_stats();

}  // namespace nxrt::gfx
