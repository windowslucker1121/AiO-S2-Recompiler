// SPDX-License-Identifier: GPL-3.0-or-later
// Frame-time policy and the guest hooks that feed it to the game. See include/nxrt/timestep.h and docs/S11.md.
//
// Threads: the Update hook, the mid hooks of Update, the friction hook, the render-timing hook and the movie hook all run on the guest main
// thread; the two streaming mid hooks run on the render thread and only read the published step (an atomic). Statistics are guarded by a lock.
#include "nxrt/kernel.h"
#include "nxrt/timestep.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include "nxrt/config.h"
#include "nxrt/game.h"
#include "nxrt/hooks.h"
#include "nxrt/log.h"
#include "nxrt/movie.h"
#include "nxrt/video_codec.h"
#include "nxrt/tt.h"

namespace nxrt::timestep {

// ---------------------------------------------------------------------------------------------------------------------------------------
// Config and the pure policy
// ---------------------------------------------------------------------------------------------------------------------------------------

const char* mode_name(Mode m) {
    switch (m) {
        case Mode::Off: return "off";
        case Mode::Real: return "real";
        case Mode::Fixed: return "fixed";
    }
    return "?";
}

Config load_config() {
    Config c;
    std::string m = config_get_str("time", "mode", "real");
    for (char& ch : m) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (m == "off" || m == "0" || m == "retail") c.mode = Mode::Off;
    else if (m == "fixed" || m == "lock") c.mode = Mode::Fixed;
    else c.mode = Mode::Real;
    c.rate = std::clamp(config_get_double("time", "rate", 60.0), 10.0, 1000.0);
    c.off_rate = std::clamp(config_get_double("time", "off_rate", 0.0), 0.0, 2000.0);
    c.max_rate = std::clamp(config_get_double("time", "max_rate", 480.0), 30.0, 2000.0);
    c.min_rate = std::clamp(config_get_double("time", "min_rate", 30.0), 5.0, c.max_rate);
    c.smooth = static_cast<int>(std::clamp<int64_t>(config_get_int("time", "smooth", 4), 1, 64));
    c.pace = config_get_bool("time", "pace", true);
    c.movie_real = config_get_bool("time", "movie_real", true);
    c.fix_friction = config_get_bool("time", "fix_friction", true);
    c.fix_render_timings = config_get_bool("time", "fix_render_timings", true);
    c.fix_streaming = config_get_bool("time", "fix_streaming", true);
    c.fix_budget = config_get_bool("time", "fix_budget", true);
    c.fix_stuck_tracker = config_get_bool("time", "fix_stuck_tracker", true);
    c.movie_max = std::clamp(config_get_double("time", "movie_max", 0.5), 0.01, 5.0);
    c.movie_follow_pictures = config_get_bool("time", "movie_follow_pictures", true);
    c.movie_hold = std::clamp(config_get_double("time", "movie_hold", 1.0), 0.0, 10.0);
    c.av_trace = config_get_bool("time", "av_trace", false);
    c.log_every = static_cast<int>(std::clamp<int64_t>(config_get_int("time", "log_every", 0), 0, 1000000));
    c.csv = config_get_str("time", "csv", "");
    c.game_base = static_cast<uint64_t>(config_get_int("time", "game_base", static_cast<int64_t>(game::info().main_base)));
    return c;
}

float scale60(float dt) { return dt / kGameStep; }

Stepper::Stepper(const Config& c) : c_(c) {
    c_.smooth = std::clamp(c_.smooth, 1, 64);
    dt_ = c_.mode == Mode::Fixed ? 1.0 / c_.rate : static_cast<double>(kGameStep);
}

double Stepper::min_period() const {
    switch (c_.mode) {
        case Mode::Off: return 0.0;
        case Mode::Fixed: return 1.0 / c_.rate;
        case Mode::Real: return 1.0 / c_.max_rate;
    }
    return 0.0;
}

double Stepper::step(double elapsed) {
    frames_++;
    if (c_.mode == Mode::Off) return dt_ = static_cast<double>(kGameStep);
    if (c_.mode == Mode::Fixed) return dt_ = 1.0 / c_.rate;
    if (elapsed < 0.0) return dt_;                         // first frame: the game's own step until a frame time exists
    const double lo = 1.0 / c_.max_rate, hi = 1.0 / c_.min_rate;
    const double e = std::clamp(elapsed, lo, hi);
    if (e != elapsed) clamped_++;
    if (c_.smooth <= 1) return dt_ = e;
    if (n_ > 0) {
        double sum = 0;
        for (int i = 0; i < n_; i++) sum += ring_[i];
        const double mean = sum / n_;
        if (std::fabs(e - mean) > 0.5 * mean) {            // a hitch or a rate change: the old window no longer describes the frame time
            n_ = 0;
            head_ = 0;
            restarts_++;
        }
    }
    if (n_ < c_.smooth) {
        ring_[n_++] = e;
        head_ = n_ % c_.smooth;
    } else {
        ring_[head_] = e;
        head_ = (head_ + 1) % c_.smooth;
    }
    double sum = 0;
    for (int i = 0; i < n_; i++) sum += ring_[i];
    return dt_ = sum / n_;
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// The state the hooks share
// ---------------------------------------------------------------------------------------------------------------------------------------
namespace {

constexpr const char* kTag = "timestep";

double qpc_seconds() {
    static const double inv = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return 1.0 / static_cast<double>(f.QuadPart);
    }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<double>(t.QuadPart) * inv;
}

struct G {
    std::mutex mu;                      // statistics and the csv
    Config cfg;
    Stepper stepper;
    bool installed = false;
    std::vector<uint32_t> hooks;

    ClockFn test_clock = nullptr;
    double det_step_test = 0;           // set_deterministic_for_tests
    bool det = false;                   // this run is deterministic
    double det_step = 1.0 / 60.0;
    double det_now = 0;                 // virtual time: frames * det_step

    std::atomic<uint32_t> dt_bits{0};   // the step as the float the game loads
    std::atomic<bool> active{false};
    float dt = kGameStep;

    // frame timing (main thread)
    bool have_last = false;
    double last_start = 0, next_target = 0;
    uint64_t frame = 0;

    // movie
    bool have_movie = false;
    double movie_last = 0;

    // render timings
    bool render_init = false;
    double render_frames = 0;
    double stuck_acc = 0.0;        // game time since the last stuck-tracker sample
    uint64_t render_addr = 0;
    void (*apply_step)(::Ctx*, uint64_t, float) = nullptr;   // Sites::apply_step
    bool movie_fixed = false;                                  // Sites::fixed_step_during_movies
    uint64_t movie_pics = 0;                                   // movie_pictures() at the last change
    double movie_pics_at = -1.0;                               // when it changed (clock_now seconds)
    bool movie_anchored = false;                               // movie_follow_pictures: the step follows the pictures since movie_anchor
    uint64_t movie_anchor = 0;                                 // movie_pictures() when the movie was first seen
    double movie_given = 0;                                    // game time handed out since then (seconds)
    uint64_t movie_short = 0;                                  // frames that got the short step because no picture came (statistics)
    double render_wrap = 0;            // the game's wrap of the render-timing counter (Sites::render_counter_wrap)

    // statistics
    Stats st;
    double sum_elapsed = 0;
    uint64_t n_elapsed = 0;
    FILE* csv = nullptr;
};

G& g() {
    static G* s = new G();               // never destroyed: hooks may still run during process teardown
    return *s;
}

uint32_t f2u(float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}

double clock_now() {
    G& s = g();
    return s.test_clock ? s.test_clock() : qpc_seconds();
}

// Waits until the clock reaches target (seconds): a high-resolution timer for the bulk, a spin for the last part.
void wait_until(double target) {
    static HANDLE timer = [] {
        HANDLE h = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        return h;
    }();
    for (;;) {
        const double r = target - clock_now();
        if (r <= 0) return;
        if (r > 0.0012) {
            const double sleep_s = r - 0.0008;
            if (timer) {
                LARGE_INTEGER due;
                due.QuadPart = -static_cast<LONGLONG>(sleep_s * 1e7);
                if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
                    WaitForSingleObject(timer, 100);
                    continue;
                }
            }
            Sleep(static_cast<DWORD>(std::max(1.0, sleep_s * 1000.0)));
        } else {
            YieldProcessor();
        }
    }
}

void publish_dt(float dt) {
    G& s = g();
    s.dt = dt;
    s.dt_bits.store(f2u(dt), std::memory_order_release);
}

float read_dt() { return g().dt; }
std::atomic<double> g_movie_pics_at{-1.0};   // movie_pics_at for movie_showing() on other threads

// Pictures of movies the game has taken so far: the movie list's (platform 1 HLE) plus nn::codec's decoded pictures read (platform 2).
uint64_t movie_pictures() { return media::movie_stats().video_frames_delivered + media::video_codec_stats().frames_read; }

std::atomic<bool> g_av_trace{false};
std::atomic<uint64_t> g_av_frame{0};       // the frame number, for av_note_sound on other threads
std::atomic<uint64_t> g_av_anchor{0};      // movie_pictures() at the start of the movie showing now (0 = none)
std::atomic<double> g_av_sim{0.0};         // game time handed out so far (seconds)

// Sites::fixed_step_during_movies. The game's movie player takes one decoded picture per frame, and the event timeline that starts the
// movie's voice lines and sounds runs on the frame step. A fixed 1/60 s step per frame kept them together only while every frame got a new
// picture: the decoder fell behind at the start of a movie (~100 pictures in its first seconds) and now and then later (~1 frame in 170),
// so the timeline ran ahead of the pictures by up to ~2.7 s at the end of the 154 s intro (2026-11-12). Now the game time follows the
// pictures: after each frame it is one 1/60 s step per picture taken since the movie began. A frame that got no picture gets the shortest
// step ([time] max_rate), and the frames after it make up the difference once pictures come again. pics: movie_pictures() now (taken
// by the frames before this one).
double movie_step(G& s, double now, double dt, uint64_t pics) {
    const Config& c = s.cfg;
    const bool fresh = pics != s.movie_pics;
    if (fresh) {
        s.movie_pics = pics;
        s.movie_pics_at = now;
        g_movie_pics_at.store(now, std::memory_order_relaxed);
    }
    const double ago = s.movie_pics_at >= 0.0 ? now - s.movie_pics_at : 1e9;
    if (!c.movie_follow_pictures) return ago < 0.5 ? static_cast<double>(kGameStep) : dt;   // a movie is showing: one 1/60 s step per frame
    if (s.movie_anchored) {
        // The movie goes on while pictures come, or while a decoder is still open and the last picture is under movie_hold old (a late
        // decoder: the timeline waits for it). A finalized decoder ends it at once.
        const media::VideoCodecStats v = media::video_codec_stats();
        const bool live = v.decoders_created > v.decoders_destroyed;
        const bool keep = live ? ago < std::max(0.5, c.movie_hold) : (v.decoders_created == 0 && ago < 0.5);
        if (!keep) {
            NXLOG_I(kTag, "movie: the step runs freely again after %llu pictures (%s); %llu frames waited for a late picture",
                    static_cast<unsigned long long>(pics - s.movie_anchor), live ? "no picture for a while" : "decoder closed",
                    static_cast<unsigned long long>(s.movie_short));
            s.movie_anchored = false;
            g_av_anchor.store(0, std::memory_order_relaxed);
        }
    }
    if (!s.movie_anchored) {
        if (!fresh) return dt;
        s.movie_anchored = true;
        s.movie_anchor = pics;
        s.movie_given = 0;
        s.movie_short = 0;
        g_av_anchor.store(pics, std::memory_order_relaxed);
        NXLOG_I(kTag, "movie: the step follows the pictures from picture %llu on (frame %llu)", static_cast<unsigned long long>(pics),
                static_cast<unsigned long long>(s.frame));
    }
    // After this frame the game will have taken one more picture: hand out the game time that needs.
    const double want = static_cast<double>(pics + 1 - s.movie_anchor) * static_cast<double>(kGameStep) - s.movie_given;
    const double lo = 1.0 / c.max_rate, hi = 4.0 * static_cast<double>(kGameStep);
    if (want < 0.5 * static_cast<double>(kGameStep)) s.movie_short++;
    const double step = std::clamp(want, lo, hi);
    s.movie_given += step;
    return step;
}

// ---- the frame boundary: entry hook on CGameArchitectureSupport::Update -------------------------------------------------------------
void begin_frame() {
    G& s = g();
    const Config& c = s.cfg;
    const bool retail = c.mode == Mode::Off;
    if (retail && c.off_rate <= 0.0) {
        s.active.store(false);
        return;
    }
    s.frame++;
    double elapsed = -1.0, now = 0.0;
    if (s.det) {
        s.det_now = static_cast<double>(s.frame) * s.det_step;
        if (!retail) publish_dt(static_cast<float>(s.det_step));
        elapsed = s.det_step;
        now = s.det_now;
        s.active.store(!retail);
    } else {
        now = clock_now();
        const bool grid = c.mode == Mode::Fixed || retail;       // a steady schedule instead of "one period after the last frame"
        const double period = retail ? 1.0 / c.off_rate : s.stepper.min_period();
        if (c.pace && s.have_last && period > 0.0) {
            const double target = grid ? s.next_target : s.last_start + period;
            if (now < target) {
                if (!s.test_clock) wait_until(target);
                now = s.test_clock ? target : clock_now();
                std::lock_guard<std::mutex> lk(s.mu);
                s.st.waits++;
            }
        }
        elapsed = s.have_last ? now - s.last_start : -1.0;
        if (!retail) {
            double dt = s.stepper.step(elapsed);
            if (s.movie_fixed) {
                const uint64_t pics = movie_pictures();
                dt = movie_step(s, now, dt, pics);
            }
            publish_dt(static_cast<float>(dt));
        }
        if (grid) {
            const double p = retail ? 1.0 / c.off_rate : 1.0 / c.rate;
            s.next_target = (s.have_last ? s.next_target : now) + p;
            if (now > s.next_target + p) s.next_target = now + p;      // fell behind by more than a frame: do not queue up catch-up frames
        }
        s.last_start = now;
        s.have_last = true;
        s.active.store(!retail);
    }
    std::lock_guard<std::mutex> lk(s.mu);
    s.st.frames = s.frame;
    s.st.last_dt = s.dt;
    if (!retail) s.st.sim_seconds += s.dt;
    g_av_frame.store(s.frame, std::memory_order_relaxed);
    g_av_sim.store(s.st.sim_seconds, std::memory_order_relaxed);
    if (elapsed >= 0) {
        s.st.last_elapsed = elapsed;
        if (s.n_elapsed == 0 || elapsed < s.st.min_elapsed) s.st.min_elapsed = elapsed;
        if (elapsed > s.st.max_elapsed) s.st.max_elapsed = elapsed;
        s.sum_elapsed += elapsed;
        s.n_elapsed++;
        s.st.mean_elapsed = s.sum_elapsed / static_cast<double>(s.n_elapsed);
    }
    if (s.csv)
        std::fprintf(s.csv, "%llu,%.6f,%.6f,%.6f,%.6f,%llu,%.4f\n", static_cast<unsigned long long>(s.frame), now, elapsed, static_cast<double>(s.dt), s.st.sim_seconds,
                     static_cast<unsigned long long>(movie_pictures()), s.st.render_seconds);
    if (c.log_every > 0 && s.frame % static_cast<uint64_t>(c.log_every) == 0)
        NXLOG_I(kTag, "frame %llu: step %.3f ms (%.1f Hz), elapsed min/mean/max %.3f/%.3f/%.3f ms, waits %llu, sim %.3f s, movie frames %llu",
                static_cast<unsigned long long>(s.frame), s.dt * 1e3, 1.0 / std::max(1e-9, static_cast<double>(s.dt)), s.st.min_elapsed * 1e3,
                s.st.mean_elapsed * 1e3, s.st.max_elapsed * 1e3, static_cast<unsigned long long>(s.st.waits), s.st.sim_seconds,
                static_cast<unsigned long long>(movie_pictures()));
}

int hook_update(::Ctx* c, uint64_t, void*) {
    begin_frame();
    G& s = g();
    if (s.apply_step && s.active.load(std::memory_order_acquire)) s.apply_step(c, s.cfg.game_base, read_dt());
    return 0;
}

// ---- mid hooks: the registers that held an immediate (user = the game's MidSite) -------------------------------------------------------
int hook_mid(::Ctx* c, uint64_t, void* user) {
    G& s = g();
    if (!s.active.load(std::memory_order_acquire)) return 0;
    const MidSite& site = *static_cast<const MidSite*>(user);
    const int reg = site.reg;
    float dt;
    {
        const uint32_t b = s.dt_bits.load(std::memory_order_acquire);
        std::memcpy(&dt, &b, 4);
    }
    switch (site.load) {
        case Load::Step:
            c->x[reg] = f2u(dt);
            break;
        case Load::NegStep:
            c->x[reg] = f2u(-dt);
            break;
        case Load::Budget:
            if (s.cfg.fix_budget) c->x[reg] = f2u(site.value * scale60(dt));
            break;
        case Load::ScaledFloat:
            if (s.cfg.fix_friction) {
                c->v[reg].u64[1] = 0;
                c->v[reg].u64[0] = 0;
                c->v[reg].f32[0] = site.value * scale60(dt);
            }
            break;
        case Load::Stream1:
            if (s.cfg.fix_streaming) c->x[reg] = f2u(dt);
            break;
        case Load::Stream2:
            if (s.cfg.fix_streaming) c->x[reg] = f2u(2.0f * dt);
            break;
    }
    return 0;
}

// ---- NRenderUtilMPT::TickRenderTimings: retail counts one frame per call and stores counter / 60 seconds ---------------------------
int hook_render_timings(::Ctx* c, uint64_t, void*) {
    G& s = g();
    if (!s.active.load(std::memory_order_acquire) || !s.cfg.fix_render_timings || !s.render_addr) return 0;
    const float dt = read_dt();
    uint32_t* counter = reinterpret_cast<uint32_t*>(s.render_addr);
    float* secs = reinterpret_cast<float*>(s.render_addr + 4);
    if (!s.render_init) {
        s.render_frames = *counter;
        s.render_init = true;
    }
    s.render_frames += static_cast<double>(scale60(dt));
    if (s.render_wrap > 0)
        while (s.render_frames >= s.render_wrap) s.render_frames -= s.render_wrap;
    const uint32_t whole = static_cast<uint32_t>(s.render_frames);
    const float seconds = static_cast<float>(s.render_frames) / 60.0f;
    *counter = whole;
    *secs = seconds;
    c->v[0].u64[0] = 0;
    c->v[0].u64[1] = 0;
    c->v[0].f32[0] = seconds;
    {
        std::lock_guard<std::mutex> lk(s.mu);
        s.st.render_timing_calls++;
        s.st.render_seconds = seconds;
    }
    return 1;
}

// ---- CPlayerStuckTrackerMP1::AddState: one sample per 1/60 s of game time ----------------------------------------------------------
// The tracker keeps the last 20 samples (positions, velocities, states, one added per player tick) and IsPlayerStuck judges the path length and the
// bounding box of those 20. Written for 60 ticks per second the window is 1/3 s; at 240 ticks per second it is 1/12 s, the top of a jump (speed
// about 1.4 units/s) covers less than the 1/30 unit threshold, the test says "stuck" and ProcessInput calls CPhysicsActorMP1::Stop on the player:
// vertical speed 0 for one frame, a jump 4 % longer. Taking a sample only when 1/60 s of game time has passed keeps the window as it was made.
int hook_stuck_add_state(::Ctx*, uint64_t, void*) {
    G& s = g();
    if (!s.active.load(std::memory_order_acquire) || !s.cfg.fix_stuck_tracker) return 0;
    const double dt = read_dt();
    if (dt >= kGameStep * 0.999) {
        s.stuck_acc = 0.0;
        return 0;
    }
    s.stuck_acc += dt;
    if (s.stuck_acc < static_cast<double>(kGameStep) - 1e-5) return 1;        // too early for a sample: handled, x0 (this) is the result
    s.stuck_acc -= kGameStep;
    if (s.stuck_acc > kGameStep) s.stuck_acc = 0.0;
    return 0;
}

// ---- CMovieList::Update(float): the movie clock is real time ---------------------------------------------------------------------------
int hook_movie(::Ctx* c, uint64_t, void*) {
    G& s = g();
    if (!s.active.load(std::memory_order_acquire) || !s.cfg.movie_real) return 0;
    const double now = s.det ? s.det_now : clock_now();
    double d = c->v[0].f32[0];
    if (s.have_movie) d = std::clamp(now - s.movie_last, 0.0, s.cfg.movie_max);
    s.movie_last = now;
    s.have_movie = true;
    c->v[0].f32[0] = static_cast<float>(d);
    std::lock_guard<std::mutex> lk(s.mu);
    s.st.movie_calls++;
    s.st.movie_total += d;
    return 0;
}

void add_hooks(const Config& c) {
    G& s = g();
    const uint64_t b = c.game_base;
    const Sites* sites = game::info().timestep;
    if (!sites || !sites->frame_update) {
        NXLOG_I(kTag, "the game lists no frame-step sites: the retail step stays");
        return;
    }
    auto E = [&](uint64_t off, SiteHookFn fn) { if (off) s.hooks.push_back(hook_add_entry(b + off, fn, nullptr)); };
    E(sites->frame_update, &hook_update);
    for (uint32_t i = 0; i < sites->mid_count; ++i)
        s.hooks.push_back(hook_add_mid(b + sites->mids[i].offset, &hook_mid, const_cast<MidSite*>(&sites->mids[i])));
    E(sites->tick_render_timings, &hook_render_timings);
    E(sites->movie_update, &hook_movie);
    E(sites->per_tick_sampler, &hook_stuck_add_state);
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------------------------------------------------------------------

void install(const Config& cfg) {
    G& s = g();
    if (s.installed) return;
    s.cfg = cfg;
    s.stepper = Stepper(cfg);
    s.frame = 0;
    s.have_last = s.have_movie = s.render_init = false;
    s.render_frames = 0;
    const Sites* sites = game::info().timestep;
    s.render_addr = sites && sites->render_timing_counter ? cfg.game_base + sites->render_timing_counter : 0;
    s.render_wrap = sites ? sites->render_counter_wrap : 0.0;
    s.apply_step = sites ? sites->apply_step : nullptr;
    s.movie_fixed = sites && sites->fixed_step_during_movies;
    s.movie_pics = 0;
    s.movie_pics_at = -1.0;
    s.movie_anchored = false;
    g_av_trace.store(cfg.av_trace, std::memory_order_relaxed);
    s.st = Stats{};
    s.sum_elapsed = 0;
    s.n_elapsed = 0;
    s.det = tt::deterministic() || s.det_step_test > 0;
    if (s.det) {
        const double hz = static_cast<double>(kernel::tick_hz());
        const double ticks = s.det_step_test > 0 ? s.det_step_test * hz
                                                 : static_cast<double>(config_get_int("test", "clock_step_ticks", static_cast<int64_t>(kernel::tick_hz() / 60)));
        s.det_step = ticks / hz;
        s.det_now = 0;
    }
    publish_dt(kGameStep);
    s.active.store(false);
    if (!cfg.csv.empty()) {
        s.csv = std::fopen(cfg.csv.c_str(), "w");
        if (s.csv) std::fprintf(s.csv, "frame,host_s,elapsed_s,step_s,sim_s,movie_frames,render_s\n");
        else NXLOG_W(kTag, "cannot write %s", cfg.csv.c_str());
    }
    if (cfg.mode != Mode::Off || cfg.off_rate > 0.0) add_hooks(cfg);
    s.installed = true;
    NXLOG_I(kTag, "mode %s (%s), rate %.0f, clamp %.0f..%.0f Hz, smooth %d, pace %d, fixes: friction %d render_timings %d streaming %d budget %d stuck_tracker %d movie %d, movie step follows pictures %d (hold %.1f s), %zu hooks at base 0x%llx",
            mode_name(cfg.mode), s.det ? "deterministic" : "host clock", cfg.mode == Mode::Fixed ? cfg.rate : 0.0, cfg.min_rate, cfg.max_rate, cfg.smooth,
            cfg.pace, cfg.fix_friction, cfg.fix_render_timings, cfg.fix_streaming, cfg.fix_budget, cfg.fix_stuck_tracker, cfg.movie_real, cfg.movie_follow_pictures, cfg.movie_hold, s.hooks.size(),
            static_cast<unsigned long long>(cfg.game_base));
}

void remove() {
    G& s = g();
    if (!s.installed) return;
    for (uint32_t id : s.hooks) hook_remove(id);
    s.hooks.clear();
    if (s.csv) {
        std::fclose(s.csv);
        s.csv = nullptr;
    }
    s.active.store(false);
    s.installed = false;
}

bool installed() { return g().installed; }

bool active_mode() {
    G& s = g();
    const Sites* sites = game::info().timestep;
    return s.installed && s.cfg.mode != Mode::Off && sites && sites->frame_update;
}

void init() { install(load_config()); }

void shutdown() {
    G& s = g();
    if (!s.installed) return;
    if (s.cfg.mode != Mode::Off) {
        const Stats st = stats();
        NXLOG_I(kTag, "end of run: %llu frames, step last %.3f ms, elapsed min/mean/max %.3f/%.3f/%.3f ms, %llu paced waits, %llu clamped, %llu smoothing restarts, movie %.3f s in %llu calls",
                static_cast<unsigned long long>(st.frames), st.last_dt * 1e3, st.min_elapsed * 1e3, st.mean_elapsed * 1e3, st.max_elapsed * 1e3,
                static_cast<unsigned long long>(st.waits), static_cast<unsigned long long>(s.stepper.clamped()),
                static_cast<unsigned long long>(s.stepper.restarts()), st.movie_total, static_cast<unsigned long long>(st.movie_calls));
    }
    remove();
}

float current_dt() { return read_dt(); }
bool active() { return g().active.load(); }
bool movie_showing() {
    const double at = g_movie_pics_at.load(std::memory_order_relaxed);
    return at >= 0.0 && clock_now() - at < 0.5;
}

bool av_trace_on() { return g_av_trace.load(std::memory_order_relaxed); }

void av_note_sound(const char* kind, int64_t id, int64_t a, int64_t b) {
    if (!av_trace_on()) return;
    const uint64_t pics = movie_pictures(), anchor = g_av_anchor.load(std::memory_order_relaxed);
    // "movie picture" = pictures taken since the movie showing now began (-1 = none): at 60 pictures/s, picture / 60 is the movie time.
    NXLOG_I("av", "sound %s %lld (%lld, %lld): frame %llu, movie picture %lld, pictures %llu, game time %.4f s, host %.4f s", kind,
            static_cast<long long>(id), static_cast<long long>(a), static_cast<long long>(b),
            static_cast<unsigned long long>(g_av_frame.load(std::memory_order_relaxed)), anchor ? static_cast<long long>(pics - anchor) : -1LL,
            static_cast<unsigned long long>(pics), g_av_sim.load(std::memory_order_relaxed), qpc_seconds());
}

Stats stats() {
    G& s = g();
    std::lock_guard<std::mutex> lk(s.mu);
    return s.st;
}

void reset_stats() {
    G& s = g();
    std::lock_guard<std::mutex> lk(s.mu);
    s.st = Stats{};
    s.sum_elapsed = 0;
    s.n_elapsed = 0;
}

void set_clock_for_tests(ClockFn fn) { g().test_clock = fn; }
void set_deterministic_for_tests(double step_seconds) { g().det_step_test = step_seconds; }

void set_run_rate(double hz) {
    G& s = g();
    if (hz > 0) {
        s.det_step = 1.0 / hz;
        s.det = true;
    } else {
        s.det = tt::deterministic() || s.det_step_test > 0;     // back to what the run was configured with
        if (s.det_step_test > 0) s.det_step = s.det_step_test;
        s.have_last = false;                                     // no frame time across the switch
    }
}

}  // namespace nxrt::timestep
