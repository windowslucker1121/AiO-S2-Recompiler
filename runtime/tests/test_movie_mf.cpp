// SPDX-License-Identifier: GPL-3.0-or-later
// movie:: on Media Foundation (src/media/movie_mf.cpp), driven the way the game drives it: through the HLE entry points (guest registers in,
// result in x0), with a fake ClientStreamReader whose callbacks are HLE thunks too.
//
// movie_import_table and movie_mediadata_buffer need nothing. movie_all_fmv plays real movies and checks them against what the container and
// ffmpeg say; it needs the movie folder made by tools/media/movie_oracle.py (game data read at test time, never committed) and skips without
// it:   NXRT_MOVIE_DIR=<dir with expected.tsv, <id>.mp4, <id>.md5, <id>.pts, <id>.ref.pcm>   [NXRT_MOVIE_OUT=<dir for decoded PCM>]
//       [NXRT_MOVIE_ONLY=<id prefix>]  [NXRT_MOVIE_THREADS=8]  [NXRT_MOVIE_FRAME_STEP=1 (check every n-th frame hash)]
#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "nxrt/dispatch.h"
#include "nxrt/module.h"
#include "nxrt/movie.h"
#include "test_framework.h"
#include "test_platform.h"

#pragma comment(lib, "bcrypt.lib")

NXTEST_GROUP(media_movie)

using namespace nxrt;

