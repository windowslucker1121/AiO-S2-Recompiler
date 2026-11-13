// SPDX-License-Identifier: GPL-3.0-or-later
// The smaller NVN2 features (plan goal 20; src/gfx/nvn/api_small.cpp, nvn_small.h): push constants, variable rate shading, exclusive scissor,
// conservative raster, fast-clear registration, ProgramBuilder, device message buffers and dirty tile maps. Command buffer calls are recorded
// through the guest entry points and the records applied to a SmallState the way the replay does; nothing here needs a GPU.
#include <cstdint>
#include <cstring>
#include <vector>

#include "nvn_cmd.h"
#include "nvn_impl.h"
#include "nvn_small.h"
#include "test_framework.h"
NXTEST_GROUP(gfx_nvn_small)

using namespace nxrt;
using namespace nxrt::nvn;
using namespace nxrt::nvn::be;

namespace {

void setup() {
    install();
    abi::reset_mismatches();
    register_api_cmdbuf();
    abi::reset_mismatches();   // api_cmdbuf.cpp: one return type the table spells differently (see test_nvn_cmd_state.cpp)
    register_api_cmd_state();
    CHECK_EQ(abi::mismatches(), 0u);
    register_api_small();
    register_api_program();
    register_api_device();
    register_api_builders();
    CHECK_EQ(abi::mismatches(), 0u);
}

// A typed caller of an NVN2-only entry point (proc_address registers the thunk by name).
template <class Sig>
abi::Caller<Sig> N2(const char* name) {
    return abi::Caller<Sig>(proc_address(name));
}

struct Recorder {
    NVNcommandBuffer cb;
    Recorder() {
        std::memset(&cb, 0, sizeof cb);
        CHECK(abi::proc<PFNNVNCOMMANDBUFFERINITIALIZEPROC>("nvnCommandBufferInitialize")(&cb, nullptr));
        abi::proc<PFNNVNCOMMANDBUFFERBEGINRECORDINGPROC>("nvnCommandBufferBeginRecording")(&cb);
    }
    ~Recorder() { abi::proc<PFNNVNCOMMANDBUFFERFINALIZEPROC>("nvnCommandBufferFinalize")(&cb); }
    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;
    Ref<CmdList> end() { return handle_lookup(abi::proc<PFNNVNCOMMANDBUFFERENDRECORDINGPROC>("nvnCommandBufferEndRecording")(&cb)); }
};

// One Op::Small record, decoded.
struct SmallRec {
    P_Small p;
    std::vector<uint8_t> blob;   // bytes after the payload (padded to 8)
};
std::vector<SmallRec> small_records(const Ref<CmdList>& list) {
    std::vector<SmallRec> out;
    CHECK(list);
    if (!list) return out;
    for (RecordReader rd(*list); rd.next();) {
        if (rd.op() != Op::Small) continue;
        SmallRec r;
        r.p = rd.payload<P_Small>();
        const uint32_t n = rd.size() - 8 - ((static_cast<uint32_t>(sizeof(P_Small)) + 7u) & ~7u);
        r.blob.assign(rd.blob<P_Small>(), rd.blob<P_Small>() + n);
        out.push_back(std::move(r));
    }
    return out;
}
void apply_recs(SmallState& st, const std::vector<SmallRec>& recs) {
    for (const SmallRec& r : recs)
        small_apply(st, r.p.kind, r.p.a, r.p.b, r.p.c, r.p.addr, r.blob.data(), static_cast<uint32_t>(r.blob.size()));
}
std::vector<SmallRec> record_and_end(Recorder& r) { return small_records(r.end()); }

// A memory pool host object over a static buffer, registered like a real one.
struct TestPool {
    NVNmemoryPool guest;
    MemoryPool* pool;
    alignas(64) uint8_t mem[1024];
    TestPool() {
        std::memset(&guest, 0, sizeof guest);
        std::memset(mem, 0, sizeof mem);
        pool = new MemoryPool();
        pool->base = mem;
        pool->size = sizeof mem;
        pool->gpu_base = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(mem));
        pool_register(pool);
        bind_guest(&guest, pool);
    }
    ~TestPool() {
        pool_unregister(pool);
        unbind_guest(&guest);
    }
};

uint32_t word(const uint8_t* base, size_t i) {
    uint32_t v;
    std::memcpy(&v, base + 4 * i, 4);
    return v;
}
}  // namespace

// ---- push constants -----------------------------------------------------------------------------------------------------------------
NXTEST(push_constants_record_and_land_in_the_stage_area) {
    setup();
    Recorder r;
    uint32_t data[64];
    for (uint32_t i = 0; i < 64; ++i) data[i] = 0x1000 + i;
    auto gfx = N2<void(NVNcommandBuffer*, int, int, int, const void*)>("nvnCommandBufferPushGraphicsConstants");
    auto cmp = N2<void(NVNcommandBuffer*, int, int, int, const void*)>("nvnCommandBufferPushComputeConstants");
    gfx(&r.cb, 9, 8, 16, data);           // stage 9 & 7 = 1, bytes 8..23
    cmp(&r.cb, 5, 0, 300, data);          // compute stage 5, size clamped to 256
    gfx(&r.cb, 0, 0, 3, data);            // under one word: nothing recorded
    gfx(&r.cb, 0, 0, 16, nullptr);        // no data: nothing recorded
    gfx(&r.cb, 2, 250, 16, data);         // offset 250 -> 248, 8 bytes fit
    const auto recs = record_and_end(r);
    CHECK_EQ(recs.size(), 3u);
    if (recs.size() != 3) return;
    CHECK_EQ(recs[0].p.kind, static_cast<uint32_t>(SmallKind::PushConstants));
    CHECK_EQ(recs[0].p.a, 1u);
    CHECK_EQ(recs[1].p.c, 256u);
    SmallState st;
    apply_recs(st, recs);
    CHECK_EQ(st.pushes, 3u);
    CHECK_EQ(word(st.push[1], 2), 0x1000u);   // bytes 8..11 hold the first word
    CHECK_EQ(word(st.push[1], 5), 0x1003u);
    CHECK_EQ(word(st.push[1], 6), 0u);        // past the 16 bytes
    CHECK_EQ(word(st.push[5], 63), 0x103Fu);
    CHECK_EQ(word(st.push[2], 62), 0x1000u);
    CHECK_EQ(word(st.push[2], 63), 0x1001u);
}

