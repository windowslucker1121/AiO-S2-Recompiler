// SPDX-License-Identifier: GPL-3.0-or-later
// Play mode: names and parsers (play_types.cpp), factory settings, the settings file reader and writer, load and save (play_config.cpp).
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_scancode.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <system_error>
#include <vector>

#include "nxrt/input_source.h"
#include "play/play_types.h"
#include "test_framework.h"
#include "play_fixture/mpr_play_fixture.h"
NXTEST_GROUP(play_config)
// The checks are written against title E's play vocabulary and factory controls.ini (play_fixture/mpr_play_fixture.h).
NXTEST_GROUP_FIXTURE(nxtest::mpr_play::install, nxtest::mpr_play::remove)

// A REQUIRE with a message (test_framework.h has REQUIRE only).
#define REQUIRE_MSG(cond, msg)                                                        \
    do {                                                                              \
        if (!(cond)) {                                                                \
            ::nxtest::fail(__FILE__, __LINE__, #cond, (msg));                         \
            return;                                                                   \
        }                                                                             \
    } while (0)

namespace {

using namespace nxrt::play;
using nxrt::hid::btn;
using nxrt::hid::kBtnA;
using nxrt::hid::kBtnB;
using nxrt::hid::kBtnDown;
using nxrt::hid::kBtnL;
using nxrt::hid::kBtnLeft;
using nxrt::hid::kBtnMinus;
using nxrt::hid::kBtnPlus;
using nxrt::hid::kBtnR;
using nxrt::hid::kBtnRight;
using nxrt::hid::kBtnStickL;
using nxrt::hid::kBtnStickR;
using nxrt::hid::kBtnUp;
using nxrt::hid::kBtnX;
using nxrt::hid::kBtnY;
using nxrt::hid::kBtnZL;
using nxrt::hid::kBtnZR;

Output make_out(uint64_t buttons, float lx = 0, float ly = 0, float rx = 0, float ry = 0) {
    Output o;
    o.buttons = buttons;
    o.lx = lx;
    o.ly = ly;
    o.rx = rx;
    o.ry = ry;
    return o;
}

InputId key_id(int scancode) {
    InputId id;
    id.kind = InKind::Key;
    id.code = scancode;
    return id;
}

bool near(double a, double b) { return std::fabs(a - b) < 1e-12; }

bool starts_with(const std::string& s, const std::string& prefix) { return s.compare(0, prefix.size(), prefix) == 0; }

// The action bound to `input` in `c` (first match), or Act::kCount when there is no such binding.
Act act_at(const Settings& s, const char* input, Ctx c) {
    InputId id;
    if (!parse_input(input, &id)) return Act::kCount;
    for (const Binding& b : s.bindings) {
        if (b.ctx == c && b.in == id) return b.act;
    }
    return Act::kCount;
}

std::vector<Binding> bindings_in(const Settings& s, int ctx) {
    std::vector<Binding> v;
    for (const Binding& b : s.bindings) {
        if (static_cast<int>(b.ctx) == ctx) v.push_back(b);
    }
    return v;
}

// Field-by-field equality. Bindings are compared per context, because format_settings writes the contexts in a fixed order.
bool same_settings(const Settings& a, const Settings& b) {
    const MouseSettings &ma = a.mouse, &mb = b.mouse;
    if (!(ma.yaw_rad == mb.yaw_rad && ma.pitch_rad == mb.pitch_rad && ma.invert_y == mb.invert_y && ma.invert_x == mb.invert_x &&
          ma.menu_cursor == mb.menu_cursor && ma.wheel_cycles == mb.wheel_cycles))
        return false;
    if (a.aim.lock_mode != b.aim.lock_mode || a.aim.free_lock_margin != b.aim.free_lock_margin || a.camera.gun_drives_view != b.camera.gun_drives_view)
        return false;
    const PadSettings &pa = a.pad, &pb = b.pad;
    if (!(pa.label_abxy == pb.label_abxy && pa.stick_deadzone == pb.stick_deadzone && pa.trigger_threshold == pb.trigger_threshold &&
          pa.gyro == pb.gyro && pa.gyro_scale == pb.gyro_scale && pa.rumble == pb.rumble && pa.rumble_scale == pb.rumble_scale &&
          pa.right_deadzone == pb.right_deadzone && pa.invert_y == pb.invert_y && pa.look_sens == pb.look_sens && pa.alt_orbit == pb.alt_orbit))
        return false;
    const TimingSettings &ta = a.timing, &tb = b.timing;
    if (!(ta.tap_ms == tb.tap_ms && ta.hold_ms == tb.hold_ms && ta.double_ms == tb.double_ms && ta.pulse_ms == tb.pulse_ms)) return false;
    const DisplaySettings &da = a.display, &db = b.display;
    if (!(da.window_mode == db.window_mode && da.vsync == db.vsync && da.fps_cap == db.fps_cap && da.width == db.width &&
          da.height == db.height && da.keep_aspect == db.keep_aspect))
        return false;
    if (!(da.render_scale == db.render_scale && da.render_width == db.render_width && da.render_height == db.render_height &&
          da.aspect == db.aspect && da.fov_offset == db.fov_offset && da.dynamic_resolution == db.dynamic_resolution &&
          da.anisotropy == db.anisotropy && da.lod_bias == db.lod_bias && da.bloom == db.bloom &&
          da.chromatic_aberration == db.chromatic_aberration && da.fast_gpu_memory == db.fast_gpu_memory &&
          da.fewer_gpu_waits == db.fewer_gpu_waits))
        return false;
    const nxrt::extras::Settings &ea = a.extras, &eb = b.extras;
    if (!(ea.hdr.mode == eb.hdr.mode && ea.hdr.format == eb.hdr.format && ea.hdr.peak_nits == eb.hdr.peak_nits &&
          ea.hdr.paper_white_nits == eb.hdr.paper_white_nits && ea.hdr.highlight_boost == eb.hdr.highlight_boost))
        return false;
    if (!(ea.upscale.mode == eb.upscale.mode && ea.upscale.sharpness == eb.upscale.sharpness && ea.upscale.fsr_dll == eb.upscale.fsr_dll &&
          ea.upscale.dlss_dll == eb.upscale.dlss_dll && ea.upscale.xess_dll == eb.upscale.xess_dll &&
          ea.upscale.frame_generation == eb.upscale.frame_generation))
        return false;
    if (!(ea.latency.mode == eb.latency.mode && ea.latency.log_timings == eb.latency.log_timings && ea.latency.frames_ahead == eb.latency.frames_ahead)) return false;
    if (a.prompts.style != b.prompts.style) return false;
    const FpsSettings &fa = a.fps, &fb = b.fps;
    if (!(fa.show == fb.show && fa.key == fb.key && fa.corner == fb.corner && fa.detail == fb.detail && fa.in_screenshots == fb.in_screenshots))
        return false;
    for (int i = 0; i < kPadSrcCount; ++i) {
        if (a.pad.remap[i] != b.pad.remap[i]) return false;
    }
    for (int i = 0; i < kActCount; ++i) {
        if (!(a.out[i] == b.out[i])) return false;
    }
    for (int c = 0; c < kCtxCount; ++c) {
        const std::vector<Binding> va = bindings_in(a, c), vb = bindings_in(b, c);
        if (va.size() != vb.size()) return false;
        for (size_t i = 0; i < va.size(); ++i) {
            if (!(va[i].in == vb[i].in && va[i].act == vb[i].act && va[i].trig == vb[i].trig && va[i].ms == vb[i].ms)) return false;
        }
    }
    return true;
}

std::filesystem::path fresh_test_dir(const char* name) {
    const std::filesystem::path p = std::filesystem::temp_directory_path() / "nxrt_play_config_test" / name;
    std::error_code ec;
    std::filesystem::remove_all(p, ec);
    std::filesystem::create_directories(p, ec);
    return p;
}

void write_file(const std::filesystem::path& p, const std::string& text) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << text;
}

}  // namespace

// ---- names -------------------------------------------------------------------------------------------------------------------

NXTEST(act_table_is_complete) {
    std::set<std::string> names;
    for (int i = 0; i < kActCount; ++i) {
        const ActInfo& info = act_info(static_cast<Act>(i));
        CHECK_EQ((int)info.act, i);
        CHECK(names.insert(info.name).second);   // unique
        Act back = Act::kCount;
        CHECK(parse_act(info.name, &back));
        CHECK_EQ((int)back, i);
    }
    CHECK_EQ(std::string(act_info(Act::None).name), std::string("none"));
    CHECK_EQ(std::string(act_info(Act::MoveForward).name), std::string("move_forward"));
    CHECK_EQ(std::string(act_info(Act::BtnRStick).name), std::string("btn_rstick"));
}

NXTEST(parse_act_is_lenient) {
    Act a = Act::None;
    CHECK(parse_act("FIRE", &a));
    CHECK(a == Act::Primary);
    CHECK(parse_act("move-forward", &a));
    CHECK(a == Act::MoveForward);
    CHECK(parse_act("Dpad-Up", &a));
    CHECK(a == Act::DpadUp);
    CHECK(parse_act("  btn-zl ", &a));
    CHECK(a == Act::BtnZL);
    CHECK(!parse_act("nope", &a));
    CHECK(!parse_act("", &a));
}

NXTEST(ctx_and_trig_names) {
    for (int c = 0; c < kCtxCount; ++c) {
        Ctx back = Ctx::All;
        CHECK(parse_ctx(ctx_name(static_cast<Ctx>(c)), &back));
        CHECK_EQ((int)back, c);
    }
    Ctx m = Ctx::All;
    CHECK(parse_ctx("MENU", &m));
    CHECK(m == Ctx::Menu);
    CHECK(!parse_ctx("lobby", &m));

    Trig t = Trig::Press;
    CHECK(parse_trig("Double_Tap", &t));
    CHECK(t == Trig::DoubleTap);
    CHECK(parse_trig("doubletap", &t));
    CHECK(t == Trig::DoubleTap);
    CHECK(parse_trig("HOLD", &t));
    CHECK(t == Trig::Hold);
    CHECK(!parse_trig("sideways", &t));
    CHECK_EQ(std::string(trig_name(Trig::Toggle)), std::string("toggle"));
}

NXTEST(default_outputs_all_parse) {
    for (int i = 0; i < kActCount; ++i) {
        Output o;
        std::string err;
        CHECK_MSG(parse_output(act_info(static_cast<Act>(i)).default_out, &o, &err), err);
    }
}

// ---- outputs -----------------------------------------------------------------------------------------------------------------

