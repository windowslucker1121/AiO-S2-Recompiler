// SPDX-License-Identifier: GPL-3.0-or-later
// NVN scene oracles, render state: depth test and depth write order, blending (funcs, equations, blend constants), channel masks,
// stencil, scissored / multi-target / integer clears, polygon offset and logic op. Every scene is drawn with REAL translated
// shaders of title E (work/shader_cache): the position-only vertex shader a656883b0cdb8ebc (gl_Position =
// attribute 0), and constant-colour fragment shaders 062cfa5657f5388d (magenta 1,0,1,1), f73f4dbe319ac111 (red 1,0,0,1),
// d46b10898ec7c247 (black 0,0,0,1), 42ec6ac7b60dc032 (magenta with alpha 0.5). The frame is read back and compared pixel by pixel
// with values computed from the geometry. Skipped when the shader data is not on this machine.
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iterator>

#include "nvn_app.h"
NXTEST_GROUP(gfx_nvn_scene_state)

using namespace nvnapp;

namespace {

constexpr int kW = 64, kH = 48;
const std::string kRefDir = nxtest::project_path("work/nvn_refs/");

struct Rgba {
    int r, g, b, a;
    bool operator==(const Rgba& o) const { return r == o.r && g == o.g && b == o.b && a == o.a; }
};
constexpr Rgba KBLACK{0, 0, 0, 255}, KRED{255, 0, 0, 255}, KMAG{255, 0, 255, 255};

Rgba px_at(const std::vector<uint8_t>& px, int x, int y) {
    const uint8_t* p = &px[(static_cast<size_t>(y) * kW + x) * 4];
    return {p[0], p[1], p[2], p[3]};
}

// Every pixel of the inclusive rectangle [x0,x1]x[y0,y1] must equal `want` within `tol` per channel. Prints the first mismatch.
bool region_is(const std::vector<uint8_t>& px, int x0, int x1, int y0, int y1, Rgba want, int tol, const char* what) {
    int bad = 0;
    Rgba first{};
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
            const Rgba p = px_at(px, x, y);
            if (std::abs(p.r - want.r) > tol || std::abs(p.g - want.g) > tol || std::abs(p.b - want.b) > tol || std::abs(p.a - want.a) > tol) {
                if (!bad) first = p;
                ++bad;
            }
        }
    if (bad)
        std::printf("      %s: %d pixels differ, first %d %d %d %d (want %d %d %d %d)\n", what, bad, first.r, first.g, first.b, first.a, want.r,
                    want.g, want.b, want.a);
    return bad == 0;
}

// Two triangles covering the NDC rectangle [x0,x1]x[y0,y1] at depth z (x, y, z per vertex: the input of the real vertex shader).
std::vector<float> quad(float x0, float x1, float y0, float y1, float z) {
    return {x0, y0, z, x1, y0, z, x1, y1, z, x0, y0, z, x1, y1, z, x0, y1, z};
}

// The real translated shaders, as programs. The blobs must outlive the programs: keep this object where it is (no copies).
struct Progs {
    App::ShaderBlob vs, mag, red, black, half;
    std::unique_ptr<NVNprogram> p_mag, p_red, p_black, p_half;
    bool load(App& app) {
        if (!App::load_shader("a656883b0cdb8ebc", &vs)) return false;
        if (!App::load_shader("062cfa5657f5388d", &mag)) return false;
        if (!App::load_shader("f73f4dbe319ac111", &red)) return false;
        if (!App::load_shader("d46b10898ec7c247", &black)) return false;
        if (!App::load_shader("42ec6ac7b60dc032", &half)) return false;
        p_mag = app.make_program({&vs, &mag});
        p_red = app.make_program({&vs, &red});
        p_black = app.make_program({&vs, &black});
        p_half = app.make_program({&vs, &half});
        return p_mag && p_red && p_black && p_half;
    }
};

// Every render state the scenes use, at its NVN default, with back-face culling off so the winding of the quads does not matter.
struct Gfx {
    NVNblendState bs;
    NVNcolorState cs;
    NVNchannelMaskState ms;
    NVNpolygonState ps;
    NVNdepthStencilState ds;
    NVNmultisampleState mss;
    NVNvertexAttribState attr;
    NVNvertexStreamState stream;
    void defaults(App& app) {
        auto& api = app.api;
        api.nvnBlendStateSetDefaults(&bs);
        api.nvnColorStateSetDefaults(&cs);
        api.nvnChannelMaskStateSetDefaults(&ms);
        api.nvnPolygonStateSetDefaults(&ps);
        api.nvnPolygonStateSetCullFace(&ps, NVN_FACE_NONE);
        api.nvnDepthStencilStateSetDefaults(&ds);
        api.nvnMultisampleStateSetDefaults(&mss);
        api.nvnVertexAttribStateSetDefaults(&attr);
        api.nvnVertexAttribStateSetFormat(&attr, NVN_FORMAT_RGB32F, 0);
        api.nvnVertexAttribStateSetStreamIndex(&attr, 0);
        api.nvnVertexStreamStateSetDefaults(&stream);
        api.nvnVertexStreamStateSetStride(&stream, 12);
    }
    void bind(App& app, NVNcommandBuffer* c) {
        auto& api = app.api;
        api.nvnCommandBufferBindBlendState(c, &bs);
        api.nvnCommandBufferBindColorState(c, &cs);
        api.nvnCommandBufferBindChannelMaskState(c, &ms);
        api.nvnCommandBufferBindPolygonState(c, &ps);
        api.nvnCommandBufferBindDepthStencilState(c, &ds);
        api.nvnCommandBufferBindMultisampleState(c, &mss);
        api.nvnCommandBufferBindVertexAttribState(c, 1, &attr);
        api.nvnCommandBufferBindVertexStreamState(c, 1, &stream);
    }
};

// A fresh command buffer with the render targets set and the full-frame viewport and scissor.
std::unique_ptr<App::CmdBuf> begin_frame(App& app, const std::vector<NVNtexture*>& colors, NVNtexture* depth, int w, int h) {
    auto cb = app.make_cmdbuf();
    if (!cb) return nullptr;
    NVNcommandBuffer* c = cb->obj.get();
    app.api.nvnCommandBufferBeginRecording(c);
    app.api.nvnCommandBufferSetRenderTargets(c, static_cast<int>(colors.size()), colors.data(), nullptr, depth, nullptr);
    app.api.nvnCommandBufferSetViewport(c, 0, 0, w, h);
    app.api.nvnCommandBufferSetScissor(c, 0, 0, w, h);
    return cb;
}

// A render target read back by copy. The presented frame forces alpha to 255 (src/gfx/vk_renderer.cpp, the capture), so the scenes
// that check alpha render into one of these instead of the window.
struct Readback {
    std::unique_ptr<Tex> tex;
    App::Buf buf;
};
Readback make_readback(App& app) {
    Readback r;
    r.tex = app.make_texture_2d(NVN_FORMAT_RGBA8, kW, kH, 0, 1, false);
    r.buf = app.make_buffer(static_cast<size_t>(kW) * kH * 4);
    return r;
}
// The texels of a readback, after the recorded work: waits for the queue, then copies the buffer out.
std::vector<uint8_t> texels(App& app, const App::Buf& buf) {
    app.finish();
    return std::vector<uint8_t>(buf.ptr, buf.ptr + buf.size);
}

