// SPDX-License-Identifier: GPL-3.0-or-later
// Native menu bar and dialogs of the game window: see win_menu.h.
#include "win_menu.h"

#ifdef _WIN32

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>

#include <SDL3/SDL_video.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "menu_model.h"
#include "nxrt/diag.h"
#include "nxrt/extras.h"
#include "nxrt/game.h"
#include "nxrt/input_source.h"
#include "nxrt/log.h"
#include "nxrt/platform.h"
#include "pad_map.h"
#include "play_game.h"

#pragma comment(lib, "comctl32.lib")

namespace nxrt::play {
namespace {

constexpr const char* kTag = "menu";
constexpr UINT_PTR kMenuLoopTimer = 0x4E584D31;   // keeps the freeze watchdog fed while a drop-down is open
constexpr UINT kOpenMsg = WM_APP + 1;             // to the UI thread: wParam = MenuAction

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}
std::wstring full_path(const std::string& p) {
    const std::wstring w = widen(p);
    wchar_t buf[MAX_PATH * 2];
    const DWORD n = GetFullPathNameW(w.c_str(), MAX_PATH * 2, buf, nullptr);
    return n && n < MAX_PATH * 2 ? std::wstring(buf, n) : w;
}

struct State {
    HWND hwnd = nullptr;
    SDL_Window* win = nullptr;
    UiHost* host = nullptr;
    WinMenuHooks hooks;
    std::vector<MenuItem> model;
    HMENU bar = nullptr;
    bool installed = false;
    std::atomic<bool> attached{false};
    std::atomic<DWORD> ui_tid{0};
    std::wstring mnemonics;   // the access keys of the top-level menus, lower case
    std::once_flag ui_once;
};
State& S() {
    static State* s = new State;   // never destroyed: the UI thread may outlive static destruction
    return *s;
}

// ---- the bar ----------------------------------------------------------------------------------------------------------------------------------
std::wstring item_label(const MenuItem& m) {
    std::string l = m.label;
    if (m.next_start) {
        const size_t tab = l.find('\t');
        l.insert(tab == std::string::npos ? l.size() : tab, "  (next start)");
    }
    return widen(l);
}

HMENU build_popup(const std::vector<MenuItem>& items) {
    HMENU m = CreatePopupMenu();
    for (const MenuItem& it : items) {
        if (it.kind == MenuItem::Kind::Separator) {
            AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        } else if (it.kind == MenuItem::Kind::Submenu) {
            AppendMenuW(m, MF_POPUP | MF_STRING, reinterpret_cast<UINT_PTR>(build_popup(it.children)), item_label(it).c_str());
        } else {
            const std::wstring label = item_label(it);
            MENUITEMINFOW mi = {sizeof mi};
            mi.fMask = MIIM_ID | MIIM_FTYPE | MIIM_STRING;
            mi.fType = MFT_STRING | (it.kind == MenuItem::Kind::Radio ? MFT_RADIOCHECK : 0u);
            mi.wID = static_cast<UINT>(it.id);
            mi.dwTypeData = const_cast<wchar_t*>(label.c_str());
            InsertMenuItemW(m, GetMenuItemCount(m), TRUE, &mi);
        }
    }
    return m;
}

void refresh_checks() {
    State& st = S();
    const Settings s = st.host->settings();
    const LiveStatus live = st.host->status();
    menu_for_each(st.model, [&](const MenuItem& m) {
        bool on = false;
        if (m.action == MenuAction::DebugOverlay) on = st.hooks.debug_overlay && st.hooks.debug_overlay();
        else if (m.checked) on = m.checked(s, live);
        else return;
        CheckMenuItem(st.bar, static_cast<UINT>(m.id), MF_BYCOMMAND | (on ? MF_CHECKED : MF_UNCHECKED));
    });
}

void open_in_explorer(const std::wstring& file) {
    const std::wstring args = L"/select,\"" + file + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
}

void set_clipboard(HWND owner, const std::string& text) {
    const std::wstring w = widen(text);
    if (!OpenClipboard(owner)) return;
    EmptyClipboard();
    if (HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, (w.size() + 1) * sizeof(wchar_t))) {
        std::memcpy(GlobalLock(g), w.c_str(), (w.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(g);
        if (!SetClipboardData(CF_UNICODETEXT, g)) GlobalFree(g);
    }
    CloseClipboard();
}

void post_ui(MenuAction a);

void on_command(int id) {
    State& st = S();
    const MenuItem* m = menu_find(st.model, id);
    if (!m) return;
    if (m->set) {
        Settings s = st.host->settings();
        if (m->set(s)) {
            st.host->apply(s);
            NXLOG_I(kTag, "%s", narrow(item_label(*m)).c_str());
        }
        return;
    }
    switch (m->action) {
        case MenuAction::SaveSettings: {
            std::string err;
            if (st.hooks.save_now && !st.hooks.save_now(&err)) NXLOG_W(kTag, "cannot save the settings: %s", err.c_str());
            break;
        }
        case MenuAction::ReloadSettings: {
            std::string err;
            if (st.hooks.reload && !st.hooks.reload(&err)) NXLOG_W(kTag, "cannot reload the settings: %s", err.c_str());
            break;
        }
        case MenuAction::OpenSettingsFile:
            ShellExecuteW(nullptr, L"open", L"notepad.exe", (L"\"" + full_path(st.host->settings_path()) + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
            break;
        case MenuAction::OpenSettingsFolder: open_in_explorer(full_path(st.host->settings_path())); break;
        case MenuAction::OpenLogFile:
            if (const char* p = log_path(); p && *p)
                ShellExecuteW(nullptr, L"open", L"notepad.exe", (L"\"" + full_path(p) + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
            break;
        case MenuAction::OpenLogFolder:
            if (const char* p = log_path(); p && *p) open_in_explorer(full_path(p));
            break;
        case MenuAction::CaptureBug:
            if (st.hooks.capture_bug) st.hooks.capture_bug();
            break;
        case MenuAction::Exit:
            if (st.hooks.quit) st.hooks.quit();
            break;
        case MenuAction::TestRumble: st.host->test_rumble(); break;
        case MenuAction::DockNow: st.host->set_docked_now(true); break;
        case MenuAction::HandheldNow: st.host->set_docked_now(false); break;
        case MenuAction::CopyDebugReport:
            if (st.hooks.debug_report) set_clipboard(st.hwnd, st.hooks.debug_report());
            break;
        case MenuAction::DebugOverlay:
            if (st.hooks.set_debug_overlay && st.hooks.debug_overlay) st.hooks.set_debug_overlay(!st.hooks.debug_overlay());
            break;
        default: post_ui(m->action); break;   // the dialogs and message boxes run on the UI thread
    }
}

LRESULT CALLBACK subclass_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    switch (msg) {
        case WM_SYSCOMMAND:
            if ((wp & 0xFFF0) == SC_KEYMENU) {
                // Alt alone and F10 (lParam 0) stay game keys. Alt + the access key of a top-level menu (Alt+G, Alt+I ...) opens it, as in any
                // Windows program; Alt+Space opens the system menu. SDL itself swallows SC_KEYMENU, so these go straight to Windows.
                const wchar_t c = static_cast<wchar_t>(towlower(static_cast<wint_t>(lp)));
                if (c == L' ' || (S().attached && c && S().mnemonics.find(c) != std::wstring::npos)) return DefWindowProcW(h, msg, wp, lp);
                return 0;
            }
            break;
        case WM_INITMENUPOPUP:
            if (!HIWORD(lp)) refresh_checks();
            break;
        case WM_ENTERMENULOOP:
            // A drop-down is open: Windows runs its modal loop on this thread, so the game's frame waits. The timer's messages are handled inside
            // that loop and tell the freeze watchdog that the process is alive.
            SetTimer(h, kMenuLoopTimer, 250, nullptr);
            diag_progress();
            break;
        case WM_EXITMENULOOP:
            KillTimer(h, kMenuLoopTimer);
            break;
        case WM_TIMER:
            if (wp == kMenuLoopTimer) {
                diag_progress();
                return 0;
            }
            break;
        case WM_COMMAND:
            if (HIWORD(wp) == 0 && LOWORD(wp) >= kMenuFirstId && lp == 0) {
                on_command(LOWORD(wp));
                return 0;
            }
            break;
        default: break;
    }
    return DefSubclassProc(h, msg, wp, lp);
}

void attach(bool on) {
    State& st = S();
    int w = 0, h = 0;
    SDL_GetWindowSize(st.win, &w, &h);   // the client size before: the bar must not take it from the picture
    SetMenu(st.hwnd, on ? st.bar : nullptr);
    DrawMenuBar(st.hwnd);
    st.attached = on;
    if (w > 0 && h > 0) SDL_SetWindowSize(st.win, w, h);   // SDL counts the bar (GetMenu) when it sizes the window
    RECT rc = {};
    GetClientRect(st.hwnd, &rc);
    NXLOG_I(kTag, "menu bar %s (client %ldx%ld, wanted %dx%d)", on ? "shown" : "hidden (fullscreen)", rc.right, rc.bottom, w, h);
}

// =====================================================================================================================================
// The dialogs: modeless windows on a thread of their own (the UI thread), with visual styles and the game window's DPI.
// =====================================================================================================================================

constexpr const char* kPadSrcNames[kPadSrcCount] = {
    "A / Cross (bottom)", "B / Circle (right)", "X / Square (left)", "Y / Triangle (top)", "LB / L1", "RB / R1", "LT / L2", "RT / R2",
    "Menu / Options", "View / Create", "Left stick click", "Right stick click", "D-pad up", "D-pad down", "D-pad left", "D-pad right",
};
struct SwitchBtn {
    const char* name;
    hid::NpadButtonBit bit;
};
constexpr SwitchBtn kSwitchBtns[] = {
    {"A", hid::kBtnA},       {"B", hid::kBtnB},         {"X", hid::kBtnX},       {"Y", hid::kBtnY},         {"L", hid::kBtnL},
    {"R", hid::kBtnR},       {"ZL", hid::kBtnZL},       {"ZR", hid::kBtnZR},     {"Plus", hid::kBtnPlus},   {"Minus", hid::kBtnMinus},
    {"D-pad up", hid::kBtnUp}, {"D-pad down", hid::kBtnDown}, {"D-pad left", hid::kBtnLeft}, {"D-pad right", hid::kBtnRight},
    {"Left stick click", hid::kBtnStickL}, {"Right stick click", hid::kBtnStickR},
};
constexpr int kSwitchBtnCount = static_cast<int>(sizeof kSwitchBtns / sizeof kSwitchBtns[0]);

// ListView through the wide messages (the build is not UNICODE, so the ListView_* macros are the ANSI ones).
void lv_insert_column(HWND lv, int i, LVCOLUMNW* c) { SendMessageW(lv, LVM_INSERTCOLUMNW, static_cast<WPARAM>(i), reinterpret_cast<LPARAM>(c)); }
void lv_insert_item(HWND lv, LVITEMW* it) { SendMessageW(lv, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(it)); }
void lv_set_text(HWND lv, int row, int sub, wchar_t* text) {
    LVITEMW it = {};
    it.iSubItem = sub;
    it.pszText = text;
    SendMessageW(lv, LVM_SETITEMTEXTW, static_cast<WPARAM>(row), reinterpret_cast<LPARAM>(&it));
}

std::string sources_text(uint32_t bits) {
    std::string s;
    for (int i = 0; i < kPadSrcCount; ++i) {
        if (!(bits & (1u << i))) continue;
        if (!s.empty()) s += ", ";
        s += kPadSrcNames[i];
    }
    return s;
}

class Dlg {
public:
    virtual ~Dlg() = default;
    HWND hwnd = nullptr;
    MenuAction kind = MenuAction::None;
    int dpi = 96;
    HFONT font = nullptr, mono = nullptr;

    int sc(int v) const { return MulDiv(v, dpi, 96); }
    HWND add(const wchar_t* cls, const std::wstring& text, DWORD style, int x, int y, int w, int h, int id = 0, DWORD ex = 0) {
        HWND c = CreateWindowExW(ex, cls, text.c_str(), WS_CHILD | WS_VISIBLE | style, sc(x), sc(y), sc(w), sc(h), hwnd,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        return c;
    }
    HWND label(const std::wstring& t, int x, int y, int w, int h = 20) { return add(L"STATIC", t, SS_LEFT, x, y, w, h); }
    HWND button(const std::wstring& t, int id, int x, int y, int w, int h = 26) { return add(L"BUTTON", t, BS_PUSHBUTTON | WS_TABSTOP, x, y, w, h, id); }
    HWND check(const std::wstring& t, int id, int x, int y, int w, bool on) {
        HWND c = add(L"BUTTON", t, BS_AUTOCHECKBOX | WS_TABSTOP, x, y, w, 22, id);
        SendMessageW(c, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
        return c;
    }
    HWND track(int id, int x, int y, int w, int lo, int hi, int v) {
        HWND c = add(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, x, y, w, 26, id);
        SendMessageW(c, TBM_SETRANGE, TRUE, MAKELPARAM(lo, hi));
        SendMessageW(c, TBM_SETPOS, TRUE, v);
        return c;
    }
    HWND combo(int id, int x, int y, int w, const std::vector<std::wstring>& items, int sel) {
        HWND c = add(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, x, y, w, 300, id);
        for (const auto& i : items) SendMessageW(c, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(i.c_str()));
        SendMessageW(c, CB_SETCURSEL, sel, 0);
        return c;
    }
    HWND item(int id) const { return GetDlgItem(hwnd, id); }
    bool checked(int id) const { return SendMessageW(item(id), BM_GETCHECK, 0, 0) == BST_CHECKED; }
    int pos(int id) const { return static_cast<int>(SendMessageW(item(id), TBM_GETPOS, 0, 0)); }
    int sel(int id) const { return static_cast<int>(SendMessageW(item(id), CB_GETCURSEL, 0, 0)); }
    void text(int id, const std::string& t) { SetWindowTextW(item(id), widen(t).c_str()); }
    UiHost& host() const { return *S().host; }
    void change(const std::function<void(Settings&)>& f) {
        Settings s = host().settings();
        f(s);
        host().apply(s);
    }

    virtual const wchar_t* title() const = 0;
    virtual SIZE size() const = 0;               // client size in 96-dpi units
    virtual void build() = 0;
    virtual void on_command(int id, int code) { (void)id; (void)code; }
    virtual void on_scroll(int id, int code) { (void)id; (void)code; }
    virtual void on_timer() {}
    virtual LRESULT on_notify(NMHDR* n) { (void)n; return 0; }
    virtual void on_activate() {}
};

std::vector<std::unique_ptr<Dlg>>& dialogs() {
    static std::vector<std::unique_ptr<Dlg>> d;   // UI thread only
    return d;
}

LRESULT CALLBACK dlg_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    Dlg* d = reinterpret_cast<Dlg*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        d = static_cast<Dlg*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(d));
        d->hwnd = h;
    }
    if (!d) return DefWindowProcW(h, msg, wp, lp);
    switch (msg) {
        case WM_COMMAND:
            if (LOWORD(wp) == IDCANCEL) {   // Escape
                DestroyWindow(h);
                return 0;
            }
            d->on_command(LOWORD(wp), HIWORD(wp));
            return 0;
        case WM_HSCROLL:
            if (lp) d->on_scroll(GetDlgCtrlID(reinterpret_cast<HWND>(lp)), LOWORD(wp));
            return 0;
        case WM_TIMER:
            d->on_timer();
            return 0;
        case WM_NOTIFY:
            return d->on_notify(reinterpret_cast<NMHDR*>(lp));
        case WM_ACTIVATE:
            if (LOWORD(wp) != WA_INACTIVE) d->on_activate();
            break;
        case WM_CLOSE:
            DestroyWindow(h);
            return 0;
        case WM_DESTROY: {
            KillTimer(h, 1);
            auto& v = dialogs();
            for (size_t i = 0; i < v.size(); ++i) {
                if (v[i].get() == d) {
                    SetWindowLongPtrW(h, GWLP_USERDATA, 0);
                    if (d->font) DeleteObject(d->font);
                    if (d->mono) DeleteObject(d->mono);
                    v.erase(v.begin() + static_cast<std::ptrdiff_t>(i));   // deletes d
                    break;
                }
            }
            return 0;
        }
        default: break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void show_dialog(std::unique_ptr<Dlg> d) {
    State& st = S();
    for (auto& o : dialogs()) {
        if (o->kind == d->kind) {   // already open: to the front
            ShowWindow(o->hwnd, SW_RESTORE);
            SetForegroundWindow(o->hwnd);
            return;
        }
    }
    d->dpi = st.hwnd ? static_cast<int>(GetDpiForWindow(st.hwnd)) : 96;
    if (d->dpi <= 0) d->dpi = 96;
    d->font = CreateFontW(-MulDiv(9, d->dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                          CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    d->mono = CreateFontW(-MulDiv(9, d->dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                          CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
    const SIZE cs = d->size();
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT r = {0, 0, MulDiv(cs.cx, d->dpi, 96), MulDiv(cs.cy, d->dpi, 96)};
    AdjustWindowRectExForDpi(&r, style, FALSE, WS_EX_CONTROLPARENT, static_cast<UINT>(d->dpi));
    RECT g = {100, 100, 0, 0};
    if (st.hwnd) GetWindowRect(st.hwnd, &g);
    const int offset = MulDiv(40 + 24 * static_cast<int>(dialogs().size()), d->dpi, 96);
    const std::string game = game::info().short_title && *game::info().short_title ? game::info().short_title : "Game";
    const std::wstring caption = widen(game) + L" - " + d->title();
    Dlg* raw = d.get();
    dialogs().push_back(std::move(d));
    HWND h = CreateWindowExW(WS_EX_CONTROLPARENT, L"NxrtMenuDialog", caption.c_str(), style, g.left + offset, g.top + offset, r.right - r.left,
                             r.bottom - r.top, nullptr, nullptr, GetModuleHandleW(nullptr), raw);
    if (!h) {
        NXLOG_W(kTag, "cannot open the %s window (%lu)", narrow(raw->title()).c_str(), GetLastError());
        dialogs().pop_back();
        return;
    }
    raw->build();
    ShowWindow(h, SW_SHOW);
    SetForegroundWindow(h);
}

// ---- Gamepad ---------------------------------------------------------------------------------------------------------------------------------
class GamepadDlg final : public Dlg {
    enum { kPads = 100, kTest, kLabel, kPosition, kDzL, kDzR, kTrig, kRumbleK, kGyroK, kRumble, kGyro, kList, kRebind, kReset, kStatus,
           kDzLV, kDzRV, kTrigV, kRumbleV, kGyroV };
    int rebind = -1;          // the Switch button row waiting for a pad button
    std::string last_pads;

public:
    const wchar_t* title() const override { return L"Gamepad"; }
    SIZE size() const override { return {620, 736}; }
    void build() override {
        const Settings s = host().settings();
        label(L"Connected controllers (every one drives the same Switch pad):", 12, 10, 590);
        add(L"STATIC", L"", SS_LEFT | SS_SUNKEN, 12, 32, 594, 58, kPads);
        button(L"Test rumble", kTest, 12, 96, 120);
        label(L"Face buttons:", 12, 132, 100);
        add(L"BUTTON", L"By label (Xbox A = Switch A)", BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, 120, 130, 230, 22, kLabel);
        add(L"BUTTON", L"By position (bottom = Switch B)", BS_AUTORADIOBUTTON, 360, 130, 240, 22, kPosition);
        SendMessageW(item(s.pad.label_abxy ? kLabel : kPosition), BM_SETCHECK, BST_CHECKED, 0);
        struct Row { int id, val; const wchar_t* name; int lo, hi, v; };
        const Row rows[] = {
            {kDzL, kDzLV, L"Left stick dead zone", 0, 50, static_cast<int>(std::lround(s.pad.stick_deadzone * 100))},
            {kDzR, kDzRV, L"Right stick dead zone", 0, 50, static_cast<int>(std::lround(s.pad.right_deadzone * 100))},
            {kTrig, kTrigV, L"Trigger threshold", 5, 100, static_cast<int>(std::lround(s.pad.trigger_threshold * 100))},
            {kRumbleK, kRumbleV, L"Rumble strength", 0, 200, static_cast<int>(std::lround(s.pad.rumble_scale * 100))},
            {kGyroK, kGyroV, L"Gyro scale", 10, 400, static_cast<int>(std::lround(s.pad.gyro_scale * 100))},
        };
        int y = 160;
        for (const Row& r : rows) {
            label(r.name, 12, y + 4, 170);
            track(r.id, 186, y, 330, r.lo, r.hi, r.v);
            add(L"STATIC", L"", SS_LEFT, 524, y + 4, 80, 20, r.val);
            y += 32;
        }
        check(L"Rumble", kRumble, 12, y, 120, s.pad.rumble);
        check(L"Gyro (motion sensor)", kGyro, 150, y, 220, s.pad.gyro);
        y += 34;
        label(L"Button mapping: select a Switch button, choose Rebind and press a controller button (the two swap jobs).", 12, y, 594, 36);
        y += 38;
        HWND lv = add(WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_BORDER | WS_TABSTOP, 12, y, 594, 280, kList);
        ListView_SetExtendedListViewStyle(lv, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
        LVCOLUMNW c = {};
        c.mask = LVCF_TEXT | LVCF_WIDTH;
        c.cx = sc(140);
        c.pszText = const_cast<wchar_t*>(L"Switch button");
        lv_insert_column(lv, 0, &c);
        c.cx = sc(430);
        c.pszText = const_cast<wchar_t*>(L"Pressed by");
        lv_insert_column(lv, 1, &c);
        for (int i = 0; i < kSwitchBtnCount; ++i) {
            const std::wstring n = widen(kSwitchBtns[i].name);
            LVITEMW it = {};
            it.mask = LVIF_TEXT;
            it.iItem = i;
            it.pszText = const_cast<wchar_t*>(n.c_str());
            lv_insert_item(lv, &it);
        }
        ListView_SetItemState(lv, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        y += 288;
        button(L"Rebind...", kRebind, 12, y, 120);
        button(L"Built-in layout", kReset, 140, y, 140);
        add(L"STATIC", L"", SS_LEFT, 290, y + 4, 316, 40, kStatus);
        refresh();
        SetTimer(hwnd, 1, 100, nullptr);
    }
    void refresh() {
        const Settings s = host().settings();
        for (const int id : {kDzLV, kDzRV, kTrigV}) {
            const int t = id == kDzLV ? kDzL : id == kDzRV ? kDzR : kTrig;
            text(id, std::to_string(pos(t)) + " %");
        }
        text(kRumbleV, std::to_string(pos(kRumbleK)) + " %");
        char g[16];
        std::snprintf(g, sizeof g, "x%.2f", pos(kGyroK) / 100.0);
        text(kGyroV, g);
        HWND lv = item(kList);
        for (int i = 0; i < kSwitchBtnCount; ++i) {
            uint32_t by = 0;
            for (int src = 0; src < kPadSrcCount; ++src)
                if (pad_effective_buttons(static_cast<PadSrc>(src), s.pad) & hid::btn(kSwitchBtns[i].bit)) by |= 1u << src;
            const std::wstring t = by ? widen(sources_text(by)) : std::wstring(L"(no controller button: keyboard only)");
            lv_set_text(lv, i, 1, const_cast<wchar_t*>(t.c_str()));
        }
    }
    void on_activate() override {
        const Settings s = host().settings();   // changed elsewhere (the in-game page, the menu bar)
        SendMessageW(item(kRumble), BM_SETCHECK, s.pad.rumble ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(item(kGyro), BM_SETCHECK, s.pad.gyro ? BST_CHECKED : BST_UNCHECKED, 0);
        refresh();
    }
    void on_command(int id, int code) override {
        if (code != BN_CLICKED) return;
        switch (id) {
            case kTest: host().test_rumble(); break;
            case kLabel: change([](Settings& s) { s.pad.label_abxy = true; }); refresh(); break;
            case kPosition: change([](Settings& s) { s.pad.label_abxy = false; }); refresh(); break;
            case kRumble: { const bool on = checked(kRumble); change([on](Settings& s) { s.pad.rumble = on; }); break; }
            case kGyro: { const bool on = checked(kGyro); change([on](Settings& s) { s.pad.gyro = on; }); break; }
            case kRebind: {
                const int row = ListView_GetNextItem(item(kList), -1, LVNI_SELECTED);
                if (row < 0) { text(kStatus, "Select a Switch button first."); break; }
                rebind = row;
                host().begin_pad_capture();
                text(kStatus, std::string("Press a controller button (or pull a trigger) for Switch ") + kSwitchBtns[row].name + " ...");
                break;
            }
            case kReset:
                change([](Settings& s) { for (uint64_t& v : s.pad.remap) v = kPadDefault; });
                text(kStatus, "Every controller button is back on the built-in layout.");
                refresh();
                break;
            default: break;
        }
    }
    void on_scroll(int id, int code) override {
        refresh();
        if (code != TB_ENDTRACK) return;   // applied when the slider is let go
        const double v = pos(id) / 100.0;
        switch (id) {
            case kDzL: change([v](Settings& s) { s.pad.stick_deadzone = v; }); break;
            case kDzR: change([v](Settings& s) { s.pad.right_deadzone = v; }); break;
            case kTrig: change([v](Settings& s) { s.pad.trigger_threshold = v; }); break;
            case kRumbleK: change([v](Settings& s) { s.pad.rumble_scale = v; }); break;
            case kGyroK: change([v](Settings& s) { s.pad.gyro_scale = v; }); break;
            default: break;
        }
    }
    void on_timer() override {
        const LiveStatus live = host().status();
        std::string t;
        for (size_t i = 0; i < live.pad_list.size(); ++i) {
            const PadLive& p = live.pad_list[i];
            static const char* const kKinds[] = {"gamepad", "Xbox", "PlayStation", "Nintendo"};
            const std::string pressed = sources_text(p.sources);
            char b[512];
            std::snprintf(b, sizeof b, "%zu. %s (%s, gyro %s, rumble %s): %s\r\n", i + 1, p.name.c_str(), kKinds[std::clamp(p.kind, 0, 3)], p.gyro ? "yes" : "no",
                          p.rumble ? "yes" : "no", pressed.empty() ? "nothing pressed" : pressed.c_str());
            t += b;
        }
        if (t.empty()) t = "No controller connected. Xbox, PlayStation, Switch Pro and most other controllers work as soon as they are plugged in.";
        if (t != last_pads) {
            last_pads = t;
            text(kPads, t);
        }
        if (rebind >= 0) {
            PadSrc src;
            if (host().pad_capture_result(&src)) {
                const uint64_t bit = hid::btn(kSwitchBtns[rebind].bit);
                change([src, bit](Settings& s) { pad_assign(&s.pad, src, bit); });
                text(kStatus, std::string(kPadSrcNames[static_cast<int>(src)]) + " now presses Switch " + kSwitchBtns[rebind].name + ".");
                rebind = -1;
                refresh();
            } else if (!host().pad_capturing()) {
                text(kStatus, "No controller button was pressed: nothing changed.");
                rebind = -1;
            }
        }
    }
};

// ---- Keyboard and mouse ------------------------------------------------------------------------------------------------------------------------
class KeyboardDlg final : public Dlg {
    enum { kCtx = 200, kList, kRebind, kAdd, kAct, kRemove, kFactory, kStatus };
    Ctx ctx = Ctx::All;
    std::vector<int> rows;    // list row -> index into settings().bindings
    int waiting = -1;         // binding index being rebound; -2 = a new binding of the action in kAct
    Act add_act = Act::BtnA;

public:
    const wchar_t* title() const override { return L"Keyboard and mouse bindings"; }
    SIZE size() const override { return {620, 560}; }
    void build() override {
        label(L"Context:", 12, 14, 60);
        std::vector<std::wstring> ctxs;
        for (int c = 0; c < kCtxCount; ++c) ctxs.push_back(widen(ctx_name(static_cast<Ctx>(c))));
        combo(kCtx, 76, 10, 160, ctxs, static_cast<int>(ctx));
        if (const GamePlay* gp = game_play(); !gp || !gp->frame)
            label(widen(std::string("This game always uses the '") + ctx_name(Ctx::Menu) + "' bindings plus the '" + ctx_name(Ctx::All) + "' layer."), 246, 14, 360, 34);
        HWND lv = add(WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_BORDER | WS_TABSTOP, 12, 52, 594, 380, kList);
        ListView_SetExtendedListViewStyle(lv, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
        const wchar_t* cols[] = {L"Input", L"Action", L"Trigger"};
        const int widths[] = {170, 260, 140};
        for (int i = 0; i < 3; ++i) {
            LVCOLUMNW c = {};
            c.mask = LVCF_TEXT | LVCF_WIDTH;
            c.cx = sc(widths[i]);
            c.pszText = const_cast<wchar_t*>(cols[i]);
            lv_insert_column(lv, i, &c);
        }
        button(L"Rebind...", kRebind, 12, 440, 110);
        button(L"Remove", kRemove, 130, 440, 90);
        button(L"Factory bindings", kFactory, 476, 440, 130);
        label(L"Add a key for:", 12, 480, 100);
        std::vector<std::wstring> acts;
        for (int a = 1; a < kActCount; ++a) acts.push_back(widen(act_info(static_cast<Act>(a)).name));
        combo(kAct, 116, 476, 230, acts, static_cast<int>(Act::BtnA) - 1);
        button(L"Add...", kAdd, 354, 475, 90);
        add(L"STATIC", L"Choose Rebind or Add, then press the key, mouse button or wheel in the game window; Escape cancels.", SS_LEFT, 12, 512, 594, 40, kStatus);
        fill();
        SetTimer(hwnd, 1, 100, nullptr);
    }
    void fill() {
        const Settings s = host().settings();
        HWND lv = item(kList);
        const int keep = ListView_GetNextItem(lv, -1, LVNI_SELECTED);
        ListView_DeleteAllItems(lv);
        rows.clear();
        for (size_t i = 0; i < s.bindings.size(); ++i) {
            const Binding& b = s.bindings[i];
            if (b.ctx != ctx) continue;
            const int r = static_cast<int>(rows.size());
            rows.push_back(static_cast<int>(i));
            const std::wstring in = widen(input_name(b.in)), act = widen(b.act == Act::None ? "none (input off)" : act_info(b.act).name);
            std::string trig = trig_name(b.trig);
            if (b.ms) trig += " " + std::to_string(b.ms) + " ms";
            const std::wstring tw = widen(trig);
            LVITEMW it = {};
            it.mask = LVIF_TEXT;
            it.iItem = r;
            it.pszText = const_cast<wchar_t*>(in.c_str());
            lv_insert_item(lv, &it);
            lv_set_text(lv, r, 1, const_cast<wchar_t*>(act.c_str()));
            lv_set_text(lv, r, 2, const_cast<wchar_t*>(tw.c_str()));
        }
        if (keep >= 0 && keep < static_cast<int>(rows.size())) ListView_SetItemState(lv, keep, LVIS_SELECTED, LVIS_SELECTED);
    }
    void on_activate() override {
        if (waiting == -1) fill();
    }
    void start_capture(int idx) {
        waiting = idx;
        host().begin_capture();
        text(kStatus, "Press the key, mouse button or wheel in the game window now (Escape cancels) ...");
        if (S().hwnd) SetForegroundWindow(S().hwnd);   // the input goes to the game window, where play mode takes it
    }
    void on_command(int id, int code) override {
        if (id == kCtx && code == CBN_SELCHANGE) {
            ctx = static_cast<Ctx>(sel(kCtx));
            fill();
            return;
        }
        if (code != BN_CLICKED) return;
        const int row = ListView_GetNextItem(item(kList), -1, LVNI_SELECTED);
        switch (id) {
            case kRebind:
                if (row < 0 || row >= static_cast<int>(rows.size())) text(kStatus, "Select a binding first.");
                else start_capture(rows[static_cast<size_t>(row)]);
                break;
            case kAdd:
                add_act = static_cast<Act>(sel(kAct) + 1);
                start_capture(-2);
                break;
            case kRemove:
                if (row >= 0 && row < static_cast<int>(rows.size())) {
                    const int idx = rows[static_cast<size_t>(row)];
                    change([idx](Settings& s) {
                        if (idx < static_cast<int>(s.bindings.size())) s.bindings.erase(s.bindings.begin() + idx);
                    });
                    fill();
                }
                break;
            case kFactory:
                change([](Settings& s) { s.bindings = default_settings().bindings; });
                text(kStatus, "The factory bindings are back.");
                fill();
                break;
            default: break;
        }
    }
    void on_timer() override {
        if (waiting == -1) return;
        InputId id;
        if (host().capture_result(&id)) {
            if (host().reserved_key(id) || (id.kind == InKind::Key && id == host().settings().fps.key)) {
                text(kStatus, input_name(id) + " is taken by the menu or the frame rate counter. Press another one (Escape cancels) ...");
                host().begin_capture();
                return;
            }
            const int idx = waiting;
            const Ctx c = ctx;
            const Act a = add_act;
            change([idx, id, c, a](Settings& s) {
                if (idx >= 0 && idx < static_cast<int>(s.bindings.size())) {
                    s.bindings[static_cast<size_t>(idx)].in = id;
                } else if (idx == -2) {
                    Binding b;
                    b.in = id;
                    b.ctx = c;
                    b.act = a;
                    s.bindings.push_back(b);
                }
            });
            text(kStatus, input_name(id) + " bound.");
            waiting = -1;
            fill();
            SetForegroundWindow(hwnd);
        } else if (!host().capturing()) {
            text(kStatus, "Cancelled.");
            waiting = -1;
            SetForegroundWindow(hwnd);
        }
    }
};

// ---- More graphics settings --------------------------------------------------------------------------------------------------------------------
class GraphicsDlg final : public Dlg {
    enum { kLod = 300, kSharp, kContrast, kSat, kBlack, kGamma, kNeutral, kVivid, kRes, kScale, kAstc, kFast, kWaits, kPrewarm, kW, kH, kSize,
           kLodV, kSharpV, kContrastV, kSatV, kBlackV, kGammaV };
    struct Opt { const char* value; const wchar_t* label; };
    static const std::vector<Opt>& res() {
        static const std::vector<Opt> v = {{"", L"The mode's default (1080p docked, 720p handheld)"}, {"1280x720", L"1280 x 720"}, {"1920x1080", L"1920 x 1080"},
                                           {"2560x1440", L"2560 x 1440"}, {"3840x2160", L"3840 x 2160"}};
        return v;
    }
    static const std::vector<Opt>& scales() {
        static const std::vector<Opt> v = {{"10", L"Level 0 (full quality)"}, {"20", L"Level 1"}, {"30", L"Level 2"}};
        return v;
    }
    static const std::vector<Opt>& astc() {
        static const std::vector<Opt> v = {{"fast", L"Fast"}, {"high", L"High (sharper, built in the background)"}, {"uncompressed", L"Uncompressed (much video memory)"}};
        return v;
    }
    static int index_of(const std::vector<Opt>& v, const std::string& value) {
        for (size_t i = 0; i < v.size(); ++i)
            if (value == v[i].value) return static_cast<int>(i);
        return 0;
    }
    static std::vector<std::wstring> labels(const std::vector<Opt>& v) {
        std::vector<std::wstring> o;
        for (const Opt& x : v) o.push_back(x.label);
        return o;
    }

public:
    const wchar_t* title() const override { return L"More graphics settings"; }
    SIZE size() const override { return {600, 560}; }
    void build() override {
        const Settings s = host().settings();
        struct Row { int id, val; const wchar_t* name; int lo, hi, v; };
        const extras::PictureSettings& c = s.extras.picture;
        const Row rows[] = {
            {kLod, kLodV, L"Texture LOD bias (live)", -200, 200, static_cast<int>(std::lround(s.display.lod_bias * 100))},
            {kSharp, kSharpV, L"FSR 1 / NIS sharpness (live)", 0, 100, static_cast<int>(std::lround(s.extras.upscale.sharpness * 100))},
            {kContrast, kContrastV, L"Contrast (live)", 50, 150, static_cast<int>(std::lround(c.contrast * 100))},
            {kSat, kSatV, L"Saturation (live)", 0, 200, static_cast<int>(std::lround(c.saturation * 100))},
            {kBlack, kBlackV, L"Black level (live)", -50, 25, static_cast<int>(std::lround(c.black * 100))},
            {kGamma, kGammaV, L"Gamma (live)", 50, 200, static_cast<int>(std::lround(c.gamma * 100))},
        };
        int y = 12;
        for (const Row& r : rows) {
            label(r.name, 12, y + 4, 200);
            track(r.id, 214, y, 290, r.lo, r.hi, r.v);
            add(L"STATIC", L"", SS_LEFT, 512, y + 4, 76, 20, r.val);
            y += 32;
        }
        button(L"Neutral colour", kNeutral, 214, y, 130);
        button(L"Vivid colour", kVivid, 352, y, 130);
        y += 40;
        label(L"Output resolution (next start)", 12, y + 4, 200);
        combo(kRes, 214, y, 374, labels(res()), index_of(res(), config_value(s, "am.resolution", "")));
        y += 34;
        label(L"Graphics scaling level (next start)", 12, y + 4, 200);
        combo(kScale, 214, y, 374, labels(scales()), index_of(scales(), config_value(s, "am.graphics_scale", "10")));
        y += 34;
        label(L"Texture compression (next start)", 12, y + 4, 200);
        combo(kAstc, 214, y, 374, labels(astc()), index_of(astc(), s.display.astc_quality));
        y += 38;
        check(L"Faster GPU memory (more video memory; next start)", kFast, 12, y, 560, s.display.fast_gpu_memory);
        check(L"Fewer GPU pipeline waits (next start)", kWaits, 12, y + 26, 560, s.display.fewer_gpu_waits);
        check(L"Prepare shaders ahead (pipeline prewarm; next start)", kPrewarm, 12, y + 52, 560, s.display.shader_prewarm);
        y += 88;
        label(L"Window size (windowed):", 12, y + 4, 160);
        add(L"EDIT", widen(std::to_string(s.display.width)), ES_NUMBER | WS_BORDER | WS_TABSTOP, 176, y, 70, 24, kW);
        label(L"x", 252, y + 4, 12);
        add(L"EDIT", widen(std::to_string(s.display.height)), ES_NUMBER | WS_BORDER | WS_TABSTOP, 268, y, 70, 24, kH);
        button(L"Set size", kSize, 348, y - 1, 100);
        values();
    }
    void values() {
        char b[32];
        std::snprintf(b, sizeof b, "%+.2f", pos(kLod) / 100.0);
        text(kLodV, b);
        for (const int id : {kSharp, kContrast, kSat, kGamma}) {
            std::snprintf(b, sizeof b, "%.2f", pos(id) / 100.0);
            text(id == kSharp ? kSharpV : id == kContrast ? kContrastV : id == kSat ? kSatV : kGammaV, b);
        }
        std::snprintf(b, sizeof b, "%+.2f", pos(kBlack) / 100.0);
        text(kBlackV, b);
    }
    extras::PictureSettings picture_now() const {
        extras::PictureSettings p = host().settings().extras.picture;
        p.contrast = pos(kContrast) / 100.0;
        p.saturation = pos(kSat) / 100.0;
        p.black = pos(kBlack) / 100.0;
        p.gamma = pos(kGamma) / 100.0;
        return p;
    }
    void set_picture_sliders(const extras::PictureSettings& p) {
        SendMessageW(item(kContrast), TBM_SETPOS, TRUE, std::lround(p.contrast * 100));
        SendMessageW(item(kSat), TBM_SETPOS, TRUE, std::lround(p.saturation * 100));
        SendMessageW(item(kBlack), TBM_SETPOS, TRUE, std::lround(p.black * 100));
        SendMessageW(item(kGamma), TBM_SETPOS, TRUE, std::lround(p.gamma * 100));
        values();
    }
    void on_scroll(int id, int code) override {
        values();
        const bool colour = id == kContrast || id == kSat || id == kBlack || id == kGamma;
        if (colour) extras::set_picture_live(picture_now());   // shows while it is dragged
        if (code != TB_ENDTRACK) return;
        const double v = pos(id) / 100.0;
        if (id == kLod) change([v](Settings& s) { s.display.lod_bias = v; });
        else if (id == kSharp) change([v](Settings& s) { s.extras.upscale.sharpness = v; });
        else if (colour) {
            const extras::PictureSettings p = picture_now();
            change([p](Settings& s) { s.extras.picture = p; });
        }
    }
    void on_command(int id, int code) override {
        if (code == CBN_SELCHANGE) {
            const int i = sel(id);
            if (id == kRes && i >= 0) change([i](Settings& s) { config_store(s, "am.resolution", res()[static_cast<size_t>(i)].value, "", false); });
            if (id == kScale && i >= 0) change([i](Settings& s) { config_store(s, "am.graphics_scale", scales()[static_cast<size_t>(i)].value, "10", false); });
            if (id == kAstc && i >= 0) change([i](Settings& s) { s.display.astc_quality = astc()[static_cast<size_t>(i)].value; });
            return;
        }
        if (code != BN_CLICKED) return;
        switch (id) {
            case kNeutral:
            case kVivid: {
                extras::PictureSettings p;
                extras::picture_preset(id == kVivid ? "vivid" : "neutral", &p);
                set_picture_sliders(p);
                extras::set_picture_live(p);
                change([p](Settings& s) { s.extras.picture = p; });
                break;
            }
            case kFast: { const bool on = checked(kFast); change([on](Settings& s) { s.display.fast_gpu_memory = on; }); break; }
            case kWaits: { const bool on = checked(kWaits); change([on](Settings& s) { s.display.fewer_gpu_waits = on; }); break; }
            case kPrewarm: { const bool on = checked(kPrewarm); change([on](Settings& s) { s.display.shader_prewarm = on; }); break; }
            case kSize: {
                wchar_t b[16];
                GetWindowTextW(item(kW), b, 16);
                const int w = std::clamp(_wtoi(b), 320, 16384);
                GetWindowTextW(item(kH), b, 16);
                const int h = std::clamp(_wtoi(b), 320, 16384);
                change([w, h](Settings& s) {
                    s.display.width = w;
                    s.display.height = h;
                });
                break;
            }
            default: break;
        }
    }
};

// ---- Debug info and Log ------------------------------------------------------------------------------------------------------------------------
class TextDlg : public Dlg {
protected:
    enum { kEdit = 400, kCopy, kPause, kLevel, kFollow, kOpen };
    std::string shown;
    void set_text(const std::string& t, bool to_end) {
        if (t == shown) return;
        shown = t;
        HWND e = item(kEdit);
        const int first = static_cast<int>(SendMessageW(e, EM_GETFIRSTVISIBLELINE, 0, 0));
        SetWindowTextW(e, widen(t).c_str());
        if (to_end) {
            const int len = GetWindowTextLengthW(e);
            SendMessageW(e, EM_SETSEL, len, len);
            SendMessageW(e, EM_SCROLLCARET, 0, 0);
        } else {
            SendMessageW(e, EM_LINESCROLL, 0, first);
        }
    }
    void make_edit(int y, int h) {
        HWND e = add(L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL | WS_VSCROLL | WS_HSCROLL | WS_BORDER | WS_TABSTOP, 12, y, 776, h, kEdit);
        SendMessageW(e, WM_SETFONT, reinterpret_cast<WPARAM>(mono), TRUE);
        SendMessageW(e, EM_SETLIMITTEXT, 4 * 1024 * 1024, 0);
    }
};

class DebugInfoDlg final : public TextDlg {
public:
    const wchar_t* title() const override { return L"Debug info"; }
    SIZE size() const override { return {800, 620}; }
    void build() override {
        button(L"Copy", kCopy, 12, 10, 90);
        check(L"Pause", kPause, 112, 12, 100, false);
        label(L"Refreshed every second. The same numbers as Debug > Copy a debug report.", 220, 14, 560);
        make_edit(44, 564);
        on_timer();
        SetTimer(hwnd, 1, 1000, nullptr);
    }
    void on_timer() override {
        if (!checked(kPause) && S().hooks.debug_report) set_text(S().hooks.debug_report(), false);
    }
    void on_command(int id, int code) override {
        if (id == kCopy && code == BN_CLICKED) set_clipboard(hwnd, shown);
    }
};

class LogDlg final : public TextDlg {
    uint64_t seq = ~0ull;
    int level = 2;

public:
    const wchar_t* title() const override { return L"Log"; }
    SIZE size() const override { return {800, 620}; }
    void build() override {
        label(L"Show:", 12, 14, 40);
        combo(kLevel, 56, 10, 200, {L"Errors", L"Warnings and errors", L"Info and above", L"Everything kept"}, level);
        check(L"Follow the end", kFollow, 270, 12, 140, true);
        button(L"Open the log file", kOpen, 420, 9, 150);
        make_edit(44, 564);
        on_timer();
        SetTimer(hwnd, 1, 500, nullptr);
    }
    void on_timer() override {
        std::vector<LogTailLine> lines;
        const uint64_t n = log_tail(&lines, kLogTailLines);
        if (n == seq) return;
        seq = n;
        std::string t;
        for (const LogTailLine& l : lines) {
            if (static_cast<int>(l.level) > level) continue;
            t += l.text;
            t += "\r\n";
        }
        set_text(t, checked(kFollow));
    }
    void on_command(int id, int code) override {
        if (id == kLevel && code == CBN_SELCHANGE) {
            level = sel(kLevel);
            seq = ~0ull;
            on_timer();
        } else if (id == kOpen && code == BN_CLICKED) {
            if (const char* p = log_path(); p && *p)
                ShellExecuteW(nullptr, L"open", L"notepad.exe", (L"\"" + full_path(p) + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
        }
    }
};

// ---- the UI thread -----------------------------------------------------------------------------------------------------------------------------
void message(const std::wstring& title, const std::wstring& text) {
    const std::string game = game::info().short_title && *game::info().short_title ? game::info().short_title : "Game";
    MessageBoxW(nullptr, text.c_str(), (widen(game) + L" - " + title).c_str(), MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
}

void open_ui(MenuAction a) {
    State& st = S();
    switch (a) {
        case MenuAction::GamepadDialog: { auto d = std::make_unique<GamepadDlg>(); d->kind = a; show_dialog(std::move(d)); break; }
        case MenuAction::KeyboardDialog: { auto d = std::make_unique<KeyboardDlg>(); d->kind = a; show_dialog(std::move(d)); break; }
        case MenuAction::GraphicsDialog: { auto d = std::make_unique<GraphicsDlg>(); d->kind = a; show_dialog(std::move(d)); break; }
        case MenuAction::DebugInfo: { auto d = std::make_unique<DebugInfoDlg>(); d->kind = a; show_dialog(std::move(d)); break; }
        case MenuAction::LogWindow: { auto d = std::make_unique<LogDlg>(); d->kind = a; show_dialog(std::move(d)); break; }
        case MenuAction::About: {
            const game::Info& gi = game::info();
            std::string t = std::string(gi.title ? gi.title : "") + " (" + (gi.version ? gi.version : "") + "), PC port\n\n";
            t += "Settings file: " + narrow(full_path(st.host->settings_path())) + "\n";
            if (const char* p = log_path(); p && *p) t += "Log: " + narrow(full_path(p)) + "\n";
            message(L"About", widen(t));
            break;
        }
        case MenuAction::MenuHelp:
            message(L"Keys and controls",
                    widen("Menu bar: Game, Input, Graphics, Audio, Debug (shown in windowed mode; Alt+Enter switches between windowed and fullscreen).\n"
                          "Settings change at once and are saved in the settings file; entries marked (next start) take effect when the game starts again.\n\n"
                          "Alt+Enter: windowed / fullscreen\n" + S().hooks.fps_key + ": frame rate counter\n" +
                          (S().hooks.capture_bug ? S().hooks.bug_key + ": capture a bug report\n" : std::string()) +
                          "F1, or View + Menu on a controller: the in-game menu, in fullscreen where the bar is hidden\n\n"
                          "The keyboard and mouse bindings are under Input > Keyboard and mouse bindings, the controller under Input > Gamepad."));
            break;
        default: break;
    }
}

void ui_main(HANDLE ready) {
    log_set_thread_name("menu-ui");
    // Visual styles for the controls (the exe has no manifest asking for common controls 6): the activation context shell32 carries.
    ACTCTXW ac = {sizeof ac};
    ac.dwFlags = ACTCTX_FLAG_RESOURCE_NAME_VALID | ACTCTX_FLAG_ASSEMBLY_DIRECTORY_VALID;
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    const std::wstring shell = std::wstring(sys) + L"\\shell32.dll";
    ac.lpSource = shell.c_str();
    ac.lpAssemblyDirectory = sys;
    ac.lpResourceName = MAKEINTRESOURCEW(124);
    HANDLE actx = CreateActCtxW(&ac);
    ULONG_PTR cookie = 0;
    if (actx != INVALID_HANDLE_VALUE) ActivateActCtx(actx, &cookie);
    INITCOMMONCONTROLSEX icc = {sizeof icc, ICC_BAR_CLASSES | ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    WNDCLASSEXW wc = {sizeof wc};
    wc.lpfnWndProc = dlg_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"NxrtMenuDialog";
    wc.hIcon = S().hwnd ? reinterpret_cast<HICON>(GetClassLongPtrW(S().hwnd, GCLP_HICON)) : nullptr;
    RegisterClassExW(&wc);
    MSG m;
    PeekMessageW(&m, nullptr, 0, 0, PM_NOREMOVE);   // the thread's queue exists before anyone posts to it
    SetEvent(ready);
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (!m.hwnd && m.message == kOpenMsg) {
            open_ui(static_cast<MenuAction>(m.wParam));
            continue;
        }
        bool done = false;
        std::vector<HWND> open;   // (a dialog may close while its message is handled: no iterator into the list across that)
        for (auto& d : dialogs())
            if (d->hwnd) open.push_back(d->hwnd);
        for (HWND h : open) {
            if (IsWindow(h) && IsDialogMessageW(h, &m)) {
                done = true;
                break;
            }
        }
        if (!done) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
}

void post_ui(MenuAction a) {
    State& st = S();
    std::call_once(st.ui_once, [&st] {
        HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        std::thread t([ready] { ui_main(ready); });
        st.ui_tid = GetThreadId(t.native_handle());
        t.detach();
        WaitForSingleObject(ready, 5000);
        CloseHandle(ready);
    });
    if (!PostThreadMessageW(st.ui_tid, kOpenMsg, static_cast<WPARAM>(a), 0)) NXLOG_W(kTag, "the menu's window thread did not take the request (%lu)", GetLastError());
}

}  // namespace

bool win_menu_install(void* hwnd, void* sdl_window, UiHost* host, const WinMenuHooks& hooks) {
    State& st = S();
    if (st.installed) return true;
    if (!hwnd || !sdl_window || !host) return false;
    st.hwnd = static_cast<HWND>(hwnd);
    st.win = static_cast<SDL_Window*>(sdl_window);
    st.host = host;
    st.hooks = hooks;
    MenuOptions o;
    o.display_options = hooks.display_options;
    o.upscaler_configured = hooks.upscaler_configured;
    o.bug_capture = static_cast<bool>(hooks.capture_bug);
    o.bug_key = hooks.bug_key;
    o.fps_key = hooks.fps_key;
    st.model = build_menu(o);
    st.bar = CreateMenu();
    for (const MenuItem& m : st.model) {
        AppendMenuW(st.bar, MF_POPUP | MF_STRING, reinterpret_cast<UINT_PTR>(build_popup(m.children)), item_label(m).c_str());
        const size_t amp = m.label.find('&');
        if (amp != std::string::npos && amp + 1 < m.label.size()) st.mnemonics += static_cast<wchar_t>(towlower(static_cast<unsigned char>(m.label[amp + 1])));
    }
    if (!SetWindowSubclass(st.hwnd, subclass_proc, 1, 0)) {
        NXLOG_W(kTag, "cannot subclass the game window (%lu): no menu bar", GetLastError());
        DestroyMenu(st.bar);
        st.bar = nullptr;
        return false;
    }
    st.installed = true;
    attach(platform::fullscreen_mode() == platform::FullscreenMode::Windowed);
    NXLOG_I(kTag, "menu bar ready (%d entries)", [&] {
        int n = 0;
        menu_for_each(st.model, [&](const MenuItem&) { ++n; });
        return n;
    }());
    return true;
}

void win_menu_frame() {
    State& st = S();
    if (!st.installed) return;
    const bool want = platform::fullscreen_mode() == platform::FullscreenMode::Windowed;
    if (want != st.attached.load()) attach(want);
}

bool win_menu_installed() { return S().installed; }
bool win_menu_attached() { return S().attached.load(); }

}  // namespace nxrt::play

#else   // not Windows: no menu bar

namespace nxrt::play {
bool win_menu_install(void*, void*, UiHost*, const WinMenuHooks&) { return false; }
void win_menu_frame() {}
bool win_menu_installed() { return false; }
bool win_menu_attached() { return false; }
}  // namespace nxrt::play

#endif
