// SPDX-License-Identifier: GPL-3.0-or-later
// Initial Setup: tools, requirements, layout, step records and child processes (setup_core.h).
#include "setup_core.h"

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>

#include "setup_games.h"

namespace fs = std::filesystem;

namespace setup {

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
std::wstring with_commas(uint64_t v) {
    std::wstring s = std::to_wstring(v);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(static_cast<size_t>(i), L",");
    return s;
}
std::wstring gib(uint64_t bytes) {
    wchar_t b[32];
    std::swprintf(b, 32, L"%.1f GB", static_cast<double>(bytes) / 1e9);
    return b;
}
bool file_exists(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
bool dir_exists(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
std::wstring join(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b;
    return (a.back() == L'\\' || a.back() == L'/') ? a + b : a + L"\\" + b;
}
std::wstring parent_of(const std::wstring& p) {
    const size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? std::wstring() : p.substr(0, s);
}
std::wstring exe_dir() {
    std::wstring buf(32768, L'\0');
    buf.resize(GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size())));
    return parent_of(buf);
}

namespace {

std::string read_text(const std::wstring& p) {
    std::ifstream f(fs::path(p), std::ios::binary);
    if (!f) return {};
    std::ostringstream s;
    s << f.rdbuf();
    return s.str();
}

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return s.substr(i);
}

// key = value lines (sections ignored, ';' comments)
std::map<std::string, std::string> read_kv(const std::wstring& p) {
    std::map<std::string, std::string> m;
    std::istringstream in(read_text(p));
    std::string l;
    while (std::getline(in, l)) {
        l = trim(l);
        if (l.empty() || l[0] == ';' || l[0] == '[') continue;
        const size_t eq = l.find('=');
        if (eq == std::string::npos) continue;
        m[trim(l.substr(0, eq))] = trim(l.substr(eq + 1));
    }
    return m;
}

bool write_atomic(const std::wstring& path, const std::string& text) {
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    const std::wstring tmp = path + L".tmp";
    {
        std::ofstream f(fs::path(tmp), std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << text;
        if (!f) return false;
    }
    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

std::wstring search_path(const wchar_t* name) {
    wchar_t buf[MAX_PATH];
    const DWORD n = SearchPathW(nullptr, name, nullptr, MAX_PATH, buf, nullptr);
    return n && n < MAX_PATH ? std::wstring(buf) : std::wstring();
}

}  // namespace

std::string read_file_text(const std::wstring& p) { return read_text(p); }
bool write_file_atomic(const std::wstring& path, const std::string& text) { return write_atomic(path, text); }

// ---- tools -----------------------------------------------------------------------------------------------------------------------------
Tools find_tools() {
    Tools t;
    t.exe_dir = exe_dir();
#ifdef SETUP_DEV_ROOT
    t.dev_root = widen(SETUP_DEV_ROOT);
    for (wchar_t& c : t.dev_root) if (c == L'/') c = L'\\';
    wchar_t full[MAX_PATH];
    if (GetFullPathNameW(t.dev_root.c_str(), MAX_PATH, full, nullptr)) t.dev_root = full;
#endif
    // A release folder carries its tools next to the setup exe; a development build uses the checkout's.
    const std::wstring rel = join(t.exe_dir, L"tools");
    auto pick = [](std::initializer_list<std::wstring> c) {
        for (const std::wstring& p : c)
            if (!p.empty() && file_exists(p)) return p;
        return std::wstring();
    };
    const std::wstring dev = t.dev_root;
    t.nup_extract = pick({join(rel, L"scripts\\nup_extract.py"), dev.empty() ? L"" : join(dev, L"tools\\extract\\nup_extract.py")});
    const std::wstring eg = pick({join(rel, L"scripts\\extract_generic.py"), dev.empty() ? L"" : join(dev, L"tools\\shaders\\scripts\\extract_generic.py")});
    t.shader_scripts = parent_of(eg);
    // The game's own default for the translator is <exe dir>\tools\spirv_harness\spirv_harness.exe (nvn_shaders.cpp start_translator).
    t.translator = pick({join(rel, L"spirv_harness\\spirv_harness.exe"),
                         dev.empty() ? L"" : join(dev, L"tools\\shaders\\build\\bin\\Release\\net10.0\\spirv_harness.exe")});
    t.make_manifest = pick({join(rel, L"scripts\\make_manifest.py"), dev.empty() ? L"" : join(dev, L"tools\\extract\\make_manifest.py")});
    if (file_exists(join(rel, L"build\\zig\\zig.exe"))) t.game_build = pick({join(rel, L"build\\game_build.py")});
    t.python = pick({join(rel, L"python\\python.exe"), dev.empty() ? L"" : join(dev, L"tools\\.venv\\Scripts\\python.exe")});
    if (t.python.empty()) {
        // the py launcher first: the python.exe on PATH may be the Microsoft Store stub, which only opens the Store
        const std::wstring py = search_path(L"py.exe");
        if (!py.empty()) { t.python = py; t.python_args = L"-3"; }
        else t.python = search_path(L"python.exe");
    }
    return t;
}

// ---- settings and layout ------------------------------------------------------------------------------------------------------------
std::wstring game_exe_path(const Tools& t, const GameDef& g) {
    const std::wstring exe = widen(g.exe);
    const std::wstring here = join(t.exe_dir, exe);
    if (file_exists(here) || t.dev_root.empty()) return here;
    const std::wstring dev = join(t.dev_root, L"game-build\\build\\release\\" + exe);
    return file_exists(dev) ? dev : here;
}

GameBuild game_build_of(const Tools& t, const GameDef& g) {
    GameBuild b;
    if (t.game_build.empty() || g.manifest.empty()) return b;
    // an experimental build links the generic runtime of this release, whatever the game
    const std::wstring dir = g.generic ? join(t.exe_dir, L"games\\generic") : parent_of(g.manifest);
    b.sdk = join(dir, L"sdk");
    b.runtime = join(dir, L"runtime");
    std::string rt = g.runtime_dll.empty() ? g.id + "_rt.dll" : g.runtime_dll;
    const std::wstring lib = join(b.sdk, widen(rt.substr(0, rt.find_last_of('.')) + ".lib"));
    if (!file_exists(lib) || !file_exists(join(b.runtime, widen(rt))) || !file_exists(join(b.sdk, L"stub.cpp"))) return b;
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](const void* p, size_t n) {
        const unsigned char* c = static_cast<const unsigned char*>(p);
        for (size_t i = 0; i < n; ++i) h = (h ^ c[i]) * 1099511628211ull;
    };
    for (const std::wstring& d : {b.sdk, b.runtime, parent_of(t.game_build)}) {
        WIN32_FIND_DATAW fd;
        const HANDLE f = FindFirstFileW(join(d, L"*").c_str(), &fd);
        if (f == INVALID_HANDLE_VALUE) continue;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            mix(fd.cFileName, wcslen(fd.cFileName) * sizeof(wchar_t));
            mix(&fd.nFileSizeLow, sizeof fd.nFileSizeLow);
            mix(&fd.ftLastWriteTime, sizeof fd.ftLastWriteTime);
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    char hex[17];
    std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(h));
    b.stamp = hex;
    b.ok = true;
    return b;
}

bool experimental_possible(const Tools& t) {
    if (t.game_build.empty() || t.make_manifest.empty() || t.python.empty()) return false;
    GameDef g;
    g.id = "generic";
    g.generic = true;
    g.runtime_dll = "generic_rt.dll";
    g.manifest = join(t.exe_dir, L"games\\generic\\game.toml");
    return game_build_of(t, g).ok;
}

std::wstring game_exe_for(const Tools& t, const GameDef& g, const Config& c) {
    if (game_build_of(t, g).ok) return join(join(c.data_dir, L"program"), widen(g.exe));
    return game_exe_path(t, g);
}

SourceKind source_kind(const std::wstring& src) {
    if (src.empty()) return SourceKind::none;
    if (dir_exists(src)) return SourceKind::folder;
    if (!file_exists(src)) return SourceKind::missing;
    std::wstring ext = src.size() > 4 ? src.substr(src.size() - 4) : L"";
    for (wchar_t& ch : ext) ch = static_cast<wchar_t>(towlower(ch));
    if (ext == L".nup") return SourceKind::nup;
    return SourceKind::unsupported;   // NSP, XCI, NSZ, XCZ: encrypted containers, never read (no decryption)
}

namespace {
// The game folder inside an extracted dump: the folder itself (exefs + romfs), else updates\<newest> or base.
std::wstring game_folder_in(const std::wstring& root) {
    if (root.empty()) return {};
    if (file_exists(join(root, L"exefs\\main")) && dir_exists(join(root, L"romfs"))) return root;
    std::wstring best;
    unsigned long best_v = 0;
    WIN32_FIND_DATAW fd;
    const HANDLE h = FindFirstFileW(join(root, L"updates\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !iswdigit(fd.cFileName[0])) continue;
            const std::wstring d = join(root, std::wstring(L"updates\\") + fd.cFileName);
            const unsigned long v = wcstoul(fd.cFileName, nullptr, 10);
            if ((best.empty() || v > best_v) && file_exists(join(d, L"exefs\\main")) && dir_exists(join(d, L"romfs"))) { best = d; best_v = v; }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (best.empty() && file_exists(join(root, L"base\\exefs\\main")) && dir_exists(join(root, L"base\\romfs"))) best = join(root, L"base");
    return best;
}
}  // namespace

Layout layout_of(const Config& c) {
    Layout l;
    l.data = c.data_dir;
    l.dump = join(l.data, L"dump");
    l.shaders = join(l.data, L"shader_cache");
    l.corpus = join(l.shaders, L"corpus");
    l.spirv = join(l.shaders, L"spirv");
    l.programs = join(l.shaders, L"programs.bin");
    l.prepare_state = join(l.shaders, L"prepare.state");
    l.game_ini = join(l.data, c.game_ini.empty() ? std::wstring(L"game.ini") : c.game_ini);
    l.state_file = join(l.data, L"setup_state.ini");
    if (source_kind(c.source) == SourceKind::folder) l.game = game_folder_in(c.source);
    else if (file_exists(join(l.dump, L"extract.json"))) l.game = game_folder_in(l.dump);
    return l;
}

GameId game_id_of(const std::wstring& game) {
    GameId g;
    if (game.empty()) return g;
    HANDLE h = CreateFileW(join(game, L"exefs\\main").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return g;
    unsigned char hdr[0x60] = {};
    DWORD n = 0;
    const bool ok = ReadFile(h, hdr, sizeof hdr, &n, nullptr) && n == sizeof hdr && std::memcmp(hdr, "NSO0", 4) == 0;
    CloseHandle(h);
    if (!ok) return g;
    char hex[41];
    for (int i = 0; i < 20; ++i) std::snprintf(hex + 2 * i, 3, "%02x", hdr[0x40 + i]);
    g.build_id = hex;
    const size_t s = game.find_last_of(L"\\/");
    g.version = narrow(s == std::wstring::npos ? game : game.substr(s + 1));
    g.ok = true;
    return g;
}

std::string translator_stamp(const std::wstring& exe) {
    if (exe.empty() || !file_exists(exe)) return {};
    // FNV-1a over (name, contents) of every exe/DLL in the translator's folder: a release with a changed translator differs, an
    // unchanged one copied over the old files (new write times) does not, so its translations stay. The contents (~80 MB) are read
    // once per process for one set of (name, size, write time).
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](const void* p, size_t n) {
        const unsigned char* b = static_cast<const unsigned char*>(p);
        for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
    };
    std::vector<std::wstring> names;
    WIN32_FIND_DATAW fd;
    const HANDLE f = FindFirstFileW(join(parent_of(exe), L"*").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return {};
    std::map<std::wstring, WIN32_FIND_DATAW> files;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring n = fd.cFileName;
        const std::wstring lower = [&] { std::wstring x = n; for (wchar_t& c : x) c = static_cast<wchar_t>(towlower(c)); return x; }();
        if (lower.size() > 4 && (lower.substr(lower.size() - 4) == L".dll" || lower.substr(lower.size() - 4) == L".exe")) files[lower] = fd;
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    for (auto& [n, d] : files) {
        mix(n.data(), n.size() * sizeof(wchar_t));
        mix(&d.nFileSizeLow, 4); mix(&d.nFileSizeHigh, 4);
        mix(&d.ftLastWriteTime, sizeof d.ftLastWriteTime);
    }
    static std::mutex mu;
    static std::map<uint64_t, std::string> memo;              // (name, size, write time) fingerprint -> content stamp
    std::lock_guard<std::mutex> lock(mu);
    if (auto it = memo.find(h); it != memo.end()) return it->second;
    const uint64_t dates = h;
    h = 1469598103934665603ull;
    std::vector<char> buf(1 << 20);
    for (auto& [n, d] : files) {
        mix(n.data(), n.size() * sizeof(wchar_t));
        std::ifstream in(join(parent_of(exe), n), std::ios::binary);
        if (!in) return {};
        while (in) {
            in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
            mix(buf.data(), static_cast<size_t>(in.gcount()));
        }
    }
    char b[24];
    std::snprintf(b, sizeof b, "c%016llx", static_cast<unsigned long long>(h));
    return memo[dates] = b;
}

std::string nup_stamp(const std::wstring& nup) {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW(nup.c_str(), GetFileExInfoStandard, &a)) return {};
    const uint64_t size = (uint64_t(a.nFileSizeHigh) << 32) | a.nFileSizeLow;
    const uint64_t t = (uint64_t(a.ftLastWriteTime.dwHighDateTime) << 32) | a.ftLastWriteTime.dwLowDateTime;
    return narrow(nup) + "|" + std::to_string(size) + "|" + std::to_string(t);
}

State load_state(const std::wstring& path) {
    auto kv = read_kv(path);
    State s;
    s.extracted_from = kv["extracted_from"];
    s.corpus_game = kv["corpus_game"];
    s.translated_game = kv["translated_game"];
    s.translated_with = kv["translated_with"];
    s.programs_game = kv["programs_game"];
    s.built_for = kv["built_for"];
    return s;
}

bool save_state(const std::wstring& path, const State& s) {
    std::string t = "; What the setup finished, and for which game and translator (a change means that step runs again).\n[state]\n";
    t += "extracted_from = " + s.extracted_from + "\n";
    t += "corpus_game = " + s.corpus_game + "\n";
    t += "translated_game = " + s.translated_game + "\n";
    t += "translated_with = " + s.translated_with + "\n";
    t += "programs_game = " + s.programs_game + "\n";
    t += "built_for = " + s.built_for + "\n";
    return write_atomic(path, t);
}

// ---- requirements -----------------------------------------------------------------------------------------------------------------
namespace {

std::wstring driver_text(uint32_t vendor, uint32_t v) {
    wchar_t b[64];
    if (vendor == 0x10de) std::swprintf(b, 64, L"%u.%u", (v >> 22) & 0x3ff, (v >> 14) & 0xff);   // NVIDIA
    else if (vendor == 0x8086) std::swprintf(b, 64, L"%u.%u", v >> 14, v & 0x3fff);           // Intel on Windows
    else std::swprintf(b, 64, L"%u.%u.%u", VK_API_VERSION_MAJOR(v), VK_API_VERSION_MINOR(v), VK_API_VERSION_PATCH(v));
    return b;
}

// The DLL names a PE file imports (its import directory, read from the file).
std::vector<std::string> pe_imports(const std::wstring& path) {
    std::vector<std::string> out;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return out;
    auto rd = [&](uint64_t off, void* dst, DWORD n) {
        LARGE_INTEGER li;
        li.QuadPart = static_cast<LONGLONG>(off);
        DWORD got = 0;
        return SetFilePointerEx(h, li, nullptr, FILE_BEGIN) && ReadFile(h, dst, n, &got, nullptr) && got == n;
    };
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS64 nt{};
    if (rd(0, &dos, sizeof dos) && dos.e_magic == IMAGE_DOS_SIGNATURE && rd(static_cast<uint64_t>(dos.e_lfanew), &nt, sizeof nt) &&
        nt.Signature == IMAGE_NT_SIGNATURE && nt.OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        std::vector<IMAGE_SECTION_HEADER> sec(nt.FileHeader.NumberOfSections);
        const uint64_t sec_off = static_cast<uint64_t>(dos.e_lfanew) + offsetof(IMAGE_NT_HEADERS64, OptionalHeader) + nt.FileHeader.SizeOfOptionalHeader;
        if (!sec.empty() && rd(sec_off, sec.data(), static_cast<DWORD>(sec.size() * sizeof(IMAGE_SECTION_HEADER)))) {
            auto file_off = [&](uint32_t rva) -> int64_t {
                for (const auto& s : sec)
                    if (rva >= s.VirtualAddress && rva < s.VirtualAddress + std::max(s.Misc.VirtualSize, s.SizeOfRawData))
                        return static_cast<int64_t>(rva - s.VirtualAddress) + s.PointerToRawData;
                return -1;
            };
            const auto& dir = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
            int64_t at = dir.VirtualAddress ? file_off(dir.VirtualAddress) : -1;
            for (int i = 0; at >= 0 && i < 512; ++i, at += sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
                IMAGE_IMPORT_DESCRIPTOR d{};
                if (!rd(static_cast<uint64_t>(at), &d, sizeof d) || !d.Name) break;
                const int64_t no = file_off(d.Name);
                char name[256] = {};
                if (no >= 0 && rd(static_cast<uint64_t>(no), name, 255)) out.emplace_back(name);
            }
        }
    }
    CloseHandle(h);
    return out;
}

bool dll_found(const std::string& name, const std::wstring& dir) {
    std::string l = name;
    for (char& c : l) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    if (l.rfind("api-ms-", 0) == 0 || l.rfind("ext-ms-", 0) == 0) return true;   // API sets: part of Windows
    const std::wstring w = widen(name);
    if (file_exists(join(dir, w))) return true;
    HMODULE m = LoadLibraryExW(w.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!m) return false;
    FreeLibrary(m);
    return true;
}

uint64_t free_bytes(std::wstring dir) {
    while (!dir.empty() && !dir_exists(dir)) dir = parent_of(dir);
    ULARGE_INTEGER avail{};
    if (dir.empty() || !GetDiskFreeSpaceExW(dir.c_str(), &avail, nullptr, nullptr)) return 0;
    return avail.QuadPart;
}

uint64_t size_of(const std::wstring& p) {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &a)) return 0;
    return (uint64_t(a.nFileSizeHigh) << 32) | a.nFileSizeLow;
}

}  // namespace

std::vector<GpuInfo> query_gpus(std::wstring* error) {
    std::vector<GpuInfo> out;
    HMODULE vk = LoadLibraryW(L"vulkan-1.dll");
    if (!vk) { *error = L"vulkan-1.dll was not found: no Vulkan driver is installed"; return out; }
    auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(vk, "vkGetInstanceProcAddr"));
    auto create = gipa ? reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr, "vkCreateInstance")) : nullptr;
    auto version = gipa ? reinterpret_cast<PFN_vkEnumerateInstanceVersion>(gipa(nullptr, "vkEnumerateInstanceVersion")) : nullptr;
    uint32_t loader = VK_API_VERSION_1_0;
    if (version) version(&loader);
    if (!create) { *error = L"the Vulkan loader is broken (no vkCreateInstance)"; FreeLibrary(vk); return out; }
    if (loader < VK_API_VERSION_1_3) { *error = L"the Vulkan loader offers less than Vulkan 1.3"; FreeLibrary(vk); return out; }
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "game_setup";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    VkInstance inst = VK_NULL_HANDLE;
    if (create(&ci, nullptr, &inst) != VK_SUCCESS) { *error = L"no Vulkan instance could be created"; FreeLibrary(vk); return out; }
