// SPDX-License-Identifier: GPL-3.0-or-later
// Play mode: the glue. See include/nxrt/play.h for the start-up order and docs/S8.md for the design.
//
// Threads:
//   main (the guest main thread, which created the window)  the Update hook (frame boundary: event pump, input, settings changes) and the
//                                                            ProcessInput hook (raw mouse aim) run here
//   presenting thread (the game's render thread)             FrameLimiter::begin_frame (the pacing wait, and the pre-frame hook that changes the
//                                                            present mode), the ImGui overlay and the settings page
//   hid sampling thread                                      PlaySource::poll (200 Hz): reads the pad snapshot and asks the binding engine for the
//                                                            keyboard and mouse contribution at that instant
#include "nxrt/extras.h"
#include "nxrt/lowlat.h"
#include "nxrt/play.h"

#include <windows.h>

#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_scancode.h>
#include <SDL3/SDL_timer.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include "nxrt/audio_sink.h"
#include "nxrt/bugcapture.h"
#include "nxrt/ftrace.h"
#include "nxrt/game.h"
#include "play_display.h"
#include "nxrt/kernel.h"
#include "nxrt/svc.h"
#include "nxrt/config.h"
#include "nxrt/jc2_mouse.h"
#include "nxrt/dispatch.h"
#include "nxrt/hooks.h"
#include "nxrt/input.h"
#include "nxrt/services_api.h"
#include "nxrt/input_source.h"
#include "nxrt/log.h"
#include "nxrt/nvn_backend.h"
#include "nxrt/overlay.h"
#include "nxrt/platform.h"
#include "nxrt/runtime.h"
#include "nxrt/services.h"
#include "nxrt/timestep.h"
#include "nxrt/tt.h"
#include "menu_mouse.h"
#include "play_capture.h"
#include "play_engine.h"
#include "play_game.h"
#include "play_sync.h"
#include "play_selftest.h"
#include "play_types.h"
#include "play_vpad.h"
#include "play_ui.h"
#include "win_menu.h"
#include "prompts.h"
#include "pad_map.h"
#include "pad_orient.h"

namespace nxrt::play {
namespace {
// Keep-alive pump: the window's events are normally pumped at the frame boundary. While the game boots or loads it can go ten seconds and
// more without one, and Windows then calls the window "Not Responding". busy_pump runs before every SVC (svc_set_busy_hook); on the
// thread that owns the window it pumps the events when the last pump is 100 ms old. Elsewhere it costs a thread-id compare.
// A game whose frames come from another thread (play driven from presents, present_frame) asks for a pump at every frame (g_pump_wanted):
// the window thread then pumps at its next SVC, so input reaches the next frame.
std::atomic<DWORD> g_pump_tid{0};
std::atomic<uint64_t> g_last_pump_ms{0};
std::atomic<bool> g_pump_wanted{false};
void busy_pump() {
    if (GetCurrentThreadId() != g_pump_tid.load(std::memory_order_relaxed)) return;
    const uint64_t t = SDL_GetTicks();
    const bool wanted = g_pump_wanted.load(std::memory_order_relaxed) && g_pump_wanted.exchange(false, std::memory_order_relaxed);
    if (!wanted && t - g_last_pump_ms.load(std::memory_order_relaxed) < 100) return;
    g_last_pump_ms.store(t, std::memory_order_relaxed);
    platform::pump_events();
}
}  // namespace

namespace {

constexpr const char* kTag = "play";
constexpr uint64_t kMs = 1000000ull;

// SDL mouse button (1 left, 2 middle, 3 right, 4 X1, 5 X2) to nn::hid::MouseButton (Left 0, Right 1, Middle 2, Forward 3, Back 4).
uint32_t jc2_button_bit(uint8_t b) {
    switch (b) {
        case 1: return 1u << 0;
        case 3: return 1u << 1;
        case 2: return 1u << 2;
        case 5: return 1u << 3;
        case 4: return 1u << 4;
        default: return 0;
    }
}

struct PadFrame {
    uint64_t buttons = 0;
    hid::StickValue left, right;
    hid::SixAxisSample six;
};

struct Touch {
    bool down = false;
    uint32_t x = 0, y = 0;     // on the 1280 x 720 touch screen
};

struct G;

// ---- the hid input source ------------------------------------------------------------------------------------------------------------
class PlaySource : public hid::InputSource {
public:
    explicit PlaySource(Engine* e) : engine_(e) {}
    void poll(hid::InputSnapshot& out) override {
        const EngineOut kb = engine_->output(FrameLimiter::now_ns());
        PadFrame pf;
        Touch t;
        {
            std::lock_guard<std::mutex> lk(mu_);
            pf = pad_;
            t = touch_;
        }
        hid::NpadInput& n = out.npad[0];
        n.connected = true;
        n.style = hid::NpadStyle::FullKey;
        n.buttons = pf.buttons | kb.buttons;
        n.left = combine(pf.left, kb.lx, kb.ly);
        n.right = combine(pf.right, kb.rx, kb.ry);
        n.six_axis = pf.six;
        {   // the pad's orientation (the game's pointer scheme reads it): integrated from the gyro, held level by the accelerometer
            const uint64_t t = FrameLimiter::now_ns();
            const double dt = last_orient_ns_ ? static_cast<double>(t - last_orient_ns_) / 1e9 : 0.0;
            last_orient_ns_ = t;
            orient_.step(pf.six.gyro, pf.six.accel, dt);
            std::memcpy(n.six_axis.direction, orient_.direction, sizeof orient_.direction);
            std::memcpy(n.six_axis.angle, orient_.angle, sizeof orient_.angle);
        }
        right_y_.store(n.right.y, std::memory_order_relaxed);
        right_x_f_.store(static_cast<float>(n.right.x) / 32767.f, std::memory_order_relaxed);
        right_y_f_.store(static_cast<float>(n.right.y) / 32767.f, std::memory_order_relaxed);
        gyro_pitch_.store(std::fabs(pf.six.gyro[0]), std::memory_order_relaxed);
        n.six_axis.gyro[0] *= look_scale_.load(std::memory_order_relaxed);      // the game integrates the pitch rate per call: see play_lookrate.h
        out.touch.count = 0;
        if (t.down) {
            out.touch.count = 1;
            hid::TouchFinger& f = out.touch.fingers[0];
            f = hid::TouchFinger{};
            f.id = 0;
            f.x = t.x;
            f.y = t.y;
        }
    }
    void set_pad(const PadFrame& p) {
        std::lock_guard<std::mutex> lk(mu_);
        pad_ = p;
    }
    // Is a pad looking up or down right now (the right stick's vertical axis off centre, or the gyro turning about the pad's pitch axis)? Read by the aim
    // on the main thread; the values are the ones the game gets.
    bool pad_look(bool gyro_enabled) const {
        return std::abs(right_y_.load(std::memory_order_relaxed)) > 650 || (gyro_enabled && gyro_pitch_.load(std::memory_order_relaxed) > 0.02f);
    }
    // The right stick as the game gets it (dead zone, invert and sensitivity applied), -1..1, + = right and up: the alternate body's camera (title E: the morph ball camera) orbits with it.
    void right_stick(float* x, float* y) const {
        *x = right_x_f_.load(std::memory_order_relaxed);
        *y = right_y_f_.load(std::memory_order_relaxed);
    }
    void set_look_scale(float k) { look_scale_.store(k, std::memory_order_relaxed); }      // the gyro's pitch rate times k (step * 60), for the game's per-call pitch
    void set_touch(const Touch& t) {
        std::lock_guard<std::mutex> lk(mu_);
        touch_ = t;
    }

private:
    static hid::StickValue combine(hid::StickValue pad, float kx, float ky) {
        double x = pad.x + static_cast<double>(kx) * 32767.0, y = pad.y + static_cast<double>(ky) * 32767.0;
        const double len = std::sqrt(x * x + y * y);
        if (len > 32767.0) { x *= 32767.0 / len; y *= 32767.0 / len; }
        hid::StickValue r;
        r.x = static_cast<int32_t>(std::lround(x));
        r.y = static_cast<int32_t>(std::lround(y));
        return r;
    }
    Engine* engine_;
    PadOrient orient_;
    uint64_t last_orient_ns_ = 0;
    std::atomic<int32_t> right_y_{0};
    std::atomic<float> right_x_f_{0.f}, right_y_f_{0.f};
    std::atomic<float> look_scale_{1.f};
    std::atomic<float> gyro_pitch_{0.f};
    std::mutex mu_;
    PadFrame pad_;
    Touch touch_;
};

// ---- the state ------------------------------------------------------------------------------------------------------------------------
struct G {
    // settings
    std::mutex mu;                       // guards settings (written on the main thread, copied by the page on the presenting thread) and pending
    Settings settings;
    Settings pending;
    bool have_pending = false;
    bool dirty = false;                  // settings changed in the game (Alt+Enter): written at stop
    std::string path;
    bool loaded = false, started = false, load_ok = true;

    FrameLimiter limiter;
    std::atomic<double> cap_base{0.0};   // the cap apply_frame_cap chose; the limiter runs at most at [display] console_fps while a movie shows
    std::atomic<bool> movie_capped{false};   // set by the presenting thread: the movie cap is in force
    std::unique_ptr<Engine> engine;
    std::unique_ptr<PlaySource> source_owner;
    std::shared_ptr<hid::InputSource> source;
    std::unique_ptr<gfx::Overlay> overlay;
    gfx::Renderer* renderer = nullptr;
    gfx::Device* device = nullptr;
    bool window = false;

    std::thread::id pump_thread;
    uint64_t base = 0;                   // set by load: [play] game_base, default the game's Info::main_base
    uint32_t hook_update = 0;            // the frame boundary (the game's GamePlay::frame_update)
    bool capture_mouse = true, quit_on_close = true, log_status = false;   // capture_mouse: [play] capture_mouse (false: [mouse] capture = off)
    MouseCapture capture;                // who holds the mouse (play_capture.h); main thread
    bool was_captured = false;           // for the log line on each change
    MouseCapture::Frame cap_frame;       // the last frame's inputs, for the clicks handled before this frame's are known
    bool mouse_stick = false;            // the game takes the mouse as its right stick (GamePlay::mouse_stick_rad_s)
    SettingsFile file;                   // controls.ini shared with the library and editors (play_sync.h)
    uint64_t last_file_poll_ms = 0;
    int bug_key = 0;                     // [bug] key: the bug capture hotkey's SDL scancode (0 = none)
    bool pointer_draw = false;         // [play] pointer_draw: the menu pointer is also drawn into the frame (screenshots show it); a test aid
    bool late_sample = true;           // pump the window's events again right before the mouse counts are taken (S11: the counts are as new as ProcessInput)
    double pitch_limit_deg = 0;

    // frame state (main thread)
    uint64_t update_seq = 0, aim_seq = 0;
    std::atomic<uint64_t> last_pi_ns{0};
    std::atomic<uint64_t> last_pi_frame{0};       // value of `frames` at the last ProcessInput (the game is playing while it keeps being called)
    uint64_t map_acts_seen = 0;
    bool map_hint = false;
    bool overlay_was_visible = false;
    bool was_in_game = false;
    float mouse_x = 0, mouse_y = 0;
    bool mouse1 = false;
    uint32_t jc2_buttons = 0;            // host mouse buttons as nn::hid::MouseButton bits, for the platform 2 controller mouse (hid jc2_mouse.h)
    float jc2_wheel_x = 0, jc2_wheel_y = 0;   // wheel notches since the last feed
    uint64_t last_status_ns = 0;
    bool warned_thread = false;

