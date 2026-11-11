// SPDX-License-Identifier: GPL-3.0-or-later
// The game library: the one program a player starts. One window for every game this release (or checkout) has a port of: a library of
// game cards, each game's preparation queue (extraction, shader scan, translation, program list, shader compile) with its status, the
// PC's requirements, and Play. Plain Win32 + GDI+ (anti-aliased cards, dark theme), common controls v6 for lists and edits. It checks
// what the PC needs and says how to get what is missing (it installs nothing and changes no system setting).
//
// Games come from their manifests at run time (setup_games.h: <exe dir>\games\<id>\game.toml, or recomp\games in a development build); the
// player's games and their folders are in <exe dir>\library.ini. A game's preparation steps are the existing tools, run as child processes:
//   extract    python nup_extract.py <nup> <data>\dump                                    (tools/extract)
//   build      python game_build.py --exefs <game>\exefs --game <data>\program ...         (release packages only: the game exe is
//              built from the player's own dump with the bundled lifter and Zig; game-build/release_b/game_build.py)
//   corpus     python extract_generic.py --romfs --exefs --out <data>\shader_cache\corpus  (tools/shaders/scripts)
//   translate  <game>.exe --translate-shaders                                              (the "Preparing shaders" window's step 1, headless)
//   programs   python bnsh_programs.py --romfs --out <data>\shader_cache\programs.bin
//   prepare    <game>.exe --prepare-shaders --if-needed                                    (pipeline parts for this GPU and driver)
// <data>\setup_state.ini records which game and translator each step ran for; the prepare step is judged by the game exe itself
// (--prepare-shaders --check). One game's steps run at a time (they use every core); the other games wait in the queue.
//
//   recomp_library.exe [--add DUMP]... [--game ID] [--data DIR] [--source PATH] [--page N] [--start] [--game-args "<text>"]
// --add identifies a dump (.nup or folder) and adds it to the library without questions (tests);
// --game selects a game of the library (or a known manifest); --data / --source set its data folder and dump; --page opens a page
// (0 library, 1 game, 2 settings, 3 this PC); --start queues its preparation; --game-args adds text to the game's command line at Play.
#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>


#include "setup_core.h"
#include "setup_games.h"
#include "setup_settings.h"
#include "setup_theme.h"

namespace fs = std::filesystem;
using namespace setup;
using namespace setup::theme;
namespace gp = Gdiplus;

namespace {

constexpr wchar_t kClass[] = L"NxrtGameLibrary";
constexpr wchar_t kCardsClass[] = L"NxrtGameCards";
constexpr wchar_t kAppName[] = L"Recomp Library";
#ifndef RECOMP_LIBRARY_VERSION
#define RECOMP_LIBRARY_VERSION "dev"
#endif
#ifndef RECOMP_LIBRARY_REPO
#define RECOMP_LIBRARY_REPO "https://github.com/windowslucker1121/AiO-S2-Recompiler"
#endif
constexpr UINT kMsgLine = WM_APP + 1;     // wParam: StepRun*, lParam: std::string* (one output line of that step)
constexpr UINT kMsgExit = WM_APP + 2;     // wParam: StepRun*, lParam: exit code
constexpr UINT kMsgCheck = WM_APP + 3;    // wParam: GameItem*, lParam: std::string* (the prepare check's answer)
constexpr UINT kMsgDlss = WM_APP + 5;     // lParam: std::wstring* (empty: DLSS downloaded, else the error)
constexpr UINT kMsgReqs = WM_APP + 4;
constexpr UINT kMsgGameExit = WM_APP + 6; // wParam: GameItem*, lParam: the game's exit code     // lParam: std::vector<Check>* (the PC's checks, made on a thread)
constexpr UINT_PTR kTimer = 1;
constexpr ULONG_PTR kCopyArgs = 0x52454C42;   // WM_COPYDATA from a second start: its command line
constexpr int kHovPc = 90, kHovBrand = 98, kHovAdd = 99, kHovReport = 100, kHovTab = 101, kHovDetails = 103;

// Two colours per game for its card's banner, from its id (stable, distinct, never too dark or too bright).
void banner_colors(const std::string& id, COLORREF* a, COLORREF* b) {
    uint32_t h = 2166136261u;
    for (char ch : id) h = (h ^ static_cast<unsigned char>(ch)) * 16777619u;
    auto hsv = [](float hue, float s, float v) {
        const float c = v * s, x = c * (1 - std::fabs(std::fmod(hue / 60.f, 2.f) - 1)), m = v - c;
        float r = 0, g = 0, bl = 0;
        if (hue < 60) { r = c; g = x; } else if (hue < 120) { r = x; g = c; } else if (hue < 180) { g = c; bl = x; }
        else if (hue < 240) { g = x; bl = c; } else if (hue < 300) { r = x; bl = c; } else { r = c; bl = x; }
        return RGB(static_cast<int>((r + m) * 255), static_cast<int>((g + m) * 255), static_cast<int>((bl + m) * 255));
    };
    const float hue = static_cast<float>(h % 360);
    *a = hsv(hue, 0.62f, 0.62f);
    *b = hsv(std::fmod(hue + 40.f, 360.f), 0.70f, 0.30f);
}

// ---- steps and games --------------------------------------------------------------------------------------------------------------
enum Page { P_LIB, P_GAME, P_SET, P_PC, P_COUNT };
enum StepId { S_EXTRACT, S_BUILD, S_CORPUS, S_TRANSLATE, S_PROGRAMS, S_PREPARE, S_COUNT };
const wchar_t* kStepName[S_COUNT] = {L"Extract the game files", L"Build the game program", L"Find the game's shaders", L"Translate the shaders",
                                     L"List the shader programs", L"Compile the shaders for your graphics card"};
// What a step takes on a 16-thread PC for a full-size game (about 25 GB), before anything is measured here. CPU-bound steps scale with the
// PC's threads, the shader steps with the game's size; the time a step took last time on this PC (setup_state.ini secs_N) wins over both.
// ponytail: from the development machines (AGENTS.md); rough on purpose: the running steps' measured rates correct the rest
constexpr double kStepBaseSecs[S_COUNT] = {300, 1500, 840, 420, 100, 1800};
constexpr bool kStepCpu[S_COUNT] = {false, true, true, true, false, true};
constexpr bool kStepBySize[S_COUNT] = {true, false, true, true, true, true};
const wchar_t* kStepShort[S_COUNT] = {L"extract", L"build", L"corpus", L"translate", L"programs", L"prepare"};
// What a step needs finished first (a dependency that is not part of the job counts as finished): everything needs the extracted dump,
// TRANSLATE the shader corpus and the built exe, PREPARE the translations, the program list and the exe.
const std::vector<int> kStepDeps[S_COUNT] = {{}, {S_EXTRACT}, {S_EXTRACT}, {S_CORPUS, S_BUILD}, {S_EXTRACT}, {S_TRANSLATE, S_PROGRAMS, S_BUILD}};

struct GameItem;
// One step of a game's job: its child process and progress. Steps run in parallel when their dependencies are done.
struct StepRun {
    GameItem* g = nullptr;
    int id = 0;
    Child child;
    bool planned = false, started = false, finished = false, ok = false, result_ok = false;
    uint64_t done = 0, total = 0;
    std::string phase;                   // the @progress label (game_build.py: verify, lift, compile, link, finish)
    std::string error;
    std::chrono::steady_clock::time_point t0;
    // first progress report of the step: the rate counts from here, so a resumed step (done > 0 at once) is not taken as fast
    bool seen = false;
    uint64_t done0 = 0;
    std::chrono::steady_clock::time_point t1;
    bool running() const { return started && !finished; }
    std::chrono::steady_clock::time_point t_end;   // when it ended (finished)
    double elapsed() const {
        if (t0 == std::chrono::steady_clock::time_point{}) return 0;
        return std::chrono::duration<double>((finished && t_end > t0 ? t_end : std::chrono::steady_clock::now()) - t0).count();
    }
    void reset() {
        started = finished = ok = result_ok = seen = false;
        done = total = done0 = 0;
        phase.clear();
        error.clear();
        t0 = t_end = {};
    }
    // Seconds left, -1 while there is no fair guess yet (under 20 s or under 2% of the work measured).
    int secs_left() const {
        const auto now = std::chrono::steady_clock::now();
        if (id == S_BUILD) {   // phases with their own counts: the weighted fraction over the whole step
            const double f = fraction(), secs = std::chrono::duration<double>(now - t0).count();
            return f >= 0.05 && f < 1 && secs > 20 ? static_cast<int>(secs / f * (1 - f)) : -1;
        }
        const double secs = std::chrono::duration<double>(now - t1).count();
        if (!seen || !total || done <= done0 || done >= total || secs < 20 || (done - done0) * 50 < total) return -1;
        // ponytail: linear rate; the preparation does the expensive programs first, so early guesses run long
        return static_cast<int>(secs / static_cast<double>(done - done0) * static_cast<double>(total - done));
    }
    // How far the step is, 0..1. The game build reports its phases one after another, each 0..total of its own:
    // weighted into one range so the bar never jumps back and the time estimate looks at the whole step.
    double fraction() const {
        if (!total) return 0;
        const double part = static_cast<double>(std::min(done, total)) / static_cast<double>(total);
        // ponytail: fixed weights (compile dominates, lift ~1/5 on 16 cores); measure the phases' times if the estimate drifts
        static const struct { const char* name; double from, w; } kBuild[] = {
            {"verify", 0.00, 0.02}, {"lift", 0.02, 0.20}, {"compile", 0.22, 0.73}, {"link", 0.95, 0.04}, {"finish", 0.99, 0.01}};
        if (id == S_BUILD)
            for (const auto& p : kBuild)
                if (phase == p.name) return p.from + p.w * part;
        return part;
    }
};

std::wstring thousands(uint64_t v) {
    std::wstring d = std::to_wstring(v);
    for (int i = static_cast<int>(d.size()) - 3; i > 0; i -= 3) d.insert(static_cast<size_t>(i), L",");
    return d;
}
// "under a minute", "about 5 min", "about 1 h 20 min"
std::wstring span_text(double secs) {
    const int left = static_cast<int>(secs + 0.5);
    if (left < 60) return L"under a minute";
    if (left < 5400) return L"about " + std::to_wstring((left + 59) / 60) + L" min";
    const int m = (left + 299) / 300 * 5;   // whole 5 minutes: the hour figures are rough anyway
    return L"about " + std::to_wstring(m / 60) + L" h" + (m % 60 ? L" " + std::to_wstring(m % 60) + L" min" : L"");
}
// "42% · 1,234 of 39,363" (the counts only for the shader steps, which count blocks / programs)
std::wstring step_counts(const StepRun& r) {
    if (!r.total) return L"Starting";
    std::wstring t = std::to_wstring(static_cast<int>(r.fraction() * 100)) + L"%";
    if (r.id == S_CORPUS || r.id == S_TRANSLATE || r.id == S_PREPARE) t += L" · " + thousands(std::min(r.done, r.total)) + L" of " + thousands(r.total);
    return t;
}

struct GameItem {
    std::string id;
    const GameDef* def = nullptr;        // null: a dump no manifest knows (no port yet)
    std::string title_id;
    std::wstring title, short_title;
    Config cfg;
    Layout lay;
    State st;
    GameId gid;
    std::wstring exe;                    // the game exe (may not exist: not built)
    GameBuild build;                     // how this release builds the exe (build.ok false: it cannot)
    bool need[S_COUNT] = {};
    std::wstring why[S_COUNT];
    bool retranslate_all = false;
    std::wstring prep_check;             // the last --check answer ("ready", "needed ...", "error ..."); empty: not known
    bool checking_prep = false;
    Child check_child;
    // its job
    bool queued = false, running = false, cancel_requested = false, force_prepare = false;
    bool stopping = false;               // no new steps start; the running ones were asked to stop (a step failed, or Cancel)
    bool killed = false;
    std::chrono::steady_clock::time_point stop_at;
    bool play_after = false;             // Play pressed before the setup was done: start the game when it is
    bool play_now = false;               // "Play now": the setup was stopped, start the game once its steps have ended
    bool game_running = false;           // the library started the game and it has not ended yet
    // The game program exists and no step that rewrites it runs: the game can start now, shaders translate and compile while it plays.
    bool can_play_now() const {
        if (!def || !gid.ok || !exe_ok() || queued) return false;
        if (!running) return !need[S_EXTRACT];
        for (int s : {S_EXTRACT, S_BUILD}) if (run[s].planned && !run[s].finished) return false;
        return true;
    }
    StepRun run[S_COUNT];
    double size_scale = 1;               // the game's size against a full-size game (the shader steps' estimates)
    std::chrono::steady_clock::time_point job_t0;
    // ---- time estimates: one honest figure for everything that is left ----
    bool planned(int s) const { return running ? run[s].planned : need[s]; }
    // what step s takes from its start, before it runs: measured here last time, else the base figure for this PC and game size
    double expected(int s) const {
        if (st.secs[s] > 0) return st.secs[s];
        const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
        double t = kStepBaseSecs[s];
        if (kStepCpu[s]) t *= 16.0 / static_cast<double>(hw);
        if (kStepBySize[s]) t *= size_scale;
        return std::max(10.0, t);
    }
    // How this PC compares to the base figures, from the running steps that report a rate (1 while none does). Steps measured here
    // before keep their own figure.
    double speed() const {
        double sum = 0;
        int n = 0;
        for (const StepRun& r : run) {
            if (!r.running() || st.secs[r.id] > 0) continue;
            const int left = r.secs_left();
            if (left < 0) continue;
            sum += (r.elapsed() + left) / expected(r.id);
            ++n;
        }
        return n ? std::clamp(sum / n, 0.33, 3.0) : 1.0;
    }
    double remaining(int s, double sp) const {
        if (!planned(s)) return 0;
        const StepRun& r = run[s];
        const double e = st.secs[s] > 0 ? expected(s) : expected(s) * sp;
        if (running && r.finished) return 0;
        if (running && r.running()) {
            const int left = r.secs_left();
            if (left >= 0) return left;
            return std::max(e - r.elapsed(), 0.1 * e);   // no rate yet: the estimate minus what ran, never "done" before it is
        }
        return e;
    }
    // Seconds until every planned step is done. Steps run side by side once their inputs are done, so it is the longest chain of
    // remaining times through the dependencies (every dependency has a lower index than its step).
    double eta() const {
        const double sp = speed();
        double fin[S_COUNT] = {};
        double end = 0;
        for (int s = 0; s < S_COUNT; ++s) {
            double start = 0;
            for (int d : kStepDeps[s]) start = std::max(start, fin[d]);
            fin[s] = start + remaining(s, sp);
            end = std::max(end, fin[s]);
        }
        return end;
    }
    // How far the whole job is, 0..1: the time it has run against that time plus what is left.
    double overall() const {
        if (!running) return 0;
        const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - job_t0).count(), left = eta();
        return el + left > 0 ? std::clamp(el / (el + left), 0.0, 0.999) : 0;
    }
    int nrunning() const { int n = 0; for (const StepRun& r : run) n += r.running(); return n; }
    std::string error;
    std::deque<std::wstring> log;
    std::vector<Check> checks;           // its own checks (disk, program, dump)
    bool in_library() const { return !cfg.source.empty(); }
    bool ready() const { for (bool n : need) if (n) return false; return true; }
    bool exe_ok() const { return file_exists(exe); }
    bool playable() const { return def && gid.ok && !need[S_EXTRACT] && exe_ok() && !running; }
    int todo() const { int n = 0; for (bool x : need) n += x; return n; }
};

// The game's size against a full-size game (25 GB): the .nup's size, or the extracted romfs's (walked once per game and kept).
double size_scale_of(const GameItem& g) {
    static std::map<std::wstring, double> memo;
    std::error_code ec;
    uint64_t bytes = 0;
    if (source_kind(g.cfg.source) == SourceKind::nup) bytes = fs::file_size(g.cfg.source, ec);
    else if (!g.lay.game.empty()) {
        const std::wstring romfs = join(g.lay.game, L"romfs");
        if (auto it = memo.find(romfs); it != memo.end()) return it->second;
        for (fs::recursive_directory_iterator it(romfs, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec))
            if (it->is_regular_file(ec)) bytes += it->file_size(ec);
        return memo[romfs] = std::clamp(static_cast<double>(bytes) / 25e9, 0.002, 4.0);
    }
    return bytes ? std::clamp(static_cast<double>(bytes) / 25e9, 0.002, 4.0) : 1.0;
}

struct Status {
    std::wstring text;
    COLORREF color = kMuted;
    double progress = -1;   // 0..1 while a step runs
};

// Owner-drawn buttons: kind of each (primary = accent fill).
enum class Btn { primary, normal, danger };

enum Id : int {
    ID_CARDS = 100, ID_STEPS, ID_LOG, ID_REQ_LIST, ID_REQ_FIX,
    ID_PLAY, ID_PREP, ID_CANCEL, ID_REBUILD, ID_SRC_NUP, ID_SRC_FOLDER, ID_DATA, ID_OPEN_DATA, ID_SHORTCUT, ID_REMOVE,
    ID_RECHECK, ID_COPY, ID_OPEN_INI, ID_ADD, ID_SETTINGS, ID_MORE, ID_DETAILS, ID_PLAY_AS_IS,
    IDM_ADD = 1000, IDM_EXIT, IDM_RECHECK, IDM_ABOUT, IDM_REPORT, IDM_PAGE0, IDM_BACK = IDM_PAGE0 + 8, IDM_PREV_GAME, IDM_NEXT_GAME,
};

