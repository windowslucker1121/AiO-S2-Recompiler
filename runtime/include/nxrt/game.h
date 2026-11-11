// SPDX-License-Identifier: GPL-3.0-or-later
// The game boundary (docs/GAME_BOUNDARY.md): everything one title supplies to the game-independent base.
//
// The runtime, the library and the test tooling know no game. A game module (runtime/games/<id>/, a static library the game exe links)
// fills one Info and registers it with set_current() before anything reads it: before nxrt::init() in the game exe, at the top of the
// library's WinMain and of the test mains. Every part is optional. A null table or an empty string turns off what needs it (no hooks, no
// probes, neutral title facts), so a new title can start with the identity alone and add tables as its hooks are found.
//
// What each table is for, and where the game one lives:
//   title_defaults   services::TitleProfile defaults (main.npdm, control.nacp); ini keys still override them    games/oot/oot_core.cpp
//   timestep         the sites of the real-time frame step (nxrt/timestep.h, Sites)                           games/oot/oot_core.cpp
//   probes           test-tooling game-state probes and script steps (nxrt/tt.h, GameProbes)                  (none for oot)
//   play_defaults    factory controls.ini text and the bindings added by later revisions (play_types.h)       games/oot/oot_game.cpp
//   play             play-mode hooks: aim, camera, targeting, display, menu pointer, F1 entries (play_game.h)     games/oot/oot_game.cpp
//   prompts          the game's button icon textures (play/prompt_resolve.h, PromptGame)                      (none for oot)
//   poll_sleeps      SleepThread calls inside the game's own polling loops, shortened (PollSleep below)        (none for oot)
// The registry is header-only, so programs that do not link the runtime library (the library) use it too.
#pragma once
#include <cstdint>

namespace nxrt {
namespace services { struct TitleProfile; }
namespace timestep { struct Sites; }
namespace tt { struct GameProbes; }
namespace play { struct GamePlay; struct GameDefaults; struct PromptGame; }
}  // namespace nxrt

namespace nxrt::game {

// One nn::os::SleepThread call that sits in a polling loop of the game: the loop re-checks its condition after every sleep, so a shorter
// sleep only checks more often (a 1 s poll for work that takes milliseconds costs up to a second each time). `ret` is the return address
// of the call (the instruction after the bl), as an offset in main; `ns` the sleep it gets instead, when that is shorter than the request.
// The kernel's SleepThread looks the caller up while [sched] poll_sleeps is on (default on).
struct PollSleep {
    uint64_t ret = 0;
    int64_t ns = 0;
};

// The console the game was built for. platform 2 games run on the same Horizon model with a few platform facts that differ
// (system tick 31.25 MHz instead of 19.2 MHz, the NVN2 graphics API).
enum class Platform : uint8_t { Switch1, Switch2 };

struct Info {
    // ---- identity ----
    const char* id = "";                    // short id, also the stem of the game's file names: "oot"
    const char* title = "";                 // the name in logs and error boxes: "oot" (neutral, never the game's own title)
    const char* product = "";               // the build's neutral name (exe properties, shortcut descriptions): "Recomp module: oot".
                                            // Never the game's name (AGENTS.md, legal rules)
    const char* window_title = "";          // window and dialog titles: "<short title> (unofficial recompilation)" (empty: title)
    const char* short_title = "";           // a short name for file headers (the library's ini comment): "oot" (empty: product)
    uint64_t program_id = 0;                // the Switch title id
    const char* version = "";               // the game version the hooks were found in: "1.0.0"
    uint64_t main_base = 0x7100000000ull;   // where main is mapped (the lifter's base for it): hook offsets are relative to this
    uint32_t languages = 0;                 // control.nacp SupportedLanguage: bit n = services::Language n (0: American English only)
    Platform platform = Platform::Switch1;  // the console the game targets

    // ---- files and branding ----
    const char* exe_name = "";              // the game exe: "oot.exe"
    const char* setup_exe_name = "";        // the setup tool: "oot_setup.exe"
    const char* launcher_ini = "";          // a separate app's own settings file (none ships now)
    const char* game_ini = "";              // the settings file the setup tool writes and the game exe reads: "oot.ini"
    const char* shader_pack = "";           // the shader pack file next to the setup tool: "shader_cache.mprshc"
    const char* save_folder = "";           // save data folder under %APPDATA%: "oot-recomp" (empty: "nxrt")
    // Earlier names of save_folder, ";"-separated: while save_folder does not exist, the first of these that does is moved to it the first
    // time the save folder is asked for (services::save_dir), so saves made under an old name keep working.
    const char* old_save_folders = "";
    // Runtime settings this title needs by default, as ini text ("[nvn]\nupscaler_replace = ...\n"): applied at start for keys that neither
    // the settings file nor an NXRT_* variable sets (the game exe, game-build/src/game_main.cpp).
    const char* config_defaults = "";

    // ---- tables (null = the feature is off) ----
    void (*title_defaults)(services::TitleProfile& p) = nullptr;
    const timestep::Sites* timestep = nullptr;
    const tt::GameProbes* probes = nullptr;
    const play::GameDefaults* play_defaults = nullptr;
    const play::GamePlay* play = nullptr;
    const play::PromptGame* prompts = nullptr;
    const PollSleep* poll_sleeps = nullptr;
    uint32_t poll_sleep_count = 0;
};

// The registered game, or nullptr. One slot per program (the function-local static is shared by every translation unit that inlines it).
inline const Info*& current_slot() {
    static const Info* slot = nullptr;
    return slot;
}
inline void set_current(const Info* g) { current_slot() = g; }
inline const Info* current() { return current_slot(); }
// The registered game, or an empty Info (every table null) when none is.
inline const Info& info() {
    static const Info none;
    const Info* g = current();
    return g ? *g : none;
}
// The title of the game's windows and dialogs: Info::window_title, else the game's title, else "nxrt".
inline const char* window_title() {
    const Info& g = info();
    if (g.window_title && *g.window_title) return g.window_title;
    return g.title && *g.title ? g.title : "nxrt";
}

}  // namespace nxrt::game
