// SPDX-License-Identifier: GPL-3.0-or-later
// NVNtexture entry points and the texture storage layout (family texture, api_texture.cpp, nvn_texture_layout.cpp). The layout
// is checked three ways: literal sizes worked out by hand from the GOB rules, an independent GOB formula over a random sweep,
// and the game's own GPU buffer sizes from the texture survey (work/survey/textures/txtr_all.tsv, skipped when absent).
// The entry points are called through abi::proc, the way a game calls them. The memory pool is built by hand (the pool
// entry points belong to another family).
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "nvn_core.h"
#include "nvn_impl.h"
#include "nvn_texture_layout.h"
#include "test_framework.h"
#include "test_platform.h"
#include "project_root.h"
NXTEST_GROUP(gfx_nvn_texture)

using namespace nxrt;
using namespace nxrt::nvn;
using be::TexLayout;
using nxtest::skip;

namespace {

void setup() {
    install();
    abi::reset_mismatches();
    register_api_texture_builder();
    register_api_texture();
    CHECK_EQ(abi::mismatches(), 0u);
}

uint64_t div_up(uint64_t a, uint64_t b) { return (a + b - 1) / b; }
uint64_t align_up(uint64_t a, uint64_t b) { return div_up(a, b) * b; }

// ---- independent reference: the GOB rules written out again (not tegra) ------------------------------------------------
// Smallest power of two g with 8 g >= rows, at most 16: the block height in GOBs of a level with `rows` block rows.
uint64_t ref_gobs(uint64_t rows) {
    uint64_t g = 1;
    while (g < 16 && g * 8 < rows) g *= 2;
    return g;
}
// Level 0 of a texture without a packaged layout: the console's rule (1.5 times the block rows against 16, 32, 64 and 128), which is the
// layout of the game's own TXTR data. Smaller textures get a smaller block height than the smallest-fit rule above would give them.
uint64_t ref_gobs0(uint64_t rows) {
    const uint64_t hh = rows + rows / 2;
    return hh >= 128 ? 16 : hh >= 64 ? 8 : hh >= 32 ? 4 : hh >= 16 ? 2 : 1;
}
// Bytes of one layer (all levels) and the layer alignment (512 bytes per GOB times the level-0 block height).
struct RefLayer {
    uint64_t bytes = 0, align = 0;
};
RefLayer ref_2d(uint64_t w, uint64_t h, uint64_t levels, uint64_t bw, uint64_t bh, uint64_t bpb) {
    RefLayer r;
    const uint64_t gy0 = ref_gobs0(div_up(h, bh));
    for (uint64_t l = 0; l < levels; ++l) {
        const uint64_t lw = std::max<uint64_t>(1, w >> l), lh = std::max<uint64_t>(1, h >> l);
        const uint64_t bx = div_up(lw, bw), by = div_up(lh, bh);
        const uint64_t gy = std::min(gy0, ref_gobs(by));
        const uint64_t pitch = align_up(bx * bpb, 64);   // a row of GOBs is 64 bytes wide
        r.bytes += pitch * align_up(by, 8 * gy);          // rows are padded to whole blocks of 8 * gy rows
    }
    r.align = 512 * gy0;
    return r;
}
uint64_t ref_levels(uint64_t w, uint64_t h, uint64_t requested) {
    uint64_t top = 1;
    while (((std::max(w, h)) >> top) != 0) ++top;
    return std::min(requested, top);
}

// ---- helpers for the entry points --------------------------------------------------------------------------------------

// A pool over a host buffer, bound to a guest NVNmemoryPool the way the pool entry points would bind it.
struct Pool {
    std::vector<uint8_t> mem;
    NVNmemoryPool guest;
    static constexpr uint64_t kGpuBase = 0x7000000000ull;
    explicit Pool(size_t bytes) : mem(bytes, 0) {
        std::memset(&guest, 0, sizeof guest);
        auto* host = new be::MemoryPool();
        host->base = mem.data();
        host->size = mem.size();
        host->gpu_base = kGpuBase;
        be::bind_guest(&guest, host);
    }
    ~Pool() { be::unbind_guest(&guest); }
};

struct Api {
    // builder
    decltype(abi::proc<PFNNVNTEXTUREBUILDERSETDEFAULTSPROC>("")) bdefaults = abi::proc<PFNNVNTEXTUREBUILDERSETDEFAULTSPROC>("nvnTextureBuilderSetDefaults");
    decltype(abi::proc<PFNNVNTEXTUREBUILDERSETTARGETPROC>("")) btarget = abi::proc<PFNNVNTEXTUREBUILDERSETTARGETPROC>("nvnTextureBuilderSetTarget");
    decltype(abi::proc<PFNNVNTEXTUREBUILDERSETSIZE2DPROC>("")) bsize2d = abi::proc<PFNNVNTEXTUREBUILDERSETSIZE2DPROC>("nvnTextureBuilderSetSize2D");
    decltype(abi::proc<PFNNVNTEXTUREBUILDERSETDEPTHPROC>("")) bdepth = abi::proc<PFNNVNTEXTUREBUILDERSETDEPTHPROC>("nvnTextureBuilderSetDepth");
    decltype(abi::proc<PFNNVNTEXTUREBUILDERSETLEVELSPROC>("")) blevels = abi::proc<PFNNVNTEXTUREBUILDERSETLEVELSPROC>("nvnTextureBuilderSetLevels");
    decltype(abi::proc<PFNNVNTEXTUREBUILDERSETFORMATPROC>("")) bformat = abi::proc<PFNNVNTEXTUREBUILDERSETFORMATPROC>("nvnTextureBuilderSetFormat");
    decltype(abi::proc<PFNNVNTEXTUREBUILDERSETFLAGSPROC>("")) bflags = abi::proc<PFNNVNTEXTUREBUILDERSETFLAGSPROC>("nvnTextureBuilderSetFlags");
    decltype(abi::proc<PFNNVNTEXTUREBUILDERSETSTRIDEPROC>("")) bstride = abi::proc<PFNNVNTEXTUREBUILDERSETSTRIDEPROC>("nvnTextureBuilderSetStride");
    decltype(abi::proc<PFNNVNTEXTUREBUILDERSETSTORAGEPROC>("")) bstorage = abi::proc<PFNNVNTEXTUREBUILDERSETSTORAGEPROC>("nvnTextureBuilderSetStorage");
    decltype(abi::proc<PFNNVNTEXTUREBUILDERSETPACKAGEDTEXTURELAYOUTPROC>("")) blayout = abi::proc<PFNNVNTEXTUREBUILDERSETPACKAGEDTEXTURELAYOUTPROC>("nvnTextureBuilderSetPackagedTextureLayout");
    decltype(abi::proc<PFNNVNTEXTUREBUILDERGETSTORAGESIZEPROC>("")) bget_size = abi::proc<PFNNVNTEXTUREBUILDERGETSTORAGESIZEPROC>("nvnTextureBuilderGetStorageSize");
    decltype(abi::proc<PFNNVNTEXTUREBUILDERGETSTORAGEALIGNMENTPROC>("")) bget_align = abi::proc<PFNNVNTEXTUREBUILDERGETSTORAGEALIGNMENTPROC>("nvnTextureBuilderGetStorageAlignment");
    decltype(abi::proc<PFNNVNTEXTUREBUILDERGETSTORAGECLASSPROC>("")) bget_class = abi::proc<PFNNVNTEXTUREBUILDERGETSTORAGECLASSPROC>("nvnTextureBuilderGetStorageClass");
    decltype(abi::proc<PFNNVNTEXTUREBUILDERGETSPARSETILELAYOUTPROC>("")) bget_sparse = abi::proc<PFNNVNTEXTUREBUILDERGETSPARSETILELAYOUTPROC>("nvnTextureBuilderGetSparseTileLayout");
    decltype(abi::proc<PFNNVNTEXTUREBUILDERGETZCULLSTORAGESIZEPROC>("")) bget_zcull = abi::proc<PFNNVNTEXTUREBUILDERGETZCULLSTORAGESIZEPROC>("nvnTextureBuilderGetZCullStorageSize");
    // view
    decltype(abi::proc<PFNNVNTEXTUREVIEWSETDEFAULTSPROC>("")) vdefaults = abi::proc<PFNNVNTEXTUREVIEWSETDEFAULTSPROC>("nvnTextureViewSetDefaults");
    decltype(abi::proc<PFNNVNTEXTUREVIEWSETLEVELSPROC>("")) vlevels = abi::proc<PFNNVNTEXTUREVIEWSETLEVELSPROC>("nvnTextureViewSetLevels");
    decltype(abi::proc<PFNNVNTEXTUREVIEWSETLAYERSPROC>("")) vlayers = abi::proc<PFNNVNTEXTUREVIEWSETLAYERSPROC>("nvnTextureViewSetLayers");
    // texture
    decltype(abi::proc<PFNNVNTEXTUREINITIALIZEPROC>("")) init = abi::proc<PFNNVNTEXTUREINITIALIZEPROC>("nvnTextureInitialize");
    decltype(abi::proc<PFNNVNTEXTUREFINALIZEPROC>("")) finalize = abi::proc<PFNNVNTEXTUREFINALIZEPROC>("nvnTextureFinalize");
    decltype(abi::proc<PFNNVNTEXTURESETDEBUGLABELPROC>("")) label = abi::proc<PFNNVNTEXTURESETDEBUGLABELPROC>("nvnTextureSetDebugLabel");
    decltype(abi::proc<PFNNVNTEXTUREGETZCULLSTORAGESIZEPROC>("")) zcull = abi::proc<PFNNVNTEXTUREGETZCULLSTORAGESIZEPROC>("nvnTextureGetZCullStorageSize");
    decltype(abi::proc<PFNNVNTEXTUREGETSTORAGECLASSPROC>("")) tclass = abi::proc<PFNNVNTEXTUREGETSTORAGECLASSPROC>("nvnTextureGetStorageClass");
    decltype(abi::proc<PFNNVNTEXTUREGETVIEWOFFSETPROC>("")) view_offset = abi::proc<PFNNVNTEXTUREGETVIEWOFFSETPROC>("nvnTextureGetViewOffset");
    decltype(abi::proc<PFNNVNTEXTUREGETFLAGSPROC>("")) flags = abi::proc<PFNNVNTEXTUREGETFLAGSPROC>("nvnTextureGetFlags");
    decltype(abi::proc<PFNNVNTEXTUREGETTARGETPROC>("")) target = abi::proc<PFNNVNTEXTUREGETTARGETPROC>("nvnTextureGetTarget");
    decltype(abi::proc<PFNNVNTEXTUREGETWIDTHPROC>("")) width = abi::proc<PFNNVNTEXTUREGETWIDTHPROC>("nvnTextureGetWidth");
    decltype(abi::proc<PFNNVNTEXTUREGETHEIGHTPROC>("")) height = abi::proc<PFNNVNTEXTUREGETHEIGHTPROC>("nvnTextureGetHeight");
    decltype(abi::proc<PFNNVNTEXTUREGETDEPTHPROC>("")) depth = abi::proc<PFNNVNTEXTUREGETDEPTHPROC>("nvnTextureGetDepth");
    decltype(abi::proc<PFNNVNTEXTUREGETLEVELSPROC>("")) levels = abi::proc<PFNNVNTEXTUREGETLEVELSPROC>("nvnTextureGetLevels");
    decltype(abi::proc<PFNNVNTEXTUREGETFORMATPROC>("")) format = abi::proc<PFNNVNTEXTUREGETFORMATPROC>("nvnTextureGetFormat");
    decltype(abi::proc<PFNNVNTEXTUREGETSAMPLESPROC>("")) samples = abi::proc<PFNNVNTEXTUREGETSAMPLESPROC>("nvnTextureGetSamples");
    decltype(abi::proc<PFNNVNTEXTUREGETSWIZZLEPROC>("")) swizzle = abi::proc<PFNNVNTEXTUREGETSWIZZLEPROC>("nvnTextureGetSwizzle");
    decltype(abi::proc<PFNNVNTEXTUREGETDEPTHSTENCILMODEPROC>("")) dsmode = abi::proc<PFNNVNTEXTUREGETDEPTHSTENCILMODEPROC>("nvnTextureGetDepthStencilMode");
    decltype(abi::proc<PFNNVNTEXTUREGETSTRIDEPROC>("")) stride = abi::proc<PFNNVNTEXTUREGETSTRIDEPROC>("nvnTextureGetStride");
    decltype(abi::proc<PFNNVNTEXTUREGETTEXTUREADDRESSPROC>("")) address = abi::proc<PFNNVNTEXTUREGETTEXTUREADDRESSPROC>("nvnTextureGetTextureAddress");
    decltype(abi::proc<PFNNVNTEXTUREGETSPARSETILELAYOUTPROC>("")) sparse = abi::proc<PFNNVNTEXTUREGETSPARSETILELAYOUTPROC>("nvnTextureGetSparseTileLayout");
    decltype(abi::proc<PFNNVNTEXTUREWRITETEXELSPROC>("")) write = abi::proc<PFNNVNTEXTUREWRITETEXELSPROC>("nvnTextureWriteTexels");
    decltype(abi::proc<PFNNVNTEXTUREWRITETEXELSSTRIDEDPROC>("")) write_s = abi::proc<PFNNVNTEXTUREWRITETEXELSSTRIDEDPROC>("nvnTextureWriteTexelsStrided");
    decltype(abi::proc<PFNNVNTEXTUREREADTEXELSPROC>("")) read = abi::proc<PFNNVNTEXTUREREADTEXELSPROC>("nvnTextureReadTexels");
    decltype(abi::proc<PFNNVNTEXTUREREADTEXELSSTRIDEDPROC>("")) read_s = abi::proc<PFNNVNTEXTUREREADTEXELSSTRIDEDPROC>("nvnTextureReadTexelsStrided");
    decltype(abi::proc<PFNNVNTEXTUREFLUSHTEXELSPROC>("")) flush = abi::proc<PFNNVNTEXTUREFLUSHTEXELSPROC>("nvnTextureFlushTexels");
    decltype(abi::proc<PFNNVNTEXTUREINVALIDATETEXELSPROC>("")) invalidate = abi::proc<PFNNVNTEXTUREINVALIDATETEXELSPROC>("nvnTextureInvalidateTexels");
    decltype(abi::proc<PFNNVNTEXTUREGETMEMORYPOOLPROC>("")) pool_of = abi::proc<PFNNVNTEXTUREGETMEMORYPOOLPROC>("nvnTextureGetMemoryPool");
    decltype(abi::proc<PFNNVNTEXTUREGETMEMORYOFFSETPROC>("")) pool_offset = abi::proc<PFNNVNTEXTUREGETMEMORYOFFSETPROC>("nvnTextureGetMemoryOffset");
    decltype(abi::proc<PFNNVNTEXTUREGETSTORAGESIZEPROC>("")) storage_size = abi::proc<PFNNVNTEXTUREGETSTORAGESIZEPROC>("nvnTextureGetStorageSize");
    decltype(abi::proc<PFNNVNTEXTURECOMPAREPROC>("")) compare = abi::proc<PFNNVNTEXTURECOMPAREPROC>("nvnTextureCompare");
    decltype(abi::proc<PFNNVNTEXTUREGETDEBUGIDPROC>("")) debug_id = abi::proc<PFNNVNTEXTUREGETDEBUGIDPROC>("nvnTextureGetDebugID");
};

// A 2D (or other) builder with the given fields, storage at pool offset `offset`.
void build(Api& a, NVNtextureBuilder& b, const Pool& pool, int target, int w, int h, int d, int levels, int format,
           ptrdiff_t offset = 0, int flags = 0) {
    std::memset(&b, 0, sizeof b);
    a.bdefaults(&b);
    a.btarget(&b, static_cast<NVNtextureTarget>(target));
    a.bsize2d(&b, w, h);
    a.bdepth(&b, d);
    a.blevels(&b, levels);
    a.bformat(&b, static_cast<NVNformat>(format));
    a.bflags(&b, flags);
    a.bstorage(&b, const_cast<NVNmemoryPool*>(&pool.guest), offset);
}

// Pattern for texel data: byte i of the buffer is a function of i and the seed.
void fill(std::vector<uint8_t>& v, uint32_t seed) {
    for (size_t i = 0; i < v.size(); ++i) v[i] = static_cast<uint8_t>((i * 131u + seed * 7u + (i >> 8)) & 0xff);
}

NVNcopyRegion region(int x, int y, int z, int w, int h, int d) {
    NVNcopyRegion r{};
    r.xoffset = x;
    r.yoffset = y;
    r.zoffset = z;
    r.width = w;
    r.height = h;
    r.depth = d;
    return r;
}

}  // namespace

