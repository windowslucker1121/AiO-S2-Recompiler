// SPDX-License-Identifier: GPL-3.0-or-later
// The settings file shared with other programs (play_sync.cpp): key-level diff and in-place edit, the merge of an outside change, the game's own
// writes not clobbering keys someone else changed, and its own write not read back as an outside change. Also the [mouse] capture keys.
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <thread>

#include "play/menu_model.h"
#include "play/play_sync.h"
#include "play/play_types.h"
#include "test_framework.h"
#include "play_fixture/sample_play_fixture.h"
NXTEST_GROUP(play_sync)
NXTEST_GROUP_FIXTURE(nxtest::sample_play::install, nxtest::sample_play::remove)

namespace {

using namespace nxrt::play;

std::string temp_file(const char* name) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "nxrt_play_sync_test";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::filesystem::path p = dir / name;
    std::filesystem::remove(p, ec);
    return p.string();
}
std::string read_all(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
void write_all(const std::string& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}
bool has(const std::string& text, const std::string& what) { return text.find(what) != std::string::npos; }

}  // namespace

NXTEST(diff_finds_changed_added_and_removed_keys) {
    const auto ch = ini_diff("[display]\nvsync = 0\nfps_limit = auto\n[audio]\nvolume = 1\n", "[DISPLAY]\nvsync = 1\nfps_limit = auto\nwidth = 1920\n");
    REQUIRE(ch.size() == 3);
    CHECK(ch[0].kind == IniChange::Kind::Set && ch[0].section == "display" && ch[0].key == "vsync" && ch[0].value == "1");
    CHECK(ch[1].kind == IniChange::Kind::Set && ch[1].key == "width" && ch[1].value == "1920");
    CHECK(ch[2].kind == IniChange::Kind::Remove && ch[2].section == "audio" && ch[2].key == "volume");
    CHECK(ini_diff("[a]\nx = 1\n", "[a]\n x=1 \n").empty());
}

NXTEST(apply_edits_in_place_and_keeps_comments_and_unknown_keys) {
    const std::string text = "# my notes\r\n[display]\r\n; a comment\r\nvsync=0 ; inline\r\nfps_limit = auto\r\n\r\n[library]\r\nfoo = 1\r\n";
    const auto ch = ini_diff("[display]\nvsync = 0\n", "[display]\nvsync = 1\nwidth = 1600\n[audio]\nvolume = 0.5\n");
    const std::string out = ini_apply(text, ch);
    CHECK(has(out, "# my notes\r\n"));
    CHECK(has(out, "; a comment\r\n"));
    CHECK(has(out, "vsync = 1\r\n"));
    CHECK(!has(out, "vsync=0"));
    CHECK(has(out, "fps_limit = auto\r\nwidth = 1600\r\n"));   // after the section's last key
    CHECK(has(out, "[library]\r\nfoo = 1\r\n"));
    CHECK(has(out, "[audio]\r\nvolume = 0.5\r\n"));            // a new section at the end
    // removing a key, and a duplicate key: the last one counts, the earlier one goes
    const std::string out2 = ini_apply("[pad]\ngyro = 1\nrumble = 1\ngyro = 0\n",
                                       ini_diff("[pad]\ngyro = 0\nrumble = 1\n", "[pad]\ngyro = 1\n"));
    CHECK_EQ(out2, std::string("[pad]\ngyro = 1\n"));
}

NXTEST(bindings_sections_change_as_a_whole) {
    const std::string base = "[bindings.all]\nW = move_forward\nS = move_back\n";
    const std::string other = "[bindings.all]\nS = move_back\nW = move_forward\nW = btn_a\n";
    const auto ch = ini_diff(base, other);
    REQUIRE(ch.size() == 1);
    CHECK(ch[0].kind == IniChange::Kind::Section && ch[0].lines.size() == 3);
    const std::string out = ini_apply("[bindings.all]\n# keys\nW = move_forward\nS = move_back\n[pad]\ngyro = 1\n", ch);
    CHECK_EQ(out, std::string("[bindings.all]\nS = move_back\nW = move_forward\nW = btn_a\n# keys\n[pad]\ngyro = 1\n"));
}

NXTEST(merge_takes_over_only_what_the_other_side_changed) {
    const Settings base = default_settings();
    Settings other = base;
    other.display.vsync = !base.display.vsync;
    other.audio.volume = 0.25;
    Settings current = base;
    current.fps.show = !base.fps.show;       // the game's own live state
    current.audio.volume = 0.75;             // changed on both sides: the other side's change is the newer one
    Settings merged;
    REQUIRE(merge_settings(base, other, current, &merged));
    CHECK(merged.display.vsync == other.display.vsync);
    CHECK(merged.fps.show == current.fps.show);
    CHECK(merged.audio.volume == 0.25);
}

NXTEST(own_write_is_not_an_outside_change) {
    const std::string path = temp_file("own.ini");
    REQUIRE(save_settings(path, default_settings(), nullptr));
    SettingsFile f;
    f.set_path(path);
    f.note_current();
    Settings disk;
    CHECK(f.poll(&disk, nullptr) == SettingsFile::Poll::Same);
    Settings mine = default_settings();
    mine.display.vsync = !mine.display.vsync;
    bool external = true;
    REQUIRE(f.save(mine, nullptr, &external));
    CHECK(!external);
    CHECK(f.poll(&disk, nullptr) == SettingsFile::Poll::Same);
    Settings back;
    REQUIRE(load_settings(path, &back, nullptr));
    CHECK(back.display.vsync == mine.display.vsync);
    // nothing changed: no write at all
    const auto t0 = std::filesystem::last_write_time(path);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    REQUIRE(f.save(mine, nullptr, &external));
    CHECK(std::filesystem::last_write_time(path) == t0);
}

