// SPDX-License-Identifier: GPL-3.0-or-later
// The mouse capture state machine: see play_capture.h.
#include "play_capture.h"

#include <cctype>
#include <cstring>

namespace nxrt::play {

const char* capture_mode_name(CaptureMode m) {
    switch (m) {
        case CaptureMode::Click: return "click";
        case CaptureMode::Auto: return "auto";
        case CaptureMode::Off: return "off";
    }
    return "click";
}

bool parse_capture_mode(const char* text, CaptureMode* out) {
    if (!text) return false;
    char l[16] = {};
    size_t n = 0;
    for (const char* p = text; *p && n + 1 < sizeof l; ++p) {
        if (*p == ' ' || *p == '\t') continue;
        l[n++] = static_cast<char>(std::tolower(static_cast<unsigned char>(*p)));
    }
    if (!std::strcmp(l, "click")) *out = CaptureMode::Click;
    else if (!std::strcmp(l, "auto") || !std::strcmp(l, "1") || !std::strcmp(l, "on") || !std::strcmp(l, "true")) *out = CaptureMode::Auto;
    else if (!std::strcmp(l, "off") || !std::strcmp(l, "0") || !std::strcmp(l, "never") || !std::strcmp(l, "false")) *out = CaptureMode::Off;
    else return false;
    return true;
}

void MouseCapture::set_mode(CaptureMode m) {
    if (m == mode_) return;
    mode_ = m;
    captured_ = false;
    hint_captured_ = false;
    released_hint_shown_ = false;
    why_ = "mode changed";
}

bool MouseCapture::on_button_down(int button, const Frame& f) {
    key_alone_ = false;                     // a click while the release key is down: not the key alone
    if (mode_ != CaptureMode::Click || captured_) return false;
    if (!f.focused || f.minimized || f.overlay || !f.playing || f.held_back) return false;
    captured_ = true;
    hint_captured_ = true;
    released_hint_shown_ = false;
    why_ = "";
    if (button >= 0 && button < 32) eat_up_ |= 1u << button;
    return true;
}

bool MouseCapture::eat_release(int button) {
    if (button < 0 || button >= 32 || !(eat_up_ & (1u << button))) return false;
    eat_up_ &= ~(1u << button);
    return true;
}

bool MouseCapture::on_key(int scancode, bool down, bool repeat) {
    if (down) {
        if (!repeat) key_alone_ = release_sc_ != 0 && scancode == release_sc_;
        return false;
    }
    if (release_sc_ == 0 || scancode != release_sc_) return false;
    const bool alone = key_alone_;
    key_alone_ = false;
    if (!alone || mode_ != CaptureMode::Click || !captured_) return false;
    release("release key");
    return true;
}

void MouseCapture::release(const char* why) {
    key_alone_ = false;
    if (!captured_) return;
    captured_ = false;
    hint_captured_ = false;
    released_hint_shown_ = false;
    why_ = why ? why : "";
}

void MouseCapture::on_focus_lost() { release("focus lost"); }

bool MouseCapture::want_relative(const Frame& f) {
    switch (mode_) {
        case CaptureMode::Off:
            captured_ = false;
            return false;
        case CaptureMode::Auto:
            return f.focused && !f.minimized && !f.overlay && f.playing && !f.held_back;
        case CaptureMode::Click:
            break;
    }
    if (!f.focused) release("focus lost");
    else if (f.minimized) release("minimized");
    else if (f.overlay) release("menu opened");
    return captured_ && f.playing && !f.held_back;
}

CaptureHint MouseCapture::take_hint(const Frame& f, bool mouse_play) {
    const bool focus_back = f.focused && !was_focused_;
    was_focused_ = f.focused;
    if (mode_ != CaptureMode::Click) {
        hint_captured_ = false;
        return CaptureHint::None;
    }
    if (focus_back) released_hint_shown_ = false;     // the window came back to the front: say again how to take the mouse
    if (hint_captured_) {
        hint_captured_ = false;
        if (captured_) return CaptureHint::Captured;
    }
    if (!captured_ && mouse_play && f.focused && !f.minimized && !f.overlay && f.playing && !f.held_back && !released_hint_shown_) {
        released_hint_shown_ = true;
        return CaptureHint::Released;
    }
    return CaptureHint::None;
}

}  // namespace nxrt::play
