// SPDX-License-Identifier: GPL-3.0-or-later
// Play mode binding engine (play_engine.cpp): the rules in play_engine.h, checked with made-up clocks. No SDL window, no game memory.
#include <SDL3/SDL_scancode.h>

#include <cmath>
#include <cstdint>
#include <string>

#include "nxrt/input_source.h"
#include "play/play_engine.h"
#include "play/play_types.h"
#include "test_framework.h"
#include "play_fixture/mpr_play_fixture.h"
NXTEST_GROUP(play_engine)
// The checks are written against Metroid Prime Remastered's play vocabulary and factory controls.ini (play_fixture/mpr_play_fixture.h).
NXTEST_GROUP_FIXTURE(nxtest::mpr_play::install, nxtest::mpr_play::remove)

namespace {

namespace hid = nxrt::hid;
namespace play = nxrt::play;
using play::Act;
using play::Ctx;
using play::Engine;
using play::EngineOut;
using play::Settings;

// Nanoseconds from milliseconds.
constexpr uint64_t ms(uint64_t v) { return v * 1000000ull; }

// The [timing] defaults (play_types.h), put in front of every test settings text.
const char* const kTiming = "[timing]\ntap_ms = 200\nhold_ms = 250\ndouble_ms = 300\npulse_ms = 80\n";

// Parses kTiming plus the given body. A parse error fails the test.
Settings settings_from(const std::string& body) {
    Settings s;
    std::string err;
    const bool ok = play::parse_settings(std::string(kTiming) + body, &s, &err);
    CHECK_MSG(ok, "parse_settings: " + err);
    return s;
}

// Switches an engine (which starts in the menu context) to gameplay at t = 1000 ms.
void to_gameplay(Engine& e) { e.set_context(Ctx::Play, ms(1000)); }

bool has(const EngineOut& o, hid::NpadButtonBit b) { return (o.buttons & hid::btn(b)) != 0; }

bool idle(const EngineOut& o) { return o.buttons == 0 && o.lx == 0 && o.ly == 0 && o.rx == 0 && o.ry == 0; }

bool close_to(double a, double b, double eps = 1e-3) { return std::fabs(a - b) <= eps; }

int bit_count(uint64_t x) {
    int n = 0;
    for (; x; x &= x - 1) ++n;
    return n;
}

uint64_t dpad_mask() {
    return hid::btn(hid::kBtnUp) | hid::btn(hid::kBtnDown) | hid::btn(hid::kBtnLeft) | hid::btn(hid::kBtnRight);
}

play::InputId key_in(int scancode) {
    play::InputId id;
    id.kind = play::InKind::Key;
    id.code = scancode;
    return id;
}

// Layers used by the context tests: Mouse1 is fire in gameplay and confirm in the menu (menu_cursor off), W is in `all` only,
// Left Ctrl is morph in `all` and switched off in the menu.
const char* const kLayers =
    "[mouse]\nmenu_cursor = 0\n"
    "[bindings.all]\nMouse1 = fire\nW = move_forward\nLeft Ctrl = morph\n"
    "[bindings.menu]\nMouse1 = confirm\nLeft Ctrl = none\n";

}  // namespace

NXTEST(press_key_and_mouse) {
    Engine e(settings_from("[bindings.all]\nSpace = jump\nMouse1 = fire\n"));
    to_gameplay(e);
    e.key(SDL_SCANCODE_SPACE, true, ms(1100));
    CHECK(has(e.output(ms(1150)), hid::kBtnB));
    e.key(SDL_SCANCODE_SPACE, false, ms(1200));
    CHECK(!has(e.output(ms(1250)), hid::kBtnB));
    e.mouse_button(1, true, ms(1300));
    CHECK(has(e.output(ms(1350)), hid::kBtnZR));
    e.mouse_button(1, false, ms(1400));
    CHECK(!has(e.output(ms(1450)), hid::kBtnZR));
}