    // button prompts: the device the player used last (Auto style) and a counter that changes with the bindings
    LastDevice last_dev = LastDevice::None;
    int last_pad_kind = 0;
    uint64_t settings_epoch = 1;

    // pads
    uint64_t last_rumble_ns = 0;
    uint16_t rumble_lo = 0, rumble_hi = 0;

    // present mode requested by the page, applied on the presenting thread
    std::atomic<int> want_present{-1};

    // vibration from the game (hid thread) -> pads (main thread)
    std::atomic<float> vib[2][2] = {};   // [hand][low, high] amplitude

    // rebinding
    std::atomic<int> capture_state{0};   // 0 idle, 1 waiting for an input
    std::mutex cap_mu;
    bool cap_have = false;
    InputId cap_id;
    // press-to-rebind of a pad button (the Gamepad tab)
    std::atomic<int> pad_capture_state{0};   // 0 idle, 1 waiting for a pad button
    std::atomic<uint64_t> pad_capture_since{0};
    bool pad_cap_have = false;
    PadSrc pad_cap_src = PadSrc::South;

    // pads: what each one presses right now (the Input page), and the buttons still held when the menu closed (held back until released)
    std::vector<PadLive> pads_live;      // under st_mu
    uint64_t seen_hides = 0;
    std::map<uint32_t, uint32_t> pad_hold;   // pad id -> SDL button bits, plus bit 16 / 17 for the left / right trigger
    std::atomic<uint64_t> rumble_test_until{0};

    // [config] of the settings file at the start: the keys applied, and the ones the game ini or the environment set instead
    std::set<std::string> config_applied, config_blocked;

    // live status
    mutable std::mutex st_mu;
    LiveStatus live;
    uint64_t fps_last_frames = 0, fps_last_ns = 0;
    double fps = 0;

    SelfTest selftest;

