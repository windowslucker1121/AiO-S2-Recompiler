# DLSS for every game: plan (draft, 2026-11-10)

Status (2026-11-10, branch feat/dlss-wp1): WP1 done (d58ee2c). WP2 mechanism done (`[dlss_pass]`, CONFIG.md, GAME_BOUNDARY.md);
`oot` defaults are inert (`motion = none`): its depth / motion-vector / jitter inputs are not identified (this machine has no extracted game
and no capture). WP0 code done: `tools/release/fetch_dlss.py` (licence flag, pinned sha256, test), `package.ps1 -Streamline` (its
`$SlSha256` and zip name are still to fill on the first real download), LEGAL.md section. WP4 done in the library: This PC "DLSS" row,
Settings hides DLSS/DLAA until the files are there and offers "Download DLSS" (licence dialog first); Play passes the folder as
`NXRT_UPSCALE_DLSS_DLL`. Nothing tested with real Streamline/DLSS DLLs. WP3 not started (optional per section 4). Goal: NVIDIA DLSS Super Resolution / DLAA in the runtime for any recompiled game,
with no per-game code. A game may at most add *data*: config defaults or manifest keys.

## 1. What already exists (more than expected)

| Piece | Where | State |
|---|---|---|
| Streamline backend (DLSS SR / DLAA) | `src/gfx/extras/temporal_dlss.cpp` | Loads `sl.interposer.dll` at run time, checks its NVIDIA signature, `slInit` before the Vulkan instance (`streamline_preinit` from `vk_device.cpp`), mode from the size ratio, tags colour/depth/MV/output. Nothing is linked. Streamline headers v2.14.16 (MIT) are in `third_party/nvidia_streamline` |
| Backend interface | `src/gfx/extras/temporal.h` (`TemporalBackend`, `TemporalFrame`, `NativeTemporalFrame`) | Shared with FSR 3.1 (`temporal_ffx.cpp`) |
| **NVN2 DLSS API** | `src/gfx/nvn/api_dlss.cpp` | All 57 `nvnDlss*` calls + `nvnCommandBufferEvaluateDlss`, object layouts from the SDK. Records `Op::EvaluateDlss` |
| Native evaluate in the replay | `nvn_replay.cpp` `do_evaluate_dlss` | Drains the recorder thread (`cmdq::drain`), runs `temporal_evaluate_native` on the game's own colour/depth/MV/jitter, blits into the game's output rect; bilinear blit when DLSS is off or missing |
| Capture/replay of `EvaluateDlss` | `nvn_capture.cpp` | Textures are captured |
| Camera-derived path (no game MVs) | `temporal.cpp`, hook in `nvn_replay.cpp` (`temporal_gbuffer_note`, `temporal_for_draw`) | Runtime-made camera MVs + runtime jitter, inserted before the tone-map draw. **Game-specific**: camera = title A's uniform layout (VP floats 0..15, view 52..67), tone map found by title E/title A SPIR-V patterns (`hdr_is_tonemap`) |
| Settings | `[upscale] mode = dlss / dlaa`, `quality`, `dlss_dll` (CONFIG.md); library Settings page lists DLSS/DLAA | Done |
| Getting `nvngx_dlss.dll` | `fetch_dlss.py` (pinned NVIDIA GitHub tag v310.9.16 + sha256) and the Tauri launcher's "Download DLSS" | **Removed** in a069ac5 with the old launcher; no package ships `sl.*.dll` now |

The game: it resolves the `nvnDlss*` procs at boot (`work/run/boot31`) but no run log has a `DLSS network` line: it never initialises a
network. Its upscaler is its own compute chain in `main` (fea98fe7 / f90d726d / 49659bca / 02dcb888, A12/A22).

## 2. Three routes, ordered by how generic they are

**Route A: the game calls NVN2 DLSS (fully generic).** Every platform 2 title that uses the console's DLSS hands over colour, depth,
motion vectors, jitter, MV scale and history reset itself. The runtime maps it 1:1 to PC DLSS. Code exists, never run on a real
title. This is the main deliverable.

