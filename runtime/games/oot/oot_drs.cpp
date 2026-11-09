// SPDX-License-Identifier: GPL-3.0-or-later
// Ocarina of Time: the game's dynamic resolution, pinned to a set internal size (user rule: the render resolution stays at the set
// resolution; only an upscaler may lower it).
//
// What the game does (main 2.0.0): the renderer object R = [[main+0x4e30498] + 0x228] owns a resolution controller C = [R + 0xb48]
// (main+0x805aa0 is its per-frame step, called from main+0x71031d3ee0). C holds a list of levels (end node at C+400, first node at C+408;
// node: scale float at -8, level u32 at -4, next at +8): level 0 = 0.75 ... level 9 = 0.3334 of the output height (R+3664 = 1440), and
// the current level at C+372, the previous one at C+376. The step predicts the GPU cost of each level from the frame times it measured
// (render-thread record times here: ReportCounter timestamps are taken when the command is recorded) and returns the chosen level's
// scale; the renderer takes it into R+3676 (R+3672 * scale / R+3664) only when the level changed. Seen: the first scene sat at level 9
// (854x480) on an RTX 4080. The console's maximum is level 0 = 0.75, 1920x1080 under the 2560x1440 output of the game's own upscaler.
//
// The pin: every level's scale is set to the configured one, so whatever level the controller picks, its scale is that one; and while
// R+3676 differs from it, C+376 is made to differ from C+372 so the renderer takes the scale over at its next step. The game's own code
// does the rest (render target sizes, the upscaler network variant). [oot] render_height = 1080 (default; 0 = the game's dynamic
// resolution), clamped to the game's own range of levels.
#include "oot_game.h"

#include "nxrt/config.h"
#include "nxrt/hooks.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"

#include <cmath>
#include <cstring>

