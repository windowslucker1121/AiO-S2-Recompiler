// SPDX-License-Identifier: GPL-3.0-or-later
// The menu pointer on KTGL screen layouts (menu_ktgl.h): the KTGL adapter of the menu pointer engine (menu_pointer.h). Written against
// title A 2.0.1 (the addresses in the comments are in its main; the offsets themselves come from the game's Config).
//
// A KTGL layout object holds its panes in a list, each pane has a name, a size and a world matrix in a 1920x1080 canvas (origin at the
// centre, y up). The game itself never hit-tests them for its menus (the engine's script call kids::placeable::screenlayout::DoPaneHitTest
// exists, 0x2611d00, and is where the pane geometry below comes from), and its menus move a cursor part (0000_parts_cursor_finger_s,
// 0250_parts_talk_cursor, 0000_parts_cursor_triangle ...) next to the selected entry when the d-pad moves the selection. So, once a frame
// while a menu is up (play mode's menu context: the camera's input is not running):
//   * every layout in use in the UI manager's pools (Config ui_mgr, pool_a / pool_b; placeable +40 -> +16 = the layout) is walked and each
//     visible pane's screen rectangle computed;
//   * the panes go to the engine as elements: the entries (an "item" pane: a button, list cell, choice, tab ... by its name) are hoverable,
//     sliders take the wheel, the entries of a key guide layout (menu_key_guides) send their button when clicked;
//   * the selected entry is the item pane nearest to the game's cursor pane (a pane named "...cursor..."): the engine's selection. KTGL has
//     no focus call, so hover moves the selection with d-pad presses and a click confirms with A;
//   * with no cursor on screen (messages, prompts) a click anywhere is A. Right click (B) and the wheel (d-pad up / down) are menu-context
//     bindings of the game.
// Menus whose selection shows no cursor pane (highlight only) get the click and the bindings but not the hover; the log says which
// layouts were on screen (menu_log = 2 lists their panes) so they can be added.
#include "menu_ktgl.h"
#include "menu_pointer.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "nxrt/config.h"
#include "nxrt/hooks.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"