// ================================================================================================================================
// Registration
// ================================================================================================================================

NXTEST(all_entry_points_registered) {
    setup();
    const char* names[] = {
        // texture: 30 (the NVN table's nvnTexture* functions, without the builder, view and pool ones; GetRawStorageClass skipped)
        "nvnTextureInitialize", "nvnTextureFinalize", "nvnTextureSetDebugLabel", "nvnTextureGetZCullStorageSize",
        "nvnTextureGetStorageClass", "nvnTextureGetViewOffset", "nvnTextureGetFlags", "nvnTextureGetTarget",
        "nvnTextureGetWidth", "nvnTextureGetHeight", "nvnTextureGetDepth", "nvnTextureGetLevels", "nvnTextureGetFormat",
        "nvnTextureGetSamples", "nvnTextureGetSwizzle", "nvnTextureGetDepthStencilMode", "nvnTextureGetStride",
        "nvnTextureGetTextureAddress", "nvnTextureGetSparseTileLayout", "nvnTextureWriteTexels",
        "nvnTextureWriteTexelsStrided", "nvnTextureReadTexels", "nvnTextureReadTexelsStrided", "nvnTextureFlushTexels",
        "nvnTextureInvalidateTexels", "nvnTextureGetMemoryPool", "nvnTextureGetMemoryOffset", "nvnTextureGetStorageSize",
        "nvnTextureCompare", "nvnTextureGetDebugID",
        // builder queries that need the layout: 5
        "nvnTextureBuilderGetStorageSize", "nvnTextureBuilderGetStorageAlignment", "nvnTextureBuilderGetStorageClass",
        "nvnTextureBuilderGetSparseTileLayout", "nvnTextureBuilderGetZCullStorageSize",
    };
    CHECK_EQ(sizeof names / sizeof names[0], 35u);
    for (const char* n : names) CHECK_MSG(is_implemented(n), n);
    CHECK_EQ(abi::mismatches(), 0u);
}

