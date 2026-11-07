// SPDX-License-Identifier: GPL-3.0-or-later
// Initial Setup: the first program the player starts (<id>_setup.exe, named in the game manifest). Plain Win32 with common controls v6:
// a menu bar, four pages (Requirements, Game files, Prepare, Play) and native dialogs. It checks what the PC needs and says how to get what
// is missing (it installs nothing and changes no system setting), extracts the player's dump, prepares the shaders with progress, cancel and
// resume, and starts the game. The preparation steps are the existing tools, run as child processes:
//   extract    python nup_extract.py <nup> <data>\dump                                    (tools/extract)
//   corpus     python extract_generic.py --romfs --exefs --out <data>\shader_cache\corpus  (tools/shaders/scripts)
//   translate  <id>.exe --translate-shaders                                                 (the "Preparing shaders" window's step 1, headless)
//   programs   python bnsh_programs.py --romfs --out <data>\shader_cache\programs.bin
//   prepare    <id>.exe --prepare-shaders --if-needed                                       (pipeline parts for this GPU and driver)
// <data>\setup_state.ini records which game and translator each step ran for; the prepare step is judged by the game exe itself
// (--prepare-shaders --check: GPU, driver, pipeline cache UUID, new translations). A change means that step (and the ones after it) runs again.
//
//   <id>_setup.exe [--data DIR] [--source PATH] [--page N] [--start] [--game-args "<text>"]
// --data / --source set the data folder and the dump (saved like a choice in the window); --page opens a page (0-3); --start runs the
// preparation at once; --game-args adds text to the game's command line when Play starts it (test runs: --instance NAME --max-seconds N).
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <uxtheme.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "game_manifest.h"
#include "setup_core.h"

namespace fs = std::filesystem;
using namespace setup;

namespace {

constexpr wchar_t kClass[] = L"NxrtInitialSetup";
constexpr UINT kMsgLine = WM_APP + 1;     // lParam: std::string* (one output line of the running step)
constexpr UINT kMsgExit = WM_APP + 2;     // wParam: exit code of the running step
constexpr UINT kMsgCheck = WM_APP + 3;    // lParam: std::string* (the prepare check's answer line)
constexpr UINT kMsgReqs = WM_APP + 4;     // lParam: std::vector<Check>* (requirements checked on a thread)
constexpr UINT_PTR kTimer = 1;

enum Page { P_REQ, P_GAME, P_PREP, P_PLAY, P_COUNT };
enum StepId { S_EXTRACT, S_CORPUS, S_TRANSLATE, S_PROGRAMS, S_PREPARE, S_COUNT };
const wchar_t* kStepName[S_COUNT] = {L"Extract the game files", L"Find the game's shaders", L"Translate the shaders",
                                     L"List the shader programs", L"Compile the shaders for your graphics card"};

enum Id : int {
    ID_NAV = 100, ID_REQ_LIST, ID_REQ_FIX, ID_REQ_RECHECK, ID_REQ_COPY,
    ID_GAME_SRC, ID_GAME_NUP, ID_GAME_FOLDER, ID_GAME_INFO, ID_GAME_DATA, ID_GAME_DATA_CHANGE, ID_GAME_DATA_INFO,
    ID_PREP_LIST, ID_PREP_BAR, ID_PREP_TEXT, ID_PREP_START, ID_PREP_CANCEL, ID_PREP_REBUILD, ID_PREP_LOG,
    ID_PLAY_STATE, ID_PLAY_GO, ID_PLAY_NOTE, ID_PLAY_CLOSE, ID_PLAY_SHORTCUT,
    // menu
    IDM_CHOOSE_NUP = 1000, IDM_CHOOSE_FOLDER, IDM_DATA, IDM_OPEN_DATA, IDM_EXIT, IDM_RECHECK, IDM_START, IDM_REBUILD, IDM_SHORTCUT, IDM_PLAY,
    IDM_ABOUT, IDM_PAGE0,
};

struct Ctl {
    HWND h;
    Page page;
};

class App {
public:
    int run(HINSTANCE inst, int show);

private:
    static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l);
    LRESULT on(UINT m, WPARAM w, LPARAM l);
    void create_controls();
    void make_fonts();
    void layout();
    int S(int v) const { return MulDiv(v, dpi_, 96); }
    HWND add(Page p, const wchar_t* cls, const wchar_t* text, DWORD style, int id, HFONT font = nullptr, DWORD ex = 0);
    void show_page(int p);
    void command(int id, int code);
    LRESULT notify(NMHDR* n);
    void draw_nav(const DRAWITEMSTRUCT* d);

    // state
    void evaluate();                 // what each step needs, from the files and setup_state.ini (cheap)
    void refresh();                  // every page's texts from the evaluation
    void start_requirements();       // the checks on a thread (Vulkan, Python take a moment)
    void start_prepare_check();      // <exe> --prepare-shaders --check, when nothing else runs
    void choose_nup();
    void choose_folder(bool data);
    void set_source(const std::wstring& s);
    void rebuild_dialog();
    void play();
    void make_shortcut();
    void about();

    // steps
    void start_steps(bool force_prepare);
    void next_step();
    void step_line(const std::string& l);
    void step_exit(DWORD code);
    void cancel_steps();
    void log(const std::wstring& s);
    bool write_game_ini();
    std::wstring game_cmd(const std::wstring& extra) const;   // --game <folder> --ini <ini> --log ...
    std::wstring py_args(const std::wstring& script, const std::wstring& rest) const;

    HINSTANCE inst_ = nullptr;
    HWND wnd_ = nullptr, nav_ = nullptr, status_ = nullptr;
    int dpi_ = 96;
    HFONT font_ = nullptr, bold_ = nullptr, title_ = nullptr;
    std::vector<Ctl> ctls_;
    int page_ = P_REQ;
    HWND h1_[P_COUNT] = {}, hint_[P_COUNT] = {};
    HWND req_list_ = nullptr, req_fix_ = nullptr, req_fix_label_ = nullptr, req_recheck_ = nullptr, req_copy_ = nullptr;
    HWND game_src_label_ = nullptr, game_src_ = nullptr, game_nup_ = nullptr, game_folder_ = nullptr, game_info_ = nullptr;
    HWND game_data_label_ = nullptr, game_data_ = nullptr, game_data_change_ = nullptr, game_data_info_ = nullptr, game_nsp_ = nullptr;
    HWND prep_list_ = nullptr, prep_bar_ = nullptr, prep_text_ = nullptr, prep_start_ = nullptr, prep_cancel_ = nullptr, prep_rebuild_ = nullptr;
    HWND prep_log_label_ = nullptr, prep_log_ = nullptr;
    HWND play_state_ = nullptr, play_go_ = nullptr, play_note_ = nullptr, play_close_ = nullptr, play_shortcut_ = nullptr;

    Tools tools_;
    Config cfg_;
    Layout lay_;
    State st_;
    GameId gid_;
    std::vector<Check> checks_;
    bool checks_running_ = false;
    bool need_[S_COUNT] = {};
    std::wstring why_[S_COUNT];
    bool retranslate_all_ = false;       // a new translator: the old translations go
    std::wstring prep_check_;            // the last --check answer ("ready", "needed ...", "error ..."); empty: not known
    bool checking_prep_ = false;
    Child check_child_;

    Child child_;
    bool running_ = false;
    bool cancel_requested_ = false;
    bool force_prepare_ = false;
    int cur_ = -1;                        // the step running
    uint64_t done_ = 0, total_ = 0;
    std::chrono::steady_clock::time_point step_t0_;
    std::string step_error_;
    bool step_result_ok_ = false;
    std::deque<std::wstring> log_lines_;
    std::wstring game_args_;
    bool auto_start_ = false;
};

App* g_app = nullptr;

std::wstring step_state_text(bool need, const std::wstring& why) { return need ? (why.empty() ? L"To do" : L"To do: " + why) : (why.empty() ? L"Done" : why); }

// ---- creation and layout ------------------------------------------------------------------------------------------------------------
void App::make_fonts() {
    for (HFONT* f : {&font_, &bold_, &title_})
        if (*f) { DeleteObject(*f); *f = nullptr; }
    NONCLIENTMETRICSW ncm{sizeof ncm};
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, static_cast<UINT>(dpi_));
    LOGFONTW lf = ncm.lfMessageFont;
    font_ = CreateFontIndirectW(&lf);
    lf.lfWeight = FW_SEMIBOLD;
    bold_ = CreateFontIndirectW(&lf);
    lf.lfHeight = -S(22);
    title_ = CreateFontIndirectW(&lf);
}

