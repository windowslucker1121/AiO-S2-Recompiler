// SPDX-License-Identifier: GPL-3.0-or-later
// NVN scene oracle 2: a triangle drawn with two REAL translated shaders of title E (a position-only vertex shader
// and a constant-magenta fragment shader, found in work/shader_cache). Checks the whole draw path: program binding, vertex input,
// pipeline, viewport flip for the upper-left window origin, pass handling. Skipped when the survey data is not on this machine.
#include "nvn_app.h"
NXTEST_GROUP(gfx_nvn_scene_triangle)

using namespace nvnapp;

namespace {
struct Rgba { int r, g, b, a; };
Rgba at(const std::vector<uint8_t>& px, uint32_t w, uint32_t x, uint32_t y) {
    const uint8_t* p = &px[(static_cast<size_t>(y) * w + x) * 4];
    return {p[0], p[1], p[2], p[3]};
}
}  // namespace

NXTEST(triangle_with_real_shaders) {
    if (!real_data_available()) { nxtest::skip("work/shader_cache not found"); return; }
    App app;
    App::Opts o;
    o.width = 64;
    o.height = 48;
    REQUIRE(app.init(o));
    App::ShaderBlob vs, fs;
    REQUIRE(App::load_shader("a656883b0cdb8ebc", &vs));   // vertex: gl_Position.xyz = attribute 0
    REQUIRE(App::load_shader("062cfa5657f5388d", &fs));   // fragment: colour (1, 0, 1, 1)
    auto prog = app.make_program({&vs, &fs});
    REQUIRE(prog != nullptr);

    const float verts[] = {-0.5f, -0.5f, 0.0f, 0.5f, -0.5f, 0.0f, 0.0f, 0.5f, 0.0f};
    App::Buf vb = app.make_buffer(sizeof verts, verts);

    auto cb = app.make_cmdbuf();
    REQUIRE(cb != nullptr);
    NVNcommandBuffer* c = cb->obj.get();
    auto& api = app.api;
    const int idx = app.acquire();
    NVNtexture* targets[] = {app.window_textures[idx]->obj.get()};
    api.nvnCommandBufferBeginRecording(c);
    api.nvnCommandBufferSetRenderTargets(c, 1, targets, nullptr, nullptr, nullptr);
    api.nvnCommandBufferSetViewport(c, 0, 0, 64, 48);
    api.nvnCommandBufferSetScissor(c, 0, 0, 64, 48);
    const float black[4] = {0, 0, 0, 1};
    api.nvnCommandBufferClearColor(c, 0, black, NVN_CLEAR_COLOR_MASK_RGBA);

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

    NVNvertexAttribState attr;
    api.nvnVertexAttribStateSetDefaults(&attr);
    api.nvnVertexAttribStateSetFormat(&attr, NVN_FORMAT_RGB32F, 0);
    api.nvnVertexAttribStateSetStreamIndex(&attr, 0);
    NVNvertexStreamState stream;
    api.nvnVertexStreamStateSetDefaults(&stream);
    api.nvnVertexStreamStateSetStride(&stream, 12);
    api.nvnCommandBufferBindVertexAttribState(c, 1, &attr);
    api.nvnCommandBufferBindVertexStreamState(c, 1, &stream);
    api.nvnCommandBufferBindVertexBuffer(c, 0, vb.address, vb.size);
    api.nvnCommandBufferBindProgram(c, prog.get(), NVN_SHADER_STAGE_VERTEX_BIT | NVN_SHADER_STAGE_FRAGMENT_BIT);
    api.nvnCommandBufferDrawArrays(c, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3);
    app.end_and_submit(*cb);

    std::vector<uint8_t> px;
    REQUIRE(app.present_and_capture(idx, &px));
    const Rgba inside = at(px, 64, 32, 28), above = at(px, 64, 32, 6), below = at(px, 64, 32, 42), left = at(px, 64, 8, 28);
    std::printf("    inside %d %d %d, above apex %d %d %d, below base %d %d %d, far left %d %d %d\n", inside.r, inside.g, inside.b, above.r, above.g, above.b,
                below.r, below.g, below.b, left.r, left.g, left.b);
    CHECK(inside.r == 255 && inside.g == 0 && inside.b == 255);
    CHECK(above.r == 0 && above.g == 0 && above.b == 0);
    CHECK(below.r == 0 && below.g == 0 && below.b == 0);
    CHECK(left.r == 0 && left.g == 0 && left.b == 0);
    size_t magenta = 0;
    for (size_t i = 0; i < px.size(); i += 4) if (px[i] == 255 && px[i + 1] == 0 && px[i + 2] == 255) ++magenta;
    std::printf("    magenta pixels %zu (base 32 px x height 24 px / 2 = 384)\n", magenta);
    CHECK(magenta > 360 && magenta < 410);
    const auto v = app.validation();
    std::printf("    validation: %llu errors, %llu warnings %s\n", static_cast<unsigned long long>(v.errors), static_cast<unsigned long long>(v.warnings), v.first_error.c_str());
    CHECK_EQ(v.errors, 0ull);
    const auto st = nxrt::nvn::backend_stats();
    std::printf("    draws %llu skipped %llu pipelines %llu unresolved %llu unsupported %llu\n", static_cast<unsigned long long>(st.draws), static_cast<unsigned long long>(st.draws_skipped),
                static_cast<unsigned long long>(st.pipelines_created), static_cast<unsigned long long>(st.unresolved_addresses), static_cast<unsigned long long>(st.unsupported_ops));
    CHECK_EQ(st.draws, 1ull);
    app.shutdown();
}