// ================================================================================================================================
// Layout: literal sizes, worked out by hand from the GOB rules (512-byte GOBs of 64 x 8 bytes, rows padded to whole blocks)
// ================================================================================================================================

namespace {
struct Case {
    const char* name;
    TextureBuilderInfo b;
    uint64_t storage, alignment, levels;
};
TextureBuilderInfo mk(int target, int w, int h, int d, int levels, int fmt, int flags = 0) {
    TextureBuilderInfo b;
    b.target = target;
    b.width = w;
    b.height = h;
    b.depth = d;
    b.levels = levels;
    b.format = fmt;
    b.flags = flags;
    return b;
}
}  // namespace

NXTEST(layout_literal_sizes) {
    // Hand-worked values (see the comments): level rows are padded to whole blocks, levels to 64 bytes per GOB row.
    Case cases[] = {
        // 1x1 R8: one 64 x 8 GOB area, 512 bytes.
        {"R8 1x1", mk(NVN_TEXTURE_TARGET_2D, 1, 1, 1, 1, NVN_FORMAT_R8), 512, 512, 1},
        // 64x64 RGBA8, one level: 256 bytes per row, 64 rows = 8 GOB rows at block height 8: 256 * 64.
        {"RGBA8 64x64 L1", mk(NVN_TEXTURE_TARGET_2D, 64, 64, 1, 1, NVN_FORMAT_RGBA8), 16384, 4096, 1},
        // 64x64 RGBA8 with 7 levels: 16384 + 4096 (32^2, block 4) + 1024 (16^2, block 2) + 4 x 512 (8^2 .. 1^2) = 23552.
        {"RGBA8 64x64 L7", mk(NVN_TEXTURE_TARGET_2D, 64, 64, 1, 7, NVN_FORMAT_RGBA8), 23552, 4096, 7},
        // BC1 256x256 (64x64 blocks of 8 bytes) with 9 levels: 32768 + 8192 + 2048 + 6 x 512 = 46080.
        {"BC1 256x256 L9", mk(NVN_TEXTURE_TARGET_2D, 256, 256, 1, 9, NVN_FORMAT_RGBA_DXT1), 46080, 4096, 9},
        // ASTC 8x5 (16 bytes per block) 100x100: 13 x 20 blocks, rows 208 -> 256 bytes, 20 rows at block height 2 (1.5 x 20 = 30 >= 16): 256 * 32.
        {"ASTC 8x5 100x100", mk(NVN_TEXTURE_TARGET_2D, 100, 100, 1, 1, NVN_FORMAT_RGBA_ASTC_8x5), 8192, 1024, 1},
        // Cube RGBA8 16x16 with 5 levels: 1024 + 4 x 512 = 3072 per face, 6 faces (aligned to 1024: already aligned).
        {"cube RGBA8 16 L5", mk(NVN_TEXTURE_TARGET_CUBEMAP, 16, 16, 1, 5, NVN_FORMAT_RGBA8), 6 * 3072, 1024, 5},
        // 2D array R8 20x20, 2 layers, 2 levels: block height 2 (30 >= 16): level 0 is 64 x 32 = 2048, level 1 (10 rows, still 2 GOBs) is
        // 64 x 16 = 1024: 3072 per layer, already a multiple of the 1024 alignment -> 6144.
        {"2D array R8 20x20 x2 L2", mk(NVN_TEXTURE_TARGET_2D_ARRAY, 20, 20, 2, 2, NVN_FORMAT_R8), 6144, 1024, 2},
        // R8 3x1 asking for 10 levels: a 3-wide texture has 2 levels (floor(log2 3) + 1), 512 each.
        {"R8 3x1 L10 clamped", mk(NVN_TEXTURE_TARGET_2D, 3, 1, 1, 10, NVN_FORMAT_R8), 1024, 512, 2},
        // 1D R8 1000 wide: one block row, 1024-byte rows, 8 rows (block height 1): 8192.
        {"1D R8 1000", mk(NVN_TEXTURE_TARGET_1D, 1000, 1, 1, 1, NVN_FORMAT_R8), 8192, 512, 1},
        // 3D RGBA8 33x33x33, 1 level: 3 slabs of 16 slices, untrimmed (the game's buffers are untrimmed): 368640.
        {"3D RGBA8 33^3", mk(NVN_TEXTURE_TARGET_3D, 33, 33, 33, 1, NVN_FORMAT_RGBA8), 368640, 512, 1},
        // Pitch-linear R8 100x10: row 100 bytes padded to 32 -> 128, 10 rows: 1280; alignment 0x1000.
        {"linear R8 100x10", mk(NVN_TEXTURE_TARGET_2D, 100, 10, 1, 1, NVN_FORMAT_R8, NVN_TEXTURE_FLAGS_LINEAR), 1280, 0x1000, 1},
        // Multisample RGBA8 64x64, 4 samples: one level, 4 x 16384.
        {"MS RGBA8 64x64 x4", mk(NVN_TEXTURE_TARGET_2D_MULTISAMPLE, 64, 64, 1, 7, NVN_FORMAT_RGBA8), 4 * 16384, 4096, 1},
    };
    for (Case& c : cases) {
        if (std::strstr(c.name, "MS ")) c.b.samples = 4;
        const TexLayout L = be::compute_texture_layout(c.b);
        CHECK_MSG(L.valid, c.name);
        CHECK_MSG(L.storage_size == c.storage, c.name);
        CHECK_MSG(L.alignment == c.alignment, c.name);
        CHECK_MSG(L.levels == c.levels, c.name);
    }
}

