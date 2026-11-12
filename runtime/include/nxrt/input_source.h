// SPDX-License-Identifier: GPL-3.0-or-later
// Where the hid service gets its controller state from (S3c). The hid service owns the applet-resource shared memory and
// refreshes it from an InputSource roughly every 5 ms (the console's 200 Hz sampling); the input / PC-feature streams provide a
// real source later (SDL3 pads, keyboard and mouse). This header has no SDL3 dependency; ManualInputSource is the source the tests use.
//
// The state is style-agnostic: the hid service fills FullKey / Handheld / JoyDual (and JoyLeft / JoyRight / Palma entries where the
// layout needs them) from the same buttons and sticks. Button bits are nn::hid::NpadButton (https://switchbrew.org/wiki/HID_services#NpadButton).
//
// The game also imports nn::hid::GetNpadState* directly (a hook-table override at that import can answer from a source without
// going through shared memory); both paths stay possible because the shared memory is always kept up to date.
#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace nxrt::hid {

// nn::hid::NpadButton bit numbers.
enum NpadButtonBit : uint32_t {
    kBtnA = 0, kBtnB, kBtnX, kBtnY, kBtnStickL, kBtnStickR, kBtnL, kBtnR, kBtnZL, kBtnZR, kBtnPlus, kBtnMinus,
    kBtnLeft, kBtnUp, kBtnRight, kBtnDown,
    kBtnStickLLeft, kBtnStickLUp, kBtnStickLRight, kBtnStickLDown,
    kBtnStickRLeft, kBtnStickRUp, kBtnStickRRight, kBtnStickRDown,
    kBtnLeftSL, kBtnLeftSR, kBtnRightSL, kBtnRightSR,
};
constexpr uint64_t btn(NpadButtonBit b) { return 1ull << b; }

// NpadIdType values used by the service: players 0..7, 0x20 handheld, 0x10 "other".
constexpr uint32_t kNpadHandheld = 0x20, kNpadOther = 0x10, kNpadPlayers = 8, kNpadSlots = 10;   // slot 8 = other, 9 = handheld
constexpr uint32_t kNpadSlotOther = 8, kNpadSlotHandheld = 9;
inline uint32_t npad_id_of_slot(uint32_t slot) { return slot < kNpadPlayers ? slot : (slot == kNpadSlotOther ? kNpadOther : kNpadHandheld); }

enum class NpadStyle : uint8_t { None = 0, FullKey, Handheld, JoyDual, JoyLeft, JoyRight };

struct StickValue { int32_t x = 0, y = 0; };            // -32767..32767

struct SixAxisSample {
    float accel[3] = {0, 0, -1.0f};                     // in G (the console reports 1.0 downwards at rest)
    float gyro[3] = {0, 0, 0};                          // revolutions per second
    float angle[3] = {0, 0, 0};                         // the pad's turn since it was reset, per axis, in revolutions (what the console's own sensor fusion reports)
    float direction[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};   // the pad's x, y and z axes in the world frame, three floats each (identity: lying still, face up)
    bool at_rest = true;
};

struct NpadInput {
    bool connected = false;
    NpadStyle style = NpadStyle::FullKey;               // what the pad presents as (Handheld for the docked-less handheld pad)
    uint64_t buttons = 0;                               // NpadButton bits (the stick-direction bits are derived by the service when 0)
    StickValue left, right;
    SixAxisSample six_axis;
    uint32_t color_body = 0xFF323232, color_buttons = 0xFFFFFFFF;   // 0xAABBGGRR as the service stores them (dark grey body, white buttons)
};

struct TouchFinger {
    uint32_t id = 0;
    uint32_t x = 0, y = 0;                               // pixels on the 1280x720 touch screen
    uint32_t diameter_x = 15, diameter_y = 15;
    int32_t angle = 0;
};
struct TouchInput {
    uint32_t count = 0;                                  // 0..16
    TouchFinger fingers[16];
};

