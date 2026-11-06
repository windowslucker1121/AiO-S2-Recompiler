// SPDX-License-Identifier: GPL-3.0-or-later
// Play mode pad mapping: SDL gamepad state to the Switch pad state. Pure functions, see pad_map.h for the rules.
#include "play/pad_map.h"

#include <algorithm>
#include <cmath>

namespace nxrt::play {
namespace {

// SDL_GamepadButton bit numbers (platform.h GamepadRaw::buttons).
enum SdlButton : int {
    kSdlSouth = 0, kSdlEast, kSdlWest, kSdlNorth, kSdlBack, kSdlGuide, kSdlStart, kSdlLeftStick, kSdlRightStick,
    kSdlLeftShoulder, kSdlRightShoulder, kSdlDpadUp, kSdlDpadDown, kSdlDpadLeft, kSdlDpadRight, kSdlMisc1,
};
// SDL_GamepadAxis order (platform.h GamepadRaw::axes).
enum SdlAxis : int { kAxisLeftX = 0, kAxisLeftY, kAxisRightX, kAxisRightY, kAxisLeftTrigger, kAxisRightTrigger };

// Face buttons, indexed by the physical position (SOUTH, EAST, WEST, NORTH).
// By printed label: label 0 (A), 1 (B), 2 (X), 3 (Y) is the Switch button of the same name.
constexpr hid::NpadButtonBit kLabelBtn[4] = {hid::kBtnA, hid::kBtnB, hid::kBtnX, hid::kBtnY};
// By position: the bottom button is Switch B, right is A, left is Y, top is X.
constexpr hid::NpadButtonBit kPosBtn[4] = {hid::kBtnB, hid::kBtnA, hid::kBtnY, hid::kBtnX};

struct ButtonMap {
    int sdl;
    PadSrc src;
    hid::NpadButtonBit btn;
};
constexpr ButtonMap kOtherButtons[] = {
    {kSdlLeftShoulder, PadSrc::LeftShoulder, hid::kBtnL},   {kSdlRightShoulder, PadSrc::RightShoulder, hid::kBtnR},
    {kSdlStart, PadSrc::Start, hid::kBtnPlus},              {kSdlBack, PadSrc::Back, hid::kBtnMinus},
    {kSdlLeftStick, PadSrc::LeftStick, hid::kBtnStickL},    {kSdlRightStick, PadSrc::RightStick, hid::kBtnStickR},
    {kSdlDpadUp, PadSrc::DpadUp, hid::kBtnUp},              {kSdlDpadDown, PadSrc::DpadDown, hid::kBtnDown},
    {kSdlDpadLeft, PadSrc::DpadLeft, hid::kBtnLeft},        {kSdlDpadRight, PadSrc::DpadRight, hid::kBtnRight},
};

// SDL stick axis (-32768..32767) to -1..1.
double norm_axis(int16_t v) { return std::clamp(v / 32768.0, -1.0, 1.0); }

// -1..1 to the hid range, rounded and clamped.
int32_t to_stick(double v) {
    const long r = std::lround(v * 32767.0);
    return static_cast<int32_t>(std::clamp(r, -32767L, 32767L));
}

// Radial dead zone. x, y are -1..1 (hid y up = +). dz is already clamped to 0..0.9.
hid::StickValue stick(double x, double y, double dz) {
    const double len = std::sqrt(x * x + y * y);
    if (len > 1.0) {  // square corners: pull back onto the unit circle so the result never exceeds full deflection
        x /= len;
        y /= len;
    }
    const double r = std::min(len, 1.0);
    if (r <= dz) return {};
    const double s = (r - dz) / (1.0 - dz) / r;
    return {to_stick(x * s), to_stick(y * s)};
}

// Rumble amplitude 0..1 times the scale, clamped, to a motor value 0..65535.
uint16_t motor(float amp, double scale) {
    double v = std::clamp(static_cast<double>(amp), 0.0, 1.0) * scale;
    if (!(v > 0.0)) return 0;
    v = std::min(v, 1.0);
    return static_cast<uint16_t>(std::lround(v * 65535.0));
}

}  // namespace

void map_pad(const platform::GamepadRaw& raw, const PadSettings& cfg, hid::NpadInput* out) {
    uint64_t bits = 0;
    // A pressed source adds its remap, or its built-in buttons when it has none (kPadDefault).
    auto press = [&](PadSrc src, uint64_t builtin) {
        const uint64_t remap = cfg.remap[static_cast<int>(src)];
        bits |= remap == kPadDefault ? builtin : remap;
    };

    for (int pos = 0; pos < 4; ++pos) {
        if (!(raw.buttons & (1u << pos))) continue;
        const uint8_t label = raw.face_label[pos];
        // An out-of-range label has no meaning; fall back to the position.
        const bool by_label = cfg.label_abxy && label < 4;
        press(static_cast<PadSrc>(pos), hid::btn(by_label ? kLabelBtn[label] : kPosBtn[pos]));
    }
    for (const ButtonMap& m : kOtherButtons) {
        if (raw.buttons & (1u << m.sdl)) press(m.src, hid::btn(m.btn));
    }

    if (raw.axes[kAxisLeftTrigger] / 32767.0 >= cfg.trigger_threshold) press(PadSrc::LeftTrigger, hid::btn(hid::kBtnZL));
    if (raw.axes[kAxisRightTrigger] / 32767.0 >= cfg.trigger_threshold) press(PadSrc::RightTrigger, hid::btn(hid::kBtnZR));

    const double dz = std::clamp(cfg.stick_deadzone, 0.0, 0.9), rdz = std::clamp(cfg.right_deadzone, 0.0, 0.9);
    out->buttons = bits;
    out->left = stick(norm_axis(raw.axes[kAxisLeftX]), -norm_axis(raw.axes[kAxisLeftY]), dz);
    out->right = stick(norm_axis(raw.axes[kAxisRightX]), -norm_axis(raw.axes[kAxisRightY]), rdz);
}

hid::StickValue adjust_look_stick(hid::StickValue right, const PadSettings& cfg) {
    const double k = std::clamp(cfg.look_sens, 0.25, 3.0);
    double x = right.x / 32767.0 * k, y = right.y / 32767.0 * k * (cfg.invert_y ? -1.0 : 1.0);
    const double len = std::sqrt(x * x + y * y);
    if (len > 1.0) {
        x /= len;
        y /= len;
    }
    return {to_stick(x), to_stick(y)};
}

uint64_t pad_default_buttons(PadSrc src, bool label_abxy) {
    switch (src) {
        case PadSrc::South:
        case PadSrc::East:
        case PadSrc::West:
        case PadSrc::North: {
            const int pos = static_cast<int>(src);
            return hid::btn(label_abxy ? kLabelBtn[pos] : kPosBtn[pos]);
        }
        case PadSrc::LeftTrigger: return hid::btn(hid::kBtnZL);
        case PadSrc::RightTrigger: return hid::btn(hid::kBtnZR);
        default: break;
    }
    for (const ButtonMap& m : kOtherButtons) {
        if (m.src == src) return hid::btn(m.btn);
    }
    return 0;
}

uint64_t pad_effective_buttons(PadSrc src, const PadSettings& cfg) {
    const int i = static_cast<int>(src);
    if (i < 0 || i >= kPadSrcCount) return 0;
    const uint64_t r = cfg.remap[i];
    return r == kPadDefault ? pad_default_buttons(src, cfg.label_abxy) : r;
}

bool pad_src_of_sdl_button(int sdl_button, PadSrc* out) {
    if (sdl_button >= kSdlSouth && sdl_button <= kSdlNorth) {
        *out = static_cast<PadSrc>(sdl_button);   // SOUTH EAST WEST NORTH are 0..3 in both
        return true;
    }
    for (const ButtonMap& m : kOtherButtons) {
        if (m.sdl == sdl_button) {
            *out = m.src;
            return true;
        }
    }
    return false;
}

uint32_t pad_sources_pressed(const platform::GamepadRaw& raw, double trigger_threshold) {
    uint32_t bits = 0;
    for (int b = 0; b < 16; ++b) {
        PadSrc s;
        if ((raw.buttons & (1u << b)) && pad_src_of_sdl_button(b, &s)) bits |= 1u << static_cast<int>(s);
    }
    if (raw.axes[kAxisLeftTrigger] / 32767.0 >= trigger_threshold) bits |= 1u << static_cast<int>(PadSrc::LeftTrigger);
    if (raw.axes[kAxisRightTrigger] / 32767.0 >= trigger_threshold) bits |= 1u << static_cast<int>(PadSrc::RightTrigger);
    return bits;
}

bool pad_assign(PadSettings* cfg, PadSrc src, uint64_t target) {
    const int si = static_cast<int>(src);
    if (si < 0 || si >= kPadSrcCount) return false;
    uint64_t now[kPadSrcCount];
    for (int i = 0; i < kPadSrcCount; ++i) now[i] = pad_effective_buttons(static_cast<PadSrc>(i), *cfg);
    if (now[si] == target) return false;
    const uint64_t old = now[si];
    now[si] = target;
    for (int i = 0; i < kPadSrcCount; ++i)
        if (i != si && now[i] == target && target != 0) now[i] = old;
    bool changed = false;
    for (int i = 0; i < kPadSrcCount; ++i) {
        const uint64_t v = now[i] == pad_default_buttons(static_cast<PadSrc>(i), cfg->label_abxy) ? kPadDefault : now[i];
        if (cfg->remap[i] != v) {
            cfg->remap[i] = v;
            changed = true;
        }
    }
    return changed;
}

void map_sensors(const float accel_sdl[3], const float gyro_sdl[3], const PadSettings& cfg, hid::SixAxisSample* out) {
    constexpr double kGravity = 9.80665;
    constexpr double kTwoPi = 6.283185307179586;
    constexpr double kSign[3] = {-1.0, 1.0, -1.0};   // SDL x, y, z to hid

    if (!cfg.gyro) {
        *out = hid::SixAxisSample{};   // the rest sample: accel (0, 0, -1), gyro 0, at_rest
        return;
    }
    for (int i = 0; i < 3; ++i) {
        out->accel[i] = static_cast<float>(kSign[i] * accel_sdl[i] / kGravity);
    }
    bool at_rest = true;
    for (int i = 0; i < 3; ++i) {
        out->gyro[i] = static_cast<float>(kSign[i] * gyro_sdl[i] / kTwoPi * cfg.gyro_scale);
        if (std::fabs(out->gyro[i]) >= 0.01f) at_rest = false;
    }
    out->at_rest = at_rest;
}

void map_rumble(float amp_low, float amp_high, const PadSettings& cfg, uint16_t* low, uint16_t* high) {
    if (!cfg.rumble) {
        *low = 0;
        *high = 0;
        return;
    }
    *low = motor(amp_low, cfg.rumble_scale);
    *high = motor(amp_high, cfg.rumble_scale);
}

}  // namespace nxrt::play
