// SPDX-License-Identifier: GPL-3.0-or-later
// The generic game module (docs/GAME_BOUNDARY.md): one runtime for every title that has no port of its own yet ("experimental" builds).
// Nothing in it is about a game. Who the game is (identity, file names, platform) and its title facts (main.npdm: main thread, system
// resource size; firmware lower bound; save sizes) come at start from the game manifest the library wrote from the player's own dump
// (tools/extract/make_manifest.py): NXRT_GAME_MANIFEST, else game.toml next to the exe. Play mode has no frame-update hook: it is
// driven from the game's presents (play.cpp present_frame), and pads use the console layout.
//   nxrt_game_generic_data   identity, branding                     nxrt_register_game_generic_data()
//   nxrt_game_generic_core   + title facts (needs nxrt)             nxrt_register_game_generic_core()
//   nxrt_game_generic        + play mode (needs nxrt_gfx)           generic_rt.dll      nxrt_register_game_generic()
#pragma once

#include <string>

#include "nxrt/game.h"

void nxrt_register_game_generic();
void nxrt_register_game_generic_core();
void nxrt_register_game_generic_data();

namespace nxrt::games::generic {

// The manifest's values (strings and integers by "table.key"; [[array]] tables are skipped).
struct Manifest {
    bool loaded = false;
    std::string path;                  // the file read
    std::string error;                 // why it was not read (empty when loaded)
    std::string get(const std::string& key, const std::string& def = "") const;
    unsigned long long num(const std::string& key, unsigned long long def) const;
};
// Reads a manifest text (the TOML subset of recomp/cmake/GameManifest.cmake). False with *error set on a malformed line.
bool parse_manifest(const std::string& text, Manifest* out, std::string* error);
// The manifest of this process: read once, from NXRT_GAME_MANIFEST or <exe dir>\game.toml.
const Manifest& manifest();

const game::Info& data_info();     // generic_data.cpp
const game::Info& core_info();     // generic_core.cpp
const game::Info& full_info();     // generic_game.cpp

}  // namespace nxrt::games::generic