// Draws the quads with a program: a vertex buffer in pool memory, the program bound for both stages, one triangle list.
void draw_quads(App& app, NVNcommandBuffer* c, NVNprogram* prog, const std::vector<float>& verts) {
    App::Buf vb = app.make_buffer(verts.size() * sizeof(float), verts.data());
    app.api.nvnCommandBufferBindVertexBuffer(c, 0, vb.address, vb.size);
    app.api.nvnCommandBufferBindProgram(c, prog, NVN_SHADER_STAGE_VERTEX_BIT | NVN_SHADER_STAGE_FRAGMENT_BIT);
    app.api.nvnCommandBufferDrawArrays(c, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, static_cast<int>(verts.size() / 3));
}

// Prints the back end counters of this case (as deltas: the counters are process-wide) and the validation errors, and checks them.
template <class S>
bool report(App& app, const S& s0, uint64_t expected_draws, bool allow_unsupported) {
    const auto v = app.validation();
    const auto s = nxrt::nvn::backend_stats();
    const uint64_t draws = s.draws - s0.draws, skipped = s.draws_skipped - s0.draws_skipped, unresolved = s.unresolved_addresses - s0.unresolved_addresses,
                   unsupported = s.unsupported_ops - s0.unsupported_ops;
    std::printf("    validation: %llu errors, %llu warnings %s\n", static_cast<unsigned long long>(v.errors), static_cast<unsigned long long>(v.warnings),
                v.first_error.c_str());
    std::printf("    draws %llu (expected %llu) skipped %llu unresolved %llu unsupported %llu clears %llu\n", static_cast<unsigned long long>(draws),
                static_cast<unsigned long long>(expected_draws), static_cast<unsigned long long>(skipped), static_cast<unsigned long long>(unresolved),
                static_cast<unsigned long long>(unsupported), static_cast<unsigned long long>(s.clears - s0.clears));
    bool ok = v.errors == 0 && draws == expected_draws && skipped == 0 && unresolved == 0;
    if (!allow_unsupported) ok = ok && unsupported == 0;
    CHECK_EQ(v.errors, 0ull);
    CHECK_EQ(draws, expected_draws);
    CHECK_EQ(skipped, 0ull);
    CHECK_EQ(unresolved, 0ull);
    if (!allow_unsupported) CHECK_EQ(unsupported, 0ull);
    return ok;
}

// The reference picture of a case: its frames side by side. With NXRT_NVN_UPDATE_REFS=1 it is written (only when the analytic
// checks of the case passed); otherwise it is compared with the file if the file exists.
// Tolerance: mean_abs <= 0.05, max_abs <= 4, fraction_over_8 <= 0.0005.
void ref_check(const char* name, const std::vector<std::vector<uint8_t>>& frames, bool analytic_ok) {
    const int n = static_cast<int>(frames.size());
    const int w = n * kW;
    std::vector<uint8_t> strip(static_cast<size_t>(w) * kH * 4);
    for (int f = 0; f < n; ++f)
        for (int y = 0; y < kH; ++y)
            std::memcpy(&strip[(static_cast<size_t>(y) * w + static_cast<size_t>(f) * kW) * 4], &frames[f][static_cast<size_t>(y) * kW * 4],
                        static_cast<size_t>(kW) * 4);
    const std::string path = kRefDir + name + ".png";
    const char* upd = std::getenv("NXRT_NVN_UPDATE_REFS");
    if (upd && std::strcmp(upd, "1") == 0) {
        if (!analytic_ok) {
            std::printf("    reference not written: the analytic checks of %s failed\n", name);
            return;
        }
        std::filesystem::create_directories(kRefDir);
        CHECK(nxrt::gfx::save_png_rgba(path, strip.data(), static_cast<uint32_t>(w), kH));
        std::printf("    reference written: %s (%d frames side by side)\n", path.c_str(), n);
        return;
    }
    if (!std::filesystem::exists(path)) {
        std::printf("    no reference %s yet (run once with NXRT_NVN_UPDATE_REFS=1)\n", path.c_str());
        return;
    }
    std::vector<uint8_t> ref;
    uint32_t rw = 0, rh = 0;
    CHECK_MSG(nxrt::gfx::load_png_rgba(path, &ref, &rw, &rh), "reference image unreadable");
    if (ref.empty()) return;
    const nxrt::gfx::ImageDiff df = nxrt::gfx::compare_rgba(strip.data(), static_cast<uint32_t>(w), kH, ref.data(), rw, rh);
    std::printf("    vs reference: mean abs %.5f, max %d, fraction over 8 %.6f\n", df.mean_abs, df.max_abs, df.fraction_over_8);
    CHECK(df.same_size);
    CHECK(df.mean_abs <= 0.05);
    CHECK(df.max_abs <= 4);
    CHECK(df.fraction_over_8 <= 0.0005);
}

}  // namespace

// 1. Depth test. Two overlapping quads, far (z 0.75, magenta) and near (z 0.25, red), drawn in both orders, into the window with
// a D32F depth target. The overlap is columns 24..40, far-only 16..23, near-only 41..48, rows 12..36. Depth write is off except in
// the last frame. Expected colours:
//   LESS against 0.5: near passes, far fails                  -> overlap red, far-only black, near-only red (both orders)
//   GREATER against 0.5: far passes, near fails               -> overlap magenta, far-only magenta, near-only black
//   ALWAYS: nothing is tested, the last quad drawn wins       -> order dependent
//   EQUAL against 0.75: far passes exactly, near fails        -> overlap magenta, far-only magenta, near-only black
//   LESS against 1.0 with depth write on, near first: the near quad writes 0.25, so the far quad fails -> overlap red
NXTEST(depth_func_and_write_order) {
    if (!real_data_available()) { nxtest::skip("work/shader_cache not found"); return; }
    App app;
    App::Opts o;
    o.width = kW;
    o.height = kH;
    REQUIRE(app.init(o));
    Progs sh;
    REQUIRE(sh.load(app));
    auto depth = app.make_texture_2d(NVN_FORMAT_DEPTH32F, kW, kH, NVN_TEXTURE_FLAGS_COMPRESSIBLE, 1, false);
    REQUIRE(depth != nullptr);
    struct Frame {
        const char* name;
        NVNdepthFunc func;
        bool write;
        float clear;
        bool far_first;
        Rgba overlap, far_only, near_only;
    };
    const Frame frames[] = {
        {"LESS, write off, clear 0.5, far first", NVN_DEPTH_FUNC_LESS, false, 0.5f, true, KRED, KBLACK, KRED},
        {"LESS, write off, clear 0.5, near first", NVN_DEPTH_FUNC_LESS, false, 0.5f, false, KRED, KBLACK, KRED},
        {"GREATER, write off, clear 0.5, far first", NVN_DEPTH_FUNC_GREATER, false, 0.5f, true, KMAG, KMAG, KBLACK},
        {"GREATER, write off, clear 0.5, near first", NVN_DEPTH_FUNC_GREATER, false, 0.5f, false, KMAG, KMAG, KBLACK},
        {"ALWAYS, write off, clear 0.5, far first", NVN_DEPTH_FUNC_ALWAYS, false, 0.5f, true, KRED, KMAG, KRED},
        {"ALWAYS, write off, clear 0.5, near first", NVN_DEPTH_FUNC_ALWAYS, false, 0.5f, false, KMAG, KMAG, KRED},
        {"EQUAL, write off, clear 0.75, far first", NVN_DEPTH_FUNC_EQUAL, false, 0.75f, true, KMAG, KMAG, KBLACK},
        {"EQUAL, write off, clear 0.75, near first", NVN_DEPTH_FUNC_EQUAL, false, 0.75f, false, KMAG, KMAG, KBLACK},
        {"LESS, write on, clear 1.0, near first", NVN_DEPTH_FUNC_LESS, true, 1.0f, false, KRED, KMAG, KRED},
    };
    const auto s0 = nxrt::nvn::backend_stats();
    std::vector<std::vector<uint8_t>> captured;
    bool ok = true;
    for (const Frame& f : frames) {
        const int idx = app.acquire();
        REQUIRE(idx >= 0);
        auto cb = begin_frame(app, {app.window_textures[idx]->obj.get()}, depth->obj.get(), kW, kH);
        REQUIRE(cb != nullptr);
        NVNcommandBuffer* c = cb->obj.get();
        auto& api = app.api;
        const float black[4] = {0, 0, 0, 1};
        api.nvnCommandBufferClearColor(c, 0, black, NVN_CLEAR_COLOR_MASK_RGBA);
        api.nvnCommandBufferClearDepthStencil(c, f.clear, 1, 0, 0xFF);
        Gfx g;
        g.defaults(app);
        api.nvnDepthStencilStateSetDepthTestEnable(&g.ds, 1);
        api.nvnDepthStencilStateSetDepthWriteEnable(&g.ds, f.write ? 1 : 0);
        api.nvnDepthStencilStateSetDepthFunc(&g.ds, f.func);
        g.bind(app, c);
        // (far and near are macros in windows.h, hence the names)
        const auto far_quad = quad(-0.5f, 0.25f, -0.5f, 0.5f, 0.75f);
        const auto near_quad = quad(-0.25f, 0.5f, -0.5f, 0.5f, 0.25f);
        if (f.far_first) {
            draw_quads(app, c, sh.p_mag.get(), far_quad);
            draw_quads(app, c, sh.p_red.get(), near_quad);
        } else {
            draw_quads(app, c, sh.p_red.get(), near_quad);
            draw_quads(app, c, sh.p_mag.get(), far_quad);
        }
        app.end_and_submit(*cb);
        std::vector<uint8_t> px;
        REQUIRE(app.present_and_capture(idx, &px));
        captured.push_back(px);
        app.finish();
        const Rgba ov = px_at(px, 32, 24), fo = px_at(px, 19, 24), no = px_at(px, 45, 24);
        std::printf("    %s: overlap %d %d %d, far-only %d %d %d, near-only %d %d %d\n", f.name, ov.r, ov.g, ov.b, fo.r, fo.g, fo.b, no.r, no.g, no.b);
        const bool frame_ok = region_is(px, 26, 38, 14, 34, f.overlap, 0, "overlap") && region_is(px, 18, 22, 14, 34, f.far_only, 0, "far-only") &&
                              region_is(px, 43, 46, 14, 34, f.near_only, 0, "near-only") && region_is(px, 0, 10, 0, kH - 1, KBLACK, 0, "background");
        CHECK(frame_ok);
        ok = ok && frame_ok;
    }
    ok = report(app, s0, 2 * (sizeof(frames) / sizeof(frames[0])), false) && ok;
    ref_check("state_depth", captured, ok);
    app.shutdown();
}