#define L_(n) auto n = reinterpret_cast<PFN_##n>(gipa(inst, #n))
    L_(vkEnumeratePhysicalDevices); L_(vkGetPhysicalDeviceProperties); L_(vkGetPhysicalDeviceFeatures2);
    L_(vkEnumerateDeviceExtensionProperties); L_(vkDestroyInstance);
#undef L_
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(inst, &n, nullptr);
    std::vector<VkPhysicalDevice> devs(n);
    if (n) vkEnumeratePhysicalDevices(inst, &n, devs.data());
    for (VkPhysicalDevice d : devs) {
        VkPhysicalDeviceProperties p{};
        vkGetPhysicalDeviceProperties(d, &p);
        if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) continue;   // software rasterizers
        GpuInfo g;
        g.name = widen(p.deviceName);
        g.driver = driver_text(p.vendorID, p.driverVersion);
        g.api = p.apiVersion;
        uint32_t ne = 0;
        vkEnumerateDeviceExtensionProperties(d, nullptr, &ne, nullptr);
        std::vector<VkExtensionProperties> ex(ne);
        if (ne) vkEnumerateDeviceExtensionProperties(d, nullptr, &ne, ex.data());
        auto has = [&](const char* e) { for (auto& x : ex) if (!std::strcmp(x.extensionName, e)) return true; return false; };
        g.pipeline_library = has(VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME);
        g.mesh_shader = has(VK_EXT_MESH_SHADER_EXTENSION_NAME);
        // what the game's device needs (runtime/src/gfx/vk_device.cpp: Vulkan 1.3, dynamic rendering, synchronization2, a swapchain)
        g.usable = true;
        if (p.apiVersion < VK_API_VERSION_1_3) { g.usable = false; g.why = L"Vulkan 1.3 not supported"; }
        else {
            VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
            VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f13};
            vkGetPhysicalDeviceFeatures2(d, &f2);
            if (!f13.dynamicRendering || !f13.synchronization2) { g.usable = false; g.why = L"dynamic rendering or synchronization2 missing"; }
            else if (!has(VK_KHR_SWAPCHAIN_EXTENSION_NAME)) { g.usable = false; g.why = L"cannot show pictures (no swapchain)"; }
        }
        out.push_back(g);
    }
    vkDestroyInstance(inst, nullptr);
    FreeLibrary(vk);
    if (out.empty()) *error = L"Vulkan found no graphics card";
    return out;
}