namespace {

// ---- calling HLE handlers like guest code does --------------------------------------------------------------------------------------
uint64_t call(const char* name, std::initializer_list<uint64_t> args) {
    ::Ctx c{};
    static thread_local unsigned char stack[512];
    c.x[31] = reinterpret_cast<uint64_t>(stack + 256);
    c.x[30] = 0x7100000100ull;
    int i = 0;
    for (uint64_t a : args) c.x[i++] = a;
    dispatch_call(&c, hle_address(name));
    return c.x[0];
}
uint64_t P(const void* p) { return reinterpret_cast<uint64_t>(p); }

// movie:: on Media Foundation is the Switch 1 movie library's (Metroid Prime Remastered's subsdk0); on Switch 2 install_movie_hle() leaves
// movie:: to the lifted library and answers false. The first call decides for the whole process (std::call_once), so it is made under the
// Switch 1 profile whatever console the test game targets (oot is a Switch 2 title).
bool install_switch1_movie_hle() {
    nxtest::ScopedPlatform sw1(game::Platform::Switch1);
    return media::install_movie_hle();
}

constexpr const char* kExtCtor = "_ZN5movie9ExtractorC1ENS_13ContainerTypeENS_9CacheSizeE";
constexpr const char* kExtDtor = "_ZN5movie9ExtractorD1Ev";
constexpr const char* kExtSetSource = "_ZN5movie9Extractor13SetDataSourceEPNS_18ClientStreamReaderEPKc";
constexpr const char* kExtTrackCount = "_ZN5movie9Extractor13GetTrackCountEPi";
constexpr const char* kExtTrackConfig = "_ZN5movie9Extractor21GetTrackConfigurationEiPNS_9MediaDataE";
constexpr const char* kExtSelect = "_ZN5movie9Extractor11SelectTrackEi";
constexpr const char* kExtTrackFor = "_ZN5movie9Extractor29GetTrackIndexForAvailableDataEPm";
constexpr const char* kExtSampleTime = "_ZN5movie9Extractor13GetSampleTimeEPl";
constexpr const char* kExtRead = "_ZN5movie9Extractor4ReadEPNS_6BufferE";
constexpr const char* kExtAdvance = "_ZN5movie9Extractor7AdvanceEv";
constexpr const char* kExtSeek = "_ZN5movie9Extractor6SeekToEl";
constexpr const char* kDecCtor = "_ZN5movie7DecoderC1ENS_11DecoderTypeENS_19DecoderOutputFormatENS_11DecoderModeEi";
constexpr const char* kDecDtor = "_ZN5movie7DecoderD1Ev";
constexpr const char* kDecEvents = "_ZN5movie7Decoder20RegisterClientEventsEPNS_13DecoderEventsE";
constexpr const char* kDecConfigure = "_ZN5movie7Decoder9ConfigureEPNS_9MediaDataE";
constexpr const char* kDecOutSize = "_ZN5movie7Decoder19GetOutputBufferSizeEPiS1_";
constexpr const char* kDecOutFormat = "_ZN5movie7Decoder15GetOutputFormatEPNS_9MediaDataE";
constexpr const char* kDecStart = "_ZN5movie7Decoder5StartEv";
constexpr const char* kDecAcqIn = "_ZN5movie7Decoder23AcquireInputBufferIndexEPiS1_";
constexpr const char* kDecGetIn = "_ZN5movie7Decoder14GetInputBufferEiPNS_6BufferE";
constexpr const char* kDecSend = "_ZN5movie7Decoder24SendInputBufferForDecodeEiiilj";
constexpr const char* kDecAcqOut = "_ZN5movie7Decoder24AcquireOutputBufferIndexEPiPlPjS1_";
constexpr const char* kDecGetOut = "_ZN5movie7Decoder15GetOutputBufferEiPNS_6BufferEPNS_9MediaDataE";
constexpr const char* kDecRelease = "_ZN5movie7Decoder24ReleaseOutputBufferIndexEi";
constexpr const char* kMdCtor = "_ZN5movie9MediaDataC1Ev";
constexpr const char* kMdCopy = "_ZN5movie9MediaDataC1ERKS0_";
constexpr const char* kMdDtor = "_ZN5movie9MediaDataD1Ev";
constexpr const char* kMdClear = "_ZN5movie9MediaData5ClearEv";
constexpr const char* kMdI32 = "_ZNK5movie9MediaData9FindInt32EPKcPi";
constexpr const char* kMdI64 = "_ZNK5movie9MediaData9FindInt64EPKcPl";
constexpr const char* kMdStr = "_ZNK5movie9MediaData10FindStringEPKcPS2_";
constexpr const char* kBufCtor = "_ZN5movie6BufferC1Ev";
constexpr const char* kBufCtorMem = "_ZN5movie6BufferC1EPvm";
constexpr const char* kBufDtor = "_ZN5movie6BufferD1Ev";
constexpr const char* kBufBase = "_ZNK5movie6Buffer4BaseEv";
constexpr const char* kBufSize = "_ZNK5movie6Buffer4SizeEv";
constexpr const char* kBufOffset = "_ZNK5movie6Buffer6OffsetEv";

// ---- the fake ClientStreamReader -----------------------------------------------------------------------------------------------------
thread_local const std::vector<uint8_t>* t_file = nullptr;
void reader_open(::Ctx* c) { c->x[0] = t_file ? 1 : 0; if (t_file) *reinterpret_cast<uint64_t*>(c->x[2]) = 0x1234; }
void reader_read(::Ctx* c) {                  // ReadAt(this, handle, offset, buf, n)
    const uint64_t off = c->x[2], buf = c->x[3], n = c->x[4];
    if (!t_file || off >= t_file->size()) { c->x[0] = 0; return; }
    const size_t k = static_cast<size_t>(std::min<uint64_t>(n, t_file->size() - off));
    std::memcpy(reinterpret_cast<void*>(buf), t_file->data() + off, k);
    c->x[0] = k;
}
void reader_close(::Ctx* c) { c->x[0] = 1; }
void reader_size(::Ctx* c) { *reinterpret_cast<int64_t*>(c->x[2]) = t_file ? static_cast<int64_t>(t_file->size()) : 0; c->x[0] = 0; }

struct FakeReader {
    uint64_t vtable_ptr;
    uint64_t vt[6];
    FakeReader() {
        hle_register("test::reader_open", reader_open);
        hle_register("test::reader_read", reader_read);
        hle_register("test::reader_close", reader_close);
        hle_register("test::reader_size", reader_size);
        vtable_ptr = P(vt);
        vt[0] = vt[1] = 0;
        vt[2] = hle_address("test::reader_open");
        vt[3] = hle_address("test::reader_read");
        vt[4] = hle_address("test::reader_close");
        vt[5] = hle_address("test::reader_size");
    }
};

std::string md5_hex(const uint8_t* d, size_t n) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    uint8_t h[16] = {};
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_MD5_ALGORITHM, nullptr, 0) != 0) return "";
    BCryptHash(alg, nullptr, 0, const_cast<uint8_t*>(d), static_cast<ULONG>(n), h, 16);
    BCryptCloseAlgorithmProvider(alg, 0);
    char s[33];
    for (int i = 0; i < 16; i++) std::snprintf(s + 2 * i, 3, "%02x", h[i]);
    return s;
}

