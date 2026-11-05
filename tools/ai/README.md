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

## Only checkable once the game boots

- Whether the SDL window takes posted key/mouse messages while unfocused and the play layer acts on them (else use `method="send"`),
  and whether the window really opens without taking the focus (`SW_SHOWNOACTIVATE` + `SDL_WINDOW_ACTIVATE_WHEN_SHOWN=0`).
- Whether PrintWindow captures the Vulkan swapchain (else `method="screen"` with the window in front, or the game's own `@shot` /
  `boot.screenshot_at`).
- Raw mouse look with `rel`, scripted runs on the real exe (`frame_source=tick` delivering frames, the exit codes, `result.json`,
  `@peek`/`@poke`), and Ocarina of Time probes (there is no `runtime/games/oot` yet, so the trace has only frame, hash and time).