HWND App::add(Page p, const wchar_t* cls, const wchar_t* text, DWORD style, int id, HFONT font, DWORD ex) {
    HWND h = CreateWindowExW(ex, cls, text, WS_CHILD | style, 0, 0, 10, 10, wnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), inst_, nullptr);
    SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(font ? font : font_), TRUE);
    ctls_.push_back({h, p});
    return h;
}

void App::create_controls() {
    nav_ = CreateWindowExW(0, WC_LISTBOXW, L"", WS_CHILD | WS_VISIBLE | LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_NOINTEGRALHEIGHT, 0, 0, 10, 10, wnd_,
                           reinterpret_cast<HMENU>(ID_NAV), inst_, nullptr);
    for (int i = 0; i < P_COUNT; ++i) SendMessageW(nav_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L""));
    status_ = CreateWindowExW(0, STATUSCLASSNAMEW, L"", WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, 0, 0, 0, 0, wnd_, nullptr, inst_, nullptr);

    const wchar_t* titles[P_COUNT] = {L"Requirements", L"Game files", L"Prepare the game", L"Play"};
    const wchar_t* hints[P_COUNT] = {
        L"What " GAME_SHORT_TITLE L" needs on this PC. The setup only checks: it installs nothing and changes no system setting. "
        L"Select an item to see what to do.",
        L"Use your own dump of " GAME_TITLE L", version " GAME_DUMP_VERSION L". This setup never downloads game files and never changes your dump.",
        L"These steps run once, about one to two hours in all; most of it is compiling the shaders for your graphics card, so the game later "
        L"runs without stutter. Cancel stops at any time and Start continues where it stopped.",
        L"Start the game from here."};
    for (int p = 0; p < P_COUNT; ++p) {
        h1_[p] = add(static_cast<Page>(p), WC_STATICW, titles[p], SS_LEFT | SS_NOPREFIX, 0, title_);
        hint_[p] = add(static_cast<Page>(p), WC_STATICW, hints[p], SS_LEFT | SS_NOPREFIX, 0);
    }
    // Requirements
    req_list_ = add(P_REQ, WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_BORDER | WS_TABSTOP, ID_REQ_LIST);
    ListView_SetExtendedListViewStyle(req_list_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    SetWindowTheme(req_list_, L"Explorer", nullptr);
    req_fix_label_ = add(P_REQ, WC_STATICW, L"What to do", SS_LEFT, 0, bold_);
    req_fix_ = add(P_REQ, WC_EDITW, L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_BORDER | WS_TABSTOP, ID_REQ_FIX);
    req_recheck_ = add(P_REQ, WC_BUTTONW, L"Check again", BS_PUSHBUTTON | WS_TABSTOP, ID_REQ_RECHECK);
    req_copy_ = add(P_REQ, WC_BUTTONW, L"Copy steps", BS_PUSHBUTTON | WS_TABSTOP, ID_REQ_COPY);
    // Game files
    game_src_label_ = add(P_GAME, WC_STATICW, L"Your dump", SS_LEFT, 0, bold_);
    game_src_ = add(P_GAME, WC_EDITW, L"", ES_READONLY | ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, ID_GAME_SRC);
    game_nup_ = add(P_GAME, WC_BUTTONW, L"Choose a .nup file...", BS_PUSHBUTTON | WS_TABSTOP, ID_GAME_NUP);
    game_folder_ = add(P_GAME, WC_BUTTONW, L"Choose a folder...", BS_PUSHBUTTON | WS_TABSTOP, ID_GAME_FOLDER);
    game_info_ = add(P_GAME, WC_STATICW, L"", SS_LEFT | SS_NOPREFIX, ID_GAME_INFO);
    game_nsp_ = add(P_GAME, WC_STATICW,
                    L"Accepted: a .nup package, or a folder with exefs\\ and romfs\\ (also with updates\\<version>\\ or base\\ inside). "
                    L"NSP and XCI files are not supported by this build yet.",
                    SS_LEFT | SS_NOPREFIX, 0);
    game_data_label_ = add(P_GAME, WC_STATICW, L"Data folder (the setup writes here)", SS_LEFT, 0, bold_);
    game_data_ = add(P_GAME, WC_EDITW, L"", ES_READONLY | ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, ID_GAME_DATA);
    game_data_change_ = add(P_GAME, WC_BUTTONW, L"Change...", BS_PUSHBUTTON | WS_TABSTOP, ID_GAME_DATA_CHANGE);
    game_data_info_ = add(P_GAME, WC_STATICW, L"", SS_LEFT | SS_NOPREFIX, ID_GAME_DATA_INFO);
    // Prepare
    prep_list_ = add(P_PREP, WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_NOSORTHEADER | WS_BORDER, ID_PREP_LIST);
    ListView_SetExtendedListViewStyle(prep_list_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    SetWindowTheme(prep_list_, L"Explorer", nullptr);
    prep_bar_ = add(P_PREP, PROGRESS_CLASSW, L"", PBS_SMOOTH, ID_PREP_BAR);
    SendMessageW(prep_bar_, PBM_SETRANGE32, 0, 1000);
    prep_text_ = add(P_PREP, WC_STATICW, L"", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, ID_PREP_TEXT);
    prep_start_ = add(P_PREP, WC_BUTTONW, L"Start", BS_DEFPUSHBUTTON | WS_TABSTOP, ID_PREP_START);
    prep_cancel_ = add(P_PREP, WC_BUTTONW, L"Cancel", BS_PUSHBUTTON | WS_TABSTOP, ID_PREP_CANCEL);
    prep_rebuild_ = add(P_PREP, WC_BUTTONW, L"Rebuild shaders...", BS_PUSHBUTTON | WS_TABSTOP, ID_PREP_REBUILD);
    prep_log_label_ = add(P_PREP, WC_STATICW, L"Output", SS_LEFT, 0, bold_);
    prep_log_ = add(P_PREP, WC_EDITW, L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_BORDER, ID_PREP_LOG);
    // Play
    play_state_ = add(P_PLAY, WC_STATICW, L"", SS_LEFT | SS_NOPREFIX, ID_PLAY_STATE, bold_);
    play_go_ = add(P_PLAY, WC_BUTTONW, L"Play " GAME_SHORT_TITLE, BS_COMMANDLINK | WS_TABSTOP, ID_PLAY_GO);
    SendMessageW(play_go_, BCM_SETNOTE, 0, reinterpret_cast<LPARAM>(L"Starts " GAME_EXE L" with your game files and the prepared shaders."));
    play_note_ = add(P_PLAY, WC_STATICW, L"", SS_LEFT | SS_NOPREFIX, ID_PLAY_NOTE);
    play_close_ = add(P_PLAY, WC_BUTTONW, L"Close the setup when the game starts", BS_AUTOCHECKBOX | WS_TABSTOP, ID_PLAY_CLOSE);
    SendMessageW(play_close_, BM_SETCHECK, BST_CHECKED, 0);
    play_shortcut_ = add(P_PLAY, WC_BUTTONW, L"Create a desktop shortcut", BS_PUSHBUTTON | WS_TABSTOP, ID_PLAY_SHORTCUT);

    auto col = [](HWND lv, int i, const wchar_t* t, int w) {
        LVCOLUMNW c{LVCF_TEXT | LVCF_WIDTH};
        c.pszText = const_cast<wchar_t*>(t);
        c.cx = w;
        ListView_InsertColumn(lv, i, &c);
    };
    col(req_list_, 0, L"Item", S(200));
    col(req_list_, 1, L"State", S(120));
    col(req_list_, 2, L"Details", S(400));
    col(prep_list_, 0, L"Step", S(300));
    col(prep_list_, 1, L"State", S(380));
    for (int i = 0; i < S_COUNT; ++i) {
        LVITEMW it{LVIF_TEXT};
        it.iItem = i;
        it.pszText = const_cast<wchar_t*>(kStepName[i]);
        ListView_InsertItem(prep_list_, &it);
    }
}

void App::layout() {
    RECT rc;
    GetClientRect(wnd_, &rc);
    SendMessageW(status_, WM_SIZE, 0, 0);
    RECT sr;
    GetWindowRect(status_, &sr);
    const int sh = sr.bottom - sr.top;
    const int nav_w = S(200);
    const int bottom = rc.bottom - sh;
    MoveWindow(nav_, 0, 0, nav_w, bottom, TRUE);
    SendMessageW(nav_, LB_SETITEMHEIGHT, 0, S(54));
    const int x = nav_w + S(24), w = rc.right - x - S(24), btn_h = S(28), btn_w = S(130);
    auto put = [](HWND h, int x0, int y0, int w0, int h0) { MoveWindow(h, x0, y0, std::max(w0, 1), std::max(h0, 1), TRUE); };
    for (int p = 0; p < P_COUNT; ++p) {
        put(h1_[p], x, S(16), w, S(34));
        put(hint_[p], x, S(54), w, S(48));
    }
    int y = S(108);
    // Requirements
    const int list_h = std::max(S(120), (bottom - y - S(220)));
    put(req_list_, x, y, w, list_h);
    ListView_SetColumnWidth(req_list_, 2, std::max(S(120), w - S(200) - S(120) - S(24)));
    int y2 = y + list_h + S(12);
    put(req_fix_label_, x, y2, w, S(20));
    put(req_fix_, x, y2 + S(22), w, bottom - (y2 + S(22)) - btn_h - S(24));
    put(req_recheck_, x, bottom - btn_h - S(12), btn_w, btn_h);
    put(req_copy_, x + btn_w + S(8), bottom - btn_h - S(12), btn_w, btn_h);
    // Game files
    y2 = y;
    put(game_src_label_, x, y2, w, S(20));
    y2 += S(24);
    put(game_src_, x, y2, w, S(24));
    y2 += S(32);
    put(game_nup_, x, y2, S(170), btn_h);
    put(game_folder_, x + S(178), y2, S(170), btn_h);
    y2 += btn_h + S(10);
    put(game_info_, x, y2, w, S(64));
    y2 += S(70);
    put(game_nsp_, x, y2, w, S(36));
    y2 += S(56);
    put(game_data_label_, x, y2, w, S(20));
    y2 += S(24);
    put(game_data_, x, y2, w - S(108), S(24));
    put(game_data_change_, x + w - S(100), y2 - S(2), S(100), btn_h);
    y2 += S(32);
    put(game_data_info_, x, y2, w, S(48));
    // Prepare
    y2 = y;
    const int steps_h = S(30) + S_COUNT * S(22);
    put(prep_list_, x, y2, w, steps_h);
    ListView_SetColumnWidth(prep_list_, 1, std::max(S(120), w - S(300) - S(8)));
    y2 += steps_h + S(12);
    put(prep_bar_, x, y2, w, S(18));
    y2 += S(24);
    put(prep_text_, x, y2, w, S(20));
    y2 += S(28);
    put(prep_start_, x, y2, btn_w, btn_h);
    put(prep_cancel_, x + btn_w + S(8), y2, btn_w, btn_h);
    put(prep_rebuild_, x + w - S(160), y2, S(160), btn_h);
    y2 += btn_h + S(14);
    put(prep_log_label_, x, y2, w, S(20));
    put(prep_log_, x, y2 + S(22), w, bottom - (y2 + S(22)) - S(12));
    // Play
    y2 = y;
    put(play_state_, x, y2, w, S(40));
    y2 += S(48);
    put(play_go_, x, y2, std::min(w, S(420)), S(64));
    y2 += S(76);
    put(play_note_, x, y2, w, S(96));
    y2 += S(104);
    put(play_close_, x, y2, w, S(22));
    y2 += S(34);
    put(play_shortcut_, x, y2, S(200), btn_h);
    InvalidateRect(wnd_, nullptr, TRUE);
}

void App::show_page(int p) {
    page_ = std::clamp(p, 0, P_COUNT - 1);
    SendMessageW(nav_, LB_SETCURSEL, static_cast<WPARAM>(page_), 0);
    for (const Ctl& c : ctls_) ShowWindow(c.h, c.page == page_ ? SW_SHOW : SW_HIDE);
    InvalidateRect(nav_, nullptr, TRUE);
}

// ---- evaluation ------------------------------------------------------------------------------------------------------------------
void App::evaluate() {
    lay_ = layout_of(cfg_);
    st_ = load_state(lay_.state_file);
    gid_ = game_id_of(lay_.game);
    for (int i = 0; i < S_COUNT; ++i) { need_[i] = false; why_[i].clear(); }
    const SourceKind sk = source_kind(cfg_.source);
    const std::string gid = gid_.ok ? gid_.text() : std::string();
    // extract
    if (sk == SourceKind::nup) {
        if (lay_.game.empty()) need_[S_EXTRACT] = true;
        else if (st_.extracted_from != nup_stamp(cfg_.source)) { need_[S_EXTRACT] = true; why_[S_EXTRACT] = L"another .nup file was chosen"; }
    } else if (sk == SourceKind::folder) {
        why_[S_EXTRACT] = lay_.game.empty() ? L"the folder has no exefs\\main and romfs\\" : L"Not needed: an extracted folder";
    } else {
        need_[S_EXTRACT] = true;
        why_[S_EXTRACT] = L"choose your dump first (Game files)";
    }
    const bool game_known = gid_.ok && !need_[S_EXTRACT];
    // corpus, translate, programs: done for this game (and translator)
    auto later = [&](int s, const std::wstring& w) { need_[s] = true; if (why_[s].empty()) why_[s] = w; };
    if (!game_known) {
        for (int s : {S_CORPUS, S_TRANSLATE, S_PROGRAMS, S_PREPARE}) later(s, L"after the game files");
    } else {
        if (st_.corpus_game != gid || !file_exists(join(lay_.corpus, L"unique.tsv")))
            later(S_CORPUS, st_.corpus_game.empty() ? L"" : L"the game changed (an update)");
        const std::string ts = translator_stamp(tools_.translator);
        retranslate_all_ = !st_.translated_with.empty() && st_.translated_with != ts && file_exists(join(lay_.spirv, L"results.tsv"));
        if (retranslate_all_) later(S_TRANSLATE, L"a new translator: every shader again");
        else if (need_[S_CORPUS] || st_.translated_game != gid || st_.translated_with != ts)
            later(S_TRANSLATE, st_.translated_game.empty() || need_[S_CORPUS] ? L"" : L"new shaders of the game update");
        if (st_.programs_game != gid || !file_exists(lay_.programs)) later(S_PROGRAMS, st_.programs_game.empty() ? L"" : L"the game changed");
        if (need_[S_CORPUS] || need_[S_TRANSLATE] || need_[S_PROGRAMS]) later(S_PREPARE, L"after the steps above");
        else if (prep_check_ == L"ready") { why_[S_PREPARE] = L"Done for this graphics card and driver"; }
        else if (prep_check_.rfind(L"needed", 0) == 0) {
            std::wstring w = prep_check_.size() > 7 ? prep_check_.substr(7) : L"";
            if (w.find(L"another GPU") != std::wstring::npos) w = L"your graphics card or driver changed";
            later(S_PREPARE, w);
        } else if (prep_check_.rfind(L"error", 0) == 0) later(S_PREPARE, L"cannot check: " + prep_check_.substr(std::min<size_t>(6, prep_check_.size())));
        else later(S_PREPARE, checking_prep_ ? L"checking..." : L"");
    }
}

bool ready_to_play(const bool need[S_COUNT]) {
    for (int i = 0; i < S_COUNT; ++i) if (need[i]) return false;
    return true;
}

void App::refresh() {
    // nav marks are drawn from the same data (draw_nav)
    InvalidateRect(nav_, nullptr, TRUE);
    // requirements
    ListView_DeleteAllItems(req_list_);
    std::wstring all_fix;
    for (size_t i = 0; i < checks_.size(); ++i) {
        const Check& c = checks_[i];
        LVITEMW it{LVIF_TEXT | LVIF_PARAM};
        it.iItem = static_cast<int>(i);
        it.pszText = const_cast<wchar_t*>(c.item.c_str());
        it.lParam = static_cast<LPARAM>(c.level);
        ListView_InsertItem(req_list_, &it);
        ListView_SetItemText(req_list_, static_cast<int>(i), 1, const_cast<wchar_t*>(c.state.c_str()));
        ListView_SetItemText(req_list_, static_cast<int>(i), 2, const_cast<wchar_t*>(c.detail.c_str()));
        if (!c.fix.empty()) all_fix += c.item + L":\r\n" + c.fix + L"\r\n\r\n";
    }
    if (checks_running_ && checks_.empty()) {
        LVITEMW it{LVIF_TEXT};
        it.pszText = const_cast<wchar_t*>(L"Checking...");
        ListView_InsertItem(req_list_, &it);
    }
    if (!checks_.empty()) SetWindowTextW(req_fix_, all_fix.empty() ? L"Nothing to do: this PC has everything the game needs." : all_fix.c_str());
    // game files
    SetWindowTextW(game_src_, cfg_.source.c_str());
    SetWindowTextW(game_data_, cfg_.data_dir.c_str());
    std::wstring info;
    switch (source_kind(cfg_.source)) {
        case SourceKind::none: info = L"No dump chosen yet."; break;
        case SourceKind::missing: info = L"Not found: " + cfg_.source; break;
        case SourceKind::unsupported: info = L"This file type is not supported by this build (NSP, XCI). Use a .nup package or an extracted folder."; break;
        case SourceKind::nup: info = L"A .nup package. "; [[fallthrough]];
        case SourceKind::folder:
            if (gid_.ok) {
                info += L"Game folder: " + lay_.game + L"\r\nmain build id " + widen(gid_.build_id) +
                        (*GAME_MAIN_BUILD_ID && _stricmp(gid_.build_id.c_str(), GAME_MAIN_BUILD_ID) == 0 ? L": version " GAME_DUMP_VERSION L", the one this release is made for"
                                                                                                         : L" (not " GAME_DUMP_VERSION L": the game may not start)");
            } else if (source_kind(cfg_.source) == SourceKind::nup) {
                info += L"It is extracted into the data folder in the first step (Prepare).";
            } else {
                info = L"This folder has no exefs\\main and romfs\\ (also looked in updates\\<version>\\ and base\\).";
            }
            break;
    }
    SetWindowTextW(game_info_, info.c_str());
    uint64_t need = remaining_disk_need(cfg_, st_);
    ULARGE_INTEGER avail{};
    std::wstring d = cfg_.data_dir;
    while (!d.empty() && !dir_exists(d)) d = parent_of(d);
    GetDiskFreeSpaceExW(d.c_str(), &avail, nullptr, nullptr);
    SetWindowTextW(game_data_info_, (L"Free there: " + gib(avail.QuadPart) + L". The remaining steps write about " + gib(need) +
                                     L" (the extracted game about 26 GB, the shader data about 10 GB).").c_str());
    // prepare
    for (int i = 0; i < S_COUNT; ++i) {
        std::wstring t = (running_ && i == cur_) ? L"Running..." : step_state_text(need_[i], why_[i]);
        ListView_SetItemText(prep_list_, i, 1, const_cast<wchar_t*>(t.c_str()));
    }
    bool partly = false;
    for (int i = 0; i < S_COUNT; ++i) partly |= !need_[i] && i != S_EXTRACT;
    SetWindowTextW(prep_start_, ready_to_play(need_) ? L"Check again" : partly ? L"Continue" : L"Start");
    EnableWindow(prep_start_, !running_);
    EnableWindow(prep_cancel_, running_);
    EnableWindow(prep_rebuild_, !running_ && gid_.ok);
    if (!running_) {
        std::wstring t = ready_to_play(need_) ? L"Everything is prepared." : L"Press Start to run the steps marked \"To do\".";
        if (!step_error_.empty()) t = L"Stopped: " + widen(step_error_);
        else if (cancel_requested_ && !ready_to_play(need_)) t = L"Cancelled. Start continues where it stopped.";
        SetWindowTextW(prep_text_, t.c_str());
    }
    // play
    const bool ok = ready_to_play(need_);
    bool req_bad = false;
    for (const Check& c : checks_) req_bad |= c.level == Level::bad;
    std::wstring ps = running_ ? L"The preparation is running (Prepare page)." : ok ? L"Ready to play." : L"Not ready yet: finish the steps on the Prepare page.";
    SetWindowTextW(play_state_, ps.c_str());
    EnableWindow(play_go_, !running_ && gid_.ok && !need_[S_EXTRACT]);
    std::wstring note;
    if (!ok && gid_.ok && !need_[S_EXTRACT] && !running_)
        note = L"You can play before the shaders are ready, but the game then translates and compiles them while it runs: expect black objects "
               L"and long stutters at first.";
    if (need_[S_PREPARE] && !need_[S_CORPUS] && !need_[S_TRANSLATE] && !need_[S_PROGRAMS])
        note = L"The shaders must be compiled again (" + why_[S_PREPARE] + L"). Press Start on the Prepare page.";
    if (req_bad) note += (note.empty() ? L"" : L"\r\n\r\n") + std::wstring(L"Some requirements are missing (Requirements page).");
    note += (note.empty() ? L"" : L"\r\n\r\n") + std::wstring(L"Settings: the game window has a menu bar (Game, Input, Graphics, Audio). Your saves stay in %APPDATA%\\" GAME_SAVE_FOLDER L".");
    SetWindowTextW(play_note_, note.c_str());
    // status bar
    std::wstring sb = L"Data folder: " + cfg_.data_dir;
    SendMessageW(status_, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(sb.c_str()));
    // menu
    HMENU m = GetMenu(wnd_);
    EnableMenuItem(m, IDM_START, MF_BYCOMMAND | (running_ ? MF_GRAYED : MF_ENABLED));
    EnableMenuItem(m, IDM_REBUILD, MF_BYCOMMAND | (running_ || !gid_.ok ? MF_GRAYED : MF_ENABLED));
    EnableMenuItem(m, IDM_PLAY, MF_BYCOMMAND | (IsWindowEnabled(play_go_) ? MF_ENABLED : MF_GRAYED));
}

void App::start_requirements() {
    if (checks_running_) return;
    checks_running_ = true;
    const Tools t = tools_;
    const Config c = cfg_;
    const uint64_t need = remaining_disk_need(cfg_, st_);
    HWND w = wnd_;
    std::thread([t, c, need, w] {
        auto* r = new std::vector<Check>(run_checks(t, c, need));
        if (!PostMessageW(w, kMsgReqs, 0, reinterpret_cast<LPARAM>(r))) delete r;
    }).detach();
    refresh();
}

void App::start_prepare_check() {
    if (running_ || checking_prep_ || !gid_.ok || need_[S_CORPUS] || need_[S_TRANSLATE] || need_[S_PROGRAMS] || !file_exists(tools_.game_exe)) return;
    if (!write_game_ini()) return;
    checking_prep_ = true;
    HWND w = wnd_;
    auto got = std::make_shared<std::string>();
    std::wstring err;
    const bool ok = check_child_.start(tools_.game_exe, game_cmd(L"--prepare-shaders --check"), lay_.data, false,
        [got](const std::string& l) { if (l.rfind("prepare: ", 0) == 0 && l.find("prepare: error") != 0) *got = l.substr(9); else if (l.rfind("prepare: error", 0) == 0) *got = l.substr(9); },
        [got, w](DWORD code) {
            std::string a = *got;
            if (a.empty()) a = code == 7 ? "error the game is running from this folder" : "error the check ended with code " + std::to_string(code);
            PostMessageW(w, kMsgCheck, 0, reinterpret_cast<LPARAM>(new std::string(a)));
        }, &err);
    if (!ok) { checking_prep_ = false; prep_check_ = L"error " + err; }
    evaluate();
    refresh();
}

// ---- steps ---------------------------------------------------------------------------------------------------------------------------
std::wstring App::py_args(const std::wstring& script, const std::wstring& rest) const {
    return (tools_.python_args.empty() ? L"" : tools_.python_args + L" ") + L"-I " + quote(script) + L" " + rest;
}

std::wstring App::game_cmd(const std::wstring& extra) const {
    return L"--game " + quote(lay_.game) + L" --ini " + quote(lay_.game_ini) + L" --log " + quote(join(lay_.data, L"logs\\setup_" GAME_LOG)) + L" " + extra;
}

bool App::write_game_ini() {
    std::error_code ec;
    fs::create_directories(fs::path(lay_.data) / L"logs", ec);
    auto fwd = [](std::wstring p) { for (wchar_t& c : p) if (c == L'\\') c = L'/'; return narrow(p); };
    std::string t = "; Written by " GAME_SETUP_EXE ": where the game finds the shader data this setup prepared.\n[nvn]\n";
    t += "shader_cache = " + fwd(lay_.spirv) + "\n";
    t += "shader_corpus = " + fwd(lay_.corpus) + "\n";
    if (!tools_.translator.empty()) t += "shader_translator = " + fwd(tools_.translator) + "\n";
    // keep anything the player added below the setup's keys
    std::string old;
    {
        FILE* f = _wfopen(lay_.game_ini.c_str(), L"rb");
        if (f) {
            char b[4096];
            size_t n;
            while ((n = std::fread(b, 1, sizeof b, f)) > 0) old.append(b, n);
            std::fclose(f);
        }
    }
    const size_t mark = old.find("\n; --- your settings below ---");
    t += mark == std::string::npos ? std::string("\n; --- your settings below ---\n") : old.substr(mark);
    fs::create_directories(fs::path(lay_.game_ini).parent_path(), ec);
    FILE* f = _wfopen(lay_.game_ini.c_str(), L"wb");
    if (!f) return false;
    std::fwrite(t.data(), 1, t.size(), f);
    std::fclose(f);
    return true;
}

void App::log(const std::wstring& s) {
    log_lines_.push_back(s);
    while (log_lines_.size() > 300) log_lines_.pop_front();
    std::wstring all;
    for (const std::wstring& l : log_lines_) all += l + L"\r\n";
    SetWindowTextW(prep_log_, all.c_str());
    SendMessageW(prep_log_, EM_SETSEL, all.size(), all.size());
    SendMessageW(prep_log_, EM_SCROLLCARET, 0, 0);
}

void App::start_steps(bool force_prepare) {
    if (running_) return;
    if (check_child_.running()) check_child_.wait();
    step_error_.clear();
    cancel_requested_ = false;
    force_prepare_ = force_prepare;
    evaluate();
    bool req_bad = false;
    for (const Check& c : checks_) req_bad |= c.level == Level::bad && c.item != L"Your game dump";
    if (req_bad && MessageBoxW(wnd_, L"Some requirements are missing (Requirements page). Start anyway?", L"" GAME_PRODUCT L" setup",
                               MB_YESNO | MB_ICONWARNING) != IDYES)
        return;
    const SourceKind sk = source_kind(cfg_.source);
    if (sk != SourceKind::nup && sk != SourceKind::folder) {
        MessageBoxW(wnd_, L"Choose your game dump first (Game files page).", L"" GAME_PRODUCT L" setup", MB_OK | MB_ICONINFORMATION);
        show_page(P_GAME);
        return;
    }
    running_ = true;
    cur_ = -1;
    show_page(P_PREP);
    log(L"--- " + std::wstring(force_prepare ? L"rebuild" : L"start") + L" ---");
    next_step();
}

void App::next_step() {
    evaluate();
    int s = -1;
    for (int i = cur_ + 1; i < S_COUNT; ++i)
        if (need_[i] || (i == S_PREPARE && force_prepare_)) { s = i; break; }
    if (cur_ == -1 && s == -1) s = -1;
    if (s < 0) {
        running_ = false;
        cur_ = -1;
        log(L"--- finished ---");
        prep_check_.clear();
        evaluate();
        refresh();
        start_prepare_check();
        if (ready_to_play(need_)) show_page(P_PLAY);
        return;
    }
    // a step that cannot run yet: stop
    if ((s != S_EXTRACT && !gid_.ok) || (s == S_EXTRACT && source_kind(cfg_.source) != SourceKind::nup)) {
        running_ = false;
        step_error_ = s == S_EXTRACT ? "the dump folder is not a game folder" : "the game files are not there";
        cur_ = -1;
        refresh();
        return;
    }
    cur_ = s;
    done_ = total_ = 0;
    step_t0_ = std::chrono::steady_clock::now();
    step_result_ok_ = false;
    SendMessageW(prep_bar_, PBM_SETPOS, 0, 0);
    SetWindowLongPtrW(prep_bar_, GWL_STYLE, (GetWindowLongPtrW(prep_bar_, GWL_STYLE) & ~PBS_MARQUEE) | (s == S_PROGRAMS ? PBS_MARQUEE : 0));
    SendMessageW(prep_bar_, PBM_SETMARQUEE, s == S_PROGRAMS, 30);
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    const std::wstring jobs = std::to_wstring(hw > 3 ? hw - 2 : 1);
    std::wstring exe, args;
    bool event = false;
    std::error_code ec;
    if (s == S_EXTRACT) {
        exe = tools_.python;
        args = py_args(tools_.nup_extract, quote(cfg_.source) + L" " + quote(lay_.dump));
    } else if (s == S_CORPUS) {
        fs::remove(fs::path(lay_.corpus) / L"unique.tsv", ec);   // a scan that stops before its end must not count
        fs::remove(fs::path(lay_.corpus) / L"unique_all.tsv", ec);
        fs::create_directories(lay_.corpus, ec);
        exe = tools_.python;
        args = py_args(join(tools_.shader_scripts, L"extract_generic.py"), L"--romfs " + quote(join(lay_.game, L"romfs")) + L" --exefs " +
                       quote(join(lay_.game, L"exefs")) + L" --out " + quote(lay_.corpus) + L" --jobs " + jobs + L" --progress");
    } else if (s == S_TRANSLATE) {
        if (retranslate_all_ && dir_exists(lay_.spirv)) {
            // a new translator: the old translations go (renamed at once, deleted in the background)
            const std::wstring old = lay_.spirv + L"_old_" + std::to_wstring(GetTickCount64());
            if (MoveFileW(lay_.spirv.c_str(), old.c_str())) {
                log(L"old translations moved aside, deleting them: " + old);
                std::thread([old] { std::error_code e; fs::remove_all(old, e); }).detach();
            }
            st_.translated_with.clear();
            save_state(lay_.state_file, st_);
        }
        write_game_ini();
        exe = tools_.game_exe;
        args = game_cmd(L"--translate-shaders");
        event = true;
    } else if (s == S_PROGRAMS) {
        exe = tools_.python;
        args = py_args(join(tools_.shader_scripts, L"bnsh_programs.py"), L"--romfs " + quote(join(lay_.game, L"romfs")) + L" --out " + quote(lay_.programs));
    } else {
        write_game_ini();
        exe = tools_.game_exe;
        args = game_cmd(force_prepare_ ? L"--prepare-shaders --force" : L"--prepare-shaders --if-needed");
        event = true;
    }
    if (exe.empty()) {
        running_ = false;
        step_error_ = s == S_TRANSLATE || s == S_PREPARE ? "the game program is missing" : "Python is missing (Requirements page)";
        cur_ = -1;
        refresh();
        return;
    }
    log(L"> " + std::wstring(kStepName[s]));
    fs::create_directories(lay_.data, ec);   // the children run in the data folder
    HWND w = wnd_;
    std::wstring err;
    if (!child_.start(exe, args, lay_.data, event, [w](const std::string& l) { PostMessageW(w, kMsgLine, 0, reinterpret_cast<LPARAM>(new std::string(l))); },
                      [w](DWORD c) { PostMessageW(w, kMsgExit, c, 0); }, &err)) {
        running_ = false;
        log(L"  " + err);
        step_error_ = narrow(err);
        cur_ = -1;
    }
    refresh();
}

void App::step_line(const std::string& l) {
    // progress lines: "@progress <label> i n" (nup_extract), "scan: d t" (extract_generic), "translate: d t", "prepare: d t"
    unsigned long long a = 0, b = 0;
    char label[64] = {};
    bool progress = false;
    if (std::sscanf(l.c_str(), "@progress %63s %llu %llu", label, &a, &b) == 3) progress = true;
    else if (std::sscanf(l.c_str(), "scan: %llu %llu", &a, &b) == 2) progress = true;
    else if (std::sscanf(l.c_str(), "translate: %llu %llu", &a, &b) == 2 && l.find("done") == std::string::npos) progress = true;
    else if (std::sscanf(l.c_str(), "prepare: %llu %llu", &a, &b) == 2) progress = true;
    if (progress) {
        if (label[0] && b != total_) step_t0_ = std::chrono::steady_clock::now();   // the extraction's second part (romfs after exefs)
        done_ = a;
        total_ = b;
        return;
    }
    if (l.rfind("@error ", 0) == 0 || l.rfind("translate: error ", 0) == 0 || l.rfind("prepare: error ", 0) == 0) step_error_ = l.substr(l.find(' ') + 1);
    if (l.rfind("@result", 0) == 0 || l.rfind("translate: done", 0) == 0 || l.rfind("prepare: done", 0) == 0 || l == "prepare: ready")
        step_result_ok_ = l.find("cancelled") == std::string::npos;
    log(widen(l));
}

void App::step_exit(DWORD code) {
    child_.wait();
    const int s = cur_;
    const std::string gid = gid_.text();
    // A step that finished before the cancel reached it counts (the game exe says "done" without "cancelled"; a script exits 0).
    bool ok = code == 0 && (!cancel_requested_ || step_result_ok_ || s == S_PROGRAMS || s == S_CORPUS);
    if (ok) {
        if (s == S_EXTRACT) {
            st_.extracted_from = nup_stamp(cfg_.source);
        } else if (s == S_CORPUS) {
            ok = file_exists(join(lay_.corpus, L"unique.tsv"));
            if (ok) st_.corpus_game = game_id_of(lay_.game).text();
        } else if (s == S_TRANSLATE) {
            st_.translated_game = game_id_of(lay_.game).text();
            st_.translated_with = translator_stamp(tools_.translator);
        } else if (s == S_PROGRAMS) {
            ok = file_exists(lay_.programs);
            if (ok) st_.programs_game = game_id_of(lay_.game).text();
        } else if (s == S_PREPARE) {
            prep_check_ = L"ready";
            force_prepare_ = false;
        }
        save_state(lay_.state_file, st_);
    }
    log(L"  exit code " + std::to_wstring(code));
    if (!ok || cancel_requested_) {
        running_ = false;
        cur_ = -1;
        if (!cancel_requested_ && step_error_.empty()) step_error_ = narrow(std::wstring(kStepName[s])) + " ended with code " + std::to_string(code) + " (see Output)";
        SetWindowLongPtrW(prep_bar_, GWL_STYLE, GetWindowLongPtrW(prep_bar_, GWL_STYLE) & ~PBS_MARQUEE);
        evaluate();
        refresh();
        return;
    }
    next_step();
}

void App::cancel_steps() {
    if (!running_) return;
    cancel_requested_ = true;
    SetWindowTextW(prep_text_, L"Stopping (what is done is kept)...");
    child_.cancel();
}

// ---- dialogs ---------------------------------------------------------------------------------------------------------------------
void App::set_source(const std::wstring& s) {
    cfg_.source = s;
    save_config(cfg_);
    prep_check_.clear();
    evaluate();
    start_prepare_check();
    start_requirements();
    refresh();
}

void App::choose_nup() {
    IFileOpenDialog* d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return;
    COMDLG_FILTERSPEC f[] = {{L"NUP package (*.nup)", L"*.nup"}, {L"All files", L"*.*"}};
    d->SetFileTypes(2, f);
    d->SetTitle(L"Choose your .nup package");
    if (SUCCEEDED(d->Show(wnd_))) {
        IShellItem* it = nullptr;
        if (SUCCEEDED(d->GetResult(&it))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) { set_source(p); CoTaskMemFree(p); }
            it->Release();
        }
    }
    d->Release();
}

