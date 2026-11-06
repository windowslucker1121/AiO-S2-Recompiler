// SPDX-License-Identifier: GPL-3.0-or-later
// Shader preparation end to end (src/gfx/nvn/nvn_gpl.cpp, section "shader preparation"; the public nxrt::nvn::prepare_shaders of
// include/nxrt/nvn_backend.h; the stub lookups ShaderCache::load_stub / release_stub_spirv of src/gfx/nvn/nvn_shaders.cpp): a program list
// ("MPRPRG01") is read, the pipeline-library parts of each program are built and dropped, and prepare.state is written next to the list.
// The programs are real: the first six records of work/shader_cache/programs.bin, or, when that file is not on this machine, six vertex
// and six fragment shaders of work/shader_cache/spirv/results.tsv. Skipped when work/shader_cache is not on this machine, like test_nvn_gpl.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include "nvn_app.h"
#include "nvn_pipeline.h"
#include "nvn_shaders.h"
#include "nxrt/config.h"
#include "nxrt/nvn_backend.h"
NXTEST_GROUP(gfx_nvn_prepare)

using namespace nvnapp;
namespace fs = std::filesystem;
namespace gpu = nxrt::nvn::be::gpu;
using nxrt::nvn::be::ShaderInfo;
using nxrt::nvn::be::ShaderStage;

namespace {

// One program of the list: vertex, fragment and geometry code hashes (0 = none).
struct Prog {
    uint64_t vs, fs, gs;
};

// A folder for the program list and the state files, removed when the case ends (declare the App of the case after it).
struct TempDir {
    fs::path path;
    TempDir() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::temp_directory_path() / ("nxrt_gfx_prepare_" + std::to_string(GetCurrentProcessId()) + "_" + std::to_string(stamp));
        std::error_code ec;
        fs::create_directories(path, ec);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

// [nvn] settings for one case: the key and cache files in the temp folder, prewarm off (a prewarm worker could build a pipeline and write
// the pipeline cache there), gpl on. Puts back what it found when the case ends, on any return.
struct PrepareConfig {
    std::string keys, cache, prewarm, gpl;
    explicit PrepareConfig(const fs::path& dir)
        : keys(nxrt::config_get_str("nvn", "pipeline_keys", "pipeline_keys.bin")),
          cache(nxrt::config_get_str("nvn", "pipeline_cache", "pipeline_cache.bin")),
          prewarm(nxrt::config_get_str("nvn", "prewarm", "1")),
          gpl(nxrt::config_get_str("nvn", "gpl", "1")) {
        const std::string k = (dir / "pipeline_keys.bin").string(), c = (dir / "pipeline_cache.bin").string();
        nxrt::config_set("nvn", "pipeline_keys", k.c_str());
        nxrt::config_set("nvn", "pipeline_cache", c.c_str());
        nxrt::config_set("nvn", "prewarm", "0");
        nxrt::config_set("nvn", "gpl", "1");
        nxrt::config_set("nvn", "prepare_cache", "1");
    }
    ~PrepareConfig() {
        nxrt::config_set("nvn", "pipeline_keys", keys.c_str());
        nxrt::config_set("nvn", "pipeline_cache", cache.c_str());
        nxrt::config_set("nvn", "prewarm", prewarm.c_str());
        nxrt::config_set("nvn", "gpl", gpl.c_str());
    }
};

std::string hex16(uint64_t h) {
    char b[20];
    std::snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(h));
    return b;
}

void put_u32(std::ofstream& f, uint32_t v) {
    char b[4];
    for (int i = 0; i < 4; ++i) b[i] = static_cast<char>((v >> (8 * i)) & 0xff);
    f.write(b, 4);
}

void put_u64(std::ofstream& f, uint64_t v) {
    char b[8];
    for (int i = 0; i < 8; ++i) b[i] = static_cast<char>((v >> (8 * i)) & 0xff);
    f.write(b, 8);
}

uint32_t get_u32(const unsigned char* p) {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(p[i]) << (8 * i);
    return v;
}

uint64_t get_u64(const unsigned char* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(p[i]) << (8 * i);
    return v;
}

// Writes a program list in the format gpl_prepare reads: "MPRPRG01", u32 count, u32 0, then three u64 per program.
bool write_programs(const fs::path& p, const std::vector<Prog>& progs) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write("MPRPRG01", 8);
    put_u32(f, static_cast<uint32_t>(progs.size()));
    put_u32(f, 0);
    for (const Prog& g : progs) {
        put_u64(f, g.vs);
        put_u64(f, g.fs);
        put_u64(f, g.gs);
    }
    return static_cast<bool>(f);
}

