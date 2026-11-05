// SPDX-License-Identifier: GPL-3.0-or-later
// Ocarina of Time: who the game is and what its files are called (nxrt/game.h Info). Pure data, no runtime: the launcher links this
// part alone. The values come from the game manifest (recomp/games/oot/game.toml) through the generated game_manifest.h.
#include "oot_game.h"
#include "game_manifest.h"

namespace nxrt::games::oot {

const game::Info& data_info() {
    static const game::Info info = [] {
        game::Info g;
        g.id = GAME_ID;
        g.title = GAME_TITLE;
        g.product = GAME_PRODUCT;
        g.short_title = GAME_SHORT_TITLE;
        g.program_id = GAME_PROGRAM_ID;
        g.version = GAME_TARGET_VERSION;
        g.main_base = GAME_MAIN_BASE;
        g.languages = GAME_LANGUAGES;          // placeholder: the NUP package has no control.nacp
        g.platform = game::Platform::GAME_PLATFORM;
        g.exe_name = GAME_EXE;
        g.setup_exe_name = GAME_SETUP_EXE;
        g.launcher_ini = GAME_LAUNCHER_INI;
        g.game_ini = GAME_INI;
        g.shader_pack = GAME_SHADER_PACK;
        g.save_folder = GAME_SAVE_FOLDER;
        return g;
    }();
    return info;
}

}  // namespace nxrt::games::oot

void nxrt_register_game_oot_data() { nxrt::game::set_current(&nxrt::games::oot::data_info()); }
