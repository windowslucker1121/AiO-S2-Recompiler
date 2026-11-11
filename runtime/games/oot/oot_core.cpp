// SPDX-License-Identifier: GPL-3.0-or-later
// the game, the core part of the game module (oot_game.h): the data part plus the title facts. Needs the runtime library, not
// the graphics one. No test probes or poll sleeps yet.
#include "oot_game.h"

#include "nxrt/game.h"
#include "nxrt/log.h"
#include "nxrt/services.h"
#include "nxrt/timestep.h"

#include <algorithm>
#include <cmath>

namespace nxrt::games::oot {

namespace {

// main.npdm of the 2.0.0 update: priority 44, core 0, stack 0x100000. Save sizes are placeholders until control.nacp is known (the
// boot log shows what the game asks for).
void title_defaults(services::TitleProfile& p) {
    p.program_id = data_info().program_id;
    p.name = data_info().title;
    p.display_version = data_info().version;
    p.supported_languages = data_info().languages;
    p.account_save_size = 0x4000000;
    p.account_save_journal = 0x4000000;
    p.device_save_size = 0x4000000;
    p.device_save_journal = 0x4000000;
    p.temporary_storage_size = 0x4000000;      // placeholder too: the game mounts temporary storage on a loader thread
    p.main_thread_stack = 0x100000;
    p.main_thread_priority = 44;
    p.main_thread_core = 0;
    // META SystemResourceSize 0x3000000 (GetInfo 16): nn::os enables its virtual address memory manager only when this is non-zero, and the
    // game allocates address regions through it (left at 0, VammManager stayed uninitialised and AllocateAddressRegion read null).
    p.system_resource_size = 0x3000000;
    p.firmware = "22.0.0";                     // a lower bound: main is ZBIC-compressed, which firmware 22.0.0 introduced
    p.user_nickname = "Player";
    p.user_uid_hi = 0x0000000000000001ull;
    p.user_uid_lo = 0x4f4f545265636d70ull;     // "OOTRecmp"
    // The movies' NDVE enhancement network (main+0x20e4d20 parses this file): run as translated compute shaders it costs ~100 ms per frame on an
    // integrated GPU (title at 8 fps). Without the file the player shows the decoded AV1 frames directly (the title movie at full speed).
    p.romfs_hide = {"/Movie/Ndve/Ndve-Nvn2-vid_seq5.dat.zs"};
}

// ---- the frame step ----------------------------------------------------------------------------------------------------------------
// The framework's per-frame virtual (main+0x3b4ea90, oot_game.cpp kFrameUpdate) runs the calc, then turns a target frame rate into the step
// everything else multiplies its per-frame motion by: scale = 60.0f / (float)fps, with fps the u32 at this+0x2c8, stored as a float global
// (main+0x4d41fc8; 1.0 at 60 fps, 2.0 at 30). The calc runs before that store, so it reads the scale the previous frame left. Retail keeps fps
// at 60, so the game advances 1/60 s per presented frame. Real-time step ([time] mode = real, the `oot` default): at the entry of that function
// (before the calc) the scale is set to measured step * 60, so the calc moves by real time, and the fps field to the nearest whole rate, so the
// function's own store after the calc lands close to it. Physics, animation, the camera and the event timeline all follow it (walking and the
// camera checked at 10 and 27 fps); the movie player does not (one picture per frame), hence fixed_step_during_movies.
constexpr uint64_t kFrameUpdate = 0x3b4ea90;
constexpr uint64_t kStepScale = 0x4d41fc8;      // float, data of main
constexpr uint64_t kFpsField = 0x2c8;           // u32 in the framework object (x0 at kFrameUpdate)

// Main thread only (the frame update's entry).
void apply_step(::Ctx* c, uint64_t base, float dt) {
    const uint64_t self = c->x[0];
    if (!self) return;
    float* scale = reinterpret_cast<float*>(base + kStepScale);
    uint32_t* fps = reinterpret_cast<uint32_t*>(self + kFpsField);
    const float s = timestep::scale60(dt);
    static bool logged = false;
    if (!logged) {
        logged = true;
        // The game puts its target rate back into the field after each frame (60, the same as the u32 at this+0xfc), so writing it per frame is
        // needed and leaves nothing behind once the step is off.
        NXLOG_I("oot", "frame step: framework object 0x%llx, fps field %u, scale %.3f; the step follows real time from now on",
                static_cast<unsigned long long>(self), *fps, static_cast<double>(*scale));
    }
    *fps = static_cast<uint32_t>(std::lround(std::clamp(60.0f / s, 1.0f, 1000.0f)));
    *scale = s;
}

const timestep::Sites& step_sites() {
    static const timestep::Sites t = [] {
        timestep::Sites s;
        s.frame_update = kFrameUpdate;
        s.apply_step = &apply_step;
        // fps_limit = auto follows the display's refresh rate. The movie player shows one picture per frame, so while a movie shows the
        // limiter holds the console's 60 anyway (play.cpp movie_cap, timestep::movie_showing). Measured 2026-11-09 at 1920x1080:
        // the house scene 64 fps uncapped with a 1% low of 48-53 (vs 60 / 41-44 capped).
        s.cap_at_display_rate = true;
        s.fixed_step_during_movies = true;
        return s;
    }();
    return t;
}

}  // namespace

const game::Info& core_info() {
    static const game::Info info = [] {
        game::Info g = data_info();
        g.title_defaults = &title_defaults;
        g.timestep = &step_sites();
        return g;
    }();
    return info;
}

}  // namespace nxrt::games::oot

void nxrt_register_game_oot_core() { nxrt::game::set_current(&nxrt::games::oot::core_info()); }
