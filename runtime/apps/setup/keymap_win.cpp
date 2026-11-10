// SPDX-License-Identifier: GPL-3.0-or-later
// Win32 to SDL key translation. The tables and the extended-key rules are ported from SDL3:
// src/events/scancodes_windows.h (windows_scancode_table) and src/video/windows/SDL_windowsevents.c (WIN_KeyboardEventToSDL).
#include "keymap_win.h"

#include <cstdint>

#include <SDL3/SDL_scancode.h>

#ifndef MAPVK_VK_TO_VSC_EX
#define MAPVK_VK_TO_VSC_EX 4
#endif

namespace mprl {
namespace {

// Plain keys, indexed by scan code 0x00..0x7F. SDL_SCANCODE_UNKNOWN where Windows sends no SDL key.
constexpr SDL_Scancode kPlainKeys[0x80] = {
    // 0x00
    SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_ESCAPE, SDL_SCANCODE_1, SDL_SCANCODE_2, SDL_SCANCODE_3, SDL_SCANCODE_4, SDL_SCANCODE_5, SDL_SCANCODE_6,
    // 0x08
    SDL_SCANCODE_7, SDL_SCANCODE_8, SDL_SCANCODE_9, SDL_SCANCODE_0, SDL_SCANCODE_MINUS, SDL_SCANCODE_EQUALS, SDL_SCANCODE_BACKSPACE, SDL_SCANCODE_TAB,
    // 0x10
    SDL_SCANCODE_Q, SDL_SCANCODE_W, SDL_SCANCODE_E, SDL_SCANCODE_R, SDL_SCANCODE_T, SDL_SCANCODE_Y, SDL_SCANCODE_U, SDL_SCANCODE_I,
    // 0x18
    SDL_SCANCODE_O, SDL_SCANCODE_P, SDL_SCANCODE_LEFTBRACKET, SDL_SCANCODE_RIGHTBRACKET, SDL_SCANCODE_RETURN, SDL_SCANCODE_LCTRL, SDL_SCANCODE_A, SDL_SCANCODE_S,
    // 0x20
    SDL_SCANCODE_D, SDL_SCANCODE_F, SDL_SCANCODE_G, SDL_SCANCODE_H, SDL_SCANCODE_J, SDL_SCANCODE_K, SDL_SCANCODE_L, SDL_SCANCODE_SEMICOLON,
    // 0x28
    SDL_SCANCODE_APOSTROPHE, SDL_SCANCODE_GRAVE, SDL_SCANCODE_LSHIFT, SDL_SCANCODE_BACKSLASH, SDL_SCANCODE_Z, SDL_SCANCODE_X, SDL_SCANCODE_C, SDL_SCANCODE_V,
    // 0x30
    SDL_SCANCODE_B, SDL_SCANCODE_N, SDL_SCANCODE_M, SDL_SCANCODE_COMMA, SDL_SCANCODE_PERIOD, SDL_SCANCODE_SLASH, SDL_SCANCODE_RSHIFT, SDL_SCANCODE_KP_MULTIPLY,
    // 0x38
    SDL_SCANCODE_LALT, SDL_SCANCODE_SPACE, SDL_SCANCODE_CAPSLOCK, SDL_SCANCODE_F1, SDL_SCANCODE_F2, SDL_SCANCODE_F3, SDL_SCANCODE_F4, SDL_SCANCODE_F5,
    // 0x40
    SDL_SCANCODE_F6, SDL_SCANCODE_F7, SDL_SCANCODE_F8, SDL_SCANCODE_F9, SDL_SCANCODE_F10, SDL_SCANCODE_NUMLOCKCLEAR, SDL_SCANCODE_SCROLLLOCK, SDL_SCANCODE_KP_7,
    // 0x48
    SDL_SCANCODE_KP_8, SDL_SCANCODE_KP_9, SDL_SCANCODE_KP_MINUS, SDL_SCANCODE_KP_4, SDL_SCANCODE_KP_5, SDL_SCANCODE_KP_6, SDL_SCANCODE_KP_PLUS, SDL_SCANCODE_KP_1,
    // 0x50
    SDL_SCANCODE_KP_2, SDL_SCANCODE_KP_3, SDL_SCANCODE_KP_0, SDL_SCANCODE_KP_PERIOD, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_NONUSBACKSLASH, SDL_SCANCODE_F11,
    // 0x58
    SDL_SCANCODE_F12, SDL_SCANCODE_KP_EQUALS, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_INTERNATIONAL6, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN,
    // 0x60
    SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_F13, SDL_SCANCODE_F14, SDL_SCANCODE_F15, SDL_SCANCODE_F16,
    // 0x68
    SDL_SCANCODE_F17, SDL_SCANCODE_F18, SDL_SCANCODE_F19, SDL_SCANCODE_F20, SDL_SCANCODE_F21, SDL_SCANCODE_F22, SDL_SCANCODE_F23, SDL_SCANCODE_UNKNOWN,
    // 0x70
    SDL_SCANCODE_INTERNATIONAL2, SDL_SCANCODE_LANG2, SDL_SCANCODE_LANG1, SDL_SCANCODE_INTERNATIONAL1, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_F24, SDL_SCANCODE_LANG4,
    // 0x78
    SDL_SCANCODE_LANG3, SDL_SCANCODE_INTERNATIONAL4, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_INTERNATIONAL5, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_INTERNATIONAL3, SDL_SCANCODE_KP_COMMA, SDL_SCANCODE_UNKNOWN,
};

// Keys with the 0xE0 prefix (extended flag set), the 0xE0 half of SDL's table. Only the low byte is passed in.
int extended_key(uint32_t s) {
    switch (s) {
        case 0x07: return SDL_SCANCODE_AGAIN;
        case 0x08: return SDL_SCANCODE_UNDO;
        case 0x0A: return SDL_SCANCODE_PASTE;
        case 0x10: return SDL_SCANCODE_MEDIA_PREVIOUS_TRACK;
        case 0x17: return SDL_SCANCODE_CUT;
        case 0x18: return SDL_SCANCODE_COPY;
        case 0x19: return SDL_SCANCODE_MEDIA_NEXT_TRACK;
        case 0x1C: return SDL_SCANCODE_KP_ENTER;
        case 0x1D: return SDL_SCANCODE_RCTRL;
        case 0x20: return SDL_SCANCODE_MUTE;
        case 0x22: return SDL_SCANCODE_MEDIA_PLAY_PAUSE;
        case 0x24: return SDL_SCANCODE_MEDIA_STOP;
        case 0x2C: return SDL_SCANCODE_MEDIA_EJECT;
        case 0x2E: return SDL_SCANCODE_VOLUMEDOWN;
        case 0x30: return SDL_SCANCODE_VOLUMEUP;
        case 0x32: return SDL_SCANCODE_AC_HOME;
        case 0x35: return SDL_SCANCODE_KP_DIVIDE;
        case 0x36: return SDL_SCANCODE_RSHIFT;
        case 0x37: return SDL_SCANCODE_PRINTSCREEN;
        case 0x38: return SDL_SCANCODE_RALT;
        case 0x3B: return SDL_SCANCODE_HELP;
        case 0x3E: return SDL_SCANCODE_AC_NEW;
        case 0x3F: return SDL_SCANCODE_AC_OPEN;
        case 0x40: return SDL_SCANCODE_AC_CLOSE;
        case 0x45: return SDL_SCANCODE_NUMLOCKCLEAR;
        case 0x46: return SDL_SCANCODE_PAUSE;
        case 0x47: return SDL_SCANCODE_HOME;
        case 0x48: return SDL_SCANCODE_UP;
        case 0x49: return SDL_SCANCODE_PAGEUP;
        case 0x4B: return SDL_SCANCODE_LEFT;
        case 0x4D: return SDL_SCANCODE_RIGHT;
        case 0x4F: return SDL_SCANCODE_END;
        case 0x50: return SDL_SCANCODE_DOWN;
        case 0x51: return SDL_SCANCODE_PAGEDOWN;
        case 0x52: return SDL_SCANCODE_INSERT;
        case 0x53: return SDL_SCANCODE_DELETE;
        case 0x57: return SDL_SCANCODE_AC_SAVE;
        case 0x58: return SDL_SCANCODE_AC_PRINT;
        case 0x5B: return SDL_SCANCODE_LGUI;
        case 0x5C: return SDL_SCANCODE_RGUI;
        case 0x5D: return SDL_SCANCODE_APPLICATION;
        case 0x5E: return SDL_SCANCODE_POWER;
        case 0x5F: return SDL_SCANCODE_SLEEP;
        case 0x63: return SDL_SCANCODE_WAKE;
        case 0x65: return SDL_SCANCODE_AC_SEARCH;
        case 0x66: return SDL_SCANCODE_AC_BOOKMARKS;
        case 0x67: return SDL_SCANCODE_AC_REFRESH;
        case 0x68: return SDL_SCANCODE_AC_STOP;
        case 0x69: return SDL_SCANCODE_AC_FORWARD;
        case 0x6A: return SDL_SCANCODE_AC_BACK;
        case 0x6D: return SDL_SCANCODE_MEDIA_SELECT;
        default: return SDL_SCANCODE_UNKNOWN;
    }
}

}  // namespace

int sdl_scancode_from_key_message(WPARAM vk, LPARAM lparam) {
    const uint32_t bits = static_cast<uint32_t>(static_cast<uint64_t>(lparam));
    const bool extended = (bits & (1u << 24)) != 0;
    const uint32_t scan = (bits >> 16) & 0x7Fu;  // bit 23 is masked off, as SDL does for on-screen keyboards

    // Same encoding as SDL: 0x00xx for plain keys, 0xE0xx for extended keys.
    uint32_t code = 0;
    if (scan != 0) {
        if (extended) {
            code = 0xE000u | scan;
        } else if (scan == 0x45) {
            code = 0xE046u;  // Pause sends 0x45 without the extended flag; Num Lock sends it with the flag
        } else {
            code = scan;
        }
    } else {
        // No scan code from the message: take one from the virtual key, as SDL does. Pause comes back with the E1 prefix.
        code = LOWORD(MapVirtualKey(static_cast<UINT>(vk), MAPVK_VK_TO_VSC_EX));
        if (code == 0xE11Du) code = 0xE046u;
    }

    // Shift needs no virtual key: the left and right Shift scan codes (0x2A, 0x36) are already different.
    if ((code & 0xFF00u) == 0xE000u) return extended_key(code & 0xFFu);
    if (code < 0x80u) return kPlainKeys[code];
    return SDL_SCANCODE_UNKNOWN;
}

int sdl_mouse_button_from_message(UINT msg, WPARAM wparam) {
    switch (msg) {
        case WM_LBUTTONDOWN: return 1;
        case WM_MBUTTONDOWN: return 2;
        case WM_RBUTTONDOWN: return 3;
        case WM_XBUTTONDOWN:
            if (GET_XBUTTON_WPARAM(wparam) == XBUTTON1) return 4;
            if (GET_XBUTTON_WPARAM(wparam) == XBUTTON2) return 5;
            return 0;
        default: return 0;
    }
}

int wheel_notch_from_message(WPARAM wparam) {
    const short delta = GET_WHEEL_DELTA_WPARAM(wparam);
    if (delta > 0) return 1;
    if (delta < 0) return -1;
    return 0;
}

}  // namespace mprl