std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    f.seekg(0, std::ios::end);
    const std::streamsize n = f.tellg();
    f.seekg(0);
    std::vector<uint8_t> v(static_cast<size_t>(std::max<std::streamsize>(n, 0)));
    if (n > 0) f.read(reinterpret_cast<char*>(v.data()), n);
    return v;
}

// ---- playing one movie the way the game's player does --------------------------------------------------------------------------------
struct Result {
    std::string id, error;
    int frames = 0, w = 0, h = 0, audio_ch = 0, audio_rate = 0;
    std::vector<int64_t> video_ts, audio_ts;
    std::vector<uint32_t> audio_sizes;
    std::vector<std::string> md5;     // per frame (every FRAME_STEP-th, "" otherwise)
    std::vector<uint8_t> pcm;
    uint64_t audio_bytes = 0;
    double seconds = 0;
};

struct DecoderSide {
    alignas(16) uint8_t obj[16] = {};
    bool present = false, video = false, ended = false;
    int track = -1;
    int size = 0, align = 0;
    std::vector<uint8_t> out;        // the game's output buffer memory
    uint64_t md_cfg = 0, md_fmt = 0;
};

uint64_t new_md() { uint64_t* h = new uint64_t(0); call(kMdCtor, {P(h)}); return P(h); }
void free_md(uint64_t h) { call(kMdDtor, {h}); delete reinterpret_cast<uint64_t*>(h); }

