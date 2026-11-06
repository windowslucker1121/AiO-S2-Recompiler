# Ocarina of Time (Switch 2) port: status and hand-off

State of branch `oot-nup` on 2026-11-06 (last commit `bd289e8`). Read this first, then `AGENTS.md` (running notes, boot fixes,
techniques, rules) and `DEVELOPMENT_SETUP.md` (what a development machine needs).

## Where it stands

| Area | State |
|---|---|
| Boot, title screen | Works. Title movie (AV1 on dav1d), logo, menu text ("New Game", "Options / ZELDA NOTES Setting") without artifacts. |
| Gamepad | Works (SDL gamepad path, checked with the test tooling's virtual Xbox pad: A starts the game, rumble reaches the pad). Keyboard bindings exist too. |
| Audio | Opus decodes on libopus (not listened to). Two audio-renderer crashes on entering gameplay are fixed. |
| Shaders | Second machine: 46,192 of the corpus's 47,203 blocks translated in a fresh batch run (`work\shaders\oot\spirv`; the first machine's cache also had on-demand and re-translated rows: 46,357 of 47,428). Pre-boot shader preparation done (39,327 of 39,363 programs, 36 without a shader) into `work\shaders\oot\pipeline_cache.bin` (2.2 GB on the RTX 4080). |
| New Game | Title -> 5-minute intro movie (`Dm_Kokiri_0000.webm`, cannot be skipped) -> ~70 s loading -> first 3D scene. |
| **First 3D scene** | **Blocked: `VK_ERROR_DEVICE_LOST` 1-7 s after its first frame, on every full run (A8, below).** |
| FPS | Title well above 30 fps. In game unknown (blocked); with 1/16 of the fragment draws enabled the scene ran at 10-15 fps. |

## The current problem: GPU device lost in the first scene (A8)

Symptom: after "New Game" and the intro, the first scene presents one to three frames (the first takes 9-112 s to replay because of
first-use texture uploads), then `vkQueueSubmit2 failed (-4): the device is lost or out of memory` in `oot.log` and the process ends.

Reproduce (about 3.5 minutes):

```bat
set NXRT_NVN_SHADER_CACHE=%CD%\work\shaders\oot\spirv
set NXRT_NVN_SHADER_CORPUS=%CD%\work\shaders\oot\corpus
set NXRT_NVN_SHADER_TRANSLATOR=%CD%\tools\shaders\build\bin\Release\net10.0\spirv_harness.exe
set NXRT_MEDIA_VIDEO_MAX_PICTURES=120
set NXRT_DIAG_WATCHDOG_SECONDS=-1
game-build\build\release\oot.exe --game %CD%\game\oot\updates\131072 --log %CD%\work\run\a8\oot.log --max-seconds 330
```

Press Enter (= A) at the title menu after about 50 s (earlier presses are lost while the menu is not ready). The MCP `launch` tool
does the same with `env`, `settings` and a script (`keydown vpad attach xbox`, `@sleep 50000`, `keydown vpad button south 1`,
`10`, `keydown vpad button south 0`, repeated at 54 and 58 s). Look for `pacing on (present N, 13xxx draws)` (the first scene) and
then `device is lost`.

What was ruled out (one run each, settings as `[section] key`):

| Test | Result |
|---|---|
| Windows System log | no display-driver timeout (event 4101): a GPU fault, not a timeout |
| `[nvn] debug_skip_compute = 1` (no dispatch runs) | still lost |
| `[nvn] skip_fs_file` = the 5,212 fragment shaders with handle-expression textures | still lost |
| `[nvn] skip_fs_file` = the other 3,296 newly translated fragment shaders | still lost |
| handheld mode + `[nvn] tex_prep_mb = 128` + `[nvn] pipeline_cache =` (empty) | still lost: not memory pressure |
| `[gfx] astc_quality = fast` (no background texture re-encodes) | still lost |
| Vulkan allocation count | limit is 2,342,492 (logged at start now); ~4,100 in use |
| `[nvn] skip_fs_file` = `0`..`f` (every draw with a fragment shader skipped) | **no loss**, gameplay runs |
| only fragment hashes starting `8`, `90-97`, `98-9b`, `9c-9d` or `9e-9f` | **no loss** (minutes of gameplay at 10-15 fps) |
| only `8` and `9` together | lost |

So it is not one bad shader. It looks like an interaction (one pass reading what another wrote, a missing barrier, a layout
problem) or depends on how much is drawn.

