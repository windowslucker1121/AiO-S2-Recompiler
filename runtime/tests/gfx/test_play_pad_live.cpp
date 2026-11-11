// SPDX-License-Identifier: GPL-3.0-or-later
// Play mode, controller path end to end with an SDL virtual gamepad: hot-plug, button/stick/trigger state, gyro, rumble and face-button
// labels travel SDL -> platform layer -> play mode's frame-boundary hook -> the hid input source the game reads. No hardware needed.
#include <SDL3/SDL.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>

#include "test_framework.h"

#include "nxrt/config.h"
#include "nxrt/hooks.h"
#include "nxrt/input.h"
#include "nxrt/input_source.h"
#include "nxrt/platform.h"
#include "nxrt/play.h"
#include "nxrt/game.h"
#include "play/play_game.h"

namespace nxrt::services {
void vibration_dispatch(uint32_t npad_slot, int hand, const hid::VibrationValue& v);   // src/services/input_source.cpp: what hid calls for a game's vibration command
}

NXTEST_GROUP(play_pad_live)

namespace {

namespace hid = nxrt::hid;
namespace platform = nxrt::platform;
namespace play = nxrt::play;

constexpr uint64_t kBase = 0x5B00000000ull;

// The registered game's frame boundary (GamePlay::frame_update, main-relative): play mode samples the pads in its entry hook there.
// oot 0x3b4ea90; title E's was 0x3c81c8. 0 when the game has no play table (the cases then skip).
uint64_t frame_update() {
    const nxrt::play::GamePlay* gp = nxrt::game::info().play;
    return gp ? gp->frame_update : 0;
}

struct Rumble {
    uint16_t low = 0, high = 0;
    int calls = 0;
};
Rumble g_rumble;

bool rumble_cb(void*, Uint16 low, Uint16 high) {
    g_rumble.low = low;
    g_rumble.high = high;
    g_rumble.calls++;
    return true;
}

struct Pad {
    SDL_JoystickID id = 0;
    SDL_Joystick* joy = nullptr;
};
Pad g_pad;

bool setup() {
    static bool done = false, ok = false;
    if (done) return ok;
    done = true;
    if (!frame_update()) return false;
    const auto dir = std::filesystem::temp_directory_path() / "nxrt_play_pad_live_test";
    std::filesystem::create_directories(dir);
    nxrt::config_set("play", "controls", (dir / "controls.ini").string().c_str());
    nxrt::config_set("play", "game_base", "0x5B00000000");
    if (!platform::init()) return false;
    play::load();
    play::StartOptions o;
    o.overlay = false;
    if (!play::start(o)) return false;

    static const SDL_VirtualJoystickSensorDesc sensors[2] = {{SDL_SENSOR_ACCEL, 100.0f}, {SDL_SENSOR_GYRO, 100.0f}};
    SDL_VirtualJoystickDesc d;
    SDL_INIT_INTERFACE(&d);
    d.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    d.naxes = SDL_GAMEPAD_AXIS_COUNT;
    d.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    d.nhats = 0;
    d.vendor_id = 0x045E;     // an Xbox 360 pad: labels A B X Y
    d.product_id = 0x028E;
    d.name = "nxrt virtual pad";
    d.nsensors = 2;
    d.sensors = sensors;
    d.Rumble = rumble_cb;
    g_pad.id = SDL_AttachVirtualJoystick(&d);
    if (!g_pad.id) return false;
    g_pad.joy = SDL_OpenJoystick(g_pad.id);
    if (!g_pad.joy) return false;
    for (int i = 0; i < 50; ++i) {      // SDL announces the pad on the next pump
        platform::pump_events();
        if (nxrt::input::gamepad_count() > 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ok = nxrt::input::gamepad_count() > 0;
    return ok;
}

// One frame boundary, then what the hid service would read.
hid::InputSnapshot sample() {
    SDL_UpdateJoysticks();
    platform::pump_events();
    ::Ctx c{};
    nx_hook_entry(&c, kBase + frame_update());
    hid::InputSnapshot s;
    auto src = hid::input_source();
    if (src) src->poll(s);
    return s;
}

void set_button(SDL_GamepadButton b, bool down) { SDL_SetJoystickVirtualButton(g_pad.joy, static_cast<int>(b), down); }
void set_axis(SDL_GamepadAxis a, int v) { SDL_SetJoystickVirtualAxis(g_pad.joy, static_cast<int>(a), static_cast<Sint16>(v)); }
void release_all() {
    for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b) set_button(static_cast<SDL_GamepadButton>(b), false);
    for (int a = 0; a < SDL_GAMEPAD_AXIS_COUNT; ++a) set_axis(static_cast<SDL_GamepadAxis>(a), 0);
    // a virtual pad's trigger axes span the whole range: the rest position is the minimum (SDL then reports 0 for the trigger)
    set_axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER, -32768);
    set_axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, -32768);
}