NXTEST(layout_packaged_block_height_and_rejects) {
    // Packaged layout byte 0 is a log2 of the level-0 block height in GOBs (0 = 1 GOB): R8 20x20 at 1 GOB is 24 rows of 64.
    TextureBuilderInfo b = mk(NVN_TEXTURE_TARGET_2D, 20, 20, 1, 1, NVN_FORMAT_R8);
    const TexLayout def = be::compute_texture_layout(b);
    CHECK(def.valid);
    CHECK_EQ(def.storage_size, 2048u);   // 4 GOBs: 64 x 32 rows
    b.has_packaged_layout = true;
    b.packaged_layout[0] = 0;
    const TexLayout packed = be::compute_texture_layout(b);
    CHECK(packed.valid);
    CHECK_EQ(packed.storage_size, 1536u);   // 1 GOB: 64 x 24 rows
    CHECK_EQ(packed.alignment, 512u);
    b.packaged_layout[0] = 7;               // out of the range smo accepts (0..4): the default rule is used
    CHECK_EQ(be::compute_texture_layout(b).storage_size, 2048u);

    // A buffer texture (Switch 2 games make small ones at start-up) is one tightly packed row of width texels, one level, not block-linear.
    const TexLayout buf = be::compute_texture_layout(mk(NVN_TEXTURE_TARGET_BUFFER, 64, 1, 1, 1, NVN_FORMAT_R8));
    CHECK(buf.valid);
    CHECK(!buf.block_linear);
    CHECK_EQ(buf.levels, 1u);
    CHECK_EQ(buf.layers, 1u);
    CHECK_EQ(buf.pitch, 64u);
    CHECK_EQ(buf.storage_size, 64u);
    CHECK_EQ(buf.alignment, 0x20u);
    CHECK(!be::compute_texture_layout(mk(NVN_TEXTURE_TARGET_BUFFER, 0, 1, 1, 1, NVN_FORMAT_R8)).valid);   // a buffer texture still needs a width

    // Rejects: unknown format, zero size, array without layers, linear stride too small.
    CHECK(!be::compute_texture_layout(mk(NVN_TEXTURE_TARGET_2D, 4, 4, 1, 1, 0)).valid);
    CHECK(!be::compute_texture_layout(mk(NVN_TEXTURE_TARGET_2D, 0, 4, 1, 1, NVN_FORMAT_R8)).valid);
    CHECK(!be::compute_texture_layout(mk(NVN_TEXTURE_TARGET_2D_ARRAY, 4, 4, 0, 1, NVN_FORMAT_R8)).valid);
    TextureBuilderInfo lin = mk(NVN_TEXTURE_TARGET_2D, 100, 10, 1, 1, NVN_FORMAT_R8, NVN_TEXTURE_FLAGS_LINEAR);
    lin.stride = 64;   // a row of 100 bytes does not fit
    CHECK(!be::compute_texture_layout(lin).valid);
    lin.stride = 256;  // a wider pitch is fine
    CHECK_EQ(be::compute_texture_layout(lin).storage_size, 2560u);
}

// ================================================================================================================================
// Layout: an independent GOB formula, the tegra library, and the survey's GPU buffer sizes
// ================================================================================================================================

NXTEST(layout_random_sweep_matches_reference_and_tegra) {
    setup();
    struct F {
        int fmt;
        uint64_t bw, bh, bpb;
    };
    const F formats[] = {
        {NVN_FORMAT_R8, 1, 1, 1},
        {NVN_FORMAT_RGBA8, 1, 1, 4},
        {NVN_FORMAT_RGBA_DXT1, 4, 4, 8},
        {NVN_FORMAT_RGBA_ASTC_5x4, 5, 4, 16},
        {NVN_FORMAT_BPTC_UNORM, 4, 4, 16},
    };
    std::mt19937 rng(20261021u);
    int compared = 0;
    for (int i = 0; i < 400; ++i) {
        const F& f = formats[rng() % 5];
        const uint32_t w = 1 + rng() % 1100, h = 1 + rng() % 1100;
        const uint32_t requested = 1 + rng() % 13;
        const uint32_t layers = (rng() % 3 == 0) ? 1 + rng() % 4 : 1;
        TextureBuilderInfo b = mk(layers > 1 ? NVN_TEXTURE_TARGET_2D_ARRAY : NVN_TEXTURE_TARGET_2D, static_cast<int>(w),
                                  static_cast<int>(h), static_cast<int>(layers), static_cast<int>(requested), f.fmt);
        const TexLayout L = be::compute_texture_layout(b);
        CHECK_MSG(L.valid, "random case valid");
        const RefLayer r = ref_2d(w, h, ref_levels(w, h, requested), f.bw, f.bh, f.bpb);
        const uint64_t ref_total = layers > 1 ? align_up(r.bytes, r.align) * layers : r.bytes;
        CHECK_MSG(L.storage_size == ref_total, "storage equals the independent GOB formula");
        CHECK_MSG(L.alignment == r.align, "alignment equals the independent GOB formula");
        CHECK_MSG(L.levels == ref_levels(w, h, requested), "levels equal the clamp");
        // tegra on the same description: its tiled size for one layer equals the reference for one layer.
        if (layers == 1) {
            const tegra::Layout t = tegra::compute_layout(L.desc);
            CHECK_MSG(t.valid && t.tiled_size == r.bytes, "tegra compute_layout equals the reference (one layer)");
        }
        ++compared;
    }
    CHECK_EQ(compared, 400);
}

