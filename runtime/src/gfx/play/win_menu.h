// SPDX-License-Identifier: GPL-3.0-or-later
// The game window's native menu bar (Windows): Game, Input, Graphics, Audio, Debug, Help, built from menu_model.h, and the dialogs it opens
// (Gamepad, Keyboard and mouse bindings, More graphics settings, Debug info, Log). runtime/docs/MENU.md.
//
// Threads: the bar belongs to the game window, so its commands arrive on the thread that pumps the window (the game's main thread) through a
// window subclass. Windows tracks an open drop-down in a modal loop on that thread: the game waits while a drop-down is open (a timer keeps the
// freeze watchdog fed meanwhile). The dialogs are modeless and run on a thread of their own, so the game keeps running while they are open;
// they talk to play mode only through UiHost (thread safe). Settings changes go through UiHost::apply, as the in-game page does.
#pragma once
#include <functional>
#include <string>

#include "play_ui.h"

namespace nxrt::play {

struct WinMenuHooks {
    std::function<void()> capture_bug;           // Game > Capture a bug report (the F9 bundle); null: the entry is left out
    std::function<void()> quit;                  // Game > Exit (saves pending settings, ends the process)
    std::function<bool(std::string*)> save_now;  // Game > Save settings now: write the settings in force (error text on false)
    std::function<bool(std::string*)> reload;    // Game > Reload: read the file again and apply it
    std::function<void(bool)> set_debug_overlay; // Debug > In-game debug overlay
    std::function<bool()> debug_overlay;
    std::function<std::string()> debug_report;   // Debug info window, the clipboard report
    std::function<void()> menu_opened;           // a drop-down or the system menu opened (the mouse is let go)
    // The library's Settings page on a tab (0 graphics, 1 display, 2 controller, 3 keyboard and mouse, 4 audio). Set: Game > Settings... exists
    // and Input > Gamepad... / Keyboard and mouse bindings... open the library instead of the native dialogs. Returns false when it could not start.
    std::function<bool(int tab)> open_library;
    std::string bug_key = "F9", fps_key = "F2";
    unsigned display_options = 0;                // play_game.h kDisplayOpt*
    bool upscaler_configured = false;
};

// Puts the bar on the window (hwnd: the HWND, sdl_window: the SDL_Window*) and starts handling its commands. Main thread (the one that pumps
// the window). False when there is no window or the bar could not be made; the game runs on without it.
bool win_menu_install(void* hwnd, void* sdl_window, UiHost* host, const WinMenuHooks& hooks);
// Main thread, every frame boundary: the bar is shown in windowed mode and taken off in fullscreen.
void win_menu_frame();
bool win_menu_installed();
bool win_menu_attached();                        // the bar is on the window now

}  // namespace nxrt::play
