// SPDX-License-Identifier: GPL-3.0-or-later
// The generic game module, core part (generic_game.h): the title facts from the manifest's [title] table (make_manifest.py writes them
// from main.npdm and, when the dump has one, control.nacp). No hooks, no frame step: the game runs with its own fixed step.
#include "generic_game.h"

#include "nxrt/services.h"

namespace nxrt::games::generic {

namespace {

void title_defaults(services::TitleProfile& p) {
    const Manifest& m = manifest();
    const game::Info& g = data_info();
    p.program_id = g.program_id;
    p.name = g.title;
    p.display_version = m.get("dump.display_version", g.version);
    p.supported_languages = g.languages;
    const uint64_t save = m.num("title.save_size", 0x4000000);
    p.account_save_size = save;
    p.account_save_journal = m.num("title.save_journal", save);
    p.device_save_size = m.num("title.device_save_size", save);
    p.device_save_journal = m.num("title.device_save_journal", save);
    p.temporary_storage_size = m.num("title.temporary_storage_size", 0x4000000);
    p.main_thread_stack = m.num("title.main_thread_stack", 0x100000);
    p.main_thread_priority = static_cast<int>(m.num("title.main_thread_priority", 44));
    p.main_thread_core = static_cast<int>(m.num("title.main_thread_core", 0));
    // main.npdm SystemResourceSize: non-zero turns on the SDK's virtual address memory manager (the game needs it, oot_core.cpp)
    p.system_resource_size = m.num("title.system_resource_size", 0);
    p.firmware = m.get("title.firmware", g.platform == game::Platform::Switch2 ? "22.0.0" : "16.0.0");
    p.user_nickname = "Player";
    p.user_uid_hi = 0x0000000000000001ull;
    p.user_uid_lo = 0x47656e5265636d70ull;     // "GenRecmp"
}

}  // namespace

const game::Info& core_info() {
    static const game::Info info = [] {
        game::Info g = data_info();
        g.title_defaults = &title_defaults;
        return g;
    }();
    return info;
}

}  // namespace nxrt::games::generic

void nxrt_register_game_generic_core() { nxrt::game::set_current(&nxrt::games::generic::core_info()); }