// 2. Blending. Full-frame quad of half-transparent magenta (1,0,1,0.5) over a clear of (0.4,0.4,0.4,1), rendered into an RGBA8
// target that is read back (the presented frame forces alpha to 255, see the report). Each frame sets one blend configuration;
// the expected values are computed by hand (see each row) and compared within +-1.
NXTEST(blend_funcs_equations_and_constant) {
    if (!real_data_available()) { nxtest::skip("work/shader_cache not found"); return; }
    App app;
    App::Opts o;
    o.width = kW;
    o.height = kH;
    REQUIRE(app.init(o));
    Progs sh;
    REQUIRE(sh.load(app));
    Readback rb = make_readback(app);
    REQUIRE(rb.tex != nullptr);
    struct Frame {
        const char* name;
        bool enable;
        NVNblendFunc sr, dr, sa, da;
        NVNblendEquation eq, eqa;
        float constant[4];
        Rgba want;
    };
    // src (1, 0, 1, 0.5), dst (0.4, 0.4, 0.4, 1.0)
    const Frame frames[] = {
        {"blend off (src alpha funcs set)", false, NVN_BLEND_FUNC_SRC_ALPHA, NVN_BLEND_FUNC_ONE_MINUS_SRC_ALPHA, NVN_BLEND_FUNC_SRC_ALPHA,
         NVN_BLEND_FUNC_ONE_MINUS_SRC_ALPHA, NVN_BLEND_EQUATION_ADD, NVN_BLEND_EQUATION_ADD, {0, 0, 0, 0}, {255, 0, 255, 128}},
        // 0.5*src + 0.5*dst: (0.7, 0.2, 0.7, 0.75)
        {"SRC_ALPHA / ONE_MINUS_SRC_ALPHA, ADD", true, NVN_BLEND_FUNC_SRC_ALPHA, NVN_BLEND_FUNC_ONE_MINUS_SRC_ALPHA, NVN_BLEND_FUNC_SRC_ALPHA,
         NVN_BLEND_FUNC_ONE_MINUS_SRC_ALPHA, NVN_BLEND_EQUATION_ADD, NVN_BLEND_EQUATION_ADD, {0, 0, 0, 0}, {179, 51, 179, 191}},
        // src + dst: (1, 0.4, 1, 1) clamped
        {"ONE / ONE, ADD", true, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ONE, NVN_BLEND_EQUATION_ADD,
         NVN_BLEND_EQUATION_ADD, {0, 0, 0, 0}, {255, 102, 255, 255}},
        // src only: (1, 0, 1, 0.5)
        {"ONE / ZERO, ADD (copy of src)", true, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO,
         NVN_BLEND_EQUATION_ADD, NVN_BLEND_EQUATION_ADD, {0, 0, 0, 0}, {255, 0, 255, 128}},
        // src - dst: (0.6, -0.4 -> 0, 0.6, 0.5 - 1 -> 0)
        {"ONE / ONE, SUB (src - dst)", true, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ONE, NVN_BLEND_EQUATION_SUB,
         NVN_BLEND_EQUATION_SUB, {0, 0, 0, 0}, {153, 0, 153, 0}},
        // min(src, dst): (0.4, 0, 0.4, 0.5); the factors are ignored by MIN
        {"ONE / ONE, MIN", true, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ONE, NVN_BLEND_EQUATION_MIN,
         NVN_BLEND_EQUATION_MIN, {0, 0, 0, 0}, {102, 0, 102, 128}},
        // max(src, dst): (1, 0.4, 1, 1)
        {"ONE / ONE, MAX", true, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ONE, NVN_BLEND_EQUATION_MAX,
         NVN_BLEND_EQUATION_MAX, {0, 0, 0, 0}, {255, 102, 255, 255}},
        // constant (0.25, 0.5, 0.75, 0): R 0.25 + 0.4*0.75 = 0.55, G 0.4*0.5 = 0.2, B 0.75 + 0.4*0.25 = 0.85, A = src alpha 0.5
        {"CONSTANT_COLOR / ONE_MINUS_CONSTANT_COLOR, ADD", true, NVN_BLEND_FUNC_CONSTANT_COLOR, NVN_BLEND_FUNC_ONE_MINUS_CONSTANT_COLOR,
         NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_EQUATION_ADD, NVN_BLEND_EQUATION_ADD, {0.25f, 0.5f, 0.75f, 0.0f}, {140, 51, 217, 128}},
    };
    const auto s0 = nxrt::nvn::backend_stats();
    std::vector<std::vector<uint8_t>> captured;
    bool ok = true;
    const float clear[4] = {0.4f, 0.4f, 0.4f, 1.0f};
    for (const Frame& f : frames) {
        auto cb = begin_frame(app, {rb.tex->obj.get()}, nullptr, kW, kH);
        REQUIRE(cb != nullptr);
        NVNcommandBuffer* c = cb->obj.get();
        auto& api = app.api;
        api.nvnCommandBufferClearColor(c, 0, clear, NVN_CLEAR_COLOR_MASK_RGBA);
        Gfx g;
        g.defaults(app);
        api.nvnBlendStateSetBlendTarget(&g.bs, 0);
        api.nvnBlendStateSetBlendFunc(&g.bs, f.sr, f.dr, f.sa, f.da);
        api.nvnBlendStateSetBlendEquation(&g.bs, f.eq, f.eqa);
        api.nvnColorStateSetBlendEnable(&g.cs, 0, f.enable ? 1 : 0);
        api.nvnCommandBufferSetBlendColor(c, f.constant);
        g.bind(app, c);
        draw_quads(app, c, sh.p_half.get(), quad(-1, 1, -1, 1, 0.5f));
        api.nvnCommandBufferCopyTextureToBuffer(c, rb.tex->obj.get(), nullptr, nullptr, rb.buf.address, 0);
        app.end_and_submit(*cb);
        std::vector<uint8_t> px = texels(app, rb.buf);
        captured.push_back(px);
        const Rgba p = px_at(px, 32, 24);
        std::printf("    %s: %d %d %d %d (want %d %d %d %d)\n", f.name, p.r, p.g, p.b, p.a, f.want.r, f.want.g, f.want.b, f.want.a);
        ok = region_is(px, 0, kW - 1, 0, kH - 1, f.want, 1, f.name) && ok;
    }
    ok = report(app, s0, std::size(frames), false) && ok;
    CHECK(ok);
    ref_check("state_blend", captured, ok);
    app.shutdown();
}

