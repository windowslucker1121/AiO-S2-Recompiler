// SPDX-License-Identifier: GPL-3.0-or-later
// Dear ImGui overlay on the Vulkan path (S4b): the settings / developer menu of the PC port. Hidden by default; a key
// (F1, [gfx] overlay_key = any SDL scancode name) toggles it. ImGui is fed from the nxrt input queue (input.h), not from an
// ImGui platform backend, so it works the same with a window and headless. While it is visible and wants the mouse or the
// keyboard, handle_event() returns true and the game must not see the event. Showing the overlay releases the relative mouse
// mode and hiding it restores it.
// Changes made in the menu that touch the swapchain or the window (present mode, fullscreen) are applied by apply_pending(),
// which the frame loop calls before Renderer::begin_frame().
// Gamepads: a chord ([gfx] overlay_pad_chord, default "back+start": View + Menu on an Xbox pad) toggles the overlay too; while it is visible the
// pads navigate it (d-pad or left stick move, the bottom face button activates, the right one goes back, Start closes the menu) and their
// events do not reach anything else.
#pragma once
#include <functional>
#include <memory>
#include <string>

#include "nxrt/frame_limiter.h"
#include "nxrt/gfx.h"
#include "nxrt/input.h"

namespace nxrt::gfx {

// Background shader work (S24). The threads that compile pipeline parts publish how many jobs are waiting or running; while there are some
// the overlay shows a small note ("Preparing shaders ..."). Any thread. [nvn] compile_note = false keeps the note off.
void set_shader_backlog(int pending);
int shader_backlog();

class Overlay {
public:
    struct Context {
        Device* device = nullptr;
        Renderer* renderer = nullptr;
        FrameLimiter* limiter = nullptr;     // optional: shows and sets the frame cap
        bool has_window = true;              // false: no window controls (headless)
        bool diagnostics_window = true;      // the stand-alone "Diagnostics" window; false when a panel shows draw_debug_page itself (play mode)
    };
    static std::unique_ptr<Overlay> create(const Context& ctx, std::string* error = nullptr);
    ~Overlay();
    Overlay(const Overlay&) = delete;
    Overlay& operator=(const Overlay&) = delete;

    // Feed every input event here (before the game sees it). Returns true when the overlay consumed it.
    bool handle_event(const input::Event& e);

    void set_visible(bool v);
    bool visible() const;
    void toggle();
    int toggle_scancode() const;             // SDL scancode of the toggle key
    void set_toggle_scancode(int sc);
    uint32_t pad_chord() const;              // the gamepad chord, as bits of SDL_GamepadButton numbers (0: none)
    bool wants_input() const;                // visible and ImGui wants the mouse or keyboard
    bool wants_mouse() const;                // visible and the pointer is over the menu or a widget is being dragged
    bool wants_keyboard() const;             // visible and a text field or navigation has the keyboard
    // (both reflect the last drawn frame: ImGui decides hover while it builds the frame)

    // Extra panels for later streams (settings, dev menu): fn runs inside an ImGui window titled `title` while visible.
    void add_panel(const std::string& title, std::function<void()> fn);

    // Applies menu choices that cannot be made in the middle of a frame. Call before Renderer::begin_frame().
    void apply_pending();
    // The same in two halves for a multi-threaded caller: the present mode must change on the thread that presents (before its begin_frame), the
    // window (fullscreen) on the thread that owns the SDL window.
    void apply_present_pending();
    void apply_window_pending();

    // Corner frame-rate counter, off by default. Safe to call from any thread; it takes effect on the next presented frame.
    struct CounterConfig {
        bool show = false;
        int corner = 0;                  // 0 top left, 1 top right, 2 bottom left, 3 bottom right
        int detail = 1;                  // 0 frame rate; 1 also the frame time; 2 also the 1 % low and the worst frame
        bool in_screenshots = false;     // leave it in screenshots too (by default it is not drawn into them)
    };
    void set_counter(const CounterConfig& c);

    // A drawn arrow at a window pixel (test aid, S28: a screenshot of the game's own frame does not contain the OS cursor). Any thread.
    void set_pointer(bool show, float x, float y, bool in_screenshots);

    // The Renderer overlay hook: builds and draws the menu on top of the frame (no-op while hidden, unless the counter is on).
    void draw(const FrameInfo& f);
    void install_on_renderer();              // renderer->set_overlay_hook(draw)

    uint64_t frames_drawn() const;

    // The visible -> hidden edges so far (play mode holds back the pad buttons that are still down when the menu closes).
    uint64_t hide_count() const;

private:
    Overlay() = default;
    struct Impl;
    Impl* impl_ = nullptr;
};

// The Debug page (overlay_debug.cpp): performance and the frame-time graph, hitches, GPU and video memory, shaders and pipelines, the NVN back end,
// the log tail and every gauge. Draws into the current ImGui window, on the presenting thread. Device, renderer and limiter may be null (headless
// tests): their parts are left out.
void draw_debug_page(const Overlay::Context& ctx);

// "back+start" -> SDL_GamepadButton bits; "none" or "" -> 0. Names: south east west north back guide start lstick rstick lb rb dpad_up
// dpad_down dpad_left dpad_right (a b x y are south east west north). False for an unknown name.
bool parse_pad_chord(const std::string& text, uint32_t* bits);

}  // namespace nxrt::gfx