**Route B: the game has its own temporal upscaler / TAA pass (generic engine, per-game data).** Replace that pass's dispatch or
draw with DLSS. The inputs are named in config, not code, extending the existing generic `[nvn] upscaler_replace / pass_copy`
mechanism. The game is the pilot.

**Route C: the game has no temporal data (generic heuristics, research).** Runtime-made camera MVs + runtime jitter, as the title A
path does, but with the camera block and the tone map found automatically. Camera motion only: animated objects ghost. Last.

## 3. Work packages

### WP0: distribution and licence (blocker for players, not for development)
- `nvngx_dlss.dll` is under the NVIDIA RTX SDK licence: not in the repo, not in the GPL package. Restore the download-on-request
  flow: bring `fetch_dlss.py` back from `a069ac5^:game-build/release_b/fetch_dlss.py` into `tools/release`, update the pin to the
  DLSS version matching the chosen Streamline release, show the licence and run it from the library (This PC / game Settings,
  "Download DLSS"), target `<exe dir>\upscalers\streamline\`.
- `sl.interposer.dll`, `sl.common.dll`, `sl.dlss.dll`: Streamline is MIT; ship the signed binaries of the pinned release in
  `upscalers\streamline\` (package.ps1: download zip, sha256 check as for Zig/Python). Record in THIRD_PARTY_NOTICES.
- **Decision for the user / LEGAL.md**: GPL-3.0 program loading a proprietary DLL the user downloads. The old design (user-initiated
  download, nothing proprietary shipped) is the safe line; LEGAL.md has no DLSS section yet: add one.
- Done when: a fresh package + one click gives a working `[upscale] mode = dlss`.

### WP1: Route A hardening (generic core)
1. **Synthetic runtime test** (`runtime/tests/gfx`): record `nvnDlssNetworkInitialize` + `EvaluateDlss` with synthetic textures
   (no game data); check the fallback blit path always, and the DLSS path when the DLLs are present (skip otherwise, like the
   validation-layer tests).
2. Check the interplay with this branch's newer render-thread machinery, each one a known risk:
   - recorder thread: drain exists before evaluate; verify nothing is queued *during* Streamline's direct recording;
   - `[nvn] hazard` (off by default): it does not see Streamline's writes. Force a full barrier after evaluate (already
     `memory_barrier`) and mark the output range written for the tracker;
   - held compute results / pool copies: the inputs may be compute results held in VRAM (`out_*`); resolve them the same way a
     sampled texture is before tagging;
   - `tex_park`, set memo / `set_keep`: `g_dirty = kAllDirty` is set; also bump the resolve epoch since DLSS writes an image.
3. Device creation through Streamline: confirm every extension this runtime needs (mesh shader, cooperative matrix, float16,
   unified layouts, GPL, BDA) still comes through `sl.interposer`'s `vkCreateDevice`; log the delta. If not, fall back to "DLSS off"
   with a clear log line, never a failed boot.
4. Mode/quality: today the network method decides the render size via `optimal_size`; respect the game's method when `[upscale]`
   quality is `Native/auto`, and use `slDLSSGetOptimalSettings` (already fetched, unused) instead of fixed percentages.
5. Map the remaining inputs: exposure texture / pre-exposure (`sl::kBufferTypeExposure`, `preExposure`), bias-current-colour
   mask (`kBufferTypeBiasCurrentColorHint`), MV-jittered and low-res-MV flags (bits 2/4 of the network flags), DLSS preset
   (`kNPreset` -> `DLSSOptions::dlaaPreset/qualityPreset...`). Several network instances: Streamline viewport per `history_key`.
6. Find a test title: any platform 2 game in reach that calls `nvnDlssNetworkInitialize` (log line `DLSS network ...`). Without one,
   the synthetic test + a capture-replay test are the evidence; say so in AGENTS.md.

### WP2: generic "replace a pass with DLSS" (Route B)
Config, no code per game (CONFIG.md, game `config_defaults`):
```
[dlss_pass]
match     = cs:fea98fe7,cs:f90d726d      ; shader hash prefixes of the pass(es) to replace; first one evaluates, the others are skipped
color     = tex0                         ; binding names as upscaler_src/dst already use them: texN, imgN, depth_rt, fs:<hash>:texN
depth     = depth_rt
motion    = tex2 | none                  ; none -> camera-derived MVs (route C)
output    = img1
jitter    = ubo:<slot>:<word>,<word> | proj | none   ; where the game keeps its jitter, or read it from the projection
mv_scale  = 1,1 | render | output
reset     = ubo:<slot>:<word> | auto     ; auto: camera cut heuristic (MV magnitude / view jump)
depth_reversed = 1
```
Steps: (1) reuse the binding resolver of `upscaler_replace` / `pass_copy`; (2) feed `NativeTemporalFrame` and call
`temporal_evaluate_native` (one code path with Route A); (3) a `[nvn] trace_frame` helper that lists each candidate pass's
bindings (format, size, render vs output size) so finding the inputs for a new game takes a capture and minutes, not reverse
engineering; (4) document the recipe in GAME_BOUNDARY.md.
**The game pilot**: on capture `20261108-221243-drs-drs1080`, identify the chain's colour, depth, MV and jitter inputs (f05596b9 /
54ecc4b4 / 4c60af1f read them; A10 lists the bindings). Unknown so far whether the game writes per-pixel MVs; if it does not, use
`motion = none`. Measure quality (SSIM vs the game's own network) and GPU time (the network is ~2 ms; DLSS on a 4080 ~0.5-1 ms
at 1080p -> 1440p).

### WP3: Route C, automatic camera (research, optional)
- Camera block: scan vertex-stage uniform blocks of the draws that write the frame's main depth target for a 4x4 matrix whose
  product with a few reconstructed positions reproduces that draw's depth (check on the GPU once per new program); remember the
  (program, slot, offset) per game in `pipeline_keys`-style cache.
- Tone-map point: first full-screen draw that reads the main HDR colour target and writes an 8-bit / display target; replace the
  Title E/title A SPIR-V patterns.
- Jitter: runtime adds Halton jitter to the found VP (exists: `temporal_jitter_block`) and sets negative LOD bias on samplers.
- Expectation: ghosting on animated meshes; offer only as "DLAA (experimental)" per game.

### WP4: UI and docs
- Library Settings: show DLSS/DLAA only when an NVIDIA RTX GPU is present and the DLLs are there; otherwise the download button.
- This PC: a "DLSS" check row (GPU, driver, DLLs).
- In-game F1 / menu: mode and quality live (DLSS options can change live; only *turning DLSS on* needs a restart because
  Streamline must own instance creation: consider always starting through Streamline when the DLLs exist, so DLSS can be switched
  on live; measure that the interposer costs nothing when idle).
- CONFIG.md, GAME_BOUNDARY.md (route B recipe), AGENTS.md state.

## 4. Order and estimate

1. WP1.1-1.3 (test, render-thread interplay, device extensions): small, de-risks everything. ~1-2 days.
2. WP0 (packaging + download): ~1 day + the licence decision.
3. WP2 with the game as pilot: the visible result on the game we have. ~3-5 days (most of it identifying the inputs).
4. WP1.4-1.6 and WP4: polish. ~2 days.
5. WP3: only if a title without NVN DLSS and without its own TAA needs it.

## 5. Risks / open questions
- No local title is known to call NVN2 DLSS: Route A can only be proven synthetically until one is found.
- Streamline in front of `vkCreateDevice` may drop or reorder features this runtime relies on (WP1.3).
- The game may have no per-pixel motion vectors (its network could be spatial + its own history); then Route B for the game needs Route C's
  camera MVs and quality drops.
- Licence: user decision on shipping the Streamline binaries and the download flow (WP0).
- Non-NVIDIA GPUs: unaffected; FSR 3 shares every input path, so Route B/C work for FSR 3 too at no extra cost.
