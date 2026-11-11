// SPDX-License-Identifier: GPL-3.0-or-later
// Display tuning that the NVN back end reads on the render thread (S10): texture filtering overrides. Play mode writes them
// from the settings page; the sampler cache includes them in its key, so a change takes effect on the next draw without a restart.
#pragma once
#include <atomic>

namespace nxrt::gfx {

struct Tuning {
    std::atomic<int> anisotropy{0};        // 0 = what the game's samplers ask for; 2, 4, 8, 16 = at least this much on every filtering sampler
    std::atomic<float> lod_bias{0.0f};     // added to the LOD bias of every sampler (negative = sharper)
    std::atomic<float> render_scale{0.0f};  // the NVN back end's resolution scale from the game's play settings ([display] render_scale); 0 = not set
    // A game's live scale on some floats of one fragment shader's uniform block (title A: the fog strength on its height-fog pass). When the
    // scale is not 1, that block is bound from a copy in the upload ring with the chosen floats multiplied; the game's memory is left alone.
    std::atomic<uint64_t> ufs_hash{0};     // the fragment shader's hash (its 16 hex digits as a number); 0 = off
    std::atomic<int> ufs_block{-1};        // the uniform block (NVN index, the N of fp_cN)
    std::atomic<uint32_t> ufs_mask{0};     // bit i: the float at byte offset 4 * i of the block
    std::atomic<float> ufs_scale{1.0f};
};

Tuning& tuning();                          // process-wide

}  // namespace nxrt::gfx
