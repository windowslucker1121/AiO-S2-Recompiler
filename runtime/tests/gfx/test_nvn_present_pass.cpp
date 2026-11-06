// SPDX-License-Identifier: GPL-3.0-or-later
// NVN present pass (src/gfx/nvn/nvn_present_pass.cpp), headless: a 64x48 RGBA8 UNORM source with a known pattern is drawn by the pass
// into the renderer's frame image (20 frames, so the 16-set descriptor ring wraps), the last frame is captured and compared with an
// expected image computed on the CPU with the same geometry. Validation must report nothing for the whole group.
//
// Tolerance of the linear cases: the GPU bilinear filter works with fixed-point weights, so the result can differ from the exact
// blend by about one 8-bit step. Measured on this machine the largest difference was 1 LSB (mean 0.30); kLinearMaxAbs = 2 leaves
// one step of margin for other drivers and nothing more. The nearest cases are compared exactly.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "nvn_present_pass.h"
#include "nxrt/gfx.h"
#include "test_framework.h"
#include "validation_layer.h"
NXTEST_GROUP(gfx_nvn_present_pass)

using namespace nxrt;
using namespace nxrt::gfx;
using nxrt::nvn::be::PresentPass;

namespace {

constexpr uint32_t kSrcW = 64, kSrcH = 48;
constexpr int kFrames = 20;              // frames per case; the last one is captured (descriptor ring of 16 wraps)
constexpr int kLinearMaxAbs = 2;         // see the header comment
constexpr double kLinearMeanAbs = 0.5;

// Source pattern: R = horizontal gradient, G = vertical gradient, B = 128, A = 255. In the top left 4x4 corner a one-texel checker
// (R and G swap, B = 0): the orientation shows in every case.
std::vector<uint8_t> source_pattern() {
    std::vector<uint8_t> px(static_cast<size_t>(kSrcW) * kSrcH * 4);
    for (uint32_t y = 0; y < kSrcH; ++y)
        for (uint32_t x = 0; x < kSrcW; ++x) {
            uint8_t* p = &px[(static_cast<size_t>(y) * kSrcW + x) * 4];
            p[0] = static_cast<uint8_t>((x * 255) / (kSrcW - 1));
            p[1] = static_cast<uint8_t>((y * 255) / (kSrcH - 1));
            p[2] = 128;
            p[3] = 255;
            if (x < 4 && y < 4) {
                const bool on = ((x + y) & 1) != 0;
                p[0] = on ? 255 : 0;
                p[1] = on ? 0 : 255;
                p[2] = 0;
            }
        }
    return px;
}

// Uploads the pattern into a SHADER_READ_ONLY_OPTIMAL image (copy through a staging buffer, then the layout transition).
bool make_source(Device& dev, Image* out) {
    const std::vector<uint8_t> px = source_pattern();
    ImageDesc id;
    id.format = VK_FORMAT_R8G8B8A8_UNORM;
    id.width = kSrcW;
    id.height = kSrcH;
    id.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    id.name = "present test source";
    std::string why;
    if (dev.create_image(id, out, &why) != VK_SUCCESS) {
        std::printf("    source image: %s\n", why.c_str());
        return false;
    }
    Buffer stage;
    if (dev.create_buffer(px.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, MemoryUse::Upload, &stage, "present test staging") != VK_SUCCESS) return false;
    std::memcpy(stage.mapped, px.data(), px.size());
    dev.flush_buffer(stage);
    const bool ok = dev.submit_once([&](VkCommandBuffer cmd) {
        VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
        b.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = out->image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        di.imageMemoryBarrierCount = 1;
        di.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &di);
        VkBufferImageCopy2 region = {VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {kSrcW, kSrcH, 1};
        VkCopyBufferToImageInfo2 ci = {VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2};
        ci.srcBuffer = stage.buffer;
        ci.dstImage = out->image;
        ci.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        ci.regionCount = 1;
        ci.pRegions = &region;
        vkCmdCopyBufferToImage2(cmd, &ci);
        b.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier2(cmd, &di);
    });
    dev.destroy_buffer(stage);
    return ok;
}

struct Rig {
    std::unique_ptr<Device> dev;
    std::unique_ptr<Renderer> rend;
    PresentPass pass;
    Image src;
};

// Headless device (validation on), a renderer of the given size, the pass on the renderer's format, the source image.
bool make_rig(Rig& r, uint32_t w, uint32_t h) {
    DeviceDesc dd;
    dd.validation = 1;
    dd.headless = 1;
    std::string err;
    r.dev = Device::create(dd, &err);
    if (!r.dev) {
        std::printf("    device: %s\n", err.c_str());
        return false;
    }
    gfxtest::validation_layer_live(*r.dev);   // the zero counts below mean something only with the layer on
    RendererDesc rd;
    rd.headless_width = w;
    rd.headless_height = h;
    r.rend = Renderer::create(*r.dev, rd, &err);
    if (!r.rend) {
        std::printf("    renderer: %s\n", err.c_str());
        return false;
    }
    if (!r.pass.init(*r.dev, r.rend->format())) return false;
    return make_source(*r.dev, &r.src);
}

// Frees what the rig made, then checks the validation counters and that the device holds no allocation.
void teardown(Rig& r) {
    if (!r.dev) return;
    r.dev->wait_idle();
    r.pass.shutdown();
    r.dev->destroy_image(r.src);
    r.rend.reset();
    const ValidationStats v = r.dev->validation_stats();
    std::printf("    validation: %llu errors, %llu warnings\n", static_cast<unsigned long long>(v.errors), static_cast<unsigned long long>(v.warnings));
    if (!v.first_error.empty()) std::printf("      first error: %s\n", v.first_error.c_str());
    CHECK_EQ(v.errors, 0ull);
    CHECK_EQ(v.warnings, 0ull);
    CHECK_EQ(r.dev->memory_stats().allocations, 0ull);
    r.dev.reset();
}

// Renders kFrames frames of the pass into the renderer's frame image and captures the last one (RGBA8, opaque, tightly packed).
bool render(Rig& r, uint32_t w, uint32_t h, const int32_t* crop, bool keep_aspect, std::vector<uint8_t>* out, uint32_t* cw, uint32_t* ch) {
    if (r.rend->extent().width != w || r.rend->extent().height != h)
        if (!r.rend->resize_headless(w, h)) return false;
    for (int i = 0; i < kFrames; ++i) {
        FrameInfo fi;
        if (!r.rend->begin_frame(&fi)) return false;
        if (i == kFrames - 1) r.rend->capture_next_frame(out, cw, ch);
        r.pass.record(fi.cmd, fi.view, fi.extent, r.src.view, VkExtent2D{kSrcW, kSrcH}, crop, keep_aspect);
        r.rend->end_frame();
    }
    return true;
}

// The expected picture, computed on the CPU with the geometry the shader uses (double precision): the picture rectangle, the crop,
// the texel under each pixel centre (nearest, 1:1), or a bilinear blend of the four texel centres around it, clamped to the crop.
// *nearest_out says which filter the pass must have used.
std::vector<uint8_t> expected(const std::vector<uint8_t>& src, uint32_t dw, uint32_t dh, const int32_t* crop, bool keep_aspect, bool* nearest_out) {
    double cx = 0, cy = 0, cw = kSrcW, ch = kSrcH;
    if (crop && crop[2] > 0 && crop[3] > 0) {
        cx = crop[0];
        cy = crop[1];
        cw = crop[2];
        ch = crop[3];
    }
    double px = 0, py = 0, pw = dw, ph = dh;
    if (keep_aspect) {
        const double a = cw / ch, b = static_cast<double>(dw) / dh;
        if (a > b) {
            pw = dw;
            ph = dw / a;
        } else {
            ph = dh;
            pw = dh * a;
        }
        px = (dw - pw) * 0.5;
        py = (dh - ph) * 0.5;
    }
    const bool nearest = std::fabs(pw - cw) < 1e-9 && std::fabs(ph - ch) < 1e-9;
    *nearest_out = nearest;
    auto texel = [&](int x, int y, int c) { return static_cast<double>(src[(static_cast<size_t>(y) * kSrcW + x) * 4 + c]); };
    auto clampd = [](double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); };
    std::vector<uint8_t> out(static_cast<size_t>(dw) * dh * 4, 0);
    for (uint32_t y = 0; y < dh; ++y)
        for (uint32_t x = 0; x < dw; ++x) {
            uint8_t* o = &out[(static_cast<size_t>(y) * dw + x) * 4];
            o[3] = 255;
            const double lx = ((x + 0.5) - px) / pw, ly = ((y + 0.5) - py) / ph;
            if (lx < 0 || ly < 0 || lx > 1 || ly > 1) continue;   // letterbox bar: black
            const double tx = clampd(cx + lx * cw, cx + 0.5, cx + cw - 0.5);
            const double ty = clampd(cy + ly * ch, cy + 0.5, cy + ch - 0.5);
            if (nearest) {
                const int ix = static_cast<int>(std::floor(tx)), iy = static_cast<int>(std::floor(ty));
                for (int c = 0; c < 3; ++c) o[c] = static_cast<uint8_t>(texel(ix, iy, c));
                continue;
            }
            const double fx = tx - 0.5, fy = ty - 0.5;
            const int x0 = static_cast<int>(std::floor(fx)), y0 = static_cast<int>(std::floor(fy));
            const double wx = fx - x0, wy = fy - y0;
            const int xa = static_cast<int>(clampd(x0, cx, cx + cw - 1)), xb = static_cast<int>(clampd(x0 + 1, cx, cx + cw - 1));
            const int ya = static_cast<int>(clampd(y0, cy, cy + ch - 1)), yb = static_cast<int>(clampd(y0 + 1, cy, cy + ch - 1));
            for (int c = 0; c < 3; ++c) {
                const double top = (1 - wx) * texel(xa, ya, c) + wx * texel(xb, ya, c);
                const double bot = (1 - wx) * texel(xa, yb, c) + wx * texel(xb, yb, c);
                o[c] = static_cast<uint8_t>(std::floor((1 - wy) * top + wy * bot + 0.5));
            }
        }
    return out;
}

