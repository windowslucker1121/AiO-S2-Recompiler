// SPDX-License-Identifier: GPL-3.0-or-later
// Start-up and menu tweaks (S46, docs/S46.md): the `[tweaks]` keys of controls.ini that skip the opening cutscene, skip the title's
// intro movie and speed up the front end. All are off by default, so the game starts and plays as it always did. The game module does the
// work (the game module); this file is only the settings: the struct, the reader and the writer of its keys.
//
//   [tweaks] skip_intro = 0        1: the opening cutscene of a new game (the gunship landing on the Frigate) is skipped through the game's own
//                                  cutscene skip, without a key press
//            skip_boot_logos = 0   1: the title's intro movie is skipped, the title screen ("press start") comes up at once
//            fast_menus = 0        1: the front end (title, main menu, save slots, options) runs its animations, fades and waits faster
//            menu_speed = 2        how much faster with fast_menus = 1: 1..4 (1 = normal speed)
//            language = auto       the game's language: auto (the Windows display language) or a name such as French; next start
#pragma once
#include <string>

namespace nxrt::play {

struct FastbootSettings {
    bool skip_intro = false;        // [tweaks] skip_intro
    bool skip_boot_logos = false;   // [tweaks] skip_boot_logos
    bool fast_menus = false;        // [tweaks] fast_menus
    int menu_speed = 2;             // [tweaks] menu_speed, 1..4; read only while fast_menus is on
    std::string language;           // [tweaks] language: a language name (AmericanEnglish, French, ...) or "" = auto (the Windows language).
                                    // Read by the settings service at start-up (services/common.cpp); a change applies at the next start.
};

// One `key = value` of the [tweaks] section. Returns false with *msg set for a bad value of one of these keys. *known is false (and the
// return true) for a key that is not one of them, so another tweak's reader can take it.
bool fastboot_apply(const std::string& key, const std::string& value, FastbootSettings* s, bool* known, std::string* msg);
// Appends this file's lines of the [tweaks] section ("key = value\n" each, no section header).
void fastboot_format(const FastbootSettings& s, std::string* out);
// The factor the front end's clock runs at: 1 with fast_menus off, else menu_speed.
double fastboot_menu_rate(const FastbootSettings& s);
// The F1 page's rows (Extras tab; play_ui_fastboot.cpp, Dear ImGui): true when the user changed a value.
bool fastboot_ui(FastbootSettings& f);

}  // namespace nxrt::play