NXTEST(output_parse_terms) {
    Output o;
    REQUIRE(parse_output("ZR", &o));
    CHECK(o == make_out(btn(kBtnZR)));

    REQUIRE(parse_output("X+UP", &o));
    CHECK(o == make_out(btn(kBtnX) | btn(kBtnUp)));
    Output beam1;   // the Beam1 default must be the same output
    REQUIRE(parse_output(act_info(Act::SetA1).default_out, &beam1));
    CHECK(o == beam1);

    REQUIRE(parse_output(" x + up ", &o));   // case and spaces
    CHECK(o == make_out(btn(kBtnX) | btn(kBtnUp)));

    REQUIRE(parse_output("LS_UP", &o));
    CHECK(o == make_out(0, 0, 1, 0, 0));
    REQUIRE(parse_output("RS_LEFT+RS_DOWN", &o));
    CHECK(o == make_out(0, 0, 0, -1, -1));
    REQUIRE(parse_output("LSTICK+RSTICK+PLUS+MINUS", &o));
    CHECK(o == make_out(btn(kBtnStickL) | btn(kBtnStickR) | btn(kBtnPlus) | btn(kBtnMinus)));
    REQUIRE(parse_output("A+B+L+R+ZL+DOWN+LEFT+RIGHT", &o));
    CHECK(o == make_out(btn(kBtnA) | btn(kBtnB) | btn(kBtnL) | btn(kBtnR) | btn(kBtnZL) | btn(kBtnDown) | btn(kBtnLeft) | btn(kBtnRight)));
    REQUIRE(parse_output("LS_LEFT+RS_RIGHT+Y", &o));
    CHECK(o == make_out(btn(nxrt::hid::kBtnY), -1, 0, 1, 0));
}

NXTEST(output_none_and_cancelation) {
    Output o = make_out(btn(kBtnA));
    CHECK(parse_output("", &o));
    CHECK(o.empty());
    o = make_out(btn(kBtnA));
    CHECK(parse_output("none", &o));
    CHECK(o.empty());
    CHECK(parse_output("  NONE ", &o));
    CHECK(o.empty());

    CHECK(parse_output("LS_UP+LS_DOWN", &o));   // opposite directions cancel
    CHECK(o.empty());
    REQUIRE(parse_output("LS_LEFT+LS_RIGHT+ZL", &o));
    CHECK(o == make_out(btn(kBtnZL)));
    REQUIRE(parse_output("LS_UP+LS_UP", &o));   // a repeated term does not stack
    CHECK(o == make_out(0, 0, 1, 0, 0));
}

NXTEST(output_errors) {
    Output o = make_out(7);
    std::string err;
    CHECK(!parse_output("X+FOO", &o, &err));
    CHECK_EQ(err, std::string("unknown output term 'FOO'"));
    CHECK(o == make_out(7));   // *out is written only on success

    err.clear();
    CHECK(!parse_output("none+X", &o, &err));
    CHECK_EQ(err, std::string("unknown output term 'none'"));
    CHECK(!parse_output("X+", &o, &err));
    CHECK(starts_with(err, "empty term"));
    CHECK(!parse_output("+", &o, nullptr));
    CHECK(!parse_output("LS_UP_", &o, nullptr));   // no null err pointer crash
}

NXTEST(output_text_canonical_and_round_trip) {
    Output o;
    REQUIRE(parse_output("RS_RIGHT+ZR+LS_DOWN", &o));
    CHECK_EQ(output_text(o), std::string("ZR+LS_DOWN+RS_RIGHT"));
    CHECK_EQ(output_text(Output{}), std::string("none"));
    REQUIRE(parse_output("UP+X", &o));
    CHECK_EQ(output_text(o), std::string("X+UP"));

    for (int i = 0; i < kActCount; ++i) {   // every default survives parse -> text -> parse
        Output a, b;
        const char* text = act_info(static_cast<Act>(i)).default_out;
        REQUIRE(parse_output(text, &a));
        REQUIRE(parse_output(output_text(a), &b));
        CHECK(a == b);
    }
}

// ---- inputs ------------------------------------------------------------------------------------------------------------------

NXTEST(input_names_parse_and_round_trip) {
    struct Case {
        const char* text;
        int scancode;
        const char* canonical;
    };
    const Case cases[] = {
        {"w", SDL_SCANCODE_W, "W"},
        {"W", SDL_SCANCODE_W, "W"},
        {"left ctrl", SDL_SCANCODE_LCTRL, "Left Ctrl"},
        {"Left Ctrl", SDL_SCANCODE_LCTRL, "Left Ctrl"},
        {"Space", SDL_SCANCODE_SPACE, "Space"},
        {"Page Up", SDL_SCANCODE_PAGEUP, "PageUp"},   // SDL's own name is "PageUp"; the factory text writes "Page Up"
        {"pageup", SDL_SCANCODE_PAGEUP, "PageUp"},
        {"Keypad Enter", SDL_SCANCODE_KP_ENTER, "Keypad Enter"},
        {"Up", SDL_SCANCODE_UP, "Up"},
        {"1", SDL_SCANCODE_1, "1"},
        {"  f  ", SDL_SCANCODE_F, "F"},
    };
    for (const Case& c : cases) {
        InputId id;
        CHECK_MSG(parse_input(c.text, &id), c.text);
        CHECK_EQ((int)id.kind, (int)InKind::Key);
        CHECK_EQ(id.code, c.scancode);
        CHECK_EQ(input_name(id), std::string(c.canonical));
        InputId again;
        CHECK(parse_input(input_name(id), &again));
        CHECK(again == id);
    }
}

NXTEST(input_mouse_and_wheel) {
    for (int n = 1; n <= 5; ++n) {
        InputId id;
        const std::string name = "Mouse" + std::to_string(n);
        CHECK(parse_input(name, &id));
        CHECK_EQ((int)id.kind, (int)InKind::Mouse);
        CHECK_EQ(id.code, n);
        CHECK_EQ(input_name(id), name);
    }
    InputId m;
    CHECK(parse_input("mouse3", &m));
    CHECK_EQ(m.code, 3);
    InputId up, down;
    CHECK(parse_input("WheelUp", &up));
    CHECK((int)up.kind == (int)InKind::Wheel && up.code == 1);
    CHECK(parse_input("wheeldown", &down));
    CHECK((int)down.kind == (int)InKind::Wheel && down.code == -1);
    CHECK_EQ(input_name(up), std::string("WheelUp"));
    CHECK_EQ(input_name(down), std::string("WheelDown"));
}

NXTEST(input_unknown_names) {
    InputId id;
    CHECK(!parse_input("NoSuchKey", &id));
    CHECK(!parse_input("", &id));
    CHECK(!parse_input("   ", &id));
    CHECK(!parse_input("Mouse6", &id));
    CHECK(!parse_input("Mouse0", &id));
}

NXTEST(input_every_scancode_name_parses_back) {
    for (int sc = 1; sc < SDL_SCANCODE_COUNT; ++sc) {
        const char* n = SDL_GetScancodeName(static_cast<SDL_Scancode>(sc));
        if (!n || !*n) continue;
        InputId id;
        CHECK_MSG(parse_input(n, &id), n);
        // Some names are shared by two scancodes ("Return" is also the keypad key on some layouts), so compare names, not codes.
        CHECK_MSG(input_name(id) == std::string(n), n);
    }
}

// ---- factory settings --------------------------------------------------------------------------------------------------------

NXTEST(default_settings_bindings_and_outputs) {
    const Settings s = default_settings();
    CHECK(act_at(s, "W", Ctx::All) == Act::MoveForward);
    CHECK(act_at(s, "Mouse1", Ctx::All) == Act::Primary);
    CHECK(act_at(s, "Mouse1", Ctx::Menu) == Act::Confirm);
    CHECK(act_at(s, "WheelUp", Ctx::All) == Act::SetAPrev);
    CHECK(act_at(s, "Return", Ctx::Menu) == Act::Confirm);
    CHECK(act_at(s, "Left Ctrl", Ctx::All) == Act::AltForm);
    CHECK(act_at(s, "W", Ctx::Menu) == Act::kCount);   // menus have no move binding of their own

    CHECK(s.out[(int)Act::Primary] == make_out(btn(kBtnZR)));
    CHECK(s.out[(int)Act::SetA1] == make_out(btn(kBtnX) | btn(kBtnUp)));
    CHECK(s.out[(int)Act::Confirm] == make_out(btn(kBtnA)));

    CHECK_EQ(s.timing.hold_ms, 250);
    CHECK_EQ(s.display.window_mode, std::string("windowed"));
    CHECK(near(s.mouse.yaw_rad, 0.000767945));
    CHECK(near(s.pad.stick_deadzone, 0.10));
}

NXTEST(default_settings_text_has_no_warnings) {
    std::vector<std::string> warnings;
    Settings s;
    std::string err;
    REQUIRE(parse_settings(default_settings_text(), &s, &err, &warnings));
    CHECK(warnings.empty());
}

// ---- reading the settings file ------------------------------------------------------------------------------------------------

NXTEST(parse_yaw_source_and_radians) {
    Settings s;
    std::string err;
    REQUIRE(parse_settings("[mouse]\nyaw_source = 2.0\npitch = 0.001\n", &s, &err));
    CHECK(near(s.mouse.yaw_rad, rad_per_count(2.0)));
    CHECK(s.mouse.pitch_rad == 0.001);

    REQUIRE(parse_settings("[mouse]\nyaw = 0.001\nyaw_source = 2\n", &s, &err));   // the last one wins
    CHECK(near(s.mouse.yaw_rad, rad_per_count(2.0)));
    REQUIRE(parse_settings("[mouse]\nyaw_source = 2\nyaw = 0.5\n", &s, &err));
    CHECK(s.mouse.yaw_rad == 0.5);
}

NXTEST(file_with_only_mouse_keeps_default_bindings) {
    Settings s;
    std::string err;
    REQUIRE(parse_settings("[mouse]\ninvert_y = 1\n", &s, &err));
    CHECK(s.mouse.invert_y);
    CHECK_EQ(s.bindings.size(), default_settings().bindings.size());
    CHECK(act_at(s, "W", Ctx::All) == Act::MoveForward);
}

NXTEST(old_factory_lock_mode_moves_to_free_once) {
    // A rev 2 file still on the old factory value takes the new one; a player's own choice in a current file stays.
    Settings s;
    std::string err;
    REQUIRE(parse_settings("[aim]\nlock_mode = classic\n[bindings.all]\nW = fire\n[meta]\ndefaults_rev = 2\n", &s, &err));
    CHECK(s.aim.lock_mode == LockMode::Free);
    CHECK(s.migrated);
    CHECK(s.migration_added.find("lock_mode = free") != std::string::npos);
    Settings k;
    REQUIRE(parse_settings("[aim]\nlock_mode = classic\n[bindings.all]\nW = fire\n[meta]\ndefaults_rev = 3\n", &k, &err));
    CHECK(k.aim.lock_mode == LockMode::Classic);
}

NXTEST(one_bindings_section_drops_default_bindings) {
    Settings s;
    std::string err;
    REQUIRE(parse_settings("[bindings.menu]\nReturn = confirm\n[meta]\ndefaults_rev = 2\n", &s, &err));
    CHECK_EQ(s.bindings.size(), 1u);
    CHECK(act_at(s, "Return", Ctx::Menu) == Act::Confirm);
    CHECK(act_at(s, "W", Ctx::All) == Act::kCount);
}

