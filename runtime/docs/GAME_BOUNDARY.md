# The game boundary

The runtime, the recompiler and the offline tools know no game. A game module brings what one title supplies: its identity, file names,
title facts, frame-step sites and play-mode hooks. The only modules in the repository are `runtime/games/oot` (the game port)
and `runtime/games/generic` (the runtime for experimental builds of any dump: identity from the manifest at start, no hooks).
This file describes the boundary. The ports the code was first written for (title E, title G, title B, title A) are removed; some code and tests still carry their vocabulary (below).

Three kinds of game knowledge:

- (a) A generic thing with one game's value baked in. The code works for any game; only a number or a name was the game's.
  The value now comes from the game (a field of `nxrt::game::Info`, or a per-game data file in the tools).
- (b) A reusable mechanism with one game's data. The code is a general technique (patching the frame step at a list of sites,
  matching button icons in the game's textures, factory settings with revisions); the table it works from was the game's. The
  mechanism stays in the base and the table moved to the game module.
- (c) Purely game-specific. Hooks on a game's functions, its camera and aim behaviour, its menus. They live in the game
  module.

Scope: `runtime/` (base and the library app `apps/setup`), `tools/` and `recomp/` outside `recomp/games/<id>`, which is the recompiler's
game folder. The game exe (`game-build/`) is the game's own program and stays there.

## The interface

`include/nxrt/game.h` holds one struct, `nxrt::game::Info`, and a registry with one slot: `set_current(const Info*)`, `current()`
and `info()` (an empty Info when nothing is registered, so the base runs without a game and the hooks that need one are skipped with
a log line). It is header-only, so the library can use it without linking the runtime.

`Info` has:

| Field | Kind | What the base does with it |
| --- | --- | --- |
| `id`, `title`, `product`, `window_title`, `short_title`, `program_id`, `version` | identity | logs, window titles (`window_title`: "<short title> (unofficial recompilation)"), the controls.ini header, the library's branding and ini comment, the title profile. `product` is a neutral build name ("Recomp module: oot"): exe properties and shortcut descriptions never carry a game's name |
| `main_base` | (a) | the default guest base of play mode, timestep and test tooling (`[play]`, `[time]`, `[test] game_base` still override) |
| `exe_name`, `setup_exe_name`, `launcher_ini` (unused now), `game_ini`, `shader_pack`, `save_folder` | file names | the library finds and starts the programs; the game exe reads `game_ini`; services put saves under `%APPDATA%\<save_folder>` (an `old_save_folders` name found instead is moved there at the first start) |
| `title_defaults(TitleProfile&)` | (a) | the services' title profile (program id, name, version, languages, save sizes, main thread, nickname, user id) |
| `timestep` (`timestep::Sites`) | (b) | the frame-step patch: frame update entry, mid-function sites with their load kind, render timing counter and its wrap, movie update, per-tick sampler |
| `probes` (`tt::GameProbes`) | (c) behind a generic API | test tooling (oot has none): state readout, warp, grant, item, guest calls, object dump, ray casts, watches |
| `play_defaults` (`play::GameDefaults`) | (b) | factory `controls.ini`, the defaults revision and the bindings added by each revision |
| `play` (`play::GamePlay`) | (c) behind a generic API | play mode's game half: input and camera hooks, display hooks, menus, status, the F1 page's Tweaks tab |
| `prompts` (`play::PromptGame`) | (b) | button prompt icons (oot has none): the table of icons, how to scan the game's textures, icon size, orientation |

A game module registers its Info before `nxrt::init()`. The oot module is `runtime/games/oot` and comes as three libraries, each
a superset of the one before, so each program links only what it needs (`generic` has the same three):

| Library | Adds | Registered by | Linked by |
| --- | --- | --- | --- |
| `nxrt_game_oot_data` | identity, names, factory settings (pure data) | `nxrt_register_game_oot_data()` | the library and its tests |
| `nxrt_game_oot_core` | title profile, timestep sites (links nxrt) | `nxrt_register_game_oot_core()` | `nxrt_tests` |
| `nxrt_game_oot` | play-mode hooks, resolution pin (links nxrt_gfx) | `nxrt_register_game_oot()` | `oot.exe`, `nxrt_gfx_tests` |