bool listed(uint32_t id) {
    nxrt::input::GamepadInfo info[8];
    const size_t n = nxrt::input::gamepad_list(info, 8);
    for (size_t k = 0; k < n; ++k) {
        if (info[k].id == id) return true;
    }
    return false;
}

// Attaches a throwaway virtual pad with the given USB ids, waits until the platform lists it and reads the kind it reports.
// SDL's virtual descriptor has no controller-type field: SDL derives the type from the vendor and product ids.
// The pad is closed and detached again before returning, so the other tests still see only the main pad.
bool probe_kind(uint16_t vendor, uint16_t product, nxrt::input::PadKind* out) {
    SDL_VirtualJoystickDesc d;
    SDL_INIT_INTERFACE(&d);
    d.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    d.naxes = SDL_GAMEPAD_AXIS_COUNT;
    d.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    d.vendor_id = vendor;
    d.product_id = product;
    d.name = "nxrt virtual probe pad";
    const SDL_JoystickID id = SDL_AttachVirtualJoystick(&d);
    if (!id) return false;
    SDL_Joystick* joy = SDL_OpenJoystick(id);
    bool found = false;
    for (int i = 0; i < 50 && !found; ++i) {      // SDL announces the pad on the next pump
        platform::pump_events();
        found = listed(id);
        if (!found) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (found) *out = nxrt::input::gamepad_kind_of(id);
    if (joy) SDL_CloseJoystick(joy);
    SDL_DetachVirtualJoystick(id);
    for (int i = 0; i < 50 && listed(id); ++i) {  // and the platform drops it on the removal event
        platform::pump_events();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return found && !listed(id);
}

}  // namespace

NXTEST(virtual_pad_is_seen_and_player_one_is_connected) {
    CHECK(setup());
    nxrt::input::GamepadInfo info[4];
    CHECK(nxrt::input::gamepad_list(info, 4) >= 1);
    CHECK(std::strstr(info[0].name, "virtual") != nullptr);
    const hid::InputSnapshot s = sample();
    CHECK(s.npad[0].connected);
    CHECK(s.npad[0].style == hid::NpadStyle::FullKey);
}

NXTEST(face_buttons_follow_the_printed_label_on_an_xbox_layout_pad) {
    CHECK(setup());
    release_all();
    set_button(SDL_GAMEPAD_BUTTON_SOUTH, true);          // the bottom button, printed A on an Xbox pad
    hid::InputSnapshot s = sample();
    CHECK_MSG(s.npad[0].buttons == hid::btn(hid::kBtnA), std::to_string(s.npad[0].buttons).c_str());    // Switch A: "press A" on the title screen
    set_button(SDL_GAMEPAD_BUTTON_SOUTH, false);
    set_button(SDL_GAMEPAD_BUTTON_EAST, true);
    s = sample();
    CHECK(s.npad[0].buttons == hid::btn(hid::kBtnB));
    release_all();
}

NXTEST(every_button_reaches_the_switch_pad) {
    CHECK(setup());
    struct { SDL_GamepadButton sdl; hid::NpadButtonBit hid; } map[] = {
        {SDL_GAMEPAD_BUTTON_WEST, hid::kBtnX}, {SDL_GAMEPAD_BUTTON_NORTH, hid::kBtnY},
        {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, hid::kBtnL}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, hid::kBtnR},
        {SDL_GAMEPAD_BUTTON_START, hid::kBtnPlus}, {SDL_GAMEPAD_BUTTON_BACK, hid::kBtnMinus},
        {SDL_GAMEPAD_BUTTON_LEFT_STICK, hid::kBtnStickL}, {SDL_GAMEPAD_BUTTON_RIGHT_STICK, hid::kBtnStickR},
        {SDL_GAMEPAD_BUTTON_DPAD_UP, hid::kBtnUp}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN, hid::kBtnDown},
        {SDL_GAMEPAD_BUTTON_DPAD_LEFT, hid::kBtnLeft}, {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, hid::kBtnRight},
    };
    for (auto& m : map) {
        release_all();
        set_button(m.sdl, true);
        const hid::InputSnapshot s = sample();
        CHECK_MSG(s.npad[0].buttons == hid::btn(m.hid), SDL_GetGamepadStringForButton(m.sdl));
    }
    release_all();
}