NXTEST(tap_pulses_after_quick_release) {
    Engine e(settings_from("[bindings.all]\nF = missile tap\n"));
    to_gameplay(e);
    e.key(SDL_SCANCODE_F, true, ms(1000));
    CHECK(!has(e.output(ms(1050)), hid::kBtnR));   // while held a tap is not active
    e.key(SDL_SCANCODE_F, false, ms(1100));       // 100 ms press: a tap
    CHECK(has(e.output(ms(1140)), hid::kBtnR));    // release + 40 ms
    CHECK(!has(e.output(ms(1200)), hid::kBtnR));   // release + 100 ms

    // A long press (400 ms, beyond the 200 ms tap window) never pulses.
    e.key(SDL_SCANCODE_F, true, ms(2000));
    CHECK(!has(e.output(ms(2300)), hid::kBtnR));
    e.key(SDL_SCANCODE_F, false, ms(2400));
    CHECK(!has(e.output(ms(2410)), hid::kBtnR));
    CHECK(!has(e.output(ms(2450)), hid::kBtnR));
}

NXTEST(hold_starts_after_delay) {
    Engine e(settings_from("[bindings.all]\nLeft Ctrl = morph hold\n"));
    to_gameplay(e);   // default hold delay 250 ms
    e.key(SDL_SCANCODE_LCTRL, true, ms(2000));
    CHECK(!has(e.output(ms(2100)), hid::kBtnY));
    CHECK(has(e.output(ms(2300)), hid::kBtnY));
    e.key(SDL_SCANCODE_LCTRL, false, ms(2400));
    CHECK(!has(e.output(ms(2410)), hid::kBtnY));

    // A press released before the delay never activates the hold.
    e.key(SDL_SCANCODE_LCTRL, true, ms(3000));
    e.key(SDL_SCANCODE_LCTRL, false, ms(3100));
    CHECK(!has(e.output(ms(3200)), hid::kBtnY));
}

NXTEST(hold_custom_delay) {
    Engine e(settings_from("[bindings.all]\nLeft Ctrl = morph hold 500\n"));
    to_gameplay(e);
    e.key(SDL_SCANCODE_LCTRL, true, ms(3000));
    CHECK(!has(e.output(ms(3300)), hid::kBtnY));   // 300 ms: still before the 500 ms delay
    CHECK(has(e.output(ms(3600)), hid::kBtnY));    // 600 ms: active
    e.key(SDL_SCANCODE_LCTRL, false, ms(3700));
}

NXTEST(toggle_flips_per_press) {
    Engine e(settings_from("[bindings.all]\nSpace = jump toggle\n"));
    to_gameplay(e);
    e.key(SDL_SCANCODE_SPACE, true, ms(1000));
    CHECK(has(e.output(ms(1010)), hid::kBtnB));    // first press: on
    e.key(SDL_SCANCODE_SPACE, false, ms(1100));
    CHECK(has(e.output(ms(1200)), hid::kBtnB));    // stays on after release
    e.key(SDL_SCANCODE_SPACE, true, ms(1300));
    CHECK(!has(e.output(ms(1310)), hid::kBtnB));   // second press: off
    e.key(SDL_SCANCODE_SPACE, false, ms(1400));
    CHECK(!has(e.output(ms(1500)), hid::kBtnB));

    // Key repeat: a second key-down without a release does not flip it again.
    e.key(SDL_SCANCODE_SPACE, true, ms(2000));
    e.key(SDL_SCANCODE_SPACE, true, ms(2100));
    CHECK(has(e.output(ms(2200)), hid::kBtnB));
    e.key(SDL_SCANCODE_SPACE, false, ms(2300));
    CHECK(has(e.output(ms(2400)), hid::kBtnB));
}

NXTEST(doubletap_active_only_on_quick_second_press) {
    Engine e(settings_from("[bindings.all]\nQ = lock_on doubletap\n"));
    to_gameplay(e);
    // A single press does nothing, before or after its release.
    e.key(SDL_SCANCODE_Q, true, ms(1000));
    CHECK(!has(e.output(ms(1050)), hid::kBtnZL));
    e.key(SDL_SCANCODE_Q, false, ms(1100));
    CHECK(!has(e.output(ms(1150)), hid::kBtnZL));

    // Second press 100 ms after the release: active while held, off after release.
    e.key(SDL_SCANCODE_Q, true, ms(1200));
    CHECK(has(e.output(ms(1250)), hid::kBtnZL));
    e.key(SDL_SCANCODE_Q, false, ms(1300));
    CHECK(!has(e.output(ms(1350)), hid::kBtnZL));

    // Second press 500 ms after the release (window is 300 ms): nothing.
    e.key(SDL_SCANCODE_Q, true, ms(2000));
    e.key(SDL_SCANCODE_Q, false, ms(2100));
    e.key(SDL_SCANCODE_Q, true, ms(2600));
    CHECK(!has(e.output(ms(2650)), hid::kBtnZL));
    e.key(SDL_SCANCODE_Q, false, ms(2700));
}

