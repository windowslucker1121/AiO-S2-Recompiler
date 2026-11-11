# xcheck: our Ampere decoder against nxnative's

Cross-check of `tools/shaders/ampere/decoder` (SM86 SASS decoder, verified against nvdisasm) with the Ampere front end of
[seedds/nxnative](https://github.com/seedds/nxnative) (`dm/third_party/ryujinx`: `DecoderAmpere.cs`, `AmpereOp.cs`,
`InstEmitAmpere*.cs`, MIT, `../licenses/LICENSE.nxnative.MIT`; it is built on Ryubing/Ryujinx, `../licenses/LICENSE.Ryubing.MIT`).
nxnative has no text decoder: its "decoder" is an opcode table (`AmpereTable`) plus emitters that read instruction bits straight into
Ryujinx IR. So the comparison is made on what each side does with a word, not on a shared text form.

## Run

```powershell
git clone https://github.com/seedds/nxnative refs\nxnative                     # once, read-only reference
powershell -File tools\shaders\xcheck\scripts\setup_nxnative.ps1            # stages the 3 Ryujinx projects into tools\shaders\third_party\nxnative
dotnet build tools\shaders\xcheck\tool\xcheck.csproj -c Release
tools\shaders\build\bin\Release\net10.0\xcheck.exe [--out work\xcheck] [--jobs 6] [--no-nvdisasm] [--only a|b|c]     # decoder diff
tools\shaders\build\bin\Release\net10.0\xcheck.exe --layout [--out work\xcheck]                                          # NVN2 control/SPH statistics
```

(or `dotnet run --project tools\shaders\xcheck\tool -c Release -- <options>`). Corpora default to `work\shaders\corpus` (title A,
`a`), `work\shaders_b\corpus` (title B, `b`) and `work\c\shader_dump` (title C, `c`); `--corpus a=<dir>` overrides one.
The default run takes about two hours on 6 threads: the scans (247 M title A instructions) 6 minutes, level 2 (1.9 M exemplar words x 112 bit flips, each run through nxnative's emitters) 12 minutes, and nvdisasm on the 1.9 M level-0 words about 100 minutes (`--no-nvdisasm` skips it and the adjudication; the words are the only thing nvdisasm sees in level 0). nvdisasm is
`C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.3\bin\nvdisasm.exe` (`NVDISASM` overrides).

## What is compared

| Level | Unit | Compared | Authority |
|---|---|---|---|
| L0 `text` | one exemplar word per form (see below) | our text against `nvdisasm -b SM86` | nvdisasm |
| L1 `opcode` | every instruction of the corpora | opcode coverage and mnemonic: our decode against nxnative's table entry (`InstName`, emitter name; `Names.Match` maps emitter names to SASS names) | nvdisasm text of an example word |
| L1 `predicate` | every instruction | guard register, negate and uniform-guard flag (bits 12..15) against `AmpereOp.Pred`, `PredInv`, `AmpereTable.HasUniformGuard` | nvdisasm |
| L2 `bit` | exemplar words x bits 16..127 | does flipping the bit change our output (text) / nxnative's IR (the emitter's Ryujinx operations, serialised)? A bit one side depends on and the other ignores is a disagreement. | nvdisasm text of the word and its flipped twin |

A *form* is an opcode plus our modifier list and operand kinds. Exemplars per form: the three words with the smallest hash, plus a greedy
cover so that every bit that is ever set in the corpus is set in at least one exemplar (about 100 words per form at most). Exemplar choice
depends on thread scheduling, so a rerun can list a few other bits for the rarest forms; the form-level rows do not change.

A level-2 bit is *live* when it takes both values among the corpus instructions of its opcode (counts in `n_a/n_b/n_c` are the instructions
with the bit set). A disagreement on a bit that has one value in every corpus instruction cannot affect these games and is closed automatically as
*unexercised*. Everything else is adjudicated by nvdisasm; see `disagreements.tsv` column `auto`.