// 3. Channel masks. Full-frame magenta over a clear of (0.4,0.4,0.4,0.4) = 102 in every channel, rendered into an RGBA8 target that is
// read back. Each frame enables some channels of the colour write; the channels that are off keep the clear value. Exact values.
NXTEST(channel_mask_writes_only_enabled_channels) {
    if (!real_data_available()) { nxtest::skip("work/shader_cache not found"); return; }
    App app;
    App::Opts o;
    o.width = kW;
    o.height = kH;
    REQUIRE(app.init(o));
    Progs sh;
    REQUIRE(sh.load(app));
    Readback rb = make_readback(app);
    REQUIRE(rb.tex != nullptr);
    struct Frame {
        const char* name;
        bool r, g, b, a;
        Rgba want;
    };
    const Frame frames[] = {
        {"R only", true, false, false, false, {255, 102, 102, 102}},
        {"G only", false, true, false, false, {102, 0, 102, 102}},
        {"B only", false, false, true, false, {102, 102, 255, 102}},
        {"A only", false, false, false, true, {102, 102, 102, 255}},
        {"R and B", true, false, true, false, {255, 102, 255, 102}},
        {"none", false, false, false, false, {102, 102, 102, 102}},
    };
    const auto s0 = nxrt::nvn::backend_stats();
    std::vector<std::vector<uint8_t>> captured;
    bool ok = true;
    const float clear[4] = {0.4f, 0.4f, 0.4f, 0.4f};
    for (const Frame& f : frames) {
        auto cb = begin_frame(app, {rb.tex->obj.get()}, nullptr, kW, kH);
        REQUIRE(cb != nullptr);
        NVNcommandBuffer* c = cb->obj.get();
        auto& api = app.api;
        api.nvnCommandBufferClearColor(c, 0, clear, NVN_CLEAR_COLOR_MASK_RGBA);
        Gfx g;
        g.defaults(app);
        api.nvnChannelMaskStateSetChannelMask(&g.ms, 0, f.r ? 1 : 0, f.g ? 1 : 0, f.b ? 1 : 0, f.a ? 1 : 0);
        g.bind(app, c);
        draw_quads(app, c, sh.p_mag.get(), quad(-1, 1, -1, 1, 0.5f));
        api.nvnCommandBufferCopyTextureToBuffer(c, rb.tex->obj.get(), nullptr, nullptr, rb.buf.address, 0);
        app.end_and_submit(*cb);
        std::vector<uint8_t> px = texels(app, rb.buf);
        captured.push_back(px);
        ok = region_is(px, 0, kW - 1, 0, kH - 1, f.want, 0, f.name) && ok;
    }
    ok = report(app, s0, std::size(frames), false) && ok;
    ref_check("state_channel_mask", captured, ok);
    app.shutdown();
}

// 4. Stencil. Stencil plane cleared to 0. Draw A (cols 16..40, rows 12..36) with colour writes off, stencil ALWAYS + REPLACE ref 1.
// Draw B: full-frame red with stencil EQUAL ref 1, so red only inside A. Depth test is off throughout. Depth24/Stencil8 target.
NXTEST(stencil_replace_then_equal_inside_shape) {
    if (!real_data_available()) { nxtest::skip("work/shader_cache not found"); return; }
    App app;
    App::Opts o;
    o.width = kW;
    o.height = kH;
    REQUIRE(app.init(o));
    Progs sh;
    REQUIRE(sh.load(app));
    auto stencil = app.make_texture_2d(NVN_FORMAT_DEPTH24_STENCIL8, kW, kH, NVN_TEXTURE_FLAGS_COMPRESSIBLE, 1, false);
    REQUIRE(stencil != nullptr);
    const auto s0 = nxrt::nvn::backend_stats();
    const int idx = app.acquire();
    REQUIRE(idx >= 0);
    auto cb = begin_frame(app, {app.window_textures[idx]->obj.get()}, stencil->obj.get(), kW, kH);
    REQUIRE(cb != nullptr);
    NVNcommandBuffer* c = cb->obj.get();
    auto& api = app.api;
    const float black[4] = {0, 0, 0, 1};
    api.nvnCommandBufferClearColor(c, 0, black, NVN_CLEAR_COLOR_MASK_RGBA);
    api.nvnCommandBufferClearDepthStencil(c, 1.0f, 1, 0, 0xFF);

    // draw A: stencil writes only
    Gfx a;
    a.defaults(app);
    api.nvnChannelMaskStateSetChannelMask(&a.ms, 0, 0, 0, 0, 0);
    api.nvnDepthStencilStateSetDepthTestEnable(&a.ds, 0);
    api.nvnDepthStencilStateSetStencilTestEnable(&a.ds, 1);
    api.nvnDepthStencilStateSetStencilFunc(&a.ds, NVN_FACE_FRONT_AND_BACK, NVN_STENCIL_FUNC_ALWAYS);
    api.nvnDepthStencilStateSetStencilOp(&a.ds, NVN_FACE_FRONT_AND_BACK, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_REPLACE);
    a.bind(app, c);
    api.nvnCommandBufferSetStencilRef(c, NVN_FACE_FRONT_AND_BACK, 1);
    api.nvnCommandBufferSetStencilValueMask(c, NVN_FACE_FRONT_AND_BACK, 0xFF);
    api.nvnCommandBufferSetStencilMask(c, NVN_FACE_FRONT_AND_BACK, 0xFF);
    draw_quads(app, c, sh.p_mag.get(), quad(-0.5f, 0.25f, -0.5f, 0.5f, 0.5f));

    // draw B: full frame, red where the stencil is 1
    Gfx b;
    b.defaults(app);
    api.nvnDepthStencilStateSetDepthTestEnable(&b.ds, 0);
    api.nvnDepthStencilStateSetStencilTestEnable(&b.ds, 1);
    api.nvnDepthStencilStateSetStencilFunc(&b.ds, NVN_FACE_FRONT_AND_BACK, NVN_STENCIL_FUNC_EQUAL);
    api.nvnDepthStencilStateSetStencilOp(&b.ds, NVN_FACE_FRONT_AND_BACK, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP);
    b.bind(app, c);
    draw_quads(app, c, sh.p_red.get(), quad(-1, 1, -1, 1, 0.5f));

    app.end_and_submit(*cb);
    std::vector<uint8_t> px;
    REQUIRE(app.present_and_capture(idx, &px));
    app.finish();
    const Rgba in = px_at(px, 26, 20), out = px_at(px, 50, 20);
    std::printf("    inside shape A %d %d %d, outside %d %d %d\n", in.r, in.g, in.b, out.r, out.g, out.b);
    // A's edges are at columns 16 and 40, rows 12 and 36: the one-pixel band at each edge is not checked
    bool ok = region_is(px, 18, 38, 14, 34, KRED, 0, "inside A");
    ok = region_is(px, 0, 12, 0, kH - 1, KBLACK, 0, "left of A") && ok;
    ok = region_is(px, 44, kW - 1, 0, kH - 1, KBLACK, 0, "right of A") && ok;
    ok = region_is(px, 0, kW - 1, 0, 8, KBLACK, 0, "above A") && ok;
    ok = region_is(px, 0, kW - 1, 40, kH - 1, KBLACK, 0, "below A") && ok;
    CHECK(ok);
    ok = report(app, s0, 2, false) && ok;
    ref_check("state_stencil", {px}, ok);
    app.shutdown();
}