NXTEST(push_constants_deferred_read_the_data_at_replay) {
    setup();
    TestPool tp;
    uint32_t words[4] = {11, 22, 33, 44};
    std::memcpy(tp.mem + 64, words, sizeof words);
    Recorder r;
    N2<void(NVNcommandBuffer*, int, int, int, NVNbufferAddress)>("nvnCommandBufferPushGraphicsConstantsDeferred")(&r.cb, 1, 4, 16, tp.pool->gpu_base + 64);
    N2<void(NVNcommandBuffer*, int, int, int, NVNbufferAddress)>("nvnCommandBufferPushComputeConstantsDeferred")(&r.cb, 5, 0, 16, 0);   // a null address
    const auto recs = record_and_end(r);
    CHECK_EQ(recs.size(), 1u);   // a null address is not recorded
    CHECK_EQ(recs.size() == 1 ? recs[0].p.kind : 0u, static_cast<uint32_t>(SmallKind::PushConstantsDeferred));
    words[0] = 99;   // the game rewrites the data before the submit
    std::memcpy(tp.mem + 64, words, sizeof words);
    SmallState st;
    apply_recs(st, recs);
    uint32_t got[2];
    std::memcpy(got, st.push[1] + 4, 8);
    CHECK_EQ(got[0], 99u);
    CHECK_EQ(got[1], 22u);
    // an address outside every pool is refused at replay
    small_apply(st, static_cast<uint32_t>(SmallKind::PushConstantsDeferred), 3, 0, 16, 0x10, nullptr, 0);
    CHECK_EQ(st.pushes, 1u);
}

// ---- variable rate shading ----------------------------------------------------------------------------------------------------------
NXTEST(shading_rate_palette_maps_to_fragment_sizes) {
    uint32_t w, h;
    const uint32_t want[12][2] = {{1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1}, {1, 1}, {2, 1}, {1, 2}, {2, 2}, {4, 2}, {2, 4}, {4, 4}};
    for (uint32_t e = 0; e < 12; ++e) {
        palette_entry_to_fragment_size(e, w, h);
        CHECK_EQ(w, want[e][0]);
        CHECK_EQ(h, want[e][1]);
    }
    palette_entry_to_fragment_size(15, w, h);
    CHECK_EQ(w * h, 1u);
    // fitting to what the device supports (bit = log2(w) * 3 + log2(h))
    const uint32_t only_2x2 = 1u << (1 * 3 + 1), with_4x4 = only_2x2 | (1u << (2 * 3 + 2));
    w = 4; h = 4;
    fit_fragment_size(with_4x4, w, h);
    CHECK(w == 4 && h == 4);
    w = 4; h = 4;
    fit_fragment_size(only_2x2, w, h);
    CHECK(w == 2 && h == 2);
    w = 4; h = 2;
    fit_fragment_size(0, w, h);
    CHECK(w == 1 && h == 1);
}

NXTEST(shading_rate_image_palette_and_draw_rate) {
    setup();
    Recorder r;
    NVNtexture tex;
    std::memset(&tex, 0, sizeof tex);
    int uniform[16];
    for (int& e : uniform) e = 8;                         // 1 invocation per 2x2 pixels for every value
    const int mixed[3] = {6, 6, 11};                      // the other 13 entries default to 5
    auto bind = N2<void(NVNcommandBuffer*, const NVNtexture*, const NVNtextureView*)>("nvnCommandBufferBindShadingRateImage");
    auto pal = N2<void(NVNcommandBuffer*, int, int, const int*)>("nvnCommandBufferShadingRateImagePalette");
    auto mode = N2<void(NVNcommandBuffer*, int)>("nvnCommandBufferSetShadingRatePaletteMode");
    pal(&r.cb, 0, 16, uniform);
    mode(&r.cb, 1);
    bind(&r.cb, &tex, nullptr);
    const auto recs1 = record_and_end(r);
    CHECK_EQ(recs1.size(), 3u);
    SmallState st;
    apply_recs(st, recs1);
    CHECK(st.palette_on[0]);
    CHECK_EQ(st.palette[0][15], 8u);
    CHECK_EQ(st.rate_palette_mode, 1);
    CHECK(st.rate_image != 0);
    const uint32_t all = 0x1FF;
    uint32_t w = 0, h = 0;
    bool exact = false;
    shading_rate_for_draw(st, all, w, h, &exact);
    CHECK(w == 2 && h == 2 && exact);
    shading_rate_for_draw(st, 0, w, h, &exact);   // a device with only 1x1: the fallback
    CHECK(w == 1 && h == 1);

    Recorder r2;
    pal(&r2.cb, 17, 3, mixed);       // index 17 & 0xF = 1... palette 0 stays uniform; palette 1 is mixed
    pal(&r2.cb, 0, 3, mixed);        // palette 0 now mixed: entries 6, 6, 11, then 5s
    const auto recs2 = record_and_end(r2);
    apply_recs(st, recs2);
    CHECK_EQ(st.palette[0][2], 11u);
    CHECK_EQ(st.palette[0][3], static_cast<uint32_t>(kPaletteOnePerPixel));
    shading_rate_for_draw(st, all, w, h, &exact);
    CHECK(w == 1 && h == 1 && !exact);   // several rates: the image is not read, full rate

    Recorder r3;
    pal(&r3.cb, 0, 16, nullptr);     // null entries: the palette is off
    bind(&r3.cb, nullptr, nullptr);  // and the image is unbound
    apply_recs(st, record_and_end(r3));
    CHECK(!st.palette_on[0]);
    CHECK_EQ(st.rate_image, 0u);
    shading_rate_for_draw(st, all, w, h, &exact);
    CHECK(w == 1 && h == 1 && exact);
}

