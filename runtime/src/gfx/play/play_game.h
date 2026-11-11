// SPDX-License-Identifier: GPL-3.0-or-later
// Play mode's half of the game boundary (docs/GAME_BOUNDARY.md, nxrt/game.h Info::play).
//
// Play mode itself is the game-independent PC front end: the window, the SDL event pump, the frame limiter, the binding engine that turns
// keys, mouse buttons and pads into Switch pad input, the menu pointer state, the F1 overlay and its settings file. What only makes sense for
// one game goes through GamePlay: the hooks on that game's functions (raw mouse aim, camera look, lock-on, display options, menu widgets, the
// tone curve), what the game's state says about the context (play, alternate body, inspect; mpr: gameplay, morph ball, scan) and its parts of the F1 page. A game without a
// GamePlay table still gets a window, pad input, the frame limiter and the overlay; the context is then always the menu.
//
// Order at start (play.cpp start()): install_render (before the frame-boundary hook), the frame-boundary hook at frame_update, install.
// Order at every frame boundary (main thread): settings_changed (when the page applied new settings), events, pads, frame_begin, the engine's
// tick, frame (context and hints), then the menu pointer (menu_frame).
#pragma once
#include <cstddef>
#include <cstdint>

#include "menu_mouse.h"
#include "play_display.h"
#include "play_types.h"

