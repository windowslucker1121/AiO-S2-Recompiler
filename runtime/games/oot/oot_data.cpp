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
        g.window_title = GAME_WINDOW_TITLE;
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
        g.old_save_folders = GAME_OLD_SAVE_FOLDERS;
        // The game's upscaler (nvn_replay.cpp, upscaler_override): the 3D scene renders at a low internal size (854x480 or 960x540 seen) and a
        // chain of compute passes builds the output, one of them a tensor-core network (fea98fe7 / f90d726d by scale ratio). Since A10 the chain
        // runs and gives the right picture (sharper and anti-aliased against the copy, same brightness; ~2 ms of GPU time on an RTX 4080), so it
        // runs by default. The scaled copy of the scene (colour target 0 of the last draw with depth, into the texture the final composite
        // fs c63c3b8f samples at binding 0) stands in only while a pass of the chain cannot run (upscaler_fallback): the network pipeline takes
        // 2-3 minutes to build the first time, and the scene would stay black meanwhile. upscaler_fallback = 0: the copy always (A12).
        g.config_defaults =
            "[nvn]\n"
            "upscaler_replace = f05596b94bbb7c5c\n"
            "upscaler_skip = 54ecc4b4d30b3e2c,4c60af1fa697e5f2,fea98fe719bc0348,f90d726d76bf9884\n"
            "upscaler_src = depth_rt\n"
            "upscaler_dst = fs:c63c3b8f092e5d4c:tex0\n"
            "upscaler_fallback = 1\n"
            // The network variants at internal scales 0.75 (f90d726d, the pinned 1920x1080, oot_drs.cpp) and 0.667 (02dcb888) came out
            // striped / blocky: TLD4/TLD .F16.RN was decoded as a 32-bit gather (A22, fixed in the translator; f90d726d checked in game).
            // 02dcb888 is not checked on screen yet and its pipeline takes ~270 s to build the first time, which the upscaler fallback
            // does not cover: it stays a linearly filtered scaled copy of its input composite (tex0) into its output (img1) for now.
            "pass_copy = 02dcb888b204187a:tex0:img1\n"
            // The real-time frame step (oot_core.cpp apply_step; [time] mode = off gives the console's fixed 1/60 s per frame). The game is
            // made for 30-60 fps; on a PC that renders 15-25 fps in Kokiri Forest the step goes up to 1/10 s (scale 6; walking and the ladder
            // checked at 10 fps). Frames slower than that slow the game down instead of taking larger steps.
            "[time]\n"
            "min_rate = 10\n";
            // (graphics_ssbo_slot_xor = 1 was set here for a while. It only hid the garbage sky lighting of the float-atomic bug, and it fed the
            // material shaders the light list as the cluster grid: NaN, unresolved lightmaps, a runaway light loop and device loss. A12.)
        return g;
    }();
    return info;
}

}  // namespace nxrt::games::oot

void nxrt_register_game_oot_data() { nxrt::game::set_current(&nxrt::games::oot::data_info()); }
