# Known gaps in the lifter

Numbers come from the oracles at the commit that contains this file: `oracle/difftest/corpus.json` (11,723 in-scope shapes, 618
mnemonics), 565,061 differential tests (the 11,603 shapes before the LRCPC additions; the 120 new ones add 1,270, all identical), 15 end-to-end programs at two optimisation levels (30 runs), 108 known-answer cases.

## Behaviour that differs from the console

| Gap | What happens | Why it is acceptable now / what would close it |
|---|---|---|
| FPCR is stored, not honoured | `msr fpcr` keeps the Cortex-A57 bits (0x07f70000) and `mrs fpcr` reads them back; rounding mode, flush-to-zero (FZ), default-NaN (DN) and AHP do not change the arithmetic, which is x86 SSE with round-to-nearest-even, no flushing, and ARM NaN propagation (DN = 0). 5 of the 108 known-answer cases set FPCR and fail for exactly this reason (listed by `run_ka.py` as FPCR-not-honoured). | None of the upstream projects' games write FPCR; title E is expected not to. Closing it: map RMode and FZ onto MXCSR at `msr fpcr` (x86 has no DN). Check with the first `cover` / grep for `msr fpcr` on the real binary. |
| FPSR cumulative flags are not tracked | IOC, DZC, OFC, UFC, IXC, IDC and QC (saturation) are never set; `mrs fpsr` returns what `msr fpsr` stored (masked to 0xf800009f). 6 known-answer cases that read QC are reported as FPSR-not-modelled; the differential test does not compare FPSR. | Games rarely read the sticky flags. Closing it: set the flags in the exact FP helpers and the saturating NEON helpers. |
| Exclusive monitor is a compare-and-swap | `ldxr` remembers (address, value); `stxr` succeeds iff the address matches and the memory still holds that value. Consequences: (1) ABA: another thread changing the value and changing it back does not make `stxr` fail; (2) the reservation granule is not modelled (a store to a neighbouring address in the granule does not clear the monitor); (3) a store of the same value by another thread does not break it; (4) `stxr` of a different size than the `ldxr` is not detected. 128-bit pairs use `cmpxchg16b` (`-mcx16`, 16-byte aligned) and have the same gaps. | This is the standard approach of all four reference projects and passes every single-thread test (88 exclusive sequences, 2 programs). ABA-sensitive lock-free code (a lock-free stack with pointer reuse) would need a version-counter monitor. |
| Memory ordering | `dmb` / `dsb` are sequentially consistent fences, `ldar` / `stlr` are acquire / release atomics, LSE atomics are seq_cst. Plain loads and stores are plain `memcpy`s: the C++ compiler may reorder or merge them as it likes between fences, and x86's store-to-load reordering is not forbidden. | Correct code that synchronises through the instructions above works. Racy code that relies on the A57's actual behaviour without barriers is not reproduced. |
| Alignment and permission faults | An unaligned `ldxr`, `ldar`, atomic or pair does not fault; unaligned ordinary accesses work as on the console (normal memory). Writes to read-only guest memory fault as host access violations, not as guest exceptions. | The runtime's crash report names the guest function. |
| System registers | Inline: TPIDR_EL0, TPIDRRO_EL0, FPCR, FPSR, NZCV, DCZID_EL0 (0x10: DC ZVA prohibited), CTR_EL0 (0x8444c004). Every other `mrs` / `msr` calls `guest_mrs` / `guest_msr` (the runtime answers: counter, ID registers). `dc civac`, `dc cvac`, `ic` are no-ops. | DCZID / CTR are constants of the A57 and are not compared with Unicorn (it reports different values). |
| WFE and YIELD wait for nothing | `wfe` and `yield` do not stop the host thread: they call `nx_spin_hint(1)` and `nx_spin_hint(0)` (ctx.h), which call the function in `nx_spin_hook` when the runtime has set one and are an x86 `pause` otherwise. The guest state does not change; `sev`, `sevl` and `wfi` are still no-ops. | The console pauses the core until an event; the runtime's hook hands the host processor to another ready thread, which is all a spin-wait loop needs. Without a hook a loop that waits for a thread on the same host processor spins until the scheduler preempts it. |
| Pointer authentication, BTI, MTE | The hint-space instructions (`paciasp`, `autiasp`, `xpaclri`, `bti`) are no-ops, `retaa` / `retab` are `ret`, and the register forms (`pacia` ... `autdzb`, `xpaci`, `xpacd`) leave the register unchanged (`pacga` gives 0). MTE tag stores (`stg`, `st2g`, `stzg`, `stz2g`) change no data except the zeroing ones, and write the base back for the pre- / post-indexed forms; `ldg`, `stgp` and the tag arithmetic are not lifted. | The platform 2 binaries carry PAC / MTE instructions the console's CPU may or may not execute; nothing is signed, so nothing can fail to authenticate. Not in the differential test: Unicorn's `max` implements real PAC and raises an exception on STG. |
| setjmp / longjmp | Not translated specially. A guest `longjmp` is the runtime's problem (the libc import); SuperMarioWonderRecomp's per-function try/catch scheme was not ported. | The runtime decides how the SDK's `setjmp` is answered. |
| Self-modifying code | Not supported except through site lists (`patch`, `dynimm`) at known addresses. | Switch code is not self-modifying, and the SDK has no JIT. |
| Direct branches out of the module | A direct `b` / `bl` to an address outside this module's `.text` becomes `call_indirect(c, addr)`. | Such branches are not produced by a compiler between NSOs. |

