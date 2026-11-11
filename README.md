# AiO S2 Recompiler

An all-in-one, open-source tool set that runs console games **you own** natively on Windows. It statically recompiles the game's
program code from your own dump into PC code, and its runtime and shader translator do the rest. One library app manages every
supported game.

Unofficial and not affiliated with any console maker or publisher (see [Legal notice and trademarks](#legal-notice-and-trademarks)).
The repository contains no game files, keys, firmware or SDK code, and the app never downloads any. See [LEGAL.md](LEGAL.md).

## What it does

- **Recognises your dump.** Add it to the library; it identifies the game and version and shows it as a card.
- **Builds the PC version on your PC.** The first **Play** recompiles the game code and translates the shaders for your graphics card,
  once. Everything it builds stays on your PC.
- **Plays it.** Settings for each game: graphics, display, controller, keyboard and mouse, audio.
- **Checks your PC** and says only when something is missing.

| Game page | Settings | This PC |
|---|---|---|
| ![Game](docs/images/game.png) | ![Settings](docs/images/settings.png) | ![This PC](docs/images/this-pc.png) |

## What you need

- Windows 10 (1809) or 11, 64-bit; a graphics card with Vulkan 1.3; 16 GB of RAM; about 40 GB of free disk space per game.
- A game you own and **your own dump of it, made from your console**: a `.nup` file or an extracted folder (`exefs` + `romfs`),
  already decrypted. This project does not help you get dumps, keys or firmware.

Which games and versions work, and how well: [GAMES.md](GAMES.md).

## How to run it

1. Start `recomp_library.exe`.
2. Press **Add a game**, or drop your `.nup` file or game folder onto the window.
3. Press **Play**.

To build from source, follow [DEVELOPMENT_SETUP.md](DEVELOPMENT_SETUP.md).

## How it is built

| Part | Folder |
|---|---|
| Recompiler (ARM64 to C++, game independent) | `recomp/` |
| Runtime shared by every game (OS, services, graphics) | `runtime/` |
| One small module per game | `recomp/games/<id>/`, `runtime/games/<id>/` |
| Library app | `runtime/apps/setup/` |
| Shader translator | `tools/shaders/` |

## Contributing

Bug reports, tests on other GPUs and code are welcome: [CONTRIBUTING.md](CONTRIBUTING.md). **Never commit or upload game files**,
extracted data, or logs and bug bundles that contain game data. Bugs: **Report a bug...** in the library, or the
[issue forms](../../issues/new/choose). Everyone follows the [code of conduct](CODE_OF_CONDUCT.md); security problems go privately
through [SECURITY.md](SECURITY.md).

## Licence and credits

GPL-3.0 ([LICENSE](LICENSE)). Third-party code keeps its own licence; notices are in `runtime/THIRD_PARTY_NOTICES.md`,
`recomp/THIRD_PARTY.md`, `tools/shaders/licenses/`, `tools/tiling/licenses/` and `tools/textures/THIRD_PARTY.md`. Built on
[nxrecomp](https://github.com/drakolordx7/nxrecomp) and work from Ryujinx, Mesa, smo-recomp, ScarletNX, ssbu-recomp,
SuperMarioWonderRecomp, yuzu/Eden, libnx and others named in those notices.

## Legal notice and trademarks

This README is the only place where the project names the console maker in its own words (identifiers that file formats, system
interfaces or third-party APIs require aside).

- AiO S2 Recompiler is an unofficial, non-commercial, open-source project. It is **not affiliated with, endorsed, sponsored or approved
  by Nintendo** or by any game developer, publisher or other rights holder.
- Nintendo and Nintendo Switch 2 are trademarks of Nintendo. Game titles that the library shows for your own dumps are read from those
  dumps and are trademarks of their owners. Such names are used only to say which hardware and which dumps the tools work with; the
  project's own name, apps, icons and package names use no Nintendo or game names, logos, icons or artwork.
- The repository and the release packages contain no game code or assets, no keys, no firmware and no Nintendo SDK code or
  documentation. You need your own legally made dump of a game you own; the project does not help you obtain games, keys or firmware.
- Rights holders: see "Takedown requests and rights-holder concerns" in [LEGAL.md](LEGAL.md).
