# The game performance: state, history, what we tried, what is running

Living document for the frame-rate work. Detailed per-session notes, with every key, measurement and pitfall, stay in
`AGENTS.md` ("Performance work" and the "Session 2026-11-xx" paragraphs). Config keys are in `runtime/docs/CONFIG.md`.
This file is the overview: where we are, how we measure, what paid, what did not, and what is next.
Last update: 2026-11-11, branch `perf/fps100`.

## Goal and constraints (from the user)

- **A real, steady 60 fps in the village** (`load_state("village_outside")`), the heaviest scene, at the **pinned render
  resolution 1920x1080** (`[oot] render_height = 1080`).
- No DLSS, FSR, frame generation, lower resolution or skipped game rendering. The game's own post-process chain (including its
  upscaler network) stays. Resolution changes only via DLSS-like tech later, and only if the user asks.
- Beyond 60 is welcome. Measure everything and keep only what pays.
- The full shader preparation (~3 h) runs only at the very end. So do the release build (`oot.exe`, `recomp_library.exe`) and the
  area-load frame work.

## Where we are (2026-11-11, RTX 4080, Ryzen 7 5800X3D, 1920x1080, uncapped)

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
- **Feature switches:** `AbFeat` in `nvn_replay.cpp` is a uint64 bit set. Used bits: 0-36, 37-39 (G1), 40-42 (P5), 48-49 (G1), 50-56,
  58. Free: 43-47, 57, 59-63.
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

## What is open, by expected gain

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