void App::choose_folder(bool data) {
    IFileOpenDialog* d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return;
    DWORD o = 0;
    d->GetOptions(&o);
    d->SetOptions(o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    d->SetTitle(data ? L"Choose the folder the setup writes to" : L"Choose your extracted game folder (exefs and romfs)");
    if (SUCCEEDED(d->Show(wnd_))) {
        IShellItem* it = nullptr;
        if (SUCCEEDED(d->GetResult(&it))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                if (data) {
                    cfg_.data_dir = p;
                    save_config(cfg_);
                    prep_check_.clear();
                    evaluate();
                    start_requirements();
                    start_prepare_check();
                    refresh();
                } else set_source(p);
                CoTaskMemFree(p);
            }
            it->Release();
        }
    }
    d->Release();
}

void App::rebuild_dialog() {
    if (running_ || !gid_.ok) return;
    TASKDIALOG_BUTTON b[] = {
        {101, L"Compile the shaders again\nAfter a new graphics driver or another graphics card. Uses the translations you have."},
        {102, L"Translate every shader again\nAfter a new translator (a new release of this port). Then compiles them."},
        {103, L"Everything again\nAfter a game update: finds the shaders, translates the new ones, lists the programs and compiles them."},
    };
    TASKDIALOGCONFIG c{sizeof c};
    c.hwndParent = wnd_;
    c.hInstance = inst_;
    c.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
    c.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    c.pszWindowTitle = L"" GAME_PRODUCT L" setup";
    c.pszMainInstruction = L"Rebuild the shaders";
    c.pszContent = L"The setup notices game updates, a new translator and a new graphics driver by itself. Use this when the game shows "
                   L"broken or missing graphics, or stutters although everything is marked done.";
    c.cButtons = 3;
    c.pButtons = b;
    int pick = 0;
    if (FAILED(TaskDialogIndirect(&c, &pick, nullptr, nullptr))) return;
    if (pick == 101) {
        start_steps(true);
    } else if (pick == 102) {
        st_.translated_game.clear();
        st_.translated_with = "rebuild";   // differs from the stamp: evaluate() moves the old translations aside
        save_state(lay_.state_file, st_);
        start_steps(true);
    } else if (pick == 103) {
        st_.corpus_game.clear();
        st_.programs_game.clear();
        st_.translated_game.clear();
        save_state(lay_.state_file, st_);
        start_steps(true);
    }
}