NXTEST(layout_survey_gpu_sizes) {
    // work/survey/textures/txtr_all.tsv: one row per texture file, with the GPU buffer size the game allocated (gpu_size).
    // All 47 3D textures are checked, plus 200 random rows; a row whose format name is not mapped here is counted and skipped.
    const std::string path = nxtest::project_path("work/survey/textures/txtr_all.tsv");
    std::ifstream in(path);
    if (!in) {
        skip("survey table not found");
        return;
    }
    std::vector<std::string> lines;
    for (std::string line; std::getline(in, line);) lines.push_back(line);
    struct Map {
        const char* fmt;
        int nvn;
    };
    // Survey format name -> NVN format.
    const Map map[] = {
        {"Astc8x5", NVN_FORMAT_RGBA_ASTC_8x5},         {"Astc8x5Srgb", NVN_FORMAT_RGBA_ASTC_8x5_SRGB},
        {"Astc5x4", NVN_FORMAT_RGBA_ASTC_5x4},         {"Astc5x5", NVN_FORMAT_RGBA_ASTC_5x5},
        {"Astc5x5Srgb", NVN_FORMAT_RGBA_ASTC_5x5_SRGB}, {"BptcUfloat", NVN_FORMAT_BPTC_UFLOAT},
        {"BptcSfloat", NVN_FORMAT_BPTC_SFLOAT},        {"BptcUnorm", NVN_FORMAT_BPTC_UNORM},
        {"BptcUnormSrgb", NVN_FORMAT_BPTC_UNORM_SRGB}, {"Bc5Unorm", NVN_FORMAT_RGTC2_UNORM},
        {"Bc4Unorm", NVN_FORMAT_RGTC1_UNORM},          {"Bc2Srgb", NVN_FORMAT_RGBA_DXT3_SRGB},
        {"Bc1Srgb", NVN_FORMAT_RGBA_DXT1_SRGB},        {"Rgba8Unorm", NVN_FORMAT_RGBA8},
        {"Rgba8Srgb", NVN_FORMAT_RGBA8_SRGB},          {"R8Unorm", NVN_FORMAT_R8},
    };
    enum class Row { Ok, Mismatch, Skipped };
    // Columns: path, id, kind, fmt, w, h, layers (the depth for 3D and 2DArray), mips, tile_mode, swizzle, ..., gpu_size (15).
    auto check_row = [&](const std::string& line) {
        std::vector<std::string> c;
        size_t p = 0;
        while (true) {
            const size_t q = line.find('\t', p);
            c.push_back(line.substr(p, q == std::string::npos ? std::string::npos : q - p));
            if (q == std::string::npos) break;
            p = q + 1;
        }
        if (c.size() < 16) return Row::Skipped;
        int nvn = -1;
        for (const Map& m : map)
            if (c[3] == m.fmt) nvn = m.nvn;
        if (nvn < 0) return Row::Skipped;
        TextureBuilderInfo b;
        b.width = std::stoi(c[4]);
        b.height = std::stoi(c[5]);
        b.format = nvn;
        b.levels = std::stoi(c[7]);
        const int extra = std::stoi(c[6]);
        if (c[2] == "3D") {
            b.target = NVN_TEXTURE_TARGET_3D;
            b.depth = extra;
        } else if (c[2] == "2DArray") {
            b.target = NVN_TEXTURE_TARGET_2D_ARRAY;
            b.depth = extra;
        } else if (c[2] == "Cube") {
            b.target = NVN_TEXTURE_TARGET_CUBEMAP;
            b.depth = 1;
        } else {
            b.target = NVN_TEXTURE_TARGET_2D;
            b.depth = 1;
        }
        const uint64_t gpu = std::stoull(c[15]);
        const TexLayout L = be::compute_texture_layout(b);
        if (!L.valid || L.storage_size != gpu) {
            CHECK_MSG(false, ("survey row " + c[1] + " " + c[2] + " " + c[3]).c_str());
            return Row::Mismatch;
        }
        return Row::Ok;
    };

    int threed = 0, threed_bad = 0;
    for (size_t i = 1; i < lines.size(); ++i)
        if (lines[i].find("\t3D\t") != std::string::npos) {
            ++threed;
            if (check_row(lines[i]) != Row::Ok) ++threed_bad;
        }

    std::vector<size_t> rows;
    for (size_t i = 1; i < lines.size(); ++i) rows.push_back(i);   // line 0 is the header
    std::mt19937 rng(7u);
    std::shuffle(rows.begin(), rows.end(), rng);
    int compared = 0, mismatches = 0, skipped = 0;
    for (size_t idx : rows) {
        if (compared == 200) break;
        const Row r = check_row(lines[idx]);
        if (r == Row::Skipped) {
            ++skipped;
            continue;
        }
        ++compared;
        if (r == Row::Mismatch) ++mismatches;
    }
    std::printf("survey: %d 3D rows (%d mismatches); %d random rows compared, %d mismatches, %d rows skipped (unmapped format or short row) of %zu data rows\n",
                threed, threed_bad, compared, mismatches, skipped, rows.size());
    CHECK_EQ(threed_bad, 0);
    CHECK_EQ(mismatches, 0);
    CHECK_EQ(compared, 200);
    CHECK_EQ(threed, 47);
}

// The sparse tile shape (64 KiB tiles, the standard sparse block shapes times the block size). The game's baked lighting volumes are
// 512 x 256 x 368 BC1 and BC6H 3D textures: tiles of 128 x 64 x 16 and 64 x 64 x 16 texels divide them exactly (368 = 23 * 16).
NXTEST(sparse_tile_shapes) {
    setup();
    Api a;
    Pool pool(1 << 16);
    struct Case { int target, format, w, h, d, tw, th, td; } cases[] = {
        {NVN_TEXTURE_TARGET_3D, NVN_FORMAT_RGBA_DXT1, 512, 256, 368, 128, 64, 16},
        {NVN_TEXTURE_TARGET_3D, NVN_FORMAT_BPTC_UFLOAT, 512, 256, 368, 64, 64, 16},
        {NVN_TEXTURE_TARGET_3D, NVN_FORMAT_RGBA8, 64, 64, 64, 32, 32, 16},
        {NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA_DXT1, 1024, 1024, 1, 512, 256, 1},
        {NVN_TEXTURE_TARGET_2D, NVN_FORMAT_BPTC_UNORM, 1024, 1024, 1, 256, 256, 1},
        {NVN_TEXTURE_TARGET_2D, NVN_FORMAT_R8, 1024, 1024, 1, 256, 256, 1},
        {NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA16F, 1024, 1024, 1, 128, 64, 1},
    };
    for (const Case& c : cases) {
        NVNtextureBuilder bld;
        build(a, bld, pool, static_cast<NVNtextureTarget>(c.target), c.w, c.h, c.d, 1, static_cast<NVNformat>(c.format));
        NVNtextureSparseTileLayout l{};
        a.bget_sparse(&bld, &l);
        CHECK_EQ(l.tileWidth, c.tw);
        CHECK_EQ(l.tileHeight, c.th);
        CHECK_EQ(l.tileDepth, c.td);
    }
}

// The storage of a sparse volume is tile-shaped: a 3D texture of 8 or 16 byte blocks takes the 64 KiB tile's block shape (2 GOBs high, 16 deep for
// the 512 x 256 x 368 BC1 volume) and its storage is whole tiles. An R8 volume's tile is 32 deep, which is outside that shape, so its layout is
// the ordinary one; without the sparse flag the volume keeps the ordinary layout.
NXTEST(sparse_volume_layout_is_tile_shaped) {
    const int sparse = NVN_TEXTURE_FLAGS_SPARSE;
    const TexLayout s = be::compute_texture_layout(mk(NVN_TEXTURE_TARGET_3D, 512, 256, 368, 1, NVN_FORMAT_RGBA_DXT1, sparse));
    CHECK(s.valid);
    CHECK_EQ(s.desc.gob_blocks_y, 2u);
    CHECK_EQ(s.desc.gob_blocks_z, 16u);
    CHECK_EQ(s.storage_size % 0x10000, uint64_t(0));
    CHECK(s.alignment >= 0x10000);

    const TexLayout plain = be::compute_texture_layout(mk(NVN_TEXTURE_TARGET_3D, 512, 256, 368, 1, NVN_FORMAT_RGBA_DXT1, 0));
    CHECK(plain.valid);
    CHECK_EQ(plain.desc.gob_blocks_y, 1u);
    CHECK_EQ(plain.desc.gob_blocks_z, 16u);

    const TexLayout r8 = be::compute_texture_layout(mk(NVN_TEXTURE_TARGET_3D, 512, 256, 368, 1, NVN_FORMAT_R8, sparse));
    CHECK(r8.valid);
    CHECK_EQ(r8.desc.gob_blocks_y, 1u);
    CHECK_EQ(r8.desc.gob_blocks_z, 16u);
}

// ================================================================================================================================
// Entry points: creation, getters, storage, teardown
// ================================================================================================================================

