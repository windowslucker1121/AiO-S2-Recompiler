# The game performance: state, history, what we tried, what is running

Living document for the frame-rate work. Detailed per-session notes, with every key, measurement and pitfall, stay in
`AGENTS.md` ("Performance work" and the "Session 2026-11-xx" paragraphs). Config keys are in `runtime/docs/CONFIG.md`.
This file is the overview: where we are, how we measure, what paid, what did not, and what is next.
Last update: 2026-11-12, branch `perf/gpu-r4` (G4).

## Goal and constraints (from the user)

- **A real, steady 60 fps in the village** (`load_state("village_outside")`), the heaviest scene, at the **pinned render
  resolution 1920x1080** (`[oot] render_height = 1080`).
- No DLSS, FSR, frame generation, lower resolution or skipped game rendering. The game's own post-process chain (including its
  upscaler network) stays. Resolution changes only via DLSS-like tech later, and only if the user asks.
- Beyond 60 is welcome. Measure everything and keep only what pays.
- The full shader preparation (~3 h) runs only at the very end. So do the release build (`oot.exe`, `recomp_library.exe`) and the
  area-load frame work.

## Where we are (2026-11-12, RTX 4080, Ryzen 7 5800X3D, 1920x1080, uncapped)

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
  `occ_mark_late`, `copy_image_q`, `tex_keyed`, `submit_async` and `r3a`; the cmd_copies bit went to G3's `g3b`.
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
- **Translator experiments, opt-in only (measured no GPU-time gain):** sin/cos fusion (`NXRT_HARNESS_SIN_FUSE=1`: MUFU.SIN/COS of an
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

## What is open, by expected gain

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

  All of them are finished; no agent is running at the time of writing.
