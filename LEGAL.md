# Legal

## What this project is

An unofficial, non-commercial, open-source tool set that statically recompiles the program code of a console game from a dump the
player made of their own copy, so that the player can run that copy on their own Windows PC. It is an interoperability and
preservation project. It is not affiliated with, endorsed by or sponsored by the console maker or any game developer, publisher or
other rights holder; the non-affiliation and trademark notice that names them is in `README.md` ("Legal notice and trademarks").

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
- No SDK code, headers or documentation of the console maker.
- No logos, icons or box art of the console maker or of any game, and no screenshots of games.
- No links to games, keys or firmware, and no help to obtain them other than dumping your own copy.

The repository holds facts needed for interoperability: title IDs, build IDs, file hashes, code offsets and function names, and notes
about file and interface formats written by the contributors.

## No piracy

The project does not support piracy in any form. Issues, pull requests and discussions that ask for or share game files, keys,
firmware, download links or ways around copy protection are closed and removed. Do not upload game files, extracted data or logs and
bundles that contain game data to issues.

## Trademarks

The trademark and non-affiliation notice is in `README.md` ("Legal notice and trademarks"); it is the only place where the repository
names the console maker in its own words. Everywhere else (code, comments, logs, documentation, tests, tools, manifests, CI) games are named only
by short neutral ids (such as `oot`) and the consoles as "platform 1" / "platform 2"; identifiers that file formats, system interfaces
or third-party APIs require stay as they are. Names of upstream open-source projects stay where they credit code taken from them. The
project's apps, icons and packages use no trademarks, logos or art of the console maker or of any game.

## DLSS

NVIDIA Streamline (`sl.interposer.dll`, `sl.common.dll`, `sl.dlss.dll`) is MIT-licensed and may ship in the release package. NVIDIA's
DLSS library `nvngx_dlss.dll` is under the NVIDIA RTX SDKs LICENSE and is never shipped or committed: the library downloads it from
NVIDIA's own repository only on the user's request, after showing the licence and getting the user's OK (`tools/release/fetch_dlss.py`).

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