// 5. Scissored clear. Full-frame black clear, then a clear to red with the scissor at (16,12) size 16x12: only that rectangle is red.
NXTEST(clear_color_only_inside_scissor) {
    App app;
    App::Opts o;
    o.width = kW;
    o.height = kH;
    REQUIRE(app.init(o));
    const auto s0 = nxrt::nvn::backend_stats();
    const int idx = app.acquire();
    REQUIRE(idx >= 0);
    auto cb = begin_frame(app, {app.window_textures[idx]->obj.get()}, nullptr, kW, kH);
    REQUIRE(cb != nullptr);
    NVNcommandBuffer* c = cb->obj.get();
    auto& api = app.api;
    const float black[4] = {0, 0, 0, 1}, red[4] = {1, 0, 0, 1};
    api.nvnCommandBufferClearColor(c, 0, black, NVN_CLEAR_COLOR_MASK_RGBA);
    api.nvnCommandBufferSetScissor(c, 16, 12, 16, 12);
    api.nvnCommandBufferClearColor(c, 0, red, NVN_CLEAR_COLOR_MASK_RGBA);
    app.end_and_submit(*cb);
    std::vector<uint8_t> px;
    REQUIRE(app.present_and_capture(idx, &px));
    bool ok = region_is(px, 16, 31, 12, 23, KRED, 0, "scissor rectangle");
    ok = region_is(px, 0, 15, 0, kH - 1, KBLACK, 0, "left of scissor") && ok;
    ok = region_is(px, 32, kW - 1, 0, kH - 1, KBLACK, 0, "right of scissor") && ok;
    ok = region_is(px, 0, kW - 1, 0, 11, KBLACK, 0, "above scissor") && ok;
    ok = region_is(px, 0, kW - 1, 24, kH - 1, KBLACK, 0, "below scissor") && ok;
    CHECK(ok);
    ok = report(app, s0, 0, false) && ok;
    ref_check("state_clear_scissor", {px}, ok);
    app.shutdown();
}

// 6. Polygon offset. A magenta full-frame quad at z 0.5 writes depth (LEQUAL). A red full-frame quad at z 0.5001 (behind it by
// 1e-4, so it fails LEQUAL) is drawn second. Without the offset the magenta stays. With FILL offset (units -100000, pulling the
// fragments towards the viewer) the red passes. The two draws bind different polygon states so the offset is only on the second.
NXTEST(polygon_offset_lets_second_quad_win_lequal) {
    if (!real_data_available()) { nxtest::skip("work/shader_cache not found"); return; }
    App app;
    App::Opts o;
    o.width = kW;
    o.height = kH;
    REQUIRE(app.init(o));
    Progs sh;
    REQUIRE(sh.load(app));
    auto depth = app.make_texture_2d(NVN_FORMAT_DEPTH32F, kW, kH, NVN_TEXTURE_FLAGS_COMPRESSIBLE, 1, false);
    REQUIRE(depth != nullptr);
    const auto s0 = nxrt::nvn::backend_stats();
    std::vector<std::vector<uint8_t>> captured;
    bool ok = true;
    for (const bool offset : {false, true}) {
        const int idx = app.acquire();
        REQUIRE(idx >= 0);
        auto cb = begin_frame(app, {app.window_textures[idx]->obj.get()}, depth->obj.get(), kW, kH);
        REQUIRE(cb != nullptr);
        NVNcommandBuffer* c = cb->obj.get();
        auto& api = app.api;
        const float black[4] = {0, 0, 0, 1};
        api.nvnCommandBufferClearColor(c, 0, black, NVN_CLEAR_COLOR_MASK_RGBA);
        api.nvnCommandBufferClearDepthStencil(c, 1.0f, 1, 0, 0xFF);
        Gfx g;
        g.defaults(app);
        api.nvnDepthStencilStateSetDepthTestEnable(&g.ds, 1);
        api.nvnDepthStencilStateSetDepthWriteEnable(&g.ds, 1);
        api.nvnDepthStencilStateSetDepthFunc(&g.ds, NVN_DEPTH_FUNC_LEQUAL);
        NVNpolygonState offset_ps;
        api.nvnPolygonStateSetDefaults(&offset_ps);
        api.nvnPolygonStateSetCullFace(&offset_ps, NVN_FACE_NONE);
        api.nvnPolygonStateSetPolygonOffsetEnables(&offset_ps, NVN_POLYGON_OFFSET_ENABLE_FILL);
        g.bind(app, c);
        api.nvnCommandBufferSetPolygonOffsetClamp(c, 0.0f, -100000.0f, 0.0f);
        draw_quads(app, c, sh.p_mag.get(), quad(-1, 1, -1, 1, 0.5f));
        if (offset) api.nvnCommandBufferBindPolygonState(c, &offset_ps);
        draw_quads(app, c, sh.p_red.get(), quad(-1, 1, -1, 1, 0.5001f));
        app.end_and_submit(*cb);
        std::vector<uint8_t> px;
        REQUIRE(app.present_and_capture(idx, &px));
        captured.push_back(px);
        app.finish();
        const Rgba want = offset ? KRED : KMAG;
        const Rgba p = px_at(px, 32, 24);
        std::printf("    %s: pixel (32,24) %d %d %d, want %d %d %d\n", offset ? "FILL offset on" : "no offset", p.r, p.g, p.b, want.r, want.g, want.b);
        ok = region_is(px, 0, kW - 1, 0, kH - 1, want, 0, offset ? "with offset" : "without offset") && ok;
    }
    ok = report(app, s0, 4, false) && ok;
    ref_check("state_polygon_offset", captured, ok);
    app.shutdown();
}