void App::play() {
    if (!gid_.ok) return;
    write_game_ini();
    const std::wstring log_path = join(lay_.data, L"logs\\" GAME_LOG);
    std::wstring cmd = quote(tools_.game_exe) + L" --game " + quote(lay_.game) + L" --ini " + quote(lay_.game_ini) + L" --log " + quote(log_path);
    if (!game_args_.empty()) cmd += L" " + game_args_;
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(tools_.game_exe.c_str(), buf.data(), nullptr, nullptr, FALSE, 0, nullptr, tools_.exe_dir.c_str(), &si, &pi)) {
        MessageBoxW(wnd_, (L"Could not start " + tools_.game_exe + L" (Windows error " + std::to_wstring(GetLastError()) + L").").c_str(),
                    L"" GAME_PRODUCT L" setup", MB_OK | MB_ICONERROR);
        return;
    }
    CloseHandle(pi.hThread);
    // A game that ends within a few seconds did not start: say why instead of closing (exit code 7: another run of the game or too little
    // free memory beside it; 1: a start-up error, its log says which).
    SetCursor(LoadCursor(nullptr, IDC_WAIT));
    const bool ended = WaitForSingleObject(pi.hProcess, 4000) == WAIT_OBJECT_0;
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    if (ended && code != 0) {
        const std::wstring why = code == 7 ? L"The game is already running, or another run of it leaves too little free memory. Close it first."
                                           : L"The game stopped while starting (exit code " + std::to_wstring(code) + L").";
        MessageBoxW(wnd_, (why + L"\r\n\r\nIts log: " + log_path).c_str(), L"" GAME_PRODUCT L" setup", MB_OK | MB_ICONWARNING);
        return;
    }
    if (SendMessageW(play_close_, BM_GETCHECK, 0, 0) == BST_CHECKED) DestroyWindow(wnd_);
}