NXTEST(binding_trigger_and_ms_parse) {
    Settings s;
    std::string err;
    REQUIRE(parse_settings("[bindings.gameplay]\nLeft Ctrl = morph hold 250\nSpace = jump toggle\nMouse1 = fire tap\n[meta]\ndefaults_rev = 2\n", &s, &err));
    REQUIRE_MSG(s.bindings.size() == 3u, "three bindings");
    CHECK(s.bindings[0].in == key_id(SDL_SCANCODE_LCTRL));
    CHECK(s.bindings[0].ctx == Ctx::Play);
    CHECK(s.bindings[0].act == Act::AltForm);
    CHECK(s.bindings[0].trig == Trig::Hold);
    CHECK_EQ(s.bindings[0].ms, 250);
    CHECK(s.bindings[1].trig == Trig::Toggle);
    CHECK_EQ(s.bindings[1].ms, 0);
    CHECK(s.bindings[2].trig == Trig::Tap);
}

NXTEST(binding_keys_equals_and_semicolon) {
    // The keys "=" and ";" are real SDL names; a line for each must not be cut apart by the '=' split or taken as a comment.
    const char* eq_name = SDL_GetScancodeName(SDL_SCANCODE_EQUALS);
    const char* semi_name = SDL_GetScancodeName(SDL_SCANCODE_SEMICOLON);
    Settings s;
    std::string err;
    const std::string text = std::string("[bindings.menu]\n") + eq_name + " = confirm\n" + semi_name + " = back\n";
    REQUIRE_MSG(parse_settings(text, &s, &err), err);
    CHECK(act_at(s, eq_name, Ctx::Menu) == Act::Confirm);
    CHECK(act_at(s, semi_name, Ctx::Menu) == Act::Back);
}

NXTEST(inline_comments_and_case) {
    Settings s;
    std::string err;
    REQUIRE(parse_settings("[mouse] # the mouse\ninvert_y = 1 ; yes\nwheel_beams = off # no\n", &s, &err));
    CHECK(s.mouse.invert_y);
    CHECK(!s.mouse.wheel_cycles);

    REQUIRE(parse_settings("[MOUSE]\nINVERT_X = TRUE\n[Display]\nWindow_Mode = Borderless\n", &s, &err));
    CHECK(s.mouse.invert_x);
    CHECK_EQ(s.display.window_mode, std::string("borderless"));
}

NXTEST(menu_touch_legacy_key_is_ignored) {
    // The old key is accepted silently and does nothing: menu_cursor keeps its default, and the writer never writes menu_touch.
    Settings s;
    std::string err;
    std::vector<std::string> w;
    REQUIRE(parse_settings("[mouse]\nmenu_touch = 0\n", &s, &err, &w));
    CHECK(w.empty());
    CHECK(s.mouse.menu_cursor);
    CHECK(format_settings(s).find("menu_touch") == std::string::npos);
}

NXTEST(menu_cursor_parses_and_round_trips) {
    Settings s;
    std::string err;
    std::vector<std::string> w;
    REQUIRE(parse_settings("[mouse]\nmenu_cursor = 0\n", &s, &err, &w));
    CHECK(w.empty());
    CHECK(!s.mouse.menu_cursor);
    const std::string text = format_settings(s);
    CHECK(text.find("menu_cursor = 0") != std::string::npos);
    Settings r;
    REQUIRE_MSG(parse_settings(text, &r, &err), err);
    CHECK(!r.mouse.menu_cursor);
    CHECK(same_settings(s, r));
}

NXTEST(lockfree_parses_and_round_trips) {
    Settings s;
    std::string err;
    std::vector<std::string> w;
    REQUIRE(parse_settings("[aim]\nlock_mode = free\n", &s, &err, &w));
    CHECK(w.empty());
    CHECK(s.aim.lock_mode == LockMode::Free);
    Settings u;
    REQUIRE(parse_settings("[aim]\nlock_mode = FREE\n", &u, &err));
    CHECK(u.aim.lock_mode == LockMode::Free);
    Settings c;
    REQUIRE(parse_settings("[aim]\nlock_mode = classic\n", &c, &err));
    CHECK(c.aim.lock_mode == LockMode::Classic);
    const std::string text = format_settings(s);
    CHECK(text.find("lock_mode = free") != std::string::npos);
    Settings r;
    REQUIRE_MSG(parse_settings(text, &r, &err), err);
    CHECK(r.aim.lock_mode == LockMode::Free);
    CHECK(same_settings(s, r));
}

NXTEST(lockfree_rejects_unknown_value) {
    Settings s;
    std::string err;
    CHECK(!parse_settings("[aim]\nlock_mode = sideways\n", &s, &err));
    CHECK(err.find("lock_mode") != std::string::npos);
}

NXTEST(lockfree_margin_parses_and_round_trips) {
    Settings s;
    std::string err;
    std::vector<std::string> w;
    REQUIRE(parse_settings("[aim]\nfree_lock_margin = 0.3\n", &s, &err, &w));
    CHECK(w.empty());
    CHECK(s.aim.free_lock_margin == 0.3);
    const std::string text = format_settings(s);
    CHECK(text.find("free_lock_margin = 0.3") != std::string::npos);
    Settings r;
    REQUIRE_MSG(parse_settings(text, &r, &err), err);
    CHECK(r.aim.free_lock_margin == 0.3);
    CHECK(same_settings(s, r));
}

NXTEST(lockfree_margin_rejects_out_of_range) {
    Settings s;
    std::string err;
    CHECK(!parse_settings("[aim]\nfree_lock_margin = 0.5\n", &s, &err));
    CHECK(err.find("free_lock_margin") != std::string::npos);
    CHECK(!parse_settings("[aim]\nfree_lock_margin = -0.1\n", &s, &err));
    CHECK(err.find("free_lock_margin") != std::string::npos);
}

NXTEST(camera_gun_drives_view_parses_and_round_trips) {
    Settings s;
    std::string err;
    std::vector<std::string> w;
    REQUIRE(parse_settings("[camera]\ngun_drives_view = 0\n", &s, &err, &w));
    CHECK(w.empty());
    CHECK(!s.camera.gun_drives_view);
    const std::string text = format_settings(s);
    CHECK(text.find("gun_drives_view = 0") != std::string::npos);
    Settings r;
    REQUIRE_MSG(parse_settings(text, &r, &err), err);
    CHECK(!r.camera.gun_drives_view);
    CHECK(same_settings(s, r));
}

NXTEST(camera_and_aim_defaults_are_free_and_on) {
    const Settings d = default_settings();
    CHECK(d.aim.lock_mode == LockMode::Free);
    CHECK(d.aim.free_lock_margin == 0.15);
    CHECK(d.camera.gun_drives_view);
}

NXTEST(parse_errors_name_the_line) {
    struct Case {
        const char* text;
        const char* expect;   // the start of err
    };
    const Case cases[] = {
        {"[mouse]\n\n[bindings.all]\nFoo = fire\n", "line 4: unknown input 'Foo'"},
        {"[timing]\ntap_ms = 0\n", "line 2: tap_ms must be 1..5000"},
        {"[bindings.all]\nW = fly\n", "line 2: unknown action 'fly'"},
        {"[bindings.all]\nW = fire sideways\n", "line 2: unknown trigger 'sideways'"},
        {"[bindings.all]\nW = fire hold 0\n", "line 2: ms must be 1..5000"},
        {"[display]\nwindow_mode = fullscreen\n", "line 2: window_mode must be windowed, borderless or exclusive"},
        {"[pad]\ndeadzone = 0.7\n", "line 2: deadzone must be 0..0.5"},
        {"[mouse]\nyaw = nope\n", "line 2: yaw must be a number"},
        {"[switch]\nfire = X+FOO\n", "line 2: unknown output term 'FOO'"},
        {"[bindings.all\nW = fire\n", "line 1: section header without ']'"},
        {"[mouse]\njust words\n", "line 2: expected 'key = value'"},
        {"[display]\nfps_cap = 5000\n", "line 2: fps_cap must be 0..1000"},
    };
    for (const Case& c : cases) {
        Settings s;
        std::string err;
        CHECK(!parse_settings(c.text, &s, &err));
        CHECK_MSG(starts_with(err, c.expect), err);
    }
}

NXTEST(failed_parse_does_not_touch_out) {
    Settings s;
    s.timing.tap_ms = 123;
    std::string err;
    CHECK(!parse_settings("[timing]\ntap_ms = 999999\n", &s, &err));
    CHECK_EQ(s.timing.tap_ms, 123);
}

NXTEST(unknown_keys_and_sections_warn) {
    Settings s;
    std::string err;
    std::vector<std::string> w;
    REQUIRE(parse_settings("[mouse]\nbogus = 1\n[nosuch]\nx = 1\n[bindings.all]\nW = fire\n[meta]\ndefaults_rev = 2\n", &s, &err, &w));
    REQUIRE_MSG(w.size() == 2u, "two warnings");
    CHECK_EQ(w[0], std::string("line 2: unknown key 'bogus' in [mouse]"));
    CHECK_EQ(w[1], std::string("line 3: unknown section '[nosuch]'"));
    CHECK(s.bindings.size() == 1u);   // the unknown section's keys did not add anything
}

// ---- writing and the round trip ----------------------------------------------------------------------------------------------

NXTEST(format_has_sections_in_order) {
    const std::string text = format_settings(default_settings());
    const char* order[] = {"[mouse]", "[pad]", "[timing]", "[display]", "[hdr]", "[upscale]", "[latency]", "[prompts]", "[switch]", "[bindings.all]", "[bindings.gameplay]",
                           "[bindings.morph]", "[bindings.map]", "[bindings.menu]", "[bindings.scan]"};
    size_t at = 0;
    for (const char* h : order) {
        const size_t p = text.find(h, at);
        CHECK_MSG(p != std::string::npos, h);
        if (p == std::string::npos) return;
        at = p;
    }
}

NXTEST(format_parse_round_trip_defaults) {
    const Settings d = default_settings();
    const std::string text = format_settings(d);
    Settings r;
    std::string err;
    REQUIRE_MSG(parse_settings(text, &r, &err), err);
    CHECK(same_settings(d, r));
    CHECK_EQ(format_settings(r), text);
}

