// SPDX-License-Identifier: GPL-3.0-or-later
// Frame time for the game's step (S11, docs/S11.md): a retail game that runs one tick of a hard-coded 1/60 s for every call of its
// frame update speeds up when it renders faster. This module measures the real time of each frame and feeds it to the game instead of the
// constant: the entry hook on the frame update computes the step, mid hooks load it into the registers that held the 1/60 immediates, and a
// few per-frame counters that are not driven by that dt (render timings, texture streaming, a per-tick friction, the background-work budget,
// the movie clock) are corrected. The mechanism is the game's own business only through Sites: the registered game (nxrt/game.h) lists
// where those places are (the game: games/oot/oot_core.cpp, the same addresses as recomp/games/oot/main.sites).
//
// Configuration, section [time] (env NXRT_TIME_<KEY>):
//   mode = real | fixed | off     real (default): the step is the measured frame time, clamped and smoothed.
//                                 fixed: the step is 1/rate and the frame is held until that time has passed (the 60 Hz lock).
//                                 off: the game's own 1/60 per call (retail behaviour, nothing is touched).
//   rate = 60                     steps per second in fixed mode
//   off_rate = 0                  off mode only: hold the frame to this rate but keep the retail 1/60 step (a diagnostic: the game then runs rate/60 times faster)
//   max_rate = 480                real mode: the step is never shorter than 1/max_rate; with pace=1 a frame is held until then
//   min_rate = 30                 real mode: the step is never longer than 1/min_rate (a slower frame makes the game run slow)
//   smooth = 4                    real mode: the step is the mean of the last N frame times (1 = raw); a jump of more than 50 % restarts it
//   pace = 1                      hold the frame until the minimum step has passed (stops an uncapped limiter from running the game faster)
//   movie_real = 1                the movie list's clock is real elapsed time (audio plays in real time), not the step
//   fix_friction = 1              per-tick float constants (Load::ScaledFloat; for example an in-air walk friction, 3.5 per 1/60 s) scaled by the step
//   fix_render_timings = 1        the render-timing clock (Sites::tick_render_timings) advances by the step (texture scroll, UV animation, water)
//   fix_streaming = 1             texture streaming ages its entries by the step instead of 1/60
//   fix_budget = 1                the background-work budget after the tick (for example 14 ms) scales with the step
//   fix_stuck_tracker = 1         a per-tick sampler (Sites::per_tick_sampler; for example a player "am I stuck" test, 20 samples, one per tick) takes one sample
//                                 per 1/60 s of game time, so its window keeps its length at more than 60 ticks per second.
//   log_every = 0                 log one line of frame statistics every N frames (0 = off)
//   csv = <file>                  write one line per frame: frame, host time (s), measured elapsed, step used, sum of steps, movie frames delivered, render-timing clock
//   game_base = 0x7100000000      guest address of the main module
// Deterministic test mode ([test] deterministic=1): the step is [test] clock_step_ticks / 19.2 MHz (default 1/60 s) and nothing waits,
// so a scripted run at 60, 120, 144 or 240 steps per second is the same run with another clock_step_ticks (160000 = 120 Hz).
#pragma once
#include <cstdint>
#include <string>

#include "nxrt/abi.h"

