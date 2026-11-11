// SPDX-License-Identifier: GPL-3.0-or-later
// Play mode pad mapping (src/gfx/play/pad_map.cpp): SDL gamepad state to the console pad. Pure functions, no SDL calls.
#include "test_framework.h"

#include <cmath>
#include <cstdint>
#include <string>

#include "play/pad_map.h"
#include "nxrt/input_source.h"
#include "nxrt/platform.h"

NXTEST_GROUP(play_pad)

namespace {

namespace hid = nxrt::hid;
namespace platform = nxrt::platform;
namespace play = nxrt::play;

// SDL_GamepadButton bit numbers.
enum : uint32_t {
    kSouth = 0, kEast, kWest, kNorth, kBack, kGuide, kStart, kLeftStick, kRightStick,
    kLeftShoulder, kRightShoulder, kDpadUp, kDpadDown, kDpadLeft, kDpadRight, kMisc1,
};

// Labels as the Xbox and PlayStation pads report them (position SOUTH, EAST, WEST, NORTH), and as a console-layout pad does.
const uint8_t kXboxLabels[4] = {0, 1, 2, 3};     // A B X Y; PlayStation cross circle square triangle are the same
const uint8_t kConsoleLabels[4] = {1, 0, 3, 2}; // B A Y X

// Maps one raw state with the given settings.
hid::NpadInput run(const platform::GamepadRaw& raw, const play::PadSettings& cfg) {
    hid::NpadInput out;
    play::map_pad(raw, cfg, &out);
    return out;
}

// A pad with only the given SDL button bits down, default labels and axes.
platform::GamepadRaw pad_buttons(uint32_t buttons) {
    platform::GamepadRaw raw;
    raw.buttons = buttons;
    return raw;
}

// Console buttons for one face button press (position SOUTH..NORTH) on a pad with the given printed labels.
uint64_t face(uint32_t pos, const uint8_t labels[4], bool label_mode) {
    platform::GamepadRaw raw;
    raw.buttons = 1u << pos;
    for (int i = 0; i < 4; ++i) raw.face_label[i] = labels[i];
    play::PadSettings cfg;
    cfg.label_abxy = label_mode;
    return run(raw, cfg).buttons;
}

// Left / right stick of one raw axis pair with default settings (dead zone 0.10).
hid::StickValue left_stick(int16_t x, int16_t y) {
    platform::GamepadRaw raw;
    raw.axes[0] = x;
    raw.axes[1] = y;
    return run(raw, play::PadSettings{}).left;
}
hid::StickValue right_stick(int16_t x, int16_t y) {
    platform::GamepadRaw raw;
    raw.axes[2] = x;
    raw.axes[3] = y;
    return run(raw, play::PadSettings{}).right;
}

bool approx(double a, double b, double eps = 1e-5) { return std::fabs(a - b) <= eps; }

// A pad with the given SDL button bits down and the Xbox face labels.
platform::GamepadRaw xbox_buttons(uint32_t buttons) {
    platform::GamepadRaw raw = pad_buttons(buttons);
    for (int i = 0; i < 4; ++i) raw.face_label[i] = kXboxLabels[i];
    return raw;
}

// Default settings with one physical source remapped to `bits`.
play::PadSettings remapped(play::PadSrc src, uint64_t bits) {
    play::PadSettings cfg;
    cfg.remap[static_cast<int>(src)] = bits;
    return cfg;
}

struct ButtonCase {
    uint32_t sdl;
    uint64_t expect;
};

}  // namespace

