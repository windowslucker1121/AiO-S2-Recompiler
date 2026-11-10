// SPDX-License-Identifier: GPL-3.0-or-later
// The menu pointer on Nintendo UI2D layouts (nn::ui2d, the BFLYT layout library of first-party games) and the button controls built on
// them: the ui2d adapter of the menu pointer engine (menu_pointer.h). Written against a Nintendo nn::ui2d game; every
// guest address and offset comes from the game's config ([menu] by default, see runtime/docs/menu_pointer.md for each one's evidence).
//
// Those games already drive their buttons with a pointer: a button group's pointer update (Config::group_update) takes a position in
// layout units and the touch's trigger / release / hold, hit-tests each button's "Hit" pane, turns the button under it On (the focus
// animation) and the one it left Off, and decides the button on a trigger. The adapter
//   * records every group whose pointer update ran (an entry hook) and lists its buttons as elements (their Hit pane's box, the game's own
//     visibility rule), the group's current button as the selection;
//   * while the menu pointer is up, hands the mouse to that update in place of the touch (the hook rewrites its arguments): hover then
//     focuses natively (the engine's focus call is that), a click confirms through a trigger and a release on the button.
// Groups the game keeps pointer-disabled get no elements (the game would ignore the pointer there); their menus keep the menu bindings.
#pragma once
#include <cstddef>
#include <cstdint>

#include "menu_mouse.h"
#include "play_types.h"

namespace nxrt::play::ui2d {

struct Config {
    const char* tag = "menu";
    uint64_t group_update = 0;            // offset in main: ButtonGroup::UpdatePointer(group, const float pos[2] or null, trig, release, hold, from_pointer)
    // button group
    size_t grp_buttons = 8;               // intrusive list head {next, prev}; a node sits at button + btn_node
    size_t grp_current = 40;              // the button the pointer is on (On), or null
    size_t grp_flags = 64;                // u32
    uint32_t grp_pointer_bit = 0x2;       // the group takes the pointer
    // button
    size_t btn_node = 8;
    size_t btn_layout = 32;               // the nn::ui2d::Layout the button is in
    size_t btn_flags = 64;                // u32; all of btn_ready_mask set = the button takes input
    uint32_t btn_ready_mask = 0x220010;
    size_t btn_hit = 88;                  // the "Hit" pane (nn::ui2d::Pane*)
    // nn::ui2d::Layout
    size_t lay_size = 40;                 // float width, height (layout units, origin at the centre, y up)
    // nn::ui2d::Pane
    size_t pane_parent = 24, pane_size = 80, pane_flags = 88, pane_alpha = 89, pane_base = 91, pane_mtx = 112, pane_name = 176;
    float canvas_w = 1920.f, canvas_h = 1080.f;   // the engine's canvas (each layout is scaled onto it by its own size)
};

// Reads Config from the ini: keys ui2d_* of `section` (docs/menu_pointer.md lists them); false when group_update is not set.
bool config_from_ini(const char* section, Config* out);

void install(uint64_t base, const Config& cfg, const char* section = "menu");   // cfg must outlive the install
void remove();
Act menu_frame(bool menu_up, const mm::Geometry& g);   // main thread, once per frame (GamePlay::menu_frame)
bool menu_over_value();
bool menu_active();
bool menu_in_game(bool in_game);                        // main thread, once per frame before the context (GamePlay::menu_in_game)
void status_text(char* buf, size_t n);

}  // namespace nxrt::play::ui2d
