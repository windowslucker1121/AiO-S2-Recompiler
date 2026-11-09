// SPDX-License-Identifier: GPL-3.0-or-later
// The game exe: the game (title and facts: game manifest, recomp/games/<id>/game.toml) as a native Windows process.
//
// This is the part of the process that Horizon's kernel and loader do before the first guest instruction:
//   1. map the NSOs of the ExeFS (the manifest's modules, entry module first) at the bases the lifter baked into the generated code,
//   2. hand the lifted module tables to the runtime (each library carries the ABI version it was built with),
//   3. relocate / link the modules,
//   4. turn the host main thread into the guest main thread (stack, TLS in TPIDRRO_EL0, main thread handle),
//   5. enter the first module (the entry module, rtld) at offset 0 with the registers the kernel gives a new process: x0 = 0, x1 = main thread handle.
// Everything after that (nn::init::Start, nninitStartup, nnMain) is the lifted game running on the runtime.
//
//   <game> [--game DIR] [--exefs DIR] [--ini FILE] [--log FILE] [--max-seconds N] [--link host|none] [--no-nvn] [--headless|--window]
//          [--instance NAME [--instance-dir DIR]]
// Environment: <ID>_EXEFS, <ID>_GAME (the manifest id uppercased: GAME_ENV below), NXRT_INSTANCE, NXRT_INSTANCE_DIR. Keys (section [boot]) are read
// through the runtime's config, see docs/S6.md.
//
// Named instances (--instance NAME): a second run of the game beside the first. Everything the run writes goes to its own folder (DIR, default
// <game folder>\instances\NAME): the log, crash / freeze / fatal reports, saves, controls.ini, the pipeline cache and key file (nxrt/instance.h).
// The game files, the shader cache and the BC7 cache stay shared. One run per game folder without an instance, one per instance folder; beside
// another run the start is refused (exit code 7) when the free memory would not hold one more ([boot] memory_per_run_gb, check_memory below).
//
// Installed layout (a release folder after the launcher's setup has run): when exefs\main sits next to the exe, or --game DIR / <ID>_GAME names a
// folder, that folder becomes the working directory, so everything the runtime reads relative to it (bc7_cache, shader_cache, romfs,
// controls.ini, the log) is found wherever the exe is started from (double click, shortcut, terminal), and the settings file is the
// manifest's ini (written by the launcher's setup). That ini next to the exe also makes the folder installed. Paths come from it, relative ones
// resolved against the game folder: [fs] exefs (default ./exefs), [fs] romfs (default ./romfs), [nvn] shader_cache (default
// ./shader_cache/spirv). --exefs / <ID>_EXEFS and the NXRT_* variables win over the ini. Without it (development runs from game-build or
// work/run) nothing changes, except that [fs] exefs in the ini named by --ini / NXRT_INI replaces the built-in ExeFS default.
// Development builds (not GAME_RELEASE) started with neither --game nor --exefs take the checkout's extracted game
// (game\<id>\updates\<newest>) as the game folder, and [nvn] shader_cache / shader_corpus / shader_translator default to the
// checkout's work\shaders\<id> and tools\shaders build: a plain `oot.exe` runs the game.
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <cwchar>
#include <cwctype>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "build_id.h"   // gen/: GAME_BUILD_COMMIT, GAME_BUILD_DIRTY (cmake/build_id.cmake)
#include "nxrt/bugcapture.h"
#include "nxrt/config.h"
#include "nxrt/diag.h"
#include "nxrt/dispatch.h"
#include "nxrt/kernel.h"
#include "nxrt/log.h"
#include "nxrt/game.h"
#include "nxrt/memory.h"
#include "nxrt/module.h"
#include "nxrt/gfx.h"
#include "nxrt/input_source.h"
#include "nxrt/instance.h"
#include "nxrt/nvn.h"
#include "nxrt/nvn_backend.h"
#include "nxrt/platform.h"
#include "nxrt/play.h"
#include "nxrt/nxmodule.h"
#include "nxrt/runtime.h"
#include "nxrt/timestep.h"

#include "game_manifest.h"   // generated from the game manifest: GAME_* names, files and the module list
#include GAME_HEADER      // the game module (runtime/games/<id>): every title-specific table the runtime uses

#ifdef GAME_AS_DLL
// Release model (b): this file is built into the runtime DLL (no game code). The game exe is linked on the player's PC from the
// libraries lifted out of their own dump plus src/stub.cpp, which passes the module tables to game_rt_main below.
#include "rt_api.h"
#else
extern "C" {
#define X(name, file, base) extern const NxModule nxmod_##name; extern const uint32_t nxmod_##name##_abi;
GAME_MODULES(X)
#undef X
}
#endif

#ifndef GAME_EXEFS_DEFAULT
#define GAME_EXEFS_DEFAULT "game/exefs"
#endif

#ifndef GAME_RELEASE_VERSION
#define GAME_RELEASE_VERSION "dev"     // set by the build: game-build/CMakeLists.txt, -DGAME_VERSION=0.1.0
#endif