NXTEST(format_parse_round_trip_modified) {
    Settings s = default_settings();
    s.mouse.invert_y = true;
    s.mouse.yaw_rad = 0.00125;   // values that %.9g writes exactly
    s.mouse.wheel_cycles = false;
    s.pad.stick_deadzone = 0.25;
    s.pad.trigger_threshold = 0.5;
    s.pad.gyro = false;
    s.pad.gyro_scale = 1.5;
    s.pad.rumble_scale = 0.75;
    s.timing.tap_ms = 150;
    s.timing.hold_ms = 400;
    s.display.window_mode = "exclusive";
    s.display.vsync = true;
    s.display.fps_cap = 144;
    s.display.width = 1920;
    s.display.height = 1080;
    s.display.keep_aspect = false;
    REQUIRE(parse_output("A+RS_UP", &s.out[(int)Act::Primary]));
    REQUIRE(parse_output("none", &s.out[(int)Act::Jump]));
    REQUIRE(parse_output("LS_LEFT+LS_RIGHT+X", &s.out[(int)Act::Target]));

    s.bindings.clear();
    Binding b;
    b.in = key_id(SDL_SCANCODE_LCTRL);
    b.ctx = Ctx::Play;
    b.act = Act::AltForm;
    b.trig = Trig::Hold;
    b.ms = 250;
    s.bindings.push_back(b);
    b = Binding{};
    b.in.kind = InKind::Mouse;
    b.in.code = 4;
    b.ctx = Ctx::All;
    b.act = Act::Secondary;
    s.bindings.push_back(b);
    b = Binding{};
    b.in = key_id(SDL_SCANCODE_EQUALS);
    b.ctx = Ctx::Menu;
    b.act = Act::Confirm;
    s.bindings.push_back(b);
    b = Binding{};
    b.in = key_id(SDL_SCANCODE_SEMICOLON);
    b.ctx = Ctx::Menu;
    b.act = Act::Back;
    b.trig = Trig::Tap;
    s.bindings.push_back(b);
    b = Binding{};
    b.in.kind = InKind::Wheel;
    b.in.code = 1;
    b.ctx = Ctx::All;
    b.act = Act::SetAPrev;
    s.bindings.push_back(b);
    b = Binding{};
    b.in = key_id(SDL_SCANCODE_SPACE);
    b.ctx = Ctx::Map;
    b.act = Act::Map;
    b.trig = Trig::Toggle;
    s.bindings.push_back(b);
    b = Binding{};
    b.in = key_id(SDL_SCANCODE_Q);
    b.ctx = Ctx::Alt;
    b.act = Act::Jump;
    b.trig = Trig::DoubleTap;
    b.ms = 300;
    s.bindings.push_back(b);

    const std::string text = format_settings(s);
    Settings r;
    std::string err;
    REQUIRE_MSG(parse_settings(text, &r, &err), err);
    CHECK(same_settings(s, r));
    CHECK_EQ(format_settings(r), text);
}

NXTEST(empty_bindings_stay_empty_after_round_trip) {
    Settings s = default_settings();
    s.bindings.clear();
    const std::string text = format_settings(s);
    CHECK(text.find("[bindings.all]") != std::string::npos);   // the sections are written even when empty
    Settings r;
    std::string err;
    REQUIRE(parse_settings(text, &r, &err));
    CHECK(r.bindings.empty());
}

// ---- load and save -----------------------------------------------------------------------------------------------------------