NXTEST(every_sdl_button_maps) {
    // Default settings: label mode, labels {0,1,2,3}, so SOUTH is Switch A, EAST B, WEST X, NORTH Y.
    const ButtonCase cases[] = {
        {kSouth, hid::btn(hid::kBtnA)},        {kEast, hid::btn(hid::kBtnB)},        {kWest, hid::btn(hid::kBtnX)},
        {kNorth, hid::btn(hid::kBtnY)},        {kBack, hid::btn(hid::kBtnMinus)},    {kGuide, 0},
        {kStart, hid::btn(hid::kBtnPlus)},     {kLeftStick, hid::btn(hid::kBtnStickL)},
        {kRightStick, hid::btn(hid::kBtnStickR)}, {kLeftShoulder, hid::btn(hid::kBtnL)},
        {kRightShoulder, hid::btn(hid::kBtnR)}, {kDpadUp, hid::btn(hid::kBtnUp)},
        {kDpadDown, hid::btn(hid::kBtnDown)},  {kDpadLeft, hid::btn(hid::kBtnLeft)},
        {kDpadRight, hid::btn(hid::kBtnRight)}, {kMisc1, 0},
    };
    for (const ButtonCase& c : cases) {
        const uint64_t got = run(pad_buttons(1u << c.sdl), play::PadSettings{}).buttons;
        CHECK_MSG(got == c.expect, "sdl button " + std::to_string(c.sdl) + " gave " + std::to_string(got));
    }
}

NXTEST(xbox_and_playstation_labels) {
    // Label mode: the printed label names the console button. Position mode: SOUTH B, EAST A, WEST Y, NORTH X.
    const uint64_t by_label[4] = {hid::btn(hid::kBtnA), hid::btn(hid::kBtnB), hid::btn(hid::kBtnX), hid::btn(hid::kBtnY)};
    const uint64_t by_pos[4] = {hid::btn(hid::kBtnB), hid::btn(hid::kBtnA), hid::btn(hid::kBtnY), hid::btn(hid::kBtnX)};
    for (uint32_t p = 0; p < 4; ++p) {
        CHECK_MSG(face(p, kXboxLabels, true) == by_label[p], "label mode, position " + std::to_string(p));
        CHECK_MSG(face(p, kXboxLabels, false) == by_pos[p], "position mode, position " + std::to_string(p));
    }
    // Xbox bottom button in the two modes, explicitly.
    CHECK_EQ(face(kSouth, kXboxLabels, true), hid::btn(hid::kBtnA));
    CHECK_EQ(face(kSouth, kXboxLabels, false), hid::btn(hid::kBtnB));
}

NXTEST(console_labels_same_in_both_modes) {
    // A console-layout pad prints B, A, Y, X on SOUTH, EAST, WEST, NORTH. Both modes must give the same console buttons.
    const uint64_t expect[4] = {hid::btn(hid::kBtnB), hid::btn(hid::kBtnA), hid::btn(hid::kBtnY), hid::btn(hid::kBtnX)};
    for (uint32_t p = 0; p < 4; ++p) {
        CHECK_MSG(face(p, kConsoleLabels, true) == expect[p], "label mode, position " + std::to_string(p));
        CHECK_MSG(face(p, kConsoleLabels, false) == expect[p], "position mode, position " + std::to_string(p));
    }
}

NXTEST(stick_center_is_zero) {
    const hid::StickValue l = left_stick(0, 0);
    const hid::StickValue r = right_stick(0, 0);
    CHECK_EQ(l.x, 0);
    CHECK_EQ(l.y, 0);
    CHECK_EQ(r.x, 0);
    CHECK_EQ(r.y, 0);
}

NXTEST(stick_full_deflection_each_direction) {
    // 32767 is 0.99997 after normalising, so the positive extreme comes out at 32766; -32768 is exactly -1.
    const hid::StickValue right = left_stick(32767, 0);
    CHECK_MSG(right.x >= 32765 && right.x <= 32767, "right x " + std::to_string(right.x));
    CHECK_EQ(right.y, 0);
    CHECK_EQ(left_stick(-32768, 0).x, -32767);
    // SDL y is down = +: pushing up (negative) gives hid y up = +.
    const hid::StickValue up = left_stick(0, -32768);
    CHECK_EQ(up.y, 32767);
    CHECK_EQ(up.x, 0);
    const hid::StickValue down = left_stick(0, 32767);
    CHECK_MSG(down.y >= -32767 && down.y <= -32765, "down y " + std::to_string(down.y));
    CHECK_EQ(down.x, 0);
}