The play-mode half talks to the base through two tables in `src/gfx/play/play_game.h`. `GamePlay` is what the game gives the base
(function pointers: `install_render`, `install`, `remove`, `settings_changed`, `frame_begin`, `frame`, `display_live`, `menu_*`,
`status`, `status_text`, `mouse_tab`, `tweaks_tab`, plus the guest address of the frame update hook). `GameHost` is what the base gives the game
(a virtual class implemented in `play.cpp`: current settings, action state, the right stick, pad look, the per-input-tick mouse
handling, aim feedback, pitch limit, log status). Hooks keep their registration order: the base installs the game's render hooks
before its own frame hook and the game's input hooks after it, as before.

CMake: `NXRT_BUILD_GAMES` (on) adds every module listed in `NXRT_GAMES` (default `oot`), so a second game needs no edit of the base. `NXRT_TEST_GAME` (default `oot`) picks the module the test programs register.
A module's own tests run in the base test programs: `games/<id>/tests/core/*.cpp` in `nxrt_tests`, `games/<id>/tests/*.cpp` in `nxrt_gfx_tests`; without a test game they are left out.
The library app reads the game manifests (`recomp/games/<id>/game.toml`) at run time, so it needs no code per game.

## Where game knowledge lives now

| Place | Kind | Source |
| --- | --- | --- |
| Services' title profile (program id, name, version, languages, save sizes, main thread) | (a) | `title_defaults` (`games/oot/oot_core.cpp`); the base defaults are neutral (0, "", "Player", "nxrtuser"); the main thread (stack 0x100000, priority 44, core 0) is also the SDK default |
| Save folder `%APPDATA%\<save_folder>` | (a) | `Info::save_folder` (fallback "nxrt") |
| Frame step | (b) | mechanism in `src/timestep/timestep.cpp`, which walks a `Sites` table; oot's table is in `games/oot/oot_core.cpp` (the same addresses as `recomp/games/oot/main.sites`) |
| Play mode hooks (input, camera, display, menus, status) | (c) | `GamePlay` (`src/gfx/play/play_game.h`); oot: `games/oot/oot_game.cpp`, `oot_drs.cpp` |
| Factory `controls.ini`, defaults revision, bindings added by revisions | (b) | revision mechanism in `play_config.cpp`; the data is `GameDefaults` (oot: `oot_game.cpp`) |
| Button prompt icons | (b) | matching and drawing in the base; the table and scan come from `PromptGame` |
| Test tooling probes | (c) | `tt::GameProbes`; `src/testtools/tt_game.cpp` keeps the base address, memory access, `@peek`/`@poke` and forwarders |
| Window title, dialog captions, exe, ini and shader pack names | (a) | `Info` (from the manifest) |
| Game exe | | `game-build/src/game_main.cpp` registers the module first and takes window and message box titles from `info().title` |
| The library app (`runtime/apps/setup`) | (a) | reads `games/<id>/game.toml` at run time |

The play-mode half talks to the base through two tables in `src/gfx/play/play_game.h`: `GamePlay` is what the game gives the base,
`GameHost` is what the base gives the game (see above).

The recompiler (`nxrecomp/`) has no game knowledge. `0x7100000000` there is the console's load address for main, a platform fact.
`scripts/build_game.cmd <id>` builds `games/<id>` (target `<id>_modules`, repository root passed as `NX_ROOT`).

## Gaps

These are still shaped by the first game the base was written for (title E, removed). None of them blocks a new
game from building; they limit what it can reuse.

- Play-mode vocabulary. The base's action and context slots are generic: `Ctx` is All, Play, Alt (the alternate body), Map, Menu,
  Inspect; the game actions are Primary, Target, Secondary, Jump, AltForm, LookHold, Map, Pause, Confirm, Back, the selects SetA1..4 /
  SetB1..4 and the cycle SetANext / SetAPrev; `GameFrame` carries `cycle_index` / `cycle_mask`. A game names them through
  `play::GameVocabulary` (in `GameDefaults`). Without a game the slots keep generic names and the game actions have no factory output.
  The test fixture `runtime/tests/play_fixture` rebuilds the old title E vocabulary for the play_config and play_engine groups. Fixed in
  the base: the number and kinds of slots (four-member sets A and B, one alternate body, one inspect context) and the `[aim]` (lock-on)
  and `[camera]` sections.