namespace nxrt::timestep {

enum class Mode : uint8_t { Off = 0, Real = 1, Fixed = 2 };

struct Config {
    Mode mode = Mode::Real;
    double rate = 60.0;
    double off_rate = 0.0;           // Mode::Off only: pace the frame to this rate without touching the step (shows what the retail step does at N Hz)
    double max_rate = 480.0;
    double min_rate = 30.0;
    int smooth = 4;
    bool pace = true;
    bool movie_real = true;
    bool fix_friction = true;
    bool fix_render_timings = true;
    bool fix_streaming = true;
    bool fix_budget = true;
    bool fix_stuck_tracker = true;
    double movie_max = 0.5;          // longest delta handed to the movie player in one call (seconds)
    int log_every = 0;
    std::string csv;
    uint64_t game_base = 0x7100000000ull;
};

Config load_config();                       // reads [time]
const char* mode_name(Mode m);

// The retail step (what the code loads, 0x3C888889 as a float).
constexpr float kGameStep = 0.016666668f;   // 1/60
float scale60(float dt);                    // dt / (1/60): exactly 1.0f when dt is the game's own step

// ---- where the step goes: the game's sites (offsets in main, nxrt/game.h Info::timestep) ------------------------------------------------
// What one mid hook loads into its register just before the instruction that reads the immediate.
enum class Load : uint8_t {
    Step = 1,       // w<reg> = step (float bits)
    NegStep,        // w<reg> = -step
    Budget,         // w<reg> = value * step * 60 (float bits)                        [time] fix_budget
    ScaledFloat,    // s<reg> = value * step * 60, the rest of v<reg> zeroed           [time] fix_friction
    Stream1,        // w<reg> = step                                                  [time] fix_streaming
    Stream2,        // w<reg> = 2 * step                                              [time] fix_streaming
};
struct MidSite {
    uint64_t offset;
    Load load;
    uint8_t reg;
    float value = 0.f;              // Budget, ScaledFloat: the retail per-1/60 s value
};
struct Sites {
    uint64_t frame_update = 0;          // entry: once per rendered frame, before the tick (measures the frame); 0 = no step hooks at all
    const MidSite* mids = nullptr;      // mid hooks, registered in this order
    uint32_t mid_count = 0;
    uint64_t tick_render_timings = 0;   // entry, handled: the per-frame render-timing clock: u32 frame counter / 60 (0 = none)
    uint64_t render_timing_counter = 0; // data: that u32 counter; the float seconds follow at +4
    double render_counter_wrap = 0;     // the counter is kept modulo this (0 = no wrap)
    uint64_t movie_update = 0;          // entry: s0 = the movie clock's delta seconds (0 = none)
    uint64_t per_tick_sampler = 0;      // entry, handled: a function that must run once per 1/60 s of game time; x0 stays as the result (0 = none)
    // A game that takes its step from data instead of immediates (the game: a frame-step scale it derives from a target fps): called on
    // the frame_update entry after the step of this frame is known, in real and fixed mode only. c: the guest context at that entry (x0 = this).
    void (*apply_step)(::Ctx* c, uint64_t game_base, float dt) = nullptr;
    // fps_limit = auto: true caps at [display] auto_fps (default 100; 0 = the display's refresh rate; the step follows real time). false keeps
    // the console's 60 ([display] console_fps): for a game that still advances something one unit per frame (the game's movie pictures).
    bool cap_at_display_rate = true;
    // The game's movie player shows one decoded picture per frame (the game): while the game reads pictures (nn::codec, the last one under 0.5 s ago)
    // the step is the console's 1/60, so the event timeline that starts the voice lines stays with the pictures (A11).
    bool fixed_step_during_movies = false;
};

// ---- the frame-time policy (pure, no clock) -----------------------------------------------------------------------------------
class Stepper {
public:
    explicit Stepper(const Config& c = {});
    // elapsed: seconds since the previous frame started (negative = no previous frame). Returns the step for this frame.
    double step(double elapsed);
    double dt() const { return dt_; }
    uint64_t frames() const { return frames_; }
    uint64_t restarts() const { return restarts_; }   // smoothing windows dropped because of a jump
    uint64_t clamped() const { return clamped_; }     // frames whose time was outside [1/max_rate, 1/min_rate]
    double min_period() const;                        // shortest frame the mode allows (0 in off mode)
private:
    Config c_;
    double dt_ = 1.0 / 60.0;
    double ring_[64] = {};
    int n_ = 0, head_ = 0;
    uint64_t frames_ = 0, restarts_ = 0, clamped_ = 0;
};

// ---- the game hooks --------------------------------------------------------------------------------------------------------------
// Registers the hooks on the lifted game (a site the module was not lifted with is simply never called). Idempotent.
void install(const Config& c);
void remove();
bool installed();
// Called from nxrt::init: reads [time] and installs.
void init();
void shutdown();

// The step of the frame that is running (what the mid hooks load), seconds. 1/60 until the first frame and in off mode.
float current_dt();
// What the game was last given: false in off mode.
bool active();
// The installed mode feeds the game a step (real or fixed) and the game lists sites: known before the first frame.
bool active_mode();
// A game with Sites::fixed_step_during_movies is showing a movie (it took a decoded picture in the last 0.5 s): its frames are one picture
// each, so the frame limiter must not run above [display] console_fps meanwhile (play.cpp caps it). Any thread; lock-free.
bool movie_showing();

struct Stats {
    uint64_t frames = 0;
    double last_dt = 0, last_elapsed = 0;
    double sim_seconds = 0;          // sum of the steps given to the game
    double min_elapsed = 0, max_elapsed = 0, mean_elapsed = 0;    // since the last reset_stats()
    uint64_t waits = 0;              // frames the pacing held
    uint64_t movie_calls = 0;
    double movie_total = 0;          // seconds handed to the movie list since the start
    uint64_t render_timing_calls = 0;
    double render_seconds = 0;       // the render-timing clock (seconds, wraps at 53000 / 60)
};
Stats stats();
void reset_stats();

// Tests: replace the clock (seconds, monotonic) and the sleep; nullptr restores the host clock. A test clock also disables the pacing sleep.
using ClockFn = double (*)();
void set_clock_for_tests(ClockFn fn);
// Tests: treat the run as deterministic with this step (seconds); 0 = follow [test] deterministic.
void set_deterministic_for_tests(double step_seconds);
// The script step `@rate <hz>` (test tooling): from the next frame on the game step is exactly 1/hz, nothing waits (hz > 0), or the configured mode again (0).
void set_run_rate(double hz);

}  // namespace nxrt::timestep