// The first `n` records of a real program list. False when the file is missing, has another magic or fewer records.
bool read_first_programs(const fs::path& p, size_t n, std::vector<Prog>* out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    unsigned char head[16];
    f.read(reinterpret_cast<char*>(head), 16);
    if (!f || std::memcmp(head, "MPRPRG01", 8) != 0) return false;
    if (get_u32(head + 8) < n) return false;
    out->clear();
    for (size_t i = 0; i < n; ++i) {
        unsigned char r[24];
        f.read(reinterpret_cast<char*>(r), 24);
        if (!f) return false;
        out->push_back({get_u64(r), get_u64(r + 8), get_u64(r + 16)});
    }
    return true;
}

// Six vertex and six fragment hashes from the harness's record (results.tsv: hash in column 1, stage in column 3), each with its .spv here.
bool pick_from_results(const fs::path& dir, std::vector<uint64_t>* vs, std::vector<uint64_t>* fsh) {
    std::ifstream f(dir / "results.tsv");
    if (!f) return false;
    std::string line;
    std::getline(f, line);  // header
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::vector<std::string> cols;
        std::stringstream ss(line);
        std::string c;
        while (std::getline(ss, c, '\t')) cols.push_back(c);
        if (cols.size() < 3 || cols[0].size() != 16) continue;
        if (!fs::exists(dir / (cols[0] + ".spv"))) continue;
        const uint64_t h = std::strtoull(cols[0].c_str(), nullptr, 16);
        if (cols[2] == "vertex" && vs->size() < 6) vs->push_back(h);
        else if (cols[2] == "fragment" && fsh->size() < 6) fsh->push_back(h);
        if (vs->size() == 6 && fsh->size() == 6) break;
    }
    return vs->size() == 6 && fsh->size() == 6;
}

// The six programs of the cases: the first six records of the real list when it is here, else six vertex/fragment pairs from results.tsv.
bool real_programs(std::vector<Prog>* out) {
    const fs::path cache_dir = shader_cache_dir();
    if (read_first_programs(cache_dir.parent_path() / "programs.bin", 6, out)) return true;
    std::vector<uint64_t> vs, fsh;
    if (!pick_from_results(cache_dir, &vs, &fsh)) return false;
    out->clear();
    for (size_t i = 0; i < 6; ++i) out->push_back({vs[i], fsh[i], 0});
    return true;
}

std::string read_text(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// The back end on a small headless device, for one case. False (with the reason printed) when it cannot start.
bool start(App& app) {
    App::Opts o;
    o.width = 64;
    o.height = 48;
    if (!app.init(o)) {
        std::printf("    app init failed: %s\n", app.error.c_str());
        return false;
    }
    return true;
}

}  // namespace

// The shader stub lookups the preparation uses: a real vertex shader loads with its reflection, the same object comes back on a second
// call, its SPIR-V can be dropped and is read again on the next lookup, and an unknown hash gives nothing.
NXTEST(load_stub_and_release) {
    if (!real_data_available()) { nxtest::skip("work/shader_cache not found"); return; }
    std::vector<Prog> progs;
    REQUIRE(real_programs(&progs));
    TempDir td;
    PrepareConfig cfg(td.path);
    App app;
    REQUIRE(start(app));
    if (!gpu::gpl_active()) {
        app.shutdown();
        nxtest::skip("pipeline libraries not supported on this GPU");
        return;
    }
    auto& cache = nxrt::nvn::be::shader_cache();
    const std::string vhash = hex16(progs[0].vs);
    const ShaderInfo* a = cache.load_stub(vhash);
    REQUIRE(a != nullptr);
    std::printf("    %s: stage %s, %zu SPIR-V words, %zu inputs, %zu descriptor bindings\n", vhash.c_str(), nxrt::nvn::be::stage_name(a->stage),
                a->spirv.size(), a->refl.inputs.size(), a->refl.bindings.size());
    CHECK(a->stage == ShaderStage::Vertex);
    CHECK(a->refl.stage == ShaderStage::Vertex);
    CHECK(!a->spirv.empty());
    CHECK(cache.load_stub(vhash) == a);
    cache.release_stub_spirv(a);
    CHECK(a->spirv.empty());
    CHECK(cache.load_stub(vhash) == a);
    CHECK(!a->spirv.empty());
    CHECK(cache.load_stub("ffffffffffffffff") == nullptr);
    app.shutdown();
}