class App {
public:
    int run(HINSTANCE inst, int show);

private:
    static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l);
    static LRESULT CALLBACK cards_proc(HWND h, UINT m, WPARAM w, LPARAM l);
    LRESULT on(UINT m, WPARAM w, LPARAM l);
    LRESULT on_cards(HWND h, UINT m, WPARAM w, LPARAM l);
    int S(int v) const { return MulDiv(v, dpi_, 96); }
    float Sf(float v) const { return v * static_cast<float>(dpi_) / 96.f; }
    void make_fonts();
    void create_controls();
    HWND button(const wchar_t* t, int id, Btn kind);
    void layout();
    void show_page(int p);
    void paint(HDC dc);
    void paint_side(gp::Graphics& g, const RECT& rc);
    void nav_layout();
    void paint_tabs(gp::Graphics& g, const gp::Font& bold);
    void paint_report(gp::Graphics& g);
    void paint_game_header(gp::Graphics& g, const RECT& rc);
    void paint_cards(HWND h, HDC dc);
    void draw_button(const DRAWITEMSTRUCT* d);
    void command(int id, int code);
    LRESULT notify(NMHDR* n);
    void apply_theme(HWND h);

    // games
    void load_games();
    GameItem* sel();
    std::string first_added();
    GameItem* item(const std::string& id);
    void select(const std::string& id);
    void save_lib();
    void evaluate(GameItem& g);
    Status status_of(const GameItem& g) const;
    void add_game();
    void add_dump(const std::wstring& path, bool quiet);
    void set_source(GameItem& g, const std::wstring& s);
    void choose_source(GameItem& g, bool folder);
    void choose_data(GameItem& g);
    void remove_game(GameItem& g);

    // preparation
    void start_requirements();
    void start_prepare_check(GameItem& g);
    void enqueue(GameItem& g, bool force_prepare);
    void run_queue();
    void schedule(GameItem& g);
    void start_step(GameItem& g, int s);
    void fail_job(GameItem& g, const std::string& error);
    void request_stop(GameItem& g);
    void finish_job(GameItem& g);
    void step_line(StepRun& r, const std::string& l);
    void step_exit(StepRun& r, DWORD code);
    void cancel(GameItem& g);
    void log(GameItem& g, const std::wstring& s);
    bool write_game_ini(GameItem& g);
    std::wstring game_cmd(const GameItem& g, const std::wstring& extra) const;
    std::wstring py_args(const std::wstring& script, const std::wstring& rest) const;
    void rebuild_dialog(GameItem& g);
    void play(GameItem& g);
    void play_now(GameItem& g);          // stop the setup (what is done stays) and start the game without its "Preparing shaders" window
    void make_shortcut(GameItem& g);
    void about();
    void report(GameItem* g);
    void more_menu();
    void primary(GameItem& g);           // the one button: add the dump, set up, or play
    void try_experimental(GameItem& g);  // a title without a port: write its manifest from the dump (make_manifest.py), then set up as usual
    std::wstring primary_label(const GameItem& g, bool* enabled) const;
    void refresh();                       // texts and enabled states of the controls of the current page
    void update_settings();               // the settings panel shows the selected game
    void download_dlss();                 // licence dialog, then fetch_dlss.py as a child
    void handle_args(const std::wstring& line, bool live);   // a second start's command line (--game, --page, --tab, --add)
    Child dlss_child_;

    HINSTANCE inst_ = nullptr;
    HWND wnd_ = nullptr, cards_ = nullptr, settings_ = nullptr;
    int dpi_ = 96;
    HFONT font_ = nullptr, bold_ = nullptr, small_ = nullptr, title_ = nullptr, big_ = nullptr;
    HBRUSH bg_brush_ = nullptr, field_brush_ = nullptr;
    ULONG_PTR gdiplus_ = 0;
    int page_ = P_LIB;
    std::vector<std::pair<HWND, int>> ctls_;   // control, page (-1: never shown by show_page)
    std::vector<std::pair<HWND, Btn>> btns_;
    HWND log_ = nullptr;
    RECT pc_rect_area_{};                 // This PC: where the checks are painted
    int pc_scroll_ = 0, pc_content_h_ = 0;
    std::vector<const Check*> pc_checks() const;   // every check, problems first
    void paint_pc(gp::Graphics& g);
    HWND b_more_ = nullptr;
    HWND b_play_ = nullptr, b_cancel_ = nullptr, b_recheck_ = nullptr, b_copy_ = nullptr, b_open_ini_ = nullptr;
    RECT brand_rect_{}, pc_rect_{}, add_rect_{}, report_rect_{}, tab_rect_[2] = {};
    std::vector<std::pair<RECT, GameItem*>> nav_games_;   // the side menu: one entry per game in the library, then This PC
    RECT prog_rect_{}, steps_rect_{}, details_rect_{};
    void paint_steps(gp::Graphics& g, const GameItem& s);
    int hover_nav_ = -1;                 // 0.. a game, kHovPc, kHovBrand, kHovAdd, kHovReport, kHovTab + 0/1
    bool details_ = false;               // the game page shows the setup's output under the steps

    // cards
    struct Hit { RECT r; GameItem* g; int action; };   // action: 0 select, 1 play, 2 prepare, 3 cancel, 4 add
    std::vector<Hit> hits_;
    int hover_hit_ = -1;
    GameItem* hover_game_ = nullptr;
    int scroll_ = 0, content_h_ = 0;
    int kb_card_ = 0, cards_cols_ = 1, cards_n_ = 1;   // the card the keyboard is on (arrows move it, Enter opens it)

    Tools tools_;
    std::vector<GameDef> defs_;
    Library lib_;
    std::vector<std::unique_ptr<GameItem>> games_;
    std::deque<GameItem*> queue_;
    GameItem* active_ = nullptr;
    bool closing_ = false;                // the window closes when the running job has stopped
    std::vector<Check> sys_checks_;
    bool checks_running_ = false;
    std::wstring game_args_;
    std::vector<std::wstring> add_paths_;   // --add
    bool auto_start_ = false;
    int start_tab_ = -1;                  // --tab: the settings tab to open
};

App* g_app = nullptr;

std::wstring step_state_text(bool need, const std::wstring& why) { return need ? (why.empty() ? L"To do" : L"To do: " + why) : (why.empty() ? L"Done" : why); }

// ---- games ---------------------------------------------------------------------------------------------------------------------------
void App::load_games() {
    defs_ = find_game_defs(tools_.exe_dir, tools_.dev_root);
    lib_ = load_library(defs_);
    auto make = [&](const std::string& id) -> GameItem& {
        if (GameItem* g = item(id)) return *g;
        games_.push_back(std::make_unique<GameItem>());
        games_.back()->id = id;
        return *games_.back();
    };
    // the player's games first (in the library's order), then every known game without a dump yet
    for (const LibraryEntry& e : lib_.games) make(e.id);
    for (const GameDef& d : defs_) make(d.id);
    for (auto& up : games_) {
        GameItem& g = *up;
        g.def = nullptr;
        for (const GameDef& d : defs_) if (d.id == g.id) g.def = &d;
        const LibraryEntry* e = lib_.find(g.id);
        g.title_id = e ? e->title_id : g.def ? g.def->title_id : "";
        if (g.def) { g.title = widen(g.def->title); g.short_title = widen(g.def->short_title); }
        else {
            g.title = e && !e->name.empty() ? widen(e->name) : L"Unknown game " + widen(g.title_id);
            g.short_title = e && !e->name.empty() ? widen(e->name) : widen(g.title_id);
        }
        g.cfg.source = e ? e->source : L"";
        g.cfg.data_dir = e ? e->data_dir : default_data_dir(g.id);
        g.cfg.game_ini = g.def ? widen(g.def->ini) : L"game.ini";
        g.exe = g.def ? game_exe_for(tools_, *g.def, g.cfg) : L"";
        evaluate(g);
    }
    if (!sel()) lib_.selected = first_added();
}

// the first game the player added (the library lists only those), or none
std::string App::first_added() {
    for (auto& g : games_) if (g->in_library()) return g->id;
    return {};
}

GameItem* App::item(const std::string& id) {
    for (auto& g : games_) if (g->id == id) return g.get();
    return nullptr;
}
// only a game the player added can be selected: known games without a dump stay hidden until a dump of them is added
GameItem* App::sel() {
    GameItem* g = item(lib_.selected);
    return g && g->in_library() ? g : nullptr;
}

void App::select(const std::string& id) {
    if (GameItem* g = item(id); !g || !g->in_library()) return;
    lib_.selected = id;
    save_lib();
    GameItem& g = *sel();
    evaluate(g);
    start_prepare_check(g);
    // the log edit shows the selected game's output
    std::wstring all;
    for (const std::wstring& l : g.log) all += l + L"\r\n";
    SetWindowTextW(log_, all.c_str());
    update_settings();
    refresh();
    InvalidateRect(wnd_, nullptr, FALSE);
}

// The NVIDIA RTX SDKs LICENSE does not fit a GPL package, so the DLL is never shipped: the player sees the licence, then
// fetch_dlss.py downloads it from NVIDIA's repository (sha256 checked) into <exe dir>\upscalers\streamline.
void App::download_dlss() {
    if (dlss_child_.running()) return;
    if (tools_.python.empty() || tools_.fetch_dlss.empty()) { MessageBoxW(wnd_, L"Python or fetch_dlss.py is missing (This PC page).", kAppName, MB_ICONWARNING); return; }
    TASKDIALOGCONFIG c{sizeof c};
    c.hwndParent = wnd_;
    c.dwFlags = TDF_ENABLE_HYPERLINKS | TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
    c.dwCommonButtons = TDCBF_OK_BUTTON | TDCBF_CANCEL_BUTTON;
    c.pszWindowTitle = kAppName;
    c.pszMainInstruction = L"Download NVIDIA DLSS?";
    c.pszContent = L"The DLSS file (nvngx_dlss.dll, about 56 MB) comes from NVIDIA's own repository and is under the "
                   L"<a href=\"https://github.com/NVIDIA/DLSS/blob/main/LICENSE.txt\">NVIDIA RTX SDKs LICENSE</a>. "
                   L"By pressing OK you accept that licence.";
    c.pfCallback = [](HWND, UINT n, WPARAM, LPARAM l, LONG_PTR) -> HRESULT {
        if (n == TDN_HYPERLINK_CLICKED) ShellExecuteW(nullptr, L"open", reinterpret_cast<const wchar_t*>(l), nullptr, nullptr, SW_SHOWNORMAL);
        return S_OK;
    };
    int pick = 0;
    if (FAILED(TaskDialogIndirect(&c, &pick, nullptr, nullptr)) || pick != IDOK) return;
    HWND w = wnd_;
    auto err = std::make_shared<std::wstring>();
    std::wstring e;
    if (!dlss_child_.start(tools_.python, py_args(tools_.fetch_dlss, L"--accept-license --out " + quote(dlss_dir(tools_))), tools_.exe_dir, false,
            [err](const std::string& l) { if (l.rfind("@error ", 0) == 0) *err = widen(l.substr(7)); },
            [w, err](DWORD code) {
                if (code != 0 && err->empty()) *err = L"The download ended with code " + std::to_wstring(code) + L".";
                PostMessageW(w, kMsgDlss, 0, reinterpret_cast<LPARAM>(new std::wstring(code == 0 ? L"" : *err)));
            }, &e))
        MessageBoxW(wnd_, e.c_str(), kAppName, MB_ICONWARNING);
}

void App::update_settings() {
    settings::set_dlss(settings_, dlss_state(tools_), [this] { download_dlss(); });
    GameItem* g = sel();
    if (!g) { settings::set_game(settings_, L"", L"", L"Add a game to the library first."); return; }
    evaluate(*g);
    std::wstring why;
    if (!g->def) why = L"No port of this game yet.";
    else if (!g->in_library()) why = L"Add your dump first.";
    else if (g->lay.game.empty()) why = L"Available once the game is set up.";
    settings::set_game(settings_, g->title, why.empty() ? join(g->lay.game, L"controls.ini") : L"", why);
}

void App::save_lib() {
    // the library holds the games with a dump; keep the order the player added them in
    Library out;
    out.selected = lib_.selected;
    for (const LibraryEntry& e : lib_.games) {
        const GameItem* g = item(e.id);
        if (!g || !g->in_library()) continue;
        LibraryEntry n = e;
        n.source = g->cfg.source;
        n.data_dir = g->cfg.data_dir;
        out.games.push_back(n);
    }
    for (auto& up : games_) {
        if (!up->in_library() || out.find(up->id)) continue;
        LibraryEntry n;
        n.id = up->id;
        n.title_id = up->title_id;
        n.source = up->cfg.source;
        n.data_dir = up->cfg.data_dir;
        out.games.push_back(n);
    }
    lib_ = out;
    save_library(lib_);
}

void App::evaluate(GameItem& g) {
    g.lay = layout_of(g.cfg);
    g.st = load_state(g.lay.state_file);
    g.gid = game_id_of(g.lay.game);
    if (!g.running && g.in_library()) g.size_scale = size_scale_of(g);
    if (g.def) { g.build = game_build_of(tools_, *g.def); g.exe = game_exe_for(tools_, *g.def, g.cfg); }
    for (int i = 0; i < S_COUNT; ++i) { g.need[i] = false; g.why[i].clear(); }
    const SourceKind sk = source_kind(g.cfg.source);
    const std::string gid = g.gid.ok ? g.gid.text() : std::string();
    if (sk == SourceKind::nup) {
        if (g.lay.game.empty()) g.need[S_EXTRACT] = true;
        else if (g.st.extracted_from != nup_stamp(g.cfg.source)) { g.need[S_EXTRACT] = true; g.why[S_EXTRACT] = L"another .nup file was chosen"; }
    } else if (sk == SourceKind::folder) {
        g.why[S_EXTRACT] = g.lay.game.empty() ? L"the folder has no exefs\\main and romfs\\" : L"Not needed: an extracted folder";
    } else {
        g.need[S_EXTRACT] = true;
        g.why[S_EXTRACT] = L"add your dump first";
    }
    const bool known = g.gid.ok && !g.need[S_EXTRACT];
    auto later = [&](int s, const std::wstring& w) { g.need[s] = true; if (g.why[s].empty()) g.why[s] = w; };
    if (!g.build.ok) g.why[S_BUILD] = g.exe_ok() ? L"Not needed: the game program is there" : L"This package cannot build it";
    if (!known) {
        for (int s : {S_CORPUS, S_TRANSLATE, S_PROGRAMS, S_PREPARE}) later(s, L"after the game files");
        if (g.build.ok) later(S_BUILD, L"after the game files");
        return;
    }
    if (g.build.ok) {
        if (!g.exe_ok()) later(S_BUILD, L"from your dump, once (15-40 minutes)");
        else if (g.st.built_for != gid + "|" + g.build.stamp) later(S_BUILD, L"a new release or another game version");
        else g.why[S_BUILD] = L"Built from your dump";
    }
    if (g.st.corpus_game != gid || !file_exists(join(g.lay.corpus, L"unique.tsv")))
        later(S_CORPUS, g.st.corpus_game.empty() ? L"" : L"the game changed (an update)");
    const std::string ts = translator_stamp(tools_.translator);
    g.retranslate_all = !g.st.translated_with.empty() && g.st.translated_with != ts && file_exists(join(g.lay.spirv, L"results.tsv"));
    if (g.retranslate_all) later(S_TRANSLATE, L"a new translator: every shader again");
    else if (g.need[S_CORPUS] || g.st.translated_game != gid || g.st.translated_with != ts)
        later(S_TRANSLATE, g.st.translated_game.empty() || g.need[S_CORPUS] ? L"" : L"new shaders of the game update");
    if (g.st.programs_game != gid || !file_exists(g.lay.programs)) later(S_PROGRAMS, g.st.programs_game.empty() ? L"" : L"the game changed");
    if (!g.exe_ok() || g.need[S_BUILD]) {
        for (int s : {S_TRANSLATE, S_PREPARE}) later(s, g.build.ok ? L"after the game program" : L"needs the game program (not built)");
    }
    if (g.need[S_CORPUS] || g.need[S_TRANSLATE] || g.need[S_PROGRAMS]) later(S_PREPARE, L"after the steps above");
    else if (g.prep_check == L"ready") g.why[S_PREPARE] = L"Done for this graphics card and driver";
    else if (g.prep_check.rfind(L"needed", 0) == 0) {
        std::wstring w = g.prep_check.size() > 7 ? g.prep_check.substr(7) : L"";
        if (w.find(L"another GPU") != std::wstring::npos) w = L"your graphics card or driver changed";
        later(S_PREPARE, w);
    } else if (g.prep_check.rfind(L"error", 0) == 0) later(S_PREPARE, L"cannot check: " + g.prep_check.substr(std::min<size_t>(6, g.prep_check.size())));
    else later(S_PREPARE, g.checking_prep ? L"checking..." : L"");
}

Status App::status_of(const GameItem& g) const {
    Status s;
    if (!g.def) { s.text = L"No port yet"; s.color = kMuted; return s; }
    if (!g.in_library()) { s.text = L"Add your dump"; s.color = kAccentHi; return s; }
    const bool exp = g.def->generic;
    if (g.running) {
        s.color = kAccentHi;
        s.text = g.cancel_requested ? L"Stopping" : L"Setting up";
        // one bar and one time for the whole job (every step that is left, side by side where they can run so)
        s.progress = g.overall();
        if (!g.cancel_requested) {
            wchar_t b[32];
            std::swprintf(b, 32, L" \u00B7 %d%%", static_cast<int>(s.progress * 100));
            s.text += b + std::wstring(L" \u00B7 ") + span_text(g.eta()) + L" left";
        }
        return s;
    }
    if (g.queued) { s.text = L"Waiting"; s.color = kAccentHi; s.progress = 0; return s; }
    if (g.game_running) { s.text = L"Playing"; s.color = kOk; return s; }
    if (!g.error.empty()) { s.text = L"Problem"; s.color = kBad; return s; }
    if (!g.exe_ok() && !g.build.ok) { s.text = L"Not built yet"; s.color = kWarn; return s; }
    if (g.ready()) { s.text = exp ? L"Ready (experimental)" : L"Ready"; s.color = kOk; return s; }
    s.text = exp ? L"Setup needed (experimental)" : L"Setup needed";
    s.color = kWarn;
    return s;
}

std::wstring App::primary_label(const GameItem& g, bool* enabled) const {
    *enabled = true;
    if (!g.def) {
        *enabled = g.in_library() && experimental_possible(tools_);
        return *enabled ? L"Try experimental build" : L"No port yet";
    }
    if (!g.in_library()) return L"Add dump";
    if (g.running || g.queued) { *enabled = false; return L"Setting up"; }
    if (g.game_running) { *enabled = false; return L"Playing"; }
    if (!g.exe_ok() && !g.build.ok) { *enabled = false; return L"Not built yet"; }
    if (!g.error.empty() && !g.ready()) return L"Try again";
    return L"Play";
}

void App::primary(GameItem& g) {
    bool en = false;
    const std::wstring l = primary_label(g, &en);
    if (!en) return;
    if (!g.def) { try_experimental(g); return; }
    if (!g.in_library()) { choose_source(g, false); return; }
    if (g.ready()) { play(g); return; }
    g.play_after = true;   // set up once, then the game starts
    enqueue(g, false);
}