NXTEST(wheel_notch_pulses_beam_select) {
    Engine e(settings_from("[bindings.all]\nWheelUp = beam_prev\nWheelDown = beam_next\n"));
    to_gameplay(e);
    e.wheel(-1, ms(1000));   // WheelDown = beam_next: tracked beam 0 -> 1 (wave, X+RIGHT)
    const uint64_t want = hid::btn(hid::kBtnX) | hid::btn(hid::kBtnRight);
    const EngineOut at40 = e.output(ms(1040));
    CHECK(at40.buttons == want);
    const EngineOut at100 = e.output(ms(1100));
    CHECK(at100.buttons == 0);
}

NXTEST(wheel_notches_are_serialised) {
    Engine e(settings_from("[bindings.all]\nWheelUp = beam_prev\nWheelDown = beam_next\n"));
    to_gameplay(e);
    e.wheel(-1, ms(2000));   // beam 0 -> 1 (X+RIGHT)
    e.wheel(-1, ms(2010));   // beam 1 -> 2 (X+DOWN), 10 ms later
    uint64_t seen = 0;
    bool together = false;
    for (uint64_t t = 2000; t <= 2400; t += 5) {
        const EngineOut o = e.output(ms(t));
        const uint64_t dirs = o.buttons & dpad_mask();
        if (bit_count(dirs) > 1) together = true;
        seen |= dirs;
    }
    CHECK_MSG(!together, "two beam combinations were on the pad at the same instant");
    CHECK_MSG((seen & hid::btn(hid::kBtnRight)) != 0, "the first notch's select (X+RIGHT) never appeared");
    CHECK_MSG((seen & hid::btn(hid::kBtnDown)) != 0, "the second notch's select (X+DOWN) never appeared");
}

NXTEST(wheel_non_select_action_is_pulse) {
    Engine e(settings_from("[bindings.all]\nWheelUp = dpad_up\n"));
    to_gameplay(e);
    e.wheel(1, ms(3000));
    CHECK(has(e.output(ms(3040)), hid::kBtnUp));
    CHECK(!has(e.output(ms(3100)), hid::kBtnUp));
}

NXTEST(beam_key_pulses_select_only) {
    Engine e(settings_from("[bindings.all]\n1 = beam_1\n2 = beam_2\n3 = beam_3\n4 = beam_4\nE = beam_next\nR = beam_prev\n"));
    to_gameplay(e);
    // Beam 4 (plasma, X+LEFT), a quick press: tracked beam becomes 3.
    e.key(SDL_SCANCODE_4, true, ms(1000));
    CHECK(e.output(ms(1040)).buttons == (hid::btn(hid::kBtnX) | hid::btn(hid::kBtnLeft)));
    e.key(SDL_SCANCODE_4, false, ms(1100));
    CHECK(e.tracked_cycle() == 3);
    CHECK(idle(e.output(ms(1150))));

    // Beam 1 (power, X+UP) held for one second: the select pulse is 80 ms long, the hold does not extend it.
    e.key(SDL_SCANCODE_1, true, ms(1300));
    CHECK(e.output(ms(1340)).buttons == (hid::btn(hid::kBtnX) | hid::btn(hid::kBtnUp)));
    CHECK(idle(e.output(ms(1500))));
    e.key(SDL_SCANCODE_1, false, ms(2300));
    CHECK(e.tracked_cycle() == 0);

    // beam_next cycles 0 -> 1 -> 2 -> 3 -> 0 over the owned beams (all four by default).
    e.key(SDL_SCANCODE_E, true, ms(3000));
    e.key(SDL_SCANCODE_E, false, ms(3050));
    CHECK(e.tracked_cycle() == 1);
    e.key(SDL_SCANCODE_E, true, ms(3100));
    e.key(SDL_SCANCODE_E, false, ms(3150));
    CHECK(e.tracked_cycle() == 2);
    e.key(SDL_SCANCODE_E, true, ms(3200));
    e.key(SDL_SCANCODE_E, false, ms(3250));
    CHECK(e.tracked_cycle() == 3);
    e.key(SDL_SCANCODE_E, true, ms(3300));
    e.key(SDL_SCANCODE_E, false, ms(3350));
    CHECK(e.tracked_cycle() == 0);
}

