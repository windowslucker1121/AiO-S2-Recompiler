// SPDX-License-Identifier: GPL-3.0-or-later
// The menu pointer on KTGL screen layouts (Koei Tecmo's engine): the KTGL adapter of the menu pointer engine (menu_pointer.h,
// docs/menu_pointer.md). The game passes the offsets of its build in Config (as the removed title A port did) and wires the functions
// below into play mode's GamePlay table (menu_frame, menu_over_value, menu_active, menu_in_game).
#pragma once
#include <cstddef>
#include <cstdint>

#include "menu_mouse.h"
#include "play_types.h"

namespace nxrt::play::ktgl {

// A pane draw's flag store, hooked as a midhook (the game's sites file must list it): the pane is in x[reg].
struct DrawSite { uint64_t off; int reg; };

struct Config {
    const char* tag = "ktgl";              // log tag
    const char* section = "";              // ini section of the menu_* keys (menu_mouse, menu_log, menu_in_game, menu_in_game_ignore, menu_key_guides)
    const char* default_key_guides = "";   // menu_key_guides' default: "layout_id=act ..."
    // the UI manager: [main + ui_mgr] -> manager; two placeable pools at +pool_a / +pool_b, each with {entry** data, u64 count} at +pool_entries
    uint64_t ui_mgr = 0, pool_a = 0, pool_b = 0, pool_entries = 0;
    size_t ent_instances = 0, ent_free = 0;          // pool A entry: -> {placeable** data, u64 count}, -> free ring
    size_t placeable_obj = 0, obj_layout = 0;        // placeable +placeable_obj -> object, +obj_layout -> the layout
    size_t lay_pane_count = 0, lay_first_node = 0, lay_sentinel = 0;   // layout: u16 pane count; pane list (node +8 next, +16 pane)
    size_t pane_name = 0, pane_tag = 0, pane_parent = 0, pane_world = 0, pane_size = 0, pane_flags = 0, pane_anchor = 0, pane_name_len = 0,
           pane_alpha = 0, pane_layout = 0;
    float canvas_w = 1920.f, canvas_h = 1080.f;     // the layouts' canvas (origin at the centre, y up)
    const DrawSite* draw_sites = nullptr;
    size_t draw_site_count = 0;
    // Gate policy: while this returns true (the player is in the world) a drawn key guide layout is the HUD's, not a menu.
    bool (*world_live)() = nullptr;
};

void install(uint64_t base, const Config& cfg);     // cfg must outlive the install
void remove();
Act menu_frame(bool menu_up, const mm::Geometry& g);   // main thread, once per frame (GamePlay::menu_frame)
bool menu_over_value();                                 // the pointer is over a slider: the wheel steps it
bool menu_active();                                     // the pointer was active this frame
bool menu_open();                                       // a menu with the game's cursor is on screen
bool menu_in_game(bool in_game);                        // main thread, once per frame before the context: a menu is up over play
void status_text(char* buf, size_t n);

}  // namespace nxrt::play::ktgl
