// SPDX-License-Identifier: GPL-3.0-or-later
// Mouse pointer in the game's menus (S28).
//
// The pointer state and the arithmetic (window pixel to the UI plane and to the picture) are here and covered by unit tests; they know no
// game. A game's menus take the pointer through hooks of its own, reached through GamePlay (play_game.h: menu_frame, menu_over_value,
// menu_active). The game module supplies the hooks.
#pragma once
#include <cstdint>
#include <string>

#include "play_types.h"

namespace nxrt::play::mm {

// ---- arithmetic -----------------------------------------------------------------------------------------------------------------
constexpr double kUiAspect = 16.0 / 9.0;      // the aspect the front end's UI camera is authored for; on a wider display it stays centred

struct Geometry {
    int win_w = 1280, win_h = 720;            // client size in pixels
    int render_w = 1280, render_h = 720;      // internal size: the picture the present pass draws
    double display_aspect = 0;                // the aspect the 3D view and UI use (0 = render_w / render_h)
    bool keep_aspect = true;                  // the present pass letterboxes the picture instead of stretching it
};

// The picture rectangle in window pixels.
struct Rect { double x = 0, y = 0, w = 0, h = 0; };
Rect picture_rect(const Geometry& g);

// A window pixel to the UI plane the touch screen maps to: nx, ny in -1..1 across the centred 16:9 region (nx right +, ny up +). `inside` is
// false when the pixel is outside the picture or beside the 16:9 region (the cursor still moves there, it just hits nothing).
void window_to_ui(const Geometry& g, float px, float py, float* nx, float* ny, bool* inside);

// A window pixel to the picture: u, v in 0..1 (v down); `inside` false outside the picture. The GUI frames (pause screen, options, map) are projected over the
// whole picture, so their normalised screen coordinates are 2u - 1 and 1 - 2v.
void window_to_picture(const Geometry& g, float px, float py, float* u, float* v, bool* inside);

// out = M * (x, y, z, 1) for a row-major 3x4 transform (the game's CTransform4f).
void transform_point(const float m[12], float x, float y, float z, float out[3]);

// ---- the pointer ------------------------------------------------------------------------------------------------------------------
// What the hooks see in one rendered frame. `hover`: the pointer moved since the last frame; `click`: the left button went down.
struct Snapshot {
    bool active = false;       // menu context, window focused, feature on
    bool hover = false;
    bool click = false;
    bool held = false;         // the left button is down
    bool click_unclaimed = false;   // a click ended this frame and no widget took it
    float nx = 0, ny = 0;      // pointer on the UI plane, -1..1
    bool inside = false;
    bool shown = false;        // the cursor is up (the mouse was the last device used)
    float gx = 0, gy = 0;      // the pointer in the GUI frames' screen space, -1..1, y up
    bool in_picture = false;
};

class Pointer {
public:
    void set_enabled(bool on) { enabled_ = on; }
    bool enabled() const { return enabled_; }
    // Events (main thread).
    void on_motion(float x, float y);
    void on_button(int button, bool down);
    void on_other_input();                          // a key or a pad button: the cursor hides until the mouse moves
    void on_focus_lost();
    bool held() const { return held_; }
    bool mid_held() const { return mid_held_; }
    // One call per rendered frame, after the context is known. `active`: a menu is up and the window has focus.
    Snapshot begin_frame(bool active, const Geometry& g);
    // The hooks tell the pointer what they did with the snapshot.
    void click_used() { click_used_ = true; }
    // The OS cursor: shown while a menu is up, the feature is on and the mouse was the last device used.
    bool cursor_visible(bool menu_up) const { return enabled_ && menu_up && cursor_shown_; }
    bool has_moved() const { return moved_; }        // a motion event came since the last frame (set by on_motion, which runs just before)
    float x() const { return x_; }
    float y() const { return y_; }
private:
    bool enabled_ = true;
    float x_ = 0, y_ = 0;
    bool moved_ = false, pressed_ = false, held_ = false, mid_held_ = false;
    bool cursor_shown_ = true;
    int click_age_ = 0;
    bool click_used_ = false;
    bool have_pos_ = false;
};

Pointer& pointer();                                 // the one pointer of the process

}  // namespace nxrt::play::mm