// ---- exclusive scissor --------------------------------------------------------------------------------------------------------------
NXTEST(exclusive_scissor_records_clamps_and_reduces) {
    setup();
    Recorder r;
    N2<void(NVNcommandBuffer*, NVNboolean)>("nvnCommandBufferEnableExclusiveScissor")(&r.cb, 1);
    N2<void(NVNcommandBuffer*, int, int, int, int)>("nvnCommandBufferSetExclusiveScissor")(&r.cb, -5, 10, 20, 30);
    const NVNrectangle rects[2] = {{0, 0, 100, 50}, {70000, 0, 10, 10}};
    N2<void(NVNcommandBuffer*, int, int, const NVNrectangle*)>("nvnCommandBufferSetExclusiveScissors")(&r.cb, 3, 2, rects);
    N2<void(NVNcommandBuffer*, int, int, const NVNrectangle*)>("nvnCommandBufferSetExclusiveScissors")(&r.cb, 16, 1, rects);   // out of range
    N2<void(NVNcommandBuffer*, int, int, const NVNrectangle*)>("nvnCommandBufferSetExclusiveScissors")(&r.cb, 0, 0, rects);    // no rectangles
    const auto recs = record_and_end(r);
    CHECK_EQ(recs.size(), 3u);
    SmallState st;
    apply_recs(st, recs);
    CHECK(st.excl_enable);
    CHECK(st.excl[0][0] == 0 && st.excl[0][1] == 10 && st.excl[0][2] == 15 && st.excl[0][3] == 30);   // negative x trims the width
    CHECK(st.excl[3][2] == 100 && st.excl[3][3] == 50);
    CHECK(st.excl[4][2] == 0 && st.excl[4][3] == 0);   // starts past 0xFFFE: empty

    const int32_t s[4] = {0, 0, 100, 100};
    int32_t out[4];
    bool exact;
    auto red = [&](int32_t x, int32_t y, int32_t w, int32_t h) {
        const int32_t e[4] = {x, y, w, h};
        exclusive_reduce(s, e, out, &exact);
    };
    red(200, 0, 10, 10);   // misses
    CHECK(exact && out[2] == 100 && out[3] == 100);
    red(-10, -10, 200, 200);   // covers
    CHECK(exact && (out[2] == 0 || out[3] == 0));
    red(-10, -10, 60, 200);   // left band
    CHECK(exact && out[0] == 50 && out[1] == 0 && out[2] == 50 && out[3] == 100);
    red(70, -10, 100, 200);   // right band
    CHECK(exact && out[0] == 0 && out[2] == 70 && out[3] == 100);
    red(-10, -10, 200, 30);   // top band
    CHECK(exact && out[1] == 20 && out[3] == 80 && out[2] == 100);
    red(-10, 80, 200, 50);   // bottom band
    CHECK(exact && out[1] == 0 && out[3] == 80);
    red(40, 40, 20, 20);   // a hole
    CHECK(!exact && out[2] == 100 && out[3] == 100);
    red(50, 50, 100, 100);   // a corner
    CHECK(!exact);

    st.excl_enable = false;
    small_exclusive_scissor(st, 0, s, out, &exact);
    CHECK(exact && out[2] == 100);   // disabled: untouched
    st.excl_enable = true;
    st.excl[1][0] = 0; st.excl[1][1] = 0; st.excl[1][2] = 100; st.excl[1][3] = 100;
    small_exclusive_scissor(st, 1, s, out, &exact);
    CHECK(out[2] == 0 || out[3] == 0);
}

