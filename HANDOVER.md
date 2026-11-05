# Handover: Ocarina of Time port, session of 2026-11-05

For the next agent. Read this and `AGENTS.md` before doing anything. When you have taken it in, fold whatever `AGENTS.md` does not
already say into `AGENTS.md` (it is the lasting record) and **delete this file**. All of this session's work is in one commit on
branch `oot-nup` (base `main` at 5339bf8), including the `refs/ryubing` submodule; nothing is pushed.

## 1. Goal and what the user wants

- Port **The Legend of Zelda: Ocarina of Time** (Nintendo Switch 2 remake, program `0400486024DEC000`, update 2.0.0 = `131072`) to this
  static recompiler, next to the existing Pokopia port, and make it **actually playable** on this PC.
- The user's dump is a NUP package in the repo root: `The Legend of Zelda Ocarina of Time [0400486024DEC000][v131072].nup` (25.6 GB;
  git-ignored through `*.nup` in `.gitignore` and `.git/info/exclude`). A first copy was truncated (956 MB); the user replaced it.
- Milestones in the user's order: (1) **boot into the main menu without artifacts** (comes first); (2) **basic gamepad integration**;
  (3) last: **FPS benchmarks and optimizations**, target **30 fps in the main menu**, and once that is met, in game, target **30 fps**.
- Instructions the user gave during the session:
  - Keep going until you cannot continue without the MCP game tools, then report.
  - **A shader compilation screen before the first boot**, so the game runs without shader-compile stutter.
  - Note everything relevant for new AI agents in `AGENTS.md` (`CLAUDE.md` only imports it); keep it current.
  - Tools may be installed **into the repo** (git-ignored folders), from official releases with checksum checks; anything that speeds up
    iteration is welcome. Never change system or security settings (e.g. Defender exclusions): tell the user the command instead.
  - The user asked for a subagent that built AI tools for the running game (`tools/ai/`, done, see section 6).
- Claude Code's permission check blocks the agent from cloning or adding external repositories ("code from external" / "untrusted code
  integration"): when something external is needed, give the user the exact PowerShell commands (that is how `refs/ryubing` got added).

## 2. Machine and setup

