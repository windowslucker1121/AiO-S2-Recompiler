// SPDX-License-Identifier: GPL-3.0-or-later
// The setup's games: manifests read at run time, the player's library and dump identification (setup_games.h).
#include "setup_games.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <sstream>

#include "setup_core.h"

namespace setup {

// ---- TOML subset ---------------------------------------------------------------------------------------------------------------------
const TomlTable* Toml::first(const std::string& name) const {
    for (const TomlTable& t : tables) if (t.name == name) return &t;
    return nullptr;
}
std::vector<const TomlTable*> Toml::all(const std::string& name) const {
    std::vector<const TomlTable*> v;
    for (const TomlTable& t : tables) if (t.name == name) v.push_back(&t);
    return v;
}
std::string Toml::get(const std::string& table, const std::string& key, const std::string& def) const {
    const TomlTable* t = first(table);
    return t ? t->get(key, def) : def;
}

namespace {
std::string strip(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    return s.substr(i);
}
}  // namespace

bool parse_toml(const std::string& text, Toml* out, std::string* error) {
    out->tables.clear();
    out->tables.push_back({});
    std::istringstream in(text);
    std::string line;
    int n = 0;
    while (std::getline(in, line)) {
        ++n;
        // cut a comment that is not inside a string
        bool q = false;
        for (size_t i = 0; i < line.size(); ++i) {
            if (line[i] == '"' && (i == 0 || line[i - 1] != '\\')) q = !q;
            else if (line[i] == '#' && !q) { line.resize(i); break; }
        }
        line = strip(line);
        if (line.empty()) continue;
        if (line[0] == '[') {
            const bool arr = line.size() > 1 && line[1] == '[';
            const size_t end = line.find(arr ? "]]" : "]");
            if (end == std::string::npos) { if (error) *error = "line " + std::to_string(n) + ": unclosed table header"; return false; }
            TomlTable t;
            t.name = strip(line.substr(arr ? 2 : 1, end - (arr ? 2 : 1)));
            out->tables.push_back(t);
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) { if (error) *error = "line " + std::to_string(n) + ": no '='"; return false; }
        const std::string key = strip(line.substr(0, eq));
        std::string v = strip(line.substr(eq + 1));
        if (!v.empty() && v[0] == '"') {
            std::string s;
            size_t i = 1;
            for (; i < v.size() && v[i] != '"'; ++i) {
                if (v[i] == '\\' && i + 1 < v.size()) {
                    const char c = v[++i];
                    s += c == 'n' ? '\n' : c == 't' ? '\t' : c;
                } else s += v[i];
            }
            if (i >= v.size()) { if (error) *error = "line " + std::to_string(n) + ": unclosed string"; return false; }
            v = s;
        }
        out->tables.back().values[key] = v;
    }
    return true;
}

// ---- manifests -----------------------------------------------------------------------------------------------------------------------
bool load_game_def(const std::wstring& manifest, GameDef* g, std::string* error) {
    Toml t;
    if (!parse_toml(read_file_text(manifest), &t, error)) return false;
    *g = GameDef{};
    g->manifest = manifest;
    g->id = t.get("game", "id");
    if (g->id.empty()) { if (error) *error = "no [game] id"; return false; }
    g->title = t.get("game", "title", g->id);
    g->short_title = t.get("game", "short_title", g->title);
    g->product = t.get("game", "product", "Recomp module: " + g->id);
    g->window_title = t.get("game", "window_title", g->short_title + " (unofficial recompilation)");
    g->title_id = t.get("game", "title_id");
    std::transform(g->title_id.begin(), g->title_id.end(), g->title_id.begin(), [](char c) { return static_cast<char>(toupper(static_cast<unsigned char>(c))); });
    g->version = t.get("game", "version");
    g->platform = t.get("game", "platform", "switch2");
    g->exe = t.get("files", "exe", g->id + ".exe");
    g->runtime_dll = t.get("files", "runtime_dll", g->id + "_rt.dll");
    g->ini = t.get("files", "ini", g->id + ".ini");
    g->log = t.get("files", "log", g->id + ".log");
    g->save_folder = t.get("files", "save_folder", g->id);
    g->dump_version = t.get("dump", "version", g->version);
    g->display_version = t.get("dump", "display_version", g->version);
    g->main_build_id = t.get("dump", "main_build_id");
    g->shader_extractor = t.get("shaders", "extractor", "generic");
    for (const TomlTable* m : t.all("module")) g->modules.push_back(m->get("name"));
    g->generic = t.get("game", "generic") == "true";
    g->is_template = t.get("game", "template") == "true";
    return true;
}

std::vector<GameDef> find_game_defs(const std::wstring& exe_dir, const std::wstring& dev_root) {
    std::vector<GameDef> out;
    std::vector<std::wstring> roots = {join(exe_dir, L"games"), join(exe_dir, L"experimental")};
    if (!dev_root.empty()) roots.push_back(join(dev_root, L"recomp\\games"));
    for (const std::wstring& r : roots) {
        WIN32_FIND_DATAW fd;
        const HANDLE h = FindFirstFileW(join(r, L"*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
            const std::wstring m = join(join(r, fd.cFileName), L"game.toml");
            GameDef g;
            if (!file_exists(m) || !load_game_def(m, &g, nullptr) || g.is_template) continue;
            if (std::none_of(out.begin(), out.end(), [&](const GameDef& o) { return o.id == g.id; })) out.push_back(g);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(out.begin(), out.end(), [](const GameDef& a, const GameDef& b) { return a.title < b.title; });
    return out;
}

// ---- dumps ---------------------------------------------------------------------------------------------------------------------------
std::string version_text(uint32_t v) {
    char b[32];
    std::snprintf(b, sizeof b, "%u.%u.%u", v >> 16, (v >> 8) & 255, v & 255);
    return b;
}

std::string npdm_program_id(const std::wstring& npdm) {
    const std::string d = read_file_text(npdm);
    if (d.size() < 0x80 || d.compare(0, 4, "META") != 0) return {};
    uint32_t aci = 0;
    std::memcpy(&aci, d.data() + 0x70, 4);
    if (aci + 0x18 > d.size() || d.compare(aci, 4, "ACI0") != 0) return {};
    uint64_t pid = 0;
    std::memcpy(&pid, d.data() + aci + 0x10, 8);
    char b[24];
    std::snprintf(b, sizeof b, "%016llX", static_cast<unsigned long long>(pid));
    return b;
}

namespace {
// "Some Game Name [0100000000000000][v0].nup" -> "Some Game Name"
std::string name_from_path(const std::wstring& source) {
    std::wstring n = source;
    while (!n.empty() && (n.back() == L'\\' || n.back() == L'/')) n.pop_back();
    const size_t sl = n.find_last_of(L"\\/");
    if (sl != std::wstring::npos) n = n.substr(sl + 1);
    if (n.size() > 4 && _wcsicmp(n.c_str() + n.size() - 4, L".nup") == 0) n.resize(n.size() - 4);
    std::wstring out;
    int depth = 0;
    for (wchar_t c : n) {
        if (c == L'[' || c == L'(') { ++depth; continue; }
        if (c == L']' || c == L')') { depth = std::max(0, depth - 1); continue; }
        if (!depth) out += c == L'_' ? L' ' : c;
    }
    while (!out.empty() && out.back() == L' ') out.pop_back();
    while (!out.empty() && out.front() == L' ') out.erase(0, 1);
    return narrow(out);
}
}  // namespace

DumpInfo identify_dump(const std::wstring& source, const std::wstring& python, const std::wstring& python_args, const std::wstring& nup_extract) {
    DumpInfo d;
    d.name = name_from_path(source);
    if (dir_exists(source)) {
        // the folder itself, the newest updates\<n>, or base
        std::wstring exefs;
        if (file_exists(join(source, L"exefs\\main.npdm"))) exefs = join(source, L"exefs");
        uint32_t best = 0;
        bool have_update = false;
        WIN32_FIND_DATAW fd;
        const HANDLE h = FindFirstFileW(join(source, L"updates\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !iswdigit(fd.cFileName[0])) continue;
                const uint32_t v = static_cast<uint32_t>(wcstoul(fd.cFileName, nullptr, 10));
                const std::wstring e = join(source, std::wstring(L"updates\\") + fd.cFileName + L"\\exefs");
                if (file_exists(join(e, L"main.npdm")) && (!have_update || v > best)) { best = v; have_update = true; exefs = e; }
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        if (exefs.empty() && file_exists(join(source, L"base\\exefs\\main.npdm"))) exefs = join(source, L"base\\exefs");
        if (exefs.empty()) { d.error = L"no exefs\\main.npdm in the folder (also looked in updates\\<version>\\ and base\\)"; return d; }
        d.title_id = npdm_program_id(join(exefs, L"main.npdm"));
        d.version = have_update ? best : 0;
        const GameId g = game_id_of(parent_of(exefs));
        if (g.ok) d.build_id = g.build_id;
        d.ok = !d.title_id.empty();
        if (!d.ok) d.error = L"main.npdm could not be read";
        return d;
    }
    if (!file_exists(source)) { d.error = L"not found"; return d; }
    if (python.empty() || nup_extract.empty()) { d.error = L"Python or nup_extract.py is missing (Requirements)"; return d; }
    std::string out;
    DWORD code = 1;
    const std::wstring args = (python_args.empty() ? L"" : python_args + L" ") + L"-I " + quote(nup_extract) + L" --list " + quote(source);
    if (!run_capture(python, args, 60000, &out, &code) || code != 0) {
        d.error = L"nup_extract.py --list failed: " + widen(out.substr(0, 300));
        return d;
    }
    // "base    010015100B514000 v0 (0.0.0)  exefs=4 romfs=19012"
    std::istringstream in(out);
    std::string line;
    while (std::getline(in, line)) {
        char kind[16] = {}, tid[24] = {};
        unsigned v = 0;
        if (std::sscanf(line.c_str(), "%15s %23s v%u", kind, tid, &v) != 3) continue;
        DumpPackage p{kind, tid, v};
        std::transform(p.title_id.begin(), p.title_id.end(), p.title_id.begin(), [](char c) { return static_cast<char>(toupper(static_cast<unsigned char>(c))); });
        d.packages.push_back(p);
    }
    for (const DumpPackage& p : d.packages) {
        if (p.kind == "base" && d.title_id.empty()) d.title_id = p.title_id;
        if (p.kind == "update" && p.version >= d.version) { d.version = p.version; if (d.title_id.empty()) d.title_id = p.title_id; }
    }
    d.ok = !d.title_id.empty();
    if (!d.ok) d.error = L"the package list names no game";
    return d;
}

// ---- library -------------------------------------------------------------------------------------------------------------------------
LibraryEntry* Library::find(const std::string& id) {
    for (LibraryEntry& e : games) if (e.id == id) return &e;
    return nullptr;
}

std::wstring library_path() { return join(exe_dir(), L"library.ini"); }
std::wstring default_data_dir(const std::string& id) { return join(join(exe_dir(), L"game_data"), widen(id)); }
std::wstring experimental_dir() { return join(exe_dir(), L"experimental"); }
std::string experimental_id(const std::string& title_id) {
    std::string id = "t" + title_id;
    std::transform(id.begin(), id.end(), id.begin(), [](char c) { return static_cast<char>(tolower(static_cast<unsigned char>(c))); });
    return id;
}

Library load_library(const std::vector<GameDef>& defs) {
    // [library] selected = <id>;  [game <id>] title_id / source / data_dir / dump_version
    Library lib;
    std::istringstream in(read_file_text(library_path()));
    std::string line;
    LibraryEntry* cur = nullptr;
    while (std::getline(in, line)) {
        line = strip(line);
        if (line.empty() || line[0] == ';') continue;
        if (line[0] == '[') {
            const std::string h = strip(line.substr(1, line.find(']') - 1));
            cur = nullptr;
            if (h.rfind("game ", 0) == 0) {
                lib.games.push_back({});
                cur = &lib.games.back();
                cur->id = strip(h.substr(5));
            }
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = strip(line.substr(0, eq)), v = strip(line.substr(eq + 1));
        if (!cur) { if (k == "selected") lib.selected = v; continue; }
        if (k == "title_id") cur->title_id = v;
        else if (k == "source") cur->source = widen(v);
        else if (k == "data_dir") cur->data_dir = widen(v);
        else if (k == "dump_version") cur->dump_version = v;
        else if (k == "name") cur->name = v;
    }
    // A setup of one game before the library existed kept its choices in <exe dir>\<id>_setup.ini and wrote to <exe dir>\game_data.
    for (const GameDef& g : defs) {
        if (lib.find(g.id)) continue;
        const std::wstring old = join(exe_dir(), widen(g.id + "_setup.ini"));
        if (!file_exists(old)) continue;
        LibraryEntry e;
        e.id = g.id;
        e.title_id = g.title_id;
        std::istringstream o(read_file_text(old));
        while (std::getline(o, line)) {
            const size_t eq = line.find('=');
            if (eq == std::string::npos || line[0] == ';') continue;
            const std::string k = strip(line.substr(0, eq)), v = strip(line.substr(eq + 1));
            if (k == "data_dir") e.data_dir = widen(v);
            else if (k == "source") e.source = widen(v);
        }
        if (e.data_dir.empty()) e.data_dir = join(exe_dir(), L"game_data");
        lib.games.push_back(e);
    }
    for (LibraryEntry& e : lib.games) if (e.data_dir.empty()) e.data_dir = default_data_dir(e.id);
    if (!lib.find(lib.selected)) lib.selected = lib.games.empty() ? std::string() : lib.games.front().id;
    return lib;
}

bool save_library(const Library& lib) {
    std::string t = "; The games added to the setup: each one's dump and the folder the setup writes to for it.\n[library]\n";
    t += "selected = " + lib.selected + "\n";
    for (const LibraryEntry& e : lib.games) {
        t += "\n[game " + e.id + "]\n";
        t += "title_id = " + e.title_id + "\n";
        t += "source = " + narrow(e.source) + "\n";
        t += "data_dir = " + narrow(e.data_dir) + "\n";
        if (!e.dump_version.empty()) t += "dump_version = " + e.dump_version + "\n";
        if (!e.name.empty()) t += "name = " + e.name + "\n";
    }
    return write_file_atomic(library_path(), t);
}

}  // namespace setup