void App::try_experimental(GameItem& g) {
    if (g.def || !g.in_library() || !experimental_possible(tools_)) return;
    const std::wstring ask = L"There is no port of " + g.title + L" yet.\r\n\r\n"
        L"An experimental build translates the game with nothing made for it: it may not start, may show a black window or may crash. "
        L"Building it takes 15-40 minutes and about 4 GB of disk space while it runs.\r\n\r\nTry it?";
    if (MessageBoxW(wnd_, ask.c_str(), kAppName, MB_OKCANCEL | MB_ICONQUESTION) != IDOK) return;
    SetCursor(LoadCursor(nullptr, IDC_WAIT));
    const std::wstring source = g.cfg.source;
    const std::string old_id = g.id, title_id = g.title_id, name = narrow(g.title);
    // the ExeFS is all the manifest needs: a .nup gives it in seconds (--exefs-only), a folder has it already
    std::wstring dump = source;
    const std::wstring scratch = join(g.cfg.data_dir, L"manifest_exefs");
    std::string out;
    DWORD code = 1;
    if (source_kind(source) == SourceKind::nup) {
        dump = scratch;
        if (!run_capture(tools_.python, py_args(tools_.nup_extract, quote(source) + L" " + quote(scratch) + L" --exefs-only"), 600000, &out, &code) || code != 0) {
            MessageBoxW(wnd_, (L"Reading the game files failed.\r\n\r\n" + widen(out.substr(0, 600))).c_str(), kAppName, MB_OK | MB_ICONWARNING);
            return;
        }
    }
    const std::string id = experimental_id(title_id);
    const std::wstring manifest = join(join(experimental_dir(), widen(id)), L"game.toml");
    const bool ok = run_capture(tools_.python, py_args(tools_.make_manifest, quote(dump) + L" --out " + quote(manifest) + L" --name " + quote(widen(name))), 120000, &out, &code) &&
                    code == 0;
    if (dump == scratch) { std::error_code ec; std::filesystem::remove_all(scratch, ec); }
    if (!ok) {
        MessageBoxW(wnd_, (L"This dump cannot be built.\r\n\r\n" + widen(out.substr(0, 600))).c_str(), kAppName, MB_OK | MB_ICONWARNING);
        return;
    }
    // the manifest makes it a known game: drop the unknown entry and add the dump again under the new id
    g.cfg.source.clear();
    save_lib();
    games_.erase(std::remove_if(games_.begin(), games_.end(), [&](const auto& p) { return p->id == old_id; }), games_.end());
    load_games();
    add_dump(source, true);
    if (GameItem* n = item(id); n && n->def) {
        select(id);
        show_page(P_GAME);
        primary(*n);                 // set up (build, shaders) and start, as Play does
    } else {
        MessageBoxW(wnd_, L"The manifest was written, but the library cannot read it.", kAppName, MB_OK | MB_ICONWARNING);
    }
    refresh();
    InvalidateRect(wnd_, nullptr, FALSE);
}

void App::add_game() {
    // a .nup file or a folder: the file dialog picks a file; Shift+click (or Cancel then the folder dialog) is not discoverable, so ask first
    TASKDIALOG_BUTTON b[] = {{101, L".nup file"}, {102, L"Extracted folder"}};
    TASKDIALOGCONFIG c{sizeof c};
    c.hwndParent = wnd_;
    c.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
    c.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    c.pszWindowTitle = kAppName;
    c.pszMainInstruction = L"Add a game";
    c.pszContent = L"Tip: you can also drop a .nup file or a folder on this window.";
    c.cButtons = 2;
    c.pButtons = b;
    int pick = 0;
    if (FAILED(TaskDialogIndirect(&c, &pick, nullptr, nullptr)) || (pick != 101 && pick != 102)) return;
    IFileOpenDialog* d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return;
    if (pick == 101) {
        COMDLG_FILTERSPEC f[] = {{L"NUP package (*.nup)", L"*.nup"}, {L"All files", L"*.*"}};
        d->SetFileTypes(2, f);
        d->SetTitle(L"Choose your .nup package");
    } else {
        DWORD o = 0;
        d->GetOptions(&o);
        d->SetOptions(o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        d->SetTitle(L"Choose your extracted game folder");
    }
    std::wstring path;
    if (SUCCEEDED(d->Show(wnd_))) {
        IShellItem* it = nullptr;
        if (SUCCEEDED(d->GetResult(&it))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) { path = p; CoTaskMemFree(p); }
            it->Release();
        }
    }
    d->Release();
    if (!path.empty()) add_dump(path, false);
}

// quiet: no questions or notes (the --add command line option).
void App::add_dump(const std::wstring& path, bool quiet) {
    if (source_kind(path) == SourceKind::unsupported) {
        if (!quiet)
            MessageBoxW(wnd_, L"NSP, XCI, NSZ and XCZ files are encrypted, and this program does not handle console keys.\r\n\r\n"
                              L"Extract the game with your own tools into a folder with the decrypted exefs and romfs (the update's, if you have it), "
                              L"or make a .nup package, and add that.", kAppName, MB_OK | MB_ICONINFORMATION);
        return;
    }
    SetCursor(LoadCursor(nullptr, IDC_WAIT));
    const DumpInfo info = identify_dump(path, tools_.python, tools_.python_args, tools_.nup_extract);
    if (!info.ok) {
        if (!quiet) MessageBoxW(wnd_, (L"Not a game dump.\r\n\r\n" + info.error).c_str(), kAppName, MB_OK | MB_ICONWARNING);
        return;
    }
    const GameDef* def = nullptr;
    for (const GameDef& gd : defs_) if (_stricmp(gd.title_id.c_str(), info.title_id.c_str()) == 0) def = &gd;
    const std::string id = def ? def->id : "title-" + info.title_id;
    GameItem* g = item(id);
    if (g && g->running) { if (!quiet) MessageBoxW(wnd_, L"This game is being set up. Cancel that first.", kAppName, MB_OK | MB_ICONINFORMATION); return; }
    if (g && g->in_library() && g->cfg.source != path && !quiet &&
        MessageBoxW(wnd_, (g->title + L" is already in your library. Use this dump instead?").c_str(), kAppName,
                    MB_YESNO | MB_ICONQUESTION) != IDYES)
        return;
    if (!g) {
        games_.push_back(std::make_unique<GameItem>());
        g = games_.back().get();
        g->id = id;
        g->def = def;
        g->title_id = info.title_id;
        g->title = info.name.empty() ? L"Unknown game " + widen(info.title_id) : widen(info.name);
        g->short_title = info.name.empty() ? widen(info.title_id) : widen(info.name);
        g->cfg.data_dir = default_data_dir(id);
        g->cfg.game_ini = L"game.ini";
    }
    if (!def && !info.name.empty()) { g->title = widen(info.name); g->short_title = g->title; }
    LibraryEntry* e = lib_.find(id);
    if (!e) { lib_.games.push_back({}); e = &lib_.games.back(); e->id = id; e->data_dir = g->cfg.data_dir; }
    e->title_id = info.title_id;
    e->dump_version = version_text(info.version);
    e->name = info.name;
    set_source(*g, path);
    select(id);
    show_page(P_GAME);
    std::wstring note;
    if (quiet) return;
    if (!def) {
        MessageBoxW(wnd_, experimental_possible(tools_) ? L"There is no port of this game yet. It stays in your library; Try experimental build "
                                                          L"translates it without any game-specific work (it may not start)."
                                                        : L"There is no port of this game yet. It stays in your library.",
                    kAppName, MB_OK | MB_ICONINFORMATION);
    } else if (!info.build_id.empty() && !def->main_build_id.empty() && _stricmp(info.build_id.c_str(), def->main_build_id.c_str()) != 0) {
        note = L"Your dump is another version than " + widen(def->dump_version) + L". The game may not start.";
        MessageBoxW(wnd_, note.c_str(), kAppName, MB_OK | MB_ICONWARNING);
    }
}

void App::set_source(GameItem& g, const std::wstring& s) {
    g.cfg.source = s;
    g.prep_check.clear();
    g.error.clear();
    save_lib();
    evaluate(g);
    start_prepare_check(g);
    start_requirements();
    refresh();
    InvalidateRect(cards_, nullptr, FALSE);
}

void App::choose_source(GameItem& g, bool folder) {
    IFileOpenDialog* d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return;
    if (folder) {
        DWORD o = 0;
        d->GetOptions(&o);
        d->SetOptions(o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        d->SetTitle(L"Choose your extracted game folder (exefs and romfs)");
    } else {
        COMDLG_FILTERSPEC f[] = {{L"NUP package (*.nup)", L"*.nup"}, {L"All files", L"*.*"}};
        d->SetFileTypes(2, f);
        d->SetTitle(L"Choose your .nup package");
    }
    if (SUCCEEDED(d->Show(wnd_))) {
        IShellItem* it = nullptr;
        if (SUCCEEDED(d->GetResult(&it))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                if (!lib_.find(g.id)) { lib_.games.push_back({}); lib_.games.back().id = g.id; lib_.games.back().title_id = g.title_id; }
                set_source(g, p);
                CoTaskMemFree(p);
            }
            it->Release();
        }
    }
    d->Release();
}

void App::choose_data(GameItem& g) {
    IFileOpenDialog* d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return;
    DWORD o = 0;
    d->GetOptions(&o);
    d->SetOptions(o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    d->SetTitle(L"Choose the folder the library writes to for this game");
    if (SUCCEEDED(d->Show(wnd_))) {
        IShellItem* it = nullptr;
        if (SUCCEEDED(d->GetResult(&it))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                g.cfg.data_dir = p;
                CoTaskMemFree(p);
                g.prep_check.clear();
                save_lib();
                evaluate(g);
                start_requirements();
                start_prepare_check(g);
            }
            it->Release();
        }
    }
    d->Release();
    refresh();
}

void App::remove_game(GameItem& g) {
    if (g.running || g.queued) return;
    // what the library made for this game, inside its data folder (never the player's own dump folder, never other files there)
    std::vector<std::wstring> names = {L"dump", L"program", L"shader_cache", L"manifest_exefs", L"setup_state.ini"};
    if (!g.cfg.game_ini.empty()) names.push_back(g.cfg.game_ini);
    if (g.cfg.data_dir.empty()) names.clear();
    uint64_t bytes = 0;
    std::error_code ec;
    for (const std::wstring& n : names) {
        const fs::path p = fs::path(g.cfg.data_dir) / n;
        if (fs::is_directory(p, ec)) {
            for (fs::recursive_directory_iterator it(p, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec))
                if (it->is_regular_file(ec)) bytes += it->file_size(ec);
        } else if (fs::is_regular_file(p, ec)) bytes += fs::file_size(p, ec);
    }
    bool wipe = false;
    if (bytes > 0) {
        const std::wstring wipe_text = L"Remove and delete its data\nFrees " + gib(bytes) + L": the extracted game, the built program and the shaders. Your own .nup file or dump folder stays.";
        TASKDIALOG_BUTTON b[] = {{301, wipe_text.c_str()},
                                 {302, L"Remove only\nKeeps the data, so adding the game again is quick."}};
        TASKDIALOGCONFIG c{sizeof c};
        c.hwndParent = wnd_;
        c.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
        c.dwCommonButtons = TDCBF_CANCEL_BUTTON;
        c.pszWindowTitle = kAppName;
        const std::wstring head = L"Remove " + g.title + L" from the library?";
        c.pszMainInstruction = head.c_str();
        c.cButtons = 2;
        c.pButtons = b;
        int pick = 0;
        if (FAILED(TaskDialogIndirect(&c, &pick, nullptr, nullptr)) || (pick != 301 && pick != 302)) return;
        wipe = pick == 301;
    } else if (MessageBoxW(wnd_, (L"Remove " + g.title + L" from the library? Nothing is deleted.").c_str(), kAppName, MB_YESNO | MB_ICONQUESTION) != IDYES)
        return;
    if (wipe) {
        SetCursor(LoadCursor(nullptr, IDC_WAIT));
        for (const std::wstring& n : names) fs::remove_all(fs::path(g.cfg.data_dir) / n, ec);
        fs::remove(g.cfg.data_dir, ec);   // only if now empty
    }
    g.cfg.source.clear();
    save_lib();
    if (!g.def) {
        const std::string id = g.id;
        games_.erase(std::remove_if(games_.begin(), games_.end(), [&](const auto& p) { return p->id == id; }), games_.end());
    } else evaluate(g);
    if (!sel()) {
        lib_.selected = first_added();
        save_lib();
        show_page(P_LIB);
    }
    refresh();
    InvalidateRect(wnd_, nullptr, FALSE);
}

// ---- preparation ---------------------------------------------------------------------------------------------------------------------
void App::start_requirements() {
    for (auto& g : games_)
        if (g->def) {
            g->checks = run_game_checks(tools_, *g->def, g->cfg, remaining_disk_need(g->cfg, g->st));
            for (Check& c : g->checks) c.item = g->short_title + L": " + c.item;
        }
    if (checks_running_) { refresh(); return; }
    checks_running_ = true;
    const Tools t = tools_;
    HWND w = wnd_;
    std::thread([t, w] {
        auto* r = new std::vector<Check>(run_system_checks(t));
        if (!PostMessageW(w, kMsgReqs, 0, reinterpret_cast<LPARAM>(r))) delete r;
    }).detach();
    refresh();
}

void App::start_prepare_check(GameItem& g) {
    if (g.running || g.checking_prep || !g.def || !g.gid.ok || g.need[S_CORPUS] || g.need[S_PROGRAMS] || !g.exe_ok()) return;
    if (g.st.translated_game != g.gid.text()) return;
    if (!write_game_ini(g)) return;
    g.checking_prep = true;
    HWND w = wnd_;
    GameItem* gp_ = &g;
    auto got = std::make_shared<std::string>();
    std::wstring err;
    const bool ok = g.check_child.start(g.exe, game_cmd(g, L"--prepare-shaders --check"), g.lay.data, false,
        [got](const std::string& l) { if (l.rfind("prepare: ", 0) == 0) *got = l.substr(9); },
        [got, w, gp_](DWORD code) {
            std::string a = *got;
            if (a.empty()) a = code == 7 ? "error the game is running from this folder" : "error the check ended with code " + std::to_string(code);
            PostMessageW(w, kMsgCheck, reinterpret_cast<WPARAM>(gp_), reinterpret_cast<LPARAM>(new std::string(a)));
        }, &err);
    if (!ok) { g.checking_prep = false; g.prep_check = L"error " + err; }
    evaluate(g);
}

std::wstring App::py_args(const std::wstring& script, const std::wstring& rest) const {
    return (tools_.python_args.empty() ? L"" : tools_.python_args + L" ") + L"-I " + quote(script) + L" " + rest;
}

std::wstring App::game_cmd(const GameItem& g, const std::wstring& extra) const {
    const std::wstring log_name = g.def ? widen(g.def->log) : L"game.log";
    return L"--game " + quote(g.lay.game) + L" --ini " + quote(g.lay.game_ini) + L" --log " + quote(join(g.lay.data, L"logs\\setup_" + log_name)) + L" " + extra;
}

bool App::write_game_ini(GameItem& g) {
    std::error_code ec;
    fs::create_directories(fs::path(g.lay.data) / L"logs", ec);
    auto fwd = [](std::wstring p) { for (wchar_t& c : p) if (c == L'\\') c = L'/'; return narrow(p); };
    std::string t = "; Written by the game library: where the game finds the shader data it prepared.\n[nvn]\n";
    t += "shader_cache = " + fwd(g.lay.spirv) + "\n";
    t += "shader_corpus = " + fwd(g.lay.corpus) + "\n";
    if (!tools_.translator.empty()) t += "shader_translator = " + fwd(tools_.translator) + "\n";
    const std::string old = read_file_text(g.lay.game_ini);
    const size_t mark = old.find("\n; --- your settings below ---");
    t += mark == std::string::npos ? std::string("\n; --- your settings below ---\n") : old.substr(mark);
    return write_file_atomic(g.lay.game_ini, t);
}

void App::log(GameItem& g, const std::wstring& s) {
    g.log.push_back(s);
    while (g.log.size() > 300) g.log.pop_front();
    if (&g != sel()) return;
    std::wstring all;
    for (const std::wstring& l : g.log) all += l + L"\r\n";
    SetWindowTextW(log_, all.c_str());
    SendMessageW(log_, EM_SETSEL, all.size(), all.size());
    SendMessageW(log_, EM_SCROLLCARET, 0, 0);
}

void App::enqueue(GameItem& g, bool force_prepare) {
    if (!g.def || g.running || g.queued) return;
    if (g.check_child.running()) g.check_child.wait();
    g.error.clear();
    g.cancel_requested = false;
    g.force_prepare = force_prepare;
    evaluate(g);
    const SourceKind sk = source_kind(g.cfg.source);
    if (sk != SourceKind::nup && sk != SourceKind::folder) {
        MessageBoxW(wnd_, L"Add your dump first.", kAppName, MB_OK | MB_ICONINFORMATION);
        return;
    }
    bool bad = false;
    for (const Check& c : sys_checks_) bad |= c.level == Level::bad;
    if (bad && MessageBoxW(wnd_, L"Something is missing on this PC (see This PC). Continue anyway?", kAppName, MB_YESNO | MB_ICONWARNING) != IDYES)
        return;
    g.queued = true;
    queue_.push_back(&g);
    log(g, L"--- " + std::wstring(force_prepare ? L"rebuild" : L"queued") + L" ---");
    run_queue();
    refresh();
    InvalidateRect(wnd_, nullptr, FALSE);
    InvalidateRect(cards_, nullptr, FALSE);
}

void App::run_queue() {
    if (active_ || queue_.empty()) return;
    GameItem& g = *queue_.front();
    queue_.pop_front();
    g.queued = false;
    g.running = true;
    g.stopping = g.killed = false;
    active_ = &g;
    evaluate(g);   // the plan of this job: the steps to run; it is not recomputed while they run (each step looks again just before it starts)
    g.job_t0 = std::chrono::steady_clock::now();
    g.size_scale = size_scale_of(g);
    for (int i = 0; i < S_COUNT; ++i) {
        g.run[i].reset();
        g.run[i].g = &g;
        g.run[i].id = i;
        g.run[i].planned = g.need[i] || (i == S_PREPARE && g.force_prepare);
    }
    schedule(g);
}

// Starts every planned step whose dependencies are finished; ends the job when nothing runs and nothing can start.
void App::schedule(GameItem& g) {
    if (!g.running) return;
    for (bool started = true; started && !g.stopping;) {
        started = false;
        for (int i = 0; i < S_COUNT && !g.stopping; ++i) {
            StepRun& r = g.run[i];
            if (!r.planned || r.started) continue;
            bool ready = true;
            for (int d : kStepDeps[i]) ready &= !g.run[d].planned || g.run[d].finished;
            if (!ready) continue;
            start_step(g, i);
            started = true;
        }
    }
    int pending = 0;
    for (const StepRun& r : g.run) pending += r.planned && !r.started;
    if (g.nrunning()) return;
    if (pending && !g.stopping) fail_job(g, "internal error: a step waits for one that is not coming");
    if (!pending && !g.stopping) log(g, L"--- finished ---");
    finish_job(g);
}

void App::request_stop(GameItem& g) {
    if (!g.stopping) { g.stopping = true; g.stop_at = std::chrono::steady_clock::now(); }
    for (StepRun& r : g.run) if (r.running()) r.child.cancel();
}

void App::fail_job(GameItem& g, const std::string& error) {
    if (g.error.empty()) g.error = error;
    g.play_after = false;
    request_stop(g);   // the others stop too: what they did is kept, Prepare continues later
}

void App::finish_job(GameItem& g) {
    g.running = false;
    g.stopping = g.killed = false;
    if (g.cancel_requested) g.error.clear();
    if (active_ == &g) active_ = nullptr;
    evaluate(g);
    if (g.error.empty() && !g.cancel_requested) { g.prep_check.clear(); start_prepare_check(g); }
    if (g.play_after) {
        g.play_after = false;
        if (g.error.empty() && g.ready()) play(g);
    }
    if (g.play_now) {
        g.play_now = false;
        play(g);
    }
    refresh();
    InvalidateRect(wnd_, nullptr, FALSE);
    InvalidateRect(cards_, nullptr, FALSE);
    run_queue();
    if (closing_ && !active_) DestroyWindow(wnd_);
}