struct Diff {
    int max_abs = 0;
    double mean_abs = 0;
};

// Per channel difference of the colour channels (alpha is forced by the capture, not by the pass).
Diff diff_rgb(const std::vector<uint8_t>& got, const std::vector<uint8_t>& want) {
    Diff d;
    const size_t n = got.size() / 4;
    double sum = 0;
    for (size_t i = 0; i < n; ++i)
        for (int c = 0; c < 3; ++c) {
            const int e = std::abs(static_cast<int>(got[i * 4 + c]) - static_cast<int>(want[i * 4 + c]));
            if (e > d.max_abs) d.max_abs = e;
            sum += e;
        }
    d.mean_abs = sum / (3.0 * static_cast<double>(n));
    return d;
}

// Renders one case, compares it with the CPU expectation and reports the numbers.
void check_case(Rig& r, const char* what, uint32_t dw, uint32_t dh, const int32_t* crop, bool keep_aspect, bool want_nearest, bool exact) {
    const std::vector<uint8_t> src = source_pattern();
    std::vector<uint8_t> got;
    uint32_t w = 0, h = 0;
    REQUIRE(render(r, dw, dh, crop, keep_aspect, &got, &w, &h));
    CHECK_EQ(w, dw);
    CHECK_EQ(h, dh);
    bool nearest = false;
    const std::vector<uint8_t> want = expected(src, dw, dh, crop, keep_aspect, &nearest);
    CHECK_EQ(nearest, want_nearest);
    REQUIRE(got.size() == want.size());
    const Diff d = diff_rgb(got, want);
    std::printf("    %s (%ux%u, %s): max abs %d, mean abs %.4f\n", what, dw, dh, nearest ? "nearest" : "linear", d.max_abs, d.mean_abs);
    if (exact) {
        CHECK_EQ(d.max_abs, 0);
    } else {
        CHECK(d.max_abs <= kLinearMaxAbs);
        CHECK(d.mean_abs <= kLinearMeanAbs);
    }
}

}  // namespace

