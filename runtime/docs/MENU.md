# The F1 menu

One window over the game with everything the PC port lets the player change, plus a debug view. Game independent (`runtime/src`); a game
module only says which of its own options exist (`play_game.h` `GamePlay`).

## Opening, closing, moving around

| Input | What it does |
|---|---|
| F1 (`[gfx] overlay_key`) | Opens and closes the menu. The key never reaches the game. |
| View + Menu held together on one pad (`[gfx] overlay_pad_chord`, default `back+start`) | Opens and closes the menu. |
| Start alone (menu open) | Closes the menu. |
| Mouse | Click, drag sliders, wheel scrolls. |
| Keyboard | Arrow keys move between controls, Space or Enter activates, Escape backs out of a combo or a field. |
| Gamepad | D-pad or left stick moves, the bottom face button (A / Cross) activates, the right one (B / Circle) backs out. |

While the menu is open the game gets no input at all: keys, mouse buttons and pads are held back. Pad buttons that are still held when the
menu closes (the chord, a face button) stay held back until they are released, so the game does not get the tail of a press meant for the
menu. The window opens centred and takes the keyboard and gamepad focus at once.

## Saving

Everything is stored in the play settings file, `controls.ini` in the game folder (`[play] controls` / `NXRT_PLAY_CONTROLS` names another
file). With **Apply changes at once** on (`[menu] auto_apply`, the default) a change takes effect and the file is written as soon as the
control is let go (a slider when it is released, a text field when it is left). Off: changes wait for **Apply and save**; **Revert** drops
them. **Factory defaults...** asks first, then loads the factory settings. While nothing waits in the menu it follows the settings in force,
so a change made elsewhere (Alt+Enter, the frame-rate hotkey F2) is never undone by the menu.

Items marked **restart** are read once at start. Those that are runtime configuration keys (`docs/CONFIG.md`) go to the `[config]` section
of `controls.ini` as `section.key = value`; at the next start each becomes that key unless the game ini or an `NXRT_*` variable sets it (then
the menu says so in orange and its choice does not count). A game module's own defaults (`game::Info::config_defaults`) give way to
`[config]`. A row whose value is the default is removed from the file. When the value in force differs from the menu's choice the row says
"now: ...; the next start uses the choice above".

## Pages

**Input**
- *Button prompts* (top): the icon set of the game's prompts; Auto follows the device used last.
- *Gamepad*: every connected pad with its type, gyro and rumble, sticks and what is pressed on it right now (before the mapping);
  **Test rumble** (half a second on every pad, at the strength set below). Layout (buttons by printed label or by position), dead zones,
  trigger threshold, rumble on/off and strength, gyro on/off and scale. **Button mapping**: one row per Switch button with the controller
  buttons that press it; **Rebind** waits up to 8 s for a press on any pad (or a trigger pulled past the threshold) and makes that button
  press this Switch button; the button that had the job before takes over the old job of the pressed one (a swap). The per-controller-button
  table (combinations, "nothing") is under *Per controller button*. Look inversion and look sensitivity only act while the game reports that
  it is in play (`GamePlay::frame`); they are hidden for a game that does not (OoT for now).
- *Keyboard and mouse*: bindings (click an input and press a key, mouse button or wheel notch; Escape cancels), per context and trigger.
  A game that does not report play or menu (OoT for now) always uses the `menu` bindings plus the `all` layer; the page says so and opens on
  `all`. The mouse-aim settings appear only for a game with aim hooks (`GamePlay::install`), the menu pointer only with menu hooks.
- *Switch layout*: what each action presses on the Switch pad.
- *Live view*: the Switch pad output of keyboard and mouse, and each pad's raw state.

**Graphics**
- *Display*: window mode, monitor, window size (live); vsync and frame limit (live); **Console mode and resolution** (restart, `[config]`):
  docked or handheld (`[am] operation_mode`), the output resolution the game is told (`[am] resolution`) and the graphics scaling level
  (`[am] graphics_scale`). "Switch to ... now (experimental)" sends the console's operation-mode change to the running game; whether the game
  changes its picture is up to it. Render scale only for games whose render size can be changed (`kDisplayOptRenderSize`).
- *Picture*: anisotropy and texture LOD bias (live, every game); the game's own options where its hooks honour them (aspect, field of view,
  dynamic resolution, bloom, chromatic aberration, quality preset); **Replace the game's own upscaler (stopgap)** (live, shown when the game
  module names a replacement, `[display] upscaler_replace`; OoT: off runs the game's own compute chain and the 3D scene is black, A12); FSR 1
  and NIS in the output pass (every game); colour (contrast, saturation, black level, gamma; live). HDR output, the scene-resolution slider and
  the temporal upscalers need the HDR scene pipeline matched to the game's tone map (`kDisplayOptHdr`); OoT does not show them.