Result play_movie(const std::string& dir, const std::string& id, size_t frame_step, bool keep_pcm) {
    Result r;
    r.id = id;
    const auto t0 = std::chrono::steady_clock::now();
    const std::vector<uint8_t> file = read_file(dir + "/" + id + ".mp4");
    if (file.empty()) { r.error = "cannot read the movie"; return r; }
    t_file = &file;
    FakeReader reader;
    alignas(16) uint8_t ext[16] = {};
    call(kExtCtor, {P(ext), 1, 2});
    auto fail = [&](const std::string& m) { if (r.error.empty()) r.error = m; };
    if (call(kExtSetSource, {P(ext), P(&reader), P("movie.mp4")}) != 0) { fail("SetDataSource failed"); call(kExtDtor, {P(ext)}); return r; }
    int n = 0;
    call(kExtTrackCount, {P(ext), P(&n)});
    DecoderSide dv, da;
    std::vector<uint64_t> cfgs;
    for (int i = 0; i < n; i++) {
        uint64_t md = new_md();
        cfgs.push_back(md);
        if (call(kExtTrackConfig, {P(ext), static_cast<uint64_t>(i), md}) != 0) { fail("GetTrackConfiguration failed"); continue; }
        const char* mime = nullptr;
        call(kMdStr, {md, P("mime"), P(&mime)});
        if (!mime) { fail("no mime"); continue; }
        const std::string m = mime;
        DecoderSide* d = m == "video/avc" ? &dv : m == "audio/mp4a-latm" ? &da : nullptr;
        if (!d) { fail("unexpected track " + m); continue; }
        d->present = true;
        d->video = m == "video/avc";
        d->track = i;
        d->md_cfg = new_md();
        call(kMdCopy, {d->md_cfg, md});
        call(kDecCtor, {P(d->obj), d->video ? 1ull : 4ull, d->video ? 4ull : 1ull, 1, 0});
        uint64_t ev[4] = {};
        call(kDecEvents, {P(d->obj), P(ev)});
        if (call(kDecConfigure, {P(d->obj), d->md_cfg}) != 0) fail("Configure failed");
        int sz = 0, al = 0;
        call(kDecOutSize, {P(d->obj), P(&sz), P(&al)});
        d->size = std::max(sz, 0x8000);
        d->align = al;
        d->out.assign(static_cast<size_t>(d->size) + 64, 0);
        call(kDecStart, {P(d->obj)});
        if (call(kExtSelect, {P(ext), static_cast<uint64_t>(i)}) != 0) fail("SelectTrack failed");
        if (d->video) {
            int32_t v = 0;
            if (call(kMdI32, {md, P("width"), P(&v)})) r.w = v;
            if (call(kMdI32, {md, P("height"), P(&v)})) r.h = v;
        } else {
            int32_t v = 0;
            if (call(kMdI32, {md, P("sample-rate"), P(&v)})) r.audio_rate = v;
            if (call(kMdI32, {md, P("channel-count"), P(&v)})) r.audio_ch = v;
        }
    }
    if (!r.error.empty()) { call(kExtDtor, {P(ext)}); return r; }

    auto drain = [&](DecoderSide& d) {
        if (!d.present) return;
        for (;;) {
            int idx = -1, more = 0;
            uint32_t flags = 0;
            int64_t ts = 0;
            if (call(kDecAcqOut, {P(d.obj), P(&idx), P(&ts), P(&flags), P(&more)}) != 0) break;
            alignas(16) uint8_t buf[0x40];
            call(kBufCtorMem, {P(buf), P(d.out.data()), static_cast<uint64_t>(d.size)});
            const uint64_t md = new_md();
            call(kDecGetOut, {P(d.obj), static_cast<uint64_t>(idx), P(buf), md});
            const uint64_t fm = new_md();
            call(kDecOutFormat, {P(d.obj), fm});
            const uint32_t got = static_cast<uint32_t>(call(kBufSize, {P(buf)}));
            if (flags & 4) {
                d.ended = true;
            } else if (d.video) {
                int32_t w = 0, h = 0, yo = -1, uo = -1, cs = -1;
                call(kMdI32, {md, P("width"), P(&w)});
                call(kMdI32, {md, P("height"), P(&h)});
                call(kMdI32, {md, P("nv12-y-offset"), P(&yo)});
                call(kMdI32, {md, P("nv12-uv-offset"), P(&uo)});
                call(kMdI32, {fm, P("nv12-colorspace"), P(&cs)});
                if (got != static_cast<uint32_t>(w) * h * 3 / 2 || yo != 0 || uo != w * h) fail("video frame geometry: size " + std::to_string(got));
                if (cs != 2 && r.frames == 0) fail("nv12-colorspace " + std::to_string(cs) + " (BT.709 limited expected)");
                const size_t k = r.video_ts.size();
                r.video_ts.push_back(ts);
                r.md5.push_back(frame_step && k % frame_step == 0 ? md5_hex(d.out.data(), got) : std::string());
                r.frames++;
            } else {
                r.audio_ts.push_back(ts);
                r.audio_sizes.push_back(got);
                r.audio_bytes += got;
                if (keep_pcm) r.pcm.insert(r.pcm.end(), d.out.begin(), d.out.begin() + got);
            }
            free_md(md);
            free_md(fm);
            call(kDecRelease, {P(d.obj), static_cast<uint64_t>(idx)});
        }
    };

    bool eos_sent = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(300);
    while (r.error.empty() && std::chrono::steady_clock::now() < deadline) {
        drain(dv);
        drain(da);
        if (eos_sent) {
            if ((!dv.present || dv.ended) && (!da.present || da.ended)) break;
            std::this_thread::yield();
            continue;
        }
        uint64_t track = ~0ull;
        const uint64_t rc = call(kExtTrackFor, {P(ext), P(&track)});
        auto send_eos = [&](DecoderSide& d) {
            if (!d.present) return;
            for (int tries = 0; tries < 100000; tries++) {
                int idx = -1, more = 0;
                if (call(kDecAcqIn, {P(d.obj), P(&idx), P(&more)}) == 0) {
                    call(kDecSend, {P(d.obj), static_cast<uint64_t>(idx), 0, 0, 0, 4});
                    return;
                }
                drain(dv);
                drain(da);
                std::this_thread::yield();
            }
            fail("no input buffer for the end-of-stream marker");
        };
        if (rc == 0x2000) {
            send_eos(dv);
            send_eos(da);
            eos_sent = true;
            continue;
        }
        if (rc != 0) { fail("GetTrackIndexForAvailableData failed: " + std::to_string(rc)); break; }
        DecoderSide* d = track == static_cast<uint64_t>(dv.track) && dv.present ? &dv : track == static_cast<uint64_t>(da.track) && da.present ? &da : nullptr;
        if (!d) { fail("data for an unselected track"); break; }
        int idx = -1, more = 0;
        if (call(kDecAcqIn, {P(d->obj), P(&idx), P(&more)}) != 0) { std::this_thread::yield(); continue; }
        alignas(16) uint8_t buf[0x40];
        call(kBufCtor, {P(buf)});
        call(kDecGetIn, {P(d->obj), static_cast<uint64_t>(idx), P(buf)});
        call(kExtRead, {P(ext), P(buf)});
        int64_t ts = -1;
        call(kExtSampleTime, {P(ext), P(&ts)});
        call(kDecSend, {P(d->obj), static_cast<uint64_t>(idx), call(kBufOffset, {P(buf)}), call(kBufSize, {P(buf)}), static_cast<uint64_t>(ts), 0});
        call(kExtAdvance, {P(ext)});
    }
    if (r.error.empty() && !(eos_sent && (!dv.present || dv.ended) && (!da.present || da.ended))) fail("timed out before both decoders reported the end");
    call(kDecDtor, {P(dv.obj)});
    if (da.present) call(kDecDtor, {P(da.obj)});
    call(kExtDtor, {P(ext)});
    for (uint64_t m : cfgs) free_md(m);
    if (dv.md_cfg) free_md(dv.md_cfg);
    if (da.md_cfg) free_md(da.md_cfg);
    t_file = nullptr;
    r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return r;
}

