// SPDX-License-Identifier: GPL-3.0-or-later
// The setup's games: the manifests it knows (recomp/games/<id>/game.toml, read at run time, so one setup exe serves every game), the
// player's library (<exe dir>\library.ini: which games were added, with their dump and data folder) and the identification of a dump
// (title id and version, from main.npdm or from the .nup package list).
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace setup {

// ---- game.toml (the subset the manifests use: [table], [[array]], key = "string" | number | true/false, # comments) ----------------
struct TomlTable {
    std::string name;                              // "game", "dump.file", "module", ...
    std::map<std::string, std::string> values;     // strings unquoted, numbers and booleans as written
    std::string get(const std::string& k, const std::string& def = {}) const {
        auto it = values.find(k);
        return it == values.end() ? def : it->second;
    }
};
struct Toml {
    std::vector<TomlTable> tables;                 // in file order; the keys before the first header are in a table named ""
    const TomlTable* first(const std::string& name) const;
    std::vector<const TomlTable*> all(const std::string& name) const;
    std::string get(const std::string& table, const std::string& key, const std::string& def = {}) const;
};
bool parse_toml(const std::string& text, Toml* out, std::string* error);

// ---- a game this setup knows (one manifest) ------------------------------------------------------------------------------------------
struct GameDef {
    std::wstring manifest;        // path of its game.toml
    std::string id, title, short_title, product, window_title, title_id, version, platform;
    std::string exe, ini, log, save_folder;      // [files]
    std::string runtime_dll;                      // [files] runtime_dll (release builds: the runtime as a DLL)
    std::string dump_version, display_version, main_build_id;   // [dump]
    std::string shader_extractor;                 // [shaders] extractor
    std::vector<std::string> modules;             // [[module]] names
};
bool load_game_def(const std::wstring& manifest, GameDef* out, std::string* error);

// Every manifest found: <exe dir>\games\<id>\game.toml (release folder) and, in a development build, <checkout>\recomp\games\<id>\game.toml.
// The first one per id wins.
std::vector<GameDef> find_game_defs(const std::wstring& exe_dir, const std::wstring& dev_root);

// ---- what a dump is -----------------------------------------------------------------------------------------------------------------------
struct DumpPackage {
    std::string kind;             // "base", "update", "addon"
    std::string title_id;         // 16 hex digits, upper case
    uint32_t version = 0;         // 65536 per x.0.0
};
struct DumpInfo {
    bool ok = false;
    std::string title_id;         // of the program (the base or the update)
    uint32_t version = 0;         // the newest update's, else 0
    std::string build_id;         // main's, when the code is readable (folder sources)
    std::vector<DumpPackage> packages;
    std::string name;             // a guess from the file or folder name ("Some Game [0100...].nup" -> "Some Game"), UTF-8
    std::wstring error;
};
std::string version_text(uint32_t v);        // 327680 -> "5.0.0"
// A folder (exefs\main.npdm directly or in updates\<n>\ or base\): read here. A .nup: python nup_extract.py --list.
DumpInfo identify_dump(const std::wstring& source, const std::wstring& python, const std::wstring& python_args, const std::wstring& nup_extract);
// The program id in a main.npdm (ACI0 +0x10), or empty.
std::string npdm_program_id(const std::wstring& npdm);

// ---- the player's library (<exe dir>\library.ini) -------------------------------------------------------------------------------------
struct LibraryEntry {
    std::string id;               // the manifest's game id; for a dump no manifest knows: "title-<title id>"
    std::string title_id;
    std::wstring source;          // the player's dump (.nup or folder)
    std::wstring data_dir;        // where the setup writes for this game
    std::string dump_version;     // what identify_dump found ("5.0.0"), informative
    std::string name;             // shown for a game no manifest knows (from the dump's file name)
};
struct Library {
    std::vector<LibraryEntry> games;
    std::string selected;         // id of the game the pages show
    LibraryEntry* find(const std::string& id);
};
std::wstring library_path();
Library load_library(const std::vector<GameDef>& defs);   // also takes over an old <id>_setup.ini of a known game
bool save_library(const Library& lib);
std::wstring default_data_dir(const std::string& id);     // <exe dir>\game_data\<id>

}  // namespace setup
