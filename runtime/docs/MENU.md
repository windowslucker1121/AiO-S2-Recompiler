# The game window's menu

The PC port's settings live in a **native Windows menu bar** on the game window (Game, Input, Graphics, Audio, Debug, Help) and the native
dialogs it opens. Game independent (`runtime/src/gfx/play`); a game module only says which of its own options exist (`play_game.h`
`GamePlay`). The ImGui window drawn over the game (F1) is kept only as the menu for fullscreen, where the bar is not shown.

## The menu bar

| Menu | Entries |
|---|---|
| Game | Save settings now, Reload settings from the file, Open the settings file, Show the settings folder, Capture a bug report (F9), Exit |
| Input | Gamepad..., Keyboard and mouse bindings..., Rumble, Test rumble, Gyro, Face buttons (by label / by position), Button prompts |
| Graphics | Window mode, Window size, Keep the 16:9 picture, Vsync, Frame limit, Console mode (docked / handheld, next start; "tell the game now", experimental), Texture anisotropy, Replace the game's upscaler (the game stopgap, live), Upscaler (off / FSR 1 / NIS), Colour (neutral / vivid), More graphics settings... |
| Audio | Volume, Mute when the window is not in focus |
| Debug | Debug info..., Log..., Open the log file, Show the log folder, Copy a debug report to the clipboard, Frame rate counter (F2) and its corner, In-game debug overlay, Diagnostics switches (next start), Hitch threshold (next start) |
| Help | Keys and controls, About |

Check marks show the settings in force; they are read when a menu opens. A change applies at once and is saved in the play settings file
(`controls.ini` in the game folder, or the instance folder of a named run; `[play] controls` names another). Entries marked
**(next start)** are runtime configuration keys: they go to the `[config]` section of that file as `section.key = value` and become config
keys at the next start, unless the game ini or an `NXRT_*` variable sets them (a game module's own default gives way). A value back at its
default removes the line.

The bar is on the window in windowed mode and taken off in fullscreen (Alt+Enter switches; Graphics > Window mode too). The client area keeps
its size when the bar comes and goes (SDL counts the bar when it sizes the window), so the swapchain and the mouse coordinates are those of
the picture. Alt alone and F10 do not open the bar (SDL swallows `SC_KEYMENU`, and so does the menu's window subclass): they stay game keys.
Alt + the underlined letter of a top-level menu (Alt+G, Alt+I, Alt+R, Alt+A, Alt+D, Alt+H) opens it, as in any Windows program.

## The dialogs

Modeless windows on a thread of their own (`menu-ui`), with the system's visual styles and the game window's DPI. **The game keeps running
while a dialog is open.** They change settings through the same host as everything else (`UiHost::apply`), and re-read the settings when
they come to the front.

- **Gamepad**: the connected controllers and what is pressed on each, Test rumble, face-button layout, dead zones, trigger threshold, rumble
  strength, gyro scale, rumble and gyro on/off, and the button mapping: select a Switch button, Rebind, press a button on the controller (or
  pull a trigger); the button that had the job takes over the old job of the pressed one (a swap). Built-in layout resets all of them.
  Sliders apply when they are let go.
- **Keyboard and mouse bindings**: the bindings of a context (a game that does not report play or menu, like the game for now, uses the `menu`
  bindings plus the `all` layer), Rebind, Remove, Add a key for an action, Factory bindings. Rebind and Add bring the game window to the front:
  press the key, mouse button or wheel there (Escape cancels); the key that opens the in-game menu and the frame-rate hotkey are refused.
- **More graphics settings**: texture LOD bias, FSR 1 / NIS sharpness and the colour controls (live; the colour shows while a slider is
  dragged), output resolution, graphics scaling level and texture compression (next start), the GPU paths (next start), window size.
- **Debug info**: performance (fps, 1% low, worst frame), hitches, GPU and video memory, shader preparation and pipeline counters, NVN
  counters and the unsupported operations; refreshed every second (`gfx::debug_report_text`, the same text as Debug > Copy a debug report).
- **Log**: the last 512 log lines kept in memory, filtered by level, following the end.

## Threads, and what waits

Menu commands arrive on the thread that pumps the game window: the game's main thread (a window subclass, `SetWindowSubclass`, takes
`WM_COMMAND`, `WM_INITMENUPOPUP` and the menu-loop messages). **While a drop-down is open Windows runs its modal menu loop on that thread, so
the game's frame waits** until the drop-down closes (measured: a drop-down held 12 s gave one 12.3 s frame). Audio keeps playing (no
underruns), and a timer in the menu loop feeds the freeze watchdog (`diag_progress`), so no freeze report is written. Moving the window's
message pump off the game thread would avoid the wait; it is not done. Everything that stays open (the dialogs) runs on its own thread.