void App::make_shortcut() {
    if (!gid_.ok) return;
    write_game_ini();
    IShellLinkW* sl = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl)))) return;
    sl->SetPath(tools_.game_exe.c_str());
    sl->SetArguments((L"--game " + quote(lay_.game) + L" --ini " + quote(lay_.game_ini)).c_str());
    sl->SetWorkingDirectory(tools_.exe_dir.c_str());
    sl->SetDescription(L"" GAME_PRODUCT);
    PWSTR desk = nullptr;
    bool ok = false;
    std::wstring path;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desk))) {
        path = join(desk, L"" GAME_PRODUCT L".lnk");
        CoTaskMemFree(desk);
        IPersistFile* pf = nullptr;
        if (SUCCEEDED(sl->QueryInterface(IID_PPV_ARGS(&pf)))) {
            ok = SUCCEEDED(pf->Save(path.c_str(), TRUE));
            pf->Release();
        }
    }
    sl->Release();
    MessageBoxW(wnd_, ok ? (L"Shortcut created: " + path).c_str() : L"The shortcut could not be created.", L"" GAME_PRODUCT L" setup",
                MB_OK | (ok ? MB_ICONINFORMATION : MB_ICONERROR));
}

void App::about() {
    std::wstring t = L"" GAME_PRODUCT L" setup\r\n\r\nGame: " GAME_TITLE L" (" GAME_TITLE_ID L"), " GAME_DUMP_VERSION L"\r\nGame program: " + tools_.game_exe +
                     L"\r\nTranslator: " + (tools_.translator.empty() ? L"(missing)" : tools_.translator) + L"\r\nPython: " +
                     (tools_.python.empty() ? L"(missing)" : tools_.python) + L"\r\nSettings: " + config_path();
    MessageBoxW(wnd_, t.c_str(), L"About", MB_OK | MB_ICONINFORMATION);
}