struct Expected {
    std::string id;
    int frames = 0, w = 0, h = 0, audio_ch = 0, audio_rate = 0;
    int64_t audio_us = 0;
};
std::vector<Expected> load_expected(const std::string& dir) {
    std::vector<Expected> v;
    std::ifstream f(dir + "/expected.tsv");
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ss(line);
        Expected e;
        long long us = 0;
        if (ss >> e.id >> e.frames >> e.w >> e.h >> e.audio_ch >> e.audio_rate >> us) { e.audio_us = us; v.push_back(e); }
    }
    return v;
}
std::vector<int64_t> load_ints(const std::string& path) {
    std::vector<int64_t> v;
    std::ifstream f(path);
    long long x;
    while (f >> x) v.push_back(x);
    return v;
}
std::vector<std::string> load_lines(const std::string& path) {
    std::vector<std::string> v;
    std::ifstream f(path);
    std::string l;
    while (std::getline(f, l)) if (!l.empty()) v.push_back(l);
    return v;
}

}  // namespace

// Plays the shortest movie of the oracle folder through the extractor only (no decoders) to its end, seeks to 0 like a looping movie does and plays it again:
// both passes must hold the same samples with the same timestamps, whether the loop restart came from the samples decoded ahead (S21) or from a fresh seek.
NXTEST(movie_loop_restart) {
    const char* dirv = std::getenv("NXRT_MOVIE_DIR");
    if (!dirv || !*dirv) { nxtest::skip("NXRT_MOVIE_DIR is not set (run tools/media/movie_oracle.py)"); return; }
    REQUIRE(install_switch1_movie_hle());
    std::vector<Expected> all = load_expected(dirv);
    REQUIRE(!all.empty());
    const Expected* pick = &all[0];
    for (const Expected& e : all) if (e.frames < pick->frames && e.frames > 30) pick = &e;
    const std::vector<uint8_t> file = read_file(std::string(dirv) + "/" + pick->id + ".mp4");
    REQUIRE(!file.empty());
    t_file = &file;
    FakeReader reader;
    alignas(16) uint8_t ext[16] = {};
    call(kExtCtor, {P(ext), 1, 2});
    REQUIRE(call(kExtSetSource, {P(ext), P(&reader), P("movie.mp4")}) == 0);
    int n = 0;
    call(kExtTrackCount, {P(ext), P(&n)});
    REQUIRE(n >= 1);
    for (int i = 0; i < n; i++) REQUIRE(call(kExtSelect, {P(ext), static_cast<uint64_t>(i)}) == 0);   // the last selection prerolls
    auto pass = [&](std::vector<std::pair<uint64_t, int64_t>>& out) {
        for (int guard = 0; guard < 200000; guard++) {
            uint64_t track = ~0ull;
            const uint64_t rc = call(kExtTrackFor, {P(ext), P(&track)});
            if (rc == 0x2000) return true;
            if (rc != 0) return false;
            int64_t ts = -1;
            if (call(kExtSampleTime, {P(ext), P(&ts)}) != 0) return false;
            out.emplace_back(track, ts);
            call(kExtAdvance, {P(ext)});
        }
        return false;
    };
    std::vector<std::pair<uint64_t, int64_t>> a, b, c;
    CHECK(pass(a));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));   // the worker has the start decoded ahead by now
    CHECK(call(kExtSeek, {P(ext), 0}) == 0);
    CHECK(pass(b));
    CHECK(call(kExtSeek, {P(ext), 0}) == 0);                       // at once: the look-ahead may not be ready
    CHECK(pass(c));
    CHECK(!a.empty());
    CHECK_MSG(a == b, "the second pass differs from the first (" + std::to_string(a.size()) + " vs " + std::to_string(b.size()) + " samples)");
    CHECK_MSG(a == c, "the third pass differs from the first (" + std::to_string(a.size()) + " vs " + std::to_string(c.size()) + " samples)");
    if (a != c) {
        size_t i = 0;
        while (i < a.size() && i < c.size() && a[i] == c[i]) ++i;
        std::printf("    first difference at sample %zu: first pass (%llu, %lld), third pass (%llu, %lld)\n", i, i < a.size() ? (unsigned long long)a[i].first : 0ull,
                    i < a.size() ? (long long)a[i].second : -1ll, i < c.size() ? (unsigned long long)c[i].first : 0ull, i < c.size() ? (long long)c[i].second : -1ll);
    }
    call(kExtDtor, {P(ext)});
    t_file = nullptr;
}