**Second machine (RTX 4080, driver 610.88, 2026-11-06): the loss reproduces**, so it is not an Intel Arc driver problem. The first
scene's frame (13,072 draws) replayed in 55.7 s, was presented at ~141 s, and the device was lost at 143.4 s (`work\run\a8_nv1`). On
NVIDIA the process does not end: every later submit fails until `--max-seconds`. NVIDIA adds two cheap ways in, before the per-draw
submit-and-wait mode below: `VK_NV_device_diagnostic_checkpoints` (a checkpoint per draw or pass; after the loss
`vkGetQueueCheckpointDataNV` names the last one the GPU reached) and `VK_EXT_device_fault` (fault type and address, which can be
matched against the runtime's buffer and image allocations). The runtime uses neither yet (`runtime/src/gfx/`).

Next steps for A8, cheapest first:
1. `[nvn] pass_barriers = full` (a full barrier after every pass). If the loss goes away, it is a synchronization hazard in the
   tracked-barrier mode (`nvn_replay.cpp`, `nvn_gpu.cpp`).
2. A per-draw submit-and-wait debug mode in `runtime/src/gfx/nvn/nvn_replay.cpp`: after every draw (or every N), end the command
   buffer, submit, wait on a fence and log the draw number and shader hashes; the first failing wait names the faulting draw.
   Combine with `[nvn] trace_frame` (that frame's draws, resources, buffer ranges) to see what it reads.
3. `[gfx] robust_access = 1` tells out-of-range accesses apart, but changes the driver's pipeline keys: the prepared cache misses and
   everything compiles again (30+ minutes of black screen). Use it only after narrowing the draws down.
4. The Vulkan validation layer with GPU-assisted validation needs the Vulkan SDK (a system install: the user decides; give them the
   LunarG SDK link, do not install it yourself).

Other things seen around the fault, not yet tied to it: `shading rate image bound with a palette of several rates: the image is
not converted` (variable rate shading), `GPU-side WaitEvent` not implemented (skipped in the single-queue replay), mesh shader draws
(`nvnCommandBufferDrawMeshTasks`) that draw nothing.

## Open issues besides A8

Numbered as in the artifacts table of `AGENTS.md`.

- **A10, compute resources**: OoT's compute shaders read storage buffers and texture handles from c[0] offsets the runtime does not map
  (storage at c0 bytes 0xC0, 0xD0, 0x100, 0x110, 0x120, 0x150, 0x160; texture/image handles at c0 words 8-142; Pokopia's table was
  0x210 uniforms-as-storage and 0x310 storage). The same word is a storage descriptor in one shader and an image handle in another,
  so it is a per-program table. Ways in: extract the GLSLC reflection sections with the corpus (`work\shaders\oot\corpus\glslc` is
  empty; `tools/shaders/harness/Nvn2Reflection.cs` parses them), or read the NVN2 driver's compute dispatch code in the `sdk`
  module. Until then, dispatches whose storage buffers map to nothing are skipped (`[nvn] skip_unmapped_dispatch`).
- **A9, mesh shaders**: foliage and terrain use task/mesh shaders (the 800 "stage 6/7 blocks without code"). Neither the translator
  nor the runtime supports them.
- **A7, clustered-light shadows**: 4,336 fragment shaders sample shadow maps through a handle read per pixel; the translator cannot
  bind that and answers 1 (lit). Fix: pool-sized texture and sampler arrays in the runtime, then turn on Ryujinx's
  `GenerateBindlessAccess` (harness `QueryHostSupportsSeparateSampler`).
- **A3**: faint vertical stripes in the title logo/smoke area when no movie is behind it.
- **Remaining untranslated shaders**: 1,071 rows (800 stage 6/7, ~180 compute blocks whose range runs into ASCII names, a few TLD4.PTP
  and TEX.F16.RN forms in compute shaders built into `main`).
- **First-scene load**: the first visit transcodes ~100 ASTC textures to BC7 on the render thread and uploads ~850 MB; a frame took up
  to 112 s. Later starts read the BC7 disk cache. Pre-transcoding during setup would remove it.
- **Performance work not started**: the game renders 2560x1440 internally in docked mode on an integrated GPU (try `[am]
  operation_mode = handheld`); the lifted code is built for baseline x86-64 (every FMADD is a CRT `fmaf` call; FMA3 flags need a full
  25-minute rebuild).
- **Housekeeping**: `recomp/games/oot/main.sites` no longer has the temporary NDVE probe hooks, but `oot_main.lib` was not rebuilt
  (`recomp\scripts\build_game.cmd oot_main`, then rebuild the exe). The runtime tests were not built here (`NXRT_BUILD_TESTS` is off
  in the game build); `runtime\scripts\build.ps1 -Test` would run them, including the new `harness_handle_expression`.
- **Pokopia regression risk** (no Pokopia dump here): the shared changes below also run for Pokopia, especially the harness's draw
  parameter answer, `LINEAR_RENDER_TARGET` as pitch-linear, the shader preparation check and the audio effect fixes.

## What was found and changed on 2026-11-06

Shaders (translator and runtime):
- **`0xf60` is TEX with the handle in a uniform register** (`TEX.SCR ... UR12, 0x0, ARRAY_2D`): URn at bits 40-45, an 8-bit immediate at
  46-53, otherwise the bound TEX 0xb60. Found with nvdisasm bit probes (`tools\nvdisasm\`, CUDA 13.3.1 redistributable).
  `TEX.SCR.B`: the handle is the first register of Rb.
- **Material textures come from a storage buffer table indexed by uniform data.** The Ryujinx patch
  `tools/shaders/harness/translator/ampere-bindless-handle-expr.patch` writes such a handle as a postfix expression; the harness puts it
  in the resource list (`tex<type>[c131072:<n>|<rpn>]`) and the runtime evaluates it per draw (`NvnSource::HandleExpr`,
  `eval_handle_expr` in `nvn_replay.cpp`). 8,624 more shaders translate.
- **Draw parameters**: NVN keeps base vertex / base instance / draw index in c[0] 0x640/0x644/0x648; the runtime never writes them, so
  the harness now answers `QueryHasConstantBufferDrawParameters` true and Ryujinx reads the Vulkan built-ins (1,985 vertex shaders).
  This made the menu glyphs appear.
- **`NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET` textures are pitch-linear.** OoT renders its menu text on the CPU into such R8 textures;
  untiled as block-linear the glyphs came out in 64-texel bands.
- `S2R SR_VARIABLE_RATE` reads 0. Re-translating a subset: filtered `unique.tsv` plus junctions to the corpus folders, `--batch`, then
  `tools\shaders\scripts\merge_results.py`.

Runtime:
- **Shader preparation** re-runs only when more programs of `programs.bin` have usable translations than it prepared
  (`usable_programs` in `nvn_shaders.cpp`); the pipeline cache now defaults to the folder of `programs.bin` / `prepare.state`.
- **Movies**: dav1d chooses its own frame delay and gets half the cores (21 -> 37-63 pictures/s). `[media] video_max_pictures` ends
  every movie early for tests.
- **Opus**: libopus via the repo-local vcpkg; the "Ex" open forms carry {rate, channels} in the raw request data.
- **Audio renderer**: reverb/delay state is built when an effect becomes enabled after its Invalid pass, and the reverb state is
  constructed in place (the effect slot is zeroed bytes; assigning never set the vtable pointers of its delay lines).
- Diagnostics: `[gfx] robust_access`, `[nvn] debug_skip_compute`, `[nvn] skip_unmapped_dispatch`, unknown NVN buffer sources logged
  with shader and offset, Vulkan allocation limits logged at start. All keys are in `runtime/docs/CONFIG.md`.

## Picking up

1. Machine: `DEVELOPMENT_SETUP.md`. Since 2026-11-06 the work runs on a second machine (Ryzen 7 5800X3D, RTX 4080, 32 GB): set
   up from scratch, everything rebuilt and checked (`AGENTS.md`, "Hardware of this machine"). Data that already exists here: game in `game\oot`, lift and libraries in `work\lift\oot`
   and `work\build\oot`, shader corpus, cache, `programs.bin`, `prepare.state` and `pipeline_cache.bin` in `work\shaders\oot`.
2. Build after runtime changes (about 20 s incremental):
   `cmd /c "runtime\scripts\env.cmd && cmake --build game-build\build\release --target game_exe"`, then check the log for `FAILED:`.
3. After translator changes: `dotnet build tools\shaders\harness\SpirvHarness.csproj -c Release`, re-translate what changed and merge
   it. The next start then runs the shader preparation window again if new programs became preparable (15-30 minutes).
4. Start with A8 (step 1 of its list above), and use the MCP `game` tools (`launch`, `wait_for`, `screenshot`, `log`, `reports`) to
   drive runs; crash reports are `game-build\build\release\oot_crash.txt`.
5. Rules that still apply (from the user): one build or one `oot.exe` at a time; tools only in git-ignored repo folders from official
   releases with checksum checks; never change system or security settings; keep `AGENTS.md` current; commit at milestones.
