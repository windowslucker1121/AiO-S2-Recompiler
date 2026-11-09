# Notes for AI agents

What a new agent needs to know before working here. Keep it current: add what you learn, remove what stops being true.
Repository basics are in `README.md`, the recompiler in `recomp/README.md`, the game boundary in `runtime/docs/GAME_BOUNDARY.md`.
**Start with this file** (state, artifacts, next steps) and `DEVELOPMENT_SETUP.md` (everything a development machine needs). Keep both
current. How to contribute: `CONTRIBUTING.md`.

## Legal rules (always; details in `LEGAL.md`)

- **Never commit anything from a game, the console or Nintendo's SDK**: game files or anything extracted or generated from them (NSO/NRO,
  RomFS files, bfsha/BNSH shaders, shader corpora, translated SPIR-V, pipeline caches, textures, audio, movies, fonts, game text, saves,
  lifted C++), keys, firmware, SDK code, headers or documentation, logos, box art, and screenshots, frame captures or bug bundles of a
  game. Facts are fine: title IDs, build IDs, hashes, offsets, symbol names, short notes about formats.
- Test fixtures and sample data are synthetic or come from openly licensed projects, with their licence recorded.
- No piracy help: no links to dumps, keys or firmware, no key derivation or decryption, no instructions beyond "dump your own copy".
  Do not add code that decrypts game containers or reads console keys. Inputs are already decrypted dumps only (an extracted folder or a
  `.nup`); the key-based NSP/XCI path (`nx_extract --keys`, the launcher's keys page) was removed on 2026-11-08 and must not come back.
- **Names:** game and Nintendo names only to say what a module is for ("module for <game>"). Never in product, app, exe, window or
  package names, and no Nintendo or game logos, icons or art styles. New game ids are short neutral ids (`oot`, `pkp`).
- Copied code needs a licence compatible with GPL-3.0 and an entry in the notices (`runtime/THIRD_PARTY_NOTICES.md` or the folder's
  `licenses/`). Never ask users to upload game files or logs that contain game data.

## State (branch `oot-nup`, 2026-11-06)

Porting **The Legend of Zelda: Ocarina of Time** (Switch 2, program `0400486024DEC000`, update 2.0.0 / `131072`) next to the
existing Pokopia port. Game id `oot`: `recomp/games/oot/` (manifest `game.toml`, `config.py`, `CMakeLists.txt`, `main.sites`).
The dump is `oot.nup` in the repo root (25.6 GB, git-ignored; the docs call it `oot.nup`: the user may rename the local file to that).

The user's goal is a port that is **actually playable** on this PC. Milestones in the user's order: (1) boot into the **main menu
without artifacts**; (2) **basic gamepad** (XInput / SDL gamepad drives the game, menus included); (3) last, **FPS**: measure first
(`[gfx] log_fps = 1`, hitch log, `[diag] soak_csv`), reach **30 fps in the main menu**, then in game **30 fps**.
**Unknowns: check the reference ports first** (user, 2026-11-07): before reverse-engineering any NVN2 / Ampere / Switch 2 behaviour or runtime bug, look at (1) **featureJosh/pokopia-recomp** (github, releases + `CHANGELOG.md`; forked from drakolordx7/switch2-recomp, the same codebase family as this one: `tools/shaders/ampere`, `runtime/src/gfx/nvn`), then (2) **jaigner-hub/ocarina-of-time-recomp** (the same game, another codebase: SM86-to-Maxwell lowering; `docs/phase3.md`, `docs/mesh*.md`, `docs/bindless.md`). Raw files via raw.githubusercontent.com (no gh CLI here). Dig in ourselves only when they have nothing, and note which facts came from where. (The earlier reference, the Lichii emulator, is dropped; facts already taken from it are marked in the A9/A17 rows.)
Standing instructions: keep going until blocked, then report; install tools only into git-ignored repo folders from official
releases with checksum checks; never change system or security settings (give the user the command); when the permission check
blocks fetching an external repository, give the user the exact PowerShell commands; one build at a time; a second `oot.exe` only as a
named instance and only by the rules of "Several runs at once" below; commit at milestones.

Done: NUP extraction, ZBIC -> plain NSO, manifest, lift of all four modules (0 untranslated instructions), and all four module
libraries compile with clang-cl (`work\build\oot\oot_{main,sdk,subsdk0,rtld}.lib`, main 427 MB). First build 25 min cold on 18
cores; the FP16 fix afterwards rebuilt one shard in 113 s (most of it the re-lift, which every lifter change triggers).
Runtime side: `runtime/games/oot` exists as a bring-up module (identity from the manifest, title facts from `main.npdm`: priority 44,
core 0, stack 0x100000, system resource 0x3000000; firmware 22.0.0 as a lower bound since ZBIC needs it; uid "OOTRecmp"; no hooks,
probes or play mode). The runtime copes without play hooks (`play.cpp`: `if (gp && gp->frame_update)`).
Placeholders (no control.nacp in the dump): languages 0x1ffff, display version "2.0.0", save sizes and temporary storage 64 MiB.
Not started: hook sites, shader cache. A `.nup` goes through the Initial Setup (`oot_setup.exe`, below).

Shader toolchain: `refs/ryubing` is a git submodule (Ryubing's Ryujinx, `https://git.ryujinx.app/ryubing/ryujinx.git`, pinned to
`a82350bb774f70fcbd41c9987bf67a3775409963`, the revision `tools/shaders` names; MIT). After `git submodule update --init`:
`powershell -File tools\shaders\scripts\setup_ryujinx.ps1` (stages 3 projects into the git-ignored `tools\shaders\third_party`), then
`dotnet build tools\shaders\harness\SpirvHarness.csproj -c Release` (about 5 min the first time; .NET SDK 10 is installed) gives
`tools\shaders\build\bin\Release\net10.0\spirv_harness.exe`. No Vulkan SDK here: pass `--no-val` to `--batch`. Corpus:
`tools\.venv\Scripts\python -I tools\shaders\scripts\extract_generic.py --romfs game\oot\updates\131072\romfs --main
game\oot\plain\exefs\main --out work\shaders\oot\corpus --jobs 6 --progress` (needs `zstandard` in the venv, installed). The agent
tooling's permission check blocks adding external repositories: the user added the submodule themselves.
The corpus is extracted (`work\shaders\oot\corpus`, 25 min): 47,203 unique blocks (17,417 vs, 25,228 ps, 3,743 cs, 815 of stages 6/7),
nearly all from `Shader/Model.Product.1x0.product.Nin_Ounce_NVN2.bfsha.zs`; the "oodle: no oozx DLL" notes are false positives of the
generic scan (OoT uses zstd). Translate:
`tools\shaders\build\bin\Release\net10.0\spirv_harness.exe --batch work\shaders\oot\corpus --out work\shaders\oot\spirv --jobs 16 --no-val`;
run the game with `NXRT_NVN_SHADER_CACHE=<repo>\work\shaders\oot\spirv`, `NXRT_NVN_SHADER_CORPUS=<repo>\work\shaders\oot\corpus` and
`NXRT_NVN_SHADER_TRANSLATOR=<repo>\tools\shaders\build\bin\Release\net10.0\spirv_harness.exe` (the MCP `launch` takes them as `env`).
A few blocks the game uses are not in the corpus; the on-demand translator gets them.
**Tensor-core variants (`<hash>.cm.spv`, 2026-11-08):** a compute block with HMMA.16816 also gets a second module: each HMMA as one
`OpCooperativeMatrixMulAddKHR` (VK_KHR_cooperative_matrix) built from the lane's own fragment registers, and HADD2/HMUL2/HFMA2 in
native float16 (`tools/shaders/ampere/frontend/AmpereCoopMat.cs`; the harness writes it in single, batch and serve mode, and
`merge_results.py` carries it). The element order of a cooperative matrix is implementation-defined: `hwcheck --coopmat`
(`tools/shaders/hwcheck/README.md`) measures it on the GPU (RTX 4080: A registers 1 and 2 swapped) and checks the translation; the
runtime runs the same module as a probe at start (`runtime/src/gfx/vk_coopmat_probe.cpp`, log `cooperative matrices for HMMA: on`)
and only then loads `.cm.spv` files (`[gfx] cooperative_matrix`). Rerun `hwcheck --coopmat --header runtime\src\gfx\vk_coopmat_probe_spv.h`
after changing the HMMA path. OoT's network passes fea98fe7 / 49659bca / f90d726d are "runtime" blocks (not in the corpus, code in
`work\shaders\oot\missing`): translate them with the single mode (`spirv_harness code_<h>.bin ctrl_<h>.bin -o <cache>\<h>.spv`
writes `<h>.cm.spv` next to it; the scalar `.spv` stays byte-identical). fea98fe7 on the village capture: 2.1 ms -> 0.05 ms of GPU
per frame, final picture SSIM 0.9999 against the scalar module (within replay-to-replay noise).
**Float16 variants (`<hash>.f16.spv`, 2026-11-08):** a block with HADD2/HMUL2/HFMA2 with a half result also gets a module that computes
them in native float16 in any stage (DenormPreserve 16 + SignedZeroInfNanPreserve 16); the runtime loads it when the device has
shaderFloat16 and both float16 float-controls properties (`[gfx] float16_shaders`, log `float16 shader variants: on`); the
cooperative-matrix variant now needs the same. OoT's corpus has only 11 such blocks, all compute HMMA blocks (they also got `.cm.spv`,
which wins on the RTX 4080): **OoT's fragment shaders use no half-result math** (only HADD2.F32 conversions). The ~100 fp64 operations
in each hot material fragment shader (957527e8, 4097c13f, dcfaa938, ...) came from **FMUL.FTZ.RZ** (the x / 2pi before MUFU.SIN/COS;
39,126 corpus blocks), whose directed rounding compared against an fp64 product; it now uses the sign of `fma(a, b, -r)` in fp32
(AmpereEmit.cs FMUL). Both checked bit for bit with `hwcheck --game-corpus` on the RTX 4080 (hwcheck README, findings 2026-11-08;
the CUDA tools are the 13.3 redistributables in `tools\nvdisasm\cuda_path\bin`, `CUDA_PATH` set to that folder's parent).
Measured on the village capture (`20261108-000413-a9g1-village`, `[capture] reset = 0`, `[prof] gpu_draws`, median per draw over
137-2,300 loops after every pipeline was built; old-vs-old noise -0.3 ms): draw stamps 40.2 -> 38.5 ms per loop, mostly fs 86d325e2
(1.74 -> 0.60 ms); 4097c13f / 40478981 +0.09 ms each (the fp32 sequence is longer than the fp64 one there); the fern 957527e8 is
unchanged (its FMUL.RZ is not on the hot path; with reset 0 its mesh counts accumulate, with reset 1 its fastest loop is 1.04 ms both
ways). Picture pixel-identical (SSIM 1.000, old vs new screenshots). **Not merged into `work\shaders\oot\spirv` yet** (only the 11
float16-variant blocks are, backup `work\shaders\d1s\backup_f16`): the 39,061 changed blocks need their pipelines built again (290 s
for the village frame's ~1,000 parts on the replay) or a preparation. The translated cache is `work\shaders\d1s\full2` (complete:
corpus re-translated plus the main cache's non-corpus rows); merge with `merge_results.py work\shaders\d1s\full2 work\shaders\oot\spirv`.

Batch translation (cache in `work\shaders\oot\spirv`): 46,357 of 47,428 rows ok (was 37,576 before the bindless work, 2026-11-06);
236 unsupported (mostly "runtime"-group compute blocks whose range runs into ASCII names: `.UNKNOWN_656`, `e6e`, ... are text), 833
exceptions (800 stage 6/7 blocks "without code": Foliage/Terrain programs are stages {6, 7, 1}, probably task/mesh). Draws that need a
missing shader are skipped. What fixed the 8,600 (they were most of the game world's materials, all needed after the intro):
- **`0xf60` = TEX with the handle in a uniform register** (`TEX.SCR RZ, R73, R76, R48, UR12, 0x0, ARRAY_2D, 0x1`): URn at bits 40..45, an
  8-bit immediate at 46..53 (always 0 seen), bits 54..59 unused, the rest as the bound TEX 0xb60 (nvdisasm bit probe). The compiler loads
  URn = handle and URn+1 = handle >> 20 (the sampler index); the emitter passes URn as a Ryujinx bindless handle.
- **`TEX.SCR.B`**: the handle is the first register of Rb, the other Rb parameters follow it (2D + DC: Ra = x, y; Rb = handle, depth ref).
- **The handles come from a storage buffer table indexed by uniform data** (`LDG [c0 storage base + (c4[34] & 0xfffff + c5[187]) * 32 +
  0x10]`). `tools/shaders/harness/translator/ampere-bindless-handle-expr.patch` (Ryujinx `BindlessElimination`) turns a handle the draw
  determines (constants, uniform words, storage words at such offsets, integer ops) into an ordinary texture binding with constant buffer
  slot 0x20000 and writes the expression as postfix text; the harness prints it in the resource list (`tex<type>[c131072:<n>|<rpn>]`)
  and the runtime evaluates it at each draw (`NvnSource::HandleExpr`, `eval_handle_expr` in `nvn_replay.cpp`). Per-pixel handles (the
  4,336 `TEX.SCR.B.DC` blocks: clustered-light shadow maps, light index from another storage load in a loop) cannot be bound this way:
  the sample is left out and a depth comparison answers 1 (lit). Ryujinx's pool-array fallback (`GenerateBindlessAccess`) is off in the
  harness (`QueryHostSupportsSeparateSampler` false) because the runtime binds no texture arrays.
- `S2R SR_VARIABLE_RATE` (350 fragment shaders) reads 0 (1x1 shading, inferred).
Re-translating a subset: make a folder with a filtered `unique.tsv` and junctions to the corpus `code`, `ctrl`, `glslc`, run `--batch` on
it, then `tools\shaders\scripts\merge_results.py <new out> work\shaders\oot\spirv` (ok rows and their .spv replace the cache's).
**nvdisasm** (CUDA 13.3.1 redistributable, SHA-256 checked) is in the git-ignored `tools\nvdisasm\`:
`tools\nvdisasm\cuda_nvdisasm-windows-x86_64-13.3.73-archive\bin\nvdisasm.exe -b SM86 -raw <file of 16-byte words>` (one word per file when a
probe word is illegal: nvdisasm stops at the first). `sassdecode` (`dotnet build tools\shaders\ampere\decoder\tool\sassdecode.csproj -c
Release`, then `dotnet tools\shaders\build\bin\Release\net10.0\sassdecode.dll --census <corpus> --out <tsv>`) gives the opcode census.

**Shader compilation screen (user requirement: no shader-compile stutter in game)**: `nxrt::nvn::shader_setup_before_start`
(`runtime/src/gfx/nvn/nvn_setup.cpp`, called from `game_main.cpp` before the game window opens): a Win32 "Preparing shaders" window that
(1) translates every corpus block without a `results.tsv` row through the cache's own `--serve` translator (corpus "C" requests,
all cores but two) and (2) runs the shader preparation (`run_shader_prepare` in game_main: headless device, pipeline parts of
`<cache>/../programs.bin` into `pipeline_cache.bin`) unless `prepare.state` says `complete: 1`. No work: no window. Closing it ends the
process; the next start resumes. `[nvn] setup_on_start = false` skips it. The freeze watchdog is paused while it runs.
`programs.bin` comes from `tools/shaders/scripts/bnsh_programs.py --romfs <romfs> --out <cache>/../programs.bin` (BNSH program records
in Shader/, Layout2Shader/ (the UI), Effect/ (vsetb); records of one program are 0x40 apart; 39,363 vertex+fragment programs, 100 s).
Without the preparation the game queues ~4,400 programs at boot and the Arc driver needs ~350 ms per pipeline part on the background
threads: black screen and 9-18 fps for minutes (boot 23). The preparation runs at ~34 programs/s on this machine (~20 min).
`prepare.state`'s key now holds the size of the cache's `results.tsv`: shaders translated since (the bindless work made 10,000 more
programs preparable) make the next start prepare again. Without that, the new shaders flooded the background part builder (350 s of
worker CPU in two minutes) and starved the game (no frame for 10 s). Headless: `oot.exe --prepare-shaders --if-needed`.
**Prepared cache lost / parts missed (fixed 2026-11-06, RTX 4080):** two causes. (1) `ensure_cache` sized the file with `ftell`, which
is 32-bit on Windows (-1 from 2 GiB on): NVIDIA's prepared cache is 2.2-2.3 GB, so the first game start loaded nothing and its periodic
save replaced the file with what it compiled itself (boot1: 2.2 GB -> 88 MB; oot_cap -> 170 MB). Sizes are 64-bit now, the log says what
the driver holds after loading (NVIDIA returns every imported byte), and a save never writes less than that. (2) The preparation built
pre-raster parts with depth clamp on only, while 1,061 of the 1,085 keys in `pipeline_keys.bin` draw with it off: `[nvn] prepare_rasters`
(default both). `prepare.state` is scheme 2 (raster flags, cache file and `cache_bytes`); the setup prepares again for an older scheme or
a cache file that is missing or under half its size. First scene, same loop-capped shaders: before 74.9 ms per background part (5,101
parts, cache not loaded), overlay ~12,300 left; after 0.73 ms per prepared part (12,241) and 20 ms per new one (175), overlay 0 from
12 s on (17 at most), cache 2,305,488,425 bytes loaded in 3.3 s and unchanged after the run. The log line `shader parts built: N the
preparation made (x ms each), M new (y ms each); the overlay shows K left` tracks it. With the driver's disk cache warm the preparation
took 25 min for 117,981 parts (both raster sets); a forced second one loads the file, rebuilds everything in 58 s and saves the same
2,305,488,425 bytes. Test folder: `work\shaders\pcfix` (deleted after the merge; runs `work\run\pcfix_*`).

**Multi-game goal and the game library (2026-11-08; user: one app "to rule it all", for every Switch 2 game).**
- Static recompilation stays one build per game (and per game update): module bases are baked into the lifted code and the runtime is
  process-global. The front end hides that. The game boundary is mostly data; what a new game really needs is hook sites (frame update)
  and whatever the lifter, services or shader translator do not cover yet. Gaps for a generic pipeline: nothing parses `main.npdm` /
  `control.nacp` (OoT's values are hand-copied); `Info::platform` defaults to Switch 1; without a frame hook pads are never polled
  (`play.cpp:880` is the only `poll_pads`); untranslated instructions do not fail a build (`guest_unimpl` at run time); no NRO loader;
  A57 CPU constants in `a64.py`; `release_b/game_build.py` (Zig) is the only path that compiles on a player's PC.
- Dumps in the repo root (static analysis only): **Mario Wonder** `.nup` (title 010015100B514000, update 5.0.0, addon with content
  only) is a **Switch 1 binary** (SDK 16.2.12, NVN1, Maxwell bnsh/bfsha, no subsdk; it calls `nn::pl::IsRunningOnOunce`): the runtime's
  untested Switch 1 path; its ExeFS is in `game\smbw`. **Pokopia** folder (update 655360 = 2.0.1, build id matches `recomp/games/pkp`):
  main/sdk/subsdk0 do **not** decode as LZ4 (invalid match offset in every segment, sha256 differ from the manifest; rtld and npdm match):
  needs a re-dump before it can be lifted.
- **`recomp_library.exe`** (`runtime/apps/setup`, target `game_setup`, replaces `oot_setup.exe`; native Win32 + GDI+, dark theme; the
  user chose this over the Tauri `launcher/` and `runtime/apps/launcher`): manifests read at run time (`setup_games.cpp`), so one exe
  serves every game; `library.ini` next to the exe; dumps identified by title id (`nup_extract.py --list` or ACI0 of `main.npdm`), unknown
  titles stay as "No port yet". One button per game, **Play**: the five preparation steps below run once as one queued job (one game at a
  time, the others wait), then the game starts. Status words only; extras in the "•••" menu; This PC lists only problems; drag and drop
  adds a dump. **The library page lists only games the player added** (user, 2026-11-08: known games without a dump stay hidden; `sel()`
  returns only added games). **Report a bug...** (Help menu and "•••") opens a GitHub issue form (`.github/ISSUE_TEMPLATE`: `bug_game.yml`,
  `bug_recompiler.yml`, `game_status.yml`) with `game`, `app` (version, `RECOMP_LIBRARY_VERSION` from the standalone CMake) and `system`
  (GPU/driver, Windows, memory from the This PC checks) prefilled by query parameters; nothing is sent by the app. Settings page (`setup_settings.cpp`): Graphics, Display, Controller, Keyboard & mouse (press-to-bind, SDL key names from
  `setup_keynames.inc`), Audio; edits the game folder's `controls.ini` in place (only changed keys). User preference: minimal text.
  Test route: `recomp\scripts\nx_env.cmd work\build\library\build.cmd` (standalone build, needs game-build's fetched Vulkan and SDL
  headers), `recomp_library.exe --add <dump>`, `runtime\apps\setup\tests\winshot.py` (shot, cmd ids of the `Id` enum, close).
  Not checked yet: Play running the setup and then the game; two games queued. Next: build a game from the library (lift + compile as a
  queue step), retire `runtime/apps/launcher` and `launcher/`, release layout `games\<id>\game.toml` next to the exe.
  **Release package / CI (2026-11-08):** `.github/workflows/release.yml` builds `tools/release/package.ps1` on `windows-latest` (tag
  `v*` or "Run workflow" with a version -> GitHub release with the zip + `.sha256`; PRs touching the library -> build only, zip as a
  workflow artifact). The package: `recomp_library.exe` (standalone CMake `tools/release/library`, no game or runtime needed, no
  `SETUP_DEV_ROOT`), `games\<id>\game.toml` (only the `-RtGames` games, default `oot`: a game without its runtime DLL cannot be built
  from the package), `tools\scripts` (3 .py), `tools\spirv_harness` (self-contained `dotnet publish`),
  `tools\python` (python.org embeddable 3.13.17, SHA-256 pinned, + zstandard/lz4/capstone wheels), licences, and the **player-side
  game build** (release model b): `tools\build\` = `game_build.py` (game-build/release_b), the lifter (`recomp\nxrecomp`, `include`,
  `games\<id>\*.py|*.sites`), Zig 0.17.0 (SHA-256 from ziglang.org's index), `nx_plain_nso.exe` (ZBIC main -> plain NSO before the
  lift); `games\<id>\sdk\` (stub.cpp, rt_api.h, game.rc, game.manifest.in, `<id>_rt.lib`) and `games\<id>\runtime\` (`<id>_rt.dll`
  from game-build with `GAME_BUILD_RT_DLL=ON GAME_RELEASE=ON`, astcdec DLLs, fonts). **A game exe is never in a release** (it holds
  lifted code): the library's step 2 "Build the game program" runs game_build.py on the player's own dump into `<data>\program`
  (exe + runtime files), recorded as `built_for` (GameId + a stamp of the sdk/runtime/tools files) in setup_state.ini; a new release
  or game version builds again. Inputs: `.nup` or a folder with decrypted exefs/romfs; NSP/XCI are refused (encrypted; no key
  handling, LEGAL.md). CI also builds tools/textures (fetch_third_party.ps1) and vcpkg dav1d/libvpx/opus at commit 661e42cd into
  `tools\vcpkg` (cached binaries), and checks that `oot_rt.dll` contains dav1d. Local run: `recomp\scripts\nx_env.cmd powershell -File
  tools\release\package.ps1 -Version 0.1.0 -Out <dir> -HostPython <a Python with pip> [-RtBuildDir work\build\rtdll]` (the venv has no
  pip; system 3.11 works).
The text below describes the preparation steps and checks, which the library keeps.
**Initial Setup (`oot_setup.exe`, since 2026-11-07; `runtime/apps/setup`, target `game_setup`, name from `game.toml` `files.setup_exe`)**:
the player's first program, native Win32 (menu bar, four pages: Requirements, Game files, Prepare, Play; task dialog for Rebuild).
Game independent: names from `game_manifest.h` (`GAME_DUMP_VERSION`, `GAME_MAIN_BUILD_ID` were added to `GameManifest.cmake`); it links
nothing of the runtime. Requirements it checks (and gives fix steps / winget commands for, never installs): Windows 10 1809+, a Vulkan 1.3
GPU (its own vulkan-1.dll query: dynamic rendering, synchronization2, swapchain; warns without VK_EXT_graphics_pipeline_library or
VK_EXT_mesh_shader), RAM, disk space, every DLL `oot.exe` and the DLLs next to it import (PE import table: no VC++ redistributable is needed,
the exe is /MT), .NET for the translator (from `spirv_harness.runtimeconfig.json`: the dev build is framework-dependent net10.0, a
self-contained publish needs none), Python with `zstandard` + `lz4` (extraction and shader scan), the dump (main's build id against the
manifest). Steps (child processes, below-normal priority, each in a job object so Cancel ends the whole tree): `nup_extract.py`,
`extract_generic.py --exefs` (main is ZBIC: the scan skips it, the on-demand translator covers main's few blocks), `oot.exe
--translate-shaders` (new: step 1 of the "Preparing shaders" window without the window, `translate_missing_shaders` in nvn_setup.cpp),
`bnsh_programs.py`, `oot.exe --prepare-shaders --if-needed`. New exe flags: `--cancel-event NAME` (graceful stop: translations stay,
pipeline cache saved, exit 3), `--prepare-shaders --check` (`prepare: ready` / `prepare: needed <why>`: GPU, driver, pipeline cache UUID,
new translations), `--prepare-shaders --force`. Redo detection: `<data>\setup_state.ini` records the game (main build id + version folder)
each step ran for and a stamp of the translator folder (names, sizes, times of its exe/DLLs); a change re-runs that step and the ones after
it (a new translator moves the old `spirv` aside first). Data folder layout and the test route: DEVELOPMENT_SETUP.md section 7b.
Checked on the RTX 4080 with a 77 MB test `.nup` (`runtime/apps/setup/tests/make_test_nup.py`: real ExeFS + 4 small romfs files): all
steps in about 2 minutes (224 blocks, 181 programs, 100 s of it the compile while the main checkout's full preparation ran beside it),
every page screenshotted (`tests/winshot.py`), driver change detected (edited `prepare.state` key), Rebuild "compile again" (0.4 s from the
pipeline cache), Cancel in the shader scan of the full game (no process left), Play started the exe (refused by the memory guard beside
another run: the setup now explains an exit within 4 s instead of closing). Not checked: a real Play into the game from the setup, a full
`.nup` extraction. NSP/XCI are not supported (encrypted; the project decrypts nothing).

**Shader preparation without batches (2026-11-07, `gpl_prepare` in nvn_gpl.cpp)**: the 192-program batches with a barrier are gone. One
work list, every thread takes the next program; each shader counts the programs of the list that still need it and the last one releases
its modules (`module_cache_release`, nvn_pipeline.cpp) and SPIR-V words. The expensive programs go first: a stage with the `gmem` token
(`ShaderCache::record_cost`, from results.tsv) or at least `[nvn] prepare_heavy_kb` (256) KB of SPIR-V, largest (gmem x8) first; the
rest grouped by fragment shader. Part keys stay in `g_libs` for the whole run, so a part shared across the old batch borders is built once.
Independent of how the global-memory search is compiled (only the token and the size are read). Test set route:
`tools/shaders/scripts/programs_subset.py` (a random sample of programs.bin with its .spv files in a folder of its own) and a fresh
`NXRT_NVN_SPIRV_SALT` per run, so the driver's disk cache cannot answer (`tools/shaders/scripts/prepare_bench.py` runs it for several exes).
**Measured (RTX 4080, 2026-11-07, 1,152 random programs = 3,456 parts, 14 threads, cold driver cache, machine otherwise idle):** old
(batches) 375.7 s, 4,722 s CPU = 12.6 cores busy; new 354.5 s, 4,867 s CPU = 13.7 cores busy: **6% faster**. With a cold cache the driver's
own compile work (1.4 s CPU per part on average) keeps the threads busy and the barrier costs little; the gain is the idle tail of each
batch. **Half warm** (the driver's disk cache already holds the parts of half the programs, as after a partly finished or repeated
preparation; one salt per exe, warmed with 576 of the programs): old 206.8 s, 11.5 cores busy; new 182.2 s, 13.7 cores: **12% faster**.
Fully warm both take about 2 s. The full preparation in the main checkout the same day (old code, 39,363 programs, 45,289 of 118,089 parts under 2 ms from the
driver's disk cache) took 5,293 s. More speed needs less driver work, not more parallelism: e.g. the two raster sets compile every
pre-raster part twice (`[nvn] prepare_rasters`), and 33% of the sample's programs count as expensive.

Hardware of this machine (since 2026-11-06, the second development machine): AMD Ryzen 7 5800X3D (8 cores, 16 threads), **NVIDIA
GeForce RTX 4080** (discrete, 15.7 GiB, Vulkan 1.4.341, driver 610.88), 32 GB RAM, Windows 11 Pro 26200. Everything above that names
timings, the Arc driver or A8 was measured on the first machine: Intel Core Ultra 5 125H, integrated Intel Arc (Vulkan 1.4.325, 8.8 GiB
shared), 16 GB RAM. **A8 reproduces on the RTX 4080** (run `work\run\a8_nv1`, 2026-11-06): not Arc-specific.
Times here: lift + build of all four modules 10 min 48 s cold (was 25 min), runtime + exe 1.5 min, corpus extraction 14 min, batch
translation 2 min 49 s, shader preparation ~80 programs/s (was 34).

**Merge of `claude/initial-setup-d27d10` verified 2026-11-07 (b22af1f):** `game_exe` and `game_setup` (oot_setup.exe) build, all 172 runtime test groups pass (`work\run\mergeverify`). After the A9/A15 fixes of the same day the main cache `work\shaders\oot\spirv` holds 314 + 770 changed blocks (LDC.64, mesh slot base): **a shader preparation is due** (not run; shaders still change). First scene in game: 14-15.5 fps at 180-210 s (RTX 4080, alone, cache not prepared for the changed blocks).

**Merged 2026-11-07 (65daeb3): A9 mesh shaders, A17 global-memory fallback with device addresses, culling-chain fixes.** All 172 runtime
test groups pass (after `ninja -t clean`: four gfx groups segfaulted on stale sccache objects first). **Main shader cache re-translated
2026-11-07 (afbe1f5)** with the merged harness plus `spirv-nan.patch` (float Clamp/Min/Max as NClamp/NMin/NMax; chained after the mesh
patch through the csproj's `Then` metadata): corpus without the quarantined block in `work\shaders\oot\corpus_noq` (filtered
`unique.tsv`, junctions), 46,988 ok / 207 unsupported / 7 exceptions (the same blocks in the old cache: 46,203 / 177 / 822; the 7 are
GotoElimination NullReferenceExceptions, old too), 3,413 `gmem` rows, mesh 770 ok, task 15 ok; the SPIR-V of 36,900 blocks changed.
On-demand rows (blocks not in the corpus) were dropped; the quarantined block keeps its old row and `.spv`. Old cache:
`work\shaders\oot\spirv_old_20261107` (delete once the new one is trusted). Culling root 4c1e8564 (from `work\dumps\code`): 0 Function
variables read but never written (the old cache's `.spv` has the 17). All 10,716 discarding fragment shaders use `OpKill` (no demote), so a
discarded fragment cannot reach the colour writes. **Preparation** with a cold driver cache: 88 min (~4 programs/s past the parts the
driver's disk cache still had), 118,089 parts, `pipeline_cache.bin` 2.83 GB; the first attempt loaded the old 4.7 GB cache on top and
hit the 16 GB `[memory] limit_gb` at 34,696 of 39,363 programs, saving nothing: move a stale `pipeline_cache.bin` aside before preparing
for re-translated shaders (old one: `pipeline_cache_old_20261107.bin`), and pass `NXRT_MEMORY_LIMIT_GB=24` to `--prepare-shaders`.
**The first scene now passes 16 GB of footprint** (peak 17.7 GB; the default limit stopped the first baseline run 4 s into the scene):
use `settings={"memory.limit_gb": 24}` with `load_state`. **Baseline `work\run\base_1`** (exe from 8d7d56e, RTX 4080, alone):
scene at 42 s, 21 s / 14 s / 10 s render-thread stalls on pipeline creation first (54 pipelines over 1 s, up to 44 s, mesh and gmem
programs), then 12-15 fps (one 10 s window 21.5), no device loss; shots `shot_2_t170` .. `shot_5_t283`: Deku Tree, roots and textures
right, background trees dark-blue silhouettes (A15), white blobs on water/stones and over the tree's face (A14), no green blocks seen.
Capture of this build and cache: `work\captures61107-193137-base_1-base`. Pitfall: when a `screenshot` call times out during a
stall, the MCP server marks the pid as never answering and later calls fall back to PrintWindow (black); write
`<instance folder>\screenshot.<pid>` (one line: the PNG path) yourself or restart the server.
The cache the merge was tested with was removed with worktree quirky-panini-7286ba; kept from it: the frame capture
`work\captures\20261107-144949-a10-deku` (first scene, format 3, readable by the merged exe) and the code dump of the culling root in
`work\dumps\code` (4c1e8564 is a block from `main`, not in the corpus). Windows Defender quarantined corpus block
`5747cdd7457ef048.bin` as a false positive (user's decision; batch translations need a `unique.tsv` without it until then).

**Shader preparation after the A18 re-translation (2026-11-08, commit 33cd6b6, RTX 4080, alone):** `oot.exe --prepare-shaders --force`
with `NXRT_MEMORY_LIMIT_GB=24` and the three `NXRT_NVN_SHADER_*` paths (log `work\run\prep_a18`): **3 h 8 min** (11,275 s), 39,363 programs,
118,089 parts (49,671 under 2 ms from the driver's caches), 0 missing, 0 failed, footprint peak 15.6 GB; `pipeline_cache.bin` 4.33 GB (the
2.83 GB file of 2026-11-07 was loaded and kept, so stale entries stay in it). Slow because 12,896 programs count as expensive (gmem, large
SPIR-V; the A17 page-table code changed all of them after the last preparation): about 45 programs/min for the first 8,000, then the rest
in minutes. **`--prepare-shaders --check` said "ready" before it**: readiness counts usable programs, not changed SPIR-V, so after a
re-translation that keeps the number of ok rows, run `--force` (or delete `prepare.state`). A normal start afterwards (default run, no
`setup_on_start` override) opened no preparation window and loaded the cache (4.3 GB in 3.6 s); 3,500 parts from the preparation at
1.8 ms each, ~115 new.

**FPS baseline, first scene (2026-11-08, same build and cache, alone; `load_state("first_scene")`, `memory.limit_gb = 24`):** run
`work\run\fps_base1` (`[gfx] log_fps`, `[diag] soak_csv`): first scene frame took 8.0 s (13,300 draws, first replay), then no hitch over
0.6 s. Over 290 s in the scene: **average 11.6 fps, min 2.6, max 24.6, median 10.5**; per 30 s window 10.3, 9.9, 10.0, 10.4, 15.3, 14.0,
15.5, 15.2, 10.1, 5.9, 7.6 (the cutscene's later village shots are the heaviest: 5.9-7.6 fps). Frame times (soak, 5 s windows): median
p50 95.6 ms, median p99 114 ms, worst 471 ms. Working set 23-25 GB, private 21-22.7 GB. **CPU** (run `work\run\prof_base1`,
`[prof] sample_hz = 100` + `[prof] trace`; 120 s window: 13.6 fps, p50 80.6 ms): the render thread is the bottleneck, `replay` busy 99.7%
of the time (73 ms per frame); 60% of it in `begin_draw`, of which `bind_sets` -> `build_set` -> `new_servable`/`out_servable` (45%) with
`mem::ww::all_armed` 30% and `ww::page_valid` 11% self time (write-watch checks per descriptor), `held_or_pool` 20%, `prepare_textures` 7%,
`YmmLoop` (memcpy) 10%. Guest threads together ~0.9 cores, the game's main thread 0.08 cores (it waits on the GPU fences the render
thread feeds). **GPU** (run `work\run\gpu_base1`, `[prof] gpu_passes`, drains inflate the total: 64 ms per frame): 675 render passes per
frame at 854x480 (24.5 ms, the scene is split into passes every few draws), storage shadows in/out 15.7 ms (61 dispatches), "write memory"
7.9 ms (844 per frame), gaps between passes 7.1 ms (788), compute 5.4 ms (largest: the network pass fea98fe7 1.9 ms, 20593fd9 0.8,
beb1205f 0.6, culling root 0.5). First targets for FPS: the write-watch checks in `build_set` and the pass splitting.

## Boot progress (`oot.exe --game game\oot\updates\131072 --log work\run\bootN\oot.log --max-seconds 60`)

Each fix below moved the boot further; all are in the branch. Crash reports land next to the exe (`game-build\build\release\oot_crash.txt`,
`oot_freeze.txt` from the watchdog after 10 s without a presented frame, `oot_fatal.txt`; a named instance writes them into its own folder);
`tools\.venv\Scripts\python tools\ai\symbolize.py <crash.txt | addr...>` names guest frames from the modules' exported symbols (main exports
little: trust `main+off`, not the name next to it; sdk exports ~28,700 symbols, so sdk frames are named well). Logs of earlier boots:
`work\run\boot1` ... `bootN`. Use absolute paths for `--game` and `--log`.

1. SDK 22 mutex owner at TLS +0x110 (kernel-written thread handle): `runtime/src/kernel/thread.cpp` writes the handle there.
2. Used memory reported 64 MiB too high (HeapDef `Program` budget 137 MiB checked against `nn::os::QueryMemoryInfo`): `svc_info.cpp` adds
   the main thread stack instead.
3. NVN API minor version: OoT needs 231.40 (`api_device.cpp`, Pokopia needs >= 30).
4. `system_resource_size` 0x3000000 (main.npdm) enables the SDK's VAMM (`oot_core.cpp`).
5. `svcMapMemory` from MapPhysicalMemory pages (VAMM thread stacks; one vm block per 64 KiB granule) (`svc_memory.cpp`).
6. hid 3150 `InitializeMouse` (`hid.cpp`); IAudioDevice 19/20 auto-tune volume (`audio.cpp`).
7. Lifter: tier 1 decoded `PACIB x30, sp` as REV16 (ignored opcode2): `lift/a64.py` (10 sites in sdk, setjmp).
8. Temporary storage created on first open (`fs.cpp`, `temporary_storage_size` in TitleProfile).
9. New service `auddmg:u` (nn::audio::devmgr): session object, ListVirtualDeviceNames answers no devices (`audio.cpp`).
10. `nn::vi` layer setters answered on the host (`core/switch2.cpp`): the SDK's ones run through the stand-in layer's null internals.
11. `svcWaitForAddress` type 3 (WaitIfEqual64, `std::atomic_wait`) (`kernel_sync.cpp`).
12. Lifter: SDK 22 puts MOD0 after `.text`; discovery treated all of `.text` as NSO header and dropped every start named only by a BL,
    relocation or tail call (function-pointer targets were never lifted). `funcs.py` now treats only the two header words and the MOD0
    record as data, and the gap scanner ends functions at RETAA/RETAB too.
13. Lifter: static callbacks whose address is taken with a single ADR (OoT's printf-style formatter) were swallowed by the function
    before them. `funcs.py` adds ADR targets outside exact functions that follow RET/RETAA/RETAB/B (not BR: jump-table bases); 57 in main.
    Symptom of any such gap: `GUEST TRAP: indirect call to an address that is not a known function (0x71...)`.
14. gRPC (in main, Nintendo's online stack) probes IPv6 with a UDP `Socket` and makes its poll engine's wakeup an `EventFd` (bsd 31);
    with every bsd command failing it aborted ("no event engine"). `network.cpp` now has a descriptor table: sockets that exist and do
    nothing, Linux-style eventfds (Read/Write/Close/Fcntl) and a Poll that waits for a Write or the timeout.

15. nifm `IGeneralService` SDK 22 commands 100-104, 127, 128, 132 (`network.cpp`): newer versions of 7/5/8/9/14/27/28/32 with larger
    structures (GetCurrentNetworkProfile 101 = profile of 0x1b4 bytes instead of 0x17c). Read from the sdk proxy vtable (see Techniques).
16. Crash handler (`runtime/src/diag/crash.cpp`): one process-wide "in handler" flag made a second thread that faulted while the first
    wrote its report terminate the process with no report. Now other threads wait (the first report terminates anyway; 15 s cap), and a
    fault inside the handler on the same thread writes the report built so far plus both exception codes (no allocation).
17. **Fibers** (boot 21, ~8.6 s: `nn::async::Await` faulting with every register 0): OoT runs `nn::async` tasks on `nn::os` fibers. The
    SDK's `nn::os::detail::DispatchFiberContext` swaps guest stacks and returns into the other fiber, which a static recompiler cannot
    follow (host frames). `runtime/src/kernel/fiber.cpp` replaces it through an entry hook (site in `recomp/games/oot/sdk.sites`,
    registered by symbol in `fibers_install()`, called from `game_main.cpp`): one host fiber (own host stack) and one Ctx per guest fiber;
    TPIDR/TPIDRRO come from the resuming thread. Known limits: host thread_locals that a function caches across a fiber switch (HLE
    thunk name, catch_call frames, diag ctx) see the wrong thread if a fiber migrates; a finished fiber's host stack is freed only when
    its FiberType is reused.

18. **Guest setjmp/longjmp** (Link's house, ~300 s into the first scene, while a dialogue box rendered: `GUEST TRAP: indirect call to an
    address that is not a known function (0x7101661690)`): FreeType's rasterizer in main (setjmp caller main+0x1661670) leaves a full
    cell pool with `longjmp(env, 1)` and retries; the guest longjmp branches to setjmp's return address, mid-function. The lifter now
    makes every `bl setjmp` return address a landing pad and its function a catcher (`Env.add_setjmp_pads`: main 3, sdk 10), and
    `dispatch_call` hands such a target to `eh::longjmp_landing`, which resumes the innermost catch frame whose CFA lies above the
    restored SP with a host longjmp (`runtime/src/eh/catch_call.cpp`; test `eh.guest_longjmp_resumes_the_setjmp_caller_at_its_pad`).

**Manifest bug (fixed for OoT, still in pkp):** the game CMakeLists cached the sites file in one variable for every module, so a second
module with `sites =` silently got main's file. OoT now uses `OOT_SITES_<MODULE>`.

What the game does by boot 20: rtld -> sdk init -> nnMain; RomFS mounted (186,808 files); account save created; 12 GiB heap; NV/vi
graphics on the host (60 Hz vsync); NVN back end up on the Arc GPU; ~67 shader programs set; loader threads read event settings, sound
banks, actor and effect packs; hid, account, nifm, prepo, ssl, nsd, bsd used; audio renderer running.

Not done yet: shader translation (draws are skipped: "no translated shader"), no frame is presented, the game names no frame-update hook.

Open points besides the milestones:
- **NVN gaps**: unknown procs get logging stubs (nvnDeviceGetFlags, nvnQueueGetTotalRayTracingMemoryUsed,
  nvnQueueBuilder{Set,Get}RayTracingMemorySize, nvnQueueBuilderGetPrimaryQueue, nvnWindow(Builder){Set,Get}AcquireTimeout,
  nvnProgramBuilder*RayTracing*, nvnTexturePool/SamplerPoolWriteDescriptorTableData, nvnMultisampleState*CoverageTruncation*,
  *GetSampleLocations, nvnCommandBufferSetComputeWorkloadPriority; ~140 lines per boot); "GPU-side WaitEvent not implemented".
  Implement what OoT depends on (NVN 231.31-231.40 additions).
- **auddmg** session answers only ListVirtualDeviceNames; microphone capture (the remake's voice/ocarina features) later.
- **nn::vi queries** (QueryDisplayHdrStatus/VrrSupported/HdrPaperWhite/CurrentRefreshRate) return the SDK's 0x272 for the stand-in display.
- **Movies** need libvpx (vcpkg `libvpx:x64-windows-static`, `NXRT_LIBVPX_ROOT`); not installed.
- **Launcher** (`runtime/apps/launcher`, mpr's, not built for OoT) accepts an extracted folder; the Initial Setup (`oot_setup.exe`) takes `.nup` (through `nup_extract.py`) or an extracted folder. Encrypted NSP/XCI are not supported (removed 2026-11-08: no code reads keys or decrypts).
- **Pokopia regression risk** (no Pokopia dump here): shared changes in funcs.py (header/MOD0, ADR starts, RETAA/RETAB, zero FDEs), a64.py
  (PAC decode, DMB LD/ST as compiler barriers, single-store scalar FP writes), tier2 FP16, `nx::fmax_`/`fmin_` NaN order, lifted-code
  flags (/O2, x86-64-v3, forced inlining), used-memory accounting, NVN minor 40, TLS +0x110, MapMemory Alias sources, WaitIfEqual64, bsd descriptors.
  Oracles, from `recomp\`: `..\tools\.venv\Scripts\python oracle/difftest/difftest.py all`, `oracle/e2e/run_e2e.py`,
  `oracle/hooks/test_hooks.py`; runtime tests `runtime\scripts\build.ps1 -Test` (all 170 groups pass, see "Runtime tests" below).

## Artifacts (the user wants every one fixed; add each one seen, mark it fixed with the cause)

| # | Where | What | State |
|---|---|---|---|
| A1 | title, intro | UI text invisible, then garbled. Two causes: the glyph pass picks its glyph records with instance + c[0][0x644] (NVN base instance, never written by the runtime; the harness now answers QueryHasConstantBufferDrawParameters true, so Ryujinx reads gl_BaseInstance), and the menu text is CPU-rendered into R8 textures with NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET, which the layout code untiled as block-linear (64-texel bands; found by re-tiling a dump and decoding it pitch-linear: "New Game"). Both fixed; the title reads "New Game" and "Options / ZELDA NOTES Setting" | fixed |
| A2 | title | black background: the title is a movie. Fixed: AV1 on dav1d (below) and the NDVE model hidden | fixed |
| A3 | title | faint vertical stripes in the logo/smoke area (x 340-940) over the black background (when the title movie is not behind it; with `[media] video_max_pictures` the movie ends and they show again on 2026-11-06) **Cause (2026-11-08):** the BC7 transcode of the title's shield-and-sword sprite (t1503, RGBA ASTC 4x4 sRGB 1664x1280, fs 9b0b4805, draw 5 of the title frame): the sprite has colour under alpha 0 (mean RGB 187/172/138 there), and bc7e left alpha 1..4 in 5,795 of its 71,831 fully transparent blocks; drawn with straight alpha over black those blocks are the faint vertical stripes (they never show over the movie). Found on capture `20261108-000706-a3t-title_stripes` with `skip_fs_file` (6977e9b1 movie, 996457c2 smoke, then 9b0b4805) and `[gfx] astc_quality = uncompressed` (stripes gone). **Fix:** `tools/textures/src/a2b.cpp` pins every fully transparent source block to alpha exactly 0 after encoding (`zero_alpha`: modes 4/5 alpha slot incl. rotation, modes 6/7 endpoints and p-bits), and BC7 entries read from `bc7_cache` (encoded before) get alpha 0 in blocks that can only decode to alpha <= 4 (`a2b_bc7_zero_faint_alpha`, nvn_texdata.cpp `cache_read`). Checked on the replay and in game (run `work/ai_run/oot/a3t`, `work/run/a3/{t5,fixed_bc7,game_after2}.png`: stripes before, none after) | fixed |
| A4 | title -> A | the screen stayed black after A: the game waited for the title movie (no video decoder). Fixed with A2; A now leads into the intro (tapestry movie) | fixed |
| A5 | setup window | blurry (DPI-virtualised) | fixed in code (per-monitor DPI), not yet seen |
| A6 | all | Opus audio was silent: `hwopus` decoded silence. Now libopus 1.6.1 (vcpkg `opus:x64-windows-static`, repo-local); the Ex open forms carry {rate, channels} in the raw data (OoT: 48 kHz mono, was read from a buffer: 0/0 and "buffer too small") | fixed (not listened to) |
| A7 | game (expected) | shadows of clustered (point/spot) lights are missing: 4,336 fragment shaders sample their shadow maps with TEX.SCR.B.DC through a handle read per pixel (light index from a storage loop), which the handle expressions cannot bind; the sample answers 1 (lit). Fix: pool-sized texture + sampler arrays in the runtime (Ryujinx GenerateBindlessAccess, harness QueryHostSupportsSeparateSampler true) **Implemented 2026-11-08 (pool arrays):** reference ports first: jaigner-hub's docs/bindless.md does the same (GenerateBindlessAccess, separate sampler array, pool-sized arrays, update-after-bind set), featureJosh/pokopia-recomp has no per-pixel bindless. Harness: `QueryHostSupportsSeparateSampler` true, so a handle that neither the handle expressions nor TryConvertBindless resolve per draw becomes an index into a 65,536-entry sampled-image array (texture id) and a 4,096-entry sampler array (sampler id), each in an extra descriptor set (fragment: sets 7 and 8; `tools/shaders/README.md`). Blocks whose handle resolves per draw keep their plain binding (the resource lists of all 4,335 changed blocks are otherwise identical). Runtime (`nvn_pool_sets.cpp`, `pool_set_sync` in nvn_replay.cpp): one persistent partially bound, update-after-bind set per (pool, array kind), all entries stand-ins at creation (depth 1.0: a comparison answers lit), then once per present the pool's depth textures that have an image on the GPU (or share one) get their views, the samplers their VkSamplers; a draw puts those textures in the sampled layout first (173 transitions in the first scene's frame, one pass break). `[nvn] pool_arrays = 0` keeps the textures stand-ins. Checked on capture `20261108-000515-a9g1-village_house`: the light records' handles (`[nvn] trace_pool_handles`) resolve to the game's DEPTH32F 384x384 / 1920x1920 shadow maps with sampler 422 (compare mode on, LEQUAL); lit foliage changes against the stand-ins (`work/run/a7/house_diff2.png`, `kid_diff.png`). Game fps of the first scene: 14.6 with the arrays, 11.4 without (warm runs, `work/ai_run/oot/a7g`), so no cost. 4,335 blocks re-translated (the subset route; one corpus block is quarantined). Not yet seen: a scene at night with torches. | fixed (night scene not checked) |
| A8 | first scene | **VK_ERROR_DEVICE_LOST** 1-7 s after the first gameplay frame, every full run (2026-11-06), on the Arc iGPU and the RTX 4080. **Root cause: endless loops that Ryujinx's goto elimination built into ~14,700 translated pixel shaders.** The faulting draws used fs `dcfaa938040cb593` (then, skipped or capped, `8ba27a98e540767c`, then `ab5959d4f145c6fd`): a material/lighting family with a light loop (0x39d0-0x64d0, back edge `BRA 0x39d0`, exit `@P1 CALL.REL.NOINC 0x64e0`) and a jump-table switch inside it (`BRX R32 -0x4e90`, targets from c1; every case jumps to the switch end 0x60e0). `GotoElimination` works through the gotos last to first: the loop exit became `if (exit) break;` first, then the switch's gotos wrapped the statements around it in new `do { } while (flag)` loops (EncloseDoWhile / Lift: the six `while (temp_736)` and the `while (temp_879 \|\| temp_880 \|\| temp_879)` loops), so the break left only the innermost new loop and the light loop's `while (true)` never ended (GLSL: `if (temp_991) break;` inside the inner loop). **Fixed** in `tools/shaders/harness/translator/structured-goto-loop-exits.patch`: a new do-while forwards the breaks/continues of outer loops it encloses through a flag (`if (flag) { flag = false; break; }` after it; the flag is cleared at the function start, because a later goto may guard the statements before the loop). **Offline check** (`NXRT_HARNESS_CFG_CHECK=N`, `translator/StructuredCfgCheck.cs`; `tools/shaders/README.md`): random walks through the structured code, every block entry checked against the IR block graph. The three shaders fail at the first walk without the fix (after the exit block the code returns to the loop head) and pass 10,000 walks with it, every block and edge reached; the whole corpus (100 walks) has 14,684 failing blocks before and none after. The fix changes the SPIR-V of 14,692 corpus blocks (all pixel shaders; the 8 not in the failing list fail after 101-107 walks; resources and statuses unchanged); 17,094 blocks contain a BRX/BRXU or CALL.REL.NOINC (16,610 / 16,944). **On the RTX 4080** (cache translated with the fix, `.claude/worktrees/busy-hamilton-0e62aa/work/shaders/oot_a8fix`, not prepared, no loop cap; run `work/run/a8fix1` there): the first scene (13,040 draws) came at 62 s and ran to the 300 s deadline with no device loss (13-22 fps while ~15,000 changed pipelines built in game); the screen stays black there, as in the capped runs (separate issue). Control in the same setup with only the three shaders' old SPIR-V put back (run `work/run/a8ctl1`): scene at 64.6 s, device lost at 68.7 s in draw 12,607 (vs `f16b4a89c5478c05`, fs `dcfaa938040cb593`, the original A8 draw), exit code 4. Before the root cause was found, ruled out one run each: compute, the handle-expression and other new fragment shaders as a group, memory pressure, texture upgrades, the allocation count; `[gfx] device_diagnostics = 1` (checkpoints, fault report, NVIDIA event 153) named the draw. `NXRT_HARNESS_LOOP_CAP=N` (`spirv-else-if.patch`): every loop of a shader leaves after N back edges per invocation. **Since 2026-11-06 on by default for vertex/fragment shaders (4096; compute off; `=0` turns it off)**: a second runaway loop in the same material family (fs `253f607ff5823bd7`, named by device checkpoints in run `gp_3`) took the GPU to ~2 s frames / TDR / device lost 1-3 minutes into the intro scene (runs gp_1..gp_3 lost the device at 127-246 s; with the cap no loss in 420-450 s). Which input drives that loop is still open (the light-cluster grid was ruled out: counts 1-3, offsets reset each frame). To use the fix in `work\shaders\oot\spirv`: rebuild the harness, translate the corpus (or the changed blocks via the subset route) and merge, then prepare again (delete `prepare.state`: the number of usable programs does not change). **Done in the main checkout on 2026-11-06** (branch merged into `oot-nup`, corpus re-translated, 23-minute preparation with both depth-clamp variants, 2.34 GB cache): the first scene ran 3 minutes without a device loss and without the loop cap (`work\run\verify_merged`); the loop-capped test cache `work\shaders\oot_cap` is deleted | fixed |
| A9 | game | foliage/terrain use **mesh shaders** (NVN2 task stage 6 / mesh stage 7, `nvnCommandBuffer(Multi)DrawMeshTasks*`). **Implemented 2026-11-06 (worktree quirky-panini-7286ba, branch claude/charming-brahmagupta-90017e), partly verified**: the translator turns task/mesh blocks into VK_EXT_mesh_shader stages (`tools/shaders/ampere/frontend/AmpereMesh.cs`, `AmpereEmit.Mesh.cs`; ISBE semantics as in the Lichii emulator's notes; 770 of 800 mesh and 15 of 15 task blocks translate; limits from control +0x58 threads / +0x5a task payload bytes / +0x60 max vertices / +0x62 max primitives, output lines from the SPH OMAP, checked against the paired fragment shaders' IMAP), the runtime binds them as NVN stages 6/7 (stage bits 0x40/0x80; c[0] storage descriptors at 0x650 / 0x750, four slots past the other stages' pattern, measured from the task's pointer table), builds whole mesh pipelines (`[gfx] mesh_shader`) and replays the eight draw entry points; indirect records stay on the GPU (3D = {x, y, z}; plain = {count, first}, inferred from VK_NV_mesh_shader, copied into {count, 1, 1}). Also new in the translator: `F2IP.U8/S8.F32` (float pair to packed bytes; the culling compute f2422a7a and 3 mesh blocks). Seen (runs `work/run/mesh_8`, `mesh_9`): mesh draws rasterize in the depth prepass and the shadow cascades (a ground strip, leaf clusters at the top left); the GPU-culled colour-pass mesh draws still add nothing in a traced frame, and one run showed a bright band on the ground that baseline runs do not have (not explained yet). The picture of the leaves and the Deku Tree's nose is unchanged. Left: why the colour-pass indirect draws stay empty (record layout or culling inputs), 17 mesh blocks with OMAP system values A, 4 with colour/clip outputs, CALL forms, per-primitive outputs, `first` != 0 **2026-11-07 (runs `work\ai_run\oot\leaf1..leaf8`): the mesh stage's storage descriptors start at c[0] 0x770, not 0x750** (task stays 0x650). The depth-prepass mesh a24d8406 reads the GPU-written record buffer (count at word 3) at 0x770 where the game binds it at mesh slot 0; the colour-pass mesh b8b873e5 reads it at 0x790 where the game binds it at slot 2, and every other slot of both fits the same way. With 0x750 the colour-pass meshes read two unbound slots (dummy data): garbage vertices, which the depth EQUAL test against the prepass rejected, so they "drew nothing" (with `[nvn] debug_equal_as = 4` they showed as huge streaks). Fixed in `storage_c0_base` / `storage_slot_for` (nvn_replay.cpp), `set_c0_offset` (nvn_shaders.cpp) and the harness's gmem candidates (`StaticGpuAccessor.GlobalFallbackWords`); the 770 translatable mesh blocks were re-translated. Since then the colour-pass ground meshes fill the holes in the replay and write pixels in the game. Mesh pipelines take 86-124 s each in the NVIDIA driver the first time (on demand). **Leftovers checked against the game (2026-11-08):** a 10-minute run from `first_scene` through the intro into the Kokiri village (`work/ai_run/oot/a9g1`, `work/run/a9/gaps_g1.txt`) hits none of them: no mesh draw was skipped or drawn without a stage, no `first != 0`, no mesh/task block failed at a draw. Two untranslated mesh blocks are only set at load (program creation at 12 s), never drawn: 87517404 (OMAP system values B 0xf8: primitive id / layer / viewport / point size) and 7289a62f (CALL.REL.NOINC). The bright band on the ground was not seen (shots `work/run/a9/g1_shot1..7.png`). Left as is until a scene draws with them | fixed for the scenes seen |
| A10 | game | **compute programs read their resources through a per-program c[0] table, and OoT's post-process chain has a push constant area in it.** Layout (`apply_nvn2_compute_table`, `nvn_shaders.cpp`): 0x10-byte header, push constants, 0x10 per uniform block descriptor, 0x10 per storage buffer descriptor, 8 per texture handle, 8 per image handle, slot n = NVN binding n; counts at control +0xb1/+0xb2/+0xb3/+0xb6, table size u16 +0xba. The bfsha/vsetb corpus (3,741 of 3,743 OoT compute blocks) has no push area and already mapped. The chain's programs built into `main` (f05596b9, 54ecc4b4, 4c60af1f, fea98fe7, f90d726d) have **0x80 bytes of push constants** after the header (= table size minus header and slots; NDVE 0x40; control byte +0xa9 = push words read, 29), so every slot was 0x80 off. Confirmed in a frame trace: the game calls `nvnCommandBufferPushComputeConstants(stage 0, offset 0, 0x74 bytes)` before f05596b9 (words: 1/854, 1/480, 854, 480, 1/214, 1/120, 214, 120 ..., 853, 479) and f05596b9's table 0x158 = 0x10 + 0x80 + 10 ssbos + 4 textures + 1 image. Runtime: compute pushes carry `kPushCompute` and land in `SmallState::push_cs`; a program with a push area gets a per-dispatch c[0] in the upload ring (header 0, the pushed words, descriptors {address, size}, handles; `compute_c0` in `nvn_replay.cpp`). Traces print `push compute ...` and `cs c0 table ...` per dispatch. **Fixed: every resource of the chain resolves** (run `work\run\a10_8` in worktree quirky-panini-7286ba, `[nvn] upscaler_replace = off`): f05596b9 scene texture slot 3 -> RG16UI tile image slot 0, ssbos 8 (200 KB) and 9 (its 16 bytes are 54ecc4b4's indirect dispatch arguments); 54ecc4b4 ubo 10 as storage, ssbo 8, textures 4 and 6, image 1; 4c60af1f textures 3 (scene) and 5 -> image 2 (RGB9E5 composite); fea98fe7 weights ssbo 2 (12 KB fp16), texture 0 = the composite -> image 1 (R11G11B10F 2560x1440). Needed on the way: **RGB9E5 storage** (the shader packs RGB9E5 and stores the word through an r32ui image; the image now gets MUTABLE + EXTENDED_USAGE and an R32_UINT storage view, `uint_alias`); **buffers in a virtual pool** (ssbo 8 is at 0x200000382000 in a sparse pool: bound through the physical memory its `nvnMemoryPoolMapVirtual` mappings name, `physical_buffer_address`, at bind replay; before, the dummy buffer); **slow compute pipelines** (fea98fe7, 4.7 MB of SPIR-V, took 116 s in the NVIDIA driver on the render thread: programs over `[nvn] compute_background_spirv_kb` build on their own thread, dispatches left out until ready; the driver cache makes later runs instant); the translator's **TLD4 .B** (blue component with a bound handle, not bindless) and **TLD4 .F16(.RN)** (f90d726d translates; 25 corpus blocks failed the same way and should translate after a subset re-translate); trace dumps of buffers in a virtual pool crashed the render thread (`host_readable`). `[nvn] skip_unmapped_dispatch` stays as the fallback for a storage descriptor outside the table (none seen now). `[nvn] upscaler_replace = off` turns the OoT stopgap off (an empty environment variable does not exist on Windows) | fixed |
| A11 | intro movie | voice lines overlap (user, 2026-11-06), then the movie too slow against its audio. **Two runtime causes, both fixed (2026-11-06):** (1) `[display] unpaced_loading` (Pokopia's heuristic: light frames = loading screen, limiter off for up to 20 s and for 1.5 s after each stall) took the intro's 3-draw movie frames for loading: the game ran at 300-500 fps and the movie player, which advances one picture per frame, raced through ~250 pictures/s while the voice lines (Opus, started along the movie's timeline) piled up. Now frames stay paced while the game reads decoded pictures (`note_frame_for_pacing`, log `pacing on: movie`). (2) `fps_limit = auto` capped the game at the display's refresh rate, and the user watches through Remote Desktop (Microsoft Remote Display Adapter, 32 Hz): 32 fps, and OoT's **60 fps** movies (`Dm_Kokiri_0000.webm` is 154 s, 9,254 pictures, DefaultDuration 16.67 ms) played at half speed (291 s). OoT has no timestep sites, so its step is a fixed 1/60 s per frame: auto now means `[display] console_fps` (60) for such games and the display's rate only for games with timestep sites (`apply_frame_cap`, log line `play: frame cap`). Checked: intro 157 s at 60.0 fps. Also the stand-in vsync event (`vi_nv.cpp`) now runs on an absolute high-resolution 60 Hz schedule (a relative 16.667 ms wait became 16 ms ticks on Windows). The ~30 s gaps between music tracks the user heard were probably the same half-speed game clock | fixed |
| A12 | first scene | **3D scene black** (only subtitles and UI; seen once A8 was out of the way with the loop-capped cache). The scene renders fine at a low internal size (854x480 or 960x540: G-buffer, lighting, HDR targets all have content in a `[nvn] trace_sec` frame trace with targets), but the 2560x1440 HDR target `t486` that the final composite (fs `c63c3b8f`) samples is only cleared and blended with a solid-colour overlay (fs `5223ec5a`, a fade). The game's upscaler fills it: a chain of compute passes, all broken here: `f05596b9` (storage at c[0] 0x110/0x120 unmapped, A10, skipped), `54ecc4b4` (TLD4.SCR.R/G.PTP: not translated), `4c60af1f` (runs, but its textures and output image come from c[0] words, A10: nothing bound), `fea98fe7` / `f90d726d` (the tensor-core network, HMMA plus TEX.SCR.F16.RN: not translated). The compute passes get their textures and their output **image** from per-program c[0] words; no image is bound through nvnCommandBufferBindImages. Translator fixed for TEX .F16(.RN) (Rd = packHalf2x16(c0, c1), Rd2 = (c2, c3); confirmed by the shader's use of the halves) and TLD4.PTP (Maxwell layout: 8 signed 6-bit offsets in 8-bit lanes of two Rb registers; textureGatherOffsets); `fea98fe7` translates to 4.8 MB of SPIR-V. Instead of emulating the network, the upscaler is **replaced** on the PC (user's decision): `[nvn] upscaler_replace/skip/src/dst` (OoT defaults in `runtime/games/oot/oot_data.cpp` through the new `game::Info::config_defaults`): the chain's first pass becomes a scaled copy of the scene into the texture the composite samples (`upscaler_src = depth_rt`: colour target 0 of the latest draw with depth; `upscaler_dst = fs:c63c3b8f:tex0`, taken from the previous frame's last composite), the other passes are left out. Untranslated blocks from `main` can be dumped with `[nvn] dump_missing` (`code_<hash>.bin` + `ctrl_<hash>.bin`; compute code starts at byte 0). **Result: the scene reaches the screen but washed out white** (`work\run\upscale3`): the chain is the game's whole final post-process, not only an upscaler. The composite `c63c3b8f` has one texture and no uniforms (it only converts to 8 bits), so the chain writes an exposed, tone-mapped, composited image; the latest depth draw's target is a near-white layer (values 0.25-1, mean 0.98: haze or fog), the HDR scene is another target (`t10348`: up to 25, mean ~1), and the network pass reads several of these. A faithful replacement would have to redo exposure, tone mapping and compositing; the proper fix is running the chain, which needs A10 (session "Implement the NVN2 compute c[0] resource table", 2026-11-06). The stopgap stays until then; switch it off with `NXRT_NVN_UPSCALER_REPLACE=off`. **With A10 fixed (2026-11-06) the whole chain runs, and the picture is still washed out white** (`a10_8`): so the brightness is not the stopgap's doing. Replacing only the network pass (`upscaler_replace = fea98fe7`, `upscaler_skip = none`, `upscaler_src = tex0`, `upscaler_dst = img1`: a scaled copy of the game's own RGB9E5 composite) gives the stopgap's picture with detail (`a10_9`): the overexposure/haze is already in the scene and composite (exposure source, fog or lighting: next to look at), and the network's own output loses detail against that copy (translation of fea98fe7's HMMA / FP16 code suspect, unverified). The OoT default is unchanged. **Checked in the main checkout after the merge** (2026-11-06, run `work\ai_run\oot\verify`, stopgap off, the 196 previously failing corpus blocks re-translated with the TLD4 .B/.F16 fix: 12 more ok, cache prepared again in 57 s): no device loss, rock detail visible, picture still overexposed white, 14 fps **Overexposure traced (2026-11-06, worktree quirky-panini-7286ba, runs `work\run\a12_*`):** the frame is a forward renderer (scene in an 854x480 R11G11B10F target), then bloom (fs caf7eb45 bright-pass: luma (0.299, 0.587, 0.114) + 0.5 * alpha, strength 0.15, luminance cap 10; 752b2104 downsample, cc8d0787 / 3f0dc2b1 5-tap blurs, cdaf772f upsample), then **the tone mapping is fs ce217436** (`ACES((max(0, M * scene) + pow(bloom, c3[3].y = 1.5)) * c3[3].z)`, M about identity, exposure **c3[3].z = 1.0 in every traced frame**), then the compute chain. Not a cause, despite appearances: the trace's `clear ... rect` is taken before the clear's pass opens and clamped to the previous pass's extent (the 854x480 tone-mapping target showed 240x135); the new `clear applied` trace line shows every clear covers its whole target (run a12_12). Measured with raw dumps (snapshots now write `.bin` for float formats; decode B10G11R11 yourself: the stats line the runtime prints for such snapshots is unreliable, one channel reads +-1e30): sunlit ground (16.5, 12.5, 8.5), sky (0.6, 1.1, 2.4), so the scene is physically scaled and needs an exposure of about 0.05; the bright-pass output sits at its cap for most pixels and the final 240x135 bloom has a mean of 4.5. Ruled out, one run each: fog passes (cfc67237 ... 69d4ca92 change the scene by under 10%), light-loop over-iteration (the frame's 24 material shaders re-translated with `NXRT_HARNESS_LOOP_CAP=6`: same picture), storage bindings smaller than what shaders read (`[nvn] storage_min_range = 4096`: same), a missed compute readback (`[nvn] compute_out_lazy = false`: same), the network pass (a plain copy instead of fea98fe7 is as white). **Cause found (same day): the graphics stages' storage buffer slots are swapped in pairs.** The material shaders read 96-byte light records (type at +16) through fragment c[0] 0x520 and a cluster index through 0x510, while the game binds the light list at slot 0 and the 9.9 MB GPU-built cluster grid at slot 1; likewise 0x530 is the sky lighting (spherical harmonics that compute 16a42858 builds from the sky panorama, slot 3) and 0x540 a 64-byte per-material table (slot 2). `[nvn] graphics_ssbo_slot_xor = 1` (OoT default; slot n at base + 0x10 * (n ^ 1), vertex and fragment: fragment alone left the scene white) makes the first scene render with its intended look with the stopgap off: Great Deku Tree, forest, ground textures, sky (runs a12_18, a12_20, a12_21 in worktree quirky-panini-7286ba). How the game found it: `[nvn] ubo_backtrace` (guest backtrace of a uniform bind by contents) showed the tone mapping's exposure is a constant 1.0 by design, so the scene itself had to be wrong. **Still open:** the upward view of the tree against the sky is washed out white in some frames and right in others (sun bloom or exposure adaptation; same camera view, both seen), and one of three runs past 115 s lost the device (instruction-pointer fault after a 4 s GPU stall, the A8 signature; a run with `[gfx] device_diagnostics = 1` did not reproduce it). Also answered on the host: nn::vi QueryDisplayHdrStatus / HdrPaperWhite / VrrSupported as an SDR display (no visible effect). **Corrected later the same day (worktree quirky-panini-7286ba, runs a12_25..a12_32, wb_2, wb_3): the slot swap was wrong.** The real cause of the white frames was the translator: OoT's sky-lighting compute 16a42858 sums spherical harmonics with RED.E.ADD.F32, which was emitted as an integer add of float bits (garbage like 1.8e29 in the SH buffer, NaN in every material; fixed in AmpereEmit.cs, CAS loop). The swap only steered the SH read (c[0] 0x530) away from that garbage, and it fed the material shaders the light list where they expect the cluster grid (0x510/0x520): NaN, the three per-draw lightmap textures unresolved (0x540 pointed at the 192-byte SH buffer instead of the 5.7 MB material table) and a runaway light loop (2 s frames / device loss). With the float atomics fixed and the plain mapping (`graphics_ssbo_slot_xor` back to 0, no longer an OoT default) the first scene has no NaN, the lightmaps resolve and it renders as intended from the first frame (wb_3). Found with `[nvn] trace_snap_ssbo` + `tools/ai/gpu_snapshots.py`. Still to check: a bright patch near the water, the green glow, the inset rectangle. **2026-11-07 (late):** the bright patch near the water was not water: see A14 (texture aliasing in virtual pools). The green glow is A13. The inset rectangle was not seen in 14 game shots and 3 replays of the first scene (runs `work\ai_run\oot\nb1`, `nb3`): no description of where it was is left, so it stays open until it is seen again.  **Upscaler stopgap retired (2026-11-08):** on capture `20261107-230737-nb3-alias_kid` the game's own chain (stopgap off) gives the right picture: same brightness and colour as the copy (mean RGB 140.8/130.9/99.4 vs 141.2/131.3/100.1), sharper and anti-aliased (mean luma gradient 1.98 vs 1.34; jagged edges on Link's hat and arms with the copy), so the network pass (HMMA/FP16) is right; it costs ~2 ms of GPU time per frame on the RTX 4080 (last submission 164.1 vs 162.0 ms in the profiled replay, `work/run/ups_cmp`). The chain is the OoT default now; `[nvn] upscaler_fallback = 1` (OoT default) lets the scaled copy stand in only while a pass cannot run (the network pipeline fea98fe7, 4.7 MB of SPIR-V, takes 2-3 minutes to build the first time; the scene was black meanwhile). Screenshots `work/run/ups_cmp/{stopgap_on,chain_on,fallback_chain,crop_link}.png`. `[prof] gpu_passes` misses the chain's dispatches (no `cs` stamps for them; open); its stamp cap per submission went from 1,024 to 8,192. | open |
| A13 | first scene | **Green blocky glow** on the ground/water (user, 2026-11-06). Measured (run lf_1, scene snapshots every 25 draws, `tools/ai/gpu_snapshots.py`): between draws 575 and 600 ~3,000 HDR pixels get **NaN in the G channel only** (R11G11B10F words like 0x4e3ffa47: G field 0x7ff, R and B normal) over a normal value; bloom spreads them and the tone mapping shows pure green blocks. Not from fs 0f3f9223, c1f31e29, 33d8129d, 73e29464, 76ea7f02 or 253f607f (each skipped with `[nvn] skip_fs_file`, green stayed). The view changes between runs, so the next step needs a capture of the exact frame (snapshot names per frame, or a NaN check per draw). **2026-11-07 (late): not seen again** in runs base_1, nb1 and nb3 (first scene to the Kokiri village, 14 shots). The cause is not proven: the HDR NaN could come from the shaders that read the scene depth through a second texture object, which sampled zeros until the virtual-pool alias fix (A14); reopen with a capture if the green blocks come back. | not reproduced |
| A14 | first scene | **White blobs** on the Deku Tree top and on water/stones, flashing (user: not the sun). With A12 fixed the HDR scene's median is ~2.5, max ~19, and the bloom input is sane in calm frames (wb_3); the blobs come and go with the view (lf_4 shot 4: water and stones). Likely the same family as A13 (non-finite or huge values in some blended draw). **Fixed 2026-11-07 (late).** Two causes. (1) The blobs on the water and stones were holes in the ground showing the sky (and the sun) through bloom: the stream bed is a heightmap terrain (TerrainGround, vs adedb129 / fs 73e29464, blended layer, depth test EQUAL against its own prepass 8880001616dd97c0). Its fragment shader fades by the scene depth, which it samples from texture t19533; the game renders that depth into t19625, another texture object over **the same virtual-pool range** (same pool, offset, format, size). The runtime shared one image between such twins only for textures with host storage (`texture_registry_find_alias` keyed by `mem`), so t19533 was a separate, never written image (all 0), the shader got alpha 0 and nothing covered the sky. Now textures in a virtual pool alias by pool + offset + layout (nvn_texture_registry.cpp); in the first scene ~20 render targets share their images this way. Found by snapshotting draws 225-1112 of the capture `work\captures61107-223924-nb1-water_patch` (`[nvn] trace_snap` takes ranges now) and tracing which prepass draw wrote the hole's depth. Before: `work\ai_run\oot\nb1\shots\shot_223930_889.png`, `shot_224243_262.png`, base_1 `shot_2_t170`/`shot_4_t242`; after (game): `work\ai_run\oot\nb3\shots\shot_230304_996.png`, `shot_230338_724.png`, `shot_230729_527.png`, `work\ai_run\oot\nb4\shots\shot_232802_697.png` (the view of base_1 `shot_4_t242`; nb4 = final build, 12-14 fps as before); the fog volume and god rays, which read the same depth, changed with it. (2) The blob over the Deku Tree's face (base_1 `shot_5_t283`) was the missing nose showing the sky (A15, fixed in 3052f92). Capture `work\captures61107-230737-nb3-alias_kid` (this fix): replay SSIM 0.956 against its reference. | fixed |
| A15 | first scene | **Leaves missing / Deku Tree's nose missing** (user: dark-blue blobs where leafy trees should be, sky visible where the nose should be). The game calls `nvnCommandBufferDrawMeshTasks(Indirect)` (stubs, A9: nothing drawn), and the two-sided foliage-like draws (fs 0f3f9223) sample three 1x1 textures (ASTC 5x5, DXT1, RGTC1). Mesh draws are implemented (A9) and draw in depth; the leaves and nose still do not show in colour (A9: culling compute). **2026-11-07:** the GPU culling 4c1e8564 feeds the mesh draws now (A17 fixes: CAS for min/max on host memory, 16-step pool-table search, stale .spv replaced, task loop cap). On replays of captures 141109/144949 the chain counts and the mesh draws get work (leafy crowns visible in the replay); in the game at 150-220 s the records still read x = 0 and the trees stay silhouettes: the game and the replay disagree here (open). **Later 2026-11-07 (measured, runs `work\run\gpt_3`, `gpt_4_nocoh`, capture `20261107-193137-base_1-base`): the culling is no longer the cause.** With the re-translated cache (RemoveNode fix) every record's word 0 is non-zero in the game at 168-174 s and on the replay (0x1c/0x21, 1, 8/0xa, 6/0x20, 6/7 groups, counts 0x18-0x21), the 11 indirect dispatches get 33, 1, 7, 10, 0, 2, 38, 0, 0, 32, 1 groups (`snapd_*`; the zeros have count 0) and the colour-pass mesh draws get 1-15 groups (`snapm_*`), with and without `[nvn] gmem_coherence`. The nose is still sky and the background trees still dark-blue silhouettes (`gpt3_t150.png`, same view as base_1's `shot_2_t170`). On the replay, leaving out the 7 colour-pass mesh fragment shaders (bb9b2ea9 fbfa1738 4097c13f 3abc0b54 957527e8 2972adf5 1e03316b, `skip_fs_file`) changes no pixel: those draws reach the colour target with nothing visible. Next: why (A9 leftovers: mesh outputs / OMAP system values, depth against the copied prepass depth t128607, the draws' per-primitive data), or which other draw owns the nose. **2026-11-07, later (game runs in `work\ai_run\oot\leaf8`, snapshots every 3-5 draws, `[nvn] debug_equal_as` = 4 / 8 to look past the EQUAL test): three causes.** (1) A9's mesh slot base (0x750 -> 0x770), see A9. (2) **The nose: LDC.64 with the destination pair over its index register** (`LDC.64 R58, c[0x6][R58+0x28]`, the bone palette read of OoT's skinning vertex shaders, e.g. e132ae05): the Ampere frontend loaded the second word from the already overwritten R58 (`EmitLdcAmpere`, now loads both words first). The face's skinned nose got one garbage translation term and stretched into streaks off screen; EQUAL hid it, so the sky showed through. 314 corpus blocks change (289 vertex, 21 mesh, 4 fragment); the nose is back in the game (`work\run\leaf_glsl\ingame_nose_fixed_t135.png`). (3) **Still open: the crowns.** The prepass draws the leaf cut-outs right (that is the silhouette); the colour pass draws them with vs 5332b131 + fs a6d374cb (no discard: it relies on EQUAL) and its colour comes out black at every pixel (with ALWAYS the whole crown hull shows black), so after fog the crowns stay dark blue. Not the textures' swizzle or a 1x1 stand-in alone (unverified), not the LDG overlap (addresses are precomputed); next: which input of a6d374cb is zero (its vertex shader is a gmem/gpt one: 5332b131's pointer loads). The colour-pass mesh draws' records are non-zero in the game (1-0x5e groups) with or without the mirror; **on the replay** they read 0 with the default mirror/compute shadows (pool right, the copy the indirect read takes stale) and only `pool_mirror = off` + `compute_shadows = false` gives the game's counts. **Crowns fixed 2026-11-07 (runtime, no shader change):** 5332b131 reads its light-probe data through pointers: vs ssbo slot 1 holds five 64-bit pointers (0x787fb0a000 grid header, +0x40 / +0x380 / +0x680 cell tables with eight u16 probe indices each, 0x7881c9a680 the probe SH records, 16 bytes each), and out 14/15/16 are the trilinearly weighted probe SH (0.282 = Y00) that a6d374cb lights the leaves with. All five point into a 149 MiB pool that no draw, copy or texture of the frame binds; pools over one import chunk (64 MiB + 16 overlap) are imported chunk by chunk only where the render thread resolves an address, so none of its chunks was imported, the page table mapped none of its pages and every pointer load read 0: no ambient light, black crowns, dark blue after fog. Seen with the new trace lines `ptr word N: <addr> -> in pool ..., page table UNMAPPED` (trace_fs_draws / trace_frame print them for the 64-bit words of a pointer shader's bound storage buffers; `gpu::page_table_lookup` walks the table on the host). Fix: `gpu::import_pools_for_pointers()` (nvn_gpu_pool.cpp), called from `gmem_before_op` for every op with a `gmem` stage: once per change of the pool registry (`pool_registry_generation`), every chunk of every non-virtual pool is imported, so the page table maps all pool memory (first scene: 56-57 more chunks, 4.1 GiB of host memory imported, 1.2-1.5 s once on the render thread; no video memory). The crowns are green in the game (`work/run/crown_fix/ingame_crowns_t110.png`, `ingame2_t150.png`, `ingame_t200.png`; same view as gpt3_t150) and on replays; fps unchanged (10.6-11.3 during the first minute as in gpt_3 / base_1, 15.9-16.2 later in the second run with warm pipelines). The replay's "records read 0" was a trace artifact (see "Frame capture and replay"): with the snapshots read after the GPU finished, the colour-pass mesh draws get 3-0x157 groups on the replay with the default mirror and compute shadows too (`work/ai_run/oot/t2_default_fixed`).| fixed |
| A16 | intro | **Intro plays slowly** (user: half speed or slower in the forest). The game steps once per presented frame (fixed 60 Hz step), so the intro runs at fps/60 speed. Was 10-12 fps in the first scene (render thread 73 ms per frame, GPU ~55 ms). **2026-11-08: 32-38 fps in the first 100 s of the first scene, 15-21 fps in its later views** (commits d8eeec9..82ac2d0, see "Performance work: state and next steps"). Not yet 60 **Game speed fixed 2026-11-08 (5b4c504): real-time game step** (`[time] mode = real`, OoT default; see "Play mode / frame hook"): the game now runs at real speed at any fps; the fps themselves are the performance work | game speed fixed, fps open |
| A17 | all | **Global memory accesses the translator cannot tie to a bound buffer are dropped** (found 2026-11-06 chasing A9): Ryujinx's GlobalToStorage removes every LDG/STG/ATOMG/RED whose address it cannot trace to a c[0] storage descriptor (`results.tsv` messages "Failed to find storage buffer for global memory operation ..."): loads read 0, stores and atomics vanish. **3,485 translated blocks** have such accesses (1,588 vertex, 1,095 fragment, 615 compute, 179 mesh, 8 task). Typical cause: a 64-bit pointer loaded from memory (pointer tables; the foliage task shader reads one at c[0] 0x670). Seen effect: OoT's GPU culling root `4c1e8564` (direct, 169 groups) loses its counter atomics, so the 11 indirect dispatches after it get 0 groups (`snapd_*` snapshots, run `work/run/mesh_14`) and every GPU-culled colour-pass mesh draw gets {0, 1, 1}: the leaves stay invisible. Lichii (checked first) keeps them as global accesses and resolves them at run time by comparing the address with the program's bound storage descriptors (base from c[0], size from the NVN descriptor word +8; nearest base for native pointers), 0 / dropped when nothing matches; pointer tables only for compute; no buffer device address. **Lichii's fallback implemented (2026-11-06)**: `tools/shaders/ampere/frontend/AmpereGlobal.cs` + `spirv-g2s-c0-base.patch` (candidates: the stage's 16 c[0] storage slots, task 0x650 / mesh 0x750, compute: the NVN2 table's storage entries; the harness marks such blocks with the resource token `gmem`, the runtime then builds a per-draw graphics c[0] with every stage's {address, size}, `graphics_c0` in nvn_replay.cpp; off: `--no-global-fallback` / `NXRT_HARNESS_NO_GLOBAL_FALLBACK=1`). Corpus: 0 "Failed to find storage buffer" left, 3,413 blocks use the fallback, no block lost its translation. **Not enough for the culling root**: `4c1e8564` takes its counter addresses from a pointer table in a uniform block (`LDC.64 c[0x4][R0*16+0xe0]`, also +0x80/+0xa0; values like 0x796bc8e000, 0x7971cf0700), and none of them lies in the 5 storage buffers bound to the dispatch: the counts stay 0 (run `work/run/gfb_4`). Lichii has nothing further (no buffer device address). Next (own design): resolve such addresses through Vulkan buffer device addresses of the imported pools (guest VA = host VA; a pool table the shader searches) **2026-11-07:** the search runs as loops (helper 8,351 -> 2,523 words; slow pipelines median 26 s -> 13 s, mesh pipelines still 40-72 s, run da_9). The device-address path works with the default `pool_mirror` / `compute_shadows` (run da_11: 14.8 fps, no device loss, counts written): the A17 test runs had both off, which halves the fps (perf_ab1 8.1 vs perf_ab2 16.4 fps, same cache). The culling root 4c1e8564 now writes its item counts (word 3 of its indirect records, `[nvn] trace_snap_after`), but word 0 (the x group count, `RED.E.MAX [ptr]` at 0x6e70/0x74d0/0x7830) stays 0: in its translation 17 function-local registers are read but never assigned (address high word and value of all three max calls, three of six atomic adds); 0 of 300 sampled other compute shaders have any, `--debug-mode` has the same 17, and the structurizer check passes, so decoding/SSA loses the definitions in that code (it also has a BRX jump table at 0x1380 and BREAK/BSSY nests). Next: the IR block graph of the tail. **Later 2026-11-07:** the 17 unassigned registers were Ryujinx's `Optimizer.RemoveNode`: removing the dead copy of a call's unused result walked on into the call (it stays: side effects) and took it out of its arguments' use lists, so their definitions were deleted (`translator/optimizer-remove-node-side-effects.patch`; 0 unassigned now, no corpus block changes). A second, runtime cause hid it in runs: the shader preloader used a leftover .spv whose results.tsv row was gone and ignored the record the on-demand translator delivered (40 compute shaders' storage buffers unmapped, dispatches left out; fixed in `preload_one`). With both, the counts are written (capture deku150: 0x41, 0x42, 6, 4, 0xc8) but word 0 (x groups, the tail's `RED.E.MAX`) is still 0 in game and replay alike; the tail's shared-memory reload is declared at the right size (384 words). Game addresses of these records change between runs (0x796bc82000 in da_10, 0x796bc8e000 now): snapshot by the pointer table (ubo1 of 4c1e8564), not by fixed addresses. Capture `work/captures/20261107-123358-a10-deku150` (this branch's build, 8-stage format) replays in ~40 s per try and reproduces the counts. **Fixed 2026-11-07** (word 0 non-zero in game and replay, see A15) by the RemoveNode fix once the main cache was re-translated with it; the coherence kill switch (`compute_shadows` / `pool_mirror` off) was not needed to tell. Also done: (1) **guest page table** instead of the 16-step search: `nvn_gpu_pool.cpp` keeps one device address per 4 KiB page (2 MiB leaves where one import holds a whole span; root per 1 GiB, 47-bit addresses; host-visible buffer written in place, 32 MiB, grows by copying), and `AmpereGlobal.EmitPageWalk` walks it generically: level shifts, root size and table address are c[0] 0xfe0..0xfef data, so the layout can change in the runtime without a re-translation. Such blocks carry the token `gpt`; older translations still get the sorted table at 0xff0/0xff8. The main cache's 3,413 gmem rows were re-translated (22 s, all ok; the 23 on-demand rows dropped); SPIR-V size barely changes (763 -> 756 MB: the candidate checks against the stage's bound storage descriptors dominate, not the search), and fps in the Deku Tree view is as before (10.4-11.8 with or without coherence, base_1 11.5-12.7 with a prepared cache). **A preparation is due** (not run: shaders still change); until then the first starts compile the changed pipelines on demand (one cold start never left the title: 100 s mesh pipelines, 27 GB private; pass `nvn.compute_background_spirv_kb: 64` and `memory.limit_gb: 26`). (2) **gmem coherence** (`[nvn] gmem_coherence`, `gmem_before_op` in nvn_replay.cpp): before a pointer shader, held compute results within 64 KiB of each pool pointer in its uniform blocks go back; after one with the `gmemw` token (9 compute blocks write that way) those ranges are `gpu_wrote`. Flushing every held result instead cost 900 MB/s of copy-backs and dropped to 7 fps; the windowed version costs ~14 MB/s plus ~11 MB of pool-copy refills per frame and no measurable fps. `[nvn] trace_snap_after` takes `u<slot>@<offset>` items (pointer from a compute uniform block). | fixed |
| A18 | first scene, village | **Foliage flicker** (user, 2026-11-08): grass, bushes, foliage and tree leaves showed since A15/A14 but flickered heavily; the replay of capture `20261107-230737-nb3-alias_kid` changed leaf clusters from loop to loop (25 of the frame's 41 GPU-culled mesh draw records differed). **Cause: the shader translator moved ballots into branches.** Ryujinx's `AstOptimizer` folds a single-assignment value into its uses, and a unary operation of an operand (a ballot) counts as worth folding for any number of uses; so `VOTE.ALL R13, PT, P0` that the SASS runs with the whole warp before `@lane!=0 skip; POPC R13 -> ATOMG.ADD` became a ballot inside the lane-0 branch: lane 0 reserved one slot per warp while every lane with P0 wrote its record at base + rank, records overwrote each other differently every frame (culling pass f2422a7a: 46 records per frame instead of 548, which ones varied), and the mesh draw counts after it varied. **Fix:** `tools/shaders/harness/translator/structured-subgroup-propagation.patch` (a value containing a ballot/vote/shuffle is folded only into the next statements of its own block; `tools/shaders/README.md`). The first stopgap (VOTE PT as the lane's own bit, REDUX as the lane's own value, commit 91a1614) is now off by default (`NXRT_HARNESS_VOTE_SELF=1`): it changed warp-wide results the game relies on (a max over an object's 32 lanes became each lane's value) and left +-0.5% variation. Corpus re-translated (4,492 blocks changed: 4,343 fragment, 123 mesh, 15 task, 11 compute; none lost), the 226 stale rows of `main` blocks dropped, the 16 culling/HiZ blocks from `main` translated from a `[nvn] dump_shader` dump. **Checked:** replay (6 traced loops, run `work/ai_run/oot/a18c1`): every mesh draw's x count the same in every loop (sum 4,398), f2422a7a's 548 records the same set (only list positions written by atomics change order); 12 replay screenshots (`work/run/a18b/shots_fix2`): no pixel differs by more than 4/255, 40-120 pixels by 2-4 (scattered, not foliage; probably float atomics); the game (first scene, 20 screenshots, `work/run/a18b/game_burst`): full plants, no popping. Not the cause (each checked): barriers (every dispatch ends with a full memory barrier; the HiZ builders a17259b2/39a1f760 use only workgroup barriers), the chain's inputs (the same up to the order of atomically appended lists), the pool mirror, storage shadows, held results, maximal reconvergence, `[capture] reset_full`. Diagnostics added: `[nvn] trace_snap_after` takes several prefixes (`a|b|c:` or `*`), numbered per present. | fixed |
| A19 | Link's house | **Vine leaves flash white** for a frame while the camera turns (user-visible bloom blobs on the wall vines, then normal again one screenshot later): `work\ai_run\oot\shots\shot_121818_844.png` (flash) vs `shot_121822_370.png` (same view, normal), run play2 2026-11-08, upscaler network 49659bca ready. Probably the A13/A14 family (a non-finite or huge value in one draw that bloom spreads); not traced yet: `[nvn] trace_snap_fs` on the vine material while turning the camera **2026-11-08 (later):** not reproduced in ~70 screenshots while turning the camera in Link's house (runs `work/ai_run/oot/rt1`). Lead: the upscaler fallback (`[nvn] upscaler_fallback`) switches between the scaled copy and the game's chain whenever a network variant (fea98fe7 / f90d726d / 49659bca, chosen by the dynamic resolution) is not built yet; the first chain frame after such a period reads temporal history the chain did not write meanwhile, and the play2 flash came right after 49659bca became ready. Every switch is logged now (`upscaler fallback: ... cannot run yet (present N)` / `... runs again (present N, ...)`): correlate the next flash with those lines; `[nvn] shot_frames = a:b:1` (`shot_dir`) writes every presented frame of a range to find the flash frame | open |
| A20 | Kokiri Forest | **Falling leaves (particles) look black or very dark** against the bright sky: `work\ai_run\oot\shots\shot_121919_586.png` (cutscene, while 49659bca was still building), `shot_121940_345.png` (dark green once it ran), `shot_122322_141.png` / `shot_122310_383.png` (in play). Not known yet whether the game lights them this dark (backlit) or the particle shader misses its lighting (probe cube from 471bac84, translated since 0a22da9, or an unresolved texture); compare a capture's particle draw with the probe filter's output **Checked 2026-11-08 on captures `20261108-130548-rt1-leaves` / `133752-rt1-leaves3` (deleted): no missing texture or constant.** The leaves are a two-pass effect (Effect/Dm_Kokiri_0011 vsetb): a prepass draw (vs 49deeadf / fs becac56f) writes depth and a constant black colour after its alpha test, then the colour pass (vs 466f64d5 / fs 33d8129d, no discard) draws with depth EQUAL and lights them (cube probe t483 has content, every texture, uniform and storage binding resolves; back faces get the reflected normal through gl_FrontFacing, which matches the culling). `[nvn] skip_fs_file` named the draws (only becac56f removes the leaves); `[nvn] debug_equal_as = 4` leaves the leaf bodies the same colour (EQUAL passes), so the dark olive/brown (e.g. 65,55,31 lit side up, 18,21,8 under the tree in shade) is the shader's own lighting of backlit or shaded leaves against bright fog. Only a few edge pixels show the prepass black where the colour pass fails EQUAL (14 pixels in one frame). Translator note: both leaf vertex shaders store `AST a[0x5c]` (probably the per-primitive shading rate), which Ryujinx drops ("Attribute offset 0x5C is not valid"); harmless here | as designed (not a rendering gap) |
| A21 | Kokiri Forest | **Signpost flat and overbright up close** (plain yellow face, no carved runes; the same sign shows its runes from further away): `work\ai_run\oot\shots\shot_122310_383.png`. Possibly a texture or mip that is not uploaded yet, or the near-camera fade; not traced **2026-11-08: not a bug.** The plain face is the back of the sign (its post is in front of the board seen from there), lit by the sun; the carved runes are on the front, which shows them up close too: `worki_run\oot\shots\shot_133831_854.png` (back, same view as 122310) and `shot_133852_619.png` (front, close) | not a bug |
| A22 | game at 1080 render height (DRS 0.75 / 0.667) | **Upscaler network output striped / blocky** (found by pinning the render resolution, `[oot] render_height = 1080`): f90d726d (0.75) gave vertical stripes in 8-px columns in 16-row bands, 02dcb888 (0.667) blocky noise, in the scalar and the `.cm.spv` alike. **Cause: the decoder dropped `.F16.RN` of TLD4** (bits 79..80; TLD and TXD have the same field, nvdisasm 13.3 probe). The gathers were translated as fp32: four registers Rd, Rd+1, Rd2, Rd2+1 instead of two half pairs, so live registers were clobbered (f90d726d: the blue gather's Rd2+1 = R34 was the next gather's x coordinate) and the luma tile got fp32 bits. Not MOVM, LDS.64/STS.64, LDG.E.64 or IMAD.HI: every ALU/memory word of the four network blocks passes `hwcheck --game-corpus` (4,557 words, mini corpus of f90d726d/02dcb888/49659bca/fea98fe7). Fixed in `SassDecoder.Tex.cs` (TXD now takes `.F16.RN` too; `tools/shaders/README.md` "TLD / TLD4 / TXD .F16"). Changes 27 "runtime" blocks from `main` (all network passes with TLD4.F16, incl. 49659bca; list in the merge commit / report), no corpus block (full batch old vs new: 0 of 47,010 files differ). TLD4 itself is not hardware-checked (hwcheck has no texture cases); the Rd = (t0, t1), Rd2 = (t2, t3) order matches how the code stores the rows. Replay of capture `20261108-221243-drs-drs1080` with the fixed blocks: no stripes; the network's picture is darker than a scaled copy of its input (mean RGB 83/80/60 vs 146/143/125) but matches the unstriped pixels of the game's own reference (91/89/77), and sharper. 02dcb888 not seen on a replay (no capture at 0.667). Runtime rows without code in `work\shaders\oot\missing` (199) could hold more TLD/TLD4.F16 blocks: dump them with `[nvn] dump_shader` and re-translate. **In game (2026-11-09):** the 27 fixed blocks are in the main cache (`results.tsv` sizes updated; old files in `work\shaders\a22_backup`); first scene at the pinned 1920x1080 with f90d726d running for real (cooperative-matrix variant): no stripes, normal brightness, 32-48 fps. The OoT default `pass_copy` now covers only 02dcb888 | fixed (0.667 not seen) |

**Movies** (`romfs/Event/Movie/*.webm`, 26 files, 1.6 GB) are **AV1** in WebM (mime `video/av01`), decoded by nn::codec on the host: libvpx
(VP9/VP8) and **dav1d 1.5.4** (AV1), both static from the repo-local vcpkg (`tools\vcpkg`, git-ignored; the user ran the bootstrap and
`vcpkg install libvpx:x64-windows-static`; `vcpkg install dav1d:x64-windows-static` ran from here). `runtime/cmake/media_deps.cmake` finds
both in `tools/vcpkg/installed/x64-windows-static` (reconfigure: `cmake game-build\build\release`). `video_codec.cpp` now has the SDK 22
names (AV1 `By{Transfer,Remote}Memory`, the 1-argument `Get*SubmissionReadyEvent`, `UnregisterAv1DecoderBuffer`, image calls without
CodecMode) and decodes **lazily** (packets queued, decoded when an output buffer waits: OoT submits the whole file at once, 10 GB of
pictures otherwise). The movie player also runs **NDVE** (an NVIDIA video-enhancement network, `romfs/Movie/Ndve/Ndve-Nvn2-vid_seq5.dat.zs`,
parsed at main+0x20e4d20): it needs NVN2 device info 0x67 (=256; the runtime now answers 0x62-0xA9 with the sdk's constants, read from
sdk+0x656210/0x648380) and then costs ~100 ms of GPU time per frame on this iGPU (8 fps). Without the file the player shows the decoded
frames directly, so OoT's title profile hides it (`TitleProfile::romfs_hide`, `[title] romfs_hide`).
dav1d ran with `max_frame_delay = 1` (no frame threading): ~21 pictures/s of the 1080p intro, the movie played slowly. Now the frame
delay is dav1d's own choice and `[media] video_decoder_threads` defaults to half the logical cores (8 here): 37-63 pictures/s.
Flow after the title: A -> `Dm_Kokiri_0000.webm` (the intro, **5 minutes**, no skip with A/B/+ presses or 3 s holds), then the first
scene. Hiding the intro file makes the game wait forever on a black loading screen. For tests, `[media] video_max_pictures = 120` ends
every movie stream after 120 pictures (the title movie then restarts every 4 s).

**Test shortcuts (2026-11-06):** reaching the first scene takes ~3 min per run: `media.video_max_pictures = 120` (movies end early), A pressed at 50, 54 and 58 s on the virtual pad (the title menu is ready around 45 s; earlier presses are lost), then ~70 s of loading. `[nvn] skip_fs_file` (fragment hash prefixes, re-read every 500 ms) bisects draws; `[nvn] trace_frame = 3500-9000` with `trace_targets = 0` traces every draw (700 MB of log). Gameplay with 1/16 of the fragment draws ran at 10-15 fps.
With `video_max_pictures` the intro's video ends at once (on the 5800X3D dav1d decodes the 120 pictures in 0.5 s, ahead of display),
while its voice lines (Opus sounds the game starts along the event timeline, not part of the video-only webm) keep coming: they
overlap about a second apart (heard by the user, 2026-11-06). A test artifact, not an audio bug. Without a pad script, Enter on the
keyboard is A: `tools\.venv\Scripts\python tools\ai\mcp_server.py send_input steps='["key Return"]'` (posted, works in the background).

**First gameplay frame (2026-11-06):** after the intro the first scene's frame has ~13,100 draws and took 85-112 s to replay: 850 MB of texture uploads with ~100 ASTC->BC7 transcodes on the render thread (first use; stand-ins exist but most waited), 3,000 pipelines linked, 460-650 memory pool imports. Later starts find the BC7 entries on disk. Pipeline parts come from the prepared cache (9,000 parts in 12 s of worker time instead of 350 s).

**Play mode / frame hook:** OoT's frame update is main+0x3b4ea90 (entryhook in `main.sites`; the framework's per-frame virtual, vtable
0x4c63f80 +224, called by the endless loop main+0x9467b0; the base class's +224 0x346c8d0 is never called). `runtime/games/oot/oot_game.cpp`
registers it plus factory key bindings (WASD move, arrows camera, Enter/E/Space A, Shift/Backspace/left mouse B, Q ZL, F R, R/T X/Y,
G ZR, Escape +); pads keep the console layout (the base engine maps SDL gamepads to a Pro Controller). A deleted `controls.ini` in the
game folder is rewritten from the factory text.
**Gamepad verified** (2026-11-06) with the test tooling's virtual SDL pad: script `keydown vpad attach xbox`, `keydown vpad button south 1`
/ `0` (MCP `launch` script lines): A on the pad starts the game from the title, and the intro's rumble reaches the pad (`vpad rumble` log
lines). A real XInput pad goes through the same SDL gamepad path.
**Keyboard and mouse verified in game** (2026-11-08, Link's house and Kokiri Forest, posted keys through the MCP `send_input`): W/A/S/D
move Link (screenshots `work\ai_run\oot\shots\shot_121748_545.png` -> `shot_121754_667.png` W, `shot_121803_510.png` S, `shot_121808_357.png`
A, `shot_121812_839.png` D), Space = A rolls while running (`shot_121828_122.png`) and opens the door ("A Öffnen", `shot_121836_933.png`),
the arrows turn the camera, and since 8ecf580 the mouse does too (OoT has no input hook, so `GamePlay::mouse_stick_rad_s` turns mouse
motion into the right stick; it is captured while the window has the focus). Jump is X (R key) in this remake ("Springen X" on ladders).
States: `link_house` (free control right after the intro, autosave) and `kokiri_outside` (outside the house after Saria's greeting; the
`continue` route reaches it in ~40 s). From `first_scene` it takes ~5 min of intro to reach `link_house`.
Fps seen (2026-11-08, RTX 4080, one run, pipelines warm after the first minutes): intro 20-33, Link's house 33-37, walking in Kokiri
Forest 18-22 (1% lows 12-14). The upscaler network 49659bca (4.9 MB of SPIR-V) took 285 s in the driver the first time; its dispatches
are left out until then (the scaled-copy fallback stands in).

**Real-time game step (2026-11-08, 5b4c504; `[time]` keys in `runtime/include/nxrt/timestep.h`).** OoT's frame update (main+0x3b4ea90,
x0 = the framework object) runs the calc, then sets the step every system multiplies its per-frame motion by: `scale = 60.0f / fps`, fps the
u32 at this+0x2c8, the scale a float in main's data (main+0x4d41fc8); retail keeps fps at 60 (the u32 at this+0xfc is 60 too and the game puts
it back into +0x2c8 after every frame), so it advanced 1/60 s per presented frame and ran at fps/60 speed. The runtime's timestep module
(entry hook on the same function, before the calc) now writes scale = measured frame time * 60 and the nearest whole rate into +0x2c8
(`timestep::Sites::apply_step`, OoT side `oot_core.cpp`). Movies: the movie player shows one decoded picture per frame, so while the game
reads nn::codec pictures the step stays 1/60 (`Sites::fixed_step_during_movies`: the event timeline that starts the voice lines stays with the
pictures, A11), and `fps_limit = auto` keeps 60 for OoT (`Sites::cap_at_display_rate = false`). OoT default `[time] min_rate = 10`: steps up
to 1/10 s, slower frames slow the game down; hitches over 100 ms are clamped (in a 40 s walk with pipeline builds game time was 84% of real
time, in steady play 100%). `game_main` reads `[time]` again after the game's config defaults. `[time] mode = off` restores the retail step;
`[time] csv = <file>` logs per frame host time, step, game time and movie pictures. **Measured (RTX 4080):** W held 2 s from
`kokiri_outside` brings Link to the same rung of the ladder at 27 fps and with a 10 fps cap (`rt_walk_real_auto.png`, `rt_walk_real_cap10.png`
in `worki_run\oot\shots`), with `mode = off` (12 fps) he only reaches the ladder's top (`rt_walk_off_auto.png`); camera turns with the
arrows match at 26 and 10 fps; no fall-through or jitter seen. Intro with a 30 fps cap: 30 pictures/s and game time 0.50 of real time during
the movie (as on the console at 30 fps), 1.00 in the 3D cutscene after it at 24-30 fps. The ocarina was not reached (not checked).

**Game window menu** (2026-11-06, `runtime/docs/MENU.md`; the user asked for real Windows menus, not an in-game window): a native menu bar
(Game, Input, Graphics, Audio, Debug, Help; `src/gfx/play/win_menu.cpp` from the data model `menu_model.cpp`) and modeless native dialogs on
their own thread (Gamepad with press-to-rebind, Keyboard and mouse bindings, More graphics settings, Debug info, Log). Changes apply at once
and are saved in `controls.ini`; "(next start)" entries are runtime keys in `controls.ini [config]` (`section.key = value`, applied at start
unless the ini or `NXRT_*` set them; game-module defaults give way via `config_set_default`). The bar is hidden in fullscreen; there the ImGui
window (F1, View + Menu) is the menu (`[gfx] overlay_menu`). Alt alone and F10 stay game keys. **An open drop-down holds the game's frame**
(Windows' modal menu loop runs on the game thread that pumps the window; audio continues, the watchdog is fed); dialogs do not. Checked as
named instances on the title screen (`work\run\nm1`..`nm3` of the menu branch): bar and client size 1280x720, fullscreen round trip, each dialog,
a menu toggle saved, a 12 s drop-down without a freeze report, press-to-rebind with the virtual pad from the native dialog. Native UI is
driven and captured without the mouse through ctypes (`PostMessage WM_COMMAND`, `SC_KEYMENU`, `PrintWindow`): boot screenshots and `@shot`
contain only the Vulkan frame. Posted mouse clicks miss in the game window (SDL reads the real cursor).

## Performance work: state and next steps (2026-11-08)

Goal from the user: 60 fps in the first scene and in game, with real rendering. **Measure** with `load_state("first_scene", instance=...)`,
settings `{"memory.limit_gb": 24}` (+ `prof.sample_hz: 100` for the CPU sampler, `prof.trace: "ftrace.txt"` for zones and `gpu.exec`
(GPU time per submission), `prof.gpu_passes: 1` for GPU time per pass kind; the passes add drains, so absolute fps is lower with them).
The fps of the hitch lines from 20 s after the scene starts (`pacing on (present N, 13xxx draws)`) is the number to compare; the intro's
camera moves, so compare the same window (20-100 s). Runs are alone on the machine. Scripts used (scratch, not in the repo): fold the
`prof_samples.folded` by thread (`nvn-render`, `main`), sum `gp:` zones of `ftrace.txt` per frame.

Baseline e2c78f7: 11.6 fps (fps_base1), first 100 s ~10 fps; render thread 99.7% busy (73 ms per frame), GPU ~55 ms per frame.

| Commit | Change | Measured (first scene) |
|---|---|---|
| d8eeec9 | page validity checks per binding a word at a time (`ww::all_valid_or_armed`) | 11.8 -> 15.5 fps (profiler on); render thread then half idle, GPU the limit |
| e39ea0f | compute shadows hold their results (`[nvn] compute_shadow_hold`) | GPU 63.8 -> 56.5 ms (pass timing on) |
| 0992100 | uniform updates at record time + snapshots (`[nvn] ubo_update_cpu`) | ~90 updates per frame no longer break the pass; small |
| 4a609a5 | occlusion queries inside the pass (`[nvn] occlusion_in_pass`) | passes per frame 670 -> 45, GPU 56 -> 30.7 ms, 19 -> 29 fps |
| 3382350 | shared graphics c[0] tables, driver constants read once, pool-array checks after changes only | render thread 28 -> 24 ms per frame, 27.6 -> 30.2 fps |
| 82ac2d0 | compute results: hot pages filled untracked (`Mirror::fresh`), partial copy-backs (`[nvn] compute_out_split`), draws fill the copy (`[nvn] draw_fill`), read-only SSBOs from SPIR-V (`[nvn] ssbo_readonly`) | GPU 29.8 -> 28.2 ms; 20-100 s: 27-31 -> 32-38 fps (no instrumentation) |

Where the time goes now (GPU ~28-30 ms per frame, the limit; render thread ~24 ms, the game thread waits in nvnSyncWait):
- render passes at 854x480 (the game's dynamic resolution; it rose to 953x536 once the GPU got faster): ~11.5 ms for ~43 passes.
  Vertex and index buffers of ~300 pools at 0x60_xxxx_xxxx are read from host memory over PCIe every draw: those pools are outside
  the heap's primary view (`viewed 0`, "pool without a copy": ~2M binds per 100 s), so they get no device-local copy (no write tracking).
- compute results going back to the pool: end of submission (eager: pages the CPU read once or hot pages) and before draws that read
  them through storage bindings: ~4-8 ms of copies.
- compute ~4.5 ms (fea98fe7, the upscaler network, 1.9 ms), shadow-map passes 1.6 ms.

Next steps, in the order they would pay:
1. **Device-local copies for the pools outside the heap's primary view** (0x60_xxxx_xxxx, flags 0x24, ~300 pools, mostly vertex and
   index data): `mirror_for` refuses them because `mem::ww` regions must lie in the primary view (another view would bypass the write
   tracking). Find out how these pools are mapped (svcMapPhysicalMemory / alias views, `mem::heap_alias`) and either track them there or
   copy them once and re-copy on nvnMemoryPoolFlushMappedRange (the game flushes what it writes into CPU_CACHED pools). Expected: most
   of the ~11 ms of pass time is PCIe reads of these buffers (an RTX 4080 draws 13k draws at 480p in a few ms).
2. Remaining compute copy-backs ("draw storage/xfb" flushes when `mirror_fill` cannot fill: banned pages, ranges over two chunks), and
   the eager copy-backs of pages the CPU read once (cpu_read is sticky: a page read once at load stays eager forever; an age-out would
   help).
3. Render thread (24 ms, the next limit once the GPU is under it): per-draw `build_set` (pool_lookup, out_overlaps under a mutex,
   resolve_texture), descriptor set allocation per draw; `out_overlaps` could keep a page bitmap instead of walking g_out.
4. ~~Dynamic resolution~~ pinned to 1920x1080 since 2026-11-08 (`[oot] render_height`, see "Render resolution pinned" below). Was: OoT's internal resolution follows the frame time (854x480 at 30 ms, 953x536 at 28 ms; the console's output is
   2560x1440 after the upscaler). The game's timestamps are CPU record times (`timestamp_ns()` in ReportCounter), not GPU times. Pinning
   it to full scale costs GPU; measure both once the GPU has room.
5. ~~Real-time game step (A16)~~ done (5b4c504): the game speed no longer depends on the fps; the fps targets stay.
6. x86-64-v3 build of the lifted code only if the guest threads become the limit (they use ~0.9 cores; main waits for the GPU).

**Session 2026-11-08 (later): measured, mostly ruled out; no fps gain committed.** First scene at d18af54/c3e83ad: 33.9-34.1 fps in
20-100 s, 22-23 fps later (no instrumentation; the same as 82ac2d0 within the run-to-run spread of +-2 fps). Committed: the crash
fix (d18af54: a write fault racing `guard()` left a guarded page without its prot bit, "unhandled write fault ... prot 0 guard 1",
seen twice in ~10 runs, not in 8 after) and diagnostics (c3e83ad): `[prof] gpu_draws` (with `gpu_passes`) stamps every draw as
`gp:d <fs> <vs|mesh>`; the 10 s lines give why draw reads over held results flush, and which results go back at submission end.
Where the GPU time is (gpu_passes + gpu_draws, ms per frame): 854x480 pass 12.4-16 (with stamps), end-of-submission copy-backs
("tail") 2.3, "compute out" flushes 2.2, fea98fe7 1.9, other compute ~3.5. **~830 draws carry the pass time, and 8.3 of the top 11.6 ms
are draws whose vertex/mesh shader is a gpt shader** (A17 pointer accesses): fern mesh fs 957527e8 / mesh fbb731e2 alone 2.4 ms,
then 4097c13f/6ec65625, 40478981/f3054ac2, bb9b2ea9/a5bfd19f, 2972adf5/40953571, 3abc0b54/301aa659 (0.5-1 ms each, all gmem gpt).
Ruled out, one game run each (pass time and fps unchanged):
- vertex/index/uniform data in host memory: device-local copies for the ~300 MapPhysicalMemory pools at 0x60_xxxx_xxxx (write
  tracking extended to the alias region; worked, 0 binds "without a copy" left): pass 12.6 -> 12.4 ms, fps 31.5 -> 32.5 (noise);
- uniform blocks and read-only storage buffers of draws copied into the upload ring (device-local BAR) at record time, even forced
  for every binding (`host_copy_force`), and draws' read-only SSBOs bound from the pool copy: no change in any draw's time;
- the guest page table pointed into the pool copies for valid pages: no change (and the entries churned: pointer shaders counted
  every binding as written, `gpu_wrote`, so their pages never stayed valid).
So the draws are bound by their shader code, not by where their data lives. The translated gmem access (AmpereGlobal.cs) does, per
32-bit word, a 16-slot candidate loop over c[0] and then a dependent page walk of 2-3 levels (two loads per level): that latency on
every pointer load is the most likely cost. **Next (needs the user's go-ahead: re-translation of the 3,413 gmem blocks, ~1 min, and
then new mesh/vertex pipelines, 40-72 s each to build on demand, or a preparation):** a per-invocation one-entry cache of the last
page walk / last candidate slot in the translator, then measure with `gpu_draws`.
Also tried and dropped: (1) aging the CPU-read marks of compute results (`cpu_read`, 600 presents): 34 -> 20 fps (OoT reads those
pages every few seconds; each read then waits for the GPU in the guard handler). (2) Copying back only the result bytes of pages that
are newer in the copy but untracked, then filling them, for draws (`draw storage/xfb` 4 GB -> 0.4 MB per 10 s): 34 -> 23 fps, because
each of ~150 such draws per frame then broke the pass. (3) A per-thread cache in `pool_lookup` and a block filter in `out_overlaps`:
their share of the render thread did not drop (still 3.5% / 4.5%). The render thread waits 45-50% (GPU-bound).
Facts for the copy-backs: the end-of-submission copies (~650 MB/s) are 8 culling output buffers of 4.4-6.6 MB (0x796bfa0000,
0x796c5f6000, 0x796cc4c000, 0x796d2a2000, 0x797045e000, 0x79708a9040, 0x7970f46000, 0x7971391040), each sent back whole about every
second frame because some of its pages count as CPU-read; "draw storage/xfb" flushes (~400 MB/s) are reads over results whose pages
are newer in the copy but not armed (`mirror_fill` skips them as "have", `new_servable` wants them armed). The replay with
`[capture] reset = 0` is useless for timing mesh draws: the culling counts accumulate loop to loop (fern draw 25 ms there).

**Session 2026-11-08 (gmem lookup, translator): GPU draw time -3.7 ms per frame, fps unchanged.** The A17 pointer accesses
(AmpereGlobal.cs) did, for every 32-bit word, a 16-slot search of the stage's bound storage descriptors and then a page walk with early
exits. Now: (1) the search only runs over the slots the runtime marks in c[0] 0xfc0 + 4 * index (`AmpereGlobal.SlotMaskC0`; index 0
compute, a graphics stage its descriptor base >> 8: vertex 1 ... fragment 5, task 6, mesh 7). `graphics_c0` (nvn_replay.cpp) marks a slot
whose binding may differ from the pool: a result held in the device-local copy (out_overlaps) or a virtual pool; compute marks every slot
(shadows). Most draws have mask 0 and go straight to the page table. (2) The walk is one loop without early exits and remembers the last
leaf (page or 2 MiB span) per invocation in a Private array (`gmem_page_cache`, cleared at main's entry), so the four words of an LDG.128
and the fields of one record walk once. NVIDIA 610.88's compiler crashed (access violation in nvoglv64.dll, pipeline build) on the first
form of the cache, a cache test branching over the old walk loop whose exits jumped to the miss label: keep the walk free of early exits.
Measured (first scene, `[prof] gpu_draws`, ms per frame, before -> after): fern mesh draw fs 957527e8 / mesh fbb731e2 2.43 -> 0.90,
a5bfd19f 0.58 -> 0.31, 40953571 0.51 -> 0.29, 301aa659 0.47 -> 0.24, 76b1c776 0.46 -> 0.26, b8b873e5 0.44 -> 0.22, a873d795 0.34 -> 0.19;
6ec65625 / f3054ac2 unchanged (~0.85: their slots are marked); sum of all draw stamps 14.6 -> 10.9. fps 20-100 s after the scene: 34.0
-> 33.5 (pf6; same within the spread), so the frame is not bound by draw time alone (render thread ~23 ms, main waits ~25 ms in
nvnSyncWait; next: find what serializes them). Picture: replay of capture 20261107-144949-a10-deku old vs new cache SSIM 0.998 (fern
close-up), Deku Tree capture (crowns) 0.998. Tried and dropped: trying the page table before the bound descriptors for every access (fast,
but then a pointer read of a bound buffer whose result is held in the copy reads the stale pool, and flushing those results for every
pointer reader cost 450,000 flushes in 200 s: 19-27 fps); the search first with the cached walk after it (fern 1.23 ms).
**The whole gpt set is re-translated in `work\shaders\oot\spirv`** (3,413 corpus blocks; the 23 `gpt` rows of blocks not in the corpus
were deleted, so the on-demand translator redoes them; list in `work\shaders\gpt_backup_old\dropped_rows.txt`; the old .spv files and
results.tsv are in `work\shaders\gpt_backup_old`). Every pipeline with one of these shaders builds again on demand (14-70 s each, the
"Preparing shaders" overlay counts them) until **the full shader preparation runs again (due; the user runs it at the very end of the
project)**. `prepare.state`'s key changed with results.tsv, so the default run's start opens the "Preparing shaders" window by itself.
Re-translating a subset now: `tools\.venv\Scripts\python -I tools\shaders\scripts\retranslate_subset.py --corpus work\shaders\oot\corpus
--cache work\shaders\oot\spirv (--token gpt | --hashes FILE | PREFIX ...) [--backup DIR]` (temporary subset and output folders, removed
when done; `--keep-temp` keeps them).
Copy-backs, measured and dropped the same day: splitting results that are still guarded from an earlier frame (so the eight culling
buffers no longer go back whole every second frame: end-of-submission copies 6.6 -> 3.3 GB per 10 s) raised the GPU-reader flushes
(9,800 -> 14,800 per 10 s, each before a draw or dispatch) and cost fps (33.5 -> 24.5): the bulk copy at submission end is cheaper than
holding these results. `[nvn] compute_out_lazy = 0` (everything back at submission end): 29.5 fps. The remaining copy-back cost is the
"draw storage/xfb" flushes (~410 MB/s, ~8 per frame, `fill failed` = pages newer in the copy but not armed); fixing them needs the copies
before the render pass opens (look ahead in the command list), not inside it.
**Session 2026-11-08 (CPU touches of held compute results, WP3 a / WP6).** The guard handler now gets read/write and the host pc; every
CPU fetch of a held result is attributed in the log (`compute out fetch:` lines, `CPU fetch:` 10 s sums, `[nvn] out_fetch_trace`). Found
(first scene): (1) **the eight culling buffers** (4.4-6.6 MB) are read by the CPU in their **first page only**: main+0x341a00 reads the
counters and writes them back to 0, main+0x342ff0 reads after an `InvalidateMappedRange` of the whole buffer (lr main+0x3433cc). They go back
whole because the next frame's dispatch writes them again while their split part is still held ("held from before" in the
`sent back at submission end` lines). Splitting those too cut the end-of-submission copies 850 -> 150 MiB/s but cost ~10 fps (the CPU's
first-page reads and the readers then wait for or flush held data); copying the CPU-written first page into the copy for draws
(`[nvn] mirror_refill_written`) cut reader flushes 800 -> 150 MiB/s but cost ~7 fps the same way. Both stay off. (2) **The periodic
20-50 ms hitches** were job-worker reads (guest function main+0x12d0fc0, lr main+0x126d234) of small GPU results (16-192 KiB), a new page
every second or so: the split held the other pages, so each new page was a guard fault waiting for the frame on the GPU. Results under 1 MiB
are no longer split (`[nvn] compute_out_split_min_kb`): write-fault-handler hitches 30 -> 6 per 160 s. Invalidates no longer fetch or mark
(`[nvn] compute_out_invalidate = guard`). A/B with flags on one exe (first scene 20-180 s, alone): 34.7 fps / 1% lows 19.1 (old) vs 34.2 /
20.8 (new); fps runs here vary by +-4 fps between runs, so compare flags back to back.
Measuring tips: give `[prof] trace_events` 20000000 for runs past ~110 s (the default 6M fills up); per-10-s fps from the log: hitch lines `N frames in X s`.
`gfx_nvn_scene_state`: passed in every full test run of this session (no flake seen). Later the same day it segfaulted in about 1 of 5
single-group runs in `clear_color_partial_mask_known_gap`, with the main checkout's test exe too (not from the changes below);
`gfx_pacing.offscreen_loop_holds_240fps` misses its rate while other work runs on the machine.

**Session 2026-11-08 (render thread / GPU overlap):** the frame model fits the traces: frame time ~ max(P, R, G, (G + R + P) / 2), because
the big submission reached the GPU only after its whole recording (R and P in series inside the game's two-frame window). Measured with the
zone trace, no gpu_passes, 854x480 internal size in every run (new log line `render size: pass sizes with the most draws ...` in the
statistics block; compare fps only at equal sizes), warm pipeline cache (runs right after the gpt re-translation still built pipelines,
`lib.build` 15-22 ms per frame on nvn-gpl, and were 5-10 fps slower: check that zone before trusting a run). Medians per frame before the
changes: first scene R 19.7 / P 24.5 / main's nvnSyncWait 12.0 / frame 26.7 ms (35 fps); Link's house R 20.5 / P 21.0 / wait 11.1 /
frame 24.8 ms (39 fps); Kokiri walk R 17.9 / P 20.8 / wait 10.1 / frame 24.6 ms (36 fps). New: (1) `[nvn] submit_chunk_draws` (default
256): the recorded part of a submission goes to the GPU without waiting at the next point with no open pass once 256 draws/dispatches were
recorded (`frame_chunk`, nvn_gpu.cpp; ~2.4 parts per frame in OoT, long passes are not cut: `submit_chunk_pass_draws` would). (2) `[nvn]
set_reuse` (default on): read-only graphics descriptor sets are bound again without resolving when their raw inputs and the resolve epoch
are unchanged (15-20% of the sets in game; `verify` mode found no difference); the per-draw held-result checks of the bound buffers
(`out_draw_bindings`) are skipped under the same condition. Paired A/B in one run (`[nvn] cpuperf_ab = 300`, ftrace marks `cpuperf.on/off`):
first scene 30.1 -> 34.0 fps (chunks alone) and 29.1 -> 32.8 (both), Kokiri walk 26.9 -> 32.3 (chunks) and 31.1 -> 35.3 (both), Link's
house (static view) 46.6 -> 48.2 (both). Most of the gain is the chunks; set reuse adds ~1-2 fps where the render thread matters. After it
the first scene and the house are GPU-bound (P ~ frame time). The biggest render-thread item left is `out_overlaps` (nvn_gpu_pool.cpp, ~15%
of nvn-render samples, its own loop under a mutex), then build_set (~20%, half of it pool_lookup / resolve_in_pool / out_add).
After the merge with the coherence work (1769f1f), all of it on vs off in one run: first scene 36.7 -> 39.2 fps, Kokiri walk 36.0 -> 43.0.
`[nvn] pass_lookahead` (settle the coming draws' reads over held results before a pass begins, so they do not break it) works (replay
SSIM 1.0) but gained nothing in paired runs (first scene 34.7 -> 33.8, Kokiri 29.4 -> 28.7): the "fill failed" reads still go back
whole, only earlier. It is off by default. Testing pitfall: after merging header changes into this worktree, 4 test groups and possibly the
game exe had stale sccache objects (bugcapture/kernel_sync segfaults); `ninja -t clean` in runtime\build\release and game-build\build\release
fixed it (all 172 pass). A sccache server another worktree started can point at a deleted worktree's clang-cl: set `SCCACHE_SERVER_PORT`
to a free port for your own builds.

**Session 2026-11-08 (D1-P: checks, compute-out lock, interval index).** Checks (RTX 4080, logs of this branch): (1) the guest page
table (`pt::g_buf`, 32 MiB, MemoryUse::Upload) lives in **device-local host-visible memory (resizable BAR, type flags 0x7)**: vulkaninfo
shows type 4 DEVICE_LOCAL|HOST_VISIBLE|HOST_COHERENT on the 15.7 GiB VRAM heap, and the new info line `vk: host-visible buffer nvn guest
page table ... device-local (resizable BAR)` confirms it; moving it was not needed (D2's VRAM-leaf idea has nothing to gain from the table's
placement itself). (2) **No draws run on linked pipelines** once the cache is warm: `nvn.pipe: pipeline binds over 10.0 s: 0 of linked
pipelines, 120-200k of whole ones` (first scene). (3) The game's nvnSyncWait waits on **queue fence syncs** (nvnQueueFenceSync), 2.0
submissions behind the newest, ~220 of ~320 calls per 10 s actually wait; its window acquire syncs are always signaled already
(`nvn: nvnSyncWait over 10 s on ...` lines). Changes: `out_fetch` submits and waits with the compute-out lock free (`[nvn]
out_fetch_unlocked`; entries marked fetching, found again by id; neutral in fps: guard faults are rare now, the render thread's lock waits
were < 0.01 ms per frame both ways). **Interval index** (`[nvn] out_index`): the held-result overlap questions (~17 M per 10 s in Link's
house, ~35 entries) are answered from sorted merged page intervals rebuilt when their version moves (~45k rebuilds / 10 s, ~0.2-0.4 ms per
frame), plus per-op page dedupe and a no-overlap cache for the pointer windows in `gmem_before_op` (~60% hits). Paired windows (`[nvn]
out_index_ab = 300`, 854x480): Link's house 47.5 -> 51.8 fps, render-thread replay 20.5 -> 17.9 ms, 1% low 34.4 -> 34.8; Kokiri outside
21.2 -> 23.8 fps, median frame 46.6 -> 36.4 ms (1% low 15.7 -> 14.7); first scene 50-260 s 26.5 -> 28.9 fps, replay 35.3 -> 29.1 ms, and in
its heavy late part (~1500 draws more per frame) 15.8 -> 18.0 fps. These scenes were render-thread bound in these runs (Kokiri outside R
~45 ms at 22 fps, lower than earlier sessions' Kokiri walk: another view). Measuring: `[nvn] out_index_ab` / `out_fetch_ab` write one `ab
window` line per window (fps, 1% low, median, replay/idle ms, sync waits, lock waits); the capture replay does not exercise held results
(0 entries even with `compute_out_lazy = 1`), so in-game screenshots across on/off windows were compared (SSIM as within a window).
Pitfalls: the `kill` of the MCP CLI refuses a run another CLI process started unless `force=true` (a run kept going for minutes); several
agents share the scratchpad and one cleanup deleted its subfolders: keep scripts in a git-ignored folder of your worktree (`work\`).

**Session 2026-11-08 (D1-R: long passes, barrier masks, memo, gap counters).** Paired windows with `[nvn] cpuperf_ab_what` (one feature at a time; the trace's `cpuperf.rcpu` marks give the render thread's CPU ms per frame per window; compare an "on" window with the mean of its two neighbours, fps drifts over a run). In every scene the render thread's CPU time per frame was about the frame time (render-thread bound; GPU busy 30-80 %), so GPU-side changes do not show. (1) `submit_chunk_pass_draws` now defaults to **256**: first scene +2.7 fps (14 of 20 windows better, render CPU 23.3 -> 22.3 ms), Kokiri +0.4 / +0.2, Link's house +0.5 (noise); 120 cost Kokiri 0.6 fps (0 of 7 better). (2) Stage-scoped barriers after dispatches and copies: no gain (Kokiri -0.2, first scene +0.3 median), dropped: the full barrier at each pass end already drains graphics before every dispatch. (3) Scoping the set-reuse memo invalidation to the copied/written ranges: dropped (Link's house +0.07 fps, -0.13 ms render CPU; with an `out_overlaps` per binding -1.5 fps). The memo is cleared by results sent back only tens of times per run; the epoch moves on commands (Link's house: Barrier 1.37 M, SetRenderTargets 1.21 M, Dispatch 0.82 M, ClearColor 0.39 M, WaitEvent 0.25 M per ~250 s; the statistics block now logs `resolve epoch moves by command`) and on new passes. Game barriers end the render pass (`Op::Barrier` -> `end_pass`): about 110 per frame in Link's house, the next thing to look at for both the memo and pass breaks; WaitEvent is a no-op here and could keep sets. (4) `nxrt::GapCounter` for hot `gap()` callers (140 ns -> 1.8 ns per call). Measured while other sessions shared the machine: absolute fps varied a lot between runs (Kokiri 15-21 fps at times, against 36 earlier), so only paired windows of one run count.

**Session 2026-11-09 (render thread: barriers, set memo, per-draw caches).** New keys, all default on, each an AB feature of `[nvn] cpuperf_ab_what` (`cpuperf_ab` now also writes `ab window ... [cpuperf]` lines; `work/scratch/ab.py`-style pairing: each on window against the mean of its two off neighbours): (1) `barrier_keep_pass`: a game barrier in an open pass whose draws wrote only attachments (`g_pass_unsafe` false) keeps the pass (Link's house ~260 barriers per frame, ~half in an open pass, 77% of those kept; the rest follow storage-writing draws). (2) `epoch_keep`: Barrier, WaitEvent, SetRenderTargets, ClearColor/DepthStencil keep the resolve epoch. (3) `epoch_keep_dispatch`: dispatches and draws writing over held results keep it too; `out_version()` (the held-result index version) is in the set memo's and the draw-buffer-check memo's epoch instead (the per-draw bump at "the held result may have grown" was ~75% of all epoch moves in Kokiri). (4) `memo_pass_keep`: a new pass no longer clears the memo; the signature holds the render targets (feedback dummies), the epoch `g_tex_image_gen` (images made/shared/rescaled/released) instead of `g_tex_state_gen` (which moves on every layout change). (5) `resolve_cache`: texture bindings resolved once per command (was up to five times per draw, two shared locks each). (6) Page bitmaps (`bits_set`/`bits_any`) a word at a time: `out_add` from build_set was 3.7% of the render thread, 2.6% in per-page `fetch_or`. (7) `overlap_cache` in `out_draw_read` and build_set's storage check (`out_overlaps_window`: cached no-overlap answers per index version). (8) `bind_skip`: a graphics set already bound at its index with the same layout in the same pass is not bound again (~17% of binds, mostly the A7 pool-array sets). `set_reuse = verify`: 0 differing sets in Link's house and Kokiri with all of it. **Paired windows at 1920x1080 (all features on vs off, one build with the A22 network, alone):** first scene 33.8 -> 34.4 fps mean, median pair +1.35 fps, 1% low +4.3, render-thread replay -2.3 ms per frame (its static late part 22.0 -> 23.5 fps, replay 44.7 -> 42.4 ms); Link's house 45.6 -> 47.4 (16 of 18 pairs better, replay -1.3 ms); Kokiri outside 21.0 -> 22.1 (8 of 8, replay 48 -> 46 ms, median frame -2.6 ms). At 854x480 (before the merge, first four features): Link's house +0.5 fps (16/24), first scene static part 26.0 -> 27.0 fps (replay -1.5 ms). Profile notes (first scene, 854x480, sampler): render thread 33% idle there; build_set 20% inclusive (out_add 3.7, resolve_read 2.1, shadow_of 1.8, alloc_set 1.4 driver, sampler_for 1.3, tex_sampled_view 1.2), prepare_textures 7.4 (pool_set_sync 2.6, tex_ensure_current 2.5 = real uploads), out_draw_bindings 6.7 (out_overlaps 3.4), tex_or_null 2.5 (cache_entry_format, image creation), driver (nvoglv64) 20% of samples in all (end_frame 3, memory_barrier 1.8, submit 1.7). Ruled out by reasoning: push descriptors (a PUSH_DESCRIPTOR set layout changes every pipeline layout, so every cached pipeline would rebuild) and unified image layouts for the descriptors (the descriptor layout is already fixed per format; the per-draw layout checks cost < 0.3%). Set reuse stays at 13-18% of sets: 70% of sets are new after resolution (only ~30% hit the submission's set cache), so the memo's ceiling is low; the remaining cost is building new sets.
**(9) `tex_park` (the big one, nvn_gpu_texture.cpp):** a 1080 profile of Kokiri outside (render thread ~46 ms) had `tex_ensure_current` at 11% (`prepare_texture` / `make_linear` CPU untiles): OoT makes the upscaler chain's 1920x1080 RGBA8 and RGB9E5 targets and a 480x270 RG16UI image (all in a virtual pool) as **new texture objects every frame** (found with `[nvn] tex_prep_log = 1`: a new id each frame, 8.6 MB each, ~3 ms per upload on the render thread), the old object is finalized first, so the alias registry finds nothing and every frame made a new image and uploaded stale pool bytes the compute chain then overwrote. Now the GPU-written image of a finalized texture (refs 1, not sparse/streamed) is parked up to 8 presents (64 max) under (storage or virtual pool + offset, layout), and the next texture over the same storage with the same layout takes it back without an upload when no CPU write was reported there (`parked_cpu_wrote` from `textures_cpu_wrote`) and a 256-sample hash of the storage is unchanged (else uploaded as before). Statistics line `parked texture images (tex_park)`: ~34k of 37k taken back per 300 s, 0 stale. Screenshots Kokiri outside on vs off: SSIM 0.94-0.96, the same as two shots 5 s apart in one run (foliage/idle motion); first scene renders normally. **Paired at 1920x1080 (alone, after the G2 lifted rebuild):** tex_park alone: Kokiri outside 21.8 -> 28.5 fps (9 of 9, render replay -11.3 ms per frame), first scene 35.6 -> 37.7 mean (median pair +1.3, 10 of 15); everything of this session on vs off: Kokiri outside 21.3 -> 28.8 fps (9 of 9, median frame -12 ms, 1% low +4.4), Link's house 44.5 -> 48.9 (17 of 17, replay -2.6 ms). Left: the house and first scene are still render-thread bound (replay ~17-20 ms); next items by the profile: build_set's resolve_read/shadow_of/alloc_set, prepare_textures' pool_set_sync (a full pool scan each present and each new texture: `textures_created` is in its key), graphics_pipeline lookups (3.5% in Kokiri), out_draw_bindings (the overlap cache misses whenever `out_version` moves).

**Session 2026-11-09 (R3: render thread, Kokiri outside at 1080).** Frame model in Kokiri outside (5e1e7c7, 1920x1080, alone, ftrace +
sampler): frame 34.6 ms (29 fps), render thread replay 33.6 ms (~90% of it CPU, 9% waits inside run_list), GPU (`gpu.exec`) ~34 ms, main 13.9 ms
busy + 20.7 ms nvnSyncWait: **render-thread bound** (paired windows confirm: replay -1.1 ms gives median frame -1.1 ms). Render-thread profile
(inclusive): begin_draw 55%, bind_sets 30%, build_set 25%, out_draw_bindings 13%, prepare_textures 9%, do_dispatch 8.5%, out_overlaps_window 8%,
pool_lookup 5%, the driver ~20% in all. Kokiri has ~2,000 draws and ~3,100 descriptor sets resolved per frame; set reuse stays at ~22% because
**compute dispatches writing storage over pool-copy pages move the resolve epoch ~120 times per frame** (new statistics line `resolve epoch moves
by GPU write site`: compute storage 75%, graphics storage 17%, the rest gmem windows / held results). New keys (all default on, each a
`cpuperf_ab_what` name; CONFIG.md): `gmem_ro_bindings` (pointer shaders without `gmemw` no longer count every storage binding as written; did
not change the epoch moves in Kokiri), `overlap_grow` (the window cache of `out_overlaps_window` keyed by a counter that moves only when held
results may cover more pages), `out_index_inc` (out_add updates the held-result index in place: 507k in-place updates per ~150 s instead of
rebuilds), `flat_set_maps` (the set cache and set memo in an open-addressing table with O(1) clear: `_Try_emplace`/malloc gone from the
profile), `pool_sync_cache` (the A7 pool-array sync keeps an entry's view while texture, view stamp `tex_view_stamp` and request are
unchanged, and walks only registered ids, `TexturePool::used_ids`: 99% of entries kept, it asked every entry of a tens-of-thousands pool
before). Always on: `on_render_thread` from a thread-local (std::this_thread::get_id was a system call per held-result query), a last-view
cache in `view_for`, a sampler memo in front of `sampler_for`, tex_park's 256-word storage sample with one walk of the virtual mappings
(was one walk per word: ~1.4%). **Paired windows at 1920x1080 (all five keys on vs off, the always-on items in both, alone):** Kokiri outside
14 of 16 pairs better, +0.8 fps (29.5 -> 30.4), render replay -1.35 ms, median frame -1.05 ms, 1% low -1.75 (lower in most on windows; the
render thread idles 0.5-1 ms more there, so other limits show; not traced); Link's house 23 of 25, +0.95 fps, replay -1.42 ms, median frame
unchanged at ~19.5 ms (replay ~15-16 ms: **the house is no longer render-thread bound**); first scene 11 of 17, +0.35 fps, replay -1.25 ms,
median frame -0.6 ms. `set_reuse = verify` in Kokiri: 0 of 2.8 M reused sets differed. Picture: Kokiri outside screenshots with
everything on vs every new key off, SSIM 0.91-0.95 (two shots of one run 0.96; Link's idle animation), foliage and fog the same. Dropped:
skipping out_draw_bindings while its last check found no held result and none grew (`odb_clean`: 31 skips in a run, every dispatch adds
results); **scoping the GPU-write epoch moves to the memoized sets that bind the written range** (`memo_write_scope`: a write log checked
on each memo hit; verify clean, epoch moves by GPU writes 767k -> 1, but the memo was then cleared as often by texture image events
(~20 per frame: tex_park and image sharing move `g_tex_image_gen`), commands and results sent back, so reuse stayed ~19%: Kokiri 7 of 15
pairs, +0.00 fps; house 5 of 19, -0.2 fps). Set reuse would need all four clear sources scoped at once. Pitfalls: TaskStop of a background
bash leaves the script running (two of my runs then raced for one instance folder): kill the bash processes by command line; the game lock
must be held across a batch of runs (other agents take it between runs); `__udivti3` is not linked: a loop with two exit conditions over a
64-bit stride made clang-cl emit it (count the trip first). Left, by the profile: build_set's buffer resolution (resolve_read /
resolve_in_pool / pool_lookup ~14% of the render thread together), the epoch moves by compute storage writes (scoping them to the sets that
bind the written range is the remaining lever for set reuse), compute shadows (shadow_of ~2.5%), handle_lookup (2.4%, a contended counter
and the handle map under a mutex), the driver's own share.

Pitfalls seen: the occlusion change cut 670 render passes per frame to 45 (each ResetCounter/ReportCounter ended the pass); look for
similar per-op pass breaks first (`nvn.cnt.pass_breaks`, `gp:gap` counts). `gp:` timings with `prof.gpu_passes` drain the GPU at every
stamp: compare runs with the same setting. The replay of a capture runs with `compute_out_lazy` off by default (pass
`nvn.compute_out_lazy: 1` to test the game's path) and its CPU profile is not the game's.

**Lifted-code compilation (D1-L, 2026-11-08):** the lifted code is built with `/O2 -march=x86-64-v3` (`NX_LIFT_OPT`, `NX_ISA` in
`recomp/cmake/NxRecomp.cmake`; it was `/O1` = -Os and baseline x86-64, where every FMADD was a CRT `fmaf` call). `-ffp-contract=off`
stays (no FMUL+FADD fusion), so the results are bit-identical: difftest 566,331 of 566,331 at /Od and with the tests built at
/O2 v3 (that build found a real bug: `nx::fmax_`/`fmin_` took the NaN result from `a + b`, which the optimiser may commute; now
FPProcessNaNs order). Release builds for CPUs without AVX2: `set NX_ISA=baseline` before `build_game.cmd` (a cache value of the build
folder). The per-instruction helpers of ctx.h / neon.h / fpexact.h are `NX_HELPER` (always inline), the NaN slow paths
`NX_COLD_HELPER`; DMB/DSB LD and ST forms are compiler barriers; scalar FP writes store the V128 once (`vmk*`). Measured alone, 854x480,
base (main checkout's /O1 libraries) -> new, same runtime source, 160 s windows from 60 s: G (main thread, nvnSyncWait end to the next
one) p50 16.9 -> 13.8 ms (first scene), 12.0 -> 10.7 (Link's house), 17.2 -> 14.3 (Kokiri outside); fps 36.4 -> 38.7, 49.3 -> 47.9,
22.1 -> 22.0 (the last two are GPU / render-thread bound). main.lib 427 -> 485 MB; a full lift + build took 19.5 min with other
agents' builds and runs on the machine. `[nvn] compute_shadows` copies
every storage buffer of a dispatch to device-local memory and back (40 GB in 10 s during NDVE): pointless on an integrated GPU.

## D3: state and next steps (2026-11-08)

The gating experiments of the D3 bets (plan in the git-ignored notes `work/notes/plan_D.md` and `analysis_architecture.md`). All of them are
config keys that change nothing by default (`runtime/src/gfx/nvn/nvn_arch_exp.cpp`, keys in `runtime/docs/CONFIG.md`). Runs: first scene
(`load_state("first_scene")`, 854x480 internal), alone on the machine, RTX 4080 / driver 610.88; logs in `work/run/d3x/`.

| Exp | How | Result | Verdict |
|---|---|---|---|
| E2b | `caps:` lines at every start; `[gfx] exp_bda_probe` | 15.67 GiB device-local heap, **resizable BAR on** (type 4 device-local + host-visible, whole heap); descriptor_buffer, push_descriptor, unified_image_layouts, pipeline_executable_properties, device_generated_commands (EXT and NV), BDA + capture/replay: yes; **map_memory_placed: no**. Opaque capture addresses the driver did not hand out are refused (VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS; its own range starts at 0xe000000000), for device-local memory and host imports; a sparse buffer takes device-local memory only (types 0x2; host imports are 0xc) | no way to give imported game memory its guest address as device address, nor a flat window over host imports. A flat window is possible only over **device-local** memory (sparse buffer, 64 KiB tiles) |
| E1 | `[nvn] exp_lists` | per frame ~125 lists recorded (79% without draws) with only ~400-530 draws, while ~13,000 draws are replayed: of the re-recorded lists' draws 3-6% repeat a list of the last 8 frames exactly, 4-8% with addresses masked or as structure only | per-list command caching of re-recorded lists: no (far under the 60% bar). The `E1 submitted` line (g1.log): per frame 3,472 lists are submitted (called ones included); of their draw calls 580 are in lists recorded this or the last frame and 1,381 in lists recorded earlier: **~70% of the draw calls are in lists the game recorded once and submits again**. (Draw calls, not replayed draws: indirect and multi-draws count once.) A cache of the Vulkan translation of those static lists would be the only form of command reuse worth a look, after bet 1 |
| E4 | `[nvn] exp_emit` (volk vkCmd* pointers timed) + 100 Hz sampler | render thread busy 24-31 ms per frame at 31-38 fps: **emit (vkCmd*) 3.3-3.7 ms** (15-18k calls), descriptor alloc/update 0.75-0.9 ms, queue submit 0.4 ms, **decide 20-26 ms**. Decide by samples (inclusive, of busy): build_set 29%, held-result checks (out_draw_bindings / out_overlaps) 17% / 14%, prepare_textures 10%, pool_lookup 9% (8.5% self, the registry's shared lock), resolve_in_pool 7%, resolve_read 5%, tex_ensure_current 4%, resolve_texture 4%, pool_set_sync 3%, tex_sampled_view 2.4%, sampler_for 1.6% | the emit floor is ~3.5 ms: no parallel recording needed. The cost is our own resolution and coherence bookkeeping (bet 1); the descriptor API itself is under 1 ms |
| E6 | ftrace (`main.update` marks, svc waits >= 100 us) | frame 25.4 ms median: main busy 4.9 ms, main waits on its guest workers 11.6 ms (WaitForAddress), nvnSyncWait 9.0 ms; **game-logic critical path ~16.5 ms** (p90 18.5); workers are 75-85% idle | game logic alone caps at ~60 fps: the x86-64-v3 build (D1-L) and PGO / hot -O2 (D2) are needed for more than 60 fps |
| E2a | `[nvn] exp_coherence = 30` | CPU_CACHED pools: 43,940 changed pages among 1.77 M looked at, **41,802 flushed, 0 unflushed**, 2,138 written by GPU commands. CPU_UNCACHED: 19,206 changed without a flush (allowed: coherent). Guard faults on held compute results: CPU_CACHED read 57 (25 after an invalidate within 2 frames), write 9; CPU_UNCACHED read 38 / write 162, none after an invalidate. The game flushed 1.75 GB/s (whole buffers; far more than it changes) | **CPU side of the contract: clean.** GPU side: only 44% of CPU reads of results are preceded by an invalidate, so the guards stay as the fallback; and copying back at invalidates was measured before (`compute_out_invalidate = fetch`) to cost more than today. Bet 1.4 as planned (GPU-written ranges resident, copy back at invalidate) has no gain without moving every GPU reader onto the copy; its CPU half (mirror validity from flushes instead of write-watch arming) is clean but small (arming 0.4% of the render thread, ~130 write faults/s) |
| E5 | `[nvn] exp_pipeline_stats` on a replay (village capture, 117 executables: 23 hot ones + a random sample), against nvdisasm of the corpus block (scratch `e5.py`; blocks hold illegal words after the code, so it disassembles in chunks) | median over all: registers x1.10, driver code bytes / 16 vs SASS instructions x2.2. **Every gpt (pointer) shader explodes**: 52-76x the code (1.2-2.6 MB per executable: fbb731e2, 596bf624, 99908ab2, acf920b7, a5bfd19f, a873d795, b8b873e5, 301aa659, bb1cc3f5 mesh, 5332b131 vertex, ab6ef2bf fragment), 255 registers. **Hot fragment shaders use 2-3x the registers of the original** (957527e8 235 vs 94, 2972adf5 244 vs 93, 1e03316b 246 vs 94, 2bfc04a1 255 vs 112, 4097c13f / ddcf096b 168 vs 75-88): lower occupancy. Non-pointer vertex / mesh shaders are within 1.0-1.1x registers and 1.6-3x code | a new backend is not needed for ordinary shaders (within ~1.1x registers); the two targets are the pointer-access code (inlined per word, the cause of the 1.2-2.6 MB binaries and 14-70 s builds) and fragment register pressure (check after D1-S's fp32/fp16 changes, which removed fp64 from these shaders) |
| E3 | `NXRT_HARNESS_GMEM_WINDOW=1` variants of the 9 heaviest gpt mesh/vertex shaders (cache `work/shaders/d3x_window`: hard links of the main cache plus the 9 re-translated) + `[nvn] exp_gmem_window = 1` (sparse device-local window over the 12 GiB heap view, all 3,299 pools, 4.1 GB, copied in once) + `[nvn] exp_gmem_no_slots`; replays of the village capture back to back with `gpu_draws` | baseline (r1b) vs window (r2c): no change (f3054ac2 3.92 / 4.12, 6ec65625 3.90 / 4.06, fbb731e2 3.33 / 3.65 ms; unchanged draws +3-5%). Window and no slot search (r2b): f3054ac2 3.11, 6ec65625 3.02, fbb731e2 2.99, acf920b7 3.13 -> 2.72 ms: -18..26% against -5..10% for unchanged draws. **Pitfall found:** `gpu_draws` stamps drain the GPU after every draw, it clocks down, and every draw reads ~3.5x slower than under load (a first window run that copied 4.1 GB every frame showed all draws, unchanged ones included, 3.5x faster): compare stamp numbers only within one run or between runs with the same load | the page walk (with its per-invocation cache) costs nothing measurable; the bound-slot search ~15-20% of the pointer draws, i.e. well under 1 ms per frame at real clocks. **Bet 2 has a small fps ceiling**; its value would be the code size (E5) and the 14-70 s pipeline builds, not frame time |

Bets started:
- **Bet 1.3 step 1, `[nvn] pool_lookup_snapshot`** (nvn_objects.cpp): pool_lookup from an immutable sorted snapshot published at every
  register/unregister (one acquire load, last hit per thread), no registry lock. off | on | ab (paired windows with `[nvn] d3_ab = N`
  presents, ftrace marks `d3.on` / `d3.off`). Measured (first scene, `d3_ab = 300`, 11 pairs of ~7 s windows, alone): on 35.0 / off 33.1 fps, paired median **+1.9 fps** (noisy: the intro's camera moves). **Default on.**

Measuring pitfalls found: `[prof] gpu_draws` numbers depend on the GPU clock (the stamps drain the GPU, it clocks down; see E3); fps A/B runs need the machine to themselves (a full lifted-code build or another run beside them moves fps by 10-20).

Next steps, in order: bet 1.2
(bindless textures through the pool arrays) is the next structural item the E4 profile supports (texture resolve + prepare ~17% of the render
thread), it needs a harness mode where bound and uniform handles are read by the shader and every sample goes through the pool arrays, plus a
full re-translation (all pipelines rebuild: measure on replays first); with `VK_KHR_unified_image_layouts` available, the per-draw layout
checks of prepare_textures can go too. Bet 2 in its planned form (GPU address = device address) is not possible on this driver (E2b).

**Game side (G-1, 2026-11-09): what main waits for, local NZCV, vector NEON fast path.** G = main thread from the end of one nvnSyncWait to
the start of the next. Measured with `[prof] trace` plus the new `[prof] trace_svc_min_us` (svc events down to 10 us) and `kwake` events
(every kernel wake-up: waker's `wake_thread` to the woken thread running); scratch analysis scripts were in the worktree's `work\` (gside2.py:
worker concurrency during main's waits). Findings, first scene, 854x480: main waits ~10.3 ms per frame in **WaitForAddress at main+0x60188**
(the job system's "wait for job group" in f_710005fe60, ~19 waits per frame, 0.9 ms each); the wake-ups are fast (p50 7.6 us, p90 11 us for
main and for the workers, ~40 per frame for main, ~500 for the workers: 0.3 ms per frame of main's G), so it is not wake latency or a lock of
ours. During main's waits on average ~3.4 of the 6 job workers (guest-146..151; guest-141..145 a further ~0.4) run lifted code: the job graph
has limited width and its critical path is worker compute, so **lifted-code speed is the lever**. (guest-94..99 and 106..111 look busy in
such an analysis only because their waits never end inside the trace: the sampler shows them 100% asleep.) Worker busy profile (sampler):
72% lifted main code, `RtlSleepConditionVariableSRW` spin 3.5%, `dispatch_call` 2.3%, `fp_nan_result32` 1.1% (NaN lanes in the game's
vector math), the HLE thunk call counter (`fetch_add` on a shared line) 1.0%, `import_resolve` 0.8%.
Tried in the kernel and dropped/kept off: issuing `WakeConditionVariable` after the kernel lock is released (paired windows: G 14.85 off /
15.21 on, wake latency unchanged; removed); spinning main 200 us before it sleeps (`[sched] wake_spin_us`, default 0: main's wake latency
7.3 -> 0.2 us, G -0.4 ms in one paired run at 480, but at 1920x1080 with both lifted-code changes G 12.12 off / 12.37 on and no fps gain: left off by default).
Kept: (1) **function-local NZCV** (`GameConfig.local_flags`, `program.localize_flags`, `NxFlags` overloads in cxx.h; `NX_LOCAL_FLAGS=0`
for a lift without): a lifted function keeps the flags in a local and writes them to the Ctx only before tail branches out of the function,
the fall-through into the next one, inline/midhook sites and SVCs, never at BL/BLR or RET (AAPCS64: undefined there); it loads them at entry.
Before, every flag-setting instruction stored N/Z/C/V because the function might return. Static code: hot job-system functions -26..-38%
instructions (f_710005fe60 11,811 -> 7,364), NEON-heavy ones -2..-6%; oot.exe 443 -> 393 MB. e2e compares NZCV at the program's return
only with `NX_LOCAL_FLAGS=0` (the flags are dead there); e2e 30/30, hooks PASS, difftest 566,331 identical, runtime tests 173/173.
(2) **vector fast path in `v_f3v`** (neon.h): single-precision FADD/FSUB/FMUL/FDIV/FMLA compute all lanes as one host vector op and test
once for a NaN lane; only then the exact per-lane path runs (difftest identical; a first version broke the one-lane by-element forms
`fmul s, s, v.s[i]`, which the difftest caught). Measured (first scene, 160 s windows from 60 s after the scene starts, alone, base =
main checkout's v3 /O2 libraries with the same runtime): at 854x480 G p50 base 14.70 / 14.73 ms, local flags 13.74 / 13.26, both changes
12.79 (fps ~40 in all: render/GPU bound). **At the pinned 1920x1080** (after the A22 merge, two pairs): G p50 base 13.52 / 13.44 ms (p90 15.33 / 15.25), both changes 12.41 / 12.10 (p90 13.98 / 13.70): **-1.2 ms, -9%**; fps 35.9 / 35.7 base vs 35.7 / 34.8 (GPU / render-thread bound: main now waits longer in nvnSyncWait, 14 -> 15-16 ms per frame). So G is no longer what limits the frame; it caps near 80 fps now. The HLE thunk call counter is a plain increment instead of a locked add (`dispatch.cpp`).
Next on the game side: per-site caches for indirect calls / imports resolved at lift time (dispatch_call + import_resolve ~3% of worker
time), x9-x15 / v16-v31 as dead at calls and returns (same idea as the flags; riskier with hand-written SDK assembly), PGO of the lifted
code; the HLE call counter could become per-thread.

**Render resolution pinned (WP7, 2026-11-08, `runtime/games/oot/oot_drs.cpp`, no lifted rebuild).** The game's dynamic resolution: renderer
R = `[[main+0x4e30498] + 0x228]` (found by a pointer search from main's data at run time), its controller C = `[R + 0xb48]` (per-frame step
main+0x805aa0, called from main+0x31d3ee0): a list of 10 levels (end node C+400, first C+408; node: scale float at -8, level u32 at -4, next
+8), level 0 = 0.75 ... level 9 = 0.3334 of the output height R+3664 (1440); current level C+372, previous C+376; the scale in force R+3676
(taken over only when the level changes). On the RTX 4080 it sat at level 9 (854x480). The pin (frame-update entry hook, registered from
`GamePlay::install`): every level's scale = `[oot] render_height` / 1440 (default 1080 = 0.75 = **1920x1080, the console's maximum**; `0`
= the game's own dynamic resolution), and C+376 is made to differ from C+372 while R+3676 differs, so the game resizes its targets itself
(672 forced updates at start, none later). Log `oot: render resolution pinned to 1920x1080 ...`. **The upscaler network variants for
0.75 (f90d726d) and 0.667 (02dcb888) are broken in translation** (f90d726d: 8-px column stripes in 16-row bands, scalar and coop-matrix
alike; 02dcb888: blocky noise; fea98fe7 at 0.333 and 49659bca are right): `[nvn] pass_copy` (new, generic) makes their dispatches a scaled
copy tex0 (RGB9E5 composite) -> img1, OoT default until a shader fix (capture `work/captures/20261108-221243-drs-drs1080`, kept, shows the
stripes). Also: 02dcb888 takes 270 s to build in the driver and the fallback list does not cover it (black scene meanwhile; pass_copy
avoids it). **Measured at 1920x1080 (alone, before the second lifted build of that evening):** first scene 25-43 fps in its first 90 s
(median ~35; at 854x480 it was 34-39), render thread `replay` 25.4 ms and `gpu.exec` 27.7 ms per frame; Link's house 31-33 fps (854x480:
47-52), replay 27.0 ms; Kokiri outside ~16 fps (1% low 10-12; 854x480: 21-24), replay 56.6 ms (partly during another agent's build).
GPU per frame with `[prof] gpu_passes` (stamps drain the GPU): first scene 32.8 ms of stamps = 1920x1080 main pass 10.1 (526 draws),
gaps 9.1 (GPU waiting for the render thread), compute out 2.3, tail 2.2, shadow pass 1.6x1.6k 1.8, write memory 0.9, uploads/layouts 0.6,
compute ~2.5; Link's house 23.8 ms = main pass 7.0 (346 draws), gaps 3.4, compute out 2.3, tail 2.2, uploads/layouts 1.2, write memory 1.0.
So at 1080 the frame is still bound by the render thread and its waits (replay ~ frame time); the GPU-side items that are ours are the
copy-backs (compute out + tail + write memory ~5.5 ms) and the main pass's pointer shaders (E3/E5). No GPU change was kept this session.

## Frame capture and replay (since 2026-11-07, `runtime/docs/CAPTURE.md`)

**Use it to iterate quickly on rendering work** (translator fixes, shader bisecting, render-state bugs, GPU timings of one fixed frame):
capture a scene once, then test each change on the replay in seconds instead of booting the game every time. Confirm the final result in
the real game. **It is experimental.** A replay can differ from the game (see "State" below), so when the replay and the game disagree,
suspect the capture first. When you hit a problem in the capture or replay itself and fix it, tell the user what was wrong and what you
changed, and record it here.

**Debug rendering without booting the game.** A capture saves one whole presented frame: its command lists, the objects they reach, the
GPU state, the guest memory the GPU reads, the images the GPU rendered before it, and the game's own picture (`reference.png`).
`oot.exe --replay-capture work\captures\<folder> --instance rep` shows it again in about 5 s, looping, with no game code running.
`[nvn] skip_fs_file`, `trace_frame`/`trace_snap*`, `[prof] gpu_passes`, screenshots and SSIM against `reference.png` all work on it.
Shaders come from the shader cache at load, so a re-translated block shows on the next replay start without a new capture.

MCP tools: `capture_frame(name, instance=...)` (the game writes `work\captures\<time>-<instance>-<name>`, 3-10 s, ~4 GB; afterwards
only the newest `NXRT_AI_CAPTURES_KEEP` captures stay, default 5, plus those marked with `captures(id, keep=true)`: a file `keep` in the
capture folder), `captures(id)`, `replay_capture(id, instance="replay", seconds=...)`. A request file `capture_frame[.<pid>]` in the request folder or
`[capture] at_present = N` do the same without the tools. The way in: `load_state("first_scene")` (30-55 s on the 5800X3D/RTX 4080),
then `capture_frame`.

Each loop starts from the captured memory and images (`[capture] reset`, default on). Without it the picture drifts whiter loop after
loop: something the game's CPU side steers from frame to frame accumulates. In replay mode `[nvn] compute_out_lazy` defaults off, so
results reach the pools.

Things learnt on the way (capture of the first scene, 2026-11-07):
- the device's window origin and depth mode live outside `GState` (`gpu::set_window_modes`): without them the replay was upside down;
- reading rendered targets back into game memory (`download_texture`) covers too little (182 of 248 could not be: scaled,
  float16-promoted, depth). Images are saved raw instead (287 images, 857 MiB);
- ~10 `CallCommands` handles per frame are unknown when the frame is submitted, in the game as well (the gaps list's "CallCommands of
  an unknown handle").

**Later state (2026-11-07, evening):** two capture/trace bugs found and fixed. (1) Trace snapshots (`trace_snap*`, `snapm_` / `snapd_` / `snapa_`) were read at the traced frame's present without waiting for the GPU: the frame's later draws read as zeros, which looked like "the replay's colour-pass mesh records are 0 with the default pool copies" (A15/A17); `trace_dump_targets` now waits for the device first. (2) **The haze was a capture gap.** OoT's volumetric fog (compute 20593fd9 / 519c3773 / 33fdcf28 / 0007bc30 into 128x72x96 3D volumes, composited by fs f6a2e402) accumulates into its volumes in place and the game clears accumulators on the CPU, while the capture saved images and the GPU-written memory as the frame *left* them: every replay loop added a second frame of fog (R16F volume mean 1.0-1.8 on the replay vs ~0 in the game; without `[capture] reset` the picture went white within seconds). Leaving out f6a2e402 on the replay removed the haze, which is how the pass was found. Now the capture reads every pool-backed GPU-written image when it is armed, before the frame's first submission (68 images, 539 MiB, 0.4 s), and saves each GPU-written memory range at its first write in the frame (`capture::frame_submit_begin`, ~960 ranges, 73 MiB); memory.bin and images.bin hold those frame-start contents (runtime/docs/CAPTURE.md). Also: BC-compressed images are captured (two BC4 64x64x64 fog noise volumes could not be read before), and each unreadable image is logged. Fresh capture `work/captures/20261107-223248-crown2-deku_fs2` (Deku Tree view with crowns, this build): **SSIM 0.86-0.89 against `reference.png`** (was 0.53-0.58 on the same view, 0.73 / 0.64 before); the haze is gone, the remaining differences are sky brightness at the top corners and fine root detail. Captures made before this build still have the haze. **The extra leaf crown was a replay bug (fixed 2026-11-07, late):** the replay put back before each loop only the ranges the GPU wrote in its *first* loop. Dispatches left out of that loop (compute pipelines still building on their own thread, `[nvn] compute_background_spirv_kb`) wrote their ranges only later, so those ranges kept what the previous loop left. One of them holds the mesh draws' indirect records (the culling root's counts at 797045e000 in the Deku Tree capture): word 0 grew from 0x13 in loop 1 to 0xa9 in loop 12 and the GPU-culled foliage mesh draw (fs bb9b2ea9) drew ever more leaves. The write list now grows in every loop (`run_replay`, nvn_capture.cpp). Also a shader with global-memory stores (A17 fallback) now counts its own bound storage descriptors as written (`gmem_before_op`). After the fix the Deku Tree replay is stable at SSIM 0.877 after 90 s (was 0.80 with the crown; the rest is the fog, which changed with the A14 fix after that capture).

**State (2026-11-07, before that):** the replay of the Deku Tree view (`work\captures\20261107-121515-cap-deku`) has the reference's geometry, textures and lighting, and every loop is pixel-identical (SSIM 1.0 loop to loop). Against `reference.png` it scores SSIM 0.73: the remaining difference is a haze over the distance (the dark trees on the left are fogged in the replay, clear in the game). Not known yet whether that is a capture gap (fog volume or history data the reset misses, CPU data rewritten before the memory was read) or the game's own frame-to-frame state. The reset costs ~0.5 s per loop (it re-uploads all 857 MiB of images; only the images the frame writes would be needed): the replay runs at ~2 fps with it, ~10 fps with `[capture] reset = 0`.

## Techniques that worked

- Bring-up probes without a full main rebuild: `entryhook <addr>` in `main.sites` (one shard recompiles) plus
  `NXRT_DIAG_TRACE_SITES=<addr,...>` and `NXRT_DIAG_TRACE_PEEK=x1+4,...`: logs registers at each function entry (trace_sites now also
  registers entry hooks). Remove the probe sites again afterwards.
- Name NVN calls the game makes through its proc globals: `peek <GOT slot> 8 0` in an MCP `launch` script (with
  `settings {"diag.watchdog_seconds": -1}`), then look the thunk address up in a debug log (`nvnDeviceGetProcAddress("...") -> 0x74...`).
- Trace a frame range: `NXRT_NVN_TRACE_FRAME=1560-1700` (ranges now work; slow: every traced frame writes its targets as PNG into the
  game folder); `NXRT_NVN_TRACE_TEX_EVENTS=2` logs init/upload/render-target events of every sparse texture.
- **GPU values of a frame** (render targets and storage buffers as the GPU holds them, not the CPU's copy): trace one frame
  (`NXRT_NVN_TRACE_SEC=80`, `NXRT_NVN_TRACE_TARGETS=0`) and name the draws to snap: `NXRT_NVN_TRACE_SNAP=560,600` or
  `NXRT_NVN_TRACE_SNAP_FS=<fs hash prefixes>`, plus `NXRT_NVN_TRACE_SNAP_SAMPLED=1` (textures the draw samples) and
  `NXRT_NVN_TRACE_SNAP_SSBO=1` (vertex/fragment storage buffers). Then `tools\.venv\Scripts\python tools\ai\gpu_snapshots.py targets
  <run dir>` (NaN/inf share, median, max per target and draw: bisect where NaN first appears), `buffers <run dir> --draw N`, `words
  <file> --float`, `texel <file> x y --run <dir>`. Draw numbers are the `draw #N` of the trace; frames differ between runs, so prefer
  `TRACE_SNAP_FS` over numbers. Game addresses of buffers are stable between runs.
- **Which draw left a pixel wrong**: on a capture, `[nvn] trace_frame = 3`, `trace_targets = 0`, `trace_snap = 520-903` (ranges work) snaps the targets after every draw of the range; diff consecutive snapshots in a pixel region with `gpu_snapshots.decode` to find the draw that writes it (or the prepass draw that wrote its depth, when the colour pass fails its EQUAL test). Unknown `CallCommands` handles now log why they are gone (`nvn.handles`: dropped by an overlapping newer list, finalized, pool released; empty lists are not logged).
- Thread dumps of a running game: create the file `game-build\build\release\dump_threads` (the watchdog writes all stacks to the log; a
  named instance looks in its instance folder; `dump_threads.<pid>` reaches one process only).

- Name an import of main: lifted stub `void f_<plt>(Ctx*) { host_import(c, NX_IMPORT_ID(0, N)); }` -> N is the index into main's
  `img.symbols()` (`nxrecomp.program.open_image`); demangle with `tools\llvm\bin\llvm-cxxfilt.exe` (absolute path).
- Read one lifted function: grep `void f_<addr>(` in `work\lift\oot\<module>\c_NNN.cpp`.
- Find who writes a global: globals are reached through the GOT; find relocations whose addend is the global (`img.relocs()`), then code
  loading that GOT slot (`adrp page` + `ldr [page + off]`) and storing through it.
- Commands of an sf interface: find the proxy vtable through the relocation whose addend is one thunk (`img.relocs()`), then read each
  slot's lifted thunk: it loads the command id and calls a shape helper with buffer/raw sizes (e.g. `x2 = 0x1b4, x3 = 0x65,
  f_75000ea990` = cmd 101 with a 0x1b4-byte out buffer). The vtable starts with 5 non-command slots.
- IPC request layout: SDK sf clients build CMIF with magic `SFCI` (0x49434653); command id, then fields at +16.. in the raw data; a set
  top bit in the header means "sends PID". Replies with a moved handle are session objects (runtime: `rsp.push_object`).
- svcBreak with Result 0 = an `NN_ABORT_UNLESS`-style check: read the caller just before `AbortImpl` in the lifted code.
- Discovery can be inspected with `nxrecomp.funcs.analyse(img)` (rows with src E/S/B/R/T/A/G and end_from). When the game calls an
  address that is "not a known function", check discovery for that address first (boot fixes 12/13).

## Gaps list: what is not implemented (since 2026-11-06)

Each run writes `gaps.tsv` next to its log: every request the runtime only stands in for, counted (NVN entry points without an
implementation, operations the back end leaves out, draws skipped or drawn without an untranslated stage, dispatches skipped, programs with
an untranslated block, unhandled IPC commands, unimplemented imports, unknown SVCs; `runtime/include/nxrt/gaps.h`). The end of the log has
the 25 most frequent (`[gaps]` lines). `tools\.venv\Scripts\python tools\ai\gaps.py <run dirs...>` merges runs into a ranked work list,
joins shader hashes with `results.tsv` and groups blocks by translator error (`tools/ai/README.md`, "Gaps"). Use it before asking the user
what looks wrong; add `nxrt::gap()` to any new stand-in. First list (run `work\run\gaps_1` in worktree quirky-panini-7286ba, first scene,
240 s, fresh main cache): VRS palette image not converted (670k binds, perf only), `nvnCommandBufferSetGeometryProcessingMode` stub (203k),
`DrawMeshTasksIndirect` / `DrawMeshTasks` stubs (A9/A15), GPU-side WaitEvent (93k), **`CallCommands` of an unknown handle skipped (24k:
whole command lists not replayed)**, compute f2422a7a skipped (1,580 dispatches, unsupported `.UNKNOWN_243`), fragment 471bac84 untranslated
(`.UNKNOWN_526`, its 1,393 draws run without a fragment shader), 1,444 draws without a usable render target, 98 task/mesh blocks, 8
vertex/fragment blocks failing with an IndexOutOfRange in `GetConstantBufferBinding`.

## AI tools for the running game (`tools/ai/`)

MCP server `game` (registered in `.mcp.json`, enabled by `"enabledMcpjsonServers": ["game"]` in the local, git-ignored
`.claude/settings.local.json`; loads only in sessions started after that; tools `mcp__game__*`), also a CLI:
`tools\.venv\Scripts\python tools\ai\mcp_server.py help`. Tools: `status`, `launch` / `restart` / `kill`, `screenshot` (PrintWindow,
works when covered), `send_input` (posted messages in the background, or SendInput with the game window in front), `wait_for`,
`compare` (SSIM against a reference), `log`, `reports` (crash / freeze / fatal). Details and limits: `tools/ai/README.md`; check:
`tools\.venv\Scripts\python tools\ai\tests\test_gamectl.py`. Names come from `game.toml` (`NXRT_AI_GAME`, default `oot`).
Verified only against a stand-in window: whether PrintWindow sees the Vulkan swapchain and whether the SDL window takes posted
input while unfocused is known only once `oot.exe` runs. `launch` sets `test.frame_source=tick` for scripts (OoT has no frame hook).
Several runs: `launch(instance="<name>")`, then `instance=` or `pid=` on every other tool; `status` lists every running game process,
other sessions' included (next section).

## Several runs at once (named instances, since 2026-11-06)

Parallel sessions no longer queue for one game window: `oot.exe --instance NAME [--instance-dir DIR]` (or `NXRT_INSTANCE`) is a run of its
own beside others. It writes everything of its own into its folder (default `<game folder>\instances\NAME`; the MCP server uses
`work\ai_run\oot\NAME`): log, crash / freeze / fatal reports, `dump_threads`, saves and temporary storage (empty at first), `controls.ini`,
`pipeline_keys.bin`, `shader_order.txt` (copied from the game folder the first time), `pipeline_cache.bin` (a hard link to the shared prepared
cache, renewed when that one is newer; **an instance never writes it back** unless `[instance] keep_pipeline_cache = 1`, since 2026-11-08:
every instance used to save a private copy of the whole cache, 2-6 GB, and ~50 instance folders held 150 GB; each instance start also
deletes the `pipeline_cache.bin` of other instance folders beside it that no running instance holds, except folders with the
`keep_pipeline_cache` marker file), screenshots, test output and the outputs of
diagnostic keys given as relative paths. Shared: the game files, the shader cache (the translator's writes are atomic) and the BC7 cache
(temporary name + rename). The full list: `runtime/docs/CONFIG.md`, "Named instances"; code: `runtime/src/core/instance.cpp`.
The window title ends in ` [NAME]`. One run per game folder without a name (as before), one per instance folder with one.

Rules for agents:
- With the MCP tools: `launch(instance="<your session's short name>")` whenever another session may run the game; pass the same `instance=`
  (or `pid=`) to `screenshot`, `send_input`, `wait_for`, `log`, `reports`, `kill`. Check `status` first: it lists every running game process,
  whose it is, its memory and the free memory. From a shell: `tasklist /FI "IMAGENAME eq oot.exe"`, and start yours with `--instance`.
- Never end a run you did not start. `kill()` without arguments ends only the MCP server's own runs; another session's needs `force=true`.
- **Memory: at most two runs on this machine, and two in-game runs do not fit.** A run at the title holds ~5.5 GB, one in the first 3D
  scene 13-16 GB of working set and 18-20 GB of private bytes (measured 2026-11-06); the machine has 32 GB, of which ~18 GB were free with no
  game running (a WSL VM, `vmmemWSL`, held 24 GB of commit; with one game in its first scene under 1 GB of commit was left, so even a build
  could fail). Beside another run the exe refuses to start (exit code 7) when the free physical memory or commit, minus what the other runs
  will still grow by, is below `[boot] memory_per_run_gb` (default 13); the MCP `launch` checks the same before starting. A lone run is never
  refused. With the default a second run is refused here (18 GB free minus the first run's remaining ~8-12 GB of growth). Runs that stay in
  the menus (title, options, input tests) may pass `settings={"boot.memory_per_run_gb": 6}` to both; then neither may enter the 3D scene.
  Ask the user before assuming the machine has room.
- **Alone, with no other `oot.exe` running:** device-loss tests (A8: a GPU timeout can reset the whole adapter and kill the other run's
  Vulkan device too; once, on 2026-11-06, an A8 loss in one instance left the other at the title running, but do not rely on it), fps and
  frame-time measurements (the other run takes GPU, CPU and memory bandwidth), the shader preparation
  (`--prepare-shaders` refuses `--instance`; the "Preparing shaders" window is off for instances: it writes the shared shader data), and
  anything meant to write the shared pipeline cache or the default run's saves.
- A build beside a running game is fine if memory allows; still only one build in `work\build\oot` at a time.
- Checked live on 2026-11-06 (RTX 4080): two instances to the title through the MCP server; Enter to one reached only that one, each wrote
  only its folder, the game folder's `controls.ini`, `pipeline_keys.bin`, `shader_order.txt`, the shared `pipeline_cache.bin`, `prepare.state`
  and the default save stayed untouched; a second start of a running instance and the memory guard both exit 7.

**States and bug bundles (2026-11-06)**: tools `states`, `save_state`, `load_state`, `capture_bug`, `bugs` (`tools/ai/states.py`, README
"States" and "Bug bundles"). **No process snapshot** (guest threads live on host stacks of lifted frames, plus kernel/service/Vulkan
objects; nothing can write that out and recreate it). A state = the game's committed save data (`work/states/oot/<name>/save`) + reach
steps from `recomp/games/oot/ai_states.toml` (routes `title`, `new_game`, `continue`, `in_game`; built-in states `title`,
`first_scene`) + build/settings/env + the instance it came from. `load_state` copies the save into the save folder of the instance it
starts (`work\ai_run\oot\save`, or `work\ai_run\oot\<instance>\save`) and launches with `[fs] save_dir` there: the player's
`%APPDATA%\oot-recomp\save` is never touched (the folder was `OcarinaRecompPC` before 2026-11-08; the game moves it at its first start, `old_save_folders` in `game.toml`). A state or bundle starts the instance it came from again unless `instance=` says
otherwise (pass one while another session may use that name). The routes use the main shader cache `work\shaders\oot\spirv` (A8 fix in,
no loop cap) and `[media] video_max_pictures = 120`. OoT has an auto-save manager (`AutoSaveMgr`, `TriggerRequestAutoSave` in main's strings),
so states made right after an autosave are exact. Bug bundles: the player presses **F9** in game (note prompt follows; `[bug]` keys in
`runtime/docs/CONFIG.md`), or `capture_bug(instance=...)` (request file `capture_bug.<pid>` in the run's request folder: the instance
folder, or the exe's folder for the default run); they land in `work\bugs\<time>[-instance][-name]` of the checkout the exe was built in,
with the instance in `bug.json`; `load_state(bug=<id>)` starts from one. Every exe now logs `boot: built from commit <sha>`
(gen/build_id.h, rewritten at each build only when the commit or the dirty state changes).
Seen on the real game (RTX 4080): posted keys reach the covered, unfocused window (Enter = A on the title, F9); **PrintWindow/BitBlt
give black for the Vulkan swapchain**, so `screenshot` asks the runtime for its next presented frame (`screenshot.<pid>` in the
request folder) and never falls back to the screen by itself. `load_state("first_scene")` took 57 s (A taken at 27 s, 13,049 draws at 51 s). After
that the intro's timeline (German subtitles, voices) runs on over a black picture (A12) for minutes even though its video ended, and
in eight minutes the game wrote no slot (only the 320-byte slot index `account.sav`); `+` showed no menu. So a state past a real save,
and the `continue` route, are still unchecked: first get the scene to show (A12) and the game to save. Checked end to end
through the MCP protocol: `load_state("first_scene")` -> `save_state("after_intro")` -> `capture_bug` -> `kill` -> `load_state("after_intro")`
(29 s) -> `load_state(bug=...)` (24 s). Pitfall: a Git Bash `tail -f` on the run log (also one left behind by a stopped monitor) blocks
the log rotation of the next `launch`.

## Machine setup (this checkout, Windows 11)

| Tool | Where | Notes |
|---|---|---|
| Visual Studio 2022 Community 17.14 (MSVC 14.44.35207, Windows SDK 10.0.26100, CMake 3.31.6, Ninja 1.12.1) | found with `vswhere` | `recomp\scripts\nx_env.cmd` sets up the environment. The "Desktop development with C++" workload had to be added on this machine (the user ran the installer); VS 2019 Build Tools (MSVC 14.29) are also installed and would win without it |
| uv 0.13.7 + Python 3.13.18 | `tools\uv\` (git-ignored; Python in `tools\uv\python`, cache in `tools\uv\cache`) | no system uv here; zip from GitHub releases, SHA-256 checked (DEVELOPMENT_SETUP.md section 4). System Python is 3.11 |
| LLVM / clang-cl 23.1.5 | `tools\llvm\` (git-ignored) | official release `clang+llvm-23.1.5-x86_64-pc-windows-msvc.tar.zst` (SHA-256 checked; unpacking needs zstd window 2^31: `compression.zstd.ZstdFile(..., options={DecompressionParameter.window_log_max: 31})`); `recomp/cmake/clang-cl.cmake` prefers it over `C:\Program Files\LLVM` |
| .NET SDKs 6.0.408, 7.0.203, 9.0.304, 10.0.403 | system | shader harness builds for net10.0 |
| vcpkg (checkout `661e42cd`): libvpx 1.16.0, dav1d 1.5.4, opus 1.6.1 | `tools\vcpkg\` (git-ignored) | static, x64-windows-static; run `vcpkg install` through `nx_env.cmd`. Its binary cache is `%LOCALAPPDATA%\vcpkg\archives` (17 MB) |
| nvdisasm 13.3.73 | `tools\nvdisasm\` (git-ignored) | CUDA redistributable, SHA-256 checked |
| not installed on purpose | | pwsh (Windows PowerShell 5.1 runs the .ps1 files), Vulkan SDK (spirv-val, validation layer), `tools/setup` |
| sccache 0.19.0 | `tools\sccache\` (git-ignored) | picked up by the toolchain file at configure time; cache in `work\build\sccache` |
| Python 3.13 venv | `tools\.venv\` (git-ignored) | `recomp/requirements.txt`, `tools/ai/requirements.txt` (Pillow, numpy) and `zstandard` installed with `tools\uv\uv.exe` |
| `nx_plain_nso.exe` | `tools\extract\build\msvc\bin\` | built with MSVC from a VS x64 prompt: `cmake -S tools\extract -B tools\extract\build\msvc -G Ninja -DCMAKE_BUILD_TYPE=Release` then `cmake --build tools\extract\build\msvc` |

Disk: C: had 130 GB free before the setup (the `.nup` already in the repo root); the extracted game takes 26.3 GB, `work\` ~8 GB.

**Runtime tests** (`runtime\scripts\build.ps1 -Test`, builds in `runtime\build\release`, ~3.5 min clean; one group: `runtime\build\release\nxrt_tests.exe <group>`
or `nxrt_gfx_tests.exe <group>`, failures print `FAILED <file>:<line>`): **all 172 groups pass** (2026-11-08). `gfx_scene.windowed_screenshot_matches_the_same_reference` failed with mean diff 26.5 on this machine since Windows HDR is on: `[hdr] mode = auto` made the test window's swapchain HDR10 (PQ values in the screenshot); not a regression of any commit. The test now pins SDR output through `nxrt::extras::set_settings` (the settings are read once per process).
They configure since the default game module became `oot` when `games/mpr` is missing (`tests/gfx/test_prompts.cpp`, which needs mpr's
pak/txtr readers, is left out) and the lift tests find the repo-local LLVM. The 27 groups that failed on the first run were all stale
expectations, none a regression of the OoT branch: the upstream tests were written against the Metroid Prime Remastered module (never in this
repository), so with `oot` registered (`NXRT_TEST_GAME`) they met another title. What changed, so later tests follow the same rules:
- **Title facts** (program id, save sizes, display version, languages, firmware, account uid/nickname, aoc base id, SystemResourceSize) are
  compared with the registered game's `Info` / `TitleProfile` (`services::profile()`), not mpr's literals.
- **Platform**: tests whose numbers belong to one console say so with `nxtest::ScopedPlatform` (`tests/test_platform.h`): Switch 1 core mask,
  total memory, 4 GiB heap limit, entropy subtypes, NVN 55.15, the Switch 1 movie:: HLE; Switch 2 checks were added next to them (NVN2 231.40
  and the 0x62..0xA9 table, the Switch 2 GOB byte order, 10.5 GiB, kHeapMax). Block-linear layouts in tests take `gob_switch2` from the platform.
- **Host language**: with no `[system] language` the profile follows the Windows display language (German on this machine), so tests that check
  an encoding pin `[system] language = AmericanEnglish`.
- **Vulkan validation layer** (no Vulkan SDK here on purpose): the zero-validation-error counts print a note instead of failing
  (`tests/gfx/validation_layer.h`); `gfx_device.validation_layer_really_reports_errors` skips.
- **Play vocabulary**: play_config, play_engine, launcher_bindings_edit and launcher_tweaks test the shared settings code through mpr's words
  ("fire", "morph", contexts gameplay/morph/scan) and factory controls.ini: `tests/play_fixture/mpr_play_fixture.{h,cpp}` rebuilds that data
  and `NXTEST_GROUP_FIXTURE` (test_framework.h) registers it around each case of those groups. play_pad_live uses the game's own frame hook.
- **tt_core**: the fake game has mpr's memory shape; without mpr's probes a stand-in probe table reads it for the script/replay cases, and
  the cases that test mpr's probes and warp sequence skip (`probes_read_the_fake_game`, `warp_*`).
- Deliberate OoT changes now have tests: WaitIfEqual64 (`kernel_sync`), used memory = heap + main thread stack (`svc_info`), the bsd
  descriptor table (Socket / EventFd / Poll, `svc_network`), batched import counting (`nxmodule`).
- One runtime fix came out of it: in deterministic test mode (`[test] deterministic = 1`) the virtual clock converted sleeps and wall time
  at 19.2 MHz while it steps frames at the platform rate, so on Switch 2 a 1 ms sleep advanced 0.61 ms (`src/testtools/tt_core.cpp`; normal
  runs never use it).
Skips that remain on this machine are data-dependent (no `work/shader_cache`, no `NXRT_MOVIE_DIR`), hardware-dependent (no integrated GPU)
or need mpr (`tt_core` probes/warp). In a git worktree the git-ignored tools are missing: `mklink /J` `tools\llvm`, `tools\sccache`,
`tools\.venv`, `tools\vcpkg`, `tools\uv` and `tools\textures\build` from the main checkout before building. **sccache in a worktree:** a
cache hit replays the include list of the compile that filled the cache, so objects first built from the main checkout's cache carry
ninja header dependencies on the *main checkout's* headers, and a header changed in the worktree (a merge that touched `nxrt/gfx.h`) does
not rebuild them: four gfx groups crashed in `Overlay::build_ui` on a stale `overlay.cpp.obj`. After a header change in a worktree, clean
first (`ninja -C runtime\build\release -t clean`, then `build.ps1 -Test`); `ninja -t deps <obj>` shows the paths ninja watches.

## OoT pipeline

```bat
rem 1. program files only (about 40 MB); drop --exefs-only for the whole game (about 26 GB)
tools\.venv\Scripts\python.exe tools\extract\nup_extract.py oot.nup game\oot --exefs-only
rem 2. main is ZBIC-compressed: write plain NSOs for the lifter (checks every segment against the NSO header hashes)
tools\extract\build\msvc\bin\nx_plain_nso.exe game\oot\updates\131072\exefs game\oot\plain\exefs
rem 3. lift + compile (one module: append oot_sdk, oot_main, ...; keep going after errors: -k 0)
recomp\scripts\build_game.cmd oot
```

```bat
rem 4. the whole game (about 26 GB, hash-checked) for the runtime
tools\.venv\Scripts\python.exe tools\extract\nup_extract.py oot.nup game\oot
rem 5. once: the ASTC -> BC7 transcoder the runtime links (sources: tools\textures\scripts\fetch_third_party.ps1, pinned)
recomp\scripts\nx_env.cmd cmake -S tools\textures -B tools\textures\build\release -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl
recomp\scripts\nx_env.cmd cmake --build tools\textures\build\release
rem 6. runtime + oot.exe (two manifests exist, so -Manifest is required; Windows PowerShell 5.1 runs it, pwsh is not installed)
powershell -ExecutionPolicy Bypass -File game-build\scripts\build.ps1 -Manifest %CD%\recomp\games\oot\game.toml
rem    first time 211 s; incremental (~20 s):
runtime\scripts\env.cmd && cmake --build game-build\build\release --target game_exe
rem 7. run (the window opens on the user's desktop; --max-seconds ends it)
game-build\build\release\oot.exe --game %CD%\game\oot\updates\131072 --log %CD%\work\run\bootN\oot.log --max-seconds 60
```

`runtime\scripts\env.cmd` now just calls `recomp\scripts\nx_env.cmd`; `game-build/scripts/build.ps1` used the upstream layout
(`..\nx\runtime`) and now uses `..\runtime`. The runtime fetches SDL3, Vulkan-Headers, volk, VMA and ImGui with FetchContent;
libvpx (video) is optional and not installed.

Outputs: generated C++ in `work\lift\oot\<module>`, objects/libraries in `work\build\oot`, build log of the first run in
`work\build_oot.log`. `game\`, `work\` and `*.nup` are git-ignored: never commit game files or generated code.
Tests: `tools\.venv\Scripts\python.exe tools\extract\tests\test_nup_extract.py`.

## What a change costs

Ninja is incremental and the lifter rewrites a generated file only when its content changes. Every shard includes `funcs.h` (all
functions of its module) and `nxrecomp/cxx.h`.

| Change | Recompiles | Time (18 cores, cold cache) |
|---|---|---|
| runtime / game exe C++ | none of the lifted code, relink only | seconds to minutes |
| code inside one function (inline site, patch) | that one shard | under a minute |
| function list of a module (a `midhook` in `main.sites`, new entries) | every shard of that module (main: 352) | about 20 min |
| `recomp/include/nxrecomp/*.h` or a new game version | everything (407 shards) | about 25 min |

sccache makes repeats of the same content cheap (toggling debug sites on and off). Turning sccache on changes the compile command,
so the first build with it recompiles everything once: when only a few shards changed, build with `set NX_NO_SCCACHE=1` and let
the next build that is full anyway (header change, new game version) switch it on. The first OoT build ran without sccache; with
sccache on, a function-list change of main (20 shards with new content) rebuilt in 5 min. Any lifter source change re-lifts all
modules first (~3 min).

## Findings

- **NUP** (`.nup`) is a community container for *decrypted* Switch 1/2 dumps: https://github.com/a55uka/nup/blob/meow/SPEC.md. No
  keys. The OoT package holds the base 1.0.0 and the update 2.0.0 as full trees (not deltas, deduplicated extents), 372,587 file
  records, all stored raw, 25.6 GB. The update's RomFS overlays the base's.
- **ZBIC**: OoT's `main` has NSO flags `0xff` (bit 7 = ZBIC); `sdk`, `subsdk0`, `rtld` are LZ4 (`0x3f`). The repo's decoder
  (`runtime/third_party/zstd_zbic`) decoded it and every segment matched its header hash: the first real-game check of that decoder.
- **No NSP/XCI path** (removed 2026-11-08): `nx_extract` (the key-based NSP/XCI reader on top of `tools/setup`, which this repository never
  had) and the launcher's raw-dump page are gone. `tools/extract` builds `nx_plain_nso` (ZBIC -> plain NSO) and has `nup_extract.py`.
- **Zero-length FDEs**: OoT's main has three (module offsets `0x3470b00`, `0x3470b70`, `0x3470be0`) on code that is called.
  `recomp/nxrecomp/funcs.py` now ignores an FDE whose end is not after its start (the lifter crashed with an empty function).
- **FP16** (FEAT_FP16; the tier-2 lifter and its difftest corpus are Cortex-A57, without it): the generic vector emitters wrote
  `.f16` lanes that `V128` does not have, so the shard did not compile while the lift reported 0 untranslated. Seen as `FNEG v.8h`
  used as a sign-bit flip on integer data (4 times, `work\lift\oot\sdk\c_007.cpp`, none in main). Now `FNEG`/`FABS` on FP16 lanes are
  exact bit operations (`recomp/nxrecomp/lift/tier2/fp.py` `f16_sign`) and other FP16 vector arithmetic raises `Unhandled` in
  `neon.py` `v_un`/`v_bin` (counted as not translated, `guest_unimpl` at run time). Scalar FP16 (`h` registers) is not handled.
- Switch 2 code uses pointer authentication (`pacibsp`, `retab`).
- Lift sizes: rtld 44 functions, subsdk0 1,545, sdk 45,857, main 249,445 (15.4M instructions, about 100 s, about 1 GB of C++).
- Pokopia's `recomp/games/pkp/CMakeLists.txt` looks for `game\` one folder above this repository and links
  `../mpr/link_check.cpp`, which does not exist here. The OoT copy fixes both; pkp was left as it is.
- **The game exe has no live control channel** (no pipe, socket or stdin reader in `runtime/src` or `game-build/src`). The runtime
  test tooling (scripts, pad input, `@peek`/`@poke`, game probes) is configured at start through `NXRT_TEST_*` and answers through
  files (`result.json` at exit, `trace.jsonl` every ~64 frames, `shots/`). Live memory access would need a channel added to the
  runtime; a live external gamepad would need a virtual controller driver.
- **Batch files need CRLF.** `recomp/.gitattributes` had `* text=auto eol=lf`, so `recomp/scripts/*.cmd` were checked out LF-only;
  cmd.exe then resumed mid-line in `nx_env.cmd` after the build and ran its `exit /b 0`, so a failed build returned 0. Now
  `*.cmd`/`*.bat` are `eol=crlf`. Still, judge a build by `FAILED:` lines in its log, not only by the exit code.
- Upstream scripts assumed VS 2026 Build Tools and `C:\Program Files\LLVM`; `nx_env.cmd` and `clang-cl.cmake` now find VS with
  vswhere and prefer the repo-local LLVM and sccache.

## Rules

- Do not run two builds in `work\build\oot` at the same time. Two copies of `oot.exe` only as named instances, by the rules of "Several
  runs at once" (device-loss tests and fps measurements alone). Background jobs die with the session.
- Changing Windows Defender exclusions (for `work\`, `tools\llvm\`) would likely speed builds, but it is a security setting: only the
  user changes it.
- Keep `recomp/nxrecomp` changes game independent; game knowledge goes in `recomp/games/<id>/` and `runtime/games/<id>/`.
