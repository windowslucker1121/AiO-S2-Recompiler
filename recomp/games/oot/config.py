# SPDX-License-Identifier: GPL-3.0-or-later
"""Per-game configuration of the game (platform 2).

Identity, build id, version and the module list come from game.toml in this folder (as for every game). What stays here is lifter
knowledge that is not manifest data.
"""
import os
import sys
import tomllib

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
from nxrecomp.config import GameConfig, ModuleSpec          # noqa: E402

with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'game.toml'), 'rb') as _f:
    MANIFEST = tomllib.load(_f)
_GAME = MANIFEST['game']
_BUILD_ID = MANIFEST['dump']['main_build_id'].upper()

BUILDS = {
    _BUILD_ID: _GAME['version'],
}


class Config(GameConfig):
    name = _GAME['id']
    title_id = _GAME['title_id']
    version = _GAME['version']
    build_id = _BUILD_ID
    modules = [ModuleSpec(m['name'], m['base'], m['file']) for m in MANIFEST['module']]
    overrides = {}
    # rtld's process entry (0x0) has no FDE, symbol or direct branch, so discovery never roots it (as for title A)
    extra_entries = {'rtld': [0x0]}
    patches = None