NXTEST(beam_cycle_skips_unowned_beams) {
    Engine e(settings_from("[bindings.all]\nE = beam_next\nR = beam_prev\n"));
    to_gameplay(e);
    e.set_cycle_hint(2, 0b0101, ms(0));   // owns beams 0 and 2 only; currently on beam 2
    CHECK(e.tracked_cycle() == 2);
    e.key(SDL_SCANCODE_E, true, ms(1000));   // 2 -> 3 is not owned, so -> 0
    e.key(SDL_SCANCODE_E, false, ms(1050));
    CHECK(e.tracked_cycle() == 0);
    e.key(SDL_SCANCODE_R, true, ms(1200));   // 0 -> 3 is not owned, so -> 2
    e.key(SDL_SCANCODE_R, false, ms(1250));
    CHECK(e.tracked_cycle() == 2);
}

NXTEST(context_layers_win_over_all) {
    Engine e(settings_from(kLayers));
    e.set_context(Ctx::Play, ms(1000));
    e.mouse_button(1, true, ms(1100));
    CHECK(has(e.output(ms(1150)), hid::kBtnZR));   // gameplay: fire from [bindings.all]
    e.mouse_button(1, false, ms(1200));

    e.set_context(Ctx::Menu, ms(1300));
    e.mouse_button(1, true, ms(1400));
    const EngineOut menu_click = e.output(ms(1450));
    CHECK(has(menu_click, hid::kBtnA));            // menu: confirm from [bindings.menu]
    CHECK(!has(menu_click, hid::kBtnZR));
    e.mouse_button(1, false, ms(1500));

    e.key(SDL_SCANCODE_W, true, ms(1600));          // W is only in `all`, still works in the menu
    CHECK(close_to(e.output(ms(1650)).ly, 1.0));
    e.key(SDL_SCANCODE_W, false, ms(1700));

    e.key(SDL_SCANCODE_LCTRL, true, ms(1800));      // `none` in the menu switches Left Ctrl off
    CHECK(idle(e.output(ms(1850))));
    e.key(SDL_SCANCODE_LCTRL, false, ms(1900));

    e.set_context(Ctx::Play, ms(2000));
    e.key(SDL_SCANCODE_LCTRL, true, ms(2100));      // back in gameplay: morph (Y)
    CHECK(has(e.output(ms(2150)), hid::kBtnY));
    e.key(SDL_SCANCODE_LCTRL, false, ms(2200));
}

NXTEST(context_switch_consumes_held_inputs) {
    Engine e(settings_from(kLayers));
    e.set_context(Ctx::Play, ms(1000));
    e.mouse_button(1, true, ms(1100));
    CHECK(has(e.output(ms(1150)), hid::kBtnZR));

    e.set_context(Ctx::Menu, ms(1200));             // Mouse1 is held, and its binding differs in the menu
    const EngineOut o = e.output(ms(1250));
    CHECK(!has(o, hid::kBtnZR));
    CHECK(!has(o, hid::kBtnA));                     // consumed: the menu confirm does not fire while held

    e.mouse_button(1, false, ms(1300));
    e.mouse_button(1, true, ms(1400));              // a new press in the menu confirms
    CHECK(has(e.output(ms(1450)), hid::kBtnA));
    e.mouse_button(1, false, ms(1500));
}

NXTEST(context_switch_keeps_identical_binding) {
    Engine e(settings_from(kLayers));
    e.set_context(Ctx::Play, ms(1000));
    e.key(SDL_SCANCODE_W, true, ms(1100));
    CHECK(close_to(e.output(ms(1150)).ly, 1.0));
    e.set_context(Ctx::Menu, ms(1200));             // W binds move_forward in both contexts: no consumption
    CHECK(close_to(e.output(ms(1250)).ly, 1.0));
    e.key(SDL_SCANCODE_W, false, ms(1300));
}

NXTEST(menu_cursor_owns_left_click) {
    // Default menu_cursor = 1: in the menu the left click belongs to the pointer, so a binding of Mouse1 gets nothing there.
    Engine e(settings_from("[bindings.all]\nMouse1 = fire\n"));
    CHECK(e.context() == Ctx::Menu);               // engines start in the menu context
    e.mouse_button(1, true, ms(1000));
    CHECK(idle(e.output(ms(1050))));
    e.mouse_button(1, false, ms(1100));

    to_gameplay(e);                                 // in gameplay the same click still fires
    e.mouse_button(1, true, ms(1300));
    CHECK(has(e.output(ms(1350)), hid::kBtnZR));
    e.mouse_button(1, false, ms(1400));
}