NXTEST(outside_change_is_seen_and_merged) {
    const std::string path = temp_file("outside.ini");
    REQUIRE(save_settings(path, default_settings(), nullptr));
    SettingsFile f;
    f.set_path(path);
    f.note_current();
    // the library writes one key in place
    std::string text = read_all(path);
    const size_t at = text.find("fps_limit = ");
    REQUIRE(at != std::string::npos);
    text.replace(at, text.find('\n', at) - at, "fps_limit = 30");
    write_all(path, text);
    Settings disk;
    REQUIRE(f.poll(&disk, nullptr) == SettingsFile::Poll::Changed);
    CHECK_EQ(disk.display.fps_cap, 30);
    Settings current = f.base();
    current.fps.show = !current.fps.show;
    Settings merged;
    REQUIRE(merge_settings(f.base(), disk, current, &merged));
    f.accept(disk);
    CHECK_EQ(merged.display.fps_cap, 30);
    CHECK(merged.fps.show == current.fps.show);
    CHECK(f.poll(&disk, nullptr) == SettingsFile::Poll::Same);
}

NXTEST(game_save_does_not_clobber_an_outside_change) {
    const std::string path = temp_file("clobber.ini");
    REQUIRE(save_settings(path, default_settings(), nullptr));
    SettingsFile f;
    f.set_path(path);
    f.note_current();
    // another program changes the frame limit and adds a comment and a key of its own; the game has not polled yet
    std::string text = read_all(path);
    const size_t at = text.find("fps_limit = ");
    text.replace(at, text.find('\n', at) - at, "fps_limit = 144");
    text = "# edited by hand\n" + text + "\n[library]\nlast_tab = 2\n";
    write_all(path, text);
    // the game saves its own change, made from the old settings
    Settings mine = default_settings();
    mine.display.vsync = !mine.display.vsync;
    bool external = false;
    Settings merged;
    REQUIRE(f.save(mine, nullptr, &external, &merged));
    CHECK(external);
    CHECK_EQ(merged.display.fps_cap, 144);
    CHECK(merged.display.vsync == mine.display.vsync);
    const std::string out = read_all(path);
    CHECK(has(out, "fps_limit = 144"));
    CHECK(has(out, "# edited by hand"));
    CHECK(has(out, "[library]\nlast_tab = 2"));
    Settings back;
    REQUIRE(load_settings(path, &back, nullptr));
    CHECK(back.display.vsync == mine.display.vsync);
    CHECK_EQ(back.display.fps_cap, 144);
    Settings disk;
    CHECK(f.poll(&disk, nullptr) == SettingsFile::Poll::Same);   // the merged write is the game's own
}

NXTEST(missing_file_is_written_whole) {
    const std::string path = temp_file("missing.ini");
    SettingsFile f;
    f.set_path(path);
    f.note_current();
    Settings mine = default_settings();
    mine.audio.volume = 0.5;
    REQUIRE(f.save(mine, nullptr));
    Settings back;
    REQUIRE(load_settings(path, &back, nullptr));
    CHECK(back.audio.volume == 0.5);
}

NXTEST(mouse_capture_keys_parse_and_round_trip) {
    Settings s;
    std::string err;
    REQUIRE(parse_settings("[mouse]\ncapture = auto\nrelease_key = Right Ctrl\n", &s, &err));
    CHECK(s.mouse.capture == CaptureMode::Auto);
    CHECK_EQ(s.mouse.release_key, 228);   // SDL_SCANCODE_RCTRL
    Settings r;
    REQUIRE(parse_settings(format_settings(s), &r, &err));
    CHECK(r.mouse.capture == CaptureMode::Auto);
    CHECK_EQ(r.mouse.release_key, 228);
    REQUIRE(parse_settings("[mouse]\nrelease_key = none\ncapture = off\n", &s, &err));
    CHECK_EQ(s.mouse.release_key, 0);
    CHECK(s.mouse.capture == CaptureMode::Off);
    CHECK(has(format_settings(s), "release_key = none"));
    const Settings d = default_settings();
    CHECK(d.mouse.capture == CaptureMode::Click);
    CHECK_EQ(d.mouse.release_key, 226);   // Left Alt
    CHECK(!parse_settings("[mouse]\ncapture = sometimes\n", &s, &err));
    CHECK(!parse_settings("[mouse]\nrelease_key = Mouse1\n", &s, &err));
}

NXTEST(menu_offers_the_library_settings_only_when_started_from_it) {
    auto count = [](const MenuOptions& o) {
        int n = 0;
        menu_for_each(build_menu(o), [&](const MenuItem& m) { n += m.action == MenuAction::LibrarySettings ? 1 : 0; });
        return n;
    };
    MenuOptions o;
    CHECK_EQ(count(o), 0);
    o.library = true;
    CHECK_EQ(count(o), 1);
    const auto menu = build_menu(o);
    REQUIRE(!menu.empty() && !menu[0].children.empty());
    CHECK(menu[0].children[0].action == MenuAction::LibrarySettings);   // the first entry of the Game menu
}
