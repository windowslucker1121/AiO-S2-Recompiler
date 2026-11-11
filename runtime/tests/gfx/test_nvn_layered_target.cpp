// SPDX-License-Identifier: GPL-3.0-or-later
// NVN scene oracles, layered rendering: a cube map target set with nvnCommandBufferSetRenderTargets and no texture view is attached
// with all six of its layers (nvn_replay.cpp make_target, ensure_pass), so a geometry shader that writes gl_Layer draws into every face.
// The geometry is six quads covering the target; quad k carries layer k in attribute 1 (u, v, k) and the geometry shader
// (80271a630230f99a) takes gl_Layer from attribute 1 z of the first vertex of each triangle. Before the layered-target fix only
// layer 0 was attached, so faces 1..5 were never drawn into and stayed clear: case 1 fails without that fix.
// Case 2 uses a passthrough geometry shader: it copies the Location 0 input to the Location 0 output (nvn_spirv_patch.cpp
// spirv_gs_passthrough), so a fragment shader that reads Location 0 sees the interpolated (u, v, layer, 1) of each face.
// Every scene uses REAL translated shaders of a game (work/shader_cache) and reads the cube back per face.
// Skipped when the shader data is not on this machine.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>

#include "nvn_app.h"
NXTEST_GROUP(gfx_nvn_layered_target)

using namespace nvnapp;

