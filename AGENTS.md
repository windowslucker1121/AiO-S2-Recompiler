# Notes for AI agents

What a new agent needs to know before working here. Keep it current: add what you learn, remove what stops being true.
Repository basics are in `README.md`, the recompiler in `recomp/README.md`, the game boundary in `runtime/docs/GAME_BOUNDARY.md`.

## State (branch `oot-nup`, 2026-11-05)

Porting **The Legend of Zelda: Ocarina of Time** (Switch 2, program `0400486024DEC000`, update 2.0.0 / `131072`) next to the
existing Pokopia port. Game id `oot`: `recomp/games/oot/` (manifest `game.toml`, `config.py`, `CMakeLists.txt`, `main.sites`).
The dump is `The Legend of Zelda Ocarina of Time [0400486024DEC000][v131072].nup` in the repo root (25.6 GB, git-ignored).

The user's goal is a port that is **actually playable** on this PC. Milestones in the user's order: (1) boot into the **main menu
without artifacts**; (2) **basic gamepad** (XInput / SDL gamepad drives the game, menus included); (3) last, **FPS**: measure first
(`[gfx] log_fps = 1`, hitch log, `[diag] soak_csv`), reach **30 fps in the main menu**, then in game **30 fps**.
Standing instructions: keep going until blocked, then report; install tools only into git-ignored repo folders from official
releases with checksum checks; never change system or security settings (give the user the command); when the permission check
blocks fetching an external repository, give the user the exact PowerShell commands; one build or one `oot.exe` at a time; never
commit without asking.

Done: NUP extraction, ZBIC -> plain NSO, manifest, lift of all four modules (0 untranslated instructions), and all four module
libraries compile with clang-cl (`work\build\oot\oot_{main,sdk,subsdk0,rtld}.lib`, main 427 MB). First build 25 min cold on 18
cores; the FP16 fix afterwards rebuilt one shard in 113 s (most of it the re-lift, which every lifter change triggers).
Runtime side: `runtime/games/oot` exists as a bring-up module (identity from the manifest, title facts from `main.npdm`: priority 44,
core 0, stack 0x100000, system resource 0x3000000; firmware 22.0.0 as a lower bound since ZBIC needs it; uid "OOTRecmp"; no hooks,
probes or play mode). The runtime copes without play hooks (`play.cpp`: `if (gp && gp->frame_update)`).
Placeholders (no control.nacp in the dump): languages 0x1ffff, display version "2.0.0", save sizes and temporary storage 64 MiB.
Not started: hook sites, shader cache, NUP support in the launcher.

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

Hardware of this machine: Intel Core Ultra 5 125H, integrated Intel Arc graphics (driver Vulkan 1.4.325, 8.8 GiB shared), 16 GB
RAM shared with the GPU. Expect GPU-bound performance; the README's minimum is a Vulkan 1.3 GPU and 16 GB.

## Boot progress (`oot.exe --game game\oot\updates\131072 --log work\run\bootN\oot.log --max-seconds 60`)

