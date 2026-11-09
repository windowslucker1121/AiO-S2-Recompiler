// SPDX-License-Identifier: GPL-3.0-or-later
// Tests of the texture pixel edit hook (nxrt/tex_edit.h, src/gfx/nvn/nvn_tex_edit.cpp) through prepare_texture: the host sees the decoded levels,
// its edit reaches the output (RGBA8 compared texel by texel, BC7 by size and difference), the edited data is kept and reused per generation,
// it never reaches the BC7 disk cache, and textures the host does not match come out as before.
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <vector>

#include <nxrt/config.h>
#include <nxrt/tex_edit.h>

#include "nvn_core.h"
#include "nvn_format.h"
#include "nvn_texdata.h"
#include "nvn_texture_layout.h"
#include "tegra_tiling.h"
#include "test_framework.h"
NXTEST_GROUP(gfx_nvn_tex_edit)

using namespace nxrt;
using namespace nxrt::nvn;
using namespace nxrt::nvn::be;
namespace fs = std::filesystem;

namespace {

void set_env(const char* name, const char* value) { _putenv_s(name, value); }

struct Built {
    Ref<Texture> tex;
    TexLayout L;
    std::vector<uint8_t> lin, mem;
};

// 64x40 ASTC 8x5 UNORM, 3 levels, every block a constant colour (void-extent block) of (r, g, b) = (4x, 4y, 200) per block, so levels differ.
void build(Built& bt, int w = 64, int h = 40) {
    TextureBuilderInfo b;
    b.target = NVN_TEXTURE_TARGET_2D;
    b.width = w;
    b.height = h;
    b.depth = 1;
    b.levels = 3;
    b.format = NVN_FORMAT_RGBA_ASTC_8x5;
    bt.L = compute_texture_layout(b);
    REQUIRE(bt.L.valid && bt.L.block_linear);
    bt.lin.assign(bt.L.layout.linear_size, 0);
    for (const tegra::LevelLayout& lv : bt.L.layout.levels)
        for (uint32_t y = 0; y < lv.blocks_y; ++y)
            for (uint32_t x = 0; x < lv.blocks_x; ++x) {
                uint8_t* blk = bt.lin.data() + lv.linear_offset + y * lv.linear_row_stride + x * 16;
                blk[0] = 0xFC;
                blk[1] = 0xFD;
                std::memset(blk + 2, 0xFF, 6);
                const uint16_t c[4] = {static_cast<uint16_t>(4 * x * 257), static_cast<uint16_t>(4 * y * 257), 200 * 257, 0xFFFF};
                std::memcpy(blk + 8, c, 8);
            }
    bt.mem.assign(bt.L.storage_size, 0);
    tegra::tile(bt.L.desc, bt.L.layout, bt.lin.data(), bt.mem.data());
    bt.tex = Ref<Texture>(new Texture());
    Texture& t = *bt.tex;
    t.b = b;
    t.fmt = bt.L.fmt;
    t.mem = bt.mem.data();
    t.storage_size = bt.L.storage_size;
    t.block_linear = true;
    t.levels = bt.L.levels;
    t.layers = bt.L.layers;
    t.block_height_gobs = bt.L.layout.levels[0].gob_blocks_y;
}

struct Host {
    uint64_t gen = 1;
    uint32_t want_w = 64;
    int levels_seen = 0;
    int calls = 0;
};
Host g_host;

uint64_t h_match(const gfx::TexEditReq& r, void* u) {
    Host* h = static_cast<Host*>(u);
    return r.width == h->want_w && r.levels == 3 && r.block_w == 8 && r.block_h == 5 && r.tiled && r.tiled_size > 0 ? 42 : 0;
}
uint64_t h_gen(void* u) { return static_cast<Host*>(u)->gen; }
void h_edit(uint64_t id, uint32_t level, uint32_t w, uint32_t h, uint8_t* rgba, void* u) {
    Host* hs = static_cast<Host*>(u);
    CHECK_EQ(id, 42u);
    CHECK_EQ(level, static_cast<uint32_t>(hs->levels_seen % 3));   // level 0 first, then 1, 2
    hs->levels_seen++;
    hs->calls++;
    for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {   // invert the colour, keep alpha
        rgba[i * 4 + 0] = static_cast<uint8_t>(255 - rgba[i * 4 + 0]);
        rgba[i * 4 + 1] = static_cast<uint8_t>(255 - rgba[i * 4 + 1]);
        rgba[i * 4 + 2] = static_cast<uint8_t>(255 - rgba[i * 4 + 2]);
    }
}

size_t files_in(const fs::path& d) {
    size_t n = 0;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(d, ec); it != fs::recursive_directory_iterator(); ++it)
        if (it->is_regular_file() && it->path().extension() == ".bc7") ++n;   // cache entries (not the format index)
    return n;
}

}  // namespace