- `[tweaks]` names title E's features. `skip_intro`, `skip_boot_logos`, `fast_menus`, `glow_color` and the per-part glow keys are base
  settings code (`play_fastboot.h`, `play_tweaks.h`); no game module uses the glow keys now.
- Code that moved out of a game module keeps namespace `nxrt::play`.
- Test tooling vocabulary: `tt::GameState` fields (morph, beam, visor, missiles), the hash over them (`tt_core.cpp`) and the item names
  in `tt_script.cpp`.
- Shader program list magic `MPRPRG01` (`nvn_gpl.cpp`) is a file format name shared with the library; harmless, but named after the old
  game.

## Deterministic mode and the New Game stall

A deterministic test boot sat on the planet loading screen after New Game for as long as it ran (25+ minutes). The game boundary was not
the cause: a build from before the boundary work stalls the same way with the same input frames (a frame-pinned script, 16,700 frames at
about 50 fps: still loading), and the boundary build loads and reaches the hangar when deterministic mode is off.

The cause was the test tooling's virtual clock. In deterministic mode `kernel::system_tick` reads a virtual clock that should move 1/60 s per
frame. But every sleep of every guest thread also moved it forward by the sleep's length, and a sleep only yields, so a thread that sleeps
in a loop spun at host speed and pushed the shared clock forward: 150 to 2,000 virtual seconds for every 60 frames. (The profile of a stalled
run points at the game's pad vibration thread, `CNXJoyCon_NPad::StaticVibrationThread`, busy in sleeps and hid requests; which thread
contributed most was not measured.) Under that clock the New Game load never finished. The one earlier run that had
passed was on a busy machine (12 fps): the vibration thread got less CPU, the clock ran at 20 to 50 seconds per 60 frames early on, and the load got
through. The trace (`out/trace.jsonl`, field `t`) shows the rate in every run.

The fix (`src/testtools/tt_core.cpp`, `virtual_sleep`): only the thread that runs the frame boundary moves the clock with its sleeps. Any other
thread waits until the frames move the clock past its wake time, for at most its sleep's length of host time, and does not move it. The clock
now runs at 60 frames per virtual second (frame 600 at 110.0 s, frame 9,000 at 250.0 s), and a deterministic boot reaches the hangar at about
frame 11,400. Deterministic mode is still not bit-exact across runs: movies and loader threads run on host time, so the same frame number can
show different moments in two runs (docs/S5.md, known limits). Compare runs at a game-time point (`@run` after `@area`), not at a frame number.

## Adding a game

1. `runtime/games/<id>/` with a CMakeLists that builds `nxrt_game_<id>_data` (and `_core`, and the full library if it has play
   mode), an `Info` and `nxrt_register_game_<id>*()` functions (copy `runtime/games/oot`).
2. A game exe that calls the register function before `nxrt::init()` (`game-build` does this from the manifest).
3. `recomp/games/<id>/` with `game.toml`, `config.py`, `CMakeLists.txt` (a `<id>_modules` target) and the hook sites; build it with
   `scriptsuild_game.cmd <id>`.
4. Nothing in the library app: it reads `game.toml`. A title without a port can use the generic module (`recomp/games/generic`).


## Replacing a game's own upscaler / TAA with DLSS or FSR 3 (`[dlss_pass]`)

No code per game: the game module's `config_defaults` names the pass. Recipe for a title that has a compute upscaler or TAA of its own:
1. Capture a frame and trace it (`[nvn] trace_frame`, `cs bound tex/img` lines per dispatch) to find the pass's shader hash and what it samples:
   scene colour, depth, motion vectors (a texture the size of the render target), and the image it writes.
2. Set `match` (hash prefixes of the pass and of the chain's other passes), `color`, `depth`, `motion`, `output` as `texN` / `imgN` / `depth_rt`.
3. Name where the game keeps its jitter (`jitter = ubo:<slot>:<word>,<word>`), `mv_scale`, `reset` and `depth_reversed`.
4. Run with `[upscale] mode = dlss` (or `dlaa`, `fsr3`): the log says `dlss pass: ...` and `EvaluateDlss: ... by <backend>`. With the mode off or the DLLs missing the
   game's pass runs untouched. Without a motion binding (`motion = none`) nothing is replaced yet (camera-derived vectors are not built for this route).
Keys: `runtime/docs/CONFIG.md` (`dlss_pass`). The game's defaults are in `runtime/games/oot/oot_data.cpp` (motion not identified, so inert).