## The in-game menu (fullscreen)

The ImGui window over the game (`play_ui.cpp`, Input / Graphics / Audio / Debug / About) opens with F1 or View + Menu on a controller
(`[gfx] overlay_pad_chord`) only where the bar is not shown: `[gfx] overlay_menu = fullscreen` (default), `always` or `never`. With the bar on
the window, F1 and the chord pass through to the game. Keyboard and gamepad navigate it; Start closes it; while it is open the game gets no
input, and pad buttons still held when it closes stay held back until released. Debug > In-game debug overlay shows the Debug page alone over
the game (display only), also in fullscreen.

## Code

| Part | File |
|---|---|
| The menu as data: labels, check marks, what each entry changes | `src/gfx/play/menu_model.cpp`, `menu_model.h` |
| The bar, the subclass, the dialogs and their thread | `src/gfx/play/win_menu.cpp`, `win_menu.h` (Windows only; elsewhere the functions do nothing) |
| Play mode's hooks (save, reload, exit, bug capture, debug report), the bar per frame, the in-game menu's switch | `src/gfx/play/play.cpp` (`install_menu_bar`) |
| The in-game menu and the `UiHost` contract | `src/gfx/play/play_ui.cpp`, `play_ui.h` |
| The overlay: toggle key and chord, navigation, the debug window | `src/gfx/overlay.cpp`, `overlay_debug.cpp`, `include/nxrt/overlay.h` |
| Settings file (`[menu]`, `[config]`, `[display] upscaler_replace`) | `src/gfx/play/play_config.cpp`, `play_types.h` |
| Pad helpers (press-to-rebind swap, pressed sources) | `src/gfx/play/pad_map.cpp` |
| Data sources | `log_tail` (`diag/log.cpp`), `frame_time_stats` and `hitch_recent` (`diag/gauge.cpp`), `nvn::shader_stats` (`nvn_gpl.cpp`), `nvn::set_upscaler_replace_enabled` (`nvn_replay.cpp`), `config_set_default` / `config_user_set` (`diag/config.cpp`) |

Keys: `[play] menu_bar` (default `1`; `0`: no bar), `[gfx] overlay_menu`, `[gfx] overlay_key`, `[gfx] overlay_pad_chord`, `[menu] auto_apply`
(the in-game menu's Apply mode), `[config]`, `[boot] screenshot_dir` (docs/CONFIG.md).

Tests: `play_ui` (the menu model: unique ids, every check and radio entry changes the settings and shows its state, `[config]` entries stored
and removed; the in-game menu's pages, press-to-rebind, auto-apply), `gfx_overlay` (pad chord, navigation), `play_config`, `play_pad`,
`log_config`, `soak`.

Checking the native UI in a running game without touching the mouse: `work/run/nm_driver.py` style ctypes calls (git-ignored helper of the
menu branch): `GetMenu` and `GetMenuStringW` list the bar, `PostMessage(WM_COMMAND, id)` runs an entry, `WM_SYSCOMMAND SC_KEYMENU 'g'` opens a
drop-down, `PrintWindow(PW_RENDERFULLCONTENT)` captures the window or a dialog. Boot screenshots and `@shot` contain only the Vulkan frame, not
the bar or the dialogs.
