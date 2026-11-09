// SPDX-License-Identifier: GPL-3.0-or-later
// The generic game module as one nxrt::game::Info (generic_game.h). Play mode runs on the base engine: SDL gamepads and the keyboard as a
// Pro Controller in the console layout. There is no frame-update hook, so play mode is driven from the game's presents (play.cpp
// present_frame); the mouse is not taken as a stick (no game is known to want it).
#include "generic_game.h"

#include "play_game.h"
#include "play_types.h"

namespace nxrt::games::generic {
namespace {

// Factory keyboard bindings: WASD the left stick, the arrows the right stick, then the face buttons and shoulders where a PC player looks
// for them. The player changes them in the game window's menu (Input).
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
Space = btn_a
E = btn_a
Backspace = btn_b
Left Shift = btn_b
R = btn_x
T = btn_y
Q = btn_l
F = btn_r
Z = btn_zl
G = btn_zr
Left Ctrl = btn_lstick
V = btn_rstick
Escape = btn_plus
C = btn_minus
1 = dpad_up
2 = dpad_down
3 = dpad_left
4 = dpad_right
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
        p.frame_update = 0;          // none known: play.cpp drives play mode from the presents
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

}  // namespace nxrt::games::generic

void nxrt_register_game_generic() { nxrt::game::set_current(&nxrt::games::generic::full_info()); }
