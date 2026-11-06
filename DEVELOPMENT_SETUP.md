# Development setup (Ocarina of Time port)

Everything a Windows machine needs to **develop** the port: lift and compile the game code, build the runtime and `oot.exe`, build
the shader translator, translate and prepare shaders, and run the game for testing. Playing a finished build needs much less (see
`README.md`). Current status and next steps: `OOT_HANDOFF.md`; running notes for agents: `AGENTS.md`.

Rule for tools: repo-local tools go into git-ignored folders (`tools\llvm`, `tools\sccache`, `tools\vcpkg`, `tools\nvdisasm`,
`tools\uv`, `tools\.venv`, `tools\shaders\third_party`) and come from official releases with checksum checks. Game files (`game\`, `*.nup`) and
generated data (`work\`) are git-ignored too: never commit them.

## 1. Hardware and disk

| Requirement | Notes |
|---|---|
| Windows 10 or 11, 64-bit | Developed on Windows 11 Pro 22631. |
| GPU with Vulkan 1.3 | The runtime uses `VK_EXT_external_memory_host` and graphics pipeline libraries when present (the pre-boot shader preparation needs the latter). This machine: Intel Arc (integrated, driver Vulkan 1.4.325). |
| 16 GB RAM minimum | The game commits up to ~13 GB in its first scene and an integrated GPU shares system RAM: 32 GB is more comfortable. |
| CPU | Many cores help: the first cold build of the lifted code takes ~25 min on 18 threads; shader preparation ~15-20 min. |
| Disk | Plan for ~70 GB free plus the dump: NUP dump 25.6 GB, extracted game 26.3 GB, `work\` ~12 GB (lift 1.1, builds 1.1, shaders 9.5 incl. a 1 GB pipeline cache), `tools\` ~5 GB (LLVM 4.0, vcpkg 1.1), `game-build\build` 0.7 GB. |
| Network | First builds download sources: CMake FetchContent (SDL3, Vulkan-Headers, volk, VMA, ImGui), vcpkg ports, NuGet packages for the shader harness. |

## 2. System-wide software (installed by the developer)

| Software | Version used | Why |
|---|---|---|
| Git | 2.45 | Repository and the `refs/ryubing` submodule. |
| Visual Studio 2022 with "Desktop development with C++" | MSVC 14.44 (v143), Windows SDK (10.0.26100), "C++ CMake tools for Windows" (CMake 3.31, Ninja 1.12) | Linker, Windows SDK, CMake and Ninja; found with `vswhere` by `recomp\scripts\nx_env.cmd`. clang-cl itself comes repo-local (below). |
| .NET SDK 10 | 10.0.403 (9.0.x also installed) | Shader translator harness (`net10.0`) and the SASS decoder tool. |
| uv | any recent | Creates the Python 3.13 venv (it also fetches Python 3.13 if missing). Can be repo-local instead (section 4). |

The C++ workload must be registered with the installer: `nx_env.cmd` takes the newest Visual Studio that `vswhere` reports with
`Microsoft.VisualStudio.Component.VC.Tools.x86.x64`. A VS 2022 that has `cl.exe` but not that component (as on the second machine:
only the core editor parts, no `libcmt.lib`, no recent Windows SDK) is skipped, and an older VS such as VS 2019 Build Tools (MSVC
14.29) would be used. Add the workload from the command line (a system install: the developer runs it):

```bat
"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\setup.exe" modify --installPath "C:\Program Files\Microsoft Visual Studio\2022\Community" --add Microsoft.VisualStudio.Workload.NativeDesktop --add Microsoft.VisualStudio.Component.VC.Tools.x86.x64 --add Microsoft.VisualStudio.Component.VC.CMake.Project --add Microsoft.VisualStudio.Component.Windows11SDK.26100 --passive --norestart
```

Check: `recomp\scripts\nx_env.cmd` without arguments prints clang-cl, CMake, Ninja and sccache versions.
| Windows PowerShell 5.1 | built in | Runs the `.ps1` build scripts. PowerShell 7 (`pwsh`) is not needed. |
| curl | built in (Windows 10+) | Used by `tools\textures\scripts\fetch_third_party.ps1`. |

Not needed and deliberately not installed: the Vulkan SDK (only for validation layers; it is a system install, the user decides),
the CUDA toolkit (only `nvdisasm` is used, from the redistributable below), PowerShell 7.

## 3. Repository and submodule

```bat
git clone <this repository>
cd switch2-recomp
git checkout oot-nup
git submodule update --init refs/ryubing
```

`refs/ryubing` is Ryubing's Ryujinx (MIT), pinned to `a82350bb774f70fcbd41c9987bf67a3775409963`. Agent tooling may be blocked from
adding or fetching external repositories; then the developer runs these commands.

## 4. Repo-local tools

### LLVM / clang-cl 23.1.5 -> `tools\llvm\`
Official release asset `clang+llvm-23.1.5-x86_64-pc-windows-msvc.tar.zst` from https://github.com/llvm/llvm-project/releases, checked
against the release's SHA-256. It needs a zstd window of 2^31 to unpack, for example with the venv's `zstandard` (step 4.3 first):

```bat
tools\.venv\Scripts\python -I -c "import tarfile,zstandard; f=open(r'<download>\clang+llvm-23.1.5-x86_64-pc-windows-msvc.tar.zst','rb'); r=zstandard.ZstdDecompressor(max_window_size=2**31).stream_reader(f); tarfile.open(fileobj=r, mode='r|').extractall(r'tools\llvm')"
```

Result: `tools\llvm\clang+llvm-23.1.5-x86_64-pc-windows-msvc\bin\clang-cl.exe`. `recomp\cmake\clang-cl.cmake` prefers it over
`C:\Program Files\LLVM`.

### sccache 0.19.0 -> `tools\sccache\sccache.exe`
The `x86_64-pc-windows-msvc` asset of https://github.com/mozilla/sccache/releases/tag/v0.19.0, checked against its `.sha256`. Picked
up by the toolchain file at configure time; cache in `work\build\sccache`. Optional, but repeat builds are much faster.

### Python 3.13 venv -> `tools\.venv\`
```bat
uv venv tools\.venv --python 3.13
uv pip install --python tools\.venv -r recomp\requirements.txt -r tools\ai\requirements.txt zstandard
```
`recomp\requirements.txt`: unicorn 2.1.4, capstone 5.0.9, lz4, pyelftools (lifter and oracles). `tools\ai\requirements.txt`: pillow,
numpy (AI game tools, image diffs). `zstandard` (0.25.0 here) is needed by the shader corpus extraction and is in neither file.

Without a system uv, keep uv and its Python repo-local in the git-ignored `tools\uv\` (second machine: uv 0.13.7, the
`uv-x86_64-pc-windows-msvc.zip` asset of https://github.com/astral-sh/uv/releases, checked against its `.sha256`; uv checks the
Python download itself):

```powershell
$env:UV_PYTHON_INSTALL_DIR = "$PWD\tools\uv\python"; $env:UV_CACHE_DIR = "$PWD\tools\uv\cache"
tools\uv\uv.exe venv tools\.venv --python 3.13 --managed-python
tools\uv\uv.exe pip install --python tools\.venv -r recomp\requirements.txt -r tools\ai\requirements.txt zstandard
```

The venv then points at `tools\uv\python\cpython-3.13.*`: keep that folder.

### vcpkg with libvpx, dav1d, opus -> `tools\vcpkg\`
```bat
git clone --filter=blob:none https://github.com/microsoft/vcpkg.git tools\vcpkg
git -C tools\vcpkg checkout 661e42cd
tools\vcpkg\bootstrap-vcpkg.bat -disableMetrics
recomp\scripts\nx_env.cmd tools\vcpkg\vcpkg.exe install libvpx:x64-windows-static dav1d:x64-windows-static opus:x64-windows-static --disable-metrics
```
The checkout pins the port versions below. vcpkg fetches its own build tools (msys2, nasm, meson, pkgconf) into
`tools\vcpkg\downloads`; libvpx is the slow one (configure under msys2).
Static libraries with the static MSVC runtime (VP9/VP8, AV1 for OoT's movies, Opus for `hwopus`); vcpkg checks each source archive's
SHA-512. Versions here: dav1d 1.5.4, opus 1.6.1, libvpx 1.16.0 (vcpkg checkout `661e42cd`). `runtime\cmake\media_deps.cmake` finds
them in `tools\vcpkg\installed\x64-windows-static`; after installing, reconfigure (`cmake game-build\build\release`). Without them
the runtime builds without movies and with silent Opus.

### Shader translator (Ryujinx-based harness) -> `tools\shaders\`
```bat
powershell -ExecutionPolicy Bypass -File tools\shaders\scripts\setup_ryujinx.ps1
dotnet build tools\shaders\harness\SpirvHarness.csproj -c Release
```
The first script stages three Ryujinx projects from `refs\ryubing` into the git-ignored `tools\shaders\third_party`; the build applies
the local patches in `tools\shaders\harness\translator\*.patch` (with `tools\shaders\scripts\apply_patch.py` and the venv's Python)
and gives `tools\shaders\build\bin\Release\net10.0\spirv_harness.exe` (about 5 minutes the first time).

Optional, for decoder work: `dotnet build tools\shaders\ampere\decoder\tool\sassdecode.csproj -c Release` (opcode census:
`dotnet tools\shaders\build\bin\Release\net10.0\sassdecode.dll --census <corpus> --out <tsv>`).

### nvdisasm 13.3 (optional, for SASS decoding work) -> `tools\nvdisasm\`
From NVIDIA's CUDA redistributables: `https://developer.download.nvidia.com/compute/cuda/redist/redistrib_13.3.1.json` names
`cuda_nvdisasm/windows-x86_64/cuda_nvdisasm-windows-x86_64-13.3.73-archive.zip`, SHA-256
`351374795b61428a85bfb7026f05ce0b2a82b2e4264cafdf08739645ca3d0632`. Unzip into `tools\nvdisasm\`.

### ASTC -> BC7 transcoder (linked by the runtime) -> `tools\textures\`
```bat
powershell -ExecutionPolicy Bypass -File tools\textures\scripts\fetch_third_party.ps1
recomp\scripts\nx_env.cmd cmake -S tools\textures -B tools\textures\build\release -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl
recomp\scripts\nx_env.cmd cmake --build tools\textures\build\release
```
The script fetches astc-encoder and bc7enc_rdo at pinned revisions and ISPC 1.32.0 (SHA-256 checked).

### NSO converter -> `tools\extract\build\msvc\`
From a Visual Studio x64 developer prompt:
```bat
cmake -S tools\extract -B tools\extract\build\msvc -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build tools\extract\build\msvc
```
Gives `nx_plain_nso.exe` (writes plain NSOs from OoT's ZBIC-compressed `main`). `tools\setup\` is not in this repository, so the
NSP/XCI reader `nx_extract` is not built here.

## 5. Game data (supplied by the developer)

A decrypted dump of The Legend of Zelda: Ocarina of Time for Switch 2 (program `0400486024DEC000`, update 2.0.0 / `131072`) as a
`.nup` package (`The Legend of Zelda Ocarina of Time [0400486024DEC000][v131072].nup`, 25.6 GB, in the repository root, git-ignored).

```bat
rem program files (~40 MB), then the whole game (~26 GB, hash-checked)
tools\.venv\Scripts\python.exe tools\extract\nup_extract.py "<game>.nup" game\oot --exefs-only
tools\.venv\Scripts\python.exe tools\extract\nup_extract.py "<game>.nup" game\oot
rem plain NSOs for the lifter (checks every segment against the NSO header hashes)
tools\extract\build\msvc\bin\nx_plain_nso.exe game\oot\updates\131072\exefs game\oot\plain\exefs
```

## 6. Building

```bat
rem lifted game code: lift + compile all four modules (first time ~25 min; one module: append oot_main, oot_sdk, ...)
recomp\scripts\build_game.cmd oot
rem runtime + oot.exe (first time ~4 min; -Manifest is required because two games exist)
powershell -ExecutionPolicy Bypass -File game-build\scripts\build.ps1 -Manifest %CD%\recomp\games\oot\game.toml
rem later runtime-only changes (~20 s)
cmd /c "runtime\scripts\env.cmd && cmake --build game-build\build\release --target game_exe"
```
Judge a build by `FAILED:` lines in its output, not only the exit code. A change to `recomp\games\oot\main.sites` or the lifter needs
`build_game.cmd` again; the game build alone does not re-lift. Costs per kind of change: `AGENTS.md`, "What a change costs".

## 7. Shaders (one-time data preparation)

```bat
rem corpus of the game's shader blocks (~25 min)
tools\.venv\Scripts\python -I tools\shaders\scripts\extract_generic.py --romfs game\oot\updates\131072\romfs --main game\oot\plain\exefs\main --out work\shaders\oot\corpus --jobs 6 --progress
rem translate to SPIR-V (~5 min with 16 jobs; no Vulkan SDK here, hence --no-val)
tools\shaders\build\bin\Release\net10.0\spirv_harness.exe --batch work\shaders\oot\corpus --out work\shaders\oot\spirv --jobs 16 --no-val
rem program list for the pre-boot shader preparation (~100 s)
tools\.venv\Scripts\python -I tools\shaders\scripts\bnsh_programs.py --romfs game\oot\updates\131072\romfs --out work\shaders\oot\programs.bin
```
A development build (`game-build\build\release\oot.exe`, not a release package) started without `--game` / `--exefs` finds all
of it in this checkout: the game folder `game\oot\updates\<newest>` and, unless the ini or `NXRT_NVN_*` variables say otherwise,
`[nvn] shader_cache` / `shader_corpus` / `shader_translator` in `work\shaders\oot` and `tools\shaders\build`. So a plain `oot.exe`
(double click or terminal) runs the game; its log is then `game\oot\updates\131072\oot.log`. The first start shows the "Preparing
shaders" window (10-30 min, depending on the GPU), which fills `work\shaders\oot\pipeline_cache.bin`. The explicit form, as test runs
use it:
```bat
set NXRT_NVN_SHADER_CACHE=%CD%\work\shaders\oot\spirv
set NXRT_NVN_SHADER_CORPUS=%CD%\work\shaders\oot\corpus
set NXRT_NVN_SHADER_TRANSLATOR=%CD%\tools\shaders\build\bin\Release\net10.0\spirv_harness.exe
game-build\build\release\oot.exe --game %CD%\game\oot\updates\131072 --log %CD%\work\run\boot1\oot.log --max-seconds 120
```
Runtime settings are `[section] key` entries of `runtime\docs\CONFIG.md`, also settable as `NXRT_<SECTION>_<KEY>` variables.

## 8. AI test tools (optional)

The MCP server `game` (`tools\ai\mcp_server.py`, registered in `.mcp.json`) lets an agent launch the game, script input (including a
virtual SDL gamepad), take screenshots and read logs. Enable it per machine in the git-ignored `.claude\settings.local.json`:
`{"enabledMcpjsonServers": ["game"]}` (takes effect in sessions started afterwards). CLI help:
`tools\.venv\Scripts\python tools\ai\mcp_server.py help`; details in `tools\ai\README.md`.

## 9. Checks

```bat
tools\.venv\Scripts\python.exe tools\extract\tests\test_nup_extract.py
tools\.venv\Scripts\python tools\ai\tests\test_gamectl.py
powershell -ExecutionPolicy Bypass -File runtime\scripts\build.ps1 -Test
cd recomp && ..\tools\.venv\Scripts\python oracle\difftest\difftest.py all
```
The runtime tests build in their own folder (the game build has `NXRT_BUILD_TESTS` off). The recompiler oracles
(`oracle\difftest`, `oracle\e2e\run_e2e.py`, `oracle\hooks\test_hooks.py`) use unicorn as the reference CPU.

## 10. Moving to another computer

The code comes from GitHub (`git clone https://github.com/windowslucker1121/switch2-win-oot.git`, default branch `oot-nup`, then
`git submodule update --init refs/ryubing`). Only the game must be copied; some generated data is worth copying to save time.