// ---- conservative raster ------------------------------------------------------------------------------------------------------------
NXTEST(conservative_raster_mode_and_dilate) {
    setup();
    CHECK_EQ(dilate_to_quarters(0.0f), 0u);
    CHECK_EQ(dilate_to_quarters(0.1f), 1u);
    CHECK_EQ(dilate_to_quarters(0.5f), 2u);
    CHECK_EQ(dilate_to_quarters(0.75f), 3u);
    CHECK_EQ(dilate_to_quarters(2.0f), 3u);
    CHECK_EQ(dilate_to_quarters(-1.0f), 0u);
    Recorder r;
    abi::proc<PFNNVNCOMMANDBUFFERSETCONSERVATIVERASTERENABLEPROC>("nvnCommandBufferSetConservativeRasterEnable")(&r.cb, 1);
    abi::proc<PFNNVNCOMMANDBUFFERSETCONSERVATIVERASTERDILATEPROC>("nvnCommandBufferSetConservativeRasterDilate")(&r.cb, 0.5f);
    N2<void(NVNcommandBuffer*, int, int)>("nvnCommandBufferSetConservativeRasterMode")(&r.cb, 2, 0x13);   // bloat keeps 4 bits: 3
    const Ref<CmdList> list = r.end();
    const auto recs = small_records(list);
    CHECK_EQ(recs.size(), 2u);   // dilate and mode; the enable is its own op
    SmallState st;
    CHECK(!small_conservative_on(st));
    st.cons_legacy = true;       // what the replay does for Op::SetConservativeRaster
    CHECK(small_conservative_on(st));
    apply_recs(st, {recs[0]});
    CHECK_EQ(st.cons_dilate_q, 2u);
    CHECK_EQ(small_conservative_extra(st, 1.0f, 0.25f), 0.5f);
    CHECK_EQ(small_conservative_extra(st, 0.25f, 0.25f), 0.25f);   // bounded by the device
    const float floored = small_conservative_extra(st, 1.0f, 0.4f);   // 0.5 floored to a multiple of the granularity
    CHECK(floored > 0.399f && floored < 0.401f);
    apply_recs(st, {recs[1]});
    CHECK_EQ(st.cons_mode, 2);
    CHECK_EQ(st.cons_mode_bloat_q, 3u);
    CHECK_EQ(small_conservative_extra(st, 1.0f, 0.25f), 0.75f);   // the mode call's bloat wins
    st.cons_legacy = false;
    CHECK(small_conservative_on(st));
    st.cons_mode = 0;
    CHECK(!small_conservative_on(st));
}

// ---- fast clear registration --------------------------------------------------------------------------------------------------------
NXTEST(fast_clear_registry_indexes_and_limits) {
    setup();
    NVNdevice dev;
    std::memset(&dev, 0, sizeof dev);
    bind_guest(&dev, new Device());
    auto color = abi::proc<PFNNVNDEVICEREGISTERFASTCLEARCOLORPROC>("nvnDeviceRegisterFastClearColor");
    auto colori = abi::proc<PFNNVNDEVICEREGISTERFASTCLEARCOLORIPROC>("nvnDeviceRegisterFastClearColori");
    auto depth = abi::proc<PFNNVNDEVICEREGISTERFASTCLEARDEPTHPROC>("nvnDeviceRegisterFastClearDepth");
    auto color_i = N2<int(NVNdevice*, const float*, NVNformat)>("nvnDeviceRegisterFastClearColorIndex");
    auto colori_i = N2<int(NVNdevice*, const int*, NVNformat)>("nvnDeviceRegisterFastClearColoriIndex");
    auto colorui_i = N2<int(NVNdevice*, const uint32_t*, NVNformat)>("nvnDeviceRegisterFastClearColoruiIndex");
    auto depth_i = N2<int(NVNdevice*, float)>("nvnDeviceRegisterFastClearDepthIndex");
    auto stencil = N2<NVNboolean(NVNdevice*, int)>("nvnDeviceRegisterFastClearStencil");
    auto stencil_i = N2<int(NVNdevice*, int, NVNformat)>("nvnDeviceRegisterFastClearStencilIndex");

    const float black[4] = {0, 0, 0, 1}, white[4] = {1, 1, 1, 1};
    CHECK_EQ(color_i(&dev, black, NVN_FORMAT_RGBA8), 0);
    CHECK_EQ(color_i(&dev, white, NVN_FORMAT_RGBA8), 1);
    CHECK_EQ(color_i(&dev, black, NVN_FORMAT_RGBA8), 0);          // the same value keeps its index
    CHECK_EQ(color_i(&dev, black, NVN_FORMAT_RGBA16F), 2);        // another format is another entry
    CHECK_EQ(color_i(&dev, black, NVN_FORMAT_NONE), -1);          // no format
    CHECK_EQ(color_i(nullptr, black, NVN_FORMAT_RGBA8), -1);
    CHECK_EQ(color_i(&dev, nullptr, NVN_FORMAT_RGBA8), -1);
    CHECK(color(&dev, white, NVN_FORMAT_RGBA8) == 1);
    CHECK(color(&dev, nullptr, NVN_FORMAT_RGBA8) == 0);
    const int ints[4] = {1, 2, 3, 4};
    CHECK_EQ(colori_i(&dev, ints, NVN_FORMAT_RGBA8I), 3);
    CHECK(colori(&dev, ints, NVN_FORMAT_RGBA8I) == 1);
    const uint32_t uints[4] = {1, 2, 3, 4};
    CHECK_EQ(colorui_i(&dev, uints, NVN_FORMAT_RGBA8I), 4);       // same words, other kind: its own entry
    for (int i = 5; i < 31; ++i) {
        const float c[4] = {0.5f, static_cast<float>(i), 0, 0};
        CHECK_EQ(color_i(&dev, c, NVN_FORMAT_RGBA8), i);
    }
    const float extra[4] = {0.25f, 0.25f, 0.25f, 0.25f};
    CHECK_EQ(color_i(&dev, extra, NVN_FORMAT_RGBA8), -1);         // 31 colours is the limit
    CHECK_EQ(color_i(&dev, white, NVN_FORMAT_RGBA8), 1);          // registered ones still answer

    CHECK_EQ(depth_i(&dev, 0.5f), -1);                            // only 0.0 and 1.0
    CHECK_EQ(depth_i(&dev, 1.0f), 0);
    CHECK_EQ(depth_i(&dev, 0.0f), 1);
    CHECK_EQ(depth_i(&dev, 1.0f), 0);
    CHECK(depth(&dev, 0.0f) == 1);
    CHECK(depth(&dev, 0.3f) == 0);

    for (int v = 0; v < 6; ++v) CHECK_EQ(stencil_i(&dev, 10 + v, static_cast<NVNformat>(54)), v);
    CHECK_EQ(stencil_i(&dev, 99, static_cast<NVNformat>(54)), -1);   // six stencil values
    CHECK_EQ(stencil_i(&dev, 12, static_cast<NVNformat>(54)), 2);
    CHECK_EQ(stencil_i(&dev, 15, static_cast<NVNformat>(53)), -1);   // that format holds five: the sixth value is out of its range
    CHECK_EQ(stencil_i(&dev, 14, static_cast<NVNformat>(53)), 4);
    CHECK_EQ(stencil_i(&dev, 1, static_cast<NVNformat>(77)), -1);    // not a depth-stencil format
    CHECK(stencil(&dev, 10) == 1);
    CHECK(stencil(&dev, 200) == 0);
    unbind_guest(&dev);
}