NXTEST(stick_dead_zone_edge) {
    // Default dead zone 0.10: 3000 / 32768 = 0.0916 is inside, 3300 / 32768 = 0.1007 is just outside.
    const hid::StickValue inside = left_stick(3000, 0);
    CHECK_EQ(inside.x, 0);
    CHECK_EQ(inside.y, 0);
    const hid::StickValue outside = left_stick(3300, 0);
    CHECK_MSG(outside.x > 0 && outside.x < 1000, "just outside gave " + std::to_string(outside.x));
    CHECK_EQ(outside.y, 0);
}

NXTEST(stick_diagonal_within_unit_circle) {
    // Corner of the square: the radial length is clamped to full deflection, so the result stays inside the circle.
    const hid::StickValue d = left_stick(32767, 32767);
    const double len = std::sqrt(static_cast<double>(d.x) * d.x + static_cast<double>(d.y) * d.y);
    CHECK_MSG(len <= 32767 * 1.0001, "diagonal length " + std::to_string(len));
    CHECK_MSG(len >= 32767 * 0.999, "diagonal length " + std::to_string(len));
    CHECK(d.x > 0);
    CHECK(d.y < 0);   // SDL y down = + : pushing down-right gives hid y negative
}

NXTEST(sticks_are_independent) {
    platform::GamepadRaw raw;
    raw.axes[0] = -32768;   // left stick full left
    raw.axes[1] = 0;
    raw.axes[2] = 0;        // right stick straight up
    raw.axes[3] = -32768;
    const hid::NpadInput out = run(raw, play::PadSettings{});
    CHECK_EQ(out.left.x, -32767);
    CHECK_EQ(out.left.y, 0);
    CHECK_EQ(out.right.x, 0);
    CHECK_EQ(out.right.y, 32767);
}

NXTEST(triggers_threshold) {
    play::PadSettings cfg;   // threshold 0.35: 11000 / 32767 = 0.336 is below, 20000 / 32767 = 0.61 is above
    platform::GamepadRaw raw;
    raw.axes[4] = 11000;
    raw.axes[5] = 11000;
    hid::NpadInput out = run(raw, cfg);
    CHECK_EQ(out.buttons & hid::btn(hid::kBtnZL), 0u);
    CHECK_EQ(out.buttons & hid::btn(hid::kBtnZR), 0u);

    raw.axes[4] = 20000;
    out = run(raw, cfg);
    CHECK(out.buttons & hid::btn(hid::kBtnZL));
    CHECK_EQ(out.buttons & hid::btn(hid::kBtnZR), 0u);

    raw.axes[4] = 0;
    raw.axes[5] = 32767;
    out = run(raw, cfg);
    CHECK_EQ(out.buttons & hid::btn(hid::kBtnZL), 0u);
    CHECK(out.buttons & hid::btn(hid::kBtnZR));
}

NXTEST(map_pad_fills_buttons_and_keeps_other_fields) {
    hid::NpadInput out;
    out.connected = true;
    out.style = hid::NpadStyle::JoyDual;
    out.buttons = 0xFFFF;   // stale value: map_pad replaces it
    out.six_axis.accel[0] = 0.5f;
    out.color_body = 0x12345678;
    out.color_buttons = 0x9ABCDEF0;

    play::map_pad(pad_buttons(0), play::PadSettings{}, &out);
    CHECK_EQ(out.buttons, 0ull);
    CHECK(out.connected);
    CHECK(out.style == hid::NpadStyle::JoyDual);
    CHECK(approx(out.six_axis.accel[0], 0.5f));
    CHECK_EQ(out.color_body, 0x12345678u);
    CHECK_EQ(out.color_buttons, 0x9ABCDEF0u);
}

