# S1b (Phase 3: lift and compile the game)

S1b, the Metroid Prime Remastered recompilation milestone, lifts `main`, `sdk`, `subsdk0` (and `rtld`) of the v1.0.0 dump with the S1 lifter and compiles every translation unit for x64. Linking against the runtime is not part of it. All numbers are from this machine (i9-12900K, 64 GB) while other projects were using most of its CPU and about 40 GB of its RAM, so wall times are pessimistic.

## Rebuild

    scripts\build_game.cmd mpr                              configure + lift + compile + archive (work\build\mpr), 20 compile processes
    scripts\nx_env.cmd cmake --build work\build\mpr --target mpr_link_check      (from the repo root; link check, see below)

`games/mpr/CMakeLists.txt` is the project: one `nxrecomp_add_module` per module (index, base and shard count fixed there), static libraries `mpr_main`, `mpr_sdk`, `mpr_subsdk0`, `mpr_rtld`, target `mpr_modules` for all four. Generated sources are written to `work\lift\<module>`, build output to `work\build\mpr`, the compiler cache (its own sccache server on port 24339, so a shared one is not touched) to `work\build\sccache`. Nothing generated is committed. Lifting alone: `python -m nxrecomp lift game\exefs\main work\lift\main --game mpr --index 0 --name main --base 0x7100000000 --shards 96`.

Libraries, not DLLs: the runtime is one exe that already owns the guest address space; a DLL per module would need an import library for the exe's `call_indirect` / `host_import` and would hide cross-module calls behind PE imports for no gain. Static archives link only what `nxmod_<name>` references, which is everything.

## Modules

| index | module | base | input | note |
|---|---|---|---|---|
| 0 | main | 0x7100000000 | NSO, text 0x102a390 | the game |
| 1 | sdk | 0x7500000000 | NSO, text 0x58f170 | Nintendo SDK, libc, libc++abi |
| 2 | subsdk0 | 0x7600000000 | NSO, text 0x33f8e0 | multimedia (movie::, stagefright, mkv, FDK-AAC, opus) |
| 3 | rtld | 0x7700000000 | NSO, 7 KB | process entry; lifted so the runtime may use it; an HLE loader can do its job instead |

Bases come from `games/mpr/config.py` (main from the plan, the rest from `runtime/include/nxrt/memory.h`: SDK modules from 0x7500000000, the heap view starts at 0x7800000000). They are baked into the generated code (adrp/adr results, call targets); changing one means re-lifting.

## Measurements

| | main | sdk | subsdk0 | rtld |
|---|---|---|---|---|
| instruction words in .text | 4,237,540 | 1,457,244 | 851,512 | 1,439 |
| functions lifted | 86,088 | 38,729 | 13,642 | 43 |
| evidence: `.eh_frame` FDE (E) | 85,670 | 29,785 | 9,843 | |
| evidence: sized symbol (S) | 82,761 (all also E) | 22,392 (22,368 also E) | 8,307 (8,292 also E) | |
| no E or S (bl / reloc / tail target, or gap) | 418 | 8,913 (41 of them gap code) | 3,785 (1 gap) | |
| E-versus-S end disagreements | 0 | 0 | 0 | |
| `br xN` (not PLT stubs) | 5,958 | 2,071 | 525 | |
| ... jump tables resolved | 881 | 622 | 335 | |
| ... indirect tail calls (debug note 007) | 5,076 | 1,445 | 189 | |
| ... dispatch fallback (target reloaded from the stack, kept conservative) | 1 | 4 | 1 | |
| functions above 20,000 instructions (optnone) | 0 | 0 | 0 | 0 |
| largest function (instructions) | 7,364 | 6,144 | 6,144 | |
| instructions not translated | 0 | 0 | 0 | 1 (a data word between `bl` and its `ldr [x30]`, never executed) |
| FDEs with a C++ LSDA | 0 | 511 | 0 | |
| LSDA call sites / landing pads / catch-capable functions | 0 | 2,810 / 1,612 / 146 | 0 | |
| typed `catch` clauses | 0 | 0 (all catch-all or cleanup) | 0 | |
| exception specifications (filter < 0) | 0 | 13, all `throw()` | 0 | |
| generated C++ (shards + module file) | 296 MB + 28 MB | 101 MB + 10 MB | 56 MB + 3 MB | 0.1 MB |
| translation units | 96 + 1 | 32 + 1 | 20 + 1 | 1 + 1 |
| compile CPU sum, cold | 1,207 s | 388 s | 265 s | 1 s |
| slowest unit | 16.5 s | 18.8 s | 21.6 s | |
| library | 147 MiB | 47 MiB | 22 MiB | 50 KiB |

