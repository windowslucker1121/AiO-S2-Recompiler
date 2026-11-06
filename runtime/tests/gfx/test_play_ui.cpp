// SPDX-License-Identifier: GPL-3.0-or-later
// Play mode's F1 menu (src/gfx/play/play_ui.cpp) drawn headless: ImGui without a renderer or platform backend, a few frames at a time, against a
// fake host. No input is simulated: tabs are opened through ImGui's tab bar, and a button or check box is activated by its id
// (ImGui::ActivateItemByID, the way keyboard and gamepad navigation activate an item).
#include "test_framework.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_internal.h"
#include "nxrt/input_source.h"
#include "nxrt/overlay.h"
#include "play/pad_map.h"
#include "play/menu_model.h"
#include "play/play_ui.h"

NXTEST_GROUP(play_ui)

namespace {

namespace play = nxrt::play;
namespace hid = nxrt::hid;

// Records what the page asks of the host and answers from fields the test sets.
class FakeHost final : public play::UiHost {
public:
    play::Settings settings_ = play::default_settings();
    play::LiveStatus status_;
    std::vector<play::InputId> queued;   // capture results, handed out one per poll
    bool capturing_ = false;
    int applies = 0, begins = 0, cancels = 0;
    play::Settings last_applied;
    std::vector<play::PadSrc> pad_queued;   // pad capture results
    bool pad_capturing_ = false;
    int pad_begins = 0, rumbles = 0, debug_draws = 0, docked_calls = 0;
    std::map<std::string, play::ConfigKeyState> keys;   // config_key answers