struct KeyboardInput {
    uint64_t keys[4] = {};                               // bitmap of HID usage ids 0..255 (nn::hid::KeyboardKey)
    uint32_t modifiers = 0;                              // nn::hid::KeyboardModifier bits (Control 0, Shift 1, LeftAlt 2, RightAlt 3, Gui 4, CapsLock 8, ScrollLock 9, NumLock 10 ...)
};

struct MouseInput {
    int32_t x = 0, y = 0;                                // pointer position
    int32_t dx = 0, dy = 0;                              // movement since the previous sample
    int32_t wheel_dx = 0, wheel_dy = 0;
    uint32_t buttons = 0;                                // nn::hid::MouseButton bits (Left 0, Right 1, Middle 2, Forward 3, Back 4)
    bool connected = false;
};

struct InputSnapshot {
    NpadInput npad[kNpadSlots];                          // see kNpadSlot*
    TouchInput touch;
    KeyboardInput keyboard;
    MouseInput mouse;
    bool keyboard_connected = false;
};

class InputSource {
public:
    virtual ~InputSource() = default;
    // Fills `out` with the current state. Called from the service's sampling thread; must be cheap and thread safe.
    virtual void poll(InputSnapshot& out) = 0;
};

// The source the service reads. nullptr (the default) means "no input": every npad is disconnected except the handheld pad in
// handheld mode and player 1 as a connected idle FullKey pad (so a game that waits for a controller proceeds).
void set_input_source(std::shared_ptr<InputSource> src);
std::shared_ptr<InputSource> input_source();

// Which open pad (SDL instance id, nxrt::input::GamepadInfo::id) plays which player beyond player 1: pad_ids[0] is player 2, pad_ids[1]
// player 3, ... Empty (the default): every pad and the keyboard drive player 1 together. The controller applet (am.cpp) sets it when a
// game asks for two or more players; the play layer's source (play.cpp poll_pads) reads it. A listed pad that is not open is a
// disconnected player.
void set_player_pads(const std::vector<uint32_t>& pad_ids);
std::vector<uint32_t> player_pads();

// Test aid ([test] unplug_player1_at_s, play.cpp): player 1 reads as disconnected, as when the console's only controller is pulled,
// until the controller applet answers (am.cpp clears it). Lets a run reach the game's own ShowControllerSupport call.
void set_player1_unplugged(bool on);
bool player1_unplugged();

// A source driven by code: the tests and scripted runs. Thread safe.
class ManualInputSource : public InputSource {
public:
    void poll(InputSnapshot& out) override {
        std::lock_guard<std::mutex> lk(mu_);
        out = snap_;
    }
    void set(const InputSnapshot& s) {
        std::lock_guard<std::mutex> lk(mu_);
        snap_ = s;
    }
    InputSnapshot get() {
        std::lock_guard<std::mutex> lk(mu_);
        return snap_;
    }
    void press(uint32_t slot, uint64_t buttons) {
        std::lock_guard<std::mutex> lk(mu_);
        snap_.npad[slot].buttons |= buttons;
    }
    void release(uint32_t slot, uint64_t buttons) {
        std::lock_guard<std::mutex> lk(mu_);
        snap_.npad[slot].buttons &= ~buttons;
    }
    void connect(uint32_t slot, NpadStyle style) {
        std::lock_guard<std::mutex> lk(mu_);
        snap_.npad[slot].connected = true;
        snap_.npad[slot].style = style;
    }

private:
    std::mutex mu_;
    InputSnapshot snap_;
};

// Vibration the game sends to a pad (nn::hid SendVibrationValue / SendVibrationValues): the newest value for one hand of one pad. The play-mode
// layer registers a sink that turns it into SDL rumble. The sink is called on the thread that handled the IPC command; it must be quick.
struct VibrationValue {
    float amp_low = 0, freq_low = 160.0f, amp_high = 0, freq_high = 320.0f;   // amplitudes 0..1, frequencies in Hz
};
using VibrationSink = void (*)(uint32_t npad_slot, int hand, const VibrationValue& v, void* user);   // hand 0 = left (or the only one), 1 = right
void set_vibration_sink(VibrationSink fn, void* user = nullptr);        // nullptr removes it

}  // namespace nxrt::hid