NXTEST(initialize_getters_and_finalize) {
    setup();
    Api a;
    Pool pool(1 << 16);
    NVNtextureBuilder bld;
    build(a, bld, pool, NVN_TEXTURE_TARGET_2D, 64, 64, 1, 7, NVN_FORMAT_RGBA8);
    CHECK_EQ(a.bget_size(&bld), 23552u);
    CHECK_EQ(a.bget_align(&bld), 4096u);
    CHECK_EQ(a.bget_class(&bld), 0);
    CHECK_EQ(a.bget_zcull(&bld), 0u);
    NVNtextureSparseTileLayout sparse{};
    sparse.numTiledLevels = 9;
    sparse.tileWidth = 7;
    a.bget_sparse(&bld, &sparse);
    CHECK_EQ(sparse.numTiledLevels, 0);
    CHECK_EQ(sparse.tileWidth, 128);   // a 64 KiB tile of 32-bit texels: 128 x 128
    CHECK_EQ(sparse.tileHeight, 128);
    CHECK_EQ(sparse.tileDepth, 1);

    const size_t before = be::texture_registry_size();
    NVNtexture tex;
    std::memset(&tex, 0, sizeof tex);
    CHECK_EQ(a.init(&tex, &bld), 1);
    CHECK_EQ(be::texture_registry_size(), before + 1);
    CHECK_EQ(a.width(&tex), 64);
    CHECK_EQ(a.height(&tex), 64);
    CHECK_EQ(a.depth(&tex), 1);
    CHECK_EQ(a.levels(&tex), 7);
    CHECK_EQ(static_cast<int>(a.target(&tex)), static_cast<int>(NVN_TEXTURE_TARGET_2D));
    CHECK_EQ(static_cast<int>(a.format(&tex)), static_cast<int>(NVN_FORMAT_RGBA8));
    CHECK_EQ(static_cast<int>(a.flags(&tex)), 0);
    CHECK_EQ(a.samples(&tex), 0);
    CHECK_EQ(static_cast<int>(a.dsmode(&tex)), 0);
    CHECK_EQ(static_cast<long long>(a.stride(&tex)), 0ll);
    CHECK_EQ(a.storage_size(&tex), 23552);
    CHECK_EQ(static_cast<long long>(a.pool_offset(&tex)), 0ll);
    CHECK(a.pool_of(&tex) == &pool.guest);
    CHECK_EQ(a.address(&tex), Pool::kGpuBase);
    CHECK_EQ(a.tclass(&tex), 0);
    CHECK_EQ(a.zcull(&tex), 0u);
    NVNtextureSwizzle r = NVN_TEXTURE_SWIZZLE_ZERO, g = r, bl = r, al = r;
    a.swizzle(&tex, &r, &g, &bl, &al);
    CHECK_EQ(static_cast<int>(r), static_cast<int>(NVN_TEXTURE_SWIZZLE_R));
    CHECK_EQ(static_cast<int>(al), static_cast<int>(NVN_TEXTURE_SWIZZLE_A));
    sparse.numTiledLevels = 9;
    sparse.tileWidth = 7;
    a.sparse(&tex, &sparse);
    CHECK_EQ(sparse.numTiledLevels, 0);
    CHECK_EQ(sparse.tileWidth, 128);
    CHECK_EQ(sparse.tileHeight, 128);
    CHECK_EQ(sparse.tileDepth, 1);
    CHECK(a.debug_id(&tex) != 0);
    a.label(&tex, "test texture");   // accepted, nothing to read back
    CHECK_EQ(a.compare(&tex, &tex), 1);

    // The host object's storage is the pool's memory at the offset.
    be::Texture* host = be::host_of<be::Texture>(&tex);
    CHECK(host != nullptr);
    CHECK(host && host->mem == pool.mem.data());
    CHECK(host && host->block_linear);
    CHECK(host && host->levels == 7);

    a.finalize(&tex);
    CHECK_EQ(be::texture_registry_size(), before);
    CHECK_EQ(a.width(&tex), 0);   // the guest object no longer names a host object
    CHECK(be::host_of<be::Texture>(&tex) == nullptr);
    a.finalize(&tex);             // a second finalize does nothing
}

NXTEST(initialize_rejects_bad_builders) {
    setup();
    Api a;
    Pool pool(1 << 16);
    NVNtextureBuilder bld;
    NVNtexture tex;
    std::memset(&tex, 0, sizeof tex);

    build(a, bld, pool, NVN_TEXTURE_TARGET_2D, 64, 64, 1, 1, 0);          // unknown format (NONE)
    CHECK_EQ(a.init(&tex, &bld), 0);
    CHECK(be::host_of<be::Texture>(&tex) == nullptr);

    build(a, bld, pool, NVN_TEXTURE_TARGET_2D, 64, 64, 1, 1, NVN_FORMAT_RGBA8, 1 << 16);   // storage past the pool's end
    CHECK_EQ(a.init(&tex, &bld), 0);

    build(a, bld, pool, NVN_TEXTURE_TARGET_2D, 64, 64, 1, 1, NVN_FORMAT_RGBA8, -4);        // negative offset
    CHECK_EQ(a.init(&tex, &bld), 0);

    std::memset(&bld, 0, sizeof bld);
    a.bdefaults(&bld);
    a.bsize2d(&bld, 16, 16);
    a.bformat(&bld, NVN_FORMAT_RGBA8);                                    // no storage set: no pool
    CHECK_EQ(a.init(&tex, &bld), 0);

    build(a, bld, pool, NVN_TEXTURE_TARGET_BUFFER, 0, 1, 1, 1, NVN_FORMAT_R8);            // a buffer texture of width 0 has no layout
    CHECK_EQ(a.init(&tex, &bld), 0);
    CHECK(be::host_of<be::Texture>(&tex) == nullptr);
}

NXTEST(builder_storage_queries_match_the_texture) {
    setup();
    Api a;
    Pool pool(1 << 20);
    NVNtextureBuilder bld;
    build(a, bld, pool, NVN_TEXTURE_TARGET_2D_ARRAY, 20, 20, 2, 2, NVN_FORMAT_R8);
    CHECK_EQ(a.bget_size(&bld), 6144u);
    CHECK_EQ(a.bget_align(&bld), 1024u);
    NVNtexture tex;
    std::memset(&tex, 0, sizeof tex);
    CHECK_EQ(a.init(&tex, &bld), 1);
    CHECK_EQ(a.storage_size(&tex), 6144);
    // Layer 1 starts one aligned layer (3072 bytes) in: the layer stride.
    NVNtextureView v;
    a.vdefaults(&v);
    a.vlayers(&v, 1, 1);
    CHECK_EQ(static_cast<long long>(a.view_offset(&tex, &v)), 3072ll);
    a.finalize(&tex);
}

// ================================================================================================================================
// Texel access: write, read back, and check that the rest of the texture is untouched
// ================================================================================================================================

NXTEST(write_read_round_trip_level0_and_level2) {
    setup();
    Api a;
    Pool pool(1 << 16);
    NVNtextureBuilder bld;
    build(a, bld, pool, NVN_TEXTURE_TARGET_2D, 64, 64, 1, 7, NVN_FORMAT_RGBA8);
    NVNtexture tex;
    std::memset(&tex, 0, sizeof tex);
    CHECK_EQ(a.init(&tex, &bld), 1);

    // Level 0: the whole 64x64 image.
    std::vector<uint8_t> src0(64 * 64 * 4), back(src0.size());
    fill(src0, 1);
    be::Texture* host = be::host_of<be::Texture>(&tex);
    host->cpu_dirty.store(false);
    NVNcopyRegion full0 = region(0, 0, 0, 64, 64, 1);
    a.write(&tex, nullptr, &full0, src0.data());
    CHECK(host->cpu_dirty.load());                  // a write marks the texture for upload
    a.read(&tex, nullptr, &full0, back.data());
    CHECK(std::memcmp(src0.data(), back.data(), src0.size()) == 0);
    // The storage is block-linear, not row by row: texels 0..3 of row 0 are the first 16 bytes. Where the next 16-byte piece of the row goes
    // is the GOB byte order of the console (tegra_tiling.h gob_switch2), which follows the registered game's platform: texels 4..7 start at
    // byte 32 on Switch 1 (row 1 at 16) and at byte 64 on Switch 2 (rows 1..3 at 16, 32, 48 first). Both are checked in
    // block_linear_gob_order_follows_the_platform.
    const bool sw2 = nxrt::game::info().platform == nxrt::game::Platform::Switch2;
    CHECK(std::memcmp(&pool.mem[0], &src0[0], 16) == 0);
    CHECK(std::memcmp(&pool.mem[sw2 ? 64 : 32], &src0[16], 16) == 0);
    CHECK(std::memcmp(&pool.mem[16], &src0[64 * 4], 16) == 0);   // row 1, texels 0..3: byte 16 on both

    // Level 2 (16x16) through a view: other levels keep their data.
    std::vector<uint8_t> src2(16 * 16 * 4), back2(src2.size());
    fill(src2, 5);
    NVNtextureView v;
    a.vdefaults(&v);
    a.vlevels(&v, 2, 1);
    NVNcopyRegion full2 = region(0, 0, 0, 16, 16, 1);
    a.write(&tex, &v, &full2, src2.data());
    a.read(&tex, &v, &full2, back2.data());
    CHECK(std::memcmp(src2.data(), back2.data(), src2.size()) == 0);
    a.read(&tex, nullptr, &full0, back.data());
    CHECK(std::memcmp(src0.data(), back.data(), src0.size()) == 0);

    // Level 2 starts at the offset the layout gives it (the view offset of the layer).
    const TexLayout L = be::compute_texture_layout(host->b);
    CHECK_EQ(static_cast<long long>(a.view_offset(&tex, &v)), static_cast<long long>(L.layout.levels[2].tiled_offset));

    // A region inside level 0 changes only that region; every other texel keeps its value.
    std::vector<uint8_t> patch(8 * 4 * 4);
    fill(patch, 9);
    NVNcopyRegion inner = region(4, 8, 0, 8, 4, 1);
    a.write(&tex, nullptr, &inner, patch.data());
    std::vector<uint8_t> after(src0.size());
    a.read(&tex, nullptr, &full0, after.data());
    bool same = true;
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) {
            const bool inside = x >= 4 && x < 12 && y >= 8 && y < 12;
            const uint8_t* want = inside ? &patch[((y - 8) * 8 + (x - 4)) * 4] : &src0[(y * 64 + x) * 4];
            if (std::memcmp(&after[(y * 64 + x) * 4], want, 4) != 0) same = false;
        }
    CHECK(same);

    a.flush(&tex, nullptr, &full0);
    a.invalidate(&tex, nullptr, &full0);
    a.finalize(&tex);
}