namespace nxrt::play {

// What play mode offers the game module. Main thread (the thread the game's frame update and input hooks run on) unless noted.
class GameHost {
public:
    virtual ~GameHost() = default;
    virtual const Settings& settings() const = 0;                 // the settings in force
    virtual bool action_active(Act a, uint64_t now_ns) const = 0;  // a binding of the action is held
    // The right stick as the game gets it (dead zone, invert and sensitivity applied), -1..1, + = right and up.
    // Activation edges of the action since start (a binding of it became active: a press, a tap, a wheel notch). A game-side toggle counts these.
    virtual uint64_t action_activations(Act a) const = 0;
    virtual void right_stick(float* x, float* y) const = 0;
    // A pad is looking up or down right now (the right stick's vertical axis off centre, or the gyro turning about the pad's pitch axis).
    virtual bool pad_look(bool gyro_enabled) const = 0;
    virtual bool has_input() const = 0;               // play mode's input source is up (pad_look and the game's own look checks apply only then)
    // The game's per-tick player-input function was entered (the game is in play: the context detection and Stats count it). Returns the mouse
    // counts gathered since the last frame boundary on the first call after it (pumping the window's events once more first), zero after that.
    virtual void input_tick(double* dx, double* dy) = 0;
    virtual void aim_applied(double dx, double dy) = 0;           // Stats: the aim used these counts
    virtual double pitch_limit_deg() const = 0;                   // [play] pitch_limit_deg
    virtual bool log_status() const = 0;                          // [play] log_status
};

// The game's answer at one frame boundary.
struct GameFrame {
    float look_scale = 1.f;          // the gyro's pitch rate is multiplied by this before the game reads it
    bool hints_valid = false;        // the cycle fields are meaningful
    int cycle_index = -1;            // the selected set-A member in the bindings' numbering (SetA1..SetA4 = 0..3), -1 = unknown (mpr: the beam)
    unsigned cycle_mask = 0;         // bit i = SetA<i+1> selectable (mpr: the beams owned)
    Ctx play_ctx = Ctx::Play;    // the context while the game is in play (Play, Alt, Inspect)
};

struct GameStatus {
    const char* aim = "";            // why the mouse aim does or does not apply now (settings page)
    double pitch_rad = 0;            // the view pitch the aim holds
};

// The display options only a game's own hooks (or a feature verified for that game) can honour: the F1 menu lists them only when the game says it
// has them. Anisotropy, LOD bias, the colour controls and the output pass's spatial upscalers work for every game and are always listed.
enum : unsigned {
    kDisplayOptFov = 1u << 0,        // [display] fov_offset
    kDisplayOptAspect = 1u << 1,     // [display] aspect
    kDisplayOptBloom = 1u << 2,      // [display] bloom
    kDisplayOptChromatic = 1u << 3,  // [display] chromatic_aberration
    kDisplayOptAll = kDisplayOptFov | kDisplayOptAspect | kDisplayOptBloom | kDisplayOptChromatic,
    kDisplayOptQuality = 1u << 4,    // [display] quality_preset, view_distance (the game's quality hooks; GamePlay::quality_preset)
    kDisplayOptRenderSize = 1u << 5, // [display] render_scale, render_width, render_height (the game resizes its targets, or the NVN back end does for it)
    kDisplayOptDynRes = 1u << 6,     // [display] dynamic_resolution (the game's own dynamic resolution, through its hooks)
    kDisplayOptHdr = 1u << 7,        // [hdr], [upscale] scene_scale and the temporal upscalers: the HDR scene pipeline matched the game's tone-map shader
};

struct GamePlay {
    unsigned display_options = 0;    // kDisplayOpt* bits: which of the options above the game's display hooks apply (0: none)
    uint64_t frame_update = 0;       // entry hook site (offset in main): once per rendered frame, on the main thread; 0 = play mode is never pumped
    // A game without an input hook (no GameHost::input_tick calls): > 0 makes the mouse turn the camera through the right stick. Every frame
    // then counts as play (the mouse is captured after a click into the picture, [mouse] capture) and the mouse's turn rate ([mouse] yaw_rad / pitch_rad per
    // count) is this many radians per second at full stick.
    double mouse_stick_rad_s = 0;
    // Before the frame-boundary hook is added: hooks that do not need the host (display options, the tone curve). window_w/h: the client size.
    void (*install_render)(uint64_t base, const DisplaySettings& d, int window_w, int window_h) = nullptr;
    // After it: input, camera, lock-on, menu widgets.
    void (*install)(uint64_t base, GameHost* host, const Settings& s) = nullptr;
    void (*remove)() = nullptr;
    void (*settings_changed)(const Settings& s) = nullptr;       // new settings are in force
    void (*frame_begin)() = nullptr;                             // after the pads are polled
    void (*frame)(bool in_game, double step_seconds, GameFrame* out) = nullptr;
    DisplayLive (*display_live)() = nullptr;                     // what the display hooks use (the menu pointer's geometry)
    uint32_t (*display_hook_calls)() = nullptr;
    // The menu pointer on the game's own menus (menu_mouse.h, the game side): once per frame, the button the click stands for.
    Act (*menu_frame)(bool menu_up, const mm::Geometry& g) = nullptr;
    bool (*menu_over_value)() = nullptr;                          // the pointer is over a slider's track: the wheel steps its value
    bool (*menu_active)() = nullptr;                              // the menu hooks saw an active pointer this frame
    // Once per frame before the context is decided: true while one of the game's menus is up although the game is in play (it keeps calling
    // its input function under an in-game menu). Play mode then treats the frame as a menu: menu context, the pointer, no mouse capture or look.
    bool (*menu_in_game)(bool in_game) = nullptr;
    void (*status)(GameStatus* out) = nullptr;
    // The game's part of the [play] log_status line, appended after the base fields.
    void (*status_text)(char* buf, size_t n) = nullptr;
    // F1 page, Mouse tab: where = 0 after the sensitivity sliders, 1 after the invert boxes, 2 at the end. Returns true when edit changed.
    bool (*mouse_tab)(int where, Settings& edit) = nullptr;
    // F1 page, Tweaks tab: the game's optional looks ([tweaks], play_tweaks.h). Returns true when edit changed; nullptr = no Tweaks tab.
    bool (*tweaks_tab)(Settings& edit) = nullptr;
    // F1 page, Graphics tab (kDisplayOptQuality): writes a quality preset's values ("switch2", "high", "ultra") into d. False: unknown name.
    bool (*quality_preset)(const char* name, DisplaySettings& d) = nullptr;
};

const GamePlay* game_play();         // the registered game's table, or nullptr

}  // namespace nxrt::play
