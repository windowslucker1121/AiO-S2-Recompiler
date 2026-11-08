# Contributing

Thanks for helping. Bug reports, tests on other GPUs and code are all welcome.

## Report a bug

1. Note what you did and where in the game it happened.
2. In the library, choose **Report a bug...** (Help menu, or a game's "•••" menu). It asks whether it is a **game bug** (something wrong
   while the game runs) or a **recompiler / library bug** (the app, adding a dump, building the game program, preparing shaders), then
   opens the matching GitHub form with the game, its version, the library version and your PC filled in. Without the library:
   [open an issue](../../issues/new/choose) and pick the form yourself.
3. Paste the relevant lines of the log as text. In the game, **F9** saves a bug bundle with a log (`log.txt`); keep the bundle's save,
   capture and image files to yourself: they contain game data.

How well a game runs (for [GAMES.md](GAMES.md)) goes into a **Game status** issue. Security problems go privately through the Security
tab ([SECURITY.md](SECURITY.md)).

## Set up a development machine

Follow [DEVELOPMENT_SETUP.md](DEVELOPMENT_SETUP.md). In short: Visual Studio 2022 with C++, repo-local LLVM / Python / .NET, your own dump in the repo root. Then:

```bat
recomp\scripts\build_game.cmd oot
powershell -ExecutionPolicy Bypass -File game-build\scripts\build.ps1 -Manifest %CD%\recomp\games\oot\game.toml
```

Read [AGENTS.md](AGENTS.md) before you start. It has the current state, known problems (the A-numbered artifacts) and how things work.

## Make a change

1. Fork the repository and create a branch from `oot-nup`.
2. Keep each change focused: one fix or one feature per pull request.
3. Put code in the right place:
   - `recomp/`: the recompiler. Keep it game independent.
   - `runtime/`: the runtime, shared by every game. Game knowledge goes in `runtime/games/<id>/` and `recomp/games/<id>/`.
   - `runtime/apps/setup/`: the library app (`recomp_library.exe`).
   - `tools/shaders/`: the shader translator.
4. Test it:
   - Runtime tests: `powershell -File runtime\scripts\build.ps1 -Test` (all groups must pass).
   - In the game: show that the fix works, and that nothing else broke where you played.
5. Update `AGENTS.md` when you learn something the next person needs, and remove what stops being true.

## Open a pull request

Say what it changes, and what you tested on which GPU. For a visible change, describe in words what looked wrong before and how it
looks now (where in the game, which effect). Do not attach screenshots, videos or frame captures of a game: they are game material
(LEGAL.md). Pictures of this project's own programs (the library app, test patterns) are fine.

`oot-nup` is protected: changes land through pull requests, which the maintainers review (`.github/CODEOWNERS`). Contributions are made
under the repository's licence (GPL-3.0), and everyone follows the [code of conduct](CODE_OF_CONDUCT.md).

## Rules

- **Never commit game files**: dumps, extracted files, lifted code, shader caches (`game/`, `work/` and `*.nup` are ignored for that).
  Nor keys, firmware, Nintendo SDK code, logos or screenshots and captures of a game. The full list is in `AGENTS.md`, "Legal rules",
  and [LEGAL.md](LEGAL.md).
- Game names only say which game a module is for; never use them, or Nintendo's marks, in product, exe or window names.
- Tools go into git-ignored folders and come from official releases with checksum checks.
- Do not change system or security settings in scripts; say what the user should run.
- Code is GPL-3.0. Code you copy from elsewhere must have a compatible licence; name its source and add it to the notices.
