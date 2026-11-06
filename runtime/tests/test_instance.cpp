// SPDX-License-Identifier: GPL-3.0-or-later
// Named instances (nxrt/instance.h, src/core/instance.cpp): the paths of a run's own files, the configuration an instance gets, its own
// copies of the shared read-and-write files and the pipeline cache link.
#include "test_framework.h"

#include "nxrt/config.h"
#include "nxrt/instance.h"

#include <windows.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

NXTEST_GROUP(instance)

using namespace nxrt;
namespace fs = std::filesystem;

namespace {

const char* const kEnv[] = {"NXRT_FS_SAVE_DIR", "NXRT_PLAY_CONTROLS", "NXRT_NVN_PIPELINE_CACHE", "NXRT_NVN_PIPELINE_KEYS", "NXRT_NVN_SHADER_ORDER",
                            "NXRT_NVN_SHADER_CACHE", "NXRT_NVN_SETUP_ON_START", "NXRT_DIAG_SOAK_CSV", "NXRT_TEST_OUT", "NXRT_TEST_SHOT_DIR",
                            "NXRT_ALBUM_DIR", "NXRT_NVN_SHOT_DIR", "NXRT_PROF_SAMPLE_OUT", "NXRT_PROF_TRACE", "NXRT_TIME_CSV"};

// A scratch game folder as the working folder, an empty configuration and none of the variables above; all of it undone at the end.
struct Scratch {
    fs::path old_cwd, root;
    Scratch() {
        old_cwd = fs::current_path();
        root = fs::temp_directory_path() / ("nxrt_instance_" + std::to_string(GetCurrentProcessId()));
        std::error_code ec;
        fs::remove_all(root, ec);
        fs::create_directories(root / "shader_cache" / "spirv");
        fs::current_path(root);
        reset();
    }
    ~Scratch() {
        reset();
        fs::current_path(old_cwd);
        std::error_code ec;
        fs::remove_all(root, ec);
    }
    static void reset() {
        instance_set("", "");
        config_clear();
        for (const char* v : kEnv) _putenv_s(v, "");     // an empty value removes the variable
    }
};

void write(const fs::path& p, const std::string& s) { std::ofstream(p, std::ios::binary) << s; }
std::string read(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}
std::string exe_stem() {
    char b[1024] = {};
    GetModuleFileNameA(nullptr, b, sizeof b);
    return fs::path(b).stem().string();
}

}  // namespace

NXTEST(default_run_keeps_every_path) {
    Scratch s;
    CHECK(!instance_active());
    CHECK(instance_name().empty() && instance_dir().empty());
    CHECK(instance_file("shots/a.png") == "shots/a.png");
    char exe[1024] = {};
    GetModuleFileNameA(nullptr, exe, sizeof exe);
    CHECK(instance_report_path("_crash.txt") == (fs::path(exe).parent_path() / (exe_stem() + "_crash.txt")).string());
    CHECK(fs::path(instance_request_dir()) == fs::path(exe).parent_path());
    instance_apply_config();
    instance_link_pipeline_cache();
    CHECK(!config_has("fs", "save_dir"));
    CHECK(!config_has("nvn", "setup_on_start"));
    CHECK(!config_has("nvn", "pipeline_cache"));
}

NXTEST(instance_paths_go_to_its_folder) {
    Scratch s;
    instance_set("a8", (s.root / "instances" / "a8" / "").string());     // a trailing separator is dropped
    CHECK(instance_active());
    CHECK(instance_name() == "a8");
    CHECK(fs::path(instance_dir()) == s.root / "instances" / "a8");
    CHECK(fs::is_directory(instance_dir()));                                // created
    CHECK(fs::path(instance_file("shot_0.png")) == fs::path(instance_dir()) / "shot_0.png");
    CHECK(instance_file("C:\\elsewhere\\x.png") == "C:\\elsewhere\\x.png");  // absolute: kept
    CHECK(instance_file("\\\\server\\share\\x.png") == "\\\\server\\share\\x.png");
    CHECK(instance_file("").empty());
    CHECK(instance_report_path("_freeze.txt") == instance_dir() + "\\" + exe_stem() + "_freeze.txt");
    CHECK(instance_request_dir() == instance_dir());
}