namespace nxrt::games::oot {
namespace {

constexpr const char* kTag = "oot";
constexpr uint64_t kFrameUpdate = 0x3b4ea90;    // the framework's per-frame virtual (oot_game.cpp), main thread
constexpr uint64_t kRendererGlobal = 0x4e30498;  // main data: a pointer to the object that holds the renderer at +0x228
constexpr uint64_t kRendererField = 0x228;
constexpr uint64_t kControllerField = 2888;      // R + 0xb48
constexpr uint64_t kOutHeight = 3664;            // R: float, the output height (1440)
constexpr uint64_t kScale = 3676;                // R: float, the scale in force
constexpr uint64_t kLevelEnd = 400, kLevelFirst = 408, kLevelNow = 372, kLevelPrev = 376;
constexpr int kMaxLevels = 64;

uint64_t g_base = 0;
uint32_t g_hook = 0;
int g_height = 1080;
uint64_t g_renderer = 0, g_controller = 0;   // the pair checked last
bool g_ok = false;
float g_scale = 0.0f;

uint64_t r64(uint64_t a) { uint64_t v; std::memcpy(&v, reinterpret_cast<const void*>(a), 8); return v; }
uint32_t r32(uint64_t a) { uint32_t v; std::memcpy(&v, reinterpret_cast<const void*>(a), 4); return v; }
float rf(uint64_t a) { float v; std::memcpy(&v, reinterpret_cast<const void*>(a), 4); return v; }
void wf(uint64_t a, float v) { std::memcpy(reinterpret_cast<void*>(a), &v, 4); }
void w32(uint64_t a, uint32_t v) { std::memcpy(reinterpret_cast<void*>(a), &v, 4); }

bool heap_ptr(uint64_t p) { return (p & 7) == 0 && p >= mem::kHeapBase && p < mem::kHeapBase + mem::kHeapMax; }

// Checks the controller's level list once per new object pair (VirtualQuery per node) and picks the pinned scale.
bool check(uint64_t r, uint64_t c) {
    if (!mem::is_mapped(r, kScale + 4) || !mem::is_mapped(c, kLevelFirst + 8)) return false;
    float lo = 1e9f, hi = 0.0f;
    int n = 0;
    for (uint64_t x = r64(c + kLevelFirst); x != c + kLevelEnd; x = r64(x + 8)) {
        if (!heap_ptr(x) || !mem::is_mapped(x - 8, 24) || ++n > kMaxLevels) return false;
        const float v = rf(x - 8);
        if (!(v > 0.05f && v <= 1.0f) || r32(x - 4) > kMaxLevels) return false;
        lo = std::fmin(lo, v);
        hi = std::fmax(hi, v);
    }
    const float out_h = rf(r + kOutHeight);
    if (n < 1 || !(out_h > 100.0f && out_h < 10000.0f)) return false;
    float s = static_cast<float>(g_height) / out_h;
    s = std::fmin(std::fmax(s, lo), hi);
    g_scale = s;
    const float out_w = out_h * 16.0f / 9.0f;
    NXLOG_I(kTag, "render resolution pinned to %dx%d (scale %.4f of the %.0fx%.0f output; the game's %d levels go from %.4f to %.4f, "
            "[oot] render_height = %d); renderer 0x%llx, controller 0x%llx at level %u",
            static_cast<int>(std::lround(out_w * s)), static_cast<int>(std::lround(out_h * s)), static_cast<double>(s),
            static_cast<double>(out_w), static_cast<double>(out_h), n, static_cast<double>(lo), static_cast<double>(hi), g_height,
            static_cast<unsigned long long>(r), static_cast<unsigned long long>(c), r32(c + kLevelNow));
    return true;
}

int frame_hook(::Ctx*, uint64_t, void*) {
    // main's data is mapped in the game, not necessarily in a test that fakes the frame hook
    static const bool global_ok = mem::is_mapped(g_base + kRendererGlobal, 8);
    if (!global_ok) return 0;
    const uint64_t holder = r64(g_base + kRendererGlobal);
    if (!heap_ptr(holder)) return 0;
    static uint64_t holder_checked = 0;
    if (holder != holder_checked) {
        if (!mem::is_mapped(holder + kRendererField, 8)) return 0;
        holder_checked = holder;
    }
    const uint64_t r = r64(holder + kRendererField);
    if (!heap_ptr(r)) return 0;
    if (r != g_renderer && !mem::is_mapped(r, kScale + 4)) return 0;
    const uint64_t c = r64(r + kControllerField);
    if (!heap_ptr(c)) return 0;
    if (r != g_renderer || c != g_controller) {
        g_renderer = r;
        g_controller = c;
        g_ok = check(r, c);
        if (!g_ok) NXLOG_W(kTag, "render resolution: renderer 0x%llx / controller 0x%llx do not look as expected; the game's dynamic resolution stays",
                           static_cast<unsigned long long>(r), static_cast<unsigned long long>(c));
    }
    if (!g_ok) return 0;
    int n = 0;
    for (uint64_t x = r64(c + kLevelFirst); x != c + kLevelEnd && n < kMaxLevels; x = r64(x + 8), ++n)
        if (rf(x - 8) != g_scale) wf(x - 8, g_scale);
    static uint64_t frames = 0, forced = 0;
    if (std::fabs(rf(r + kScale) - g_scale) > 1e-4f) {   // "the level changed": the renderer takes the scale over at its next step
        w32(c + kLevelPrev, r32(c + kLevelNow) ^ 1u);
        ++forced;
    }
    if (++frames % 3600 == 0)
        NXLOG_I(kTag, "render resolution: scale in force %.4f, level %u, %llu forced updates in %llu frames", static_cast<double>(rf(r + kScale)),
                r32(c + kLevelNow), static_cast<unsigned long long>(forced), static_cast<unsigned long long>(frames));
    return 0;
}

}  // namespace

void drs_install(uint64_t base) {
    g_base = base;
    g_height = static_cast<int>(config_get_int("oot", "render_height", 1080));
    if (g_height <= 0) {
        NXLOG_I(kTag, "render resolution: the game's dynamic resolution ([oot] render_height = 0)");
        return;
    }
    if (!g_hook) g_hook = hook_add_entry(base + kFrameUpdate, &frame_hook);
}

void drs_remove() {
    if (g_hook) hook_remove(g_hook);
    g_hook = 0;
    g_renderer = g_controller = 0;
    g_ok = false;
}

}  // namespace nxrt::games::oot