// The six programs are prepared with two threads: every program is counted once, at least one is prepared, the progress callback ends at
// done == total, prepare.state says complete, and no pipeline cache file is written (the preparation drops its parts).
NXTEST(prepare_six_programs) {
    if (!real_data_available()) { nxtest::skip("work/shader_cache not found"); return; }
    std::vector<Prog> progs;
    REQUIRE(real_programs(&progs));
    TempDir td;
    PrepareConfig cfg(td.path);
    const fs::path file = td.path / "programs.bin";
    REQUIRE(write_programs(file, progs));
    App app;
    REQUIRE(start(app));
    if (!gpu::gpl_active()) {
        app.shutdown();
        nxtest::skip("pipeline libraries not supported on this GPU");
        return;
    }
    nxrt::config_set("nvn", "prepare_cache", "0");           // keeps nothing in the pipeline cache file
    nxrt::nvn::ShaderPrepareOptions o;
    o.programs_file = file.string();
    o.threads = 2;
    std::atomic<uint64_t> calls{0}, last_done{~uint64_t(0)}, last_total{0};
    o.progress = [&](uint64_t done, uint64_t total) {
        calls.fetch_add(1);
        last_done.store(done);
        last_total.store(total);
        return true;
    };
    nxrt::nvn::ShaderPrepareResult r;
    CHECK(nxrt::nvn::prepare_shaders(o, &r));
    std::printf("    programs %llu, prepared %llu, missing %llu, failed %llu, parts %llu, %.2f s, progress calls %llu\n",
                static_cast<unsigned long long>(r.programs), static_cast<unsigned long long>(r.prepared),
                static_cast<unsigned long long>(r.missing_shaders), static_cast<unsigned long long>(r.failed),
                static_cast<unsigned long long>(r.parts), r.seconds, static_cast<unsigned long long>(calls.load()));
    CHECK_EQ(r.programs, 6ull);
    CHECK_EQ(r.prepared + r.missing_shaders + r.failed, 6ull);
    CHECK(r.prepared >= 1);
    CHECK_EQ(r.failed, 0ull);  // a state file is written only for a run without failures, so this is also what case 3 needs
    CHECK(!r.cancelled);
    CHECK_EQ(last_done.load(), 6ull);
    CHECK_EQ(last_total.load(), 6ull);
    app.shutdown();  // the pipeline cache, if any, is written at shutdown: check after it
    const std::string state = read_text(td.path / "prepare.state");
    CHECK(state.find("complete: 1") != std::string::npos);
    CHECK(!fs::exists(td.path / "pipeline_cache.bin"));
}

// With prepare_cache on (the default) the result is kept in the pipeline cache file.
NXTEST(keeps_cache_file_when_asked) {
    if (!real_data_available()) { nxtest::skip("work/shader_cache not found"); return; }
    std::vector<Prog> progs;
    REQUIRE(real_programs(&progs));
    TempDir td;
    PrepareConfig cfg(td.path);
    const fs::path file = td.path / "programs.bin";
    REQUIRE(write_programs(file, progs));
    App app;
    REQUIRE(start(app));
    if (!gpu::gpl_active()) {
        app.shutdown();
        nxtest::skip("pipeline libraries not supported on this GPU");
        return;
    }
    nxrt::config_set("nvn", "prepare_cache", "1");
    nxrt::nvn::ShaderPrepareOptions o;
    o.programs_file = file.string();
    o.threads = 2;
    nxrt::nvn::ShaderPrepareResult r;
    CHECK(nxrt::nvn::prepare_shaders(o, &r));
    CHECK(r.prepared >= 1);
    app.shutdown();
    std::error_code ec;
    const auto size = fs::exists(td.path / "pipeline_cache.bin") ? fs::file_size(td.path / "pipeline_cache.bin", ec) : 0;
    std::printf("    pipeline cache file: %llu bytes\n", static_cast<unsigned long long>(size));
    CHECK(size > 1000);
}