NXTEST(sensors_rest_pose) {
    const float accel[3] = {0.0f, 0.0f, 9.80665f};   // face up, at rest
    const float gyro[3] = {0.0f, 0.0f, 0.0f};
    hid::SixAxisSample s;
    play::map_sensors(accel, gyro, play::PadSettings{}, &s);
    CHECK(approx(s.accel[0], 0.0));
    CHECK(approx(s.accel[1], 0.0));
    CHECK(approx(s.accel[2], -1.0));
    CHECK(approx(s.gyro[0], 0.0));
    CHECK(approx(s.gyro[1], 0.0));
    CHECK(approx(s.gyro[2], 0.0));
    CHECK(s.at_rest);
}

NXTEST(sensors_gyro_and_axis_flip) {
    // SDL x is flipped: 2 pi rad/s on SDL x is -1 rev/s on hid x. Accel x is flipped too.
    const float accel[3] = {9.80665f, 0.0f, 0.0f};
    const float gyro[3] = {6.2831853f, 0.0f, 0.0f};
    hid::SixAxisSample s;
    play::map_sensors(accel, gyro, play::PadSettings{}, &s);
    CHECK(approx(s.gyro[0], -1.0, 1e-4));
    CHECK(approx(s.gyro[1], 0.0));
    CHECK(approx(s.accel[0], -1.0, 1e-4));
    CHECK(!s.at_rest);
}

NXTEST(sensors_gyro_scale_and_rest_threshold) {
    const float accel[3] = {0.0f, 0.0f, 9.80665f};
    const float gyro[3] = {6.2831853f, 0.0f, 0.0f};
    play::PadSettings cfg;
    cfg.gyro_scale = 0.5;
    hid::SixAxisSample s;
    play::map_sensors(accel, gyro, cfg, &s);
    CHECK(approx(s.gyro[0], -0.5, 1e-4));
    CHECK(!s.at_rest);

    // 0.1 rad/s is 0.0159 rev/s: moving. With gyro_scale 0 the same input is at rest.
    const float slow[3] = {0.1f, 0.0f, 0.0f};
    const float none[3] = {0.0f, 0.0f, 0.0f};
    cfg.gyro_scale = 1.0;
    play::map_sensors(accel, slow, cfg, &s);
    CHECK(!s.at_rest);
    cfg.gyro_scale = 0.0;
    play::map_sensors(accel, gyro, cfg, &s);
    CHECK(s.at_rest);
    CHECK(approx(s.gyro[0], 0.0));
    play::map_sensors(accel, none, cfg, &s);
    CHECK(s.at_rest);
}

NXTEST(sensors_gyro_off_gives_rest_sample) {
    const float accel[3] = {3.0f, -2.0f, 5.0f};
    const float gyro[3] = {6.2831853f, 6.2831853f, 6.2831853f};
    play::PadSettings cfg;
    cfg.gyro = false;
    hid::SixAxisSample s;
    play::map_sensors(accel, gyro, cfg, &s);
    CHECK(approx(s.accel[0], 0.0));
    CHECK(approx(s.accel[1], 0.0));
    CHECK(approx(s.accel[2], -1.0));
    CHECK(approx(s.gyro[0], 0.0));
    CHECK(approx(s.gyro[1], 0.0));
    CHECK(approx(s.gyro[2], 0.0));
    CHECK(s.at_rest);
}

