// SPDX-License-Identifier: GPL-3.0-or-later
// The menu pointer engine (menu_pointer.h): hit-test, hover -> focus, click -> confirm, the menu gate. No game, no UI library.
#include "menu_pointer.h"

#include <chrono>
#include <cmath>
#include <cstdio>

#include "nxrt/log.h"

namespace nxrt::play::mp {

namespace {

uint64_t now_ns() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

const char* act_name(Act a) {
    switch (a) {
        case Act::DpadUp: return "up";
        case Act::DpadDown: return "down";
        case Act::DpadLeft: return "left";
        case Act::DpadRight: return "right";
        case Act::BtnA: return "A";
        default: return "?";
    }
}

}  // namespace

Engine& engine() {
    static Engine e;
    return e;
}

Act dir_toward(const Element& from, const Element& to, bool other_axis) {
    const float dx = to.cx() - from.cx(), dy = to.cy() - from.cy();
    bool horiz = std::fabs(dx) > std::fabs(dy);
    if (other_axis && std::fabs(dx) > 4.f && std::fabs(dy) > 4.f) horiz = !horiz;
    if (horiz) return dx > 0 ? Act::DpadRight : Act::DpadLeft;
    return dy > 0 ? Act::DpadDown : Act::DpadUp;
}

bool Engine::log_ok(bool force) {
    if (log_level_ <= 0 || log_lines_ > 4000) return false;
    const uint64_t now = now_ns();
    if (!force && now - last_log_ns_ < 200000000ull) return false;
    last_log_ns_ = now;
    ++log_lines_;
    return true;
}

int Engine::add(const Element& el) {
    els_.push_back(el);
    return static_cast<int>(els_.size()) - 1;
}

void Engine::set_selection(int focused, bool marker, const char* marker_name) {
    focused_ = focused >= 0 && focused < static_cast<int>(els_.size()) ? focused : -1;
    marker_ = marker;
    std::snprintf(marker_name_, sizeof marker_name_, "%s", marker_name ? marker_name : "");
}

bool Engine::take_frame_result(bool* present) {
    const bool had = frame_scanned_;
    *present = frame_present_;
    frame_scanned_ = false;
    return had;
}

int Engine::find(uint64_t key) const {
    for (size_t i = 0; i < els_.size(); ++i)
        if (els_[i].key == key) return static_cast<int>(i);
    return -1;
}

void Engine::clear_nav() {
    target_ = 0;
    steps_ = wait_ = fails_ = click_wait_ = 0;
    pending_confirm_ = false;
    prefer_other_axis_ = false;
}

void Engine::reset() {
    clear_nav();
    els_.clear();
    hovered_ = focused_ = slider_ = -1;
    marker_ = active_ = false;
    frame_scanned_ = frame_present_ = false;
}

Act Engine::confirm(const Adapter& a, const Element& el) {
    if (a.confirm && a.confirm(el, a.user)) return Act::None;
    return Act::BtnA;
}

Act Engine::frame(bool menu_up, const mm::Geometry& g, const Adapter& a) {
    mm::Pointer& ptr = mm::pointer();
    const mm::Snapshot snap = ptr.begin_frame(menu_up, g);
    active_ = false;
    if (log_level_ >= 3) {
        static uint32_t n = 0;
        if (++n % 600 == 0) NXLOG_I(a.tag, "menu: frame menu_up %d active %d inside %d shown %d", menu_up ? 1 : 0, snap.active ? 1 : 0, snap.inside ? 1 : 0, snap.shown ? 1 : 0);
    }
    if ((a.ready && !a.ready(a.user)) || !snap.active || !a.collect) {
        if (a.idle) a.idle(a.user);
        els_.clear();
        hovered_ = focused_ = slider_ = -1;
        marker_ = false;
        clear_nav();
        return Act::None;
    }
    // The mouse is not the device in use and nothing is being carried out for it: the elements only feed the menu gate and the log, so the
    // scan (a walk of every layout in the game's UI pools) runs four times a second, not every frame. While title A loads its screens the
    // per-frame scan was up to half of the main thread's time. The last scan's elements and result stand in between.
    if (!snap.shown && !snap.click && !snap.held && !target_ && !pending_confirm_) {
        const uint64_t now = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
        if (now - idle_scan_ns_ < kIdleScanNs) {
            active_ = true;
            frame_scanned_ = true;   // the gate takes the last result instead of scanning itself
            return Act::None;
        }
        idle_scan_ns_ = now;
    }
    active_ = true;
    els_.clear();
    hovered_ = focused_ = slider_ = -1;
    marker_ = false;
    marker_name_[0] = 0;
    // the pointer on the canvas: the UI plane (-1..1 across the centred 16:9 region, y up) stretched over it, y down
    const float px = (snap.nx + 1.f) * 0.5f * a.canvas_w, py = (1.f - snap.ny) * 0.5f * a.canvas_h;
    const bool inside = snap.inside && snap.shown;
    frame_present_ = a.collect(*this, px, py, inside, a.user);
    frame_scanned_ = true;
    if (inside) {   // the smallest hoverable element under the pointer (the first on a tie); the first slider under it
        float hb = 1e30f;
        for (size_t i = 0; i < els_.size(); ++i) {
            const Element& p = els_[i];
            if (!p.contains(px, py)) continue;
            if ((p.flags & kSlider) && slider_ < 0) slider_ = static_cast<int>(i);
            if (!(p.flags & kHover)) continue;
            if (p.area() < hb) { hb = p.area(); hovered_ = static_cast<int>(i); }
        }
    }

    const Element* hov = hovered_ >= 0 ? &els_[hovered_] : nullptr;
    const Element* foc = focused_ >= 0 ? &els_[focused_] : nullptr;
    const uint64_t hov_key = hov ? hov->key : 0, foc_key = foc ? foc->key : 0;
    if (hov_key != last_hover_) {
        std::snprintf(hover_name_, sizeof hover_name_, "%s", hov ? hov->name : "");
        if (log_ok()) {
            if (hov) NXLOG_I(a.tag, "menu: hover '%s' (layout %u/%u, drawn rect %.0f,%.0f..%.0f,%.0f) at %.0f,%.0f; selected '%s'", hov->name, hov->layout,
                             hov->sub, hov->x0, hov->y0, hov->x1, hov->y1, px, py, foc ? foc->name : "");
            else NXLOG_I(a.tag, "menu: hover nothing at %.0f,%.0f", px, py);
        }
        last_hover_ = hov_key;
    }
    if (foc_key != last_focus_) {
        std::snprintf(focus_name_, sizeof focus_name_, "%s", foc ? foc->name : "");
        if (foc && log_ok()) NXLOG_I(a.tag, "menu: selected '%s' (cursor '%s')", foc->name, marker_name_);
        last_focus_ = foc_key;
        focus_stable_ = 0;
    } else {
        ++focus_stable_;
    }

    // keyboard or pad used: the pointer lets go of the selection
    if (!snap.shown) clear_nav();
    // the pointer moved onto another element: the selection follows it
    if (snap.hover && hov && hov_key != target_ && marker_) {
        clear_nav();
        target_ = hov_key;
    }
    // left click
    if (snap.click) {
        ptr.click_used();
        ++n_clicks_;
        // an element with a button of its own under the pointer (a key guide: "(-) Settings")
        if (inside) {
            for (const Element& p : els_) {
                if (p.click == Act::None || !p.contains(px, py)) continue;
                if (log_ok(true)) NXLOG_I(a.tag, "menu: click on key guide '%s' (layout %u/%u) -> %s", p.name, p.layout, p.sub, act_info(p.click).name);
                clear_nav();
                return p.click;
            }
        }
        if (!marker_) {
            if (log_ok(true)) NXLOG_I(a.tag, "menu: click -> A (no cursor on screen)");
            clear_nav();
            return Act::BtnA;
        }
        if (hov && foc_key == hov_key) {
            if (log_ok(true)) NXLOG_I(a.tag, "menu: click on the selected '%s' -> A", hov->name);
            clear_nav();
            return confirm(a, *hov);
        }
        if (hov) {
            target_ = hov_key;
            pending_confirm_ = true;
            click_wait_ = 0;
            if (log_ok(true)) NXLOG_I(a.tag, "menu: click on '%s', selecting it first", hov->name);
        } else if (log_ok(true)) {
            NXLOG_I(a.tag, "menu: click on no entry (ignored)");
        }
    }

    if (!target_) return Act::None;
    const int ti = find(target_);
    if (ti < 0 || !foc) {
        if (pending_confirm_ && ++click_wait_ > 90) clear_nav();
        else if (!pending_confirm_) clear_nav();
        return Act::None;
    }
    if (foc_key == target_) {
        if (pending_confirm_) {
            if (focus_stable_ < 2) return Act::None;      // the selection still moving
            if (log_ok(true)) NXLOG_I(a.tag, "menu: '%s' selected -> A", foc->name);
            clear_nav();
            return confirm(a, *foc);
        }
        clear_nav();
        return Act::None;
    }
    if (wait_ > 0) {
        --wait_;
        if (wait_ == 0 && foc_key == focus_before_) {
            ++fails_;                                  // the press moved nothing: try the other axis, then give up
            prefer_other_axis_ = !prefer_other_axis_;
            if (fails_ >= 4) {
                if (log_ok()) NXLOG_I(a.tag, "menu: cannot reach '%s' from '%s'", els_[ti].name, foc->name);
                clear_nav();
            }
        }
        return Act::None;
    }
    if (steps_ >= 40) { clear_nav(); return Act::None; }
    ++steps_;
    ++n_steps_;
    wait_ = 8;
    focus_before_ = foc_key;
    if (a.focus && a.focus(els_[ti], a.user)) {
        if (log_level_ >= 2 && log_ok()) NXLOG_I(a.tag, "menu: focus '%s' from '%s'", els_[ti].name, foc->name);
        return Act::None;
    }
    const Act d = dir_toward(*foc, els_[ti], prefer_other_axis_);
    if (log_level_ >= 2 && log_ok()) NXLOG_I(a.tag, "menu: step %s from '%s' toward '%s'", act_name(d), foc->name, els_[ti].name);
    return d;
}

bool Gate::poll(bool live, Engine& e, bool (*scan)(void* user), void* user, Event* ev) {
    *ev = kNone;
    bool scanned_present = false;
    const bool scanned = e.take_frame_result(&scanned_present);
    if (!live) {
        if (up_) *ev = kLeft;
        reset();
        return false;
    }
    bool present;
    if (up_ && scanned) {
        present = scanned_present;
    } else {
        if (!up_ && ++tick_ < kPollFrames) return false;
        tick_ = 0;
        present = scan(user);
    }
    if (!up_) {
        hits_ = present ? hits_ + 1 : 0;
        if (hits_ >= kOpenHits) {
            up_ = true;
            miss_ = 0;
            ++opens_;
            *ev = kOpened;
        }
    } else {
        miss_ = present ? 0 : miss_ + 1;
        if (miss_ >= kCloseFrames) {
            up_ = false;
            hits_ = 0;
            *ev = kClosed;
        }
    }
    return up_;
}

}  // namespace nxrt::play::mp