NXTEST(stick_diagonal_is_normalised) {
    Engine e(settings_from("[bindings.all]\nW = move_forward\nD = move_right\nS = move_back\n"));
    to_gameplay(e);
    e.key(SDL_SCANCODE_W, true, ms(1000));
    e.key(SDL_SCANCODE_D, true, ms(1000));
    const EngineOut diag = e.output(ms(1050));
    CHECK(close_to(diag.lx, 0.7071));
    CHECK(close_to(diag.ly, 0.7071));
    e.key(SDL_SCANCODE_W, false, ms(1100));
    e.key(SDL_SCANCODE_D, false, ms(1100));

    // W and S together cancel.
    e.key(SDL_SCANCODE_W, true, ms(2000));
    e.key(SDL_SCANCODE_S, true, ms(2000));
    const EngineOut cancel = e.output(ms(2050));
    CHECK(close_to(cancel.lx, 0.0));
    CHECK(close_to(cancel.ly, 0.0));
    e.key(SDL_SCANCODE_S, false, ms(2100));
    e.key(SDL_SCANCODE_W, false, ms(2100));

    // A single W is full deflection on y.
    e.key(SDL_SCANCODE_W, true, ms(3000));
    const EngineOut single = e.output(ms(3050));
    CHECK(close_to(single.ly, 1.0));
    CHECK(close_to(single.lx, 0.0));
    e.key(SDL_SCANCODE_W, false, ms(3100));
}

NXTEST(release_all_clears_everything) {
    Engine e(settings_from("[bindings.all]\nW = move_forward\nMouse1 = fire\nF = missile tap\n"));
    to_gameplay(e);
    e.key(SDL_SCANCODE_W, true, ms(1000));
    e.mouse_button(1, true, ms(1000));
    e.key(SDL_SCANCODE_F, true, ms(1100));
    e.key(SDL_SCANCODE_F, false, ms(1150));          // a tap, pulse pending until 1230
    e.release_all(ms(1160));

    CHECK(idle(e.output(ms(1170))));
    CHECK(idle(e.output(ms(1240))));                 // the pending tap is gone as well (no taps fire)
    CHECK(!e.input_held(key_in(SDL_SCANCODE_W)));

    e.key(SDL_SCANCODE_W, true, ms(1300));           // a later press works again
    CHECK(close_to(e.output(ms(1350)).ly, 1.0));
    e.mouse_button(1, true, ms(1400));
    CHECK(has(e.output(ms(1450)), hid::kBtnZR));
}

NXTEST(set_settings_consumes_rebound_held_input) {
    Engine e(settings_from("[bindings.all]\nMouse1 = fire\nW = move_forward\n"));
    to_gameplay(e);
    e.mouse_button(1, true, ms(1100));
    e.key(SDL_SCANCODE_W, true, ms(1100));
    CHECK(has(e.output(ms(1150)), hid::kBtnZR));

    e.set_settings(settings_from("[bindings.all]\nMouse1 = jump\nW = move_forward\n"), ms(1200));
    const EngineOut o = e.output(ms(1250));
    CHECK(!has(o, hid::kBtnZR));                     // Mouse1 changed binding: consumed while held
    CHECK(!has(o, hid::kBtnB));
    CHECK(close_to(o.ly, 1.0));                      // W unchanged: keeps working

    e.mouse_button(1, false, ms(1300));
    e.mouse_button(1, true, ms(1400));               // released and pressed again: the new binding
    CHECK(has(e.output(ms(1450)), hid::kBtnB));
    e.mouse_button(1, false, ms(1500));
    e.key(SDL_SCANCODE_W, false, ms(1500));
}

