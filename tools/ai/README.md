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
| `screenshot` | Client area of the game window (PrintWindow: works covered and unfocused, not minimised); returns the picture and a full-size PNG path; `crop`. |
| `send_input` | Steps `key/down/up/text/click/rclick/mclick/move/wheel/rel/wait`; coordinates in pixels or fractions. `post` = window messages to the game window only (background); `send` = SendInput after bringing it to the front, refused if it is not in front. |
| `wait_for` | Until `window`, `visible` (not black), `change`, `image` (SSIM against a reference), `log` (regex), `exit`, with a timeout. |
| `compare` | imgdiff (SSIM, MAE, histogram, changed pixels, masks) of two PNGs or a reference against the window now; writes a diff picture. |
| `log` | Tail, regex over the whole log, or only new lines since the last call. |
| `reports` | `<exe stem>_crash.txt`, `_freeze.txt`, `_fatal.txt` next to the exe and the test output's `*.txt`, flagged new since the launch. |

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
a launch and a screenshot coming back as an image. Instances: two named runs at once (own folders and arguments, input and capture reach
only the one named by instance or pid, separate logs and reports, the error without a name, relaunch and kill of one), a run this server
did not start (found by its command line; launch refuses its name, kill needs force), the memory check, an exe that refuses with exit code 7,
and name validation. A real game running meanwhile is never touched. `set AI_TEST_FOCUS=1` adds `send_input(method="send")` (takes the focus briefly).

## Only checkable once the game boots

- Whether the SDL window takes posted key/mouse messages while unfocused and the play layer acts on them (else use `method="send"`),
  and whether the window really opens without taking the focus (`SW_SHOWNOACTIVATE` + `SDL_WINDOW_ACTIVATE_WHEN_SHOWN=0`).
- Whether PrintWindow captures the Vulkan swapchain (else `method="screen"` with the window in front, or the game's own `@shot` /
  `boot.screenshot_at`).
- Raw mouse look with `rel`, scripted runs on the real exe (`frame_source=tick` delivering frames, the exit codes, `result.json`,
  `@peek`/`@poke`), and Ocarina of Time probes (there is no `runtime/games/oot` yet, so the trace has only frame, hash and time).