NXTEST(edit_reaches_rgba8_output_and_is_reused) {
    set_env("NXRT_NVN_BC7_CACHE", "");
    config_set("nvn", "bc7_cache", "");
    Built bt;
    build(bt);
    TexCaps rgba;
    rgba.bc7_transcode = false;
    PreparedTexture plain;
    REQUIRE(prepare_texture(*bt.tex, rgba, plain));

    g_host = Host{};
    gfx::TexEditHooks hk;
    hk.match = &h_match;
    hk.generation = &h_gen;
    hk.edit = &h_edit;
    hk.user = &g_host;
    gfx::set_texture_edit(&hk);

    PreparedTexture ed;
    REQUIRE(prepare_texture(*bt.tex, rgba, ed));
    CHECK_EQ(g_host.calls, 3);   // three levels
    REQUIRE(ed.data.size() == plain.data.size());
    CHECK(ed.format == plain.format);
    bool all_inverted = true;
    for (size_t i = 0; i < plain.data.size(); ++i) {
        const uint8_t want = (i % 4 == 3) ? plain.data[i] : static_cast<uint8_t>(255 - plain.data[i]);
        if (ed.data[i] != want) all_inverted = false;
    }
    CHECK(all_inverted);

    // the same generation: kept data, no new edit
    const gfx::TexEditStats s0 = gfx::texture_edit_stats();
    PreparedTexture again;
    REQUIRE(prepare_texture(*bt.tex, rgba, again));
    CHECK_EQ(g_host.calls, 3);
    CHECK(again.data == ed.data);
    CHECK_EQ(gfx::texture_edit_stats().reuses, s0.reuses + 1);

    // a new generation: edited again
    g_host.gen = 2;
    PreparedTexture third;
    REQUIRE(prepare_texture(*bt.tex, rgba, third));
    CHECK_EQ(g_host.calls, 6);
    CHECK(third.data == ed.data);

    // a texture the host does not match comes out as before
    g_host.want_w = 32;
    PreparedTexture other;
    REQUIRE(prepare_texture(*bt.tex, rgba, other));
    CHECK(other.data == plain.data);
    CHECK_EQ(g_host.calls, 6);

    gfx::set_texture_edit(nullptr);
    PreparedTexture off;
    REQUIRE(prepare_texture(*bt.tex, rgba, off));
    CHECK(off.data == plain.data);
}

// BC7 output: the edit changes the blocks, the sizes stay, and the shared disk cache gets nothing from the edit.
NXTEST(edited_bc7_stays_out_of_the_disk_cache) {
    const fs::path dir = fs::temp_directory_path() / ("nxrt_texedit_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code ec;
    fs::create_directories(dir, ec);
    set_env("NXRT_NVN_BC7_CACHE", dir.string().c_str());
    Built bt;
    build(bt);

    g_host = Host{};
    gfx::TexEditHooks hk;
    hk.match = &h_match;
    hk.generation = &h_gen;
    hk.edit = &h_edit;
    hk.user = &g_host;
    gfx::set_texture_edit(&hk);
    PreparedTexture ed;
    REQUIRE(prepare_texture(*bt.tex, TexCaps(), ed));
    CHECK(ed.format == VK_FORMAT_BC7_UNORM_BLOCK);
    CHECK(!ed.from_cache);
    CHECK_EQ(g_host.calls, 3);
    CHECK_EQ(files_in(dir), 0u);   // nothing written for the edited texture

    gfx::set_texture_edit(nullptr);
    PreparedTexture plain;
    REQUIRE(prepare_texture(*bt.tex, TexCaps(), plain));
    CHECK_EQ(files_in(dir), 1u);   // the game's own data is cached as always
    CHECK_EQ(plain.data.size(), ed.data.size());
    CHECK(plain.data != ed.data);
    PreparedTexture cached;
    REQUIRE(prepare_texture(*bt.tex, TexCaps(), cached));
    CHECK(cached.from_cache);
    CHECK(cached.data == plain.data);

    // with the hooks back, the cache entry of the game's data is not used for the edited texture
    g_host.gen = 5;
    gfx::set_texture_edit(&hk);
    PreparedTexture ed2;
    REQUIRE(prepare_texture(*bt.tex, TexCaps(), ed2));
    CHECK(!ed2.from_cache);
    CHECK(ed2.data == ed.data);
    gfx::set_texture_edit(nullptr);

    set_env("NXRT_NVN_BC7_CACHE", "");
    fs::remove_all(dir, ec);
}
