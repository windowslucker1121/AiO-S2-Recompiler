// SPDX-License-Identifier: GPL-3.0-or-later
// Display options of play mode (S10): internal resolution, aspect ratio, field of view, dynamic resolution, filtering and post effects.
//
// Two halves. This one (no game, no host state) is the arithmetic every option reduces to and is covered by unit tests, plus the renderer
// tuning (anisotropy, LOD bias) that applies to every game. The other half applies the options to one game's own objects through hooks
// and belongs to the game module (the game module); play mode reaches it through GamePlay
// (play_game.h: install_render, settings_changed, display_live).
#pragma once
#include <cstdint>

#include "play_types.h"

namespace nxrt::play {

// ---- arithmetic -----------------------------------------------------------------------------------------------------------------
constexpr double kHudAspect = 16.0 / 9.0;           // what the GUIF frames and the retail cameras are authored for
constexpr int kMinRenderDim = 320, kMaxRenderDim = 16384;
constexpr double kMinAspect = 1.0, kMaxAspect = 5.0;

// The internal (render) size. A fixed size (render_width and render_height both > 0) wins; otherwise the window's client size times
// render_scale percent. The result is rounded to even numbers and clamped to kMinRenderDim..kMaxRenderDim per side and to at most
// 8192 x 8192 x 2 pixels in area (the clamp keeps the aspect).
void internal_size(const DisplaySettings& d, int window_w, int window_h, int* out_w, int* out_h);

// The aspect the 3D view and the HUD use: the forced ratio when `aspect` > 0 (clamped to kMinAspect..kMaxAspect), else w / h.
double display_aspect(const DisplaySettings& d, int w, int h);

// A GUI perspective camera's aspect: returns true and sets *aspect when it was the authored 16:9 (or already the display aspect).
bool hud_perspective(float* aspect, double display_aspect, float* fov_degrees = nullptr);
// A GUI orthographic camera's extents (left, right, bottom, top as the game stores them: either order of each pair; the retail ball HUD has
// left 8, right -8): widens the width around its centre so |width| / |height| = display aspect, keeping the order. Only cameras whose own
// ratio is 16:9 (or already the display aspect) are touched.
bool hud_ortho(float* left, float* right, float* bottom, float* top, double display_aspect);

// Vertical field of view in radians plus an offset in degrees, kept between 10 and 170 degrees.
float fov_with_offset(float base_radians, double offset_degrees);

// ---- what the game's display hooks use (play_game.h GamePlay::display_live) -------------------------------------------------------
struct DisplayLive {
    int render_w = 1600, render_h = 900;   // internal size, fixed once the game has asked
    double aspect = 0;                     // 0 = render_w / render_h
    double fov_offset = 0;
    bool dynamic_resolution = false;
    bool bloom = true;
    bool chromatic_aberration = true;
};

// The renderer's sampler tuning from the settings ([display] anisotropy, lod_bias): at start and whenever the settings change.
void display_tuning(const DisplaySettings& d);

}  // namespace nxrt::play
