# nxrecomp: AArch64 to C++ static recompiler

Turns the code of a Nintendo Switch program (NSO modules, or any static AArch64 ELF) into C++ that builds natively on Windows
x64 with clang-cl. The translation is done by deterministic code. There is no inline assembly and no
interpreter. It is the CPU half of the Metroid Prime Remastered PC port; nothing here
is specific to that game except `games/mpr/config.py`, which is a placeholder until the binary is dumped.

Licence: GPL-3.0-or-later (`LICENSE`). Borrowed code and its licences: `THIRD_PARTY.md`.

## What it produces

For each guest function `f_<address>(Ctx *c)`, working on the register context of `include/nxrecomp/ctx.h`. Guest memory is
host memory (the image is mapped at its guest address), direct branches are `goto`, calls are direct C calls, indirect calls go
to the runtime's `call_indirect`, jump tables become bounded `switch` statements, C++ landing pads become resume entries
`u_<address>(c, pad)`, and each module gets a descriptor `nxmod_<name>` (function table, dynamic symbols, resume table, unwind
tables). Output is sharded into a fixed number of translation units for the build.

**The generated-code ABI is `include/nxrecomp/ctx.h`** (include with `#include <nxrecomp/ctx.h>` and
`-I recomp/include`). The runtime implements what the header declares as runtime-provided: `call_indirect`, `host_import`,
`guest_trap`, `guest_unimpl`, `guest_svc`, `guest_mrs/msr`, `stx_diag`, `nx_catch_call`, and the three site hooks. The ABI
version is `NXRECOMP_ABI_VERSION` (3).

## Use

Python is `tools\.venv\Scripts\python.exe` at the repository root (3.13; packages in `requirements.txt`). Build tools: LLVM
23 clang-cl, CMake, Ninja, sccache; `scripts\nx_env.cmd <command>` runs a command with the MSVC environment set up.

    python -m nxrecomp lift  <module.nso|prog.elf> <outdir> [--game mpr] [--base 0x7100000000] [--name main] [--index N]
                             [--shards 16] [--jobs 8] [--patches FILE] [--sites FILE]
    python -m nxrecomp cover <module.nso|prog.elf> [--game mpr]      per-instruction-word translation coverage of .text
    python -m nxrecomp.funcs <module> --out functions.tsv             function boundaries and the evidence for each

In a CMake build (the game, or an oracle), `cmake/NxRecomp.cmake` gives `nxrecomp_add_module(<target> INPUT <file> ...)`, which
lifts as a build step and builds the result as a static library (flags: C++20, `-fno-strict-aliasing`, `-ffp-contract=off`,
`-mcx16`, optnone for functions over `optnone_insns` instructions; `NX_USE_LAUNCHER` adds a time-limit fallback). Presets:
`cmake --preset default`.

## How it is built

- `nxrecomp/image.py` loads a module (NSO, smo-style ELF, or static ELF) into one `Image`; `funcs.py` finds function
  boundaries (`.eh_frame` and sized symbols first); `eh.py` reads CFI and C++ LSDAs; `jumptables.py` resolves `br` tables;
  `program.py` lifts and writes the sources.
- Instruction lifting has two tiers. Tier 1 (`lift/a64.py`) decodes bit fields and is the reference where it applies. Tier 2
  (`lift/tier2/`) is Capstone-driven and covers what tier 1 does not: LSE atomics, crypto, the long tail of AdvSIMD, structure
  loads and stores. The files `x_*.py` there override or add handlers; the ported emitters beside them are kept as the base.
- `games/<name>/config.py` is the per-game seam (module list and bases, function overrides, extra entries, patches).

## Oracles (pass/fail checks under `oracle/`, hash-locked in `oracle/SHA256SUMS`)

| Oracle | Command (from `recomp/`) | What it proves |
|---|---|---|
| Differential test | `python oracle/difftest/difftest.py all` | every instruction shape of `corpus.json` (11,603 shapes of Cortex-A57 + LSE) gives identical registers, flags, TPIDR/FPCR and memory in Unicorn and in the lifted code, from random, integer-edge, colliding-operand and special-FP states: 565,061 of 565,061 tests |
| Mutation test | `python oracle/difftest/mutation.py` | All 22 planted lifter bugs are reported as different, and the unmutated copy passes |
| End to end | `python oracle/e2e/run_e2e.py` | 15 clang-built AArch64 programs (integers, switches, FP, NEON, crypto, LSE, exclusives incl. 128-bit, TLS, virtual calls, tail calls, C++ exceptions) lifted, built for x64 and run: every register and every writable byte equals Unicorn's |
| Site lists | `python oracle/hooks/test_hooks.py` | patches, inline callouts, function hooks and dynamic immediates behave and count correctly |
| Known answers | `python oracle/ka/run_ka.py` | 108 cases of the rEden / dynarmic A64 tests (through ScarletNX), no emulator involved: 89 pass, 0 fail, 7 skipped, 6 need FPSR flags and 5 need FPCR (both documented gaps) |
| Corpus | `python oracle/difftest/gen_corpus.py` | regenerates `corpus.json` (deterministic) |

Lifting and compiling the game itself (`scripts\build_game.cmd <id>`) is described in `docs/S1b.md`. Gaps that remain are listed with numbers in `docs/GAPS.md`. The S1 log is `docs/S1.md`, numbered bug notes are in
`docs/debug/`.
