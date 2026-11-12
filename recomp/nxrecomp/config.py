# SPDX-License-Identifier: GPL-3.0-or-later
"""The per-game configuration seam.

Everything game-specific that the generic lifter must not know lives in a `GameConfig` subclass under
recomp/games/<name>/config.py (and is selected with --game <name>). smo-recomp kept this knowledge inside its
lifter (sdklift_rules.txt, "nerve" RTTI naming, NPDM / version checks, base-to-update symbol matching); here the
lifter only asks the config. The default config is a neutral one that lifts a module as it is.
"""
import importlib
import importlib.util
import os


class ModuleSpec:
    """One module of the program: where it is mapped and how it is lifted."""

    def __init__(self, name, base, source=None, kind='nso'):
        self.name = name              # 'main', 'sdk', 'subsdk0', ...
        self.base = base              # guest address of the module's first byte
        self.source = source          # default input path (relative to the game's work dir) or None
        self.kind = kind              # 'nso' | 'nx_elf' | 'static_elf'


class GameConfig:
    name = 'generic'

    # modules of the program, in load order
    modules = []                      # [ModuleSpec]

    # guest address -> name of a host handler `h_<name>` that REPLACES the guest function (called as h_<name>(Ctx *c)).
    # Direct calls, indirect calls (call_indirect) and tail calls to that address all go to the handler.
    overrides = {}

    # extra function entries the discovery cannot see (offsets are module-relative), per module name
    extra_entries = {}

    # instruction patches: file of `<address> <original word> <new word>` lines (see program.load_patches), or None
    patches = None

    # a function with more than this many instructions is compiled with optnone (bounded compile time); the build's
    # time-limit fallback (cmake/nx_cc_launcher.py) lowers it when a file still takes too long
    optnone_insns = 20000

    # NZCV as function-local variables (program.local_flags): each lifted function keeps the flags in a local NxFlags and
    # writes them to the context only where another function or the runtime may read them (tail branches, hooks, SVCs), not
    # at calls (BL / BLR) or returns, where the AArch64 procedure call standard leaves them undefined. The environment
    # variable NX_LOCAL_FLAGS=0 turns it off for a lift.
    local_flags = True

    # inline caches (program.inline_caches_on, ctx.h NX_ICALL / NX_IIMPORT): every indirect call site (BLR, BR out of a
    # function, unresolved jump tables) and every import stub keeps the last lifted target in a static of its own and calls it
    # directly; misses go through the runtime as before. NX_INLINE_CACHES=0 turns it off for a lift.
    inline_caches = True

    # x9..x17 as function-local variables (program.localize_scratch, needs local_flags): written to the context only at tail
    # branches, sites and SVCs, not at calls or returns (temporaries of the procedure call standard). NX_SCRATCH_LOCALS=0 turns
    # it off for a lift.
    scratch_locals = True

    # x0..x8, x19..x29 and SP as function-local variables (program.localize_callee_saved, needs local_flags): loaded at entry
    # and after sites / SVCs, written to the context before every call and at every exit (only what changed since the context
    # was last in step); x19..x29 and SP are kept across calls (the callee preserves them), x0..x8 are loaded again after each.
    # Removes the reloads clang made after every guest store, but measured slower (village, 2026-11-12: game time per frame
    # 6.75 / 6.72 vs 6.36 / 6.34 ms): every local is loaded at entry and most are spilled to the host stack again. OFF by default
    # (lift output unchanged); NX_CALLEE_LOCALS=1 turns it on for a lift, NX_CALLEE_LOCALS_REGS chooses the registers.
    callee_locals = False

    # host function names to treat as noreturn-like terminators (none by default)
    noreturn_imports = ()

    # a hook the game config may use to name functions in the generated code's comments: addr -> str
    def function_name(self, module, addr):
        return None


class GenericConfig(GameConfig):
    """The neutral config of a title without a port (experimental builds, runtime/games/generic): nothing about the game, only what every
    Switch program has in common."""
    # rtld's process entry (offset 0) has no FDE, symbol or direct branch, so discovery never roots it (as the oot config and title A's said)
    extra_entries = {'rtld': [0x0]}


def load_game(name):
    """The GameConfig instance of recomp/games/<name>/config.py (class `Config`); 'generic' (or none): GenericConfig."""
    if not name or name == 'generic':
        return GenericConfig()
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    path = os.path.join(here, 'games', name, 'config.py')
    spec = importlib.util.spec_from_file_location('nxrecomp_game_' + name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod.Config()
