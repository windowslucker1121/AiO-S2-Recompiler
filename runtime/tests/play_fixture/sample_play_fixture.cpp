// SPDX-License-Identifier: GPL-3.0-or-later
// A sample game's play-mode vocabulary and factory controls.ini as a test fixture (see sample_play_fixture.h for why and how).
#include "sample_play_fixture.h"

#include "nxrt/game.h"
#include "play/play_types.h"

namespace nxtest::sample_play {
namespace {

using nxrt::play::Act;
using nxrt::play::ActInfo;
using nxrt::play::Ctx;
using nxrt::play::GameActLabel;
using nxrt::play::GameDefaultAddition;
using nxrt::play::GameDefaultChange;
using nxrt::play::GameDefaults;
using nxrt::play::GameVocabulary;

// One row per Act, in enum order: the settings-file name and the factory [console] output. Set A is four weapons, set B four modes.
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
    {Act::Primary, "attack", "ZR"},
    {Act::Target, "target", "ZL"},
    {Act::Secondary, "item", "R"},
    {Act::Jump, "jump", "B"},
    {Act::AltForm, "vehicle", "Y"},
    {Act::LookHold, "free_look", "RSTICK"},
    {Act::Map, "map", "PLUS"},
    {Act::Pause, "pause", "MINUS"},
    {Act::Confirm, "confirm", "A"},
    {Act::Back, "back", "B"},
    {Act::DpadUp, "dpad_up", "UP"},
    {Act::DpadDown, "dpad_down", "DOWN"},
    {Act::DpadLeft, "dpad_left", "LEFT"},
    {Act::DpadRight, "dpad_right", "RIGHT"},
    {Act::SetA1, "weapon_1", "X+UP"},
    {Act::SetA2, "weapon_2", "X+RIGHT"},
    {Act::SetA3, "weapon_3", "X+DOWN"},
    {Act::SetA4, "weapon_4", "X+LEFT"},
    {Act::SetB1, "mode_1", "L+UP"},
    {Act::SetB2, "mode_2", "L+RIGHT"},
    {Act::SetB3, "mode_3", "L+DOWN"},
    {Act::SetB4, "mode_4", "L+LEFT"},
    {Act::SetANext, "weapon_next", ""},
    {Act::SetAPrev, "weapon_prev", ""},
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

const char* const kCtxNames[] = {"all", "field", "vehicle", "map", "menu", "focus"};
static_assert(sizeof(kCtxNames) / sizeof(kCtxNames[0]) == nxrt::play::kCtxCount, "one name per Ctx");

// The library's keyboard rows (26), in display order.
const GameActLabel kLabels[] = {
    {Act::MoveForward, "Move forward", "Movement"}, {Act::MoveBack, "Move back", "Movement"},
    {Act::MoveLeft, "Move left", "Movement"},       {Act::MoveRight, "Move right", "Movement"},
    {Act::LookUp, "Look up", "Looking"},            {Act::LookDown, "Look down", "Looking"},
    {Act::LookLeft, "Look left", "Looking"},        {Act::LookRight, "Look right", "Looking"},
    {Act::Primary, "Attack", "Action"},             {Act::Secondary, "Item", "Action"},
    {Act::Target, "Target", "Action"},              {Act::Jump, "Jump", "Action"},
    {Act::AltForm, "Vehicle", "Action"},            {Act::LookHold, "Free look", "Action"},
    {Act::SetA1, "Weapon 1", "Weapons"},            {Act::SetA2, "Weapon 2", "Weapons"},
    {Act::SetA3, "Weapon 3", "Weapons"},            {Act::SetA4, "Weapon 4", "Weapons"},
    {Act::SetANext, "Next weapon", "Weapons"},      {Act::SetAPrev, "Previous weapon", "Weapons"},
    {Act::SetB1, "Mode 1", "Modes"},                {Act::SetB2, "Mode 2", "Modes"},
    {Act::SetB3, "Mode 3", "Modes"},                {Act::SetB4, "Mode 4", "Modes"},
    {Act::Map, "Map", "Menus"},                     {Act::Pause, "Pause", "Menus"},
};

const GameVocabulary kVocabulary = [] {
    GameVocabulary v;
    v.acts = kActs;
    v.ctx_names = kCtxNames;
    v.labels = kLabels;
    v.label_count = sizeof(kLabels) / sizeof(kLabels[0]);
    v.key_alt_yaw = "vehicle_yaw";
    v.key_alt_pitch = "vehicle_pitch";
    v.key_wheel_cycles = "wheel_weapons";
    v.key_alt_orbit = "vehicle_orbit";
    v.key_alt_camera = "vehicle_camera";
    v.key_alt_sens = "vehicle_sensitivity";
    v.label_alt_camera = "Vehicle camera";
    v.label_wheel_cycles = "Mouse wheel changes the weapon";
    v.label_alt_camera_on = "Mouse and right stick turn the vehicle camera";
    return v;
}();

// The factory controls.ini at revision 3. Every key is one the reader knows (the tests parse it without warnings); the display keys sit on
// lines of their own, and the four that only apply at the next start say so on the comment line above them.
constexpr const char* kSettingsText = R"INI(# sample game: keyboard, mouse and controller settings (test fixture)
[meta]
defaults_rev = 3

[mouse]
yaw = 0.000767945
pitch = 0.000767945
invert_y = 0
invert_x = 0
menu_cursor = 1
wheel_weapons = 1
vehicle_camera = 1
capture = click

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
vehicle_orbit = 2

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

[bindings.all]
W = move_forward
S = move_back
A = move_left
D = move_right
Up = look_up
Down = look_down
Left = look_left
Right = look_right
Mouse1 = attack
Mouse2 = item
F = item
Mouse3 = target
Space = jump
Left Ctrl = vehicle
Left Shift = free_look
WheelUp = weapon_prev
WheelDown = weapon_next
1 = weapon_1
2 = weapon_2
3 = weapon_3
4 = weapon_4
5 = mode_1
6 = mode_2
7 = mode_3
8 = mode_4
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

// Revision 2 added X on the map (the map's own X action); revision 3 moved the mouse capture from auto to click.
const GameDefaultAddition kAdditions[] = {
    {2, Ctx::Map, "X", Act::BtnX},
};
const GameDefaultChange kChanges[] = {
    {3, "mouse", "capture", "auto", "click"},
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

}  // namespace nxtest::sample_play
