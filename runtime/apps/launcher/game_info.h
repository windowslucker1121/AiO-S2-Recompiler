// SPDX-License-Identifier: GPL-3.0-or-later
// Launcher helpers: what is in the game folder, the launcher's own settings file, command-line quoting, and starting the game exe
// or the setup tool. The pure parts (check_game_dir, summary, parse/format, quote_arg) do not touch the registry or the
// launcher's own folder. The names of the game's programs and files are the registered game's (nxrt/game.h; the launcher registers
// the data part of its game module at start, cmake/launcher.cmake NXRT_LAUNCHER_GAME).
#pragma once
#include <windows.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

namespace mprl {

// ---- Branding: the registered game's names (nxrt/game.h Info) ---------------------------------------------------------------------
std::wstring product_name();      // the window title and the dialogs: "Recomp module: mpr" (the manifest's neutral product name)
std::wstring game_exe_name();     // "mpr.exe"
std::wstring setup_exe_name();    // "mpr_setup.exe"
std::wstring shader_pack_name();  // the shader pack next to the launcher: "shader_cache.mprshc"
// A text of the registered game (UTF-8) as a wide string, or `fallback` when the game gives none.
std::wstring game_text(const char* utf8, const wchar_t* fallback);

struct GameStatus {
    bool exefs = false;           // exefs\main exists
    bool romfs = false;           // romfs\ exists and holds at least one entry
    bool textures = false;        // bc7_cache\ exists and holds at least one entry
    bool shaders = false;         // shader_cache\ exists and holds at least one entry
    bool setup_finished = false;  // setup_state.tsv records a finished setup run
    bool ready() const;           // exefs and romfs are there, enough to start the game
    std::wstring summary() const; // one plain sentence for the player
};

// Pure filesystem checks of one game folder. Never throws; an empty dir gives nothing found.
GameStatus check_game_dir(const std::wstring& dir);

// Folder of the running launcher exe (no trailing backslash), and the default game folder (the same folder).
std::wstring launcher_exe_dir();
std::wstring default_game_dir();
std::wstring controls_path(const std::wstring& game_dir);  // <dir>\controls.ini
std::wstring launcher_config_path();                       // <exe dir>\<the game's launcher ini> (mpr_launcher.ini)

// The launcher's own settings file: UTF-8 text with a [launcher] section.
//   [launcher]
//   game_dir = D:\Games\Metroid Prime
struct LauncherConfig {
    std::wstring game_dir;
};
std::string format_launcher_config(const LauncherConfig& cfg);
// Fills *out. Keys that are missing keep the value already in *out. Returns false (and leaves *out alone) when
// the value is not valid UTF-8 or out is null.
bool parse_launcher_config(const std::string& utf8_text, LauncherConfig* out);
// On failure (missing or unreadable file, bad text) *out gets the default game folder and false is returned.
bool load_launcher_config(const std::wstring& path, LauncherConfig* out);
// Writes a temp file next to the target and renames it over the target.
bool save_launcher_config(const std::wstring& path, const LauncherConfig& cfg, std::wstring* err);

// One command-line argument, quoted by the CommandLineToArgvW rules. Plain words stay as they are.
std::wstring quote_arg(const std::wstring& s);

// Starts <launcher exe dir>\<game exe> --game <game_dir> [extra_args] with the launcher folder as working directory.
// Does not wait. extra_args is added as typed (for test runs).
bool start_game(const std::wstring& game_dir, std::wstring* err, const std::wstring& extra_args = L"");

// Starts <launcher exe dir>\<setup exe> --out <game_dir> [extra_args]. The process handle goes to *process;
// the caller waits for it and closes it. *process is null when false is returned.
bool start_setup(const std::wstring& game_dir, HANDLE* process, std::wstring* err, const std::wstring& extra_args = L"");

// ---- Shader preparation ----------------------------------------------------------------------------------------------
// `mpr.exe --prepare-shaders [--if-needed]`, run in the game folder, compiles the game's shaders ahead of play. The setup tool
// writes <game folder>\shader_cache\programs.bin (the list of programs); a finished run writes shader_cache\prepare.state.

enum class PrepareKind { other, progress, ready, done, error };

// One line of the prepare run's output. Only lines starting with "prepare:" are of a known kind; anything else is `other`.
struct PrepareLine {
    PrepareKind kind = PrepareKind::other;
    unsigned long long done = 0;    // progress: programs compiled so far
    unsigned long long total = 0;   // progress: programs in all
    long long failed = -1;          // done: the failed= count, -1 when the line has none
    bool cancelled = false;         // done: the line ends with "cancelled"
    std::string text;               // error: the error text; ready and done: the rest of the line; other: the whole line
};
PrepareLine parse_prepare_line(const std::string& line);

// True when shader_cache\programs.bin exists. Without it there is nothing to prepare (older game folders).
bool shader_prepare_available(const std::wstring& game_dir);
// True when shader_cache\prepare.state has the line "complete: 1". It does not know about driver changes; --if-needed checks those.
bool shader_prepare_done(const std::wstring& game_dir);

// One prepare run: starts mpr.exe --prepare-shaders [--if-needed] with the game folder as working folder, no console window,
// and its output (stdout and stderr) read through a pipe. A worker thread reads and parses the lines and never touches a window.
// It posts msg_update to `notify` when the progress or the result changed (changes may share one message), and msg_finished
// once the child has exited and its output is read. The UI thread then calls ack_update() / progress() / result() and, after
// msg_finished, finish() exactly once. notify may be null (a run the caller waits for with finish()).
class PrepareRun {
public:
    PrepareRun() = default;
    ~PrepareRun();   // a run still going is cancelled and waited for
    PrepareRun(const PrepareRun&) = delete;
    PrepareRun& operator=(const PrepareRun&) = delete;

    // Returns false with *err set when the run could not be started (mpr.exe missing, Windows error).
    bool start(const std::wstring& game_dir, bool if_needed, HWND notify, UINT msg_update, UINT msg_finished, std::wstring* err);
    bool active() const { return active_; }        // started and finish() not called yet
    void cancel();                                  // TerminateProcess of this run's child only; the next run starts from zero
    bool cancel_requested() const { return cancel_requested_.load(); }
    void ack_update();                              // call on msg_update, before reading progress() and result()
    PrepareLine progress() const;                   // last "prepare: <done> <total>" line (kind other before the first)
    PrepareLine result() const;                     // last ready, done or error line (kind other until one comes)
    // Joins the worker and closes the handles. Returns the child's exit code (0xFFFFFFFF when unknown).
    DWORD finish();

private:
    void worker();
    void handle_line(const std::string& line);

    bool active_ = false;
    HWND notify_ = nullptr;
    UINT msg_update_ = 0;
    UINT msg_finished_ = 0;
    HANDLE proc_ = nullptr;
    HANDLE pipe_ = nullptr;                         // read end of the child's output
    std::thread thread_;
    mutable std::mutex mu_;                         // progress_ and result_
    PrepareLine progress_;
    PrepareLine result_;
    std::atomic<bool> update_pending_{false};       // an msg_update is posted and not yet acknowledged
    std::atomic<bool> cancel_requested_{false};
    DWORD exit_code_ = 0xFFFFFFFFu;                 // written by the worker, read after join
};

}  // namespace mprl