NXTEST(init_and_shutdown_release_everything) {
    Rig r;
    REQUIRE(make_rig(r, 64, 48));
    CHECK(r.pass.initialised());
    CHECK(r.pass.init(*r.dev, r.rend->format()));   // a second init is a no-op
    r.pass.shutdown();
    CHECK(!r.pass.initialised());
    r.pass.shutdown();                              // and so is a second shutdown
    teardown(r);
}

NXTEST(stretch_full_source_linear_matches_cpu_bilinear) {
    // 256x192 is an integer (4x) stretch; 100x70 is not, so the weights are not trivial. A null crop and an empty crop both mean
    // the whole source: the first case passes null, the second an empty crop.
    Rig r;
    REQUIRE(make_rig(r, 256, 192));
    check_case(r, "full source, integer stretch", 256, 192, nullptr, false, false, false);
    const int32_t empty[4] = {0, 0, 0, 0};
    check_case(r, "full source, empty crop", 100, 70, empty, false, false, false);
    teardown(r);
}

NXTEST(one_to_one_copy_is_bit_exact) {
    // Destination equals the source size: nearest sampling, every pixel is one texel, the copy must match to the bit.
    Rig r;
    REQUIRE(make_rig(r, 64, 48));
    check_case(r, "full source 1:1", 64, 48, nullptr, false, true, true);
    // and a 1:1 crop, to show the offset is right as well: the 32x24 region at {16, 8}
    const int32_t crop[4] = {16, 8, 32, 24};
    check_case(r, "crop 1:1", 32, 24, crop, false, true, true);
    teardown(r);
}

