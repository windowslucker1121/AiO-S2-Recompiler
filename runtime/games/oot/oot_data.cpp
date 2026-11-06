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
        // The game's upscaler, replaced (nvn_replay.cpp, upscaler_override): the 3D scene renders at a low internal size (854x480 or 960x540
        // seen) and a chain of compute passes builds the output, one of them a tensor-core network (fea98fe7 / f90d726d by scale ratio). On
        // the PC the chain's first pass becomes a scaled copy of the scene (colour target 0 of the last draw with depth) into the texture
        // the frame's final composite (fs c63c3b8f) samples at binding 0, and the other passes are left out. Without it the output stayed
        // black (A12, AGENTS.md).
        g.config_defaults =
            "[nvn]\n"
            "upscaler_replace = f05596b94bbb7c5c\n"
            "upscaler_skip = 54ecc4b4d30b3e2c,4c60af1fa697e5f2,fea98fe719bc0348,f90d726d76bf9884\n"
            "upscaler_src = depth_rt\n"
            "upscaler_dst = fs:c63c3b8f092e5d4c:tex0\n";
            // (graphics_ssbo_slot_xor = 1 was set here for a while. It only hid the garbage sky lighting of the float-atomic bug, and it fed the
            // material shaders the light list as the cluster grid: NaN, unresolved lightmaps, a runaway light loop and device loss. A12.)
        return g;
    }();
    return info;
}

}  // namespace nxrt::games::oot

void nxrt_register_game_oot_data() { nxrt::game::set_current(&nxrt::games::oot::data_info()); }