NXTEST(rumble_scale_and_clamp) {
    uint16_t lo = 0, hi = 0;
    play::map_rumble(1.0f, 0.25f, play::PadSettings{}, &lo, &hi);
    CHECK_EQ(lo, 65535);
    CHECK_EQ(hi, 16384);   // 0.25 * 65535 = 16383.75

    play::PadSettings cfg;
    cfg.rumble_scale = 0.25;
    play::map_rumble(1.0f, 1.0f, cfg, &lo, &hi);
    CHECK_EQ(lo, 16384);
    CHECK_EQ(hi, 16384);

    // Amplitudes clamp to 0..1 first, then the scale clamps the product again.
    cfg.rumble_scale = 4.0;
    play::map_rumble(0.5f, 2.0f, cfg, &lo, &hi);
    CHECK_EQ(lo, 65535);
    CHECK_EQ(hi, 65535);
    play::map_rumble(-1.0f, 0.0f, play::PadSettings{}, &lo, &hi);
    CHECK_EQ(lo, 0);
    CHECK_EQ(hi, 0);
}

NXTEST(rumble_off_gives_zero) {
    uint16_t lo = 1234, hi = 1234;
    play::PadSettings cfg;
    cfg.rumble = false;
    play::map_rumble(1.0f, 1.0f, cfg, &lo, &hi);
    CHECK_EQ(lo, 0);
    CHECK_EQ(hi, 0);
}

// ---- pad button remapping ----------------------------------------------------------------------------------------------------------
NXTEST(default_remap_adds_built_in_buttons_together) {
    // Four presses at once, label mode and the default labels: each adds its built-in buttons.
    platform::GamepadRaw raw = xbox_buttons((1u << kSouth) | (1u << kEast) | (1u << kLeftShoulder) | (1u << kStart));
    raw.axes[4] = 32767;
    const uint64_t expect = hid::btn(hid::kBtnA) | hid::btn(hid::kBtnB) | hid::btn(hid::kBtnL) | hid::btn(hid::kBtnPlus) | hid::btn(hid::kBtnZL);
    CHECK_EQ(run(raw, play::PadSettings{}).buttons, expect);
}

NXTEST(remapped_face_button_presses_the_remap) {
    // South remapped to X: the bottom button gives X only, and East keeps its built-in B.
    const play::PadSettings cfg = remapped(play::PadSrc::South, hid::btn(hid::kBtnX));
    CHECK_EQ(run(xbox_buttons(1u << kSouth), cfg).buttons, hid::btn(hid::kBtnX));
    CHECK_EQ(run(xbox_buttons(1u << kEast), cfg).buttons, hid::btn(hid::kBtnB));
}

NXTEST(remapped_source_drops_its_built_in_buttons) {
    // Start remapped to A: no Plus. LeftShoulder remapped to ZR: no L.
    CHECK_EQ(run(pad_buttons(1u << kStart), remapped(play::PadSrc::Start, hid::btn(hid::kBtnA))).buttons, hid::btn(hid::kBtnA));
    CHECK_EQ(run(pad_buttons(1u << kLeftShoulder), remapped(play::PadSrc::LeftShoulder, hid::btn(hid::kBtnZR))).buttons,
             hid::btn(hid::kBtnZR));
}

NXTEST(remapped_left_trigger_to_a) {
    // LeftTrigger remapped to A: the trigger gives A and no ZL. The right trigger keeps ZR.
    const play::PadSettings cfg = remapped(play::PadSrc::LeftTrigger, hid::btn(hid::kBtnA));
    platform::GamepadRaw raw = pad_buttons(0);
    raw.axes[4] = 32767;
    CHECK_EQ(run(raw, cfg).buttons, hid::btn(hid::kBtnA));
    raw.axes[5] = 32767;
    CHECK_EQ(run(raw, cfg).buttons, hid::btn(hid::kBtnA) | hid::btn(hid::kBtnZR));
}

NXTEST(remap_to_zero_does_nothing) {
    CHECK_EQ(run(xbox_buttons(1u << kSouth), remapped(play::PadSrc::South, 0)).buttons, 0ull);
    platform::GamepadRaw raw = pad_buttons(0);
    raw.axes[4] = 32767;
    CHECK_EQ(run(raw, remapped(play::PadSrc::LeftTrigger, 0)).buttons, 0ull);
}

