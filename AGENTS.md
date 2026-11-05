# Notes for AI agents

What a new agent needs to know before working here. Keep it current: add what you learn, remove what stops being true.
Repository basics are in `README.md`, the recompiler in `recomp/README.md`, the game boundary in `runtime/docs/GAME_BOUNDARY.md`.

## State (branch `oot-nup`, 2026-11-05)

Porting **The Legend of Zelda: Ocarina of Time** (Switch 2, program `0400486024DEC000`, update 2.0.0 / `131072`) next to the
existing Pokopia port. Game id `oot`: `recomp/games/oot/` (manifest `game.toml`, `config.py`, `CMakeLists.txt`, `main.sites`).

Done: NUP extraction, ZBIC -> plain NSO, manifest, lift of all four modules (0 untranslated instructions), and all four module
libraries compile with clang-cl (`work\build\oot\oot_{main,sdk,subsdk0,rtld}.lib`, main 427 MB). First build 25 min cold on 18
cores; the FP16 fix afterwards rebuilt one shard in 113 s (most of it the re-lift, which every lifter change triggers).
Runtime side: `runtime/games/oot` exists as a bring-up module (identity from the manifest, title facts from `main.npdm`, firmware
22.0.0 as a lower bound since ZBIC needs it; no hooks, probes or play mode). The runtime copes without play hooks
(`play.cpp`: `if (gp && gp->frame_update)`).
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
generic scan (OoT uses zstd). Next: translate it and point oot.exe at the cache (not run yet):
`dotnet tools\shaders\build\bin\Release\net10.0\spirv_harness.dll --batch work\shaders\oot\corpus --out work\shaders\oot\spirv --jobs 12 --no-val`,
then run with `NXRT_NVN_SHADER_CACHE=<repo>\work\shaders\oot\spirv`, `NXRT_NVN_SHADER_CORPUS=<repo>\work\shaders\oot\corpus` and, for
blocks the batch missed, `NXRT_NVN_SHADER_TRANSLATOR=<repo>\tools\shaders\build\bin\Release\net10.0\spirv_harness.exe`.

Requirement from the user: a **shader compilation screen before the first boot**, so the game runs without shader-compile stutter. The
pieces exist (`oot.exe --prepare-shaders`, the launcher's setup step that builds the cache, `tools/shaders`); they need OoT's shader
extraction (`[shaders] extractor = "generic"` in game.toml) and the translator (`tools/shaders/harness`, .NET 8) working first.

Hardware of this machine: Intel Core Ultra 5 125H, integrated Intel Arc graphics (Vulkan 1.3), 16 GB RAM shared with the GPU.
Expect GPU-bound performance; the README's minimum is a Vulkan 1.3 GPU and 16 GB.

## Boot progress (`oot.exe --game game\oot\updates\131072 --log work\run\bootN\oot.log --max-seconds 60`)

Each fix below moved the boot further; all are in the branch. Crash reports land next to the exe (`game-build\build\release\oot_crash.txt`);
`tools\.venv\Scripts\python tools\ai\symbolize.py <crash.txt | addr...>` names guest frames from the modules' exported symbols (main exports
little: trust `main+off`, not the name next to it).

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

**Where it stops now (boot 20, about 8 s in):** `nn::nifm::detail::IGeneralService cmd 101 not implemented (in 0 bytes)`, new in SDK 22;
find its SDK caller (`nn::nifm` symbols in sdk), answer it like the other offline nifm commands in `network.cpp`. Right after, the guest
faulted and the runtime's crash handler faulted too ("second exception inside the crash handler"), so no crash report was written:
worth making the handler robust (several threads crashing at once?).

Not done yet: shader translation (draws are skipped: "no translated shader"), no frame is presented, the game names no frame-update hook.

## AI tools for the running game (`tools/ai/`)

MCP server `game` (registered in `.mcp.json`; the user approves it once in `/mcp`, then the tools are `mcp__game__*`), also a CLI:
`tools\.venv\Scripts\python tools\ai\mcp_server.py help`. Tools: `status`, `launch` / `restart` / `kill`, `screenshot` (PrintWindow,
works when covered), `send_input` (posted messages in the background, or SendInput with the game window in front), `wait_for`,
`compare` (SSIM against a reference), `log`, `reports` (crash / freeze / fatal). Details and limits: `tools/ai/README.md`; check:
`tools\.venv\Scripts\python tools\ai\tests\test_gamectl.py`. Names come from `game.toml` (`NXRT_AI_GAME`, default `oot`).
Verified only against a stand-in window: whether PrintWindow sees the Vulkan swapchain and whether the SDL window takes posted
input while unfocused is known only once `oot.exe` runs.

## Machine setup (this checkout, Windows 11)

| Tool | Where | Notes |
|---|---|---|
| Visual Studio 2022 (MSVC 14.44, CMake, Ninja) | found with `vswhere` | `recomp\scripts\nx_env.cmd` sets up the environment |
| LLVM / clang-cl 23.1.5 | `tools\llvm\` (git-ignored) | official release archive; `recomp/cmake/clang-cl.cmake` prefers it over `C:\Program Files\LLVM` |
| sccache 0.19.0 | `tools\sccache\` (git-ignored) | picked up by the toolchain file at configure time; cache in `work\build\sccache` |
| Python 3.13 venv | `tools\.venv\` (git-ignored) | `recomp/requirements.txt` and `tools/ai/requirements.txt` (Pillow, numpy) installed. Python 3.14 is also installed system-wide (has `compression.zstd`) |
| `nx_plain_nso.exe` | `tools\extract\build\msvc\bin\` | built with MSVC from a VS x64 prompt: `cmake -S tools\extract -B tools\extract\build\msvc -G Ninja -DCMAKE_BUILD_TYPE=Release` then `cmake --build tools\extract\build\msvc` |

Disk is tight: C: had about 38 GB free after the lift. The full RomFS is about 26 GB; extract it only when needed.

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
the next build that is full anyway (header change, new game version) switch it on. The first OoT build ran without sccache.

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

- Do not run two builds in `work\build\oot` at the same time.
- Changing Windows Defender exclusions (for `work\`, `tools\llvm\`) would likely speed builds, but it is a security setting: only the
  user changes it.
- Keep `recomp/nxrecomp` changes game independent; game knowledge goes in `recomp/games/<id>/` and `runtime/games/<id>/`.
