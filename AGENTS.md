# Notes for AI agents

What a new agent needs to know before working here. Keep it current: add what you learn, remove what stops being true.
Repository basics are in `README.md`, the recompiler in `recomp/README.md`, the game boundary in `runtime/docs/GAME_BOUNDARY.md`.
**Start with `OOT_HANDOFF.md`** (status, the current blocker, next steps, where to pick up) and `DEVELOPMENT_SETUP.md` (everything a
development machine needs). Keep both current together with this file.

## State (branch `oot-nup`, 2026-11-06)

Porting **The Legend of Zelda: Ocarina of Time** (Switch 2, program `0400486024DEC000`, update 2.0.0 / `131072`) next to the
existing Pokopia port. Game id `oot`: `recomp/games/oot/` (manifest `game.toml`, `config.py`, `CMakeLists.txt`, `main.sites`).
The dump is `The Legend of Zelda Ocarina of Time [0400486024DEC000][v131072].nup` in the repo root (25.6 GB, git-ignored).

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
`.nup` extraction, NSP/XCI (not supported: `tools/setup` is not in this repository).

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
- **Launcher** (`runtime/apps/launcher`, mpr's, not built for OoT) accepts NSP/XCI/folder only; the Initial Setup (`oot_setup.exe`) takes `.nup` (through `nup_extract.py`) or an extracted folder. NSP/XCI need `tools/setup` (not in this repository).
- **Pokopia regression risk** (no Pokopia dump here): shared changes in funcs.py (header/MOD0, ADR starts, RETAA/RETAB, zero FDEs), a64.py
  (PAC decode), tier2 FP16, used-memory accounting, NVN minor 40, TLS +0x110, MapMemory Alias sources, WaitIfEqual64, bsd descriptors.
  Oracles, from `recomp\`: `..\tools\.venv\Scripts\python oracle/difftest/difftest.py all`, `oracle/e2e/run_e2e.py`,
  `oracle/hooks/test_hooks.py`; runtime tests `runtime\scripts\build.ps1 -Test` (all 170 groups pass, see "Runtime tests" below).

## Artifacts (the user wants every one fixed; add each one seen, mark it fixed with the cause)

| # | Where | What | State |
|---|---|---|---|
| A1 | title, intro | UI text invisible, then garbled. Two causes: the glyph pass picks its glyph records with instance + c[0][0x644] (NVN base instance, never written by the runtime; the harness now answers QueryHasConstantBufferDrawParameters true, so Ryujinx reads gl_BaseInstance), and the menu text is CPU-rendered into R8 textures with NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET, which the layout code untiled as block-linear (64-texel bands; found by re-tiling a dump and decoding it pitch-linear: "New Game"). Both fixed; the title reads "New Game" and "Options / ZELDA NOTES Setting" | fixed |
| A2 | title | black background: the title is a movie. Fixed: AV1 on dav1d (below) and the NDVE model hidden | fixed |
| A3 | title | faint vertical stripes in the logo/smoke area (x 340-940) over the black background (when the title movie is not behind it; with `[media] video_max_pictures` the movie ends and they show again on 2026-11-06) | open |
| A4 | title -> A | the screen stayed black after A: the game waited for the title movie (no video decoder). Fixed with A2; A now leads into the intro (tapestry movie) | fixed |
| A5 | setup window | blurry (DPI-virtualised) | fixed in code (per-monitor DPI), not yet seen |
| A6 | all | Opus audio was silent: `hwopus` decoded silence. Now libopus 1.6.1 (vcpkg `opus:x64-windows-static`, repo-local); the Ex open forms carry {rate, channels} in the raw data (OoT: 48 kHz mono, was read from a buffer: 0/0 and "buffer too small") | fixed (not listened to) |
| A7 | game (expected) | shadows of clustered (point/spot) lights are missing: 4,336 fragment shaders sample their shadow maps with TEX.SCR.B.DC through a handle read per pixel (light index from a storage loop), which the handle expressions cannot bind; the sample answers 1 (lit). Fix: pool-sized texture + sampler arrays in the runtime (Ryujinx GenerateBindlessAccess, harness QueryHostSupportsSeparateSampler true) | open |
| A8 | first scene | **VK_ERROR_DEVICE_LOST** 1-7 s after the first gameplay frame, every full run (2026-11-06), on the Arc iGPU and the RTX 4080. **Root cause: endless loops that Ryujinx's goto elimination built into ~14,700 translated pixel shaders.** The faulting draws used fs `dcfaa938040cb593` (then, skipped or capped, `8ba27a98e540767c`, then `ab5959d4f145c6fd`): a material/lighting family with a light loop (0x39d0-0x64d0, back edge `BRA 0x39d0`, exit `@P1 CALL.REL.NOINC 0x64e0`) and a jump-table switch inside it (`BRX R32 -0x4e90`, targets from c1; every case jumps to the switch end 0x60e0). `GotoElimination` works through the gotos last to first: the loop exit became `if (exit) break;` first, then the switch's gotos wrapped the statements around it in new `do { } while (flag)` loops (EncloseDoWhile / Lift: the six `while (temp_736)` and the `while (temp_879 \|\| temp_880 \|\| temp_879)` loops), so the break left only the innermost new loop and the light loop's `while (true)` never ended (GLSL: `if (temp_991) break;` inside the inner loop). **Fixed** in `tools/shaders/harness/translator/structured-goto-loop-exits.patch`: a new do-while forwards the breaks/continues of outer loops it encloses through a flag (`if (flag) { flag = false; break; }` after it; the flag is cleared at the function start, because a later goto may guard the statements before the loop). **Offline check** (`NXRT_HARNESS_CFG_CHECK=N`, `translator/StructuredCfgCheck.cs`; `tools/shaders/README.md`): random walks through the structured code, every block entry checked against the IR block graph. The three shaders fail at the first walk without the fix (after the exit block the code returns to the loop head) and pass 10,000 walks with it, every block and edge reached; the whole corpus (100 walks) has 14,684 failing blocks before and none after. The fix changes the SPIR-V of 14,692 corpus blocks (all pixel shaders; the 8 not in the failing list fail after 101-107 walks; resources and statuses unchanged); 17,094 blocks contain a BRX/BRXU or CALL.REL.NOINC (16,610 / 16,944). **On the RTX 4080** (cache translated with the fix, `.claude/worktrees/busy-hamilton-0e62aa/work/shaders/oot_a8fix`, not prepared, no loop cap; run `work/run/a8fix1` there): the first scene (13,040 draws) came at 62 s and ran to the 300 s deadline with no device loss (13-22 fps while ~15,000 changed pipelines built in game); the screen stays black there, as in the capped runs (separate issue). Control in the same setup with only the three shaders' old SPIR-V put back (run `work/run/a8ctl1`): scene at 64.6 s, device lost at 68.7 s in draw 12,607 (vs `f16b4a89c5478c05`, fs `dcfaa938040cb593`, the original A8 draw), exit code 4. Before the root cause was found, ruled out one run each: compute, the handle-expression and other new fragment shaders as a group, memory pressure, texture upgrades, the allocation count; `[gfx] device_diagnostics = 1` (checkpoints, fault report, NVIDIA event 153) named the draw. `NXRT_HARNESS_LOOP_CAP=N` (`spirv-else-if.patch`): every loop of a shader leaves after N back edges per invocation. **Since 2026-11-06 on by default for vertex/fragment shaders (4096; compute off; `=0` turns it off)**: a second runaway loop in the same material family (fs `253f607ff5823bd7`, named by device checkpoints in run `gp_3`) took the GPU to ~2 s frames / TDR / device lost 1-3 minutes into the intro scene (runs gp_1..gp_3 lost the device at 127-246 s; with the cap no loss in 420-450 s). Which input drives that loop is still open (the light-cluster grid was ruled out: counts 1-3, offsets reset each frame). To use the fix in `work\shaders\oot\spirv`: rebuild the harness, translate the corpus (or the changed blocks via the subset route) and merge, then prepare again (delete `prepare.state`: the number of usable programs does not change). **Done in the main checkout on 2026-11-06** (branch merged into `oot-nup`, corpus re-translated, 23-minute preparation with both depth-clamp variants, 2.34 GB cache): the first scene ran 3 minutes without a device loss and without the loop cap (`work\run\verify_merged`); the loop-capped test cache `work\shaders\oot_cap` is deleted | fixed |
| A9 | game | foliage/terrain use **mesh shaders** (NVN2 task stage 6 / mesh stage 7, `nvnCommandBuffer(Multi)DrawMeshTasks*`). **Implemented 2026-11-06 (worktree quirky-panini-7286ba, branch claude/charming-brahmagupta-90017e), partly verified**: the translator turns task/mesh blocks into VK_EXT_mesh_shader stages (`tools/shaders/ampere/frontend/AmpereMesh.cs`, `AmpereEmit.Mesh.cs`; ISBE semantics as in the Lichii emulator's notes; 770 of 800 mesh and 15 of 15 task blocks translate; limits from control +0x58 threads / +0x5a task payload bytes / +0x60 max vertices / +0x62 max primitives, output lines from the SPH OMAP, checked against the paired fragment shaders' IMAP), the runtime binds them as NVN stages 6/7 (stage bits 0x40/0x80; c[0] storage descriptors at 0x650 / 0x750, four slots past the other stages' pattern, measured from the task's pointer table), builds whole mesh pipelines (`[gfx] mesh_shader`) and replays the eight draw entry points; indirect records stay on the GPU (3D = {x, y, z}; plain = {count, first}, inferred from VK_NV_mesh_shader, copied into {count, 1, 1}). Also new in the translator: `F2IP.U8/S8.F32` (float pair to packed bytes; the culling compute f2422a7a and 3 mesh blocks). Seen (runs `work/run/mesh_8`, `mesh_9`): mesh draws rasterize in the depth prepass and the shadow cascades (a ground strip, leaf clusters at the top left); the GPU-culled colour-pass mesh draws still add nothing in a traced frame, and one run showed a bright band on the ground that baseline runs do not have (not explained yet). The picture of the leaves and the Deku Tree's nose is unchanged. Left: why the colour-pass indirect draws stay empty (record layout or culling inputs), 17 mesh blocks with OMAP system values A, 4 with colour/clip outputs, CALL forms, per-primitive outputs, `first` != 0 **2026-11-07 (runs `work\ai_run\oot\leaf1..leaf8`): the mesh stage's storage descriptors start at c[0] 0x770, not 0x750** (task stays 0x650). The depth-prepass mesh a24d8406 reads the GPU-written record buffer (count at word 3) at 0x770 where the game binds it at mesh slot 0; the colour-pass mesh b8b873e5 reads it at 0x790 where the game binds it at slot 2, and every other slot of both fits the same way. With 0x750 the colour-pass meshes read two unbound slots (dummy data): garbage vertices, which the depth EQUAL test against the prepass rejected, so they "drew nothing" (with `[nvn] debug_equal_as = 4` they showed as huge streaks). Fixed in `storage_c0_base` / `storage_slot_for` (nvn_replay.cpp), `set_c0_offset` (nvn_shaders.cpp) and the harness's gmem candidates (`StaticGpuAccessor.GlobalFallbackWords`); the 770 translatable mesh blocks were re-translated. Since then the colour-pass ground meshes fill the holes in the replay and write pixels in the game. Mesh pipelines take 86-124 s each in the NVIDIA driver the first time (on demand). | partly fixed |
| A10 | game | **compute programs read their resources through a per-program c[0] table, and OoT's post-process chain has a push constant area in it.** Layout (`apply_nvn2_compute_table`, `nvn_shaders.cpp`): 0x10-byte header, push constants, 0x10 per uniform block descriptor, 0x10 per storage buffer descriptor, 8 per texture handle, 8 per image handle, slot n = NVN binding n; counts at control +0xb1/+0xb2/+0xb3/+0xb6, table size u16 +0xba. The bfsha/vsetb corpus (3,741 of 3,743 OoT compute blocks) has no push area and already mapped. The chain's programs built into `main` (f05596b9, 54ecc4b4, 4c60af1f, fea98fe7, f90d726d) have **0x80 bytes of push constants** after the header (= table size minus header and slots; NDVE 0x40; control byte +0xa9 = push words read, 29), so every slot was 0x80 off. Confirmed in a frame trace: the game calls `nvnCommandBufferPushComputeConstants(stage 0, offset 0, 0x74 bytes)` before f05596b9 (words: 1/854, 1/480, 854, 480, 1/214, 1/120, 214, 120 ..., 853, 479) and f05596b9's table 0x158 = 0x10 + 0x80 + 10 ssbos + 4 textures + 1 image. Runtime: compute pushes carry `kPushCompute` and land in `SmallState::push_cs`; a program with a push area gets a per-dispatch c[0] in the upload ring (header 0, the pushed words, descriptors {address, size}, handles; `compute_c0` in `nvn_replay.cpp`). Traces print `push compute ...` and `cs c0 table ...` per dispatch. **Fixed: every resource of the chain resolves** (run `work\run\a10_8` in worktree quirky-panini-7286ba, `[nvn] upscaler_replace = off`): f05596b9 scene texture slot 3 -> RG16UI tile image slot 0, ssbos 8 (200 KB) and 9 (its 16 bytes are 54ecc4b4's indirect dispatch arguments); 54ecc4b4 ubo 10 as storage, ssbo 8, textures 4 and 6, image 1; 4c60af1f textures 3 (scene) and 5 -> image 2 (RGB9E5 composite); fea98fe7 weights ssbo 2 (12 KB fp16), texture 0 = the composite -> image 1 (R11G11B10F 2560x1440). Needed on the way: **RGB9E5 storage** (the shader packs RGB9E5 and stores the word through an r32ui image; the image now gets MUTABLE + EXTENDED_USAGE and an R32_UINT storage view, `uint_alias`); **buffers in a virtual pool** (ssbo 8 is at 0x200000382000 in a sparse pool: bound through the physical memory its `nvnMemoryPoolMapVirtual` mappings name, `physical_buffer_address`, at bind replay; before, the dummy buffer); **slow compute pipelines** (fea98fe7, 4.7 MB of SPIR-V, took 116 s in the NVIDIA driver on the render thread: programs over `[nvn] compute_background_spirv_kb` build on their own thread, dispatches left out until ready; the driver cache makes later runs instant); the translator's **TLD4 .B** (blue component with a bound handle, not bindless) and **TLD4 .F16(.RN)** (f90d726d translates; 25 corpus blocks failed the same way and should translate after a subset re-translate); trace dumps of buffers in a virtual pool crashed the render thread (`host_readable`). `[nvn] skip_unmapped_dispatch` stays as the fallback for a storage descriptor outside the table (none seen now). `[nvn] upscaler_replace = off` turns the OoT stopgap off (an empty environment variable does not exist on Windows) | fixed |
| A11 | intro movie | voice lines overlap (user, 2026-11-06), then the movie too slow against its audio. **Two runtime causes, both fixed (2026-11-06):** (1) `[display] unpaced_loading` (Pokopia's heuristic: light frames = loading screen, limiter off for up to 20 s and for 1.5 s after each stall) took the intro's 3-draw movie frames for loading: the game ran at 300-500 fps and the movie player, which advances one picture per frame, raced through ~250 pictures/s while the voice lines (Opus, started along the movie's timeline) piled up. Now frames stay paced while the game reads decoded pictures (`note_frame_for_pacing`, log `pacing on: movie`). (2) `fps_limit = auto` capped the game at the display's refresh rate, and the user watches through Remote Desktop (Microsoft Remote Display Adapter, 32 Hz): 32 fps, and OoT's **60 fps** movies (`Dm_Kokiri_0000.webm` is 154 s, 9,254 pictures, DefaultDuration 16.67 ms) played at half speed (291 s). OoT has no timestep sites, so its step is a fixed 1/60 s per frame: auto now means `[display] console_fps` (60) for such games and the display's rate only for games with timestep sites (`apply_frame_cap`, log line `play: frame cap`). Checked: intro 157 s at 60.0 fps. Also the stand-in vsync event (`vi_nv.cpp`) now runs on an absolute high-resolution 60 Hz schedule (a relative 16.667 ms wait became 16 ms ticks on Windows). The ~30 s gaps between music tracks the user heard were probably the same half-speed game clock | fixed |
| A12 | first scene | **3D scene black** (only subtitles and UI; seen once A8 was out of the way with the loop-capped cache). The scene renders fine at a low internal size (854x480 or 960x540: G-buffer, lighting, HDR targets all have content in a `[nvn] trace_sec` frame trace with targets), but the 2560x1440 HDR target `t486` that the final composite (fs `c63c3b8f`) samples is only cleared and blended with a solid-colour overlay (fs `5223ec5a`, a fade). The game's upscaler fills it: a chain of compute passes, all broken here: `f05596b9` (storage at c[0] 0x110/0x120 unmapped, A10, skipped), `54ecc4b4` (TLD4.SCR.R/G.PTP: not translated), `4c60af1f` (runs, but its textures and output image come from c[0] words, A10: nothing bound), `fea98fe7` / `f90d726d` (the tensor-core network, HMMA plus TEX.SCR.F16.RN: not translated). The compute passes get their textures and their output **image** from per-program c[0] words; no image is bound through nvnCommandBufferBindImages. Translator fixed for TEX .F16(.RN) (Rd = packHalf2x16(c0, c1), Rd2 = (c2, c3); confirmed by the shader's use of the halves) and TLD4.PTP (Maxwell layout: 8 signed 6-bit offsets in 8-bit lanes of two Rb registers; textureGatherOffsets); `fea98fe7` translates to 4.8 MB of SPIR-V. Instead of emulating the network, the upscaler is **replaced** on the PC (user's decision): `[nvn] upscaler_replace/skip/src/dst` (OoT defaults in `runtime/games/oot/oot_data.cpp` through the new `game::Info::config_defaults`): the chain's first pass becomes a scaled copy of the scene into the texture the composite samples (`upscaler_src = depth_rt`: colour target 0 of the latest draw with depth; `upscaler_dst = fs:c63c3b8f:tex0`, taken from the previous frame's last composite), the other passes are left out. Untranslated blocks from `main` can be dumped with `[nvn] dump_missing` (`code_<hash>.bin` + `ctrl_<hash>.bin`; compute code starts at byte 0). **Result: the scene reaches the screen but washed out white** (`work\run\upscale3`): the chain is the game's whole final post-process, not only an upscaler. The composite `c63c3b8f` has one texture and no uniforms (it only converts to 8 bits), so the chain writes an exposed, tone-mapped, composited image; the latest depth draw's target is a near-white layer (values 0.25-1, mean 0.98: haze or fog), the HDR scene is another target (`t10348`: up to 25, mean ~1), and the network pass reads several of these. A faithful replacement would have to redo exposure, tone mapping and compositing; the proper fix is running the chain, which needs A10 (session "Implement the NVN2 compute c[0] resource table", 2026-11-06). The stopgap stays until then; switch it off with `NXRT_NVN_UPSCALER_REPLACE=off`. **With A10 fixed (2026-11-06) the whole chain runs, and the picture is still washed out white** (`a10_8`): so the brightness is not the stopgap's doing. Replacing only the network pass (`upscaler_replace = fea98fe7`, `upscaler_skip = none`, `upscaler_src = tex0`, `upscaler_dst = img1`: a scaled copy of the game's own RGB9E5 composite) gives the stopgap's picture with detail (`a10_9`): the overexposure/haze is already in the scene and composite (exposure source, fog or lighting: next to look at), and the network's own output loses detail against that copy (translation of fea98fe7's HMMA / FP16 code suspect, unverified). The OoT default is unchanged. **Checked in the main checkout after the merge** (2026-11-06, run `work\ai_run\oot\verify`, stopgap off, the 196 previously failing corpus blocks re-translated with the TLD4 .B/.F16 fix: 12 more ok, cache prepared again in 57 s): no device loss, rock detail visible, picture still overexposed white, 14 fps **Overexposure traced (2026-11-06, worktree quirky-panini-7286ba, runs `work\run\a12_*`):** the frame is a forward renderer (scene in an 854x480 R11G11B10F target), then bloom (fs caf7eb45 bright-pass: luma (0.299, 0.587, 0.114) + 0.5 * alpha, strength 0.15, luminance cap 10; 752b2104 downsample, cc8d0787 / 3f0dc2b1 5-tap blurs, cdaf772f upsample), then **the tone mapping is fs ce217436** (`ACES((max(0, M * scene) + pow(bloom, c3[3].y = 1.5)) * c3[3].z)`, M about identity, exposure **c3[3].z = 1.0 in every traced frame**), then the compute chain. Not a cause, despite appearances: the trace's `clear ... rect` is taken before the clear's pass opens and clamped to the previous pass's extent (the 854x480 tone-mapping target showed 240x135); the new `clear applied` trace line shows every clear covers its whole target (run a12_12). Measured with raw dumps (snapshots now write `.bin` for float formats; decode B10G11R11 yourself: the stats line the runtime prints for such snapshots is unreliable, one channel reads +-1e30): sunlit ground (16.5, 12.5, 8.5), sky (0.6, 1.1, 2.4), so the scene is physically scaled and needs an exposure of about 0.05; the bright-pass output sits at its cap for most pixels and the final 240x135 bloom has a mean of 4.5. Ruled out, one run each: fog passes (cfc67237 ... 69d4ca92 change the scene by under 10%), light-loop over-iteration (the frame's 24 material shaders re-translated with `NXRT_HARNESS_LOOP_CAP=6`: same picture), storage bindings smaller than what shaders read (`[nvn] storage_min_range = 4096`: same), a missed compute readback (`[nvn] compute_out_lazy = false`: same), the network pass (a plain copy instead of fea98fe7 is as white). **Cause found (same day): the graphics stages' storage buffer slots are swapped in pairs.** The material shaders read 96-byte light records (type at +16) through fragment c[0] 0x520 and a cluster index through 0x510, while the game binds the light list at slot 0 and the 9.9 MB GPU-built cluster grid at slot 1; likewise 0x530 is the sky lighting (spherical harmonics that compute 16a42858 builds from the sky panorama, slot 3) and 0x540 a 64-byte per-material table (slot 2). `[nvn] graphics_ssbo_slot_xor = 1` (OoT default; slot n at base + 0x10 * (n ^ 1), vertex and fragment: fragment alone left the scene white) makes the first scene render with its intended look with the stopgap off: Great Deku Tree, forest, ground textures, sky (runs a12_18, a12_20, a12_21 in worktree quirky-panini-7286ba). How the game found it: `[nvn] ubo_backtrace` (guest backtrace of a uniform bind by contents) showed the tone mapping's exposure is a constant 1.0 by design, so the scene itself had to be wrong. **Still open:** the upward view of the tree against the sky is washed out white in some frames and right in others (sun bloom or exposure adaptation; same camera view, both seen), and one of three runs past 115 s lost the device (instruction-pointer fault after a 4 s GPU stall, the A8 signature; a run with `[gfx] device_diagnostics = 1` did not reproduce it). Also answered on the host: nn::vi QueryDisplayHdrStatus / HdrPaperWhite / VrrSupported as an SDR display (no visible effect). **Corrected later the same day (worktree quirky-panini-7286ba, runs a12_25..a12_32, wb_2, wb_3): the slot swap was wrong.** The real cause of the white frames was the translator: OoT's sky-lighting compute 16a42858 sums spherical harmonics with RED.E.ADD.F32, which was emitted as an integer add of float bits (garbage like 1.8e29 in the SH buffer, NaN in every material; fixed in AmpereEmit.cs, CAS loop). The swap only steered the SH read (c[0] 0x530) away from that garbage, and it fed the material shaders the light list where they expect the cluster grid (0x510/0x520): NaN, the three per-draw lightmap textures unresolved (0x540 pointed at the 192-byte SH buffer instead of the 5.7 MB material table) and a runaway light loop (2 s frames / device loss). With the float atomics fixed and the plain mapping (`graphics_ssbo_slot_xor` back to 0, no longer an OoT default) the first scene has no NaN, the lightmaps resolve and it renders as intended from the first frame (wb_3). Found with `[nvn] trace_snap_ssbo` + `tools/ai/gpu_snapshots.py`. Still to check: a bright patch near the water, the green glow, the inset rectangle. **2026-11-07 (late):** the bright patch near the water was not water: see A14 (texture aliasing in virtual pools). The green glow is A13. The inset rectangle was not seen in 14 game shots and 3 replays of the first scene (runs `work\ai_run\oot\nb1`, `nb3`): no description of where it was is left, so it stays open until it is seen again.  **Upscaler stopgap retired (2026-11-08):** on capture `20261107-230737-nb3-alias_kid` the game's own chain (stopgap off) gives the right picture: same brightness and colour as the copy (mean RGB 140.8/130.9/99.4 vs 141.2/131.3/100.1), sharper and anti-aliased (mean luma gradient 1.98 vs 1.34; jagged edges on Link's hat and arms with the copy), so the network pass (HMMA/FP16) is right; it costs ~2 ms of GPU time per frame on the RTX 4080 (last submission 164.1 vs 162.0 ms in the profiled replay, `work/run/ups_cmp`). The chain is the OoT default now; `[nvn] upscaler_fallback = 1` (OoT default) lets the scaled copy stand in only while a pass cannot run (the network pipeline fea98fe7, 4.7 MB of SPIR-V, takes 2-3 minutes to build the first time; the scene was black meanwhile). Screenshots `work/run/ups_cmp/{stopgap_on,chain_on,fallback_chain,crop_link}.png`. `[prof] gpu_passes` misses the chain's dispatches (no `cs` stamps for them; open); its stamp cap per submission went from 1,024 to 8,192. | open |
| A13 | first scene | **Green blocky glow** on the ground/water (user, 2026-11-06). Measured (run lf_1, scene snapshots every 25 draws, `tools/ai/gpu_snapshots.py`): between draws 575 and 600 ~3,000 HDR pixels get **NaN in the G channel only** (R11G11B10F words like 0x4e3ffa47: G field 0x7ff, R and B normal) over a normal value; bloom spreads them and the tone mapping shows pure green blocks. Not from fs 0f3f9223, c1f31e29, 33d8129d, 73e29464, 76ea7f02 or 253f607f (each skipped with `[nvn] skip_fs_file`, green stayed). The view changes between runs, so the next step needs a capture of the exact frame (snapshot names per frame, or a NaN check per draw). **2026-11-07 (late): not seen again** in runs base_1, nb1 and nb3 (first scene to the Kokiri village, 14 shots). The cause is not proven: the HDR NaN could come from the shaders that read the scene depth through a second texture object, which sampled zeros until the virtual-pool alias fix (A14); reopen with a capture if the green blocks come back. | not reproduced |
| A14 | first scene | **White blobs** on the Deku Tree top and on water/stones, flashing (user: not the sun). With A12 fixed the HDR scene's median is ~2.5, max ~19, and the bloom input is sane in calm frames (wb_3); the blobs come and go with the view (lf_4 shot 4: water and stones). Likely the same family as A13 (non-finite or huge values in some blended draw). **Fixed 2026-11-07 (late).** Two causes. (1) The blobs on the water and stones were holes in the ground showing the sky (and the sun) through bloom: the stream bed is a heightmap terrain (TerrainGround, vs adedb129 / fs 73e29464, blended layer, depth test EQUAL against its own prepass 8880001616dd97c0). Its fragment shader fades by the scene depth, which it samples from texture t19533; the game renders that depth into t19625, another texture object over **the same virtual-pool range** (same pool, offset, format, size). The runtime shared one image between such twins only for textures with host storage (`texture_registry_find_alias` keyed by `mem`), so t19533 was a separate, never written image (all 0), the shader got alpha 0 and nothing covered the sky. Now textures in a virtual pool alias by pool + offset + layout (nvn_texture_registry.cpp); in the first scene ~20 render targets share their images this way. Found by snapshotting draws 225-1112 of the capture `work\captures61107-223924-nb1-water_patch` (`[nvn] trace_snap` takes ranges now) and tracing which prepass draw wrote the hole's depth. Before: `work\ai_run\oot\nb1\shots\shot_223930_889.png`, `shot_224243_262.png`, base_1 `shot_2_t170`/`shot_4_t242`; after (game): `work\ai_run\oot\nb3\shots\shot_230304_996.png`, `shot_230338_724.png`, `shot_230729_527.png`, `work\ai_run\oot\nb4\shots\shot_232802_697.png` (the view of base_1 `shot_4_t242`; nb4 = final build, 12-14 fps as before); the fog volume and god rays, which read the same depth, changed with it. (2) The blob over the Deku Tree's face (base_1 `shot_5_t283`) was the missing nose showing the sky (A15, fixed in 3052f92). Capture `work\captures61107-230737-nb3-alias_kid` (this fix): replay SSIM 0.956 against its reference. | fixed |
| A15 | first scene | **Leaves missing / Deku Tree's nose missing** (user: dark-blue blobs where leafy trees should be, sky visible where the nose should be). The game calls `nvnCommandBufferDrawMeshTasks(Indirect)` (stubs, A9: nothing drawn), and the two-sided foliage-like draws (fs 0f3f9223) sample three 1x1 textures (ASTC 5x5, DXT1, RGTC1). Mesh draws are implemented (A9) and draw in depth; the leaves and nose still do not show in colour (A9: culling compute). **2026-11-07:** the GPU culling 4c1e8564 feeds the mesh draws now (A17 fixes: CAS for min/max on host memory, 16-step pool-table search, stale .spv replaced, task loop cap). On replays of captures 141109/144949 the chain counts and the mesh draws get work (leafy crowns visible in the replay); in the game at 150-220 s the records still read x = 0 and the trees stay silhouettes: the game and the replay disagree here (open). **Later 2026-11-07 (measured, runs `work\run\gpt_3`, `gpt_4_nocoh`, capture `20261107-193137-base_1-base`): the culling is no longer the cause.** With the re-translated cache (RemoveNode fix) every record's word 0 is non-zero in the game at 168-174 s and on the replay (0x1c/0x21, 1, 8/0xa, 6/0x20, 6/7 groups, counts 0x18-0x21), the 11 indirect dispatches get 33, 1, 7, 10, 0, 2, 38, 0, 0, 32, 1 groups (`snapd_*`; the zeros have count 0) and the colour-pass mesh draws get 1-15 groups (`snapm_*`), with and without `[nvn] gmem_coherence`. The nose is still sky and the background trees still dark-blue silhouettes (`gpt3_t150.png`, same view as base_1's `shot_2_t170`). On the replay, leaving out the 7 colour-pass mesh fragment shaders (bb9b2ea9 fbfa1738 4097c13f 3abc0b54 957527e8 2972adf5 1e03316b, `skip_fs_file`) changes no pixel: those draws reach the colour target with nothing visible. Next: why (A9 leftovers: mesh outputs / OMAP system values, depth against the copied prepass depth t128607, the draws' per-primitive data), or which other draw owns the nose. **2026-11-07, later (game runs in `work\ai_run\oot\leaf8`, snapshots every 3-5 draws, `[nvn] debug_equal_as` = 4 / 8 to look past the EQUAL test): three causes.** (1) A9's mesh slot base (0x750 -> 0x770), see A9. (2) **The nose: LDC.64 with the destination pair over its index register** (`LDC.64 R58, c[0x6][R58+0x28]`, the bone palette read of OoT's skinning vertex shaders, e.g. e132ae05): the Ampere frontend loaded the second word from the already overwritten R58 (`EmitLdcAmpere`, now loads both words first). The face's skinned nose got one garbage translation term and stretched into streaks off screen; EQUAL hid it, so the sky showed through. 314 corpus blocks change (289 vertex, 21 mesh, 4 fragment); the nose is back in the game (`work\run\leaf_glsl\ingame_nose_fixed_t135.png`). (3) **Still open: the crowns.** The prepass draws the leaf cut-outs right (that is the silhouette); the colour pass draws them with vs 5332b131 + fs a6d374cb (no discard: it relies on EQUAL) and its colour comes out black at every pixel (with ALWAYS the whole crown hull shows black), so after fog the crowns stay dark blue. Not the textures' swizzle or a 1x1 stand-in alone (unverified), not the LDG overlap (addresses are precomputed); next: which input of a6d374cb is zero (its vertex shader is a gmem/gpt one: 5332b131's pointer loads). The colour-pass mesh draws' records are non-zero in the game (1-0x5e groups) with or without the mirror; **on the replay** they read 0 with the default mirror/compute shadows (pool right, the copy the indirect read takes stale) and only `pool_mirror = off` + `compute_shadows = false` gives the game's counts. **Crowns fixed 2026-11-07 (runtime, no shader change):** 5332b131 reads its light-probe data through pointers: vs ssbo slot 1 holds five 64-bit pointers (0x787fb0a000 grid header, +0x40 / +0x380 / +0x680 cell tables with eight u16 probe indices each, 0x7881c9a680 the probe SH records, 16 bytes each), and out 14/15/16 are the trilinearly weighted probe SH (0.282 = Y00) that a6d374cb lights the leaves with. All five point into a 149 MiB pool that no draw, copy or texture of the frame binds; pools over one import chunk (64 MiB + 16 overlap) are imported chunk by chunk only where the render thread resolves an address, so none of its chunks was imported, the page table mapped none of its pages and every pointer load read 0: no ambient light, black crowns, dark blue after fog. Seen with the new trace lines `ptr word N: <addr> -> in pool ..., page table UNMAPPED` (trace_fs_draws / trace_frame print them for the 64-bit words of a pointer shader's bound storage buffers; `gpu::page_table_lookup` walks the table on the host). Fix: `gpu::import_pools_for_pointers()` (nvn_gpu_pool.cpp), called from `gmem_before_op` for every op with a `gmem` stage: once per change of the pool registry (`pool_registry_generation`), every chunk of every non-virtual pool is imported, so the page table maps all pool memory (first scene: 56-57 more chunks, 4.1 GiB of host memory imported, 1.2-1.5 s once on the render thread; no video memory). The crowns are green in the game (`work/run/crown_fix/ingame_crowns_t110.png`, `ingame2_t150.png`, `ingame_t200.png`; same view as gpt3_t150) and on replays; fps unchanged (10.6-11.3 during the first minute as in gpt_3 / base_1, 15.9-16.2 later in the second run with warm pipelines). The replay's "records read 0" was a trace artifact (see "Frame capture and replay"): with the snapshots read after the GPU finished, the colour-pass mesh draws get 3-0x157 groups on the replay with the default mirror and compute shadows too (`work/ai_run/oot/t2_default_fixed`).| fixed |
| A16 | intro | **Intro plays slowly** (user: half speed or slower in the forest). The game steps once per presented frame (fixed 60 Hz step) and the first scene runs at 13-20 fps: render thread replay 55-90 ms per frame, plus ~1.8 GB/s each way of compute storage-buffer shadow copies (`[nvn] compute_shadows`, ~300 ms GPU per s, `[prof] gpu_passes`). FPS milestone. | open |
| A17 | all | **Global memory accesses the translator cannot tie to a bound buffer are dropped** (found 2026-11-06 chasing A9): Ryujinx's GlobalToStorage removes every LDG/STG/ATOMG/RED whose address it cannot trace to a c[0] storage descriptor (`results.tsv` messages "Failed to find storage buffer for global memory operation ..."): loads read 0, stores and atomics vanish. **3,485 translated blocks** have such accesses (1,588 vertex, 1,095 fragment, 615 compute, 179 mesh, 8 task). Typical cause: a 64-bit pointer loaded from memory (pointer tables; the foliage task shader reads one at c[0] 0x670). Seen effect: OoT's GPU culling root `4c1e8564` (direct, 169 groups) loses its counter atomics, so the 11 indirect dispatches after it get 0 groups (`snapd_*` snapshots, run `work/run/mesh_14`) and every GPU-culled colour-pass mesh draw gets {0, 1, 1}: the leaves stay invisible. Lichii (checked first) keeps them as global accesses and resolves them at run time by comparing the address with the program's bound storage descriptors (base from c[0], size from the NVN descriptor word +8; nearest base for native pointers), 0 / dropped when nothing matches; pointer tables only for compute; no buffer device address. **Lichii's fallback implemented (2026-11-06)**: `tools/shaders/ampere/frontend/AmpereGlobal.cs` + `spirv-g2s-c0-base.patch` (candidates: the stage's 16 c[0] storage slots, task 0x650 / mesh 0x750, compute: the NVN2 table's storage entries; the harness marks such blocks with the resource token `gmem`, the runtime then builds a per-draw graphics c[0] with every stage's {address, size}, `graphics_c0` in nvn_replay.cpp; off: `--no-global-fallback` / `NXRT_HARNESS_NO_GLOBAL_FALLBACK=1`). Corpus: 0 "Failed to find storage buffer" left, 3,413 blocks use the fallback, no block lost its translation. **Not enough for the culling root**: `4c1e8564` takes its counter addresses from a pointer table in a uniform block (`LDC.64 c[0x4][R0*16+0xe0]`, also +0x80/+0xa0; values like 0x796bc8e000, 0x7971cf0700), and none of them lies in the 5 storage buffers bound to the dispatch: the counts stay 0 (run `work/run/gfb_4`). Lichii has nothing further (no buffer device address). Next (own design): resolve such addresses through Vulkan buffer device addresses of the imported pools (guest VA = host VA; a pool table the shader searches) **2026-11-07:** the search runs as loops (helper 8,351 -> 2,523 words; slow pipelines median 26 s -> 13 s, mesh pipelines still 40-72 s, run da_9). The device-address path works with the default `pool_mirror` / `compute_shadows` (run da_11: 14.8 fps, no device loss, counts written): the A17 test runs had both off, which halves the fps (perf_ab1 8.1 vs perf_ab2 16.4 fps, same cache). The culling root 4c1e8564 now writes its item counts (word 3 of its indirect records, `[nvn] trace_snap_after`), but word 0 (the x group count, `RED.E.MAX [ptr]` at 0x6e70/0x74d0/0x7830) stays 0: in its translation 17 function-local registers are read but never assigned (address high word and value of all three max calls, three of six atomic adds); 0 of 300 sampled other compute shaders have any, `--debug-mode` has the same 17, and the structurizer check passes, so decoding/SSA loses the definitions in that code (it also has a BRX jump table at 0x1380 and BREAK/BSSY nests). Next: the IR block graph of the tail. **Later 2026-11-07:** the 17 unassigned registers were Ryujinx's `Optimizer.RemoveNode`: removing the dead copy of a call's unused result walked on into the call (it stays: side effects) and took it out of its arguments' use lists, so their definitions were deleted (`translator/optimizer-remove-node-side-effects.patch`; 0 unassigned now, no corpus block changes). A second, runtime cause hid it in runs: the shader preloader used a leftover .spv whose results.tsv row was gone and ignored the record the on-demand translator delivered (40 compute shaders' storage buffers unmapped, dispatches left out; fixed in `preload_one`). With both, the counts are written (capture deku150: 0x41, 0x42, 6, 4, 0xc8) but word 0 (x groups, the tail's `RED.E.MAX`) is still 0 in game and replay alike; the tail's shared-memory reload is declared at the right size (384 words). Game addresses of these records change between runs (0x796bc82000 in da_10, 0x796bc8e000 now): snapshot by the pointer table (ubo1 of 4c1e8564), not by fixed addresses. Capture `work/captures/20261107-123358-a10-deku150` (this branch's build, 8-stage format) replays in ~40 s per try and reproduces the counts. **Fixed 2026-11-07** (word 0 non-zero in game and replay, see A15) by the RemoveNode fix once the main cache was re-translated with it; the coherence kill switch (`compute_shadows` / `pool_mirror` off) was not needed to tell. Also done: (1) **guest page table** instead of the 16-step search: `nvn_gpu_pool.cpp` keeps one device address per 4 KiB page (2 MiB leaves where one import holds a whole span; root per 1 GiB, 47-bit addresses; host-visible buffer written in place, 32 MiB, grows by copying), and `AmpereGlobal.EmitPageWalk` walks it generically: level shifts, root size and table address are c[0] 0xfe0..0xfef data, so the layout can change in the runtime without a re-translation. Such blocks carry the token `gpt`; older translations still get the sorted table at 0xff0/0xff8. The main cache's 3,413 gmem rows were re-translated (22 s, all ok; the 23 on-demand rows dropped); SPIR-V size barely changes (763 -> 756 MB: the candidate checks against the stage's bound storage descriptors dominate, not the search), and fps in the Deku Tree view is as before (10.4-11.8 with or without coherence, base_1 11.5-12.7 with a prepared cache). **A preparation is due** (not run: shaders still change); until then the first starts compile the changed pipelines on demand (one cold start never left the title: 100 s mesh pipelines, 27 GB private; pass `nvn.compute_background_spirv_kb: 64` and `memory.limit_gb: 26`). (2) **gmem coherence** (`[nvn] gmem_coherence`, `gmem_before_op` in nvn_replay.cpp): before a pointer shader, held compute results within 64 KiB of each pool pointer in its uniform blocks go back; after one with the `gmemw` token (9 compute blocks write that way) those ranges are `gpu_wrote`. Flushing every held result instead cost 900 MB/s of copy-backs and dropped to 7 fps; the windowed version costs ~14 MB/s plus ~11 MB of pool-copy refills per frame and no measurable fps. `[nvn] trace_snap_after` takes `u<slot>@<offset>` items (pointer from a compute uniform block). | fixed |

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

**Performance notes so far:** the lifted code is built for baseline x86-64, so every FMADD is a CRT `fmaf` call (software FMA): a worker
spent its time in `fmaf` in main+0x370a5a0. `/arch:AVX2`-style flags (FMA3) would need a full rebuild. `[nvn] compute_shadows` copies
every storage buffer of a dispatch to device-local memory and back (40 GB in 10 s during NDVE): pointless on an integrated GPU.

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

MCP tools: `capture_frame(name, instance=...)` (the game writes `work\captures\<time>-<instance>-<name>`, 3-10 s, ~4 GB),
`captures(id)`, `replay_capture(id, instance="replay", seconds=...)`. A request file `capture_frame[.<pid>]` in the request folder or
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
cache, renewed when that one is newer; the instance's first save writes its own file of the shared one's size, which happened at the
title already: ~3,000 shader parts built ahead), screenshots, test output and the outputs of
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
`%APPDATA%\OcarinaRecompPC\save` is never touched. A state or bundle starts the instance it came from again unless `instance=` says
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
or `nxrt_gfx_tests.exe <group>`, failures print `FAILED <file>:<line>`): **all 170 groups pass** (2026-11-06).
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
tools\.venv\Scripts\python.exe tools\extract\nup_extract.py "<game>.nup" game\oot --exefs-only
rem 2. main is ZBIC-compressed: write plain NSOs for the lifter (checks every segment against the NSO header hashes)
tools\extract\build\msvc\bin\nx_plain_nso.exe game\oot\updates\131072\exefs game\oot\plain\exefs
rem 3. lift + compile (one module: append oot_sdk, oot_main, ...; keep going after errors: -k 0)
recomp\scripts\build_game.cmd oot
```

```bat
rem 4. the whole game (about 26 GB, hash-checked) for the runtime
tools\.venv\Scripts\python.exe tools\extract\nup_extract.py "<game>.nup" game\oot
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
- **`tools/setup/` is not in this repository**, so `nx_extract` (NSP/XCI reader, launcher raw-dump path) cannot be built here.
  `tools/extract/CMakeLists.txt` builds only `nx_plain_nso` in that case.
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
