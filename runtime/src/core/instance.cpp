// SPDX-License-Identifier: GPL-3.0-or-later
// Named instances (nxrt/instance.h): the folder of a run's own files and the configuration that sends every write there.
#define _SILENCE_CXX20_U8PATH_DEPRECATION_WARNING
#include "nxrt/instance.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>

#include "nxrt/config.h"
#include "nxrt/log.h"

namespace nxrt {
namespace {

namespace fs = std::filesystem;

std::string g_name, g_dir;

fs::path u8(const std::string& s) { return fs::u8path(s); }
std::string str(const fs::path& p) {
    const std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}
bool is_absolute(const std::string& p) {
    if (p.size() >= 2 && (p[0] == '\\' || p[0] == '/') && (p[1] == '\\' || p[1] == '/')) return true;   // UNC
    return u8(p).is_absolute();
}

// Two names of one file (a hard link): same volume and file index.
bool same_file(const std::string& a, const std::string& b) {
    auto id = [](const std::string& p, BY_HANDLE_FILE_INFORMATION* i) {
        HANDLE h = CreateFileW(u8(p).c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        const bool ok = GetFileInformationByHandle(h, i) != 0;
        CloseHandle(h);
        return ok;
    };
    BY_HANDLE_FILE_INFORMATION x = {}, y = {};
    return id(a, &x) && id(b, &y) && x.dwVolumeSerialNumber == y.dwVolumeSerialNumber && x.nFileIndexHigh == y.nFileIndexHigh &&
           x.nFileIndexLow == y.nFileIndexLow;
}

std::string exe_path() {
    char exe[1024] = {};
    const DWORD n = GetModuleFileNameA(nullptr, exe, sizeof exe);
    return std::string(exe, n < sizeof exe ? n : sizeof exe - 1);
}

// Sets a key to `value` where its effective value comes from: the environment (NXRT_<SECTION>_<KEY>, which beats the ini) or the ini.
void put(const char* section, const char* key, const std::string& value) {
    const std::string env = config_env_name(section, key);
    if (std::getenv(env.c_str())) _putenv_s(env.c_str(), value.c_str());
    else config_set(section, key, value.c_str());
}

// An output: a relative value moves into the instance folder; unset, it gets `def` there (nullptr: stays unset, the output is off).
void output(const char* section, const char* key, const char* def) {
    if (config_has(section, key)) {
        const std::string v = config_get_str(section, key, "");
        if (!v.empty() && !is_absolute(v)) put(section, key, instance_file(v));
    } else if (def) {
        config_set(section, key, instance_file(def).c_str());
    }
}

// A file the run reads and writes back (relative to the game folder by default): the instance uses its own copy, made from the shared
// file the first time. An absolute value is kept (the caller asked for that file); an empty one stays off.
void own_copy(const char* section, const char* key, const char* def) {
    std::string shared = def;
    if (config_has(section, key)) {
        shared = config_get_str(section, key, "");
        if (shared.empty() || is_absolute(shared)) return;
    }
    const std::string own = instance_file(shared);
    std::error_code ec;
    if (!fs::exists(u8(own), ec) && fs::is_regular_file(u8(shared), ec)) {
        fs::create_directories(u8(own).parent_path(), ec);
        if (CopyFileW(u8(shared).c_str(), u8(own).c_str(), TRUE))
            NXLOG_I("instance", "%s: own copy of %s", own.c_str(), str(fs::absolute(u8(shared), ec)).c_str());
    }
    put(section, key, own);
}

}  // namespace

void instance_set(const std::string& name, const std::string& dir) {
    g_name = name;
    g_dir.clear();
    if (name.empty()) return;
    std::error_code ec;
    fs::path p = fs::absolute(u8(dir), ec).lexically_normal();
    std::string d = str(p);
    while (d.size() > 3 && (d.back() == '\\' || d.back() == '/')) d.pop_back();
    g_dir = d;
    fs::create_directories(u8(g_dir), ec);
}

const std::string& instance_name() { return g_name; }
const std::string& instance_dir() { return g_dir; }
bool instance_active() { return !g_name.empty(); }

std::string instance_report_path(const char* suffix) {
    std::string p = exe_path();
    const size_t slash = p.find_last_of("\\/");
    const size_t dot = p.find_last_of('.');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) p.erase(dot);
    if (!instance_active()) return p + suffix;
    return g_dir + "\\" + (slash == std::string::npos ? p : p.substr(slash + 1)) + suffix;
}

std::string instance_request_dir() {
    if (instance_active()) return g_dir;
    const std::string p = exe_path();
    const size_t slash = p.find_last_of("\\/");
    return slash == std::string::npos ? "." : p.substr(0, slash);
}

std::string instance_file(const std::string& path) {
    if (!instance_active() || path.empty() || is_absolute(path)) return path;
    return str(u8(g_dir) / u8(path));
}

void instance_apply_config() {
    if (!instance_active()) return;
    NXLOG_I("instance", "instance '%s': this run's own files are in %s (game data, shader cache and BC7 cache stay shared)", g_name.c_str(), g_dir.c_str());
    // Save data and temporary storage (default %APPDATA%\<save folder>\save): the instance starts with none of its own.
    output("fs", "save_dir", "save");
    if (const std::string s = config_get_str("fs", "save_dir", ""); s.rfind(g_dir, 0) != 0)
        NXLOG_W("instance", "[fs] save_dir is %s: shared with every run that uses it", s.c_str());
    // Outputs that have a default file or folder.
    output("album", "dir", "album");
    output("nvn", "shot_dir", "shots");
    output("test", "out", "testout");
    output("prof", "sample_out", "prof_samples.folded");
    // Outputs that are off unless a path is given.
    static const char* const kOff[][2] = {{"test", "shot_dir"}, {"test", "record"}, {"diag", "soak_csv"}, {"diag", "frame_csv"}, {"prof", "trace"}, {"prof", "sample_raw"}, {"time", "csv"},
                                          {"audio", "wav_dump"}, {"nvn", "dump_missing"}, {"nvn", "dump_astc"}};
    for (const auto& k : kOff) output(k[0], k[1], nullptr);
    // Read and written back: the instance's own copies.
    own_copy("play", "controls", "controls.ini");
    own_copy("nvn", "pipeline_keys", "pipeline_keys.bin");
    own_copy("nvn", "shader_order", "shader_order.txt");
    // The "Preparing shaders" window translates into the shared shader cache and rewrites the shared pipeline cache and prepare.state:
    // the default run (or --prepare-shaders) does that.
    if (!config_has("nvn", "setup_on_start")) config_set("nvn", "setup_on_start", "0");
    // The marker of an instance that writes its pipeline cache back (instance_keep_pipeline_cache): the start-up sweep of other runs skips it.
    std::error_code ec;
    const fs::path marker = u8(g_dir) / "keep_pipeline_cache";
    if (instance_keep_pipeline_cache()) {
        if (FILE* f = _wfopen(marker.c_str(), L"wb")) { std::fputs("[instance] keep_pipeline_cache = 1: this folder's pipeline_cache.bin is kept\n", f); std::fclose(f); }
    } else {
        fs::remove(marker, ec);
    }
}

bool instance_keep_pipeline_cache() {
    return instance_active() && config_get_bool("instance", "keep_pipeline_cache", false);
}

void instance_link_pipeline_cache() {
    if (!instance_active()) return;
    std::error_code ec;
    std::string shared;
    if (config_has("nvn", "pipeline_cache")) {
        shared = config_get_str("nvn", "pipeline_cache", "");
        if (shared.empty() || is_absolute(shared)) return;
    } else {
        // The default of nvn_pipeline.cpp: pipeline_cache.bin beside the shader cache folder, else in the working folder.
        std::string cache = config_get_str("nvn", "shader_cache", "");
        if (cache.empty()) {
            const std::string p = exe_path();
            const size_t slash = p.find_last_of("\\/");
            for (const std::string& c : {std::string("./shader_cache/spirv"), (slash == std::string::npos ? std::string(".") : p.substr(0, slash)) + "/shader_cache/spirv"})
                if (fs::exists(u8(c + "/results.tsv"), ec)) { cache = c; break; }
        }
        shared = "pipeline_cache.bin";
        if (!cache.empty()) {
            const fs::path beside = u8(cache).parent_path() / "pipeline_cache.bin";
            if (fs::exists(beside, ec) || !fs::exists(u8(shared), ec)) shared = str(beside);
        }
    }
    const std::string own = str(u8(g_dir) / "pipeline_cache.bin");
    if (fs::is_regular_file(u8(shared), ec)) {
        const auto t_shared = fs::last_write_time(u8(shared), ec);
        std::error_code e2;
        const bool have = fs::exists(u8(own), e2);
        const bool older = have && fs::last_write_time(u8(own), e2) < t_shared;
        // an instance that does not keep its cache (the default) always starts from the shared one: a private file left by an older build
        // or a kept run goes
        const bool stale = have && !instance_keep_pipeline_cache() && !same_file(own, shared);
        if (!have || older || stale) {
            // A hard link costs nothing (gigabytes after a shader preparation); the instance's first save writes a file of its own under
            // the same name and leaves the shared one alone. Another volume: a copy.
            fs::remove(u8(own), e2);
            const bool linked = CreateHardLinkW(u8(own).c_str(), u8(shared).c_str(), nullptr) != 0;
            const bool copied = !linked && CopyFileW(u8(shared).c_str(), u8(own).c_str(), FALSE);
            NXLOG_I("instance", "pipeline cache %s: %s %s%s", own.c_str(), linked ? "hard link to" : copied ? "copy of" : "could not take", shared.c_str(),
                    older ? " (the shared one is newer)" : "");
        }
    }
    put("nvn", "pipeline_cache", own);
}

}  // namespace nxrt
