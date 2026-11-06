// SPDX-License-Identifier: GPL-3.0-or-later
// NVN scene oracles: textures (S4d). Every scene draws a 64x48 frame with REAL translated shaders of Metroid Prime
// Remastered: vertex 2bf6629719b97303 (gl_Position = attribute 0, varying 0 = attribute 1, the texture coordinate) and fragment
// 2192ff78120fa063 (out.rgb = texture(sampler, uv).rgb, alpha 1, no uniform reads). Textures are tiled with tegra::tile into the
// pool memory the way a game writes pre-tiled assets; the real samples of work/survey/textures/samples are copied as they sit
// in the game's buffers. Expected pixels come from the CPU (texel positions, a reference sampler, a BC1/BC7/RGBA8 decoder, the
// astc2bc7 ASTC decode and a PSNR bound for the compressed cases). Texture coordinates use the NVN convention: v = 0 is the first
// texel row, and the top row of the frame (window row 0) shows the quad's top edge.
#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>

#include <nxrt/config.h>
#include <nxrt/game.h>

#include "astc2bc7.h"
#ifdef NXRT_HAVE_BC7_REF
#include "bc7decomp.h"
#endif
#include "tegra_tiling.h"
#include "nvn_app.h"
#include "nvn_texture_layout.h"
NXTEST_GROUP(gfx_nvn_scene_texture)

using namespace nvnapp;

