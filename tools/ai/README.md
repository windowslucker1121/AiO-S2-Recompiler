# tools/ai: drive the game exe from Claude Code

An MCP server (`game`) that lets an AI agent start, watch, play and debug the game exe: process, window, screenshots, keyboard and mouse,
log, crash reports and the runtime's test tooling. Game independent: exe and log names come from `recomp/games/<id>/game.toml`.

| File | What |
|---|---|
| `gamectl.py` | The tools (methods of `Game`; the docstrings are what the agent reads). Reuses `tools/test/wincap.py` and `imgdiff.py`. |
| `mcp_server.py` | MCP over stdio (stdlib JSON-RPC, no SDK), and the same tools as a CLI. |
| `tests/test_gamectl.py`, `tests/fake_window.py` | The check, against a real stand-in window. |
| `../../.mcp.json` | Registers the server for Claude Code in this repo. |

## Enable

    tools\.venv\Scripts\python -m pip install -r tools\ai\requirements.txt      (Pillow, numpy)

Start `claude` in the repo root and approve the project server `game` when asked (or later in `/mcp`). The tools then appear as
`mcp__game__status`, `mcp__game__launch`, ... Settings, all optional, in the environment or the `env` block of `.mcp.json`:

| Variable | Default |
|---|---|
| `NXRT_AI_GAME` | `oot` (a folder under `recomp/games`) |
| `NXRT_AI_EXE` | `game-build/build/release/<exe from game.toml>` (`launch` also takes `exe=`) |
| `NXRT_AI_RUN_DIR` | `work/ai_run/<id>`: the log, stdout.txt/stderr.txt, script.nxs, testout/, shots/ of each launch |

CLI, same tools, one process per call: `tools\.venv\Scripts\python tools\ai\mcp_server.py help`, `... status`, `... log tail=20 grep=error`,
`... send_input steps='["key Return"]'`. A CLI call finds the game by the exe's file name.

## Tools

| Tool | What it does |
|---|---|
| `status` | One call: exe and build time, processes (uptime, CPU s, memory), exit code, window (size, focused, minimised, hung), log (seconds since last write, recent errors/warnings, last lines), reports present, scripted-run result and last trace lines. |
| `launch` | (Re)start the game (kills a running one): args, `settings` = runtime config keys as `NXRT_<SECTION>_<KEY>` (beat the ini), env, `headless`, `script` (test tooling, with `peek`/`poke` shorthands), `wait` for the exit and get the result. |
| `restart` | Kill and launch again with the last launch's arguments. |
| `kill` | End the game's process tree. |
| `screenshot` | Client area of the game window (PrintWindow: works covered and unfocused, not minimised); returns the picture and a full-size PNG path; `crop`. |
| `send_input` | Steps `key/down/up/text/click/rclick/mclick/move/wheel/rel/wait`; coordinates in pixels or fractions. `post` = window messages to the game window only (background); `send` = SendInput after bringing it to the front, refused if it is not in front. |
| `wait_for` | Until `window`, `visible` (not black), `change`, `image` (SSIM against a reference), `log` (regex), `exit`, with a timeout. |
| `compare` | imgdiff (SSIM, MAE, histogram, changed pixels, masks) of two PNGs or a reference against the window now; writes a diff picture. |
| `log` | Tail, regex over the whole log, or only new lines since the last call. |
| `reports` | `<exe stem>_crash.txt`, `_freeze.txt`, `_fatal.txt` next to the exe and the test output's `*.txt`, flagged new since the launch. |
| `states` | Saved states (`work/states/<game>/<name>`) and the game's built-in ones and routes (`recomp/games/<id>/ai_states.toml`), with whether the exe on disk is the build a state was saved with. |
| `save_state` | The game's committed save data of the running or last run (or of a bug bundle) as a named state, with the build, the launch's settings and env, the steps back (`reach`), a ready check and a screenshot. |
| `load_state` | Start the game at a state or a bug bundle: its save data into `<run dir>/save` (`[fs] save_dir`), the launch, then the reach steps and the ready check; the steps and their seconds, or the one that failed with the log tail. |
| `capture_bug` | A bug bundle of this moment (`work/bugs/<time>[-name]`): the running game writes it (request file), this side adds the window picture, the launch and stdout/stderr; with no game running it is made from the last run's files. |
| `bugs` | The bundles, newest first (note, origin, state, build), or one in full (bug.json, note, files, log tail, the screenshot). |

