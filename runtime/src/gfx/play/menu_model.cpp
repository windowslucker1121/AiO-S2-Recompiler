// SPDX-License-Identifier: GPL-3.0-or-later
// The menu bar as data: see menu_model.h.
#include "menu_model.h"

#include <cmath>
#include <cstdio>

#include "play_game.h"

namespace nxrt::play {
namespace {

std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}
bool truthy(const std::string& v) {
    const std::string l = lower(v);
    return l == "1" || l == "true" || l == "yes" || l == "on";
}

// Builds the entries with consecutive ids.
struct Builder {
    int next = kMenuFirstId;

    MenuItem sub(const char* label, std::vector<MenuItem> children) {
        MenuItem m;
        m.kind = MenuItem::Kind::Submenu;
        m.label = label;
        m.children = std::move(children);
        return m;
    }
    MenuItem sep() {
        MenuItem m;
        m.kind = MenuItem::Kind::Separator;
        return m;
    }
    MenuItem cmd(const std::string& label, MenuAction a) {
        MenuItem m;
        m.kind = MenuItem::Kind::Command;
        m.label = label;
        m.id = next++;
        m.action = a;
        return m;
    }
    // A check box over a bool of the settings.
    MenuItem check(const std::string& label, std::function<bool&(Settings&)> field, bool next_start = false) {
        MenuItem m;
        m.kind = MenuItem::Kind::Check;
        m.label = label;
        m.id = next++;
        m.next_start = next_start;
        m.checked = [field](const Settings& s, const LiveStatus&) { return field(const_cast<Settings&>(s)); };
        m.set = [field](Settings& s) {
            bool& b = field(s);
            b = !b;
            return true;
        };
        return m;
    }
    // One choice of a group: checked when `is` holds, `to` makes it hold.
    MenuItem radio(const std::string& label, std::function<bool(const Settings&)> is, std::function<void(Settings&)> to, bool next_start = false) {
        MenuItem m;
        m.kind = MenuItem::Kind::Radio;
        m.label = label;
        m.id = next++;
        m.next_start = next_start;
        m.checked = [is](const Settings& s, const LiveStatus&) { return is(s); };
        m.set = [is, to](Settings& s) {
            if (is(s)) return false;
            to(s);
            return true;
        };
        return m;
    }
    // A boolean [config] key.
    MenuItem config_check(const std::string& label, const char* name, const char* def) {
        MenuItem m;
        m.kind = MenuItem::Kind::Check;
        m.label = label;
        m.id = next++;
        m.next_start = true;
        const std::string n = name, d = def;
        m.checked = [n, d](const Settings& s, const LiveStatus&) { return truthy(config_value(s, n, d)); };
        m.set = [n, d](Settings& s) { return config_store(s, n, truthy(config_value(s, n, d)) ? "0" : "1", d, true); };
        return m;
    }
    // One value of a [config] key.
    MenuItem config_radio(const std::string& label, const char* name, const char* def, const char* value) {
        const std::string n = name, d = def, v = value;
        return radio(label, [n, d, v](const Settings& s) { return lower(config_value(s, n, d)) == lower(v); },
                     [n, d, v](Settings& s) { config_store(s, n, v, d, false); }, true);
    }
};

std::string fps_label(int fps) {
    if (fps < 0) return "&Refresh rate of the display";
    if (fps == 0) return "&Unlimited";
    return std::to_string(fps) + " fps";
}

}  // namespace

std::string config_value(const Settings& s, const std::string& name, const std::string& def) {
    const auto it = s.config.find(name);
    return it == s.config.end() ? def : it->second;
}

bool config_store(Settings& s, const std::string& name, const std::string& value, const std::string& def, bool is_bool) {
    const bool is_def = is_bool ? truthy(value) == truthy(def) : lower(value) == lower(def);
    const auto it = s.config.find(name);
    if (is_def) {
        if (it == s.config.end()) return false;
        s.config.erase(it);
        return true;
    }
    if (it != s.config.end() && it->second == value) return false;
    s.config[name] = value;
    return true;
}

