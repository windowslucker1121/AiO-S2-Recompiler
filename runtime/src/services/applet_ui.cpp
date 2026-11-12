// SPDX-License-Identifier: GPL-3.0-or-later
// Native windows for the system dialogs a game can ask for (services_api.h appletui; am.cpp decides when): the controller dialog (the game
// wants more pads than are connected), non-modal notices (an error the game shows, "online services are not available"), and the question
// whether to open a web page in the default browser. Each runs on the thread that calls it; am.cpp calls them from threads of its own, so
// the game waits for the applet as on the console while its window stays responsive.
#include <windows.h>
#include <shellapi.h>

#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include "nxrt/diag.h"
#include "nxrt/log.h"
#include "nxrt/services_api.h"

namespace nxrt::services::appletui {

namespace {

std::atomic<bool> g_windows{false};

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// ---- the controller dialog -------------------------------------------------------------------------------------------------------------
constexpr int kIdList = 100, kIdLabel = 101, kIdOk = IDOK, kIdFewer = 102, kIdCancel = IDCANCEL;
constexpr UINT_PTR kTimer = 1;
constexpr wchar_t kCtrlClass[] = L"nxrt_controller_applet";

struct CtrlState {
    ControllerRequest req;
    HWND list = nullptr, label = nullptr, ok = nullptr, fewer = nullptr;
    bool done = false;
    ControllerChoice choice = ControllerChoice::Cancel;
    int players = 1;
    std::wstring last_text;
};

std::atomic<PadLister> g_lister{nullptr};

// Connected players: player 1 is the keyboard and mouse with the first pad, every further pad one more player.
int connected_players(std::wstring* lines) {
    const std::vector<Pad> p = pads();
    const size_t n = p.size();
    if (lines) {
        lines->clear();
        *lines += L"Player 1: keyboard and mouse";
        if (n > 0) *lines += L" + " + widen(p[0].name);
        for (size_t i = 1; i < n; ++i) *lines += L"\nPlayer " + std::to_wstring(i + 1) + L": " + widen(p[i].name);
    }
    return n > 1 ? static_cast<int>(n) : 1;
}

void ctrl_refresh(CtrlState* cs) {
    std::wstring lines;
    const int have = connected_players(&lines);
    cs->players = have;
    if (lines != cs->last_text) {
        cs->last_text = lines;
        SendMessageW(cs->list, LB_RESETCONTENT, 0, 0);
        size_t start = 0;
        while (start <= lines.size()) {
            const size_t end = lines.find(L'\n', start);
            const std::wstring row = lines.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
            SendMessageW(cs->list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(row.c_str()));
            if (end == std::wstring::npos) break;
            start = end + 1;
        }
    }
    const int need = cs->req.min_players;
    std::wstring text = L"The game asks for " + std::to_wstring(need) + (need == 1 ? L" controller" : L" controllers") +
                        L" (one per player). Connect " + (have >= need ? L"no more: ready." : L"more controllers, then press OK.") +
                        L"\nConnected players: " + std::to_wstring(have);
    SetWindowTextW(cs->label, text.c_str());
    EnableWindow(cs->ok, have >= need);
    diag_progress();   // the game waits for the applet: not a freeze
}

void ctrl_finish(HWND hwnd, CtrlState* cs, ControllerChoice c) {
    if (cs->done) return;
    cs->choice = c;
    cs->done = true;
    KillTimer(hwnd, kTimer);
    DestroyWindow(hwnd);
}

LRESULT CALLBACK ctrl_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* cs = reinterpret_cast<CtrlState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_TIMER:
            if (cs) ctrl_refresh(cs);
            return 0;
        case WM_COMMAND:
            if (!cs) return 0;
            if (LOWORD(wp) == kIdOk && cs->players >= cs->req.min_players) ctrl_finish(hwnd, cs, ControllerChoice::Enough);
            else if (LOWORD(wp) == kIdFewer) ctrl_finish(hwnd, cs, ControllerChoice::Fewer);
            else if (LOWORD(wp) == kIdCancel) ctrl_finish(hwnd, cs, ControllerChoice::Cancel);
            return 0;
        case WM_CLOSE:
            if (cs) ctrl_finish(hwnd, cs, ControllerChoice::Cancel);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

// ---- notices ---------------------------------------------------------------------------------------------------------------------------
std::mutex g_notice_mu;
struct NoticeSlot {
    bool open = false;
    std::chrono::steady_clock::time_point last{};
    bool shown = false;
};
std::map<std::string, NoticeSlot> g_notices;

}  // namespace

void enable_windows(bool on) { g_windows.store(on); }
bool windows_enabled() { return g_windows.load(); }
void set_pad_lister(PadLister fn) { g_lister.store(fn); }
std::vector<Pad> pads() {
    const PadLister fn = g_lister.load();
    return fn ? fn() : std::vector<Pad>{};
}

ControllerChoice win32_controller(const ControllerRequest& r, int* players) {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = {sizeof wc};
        wc.lpfnWndProc = ctrl_proc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = kCtrlClass;
        registered = RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }
    const int W = 480, H = 280;
    const int sx = GetSystemMetrics(SM_CXSCREEN), sy = GetSystemMetrics(SM_CYSCREEN);
    const std::string title = (r.title.empty() ? std::string("Game") : r.title) + ": controllers";
    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_DLGMODALFRAME, kCtrlClass, widen(title).c_str(), WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                                (sx - W) / 2, (sy - H) / 2, W, H, nullptr, nullptr, inst, nullptr);
    if (!hwnd) {
        if (players) *players = connected_players(nullptr);
        return ControllerChoice::Fewer;
    }
    CtrlState cs;
    cs.req = r;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&cs));
    HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    auto child = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id) {
        HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), inst, nullptr);
        if (c) SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        return c;
    };
    cs.label = child(L"STATIC", L"", 0, 12, 10, W - 36, 44, kIdLabel);
    cs.list = child(L"LISTBOX", L"", WS_BORDER | WS_VSCROLL | LBS_NOSEL, 12, 60, W - 36, 120, kIdList);
    cs.ok = child(L"BUTTON", L"OK", WS_TABSTOP | BS_DEFPUSHBUTTON, W - 300, 196, 80, 28, kIdOk);
    cs.fewer = child(L"BUTTON", L"Continue anyway", WS_TABSTOP, W - 210, 196, 110, 28, kIdFewer);
    child(L"BUTTON", L"Cancel", WS_TABSTOP, W - 92, 196, 70, 28, kIdCancel);
    ctrl_refresh(&cs);
    SetTimer(hwnd, kTimer, 300, nullptr);
    SetForegroundWindow(hwnd);
    MSG m;
    while (!cs.done && GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (m.message == WM_KEYDOWN && m.wParam == VK_ESCAPE) { ctrl_finish(hwnd, &cs, ControllerChoice::Cancel); continue; }
        if (IsDialogMessageW(hwnd, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    if (players) *players = cs.players;
    return cs.choice;
}

void notice(const std::string& key, const std::string& title, const std::string& text, double min_gap_s, const std::string& settings_uri) {
    NXLOG_I("applet", "notice [%s]: %s", key.c_str(), text.c_str());
    if (!windows_enabled()) return;
    {
        std::lock_guard<std::mutex> lk(g_notice_mu);
        NoticeSlot& s = g_notices[key];
        const auto now = std::chrono::steady_clock::now();
        if (s.open) return;
        if (s.shown && min_gap_s >= 0 && std::chrono::duration<double>(now - s.last).count() < min_gap_s) return;
        if (s.shown && min_gap_s < 0) return;   // once per run
        s.open = true;
        s.shown = true;
        s.last = now;
    }
    std::thread([key, title, text, settings_uri] {
        std::wstring body = widen(text);
        UINT flags = MB_TOPMOST | MB_SETFOREGROUND | MB_ICONINFORMATION;
        if (!settings_uri.empty()) {
            body += L"\n\nOpen the Windows settings for this?";
            flags |= MB_YESNO;
        } else {
            flags |= MB_OK;
        }
        const int r = MessageBoxW(nullptr, body.c_str(), widen(title).c_str(), flags);
        if (r == IDYES && !settings_uri.empty())
            ShellExecuteW(nullptr, L"open", widen(settings_uri).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        std::lock_guard<std::mutex> lk(g_notice_mu);
        g_notices[key].open = false;
        g_notices[key].last = std::chrono::steady_clock::now();
    }).detach();
}

bool win32_open_url(const std::string& title, const std::string& url) {
    const bool web = url.rfind("https://", 0) == 0 || url.rfind("http://", 0) == 0;
    if (!web) return false;
    const std::wstring body = L"The game wants to show a web page:\n\n" + widen(url) +
                              L"\n\nOpen it in your web browser? The game continues as if the page was closed.";
    const int r = MessageBoxW(nullptr, body.c_str(), widen(title.empty() ? std::string("Web page") : title).c_str(),
                              MB_TOPMOST | MB_SETFOREGROUND | MB_ICONQUESTION | MB_YESNO | MB_DEFBUTTON2);
    if (r != IDYES) return false;
    ShellExecuteW(nullptr, L"open", widen(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return true;
}

}  // namespace nxrt::services::appletui
