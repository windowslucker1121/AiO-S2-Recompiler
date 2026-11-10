// SPDX-License-Identifier: GPL-3.0-or-later
// NVN2 DLSS (src/gfx/nvn/api_dlss.cpp): every nvnDlss* function is called through its thunk the way the game calls it, the builder / network / parameters
// layouts round-trip, and nvnCommandBufferEvaluateDlss is recorded and replayed: with no temporal upscaler selected a 4x4 image is scaled to 8x8 by the
// plain path and the output size and pixels are read back.
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include "nvn_app.h"
#include "nvn_impl.h"
#include "nxrt/extras.h"
NXTEST_GROUP(gfx_nvn_dlss)

using namespace nxrt;
using namespace nxrt::nvn;
using nvnapp::App;

namespace {

template <class Sig>
abi::Caller<Sig> proc(const char* name) {
    return abi::Caller<Sig>(nxrt::nvn::proc_address(name));
}

// The three objects as the game holds them: zero-filled, 8-byte aligned storage of the sizes the sdk uses (docs/nvn2_abi.md).
struct Objects {
    std::vector<uint64_t> builder = std::vector<uint64_t>(0xB8 / 8, 0xABABABABABABABABull);
    std::vector<uint64_t> network = std::vector<uint64_t>(0x220 / 8, 0);
    std::vector<uint64_t> params = std::vector<uint64_t>(0xC0 / 8, 0xABABABABABABABABull);
    void* b() { return builder.data(); }
    void* n() { return network.data(); }
    void* p() { return params.data(); }
};

void setup() {
    install();
    abi::reset_mismatches();
    register_all_impls();
}

}  // namespace

