// SPDX-License-Identifier: GPL-3.0-or-later
// Platform layer (S4b): SDL3 window, fullscreen, relative (raw) mouse, gamepad hot-plug, Vulkan surface creation.
// Everything here is called from one thread (the one that created the window, normally the main thread). Events are not
// returned: pump_events() turns them into nxrt::input events (input.h).
// Configuration (config.h): [gfx] hidden / NXRT_GFX_HIDDEN=1 creates the window hidden (automated tests).
#pragma once
#include <cstdint>

namespace nxrt::platform {

struct InitOptions {
    bool gamepads = true;       // init the joystick/gamepad subsystems and open pads as they are plugged in
};
bool init(const InitOptions& opt = {});   // idempotent; sets the SDL hints (raw mouse), SDL_Init(VIDEO|EVENTS|JOYSTICK|GAMEPAD)
void shutdown();                           // closes the window and every opened gamepad, SDL_Quit
bool initialised();

struct WindowDesc {
    const char* title = "nxrt";
    int width = 1280, height = 720;     // client size in screen coordinates at creation
    bool resizable = true;
    bool hidden = false;                // also forced by [gfx] hidden / NXRT_GFX_HIDDEN
};
bool create_window(const WindowDesc& desc);   // Vulkan-capable window (SDL_WINDOW_VULKAN, high pixel density)
void destroy_window();
bool has_window();
void* native_window();                        // SDL_Window*
void* native_hwnd();                          // the Win32 HWND of the window (null when there is none)
void set_window_title(const char* title);
void pixel_size(int* w, int* h);              // client size in pixels (0, 0 while minimised)
bool set_window_size(int w, int h);           // windowed size in screen coordinates
bool minimized();
bool focused();
bool visible();

enum class FullscreenMode { Windowed, Borderless, Exclusive };
// Borderless: desktop-resolution borderless window. Exclusive: SDL fullscreen with a real display mode closest to w x h at hz
// (zeros: the desktop mode). Returns false when the change failed (the mode is then unchanged). Synchronous: waits until SDL
// reports the new state or ~2 s.
bool set_fullscreen(FullscreenMode mode, int w = 0, int h = 0, float hz = 0.f);
FullscreenMode fullscreen_mode();
float display_refresh_hz();                   // refresh rate of the display the window is on (desktop mode), 0 if unknown

// Displays (monitors), numbered 1.. in the system's order. display_name is "Name (1920x1080 @ 144 Hz)"-style text for menus, empty for a bad number.
int display_count();
bool display_name(int number, char* out, int cap);
int window_display();                         // the number of the display the window is on, 0 if unknown
// Moves the window to the centre of that display, keeping its mode (a fullscreen window leaves fullscreen, moves and goes back).
bool move_to_display(int number);

// Relative mouse mode: raw, unaccelerated deltas (SDL_SetWindowRelativeMouseMode with SDL_HINT_MOUSE_RELATIVE_SYSTEM_SCALE
// forced to "0" and SDL_HINT_MOUSE_RELATIVE_SPEED_SCALE to "1"). The cursor is hidden and confined while it is on.
bool set_relative_mouse(bool on);
bool relative_mouse();
void set_cursor_visible(bool on);             // the OS cursor over the window (not in relative mode, which hides it); no-op without a window
bool relative_mouse_is_raw();                 // true when the system-scale hint is off (the unaccelerated setup)

// Polled gamepad state (play mode): what SDL's gamepad layer reports for one open pad. SDL normalises every controller to the Xbox
// layout by position (SOUTH is the bottom face button), whatever is printed on it.
struct GamepadRaw {
    uint32_t buttons = 0;               // bit n set = SDL_GamepadButton n is down (SOUTH 0, EAST 1, WEST 2, NORTH 3, BACK 4, GUIDE 5, START 6,
                                        // LEFT_STICK 7, RIGHT_STICK 8, LEFT_SHOULDER 9, RIGHT_SHOULDER 10, DPAD_UP 11, DOWN 12, LEFT 13, RIGHT 14, MISC1 15)
    int16_t axes[6] = {};               // SDL_GamepadAxis order: LEFTX LEFTY RIGHTX RIGHTY (-32768..32767, y down = +), LEFT_TRIGGER RIGHT_TRIGGER (0..32767)
    uint8_t face_label[4] = {0, 1, 2, 3};   // what is printed on SOUTH, EAST, WEST, NORTH: 0 = A, 1 = B, 2 = X, 3 = Y
                                        // (PlayStation cross, circle, square, triangle count as A, B, X, Y; a console-layout pad prints B, A, Y, X)
};
bool gamepad_poll(uint32_t id, GamepadRaw* out);                       // false when no such pad is open
bool gamepad_sensors(uint32_t id, float accel[3], float gyro[3]);      // latest accelerometer (m/s^2) and gyro (rad/s) in SDL's axes; false without both sensors
bool gamepad_rumble(uint32_t id, uint16_t low, uint16_t high, uint32_t duration_ms);   // 0..65535 per motor; (0, 0) stops it; false when unsupported

// Pumps the SDL event queue and translates it into input events and window state. wait: block for up to timeout_ms (-1 =
// forever) for the first event (used while minimised). Returns the number of SDL events handled.
int pump_events(bool wait = false, int timeout_ms = -1);
bool quit_requested();
void clear_quit();
void request_quit();

uint64_t ticks_ns();                          // SDL_GetTicksNS

// Vulkan: the instance extensions SDL needs (VK_KHR_surface + the platform one) and surface creation/destruction. The Vulkan
// handles are passed as integers so that this header needs no Vulkan include.
const char* const* vulkan_instance_extensions(uint32_t* count);
bool create_vulkan_surface(void* vk_instance, uint64_t* surface_out);
void destroy_vulkan_surface(void* vk_instance, uint64_t surface);

}  // namespace nxrt::platform