NXTEST(remap_to_two_buttons_gives_both) {
    // East remapped to ZL + L: one press gives both bits, and not B.
    const uint64_t zl_l = hid::btn(hid::kBtnZL) | hid::btn(hid::kBtnL);
    CHECK_EQ(run(xbox_buttons(1u << kEast), remapped(play::PadSrc::East, zl_l)).buttons, zl_l);
}

NXTEST(pad_default_buttons_label_and_position) {
    using S = play::PadSrc;
    // label mode: the printed letters; position mode: South B, East A, West Y, North X
    CHECK_EQ(play::pad_default_buttons(S::South, true), hid::btn(hid::kBtnA));
    CHECK_EQ(play::pad_default_buttons(S::East, true), hid::btn(hid::kBtnB));
    CHECK_EQ(play::pad_default_buttons(S::West, true), hid::btn(hid::kBtnX));
    CHECK_EQ(play::pad_default_buttons(S::North, true), hid::btn(hid::kBtnY));
    CHECK_EQ(play::pad_default_buttons(S::South, false), hid::btn(hid::kBtnB));
    CHECK_EQ(play::pad_default_buttons(S::East, false), hid::btn(hid::kBtnA));
    CHECK_EQ(play::pad_default_buttons(S::West, false), hid::btn(hid::kBtnY));
    CHECK_EQ(play::pad_default_buttons(S::North, false), hid::btn(hid::kBtnX));
    // the other sources do not depend on the label question
    CHECK_EQ(play::pad_default_buttons(S::LeftShoulder, true), hid::btn(hid::kBtnL));
    CHECK_EQ(play::pad_default_buttons(S::RightShoulder, false), hid::btn(hid::kBtnR));
    CHECK_EQ(play::pad_default_buttons(S::LeftTrigger, true), hid::btn(hid::kBtnZL));
    CHECK_EQ(play::pad_default_buttons(S::RightTrigger, false), hid::btn(hid::kBtnZR));
    CHECK_EQ(play::pad_default_buttons(S::Start, true), hid::btn(hid::kBtnPlus));
    CHECK_EQ(play::pad_default_buttons(S::Back, true), hid::btn(hid::kBtnMinus));
    CHECK_EQ(play::pad_default_buttons(S::LeftStick, true), hid::btn(hid::kBtnStickL));
    CHECK_EQ(play::pad_default_buttons(S::RightStick, false), hid::btn(hid::kBtnStickR));
    CHECK_EQ(play::pad_default_buttons(S::DpadUp, true), hid::btn(hid::kBtnUp));
    CHECK_EQ(play::pad_default_buttons(S::DpadDown, true), hid::btn(hid::kBtnDown));
    CHECK_EQ(play::pad_default_buttons(S::DpadLeft, false), hid::btn(hid::kBtnLeft));
    CHECK_EQ(play::pad_default_buttons(S::DpadRight, false), hid::btn(hid::kBtnRight));
    // and map_pad with the built-in mapping gives exactly these bits
    CHECK_EQ(face(kSouth, kXboxLabels, false), play::pad_default_buttons(S::South, false));
    CHECK_EQ(face(kNorth, kXboxLabels, true), play::pad_default_buttons(S::North, true));
}

NXTEST(left_and_right_dead_zones_differ) {
    // 0.2 of full deflection (6554 of 32768) on both sticks: inside the right dead zone (0.3), outside the left one (0.1).
    platform::GamepadRaw raw;
    raw.axes[0] = 6554;   // left stick X
    raw.axes[2] = 6554;   // right stick X
    play::PadSettings cfg;
    cfg.stick_deadzone = 0.1;
    cfg.right_deadzone = 0.3;
    const hid::NpadInput out = run(raw, cfg);
    CHECK(out.left.x > 0);
    CHECK_EQ(out.right.x, 0);

    // swapped: the left stick now sits inside its dead zone, the right one outside
    cfg.stick_deadzone = 0.3;
    cfg.right_deadzone = 0.1;
    const hid::NpadInput sw = run(raw, cfg);
    CHECK_EQ(sw.left.x, 0);
    CHECK(sw.right.x > 0);
}