Evidence for the 92,784 named `main` functions: 82,761 distinct addresses carry a symbol (the rest are aliases, C1/C2 constructors and the like), and every one of them also has an FDE. `main` has no landing pads at all (built without C++ exceptions); all exception code is in `sdk`. `main` never reads TPIDRRO_EL0; `sdk` does 65 times, TPIDR_EL0 5 times.

Build (20 compile processes, ninja, clang-cl 23.1.4, `/O1`):

| run | wall | CPU | peak RAM |
|---|---|---|---|
| cold (empty sccache), 162 steps | 123 s | 1,789 s | job 6.5 GB committed, largest process 1.06 GB; system in use rose from 49.7 to 57.3 GB with the other load |
| incremental after the ABI 3 header change (every unit recompiled, sccache misses) | 115 s | 1,782 s | job 6.5 GB, largest process 1.05 GB |
| warm (new build dir, every compile a cache hit) | 36 s, of which the lifts take 31 s | 165 s | job 2.8 GB |
| lift of all four modules (inside the build) | 31 s (main, 24 lifter processes) | | under 3 GB |
| link check (`mpr_link_check.exe`, 130 MB) | 6.6 s | 4 s | 0.3 GB |

A 5.5 MB / 88k-instruction unit compiles in 20 s with 0.46 GB, so the unit size (45k instructions) was chosen for balance, not for memory; the fixed cost of a unit (cxx.h plus the `funcs.h` of the module) is 2 s. No unit comes near the 180 s limit of `cmake/nx_cc_launcher.py`; the launcher is not enabled. `/O1` is as in S1's `NX_OPT`; nothing was tuned.

## Lifter changes the game forced (debug notes)

- 007: unresolved `br xN` are indirect tail calls (vtables, function pointers); they no longer produce a dispatch over every instruction of the function. 6,710 of them across the modules; the 6 whose target register is reloaded from the stack frame (`str x2, [x29, #0x18]` ... `ldr x2, [x29, #0x18]; br x2`) keep the old dispatch, because at -O0 that is how a spilled jump-table target looks.
- 008: code that no FDE, symbol or branch reaches (hand-written SVC stubs, FPCR/FPSR accessors) is now cut into functions (evidence `G`): 41 in `sdk`, 1 in `subsdk0`.
- 009: ABI 3 (below).

## ABI 3 (changes needed by S3b, all in `include/nxrecomp/ctx.h`)

1. Version export: each `module_<name>.cpp` defines `extern "C" const uint32_t nxmod_<name>_abi = NXRECOMP_ABI_VERSION;` next to `nxmod_<name>`. It is compiled from the same `ctx.h` the library was built with, so a stale prebuilt library carries its old number and the runtime can refuse it.
2. `DynSym.value` is a guest address, for every input kind: defined symbols get the module base added to the NSO's module-relative `st_value` (static ELFs were absolute already); undefined symbols have 0. Two raw exceptions: STT_TLS symbols (a TLS offset) and SHN_ABS symbols (a number). The runtime no longer needs to rebase.
3. `NxEh.types` are final addresses. `R_AARCH64_RELATIVE` slots get the module base added, slots bound to a symbol the module defines resolve to it, 0 stays catch(...). A typeinfo the module only imports (relocation against an undefined symbol) is 0 in `types` and the symbol's index in the new parallel array `NxEh.type_syms` (`-1` otherwise); the runtime resolves that name like an import. (In the game every typed table entry is null: the SDK's catches are catch-all, so this path is exercised only by the oracle programs.)
4. Exception specifications: a negative `NxAction.filter` is `-(index + 1)` into `NxEh.specs`, where a 0-terminated list of type filters starts (each selects `types[fde.type_first + k - 1]`). `sdk` has 13, all empty (`throw()`).

`NXRECOMP_ABI_VERSION` is now **3**. `NxEh` gained two trailing pointers, so code that only reads the old fields keeps working; the version number still has to change because the meaning of `types` and `DynSym.value` did.

