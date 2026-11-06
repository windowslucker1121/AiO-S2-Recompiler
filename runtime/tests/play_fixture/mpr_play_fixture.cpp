// SPDX-License-Identifier: GPL-3.0-or-later
// Metroid Prime Remastered's play-mode vocabulary and factory controls.ini as a test fixture (see mpr_play_fixture.h for why and how).
#include "mpr_play_fixture.h"

#include "nxrt/game.h"
#include "play/play_types.h"

namespace nxtest::mpr_play {
namespace {

using nxrt::play::Act;
using nxrt::play::ActInfo;
using nxrt::play::Ctx;
using nxrt::play::GameActLabel;
using nxrt::play::GameDefaultAddition;
using nxrt::play::GameDefaultChange;
using nxrt::play::GameDefaults;
using nxrt::play::GameVocabulary;

// One row per Act, in enum order: the settings-file name and the factory [switch] output. The beams are power, wave, ice and plasma (set A),
// the visors combat, scan, thermal and x-ray (set B).
const ActInfo kActs[] = {
    {Act::None, "none", ""},
    {Act::MoveForward, "move_forward", "LS_UP"},
    {Act::MoveBack, "move_back", "LS_DOWN"},
    {Act::MoveLeft, "move_left", "LS_LEFT"},
    {Act::MoveRight, "move_right", "LS_RIGHT"},
    {Act::LookUp, "look_up", "RS_UP"},
    {Act::LookDown, "look_down", "RS_DOWN"},
    {Act::LookLeft, "look_left", "RS_LEFT"},
    {Act::LookRight, "look_right", "RS_RIGHT"},
    {Act::Primary, "fire", "ZR"},
    {Act::Target, "lock_on", "ZL"},
    {Act::Secondary, "missile", "R"},
    {Act::Jump, "jump", "B"},
    {Act::AltForm, "morph", "Y"},
    {Act::LookHold, "free_look", "RSTICK"},
    {Act::Map, "map", "PLUS"},
    {Act::Pause, "pause", "MINUS"},
    {Act::Confirm, "confirm", "A"},
    {Act::Back, "back", "B"},
    {Act::DpadUp, "dpad_up", "UP"},
    {Act::DpadDown, "dpad_down", "DOWN"},
    {Act::DpadLeft, "dpad_left", "LEFT"},
    {Act::DpadRight, "dpad_right", "RIGHT"},
    {Act::SetA1, "beam_1", "X+UP"},
    {Act::SetA2, "beam_2", "X+RIGHT"},
    {Act::SetA3, "beam_3", "X+DOWN"},
    {Act::SetA4, "beam_4", "X+LEFT"},
    {Act::SetB1, "visor_1", "L+UP"},
    {Act::SetB2, "visor_2", "L+RIGHT"},
    {Act::SetB3, "visor_3", "L+DOWN"},
    {Act::SetB4, "visor_4", "L+LEFT"},
    {Act::SetANext, "beam_next", ""},
    {Act::SetAPrev, "beam_prev", ""},
    {Act::BtnA, "btn_a", "A"},
    {Act::BtnB, "btn_b", "B"},
    {Act::BtnX, "btn_x", "X"},
    {Act::BtnY, "btn_y", "Y"},
    {Act::BtnL, "btn_l", "L"},
    {Act::BtnR, "btn_r", "R"},
    {Act::BtnZL, "btn_zl", "ZL"},
    {Act::BtnZR, "btn_zr", "ZR"},
    {Act::BtnPlus, "btn_plus", "PLUS"},
    {Act::BtnMinus, "btn_minus", "MINUS"},
    {Act::BtnLStick, "btn_lstick", "LSTICK"},
    {Act::BtnRStick, "btn_rstick", "RSTICK"},
    {Act::MouseMode, "mouse_mode", ""},
};
static_assert(sizeof(kActs) / sizeof(kActs[0]) == nxrt::play::kActCount, "one row per Act");

const char* const kCtxNames[] = {"all", "gameplay", "morph", "map", "menu", "scan"};
static_assert(sizeof(kCtxNames) / sizeof(kCtxNames[0]) == nxrt::play::kCtxCount, "one name per Ctx");

// The launcher's keyboard rows (26), in display order.
const GameActLabel kLabels[] = {
    {Act::MoveForward, "Move forward", "Movement"}, {Act::MoveBack, "Move back", "Movement"},
    {Act::MoveLeft, "Move left", "Movement"},       {Act::MoveRight, "Move right", "Movement"},
    {Act::LookUp, "Look up", "Looking"},            {Act::LookDown, "Look down", "Looking"},
    {Act::LookLeft, "Look left", "Looking"},        {Act::LookRight, "Look right", "Looking"},
    {Act::Primary, "Fire", "Combat"},               {Act::Secondary, "Missile", "Combat"},
    {Act::Target, "Lock on", "Combat"},             {Act::Jump, "Jump", "Combat"},
    {Act::AltForm, "Morph ball", "Combat"},         {Act::LookHold, "Free look", "Combat"},
    {Act::SetA1, "Power beam", "Beams"},            {Act::SetA2, "Wave beam", "Beams"},
    {Act::SetA3, "Ice beam", "Beams"},              {Act::SetA4, "Plasma beam", "Beams"},
    {Act::SetANext, "Next beam", "Beams"},          {Act::SetAPrev, "Previous beam", "Beams"},
    {Act::SetB1, "Combat visor", "Visors"},         {Act::SetB2, "Scan visor", "Visors"},
    {Act::SetB3, "Thermal visor", "Visors"},        {Act::SetB4, "X-ray visor", "Visors"},
    {Act::Map, "Map", "Menus"},                     {Act::Pause, "Pause", "Menus"},
};

const GameVocabulary kVocabulary = [] {
    GameVocabulary v;
    v.acts = kActs;
    v.ctx_names = kCtxNames;
    v.labels = kLabels;
    v.label_count = sizeof(kLabels) / sizeof(kLabels[0]);
    v.key_alt_yaw = "morph_yaw";
    v.key_alt_pitch = "morph_pitch";
    v.key_wheel_cycles = "wheel_beams";
    v.key_alt_orbit = "ball_orbit";
    v.key_alt_camera = "ball_camera";
    v.key_alt_sens = "ball_sensitivity";
    v.label_alt_camera = "Morph ball camera";
    v.label_wheel_cycles = "Mouse wheel changes the beam";
    v.label_alt_camera_on = "Mouse and right stick turn the morph ball camera";
    return v;
}();

// The factory controls.ini at revision 3. Every key is one the reader knows (the tests parse it without warnings); the display keys sit on
// lines of their own, and the four that only apply at the next start say so on the comment line above them.
constexpr const char* kSettingsText = R"INI(# Metroid Prime Remastered: keyboard, mouse and controller settings (test fixture)
[meta]
defaults_rev = 3

[mouse]
yaw = 0.000767945
pitch = 0.000767945
invert_y = 0
invert_x = 0
menu_cursor = 1
wheel_beams = 1
ball_camera = 1

[aim]
lock_mode = free
free_lock_margin = 0.15

[camera]
gun_drives_view = 1

[pad]
label_abxy = 1
deadzone = 0.1
right_deadzone = 0.1
invert_y = 0
look_sens = 1
trigger_threshold = 0.35
gyro = 1
gyro_scale = 1
rumble = 1
rumble_scale = 1
ball_orbit = 2

[timing]
tap_ms = 200
hold_ms = 250
double_ms = 300
pulse_ms = 80

[display]
window_mode = windowed
vsync = 0
fps_limit = 60
width = 1280
height = 720
keep_aspect = 1
# internal resolution in percent (25..400): applies at the next start
render_scale = 100
# a fixed internal size, 0 = follow the window: applies at the next start
render_width = 0
render_height = 0
# the forced aspect ratio, 0 = the internal resolution's: applies at the next start
aspect = 0
fov_offset = 0
# the game's own dynamic resolution: applies at the next start
dynamic_resolution = 0
anisotropy = 0
lod_bias = 0
bloom = 1
chromatic_aberration = 1
fast_gpu_memory = 1
fewer_gpu_waits = 1

[prompts]
style = auto

[tweaks]
glow_color = off
suit_glow = auto
ball_glow = auto
gun_glow = auto

[bindings.all]
W = move_forward
S = move_back
A = move_left
D = move_right
Up = look_up
Down = look_down
Left = look_left
Right = look_right
Mouse1 = fire
Mouse2 = missile
F = missile
Mouse3 = lock_on
Space = jump
Left Ctrl = morph
Left Shift = free_look
WheelUp = beam_prev
WheelDown = beam_next
1 = beam_1
2 = beam_2
3 = beam_3
4 = beam_4
5 = visor_1
6 = visor_2
7 = visor_3
8 = visor_4
Tab = map
Escape = pause

[bindings.map]
X = btn_x
Tab = map
Escape = back

[bindings.menu]
Return = confirm
Keypad Enter = confirm
Mouse1 = confirm
Escape = back
Backspace = back
Up = dpad_up
Down = dpad_down
Left = dpad_left
Right = dpad_right
Q = btn_l
E = btn_r
)INI";

// Revision 2 added X on the map (the map's own X action); revision 3 moved the lock-on from the game's classic one to the free one.
const GameDefaultAddition kAdditions[] = {
    {2, Ctx::Map, "X", Act::BtnX},
};
const GameDefaultChange kChanges[] = {
    {3, "aim", "lock_mode", "classic", "free"},
};

const GameDefaults kDefaults = [] {
    GameDefaults d;
    d.settings_text = kSettingsText;
    d.defaults_rev = 3;
    d.additions = kAdditions;
    d.addition_count = sizeof(kAdditions) / sizeof(kAdditions[0]);
    d.vocabulary = &kVocabulary;
    d.changes = kChanges;
    d.change_count = sizeof(kChanges) / sizeof(kChanges[0]);
    return d;
}();

const nxrt::game::Info* g_prev = nullptr;
nxrt::game::Info g_info;
bool g_installed = false;

}  // namespace

const GameDefaults& defaults() { return kDefaults; }

void install() {
    if (g_installed) return;
    g_prev = nxrt::game::current();
    g_info = nxrt::game::info();
    g_info.play_defaults = &kDefaults;
    nxrt::game::set_current(&g_info);
    g_installed = true;
}

void remove() {
    if (!g_installed) return;
    nxrt::game::set_current(g_prev);
    g_installed = false;
}

}  // namespace nxtest::mpr_play