## States: start at a moment instead of booting every time

There is no process snapshot. A state is what the game itself can restore, its **save data**, plus the **steps that bring a fresh start
back to the moment**. Why not a snapshot: the guest's threads run on host threads whose stacks hold lifted C++ frames (return addresses into
the exe, host heap pointers), next to kernel and service objects in host memory, nn::os fibers on host fibers, and the GPU state in the
Vulkan driver (devices, pipelines, memory that cannot be written out and re-created). Restoring that is a whole-process checkpoint/restore
of a Windows process with a live GPU device; nothing in the runtime or Windows offers it, and guest memory alone (up to 12 GiB) is useless
without the host frames that hold the threads' state.

| Part | Where | What |
|---|---|---|
| `state.json` | `work/states/<game>/<name>/` | description, created, build (exe size, time, SHA-256; the commit from the exe's log line `built from commit`), settings, env and args of the launch it was saved from, `reach`, `ready` |
| `save/` | same | the committed save data (no journal work folders `<key>.work.<pid>`, journals or commit markers): the runtime keeps uncommitted writes apart until the game commits (`runtime/src/services/fs_volume.h`) |
| `shot.png` | same | the window when it was saved |
| routes, built-in states, defaults | `recomp/games/<id>/ai_states.toml` | the game knowledge: how to reach the title, load a save, wait for a scene; the shader cache and test switches every `load_state` uses |

`load_state` never touches the player's own save (`%APPDATA%\<save folder>\save`): it copies the state's `save/` into `<run dir>/save` and
launches with `[fs] save_dir` there. Settings and env merge, later wins: the game's `[defaults]`, the state's, the call's. A reach step is
a route name or a dict with one of `route`, `wait_for` (the tool's arguments), `input` (send_input steps, posted), `sleep`, `shot`,
`press_until` (`input`, `every_s`, `until` = wait_for arguments, `timeout_s`; a log condition counts from the first press); `optional`
lets a step fail. The routes use log lines and the window, so they survive load times that vary by minutes.

Limits: a state is only as close to the moment as the game's last save (OoT saves when the player saves; a state made right after an
autosave or a save is exact, later ones restart from that save); everything after the save (position, enemies, time of day as the game
does not store it) comes back as the game restores it. A state made with another build still loads; the result says so.

## Bug bundles

The player presses **F9** in the game window (`[bug] key`): the runtime writes a bundle at once (screenshot of the presented frame, log,
effective config, environment, thread dump, committed save data, crash/freeze/fatal reports, `bug.json` with the exe's SHA-256 and the
commit it was built from) and a small window asks for a note (Enter saves it; the game keeps running). `capture_bug` asks the running
game for the same bundle through `<exe dir>/capture_bug.<pid>` and adds `window.png` (PrintWindow), `mcp.json` (launch arguments,
settings, env, run dir) and stdout/stderr. Keys and the bundle layout: `runtime/include/nxrt/bugcapture.h`, `[bug]` in
`runtime/docs/CONFIG.md`. Bundles go to `work/bugs` of the checkout the exe was built in (`launch` passes `NXRT_BUG_DIR`, so bundles
of runs this server starts land where `bugs` looks).

Reproducing one: `bugs()` (newest first), `bugs(id=...)` (note, build, log tail, screenshot), then `load_state(bug=...)`: the bundle's save
data and environment, and the reach of the state its run was loaded from (`bug.json` `state`; the game's default otherwise).
`save_state(name, from_bug=...)` keeps it as a named state.

| Variable | Default |
|---|---|
| `NXRT_AI_STATES_DIR` | `work/states/<id>` |
| `NXRT_AI_BUGS_DIR` | `work/bugs` |
| `NXRT_AI_STATES_TOML` | `recomp/games/<id>/ai_states.toml` |

## What the running exe offers (and what it does not)

There is no live control channel into a running game: no pipe, socket, command file or stdin reader (searched `runtime/src`,
`game-build/src`). Everything else is set at start and answers through files:

- Command line (`game_main.cpp`): `--log --ini --max-seconds --headless|--window --exefs --game --link --no-nvn`.
- Config keys, any of `runtime/docs/CONFIG.md`, through `launch(settings=...)`. Useful ones: `log.level`, `diag.watchdog_seconds`,
  `boot.press` (`"A@12,PLUS@30"`: timed pad presses), `boot.screenshot_at` / `boot.screenshot_every` (the renderer writes `shot_<n>.png`
  into the working folder), `diag.soak_csv` (a JSON line of process gauges every few seconds), `play.selftest` (window steps).
- Test tooling (`runtime/include/nxrt/tt.h`, `tt_script.h`), through `launch(script=[...])`: pad buttons and sticks, keyboard/mouse
  names (the play layer's key sink), `keydown vpad attach xbox` / `vpad button south 1` / `vpad axis lx -0.5` (an in-process SDL virtual
  pad), `@shot` (the presented frame, also headless), `@hash`, `@assert`, `@peek`/`@poke` guest memory (the `peek`/`poke` shorthands write
  the in-files and return the bytes), and game probes. Output in `<run dir>/testout`: `result.json` at the end, `trace.jsonl` written while
  it runs (the probes' state per frame), `shots/`. `settings={"test.enabled": 1}` gives the trace without a script.
  `launch` sets `test.frame_source=tick` unless told otherwise, because the default `hook` needs game hooks.
- Live, from outside: window messages or SendInput to the game window (`send_input`), PrintWindow capture, process and log watching.
  A live gamepad from outside would need a virtual-controller driver (ViGEmBus); not done. Guest memory of a running game is reachable
  only through a script's `@peek`/`@poke`.

## The check

    tools\.venv\Scripts\python tools\ai\tests\test_gamectl.py -v

Launches `fake_window.py` through a `.cmd` posing as the exe: window search through the process tree, settings reaching the
environment, capture while a topmost window of another process covers it, crop, post input (keys with scan codes and the extended bit,
text, clicks at pixel and fraction coordinates, wheel), `wait_for` window/change/log/visible/image and the not-running stop, `compare`,
`reports`, `kill`, a scripted run with `peek`/`poke` (the fake answers `@peek` like the runtime), `restart`, and the MCP protocol with
a launch and a screenshot coming back as an image. `set AI_TEST_FOCUS=1` adds `send_input(method="send")` (takes the focus briefly).
States and bundles (the fake logs its save folder, saves a slot on S with an uncommitted work folder beside it, loads it on Enter and
answers bug request files like the runtime): `load_state` of a built-in state with the routes of a test `ai_states.toml`, `save_state`
(committed files only, build, settings), `states`, `load_state` of the saved state in a new process, `capture_bug` from the running game
and with none running, `bugs` (list, one in full with its picture), `load_state(bug=...)`, `save_state(from_bug=...)`; and every game's
checked-in `ai_states.toml` names only known routes and step kinds. The runtime side (bundle contents, committed-only save copy, request
files): `runtime\build\release\nxrt_tests.exe bugcapture`.

## Only checkable once the game boots

- Whether the SDL window takes posted key/mouse messages while unfocused and the play layer acts on them (else use `method="send"`),
  and whether the window really opens without taking the focus (`SW_SHOWNOACTIVATE` + `SDL_WINDOW_ACTIVATE_WHEN_SHOWN=0`).
- Whether PrintWindow captures the Vulkan swapchain (else `method="screen"` with the window in front, or the game's own `@shot` /
  `boot.screenshot_at`).
- Raw mouse look with `rel`, scripted runs on the real exe (`frame_source=tick` delivering frames, the exit codes, `result.json`,
  `@peek`/`@poke`), and Ocarina of Time probes (there is no `runtime/games/oot` yet, so the trace has only frame, hash and time).
