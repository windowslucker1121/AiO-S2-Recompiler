# Third-party code in recomp/

nxrecomp is GPL-3.0-or-later (see `LICENSE`). It is built from the projects below. Every borrowed file carries a header
naming its source; this page lists them with the exact upstream commit that was read.

| Source (commit) | Licence | What was taken | Where it is now |
|---|---|---|---|
| smo-recomp `f88beab36ba39a29f574a9c97ee28b052e96104b` | GPL-3.0-or-later | the instruction translator (class `Fn`), function discovery, program-level lift, NSO reading, the Unicorn differential test and its host program, the mutation-test idea, `fpexact.h` (exact FP helpers), `neon.h` (SIMD helpers), the context struct and flag helpers (`runtime.h`), the optnone / time-limit build rule (`build_main.py`) | `nxrecomp/lift/a64.py`, `nxrecomp/program.py`, `nxrecomp/funcs.py`, `nxrecomp/nso.py`, `nxrecomp/image.py` (loader part), `include/nxrecomp/ctx.h`, `fpexact.h`, `neon.h`, `oracle/difftest/{difftest.py,host.cpp,mutation.py}`, `cmake/nx_cc_launcher.py` |
| SuperMarioWonderRecomp `665b2f9c175be3386cb0f683b677aa443afbbf06` | MIT (notice below) | the Capstone-driven emitters for newer-SDK instruction coverage (LSE atomics, LDXP/STXP, crypto AES/SHA/PMULL, scalar AdvSIMD forms, pairwise / across-lane / long forms), the C++ helper header, the jump-table resolver | `nxrecomp/lift/tier2/*.py` (ported emitters, kept as the base), `include/nxrecomp/cxx.h` (helpers), `nxrecomp/jumptables.py` |
| ssbu-recomp `fea28c18b785372b5024ece0a2ea52e860b1fa04` | GPL-3.0 | C++ exception lifting with landing-pad resume entries and `nro_catch_call` (here `nx_catch_call`), the site list (patch / inline / midhook / dynimm / noinline / jtable) of `relift_sites.py`, the hook-table / noinline rule of `hook_inline_check.py` | `nxrecomp/program.py` (`u_<entry>`, resume table, `load_sites`), `include/nxrecomp/ctx.h` (`NxResume`, `nx_catch_call`) |
| ScarletNX `9ec73af7fe2712e8f823f399640ca3e71d3baff6` | GPL-3.0-or-later | TPIDR_EL0 / TPIDRRO_EL0 kept apart; Cortex-A57 as the Unicorn reference; the audit corpus's edge-value design; the known-answer cases (`tests/a64_ported_cases.h`, originally rEden / dynarmic tests) are read from `refs/` by `oracle/ka/run_ka.py`, not copied | `ctx.h` (`tpidrro`), `oracle/difftest/common.py`, `oracle/ka/` |

Not taken: smo-recomp's `sdklift.py` / `sdklift_rules.txt`, `gen_tables.py` (game symbol matching between the base game and the
update, nerve naming), `extract.py` (game version / NPDM checks), its clang-9 build scripts, and everything under its `runtime/`
except the three headers above. The game-specific seam is `games/<name>/config.py`.

## MIT notice (SuperMarioWonderRecomp)

```
MIT License

Copyright (c) 2026 BelmanteGu

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```