// ---- ProgramBuilder -----------------------------------------------------------------------------------------------------------------
NXTEST(program_builder_fields_and_initialize) {
    setup();
    NVNprogramBuilder b;
    std::memset(&b, 0xAB, sizeof b);
    N2<void(NVNprogramBuilder*)>("nvnProgramBuilderSetDefaults")(&b);
    const uint8_t* raw = reinterpret_cast<const uint8_t*>(&b);
    uint32_t v;
    std::memcpy(&v, raw + 0x58, 4);
    CHECK_EQ(v, 2u);                                   // default recursion depths, as the sdk sets them (0x5E5DE0)
    std::memcpy(&v, raw + 0x28, 4);
    CHECK_EQ(v, 0u);
    CHECK_EQ(raw[0x08], 0xABu);                        // the device word is left alone
    CHECK_EQ(raw[0x17], 0xABu);

    NVNdevice* dev = reinterpret_cast<NVNdevice*>(0x1234500);
    N2<void(NVNprogramBuilder*, NVNdevice*)>("nvnProgramBuilderSetDevice")(&b, dev);
    CHECK(N2<NVNdevice*(const NVNprogramBuilder*)>("nvnProgramBuilderGetDevice")(&b) == dev);
    N2<void(NVNprogramBuilder*, int)>("nvnProgramBuilderSetSampleShading")(&b, 3);
    CHECK_EQ(N2<int(const NVNprogramBuilder*)>("nvnProgramBuilderGetSampleShading")(&b), 3);
    NVNshaderData sd[2] = {};
    N2<void(NVNprogramBuilder*, int, const NVNshaderData*)>("nvnProgramBuilderSetShaders")(&b, 2, sd);
    CHECK_EQ(N2<int(const NVNprogramBuilder*)>("nvnProgramBuilderGetShaderCount")(&b), 2);
    CHECK(N2<const NVNshaderData*(const NVNprogramBuilder*)>("nvnProgramBuilderGetShaders")(&b) == sd);
    int hashes[2];
    N2<void(NVNprogramBuilder*, int, const void*)>("nvnProgramBuilderSetDebugDataHashes")(&b, 2, hashes);
    CHECK(N2<const void*(const NVNprogramBuilder*)>("nvnProgramBuilderGetDebugDataHashes")(&b) == hashes);
    void* links[3] = {};
    N2<void(NVNprogramBuilder*, int, void* const*)>("nvnProgramBuilderSetSubroutineLinkage")(&b, 3, links);
    CHECK_EQ(N2<int(const NVNprogramBuilder*)>("nvnProgramBuilderGetSubroutineLinkageCount")(&b), 3);
    CHECK(N2<void* const*(const NVNprogramBuilder*)>("nvnProgramBuilderGetSubroutineLinkage")(&b) == links);

    // InitializeFromBuilder: a program with the builder's sample shading; no shaders is accepted, a shader count without an array is not
    NVNprogram prog;
    std::memset(&prog, 0, sizeof prog);
    N2<void(NVNprogramBuilder*, int, const NVNshaderData*)>("nvnProgramBuilderSetShaders")(&b, 0, nullptr);
    auto init = N2<NVNboolean(NVNprogram*, const NVNprogramBuilder*)>("nvnProgramInitializeFromBuilder");
    CHECK(init(&prog, &b) == 1);
    Program* p = host_of<Program>(&prog);
    CHECK(p != nullptr);
    if (p) CHECK_EQ(p->sample_shading, 3u);
    N2<void(NVNprogramBuilder*, int, const NVNshaderData*)>("nvnProgramBuilderSetShaders")(&b, 2, nullptr);
    CHECK(init(&prog, &b) == 0);
    CHECK(init(nullptr, &b) == 0);
    CHECK(init(&prog, nullptr) == 0);
    abi::proc<PFNNVNPROGRAMFINALIZEPROC>("nvnProgramFinalize")(&prog);
}