namespace {

constexpr uint32_t kW = 64, kH = 48;
const std::string kRefDirStr = nxtest::project_path("work/nvn_refs");
const std::string kSamplesStr = nxtest::project_path("work/survey/textures/samples/");
const char* kRefDir = kRefDirStr.c_str();
const char* kSamples = kSamplesStr.c_str();

// A new, empty BC7 cache folder for one test (the [nvn] bc7_cache environment variable points at it for the test's lifetime): the back end then
// transcodes with this build's encoder instead of picking up an entry another encoder revision left in the working directory.
struct FreshBc7Cache {
    std::filesystem::path dir;
    std::string old;
    bool had_old = false;
    FreshBc7Cache() {
        static int n = 0;
        dir = std::filesystem::temp_directory_path() /
              ("nxrt_scene_tex_cache_" + std::to_string(::GetCurrentProcessId()) + "_" + std::to_string(++n));
        std::filesystem::create_directories(dir);
        if (const char* e = std::getenv("NXRT_NVN_BC7_CACHE")) { old = e; had_old = true; }
        _putenv_s("NXRT_NVN_BC7_CACHE", dir.string().c_str());
    }
    ~FreshBc7Cache() {
        _putenv_s("NXRT_NVN_BC7_CACHE", had_old ? old.c_str() : "");
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
};

struct Rgb { int r, g, b; };
struct Rgbf { double r, g, b; };

Rgb at(const std::vector<uint8_t>& px, uint32_t x, uint32_t y) {
    const uint8_t* p = &px[(static_cast<size_t>(y) * kW + x) * 4];
    return {p[0], p[1], p[2]};
}
int rnd(double v) { return static_cast<int>(std::lround(v)); }

// ---- frame comparison --------------------------------------------------------------------------------------------------------
struct Cmp {
    int n = 0, bad = 0, worst = 0;
    int fx = -1, fy = -1;   // first pixel outside the tolerance
    Rgb got{0, 0, 0};
    Rgbf want{0, 0, 0};
};

// Compares the pixels of a rectangle with expect(x, y) (channel values 0..255) within tol per channel.
template <class F>
Cmp compare_rect(const std::vector<uint8_t>& px, uint32_t x0, uint32_t y0, uint32_t w, uint32_t h, F&& expect, int tol) {
    Cmp c;
    for (uint32_t y = y0; y < y0 + h; ++y)
        for (uint32_t x = x0; x < x0 + w; ++x) {
            const Rgb g = at(px, x, y);
            const Rgbf e = expect(x, y);
            const int d = std::max({std::abs(g.r - rnd(e.r)), std::abs(g.g - rnd(e.g)), std::abs(g.b - rnd(e.b))});
            c.n++;
            c.worst = std::max(c.worst, d);
            if (d > tol) {
                if (c.bad == 0) {
                    c.fx = static_cast<int>(x);
                    c.fy = static_cast<int>(y);
                    c.got = g;
                    c.want = e;
                }
                c.bad++;
            }
        }
    return c;
}
// PSNR over the rgb channels of a rectangle against the unrounded expectation (the bound for compressed formats).
struct Psnr {
    double db = 0;
    int worst = 0;   // largest channel difference after rounding
};
template <class F>
Psnr psnr_rect(const std::vector<uint8_t>& px, uint32_t x0, uint32_t y0, uint32_t w, uint32_t h, F&& expect) {
    double se = 0;
    int worst = 0;
    size_t n = 0;
    for (uint32_t y = y0; y < y0 + h; ++y)
        for (uint32_t x = x0; x < x0 + w; ++x) {
            const Rgb g = at(px, x, y);
            const Rgbf e = expect(x, y);
            const double d[3] = {g.r - e.r, g.g - e.g, g.b - e.b};
            for (double v : d) se += v * v;
            worst = std::max({worst, std::abs(g.r - rnd(e.r)), std::abs(g.g - rnd(e.g)), std::abs(g.b - rnd(e.b))});
            n += 3;
        }
    const double mse = se / static_cast<double>(n);
    return {mse == 0 ? 999.0 : 10.0 * std::log10(255.0 * 255.0 / mse), worst};
}

void report(const char* what, const Cmp& c, int tol) {
    std::printf("    %s: %d of %d pixels off by more than %d (worst channel difference %d)\n", what, c.bad, c.n, tol, c.worst);
    if (c.bad > 0)
        std::printf("      first off pixel (%d,%d): got %d %d %d, expected %.2f %.2f %.2f\n", c.fx, c.fy, c.got.r, c.got.g, c.got.b, c.want.r, c.want.g,
                    c.want.b);
}

// Reference-image oracle (guide): written with NXRT_NVN_UPDATE_REFS=1, compared afterwards.
void reference_check(const char* name, const std::vector<uint8_t>& px) {
    const std::string path = std::string(kRefDir) + "/" + name + ".png";
    if (const char* e = std::getenv("NXRT_NVN_UPDATE_REFS"); e && *e == '1') {
        std::filesystem::create_directories(kRefDir);
        CHECK(nxrt::gfx::save_png_rgba(path, px.data(), kW, kH));
        std::printf("    reference written: %s\n", path.c_str());
        return;
    }
    if (!std::filesystem::exists(path)) {
        std::printf("    no reference yet for %s (run once with NXRT_NVN_UPDATE_REFS=1)\n", name);
        return;
    }
    std::vector<uint8_t> ref;
    uint32_t rw = 0, rh = 0;
    if (!nxrt::gfx::load_png_rgba(path, &ref, &rw, &rh)) {
        CHECK_MSG(false, "reference image unreadable");
        return;
    }
    const nxrt::gfx::ImageDiff df = nxrt::gfx::compare_rgba(px.data(), kW, kH, ref.data(), rw, rh);
    std::printf("    vs reference %s: mean abs %.5f, max %d, fraction over 8 %.6f\n", name, df.mean_abs, df.max_abs, df.fraction_over_8);
    CHECK(df.same_size);
    CHECK(df.mean_abs <= 0.05);
    CHECK(df.max_abs <= 4);
    CHECK(df.fraction_over_8 <= 0.0005);
}

void check_clean(App& app) {
    const auto v = app.validation();
    std::printf("    validation: %llu errors, %llu warnings %s\n", static_cast<unsigned long long>(v.errors), static_cast<unsigned long long>(v.warnings), v.first_error.c_str());
    CHECK_EQ(v.errors, 0ull);
    const auto st = nxrt::nvn::backend_stats();
    std::printf("    back end: draws %llu skipped %llu unresolved %llu unsupported %llu\n", static_cast<unsigned long long>(st.draws),
                static_cast<unsigned long long>(st.draws_skipped), static_cast<unsigned long long>(st.unresolved_addresses),
                static_cast<unsigned long long>(st.unsupported_ops));
    CHECK_EQ(st.draws_skipped, 0ull);
    CHECK_EQ(st.unresolved_addresses, 0ull);
    CHECK_EQ(st.unsupported_ops, 0ull);
}

// ---- shaders and geometry ----------------------------------------------------------------------------------------------------
bool load_quad_shaders(App::ShaderBlob* vs, App::ShaderBlob* fs) {
    return App::load_shader("2bf6629719b97303", vs) && App::load_shader("2192ff78120fa063", fs);
}

struct Vtx {
    float p[3];   // attribute 0: position (w defaults to 1)
    float t[4];   // attribute 1: texture coordinate in .xy
};
constexpr int kStride = static_cast<int>(sizeof(Vtx));   // 28

// Vertex layout and state shared by every pass of a scene (the same as the triangle scene, plus the texture coordinate).
void bind_common(App& app, NVNcommandBuffer* c) {
    auto& api = app.api;
    NVNblendState bs;       api.nvnBlendStateSetDefaults(&bs);
    NVNcolorState cs;       api.nvnColorStateSetDefaults(&cs);
    NVNchannelMaskState ms; api.nvnChannelMaskStateSetDefaults(&ms);
    NVNpolygonState ps;     api.nvnPolygonStateSetDefaults(&ps);
    NVNdepthStencilState ds; api.nvnDepthStencilStateSetDefaults(&ds);
    NVNmultisampleState mss; api.nvnMultisampleStateSetDefaults(&mss);
    api.nvnCommandBufferBindBlendState(c, &bs);
    api.nvnCommandBufferBindColorState(c, &cs);
    api.nvnCommandBufferBindChannelMaskState(c, &ms);
    api.nvnCommandBufferBindPolygonState(c, &ps);
    api.nvnCommandBufferBindDepthStencilState(c, &ds);
    api.nvnCommandBufferBindMultisampleState(c, &mss);

    NVNvertexAttribState attr[2];
    api.nvnVertexAttribStateSetDefaults(&attr[0]);
    api.nvnVertexAttribStateSetFormat(&attr[0], NVN_FORMAT_RGB32F, 0);
    api.nvnVertexAttribStateSetStreamIndex(&attr[0], 0);
    api.nvnVertexAttribStateSetDefaults(&attr[1]);
    api.nvnVertexAttribStateSetFormat(&attr[1], NVN_FORMAT_RGBA32F, 12);
    api.nvnVertexAttribStateSetStreamIndex(&attr[1], 0);
    NVNvertexStreamState stream;
    api.nvnVertexStreamStateSetDefaults(&stream);
    api.nvnVertexStreamStateSetStride(&stream, kStride);
    api.nvnCommandBufferBindVertexAttribState(c, 2, attr);
    api.nvnCommandBufferBindVertexStreamState(c, 1, &stream);

    api.nvnCommandBufferSetTexturePool(c, app.tex_pool.get());
    api.nvnCommandBufferSetSamplerPool(c, app.smp_pool.get());
}

// Starts a pass on `target`: render target, full viewport, cleared to black, program bound.
void record_pass(App& app, NVNcommandBuffer* c, NVNtexture* target, NVNprogram* prog, uint32_t w, uint32_t h) {
    NVNtexture* targets[] = {target};
    app.api.nvnCommandBufferSetRenderTargets(c, 1, targets, nullptr, nullptr, nullptr);
    app.api.nvnCommandBufferSetViewport(c, 0, 0, static_cast<int>(w), static_cast<int>(h));
    app.api.nvnCommandBufferSetScissor(c, 0, 0, static_cast<int>(w), static_cast<int>(h));
    const float black[4] = {0, 0, 0, 1};
    app.api.nvnCommandBufferClearColor(c, 0, black, NVN_CLEAR_COLOR_MASK_RGBA);
    app.api.nvnCommandBufferBindProgram(c, prog, NVN_SHADER_STAGE_VERTEX_BIT | NVN_SHADER_STAGE_FRAGMENT_BIT);
}

// One quad over the viewport rectangle (x, y, w, h). The texture coordinate is (u0, v0) at the top-left corner and (u1, v1) at the
// bottom-right corner, linear in between, so a pixel centre gets u = u0 + (xl + 0.5) / w * (u1 - u0) (xl = column inside the rectangle).
void draw_quad(App& app, NVNcommandBuffer* c, uint64_t tex_handle, int x, int y, int w, int h, float u0, float v0, float u1, float v1) {
    auto mk = [&](float px, float py) {
        Vtx v;
        v.p[0] = px;
        v.p[1] = py;
        v.p[2] = 0.0f;
        v.t[0] = u0 + (px + 1.0f) * 0.5f * (u1 - u0);
        v.t[1] = v0 + (1.0f - py) * 0.5f * (v1 - v0);
        v.t[2] = 0.0f;
        v.t[3] = 1.0f;
        return v;
    };
    const Vtx tl = mk(-1, 1), tr = mk(1, 1), bl = mk(-1, -1), br = mk(1, -1);
    const Vtx verts[6] = {tl, bl, tr, tr, bl, br};   // both triangles counter-clockwise
    App::Buf vb = app.make_buffer(sizeof verts, verts);
    app.api.nvnCommandBufferSetViewport(c, x, y, w, h);
    app.api.nvnCommandBufferSetScissor(c, x, y, w, h);
    app.api.nvnCommandBufferBindVertexBuffer(c, 0, vb.address, vb.size);
    app.api.nvnCommandBufferBindTexture(c, NVN_SHADER_STAGE_FRAGMENT, 0, tex_handle);
    app.api.nvnCommandBufferDrawArrays(c, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 6);
}

// Nearest sampler with the given wrap modes (no mip levels: the base level is always used).
int make_sampler_nearest(App& app, NVNwrapMode wu = NVN_WRAP_MODE_CLAMP_TO_EDGE, NVNwrapMode wv = NVN_WRAP_MODE_CLAMP_TO_EDGE) {
    return app.make_sampler([&](NVNsamplerBuilder& sb) {
        app.api.nvnSamplerBuilderSetMinMagFilter(&sb, NVN_MIN_FILTER_NEAREST, NVN_MAG_FILTER_NEAREST);
        app.api.nvnSamplerBuilderSetWrapMode(&sb, wu, wv, NVN_WRAP_MODE_CLAMP_TO_EDGE);
    });
}
int make_sampler_linear(App& app, NVNwrapMode wu = NVN_WRAP_MODE_CLAMP_TO_EDGE, NVNwrapMode wv = NVN_WRAP_MODE_CLAMP_TO_EDGE) {
    return app.make_sampler([&](NVNsamplerBuilder& sb) {
        app.api.nvnSamplerBuilderSetMinMagFilter(&sb, NVN_MIN_FILTER_LINEAR, NVN_MAG_FILTER_LINEAR);
        app.api.nvnSamplerBuilderSetWrapMode(&sb, wu, wv, NVN_WRAP_MODE_CLAMP_TO_EDGE);
    });
}

// ---- tiling ------------------------------------------------------------------------------------------------------------------
// The block-linear layout the back end computes for a single-layer 2D texture (same rules as nvn_texture_layout.cpp: GOB block
// height from the level-0 block rows, tight linear rows).
struct Lay {
    tegra::TextureDesc d;
    tegra::Layout L;
};
Lay make_layout(uint32_t w, uint32_t h, uint32_t levels, uint32_t bw, uint32_t bh, uint32_t bpb) {
    Lay r;
    r.d.width = w;
    r.d.height = h;
    r.d.depth = 1;
    r.d.levels = levels;
    r.d.layers = 1;
    r.d.block_w = bw;
    r.d.block_h = bh;
    r.d.bytes_per_block = bpb;
    r.d.gob_blocks_y = nxrt::nvn::be::console_block_height((h + bh - 1) / bh);
    r.d.gob_blocks_z = 1;
    r.d.linear_stride_align = 1;
    // the console's GOB byte order, as the runtime picks it for the registered game's platform (nvn_texture_layout.cpp; oot is a Switch 2 title)
    r.d.gob_switch2 = nxrt::game::info().platform == nxrt::game::Platform::Switch2;
    r.L = tegra::compute_layout(r.d);
    return r;
}

// Tiles linear data (levels back to back, tight rows) and copies it into the texture's pool memory.
bool store_linear(Tex& t, const Lay& lay, const std::vector<uint8_t>& linear) {
    if (!lay.L.valid || linear.size() < lay.L.linear_size || t.size < lay.L.tiled_size) return false;
    std::vector<uint8_t> tiled(lay.L.tiled_size, 0);
    if (!tegra::tile(lay.d, lay.L, linear.data(), tiled.data())) return false;
    std::memcpy(t.memory(), tiled.data(), tiled.size());
    return true;
}

// Untiles the texture's storage (as the game holds it) back to linear data.
std::vector<uint8_t> load_linear(const Tex& t, const Lay& lay) {
    std::vector<uint8_t> tiled(lay.L.tiled_size), linear(lay.L.linear_size);
    std::memcpy(tiled.data(), t.memory(), tiled.size());
    tegra::untile(lay.d, lay.L, tiled.data(), linear.data());
    return linear;
}

// Writes one RGBA8 colour into every texel of a level (for solid-colour levels).
void fill_level_rgba8(std::vector<uint8_t>& linear, const Lay& lay, uint32_t level, const uint8_t c[4]) {
    const tegra::LevelLayout& lv = lay.L.levels[level];
    for (uint64_t i = 0; i < lv.linear_slice_size; ++i) linear[lv.linear_offset + i] = c[i & 3];
}

std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

// The 16x16 RGBA8 test pattern: texel (tx, ty) = (16 tx, 16 ty, 16 ((tx ^ ty) & 15), 255).
Rgb pattern16(int tx, int ty) { return {16 * tx, 16 * ty, 16 * ((tx ^ ty) & 15)}; }
std::vector<uint8_t> pattern16_linear() {
    std::vector<uint8_t> lin(16 * 16 * 4);
    for (int ty = 0; ty < 16; ++ty)
        for (int tx = 0; tx < 16; ++tx) {
            const Rgb c = pattern16(tx, ty);
            uint8_t* p = &lin[(ty * 16 + tx) * 4];
            p[0] = static_cast<uint8_t>(c.r);
            p[1] = static_cast<uint8_t>(c.g);
            p[2] = static_cast<uint8_t>(c.b);
            p[3] = 255;
        }
    return lin;
}
// Frame pixel of the 16x16 pattern magnified over the 64x48 frame: texel (x / 4, y / 3).
Rgbf pattern16_frame(uint32_t x, uint32_t y) {
    const Rgb c = pattern16(static_cast<int>(x / 4), static_cast<int>(y / 3));
    return {static_cast<double>(c.r), static_cast<double>(c.g), static_cast<double>(c.b)};
}

// ---- CPU references ----------------------------------------------------------------------------------------------------------
struct Texels {
    int w = 0, h = 0;
    std::vector<Rgb> t;
};
enum class Wrap { Repeat, Mirror, ClampEdge, ClampBorder };

// Maps a texel index through a wrap mode; false when the texel is outside and the mode is clamp-to-border.
bool wrap_index(int i, int n, Wrap m, int* out) {
    switch (m) {
        case Wrap::Repeat: *out = ((i % n) + n) % n; return true;
        case Wrap::Mirror: {
            const int p = 2 * n;
            const int k = ((i % p) + p) % p;
            *out = k < n ? k : p - 1 - k;
            return true;
        }
        case Wrap::ClampEdge: *out = std::clamp(i, 0, n - 1); return true;
        case Wrap::ClampBorder:
            if (i < 0 || i >= n) return false;
            *out = i;
            return true;
    }
    return false;
}
Rgbf fetch(const Texels& T, int ix, int iy, Wrap wu, Wrap wv, const Rgbf& border) {
    int x = 0, y = 0;
    if (!wrap_index(ix, T.w, wu, &x) || !wrap_index(iy, T.h, wv, &y)) return border;
    const Rgb c = T.t[static_cast<size_t>(y) * T.w + x];
    return {static_cast<double>(c.r), static_cast<double>(c.g), static_cast<double>(c.b)};
}
// Reference sampler: nearest picks texel floor(u * W); linear blends the four texels around (u * W - 0.5) with weights from the
// fractions, as GL and Vulkan define it. (u, v) in texture units 0..1 (may lie outside).
Rgbf sample_ref(const Texels& T, double u, double v, bool linear, Wrap wu, Wrap wv, const Rgbf& border) {
    const double tx = u * T.w, ty = v * T.h;
    if (!linear) return fetch(T, static_cast<int>(std::floor(tx)), static_cast<int>(std::floor(ty)), wu, wv, border);
    const double sx = tx - 0.5, sy = ty - 0.5;
    const int x0 = static_cast<int>(std::floor(sx)), y0 = static_cast<int>(std::floor(sy));
    const double fx = sx - x0, fy = sy - y0;
    Rgbf r{0, 0, 0};
    for (int dy = 0; dy < 2; ++dy)
        for (int dx = 0; dx < 2; ++dx) {
            const double wgt = (dx ? fx : 1 - fx) * (dy ? fy : 1 - fy);
            const Rgbf c = fetch(T, x0 + dx, y0 + dy, wu, wv, border);
            r.r += wgt * c.r;
            r.g += wgt * c.g;
            r.b += wgt * c.b;
        }
    return r;
}

// The 2x2 and 4x4 gradient textures of the filter scenes.
Texels gradient4() {
    Texels T;
    T.w = T.h = 4;
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) T.t.push_back({32 * x + 16, 32 * y + 16, 128});
    return T;
}
std::vector<uint8_t> rgba_of(const Texels& T) {
    std::vector<uint8_t> out;
    for (const Rgb& c : T.t) {
        out.push_back(static_cast<uint8_t>(c.r));
        out.push_back(static_cast<uint8_t>(c.g));
        out.push_back(static_cast<uint8_t>(c.b));
        out.push_back(255);
    }
    return out;
}

// BC1 (DXT1) texel decode, 8-byte block, p = row-major texel index 0..15 in the block.
Rgb expand565(uint16_t c) {
    const int r5 = (c >> 11) & 31, g6 = (c >> 5) & 63, b5 = c & 31;
    return {(r5 << 3) | (r5 >> 2), (g6 << 2) | (g6 >> 4), (b5 << 3) | (b5 >> 2)};
}
Rgb bc1_texel(const uint8_t* blk, int p) {
    const uint16_t c0 = static_cast<uint16_t>(blk[0] | (blk[1] << 8));
    const uint16_t c1 = static_cast<uint16_t>(blk[2] | (blk[3] << 8));
    const uint32_t word = blk[4] | (blk[5] << 8) | (blk[6] << 16) | (static_cast<uint32_t>(blk[7]) << 24);
    const int idx = static_cast<int>((word >> (2 * p)) & 3);
    const Rgb a = expand565(c0), b = expand565(c1);
    if (c0 > c1) {
        switch (idx) {
            case 0: return a;
            case 1: return b;
            case 2: return {(2 * a.r + b.r) / 3, (2 * a.g + b.g) / 3, (2 * a.b + b.b) / 3};
            default: return {(a.r + 2 * b.r) / 3, (a.g + 2 * b.g) / 3, (a.b + 2 * b.b) / 3};
        }
    }
    switch (idx) {
        case 0: return a;
        case 1: return b;
        case 2: return {(a.r + b.r) / 2, (a.g + b.g) / 2, (a.b + b.b) / 2};
        default: return {0, 0, 0};   // transparent black in the 3-colour mode
    }
}
void bc1_encode(uint8_t* out, uint16_t c0, uint16_t c1, const int idx[16]) {
    out[0] = static_cast<uint8_t>(c0 & 0xff);
    out[1] = static_cast<uint8_t>(c0 >> 8);
    out[2] = static_cast<uint8_t>(c1 & 0xff);
    out[3] = static_cast<uint8_t>(c1 >> 8);
    uint32_t w = 0;
    for (int p = 0; p < 16; ++p) w |= static_cast<uint32_t>(idx[p] & 3) << (2 * p);
    for (int k = 0; k < 4; ++k) out[4 + k] = static_cast<uint8_t>(w >> (8 * k));
}

// BC7 mode 6 (one 4-bit index per texel, 7-bit endpoints with one p-bit each) encoder for hand-built blocks.
struct BitWriter {
    std::array<uint8_t, 16> b{};
    int n = 0;
    void put(uint32_t v, int bits) {
        for (int i = 0; i < bits; ++i, ++n)
            if ((v >> i) & 1) b[n >> 3] = static_cast<uint8_t>(b[n >> 3] | (1u << (n & 7)));
    }
};
std::array<uint8_t, 16> bc7_mode6(const int e0[4], const int e1[4], int p0, int p1, const int idx[16]) {
    BitWriter w;
    w.put(0, 6);   // mode 6: six zero bits, then a one
    w.put(1, 1);
    for (int ch = 0; ch < 4; ++ch) {   // R, G, B, A endpoints, 7 bits each
        w.put(static_cast<uint32_t>(e0[ch]), 7);
        w.put(static_cast<uint32_t>(e1[ch]), 7);
    }
    w.put(static_cast<uint32_t>(p0), 1);
    w.put(static_cast<uint32_t>(p1), 1);
    w.put(static_cast<uint32_t>(idx[0]), 3);   // texel 0 is the anchor: its top bit is implied 0
    for (int p = 1; p < 16; ++p) w.put(static_cast<uint32_t>(idx[p]), 4);
    return w.b;   // exactly 128 bits
}
const int kBc7Weight4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};
// Decodes the mode-6 block (as the encoder above) for texel p; rgb only.
Rgb bc7_mode6_texel(const int e0[4], const int e1[4], int p0, int p1, const int idx[16], int p) {
    int c[3];
    for (int ch = 0; ch < 3; ++ch) {
        const int a = (e0[ch] << 1) | p0, b = (e1[ch] << 1) | p1, w = kBc7Weight4[idx[p]];
        c[ch] = ((64 - w) * a + w * b + 32) >> 6;
    }
    return {c[0], c[1], c[2]};
}

