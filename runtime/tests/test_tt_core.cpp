// SPDX-License-Identifier: GPL-3.0-or-later
// Test tooling core (S5): a fake game whose memory has exactly the shape the registered test game's probes read (games/<id>/mpr_tt_probe.cpp), driven frame by frame
// through tt::frame_boundary. Covers scripted runs (pass, wait timeout, assert), record -> replay determinism, the virtual clock and RNG,
// memory-region hashing, the warp sequence (host functions registered at the guest addresses of the real functions) and @shot.
#include "test_framework.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#include "nxrt/config.h"
#include "nxrt/diag.h"
#include "nxrt/dispatch.h"
#include "nxrt/game.h"
#include "nxrt/kernel.h"
#include "nxrt/tt.h"

NXTEST_GROUP(tt_core)

namespace {

using namespace nxrt::tt;
namespace game = nxrt::game;

constexpr uint64_t kBase = 0x5000000000ull;          // the fake "main module" base: only a key for fn_register / hook addresses
const char* kWorld = "4b5e8635-0f43-4ffb-9427-2e62186e517d";
const char* kAreaA = "4ca69978-0000-4000-8000-000000000001";
const char* kAreaB = "4ca69978-0000-4000-8000-000000000002";

// The fake game below has title E's memory shape (docs/S5.md), which that game's probes read (nxrt/game.h Info::probes,
// games/<id>/mpr_tt_probe.cpp). That module is not in every checkout (this one registers oot, which has no probes): then the cases that only
// need some probe readings (health, position, world and area for the script steps) get a stand-in probe table that reads the same shape, and
// the cases that check those probes or the game's own warp sequence skip.
bool mpr_probes_registered() { return game::info().probes && std::strcmp(game::info().id, "mpr") == 0; }

ProbeLayout g_rig_layout;
template <class T>
T rig_at(uint64_t a) {
    T v;
    std::memcpy(&v, reinterpret_cast<const void*>(a), sizeof v);
    return v;
}
bool rig_read_state(GameState* out) {
    const ProbeLayout& l = g_rig_layout;
    if (!l.game_state_global || !l.state_manager) return false;
    *out = GameState{};
    const uint64_t gs = rig_at<uint64_t>(l.game_state_global);
    std::memcpy(out->world, reinterpret_cast<const void*>(gs), 16);
    const int32_t count = rig_at<int32_t>(gs + 0x10);
    const uint64_t worlds = rig_at<uint64_t>(gs + 0x18);
    for (int32_t i = 0; i < count; ++i)
        if (std::memcmp(reinterpret_cast<const void*>(worlds + 0x50 * i), out->world, 16) == 0) std::memcpy(out->area, reinterpret_cast<const void*>(worlds + 0x50 * i + 0x10), 16);
    out->have_world = true;
    out->hp = rig_at<float>(rig_at<uint64_t>(gs + 0x20) + 0xc);
    out->have_hp = true;
    const uint64_t cgs = rig_at<uint64_t>(rig_at<uint64_t>(l.state_manager + 8) + 8);
    out->igt = rig_at<double>(cgs + 0xffb08);
    out->have_igt = true;
    const uint64_t player = rig_at<uint64_t>(rig_at<uint64_t>(l.state_manager + 0x4e0150) + 0x18);
    const uint64_t xf = rig_at<uint64_t>(rig_at<uint64_t>(rig_at<uint64_t>(player + 8) + 0x20) + 0x40);
    out->pos[0] = rig_at<float>(xf + 3 * 4);
    out->pos[1] = rig_at<float>(xf + 7 * 4);
    out->pos[2] = rig_at<float>(xf + 11 * 4);
    out->have_player = true;
    return true;
}
void rig_set_layout(const ProbeLayout& l) { g_rig_layout = l; }
const GameProbes kRigProbes = [] {
    GameProbes p;
    p.read_state = &rig_read_state;
    p.set_probe_layout = &rig_set_layout;
    return p;
}();

// Registers the stand-in probes for one Rig's lifetime unless the registered game is title E (restored in reverse order).
struct RigProbes {
    const game::Info* prev = game::current();
    game::Info copy = game::info();
    RigProbes() {
        if (mpr_probes_registered()) return;
        copy.probes = &kRigProbes;
        game::set_current(&copy);
    }
    ~RigProbes() { game::set_current(prev); }
    RigProbes(const RigProbes&) = delete;
    RigProbes& operator=(const RigProbes&) = delete;
};

// A fake game whose memory has the shape the probes read: gpGameState -> CGameStateMP1 -> world array; CStateManager -> CGameState (igt),
// GameLogic -> player -> transform. The toy simulation: UP walks +y, left stick strafes +x, A raises z, igt counts.
struct Rig {
    RigProbes probes_;
    alignas(16) uint8_t gs[0x40] = {};
    alignas(16) uint8_t worlds[2 * 0x50] = {};
    alignas(16) uint8_t pstate[0x20] = {};
    uint64_t gp = 0;
    uint8_t* mgr;                // CStateManager, large enough for +0x4e0150
    uint8_t* cgs;                // CGameState, 1 MiB
    alignas(16) uint8_t cgsw[0x10] = {};
    alignas(16) uint8_t logic[0x40] = {};
    alignas(16) uint8_t player[0x400] = {};
    alignas(16) uint8_t pa[0x40] = {};
    alignas(16) uint8_t pb[0x80] = {};
    float xf[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    double igt = 0;
    int warp_calls = 0;
    uint64_t warp_world[2] = {}, warp_area[2] = {}, quit_reason = 0;
    // S42 warp gate: the game globals -> CResourceFactory -> CResourceLoaderManager chain, with the queue counts IsThreadWorkDone reads
    bool with_loader = false;
    uint64_t globals = 0;
    alignas(16) uint8_t factory[0x20] = {};
    alignas(16) uint8_t loader[0x200] = {};
    uint8_t pending_area[16] = {};
    bool pending = false;
    int pending_in = 0;
    std::string out;
    int exit_code_seen = -999;
    static Rig* cur;

    explicit Rig(const char* name) {
        cur = this;
        out = std::string("tt_test_out\\") + name;
        mgr = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x4f0000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        cgs = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x101000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        tt_guid_to_bytes(kWorld, gs);
        std::memcpy(worlds, gs, 16);
        tt_guid_to_bytes(kAreaA, worlds + 0x10);
        const int32_t count = 1;
        std::memcpy(gs + 0x10, &count, 4);
        put(gs + 0x18, worlds);
        put(gs + 0x20, pstate);
        const float hp = 99.0f;
        std::memcpy(pstate + 0xc, &hp, 4);
        gp = reinterpret_cast<uint64_t>(gs);
        put(mgr + 8, cgsw);
        put(cgsw + 8, cgs);
        put(mgr + 0x4e0150, logic);
        put(logic + 0x18, player);
        put(player + 8, pa);
        put(pa + 0x20, pb);
        put(pb + 0x40, xf);
        put(factory + 0x10, loader);
        globals = reinterpret_cast<uint64_t>(factory);
        set_igt(0);
    }
    void set_loader_queue(int32_t n) { std::memcpy(loader + 0xe8, &n, 4); }      // the load list count
    ~Rig() {
        VirtualFree(mgr, 0, MEM_RELEASE);
        VirtualFree(cgs, 0, MEM_RELEASE);
        cur = nullptr;
    }
    static void put(uint8_t* where, const void* ptr) { const uint64_t v = reinterpret_cast<uint64_t>(ptr); std::memcpy(where, &v, 8); }
    void set_igt(double v) { igt = v; std::memcpy(cgs + 0xffb08, &v, 8); }
    void layout() {
        ProbeLayout l;
        l.game_state_global = reinterpret_cast<uint64_t>(&gp);
        l.state_manager = reinterpret_cast<uint64_t>(mgr);
        if (with_loader) l.game_globals = reinterpret_cast<uint64_t>(&globals);
        set_probe_layout(l);
    }
    // one frame of the toy game, using the pad of the frame that just began
    void step(const PadFrame& p) {
        if (p.buttons & (1ull << 13)) xf[7] += 0.5f;
        xf[3] += static_cast<float>(p.lx) / 32767.0f * 0.25f;
        if (p.buttons & 1ull) xf[11] += 0.125f;
        set_igt(igt + 1.0 / 60.0);
        if (pending && --pending_in <= 0) {              // a warp "loads" for 3 frames
            std::memcpy(worlds + 0x10, pending_area, 16);
            pending = false;
        }
    }
};
Rig* Rig::cur = nullptr;

void fake_set_world(::Ctx* c) { Rig* r = Rig::cur; r->warp_calls++; r->warp_world[0] = c->x[1]; r->warp_world[1] = c->x[2]; std::memcpy(r->gs, &c->x[1], 8); std::memcpy(r->gs + 8, &c->x[2], 8); }
void fake_current_ws(::Ctx* c) { c->x[0] = reinterpret_cast<uint64_t>(Rig::cur->worlds); }
void fake_set_desired(::Ctx* c) { Rig* r = Rig::cur; r->warp_area[0] = c->x[1]; r->warp_area[1] = c->x[2]; }
void fake_continue(::Ctx* c) {
    Rig* r = Rig::cur;
    std::memcpy(r->pending_area, reinterpret_cast<void*>(c->x[1]), 16);
    int32_t slot;
    std::memcpy(&slot, reinterpret_cast<uint8_t*>(c->x[1]) + 16, 4);
    if (slot != -1 || c->x[0] != reinterpret_cast<uint64_t>(r->cgs)) r->warp_calls += 1000;     // wrong arguments are visible in the count
}
void fake_gis(::Ctx* c) { c->x[0] = 0x1234000; }
void fake_quit(::Ctx* c) { Rig* r = Rig::cur; r->quit_reason = c->x[1]; r->pending = true; r->pending_in = 3; }
void on_exit_code(int code) { if (Rig::cur) Rig::cur->exit_code_seen = code; }

std::string slurp(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
void spit(const std::string& p, const std::string& t) { std::ofstream f(p, std::ios::binary); f << t; }

std::string scriptfile(const char* name, const std::string& text) {
    CreateDirectoryA("tt_test_out", nullptr);
    const std::string p = std::string("tt_test_out\\") + name;
    spit(p, text);
    return p;
}

void begin(Rig& r, std::initializer_list<std::pair<const char*, std::string>> kv) {
    nxrt::config_clear();
    reset_for_tests();
    set_exit_handler(on_exit_code);
    nxrt::config_set("test", "out", r.out.c_str());
    nxrt::config_set("test", "frame_source", "manual");
    nxrt::config_set("test", "game_base", std::to_string(kBase).c_str());
    for (auto& p : kv) nxrt::config_set("test", p.first, p.second.c_str());
    r.layout();
    r.exit_code_seen = -999;
    r.pending = false;
    init();
}

void run_frames(Rig& r, int max_frames) {
    ::Ctx ctx{};
    for (int i = 0; i < max_frames && !finished(); i++) {
        frame_boundary(&ctx);
        if (finished()) break;
        r.step(current_pad());
    }
}

// the 'h' column of every frame line of a trace file
std::vector<std::string> hashes_of(const std::string& trace) {
    std::vector<std::string> v;
    std::istringstream is(trace);
    std::string line;
    while (std::getline(is, line)) {
        const size_t f = line.find("\"f\":"), h = line.find("\"h\":\"");
        if (f == std::string::npos || h == std::string::npos) continue;
        v.push_back(line.substr(f, line.find(',', f) - f) + line.substr(h, 22));
    }
    return v;
}

const char* kSession =
    "# walk and hash\n"
    "5\n"
    "hold UP\n"
    "20\n"
    "release UP\n"
    "stick left 1.0 0 for 10\n"
    "press A for 3\n"
    "mouse 4 -2 for 2\n"
    "@hash walked\n"
    "@assert health >= 90\n"
    "@pos 2.5 10.0 0.375 0.01\n"
    "@pass\n";

}  // namespace

NXTEST(probes_read_the_fake_game) {
    if (!mpr_probes_registered()) {   // this case checks those probes' offsets, not the stand-in that copies them
        nxtest::skip("needs title E's tt probes (games/mpr, not in this checkout)");
        return;
    }
    Rig r("probes");
    r.set_igt(12.5);
    r.xf[3] = 1.5f; r.xf[7] = -2.0f; r.xf[11] = 3.25f;
    begin(r, {{"trace", "none"}, {"deterministic", "1"}});
    GameState g;
    REQUIRE(read_state(&g));
    CHECK(g.have_world && g.have_player && g.have_igt && g.have_hp);
    CHECK_EQ(tt_guid_to_text(g.world), std::string(kWorld));
    CHECK_EQ(tt_guid_to_text(g.area), std::string(kAreaA));
    CHECK(g.pos[0] == 1.5f && g.pos[1] == -2.0f && g.pos[2] == 3.25f);
    CHECK(g.hp == 99.0f);
    CHECK(g.igt == 12.5);
    shutdown();
}

NXTEST(script_runs_and_passes) {
    Rig r("pass");
    const std::string s = scriptfile("pass.nxs", kSession);
    begin(r, {{"script", s}, {"deterministic", "1"}});
    run_frames(r, 200);
    CHECK(finished());
    CHECK_EQ(r.exit_code_seen, 0);
    const std::string res = slurp(r.out + "\\result.json");
    CHECK(res.find("\"verdict\":\"pass\"") != std::string::npos);
    CHECK(res.find("\"label\":\"walked\"") != std::string::npos);
    // UP held for 20 frames (+ the frame the hold takes effect): y = 0.5 per frame
    CHECK(std::fabs(r.xf[7] - 10.0f) < 0.01f);
    CHECK(std::fabs(r.xf[11] - 0.375f) < 1e-6f);
    CHECK(!hashes_of(slurp(r.out + "\\trace.jsonl")).empty());
    shutdown();
}

NXTEST(wait_timeout_fails_with_the_script_line) {
    Rig r("timeout");
    const std::string s = scriptfile("timeout.nxs", "3\n@area " + std::string(kAreaB) + " timeout=12\n@pass\n");
    begin(r, {{"script", s}});
    run_frames(r, 100);
    CHECK_EQ(r.exit_code_seen, 1);
    const std::string res = slurp(r.out + "\\result.json");
    CHECK(res.find("\"verdict\":\"fail\"") != std::string::npos);
    CHECK(res.find("timeout.nxs:2:") != std::string::npos);
    CHECK(res.find("\"fail_line\":2") != std::string::npos);
    shutdown();
}

NXTEST(assert_failure_ends_the_run) {
    Rig r("assert");
    const std::string s = scriptfile("assert.nxs", "2\n@assert health < 50\n@pass\n");
    begin(r, {{"script", s}});
    run_frames(r, 50);
    CHECK_EQ(r.exit_code_seen, 1);
    CHECK(slurp(r.out + "\\result.json").find("assertion failed") != std::string::npos);
    shutdown();
}

NXTEST(parse_error_is_exit_code_2) {
    Rig r("parse");
    const std::string s = scriptfile("bad.nxs", "5\nfrobnicate 3\n");
    begin(r, {{"script", s}});
    CHECK_EQ(r.exit_code_seen, 2);
    CHECK(slurp(r.out + "\\result.json").find("bad.nxs:2") != std::string::npos);
    shutdown();
}

NXTEST(replay_of_a_recording_is_bit_identical) {
    Rig a("rec");
    const std::string s = scriptfile("session.nxs", kSession);
    const std::string rec = "tt_test_out\\session.nxin";
    begin(a, {{"script", s}, {"deterministic", "1"}, {"record", rec}});
    run_frames(a, 200);
    REQUIRE(a.exit_code_seen == 0);
    const uint64_t chain_script = chain_hash();
    const auto trace_script = hashes_of(slurp(a.out + "\\trace.jsonl"));
    shutdown();
    CHECK(slurp(rec).find("# nxin 1") == 0);

    uint64_t chain_replay[2] = {0, 0};
    std::vector<std::string> traces[2];
    for (int i = 0; i < 2; i++) {
        Rig b(i ? "replay1" : "replay0");
        begin(b, {{"replay", rec}, {"deterministic", "1"}, {"replay_exit", "1"}});
        run_frames(b, 300);
        CHECK_EQ(b.exit_code_seen, 0);
        chain_replay[i] = chain_hash();
        traces[i] = hashes_of(slurp(b.out + "\\trace.jsonl"));
        CHECK(std::fabs(b.xf[7] - 10.0f) < 0.01f);       // the replay moved the player the same way
        shutdown();
    }
    CHECK_EQ(chain_replay[0], chain_replay[1]);
    CHECK(traces[0] == traces[1]);
    // the replay runs past the script's last frame by a frame or two (the trace's end): compare the common prefix
    REQUIRE(traces[0].size() >= trace_script.size() - 1);
    for (size_t i = 0; i + 2 < trace_script.size(); i++) CHECK_EQ(traces[0][i], trace_script[i]);
}

NXTEST(two_script_runs_have_the_same_final_hash) {
    uint64_t h[2];
    for (int i = 0; i < 2; i++) {
        Rig r("twice");
        begin(r, {{"script", scriptfile("twice.nxs", kSession)}, {"deterministic", "1"}});
        run_frames(r, 200);
        CHECK_EQ(r.exit_code_seen, 0);
        h[i] = chain_hash();
        CHECK(slurp(r.out + "\\result.json").find("\"final_hash\":\"") != std::string::npos);
        shutdown();
    }
    CHECK_EQ(h[0], h[1]);
}

NXTEST(a_different_input_gives_a_different_hash) {
    uint64_t h[2];
    const char* scripts[2] = {"5\nhold UP\n10\n@pass\n", "5\nhold UP\n11\n@pass\n"};
    for (int i = 0; i < 2; i++) {
        Rig r("diff");
        begin(r, {{"script", scriptfile("diff.nxs", scripts[i])}, {"deterministic", "1"}});
        run_frames(r, 200);
        h[i] = chain_hash();
        shutdown();
    }
    CHECK(h[0] != h[1]);
}

NXTEST(virtual_clock_steps_per_frame) {
    Rig r("clock");
    begin(r, {{"deterministic", "1"}, {"trace", "none"}});
    uint64_t t0 = 0, t1 = 0, t2 = 0;
    // system ticks at the platform's rate: 19.2 MHz on platform 1 (320000 per 1/60 s frame), 31.25 MHz on platform 2 (the test game's console)
    const uint64_t hz = nxrt::kernel::tick_hz(), frame_ticks = hz / 60, ms_ticks = hz / 1000;
    REQUIRE(virtual_tick(&t0));
    REQUIRE(virtual_tick(&t1));
    CHECK_EQ(t1, t0 + 1);                                  // every read advances time by one tick
    ::Ctx ctx{};
    for (int i = 0; i < 4; i++) frame_boundary(&ctx);      // frames 0..3
    REQUIRE(virtual_tick(&t2));
    CHECK(t2 >= t0 + 3 * frame_ticks);                     // three full steps since frame 0
    CHECK(t2 < t0 + 4 * frame_ticks);
    const uint64_t before = t2;
    virtual_sleep(1000000);                                // 1 ms of ticks, no real wait
    uint64_t t3 = 0;
    virtual_tick(&t3);
    CHECK(t3 >= before + ms_ticks);
    CHECK(t3 < before + ms_ticks + 16);                    // exactly 1 ms (and the few reads in between), not 1 ms at another platform's rate
    // A sleep on another thread does not move the clock: it waits for the frames (here: none come) for at most its length of host time.
    uint64_t t4 = 0, t5 = 0;
    virtual_tick(&t4);
    std::thread other([] { for (int i = 0; i < 20; i++) virtual_sleep(1000000); });
    other.join();
    virtual_tick(&t5);
    CHECK(t5 < t4 + ms_ticks);                             // 20 sleeps of 1 ms on the other thread: less than 1 ms of virtual time
    shutdown();
    CHECK(!virtual_tick(&t3));                             // off after shutdown / when not enabled
}

NXTEST(rng_is_a_seeded_stream_in_deterministic_mode) {
    uint8_t a[24], b[24], c[24];
    Rig r("rng");
    begin(r, {{"deterministic", "1"}, {"seed", "42"}, {"trace", "none"}});
    rng_fill(a, 24);
    const uint64_t n1 = rng_next();
    shutdown();
    begin(r, {{"deterministic", "1"}, {"seed", "42"}, {"trace", "none"}});
    rng_fill(b, 24);
    CHECK(std::memcmp(a, b, 24) == 0);
    CHECK_EQ(n1, rng_next());
    shutdown();
    begin(r, {{"deterministic", "1"}, {"seed", "43"}, {"trace", "none"}});
    rng_fill(c, 24);
    CHECK(std::memcmp(a, c, 24) != 0);
    CHECK_EQ(wall_filetime(), wall_filetime() - 0 + 0);    // fixed epoch + virtual time: reads do not move wall time
    // one second of virtual time is 10^7 units of wall time, at either platform's tick rate
    const uint64_t w0 = wall_filetime();
    virtual_sleep(1000000000);
    const uint64_t w1 = wall_filetime();
    CHECK(w1 - w0 >= 10000000ull && w1 - w0 < 10000100ull);
    shutdown();
}

NXTEST(hash_regions_fold_memory_into_the_frame_hash) {
    static uint8_t buf[256];
    std::memset(buf, 7, sizeof buf);
    char spec[64];
    std::snprintf(spec, sizeof spec, "0x%llx+0x100", static_cast<unsigned long long>(reinterpret_cast<uint64_t>(buf)));
    uint64_t h[3];
    for (int i = 0; i < 3; i++) {
        Rig r("regions");
        begin(r, {{"deterministic", "1"}, {"hash_regions", spec}, {"trace", "none"}});
        if (i == 2) buf[100] ^= 1;
        ::Ctx ctx{};
        frame_boundary(&ctx);
        h[i] = last_hash();
        shutdown();
    }
    CHECK_EQ(h[0], h[1]);
    CHECK(h[0] != h[2]);
}

NXTEST(warp_runs_the_practice_mod_sequence) {
    if (!mpr_probes_registered()) {   // the warp is the game's own function sequence (GameProbes::warp)
        nxtest::skip("needs title E's tt probes and warp (games/mpr, not in this checkout)");
        return;
    }
    Rig r("warp");
    nxrt::fn_register(kBase + 0xc838e4, fake_set_world);
    nxrt::fn_register(kBase + 0xc84478, fake_current_ws);
    nxrt::fn_register(kBase + 0xc8275c, fake_set_desired);
    nxrt::fn_register(kBase + 0x40a820, fake_continue);
    nxrt::fn_register(kBase + 0xb01cc8, fake_gis);
    nxrt::fn_register(kBase + 0x410544, fake_quit);
    spit("tt_test_out\\regions.tsv", std::string("# test table\nfrig\t") + kWorld + "\t\nfrig_b\t" + kWorld + "\t" + kAreaB + "\n");
    const std::string s = scriptfile("warp.nxs", "2\n@warp frig frig_b\n@assert area == frig_b\n@assert world == frig\n@pass\n");
    begin(r, {{"script", s}, {"regions", "tt_test_out\\regions.tsv"}});
    run_frames(r, 100);
    CHECK_EQ(r.exit_code_seen, 0);
    CHECK_EQ(r.warp_calls, 1);                              // SetCurrentWorldId once, ContinueWithNewInitialState got the right arguments
    uint8_t w[16], a[16];
    tt_guid_to_bytes(kWorld, w);
    tt_guid_to_bytes(kAreaB, a);
    CHECK(std::memcmp(r.warp_world, w, 16) == 0);
    CHECK(std::memcmp(r.warp_area, a, 16) == 0);
    CHECK_EQ(r.quit_reason, 7u);
    shutdown();
}

namespace {
void register_warp_fakes() {
    nxrt::fn_register(kBase + 0xc838e4, fake_set_world);
    nxrt::fn_register(kBase + 0xc84478, fake_current_ws);
    nxrt::fn_register(kBase + 0xc8275c, fake_set_desired);
    nxrt::fn_register(kBase + 0x40a820, fake_continue);
    nxrt::fn_register(kBase + 0xb01cc8, fake_gis);
    nxrt::fn_register(kBase + 0x410544, fake_quit);
    spit("tt_test_out\\regions.tsv", std::string("# test table\nfrig\t") + kWorld + "\t\nfrig_b\t" + kWorld + "\t" + kAreaB + "\n");
}
const char* kWarpScript = "2\n@warp frig frig_b\n@assert area == frig_b\n@pass\n";
}  // namespace

// S42: @warp waits while the game's resource loader has work queued, and warps once it has been idle for 10 frames in a row.
NXTEST(warp_waits_for_the_idle_resource_loader) {
    if (!mpr_probes_registered()) {
        nxtest::skip("needs title E's tt probes and warp (games/mpr, not in this checkout)");
        return;
    }
    Rig r("warpgate");
    r.with_loader = true;
    r.set_loader_queue(3);
    register_warp_fakes();
    const std::string s = scriptfile("warpgate.nxs", kWarpScript);
    begin(r, {{"script", s}, {"regions", "tt_test_out\\regions.tsv"}});
    run_frames(r, 60);
    CHECK_EQ(r.warp_calls, 0);                              // still busy: no warp
    CHECK(!finished());
    r.set_loader_queue(0);
    run_frames(r, 9);
    CHECK_EQ(r.warp_calls, 0);                              // idle, but not yet for 10 frames in a row
    r.set_loader_queue(1);
    run_frames(r, 5);                                       // busy again: the count starts over
    r.set_loader_queue(0);
    run_frames(r, 9);
    CHECK_EQ(r.warp_calls, 0);
    run_frames(r, 100);
    CHECK_EQ(r.warp_calls, 1);
    CHECK_EQ(r.exit_code_seen, 0);
    shutdown();
}

NXTEST(warp_gate_can_be_turned_off_and_gives_up_after_its_limit) {
    if (!mpr_probes_registered()) {
        nxtest::skip("needs title E's tt probes and warp (games/mpr, not in this checkout)");
        return;
    }
    for (int mode = 0; mode < 2; mode++) {
        Rig r(mode ? "warpgate_max" : "warpgate_off");
        r.with_loader = true;
        r.set_loader_queue(5);                              // busy for the whole run
        register_warp_fakes();
        const std::string s = scriptfile("warpgate2.nxs", kWarpScript);
        if (mode == 0) begin(r, {{"script", s}, {"regions", "tt_test_out\\regions.tsv"}, {"warp_wait_loader", "0"}});
        else begin(r, {{"script", s}, {"regions", "tt_test_out\\regions.tsv"}, {"warp_loader_max_ms", "0"}});
        run_frames(r, 100);
        CHECK_EQ(r.warp_calls, 1);
        CHECK_EQ(r.exit_code_seen, 0);
        shutdown();
    }
}

namespace {
bool shot_ok(const char* path, void*) { spit(path, "PNG?"); return true; }
}

NXTEST(shots_go_through_the_provider) {
    Rig r("shot");
    const std::string s = scriptfile("shot.nxs", "2\n@shot first\n1\n@shot second\n@pass\n");
    begin(r, {{"script", s}});
    set_screenshot_provider(shot_ok, nullptr);
    run_frames(r, 50);
    CHECK_EQ(r.exit_code_seen, 0);
    const std::string res = slurp(r.out + "\\result.json");
    CHECK(res.find("\"first.png\"") != std::string::npos && res.find("\"second.png\"") != std::string::npos);
    CHECK_EQ(slurp(r.out + "\\shots\\first.png"), std::string("PNG?"));
    shutdown();
    set_screenshot_provider(nullptr, nullptr);
}

NXTEST(missing_provider_is_a_note_unless_required) {
    for (int req = 0; req < 2; req++) {
        Rig r("noshot");
        const std::string s = scriptfile("noshot.nxs", "2\n@shot x\n@pass\n");
        begin(r, {{"script", s}, {"require_shots", req ? "1" : "0"}});
        run_frames(r, 50);
        CHECK_EQ(r.exit_code_seen, req ? 1 : 0);
        if (!req) CHECK(slurp(r.out + "\\result.json").find("\"missing_shots\":[\"x\"]") != std::string::npos);
        shutdown();
    }
}

NXTEST(max_frames_budget_fails_the_run) {
    Rig r("budget");
    const std::string s = scriptfile("budget.nxs", "100000\n");
    begin(r, {{"script", s}, {"max_frames", "30"}});
    run_frames(r, 500);
    CHECK_EQ(r.exit_code_seen, 1);
    CHECK(slurp(r.out + "\\result.json").find("max_frames") != std::string::npos);
    shutdown();
}

NXTEST(watchdog_freeze_ends_a_scripted_run_with_code_5) {
    Rig r("freeze");
    const std::string s = scriptfile("freeze.nxs", "100000\n");
    nxrt::config_clear();
    nxrt::config_set("diag", "watchdog_seconds", "1");
    reset_for_tests();
    set_exit_handler(on_exit_code);
    nxrt::config_set("test", "out", r.out.c_str());
    nxrt::config_set("test", "frame_source", "manual");
    nxrt::config_set("test", "script", s.c_str());
    nxrt::config_set("test", "freeze_exit", "1");
    r.layout();
    r.exit_code_seen = -999;
    init();
    ::Ctx ctx{};
    frame_boundary(&ctx);                       // one frame, then the "game" hangs
    for (int i = 0; i < 60 && r.exit_code_seen == -999; i++) Sleep(100);
    CHECK_EQ(r.exit_code_seen, 5);
    CHECK(slurp(r.out + "\\result.json").find("\"verdict\":\"freeze\"") != std::string::npos);
    shutdown();
    nxrt::watchdog_stop();
}

// ---- scripted key steps (set_key_sink) ----------------------------------------------------------------------------------------
namespace {
struct KeyEvent {
    std::string name;
    bool down;
    uint64_t frame;      // the frame boundary (0-based) the sink was called at
};
std::vector<KeyEvent> g_key_events;

// the sink: refuses "Nope", records everything else with the frame it was called at
bool record_key(const char* name, bool down) {
    if (std::string(name) == "Nope") return false;
    g_key_events.push_back({name, down, frame() - 1});
    return true;
}
}  // namespace

NXTEST(key_steps_reach_the_key_sink_on_their_frames) {
    Rig r("keys");
    const std::string s = scriptfile("keys.nxs", "key W for 3\nkeydown Left Ctrl\nkeyup Left Ctrl\n@pass\n");
    begin(r, {{"script", s}, {"deterministic", "1"}});
    g_key_events.clear();
    set_key_sink(record_key);
    run_frames(r, 50);
    CHECK(finished());
    CHECK_EQ(r.exit_code_seen, 0);
    // "key W for 3" is down for frames 0, 1, 2 and released at frame 3, where the next two steps run in the same frame
    REQUIRE(g_key_events.size() == 4);
    CHECK(g_key_events[0].name == "W" && g_key_events[0].down && g_key_events[0].frame == 0);
    CHECK(g_key_events[1].name == "W" && !g_key_events[1].down && g_key_events[1].frame == 3);
    CHECK(g_key_events[2].name == "Left Ctrl" && g_key_events[2].down && g_key_events[2].frame == 3);
    CHECK(g_key_events[3].name == "Left Ctrl" && !g_key_events[3].down && g_key_events[3].frame == 3);
    set_key_sink(nullptr);
    shutdown();
}

NXTEST(key_without_for_is_four_frames) {
    Rig r("keys4");
    const std::string s = scriptfile("keys4.nxs", "key Mouse1\n@pass\n");
    begin(r, {{"script", s}});
    g_key_events.clear();
    set_key_sink(record_key);
    run_frames(r, 50);
    REQUIRE(g_key_events.size() == 2);
    CHECK(g_key_events[0].name == "Mouse1" && g_key_events[0].down && g_key_events[0].frame == 0);
    CHECK(g_key_events[1].name == "Mouse1" && !g_key_events[1].down && g_key_events[1].frame == 4);
    set_key_sink(nullptr);
    shutdown();
}

NXTEST(unknown_key_name_fails_the_run_and_releases_held_keys) {
    Rig r("keyunknown");
    // keydown W holds W; the next line names a key the sink refuses, so the run fails there and W must be released
    const std::string s = scriptfile("keys_unknown.nxs", "keydown W\nkey Nope\n@pass\n");
    begin(r, {{"script", s}});
    g_key_events.clear();
    set_key_sink(record_key);
    run_frames(r, 50);
    CHECK_EQ(r.exit_code_seen, 1);
    const std::string res = slurp(r.out + "/result.json");
    CHECK(res.find("\"verdict\":\"fail\"") != std::string::npos);
    CHECK(res.find("keys_unknown.nxs:2:") != std::string::npos);
    CHECK(res.find("unknown input 'Nope'") != std::string::npos);
    REQUIRE(g_key_events.size() == 2);
    CHECK(g_key_events[0].name == "W" && g_key_events[0].down);
    CHECK(g_key_events[1].name == "W" && !g_key_events[1].down);
    set_key_sink(nullptr);
    shutdown();
}

NXTEST(keys_still_down_are_released_when_the_script_passes) {
    Rig r("keyhold");
    const std::string s = scriptfile("keyhold.nxs", "keydown Left Ctrl\n2\n@pass\n");
    begin(r, {{"script", s}});
    g_key_events.clear();
    set_key_sink(record_key);
    run_frames(r, 50);
    CHECK_EQ(r.exit_code_seen, 0);
    REQUIRE(g_key_events.size() == 2);
    CHECK(g_key_events[0].name == "Left Ctrl" && g_key_events[0].down && g_key_events[0].frame == 0);
    CHECK(g_key_events[1].name == "Left Ctrl" && !g_key_events[1].down && g_key_events[1].frame == 2);
    set_key_sink(nullptr);
    shutdown();
}