## Coverage of the instruction space

| Class | Count | Treatment |
|---|---|---|
| In-scope shapes (Cortex-A57 ISA: ARMv8.0 base, FP, AdvSIMD, crypto AES / SHA-1 / SHA-256 / PMULL, CRC32; plus ARMv8.1 LSE atomics and the FEAT_LRCPC / LRCPC2 loads and stores) | 11,723 | all lifted; 11,539 run in the differential test, 184 (the exclusive-access families) run as 88 sequences |
| SVE / SME | 5,111 shapes seen | not lifted (`guest_unimpl`); not instructions of the console's CPU |
| ARMv8.1+ extensions other than LSE and LRCPC (RDM, dot product, FP16 arithmetic, JSCVT, FCMA, BF16, I8MM, ...) | 515 | outside the reference model: Unicorn's Cortex-A57 faults on them. Tier 2 lifts some of them (the ported half-precision arithmetic) but they are not verified |
| LRCPC (`ldapr[bh]`, `ldapur*`, `stlur*`), pointer authentication register forms, MTE tag stores | 120 in scope; 34 PAC and 64 MTE shapes out of scope | LRCPC is lifted as acquire loads / release stores like `ldar` / `stlr` and runs in the differential test on Unicorn's `max` model. PAC (identity) and `stg` / `st2g` / `stzg` / `stz2g` (no-op / zeroing) are lifted but cannot be compared: Unicorn's `max` signs pointers for real and faults on the MTE tag stores. `cover` is 100% on the title A main, sdk, subsdk0 and rtld |
| System / privileged (`mrs`, `msr`, `sys`, `dc`, `ic`, `tlbi`, `at`, `svc`, `hvc`, `smc`, `eret`) | 14 mnemonics | whitelisted forms are hand-written tests; `svc` is `guest_svc` |
| Control transfers (`b`, `bl`, `br`, `blr`, `ret`) | 5 | exercised by the end-to-end programs, not by the differential test |
| Unallocated or UNPREDICTABLE-only encodings that Capstone decodes | 3,628 | rejected by the reference |
| Undefined words (op0 = 0..3) | n/a | trap (`guest_trap`), tested with 9 words |

Excluded from the differential test by design: words whose index register is also the base register (115 shape names, the shapes
are still covered by other words); exclusive loads / stores as single words (tested as sequences); LDXP/STXP as sequences too.
Each shape is tested with at most 6 real encodings and two machine states (random, integer-edge), floating-point shapes with up
to 35 x 35 special operand pairs sampled at every 11th pair. The coverage is broad but not exhaustive over registers, immediates and
operand values.

## Reference corrections and oracle limits

- **Unicorn 2.1.4 executes `ldsmax` / `ldsmin` / `stsmax` / `stsmin` of 8, 16 and 32 bits unsigned** (docs/debug/001.md). The
  differential test takes the result of exactly those instructions from the architectural definition (`spec_smaxmin`), applied
  on top of Unicorn's state. No other instruction is corrected.
- Every test runs on Unicorn's Cortex-A57 model, except the LSE atomics which run on its `max` model (the A57 has none).
- FPSR is not compared; FPCR is (after `msr fpcr`).
- The known-answer oracle (`oracle/ka`) skips 7 of 108 cases: 3 with a real branch before the stop, 1 literal load of the code,
  1 window below 0x10000 and 2 windows that cannot be mapped on Windows. 6 more expectations read FPSR flags (QC) and 5 cases
  need FPCR (both gaps above); they are counted apart, not as passes.

## Measured on the game (S1b)

- `cover` is 100% on `main`, `sdk` and `subsdk0`; code outside every discovered function (hand-written SDK stubs) was found by a separate gap scan and is lifted (debug 008).
- Jump tables: 881 / 622 / 335 resolved; every other `br` is an indirect tail call (debug 007), 6 keep the dispatch fallback.
- Compile time and size: 6.5 M instructions lift to 453 MB of C++ and compile cold in about two minutes; no function reaches `optnone_insns`.
- C++ exceptions: only `sdk` has LSDAs; no typed catch and 13 empty `throw()` specifications. Class-hierarchy matching and the unwinder are the runtime's.
- Still not covered: hook sites on the real game (none configured), FPCR (3 writes, all in unreferenced SDK stubs), and anything that only a run can show.
