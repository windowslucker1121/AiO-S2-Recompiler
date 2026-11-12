# tools/shaders: NVN2 shader to SPIR-V translation

Offline and on-demand translator that turns the game's shader machine code (Ampere SASS for platform 2, Maxwell for platform 1) into
SPIR-V: Ryujinx's `Ryujinx.Graphics.Shader` (built from `refs
yubing` plus the `.patch` files of `harness/translator/`) with an
Ampere frontend (`ampere/`). The harness writes `results.tsv`, the binding contract the runtime reads.

## Layout

| Path | What |
|---|---|
| `harness/` | C# console `spirv_harness` (single stage, `--batch`, `--serve`), `StaticGpuAccessor.cs` (answers the translator's GPU queries), `translator/` (csproj that builds Ryujinx's translator from staged sources plus the `.patch` files) |
| `ampere/` | SASS decoder, frontend (Ampere instructions into Ryujinx's IR), nvdisasm probes and oracle; `docs/AMPERE.md` |
| `scripts/` | `setup_ryujinx.ps1` (stage Ryujinx sources), `apply_patch.py`, corpus extraction (`extract_generic.py`, `bnsh_programs.py`), cache tools (`merge_results.py`, `retranslate_subset.py`, `private_cache.py`) |
| `hwcheck/`, `pipestat/`, `xcheck/` | GPU oracle per instruction, driver statistics of a module, NVN2 layout cross-check |
| `licenses/` | licence texts of the borrowed code |
| `third_party/`, `build/` | git-ignored: staged Ryujinx sources, build output |

## Build

```powershell
powershell -File tools\shaders\scripts\setup_ryujinx.ps1          # copies 3 projects from refs\ryubing (never builds in refs\)
dotnet build tools\shaders\harness\SpirvHarness.csproj -c Release   # first restore ~2 min (an unreachable NuGet feed times out), then seconds
```

Needs .NET SDK 10 and the project venv (`tools\.venv`) for the scripts; the Vulkan SDK's `spirv-val` is optional (`--no-val`).

## Use

Hardware oracle per ALU instruction (GPU vs our translation): `dotnet run --project tools\shaders\hwcheck -c Release`, see `hwcheck/README.md`.

Input is a folder of extracted blocks in SuperMarioWonderRecomp's layout: `manifest.tsv` (columns `id stage code_off code_size ctrl_off
ctrl_size program_start program_bytes const_off const_size code_sha1_16 code_file ctrl_file`), `code_<hash>.bin` (the code
block, SPH at `program_start`) and `ctrl_<hash>.bin` (the NVN control section; the harness reads +0x6f8/+0x6fc/+0x700
program length and constant data, +0x708 start, +0x714 stage, +0x748 work group, +0x754 shared memory).

```powershell
dotnet tools\shaders\build\bin\Release\net10.0\spirv_harness.dll --batch <corpus> --out <out> --jobs 8 [--host desktop|moltenvk] [--target-env vulkan1.3] [--no-retry]
dotnet tools\shaders\build\bin\Release\net10.0\spirv_harness.dll <code.bin> <ctrl.bin> -o x.spv [--glsl]
```

`spirv_harness --serve --cache <spirv dir> [--corpus <dir>] [--jobs N] [--priority below|idle|normal]` is the long-lived form the
library's setup and the runtime's on-demand translation use: requests on stdin (`C <hash>` corpus block, `T <hash> <code b64> <ctrl b64>`,
`P <priority>`, `E` end), answers on stdout (`R<TAB><results.tsv row>` once the `.spv` and the row are in the cache, `X <hash> <reason>`).
Protocol and the rules for two writers on one cache: `harness/Serve.cs`. The project uses server GC (one heap per core; the runtime caps
it with `DOTNET_GCHeapCount`).

Output: `<hash>.spv`, `results.tsv` (hash, group, stage, status, spirv-val result, bound resources in Maxwell and Vulkan
terms, translator messages, draw-time queries) and `summary.txt`. The binding scheme is Ryujinx's Vulkan one (set 0
uniform buffers with binding 0 reserved for the support buffer, set 1 storage buffers, set 2 textures, set 3 images; see
`refs\SuperMarioWonderRecomp\docs\nvn\TRANSLATOR.md`, which also explains the `ok-retry` status).

### Differences from SuperMarioWonderRecomp's harness

- Host profile `--host desktop` (default): full precision, geometry shaders, float64. `--host moltenvk` reproduces SuperMarioWonderRecomp's answers.
- Stage comes from the SPH (vertex, tessellation, geometry, pixel); compute also from control +0x714 = 5, for blocks without an SPH.
- `spirv-val` target defaults to `vulkan1.3`; the executable is found through `VULKAN_SDK` or `PATH`.
- Extra post-pass: `OpCapability MultiViewport` when a module decorates `ViewportIndex` without a capability (`docs/debug/001.md`).
- Sources are staged into `third_party\` so `refs\` stays read-only.

## Provenance and licences

| Files | From | Licence |
|---|---|---|
| `harness/*`, `harness/translator/*` (csproj, `spirv-else-if.patch`, `spirv-fmz.patch`, `spirv-nan.patch`), `scripts/apply_patch.py` | SuperMarioWonderRecomp `tools/maxwell/` | MIT, Copyright (c) 2026 BelmanteGu (`licenses/LICENSE.SuperMarioWonderRecomp.MIT`); modified as listed above |
| staged `third_party/ryubing` (not committed) | Ryubing, Codeberg archive `a82350bb77`, via `refs\ryubing` | MIT (`licenses/LICENSE.Ryubing.MIT`) |
| `ampere/decoder/*.cs` (decode rules) | inverted from Mesa NAK `src/nouveau/compiler/nak/sm70_encode.rs` (no code copied); gaps filled by nvdisasm bit probing (`ampere/probe`) | MIT, Copyright © 2022 Collabora, Ltd. (`licenses/LICENSE.mesa-nak.MIT`) |

## Changes for the NVN back end (S4d)

- `harness/translator/spirv-pixld-*.patch` (five files): PIXLD.MY_INDEX reads `gl_SampleID` (new `IoVariable.SampleId`), `gl_PerVertex` declares `ClipDistance[7]`. Before the patch 33,361 of 33,744 pixel blocks hit "Shader instruction Pixld is not implemented"; after it none do.
- `StaticGpuAccessor.cs`: the host profile reports no fragment shader interlock, so no module needs `SPV_EXT_fragment_shader_interlock`.

`spirv-nan.patch` (carried from SuperMarioWonderRecomp) is applied since 2026-11-07: float Clamp/Min/Max become NClamp/NMin/NMax (FMNMX and .SAT drop NaN). It patches the same file as `ampere-mesh-instructions.patch` and is chained after it through the `Then` metadata of that `ShaderPatch` item (`scripts/apply_patch.py` takes several patch files in order).

## Structurizer: loop exits in goto elimination (the game A8)

- `harness/translator/structured-goto-loop-exits.patch` (`StructuredIr/GotoElimination.cs`): Ryujinx eliminates gotos last to
  first. A goto that leaves a loop becomes `if (c) break;` in the loop (MoveOutward); when a later goto in the same loop body then
  encloses statements in a new do-while (EncloseDoWhile, Lift: the flag loops `do { ... } while (gotoTemp)`), that break was
  enclosed too and left only the new loop. The game's material shaders (a light loop with a `CALL.REL.NOINC` exit and a `BRX` switch
  inside whose cases jump to the switch's end) then never left the light loop: the GPU was lost after the driver's timeout. A new
  do-while now forwards every break/continue of an outer loop it encloses: the exit sets a flag and leaves the new loop, and
  `if (flag) { flag = false; break; }` (or `continue`) right after the new loop repeats it. The flags are cleared at the start of the
  function, not before the loop (a later goto may guard the statements before it).
- **Structurizer check** (`harness/translator/StructuredCfgCheck.cs`, hooks in `structured-program-cfg-check.patch` and
  `structured-context-node-blocks.patch`): `NXRT_HARNESS_CFG_CHECK=N` (off by default; then the output is unchanged) makes every
  translation run N random walks through each structured function right after goto elimination. Branch conditions of the original
  code take random values, goto-elimination flags are evaluated exactly, and every time the walk enters a basic block (its first AST
  statement) the block must be the one the block graph names after the previous block. A mismatch, a statement that runs outside its
  block or a walk that does not end is an exception, so `--batch` lists the block as `exception: ... CFG check`; a passing translation
  prints `cfg check <function>: N walks ok, blocks seen/total, edges seen/total` on stderr. The game corpus (47,203 blocks, 100 walks each,
  `--batch ... --jobs 8`, ~6 min): 14,684 blocks fail before the fix, all pixel shaders with BRX/CALL.REL.NOINC, and none after it
  (one compute block branches on an undefined value and is not checked). The fix changes the SPIR-V of 14,692 blocks (the 8 more
  fail before it after 100-107 walks); 17,094 corpus blocks contain a BRX/BRXU or CALL.REL.NOINC.

## Structurizer for reducible graphs (register pressure, 2026-11-09)

- Ryujinx's structurizer (`StructuredProgramContext` + `GotoElimination`) makes a goto of every branch that does not land inside the
  current `if` and then removes the gotos with the "Taming Control Flow" moves. The game's material fragment shaders (a light loop with a
  BRX switch whose cases leave to a common merge from different depths) came out with five `do { } while (flag)` loops nested in the
  light loop plus many `if (!flag)` guards, and NVIDIA's compiler (610.88) needed 235-255 registers (or capped at 168 and spilled) for
  code whose SASS uses 75-112.
- `harness/translator/CfgStructurizer.cs` (+ `.Lowering.cs`; hooks `structured-program-cfg-structurizer.patch`,
  `structured-context-cfg-structurizer.patch`, applied through the csproj's new `Then2` metadata) structures every function whose block
  graph is reducible after Ramsey, "Beyond Relooper" (ICFP 2022): dominator tree, merge nodes (two or more forward in-edges, or entered
  by an edge leaving a loop) after the code they follow, branches to a node dominated alone inlined, back edges as `continue`. A branch
  to a merge node from the end of an if/else chain needs nothing; from further inside it sets a flag that guards the statements up to
  the merge (`if (!flag)`), and across a loop it breaks (`flag = true; break;`, `if (flag) break;`). Loops are `do { ...; break; }
  while (true)` or `do { } while (c)` when the body ends with its only continue. `if (c) { ...; return/discard/break/continue; }` takes
  the else part after it, not in an else block. Flags nobody reads are removed. Irreducible graphs keep Ryujinx's path, and so do the
  translator's own helper functions (only fun0..N use the new one): with it the gmem page-walk helpers grew ~1% and the gmem compute
  shaders that call them (the game's HiZ a17259b2 & co.) ran ~30% slower in game.
  Off: `NXRT_HARNESS_STRUCTURIZER=ryujinx`; `=all` also the helpers; `=log` prints fallbacks. `NXRT_HARNESS_STRUCT_FLAT_ELSE=0` keeps
  else blocks (without the flat form small fragment shaders came out 1-8 registers above the old path).
- Measured with `pipestat` on random corpus samples (old cache vs new, RTX 4080, 610.88): fragment n=200 registers mean 136.4 -> 93.1,
  median 168 -> 96, binary 51.3 -> 39.4 KB, 133 better / 2 worse; vertex n=120 59.5 -> 59.7 (9 worse / 6 better by a few registers);
  mesh n=60 139 -> 129; compute n=60 unchanged. Hot material fragment shaders 168-255 -> 80-124 (their SASS: 75-112). The gpt mesh/vertex
  stages stay at 255 (also with `NXRT_HARNESS_GMEM_IDENTITY=1`, `NO_SLOTS=1` and the new `NXRT_HARNESS_GMEM_FORCEINLINE=1`, helpers
  inlined: measurement only).
- Only real loops are loops now, so the vertex/fragment loop cap (`NXRT_HARNESS_LOOP_CAP`) counts real back edges only. Ballots and
  votes stay where the IR has them (`structured-subgroup-propagation.patch` works on the AST afterwards as before).
- Checked: the corpus with `NXRT_HARNESS_CFG_CHECK=100` (47,202 blocks, 56,594 functions): 0 exceptions (the 7
  GotoElimination NullReferenceExceptions of the old path translate now), 46,995 ok as before.
- `BlockOrder.cs` (`cfg-block-order.patch`, a first guess: blocks in a loop-contiguous topological order before structuring) changes
  nothing for the game (the IR is already in such an order) and is kept as a guard for code laid out otherwise; `NXRT_HARNESS_BLOCK_ORDER=0`
  turns it off, `=log` prints each function's block graph.

## Pool arrays: texture handles computed per pixel (the game A7)

- A bindless sample whose handle neither the bindless-handle patch nor Ryujinx's `TryConvertBindless` can tie to something fixed per
  draw (the game: 4,336 fragment shaders sample their clustered-light shadow maps with `TEX.SCR.B.DC`, the handle read per light from a
  storage buffer inside the light loop) used to be dropped (it read 0). `StaticGpuAccessor.QueryHostSupportsSeparateSampler` now
  answers true, so Ryujinx's `GenerateBindlessAccess` turns it into an index into a sampled-image array (texture id = handle & 0xfffff,
  65,536 entries) and one into a sampler array (sampler id = handle >> 20, 4,096 entries), with `NonUniform` decorations. Blocks whose
  handle resolves per draw are untouched: `TryConvertBindless` runs first.
- Each array gets an extra descriptor set (`CreateExtraSet`): set 4 + 3 x stage slot + n, binding 0 (fragment: set 7 = the textures,
  set 8 = the samplers). The resource list names them `tex<type>[c0:1073741824x<length>]=s<set>b0` (`texNone` = the sampler array).
  The runtime binds its persistent pool sets there (`runtime/src/gfx/nvn/nvn_pool_sets.cpp`, `pool_set_sync` in `nvn_replay.cpp`).
- Off: `NXRT_HARNESS_POOL_BINDLESS=0` or `--no-pool-bindless` (the sample is dropped as before).

## Ballots, votes and shuffles stay where the SASS runs them (the game A18)

- NVN compute code counts with warp-aggregated atomics: `VOTE[U].ANY|ALL Rd, PT|Pp` (the active mask or a ballot), leader = FLO of it or
  lane 0, one ATOMG/RED by the leader with POPC(mask) or REDUX.SUM, SHFL.IDX of the result from the leader, rank = POPC(mask &
  lanemask_lt). The SASS runs the VOTE with every lane of the warp before the branch to the leader.
- Ryujinx's `AstOptimizer` folded a single-assignment value into its uses; a unary operation of an operand (a ballot) counts as worth
  folding for any number of uses, so the ballot moved into each use, also into the leader-only branch, where it ran with the leader
  alone. The game's GPU culling (4c1e8564 and the compute chain after it, all from `main`) then reserved one slot per warp while every lane
  wrote its record at base + rank: records overwrote each other differently every frame (f2422a7a: 46 records instead of 548), the mesh
  draw counts changed from replay loop to replay loop, and grass, bushes and leaves flickered.
- `structured-subgroup-propagation.patch` (after `spirv-call-args.patch`): a value that contains a ballot, vote or shuffle is folded only
  into the next statements of its own block with no block in between; otherwise it stays in its local, computed where the SASS
  computes it. With it the chain's results are the same in every replay loop (mesh draw x counts and f2422a7a's records equal as sets;
  only list positions written by atomics differ in order).
- `NXRT_HARNESS_VOTE_SELF=1` (diagnosis, off by default): in compute programs `VOTE Rd, PT` is the lane's own bit and `REDUX` the lane's own
  value. That was the first stopgap for the culling counts (2026-11-08); it changes warp-wide results the game relies on (a max over the
  object's 32 lanes became each lane's own value) and left +-0.5% of loop-to-loop variation.
- Ruled out on the way (replays of capture `20261107-230737-nb3-alias_kid`): the pool mirror, the storage shadows and held compute
  results, volatile device-pointer accesses, maximal reconvergence, memory the replay does not reset (`[capture] reset_full`), barriers
  between dispatches (every dispatch already ends with a full memory barrier), and the chain's inputs (the same up to the order of
  atomically appended lists).

## Pointer accesses: out-of-line helpers, inline fast path (A17 follow-up, 2026-11-09)

- Ryujinx's `GlobalToStorage` makes one helper function per access kind and calls it for every 32-bit word; NVIDIA's compiler inlined
  every call, so a pointer shader (`gpt`) carried the bound-slot search and the page walk once per word: the game's fern mesh `fbb731e2`
  253 calls, 1.26 MB of driver code, 20 s to build; the heavier gpt mesh blocks 2.5 MB and 45-82 s.
- Now (`ampere/frontend/AmpereGlobal.cs`, hooks in `spirv-g2s-c0-base.patch`, `ampere-mesh-instructions.patch`, `spirv-else-if.patch`):
  the fallback helpers are `DontInline` (the 610.88 driver honours it), take their in arguments by value (Ryujinx's pointer arguments put
  every call site's temporaries into local memory once the call stays a call) and a plain 32-bit load gets an inline fast path: the
  page cache (5 words, now main's own, so it stays in registers) is checked at the call site, and with no bound slot marked (c[0]
  `SlotMaskC0` word 0) and the address in the cached span, the word is loaded right there through the cached device address; only a
  miss calls the helper, which returns {value, the span its walk ended in} as a struct. The bound-slot search visits only the marked
  slots (FindLSB loop) instead of all 16.
- Measured (RTX 4080, driver 610.88, `pipestat`), code / build time before -> after: fern mesh fbb731e2 1.26 MB / 20 s -> 169 KB / 1.4 s;
  vertex 5332b131 1.22 MB / 16 s -> 140 KB / 1.0 s; mesh 596bf624 2.57 MB / 82 s -> 351 KB / 3.0 s (the other 2.5 MB mesh blocks
  99908ab2, acf920b7, a5bfd19f, a873d795, b8b873e5 took 46-68 s before). Registers stay at 255: the original SASS of 5332b131 / 40953571
  already uses R247 / R252, and with the lookup removed altogether (`IDENTITY`) they are still 255. In game (village outside, 1920x1080,
  100-300 s, alone, same exe; two pairs): 35.0 vs 34.7 fps and 33.6 vs 34.3 (one slow window while new pipelines built in the
  background), steady windows +0.4: **fps unchanged**; the gain is the pipeline build time (on-demand builds, the shader preparation).
  Cost: the SPIR-V of the 3,425 gpt blocks grows from 742 MB to 1.85 GB (the per-word call sites). Dead end: helpers out of line with
  Ryujinx's pointer arguments and no fast path halved the fps (16.6 vs 32.7: every word a call, its temporaries in local memory).
- Switches (for measuring): `NXRT_HARNESS_GMEM_NOINLINE=0` (helpers inlined again), `NXRT_HARNESS_GMEM_FAST=0` (no fast path),
  `NXRT_HARNESS_GMEM_SCAN_ALL=1` (the 16-slot search). Measurement only, wrong results: `NXRT_HARNESS_GMEM_IDENTITY=1` (guest address
  used as device address), `NXRT_HARNESS_GMEM_NO_SLOTS=1`, `NXRT_HARNESS_GMEM_FIRSTSLOT=1`.
- `pipestat/` (`build.cmd`, then `pipestat.exe [--frag fs.spv] a.spv [task.spv+mesh.spv] ...`): compiles modules into pipelines on the
  local GPU with no game (descriptor layouts from the modules' decorations; vertex/mesh without a fragment stage) and prints the
  driver's statistics per executable (registers, binary size, local memory) and the compile time.

## Pointer accesses: vector loads (F1, 2026-11-10)

- The front end splits LDG.64 / LDG.128 into 32-bit loads (the IR is scalar), so through the fast path each word made its own page-cache
  test and its own 4-byte load: the game's gpt mesh blocks read 61-66 light-probe records per lane with LDG.128 (scattered over memory), i.e.
  four load instructions where the SASS has one.
- Now the words of a vector load carry a tag (`AmpereGlobal.NoteVectorWord`, an extra constant argument of the fast helper call that the
  helper ignores; `FastArguments` = 2). At word 0 the back end (`TryGenerateHelperCall`) loads the whole `uvec2` / `uvec4` through the
  cached span (`Aligned` 8 / 16; an aligned LDG.64/.128 never crosses a 4 KiB page); on a miss the helper walks as before and the vector is
  loaded through the span its walk returned. The other words take their lane; only when a bound slot answered word 0 (no span) do they
  call the helper themselves. `NXRT_HARNESS_GMEM_VECTOR=0`: the old per-word form.
- Measured: pipestat fbb731e2 165 -> 145 KB, 5332b131 136 -> 116 KB, 6ec65625 204 -> 184 KB (registers stay 255); in game (village
  outside, 1920x1080 uncapped, same exe, alone, warm): **36.6 -> 40.4 fps** (results in AGENTS.md "Session 2026-11-10 (F1)"). Village
  capture replay: the same picture as the old cache (SSIM 1.000000, max difference 2, as two shots of one cache).

## Pointer accesses in compute: the page cache with marked slots (C1, 2026-11-10)

- A compute binding is the pool copy or a shadow, so the runtime marks bound slots in the c[0] slot mask (all of them before P4's
  `[nvn] cs_gmem_held`; since then the slots over held results, ~5 of 11 per dispatch in the village). The inline fast path required mask 0, so
  The game's HiZ / culling pointer shaders (submission #3, the one the game's main thread waits on) never took it: every 32-bit word (and every
  word of an LDG.128) called its helper, searched the marked slots and walked the table.
- Now (compute only, `AmpereGlobal.EmitSpanClip` / `CacheAloneDecides`): the helper hands main the span its walk ended in only when no
  marked slot overlaps it (64-bit range test per marked slot, on the slow path only), so a cached span can never hide a marked binding, and
  compute call sites test the cache alone. `NXRT_HARNESS_GMEM_CS_SPAN=0`: the old form. F1's vector loads stay off for compute
  (`NXRT_HARNESS_GMEM_CS_VECTOR=1`): on the village replay they changed ~175 pixels by more than 20, every loop alike, unexplained.
- Measured (village outside 1080, stamped `gpu_passes`, P4 runtime): #3 5.63 -> 4.90 ms per frame, a17259b2 (6 dispatches) 0.77 -> 0.24 ms,
  39a1f760 / 5d8324bd 0.12 -> < 0.05 ms each; registers and code size unchanged (pipestat: a17259b2 still 255 registers, ~178 KB; the 255
  is not the lookup: with `GMEM_IDENTITY` + `NO_SLOTS` + forced inline it stays 255 at 115 KB). fps unchanged (render-thread bound).

## Register pressure of the gpt pre-raster stages: what it is, and what did not pay (F1, 2026-11-10)

- Most hot gpt blocks use ~253 registers in their original SASS too (40953571 R252, 5332b131 R247, a5bfd19f / b8b873e5 / a873d795 /
  e41230f8 / 8fb97d28 R252): 255 there is the shader, not the translation. Only fbb731e2 and 6ec65625 (SASS R125, NVN register count 128 in
  the control section +0x4c) are inflated.
- Cause 1: `AstOptimizer` folds every single-use value into its use, however far away (constant-buffer loads, texture results, whole
  arithmetic trees): the inputs then live until the attribute stores at the end. `structured-propagation-distance.patch`,
  `NXRT_HARNESS_PROPAGATE=near:K` (fold only into one of the next K statements of the same block; `none` = DebugMode's form; default
  `all` = Ryujinx's): fbb731e2 255 + 288 B spills -> 174 registers, no spills; 6ec65625 255 + 176 B -> 174-204; fragment sample (n=60)
  mean 81.3 -> 72.4 registers (32 better / 4 worse), vertex n=60 unchanged; near:4 = near:16 = none. **No fps in game** (village outside:
  36.2 / 36.6 vs 36.9; with vector loads 40.2 vs 40.4), so it stays an experiment.
- Cause 2: with the page-cache branches removed (straight-line loads, `GMEM_IDENTITY` + `FAST=0`), NVIDIA's compiler hoists every load
  and goes back to 255 even without propagation: the per-word diamonds were limiting it.
- `NXRT_HARNESS_MESH_DIRECT=1` (`AmpereMesh.cs`): mesh attributes written straight into the mesh outputs instead of the 13 KB
  shared-memory ISBE copy that the end of the program copied out (the address form `[Ra + k * 128]` is proven from Ra's definition,
  `LineZeroForm`, so the line is static; other forms decoded inline with a switch; the counts are declared at the start with the maximum
  primitives, the unused ones get degenerate indices). Shared memory 13 KB -> 0.4 KB, mesh sample registers as before (n=60 mean 116 ->
  115). **Slower in game** (36.1 / 36.3 vs 36.9 fps; probably the degenerate primitives up to the maximum): off.
- Not tried: spirv-opt (no Vulkan SDK here), specialization constants for the page-table shifts (the walk runs only on a miss).

## gpt pre-raster stages after F1: what is left, experiments (F2, 2026-11-10)

- Measured in game (village outside, 1920x1080 uncapped, alone, exe of a70f2aa, paired `cpuperf_ab` windows from 100 s): `[nvn] skip_gpt = 2`
  (gpt draws without fragments) **+0.1 fps** (8 pairs, 43.0 vs 42.9): the gpt draws' rasterization and fragment work is off the critical
  path; `skip_gpt = 1` (gpt draws left out) +3.75 fps (9/9), and +3.95 with the mesh blocks emitting no primitives (below): the gpt draws'
  remaining cost (~1.9 ms per frame) is their pre-raster work plus the render thread's per-draw work, and the frame is render-thread bound
  (replay 22.4 ms, idle 0.6 ms per frame). Graphics pointer stages with a slot marked (no inline fast path): 25-29% of the sets.
- Experiment switches, all off (measurement or no gain; the default translation is byte-identical to F1's):
  - `NXRT_HARNESS_GMEM_SELECT=1`: the fast-path load sits after the merge through a selected address (cached span, the span the walk
    returned, else the page table's own address as a stand-in) and the value is selected; only the miss call is control flow. pipestat:
    same code size, 50-130 B less local memory. Game: 41.3 vs 41.7 and 39.7 vs 42.2 fps: dropped.
  - `NXRT_HARNESS_MESH_EARLY_COUNT=1` (with `NXRT_HARNESS_MESH_DIRECT=1`): SetMeshOutputsEXT with the real primitive count right after the
    program's own count write (`ISBEWR.O.32 [3]`, lane 0, before any attribute write in all hot blocks), so the direct-output epilogue no
    longer sends degenerate primitives up to the maximum; used only when the count is written once and before the first attribute.
    Shared memory 13 KB -> 0.4 KB, code unchanged, picture the same. Game, four pairs against the shared cache: +0.8, -1.3, 0.0,
    +0.2 fps (42.5 / 40.9 / 42.0 / 42.4 vs 41.7 / 42.2 / 42.0 / 42.1): noise, stays off.
  - `NXRT_HARNESS_GMEM_IGNORE_MASK=1` (measurement only, wrong where a held result lives only in the pool copy): the fast path whatever
    the slot mask says, i.e. what a runtime with every mask 0 gives: 41.5 / 40.7 vs 41.7 / 42.2 fps, no GPU gain.
  - `NXRT_HARNESS_MESH_EXP=noprims` (measurement only): every mesh work group runs but emits no primitive.
  - `NXRT_HARNESS_MESH_PAYLOAD_EXACT=1` (experiment, off): the task payload is declared at the block's own size (control +0x5a, rounded
    to 16 bytes) instead of 4 KiB. The grass task/mesh draws took the same GPU time with it (PERFORMANCE.md, GPU round 5).
- Not built, with the reason: vector stores (the gpt pre-raster stages have no STG); vector loads for the bound-slot path (the GPU side of
  the gpt draws is not on the critical path, see `IGNORE_MASK`); per-thread-indexed constant buffer reads through storage buffers (the
  console's own SASS reads them with LDC as well); pointer tables bound as storage buffers (a load through them still needs a bounds check
  and a fallback, the same shape as the page-cache test).

## Direct mesh outputs on by default (R8, 2026-11-12)

- Once the village frame was GPU-bound (render thread with slack after R8), F2's `NXRT_HARNESS_MESH_DIRECT` + `NXRT_HARNESS_MESH_EARLY_COUNT`
  were measured again: alternating runs of two private caches whose mesh/task blocks differ only by these switches (both translated by
  the same harness), two runs each after a warm-up: **69.0 / 69.0 vs 66.7 / 66.6 fps**, GPU exec -0.4..-0.5 ms per frame, frames over
  15 ms 34-36% vs 44-52%. Capture replays of both village captures: SSIM 1.0 / 0.999999 against the shared cache. Both are on by default
  now (`=0` turns them off); the shared cache's 785 mesh/task blocks were translated again.
- `NXRT_HARNESS_PROPAGATE=near:16` on top (mesh blocks and ~55 hot fragment/vertex blocks; the hot meshes 6ec65625 / fbb731e2 go from 255
  registers with spills to 169 / 178 without): 69.5 / 69.6 vs 69.2 / 68.7 fps, frames over 15 ms 22-23% vs 25-37%. A small gain; not made
  default (it would apply to every stage; a per-stage default is the next step if it is wanted).

## TLD / TLD4 / TXD .F16 (the game upscaler network, A22)

- TEX, TLD, TLD4 and TXD all carry `.F16.RN` / `.F16.RZ` in bits 79..80 (nvdisasm 13.3 probe: `TLD4.SCR.R.F16.RN.PTP`,
  `TLD.SCR.F16.RN.LZ`, `TXD.F16.RN`; TMML ignores the bits). The decoder knew the field for TEX only, so TLD4.F16 was translated as an
  fp32 gather: four components into Rd, Rd+1, Rd2, Rd2+1 instead of two fp16 pairs into Rd and Rd2. That clobbered live registers (in
  f90d726d the blue gather's Rd2+1 = R34 was the x coordinate of the next gather) and left fp32 bits where the code reads half pairs.
- With the field decoded the existing F16 path applies: Rd = packHalf2x16(t0, t1), Rd2 = packHalf2x16(t2, t3). For the game's
  `TLD4.PTP` with offsets (0,0) (1,0) (0,1) (1,1) the code stores Rd as the row-y pair and Rd2 as the row-y+1 pair of a shared-memory
  tile (`STS [R8], R12; STS [R8+0x90], R13`), which fits that order. TXD now takes `.F16.RN` the same way. `.F16.RZ` stays unsupported.
- Affects only blocks from `main` (27 "runtime" compute blocks: the game's upscaler network variants); no bfsha/vsetb corpus block has it.


## Peepholes and measurement switches (G4, 2026-11-12; all opt-in)

`ampere/frontend/AmpereFuse.cs`. None of these is on by default: the empty-program test showed that the hot grass meshes' GPU time is not
their code (PERFORMANCE.md, "GPU round 4"), and in game none moved the GPU time.
- `NXRT_HARNESS_SIN_FUSE=1`: a MUFU.SIN/COS that reads the product of `FMUL[.FTZ].RZ Rd, x, 1/(2pi)` in the same straight-line run (no label
  or control flow, Rd and the guard not rewritten, same guard) is emitted as sin(x) / cos(x) of a copy of x taken at the FMUL. The driver
  compiles that into the console's own pair (FMUL.FTZ.RZ by 1/(2pi), MUFU.SIN), instead of the exact RZ emulation (~15 instructions) and
  sin(2pi * (t - round(t))). The FMUL itself stays exact for any other reader. `hwcheck --sin-pairs` builds two-instruction cases for it
  (their CUDA launch still fails, see PERFORMANCE.md).
- `NXRT_HARNESS_SPECULATE=1`: a predicated pure ALU instruction runs unconditionally when, on every path on which its guard is false, each
  register it writes is overwritten before anything may read it (forward search that knows the guard predicate until it is rewritten).
- `NXRT_HARNESS_MESH_PROOF_SHIFT=1`: a mesh output address `x << s` with 2^s a multiple of the vertex group size counts as line 0 of its
  vertex (no run-time form check).
- Measurement only (wrong results): `NXRT_HARNESS_EXP_NOPRED=1` (predicated ALU instructions unconditional: the ceiling of the above),
  `NXRT_HARNESS_EXP_NOFTZ=1` (no FTZ input flushes), `NXRT_HARNESS_MESH_EXP=zeroprims` (no primitive), `NXRT_HARNESS_MESH_EXP=empty` and
  `NXRT_HARNESS_EXP_EMPTY=1` (the program is only its EXIT): translate one block into a private cache and compare skip windows of its draw.
- `NXRT_HARNESS_FUSE_DEBUG=1` prints the fused pairs and the speculated count.
- `pipestat` now turns robustness off as the runtime does (`PIPESTAT_ROBUST=1` keeps it on).
