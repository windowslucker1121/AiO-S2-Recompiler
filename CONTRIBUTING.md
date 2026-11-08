# Contributing

Thanks for helping. Bug reports, tests on other GPUs and code are all welcome.

## Report a bug

1. Note what you did and where in the game it happened.
2. In the game press **F9**. It saves a bug bundle (log, screenshot, state) and asks for a short note.
3. [Open an issue](../../issues/new/choose) with the bundle or the game's log, your GPU, driver and Windows version, and the commit you built.

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

Say what it changes, and what you tested on which GPU. Add before and after screenshots for anything you can see.

## Rules

- **Never commit game files**: dumps, extracted files, lifted code, shader caches (`game/`, `work/` and `*.nup` are ignored for that).
- Tools go into git-ignored folders and come from official releases with checksum checks.
- Do not change system or security settings in scripts; say what the user should run.
- Code is GPL-3.0. Code you copy from elsewhere must have a compatible licence; name its source and add it to the notices.
