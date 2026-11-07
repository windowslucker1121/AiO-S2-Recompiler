// SPDX-License-Identifier: GPL-3.0-or-later
// Shader setup before the game starts (nvn_backend.h shader_setup_before_start): the "Preparing shaders" screen. Step 1 translates every
// block of the setup's corpus that the shader cache has no results.tsv row for, through the cache's own on-demand translator (spirv_harness
// --serve, corpus requests); step 2 runs the shader preparation (pipeline parts of programs.bin, the caller's function because it needs a
// graphics device) when prepare.state does not say it finished. Both steps resume where a closed run stopped. A plain Win32 window shows
// the progress (no Vulkan: the game's window and device come later); with nothing to do there is no window.
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "nvn_shaders.h"
#include "nxrt/config.h"
#include "nxrt/diag.h"
#include "nxrt/log.h"
#include "nxrt/nvn_backend.h"

namespace nxrt::nvn {
namespace {

using Clock = std::chrono::steady_clock;

struct Progress {
    std::atomic<int> step{0};             // 1 translate, 2 prepare
    std::atomic<uint64_t> done{0}, total{0};
    std::atomic<bool> cancel{false}, finished{false};
    Clock::time_point step_start = Clock::now();
    int steps = 0;
    std::wstring title;
};
Progress g;
HWND g_wnd = nullptr;

std::vector<std::string> corpus_hashes(const std::string& corpus) {
    std::vector<std::string> out;
    for (const char* name : {"unique_all.tsv", "unique.tsv"}) {
        std::ifstream f(std::filesystem::path(corpus) / name);
        if (!f) continue;
        std::string line;
        std::getline(f, line);   // header
        while (std::getline(f, line))
            if (line.size() >= 16) out.push_back(line.substr(0, line.find('\t')));
        break;
    }
    return out;
}

// Finished, and no program of the list has become preparable since (translations added later, e.g. a re-run of the batch translation;
// usable_programs against the state's prepared + failed counts).
// Also not finished: made by an older scheme or for other raster flags, or its pipeline cache file is gone or lost most of its bytes.
bool prepare_finished(const std::filesystem::path& state, const std::filesystem::path& programs) {
    // ponytail: the state's GPU/driver key is not checked here (that needs a device); after a driver update the game links parts at run time
    std::string why;
    if (!be::prepare_state_complete(state.string(), &why)) {
        NXLOG_I("setup", "shader preparation needed: %s", why.c_str());
        return false;
    }
    std::ifstream f(state);
    std::string l;
    uint64_t covered = 0;
    while (std::getline(f, l)) {
        while (!l.empty() && l.back() == 13) l.pop_back();
        if (l.rfind("prepared: ", 0) == 0 || l.rfind("failed: ", 0) == 0) covered += std::strtoull(l.c_str() + l.find(' ') + 1, nullptr, 10);
    }
    std::vector<be::ProgramHashes> progs;
    std::string error;
    return !be::read_programs_file(programs.string(), &progs, &error) || be::usable_programs(progs) <= covered;
}

std::wstring thousands(uint64_t v) {
    std::wstring s = std::to_wstring(v);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(static_cast<size_t>(i), L",");
    return s;
}

void paint(HWND h) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);
    RECT rc;
    GetClientRect(h, &rc);
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
    HGDIOBJ old_bmp = SelectObject(mem, bmp);
    const int dpi = static_cast<int>(GetDpiForWindow(h));
    auto px = [dpi](int v) { return MulDiv(v, dpi, 96); };
    HBRUSH bg = CreateSolidBrush(RGB(24, 26, 32));
    FillRect(mem, &rc, bg);
    SetBkMode(mem, TRANSPARENT);
    HFONT big = CreateFontW(-px(22), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    HFONT small = CreateFontW(-px(14), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    const int step = g.step.load();
    const uint64_t done = g.done.load(), total = g.total.load();
    RECT t{px(24), px(20), rc.right - px(24), px(56)};
    HGDIOBJ old_font = SelectObject(mem, big);
    SetTextColor(mem, RGB(236, 238, 242));
    DrawTextW(mem, L"Preparing shaders", -1, &t, DT_LEFT | DT_SINGLELINE);
    SelectObject(mem, small);
    SetTextColor(mem, RGB(170, 176, 190));
    std::wstring what = step == 1 ? L"Translating the game's shaders for your graphics card" : L"Compiling shaders with your graphics driver";
    if (g.steps > 1) what = L"Step " + std::to_wstring(step == 1 || g.steps == 1 ? 1 : 2) + L" of " + std::to_wstring(g.steps) + L": " + what;
    RECT s{px(24), px(60), rc.right - px(24), px(84)};
    DrawTextW(mem, what.c_str(), -1, &s, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    RECT bar{px(24), px(98), rc.right - px(24), px(114)};
    HBRUSH track = CreateSolidBrush(RGB(48, 52, 62)), fill = CreateSolidBrush(RGB(92, 170, 120));
    FillRect(mem, &bar, track);
    if (total) {
        RECT f = bar;
        f.right = bar.left + static_cast<LONG>((bar.right - bar.left) * static_cast<double>(std::min(done, total)) / static_cast<double>(total));
        FillRect(mem, &f, fill);
    }
    std::wstring detail = thousands(done) + L" of " + thousands(total);
    const double secs = std::chrono::duration<double>(Clock::now() - g.step_start).count();
    if (done > 0 && done < total && secs > 5) {
        const int left = static_cast<int>(secs / static_cast<double>(done) * static_cast<double>(total - done));
        detail += left >= 120 ? L"  ·  about " + std::to_wstring(left / 60) + L" min left" : L"  ·  about " + std::to_wstring(std::max(left, 1)) + L" s left";
    }
    RECT d{px(24), px(124), rc.right - px(24), px(148)};
    DrawTextW(mem, detail.c_str(), -1, &d, DT_LEFT | DT_SINGLELINE);
    RECT n{px(24), px(156), rc.right - px(24), rc.bottom - px(8)};
    SetTextColor(mem, RGB(120, 126, 140));
    DrawTextW(mem, L"This happens once, so the game later runs without stutter. Closing this window stops it; the next start continues.", -1, &n,
              DT_LEFT | DT_WORDBREAK);
    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old_font);
    SelectObject(mem, old_bmp);
    for (HGDIOBJ o : {static_cast<HGDIOBJ>(big), static_cast<HGDIOBJ>(small), static_cast<HGDIOBJ>(bg), static_cast<HGDIOBJ>(track),
                      static_cast<HGDIOBJ>(fill), static_cast<HGDIOBJ>(bmp)})
        DeleteObject(o);
    DeleteDC(mem);
    EndPaint(h, &ps);
}

LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_PAINT: paint(h); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_TIMER: InvalidateRect(h, nullptr, FALSE); return 0;
        case WM_APP: DestroyWindow(h); return 0;   // the work is over
        case WM_CLOSE:
            if (MessageBoxW(h, L"Stop preparing shaders? The game starts once they are ready; the next start continues from here.",
                            g.title.c_str(), MB_YESNO | MB_ICONQUESTION) == IDYES)
                g.cancel = true;
            return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

HWND open_window() {
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);   // sharp text: paint() scales by the window's DPI
    WNDCLASSW wc{};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1));
    wc.lpszClassName = L"nxrt_shader_setup";
    RegisterClassW(&wc);
    const UINT dpi = GetDpiForSystem();
    RECT r{0, 0, MulDiv(620, static_cast<int>(dpi), 96), MulDiv(210, static_cast<int>(dpi), 96)};
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRectExForDpi(&r, style, FALSE, 0, dpi);
    const int ww = r.right - r.left, wh = r.bottom - r.top;
    HWND h = CreateWindowExW(0, wc.lpszClassName, g.title.c_str(), style, (GetSystemMetrics(SM_CXSCREEN) - ww) / 2,
                             (GetSystemMetrics(SM_CYSCREEN) - wh) / 2, ww, wh, nullptr, nullptr, wc.hInstance, nullptr);
    if (!h) return nullptr;
    ShowWindow(h, SW_SHOWNORMAL);
    SetTimer(h, 1, 200, nullptr);
    return h;
}