NXTEST(builder_network_and_parameters_round_trip) {
    setup();
    Objects o;
    proc<void(void*)>("nvnDlssNetworkBuilderSetDefaults")(o.b());
    CHECK_EQ(proc<uint32_t(const void*)>("nvnDlssNetworkBuilderGetMaxEvaluationEntries")(o.b()), 1u);
    int dev_dummy = 0;
    proc<void(void*, void*)>("nvnDlssNetworkBuilderSetDevice")(o.b(), &dev_dummy);
    CHECK_EQ(proc<uint64_t(const void*)>("nvnDlssNetworkBuilderGetDevice")(o.b()), reinterpret_cast<uint64_t>(&dev_dummy));
    proc<void(void*, uint32_t)>("nvnDlssNetworkBuilderSetFlags")(o.b(), 0x4Eu);
    CHECK_EQ(proc<uint32_t(const void*)>("nvnDlssNetworkBuilderGetFlags")(o.b()), 0x4Eu);
    proc<void(void*, int)>("nvnDlssNetworkBuilderSetMethod")(o.b(), 1);
    CHECK_EQ(proc<uint32_t(const void*)>("nvnDlssNetworkBuilderGetMethod")(o.b()), 1u);
    proc<void(void*, int)>("nvnDlssNetworkBuilderSetPreset")(o.b(), 9);
    CHECK_EQ(proc<uint32_t(const void*)>("nvnDlssNetworkBuilderGetPreset")(o.b()), 9u);
    proc<void(void*, int, int)>("nvnDlssNetworkBuilderSetOutputResolution")(o.b(), 1920, 1080);
    int w = 0, h = 0;
    proc<void(const void*, int*, int*)>("nvnDlssNetworkBuilderGetOutputResolution")(o.b(), &w, &h);
    CHECK_EQ(w, 1920);
    CHECK_EQ(h, 1080);
    char storage[16];
    proc<void(void*, void*, size_t)>("nvnDlssNetworkBuilderSetStorage")(o.b(), storage, 0x40000);
    CHECK_EQ(proc<uint64_t(const void*)>("nvnDlssNetworkBuilderGetMemory")(o.b()), reinterpret_cast<uint64_t>(storage));
    CHECK_EQ(proc<uint64_t(const void*)>("nvnDlssNetworkBuilderGetMemorySize")(o.b()), 0x40000ull);
    proc<void(void*, int)>("nvnDlssNetworkBuilderSetMaxEvaluationEntries")(o.b(), 3);
    CHECK_EQ(proc<uint32_t(const void*)>("nvnDlssNetworkBuilderGetMaxEvaluationEntries")(o.b()), 3u);
    int ids[32], back[32];
    for (int i = 0; i < 32; ++i) ids[i] = 100 + i;
    int pool_dummy = 0;
    proc<void(void*, void*, const int*)>("nvnDlssNetworkBuilderSetTextureIDs")(o.b(), &pool_dummy, ids);
    proc<void(const void*, int*)>("nvnDlssNetworkBuilderGetTextureIDs")(o.b(), back);
    CHECK_EQ(std::memcmp(ids, back, sizeof ids), 0);
    CHECK_EQ(proc<uint64_t(const void*)>("nvnDlssNetworkBuilderGetTexturePool")(o.b()), reinterpret_cast<uint64_t>(&pool_dummy));
    CHECK(proc<uint64_t(const void*)>("nvnDlssNetworkBuilderGetStorageSize")(o.b()) >= 0x1000);

    // with no temporal upscaler selected the optimal render size is the output size, as one size (largest = smallest)
    int ow = 0, oh = 0, xw = 0, xh = 0, nw = 0, nh = 0;
    CHECK(proc<bool(const void*, int*, int*, int*, int*, int*, int*)>("nvnDlssNetworkBuilderGetOptimalSettings")(o.b(), &ow, &oh, &xw, &xh, &nw, &nh));
    CHECK_EQ(ow, 1920);
    CHECK_EQ(oh, 1080);
    CHECK(xw == ow && xh == oh && nw == ow && nh == oh);

    // the network
    CHECK(proc<bool(void*, const void*)>("nvnDlssNetworkInitialize")(o.n(), o.b()));
    CHECK(proc<bool(void*, int, int)>("nvnDlssNetworkResize")(o.n(), 1280, 720));
    CHECK(!proc<bool(void*, int, int)>("nvnDlssNetworkResize")(o.n(), 2560, 1440));   // beyond the size it was made for
    proc<void(void*)>("nvnDlssNetworkFinalize")(o.n());
    CHECK(!proc<bool(void*, int, int)>("nvnDlssNetworkResize")(o.n(), 1280, 720));
    int zero_builder_ok = proc<bool(void*, const void*)>("nvnDlssNetworkInitialize")(o.n(), [] {
        static std::vector<uint64_t> empty(0xB8 / 8, 0);
        return empty.data();
    }());
    CHECK_EQ(zero_builder_ok, 0);   // no output resolution: no network

    // the parameters
    void* p = o.p();
    proc<void(void*)>("nvnDlssParametersSetDefaults")(p);
    float jx = 9, jy = 9, mx = 0, my = 0;
    proc<float(const void*, float*, float*)>("nvnDlssParametersGetMotionVectorScale")(p, &mx, &my);
    CHECK(mx == 1.0f && my == 1.0f);
    CHECK_EQ(proc<float(const void*)>("nvnDlssParametersGetPreExposure")(p), 1.0f);
    proc<float(const void*, float*, float*)>("nvnDlssParametersGetJitterOffset")(p, &jx, &jy);
    CHECK(jx == 0.0f && jy == 0.0f);
    proc<void(void*, float, float)>("nvnDlssParametersSetJitterOffset")(p, 0.25f, -0.5f);
    proc<float(const void*, float*, float*)>("nvnDlssParametersGetJitterOffset")(p, &jx, &jy);
    CHECK(jx == 0.25f && jy == -0.5f);
    proc<void(void*, float, float)>("nvnDlssParametersSetMotionVectorScale")(p, 2.0f, 3.0f);
    proc<float(const void*, float*, float*)>("nvnDlssParametersGetMotionVectorScale")(p, &mx, &my);
    CHECK(mx == 2.0f && my == 3.0f);
    proc<void(void*, float)>("nvnDlssParametersSetPreExposure")(p, 0.5f);
    CHECK_EQ(proc<float(const void*)>("nvnDlssParametersGetPreExposure")(p), 0.5f);
    proc<void(void*, int)>("nvnDlssParametersSetHistoryReset")(p, 1);
    CHECK_EQ(proc<uint32_t(const void*)>("nvnDlssParametersGetHistoryReset")(p), 1u);
    proc<void(void*, int)>("nvnDlssParametersSetEvaluationIndex")(p, 2);
    CHECK_EQ(proc<uint32_t(const void*)>("nvnDlssParametersGetEvaluationIndex")(p), 2u);
    struct Pair {
        const char* set;
        const char* get;
    };
    const Pair pairs[] = {{"nvnDlssParametersSetInputColorSubrectangleBase", "nvnDlssParametersGetInputColorSubrectangleBase"},
                          {"nvnDlssParametersSetDepthSubrectangleBase", "nvnDlssParametersGetDepthSubrectangleBase"},
                          {"nvnDlssParametersSetMotionVectorSubrectangleBase", "nvnDlssParametersGetMotionVectorSubrectangleBase"},
                          {"nvnDlssParametersSetBiasCurrentColorSubrectangleBase", "nvnDlssParametersGetBiasCurrentColorSubrectangleBase"},
                          {"nvnDlssParametersSetOutputSubrectangleBase", "nvnDlssParametersGetOutputSubrectangleBase"},
                          {"nvnDlssParametersSetInputSubrectangleSize", "nvnDlssParametersGetInputSubrectangleSize"}};
    int k = 10;
    for (const Pair& pr : pairs) {
        proc<void(void*, int, int)>(pr.set)(p, k, k + 1);
        int a = 0, b = 0;
        proc<void(const void*, int*, int*)>(pr.get)(p, &a, &b);
        CHECK_MSG(a == k && b == k + 1, pr.get);
        k += 10;
    }
    // the texture pairs and the overlay switches are stored (nothing to read back but the neighbours stay intact)
    int tex_dummy = 0;
    for (const char* n : {"nvnDlssParametersSetInputColor", "nvnDlssParametersSetOutputColor", "nvnDlssParametersSetMotionVectors", "nvnDlssParametersSetDepth",
                          "nvnDlssParametersSetBiasCurrentColorMask", "nvnDlssParametersSetExposureTexture"})
        proc<void(void*, const void*, const void*)>(n)(p, &tex_dummy, nullptr);
    for (const char* n : {"nvnDlssParametersSetOverlayDebugLayer", "nvnDlssParametersSetOverlayFullScreen", "nvnDlssParametersSetOverlayShowNans",
                          "nvnDlssParametersSetOverlayJitterDebug"})
        proc<void(void*, int)>(n)(p, 0);
    proc<float(const void*, float*, float*)>("nvnDlssParametersGetJitterOffset")(p, &jx, &jy);
    CHECK(jx == 0.25f && jy == -0.5f);
    CHECK_EQ(abi::mismatches(), 0u);
}