// The GOB byte order of a block-linear texture is the console's (tegra_tiling.h gob_piece_offset): a 16-byte piece p of row y inside a GOB
// lands at Switch 1: (y & 1) * 16 + (p & 1) * 32 + (y >> 1) * 64 (bits 4..8 = y0 x4 y1 y2 x5), Switch 2: (y & 1) * 16 + (y >> 1 & 1) * 32 +
// (p & 1) * 64 + (y >> 2) * 128 (bits 4..8 = y0 y1 x4 y2 x5). The texture takes the order of the platform it was made under.
NXTEST(block_linear_gob_order_follows_the_platform) {
    for (const nxrt::game::Platform plat : {nxrt::game::Platform::Switch1, nxrt::game::Platform::Switch2}) {
        nxtest::ScopedPlatform sp(plat);
        const bool sw2 = plat == nxrt::game::Platform::Switch2;
        setup();
        Api a;
        Pool pool(1 << 16);
        NVNtextureBuilder bld;
        build(a, bld, pool, NVN_TEXTURE_TARGET_2D, 64, 64, 1, 1, NVN_FORMAT_RGBA8);
        NVNtexture tex;
        std::memset(&tex, 0, sizeof tex);
        REQUIRE(a.init(&tex, &bld) == 1);
        std::vector<uint8_t> src(64 * 64 * 4);
        fill(src, 3);
        NVNcopyRegion full = region(0, 0, 0, 64, 64, 1);
        a.write(&tex, nullptr, &full, src.data());
        auto piece = [&](uint32_t y, uint32_t p) { return &src[(y * 64 + p * 4) * 4]; };   // texels 4p..4p+3 of row y
        const uint32_t s1[4][2] = {{0, 32}, {16, 48}, {64, 96}, {80, 112}};      // [y][p] byte offsets, Switch 1
        const uint32_t s2[4][2] = {{0, 64}, {16, 80}, {32, 96}, {48, 112}};      // Switch 2
        for (uint32_t y = 0; y < 4; ++y)
            for (uint32_t p = 0; p < 2; ++p) CHECK_MSG(std::memcmp(&pool.mem[(sw2 ? s2 : s1)[y][p]], piece(y, p), 16) == 0, sw2 ? "Switch 2" : "Switch 1");
        a.finalize(&tex);
    }
}

NXTEST(builder_packaged_layout_and_stride_change_the_size) {
    setup();
    Api a;
    Pool pool(1 << 16);
    NVNtextureBuilder bld;
    build(a, bld, pool, NVN_TEXTURE_TARGET_2D, 20, 20, 1, 1, NVN_FORMAT_R8);
    CHECK_EQ(a.bget_size(&bld), 2048u);
    NVNpackagedTextureLayout pk;
    std::memset(&pk, 0, sizeof pk);
    a.blayout(&bld, &pk);                 // byte 0 = 0: one GOB of block height
    CHECK_EQ(a.bget_size(&bld), 1536u);
    pk.layout[0] = 7;                     // not a log2 the rule accepts: the default is used again
    a.blayout(&bld, &pk);
    CHECK_EQ(a.bget_size(&bld), 2048u);
    // Pitch-linear with an explicit stride: 256 bytes per row, 20 rows.
    a.bflags(&bld, NVN_TEXTURE_FLAGS_LINEAR);
    a.bstride(&bld, 256);
    CHECK_EQ(a.bget_size(&bld), 5120u);
    CHECK_EQ(a.bget_align(&bld), 4096u);
}

NXTEST(array_layer_and_view_offset) {
    setup();
    Api a;
    Pool pool(1 << 16);
    NVNtextureBuilder bld;
    build(a, bld, pool, NVN_TEXTURE_TARGET_2D_ARRAY, 16, 16, 3, 1, NVN_FORMAT_RGBA8);
    NVNtexture tex;
    std::memset(&tex, 0, sizeof tex);
    CHECK_EQ(a.init(&tex, &bld), 1);
    CHECK_EQ(a.storage_size(&tex), 3 * 1024 + 0);   // 1024 per layer (16 x 16 RGBA8 at block height 2), aligned 1024

    std::vector<uint8_t> data(16 * 16 * 4), zero(data.size(), 0), back(data.size());
    fill(data, 3);
    NVNtextureView v;
    a.vdefaults(&v);
    a.vlayers(&v, 1, 1);
    NVNcopyRegion full = region(0, 0, 0, 16, 16, 1);
    a.write(&tex, &v, &full, data.data());
    a.read(&tex, &v, &full, back.data());
    CHECK(std::memcmp(data.data(), back.data(), data.size()) == 0);
    a.vlayers(&v, 0, 1);
    a.read(&tex, &v, &full, back.data());
    CHECK(std::memcmp(zero.data(), back.data(), zero.size()) == 0);   // layer 0 was not written
    a.vlayers(&v, 2, 1);
    a.read(&tex, &v, &full, back.data());
    CHECK(std::memcmp(zero.data(), back.data(), zero.size()) == 0);
    a.vlayers(&v, 1, 1);
    CHECK_EQ(static_cast<long long>(a.view_offset(&tex, &v)), 1024ll);
    a.finalize(&tex);
}

NXTEST(bc1_block_aligned_region) {
    setup();
    Api a;
    Pool pool(1 << 14);
    NVNtextureBuilder bld;
    build(a, bld, pool, NVN_TEXTURE_TARGET_2D, 16, 16, 1, 1, NVN_FORMAT_RGBA_DXT1);
    NVNtexture tex;
    std::memset(&tex, 0, sizeof tex);
    CHECK_EQ(a.init(&tex, &bld), 1);
    CHECK_EQ(a.storage_size(&tex), 512);

    // Two blocks per row, two rows of blocks: 8 x 8 texels at (4, 8). Each block is 8 bytes.
    std::vector<uint8_t> src(2 * 2 * 8), back(src.size());
    fill(src, 11);
    NVNcopyRegion blk = region(4, 8, 0, 8, 8, 1);
    a.write(&tex, nullptr, &blk, src.data());
    a.read(&tex, nullptr, &blk, back.data());
    CHECK(std::memcmp(src.data(), back.data(), src.size()) == 0);

    // A region that does not start on a block boundary is not written (the call does nothing).
    std::vector<uint8_t> whole(16 * 16 / 16 * 8), before(whole.size()), after(whole.size());
    NVNcopyRegion all = region(0, 0, 0, 16, 16, 1);
    a.read(&tex, nullptr, &all, before.data());
    std::vector<uint8_t> bad(8, 0xEE);
    NVNcopyRegion off = region(2, 0, 0, 4, 4, 1);
    a.write(&tex, nullptr, &off, bad.data());
    a.read(&tex, nullptr, &all, after.data());
    CHECK(std::memcmp(before.data(), after.data(), before.size()) == 0);
    a.finalize(&tex);
}

