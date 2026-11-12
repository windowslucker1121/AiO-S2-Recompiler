// SPDX-License-Identifier: GPL-3.0-or-later
// Play mode, TEST ONLY: the virtual SDL gamepad behind "vpad ..." test steps (see play_vpad.h for the command list).
// The state lock is never held across an SDL call: SDL may call the rumble callback from inside its own functions.
#include "play_vpad.h"

#include <SDL3/SDL.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

#include "nxrt/log.h"

namespace nxrt::play {
namespace {

constexpr const char* kTag = "vpad";
constexpr const char* kName = "nxrt virtual pad";

struct KindId {
    const char* name;
    uint16_t vendor;
    uint16_t product;
};
constexpr KindId kKinds[] = {
    {"xbox", 0x045E, 0x028E},
    {"ps4", 0x054C, 0x09CC},
    {"ps5", 0x054C, 0x0CE6},
    {"switch", 0x057E, 0x2009},
};

struct AxisId {
    const char* name;
    SDL_GamepadAxis axis;
    bool trigger;
};
constexpr AxisId kAxes[] = {
    {"lx", SDL_GAMEPAD_AXIS_LEFTX, false},
    {"ly", SDL_GAMEPAD_AXIS_LEFTY, false},
    {"rx", SDL_GAMEPAD_AXIS_RIGHTX, false},
    {"ry", SDL_GAMEPAD_AXIS_RIGHTY, false},
    {"lt", SDL_GAMEPAD_AXIS_LEFT_TRIGGER, true},
    {"rt", SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, true},
};

struct ButtonId {
    const char* name;
    SDL_GamepadButton button;
};
constexpr ButtonId kButtons[] = {
    {"south", SDL_GAMEPAD_BUTTON_SOUTH},
    {"east", SDL_GAMEPAD_BUTTON_EAST},
    {"west", SDL_GAMEPAD_BUTTON_WEST},
    {"north", SDL_GAMEPAD_BUTTON_NORTH},
    {"back", SDL_GAMEPAD_BUTTON_BACK},
    {"guide", SDL_GAMEPAD_BUTTON_GUIDE},
    {"start", SDL_GAMEPAD_BUTTON_START},
    {"lstick", SDL_GAMEPAD_BUTTON_LEFT_STICK},
    {"rstick", SDL_GAMEPAD_BUTTON_RIGHT_STICK},
    {"lshoulder", SDL_GAMEPAD_BUTTON_LEFT_SHOULDER},
    {"rshoulder", SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
    {"up", SDL_GAMEPAD_BUTTON_DPAD_UP},
    {"down", SDL_GAMEPAD_BUTTON_DPAD_DOWN},
    {"left", SDL_GAMEPAD_BUTTON_DPAD_LEFT},
    {"right", SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
};

const SDL_VirtualJoystickSensorDesc kSensors[2] = {
    {SDL_SENSOR_ACCEL, 100.0f},
    {SDL_SENSOR_GYRO, 100.0f},
};

struct VpadState {
    std::mutex mu;
    SDL_JoystickID id = 0;
    SDL_Joystick* joy = nullptr;
    VpadRumble rumble;
};

VpadState& vpad_state() {
    static VpadState s;
    return s;
}

// SDL calls this with the game's rumble. Logs only when the motor values change.
bool vpad_rumble_cb(void*, Uint16 low, Uint16 high) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> g(vpad_state().mu);
        VpadRumble& r = vpad_state().rumble;
        r.calls++;
        if (r.low != low || r.high != high) {
            r.low = low;
            r.high = high;
            changed = true;
        }
    }
    if (changed) NXLOG_I(kTag, "vpad rumble low=%u high=%u", static_cast<unsigned>(low), static_cast<unsigned>(high));
    return true;
}

SDL_Joystick* vpad_current_joy() {
    std::lock_guard<std::mutex> g(vpad_state().mu);
    return vpad_state().joy;
}

void vpad_detach() {
    SDL_Joystick* joy = nullptr;
    SDL_JoystickID id = 0;
    {
        std::lock_guard<std::mutex> g(vpad_state().mu);
        joy = vpad_state().joy;
        id = vpad_state().id;
        vpad_state().joy = nullptr;
        vpad_state().id = 0;
    }
    if (!joy && !id) return;
    if (joy) SDL_CloseJoystick(joy);
    if (id && !SDL_DetachVirtualJoystick(id)) NXLOG_W(kTag, "vpad detach failed: %s", SDL_GetError());
    SDL_UpdateJoysticks();
    NXLOG_I(kTag, "vpad detached");
}

bool vpad_attach(const KindId& k) {
    vpad_detach();
    SDL_VirtualJoystickDesc d;
    SDL_INIT_INTERFACE(&d);
    d.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    d.naxes = SDL_GAMEPAD_AXIS_COUNT;
    d.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    d.vendor_id = k.vendor;
    d.product_id = k.product;
    d.name = kName;
    d.nsensors = 2;
    d.sensors = kSensors;
    d.Rumble = vpad_rumble_cb;
    const SDL_JoystickID id = SDL_AttachVirtualJoystick(&d);
    if (!id) {
        NXLOG_W(kTag, "vpad attach failed: %s", SDL_GetError());
        return false;
    }
    SDL_Joystick* joy = SDL_OpenJoystick(id);
    if (!joy) {
        NXLOG_W(kTag, "vpad open failed: %s", SDL_GetError());
        SDL_DetachVirtualJoystick(id);
        SDL_UpdateJoysticks();
        return false;
    }
    {
        std::lock_guard<std::mutex> g(vpad_state().mu);
        vpad_state().id = id;
        vpad_state().joy = joy;
        vpad_state().rumble = VpadRumble{};
    }
    SDL_UpdateJoysticks();
    NXLOG_I(kTag, "vpad attached %s (%04x:%04x) id=%u", k.name, static_cast<unsigned>(k.vendor), static_cast<unsigned>(k.product),
            static_cast<unsigned>(id));
    return true;
}

// Splits on spaces, dropping empty tokens.
std::vector<std::string> vpad_split(const char* s) {
    std::vector<std::string> out;
    std::string cur;
    for (const char* p = s;; ++p) {
        if (*p == ' ' || *p == '\0') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
            if (*p == '\0') break;
        } else {
            cur.push_back(*p);
        }
    }
    return out;
}

bool vpad_parse_float(const std::string& s, float* out) {
    char* end = nullptr;
    const float v = std::strtof(s.c_str(), &end);
    if (end == s.c_str() || *end != '\0' || !std::isfinite(v)) return false;
    *out = v;
    return true;
}

// Stick: -1..1 maps to -32768..32767 (down is positive on ly/ry, as SDL has it). Trigger: 0..1 maps to -32768..32767 (rest is -32768).
bool vpad_axis_value(const AxisId& a, float v, Sint16* out) {
    double scaled = 0.0;
    if (a.trigger) {
        if (v < 0.0f || v > 1.0f) return false;
        scaled = -32768.0 + static_cast<double>(v) * 65535.0;
    } else {
        if (v < -1.0f || v > 1.0f) return false;
        scaled = v < 0.0f ? static_cast<double>(v) * 32768.0 : static_cast<double>(v) * 32767.0;
    }
    long r = std::lround(scaled);
    if (r < -32768) r = -32768;
    if (r > 32767) r = 32767;
    *out = static_cast<Sint16>(r);
    return true;
}

const KindId* vpad_find_kind(const std::string& name) {
    for (const KindId& k : kKinds) {
        if (name == k.name) return &k;
    }
    return nullptr;
}

const AxisId* vpad_find_axis(const std::string& name) {
    for (const AxisId& a : kAxes) {
        if (name == a.name) return &a;
    }
    return nullptr;
}

const ButtonId* vpad_find_button(const std::string& name) {
    for (const ButtonId& b : kButtons) {
        if (name == b.name) return &b;
    }
    return nullptr;
}

// A second virtual pad for two-player tests ("vpad attach2 xbox", "vpad button2 south 1", "vpad detach2"): buttons only, no rumble or
// sensors. Only the thread that runs the test steps touches it.
SDL_JoystickID g_id2 = 0;
SDL_Joystick* g_joy2 = nullptr;

void vpad_detach2() {
    if (g_joy2) SDL_CloseJoystick(g_joy2);
    if (g_id2) SDL_DetachVirtualJoystick(g_id2);
    if (g_joy2 || g_id2) {
        SDL_UpdateJoysticks();
        NXLOG_I(kTag, "vpad 2 detached");
    }
    g_joy2 = nullptr;
    g_id2 = 0;
}

bool vpad_attach2(const KindId& k) {
    vpad_detach2();
    SDL_VirtualJoystickDesc d;
    SDL_INIT_INTERFACE(&d);
    d.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    d.naxes = SDL_GAMEPAD_AXIS_COUNT;
    d.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    d.vendor_id = k.vendor;
    d.product_id = k.product;
    d.name = "nxrt virtual pad 2";
    g_id2 = SDL_AttachVirtualJoystick(&d);
    if (!g_id2) return false;
    g_joy2 = SDL_OpenJoystick(g_id2);
    SDL_UpdateJoysticks();
    NXLOG_I(kTag, "vpad 2 attached %s id=%u", k.name, static_cast<unsigned>(g_id2));
    return g_joy2 != nullptr;
}

bool vpad_run(const std::vector<std::string>& t) {
    if (t.empty()) return false;
    const std::string& cmd = t[0];

    if (cmd == "attach2") {
        const KindId* k = vpad_find_kind(t.size() == 2 ? t[1] : std::string("xbox"));
        return k && t.size() <= 2 && vpad_attach2(*k);
    }
    if (cmd == "detach2") {
        vpad_detach2();
        return true;
    }
    if (cmd == "button2") {
        if (t.size() != 3 || !g_joy2) return false;
        const ButtonId* b = vpad_find_button(t[1]);
        if (!b || (t[2] != "0" && t[2] != "1")) return false;
        return SDL_SetJoystickVirtualButton(g_joy2, static_cast<int>(b->button), t[2] == "1");
    }

    if (cmd == "attach") {
        if (t.size() > 2) return false;
        const KindId* k = vpad_find_kind(t.size() == 2 ? t[1] : std::string("xbox"));
        if (!k) return false;
        return vpad_attach(*k);
    }

    if (cmd == "detach") {
        if (t.size() != 1) return false;
        vpad_detach();
        return true;
    }

    if (cmd == "axis") {
        if (t.size() != 3) return false;
        const AxisId* a = vpad_find_axis(t[1]);
        float v = 0.0f;
        if (!a || !vpad_parse_float(t[2], &v)) return false;
        Sint16 value = 0;
        if (!vpad_axis_value(*a, v, &value)) return false;
        SDL_Joystick* joy = vpad_current_joy();
        if (!joy) return false;
        return SDL_SetJoystickVirtualAxis(joy, static_cast<int>(a->axis), value);
    }

    if (cmd == "button") {
        if (t.size() != 3) return false;
        const ButtonId* b = vpad_find_button(t[1]);
        if (!b || (t[2] != "0" && t[2] != "1")) return false;
        SDL_Joystick* joy = vpad_current_joy();
        if (!joy) return false;
        return SDL_SetJoystickVirtualButton(joy, static_cast<int>(b->button), t[2] == "1");
    }

    if (cmd == "gyro" || cmd == "accel") {
        if (t.size() != 4) return false;
        float data[3] = {0.0f, 0.0f, 0.0f};
        for (int i = 0; i < 3; ++i) {
            if (!vpad_parse_float(t[static_cast<size_t>(i) + 1], &data[i])) return false;
        }
        SDL_Joystick* joy = vpad_current_joy();
        if (!joy) return false;
        const SDL_SensorType type = cmd == "gyro" ? SDL_SENSOR_GYRO : SDL_SENSOR_ACCEL;
        return SDL_SendJoystickVirtualSensorData(joy, type, SDL_GetTicksNS(), data, 3);
    }

    return false;
}

}  // namespace

bool vpad_command(const char* args) {
    if (!args) return false;
    const bool ok = vpad_run(vpad_split(args));
    if (!ok) NXLOG_W(kTag, "vpad command rejected: '%s'", args);
    return ok;
}

void vpad_shutdown() {
    vpad_detach();
    vpad_detach2();
}

VpadRumble vpad_last_rumble() {
    std::lock_guard<std::mutex> g(vpad_state().mu);
    return vpad_state().rumble;
}

}  // namespace nxrt::play
