// SPDX-License-Identifier: GPL-3.0-or-later
// The generic game module, data part (generic_game.h): who the game is, read from the game manifest at start. Pure data, no runtime.
#include "generic_game.h"

#include <windows.h>

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>

namespace nxrt::games::generic {

namespace {

std::map<std::string, std::string>& values() {
    static std::map<std::string, std::string> v;
    return v;
}

std::string strip(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    return s.substr(i);
}

std::string exe_dir() {
    std::wstring p(32768, L'\0');
    p.resize(GetModuleFileNameW(nullptr, p.data(), static_cast<DWORD>(p.size())));
    const size_t sl = p.find_last_of(L"\\/");
    if (sl != std::wstring::npos) p.resize(sl);
    const int n = WideCharToMultiByte(CP_UTF8, 0, p.c_str(), static_cast<int>(p.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, p.c_str(), static_cast<int>(p.size()), s.data(), n, nullptr, nullptr);
    return s;
}

bool read_file(const std::string& utf8_path, std::string* out) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8_path.c_str(), -1, nullptr, 0);
    std::wstring w(static_cast<size_t>(n > 0 ? n : 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8_path.c_str(), -1, w.data(), n);
    std::ifstream f(w.c_str(), std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    *out = ss.str();
    return true;
}

}  // namespace

std::string Manifest::get(const std::string& key, const std::string& def) const {
    if (!loaded) return def;
    const auto& v = values();
    const auto it = v.find(path + "|" + key);
    return it == v.end() ? def : it->second;
}

unsigned long long Manifest::num(const std::string& key, unsigned long long def) const {
    const std::string s = get(key);
    if (s.empty()) return def;
    char* end = nullptr;
    const unsigned long long v = std::strtoull(s.c_str(), &end, 0);
    return end && *end == 0 ? v : def;
}

bool parse_manifest(const std::string& text, Manifest* out, std::string* error) {
    // values of one manifest are kept under "<path>|table.key" so tests can parse several texts in one process
    std::istringstream in(text);
    std::string line, table;
    bool in_array = false;
    int n = 0;
    std::map<std::string, std::string> got;
    while (std::getline(in, line)) {
        ++n;
        bool q = false;
        for (size_t i = 0; i < line.size(); ++i) {
            if (line[i] == '"') q = !q;
            else if (line[i] == '#' && !q) { line.resize(i); break; }
        }
        line = strip(line);
        if (line.empty()) continue;
        if (line[0] == '[') {
            in_array = line.size() > 1 && line[1] == '[';
            const size_t end = line.find(']');
            if (end == std::string::npos) { if (error) *error = "line " + std::to_string(n) + ": unclosed table header"; return false; }
            table = strip(line.substr(in_array ? 2 : 1, end - (in_array ? 2 : 1)));
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) { if (error) *error = "line " + std::to_string(n) + ": no '='"; return false; }
        if (in_array) continue;
        const std::string key = strip(line.substr(0, eq));
        std::string v = strip(line.substr(eq + 1));
        if (!v.empty() && v[0] == '"') {
            const size_t close = v.find('"', 1);
            if (close == std::string::npos) { if (error) *error = "line " + std::to_string(n) + ": unclosed string"; return false; }
            v = v.substr(1, close - 1);
        }
        got[(table.empty() ? "" : table + ".") + key] = v;
    }
    out->loaded = true;
    out->error.clear();
    for (const auto& kv : got) values()[out->path + "|" + kv.first] = kv.second;
    return true;
}

const Manifest& manifest() {
    static const Manifest m = [] {
        Manifest r;
        const char* env = std::getenv("NXRT_GAME_MANIFEST");
        r.path = env && *env ? std::string(env) : exe_dir() + "\\game.toml";
        std::string text, err;
        if (!read_file(r.path, &text)) { r.error = "cannot read the game manifest " + r.path; return r; }
        if (!parse_manifest(text, &r, &err)) { r.loaded = false; r.error = r.path + ": " + err; return r; }
        if (r.get("game.id").empty()) { r.loaded = false; r.error = r.path + ": no [game] id"; }
        return r;
    }();
    return m;
}

const game::Info& data_info() {
    // the strings outlive the Info (const char* fields)
    static std::string s_id, s_title, s_product, s_window, s_short, s_version, s_exe, s_ini, s_save;
    static const game::Info info = [] {
        const Manifest& m = manifest();
        game::Info g;
        s_id = m.get("game.id", "generic");
        s_title = m.get("game.title", "Unknown game");
        s_short = m.get("game.short_title", s_title);
        s_product = m.get("game.product", "Recomp module: " + s_id);
        s_window = m.get("game.window_title", s_short + " (unofficial recompilation, experimental)");
        s_version = m.get("game.version");
        s_exe = m.get("files.exe", s_id + ".exe");
        s_ini = m.get("files.ini", s_id + ".ini");
        s_save = m.get("files.save_folder", s_id + "-recomp");
        g.id = s_id.c_str();
        g.title = s_title.c_str();
        g.product = s_product.c_str();
        g.window_title = s_window.c_str();
        g.short_title = s_short.c_str();
        g.version = s_version.c_str();
        g.program_id = std::strtoull(m.get("game.title_id", "0").c_str(), nullptr, 16);
        g.languages = static_cast<uint32_t>(m.num("game.languages", 0x1ffff));
        g.platform = m.get("game.platform", "switch2") == "switch1" ? game::Platform::Switch1 : game::Platform::Switch2;
        g.exe_name = s_exe.c_str();
        g.game_ini = s_ini.c_str();
        g.save_folder = s_save.c_str();
        // the main module's load address is the same in every manifest make_manifest.py writes
        g.main_base = 0x7100000000ull;
        return g;
    }();
    return info;
}

}  // namespace nxrt::games::generic

void nxrt_register_game_generic_data() { nxrt::game::set_current(&nxrt::games::generic::data_info()); }