## What the runtime needs to know

- Entry: the process starts at `rtld` offset 0 (`b +8`); its dynamic imports are `nn::init::Start(unsigned long, unsigned long, void(*)(), ...)` and `nn::os::detail::UserExceptionHandler`, both defined in `sdk`. `sdk` defines `main` (0x7500566c48) and imports `nninitStartup` (0x7100000470) and `nnMain` (0x71000004a0) from `main`. Each module has `_init` at +0x30 and `_fini` at +0x170 (DT_INIT / DT_FINI).
- Constructors: DT_INIT_ARRAY has main 1,445 entries at 0x7102126c10; sdk 112 at 0x7500c90038; subsdk0 44 at 0x760057e210. DT_FINI_ARRAY has sdk 2 at 0x7500c903b8, subsdk0 3 at 0x760057e370. The entries are pointers fixed up by RELATIVE relocations.
- Relocations: RELATIVE main 86,883, sdk 10,651, subsdk0 1,667. ABS64 against symbols sdk 13,231, subsdk0 12,718 (data pointers to symbols, mostly vtables). GLOB_DAT main 5, sdk 1,149, subsdk0 1,033. JUMP_SLOT (PLT) main 417, sdk 8,808, subsdk0 3,782. No TLS relocations and no PT_TLS in any module: thread-local data is the SDK's own scheme over TPIDRRO_EL0 (the lifter keeps `tpidrro` separate from `tpidr`).
- Imports: `main` imports 422 symbols. 377 are defined in `sdk`, 41 in `subsdk0`, 4 are linker symbols (`__rel_dyn_start`, `__rel_dyn_end`, `__rel_plt_start`, `__rel_plt_end`: the loader defines them as the addresses of the module's RELA and JMPREL tables). `sdk` imports 13 (8 from `main`: `nnMain`, `nninitStartup`, `__nnDetailNintendoSdkRuntimeObjectFile` and the allocator `malloc`/`free`/`realloc`/`calloc`/`aligned_alloc`, which the game provides; the 4 linker symbols, `nnosGetDefaultUserExceptionStackForApplication`, an optional application hook). `subsdk0` imports 657 (643 from `sdk`, 10 from `main`, 4 linker). So the nn::, nvn and libc names the plan counted as imports are lifted `sdk` code; what the runtime answers is the 76 SVC sites (`guest_svc`, all in `sdk`) plus any function it overrides by name. The per-module import lists with their provider are written by `modfacts.py --imports-out` (local test script).
- `host_import` is generated for PLT stubs whose symbol is undefined in the module: main 417, sdk 9, subsdk0 520, rtld 2. The id is `NX_IMPORT_ID(module index, DynSym index)`.
- System registers: `guest_mrs` is called with sysreg 0xdf01 (CNTPCT_EL0) 33 times, only in `sdk`. FPCR is written only in `sdk` (3 sites, all in the FPCR/FPSR accessor stubs that have no direct caller and were found by debug note 008).
- Calls with no target: no `call_indirect` with a constant address was generated (every direct branch lands in its module's `.text`); `guest_unimpl` appears once, in `rtld`, on a data word. 52 functions of `main`, 19 of `subsdk0` and 1,704 of `sdk` end in `guest_trap("fell off the end")` after a call: those callees do not return (aborts), or the next instruction is padding.
- Exceptions: only `sdk` has unwind data with LSDAs (above). `main` and `subsdk0` have CFI rows only (needed to unwind through them).
- Build facts: NSO `MOD0` extension words and `DT_NEEDED` (a build-server path to `multimedia.nss`, no use) are not needed by the generated code.

## Link check

`mpr_link_check` (`games/mpr/link_check.cpp`) links the four libraries with stubs for the runtime-provided functions of `ctx.h` and prints each module descriptor. It proves that every `f_<addr>` / `u_<addr>` the generated code references is defined exactly once. It is not a runtime and runs nothing of the game.

## Tools (local test scripts)

`lift_stats.py` (per-module discovery, evidence, jump tables, LSDA, sizes, gaps), `br_classify.py` (how unresolved `br` targets are produced), `modfacts.py` (layout, dynamic tags, init arrays, relocations, imports and providers), `ninja_times.py` (compile times per module from `.ninja_log`), `peakmem.py` (runs a command in a job object: wall, CPU, peak memory, free disk).