void begin_step(int step, uint64_t total) {
    g.done = 0;
    g.total = total;
    g.step_start = Clock::now();
    g.step = step;
}

std::vector<std::string> missing_corpus_blocks() {
    std::vector<std::string> missing;
    be::ShaderCache& cache = be::shader_cache();
    if (!cache.is_open() || !cache.translator_on() || cache.corpus_dir().empty()) return missing;
    for (std::string& h : corpus_hashes(cache.corpus_dir()))
        if (!cache.has_record(h)) missing.push_back(std::move(h));
    return missing;
}

void use_setup_translate_jobs() {
    // The setup may use every core but two: the translator's job count is read when the cache opens (on first use, after this).
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    if (!config_has("nvn", "shader_translate_jobs")) config_set("nvn", "shader_translate_jobs", std::to_string(hw > 3 ? hw - 2 : 1).c_str());
}

// Step 1 of the setup: every block in `missing` goes to the translator; progress until all are answered or progress() says stop.
void translate_blocks(const std::vector<std::string>& missing, const std::function<bool(uint64_t, uint64_t)>& progress, ShaderTranslateResult* r) {
    be::ShaderCache& cache = be::shader_cache();
    r->missing = missing.size();
    const uint64_t before = cache.translator_done();
    for (const std::string& h : missing) cache.translate_corpus_block(h);
    auto answered = [&] { return missing.size() - std::min<uint64_t>(missing.size(), cache.translator_pending()); };
    while (cache.translator_on() && cache.translator_pending() > 0) {
        if (progress && !progress(answered(), missing.size())) { r->cancelled = true; break; }
        Sleep(100);
    }
    r->answered = answered();
    r->with_spirv = cache.translator_done() - before;
    if (progress && !r->cancelled) progress(r->answered, missing.size());
    NXLOG_I("setup", "translation: %llu of %zu blocks answered (%llu with SPIR-V)%s", static_cast<unsigned long long>(r->answered), missing.size(),
            static_cast<unsigned long long>(r->with_spirv), r->cancelled ? ", stopped" : "");
    if (!cache.translator_on() && cache.translator_pending() > 0) {
        r->error = "the shader translator stopped before the end";
        NXLOG_W("setup", "%s", r->error.c_str());
    }
}

}  // namespace