double srgb_to_linear_byte(int v) {
    const double c = v / 255.0;
    const double l = c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
    return l * 255.0;
}

// Scene pixel to the 8x8 texture of the block scenes: texel (x / 8, y / 6).
// Block of texel (tx, ty) in a texture that is blocks_per_row blocks wide: index (ty / 4) * blocks_per_row + tx / 4.

}  // namespace

// ================================================================================================================================
// Scene 1: nearest magnification of a tiled 16x16 RGBA8 texture.
NXTEST(nearest_magnify_rgba8_pattern) {
    if (!real_data_available()) { nxtest::skip("work/shader_cache not found"); return; }
    App app;
    App::Opts o;
    o.width = kW;
    o.height = kH;
    REQUIRE(app.init(o));
    App::ShaderBlob vs, fs;
    REQUIRE(load_quad_shaders(&vs, &fs));
    auto prog = app.make_program({&vs, &fs});
    REQUIRE(prog != nullptr);
    auto tex = app.make_texture_2d(NVN_FORMAT_RGBA8, 16, 16);
    REQUIRE(tex != nullptr);
    const Lay lay = make_layout(16, 16, 1, 1, 1, 4);
    std::printf("    storage %zu bytes, layout %llu bytes, gob block height %u\n", tex->size, static_cast<unsigned long long>(lay.L.tiled_size), lay.d.gob_blocks_y);
    REQUIRE(store_linear(*tex, lay, pattern16_linear()));
    const uint64_t th = app.texture_handle(tex->id, make_sampler_nearest(app));

    auto cb = app.make_cmdbuf();
    REQUIRE(cb != nullptr);
    NVNcommandBuffer* c = cb->obj.get();
    const int idx = app.acquire();
    REQUIRE(idx >= 0);
    app.api.nvnCommandBufferBeginRecording(c);
    bind_common(app, c);
    record_pass(app, c, app.window_textures[idx]->obj.get(), prog.get(), kW, kH);
    draw_quad(app, c, th, 0, 0, kW, kH, 0, 0, 1, 1);
    app.end_and_submit(*cb);
    std::vector<uint8_t> px;
    REQUIRE(app.present_and_capture(idx, &px));

    const Cmp cmp = compare_rect(px, 0, 0, kW, kH, pattern16_frame, 0);
    report("frame vs pattern (exact)", cmp, 0);
    std::printf("    pixel (0,0) %d %d %d, (63,47) %d %d %d\n", at(px, 0, 0).r, at(px, 0, 0).g, at(px, 0, 0).b, at(px, 63, 47).r, at(px, 63, 47).g, at(px, 63, 47).b);
    CHECK_EQ(cmp.bad, 0);
    check_clean(app);
    if (cmp.bad == 0) reference_check("texture_nearest_rgba8", px);
}

// Scene 6d: CPU texel writes and reads, and the storage they leave behind (no GPU work).
NXTEST(texel_write_read_round_trip) {
    App app;
    App::Opts o;
    o.width = kW;
    o.height = kH;
    REQUIRE(app.init(o));
    auto tex = app.make_texture_2d(NVN_FORMAT_RGBA8, 16, 16);
    REQUIRE(tex != nullptr);
    const Lay lay = make_layout(16, 16, 1, 1, 1, 4);
    NVNcopyRegion full{0, 0, 0, 16, 16, 1};
    const std::vector<uint8_t> lin = pattern16_linear();
    app.api.nvnTextureWriteTexels(tex->obj.get(), nullptr, &full, lin.data());
    std::vector<uint8_t> back(lin.size(), 0);
    app.api.nvnTextureReadTexels(tex->obj.get(), nullptr, &full, back.data());
    size_t bad = 0;
    for (size_t i = 0; i < lin.size(); ++i) bad += lin[i] != back[i] ? 1 : 0;
    std::printf("    full write then read: %zu of %zu bytes differ\n", bad, lin.size());
    CHECK_EQ(bad, 0u);

    // a 4x4 write at (4, 4) replaces only that block of texels
    std::vector<uint8_t> patch(4 * 4 * 4);
    for (size_t i = 0; i < patch.size(); i += 4) {
        patch[i] = 1;
        patch[i + 1] = 2;
        patch[i + 2] = 3;
        patch[i + 3] = 255;
    }
    NVNcopyRegion part{4, 4, 0, 4, 4, 1};
    app.api.nvnTextureWriteTexels(tex->obj.get(), nullptr, &part, patch.data());
    std::vector<uint8_t> expect = lin;
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x)
            for (int k = 0; k < 4; ++k) expect[((4 + y) * 16 + (4 + x)) * 4 + k] = patch[(y * 4 + x) * 4 + k];
    std::vector<uint8_t> back2(expect.size(), 0);
    app.api.nvnTextureReadTexels(tex->obj.get(), nullptr, &full, back2.data());
    bad = 0;
    for (size_t i = 0; i < expect.size(); ++i) bad += expect[i] != back2[i] ? 1 : 0;
    std::printf("    partial write (4x4 at 4,4) then full read: %zu bytes differ\n", bad);
    CHECK_EQ(bad, 0u);

    // the storage itself holds the texels in block-linear order: untiling it gives the same image
    const std::vector<uint8_t> untiled = load_linear(*tex, lay);
    bad = 0;
    for (size_t i = 0; i < expect.size(); ++i) bad += expect[i] != untiled[i] ? 1 : 0;
    std::printf("    untiled storage vs written texels: %zu bytes differ\n", bad);
    CHECK_EQ(bad, 0u);
    const auto v = app.validation();
    CHECK_EQ(v.errors, 0ull);
}

// Scene 2a: linear against nearest on a 2x2 texture, each in its own half of the frame. Linear is checked against the reference
// sampler within 2 levels (the GPU's filter weights are 8-bit); nearest must be exact.
NXTEST(linear_and_nearest_filters_2x2) {
    if (!real_data_available()) { nxtest::skip("work/shader_cache not found"); return; }
    App app;
    App::Opts o;
    o.width = kW;
    o.height = kH;
    REQUIRE(app.init(o));
    App::ShaderBlob vs, fs;
    REQUIRE(load_quad_shaders(&vs, &fs));
    auto prog = app.make_program({&vs, &fs});
    REQUIRE(prog != nullptr);
    auto tex = app.make_texture_2d(NVN_FORMAT_RGBA8, 2, 2);
    REQUIRE(tex != nullptr);
    Texels T;
    T.w = T.h = 2;
    T.t = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 255}};   // (0,0) red, (1,0) green, (0,1) blue, (1,1) white
    const std::vector<uint8_t> lin = rgba_of(T);
    REQUIRE(store_linear(*tex, make_layout(2, 2, 1, 1, 1, 4), lin));
    const uint64_t th_near = app.texture_handle(tex->id, make_sampler_nearest(app));
    const uint64_t th_lin = app.texture_handle(tex->id, make_sampler_linear(app));

    auto cb = app.make_cmdbuf();
    REQUIRE(cb != nullptr);
    NVNcommandBuffer* c = cb->obj.get();
    const int idx = app.acquire();
    REQUIRE(idx >= 0);
    app.api.nvnCommandBufferBeginRecording(c);
    bind_common(app, c);
    record_pass(app, c, app.window_textures[idx]->obj.get(), prog.get(), kW, kH);
    draw_quad(app, c, th_near, 0, 0, 32, kH, 0, 0, 1, 1);
    draw_quad(app, c, th_lin, 32, 0, 32, kH, 0, 0, 1, 1);
    app.end_and_submit(*cb);
    std::vector<uint8_t> px;
    REQUIRE(app.present_and_capture(idx, &px));

    // the quad's pixel centres: u = (xl + 0.5) / 32, v = (y + 0.5) / 48 inside each half
    const Texels& TT = T;
    const Cmp nearc = compare_rect(px, 0, 0, 32, kH, [&](uint32_t x, uint32_t y) {
        const Rgbf e = sample_ref(TT, (x + 0.5) / 32.0, (y + 0.5) / 48.0, false, Wrap::ClampEdge, Wrap::ClampEdge, {0, 0, 0});
        return e;
    }, 0);
    const Cmp lin_c = compare_rect(px, 32, 0, 32, kH, [&](uint32_t x, uint32_t y) {
        const Rgbf e = sample_ref(TT, (x - 32 + 0.5) / 32.0, (y + 0.5) / 48.0, true, Wrap::ClampEdge, Wrap::ClampEdge, {0, 0, 0});
        return e;
    }, 2);
    report("nearest half vs reference (exact)", nearc, 0);
    report("linear half vs reference (+-2)", lin_c, 2);
    const Rgb mid = at(px, 47, 24);
    std::printf("    linear pixel (47,24) %d %d %d (blend of the four texels)\n", mid.r, mid.g, mid.b);
    CHECK_EQ(nearc.bad, 0);
    CHECK_EQ(lin_c.bad, 0);
    check_clean(app);
}