// A binding added in front of the held key's binding moves that binding's index in Settings::bindings, but the held key's own bindings did
// not change: the key stays live. play_engine.cpp compares what the bindings are (effective_sig), not their indices, which consumed it once.
NXTEST(set_settings_keeps_unrelated_held_key_when_bindings_shift) {
    Engine e(settings_from("[bindings.all]\nMouse1 = fire\nW = move_forward\n"));
    to_gameplay(e);
    e.key(SDL_SCANCODE_W, true, ms(1100));
    CHECK(close_to(e.output(ms(1150)).ly, 1.0));

    e.set_settings(settings_from("[bindings.all]\nQ = lock_on\nMouse1 = fire\nW = move_forward\n"), ms(1200));
    CHECK_MSG(close_to(e.output(ms(1250)).ly, 1.0), "W is still held and its binding did not change, but it was consumed");
    e.key(SDL_SCANCODE_W, false, ms(1300));
}

NXTEST(activations_count_edges) {
    Engine e(settings_from("[bindings.all]\nMouse1 = fire\nF = missile tap\n"));
    to_gameplay(e);
    CHECK(e.activations(Act::Primary) == 0);
    e.mouse_button(1, true, ms(1100));
    e.mouse_button(1, true, ms(1150));               // repeat while held: not an edge
    e.output(ms(1200));
    CHECK(e.activations(Act::Primary) == 1);
    e.mouse_button(1, false, ms(1300));
    e.mouse_button(1, true, ms(1400));               // a second press: a second edge
    e.output(ms(1450));
    CHECK(e.activations(Act::Primary) == 2);
    CHECK(e.last_activated() == Act::Primary);
    e.mouse_button(1, false, ms(1500));

    e.key(SDL_SCANCODE_F, true, ms(1600));
    e.key(SDL_SCANCODE_F, false, ms(1650));          // the tap activates when its pulse starts
    CHECK(e.activations(Act::Secondary) == 1);
    CHECK(e.last_activated() == Act::Secondary);
}

NXTEST(default_settings_smoke) {
    Engine e(play::default_settings());
    CHECK(e.context() == Ctx::Menu);

    e.set_context(Ctx::Play, ms(1000));
    e.key(SDL_SCANCODE_W, true, ms(1100));
    CHECK(close_to(e.output(ms(1150)).ly, 1.0));
    e.key(SDL_SCANCODE_W, false, ms(1200));

    e.mouse_button(1, true, ms(1300));
    CHECK(has(e.output(ms(1350)), hid::kBtnZR));     // Mouse1 = fire
    e.mouse_button(1, false, ms(1400));

    e.mouse_button(3, true, ms(1500));
    CHECK(has(e.output(ms(1550)), hid::kBtnZL));     // Mouse3 = lock_on
    e.mouse_button(3, false, ms(1600));

    e.key(SDL_SCANCODE_F, true, ms(1700));
    CHECK(has(e.output(ms(1750)), hid::kBtnR));      // F = missile (press)
    e.key(SDL_SCANCODE_F, false, ms(1800));

    e.key(SDL_SCANCODE_SPACE, true, ms(1900));
    CHECK(has(e.output(ms(1950)), hid::kBtnB));      // Space = jump
    e.key(SDL_SCANCODE_SPACE, false, ms(2000));

    e.key(SDL_SCANCODE_LCTRL, true, ms(2100));
    CHECK(has(e.output(ms(2150)), hid::kBtnY));      // Left Ctrl = morph (press)
    e.key(SDL_SCANCODE_LCTRL, false, ms(2200));

    e.key(SDL_SCANCODE_TAB, true, ms(2300));
    CHECK(has(e.output(ms(2350)), hid::kBtnPlus));   // Tab = map
    e.key(SDL_SCANCODE_TAB, false, ms(2400));

    e.set_context(Ctx::Menu, ms(2500));
    e.key(SDL_SCANCODE_RETURN, true, ms(2600));
    CHECK(has(e.output(ms(2650)), hid::kBtnA));      // Return = confirm
    e.key(SDL_SCANCODE_RETURN, false, ms(2700));

    e.key(SDL_SCANCODE_ESCAPE, true, ms(2800));
    CHECK(has(e.output(ms(2850)), hid::kBtnB));      // Escape = back
    e.key(SDL_SCANCODE_ESCAPE, false, ms(2900));

    e.key(SDL_SCANCODE_Q, true, ms(3000));
    CHECK(has(e.output(ms(3050)), hid::kBtnL));      // Q = btn_l
    e.key(SDL_SCANCODE_Q, false, ms(3100));
}