- *Advanced*: texture compression quality, GPU memory mirror, pass barriers, pipeline prewarm, low latency (restart).

**Audio**: master volume, mute while unfocused (live), output device (restart).

**Debug** (game independent, `src/gfx/overlay_debug.cpp`)
- Frame rate counter (corner overlay, hotkey F2).
- *Performance*: fps, 1% low, median and worst frame of the last 5 s and a frame-time graph of the last 300 presented frames with the 60 and
  30 fps lines; GPU time of the output pass, swapchain, frame limiter.
- *Hitches*: slow frames since start (`[diag] hitch_ms`, default 50 ms), the last 30 s (frames over twice the median), and the hitch log's
  lines (the worst slow frame of each second with what happened during it).
- *GPU and video memory*: GPU, driver, validation layer, video memory in use against the driver's budget, process memory.
- *Shaders and pipelines*: what the start found of the shader preparation (`prepare.state`), parts built from it and new ones with their
  times, pipelines linked, built at the draw, draws deferred, background jobs, the "Preparing shaders" backlog.
- *NVN back end*: draws drawn and skipped, dispatches, submissions, textures and uploads, unsupported operations, unresolved addresses,
  missing handles, entry points and stub calls (most called), the unsupported-operation report.
- *Log*: the last 512 lines of the log in memory, filtered by level and text, with Follow and Copy.
- *Gauges*: every registered gauge (live Vulkan and NVN objects, caches, queues), filterable.
- *Diagnostics switches (next start)*, `[config]`: GPU checkpoints (`gfx.device_diagnostics`), the driver fault report
  (`gfx.device_fault`), ending after a device loss (`gfx.exit_on_device_lost`), robust access (`gfx.robust_access`; the prepared pipeline
  cache then no longer matches), skipping dispatches with unmapped buffers (`nvn.skip_unmapped_dispatch`) or every dispatch
  (`nvn.debug_skip_compute`), the frame-rate log line (`gfx.log_fps`), the hitch threshold (`diag.hitch_ms`).
  Reads that may take locks other threads hold (gauges, shader statistics, the unsupported report) run on a small sampler thread once a
  second while the page is on screen; the presenting thread only copies the result.

**About**: version, build, folders and files in use. A game's own page (**Game**) appears when its module has one.

## Code

| Part | File |
|---|---|
| The overlay: toggle key and pad chord, gamepad and keyboard navigation, window placement | `src/gfx/overlay.cpp`, `include/nxrt/overlay.h` |
| The Debug page | `src/gfx/overlay_debug.cpp` (`draw_debug_page`) |
| The menu pages and the `[config]` rows | `src/gfx/play/play_ui.cpp`, `play_ui.h` (`UiHost`) |
| The host: settings in force, key and pad capture, pad list, rumble test, `[config]` applied at start, held-back pad buttons | `src/gfx/play/play.cpp` |
| The settings file (`[menu]`, `[config]`, `[display] upscaler_replace`) | `src/gfx/play/play_config.cpp`, `play_types.h` |
| Pad helpers (press-to-rebind swap, pressed sources) | `src/gfx/play/pad_map.cpp` |
| Data sources | `log_tail` (`diag/log.cpp`), `frame_time_stats` and `hitch_recent` (`diag/gauge.cpp`), `nvn::shader_stats` (`nvn_gpl.cpp`), `nvn::set_upscaler_replace_enabled` (`nvn_replay.cpp`), `config_set_default` / `config_user_set` (`diag/config.cpp`) |

Tests: `play_ui` (every page and tab drawn headless; press-to-rebind with the swap and auto-apply; auto-apply off; a `[config]` switch stored
and removed; changes made elsewhere kept), `gfx_overlay` (pad chord, navigation, Start closes, the menu without the stand-alone Diagnostics
window), `play_config` (`[menu]`, `[config]`, `upscaler_replace`), `play_pad` (rebind helpers), `log_config` (log tail,
`config_set_default`), `soak` (frame-time statistics).

Checking a change in the running game: the menu is in the boot screenshots (`[boot] screenshot_at`, `screenshot_every`, `screenshot_dir`)
and in the test tooling's `@shot`; the virtual pad of the test tooling (`keydown vpad attach xbox`, `keydown vpad button back 1` ...) drives
the chord and the navigation. Window messages posted for mouse clicks do not land where they are aimed (SDL reads the real cursor), so drive
the menu with keys or the virtual pad.

## Limits

- The pad mapping is one mapping for all pads. The rebind table assumes the printed labels A B X Y at the bottom, right, left and top (Xbox,
  PlayStation); on a Nintendo-layout pad with "buttons by label" the face rows show the positions' Xbox names.
- The live docked/handheld switch only sends the console's notice; OoT's reaction to it is not verified. The setting for the next start is
  the reliable one.
- `[am] resolution` and `[am] graphics_scale` are answers the game may use or ignore; their effect on OoT is not measured yet.