// Scene 3a: BC1 blocks built by hand (4-colour and 3-colour modes, transparent index), 8x8 texture magnified to the frame.
NXTEST(bc1_blocks_built_by_hand) {
    if (!real_data_available()) { nxtest::skip("work/shader_cache not found"); return; }
    App app;
    App::Opts o;
    o.width = kW;
    o.height = kH;
    REQUIRE(app.init(o));
    App::ShaderBlob vs, fs;
    REQUIRE(load_quad_shaders(&vs, &fs));
    auto prog = app.make_program({&vs, &fs});
    REQUIRE(prog != nullptr);
    // 2x2 blocks of 8 bytes; block order is row-major over blocks
    std::vector<uint8_t> blocks(4 * 8);
    int i0[16], i1[16], i2[16], i3[16];
    for (int p = 0; p < 16; ++p) {
        i0[p] = p & 3;                  // 0 red, 1 blue, 2 mid, 3 mid
        i1[p] = (p + p / 4) & 3;        // mixes all four
        i2[p] = (p * 7 + 1) & 3;        // includes index 3 (transparent in the 3-colour mode)
        i3[p] = 3 - (p & 3);
    }
    bc1_encode(&blocks[0], 0xF800, 0x001F, i0);   // pure red, pure blue (4-colour)
    bc1_encode(&blocks[8], 0x07E0, 0x0000, i1);   // pure green, black (4-colour)
    bc1_encode(&blocks[16], 0x0000, 0xFFFF, i2);  // black, white (3-colour: c0 <= c1)
    bc1_encode(&blocks[24], 0x8410, 0x0000, i3);  // mid grey, black (4-colour)

    auto tex = app.make_texture_2d(NVN_FORMAT_RGBA_DXT1, 8, 8);
    REQUIRE(tex != nullptr);
    const Lay lay = make_layout(8, 8, 1, 4, 4, 8);
    std::printf("    storage %zu bytes, tiled layout %llu bytes, block height %u GOB\n", tex->size, static_cast<unsigned long long>(lay.L.tiled_size), lay.d.gob_blocks_y);
    REQUIRE(store_linear(*tex, lay, blocks));
    const uint64_t th = app.texture_handle(tex->id, make_sampler_nearest(app));

    auto cb = app.make_cmdbuf();
    REQUIRE(cb != nullptr);
    NVNcommandBuffer* c = cb->obj.get();
    const int idx = app.acquire();
    REQUIRE(idx >= 0);
    app.api.nvnCommandBufferBeginRecording(c);
    bind_common(app, c);
    record_pass(app, c, app.window_textures[idx]->obj.get(), prog.get(), kW, kH);
    draw_quad(app, c, th, 0, 0, kW, kH, 0, 0, 1, 1);
    app.end_and_submit(*cb);
    std::vector<uint8_t> px;
    REQUIRE(app.present_and_capture(idx, &px));

    // texel (x / 8, y / 6); block (tx / 4, ty / 4); texel in block (ty % 4) * 4 + tx % 4
    auto expect = [&](uint32_t x, uint32_t y) {
        const int tx = static_cast<int>(x / 8), ty = static_cast<int>(y / 6);
        const uint8_t* blk = &blocks[static_cast<size_t>((ty / 4) * 2 + tx / 4) * 8];
        const Rgb c2 = bc1_texel(blk, (ty % 4) * 4 + tx % 4);
        return Rgbf{static_cast<double>(c2.r), static_cast<double>(c2.g), static_cast<double>(c2.b)};
    };
    // Compressed formats are judged by PSNR (the guide's bound). The GPU's BC1 colour interpolation is not the formula's exact
    // rounding (see the palette below), so a per-texel check is not the right bound.
    const Psnr ps = psnr_rect(px, 0, 0, kW, kH, expect);
    std::printf("    frame vs BC1 decode of the blocks: PSNR %.2f dB (bound 30), worst channel difference %d\n", ps.db, ps.worst);
    // diagnostic: for every block and palette index, the mean GPU colour of the texels that use it against the formula's colour
    for (int bk = 0; bk < 4; ++bk) {
        const int bx = bk % 2, by = bk / 2;
        for (int k = 0; k < 4; ++k) {
            long long sum[3] = {0, 0, 0};
            int n = 0;
            Rgb formula{0, 0, 0};
            for (int p = 0; p < 16; ++p) {
                const int idx = (bk == 0 ? i0[p] : bk == 1 ? i1[p] : bk == 2 ? i2[p] : i3[p]);
                if (idx != k) continue;
                const uint32_t tx = static_cast<uint32_t>(bx * 4 + p % 4), ty = static_cast<uint32_t>(by * 4 + p / 4);
                for (uint32_t y = ty * 6; y < ty * 6 + 6; ++y)
                    for (uint32_t x = tx * 8; x < tx * 8 + 8; ++x) {
                        const Rgb g = at(px, x, y);
                        sum[0] += g.r;
                        sum[1] += g.g;
                        sum[2] += g.b;
                        ++n;
                    }
                const uint8_t* blk = &blocks[static_cast<size_t>(by * 2 + bx) * 8];
                formula = bc1_texel(blk, p);
            }
            if (n) std::printf("    block %d index %d: GPU mean %.1f %.1f %.1f (%d px), formula %d %d %d\n", bk, k,
                               static_cast<double>(sum[0]) / n, static_cast<double>(sum[1]) / n, static_cast<double>(sum[2]) / n, n, formula.r,
                               formula.g, formula.b);
        }
    }
    std::printf("    red block corner %d %d %d, transparent-index texel %d %d %d\n", at(px, 0, 0).r, at(px, 0, 0).g, at(px, 0, 0).b,
                at(px, 0, 30).r, at(px, 0, 30).g, at(px, 0, 30).b);
    CHECK(ps.db >= 30.0);
    check_clean(app);
    if (ps.db >= 30.0) reference_check("texture_bc1_hand_built", px);
}

// Scene 3e: a real RGBA8 texture of the game (Rgba8Unorm 256x128, 9 levels, sample s055), copied in as stored. The storage must
// have the size the survey recorded, untile to the survey's linear data, and sample exactly.
NXTEST(real_rgba8_texture_from_the_game) {
    const std::string dir = kSamples;
    const std::vector<uint8_t> tiled = read_file(dir + "s055.tiled"), lin_ref = read_file(dir + "s055.lin");
    if (tiled.empty() || lin_ref.empty()) { nxtest::skip("survey sample s055 not found"); return; }
    if (!real_data_available()) { nxtest::skip("work/shader_cache not found"); return; }
    App app;
    App::Opts o;
    o.width = kW;
    o.height = kH;
    REQUIRE(app.init(o));
    App::ShaderBlob vs, fs;
    REQUIRE(load_quad_shaders(&vs, &fs));
    auto prog = app.make_program({&vs, &fs});
    REQUIRE(prog != nullptr);
    const Lay lay = make_layout(256, 128, 9, 1, 1, 4);
    std::printf("    survey tiled %zu bytes, layout %llu bytes, linear %llu bytes (survey .lin %zu)\n", tiled.size(),
                static_cast<unsigned long long>(lay.L.tiled_size), static_cast<unsigned long long>(lay.L.linear_size), lin_ref.size());
    CHECK_EQ(lay.L.tiled_size, static_cast<uint64_t>(tiled.size()));
    auto tex = app.make_texture_2d(NVN_FORMAT_RGBA8, 256, 128, 0, 9);
    REQUIRE(tex != nullptr);
    std::printf("    storage from the builder %zu bytes\n", tex->size);
    CHECK_EQ(tex->size, tiled.size());
    REQUIRE(tex->size >= tiled.size());
    std::memcpy(tex->memory(), tiled.data(), tiled.size());
    // the layout reproduces the survey's linear mip chain byte for byte
    const std::vector<uint8_t> untiled = load_linear(*tex, lay);
    size_t bad = 0;
    for (size_t i = 0; i < lin_ref.size() && i < untiled.size(); ++i) bad += lin_ref[i] != untiled[i] ? 1 : 0;
    std::printf("    untiled chain vs survey .lin: %zu bytes differ\n", bad);
    CHECK_EQ(bad, 0u);

    const uint64_t th = app.texture_handle(tex->id, make_sampler_nearest(app));
    auto cb = app.make_cmdbuf();
    REQUIRE(cb != nullptr);
    NVNcommandBuffer* c = cb->obj.get();
    const int idx = app.acquire();
    REQUIRE(idx >= 0);
    app.api.nvnCommandBufferBeginRecording(c);
    bind_common(app, c);
    record_pass(app, c, app.window_textures[idx]->obj.get(), prog.get(), kW, kH);
    // texel column floor(3 (x + 0.5)), row floor(2.5 (y + 0.5)): the frame shows the top-left 192 x 120 texels of level 0
    draw_quad(app, c, th, 0, 0, kW, kH, 0, 0, 0.75f, 0.9375f);
    app.end_and_submit(*cb);
    std::vector<uint8_t> px;
    REQUIRE(app.present_and_capture(idx, &px));

    const Cmp cmp = compare_rect(px, 0, 0, kW, kH, [&](uint32_t x, uint32_t y) {
        const int tx = static_cast<int>(std::floor(3.0 * (x + 0.5))), ty = static_cast<int>(std::floor(2.5 * (y + 0.5)));
        const size_t at_texel = (static_cast<size_t>(ty) * 256 + tx) * 4;
        return Rgbf{static_cast<double>(lin_ref[at_texel]), static_cast<double>(lin_ref[at_texel + 1]), static_cast<double>(lin_ref[at_texel + 2])};
    }, 0);
    report("frame vs survey texels (exact)", cmp, 0);
    CHECK_EQ(cmp.bad, 0);
    check_clean(app);
    if (cmp.bad == 0) reference_check("texture_real_rgba8_s055", px);
}

// Scene 3c: a real ASTC 8x5 texture (sample s023, 64x64, 7 levels; 1005 distinct colours). The back end transcodes it (to BC7, or keeps it when the device
// has ASTC); the frame must match the astc2bc7 decode of the same bytes with PSNR >= 30 dB.
NXTEST(real_astc_8x5_texture_transcoded) {
    FreshBc7Cache fresh_cache;
    const std::string dir = kSamples;
    const std::vector<uint8_t> tiled = read_file(dir + "s023.tiled"), lin_ref = read_file(dir + "s023.lin");
    if (tiled.empty() || lin_ref.empty()) { nxtest::skip("survey sample s023 not found"); return; }
    if (!real_data_available()) { nxtest::skip("work/shader_cache not found"); return; }
    App app;
    App::Opts o;
    o.width = kW;
    o.height = kH;
    REQUIRE(app.init(o));
    App::ShaderBlob vs, fs;
    REQUIRE(load_quad_shaders(&vs, &fs));
    auto prog = app.make_program({&vs, &fs});
    REQUIRE(prog != nullptr);
    const Lay lay = make_layout(64, 64, 7, 8, 5, 16);
    CHECK_EQ(lay.L.tiled_size, static_cast<uint64_t>(tiled.size()));
    auto tex = app.make_texture_2d(NVN_FORMAT_RGBA_ASTC_8x5, 64, 64, 0, 7);
    REQUIRE(tex != nullptr);
    std::printf("    storage %zu bytes (survey tiled %zu)\n", tex->size, tiled.size());
    CHECK_EQ(tex->size, tiled.size());
    REQUIRE(tex->size >= tiled.size());
    std::memcpy(tex->memory(), tiled.data(), tiled.size());
    const std::vector<uint8_t> untiled = load_linear(*tex, lay);
    size_t bad = 0;
    for (size_t i = 0; i < lin_ref.size() && i < untiled.size(); ++i) bad += lin_ref[i] != untiled[i] ? 1 : 0;
    std::printf("    untiled ASTC chain vs survey .lin: %zu bytes differ\n", bad);
    CHECK_EQ(bad, 0u);

    // reference: astc2bc7's ASTC decode of level 0 (104 blocks of 16 bytes: 8 x 13 blocks of 8 x 5 texels)
    std::vector<uint8_t> ref(64 * 64 * 4);
    a2b_input in{};
    in.format = A2B_ASTC_UNORM;
    in.width = 64;
    in.height = 64;
    in.block_w = 8;
    in.block_h = 5;
    const a2b_status st = a2b_decode_astc_rgba8(&in, untiled.data(), a2b_astc_size(64, 64, 8, 5), 0, ref.data(), nullptr);
    CHECK_EQ(static_cast<int>(st), static_cast<int>(A2B_OK));

    const uint64_t th = app.texture_handle(tex->id, make_sampler_nearest(app));
    auto cb = app.make_cmdbuf();
    REQUIRE(cb != nullptr);
    NVNcommandBuffer* c = cb->obj.get();
    const int idx = app.acquire();
    REQUIRE(idx >= 0);
    app.api.nvnCommandBufferBeginRecording(c);
    bind_common(app, c);
    record_pass(app, c, app.window_textures[idx]->obj.get(), prog.get(), kW, kH);
    draw_quad(app, c, th, 0, 0, kW, kH, 0, 0, 1.0f, 0.75f);   // texel (x, y) on pixel (x, y): the top 48 rows
    app.end_and_submit(*cb);
    std::vector<uint8_t> px;
    REQUIRE(app.present_and_capture(idx, &px));

    double mse = 0;
    int worst = 0;
    for (uint32_t y = 0; y < kH; ++y)
        for (uint32_t x = 0; x < kW; ++x) {
            const size_t t = (static_cast<size_t>(y) * 64 + x) * 4;
            const Rgb g = at(px, x, y);
            const int d[3] = {g.r - ref[t], g.g - ref[t + 1], g.b - ref[t + 2]};
            for (int k = 0; k < 3; ++k) {
                mse += static_cast<double>(d[k]) * d[k];
                worst = std::max(worst, std::abs(d[k]));
            }
        }
    mse /= static_cast<double>(kW * kH * 3);
    const double psnr = mse == 0 ? 999.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
    std::printf("    PSNR vs astc2bc7 decode %.2f dB (bound 30), worst channel difference %d\n", psnr, worst);
    CHECK(psnr >= 30.0);
    check_clean(app);
#ifdef NXRT_HAVE_BC7_REF
    // The expected picture is worked out here, with the encoder of this build: level 0 goes through the same a2b_transcode call and options the back end uses
    // (quality from [nvn] bc7_quality, default 3), the BC7 blocks are decoded with the reference decoder, and the frame must equal that decode up to the
    // rounding of the GPU's own BC7 decode. A stored image would go stale with every encoder revision.
    {
        a2b_options eo{};
        eo.quality = static_cast<int>(nxrt::config_get_int("nvn", "bc7_quality", 3));
        eo.threads = 0;
        eo.perceptual = 0;
        eo.allow_opaque_mode6 = 0;
        a2b_format of;
        std::vector<uint8_t> blocks(static_cast<size_t>(a2b_output_size(&in)));
        const a2b_status ts = a2b_transcode(&in, untiled.data(), a2b_astc_size(64, 64, 8, 5), &eo, &of, blocks.data(), blocks.size(), nullptr);
        CHECK_EQ(static_cast<int>(ts), static_cast<int>(A2B_OK));
        if (ts == A2B_OK) {
            const Cmp own = compare_rect(px, 0, 0, kW, kH, [&](uint32_t x, uint32_t y) {
                bc7decomp::color_rgba blk[16];
                const uint8_t* b = &blocks[(static_cast<size_t>(y / 4) * 16 + x / 4) * 16];
                bc7decomp_ref::unpack_bc7(b, blk);
                const bc7decomp::color_rgba& c = blk[(y % 4) * 4 + (x % 4)];
                return Rgbf{static_cast<double>(c.r), static_cast<double>(c.g), static_cast<double>(c.b)};
            }, 2);
            report("frame vs the BC7 decode of this build's transcode (+-2)", own, 2);
            CHECK_EQ(own.bad, 0);
        }
    }
#endif
}