// After a finished run, a second call with only_if_needed does nothing (already_ready, no parts); with force it runs again.
NXTEST(only_if_needed_and_force) {
    if (!real_data_available()) { nxtest::skip("work/shader_cache not found"); return; }
    std::vector<Prog> progs;
    REQUIRE(real_programs(&progs));
    TempDir td;
    PrepareConfig cfg(td.path);
    const fs::path file = td.path / "programs.bin";
    REQUIRE(write_programs(file, progs));
    App app;
    REQUIRE(start(app));
    if (!gpu::gpl_active()) {
        app.shutdown();
        nxtest::skip("pipeline libraries not supported on this GPU");
        return;
    }
    nxrt::nvn::ShaderPrepareOptions o;
    o.programs_file = file.string();
    o.threads = 2;
    nxrt::nvn::ShaderPrepareResult first;
    REQUIRE(nxrt::nvn::prepare_shaders(o, &first));
    REQUIRE(first.prepared >= 1);

    nxrt::nvn::ShaderPrepareOptions again = o;
    again.only_if_needed = true;
    nxrt::nvn::ShaderPrepareResult r2;
    CHECK(nxrt::nvn::prepare_shaders(again, &r2));
    CHECK(r2.already_ready);
    CHECK_EQ(r2.parts, 0ull);

    nxrt::nvn::ShaderPrepareOptions forced = again;
    forced.force = true;
    nxrt::nvn::ShaderPrepareResult r3;
    CHECK(nxrt::nvn::prepare_shaders(forced, &r3));
    CHECK(!r3.already_ready);
    CHECK(r3.parts > 0);
    std::printf("    second call: already_ready %d, parts %llu; forced call: parts %llu\n", r2.already_ready ? 1 : 0,
                static_cast<unsigned long long>(r2.parts), static_cast<unsigned long long>(r3.parts));
    app.shutdown();
}

// A list with the wrong magic and a list that is not there both fail with a reason.
NXTEST(bad_program_files) {
    if (!real_data_available()) { nxtest::skip("work/shader_cache not found"); return; }
    TempDir td;
    PrepareConfig cfg(td.path);
    const fs::path bad = td.path / "bad_programs.bin";
    {
        std::ofstream f(bad, std::ios::binary | std::ios::trunc);
        f.write("NOTAPRG!", 8);
        put_u32(f, 1);
        put_u32(f, 0);
    }
    App app;
    REQUIRE(start(app));
    nxrt::nvn::ShaderPrepareOptions o;
    o.programs_file = bad.string();
    nxrt::nvn::ShaderPrepareResult r;
    CHECK(!nxrt::nvn::prepare_shaders(o, &r));
    std::printf("    wrong magic: %s\n", r.error.c_str());
    CHECK(!r.error.empty());

    nxrt::nvn::ShaderPrepareOptions m;
    m.programs_file = (td.path / "missing_programs.bin").string();
    nxrt::nvn::ShaderPrepareResult r2;
    CHECK(!nxrt::nvn::prepare_shaders(m, &r2));
    std::printf("    missing file: %s\n", r2.error.c_str());
    CHECK(!r2.error.empty());
    app.shutdown();
}