NXTEST(crop_16_8_32_24_is_stretched_and_filtered) {
    // The region x 16..47, y 8..31 of the source, shown over the whole 64x48 target (2x, linear).
    Rig r;
    REQUIRE(make_rig(r, 64, 48));
    const int32_t crop[4] = {16, 8, 32, 24};
    check_case(r, "crop {16, 8, 32, 24}", 64, 48, crop, false, false, false);
    teardown(r);
}

NXTEST(keep_aspect_letterbox_bars_are_black_and_picture_is_exact) {
    // 64x48 (4:3) into 256x256: the picture is 256x192 in the rows 32..223, black above and below.
    Rig r;
    REQUIRE(make_rig(r, 256, 256));
    const std::vector<uint8_t> src = source_pattern();
    std::vector<uint8_t> got;
    uint32_t w = 0, h = 0;
    REQUIRE(render(r, 256, 256, nullptr, true, &got, &w, &h));
    CHECK_EQ(w, 256u);
    CHECK_EQ(h, 256u);
    size_t bar_bad = 0;
    for (uint32_t y = 0; y < 256; ++y)
        for (uint32_t x = 0; x < 256; ++x) {
            if (y >= 32 && y < 224) continue;
            const uint8_t* p = &got[(static_cast<size_t>(y) * 256 + x) * 4];
            if (p[0] != 0 || p[1] != 0 || p[2] != 0) ++bar_bad;
        }
    std::printf("    letterbox bars: %zu pixels not black\n", bar_bad);
    CHECK_EQ(bar_bad, 0u);
    bool nearest = true;
    const std::vector<uint8_t> want = expected(src, 256, 256, nullptr, true, &nearest);
    CHECK(!nearest);
    std::vector<uint8_t> got_band, want_band;
    for (uint32_t y = 32; y < 224; ++y) {
        got_band.insert(got_band.end(), got.begin() + static_cast<size_t>(y) * 256 * 4, got.begin() + static_cast<size_t>(y + 1) * 256 * 4);
        want_band.insert(want_band.end(), want.begin() + static_cast<size_t>(y) * 256 * 4, want.begin() + static_cast<size_t>(y + 1) * 256 * 4);
    }
    const Diff d = diff_rgb(got_band, want_band);
    std::printf("    picture band (256x192, linear): max abs %d, mean abs %.4f\n", d.max_abs, d.mean_abs);
    CHECK(d.max_abs <= kLinearMaxAbs);
    CHECK(d.mean_abs <= kLinearMeanAbs);
    teardown(r);
}