NXTEST(instance_configuration_and_own_copies) {
    Scratch s;
    write(s.root / "controls.ini", "[tweaks]\nlanguage = de\n");
    write(s.root / "pipeline_keys.bin", "KEYS");
    config_load_string("[diag]\nsoak_csv = soak.csv\n[prof]\ntrace = C:\\abs\\trace.json\n");
    _putenv_s("NXRT_TEST_SHOT_DIR", "myshots");
    instance_set("b", (s.root / "instances" / "b").string());
    instance_apply_config();
    const fs::path d = instance_dir();
    CHECK(fs::path(config_get_str("fs", "save_dir")) == d / "save");
    CHECK(fs::path(config_get_str("album", "dir")) == d / "album");
    CHECK(fs::path(config_get_str("test", "out")) == d / "testout");
    CHECK(fs::path(config_get_str("diag", "soak_csv")) == d / "soak.csv");          // a relative output moves into the folder
    CHECK(config_get_str("prof", "trace") == "C:\\abs\\trace.json");                 // an absolute one stays
    CHECK(std::getenv("NXRT_TEST_SHOT_DIR") && fs::path(std::getenv("NXRT_TEST_SHOT_DIR")) == d / "myshots");   // set in the environment
    CHECK(!config_has("time", "csv"));                                               // an output that is off stays off
    CHECK(!config_get_bool("nvn", "setup_on_start", true));                          // the setup window writes shared data
    CHECK(fs::path(config_get_str("play", "controls")) == d / "controls.ini");
    CHECK(read(d / "controls.ini") == "[tweaks]\nlanguage = de\n");                  // copied from the game folder the first time
    CHECK(fs::path(config_get_str("nvn", "pipeline_keys")) == d / "pipeline_keys.bin");
    CHECK(read(d / "pipeline_keys.bin") == "KEYS");
    CHECK(fs::path(config_get_str("nvn", "shader_order")) == d / "shader_order.txt");
    CHECK(!fs::exists(d / "shader_order.txt"));                                      // nothing shared to copy: the run starts its own
    // The next start keeps the instance's own copy, and an explicit setup_on_start wins.
    write(d / "controls.ini", "[tweaks]\nlanguage = fr\n");
    config_clear();
    config_set("nvn", "setup_on_start", "1");
    _putenv_s("NXRT_TEST_SHOT_DIR", "");
    instance_apply_config();
    CHECK(read(d / "controls.ini") == "[tweaks]\nlanguage = fr\n");
    CHECK(config_get_bool("nvn", "setup_on_start", false));
}

NXTEST(instance_pipeline_cache_links_to_the_shared_one) {
    Scratch s;
    const fs::path shared = s.root / "shader_cache" / "pipeline_cache.bin";
    write(s.root / "shader_cache" / "spirv" / "results.tsv", "hash\n");
    write(shared, "PREPARED");
    const std::string cache = (s.root / "shader_cache" / "spirv").string();
    config_set("nvn", "shader_cache", cache.c_str());
    instance_set("c", (s.root / "instances" / "c").string());
    instance_link_pipeline_cache();
    const fs::path own = fs::path(instance_dir()) / "pipeline_cache.bin";
    CHECK(fs::path(config_get_str("nvn", "pipeline_cache")) == own);
    CHECK(read(own) == "PREPARED");
    CHECK(fs::hard_link_count(own) == 2);                                           // a link, not a copy of gigabytes
    // The instance's save replaces its own name only (a temporary file renamed over it, as nvn_pipeline.cpp does).
    write(own.string() + ".tmp", "PREPARED+MINE");
    fs::rename(own.string() + ".tmp", own);
    CHECK(read(shared) == "PREPARED");
    // The next start keeps the instance's newer file ...
    config_clear();
    config_set("nvn", "shader_cache", cache.c_str());
    instance_link_pipeline_cache();
    CHECK(read(own) == "PREPARED+MINE");
    // ... until the shared one is newer (a new shader preparation): then it links again.
    fs::last_write_time(shared, fs::file_time_type::clock::now() + std::chrono::hours(1));
    config_clear();
    config_set("nvn", "shader_cache", cache.c_str());
    instance_link_pipeline_cache();
    CHECK(read(own) == "PREPARED");
    // An absolute [nvn] pipeline_cache is the caller's choice: left alone.
    config_set("nvn", "pipeline_cache", "C:\\elsewhere\\p.bin");
    instance_link_pipeline_cache();
    CHECK(config_get_str("nvn", "pipeline_cache") == "C:\\elsewhere\\p.bin");
}