// ---- device message buffers ---------------------------------------------------------------------------------------------------------
NXTEST(device_message_buffer_reset_read_and_command_reset) {
    setup();
    TestPool tp;
    NVNdeviceMessageBuffer mb;
    std::memset(&mb, 0xCD, sizeof mb);
    N2<void(NVNdeviceMessageBuffer*)>("nvnDeviceMessageBufferSetDefaults")(&mb);
    const uint8_t* raw = reinterpret_cast<const uint8_t*>(&mb);
    CHECK_EQ(raw[0], 0xCDu);   // the device word stays
    for (int i = 8; i < 0x28; ++i) CHECK_EQ(raw[i], 0u);
    N2<void(NVNdeviceMessageBuffer*, NVNdevice*)>("nvnDeviceMessageBufferSetDevice")(&mb, nullptr);
    N2<void(NVNdeviceMessageBuffer*, const NVNmemoryPool*, int, int)>("nvnDeviceMessageBufferSetStorage")(&mb, &tp.guest, 128, 256);

    N2<void(NVNdeviceMessageBuffer*)>("nvnDeviceMessageBufferReset")(&mb);
    uint32_t hdr[3];
    std::memcpy(hdr, tp.mem + 128, 12);
    CHECK(hdr[0] == 12 && hdr[1] == 256 && hdr[2] == 0);

    // two messages after the header: {len, type, id, args, text}
    auto put = [&](uint32_t at, uint32_t len, uint16_t type, int16_t id, const char* text) {
        uint8_t* m = tp.mem + 128 + at;
        std::memset(m, 0, len);
        std::memcpy(m, &len, 4);
        std::memcpy(m + 4, &type, 2);
        std::memcpy(m + 6, &id, 2);
        const uint32_t args[4] = {1, 2, 3, at};
        std::memcpy(m + 8, args, 16);
        std::memcpy(m + 0x18, text, std::strlen(text) + 1);
    };
    put(12, 0x20, 7, -3, "hello");
    put(12 + 0x20, 0x20, 9, 4, "second");
    const uint32_t end = 12 + 0x40;
    std::memcpy(tp.mem + 128, &end, 4);

    auto begin = N2<void(NVNdeviceMessageBuffer*)>("nvnDeviceMessageBufferBeginMessages");
    auto next = N2<NVNboolean(NVNdeviceMessageBuffer*, NVNdeviceMessage*, char*, size_t)>("nvnDeviceMessageBufferGetNextMessage");
    auto finish = N2<void(NVNdeviceMessageBuffer*)>("nvnDeviceMessageBufferEndMessages");
    NVNdeviceMessage msg{};
    char text[16] = {};
    CHECK(next(&mb, &msg, text, sizeof text) == 0);   // before Begin: nothing
    begin(&mb);
    CHECK(next(&mb, &msg, text, sizeof text) == 1);
    CHECK(msg.id == -3 && msg.kind == 7 && msg.args[0] == 1 && msg.args[3] == 12);
    CHECK(std::strcmp(text, "hello") == 0);
    CHECK(next(&mb, &msg, text, 4) == 1);               // a short text buffer cuts and ends the text
    CHECK(msg.id == 4 && std::strcmp(text, "sec") == 0);
    CHECK(next(&mb, &msg, text, sizeof text) == 0);     // the end
    finish(&mb);
    CHECK(next(&mb, &msg, text, sizeof text) == 0);
    // a record whose length runs past the end of the data stops the walk
    const uint32_t bad = 0x400;
    std::memcpy(tp.mem + 128 + 12, &bad, 4);
    begin(&mb);
    CHECK(next(&mb, &msg, text, sizeof text) == 0);
    finish(&mb);

    // a storage that does not fit the pool is unreadable and Reset leaves memory alone
    N2<void(NVNdeviceMessageBuffer*, const NVNmemoryPool*, int, int)>("nvnDeviceMessageBufferSetStorage")(&mb, &tp.guest, 1000, 256);
    std::memset(tp.mem, 0x5A, 16);
    N2<void(NVNdeviceMessageBuffer*)>("nvnDeviceMessageBufferReset")(&mb);
    CHECK_EQ(tp.mem[0], 0x5Au);

    // the command buffer's reset is a GPU-timeline write of the same header
    N2<void(NVNdeviceMessageBuffer*, const NVNmemoryPool*, int, int)>("nvnDeviceMessageBufferSetStorage")(&mb, &tp.guest, 128, 256);
    Recorder r;
    N2<void(NVNcommandBuffer*, const NVNdeviceMessageBuffer*)>("nvnCommandBufferResetDeviceMessageBuffer")(&r.cb, &mb);
    N2<void(NVNcommandBuffer*, int, int, const NVNdeviceMessageBuffer*)>("nvnCommandBufferBindDeviceMessageBuffer")(&r.cb, 1, 0, &mb);
    N2<void(NVNcommandBuffer*, const NVNdeviceMessageBuffer*)>("nvnCommandBufferResetDeviceMessageBuffer")(&r.cb, nullptr);
    const Ref<CmdList> list = r.end();
    CHECK(list);
    if (!list) return;
    CHECK_EQ(list->record_count, 1u);   // only the reset; the bind records nothing
    RecordReader rd(*list);
    CHECK(rd.next());
    CHECK_EQ(static_cast<int>(rd.op()), static_cast<int>(Op::UpdateUniformBuffer));
    const auto& p = rd.payload<P_UpdateUniformBuffer>();
    CHECK_EQ(p.address, tp.pool->gpu_base + 128);
    CHECK_EQ(p.size, 12u);
    const uint32_t* w = reinterpret_cast<const uint32_t*>(rd.blob<P_UpdateUniformBuffer>());
    CHECK(w[0] == 12 && w[1] == 256 && w[2] == 0);
}

