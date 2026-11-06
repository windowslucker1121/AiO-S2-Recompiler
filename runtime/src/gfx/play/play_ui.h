// SPDX-License-Identifier: GPL-3.0-or-later
// Play mode: the F1 menu (one window of the overlay): Input, Graphics, Audio, Debug and About, plus the game's own page. The page only talks to
// the game through UiHost, so it can be tested and developed without a game running. It runs inside ImGui on the presenting thread; UiHost
// implementations are thread safe. docs: runtime/docs/MENU.md.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "play_engine.h"
#include "play_types.h"

namespace nxrt::play {

// One open gamepad as the Input page shows it: what it is and what is pressed on it right now (before the mapping).
struct PadLive {
    std::string name;
    int kind = 0;                      // input::PadKind: 0 other, 1 Xbox, 2 PlayStation, 3 Switch
    bool gyro = false, rumble = false;
    uint32_t sources = 0;              // the PadSrc bits pressed (pad_map.h pad_sources_pressed)
    float lx = 0, ly = 0, rx = 0, ry = 0;   // the sticks, -1..1, y up = +, before the dead zone
    float lt = 0, rt = 0;              // the triggers, 0..1
};

// A restart-only runtime key of [config] (play_types.h) as it stands now.
struct ConfigKeyState {
    std::string now;                   // the value in force in this run ("" when unset: the code's default applies)
    std::string from;                  // where it came from: "env", "ini", "controls.ini", "game" (the game module's default) or "" (not set)
};

struct LiveStatus {
    Ctx ctx = Ctx::Menu;               // the context the engine is in now
    const char* aim = "";              // why the mouse aim is on or off ("aim on", "locked on", "not in game" ...)
    bool mouse_captured = false;       // the pointer is captured (relative mode)
    int pads = 0;                      // open gamepads
    std::string pad_name;              // the first one
    bool pad_gyro = false, pad_rumble = false;
    const char* prompt_now = "";       // the button prompt style shown right now ("xbox" when Auto follows an Xbox pad); the Controller tab shows it under Auto
    double fps = 0;                    // presented frames per second (measured over the last second)
    const char* present_mode = "";     // "fifo" "mailbox" "immediate" ...
    const char* window_mode = "";      // "windowed" "borderless" "exclusive" (what the window is, not what the settings ask)
    float refresh_hz = 0;              // refresh rate of the display the window is on
    int width = 0, height = 0;         // window size in pixels
    EngineOut out;                     // what the keyboard and mouse press on the Switch pad right now
    double pitch_deg = 0;              // the aim's pitch
    uint64_t input_calls = 0;          // the game's per-tick player-input calls seen (GameHost::input_tick; 0: the game has not reached gameplay)
    std::vector<PadLive> pad_list;     // every open pad, in the platform's order
    bool docked = true;                // what the game is told now (docked or handheld)
    bool upscaler_configured = false;  // the game module names an upscaler replacement ([nvn] upscaler_replace)
};

class UiHost {
public:
    virtual ~UiHost() = default;
    virtual Settings settings() const = 0;               // the settings in force (a copy)
    virtual Settings factory() const = 0;                // the factory settings
    // Queues new settings: the game's main thread applies them at its next frame (bindings, sensitivity, window mode, vsync, frame cap)
    // and writes them to the settings file. Cheap to call; the page calls it when the user changes something (not every frame).
    virtual void apply(const Settings& s) = 0;
    virtual LiveStatus status() const = 0;
    virtual std::string settings_path() const = 0;
    // Rebinding: after begin_capture() the next key press, mouse button or wheel notch does not reach the game or the page; it is returned
    // by capture_result() instead. Escape cancels (capturing() turns false, no result).
    virtual void begin_capture() = 0;
    virtual bool capturing() const = 0;
    virtual bool capture_result(InputId* out) = 0;       // true once per captured input
    virtual void cancel_capture() = 0;
    // An input the page must not let the player bind (the key that opens this menu). The page keeps waiting for another one.
    virtual bool reserved_key(const InputId& id) const { (void)id; return false; }

    // Press-to-rebind for pads: after begin_pad_capture() the next button press on any pad (or a trigger pulled past the threshold) reaches
    // neither the game nor the menu; pad_capture_result() returns its source once. The capture ends by itself after 8 seconds without a press.
    virtual void begin_pad_capture() {}
    virtual bool pad_capturing() const { return false; }
    virtual bool pad_capture_result(PadSrc* out) { (void)out; return false; }
    virtual void cancel_pad_capture() {}
    virtual void test_rumble() {}                        // half a second of rumble on every pad
    // The Debug page's live part (gfx::draw_debug_page with the game's device and renderer). Nothing without a renderer.
    virtual void draw_debug() {}
    // A [config] key ("am.operation_mode") as it is in force in this run.
    virtual ConfigKeyState config_key(const std::string& name) const { (void)name; return {}; }
    // Tells the game it is docked or handheld right now (the console's operation-mode change notice). Experimental: what the game does with it is its
    // business; the setting in [config] is what the next start uses.
    virtual void set_docked_now(bool docked) { (void)docked; }
};

// Draws the menu: pages Input (gamepad, keyboard and mouse, Switch layout, live view), Graphics (display, picture, advanced), Audio, Debug and About,
// plus a Game page when the registered game has tweaks of its own. Call between ImGui::Begin / End of the window. Everything is laid out in units
// of the font size, so it follows the overlay's UI scale (overlay.cpp). Keeps its editing state in a function-local static (one page per process).
void draw_settings_page(UiHost& host);

// The [config] keys the menu offers (name, what it means, the code's default). For the docs and the tests.
struct ConfigOption {
    const char* name;                  // "gfx.device_diagnostics"
    const char* label;
    const char* def;                   // the value the runtime uses when nothing sets it
};
const std::vector<ConfigOption>& menu_config_options();

// The About page (play_ui_about.cpp): version, build and the folders and files in use. Draws into the current window.
void draw_about_page(const std::string& settings_path);

}  // namespace nxrt::play
