# Legal

## What this project is

An unofficial, non-commercial, open-source tool set that statically recompiles the program code of a Nintendo Switch 2 game from a dump
the player made of their own copy, so that the player can run that copy on their own Windows PC. It is an interoperability and
preservation project. It is not affiliated with, endorsed by or sponsored by Nintendo, The Pokémon Company, Creatures Inc., GAME FREAK
inc. or any other publisher or rights holder.

## Disclaimer

The software is provided "as is", without warranty of any kind (see `LICENSE`, sections 15 and 16). You use it at your own risk and
are responsible for using it lawfully where you live. Nothing here is legal advice.

## Bring your own legally obtained dump

- You need a game you bought and own, and a dump you made yourself from your own console and your own copy.
- Whether making and using such a dump is allowed depends on the law of your country. Follow it.
- The supported input is a dump that is already decrypted: an extracted folder (`exefs` + `romfs`) or a `.nup` package. No part
  of the project decrypts game containers, reads console keys or circumvents any protection; encrypted NSP, XCI, NSZ and XCZ files are
  not supported.
- What the tools build from your dump (recompiled code, extracted files, shader caches, captures, bug bundles) contains the game's
  copyrighted material. It is for your own use on your own PC. Do not upload, share or sell it.

## What the repository does not contain

- No game code, game files or game assets: no executables, RomFS data, shaders or shader caches, textures, models, audio, movies,
  fonts, text or save files of any game.
- No encryption keys, title keys, firmware or system files.
- No Nintendo SDK code, headers or documentation.
- No Nintendo or game logos, icons, box art or screenshots of games.
- No links to games, keys or firmware, and no help to obtain them other than dumping your own copy.

The repository holds facts needed for interoperability: title IDs, build IDs, file hashes, code offsets and function names, and notes
about file and interface formats written by the contributors.

## No piracy

The project does not support piracy in any form. Issues, pull requests and discussions that ask for or share game files, keys,
firmware, download links or ways around copy protection are closed and removed. Do not upload game files, extracted data or logs and
bundles that contain game data to issues.

## Trademarks

Nintendo, Nintendo Switch and Nintendo Switch 2 are trademarks of Nintendo. The Legend of Zelda and Ocarina of Time are trademarks of
Nintendo. Pokémon is a trademark of Nintendo, Creatures Inc. and GAME FREAK inc. Other names are trademarks of their owners.
They are used only nominatively: to say which hardware a dump comes from and which game a module supports. Their use does not mean
that any rights holder endorses or is connected with this project. The project's apps and icons use no Nintendo or game logos or art.

## Licences

The project is licensed under GPL-3.0 (`LICENSE`; parts say GPL-3.0-or-later). Third-party code keeps its own licence:
`runtime/THIRD_PARTY_NOTICES.md`, `recomp/THIRD_PARTY.md`, `tools/shaders/licenses/`, `tools/tiling/licenses/`,
`tools/textures/THIRD_PARTY.md` and `tools/textures/licenses/`.

## Takedown requests and rights-holder concerns

If you are a rights holder, or act for one, and think something in this repository infringes your rights:

1. Open an issue titled **"Rights holder contact"** with your contact details only (no details of the material are needed there), or
   use GitHub's DMCA process (<https://docs.github.com/en/site-policy/content-removal-policies/dmca-takedown-policy>).
2. Name the files or commits and the rights concerned.

We will answer, and we remove material that should not be here, including from the history where that is needed.