NXTEST(pitch_linear_round_trip_and_strides) {
    setup();
    Api a;
    Pool pool(1 << 16);
    NVNtextureBuilder bld;
    build(a, bld, pool, NVN_TEXTURE_TARGET_2D, 100, 10, 1, 1, NVN_FORMAT_R8, 0, NVN_TEXTURE_FLAGS_LINEAR);
    NVNtexture tex;
    std::memset(&tex, 0, sizeof tex);
    CHECK_EQ(a.init(&tex, &bld), 1);
    CHECK_EQ(a.storage_size(&tex), 1280);
    CHECK_EQ(a.bget_align(&bld), 4096u);
    be::Texture* host = be::host_of<be::Texture>(&tex);
    CHECK(host && !host->block_linear);

    // Rows of 100 bytes at a 128-byte pitch: write row y with value y, then check the pool memory directly.
    std::vector<uint8_t> rows(100 * 10);
    for (int y = 0; y < 10; ++y)
        for (int x = 0; x < 100; ++x) rows[y * 100 + x] = static_cast<uint8_t>(y * 16 + x);
    NVNcopyRegion full = region(0, 0, 0, 100, 10, 1);
    a.write(&tex, nullptr, &full, rows.data());
    for (int y = 0; y < 10; ++y)
        CHECK(std::memcmp(&pool.mem[y * 128], &rows[y * 100], 100) == 0);
    std::vector<uint8_t> back(rows.size());
    a.read(&tex, nullptr, &full, back.data());
    CHECK(std::memcmp(rows.data(), back.data(), rows.size()) == 0);

    // Strided: the caller's rows are 160 bytes apart; read them back at 200 bytes apart.
    std::vector<uint8_t> wide(160 * 10, 0);
    for (int y = 0; y < 10; ++y) std::memcpy(&wide[y * 160], &rows[y * 100], 100);
    NVNcopyRegion part = region(10, 2, 0, 20, 4, 1);
    a.write_s(&tex, nullptr, &part, &wide[2 * 160 + 0], 160, 0);   // rows 2..5 of wide, starting at x 0
    std::vector<uint8_t> spread(200 * 4, 0);
    a.read_s(&tex, nullptr, &part, spread.data(), 200, 0);
    for (int y = 0; y < 4; ++y)
        CHECK(std::memcmp(&spread[y * 200], &wide[(2 + y) * 160], 20) == 0);
    a.finalize(&tex);
}

NXTEST(strided_block_linear_round_trip) {
    setup();
    Api a;
    Pool pool(1 << 16);
    NVNtextureBuilder bld;
    build(a, bld, pool, NVN_TEXTURE_TARGET_2D, 16, 16, 1, 1, NVN_FORMAT_RGBA8);
    NVNtexture tex;
    std::memset(&tex, 0, sizeof tex);
    CHECK_EQ(a.init(&tex, &bld), 1);
    // 8 x 8 texels, caller rows 64 bytes apart (wider than 32), images 1024 bytes apart (unused for 2D).
    std::vector<uint8_t> src(64 * 8, 0);
    fill(src, 21);
    NVNcopyRegion reg = region(8, 8, 0, 8, 8, 1);
    a.write_s(&tex, nullptr, &reg, src.data(), 64, 1024);
    std::vector<uint8_t> out(64 * 8, 0);
    a.read_s(&tex, nullptr, &reg, out.data(), 64, 1024);
    for (int y = 0; y < 8; ++y) CHECK(std::memcmp(&out[y * 64], &src[y * 64], 32) == 0);
    a.finalize(&tex);
}

NXTEST(flush_marks_dirty_and_views_and_compare) {
    setup();
    Api a;
    Pool pool(1 << 16);
    NVNtextureBuilder bld;
    build(a, bld, pool, NVN_TEXTURE_TARGET_2D, 32, 32, 1, 1, NVN_FORMAT_RGBA8);
    NVNtexture t1, t2;
    std::memset(&t1, 0, sizeof t1);
    std::memset(&t2, 0, sizeof t2);
    CHECK_EQ(a.init(&t1, &bld), 1);
    CHECK_EQ(a.init(&t2, &bld), 1);   // same storage, different host object
    CHECK(a.debug_id(&t1) != a.debug_id(&t2));
    CHECK_EQ(a.compare(&t1, &t2), 1);   // same storage address and size
    CHECK_EQ(a.compare(&t1, &t1), 1);

    be::host_of<be::Texture>(&t1)->cpu_dirty.store(false);
    a.flush(&t1, nullptr, nullptr);
    CHECK(be::host_of<be::Texture>(&t1)->cpu_dirty.load());

    NVNtexture other;
    std::memset(&other, 0, sizeof other);
    build(a, bld, pool, NVN_TEXTURE_TARGET_2D, 32, 32, 1, 1, NVN_FORMAT_RGBA8, 16384);
    CHECK_EQ(a.init(&other, &bld), 1);
    CHECK_EQ(a.compare(&t1, &other), 0);   // different storage

    // Same storage, different channel mapping or format: different (layout captures are rebuilt this way)
    auto bswz = abi::proc<PFNNVNTEXTUREBUILDERSETSWIZZLEPROC>("nvnTextureBuilderSetSwizzle");
    NVNtexture swz, fmt;
    std::memset(&swz, 0, sizeof swz);
    std::memset(&fmt, 0, sizeof fmt);
    build(a, bld, pool, NVN_TEXTURE_TARGET_2D, 32, 32, 1, 1, NVN_FORMAT_RGBA8);
    bswz(&bld, NVN_TEXTURE_SWIZZLE_ONE, NVN_TEXTURE_SWIZZLE_ONE, NVN_TEXTURE_SWIZZLE_ONE, NVN_TEXTURE_SWIZZLE_R);
    CHECK_EQ(a.init(&swz, &bld), 1);
    CHECK_EQ(a.compare(&t1, &swz), 0);
    build(a, bld, pool, NVN_TEXTURE_TARGET_2D, 32, 32, 1, 1, NVN_FORMAT_RGBA8UI);
    CHECK_EQ(a.init(&fmt, &bld), 1);
    CHECK_EQ(a.compare(&t1, &fmt), 0);
    a.finalize(&swz);
    a.finalize(&fmt);
    CHECK_EQ(static_cast<long long>(a.pool_offset(&other)), 16384ll);

    // Null out-pointers and null views are allowed.
    a.swizzle(&t1, nullptr, nullptr, nullptr, nullptr);
    a.sparse(&t1, nullptr);
    CHECK_EQ(a.storage_size(&t1), 4096);

    // Pool memory returned by the getter is the builder's pool.
    CHECK(a.pool_of(&t1) == &pool.guest);
    a.finalize(&other);
    a.finalize(&t1);
    a.finalize(&t2);
}

NXTEST(texture_entry_points_are_only_bound_to_their_own_objects) {
    setup();
    Api a;
    Pool pool(1 << 14);
    NVNtexture never;
    std::memset(&never, 0, sizeof never);
    // An uninitialized texture: getters return zero, texel calls do nothing, finalize does nothing.
    CHECK_EQ(a.width(&never), 0);
    CHECK_EQ(a.debug_id(&never), 0u);
    CHECK_EQ(a.storage_size(&never), 0);
    NVNcopyRegion r = region(0, 0, 0, 4, 4, 1);
    uint8_t buf[64] = {};
    a.write(&never, nullptr, &r, buf);
    a.read(&never, nullptr, &r, buf);
    a.finalize(&never);
    (void)pool;
}