NXTEST(sticks_and_triggers) {
    CHECK(setup());
    release_all();
    set_axis(SDL_GAMEPAD_AXIS_LEFTY, -32768);            // up on the stick is negative in SDL
    set_axis(SDL_GAMEPAD_AXIS_RIGHTX, 32767);
    set_axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 32767);
    set_axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER, -26000);     // a light pull: about 10 percent of the travel, below the threshold
    hid::InputSnapshot s = sample();
    CHECK(s.npad[0].left.y > 32000);                      // hid: up is positive
    CHECK(std::abs(s.npad[0].left.x) < 100);
    CHECK(s.npad[0].right.x > 32000);
    CHECK((s.npad[0].buttons & hid::btn(hid::kBtnZR)) != 0);
    CHECK((s.npad[0].buttons & hid::btn(hid::kBtnZL)) == 0);
    set_axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 32767);
    s = sample();
    CHECK((s.npad[0].buttons & hid::btn(hid::kBtnZL)) != 0);
    release_all();
    s = sample();
    CHECK(s.npad[0].left.y == 0 && s.npad[0].right.x == 0);
}

NXTEST(gyro_reaches_the_six_axis_sample) {
    CHECK(setup());
    release_all();
    const float accel[3] = {0.0f, 0.0f, 9.80665f};               // lying still, face up
    const float gyro[3] = {6.2831853f, 0.0f, 0.0f};              // one turn per second around SDL's x axis
    SDL_SendJoystickVirtualSensorData(g_pad.joy, SDL_SENSOR_ACCEL, SDL_GetTicksNS(), accel, 3);
    SDL_SendJoystickVirtualSensorData(g_pad.joy, SDL_SENSOR_GYRO, SDL_GetTicksNS(), gyro, 3);
    hid::InputSnapshot s = sample();
    CHECK(std::fabs(s.npad[0].six_axis.gyro[0] + 1.0f) < 0.02f);   // -1 revolution per second (smo-recomp's axis mapping)
    CHECK(std::fabs(s.npad[0].six_axis.accel[2] + 1.0f) < 0.02f);  // 1 G pointing down at rest
    CHECK(!s.npad[0].six_axis.at_rest);
    const float still[3] = {0, 0, 0};
    SDL_SendJoystickVirtualSensorData(g_pad.joy, SDL_SENSOR_GYRO, SDL_GetTicksNS(), still, 3);
    s = sample();
    CHECK(s.npad[0].six_axis.at_rest);
}

NXTEST(game_vibration_reaches_the_pad) {
    CHECK(setup());
    release_all();
    g_rumble = Rumble{};
    hid::VibrationValue v;
    v.amp_low = 0.5f;
    v.amp_high = 0.25f;
    nxrt::services::vibration_dispatch(0, 0, v);          // what the hid service does when the game sends a vibration value for player 1
    for (int i = 0; i < 5; ++i) sample();                 // the frame boundary hands it to the pad
    CHECK(g_rumble.calls > 0);
    CHECK(std::abs(static_cast<int>(g_rumble.low) - 32768) < 200);
    CHECK(std::abs(static_cast<int>(g_rumble.high) - 16384) < 200);
    v = hid::VibrationValue{};
    nxrt::services::vibration_dispatch(0, 0, v);          // the game stops the vibration
    for (int i = 0; i < 5; ++i) sample();
    CHECK(g_rumble.low == 0 && g_rumble.high == 0);
}

NXTEST(unplugging_the_pad_clears_the_state) {
    CHECK(setup());
    release_all();
    set_button(SDL_GAMEPAD_BUTTON_SOUTH, true);
    hid::InputSnapshot s = sample();
    CHECK((s.npad[0].buttons & hid::btn(hid::kBtnA)) != 0);
    SDL_CloseJoystick(g_pad.joy);
    g_pad.joy = nullptr;
    SDL_DetachVirtualJoystick(g_pad.id);
    for (int i = 0; i < 5; ++i) s = sample();
    CHECK(nxrt::input::gamepad_count() == 0);
    CHECK((s.npad[0].buttons & hid::btn(hid::kBtnA)) == 0);      // nothing stays latched
    CHECK(s.npad[0].connected);                                  // the keyboard still plays as player one
}

NXTEST(a_dualsense_reports_the_playstation_kind) {
    CHECK(setup());
    nxrt::input::PadKind kind = nxrt::input::PadKind::Other;
    CHECK(probe_kind(0x054C, 0x0CE6, &kind));                    // Sony DualSense: SDL types it PS5
    CHECK(kind == nxrt::input::PadKind::PlayStation);
}

NXTEST(an_xbox_one_pad_reports_the_xbox_kind) {
    CHECK(setup());
    nxrt::input::PadKind kind = nxrt::input::PadKind::Other;
    CHECK(probe_kind(0x045E, 0x02EA, &kind));                    // Xbox One S pad: SDL types it XBOXONE
    CHECK(kind == nxrt::input::PadKind::Xbox);
}