void App::start_step(GameItem& g, int s) {
    StepRun& r = g.run[s];
    r.started = true;
    // another look at the state: an earlier step may have made this one unnecessary (an extraction that kept the built program, ...)
    evaluate(g);
    if (!g.need[s] && !(s == S_PREPARE && g.force_prepare)) { r.finished = r.ok = true; return; }
    auto fail = [&](const std::string& e) { r.finished = true; fail_job(g, e); };
    if ((s != S_EXTRACT && !g.gid.ok) || (s == S_EXTRACT && source_kind(g.cfg.source) != SourceKind::nup)) {
        fail(s == S_EXTRACT ? "the dump folder is not a game folder" : "the game files are not there");
        return;
    }
    if (s == S_BUILD && (!g.build.ok || tools_.python.empty())) { fail("this package cannot build the game program"); return; }
    if ((s == S_TRANSLATE || s == S_PREPARE) && !g.exe_ok()) {
        log(g, L"--- the game program is not built: " + g.exe + L" ---");
        fail("the game program is not built yet");
        return;
    }
    r.t0 = std::chrono::steady_clock::now();
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    const std::wstring jobs = std::to_wstring(hw > 3 ? hw - 2 : 1);
    std::wstring exe, args;
    bool event = false;
    std::error_code ec;
    if (s == S_EXTRACT) {
        exe = tools_.python;
        args = py_args(tools_.nup_extract, quote(g.cfg.source) + L" " + quote(g.lay.dump));
    } else if (s == S_BUILD) {
        fs::create_directories(parent_of(g.exe), ec);
        exe = tools_.python;
        args = py_args(tools_.game_build, L"--exefs " + quote(join(g.lay.game, L"exefs")) + L" --game " + quote(parent_of(g.exe)) +
                       L" --manifest " + quote(g.def->manifest) + L" --sdk " + quote(g.build.sdk) + L" --runtime " + quote(g.build.runtime) +
                       L" --jobs " + std::to_wstring(std::max(1u, hw > 1 ? hw - 1 : 1)));
    } else if (s == S_CORPUS) {
        fs::remove(fs::path(g.lay.corpus) / L"unique.tsv", ec);   // a scan that stops before its end must not count
        fs::remove(fs::path(g.lay.corpus) / L"unique_all.tsv", ec);
        fs::create_directories(g.lay.corpus, ec);
        exe = tools_.python;
        args = py_args(join(tools_.shader_scripts, L"extract_generic.py"), L"--romfs " + quote(join(g.lay.game, L"romfs")) + L" --exefs " +
                       quote(join(g.lay.game, L"exefs")) + L" --out " + quote(g.lay.corpus) + L" --jobs " + jobs + L" --progress");
    } else if (s == S_TRANSLATE) {
        if (g.retranslate_all && dir_exists(g.lay.spirv)) {
            const std::wstring old = g.lay.spirv + L"_old_" + std::to_wstring(GetTickCount64());
            if (MoveFileW(g.lay.spirv.c_str(), old.c_str())) {
                log(g, L"old translations moved aside, deleting them: " + old);
                std::thread([old] { std::error_code e; fs::remove_all(old, e); }).detach();
            }
            g.st.translated_with.clear();
            save_state(g.lay.state_file, g.st);
        }
        write_game_ini(g);
        exe = g.exe;
        args = game_cmd(g, L"--translate-shaders");
        event = true;
    } else if (s == S_PROGRAMS) {
        exe = tools_.python;
        args = py_args(join(tools_.shader_scripts, L"bnsh_programs.py"), L"--romfs " + quote(join(g.lay.game, L"romfs")) + L" --out " + quote(g.lay.programs));
    } else {
        write_game_ini(g);
        exe = g.exe;
        args = game_cmd(g, g.force_prepare ? L"--prepare-shaders --force" : L"--prepare-shaders --if-needed");
        event = true;
    }
    if (exe.empty()) { fail("Python is missing (This PC page)"); return; }
    log(g, L"> " + std::wstring(kStepName[s]));
    fs::create_directories(g.lay.data, ec);
    HWND w = wnd_;
    StepRun* rp = &r;
    std::wstring err;
    if (!r.child.start(exe, args, g.lay.data, event,
                       [w, rp](const std::string& l) { PostMessageW(w, kMsgLine, reinterpret_cast<WPARAM>(rp), reinterpret_cast<LPARAM>(new std::string(l))); },
                       [w, rp](DWORD c) { PostMessageW(w, kMsgExit, reinterpret_cast<WPARAM>(rp), static_cast<LPARAM>(c)); }, &err)) {
        log(g, L"  " + err);
        fail(narrow(err));
        return;
    }
    refresh();
}

void App::step_line(StepRun& r, const std::string& l) {
    GameItem& g = *r.g;
    unsigned long long a = 0, b = 0;
    char label[64] = {};
    bool progress = false;
    if (std::sscanf(l.c_str(), "@progress %63s %llu %llu", label, &a, &b) == 3) progress = true;
    else if (std::sscanf(l.c_str(), "scan: %llu %llu", &a, &b) == 2) progress = true;
    else if (std::sscanf(l.c_str(), "translate: %llu %llu", &a, &b) == 2 && l.find("done") == std::string::npos) progress = true;
    else if (std::sscanf(l.c_str(), "prepare: %llu %llu", &a, &b) == 2) progress = true;
    if (progress) {
        // a new tree of the extraction starts its own count; the build's phases share one step-wide fraction and clock
        if (label[0] && b != r.total && r.id != S_BUILD) { r.t0 = std::chrono::steady_clock::now(); r.seen = false; }
        if (!r.seen) { r.seen = true; r.done0 = a; r.t1 = std::chrono::steady_clock::now(); }
        r.phase = label;
        r.done = a;
        r.total = b;
        return;
    }
    if (l.rfind("@error ", 0) == 0 || l.rfind("translate: error ", 0) == 0 || l.rfind("prepare: error ", 0) == 0) r.error = l.substr(l.find(' ') + 1);
    if (l.rfind("@result", 0) == 0 || l.rfind("translate: done", 0) == 0 || l.rfind("prepare: done", 0) == 0 || l == "prepare: ready")
        r.result_ok = l.find("cancelled") == std::string::npos;
    log(g, (g.nrunning() > 1 ? L"[" + std::wstring(kStepShort[r.id]) + L"] " : L"") + widen(l));
}

void App::step_exit(StepRun& r, DWORD code) {
    GameItem& g = *r.g;
    const int s = r.id;
    r.child.wait();
    const bool cancelled = g.cancel_requested || g.stopping;
    bool ok = code == 0 && (!cancelled || r.result_ok || s == S_PROGRAMS || s == S_CORPUS);
    if (ok) {
        if (s == S_EXTRACT) g.st.extracted_from = nup_stamp(g.cfg.source);
        else if (s == S_BUILD) { ok = file_exists(g.exe); if (ok) g.st.built_for = game_id_of(g.lay.game).text() + "|" + g.build.stamp; }
        else if (s == S_CORPUS) { ok = file_exists(join(g.lay.corpus, L"unique.tsv")); if (ok) g.st.corpus_game = game_id_of(g.lay.game).text(); }
        else if (s == S_TRANSLATE) { g.st.translated_game = game_id_of(g.lay.game).text(); g.st.translated_with = translator_stamp(tools_.translator); }
        else if (s == S_PROGRAMS) { ok = file_exists(g.lay.programs); if (ok) g.st.programs_game = game_id_of(g.lay.game).text(); }
        else if (s == S_PREPARE) { g.prep_check = L"ready"; g.force_prepare = false; }
        r.t_end = std::chrono::steady_clock::now();
        if (ok && !cancelled) g.st.secs[s] = std::max(1, static_cast<int>(r.elapsed()));   // the next estimate of this step on this PC
        save_state(g.lay.state_file, g.st);
    }
    r.finished = true;
    r.ok = ok;
    log(g, (g.nrunning() > 0 ? L"[" + std::wstring(kStepShort[s]) + L"] " : L"") + L"  exit code " + std::to_wstring(code));
    if (!ok && !cancelled)
        fail_job(g, r.error.empty() ? narrow(std::wstring(kStepName[s])) + " ended with code " + std::to_string(code) + " (see Output)" : r.error);
    refresh();
    schedule(g);
}

void App::cancel(GameItem& g) {
    if (g.queued) {
        queue_.erase(std::remove(queue_.begin(), queue_.end(), &g), queue_.end());
        g.queued = false;
        g.play_after = false;
        log(g, L"--- removed from the queue ---");
    } else if (g.running) {
        g.play_after = g.play_now = false;
        g.cancel_requested = true;
        request_stop(g);
        schedule(g);   // ends the job at once when nothing runs
    }
    refresh();
    InvalidateRect(cards_, nullptr, FALSE);
}

void App::rebuild_dialog(GameItem& g) {
    if (g.running || g.queued || !g.gid.ok) return;
    TASKDIALOG_BUTTON b[] = {
        {101, L"Compile shaders again"},
        {102, L"Translate and compile again"},
        {103, L"Set up everything again"},
    };
    TASKDIALOGCONFIG c{sizeof c};
    c.hwndParent = wnd_;
    c.hInstance = inst_;
    c.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
    c.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    c.pszWindowTitle = kAppName;
    const std::wstring head = L"Rebuild the shaders of " + g.short_title;
    c.pszMainInstruction = head.c_str();
    c.pszContent = L"Only needed when graphics look broken. Updates and new drivers are handled by themselves.";
    c.cButtons = 3;
    c.pButtons = b;
    int pick = 0;
    if (FAILED(TaskDialogIndirect(&c, &pick, nullptr, nullptr))) return;
    if (pick == 102) { g.st.translated_game.clear(); g.st.translated_with = "rebuild"; save_state(g.lay.state_file, g.st); }
    else if (pick == 103) { g.st.corpus_game.clear(); g.st.programs_game.clear(); g.st.translated_game.clear(); save_state(g.lay.state_file, g.st); }
    if (pick >= 101 && pick <= 103) enqueue(g, true);
}

void App::play(GameItem& g) {
    if (!g.playable()) return;
    write_game_ini(g);
    const std::wstring log_path = join(g.lay.data, L"logs\\" + widen(g.def->log));
    std::wstring cmd = quote(g.exe) + L" --game " + quote(g.lay.game) + L" --ini " + quote(g.lay.game_ini) + L" --log " + quote(log_path);
    if (!game_args_.empty()) cmd += L" " + game_args_;
    // the game program is built into <data>\program: point it at the library's downloaded DLSS files (inherited environment)
    if (dlss_state(tools_) == 2) SetEnvironmentVariableW(L"NXRT_UPSCALE_DLSS_DLL", dlss_dir(tools_).c_str());
    // setup not done: no "Preparing shaders" window at start; missing shaders are translated and compiled while the game runs
    SetEnvironmentVariableW(L"NXRT_NVN_SETUP_ON_START", g.ready() ? nullptr : L"0");
    // the game's Settings entries open this library on its settings page (one place for the settings: runtime/docs/MENU.md)
    wchar_t self[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, self, static_cast<DWORD>(std::size(self)));
    SetEnvironmentVariableW(L"NXRT_LIBRARY_EXE", self);
    SetEnvironmentVariableW(L"NXRT_LIBRARY_GAME", widen(g.id).c_str());
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(g.exe.c_str(), buf.data(), nullptr, nullptr, FALSE, 0, nullptr, parent_of(g.exe).c_str(), &si, &pi)) {
        MessageBoxW(wnd_, (L"Could not start " + g.exe + L" (Windows error " + std::to_wstring(GetLastError()) + L").").c_str(), kAppName, MB_OK | MB_ICONERROR);
        return;
    }
    CloseHandle(pi.hThread);
    // A game that ends within a few seconds did not start: say why (exit code 7: another run of it, or too little free memory beside one).
    SetCursor(LoadCursor(nullptr, IDC_WAIT));
    const bool ended = WaitForSingleObject(pi.hProcess, 4000) == WAIT_OBJECT_0;
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    if (ended) {
        CloseHandle(pi.hProcess);
        if (code != 0) {
            const std::wstring why = code == 7 ? L"The game is already running, or another run of it leaves too little free memory. Close it first."
                                               : L"The game stopped while starting (exit code " + std::to_wstring(code) + L").";
            MessageBoxW(wnd_, (why + L"\r\n\r\nIts log: " + log_path).c_str(), kAppName, MB_OK | MB_ICONWARNING);
        }
        return;
    }
    // it runs: the library shows "Playing" until it ends (a thread waits for the process)
    g.game_running = true;
    HWND w = wnd_;
    GameItem* gp_ = &g;
    std::thread([w, gp_, h = pi.hProcess] {
        WaitForSingleObject(h, INFINITE);
        DWORD c = 0;
        GetExitCodeProcess(h, &c);
        CloseHandle(h);
        PostMessageW(w, kMsgGameExit, reinterpret_cast<WPARAM>(gp_), static_cast<LPARAM>(c));
    }).detach();
    refresh();
    InvalidateRect(wnd_, nullptr, FALSE);
    InvalidateRect(cards_, nullptr, FALSE);
    if (!g.ready()) return;
    ShowWindow(wnd_, SW_MINIMIZE);
}

void App::play_now(GameItem& g) {
    if (!g.can_play_now()) return;
    if (!g.running) { play(g); return; }
    g.play_after = false;
    g.play_now = true;        // finish_job starts the game once every step has ended (Compile saves its progress when cancelled)
    g.cancel_requested = true;
    log(g, L"--- stopping the setup: the game starts when the steps have ended ---");
    request_stop(g);
    schedule(g);
    refresh();
    InvalidateRect(cards_, nullptr, FALSE);
}

void App::handle_args(const std::wstring& line, bool live) {
    (void)live;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(line.c_str(), &argc);
    int page = -1, tab = -1;
    for (int i = 1; argv && i < argc; ++i) {
        const std::wstring a = argv[i];
        const bool more = i + 1 < argc;
        if (a == L"--game" && more) { const std::string id = narrow(argv[++i]); if (GameItem* g = item(id); g && g->in_library()) select(id); }
        else if (a == L"--page" && more) page = _wtoi(argv[++i]);
        else if (a == L"--tab" && more) tab = _wtoi(argv[++i]);
        else if (a == L"--add" && more) add_dump(argv[++i], true);
    }
    if (argv) LocalFree(argv);
    if (page >= 0) show_page(page);
    if (tab >= 0) settings::set_tab(settings_, tab);
}

void App::make_shortcut(GameItem& g) {
    if (!g.playable()) return;
    write_game_ini(g);
    IShellLinkW* sl = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl)))) return;
    sl->SetPath(g.exe.c_str());
    sl->SetArguments((L"--game " + quote(g.lay.game) + L" --ini " + quote(g.lay.game_ini)).c_str());
    sl->SetWorkingDirectory(parent_of(g.exe).c_str());
    const std::wstring product = widen(g.def->product);
    sl->SetDescription(product.c_str());
    // The shortcut is named like the game window; characters a file name cannot hold become spaces (no ':' in a file name).
    std::wstring name = widen(g.def->window_title);
    for (wchar_t& c : name)
        if (wcschr(L"<>:\"/\\|?*", c) || c < 32) c = L' ';
    PWSTR desk = nullptr;
    bool ok = false;
    std::wstring path;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desk))) {
        path = join(desk, name + L".lnk");
        CoTaskMemFree(desk);
        IPersistFile* pf = nullptr;
        if (SUCCEEDED(sl->QueryInterface(IID_PPV_ARGS(&pf)))) { ok = SUCCEEDED(pf->Save(path.c_str(), TRUE)); pf->Release(); }
    }
    sl->Release();
    MessageBoxW(wnd_, ok ? (L"Shortcut created: " + path).c_str() : L"The shortcut could not be created.", kAppName, MB_OK | (ok ? MB_ICONINFORMATION : MB_ICONERROR));
}

void App::about() {
    std::wstring t = std::wstring(kAppName) + L" " + widen(RECOMP_LIBRARY_VERSION) + L"\r\n\r\nGames known: " + std::to_wstring(defs_.size()) + L"\r\nTranslator: " +
                     (tools_.translator.empty() ? L"(missing)" : tools_.translator) + L"\r\nPython: " + (tools_.python.empty() ? L"(missing)" : tools_.python) +
                     L"\r\nLibrary: " + library_path();
    for (const GameDef& d : defs_) t += L"\r\n  " + widen(d.title) + L" (" + widen(d.title_id) + L"): " + d.manifest;
    MessageBoxW(wnd_, t.c_str(), L"About", MB_OK | MB_ICONINFORMATION);
}

