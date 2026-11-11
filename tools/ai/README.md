# tools/ai: drive the game exe from Claude Code

An MCP server (`game`) that lets an AI agent start, watch, play and debug the game exe: process, window, screenshots, keyboard and mouse,
log, crash reports and the runtime's test tooling. Game independent: exe and log names come from `recomp/games/<id>/game.toml`.

| File | What |
|---|---|
| `gamectl.py` | The tools (methods of `Game`; the docstrings are what the agent reads). Reuses `tools/test/wincap.py` and `imgdiff.py`. |
| `states.py` | The state and bug tools (`states`, `save_state`, `load_state`, `capture_bug`, `bugs`), a mixin of `Game`. |
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
| `NXRT_AI_RUN_DIR` | `work/ai_run/<id>`: the log, stdout.txt/stderr.txt, script.nxs, testout/, shots/ of each launch; `<run dir>/<instance>` for a named instance |
| `NXRT_BOOT_MEMORY_PER_RUN_GB` | `13`: what `launch` (and the exe) count for one run in the memory check; `0` turns it off |

CLI, same tools, one process per call: `tools\.venv\Scripts\python tools\ai\mcp_server.py help`, `... status`, `... log tail=20 grep=error`,
`... send_input steps='["key Return"]' instance=a8`. A CLI call finds the game by the exe's file name and command line (it started nothing
itself, so `kill` needs `instance=`/`pid=` and `force=true` there).

## Several runs at once (instances)

Several sessions may run the game at the same time. Each run is an *instance*: `launch(instance="a8")` starts the exe with
`--instance a8 --instance-dir <run dir>/a8`, and that run writes everything of its own into that folder (log, stdout/stderr, test output,
screenshots, crash/freeze/fatal reports, saves and temporary storage, controls.ini, pipeline cache and key file), while the game files, the
shader cache and the BC7 cache stay shared (`runtime/docs/CONFIG.md`, "Named instances"). The run without a name is `default` (the run dir
itself, the shared saves and pipeline cache, reports next to the exe): only one default run per game folder can start.