// 7. Clear of target 1 of two colour targets. Target 0 (the window) is cleared to black, target 1 to blue; then target 1 is copied
// to a buffer and read back. Expected: window all black, target 1 all blue.
NXTEST(clear_color_of_second_render_target) {
    App app;
    App::Opts o;
    o.width = kW;
    o.height = kH;
    REQUIRE(app.init(o));
    auto t1 = app.make_texture_2d(NVN_FORMAT_RGBA8, kW, kH, 0, 1, false);
    REQUIRE(t1 != nullptr);
    App::Buf buf = app.make_buffer(static_cast<size_t>(kW) * kH * 4);
    const auto s0 = nxrt::nvn::backend_stats();
    const int idx = app.acquire();
    REQUIRE(idx >= 0);
    auto cb = begin_frame(app, {app.window_textures[idx]->obj.get(), t1->obj.get()}, nullptr, kW, kH);
    REQUIRE(cb != nullptr);
    NVNcommandBuffer* c = cb->obj.get();
    auto& api = app.api;
    const float black[4] = {0, 0, 0, 1}, blue[4] = {0, 0, 1, 1};
    api.nvnCommandBufferClearColor(c, 0, black, NVN_CLEAR_COLOR_MASK_RGBA);
    api.nvnCommandBufferClearColor(c, 1, blue, NVN_CLEAR_COLOR_MASK_RGBA);
    api.nvnCommandBufferCopyTextureToBuffer(c, t1->obj.get(), nullptr, nullptr, buf.address, 0);
    app.end_and_submit(*cb);
    std::vector<uint8_t> window;
    REQUIRE(app.present_and_capture(idx, &window));
    std::vector<uint8_t> target1 = texels(app, buf);
    bool ok = region_is(window, 0, kW - 1, 0, kH - 1, KBLACK, 0, "target 0 (window)");
    ok = region_is(target1, 0, kW - 1, 0, kH - 1, {0, 0, 255, 255}, 0, "target 1 (read back)") && ok;
    CHECK(ok);
    ok = report(app, s0, 0, false) && ok;
    ref_check("state_clear_mrt", {window, target1}, ok);
    app.shutdown();
}

// 8. Integer clears on an R32UI target (16x8). Full clear with ClearColorui 0x01020304, then ClearColori -2 inside the scissor
// (4,2) size 8x4. Read back with a copy to a buffer. Expected words: 0xFFFFFFFE inside the scissor, 0x01020304 elsewhere.
NXTEST(clear_color_integer_r32ui) {
    App app;
    App::Opts o;
    o.width = kW;
    o.height = kH;
    REQUIRE(app.init(o));
    constexpr int iw = 16, ih = 8;
    auto tex = app.make_texture_2d(NVN_FORMAT_R32UI, iw, ih, 0, 1, false);
    REQUIRE(tex != nullptr);
    App::Buf buf = app.make_buffer(static_cast<size_t>(iw) * ih * 4);
    std::memset(buf.ptr, 0, buf.size);
    const auto s0 = nxrt::nvn::backend_stats();
    auto cb = begin_frame(app, {tex->obj.get()}, nullptr, iw, ih);
    REQUIRE(cb != nullptr);
    NVNcommandBuffer* c = cb->obj.get();
    auto& api = app.api;
    const uint32_t full[4] = {0x01020304u, 0, 0, 0};
    const int neg[4] = {-2, 0, 0, 0};
    api.nvnCommandBufferClearColorui(c, 0, full, NVN_CLEAR_COLOR_MASK_RGBA);
    api.nvnCommandBufferSetScissor(c, 4, 2, 8, 4);
    api.nvnCommandBufferClearColori(c, 0, neg, NVN_CLEAR_COLOR_MASK_RGBA);
    api.nvnCommandBufferCopyTextureToBuffer(c, tex->obj.get(), nullptr, nullptr, buf.address, 0);
    app.end_and_submit(*cb);
    app.finish();
    const uint32_t* words = reinterpret_cast<const uint32_t*>(buf.ptr);
    int bad = 0;
    uint32_t first = 0;
    for (int y = 0; y < ih; ++y)
        for (int x = 0; x < iw; ++x) {
            const bool inside = x >= 4 && x <= 11 && y >= 2 && y <= 5;
            const uint32_t want = inside ? 0xFFFFFFFEu : 0x01020304u;
            const uint32_t got = words[y * iw + x];
            if (got != want) {
                if (!bad) first = got;
                ++bad;
            }
        }
    std::printf("    texel (6,3) %08x, texel (1,1) %08x, texels wrong %d (first %08x)\n", words[3 * iw + 6], words[1 * iw + 1], bad, first);
    CHECK_MSG(bad == 0, "R32UI texels differ from the integer clears");
    report(app, s0, 0, false);
    app.shutdown();
}

// 9. Logic op. White clear, blending off, logic op XOR, full-frame magenta (1,0,1,1), read back from an RGBA8 target. Expected per
// channel: 255 ^ 255 = 0, 255 ^ 0 = 255, 255 ^ 255 = 0, and alpha 255 ^ 255 = 0, so (0, 255, 0, 0). If the device has no logicOp
// feature the back end runs COPY and the target holds the magenta (255, 0, 255, 255) instead.
NXTEST(logic_op_xor_on_white) {
    if (!real_data_available()) { nxtest::skip("work/shader_cache not found"); return; }
    App app;
    App::Opts o;
    o.width = kW;
    o.height = kH;
    REQUIRE(app.init(o));
    Progs sh;
    REQUIRE(sh.load(app));
    Readback rb = make_readback(app);
    REQUIRE(rb.tex != nullptr);
    const auto s0 = nxrt::nvn::backend_stats();
    auto cb = begin_frame(app, {rb.tex->obj.get()}, nullptr, kW, kH);
    REQUIRE(cb != nullptr);
    NVNcommandBuffer* c = cb->obj.get();
    auto& api = app.api;
    const float white[4] = {1, 1, 1, 1};
    api.nvnCommandBufferClearColor(c, 0, white, NVN_CLEAR_COLOR_MASK_RGBA);
    Gfx g;
    g.defaults(app);
    api.nvnColorStateSetLogicOp(&g.cs, NVN_LOGIC_OP_XOR);
    g.bind(app, c);
    draw_quads(app, c, sh.p_mag.get(), quad(-1, 1, -1, 1, 0.5f));
    api.nvnCommandBufferCopyTextureToBuffer(c, rb.tex->obj.get(), nullptr, nullptr, rb.buf.address, 0);
    app.end_and_submit(*cb);
    std::vector<uint8_t> px = texels(app, rb.buf);
    const Rgba p = px_at(px, 32, 24);
    std::printf("    pixel %d %d %d %d (want 0 255 0 0; copy would be 255 0 255 255)\n", p.r, p.g, p.b, p.a);
    bool ok = region_is(px, 0, kW - 1, 0, kH - 1, {0, 255, 0, 0}, 0, "XOR result");
    CHECK(ok);
    ok = report(app, s0, 1, false) && ok;
    ref_check("state_logic_op", {px}, ok);
    app.shutdown();
}

// 10. Partial clear mask (KNOWN GAP: the back end clears all four channels, see do_clear_color in nvn_replay.cpp). Background
// black (0,0,0,1), then ClearColor of (1,1,1,0) with mask R|B, read back from an RGBA8 target. Correct result: (255, 0, 255, 255).
// This assertion is the correct one; the gap is closed (masked clears are a draw with a colour write mask, nvn_mask_clear.cpp).
NXTEST(clear_color_partial_mask_known_gap) {
    App app;
    App::Opts o;
    o.width = kW;
    o.height = kH;
    REQUIRE(app.init(o));
    Readback rb = make_readback(app);
    REQUIRE(rb.tex != nullptr);
    const auto s0 = nxrt::nvn::backend_stats();
    auto cb = begin_frame(app, {rb.tex->obj.get()}, nullptr, kW, kH);
    REQUIRE(cb != nullptr);
    NVNcommandBuffer* c = cb->obj.get();
    auto& api = app.api;
    const float black[4] = {0, 0, 0, 1}, value[4] = {1, 1, 1, 0};
    api.nvnCommandBufferClearColor(c, 0, black, NVN_CLEAR_COLOR_MASK_RGBA);
    api.nvnCommandBufferClearColor(c, 0, value, 0x1 | 0x4);   // R | B
    api.nvnCommandBufferCopyTextureToBuffer(c, rb.tex->obj.get(), nullptr, nullptr, rb.buf.address, 0);
    app.end_and_submit(*cb);
    std::vector<uint8_t> px = texels(app, rb.buf);
    const Rgba p = px_at(px, 32, 24);
    std::printf("    pixel %d %d %d %d (correct 255 0 255 255)\n", p.r, p.g, p.b, p.a);
    bool ok = region_is(px, 0, kW - 1, 0, kH - 1, {255, 0, 255, 255}, 0, "partial mask clear");
    CHECK(ok);
    // this scene is about the unsupported partial mask, so unsupported_ops is allowed to be 1 here
    ok = report(app, s0, 0, true) && ok;
    app.shutdown();
}

