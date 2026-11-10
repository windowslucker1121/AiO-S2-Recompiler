// SPDX-License-Identifier: GPL-3.0-or-later
// The library's Settings page: one game's graphics, display, controller, keyboard and audio settings. It edits that game's controls.ini
// (in its game folder, the file the game's own menu bar writes too) in place: only the keys the player changes are written, comments and
// every other line stay. Game independent: the keys are the runtime's ([display], [upscale], [hdr], [fps], [pad], [padmap], [audio],
// [bindings.*]; runtime/docs/CONFIG.md). A file without [bindings.*] sections gets the game's factory bindings from the game itself.
#pragma once
#include <windows.h>

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace setup::settings {

// ---- an ini file edited in place --------------------------------------------------------------------------------------------------
class Ini {
public:
    bool load(const std::wstring& path);         // false: no file (an empty Ini)
    bool save(const std::wstring& path) const;
    bool has(const std::string& section, const std::string& key) const;
    std::string get(const std::string& section, const std::string& key, const std::string& def = {}) const;
    void set(const std::string& section, const std::string& key, const std::string& value);
    std::vector<std::string> sections() const;   // in file order
    std::vector<std::pair<std::string, std::string>> entries(const std::string& section) const;
    // Replaces the key lines of a section (comments stay where they were); creates the section when it is missing.
    void replace_entries(const std::string& section, const std::vector<std::pair<std::string, std::string>>& e);
    void remove_section(const std::string& section);
    std::string text() const;

private:
    int header(const std::string& section) const;   // line of "[section]", or -1
    int end_of(int header) const;                   // first line after the section
    std::vector<std::string> lines_;
};

// ---- the panel ------------------------------------------------------------------------------------------------------------------------
struct Fonts {
    HFONT font = nullptr, bold = nullptr, little = nullptr, title = nullptr;   // ("small" is a macro of the Windows headers)
    int dpi = 96;
};
bool register_class(HINSTANCE inst);
HWND create(HWND parent, HINSTANCE inst, int id);
void set_fonts(HWND panel, const Fonts& f);
// The game whose settings the panel shows. controls_ini empty: not available yet, why_not says why (shown instead of the settings).
void set_game(HWND panel, const std::wstring& title, const std::wstring& controls_ini, const std::wstring& why_not);

// DLSS availability (setup::dlss_state): 2 shows the DLSS/DLAA entries; 1 hides them and offers a "Download DLSS" button that calls
// download; 0 hides them. Call before set_game.
void set_dlss(HWND panel, int state, std::function<void()> download);

}  // namespace setup::settings