NXTEST(load_missing_file_gives_defaults_and_no_error) {
    const std::filesystem::path dir = fresh_test_dir("missing");
    const std::string path = (dir / "settings.ini").string();
    Settings s;
    std::string err = "not cleared";
    CHECK(!load_settings(path, &s, &err));
    CHECK(err.empty());
    CHECK(same_settings(s, default_settings()));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

NXTEST(load_invalid_file_gives_defaults_and_error) {
    const std::filesystem::path dir = fresh_test_dir("invalid");
    const std::string path = (dir / "settings.ini").string();
    write_file(dir / "settings.ini", "[mouse]\nyaw = nope\n");
    Settings s;
    s.timing.tap_ms = 7;
    std::string err;
    CHECK(!load_settings(path, &s, &err));
    CHECK(err.find(path) != std::string::npos);
    CHECK(err.find("line 2:") != std::string::npos);
    CHECK(same_settings(s, default_settings()));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

NXTEST(save_then_load_round_trip) {
    const std::filesystem::path dir = fresh_test_dir("roundtrip");
    const std::string path = (dir / "sub" / "settings.ini").string();   // the folder does not exist yet: save creates it

    Settings s = default_settings();
    s.mouse.invert_x = true;
    s.display.fps_cap = 144;
    std::string err;
    REQUIRE_MSG(save_settings(path, s, &err), err);
    CHECK(!std::filesystem::exists(path + ".tmp"));   // the temp file was renamed away

    Settings r;
    REQUIRE_MSG(load_settings(path, &r, &err), err);
    CHECK(err.empty());
    CHECK(same_settings(r, s));

    // Saving again replaces the file.
    s.display.fps_cap = 30;
    s.bindings.clear();
    REQUIRE_MSG(save_settings(path, s, &err), err);
    Settings r2;
    REQUIRE_MSG(load_settings(path, &r2, &err), err);
    CHECK(same_settings(r2, s));
    CHECK(r2.bindings.empty());

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// ---- S10 display options -----------------------------------------------------------------------------------------------------

NXTEST(display_render_keys_defaults) {
    const DisplaySettings d = default_settings().display;
    CHECK_EQ(d.render_scale, 100);
    CHECK_EQ(d.render_width, 0);
    CHECK_EQ(d.render_height, 0);
    CHECK(d.aspect == 0.0);
    CHECK(d.fov_offset == 0.0);
    CHECK(!d.dynamic_resolution);
    CHECK_EQ(d.anisotropy, 0);
    CHECK(d.lod_bias == 0.0);
    CHECK(d.bloom);
    CHECK(d.chromatic_aberration);
    CHECK(d.fast_gpu_memory);
    CHECK(d.fewer_gpu_waits);
}

NXTEST(default_settings_text_has_display_keys) {
    const std::string text = default_settings_text();
    const char* lines[] = {"render_scale = 100", "render_width = 0",  "render_height = 0", "aspect = 0",        "fov_offset = 0",
                           "dynamic_resolution = 0", "anisotropy = 0", "lod_bias = 0",    "bloom = 1",       "chromatic_aberration = 1",
                           "fast_gpu_memory = 1",    "fewer_gpu_waits = 1"};
    for (const char* l : lines) CHECK_MSG(text.find(std::string("\n") + l + "\n") != std::string::npos, l);
    // The four that only apply at the next start say so next to their key.
    size_t restart_notes = 0;
    for (size_t p = text.find("applies at the next start"); p != std::string::npos; p = text.find("applies at the next start", p + 1))
        ++restart_notes;
    CHECK(restart_notes >= 4u);
}

NXTEST(display_render_keys_parse) {
    Settings s;
    std::string err;
    const char* text =
        "[display]\nrender_scale = 150\nrender_width = 1600\nrender_height = 900\naspect = 1.6\nfov_offset = -12.5\n"
        "dynamic_resolution = yes\nanisotropy = 8\nlod_bias = -0.75\nbloom = 0\nchromatic_aberration = off\n"
        "fast_gpu_memory = 0\nfewer_gpu_waits = off\n";
    REQUIRE_MSG(parse_settings(text, &s, &err), err);
    CHECK_EQ(s.display.render_scale, 150);
    CHECK_EQ(s.display.render_width, 1600);
    CHECK_EQ(s.display.render_height, 900);
    CHECK(s.display.aspect == 1.6);
    CHECK(s.display.fov_offset == -12.5);
    CHECK(s.display.dynamic_resolution);
    CHECK_EQ(s.display.anisotropy, 8);
    CHECK(s.display.lod_bias == -0.75);
    CHECK(!s.display.bloom);
    CHECK(!s.display.chromatic_aberration);
    CHECK(!s.display.fast_gpu_memory);
    CHECK(!s.display.fewer_gpu_waits);
}

NXTEST(display_render_keys_range_errors) {
    const struct {
        const char* text;
        const char* expect;
    } cases[] = {
        {"[display]\nrender_scale = 24\n", "line 2: render_scale must be 25..400, got 24"},
        {"[display]\nrender_scale = 401\n", "line 2: render_scale must be 25..400, got 401"},
        {"[display]\nrender_scale = 1.5\n", "line 2: render_scale must be a whole number"},
        {"[display]\nrender_width = 100\n", "line 2: render_width must be 0 or 320..16384, got 100"},
        {"[display]\nrender_height = 16385\n", "line 2: render_height must be 0 or 320..16384, got 16385"},
        {"[display]\nrender_height = wide\n", "line 2: render_height must be a whole number"},
        {"[display]\naspect = 0.5\n", "line 2: aspect must be 0 (auto) or 1.0..5.0, got 0.5"},
        {"[display]\naspect = 5.5\n", "line 2: aspect must be 0 (auto) or 1.0..5.0, got 5.5"},
        {"[display]\nfov_offset = 41\n", "line 2: fov_offset must be -30..40, got 41"},
        {"[display]\nfov_offset = -31\n", "line 2: fov_offset must be -30..40, got -31"},
        {"[display]\nfov_offset = abc\n", "line 2: fov_offset must be a number"},
        {"[display]\ndynamic_resolution = maybe\n", "line 2: dynamic_resolution must be 1/0"},
        {"[display]\nanisotropy = 3\n", "line 2: anisotropy must be 0, 2, 4, 8 or 16, got 3"},
        {"[display]\nanisotropy = 32\n", "line 2: anisotropy must be 0, 2, 4, 8 or 16, got 32"},
        {"[display]\nlod_bias = 2.5\n", "line 2: lod_bias must be -2..2, got 2.5"},
        {"[display]\nbloom = 2\n", "line 2: bloom must be 1/0"},
        {"[display]\nchromatic_aberration = sometimes\n", "line 2: chromatic_aberration must be 1/0"},
    };
    for (const auto& c : cases) {
        Settings s;
        std::string err;
        CHECK(!parse_settings(c.text, &s, &err));
        CHECK_MSG(starts_with(err, c.expect), err);
    }
}

NXTEST(display_render_keys_boundaries_accepted) {
    Settings s;
    std::string err;
    REQUIRE_MSG(parse_settings("[display]\nrender_scale = 25\nrender_width = 320\nrender_height = 16384\naspect = 1.0\nfov_offset = -30\n"
                               "anisotropy = 16\nlod_bias = -2\n",
                               &s, &err),
                err);
    CHECK_EQ(s.display.render_scale, 25);
    CHECK_EQ(s.display.render_width, 320);
    CHECK_EQ(s.display.render_height, 16384);
    CHECK(s.display.aspect == 1.0);
    CHECK(s.display.fov_offset == -30.0);
    CHECK_EQ(s.display.anisotropy, 16);
    CHECK(s.display.lod_bias == -2.0);

    REQUIRE_MSG(parse_settings("[display]\nrender_scale = 400\nrender_width = 0\nrender_height = 0\naspect = 5\nfov_offset = 40\n"
                               "anisotropy = 0\nlod_bias = 2\n",
                               &s, &err),
                err);
    CHECK_EQ(s.display.render_scale, 400);
    CHECK_EQ(s.display.render_width, 0);
    CHECK(s.display.aspect == 5.0);
    CHECK(s.display.fov_offset == 40.0);
    CHECK_EQ(s.display.anisotropy, 0);
    CHECK(s.display.lod_bias == 2.0);

    for (int a : {2, 4, 8}) {
        REQUIRE_MSG(parse_settings("[display]\nanisotropy = " + std::to_string(a) + "\n", &s, &err), err);
        CHECK_EQ(s.display.anisotropy, a);
    }
}

NXTEST(display_render_keys_round_trip) {
    Settings s = default_settings();
    s.display.render_scale = 150;
    s.display.render_width = 1600;
    s.display.render_height = 900;
    s.display.aspect = 16.0 / 9.0;            // 1.7777... needs 17 digits to read back exactly
    s.display.fov_offset = -12.3456789;
    s.display.dynamic_resolution = true;
    s.display.anisotropy = 16;
    s.display.lod_bias = -0.7;
    s.display.bloom = false;
    s.display.chromatic_aberration = false;
    s.display.fast_gpu_memory = false;
    s.display.fewer_gpu_waits = false;

    const std::string text = format_settings(s);
    Settings r;
    std::string err;
    REQUIRE_MSG(parse_settings(text, &r, &err), err);
    CHECK(same_settings(s, r));
    CHECK(r.display.aspect == 16.0 / 9.0);
    CHECK_EQ(format_settings(r), text);

    // The other presets, and the Auto and fixed-size defaults, also come back exactly.
    const double presets[] = {1.6, 21.0 / 9.0, 32.0 / 9.0, 0.0};
    for (double a : presets) {
        s.display.aspect = a;
        REQUIRE_MSG(parse_settings(format_settings(s), &r, &err), err);
        CHECK(r.display.aspect == a);
    }
}

// ---- display extras: [hdr] [upscale] [latency] (nxrt/extras.h) ----------------------------------------------------------------------

using nxrt::extras::HdrFormat;
using nxrt::extras::HdrMode;
using nxrt::extras::LatencyMode;
using nxrt::extras::UpscaleMode;

namespace {
// Every key of the three sections, with values that differ from the defaults.
const char* const kExtrasFile =
    "[hdr]\n"
    "mode = on\n"
    "format = hdr10\n"
    "peak_nits = 1000\n"
    "paper_white_nits = 250\n"
    "highlight_boost = 1.5\n"
    "[upscale]\n"
    "mode = fsr1\n"
    "sharpness = 0.5\n"
    "fsr_dll = C:\\Tools\\amd_fidelityfx_vk.dll\n"
    "dlss_dll = D:\\dlss\\nvngx_dlss.dll\n"
    "xess_dll = \n"
    "frame_generation = 1\n"
    "[latency]\n"
    "mode = boost\n"
    "frames_ahead = 2\n"
    "log_timings = 1\n";
}  // namespace

NXTEST(extras_defaults) {
    const Settings d = default_settings();
    CHECK(d.extras.hdr.mode == HdrMode::Auto);
    CHECK(d.extras.hdr.format == HdrFormat::Auto);
    CHECK(d.extras.hdr.peak_nits == 0.0);
    CHECK(d.extras.hdr.paper_white_nits == 0.0);
    CHECK(d.extras.hdr.highlight_boost == 1.0);
    CHECK(d.extras.upscale.mode == UpscaleMode::Off);
    CHECK(d.extras.upscale.sharpness == 0.8);
    CHECK(d.extras.upscale.fsr_dll.empty());
    CHECK(d.extras.upscale.dlss_dll.empty());
    CHECK(d.extras.upscale.xess_dll.empty());
    CHECK(!d.extras.upscale.frame_generation);
    CHECK(d.extras.latency.mode == LatencyMode::Off);
    CHECK(!d.extras.latency.log_timings);

    // A file without the sections keeps the defaults.
    Settings s;
    std::string err;
    REQUIRE_MSG(parse_settings("[mouse]\nyaw = 0.001\n", &s, &err), err);
    CHECK(s.extras.hdr.mode == HdrMode::Auto);
    CHECK(s.extras.latency.mode == LatencyMode::Off);
}

NXTEST(extras_full_file_every_key) {
    Settings s;
    std::string err;
    std::vector<std::string> w;
    REQUIRE_MSG(parse_settings(kExtrasFile, &s, &err, &w), err);
    CHECK(w.empty());
    CHECK(s.extras.hdr.mode == HdrMode::On);
    CHECK(s.extras.hdr.format == HdrFormat::Hdr10);
    CHECK(s.extras.hdr.peak_nits == 1000.0);
    CHECK(s.extras.hdr.paper_white_nits == 250.0);
    CHECK(s.extras.hdr.highlight_boost == 1.5);
    CHECK(s.extras.upscale.mode == UpscaleMode::Fsr1);
    CHECK(s.extras.upscale.sharpness == 0.5);
    CHECK_EQ(s.extras.upscale.fsr_dll, std::string("C:\\Tools\\amd_fidelityfx_vk.dll"));
    CHECK_EQ(s.extras.upscale.dlss_dll, std::string("D:\\dlss\\nvngx_dlss.dll"));
    CHECK(s.extras.upscale.xess_dll.empty());
    CHECK(s.extras.upscale.frame_generation);
    CHECK(s.extras.latency.mode == LatencyMode::Boost);
    CHECK(s.extras.latency.log_timings);
    CHECK_EQ(s.extras.latency.frames_ahead, 2);
}

NXTEST(extras_numbers_range_and_auto) {
    struct Case {
        const char* text;
        const char* expect;   // the start of err
    };
    const Case bad[] = {
        {"[hdr]\npeak_nits = 100\n", "line 2: peak_nits must be 0 (auto) or 200..10000, got 100"},
        {"[hdr]\npeak_nits = 10001\n", "line 2: peak_nits must be 0 (auto) or 200..10000, got 10001"},
        {"[hdr]\npeak_nits = bright\n", "line 2: peak_nits must be a number"},
        {"[hdr]\npaper_white_nits = 79\n", "line 2: paper_white_nits must be 0 (auto) or 80..500, got 79"},
        {"[hdr]\npaper_white_nits = 501\n", "line 2: paper_white_nits must be 0 (auto) or 80..500, got 501"},
        {"[hdr]\nhighlight_boost = 2.5\n", "line 2: highlight_boost must be 0..2, got 2.5"},
        {"[hdr]\nhighlight_boost = -0.1\n", "line 2: highlight_boost must be 0..2, got -0.1"},
        {"[upscale]\nsharpness = 1.5\n", "line 2: sharpness must be 0..1, got 1.5"},
    };
    for (const Case& c : bad) {
        Settings s;
        std::string err;
        CHECK(!parse_settings(c.text, &s, &err));
        CHECK_MSG(starts_with(err, c.expect), err);
    }

    // The edges, and 0 (auto), are accepted.
    Settings s;
    std::string err;
    REQUIRE_MSG(parse_settings("[hdr]\npeak_nits = 200\npaper_white_nits = 80\nhighlight_boost = 0\n", &s, &err), err);
    CHECK(s.extras.hdr.peak_nits == 200.0);
    CHECK(s.extras.hdr.paper_white_nits == 80.0);
    CHECK(s.extras.hdr.highlight_boost == 0.0);
    REQUIRE_MSG(parse_settings("[hdr]\npeak_nits = 10000\npaper_white_nits = 500\nhighlight_boost = 2\n", &s, &err), err);
    CHECK(s.extras.hdr.peak_nits == 10000.0);
    CHECK(s.extras.hdr.paper_white_nits == 500.0);
    CHECK(s.extras.hdr.highlight_boost == 2.0);
    REQUIRE_MSG(parse_settings("[hdr]\npeak_nits = 0\npaper_white_nits = 0\n", &s, &err), err);
    CHECK(s.extras.hdr.peak_nits == 0.0);
    CHECK(s.extras.hdr.paper_white_nits == 0.0);
    REQUIRE_MSG(parse_settings("[upscale]\nsharpness = 0\n", &s, &err), err);
    CHECK(s.extras.upscale.sharpness == 0.0);
    REQUIRE_MSG(parse_settings("[upscale]\nsharpness = 1\n", &s, &err), err);
    CHECK(s.extras.upscale.sharpness == 1.0);
}

NXTEST(extras_bad_enum_words_name_the_allowed_ones) {
    struct Case {
        const char* text;
        const char* expect;
    };
    const Case bad[] = {
        {"[hdr]\nmode = maybe\n", "line 2: mode must be off, auto, on or native, got 'maybe'"},
        {"[hdr]\nformat = hdr12\n", "line 2: format must be auto, hdr10 or scrgb, got 'hdr12'"},
        {"[upscale]\nmode = taa\n", "line 2: mode must be off, fsr1, spatial, fsr3, dlss, xess, nis or dlaa, got 'taa'"},
        {"[latency]\nmode = fast\n", "line 2: mode must be off, auto, on or boost, got 'fast'"},
        {"[latency]\nlog_timings = maybe\n", "line 2: log_timings must be 1/0"},
        {"[upscale]\nframe_generation = x\n", "line 2: frame_generation must be 1/0"},
    };
    for (const Case& c : bad) {
        Settings s;
        std::string err;
        CHECK(!parse_settings(c.text, &s, &err));
        CHECK_MSG(starts_with(err, c.expect), err);
    }
}

NXTEST(extras_upscale_modes_round_trip) {
    struct Case {
        const char* word;      // as written in the file
        UpscaleMode want;
        const char* written;   // as format_settings writes it
    };
    const Case cases[] = {
        {"off", UpscaleMode::Off, "off"},   {"fsr1", UpscaleMode::Fsr1, "fsr1"}, {"spatial", UpscaleMode::Fsr1, "fsr1"},
        {"fsr3", UpscaleMode::Fsr3, "fsr3"}, {"dlss", UpscaleMode::Dlss, "dlss"}, {"xess", UpscaleMode::Xess, "xess"},
    };
    for (const Case& c : cases) {
        Settings s;
        std::string err;
        REQUIRE_MSG(parse_settings(std::string("[upscale]\nmode = ") + c.word + "\n", &s, &err), err);
        CHECK(s.extras.upscale.mode == c.want);
        const std::string text = format_settings(s);
        CHECK_MSG(text.find(std::string("[upscale]\nmode = ") + c.written + "\n") != std::string::npos, c.word);
        Settings r;
        REQUIRE_MSG(parse_settings(text, &r, &err), err);
        CHECK(r.extras.upscale.mode == c.want);
        CHECK(same_settings(s, r));
    }
}

NXTEST(extras_unknown_keys_warn_not_fail) {
    Settings s;
    std::string err;
    std::vector<std::string> w;
    REQUIRE_MSG(parse_settings("[hdr]\nbogus = 1\n[upscale]\nsharpen = 1\n[latency]\nmode = on\n", &s, &err, &w), err);
    REQUIRE_MSG(w.size() == 2u, "two warnings");
    CHECK_EQ(w[0], std::string("line 2: unknown key 'bogus' in [hdr]"));
    CHECK_EQ(w[1], std::string("line 4: unknown key 'sharpen' in [upscale]"));
    CHECK(s.extras.latency.mode == LatencyMode::On);
}

NXTEST(extras_keys_and_words_case_insensitive) {
    const char* text =
        "[HDR]\nMODE = ON\nFormat = ScRGB\nPeak_Nits = 1000\nPaper_White_Nits = 0\nHighlight_Boost = 0.5\n"
        "[Upscale]\nMode = FSR1\nSharpness = 0.25\nFrame_Generation = YES\n"
        "[LATENCY]\nMode = Boost\nLog_Timings = On\n";
    Settings s;
    std::string err;
    std::vector<std::string> w;
    REQUIRE_MSG(parse_settings(text, &s, &err, &w), err);
    CHECK(w.empty());
    CHECK(s.extras.hdr.mode == HdrMode::On);
    CHECK(s.extras.hdr.format == HdrFormat::ScRgb);
    CHECK(s.extras.hdr.peak_nits == 1000.0);
    CHECK(s.extras.hdr.paper_white_nits == 0.0);
    CHECK(s.extras.hdr.highlight_boost == 0.5);
    CHECK(s.extras.upscale.mode == UpscaleMode::Fsr1);
    CHECK(s.extras.upscale.sharpness == 0.25);
    CHECK(s.extras.upscale.frame_generation);
    CHECK(s.extras.latency.mode == LatencyMode::Boost);
    CHECK(s.extras.latency.log_timings);
}

NXTEST(extras_defaults_are_written) {
    const std::string text = format_settings(default_settings());
    CHECK(text.find("[hdr]\nmode = auto\nformat = auto\npeak_nits = 0\npaper_white_nits = 0\nhighlight_boost = 1\n") != std::string::npos);
    CHECK(text.find("[upscale]\nmode = off\nquality = quality\nsharpness = 0.8\nscene_scale = 100\nfsr_dll = \ndlss_dll = \nxess_dll = \nframe_generation = 0\n") !=
          std::string::npos);
    CHECK(text.find("[latency]\nmode = off\nframes_ahead = 1\nlog_timings = 0\n") != std::string::npos);
}

NXTEST(extras_full_file_round_trip) {
    Settings s;
    std::string err;
    REQUIRE_MSG(parse_settings(kExtrasFile, &s, &err), err);
    const std::string text = format_settings(s);
    CHECK(text.find("[hdr]\nmode = on\nformat = hdr10\npeak_nits = 1000\npaper_white_nits = 250\nhighlight_boost = 1.5\n") !=
          std::string::npos);
    CHECK(text.find("[upscale]\nmode = fsr1\nquality = quality\nsharpness = 0.5\nscene_scale = 100\nfsr_dll = C:\\Tools\\amd_fidelityfx_vk.dll\n") != std::string::npos);
    CHECK(text.find("xess_dll = \nframe_generation = 1\n") != std::string::npos);
    CHECK(text.find("[latency]\nmode = boost\nframes_ahead = 2\nlog_timings = 1\n") != std::string::npos);

    Settings r;
    REQUIRE_MSG(parse_settings(text, &r, &err), err);
    CHECK(same_settings(s, r));
    CHECK_EQ(format_settings(r), text);
}

NXTEST(extras_fsr3_and_dlss_paths_survive_a_save) {
    Settings s = default_settings();
    s.extras.upscale.mode = UpscaleMode::Fsr3;
    s.extras.upscale.frame_generation = true;
    s.extras.upscale.fsr_dll = "C:\\Program Files\\AMD Tools\\amd_fidelityfx_vk.dll";
    s.extras.upscale.dlss_dll = "D:\\games\\dlss = 3\\nvngx_dlss.dll";   // an '=' in a path: the key ends at the first '='
    Settings r;
    std::string err;
    REQUIRE_MSG(parse_settings(format_settings(s), &r, &err), err);
    CHECK(r.extras.upscale.mode == UpscaleMode::Fsr3);
    CHECK(r.extras.upscale.frame_generation);
    CHECK_EQ(r.extras.upscale.fsr_dll, std::string("C:\\Program Files\\AMD Tools\\amd_fidelityfx_vk.dll"));
    CHECK_EQ(r.extras.upscale.dlss_dll, std::string("D:\\games\\dlss = 3\\nvngx_dlss.dll"));
    CHECK(same_settings(s, r));
}

NXTEST(extras_numbers_round_trip_exactly) {
    Settings s = default_settings();
    s.extras.hdr.mode = HdrMode::On;
    s.extras.hdr.format = HdrFormat::ScRgb;
    s.extras.hdr.peak_nits = 1234.5678;
    s.extras.hdr.paper_white_nits = 203.33333333333334;   // needs 17 digits
    s.extras.hdr.highlight_boost = 0.1;
    s.extras.upscale.sharpness = 1.0 / 3.0;
    s.extras.latency.mode = LatencyMode::Auto;
    s.extras.latency.log_timings = true;
    const std::string text = format_settings(s);
    Settings r;
    std::string err;
    REQUIRE_MSG(parse_settings(text, &r, &err), err);
    CHECK(r.extras.hdr.peak_nits == 1234.5678);
    CHECK(r.extras.hdr.paper_white_nits == 203.33333333333334);
    CHECK(r.extras.hdr.highlight_boost == 0.1);
    CHECK(r.extras.upscale.sharpness == 1.0 / 3.0);
    CHECK(same_settings(s, r));
    CHECK_EQ(format_settings(r), text);
}

NXTEST(extras_sections_written_after_display) {
    const std::string text = format_settings(default_settings());
    const size_t display = text.find("[display]");
    const size_t hdr = text.find("[hdr]");
    const size_t upscale = text.find("[upscale]");
    const size_t latency = text.find("[latency]");
    const size_t sw = text.find("[switch]");
    REQUIRE_MSG(display != std::string::npos && hdr != std::string::npos && upscale != std::string::npos && latency != std::string::npos &&
                    sw != std::string::npos,
                "every section is written");
    CHECK(display < hdr);
    CHECK(hdr < upscale);
    CHECK(upscale < latency);
    CHECK(latency < sw);
}

// ---- button prompts ----------------------------------------------------------------------------------------------------------------

NXTEST(prompt_style_default_is_auto) {
    Settings s;
    CHECK(s.prompts.style == PromptStyle::Auto);
    CHECK(default_settings().prompts.style == PromptStyle::Auto);
}

NXTEST(prompt_style_names) {
    const char* names[] = {"switch", "xbox", "playstation", "keyboard", "auto"};
    for (int i = 0; i < kPromptStyleCount; ++i) {
        const PromptStyle st = static_cast<PromptStyle>(i);
        CHECK(std::string(prompt_style_name(st)) == names[i]);
        PromptStyle back = PromptStyle::Auto;
        REQUIRE_MSG(parse_prompt_style(names[i], &back), names[i]);
        CHECK(back == st);
    }
}

NXTEST(prompt_style_parses_aliases_and_case) {
    struct Case {
        const char* text;
        PromptStyle want;
    };
    const Case cases[] = {
        {"Switch", PromptStyle::Switch},        {"XBOX", PromptStyle::Xbox},        {"PlayStation", PromptStyle::PlayStation},
        {"ps", PromptStyle::PlayStation},       {"PS4", PromptStyle::PlayStation},  {"ps5", PromptStyle::PlayStation},
        {"DualSense", PromptStyle::PlayStation}, {"keyboard", PromptStyle::Keyboard}, {"kbm", PromptStyle::Keyboard},
        {"keyboard_mouse", PromptStyle::Keyboard}, {"keyboard-mouse", PromptStyle::Keyboard}, {"mouse", PromptStyle::Keyboard},
        {"Auto", PromptStyle::Auto},
    };
    for (const Case& c : cases) {
        PromptStyle st = PromptStyle::Switch;
        REQUIRE_MSG(parse_prompt_style(c.text, &st), c.text);
        CHECK(st == c.want);
    }
    PromptStyle st = PromptStyle::Auto;
    CHECK(!parse_prompt_style("banana", &st));
    CHECK(!parse_prompt_style("", &st));
}

NXTEST(prompt_style_settings_key_parses) {
    Settings s;
    std::string err;
    REQUIRE_MSG(parse_settings("[prompts]\nstyle = PS5\n", &s, &err), err);
    CHECK(s.prompts.style == PromptStyle::PlayStation);
}

NXTEST(prompt_style_bad_value_names_the_key) {
    Settings s;
    std::string err;
    CHECK(!parse_settings("[prompts]\nstyle = banana\n", &s, &err));
    CHECK(err.find("style") != std::string::npos);
    CHECK(err.find("line 2") != std::string::npos);
}

NXTEST(prompt_style_round_trip_every_style) {
    for (int i = 0; i < kPromptStyleCount; ++i) {
        Settings s = default_settings();
        s.prompts.style = static_cast<PromptStyle>(i);
        const std::string text = format_settings(s);
        Settings r;
        std::string err;
        REQUIRE_MSG(parse_settings(text, &r, &err), err);
        CHECK(r.prompts.style == s.prompts.style);
        CHECK(same_settings(s, r));
        CHECK_EQ(format_settings(r), text);
    }
}

NXTEST(default_settings_text_has_prompts_section) {
    const std::string text = default_settings_text();
    const size_t at = text.find("[prompts]");
    REQUIRE_MSG(at != std::string::npos, "[prompts] section");
    CHECK(text.find("style = auto\n", at) != std::string::npos);
}

// ---- frame rate counter and pad remap ----------------------------------------------------------------------------------------------

NXTEST(fps_defaults) {
    const Settings s = default_settings();
    CHECK(!s.fps.show);
    CHECK(s.fps.key == key_id(kScancodeF2));
    CHECK_EQ(s.fps.key.code, 59);   // SDL_SCANCODE_F2
    CHECK(s.fps.corner == Corner::TopLeft);
    CHECK_EQ(s.fps.detail, 1);
    CHECK(!s.fps.in_screenshots);
}

NXTEST(padmap_defaults) {
    const Settings s = default_settings();
    for (int i = 0; i < kPadSrcCount; ++i) {
        CHECK(s.pad.remap[i] == kPadDefault);
    }
}

NXTEST(default_settings_text_fps_and_padmap_match_defaults) {
    Settings s;
    std::string err;
    std::vector<std::string> warnings;
    REQUIRE_MSG(parse_settings(default_settings_text(), &s, &err, &warnings), err);
    CHECK(warnings.empty());
    CHECK(!s.fps.show);
    CHECK(s.fps.key == key_id(kScancodeF2));
    CHECK(s.fps.corner == Corner::TopLeft);
    CHECK_EQ(s.fps.detail, 1);
    CHECK(!s.fps.in_screenshots);
    for (int i = 0; i < kPadSrcCount; ++i) {
        CHECK(s.pad.remap[i] == kPadDefault);
    }
}

NXTEST(format_writes_fps_and_padmap_sections) {
    const std::string text = format_settings(default_settings());
    CHECK(text.find("\n[fps]\n") != std::string::npos);
    CHECK(text.find("\n[padmap]\n") != std::string::npos);
    CHECK(text.find("\nsouth = default\n") != std::string::npos);
}

NXTEST(fps_and_padmap_file_round_trip) {
    const std::string file_text = "[fps]\nshow = 1\nkey = Tab\ncorner = bottom_right\ndetail = 2\nin_screenshots = 1\n"
                                  "[padmap]\neast = A\nleft_trigger = ZL+L\nstart = none\n";
    Settings s;
    std::string err;
    std::vector<std::string> warnings;
    REQUIRE_MSG(parse_settings(file_text, &s, &err, &warnings), err);
    CHECK(warnings.empty());
    CHECK(s.fps.show);
    CHECK(s.fps.key == key_id(SDL_SCANCODE_TAB));
    CHECK(s.fps.corner == Corner::BottomRight);
    CHECK_EQ(s.fps.detail, 2);
    CHECK(s.fps.in_screenshots);
    CHECK(s.pad.remap[static_cast<int>(PadSrc::East)] == btn(kBtnA));
    CHECK(s.pad.remap[static_cast<int>(PadSrc::LeftTrigger)] == (btn(kBtnZL) | btn(kBtnL)));
    CHECK(s.pad.remap[static_cast<int>(PadSrc::Start)] == 0u);
    CHECK(s.pad.remap[static_cast<int>(PadSrc::West)] == kPadDefault);

    const std::string text = format_settings(s);
    Settings r;
    REQUIRE_MSG(parse_settings(text, &r, &err), err);
    CHECK(same_settings(s, r));
    CHECK_EQ(format_settings(r), text);
}

NXTEST(fps_key_and_corner_spellings) {
    Settings s;
    std::string err;
    REQUIRE_MSG(parse_settings("[fps]\nkey = f2\ncorner = Top-Right\n", &s, &err), err);
    CHECK(s.fps.key == key_id(kScancodeF2));
    CHECK(s.fps.corner == Corner::TopRight);
}

NXTEST(fps_and_padmap_bad_values_are_errors) {
    struct Case {
        const char* text;
        const char* names;   // a word the message must contain
    };
    const Case cases[] = {
        {"[fps]\ndetail = 3\n", "detail"},
        {"[fps]\nkey = Mouse1\n", "key"},
        {"[fps]\nkey = WheelUp\n", "key"},
        {"[fps]\ncorner = middle\n", "corner"},
        {"[padmap]\nwest = LS_UP\n", "west"},
        {"[padmap]\nwest = Q\n", "'Q'"},
    };
    for (const Case& c : cases) {
        Settings s;
        std::string err;
        CHECK(!parse_settings(c.text, &s, &err));
        CHECK(err.find("line 2") != std::string::npos);
        CHECK(err.find(c.names) != std::string::npos);
    }
}

NXTEST(padmap_unknown_button_warns_only) {
    Settings s;
    std::string err;
    std::vector<std::string> w;
    REQUIRE_MSG(parse_settings("[padmap]\ntriangle = A\n", &s, &err, &w), err);
    REQUIRE_MSG(w.size() == 1u, "one warning");
    CHECK_EQ(w[0], std::string("line 2: unknown key 'triangle' in [padmap]"));
    for (int i = 0; i < kPadSrcCount; ++i) {
        CHECK(s.pad.remap[i] == kPadDefault);
    }
}

NXTEST(file_without_fps_and_padmap_keeps_defaults) {
    Settings s;
    std::string err;
    REQUIRE_MSG(parse_settings("[mouse]\nyaw = 0.001\n[timing]\ntap_ms = 150\n", &s, &err), err);
    CHECK(!s.fps.show);
    CHECK(s.fps.key == key_id(kScancodeF2));
    CHECK(s.fps.corner == Corner::TopLeft);
    CHECK_EQ(s.fps.detail, 1);
    CHECK(!s.fps.in_screenshots);
    for (int i = 0; i < kPadSrcCount; ++i) {
        CHECK(s.pad.remap[i] == kPadDefault);
    }
}

// ---- factory-defaults revision ([meta] defaults_rev, kDefaultAdditions in play_config.cpp) --------------------------------------------

NXTEST(old_file_gets_new_factory_binding_on_map) {
    Settings s;
    std::string err;
    REQUIRE_MSG(parse_settings("[bindings.map]\nReturn = confirm\n", &s, &err), err);
    CHECK(s.migrated);
    CHECK_EQ(s.defaults_rev, 3);
    CHECK(act_at(s, "X", Ctx::Map) == Act::BtnX);
    CHECK(act_at(s, "Return", Ctx::Map) == Act::Confirm);   // the file's own binding is kept
    CHECK_EQ(static_cast<int>(bindings_in(s, static_cast<int>(Ctx::Map)).size()), 2);
}

NXTEST(old_file_keeps_users_own_key_on_map) {
    Settings s;
    std::string err;
    REQUIRE_MSG(parse_settings("[bindings.map]\nX = back\n", &s, &err), err);
    CHECK(s.migrated);
    CHECK(act_at(s, "X", Ctx::Map) == Act::Back);
    CHECK_EQ(static_cast<int>(bindings_in(s, static_cast<int>(Ctx::Map)).size()), 1);
}

NXTEST(file_at_current_revision_is_not_migrated) {
    Settings s;
    std::string err;
    REQUIRE_MSG(parse_settings("[meta]\ndefaults_rev = 3\n[bindings.map]\nReturn = confirm\n", &s, &err), err);
    CHECK(!s.migrated);
    CHECK_EQ(s.defaults_rev, 3);
    CHECK(act_at(s, "X", Ctx::Map) == Act::kCount);   // the user removed X: it stays removed
}

NXTEST(format_round_trip_keeps_defaults_rev) {
    const Settings s = default_settings();
    const std::string text = format_settings(s);
    CHECK(text.find("[meta]\ndefaults_rev = 3\n") != std::string::npos);
    Settings back;
    std::string err;
    REQUIRE_MSG(parse_settings(text, &back, &err), err);
    CHECK_EQ(back.defaults_rev, 3);
    CHECK(!back.migrated);
    CHECK(same_settings(s, back));
}

NXTEST(factory_text_is_at_current_revision) {
    Settings s;
    std::string err;
    REQUIRE_MSG(parse_settings(default_settings_text(), &s, &err), err);
    CHECK_EQ(s.defaults_rev, 3);
    CHECK(!s.migrated);
    CHECK_EQ(default_settings().defaults_rev, 3);
    CHECK(act_at(default_settings(), "X", Ctx::Map) == Act::BtnX);
}

NXTEST(meta_defaults_rev_is_known_and_unknown_meta_key_warns) {
    Settings s;
    std::string err;
    std::vector<std::string> w;
    REQUIRE_MSG(parse_settings("[meta]\ndefaults_rev = 3\n[bindings.all]\nW = fire\n", &s, &err, &w), err);
    CHECK(w.empty());
    std::vector<std::string> w2;
    REQUIRE_MSG(parse_settings("[meta]\nfoo = 1\n", &s, &err, &w2), err);
    REQUIRE_MSG(w2.size() == 1u, "one warning");
    CHECK_EQ(w2[0], std::string("line 2: unknown key 'foo' in [meta]"));
}

NXTEST(file_without_bindings_gets_factory_bindings_at_current_revision) {
    Settings s;
    std::string err;
    REQUIRE_MSG(parse_settings("[mouse]\nyaw = 0.001\n", &s, &err), err);
    CHECK(!s.migrated);
    CHECK_EQ(s.defaults_rev, 3);
    CHECK(act_at(s, "X", Ctx::Map) == Act::BtnX);
}

// ---- controller sticks: right_deadzone, invert_y, look_sens ([pad]) ----------------------------------------------------------------

NXTEST(pad_stick_keys_defaults) {
    const PadSettings d = default_settings().pad;
    CHECK(near(d.stick_deadzone, 0.10));
    CHECK(near(d.right_deadzone, 0.10));
    CHECK(!d.invert_y);
    CHECK(d.look_sens == 1.0);
    CHECK(d.alt_orbit == 2.0);
}

NXTEST(pad_stick_keys_round_trip) {
    Settings s = default_settings();
    s.pad.stick_deadzone = 0.2;
    s.pad.right_deadzone = 0.25;
    s.pad.invert_y = true;
    s.pad.look_sens = 1.5;
    s.pad.alt_orbit = 3.5;
    const std::string text = format_settings(s);
    CHECK(text.find("\ndeadzone = 0.2\nright_deadzone = 0.25\ninvert_y = 1\nlook_sens = 1.5\ntrigger_threshold = ") != std::string::npos);
    Settings r;
    std::string err;
    REQUIRE_MSG(parse_settings(text, &r, &err), err);
    CHECK(near(r.pad.stick_deadzone, 0.2));
    CHECK(near(r.pad.right_deadzone, 0.25));
    CHECK(r.pad.invert_y);
    CHECK(r.pad.look_sens == 1.5);
    CHECK(r.pad.alt_orbit == 3.5);
    CHECK(same_settings(s, r));
    CHECK_EQ(format_settings(r), text);
}

NXTEST(pad_stick_keys_written_in_order) {
    const std::string text = format_settings(default_settings());
    const size_t pad_at = text.find("\n[pad]\n");
    REQUIRE_MSG(pad_at != std::string::npos, "[pad] section");
    const char* order[] = {"\ndeadzone = ", "\nright_deadzone = ", "\ninvert_y = ", "\nlook_sens = ", "\ntrigger_threshold = "};
    size_t at = pad_at;
    for (const char* k : order) {
        const size_t p = text.find(k, at);
        CHECK_MSG(p != std::string::npos, k);
        if (p == std::string::npos) return;
        at = p;
    }
}

NXTEST(pad_stick_keys_range_errors) {
    const struct {
        const char* text;
        const char* expect;
    } cases[] = {
        {"[pad]\nright_deadzone = 0.6\n", "line 2: right_deadzone must be 0..0.5, got 0.6"},
        {"[pad]\nright_deadzone = -0.1\n", "line 2: right_deadzone must be 0..0.5, got -0.1"},
        {"[pad]\nright_deadzone = abc\n", "line 2: right_deadzone must be a number"},
        {"[pad]\nlook_sens = 0.2\n", "line 2: look_sens must be 0.25..3, got 0.2"},
        {"[pad]\nlook_sens = 3.5\n", "line 2: look_sens must be 0.25..3, got 3.5"},
        {"[pad]\nlook_sens = fast\n", "line 2: look_sens must be a number"},
        {"[pad]\ninvert_y = maybe\n", "line 2: invert_y must be 1/0"},
        {"[pad]\nball_orbit = -1\n", "line 2: ball_orbit must be 0..6, got -1"},
        {"[pad]\nball_orbit = 7\n", "line 2: ball_orbit must be 0..6, got 7"},
    };
    for (const auto& c : cases) {
        Settings s;
        std::string err;
        CHECK(!parse_settings(c.text, &s, &err));
        CHECK_MSG(starts_with(err, c.expect), err);
    }
}

NXTEST(pad_stick_keys_boundaries_accepted) {
    Settings s;
    std::string err;
    REQUIRE_MSG(parse_settings("[pad]\nright_deadzone = 0\nlook_sens = 0.25\ninvert_y = yes\n", &s, &err), err);
    CHECK(s.pad.right_deadzone == 0.0);
    CHECK(s.pad.look_sens == 0.25);
    CHECK(s.pad.invert_y);
    REQUIRE_MSG(parse_settings("[pad]\nright_deadzone = 0.5\nlook_sens = 3\ninvert_y = 0\n", &s, &err), err);
    CHECK(s.pad.right_deadzone == 0.5);
    CHECK(s.pad.look_sens == 3.0);
    CHECK(!s.pad.invert_y);
}

NXTEST(pad_right_deadzone_follows_old_deadzone) {
    // A file with deadzone but no right_deadzone was written before the right stick had its own dead zone: the right stick takes deadzone's value.
    Settings s;
    std::string err;
    std::vector<std::string> w;
    REQUIRE_MSG(parse_settings("[pad]\ndeadzone = 0.3\n", &s, &err, &w), err);
    CHECK(w.empty());
    CHECK(near(s.pad.stick_deadzone, 0.3));
    CHECK(near(s.pad.right_deadzone, 0.3));

    // A zero deadzone is copied as well, and the next save writes both keys.
    REQUIRE_MSG(parse_settings("[pad]\ndeadzone = 0\n", &s, &err), err);
    CHECK(s.pad.right_deadzone == 0.0);
    CHECK(format_settings(s).find("\ndeadzone = 0\nright_deadzone = 0\n") != std::string::npos);
}

NXTEST(pad_right_deadzone_explicit_wins_over_deadzone) {
    Settings s;
    std::string err;
    REQUIRE_MSG(parse_settings("[pad]\ndeadzone = 0.3\nright_deadzone = 0.05\n", &s, &err), err);
    CHECK(near(s.pad.stick_deadzone, 0.3));
    CHECK(near(s.pad.right_deadzone, 0.05));
    // The order of the two lines does not matter.
    REQUIRE_MSG(parse_settings("[pad]\nright_deadzone = 0.05\ndeadzone = 0.3\n", &s, &err), err);
    CHECK(near(s.pad.stick_deadzone, 0.3));
    CHECK(near(s.pad.right_deadzone, 0.05));
}

NXTEST(pad_right_deadzone_default_kept_without_pad_deadzone) {
    // No deadzone in [pad]: the right dead zone keeps its default.
    Settings s;
    std::string err;
    REQUIRE_MSG(parse_settings("[mouse]\nyaw = 0.001\n", &s, &err), err);
    CHECK(near(s.pad.right_deadzone, 0.10));
}

// ---- the alternate body's camera (title E: the morph ball camera, S48): [mouse] alt_camera, alt_sensitivity ------------------------------------

NXTEST(alt_camera_defaults_on) {
    const MouseSettings d = default_settings().mouse;
    CHECK(d.alt_camera);
    CHECK(d.alt_yaw_rad == d.alt_pitch_rad);
    Settings s;
    std::string err;
    REQUIRE_MSG(parse_settings("[mouse]\nyaw = 0.001\n", &s, &err), err);   // a file without the keys keeps the camera on
    CHECK(s.mouse.alt_camera);
}

NXTEST(alt_camera_off_parses_and_round_trips) {
    Settings s;
    std::string err;
    REQUIRE_MSG(parse_settings(std::string("[mouse]\n") + key_alt_camera() + " = 0\n", &s, &err), err);
    CHECK(!s.mouse.alt_camera);
    const std::string text = format_settings(s);
    CHECK(text.find(std::string("\n") + key_alt_camera() + " = 0\n") != std::string::npos);
    Settings r;
    REQUIRE_MSG(parse_settings(text, &r, &err), err);
    CHECK(!r.mouse.alt_camera);
    CHECK_EQ(format_settings(r), text);
    REQUIRE(!parse_settings(std::string("[mouse]\n") + key_alt_camera() + " = maybe\n", &r, &err));
}

NXTEST(alt_sensitivity_is_source_scale_for_both) {
    Settings s;
    std::string err;
    REQUIRE_MSG(parse_settings(std::string("[mouse]\n") + key_alt_sens() + " = 3.5\n", &s, &err), err);
    CHECK(near(s.mouse.alt_yaw_rad, 3.5 * 0.022 * kPi / 180.0));
    CHECK(s.mouse.alt_pitch_rad == s.mouse.alt_yaw_rad);
    CHECK(near(s.mouse.yaw_rad, default_settings().mouse.yaw_rad));    // the aim keeps its own
    const std::string text = format_settings(s);
    CHECK(text.find(std::string("\n") + key_alt_sens() + " = 3.5\n") != std::string::npos);
    CHECK(text.find(std::string("\n") + key_alt_pitch() + " = ") == std::string::npos);   // the same for both: one key
    Settings r;
    REQUIRE_MSG(parse_settings(text, &r, &err), err);
    CHECK(std::fabs(r.mouse.alt_yaw_rad - s.mouse.alt_yaw_rad) < 1e-11);
    CHECK_EQ(format_settings(r), text);
    REQUIRE(!parse_settings(std::string("[mouse]\n") + key_alt_sens() + " = fast\n", &r, &err));
}

NXTEST(alt_sensitivity_keeps_a_separate_vertical) {
    // the older radians-per-count keys still read; a vertical of its own is written after the pair and survives the round trip
    Settings s;
    std::string err;
    REQUIRE_MSG(parse_settings(std::string("[mouse]\n") + key_alt_yaw() + " = 0.001\n" + key_alt_pitch() + " = 0.0005\n", &s, &err), err);
    CHECK(s.mouse.alt_yaw_rad == 0.001);
    CHECK(s.mouse.alt_pitch_rad == 0.0005);
    const std::string text = format_settings(s);
    const size_t sens_at = text.find(std::string("\n") + key_alt_sens() + " = ");
    const size_t pitch_at = text.find(std::string("\n") + key_alt_pitch() + " = 0.0005\n");
    REQUIRE(sens_at != std::string::npos && pitch_at != std::string::npos);
    CHECK(sens_at < pitch_at);
    Settings r;
    REQUIRE_MSG(parse_settings(text, &r, &err), err);
    CHECK(std::fabs(r.mouse.alt_yaw_rad - 0.001) < 1e-11);   // written on the Source scale with 9 digits
    CHECK(r.mouse.alt_pitch_rad == 0.0005);
}

NXTEST(ball_camera_keys_with_the_mpr_vocabulary) {
    // the group's fixture is title E's vocabulary: its words for the alternate body's camera keys
    REQUIRE(std::string(key_alt_camera()) == "ball_camera");
    REQUIRE(std::string(key_alt_sens()) == "ball_sensitivity");
    REQUIRE(std::string(key_alt_yaw()) == "morph_yaw");
    REQUIRE(std::string(key_alt_pitch()) == "morph_pitch");
    Settings s;
    std::string err;
    REQUIRE_MSG(parse_settings("[mouse]\nball_camera = 0\nball_sensitivity = 4\n", &s, &err), err);
    CHECK(!s.mouse.alt_camera);
    CHECK(near(s.mouse.alt_yaw_rad, rad_per_count(4.0)) && s.mouse.alt_pitch_rad == s.mouse.alt_yaw_rad);
    const std::string text = format_settings(s);
    CHECK(text.find("\nball_camera = 0\nball_sensitivity = 4\n") != std::string::npos);
    CHECK(text.find("morph_yaw") == std::string::npos && text.find("morph_pitch") == std::string::npos);
    // older files: morph_yaw and morph_pitch in radians per count
    REQUIRE_MSG(parse_settings("[mouse]\nmorph_yaw = 0.002\nmorph_pitch = 0.001\n", &s, &err), err);
    CHECK(s.mouse.alt_yaw_rad == 0.002 && s.mouse.alt_pitch_rad == 0.001);
    CHECK(s.mouse.alt_camera);
    // morph_pitch before ball_sensitivity: the later key wins, so both take the sensitivity
    REQUIRE_MSG(parse_settings("[mouse]\nmorph_pitch = 0.001\nball_sensitivity = 3\n", &s, &err), err);
    CHECK(near(s.mouse.alt_pitch_rad, rad_per_count(3.0)) && s.mouse.alt_yaw_rad == s.mouse.alt_pitch_rad);
    // after it: the vertical keeps its own
    REQUIRE_MSG(parse_settings("[mouse]\nball_sensitivity = 3\nmorph_pitch = 0.001\n", &s, &err), err);
    CHECK(near(s.mouse.alt_yaw_rad, rad_per_count(3.0)) && s.mouse.alt_pitch_rad == 0.001);
}

NXTEST(ball_sensitivity_range) {
    Settings s;
    std::string err;
    for (const char* bad : {"0", "-1", "0.05", "25", "1e9"})
        CHECK(!parse_settings(std::string("[mouse]\nball_sensitivity = ") + bad + "\n", &s, &err));
    for (const char* bad : {"0", "-0.001", "1"}) {
        CHECK(!parse_settings(std::string("[mouse]\nmorph_yaw = ") + bad + "\n", &s, &err));
        CHECK(!parse_settings(std::string("[mouse]\nmorph_pitch = ") + bad + "\n", &s, &err));
    }
    for (const char* ok : {"0.1", "2", "20"}) REQUIRE_MSG(parse_settings(std::string("[mouse]\nball_sensitivity = ") + ok + "\n", &s, &err), err);
}

// ---- the F1 menu's keys: [display] upscaler_replace, [menu], [config] ---------------------------------------------------------------------

NXTEST(menu_and_config_sections_round_trip) {
    Settings s = default_settings();
    CHECK(s.display.upscaler_replace);
    CHECK(s.menu.auto_apply);
    CHECK(s.config.empty());
    s.display.upscaler_replace = false;
    s.menu.auto_apply = false;
    s.config["am.operation_mode"] = "handheld";
    s.config["gfx.device_diagnostics"] = "1";
    s.config["am.resolution"] = "";   // an empty value is kept (it sets the key to the empty string)
    const std::string text = format_settings(s);
    CHECK(text.find("\nupscaler_replace = 0\n") != std::string::npos);
    CHECK(text.find("\n[menu]\nauto_apply = 0\n") != std::string::npos);
    CHECK(text.find("\n[config]\n") != std::string::npos);
    CHECK(text.find("\nam.operation_mode = handheld\n") != std::string::npos);
    Settings back;
    std::string err;
    REQUIRE_MSG(parse_settings(text, &back, &err), err);
    CHECK(!back.display.upscaler_replace);
    CHECK(!back.menu.auto_apply);
    CHECK_EQ(back.config.size(), static_cast<size_t>(3));
    CHECK(back.config["am.operation_mode"] == "handheld");
    CHECK(back.config["gfx.device_diagnostics"] == "1");
    CHECK(back.config.count("am.resolution") == 1 && back.config["am.resolution"].empty());
    CHECK(format_settings(back) == text);
}

NXTEST(config_section_rejects_names_that_are_not_section_dot_key) {
    Settings s;
    std::string err;
    for (const char* bad : {"operation_mode", ".mode", "am.", "am.operation.mode", "am.Mode!", "a-b.c"}) {
        CHECK(!parse_settings(std::string("[config]\n") + bad + " = 1\n", &s, &err));
        CHECK(!err.empty());
    }
    // section and key names are case-insensitive in the file: kept in lower case
    REQUIRE_MSG(parse_settings("[config]\nGFX.Device_Diagnostics = 1\n", &s, &err), err);
    CHECK(s.config.count("gfx.device_diagnostics") == 1);
    CHECK(config_entry_name_ok("diag.hitch_ms"));
    CHECK(!config_entry_name_ok("diag"));
}

NXTEST(older_file_without_menu_keys_keeps_the_defaults) {
    Settings s;
    std::string err;
    REQUIRE_MSG(parse_settings("[display]\nvsync = 1\n", &s, &err), err);
    CHECK(s.display.upscaler_replace);
    CHECK(s.menu.auto_apply);
    CHECK(s.config.empty());
}