// ---- messages ----------------------------------------------------------------------------------------------------------------------
void App::draw_nav(const DRAWITEMSTRUCT* d) {
    if (d->itemID == static_cast<UINT>(-1)) return;
    const int i = static_cast<int>(d->itemID);
    const bool sel = i == page_;
    HDC dc = d->hDC;
    RECT r = d->rcItem;
    HBRUSH bg = CreateSolidBrush(sel ? RGB(0xE5, 0xF1, 0xFB) : GetSysColor(COLOR_WINDOW));
    FillRect(dc, &r, bg);
    DeleteObject(bg);
    if (sel) {
        RECT bar{r.left, r.top + S(8), r.left + S(4), r.bottom - S(8)};
        HBRUSH b = CreateSolidBrush(RGB(0x00, 0x5F, 0xB8));
        FillRect(dc, &bar, b);
        DeleteObject(b);
    }
    const wchar_t* names[P_COUNT] = {L"Requirements", L"Game files", L"Prepare", L"Play"};
    std::wstring sub;
    COLORREF sub_col = RGB(0x5F, 0x5F, 0x5F);
    if (i == P_REQ) {
        int bad = 0, warn = 0;
        for (const Check& c : checks_) { bad += c.level == Level::bad; warn += c.level == Level::warn; }
        sub = checks_.empty() ? L"Checking..." : bad ? std::to_wstring(bad) + L" missing" : warn ? L"OK, " + std::to_wstring(warn) + L" note(s)" : L"All there";
        sub_col = checks_.empty() ? sub_col : bad ? RGB(0xB3, 0x1B, 0x1B) : warn ? RGB(0x8A, 0x5A, 0x00) : RGB(0x0F, 0x6B, 0x2F);
    } else if (i == P_GAME) {
        sub = gid_.ok ? L"Found" : source_kind(cfg_.source) == SourceKind::nup ? L"To extract" : L"Choose your dump";
        sub_col = gid_.ok || source_kind(cfg_.source) == SourceKind::nup ? RGB(0x0F, 0x6B, 0x2F) : RGB(0xB3, 0x1B, 0x1B);
    } else if (i == P_PREP) {
        int todo = 0;
        for (int s = 0; s < S_COUNT; ++s) todo += need_[s];
        sub = running_ ? L"Running" : todo ? std::to_wstring(todo) + L" step(s) to do" : L"Done";
        sub_col = running_ ? RGB(0x00, 0x5F, 0xB8) : todo ? RGB(0x8A, 0x5A, 0x00) : RGB(0x0F, 0x6B, 0x2F);
    } else {
        sub = ready_to_play(need_) ? L"Ready" : L"Not ready yet";
        sub_col = ready_to_play(need_) ? RGB(0x0F, 0x6B, 0x2F) : sub_col;
    }
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ of = SelectObject(dc, bold_);
    SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
    RECT t{r.left + S(18), r.top + S(8), r.right - S(8), r.top + S(30)};
    const std::wstring name = std::to_wstring(i + 1) + L"   " + names[i];
    DrawTextW(dc, name.c_str(), -1, &t, DT_SINGLELINE | DT_LEFT | DT_NOPREFIX);
    SelectObject(dc, font_);
    SetTextColor(dc, sub_col);
    RECT s2{r.left + S(36), r.top + S(29), r.right - S(8), r.bottom - S(4)};
    DrawTextW(dc, sub.c_str(), -1, &s2, DT_SINGLELINE | DT_LEFT | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(dc, of);
}

LRESULT App::notify(NMHDR* n) {
    if (n->idFrom == ID_REQ_LIST && n->code == NM_CUSTOMDRAW) {
        auto* cd = reinterpret_cast<NMLVCUSTOMDRAW*>(n);
        if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
        if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) return CDRF_NOTIFYSUBITEMDRAW;
        if (cd->nmcd.dwDrawStage == (CDDS_ITEMPREPAINT | CDDS_SUBITEM)) {
            const size_t i = cd->nmcd.dwItemSpec;
            cd->clrText = GetSysColor(COLOR_WINDOWTEXT);
            if (cd->iSubItem == 1 && i < checks_.size()) {
                const Level lv = checks_[i].level;
                cd->clrText = lv == Level::ok ? RGB(0x0F, 0x6B, 0x2F) : lv == Level::warn ? RGB(0x8A, 0x5A, 0x00) : lv == Level::bad ? RGB(0xB3, 0x1B, 0x1B) : RGB(0x5F, 0x5F, 0x5F);
            }
            return CDRF_NEWFONT;
        }
    }
    if (n->idFrom == ID_REQ_LIST && n->code == LVN_ITEMCHANGED) {
        auto* lv = reinterpret_cast<NMLISTVIEW*>(n);
        if ((lv->uNewState & LVIS_SELECTED) && lv->iItem >= 0 && static_cast<size_t>(lv->iItem) < checks_.size()) {
            const Check& c = checks_[static_cast<size_t>(lv->iItem)];
            const std::wstring t = c.item + L": " + c.state + L"\r\n" + c.detail + L"\r\n\r\n" + (c.fix.empty() ? L"Nothing to do." : c.fix);
            SetWindowTextW(req_fix_, t.c_str());
        }
    }
    if (n->idFrom == ID_PREP_LIST && n->code == NM_CUSTOMDRAW) {
        auto* cd = reinterpret_cast<NMLVCUSTOMDRAW*>(n);
        if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
        if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) return CDRF_NOTIFYSUBITEMDRAW;
        if (cd->nmcd.dwDrawStage == (CDDS_ITEMPREPAINT | CDDS_SUBITEM)) {
            const int i = static_cast<int>(cd->nmcd.dwItemSpec);
            cd->clrText = GetSysColor(COLOR_WINDOWTEXT);
            if (cd->iSubItem == 1 && i < S_COUNT)
                cd->clrText = (running_ && i == cur_) ? RGB(0x00, 0x5F, 0xB8) : need_[i] ? RGB(0x8A, 0x5A, 0x00) : RGB(0x0F, 0x6B, 0x2F);
            return CDRF_NEWFONT;
        }
    }
    return 0;
}