// ---- dirty tile maps ----------------------------------------------------------------------------------------------------------------
NXTEST(dirty_tile_map_parameters_roundtrip_and_defaults) {
    setup();
    NVNdirtyTileMapParameters p;
    std::memset(&p, 0x77, sizeof p);
    N2<void(NVNdirtyTileMapParameters*)>("nvnDirtyTileMapParametersSetDefaults")(&p);
    const uint8_t* raw = reinterpret_cast<const uint8_t*>(&p);
    uint32_t v;
    std::memcpy(&v, raw + 0x5C, 4);
    CHECK_EQ(v, 16u);
    std::memcpy(&v, raw + 0x58, 4);
    CHECK_EQ(v, 1u);
    for (int i = 0; i < 0x58; ++i) CHECK_EQ(raw[i], 0u);
    CHECK_EQ(raw[0x74], 0x77u);   // the object past the sdk's 0x74 bytes is not touched

    NVNtexture tex;
    NVNtextureView view;
    NVNcopyRegion region = {1, 2, 3, 40, 20, 1};
    N2<void(NVNdirtyTileMapParameters*, const NVNtexture*, const NVNtextureView*, const NVNcopyRegion*)>("nvnDirtyTileMapParametersSetTexture")(&p, &tex, &view, &region);
    const NVNtexture* gt = nullptr;
    const NVNtextureView* gv = nullptr;
    NVNcopyRegion gr = {};
    N2<void(const NVNdirtyTileMapParameters*, const NVNtexture**, const NVNtextureView**, NVNcopyRegion*)>("nvnDirtyTileMapParametersGetTexture")(&p, &gt, &gv, &gr);
    CHECK(gt == &tex && gv == &view);
    CHECK(gr.xoffset == 1 && gr.width == 40 && gr.height == 20 && gr.depth == 1);

    N2<void(NVNdirtyTileMapParameters*, int, int, int, int)>("nvnDirtyTileMapParametersSetInputs")(&p, 1, 2, 32, 8);
    int a = 0, b = 0, c = 0, d = 0;
    N2<void(const NVNdirtyTileMapParameters*, int*, int*, int*, int*)>("nvnDirtyTileMapParametersGetInputs")(&p, &a, &b, &c, &d);
    CHECK(a == 1 && b == 2 && c == 32 && d == 8);
    N2<void(NVNdirtyTileMapParameters*, NVNbufferAddress, int, int, uint64_t)>("nvnDirtyTileMapParametersSetOutputs")(&p, 0x5000, 8, 64, 0x99);
    NVNbufferAddress addr = 0;
    uint64_t ex = 0;
    N2<void(const NVNdirtyTileMapParameters*, NVNbufferAddress*, int*, int*, uint64_t*)>("nvnDirtyTileMapParametersGetOutputs")(&p, &addr, &a, &b, &ex);
    CHECK(addr == 0x5000 && a == 8 && b == 64 && ex == 0x99);
    const void* table = &tex;
    N2<void(NVNdirtyTileMapParameters*, const void*, int, int, uint64_t)>("nvnDirtyTileMapParametersSetConstantColorTable")(&p, table, 4, 5, 6);
    const void* gtab = nullptr;
    N2<void(const NVNdirtyTileMapParameters*, const void**, int*, int*, uint64_t*)>("nvnDirtyTileMapParametersGetConstantColorTable")(&p, &gtab, &a, &b, &ex);
    CHECK(gtab == table && a == 4 && b == 5 && ex == 6);
    N2<void(NVNdirtyTileMapParameters*, int, int)>("nvnDirtyTileMapParametersSetDepthStencilMask")(&p, 0xF, 0xFF);
    N2<void(const NVNdirtyTileMapParameters*, int*, int*)>("nvnDirtyTileMapParametersGetDepthStencilMask")(&p, &a, &b);
    CHECK(a == 0xF && b == 0xFF);
    N2<void(NVNdirtyTileMapParameters*, int, int, int, int)>("nvnDirtyTileMapParametersSetExpandFootprint")(&p, 1, 2, 3, 4);
    uint8_t l = 0, t = 0, rr = 0, bt = 0;
    N2<void(const NVNdirtyTileMapParameters*, uint8_t*, uint8_t*, uint8_t*, uint8_t*)>("nvnDirtyTileMapParametersGetExpandFootprint")(&p, &l, &t, &rr, &bt);
    CHECK(l == 1 && t == 2 && rr == 3 && bt == 4);
    // the output arguments of every getter may be null
    N2<void(const NVNdirtyTileMapParameters*, int*, int*, int*, int*)>("nvnDirtyTileMapParametersGetInputs")(&p, nullptr, nullptr, nullptr, nullptr);
}

