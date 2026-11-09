// SPDX-License-Identifier: GPL-3.0-or-later
// nvnQueuePresentTexture: the window texture is drawn into the Renderer's frame (swapchain or headless image) and presented.
// The shell's FrameLimiter, when given, paces the frame (begin_frame before the image is acquired).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <emmintrin.h>
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "nxrt/config.h"
#include "nxrt/diag.h"
#include "nxrt/extras.h"
#include "nxrt/lowlat.h"
#include "extras/output_pass.h"
#include "nxrt/frame_limiter.h"
#include "nxrt/gauge.h"
#include "nxrt/ftrace.h"
#include "nxrt/log.h"
#include "nvn_gpu_internal.h"
#include "nvn_cmdq.h"
#include "nvn_arch_exp.h"

namespace nxrt::nvn::be::gpu {

namespace {
struct CaptureEnd {
    ~CaptureEnd() {
        trace_dump_targets();
    }
};

// [nvn] shot_frames = "300,1000:2000:50" (NXRT_NVN_SHOT_FRAMES): the presented frames (counted from 1 like the trace) whose picture is written as
// <shot_dir>/<number, 6 digits>.png (default shot_dir "shots"). A range is from:to:step. Lets a run be compared frame by frame against a reference capture.
struct ShotSpec { int64_t from, to, step; };
std::vector<ShotSpec> g_shots;
bool g_shots_read = false;
std::string g_shot_dir;

void read_shots() {
    g_shots_read = true;
    g_shot_dir = nxrt::config_get_str("nvn", "shot_dir", "shots");
    const std::string spec = nxrt::config_get_str("nvn", "shot_frames", "");
    size_t i = 0;
    while (i < spec.size()) {
        size_t j = spec.find(',', i);
        if (j == std::string::npos) j = spec.size();
        const std::string item = spec.substr(i, j - i);
        long long a = 0, b = 0, c = 1;
        const int n = std::sscanf(item.c_str(), "%lld:%lld:%lld", &a, &b, &c);
        if (n == 1) g_shots.push_back({a, a, 1});
        else if (n >= 2 && c > 0) g_shots.push_back({a, b, c});
        i = j + 1;
    }
}

bool shot_wanted(int64_t n) {
    if (!g_shots_read) read_shots();
    for (const ShotSpec& s : g_shots)
        if (n >= s.from && n <= s.to && (n - s.from) % s.step == 0) return true;
    return false;
}
// Sleeps on a high-resolution timer, then spins the last 0.3 ms (the present pacing wait).
void wait_until_ns(uint64_t due) {
    thread_local HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    for (;;) {
        const uint64_t now = FrameLimiter::now_ns();
        if (now >= due) return;
        if (due - now > 400000 && timer) {
            LARGE_INTEGER t;
            t.QuadPart = -static_cast<LONGLONG>((due - now - 300000) / 100);
            if (SetWaitableTimerEx(timer, &t, 0, nullptr, nullptr, nullptr, 0)) { WaitForSingleObject(timer, 50); continue; }
        }
        _mm_pause();
    }
}

bool present_pace_on() {
    static const bool on = nxrt::config_get_bool("display", "present_pace", true);
    return on;
}

}  // namespace

void present_on_render_thread(Window* w, int index) {
    cmdq::begin_submission(false);   // [nvn] cmd_thread: the present path (overlay, upscalers, other loaders) calls the driver directly
    NXFT("present");
    nxrt::HitchTimer ht_present(nxrt::kHpRenderPresent);
    CaptureEnd capture_end;
    gfx::Renderer* rend = renderer();
    PresentPass* pp = present_pass();
    if (!w || index < 0 || static_cast<size_t>(index) >= w->textures.size()) return;
    Texture* t = w->textures[index].get();
    if (!t) return;
    counters().presents.fetch_add(1, std::memory_order_relaxed);
    if (archexp::emit_on()) archexp::render_frame_tick();   // E4 (measurement only)
    if (archexp::d3_ab_on()) archexp::d3_ab_tick();
    { NXFT("present.frame_tick"); diag_frame_tick(); }   // the freeze watchdog counts presented frames (and the test runner its frames)
    if (!rend || !pp) return;
    { NXFT("present.tex"); if (!tex_gpu(t)) return; }
    extras::pace_note_present();
    if (FrameLimiter* lim = limiter()) {
        // the pre-frame hook (present mode changes) always runs here; the wait runs here only when no frame boundary has paced this present (S18)
        { NXFT("present.hook"); lim->run_pre_frame_hook(); }
        if (!(extras::pace_in_main() && extras::pace_boundary_consume())) { NXFT("limiter"); nxrt::HitchTimer ht(nxrt::kHpRenderLimiter); lim->begin_frame_nohook(); }
        else if (present_pace_on()) {
            // [display] present_pace: the game's main thread is paced at the frame boundary, but how long its frame takes to replay varies,
            // so presents came 10-24 ms apart at a steady 60 (Link's house: stdev 4.2 ms): uneven motion with an even game step. A present
            // never follows the previous one sooner than [display] present_pace_min (0.95) of the cap's period: the short intervals, the
            // other half of every long one, go. Relative to the last present (no grid), so a late frame adds no lasting delay.
            static const double frac = std::clamp(nxrt::config_get_double("display", "present_pace_min", 0.95), 0.5, 1.0);
            static uint64_t last = 0;
            const double fps = lim->target_fps();
            if (fps > 0 && last && !frame_limiter_unpaced()) {
                const uint64_t due = last + static_cast<uint64_t>(frac * 1e9 / fps);
                if (FrameLimiter::now_ns() < due) {
                    NXFT("present.pace");
                    nxrt::HitchTimer ht(nxrt::kHpRenderLimiter);
                    wait_until_ns(due);
                }
            }
            last = FrameLimiter::now_ns();
        }
    }
    {
        const int64_t n = static_cast<int64_t>(counters().presents.load());
        if (shot_wanted(n)) {
            char name[64];
            std::snprintf(name, sizeof name, "/%06lld.png", static_cast<long long>(n));
            rend->request_screenshot(g_shot_dir + name);
        }
    }
    gfx::FrameInfo fi;
    { NXFT("present.acquire"); nxrt::HitchTimer ht(nxrt::kHpRenderAcquire); if (!rend->begin_frame(&fi)) return; }
    ft::frame();
    Frame f;
    f.cb = fi.cmd;
    f.serial = current_serial();
    VkImageView src = VK_NULL_HANDLE;
    {
        NXFT("present.transition");
        tex_transition(t, f, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        src = tex_present_view(t);
    }
    NXFT("present.record");
    const VkExtent3D e = tex_image_extent(t, 0);     // a scaled target is presented whole (its crop in image texels)
    int32_t crop[4] = {w->crop[0], w->crop[1], w->crop[2], w->crop[3]};
    if (const float sc = tex_scale(t); sc != 1.0f)
        for (int32_t& c : crop) c = static_cast<int32_t>(std::lround(static_cast<float>(c) * sc));
    pp->set_flip_y(window_origin_mode() == NVN_WINDOW_ORIGIN_MODE_LOWER_LEFT);   // the game runs with the lower-left origin (mode 0)
    // HDR scene / HDR swapchain / FSR 1 or NIS / picture controls go through the output pass (S14); everything else is the plain present pass of the SDR path
    const extras::HdrState hs = extras::hdr_state();
    const extras::PictureLive pic = extras::picture_live();
    extras::OutputPass* op = output_pass();
    if (op && (hs.scene_hdr || hs.output != extras::OutputKind::Sdr8 || extras::upscale_mode() == extras::UpscaleMode::Fsr1 ||
               extras::upscale_mode() == extras::UpscaleMode::Nis || pic.active)) {
        extras::OutputParams prm;
        prm.src = src;
        prm.src_extent = VkExtent2D{e.width, e.height};
        for (int i = 0; i < 4; ++i) prm.crop[i] = crop[i];
        prm.keep_aspect = keep_aspect();
        prm.flip_y = window_origin_mode() == NVN_WINDOW_ORIGIN_MODE_LOWER_LEFT;
        prm.src_linear = hs.scene_hdr;
        prm.out = hs.output;
        prm.paper_white_nits = hs.paper_white_nits;
        prm.peak_nits = hs.peak_nits;
        prm.fsr1 = extras::upscale_mode() == extras::UpscaleMode::Fsr1;
        prm.nis = extras::upscale_mode() == extras::UpscaleMode::Nis;
        prm.sharpness = extras::upscale_sharpness();
        prm.picture = pic.active;
        prm.pic[0] = pic.contrast;
        prm.pic[1] = pic.saturation;
        prm.pic[2] = pic.black;
        prm.pic[3] = pic.gamma;
        op->record(fi.cmd, fi.view, rend->format(), fi.extent, prm);
    } else {
        pp->record(fi.cmd, fi.view, fi.extent, src, VkExtent2D{e.width, e.height}, crop, keep_aspect());
    }
    { NXFT("present.end"); nxrt::HitchTimer ht(nxrt::kHpRenderPresentEnd); rend->end_frame(); }
}

}  // namespace nxrt::nvn::be::gpu