namespace {

struct GameModule {
    const char* file;            // name of the NSO in the ExeFS
    const NxModule* nx;
    const uint32_t* abi;
    uint64_t base;               // must equal nx->base: the generated code has the address baked in
};

// Load order = the order the console maps them: the entry module first.
#ifdef GAME_AS_DLL
GameModule kModules[] = {      // filled by game_rt_main from the tables the game exe passes
#define X(name, file, base) {file, nullptr, nullptr, base},
    GAME_MODULES(X)
#undef X
};
#else
const GameModule kModules[] = {
#define X(name, file, base) {file, &nxmod_##name, &nxmod_##name##_abi, base},
    GAME_MODULES(X)
#undef X
};
#endif

std::unique_ptr<nxrt::gfx::Device> g_gfx_device;
std::unique_ptr<nxrt::gfx::Renderer> g_gfx_renderer;

// Exit code 1: start-up error; 7 (kExitRefused): refused to start beside another run (the same game folder or instance, or too little memory).
[[noreturn]] void die_code(int code, const char* fmt, const char* a = "", const char* b = "") {
    NXLOG_E("boot", fmt, a, b);
    nxrt::log_flush();
    std::fprintf(stderr, GAME_ID ": ");
    std::fprintf(stderr, fmt, a, b);
    std::fprintf(stderr, "\n");
#ifdef GAME_GUI_SUBSYSTEM
    char msg[1024];     // a double-clicked exe has no console: say what is wrong in a box
    std::snprintf(msg, sizeof msg, fmt, a, b);
    MessageBoxA(nullptr, msg, nxrt::game::window_title(), MB_OK | MB_ICONERROR);
#endif
    TerminateProcess(GetCurrentProcess(), static_cast<UINT>(code));
    for (;;) {}
}
[[noreturn]] void die(const char* fmt, const char* a = "", const char* b = "") { die_code(1, fmt, a, b); }
constexpr int kExitRefused = 7;

std::wstring widen_utf8(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
std::string narrow_utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}
std::wstring exe_dir() {
    std::wstring buf(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    buf.resize(n);
    const size_t slash = buf.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : buf.substr(0, slash);
}
bool file_exists(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
std::wstring full_path(const std::wstring& p) {
    std::wstring full(32768, L'\0');
    full.resize(GetFullPathNameW(p.c_str(), static_cast<DWORD>(full.size()), full.data(), nullptr));
    while (full.size() > 3 && (full.back() == L'\\' || full.back() == L'/')) full.pop_back();
    return full;
}
// A named mutex held for the life of the process: false when another process holds the one of this name. The name comes from a folder's
// full path (case-insensitive), so two spellings of one folder meet.
bool take_folder_mutex(const wchar_t* kind, const std::wstring& folder) {
    unsigned long long h = 1469598103934665603ull;
    for (wchar_t ch : full_path(folder)) h = (h ^ static_cast<unsigned long long>(std::towlower(ch))) * 1099511628211ull;
    const std::wstring name = L"Local\\" + widen_utf8(GAME_SAVE_FOLDER) + kind + std::to_wstring(h);
    return !(CreateMutexW(nullptr, FALSE, name.c_str()) && GetLastError() == ERROR_ALREADY_EXISTS);
}
// Whether a run holds the mutex take_folder_mutex(kind, folder) took (without creating it, so a run starting meanwhile is not refused).
bool folder_mutex_held(const wchar_t* kind, const std::wstring& folder) {
    unsigned long long h = 1469598103934665603ull;
    for (wchar_t ch : full_path(folder)) h = (h ^ static_cast<unsigned long long>(std::towlower(ch))) * 1099511628211ull;
    const std::wstring name = L"Local\\" + widen_utf8(GAME_SAVE_FOLDER) + kind + std::to_wstring(h);
    HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, name.c_str());
    if (!m) return false;
    CloseHandle(m);
    return true;
}
// A named instance's start: the pipeline caches other instance folders beside this one still hold although no run uses them (a crashed or
// killed run, or a build from before instances stopped writing the cache back): deleted, except in folders marked keep_pipeline_cache
// (nxrt/instance.h). Instance folders are named after their runs, so the mutex of a folder tells whether a run uses it.
void sweep_instance_pipeline_caches(const std::string& own_dir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path own = fs::u8path(own_dir);
    const fs::path parent = own.parent_path();
    if (parent.empty() || !fs::is_directory(parent, ec)) return;
    int n = 0;
    uint64_t bytes = 0;
    for (const fs::directory_entry& e : fs::directory_iterator(parent, ec)) {
        if (!e.is_directory(ec) || fs::equivalent(e.path(), own, ec)) continue;
        const fs::path cache = e.path() / "pipeline_cache.bin";
        if (!fs::exists(cache, ec) || fs::exists(e.path() / "keep_pipeline_cache", ec)) continue;
        if (folder_mutex_held(L"_instance_", e.path().wstring())) continue;
        const uint64_t size = fs::file_size(cache, ec);
        const uint64_t links = fs::hard_link_count(cache, ec);
        if (fs::remove(cache, ec)) {
            ++n;
            if (links == 1) bytes += size;   // a hard link to the shared cache frees nothing (unless it was the last name of an old one)
            fs::remove(e.path() / "pipeline_cache.bin.tmp", ec);
        }
    }
    if (n) {
        const std::u8string where = parent.u8string();
        NXLOG_I("instance", "removed the pipeline caches of %d instance folders no run uses (%.1f GB of private copies) in %s", n, bytes / 1e9,
                reinterpret_cast<const char*>(where.c_str()));
    }
}
// An instance name: 1-40 letters, digits, '-', '_' or '.', not starting with '.', and not "default" (what tools call the run without one).
bool valid_instance_name(const std::string& n) {
    if (n.empty() || n.size() > 40 || n[0] == '.' || _stricmp(n.c_str(), "default") == 0) return false;
    for (char c : n)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.')) return false;
    return true;
}

// The other running processes of this game exe (the same file name: builds of other checkouts count too): working set and commit.
struct OtherRun { DWORD pid; uint64_t working_set, private_bytes; };
std::vector<OtherRun> other_runs() {
    std::vector<OtherRun> out;
    wchar_t self[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, self, MAX_PATH);
    if (!n || n >= MAX_PATH) return out;
    const wchar_t* slash = std::wcsrchr(self, L'\\');
    const wchar_t* name = slash ? slash + 1 : self;
    const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W e{};
    e.dwSize = sizeof e;
    for (BOOL ok = Process32FirstW(snap, &e); ok; ok = Process32NextW(snap, &e)) {
        if (e.th32ProcessID == GetCurrentProcessId() || _wcsicmp(e.szExeFile, name) != 0) continue;
        OtherRun r{e.th32ProcessID, 0, 0};
        if (const HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, e.th32ProcessID)) {
            PROCESS_MEMORY_COUNTERS_EX pmc{};
            if (K32GetProcessMemoryInfo(h, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof pmc)) {
                r.working_set = pmc.WorkingSetSize;
                r.private_bytes = pmc.PrivateUsage;
            }
            CloseHandle(h);
        }
        out.push_back(r);
    }
    CloseHandle(snap);
    return out;
}

// Beside other runs of the game the start is refused when the machine's free memory would not hold one more: what is free now, minus
// what the other runs will still grow by (a run's need minus what it holds now: a run at the title holds a few GB, one in the first 3D
// scene 13-16 GB of working set), must be at least one run's need, for physical memory (working sets) and for the commit limit (private
// bytes; RAM plus page file). [boot] memory_per_run_gb (default 13; 0 turns the check off). A lone run is never refused (a 16 GB machine
// runs the game, paging).
void check_memory() {
    const double gb = nxrt::config_get_double("boot", "memory_per_run_gb", 13.0);
    if (!(gb > 0)) return;
    const std::vector<OtherRun> others = other_runs();
    if (others.empty()) return;
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof ms;
    if (!GlobalMemoryStatusEx(&ms)) return;
    constexpr double kGB = double(1ull << 30);
    const double need = gb * kGB, avail = static_cast<double>(ms.ullAvailPhys), commit = static_cast<double>(ms.ullAvailPageFile);
    double growth = 0, commit_growth = 0;
    std::string pids;
    for (const OtherRun& r : others) {
        growth += (std::max)(0.0, need - static_cast<double>(r.working_set));
        commit_growth += (std::max)(0.0, need - static_cast<double>(r.private_bytes));
        pids += (pids.empty() ? "" : ", ") + std::to_string(r.pid) + " at " + std::to_string(r.working_set >> 20) + " MB";
    }
    char msg[768];
    std::snprintf(msg, sizeof msg, "%zu other run%s of the game (pid %s); %.1f of %.1f GB memory free, %.1f GB of it still to be taken by %s; "
                  "%.1f GB commit free, %.1f GB of it still to be taken; %.0f GB needed by this run ([boot] memory_per_run_gb)",
                  others.size(), others.size() == 1 ? "" : "s", pids.c_str(), avail / kGB, static_cast<double>(ms.ullTotalPhys) / kGB, growth / kGB,
                  others.size() == 1 ? "it" : "them", commit / kGB, commit_growth / kGB, gb);
    if (avail - growth < need || commit - commit_growth < need)
        die_code(kExitRefused, "Not enough free memory for another run: %s. Close a run of the game first.", msg);
    NXLOG_I("boot", "memory check: %s: ok", msg);
}
// Absolute form of a path given on the command line, resolved against the current directory (before it is changed).
std::string absolute_utf8(const std::string& p) {
    const std::wstring w = widen_utf8(p);
    std::wstring buf(32768, L'\0');
    const DWORD n = GetFullPathNameW(w.c_str(), static_cast<DWORD>(buf.size()), buf.data(), nullptr);
    if (!n || n >= buf.size()) return p;
    buf.resize(n);
    return narrow_utf8(buf);
}

#ifdef GAME_DEV_ROOT
// Development builds (GAME_DEV_ROOT = the checkout, see CMakeLists.txt): the game extracted in this checkout (DEVELOPMENT_SETUP.md
// section 5), so a plain start of the exe (double click, no arguments) runs it as --game would: the newest
// game\<id>\updates\<version>\ with an ExeFS, else game\<id>\ itself. Empty when neither has exefs\main.
std::string dev_game_folder() {
    const std::wstring root = widen_utf8(GAME_DEV_ROOT "/game/" GAME_ID);
    std::wstring best;
    unsigned long best_version = 0;
    WIN32_FIND_DATAW fd;
    const HANDLE h = FindFirstFileW((root + L"\\updates\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !std::iswdigit(fd.cFileName[0])) continue;
            const unsigned long v = std::wcstoul(fd.cFileName, nullptr, 10);
            const std::wstring dir = root + L"\\updates\\" + fd.cFileName;
            if ((best.empty() || v > best_version) && file_exists(dir + L"\\exefs\\main")) {
                best = dir;
                best_version = v;
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (best.empty() && file_exists(root + L"\\exefs\\main")) best = root;
    return narrow_utf8(best);
}
// A [section] key the ini and the NXRT_* variables leave unset gets this checkout's file or folder, when it exists.
void dev_default(const char* section, const char* key, const char* path) {
    if (nxrt::config_has(section, key) || GetFileAttributesW(widen_utf8(path).c_str()) == INVALID_FILE_ATTRIBUTES) return;
    nxrt::config_set(section, key, path);
}
#endif

// game::Info::config_defaults: "[section]" lines and "key = value" lines (# or ; starts a comment). A key the ini or an NXRT_* variable sets keeps
// that value.
void apply_game_config_defaults(const char* text) {
    std::string section;
    for (const char* p = text; p && *p;) {
        const char* e = std::strchr(p, '\n');
        std::string line(p, e ? static_cast<size_t>(e - p) : std::strlen(p));
        p = e ? e + 1 : nullptr;
        auto trim = [](std::string s) {
            const size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
            return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
        };
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (line.front() == '[' && line.back() == ']') { section = trim(line.substr(1, line.size() - 2)); continue; }
        const size_t eq = line.find('=');
        if (eq == std::string::npos || section.empty()) continue;
        const std::string key = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
        // a default, not a user setting: the player's controls.ini [config] may still replace it (play::load, config_user_set)
        if (!key.empty() && !nxrt::config_has(section.c_str(), key.c_str())) nxrt::config_set_default(section.c_str(), key.c_str(), value.c_str());
    }
}

// [boot] press = "A@12,A@20:500,PLUS@30,A#1500": scripted controller presses on player 1 (button@seconds-after-start[:hold ms, default 300]).
// "A#1500:300" is keyed to the NVN back end's presented-frame count instead of seconds: a loaded machine plays the same sequence.
struct ScriptedPress { uint64_t bit; double at; int hold_ms; bool by_frame = false; };
std::shared_ptr<nxrt::hid::ManualInputSource> g_pad;
std::vector<ScriptedPress> g_presses;
DWORD WINAPI press_thread(LPVOID) {
    const ULONGLONG t0 = GetTickCount64();
    for (const ScriptedPress& p : g_presses) {
        if (p.by_frame) {
            while (!nxrt::nvn::backend_installed() || nxrt::nvn::backend_stats().presents < static_cast<uint64_t>(p.at)) Sleep(20);
            NXLOG_I("boot", "scripted press at present %.0f (%.1f s)", p.at, (GetTickCount64() - t0) / 1000.0);
        } else {
            const ULONGLONG due = t0 + static_cast<ULONGLONG>(p.at * 1000);
            for (ULONGLONG now = GetTickCount64(); now < due; now = GetTickCount64()) Sleep(static_cast<DWORD>(due - now > 50 ? 50 : due - now));
            NXLOG_I("boot", "scripted press at %.1f s", p.at);
        }
        g_pad->press(0, p.bit);
        Sleep(p.hold_ms);
        g_pad->release(0, p.bit);
    }
    return 0;
}
void parse_presses(const std::string& spec) {
    using namespace nxrt::hid;
    static const struct { const char* n; NpadButtonBit b; } names[] = {{"A", kBtnA}, {"B", kBtnB}, {"X", kBtnX}, {"Y", kBtnY}, {"PLUS", kBtnPlus},
        {"MINUS", kBtnMinus}, {"UP", kBtnUp}, {"DOWN", kBtnDown}, {"LEFT", kBtnLeft}, {"RIGHT", kBtnRight}, {"L", kBtnL}, {"R", kBtnR}, {"ZL", kBtnZL}, {"ZR", kBtnZR}};
    size_t i = 0;
    while (i < spec.size()) {
        size_t j = spec.find(',', i);
        if (j == std::string::npos) j = spec.size();
        const std::string item = spec.substr(i, j - i);
        i = j + 1;
        size_t at = item.find('@');
        const bool by_frame = at == std::string::npos && item.find('#') != std::string::npos;
        if (by_frame) at = item.find('#');
        if (at == std::string::npos) continue;
        const std::string name = item.substr(0, at);
        const size_t colon = item.find(':', at);
        ScriptedPress sp{0, std::atof(item.c_str() + at + 1), colon == std::string::npos ? 300 : std::atoi(item.c_str() + colon + 1), by_frame};
        for (const auto& e : names) if (name == e.n) sp.bit = btn(e.b);
        if (sp.bit) g_presses.push_back(sp);
    }
}

// [boot] screenshot_at = N (seconds after start, repeatable through screenshot_every): the renderer writes work/run/shot_<n>.png of the presented frame.
DWORD WINAPI shot_thread(LPVOID) {
    const int64_t first = nxrt::config_get_int("boot", "screenshot_at", 0), every = nxrt::config_get_int("boot", "screenshot_every", 0);
    std::string dir = nxrt::config_get_str("boot", "screenshot_dir", "");   // empty: the working directory
    if (!dir.empty() && dir.back() != '/' && dir.back() != '\\') dir += '/';
    int n = 0;
    Sleep(static_cast<DWORD>(first * 1000));
    for (;;) {
        if (g_gfx_renderer) {
            char name[64];
            std::snprintf(name, sizeof name, "shot_%d.png", n++);
            const std::string path = dir + name;
            g_gfx_renderer->request_screenshot(path);
            NXLOG_I("boot", "screenshot requested: %s", path.c_str());
        }
        if (every <= 0) break;
        Sleep(static_cast<DWORD>(every * 1000));
    }
    return 0;
}

// [prof] llvm_profile_at_s = N: for an exe linked with lifted libraries built with NX_PGO=generate (recomp/cmake/NxRecomp.cmake),
// writes the instrumentation counters every N seconds (file: LLVM_PROFILE_FILE, default default.profraw in the working folder;
// each write replaces the previous one). Without the profile runtime the call resolves to a stand-in that says so.
extern "C" int __llvm_profile_write_file(void);
extern "C" int nx_llvm_profile_absent(void) { return -1; }
#pragma comment(linker, "/alternatename:__llvm_profile_write_file=nx_llvm_profile_absent")
DWORD WINAPI llvm_profile_thread(LPVOID p) {
    const DWORD s = static_cast<DWORD>(reinterpret_cast<uintptr_t>(p));
    for (int n = 1;; ++n) {
        Sleep(s * 1000);
        const int r = __llvm_profile_write_file();
        NXLOG_I("boot", "llvm profile write %d: %s", n, r == 0 ? "ok" : "failed or no profile runtime linked");
        if (r != 0) return 0;
    }
}

DWORD WINAPI deadline_thread(LPVOID p) {
    const DWORD ms = static_cast<DWORD>(reinterpret_cast<uintptr_t>(p));
    Sleep(ms);
    NXLOG_W("boot", "max_seconds reached: the guest is still running; ending the process");
    nxrt::log_flush();
    if (nxrt::nvn::backend_installed()) {
        const nxrt::nvn::BackendStats b = nxrt::nvn::backend_stats();
        NXLOG_I("boot", "nvn back end at the deadline: submits %llu lists %llu draws %llu (skipped %llu) presents %llu pipelines %llu textures %llu (failed %llu) unsupported %llu",
                (unsigned long long)b.submits, (unsigned long long)b.lists_replayed, (unsigned long long)b.draws, (unsigned long long)b.draws_skipped,
                (unsigned long long)b.presents, (unsigned long long)b.pipelines_created, (unsigned long long)b.textures_created,
                (unsigned long long)b.textures_failed, (unsigned long long)b.unsupported_ops);
        NXLOG_I("boot", "nvn unsupported operations: %s", nxrt::nvn::backend_unsupported_report().c_str());
    }
    NXLOG_I("boot", "state at the deadline:\n%s", nxrt::summary().c_str());
    nxrt::log_flush();
    TerminateProcess(GetCurrentProcess(), 124);
    return 0;
}

// --prepare-shaders: no game, no window. A headless Vulkan device and the NVN back end's pipeline code compile the shader parts of every program the
// setup tool listed (shader_cache/programs.bin), so the driver has them in its cache before the first start. Progress goes to stdout as
// "prepare: <done> <total>" lines (the launcher reads them), the end as "prepare: done ..." / "prepare: ready" (already up to date).
// Exit code: 0 done or ready, 1 could not run, 3 cancelled.
// The shader preparation on a headless device of its own: --prepare-shaders and the shader setup before the start (shader_setup_before_start).
// False: it could not run (err) or prepare_shaders failed (pr->error).
bool run_shader_prepare(const nxrt::nvn::ShaderPrepareOptions& po, nxrt::nvn::ShaderPrepareResult* pr, std::string* err) {
    nxrt::gfx::DeviceDesc dd;
    dd.headless = 1;
    auto dev = nxrt::gfx::Device::create(dd, err);
    if (!dev) { *err = "graphics device: " + *err; return false; }
    nxrt::nvn::BackendDesc bd;
    bd.device = dev.get();
    bd.register_entry_points = false;
    if (!nxrt::nvn::install_backend(bd)) { *err = "the graphics back end did not start"; return false; }
    const bool ok = nxrt::nvn::prepare_shaders(po, pr);
    if (!ok) *err = pr->error;
    nxrt::nvn::shutdown_backend();
    dev.reset();
    return ok;
}

// --cancel-event NAME (the Initial Setup app): a named event; once it is set, --prepare-shaders and --translate-shaders stop at the next
// progress report and end as if the window had been closed (what is done stays done: the pipeline cache is saved, translations stay in
// the cache), instead of being killed.
HANDLE g_cancel_event = nullptr;
bool cancel_requested() { return g_cancel_event && WaitForSingleObject(g_cancel_event, 0) == WAIT_OBJECT_0; }

int prepare_shaders_main(bool if_needed, bool check_only, bool force, int threads, const std::string& programs) {
    nxrt::nvn::ShaderPrepareOptions po;
    po.only_if_needed = if_needed;
    po.check_only = check_only;
    po.force = force;
    po.programs_file = programs;
    po.threads = threads;
    auto last = std::chrono::steady_clock::now();
    po.progress = [&](uint64_t done, uint64_t total) {
        if (cancel_requested()) return false;
        const auto now = std::chrono::steady_clock::now();
        if (done < total && now - last < std::chrono::milliseconds(250)) return true;
        last = now;
        std::printf("prepare: %llu %llu\n", static_cast<unsigned long long>(done), static_cast<unsigned long long>(total));
        std::fflush(stdout);
        return true;
    };
    nxrt::nvn::ShaderPrepareResult pr;
    std::string err;
    const bool ok = run_shader_prepare(po, &pr, &err);
    int rc = 0;
    if (!ok) { std::printf("prepare: error %s\n", err.c_str()); rc = 1; }
    else if (pr.already_ready) std::printf("prepare: ready\n");
    else if (check_only) std::printf("prepare: needed %s\n", pr.needed_why.empty() ? "not prepared yet" : pr.needed_why.c_str());
    else std::printf("prepare: done programs=%llu prepared=%llu missing=%llu failed=%llu parts=%llu fast=%llu seconds=%.1f%s\n", static_cast<unsigned long long>(pr.programs),
                     static_cast<unsigned long long>(pr.prepared), static_cast<unsigned long long>(pr.missing_shaders), static_cast<unsigned long long>(pr.failed),
                     static_cast<unsigned long long>(pr.parts), static_cast<unsigned long long>(pr.fast_parts), pr.seconds, pr.cancelled ? " cancelled" : "");
    std::fflush(stdout);
    if (ok && pr.cancelled) rc = 3;
    nxrt::shutdown();
    return rc;
}

// --translate-shaders: no game, no window, no GPU. The first step of the "Preparing shaders" window: every block of the shader corpus that
// the shader cache has no row for goes through the on-demand translator ([nvn] shader_cache / shader_corpus / shader_translator).
// Output: "translate: <done> <total>" lines, then "translate: done missing=N answered=N spirv=N [cancelled]" or "translate: error <text>".
// Exit code: 0 done, 1 could not run, 3 cancelled.
int translate_shaders_main() {
    auto last = std::chrono::steady_clock::now();
    nxrt::nvn::ShaderTranslateResult tr;
    const bool ok = nxrt::nvn::translate_missing_shaders([&](uint64_t done, uint64_t total) {
        if (cancel_requested()) return false;
        const auto now = std::chrono::steady_clock::now();
        if (done < total && now - last < std::chrono::milliseconds(250)) return true;
        last = now;
        std::printf("translate: %llu %llu\n", static_cast<unsigned long long>(done), static_cast<unsigned long long>(total));
        std::fflush(stdout);
        return true;
    }, &tr);
    int rc = 0;
    if (!ok) { std::printf("translate: error %s\n", tr.error.c_str()); rc = 1; }
    else std::printf("translate: done missing=%llu answered=%llu spirv=%llu%s\n", static_cast<unsigned long long>(tr.missing),
                     static_cast<unsigned long long>(tr.answered), static_cast<unsigned long long>(tr.with_spirv), tr.cancelled ? " cancelled" : "");
    std::fflush(stdout);
    if (ok && tr.cancelled) rc = 3;
    nxrt::shutdown();
    return rc;
}

}  // namespace

#ifdef GAME_AS_DLL
extern "C" int game_rt_main(int argc, char** argv, const GameRtModules* mods) {
    if (!mods || mods->size != sizeof(GameRtModules) || mods->api != GAME_RT_API || mods->count != sizeof(kModules) / sizeof(kModules[0])) {
        MessageBoxA(nullptr, GAME_EXE " and " GAME_RT_DLL " come from different releases. Run the launcher's Set up again.", GAME_SHORT_TITLE, MB_OK | MB_ICONERROR);
        return 3;
    }
    for (uint32_t i = 0; i < mods->count; i++) { kModules[i].nx = mods->mods[i]; kModules[i].abi = mods->abis[i]; }
#else
int main(int argc, char** argv) {
#endif
    GAME_REGISTER();      // before anything reads it (nxrt::init, play mode, the services, the test tooling): docs/GAME_BOUNDARY.md
#ifdef GAME_GUI_SUBSYSTEM
    // Started from a terminal: show the output there. When the output is redirected to a file or a pipe (<game> --version > v.txt), leave it alone.
    const HANDLE out_handle = GetStdHandle(STD_OUTPUT_HANDLE);
    if ((!out_handle || out_handle == INVALID_HANDLE_VALUE) && AttachConsole(ATTACH_PARENT_PROCESS)) {
        std::freopen("CONOUT$", "w", stdout);
        std::freopen("CONOUT$", "w", stderr);
    }
#endif
    std::string exefs = std::getenv(GAME_ENV("EXEFS")) ? std::getenv(GAME_ENV("EXEFS")) : GAME_EXEFS_DEFAULT;
    bool exefs_given = std::getenv(GAME_ENV("EXEFS")) != nullptr;
    std::string game = std::getenv(GAME_ENV("GAME")) ? std::getenv(GAME_ENV("GAME")) : "";
    const char* ini = nullptr;
    const char* logp = nullptr;
    std::string replay_capture;                            // --replay-capture DIR: show a frame capture in a loop instead of starting the game (runtime/docs/CAPTURE.md)
    bool prepare = false, prepare_if_needed = false;      // --prepare-shaders [--if-needed] [--threads N]: compile the game's shaders ahead and exit (no game start)
    bool prepare_force = false;                           // --prepare-shaders --force: prepare again although prepare.state says it is done
    bool prepare_check = false;                           // --prepare-shaders --check: only say "prepare: ready" or "prepare: needed <why>"
    bool translate = false;                               // --translate-shaders: translate the corpus blocks the shader cache lacks and exit
    int prepare_threads = 0;
    std::string prepare_programs;                         // --programs FILE: another program list (tests)
    std::string instance = std::getenv("NXRT_INSTANCE") ? std::getenv("NXRT_INSTANCE") : "";              // --instance NAME (nxrt/instance.h)
    std::string instance_dir = std::getenv("NXRT_INSTANCE_DIR") ? std::getenv("NXRT_INSTANCE_DIR") : "";  // --instance-dir DIR
    std::vector<std::pair<std::string, std::string>> boot_opts;   // [boot] keys from the command line: nxrt::init() reloads the configuration, so they are applied after it
    for (int i = 1; i < argc; i++) {
        auto arg = [&](const char* n) { return !std::strcmp(argv[i], n) && i + 1 < argc; };
        if (arg("--game")) game = argv[++i];
        else if (arg("--exefs")) { exefs = argv[++i]; exefs_given = true; }
        else if (arg("--ini")) ini = argv[++i];
        else if (arg("--log")) logp = argv[++i];
        else if (arg("--max-seconds")) boot_opts.emplace_back("max_seconds", argv[++i]);
        else if (arg("--link")) boot_opts.emplace_back("link", argv[++i]);
        else if (!std::strcmp(argv[i], "--version")) { std::printf(GAME_ID " %s\n", GAME_RELEASE_VERSION); return 0; }
        else if (!std::strcmp(argv[i], "--prepare-shaders")) { prepare = true; _putenv("NXRT_DIAG_WATCHDOG_SECONDS=0"); }
        else if (!std::strcmp(argv[i], "--if-needed")) prepare_if_needed = true;
        else if (!std::strcmp(argv[i], "--check")) prepare_check = true;
        else if (!std::strcmp(argv[i], "--force")) prepare_force = true;
        else if (!std::strcmp(argv[i], "--translate-shaders")) { translate = true; _putenv("NXRT_DIAG_WATCHDOG_SECONDS=0"); }
        else if (arg("--cancel-event")) g_cancel_event = OpenEventW(SYNCHRONIZE, FALSE, widen_utf8(argv[++i]).c_str());
        else if (arg("--threads")) prepare_threads = std::atoi(argv[++i]);
        else if (arg("--programs")) prepare_programs = argv[++i];
        else if (arg("--instance")) instance = argv[++i];
        else if (arg("--instance-dir")) instance_dir = argv[++i];
        else if (arg("--replay-capture")) replay_capture = argv[++i];
        else if (!std::strcmp(argv[i], "--no-nvn")) boot_opts.emplace_back("nvn", "0");
        else if (!std::strcmp(argv[i], "--headless")) boot_opts.emplace_back("gfx", "headless");
        else if (!std::strcmp(argv[i], "--window")) boot_opts.emplace_back("gfx", "window");
        else { std::fprintf(stderr, GAME_ID ": unknown argument %s\n", argv[i]); return 2; }
    }
    if (!instance.empty() && !valid_instance_name(instance)) {
        std::fprintf(stderr, GAME_ID ": instance name '%s': use 1-40 letters, digits, '-', '_' or '.' (not \"default\")\n", instance.c_str());
        return 2;
    }
    if (instance.empty() && !instance_dir.empty()) { std::fprintf(stderr, GAME_ID ": --instance-dir needs --instance NAME\n"); return 2; }
    if (!instance.empty() && prepare) {
        std::fprintf(stderr, GAME_ID ": --prepare-shaders writes the shared shader data (pipeline cache, prepare.state): run it without --instance\n");
        return 2;
    }
    if (!instance_dir.empty()) instance_dir = absolute_utf8(instance_dir);     // against the folder the exe was started from

#ifdef GAME_DEV_ROOT
    // A development build started without a game folder or ExeFS, and not from an installed folder: the checkout's game.
    if (game.empty() && !exefs_given && !file_exists(exe_dir() + L"\\exefs\\main")) {
        const char* gi = nxrt::game::info().game_ini;
        if (!(*gi && file_exists(exe_dir() + L"\\" + widen_utf8(gi)))) game = dev_game_folder();
    }
#endif

    // Installed layout: work from the game folder (see the file comment).
    std::string ini_abs, log_abs;
    bool installed = false;
    {
        const std::wstring base = game.empty() ? exe_dir() : widen_utf8(absolute_utf8(game));
        const char* gi = nxrt::game::info().game_ini;
        installed = !game.empty() || file_exists(base + L"\\exefs\\main") || (*gi && file_exists(base + L"\\" + widen_utf8(gi)));
        if (installed) {
            if (exefs_given) exefs = absolute_utf8(exefs);
            if (ini) { ini_abs = absolute_utf8(ini); ini = ini_abs.c_str(); }
            if (logp) { log_abs = absolute_utf8(logp); logp = log_abs.c_str(); }
            if (!SetCurrentDirectoryW(base.c_str())) die("cannot use the game folder %s", narrow_utf8(base).c_str());
            // One game (or one shader preparation) per game folder: two would write the same log, pipeline cache and saves. The name comes from
            // the folder's full path; NXRT_ALLOW_MULTIPLE=1 lifts the check for test rigs. A named instance writes its own files: it takes the
            // mutex of its instance folder instead (below), so it runs beside the default run and beside other instances.
            if (instance.empty() && !std::getenv("NXRT_ALLOW_MULTIPLE") && !take_folder_mutex(L"_", base))
                die_code(kExitRefused, "The game is already running from this folder (or its shaders are being prepared). Close that one first, "
                                       "or start another run beside it with --instance NAME.");
            if (!logp && instance.empty()) logp = GAME_LOG;
            const char* game_ini = nxrt::game::info().game_ini;     // the manifest's ini, written by setup
            if (!ini && !std::getenv("NXRT_INI") && *game_ini && file_exists(std::wstring(game_ini, game_ini + std::strlen(game_ini)))) ini = game_ini;
        }
    }

    // A named instance: its folder (default <game folder>\instances\<name>), one run per folder, and the log there unless --log names one.
    std::string instance_log;
    if (!instance.empty()) {
        if (instance_dir.empty()) {
            std::wstring cwd(32768, L'\0');
            cwd.resize(GetCurrentDirectoryW(static_cast<DWORD>(cwd.size()), cwd.data()));
            instance_dir = narrow_utf8(cwd) + "\\instances\\" + instance;
        }
        if (!std::getenv("NXRT_ALLOW_MULTIPLE") && !take_folder_mutex(L"_instance_", widen_utf8(instance_dir)))
            die_code(kExitRefused, "Instance '%s' is already running (its folder is %s). Close it first, or choose another --instance name.", instance.c_str(),
                     instance_dir.c_str());
        nxrt::instance_set(instance, instance_dir);       // before init: the crash handler and the watchdog take their report files from it
        if (!logp) logp = (instance_log = nxrt::instance_file(GAME_LOG)).c_str();
    }

    nxrt::InitOptions opt;
    opt.ini_path = ini;
    opt.log_path = logp;
    if (!nxrt::init(opt)) die("runtime start-up failed (address-space layout)");
    for (const auto& kv : boot_opts) nxrt::config_set("boot", kv.first.c_str(), kv.second.c_str());
    if (installed && !nxrt::config_has("fs", "romfs")) nxrt::config_set("fs", "romfs", "romfs");   // the ini missing or without [fs]
    // [fs] exefs (the game ini, or NXRT_FS_EXEFS) names the ExeFS when neither --exefs nor <ID>_EXEFS does; installed: relative to the game folder.
    if (!exefs_given) exefs = nxrt::config_get_str("fs", "exefs", installed ? "exefs" : exefs.c_str());
    nxrt::config_set("fs", "exefs", exefs.c_str());     // the one in use, for the About page
    nxrt::config_set("process", "program_id", "0x" GAME_TITLE_ID);     // GetInfo(ProgramId); an ini value or the environment still wins
    apply_game_config_defaults(nxrt::game::info().config_defaults);   // the title's own setting defaults (runtime/games/<id>)
    // nxrt::init installed the frame step before these defaults existed: read [time] again (no frame has run yet).
    if (nxrt::timestep::installed()) {
        nxrt::timestep::remove();
        nxrt::timestep::install(nxrt::timestep::load_config());
    }
#ifdef GAME_DEV_ROOT
    // Development build: the shader data this checkout prepared (DEVELOPMENT_SETUP.md section 7), unless the ini or NXRT_NVN_* name other.
    dev_default("nvn", "shader_cache", GAME_DEV_ROOT "/work/shaders/" GAME_ID "/spirv");
    dev_default("nvn", "shader_corpus", GAME_DEV_ROOT "/work/shaders/" GAME_ID "/corpus");
    dev_default("nvn", "shader_translator", GAME_DEV_ROOT "/tools/shaders/build/bin/Release/net10.0/spirv_harness.exe");
    // Bug capture bundles (F9, capture_bug request files) go to the checkout's work/bugs, where tools/ai lists them.
    if (!nxrt::config_has("bug", "dir")) nxrt::config_set("bug", "dir", GAME_DEV_ROOT "/work/bugs");
    // Frame captures (runtime/docs/CAPTURE.md) go to the checkout's work/captures.
    if (!nxrt::config_has("capture", "dir")) nxrt::config_set("capture", "dir", GAME_DEV_ROOT "/work/captures");
#endif
    nxrt::bug::set_build_info(GAME_BUILD_COMMIT, GAME_BUILD_DIRTY != 0);
    NXLOG_I("boot", "built from commit %s%s", *GAME_BUILD_COMMIT ? GAME_BUILD_COMMIT : "(unknown)", GAME_BUILD_DIRTY ? " with uncommitted changes" : "");
    NXLOG_I("boot", GAME_ID " %s (%s PC port)", GAME_RELEASE_VERSION, GAME_TITLE);
    NXLOG_I("boot", "%s %s: exefs '%s'", GAME_TITLE, nxrt::game::info().version, exefs.c_str());
    if (!prepare && !translate) check_memory();             // beside other runs of the game: refuse when the memory would not hold one more
    nxrt::instance_link_pipeline_cache();     // a named instance: its own pipeline cache file, linked to the shared one (needs [nvn] shader_cache)
    if (nxrt::instance_active()) sweep_instance_pipeline_caches(nxrt::instance_dir());

    if (prepare) return prepare_shaders_main(prepare_if_needed, prepare_check, prepare_force, prepare_threads, prepare_programs);
    if (translate) return translate_shaders_main();

    // A frame capture replay puts the captured memory back before every loop: compute results must reach the pools at the end of each
    // submission instead of staying in device-local copies (runtime/docs/CAPTURE.md).
    if (!replay_capture.empty() && !nxrt::config_has("nvn", "compute_out_lazy")) nxrt::config_set("nvn", "compute_out_lazy", "0");
    bool play_started = false;
    if (nxrt::config_get_bool("boot", "nvn", true)) {
        nxrt::nvn::install();   // nvnBootstrapLoader and the NVN entry points (logging stubs for what no back end implements)
        // The NVN back end (Vulkan). [boot] gfx = window (the default for a normal launch: play mode, see docs/S8.md) | headless (an offscreen
        // image ring: tests and scripted runs; also the default when [gfx] headless / NXRT_GFX_HEADLESS is set) | none (keep the logging stubs).
        const std::string gfxmode = nxrt::config_get_str("boot", "gfx", nxrt::config_get_bool("gfx", "headless", false) ? "headless" : "window");
        if (gfxmode != "none") {
            const bool headless = gfxmode != "window";
            nxrt::play::DisplayConfig disp;
            // First start (or after a cancelled one): the "Preparing shaders" window translates and compiles every shader before the game runs.
            if (!headless && replay_capture.empty()) {
                const bool go = nxrt::nvn::shader_setup_before_start(nxrt::game::window_title(), [](const std::function<bool(uint64_t, uint64_t)>& progress) {
                    nxrt::nvn::ShaderPrepareOptions po;
                    po.only_if_needed = true;
                    po.progress = progress;
                    nxrt::nvn::ShaderPrepareResult pr;
                    std::string err;
                    const bool ok = run_shader_prepare(po, &pr, &err);
                    if (!ok) NXLOG_W("setup", "shader preparation: %s", err.c_str());
                    else NXLOG_I("setup", "shader preparation: %llu programs, %llu prepared, %llu missing shaders, %llu failed, %.1f s",
                                 static_cast<unsigned long long>(pr.programs), static_cast<unsigned long long>(pr.prepared),
                                 static_cast<unsigned long long>(pr.missing_shaders), static_cast<unsigned long long>(pr.failed), pr.seconds);
                    return ok;
                });
                if (!go) { nxrt::shutdown(); return 3; }
            }
            if (!headless) {
                disp = nxrt::play::display();       // controls.ini: window size, present mode, frame cap
                nxrt::platform::init();
                nxrt::platform::WindowDesc wd;
                const std::string title = nxrt::instance_active() ? std::string(nxrt::game::window_title()) + " [" + nxrt::instance_name() + "]"
                                                                  : std::string(nxrt::game::window_title());
                wd.title = title.c_str();
                wd.width = disp.width;
                wd.height = disp.height;
                if (!nxrt::platform::create_window(wd)) die("cannot create the game window. The game needs a graphics driver with Vulkan 1.3: update the driver for your graphics card and try again. The reason is in " GAME_LOG ".");
            }
            nxrt::gfx::DeviceDesc dd;
            dd.headless = headless ? 1 : 0;
            std::string err;
            g_gfx_device = nxrt::gfx::Device::create(dd, &err);
            if (!g_gfx_device) die("graphics device: %s", err.c_str());
            nxrt::gfx::RendererDesc rd;
            rd.headless_width = static_cast<uint32_t>(nxrt::config_get_int("boot", "headless_width", 1280));     // test runs: small and light
            rd.headless_height = static_cast<uint32_t>(nxrt::config_get_int("boot", "headless_height", 720));
            if (!headless) rd.present_mode = disp.present_mode;
            g_gfx_renderer = nxrt::gfx::Renderer::create(*g_gfx_device, rd, &err);
            if (!g_gfx_renderer) die("renderer: %s", err.c_str());
            nxrt::nvn::BackendDesc bd;
            bd.device = g_gfx_device.get();
            bd.renderer = g_gfx_renderer.get();
            if (!headless) {
                bd.limiter = nxrt::play::limiter();        // the frame limiter paces the presenting thread
                bd.keep_aspect = disp.keep_aspect;
            }
            if (!nxrt::nvn::install_backend(bd)) die("NVN back end failed to start (see the log)");
            NXLOG_I("boot", "NVN back end running on %s (%s)", g_gfx_device->gpu().name.c_str(), headless ? "headless" : "window");
            nxrt::play::StartOptions po;
            po.device = g_gfx_device.get();
            po.renderer = g_gfx_renderer.get();
            po.window = !headless;
            po.overlay = !headless;
            nxrt::play::start(po);
            play_started = true;
            if (!replay_capture.empty()) {   // a frame capture instead of the game: no module is mapped, no game code runs
                const int rc = nxrt::nvn::run_capture_replay(absolute_utf8(replay_capture));
                nxrt::nvn::backend_flush_caches();
                nxrt::log_flush();
                TerminateProcess(GetCurrentProcess(), static_cast<UINT>(rc));
            }
        }
    }
    if (!replay_capture.empty()) die("--replay-capture needs the graphics back end ([boot] nvn and gfx must not be off)");
    if (!play_started) {
        nxrt::play::StartOptions po;           // no graphics: play mode still turns scripted keys into pad input
        po.overlay = false;
        nxrt::play::start(po);
    }

    // 1. map
    for (const GameModule& g : kModules) {
        nxrt::LoadOptions lo;
        lo.name = g.file;
        lo.base = g.base;
        std::string err;
        const std::string path = exefs + "\\" + g.file;
        if (!file_exists(widen_utf8(path)))
            die("The game files were not found (%s). Choose your game files in the " GAME_SHORT_TITLE " launcher (or set [fs] exefs in " GAME_INI "), then start the game again.", path.c_str());
        nxrt::Module* m = nxrt::module_load_nso_file(path.c_str(), lo, &err);
        if (!m) die("cannot load %s: %s", path.c_str(), err.c_str());
        if (m->base() != g.nx->base) die("module %s is mapped at a base different from the one the library was lifted for", g.file);
        // 2. register the lifted functions and tables (an ABI-version mismatch of a stale library is refused here)
        const nxrt::RegResult r = nxrt::nxmodule_register(m, g.nx, *g.abi);
        if (!r.ok()) die("lifted module %s refused: %s", g.file, r.message.c_str());
    }

    nxrt::kernel::fibers_install();   // nn::os fibers on host fibers (when the lifted sdk has the entryhook site)

    // The game reaches NVN only through `nvnBootstrapLoader` (an import of main that the sdk defines: Nintendo's driver). The lifted PLT
    // stub of main calls host_import(), not the GOT that rtld fills, so the symbol is forced to the runtime's NVN front end here; the
    // sdk's own definition is also replaced in the call table for any indirect call that reaches it by address (debug note 120).
    if (nxrt::config_get_bool("boot", "nvn", true)) {
        nxrt::module_override_symbol("nvnBootstrapLoader");
        if (const uint64_t def = nxrt::nxmodule_find_export("nvnBootstrapLoader"))
            if (nxrt::HleFn fn = nxrt::hle_lookup("nvnBootstrapLoader")) nxrt::fn_register(def, fn);
    }

    // 3. link: relocations of every module, imports between modules by symbol name, page protections
    //    [boot] link = rtld (default): the host only maps and protects; the game's own rtld (lifted) relocates every module, as on the console
    //    [boot] link = host: the runtime's linker relocates, rtld's linker step is skipped (a cross-check of the two)
    const std::string link = nxrt::config_get_str("boot", "link", "rtld");
    if (link == "host") {
        if (!nxrt::module_link_all()) die("linking the modules failed (see the log)");
    } else {
        nxrt::module_protect_all();
    }

    if (const std::string spec = nxrt::config_get_str("boot", "press", ""); !spec.empty()) {
        parse_presses(spec);
        g_pad = std::make_shared<nxrt::hid::ManualInputSource>();
        g_pad->connect(0, nxrt::hid::NpadStyle::FullKey);
        nxrt::hid::set_input_source(g_pad);
        CreateThread(nullptr, 0, press_thread, nullptr, 0, nullptr);
    }
    if (nxrt::config_get_int("boot", "screenshot_at", 0) > 0) CreateThread(nullptr, 0, shot_thread, nullptr, 0, nullptr);
    if (const int64_t s = nxrt::config_get_int("boot", "max_seconds", 0); s > 0)
        CreateThread(nullptr, 0, deadline_thread, reinterpret_cast<void*>(static_cast<uintptr_t>(s * 1000)), 0, nullptr);
    if (const int64_t s = nxrt::config_get_int("prof", "llvm_profile_at_s", 0); s > 0)
        CreateThread(nullptr, 0, llvm_profile_thread, reinterpret_cast<void*>(static_cast<uintptr_t>(s)), 0, nullptr);

    // 4. the main thread: the npdm of main says 1 MiB of stack, priority 44, core 0
    const uint64_t stack = static_cast<uint64_t>(nxrt::config_get_int("boot", "main_stack_kb", 1024)) << 10;
    const uint32_t h = nxrt::kernel::main_thread_adopt(stack, "main");
    if (!h) die("cannot create the main thread");
    ::Ctx* ctx = nxrt::kernel::current_ctx();

    // SDK 20's rtld is lifted whole (the game's recomp config.py extra_entries: the entry at 0 and the three callbacks it hands to
    // nn::init::Start), so no host stand-ins are needed; refuse a library lifted without them.
    if (!nxrt::fn_lookup(kModules[0].base)) die("the lifted rtld has no function at its entry: rebuild the game libraries");

    // 5. the first instruction: rtld offset 0. x0 = 0 (the exception entry would be non-zero), x1 = the main thread's handle.
    const uint64_t entry = kModules[0].base;
    ctx->x[0] = 0;
    ctx->x[1] = h;
    ctx->x[30] = 0;
    NXLOG_I("boot", "entering %s at 0x%llx (x1 = main thread handle 0x%x)", kModules[0].file, static_cast<unsigned long long>(entry), h);
    nxrt::dispatch_call(ctx, entry);

    NXLOG_I("boot", "the entry function returned (x0 = 0x%llx): ending the process", static_cast<unsigned long long>(ctx->x[0]));
    nxrt::play::stop();
    nxrt::kernel::main_thread_release();
    nxrt::shutdown();
    return static_cast<int>(ctx->x[0] & 0xff);
}