void App::command(int id, int code) {
    // only clicks, menu items and accelerators (code 0 or 1); edit notifications (EN_CHANGE when refresh() sets a text) would recurse
    if (id != ID_NAV && code != BN_CLICKED && code != 1) return;
    switch (id) {
        case ID_NAV:
            if (code == LBN_SELCHANGE) show_page(static_cast<int>(SendMessageW(nav_, LB_GETCURSEL, 0, 0)));
            break;
        case ID_REQ_RECHECK: case IDM_RECHECK:
            if (running_) break;
            checks_.clear();
            step_error_.clear();
            cancel_requested_ = false;
            prep_check_.clear();
            evaluate();
            start_requirements();
            start_prepare_check();
            break;
        case ID_REQ_COPY: {
            const int len = GetWindowTextLengthW(req_fix_);
            std::wstring t(static_cast<size_t>(len) + 1, L'\0');
            GetWindowTextW(req_fix_, t.data(), len + 1);
            if (OpenClipboard(wnd_)) {
                EmptyClipboard();
                HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, t.size() * sizeof(wchar_t));
                if (g) {
                    std::memcpy(GlobalLock(g), t.c_str(), t.size() * sizeof(wchar_t));
                    GlobalUnlock(g);
                    SetClipboardData(CF_UNICODETEXT, g);
                }
                CloseClipboard();
            }
            break;
        }
        case ID_GAME_NUP: case IDM_CHOOSE_NUP: if (!running_) choose_nup(); break;
        case ID_GAME_FOLDER: case IDM_CHOOSE_FOLDER: if (!running_) choose_folder(false); break;
        case ID_GAME_DATA_CHANGE: case IDM_DATA: if (!running_) choose_folder(true); break;
        case IDM_OPEN_DATA: {
            std::error_code ec;
            fs::create_directories(cfg_.data_dir, ec);
            ShellExecuteW(wnd_, L"open", cfg_.data_dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            break;
        }
        case ID_PREP_START: case IDM_START:
            if (ready_to_play(need_) && id == ID_PREP_START) { step_error_.clear(); cancel_requested_ = false; prep_check_.clear(); evaluate(); start_prepare_check(); }
            else start_steps(false);
            break;
        case ID_PREP_CANCEL: cancel_steps(); break;
        case ID_PREP_REBUILD: case IDM_REBUILD: rebuild_dialog(); break;
        case ID_PLAY_GO: case IDM_PLAY: play(); break;
        case ID_PLAY_SHORTCUT: case IDM_SHORTCUT: make_shortcut(); break;
        case IDM_ABOUT: about(); break;
        case IDM_EXIT: PostMessageW(wnd_, WM_CLOSE, 0, 0); break;
        default:
            if (id >= IDM_PAGE0 && id < static_cast<int>(IDM_PAGE0) + P_COUNT) show_page(id - static_cast<int>(IDM_PAGE0));
    }
    if (id != ID_NAV) refresh();
}

LRESULT CALLBACK App::proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCCREATE) g_app->wnd_ = h;
    if (!g_app || !g_app->wnd_) return DefWindowProcW(h, m, w, l);
    try {
        return g_app->on(m, w, l);
    } catch (const std::exception& e) {   // a bug, not a player error: say what it was instead of vanishing
        MessageBoxA(h, e.what(), GAME_PRODUCT " setup: internal error", MB_OK | MB_ICONERROR);
        return DefWindowProcW(h, m, w, l);
    }
}

