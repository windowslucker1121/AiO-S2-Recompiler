// SPDX-License-Identifier: GPL-3.0-or-later
// The Legend of Zelda: Ocarina of Time game module (docs/GAME_BOUNDARY.md), as games/pkp: one nxrt::game::Info in three libraries,
// each a superset of the one before:
//   nxrt_game_oot_data   identity, branding (pure data)          the launcher           nxrt_register_game_oot_data()
//   nxrt_game_oot_core   + title facts (needs nxrt)              the service tests      nxrt_register_game_oot_core()
//   nxrt_game_oot        + play-mode hooks (needs nxrt_gfx)      oot.exe                nxrt_register_game_oot()
// Bring-up: no hooks, probes or play mode yet.
#pragma once

#include "nxrt/game.h"

void nxrt_register_game_oot();
void nxrt_register_game_oot_core();
void nxrt_register_game_oot_data();

namespace nxrt::games::oot {
const game::Info& data_info();     // oot_data.cpp
const game::Info& core_info();     // oot_core.cpp
const game::Info& full_info();     // oot_game.cpp
// The game's dynamic resolution pinned to a fixed internal size ([oot] resolution keys, oot_drs.cpp); from play mode's install / remove.
void drs_install(uint64_t base);
void drs_remove();
}  // namespace nxrt::games::oot