    play::Settings settings() const override { return settings_; }
    play::Settings factory() const override { return play::default_settings(); }
    void apply(const play::Settings& s) override {
        applies++;
        last_applied = s;
        settings_ = s;   // in force, as the game's host does at its next frame
    }
    play::LiveStatus status() const override { return status_; }
    std::string settings_path() const override { return "test_settings.ini"; }
    void begin_capture() override {
        begins++;
        capturing_ = true;
    }
    bool capturing() const override { return capturing_; }
    bool capture_result(play::InputId* out) override {
        if (queued.empty()) return false;
        *out = queued.front();
        queued.erase(queued.begin());
        capturing_ = false;
        return true;
    }
    void cancel_capture() override {
        cancels++;
        capturing_ = false;
    }
    void begin_pad_capture() override {
        pad_begins++;
        pad_capturing_ = true;
    }
    bool pad_capturing() const override { return pad_capturing_; }
    bool pad_capture_result(play::PadSrc* out) override {
        if (pad_queued.empty()) return false;
        *out = pad_queued.front();
        pad_queued.erase(pad_queued.begin());
        pad_capturing_ = false;
        return true;
    }
    void cancel_pad_capture() override { pad_capturing_ = false; }
    void test_rumble() override { rumbles++; }
    void draw_debug() override {
        debug_draws++;
        nxrt::gfx::draw_debug_page(nxrt::gfx::Overlay::Context{});   // no device, renderer or limiter: those parts are left out
    }
    play::ConfigKeyState config_key(const std::string& name) const override {
        const auto it = keys.find(name);
        return it == keys.end() ? play::ConfigKeyState{} : it->second;
    }
    void set_docked_now(bool) override { docked_calls++; }
};

// One ImGui context without a backend. ImGui 1.92 without RendererHasTextures builds the font atlas on request, so it is built here.
struct Headless {
    Headless() {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(1280, 720);
        io.DeltaTime = 1.0f / 60.0f;
        io.IniFilename = nullptr;   // no imgui.ini in the working directory
        io.LogFilename = nullptr;
        unsigned char* px = nullptr;
        int w = 0, h = 0;
        io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    }
    ~Headless() { ImGui::DestroyContext(); }
};

// Draws n frames of the Controls window with the settings page inside it.
void run_frames(play::UiHost& host, int n) {
    for (int i = 0; i < n; ++i) {
        ImGui::NewFrame();
        ImGui::SetNextWindowSize(ImVec2(640, 720), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Controls")) play::draw_settings_page(host);
        ImGui::End();
        ImGui::Render();
    }
}

// The page has two levels of tabs: the pages (Input, Graphics, Audio, Debug, About, and the game's own) in the "settings_pages" bar, and inside
// the Input page the "input_tabs" bar (Gamepad, Keyboard and mouse, Switch layout, Live view), inside Graphics the "graphics_tabs" bar (Display,
// Picture, Advanced). A tab bar's id is its name hashed with the top of the ID stack where it is drawn: the window for the pages, the open page's
// tab for a nested bar. A tab's id is its label hashed with the tab bar's id as the seed (the tab bar is on the ID stack while its tabs are drawn).
ImGuiID pages_bar_id() {
    ImGuiWindow* win = ImGui::FindWindowByName("Controls");
    return win ? win->GetID("settings_pages") : 0;
}
ImGuiID tab_id(ImGuiID bar, const char* tab) { return bar ? ImHashStr(tab, 0, bar) : 0; }
ImGuiID nested_bar_id(const char* page, const char* bar) {
    const ImGuiID pages = pages_bar_id();
    return pages ? ImHashStr(bar, 0, tab_id(pages, page)) : 0;
}
ImGuiID input_bar_id() { return nested_bar_id("Input", "input_tabs"); }
ImGuiID graphics_bar_id() { return nested_bar_id("Graphics", "graphics_tabs"); }

// The tab bar with this id, or nullptr (before the first frame, or while its page is not the open one).
ImGuiTabBar* find_bar(ImGuiID id) { return id ? ImGui::TabBarFindByID(id) : nullptr; }

// Asks the tab bar to open the named tab at the next frame. TabBarQueueFocus by name needs a current window, which there is not between frames.
void open_tab(ImGuiID bar_id, const char* name) {
    if (ImGuiTabBar* bar = find_bar(bar_id)) bar->NextSelectedTabId = ImHashStr(name, 0, bar_id);
}

// Returns true when the open tab of the bar has this name. Checked through the tab bar's selected id.
bool tab_open(ImGuiID bar_id, const char* name) {
    ImGuiTabBar* bar = find_bar(bar_id);
    if (!bar) return false;
    for (int i = 0; i < bar->Tabs.Size; ++i) {
        if (bar->Tabs[i].ID == bar->SelectedTabId) return std::strcmp(ImGui::TabBarGetTabName(bar, &bar->Tabs[i]), name) == 0;
    }
    return false;
}

// Opens one tab of a bar for two frames and checks that it is the open one.
bool visit_tab(play::UiHost& host, ImGuiID bar_id, const char* name) {
    open_tab(bar_id, name);
    run_frames(host, 2);
    if (tab_open(bar_id, name)) return true;
    std::printf("    tab did not open: %s\n", name);
    return false;
}

// Draws every tab: one frame so the tabs exist, then each page is opened in turn with two frames, and checked to be the open one; the Controls
// page's own tabs are opened in turn the same way. Returns false when a tab did not open.
bool draw_all_tabs(play::UiHost& host) {
    static const char* const kPages[] = {"Input", "Graphics", "Audio", "Debug", "About"};
    static const char* const kInputTabs[] = {"Gamepad", "Keyboard and mouse", "Switch layout", "Live view"};
    static const char* const kGraphicsTabs[] = {"Display", "Picture", "Advanced"};
    bool ok = true;
    run_frames(host, 1);
    for (const char* t : kPages) {
        if (!visit_tab(host, pages_bar_id(), t)) {
            ok = false;
            continue;
        }
        if (std::strcmp(t, "Input") == 0)
            for (const char* c : kInputTabs)
                if (!visit_tab(host, input_bar_id(), c)) ok = false;
        if (std::strcmp(t, "Graphics") == 0)
            for (const char* c : kGraphicsTabs)
                if (!visit_tab(host, graphics_bar_id(), c)) ok = false;
    }
    // the game's own page, when the registered game has one
    if (ImGuiTabBar* bar = find_bar(pages_bar_id())) {
        for (int i = 0; i < bar->Tabs.Size; ++i)
            if (std::strcmp(ImGui::TabBarGetTabName(bar, &bar->Tabs[i]), "Game") == 0 && !visit_tab(host, pages_bar_id(), "Game")) ok = false;
    }
    return ok;
}

}  // namespace

NXTEST(default_settings_draw_20_frames) {
    Headless ui;
    FakeHost host;
    run_frames(host, 20);
    CHECK_EQ(host.applies, 0);   // drawing never applies by itself
    CHECK_EQ(host.begins, 0);
}

NXTEST(every_tab_draws_with_default_settings) {
    Headless ui;
    FakeHost host;
    CHECK(draw_all_tabs(host));
    CHECK_EQ(host.applies, 0);
    CHECK_EQ(host.begins, 0);
}

// The page keeps its working copy for the life of the process, so the binding counts run in a fresh process each.
NXTEST_CHILD(play_ui_empty_bindings) {
    Headless ui;
    FakeHost host;
    host.settings_.bindings.clear();
    return draw_all_tabs(host) ? 0 : 1;
}

NXTEST_CHILD(play_ui_300_bindings) {
    Headless ui;
    FakeHost host;
    host.settings_.bindings.clear();
    for (int i = 0; i < 300; ++i) {
        play::Binding b;
        b.in.kind = play::InKind::Key;
        b.in.code = 4 + i % 26;   // SDL scancodes of the letters
        b.ctx = (i % 2) ? play::Ctx::Play : play::Ctx::Menu;
        b.act = play::Act::Primary;
        b.trig = play::Trig::Tap;   // a timed trigger: the ms column is enabled
        b.ms = 200;
        host.settings_.bindings.push_back(b);
    }
    return draw_all_tabs(host) ? 0 : 1;
}

NXTEST(bindings_empty_and_300_each_in_fresh_process) {
    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    const char* kids[] = {"play_ui_empty_bindings", "play_ui_300_bindings"};
    for (const char* k : kids) {
        // cmd /c strips the outer pair of quotes, so the whole command is wrapped in one more pair.
        const std::string cmd = std::string("\"\"") + exe + "\" --child " + k + "\"";
        const int rc = std::system(cmd.c_str());
        CHECK_MSG(rc == 0, std::string("child ") + k + " returned " + std::to_string(rc));
    }
}

NXTEST(status_values_draw) {
    Headless ui;
    FakeHost host;
    host.status_.fps = 59.94;
    host.status_.pads = 1;
    host.status_.pad_name = "Test pad";
    host.status_.pad_gyro = true;
    host.status_.refresh_hz = 60.0f;
    host.status_.window_mode = "windowed";
    host.status_.present_mode = "fifo";
    host.status_.width = 1280;
    host.status_.height = 720;
    host.status_.aim = "aim on";
    host.status_.input_calls = 0;
    CHECK(draw_all_tabs(host));
    host.status_.input_calls = 5;
    host.status_.out.buttons = hid::btn(hid::kBtnA);
    host.status_.out.ly = 1.0f;
    CHECK(draw_all_tabs(host));
    CHECK_EQ(host.applies, 0);
}

NXTEST(capture_hint_and_result_are_consumed) {
    Headless ui;
    FakeHost host;
    host.begin_capture();   // the host is capturing: the page shows the hint line
    run_frames(host, 3);
    play::InputId key;
    key.kind = play::InKind::Key;
    key.code = 4;
    host.queued.push_back(key);
    run_frames(host, 1);
    CHECK(host.queued.empty());   // the page takes the result even when it did not ask for one
    CHECK_EQ(host.applies, 0);
}

// The frame rate counter is live: the game's hotkey can flip fps.show after the page has loaded. Drawing every tab after that must not
// hand anything to the host, and the page keeps drawing.
NXTEST(fps_flip_after_load_draws_without_apply) {
    Headless ui;
    FakeHost host;
    CHECK(draw_all_tabs(host));
    host.settings_.fps.show = true;
    host.settings_.fps.corner = play::Corner::BottomRight;
    CHECK(draw_all_tabs(host));
    CHECK_EQ(host.applies, 0);
    CHECK_EQ(host.begins, 0);
}

// ---- the menu's own behaviour: pad press-to-rebind, auto-apply, [config] rows, the Debug page --------------------------------------------------

namespace {
// The id of the "Rebind" button of Switch button row `row` on the Gamepad tab: the tab's id, then the table (instance 0 has the table's id), then
// PushID(row), then the label.
ImGuiID rebind_button_id(int row) {
    const ImGuiID gamepad_tab = tab_id(input_bar_id(), "Gamepad");
    const ImGuiID table = ImHashStr("pad_rebind", 0, gamepad_tab);
    const ImGuiID row_id = ImHashData(&row, sizeof row, table);
    return ImHashStr("Rebind", 0, row_id);
}
// A control drawn straight in the window (the menu's top bar).
ImGuiID window_item_id(const char* label) {
    ImGuiWindow* win = ImGui::FindWindowByName("Controls");
    return win ? win->GetID(label) : 0;
}
// Activates an item at the next frame, as navigation does, and draws two frames.
void activate(play::UiHost& host, ImGuiID id) {
    ImGui::ActivateItemByID(id);
    run_frames(host, 4);
}
}  // namespace

NXTEST(pad_rebind_by_press_swaps_and_applies_at_once) {
    Headless ui;
    FakeHost host;
    CHECK(host.settings_.menu.auto_apply);
    run_frames(host, 1);
    CHECK(visit_tab(host, pages_bar_id(), "Input"));
    CHECK(visit_tab(host, input_bar_id(), "Gamepad"));
    activate(host, rebind_button_id(0));   // row 0 is Switch A
    CHECK_EQ(host.pad_begins, 1);
    CHECK(host.pad_capturing_);
    CHECK_EQ(host.applies, 0);
    host.pad_queued.push_back(play::PadSrc::East);   // the player presses the right face button
    run_frames(host, 2);
    CHECK(!host.pad_capturing_);
    CHECK_EQ(host.applies, 1);   // Apply changes at once: handed over and saved
    const play::PadSettings& pad = host.last_applied.pad;
    CHECK(play::pad_effective_buttons(play::PadSrc::East, pad) == hid::btn(hid::kBtnA));
    CHECK(play::pad_effective_buttons(play::PadSrc::South, pad) == hid::btn(hid::kBtnB));   // the swap
    run_frames(host, 3);
    CHECK_EQ(host.applies, 1);   // nothing more
}

NXTEST(pad_capture_timeout_changes_nothing) {
    Headless ui;
    FakeHost host;
    run_frames(host, 1);
    CHECK(visit_tab(host, pages_bar_id(), "Input"));
    CHECK(visit_tab(host, input_bar_id(), "Gamepad"));
    activate(host, rebind_button_id(3));
    CHECK_EQ(host.pad_begins, 1);
    host.pad_capturing_ = false;   // the host gave up (8 s without a press)
    run_frames(host, 2);
    CHECK_EQ(host.applies, 0);
}

NXTEST(auto_apply_off_waits_for_apply_and_save) {
    Headless ui;
    FakeHost host;
    run_frames(host, 2);
    activate(host, window_item_id("Apply changes at once"));
    CHECK_EQ(host.applies, 1);   // turning it off is saved at once
    CHECK(!host.last_applied.menu.auto_apply);
    CHECK(visit_tab(host, pages_bar_id(), "Input"));
    CHECK(visit_tab(host, input_bar_id(), "Gamepad"));
    activate(host, rebind_button_id(1));
    host.pad_queued.push_back(play::PadSrc::North);
    run_frames(host, 3);
    CHECK_EQ(host.applies, 1);   // the rebind waits in the working copy
    activate(host, window_item_id("Apply and save"));
    CHECK_EQ(host.applies, 2);
    CHECK(play::pad_effective_buttons(play::PadSrc::North, host.last_applied.pad) == hid::btn(hid::kBtnB));
}

NXTEST(config_rows_and_debug_page_draw_without_applying) {
    Headless ui;
    FakeHost host;
    host.settings_.config["am.operation_mode"] = "handheld";
    host.settings_.config["gfx.device_diagnostics"] = "1";
    host.keys["am.operation_mode"] = play::ConfigKeyState{"docked", ""};                // the next start changes it
    host.keys["gfx.device_diagnostics"] = play::ConfigKeyState{"0", "ini"};            // the game ini wins
    host.keys["diag.hitch_ms"] = play::ConfigKeyState{"100", "env"};
    host.status_.docked = true;
    host.status_.upscaler_configured = true;
    play::PadLive pl;
    pl.name = "Test pad";
    pl.kind = 1;
    pl.sources = (1u << static_cast<int>(play::PadSrc::South)) | (1u << static_cast<int>(play::PadSrc::LeftTrigger));
    pl.lt = 1.0f;
    host.status_.pad_list.push_back(pl);
    CHECK(draw_all_tabs(host));
    CHECK(host.debug_draws > 0);
    CHECK_EQ(host.applies, 0);
    CHECK_EQ(host.docked_calls, 0);
    for (const play::ConfigOption& o : play::menu_config_options()) CHECK_MSG(play::config_entry_name_ok(o.name), o.name);
}

// A [config] switch on the Debug page: turning it on stores section.key = 1 (and applies at once), turning it back to the default removes the line.
NXTEST(config_switch_is_stored_and_removed_at_the_default) {
    Headless ui;
    FakeHost host;
    run_frames(host, 1);
    CHECK(visit_tab(host, pages_bar_id(), "Debug"));
    const ImGuiID debug_tab = tab_id(pages_bar_id(), "Debug");
    activate(host, ImHashStr("Diagnostics switches (next start)", 0, debug_tab));   // open the header
    const ImGuiID box = ImHashStr("GPU checkpoints (NVIDIA)", 0, ImHashStr("gfx.device_diagnostics", 0, debug_tab));
    host.settings_.display.window_mode = "borderless";   // changed elsewhere (Alt+Enter) after the page loaded: the page must not undo it
    run_frames(host, 1);
    activate(host, box);
    REQUIRE(host.applies == 1);
    CHECK(host.last_applied.display.window_mode == "borderless");
    CHECK(host.last_applied.config.count("gfx.device_diagnostics") == 1);
    CHECK(host.last_applied.config["gfx.device_diagnostics"] == "1");
    activate(host, box);
    CHECK_EQ(host.applies, 2);
    CHECK(host.last_applied.config.empty());   // false is the default: nothing in the file
}

// ---- the native menu bar's model (menu_model.h): what every entry changes and shows ----------------------------------------------------------

NXTEST(menu_model_ids_are_unique_and_findable) {
    play::MenuOptions o;
    o.upscaler_configured = true;
    const std::vector<play::MenuItem> m = play::build_menu(o);
    REQUIRE(m.size() == 6);   // Game, Input, Graphics, Audio, Debug, Help
    std::vector<int> ids;
    play::menu_for_each(m, [&](const play::MenuItem& it) { ids.push_back(it.id); });
    CHECK(ids.size() > 60);
    for (size_t i = 0; i < ids.size(); ++i) {
        CHECK(ids[i] == play::kMenuFirstId + static_cast<int>(i));   // consecutive, in the order they are shown
        const play::MenuItem* f = play::menu_find(m, ids[i]);
        CHECK(f != nullptr && f->id == ids[i]);
    }
    CHECK(play::menu_find(m, play::kMenuFirstId - 1) == nullptr);
    o.upscaler_configured = false;
    o.bug_capture = false;
    std::vector<int> fewer;
    play::menu_for_each(play::build_menu(o), [&](const play::MenuItem& it) { fewer.push_back(it.id); });
    CHECK(fewer.size() == ids.size() - 2);   // no upscaler switch, no bug capture entry
}

// Every check and radio entry changes the settings, and shows itself checked afterwards; a check box goes back with a second click.
NXTEST(menu_model_entries_change_what_they_show) {
    play::MenuOptions o;
    o.upscaler_configured = true;
    const std::vector<play::MenuItem> m = play::build_menu(o);
    const play::LiveStatus live;
    int checks = 0, radios = 0;
    play::menu_for_each(m, [&](const play::MenuItem& it) {
        if (it.kind != play::MenuItem::Kind::Check && it.kind != play::MenuItem::Kind::Radio) return;
        if (!it.set) return;   // the debug overlay: live state, the host answers
        play::Settings s = play::default_settings();
        const bool before = it.checked(s, live);
        const std::string text_before = play::format_settings(s);
        const bool changed = it.set(s);
        if (it.kind == play::MenuItem::Kind::Radio) {
            ++radios;
            CHECK_MSG(changed == !before, it.label);
            CHECK_MSG(it.checked(s, live), it.label);
            CHECK_MSG(!it.set(s), it.label);   // picking the checked one again changes nothing
        } else {
            ++checks;
            CHECK_MSG(changed, it.label);
            CHECK_MSG(it.checked(s, live) == !before, it.label);
            CHECK_MSG(play::format_settings(s) != text_before, it.label);   // written to the file
            it.set(s);
            CHECK_MSG(play::format_settings(s) == text_before, it.label);   // and back
        }
    });
    CHECK(checks >= 14);   // pad rumble and gyro, 16:9, vsync, upscaler, mute, fps counter, 7 diagnostics switches
    CHECK(radios >= 40);
}

NXTEST(menu_model_config_entries_store_and_remove) {
    play::Settings s = play::default_settings();
    CHECK(play::config_store(s, "gfx.device_diagnostics", "1", "false", true));
    CHECK(s.config["gfx.device_diagnostics"] == "1");
    CHECK(!play::config_store(s, "gfx.device_diagnostics", "true", "false", true) || s.config.count("gfx.device_diagnostics") == 1);
    CHECK(play::config_store(s, "gfx.device_diagnostics", "0", "false", true));   // the default: the line goes
    CHECK(s.config.empty());
    CHECK(!play::config_store(s, "am.operation_mode", "Docked", "docked", false));
    CHECK(play::config_store(s, "am.operation_mode", "handheld", "docked", false));
    CHECK(play::config_value(s, "am.operation_mode", "docked") == "handheld");
    CHECK(play::config_value(s, "diag.hitch_ms", "50") == "50");
}