NXTEST(select_pulse_survives_a_slow_game_frame) {
    // The game polls once per frame and a frame can take 100 ms or more: a pulse that started and ended between two polls must still be
    // reported once, and two selects must never show in the same poll.
    Engine e(settings_from("[bindings.all]\n1 = beam_1\n2 = beam_2\n"));
    to_gameplay(e);
    e.key(SDL_SCANCODE_1, true, ms(1000));
    e.key(SDL_SCANCODE_1, false, ms(1010));
    e.key(SDL_SCANCODE_2, true, ms(1020));
    e.key(SDL_SCANCODE_2, false, ms(1030));
    const EngineOut first = e.output(ms(1400));                   // first poll, long after both pulses should have ended
    CHECK(first.buttons == (hid::btn(hid::kBtnX) | hid::btn(hid::kBtnUp)));
    const EngineOut gap = e.output(ms(1500));                     // the poll that retires it shows nothing
    CHECK(idle(gap));
    const EngineOut second = e.output(ms(1600));
    CHECK(second.buttons != 0);
    CHECK(second.buttons != first.buttons);
    CHECK(idle(e.output(ms(1700))));
    CHECK(idle(e.output(ms(1800))));
}

NXTEST(select_pulse_holds_for_frame_boundaries_once_they_are_counted) {
    Engine e(settings_from("[bindings.all]\n2 = beam_2\n"));
    to_gameplay(e);
    e.frame_tick();
    e.key(SDL_SCANCODE_2, true, ms(1000));
    e.key(SDL_SCANCODE_2, false, ms(1010));
    const uint64_t want = hid::btn(hid::kBtnX) | hid::btn(hid::kBtnRight);
    CHECK(e.output(ms(1010)).buttons == want);          // a poll that is not the game's tick (first poll)
    CHECK(e.output(ms(1500)).buttons == want);          // past the pulse's end but no frame boundary yet: still on
    e.frame_tick();
    CHECK(e.output(ms(1600)).buttons == want);          // one boundary after the first poll: still on
    e.frame_tick();
    CHECK(idle(e.output(ms(1700))));                    // two boundaries: retired (this poll shows nothing)
}

NXTEST(engine_pulse_confirm_presses_a_once) {
    Engine e;
    CHECK(e.output(ms(900)).buttons == 0);
    e.pulse(Act::Confirm, ms(1000));                // [timing] pulse_ms (80 ms by default)
    CHECK(has(e.output(ms(1010)), hid::kBtnA));     // first poll: the press is on
    CHECK(idle(e.output(ms(1400))));                // seen, and past its end: gone
    CHECK(idle(e.output(ms(1500))));
}

NXTEST(engine_pulse_uses_given_length) {
    Engine e;
    e.pulse(Act::BtnR, ms(2000), 300);              // 300 ms, not the 80 ms default
    CHECK(has(e.output(ms(2010)), hid::kBtnR));
    CHECK(has(e.output(ms(2250)), hid::kBtnR));     // still inside the 300 ms
    CHECK(idle(e.output(ms(2500))));
    CHECK(idle(e.output(ms(2600))));
}

NXTEST(engine_aux_sticks_add_and_clamp) {
    Engine e(settings_from("[bindings.all]\nW = move_forward\n"));
    to_gameplay(e);
    e.set_aux_sticks(0.5f, 0, 0, 0);
    CHECK(close_to(e.output(ms(1050)).lx, 0.5));
    e.key(SDL_SCANCODE_W, true, ms(1100));          // ly = 1 from W, lx = 0.5 + 0 added: the pair is clamped to length 1
    const EngineOut both = e.output(ms(1150));
    CHECK(std::sqrt(both.lx * both.lx + both.ly * both.ly) <= 1.0001);
    CHECK(close_to(both.lx, 1.0 / std::sqrt(5.0)));
    CHECK(close_to(both.ly, 2.0 / std::sqrt(5.0)));
    e.set_aux_sticks(0, 0, 0, 0);
    CHECK(close_to(e.output(ms(1200)).lx, 0.0));
    CHECK(close_to(e.output(ms(1200)).ly, 1.0));
    e.set_aux_sticks(0, 0, -0.7f, 0.2f);
    const EngineOut right = e.output(ms(1250));
    CHECK(close_to(right.rx, -0.7));
    CHECK(close_to(right.ry, 0.2));
    e.key(SDL_SCANCODE_W, false, ms(1300));
    e.set_aux_sticks(0, 0, 0, 0);
}
