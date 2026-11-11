# ZBIC: zstd-compressed NSO segments

Firmware 22.0.0 added a second compression for NSO0 segments: NSO header flag bit 7, `UseZbicCompression`, "use zstd-based
compression instead of LZ4" ([switchbrew NSO0](https://switchbrew.org/wiki/NSO0), edited 2026-03-26). Bit 6 of the same word,
`ExecuteOnlyMemory` (20.0.0+), is unrelated to compression and does not matter to a recompiler.

## Where it applies

- Flags bits 0-2 still say which segments (text, ro, data) are compressed. Bit 7 picks the codec for all of them: set = ZBIC, clear =
  LZ4 block. A segment whose bit 0-2 is clear is stored as is, as before. Compressed sizes at +0x60/+0x64/+0x68 are the ZBIC
  stream lengths; the segment hashes at +0xA0 are over the decompressed bytes, as for LZ4.
- Each compressed segment is one ZBIC frame (Atmosphère decodes a segment with one `ZSTD_decompressDCtx` call, so several frames
  back to back would also work; our decoder accepts that too).
- Atmosphère's loader (`stratosphere/loader/source/ldr_process_creation.cpp`, read 2026-11-04) allows bit 7 only on `main`, and only
  when `rtld` and `sdk` are both present. It maps `main` with room from its start to the end of the whole module area for the
  in-place decode (`ZSTD_decompressionMargin`); a decoder writing into its own buffer needs no margin.
- None of our dumps use it: every NSO of title A 2.0.1a, title B (base and 458752), title C and the title I demo has
  flags 0x3f (checked 2026-11-04). Which titles do is not known to us.

## The format

ZBIC is zstd (frame format of RFC 8878, the zstd 1.5.7 code base) with two differences, both visible in the only public source of the
codec (hexkyz/zstd, commits `a7cb12b` "implement ZBIC" and `02a8f10` "ZBIC improvements", 2026-03-26 / 04-02; copied unchanged
by Atmosphère 1.12.0+ `libstratosphere/source/util/zstd.inc`, kinnay/zbic and alula/beetool):

1. **Frame magic** `5A 42 49 43` ("ZBIC", little-endian 0x4349425A) in place of zstd's `28 B5 2F FD`. Skippable frames
   (0x184D2A5?) and dictionaries keep their magics.
2. **FSE table descriptions are coded with binary interpolative coding (BIC)** instead of zstd's variable-bit NCount header. This is
   the header that precedes every FSE-compressed table: the literal-length, match-length and offset tables of a sequences section
   (when its mode is `FSE_Compressed`) and the FSE-compressed Huffman weights of a literals section. Everything else (block
   headers, Huffman streams, sequences, repeat offsets, checksums, window rules) is plain zstd.

### BIC table description

The decoder is `FSE_readNCount_bic` (entropy_common.c of the fork). Layout, as read from it:

- Byte 0: bit 7 = `useLowProbCount`, bits 0-6 = `n`, the number of following bytes. The description is `n + 1` bytes long (the
  reader returns that as the header size).
- Bytes 1..n are one little big number, read from the **last** byte backwards (byte n is the most significant): the decoder keeps a
  64-bit accumulator and pulls in the next lower byte whenever the accumulator's upper 32 bits drop below 0x100.
- Out of that number come, by repeated division (mixed radix, most significant first):
  - `maxSymbolValue - 1` = value mod 52, so maxSymbolValue 1..52 (the reader fails when it exceeds the caller's limit for that
    table);
  - `tableLog - 5` = next value mod 8, so tableLog 5..12;
  - `r` = next value mod 2^tableLog; the last cumulative entry is `r + 1`, or `r + maxSymbolValue + 2` with `useLowProbCount`;
  - then the cumulative count table: `C[0] = 0`, `C[P] = that last entry`, where P is the smallest power of two above
    maxSymbolValue, and the entries between are filled by binary interpolative decoding. A fixed table of 256 (middle, first, last)
    index triples (`BIC_table`, 0x300 words) gives the order; for a given P the decoder walks its last P triples. For each triple:
    if `C[first] == C[last]`, every entry between them gets that value; otherwise `C[middle] = C[first] + (value mod (C[last] -
    C[first] + 1))` and the value is divided by that range.
- The normalized counts are the differences `C[s+1] - C[s]`, each minus 1 when `useLowProbCount` is set (so -1, the "less than
  one" probability of zstd, can be expressed as a difference of 0). The absolute values must add up to 2^tableLog.
- The reader fails (corruption) when bytes are left over, the counts do not add up, or P would exceed 0xFF.

`useLowProbCount` mirrors the zstd compressor's choice for that table (in the fork's compressor: on for sequence tables of a block
with 2048 or more sequences, off for Huffman weights), so it is a property of the encoder, not of the frame.

## Our implementation

- `runtime/third_party/zstd_zbic/zstddeclib.c`: the decoder only, amalgamated unchanged from hexkyz/zstd `02a8f10` with zstd's
  `combine.py` (BSD-3-Clause; provenance and hashes in the README there). Not a reimplementation: the BIC reader has no second
  independent source to check one against, so we take the one every other project uses.
- `runtime/src/loader/zbic.cpp`: compiles it in one translation unit with `ZSTD_ZBIC_SUPPORT 1` and every zstd symbol `static`, so it
  can sit next to a plain zstd in the same binary (`tools/extract` links both). `zbic_decompress()` decodes one segment and succeeds
  only on exactly the expected size.
- `runtime/src/loader/nso.cpp`: segments with their compressed bit set are decoded with ZBIC when flag bit 7 is set, LZ4 otherwise.
- `tools/extract` (`nx_plain_nso <exefs> <out>`): writes a plain NSO (segments stored, compressed sizes = sizes, bits 0-2
  and 7 cleared, hash bits and hashes kept) so the lifter (`recomp/nxrecomp/nso.py`, which refuses bit 7 with a pointer here) and
  any LZ4-only tool read it. The launcher runs it at Set up when the dump's ExeFS has a ZBIC module (`tools/extract/nx_plain_nso.exe` in the package).

## What was checked

- Round trip (2026-11-04, scratch only): data compressed with kinnay/zbic's encoder (`zstd.c` from Atmosphère, `ZSTD_ZBIC_SUPPORT 1`)
  at levels 1, 3, 9 and 19 and sizes from 1 byte to 12 MB (a compiler binary and a C source), decoded by `zbic_decompress`: 39 of 40
  streams decoded bit-exactly. The 40th (12 MB, level 3) is rejected by Atmosphère's own decoder in the same way ("Data corruption
  detected"), so that encoder can write a table description its reader refuses; our decoder agrees with the reference reader on
  every stream. The platform's encoder is a different program; no stream of it was available.
- `tools/extract/tests/test_extract.cpp`: a synthetic three-segment ZBIC NSO converts to the plain NSO with the expected segments, and a
  damaged frame is refused.
- Not checked: a real ZBIC NSO from a game. None of our dumps has one.
