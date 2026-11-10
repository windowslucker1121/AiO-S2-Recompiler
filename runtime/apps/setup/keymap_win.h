// SPDX-License-Identifier: GPL-3.0-or-later
// Win32 key and mouse messages to the SDL values the launcher stores in controls.ini.
#pragma once

#include <windows.h>

namespace mprl {

// WM_KEYDOWN, WM_SYSKEYDOWN and WM_KEYUP. The scan code is in lparam bits 16..23 and the extended flag is bit 24.
// vk is only used when the scan code is 0. Returns an SDL_Scancode value, or 0 (SDL_SCANCODE_UNKNOWN) for no SDL key.
int sdl_scancode_from_key_message(WPARAM vk, LPARAM lparam);

// WM_LBUTTONDOWN, WM_MBUTTONDOWN, WM_RBUTTONDOWN, WM_XBUTTONDOWN: 1 left, 2 middle, 3 right, 4 x1, 5 x2. 0 for anything else.
int sdl_mouse_button_from_message(UINT msg, WPARAM wparam);

// WM_MOUSEWHEEL: +1 for wheel up, -1 for wheel down, 0 for no movement.
int wheel_notch_from_message(WPARAM wparam);

}  // namespace mprl
