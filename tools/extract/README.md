# tools/extract

Tools that turn the player's own, **already decrypted** dump into the layout the lifter and the runtime read. Nothing here reads keys,
decrypts containers or circumvents any protection; encrypted NSP / XCI / NSZ / XCZ files are not supported.

Accepted dumps: an extracted folder (`exefs/` + `romfs/`, as `<dump>\base\...` and/or `<dump>\updates\<version>\...`) or a `.nup`
package (a community container of decrypted dumps, https://github.com/a55uka/nup/blob/meow/SPEC.md).

| Tool | What it does |
|---|---|
| `nup_extract.py <package>.nup <out> [--exefs-only]` | unpacks a `.nup` into `<out>/base/{exefs,romfs}` and `<out>/updates/<version>/{exefs,romfs}` (hash-checked; files whose size already matches are kept) |
| `nx_plain_nso <exefs folder> <out folder>` | copies an ExeFS folder and writes ZBIC-compressed NSOs as plain NSOs (segments stored, compressed sizes = sizes, bits 0-2 and 7 cleared, hash bits and hashes kept); each decoded segment is checked against its hash in the NSO header. Exit codes: 0 done, 1 error, 2 damaged NSO, 4 disk |

ZBIC: runtime/src/loader/zbic.cpp, format in runtime/docs/zbic.md.

Build and test: `powershell -ExecutionPolicy Bypass -File tools\extract\scripts\build.ps1 -Test` (CMake, Ninja, clang-cl through
`recomp\scripts\nx_env.cmd`; `build\release\bin\nx_plain_nso.exe`). The test converts a synthetic ZBIC NSO (`tests/zbic_frames.inc`).
`nup_extract.py` has its own test: `tools\.venv\Scripts\python.exe tools\extract\tests\test_nup_extract.py`.
