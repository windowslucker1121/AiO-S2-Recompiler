// SPDX-License-Identifier: GPL-3.0-or-later
// The mouse capture state machine (play_capture.cpp): click to capture, every way out, and never a capture by itself after a release.
#include <string>

#include "play/play_capture.h"
#include "test_framework.h"
NXTEST_GROUP(play_capture)

namespace {

using namespace nxrt::play;
constexpr int kLAlt = 226, kTab = 43, kW = 26, kG = 10;

MouseCapture::Frame playing() {
    MouseCapture::Frame f;
    f.focused = true;
    f.playing = true;
    return f;
}

}  // namespace

NXTEST(click_mode_captures_only_after_a_click_and_eats_it) {
    MouseCapture c;
    const MouseCapture::Frame f = playing();
    CHECK(!c.want_relative(f));                       // focus alone does not capture
    CHECK(c.take_hint(f, true) == CaptureHint::Released);   // "Click to control the camera"
    CHECK(c.take_hint(f, true) == CaptureHint::None);       // once
    CHECK(c.on_button_down(1, f));                    // the click captures and does not reach the game
    CHECK(c.want_relative(f));
    CHECK(c.take_hint(f, true) == CaptureHint::Captured);
    CHECK(c.take_hint(f, true) == CaptureHint::None);
    CHECK(c.eat_release(1));                          // its release is swallowed too, once
    CHECK(!c.eat_release(1));
    CHECK(!c.on_button_down(1, f));                   // later clicks are the game's
    CHECK(!c.eat_release(1));
}

NXTEST(release_key_alone_lets_go_and_nothing_captures_by_itself) {
    MouseCapture c;
    MouseCapture::Frame f = playing();
    c.on_button_down(1, f);
    c.eat_release(1);
    REQUIRE(c.want_relative(f));
    // Alt + another key (Alt+G opens a menu, Alt+Enter, Alt+Tab): not the release key alone
    CHECK(!c.on_key(kLAlt, true, false));
    CHECK(!c.on_key(kG, true, false));
    CHECK(!c.on_key(kG, false, false));
    CHECK(!c.on_key(kLAlt, false, false));
    CHECK(c.want_relative(f));
    // Alt alone (with key repeat while held)
    CHECK(!c.on_key(kLAlt, true, false));
    CHECK(!c.on_key(kLAlt, true, true));
    CHECK(c.on_key(kLAlt, false, false));
    CHECK(!c.captured());
    for (int i = 0; i < 5; ++i) CHECK(!c.want_relative(f));
    CHECK(c.take_hint(f, true) == CaptureHint::Released);
    // focus lost and back: still not captured, the hint is shown again
    f.focused = false;
    CHECK(!c.want_relative(f));
    c.take_hint(f, true);
    f.focused = true;
    CHECK(!c.want_relative(f));
    CHECK(c.take_hint(f, true) == CaptureHint::Released);
    CHECK(c.on_button_down(1, f));                    // only a click takes it again
    CHECK(c.want_relative(f));
}

NXTEST(focus_loss_minimize_overlay_and_menu_release) {
    MouseCapture c;
    MouseCapture::Frame f = playing();
    c.on_button_down(1, f);
    c.on_focus_lost();                                // Alt+Tab, the Win key
    CHECK(!c.captured());
    f.focused = true;
    CHECK(!c.want_relative(f));
    c.on_button_down(1, f);
    MouseCapture::Frame m = f;
    m.minimized = true;
    CHECK(!c.want_relative(m));
    CHECK(!c.want_relative(f));
    c.on_button_down(1, f);
    MouseCapture::Frame o = f;
    o.overlay = true;
    CHECK(!c.want_relative(o));
    CHECK(!c.want_relative(f));
    CHECK(!c.on_button_down(1, o));                   // a click on the open menu does not capture
    c.on_button_down(1, f);
    c.release("menu bar opened");
    CHECK(!c.want_relative(f));
    CHECK(std::string(c.last_release()) == "menu bar opened");
}

NXTEST(game_menus_hold_the_capture_back_without_releasing_it) {
    MouseCapture c;
    MouseCapture::Frame f = playing();
    MouseCapture::Frame menu = f;
    menu.playing = false;
    CHECK(!c.on_button_down(1, menu));                // a click in the game's menu is the game's
    c.on_button_down(1, f);
    CHECK(!c.want_relative(menu));                    // the menu pointer needs the cursor
    CHECK(c.captured());
    CHECK(c.want_relative(f));                        // back in play: the player's capture holds
    MouseCapture::Frame rebind = f;
    rebind.held_back = true;
    CHECK(!c.want_relative(rebind));
    CHECK(c.want_relative(f));
}

NXTEST(auto_and_off_modes) {
    MouseCapture c;
    c.set_mode(CaptureMode::Auto);
    MouseCapture::Frame f = playing();
    CHECK(c.want_relative(f));
    CHECK(!c.on_button_down(1, f));                   // clicks are the game's
    CHECK(c.take_hint(f, true) == CaptureHint::None);
    f.focused = false;
    CHECK(!c.want_relative(f));
    c.set_mode(CaptureMode::Off);
    f.focused = true;
    CHECK(!c.want_relative(f));
    CHECK(!c.on_button_down(1, f));
    MouseCapture::Frame g = playing();
    c.set_mode(CaptureMode::Click);
    c.set_release_key(0);                             // no release key: Alt does nothing
    CHECK(c.on_button_down(1, g));
    c.on_key(kLAlt, true, false);
    CHECK(!c.on_key(kLAlt, false, false));
    CHECK(c.want_relative(g));
    (void)kTab;
    (void)kW;
}

NXTEST(capture_mode_words) {
    CaptureMode m = CaptureMode::Off;
    CHECK(parse_capture_mode("Click", &m) && m == CaptureMode::Click);
    CHECK(parse_capture_mode("auto", &m) && m == CaptureMode::Auto);
    CHECK(parse_capture_mode("off", &m) && m == CaptureMode::Off);
    CHECK(!parse_capture_mode("maybe", &m));
    CHECK(std::string(capture_mode_name(CaptureMode::Auto)) == "auto");
}
