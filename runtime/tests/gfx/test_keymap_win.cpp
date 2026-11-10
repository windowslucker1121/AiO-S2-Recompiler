// SPDX-License-Identifier: GPL-3.0-or-later
// Library app key and mouse translation (apps/setup/keymap_win.cpp): Win32 messages to SDL scancodes and button numbers.
#include "test_framework.h"

#include <cstdint>
#include <cstdio>
#include <string>

#include <SDL3/SDL_scancode.h>

#include "keymap_win.h"
#include "play/play_types.h"

NXTEST_GROUP(launcher_keymap)

namespace {

namespace play = nxrt::play;

// lparam of a key message: scan code in bits 16..23, extended flag in bit 24.
LPARAM make_lparam(unsigned scan, bool extended) {
    return static_cast<LPARAM>((scan << 16) | (extended ? (1u << 24) : 0u));
}

int key(unsigned scan, bool extended = false, WPARAM vk = 0) {
    return mprl::sdl_scancode_from_key_message(vk, make_lparam(scan, extended));
}

// wparam of WM_MOUSEWHEEL: the wheel delta is the high word.
WPARAM wheel(short delta) {
    return static_cast<WPARAM>(static_cast<WORD>(delta)) << 16;
}

std::string hex(unsigned v) {
    char b[16];
    std::snprintf(b, sizeof b, "0x%02X", v);
    return b;
}

}  // namespace

NXTEST(plain_keys) {
    CHECK_EQ(key(0x11), SDL_SCANCODE_W);
    CHECK_EQ(key(0x1E), SDL_SCANCODE_A);
    CHECK_EQ(key(0x39), SDL_SCANCODE_SPACE);
    CHECK_EQ(key(0x01), SDL_SCANCODE_ESCAPE);
    CHECK_EQ(key(0x1C), SDL_SCANCODE_RETURN);
    CHECK_EQ(key(0x0F), SDL_SCANCODE_TAB);
    CHECK_EQ(key(0x02), SDL_SCANCODE_1);
    CHECK_EQ(key(0x0B), SDL_SCANCODE_0);
    CHECK_EQ(key(0x3B), SDL_SCANCODE_F1);
    CHECK_EQ(key(0x44), SDL_SCANCODE_F10);
    CHECK_EQ(key(0x57), SDL_SCANCODE_F11);
    CHECK_EQ(key(0x58), SDL_SCANCODE_F12);
}

NXTEST(modifiers_left_and_right) {
    CHECK_EQ(key(0x1D), SDL_SCANCODE_LCTRL);
    CHECK_EQ(key(0x1D, true), SDL_SCANCODE_RCTRL);
    CHECK_EQ(key(0x38), SDL_SCANCODE_LALT);
    CHECK_EQ(key(0x38, true), SDL_SCANCODE_RALT);
    CHECK_EQ(key(0x2A), SDL_SCANCODE_LSHIFT);
    CHECK_EQ(key(0x36), SDL_SCANCODE_RSHIFT);
}

NXTEST(keypad_enter_and_return) {
    CHECK_EQ(key(0x1C, true), SDL_SCANCODE_KP_ENTER);
    CHECK_EQ(key(0x1C), SDL_SCANCODE_RETURN);
}

NXTEST(arrows_and_keypad_share_scan_codes) {
    CHECK_EQ(key(0x48, true), SDL_SCANCODE_UP);
    CHECK_EQ(key(0x50, true), SDL_SCANCODE_DOWN);
    CHECK_EQ(key(0x4B, true), SDL_SCANCODE_LEFT);
    CHECK_EQ(key(0x4D, true), SDL_SCANCODE_RIGHT);
    CHECK_EQ(key(0x48), SDL_SCANCODE_KP_8);
    CHECK_EQ(key(0x50), SDL_SCANCODE_KP_2);
    CHECK_EQ(key(0x4B), SDL_SCANCODE_KP_4);
    CHECK_EQ(key(0x4D), SDL_SCANCODE_KP_6);
}