std::vector<MenuItem> build_menu(const MenuOptions& o) {
    Builder b;
    std::vector<MenuItem> top;

    // ---- Game
    {
        std::vector<MenuItem> v;
        v.push_back(b.cmd("&Save settings now", MenuAction::SaveSettings));
        v.push_back(b.cmd("&Reload settings from the file", MenuAction::ReloadSettings));
        v.push_back(b.cmd("&Open the settings file", MenuAction::OpenSettingsFile));
        v.push_back(b.cmd("Show the settings &folder", MenuAction::OpenSettingsFolder));
        v.push_back(b.sep());
        if (o.bug_capture) v.push_back(b.cmd("&Capture a bug report\t" + o.bug_key, MenuAction::CaptureBug));
        v.push_back(b.sep());
        v.push_back(b.cmd("E&xit", MenuAction::Exit));
        top.push_back(b.sub("&Game", std::move(v)));
    }

    // ---- Input
    {
        std::vector<MenuItem> v;
        v.push_back(b.cmd("&Gamepad...", MenuAction::GamepadDialog));
        v.push_back(b.cmd("&Keyboard and mouse bindings...", MenuAction::KeyboardDialog));
        v.push_back(b.sep());
        v.push_back(b.check("&Rumble", [](Settings& s) -> bool& { return s.pad.rumble; }));
        v.push_back(b.cmd("&Test rumble", MenuAction::TestRumble));
        v.push_back(b.check("G&yro (motion sensor)", [](Settings& s) -> bool& { return s.pad.gyro; }));
        v.push_back(b.sub("Face &buttons", {
            b.radio("By &label (Xbox A = Switch A)", [](const Settings& s) { return s.pad.label_abxy; }, [](Settings& s) { s.pad.label_abxy = true; }),
            b.radio("By &position (bottom button = Switch B)", [](const Settings& s) { return !s.pad.label_abxy; },
                    [](Settings& s) { s.pad.label_abxy = false; }),
        }));
        static const char* const kPrompt[] = {"&Switch (the game's own)", "&Xbox", "&PlayStation", "&Keyboard and mouse", "&Auto (the device used last)"};
        std::vector<MenuItem> prompts;
        for (int i = 0; i < kPromptStyleCount; ++i) {
            const PromptStyle ps = static_cast<PromptStyle>(i);
            prompts.push_back(b.radio(kPrompt[i], [ps](const Settings& s) { return s.prompts.style == ps; }, [ps](Settings& s) { s.prompts.style = ps; }));
        }
        v.push_back(b.sub("Button &prompts", std::move(prompts)));
        top.push_back(b.sub("&Input", std::move(v)));
    }

    // ---- Graphics
    {
        std::vector<MenuItem> v;
        static const char* const kModes[][2] = {{"windowed", "&Windowed"}, {"borderless", "&Borderless fullscreen"}, {"exclusive", "&Exclusive fullscreen"}};
        std::vector<MenuItem> modes;
        for (const auto& m : kModes) {
            const std::string key = m[0];
            modes.push_back(b.radio(std::string(m[1]) + (key == "windowed" ? "" : "\tAlt+Enter"),
                                    [key](const Settings& s) { return s.display.window_mode == key; }, [key](Settings& s) { s.display.window_mode = key; }));
        }
        v.push_back(b.sub("&Window mode", std::move(modes)));
        static const int kSizes[][2] = {{1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3840, 2160}};
        std::vector<MenuItem> sizes;
        for (const auto& z : kSizes) {
            const int w = z[0], h = z[1];
            sizes.push_back(b.radio(std::to_string(w) + " x " + std::to_string(h), [w, h](const Settings& s) { return s.display.width == w && s.display.height == h; },
                                    [w, h](Settings& s) {
                                        s.display.width = w;
                                        s.display.height = h;
                                    }));
        }
        v.push_back(b.sub("Window &size", std::move(sizes)));
        v.push_back(b.check("&Keep the 16:9 picture (letterbox)", [](Settings& s) -> bool& { return s.display.keep_aspect; }));
        v.push_back(b.sep());
        v.push_back(b.check("&Vsync", [](Settings& s) -> bool& { return s.display.vsync; }));
        std::vector<MenuItem> caps;
        for (int fps : {-1, 0, 30, 60, 120, 144, 165, 240}) {
            caps.push_back(b.radio(fps_label(fps), [fps](const Settings& s) { return s.display.fps_cap == fps; }, [fps](Settings& s) { s.display.fps_cap = fps; }));
        }
        v.push_back(b.sub("&Frame limit", std::move(caps)));
        v.push_back(b.sep());
        v.push_back(b.sub("&Console mode", {
            b.config_radio("&Docked (TV)", "am.operation_mode", "docked", "docked"),
            b.config_radio("&Handheld (lighter)", "am.operation_mode", "docked", "handheld"),
            b.sep(),
            b.cmd("Tell the game &docked now (experimental)", MenuAction::DockNow),
            b.cmd("Tell the game h&andheld now (experimental)", MenuAction::HandheldNow),
        }));
        std::vector<MenuItem> aniso;
        for (int a : {0, 2, 4, 8, 16}) {
            aniso.push_back(b.radio(a == 0 ? std::string("&Game default") : std::to_string(a) + "x", [a](const Settings& s) { return s.display.anisotropy == a; },
                                    [a](Settings& s) { s.display.anisotropy = a; }));
        }
        v.push_back(b.sub("Texture &anisotropy", std::move(aniso)));
        if (o.upscaler_configured)
            v.push_back(b.check("&Replace the game's upscaler (stopgap)", [](Settings& s) -> bool& { return s.display.upscaler_replace; }));
        v.push_back(b.sub("&Upscaler (window larger than the picture)", {
            b.radio("&Off (bilinear)", [](const Settings& s) { return s.extras.upscale.mode == extras::UpscaleMode::Off; },
                    [](Settings& s) { s.extras.upscale.mode = extras::UpscaleMode::Off; }),
            b.radio("&FSR 1", [](const Settings& s) { return s.extras.upscale.mode == extras::UpscaleMode::Fsr1; },
                    [](Settings& s) { s.extras.upscale.mode = extras::UpscaleMode::Fsr1; }),
            b.radio("&NIS", [](const Settings& s) { return s.extras.upscale.mode == extras::UpscaleMode::Nis; },
                    [](Settings& s) { s.extras.upscale.mode = extras::UpscaleMode::Nis; }),
        }));
        v.push_back(b.sub("C&olour", {
            b.radio("&Neutral (the game's own)", [](const Settings& s) { return s.extras.picture.neutral(); },
                    [](Settings& s) { extras::picture_preset("neutral", &s.extras.picture); }),
            b.radio("&Vivid", [](const Settings& s) {
                        extras::PictureSettings p;
                        extras::picture_preset("vivid", &p);
                        const extras::PictureSettings& c = s.extras.picture;
                        return std::fabs(c.contrast - p.contrast) < 1e-6 && std::fabs(c.saturation - p.saturation) < 1e-6 && std::fabs(c.black - p.black) < 1e-6 &&
                               std::fabs(c.gamma - p.gamma) < 1e-6;
                    },
                    [](Settings& s) { extras::picture_preset("vivid", &s.extras.picture); }),
        }));
        v.push_back(b.sep());
        v.push_back(b.cmd("&More graphics settings...", MenuAction::GraphicsDialog));
        top.push_back(b.sub("G&raphics", std::move(v)));
    }

    // ---- Audio
    {
        std::vector<MenuItem> vol;
        for (int pct : {100, 75, 50, 25, 0}) {
            vol.push_back(b.radio(pct ? std::to_string(pct) + " %" : std::string("&Mute"),
                                  [pct](const Settings& s) { return std::lround(s.audio.volume * 100.0) == pct; },
                                  [pct](Settings& s) { s.audio.volume = pct / 100.0; }));
        }
        std::vector<MenuItem> v;
        v.push_back(b.sub("&Volume", std::move(vol)));
        v.push_back(b.check("Mute when the window is &not in focus", [](Settings& s) -> bool& { return s.audio.mute_unfocused; }));
        top.push_back(b.sub("&Audio", std::move(v)));
    }

    // ---- Debug
    {
        std::vector<MenuItem> v;
        v.push_back(b.cmd("Debug &info...", MenuAction::DebugInfo));
        v.push_back(b.cmd("&Log...", MenuAction::LogWindow));
        v.push_back(b.cmd("Open the log &file", MenuAction::OpenLogFile));
        v.push_back(b.cmd("Show the log f&older", MenuAction::OpenLogFolder));
        v.push_back(b.cmd("&Copy a debug report to the clipboard", MenuAction::CopyDebugReport));
        v.push_back(b.sep());
        v.push_back(b.check("&Frame rate counter\t" + o.fps_key, [](Settings& s) -> bool& { return s.fps.show; }));
        static const char* const kCorner[] = {"Top &left", "Top &right", "&Bottom left", "Bottom r&ight"};
        std::vector<MenuItem> corners;
        for (int i = 0; i < 4; ++i) {
            const Corner c = static_cast<Corner>(i);
            corners.push_back(b.radio(kCorner[i], [c](const Settings& s) { return s.fps.corner == c; }, [c](Settings& s) { s.fps.corner = c; }));
        }
        v.push_back(b.sub("Frame rate counter &corner", std::move(corners)));
        {
            MenuItem m = b.cmd("In-game &debug overlay", MenuAction::DebugOverlay);
            m.kind = MenuItem::Kind::Check;   // checked from the live state (the host answers)
            v.push_back(std::move(m));
        }
        v.push_back(b.sep());
        v.push_back(b.sub("Diagnostics &switches", {
            b.config_check("GPU &checkpoints (NVIDIA: names the draw of a device loss)", "gfx.device_diagnostics", "false"),
            b.config_check("Driver &fault report after a device loss", "gfx.device_fault", "true"),
            b.config_check("&End the game after a device loss", "gfx.exit_on_device_lost", "true"),
            b.config_check("&Robust buffer and image access (slow, rebuilds pipelines)", "gfx.robust_access", "false"),
            b.config_check("Skip compute dispatches with &unmapped buffers", "nvn.skip_unmapped_dispatch", "true"),
            b.config_check("Skip &every compute dispatch", "nvn.debug_skip_compute", "false"),
            b.config_check("&Log the frame rate every second", "gfx.log_fps", "false"),
        }));
        v.push_back(b.sub("&Hitch threshold", {
            b.config_radio("&Off", "diag.hitch_ms", "50", "0"),
            b.config_radio("&33 ms", "diag.hitch_ms", "50", "33"),
            b.config_radio("&50 ms", "diag.hitch_ms", "50", "50"),
            b.config_radio("&100 ms", "diag.hitch_ms", "50", "100"),
            b.config_radio("&250 ms", "diag.hitch_ms", "50", "250"),
        }));
        top.push_back(b.sub("&Debug", std::move(v)));
    }

    // ---- Help
    top.push_back(b.sub("&Help", {
        b.cmd("&Keys and controls", MenuAction::MenuHelp),
        b.cmd("&About", MenuAction::About),
    }));
    (void)o.display_options;
    return top;
}

void menu_for_each(const std::vector<MenuItem>& menu, const std::function<void(const MenuItem&)>& fn) {
    for (const MenuItem& m : menu) {
        if (m.kind == MenuItem::Kind::Submenu) menu_for_each(m.children, fn);
        else if (m.id) fn(m);
    }
}

const MenuItem* menu_find(const std::vector<MenuItem>& menu, int id) {
    for (const MenuItem& m : menu) {
        if (m.kind == MenuItem::Kind::Submenu) {
            if (const MenuItem* f = menu_find(m.children, id)) return f;
        } else if (m.id == id) {
            return &m;
        }
    }
    return nullptr;
}

}  // namespace nxrt::play
