// SPDX-License-Identifier: GPL-3.0-or-later
// platform 2 platform 2 controller mouse ("Nmouse" / "Jcms") from the PC mouse. The hid service reports one virtual platform 2 controller (R) that works as a
// mouse and writes its state into the game's Nmouse transfer memory (layout: docs/hid_mouse_sw2.md). The play layer feeds it the host
// pointer once per frame; whoever owns the mouse camera asks here whether it must hand the mouse over.
//
// Config: [input] jc2_mouse = auto | on | off (default auto).
//   off   no platform 2 controller mouse is listed; the game never sees one.
//   on    the mouse is always the platform 2 controller mouse in play: play mode never captures it for the camera.
//   auto  the platform 2 controller mouse is on its surface whenever the mouse is not captured by the camera. The camera owner releases it with
//         jc2_mouse_request(true) (for example while the game is placing objects or using Rock Smash, once that can be detected).
//
// Handover contract for the camera code:
//   * every frame, before taking mouse counts: if jc2_mouse_active() is true, do not turn the camera with the mouse (drop the counts);
//   * to give the mouse to the game in auto mode, call jc2_mouse_request(true) and later jc2_mouse_request(false); play mode then stops
//     capturing (relative mode off) while jc2_mouse_wants_pointer() is true, and the platform 2 controller mouse goes on its surface.
// All functions are thread safe and cheap.
#pragma once
#include <cstdint>

namespace nxrt::hid {

enum class Jc2MouseMode : uint8_t { Off = 0, Auto = 1, On = 2 };

Jc2MouseMode jc2_mouse_mode();                 // [input] jc2_mouse, read on first use (auto when missing or unknown)
void jc2_mouse_set_mode(Jc2MouseMode m);       // overrides the config value (tests, a future menu)
const char* jc2_mouse_mode_name(Jc2MouseMode m);

// The host pointer, once per rendered frame (play mode, main thread).
struct HostMouse {
    float u = 0, v = 0;          // pointer on the game picture, 0..1 (v down); outside the picture it is clamped
    uint32_t buttons = 0;        // nn::hid::MouseButton bits: Left 0, Right 1, Middle 2, Forward 3, Back 4
    float wheel_x = 0, wheel_y = 0;   // wheel notches since the previous feed (y: away from the user is +)
    bool focused = false;        // the window has focus
    bool captured = false;       // the camera holds the mouse (relative mode)
};
void jc2_mouse_feed(const HostMouse& m);

// Camera owner / game layer: the game is in a mode that wants a pointer (auto mode only; on and off ignore it).
void jc2_mouse_request(bool want);
// Play mode: true = do not capture the mouse for the camera (mode on, or auto with a request).
bool jc2_mouse_wants_pointer();
// True while the virtual platform 2 controller mouse is on its surface (the game sees mouse input): the camera must not use the mouse counts.
bool jc2_mouse_active();

}  // namespace nxrt::hid