NXTEST(movie_import_table) {
    CHECK(install_switch1_movie_hle());
    size_t n = 0;
    const char* const* names = media::movie_import_names(&n);
    CHECK_EQ(n, 41u);
    for (size_t i = 0; i < n; i++) {
        CHECK_MSG(hle_lookup(names[i]) != nullptr, names[i]);
        CHECK_MSG(module_symbol_overridden(names[i]), names[i]);
    }
}

NXTEST(movie_mediadata_buffer) {
    REQUIRE(install_switch1_movie_hle());
    uint64_t md = 0;
    call(kMdCtor, {P(&md)});
    REQUIRE(md != 0);
    int32_t v = 7;
    CHECK_EQ(call(kMdI32, {P(&md), P("width"), P(&v)}) & 1, 0ull);   // nothing stored yet
    CHECK_EQ(v, 7);
    uint64_t copy = 0;
    call(kMdCopy, {P(&copy), P(&md)});
    CHECK(copy != 0 && copy != md);
    call(kMdClear, {P(&md)});
    call(kMdDtor, {P(&md)});
    call(kMdDtor, {P(&copy)});
    CHECK_EQ(md, 0ull);

    alignas(16) uint8_t b[0x40];
    std::memset(b, 0xff, sizeof b);
    uint8_t mem[256];
    call(kBufCtorMem, {P(b), P(mem), 256});
    CHECK_EQ(call(kBufBase, {P(b)}), P(mem));
    CHECK_EQ(call(kBufSize, {P(b)}), 0ull);
    CHECK_EQ(call(kBufOffset, {P(b)}), 0ull);
    call(kBufCtor, {P(b)});
    CHECK_EQ(call(kBufBase, {P(b)}), 0ull);
    call(kBufDtor, {P(b)});
}