// A progress callback that says stop at its first call cancels the run: with one thread at most one program is done, and prepare.state
// is written as not complete.
NXTEST(cancel_leaves_state_incomplete) {
    if (!real_data_available()) { nxtest::skip("work/shader_cache not found"); return; }
    std::vector<Prog> progs;
    REQUIRE(real_programs(&progs));
    TempDir td;
    PrepareConfig cfg(td.path);
    const fs::path file = td.path / "programs.bin";
    REQUIRE(write_programs(file, progs));
    App app;
    REQUIRE(start(app));
    if (!gpu::gpl_active()) {
        app.shutdown();
        nxtest::skip("pipeline libraries not supported on this GPU");
        return;
    }
    nxrt::nvn::ShaderPrepareOptions o;
    o.programs_file = file.string();
    o.threads = 1;
    o.progress = [](uint64_t, uint64_t) { return false; };
    nxrt::nvn::ShaderPrepareResult r;
    CHECK(nxrt::nvn::prepare_shaders(o, &r));
    std::printf("    cancelled %d, prepared %llu of %llu\n", r.cancelled ? 1 : 0, static_cast<unsigned long long>(r.prepared),
                static_cast<unsigned long long>(r.programs));
    CHECK(r.cancelled);
    CHECK(r.prepared < r.programs);
    const std::string state = read_text(td.path / "prepare.state");
    CHECK(state.find("complete: 1") == std::string::npos);
    app.shutdown();
}

// The state checks the setup makes before a start (no device): scheme, raster flags of [nvn] prepare_rasters and the pipeline cache file the
// preparation wrote (missing, or under half of its bytes: prepare again). prepare_rasters_setting normalises the setting.
NXTEST(state_needs_scheme_rasters_and_cache_file) {
    namespace be = nxrt::nvn::be;
    const std::string old = nxrt::config_get_str("nvn", "prepare_rasters", "0.1.1 0.0.1");   // put back as found (unset reads as the default)
    nxrt::config_set("nvn", "prepare_rasters", "0.1.1, 0.0.1;0.0.1 9.9.9");
    CHECK_EQ(be::prepare_rasters_setting(), std::string("0.1.1 0.0.1"));
    TempDir td;
    const fs::path state = td.path / "prepare.state", cache = td.path / "pipeline_cache.bin";
    auto write_state = [&](int scheme, const std::string& rasters, uint64_t cache_bytes, bool complete) {
        std::ofstream f(state, std::ios::trunc);
        f << "key: scheme=" << scheme << " programs=6 gpu=x driver=1 api=1 cache=00 salt=\n" << "prepared: 6\n" << "rasters: " << rasters << "\n";
        if (cache_bytes) f << "cache: " << cache.string() << "\n" << "cache_bytes: " << cache_bytes << "\n";
        f << "complete: " << (complete ? 1 : 0) << "\n";
    };
    auto write_cache = [&](size_t bytes) {
        std::ofstream f(cache, std::ios::binary | std::ios::trunc);
        f << std::string(bytes, 'c');
    };
    std::string why;
    CHECK(!be::prepare_state_complete(state.string(), &why));                        // no state file
    write_cache(1000);
    write_state(be::kPrepareScheme, "0.1.1 0.0.1", 1000, true);
    CHECK(be::prepare_state_complete(state.string(), &why));
    write_state(be::kPrepareScheme, "0.1.1 0.0.1", 1000, false);
    CHECK(!be::prepare_state_complete(state.string(), &why));                        // not finished
    write_state(1, "0.1.1 0.0.1", 1000, true);
    CHECK(!be::prepare_state_complete(state.string(), &why));                        // older scheme
    write_state(be::kPrepareScheme, "0.1.1", 1000, true);
    CHECK(!be::prepare_state_complete(state.string(), &why));                        // other raster flags
    write_state(be::kPrepareScheme, "0.1.1 0.0.1", 1000, true);
    write_cache(600);                                                                // a later save may differ a little: still fine
    CHECK(be::prepare_state_complete(state.string(), &why));
    write_cache(400);                                                                // lost most of it (the old 2 GiB load failure)
    CHECK(!be::prepare_state_complete(state.string(), &why));
    std::printf("    %s\n", why.c_str());
    std::error_code ec;
    fs::remove(cache, ec);
    CHECK(!be::prepare_state_complete(state.string(), &why));                        // gone
    write_state(be::kPrepareScheme, "0.1.1 0.0.1", 0, true);                         // prepare_cache off: no file named, nothing to check
    CHECK(be::prepare_state_complete(state.string(), &why));
    nxrt::config_set("nvn", "prepare_rasters", old.c_str());
}
