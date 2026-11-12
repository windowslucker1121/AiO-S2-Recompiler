# The game performance: state, history, what we tried, what is running

Living document for the frame-rate work. Detailed per-session notes, with every key, measurement and pitfall, stay in
`AGENTS.md` ("Performance work" and the "Session 2026-11-xx" paragraphs). Config keys are in `runtime/docs/CONFIG.md`.
This file is the overview: where we are, how we measure, what paid, what did not, and what is next.
Last update: 2026-11-12, branch `perf/render-r6` (R6).

## Goal and constraints (from the user)

- **A real, steady 60 fps in the village** (`load_state("village_outside")`), the heaviest scene, at the **pinned render
  resolution 1920x1080** (`[oot] render_height = 1080`).
- No DLSS, FSR, frame generation, lower resolution or skipped game rendering. The game's own post-process chain (including its
  upscaler network) stays. Resolution changes only via DLSS-like tech later, and only if the user asks.
- Beyond 60 is welcome. Measure everything and keep only what pays.
- The full shader preparation (~3 h) runs only at the very end. So do the release build (`oot.exe`, `recomp_library.exe`) and the
  area-load frame work.

## Where we are (2026-11-12, RTX 4080, Ryzen 7 5800X3D, 1920x1080, uncapped)

**2026-11-12 (P2, `perf/p2-coherence`): the "coherence v2" gate (copy always the GPU's binding) measured; the full model was not built.**
Measurement-only switch `[nvn] exp_coh_free` (`off | on | ab`; CONFIG.md, with `exp_coh_free_parts` and `exp_coh_free_cs`): graphics
draws bind every buffer from the pool copy unchecked when the copy has held every page of the range at some point (`Mirror::ever`),
skip the draws' held-result checks (`out_before_op`; indirect arguments are still settled) and the graphics pointer-window flushes
(`gmem_before_op`), get pointer slot masks 0 and read-only storage from the copy. Wrong picture allowed (pages the CPU rewrote since).
Village outside, 1080 uncapped, frame trace, paired 300-present windows (`cpuperf_ab_what = none`), another agent's idle-priority build
(4 clang-cl) on the machine in both window kinds:
- **All parts (65 pairs): fps +5.2 median (61/65), render replay -0.79 ms (59/65), GPU busy -0.98 ms, GPU exec -1.01 ms, game sync wait
  -0.86 ms** (means on 83.3 fps / replay 10.31 / busy 10.34 vs off 78.2 / 11.03 / 11.18). The render-thread gate (replay >= -1.5 ms) is
  not met; the GPU drops by ~1 ms in every pair. So the per-draw coherence work is worth ~0.8 ms of replay at most, not the ~2 ms the
  sampler share suggested (most of those functions' time is checks that a v2 would still need in another form, or below the noise).
- **Which part pays** (one part per run, 36-47 pairs each; absolute fps differs between runs, compare deltas):
  - read-only graphics storage buffers bound from the copy (`exp_coh_free_parts = 4`): **GPU busy -1.51 ms**, fps +1.9 (27/36), replay
    +-0. Today those bindings read the pool (host memory over PCIe) unless a held result covers them. This is the lead: bind read-only
    storage of draws from the copy with the same valid-page decision uniform blocks get (`resolve_read_dep`), not unchecked. (Not the
    dropped `exp_ssbo_copy`, which also marked their slots for pointer shaders and paid for the slot search.)
  - graphics pointer slot masks 0 (`= 2`): nothing (fps -0.05, GPU +0.00): only 45 of ~7,000 graphics pointer-stage tables have a slot
    marked; graphics already runs the inline fast path. The slot-mask lever is compute's (`exp_cs_gmem_none`, 3.7 slots per dispatch).
  - unchecked bindings + no held-result checks + no pointer-window flushes without the storage part (`= 1`): replay -0.47 ms but GPU
    +2.1 ms and fps -10.9: draws then read held compute results from the pool (stale culling data, more work). Correctness, not a lever.
- **Unchecked compute uniforms hang the GPU.** The first form of the gate also took compute uniform blocks from the copy unchecked: TDR
  60-90 s in (device checkpoints: compute cs 961b7f2d 64x64, after beb1205f; instruction-pointer faults = a runaway loop on stale
  CPU-written data). Any v2 must keep CPU-written pages current before every compute reader.
- **What a full v2 would still need** (not built, per the user's wrap-up): CPU-written pages brought current per submission chunk (a
  sweep driven by the front plan's addresses; write-watch as the dirty collector for uncached pools, arm before copy; flush-driven for
  cached ones) before *every* reader, compute included; GPU results resident in the copy with only CPU readers fetching (guards, first-page
  counter reads); page-table entries at the copies, updated GPU-ordered (vkCmdUpdateBuffer after the writing dispatch), for compute's slot
  masks. Expected from this gate: ~0.8 ms replay and ~1 ms GPU in the village, most of the GPU part reachable with the storage change alone.

**2026-11-12 (R6, `perf/render-r6`): set 0's uniform blocks are dynamic uniform buffers (`[nvn] ubo_dynamic`, default on).** Village
outside 1080 uncapped, no frame trace, 60-200 s, same machine: before 87.5 fps, 1% low 65.9, frames over 10 ms 97.8%, render replay median
10.88 ms; after 90.0 fps, 1% low 65.7, frames over 10 ms 80.7%, replay median 10.21 ms (first form; the tuned path adds ~0.2 ms more in
pairs). Paired windows (`cpuperf_ab_what = r6`, the old path on the same dynamic layouts): first form replay -0.38 ms (47/49), fps +1.0
(45/49); tuned form replay -0.60 ms (45/45), fps +1.4 (45/45); with the frame trace replay -0.54 ms, fps +1.25 (32/33).
- **What it is:** graphics programs' set-0 uniform blocks are `UNIFORM_BUFFER_DYNAMIC` (up to the device's limit, by binding number). A
  draw's set 0 is the program's set for the *buffers* its blocks resolve to (pool-copy chunk or pool chunk), bound with the blocks' offsets
  as dynamic offsets. Per draw only each block's copy-or-pool decision is taken: a per-address memo (and a per-stage-slot cache of the last
  draw's entry) checked with `read_dep_check`, exactly as a kept set checks it; then the program's last set (61%) or a set by content
  (39%). No signature, memo, kept-set table or build for the ~2,000 per-object constant sets per frame. Sets come from the persistent pools
  and are forgotten when a pool unregisters or the persistent pools start a new era. Update ranges, unresolved or unaligned blocks,
  render scales and eager results take the old path (zero offsets).
- **Ceiling first:** `[nvn] exp_set0_free` (measurement only: set 0 bound unchecked from the program's last kept set, wrong picture) gave
  replay -1.05 ms in pairs; the dynamic path keeps ~0.35 us per draw of decision checks and the bind (exp_op_cost: 0.73 ms per frame for
  ~2,100 draws), so it realizes ~60% of the ceiling.
- **Costs:** every graphics pipeline layout changes, so every pipeline builds again (a fresh instance needed one ~5 minute run of background
  builds; the full shader preparation at the end covers it). The hazard tracker now applies each bind's dynamic offsets (it took dynamic
  descriptors to the buffer's end before).
- **Correctness:** `set_reuse = verify` compares every block decision with a fresh resolve: 0 of 173 M differ (an early run without the
  race classification showed 2 of 148 M; with it 0 real and 0 raced); kept sets / memo / `odb_incremental` verify 0; A18 on the nb3
  capture 41 mesh draws, x sum 4,398 in every traced loop; village capture replay with and without it pixel-identical; runtime tests
  170/170; in-game screenshot normal.
- **What is left (exp_op_cost per present, village):** bind_sets 2.6 ms (set 0 dynamic path 0.73, set 2 textures 0.96 of which 0.67 builds
  for ~420 first sightings, set 1 0.41), prepare_textures 0.70, out_before_op 0.50, pipeline 0.37, vertex buffers 0.27, dynamic state
  0.27. Set 2's first sightings (`[nvn] r6_diag`, by program): mostly bound textures (`SamplerBinding`) whose handles are new every
  frame (fs 05a99b18 ~13 textures per set, 95d3ec66, a14ae378, ...): the sets are new by handle, not by content; R3/R4's image-keyed
  variants did not pay. **The GPU is now close:** with the frame trace GPU busy ~10.9 ms and exec ~12.0 ms per frame (88-90% load at
  2.8 GHz without it), and the render thread idles more in the faster windows (+0.35..0.42 ms): further replay cuts convert into fps at
  less than half their size.
- **Texture sets by image (`[nvn] tex_img_sets` + `tex_img_early`, default on).** `[nvn] r6_diag` showed the ~420 texture-set first
  sightings per frame are new by handle *and* by texture object (0 found by texture object in an earlier present) but ~87% are the same
  images: the game makes new texture objects every frame for the same images. A texture set whose bindings are all sampled textures is
  looked up by what its descriptors hold (per binding the image's view stamp, which is unique per image and per clearing of its views,
  the binding, the texture's swizzle and depth/stencil mode, view info, format and sampler info, plus the sampler tuning), first, before
  the kept-set table. Made once from the persistent pools; a destroyed image never returns its stamp. Paired against the kept-set path
  (`cpuperf_ab_what = r6t`, final form): replay -0.64 ms (44/45), fps +1.2 (39/45); verify 6.7 M look-ups, 0 differences; A18 4,398 every
  loop, capture replay pixel-identical. Before/after the early placement alone: replay -0.13 ms (39/45), 1% low +0.65.
- **Final R6 run** (same settings as the baseline run, 60-200 s, quiet machine): 88.8 fps, 1% low 61.1, frames over 10 ms 62.4% (baseline
  97.8%), over 15 ms 1.31%, render replay median 9.41 ms (baseline 10.88), render idle 1.52 ms (baseline 0.38). Mean fps barely moved
  against the first ubo_dynamic run (90.0) because the 4-frame cycle locked into 12.8 / 13.0 / 9.3 / 10.0 ms: the two heavy frames are
  no longer the render thread's.
- **Dropped:** `submit_async` again (now `cpuperf_ab_what = r6b`): replay -0.21 ms in two runs, fps +0.35 (29/45) / +0.05 (24/45): off.
- **Where the frame is now** (village, no trace, ~88-90 fps): render replay median ~9.7-10.0 ms, render idle ~1.0-1.5 ms, GPU 91-95% busy
  (cpuwatch GPU load) and the game's own CPU frame alternating: by draw-count rank in the 4-frame cycle 12.9 / 13.1 / 9.4 / 10.0 ms with render
  idle 2.1-2.5 ms in the two heavy frames (the game's main thread waits longer on its workers there). The game's GPU sync waits on the big
  submission of the previous frame (`chain.py`: queued -> replay start 1.6 ms, replay 9.2 ms, the GPU ends ~4.6 ms after the replay with
  the trace): GPU and game CPU frame are now co-limits with the render thread. Stamped GPU per frame (gpu_passes + gpu_draws, inflated):
  culling submission 3.0 ms (end-of-submission copy-backs 0.39, culling root 0.38, HiZ 0.19), main 10.0 ms (draws 7.2, gaps 1.8 = waiting
  for the render thread, pass ends 0.27).

**2026-11-12 (R5, structural render-thread work, `perf/render-r5`): a front thread walks every submission ahead of the replay
(`[nvn] front_plan`, default on).** Paired windows without the frame trace (village outside, 48 pairs): **fps +1.92 (47/48), render
replay -0.65 ms per frame (47/48), 1% low +0.48**; with the frame trace replay -0.54 ms (40/41). The village is ~85-88 fps with it (start of
R5: 85.5 fps, 1% low 59.1, replay median 11.1 ms); replay now ~10.4-10.6 ms per frame, render idle ~0.9 ms, so the frame is no longer
purely render-thread bound (game GPU sync wait ~4 ms, GPU ~91% busy in cpuwatch). The 100 fps target (replay < ~8 ms) is not reached.
- **What the plan is:** the game thread makes a plan per submission and hands it to the replay lambda and to the thread `nvn-front`, which
  walks the lists as the replay will (every `CallCommands` followed, state carried across submissions) and writes a stream in replay
  order: the lists each `CallCommands` calls (held by the plan; their last references are dropped on `nvn-front`, not on the render thread),
  the physical addresses of uniform/storage bindings (checked against the pool-registry and virtual-map generations at every use), and per
  draw/dispatch the program layout and `set_signature`'s raw-input hash of each set. The render thread reads the stream with one cursor while
  it is still being written (a 0.2-0.3 ms walk per submission; waits for the walk ~1 per 20 s); a record that is not the next entry's
  ends the plan for the rest of the submission; the forward scans (mesh/fill/pass look-aheads) find entries by record once the walk is done.
- **Correctness:** handles are never reused and `drop_locked` sets `dropped` before it erases a handle (handle_lookup's cache relies on the
  same flag), so "not dropped" is exactly what `handle_lookup` returns at that moment; a drop between the plan's check and a verify look-up
  is counted as a race. `front_plan = verify`: 0 differences (calls, addresses, signatures) in 37 M calls / 68 M addresses; `set_reuse` and
  `odb_incremental` verify 0; A18 on the nb3 capture 41 mesh draws, x sum 4,398 in every traced loop; village capture replay on vs off
  pixel-identical; runtime tests 170/170.
- **Ceilings measured first (1 kHz sampler, render thread busy samples, start of R5):** flat; the coherence-free share was ~20-25%
  (handle look-ups 3.4%, set signatures 3.6%, texture resolution 3.9%, physical addresses 2.1%, pipeline key/look-up 3.4%, program layout
  1.2%, mesh look-ahead 2.8%, list frees ~1-2%). The plan realized about the share it took over: a hash map keyed by record (first form)
  cost as much as the look-ups it replaced (fp_phys 2.3%); the sequential stream and releasing lists elsewhere gave the gain.
- **Found on the way:** texture handles cannot be planned: the game's material handles are read from storage buffers (GPU-written
  material tables), 0 of the planned handle reads applied. Set-signature hashes are planned (48 M per 300 s) but added nothing measurable
  over the calls/addresses part (-0.52 vs -0.53 ms). Unpaired back-to-back runs (front on/off) showed the 4-frame phase-locking swing of R4
  again (85.6 vs 87.6 fps with replay 10.4 vs 11.0 ms): judge by paired windows.

**2026-11-12 (R4, render thread and long frames, `perf/render-r4`): the render thread is now the only limit for 100 fps** (GPU busy
~8.5-8.9 ms after G4; render replay ~11.0-11.8 ms per frame, idle 0.05-0.2 ms). Alternating final runs, R3's exe vs R4's (same shared shader
cache, quiet machine): fps 78.3 / 83.4 vs 82.7 / 84.1, 1% low 51.2 / 60.0 vs 42.9 (one 170 ms render stall during an outside download) / 61.9,
frames over 15 ms 14.9% / 0.9% vs 1.2% / 0.6%, frames over 10 ms ~97-99% either way.
- **The run-to-run swing of frames over 15 ms (R3: 12.4% vs 0.7%) and the "~65 s burst" had one main cause: texture image churn**
  (`tex_share_index`, below). The game makes a 960x540 R11G11B10F target as a new texture object every fourth frame; when the new object's
  first use came after the old one's finalize but before the render thread ran its release, nothing was found (alias search: live objects
  only; park: not yet parked) and a new VkImage was made, the old one destroyed a frame later. Which order the threads took changed from run to
  run and for minutes within a run: windows of 38-62 fps, image creation up to 200 ms (21 images, 219 ms in one frame), GPU stalls (sync waits
  13-110 ms). With the index, steady textures created per frame 0.2-1.2 -> 0.00.
- **What is left in frames over 15 ms on a quiet machine:** the heavy frames of the game's own 4-frame cycle (draws ~2,290 / 2,300 / 2,165
  / 2,060). With no slack in the pipeline (now render-bound) the heavy frames' cost lands in their own present interval; which phase a run
  locks into decides whether that shows as 12/12/12/10 or 14/14/11/10 ms (`tools/perf/frames_report.py <frames.csv>` prints it as "ms by draw rank in 4-frame blocks").
- **Long frames from outside the game:** other agents' ThinLTO links (lld-link, ~8 cores for 15-30 s), dotnet / VBCSCompiler builds and
  translator batches during a run gave 40-80 ms game-side frames and 60-70 fps windows; once a 5 s whole-process stall (audio thread too).
  Use `tools/perf/cpuwatch.ps1 -Out <log> -Seconds <n>` (every 5 s: processes over 0.3 cores, GPU clock / load / power / memory) beside every measuring run and discard
  windows with outside load. Do not build while one of your own runs is measuring (a below-normal build still cost ~15 fps in the last 30 s).
- **Render thread at its floor in this design.** exp_op_cost (now split per set index and step, and per begin_draw phase): per frame
  bind_sets ~3.1 ms (set 0 uniform kept-set checks ~2,000 at ~0.39 us, set 2 texture builds ~420 at 1.6 us of which 1.3 us resolving),
  prepare_textures 0.7, out_before_op 0.5, pipeline key + look-up 0.4, vertex buffers 0.3, the rest per op (~30k ops: BindBuffers 11k,
  BindTextures 5.7k, ReportCounter / ResetCounter 1.5k each at ~0.2 us). No single item over ~0.8 ms; every memo tried (pipelines, set-0
  memo bypass, content-keyed texture sets) found too few repeats or too little cost behind the look-up. 8.5 ms needs a structural change
  (see "What is open").

**2026-11-12 (R3, render thread): village ~74 -> ~82.6 fps, 1% low ~55 -> ~60, frames over 15 ms 5.9% -> 1.5% (no frame trace, no
sampler; `perf/render-r3`).** The big step was not a cut but a placement: the render thread and the recorder thread pinned to two separate
physical cores (`[nvn] render_cpus = auto`). Unpinned, Windows let them share a core (SMT siblings) or migrate; the hazard tracker's
recorder work then slowed the render thread (that was its whole "render-thread cost": the sampler showed no extra render-thread self time).
Pinned, the hazard tracker (`[nvn] hazard`, now on by default) gives its GPU gain: hazard on vs off under pinning fps +6.2 (24/24), 1% low
+5.1, median frame -1.25 ms; pinning with hazard on +6.8 / +7.1 fps (24/24 twice). Smaller render-thread cuts on the way (paired, replay ms
per frame): r3a -0.45, dset_recycle -0.34, copy/clear queue -0.34, occ_mark_late -0.10. With pinning and no hazard the render thread has
~1-2.7 ms idle per frame: the village is GPU-bound again (~12 ms of GPU per frame).

**2026-11-12 (R8, render thread): village 56 -> ~68 fps, 1% low ~34 -> ~47-49, render replay ~17.4 -> ~13 ms per frame (no PGO), frames
over 20 ms < 1%. The village frame is now GPU-bound** (GPU submissions cover 98.6% of wall time, GPU span sum ~14.5 ms per frame = the
frame; starve 0.13 ms). The game's main thread waits ~6.8 ms per frame on the previous frame's big submission (#6, not #3): queued ->
replayed in 11.4 ms -> its GPU work starts 3 ms before the replay ends (the GPU is still busy with earlier work) -> 11.3 ms of GPU -> the
wait ends at the GPU end. The render thread finishes ~8 ms before the GPU and idles ~1.5-2.5 ms per frame. Frame < 10 ms needs GPU time
per frame < 10 ms; the CPU chain cannot shorten it.

| Scene | Start of plan D (854x480) | First 1080 runs (W7) | 2026-11-09 evening | 2026-11-10 (G1) | Now (fps100) |
|---|---|---|---|---|---|
| Village outside | 21-24 | ~16 | 36.6 | ~49.6 | **56.2**, 1% low 34.2 (same run, fps100 features off: 51.7 / 31.3; 19/19 pairs) |
| First scene (intro village) | 34-39 | 25-43 | 50-62 | ~62-66 | **+5.9 fps** over the whole intro (27/27 pairs; 59.3 -> 62.7 in its late part) |
| The house scene | 47-52 | 31-33 | 64 | ~79-83 | **93.0** (off: 87.0; 30/30); 96 fps with the default `auto` cap of 100 |

- Village frame: ~17.5 ms. The **render thread replay is ~17.4 ms per frame** (the limit) and the **GPU is busy ~14.8 ms per frame**
  (was ~18.8 before flush_copy). For 100 fps both must reach ~9-10 ms; the house scene is there (replay 9.3, GPU 9.8).
- The game's main thread (G) is ~10 ms busy per frame. It caps near ~95 fps.
- Render thread in the village (exp_op_cost, per frame): DrawElements 10.0 ms for ~2,100 draws (bind_sets 4.2, out_before_op 1.0,
  prepare_textures 1.2, the rest decode / pipeline and layout lookups / dynamic state / texture resolve), command-list overhead ~2.5,
  dispatches 0.9, mesh draws 0.9, occlusion reports 1.1. Most draws change the program, so set-level memos help little.
- Frame cap: `fps_limit = auto` caps at `[display] auto_fps` (default **100**, 0 = the display's refresh rate) for the game since
  2026-11-11 (fps100). Movies are always held at 60 (`movie_cap`); with `vsync = 1` the display still bounds it.

## How to measure

- **Alone on the machine**, under the game lock:
  - `work\game.lock` is created with PowerShell `New-Item`, which must succeed. Check that no `oot.exe` runs first.
  - `work\game.next` holds a reservation.
  - Other agents' builds run at below-normal priority with at most 6 jobs.
- Runs: `load_state("<state>", instance="<name>", settings={"memory.limit_gb": 24})`.
- **Uncapped:** pre-create the instance folder `work\ai_run\oot\<name>` with a `controls.ini` holding `[display] fps_limit = 0`.
  The MCP setting `display.fps_limit` does NOT reach the limiter.
- **Paired windows**, the standard method:
  - `[nvn] cpuperf_ab = 300` with `[nvn] cpuperf_ab_what = <feature>` switches one feature on and off every 300 presents.
  - Compare each "on" window with its two "off" neighbours.
  - The `ab window` lines give fps, 1% low, median frame, render replay ms, GPU exec/busy ms and full barriers per frame.
  - Absolute fps between separate runs varies by ±2-4.
  - **Bias before 2026-11-12 (R8):** `ab_on()` called `ab_mask()`, a guarded function-local static, only in "off" windows, hundreds of
    thousands of times per frame. With nothing switched (`cpuperf_ab_what = none`) the off windows showed 0.6-1.1 ms more render replay
    (13/13 pairs). Paired *render replay* gains measured before d6af625 are inflated by about that much (GPU-bound fps and GPU times
    are not). Fixed: ab_mask is a constant-initialized atomic; `none` now gives -0.08 ms (7/13). Run a `none` run first when in doubt.
  - Build-level changes (compiler flags, PGO) cannot be paired: alternate two exes back to back (`NXRT_AI_EXE`), two runs each.
  - Critical path of the main thread's waits: frame trace events `q.sub`, `replay.s`, `sync.w`, `gpu.raw` (serial in the arg; raw GPU
    timestamps mapped onto the CPU clock through the waits' ends).
- **Feature switches:** `AbFeat` in `nvn_replay.cpp` is a uint64 bit set. All 64 bits are used (59-62 R8: handle_big, chunk_async,
  pds_q, fb_image; 63 pt_copy). Free bits by retiring dead experiments first. R3 took the bits of chunk_time, keep_writes, vi_upload,
  cs_gmem_flush, vrs, sig_tex and alloc_batch (those keys still work, just without paired windows) for `dset_recycle`, `render_pin`,
  `occ_mark_late`, `copy_image_q`, `tex_keyed`, `submit_async` and `r3a`; the cmd_copies bit went to G3's `g3b`. R4: `tex_keyed` -> `tex_share`, `submit_async` -> `keep_chain`, `lookahead` -> `hot_skip`.
- **Since R3 the render thread and the recorder are pinned (`render_cpus = auto`) and `hazard` is on by default.** A run that should
  compare against older numbers sets `nvn.render_cpus: "off"` and `nvn.hazard: "off"`.
- **Real-clock GPU breakdowns:** these are measurement-only skip words in `[nvn] skip_fs_file`, and live edits take effect:
  - `c:<prefix>` leaves out dispatches of that compute shader;
  - `bar` / `bar:<line>` drops full barriers;
  - `v:<prefix>` and `gpt` leave out draws.

  `[prof] gpu_passes` / `gpu_draws` drain the GPU at every stamp and down-clock it, so use them only for proportions.
- **Other tools:**
  - `[prof] gpu_stats`: pipeline statistics per draw family (G1).
  - `[nvn] exp_draw_cut`, `exp_occ_off`, `exp_vi_upload`, `exp_static`, `skip_gpt`: measurement only.
  - `[diag] frame_csv`: every frame with its phases.
  - `[prof] trace` + `sample_hz = 100`: zone trace and sampler.
  - `tools/shaders/pipestat`: driver registers and binary size of a .spv, offline.
- **Correctness after every change:**
  - Village capture replay SSIM (`20261108-000413-a9g1-village`, `20261107-230737-nb3-alias_kid`).
  - A18 culling records stable from loop to loop: 41 mesh draws, x sum 2,999 / 4,398.
  - In-game screenshots you actually look at: foliage, crowns, fog, and the upscaler (no 8-px stripes).
  - `set_reuse = verify` / `odb_incremental = verify`: 0 differences.
  - `runtime\scripts\build.ps1 -Test`: 174 groups. `gfx_nvn_scene_state` is a known flake; rerun it alone.

## Frame anatomy (village outside, 1080, before G1 / P5)

Measured by K1 at real clocks; detail in the git-ignored `work/notes/k1_roadmap.md`.

- 6 submissions per frame. The game's main thread waits on the fence of **#3**: culling, HiZ and fog, compute only. **#6** is the main
  frame.
- **#3 cost.** It was 8.6 ms and is now ~4.9 ms after C1 and P4. Its parts:
  - HiZ a17259b2 ×6: 2.0 ms, now 0.24;
  - pointer compute 39a1f760 / 5d8324bd / a68b51f9 / d89e74b6;
  - culling 4c1e8564 + f2422a7a / f0f09798 / 5fc79285;
  - fog 20593fd9 group;
  - our copies.
- **#6 cost: ~18.7 ms.**
  - 55 passes with ~2,200 draws.
  - The gpt (pointer) foliage vertex/mesh stages take ~3.8 ms.
  - The depth prepass 45ab0a1f takes ~2.2 ms. It is needed: removing it costs more.
  - Shadows take ~2.3 ms.
- **G1's draw anatomy.**
  - Leaving every draw out: 22.7 → 5.9 ms of GPU.
  - Draws without fragments: only −2.6 ms. Fragment shading is cheap, and early-Z works (1.0-1.2 fragments per visible sample).
  - Rendering at 480 instead of 1080 saves only 1.1 ms. **Pixel count is not the cost.**
  - The draws' own work (recording every draw twice: +9 ms) is mostly the gpt pre-raster stages.
  - The rest was work around the draws: occlusion reports 3.1 ms, now fixed; the game's occlusion queries ~1.0 ms; index and small
    vertex buffers over PCIe ~1.1 ms.

## What paid (kept, default on)

Gains are from paired windows unless stated. Rows are in rough chronological order.

| When / who | Change (key) | Gain |
|---|---|---|
| D1/D3 (854x480) | page checks word-wise; occlusion queries inside the pass (passes 670 → 45 per frame); shared c[0] tables; interval index of held results (`out_index`); pass chunks; `pool_lookup_snapshot` | 11.6 → ~38 fps first scene at 480 |
| G-1/G2/G3 (lifted code) | `/O2 -march=x86-64-v3`; function-local NZCV; scratch registers x9-x17 as locals; inline caches for indirect calls (`NX_ICALL`); exact closed-form FRSQRTE; opt-in PGO | main thread G 13.2 → 10.5 ms (−21%) |
| R2 | `tex_park`: parks the GPU-written images of per-frame texture objects | Village 21 → 29 fps |
| R3 | `gmem_ro_bindings`, `overlap_grow`, `out_index_inc`, `flat_set_maps`, `pool_sync_cache` | render −1.35 ms |
| S1 / R4 | `set_keep` (persistent validated descriptor sets); `odb_incremental`; recorder thread `cmd_thread` | Village replay −5 ms; house +1.75 fps |
| P2 | `out_delta`: copy-backs of changed words only; `write_defer`; fp32 FMUL.RZ shaders | first scene 39 → 45 fps |
| H1 | `ww_quiet_wait`, `tex_park_presents` 240, `bc7_format_index`, `present_pace`, prewarm key saver, movie cap | Village 1% low 11.6 → 23.9 |
| T1 | `tex_lookahead`: textures named by handles prepared ahead | camera cut worst frame 703 → 95 ms |
| R5 | background pool imports; `pool_sync_depth_ids`; `c0_persist` | load frame 2.7 s → 0.75 s; village replay 26.7 → 17.8 ms |
| U1 (A22) | TLD4 `.F16.RN` decoder fix: no upscaler stripes at 1080 | picture |
| S2 / S3 | gpt helpers out of line with an inline page cache; new CFG structurizer | pipeline builds 16-82 s → 1-3 s; fragment registers mean 136 → 93 |
| step 2 | `cap_at_display_rate = true` (uncapped house 64 fps vs 60 capped, smoother) | house 1% low 41 → 48 |
| F1 | pointer fast path: one vector load per LDG.64/.128 (`NXRT_HARNESS_GMEM_VECTOR`) | Village 36.6 → 40.4 fps (42.6 in the shared cache) |
| P4 | `cs_gmem_held`, `refill_compute`, `pass_pre_layout`, `occ_defer` | about +1.5-2 fps |
| C1 | compute pointer shaders take the page cache when no marked slot overlaps (`NXRT_HARNESS_GMEM_CS_SPAN`) | #3 5.63 → 4.90 ms; village 44.0 fps (shared cache) |
| R6 | `cmd_thread_desc`, `odb_program`, `pool_sync_fast`, `overlap_pos`, `handle_cache`, word hashes, layout memo, pool page hints | Village replay 21.9 → 20.2 ms; house +0.4 fps |
| V1 | `mem::ww::changes()` in the set-memo epoch: closes a race (stale set-memo hits 1 per run → 0) | correctness, 0 fps cost |
| R7 | `reflect_literals`: SPIR-V reflection had taken literal operands for written storage ids (37,479 of 47,409 modules wrongly "written") | replay −2 ms; first scene 58.5 → 61.3; house 76 → 78.9 |
| R7 | `memo_storage_check` | correctness for the newly reusable storage sets |
| P5 | `fill_ahead` (copy-ins of a run of dispatches under one barrier pair); `mesh_copy_ahead` (mesh-indirect record copies before the pass) | +0.75 fps (30/30), GPU −0.34 ms |
| G1 | `occ_batch`: ~1,450 occlusion reports per frame written with a few multi-region copies instead of ~2,900 tiny transfer commands | **Village 43.3 → 49.6 fps**, GPU −3.1 ms; first scene 62 → 65.8; house 78.4 → 82.8 |
| fps100 | `flush_copy`: device-local copies of the ~300 CPU_CACHED pools outside the heap's primary view (vertex/index data), kept current by the game's flushes | village GPU busy 18.8 → 14.8 ms, +2.75 fps (16/17), 1% low +2.9 |
| fps100 | `cmd_publish_batch` (recorder ring published every 32 packets); `render_priority = highest` for the render and recorder threads | replay −0.44 ms; +0.4 fps (13/17) |
| fps100 | `servable_cache` (out_servable answers keyed by change counters, verify clean); `keep_memo` (kept sets enter the epoch memo) | replay −0.84 ms (16/16), +2 fps; replay −0.11 ms, +0.4 fps |
| gpu-time (round 2) | `pt_copy`: pools whose pointers a pointer shader's bound storage buffers hold (light-probe grids and record tables of the foliage meshes, 11 pools) are copied whole into the device-local pool copy, and the guest page table points their valid pages there (2 MiB leaves only when the whole span qualifies). The fern mesh fbb731e2 (39k mesh invocations per frame) read those through the bus | **village +4.2 / +4.6 fps median** (20/23, 17/21), median frame −1.2 ms, main-submission GPU busy −1.4..−1.55 ms, game sync wait −1.2 ms |
| gpu-time | #3 compute re-translated in C1's form (fog 20593fd9, 24458020, beb1205f: runtime blocks dumped with `[nvn] dump_shader`) | stamped culling submission 4.32 → 4.09 ms |
| R8 | runtime release build `-march=x86-64-v3` + ThinLTO (`NXRT_FAST_CPU`, default on; no FMA contraction) | back-to-back exes: 64.8 / 65.1 vs 58.8 / 54.7 fps, replay ~15.1 vs ~17.3 ms |
| R8 | feedback copies keyed by the attachment image (`fb_image`): two 8 MiB images were made and destroyed every frame; 16k command-handle cache (`handle_big`); submission chunks and push descriptors on the recorder thread (`chunk_async`, `cmd_thread_pds`) | together, bias-free pairs: replay -1.29 ms (11/12), fps +2.05 |
| R8 | handle expressions once per command; virtual-texture registry indexed and grouped by pool (find_alias and CPU-write checks no longer walk every texture under the registry lock) | alternating exes: replay 12.9 / 13.4 vs 14.6 / 13.95 ms |
| R8 | unbiased cpuperf_ab; hot config switches read without guarded statics (`nvn_cfg.h`) | measurement; neutral |
| R8 | runtime PGO, opt-in (`NXRT_PGO = generate | <profdata>`; the profile is not in the repository) | replay ~12.3 vs ~14.8 ms; no fps while GPU-bound |
| R8 (shader side) | direct mesh outputs + real primitive count (`NXRT_HARNESS_MESH_DIRECT` / `MESH_EARLY_COUNT`) on by default; the shared cache's 785 mesh/task blocks re-translated (backup `work/shaders/mesh_backup_r8`). **The main checkout's harness must be rebuilt from the merged branch before any later re-translation**, or the mesh blocks revert | alternating private caches: 69.0 / 69.0 vs 66.7 / 66.6 fps, GPU -0.4..-0.5 ms; shared cache in game 69.2 fps, frames over 15 ms 45% -> 24% |
| gpu-time | `indirect_from_held`: indirect arguments inside one held result read from its copy (no whole 6.5 MB copy-back per indirect dispatch) | culling submission 3.31 → 3.08 ms (stamped), main submission and mesh invocations unchanged |
| gpu-time | `pt_copy` measured again without the frame trace, hazard off (the fps verdict) | **fps +2.7 median (25/25), 1% low +4.7 (24/25)** |
| R3 | `render_cpus = auto`: render and recorder threads pinned to two physical cores. First form: second and third fastest core (here 2, 4 = guest ideal processors). Final form (`nvn_cpu_pick.h`, unit-tested on synthetic topologies): fast cores only, none a guest ideal core, main thread's L3 first, recorder on a second free core or core 0's second thread, else no pinning (here 14, 1) | first form, hazard on: **fps +6.8 / +7.1 (24/24 twice)**, replay -1.5 / -1.9 ms; hazard off: replay -1.26 ms (27/34). Final form (14,1) AB +8.2 fps (21/23); absolute runs 80.7 / 81.0 vs first form 81.7 / 82.7 fps (within the run-to-run spread) |
| R3 | `hazard` on by default (B1's tracker on the recorder; its submission start now reaches the recorder through the ring instead of a drain) | under pinning: **fps +6.2 (24/24), 1% low +5.1**, median frame -1.25 ms; village no-AB run 82.6 fps, 1% low 60.3, 1.5% of frames over 15 ms |
| R3 | `r3a`: texture-only resolves without copying `TexResolved` (~150 bytes, up to 6 copies per binding), chunk check fast path, gmem pointer table without a 4 KiB clear per op, 4 recent pools in `physical_buffer_address` | replay -0.45 ms (22/25), fps +1.6 |
| R3 | `dset_recycle`: descriptor sets of a frame slot go back to per-layout lists when its submission completed (no `vkAllocateDescriptorSets` for ~800 sets per frame) | replay -0.34 ms (22/25), 1% low +1.45 |
| R3 | `cmd_thread_copy_image`: `vkCmdCopyImage` and image clears queued for the recorder (~11 drains per frame fewer) | replay -0.34 ms (17/22), fps +0.5 |
| R4 | `tex_share_index`: images of pool textures indexed by storage and layout while they have owners; a new object over the same storage shares the image of one finalized (or dropped) before its release ran | removes the texture-churn windows (38-62 fps for minutes); paired fps +0.55 median (17/24), 1% low +3.6; steady images made 0.2-1.2 -> 0.00 per frame |
| R4 | `keep_chain`: kept descriptor sets remember their successor per set index (~78% right), take it without the hash table and prefetch it | replay -0.08 / -0.11 ms (17/23, 22/23), fps +0.15 / +0.40 |
| R4 | `hot_skip`: uniform blocks over hot pages resolve to the pool until the hold ends; their kept-set checks skip the page checks | replay -0.04 / -0.10 ms (14/23, 21/24), fps +0.05 / +0.37 |
| R5 | `front_plan`: submissions walked ahead on `nvn-front` (called lists, physical addresses, per-draw layouts and set-signature hashes as a stream the replay reads with one cursor); list frees off the render thread; occlusion-flush and prepare_textures scratch kept across calls | **fps +1.92 (47/48)**, replay -0.65 ms, 1% low +0.48 (no frame trace) |
| R6 | `ubo_dynamic`: set 0's uniform blocks as dynamic uniform buffers, sets by buffers, per-address copy-or-pool memo; the hazard tracker applies dynamic offsets | replay -0.60 ms (45/45), fps +1.4 (45/45); village 87.5 -> ~90 fps |
| R6 | `tex_img_sets` + `tex_img_early`: texture sets looked up by image (the game makes new texture objects every frame for the same images) | replay -0.64 ms (44/45), fps +1.2 (39/45) |
| R3 | `occ_mark_late`: samples-passed reports' GPU writes noted at the flush; per-set binding index lists (`ProgLayout::set_idx`) | replay -0.10 ms (17/23); index lists not paired (always on) |

Shared shader cache, `work/shaders/oot/spirv` (git-ignored):
- F1's set: 3,419 gpt corpus blocks plus 12 runtime blocks; backup `gpt_backup_v9`.
- C1's set: 550 compute gpt blocks plus 12 runtime blocks; backup `gpt_backup_v10`.
- 2026-11-12 (gpu-time): 20593fd9, 24458020, beb1205f re-translated in C1's form (backup `gpt_backup_v12`). The other compute gpt
  runtime blocks without a dump are still in the old form. Dump them with `[nvn] dump_shader`.
- **A full shader preparation is due**, as the last step.

## What we tried and dropped (measured)

- **Barriers.**
  - Scoped stage masks (`dispatch_chain`, `xfer_scope`): neutral.
  - B1's range-based hazard tracker (`[nvn] hazard`, kept in the code, default off): full barriers 908 → ~300 per frame, GPU −0.8 ms,
    but render thread +3.2 ms, so −3.8 fps.
  - The ~300 barriers left are real dependencies.
  - Removing all barriers (measurement only) would save 3.85 ms.
- **Unified image layouts** (`[gfx] unified_layouts`, P5, default off): neutral. Most layout pass breaks are real dependencies.
- **Shader side, fewer registers did not pay:**
  - propagation limit `NXRT_HARNESS_PROPAGATE=near:16` (fbb731e2 255 → 174 registers, no fps);
  - direct mesh outputs plus an early primitive count;
  - branch-free select fast path;
  - fast path regardless of mask;
  - `fs_contract`.
  - Most hot gpt blocks use ~253 registers in the console's own SASS too.
- **Compute vector loads** (`NXRT_HARNESS_GMEM_CS_VECTOR`): ~175 replay pixels changed; cause open; off.
- **Render-thread structure (R7):**
  - **(a) static-list translation cache:** static lists hold only 345 of 2,200 draws. E1's "70%" was a misreading.
  - **(c) parallel recording:** the coherence state is sequential per draw.
  - **(b) sets built ahead:** only ~1.5 ms is movable, and it needs the same state.
  - `alloc_batch` (R6): +0.3 ms.
  - `cmd_thread_copies`: mixed.
  - Time-based submit chunks.
  - `memo_write_scope`.
  - `odb_clean`.
  - VMA pools for images.
- **Copies:**
  - splitting the culling buffers or aging CPU-read marks: −10 / −14 fps;
  - `mirror_refill_written` for all callers: −10 fps;
  - `cs_gmem_flush`;
  - `write_merge`;
  - `uil_pending_barrier`;
  - `pass_lookahead`: neutral.
- **Device-local copies of the ~300 MapPhysicalMemory pools** (0x60_xxxx_xxxx): an earlier try at 480p was neutral. At 1080 G1
  measured a ceiling of ~1.1 ms, but a CPU-side copy costs +5.5 ms render.
- **Ruled out by measurement:**
  - pixel count;
  - fragment overdraw / early-Z;
  - the shading-rate image (0.2 ms);
  - render-target storage usage;
  - VRS state sets;
  - uniform blocks in host memory;
  - robustness (already off);
  - FP16 (no gain on Ada).
- **Kernel / game side:** WaitOnAddress instead of SRW condition variables (worse), wake spin (no fps).
- **fps100 (2026-11-11), village outside paired windows:** `sig_tex` (kept sets keyed by image instead of handle: replay +0.4 ms, off);
  a separate memo for texture-free sets (`memo_nt`: replay +0.13 ms, removed); a per-slot memo of `physical_buffer_address` (noise,
  removed); `compute_out_lazy = 0` with the GPU headroom flush_copy gave (GPU +6 ms, 47 fps); mesh shaders with direct outputs and the
  early primitive count (`NXRT_HARNESS_MESH_DIRECT` + `MESH_EARLY_COUNT`, private cache): GPU busy −0.14 ms only.

- **R8 shader side (2026-11-12, GPU-bound village):** `NXRT_HARNESS_PROPAGATE=near:16` as a mesh default (hot meshes 255 registers with
  spills -> ~170): 69.3 / 69.4 vs 69.1 / 68.9 fps, noise; on the 14 hottest compute blocks (HiZ a17259b2 255 -> 128 registers): 65.6 / 68.6
  vs 68.8 / 69.3, worse. gpt draws without fragments (`skip_gpt = 2`): GPU -0.17 ms only (their cost is the pre-raster stage); gpt draws
  left out (`skip_gpt = 1`): +11.3 fps, GPU -2.3 ms (the ceiling of the pointer pre-raster stages).
- **R8, hazard tracker (now on the recorder thread) at 5ff8131, paired, two runs:** GPU busy -1.1 ms per frame, but render replay +0.7
  ms and fps -2.8 / -1.25 (0/15, 1/15): with it on the render thread becomes the limit (idle 1.2 -> 0.1-0.3 ms). Stays off; it pays once
  the render thread has ~1 ms more slack.
- **R8 (2026-11-12):** `park_shared` (the last reference of a shared image parks it): replay +0.14 ms, 1% low -2.3 (4/12), removed.
  The earlier paired gains of `handle_big` / `chunk_async` / `fb_image` alone were biased (see How to measure); re-measured together.
- **gpu-time round 2 (2026-11-12), village 1080, paired windows:**
  - `hazard` moved onto the recorder thread (pass barrier by a look-ahead through the queued commands; also fixed: the old tracker read
    descriptor summaries before the queued `cmd_thread_desc` updates). GPU busy −1.1 ms (22/23), barriers 615 → 177 per frame, but
    without the frame trace the frame is render-thread bound and it cost fps −1.9 (replay +0.46 ms); without the look-ahead (barrier at
    every pass with anything unordered) −3.6. Stays off; turn it on when the GPU is the limit.
  - `exp_ssbo_copy` (read-only storage buffers of pointer draws bound from the pool copy and their slots marked): GPU busy +0.6 ms (the
    slot search costs more than the bus). Removed.
  - `gmem_scan_memo` (skip pointer-shader uniform scans when nothing changed): neutral (8/26), removed.
  - pt_copy that split 2 MiB leaves into page leaves: HiZ a17259b2 0.24 → 0.48 ms (a walk per 4 KiB page); now spans stay whole.

- **R3 (2026-11-12), village paired windows, no frame trace:**
  - `submit_async` (the end of each submission queued for the recorder) + `cmd_nodrain_begin`: replay -0.31 ms (22/24, 22/23) but fps
    +0.4 / +0.0 and 1% low -0.8 / -2.5: built, off.
  - `cmd_prefetch` (software prefetch of the command records): replay +0.10 ms (9/23), removed.
  - `keep_tex_keyed` (first-sighting texture sets looked up by image): replay +0.45 ms (4/27); only 10% found although `r3_diag` saw 96%
    of them by image in an earlier present (most such sets are not keepable: feedback / scaled-scene bindings). Built, off.
  - Profile notes: the render thread is flat (run_list's own loop ~11%, begin_draw ~5.6%, keep machinery ~8%, texture resolution ~7%,
    no function over ~3% else). Set statistics per frame (`graphics sets by index`): set 0 (uniform blocks) ~1,750 kept-set checks and
    almost no memo hits (per-object constant addresses repeat across frames, not within one); set 2 (textures) ~370 first sightings.

## GPU round 3 (G3, 2026-11-12, branch `perf/gpu-r3`)

Village 1080 uncapped, real clocks (skip-word windows of the frame trace; main = the big submission, busy = without starve).
Start: ~76 fps no trace, 1% low ~60; main busy ~10.2 ms + culling submission #3 ~3.4 ms per frame.
- **Ablations** (main busy per frame): all full barriers −4.9 ms, but ~3.7 of it is the mesh-indirect copy barriers (`bar:6335,6337`):
  without them the records race and the mesh draws get less work. Every other barrier site alone −0.1..−0.3. gpt draws −3.95 ms;
  by draw: grass prepass mesh f3054ac2 −1.2 (one draw), grass colour mesh 6ec65625 −1.2 (one draw), fern fbb731e2 −0.45; the rest of
  the gpt meshes ~0. Stamped order of one main submission: the non-gpt meshes 4b9e2d28 (~0.45) and 7477b2c8 (shadows, ~0.7) are next.
- **New tool: the driver's own machine code.** `pipestat` with `PIPESTAT_CACHE=<file>` writes the pipeline cache data; it holds a zstd
  frame (the first `28 b5 2f fd`) whose payload contains the SM89 code; `work/g3/cachesass.py` (git-ignored scratch) finds the EXIT word
  and disassembles with nvdisasm `-b SM89`. Counts against the console's SASS: f3054ac2 3,856 vs 1,776 instructions (its 3 pointer loads
  are not the cause: selects/compares of FTZ and NaN handling, shuffle calls, if-converted switches), 6ec65625 12,156 vs 2,760,
  fbb731e2 9,626 vs 1,417, 4b9e2d28 874 vs 308, 7477b2c8 1,758 vs 473.
- **Kept: task/mesh pointer loads vectorized again** (`NXRT_HARNESS_GMEM_MESH_FIX`, default on). Task and mesh blocks translate as
  compute work groups, so C1's compute-only switch had turned F1's vector loads off for every mesh block (fbb731e2: 264 32-bit loads
  instead of 66 LDG.128) and they read compute's slot-mask word (word 0 of a graphics c[0], a driver constant) instead of their own.
  187 gpt mesh/task blocks change. 4 alternating pairs: main busy 10.23 → 10.07 ms (4/4).
- **Kept: mesh index map one u32 per byte** (`NXRT_HARNESS_MESH_MAP_BYTES`, default on): the 8-bit index writes were atomic
  read-modify-write loops (the driver keeps a mesh shader's shared memory in its output buffer and turns the atomics into MATCH.ANY
  loops); driver code −5..−10% on meshes; on top of the fix above 4 more pairs: main busy −0.22 ms (3/4). Both together ~10.2 → ~9.8 ms.
  **Shared cache updated** (all 785 mesh/task corpus blocks, 778 changed; backup `work/shaders/mesh_backup_g3`), byte-identical to the
  private cache that passed the checks: A18 on the nb3 capture (25 mesh draws, x sum 4,398 in every traced loop), village capture replay
  SSIM against its reference the same as the old cache (0.49285), in-game village screenshots normal (foliage, crowns, fog, no stripes).
  The main checkout's harness must be rebuilt from the merged branch before any later re-translation, or the mesh blocks revert.
- **Measured, opt-in:** `NXRT_HARNESS_FTZ_FOLD=1` (flushes that cannot change a compare against a nonzero immediate or a truncation): −1.5% instructions,
  too small to re-translate every block for.
- **Dropped:** `out_split_held` (split results held from an earlier submission, the culling tail): main busy +0.49 ms, fps −2.4 (0/19);
  `[nvn] dispatch_trust` (the barrier after a dispatch owed and paid by the next call a plain dispatch does not make; the game's own
  barriers order its dispatches): −58 barriers per frame, busy −0.07 ms (18/19): the ~90 tiny 4-5 µs dispatches of #3 are not drain
  bound; code kept, default off (cpuperf_ab name g3b). `NXRT_HARNESS_GMEM_UMISS` (uniform miss branch by a subgroup vote): the driver's
  compiler crashes (access violation) on it, with OpGroupNonUniformAny and with a ballot. `NXRT_HARNESS_INLINE_HELPERS`: no change (the
  calls in the code are the driver's own WARPSYNC/SHFL subroutines).
- **Second-queue overlap of #3:** #3(N+1) is replayed ~1.8 ms before #6(N) ends on the GPU and then waits behind it, so that is the
  overlap ceiling. A two-frame trace with written storage bindings marked (`[nvn] trace_frame`: submissions, ` W` on written storage
  bindings, pointer write windows) shows real write-after-read conflicts: #3(N+1) writes (1) the 720 KB material table at fragment storage
  slot 3 (one address in both frames) that every material fragment shader of #6(N) reads, up to its last colour pass, and (2) the
  16 KiB frame-globals buffers (compute storage slot 8; two of them, both written each frame) that #6's draws bind as uniform block 0
  (snapshotted at record time) and fragment storage slot 10. Overlap would need the runtime to double-buffer those game buffers (copy
  for #6's readers before #3 runs), plus a second timeline per queue. Not built; ceiling ~1.8 ms, realistic less (both share the SMs).

## GPU round 4 (G4, 2026-11-12, branch `perf/gpu-r4`)

Village 1080 uncapped. The frame is render-thread bound (replay ~11.5-12 ms per frame = the frame; GPU busy ~8.5-9 ms by the frame
trace's `gpu.busy`), so a GPU change is judged by GPU busy *and* render-thread replay in paired windows.
- **The hot grass meshes are not code-bound.** Empty-program test (G4's measurement switches: `NXRT_HARNESS_MESH_EXP=empty` makes a mesh
  program only its EXIT, `=zeroprims` emits no primitive, `NXRT_HARNESS_EXP_EMPTY=1` the same for other stages; each translated into a private
  cache, then skip windows `v:<hash>` / `c:<hash>` in the same run): leaving out grass f3054ac2 or 6ec65625 saves 1.19-1.23 ms of GPU busy
  each, and **exactly as much with their program empty** (and with zero primitives); GPU exec drops only 0.24-0.58 ms and starve rises by
  ~1 ms. So the 1.2 ms each is the GPU around these indirect mesh draws (the record copies and their barriers, pass breaks), not their code.
  The shader code's own share of GPU busy (full minus empty): fern fbb731e2 ~0.44 ms, mesh 4b9e2d28 ~0.24 ms, shadow mesh 7477b2c8 ~0,
  HiZ a17259b2 ~0, fog 20593fd9 ~0.
- **Kept: `[nvn] mesh_ahead_mem` + `mesh_ahead_pass`** (default on, `cpuperf_ab_what` name `mesh_mem`; CONFIG.md). The `{count, first}` ->
  `{count, 1, 1}` record copies of the mesh-indirect draws used to happen ahead of their pass only until the look-ahead met an occlusion
  report, a uniform update, a barrier or a target change; ~16 draws per frame still ended their pass and copied between two full barriers.
  The look-ahead now goes on over commands with a known write range (checked against the records), barriers and events, and across passes
  (target changes, clears, discards, texture copies), and does not scan again over a range it found free of mesh draws. Paired windows (2
  runs): **fps +2.3 / +1.7 median (19/19, 20/20)**, median frame -0.27 / -0.29 ms, GPU busy -0.53 / -0.52 ms, full barriers -34 per frame,
  render replay +0.12 / +0.13 ms (without the re-scan memo: +0.45). A18 on the nb3 capture: 25 mesh draws, x sum 4,398 in every loop; village
  capture replay SSIM 0.49282 (off 0.49288, G3's 0.49285). About 11 draws per frame still copy in their pass (the look-ahead ends at writing
  dispatches and other commands; statistics line `mesh_copy_ahead look-ahead ended by:`). With signal events allowed too (final form),
  2 more runs: fps +1.75 / +2.25 median (21/21 both), GPU busy -0.52 ms, replay +0.09 / +0.12 ms; A18 again 4,398 in every loop, SSIM 0.49288.
  Then (look-ahead budget 4,096 -> 16,384 commands, and a draw takes the early copy of the same records also when they now resolve to another
  buffer: a held result sent back at a submission chunk in between, same contents): in-pass copies ~4.8 -> ~1.2 per frame (left: draws past
  where a look-ahead ended at a writing dispatch), 2 runs: fps +2.2 / +1.85 median (19/20, 19/19), GPU busy -0.54 ms, full barriers -39 per frame,
  replay +0.13 / +0.18 ms. The statistics line `mesh_copy_ahead look-ahead ended by:` now also splits the in-pass copies by reason.
- **Look-ahead cost on the render thread** (sampler, village): ~25 look-aheads per present read ~13,500 commands (statistics line), ~2.2%
  of the render thread's samples, 43% of it the handle look-ups of called lists. Cut: the look-ahead's handle look-ups no longer count calls,
  storage bindings are resolved to physical addresses only when a writing draw needs them, a memo of the last stage set's write flag:
  2.23% -> 2.08% (physical_buffer_address gone from it); paired replay cost +0.10 / +0.15 ms (before +0.13 / +0.18: within noise).
- **The heavy frame of the game's 4-frame cycle** (frame trace, all features on, 40-200 s): GPU busy per frame median 8.1 ms, p90 9.19,
  p99 9.29, max 10.0; by cycle position busy 9.17 / 9.10 / 7.58 / 7.54 ms while the frame is 11.1 / 13.0 / 12.3 / 12.0 ms and the render
  replay 10.8 / 12.3 / 12.0 / 11.5 ms. No frame's GPU work is over ~9.5 ms (with the trace on); the heavy frames are the render thread's.- **Translator experiments, opt-in only (measured no GPU-time gain):** sin/cos fusion (`NXRT_HARNESS_SIN_FUSE=1`: MUFU.SIN/COS of an
  `FMUL.RZ x, 1/(2pi)` emitted as sin(x), which the driver compiles into the same FMUL.RZ + MUFU pair; f3054ac2 -7% host instructions),
  shifted mesh store addresses proven (`NXRT_HARNESS_MESH_PROOF_SHIFT=1`: shadow mesh 7477b2c8 40 stores, 1,590 -> 938 instructions),
  predicated instructions run unconditionally where the old value is dead on the guard-false paths (`NXRT_HARNESS_SPECULATE=1`: proves few
  in the hot meshes; the ceiling, measured by dropping all predication, was ~620 of f3054ac2's ~3,400). In game (private cache with the first
  two, 2 pairs): main submission exec 9.79 / 9.99 vs 9.77 / 9.92 ms, noise.
- **Tool fix:** `pipestat` enabled every available feature, robustBufferAccess included (the runtime turns robustness off), so earlier
  counts included bounds checks on descriptor loads; now off (`PIPESTAT_ROBUST=1` for the old behaviour). Driver-code dumps: a SPIR-V id
  bound salt defeats the driver's disk cache (scratch `work/g4/drvsass.py`, not committed). Main-body counts vs the console's SASS
  (robustness off): f3054ac2 3,388 vs 1,776, 6ec65625 10,323 vs 2,760, fbb731e2 8,336 vs 1,417, 7477b2c8 1,590 vs 473.
- `hwcheck --sin-pairs` (two-instruction FMUL.RZ + MUFU.SIN/COS cases from the game corpus): the CUDA launch of the two-word kernel fails
  (CUDA_ERROR_INVALID_VALUE, single MUFU cases pass); not resolved, the fusion stays opt-in.

- **R5 (2026-11-12):** `pool_seqlock` (texture/sampler pool entries read with sequence counts instead of the shared locks): replay -0.05 ms
  (28/41), fps +0.1, 1% low -0.65: removed. A record-keyed hash map for the plan's look-ups (first form of `front_plan`): its look-ups cost
  as much as `physical_buffer_address` did; replaced by the sequential stream.
- **R4 (2026-11-12), village paired windows:**
  - `mirror_hot_window = 8` (pages rewritten every fourth frame faulting every cycle): faults per frame unchanged (6.8), fps +0.3: removed.
  - `memo_skip` (bypass the set memo for set indices with few hits): at < 2% it never skipped (set 0 hits ~2.1%); at < 10% it skipped 7 M
    look-ups per run for replay +0.01 / -0.03 ms: removed.
  - `pipe_memo` (pipeline per program layout, state content hashes and pass formats, 4,096 slots): 45-47% hits, replay -0.02 / -0.10 ms:
    removed. Most draws change the program to one with another state combination.
  - `keep_tex_keyed` again (after tex_share): 7% of first sightings found, failed texture checks; set 2 kept checks 0.16 -> 0.52 ms: stays off.
  - `ban_skip` (ranges of banned pages): 0 banned uniform pages in the village: removed. A persistent content-keyed texture-set cache: not built
    (set 2 builds spend 0.57 of 0.69 ms resolving, the cache could save <= 0.1 ms).

## What is open, by expected gain

**After R6:** replay ~9.7-10 ms; ~88-90 fps. The render thread, the GPU (~93% busy) and the game's CPU frame are co-limits; a replay cut now
converts into fps at less than half its size. Render-thread items left (exp_op_cost per present): set 0 dynamic path 0.73 ms, texture
preparation 0.70, out_before_op 0.50, set 1 0.41, pipeline key/look-up 0.37, vertex buffers 0.27, dynamic state 0.27, the per-command loop
(~11% of the render thread's samples), recorder drains at each chunk submit (~7 per frame). GPU: the culling submission's tail copy-backs
(0.39 ms) and the main submission's draws. Game side: the heavy frames of the 4-frame cycle (G).

Parallel pass recording (R6 design note, not built)

Ceiling: the big submission is ~9.2 of ~10.2 ms of replay per frame; ~2,200 draws in ~55 passes. Split across two threads the
draw work of the long passes (the main colour, depth prepass and shadow passes hold most draws) could save up to ~3-4 ms of
replay. But since R6 the frame is co-limited by the GPU (cpuwatch GPU load 91-95% at ~89 fps) and by the game's own CPU frame
(main thread G p50 8.7 ms with the frame trace, 4-frame cycle with render idle 2+ ms in the heavy frames): a replay cut converts
into fps at less than half its size (R6 pairs: replay -0.64 ms -> median frame -0.10 ms). The parallel replay's fps ceiling at
the current GPU load is therefore ~+3-5 fps, not +30.

State a draw touches, by kind (from the replay code):
- per-command state (GState S, programs, bindings): reproducible per worker from the front plan's walk (nvn-front already
  carries it across submissions);
- descriptor caches (memo, kept sets, dyn0/img caches, per-frame descriptor pools): per worker or sharded;
- coherence state with commutative per-draw effects inside one unbroken pass: read_dep_check's used/want page bits
  (plain |= on shared vectors today: would need atomics);
- coherence state with ordered effects: held results (out_draw_read / out_before_op may copy back and end the pass), storage
  writes of draws (gpu_wrote, g_pass_unsafe), texture layout transitions (prepare_textures ends the pass), occlusion report
  bookkeeping, mesh-indirect record copies, hazard tracking (recorder side, one command buffer). A worker would run a chunk
  speculatively and abort to the sequential path on the first draw that needs any of these.
- recording: today every vkCmd goes through one recorder ring; workers would record secondary command buffers directly
  (VK_KHR_dynamic_rendering inheritance) and the hazard tracker would need per-buffer state.
Effort: several days, most of it making ~100 replay globals per-thread. Not started; the GPU and the game's CPU frame are the
nearer limits now.

**After R5:** replay ~10.5 ms with `front_plan`; render idle ~0.9 ms; game GPU sync ~4 ms. What is left on the render thread is the
coherence-tied part (kept-set checks ~1 ms, set builds ~1.2 ms for ~1,000 first-sighting / written-storage sets per frame, held-result
checks, texture preparation) and the per-command loop (~1 ms for ~35k commands). Moving more to the front thread was measured or judged
small (texture handles: storage-held; set signatures: no gain; pipeline keys depend on attachment decisions). The remaining routes to
-3 ms are the ones R4 named and R5 did not build: parallel recording of passes into secondary command buffers with per-thread replay
state (a multi-day refactor of the replay's globals), dynamic uniform offsets for set 0 (estimated -0.8..-1.0 ms; changes every pipeline
layout, so only together with the final shader preparation), and push-based validity for kept sets.


**After R4:** the render thread (~11-11.8 ms replay, idle ~0.1 ms) is the only limit for 100 fps; the GPU is at ~8.5-8.9 ms. Per-draw work
is ~3.2 us over ~2,100 draws plus ~3 ms of per-op and submission work, spread over dozens of items of 0.1-0.8 ms each. Ways to the ~2.5 ms
still needed, all structural: (1) a front-end thread that decodes the lists and does the coherence-free work one submission ahead (texture
resolves, pipeline keys and look-ups, program layouts, set signatures, handle look-ups: ~20% of the render thread's self time by the sampler)
and hands the render thread compact draw records; (2) push-based validity for kept sets (per-page or per-64-page versions bumped by write
faults, copies, GPU writes and fills, so a kept set is checked by comparing a few counters instead of the page bits); (3) uniform blocks by
dynamic offset (removes ~2,000 set-0 binds per frame) only together with the final shader preparation, since the pipeline layouts change.
Off the render thread: write-watch arming (~0.3 ms per frame of VirtualProtect in mirror_begin_submit / out_end_submit) could move to a
worker only with an ordering proof against concurrent invalidates.


**After R3:** the village at ~82 fps is GPU-bound with hazard and pinning (render thread idle ~0.8 ms per frame with hazard on). For 100 fps
the GPU must lose ~2 ms per frame (G3's work) and the render thread ~1.5 ms (replay ~11.3 ms with hazard on). Render-thread leads left:
the per-object uniform sets (set 0: ~1,750 kept-set checks per frame, mostly cache misses on the kept-set table and its dependencies),
first-sighting texture sets (~370 per frame), compute sets (~250 built per frame, never reused), the per-op loop for ~35k commands per frame.
Pinning the game's main thread (G ~10 ms) has not been tried. Long frames left in the final village runs: one burst at ~65 s after the load
(31 new textures, ~5 MB uploaded on the render thread: 93 ms + 50 ms frames, once per run); otherwise 0-3 frames over 20 ms per 2 minutes.
The share of frames over 15 ms still varies between runs (0.7% / 12.4% in two back-to-back runs of the same exe).


**After R8:** the village is GPU-bound (~14.5 ms of GPU per frame); the render thread (~13 ms, ~11-12 with PGO) has ~2 ms of slack.
Render-thread items left by the profile: bind_sets ~23% (build_set ~10, kept-set checks ~7), run_list's own loop ~8%, write-watch
arming (VirtualProtect) ~3.6% (out_end_submit re-arms written spans), texture resolution ~5%, held-result checks ~6%.

**fps100's design for the next step (render thread 17.4 -> ~9 ms; not built):**
1. Draw-level translation cache keyed by program + raw bindings + targets, valid while one coherence epoch holds: skips bind_sets,
   prepare_textures and out_before_op (~6.4 ms per frame if everything hits).
2. That needs scoped invalidation first: the set-memo epoch moves ~29 times per frame today (resolve epoch ~9, texture image events
   ~17, results sent back ~3); per-resource versions (held-result ranges, TexGpu images, pool page runs) instead of one global epoch.
   A first step (`memo_nt`, a texture-free memo) did not pay: the uniform and storage sets are kept sets, not memo misses.
3. Resolve-ahead thread (like tex_lookahead): decode, handle lookups, pipeline keys and set signatures one submission ahead (~3.5 ms).
4. Parallel recording of passes into secondary command buffers: the only route that halves the replay, but texture layouts and the
   held-result state are sequential per draw; a pre-pass would have to plan layouts.
GPU side after that (village busy 14.8 ms): the three gpt foliage mesh draws ~3.3 ms, barriers ~2.9 ms (B1's tracker gave -0.8 ms
of it but cost the render thread +3.2 ms: running it on the recorder thread would keep the GPU gain), culling submission #3 ~4.2 ms.

1. **gpt pointer pre-raster stages** (~6.9 ms before F1, now less).
   - The draws' own work is ~9 ms per frame in total.
   - Ideas:
     - page-table entries for held results pointing into the pool copy (P4's design in AGENTS.md), so slot masks can be 0;
     - pointer tables as storage bindings;
     - why fbb731e2 / 6ec65625 need ~180 registers vs 126 in the SASS.
2. **Render thread to ≤ 15 ms (now ~18.5).**
   - About 700 never-seen descriptor sets per frame. They come from per-frame buffer addresses, and dynamic offsets would need new
     pipeline layouts (every pipeline rebuilds).
   - Held-result re-checks ~9%, texture preparation ~8%, dispatches ~7%.
   - The driver itself takes ~20%.
3. **Index and vertex data over PCIe** (~1.1 ms): device-local copies of the alias-region pools, tracked through flushes, not
   CPU copies on the render thread.
4. **#3 culling submission** (~4.9 ms; main waits on it):
   - 7 copy-backs before indirect dispatches (0.36 ms);
   - the fog group;
   - HiZ.
5. Async compute (fog / post-process against the shadow pass): ~0.5-1 ms, high effort.
6. Nsight Graphics would settle the remaining per-draw cost. It needs a login at developer.nvidia.com, so the user would install it
   (`winget search Nsight`).

## Orchestration (how this work is run)

- One orchestrator session assigns work to agents in git worktrees. Each agent gets:
  - one item;
  - the rules: game lock, named instance, below-normal builds with at most 6 jobs, no shared-cache writes (private caches via
    `tools/shaders/scripts/private_cache.py`);
  - its own `AbFeat` bit range.

  Each agent commits on its branch. The orchestrator merges into `feat/perf-generic-runtime`, rebuilds, runs the 174 tests, then
  fetches and merges origin and pushes. The user opens and merges PRs into `oot-nup`; direct pushes there are refused.
- Changes to the shared shader cache go through a merge agent:
  - rebuild the harness;
  - `retranslate_subset.py --token gpt` (or `--hashes`) with a backup folder;
  - single-mode translation of the runtime-group blocks in `work/ai_run/oot/a18dump/dump`, never the upscaler rows (A22 merge
    pitfall);
  - an in-game check at 1080.
- **Commit hygiene:**
  - commit messages end with the Co-Authored-By line;
  - before every commit, a PowerShell forbidden-word check over the diff and the message must count 0;
  - never commit game data, SPIR-V, captures, screenshots or profiles;
  - edit sources with the Edit tool or Python, never PowerShell 5.1 Get-Content/Set-Content, which corrupts UTF-8.
- Agents of this round:

  | Agent | Topic |
  |---|---|
  | K1 | roadmap |
  | W1 | web research (notes in `work/notes/w1_web_research.md`) |
  | F1 / F2 | gpt shaders |
  | P4 / P5 | our copies |
  | B1 | barriers |
  | C1 | compute pointer shaders |
  | R6 / R7 | render thread |
  | V1 | memo race |
  | M1 | cache merge |
  | G1 | draw anatomy |
  | P2 | coherence v2 gate (`exp_coh_free`) |

  All of them are finished; no agent is running at the time of writing.