LRESULT App::on(UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_CREATE:
            dpi_ = static_cast<int>(GetDpiForWindow(wnd_));
            make_fonts();
            create_controls();
            evaluate();
            layout();
            show_page(page_);
            start_requirements();
            start_prepare_check();
            refresh();
            SetTimer(wnd_, kTimer, 250, nullptr);
            if (auto_start_) PostMessageW(wnd_, WM_COMMAND, IDM_START, 0);
            return 0;
        case WM_SIZE: layout(); return 0;
        case WM_GETMINMAXINFO: {
            auto* mm = reinterpret_cast<MINMAXINFO*>(l);
            mm->ptMinTrackSize = {S(820), S(600)};
            return 0;
        }
        case WM_DPICHANGED: {
            dpi_ = HIWORD(w);
            make_fonts();
            for (const Ctl& c : ctls_) SendMessageW(c.h, WM_SETFONT, reinterpret_cast<WPARAM>(c.h == play_state_ ? bold_ : font_), FALSE);
            for (int p = 0; p < P_COUNT; ++p) SendMessageW(h1_[p], WM_SETFONT, reinterpret_cast<WPARAM>(title_), FALSE);
            for (HWND hh : {req_fix_label_, game_src_label_, game_data_label_, prep_log_label_}) SendMessageW(hh, WM_SETFONT, reinterpret_cast<WPARAM>(bold_), FALSE);
            const RECT* r = reinterpret_cast<const RECT*>(l);
            SetWindowPos(wnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            layout();
            return 0;
        }
        case WM_MEASUREITEM: {
            auto* mi = reinterpret_cast<MEASUREITEMSTRUCT*>(l);
            mi->itemHeight = static_cast<UINT>(S(54));
            return TRUE;
        }
        case WM_DRAWITEM:
            if (w == ID_NAV) { draw_nav(reinterpret_cast<const DRAWITEMSTRUCT*>(l)); return TRUE; }
            break;
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(w);
            HWND c = reinterpret_cast<HWND>(l);
            SetBkColor(dc, GetSysColor(COLOR_WINDOW));
            bool grey = c == game_nsp_ || c == game_data_info_;
            for (int p = 0; p < P_COUNT; ++p) grey |= c == hint_[p];
            SetTextColor(dc, grey ? RGB(0x5F, 0x5F, 0x5F) : GetSysColor(COLOR_WINDOWTEXT));
            if (c == req_fix_ || c == prep_log_ || c == game_src_ || c == game_data_) return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
        }
        case WM_ERASEBKGND: {
            RECT rc;
            GetClientRect(wnd_, &rc);
            FillRect(reinterpret_cast<HDC>(w), &rc, GetSysColorBrush(COLOR_WINDOW));
            RECT line{S(200), 0, S(200) + 1, rc.bottom};
            FillRect(reinterpret_cast<HDC>(w), &line, GetSysColorBrush(COLOR_3DLIGHT));
            return 1;
        }
        case WM_COMMAND: command(LOWORD(w), HIWORD(w)); return 0;
        case WM_NOTIFY: return notify(reinterpret_cast<NMHDR*>(l));
        case WM_TIMER:
            if (running_ && cur_ >= 0) {
                std::wstring t = std::wstring(kStepName[cur_]);
                if (total_) {
                    SendMessageW(prep_bar_, PBM_SETPOS, static_cast<WPARAM>(1000.0 * static_cast<double>(std::min(done_, total_)) / static_cast<double>(total_)), 0);
                    t += L": " + with_commas(done_) + L" of " + with_commas(total_);
                    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - step_t0_).count();
                    if (done_ > 0 && done_ < total_ && secs > 5) {
                        const int left = static_cast<int>(secs / static_cast<double>(done_) * static_cast<double>(total_ - done_));
                        t += left >= 120 ? L"  ·  about " + std::to_wstring(left / 60) + L" min left" : L"  ·  about " + std::to_wstring(std::max(left, 1)) + L" s left";
                    }
                } else {
                    t += L"...";
                }
                if (cancel_requested_) t = L"Stopping (what is done is kept)...";
                SetWindowTextW(prep_text_, t.c_str());
            }
            return 0;
        case kMsgLine: {
            std::unique_ptr<std::string> s(reinterpret_cast<std::string*>(l));
            step_line(*s);
            return 0;
        }
        case kMsgExit: step_exit(static_cast<DWORD>(w)); return 0;
        case kMsgCheck: {
            std::unique_ptr<std::string> s(reinterpret_cast<std::string*>(l));
            check_child_.wait();
            checking_prep_ = false;
            prep_check_ = widen(*s);
            evaluate();
            refresh();
            return 0;
        }
        case kMsgReqs: {
            std::unique_ptr<std::vector<Check>> r(reinterpret_cast<std::vector<Check>*>(l));
            checks_ = std::move(*r);
            checks_running_ = false;
            refresh();
            return 0;
        }
        case WM_CLOSE:
            if (running_) {
                if (MessageBoxW(wnd_, L"The preparation is running. Stop it and close the setup? What is done is kept; Start continues later.",
                                L"" GAME_PRODUCT L" setup", MB_YESNO | MB_ICONQUESTION) != IDYES)
                    return 0;
                cancel_steps();
                child_.wait();
            }
            DestroyWindow(wnd_);
            return 0;
        case WM_DESTROY:
            KillTimer(wnd_, kTimer);
            if (check_child_.running()) check_child_.cancel();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(wnd_, m, w, l);
}

HMENU make_menu() {
    HMENU bar = CreateMenu(), file = CreatePopupMenu(), set = CreatePopupMenu(), view = CreatePopupMenu(), help = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, IDM_CHOOSE_NUP, L"Choose a .nup &file...");
    AppendMenuW(file, MF_STRING, IDM_CHOOSE_FOLDER, L"Choose a game &folder...");
    AppendMenuW(file, MF_STRING, IDM_DATA, L"Change the &data folder...");
    AppendMenuW(file, MF_STRING, IDM_OPEN_DATA, L"&Open the data folder");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, IDM_EXIT, L"E&xit");
    AppendMenuW(set, MF_STRING, IDM_RECHECK, L"&Check requirements again\tF5");
    AppendMenuW(set, MF_STRING, IDM_START, L"&Start or continue the preparation");
    AppendMenuW(set, MF_STRING, IDM_REBUILD, L"&Rebuild shaders...");
    AppendMenuW(set, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(set, MF_STRING, IDM_PLAY, L"&Play");
    AppendMenuW(set, MF_STRING, IDM_SHORTCUT, L"Create a desktop s&hortcut");
    const wchar_t* pages[P_COUNT] = {L"&1 Requirements", L"&2 Game files", L"&3 Prepare", L"&4 Play"};
    for (int i = 0; i < P_COUNT; ++i) AppendMenuW(view, MF_STRING, IDM_PAGE0 + i, pages[i]);
    AppendMenuW(help, MF_STRING, IDM_ABOUT, L"&About");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"&File");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(view), L"&View");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(set), L"&Setup");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(help), L"&Help");
    return bar;
}

int App::run(HINSTANCE inst, int show) {
    inst_ = inst;
    tools_ = find_tools();
    cfg_ = load_config();
    // command line
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool changed = false;
    for (int i = 1; argv && i < argc; ++i) {
        const std::wstring a = argv[i];
        const bool more = i + 1 < argc;
        if (a == L"--data" && more) { cfg_.data_dir = fs::absolute(argv[++i]).wstring(); changed = true; }
        else if (a == L"--source" && more) { cfg_.source = fs::absolute(argv[++i]).wstring(); changed = true; }
        else if (a == L"--page" && more) page_ = _wtoi(argv[++i]);
        else if (a == L"--start") auto_start_ = true;
        else if (a == L"--game-args" && more) game_args_ = argv[++i];
    }
    if (argv) LocalFree(argv);
    if (changed) save_config(cfg_);

    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
    const int sys = static_cast<int>(GetDpiForSystem());
    const int w = MulDiv(940, sys, 96), h = MulDiv(680, sys, 96);
    HACCEL acc = nullptr;
    {
        ACCEL a[] = {{FVIRTKEY, VK_F5, IDM_RECHECK}};
        acc = CreateAcceleratorTableW(a, 1);
    }
    CreateWindowExW(0, kClass, L"" GAME_PRODUCT L" - Initial Setup", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, w, h, nullptr,
                    make_menu(), inst, nullptr);
    if (!wnd_) return 1;
    ShowWindow(wnd_, show);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (TranslateAcceleratorW(wnd_, acc, &msg)) continue;
        if (IsDialogMessageW(wnd_, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
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
    if (FILE* f = _wfopen(join(exe_dir(), L"" GAME_ID L"_setup_crash.txt").c_str(), L"ab")) { std::fputs(b, f); std::fclose(f); }
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
