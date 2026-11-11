// SPDX-License-Identifier: GPL-3.0-or-later
// The game window's menu bar as data (runtime/docs/MENU.md): every entry with its label, whether it shows a check mark, and what it changes in
// the play settings or which action it starts. Pure (no Win32): win_menu.cpp builds the native HMENU from it, and the tests check that every
// settings entry changes what it says and shows its own state. Game independent; a game's options appear only where play_game.h says it has
// them.
#pragma once
#include <functional>
#include <string>
#include <vector>

#include "play_types.h"
#include "play_ui.h"

namespace nxrt::play {

// What a command entry does when it is not a settings change.
enum class MenuAction {
    None = 0,
    SaveSettings, ReloadSettings, OpenSettingsFile, OpenSettingsFolder, CaptureBug, Exit,
    GamepadDialog, KeyboardDialog, TestRumble,
    GraphicsDialog, DockNow, HandheldNow,
    DebugInfo, LogWindow, OpenLogFile, OpenLogFolder, CopyDebugReport, DebugOverlay, OverlayMenu,
    About, MenuHelp,
    LibrarySettings,   // Game > Settings...: the library's Settings page (only when the game was started from the library)
};

struct MenuItem {
    enum class Kind { Command, Check, Radio, Separator, Submenu };
    Kind kind = Kind::Command;
    std::string label;                 // with '&' for the access key; "\t" then a key name to show a shortcut
    int id = 0;                        // command id (unique, >= kMenuFirstId); 0 for separators and submenus
    bool next_start = false;           // a restart-only setting: the label says "(next start)"
    // Check and radio entries: the check mark, read from the settings in force and the live state.
    std::function<bool(const Settings&, const LiveStatus&)> checked;
    // Settings entries: change the copy; false when nothing changed (the host is then not called).
    std::function<bool(Settings&)> set;
    MenuAction action = MenuAction::None;   // command entries without `set`
    std::vector<MenuItem> children;    // Submenu
};

constexpr int kMenuFirstId = 41000;

struct MenuOptions {
    unsigned display_options = 0;      // GamePlay::display_options of the registered game
    bool upscaler_configured = false;  // the game module names an upscaler replacement
    bool bug_capture = true;           // the bug capture hotkey exists
    std::string bug_key = "F9";
    std::string fps_key = "F2";
    bool library = false;              // the game was started from the library: Game > Settings... and the Input dialogs open its Settings page
};

// The top-level menus: Game, Input, Graphics, Audio, Debug, Help.
std::vector<MenuItem> build_menu(const MenuOptions& o);
// Every entry with an id, depth first.
void menu_for_each(const std::vector<MenuItem>& menu, const std::function<void(const MenuItem&)>& fn);
const MenuItem* menu_find(const std::vector<MenuItem>& menu, int id);

// The [config] rows of play_types.h: the value the settings ask for (the default when the key is absent), and storing one (the default
// removes the line). bool keys compare as booleans ("1" == "true").
std::string config_value(const Settings& s, const std::string& name, const std::string& def);
bool config_store(Settings& s, const std::string& name, const std::string& value, const std::string& def, bool is_bool);

}  // namespace nxrt::play