// Scene 3d: a real BC1 sRGB texture of the game (512x512, 10 levels, sample s037), sampled magnified-by-1/7.5 texels per pixel. The
// sampler converts sRGB to linear, so the expected value is the BC1 decode passed through the sRGB curve.
NXTEST(real_bc1_srgb_texture_from_the_game) {
    const std::string dir = kSamples;
    const std::vector<uint8_t> tiled = read_file(dir + "s037.tiled"), lin_ref = read_file(dir + "s037.lin");
    if (tiled.empty() || lin_ref.empty()) { nxtest::skip("survey sample s037 not found"); return; }
    if (!real_data_available()) { nxtest::skip("work/shader_cache not found"); return; }
    App app;
    App::Opts o;
    o.width = kW;
    o.height = kH;
    REQUIRE(app.init(o));
    App::ShaderBlob vs, fs;
    REQUIRE(load_quad_shaders(&vs, &fs));
    auto prog = app.make_program({&vs, &fs});
    REQUIRE(prog != nullptr);
    const Lay lay = make_layout(512, 512, 10, 4, 4, 8);
    CHECK_EQ(lay.L.tiled_size, static_cast<uint64_t>(tiled.size()));
    CHECK_EQ(lay.L.linear_size, static_cast<uint64_t>(lin_ref.size()));
    auto tex = app.make_texture_2d(NVN_FORMAT_RGBA_DXT1_SRGB, 512, 512, 0, 10);
    REQUIRE(tex != nullptr);
    std::printf("    storage %zu bytes (survey tiled %zu), block height %u GOB\n", tex->size, tiled.size(), lay.d.gob_blocks_y);
    CHECK_EQ(tex->size, tiled.size());
    REQUIRE(tex->size >= tiled.size());
    std::memcpy(tex->memory(), tiled.data(), tiled.size());
    const std::vector<uint8_t> untiled = load_linear(*tex, lay);
    size_t bad = 0;
    for (size_t i = 0; i < lin_ref.size() && i < untiled.size(); ++i) bad += lin_ref[i] != untiled[i] ? 1 : 0;
    std::printf("    untiled BC1 chain vs survey .lin: %zu bytes differ\n", bad);
    CHECK_EQ(bad, 0u);

    const uint64_t th = app.texture_handle(tex->id, make_sampler_nearest(app));
    auto cb = app.make_cmdbuf();
    REQUIRE(cb != nullptr);
    NVNcommandBuffer* c = cb->obj.get();
    const int idx = app.acquire();
    REQUIRE(idx >= 0);
    app.api.nvnCommandBufferBeginRecording(c);
    bind_common(app, c);
    record_pass(app, c, app.window_textures[idx]->obj.get(), prog.get(), kW, kH);
    // texel column floor(7.5 (x + 0.5)), row floor(7.5 (y + 0.5)) of level 0: the frame shows the top-left 480 x 360 texels
    draw_quad(app, c, th, 0, 0, kW, kH, 0, 0, 0.9375f, 0.703125f);
    app.end_and_submit(*cb);
    std::vector<uint8_t> px;
    REQUIRE(app.present_and_capture(idx, &px));

    auto expect = [&](uint32_t x, uint32_t y) {
        const int tx = static_cast<int>(std::floor(7.5 * (x + 0.5))), ty = static_cast<int>(std::floor(7.5 * (y + 0.5)));
        const uint8_t* blk = &lin_ref[static_cast<size_t>((ty / 4) * 128 + tx / 4) * 8];
        const Rgb c2 = bc1_texel(blk, (ty % 4) * 4 + tx % 4);
        return Rgbf{srgb_to_linear_byte(c2.r), srgb_to_linear_byte(c2.g), srgb_to_linear_byte(c2.b)};
    };
    // PSNR bound for compressed formats (guide); the sRGB curve steepens the BC1 interpolation difference near white
    const Psnr ps = psnr_rect(px, 0, 0, kW, kH, expect);
    std::printf("    frame vs BC1 decode through sRGB->linear: PSNR %.2f dB (bound 30), worst channel difference %d\n", ps.db, ps.worst);
    CHECK(ps.db >= 30.0);
    check_clean(app);
    if (ps.db >= 30.0) reference_check("texture_bc1_srgb_s037", px);
}

// Scene 3b: BC7 mode 6 blocks built by hand, 8x8 texture (2x2 blocks), decoded on the CPU with the spec's formula.
NXTEST(bc7_mode6_blocks_built_by_hand) {
    if (!real_data_available()) { nxtest::skip("work/shader_cache not found"); return; }
    App app;
    App::Opts o;
    o.width = kW;
    o.height = kH;
    REQUIRE(app.init(o));
    App::ShaderBlob vs, fs;
    REQUIRE(load_quad_shaders(&vs, &fs));
    auto prog = app.make_program({&vs, &fs});
    REQUIRE(prog != nullptr);

    struct Blk {
        int e0[4], e1[4], p0, p1;
        int idx[16];
    };
    Blk b[4];
    const int e0a[4] = {10, 20, 30, 127}, e1a[4] = {100, 90, 80, 127};
    const int e0b[4] = {120, 0, 64, 100}, e1b[4] = {5, 127, 64, 30};
    const int e0c[4] = {127, 127, 0, 0}, e1c[4] = {0, 0, 127, 127};
    const int e0d[4] = {1, 3, 5, 0}, e1d[4] = {2, 4, 6, 127};
    const int pd[4][2] = {{1, 0}, {0, 1}, {1, 1}, {0, 0}};
    const int* E0[4] = {e0a, e0b, e0c, e0d};
    const int* E1[4] = {e1a, e1b, e1c, e1d};
    for (int k = 0; k < 4; ++k) {
        for (int ch = 0; ch < 4; ++ch) {
            b[k].e0[ch] = E0[k][ch];
            b[k].e1[ch] = E1[k][ch];
        }
        b[k].p0 = pd[k][0];
        b[k].p1 = pd[k][1];
        for (int p = 0; p < 16; ++p) b[k].idx[p] = p == 0 ? 0 : (k == 0 ? p : k == 1 ? 15 - p : k == 2 ? (p * 5) & 15 : (p * 3 + 1) & 15);
    }
    std::vector<uint8_t> blocks;
    for (int k = 0; k < 4; ++k) {
        const std::array<uint8_t, 16> enc = bc7_mode6(b[k].e0, b[k].e1, b[k].p0, b[k].p1, b[k].idx);
        blocks.insert(blocks.end(), enc.begin(), enc.end());
    }
    auto tex = app.make_texture_2d(NVN_FORMAT_BPTC_UNORM, 8, 8);
    REQUIRE(tex != nullptr);
    REQUIRE(store_linear(*tex, make_layout(8, 8, 1, 4, 4, 16), blocks));
    const uint64_t th = app.texture_handle(tex->id, make_sampler_nearest(app));

    auto cb = app.make_cmdbuf();
    REQUIRE(cb != nullptr);
    NVNcommandBuffer* c = cb->obj.get();
    const int idx = app.acquire();
    REQUIRE(idx >= 0);
    app.api.nvnCommandBufferBeginRecording(c);
    bind_common(app, c);
    record_pass(app, c, app.window_textures[idx]->obj.get(), prog.get(), kW, kH);
    draw_quad(app, c, th, 0, 0, kW, kH, 0, 0, 1, 1);
    app.end_and_submit(*cb);
    std::vector<uint8_t> px;
    REQUIRE(app.present_and_capture(idx, &px));

    auto expect = [&](uint32_t x, uint32_t y) {
        const int tx = static_cast<int>(x / 8), ty = static_cast<int>(y / 6);
        const int k = (ty / 4) * 2 + tx / 4;
        const int p = (ty % 4) * 4 + tx % 4;
        const Rgb c2 = bc7_mode6_texel(b[k].e0, b[k].e1, b[k].p0, b[k].p1, b[k].idx, p);
        return Rgbf{static_cast<double>(c2.r), static_cast<double>(c2.g), static_cast<double>(c2.b)};
    };
    const Cmp cmp = compare_rect(px, 0, 0, kW, kH, expect, 1);
    report("frame vs BC7 mode 6 decode (+-1)", cmp, 1);
    CHECK_EQ(cmp.bad, 0);
    check_clean(app);
    if (cmp.bad == 0) reference_check("texture_bc7_mode6_hand_built", px);
}

// Scene 2b: wrap modes outside the unit square. Four strips of 16 x 48 pixels, texture coordinates from -1 to 2 in both directions
// over a 4x4 gradient: REPEAT, MIRRORED_REPEAT and CLAMP_TO_EDGE with nearest sampling, REPEAT with linear sampling.
NXTEST(sampler_wrap_modes_outside_the_unit_square) {
    if (!real_data_available()) { nxtest::skip("work/shader_cache not found"); return; }
    App app;
    App::Opts o;
    o.width = kW;
    o.height = kH;
    REQUIRE(app.init(o));
    App::ShaderBlob vs, fs;
    REQUIRE(load_quad_shaders(&vs, &fs));
    auto prog = app.make_program({&vs, &fs});
    REQUIRE(prog != nullptr);
    const Texels T = gradient4();
    auto tex = app.make_texture_2d(NVN_FORMAT_RGBA8, 4, 4);
    REQUIRE(tex != nullptr);
    REQUIRE(store_linear(*tex, make_layout(4, 4, 1, 1, 1, 4), rgba_of(T)));
    const uint64_t th_rep = app.texture_handle(tex->id, make_sampler_nearest(app, NVN_WRAP_MODE_REPEAT, NVN_WRAP_MODE_REPEAT));
    const uint64_t th_mir = app.texture_handle(tex->id, make_sampler_nearest(app, NVN_WRAP_MODE_MIRRORED_REPEAT, NVN_WRAP_MODE_MIRRORED_REPEAT));
    const uint64_t th_clp = app.texture_handle(tex->id, make_sampler_nearest(app, NVN_WRAP_MODE_CLAMP_TO_EDGE, NVN_WRAP_MODE_CLAMP_TO_EDGE));
    const uint64_t th_lin = app.texture_handle(tex->id, make_sampler_linear(app, NVN_WRAP_MODE_REPEAT, NVN_WRAP_MODE_REPEAT));

    auto cb = app.make_cmdbuf();
    REQUIRE(cb != nullptr);
    NVNcommandBuffer* c = cb->obj.get();
    const int idx = app.acquire();
    REQUIRE(idx >= 0);
    app.api.nvnCommandBufferBeginRecording(c);
    bind_common(app, c);
    record_pass(app, c, app.window_textures[idx]->obj.get(), prog.get(), kW, kH);
    draw_quad(app, c, th_rep, 0, 0, 16, kH, -1, -1, 2, 2);
    draw_quad(app, c, th_mir, 16, 0, 16, kH, -1, -1, 2, 2);
    draw_quad(app, c, th_clp, 32, 0, 16, kH, -1, -1, 2, 2);
    draw_quad(app, c, th_lin, 48, 0, 16, kH, -1, -1, 2, 2);
    app.end_and_submit(*cb);
    std::vector<uint8_t> px;
    REQUIRE(app.present_and_capture(idx, &px));

    const Rgbf none{0, 0, 0};
    auto strip = [&](int x0, Wrap wm, bool linear, uint32_t x, uint32_t y) {
        const double u = -1.0 + (x - x0 + 0.5) / 16.0 * 3.0;
        const double v = -1.0 + (y + 0.5) / 48.0 * 3.0;
        return sample_ref(T, u, v, linear, wm, wm, none);
    };
    const Cmp rep = compare_rect(px, 0, 0, 16, kH, [&](uint32_t x, uint32_t y) { return strip(0, Wrap::Repeat, false, x, y); }, 0);
    const Cmp mir = compare_rect(px, 16, 0, 16, kH, [&](uint32_t x, uint32_t y) { return strip(16, Wrap::Mirror, false, x, y); }, 0);
    const Cmp clp = compare_rect(px, 32, 0, 16, kH, [&](uint32_t x, uint32_t y) { return strip(32, Wrap::ClampEdge, false, x, y); }, 0);
    const Cmp lin = compare_rect(px, 48, 0, 16, kH, [&](uint32_t x, uint32_t y) { return strip(48, Wrap::Repeat, true, x, y); }, 2);
    report("REPEAT nearest (exact)", rep, 0);
    report("MIRRORED_REPEAT nearest (exact)", mir, 0);
    report("CLAMP_TO_EDGE nearest (exact)", clp, 0);
    report("REPEAT linear (+-2)", lin, 2);
    CHECK_EQ(rep.bad, 0);
    CHECK_EQ(mir.bad, 0);
    CHECK_EQ(clp.bad, 0);
    CHECK_EQ(lin.bad, 0);
    check_clean(app);
    if (rep.bad + mir.bad + clp.bad + lin.bad == 0) reference_check("texture_wrap_modes", px);
}

