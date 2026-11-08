# Security

The runtime and the tools run native code on your PC and read files you give them: game dumps, NSO modules, save data, shader and texture caches. A bug in how they parse those files, or in how they handle memory, can be a security problem.

If you find one, report it privately through the repository's Security tab ("Report a vulnerability"). Do not open a public issue. Please include the version or commit, what you did, and what happened. We will reply, work on a fix, and credit you when it ships if you want that.

Things we treat as security problems:

- a crafted `.nup` package, dump folder, NSO module, RomFS file or save file that crashes or runs code in the loader, the runtime,
  the game build or the library app (`recomp_library.exe`)
- the library app or the game build writing outside the folders you chose
- the recompiler or a tool executing something from an input file
- a release package that contains something other than what `tools/release/package.ps1` puts in it

Everything else (crashes with normal files, graphics bugs, wrong behaviour) goes through the normal issue forms.

Fixes go to the default branch `oot-nup` and the next release. Older releases get no fixes.