NXTEST(movie_all_fmv) {
    const char* dirv = std::getenv("NXRT_MOVIE_DIR");
    if (!dirv || !*dirv) { nxtest::skip("NXRT_MOVIE_DIR is not set (run tools/media/movie_oracle.py)"); return; }
    REQUIRE(install_switch1_movie_hle());
    const std::string dir = dirv;
    const char* outv = std::getenv("NXRT_MOVIE_OUT");
    const char* onlyv = std::getenv("NXRT_MOVIE_ONLY");
    const char* thrv = std::getenv("NXRT_MOVIE_THREADS");
    const char* stepv = std::getenv("NXRT_MOVIE_FRAME_STEP");
    const size_t step = stepv ? static_cast<size_t>(std::max(0, std::atoi(stepv))) : 1;
    std::vector<Expected> all = load_expected(dir);
    REQUIRE(!all.empty());
    std::vector<Expected> todo;
    for (const Expected& e : all) if (!onlyv || !*onlyv || e.id.rfind(onlyv, 0) == 0) todo.push_back(e);
    REQUIRE(!todo.empty());

    std::vector<Result> results(todo.size());
    std::atomic<size_t> next{0};
    const unsigned nthreads = std::max(1, thrv ? std::atoi(thrv) : 8);
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < nthreads; t++) {
        pool.emplace_back([&] {
            for (size_t i; (i = next.fetch_add(1)) < todo.size();) results[i] = play_movie(dir, todo[i].id, step, outv != nullptr);
        });
    }
    for (auto& t : pool) t.join();

    uint64_t total_frames = 0, total_audio = 0;
    double total_s = 0;
    int audio_movies = 0;
    for (size_t i = 0; i < todo.size(); i++) {
        const Expected& e = todo[i];
        const Result& r = results[i];
        const std::string tag = e.id.substr(0, 8);
        CHECK_MSG(r.error.empty(), tag + ": " + r.error);
        if (!r.error.empty()) continue;
        CHECK_MSG(r.frames == e.frames, tag + ": frames " + std::to_string(r.frames) + " expected " + std::to_string(e.frames));
        CHECK_MSG(r.w == e.w && r.h == e.h, tag + ": size " + std::to_string(r.w) + "x" + std::to_string(r.h));
        const std::vector<int64_t> pts = load_ints(dir + "/" + e.id + ".pts");
        CHECK_MSG(pts.size() == static_cast<size_t>(e.frames), tag + ": pts file has " + std::to_string(pts.size()) + " entries");
        int bad_ts = 0, bad_md5 = 0, hashed = 0;
        const std::vector<std::string> ref = load_lines(dir + "/" + e.id + ".md5");
        for (size_t k = 0; k < r.video_ts.size() && k < pts.size(); k++) {
            if (std::llabs(r.video_ts[k] - pts[k]) > 2) bad_ts++;
            if (!r.md5[k].empty()) {
                hashed++;
                if (k >= ref.size() || r.md5[k] != ref[k]) bad_md5++;
            }
        }
        if (bad_ts) std::printf("  %s: first video ts: ours %lld %lld %lld, container %lld %lld %lld\n", tag.c_str(), (long long)r.video_ts[0], (long long)r.video_ts[1], (long long)r.video_ts[2], (long long)pts[0], (long long)pts[1], (long long)pts[2]);
        CHECK_MSG(bad_ts == 0, tag + ": " + std::to_string(bad_ts) + " frame timestamps differ from the container by more than 2 us");
        CHECK_MSG(bad_md5 == 0, tag + ": " + std::to_string(bad_md5) + " of " + std::to_string(hashed) + " frames differ from the ffmpeg decode");
        if (e.audio_ch) {
            audio_movies++;
            CHECK_MSG(r.audio_ch == e.audio_ch && r.audio_rate == e.audio_rate, tag + ": audio format " + std::to_string(r.audio_ch) + " ch " + std::to_string(r.audio_rate));
            std::printf("  %s: audio blocks %zu, bytes %llu\n", tag.c_str(), r.audio_ts.size(), (unsigned long long)r.audio_bytes);
            const std::vector<uint8_t> refpcm = read_file(dir + "/" + e.id + ".ref.pcm");
            const double ours = static_cast<double>(r.audio_bytes) / (r.audio_ch * 2) / r.audio_rate;
            const double theirs = static_cast<double>(refpcm.size()) / (e.audio_ch * 2) / e.audio_rate;
            CHECK_MSG(std::fabs(ours - theirs) < 0.045, tag + ": audio " + std::to_string(ours) + " s, ffmpeg " + std::to_string(theirs) + " s");
            CHECK_MSG(!r.audio_ts.empty() && r.audio_ts[0] == 0, tag + ": the first audio block is not at 0");
            // timestamps follow the samples: block k starts where the data before it ends
            uint64_t acc = 0;
            int bad_a = 0;
            for (size_t k = 0; k < r.audio_ts.size(); k++) {
                const int64_t expect = static_cast<int64_t>(acc / (r.audio_ch * 2) * 1000000ull / r.audio_rate);
                if (std::llabs(r.audio_ts[k] - expect) > 100) bad_a++;
                acc += r.audio_sizes[k];
            }
            CHECK_MSG(bad_a == 0, tag + ": " + std::to_string(bad_a) + " audio blocks whose timestamp does not follow the data before them");
            total_audio += r.audio_bytes;
            if (outv) {
                std::ofstream f(std::string(outv) + "/" + e.id + ".pcm", std::ios::binary);
                f.write(reinterpret_cast<const char*>(r.pcm.data()), static_cast<std::streamsize>(r.pcm.size()));
            }
        } else {
            CHECK_MSG(r.audio_bytes == 0, tag + ": audio data from a silent movie");
        }
        total_frames += static_cast<uint64_t>(r.frames);
        total_s += r.seconds;
        std::printf("  %s: %d frames %dx%d %.1f s (%.0f fps)%s\n", tag.c_str(), r.frames, r.w, r.h, r.seconds, r.frames / std::max(r.seconds, 1e-6),
                    e.audio_ch ? ", audio ok" : "");
    }
    const media::MovieStats st = media::movie_stats();
    std::printf("movies %zu, frames %llu, audio movies %d, audio bytes %llu, cpu-seconds %.1f; extractors %llu/%llu decoders %llu/%llu\n", todo.size(),
                static_cast<unsigned long long>(total_frames), audio_movies, static_cast<unsigned long long>(total_audio), total_s,
                static_cast<unsigned long long>(st.extractors_destroyed), static_cast<unsigned long long>(st.extractors_created),
                static_cast<unsigned long long>(st.decoders_destroyed), static_cast<unsigned long long>(st.decoders_created));
    CHECK_EQ(st.extractors_created, st.extractors_destroyed);
    CHECK_EQ(st.decoders_created, st.decoders_destroyed);
}