namespace nxrt::play::ktgl {

namespace {

const Config* g_cfg = nullptr;
const Config& C() {
    static const Config none;
    return g_cfg ? *g_cfg : none;
}

constexpr uint64_t kMaxPoolEntries = 512, kMaxInstances = 64;   // pool A: entries in use (cap 512), instances of one layout
constexpr size_t kMaxPanes = 4096;

struct Pane {
    uint64_t ptr = 0;
    uint32_t lay_id = 0, lay_sub = 0;
    int lay = -1;                      // index into the frame's layouts
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;   // canvas pixels, y down
    float z = 0;
    uint8_t alpha = 0;
    uint32_t flags = 0;
    bool item = false, cursor = false, slider = false;
    char name[48] = {};
    float cx() const { return (x0 + x1) * 0.5f; }
    float cy() const { return (y0 + y1) * 0.5f; }
    float area() const { return (x1 - x0) * (y1 - y0); }
    bool contains(float x, float y) const { return x >= x0 && x <= x1 && y >= y0 && y <= y1; }
    float dist(float x, float y) const {
        const float dx = std::max({x0 - x, 0.f, x - x1}), dy = std::max({y0 - y, 0.f, y - y1});
        return std::sqrt(dx * dx + dy * dy);
    }
};

struct Layout {
    uint64_t ptr = 0;
    uint32_t id = 0, sub = 0;
    int panes = 0, visible = 0;
};

struct State {
    uint64_t base = 0x7100000000ull;
    bool installed = false;
    bool enabled = true;
    // the frame's scan
    std::vector<Layout> layouts;
    std::vector<Pane> panes;
    int focused = -1, cursor = -1;
    // how pick() found the selection (menus outside play, see pick_extend): 0 the cursor's rule, 1 promoted to the list entry that holds
    // the cursor's part, 2 sibling instances of one button layout (group = their entry panes)
    int mode = 0;
    std::vector<int> group;
    uint64_t mode_sig = 0;
    std::unordered_map<uint32_t, Act> key_guides;   // menu_key_guides: a key guide layout id -> the button its click sends
    // in-game menus (menu_in_game): a layout with the game's cursor on an entry while the game is in play
    bool ig_enabled = false;
    mp::Gate gate;                     // the in-game menu is up (debounced)
    uint32_t ig_lay_id = 0, ig_lay_sub = 0;
    char ig_cursor[48] = {};
    std::unordered_set<uint64_t> ig_ignore;   // menu_in_game_ignore: layouts (id << 32 | sub) whose cursor never counts (a HUD with a cursor)
    int n_not_drawn = 0;               // this scan: pooled layouts skipped as not drawn
    int rec_panes = 0, rec_with = 0, rec_fallback_layouts = 0;      // this scan: shown panes of drawn layouts, those with a draw record,
    std::vector<uint32_t> rec_ages;                                 // layouts with no fresh record (fallback), the stale records' ages
    // diagnostics
    bool was_up = false;
    uint64_t layout_sig = 0;
    std::unordered_set<uint64_t> dumped;    // layout (id, sub) whose panes were listed
    std::atomic<uint32_t> n_layouts{0}, n_panes{0}, n_items{0};
    // page readability cache (one VirtualQuery per page and scan)
    std::unordered_map<uint64_t, bool> pages;
};

State& st() {
    static State s;
    return s;
}

template <class T> T rd(uint64_t a) { T v; std::memcpy(&v, reinterpret_cast<const void*>(a), sizeof(T)); return v; }

// The range lies in guest memory that can be read (cached per 4 KiB page for one scan).
bool ok(uint64_t a, uint64_t n) {
    // guest space: the heap and stacks (0x78........) and main's image at 0x7100000000 (the UI manager pointer lives there)
    if (a < 0x1000000000ull || a + n > 0x8000000000ull || n == 0 || n > 0x10000) return false;
    State& s = st();
    for (uint64_t p = a & ~0xfffull; p < a + n; p += 0x1000) {
        auto it = s.pages.find(p);
        bool m;
        if (it != s.pages.end()) m = it->second;
        else {
            m = mem::is_mapped(p, 0x1000);
            s.pages.emplace(p, m);
        }
        if (!m) return false;
    }
    return true;
}

uint64_t rptr(uint64_t a) { return ok(a, 8) ? rd<uint64_t>(a) : 0; }

void read_str(uint64_t p, size_t len_hint, char* out, size_t n) {
    out[0] = 0;
    if (!p) return;
    size_t len = len_hint ? std::min(len_hint, n - 1) : n - 1;
    if (!ok(p, len)) {
        len = 0;
        while (len < n - 1 && ok(p + len, 1) && rd<char>(p + len)) ++len;
    }
    size_t i = 0;
    for (; i < len; ++i) {
        const char c = rd<char>(p + i);
        if (!c) break;
        out[i] = (c >= 0x20 && c < 0x7f) ? c : '?';
    }
    out[i] = 0;
}

bool has(const std::string& s, const char* k) { return s.find(k) != std::string::npos; }

void classify(Pane& p, const char* tag) {
    std::string n = p.name;
    n += '|';
    n += tag;
    for (char& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    p.cursor = has(n, "cursor") && !has(n, "mouse");
    p.slider = has(n, "slider") || has(n, "slide_bar");
    static const char* const kItem[] = {"btn", "button", "item", "cell", "choice", "list", "tab", "slot", "chip", "select", "menu"};
    static const char* const kNot[] = {"cursor", "txt", "text", "icon", "shadow", "_bg", "bg_", "frame", "base", "line", "effect", "eff_"};
    bool item = false;
    for (const char* k : kItem) item = item || has(n, k);
    for (const char* k : kNot) item = item && !has(n, k);
    p.item = item;
}

// One pane's rectangle on the canvas (the anchor rule of DoPaneHitTest's box, 0x2645000): the pane's origin is its left / centre / right
// edge by the anchor's high nibble (0 / 1 / 2) and its top / middle / bottom by the low nibble; x and y axes from the world matrix.
// The rectangle from the world matrix's X axis (m0, m1), Y axis (m4, m5) and translation (m12, m13), the size and the anchor: the corners the
// pane draws build (0x7101f53010: the same anchor offsets, then the matrix, then the UI camera).
bool rect_from(const float* m, float m14, uint32_t size, uint8_t anchor, Pane* out) {
    const float w = static_cast<float>(size & 0xffff), h = static_cast<float>(size >> 16);
    if (w <= 0.f || h <= 0.f) return false;
    const int ah = anchor >> 4, av = anchor & 0xf;
    const float ox = ah == 0 ? w * 0.5f : (ah == 2 ? -w * 0.5f : 0.f);
    const float oy = av == 0 ? -h * 0.5f : (av == 2 ? h * 0.5f : 0.f);
    const float cx = m[12] + m[0] * ox + m[4] * oy, cy = m[13] + m[1] * ox + m[5] * oy;
    const float hx = (std::fabs(m[0]) * w + std::fabs(m[4]) * h) * 0.5f, hy = (std::fabs(m[1]) * w + std::fabs(m[5]) * h) * 0.5f;
    out->x0 = C().canvas_w * 0.5f + cx - hx;
    out->x1 = C().canvas_w * 0.5f + cx + hx;
    out->y0 = C().canvas_h * 0.5f - cy - hy;
    out->y1 = C().canvas_h * 0.5f - cy + hy;
    out->z = m14;
    return true;
}

bool pane_rect(uint64_t pane, Pane* out) {
    if (!ok(pane, C().pane_alpha + 1)) return false;
    const uint64_t mp = rd<uint64_t>(pane + C().pane_world);
    if (!ok(mp, 64)) return false;
    float m[16];
    std::memcpy(m, reinterpret_cast<const void*>(mp), sizeof m);
    for (float v : m)
        if (!std::isfinite(v)) return false;
    if (!rect_from(m, m[14], rd<uint32_t>(pane + C().pane_size), rd<uint8_t>(pane + C().pane_anchor), out)) return false;
    out->alpha = rd<uint8_t>(pane + C().pane_alpha);
    out->flags = rd<uint32_t>(pane + C().pane_flags);
    return true;
}

bool log_ok(bool force = false) { return mp::engine().log_ok(force); }

// menu_log = 3: the chain from the UI manager global down to the panes, once a second while scanning.
// ---- the layouts the game draws (needs the draw sites as midhooks in the game's sites file) ----
// Every KTGL pane class's draw (vtable slot +0x2b0; e.g. the picture pane 0x7101f5b248) starts the same way: it ORs 0x20 into the pane's
// flags (+0x104 = +260), then returns early unless flags bit 0x4000 (forced) or bit 0x100 (visible) with no ancestor flagged 0x40 (hidden,
// 0x7101f72780) and a nonzero effective alpha (+0x113 = +275); the visible ones then emit their primitives through the draw context (x1).
// Pooled layouts are never handed to the draw, so "a pane of this layout went through a draw this frame" is the drawn test. The hooks sit
// on each draw's flag store (pane in x0, x21 for 0x7101f77fe0); pane +216 is its layout (the draw reads [pane+0xd8]+0x2e0, the layout's
// renderer, as 0x7101f56180 does). The scan then keeps only layouts drawn in the last kDrawnFrames frames.
constexpr uint32_t kDrawnFrames = 3;
constexpr size_t kDrawnSlots = 4096;
std::atomic<uint64_t> g_drawn_key[kDrawnSlots];
std::atomic<uint32_t> g_drawn_frame[kDrawnSlots];
std::atomic<uint32_t> g_frame{1};             // play mode frames (menu_in_game runs once per frame)
std::atomic<uint64_t> g_draw_calls{0};
std::vector<uint32_t> g_draw_hooks;

size_t drawn_slot(uint64_t lay) { return static_cast<size_t>((lay >> 4) * 0x9e3779b97f4a7c15ull >> 52) & (kDrawnSlots - 1); }

void drawn_mark(uint64_t lay, uint32_t frame) {
    size_t h = drawn_slot(lay);
    for (int k = 0; k < 32; ++k, h = (h + 1) & (kDrawnSlots - 1)) {
        uint64_t key = g_drawn_key[h].load(std::memory_order_relaxed);
        if (key == 0 && g_drawn_key[h].compare_exchange_strong(key, lay, std::memory_order_relaxed)) key = lay;
        if (key == lay) { g_drawn_frame[h].store(frame, std::memory_order_relaxed); return; }
    }
}

// The layout went through a pane draw in the last kDrawnFrames frames.
bool drawn_recent(uint64_t lay) {
    const uint32_t now = g_frame.load(std::memory_order_relaxed);
    size_t h = drawn_slot(lay);
    for (int k = 0; k < 32; ++k, h = (h + 1) & (kDrawnSlots - 1)) {
        const uint64_t key = g_drawn_key[h].load(std::memory_order_relaxed);
        if (key == 0) return false;
        if (key == lay) return now - g_drawn_frame[h].load(std::memory_order_relaxed) <= kDrawnFrames;
    }
    return false;
}

bool draw_hooks_live() { return g_draw_calls.load(std::memory_order_relaxed) != 0; }

// Each pane that went through a draw: the frame, and the world matrix, size and anchor as the draw read them (the rect is the drawn one,
// whatever parent or animation moved it). Written by the drawing thread (the data, then the frame), read by the scan.
struct DrawnPane {
    std::atomic<uint64_t> key{0};
    std::atomic<uint32_t> frame{0};
    float m[16];
    uint32_t size = 0;
    uint8_t anchor = 0;
};
constexpr size_t kPaneSlots = 16384;
DrawnPane g_drawn_panes[kPaneSlots];
std::atomic<uint64_t> g_pane_table_full{0};      // marks dropped: 32 probes found no slot

size_t pane_slot(uint64_t pane) { return static_cast<size_t>((pane >> 3) * 0x9e3779b97f4a7c15ull >> 50) & (kPaneSlots - 1); }

bool pane_mark(uint64_t pane, uint32_t frame) {
    size_t h = pane_slot(pane);
    for (int k = 0; k < 32; ++k, h = (h + 1) & (kPaneSlots - 1)) {
        DrawnPane& d = g_drawn_panes[h];
        uint64_t key = d.key.load(std::memory_order_relaxed);
        if (key == 0 && d.key.compare_exchange_strong(key, pane, std::memory_order_relaxed)) key = pane;
        if (key != pane) continue;
        if (d.frame.load(std::memory_order_relaxed) == frame) return true;
        const uint64_t mp = *reinterpret_cast<const uint64_t*>(pane + C().pane_world);   // the draw builds its corners from this matrix
        if (mp) std::memcpy(d.m, reinterpret_cast<const void*>(mp), sizeof d.m);
        d.size = *reinterpret_cast<const uint32_t*>(pane + C().pane_size);
        d.anchor = *reinterpret_cast<const uint8_t*>(pane + C().pane_anchor);
        d.frame.store(mp ? frame : 0, std::memory_order_release);
        return false;
    }
    g_pane_table_full.fetch_add(1, std::memory_order_relaxed);
    return false;
}

// The pane's draw record: kNoRecord, kStale (drawn, but more than kDrawnFrames frames ago; *age set) or kFresh (*out = its drawn rectangle).
enum { kNoRecord = 0, kStale = 1, kFresh = 2 };
int pane_record(uint64_t pane, Pane* out, uint32_t* age) {
    const uint32_t now = g_frame.load(std::memory_order_relaxed);
    size_t h = pane_slot(pane);
    for (int k = 0; k < 32; ++k, h = (h + 1) & (kPaneSlots - 1)) {
        const DrawnPane& d = g_drawn_panes[h];
        const uint64_t key = d.key.load(std::memory_order_relaxed);
        if (key == 0) return kNoRecord;
        if (key != pane) continue;
        const uint32_t f = d.frame.load(std::memory_order_acquire);
        if (f == 0) return kNoRecord;
        *age = now - f;
        if (*age > kDrawnFrames) return kStale;
        float m[16];
        std::memcpy(m, d.m, sizeof m);
        for (float v : m)
            if (!std::isfinite(v)) return kStale;
        return rect_from(m, m[14], d.size, d.anchor, out) ? kFresh : kStale;
    }
    return kNoRecord;
}

// A drawn pane and its ancestors: entries and cursors are mostly Null panes and part instances ('CL_...') that hold the drawn pictures and
// texts but have no draw of their own (p5j: marking only the drawing panes left 2-4 visible panes on screens with dozens). An ancestor
// already marked this frame has its own ancestors marked too.
void mark_with_ancestors(uint64_t pane, uint32_t frame) {
    for (int depth = 0; pane && depth < 64; ++depth) {
        if (pane_mark(pane, frame)) return;
        pane = *reinterpret_cast<const uint64_t*>(pane + C().pane_parent);
    }
}

int hook_pane_draw(::Ctx* c, uint64_t, void* user) {
    const uint64_t pane = c->x[reinterpret_cast<uintptr_t>(user)];
    const uint32_t frame = g_frame.load(std::memory_order_relaxed);
    thread_local uint64_t last_pane_lay = 0;
    thread_local uint32_t last_frame = 0;
    if (!pane) return 0;
    const uint64_t lay = *reinterpret_cast<const uint64_t*>(pane + C().pane_layout);    // the draw itself reads it next
    g_draw_calls.fetch_add(1, std::memory_order_relaxed);
    if (!(*reinterpret_cast<const uint32_t*>(pane + C().pane_flags) & 0x4000)) {
        // the draw goes on only for panes it shows (0x100, no hidden ancestor, alpha); record those
        const uint32_t fl = *reinterpret_cast<const uint32_t*>(pane + C().pane_flags);
        bool shown = (fl & 0x100) && *reinterpret_cast<const uint8_t*>(pane + C().pane_alpha) != 0;
        for (uint64_t q = pane; shown && q; q = *reinterpret_cast<const uint64_t*>(q + C().pane_parent))
            if (*reinterpret_cast<const uint32_t*>(q + C().pane_flags) & 0x40) shown = false;
        if (shown) mark_with_ancestors(pane, frame);
    } else {
        mark_with_ancestors(pane, frame);
    }
    if (!lay || (lay == last_pane_lay && frame == last_frame)) return 0;
    last_pane_lay = lay;
    last_frame = frame;
    drawn_mark(lay, frame);
    return 0;
}

// The game's own visibility rule for a pane (the draw's checks above, without the draw-list membership).
bool pane_shown(uint64_t pane, uint32_t flags) {
    if (flags & 0x4000) return true;
    if (!(flags & 0x100)) return false;
    uint64_t q = pane;
    for (int depth = 0; q && depth < 64; ++depth) {
        if (!ok(q + C().pane_flags, 4)) return false;
        if (rd<uint32_t>(q + C().pane_flags) & 0x40) return false;
        q = ok(q + C().pane_parent, 8) ? rd<uint64_t>(q + C().pane_parent) : 0;
    }
    return true;
}

bool g_trace = false;
bool g_trace_pick = false;      // the scan traced: pick() logs its choice once
#define TRACE(...) do { if (g_trace) NXLOG_I(C().tag, "menu chain: " __VA_ARGS__); } while (0)

// One layout's visible panes (layout = placeable +40 -> +16).
void add_layout(uint64_t plc, uint32_t id, uint32_t sub, int trace_n) {
    State& s = st();
    const uint64_t obj = rptr(plc + C().placeable_obj);
    const uint64_t lay = rptr(obj + C().obj_layout);
    if (!lay || !ok(lay, C().lay_sentinel + 8)) {
        if (trace_n < 4) TRACE("    placeable 0x%llx -> obj 0x%llx -> layout 0x%llx: stop (layout not readable)", (unsigned long long)plc,
                               (unsigned long long)obj, (unsigned long long)lay);
        return;
    }
    for (const Layout& L : s.layouts)
        if (L.ptr == lay) return;
    if (draw_hooks_live() && !drawn_recent(lay)) { ++s.n_not_drawn; return; }
    Layout L;
    L.ptr = lay;
    L.id = id;
    L.sub = sub;
    const int n = rd<uint16_t>(lay + C().lay_pane_count);
    uint64_t node = rd<uint64_t>(lay + C().lay_first_node);
    const uint64_t first = node;
    const int li = static_cast<int>(s.layouts.size());
    int unreadable = 0, alpha0 = 0, offscreen = 0;
    // the candidates: the panes the draw's own rule shows (flag 0x100 / 0x4000, no hidden ancestor, alpha above 0), each with its draw record
    struct Cand { uint64_t pane; Pane p; int rec; };
    std::vector<Cand> cands;
    const bool live = draw_hooks_live();
    int fresh = 0;
    for (int k = 0; k < n && k < static_cast<int>(kMaxPanes) && node && node != lay + C().lay_sentinel; ++k) {
        if (!ok(node, 24)) break;
        const uint64_t pane = rd<uint64_t>(node + 16);
        node = rd<uint64_t>(node + 8);
        ++L.panes;
        Pane p;
        if (!pane_rect(pane, &p)) { ++unreadable; continue; }
        if (p.alpha == 0 || !pane_shown(pane, p.flags)) { ++alpha0; continue; }
        if (!live && !(p.flags & 0x20)) { ++alpha0; continue; }     // no hooks (not re-lifted): never went through a draw
        Cand cd{pane, p, kNoRecord};
        if (live) {
            ++s.rec_panes;
            Pane r;
            uint32_t age = 0;
            cd.rec = pane_record(pane, &r, &age);
            if (cd.rec != kNoRecord) ++s.rec_with;
            if (cd.rec == kStale) s.rec_ages.push_back(age);
            if (cd.rec == kFresh) {
                ++fresh;
                cd.p.x0 = r.x0; cd.p.y0 = r.y0; cd.p.x1 = r.x1; cd.p.y1 = r.y1; cd.p.z = r.z;     // the rect it was drawn at
            }
        }
        cands.push_back(cd);
    }
    // With fresh records in this layout, its panes without one (or with an old one) were not drawn: dropped. Without any (a pane class the
    // hooks do not cover, a draw on another cadence), the layout-level filter alone decides, as before the per-pane records.
    // Only when most of the layout's shown panes have records: some pane classes draw without going through the hooked draws (the bag's
    // empty slots: p5k kept 4 of ~70 inventory panes), and dropping their panes would lose the menu.
    const bool refine = live && fresh > 0 && fresh * 2 >= static_cast<int>(cands.size());
    if (live && !refine && !cands.empty()) ++s.rec_fallback_layouts;
    for (Cand& cd : cands) {
        if (refine && cd.rec != kFresh) { ++alpha0; continue; }
        const uint64_t pane = cd.pane;
        Pane& p = cd.p;
        if (p.x1 < 0.f || p.y1 < 0.f || p.x0 > C().canvas_w || p.y0 > C().canvas_h) { ++offscreen; continue; }     // off screen (or a world-space layout)
        p.ptr = pane;
        p.lay = li;
        p.lay_id = L.id;
        p.lay_sub = L.sub;
        read_str(rd<uint64_t>(pane + C().pane_name), rd<uint16_t>(pane + C().pane_name_len), p.name, sizeof p.name);
        char tag[48];
        read_str(rd<uint64_t>(pane + C().pane_tag), 0, tag, sizeof tag);
        classify(p, tag);
        ++L.visible;
        s.panes.push_back(p);
    }
    // candidate "drawn" fields, logged so one run with a menu open shows which differs (all inferred, none confirmed): the placeable's
    // atomic flag word (+488, tested by 0x7101fdc1e0), the layout's counters at +744..+756 (+748 = pending textures, 0x7101f55880;
    // +756 feeds pane flag 0x200 in 0x7101f56060), the root pane's flags (+260).
    if (trace_n < 6 && L.visible > 0) {
        const uint64_t pflags = ok(plc + 488, 8) ? rd<uint64_t>(plc + 488) : 0;
        uint32_t lf[4] = {};
        if (ok(lay + 744, 16)) std::memcpy(lf, reinterpret_cast<const void*>(lay + 744), sizeof lf);
        const uint64_t root = ok(first, 24) ? rd<uint64_t>(first + 16) : 0;
        const uint32_t rflags = ok(root + C().pane_flags, 4) ? rd<uint32_t>(root + C().pane_flags) : 0;
        TRACE("    layout %u/%u 0x%llx (placeable 0x%llx, obj 0x%llx): panes %d, visible %d (unreadable %d, alpha 0 %d, off screen %d); "
              "placeable+488 0x%llx, layout+744 %u +748 %u +752 %u +756 %u, root pane flags 0x%x",
              id, sub, (unsigned long long)lay, (unsigned long long)plc, (unsigned long long)obj, L.panes, L.visible, unreadable, alpha0, offscreen,
              (unsigned long long)pflags, lf[0], lf[1], lf[2], lf[3], rflags);
    }
    s.layouts.push_back(L);
}

// A pool's entries {entry** data, u64 count}; false (and the reason in the trace) when the list cannot be read.
bool pool_entries(uint64_t mgr, uint64_t pool, const char* name, uint64_t* data, uint64_t* count) {
    const uint64_t a = mgr + pool + C().pool_entries;
    if (!ok(a, 16)) { TRACE("  pool %s at 0x%llx: stop (entry list header 0x%llx not readable)", name, (unsigned long long)(mgr + pool), (unsigned long long)a); return false; }
    *data = rd<uint64_t>(a);
    *count = rd<uint64_t>(a + 8);
    TRACE("  pool %s at 0x%llx: entries data 0x%llx count %llu", name, (unsigned long long)(mgr + pool), (unsigned long long)*data,
          (unsigned long long)*count);
    if (!*data || *count == 0) { TRACE("  pool %s: stop (empty)", name); return false; }
    if (*count > kMaxPoolEntries || !ok(*data, *count * 8)) { TRACE("  pool %s: stop (count over %llu or data not readable)", name, (unsigned long long)kMaxPoolEntries); return false; }
    return true;
}

// Walks every screen layout the UI manager's pools hold and collects the visible panes.
void scan() {
    State& s = st();
    s.layouts.clear();
    s.panes.clear();
    if (s.pages.size() > 20000) s.pages.clear();
    {
        static std::chrono::steady_clock::time_point last{};
        const auto now = std::chrono::steady_clock::now();
        g_trace = mp::engine().log_level() >= 3 && mp::engine().log_lines() < 4000 && now - last >= std::chrono::seconds(1);
        if (g_trace) { last = now; mp::engine().add_log_lines(24); }
        g_trace_pick = g_trace;
    }
    s.n_not_drawn = 0;
    s.rec_panes = s.rec_with = s.rec_fallback_layouts = 0;
    s.rec_ages.clear();
    TRACE("drawn set: hooks %s, %llu pane draws so far, frame %u", draw_hooks_live() ? "live" : "silent (not re-lifted? panes need flag 0x20 instead)",
          (unsigned long long)g_draw_calls.load(), g_frame.load());
    const uint64_t gaddr = s.base + C().ui_mgr;
    const uint64_t mgr = rptr(gaddr);
    TRACE("global 0x%llx (main+0x%llx) readable %d -> UI manager 0x%llx", (unsigned long long)gaddr, (unsigned long long)C().ui_mgr, ok(gaddr, 8) ? 1 : 0,
          (unsigned long long)mgr);
    if (!mgr) { TRACE("stop (no UI manager)"); g_trace = false; return; }
    uint64_t data = 0, count = 0;
    // pool A: per layout resource, the instances in use (all minus the free ring)
    if (pool_entries(mgr, C().pool_a, "A", &data, &count)) {
        int traced = 0;
        for (uint64_t i = 0; i < count; ++i) {
            const uint64_t e = rd<uint64_t>(data + 8 * i);
            if (!ok(e, 48)) { if (i < 4) TRACE("   A[%llu] entry 0x%llx: stop (not readable)", (unsigned long long)i, (unsigned long long)e); continue; }
            const uint32_t id = rd<uint32_t>(e + 8), sub = rd<uint32_t>(e + 12);
            const uint64_t vec = rd<uint64_t>(e + C().ent_instances), ring = rd<uint64_t>(e + C().ent_free);
            uint64_t vdata = 0, vcount = 0;
            if (ok(vec, 16)) { vdata = rd<uint64_t>(vec); vcount = rd<uint64_t>(vec + 8); }
            uint64_t free_ptrs[kMaxInstances];
            size_t nfree = 0;
            uint64_t rcount = 0;
            if (ok(ring, 40)) {
                const uint64_t rdata = rd<uint64_t>(ring), rsize = rd<uint64_t>(ring + 8), rcap = rd<uint64_t>(ring + 16), rhead = rd<uint64_t>(ring + 24);
                rcount = rd<uint64_t>(ring + 32);
                if (rdata && rsize && rcap && rcount <= kMaxInstances && rsize <= 4096 && ok(rdata, rsize * 8))
                    for (uint64_t k = 0; k < rcount; ++k) {
                        const uint64_t idx = std::min((rhead + k) % rcap, rsize - 1);      // 0x710087da00's indexing
                        free_ptrs[nfree++] = rd<uint64_t>(rdata + 8 * idx);
                    }
            }
            if (i < 4)
                TRACE("   A[%llu] entry 0x%llx id %u sub %u: instances 0x%llx {data 0x%llx count %llu}, free ring 0x%llx count %llu", (unsigned long long)i,
                      (unsigned long long)e, id, sub, (unsigned long long)vec, (unsigned long long)vdata, (unsigned long long)vcount,
                      (unsigned long long)ring, (unsigned long long)rcount);
            if (!vdata || vcount == 0 || vcount > kMaxInstances || !ok(vdata, vcount * 8)) continue;
            for (uint64_t k = 0; k < vcount; ++k) {
                const uint64_t plc = rd<uint64_t>(vdata + 8 * k);
                if (!plc || std::find(free_ptrs, free_ptrs + nfree, plc) != free_ptrs + nfree) continue;     // free: not on screen
                add_layout(plc, id, sub, traced);
                if (!s.layouts.empty() && s.layouts.back().ptr && s.layouts.back().visible > 0) ++traced;
            }
        }
    }
    // pool B: entries are placeables
    if (pool_entries(mgr, C().pool_b, "B", &data, &count)) {
        for (uint64_t i = 0; i < count; ++i) {
            const uint64_t e = rd<uint64_t>(data + 8 * i);
            if (!ok(e, 16)) continue;
            add_layout(rd<uint64_t>(e), rd<uint32_t>(e + 8), rd<uint32_t>(e + 12), static_cast<int>(i));
        }
    }
    TRACE("result: %zu layouts drawn (%d pooled ones not drawn skipped), %zu visible panes", s.layouts.size(), s.n_not_drawn, s.panes.size());
    if (g_trace) {
        std::vector<uint32_t> a = s.rec_ages;
        std::sort(a.begin(), a.end());
        TRACE("pane records: %d shown panes in drawn layouts, %d with a record, %d fresh, %zu too old (age min %u median %u max %u frames), "
              "%d layouts on the layout-level fallback, %llu marks dropped (table full), frame %u",
              s.rec_panes, s.rec_with, s.rec_with - static_cast<int>(a.size()), a.size(), a.empty() ? 0u : a.front(), a.empty() ? 0u : a[a.size() / 2],
              a.empty() ? 0u : a.back(), s.rec_fallback_layouts, (unsigned long long)g_pane_table_full.load(), g_frame.load());
    }
    if (g_trace) {
        char ids[900];
        size_t k = 0;
        ids[0] = 0;
        for (const Layout& L : s.layouts) {
            if (k + 24 >= sizeof ids) break;
            const int m = std::snprintf(ids + k, sizeof ids - k, " %u/%u(%d)", L.id, L.sub, L.visible);
            if (m > 0) k += static_cast<size_t>(m);
        }
        TRACE("drawn layouts id/sub(visible panes):%s", ids);
        // every visible pane of the drawn layouts, once each time their number changes (names for the cursor / entry rules)
        static size_t dumped = static_cast<size_t>(-1);
        if (s.panes.size() != dumped && s.panes.size() <= 400) {
            dumped = s.panes.size();
            for (const Pane& p : s.panes)
                TRACE("  pane %u/%u '%s'%s%s rect %.0f,%.0f..%.0f,%.0f", p.lay_id, p.lay_sub, p.name, p.item ? " [item]" : "", p.cursor ? " [cursor]" : "",
                      p.x0, p.y0, p.x1, p.y1);
        }
    }
    g_trace = false;
}

// The entry under the pointer, the cursor and the entry it marks.
bool advance_mark(const Pane& p) {
    // a message box's "next" arrow is a cursor pane with no choice behind it (p5d: 'CL_0000_parts_cursor_triangle' over '0000_btn_common_1')
    std::string n = p.name;
    for (char& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return has(n, "triangle");
}

// Menus outside play (title, settings, language, save and yes / no dialogs) build their entries from parts, and in KTGL each part instance is
// a layout of its own in the pool (the bag's triangle 4148535219/43 is one too). Pane names come from the layout data only: main holds no
// pane-name string at all (no "cursor", "Win_", "btn_" bytes in the image), the screens are driven by their scripts, and no selection field
// could be read from code, so the selection is still read off the cursor panes. Two shapes the cursor rule above misses (from the
// settings / language layouts listed by p5d / p5e, menu_log 2):
//   * settings: the list 434523034 holds 'CL_0020_parts_setting_menu_btn_0..3'; each button is a part layout (2408049848) with its own
//     'Win_cursor' highlight and entries ('Null_type_1tab', 'CL_0000_parts_btn_choices' ...). The cursor rule picks an entry of that part,
//     and hover then stays inside the one button. Rule 1 (promotion): when the chosen entry's layout is small (at most 4 entries) and the
//     entry sits inside an entry of another list layout, the selection is that list entry (the smallest holding the entry's centre), so the
//     hover covers the whole list. Also, an entry chosen by a non-triangle cursor that has a smaller entry of its layout under the cursor's
//     centre gives way to it (a list frame 'posi_list' around the buttons).
//   * yes / no, language and save choices: each choice is an instance of one button layout ('Null_btn', '0000_btn_common_N', and the
//     language's '0030_prt_language_1'), the selected one showing 'CL_0000_parts_cursor_triangle' at its left; the cursor rule takes that
//     triangle for a message box's advance arrow (under 3 entries in its layout). Rule 2 (siblings): when no cursor was found, a cursor
//     whose layout has at least one other drawn instance (same layout id) is the selection; the entries are each instance's largest entry
//     (they must not all sit on one spot), and hover / steps move between them. Draw hooks only (pooled templates all look visible).
// The in-game menus seen so far (bag: 41 entries, chat choices: 8) never reach either rule, so their path is unchanged.
bool ignored_layout(const Pane& p) {
    const State& s = st();
    return !s.ig_ignore.empty() && s.ig_ignore.count(static_cast<uint64_t>(p.lay_id) << 32 | p.lay_sub);
}

void pick_extend(const std::vector<int>& items, float big) {
    State& s = st();
    s.mode = 0;
    s.group.clear();
    const int nl = static_cast<int>(items.size());
    if (s.focused >= 0 && s.cursor >= 0) {
        const Pane& cp = s.panes[s.cursor];
        // a frame entry around several entries ('posi_list'): the entry of that layout under the cursor's centre
        if (!advance_mark(cp)) {
            const Pane& f = s.panes[s.focused];
            int inner = 0, under = -1;
            for (size_t i = 0; i < s.panes.size(); ++i) {
                const Pane& p = s.panes[i];
                if (static_cast<int>(i) == s.focused || !p.item || p.lay != f.lay || p.area() >= f.area()) continue;
                if (!f.contains(p.cx(), p.cy())) continue;
                ++inner;
                if (p.contains(cp.cx(), cp.cy()) && (under < 0 || p.area() < s.panes[under].area())) under = static_cast<int>(i);
            }
            if (inner >= 2 && under >= 0) s.focused = under;
        }
        // rule 1: the chosen entry belongs to a part (a small layout) placed in an entry of a list layout
        const Pane& f = s.panes[s.focused];
        if (f.lay >= 0 && f.lay < nl && items[f.lay] <= 4) {
            int up = -1;
            for (size_t i = 0; i < s.panes.size(); ++i) {
                const Pane& p = s.panes[i];
                if (!p.item || p.area() > big || p.lay < 0 || p.lay >= nl || p.lay == f.lay || items[p.lay] < 2) continue;
                if (!p.contains(f.cx(), f.cy()) || f.area() > p.area() * 1.5f) continue;
                if (up < 0 || p.area() < s.panes[up].area()) up = static_cast<int>(i);
            }
            if (up >= 0) { s.focused = up; s.mode = 1; }
        }
        return;
    }
    // rule 2: sibling instances of one button layout, the cursor in one of them
    if (!draw_hooks_live()) return;
    std::vector<int> rep(s.layouts.size(), -1);       // each layout's largest entry
    for (size_t i = 0; i < s.panes.size(); ++i) {
        const Pane& p = s.panes[i];
        if (!p.item || p.area() > big || p.lay < 0 || p.lay >= static_cast<int>(rep.size())) continue;
        if (rep[p.lay] < 0 || p.area() > s.panes[rep[p.lay]].area()) rep[p.lay] = static_cast<int>(i);
    }
    float best = 160.f;
    for (size_t c = 0; c < s.panes.size(); ++c) {
        const Pane& cp = s.panes[c];
        if (!cp.cursor || cp.area() > big || cp.lay < 0 || cp.lay >= static_cast<int>(rep.size()) || ignored_layout(cp)) continue;
        const int mine = rep[cp.lay];
        if (mine < 0) continue;
        const uint32_t id = s.layouts[cp.lay].id;
        std::vector<int> g;
        int apart = 0;
        for (size_t li = 0; li < s.layouts.size(); ++li) {
            if (s.layouts[li].id != id || rep[li] < 0) continue;
            g.push_back(rep[li]);
            const Pane& q = s.panes[rep[li]];
            if (std::fabs(q.cx() - s.panes[mine].cx()) > 8.f || std::fabs(q.cy() - s.panes[mine].cy()) > 8.f) ++apart;
        }
        if (g.size() < 2 || apart == 0) continue;
        const float d = s.panes[mine].dist(cp.cx(), cp.cy());
        if (d > best) continue;
        best = d;
        s.cursor = static_cast<int>(c);
        s.focused = mine;
        s.group = std::move(g);
        s.mode = 2;
    }
}

// The game's cursor: a visible cursor pane (not a message box's advance arrow, not in an ignored layout) whose own layout has at least two
// entries, one of them within 160 px; of those, the closest. The entry under the pointer only counts in the cursor's layout (pooled layouts
// that are not drawn still look visible: their panes keep alpha 255 and a rectangle).
void pick() {
    State& s = st();
    s.focused = s.cursor = -1;
    const float big = C().canvas_w * C().canvas_h * 0.5f;
    std::vector<int> items(s.layouts.size(), 0);
    for (const Pane& p : s.panes)
        if (p.item && p.area() <= big && p.lay >= 0 && p.lay < static_cast<int>(items.size())) ++items[p.lay];
    float best = 1e30f;
    int rejected_arrow = 0, rejected_lone = 0;
    for (size_t c = 0; c < s.panes.size(); ++c) {
        const Pane& cp = s.panes[c];
        if (!cp.cursor || cp.area() > big) continue;
        if (!s.ig_ignore.empty() && s.ig_ignore.count(static_cast<uint64_t>(cp.lay_id) << 32 | cp.lay_sub)) continue;
        if (cp.lay < 0 || cp.lay >= static_cast<int>(items.size())) { ++rejected_lone; continue; }
        // The cursor's own layout holds the entries, or (only with the draw hooks live, so every layout here is on screen) the cursor is a
        // layout of its own over another drawn layout's list: the bag's triangle (615988817/11) sits on slot 0 of the inventory 4084873301/154.
        const bool own = items[cp.lay] >= 2;
        if (!own && !draw_hooks_live()) { ++rejected_lone; continue; }
        for (size_t i = 0; i < s.panes.size(); ++i) {
            const Pane& p = s.panes[i];
            if (!p.item || p.area() > big || p.lay < 0) continue;
            if (own ? p.lay != cp.lay : items[p.lay] < 2) continue;
            // a triangle is a message box's advance arrow unless it marks an entry of a real list (3+ entries)
            if (advance_mark(cp) && items[p.lay] < 3) continue;
            // a triangle pointing down ('..._180_..', the bag) sits above its entry: its tip, a quarter of its height below the bottom, is
            // where it points; ties between overlapping slots go to the nearer centre
            const bool down = advance_mark(cp) && has(std::string(cp.name), "180");
            const float qx = cp.cx(), qy = down ? cp.y1 + (cp.y1 - cp.y0) * 0.25f : cp.cy();
            const float d = p.dist(qx, qy) + 0.001f * std::hypot(p.cx() - qx, p.cy() - qy);
            // on a tie (the cursor over a row container and its slot) the smaller entry wins
            if (d < best || (d == best && s.focused >= 0 && p.area() < s.panes[s.focused].area())) {
                best = d;
                s.cursor = static_cast<int>(c);
                s.focused = static_cast<int>(i);
            }
        }
        if (advance_mark(cp) && (s.cursor != static_cast<int>(c))) ++rejected_arrow;
    }
    if (best > 160.f) s.cursor = s.focused = -1;
    pick_extend(items, big);
    if (g_trace_pick) {
        g_trace_pick = false;
        if (s.cursor >= 0) {
            const Pane& c = s.panes[s.cursor];
            NXLOG_I(C().tag, "menu chain: chosen layout %u/%u (cursor '%s' on '%s', %d entries in it); skipped %d advance arrows, %d cursors in layouts "
                    "with under 2 entries", c.lay_id, c.lay_sub, c.name, s.panes[s.focused].name, items[c.lay], rejected_arrow, rejected_lone);
        } else {
            NXLOG_I(C().tag, "menu chain: no cursor chosen; skipped %d advance arrows, %d cursors in layouts with under 2 entries", rejected_arrow,
                    rejected_lone);
        }
    }
}

void log_layouts() {
    State& s = st();
    uint64_t sig = 0;
    int vis = 0, items = 0, cursors = 0;
    for (const Layout& L : s.layouts)
        if (L.visible) sig = sig * 1000003ull + (static_cast<uint64_t>(L.id) << 32 | L.sub) + 1;
    for (const Pane& p : s.panes) { ++vis; items += p.item; cursors += p.cursor; }
    s.n_layouts.store(static_cast<uint32_t>(s.layouts.size()), std::memory_order_relaxed);
    s.n_panes.store(static_cast<uint32_t>(vis), std::memory_order_relaxed);
    s.n_items.store(static_cast<uint32_t>(items), std::memory_order_relaxed);
    const bool opened = !s.was_up;
    if ((opened || sig != s.layout_sig) && log_ok(opened)) {
        NXLOG_I(C().tag, "menu: %s, %zu layouts (%zu with visible panes), %d visible panes, %d entries, %d cursors", opened ? "open" : "changed",
                s.layouts.size(), static_cast<size_t>(std::count_if(s.layouts.begin(), s.layouts.end(), [](const Layout& L) { return L.visible > 0; })),
                vis, items, cursors);
    }
    s.layout_sig = sig;
    if (mp::engine().log_level() < 2) return;
    // menu_log = 2: each layout's visible panes, once per layout
    for (size_t li = 0; li < s.layouts.size(); ++li) {
        const Layout& L = s.layouts[li];
        const uint64_t key = static_cast<uint64_t>(L.id) << 32 | L.sub;
        if (!L.visible || s.dumped.count(key) || s.dumped.size() > 200 || mp::engine().log_lines() > 4000) continue;
        s.dumped.insert(key);
        NXLOG_I(C().tag, "menu: layout id=%u sub=%u ptr=0x%llx panes=%d visible=%d", L.id, L.sub, static_cast<unsigned long long>(L.ptr), L.panes,
                L.visible);
        int shown = 0;
        for (const Pane& p : s.panes) {
            if (p.lay != static_cast<int>(li) || shown >= 60) continue;
            ++shown;
            mp::engine().add_log_lines(1);
            NXLOG_I(C().tag, "menu:   '%s'%s%s%s rect %.0f,%.0f..%.0f,%.0f z=%.1f a=%u flags=0x%x", p.name, p.item ? " [entry]" : "",
                    p.cursor ? " [cursor]" : "", p.slider ? " [slider]" : "", p.x0, p.y0, p.x1, p.y1, p.z, p.alpha, p.flags);
        }
    }
}

// While the game is in play its menus (bag, crafting, shops, choice boxes) are KTGL layouts over the world and the game keeps calling its
// input function, so play mode cannot tell them from play. A layout with the game's selection cursor on an entry (pick's rule: a visible
// "...cursor..." pane within 160 px of an "item" pane) is such a menu (the engine's Gate polls it).
// A drawn key guide layout from menu_key_guides (the title's "(-) Settings"): a screen with no selection cursor that still takes clicks.
// Gameplay's HUD can draw the same key guide layout, so it counts only while Config::world_live says the player is not in the world.
bool key_guide_drawn(const State& s) {
    if (s.key_guides.empty() || (C().world_live && C().world_live())) return false;
    for (const Layout& L : s.layouts)
        if (L.visible > 0 && s.key_guides.count(L.id)) return true;
    return false;
}

bool present(const State& s) { return (s.cursor >= 0 && s.focused >= 0) || key_guide_drawn(s); }

// The gate's own poll: one scan of the screen.
bool poll_scan(void*) {
    State& s = st();
    s.pages.clear();
    scan();
    pick();
    if (mp::engine().log_level() >= 3) log_layouts();   // what play-time screens (the title too) draw
    return present(s);
}

bool ready(void*) { return st().installed && st().enabled; }

void idle(void*) {
    State& s = st();
    if (s.was_up && log_ok(true)) NXLOG_I(C().tag, "menu: closed");
    s.was_up = false;
    s.panes.clear();
    s.focused = s.cursor = -1;
}

// The frame's elements: every visible pane of the drawn layouts, and the selection pick() found.
bool collect(mp::Engine& e, float, float, bool, void*) {
    State& s = st();
    s.pages.clear();
    scan();
    pick();
    const bool up = present(s);
    log_layouts();
    s.was_up = true;
    {   // which rule found the selection (pick_extend), once per change: the proof for menus outside play
        const Pane* c = s.cursor >= 0 ? &s.panes[s.cursor] : nullptr;
        const Pane* f = s.focused >= 0 ? &s.panes[s.focused] : nullptr;
        const uint64_t sig = c && f ? (static_cast<uint64_t>(s.mode) << 62) ^ (static_cast<uint64_t>(c->lay_id) << 32) ^ f->lay_id ^
                                          (static_cast<uint64_t>(s.group.size()) << 48)
                                    : 0;
        if (sig != s.mode_sig && c && f && log_ok(true)) {
            static const char* const kMode[] = {"cursor", "list entry holding the cursor's part", "sibling button layouts"};
            NXLOG_I(C().tag, "menu: selection by %s: cursor '%s' (layout %u/%u) on '%s' (layout %u/%u), %zu sibling entries", kMode[s.mode], c->name,
                    c->lay_id, c->lay_sub, f->name, f->lay_id, f->lay_sub, s.group.size());
        }
        s.mode_sig = sig;
    }
    // Hover rules: with sibling button layouts (mode 2) the group's entries; else the entries of the selected entry's layout (it may differ
    // from the cursor's), or of any layout with no selection. Sliders: the same layout rule, not in mode 2.
    const float big = C().canvas_w * C().canvas_h * 0.5f;
    const int flay = s.focused >= 0 ? s.panes[s.focused].lay : -1;
    std::vector<char> in_group;
    if (s.mode == 2) {
        in_group.assign(s.panes.size(), 0);
        for (int i : s.group)
            if (i >= 0 && i < static_cast<int>(in_group.size())) in_group[i] = 1;
    }
    for (size_t i = 0; i < s.panes.size(); ++i) {
        const Pane& p = s.panes[i];
        mp::Element el;
        el.key = p.ptr;
        el.x0 = p.x0;
        el.y0 = p.y0;
        el.x1 = p.x1;
        el.y1 = p.y1;
        el.layout = p.lay_id;
        el.sub = p.lay_sub;
        std::memcpy(el.name, p.name, sizeof el.name);
        if (s.mode == 2) {
            if (in_group[i]) el.flags |= mp::kHover;
        } else {
            const bool lay_ok = flay < 0 || p.lay == flay;
            if (lay_ok && p.slider) el.flags |= mp::kSlider;
            if (lay_ok && p.item && p.area() <= big) el.flags |= mp::kHover;
        }
        if (p.item && !s.key_guides.empty()) {
            const auto it = s.key_guides.find(p.lay_id);
            if (it != s.key_guides.end()) el.click = it->second;
        }
        e.add(el);
    }
    e.set_selection(s.focused, s.cursor >= 0, s.cursor >= 0 ? s.panes[s.cursor].name : "");
    return up;
}

mp::Adapter& adapter() {
    static mp::Adapter a;
    return a;
}

}  // namespace

void install(uint64_t base, const Config& cfg) {
    g_cfg = &cfg;
    State& s = st();
    s.base = base;
    const char* sec = cfg.section;
    s.enabled = config_get_bool(sec, "menu_mouse", true);
    mp::engine().set_log_level(static_cast<int>(config_get_int(sec, "menu_log", 1)));
    s.ig_enabled = config_get_bool(sec, "menu_in_game", true);
    s.ig_ignore.clear();
    {   // "id/sub id/sub ..." (the ids the "in-game menu up" log line prints)
        const std::string list = config_get_str(sec, "menu_in_game_ignore", "");
        unsigned id = 0, sub = 0;
        int used = 0;
        for (const char* q = list.c_str(); *q;) {
            if (std::sscanf(q, " %u/%u%n", &id, &sub, &used) == 2 && used > 0) {
                s.ig_ignore.insert(static_cast<uint64_t>(id) << 32 | sub);
                q += used;
            } else {
                ++q;
            }
        }
    }
    s.key_guides.clear();
    {   // "id=act ..." (act names as in the [switch] section: btn_minus, btn_plus, btn_x ...); "" turns them off
        const std::string list = config_get_str(sec, "menu_key_guides", cfg.default_key_guides ? cfg.default_key_guides : "");
        char name[32];
        unsigned id = 0;
        int used = 0;
        for (const char* q = list.c_str(); *q;) {
            Act a;
            if (std::sscanf(q, " %u=%31[A-Za-z0-9_]%n", &id, name, &used) == 2 && used > 0) {
                if (parse_act(name, &a)) s.key_guides[id] = a;
                else NXLOG_W(cfg.tag, "menu_key_guides: unknown action '%s'", name);
                q += used;
            } else {
                ++q;
            }
        }
    }
    for (size_t i = 0; i < cfg.draw_site_count; ++i)
        g_draw_hooks.push_back(hook_add_mid(base + cfg.draw_sites[i].off, &hook_pane_draw,
                                            reinterpret_cast<void*>(static_cast<uintptr_t>(cfg.draw_sites[i].reg))));
    mp::Adapter& a = adapter();
    a.tag = cfg.tag;
    a.canvas_w = cfg.canvas_w;
    a.canvas_h = cfg.canvas_h;
    a.ready = &ready;
    a.collect = &collect;
    a.idle = &idle;
    a.focus = nullptr;      // KTGL menus move their selection with the d-pad only
    a.confirm = nullptr;    // and confirm with A
    s.installed = true;
    NXLOG_I(cfg.tag, "menu mouse %s (hover moves the game's cursor, click confirms; [%s] menu_log %d; 3 = the layout chain once a second)",
            s.enabled ? "on" : "off", sec, mp::engine().log_level());
}

void remove() {
    for (uint32_t id : g_draw_hooks) hook_remove(id);
    g_draw_hooks.clear();
    st().installed = false;
    st().gate.reset();
    mp::engine().reset();
}

bool menu_in_game(bool in_game) {
    State& s = st();
    g_frame.fetch_add(1, std::memory_order_relaxed);     // the drawn set's clock (once per play-mode frame)
    if (mp::engine().log_level() >= 3) {   // once a second: why the menu code does or does not look at this frame
        static uint32_t n = 0;
        if (++n % 600 == 0)
            NXLOG_I(C().tag, "menu: poll in_game %d installed %d enabled %d in_game_menus %d up %d layouts %zu", in_game ? 1 : 0, s.installed ? 1 : 0,
                    s.enabled ? 1 : 0, s.ig_enabled ? 1 : 0, s.gate.up() ? 1 : 0, s.layouts.size());
    }
    mp::Gate::Event ev = mp::Gate::kNone;
    const bool up = s.gate.poll(in_game && s.installed && s.enabled && s.ig_enabled, mp::engine(), &poll_scan, nullptr, &ev);
    if (ev == mp::Gate::kLeft) {
        if (log_ok(true)) NXLOG_I(C().tag, "menu: in-game menu closed (layout %u/%u, the game left play)", s.ig_lay_id, s.ig_lay_sub);
    } else if (ev == mp::Gate::kClosed) {
        if (log_ok(true)) NXLOG_I(C().tag, "menu: in-game menu closed (layout %u/%u, cursor '%s')", s.ig_lay_id, s.ig_lay_sub, s.ig_cursor);
    } else if (ev == mp::Gate::kOpened) {
        if (s.cursor < 0 || s.focused < 0) {   // a key guide screen (title): no cursor
            s.ig_lay_id = s.ig_lay_sub = 0;
            std::snprintf(s.ig_cursor, sizeof s.ig_cursor, "%s", "(key guide)");
            if (log_ok(true))
                NXLOG_I(C().tag, "menu: in-game menu up: a key guide screen with no cursor (%zu layouts); clicks go to the key guides, else A", s.layouts.size());
        } else {
            const Pane& c = s.panes[s.cursor];
            const Pane& f = s.panes[s.focused];
            s.ig_lay_id = c.lay_id;
            s.ig_lay_sub = c.lay_sub;
            std::snprintf(s.ig_cursor, sizeof s.ig_cursor, "%s", c.name);
            if (log_ok(true))
                NXLOG_I(C().tag,
                        "menu: in-game menu up: layout %u/%u (cursor '%s' on '%s', entry layout %u/%u), %zu layouts, %zu visible panes; "
                        "the mouse is the menu pointer ([%s] menu_in_game_ignore = %u/%u if this is not a menu)",
                        c.lay_id, c.lay_sub, c.name, f.name, f.lay_id, f.lay_sub, s.layouts.size(), s.panes.size(), C().section, c.lay_id, c.lay_sub);
        }
    }
    return up;
}

Act menu_frame(bool menu_up, const mm::Geometry& g) { return mp::engine().frame(menu_up, g, adapter()); }

bool menu_over_value() { return mp::engine().over_value(); }

bool menu_active() { return mp::engine().active(); }

bool menu_open() { return mp::engine().marker(); }

void status_text(char* buf, size_t n) {
    const State& s = st();
    const mp::Engine& e = mp::engine();
    std::snprintf(buf, n,
                  " menu_layouts=%u menu_panes=%u menu_entries=%u hover='%s' selected='%s' steps=%u clicks=%u in_game_menu=%d (%u/%u) in_game_opens=%u",
                  s.n_layouts.load(), s.n_panes.load(), s.n_items.load(), e.hover_name(), e.focus_name(), e.steps(), e.clicks(), s.gate.up() ? 1 : 0,
                  s.ig_lay_id, s.ig_lay_sub, s.gate.opens());
}

}  // namespace nxrt::play::ktgl
