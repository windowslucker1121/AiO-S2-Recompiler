# Recomp Launcher

Tauri 2 launcher for a game module's exe (here module `pkp`, `pkp.exe`; names come from the module's `game.toml`). It ships no game files and never changes yours. It reads the game
files you extracted from your own copy, writes `pkp.ini` next to `pkp.exe` and builds the shader cache from your romfs.

## First run

1. **Game files**: pick the dump folder (`<dump>\base\{exefs,romfs}` and/or `<dump>\updates\<version>\{exefs,romfs}`).
   The newest complete update is used, otherwise the base. "Use these game files" writes `[fs] exefs` and `[fs] romfs`.
2. **Set up**: writes the `pkp.ini` paths (`[fs] exefs`, `[fs] romfs`, `[fs] save_dir`, `[nvn] shader_cache =
   shader_cache/spirv`) and builds `shader_cache\spirv\` next to `pkp.exe`:
   - Extract: `tools\shaders\extract_corpus.py` finds every shader in the romfs and in `exefs\main` and writes the
     distinct blocks to `shader_cache\corpus\` (about 10 GB, deleted when setup finishes).
   - Translate: `tools\spirv_harness\spirv_harness.exe --batch` in chunks of 1000 into `shader_cache\spirv\`
     (`<hash>.spv` + `results.tsv`, about 26 GB for the 99,012 blocks of 2.0.1).
   A progress bar shows each step. Pause stops the tools and Continue setup picks up where they stopped: a block listed
   in `results.tsv` is never translated twice, and extraction is skipped if it already ran for the same romfs. The tools
   run at below-normal priority.
   State: `shader_cache\setup.json`.
3. **Play** turns on once setup has finished. The launcher starts pkp.exe with no paths on the command line. A `pkp.ini`
   next to it makes its folder the working directory, and every path comes from the ini (relative paths start there).

`release\Play.bat` starts the game the same way without the launcher (after setup).

## Release model (b): pkp.exe built on the player's PC

When `tools\build\pkp_build.py` is present (the package from the main project's release script), the release holds no
`pkp.exe`. Set up first builds it from the chosen dump (`src-tauri/src/gamebuild.rs`): verify the ExeFS by hash, lift,
compile with the bundled Zig, link against `pkp_rt.dll`; then the shader steps below run. Play stays off until both are
done. Pause works in every step and Continue setup resumes. Headless check of the whole first run:
`cargo run --release --example first_run -- <game dir> <exefs> <romfs> <tools dir> [shader limit] [jobs]`.

## Release folder

The launcher looks for its tools in `tools\` next to its own exe. A release folder must contain:

```
pkp.exe                     the game (game-build, -Release)
pkp_launcher.exe            this launcher (<launcher.product_name>.exe) (npm run tauri build)
Play.bat                    release\Play.bat (optional)
tools\
  spirv_harness\            self-contained publish of nx tools/shaders/harness:
                              dotnet publish tools\shaders\harness\SpirvHarness.csproj -c Release -r win-x64
                                --self-contained true -o <release>\tools\spirv_harness     (~84 MB)
                            (stage the Ryujinx sources first: tools\shaders\scripts\setup_ryujinx.ps1)
  shaders\extract_corpus.py nx tools/shaders/ampere/extract_corpus.py
  python\python.exe         optional: Python 3 (for example the embeddable package) with the lz4 package installed.
                            Without it the first python.exe on PATH that can `import lz4.block` is used.
```

If a file is missing, setup stops and names it. `pkp.exe` can live in another folder (Settings, game folder), but
`tools\` always sits next to the launcher.

## Development

- `cargo check` in `src-tauri`; `npm run tauri build` for the exe.
- `cargo run --example shader_setup -- <game dir> <romfs> <exefs/main> <tools dir> [limit] [jobs]` runs the setup's
  shader steps without the window (same code, `src-tauri/src/shaders.rs`); `limit` translates only that many blocks.