bool translate_missing_shaders(const std::function<bool(uint64_t done, uint64_t total)>& progress, ShaderTranslateResult* r) {
    *r = ShaderTranslateResult{};
    use_setup_translate_jobs();
    be::ShaderCache& cache = be::shader_cache();
    if (!cache.is_open()) { r->error = "no shader cache is open ([nvn] shader_cache)"; return false; }
    if (!cache.translator_on()) { r->error = "the shader translator did not start ([nvn] shader_translator)"; return false; }
    if (cache.corpus_dir().empty()) { r->error = "no shader corpus ([nvn] shader_corpus)"; return false; }
    translate_blocks(missing_corpus_blocks(), progress, r);
    return r->error.empty();
}

bool shader_setup_before_start(const char* title, const ShaderSetupPrepare& prepare) {
    if (!config_get_bool("nvn", "setup_on_start", true)) return true;
    use_setup_translate_jobs();
    be::ShaderCache& cache = be::shader_cache();
    if (!cache.is_open()) return true;   // logged by the cache; the game runs with what it has

    const std::vector<std::string> missing = missing_corpus_blocks();
    const std::filesystem::path base = std::filesystem::path(cache.dir()).parent_path();
    std::error_code ec;
    const bool want_prepare = prepare && std::filesystem::is_regular_file(base / "programs.bin", ec) && !prepare_finished(base / "prepare.state", base / "programs.bin");
    if (missing.empty() && !want_prepare) return true;

    g.title.resize(static_cast<size_t>(MultiByteToWideChar(CP_UTF8, 0, title, -1, nullptr, 0)));
    MultiByteToWideChar(CP_UTF8, 0, title, -1, g.title.data(), static_cast<int>(g.title.size()));
    if (!g.title.empty()) g.title.pop_back();   // the terminator
    g.steps = (missing.empty() ? 0 : 1) + (want_prepare ? 1 : 0);
    NXLOG_I("setup", "preparing shaders before the start: %zu blocks to translate%s", missing.size(), want_prepare ? ", then the shader parts" : "");
    const bool watchdog = watchdog_running();   // no frames while this runs: not a freeze
    if (watchdog) watchdog_stop();
    g_wnd = open_window();
    bool ok = true;
    std::thread worker([&] {
        if (!missing.empty()) {
            begin_step(1, missing.size());
            ShaderTranslateResult tr;
            translate_blocks(missing, [](uint64_t done, uint64_t) {
                g.done = done;
                return !g.cancel.load();
            }, &tr);
        }
        if (want_prepare && !g.cancel) {
            begin_step(2, 0);
            ok = prepare([](uint64_t done, uint64_t total) {
                g.done = done;
                g.total = total;
                return !g.cancel.load();
            });
        }
        g.finished = true;
        if (g_wnd) PostMessageW(g_wnd, WM_APP, 0, 0);
    });
    if (g_wnd) {
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    worker.join();
    g_wnd = nullptr;
    if (watchdog) watchdog_start();
    if (g.cancel) {
        NXLOG_I("setup", "shader preparation stopped by the player: the game does not start");
        return false;
    }
    if (!ok) NXLOG_W("setup", "the shader preparation did not finish (see above); the game starts anyway");
    return true;
}

}  // namespace nxrt::nvn