// Opens a GitHub issue form (.github/ISSUE_TEMPLATE) in the browser with the game, the library version and this PC filled in.
// The library sends nothing itself: the player reads the form and submits it (or not) on GitHub. No paths, logs or game data go into it.
void App::report(GameItem* g) {
    TASKDIALOG_BUTTON b[] = {{201, L"Game bug\nSomething is wrong while the game runs: picture, sound, controls, a crash, speed."},
                             {202, L"Recompiler / library bug\nThis app, adding a dump, building the game program, preparing shaders."},
                             {203, L"Game status\nTell how well a game runs, for the list of games."}};
    TASKDIALOGCONFIG c{sizeof c};
    c.hwndParent = wnd_;
    c.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
    c.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    c.pszWindowTitle = kAppName;
    c.pszMainInstruction = L"Report a bug";
    c.pszContent = L"Opens a form on GitHub (needs a GitHub account) with the game and this PC filled in. "
                   L"Never attach game files, saves, keys or screenshots of the game.";
    c.cButtons = 3;
    c.pButtons = b;
    int pick = 0;
    if (FAILED(TaskDialogIndirect(&c, &pick, nullptr, nullptr)) || pick < 201 || pick > 203) return;
    auto enc = [](const std::wstring& w) {
        static const char* hex = "0123456789ABCDEF";
        std::string out;
        for (unsigned char ch : narrow(w)) {
            if ((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '-' || ch == '_' || ch == '.' || ch == '~')
                out += static_cast<char>(ch);
            else { out += '%'; out += hex[ch >> 4]; out += hex[ch & 15]; }
        }
        return widen(out);
    };
    std::wstring system;
    for (const wchar_t* name : {L"Graphics card (Vulkan 1.3)", L"Windows", L"Memory"})
        for (const Check& k : sys_checks_)
            if (k.item == name && !k.detail.empty()) system += (system.empty() ? L"" : L"; ") + k.detail;
    std::wstring game;
    if (g && g->def) game = widen(g->def->id) + L", " + widen(g->title_id) + L", " + widen(g->def->display_version);
    else if (g) game = L"no port yet, " + widen(g->title_id);
    const wchar_t* tpl = pick == 201 ? L"bug_game.yml" : pick == 202 ? L"bug_recompiler.yml" : L"game_status.yml";
    std::wstring url = widen(RECOMP_LIBRARY_REPO) + L"/issues/new?template=" + tpl + L"&app=" + enc(widen(RECOMP_LIBRARY_VERSION));
    if (!system.empty()) url += L"&system=" + enc(system);
    if (!game.empty()) url += L"&game=" + enc(game);
    ShellExecuteW(wnd_, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// ---- controls -----------------------------------------------------------------------------------------------------------------------
void App::make_fonts() {
    for (HFONT* f : {&font_, &bold_, &small_, &title_, &big_})
        if (*f) { DeleteObject(*f); *f = nullptr; }
    LOGFONTW lf{};
    lf.lfQuality = CLEARTYPE_QUALITY;
    lf.lfCharSet = DEFAULT_CHARSET;
    wcscpy_s(lf.lfFaceName, L"Segoe UI Variable Text");
    lf.lfHeight = -S(14);
    font_ = CreateFontIndirectW(&lf);
    lf.lfHeight = -S(12);
    small_ = CreateFontIndirectW(&lf);
    lf.lfHeight = -S(14);
    lf.lfWeight = FW_SEMIBOLD;
    bold_ = CreateFontIndirectW(&lf);
    wcscpy_s(lf.lfFaceName, L"Segoe UI Variable Display");
    lf.lfHeight = -S(24);
    title_ = CreateFontIndirectW(&lf);
    lf.lfHeight = -S(34);
    lf.lfWeight = FW_BOLD;
    big_ = CreateFontIndirectW(&lf);
}

HWND App::button(const wchar_t* t, int id, Btn kind) {
    HWND h = CreateWindowExW(0, WC_BUTTONW, t, WS_CHILD | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 10, 10, wnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), inst_, nullptr);
    btns_.push_back({h, kind});
    return h;
}

void App::apply_theme(HWND h) {
    SetWindowTheme(h, L"DarkMode_Explorer", nullptr);
    SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
}

void App::create_controls() {
    cards_ = CreateWindowExW(0, kCardsClass, L"", WS_CHILD | WS_VSCROLL | WS_TABSTOP, 0, 0, 10, 10, wnd_, reinterpret_cast<HMENU>(ID_CARDS), inst_, nullptr);
    SetWindowTheme(cards_, L"DarkMode_Explorer", nullptr);
    ctls_.push_back({cards_, P_LIB});
    // game page: the one button, a cancel while it sets up, and the rest in a menu
    b_play_ = button(L"Play", ID_PLAY, Btn::primary);
    b_cancel_ = button(L"Cancel", ID_CANCEL, Btn::danger);
    b_more_ = button(L"\u2022\u2022\u2022", ID_MORE, Btn::normal);
    for (HWND h : {b_play_, b_cancel_, b_more_}) ctls_.push_back({h, P_GAME});
    // the steps are painted (paint_steps); the output below them shows with details_
    log_ = CreateWindowExW(0, WC_EDITW, L"", WS_CHILD | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, 0, 0, 10, 10, wnd_,
                           reinterpret_cast<HMENU>(ID_LOG), inst_, nullptr);
    ctls_.push_back({log_, -1});
    // settings page
    settings_ = settings::create(wnd_, inst_, ID_SETTINGS);
    ctls_.push_back({settings_, P_SET});
    b_open_ini_ = button(L"Open file", ID_OPEN_INI, Btn::normal);
    ctls_.push_back({b_open_ini_, P_SET});
    // this PC: the checks are painted (paint_pc); the two buttons below them
    b_recheck_ = button(L"Check again", ID_RECHECK, Btn::normal);
    b_copy_ = button(L"Copy report", ID_COPY, Btn::normal);
    for (HWND h : {b_recheck_, b_copy_}) ctls_.push_back({h, P_PC});
    apply_theme(log_);
    for (auto& [h, k] : btns_) SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(bold_), TRUE);
}

constexpr int kSideW = 236;

void App::layout() {
    RECT rc;
    GetClientRect(wnd_, &rc);
    const int x = S(kSideW) + S(32), w = rc.right - x - S(32), bottom = rc.bottom;
    auto put = [](HWND h, int x0, int y0, int w0, int h0) { MoveWindow(h, x0, y0, std::max(w0, 1), std::max(h0, 1), TRUE); };
    // side nav
    nav_layout();
    // library
    put(cards_, S(kSideW), S(92), rc.right - S(kSideW), bottom - S(92));
    // game page: banner 0..S(200), the button row, a status line with the bar, then (details) the steps and the output
    int y = S(214);
    put(b_play_, x, y, S(200), S(48));
    put(b_cancel_, x + S(212), y + S(6), S(110), S(36));
    put(b_more_, x + w - S(48), y + S(6), S(48), S(36));
    y += S(64);
    prog_rect_ = {x, y, x + w, y + S(48)};
    y += S(60);
    // the steps card: a header row, then one row per step
    steps_rect_ = {x, y, x + w, y + S(52) + S_COUNT * S(46) + S(8)};
    y = steps_rect_.bottom + S(16);
    put(log_, x, y, w, std::max(S(80), bottom - y - S(24)));
    // settings: the panel below the page title, the file button in the title row
    put(settings_, S(kSideW), S(92), rc.right - S(kSideW), bottom - S(92));
    put(b_open_ini_, rc.right - S(32) - S(130), S(58), S(130), S(30));
    // this PC: the checks (painted) between the title and the buttons
    const int bh = S(36);
    pc_rect_area_ = {x, S(104), x + w, bottom - S(72)};
    put(b_recheck_, x, bottom - S(56), S(150), bh);
    put(b_copy_, x + S(162), bottom - S(56), S(150), bh);
    InvalidateRect(wnd_, nullptr, TRUE);
}

void App::show_page(int p) {
    const int before = page_;
    page_ = std::clamp(p, 0, P_COUNT - 1);
    if ((page_ == P_GAME || page_ == P_SET) && !sel()) page_ = P_LIB;
    if (page_ == P_SET && before != P_SET) update_settings();
    for (auto& [h, pg] : ctls_) ShowWindow(h, pg == page_ ? SW_SHOW : SW_HIDE);
    refresh();
    // the keyboard starts where the page's main thing is
    HWND f = page_ == P_LIB ? cards_ : page_ == P_GAME ? (IsWindowEnabled(b_play_) ? b_play_ : b_more_) : page_ == P_SET ? settings_ : b_recheck_;
    if (f && IsWindowVisible(wnd_)) SetFocus(f);
    InvalidateRect(wnd_, nullptr, TRUE);
}

void App::refresh() {
    GameItem* g = sel();
    const bool game_page = page_ == P_GAME;
    if (g) {
        bool en = false;
        SetWindowTextW(b_play_, primary_label(*g, &en).c_str());
        EnableWindow(b_play_, en);
        ShowWindow(b_cancel_, game_page && (g->running || g->queued) ? SW_SHOW : SW_HIDE);
        EnableWindow(b_cancel_, g->running || g->queued);
        EnableWindow(b_open_ini_, g->in_library() && !g->lay.game.empty());
    }
    ShowWindow(log_, game_page && g && details_ ? SW_SHOW : SW_HIDE);
    if (page_ == P_PC) InvalidateRect(wnd_, &pc_rect_area_, FALSE);
    for (auto& [h, k] : btns_) InvalidateRect(h, nullptr, FALSE);
}

void App::more_menu() {
    GameItem* g = sel();
    if (!g) return;
    const bool idle = !g->running && !g->queued;
    HMENU m = CreatePopupMenu();
    auto item = [&](int id, const wchar_t* t, bool en) { AppendMenuW(m, MF_STRING | (en ? 0 : MF_GRAYED), id, t); };
    item(ID_DETAILS, details_ ? L"Hide details" : L"Show details", true);
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    if (g->can_play_now() && !g->ready()) {   // setup not done (or still running): start the program built last time as it is
        item(ID_PLAY_AS_IS, g->running ? L"Play now (stop the setup; shaders compile while playing)" : L"Play now (skip setup)",
             !g->cancel_requested && !g->play_now);
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    }
    item(ID_SRC_NUP, L"Choose another .nup file...", idle && g->def);
    item(ID_SRC_FOLDER, L"Choose another folder...", idle && g->def);
    item(ID_DATA, L"Move the data folder...", idle && g->in_library());
    item(ID_OPEN_DATA, L"Open the data folder", g->in_library());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    item(ID_REBUILD, L"Rebuild shaders...", idle && g->def && g->gid.ok);
    item(ID_SHORTCUT, L"Desktop shortcut", g->playable());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    item(ID_REMOVE, L"Remove from library", idle && g->in_library());
    RECT r;
    GetWindowRect(b_more_, &r);
    const int id = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTALIGN | TPM_TOPALIGN, r.right, r.bottom + S(4), 0, wnd_, nullptr);
    DestroyMenu(m);
    if (id) command(id, 0);
}

// ---- painting ------------------------------------------------------------------------------------------------------------------------
void App::paint_side(gp::Graphics& g, const RECT& rc) {
    gp::SolidBrush side(C(kSide));
    g.FillRectangle(&side, 0.f, 0.f, Sf(kSideW), static_cast<gp::REAL>(rc.bottom));
    HDC dc = g.GetHDC();
    gp::Font big(dc, title_), bold(dc, bold_), smallf(dc, small_), normal(dc, font_);
    g.ReleaseHDC(dc);
    // brand
    gp::LinearGradientBrush lg(gp::PointF(Sf(20), Sf(28)), gp::PointF(Sf(52), Sf(60)), C(kAccent), C(RGB(0xB0, 0x6B, 0xFF)));
    fill_round(g, gp::RectF(Sf(20), Sf(26), Sf(34), Sf(34)), Sf(9), lg);
    text(g, L"R", bold, gp::RectF(Sf(20), Sf(26), Sf(34), Sf(34)), RGB(255, 255, 255), gp::StringAlignmentCenter, gp::StringAlignmentCenter);
    text(g, L"Recomp", bold, gp::RectF(Sf(64), Sf(24), Sf(160), Sf(20)), kText);
    text(g, L"Library", smallf, gp::RectF(Sf(64), Sf(43), Sf(160), Sf(18)), kMuted);
    // nav: the games of the library (a game opens its page, which has the settings), then This PC
    nav_layout();
    const GameItem* s = sel();
    auto row = [&](const RECT& r, const std::wstring& name, bool on, bool hot, COLORREF dot, const std::wstring& sub) {
        gp::RectF rf(static_cast<gp::REAL>(r.left), static_cast<gp::REAL>(r.top), static_cast<gp::REAL>(r.right - r.left), static_cast<gp::REAL>(r.bottom - r.top));
        if (on || hot) {
            gp::SolidBrush b(C(on ? kCardHi : kCard));
            fill_round(g, rf, Sf(8), b);
        }
        if (on) {
            gp::SolidBrush a(C(kAccent));
            fill_round(g, gp::RectF(rf.X, rf.Y + Sf(10), Sf(4), rf.Height - Sf(20)), Sf(2), a);
        }
        text(g, name, on ? bold : normal, gp::RectF(rf.X + Sf(16), rf.Y, rf.Width - Sf(56), rf.Height), kText, gp::StringAlignmentNear, gp::StringAlignmentCenter);
        if (dot) {
            gp::SolidBrush db(C(dot));
            g.FillEllipse(&db, rf.X + rf.Width - Sf(24), rf.Y + rf.Height / 2 - Sf(4), Sf(8), Sf(8));
        } else if (!sub.empty()) {
            text(g, sub, smallf, gp::RectF(rf.X + rf.Width - Sf(44), rf.Y, Sf(32), rf.Height), kMuted, gp::StringAlignmentFar, gp::StringAlignmentCenter);
        }
    };
    if (nav_games_.empty()) text(g, L"No games yet. Add one below.", smallf, gp::RectF(Sf(20), Sf(100), Sf(kSideW - 40), Sf(40)), kMuted, gp::StringAlignmentNear, gp::StringAlignmentNear, true);
    for (size_t i = 0; i < nav_games_.size(); ++i) {
        const GameItem* gi = nav_games_[i].second;
        row(nav_games_[i].first, gi->short_title, gi == s && (page_ == P_GAME || page_ == P_SET), hover_nav_ == static_cast<int>(i), status_of(*gi).color, L"");
    }
    int bad = 0;
    for (const Check& c : sys_checks_) bad += c.level == Level::bad;
    row(pc_rect_, L"This PC", page_ == P_PC, hover_nav_ == kHovPc, bad ? kBad : sys_checks_.empty() ? 0 : kOk, L"");
    // the queue, at the bottom: what runs, what waits
    float qy = static_cast<float>(add_rect_.top) - Sf(70);
    if (active_ || !queue_.empty()) {
        gp::SolidBrush card(C(kCard));
        fill_round(g, gp::RectF(Sf(12), qy, Sf(kSideW - 24), Sf(60)), Sf(8), card);
        std::wstring t = active_ ? active_->short_title : L"";
        const Status st = active_ ? status_of(*active_) : Status{};
        text(g, L"SETTING UP", smallf, gp::RectF(Sf(24), qy + Sf(6), Sf(180), Sf(14)), kMuted);
        text(g, t + (queue_.empty() ? L"" : L"  +" + std::to_wstring(queue_.size()) + L" waiting"), bold, gp::RectF(Sf(24), qy + Sf(20), Sf(kSideW - 48), Sf(18)), kText);
        const float pw = Sf(kSideW - 48);
        gp::SolidBrush track(C(kLine)), fillb(C(kAccent));
        fill_round(g, gp::RectF(Sf(24), qy + Sf(44), pw, Sf(5)), Sf(2.5f), track);
        if (st.progress > 0) fill_round(g, gp::RectF(Sf(24), qy + Sf(44), std::max(Sf(5), pw * static_cast<float>(st.progress)), Sf(5)), Sf(2.5f), fillb);
    }
    // add a game
    gp::RectF ar(static_cast<gp::REAL>(add_rect_.left), static_cast<gp::REAL>(add_rect_.top), static_cast<gp::REAL>(add_rect_.right - add_rect_.left),
                 static_cast<gp::REAL>(add_rect_.bottom - add_rect_.top));
    gp::SolidBrush ab(C(hover_nav_ == kHovAdd ? kAccentHi : kAccent));
    fill_round(g, ar, Sf(10), ab);
    text(g, L"+  Add a game", bold, ar, RGB(255, 255, 255), gp::StringAlignmentCenter, gp::StringAlignmentCenter);
}

// the side menu's rectangles: the games the player added, This PC after them, the brand (the library) on top
void App::nav_layout() {
    RECT rc;
    GetClientRect(wnd_, &rc);
    brand_rect_ = {S(12), S(16), S(kSideW) - S(12), S(70)};
    nav_games_.clear();
    int ny = S(88);
    for (auto& gi : games_)
        if (gi->in_library()) { nav_games_.push_back({RECT{S(12), ny, S(kSideW) - S(12), ny + S(42)}, gi.get()}); ny += S(46); }
    if (nav_games_.empty()) ny += S(44);   // room for the "no games yet" note
    pc_rect_ = {S(12), ny + S(8), S(kSideW) - S(12), ny + S(50)};
    add_rect_ = {S(12), rc.bottom - S(60), S(kSideW) - S(12), rc.bottom - S(16)};
    report_rect_ = {rc.right - S(24) - S(132), S(14), rc.right - S(24), S(46)};
}

// "Overview | Settings" of the selected game (the same place on both pages)
void App::paint_tabs(gp::Graphics& g, const gp::Font& bold) {
    const wchar_t* names[2] = {L"Overview", L"Settings"};
    float x = Sf(kSideW) + Sf(32);
    for (int i = 0; i < 2; ++i) {
        const bool on = (i == 0) == (page_ == P_GAME);
        const float w = Sf(i ? 90 : 96);
        text(g, names[i], bold, gp::RectF(x, Sf(26), w, Sf(26)), on || hover_nav_ == kHovTab + i ? RGB(255, 255, 255) : RGB(0xD0, 0xD4, 0xDC), gp::StringAlignmentCenter, gp::StringAlignmentCenter);
        if (on) { gp::SolidBrush u(C(kAccentHi)); g.FillRectangle(&u, x + Sf(8), Sf(54), w - Sf(16), Sf(3)); }
        tab_rect_[i] = {static_cast<LONG>(x), S(20), static_cast<LONG>(x + w), S(62)};
        x += w + Sf(8);
    }
}

// Report a bug: top right, on every page
void App::paint_report(gp::Graphics& g) {
    HDC dc = g.GetHDC();
    gp::Font bold(dc, bold_);
    g.ReleaseHDC(dc);
    const RECT& r = report_rect_;
    const gp::RectF rf(static_cast<float>(r.left), static_cast<float>(r.top), static_cast<float>(r.right - r.left), static_cast<float>(r.bottom - r.top));
    gp::SolidBrush b(C(hover_nav_ == kHovReport ? RGB(0x3A, 0x40, 0x4D) : RGB(0x0C, 0x0E, 0x12), hover_nav_ == kHovReport ? 235 : 170));
    fill_round(g, rf, Sf(8), b);
    text(g, L"Report a bug", bold, rf, RGB(255, 255, 255), gp::StringAlignmentCenter, gp::StringAlignmentCenter);
}

