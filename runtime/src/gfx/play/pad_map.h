// SPDX-License-Identifier: GPL-3.0-or-later
// Play mode: from a gamepad as SDL reports it to the Switch pad the game expects. Pure functions, no SDL calls, no state.
#pragma once
#include "nxrt/input_source.h"
#include "nxrt/platform.h"
#include "play_types.h"

namespace nxrt::play {

// Fills out->buttons, out->left and out->right from the raw pad. Other fields of *out (connected, style, six_axis, colours) are untouched.
//  buttons   Switch button bits (hid::btn(hid::kBtnA) ...).
//            Each pressed physical source adds cfg.remap[source] when that is not kPadDefault (the bits of the value; 0 adds nothing, and the
//            built-in mapping of that source is then not added). A source with kPadDefault adds the built-in mapping below.
//            Face buttons: cfg.label_abxy = true maps by the printed label (raw.face_label: label A -> Switch A, B -> B, X -> X, Y -> Y), so an
//            Xbox pad's bottom button (label A) is Switch A; false maps by position: SOUTH -> B, EAST -> A, WEST -> Y, NORTH -> X.
//            LEFT_SHOULDER -> L, RIGHT_SHOULDER -> R, START -> PLUS, BACK -> MINUS, LEFT_STICK -> LStick click, RIGHT_STICK -> RStick click,
//            DPAD_* -> Up/Down/Left/Right. Triggers: axis value / 32767 >= cfg.trigger_threshold -> ZL / ZR. GUIDE and MISC1 do nothing.
//  sticks    raw axis / 32768 (clamped to -1..1), y flipped (SDL y down = +, hid y up = +), radial dead zone cfg.stick_deadzone with the
//            remaining travel rescaled to 0..1 ((len - dz) / (1 - dz)), then * 32767 rounded; the result is within +-32767.
// Stick dead zones: cfg.stick_deadzone for the left stick, cfg.right_deadzone for the right one.
void map_pad(const platform::GamepadRaw& raw, const PadSettings& cfg, hid::NpadInput* out);

// The right stick as the game's look input, in play: vertical flipped when cfg.invert_y, both axes times cfg.look_sens (clamped to 0.25..3), then pulled back
// onto the unit circle. Menus and the map do not call this (their stick is not a look).
hid::StickValue adjust_look_stick(hid::StickValue right, const PadSettings& cfg);

// The Switch button bits the built-in mapping gives a physical source (the kPadDefault case of map_pad), for a pad with the printed labels
// A B X Y on SOUTH EAST WEST NORTH (label_abxy true: South A, East B, West X, North Y; false: South B, East A, West Y, North X).
// LEFT_TRIGGER -> ZL, RIGHT_TRIGGER -> ZR, the other sources as in map_pad. Returns 0 for kCount.
uint64_t pad_default_buttons(PadSrc src, bool label_abxy);

// What a source presses with these settings: its remap, or the built-in buttons when it has none.
uint64_t pad_effective_buttons(PadSrc src, const PadSettings& cfg);

// The source of an SDL_GamepadButton number (platform.h GamepadRaw::buttons bit numbers). False for GUIDE, MISC1 and anything unknown:
// those are never bound.
bool pad_src_of_sdl_button(int sdl_button, PadSrc* out);

// The sources pressed in a raw state, as bits (1 << PadSrc): the buttons and the two triggers at the threshold.
uint32_t pad_sources_pressed(const platform::GamepadRaw& raw, double trigger_threshold);

// Press-to-rebind: makes `src` press the Switch buttons `target` (hid::btn bits). Every other source that pressed exactly `target` before
// takes over what `src` pressed before (a swap), so no Switch button is left without a pad button. A source whose result equals its built-in
// mapping is stored as kPadDefault. Returns true when anything changed.
bool pad_assign(PadSettings* cfg, PadSrc src, uint64_t target);

// Gyro and accelerometer in SDL axes (m/s^2, rad/s) to the six-axis sample the hid service stores (G, revolutions per second), the
// mapping that smo-recomp uses: accel = (-ax, ay, -az) / 9.80665, gyro = (-gx, gy, -gz) / (2 pi) * cfg.gyro_scale. A pad lying still, face up
// (SDL accel (0, 0, +9.80665)) gives accel (0, 0, -1). at_rest = |gyro| < 0.01 rev/s on every axis. cfg.gyro = false gives the rest sample
// (accel (0, 0, -1), gyro 0, at_rest).
void map_sensors(const float accel_sdl[3], const float gyro_sdl[3], const PadSettings& cfg, hid::SixAxisSample* out);

// Rumble: the Switch gives an amplitude/frequency pair per hand (nn::hid::VibrationValue: low band and high band). Returns the two SDL
// motor strengths (0..65535): low = low band amplitude, high = high band amplitude, both clamped to 0..1, times cfg.rumble_scale, clamped
// again, times 65535 and rounded. cfg.rumble = false gives (0, 0).
void map_rumble(float amp_low, float amp_high, const PadSettings& cfg, uint16_t* low, uint16_t* high);

}  // namespace nxrt::play
