# Pokopia Recomp

Pokémon Pokopia (Switch 2) recompiled to run natively on Windows.

[Download the latest build](https://github.com/drakolordx7/pokopia-recomp/releases/latest)

You need your own copy of the game, version 2.0.1a: an extracted folder, or NSP/XCI files plus your keys. Not affiliated with Nintendo, The Pokémon Company or Koei Tecmo.

## Requirements

- Windows 10 or 11, 64-bit
- GPU with Vulkan 1.3
- 16 GB of RAM

## Usage

1. Unzip the build and run `PokopiaLauncher.exe`.
2. Pick your dump folder (or your NSP/XCI files and key files) and press Set up. It compiles the game on your PC (about 7 minutes), then builds the shader cache (1 to 2 hours). You can pause it and continue later.
3. Press Play.

F1 opens the settings in game. Middle mouse toggles Joy-Con 2 mouse mode for placing items.

## Ocarina of Time (branch `oot-nup`): how a player starts

The Ocarina of Time port has its own first program, **`oot_setup.exe`** (Initial Setup), next to `oot.exe`:

1. **Requirements**: it checks the PC and says how to get anything missing (it installs nothing and changes no system setting):
   Windows 10 1809 or newer, 64-bit; a graphics card with Vulkan 1.3 and a current driver (VK_EXT_graphics_pipeline_library to compile
   the shaders ahead; VK_EXT_mesh_shader for foliage); 16 GB of RAM recommended; about 40 GB free (the extracted game 26 GB, shader data
   about 10 GB); the .NET 10 runtime when the bundled shader translator is not self-contained; Python 3.11+ with `zstandard` and `lz4`
   for the extraction and the shader scan (`py -3 -m pip install --user zstandard lz4`). No Visual C++ Redistributable: the exe links its
   runtime statically (the setup checks every DLL `oot.exe` imports).
2. **Game files**: your own dump of version 2.0.0 (update 131072), as a `.nup` package or an extracted folder (`exefs\` + `romfs\`).
   NSP/XCI are not supported by this build yet. Choose a data folder for what the setup writes.
3. **Prepare**: extract, find the shaders, translate them, list the programs, compile them for your graphics card (one to two hours, most
   of it the last step). Cancel stops at any time; Start continues where it stopped.
4. **Play** starts the game (or make a desktop shortcut there). After a game update, a new translator or a new graphics driver the setup
   marks the steps that must run again; Rebuild shaders does it by hand.

## Reporting problems

[Open an issue](../../issues/new/choose) with `pkp.log` from the session where it happened, your GPU and your Windows version.

## Contributing

- Open pull requests against `main` and say what you tested on which GPU.
- Code for this game goes in `recomp/games/pkp/` and `runtime/games/pkp/`; the rest is shared with other games.
- Building from source needs Visual Studio 2022 Build Tools or newer, LLVM, CMake, Ninja, Python 3.13, .NET 8 SDK, vcpkg with `libvpx:x64-windows-static`, Rust and Node.js. Copy the game's `exefs` files to `game\dump\exefs\`, run `recomp\scripts\build_game.cmd pkp` in a Developer Command Prompt, then `pwsh -File game-build\scripts\build.ps1`. Shaders: [tools/shaders/README.md](tools/shaders/README.md). Launcher: `npm install`, `npm run tauri build` in `launcher\`.
- Runtime tests: `pwsh -File runtime\scripts\build.ps1 -Test`.

## Credits

Built on [nxrecomp](https://github.com/drakolordx7/nxrecomp). Uses code from [Ryujinx](https://github.com/Ryubing/Ryujinx), [Mesa](https://gitlab.freedesktop.org/mesa/mesa), smo-recomp, ScarletNX, ssbu-recomp, SuperMarioWonderRecomp, yuzu, Eden, libnx, [astc-encoder](https://github.com/ARM-software/astc-encoder), [bc7enc_rdo](https://github.com/richgel999/bc7enc_rdo), [libvpx](https://chromium.googlesource.com/webm/libvpx), AMD FidelityFX, NVIDIA Image Scaling, [SDL3](https://github.com/libsdl-org/SDL), [Tauri](https://tauri.app), [Zig](https://ziglang.org) and Python, plus [Kenney input prompts](https://kenney.nl/assets/input-prompts) and DejaVu fonts. Licences are in `runtime/THIRD_PARTY_NOTICES.md`, `recomp/THIRD_PARTY.md` and `tools/shaders/licenses/`.

## License

GPL-3.0. Third-party code keeps its own licence.
