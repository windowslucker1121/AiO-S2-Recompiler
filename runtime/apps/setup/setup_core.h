// SPDX-License-Identifier: GPL-3.0-or-later
// Initial Setup (the <id>_setup.exe the player starts first): everything that is not window code. Which tools there are, what the PC has
// (requirements), where the setup writes, which preparation steps are done or must be done again, and child processes with their
// output read line by line. Game independent: names come from the game manifest (game_manifest.h, recomp/games/<id>/game.toml).
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

// ---- tools: where each one is in a release folder (next to the setup exe) or in a development checkout ---------------------------------
struct Tools {
    std::wstring exe_dir;          // folder of the setup exe
    std::wstring dev_root;         // development build: the checkout (GAME_DEV_ROOT), else empty
    std::wstring game_exe;         // <exe dir>\<id>.exe
    std::wstring nup_extract;      // nup_extract.py
    std::wstring shader_scripts;   // folder with extract_generic.py and bnsh_programs.py
    std::wstring translator;       // spirv_harness.exe
    std::wstring python;           // python.exe found, or empty
    std::wstring python_args;      // "-3" for the py.exe launcher
};
Tools find_tools();

// ---- the setup's own settings (<exe dir>\<id>_setup.ini) and its data folder ---------------------------------------------------------
struct Config {
    std::wstring data_dir;   // where the setup writes: the extracted game (dump\), the shader data (shader_cache\), the game ini
    std::wstring source;     // the player's dump: a .nup file or an extracted folder
};
std::wstring config_path();
Config load_config();
bool save_config(const Config& c);

enum class SourceKind { none, nup, folder, unsupported, missing };
SourceKind source_kind(const std::wstring& source);

// The layout under the data folder.
struct Layout {
    std::wstring data, dump, shaders, corpus, spirv, programs, prepare_state, game_ini, state_file;
    std::wstring game;   // the game folder the exe runs (exefs + romfs); empty while the dump is not extracted
};
Layout layout_of(const Config& c);

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
};
struct GpuInfo {
    std::wstring name;
    std::wstring driver;          // decoded driver version
    uint32_t api = 0;             // Vulkan version
    bool usable = false;          // Vulkan 1.3, dynamic rendering + synchronization2, swapchain
    bool pipeline_library = false;
    bool mesh_shader = false;
    std::wstring why;             // not usable: why
};
std::vector<GpuInfo> query_gpus(std::wstring* error);
// Every check. disk_need: bytes the remaining steps will still write.
std::vector<Check> run_checks(const Tools& t, const Config& c, uint64_t disk_need);
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