uint64_t remaining_disk_need(const Config& c, const State& s) {
    // Sizes seen with Ocarina of Time 2.0.0 on an NVIDIA RTX 4080: the game 26.3 GB, the shader corpus 0.9 GB, the translations 5.5 GB,
    // the compiled pipeline cache 2.3-4.7 GB. The extraction needs about the package's size.
    const Layout l = layout_of(c);
    uint64_t need = 0;
    if (source_kind(c.source) == SourceKind::nup && l.game.empty()) need += size_of(c.source);
    if (!file_exists(join(l.corpus, L"unique.tsv"))) need += 1'000'000'000ull;
    if (s.translated_game.empty()) need += 5'500'000'000ull;
    if (s.built_for.empty()) need += 4'000'000'000ull;   // the game build's generated C++ and objects (removed when it is done)
    if (!file_exists(join(l.shaders, L"pipeline_cache.bin"))) need += 5'000'000'000ull;
    return need;
}

std::vector<Check> run_system_checks(const Tools& t) {
    std::vector<Check> out;
    // Windows
    {
        Check k{L"Windows", L"", L"", L"", Level::ok};
        typedef LONG(WINAPI * RtlGetVersionFn)(OSVERSIONINFOW*);
        OSVERSIONINFOW v{sizeof v};
        if (auto f = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"))) f(&v);
        k.detail = (v.dwBuildNumber >= 22000 ? L"Windows 11" : L"Windows 10") + std::wstring(L", build ") + std::to_wstring(v.dwBuildNumber) + L", 64-bit";
        if (v.dwMajorVersion < 10 || v.dwBuildNumber < 17763) {
            k.level = Level::bad;
            k.state = L"Too old";
            k.fix = L"Windows 10 version 1809 or newer (64-bit) is needed. Update Windows: Settings > Windows Update.";
        } else k.state = L"OK";
        out.push_back(k);
    }
    // Graphics card
    {
        Check k{L"Graphics card (Vulkan 1.3)", L"", L"", L"", Level::ok};
        std::wstring err;
        const std::vector<GpuInfo> gpus = query_gpus(&err);
        const GpuInfo* best = nullptr;
        for (const GpuInfo& g : gpus) if (g.usable && (!best || (g.pipeline_library && !best->pipeline_library))) best = &g;
        if (!best) {
            k.level = Level::bad;
            k.state = L"Missing";
            k.detail = err.empty() ? (gpus.empty() ? L"no GPU" : gpus[0].name + L": " + gpus[0].why) : err;
            k.fix = L"The game needs a graphics card with Vulkan 1.3 and a current driver:\r\n"
                    L"  NVIDIA (GeForce GTX 900 or newer): https://www.nvidia.com/Download/index.aspx\r\n"
                    L"  AMD (Radeon RX 400 or newer): https://www.amd.com/en/support/download/drivers.html\r\n"
                    L"  Intel (Arc, Iris Xe, UHD 600 or newer): https://www.intel.com/content/www/us/en/download-center/home.html\r\n"
                    L"Install the driver, restart Windows, then press Check again.";
        } else {
            k.detail = best->name + L", driver " + best->driver + L", Vulkan " + std::to_wstring(VK_API_VERSION_MAJOR(best->api)) + L"." +
                       std::to_wstring(VK_API_VERSION_MINOR(best->api));
            if (!best->pipeline_library) {
                k.level = Level::warn;
                k.state = L"Limited";
                k.detail += L"; no VK_EXT_graphics_pipeline_library";
                k.fix = L"Your driver cannot compile shaders ahead (VK_EXT_graphics_pipeline_library is missing): the game will stutter while it "
                        L"compiles them during play. A newer driver usually has it: install the newest driver of your graphics card.";
            } else {
                k.state = L"OK";
                if (!best->mesh_shader) { k.level = Level::warn; k.state = L"OK (no mesh shaders)"; k.detail += L"; foliage needs VK_EXT_mesh_shader"; }
            }
        }
        out.push_back(k);
    }
    // Memory
    {
        Check k{L"Memory", L"", L"", L"", Level::ok};
        MEMORYSTATUSEX ms{sizeof ms};
        GlobalMemoryStatusEx(&ms);
        const double gbs = static_cast<double>(ms.ullTotalPhys) / double(1ull << 30);
        wchar_t b[64];
        std::swprintf(b, 64, L"%.0f GB installed (16 GB recommended)", gbs);
        k.detail = b;
        if (gbs < 11.5) {
            k.level = Level::warn;
            k.state = L"Low";
            k.fix = L"The game holds 13-16 GB while it plays. With less memory Windows pages to disk and the game stutters. Close other programs "
                    L"while playing; keep the page file on (Windows default).";
        } else k.state = L"OK";
        out.push_back(k);
    }
    // .NET for the shader translator (a self-contained translator needs none)
    {
        Check k{L"Shader translator", L"", L"", L"", Level::ok};
        if (t.translator.empty()) {
            k.level = Level::bad;
            k.state = L"Missing";
            k.detail = L"tools\\spirv_harness\\spirv_harness.exe";
            k.fix = L"The translator belongs in tools\\spirv_harness next to this program: unpack the whole release archive again.";
        } else {
            const std::string rc = read_text(join(parent_of(t.translator), L"spirv_harness.runtimeconfig.json"));
            const size_t fw = rc.find("\"framework");
            const bool self_contained = rc.empty() || rc.find("includedFrameworks") != std::string::npos || fw == std::string::npos;
            if (self_contained) {
                k.state = L"OK";
                k.detail = t.translator + L" (self-contained)";
            } else {
                std::string ver;
                const size_t v = rc.find("\"version\"", fw);
                if (v != std::string::npos) {
                    const size_t q1 = rc.find('"', rc.find(':', v) + 1), q2 = rc.find('"', q1 + 1);
                    if (q1 != std::string::npos && q2 != std::string::npos) ver = rc.substr(q1 + 1, q2 - q1 - 1);
                }
                const std::string major = ver.substr(0, ver.find('.'));
                bool found = false;
                std::vector<std::wstring> roots;
                wchar_t env[MAX_PATH];
                if (GetEnvironmentVariableW(L"DOTNET_ROOT", env, MAX_PATH)) roots.push_back(env);
                if (GetEnvironmentVariableW(L"ProgramFiles", env, MAX_PATH)) roots.push_back(join(env, L"dotnet"));
                for (const std::wstring& r : roots) {
                    WIN32_FIND_DATAW fd;
                    const HANDLE f = FindFirstFileW(join(r, L"shared\\Microsoft.NETCore.App\\" + widen(major) + L".*").c_str(), &fd);
                    if (f != INVALID_HANDLE_VALUE) { found = true; FindClose(f); }
                }
                k.detail = t.translator + L" needs the .NET " + widen(major) + L" runtime";
                if (found) k.state = L"OK";
                else {
                    k.level = Level::bad;
                    k.state = L".NET missing";
                    k.fix = L"Install the .NET " + widen(major) + L" Runtime (x64):\r\n  https://dotnet.microsoft.com/download/dotnet/" + widen(major) +
                            L".0\r\nor in a terminal:\r\n  winget install Microsoft.DotNet.Runtime." + widen(major);
                }
            }
        }
        out.push_back(k);
    }
    // Python for the extraction and the shader scan
    {
        Check k{L"Python (extraction, shader scan)", L"", L"", L"", Level::ok};
        const std::wstring pip_cmd = (t.python.empty() ? L"py -3" : quote(t.python) + (t.python_args.empty() ? L"" : L" " + t.python_args)) +
                                     L" -m pip install --user zstandard lz4";
        if (t.nup_extract.empty() || t.shader_scripts.empty()) {
            k.level = Level::bad;
            k.state = L"Scripts missing";
            k.detail = L"tools\\scripts\\nup_extract.py, extract_generic.py, bnsh_programs.py";
            k.fix = L"The scripts belong in tools\\scripts next to this program: unpack the whole release archive again.";
        } else if (t.python.empty()) {
            k.level = Level::bad;
            k.state = L"Missing";
            k.detail = L"no Python 3 found (tools\\python, py.exe or python.exe on PATH)";
            k.fix = L"Install Python 3.11 or newer (64-bit) from https://www.python.org/downloads/windows/ (tick \"Add python.exe to PATH\")\r\n"
                    L"or in a terminal:\r\n  winget install Python.Python.3.13\r\nthen add the two packages the scripts need:\r\n  py -3 -m pip install --user zstandard lz4";
        } else {
            std::string o;
            DWORD code = 1;
            const std::wstring args = (t.python_args.empty() ? L"" : t.python_args + L" ") +
                                      L"-I -c \"import sys; print(sys.version.split()[0]); import zstandard, lz4.block; print('modules ok')\"";
            const bool ran = run_capture(t.python, args, 20000, &o, &code);
            const std::string ver = trim(o.substr(0, o.find('\n')));
            if (!ran || ver.empty() || !std::isdigit(static_cast<unsigned char>(ver[0]))) {
                k.level = Level::bad;
                k.state = L"Does not run";
                k.detail = t.python + L" did not start (the Microsoft Store stub?)";
                k.fix = L"Install Python 3.11 or newer from https://www.python.org/downloads/windows/\r\nor in a terminal:\r\n  winget install Python.Python.3.13";
            } else if (o.find("modules ok") == std::string::npos) {
                k.level = Level::bad;
                k.state = L"Packages missing";
                k.detail = L"Python " + widen(ver) + L" (" + t.python + L") lacks zstandard or lz4";
                k.fix = L"In a terminal:\r\n  " + pip_cmd;
            } else {
                k.state = L"OK";
                k.detail = L"Python " + widen(ver) + L" (" + t.python + L") with zstandard and lz4";
            }
        }
        out.push_back(k);
    }
    return out;
}

std::vector<Check> run_game_checks(const Tools& t, const GameDef& g, const Config& c, uint64_t disk_need) {
    std::vector<Check> out;
    const std::wstring game_exe = game_exe_for(t, g, c);
    const bool buildable = game_build_of(t, g).ok;
    const std::wstring game_dir = parent_of(game_exe);
    const std::wstring exe_name = widen(g.exe);
    const std::wstring title = widen(g.title);
    const std::wstring want_ver = widen(g.dump_version);
    // Disk space
    {
        Check k{L"Disk space", L"", L"", L"", Level::ok};
        const uint64_t free = free_bytes(c.data_dir);
        k.detail = gib(free) + L" free where the setup writes (" + c.data_dir + L"); still needed: about " + gib(disk_need);
        if (free < disk_need) {
            k.level = Level::bad;
            k.state = L"Too little";
            k.fix = L"Free up " + gib(disk_need - free) + L" on that drive, or choose a data folder on another drive (Game files page, Change...).";
        } else k.state = L"OK";
        out.push_back(k);
    }
    // The game exe and the DLLs it imports (the Visual C++ runtime is linked in statically; this finds whatever else is missing)
    {
        Check k{L"Game program", L"", L"", L"", Level::ok};
        if (!file_exists(game_exe) && buildable) {
            k.level = Level::info;
            k.state = L"Not built yet";
            k.detail = game_exe;
            k.fix = L"Built from your dump on this PC the first time you press Play (once, 15-40 minutes).";
        } else if (!file_exists(game_exe)) {
            k.level = Level::bad;
            k.state = L"Missing";
            k.detail = game_exe;
            k.fix = exe_name + L" is not built. This package cannot build it (no games\\<id>\\sdk or tools\\build): unpack the whole release archive again. A development checkout builds it with game-build\\scripts\\build.ps1 -Manifest <its game.toml>.";
        } else {
            std::vector<std::wstring> missing;
            std::vector<std::wstring> mods = {game_exe};
            WIN32_FIND_DATAW fd;
            const HANDLE f = FindFirstFileW(join(game_dir, L"*.dll").c_str(), &fd);
            if (f != INVALID_HANDLE_VALUE) {
                do mods.push_back(join(game_dir, fd.cFileName));
                while (FindNextFileW(f, &fd));
                FindClose(f);
            }
            bool vc = false;
            for (const std::wstring& m : mods)
                for (const std::string& d : pe_imports(m))
                    if (!dll_found(d, game_dir) && std::find(missing.begin(), missing.end(), widen(d)) == missing.end()) {
                        missing.push_back(widen(d));
                        std::string l = d;
                        for (char& ch : l) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
                        if (l.rfind("vcruntime", 0) == 0 || l.rfind("msvcp", 0) == 0 || l.rfind("concrt", 0) == 0) vc = true;
                    }
            if (missing.empty()) {
                k.state = L"OK";
                k.detail = exe_name + L" and every DLL it needs are there (no Visual C++ Redistributable needed)";
            } else {
                k.level = Level::bad;
                k.state = L"DLL missing";
                for (const std::wstring& m : missing) k.detail += (k.detail.empty() ? L"" : L", ") + m;
                k.fix = vc ? L"Install the Microsoft Visual C++ Redistributable (x64):\r\n  https://aka.ms/vs/17/release/vc_redist.x64.exe\r\n"
                             L"or in a terminal:\r\n  winget install Microsoft.VCRedist.2015+.x64"
                           : L"These DLLs belong next to " + exe_name + L": unpack the whole release archive again.";
            }
        }
        out.push_back(k);
    }
    // The player's dump
    {
        Check k{L"Your game dump", L"", L"", L"", Level::ok};
        const SourceKind sk = source_kind(c.source);
        const Layout l = layout_of(c);
        const std::wstring want = title + L" (" + widen(g.title_id) + L"), " + want_ver;
        if (sk == SourceKind::none || sk == SourceKind::missing) {
            k.level = Level::bad;
            k.state = sk == SourceKind::none ? L"Not chosen" : L"Not found";
            k.detail = sk == SourceKind::none ? want : c.source;
            k.fix = L"Dump your own copy of " + title + L", version " + want_ver + L", from your console, then choose it on the Game files page:\r\n"
                    L"  a .nup package, or a folder with exefs\\ and romfs\\ (also updates\\<version>\\ or base\\ inside it).\r\n"
                    L"This setup never downloads game files.";
        } else if (sk == SourceKind::unsupported) {
            k.level = Level::bad;
            k.state = L"Not supported";
            k.detail = c.source;
            k.fix = L"NSP, XCI, NSZ and XCZ files are encrypted, and this program does not handle console keys. Extract the game with your own "
                    L"tools into a folder with the decrypted exefs\\ and romfs\\ (the update's, if you have it), or make a .nup package, and choose that.";
        } else {
            const GameId gi = game_id_of(l.game);
            if (!gi.ok) {
                k.level = sk == SourceKind::nup ? Level::info : Level::bad;
                k.state = sk == SourceKind::nup ? L"To extract" : L"Not a game folder";
                k.detail = c.source;
                if (sk != SourceKind::nup) k.fix = L"The folder needs exefs\\main and romfs\\ (directly, or in updates\\<version>\\ or base\\).";
            } else {
                k.detail = l.game + L" (main " + widen(gi.build_id.substr(0, 16)) + L")";
                if (!g.main_build_id.empty() && _stricmp(gi.build_id.c_str(), g.main_build_id.c_str()) != 0) {
                    k.level = Level::warn;
                    k.state = L"Other version";
                    k.fix = L"This port is made for " + want_ver + L". Your dump has another main module: the game may not start. Dump "
                            L"the game again with that update installed.";
                } else k.state = L"OK";
            }
        }
        out.push_back(k);
    }
    return out;
}

// ---- child processes --------------------------------------------------------------------------------------------------------------
std::wstring quote(const std::wstring& s) {
    if (!s.empty() && s.find_first_of(L" \t\"") == std::wstring::npos) return s;
    std::wstring o = L"\"";
    size_t bs = 0;
    for (wchar_t ch : s) {
        if (ch == L'\\') { ++bs; continue; }
        if (ch == L'"') o.append(bs * 2 + 1, L'\\');
        else o.append(bs, L'\\');
        bs = 0;
        o.push_back(ch);
    }
    o.append(bs * 2, L'\\');
    return o + L"\"";
}

namespace {
// job: when not null, the child (and every process it starts) goes into a new job object that ends them all when it is closed or
// terminated (the venv python.exe is a launcher stub; the shader scan runs a process pool).
bool spawn(const std::wstring& exe, const std::wstring& args, const std::wstring& cwd, HANDLE* proc, HANDLE* rd, std::wstring* err,
           HANDLE* job = nullptr) {
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE wr = nullptr;
    if (!CreatePipe(rd, &wr, &sa, 0)) { if (err) *err = L"CreatePipe failed"; return false; }
    SetHandleInformation(*rd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW si{sizeof si};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = wr;
    si.hStdError = wr;
    std::wstring cmd = quote(exe) + L" " + args;
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    PROCESS_INFORMATION pi{};
    // Python prints line by line into a pipe only unbuffered
    SetEnvironmentVariableW(L"PYTHONUNBUFFERED", L"1");
    SetEnvironmentVariableW(L"PYTHONIOENCODING", L"utf-8");
    const BOOL ok = CreateProcessW(exe.c_str(), buf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS | CREATE_SUSPENDED,
                                   nullptr, cwd.empty() ? nullptr : cwd.c_str(), &si, &pi);
    const DWORD code = GetLastError();
    CloseHandle(wr);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) {
        CloseHandle(*rd);
        *rd = nullptr;
        if (err) *err = L"could not start " + exe + L" (Windows error " + std::to_wstring(code) + L")";
        return false;
    }
    if (job) {
        *job = CreateJobObjectW(nullptr, nullptr);
        if (*job) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
            li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(*job, JobObjectExtendedLimitInformation, &li, sizeof li);
            if (!AssignProcessToJobObject(*job, pi.hProcess)) { CloseHandle(*job); *job = nullptr; }
        }
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    *proc = pi.hProcess;
    return true;
}
}  // namespace

Child::~Child() {
    if (running_) cancel();
    wait();
}

bool Child::start(const std::wstring& exe, const std::wstring& args, const std::wstring& cwd, bool with_cancel_event,
                  std::function<void(const std::string&)> on_line, std::function<void(DWORD)> on_exit, std::wstring* err) {
    wait();
    std::wstring a = args;
    if (with_cancel_event) {
        static std::atomic<int> seq{0};
        const std::wstring name = L"Local\\game_setup_cancel_" +std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(++seq);
        event_ = CreateEventW(nullptr, TRUE, FALSE, name.c_str());
        a += L" --cancel-event " + name;
    }
    if (!spawn(exe, a, cwd, &proc_, &pipe_, err, &job_)) {
        if (event_) { CloseHandle(event_); event_ = nullptr; }
        return false;
    }
    running_ = true;
    thread_ = std::thread([this, on_line, on_exit] {
        std::string acc;
        char buf[4096];
        DWORD n = 0;
        while (ReadFile(pipe_, buf, sizeof buf, &n, nullptr) && n) {
            acc.append(buf, n);
            size_t p;
            while ((p = acc.find_first_of("\r\n")) != std::string::npos) {
                std::string line = acc.substr(0, p);
                acc.erase(0, p + 1);
                if (!line.empty() && on_line) on_line(line);
            }
        }
        if (!acc.empty() && on_line) on_line(acc);
        WaitForSingleObject(proc_, INFINITE);
        DWORD code = 1;
        GetExitCodeProcess(proc_, &code);
        running_ = false;
        if (on_exit) on_exit(code);
    });
    return true;
}

void Child::cancel() {
    if (!running_) return;
    if (event_) SetEvent(event_);           // the child stops at its next progress report and saves what it has
    else if (job_) TerminateJobObject(job_, 3);     // the scripts keep finished files and continue from them
    else if (proc_) TerminateProcess(proc_, 3);
}

void Child::wait() {
    if (thread_.joinable()) thread_.join();
    for (HANDLE* h : {&proc_, &pipe_, &event_, &job_})
        if (*h) { CloseHandle(*h); *h = nullptr; }
}

bool run_capture(const std::wstring& exe, const std::wstring& args, DWORD timeout_ms, std::string* out, DWORD* code) {
    HANDLE proc = nullptr, rd = nullptr;
    if (!spawn(exe, args, L"", &proc, &rd, nullptr)) return false;
    std::thread reader([&] {
        char b[4096];
        DWORD n = 0;
        while (ReadFile(rd, b, sizeof b, &n, nullptr) && n) out->append(b, n);
    });
    const bool done = WaitForSingleObject(proc, timeout_ms) == WAIT_OBJECT_0;
    if (!done) TerminateProcess(proc, 1);
    reader.join();
    GetExitCodeProcess(proc, code);
    CloseHandle(proc);
    CloseHandle(rd);
    return done;
}

}  // namespace setup
