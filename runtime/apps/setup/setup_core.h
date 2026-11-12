// SPDX-License-Identifier: GPL-3.0-or-later
// Initial Setup (the <id>_setup.exe the player starts first): everything that is not window code. Which tools there are, what the PC has
// (requirements), where the setup writes, which preparation steps are done or must be done again, and child processes with their
// output read line by line. Game independent: every game name comes from its manifest, read at run time (setup_games.h).
#pragma once
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace setup {

std::wstring widen(const std::string& s);
std::string narrow(const std::wstring& w);
std::wstring with_commas(uint64_t v);
std::wstring gib(uint64_t bytes);              // "26.3 GB"
bool file_exists(const std::wstring& p);
bool dir_exists(const std::wstring& p);
std::wstring join(const std::wstring& a, const std::wstring& b);
std::wstring parent_of(const std::wstring& p);
std::wstring exe_dir();
std::string read_file_text(const std::wstring& p);                    // the whole file, empty when it cannot be read
bool write_file_atomic(const std::wstring& path, const std::string& text);

struct GameDef;   // setup_games.h

// ---- tools: where each one is in a release folder (next to the setup exe) or in a development checkout ---------------------------------
struct Tools {
    std::wstring exe_dir;          // folder of the setup exe
    std::wstring dev_root;         // development build: the checkout (GAME_DEV_ROOT), else empty
    std::wstring nup_extract;      // nup_extract.py
    std::wstring shader_scripts;   // folder with extract_generic.py and bnsh_programs.py
    std::wstring translator;       // spirv_harness.exe
    std::wstring python;           // python.exe found, or empty
    std::wstring python_args;      // "-3" for the py.exe launcher
    std::wstring game_build;       // tools\build\game_build.py of a release (with its Zig and lifter), else empty
    std::wstring make_manifest;    // make_manifest.py (tools\scripts in a release, tools\extract in the checkout)
    std::wstring fetch_dlss;       // fetch_dlss.py (tools\scripts in a release, tools\release in the checkout)
};
Tools find_tools();
// The game's exe: <exe dir>\<files.exe>; a development build also looks in the checkout's game-build\build\release.
std::wstring game_exe_path(const Tools& t, const GameDef& g);

// The player-side build of the game exe (release packages: games\<id>\sdk + runtime, tools\build\game_build.py). A release never
// carries a game exe: the player builds it from their own dump into <data>\program.
struct GameBuild {
    bool ok = false;               // this release can build the game's exe
    std::wstring sdk, runtime;     // games\<id>\sdk (stub, import library), games\<id>\runtime (runtime DLL and what it loads)
    std::string stamp;             // names, sizes and times of the sdk and runtime files: a new release builds the exe again
};
GameBuild game_build_of(const Tools& t, const GameDef& g);
// An experimental build of a title without a port can be made here: the game build, make_manifest.py and the generic runtime
// (games\generic\sdk + runtime) are all in this release.
bool experimental_possible(const Tools& t);

// ---- one game's choices (from the library, setup_games.h) ----------------------------------------------------------------------
struct Config {
    std::wstring data_dir;   // where the setup writes: the extracted game (dump\), the shader data (shader_cache\), the game ini
    std::wstring source;     // the player's dump: a .nup file or an extracted folder
    std::wstring game_ini;   // the ini name of the game ([files] ini)
};

enum class SourceKind { none, nup, folder, unsupported, missing };
SourceKind source_kind(const std::wstring& source);

// The layout under the data folder.
struct Layout {
    std::wstring data, dump, shaders, corpus, spirv, programs, prepare_state, game_ini, state_file;
    std::wstring game;   // the game folder the exe runs (exefs + romfs); empty while the dump is not extracted
};
Layout layout_of(const Config& c);

struct Config;
// The exe a game runs: the one the player built (<data>\program\<exe>) when this release builds it, else game_exe_path.
std::wstring game_exe_for(const Tools& t, const GameDef& g, const Config& c);

// The game in a game folder: main's build id (hex, from the NSO header) and what the extraction recorded.
struct GameId {
    bool ok = false;
    std::string build_id;      // 40 hex digits
    std::string version;       // the update version of extract.json, or the folder name
    std::string text() const { return build_id + "/" + version; }
};
GameId game_id_of(const std::wstring& game_folder);

// A stamp of the translator: names, sizes and times of the files in its folder (a new translator release changes it).
std::string translator_stamp(const std::wstring& translator_exe);

// ---- the setup's record of its steps (<data>\setup_state.ini: key = value lines) ------------------------------------------------------
struct State {
    std::string extracted_from;   // "<source>|<size>|<time>" of the .nup the dump came from
    std::string corpus_game;      // the GameId the corpus was scanned from
    std::string translated_game;  // the corpus (GameId) every block of which was sent to the translator
    std::string translated_with;  // translator_stamp of that translation
    std::string programs_game;    // the GameId programs.bin was listed from
    std::string built_for;        // "<GameId>|<GameBuild stamp>" the game exe in <data>\program was built for
    // How long each step took the last time it ran from its start to its end on this PC (seconds, 0: not measured): the time estimate
    // of the next run (the library's step order: extract, build, corpus, translate, programs, prepare).
    int secs[6] = {};
};
State load_state(const std::wstring& path);
bool save_state(const std::wstring& path, const State& s);
std::string nup_stamp(const std::wstring& nup);

// ---- requirements ------------------------------------------------------------------------------------------------------------------------
enum class Level { ok, warn, bad, info };
struct Check {
    std::wstring item, state, detail;
    std::wstring fix;     // exact steps or commands for the player; empty when nothing is to do
    Level level = Level::info;
    std::wstring need;    // what the check measures against ("Windows 10 1809 or newer"), shown next to what this PC has
};
struct GpuInfo {
    std::wstring name;
    std::wstring driver;          // decoded driver version
    uint32_t api = 0;             // Vulkan version
    bool usable = false;          // Vulkan 1.3, dynamic rendering + synchronization2, swapchain
    bool pipeline_library = false;
    bool mesh_shader = false;
    uint32_t vendor = 0;          // PCI vendor id (0x10DE NVIDIA)
    std::wstring why;             // not usable: why
};
std::vector<GpuInfo> query_gpus(std::wstring* error);
// DLSS (optional): 0 no NVIDIA RTX GPU, 1 RTX but nvngx_dlss.dll missing (the library downloads it), 2 ready, 3 nvngx_dlss.dll there
// but Streamline's sl.interposer.dll / sl.common.dll / sl.dlss.dll missing (MIT; only packages built with -Streamline carry them, the
// library cannot download them). The DLLs live in <exe dir>\upscalers\streamline. The GPU answer is asked once and kept.
enum : int { kDlssNoRtx = 0, kDlssDownload = 1, kDlssReady = 2, kDlssNoStreamline = 3 };
int dlss_state(const Tools& t);
std::wstring dlss_dir(const Tools& t);
std::wstring dlss_missing_files(const Tools& t);   // the missing DLLs, comma-separated (empty: none)
// The PC's checks (Windows, GPU, memory, translator, Python): the same for every game.
std::vector<Check> run_system_checks(const Tools& t);
// One game's checks: disk space for its remaining steps, its program and the DLLs it needs, its dump. disk_need: bytes still to write.
std::vector<Check> run_game_checks(const Tools& t, const GameDef& g, const Config& c, uint64_t disk_need);
uint64_t remaining_disk_need(const Config& c, const State& s);

// ---- child processes -----------------------------------------------------------------------------------------------------------------
// One child with its stdout + stderr read through a pipe on a worker thread; each line goes to on_line (worker thread), on_exit gets the
// exit code (worker thread). cancel(): sets the named event the child watches (--cancel-event) or, without one, ends the child.
class Child {
public:
    ~Child();
    bool start(const std::wstring& exe, const std::wstring& args, const std::wstring& cwd, bool with_cancel_event,
               std::function<void(const std::string&)> on_line, std::function<void(DWORD)> on_exit, std::wstring* err);
    void cancel();
    void kill();   // ends the child (its process tree) now, whatever it does
    bool running() const { return running_.load(); }
    void wait();
private:
    HANDLE proc_ = nullptr, pipe_ = nullptr, event_ = nullptr, job_ = nullptr;   // job_: the child's process tree
    std::thread thread_;
    std::atomic<bool> running_{false};
};
std::wstring quote(const std::wstring& s);
// Runs a command to its end (short checks); output in *out. False when it did not start or ran longer than timeout_ms (then ended).
bool run_capture(const std::wstring& exe, const std::wstring& args, DWORD timeout_ms, std::string* out, DWORD* code);

}  // namespace setup
