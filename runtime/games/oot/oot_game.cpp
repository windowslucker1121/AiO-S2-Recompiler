// SPDX-License-Identifier: GPL-3.0-or-later
// Ocarina of Time: the whole game module as one nxrt::game::Info (oot_game.h). Play-mode hooks come here as they are found; until then
// the base runs the game without them (no frame-boundary hook: play.cpp skips what needs one).
#include "oot_game.h"

namespace nxrt::games::oot {

const game::Info& full_info() {
    static const game::Info info = core_info();
    return info;
}

}  // namespace nxrt::games::oot

void nxrt_register_game_oot() { nxrt::game::set_current(&nxrt::games::oot::full_info()); }