namespace {

constexpr int kSide = 8;   // every cube face is kSide x kSide texels, one level

// Six quads covering the whole NDC square: quad k is layer k. Vertex = position (x, y, 0), attribute 1 = (u, v, k): 24 bytes.
// Two triangles per quad, 36 indices in all.
struct Geometry {
    std::vector<float> verts;    // 24 vertices x 6 floats
    std::vector<uint16_t> idx;   // 36 indices
};
Geometry cube_geometry() {
    Geometry g;
    const float corner[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
    for (int q = 0; q < 6; ++q) {
        for (int i = 0; i < 4; ++i) {
            const float x = corner[i][0], y = corner[i][1];
            g.verts.insert(g.verts.end(), {x, y, 0.0f, (x + 1) * 0.5f, (y + 1) * 0.5f, static_cast<float>(q)});
        }
        const uint16_t b = static_cast<uint16_t>(4 * q);
        g.idx.insert(g.idx.end(), {b, static_cast<uint16_t>(b + 1), static_cast<uint16_t>(b + 2), b, static_cast<uint16_t>(b + 2),
                                   static_cast<uint16_t>(b + 3)});
    }
    return g;
}

// A cube map target: six faces of kSide x kSide, one level, no view. The back end attaches every face to a render pass that uses it.
std::unique_ptr<Tex> make_cube(App& app, int format, int flags) {
    auto t = std::make_unique<Tex>();
    t->obj = std::make_unique<NVNtexture>();
    std::memset(t->obj.get(), 0, sizeof(NVNtexture));
    auto& api = app.api;
    NVNtextureBuilder b;
    api.nvnTextureBuilderSetDevice(&b, app.device.get());
    api.nvnTextureBuilderSetDefaults(&b);
    api.nvnTextureBuilderSetFlags(&b, flags);
    api.nvnTextureBuilderSetTarget(&b, NVN_TEXTURE_TARGET_CUBEMAP);
    api.nvnTextureBuilderSetSize2D(&b, kSide, kSide);
    api.nvnTextureBuilderSetFormat(&b, static_cast<NVNformat>(format));
    const size_t size = api.nvnTextureBuilderGetStorageSize(&b), align = api.nvnTextureBuilderGetStorageAlignment(&b);
    t->pool = app.pool_main.get();
    t->offset = t->pool->take(size, std::max<size_t>(align, 256));
    t->size = size;
    t->width = kSide;
    t->height = kSide;
    t->format = format;
    api.nvnTextureBuilderSetStorage(&b, t->pool->obj.get(), static_cast<ptrdiff_t>(t->offset));
    if (!api.nvnTextureInitialize(t->obj.get(), &b)) return nullptr;
    return t;
}

// The render state of the cube draws: NVN defaults, back-face culling off (the winding of the quads does not matter), attribute 0 =
// position at offset 0 and attribute 1 at offset 12, both on stream 0 with stride 24. The caller keeps it alive while recording:
// the bindings take pointers to these structs.
struct CubeState {
    NVNblendState bs;
    NVNcolorState cs;
    NVNchannelMaskState cm;
    NVNpolygonState ps;
    NVNdepthStencilState ds;
    NVNmultisampleState mss;
    NVNvertexAttribState attr[2];
    NVNvertexStreamState stream;
    void defaults(App& app) {
        auto& api = app.api;
        api.nvnBlendStateSetDefaults(&bs);
        api.nvnColorStateSetDefaults(&cs);
        api.nvnChannelMaskStateSetDefaults(&cm);
        api.nvnPolygonStateSetDefaults(&ps);
        api.nvnPolygonStateSetCullFace(&ps, NVN_FACE_NONE);
        api.nvnDepthStencilStateSetDefaults(&ds);
        api.nvnMultisampleStateSetDefaults(&mss);
        api.nvnVertexAttribStateSetDefaults(&attr[0]);
        api.nvnVertexAttribStateSetFormat(&attr[0], NVN_FORMAT_RGB32F, 0);
        api.nvnVertexAttribStateSetStreamIndex(&attr[0], 0);
        api.nvnVertexAttribStateSetDefaults(&attr[1]);
        api.nvnVertexAttribStateSetFormat(&attr[1], NVN_FORMAT_RGB32F, 12);
        api.nvnVertexAttribStateSetStreamIndex(&attr[1], 0);
        api.nvnVertexStreamStateSetDefaults(&stream);
        api.nvnVertexStreamStateSetStride(&stream, 24);
    }
    void bind(App& app, NVNcommandBuffer* c) {
        auto& api = app.api;
        api.nvnCommandBufferBindBlendState(c, &bs);
        api.nvnCommandBufferBindColorState(c, &cs);
        api.nvnCommandBufferBindChannelMaskState(c, &cm);
        api.nvnCommandBufferBindPolygonState(c, &ps);
        api.nvnCommandBufferBindDepthStencilState(c, &ds);
        api.nvnCommandBufferBindMultisampleState(c, &mss);
        api.nvnCommandBufferBindVertexAttribState(c, 2, attr);
        api.nvnCommandBufferBindVertexStreamState(c, 1, &stream);
    }
};

// Clears the cube to (0,0,0,0), then draws the 36 indices of the six quads with the program. The cube is the only render target and
// is set without a view, so every face is attached (the layered-target path under test).
bool render_cube(App& app, Tex& cube, NVNprogram* prog, App::Buf vb, App::Buf ib) {
    auto& api = app.api;
    auto cb = app.make_cmdbuf();
    if (!cb) return false;
    NVNcommandBuffer* c = cb->obj.get();
    NVNtexture* targets[] = {cube.obj.get()};
    CubeState st;
    st.defaults(app);
    api.nvnCommandBufferBeginRecording(c);
    api.nvnCommandBufferSetRenderTargets(c, 1, targets, nullptr, nullptr, nullptr);
    api.nvnCommandBufferSetViewport(c, 0, 0, kSide, kSide);
    api.nvnCommandBufferSetScissor(c, 0, 0, kSide, kSide);
    const float clear[4] = {0, 0, 0, 0};
    api.nvnCommandBufferClearColor(c, 0, clear, NVN_CLEAR_COLOR_MASK_RGBA);
    st.bind(app, c);
    api.nvnCommandBufferBindVertexBuffer(c, 0, vb.address, vb.size);
    api.nvnCommandBufferBindProgram(c, prog, NVN_SHADER_STAGE_VERTEX_BIT | NVN_SHADER_STAGE_GEOMETRY_BIT | NVN_SHADER_STAGE_FRAGMENT_BIT);
    api.nvnCommandBufferDrawElements(c, NVN_DRAW_PRIMITIVE_TRIANGLES, NVN_INDEX_TYPE_UNSIGNED_SHORT, 36, ib.address);
    app.end_and_submit(*cb);
    app.finish();
    return true;
}

// Reads one face of the cube (bytes_per_texel per texel, rows tight). A view that names the layer selects the face: for cube maps the
// view's layer is the slice that is read.
void read_face(App& app, Tex& cube, int face, size_t bytes_per_texel, std::vector<uint8_t>& out) {
    NVNtextureView v;
    std::memset(&v, 0, sizeof v);
    app.api.nvnTextureViewSetDefaults(&v);
    app.api.nvnTextureViewSetLayers(&v, face, 1);
    NVNcopyRegion full{0, 0, 0, kSide, kSide, 1};
    out.assign(static_cast<size_t>(kSide) * kSide * bytes_per_texel, 0);
    app.api.nvnTextureReadTexels(cube.obj.get(), &v, &full, out.data());
}

}  // namespace

NXTEST(cube_target_all_layers_are_drawn) {
    if (!real_data_available()) { nxtest::skip("work/shader_cache not found"); return; }
    App app;
    REQUIRE(app.init());
    App::ShaderBlob vs, gs, fs;
    REQUIRE(App::load_shader("38f38f70414e8efe", &vs));   // vertex: position = attribute 0 (w = 1), Location 0 = attribute 1
    REQUIRE(App::load_shader("80271a630230f99a", &gs));   // geometry: gl_Position passthrough, gl_Layer = trunc(input Location 0 .z)
    REQUIRE(App::load_shader("062cfa5657f5388d", &fs));   // fragment: constant magenta (1, 0, 1, 1)
    auto prog = app.make_program({&vs, &gs, &fs});
    REQUIRE(prog != nullptr);

    const Geometry g = cube_geometry();
    App::Buf vb = app.make_buffer(g.verts.size() * sizeof(float), g.verts.data());
    App::Buf ib = app.make_buffer(g.idx.size() * sizeof(uint16_t), g.idx.data());
    auto cube = make_cube(app, NVN_FORMAT_RGBA8, NVN_TEXTURE_FLAGS_COMPRESSIBLE);
    REQUIRE(cube != nullptr);
    REQUIRE(render_cube(app, *cube, prog.get(), vb, ib));

    int faces_bad = 0;
    for (int f = 0; f < 6; ++f) {
        std::vector<uint8_t> px;
        read_face(app, *cube, f, 4, px);
        size_t magenta = 0;
        for (size_t i = 0; i < px.size(); i += 4)
            if (px[i] == 255 && px[i + 1] == 0 && px[i + 2] == 255 && px[i + 3] == 255) ++magenta;
        std::printf("    face %d: %zu of %d texels magenta\n", f, magenta, kSide * kSide);
        if (magenta != static_cast<size_t>(kSide) * kSide) ++faces_bad;
    }
    CHECK_EQ(faces_bad, 0);
    const auto v = app.validation();
    std::printf("    validation: %llu errors %s\n", static_cast<unsigned long long>(v.errors), v.first_error.c_str());
    CHECK_EQ(v.errors, 0ull);
    app.shutdown();
}

NXTEST(passthrough_geometry_shader_copies_location_0) {
    if (!real_data_available()) { nxtest::skip("work/shader_cache not found"); return; }
    App app;
    REQUIRE(app.init());
    App::ShaderBlob vs, gs, fs;
    REQUIRE(App::load_shader("38f38f70414e8efe", &vs));   // vertex: as in case 1, Location 0 output = attribute 1 (u, v, layer, 1)
    REQUIRE(App::load_shader("80271a630230f99a", &gs));   // geometry: passthrough of the Location 0 input (no Location output in the translation)
    REQUIRE(App::load_shader("8b7d1022ca3c42ff", &fs));   // fragment: output Location 0 = input Location 0 (vec4), nothing else
    auto prog = app.make_program({&vs, &gs, &fs});
    REQUIRE(prog != nullptr);

    const Geometry g = cube_geometry();
    App::Buf vb = app.make_buffer(g.verts.size() * sizeof(float), g.verts.data());
    App::Buf ib = app.make_buffer(g.idx.size() * sizeof(uint16_t), g.idx.data());
    // RGBA32F, so the layer number survives the store exactly (an 8-bit target would clamp layers 1..5 to 1)
    auto cube = make_cube(app, NVN_FORMAT_RGBA32F, 0);
    REQUIRE(cube != nullptr);
    REQUIRE(render_cube(app, *cube, prog.get(), vb, ib));

    // Expected texel (column c, row r) of face f: (u, v, f, 1) with u = (c + 0.5) / 8 and v = (r + 0.5) / 8 when the rows run down
    // from NDC y = -1, or v = 1 - (r + 0.5) / 8 when they run the other way. The orientation is not assumed: both are counted and the
    // larger count must cover every texel.
    constexpr float kTol = 2e-3f;
    const size_t total = 6 * static_cast<size_t>(kSide) * kSide;
    size_t match_down = 0, match_up = 0;
    for (int f = 0; f < 6; ++f) {
        std::vector<uint8_t> raw;
        read_face(app, *cube, f, 16, raw);
        for (int r = 0; r < kSide; ++r)
            for (int c = 0; c < kSide; ++c) {
                float t[4];
                std::memcpy(t, &raw[(static_cast<size_t>(r) * kSide + c) * 16], 16);
                const float u = (static_cast<float>(c) + 0.5f) / kSide;
                const float v_down = (static_cast<float>(r) + 0.5f) / kSide, v_up = 1.0f - v_down;
                const bool uvz = std::fabs(t[0] - u) < kTol && std::fabs(t[2] - static_cast<float>(f)) < kTol && std::fabs(t[3] - 1.0f) < kTol;
                if (uvz && std::fabs(t[1] - v_down) < kTol) ++match_down;
                if (uvz && std::fabs(t[1] - v_up) < kTol) ++match_up;
            }
    }
    std::printf("    texels matching (u, v, face, 1): rows down %zu, rows up %zu, of %zu\n", match_down, match_up, total);
    CHECK(std::max(match_down, match_up) == total);
    const auto v = app.validation();
    std::printf("    validation: %llu errors %s\n", static_cast<unsigned long long>(v.errors), v.first_error.c_str());
    CHECK_EQ(v.errors, 0ull);
    app.shutdown();
}