// Scene 2c: CLAMP_TO_BORDER with a border colour (0.25, 0.5, 1.0): nearest on the left half, linear on the right half.
NXTEST(sampler_border_colour) {
    if (!real_data_available()) { nxtest::skip("work/shader_cache not found"); return; }
    App app;
    App::Opts o;
    o.width = kW;
    o.height = kH;
    REQUIRE(app.init(o));
    App::ShaderBlob vs, fs;
    REQUIRE(load_quad_shaders(&vs, &fs));
    auto prog = app.make_program({&vs, &fs});
    REQUIRE(prog != nullptr);
    const Texels T = gradient4();
    auto tex = app.make_texture_2d(NVN_FORMAT_RGBA8, 4, 4);
    REQUIRE(tex != nullptr);
    REQUIRE(store_linear(*tex, make_layout(4, 4, 1, 1, 1, 4), rgba_of(T)));
    const float border[4] = {0.25f, 0.5f, 1.0f, 1.0f};
    auto mk = [&](NVNminFilter mn, NVNmagFilter mg) {
        return app.make_sampler([&](NVNsamplerBuilder& sb) {
            app.api.nvnSamplerBuilderSetMinMagFilter(&sb, mn, mg);
            app.api.nvnSamplerBuilderSetWrapMode(&sb, NVN_WRAP_MODE_CLAMP_TO_BORDER, NVN_WRAP_MODE_CLAMP_TO_BORDER, NVN_WRAP_MODE_CLAMP_TO_BORDER);
            app.api.nvnSamplerBuilderSetBorderColor(&sb, border);
        });
    };
    const uint64_t th_near = app.texture_handle(tex->id, mk(NVN_MIN_FILTER_NEAREST, NVN_MAG_FILTER_NEAREST));
    const uint64_t th_lin = app.texture_handle(tex->id, mk(NVN_MIN_FILTER_LINEAR, NVN_MAG_FILTER_LINEAR));

    auto cb = app.make_cmdbuf();
    REQUIRE(cb != nullptr);
    NVNcommandBuffer* c = cb->obj.get();
    const int idx = app.acquire();
    REQUIRE(idx >= 0);
    app.api.nvnCommandBufferBeginRecording(c);
    bind_common(app, c);
    record_pass(app, c, app.window_textures[idx]->obj.get(), prog.get(), kW, kH);
    draw_quad(app, c, th_near, 0, 0, 32, kH, -1, -1, 2, 2);
    draw_quad(app, c, th_lin, 32, 0, 32, kH, -1, -1, 2, 2);
    app.end_and_submit(*cb);
    std::vector<uint8_t> px;
    REQUIRE(app.present_and_capture(idx, &px));

    const Rgbf bc{0.25 * 255.0, 0.5 * 255.0, 255.0};
    const Cmp nearc = compare_rect(px, 0, 0, 32, kH, [&](uint32_t x, uint32_t y) {
        return sample_ref(T, -1.0 + (x + 0.5) / 32.0 * 3.0, -1.0 + (y + 0.5) / 48.0 * 3.0, false, Wrap::ClampBorder, Wrap::ClampBorder, bc);
    }, 1);
    const Cmp lin = compare_rect(px, 32, 0, 32, kH, [&](uint32_t x, uint32_t y) {
        return sample_ref(T, -1.0 + (x - 32 + 0.5) / 32.0 * 3.0, -1.0 + (y + 0.5) / 48.0 * 3.0, true, Wrap::ClampBorder, Wrap::ClampBorder, bc);
    }, 2);
    report("nearest, border outside (+-1)", nearc, 1);
    report("linear, border blended (+-2)", lin, 2);
    const Rgb corner = at(px, 0, 0);
    std::printf("    corner pixel %d %d %d (border 64 128 255)\n", corner.r, corner.g, corner.b);
    CHECK_EQ(nearc.bad, 0);
    CHECK_EQ(lin.bad, 0);
    check_clean(app);
}

// Scene 2d: mip levels with explicit LOD bias and clamp. Three solid levels (red, green, blue), an 8x8 texture drawn over 4x4
// pixels so the base LOD is exactly 1. Nominal LOD 1 selects green; bias +1 selects blue; clamp to 0.3 selects red; clamp from 1.7
// selects blue. (Nearest mip selection takes level ceil(lod + 0.5) - 1, and every case keeps lod at least 0.2 from a boundary.)
NXTEST(mip_lod_bias_and_clamp) {
    if (!real_data_available()) { nxtest::skip("work/shader_cache not found"); return; }
    App app;
    App::Opts o;
    o.width = kW;
    o.height = kH;
    REQUIRE(app.init(o));
    App::ShaderBlob vs, fs;
    REQUIRE(load_quad_shaders(&vs, &fs));
    auto prog = app.make_program({&vs, &fs});
    REQUIRE(prog != nullptr);
    auto tex = app.make_texture_2d(NVN_FORMAT_RGBA8, 8, 8, 0, 3);
    REQUIRE(tex != nullptr);
    const Lay lay = make_layout(8, 8, 3, 1, 1, 4);
    std::vector<uint8_t> lin(lay.L.linear_size, 0);
    const uint8_t red[4] = {255, 0, 0, 255}, green[4] = {0, 255, 0, 255}, blue[4] = {0, 0, 255, 255};
    fill_level_rgba8(lin, lay, 0, red);
    fill_level_rgba8(lin, lay, 1, green);
    fill_level_rgba8(lin, lay, 2, blue);
    REQUIRE(store_linear(*tex, lay, lin));

    auto mk = [&](float bias, float lod_min, float lod_max) {
        return app.make_sampler([&](NVNsamplerBuilder& sb) {
            app.api.nvnSamplerBuilderSetMinMagFilter(&sb, NVN_MIN_FILTER_NEAREST_MIPMAP_NEAREST, NVN_MAG_FILTER_NEAREST);
            app.api.nvnSamplerBuilderSetWrapMode(&sb, NVN_WRAP_MODE_CLAMP_TO_EDGE, NVN_WRAP_MODE_CLAMP_TO_EDGE, NVN_WRAP_MODE_CLAMP_TO_EDGE);
            app.api.nvnSamplerBuilderSetLodBias(&sb, bias);
            app.api.nvnSamplerBuilderSetLodClamp(&sb, lod_min, lod_max);
        });
    };
    const uint64_t th_nom = app.texture_handle(tex->id, mk(0.0f, 0.0f, 1000.0f));
    const uint64_t th_bias = app.texture_handle(tex->id, mk(1.0f, 0.0f, 1000.0f));
    const uint64_t th_cmax = app.texture_handle(tex->id, mk(0.0f, 0.0f, 0.3f));
    const uint64_t th_cmin = app.texture_handle(tex->id, mk(0.0f, 1.7f, 1000.0f));

    auto cb = app.make_cmdbuf();
    REQUIRE(cb != nullptr);
    NVNcommandBuffer* c = cb->obj.get();
    const int idx = app.acquire();
    REQUIRE(idx >= 0);
    app.api.nvnCommandBufferBeginRecording(c);
    bind_common(app, c);
    record_pass(app, c, app.window_textures[idx]->obj.get(), prog.get(), kW, kH);
    draw_quad(app, c, th_nom, 0, 0, 4, 4, 0, 0, 1, 1);
    draw_quad(app, c, th_bias, 4, 0, 4, 4, 0, 0, 1, 1);
    draw_quad(app, c, th_cmax, 8, 0, 4, 4, 0, 0, 1, 1);
    draw_quad(app, c, th_cmin, 12, 0, 4, 4, 0, 0, 1, 1);
    app.end_and_submit(*cb);
    std::vector<uint8_t> px;
    REQUIRE(app.present_and_capture(idx, &px));

    const Rgbf g{0, 255, 0}, b{0, 0, 255}, r{255, 0, 0};
    const Cmp c0 = compare_rect(px, 0, 0, 4, 4, [&](uint32_t, uint32_t) { return g; }, 1);
    const Cmp c1 = compare_rect(px, 4, 0, 4, 4, [&](uint32_t, uint32_t) { return b; }, 1);
    const Cmp c2 = compare_rect(px, 8, 0, 4, 4, [&](uint32_t, uint32_t) { return r; }, 1);
    const Cmp c3 = compare_rect(px, 12, 0, 4, 4, [&](uint32_t, uint32_t) { return b; }, 1);
    report("nominal LOD 1: green (level 1)", c0, 1);
    report("bias +1: blue (level 2)", c1, 1);
    report("clamp max 0.3: red (level 0)", c2, 1);
    report("clamp min 1.7: blue (level 2)", c3, 1);
    CHECK_EQ(c0.bad, 0);
    CHECK_EQ(c1.bad, 0);
    CHECK_EQ(c2.bad, 0);
    CHECK_EQ(c3.bad, 0);
    check_clean(app);
}

