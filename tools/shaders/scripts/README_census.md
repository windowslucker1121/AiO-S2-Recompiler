# Shader census

Instruction census of a game's Ampere (NVN2, SM86) shader corpus, decoded with our own decoder (`tools/shaders/ampere/decoder`).

```
toolchain\.venv\Scripts\python.exe -I tools\shaders\scripts\census.py <corpus dir> --game <name>
```

Output (default `<repo root>\work\audit\`): `census_<game>.tsv` and `census_<game>_summary.md`.

- TSV: `opcode`, `modifiers`, `count` (instructions over the distinct code blocks), `blocks` (distinct blocks containing it), `stage_blocks`,
  `decode` (ok / partial / undecoded), `flag` (tensor / raytracing / mesh), `nvdisasm` (name of an undecoded opcode, from NVIDIA nvdisasm run
  on one example word; needs the CUDA toolkit, `NVDISASM` overrides the path), `example_blocks` (smallest hashes, `--all-blocks` lists all),
  `example_addr/lo/hi` (first occurrence of an undecoded or partial row).
- Summary: counts per stage, tensor (HMMA/IMMA/BMMA/LDSM/MOVM), ray tracing (TTU*), mesh/task (ISBERD/ISBEWR and blocks whose control stage value
  is above 5), every undecodable opcode with count and example block, partially decoded rows.
- The first run builds `sassdecode` (`dotnet build`, seconds). `sassdecode --census` is the C# mode behind it; add `--jobs N` to the wrapper
  to change the thread count (default 6, below-normal priority). Title A's 99,012 blocks take about 2 minutes.
- Corpus layouts read: title A's (`extract_generic.py`: `unique.tsv`, `code/`, `ctrl/`) and the runtime dump (`NXRT_NVN_DUMP_MISSING`:
  `manifest.tsv`, `code_<h>.bin`, `ctrl_<h>.bin`; duplicate manifest rows are folded).
- Counts are per distinct code block, not per draw.

Other games' corpora:

- The platform EPD engine (BFSHA/BNSH, zstd): `tools\shaders\scripts\extract_generic.py --romfs <romfs> --out <dir>` handles the Shader/ folder, then the census on that folder.
- Unreal Engine (title I demo): shaders are in Oodle-compressed IoStore/pak; no extractor.
