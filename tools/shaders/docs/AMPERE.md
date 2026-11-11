# Title A shaders: container, NVN2 control section, Ampere SASS

First stage of the Ampere (platform 2) translator: where the game's shaders live, how a shader is laid out, and the
nvdisasm reference. Data: title A v2.0.1a (update 655360). Everything below was read from the game's own files unless
marked *inferred*. Game data and the corpus stay in `work\shaders\` (gitignored).

## 1. Archives (KTGL RDB)

`romfs\motor_asset_re\KIDS_PLATFORM_OUNCE\`: `root.rdb`/`system.rdb` (index, magic `_DRK0000`, entries `IDRK0000` that
describe files), `root.rdx`/`system.rdx` (u16 index → fdata KTID), 12 `0x<KTID>.fdata` and 4 `data\0x<KTID>.file`.
No new RE was needed: the layout matches the notes in seedds/nxnative `dm/gfx/tools/shader_split.py` (MIT) for
Tools written for two other titles of the same engine (eArmada8/yumia_fdata_tools, GPL-3.0, and
wattostudios GameExtractor `Plugin_RDB_DRK` cover the same family; code was not copied from them).

- `.fdata`: `PDRK0000`, u64 0x10, then IDRK entries. `.file`: IDRK entries only.
- IDRK entry: `IDRK0000`, u64 entry length, u64 stored length, u64 inflated length, u32 (1), u32 file KTID,
  u32 type KTID, u32 flags, parameter words; the stored data is the entry's last *stored length* bytes.
- Every entry in title A has flags 0x400000: chunks of {u16 stored length, 8 bytes, zlib stream}, a zero length ends,
  each chunk inflates to ≤ 0x4000 bytes. (0x500000 = LZ4 chunks, 0x100000 alone = u32-length zlib chunks; not seen here.)

Survey (`.weaver\ktgl_rdb.py survey`, 5,574 type/magic rows): shaders are in type KTID **0x7bcd279f** = `_S2G7000`
(G2S container, 26,137 entries, 8.5 GB inflated) and **0x20a6a0bb** = `_DOK0000` (system resources, 4,144 entries, a few
of them hold shaders). Others: `GT1G` textures (0xafbec60c), `_M1G` models, `_A2G` animations, etc. `main` also embeds
**160 GLSLC outputs** (the engine's built-in shaders: these are the first ones the runtime sees, see §5).

## 2. GLSLC output (NVN2)

Each shader stage is a whole NVN2 GLSLC output (a G2S container holds a material's variations, several outputs each).
Same idea as platform 1, but the header grew by 0x18 bytes:

| Offset | Field |
|---|---|
| +0x00 | u32 magic 0x19866891 |
| +0x08 | u16 0x15, u16 0x22 (GLSLC version; all 99,012 outputs) |
| +0x5c | u32 output length |
| +0x60 | u32 offset of first section data (0x308 seen) |
| +0x64 | u32 section count (always 2) |
| +0xa8 + 0x98·i | section header: u32 size, u32 data offset (from output), u32 type |
| GPU code header +0x30/+0x34/+0x38/+0x3c | control offset, code offset (from section data), code length, control length |

Sections seen: type 0 (GPU code: control section, then code area) and type 3 (*inferred* reflection, as platform 1's
GLSLC section type 3; not parsed yet: it is where uniform block / sampler names and bindings should be).

## 3. NVN2 control section (0xd8 bytes + optional tail)

Statistics over all 99,012 distinct code blocks; confirmed on the 12 control sections the runtime dumped from
`nvnProgramSetShaders` (`work\shader_dump\nvn2_ctrl\`, all 12 found in the corpus: main's built-in shaders).

| Offset | Field | Evidence |
|---|---|---|
| +0x00 | u32 magic 0x98761234 | all |
| +0x04 | u32 0xc9 (201) | all; *inferred* version major |
| +0x08 | u32 0xb / 0x11 / 0x4 | 3 values; not the stage; *inferred* version minor or compiler flavour |
| +0x0c | u32 0x000e0170 | all |
| +0x18 | u16 0x15, u16 0x22 | = GLSLC version |
| +0x1c | u32 0x00288000 | all |
| +0x20 | 16 bytes hash | +0x28..+0x2f is the same for every copy of one code block; +0x20..+0x27 can differ (31,878 cases) |
| +0x30 | u32 control section length (0xd8 base) | = GPU code header control length |
| +0x38 | u32 program length: SPH + instructions, a multiple of 0x80 | ≤ +0x40; the rest up to +0x40 is zero padding |
| +0x3c | u32 constant data length (0, 0x10, 0x40, 0xc0, 0x400...) | |
| +0x40 | u32 constant data offset in the code area (0x100-aligned) | +0x40 + +0x3c ≤ code size in every block |
| **+0x44** | **u32 code area length** (what to hash / upload) | = GPU code header code length in all 99,012 |
| +0x4c | u32 register count (*inferred*: 0x6..0xff, typical 0x40-0x80) | |
| +0x50 | u32 local memory bytes per thread (0, 0x200, 0x400, 0xc00, 0x1800...) | non-zero exactly when the SASS has LDL/STL (all 3 corpora, every sampled block; for graphics also the SPH's local memory low size is non-zero exactly then); was documented as shared memory until the cross-check, see nvn2_layout_xcheck.md. Compute: +0x68 = +0x50 / 32 |
| +0x64 | u32 compute shared memory bytes (0, 0x400, 0xb00, 0xf00, 0xbc00...) | non-zero exactly when the SASS has LDS/STS/ATOMS, and every static LDS/STS/ATOMS address of the 98 compute blocks that have one ends within it. (tes/tcs: GL winding, next row) |
| **+0x56** | **u16 stage: 0 vs, 1 ps, 2 gs, 3 tcs, 4 tes, 5 cs** | agrees with the SPH shader type of every graphics block |
| +0x58/+0x5c/+0x60 | u32 compute work group x, y, z (8,8,1 most common) | compute only; graphics use these for other flags |
| +0x58/+0x5c/+0x64 (tes) | u32 GLSL input layout as GL enums: domain (GL_TRIANGLES 4, GL_QUADS 7, GL_ISOLINES 0x8e7a), spacing (GL_EQUAL 0x202, GL_FRACTIONAL_ODD 0x8e7b, GL_FRACTIONAL_EVEN 0x8e7c), winding (GL_CW 0x900, GL_CCW 0x901) | all 527 tes: 4 / 0x8e7b / 0x901 (triangles, fractional odd, ccw); the SASS reads gl_TessCoord.xy and computes z = 1 - x - y (triangle domain). gs: +0x58 = input primitive (0 points ×7, 4 triangles ×2). tcs: 4 / 0x202 / 0x901 in all 50 (*inferred* defaults). The harness's StaticGpuAccessor turns the tes values into Ryujinx's TessPatchType/TessSpacing/TessCw |
| +0x98 | u32 = control length | |
| +0xa4 | u32 0x01000000 (graphics) | |
| +0xa8..+0xb8 | compute-only words (*inferred* QMD-ish settings) | |
| +0xbc/+0xc4/+0xc8 | u32 = control length, or 0xe0 when a tail exists | |
| +0xd8 | tail (control length > 0xd8): u32 table offset (0xe0), u32 table length | 8% of blocks |
| table | u32 count, then count × {u32 code offset, u32 word count, u32 0x00010000, u32 running word total} | purpose unknown (*inferred* patch points) |

No constant-buffer or resource binding table in the control section: bindings are by convention in the code
(`c[0x3]` is the uniform block in the shaders looked at; textures use bound `TEX` with constant handle slots, e.g.
`TEX.SCR R20, R4, R0, R1, 0x2, 0x10, 2D`). The binding contract has to come from the reflection section and the code.

Runtime: for `read_control`, code size = **+0x44**, stage = **u16 +0x56**, program end = +0x38, constants at +0x40/+0x3c.
Platform 1's +0x6f8/+0x704/+0x714 offsets do not apply (the control section is 0xd8 bytes, not 0x880+).

## 4. Code area and SM version

- Graphics stages: 0x80-byte **SPH v4** (open-gpu-doc `classes/3d/clc797sph.h`, Ampere; word 0: SphType 1 VTG / 2 PS,
  version 4, ShaderType bits 13:10) at +0, instructions from +0x80. Compute: no SPH, instructions from +0.
- Instructions are 128-bit Volta-family encodings, ending with `EXIT` and a `BRA` self-loop, padded with zero words.
- **SM version: Ampere, SM 8.6 family.** nvdisasm 13.3 (`-b`) on a 1-in-200 hash-order sample (496 blocks,
  1,174,168 instructions): SM86, SM87 and SM88 give byte-identical output with 0 illegal instructions; SM89 differs only
  in spelling `F2FP.PACK_AB`/`MERGE_C` as `F2FP.F16.F32.*` (121 blocks); SM75 (Turing) rejects `F2FP.PACK_AB/MERGE_C`
  (Ampere additions); SM90 and SM100+ decode some of the same words differently (`MUFU.RSQ.BF16`, a different `BRA` target)
  or not at all. nvdisasm cannot tell 8.6/8.7/8.8 apart; the oracle uses **SM86**. (T239's exact compute capability is
  not public; *inferred* from the encodings only.)

## 5. Corpus and oracle (work\shaders\, not committed)

`tools\shaders\ampere\extract_corpus.py --romfs <romfs> --main work\nvn2\main.elf --out work\shaders\corpus`:
213,329 shader stages, **99,012 distinct code blocks** (first 16 hex of SHA-1 of the code area, +0x44 bytes):

| Stage | Occurrences | Unique | Code bytes (unique) |
|---|---|---|---|
| ps | 138,471 | 90,917 | 3.87 GB |
| vs | 66,217 | 6,123 | 61 MB |
| tes | 4,346 | 527 | 5 MB |
| cs | 3,283 | 1,386 | 53 MB |
| tcs | 1,001 | 50 | 0.3 MB |
| gs | 11 | 9 | 27 KB |

`tools\shaders\ampere\oracle\sass_oracle.py` (hash-locked, `SHA256SUMS`): per block `nvdisasm -b SM86 -hex` over
[SPH end, +0x38) minus trailing zero words, base address 0 = first instruction; per-instruction fallback marks words
nvdisasm rejects as `.ILLEGAL`. Writes `<hash>.sass`, `index.tsv`, `census.tsv`, `census_full.tsv`. nvdisasm takes
about a second per average block, so the reference set is **all non-pixel blocks + every 20th pixel block in hash
order** (`--stage-every ps:20`); the full corpus is the same command without that option (hours at 4 jobs).

## 6. SASS decoder (`tools\shaders\ampere\decoder`)

C# library `AmpereDecoder` (shared model `SassInst.cs`; `SassDecoder.Decode(lo, hi, addr)` / `DecodeBlock(code, start, end)`;
`SassText.Format` prints nvdisasm's spelling) and console `sassdecode` (`tool\`). Build:
`dotnet build tools\shaders\ampere\decoder\tool\sassdecode.csproj -c Release` (output in `tools\shaders\build\`).

- Dispatch is a table lookup on the low 12 opcode bits, then the 9-bit ALU opcode (form in bits 9..11: 1 R,R,R; 2 R,R,imm;
  3 R,R,c; 4 R,imm,R; 5 R,c,R; 6 R,UR,R; 7 R,R,UR). Handlers are grouped by unit (`SassDecoder.Float/Int/Half/Conv/Mem/Tex/
  Ctrl/Uniform.cs`); uniform-datapath ops reuse the vector handler with UR/UP register files.
- Provenance tags in the code: **[NAK]** = inverted from Mesa NAK `sm70_encode.rs` (MIT); **[probe]** = found by flipping bits
  of corpus words and reading nvdisasm (`ampere\probe\sass_probe.py`, findings in `probe\findings\*.tsv`, tables generated
  by `probe\gen_tables.py` into `SassDecoder.Probed.cs`); **[oracle]** = spelling read from the oracle text.
- Spelling rules learnt from nvdisasm: `.reuse` follows the printed source slot (A bit 122, B 123, C 124; FADD/HADD2 second
  source uses C) and is never printed on RZ or texture operands; fp32 immediates are C `%.20g` (|v| ≥ 2^63: `%.20e`), INF/NaN
  as `+INF `/`-QNAN ` (trailing space); fp16 pairs print the high half first; scalar selectors (`.H1`) sit inside `|..|`,
  packed ones (`.H0_H0`) outside; IMAD aliases MOV/SHL/IADD (SHL for 2^1..2^30 except 2^16).
- `xcheck` (`tools/shaders/xcheck`) compares the decoder with nxnative's Ampere front end and with nvdisasm over the title A, title B and title C corpora (README there: results, fixes); `nvn2_layout_xcheck.md` does the same for the NVN2 control section and the SPH.
- `sassdecode --verify <oracle> <corpus>` decodes every reference block from the corpus and compares each line with the
  oracle (whitespace normalised). `--corpus <corpus>` decodes every unique block. `--dump <code.bin> <ctrl.bin>`.
- `Sched` = hi >> 41 (stall 105..108, yield 109, write barrier 110..112, read barrier 113..115, wait mask 116..121, reuse
  122..125). `DstCount` = leading written operands (predicate destinations included; barrier/PR/special destinations count).

## 7. Front end: global memory

- LDG/STG/ATOMG/RED map to storage buffers through Ryujinx's GlobalToStorage: the address is traced back to a c[0] word, which in NVN2 compute is the per-program table (§3: header 0x10, then 0x10 per uniform block, then 0x10 per storage buffer {addr, size}; the runtime's `apply_nvn2_compute_table` turns the c[0] offset into the NVN binding). The front end emits each vector element's address as `(Ra + URb) + (imm + 4n)`, one constant add, because GlobalToStorage peels only one constant before looking for the c[0] base; two adds (`+imm` then `+4n`) left 7 compute shaders (cbabef05 and 6 SDF/heightmap ones) without a storage buffer, so their loads read 0 and stores were dropped.
