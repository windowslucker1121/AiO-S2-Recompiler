# tools/shaders: Maxwell to SPIR-V translation harness

Offline harness that turns Maxwell (SM 5.3) shader machine code into SPIR-V with two independent translators, validates
every output with `spirv-val`, and compares them. Written to run on the game's shader blobs once the dump exists; until
then it is tested on a stand-in corpus of real Maxwell binaries.

| Translator | Source | Output | Role |
|---|---|---|---|
| Ryujinx `Ryujinx.Graphics.Shader` (C#) | harness ported from SuperMarioWonderRecomp, built against `refs\ryubing` | SPIR-V directly | primary candidate; also writes `results.tsv`, the binding contract |
| smo-recomp `mx_translate.c` | copied from smo-recomp | GLSL, compiled to SPIR-V by the Vulkan SDK's `glslang` | second candidate, same inputs |

## Layout

| Path | What |
|---|---|
| `harness/` | C# console `spirv_harness` (single stage or `--batch`), `StaticGpuAccessor.cs` (answers the translator's GPU queries), `translator/` (csproj that builds Ryujinx's translator from staged sources plus the `.patch` files) |
| `smo/` | `mx_translate.c/.h`, `mx_ops.h` (unmodified), `mx_batch.c` (parallel batch driver), `build.ps1` |
| `scripts/` | `setup_ryujinx.ps1` (stage Ryujinx sources), `apply_patch.py`, `dksh_to_corpus.py`, `corpus_to_mxs.py`, `build_corpus.py` |
| `oracle/` | `run_oracle.py` and `expected_sascha.json` |
| `corpus/sascha_dksh/` | 289 stand-in Maxwell binaries (`.dksh`) plus the build report |
| `licenses/` | licence texts of the borrowed code |
| `third_party/`, `build/` | git-ignored: staged Ryujinx sources, build output |

## Build

```powershell
powershell -File tools\shaders\scripts\setup_ryujinx.ps1          # copies 3 projects from refs\ryubing (never builds in refs\)
dotnet build tools\shaders\harness\SpirvHarness.csproj -c Release   # first restore ~2 min (an unreachable NuGet feed times out), then seconds
pwsh -File tools\shaders\smo\build.ps1                              # clang-cl + vcvars64, builds build\smo\mx_batch.exe
```

Needs .NET SDK 10, the Vulkan SDK (`spirv-val`, `glslang`; the oracle defaults to `C:\VulkanSDK\1.4.363.0\Bin`) and the
project venv (`tools\.venv`) for the scripts.

## Use

Hardware oracle per ALU instruction (GPU vs our translation): `dotnet run --project tools\shaders\hwcheck -c Release`, see `hwcheck/README.md`.

Input is a folder of extracted blocks in Wonder's layout: `manifest.tsv` (columns `id stage code_off code_size ctrl_off
ctrl_size program_start program_bytes const_off const_size code_sha1_16 code_file ctrl_file`), `code_<hash>.bin` (the code
block, SPH at `program_start`) and `ctrl_<hash>.bin` (the NVN control section; the harness reads +0x6f8/+0x6fc/+0x700
program length and constant data, +0x708 start, +0x714 stage, +0x748 work group, +0x754 shared memory).

```powershell
dotnet tools\shaders\build\bin\Release\net10.0\spirv_harness.dll --batch <corpus> --out <out> --jobs 8 [--host desktop|moltenvk] [--target-env vulkan1.3] [--no-retry]
dotnet tools\shaders\build\bin\Release\net10.0\spirv_harness.dll <code.bin> <ctrl.bin> -o x.spv [--glsl]
```

`spirv_harness --serve --cache <spirv dir> [--corpus <dir>] [--jobs N] [--priority below|idle|normal]` is the long-lived form the
launcher's setup and the runtime's on-demand translation use: requests on stdin (`C <hash>` corpus block, `T <hash> <code b64> <ctrl b64>`,
`P <priority>`, `E` end), answers on stdout (`R<TAB><results.tsv row>` once the `.spv` and the row are in the cache, `X <hash> <reason>`).
Protocol and the rules for two writers on one cache: `harness/Serve.cs`. The project uses server GC (one heap per core; the runtime caps
it with `DOTNET_GCHeapCount`).

Output: `<hash>.spv`, `results.tsv` (hash, group, stage, status, spirv-val result, bound resources in Maxwell and Vulkan
terms, translator messages, draw-time queries) and `summary.txt`. The binding scheme is Ryujinx's Vulkan one (set 0
uniform buffers with binding 0 reserved for the support buffer, set 1 storage buffers, set 2 textures, set 3 images; see
`refs\SuperMarioWonderRecomp\docs\nvn\TRANSLATOR.md`, which also explains the `ok-retry` status).

Stand-in corpus to translator input: `scripts\dksh_to_corpus.py <dksh dir> --out <dir>` (DKSH to the layout above);
`scripts\corpus_to_mxs.py <corpus> --out <mxs dir>` (layout above to smo's stage files). The oracle does both itself.

### Differences from Wonder's harness

- Host profile `--host desktop` (default): full precision, geometry shaders, float64. `--host moltenvk` reproduces Wonder's answers.
- Stage comes from the SPH (vertex, tessellation, geometry, pixel); compute also from control +0x714 = 5, so DKSH-derived blocks work.
- `spirv-val` target defaults to `vulkan1.3`; the executable is found through `VULKAN_SDK` or `PATH`.
- Extra post-pass: `OpCapability MultiViewport` when a module decorates `ViewportIndex` without a capability (`docs/debug/001.md`).
- Sources are staged into `third_party\` so `refs\` stays read-only.

## Stand-in corpus

Real Maxwell binaries, made from freely licensed sources: Sascha Willems' Vulkan GLSL samples (MIT, commit `1cbc8d87`) rewritten by
`scripts\build_corpus.py` into the GLSL dialect of devkitPro's `uam` (set numbers removed, push constants to a uniform
block, `gl_VertexIndex` to `gl_VertexID`, and so on) and compiled by `uam` 1.0.0 (built from the official release
tarball in WSL) to `.dksh` modules. 289 of 300 shaders compile (138 vertex, 138 fragment, 10 compute, 3 geometry); the
222 distinct code blocks are what the oracle counts. Caveat: deko3d's driver constant buffer layout differs from NVN's
(`uam` README: "Internal deko3d constbuf layout"), so texture handles sit at different words than in an NVN shader. That
is why smo's translator, which hard-codes NVN's `c[2]` handle table, fails 72 of 222 blocks on this corpus
("texture handle"); Ryujinx resolves them. The corpus shows that the plumbing and instruction coverage work; the pass-rate
comparison that decides the translator must be redone on the real game shaders.

## Oracle

```powershell
$env:VULKAN_SDK='C:\VulkanSDK\1.4.363.0'
tools\.venv\Scripts\python.exe tools\shaders\oracle\run_oracle.py --dksh tools\shaders\corpus\sascha_dksh --out work\corpus\shaders_oracle_dksh --expect tools\shaders\oracle\expected_sascha.json
```

Runs both translators, re-validates every `.spv` itself with `spirv-val --target-env vulkan1.3`, prints per-stage counts
and an agreement table, and exits 0 only when the totals equal the locked baseline and the independent validation
count equals what each translator claimed. Current result: 222 blocks; Ryujinx 222 valid (206 first try, 16 after the
DebugMode retry); smo 135 valid (87 not translated, 72 of them texture handles, see the corpus caveat). About 8 s.

## Provenance and licences

| Files | From | Licence |
|---|---|---|
| `harness/*`, `harness/translator/*` (csproj, `spirv-else-if.patch`, `spirv-fmz.patch`, `spirv-nan.patch`), `scripts/apply_patch.py` | SuperMarioWonderRecomp `tools/maxwell/` | MIT, Copyright (c) 2026 BelmanteGu (`licenses/LICENSE.SuperMarioWonderRecomp.MIT`); modified as listed above |
| staged `third_party/ryubing` (not committed) | Ryubing, Codeberg archive `a82350bb77`, via `refs\ryubing` | MIT (`licenses/LICENSE.Ryubing.MIT`) |
| `smo/mx_translate.c`, `mx_translate.h`, `mx_ops.h` | smo-recomp `runtime/gfx/` | GPL-3.0 (`licenses/LICENSE.smo-recomp.GPL-3.0`), unmodified |
| `smo/mx_batch.c`, `scripts/corpus_to_mxs.py` | derived from smo-recomp `tests/mx_all.c`, `tests/check_all_shaders.py` | GPL-3.0 |
| `corpus/sascha_dksh/*.dksh` | compiled from SaschaWillems/Vulkan GLSL by `uam` 1.0.0 | sources MIT (Sascha Willems); `uam` is zlib-style plus Mesa MIT; output carries the source licence |
| `ampere/decoder/*.cs` (decode rules) | inverted from Mesa NAK `src/nouveau/compiler/nak/sm70_encode.rs` (no code copied); gaps filled by nvdisasm bit probing (`ampere/probe`) | MIT, Copyright © 2022 Collabora, Ltd. (`licenses/LICENSE.mesa-nak.MIT`) |

## Changes for the NVN back end (S4d)

- `harness/translator/spirv-pixld-*.patch` (five files): PIXLD.MY_INDEX reads `gl_SampleID` (new `IoVariable.SampleId`), `gl_PerVertex` declares `ClipDistance[7]`. Before the patch 33,361 of 33,744 pixel blocks hit "Shader instruction Pixld is not implemented"; after it none do.
- `StaticGpuAccessor.cs`: the host profile reports no fragment shader interlock, so no module needs `SPV_EXT_fragment_shader_interlock`.
- `scripts/make_code_corpus.py <corpus> --out <dir> [--verify <results dir>]`: collapses the 67,506-block survey corpus to the 13,016 distinct code blocks, keyed by the first 16 hex digits of sha1(code). `--verify` checks that blocks sharing code gave identical SPIR-V (9,750 groups, 0 differ).

`spirv-nan.patch` (carried from Wonder) is applied since 2026-11-07: float Clamp/Min/Max become NClamp/NMin/NMax (FMNMX and .SAT drop NaN). It patches the same file as `ampere-mesh-instructions.patch` and is chained after it through the `Then` metadata of that `ShaderPatch` item (`scripts/apply_patch.py` takes several patch files in order).

## Structurizer: loop exits in goto elimination (OoT A8)

- `harness/translator/structured-goto-loop-exits.patch` (`StructuredIr/GotoElimination.cs`): Ryujinx eliminates gotos last to
  first. A goto that leaves a loop becomes `if (c) break;` in the loop (MoveOutward); when a later goto in the same loop body then
  encloses statements in a new do-while (EncloseDoWhile, Lift: the flag loops `do { ... } while (gotoTemp)`), that break was
  enclosed too and left only the new loop. OoT's material shaders (a light loop with a `CALL.REL.NOINC` exit and a `BRX` switch
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
  prints `cfg check <function>: N walks ok, blocks seen/total, edges seen/total` on stderr. OoT corpus (47,203 blocks, 100 walks each,
  `--batch ... --jobs 8`, ~6 min): 14,684 blocks fail before the fix, all pixel shaders with BRX/CALL.REL.NOINC, and none after it
  (one compute block branches on an undefined value and is not checked). The fix changes the SPIR-V of 14,692 blocks (the 8 more
  fail before it after 100-107 walks); 17,094 corpus blocks contain a BRX/BRXU or CALL.REL.NOINC.

## Cache completeness (S40)

- `scripts/gap_corpus.py <romfs> --cache <spirv dir> --out <dir> [--programs programs.bin] [--all]`: scans the game files the way the
  setup tool does (MaterialArchive.arc and every pak MTRL asset, read from the paks, not from retrotool output) and writes the blocks
  the cache lacks as a `--batch` corpus plus `gap_report.json`. The survey corpus had missed 85 gun fragment blocks (328 programs)
  because retrotool stopped part way through `SamusGunMP1.pak`; see `docs/S40.md`.
