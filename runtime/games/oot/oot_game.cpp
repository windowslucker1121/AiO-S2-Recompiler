// SPDX-License-Identifier: GPL-3.0-or-later
// Ocarina of Time: the whole game module as one nxrt::game::Info (oot_game.h). Play mode runs on the base engine (SDL gamepads and the
// keyboard as a Pro Controller); game-specific hooks come here as they are found.
#include "oot_game.h"

#include "play_game.h"
#include "play_types.h"

namespace nxrt::games::oot {
namespace {

// Once per frame on the main thread: the game framework's per-frame virtual (vtable 0x4c63f80 +224; its main loop is main+0x9467b0). It runs
// the calc (vtable +296), sets the frame step (60 / fps) and tail-calls the draw (main+0x346c680, which ends in the present). The base class's
// +224 (0x346c8d0) is never called. Entry hook site in recomp/games/oot/main.sites.
constexpr uint64_t kFrameUpdate = 0x3b4ea90;

// Factory keyboard and mouse bindings (play_types.h settings file). Pads keep the console layout. Keys: WASD move, arrows the camera, Enter/E/Space
// A (action), Shift/Backspace/left mouse B (sword, back), Q/right mouse ZL (target), F R (shield), R/T X/Y and G ZR (items), Escape +, C -.
constexpr const char* kSettingsText = R"INI(
[meta]
defaults_rev = 1

[display]
render_scale = auto
fps_limit = auto

[bindings.all]
W = move_forward
S = move_back
A = move_left
D = move_right
Up = look_up
Down = look_down
Left = look_left
Right = look_right
Return = btn_a
Keypad Enter = btn_a
E = btn_a
Space = btn_a
Left Shift = btn_b
Backspace = btn_b
Mouse1 = btn_b
Mouse3 = btn_zl
Q = btn_zl
F = btn_r
R = btn_x
T = btn_y
G = btn_zr
Z = btn_l
Left Ctrl = btn_lstick
V = btn_rstick
Escape = btn_plus
C = btn_minus
1 = dpad_up
2 = dpad_down
3 = dpad_left
4 = dpad_right

[bindings.menu]
WheelUp = dpad_up
WheelDown = dpad_down
)INI";

const play::GameDefaults& game_defaults() {
    static const play::GameDefaults d = [] {
        play::GameDefaults g;
        g.settings_text = kSettingsText;
        g.defaults_rev = 1;
        return g;
    }();
    return d;
}

const play::GamePlay& game_play() {
    static const play::GamePlay t = [] {
        play::GamePlay p;
        p.frame_update = kFrameUpdate;
        return p;
    }();
    return t;
}

}  // namespace

const game::Info& full_info() {
    static const game::Info info = [] {
        game::Info g = core_info();
        g.play = &game_play();
        g.play_defaults = &game_defaults();
        return g;
    }();
    return info;
}

}  // namespace nxrt::games::oot

void nxrt_register_game_oot() { nxrt::game::set_current(&nxrt::games::oot::full_info()); }