namespace {

// One clear of the depth/stencil target: the depth value and whether depth is cleared, the stencil value and the stencil write mask.
struct DsClear {
    float depth;
    bool depth_mask;
    int stencil, stencil_mask;
};

// One check frame of the partial stencil cases: the window is cleared to black, the case's clears are recorded, then one full-frame
// quad (red or magenta) is drawn with the given depth test and stencil test. Depth writes and stencil writes are off, colour writes on.
struct PartialCheck {
    const char* name;
    bool depth_test;
    float z;
    bool stencil_test;
    int ref, compare_mask;
    bool red;
    Rgba want;
};

bool partial_stencil_frame(App& app, Progs& sh, NVNtexture* ds, const std::vector<DsClear>& clears, const PartialCheck& ck, std::vector<uint8_t>* px) {
    const int idx = app.acquire();
    if (idx < 0) return false;
    auto cb = begin_frame(app, {app.window_textures[idx]->obj.get()}, ds, kW, kH);
    if (!cb) return false;
    NVNcommandBuffer* c = cb->obj.get();
    auto& api = app.api;
    const float black[4] = {0, 0, 0, 1};
    api.nvnCommandBufferClearColor(c, 0, black, NVN_CLEAR_COLOR_MASK_RGBA);
    for (const DsClear& k : clears) api.nvnCommandBufferClearDepthStencil(c, k.depth, k.depth_mask ? 1 : 0, k.stencil, k.stencil_mask);
    Gfx g;
    g.defaults(app);
    api.nvnDepthStencilStateSetDepthTestEnable(&g.ds, ck.depth_test ? 1 : 0);
    api.nvnDepthStencilStateSetDepthWriteEnable(&g.ds, 0);
    api.nvnDepthStencilStateSetDepthFunc(&g.ds, NVN_DEPTH_FUNC_LESS);
    api.nvnDepthStencilStateSetStencilTestEnable(&g.ds, ck.stencil_test ? 1 : 0);
    api.nvnDepthStencilStateSetStencilFunc(&g.ds, NVN_FACE_FRONT_AND_BACK, NVN_STENCIL_FUNC_EQUAL);
    api.nvnDepthStencilStateSetStencilOp(&g.ds, NVN_FACE_FRONT_AND_BACK, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP);
    g.bind(app, c);
    api.nvnCommandBufferSetStencilRef(c, NVN_FACE_FRONT_AND_BACK, ck.ref);
    api.nvnCommandBufferSetStencilValueMask(c, NVN_FACE_FRONT_AND_BACK, ck.compare_mask);
    api.nvnCommandBufferSetStencilMask(c, NVN_FACE_FRONT_AND_BACK, 0);
    draw_quads(app, c, ck.red ? sh.p_red.get() : sh.p_mag.get(), quad(-1, 1, -1, 1, ck.z));
    app.end_and_submit(*cb);
    if (!app.present_and_capture(idx, px)) return false;
    app.finish();
    return true;
}

}  // namespace

// 11. Partial stencil mask clear. Stencil cleared to 0xF0 everywhere (full mask), then ClearDepthStencil with depth not cleared,
// stencil value 0x0F and stencil mask 0x03: bits 0..1 become 0x0F & 0x03 = 0x03, the other bits keep 0xF0, so the stencil is 0xF3.
// Each check is one frame: a stencil EQUAL test against a ref and compare mask, depth test off. Depth/stencil target Depth24/Stencil8.
NXTEST(clear_depth_stencil_partial_stencil_mask) {
    if (!real_data_available()) { nxtest::skip("work/shader_cache not found"); return; }
    App app;
    App::Opts o;
    o.width = kW;
    o.height = kH;
    REQUIRE(app.init(o));
    Progs sh;
    REQUIRE(sh.load(app));
    auto stencil = app.make_texture_2d(NVN_FORMAT_DEPTH24_STENCIL8, kW, kH, NVN_TEXTURE_FLAGS_COMPRESSIBLE, 1, false);
    REQUIRE(stencil != nullptr);
    const std::vector<DsClear> clears = {{1.0f, true, 0xF0, 0xFF}, {1.0f, false, 0x0F, 0x03}};
    const PartialCheck checks[] = {
        {"stencil EQUAL 0xF3 under mask 0xFF: red must cover the frame", false, 0.5f, true, 0xF3, 0xFF, true, KRED},
        {"stencil EQUAL 0xF0 under mask 0xFF: magenta must not appear", false, 0.5f, true, 0xF0, 0xFF, false, KBLACK},
        {"stencil EQUAL 0x03 under mask 0x03: magenta must cover the frame", false, 0.5f, true, 0x03, 0x03, false, KMAG},
    };
    const auto s0 = nxrt::nvn::backend_stats();
    bool ok = true;
    for (const PartialCheck& ck : checks) {
        std::vector<uint8_t> px;
        REQUIRE(partial_stencil_frame(app, sh, stencil->obj.get(), clears, ck, &px));
        const Rgba p = px_at(px, 32, 24);
        std::printf("    %s: pixel %d %d %d (want %d %d %d)\n", ck.name, p.r, p.g, p.b, ck.want.r, ck.want.g, ck.want.b);
        ok = region_is(px, 0, kW - 1, 0, kH - 1, ck.want, 0, ck.name) && ok;
    }
    CHECK(ok);
    ok = report(app, s0, std::size(checks), false) && ok;
    app.shutdown();
}