void App::paint_game_header(gp::Graphics& g, const RECT& rc) {
    GameItem* s = sel();
    details_rect_ = {};
    if (!s) return;
    HDC dc = g.GetHDC();
    gp::Font big(dc, big_), bold(dc, bold_), normal(dc, font_), smallf(dc, small_);
    g.ReleaseHDC(dc);
    const float x0 = Sf(kSideW), w = static_cast<float>(rc.right) - x0;
    COLORREF a, b;
    banner_colors(s->id, &a, &b);
    gp::LinearGradientBrush lg(gp::PointF(x0, 0), gp::PointF(x0 + w, Sf(200)), C(a), C(b));
    g.FillRectangle(&lg, x0, 0.f, w, Sf(196));
    // fade into the background
    gp::LinearGradientBrush fade(gp::PointF(0, Sf(110)), gp::PointF(0, Sf(197)), C(kBg, 0), C(kBg, 255));
    g.FillRectangle(&fade, x0, Sf(110), w, Sf(87));
    const float x = x0 + Sf(32);
    text(g, s->title, big, gp::RectF(x, Sf(80), w - Sf(64), Sf(48)), RGB(255, 255, 255));
    std::wstring sub = s->def ? L"Version " + widen(s->def->display_version) + (s->def->generic ? L"  ·  experimental build" : L"")
                              : std::wstring(L"No port yet");
    text(g, sub, normal, gp::RectF(x, Sf(130), w - Sf(64), Sf(20)), RGB(0xD8, 0xDC, 0xE4));
    const Status st = status_of(*s);
    // status chip
    HDC dc2 = g.GetHDC();
    SIZE ts{};
    HGDIOBJ of = SelectObject(dc2, bold_);
    GetTextExtentPoint32W(dc2, st.text.c_str(), static_cast<int>(st.text.size()), &ts);
    SelectObject(dc2, of);
    g.ReleaseHDC(dc2);
    const float cw = static_cast<float>(ts.cx) + Sf(34);
    gp::SolidBrush chip(C(st.color, 40));
    fill_round(g, gp::RectF(x, Sf(160), cw, Sf(26)), Sf(13), chip);
    gp::SolidBrush dotb(C(st.color));
    g.FillEllipse(&dotb, x + Sf(11), Sf(169), Sf(8), Sf(8));
    text(g, st.text, bold, gp::RectF(x + Sf(24), Sf(160), cw, Sf(26)), st.color, gp::StringAlignmentNear, gp::StringAlignmentCenter);
    // the line under the button: the whole setup's progress and time, what went wrong, or what to do
    const float px = static_cast<float>(prog_rect_.left), py = static_cast<float>(prog_rect_.top), pw = static_cast<float>(prog_rect_.right - prog_rect_.left);
    std::wstring pt, right;
    COLORREF pc = kMuted;
    bool bar = false;
    if (s->running) {
        bar = true;
        if (s->cancel_requested) pt = s->play_now ? L"Stopping the setup, then the game starts" : L"Stopping…";
        else {
            pt = L"All steps · " + std::to_wstring(static_cast<int>(st.progress * 100)) + L"%";
            if (s->play_after) pt += L" · the game starts when it is done";
            right = span_text(s->eta()) + L" left in total";
        }
        pc = kText;
    } else if (s->queued) {
        bar = true;
        pt = L"Waiting for the game before it";
    } else if (!s->error.empty()) {
        pt = L"⚠  " + widen(s->error);
        pc = kBad;
        right = details_ ? L"" : L"See the output below";
    } else if (s->game_running) {
        pt = L"The game is running. Settings changes reach it at once.";
    } else if (s->def && s->in_library() && !s->exe_ok() && !s->build.ok) {
        pt = L"This release has no game program for it yet.";
    } else if (s->def && s->in_library() && !s->ready()) {
        pt = L"The first Play sets it up: " + span_text(s->eta()) + L".";
    } else {
        bool bad = false;
        for (const Check& c : sys_checks_) bad |= c.level == Level::bad;
        if (bad) { pt = L"Something is missing on this PC: see This PC."; pc = kWarn; }
    }
    text(g, pt, normal, gp::RectF(px, py, pw - (right.empty() ? 0 : Sf(260)), Sf(22)), pc);
    if (!right.empty()) text(g, right, normal, gp::RectF(px + pw - Sf(260), py, Sf(260), Sf(22)), kMuted, gp::StringAlignmentFar);
    if (bar) {
        gp::SolidBrush track(C(kLine)), fillb(C(kAccent));
        fill_round(g, gp::RectF(px, py + Sf(30), pw, Sf(8)), Sf(4), track);
        if (st.progress > 0) fill_round(g, gp::RectF(px, py + Sf(30), std::max(Sf(8), pw * static_cast<float>(st.progress)), Sf(8)), Sf(4), fillb);
    }
    if (s->def && s->in_library()) paint_steps(g, *s);
    (void)smallf;
    (void)rc;
}

// The steps card: one row per step with its own state, bar and time. The times of steps still to come are estimates; the header
// gives the setup's time as a whole (steps that can run side by side do, so it is less than the sum of the rows).
void App::paint_steps(gp::Graphics& g, const GameItem& s) {
    HDC dc = g.GetHDC();
    gp::Font bold(dc, bold_), normal(dc, font_), smallf(dc, small_);
    g.ReleaseHDC(dc);
    const RECT& R = steps_rect_;
    const float x = static_cast<float>(R.left), y0 = static_cast<float>(R.top), w = static_cast<float>(R.right - R.left);
    gp::SolidBrush card(C(kCard));
    fill_round(g, gp::RectF(x, y0, w, static_cast<float>(R.bottom - R.top)), Sf(12), card);
    // header: "Setup" and the output toggle
    int todo = 0, total = 0;
    for (int i = 0; i < S_COUNT; ++i) { total += s.planned(i) ? 1 : 0; todo += s.planned(i) && !(s.running && s.run[i].finished) ? 1 : 0; }
    std::wstring head = L"Setup";
    if (s.running) head += L" · step " + std::to_wstring(std::min(total, total - todo + 1)) + L" of " + std::to_wstring(total);
    else if (!todo) head += L" · done";
    text(g, head, bold, gp::RectF(x + Sf(20), y0 + Sf(14), w - Sf(200), Sf(24)), kText, gp::StringAlignmentNear, gp::StringAlignmentCenter);
    const std::wstring link = details_ ? L"Hide output" : L"Show output";
    details_rect_ = {static_cast<LONG>(x + w - Sf(150)), static_cast<LONG>(y0 + Sf(10)), static_cast<LONG>(x + w - Sf(16)), static_cast<LONG>(y0 + Sf(42))};
    text(g, link, normal, gp::RectF(x + w - Sf(150), y0 + Sf(14), Sf(134), Sf(24)), hover_nav_ == kHovDetails ? kAccentHi : kAccent, gp::StringAlignmentFar,
         gp::StringAlignmentCenter);
    gp::SolidBrush line(C(kLine));
    g.FillRectangle(&line, x + Sf(16), y0 + Sf(50), w - Sf(32), Sf(1));
    const double sp = s.speed();
    for (int i = 0; i < S_COUNT; ++i) {
        const StepRun& r = s.run[i];
        const float ry = y0 + Sf(56) + static_cast<float>(i) * Sf(46), cx = x + Sf(34), cy = ry + Sf(16);
        // state of this step
        enum { kPending, kRunning, kDone, kSkip, kFail } state = kPending;
        if (s.running && r.planned) state = r.running() ? kRunning : r.finished ? (r.ok ? kDone : kFail) : kPending;
        else if (!s.need[i]) state = kSkip;
        if (state == kSkip && s.run[i].finished && s.run[i].ok && s.run[i].planned) state = kDone;
        const bool done_before = state == kSkip && !s.why[i].empty() && s.why[i].rfind(L"Not needed", 0) != 0 && s.why[i].rfind(L"This package", 0) != 0;
        // icon
        if (state == kRunning) {
            gp::Pen ring(C(kLine), Sf(3));
            g.DrawEllipse(&ring, cx - Sf(9), cy - Sf(9), Sf(18), Sf(18));
            gp::Pen arc(C(kAccentHi), Sf(3));
            const float sweep = static_cast<float>(std::max(0.08, std::min(r.fraction(), 1.0)) * 360.0);
            g.DrawArc(&arc, cx - Sf(9), cy - Sf(9), Sf(18), Sf(18), -90.f, sweep);
        } else if (state == kDone || done_before) {
            gp::SolidBrush ok(C(kOk));
            g.FillEllipse(&ok, cx - Sf(10), cy - Sf(10), Sf(20), Sf(20));
            gp::Pen tick(C(kBg), Sf(2.4f));
            tick.SetStartCap(gp::LineCapRound);
            tick.SetEndCap(gp::LineCapRound);
            const gp::PointF pts[3] = {{cx - Sf(4.5f), cy}, {cx - Sf(1), cy + Sf(3.5f)}, {cx + Sf(5), cy - Sf(3.5f)}};
            g.DrawLines(&tick, pts, 3);
        } else if (state == kFail) {
            gp::SolidBrush bad(C(kBad));
            g.FillEllipse(&bad, cx - Sf(10), cy - Sf(10), Sf(20), Sf(20));
            gp::Pen xp(C(kBg), Sf(2.4f));
            g.DrawLine(&xp, cx - Sf(4), cy - Sf(4), cx + Sf(4), cy + Sf(4));
            g.DrawLine(&xp, cx + Sf(4), cy - Sf(4), cx - Sf(4), cy + Sf(4));
        } else if (state == kSkip) {
            gp::Pen dash(C(kMuted), Sf(2));
            g.DrawLine(&dash, cx - Sf(5), cy, cx + Sf(5), cy);
        } else {
            gp::Pen ring(C(kMuted), Sf(2));
            g.DrawEllipse(&ring, cx - Sf(8), cy - Sf(8), Sf(16), Sf(16));
        }
        // name and what it is doing / why
        const COLORREF nc = state == kSkip && !done_before ? kMuted : kText;
        text(g, kStepName[i], state == kRunning ? bold : normal, gp::RectF(x + Sf(58), ry + Sf(4), w - Sf(330), Sf(22)), nc);
        std::wstring sub, right;
        if (state == kRunning) {
            sub = step_counts(r);
            const int left = r.secs_left();
            right = left >= 0 ? span_text(left) + L" left" : L"Estimating…";
        } else if (state == kPending) {
            sub = s.why[i].rfind(L"after", 0) == 0 ? L"" : s.why[i];
            right = span_text(s.remaining(i, sp));
        } else if (state == kDone) {
            right = L"Done";
            if (s.running && r.finished && r.elapsed() > 0) sub = L"Took " + span_text(r.elapsed());
        } else if (state == kFail) {
            sub = r.error.empty() ? L"failed" : widen(r.error);
            right = L"Failed";
        } else {
            sub = s.why[i];
            right = done_before ? L"Done" : L"Skipped";
        }
        if (state == kRunning) {
            // the step's own bar, labelled by its row
            const float bx = x + Sf(58), bw = w - Sf(58) - Sf(190);
            text(g, sub, smallf, gp::RectF(bx, ry + Sf(22), bw, Sf(16)), kMuted);
            gp::SolidBrush track(C(kLine)), fillb(C(kAccent));
            fill_round(g, gp::RectF(bx, ry + Sf(40), bw, Sf(4)), Sf(2), track);
            const float f = static_cast<float>(std::min(r.fraction(), 1.0));
            if (f > 0) fill_round(g, gp::RectF(bx, ry + Sf(40), std::max(Sf(4), bw * f), Sf(4)), Sf(2), fillb);
        } else if (!sub.empty()) {
            text(g, sub, smallf, gp::RectF(x + Sf(58), ry + Sf(24), w - Sf(330), Sf(16)), state == kFail ? kBad : kMuted);
        }
        const COLORREF rc = state == kRunning ? kAccentHi : state == kDone || done_before ? kOk : state == kFail ? kBad : kMuted;
        text(g, right, state == kRunning ? bold : normal, gp::RectF(x + w - Sf(196), ry + Sf(4), Sf(176), Sf(22)), rc, gp::StringAlignmentFar);
    }
}

std::vector<const Check*> App::pc_checks() const {
    std::vector<const Check*> all;
    for (const Check& c : sys_checks_) all.push_back(&c);
    if (const GameItem* g = const_cast<App*>(this)->sel()) for (const Check& c : g->checks) all.push_back(&c);
    auto rank = [](Level l) { return l == Level::bad ? 0 : l == Level::warn ? 1 : 2; };
    std::stable_sort(all.begin(), all.end(), [&](const Check* a, const Check* b) { return rank(a->level) < rank(b->level); });
    return all;
}

// This PC: what needs attention first, each with what to do; the checks that pass as one compact list below.
void App::paint_pc(gp::Graphics& g) {
    HDC dc = g.GetHDC();
    gp::Font bold(dc, bold_), normal(dc, font_), smallf(dc, small_);
    g.ReleaseHDC(dc);
    const RECT& A = pc_rect_area_;
    const float x = static_cast<float>(A.left), w = static_cast<float>(A.right - A.left);
    gp::Region clip(gp::RectF(x - Sf(4), static_cast<float>(A.top), w + Sf(8), static_cast<float>(A.bottom - A.top)));
    g.SetClip(&clip);
    float y = static_cast<float>(A.top) - static_cast<float>(pc_scroll_);
    const auto all = pc_checks();
    auto measure = [&](const std::wstring& t, const gp::Font& f, float width) {
        gp::RectF out;
        gp::StringFormat sf;
        g.MeasureString(t.c_str(), static_cast<INT>(t.size()), &f, gp::RectF(0, 0, width, 10000), &sf, &out);
        return out.Height;
    };
    bool ok_head = false;
    for (const Check* c : all) {
        const bool problem = c->level == Level::bad || c->level == Level::warn;
        const COLORREF col = c->level == Level::ok ? kOk : c->level == Level::warn ? kWarn : c->level == Level::bad ? kBad : kMuted;
        if (problem) {
            // a card: the problem, what this PC has against what it needs, and the steps to fix it
            const float tw = w - Sf(72);
            const std::wstring fix = c->fix.empty() ? std::wstring() : c->fix;
            const float fh = fix.empty() ? 0 : measure(fix, normal, tw) + Sf(10);
            const float h = Sf(64) + fh;
            gp::SolidBrush card(C(kCard));
            fill_round(g, gp::RectF(x, y, w, h), Sf(12), card);
            gp::SolidBrush bar(C(col));
            fill_round(g, gp::RectF(x, y + Sf(12), Sf(4), h - Sf(24)), Sf(2), bar);
            text(g, c->level == Level::bad ? L"\u2715" : L"!", bold, gp::RectF(x + Sf(18), y + Sf(12), Sf(24), Sf(22)), col, gp::StringAlignmentCenter);
            text(g, c->item + L"  \u00B7  " + c->state, bold, gp::RectF(x + Sf(48), y + Sf(12), tw, Sf(22)), kText);
            text(g, (c->detail.empty() ? L"" : c->detail) + (c->need.empty() ? L"" : L"   \u2014   needs " + c->need), smallf,
                 gp::RectF(x + Sf(48), y + Sf(36), tw, Sf(18)), kMuted);
            if (!fix.empty()) text(g, fix, normal, gp::RectF(x + Sf(48), y + Sf(60), tw, fh), RGB(0xD8, 0xDC, 0xE4), gp::StringAlignmentNear, gp::StringAlignmentNear, true);
            y += h + Sf(12);
            continue;
        }
        if (!ok_head) {
            ok_head = true;
            y += Sf(6);
            text(g, L"Ready", bold, gp::RectF(x, y, w, Sf(22)), kMuted);
            y += Sf(30);
        }
        // a passed check: one line
        text(g, c->level == Level::ok ? L"\u2713" : L"\u2022", bold, gp::RectF(x + Sf(4), y, Sf(20), Sf(26)), col, gp::StringAlignmentCenter, gp::StringAlignmentCenter);
        text(g, c->item, normal, gp::RectF(x + Sf(32), y, w * 0.34f, Sf(26)), kText, gp::StringAlignmentNear, gp::StringAlignmentCenter);
        text(g, c->detail, normal, gp::RectF(x + Sf(32) + w * 0.34f, y, w * 0.66f - Sf(40), Sf(26)), kMuted, gp::StringAlignmentNear, gp::StringAlignmentCenter);
        y += Sf(32);
    }
    pc_content_h_ = static_cast<int>(y + static_cast<float>(pc_scroll_)) - A.top;
    g.ResetClip();
}

void App::paint(HDC hdc) {
    RECT rc;
    GetClientRect(wnd_, &rc);
    HDC dc = CreateCompatibleDC(hdc);
    HBITMAP bm = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
    HGDIOBJ old = SelectObject(dc, bm);
    {
        gp::Graphics g(dc);
        g.SetSmoothingMode(gp::SmoothingModeAntiAlias);
        g.SetTextRenderingHint(gp::TextRenderingHintClearTypeGridFit);
        gp::SolidBrush bg(C(kBg));
        g.FillRectangle(&bg, 0, 0, rc.right, rc.bottom);
        paint_side(g, rc);
        HDC fdc = g.GetHDC();
        gp::Font title(fdc, title_), normal(fdc, font_), smallf(fdc, small_), boldf(fdc, bold_);
        g.ReleaseHDC(fdc);
        const float x = Sf(kSideW) + Sf(32), w = static_cast<float>(rc.right) - x - Sf(32);
        if (page_ == P_LIB) {
            text(g, L"Your games", title, gp::RectF(x, Sf(24), w, Sf(34)), kText);
        } else if (page_ == P_GAME) {
            paint_game_header(g, rc);
            paint_tabs(g, boldf);
        } else if (page_ == P_SET) {
            const GameItem* sg = sel();
            paint_tabs(g, boldf);
            text(g, sg ? sg->title : std::wstring(), normal, gp::RectF(x, Sf(64), w - Sf(220), Sf(24)), kMuted, gp::StringAlignmentNear, gp::StringAlignmentCenter);
        } else if (page_ == P_PC) {
            text(g, L"This PC", title, gp::RectF(x, Sf(24), w, Sf(34)), kText);
            int total = 0, met = 0;
            for (const Check& c : sys_checks_) { ++total; met += c.level == Level::ok || c.level == Level::info; }
            if (const GameItem* sg = sel()) for (const Check& c : sg->checks) { ++total; met += c.level == Level::ok || c.level == Level::info; }
            wchar_t b[96];
            std::swprintf(b, 96, L"%d of %d checks met", met, total);
            if (sys_checks_.empty()) text(g, L"Checking\u2026", normal, gp::RectF(x, Sf(64), w, Sf(24)), kMuted);
            else {
                const int todo = total - met;
                text(g, todo == 0 ? std::wstring(L"\u2713  Everything this PC needs is there") : std::to_wstring(todo) + (todo == 1 ? L" thing needs" : L" things need") + L" attention",
                     normal, gp::RectF(x, Sf(64), w, Sf(24)), todo == 0 ? kOk : kWarn);
                paint_pc(g);
            }
            (void)b;
        }
        paint_report(g);
    }
    BitBlt(hdc, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, old);
    DeleteObject(bm);
    DeleteDC(dc);
}