NXTEST(navigation_cluster) {
    CHECK_EQ(key(0x49, true), SDL_SCANCODE_PAGEUP);
    CHECK_EQ(key(0x51, true), SDL_SCANCODE_PAGEDOWN);
    CHECK_EQ(key(0x47, true), SDL_SCANCODE_HOME);
    CHECK_EQ(key(0x4F, true), SDL_SCANCODE_END);
    CHECK_EQ(key(0x52, true), SDL_SCANCODE_INSERT);
    CHECK_EQ(key(0x53, true), SDL_SCANCODE_DELETE);
    CHECK_EQ(key(0x53), SDL_SCANCODE_KP_PERIOD);
}

NXTEST(divide_slash_and_print_screen) {
    CHECK_EQ(key(0x35, true), SDL_SCANCODE_KP_DIVIDE);
    CHECK_EQ(key(0x35), SDL_SCANCODE_SLASH);
    CHECK_EQ(key(0x37, true), SDL_SCANCODE_PRINTSCREEN);
    CHECK_EQ(key(0x37), SDL_SCANCODE_KP_MULTIPLY);
}

NXTEST(pause_and_num_lock) {
    // Pause sends scan 0x45 without the extended flag; Num Lock sends 0x45 with it.
    CHECK_EQ(key(0x45), SDL_SCANCODE_PAUSE);
    CHECK_EQ(key(0x45, true), SDL_SCANCODE_NUMLOCKCLEAR);
    CHECK_EQ(key(0x46), SDL_SCANCODE_SCROLLLOCK);
}

NXTEST(unknown_keys_give_zero) {
    CHECK_EQ(key(0), 0);                    // no scan code and no virtual key
    CHECK_EQ(key(0x54), 0);
    CHECK_EQ(key(0x55), 0);
    CHECK_EQ(key(0x1E, true), 0);           // extended A does not exist
}

NXTEST(mouse_buttons) {
    CHECK_EQ(mprl::sdl_mouse_button_from_message(WM_LBUTTONDOWN, 0), 1);
    CHECK_EQ(mprl::sdl_mouse_button_from_message(WM_MBUTTONDOWN, 0), 2);
    CHECK_EQ(mprl::sdl_mouse_button_from_message(WM_RBUTTONDOWN, 0), 3);
    CHECK_EQ(mprl::sdl_mouse_button_from_message(WM_XBUTTONDOWN, MAKEWPARAM(0, XBUTTON1)), 4);
    CHECK_EQ(mprl::sdl_mouse_button_from_message(WM_XBUTTONDOWN, MAKEWPARAM(0, XBUTTON2)), 5);
    CHECK_EQ(mprl::sdl_mouse_button_from_message(WM_LBUTTONUP, 0), 0);
    CHECK_EQ(mprl::sdl_mouse_button_from_message(WM_RBUTTONUP, 0), 0);
    CHECK_EQ(mprl::sdl_mouse_button_from_message(WM_MOUSEMOVE, 0), 0);
}

NXTEST(wheel_notches) {
    CHECK_EQ(mprl::wheel_notch_from_message(wheel(WHEEL_DELTA)), 1);
    CHECK_EQ(mprl::wheel_notch_from_message(wheel(-WHEEL_DELTA)), -1);
    CHECK_EQ(mprl::wheel_notch_from_message(wheel(0)), 0);
    CHECK_EQ(mprl::wheel_notch_from_message(wheel(2 * WHEEL_DELTA)), 1);  // high-resolution wheels send multiples of 120
}

// Every plain scan code the translation returns must have a name that parses back to the same code, since controls.ini stores names.
// Scan 0x45 (Pause) and 0x56 (non-US backslash) are included. SDL also names SDL_SCANCODE_RETURN2 "Return", but this translation
// never returns RETURN2, and parse_input("Return") gives SDL_SCANCODE_RETURN, so no exception is needed here.
NXTEST(plain_keys_name_round_trip) {
    for (unsigned scan = 1; scan <= 0x58; ++scan) {
        const int code = key(scan, false);
        if (code == 0) continue;  // no SDL key for this scan code
        const std::string where = "scan " + hex(scan) + " code " + std::to_string(code);
        const std::string name = play::input_name(play::InputId{play::InKind::Key, code});
        CHECK_MSG(!name.empty(), where + ": no name");
        play::InputId back;
        CHECK_MSG(play::parse_input(name, &back), where + ": name \"" + name + "\" does not parse");
        CHECK_MSG(back.kind == play::InKind::Key && back.code == code,
                  where + ": name \"" + name + "\" parses to another code " + std::to_string(back.code));
    }
}
