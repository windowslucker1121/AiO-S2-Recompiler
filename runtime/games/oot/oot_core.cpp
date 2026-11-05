// SPDX-License-Identifier: GPL-3.0-or-later
// Ocarina of Time, the core part of the game module (oot_game.h): the data part plus the title facts. Needs the runtime library, not
// the graphics one. No test probes or poll sleeps yet.
#include "oot_game.h"

#include "nxrt/game.h"
#include "nxrt/services.h"

namespace nxrt::games::oot {

namespace {

// main.npdm of the 2.0.0 update: priority 44, core 0, stack 0x100000. Save sizes are placeholders until control.nacp is known (the
// boot log shows what the game asks for).
void title_defaults(services::TitleProfile& p) {
    p.program_id = data_info().program_id;
    p.name = data_info().title;
    p.display_version = data_info().version;
    p.supported_languages = data_info().languages;
    p.account_save_size = 0x4000000;
    p.account_save_journal = 0x4000000;
    p.device_save_size = 0x4000000;
    p.device_save_journal = 0x4000000;
    p.temporary_storage_size = 0x4000000;      // placeholder too: the game mounts temporary storage on a loader thread
    p.main_thread_stack = 0x100000;
    p.main_thread_priority = 44;
    p.main_thread_core = 0;
    // META SystemResourceSize 0x3000000 (GetInfo 16): nn::os enables its virtual address memory manager only when this is non-zero, and the
    // game allocates address regions through it (left at 0, VammManager stayed uninitialised and AllocateAddressRegion read null).
    p.system_resource_size = 0x3000000;
    p.firmware = "22.0.0";                     // a lower bound: main is ZBIC-compressed, which firmware 22.0.0 introduced
    p.user_nickname = "Player";
    p.user_uid_hi = 0x0000000000000001ull;
    p.user_uid_lo = 0x4f4f545265636d70ull;     // "OOTRecmp"
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

}  // namespace nxrt::games::oot

void nxrt_register_game_oot_core() { nxrt::game::set_current(&nxrt::games::oot::core_info()); }
