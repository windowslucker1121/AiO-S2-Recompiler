# Games

Which games the library knows, and how well they run. The library recognises a dump by its **title ID** and its version (the **build
ID** of the game's `main` program). A dump of another version shows a warning and may not start.

## States

| State | Meaning |
|---|---|
| ✅ Playable | Can be played from start to end. Small problems only. |
| 🟡 In game | Gets into the game. Graphics problems, low frame rate or missing features. |
| 🟠 Boots | Starts, but does not get into the game. |
| ⚪ Port exists | A port exists, but it is not built or not tested in this branch. |
| ❌ No port | The library knows the dump, but there is no port yet. |

## List

| Game | Title ID | Version | Main build ID | State | Notes |
|---|---|---|---|---|---|
| The Legend of Zelda: Ocarina of Time | `0400486024DEC000` | 2.0.0 (131072) | `e0f3ccfaf50065ae4ea01eb5aeb13b1a45da3348` | 🟡 In game | Title, intro movie and the first scenes render. 30-38 fps early in the first scene, 15-23 fps in heavier views. |
| The Legend of Zelda: Ocarina of Time | `0400486024DEC000` | 1.0.0 (base) | `33fa6c2c23848282a77e8b88a0a93f4887c55920` | ❌ No port | Install update 2.0.0 and dump again. |
| Pokémon Pokopia | `0400A3A01EE4C000` | 2.0.1a (655360) | `76a489f3bd3642fa5e18bd83d32c98bacc5f7d0e` | ⚪ Port exists | `recomp/games/pkp`, `runtime/games/pkp`. Not built here. |
| Pokémon Pokopia | `0400A3A01EE4C000` | 1.0.0 (base) | `d4e672b8b97a5a0a56620c52a6ae88a2cab83020` | ❌ No port | Install update 2.0.1a and dump again. |
| Super Mario Bros. Wonder (Switch 2 Edition) | `010015100B514000` | 5.0.0 (327680) | `ff773e90972d544eb79406eaa65396d53c43efb9` | ❌ No port | A Switch 1 program with Switch 2 features. |

## Known problems

- **Ocarina of Time**: still runs at about half speed, because the game advances one step per frame and the frame rate is below 60. Some
  scenes are not checked yet. Details in [AGENTS.md](AGENTS.md) (the artifact table).

## Report a game

Tried a game or version that is not listed, or one that runs better or worse than its state says?
[Open an issue](../../issues/new/choose) with the title ID, the version and what you saw. The library's game page shows the title ID.