// 12. Partial stencil mask clear with depth. Stencil cleared to 0xF0 (full mask), then ClearDepthStencil with depth 0.25 and depth
// cleared, stencil value 0x0F and stencil mask 0x0C. Stencil expected (0xF0 & ~0x0C) | (0x0F & 0x0C) = 0xFC. Depth expected 0.25.
// Checks: quad at z 0.5 with depth LESS is rejected everywhere (black), quad at z 0.1 passes everywhere (red); stencil EQUAL 0xFC under
// mask 0xFF covers the frame, EQUAL 0xF0 under mask 0xFF does not appear, and mask 0x0C ref 0x0C covers the frame.
NXTEST(clear_depth_stencil_partial_stencil_mask_with_depth) {
    if (!real_data_available()) { nxtest::skip("work/shader_cache not found"); return; }
    App app;
    App::Opts o;
    o.width = kW;
    o.height = kH;
    REQUIRE(app.init(o));
    Progs sh;
    REQUIRE(sh.load(app));
    auto stencil = app.make_texture_2d(NVN_FORMAT_DEPTH24_STENCIL8, kW, kH, NVN_TEXTURE_FLAGS_COMPRESSIBLE, 1, false);
    REQUIRE(stencil != nullptr);
    const std::vector<DsClear> clears = {{1.0f, true, 0xF0, 0xFF}, {0.25f, true, 0x0F, 0x0C}};
    const PartialCheck checks[] = {
        {"depth LESS against 0.25, quad at z 0.5 magenta: must be rejected", true, 0.5f, false, 0, 0, false, KBLACK},
        {"depth LESS against 0.25, quad at z 0.1 red: must pass", true, 0.1f, false, 0, 0, true, KRED},
        {"stencil EQUAL 0xFC under mask 0xFF: red must cover the frame", false, 0.5f, true, 0xFC, 0xFF, true, KRED},
        {"stencil EQUAL 0xF0 under mask 0xFF: magenta must not appear", false, 0.5f, true, 0xF0, 0xFF, false, KBLACK},
        {"stencil EQUAL 0x0C under mask 0x0C: magenta must cover the frame", false, 0.5f, true, 0x0C, 0x0C, false, KMAG},
    };
    const auto s0 = nxrt::nvn::backend_stats();
    bool ok = true;
    for (const PartialCheck& ck : checks) {
        std::vector<uint8_t> px;
        REQUIRE(partial_stencil_frame(app, sh, stencil->obj.get(), clears, ck, &px));
        const Rgba p = px_at(px, 32, 24);
        std::printf("    %s: pixel %d %d %d (want %d %d %d)\n", ck.name, p.r, p.g, p.b, ck.want.r, ck.want.g, ck.want.b);
        ok = region_is(px, 0, kW - 1, 0, kH - 1, ck.want, 0, ck.name) && ok;
    }
    CHECK(ok);
    ok = report(app, s0, std::size(checks), false) && ok;
    app.shutdown();
}

// 6. Occlusion counter (NVN_COUNTER_TYPE_SAMPLES_PASSED). Colour target from make_readback, D32F depth, depth test LESS throughout.
// Depth write is on only for the occluder A (z 0.25, columns 16..31, rows 12..35). Each ResetCounter / ReportCounter pair brackets the
// draws of one slot in a 16-byte report: the sample count is the first 8 bytes, a timestamp the second 8 bytes. Expected counts:
//   slot 0: B (z 0.75, columns 19..28, fully behind A, depth write off)                   -> 0
//   slot 1: C (z 0.5, columns 40..55, rows 12..35, free screen, depth write off)         -> 384 (+-8 accepted)
//   slot 2: D (z 0.75, columns 24..39, rows 12..35, columns 24..31 behind A)             -> 8 * 24 = 192
//   slot 3: ResetCounter and ReportCounter with nothing drawn in between                  -> 0
NXTEST(occlusion_counter_counts_passing_samples) {
    if (!real_data_available()) { nxtest::skip("work/shader_cache not found"); return; }
    App app;
    App::Opts o;
    o.width = kW;
    o.height = kH;
    REQUIRE(app.init(o));
    Progs sh;
    REQUIRE(sh.load(app));
    Readback rb = make_readback(app);
    REQUIRE(rb.tex != nullptr);
    auto depth = app.make_texture_2d(NVN_FORMAT_DEPTH32F, kW, kH, NVN_TEXTURE_FLAGS_COMPRESSIBLE, 1, false);
    REQUIRE(depth != nullptr);
    App::Buf rep = app.make_buffer(64);   // four 16-byte reports, slot i at rep.address + 16 * i
    std::memset(rep.ptr, 0, 64);
    auto cb = begin_frame(app, {rb.tex->obj.get()}, depth->obj.get(), kW, kH);
    REQUIRE(cb != nullptr);
    NVNcommandBuffer* c = cb->obj.get();
    auto& api = app.api;
    const float black[4] = {0, 0, 0, 1};
    api.nvnCommandBufferClearColor(c, 0, black, NVN_CLEAR_COLOR_MASK_RGBA);
    api.nvnCommandBufferClearDepthStencil(c, 1.0f, 1, 0, 0xFF);
    const auto s0 = nxrt::nvn::backend_stats();

    // Two state objects, both kept alive for the whole frame: ga writes depth (occluder A), gr does not (the counted draws).
    Gfx ga, gr;
    ga.defaults(app);
    api.nvnDepthStencilStateSetDepthTestEnable(&ga.ds, 1);
    api.nvnDepthStencilStateSetDepthWriteEnable(&ga.ds, 1);
    api.nvnDepthStencilStateSetDepthFunc(&ga.ds, NVN_DEPTH_FUNC_LESS);
    gr.defaults(app);
    api.nvnDepthStencilStateSetDepthTestEnable(&gr.ds, 1);
    api.nvnDepthStencilStateSetDepthWriteEnable(&gr.ds, 0);
    api.nvnDepthStencilStateSetDepthFunc(&gr.ds, NVN_DEPTH_FUNC_LESS);

    // occluder A: depth write on, not counted
    ga.bind(app, c);
    draw_quads(app, c, sh.p_mag.get(), quad(-0.5f, 0.0f, -0.5f, 0.5f, 0.25f));

    gr.bind(app, c);
    // slot 0: B, fully behind A
    api.nvnCommandBufferResetCounter(c, NVN_COUNTER_TYPE_SAMPLES_PASSED);
    draw_quads(app, c, sh.p_red.get(), quad(-0.4f, -0.1f, -0.4f, 0.4f, 0.75f));
    api.nvnCommandBufferReportCounter(c, NVN_COUNTER_TYPE_SAMPLES_PASSED, rep.address + 0);
    // slot 1: C, free screen area
    api.nvnCommandBufferResetCounter(c, NVN_COUNTER_TYPE_SAMPLES_PASSED);
    draw_quads(app, c, sh.p_mag.get(), quad(0.25f, 0.75f, -0.5f, 0.5f, 0.5f));
    api.nvnCommandBufferReportCounter(c, NVN_COUNTER_TYPE_SAMPLES_PASSED, rep.address + 16);
    // slot 2: D, half hidden by A
    api.nvnCommandBufferResetCounter(c, NVN_COUNTER_TYPE_SAMPLES_PASSED);
    draw_quads(app, c, sh.p_red.get(), quad(-0.25f, 0.25f, -0.5f, 0.5f, 0.75f));
    api.nvnCommandBufferReportCounter(c, NVN_COUNTER_TYPE_SAMPLES_PASSED, rep.address + 32);
    // slot 3: nothing drawn
    api.nvnCommandBufferResetCounter(c, NVN_COUNTER_TYPE_SAMPLES_PASSED);
    api.nvnCommandBufferReportCounter(c, NVN_COUNTER_TYPE_SAMPLES_PASSED, rep.address + 48);

    app.end_and_submit(*cb);
    app.finish();
    uint64_t cnt[4] = {}, ts[4] = {};
    for (int i = 0; i < 4; ++i) {
        std::memcpy(&cnt[i], rep.ptr + 16 * i, 8);
        std::memcpy(&ts[i], rep.ptr + 16 * i + 8, 8);
        std::printf("    slot %d: samples passed %llu, timestamp %llu\n", i, static_cast<unsigned long long>(cnt[i]),
                    static_cast<unsigned long long>(ts[i]));
    }
    // slot 1 is accepted within 384 +- 8 (rasterisation rules at the edges); the other slots are exact
    if (cnt[1] != 384ull) std::printf("    slot 1 is not exactly 384: %llu\n", static_cast<unsigned long long>(cnt[1]));
    CHECK_EQ(cnt[0], 0ull);
    CHECK(cnt[1] >= 376ull && cnt[1] <= 392ull);
    CHECK_EQ(cnt[2], 192ull);
    CHECK_EQ(cnt[3], 0ull);
    for (int i = 0; i < 4; ++i) CHECK(ts[i] != 0ull);
    report(app, s0, 4, false);
    app.shutdown();
}