Each fix below moved the boot further; all are in the branch. Crash reports land next to the exe (`game-build\build\release\oot_crash.txt`,
`oot_freeze.txt` from the watchdog after 10 s without a presented frame, `oot_fatal.txt`);
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
- **Launcher** accepts NSP/XCI/folder only; `.nup` would go through `nup_extract.py`.
- **Pokopia regression risk** (no Pokopia dump here): shared changes in funcs.py (header/MOD0, ADR starts, RETAA/RETAB, zero FDEs), a64.py
  (PAC decode), tier2 FP16, used-memory accounting, NVN minor 40, TLS +0x110, MapMemory Alias sources, WaitIfEqual64, bsd descriptors.
  Oracles, from `recomp\`: `..\tools\.venv\Scripts\python oracle/difftest/difftest.py all`, `oracle/e2e/run_e2e.py`,
  `oracle/hooks/test_hooks.py`; runtime tests `runtime\scripts\build.ps1 -Test`.

## Artifacts (the user wants every one fixed; add each one seen, mark it fixed with the cause)

| # | Where | What | State |
|---|---|---|---|
| A1 | title, intro | UI text invisible ("press a button" plate, likely the intro subtitles). Layout2 text is rendered on the GPU into sparse (virtual-pool) render targets: glyph pass vs `a6e1624c` fs `2488ab4f` (font atlas R8 256x46 + 4x4 white) into RGBA8 (e.g. 242x40), then R8 passes (fs `1d8ac4b3`, `78ed7ef1`) into the R8 text texture the composite (fs `7c4fccd6`) samples. The glyph pass already writes (almost) nothing: its vs reads glyph data from two storage buffers (c0 words 0x74/0x5c = vertex bindings 12 and 6) and cb7. Next: check those storage buffers in a trace (`ssbo` lines, added) | open |
| A2 | title | black background: the title is a movie. Fixed: AV1 on dav1d (below) and the NDVE model hidden | fixed |
| A3 | title | faint vertical stripes in the logo/smoke area (x 340-940), seen over the black background; not seen over the movie | check again |
| A4 | title -> A | the screen stayed black after A: the game waited for the title movie (no video decoder). Fixed with A2; A now leads into the intro (tapestry movie) | fixed |
| A5 | setup window | blurry (DPI-virtualised) | fixed in code (per-monitor DPI), not yet seen |
| A6 | all | Opus audio is silent: `hwopus` (IHardwareOpusDecoder) "decodes silence" (no Opus decoder in the runtime). vcpkg `opus:x64-windows-static` would give one | open |

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

**Play mode / frame hook:** OoT's frame update is main+0x3b4ea90 (entryhook in `main.sites`; the framework's per-frame virtual, vtable
0x4c63f80 +224, called by the endless loop main+0x9467b0; the base class's +224 0x346c8d0 is never called). `runtime/games/oot/oot_game.cpp`
registers it plus factory key bindings (WASD move, arrows camera, Enter/E/Space A, Shift/Backspace/left mouse B, Q ZL, F R, R/T X/Y,
G ZR, Escape +); pads keep the console layout (the base engine maps SDL gamepads to a Pro Controller). A deleted `controls.ini` in the
game folder is rewritten from the factory text.
**Gamepad verified** (2026-11-06) with the test tooling's virtual SDL pad: script `keydown vpad attach xbox`, `keydown vpad button south 1`
/ `0` (MCP `launch` script lines): A on the pad starts the game from the title, and the intro's rumble reaches the pad (`vpad rumble` log
lines). A real XInput pad goes through the same SDL gamepad path.

**Performance notes so far:** the lifted code is built for baseline x86-64, so every FMADD is a CRT `fmaf` call (software FMA): a worker
spent its time in `fmaf` in main+0x370a5a0. `/arch:AVX2`-style flags (FMA3) would need a full rebuild. `[nvn] compute_shadows` copies
every storage buffer of a dispatch to device-local memory and back (40 GB in 10 s during NDVE): pointless on an integrated GPU.

## Techniques that worked

- Bring-up probes without a full main rebuild: `entryhook <addr>` in `main.sites` (one shard recompiles) plus
  `NXRT_DIAG_TRACE_SITES=<addr,...>` and `NXRT_DIAG_TRACE_PEEK=x1+4,...`: logs registers at each function entry (trace_sites now also
  registers entry hooks). Remove the probe sites again afterwards.
- Name NVN calls the game makes through its proc globals: `peek <GOT slot> 8 0` in an MCP `launch` script (with
  `settings {"diag.watchdog_seconds": -1}`), then look the thunk address up in a debug log (`nvnDeviceGetProcAddress("...") -> 0x74...`).
- Trace a frame range: `NXRT_NVN_TRACE_FRAME=1560-1700` (ranges now work; slow: every traced frame writes its targets as PNG into the
  game folder); `NXRT_NVN_TRACE_TEX_EVENTS=2` logs init/upload/render-target events of every sparse texture.
- Thread dumps of a running game: create the file `game-build\build\release\dump_threads` (the watchdog writes all stacks to the log).

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

## AI tools for the running game (`tools/ai/`)

MCP server `game` (registered in `.mcp.json`, enabled by `"enabledMcpjsonServers": ["game"]` in the local, git-ignored
`.claude/settings.local.json`; loads only in sessions started after that; tools `mcp__game__*`), also a CLI:
`tools\.venv\Scripts\python tools\ai\mcp_server.py help`. Tools: `status`, `launch` / `restart` / `kill`, `screenshot` (PrintWindow,
works when covered), `send_input` (posted messages in the background, or SendInput with the game window in front), `wait_for`,
`compare` (SSIM against a reference), `log`, `reports` (crash / freeze / fatal). Details and limits: `tools/ai/README.md`; check:
`tools\.venv\Scripts\python tools\ai\tests\test_gamectl.py`. Names come from `game.toml` (`NXRT_AI_GAME`, default `oot`).
Verified only against a stand-in window: whether PrintWindow sees the Vulkan swapchain and whether the SDL window takes posted
input while unfocused is known only once `oot.exe` runs. `launch` sets `test.frame_source=tick` for scripts (OoT has no frame hook).

## Machine setup (this checkout, Windows 11)

| Tool | Where | Notes |
|---|---|---|
| Visual Studio 2022 (MSVC 14.44, CMake, Ninja) | found with `vswhere` | `recomp\scripts\nx_env.cmd` sets up the environment |
| LLVM / clang-cl 23.1.5 | `tools\llvm\` (git-ignored) | official release `clang+llvm-23.1.5-x86_64-pc-windows-msvc.tar.zst` (SHA-256 checked; unpacking needs zstd window 2^31: `compression.zstd.ZstdFile(..., options={DecompressionParameter.window_log_max: 31})`); `recomp/cmake/clang-cl.cmake` prefers it over `C:\Program Files\LLVM` |
| .NET SDKs 9.0.301, 9.0.319, 10.0.403 | system | shader harness builds for net10.0 |
| not installed on purpose | | pwsh (Windows PowerShell 5.1 runs the .ps1 files), Vulkan SDK (spirv-val), libvpx, `tools/setup` |
| sccache 0.19.0 | `tools\sccache\` (git-ignored) | picked up by the toolchain file at configure time; cache in `work\build\sccache` |
| Python 3.13 venv | `tools\.venv\` (git-ignored) | `recomp/requirements.txt` and `tools/ai/requirements.txt` (Pillow, numpy) installed. Python 3.14 is also installed system-wide (has `compression.zstd`) |
| `nx_plain_nso.exe` | `tools\extract\build\msvc\bin\` | built with MSVC from a VS x64 prompt: `cmake -S tools\extract -B tools\extract\build\msvc -G Ninja -DCMAKE_BUILD_TYPE=Release` then `cmake --build tools\extract\build\msvc` |

Disk: C: had 59 GB free with the whole game extracted (`game\oot`, 26.3 GB RomFS) and `work\` (~1.1 GB lift, builds, shader corpus).

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

- Do not run two builds in `work\build\oot` at the same time, nor two copies of `oot.exe`. Background jobs die with the session.
- Changing Windows Defender exclusions (for `work\`, `tools\llvm\`) would likely speed builds, but it is a security setting: only the
  user changes it.
- Keep `recomp/nxrecomp` changes game independent; game knowledge goes in `recomp/games/<id>/` and `runtime/games/<id>/`.