| Item | State |
|---|---|
| OS / hardware | Windows 11 Pro; Intel Core Ultra 5 125H; **integrated Intel Arc** (Vulkan 1.4.325 driver, 8.8 GiB shared); 16 GB RAM. Expect GPU-bound performance. |
| Visual Studio | 2022 Community and Build Tools (MSVC 14.44), found with vswhere. No VS 2026, which upstream scripts assumed. |
| LLVM | 23.1.5 in `tools\llvm\` (official GitHub release `clang+llvm-23.1.5-x86_64-pc-windows-msvc.tar.zst`, SHA-256 checked; unpacking needs a zstd window of 2^31: `compression.zstd.ZstdFile(..., options={DecompressionParameter.window_log_max: 31})`). |
| sccache | 0.19.0 in `tools\sccache\` (official zip, SHA-256 checked). The game build uses it (cache `work\build\sccache`, port 24339 from build_game.cmd); `NX_NO_SCCACHE=1` turns it off. |
| Python | venv `tools\.venv` (3.13): recomp/requirements.txt + Pillow, numpy (tools/ai), zstandard (shader extractor). System Python 3.14 has `compression.zstd`. |
| .NET | SDKs 9.0.301, 9.0.319, 10.0.403. |
| Missing on purpose | pwsh (Windows PowerShell 5.1 runs the repo's .ps1 files), Vulkan SDK (spirv-val; harness has `--no-val`), libvpx (vcpkg `libvpx:x64-windows-static`: without it no movie playback), tools/setup (see findings). |
| Disk | C: 59 GB free now. `game\oot` holds the extracted game (26.3 GB RomFS), `work\` the lift output (~1.1 GB), builds and the shader corpus. |
| MCP | `.mcp.json` registers server `game` (tools/ai); `.claude/settings.local.json` (local, git-ignored) has `"enabledMcpjsonServers": ["game"]`. It loads only in a session started after that, so this session never had `mcp__game__*`. |

## 3. Pipeline (all verified this session)

```bat
rem 1. NUP -> extracted layout (exefs ~40 MB with --exefs-only; whole game 26 GB, 8 min, every file SHA-256 checked)
tools\.venv\Scripts\python.exe tools\extract\nup_extract.py "<game>.nup" game\oot
rem 2. ZBIC main -> plain NSOs for the lifter (nx_plain_nso was built with MSVC: see AGENTS.md)
tools\extract\build\msvc\bin\nx_plain_nso.exe game\oot\updates\131072\exefs game\oot\plain\exefs
rem 3. lift + compile the four modules -> work\build\oot\oot_{main,sdk,subsdk0,rtld}.lib
recomp\scripts\build_game.cmd oot
rem 4. texture transcoder (once; sources fetched by tools\textures\scripts\fetch_third_party.ps1, already done)
recomp\scripts\nx_env.cmd cmake -S tools\textures -B tools\textures\build\release -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl
recomp\scripts\nx_env.cmd cmake --build tools\textures\build\release
rem 5. runtime + oot.exe (first time 211 s; incremental ~20 s). Full configure:
powershell -ExecutionPolicy Bypass -File game-build\scripts\build.ps1 -Manifest %CD%\recomp\games\oot\game.toml
rem    incremental: runtime\scripts\env.cmd && cmake --build game-build\build\release --target game_exe
rem 6. run (window opens on the user's desktop; --max-seconds ends it)
game-build\build\release\oot.exe --game %CD%\game\oot\updates\131072 --log %CD%\work\run\bootN\oot.log --max-seconds 60
```

- Crash reports: `game-build\build\release\oot_crash.txt` / `_freeze.txt` (watchdog, 10 s without a presented frame) / `_fatal.txt`.
- `tools\.venv\Scripts\python tools\ai\symbolize.py <crash.txt | addr...>` names guest frames from the modules' exported symbols. main
  exports little: trust `main+off`, not the name beside it. sdk exports ~28,700 symbols, so sdk frames are named well.
- Lifted C++: `work\lift\oot\<module>\c_NNN.cpp`, one `void f_<addr>(Ctx*)` per guest function. Grep `void f_<addr>(` to read one.
- Logs of all boot attempts: `work\run\boot1` ... `boot20`; build logs `work\build_*.log`.
- Rebuild costs: runtime/game exe change ~20 s; one lifted function ~1 min; a change of a module's function list (discovery, midhook
  sites) recompiles that module (main 352 shards); with sccache the last such rebuild took 5 min (20 shards changed). Cold full: 25 min.
  Any lifter source change re-lifts all modules first (~3 min; files rewritten only when their content changes).
- `build_game.cmd` exit codes were unreliable (see findings, fixed); judge builds by `FAILED:` lines in the log.
- Never run two builds in `work\build\oot` or two copies of oot.exe at once. Background jobs of a session die with the session.

## 4. Everything done, in order

1. Cloned the repo into the folder that held the dump; branch `oot-nup`.
2. **NUP support:** `tools/extract/nup_extract.py` (+ `tests/test_nup_extract.py`, passes on 3.13 and 3.14): superblock/TOC
   validation, TOC SHA-256, per-file SHA-256, raw and zstd codecs, name sanitising (no path traversal), update overlaid on base, writes
   the same layout and `extract.json` as nx_extract. Format spec: https://github.com/a55uka/nup/blob/meow/SPEC.md (decrypted dumps, no keys).
3. **ZBIC:** `tools/extract/cli/plain_nso.cpp` = `nx_plain_nso.exe` (built standalone from runtime/src/loader/zbic.cpp, BCrypt SHA-256
   checks every segment against the NSO header hashes). `tools/extract/CMakeLists.txt` builds only this target when tools/setup is absent.
4. **Game manifest:** `recomp/games/oot/{game.toml, config.py, CMakeLists.txt, main.sites}` (main.sites empty). The CMakeLists fixes two
   pkp bugs (root path one level too high for this flattened repo; a link_check.cpp that does not exist).
5. **Lift:** all four modules lift with 0 untranslated instructions (now main 249,577 functions / 15.4M instructions, sdk 45,923,
   subsdk0 1,547, rtld 49).
6. **Toolchain plumbing:** `recomp/scripts/nx_env.cmd` (vswhere, repo-local LLVM and sccache, VS Installer on PATH to silence vcvars),
   `recomp/cmake/clang-cl.cmake` (prefers tools\llvm, sccache hint), `runtime/scripts/env.cmd` (calls nx_env.cmd),
   `game-build/scripts/build.ps1` (`..\nx\runtime` -> `..\runtime`), `recomp/.gitattributes` (`*.cmd`/`*.bat` eol=crlf), `.gitignore`
   (`*.nup`, `/tools/llvm/`, `/tools/sccache/`).
7. **Runtime game module:** `runtime/games/oot/{oot_game.h, oot_data.cpp, oot_core.cpp, oot_game.cpp, CMakeLists.txt, launcher.rc,
   launcher.manifest}`. Title facts from main.npdm (priority 44, core 0, stack 0x100000, system resource 0x3000000), firmware "22.0.0"
   (a lower bound: ZBIC needs it), save sizes and temporary storage 64 MiB placeholders (no control.nacp), uid "OOTRecmp". No play hooks,
   probes or poll sleeps.
8. **Texture transcoder** built (tools/textures/build/release: astc2bc7.lib, bc7e_ispc.lib, astcdec DLLs).
9. **oot.exe** builds and runs; 20 boot attempts, each fixing the blocker below (section 5).
10. **Shaders:** user added submodule `refs/ryubing` (Ryubing Ryujinx, `https://git.ryujinx.app/ryubing/ryujinx.git`, pinned
    `a82350bb774f70fcbd41c9987bf67a3775409963`, the revision tools/shaders names; host redirects to git.ryujinx.app/projects/Ryubing).
    `setup_ryujinx.ps1` staged the 3 projects; `spirv_harness.exe` built (tools/shaders/build/bin/Release/net10.0, 5 min, 0 errors, with
    the repo's Ampere front end). Corpus extracted to `work\shaders\oot\corpus` (25 min): 47,203 unique blocks (17,417 vs, 25,228 ps,
    3,743 cs, 815 stages 6/7) from 71,502 occurrences, nearly all in `Shader/Model.Product.1x0.product.Nin_Ounce_NVN2.bfsha.zs`. The
    340k "oodle: no oozx DLL" notes are false positives (OoT uses zstd). **Batch translation has not been run yet.**
11. AI tools (subagent), symbolizer, AGENTS.md / CLAUDE.md, memory notes (keep AGENTS.md current; repo-local tools are fine).

## 5. Boot history: every blocker and its fix

| # | Symptom | Cause | Fix (file) |
|---|---|---|---|
| 1 | svcBreak in pthread_mutex_lock during subsdk0 `_init` | SDK 22 `SdkMutexType::IsLockedByCurrentThread` compares the mutex word with the u32 at **TLS +0x110** (kernel-written thread handle); 0 made every free mutex "owned by me" | write the thread handle there (`runtime/src/kernel/thread.cpp`) |
| 2 | abort in nnMain after reading `/System/HeapSize/Product.Nin_Ounce_NVN2.HeapDef.byml.zs` | HeapDef `Program` budget 0x22400 KiB = 137 MiB must exceed `nn::os::QueryMemoryInfo` used size; runtime added an invented 64 MiB | used = heap + modules + main thread stack (`svc_info.cpp`) |
| 3 | null deref in the main loop | game requires NVN API 231.**40** (`nvnDeviceGetInteger` major == 231, minor >= 40), runtime said 231.30; the graphics object was never created | minor 40 (`gfx/nvn/api_device.cpp`) |
| 4 | null deref in `VammManager::AllocateAddressRegion` | VAMM enabled only when GetInfo 16 (SystemResourceSize) != 0 | `system_resource_size = 0x3000000` (`oot_core.cpp`) |
| 5 | `MapMemory (range)` 0xD401 for a thread stack | VAMM memory = MapPhysicalMemory pages (state Alias here, Normal on HW), one vm block per 64 KiB granule | accept Alias sources, walk every block (`svc_memory.cpp`; also a diagnostic log line on rejection) |
| 6 | abort in `nn::hid::InitializeMouse` | IHidServer **3150** (SDK 22: u32 @0, aruid @8, PID) | accepted (`services/hid.cpp`) |
| 7 | GUEST TRAP undefined instruction in sdk `setjmp` | **lifter bug**: tier 1 decoded `PACIB x30, sp` as REV16 of xzr (ignored opcode2 in data-processing 1-source; PACIA->RBIT, AUTIA->CLZ...). 10 sites, all in sdk | tier 1 raises Unimpl when opcode2/S != 0, tier 2 x_pac.py lifts PAC as identity (`recomp/nxrecomp/lift/a64.py`) |
| 8 | abort: IAudioDevice 20 / 19 | `Is/SetDeviceOutputVolumeAutoTuneModeEnabled` (SDK 22) | answered, flag kept (`services/audio.cpp`) |
| 9 | FS TargetNotFound in `MountTemporaryStorage` | temporary storage is made by the system at launch from NACP | created on first open; `temporary_storage_size` in TitleProfile + `[title]` key (`services/fs.cpp`, `common.cpp`, `nxrt/services.h`) |
| 10 | stub service `auddmg:u` | new Switch 2 service **nn::audio::devmgr** (audio device manager; ~25 calls: virtual devices, capture/injection streams, mix topology). Command 0 returns a session object (moved handle) | `auddmg:u` with cmd 0 OpenSession; session cmd 0 ListVirtualDeviceNames answers 0 devices (`services/audio.cpp`) |
| 11 | indirect call through null | `nn::vi::ClearLayerExtendedSwapInterval` etc. ran the SDK's code on the host stand-in layer | host answers for SetDisplay120HzRefreshRateCapability, Set/ClearLayerExtendedSwapInterval, SetLayerNextHdrContentGamutMetadata, SetLayerNextHdrToSdrToneMappingMetadata (`core/switch2.cpp`) |
| 12 | abort in `std::atomic_wait` | `svcWaitForAddress` type **3 = WaitIfEqual64** (64-bit value in x2, 8-byte aligned) | implemented; value passed as int64 (`kernel_sync.cpp`, `svc_thread.cpp`, `nxrt/kernel.h`) |
| 13 | GUEST TRAP indirect call to unknown function 0x71024a2960 | **lifter discovery bug**: SDK 22 puts MOD0 **after .text** in every module; "NSO header = up to MOD0+0x1c" covered all of .text, so every start known only from BL/relocation/tail call was dropped (and gap scanning was off) | header = two words at 0 + the MOD0 record wherever it is; gap scanner also ends at RETAA/RETAB (`recomp/nxrecomp/funcs.py`) |
| 14 | same trap at 0x71024a5560 | static callback taken with one **ADR** (printf-style formatter's output function) swallowed by the previous function | ADR targets become starts when outside exact functions and right after RET/RETAA/RETAB/B (not BR: jump-table bases); 57 in main (`funcs.py`) |
| 15 | abort in gRPC event-engine init ("no event engine") | gRPC (Nintendo's NPLN stack in main) probes IPv6 with a UDP `Socket`, then makes its poll engine wakeup with bsd **EventFd (31)**; every bsd command failed | bsd descriptor table: sockets that exist and do nothing, Linux eventfds (Read/Write/Close/Fcntl), Poll that waits for a Write or the timeout (`services/network.cpp`) |
| **now** | boot 20, ~8 s: `nn::nifm::detail::IGeneralService cmd 101 not implemented (in 0 bytes)`, then a guest fault and "**second exception inside the crash handler, terminating**" (no crash report) | nifm 101 is new in SDK 22 | **open** (section 7) |

Earlier compile-time issues (also fixed): zero-length FDEs in main (0x3470b00/b70/be0) crashed the lifter (`funcs.py` ignores FDEs with
end <= start); FP16 vector code (`FNEG v.8h` used as a sign flip, sdk/c_007) did not compile (`lift/tier2/fp.py` f16_sign does exact bit
ops for FNEG/FABS on FP16 lanes; `neon.py` v_un/v_bin raise Unhandled for other FP16 arithmetic; scalar FP16 is not handled).

What the game does by boot 20: rtld -> sdk init -> nnMain; RomFS mounted (186,808 files); account save created; heap 12 GiB section;
NV/vi graphics on the host (display, layer, 60 Hz vsync); NVN back end up on the Arc GPU; ~67+ shader programs set (all unlinked: no
translated shaders); background texture prep threads; loader threads read event settings, sound banks (`/Sound/Resource/*.bars.zs`),
actor and effect packs; hid, account, nifm, prepo, ssl, nsd, bsd used; audio renderer running. **No frame has been presented yet.**

## 6. AI tools (`tools/ai/`, built by a subagent; its test passes: `tools\.venv\Scripts\python tools\ai\tests\test_gamectl.py`)

MCP server `game` (stdio, stdlib only) + CLI (`tools\.venv\Scripts\python tools\ai\mcp_server.py help`): `status`, `launch` / `restart`
/ `kill` (settings as `NXRT_<SECTION>_<KEY>`, scripts with `peek`/`poke` shorthands, `wait`), `screenshot` (PrintWindow, works covered),
`send_input` (posted messages in the background or SendInput with the game window in front; refuses other windows), `wait_for`,
`compare` (SSIM), `log`, `reports`. Names from game.toml (`NXRT_AI_GAME`, default oot; exe default `game-build/build/release/oot.exe`).
Facts: **the exe has no live control channel** (no pipe/socket/stdin; test tooling is configured at start via `NXRT_TEST_*` and answers
through `result.json`, `trace.jsonl`, `shots/`). Untested until the game shows a window: PrintWindow on the Vulkan swapchain (fallback
`method="screen"` or `@shot`), posted input to the unfocused SDL window, raw mouse. `launch` sets `test.frame_source=tick` for scripts
(OoT has no frame hook). `tools/ai/symbolize.py` was added by the main agent (not by the subagent).

## 7. Open points (roughly in order)

1. **nifm `IGeneralService` cmd 101** (no input): find the SDK caller (sdk exports `nn::nifm::*`; the IPC thunk loads 0x65 as the command
   id), learn its output, answer it like the other offline nifm commands in `network.cpp`.
2. **Crash handler robustness:** "second exception inside the crash handler, terminating" lost the report of boot 20 (several threads
   crashing at once, or the handler touching bad state). Make it write at least the first report.
3. **Shaders:** batch-translate the corpus
   `dotnet tools\shaders\build\bin\Release\net10.0\spirv_harness.dll --batch work\shaders\oot\corpus --out work\shaders\oot\spirv --jobs 12 --no-val`,
   report ok/ok-retry/failed, then run with `NXRT_NVN_SHADER_CACHE=<repo>\work\shaders\oot\spirv`, `NXRT_NVN_SHADER_CORPUS=<repo>\work\shaders\oot\corpus`
   and `NXRT_NVN_SHADER_TRANSLATOR=<repo>\tools\shaders\build\bin\Release\net10.0\spirv_harness.exe` (on-demand for misses). Optional:
   spirv-val from a repo-local SPIRV-Tools/Vulkan SDK (ask the user to fetch it if the permission check blocks you).
4. **Shader compilation screen before first boot (user requirement):** wire OoT into the existing path (`oot.exe --prepare-shaders`
   in game-build/src/game_main.cpp; the launcher's setup step; game.toml `[shaders] extractor = "generic"`), show progress, and make the
   game start only with a complete cache. Also pipeline prewarm (`[nvn] prewarm`, `pipeline_keys.bin`, `pipeline_cache.bin`).
5. **First presented frame:** after shaders, find why no frame is presented (watchdog says frame count 0). Then find OoT's per-frame
   function for play mode (Pokopia hooks its frame update via `entryhook` in main.sites and `GamePlay::frame_update`); without it play
   mode is not pumped (window events / input sampling).
6. **NVN gaps:** unknown procs handed logging stubs (seen: nvnDeviceGetFlags, nvnQueueGetTotalRayTracingMemoryUsed,
   nvnQueueBuilder{Set,Get}RayTracingMemorySize, nvnQueueBuilderGetPrimaryQueue, nvnWindow(Builder){Set,Get}AcquireTimeout,
   nvnProgramBuilder*RayTracing*, nvnTexturePool/SamplerPoolWriteDescriptorTableData, nvnMultisampleState*CoverageTruncation*,
   *GetSampleLocations, nvnCommandBufferSetComputeWorkloadPriority; ~140 lines per boot); "GPU-side WaitEvent not implemented".
   Implement what OoT actually depends on (NVN 231.31-231.40 additions).
7. **auddmg session:** only ListVirtualDeviceNames (0 devices). Other devmgr calls will show up as unimplemented. Microphone capture
   (the remake's voice/ocarina features use the mic) later.
8. **nn::vi queries** (QueryDisplayHdrStatus/VrrSupported/HdrPaperWhite/CurrentRefreshRate) return the SDK's error 0x272 for the
   stand-in display; give host answers once their output layouts are known.
9. **Placeholders without control.nacp:** languages 0x1ffff, display version "2.0.0", save sizes, temporary storage 64 MiB, firmware.
10. **Movies:** install libvpx via vcpkg (`libvpx:x64-windows-static`) into a repo-local vcpkg if allowed, set `NXRT_LIBVPX_ROOT`.
11. **Regression risk for Pokopia** (no Pokopia dump here to test): shared changes in funcs.py (header/MOD0, ADR starts, RETAA/RETAB, zero
    FDEs), a64.py (PAC decode), tier2 FP16, used-memory accounting (-64 MiB), NVN minor 40, TLS +0x110 write, MapMemory Alias sources,
    WaitIfEqual64, bsd descriptors (Close/Poll/Read/Write semantics changed). Run the oracles: from `recomp\`:
    `..\tools\.venv\Scripts\python oracle/difftest/difftest.py all`, `oracle/e2e/run_e2e.py`, `oracle/hooks/test_hooks.py`; and the runtime
    tests (`runtime\scripts\build.ps1 -Test`, with Windows PowerShell; may need the same VS/LLVM path fixes).
12. **Launcher:** accepts NSP/XCI/folder only; add `.nup` through nup_extract. `nx_extract` (NSP/XCI) cannot be built here: `tools/setup/`
    is not in this repository.
13. **Main menu without artifacts** (the first milestone): title screen and main menu render correctly (shaders, textures, render
    targets); compare against the real game's look with the MCP `screenshot` / `compare` tools.
14. **Basic gamepad integration** (after the main menu is clean): a standard controller (XInput / SDL gamepad) drives the game through
    hid (Pro Controller / Joy-Con 2 style input), including menus; runtime play mode needs OoT's frame-update hook for input sampling.
15. **FPS benchmarks and optimizations** (the last step): measure (`[gfx] log_fps = 1`, the hitch log, `[diag] soak_csv`), first reach
    **30 fps in the main menu**, then go in game and reach **30 fps** there, on this Intel Arc iGPU (render scale, FSR, frame pacing,
    pool mirror caps under 4 GiB VRAM, CPU-side costs of the lifted code).
16. Suggested to the user (their action, not ours): Defender exclusions for `work\` and `tools\llvm\` (`Add-MpPreference -ExclusionPath ...`).

## 8. Techniques that worked (for the next blockers)

- Name an import of main: lifted stub `void f_<plt>(Ctx*) { host_import(c, NX_IMPORT_ID(0, N)); }` -> N is the index into main's
  `img.symbols()` (nxrecomp.program.open_image), demangle with `tools\llvm\...\bin\llvm-cxxfilt.exe` (use an absolute path).
- Find who writes a global: globals are reached through the GOT; look for relocations whose addend is the global
  (`img.relocs()`), then for code loading that GOT slot (`adrp page` + `ldr [page + off]`) and storing through it.
- IPC request layout: SDK sf clients build CMIF with magic `SFCI` (0x49434653); command id, then fields at +16.. in the raw data; a
  set top bit in the header means "sends PID". Replies with a moved handle are session objects (runtime: `rsp.push_object`).
- svcBreak with Result 0 = an `NN_ABORT_UNLESS`-style check: read the caller just before `AbortImpl` in the lifted code.
- The lifter's discovery can be inspected with `nxrecomp.funcs.analyse(img)` (rows with src E/S/B/R/T/A/G and end_from).
- When the game calls through an address that is "not a known function", check discovery for that address first (section 5 #13/#14).