// Scene 5: render to a 64x48 RGBA8 texture (a magenta triangle), then sample that texture onto the window. The window must show the
// triangle and equal the texture read back after the queue finishes.
NXTEST(render_to_texture_then_sample) {
    if (!real_data_available()) { nxtest::skip("work/shader_cache not found"); return; }
    App app;
    App::Opts o;
    o.width = kW;
    o.height = kH;
    REQUIRE(app.init(o));
    App::ShaderBlob tvs, tfs, qvs, qfs;
    REQUIRE(App::load_shader("a656883b0cdb8ebc", &tvs));   // vertex: position from attribute 0
    REQUIRE(App::load_shader("062cfa5657f5388d", &tfs));   // fragment: constant magenta (1, 0, 1, 1)
    REQUIRE(load_quad_shaders(&qvs, &qfs));
    auto tri = app.make_program({&tvs, &tfs});
    auto quad = app.make_program({&qvs, &qfs});
    REQUIRE(tri != nullptr && quad != nullptr);
    auto rt = app.make_texture_2d(NVN_FORMAT_RGBA8, kW, kH);
    REQUIRE(rt != nullptr);
    const uint64_t th = app.texture_handle(rt->id, make_sampler_nearest(app));

    Vtx tv[3];
    const float tp[3][3] = {{-0.5f, -0.5f, 0.0f}, {0.5f, -0.5f, 0.0f}, {0.0f, 0.5f, 0.0f}};
    for (int i = 0; i < 3; ++i) {
        for (int k = 0; k < 3; ++k) tv[i].p[k] = tp[i][k];
        for (int k = 0; k < 4; ++k) tv[i].t[k] = 0.0f;
    }
    App::Buf tb = app.make_buffer(sizeof tv, tv);

    auto cb = app.make_cmdbuf();
    REQUIRE(cb != nullptr);
    NVNcommandBuffer* c = cb->obj.get();
    const int idx = app.acquire();
    REQUIRE(idx >= 0);
    app.api.nvnCommandBufferBeginRecording(c);
    bind_common(app, c);
    record_pass(app, c, rt->obj.get(), tri.get(), kW, kH);
    app.api.nvnCommandBufferBindVertexBuffer(c, 0, tb.address, tb.size);
    app.api.nvnCommandBufferDrawArrays(c, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3);
    record_pass(app, c, app.window_textures[idx]->obj.get(), quad.get(), kW, kH);
    draw_quad(app, c, th, 0, 0, kW, kH, 0, 0, 1, 1);
    app.end_and_submit(*cb);
    std::vector<uint8_t> px;
    REQUIRE(app.present_and_capture(idx, &px));
    app.finish();

    std::vector<uint8_t> rtpx(static_cast<size_t>(kW) * kH * 4, 0);
    NVNcopyRegion full{0, 0, 0, static_cast<int>(kW), static_cast<int>(kH), 1};
    app.api.nvnTextureReadTexels(rt->obj.get(), nullptr, &full, rtpx.data());
    const Cmp same = compare_rect(px, 0, 0, kW, kH, [&](uint32_t x, uint32_t y) {
        const size_t i = (static_cast<size_t>(y) * kW + x) * 4;
        return Rgbf{static_cast<double>(rtpx[i]), static_cast<double>(rtpx[i + 1]), static_cast<double>(rtpx[i + 2])};
    }, 0);
    report("window vs render target readback (exact)", same, 0);
    const Rgb inside = at(px, 32, 28), above = at(px, 32, 6), below = at(px, 32, 42), left = at(px, 8, 28);
    std::printf("    inside %d %d %d, above apex %d %d %d, below base %d %d %d, far left %d %d %d\n", inside.r, inside.g, inside.b, above.r, above.g,
                above.b, below.r, below.g, below.b, left.r, left.g, left.b);
    size_t magenta = 0;
    for (uint32_t y = 0; y < kH; ++y)
        for (uint32_t x = 0; x < kW; ++x) {
            const Rgb p = at(px, x, y);
            if (p.r == 255 && p.g == 0 && p.b == 255) ++magenta;
        }
    std::printf("    magenta pixels %zu (the triangle scene counts 360..410)\n", magenta);
    CHECK_EQ(same.bad, 0);
    CHECK(inside.r == 255 && inside.g == 0 && inside.b == 255);
    CHECK(above.r == 0 && above.g == 0 && above.b == 0);
    CHECK(below.r == 0 && below.g == 0 && below.b == 0);
    CHECK(left.r == 0 && left.g == 0 && left.b == 0);
    CHECK(magenta > 360 && magenta < 410);
    check_clean(app);
    if (same.bad == 0 && magenta > 360 && magenta < 410) reference_check("texture_render_to_texture", px);
}

// S4g: the game makes several texture objects over one piece of memory (S4f: they share one image). A draw that samples one of them while another
// is the colour target of the pass is a feedback loop all the same. It must sample a copy of the image made before the draw (the mip chain of
// the environment map is built that way, one object per level), and the check has to find the loop when the object that is the target has not
// had its image yet: the sampled object already owns the image, the target object gets it only when the pass begins. Before that was handled the
// binding got the dummy texture and the chain stayed black; validation had reported VUID-vkCmdDraw-imageLayout-00344 even earlier.
// Here X (holds the pattern) is sampled first, so it owns the image. The pass then renders into Y, a new object over the same storage, mirroring
// X left to right; the window shows Y afterwards.
NXTEST(sampling_an_alias_of_the_pass_target_reads_the_copy_made_before_the_draw) {
    if (!real_data_available()) { nxtest::skip("work/shader_cache not found"); return; }
    App app;
    App::Opts o;
    o.width = kW;
    o.height = kH;
    REQUIRE(app.init(o));
    App::ShaderBlob qvs, qfs;
    REQUIRE(load_quad_shaders(&qvs, &qfs));
    auto quad = app.make_program({&qvs, &qfs});
    REQUIRE(quad != nullptr);
    auto x = app.make_texture_2d(NVN_FORMAT_RGBA8, 16, 16);
    REQUIRE(x != nullptr);
    REQUIRE(store_linear(*x, make_layout(16, 16, 1, 1, 1, 4), pattern16_linear()));
    const uint64_t th_x = app.texture_handle(x->id, make_sampler_nearest(app));

    // Y: a second texture object over the storage of X; the target of the pass
    auto y = std::make_unique<Tex>();
    y->obj = std::make_unique<NVNtexture>();
    std::memset(y->obj.get(), 0, sizeof(NVNtexture));
    NVNtextureBuilder b;
    app.api.nvnTextureBuilderSetDevice(&b, app.device.get());
    app.api.nvnTextureBuilderSetDefaults(&b);
    app.api.nvnTextureBuilderSetFlags(&b, 0);
    app.api.nvnTextureBuilderSetTarget(&b, NVN_TEXTURE_TARGET_2D);
    app.api.nvnTextureBuilderSetSize2D(&b, 16, 16);
    app.api.nvnTextureBuilderSetFormat(&b, NVN_FORMAT_RGBA8);
    app.api.nvnTextureBuilderSetLevels(&b, 1);
    app.api.nvnTextureBuilderSetStorage(&b, x->pool->obj.get(), static_cast<ptrdiff_t>(x->offset));
    REQUIRE(app.api.nvnTextureInitialize(y->obj.get(), &b));
    y->id = app.next_tex_id++;
    app.api.nvnTexturePoolRegisterTexture(app.tex_pool.get(), y->id, y->obj.get(), nullptr);
    const uint64_t th_y = app.texture_handle(y->id, make_sampler_nearest(app));

    // first submission: X is sampled onto the window, so X owns its image (and the pattern is checked on the way)
    {
        auto cb0 = app.make_cmdbuf();
        REQUIRE(cb0 != nullptr);
        NVNcommandBuffer* c0 = cb0->obj.get();
        const int i0 = app.acquire();
        REQUIRE(i0 >= 0);
        app.api.nvnCommandBufferBeginRecording(c0);
        bind_common(app, c0);
        record_pass(app, c0, app.window_textures[i0]->obj.get(), quad.get(), kW, kH);
        draw_quad(app, c0, th_x, 0, 0, kW, kH, 0, 0, 1, 1);
        app.end_and_submit(*cb0);
        std::vector<uint8_t> px0;
        REQUIRE(app.present_and_capture(i0, &px0));
        const Cmp c0cmp = compare_rect(px0, 0, 0, kW, kH, pattern16_frame, 0);
        report("X sampled first (exact)", c0cmp, 0);
        CHECK_EQ(c0cmp.bad, 0);
    }
    // second submission: the pass renders into Y, a new object over the same storage, mirroring X; the window then shows Y
    auto cb = app.make_cmdbuf();
    REQUIRE(cb != nullptr);
    NVNcommandBuffer* c = cb->obj.get();
    const int idx = app.acquire();
    REQUIRE(idx >= 0);
    app.api.nvnCommandBufferBeginRecording(c);
    bind_common(app, c);
    {   // the pass on Y without the clear that record_pass records: the draw must see what the image holds
        NVNtexture* targets[] = {y->obj.get()};
        app.api.nvnCommandBufferSetRenderTargets(c, 1, targets, nullptr, nullptr, nullptr);
        app.api.nvnCommandBufferSetViewport(c, 0, 0, 16, 16);
        app.api.nvnCommandBufferSetScissor(c, 0, 0, 16, 16);
        app.api.nvnCommandBufferBindProgram(c, quad.get(), NVN_SHADER_STAGE_VERTEX_BIT | NVN_SHADER_STAGE_FRAGMENT_BIT);
    }
    draw_quad(app, c, th_x, 0, 0, 16, 16, 1, 0, 0, 1);        // X, mirrored, into Y: the same image is sampled and rendered to
    record_pass(app, c, app.window_textures[idx]->obj.get(), quad.get(), kW, kH);
    draw_quad(app, c, th_y, 0, 0, kW, kH, 0, 0, 1, 1);
    app.end_and_submit(*cb);
    std::vector<uint8_t> px;
    REQUIRE(app.present_and_capture(idx, &px));
    app.finish();
    const Cmp cmp = compare_rect(px, 0, 0, kW, kH, [](uint32_t px_x, uint32_t px_y) { return pattern16_frame(kW - 1 - px_x, px_y); }, 0);
    report("frame vs the mirrored pattern (exact)", cmp, 0);
    CHECK_EQ(cmp.bad, 0);
    check_clean(app);
}

// Scene 6a: CopyBufferToTexture from a linear buffer with a padded row stride (256 bytes for 64 bytes of texels).
NXTEST(copy_buffer_to_texture_with_row_stride) {
    if (!real_data_available()) { nxtest::skip("work/shader_cache not found"); return; }
    App app;
    App::Opts o;
    o.width = kW;
    o.height = kH;
    REQUIRE(app.init(o));
    App::ShaderBlob vs, fs;
    REQUIRE(load_quad_shaders(&vs, &fs));
    auto prog = app.make_program({&vs, &fs});
    REQUIRE(prog != nullptr);
    const int stride = 256;
    App::Buf src = app.make_buffer(16 * stride);
    std::memset(src.ptr, 0xEE, 16 * stride);   // padding must not reach the texture
    const std::vector<uint8_t> lin = pattern16_linear();
    for (int y = 0; y < 16; ++y) std::memcpy(src.ptr + y * stride, &lin[static_cast<size_t>(y) * 64], 64);
    auto tex = app.make_texture_2d(NVN_FORMAT_RGBA8, 16, 16);
    REQUIRE(tex != nullptr);
    const uint64_t th = app.texture_handle(tex->id, make_sampler_nearest(app));

    auto cb = app.make_cmdbuf();
    REQUIRE(cb != nullptr);
    NVNcommandBuffer* c = cb->obj.get();
    const int idx = app.acquire();
    REQUIRE(idx >= 0);
    app.api.nvnCommandBufferBeginRecording(c);
    bind_common(app, c);
    app.api.nvnCommandBufferSetCopyRowStride(c, stride);
    NVNcopyRegion reg{0, 0, 0, 16, 16, 1};
    app.api.nvnCommandBufferCopyBufferToTexture(c, src.address, tex->obj.get(), nullptr, &reg, NVN_COPY_FLAGS_NONE);
    record_pass(app, c, app.window_textures[idx]->obj.get(), prog.get(), kW, kH);
    draw_quad(app, c, th, 0, 0, kW, kH, 0, 0, 1, 1);
    app.end_and_submit(*cb);
    std::vector<uint8_t> px;
    REQUIRE(app.present_and_capture(idx, &px));
    const Cmp cmp = compare_rect(px, 0, 0, kW, kH, pattern16_frame, 0);
    report("frame vs pattern after the copy (exact)", cmp, 0);
    CHECK_EQ(cmp.bad, 0);
    check_clean(app);
}

// Scene 6b: CopyTextureToBuffer, read back after the queue finishes and compared byte for byte.
NXTEST(copy_texture_to_buffer_read_back) {
    App app;
    App::Opts o;
    o.width = kW;
    o.height = kH;
    REQUIRE(app.init(o));
    auto tex = app.make_texture_2d(NVN_FORMAT_RGBA8, 16, 16);
    REQUIRE(tex != nullptr);
    REQUIRE(store_linear(*tex, make_layout(16, 16, 1, 1, 1, 4), pattern16_linear()));
    App::Buf dst = app.make_buffer(16 * 64);
    std::memset(dst.ptr, 0xEE, 16 * 64);

    auto cb = app.make_cmdbuf();
    REQUIRE(cb != nullptr);
    NVNcommandBuffer* c = cb->obj.get();
    app.api.nvnCommandBufferBeginRecording(c);
    app.api.nvnCommandBufferSetCopyRowStride(c, 64);
    NVNcopyRegion reg{0, 0, 0, 16, 16, 1};
    app.api.nvnCommandBufferCopyTextureToBuffer(c, tex->obj.get(), nullptr, &reg, dst.address, NVN_COPY_FLAGS_NONE);
    app.end_and_submit(*cb);
    app.finish();

    const std::vector<uint8_t> lin = pattern16_linear();
    size_t bad = 0;
    for (size_t i = 0; i < lin.size(); ++i) bad += dst.ptr[i] != lin[i] ? 1 : 0;
    std::printf("    %zu of %zu bytes differ from the texture\n", bad, lin.size());
    CHECK_EQ(bad, 0u);
    check_clean(app);
}