NXTEST(look_stick_identity_at_defaults) {
    const hid::StickValue out = play::adjust_look_stick(hid::StickValue{12000, -8000}, play::PadSettings{});
    CHECK_EQ(out.x, 12000);
    CHECK_EQ(out.y, -8000);
}

NXTEST(look_stick_invert_flips_only_y) {
    play::PadSettings cfg;
    cfg.invert_y = true;
    const hid::StickValue out = play::adjust_look_stick(hid::StickValue{12000, -8000}, cfg);
    CHECK_EQ(out.x, 12000);
    CHECK_EQ(out.y, 8000);
}

NXTEST(look_stick_sens_half_halves) {
    play::PadSettings cfg;
    cfg.look_sens = 0.5;
    const hid::StickValue out = play::adjust_look_stick(hid::StickValue{12000, -8000}, cfg);
    CHECK_EQ(out.x, 6000);
    CHECK_EQ(out.y, -4000);
}

NXTEST(look_stick_sens_two_clamps_to_unit_circle) {
    // (20000, 20000) times 2 is far outside the circle: it is pulled back onto it, keeping the 45 degree direction.
    // Each axis is rounded after the pull-back, so the length can be about 0.3 counts over 32767 (here 23170 on both axes).
    play::PadSettings cfg;
    cfg.look_sens = 2.0;
    const hid::StickValue out = play::adjust_look_stick(hid::StickValue{20000, 20000}, cfg);
    const double len = std::sqrt(static_cast<double>(out.x) * out.x + static_cast<double>(out.y) * out.y);
    CHECK_MSG(len <= 32767 * 1.0001, "clamped length " + std::to_string(len));
    CHECK_MSG(len >= 32767 * 0.999, "clamped length " + std::to_string(len));
    CHECK(out.x > 0 && out.y > 0);
    CHECK_MSG(out.x - out.y <= 1 && out.y - out.x <= 1, "direction " + std::to_string(out.x) + " " + std::to_string(out.y));
}

NXTEST(look_stick_sens_clamped_to_range) {
    // below 0.25 behaves as 0.25: (12000, -8000) * 0.25 = (3000, -2000)
    play::PadSettings low;
    low.look_sens = 0.1;
    play::PadSettings quarter;
    quarter.look_sens = 0.25;
    const hid::StickValue a = play::adjust_look_stick(hid::StickValue{12000, -8000}, low);
    const hid::StickValue b = play::adjust_look_stick(hid::StickValue{12000, -8000}, quarter);
    CHECK_EQ(a.x, b.x);
    CHECK_EQ(a.y, b.y);
    CHECK_EQ(b.x, 3000);
    CHECK_EQ(b.y, -2000);

    // above 3 behaves as 3: (1000, -1000) * 3 = (3000, -3000)
    play::PadSettings high;
    high.look_sens = 10.0;
    play::PadSettings three;
    three.look_sens = 3.0;
    const hid::StickValue c = play::adjust_look_stick(hid::StickValue{1000, -1000}, high);
    const hid::StickValue d = play::adjust_look_stick(hid::StickValue{1000, -1000}, three);
    CHECK_EQ(c.x, d.x);
    CHECK_EQ(c.y, d.y);
    CHECK_EQ(d.x, 3000);
    CHECK_EQ(d.y, -3000);
}

// ---- press-to-rebind (the F1 menu's Gamepad tab) ----------------------------------------------------------------------------------------------

