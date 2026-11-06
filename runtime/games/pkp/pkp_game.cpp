// SPDX-License-Identifier: GPL-3.0-or-later
// Pokémon Pokopia: the whole game module as one nxrt::game::Info (pkp_game.h). Play-mode hooks come here as they are found.
#include "pkp_game.h"
#include "pkp_menu.h"
#include "pkp_play.h"
#include "pkp_quality.h"
#include "pkp_taa.h"

#include <cstring>

#include "play_game.h"
#include "play_types.h"

namespace nxrt::games::pkp {

namespace {

// The main loop (0x7100882c60, called from nnMain's startup) calls 0x7101fdcde4 once per iteration on the main thread: the frame boundary
// that pumps window events and samples the pad/keyboard for play mode (needs `entryhook 0x7101fdcde4` in recomp/games/pkp/main.sites).
constexpr uint64_t kFrameUpdate = 0x1fdcde4;

// Factory keyboard and mouse bindings (play_types.h settings file). Button roles from the game's default pad layout
// (pokopia.wiki.fextralife.com/Controls): A interact, B run/back, X bag, Y pick up, R jump, ZR use move, ZL strafe, L set move,
// + main menu, - camera, D-pad follow/emote/swap move. Pads keep the console layout.
constexpr const char* kSettingsText = R"INI(
[meta]
defaults_rev = 5

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
E = btn_a
Return = btn_a
Keypad Enter = btn_a
Mouse1 = btn_zr
Mouse3 = btn_zl
Space = btn_r
Left Shift = btn_b
Backspace = btn_b
Tab = btn_x
F = btn_y
Q = btn_l
Left Ctrl = btn_lstick
V = btn_rstick
Escape = btn_plus
C = btn_minus
1 = dpad_up
2 = dpad_down
3 = dpad_left
4 = dpad_right

[bindings.play]
Mouse2 = mouse_mode

[bindings.menu]
Mouse3 = btn_b
WheelUp = dpad_up
WheelDown = dpad_down
)INI";

// Revision 2: the render scale follows the window (the NVN back end renders the targets larger) unless the player changed it.
// Revision 3: the frame limit follows the display's refresh rate (the game's own 60 Hz wait is off, pkp_play.cpp) unless the player changed it.
// Revision 4: in the menus the right button is B (back) and the wheel moves the selection (pkp_menu.cpp: hover and click).
// Revision 5: the middle mouse button is the Mouse mode key in play (the Joy-Con 2 mouse, pkp_play.cpp play_frame).
constexpr play::GameDefaultAddition kAdditions[] = {
    {5, play::Ctx::Play, "Mouse2", play::Act::MouseMode},
    {4, play::Ctx::Menu, "Mouse3", play::Act::BtnB},
    {4, play::Ctx::Menu, "WheelUp", play::Act::DpadUp},
    {4, play::Ctx::Menu, "WheelDown", play::Act::DpadDown},
};

constexpr play::GameDefaultChange kChanges[] = {
    {2, "display", "render_scale", "100", "auto"},
    {3, "display", "fps_limit", "60", "auto"},
};

const play::GameDefaults& game_defaults() {
    static const play::GameDefaults d = [] {
        play::GameDefaults g;
        g.settings_text = kSettingsText;
        g.defaults_rev = 5;
        g.additions = kAdditions;
        g.addition_count = sizeof kAdditions / sizeof kAdditions[0];
        g.changes = kChanges;
        g.change_count = sizeof kChanges / sizeof kChanges[0];
        return g;
    }();
    return d;
}

// play mode's install / remove / status, plus the menu mouse (pkp_menu.cpp)
void install_all(uint64_t base, play::GameHost* host, const play::Settings& s) {
    play_install(base, host, s);
    menu_install(base);
    quality_install(base, s);
    taa_install(base);
}
void settings_changed_all(const play::Settings& s) {
    play_settings_changed(s);
    quality_settings_changed(s);
}
void remove_all() {
    taa_remove();
    quality_remove();
    menu_remove();
    play_remove();
}
void status_all(char* buf, size_t n) {
    play_status_text(buf, n);
    const size_t k = std::strlen(buf);
    if (k + 1 < n) menu_status_text(buf + k, n - k);
    const size_t k2 = std::strlen(buf);
    if (k2 + 1 < n) quality_status_text(buf + k2, n - k2);
    const size_t k3 = std::strlen(buf);
    if (k3 + 2 < n) {
        buf[k3] = ' ';
        taa_status_text(buf + k3 + 1, n - k3 - 1);
    }
}

const play::GamePlay& game_play() {
    static const play::GamePlay t = [] {
        play::GamePlay p;
        p.frame_update = kFrameUpdate;
        // view distance and the quality preset (pkp_quality.cpp); the render size and dynamic resolution (pkp_play.cpp); the HDR LUT tone map
        p.display_options = play::kDisplayOptQuality | play::kDisplayOptRenderSize | play::kDisplayOptDynRes | play::kDisplayOptHdr;
        p.quality_preset = &quality_preset;
        p.install_render = &play_install_render;
        p.install = &install_all;
        p.remove = &remove_all;
        p.frame = &play_frame;
        p.settings_changed = &settings_changed_all;
        p.display_live = &play_display_live;
        p.display_hook_calls = &play_display_calls;
        p.status_text = &status_all;
        p.menu_frame = &menu_frame;
        p.menu_over_value = &menu_over_value;
        p.menu_active = &menu_active;
        p.menu_in_game = &menu_in_game;
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

}  // namespace nxrt::games::pkp

void nxrt_register_game_pkp() { nxrt::game::set_current(&nxrt::games::pkp::full_info()); }