// The plain path: no temporal mode is selected ([upscale] mode off), so nvnCommandBufferEvaluateDlss scales the input rectangle into the output
// texture with the bilinear filter. A 4x4 picture (left half red, right half blue) becomes 8x8.
NXTEST(evaluate_dlss_scales_a_small_image_through_the_plain_path) {
    App app;
    App::Opts o;
    o.width = 64;
    o.height = 48;
    REQUIRE(app.init(o));
    auto src = app.make_texture_2d(NVN_FORMAT_RGBA8, 4, 4);
    auto dst = app.make_texture_2d(NVN_FORMAT_RGBA8, 8, 8);
    REQUIRE(src != nullptr && dst != nullptr);

    uint8_t pic[4 * 4 * 4];
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) {
            uint8_t* px = pic + (y * 4 + x) * 4;
            px[0] = x < 2 ? 255 : 0;
            px[1] = 0;
            px[2] = x < 2 ? 0 : 255;
            px[3] = 255;
        }
    App::Buf up = app.make_buffer(sizeof pic, pic);
    App::Buf back = app.make_buffer(8 * 8 * 4);
    std::memset(back.ptr, 0xEE, 8 * 8 * 4);

    Objects obj;
    proc<void(void*)>("nvnDlssNetworkBuilderSetDefaults")(obj.b());
    proc<void(void*, void*)>("nvnDlssNetworkBuilderSetDevice")(obj.b(), app.device.get());
    proc<void(void*, int, int)>("nvnDlssNetworkBuilderSetOutputResolution")(obj.b(), 8, 8);
    REQUIRE(proc<bool(void*, const void*)>("nvnDlssNetworkInitialize")(obj.n(), obj.b()));
    proc<void(void*)>("nvnDlssParametersSetDefaults")(obj.p());
    proc<void(void*, const void*, const void*)>("nvnDlssParametersSetInputColor")(obj.p(), src->obj.get(), nullptr);
    proc<void(void*, const void*, const void*)>("nvnDlssParametersSetOutputColor")(obj.p(), dst->obj.get(), nullptr);
    proc<void(void*, int, int)>("nvnDlssParametersSetInputSubrectangleSize")(obj.p(), 4, 4);

    auto cb = app.make_cmdbuf();
    REQUIRE(cb != nullptr);
    NVNcommandBuffer* c = cb->obj.get();
    app.api.nvnCommandBufferBeginRecording(c);
    app.api.nvnCommandBufferSetCopyRowStride(c, 16);
    NVNcopyRegion up_region{0, 0, 0, 4, 4, 1};
    app.api.nvnCommandBufferCopyBufferToTexture(c, up.address, src->obj.get(), nullptr, &up_region, NVN_COPY_FLAGS_NONE);
    proc<void(NVNcommandBuffer*, const void*, const void*)>("nvnCommandBufferEvaluateDlss")(c, obj.n(), obj.p());
    app.api.nvnCommandBufferSetCopyRowStride(c, 32);
    NVNcopyRegion down_region{0, 0, 0, 8, 8, 1};
    app.api.nvnCommandBufferCopyTextureToBuffer(c, dst->obj.get(), nullptr, &down_region, back.address, NVN_COPY_FLAGS_NONE);
    app.end_and_submit(*cb);
    app.finish();

    // output size: every one of the 8 x 8 pixels was written (the sentinel is gone), and the picture is the enlarged input
    size_t untouched = 0, wrong = 0;
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) {
            const uint8_t* px = back.ptr + (y * 8 + x) * 4;
            if (px[0] == 0xEE && px[1] == 0xEE && px[2] == 0xEE && px[3] == 0xEE) ++untouched;
            if (x <= 2 && !(px[0] == 255 && px[1] == 0 && px[2] == 0 && px[3] == 255)) ++wrong;   // only red source texels reach these
            if (x >= 5 && !(px[0] == 0 && px[1] == 0 && px[2] == 255 && px[3] == 255)) ++wrong;   // only blue ones reach these
        }
    std::printf("    pixel (0,0) %d %d %d %d, pixel (7,7) %d %d %d %d, %zu unwritten, %zu wrong\n", back.ptr[0], back.ptr[1], back.ptr[2], back.ptr[3],
                back.ptr[(7 * 8 + 7) * 4], back.ptr[(7 * 8 + 7) * 4 + 1], back.ptr[(7 * 8 + 7) * 4 + 2], back.ptr[(7 * 8 + 7) * 4 + 3], untouched, wrong);
    CHECK_EQ(untouched, 0u);
    CHECK_EQ(wrong, 0u);
    const auto v = app.validation();
    CHECK_EQ(v.errors, 0ull);
    app.shutdown();
}