Output (`work\xcheck`): `disagreements.tsv` (one row per disagreeing form or bit: counts per corpus, example word and hash, the flipped twin, both
nvdisasm texts, our text, nxnative's IR before and after, `auto` verdict, `status`, `note`) and `summary.txt`. `status` is `justified`
(automatic, from nvdisasm or the unexercised rule) or the entry of `verdicts.tsv` for the row's `key` (`justified`, `fixed`), which overrides
it. `open` rows are the ones nobody has looked at.

`--words <file>` (lines `lo hi`, hex) and `--ops 804,803,...|all` recheck chosen words, or every opcode (250 words each, about 8 minutes), against nvdisasm without the full run; they are how the fixes below were verified.

`--layout` writes `layout_words.tsv` (value distribution of every control-section word per corpus and stage), `layout_invariants.tsv`,
`layout_crosstabs.tsv`, `sph_words.tsv` and `sph_io.tsv` (SPH v4 fields and their agreement with the SASS); they are the evidence for
`../docs/nvn2_layout_xcheck.md`.

## Files

| Path | What |
|---|---|
| `nxref/NxnativeRef.csproj` | nxnative's Ryujinx shader project, compiled unmodified from the staged copy (assembly `Ryujinx.Graphics.Shader`, internals visible to `xcheck`) |
| `tool/` | the `xcheck` console: `Scan.cs` (L1, exemplars), `Perturb.cs` (L2), `Nx.cs` (drives `AmpereTable` and the emitters on a stub translator context), `Nvdis.cs`, `Report.cs`, `Names.cs`, `Layout.cs` |
| `scripts/setup_nxnative.ps1` | stages `refs\nxnative\dm\third_party\ryujinx\src\{Ryujinx.Common,Spv.Generator,Ryujinx.Graphics.Shader}` into `tools\shaders\third_party\nxnative` (git-ignored) |
| `verdicts.tsv` | manual verdicts, `key<TAB>status<TAB>note` |

## First full run (2026-11-04)

Title A 246,687,408 instructions in 99,012 blocks, title B 1,966,040 in 2,151, title C 3,665,720 in 5,705; 203 opcode rows, 1,315 forms, 1,912,824
exemplar words, 22,512 (opcode, bit) cells. nxnative's table has an entry for every opcode of the three corpora.

| | rows | fixed | justified |
|---|---|---|---|
| L0 text differs from nvdisasm (710 exemplar words) | 15 | 15 | 0 |
| L1 opcode: name differs / we could not decode | 21 / 2 | 3 | 20 |
| L1 predicate | 1 | 0 | 1 |
| L2 bit: nxnative depends on a bit we ignore | 151 | 3 | 148 |
| L2 bit: we depend on a bit nxnative ignores | 1,418 | 13 | 1,405 |
| total | 1,608 | 34 | 1,574 |

The 1,574 justified rows: 883 L2 rows whose bit has one value in every corpus instruction of the opcode (unexercised); 471 where nvdisasm prints the bit and
nxnative's emitter ignores it (memory order and scope, rounding and cache hints, register fields of unused second destinations); 191 where nxnative's emitter raises
on the stub translator context (IPA, BRX, RET need the decoded program) so its bit use cannot be seen, ours matching nvdisasm; 20 L1 rows that are only an emitter name
(nxnative registers UIMAD, BRXU, LDL, LDS, S2UR, TLD, TLD4, TXD, SULD, SUST on a shared emitter, DEPBAR, BSSY, BSYNC, BREAK, WARPSYNC and BMOV as no-ops, which our
front end does as well); 8 OUT.FINAL register-field rows (a non-RZ Rd is reserved, nvdisasm prints `???N`); 1 predicate row. The notes of the non-automatic ones are in
`verdicts.tsv`.

The 34 fixed rows are 13 decoder defects, fixed in 12 commits (`git log --oneline -- tools/shaders/ampere`):

| Defect | Rows / words | Translation output |
|---|---|---|
| constant buffer offsets >= 0x8000 print signed (c[0x9][-0x5a44]) | 219 words | none (text only) |
| fp32 immediates >= 1e9 print as %.20e | 1 | none |
| packed-half constant buffer operand prints `c[0x7] [0xf0]` | 7 words | none |
| P2R/R2P byte select is bits 76..77, R2P spells it on the source | 109 words; 1,656 / 1,271 c instructions with bit 76 / 77 set | R2P front end now reads the source suffix |
| **LEA.HI.SX32** had no third source and no sign extension | 28 words | **changed**: sign of Ra instead of 0 as the high word; hwcheck passes on the RTX 3080 |
| **TEX .F16.RN/.RZ** not decoded | 2 words | **changed**: front end rejects it (was a full precision sample) |
| **TMML** decoded like TEX (.EF, a P0 destination, no .LOD) | 3,045 c instructions | **changed**: no spurious P0 = true; .LOD accepted |
| SURED undecoded | 1 c instruction | none (front end still rejects it) |
| ISBEWR undecoded, ISBERD modifier order and unsigned offset | 634 c instructions | none (task/mesh stages are rejected before the front end) |
| memory order has 16 values (we had 5), access size 7 / LDC has no 128-bit | rare combinations | none (the front end ignores them) |
| I2FP rounding 1/2 are INVALID | 0 live | none |
| TLD/TXD reserved dimension codes, SURED/SULD INVALID6/7 | 0 live | none |
| FTZ with FMZ is INVALID3 | 0 live | none |

After the fixes `sassdecode --verify` is still 19,246,912 of 19,246,912 on the hash-locked oracle (it was 19,246,906 with 6 unknown opcodes), and `--ops all`
(37,194 words of the three corpora, 203 opcodes) differs from nvdisasm in 3 words, the R2P `.reuse` ordering, fixed afterwards and rechecked.

Limits: level 2 compares what the code depends on, not whether the same thing is done with it (a register field both sides read counts as agreeing); the emitters are run
on a stub translator context, so memory, texture and control-flow emitters that need a program or resources are only partly visible; exemplar choice depends on thread
scheduling; F64 immediates keep the old 2^63 threshold (not probed); hwcheck cannot run R2P, TMML or texture instructions (their operands do not fit its register window).