// Scene 6c: CopyTextureToTexture: an unscaled copy (exact) and a scaled copy 16x16 -> 8x8 with the linear filter bit (each texel is
// the average of a 2x2 block of the source, within 2 levels). Both copies are sampled onto the window.
NXTEST(copy_texture_to_texture_unscaled_and_scaled) {
    if (!real_data_available()) { nxtest::skip("work/shader_cache not found"); return; }
    App app;
    App::Opts o;
    o.width = kW;
    o.height = kH;
    REQUIRE(app.init(o));
    App::ShaderBlob vs, fs;
    REQUIRE(load_quad_shaders(&vs, &fs));
    auto prog = app.make_program({&vs, &fs});
    REQUIRE(prog != nullptr);
    auto src = app.make_texture_2d(NVN_FORMAT_RGBA8, 16, 16);
    auto dst16 = app.make_texture_2d(NVN_FORMAT_RGBA8, 16, 16);
    auto dst8 = app.make_texture_2d(NVN_FORMAT_RGBA8, 8, 8);
    auto dst8_nf = app.make_texture_2d(NVN_FORMAT_RGBA8, 8, 8, 0, 1, false);   // scaled copy without the filter bit, read back
    REQUIRE(src != nullptr && dst16 != nullptr && dst8 != nullptr && dst8_nf != nullptr);
    REQUIRE(store_linear(*src, make_layout(16, 16, 1, 1, 1, 4), pattern16_linear()));
    const uint64_t th16 = app.texture_handle(dst16->id, make_sampler_nearest(app));
    const uint64_t th8 = app.texture_handle(dst8->id, make_sampler_nearest(app));

    auto cb = app.make_cmdbuf();
    REQUIRE(cb != nullptr);
    NVNcommandBuffer* c = cb->obj.get();
    const int idx = app.acquire();
    REQUIRE(idx >= 0);
    app.api.nvnCommandBufferBeginRecording(c);
    bind_common(app, c);
    NVNcopyRegion r16{0, 0, 0, 16, 16, 1}, r8{0, 0, 0, 8, 8, 1};
    app.api.nvnCommandBufferCopyTextureToTexture(c, src->obj.get(), nullptr, &r16, dst16->obj.get(), nullptr, &r16, NVN_COPY_FLAGS_NONE);
    app.api.nvnCommandBufferCopyTextureToTexture(c, src->obj.get(), nullptr, &r16, dst8->obj.get(), nullptr, &r8, NVN_COPY_FLAGS_LINEAR_FILTER_BIT);
    app.api.nvnCommandBufferCopyTextureToTexture(c, src->obj.get(), nullptr, &r16, dst8_nf->obj.get(), nullptr, &r8, NVN_COPY_FLAGS_NONE);
    record_pass(app, c, app.window_textures[idx]->obj.get(), prog.get(), kW, kH);
    draw_quad(app, c, th16, 0, 0, 32, kH, 0, 0, 1, 1);
    draw_quad(app, c, th8, 32, 0, 32, kH, 0, 0, 1, 1);
    app.end_and_submit(*cb);
    std::vector<uint8_t> px;
    REQUIRE(app.present_and_capture(idx, &px));
    app.finish();

    // diagnostic: the scaled copy without the filter bit, read back; each texel is compared with the nearest source texels and the average
    std::vector<uint8_t> nf(8 * 8 * 4, 0);
    NVNcopyRegion full8{0, 0, 0, 8, 8, 1};
    app.api.nvnTextureReadTexels(dst8_nf->obj.get(), nullptr, &full8, nf.data());
    int as_avg = 0, as_nearest = 0, other = 0;
    for (int j = 0; j < 8; ++j)
        for (int i = 0; i < 8; ++i) {
            const Rgb avg_c = [&] {
                int sr = 0, sg = 0, sb = 0;
                for (int dy = 0; dy < 2; ++dy)
                    for (int dx = 0; dx < 2; ++dx) {
                        const Rgb cc = pattern16(2 * i + dx, 2 * j + dy);
                        sr += cc.r;
                        sg += cc.g;
                        sb += cc.b;
                    }
                return Rgb{(sr + 2) / 4, (sg + 2) / 4, (sb + 2) / 4};
            }();
            const Rgb p = {nf[(j * 8 + i) * 4], nf[(j * 8 + i) * 4 + 1], nf[(j * 8 + i) * 4 + 2]};
            auto nearby = [&](int dx, int dy) {
                const Rgb cc = pattern16(2 * i + dx, 2 * j + dy);
                return std::abs(p.r - cc.r) <= 1 && std::abs(p.g - cc.g) <= 1 && std::abs(p.b - cc.b) <= 1;
            };
            if (std::abs(p.r - avg_c.r) <= 2 && std::abs(p.g - avg_c.g) <= 2 && std::abs(p.b - avg_c.b) <= 2) ++as_avg;
            else if (nearby(0, 0) || nearby(1, 0) || nearby(0, 1) || nearby(1, 1)) ++as_nearest;
            else ++other;
        }
    std::printf("    diagnostic, scaled copy with flags NONE: %d texels average, %d texels equal a source texel, %d other (of 64)\n", as_avg, as_nearest, other);

    // unscaled: the 16x16 texture shown over the left 32 x 48 px, texel (x / 2, y / 3)
    const Cmp unscaled = compare_rect(px, 0, 0, 32, kH, [&](uint32_t x, uint32_t y) {
        const Rgb cc = pattern16(static_cast<int>(x / 2), static_cast<int>(y / 3));
        return Rgbf{static_cast<double>(cc.r), static_cast<double>(cc.g), static_cast<double>(cc.b)};
    }, 0);
    // scaled: texel (i, j) of the 8x8 target is the mean of source texels (2i..2i+1, 2j..2j+1); shown over the right 32 x 48 px: texel ((x - 32) / 4, y / 6)
    const Cmp scaled = compare_rect(px, 32, 0, 32, kH, [&](uint32_t x, uint32_t y) {
        const int i = static_cast<int>((x - 32) / 4), j = static_cast<int>(y / 6);
        int sr = 0, sg = 0, sb = 0;
        for (int dy = 0; dy < 2; ++dy)
            for (int dx = 0; dx < 2; ++dx) {
                const Rgb cc = pattern16(2 * i + dx, 2 * j + dy);
                sr += cc.r;
                sg += cc.g;
                sb += cc.b;
            }
        return Rgbf{sr / 4.0, sg / 4.0, sb / 4.0};
    }, 2);
    report("unscaled copy 16x16 (exact)", unscaled, 0);
    report("scaled copy 16x16 -> 8x8, linear (+-2)", scaled, 2);
    CHECK_EQ(unscaled.bad, 0);
    CHECK_EQ(scaled.bad, 0);
    check_clean(app);
}

// Scene 4: texture views on one RGBA8 texture (3 solid levels: (200,100,50,64), (10,220,30,255), (90,90,90,255)). Four strips of
// 16 px: the base view, a swizzle view (R <- A, A <- R), a view of level 1 only, and an sRGB format view of the base level.
NXTEST(texture_views_swizzle_levels_srgb) {
    if (!real_data_available()) { nxtest::skip("work/shader_cache not found"); return; }
    App app;
    App::Opts o;
    o.width = kW;
    o.height = kH;
    REQUIRE(app.init(o));
    App::ShaderBlob vs, fs;
    REQUIRE(load_quad_shaders(&vs, &fs));
    auto prog = app.make_program({&vs, &fs});
    REQUIRE(prog != nullptr);
    auto tex = app.make_texture_2d(NVN_FORMAT_RGBA8, 8, 8, 0, 3);
    REQUIRE(tex != nullptr);
    const Lay lay = make_layout(8, 8, 3, 1, 1, 4);
    std::vector<uint8_t> lin(lay.L.linear_size, 0);
    const uint8_t l0[4] = {200, 100, 50, 64}, l1[4] = {10, 220, 30, 255}, l2[4] = {90, 90, 90, 255};
    fill_level_rgba8(lin, lay, 0, l0);
    fill_level_rgba8(lin, lay, 1, l1);
    fill_level_rgba8(lin, lay, 2, l2);
    REQUIRE(store_linear(*tex, lay, lin));

    NVNtextureView swz, lvl, srgb;
    app.api.nvnTextureViewSetDefaults(&swz);
    app.api.nvnTextureViewSetSwizzle(&swz, NVN_TEXTURE_SWIZZLE_A, NVN_TEXTURE_SWIZZLE_G, NVN_TEXTURE_SWIZZLE_B, NVN_TEXTURE_SWIZZLE_R);
    app.api.nvnTextureViewSetDefaults(&lvl);
    app.api.nvnTextureViewSetLevels(&lvl, 1, 1);
    app.api.nvnTextureViewSetDefaults(&srgb);
    app.api.nvnTextureViewSetFormat(&srgb, NVN_FORMAT_RGBA8_SRGB);
    app.api.nvnTexturePoolRegisterTexture(app.tex_pool.get(), 1, tex->obj.get(), &swz);
    app.api.nvnTexturePoolRegisterTexture(app.tex_pool.get(), 2, tex->obj.get(), &lvl);
    app.api.nvnTexturePoolRegisterTexture(app.tex_pool.get(), 3, tex->obj.get(), &srgb);
    const int smp = make_sampler_nearest(app);
    const uint64_t th0 = app.texture_handle(0, smp), th1 = app.texture_handle(1, smp), th2 = app.texture_handle(2, smp), th3 = app.texture_handle(3, smp);

    auto cb = app.make_cmdbuf();
    REQUIRE(cb != nullptr);
    NVNcommandBuffer* c = cb->obj.get();
    const int idx = app.acquire();
    REQUIRE(idx >= 0);
    app.api.nvnCommandBufferBeginRecording(c);
    bind_common(app, c);
    record_pass(app, c, app.window_textures[idx]->obj.get(), prog.get(), kW, kH);
    draw_quad(app, c, th0, 0, 0, 16, kH, 0, 0, 1, 1);
    draw_quad(app, c, th1, 16, 0, 16, kH, 0, 0, 1, 1);
    draw_quad(app, c, th2, 32, 0, 16, kH, 0, 0, 1, 1);
    draw_quad(app, c, th3, 48, 0, 16, kH, 0, 0, 1, 1);
    app.end_and_submit(*cb);
    std::vector<uint8_t> px;
    REQUIRE(app.present_and_capture(idx, &px));

    auto lin_of = [](int v) { return srgb_to_linear_byte(v); };
    const Cmp base = compare_rect(px, 0, 0, 16, kH, [](uint32_t, uint32_t) { return Rgbf{200, 100, 50}; }, 0);
    const Cmp swzc = compare_rect(px, 16, 0, 16, kH, [](uint32_t, uint32_t) { return Rgbf{64, 100, 50}; }, 0);
    const Cmp lvlc = compare_rect(px, 32, 0, 16, kH, [](uint32_t, uint32_t) { return Rgbf{10, 220, 30}; }, 0);
    const Cmp srgbc = compare_rect(px, 48, 0, 16, kH, [&](uint32_t, uint32_t) { return Rgbf{lin_of(200), lin_of(100), lin_of(50)}; }, 2);
    report("base view (exact)", base, 0);
    report("swizzle view R<-A, A<-R (exact)", swzc, 0);
    report("level 1 view (exact)", lvlc, 0);
    report("sRGB format view (+-2)", srgbc, 2);
    CHECK_EQ(base.bad, 0);
    CHECK_EQ(swzc.bad, 0);
    CHECK_EQ(lvlc.bad, 0);
    CHECK_EQ(srgbc.bad, 0);
    check_clean(app);
}

// Scene 7: cube maps and 2D arrays. Not run: the translated shader set has a single-texture 2D-array fragment (7536db62a01ca751) and a
// single-texture cube fragment (4afead41f699131e), but both also need uniform data (cb3) and a vertex shader with three or more
// varyings (colour, scale, uv + layer) that no small corpus vertex shader provides in a form that passes the quad through. Recorded
// here so the gap is visible; see the report.
NXTEST(cube_and_array_textures_not_run) {
    nxtest::skip("no vertex shader pairs with the single-texture 2D-array (7536db62a01ca751) or cube (4afead41f699131e) fragment shader");
}