NXTEST(dirty_tile_map_reports_every_tile_dirty) {
    setup();
    TestPool tp;
    std::memset(tp.mem, 0xEE, sizeof tp.mem);
    NVNdirtyTileMapParameters p;
    N2<void(NVNdirtyTileMapParameters*)>("nvnDirtyTileMapParametersSetDefaults")(&p);
    NVNcopyRegion region = {0, 0, 0, 40, 20, 1};   // 3 x 2 tiles of 16 x 16
    N2<void(NVNdirtyTileMapParameters*, const NVNtexture*, const NVNtextureView*, const NVNcopyRegion*)>("nvnDirtyTileMapParametersSetTexture")(&p, nullptr, nullptr, &region);

    const DirtyTileGeometry g0 = dirty_tile_geometry(reinterpret_cast<const uint8_t*>(&p));
    CHECK(g0.tiles_x == 3 && g0.tiles_y == 2 && g0.slices == 1 && g0.row_bytes == 3 && !g0.bit_per_tile);

    // one byte per tile, rows 4 bytes apart: a word per row, the pad byte is written as 0
    N2<void(NVNdirtyTileMapParameters*, NVNbufferAddress, int, int, uint64_t)>("nvnDirtyTileMapParametersSetOutputs")(&p, tp.pool->gpu_base + 16, 4, 0, 0);
    {
        Recorder r;
        N2<void(NVNcommandBuffer*, const NVNdirtyTileMapParameters*)>("nvnCommandBufferComputeDirtyTileMapEx")(&r.cb, &p);
        const Ref<CmdList> list = r.end();
        CHECK(list);
        if (!list) return;
        CHECK_EQ(list->record_count, 1u);   // packed rows: one update for the slice
        RecordReader rd(*list);
        CHECK(rd.next());
        CHECK_EQ(static_cast<int>(rd.op()), static_cast<int>(Op::UpdateUniformBuffer));
        const auto& up = rd.payload<P_UpdateUniformBuffer>();
        CHECK_EQ(up.address, tp.pool->gpu_base + 16);
        CHECK_EQ(up.size, 8u);
        const uint8_t* d = rd.blob<P_UpdateUniformBuffer>();
        const uint8_t want[8] = {1, 1, 1, 0, 1, 1, 1, 0};
        CHECK(std::memcmp(d, want, 8) == 0);
    }
    // one bit per tile: flags bit 0; a row is one byte (3 tiles: 0b111), stride 8: one update per row
    N2<void(NVNdirtyTileMapParameters*, int, int, int, int)>("nvnDirtyTileMapParametersSetInputs")(&p, 1, 0, 16, 16);
    N2<void(NVNdirtyTileMapParameters*, NVNbufferAddress, int, int, uint64_t)>("nvnDirtyTileMapParametersSetOutputs")(&p, tp.pool->gpu_base + 32, 8, 0, 0);
    {
        Recorder r;
        N2<void(NVNcommandBuffer*, const NVNdirtyTileMapParameters*)>("nvnCommandBufferComputeDirtyTileMapEx")(&r.cb, &p);
        const Ref<CmdList> list = r.end();
        CHECK(list);
        if (!list) return;
        CHECK_EQ(list->record_count, 2u);
        RecordReader rd(*list);
        for (uint32_t row = 0; row < 2; ++row) {
            CHECK(rd.next());
            const auto& up = rd.payload<P_UpdateUniformBuffer>();
            CHECK_EQ(up.address, tp.pool->gpu_base + 32 + 8 * row);
            CHECK_EQ(up.size, 4u);
            const uint8_t* d = rd.blob<P_UpdateUniformBuffer>();
            CHECK(d[0] == 0x07 && d[1] == 0 && d[2] == 0 && d[3] == 0);
        }
    }
    // the older call builds the same parameters
    {
        Recorder r;
        N2<void(NVNcommandBuffer*, const NVNtexture*, const NVNtextureView*, const NVNcopyRegion*, int, NVNbufferAddress, int, int)>("nvnCommandBufferComputeDirtyTileMap")(
            &r.cb, nullptr, nullptr, &region, 0, tp.pool->gpu_base + 16, 4, 0);
        const Ref<CmdList> list = r.end();
        CHECK(list);
        if (list) CHECK_EQ(list->record_count, 1u);
    }
    // no output address: nothing to write
    {
        N2<void(NVNdirtyTileMapParameters*, NVNbufferAddress, int, int, uint64_t)>("nvnDirtyTileMapParametersSetOutputs")(&p, 0, 4, 0, 0);
        Recorder r;
        N2<void(NVNcommandBuffer*, const NVNdirtyTileMapParameters*)>("nvnCommandBufferComputeDirtyTileMapEx")(&r.cb, &p);
        const Ref<CmdList> list = r.end();
        CHECK(list);
        if (list) CHECK_EQ(list->record_count, 0u);
    }
}

// ---- pool registry: registration serials and re-registration statistics ([nvn] keep_pool_check, K1/K2 of the 1% lows round) ---------
NXTEST(pool_registered_again_over_the_same_range_is_another_registration) {
    setup();
    alignas(64) static uint8_t mem[4096];
    const uint64_t base = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(mem));
    auto make = [&](uint64_t off, uint64_t size) {
        MemoryPool* p = new MemoryPool();
        p->base = mem + off;
        p->size = size;
        p->gpu_base = base + off;
        return p;
    };
    const PoolRegStats s0 = pool_registry_stats();
    MemoryPool* a = make(0, 2048);
    pool_register(a);
    const uint64_t sa = a->reg_serial;
    CHECK(sa != 0);
    CHECK(pool_lookup(base + 100).pool == a);
    const uint64_t ug = pool_unregister_generation();
    pool_unregister(a);
    CHECK(pool_unregister_generation() != ug);
    CHECK(pool_lookup(base + 100).pool == nullptr);
    MemoryPool* b = make(0, 2048);   // the same range again: a new registration (a kept set bound to `a` must not pass as bound to `b`)
    pool_register(b);
    CHECK(b->reg_serial != sa);
    CHECK(pool_lookup(base + 100).pool == b);
    MemoryPool* c = make(2048, 1024);   // a new range: counted as neither
    pool_register(c);
    const PoolRegStats s1 = pool_registry_stats();
    CHECK_EQ(s1.registered - s0.registered, 3u);
    CHECK_EQ(s1.unregistered - s0.unregistered, 1u);
    CHECK_EQ(s1.same_range - s0.same_range, 1u);
    CHECK_EQ(s1.same_base - s0.same_base, 1u);
    CHECK_EQ(s1.overlap - s0.overlap, 1u);
    pool_unregister(b);
    pool_unregister(c);
}