- Every tool except `launch` takes `instance=` or `pid=` (any process of the run: the exe, its translator child, the window's process). Without
  them a tool uses the one run this server started, else the one game process running; with several it asks for a name. `status` always
  lists every running game process (`instances`: name, pid, started by this server or not, memory, window title, log) and the free memory.
- Runs this server did not start (other sessions') are found by the exe's file name; their instance, log and instance folder come from their
  command line (`--instance`, `--log`, `--instance-dir`). They can be watched, captured and given input, but `launch` never replaces one
  (an instance name in use is refused) and `kill` ends one only with `force=true`. `kill()` without arguments ends only this server's runs.
- `launch` refuses to start (before the exe does) when the free memory minus what the other runs will still take would not hold one more
  run (`settings={"boot.memory_per_run_gb": 0}` skips it). The exe repeats the check and refuses with exit code 7; a run that ends within
  1.5 s comes back with its exit code and stderr.
- Device-loss tests and fps measurements still need the machine to themselves (AGENTS.md, "Several runs at once").

## Tools

| Tool | What it does |
|---|---|
| `status` | One call: every running game process (instance, pid, own or not, memory, window title, log), free memory; for one run: processes (uptime, CPU s, memory), exit code, window (size, focused, minimised, hung), log (seconds since last write, recent errors/warnings, last lines), reports present, scripted-run result and last trace lines. |
| `launch` | (Re)start a run (`instance` name, or the default run; replaces only this server's run of that instance): args, `settings` = runtime config keys as `NXRT_<SECTION>_<KEY>` (beat the ini), env, `headless`, `script` (test tooling, with `peek`/`poke` shorthands), `wait` for the exit and get the result. Memory check first. |
| `restart` | Kill and launch again with the last launch's arguments (of `instance`, default the last launched). |
| `kill` | End a run's process tree: this server's runs, or `instance`/`pid` (another session's only with `force`). |
| `screenshot` | The game's own next presented frame (request file the runtime answers; works covered and unfocused), else PrintWindow/BitBlt of the window (never the screen unless `method="screen"`); returns the picture and a full-size PNG path; `crop`. |
| `send_input` | Steps `key/down/up/text/click/rclick/mclick/move/wheel/rel/wait`; coordinates in pixels or fractions. `post` = window messages to the game window only (background); `send` = SendInput after bringing it to the front, refused if it is not in front. |
| `wait_for` | Until `window`, `visible` (not black), `change`, `image` (SSIM against a reference), `log` (regex), `exit`, with a timeout. |
| `compare` | imgdiff (SSIM, MAE, histogram, changed pixels, masks) of two PNGs or a reference against the window now; writes a diff picture. |
| `log` | Tail, regex over the whole log, or only new lines since the last call. |
| `reports` | `<exe stem>_crash.txt`, `_freeze.txt`, `_fatal.txt` next to the exe and the test output's `*.txt`, flagged new since the launch. |
| `states` | Saved states (`work/states/<game>/<name>`) and the game's built-in ones and routes (`recomp/games/<id>/ai_states.toml`), with whether the exe on disk is the build a state was saved with. |
| `save_state` | The game's committed save data of the running or last run (or of a bug bundle) as a named state, with the build, the launch's settings and env, the steps back (`reach`), a ready check and a screenshot. |
| `load_state` | Start the game at a state or a bug bundle: its save data into the instance's own save folder (`[fs] save_dir`), the launch of that instance (default: the one it came from), then the reach steps and the ready check; the steps and their seconds, or the one that failed with the log tail. |
| `capture_bug` | A bug bundle of a run's moment (`work/bugs/<time>[-instance][-name]`): the running game writes it (request file), this side adds the launch and stdout/stderr; with no game running it is made from the last run's files. |
| `bugs` | The bundles, newest first (note, origin, instance, state, build; `instance=` filters), or one in full (bug.json, note, files, log tail, the screenshot). |

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

`load_state` never touches the player's own save (`%APPDATA%\<save folder>\save`): it copies the state's `save/` into the save folder of
the instance it starts (`<run dir>/save` for the default run, `<run dir>/<instance>/save` for a named one) and launches with `[fs] save_dir`
there. A state records the instance it was saved from and `load_state` starts that instance again unless given `instance=`; a bug bundle
starts in the instance it came from the same way, and `instance=` puts it beside the original. Settings and env merge, later wins: the game's `[defaults]`, the state's, the call's. A reach step is
a route name or a dict with one of `route`, `wait_for` (the tool's arguments), `input` (send_input steps, posted), `sleep`, `shot`,
`press_until` (`input`, `every_s`, `until` = wait_for arguments, `timeout_s`; a log condition counts from the first press); `optional`
lets a step fail. The routes use log lines and the window, so they survive load times that vary by minutes.

Limits: a state is only as close to the moment as the game's last save (the game has an auto-save manager and a save in the menu; a state
made right after an autosave or a save is exact, later ones restart from that save); everything after the save (position, enemies, what
the game does not store) comes back as the game restores it. Before the first save a state is its route: `first_scene` and a state saved
there replay New Game every time. A state made with another build still loads; the result says so. On 2026-11-06 the game wrote no slot
in the first eight minutes after New Game (the first scene's picture stayed black behind the intro's subtitles, the pause key showed
nothing), so the `continue` route (A on the first title entry, meant to continue a save) is not checked against a real slot yet.

## Bug bundles

The player presses **F9** in the game window (`[bug] key`): the runtime writes a bundle at once (screenshot of the presented frame, log,
effective config, environment, thread dump, committed save data, crash/freeze/fatal reports, `bug.json` with the exe's SHA-256 and the
commit it was built from) and a small window asks for a note (Enter saves it; the game keeps running). `capture_bug` asks the running
game (`instance=`/`pid=` choose the run) for the same bundle through `capture_bug.<pid>` in the run's request folder (the instance folder,
or the exe's folder for the default run) and adds `mcp.json` (instance, launch arguments, settings, env, run dir), stdout/stderr and, only
when the game gave no frame, `window.png`. A named instance's bundle has the instance in its folder name and in `bug.json`. Keys and the bundle layout: `runtime/include/nxrt/bugcapture.h`, `[bug]` in
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

## Gaps: what is not implemented, without looking at every scene

Every run writes `gaps.tsv` next to its log (rewritten every 30 s while new gaps appear, and at the end): each request the runtime only
stands in for, counted every time it happens. Areas: `nvn-proc` (an NVN entry point without an implementation, e.g. the mesh draws),
`nvn` (an operation the Vulkan back end leaves out or answers wrongly, e.g. GPU-side WaitEvent), `draw` / `dispatch` (skipped, or drawn
without a stage the translator could not do; detail = the shader hash), `shader` (a program set with an untranslated block), `import`,
`ipc`, `svc`. The end-of-run log has the 25 most frequent. The work list over one or more runs:

```bat
tools\.venv\Scripts\python tools\ai\gaps.py work\run\gaps_1 [more runs...] [--md gaps.md]
```

ranks them, joins shader hashes with the shader cache's `results.tsv` (stage, status, translator error; `--cache` or
`NXRT_NVN_SHADER_CACHE`, default the main checkout's `work\shaders\oot\spirv`), sums the untranslated stages by translator error weighted
by the draws that needed them, and names the AGENTS.md artifact row of known gaps. A place that answers with a stand-in and is not counted
yet should call `nxrt::gap(area, what, detail)` (`runtime/include/nxrt/gaps.h`). Things the runtime cannot see (a shader that translates
but computes the wrong thing, a texture decoded wrongly) never show up here.

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
- Request files in the run's request folder (`runtime/include/nxrt/bugcapture.h`): `screenshot.<pid>` naming a PNG (the presented frame is
  written there: `screenshot`'s default) and `capture_bug.<pid>` (a bug bundle: `capture_bug`); `dump_threads` (the watchdog's thread dump).
- Live, from outside: window messages or SendInput to the game window (`send_input`), PrintWindow capture, process and log watching.
  A live gamepad from outside would need a virtual-controller driver (ViGEmBus); not done. Guest memory of a running game is reachable
  only through a script's `@peek`/`@poke`.

## The check

    tools\.venv\Scripts\python tools\ai\tests\test_gamectl.py -v

Launches `fake_window.py` through a `.cmd` posing as the exe: window search through the process tree, settings reaching the
environment, capture while a topmost window of another process covers it, crop, post input (keys with scan codes and the extended bit,
text, clicks at pixel and fraction coordinates, wheel), `wait_for` window/change/log/visible/image and the not-running stop, `compare`,
`reports`, `kill`, a scripted run with `peek`/`poke` (the fake answers `@peek` like the runtime), `restart`, and the MCP protocol with
a launch and a screenshot coming back as an image. Instances: two named runs at once (own folders and arguments, input and capture reach
only the one named by instance or pid, separate logs and reports, the error without a name, relaunch and kill of one), a run this server
did not start (found by its command line; launch refuses its name, kill needs force), the memory check, an exe that refuses with exit code 7,
and name validation. States and bundles (the fake logs its save folder, saves a slot on S with an uncommitted work folder beside it,
loads it on Enter and answers screenshot and bug request files in its request folder like the runtime): `load_state` of a built-in state in
an instance with the routes of a test `ai_states.toml`, `save_state` (committed files only, instance, build, settings), `states`,
`load_state` of the saved state in a new process of the same instance, `capture_bug` from the running game and with none running, `bugs`
(list, filter by instance, one in full with its picture), `load_state(bug=...)` in its own instance and beside it in another,
`save_state(from_bug=...)`, the default run's save folder; and every game's checked-in `ai_states.toml` names only known routes and step
kinds. The runtime side (bundle contents, committed-only save copy, request files, a named instance's folder and name):
`runtime\build\release\nxrt_tests.exe bugcapture`. A real game running meanwhile is never touched. `set AI_TEST_FOCUS=1` adds `send_input(method="send")` (takes the focus briefly).

## Seen on the real game (oot.exe, RTX 4080, 2026-11-06)

- The window opens without taking the focus, and posted key messages reach the play layer while it is covered and unfocused: Enter
  posted by `send_input` is A on the title menu, Escape is +, F9 the bug hotkey.
- **PrintWindow and BitBlt do not see the Vulkan swapchain** (black), and `method="screen"` shows whatever covers the window (another
  application on the desktop). `screenshot` therefore asks the game for its own next presented frame first (request file
  `screenshot.<pid>` in the run's request folder, answered by the runtime's request poller); bundles and states never keep a black or screen picture.
- `load_state("first_scene")`: boot to the title movie 17 s, A taken on the second press at 27 s (the title movie's decoder finalized
  after 26 pictures), the first frame with 13,049 draws at 51 s; 57 s in all.
- Through the MCP protocol: `load_state("first_scene")` 30 s with warm caches, `save_state("after_intro")`, `capture_bug` (answered by
  the game), `kill`, `load_state("after_intro")` in a new process 29 s, `bugs`, `load_state(bug=...)` 24 s.
- A Git Bash `tail -f` on the run's log keeps it from being renamed (`launch` keeps the previous log as `<log>.prev`): `launch` waits
  20 s and then says the log is still open. A `tail` started by a stopped Claude Code monitor can survive it; end that `tail.exe`.
- Still open: raw mouse look with `rel`, scripted runs on the real exe (`frame_source=tick`, `result.json`, `@peek`/`@poke`), and the game probes (`runtime/games/oot` has no `tt::GameProbes`, so the trace has only frame, hash and time).