void App::paint_cards(HWND h, HDC hdc) {
    RECT rc;
    GetClientRect(h, &rc);
    HDC dc = CreateCompatibleDC(hdc);
    HBITMAP bm = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
    HGDIOBJ old = SelectObject(dc, bm);
    hits_.clear();
    {
        gp::Graphics g(dc);
        g.SetSmoothingMode(gp::SmoothingModeAntiAlias);
        g.SetTextRenderingHint(gp::TextRenderingHintClearTypeGridFit);
        gp::SolidBrush bg(C(kBg));
        g.FillRectangle(&bg, 0, 0, rc.right, rc.bottom);
        HDC fdc = g.GetHDC();
        gp::Font title(fdc, title_), bold(fdc, bold_), normal(fdc, font_), smallf(fdc, small_);
        g.ReleaseHDC(fdc);
        const float pad = Sf(32), gap = Sf(20), cw = Sf(300), ch = Sf(280);
        const int cols = std::max(1, static_cast<int>((static_cast<float>(rc.right) - 2 * pad + gap) / (cw + gap)));
        // the player's games, then "add" (a known game shows up once a dump of it is added)
        std::vector<GameItem*> order;
        for (auto& gi : games_) if (gi->in_library()) order.push_back(gi.get());
        const int n = static_cast<int>(order.size()) + 1;
        cards_cols_ = cols;
        cards_n_ = n;
        if (kb_card_ >= n) kb_card_ = n - 1;
        const bool kb = GetFocus() == h && kb_card_ >= 0;
        content_h_ = static_cast<int>(pad + ((n + cols - 1) / cols) * (ch + gap));
        const float oy = -static_cast<float>(scroll_);
        if (order.empty()) {
            // empty library: one clear way in
            const float W = static_cast<float>(rc.right), cx = W / 2, top = std::max(Sf(40), static_cast<float>(rc.bottom) / 2 - Sf(200));
            text(g, L"Add your first game", title, gp::RectF(0, top, W, Sf(40)), kText, gp::StringAlignmentCenter);
            text(g, L"Choose your own dump: a .nup package or an extracted folder with exefs and romfs.", normal, gp::RectF(Sf(40), top + Sf(48), W - Sf(80), Sf(24)),
                 kMuted, gp::StringAlignmentCenter);
            const RECT b{static_cast<LONG>(cx - Sf(120)), static_cast<LONG>(top + Sf(100)), static_cast<LONG>(cx + Sf(120)), static_cast<LONG>(top + Sf(152))};
            const int hi = static_cast<int>(hits_.size());
            hits_.push_back({b, nullptr, 4});
            const bool hv = hover_hit_ == hi || kb;
            gp::SolidBrush bb(C(hv ? kAccentHi : kAccent));
            const gp::RectF bf(static_cast<float>(b.left), static_cast<float>(b.top), static_cast<float>(b.right - b.left), static_cast<float>(b.bottom - b.top));
            fill_round(g, bf, Sf(10), bb);
            text(g, L"+  Add a game", bold, bf, RGB(255, 255, 255), gp::StringAlignmentCenter, gp::StringAlignmentCenter);
            if (kb) { gp::Pen fp(C(kText), Sf(2)); gp::GraphicsPath pth; round_path(pth, gp::RectF(bf.X - Sf(4), bf.Y - Sf(4), bf.Width + Sf(8), bf.Height + Sf(8)), Sf(13)); g.DrawPath(&fp, &pth); }
            text(g, L"or drop it on this window", smallf, gp::RectF(0, top + Sf(166), W, Sf(20)), kMuted, gp::StringAlignmentCenter);
            content_h_ = 0;
        }
        for (int i = 0; i < n && !order.empty(); ++i) {
            const float x = pad + static_cast<float>(i % cols) * (cw + gap), y = Sf(8) + oy + static_cast<float>(i / cols) * (ch + gap);
            const RECT card{static_cast<LONG>(x), static_cast<LONG>(y), static_cast<LONG>(x + cw), static_cast<LONG>(y + ch)};
            if (i == n - 1) {
                // add a game
                const int hi = static_cast<int>(hits_.size());
                hits_.push_back({card, nullptr, 4});
                const bool hv = hover_hit_ == hi && !hover_game_;
                gp::Pen pen(C(hv ? kAccent : kLine), Sf(2));
                pen.SetDashStyle(gp::DashStyleDash);
                gp::GraphicsPath p;
                round_path(p, gp::RectF(x + Sf(1), y + Sf(1), cw - Sf(2), ch - Sf(2)), Sf(14));
                if (hv) { gp::SolidBrush hb(C(kCard)); g.FillPath(&hb, &p); }
                g.DrawPath(&pen, &p);
                if (kb && kb_card_ == i) { gp::Pen fp(C(kAccentHi), Sf(2)); g.DrawPath(&fp, &p); }
                text(g, L"+", title, gp::RectF(x, y + ch / 2 - Sf(48), cw, Sf(40)), hv ? kAccentHi : kMuted, gp::StringAlignmentCenter, gp::StringAlignmentCenter);
                text(g, L"Add a game", bold, gp::RectF(x, y + ch / 2, cw, Sf(20)), hv ? kText : kMuted, gp::StringAlignmentCenter);
                text(g, L"or drop it here", smallf, gp::RectF(x, y + ch / 2 + Sf(22), cw, Sf(18)), kMuted, gp::StringAlignmentCenter);
                continue;
            }
            GameItem& gi = *order[static_cast<size_t>(i)];
            hits_.push_back({card, &gi, 0});
            const bool hv = hover_game_ == &gi;
            gp::SolidBrush cb(C(hv ? kCardHi : kCard));
            gp::GraphicsPath path;
            round_path(path, gp::RectF(x, y, cw, ch), Sf(14));
            g.FillPath(&cb, &path);
            // banner
            COLORREF a, b;
            banner_colors(gi.id, &a, &b);
            g.SetClip(&path);
            gp::LinearGradientBrush lg(gp::PointF(x, y), gp::PointF(x + cw, y + Sf(140)), C(a), C(b));
            g.FillRectangle(&lg, x, y, cw, Sf(140));
            g.ResetClip();
            const std::wstring initials = [&] {
                std::wstring t;
                bool next = true;
                for (wchar_t c : gi.short_title) { if (next && iswalnum(c)) { t += static_cast<wchar_t>(towupper(c)); next = false; } if (c == L' ') next = true; }
                return gi.def ? t.substr(0, 3) : std::wstring(L"?");
            }();
            text(g, initials, title, gp::RectF(x + Sf(18), y + Sf(14), cw - Sf(36), Sf(40)), RGB(255, 255, 255));
            text(g, gi.def ? (gi.def->generic ? L"EXPERIMENTAL" : L"VERSION " + widen(gi.def->display_version)) : std::wstring(L"NO PORT YET"), smallf, gp::RectF(x + Sf(18), y + Sf(112), cw - Sf(36), Sf(16)),
                 RGB(0xE8, 0xEA, 0xF0));
            // texts
            text(g, gi.title, bold, gp::RectF(x + Sf(18), y + Sf(152), cw - Sf(36), Sf(20)), kText);
            const Status st = status_of(gi);
            gp::SolidBrush dotb(C(st.color));
            g.FillEllipse(&dotb, x + Sf(18), y + Sf(186), Sf(8), Sf(8));
            text(g, st.text, normal, gp::RectF(x + Sf(32), y + Sf(178), cw - Sf(50), Sf(22)), st.color, gp::StringAlignmentNear, gp::StringAlignmentCenter);
            if (st.progress >= 0) {
                gp::SolidBrush track(C(kLine)), fillb(C(kAccent));
                fill_round(g, gp::RectF(x + Sf(18), y + Sf(206), cw - Sf(36), Sf(5)), Sf(2.5f), track);
                if (st.progress > 0) fill_round(g, gp::RectF(x + Sf(18), y + Sf(206), std::max(Sf(5), (cw - Sf(36)) * static_cast<float>(st.progress)), Sf(5)), Sf(2.5f), fillb);
            }
            // buttons: Play and Prepare / Cancel
            auto btn = [&](float bx, float bw, const std::wstring& t, int action, bool primary, bool enabled) {
                const RECT r{static_cast<LONG>(bx), static_cast<LONG>(y + ch - Sf(52)), static_cast<LONG>(bx + bw), static_cast<LONG>(y + ch - Sf(16))};
                const int idx = static_cast<int>(hits_.size());
                if (enabled) hits_.push_back({r, &gi, action});
                const bool h2 = enabled && hover_hit_ == idx;
                COLORREF fill = primary ? (h2 ? kAccentHi : kAccent) : (h2 ? RGB(0x3A, 0x40, 0x4D) : RGB(0x30, 0x35, 0x40));
                if (!enabled) fill = RGB(0x26, 0x2A, 0x33);
                gp::SolidBrush bb(C(fill));
                fill_round(g, gp::RectF(static_cast<gp::REAL>(r.left), static_cast<gp::REAL>(r.top), bw, Sf(36)), Sf(8), bb);
                text(g, t, bold, gp::RectF(static_cast<gp::REAL>(r.left), static_cast<gp::REAL>(r.top), bw, Sf(36)), enabled ? RGB(255, 255, 255) : kMuted,
                     gp::StringAlignmentCenter, gp::StringAlignmentCenter);
            };
            const float full = cw - Sf(36);
            if (gi.running || gi.queued) {
                btn(x + Sf(18), full, L"Cancel", 3, false, true);
            } else {
                bool en = false;
                const std::wstring l = primary_label(gi, &en);
                if (en) btn(x + Sf(18), full, l == L"Play" ? L"\u25B6  Play" : l, 1, true, true);   // nothing to press: the status says why
            }
            if (kb && kb_card_ == i) {   // keyboard focus
                gp::Pen fp(C(kAccentHi), Sf(2));
                gp::GraphicsPath fpath;
                round_path(fpath, gp::RectF(x - Sf(3), y - Sf(3), cw + Sf(6), ch + Sf(6)), Sf(16));
                g.DrawPath(&fp, &fpath);
            }
        }
    }
    BitBlt(hdc, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, old);
    DeleteObject(bm);
    DeleteDC(dc);
    SCROLLINFO si{sizeof si, SIF_RANGE | SIF_PAGE | SIF_POS, 0, std::max(0, content_h_), static_cast<UINT>(rc.bottom), scroll_};
    SetScrollInfo(h, SB_VERT, &si, TRUE);
}

void App::draw_button(const DRAWITEMSTRUCT* d) {
    Btn kind = Btn::normal;
    for (auto& [h, k] : btns_) if (h == d->hwndItem) kind = k;
    const bool enabled = IsWindowEnabled(d->hwndItem), pressed = d->itemState & ODS_SELECTED, focus = (d->itemState & ODS_FOCUS) && !(d->itemState & ODS_NOFOCUSRECT);
    POINT pt;
    GetCursorPos(&pt);
    RECT wr;
    GetWindowRect(d->hwndItem, &wr);
    const bool hot = enabled && PtInRect(&wr, pt);
    const RECT r = d->rcItem;
    HDC dc = CreateCompatibleDC(d->hDC);
    HBITMAP bm = CreateCompatibleBitmap(d->hDC, r.right, r.bottom);
    HGDIOBJ old = SelectObject(dc, bm);
    {
        gp::Graphics g(dc);
        g.SetSmoothingMode(gp::SmoothingModeAntiAlias);
        g.SetTextRenderingHint(gp::TextRenderingHintClearTypeGridFit);
        // the parent's background behind the rounded corners
        COLORREF behind = kBg;
        gp::SolidBrush bg(C(behind));
        g.FillRectangle(&bg, 0, 0, r.right, r.bottom);
        COLORREF fill = kind == Btn::primary ? (hot ? kAccentHi : kAccent) : kind == Btn::danger ? (hot ? RGB(0x5A, 0x2A, 0x2E) : RGB(0x45, 0x24, 0x28))
                                                                                                 : (hot ? RGB(0x3A, 0x40, 0x4D) : RGB(0x2C, 0x31, 0x3C));
        if (!enabled) fill = RGB(0x1E, 0x21, 0x29);
        if (pressed) fill = kind == Btn::primary ? kAccent : RGB(0x24, 0x28, 0x31);
        gp::SolidBrush b(C(fill));
        fill_round(g, gp::RectF(0, 0, static_cast<gp::REAL>(r.right), static_cast<gp::REAL>(r.bottom)), Sf(8), b);
        wchar_t t[128] = {};
        GetWindowTextW(d->hwndItem, t, 128);
        std::wstring label = t;
        if (d->hwndItem == b_play_ && label == L"Play") label = L"\u25B6  " + label;
        HDC fdc = g.GetHDC();
        gp::Font f(fdc, bold_);
        g.ReleaseHDC(fdc);
        COLORREF tc = !enabled ? RGB(0x5E, 0x64, 0x70) : kind == Btn::danger ? RGB(0xFF, 0xA0, 0xA0) : RGB(255, 255, 255);
        text(g, label, f, gp::RectF(0, 0, static_cast<gp::REAL>(r.right), static_cast<gp::REAL>(r.bottom)), tc, gp::StringAlignmentCenter, gp::StringAlignmentCenter);
        if (focus) {   // keyboard focus
            gp::Pen fp(C(kind == Btn::primary ? RGB(255, 255, 255) : kAccentHi), Sf(1.5f));
            gp::GraphicsPath p;
            round_path(p, gp::RectF(Sf(2), Sf(2), static_cast<gp::REAL>(r.right) - Sf(4), static_cast<gp::REAL>(r.bottom) - Sf(4)), Sf(6));
            g.DrawPath(&fp, &p);
        }
    }
    BitBlt(d->hDC, 0, 0, r.right, r.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, old);
    DeleteObject(bm);
    DeleteDC(dc);
}

// ---- messages ----------------------------------------------------------------------------------------------------------------------
LRESULT App::notify(NMHDR* n) {
    (void)n;
    return 0;
}
void App::command(int id, int code) {
    if (code != BN_CLICKED && code != 1 && code != 0) return;
    GameItem* g = sel();
    switch (id) {
        case ID_PLAY: if (g) primary(*g); break;
        case ID_PLAY_AS_IS: if (g) play_now(*g); break;
        case ID_MORE: more_menu(); break;
        case ID_DETAILS: details_ = !details_; break;
        case ID_PREP:
            if (!g) break;
            if (g->ready()) { g->error.clear(); g->prep_check.clear(); evaluate(*g); start_prepare_check(*g); }
            else enqueue(*g, false);
            break;
        case ID_CANCEL: if (g) cancel(*g); break;
        case ID_REBUILD: if (g) rebuild_dialog(*g); break;
        case ID_SRC_NUP: if (g) choose_source(*g, false); break;
        case ID_SRC_FOLDER: if (g) choose_source(*g, true); break;
        case ID_DATA: if (g) choose_data(*g); break;
        case ID_OPEN_DATA:
            if (g) { std::error_code ec; fs::create_directories(g->cfg.data_dir, ec); ShellExecuteW(wnd_, L"open", g->cfg.data_dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL); }
            break;
        case ID_SHORTCUT: if (g) make_shortcut(*g); break;
        case ID_REMOVE: if (g) remove_game(*g); break;
        case ID_OPEN_INI:
            if (g && !g->lay.game.empty()) {
                const std::wstring f = join(g->lay.game, L"controls.ini");
                if (file_exists(f)) ShellExecuteW(wnd_, L"open", L"notepad.exe", quote(f).c_str(), nullptr, SW_SHOWNORMAL);
                else MessageBoxW(wnd_, (L"There is no settings file yet:\r\n" + f + L"\r\n\r\nThe game writes it when it starts; a change on this page creates it too.").c_str(),
                                 kAppName, MB_OK | MB_ICONINFORMATION);
            }
            break;
        case ID_RECHECK: case IDM_RECHECK:
            sys_checks_.clear();
            for (auto& gi : games_) { gi->prep_check.clear(); evaluate(*gi); }
            start_requirements();
            if (g) start_prepare_check(*g);
            break;
        case ID_COPY: {
            // every check as text: what it needs, what this PC has, and what to do (for a bug report or a forum post)
            std::wstring t = std::wstring(kAppName) + L" " + widen(RECOMP_LIBRARY_VERSION) + L"\r\n";
            for (const Check* c : pc_checks()) {
                const wchar_t* mark = c->level == Level::ok ? L"[ok]  " : c->level == Level::warn ? L"[!]   " : c->level == Level::bad ? L"[x]   " : L"[i]   ";
                t += mark + c->item + L": " + c->state + (c->detail.empty() ? L"" : L" (" + c->detail + L")") + L"\r\n";
                if (!c->fix.empty()) t += L"      " + c->fix + L"\r\n";
            }
            if (OpenClipboard(wnd_)) {
                EmptyClipboard();
                if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (t.size() + 1) * sizeof(wchar_t))) {
                    std::memcpy(GlobalLock(h), t.c_str(), (t.size() + 1) * sizeof(wchar_t));
                    GlobalUnlock(h);
                    SetClipboardData(CF_UNICODETEXT, h);
                }
                CloseClipboard();
            }
            break;
        }
        case ID_ADD: case IDM_ADD: add_game(); break;
        case IDM_ABOUT: about(); break;
        case IDM_REPORT: report(g); break;
        case IDM_EXIT: PostMessageW(wnd_, WM_CLOSE, 0, 0); break;
        case IDCANCEL: case IDM_BACK: if (page_ != P_LIB) show_page(P_LIB); break;
        case IDM_PREV_GAME: case IDM_NEXT_GAME: {
            // Ctrl+Up / Ctrl+Down: the game above or below in the side list
            std::vector<GameItem*> order;
            for (auto& gi : games_) if (gi->in_library()) order.push_back(gi.get());
            if (order.empty()) break;
            int at = 0;
            for (size_t i = 0; i < order.size(); ++i) if (order[i] == g) at = static_cast<int>(i);
            at = std::clamp(at + (id == IDM_NEXT_GAME ? 1 : -1), 0, static_cast<int>(order.size()) - 1);
            select(order[static_cast<size_t>(at)]->id);
            show_page(page_ == P_SET ? P_SET : P_GAME);
            break;
        }
        default:
            if (id >= IDM_PAGE0 && id < static_cast<int>(IDM_PAGE0) + P_COUNT) show_page(id - static_cast<int>(IDM_PAGE0));
    }
    refresh();
    InvalidateRect(wnd_, nullptr, FALSE);
    InvalidateRect(cards_, nullptr, FALSE);
}

LRESULT CALLBACK App::proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCCREATE) g_app->wnd_ = h;
    if (!g_app || !g_app->wnd_) return DefWindowProcW(h, m, w, l);
    try {
        return g_app->on(m, w, l);
    } catch (const std::exception& e) {
        MessageBoxA(h, e.what(), "Recomp Library: internal error", MB_OK | MB_ICONERROR);
        return DefWindowProcW(h, m, w, l);
    }
}

LRESULT CALLBACK App::cards_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (!g_app) return DefWindowProcW(h, m, w, l);
    return g_app->on_cards(h, m, w, l);
}

