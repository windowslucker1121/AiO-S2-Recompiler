// SPDX-License-Identifier: GPL-3.0-or-later
// Play mode self-test: a tiny scripted driver for the window, used by the play tests. It is switched on by
// [play] selftest = "<step>,<step>,..." (environment NXRT_PLAY_SELFTEST) and runs one step per game frame boundary, on the main thread, through
// the same paths a person would use: SDL window calls and real SDL events pushed into SDL's queue (so they come back out of the pump).
//
//   wait:N                 wait N frames
//   size:WxH               SDL_SetWindowSize; the next steps see the resize event
//   mode:windowed|borderless|exclusive    platform::set_fullscreen
//   altenter               Alt+Enter as four SDL key events (the fullscreen toggle of play mode)
//   key:<name>:down|up     one SDL key event by scancode name, e.g. key:Return:down
//   close                  SDL_EVENT_QUIT (the close button): play mode ends the process with exit code 0
//   cap:N                  frame cap N fps (0 = unlimited) on the limiter, then the measurement window restarts
//   measure:S              wait S seconds of wall time, then log "selftest: fps=<presents per second>" for the cap in force
//   expect_mode:X          fail unless the window is in mode X
//   expect_size:WxH        fail unless the window client size is exactly W x H pixels
//   expect_fps:N:TOL       fail unless the last measure was within TOL (fraction, e.g. 0.03) of N fps
//   expect_late:MS         fail unless the frames that waited in the last measure started within MS (p99) of their scheduled time
//   set:<key>=<value>      change a setting the way the settings page does (queued, applied at the next frame, written to the settings file):
//                          vsync (0/1), fps_cap, window_mode, yaw_source, pitch_source, invert_y, invert_x, prompt_style (switch xbox playstation
//                          keyboard auto: the Button prompts combo of the Controller tab)
//   expect_present:X       fail unless the swapchain presents with mode X (fifo, mailbox, immediate)
//   expect_cap:N           fail unless the frame limiter's target is N fps
//   expect_ini:TEXT        fail unless the settings file contains TEXT
//   gate:NAME              hold the script until a file selftest_NAME exists in the working folder (it is deleted, then the script goes on)
//   report[:label]         log one line: window mode, pixel size, focus, minimised, refresh rate, frames
//   pass                   exit code 0 now ("selftest PASS" in the log); without it the process keeps running after the last step
// A failed expectation logs "selftest FAIL: <why>" and ends the process with exit code 1. Every step is logged ("selftest: step n: ...").
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "nxrt/frame_limiter.h"
#include "nxrt/gfx.h"

namespace nxrt::play {

// Ends the process: writes the scripted-run result (tt::end_run) when the test tooling is on, otherwise just terminates. Implemented in play.cpp.
[[noreturn]] void exit_process(int code, const char* verdict, const std::string& message);

// What the self-test needs from play mode besides the window: the settings page's entry points.
struct SelfTestHost {
    virtual ~SelfTestHost() = default;
    virtual bool set(const std::string& key, const std::string& value) = 0;      // false for an unknown key
    virtual std::string present_mode() = 0;
    virtual std::string settings_text() = 0;                                      // the settings file as written
};

class SelfTest {
public:
    bool parse(const std::string& script, std::string* err);
    bool active() const { return !steps_.empty() && next_ < steps_.size(); }
    // One call per frame boundary. Returns false when the script has ended the process (it does not return in that case).
    void tick(gfx::Renderer* renderer, FrameLimiter* limiter, SelfTestHost* host);

private:
    struct Step { std::string verb, a, b; };
    std::vector<Step> steps_;
    size_t next_ = 0;
    int wait_ = 0;
    // measure state
    bool measuring_ = false;
    uint64_t m_start_ns_ = 0, m_frames0_ = 0, m_len_ns_ = 0;
    double last_fps_ = 0;
};

}  // namespace nxrt::play
