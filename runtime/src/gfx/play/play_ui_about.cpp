// SPDX-License-Identifier: GPL-3.0-or-later
// The About page of the F1 settings menu (play_ui.h draw_about_page): which build this is and where the files in use are. Every path is a
// read-only text field, so it can be selected and copied.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cctype>
#include <cfloat>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "imgui.h"
#include "nxrt/config.h"
#include "nxrt/game.h"
#include "nxrt/services.h"
#include "play_ui.h"

namespace nxrt::play {

namespace {

namespace fs = std::filesystem;

// A path as an absolute, readable string (relative paths resolve against the working directory, which is the game folder once installed).
std::string absolute(const std::string& path) {
    if (path.empty()) return {};
    std::error_code ec;
    const fs::path abs = fs::absolute(fs::path(path), ec);
    return ec ? path : abs.lexically_normal().string();
}

std::string exe_folder() {
    char buf[MAX_PATH * 2] = {};
    const DWORD n = GetModuleFileNameA(nullptr, buf, static_cast<DWORD>(sizeof buf));
    if (n == 0 || n >= sizeof buf) return {};
    return fs::path(buf).parent_path().string();
}

bool exists(const std::string& path) {
    std::error_code ec;
    return !path.empty() && fs::exists(path, ec);
}

// The shader cache folder the NVN back end opens: [nvn] shader_cache, else ./shader_cache/spirv, else shader_cache/spirv beside the executable
// (nvn_shaders.cpp shader_cache()). "not found" when none of them holds results.tsv.
std::string shader_cache_dir() {
    const std::string configured = config_get_str("nvn", "shader_cache", "");
    if (!configured.empty()) return absolute(configured);
    const std::string cwd = "./shader_cache/spirv";
    if (exists(cwd + "/results.tsv")) return absolute(cwd);
    const std::string exe = exe_folder();
    if (!exe.empty() && exists(exe + "/shader_cache/spirv/results.tsv")) return absolute(exe + "/shader_cache/spirv");
    return "not found";
}

// The ExeFS folder: [fs] exefs (the game's main() stores the folder it loaded there; the player may also set it in the game ini), else
// the old rule for mains that do not: <ID>_EXEFS-style variable, else ./exefs when it holds the main module, else game/exefs.
std::string exefs_dir() {
    if (const std::string configured = config_get_str("fs", "exefs", ""); !configured.empty()) return absolute(configured);
    std::string var = game::info().id ? game::info().id : "";
    for (char& c : var) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    var += "_EXEFS";
    if (const char* v = std::getenv(var.c_str()); v && *v) return absolute(v);
    if (exists("exefs/main")) return absolute("exefs");
    return absolute("game/exefs");
}

// One labelled, copyable path. `note` (may be empty) follows the field in grey.
void path_row(const char* label, const std::string& value, const char* note = "") {
    ImGui::PushID(label);
    ImGui::TextUnformatted(label);
    ImGui::SetNextItemWidth(-FLT_MIN);
    char buf[1024];
    std::snprintf(buf, sizeof buf, "%s", value.c_str());
    ImGui::InputText("##path", buf, sizeof buf, ImGuiInputTextFlags_ReadOnly);
    if (note && *note) ImGui::TextDisabled("%s", note);
    ImGui::PopID();
}

}  // namespace

void draw_about_page(const std::string& settings_path) {
    const game::Info& info = game::info();
    ImGui::SeparatorText("Build");
    ImGui::Text("%s", info.product && *info.product ? info.product : (info.title ? info.title : "nxrt"));
    ImGui::Text("Game version %s", info.version ? info.version : "?");
#ifdef NXRT_BUILD_COMMIT
    ImGui::Text("Runtime commit %s", NXRT_BUILD_COMMIT);
#else
    ImGui::TextDisabled("Runtime commit: not recorded in this build");
#endif
    ImGui::Text("Built %s %s", __DATE__, __TIME__);

    ImGui::SeparatorText("Folders and files in use");
    path_row("Working folder", absolute("."));
    path_row("Settings (controls)", absolute(settings_path), "Written by Apply and save.");
    if (info.game_ini && *info.game_ini) path_row("Game ini", absolute(info.game_ini), "Runtime options ([fs], [nvn], [audio] ...); docs/CONFIG.md lists the keys.");
    path_row("Log", absolute(config_get_str("log", "path", "nxrt.log")));
    path_row("RomFS", absolute(services::romfs_dir()), "[fs] romfs");
    path_row("ExeFS", exefs_dir(), "[fs] exefs; --exefs on the command line wins.");
    path_row("Save data", absolute(services::save_dir()), "[fs] save_dir");
    path_row("Shader cache", shader_cache_dir(), "[nvn] shader_cache");
    path_row("Pipeline cache", absolute(config_get_str("nvn", "pipeline_cache", "pipeline_cache.bin")), "[nvn] pipeline_cache; the pipeline keys sit beside it.");
    if (info.shader_pack && *info.shader_pack) path_row("Shader pack", absolute(info.shader_pack), "Made by the setup tool.");
}

}  // namespace nxrt::play
