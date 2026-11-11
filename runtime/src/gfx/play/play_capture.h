// SPDX-License-Identifier: GPL-3.0-or-later
// Who holds the mouse: the capture state machine of the game window (runtime/docs/MENU.md, "The mouse"). Pure: no SDL, no Win32, so the tests
// drive it with plain inputs; play.cpp feeds it the window's events and asks it every frame whether the relative mouse mode should be on.
//
// [mouse] capture (controls.ini):
//   click (default)  the mouse is captured only after a click into the picture while the game is playing; that click never reaches the game.
//                    It is released on focus loss, minimizing, the overlay opening, the menu bar opening (Alt + letter, Alt+Space), the release
//                    key ([mouse] release_key, default Left Alt pressed and released alone) and a library or dialog taking over. After a release
//                    nothing captures again by itself (not even focus coming back): only the next click does.
//   auto             the older behaviour: captured whenever the window has the focus and the game is playing.
//   off              never captured.
// While captured, the relative mode is held back (not released) when the game leaves play for a menu of its own, while a rebind waits for an input,
// or while the Joy-Con 2 mouse wants the pointer; it comes back with play.
#pragma once
#include <cstdint>

namespace nxrt::play {

enum class CaptureMode : uint8_t { Click = 0, Auto, Off };
const char* capture_mode_name(CaptureMode m);              // "click" "auto" "off"
bool parse_capture_mode(const char* text, CaptureMode* out);   // case-insensitive

enum class CaptureHint : uint8_t { None = 0, Captured, Released };

class MouseCapture {
public:
    struct Frame {
        bool focused = false;
        bool minimized = false;
        bool overlay = false;        // the in-game menu (overlay panel) is up
        bool playing = false;        // the game is in play (not under one of its menus)
        bool held_back = false;      // a rebind waits for an input, or the Joy-Con 2 mouse wants the pointer
    };

    void set_mode(CaptureMode m);
    CaptureMode mode() const { return mode_; }
    void set_release_key(int scancode) { release_sc_ = scancode; }   // 0: no release key
    int release_key() const { return release_sc_; }

    // A mouse button went down in the window. True when this press captured the mouse: the press and its release must not reach the game
    // (eat_release answers for the release).
    bool on_button_down(int button, const Frame& f);
    bool eat_release(int button);    // true once for the release of a press on_button_down took
    // A key. True when it was the release key let go with nothing else pressed in between, and the mouse was captured (now released).
    bool on_key(int scancode, bool down, bool repeat);
    // Any release from outside the per-frame state: the menu bar or the system menu opened, a library window or a settings dialog took over.
    void release(const char* why);
    void on_focus_lost();

    // Once per frame: whether the relative mouse mode should be on now. Also clears the capture on focus loss, minimizing and the overlay.
    bool want_relative(const Frame& f);
    bool captured() const { return captured_; }    // the player's capture (not held back)
    const char* last_release() const { return why_; }

    // The hint to show now, once per change: Captured right after a click captured; Released when the mouse was let go (or the window came
    // back to the front) while the game is playing in click mode. `mouse_play`: the game takes the mouse in play (it turns the camera or aims).
    CaptureHint take_hint(const Frame& f, bool mouse_play);

private:
    CaptureMode mode_ = CaptureMode::Click;
    int release_sc_ = 226;           // SDL_SCANCODE_LALT
    bool captured_ = false;
    bool key_alone_ = false;         // the release key is down and nothing else was pressed since
    uint32_t eat_up_ = 0;            // buttons whose release is swallowed (bit per button)
    bool hint_captured_ = false;     // a Captured hint is owed
    bool released_hint_shown_ = false;
    bool was_focused_ = false;
    const char* why_ = "";
};

}  // namespace nxrt::play
