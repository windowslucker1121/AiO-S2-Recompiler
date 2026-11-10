// SPDX-License-Identifier: GPL-3.0-or-later
// [prof] gpu_stats: per-draw pipeline statistics and occlusion counts per draw family (measurement only; nvn_gpu_stats.cpp).
#pragma once
#include <cstdint>

#include <volk.h>

namespace nxrt::nvn::be::gpu::gstats {
bool on();
// At the start of a submission in slot `slot` (render thread, after the slot's previous submission completed): adds that submission's
// results and resets the slot's queries in `cb`.
void slot_begin(VkCommandBuffer cb, uint32_t slot);
// Around one draw inside its render pass. stats_free / occlusion_free: no other query of that type is active (the game's counter
// windows and occlusion queries), so one can be begun.
void draw_begin(VkCommandBuffer cb, const char* label, bool stats_free, bool occlusion_free);
void draw_end(VkCommandBuffer cb);
bool active();
// A draw switched between a mesh-shader and a vertex-shader pipeline (counted per frame in the stats lines).
void note_mesh_switch();
}  // namespace nxrt::nvn::be::gpu::gstats