LRESULT App::on_cards(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            paint_cards(h, dc);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_MOUSEMOVE: {
            const POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            int hit = -1;
            // the last matching rect wins: buttons are pushed after their card
            for (int i = 0; i < static_cast<int>(hits_.size()); ++i) if (PtInRect(&hits_[static_cast<size_t>(i)].r, p)) hit = i;
            GameItem* hg = hit >= 0 ? hits_[static_cast<size_t>(hit)].g : nullptr;
            if (hit != hover_hit_ || hg != hover_game_) { hover_hit_ = hit; hover_game_ = hg; InvalidateRect(h, nullptr, FALSE); }
            TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, h, 0};
            TrackMouseEvent(&t);
            SetCursor(LoadCursor(nullptr, hit >= 0 ? IDC_HAND : IDC_ARROW));
            return 0;
        }
        case WM_MOUSELEAVE: hover_hit_ = -1; hover_game_ = nullptr; InvalidateRect(h, nullptr, FALSE); return 0;
        case WM_GETDLGCODE: {
            const MSG* msg = reinterpret_cast<const MSG*>(l);
            if (msg && msg->message == WM_KEYDOWN && (msg->wParam == VK_RETURN || msg->wParam == VK_SPACE)) return DLGC_WANTMESSAGE;
            return DLGC_WANTARROWS | DLGC_WANTCHARS;
        }
        case WM_SETFOCUS: case WM_KILLFOCUS: InvalidateRect(h, nullptr, FALSE); return 0;
        case WM_LBUTTONDOWN: SetFocus(h); return 0;
        case WM_KEYDOWN: {
            // arrows move between the cards, Enter or Space opens a game (or adds one), P plays it
            int k = kb_card_;
            if (w == VK_LEFT) k -= 1;
            else if (w == VK_RIGHT) k += 1;
            else if (w == VK_UP) k -= cards_cols_;
            else if (w == VK_DOWN) k += cards_cols_;
            else if (w == VK_HOME) k = 0;
            else if (w == VK_END) k = cards_n_ - 1;
            else if (w == VK_RETURN || w == VK_SPACE || w == 'P') {
                std::vector<GameItem*> order;
                for (auto& gi : games_) if (gi->in_library()) order.push_back(gi.get());
                if (kb_card_ >= static_cast<int>(order.size())) { add_game(); return 0; }
                GameItem& g = *order[static_cast<size_t>(kb_card_)];
                select(g.id);
                if (w == 'P') primary(g);
                else show_page(P_GAME);
                return 0;
            } else break;
            kb_card_ = std::clamp(k, 0, std::max(0, cards_n_ - 1));
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        case WM_SETCURSOR: return TRUE;
        case WM_LBUTTONUP: {
            const POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            int hit = -1;
            for (int i = 0; i < static_cast<int>(hits_.size()); ++i) if (PtInRect(&hits_[static_cast<size_t>(i)].r, p)) hit = i;
            if (hit < 0) return 0;
            const Hit hh = hits_[static_cast<size_t>(hit)];
            if (hh.action == 4) { add_game(); return 0; }
            GameItem& g = *hh.g;
            if (hh.action == 1) { select(g.id); primary(g); }
            else if (hh.action == 3) cancel(g);
            else { select(g.id); show_page(P_GAME); if (!g.in_library() && g.def) choose_source(g, false); }
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        case WM_MOUSEWHEEL: {
            RECT rc;
            GetClientRect(h, &rc);
            scroll_ = std::clamp(scroll_ - GET_WHEEL_DELTA_WPARAM(w) * S(60) / WHEEL_DELTA, 0, std::max(0, content_h_ - static_cast<int>(rc.bottom)));
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        case WM_VSCROLL: {
            RECT rc;
            GetClientRect(h, &rc);
            SCROLLINFO si{sizeof si, SIF_ALL};
            GetScrollInfo(h, SB_VERT, &si);
            int pos = scroll_;
            switch (LOWORD(w)) {
                case SB_LINEUP: pos -= S(40); break;
                case SB_LINEDOWN: pos += S(40); break;
                case SB_PAGEUP: pos -= rc.bottom; break;
                case SB_PAGEDOWN: pos += rc.bottom; break;
                case SB_THUMBTRACK: case SB_THUMBPOSITION: pos = si.nTrackPos; break;
            }
            scroll_ = std::clamp(pos, 0, std::max(0, content_h_ - static_cast<int>(rc.bottom)));
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
    }
    return DefWindowProcW(h, m, w, l);
}

LRESULT App::on(UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_CREATE: {
            dpi_ = static_cast<int>(GetDpiForWindow(wnd_));
            BOOL dark = TRUE;
            DwmSetWindowAttribute(wnd_, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof dark);
            DragAcceptFiles(wnd_, TRUE);
            make_fonts();
            settings::register_class(inst_);
            create_controls();
            settings::set_fonts(settings_, {font_, bold_, small_, title_, dpi_});
            update_settings();
            layout();
            show_page(page_);
            start_requirements();
            if (GameItem* g = sel()) start_prepare_check(*g);
            SetTimer(wnd_, kTimer, 250, nullptr);
            for (const std::wstring& p : add_paths_) add_dump(p, true);
            if (!add_paths_.empty()) show_page(page_);
            if (auto_start_) if (GameItem* g = sel()) enqueue(*g, false);
            if (start_tab_ >= 0) settings::set_tab(settings_, start_tab_);
            return 0;
        }
        case WM_SIZE: layout(); return 0;
        case WM_GETMINMAXINFO: {
            auto* mm = reinterpret_cast<MINMAXINFO*>(l);
            mm->ptMinTrackSize = {S(980), S(800)};
            return 0;
        }
        case WM_DPICHANGED: {
            dpi_ = HIWORD(w);
            make_fonts();
            settings::set_fonts(settings_, {font_, bold_, small_, title_, dpi_});
            for (HWND h : {log_}) SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(font_), FALSE);
            for (auto& [h, k] : btns_) SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(bold_), FALSE);
            const RECT* r = reinterpret_cast<const RECT*>(l);
            SetWindowPos(wnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            layout();
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(wnd_, &ps);
            paint(dc);
            EndPaint(wnd_, &ps);
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_DRAWITEM: {
            auto* d = reinterpret_cast<const DRAWITEMSTRUCT*>(l);
            if (d->CtlType == ODT_BUTTON) { draw_button(d); return TRUE; }
            break;
        }
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(w);
            SetTextColor(dc, RGB(0xC8, 0xCC, 0xD4));
            SetBkColor(dc, kField);
            return reinterpret_cast<LRESULT>(field_brush_);
        }
        case WM_MOUSEMOVE: {
            const POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            nav_layout();
            int hov = -1;
            for (size_t i = 0; i < nav_games_.size(); ++i) if (PtInRect(&nav_games_[i].first, p)) hov = static_cast<int>(i);
            if (PtInRect(&pc_rect_, p)) hov = kHovPc;
            if (PtInRect(&brand_rect_, p)) hov = kHovBrand;
            if (PtInRect(&add_rect_, p)) hov = kHovAdd;
            if (PtInRect(&report_rect_, p)) hov = kHovReport;
            if (page_ == P_GAME && sel() && PtInRect(&details_rect_, p)) hov = kHovDetails;
            if (sel() && (page_ == P_GAME || page_ == P_SET))
                for (int i = 0; i < 2; ++i) if (PtInRect(&tab_rect_[i], p)) hov = kHovTab + i;
            if (hov != hover_nav_) {
                hover_nav_ = hov;
                InvalidateRect(wnd_, nullptr, FALSE);
            }
            TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, wnd_, 0};
            TrackMouseEvent(&t);
            // the owner-drawn buttons show their hover state
            for (auto& [h, k] : btns_) if (IsWindowVisible(h)) InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        case WM_MOUSELEAVE: {
            hover_nav_ = -1;
            InvalidateRect(wnd_, nullptr, FALSE);
            for (auto& [h, k] : btns_) if (IsWindowVisible(h)) InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        case WM_SETCURSOR:
            if (LOWORD(l) == HTCLIENT && (hover_nav_ >= 0)) { SetCursor(LoadCursor(nullptr, IDC_HAND)); return TRUE; }
            break;
        case WM_LBUTTONUP: {
            const POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            nav_layout();
            for (const auto& [r, gi] : nav_games_)
                if (PtInRect(&r, p)) { select(gi->id); show_page(page_ == P_SET ? P_SET : P_GAME); return 0; }
            if (PtInRect(&pc_rect_, p)) show_page(P_PC);
            else if (PtInRect(&brand_rect_, p)) show_page(P_LIB);
            else if (PtInRect(&add_rect_, p)) add_game();
            else if (PtInRect(&report_rect_, p)) report(sel());
            else if (page_ == P_GAME && sel() && PtInRect(&details_rect_, p)) { details_ = !details_; refresh(); InvalidateRect(wnd_, nullptr, FALSE); }
            else if (sel() && (page_ == P_GAME || page_ == P_SET))
                for (int i = 0; i < 2; ++i) if (PtInRect(&tab_rect_[i], p)) show_page(i == 0 ? P_GAME : P_SET);
            return 0;
        }
        case WM_COMMAND: command(LOWORD(w), HIWORD(w)); return 0;
        case WM_MOUSEWHEEL:
            if (page_ == P_PC) {
                const int view = pc_rect_area_.bottom - pc_rect_area_.top;
                pc_scroll_ = std::clamp(pc_scroll_ - GET_WHEEL_DELTA_WPARAM(w) * S(60) / WHEEL_DELTA, 0, std::max(0, pc_content_h_ - view));
                InvalidateRect(wnd_, &pc_rect_area_, FALSE);
                return 0;
            }
            break;
        case WM_DROPFILES: {
            // a .nup file or a game folder dropped on the window: add it
            HDROP d = reinterpret_cast<HDROP>(w);
            const UINT n = DragQueryFileW(d, 0xFFFFFFFF, nullptr, 0);
            std::vector<std::wstring> paths;
            for (UINT i = 0; i < n; ++i) {
                wchar_t p[MAX_PATH * 4];
                if (DragQueryFileW(d, i, p, static_cast<UINT>(std::size(p)))) paths.push_back(p);
            }
            DragFinish(d);
            for (const std::wstring& p : paths) add_dump(p, false);
            return 0;
        }
        case WM_NOTIFY: return notify(reinterpret_cast<NMHDR*>(l));
        case WM_TIMER:
            if (active_) {
                // a stop that takes too long (a child that does not react): end the processes
                if (active_->stopping && !active_->killed && std::chrono::steady_clock::now() - active_->stop_at > std::chrono::seconds(20)) {
                    active_->killed = true;
                    for (StepRun& r : active_->run) if (r.running()) r.child.kill();
                }
                InvalidateRect(cards_, nullptr, FALSE);
                RECT side{0, 0, S(kSideW), 10000};
                InvalidateRect(wnd_, &side, FALSE);
                if (page_ == P_GAME && sel() == active_) {
                    RECT top{S(kSideW), 0, 10000, S(200)};
                    InvalidateRect(wnd_, &top, FALSE);
                    InvalidateRect(wnd_, &prog_rect_, FALSE);
                    InvalidateRect(wnd_, &steps_rect_, FALSE);
                }
            }
            return 0;
        case kMsgLine: {
            std::unique_ptr<std::string> s(reinterpret_cast<std::string*>(l));
            step_line(*reinterpret_cast<StepRun*>(w), *s);
            return 0;
        }
        case kMsgExit: step_exit(*reinterpret_cast<StepRun*>(w), static_cast<DWORD>(l)); return 0;
        case kMsgGameExit: {
            GameItem& g = *reinterpret_cast<GameItem*>(w);
            g.game_running = false;
            if (sel() == &g) update_settings();   // the game may have changed its settings on the way out
            refresh();
            InvalidateRect(wnd_, nullptr, FALSE);
            InvalidateRect(cards_, nullptr, FALSE);
            return 0;
        }
        case WM_COPYDATA: {
            // another start of the library (the game's Settings entry: --game ID --page N --tab N) hands its command line over
            const auto* cd = reinterpret_cast<const COPYDATASTRUCT*>(l);
            if (cd->dwData != kCopyArgs || !cd->lpData) return FALSE;
            const std::wstring line(static_cast<const wchar_t*>(cd->lpData), cd->cbData / sizeof(wchar_t));
            handle_args(line, true);
            if (IsIconic(wnd_)) ShowWindow(wnd_, SW_RESTORE);
            SetForegroundWindow(wnd_);
            return TRUE;
        }
        case kMsgCheck: {
            std::unique_ptr<std::string> s(reinterpret_cast<std::string*>(l));
            GameItem& g = *reinterpret_cast<GameItem*>(w);
            g.check_child.wait();
            g.checking_prep = false;
            g.prep_check = widen(*s);
            evaluate(g);
            refresh();
            InvalidateRect(wnd_, nullptr, FALSE);
            InvalidateRect(cards_, nullptr, FALSE);
            return 0;
        }
        case kMsgDlss: {
            std::unique_ptr<std::wstring> s(reinterpret_cast<std::wstring*>(l));
            dlss_child_.wait();
            if (!s->empty()) MessageBoxW(wnd_, s->c_str(), kAppName, MB_ICONWARNING);
            update_settings();
            start_requirements();
            return 0;
        }
        case kMsgReqs: {
            std::unique_ptr<std::vector<Check>> r(reinterpret_cast<std::vector<Check>*>(l));
            sys_checks_ = std::move(*r);
            checks_running_ = false;
            refresh();
            RECT side{0, 0, S(kSideW), 10000};
            InvalidateRect(wnd_, &side, FALSE);
            return 0;
        }
        case WM_CLOSE:
            if (active_) {
                if (closing_) return 0;   // already stopping: the window goes when the last step has ended
                if (MessageBoxW(wnd_, L"A game is being prepared. Stop it and close? What is done is kept; Prepare continues later.", kAppName,
                                MB_YESNO | MB_ICONQUESTION) != IDYES)
                    return 0;
                queue_.clear();
                closing_ = true;
                SetWindowTextW(wnd_, (std::wstring(kAppName) + L" - stopping...").c_str());
                cancel(*active_);   // the window goes when the last step has ended (finish_job), after 20 s the processes are ended
                return 0;
            }
            DestroyWindow(wnd_);
            return 0;
        case WM_DESTROY:
            KillTimer(wnd_, kTimer);
            for (auto& g : games_) if (g->check_child.running()) g->check_child.cancel();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(wnd_, m, w, l);
}

[[maybe_unused]] HMENU make_menu() {
    HMENU bar = CreateMenu(), file = CreatePopupMenu(), view = CreatePopupMenu(), help = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, IDM_ADD, L"&Add a game...\tCtrl+O");
    AppendMenuW(file, MF_STRING, IDM_RECHECK, L"&Check this PC again\tF5");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, IDM_EXIT, L"E&xit");
    const wchar_t* pages[P_COUNT] = {L"&Library\tCtrl+1", L"&Game\tCtrl+2", L"&Settings\tCtrl+3", L"&This PC\tCtrl+4"};
    for (int i = 0; i < P_COUNT; ++i) AppendMenuW(view, MF_STRING, IDM_PAGE0 + i, pages[i]);
    AppendMenuW(help, MF_STRING, IDM_REPORT, L"&Report a bug...");
    AppendMenuW(help, MF_STRING, IDM_ABOUT, L"&About");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"&File");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(view), L"&View");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(help), L"&Help");
    return bar;
}

int App::run(HINSTANCE inst, int show) {
    inst_ = inst;
    gp::GdiplusStartupInput gsi;
    gp::GdiplusStartup(&gdiplus_, &gsi, nullptr);
    bg_brush_ = CreateSolidBrush(kBg);
    field_brush_ = CreateSolidBrush(kField);
    tools_ = find_tools();
    load_games();
    // command line
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring data, source;
    for (int i = 1; argv && i < argc; ++i) {
        const std::wstring a = argv[i];
        const bool more = i + 1 < argc;
        if (a == L"--game" && more) { const std::string id = narrow(argv[++i]); if (item(id)) lib_.selected = id; }
        else if (a == L"--data" && more) data = fs::absolute(argv[++i]).wstring();
        else if (a == L"--source" && more) source = fs::absolute(argv[++i]).wstring();
        else if (a == L"--page" && more) page_ = _wtoi(argv[++i]);
        else if (a == L"--start") auto_start_ = true;
        else if (a == L"--game-args" && more) game_args_ = argv[++i];
        else if (a == L"--add" && more) add_paths_.push_back(fs::absolute(argv[++i]).wstring());
        else if (a == L"--tab" && more) start_tab_ = _wtoi(argv[++i]);
    }
    if (argv) LocalFree(argv);
    // one library at a time: a second start (the game's Settings entry, a shortcut) hands its command line to the running one
    if (HWND other = FindWindowW(kClass, nullptr)) {
        const std::wstring line = GetCommandLineW();
        COPYDATASTRUCT cd{kCopyArgs, static_cast<DWORD>((line.size() + 1) * sizeof(wchar_t)), const_cast<wchar_t*>(line.c_str())};
        DWORD pid = 0;
        GetWindowThreadProcessId(other, &pid);
        AllowSetForegroundWindow(pid);
        DWORD_PTR res = 0;
        if (SendMessageTimeoutW(other, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cd), SMTO_ABORTIFHUNG, 5000, &res) && res) {
            gp::GdiplusShutdown(gdiplus_);
            return 0;
        }
    }
    if (GameItem* g = item(lib_.selected); g && (!data.empty() || !source.empty())) {
        if (!data.empty()) g->cfg.data_dir = data;
        if (!source.empty()) {
            g->cfg.source = source;
            if (!lib_.find(g->id)) { lib_.games.push_back({}); lib_.games.back().id = g->id; lib_.games.back().title_id = g->title_id; }
        }
        save_lib();
        evaluate(*g);
    }

    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hbrBackground = bg_brush_;
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
    WNDCLASSEXW cc{sizeof cc};
    cc.lpfnWndProc = cards_proc;
    cc.hInstance = inst;
    cc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    cc.lpszClassName = kCardsClass;
    RegisterClassExW(&cc);
    const int sys = static_cast<int>(GetDpiForSystem());
    const int w = MulDiv(1200, sys, 96), h = MulDiv(880, sys, 96);
    ACCEL a[] = {{FVIRTKEY, VK_F5, IDM_RECHECK}, {FVIRTKEY | FCONTROL, 'O', IDM_ADD}, {FVIRTKEY | FCONTROL, '1', IDM_PAGE0},
                 {FVIRTKEY | FCONTROL, '2', IDM_PAGE0 + 1}, {FVIRTKEY | FCONTROL, '3', IDM_PAGE0 + 2}, {FVIRTKEY | FCONTROL, '4', IDM_PAGE0 + 3},
                 {FVIRTKEY | FCONTROL, VK_UP, IDM_PREV_GAME}, {FVIRTKEY | FCONTROL, VK_DOWN, IDM_NEXT_GAME}, {FVIRTKEY | FALT, VK_LEFT, IDM_BACK},
                 {FVIRTKEY, VK_BROWSER_BACK, IDM_BACK}};
    HACCEL acc = CreateAcceleratorTableW(a, static_cast<int>(sizeof a / sizeof a[0]));
    CreateWindowExW(0, kClass, kAppName, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, w, h, nullptr, nullptr, inst, nullptr);
    if (!wnd_) return 1;
    ShowWindow(wnd_, show);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (TranslateAcceleratorW(wnd_, acc, &msg)) continue;
        if (IsDialogMessageW(wnd_, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    games_.clear();
    gp::GdiplusShutdown(gdiplus_);
    return 0;
}

}  // namespace

// A crash inside a window procedure ends the process with 0xC000041D and no report: write where it happened next to the exe.
LONG WINAPI crash_note(EXCEPTION_POINTERS* e) {
    const DWORD code = e->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_STACK_OVERFLOW && code != EXCEPTION_ILLEGAL_INSTRUCTION) return EXCEPTION_CONTINUE_SEARCH;
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    char b[160];
    std::snprintf(b, sizeof b, "exception 0x%08lx at exe+0x%llx\n", code,
                  static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(e->ExceptionRecord->ExceptionAddress) - base));
    if (FILE* f = _wfopen(join(exe_dir(), L"game_setup_crash.txt").c_str(), L"ab")) { std::fputs(b, f); std::fclose(f); }
    return EXCEPTION_CONTINUE_SEARCH;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    AddVectoredExceptionHandler(1, crash_note);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS | ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES};
    InitCommonControlsEx(&icc);
    App app;
    g_app = &app;
    const int rc = app.run(inst, show);
    g_app = nullptr;
    CoUninitialize();
    return rc;
}