NXTEST(sdl_buttons_name_their_sources) {
    play::PadSrc s;
    CHECK(play::pad_src_of_sdl_button(kSouth, &s) && s == play::PadSrc::South);
    CHECK(play::pad_src_of_sdl_button(kNorth, &s) && s == play::PadSrc::North);
    CHECK(play::pad_src_of_sdl_button(kBack, &s) && s == play::PadSrc::Back);
    CHECK(play::pad_src_of_sdl_button(kStart, &s) && s == play::PadSrc::Start);
    CHECK(play::pad_src_of_sdl_button(kLeftShoulder, &s) && s == play::PadSrc::LeftShoulder);
    CHECK(play::pad_src_of_sdl_button(kDpadRight, &s) && s == play::PadSrc::DpadRight);
    CHECK(!play::pad_src_of_sdl_button(kGuide, &s));
    CHECK(!play::pad_src_of_sdl_button(kMisc1, &s));
    CHECK(!play::pad_src_of_sdl_button(40, &s));
}

NXTEST(pressed_sources_include_the_triggers_at_the_threshold) {
    platform::GamepadRaw raw = pad_buttons((1u << kSouth) | (1u << kDpadUp));
    raw.axes[4] = 32767 / 2;   // left trigger half way
    raw.axes[5] = 32767;       // right trigger full
    const uint32_t b = play::pad_sources_pressed(raw, 0.6);
    CHECK(b & (1u << static_cast<int>(play::PadSrc::South)));
    CHECK(b & (1u << static_cast<int>(play::PadSrc::DpadUp)));
    CHECK(!(b & (1u << static_cast<int>(play::PadSrc::LeftTrigger))));
    CHECK(b & (1u << static_cast<int>(play::PadSrc::RightTrigger)));
    CHECK(play::pad_sources_pressed(raw, 0.4) & (1u << static_cast<int>(play::PadSrc::LeftTrigger)));
}

NXTEST(assign_swaps_with_the_button_that_had_the_job) {
    play::PadSettings cfg;   // by label: South A, East B
    const uint64_t a = hid::btn(hid::kBtnA), b = hid::btn(hid::kBtnB);
    CHECK(play::pad_effective_buttons(play::PadSrc::South, cfg) == a);
    CHECK(play::pad_assign(&cfg, play::PadSrc::South, b));    // South now presses B ...
    CHECK(play::pad_effective_buttons(play::PadSrc::South, cfg) == b);
    CHECK(play::pad_effective_buttons(play::PadSrc::East, cfg) == a);   // ... and East takes over South's old job
    CHECK(run(pad_buttons(1u << kSouth), cfg).buttons == b);
    CHECK(run(pad_buttons(1u << kEast), cfg).buttons == a);
    CHECK(!play::pad_assign(&cfg, play::PadSrc::South, b));   // nothing to change
    CHECK(play::pad_assign(&cfg, play::PadSrc::South, a));    // back: both are the built-in mapping again, stored as default
    CHECK(cfg.remap[static_cast<int>(play::PadSrc::South)] == play::kPadDefault);
    CHECK(cfg.remap[static_cast<int>(play::PadSrc::East)] == play::kPadDefault);
}

NXTEST(assign_to_a_trigger_and_a_combination) {
    play::PadSettings cfg;
    const uint64_t zr = hid::btn(hid::kBtnZR), x = hid::btn(hid::kBtnX);
    CHECK(play::pad_assign(&cfg, play::PadSrc::LeftShoulder, zr));   // LB presses ZR; RT takes over the L of LB
    CHECK(play::pad_effective_buttons(play::PadSrc::LeftShoulder, cfg) == zr);
    CHECK(play::pad_effective_buttons(play::PadSrc::RightTrigger, cfg) == hid::btn(hid::kBtnL));
    // a source that pressed a combination is not the one that had the job of a single button: it keeps its combination
    cfg.remap[static_cast<int>(play::PadSrc::Back)] = x | zr;
    CHECK(play::pad_assign(&cfg, play::PadSrc::North, x));
    CHECK(cfg.remap[static_cast<int>(play::PadSrc::Back)] == (x | zr));
    CHECK(play::pad_effective_buttons(play::PadSrc::West, cfg) == hid::btn(hid::kBtnY));   // West (X) got the Y of North
}