// ---- fallback blit with rectangles, and the temporal modes without their DLLs ------------------------------------------------------------------
namespace {

#define REQ_EMPTY(c) do { if (!(c)) { CHECK_MSG(false, #c); return {}; } } while (0)

struct EvalCase {
    int sw, sh, dw, dh;                  // source and destination texture sizes
    int in_bx, in_by, in_w, in_h;        // colour rectangle (base, size)
    int out_bx, out_by, out_w, out_h;    // output rectangle base and the network's output size
    bool temporal_inputs = false;        // also give depth and motion textures
};

// Source texel (x, y) = what src_px writes; the destination starts as 0xEE everywhere. Returns the destination after the evaluation.
template <class F>
std::vector<uint8_t> run_eval(const EvalCase& c, F src_px) {
    App app;
    App::Opts o;
    o.width = 64;
    o.height = 48;
    REQ_EMPTY(app.init(o));
    auto src = app.make_texture_2d(NVN_FORMAT_RGBA8, c.sw, c.sh);
    auto dst = app.make_texture_2d(NVN_FORMAT_RGBA8, c.dw, c.dh);
    decltype(app.make_texture_2d(0, 0, 0)) depth, motion;
    if (c.temporal_inputs) {
        depth = app.make_texture_2d(NVN_FORMAT_DEPTH32F, c.sw, c.sh, NVN_TEXTURE_FLAGS_COMPRESSIBLE, 1, false);
        motion = app.make_texture_2d(NVN_FORMAT_RG16F, c.sw, c.sh);
        REQ_EMPTY(depth != nullptr && motion != nullptr);
    }
    REQ_EMPTY(src != nullptr && dst != nullptr);
    std::vector<uint8_t> pic(static_cast<size_t>(c.sw) * c.sh * 4), fill(static_cast<size_t>(c.dw) * c.dh * 4, 0xEE);
    for (int y = 0; y < c.sh; ++y)
        for (int x = 0; x < c.sw; ++x) src_px(x, y, &pic[(static_cast<size_t>(y) * c.sw + x) * 4]);
    App::Buf up = app.make_buffer(pic.size(), pic.data());
    App::Buf init = app.make_buffer(fill.size(), fill.data());
    App::Buf back = app.make_buffer(fill.size());

    Objects obj;
    proc<void(void*)>("nvnDlssNetworkBuilderSetDefaults")(obj.b());
    proc<void(void*, void*)>("nvnDlssNetworkBuilderSetDevice")(obj.b(), app.device.get());
    proc<void(void*, int, int)>("nvnDlssNetworkBuilderSetOutputResolution")(obj.b(), c.out_w, c.out_h);
    REQ_EMPTY(proc<bool(void*, const void*)>("nvnDlssNetworkInitialize")(obj.n(), obj.b()));
    proc<void(void*)>("nvnDlssParametersSetDefaults")(obj.p());
    proc<void(void*, const void*, const void*)>("nvnDlssParametersSetInputColor")(obj.p(), src->obj.get(), nullptr);
    proc<void(void*, const void*, const void*)>("nvnDlssParametersSetOutputColor")(obj.p(), dst->obj.get(), nullptr);
    if (c.temporal_inputs) {
        proc<void(void*, const void*, const void*)>("nvnDlssParametersSetDepth")(obj.p(), depth->obj.get(), nullptr);
        proc<void(void*, const void*, const void*)>("nvnDlssParametersSetMotionVectors")(obj.p(), motion->obj.get(), nullptr);
    }
    proc<void(void*, int, int)>("nvnDlssParametersSetInputSubrectangleSize")(obj.p(), c.in_w, c.in_h);
    proc<void(void*, int, int)>("nvnDlssParametersSetInputColorSubrectangleBase")(obj.p(), c.in_bx, c.in_by);
    proc<void(void*, int, int)>("nvnDlssParametersSetOutputSubrectangleBase")(obj.p(), c.out_bx, c.out_by);

    auto cb = app.make_cmdbuf();
    REQ_EMPTY(cb != nullptr);
    NVNcommandBuffer* cmd = cb->obj.get();
    app.api.nvnCommandBufferBeginRecording(cmd);
    app.api.nvnCommandBufferSetCopyRowStride(cmd, c.sw * 4);
    NVNcopyRegion up_region{0, 0, 0, c.sw, c.sh, 1};
    app.api.nvnCommandBufferCopyBufferToTexture(cmd, up.address, src->obj.get(), nullptr, &up_region, NVN_COPY_FLAGS_NONE);
    app.api.nvnCommandBufferSetCopyRowStride(cmd, c.dw * 4);
    NVNcopyRegion init_region{0, 0, 0, c.dw, c.dh, 1};
    app.api.nvnCommandBufferCopyBufferToTexture(cmd, init.address, dst->obj.get(), nullptr, &init_region, NVN_COPY_FLAGS_NONE);
    proc<void(NVNcommandBuffer*, const void*, const void*)>("nvnCommandBufferEvaluateDlss")(cmd, obj.n(), obj.p());
    app.api.nvnCommandBufferCopyTextureToBuffer(cmd, dst->obj.get(), nullptr, &init_region, back.address, NVN_COPY_FLAGS_NONE);
    app.end_and_submit(*cb);
    app.finish();
    std::vector<uint8_t> result(back.ptr, back.ptr + fill.size());
    CHECK_EQ(app.validation().errors, 0ull);
    app.shutdown();
    return result;
}

// Same size: a copy of the colour rectangle (bit exact) placed at the output base; everything outside stays as it was.
void check_exact_copy(const EvalCase& c, const std::vector<uint8_t>& out) {
    size_t bad = 0;
    for (int y = 0; y < c.dh; ++y)
        for (int x = 0; x < c.dw; ++x) {
            const uint8_t* px = &out[(static_cast<size_t>(y) * c.dw + x) * 4];
            const int rx = x - c.out_bx, ry = y - c.out_by;
            if (rx >= 0 && ry >= 0 && rx < c.out_w && ry < c.out_h) {
                const int sx = c.in_bx + rx, sy = c.in_by + ry;
                if (px[0] != sx * 16 || px[1] != sy * 16 || px[2] != 7 || px[3] != 255) ++bad;
            } else if (px[0] != 0xEE || px[1] != 0xEE || px[2] != 0xEE || px[3] != 0xEE) ++bad;
        }
    CHECK_EQ(bad, 0u);
}

void gradient(int x, int y, uint8_t* px) {
    px[0] = static_cast<uint8_t>(x * 16);
    px[1] = static_cast<uint8_t>(y * 16);
    px[2] = 7;
    px[3] = 255;
}

}  // namespace