| What | Size | Copy? |
|---|---|---|
| `game\oot\` (extracted game, plain NSOs, BC7 texture cache in `updates\131072\bc7_cache`) | ~28 GB | **Yes**, to the same place in the new clone. Or copy the `.nup` (25.6 GB) and extract again (section 5). The BC7 cache works on any GPU. |
| `game\oot\updates\131072\pipeline_cache.bin` | ~650 MB | Only for the same GPU and driver; otherwise delete it. `snap_*.png` / `trace_*.png` there are debug leftovers. |
| `work\shaders\oot\corpus\` | 0.7 GB | Recommended (saves ~25 min of extraction). |
| `work\shaders\oot\spirv\` | 5.1 GB | Recommended (saves the translation run, ~5 min). |
| `work\shaders\oot\programs.bin` | 1 MB | Recommended (saves ~100 s). |
| `work\shaders\oot\prepare.state`, `pipeline_cache.bin` | 1 GB | **Only for the same GPU and driver.** Elsewhere leave both out so the first start prepares shaders (15-30 min); a copied `prepare.state` saying "complete" would skip that step (its GPU key is not checked before the start) and the game would compile pipelines while playing. |
| `work\shaders\oot\redo*`, `results_*.tsv`, `results.full.tsv` | ~2.7 GB | No, leftovers of re-translation runs. |
| `work\lift\`, `work\build\`, `game-build\build\` | ~3 GB | No: Ninja files hold absolute paths. Rebuild (section 6, ~30 min). |
| `tools\.venv\` | 0.1 GB | No: not relocatable. Recreate (section 4). |
| `tools\llvm\`, `tools\vcpkg\`, `tools\nvdisasm\` | ~5 GB | Optional: they can be copied as they are instead of downloaded and built again. |
| `tools\shaders\third_party\`, `tools\textures\build\` | small | No: the setup scripts regenerate them. |
| `.claude\settings.local.json` | tiny | Recreate if the MCP `game` tools are wanted (section 8). `controls.ini` is recreated by the game. |

System software (section 2) is installed again on the new machine.

## 11. Not changed on this machine, on purpose

Windows Defender exclusions for `work\` and `tools\llvm\` would speed up builds, but they are a security setting: only the machine's
owner changes them.