    // counters
    std::atomic<uint64_t> frames{0}, pi_calls{0}, aim_frames{0}, events{0}, counts{0};
    std::atomic<bool> thread_mismatch{false};
};

SelfTestHost* selftest_host();      // defined after the settings page host

G& g() {
    static G inst;
    return inst;
}

uint64_t now_ns() { return FrameLimiter::now_ns(); }

// vsync off: immediate (tearing, lowest latency; mailbox where immediate is missing). vsync on: FIFO, except with [display] fps_limit = 0
// (uncapped): mailbox, so the game renders as fast as it can and the display still shows whole frames.
gfx::PresentMode present_mode_for(const DisplaySettings& d) {
    if (!d.vsync) return gfx::PresentMode::Immediate;
    return d.fps_cap == 0 ? gfx::PresentMode::Mailbox : gfx::PresentMode::Fifo;
}

const char* window_mode_name(platform::FullscreenMode m) {
    switch (m) {
        case platform::FullscreenMode::Windowed: return "windowed";
        case platform::FullscreenMode::Borderless: return "borderless";
        case platform::FullscreenMode::Exclusive: return "exclusive";
    }
    return "?";
}

// Master volume (live). The mute while the window has no focus is set every frame (update_hook) and on focus changes.
void apply_audio(const AudioSettings& a) { audio::set_master_volume(static_cast<float>(a.volume)); }

void apply_unfocused_mute() {
    G& st = g();
    audio::set_muted(st.window && st.settings.audio.mute_unfocused && !platform::focused());
}

// A movie is one picture per frame (timestep::movie_showing): the cap may not exceed the console's rate then, or the movie and its voice lines part.
double movie_cap(double cap) {
    const double cf = config_get_double("display", "console_fps", 60.0);
    return (cap <= 0 || cap > cf) ? cf : cap;
}

void apply_frame_cap(const Settings& s) {
    G& st = g();
    double cap = s.display.fps_cap;
    if (cap < 0) {
        // fps_limit = auto: the display's refresh rate (no limit while it is unknown, e.g. headless runs) for a game whose frame step
        // follows real time (timestep sites). A game without them advances one fixed 1/60 s step per frame, as on the console, where the
        // 60 Hz display paces it: there auto means 60, whatever the monitor runs at. the game on a Remote Desktop display (32 Hz) ran at 32 fps,
        // its 60 fps movies at half speed against their voice lines (A11); a 144 Hz monitor would have run it 2.4 times too fast.
        // A game with sites that still advances something per frame (Sites::cap_at_display_rate false) stays at the console's rate.
        const timestep::Sites* ts = game::info().timestep;
        if (ts && ts->frame_update && ts->cap_at_display_rate && timestep::active_mode()) {
            const float hz = st.window ? platform::display_refresh_hz() : 0.f;
            cap = hz > 0 ? static_cast<double>(hz) : 0;
        } else {
            cap = config_get_double("display", "console_fps", 60.0);
        }
    }
    if (s.display.vsync && s.display.fps_cap == 0) {
        cap = 0;     // uncapped with vsync: mailbox presents, no limiter
    } else if (s.display.vsync) {
        // The display paces; a cap only matters when it is below the refresh rate. With no cap below the refresh rate, the frame limiter still keeps just
        // under it (S21, [latency] vsync_cap): the game otherwise renders as fast as the swapchain takes frames, the queue behind the display
        // fills (three images) and every picture is shown one to two frames after it was simulated; just under the refresh rate the queue stays empty.
        const float hz = st.window ? platform::display_refresh_hz() : 0.f;
        static const bool vsync_cap = config_get_bool("latency", "vsync_cap", true);
        if (cap <= 0 || (hz > 0 && cap >= hz)) cap = (vsync_cap && hz > 0) ? static_cast<double>(hz) * 0.9995 : 0;
    }
    if (cap != st.limiter.target_fps())
        NXLOG_I(kTag, "frame cap: %.2f fps (fps_limit %d, vsync %d, display %.2f Hz)", cap, s.display.fps_cap, s.display.vsync ? 1 : 0,
                st.window ? static_cast<double>(platform::display_refresh_hz()) : 0.0);
    st.cap_base.store(cap);
    if (st.movie_capped) cap = movie_cap(cap);
    st.limiter.set_target_fps(cap);
    extras::lowlat_note_cap(cap);     // Reflex / Anti-Lag pace to the same cap (remembered for a renderer that is created later)
}

// ---- hooks ----------------------------------------------------------------------------------------------------------------------------
void pre_frame(void*) {
    // On the presenting thread, before the frame: the one place a present-mode change cannot collide with a frame in flight.
    G& st = g();
    if (const bool mv = timestep::movie_showing(); mv != st.movie_capped) {   // runs on the presenting thread before every limiter wait
        st.movie_capped = mv;
        const double base = st.cap_base.load();
        const double cap = mv ? movie_cap(base) : base;
        if (cap != st.limiter.target_fps()) {
            NXLOG_I(kTag, "frame cap: %.2f fps (%s)", cap, mv ? "a movie is showing: the console's rate" : "movie over");
            st.limiter.set_target_fps(cap);
        }
    }
    const int want = st.want_present.exchange(-1);
    if (st.renderer && want >= 0) st.renderer->set_present_mode(static_cast<gfx::PresentMode>(want));
    if (st.overlay) st.overlay->apply_present_pending();
}

bool key_sink(const char* name, bool down) {
    InputId id;
    // Scripted pointer: `keydown mpos <x> <y>` puts the pointer at window pixel (x, y) the way a real motion event does (S28 tests).
    if (name && std::strncmp(name, "mpos ", 5) == 0) {
        if (!down) return true;
        float px = 0, py = 0;
        if (std::sscanf(name + 5, "%f %f", &px, &py) != 2) return false;
        input::Event m;
        m.type = input::EventType::MouseMotion;
        m.u.motion.x = px;
        m.u.motion.y = py;
        input::queue_push(m);
        return true;
    }
    // Scripted virtual gamepad (S35 tests): `keydown vpad attach xbox`, `keydown vpad axis ry 0.8`, `keydown vpad button south 1`, `keydown vpad gyro 0 1 0` ...
    if (name && std::strncmp(name, "vpad ", 5) == 0) return !down || vpad_command(name + 5);
    if (!name || !parse_input(name, &id)) return false;
    input::Event e;
    e.timestamp_ns = 0;
    switch (id.kind) {
        case InKind::Key:
            e.type = down ? input::EventType::KeyDown : input::EventType::KeyUp;
            e.u.key.scancode = id.code;
            e.u.key.keycode = static_cast<int32_t>(SDL_GetKeyFromScancode(static_cast<SDL_Scancode>(id.code), SDL_KMOD_NONE, false));
            break;
        case InKind::Mouse:
            e.type = down ? input::EventType::MouseButtonDown : input::EventType::MouseButtonUp;
            e.u.button.button = static_cast<uint8_t>(id.code);
            e.u.button.clicks = 1;
            break;
        case InKind::Wheel:
            if (!down) return true;      // a notch has no release
            e.type = input::EventType::MouseWheel;
            e.u.wheel.dy = static_cast<float>(id.code);
            break;
        default: return false;
    }
    input::queue_push(e);
    return true;
}

// @shot in a script: the renderer writes the next presented frame as a PNG; the call returns when the file exists (or after 5 s).
bool screenshot_provider(const char* path, void*) {
    G& st = g();
    if (!st.renderer || !path) return false;
    DeleteFileA(path);
    if (std::strstr(path, "nvntrace_")) nvn::backend_trace_next_frame();   // `@shot nvntrace_<name>`: also trace the frame that is captured
    // The caller is the game's thread at a frame boundary, and the capture happens at the render thread's next end of frame, which
    // needs the game's next frame: waiting here for the file held every shot for the 5 s timeout. Wait only for an earlier request to
    // be taken (one capture path at a time), then return; the file is written on a worker and the test runner waits for it at exit.
    for (int i = 0; i < 200 && st.renderer->capture_pending(); ++i) Sleep(5);
    st.renderer->request_screenshot(path);
    return true;
}

void vib_sink(uint32_t slot, int hand, const hid::VibrationValue& v, void*) {
    if (slot != 0 || hand < 0 || hand > 1) return;
    G& st = g();
    st.vib[hand][0].store(v.amp_low);
    st.vib[hand][1].store(v.amp_high);
}

}  // namespace

[[noreturn]] void exit_process(int code, const char* verdict, const std::string& message) {
    G& st = g();
    NXLOG_I(kTag, "ending the process (%s): %s", verdict, message.c_str());
    if (st.dirty) {
        std::string err;
        std::lock_guard<std::mutex> lk(st.mu);
        if (!st.file.save(st.settings, &err)) NXLOG_W(kTag, "cannot save %s: %s", st.path.c_str(), err.c_str());
        st.dirty = false;
    }
    nvn::backend_flush_caches();                    // the pipeline cache; this path does not shut the back end down
    nxrt::log_flush();
    if (tt::enabled() && !tt::finished()) tt::end_run(code, verdict, message);     // writes result.json, then terminates
    TerminateProcess(GetCurrentProcess(), static_cast<UINT>(code));
    for (;;) Sleep(1000);
}

namespace {

[[noreturn]] void quit_now(const char* why) { exit_process(0, "pass", why); }

void set_window_mode(const std::string& mode) {
    G& st = g();
    if (!st.window) return;
    const platform::FullscreenMode want = mode == "borderless" ? platform::FullscreenMode::Borderless
                                         : mode == "exclusive" ? platform::FullscreenMode::Exclusive
                                                               : platform::FullscreenMode::Windowed;
    if (platform::fullscreen_mode() == want) return;
    if (!platform::set_fullscreen(want)) {
        NXLOG_W(kTag, "window mode '%s' was refused", mode.c_str());
        // Exclusive fullscreen needs the display to accept a mode change (it does not on a remote or locked session, or with some drivers).
        // The next best thing is borderless at the desktop mode, which always works; the setting stays as the player chose it.
        if (want == platform::FullscreenMode::Exclusive && platform::fullscreen_mode() == platform::FullscreenMode::Windowed) {
            if (platform::set_fullscreen(platform::FullscreenMode::Borderless)) NXLOG_W(kTag, "using borderless fullscreen instead");
            else NXLOG_W(kTag, "borderless fullscreen was refused too; staying windowed");
        }
    }
    st.limiter.reset_schedule();
}

void toggle_fullscreen() {
    G& st = g();
    if (!st.window) return;
    std::string target;
    if (platform::fullscreen_mode() == platform::FullscreenMode::Windowed)
        target = st.settings.display.window_mode == "exclusive" ? "exclusive" : "borderless";
    else
        target = "windowed";
    set_window_mode(target);
    std::lock_guard<std::mutex> lk(st.mu);
    st.settings.display.window_mode = window_mode_name(platform::fullscreen_mode());
    st.dirty = true;
}

void toggle_fps_counter();       // defined after the settings page host

// The bug capture hotkey: a bundle of this moment (nxrt/bugcapture.h: screenshot, log, config, save data, thread dump), then a small
// window of its own asks for a note while the game keeps running ([bug] note_prompt = false: no window).
void bug_hotkey() {
    bug::Request r;
    r.origin = "hotkey";
    r.trace_frame = config_get_bool("bug", "trace_frame", false);
    const std::string dir = bug::capture(r);
    if (dir.empty() || !g().window || !config_get_bool("bug", "note_prompt", true)) return;
    std::thread([dir] {
        services::swkbdui::Request q;
        q.title = std::string(game::window_title()) + ": bug capture";
        q.header = "Saved to " + dir;
        q.guide = "What went wrong? Enter saves the note, Escape leaves it out.";
        q.ok_text = "Save note";
        std::string note;
        if (services::swkbdui::win32_prompt(q, &note) && !note.empty()) {
            bug::add_note(dir, note);
            NXLOG_I(kTag, "bug note saved in %s", dir.c_str());
        }
    }).detach();
}

// Tells the overlay how the frame rate counter should look (the overlay draws it; nothing happens while it is off).
void apply_fps_counter(const FpsSettings& f) {
    G& st = g();
    if (!st.overlay) return;
    gfx::Overlay::CounterConfig c;
    c.show = f.show;
    c.corner = static_cast<int>(f.corner);
    c.detail = f.detail;
    c.in_screenshots = f.in_screenshots;
    st.overlay->set_counter(c);
}

// The pointer is over a slider's track in the game's menu (its menu hooks are active this frame): the wheel steps the value.
bool game_menu_over_value() {
    const GamePlay* gp = game_play();
    return gp && gp->menu_active && gp->menu_over_value && gp->menu_active() && gp->menu_over_value();
}

// The platform 2 controller mouse is on its surface while the game is in play (menus and the map keep their bindings and the menu pointer).
// [input] jc2_clicks: "both" (default) also hands clicks to the bindings (a real platform 2 controller sends its buttons as npad R/ZR),
// "device" sends them to the controller mouse only.
bool jc2_owns_clicks() {
    G& st = g();
    static const bool device_only = config_get_str("input", "jc2_clicks", "both") == "device";
    return device_only && st.engine && ctx_in_play(st.engine->context()) && hid::jc2_mouse_active();
}

void handle_event(const input::Event& e, uint64_t now) {
    G& st = g();
    using input::EventType;
    st.events++;
    // rebinding capture comes first: the page asked for the next input
    if (st.capture_state.load() == 1) {
        InputId id;
        bool got = false, cancel = false;
        if (e.type == EventType::KeyDown && !(e.flags & input::kFlagRepeat)) {
            if (e.u.key.scancode == SDL_SCANCODE_ESCAPE) cancel = true;
            else { id = {InKind::Key, e.u.key.scancode}; got = true; }
        } else if (e.type == EventType::MouseButtonDown) {
            id = {InKind::Mouse, e.u.button.button}; got = true;
        } else if (e.type == EventType::MouseWheel && e.u.wheel.dy != 0) {
            id = {InKind::Wheel, e.u.wheel.dy > 0 ? 1 : -1}; got = true;
        }
        if (got) {
            std::lock_guard<std::mutex> lk(st.cap_mu);
            st.cap_id = id;
            st.cap_have = true;
            st.capture_state.store(0);
            return;
        }
        if (cancel) { st.capture_state.store(0); return; }
        if (e.type == EventType::KeyDown || e.type == EventType::KeyUp) return;    // nothing reaches the game while capturing
    }
    // press-to-rebind of a pad button: the next press on any pad (or a trigger past the threshold) is the answer; it reaches neither the menu nor
    // the game. Releases still go to the menu, so the press that started the capture is not left held there.
    if (st.pad_capture_state.load() == 1) {
        PadSrc src = PadSrc::South;
        bool got = false;
        if (e.type == EventType::GamepadButtonDown) {
            got = pad_src_of_sdl_button(e.u.gpbutton.button, &src);
        } else if (e.type == EventType::GamepadAxis && (e.u.gpaxis.axis == 4 || e.u.gpaxis.axis == 5) &&
                   e.u.gpaxis.value / 32767.0 >= st.settings.pad.trigger_threshold) {
            src = e.u.gpaxis.axis == 4 ? PadSrc::LeftTrigger : PadSrc::RightTrigger;
            got = true;
        }
        if (got) {
            std::lock_guard<std::mutex> lk(st.cap_mu);
            st.pad_cap_src = src;
            st.pad_cap_have = true;
            st.pad_capture_state.store(0);
            return;
        }
        if (e.type == EventType::GamepadButtonDown || e.type == EventType::GamepadAxis) return;
    }
    // The bug capture hotkey ([bug] key, default F9): never reaches the game either.
    if (st.bug_key && (e.type == EventType::KeyDown || e.type == EventType::KeyUp) && e.u.key.scancode == st.bug_key) {
        if (e.type == EventType::KeyDown && !(e.flags & input::kFlagRepeat)) bug_hotkey();
        return;
    }
    // The frame rate counter hotkey: flips the counter and never reaches the game (press, repeat and release).
    if (st.overlay && (e.type == EventType::KeyDown || e.type == EventType::KeyUp) && st.settings.fps.key.kind == InKind::Key &&
        e.u.key.scancode == st.settings.fps.key.code) {
        if (e.type == EventType::KeyDown && !(e.flags & input::kFlagRepeat)) toggle_fps_counter();
        return;
    }
    if (st.overlay && st.overlay->handle_event(e)) return;
    const bool menu_blocked = st.overlay && st.overlay->visible();
    // The mouse capture (play_capture.h): the click that captures and its release never reach the game; the release key lets the mouse go.
    if (st.window) {
        MouseCapture::Frame cf = st.cap_frame;
        cf.focused = platform::focused();
        cf.minimized = platform::minimized();
        cf.overlay = menu_blocked;
        if (e.type == EventType::MouseButtonDown && st.capture.on_button_down(e.u.button.button, cf)) {
            input::mouse_delta_take();
            return;
        }
        if (e.type == EventType::MouseButtonUp && st.capture.eat_release(e.u.button.button)) return;
        if (e.type == EventType::KeyDown || e.type == EventType::KeyUp)
            st.capture.on_key(e.u.key.scancode, e.type == EventType::KeyDown, (e.flags & input::kFlagRepeat) != 0);
    }
    if ((e.type == EventType::KeyDown && !(e.flags & input::kFlagRepeat)) || e.type == EventType::MouseButtonDown || e.type == EventType::MouseWheel)
        st.last_dev = LastDevice::KeyboardMouse;
    switch (e.type) {
        case EventType::Quit:
            if (st.quit_on_close) quit_now("window closed");
            break;
        case EventType::KeyDown:
            if ((e.u.key.scancode == SDL_SCANCODE_RETURN || e.u.key.scancode == SDL_SCANCODE_KP_ENTER) && (e.mods & SDL_KMOD_ALT)) {
                if (!(e.flags & input::kFlagRepeat)) toggle_fullscreen();
                return;
            }
            if (!(e.flags & input::kFlagRepeat)) mm::pointer().on_other_input();
            if (!(e.flags & input::kFlagRepeat) && !menu_blocked) st.engine->key(e.u.key.scancode, true, now);
            break;
        case EventType::KeyUp:
            st.engine->key(e.u.key.scancode, false, now);
            break;
        case EventType::MouseButtonDown:
            if (e.u.button.button == 1) st.mouse1 = true;
            st.jc2_buttons |= jc2_button_bit(e.u.button.button);
            if (!menu_blocked) mm::pointer().on_button(e.u.button.button, true);
            // While the platform 2 controller mouse owns the mouse in play, the left and right buttons belong to the device alone (the middle button stays
            // the mouse-mode key). A release is always passed on: it only matters when the press was seen before the device took over.
            if (!menu_blocked && !(jc2_owns_clicks() && (e.u.button.button == 1 || e.u.button.button == 3))) st.engine->mouse_button(e.u.button.button, true, now);
            break;
        case EventType::MouseButtonUp:
            if (e.u.button.button == 1) st.mouse1 = false;
            st.jc2_buttons &= ~jc2_button_bit(e.u.button.button);
            mm::pointer().on_button(e.u.button.button, false);
            st.engine->mouse_button(e.u.button.button, false, now);
            break;
        case EventType::MouseWheel: {
            if (menu_blocked) break;
            st.jc2_wheel_x += e.u.wheel.dx;
            st.jc2_wheel_y += e.u.wheel.dy;
            if (jc2_owns_clicks()) break;           // the wheel goes to the platform 2 controller mouse only
            const float dy = e.u.wheel.dy;
            // the map: the wheel zooms (the L and R buttons), a longer pulse than a button tap so one notch is a visible step
            if (dy != 0 && st.settings.mouse.menu_cursor && st.engine->context() == Ctx::Map && !(st.overlay && st.overlay->visible())) {
                st.engine->pulse(dy > 0 ? Act::BtnR : Act::BtnL, now, 160);
                break;
            }
            // over a slider's track in a menu the wheel steps the value (right is more) instead of moving the selection
            if (dy != 0 && st.settings.mouse.menu_cursor && mm::pointer().enabled() && game_menu_over_value()) {
                st.engine->pulse(dy > 0 ? Act::DpadRight : Act::DpadLeft, now);
                break;
            }
            const int n = std::max(1, static_cast<int>(std::lround(std::fabs(dy))));
            if (dy != 0) for (int i = 0; i < n; ++i) st.engine->wheel(dy > 0 ? 1 : -1, now);
            break;
        }
        case EventType::MouseMotion:
            if (!(e.flags & input::kFlagRelative)) {
                st.mouse_x = e.u.motion.x;
                st.mouse_y = e.u.motion.y;
                if (!menu_blocked) mm::pointer().on_motion(e.u.motion.x, e.u.motion.y);
                // moving the pointer over a menu counts as using the mouse (the prompts switch to keyboard icons); the raw motion of the aim in play does not
                if (!menu_blocked && st.settings.mouse.menu_cursor && mm::pointer().has_moved()) st.last_dev = LastDevice::KeyboardMouse;
            }
            break;
        case EventType::WindowFocusLost:
            st.capture.on_focus_lost();
            audio::set_muted(st.window && st.settings.audio.mute_unfocused);
            st.engine->release_all(now);
            st.mouse1 = false;
            st.jc2_buttons = 0;
            mm::pointer().on_focus_lost();
            break;
        case EventType::WindowFocusGained:
            audio::set_muted(false);
            st.limiter.reset_schedule();
            break;
        case EventType::WindowMinimized:
            st.capture.release("minimized");
            st.limiter.reset_schedule();
            break;
        case EventType::WindowRestored:
            st.limiter.reset_schedule();
            break;
        case EventType::GamepadAdded:
            NXLOG_I(kTag, "controller %u connected", static_cast<unsigned>(e.u.gamepad.id));
            // a pad that replaces the one in use: the prompts follow its kind at once (without it they change at the first press)
            if (st.last_dev == LastDevice::Pad) st.last_pad_kind = static_cast<int>(input::gamepad_kind_of(e.u.gamepad.id));
            break;
        case EventType::GamepadRemoved:
            NXLOG_I(kTag, "controller %u disconnected", static_cast<unsigned>(e.u.gamepad.id));
            break;
        default: break;
    }
}

// Reads every open pad, merges them (buttons OR, the stick with the larger deflection wins) and publishes the result to the source.
void poll_pads(uint64_t now) {
    G& st = g();
    PadFrame pf;
    input::GamepadInfo infos[8];
    const size_t n = input::gamepad_list(infos, 8);
    bool have_six = false;
    uint16_t lo = 0, hi = 0;
    {
        const float l = std::max(st.vib[0][0].load(), st.vib[1][0].load()), h = std::max(st.vib[0][1].load(), st.vib[1][1].load());
        map_rumble(l, h, st.settings.pad, &lo, &hi);
    }
    if (now < st.rumble_test_until.load()) {   // the Gamepad tab's Test rumble: both motors, through the player's rumble settings
        uint16_t tl = 0, th = 0;
        map_rumble(0.7f, 0.7f, st.settings.pad, &tl, &th);
        lo = std::max(lo, tl);
        hi = std::max(hi, th);
    }
    // The menu just closed: the pad buttons still held (the chord that closed it, a face button) stay held back until they are released, so the
    // game does not get the tail of a press that was meant for the menu.
    const uint64_t hides = st.overlay ? st.overlay->hide_count() : 0;
    const bool menu_closed = hides != st.seen_hides;
    st.seen_hides = hides;
    if (menu_closed) st.pad_hold.clear();
    std::vector<PadLive> live_pads;
    live_pads.reserve(n);
    const Ctx cx = st.engine ? st.engine->context() : Ctx::Menu;
    const bool look_ctx = cx == Ctx::Play || cx == Ctx::Alt || cx == Ctx::Inspect;      // the right stick is a look only in play
    const bool rumble_changed = lo != st.rumble_lo || hi != st.rumble_hi;
    const bool rumble_refresh = (lo || hi) && now - st.last_rumble_ns > 100 * kMs;
    for (size_t i = 0; i < n; ++i) {
        platform::GamepadRaw raw;
        if (!platform::gamepad_poll(infos[i].id, &raw)) continue;
        {
            PadLive pl;
            pl.name = infos[i].name;
            pl.kind = static_cast<int>(infos[i].kind);
            pl.gyro = infos[i].gyro;
            pl.rumble = infos[i].rumble;
            pl.sources = pad_sources_pressed(raw, st.settings.pad.trigger_threshold);
            pl.lx = raw.axes[0] / 32767.f;
            pl.ly = -raw.axes[1] / 32767.f;
            pl.rx = raw.axes[2] / 32767.f;
            pl.ry = -raw.axes[3] / 32767.f;
            pl.lt = raw.axes[4] / 32767.f;
            pl.rt = raw.axes[5] / 32767.f;
            live_pads.push_back(std::move(pl));
        }
        {   // held back since the menu closed: a bit stays until its button is let go
            const double thr = st.settings.pad.trigger_threshold;
            const uint32_t down = raw.buttons | (raw.axes[4] / 32767.0 >= thr ? 1u << 16 : 0u) | (raw.axes[5] / 32767.0 >= thr ? 1u << 17 : 0u);
            uint32_t& hold = st.pad_hold[infos[i].id];
            if (menu_closed) hold = down;
            hold &= down;
            raw.buttons &= ~(hold & 0xffffu);
            if (hold & (1u << 16)) raw.axes[4] = 0;
            if (hold & (1u << 17)) raw.axes[5] = 0;
        }
        hid::NpadInput tmp;
        map_pad(raw, st.settings.pad, &tmp);
        {   // a pressed button or a stick well off centre makes this the pad the prompts follow
            auto off_centre = [](hid::StickValue v) { return static_cast<int64_t>(v.x) * v.x + static_cast<int64_t>(v.y) * v.y > 20000ll * 20000ll; };
            if (tmp.buttons != 0 || off_centre(tmp.left) || off_centre(tmp.right)) {
                st.last_dev = LastDevice::Pad;
                st.last_pad_kind = static_cast<int>(infos[i].kind);
                mm::pointer().on_other_input();
            }
        }
        if (look_ctx) tmp.right = adjust_look_stick(tmp.right, st.settings.pad);
        pf.buttons |= tmp.buttons;
        auto big = [](hid::StickValue a, hid::StickValue b) {
            return static_cast<int64_t>(a.x) * a.x + static_cast<int64_t>(a.y) * a.y >= static_cast<int64_t>(b.x) * b.x + static_cast<int64_t>(b.y) * b.y ? a : b;
        };
        pf.left = big(pf.left, tmp.left);
        pf.right = big(pf.right, tmp.right);
        if (!have_six) {
            float a[3], gy[3];
            if (platform::gamepad_sensors(infos[i].id, a, gy)) {
                map_sensors(a, gy, st.settings.pad, &pf.six);
                have_six = true;
            }
        }
        if (rumble_changed || rumble_refresh) platform::gamepad_rumble(infos[i].id, lo, hi, 250);
    }
    if (rumble_changed || rumble_refresh) {
        st.rumble_lo = lo;
        st.rumble_hi = hi;
        st.last_rumble_ns = now;
    }
    {
        std::lock_guard<std::mutex> lk(st.st_mu);
        st.pads_live = std::move(live_pads);
    }
    // The settings menu is open: the pad does not reach the game (the keyboard and mouse are held back the same way in handle_event).
    if (st.overlay && st.overlay->visible()) {
        pf.buttons = 0;
        pf.left = hid::StickValue{};
        pf.right = hid::StickValue{};
    }
    st.source_owner->set_pad(pf);
}

// ---- the mouse capture's hints and the settings file ----------------------------------------------------------------------------------
void apply_capture_settings(const Settings& s) {
    G& st = g();
    st.capture.set_mode(st.capture_mouse ? s.mouse.capture : CaptureMode::Off);
    st.capture.set_release_key(s.mouse.release_key);
}

void capture_hint(CaptureHint h) {
    G& st = g();
    if (h == CaptureHint::None || !st.overlay) return;
    if (h == CaptureHint::Captured) {
        const int k = st.capture.release_key();
        std::string key = k == SDL_SCANCODE_LALT ? std::string("Alt") : k ? input_name(InputId{InKind::Key, k}) : std::string();
        st.overlay->show_toast(key.empty() ? std::string("Alt+Tab releases the mouse") : key + " releases the mouse", 3.0);
    } else {
        st.overlay->show_toast(st.mouse_stick ? "Click to control the camera with the mouse" : "Click to use the mouse in the game", 4.0);
    }
}

// Let the mouse go now (the menu bar opened, the library takes over). Main thread.
void release_mouse_now(const char* why) {
    G& st = g();
    st.capture.release(why);
    if (st.window && platform::relative_mouse()) platform::set_relative_mouse(false);
}

// Once a second on the window thread: someone else (the library's Settings page, an editor) changed the settings file. The keys they changed
// are taken over into the settings in force (the game's own live state stays) and applied like a change from the menu.
void poll_settings_file() {
    G& st = g();
    static const bool on = config_get_bool("play", "watch_settings", true);
    if (!on) return;
    const uint64_t t = SDL_GetTicks();
    if (t - st.last_file_poll_ms < 1000) return;
    st.last_file_poll_ms = t;
    Settings disk;
    std::string err;
    const SettingsFile::Poll r = st.file.poll(&disk, &err);
    if (r == SettingsFile::Poll::Unreadable) {
        NXLOG_W(kTag, "%s was changed by another program but cannot be read (%s): kept the settings in force", st.path.c_str(), err.c_str());
        return;
    }
    if (r != SettingsFile::Poll::Changed) return;
    const Settings base = st.file.base();
    const std::vector<IniChange> ch = ini_diff(format_settings(base), format_settings(disk));
    std::string keys;
    for (size_t i = 0; i < ch.size() && i < 8; ++i) {
        if (!keys.empty()) keys += ", ";
        keys += "[" + ch[i].section + "]" + (ch[i].kind == IniChange::Kind::Section ? std::string() : " " + ch[i].key);
    }
    if (ch.size() > 8) keys += ", ...";
    std::lock_guard<std::mutex> lk(st.mu);
    const Settings cur = st.have_pending ? st.pending : st.settings;
    Settings merged;
    if (!merge_settings(base, disk, cur, &merged)) {
        NXLOG_W(kTag, "the outside change of %s does not merge: taking the file as it is", st.path.c_str());
        merged = disk;
    }
    st.file.accept(disk);
    if (ch.empty()) return;
    st.pending = merged;
    st.have_pending = true;
    NXLOG_I(kTag, "%s was changed by another program: %zu change%s taken over (%s)", st.path.c_str(), ch.size(), ch.size() == 1 ? "" : "s", keys.c_str());
}

int update_hook(::Ctx*, uint64_t, void*) {
    G& st = g();
    ft::mark("main.update");
    // frame pacing (S18): the limiter waits here, before the input is sampled, so the picture shown is the one simulated one render time
    // earlier. The wait for frames still in flight comes first, then the limiter's schedule.
    // All of it runs once per presented frame: the game's main loop runs more iterations than it presents frames (title A ~79 against ~50 a
    // second), and per iteration the limiter took several slots per frame while the in-flight list (one id per sim_begin, one popped per
    // present) filled up until wait_in_flight held the main thread for presents that only come after more main-loop work.
    const bool frame_start = extras::pace_boundary_due();
    if (frame_start) {
        extras::LowLatency* ll = extras::lowlat();
        if (extras::pace_in_main()) {
            if (ll) ll->wait_in_flight();
            // With Reflex on and a frame cap, the Reflex sleep that follows is the limiter (minimum interval between simulation starts)
            if (!(ll && ll->paces_frames())) { NXFT("limiter.main"); st.limiter.begin_frame_nohook(); }
            extras::pace_boundary_done();
        }
        // low latency (S14): the sleep that lets the frame start as late as the GPU allows, then the input sample marker; the events are pumped right after
        if (ll) {
            ll->sim_begin();
            ll->input_sample();
        }
    }
    const uint64_t now = now_ns();
    st.frames++;
    st.update_seq++;

    const bool on_pump_thread = std::this_thread::get_id() == st.pump_thread;
    if (on_pump_thread) g_last_pump_ms.store(SDL_GetTicks(), std::memory_order_relaxed);
    if (!on_pump_thread && !st.warned_thread) {
        st.warned_thread = true;
        st.thread_mismatch.store(true);
        NXLOG_W(kTag, "the frame boundary runs on another thread than the one that started play mode: the window's events are pumped there at its next SVC");
    }
    if (!on_pump_thread) g_pump_wanted.store(true, std::memory_order_relaxed);
    if (st.window && on_pump_thread) platform::pump_events();

    // settings queued by the page
    {
        std::unique_lock<std::mutex> lk(st.mu);
        if (st.have_pending) {
            Settings s = st.pending;
            st.have_pending = false;
            const std::string old_mode = st.settings.display.window_mode;
            const int old_monitor = st.settings.display.monitor;
            const int old_w = st.settings.display.width, old_h = st.settings.display.height;
            st.settings = s;
            st.settings_epoch++;
            lk.unlock();
            st.engine->set_settings(s, now);
            apply_frame_cap(s);
            display_tuning(s.display);
            if (const GamePlay* gp = game_play(); gp && gp->settings_changed) gp->settings_changed(s);     // the game's display options, lock-on
            extras::set_settings(s.extras);
            extras::hdr_refresh_levels();
            nvn::set_upscaler_replace_enabled(s.display.upscaler_replace);
            apply_fps_counter(s.fps);
            st.want_present.store(static_cast<int>(present_mode_for(s.display)));
            apply_audio(s.audio);
            if (st.window && s.display.monitor != old_monitor && s.display.monitor > 0) {
                if (platform::move_to_display(s.display.monitor)) st.limiter.reset_schedule();
                apply_frame_cap(s);                        // another display can mean another refresh rate
            }
            if (s.display.window_mode != old_mode) {
                set_window_mode(s.display.window_mode);
                apply_frame_cap(s);                        // another window mode can mean another refresh rate (vsync cap)
            }
            // The windowed size is live while the window is windowed; in fullscreen it is the size the window goes back to (the next start).
            if (st.window && (s.display.width != old_w || s.display.height != old_h) && platform::fullscreen_mode() == platform::FullscreenMode::Windowed)
                platform::set_window_size(s.display.width, s.display.height);
            apply_capture_settings(s);
            std::string err;
            bool external = false;
            Settings merged;
            if (!st.file.save(s, &err, &external, &merged)) NXLOG_W(kTag, "cannot save %s: %s", st.path.c_str(), err.c_str());
            else NXLOG_I(kTag, "settings applied and saved to %s", st.path.c_str());
            if (external) {   // another program changed the file meanwhile: its keys were kept, take them over too
                NXLOG_I(kTag, "%s was changed by another program meanwhile: its changes were kept and are applied", st.path.c_str());
                std::lock_guard<std::mutex> lk2(st.mu);
                if (!st.have_pending) {
                    st.pending = merged;
                    st.have_pending = true;
                }
            }
        }
    }

    apply_unfocused_mute();
    if (on_pump_thread) poll_settings_file();

    // The native menu bar: on the window in windowed mode, off in fullscreen. The in-game menu (the overlay's panel, F1 / the pad chord) is the
    // menu only where the bar is not: [gfx] overlay_menu = fullscreen (default), always or never.
    if (st.window) {
        win_menu_frame();
        if (st.overlay) {
            static const std::string mode = config_get_str("gfx", "overlay_menu", "fullscreen");
            const bool bar = win_menu_installed() && win_menu_attached();
            st.overlay->set_menu_enabled(mode == "always" || (mode != "never" && !bar));
        }
    }

    // events
    input::Event buf[128];
    for (;;) {
        const size_t got = input::queue_drain(buf, 128);
        if (!got) break;
        for (size_t i = 0; i < got; ++i) handle_event(buf[i], now);
    }
    poll_pads(now);
    const GamePlay* gp = game_play();
    if (gp && gp->frame_begin) gp->frame_begin();
    if (st.engine) st.engine->frame_tick();
    // A game without an input hook that takes the mouse as its right stick (GamePlay::mouse_stick_rad_s): every frame is play.
    const bool mouse_stick = gp && gp->mouse_stick_rad_s > 0;
    st.mouse_stick = mouse_stick;
    if (mouse_stick) {
        st.last_pi_ns.store(now);
        st.last_pi_frame.store(st.frames.load());
    }

    // what the game is doing: the context, the cycle hint, the pointer
    const uint64_t pi = st.last_pi_ns.load();
    // "In game" = the game called ProcessInput during one of the last few frame boundaries. Counted in frames, not host time: a slow frame (the game
    // can take 100 ms or more per frame under load) must not read as a menu.
    // Also within 100 ms of host time: a catch-up burst of frame boundaries with no input call (1-2 frames about once a second at ~50 fps)
    // dropped the context to the menu and released the mouse for a moment.
    const bool in_game = pi != 0 && (st.frames.load() - st.last_pi_frame.load() <= 4 || pi + 100000000ull >= now);
    // The game's part (play_game.h GamePlay::frame): the gyro's pitch-rate scale for this step, the cycle hint, the context while in game. This
    // step's length is known here, before the game reads the sample (the timestep hook on the same frame update runs first: hooks of one address run
    // in registration order).
    // One of the game's menus over play (bag, shop, choice box: GamePlay::menu_in_game): the frame is a menu frame for the context, the
    // pointer and the mouse capture (with the menu pointer off, play stays play).
    const bool game_menu = gp && gp->menu_in_game && gp->menu_in_game(in_game && st.settings.mouse.menu_cursor) && in_game;
    const bool playing = in_game && !game_menu;
    GameFrame gf;
    if (gp && gp->frame) {
        const double step = timestep::active() ? static_cast<double>(timestep::current_dt()) : static_cast<double>(timestep::kGameStep);
        gp->frame(playing, step, &gf);
    }
    if (st.source_owner) st.source_owner->set_look_scale(gf.look_scale);
    if (gf.hints_valid) st.engine->set_cycle_hint(gf.cycle_index, gf.cycle_mask, now);
    const uint64_t map_acts = st.engine->activations(Act::Map);
    if (in_game && map_acts != st.map_acts_seen) st.map_hint = true;
    st.map_acts_seen = map_acts;
    if (in_game && !st.was_in_game) st.map_hint = false;
    st.was_in_game = in_game;
    Ctx ctx;
    if (!in_game) ctx = st.map_hint ? Ctx::Map : Ctx::Menu;
    else if (game_menu) ctx = Ctx::Menu;
    else ctx = gf.play_ctx;
    st.engine->set_context(ctx, now);

    {   // button prompts: the style that shows, and the icon textures behind it
        PromptInputs pin;
        pin.requested = st.settings.prompts.style;
        pin.label_abxy = st.settings.pad.label_abxy;
        pin.ctx = ctx;
        pin.settings_epoch = st.settings_epoch;
        pin.last = st.last_dev;
        pin.last_pad_kind = st.last_pad_kind;
        input::GamepadInfo pads[1];
        pin.connected_pad_kind = input::gamepad_list(pads, 1) > 0 ? static_cast<int>(pads[0].kind) : -1;
        prompts_update(st.settings, pin);
    }

    const bool overlay_visible = st.overlay && st.overlay->visible();
    if (overlay_visible && !st.overlay_was_visible) st.engine->release_all(now);
    st.overlay_was_visible = overlay_visible;

    if (st.window) {
        // [mouse] capture (play_capture.h): click (default) captures after a click into the picture and never by itself after a release
        MouseCapture::Frame cf;
        cf.focused = platform::focused();
        cf.minimized = platform::minimized();
        cf.overlay = overlay_visible;
        cf.playing = playing;
        cf.held_back = st.capture_state.load() != 0 || hid::jc2_mouse_wants_pointer();
        st.cap_frame = cf;
        const bool want = st.capture.want_relative(cf);
        if (want != platform::relative_mouse()) {
            platform::set_relative_mouse(want);
            if (want) input::mouse_delta_take();      // nothing that piled up before the capture
        }
        if (st.capture.captured() != st.was_captured) {
            st.was_captured = st.capture.captured();
            if (st.was_captured) NXLOG_I(kTag, "mouse captured (a click into the picture; [mouse] release_key lets it go)");
            else NXLOG_I(kTag, "mouse released (%s)", st.capture.last_release());
        }
        capture_hint(st.capture.take_hint(cf, true));
    }
    {   // the map: dragging with the left button rotates it (the left stick), with the middle button moves it (the right stick); the speed is the pointer's speed
        static float px = 0, py = 0;
        static uint64_t last_ns = 0;
        static bool have = false;
        float lx = 0, ly = 0, rx = 0, ry = 0;
        mm::Pointer& pp = mm::pointer();
        const bool on_map = st.window && ctx == Ctx::Map && st.settings.mouse.menu_cursor && pp.enabled() && (platform::focused() || tt::enabled()) && !overlay_visible;
        if (on_map && have && (pp.held() || pp.mid_held())) {
            int ww = 1, wh = 1;
            platform::pixel_size(&ww, &wh);
            // the stick follows the pointer's speed: a full stick is a drag of 0.8 window heights per second, whatever the frame rate
            const float dt = std::max(1e-3f, static_cast<float>(now - last_ns) / 1e9f);
            const float k = 1.f / (0.8f * static_cast<float>(std::max(1, wh)) * dt);
            const float dx = std::max(-1.f, std::min(1.f, (pp.x() - px) * k)), dy = std::max(-1.f, std::min(1.f, (pp.y() - py) * k));
            if (pp.held()) { lx = dx; ly = -dy; }
            else { rx = dx; ry = -dy; }
        }
        // the mouse as the right stick (GamePlay::mouse_stick_rad_s): its turn rate over the frame, full stick at that rate
        if (mouse_stick && playing && platform::relative_mouse() && have && !on_map) {
            const double dt = std::max(1e-3, static_cast<double>(now - last_ns) / 1e9);
            const input::MouseDelta d = input::mouse_delta_take();
            const MouseSettings& ms = st.settings.mouse;
            const double sx = d.dx * ms.yaw_rad / dt / gp->mouse_stick_rad_s * (ms.invert_x ? -1 : 1);
            const double sy = -d.dy * ms.pitch_rad / dt / gp->mouse_stick_rad_s * (ms.invert_y ? -1 : 1);
            rx = static_cast<float>(std::max(-1.0, std::min(1.0, sx)));
            ry = static_cast<float>(std::max(-1.0, std::min(1.0, sy)));
        }
        px = pp.x();
        py = pp.y();
        last_ns = now;
        have = true;
        st.engine->set_aux_sticks(lx, ly, rx, ry);
    }
    {   // the mouse as a pointer in the menus (S28): the cursor, and the snapshot the widget hooks read this frame
        mm::Pointer& ptr = mm::pointer();
        ptr.set_enabled(st.settings.mouse.menu_cursor);
        // (a scripted run has no focus: the test tooling drives the pointer with synthetic events)
        const bool menu_up = st.window && !playing && ctx == Ctx::Menu && (platform::focused() || tt::enabled()) && !platform::minimized() && !overlay_visible;
        if (tt::enabled()) {   // test runs: why the menu pointer is or is not up, on each change
            static int last = -1;
            const int sig = (st.window ? 1 : 0) | (playing ? 2 : 0) | (static_cast<int>(ctx) << 2) | (overlay_visible ? 256 : 0);
            if (sig != last && (last = sig, true))
                NXLOG_I("play", "menu pointer gate: window %d playing %d ctx %d (menu %d) minimized %d overlay %d -> %d", st.window ? 1 : 0, playing ? 1 : 0,
                        static_cast<int>(ctx), static_cast<int>(Ctx::Menu), platform::minimized() ? 1 : 0, overlay_visible ? 1 : 0, menu_up ? 1 : 0);
        }
        mm::Geometry geo;
        platform::pixel_size(&geo.win_w, &geo.win_h);
        const DisplayLive dl = gp && gp->display_live ? gp->display_live() : DisplayLive{};
        geo.render_w = dl.render_w;
        geo.render_h = dl.render_h;
        geo.display_aspect = dl.aspect > 0 ? dl.aspect : 0;
        geo.keep_aspect = st.settings.display.keep_aspect;
        if (const Act a = gp && gp->menu_frame ? gp->menu_frame(menu_up, geo) : Act::None; a != Act::None) st.engine->pulse(a, now);
        if (st.overlay && st.pointer_draw) st.overlay->set_pointer(menu_up && ptr.enabled() && ptr.cursor_visible(menu_up), ptr.x(), ptr.y(), true);
        // In play with the platform 2 controller mouse on its surface the game draws its own cursor: the OS cursor is hidden (unless the overlay is up).
        const bool game_cursor = playing && !overlay_visible && hid::jc2_mouse_active();
        if (st.window && !platform::relative_mouse())
            platform::set_cursor_visible(!game_cursor && (overlay_visible || !(ptr.enabled() && menu_up) || ptr.cursor_visible(menu_up)));
        if (st.window) {   // the platform 2 controller mouse (hid): the pointer on the picture, the buttons and the wheel
            hid::HostMouse hm;
            bool inside = false;
            mm::window_to_picture(geo, st.mouse_x, st.mouse_y, &hm.u, &hm.v, &inside);
            hm.buttons = overlay_visible ? 0u : st.jc2_buttons;
            hm.wheel_x = st.jc2_wheel_x;
            hm.wheel_y = st.jc2_wheel_y;
            hm.focused = (platform::focused() || tt::enabled()) && !platform::minimized() && !overlay_visible;
            hm.captured = platform::relative_mouse();
            hid::jc2_mouse_feed(hm);
            st.jc2_wheel_x = st.jc2_wheel_y = 0;
        }
    }
    if (!playing) input::mouse_delta_take();         // aim only counts what arrives while the game is playing (not under one of its menus)

    if (st.log_status && now - st.last_status_ns > 1000 * kMs) {
        st.last_status_ns = now;
        GameStatus gs;
        if (gp && gp->status) gp->status(&gs);
        char game_part[512] = "";
        if (gp && gp->status_text) gp->status_text(game_part, sizeof game_part);
        NXLOG_I(kTag, "status: ctx=%s in_game=%d aim=%s frames=%llu process_input=%llu aim_frames=%llu events=%llu pads=%zu captured=%d%s", ctx_name(ctx),
                in_game ? 1 : 0, gs.aim, static_cast<unsigned long long>(st.frames.load()),
                static_cast<unsigned long long>(st.pi_calls.load()), static_cast<unsigned long long>(st.aim_frames.load()),
                static_cast<unsigned long long>(st.events.load()), input::gamepad_count(), platform::relative_mouse() ? 1 : 0, game_part);
    }
    if (st.selftest.active()) st.selftest.tick(st.renderer, &st.limiter, selftest_host());
    return 0;
}

// ---- what the game module gets from play mode (play_game.h GameHost) ----------------------------------------------------------------------
class GameHostImpl : public GameHost {
public:
    const Settings& settings() const override { return g().settings; }
    bool action_active(Act a, uint64_t now_ns) const override { return g().engine && g().engine->act_active(a, now_ns); }
    uint64_t action_activations(Act a) const override { return g().engine ? g().engine->activations(a) : 0; }
    void right_stick(float* x, float* y) const override {
        *x = *y = 0;
        if (g().source_owner) g().source_owner->right_stick(x, y);
    }
    bool pad_look(bool gyro_enabled) const override { return g().source_owner && g().source_owner->pad_look(gyro_enabled); }
    bool has_input() const override { return g().source_owner != nullptr; }
    void input_tick(double* dx, double* dy) override {
        G& st = g();
        const uint64_t now = now_ns();
        st.last_pi_ns.store(now);
        st.last_pi_frame.store(st.frames.load());
        st.pi_calls++;
        *dx = *dy = 0;
        if (st.update_seq != st.aim_seq) {                // the first input tick since the frame boundary takes the mouse counts
            st.aim_seq = st.update_seq;
            // Late sample: the frame boundary pumped the events at the start of the frame update; the tick that reaches the input function is a little
            // later (the whole update body runs first). Pump again so the counts include whatever the mouse did in between. Main thread only, like the
            // pump in the frame-boundary hook; the key and button events it queues are handled at the next boundary.
            if (st.late_sample && st.window && std::this_thread::get_id() == st.pump_thread) platform::pump_events();
            if (extras::LowLatency* ll = extras::lowlat()) ll->input_sample();
            const input::MouseDelta d = input::mouse_delta_take();
            *dx = d.dx;
            *dy = d.dy;
        }
    }
    void aim_applied(double dx, double dy) override {
        G& st = g();
        st.aim_frames++;
        st.counts += static_cast<uint64_t>(std::fabs(dx) + std::fabs(dy));
    }
    double pitch_limit_deg() const override { return g().pitch_limit_deg; }
    bool log_status() const override { return g().log_status; }
};
GameHostImpl g_game_host;

// ---- the settings page host -----------------------------------------------------------------------------------------------------------
class Host : public UiHost {
public:
    Settings settings() const override { G& st = g(); std::lock_guard<std::mutex> lk(st.mu); return st.have_pending ? st.pending : st.settings; }
    Settings factory() const override { return default_settings(); }
    void apply(const Settings& s) override { G& st = g(); std::lock_guard<std::mutex> lk(st.mu); st.pending = s; st.have_pending = true; }
    LiveStatus status() const override {
        G& st = g();
        LiveStatus s;
        s.ctx = st.engine ? st.engine->context() : Ctx::Menu;
        GameStatus gs;
        if (const GamePlay* gp = game_play(); gp && gp->status) gp->status(&gs);
        s.aim = gs.aim;
        s.prompt_now = prompts_effective_name();
        if (st.pi_calls.load() == 0) s.aim = "not in game";
        s.mouse_captured = st.window && platform::relative_mouse();
        input::GamepadInfo infos[4];
        s.pads = static_cast<int>(input::gamepad_count());
        if (input::gamepad_list(infos, 4) > 0) {
            s.pad_name = infos[0].name;
            s.pad_gyro = infos[0].gyro;
            s.pad_rumble = infos[0].rumble;
        }
        if (st.renderer) {
            s.present_mode = gfx::present_mode_name(st.renderer->present_mode());
            const uint64_t now = now_ns();
            const uint64_t frames = st.renderer->stats().frames;
            std::lock_guard<std::mutex> lk(st.st_mu);
            if (now - st.fps_last_ns > 1000 * kMs) {
                if (st.fps_last_ns) st.fps = static_cast<double>(frames - st.fps_last_frames) * 1e9 / static_cast<double>(now - st.fps_last_ns);
                st.fps_last_frames = frames;
                st.fps_last_ns = now;
            }
            s.fps = st.fps;
        }
        if (st.window) {
            s.window_mode = window_mode_name(platform::fullscreen_mode());
            s.refresh_hz = platform::display_refresh_hz();
            platform::pixel_size(&s.width, &s.height);
        }
        s.out = st.engine->output(now_ns());
        s.pitch_deg = gs.pitch_rad * 180.0 / kPi;
        s.input_calls = st.pi_calls.load();
        {
            std::lock_guard<std::mutex> lk(st.st_mu);
            s.pad_list = st.pads_live;
        }
        s.docked = services::amctl::docked();
        s.upscaler_configured = nvn::upscaler_replace_configured();
        return s;
    }
    std::string settings_path() const override { return g().path; }
    void begin_capture() override {
        G& st = g();
        std::lock_guard<std::mutex> lk(st.cap_mu);
        st.cap_have = false;
        st.capture_state.store(1);
    }
    bool capturing() const override { return g().capture_state.load() == 1; }
    bool capture_result(InputId* out) override {
        G& st = g();
        std::lock_guard<std::mutex> lk(st.cap_mu);
        if (!st.cap_have) return false;
        st.cap_have = false;
        if (out) *out = st.cap_id;
        return true;
    }
    void cancel_capture() override { g().capture_state.store(0); }
    bool reserved_key(const InputId& id) const override {
        G& st = g();
        return st.overlay && id.kind == InKind::Key && id.code == st.overlay->toggle_scancode();
    }
    void begin_pad_capture() override {
        G& st = g();
        std::lock_guard<std::mutex> lk(st.cap_mu);
        st.pad_cap_have = false;
        st.pad_capture_since.store(now_ns());
        st.pad_capture_state.store(1);
    }
    bool pad_capturing() const override {
        G& st = g();
        if (st.pad_capture_state.load() != 1) return false;
        if (now_ns() - st.pad_capture_since.load() > 8000 * kMs) {   // nobody pressed anything: give up
            st.pad_capture_state.store(0);
            return false;
        }
        return true;
    }
    bool pad_capture_result(PadSrc* out) override {
        G& st = g();
        std::lock_guard<std::mutex> lk(st.cap_mu);
        if (!st.pad_cap_have) return false;
        st.pad_cap_have = false;
        if (out) *out = st.pad_cap_src;
        return true;
    }
    void cancel_pad_capture() override { g().pad_capture_state.store(0); }
    void test_rumble() override { g().rumble_test_until.store(now_ns() + 500 * kMs); }
    void draw_debug() override {
        G& st = g();
        gfx::Overlay::Context c;
        c.device = st.device;
        c.renderer = st.renderer;
        c.limiter = &st.limiter;
        c.has_window = st.window;
        gfx::draw_debug_page(c);
    }
    ConfigKeyState config_key(const std::string& name) const override {
        G& st = g();
        ConfigKeyState k;
        const size_t dot = name.find('.');
        if (dot == std::string::npos) return k;
        const std::string sec = name.substr(0, dot), key = name.substr(dot + 1);
        k.now = config_get_str(sec.c_str(), key.c_str(), "");
        if (std::getenv(config_env_name(sec.c_str(), key.c_str()).c_str())) k.from = "env";
        else if (st.config_applied.count(name)) k.from = "controls.ini";
        else if (config_user_set(sec.c_str(), key.c_str())) k.from = "ini";
        else if (config_has(sec.c_str(), key.c_str())) k.from = "game";
        return k;
    }
    void set_docked_now(bool docked) override {
        NXLOG_I(kTag, "the menu tells the game it is %s now", docked ? "docked" : "handheld");
        services::amctl::set_docked(docked);
    }
};
Host g_host;

void toggle_fps_counter() {
    Settings s = g_host.settings();
    s.fps.show = !s.fps.show;
    g_host.apply(s);
}

// The self-test's window onto play mode: the same entry points the settings page uses.
class SelfTestHostImpl : public SelfTestHost {
public:
    bool set(const std::string& key, const std::string& value) override {
        G& st = g();
        Settings s = g_host.settings();
        const double d = std::atof(value.c_str());
        if (key == "vsync") s.display.vsync = d != 0;
        else if (key == "fps_cap" || key == "fps_limit") s.display.fps_cap = static_cast<int>(d);
        else if (key == "window_mode") s.display.window_mode = value;
        else if (key == "yaw_source") s.mouse.yaw_rad = rad_per_count(d);
        else if (key == "pitch_source") s.mouse.pitch_rad = rad_per_count(d);
        else if (key == "invert_y") s.mouse.invert_y = d != 0;
        else if (key == "invert_x") s.mouse.invert_x = d != 0;
        else if (key == "fps") s.fps.show = d != 0;
        else if (key == "fps_in_shots") s.fps.in_screenshots = d != 0;
        else if (key == "fps_corner") {
            if (!parse_corner(value, &s.fps.corner)) return false;
        } else if (key == "fps_detail") s.fps.detail = std::clamp(static_cast<int>(d), 0, 2);
        else if (key == "prompt_style") {
            if (!parse_prompt_style(value, &s.prompts.style)) return false;
        } else return false;
        g_host.apply(s);
        (void)st;
        return true;
    }
    std::string present_mode() override { G& st = g(); return st.renderer ? gfx::present_mode_name(st.renderer->present_mode()) : ""; }
    std::string settings_text() override {
        G& st = g();
        std::string text;
        if (FILE* f = std::fopen(st.path.c_str(), "rb")) {
            char buf[4096];
            size_t n;
            while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
            std::fclose(f);
        }
        return text;
    }
};

SelfTestHost* selftest_host() {
    static SelfTestHostImpl h;
    return &h;
}

// the game's ini and the environment override the settings file for the display extras. A key that is set there ([hdr] mode in the game's ini, or NXRT_HDR_MODE)
// replaces the file's value with the one extras::settings_from_config() reads; other keys keep the file's value (the F1 page edits the file).
extras::Settings merge_env(const extras::Settings& from_file) {
    const extras::Settings env = extras::settings_from_config();
    extras::Settings r = from_file;
    auto set = [](const char* name) {
        const char* v = std::getenv(name);
        return v != nullptr && v[0] != '\0';
    };
    if (config_has("hdr", "mode")) r.hdr.mode = env.hdr.mode;
    if (config_has("hdr", "format")) r.hdr.format = env.hdr.format;
    if (config_has("hdr", "peak_nits")) r.hdr.peak_nits = env.hdr.peak_nits;
    if (config_has("hdr", "paper_white_nits")) r.hdr.paper_white_nits = env.hdr.paper_white_nits;
    if (config_has("hdr", "highlight_boost")) r.hdr.highlight_boost = env.hdr.highlight_boost;
    if (config_has("upscale", "mode")) r.upscale.mode = env.upscale.mode;
    if (config_has("upscale", "sharpness")) r.upscale.sharpness = env.upscale.sharpness;
    if (config_has("upscale", "quality")) r.upscale.quality = env.upscale.quality;
    if (config_has("upscale", "scene_scale")) r.upscale.scene_scale = env.upscale.scene_scale;
    if (config_has("upscale", "fsr_dll")) r.upscale.fsr_dll = env.upscale.fsr_dll;
    if (config_has("upscale", "dlss_dll")) r.upscale.dlss_dll = env.upscale.dlss_dll;
    if (config_has("upscale", "xess_dll")) r.upscale.xess_dll = env.upscale.xess_dll;
    if (config_has("upscale", "frame_generation")) r.upscale.frame_generation = env.upscale.frame_generation;
    if (config_has("latency", "mode")) r.latency.mode = env.latency.mode;
    if (config_has("latency", "log_timings")) r.latency.log_timings = env.latency.log_timings;
    // [picture]: a preset or any of the four keys in the game's ini / the environment replaces the file's picture controls as a whole
    if (config_has("picture", "preset") || config_has("picture", "contrast") || config_has("picture", "saturation") || config_has("picture", "black") ||
        config_has("picture", "gamma"))
        r.picture = env.picture;
    return r;
}

// The library (recomp_library.exe) started the game: NXRT_LIBRARY_EXE names it, NXRT_LIBRARY_GAME the game id. Its Settings page edits the same
// controls.ini; the game takes the changes over live (poll_settings_file).
std::wstring library_exe() {
    wchar_t buf[MAX_PATH * 2];
    const DWORD n = GetEnvironmentVariableW(L"NXRT_LIBRARY_EXE", buf, MAX_PATH * 2);
    if (n == 0 || n >= MAX_PATH * 2) return {};
    return std::wstring(buf, n);
}
bool library_exe_ok() {
    const std::wstring exe = library_exe();
    if (exe.empty()) return false;
    const DWORD a = GetFileAttributesW(exe.c_str());
    const bool ok = a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
    if (!ok) NXLOG_W(kTag, "NXRT_LIBRARY_EXE names no file: the settings dialogs of the game are used");
    return ok;
}
bool open_library(int tab) {
    const std::wstring exe = library_exe();
    if (exe.empty()) return false;
    std::wstring id;
    {
        wchar_t buf[256];
        const DWORD n = GetEnvironmentVariableW(L"NXRT_LIBRARY_GAME", buf, 256);
        if (n > 0 && n < 256) id.assign(buf, n);
        else for (const char* p = game::info().id; p && *p; ++p) id += static_cast<wchar_t>(*p);
    }
    release_mouse_now("library opened");
    std::wstring cmd = L"\"" + exe + L"\" --game " + id + L" --page 2 --tab " + std::to_wstring(tab);
    std::wstring dir = exe;
    const size_t slash = dir.find_last_of(L"\\/");
    dir = slash == std::wstring::npos ? std::wstring() : dir.substr(0, slash);
    AllowSetForegroundWindow(ASFW_ANY);    // the library (single instance) brings its window to the front
    STARTUPINFOW si = {sizeof si};
    PROCESS_INFORMATION pi = {};
    std::vector<wchar_t> line(cmd.begin(), cmd.end());
    line.push_back(L'\0');
    if (!CreateProcessW(exe.c_str(), line.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.empty() ? nullptr : dir.c_str(), &si, &pi)) {
        NXLOG_W(kTag, "cannot start the library (%lu)", GetLastError());
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    NXLOG_I(kTag, "library settings opened (tab %d)", tab);
    return true;
}

}  // namespace

// The native menu bar (win_menu.h) with play mode's own actions.
void install_menu_bar() {
    G& st = g();
    WinMenuHooks h;
    if (st.bug_key) {
        h.capture_bug = [] { bug_hotkey(); };
        h.bug_key = SDL_GetScancodeName(static_cast<SDL_Scancode>(st.bug_key));
    }
    h.fps_key = input_name(st.settings.fps.key);
    h.quit = [] { quit_now("Game > Exit"); };
    h.save_now = [](std::string* err) {
        G& s = g();
        Settings cur = g_host.settings();
        bool external = false;
        Settings merged;
        if (!s.file.save(cur, err, &external, &merged)) return false;
        if (external) g_host.apply(merged);   // another program's changes were kept: take them over
        NXLOG_I(kTag, "settings saved to %s (menu)", s.path.c_str());
        return true;
    };
    h.reload = [](std::string* err) {
        Settings fresh;
        if (!load_settings(g().path, &fresh, err)) return false;
        g().file.note_current();             // the file as it is now is what the game has
        fresh.fps = g_host.settings().fps;   // the counter is live state, as the in-game page keeps it
        g_host.apply(fresh);
        NXLOG_I(kTag, "settings reloaded from %s (menu)", g().path.c_str());
        return true;
    };
    h.set_debug_overlay = [](bool on) {
        if (g().overlay) g().overlay->set_debug_window(on);
    };
    h.debug_overlay = [] { return g().overlay && g().overlay->debug_window(); };
    h.debug_report = [] {
        G& s = g();
        gfx::Overlay::Context c;
        c.device = s.device;
        c.renderer = s.renderer;
        c.limiter = &s.limiter;
        c.has_window = s.window;
        return gfx::debug_report_text(c);
    };
    if (const GamePlay* gp = game_play()) h.display_options = gp->display_options;
    h.upscaler_configured = nvn::upscaler_replace_configured();
    h.menu_opened = [] { release_mouse_now("menu bar opened"); };
    if (library_exe_ok()) h.open_library = [](int tab) { return open_library(tab); };
    if (!win_menu_install(platform::native_hwnd(), platform::native_window(), &g_host, h)) NXLOG_W(kTag, "no menu bar on the game window");
}

// ---- public API ---------------------------------------------------------------------------------------------------------------------------
bool load() {
    G& st = g();
    if (st.loaded) return st.load_ok;
    st.loaded = true;
    st.path = config_get_str("play", "controls", "controls.ini");
    st.file.set_path(st.path);
    std::string err;
    const bool existed = GetFileAttributesA(st.path.c_str()) != INVALID_FILE_ATTRIBUTES;
    st.load_ok = load_settings(st.path, &st.settings, &err);
    if (!existed) {
        std::string e2;
        if (save_settings(st.path, st.settings, &e2)) NXLOG_I(kTag, "wrote the factory settings to %s", st.path.c_str());
    } else if (!st.load_ok) {
        NXLOG_W(kTag, "%s: %s (the factory settings are in use)", st.path.c_str(), err.c_str());
    } else {
        NXLOG_I(kTag, "settings loaded from %s (%zu bindings)", st.path.c_str(), st.settings.bindings.size());
    }
    // An older controls.ini got the newer factory bindings (play_config.cpp, kDefaultAdditions): keep the old file once as .bak, then
    // write the upgraded one. CopyFileA with bFailIfExists = TRUE leaves an existing .bak alone.
    if (existed && st.load_ok && st.settings.migrated) {
        const std::string bak = st.path + ".bak";
        if (CopyFileA(st.path.c_str(), bak.c_str(), TRUE)) NXLOG_I(kTag, "kept the previous %s as %s", st.path.c_str(), bak.c_str());
        std::string e3;
        if (save_settings(st.path, st.settings, &e3))
            NXLOG_I(kTag, "controls.ini updated to defaults revision %d (added: %s)", st.settings.defaults_rev,
                    st.settings.migration_added.empty() ? "nothing new" : st.settings.migration_added.c_str());
        else
            NXLOG_W(kTag, "cannot save the upgraded %s: %s", st.path.c_str(), e3.c_str());
    }
    st.file.note_current();   // what the game read (or wrote): the base its own saves and the outside changes are measured against
    // [config]: restart-only runtime keys the menu set. Each becomes [section] key unless the game ini or the environment sets it (a game module's
    // own default is replaced). Read before anything else reads them: the device, the services' profile and the back end start after this.
    {
        bool profile_keys = false;
        for (const auto& e : st.settings.config) {
            const size_t dot = e.first.find('.');
            if (dot == std::string::npos) continue;
            const std::string sec = e.first.substr(0, dot), key = e.first.substr(dot + 1);
            if (config_user_set(sec.c_str(), key.c_str())) {
                st.config_blocked.insert(e.first);
                NXLOG_I(kTag, "%s [config] %s = %s not used: the %s sets it", st.path.c_str(), e.first.c_str(), e.second.c_str(),
                        std::getenv(config_env_name(sec.c_str(), key.c_str()).c_str()) ? "environment" : "ini");
                continue;
            }
            config_set(sec.c_str(), key.c_str(), e.second.c_str());
            st.config_applied.insert(e.first);
            if (sec == "am" || sec == "system") profile_keys = true;
            NXLOG_I(kTag, "[%s] %s = %s (from %s)", sec.c_str(), key.c_str(), e.second.c_str(), st.path.c_str());
        }
        if (profile_keys) {   // the services' profile may have been read already: the operation mode comes from it
            services::profile_reload();
            services::amctl::set_docked(services::profile().docked);
        }
    }
    nvn::set_upscaler_replace_enabled(st.settings.display.upscaler_replace);
    // Display extras: a value set in the game's ini or in the environment (NXRT_HDR_MODE ...) overrides the file; the file overrides the built-in defaults.
    st.settings.extras = merge_env(st.settings.extras);
    extras::set_settings(st.settings.extras);
    // S38 GPU paths: the file's choice becomes [nvn] pool_mirror / pass_barriers unless the game's ini or the environment set them. The replay
    // reads both once, at its first pool and its first submission, after this.
    if (!config_has("nvn", "pool_mirror")) config_set("nvn", "pool_mirror", st.settings.display.fast_gpu_memory ? "on" : "off");
    if (!config_has("nvn", "pass_barriers")) config_set("nvn", "pass_barriers", st.settings.display.fewer_gpu_waits ? "tracked" : "full");
    // Prewarm: the file's choice becomes [nvn] prewarm unless the ini or the environment set it (read once, by the pipeline prewarm).
    if (!config_has("nvn", "prewarm")) config_set("nvn", "prewarm", st.settings.display.shader_prewarm ? "true" : "false");
    // ASTC texture quality: the file's choice becomes [gfx] astc_quality unless the ini or the environment set it (the texture back end reads it at its first ASTC texture).
    if (!config_has("gfx", "astc_quality")) config_set("gfx", "astc_quality", st.settings.display.astc_quality.c_str());
    // Audio: a volume or device set in the ini or the environment wins over the file; the page then shows what is in force.
    if (config_has("audio", "volume")) st.settings.audio.volume = audio::master_volume();
    else audio::set_master_volume(static_cast<float>(st.settings.audio.volume));
    if (config_has("audio", "device")) st.settings.audio.device = config_get_str("audio", "device", "default");
    else config_set("audio", "device", st.settings.audio.device.c_str());
    st.capture_mouse = config_get_bool("play", "capture_mouse", true);
    apply_capture_settings(st.settings);
    st.late_sample = config_get_bool("play", "late_sample", true);
    st.quit_on_close = config_get_bool("play", "quit_on_close", true);
    st.log_status = config_get_bool("play", "log_status", false);
    st.pointer_draw = config_get_bool("play", "pointer_draw", false);
    st.pitch_limit_deg = config_get_double("play", "pitch_limit_deg", 0.0);
    st.base = static_cast<uint64_t>(config_get_int("play", "game_base", static_cast<int64_t>(game::info().main_base)));
    apply_frame_cap(st.settings);
    return st.load_ok;
}

DisplayConfig display() {
    G& st = g();
    load();
    DisplayConfig d;
    d.width = st.settings.display.width;
    d.height = st.settings.display.height;
    d.window_mode = st.settings.display.window_mode;
    d.vsync = st.settings.display.vsync;
    d.fps_cap = st.settings.display.fps_cap;
    d.keep_aspect = st.settings.display.keep_aspect;
    d.present_mode = present_mode_for(st.settings.display);
    return d;
}

FrameLimiter* limiter() {
    load();
    return &g().limiter;
}

std::string settings_path() {
    load();
    return g().path;
}

bool start(const StartOptions& opt) {
    G& st = g();
    load();
    if (st.started) return true;
    st.renderer = opt.renderer;
    st.device = opt.device;
    st.window = opt.window && platform::has_window();
    apply_frame_cap(st.settings);                      // the display's refresh rate is known now (vsync cap)
    kernel::sched_set_process_priority(st.window);     // a person is playing: keep the frame out of the background load
    st.pump_thread = std::this_thread::get_id();
    g_pump_tid.store(GetCurrentThreadId(), std::memory_order_relaxed);
    if (st.window) svc_set_busy_hook(&busy_pump);       // the window keeps answering while the game boots or loads (no frame boundary for seconds)
    st.engine = std::make_unique<Engine>(st.settings);
    st.engine->set_context(Ctx::Menu, now_ns());
    st.source_owner = std::make_unique<PlaySource>(st.engine.get());
    st.source = std::shared_ptr<hid::InputSource>(st.source_owner.get(), [](hid::InputSource*) {});   // owned by G
    hid::set_input_source(st.source);
    hid::set_vibration_sink(&vib_sink, nullptr);
    tt::set_mouse_sink(&input::mouse_delta_add);
    tt::set_key_sink(&key_sink);
    if (st.renderer) tt::set_screenshot_provider(&screenshot_provider, nullptr);
    if (st.renderer) bug::set_screenshot_fn([](const char* path) { return screenshot_provider(path, nullptr); });
    bug::set_trace_fn(&nvn::backend_trace_next_frame);
    if (const std::string k = config_get_str("bug", "key", "F9"); !k.empty() && k != "none") {
        st.bug_key = static_cast<int>(SDL_GetScancodeFromName(k.c_str()));
        if (!st.bug_key) NXLOG_W(kTag, "[bug] key '%s' is not a key name (SDL names: F9, Pause, ...): no bug capture hotkey", k.c_str());
    }
    if (const std::string script = config_get_str("play", "selftest", ""); !script.empty()) {
        std::string err;
        if (!st.selftest.parse(script, &err)) {
            NXLOG_E(kTag, "[play] selftest: %s", err.c_str());
            nxrt::log_flush();
            TerminateProcess(GetCurrentProcess(), 2);
        }
        NXLOG_I(kTag, "selftest script: %s", script.c_str());
    }

    // The software keyboard asks the player in a small native window while there is a game window ([applet] swkbd_prompt = false: never).
    services::swkbdui::enable_prompt(st.window && config_get_bool("applet", "swkbd_prompt", true));
    if (st.renderer) {
        st.limiter.set_pre_frame_hook(&pre_frame, nullptr);
        if (!st.renderer->headless()) st.renderer->set_present_mode(present_mode_for(st.settings.display));
        if (opt.overlay && opt.device) {
            gfx::Overlay::Context oc;
            oc.device = opt.device;
            oc.renderer = opt.renderer;
            oc.limiter = &st.limiter;
            oc.has_window = st.window;
            oc.diagnostics_window = false;     // the menu's Debug page shows it
            std::string err;
            st.overlay = gfx::Overlay::create(oc, &err);
            if (!st.overlay) {
                NXLOG_W(kTag, "no overlay: %s", err.c_str());
            } else {
                const std::string title = std::string(game::info().short_title && *game::info().short_title ? game::info().short_title : "Game") + " menu###nxrt_menu";
                st.overlay->add_panel(title, [] { draw_settings_page(g_host); });
                st.overlay->install_on_renderer();
                st.overlay->set_restore_mouse(false);   // the capture state machine decides when the mouse is taken again (play_capture.h)
                apply_fps_counter(st.settings.fps);
            }
        }
    }
    if (st.window && st.settings.display.monitor > 0) platform::move_to_display(st.settings.display.monitor);
    if (st.window && st.settings.display.window_mode != "windowed") set_window_mode(st.settings.display.window_mode);
    if (st.window && config_get_bool("play", "menu_bar", true)) install_menu_bar();

    const GamePlay* gp = game_play();
    {   // display options: the internal resolution comes from the window as it is now (after the window mode change above)
        int ww = 0, wh = 0;                    // no window (headless, test runs): the configured window size, not a made-up one
        if (st.window) platform::pixel_size(&ww, &wh);
        if (ww <= 0 || wh <= 0) { ww = st.settings.display.width; wh = st.settings.display.height; }
        display_tuning(st.settings.display);
        if (gp && gp->install_render) gp->install_render(st.base, st.settings.display, ww, wh);   // the game's display options, tone curve
    }
    // the frame boundary, then the game's input, camera, lock-on and menu hooks (hooks of one address run in registration order)
    if (gp && gp->frame_update) st.hook_update = hook_add_entry(st.base + gp->frame_update, &update_hook);
    else NXLOG_W(kTag, "the game (%s) names no frame update: play mode runs from its presents (present_frame)", game::info().id);
    if (gp && gp->install) gp->install(st.base, &g_game_host, st.settings);
    st.started = true;
    NXLOG_I(kTag, "play mode started (%s, overlay %s, base 0x%llx, game %s, frame hook %u)", st.window ? "window" : "no window", st.overlay ? "on" : "off",
            static_cast<unsigned long long>(st.base), game::info().id, st.hook_update);
    return true;
}

void present_frame() {
    G& st = g();
    if (!st.started || st.hook_update) return;
    static const bool on = config_get_bool("play", "frame_from_present", true);
    if (!on) return;
    static bool logged = false;
    if (!logged) {
        logged = true;
        NXLOG_I(kTag, "no frame-update hook: play mode runs once per present");
    }
    update_hook(nullptr, 0, nullptr);
}

void stop() {
    G& st = g();
    if (!st.started) return;
    vpad_shutdown();      // a virtual pad from a test script does not outlive play mode
    if (const GamePlay* gp = game_play(); gp && gp->remove) gp->remove();
    if (st.hook_update) hook_remove(st.hook_update);
    st.hook_update = 0;
    hid::set_vibration_sink(nullptr, nullptr);
    if (hid::input_source() == st.source) hid::set_input_source(nullptr);
    if (st.dirty) {
        std::string err;
        std::lock_guard<std::mutex> lk(st.mu);
        st.file.save(st.settings, &err);
        st.dirty = false;
    }
    st.limiter.set_pre_frame_hook(nullptr, nullptr);
    st.started = false;
}

Stats stats() {
    G& st = g();
    Stats s;
    s.frames = st.frames.load();
    s.process_input_calls = st.pi_calls.load();
    s.aim_frames = st.aim_frames.load();
    s.events = st.events.load();
    s.mouse_counts_applied = st.counts.load();
    s.pump_thread_mismatch = st.thread_mismatch.load();
    return s;
}

}  // namespace nxrt::play