NXTEST(evaluate_dlss_copies_exactly_at_equal_size_with_rectangle_bases) {
    setup();
    const EvalCase c{8, 8, 12, 10, 2, 1, 4, 4, 5, 3, 4, 4};   // 4x4 of the 8x8 source from (2, 1) into the 12x10 output at (5, 3)
    check_exact_copy(c, run_eval(c, gradient));
}

NXTEST(evaluate_dlss_scales_a_rectangle_into_a_rectangle_bilinearly) {
    setup();
    // a 4x4 rectangle of colour B at (2, 2) in a source of colour A, enlarged to 8x8 at (4, 4) in a 16x16 output: the interior is B, the rest untouched
    const EvalCase c{8, 8, 16, 16, 2, 2, 4, 4, 4, 4, 8, 8};
    const auto out = run_eval(c, [](int x, int y, uint8_t* px) {
        const bool b = x >= 2 && x < 6 && y >= 2 && y < 6;
        px[0] = b ? 200 : 10;
        px[1] = b ? 100 : 20;
        px[2] = b ? 50 : 30;
        px[3] = 255;
    });
    size_t bad = 0;
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x) {
            const uint8_t* px = &out[(static_cast<size_t>(y) * 16 + x) * 4];
            const bool inside = x >= 4 && x < 12 && y >= 4 && y < 12;
            const bool interior = x >= 6 && x < 10 && y >= 6 && y < 10;   // away from the rectangle's edge, where the filter may reach past it
            if (!inside && !(px[0] == 0xEE && px[1] == 0xEE && px[2] == 0xEE && px[3] == 0xEE)) ++bad;
            if (inside && px[0] == 0xEE && px[3] == 0xEE) ++bad;   // every output texel of the rectangle was written
            if (interior && !(px[0] == 200 && px[1] == 100 && px[2] == 50 && px[3] == 255)) ++bad;
        }
    CHECK_EQ(bad, 0u);
}

// [upscale] mode = dlss / dlaa / fsr3 with no SDK DLLs here (none is shipped): the call falls back to the plain copy, no crash, no missing output.
NXTEST(evaluate_dlss_falls_back_to_the_plain_path_when_the_sdk_dlls_are_missing) {
    setup();
    const extras::Settings saved = extras::settings();
    extras::Settings s = saved;
    for (extras::UpscaleMode m : {extras::UpscaleMode::Dlss, extras::UpscaleMode::Dlaa, extras::UpscaleMode::Fsr3}) {
        s.upscale.mode = m;
        s.upscale.dlss_dll = "C:\\nonexistent-streamline-folder";
        s.upscale.fsr_dll = "C:\\nonexistent-fidelityfx-folder";
        extras::set_settings(s);
        EvalCase c{8, 8, 12, 10, 2, 1, 4, 4, 5, 3, 4, 4};
        c.temporal_inputs = true;
        check_exact_copy(c, run_eval(c, gradient));
    }
    extras::set_settings(saved);
}
