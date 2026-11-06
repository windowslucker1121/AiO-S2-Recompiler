// SPDX-License-Identifier: GPL-3.0-or-later
// Output pass (src/gfx/extras/output_pass.cpp) on the GPU, headless. Each case uploads a known source image, records OutputPass::record
// into a destination image, reads the destination back and checks it: against the CPU reference (hdr_math.h) for the HDR encodes, decoded
// with decode_hdr_capture; against the exact bytes for the SDR copy; against a CPU bilinear upscale for FSR 1. The group also requires zero
// validation errors and no leaked allocations.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "test_framework.h"

#include "extras/hdr_capture.h"
#include "extras/hdr_math.h"
#include "extras/output_pass.h"
#include "nxrt/gfx.h"
#include "validation_layer.h"

NXTEST_GROUP(extras_output)

namespace {

namespace hdr = nxrt::extras;
namespace gfx = nxrt::gfx;

// ---- half floats -----------------------------------------------------------------------------------------------------------------------

uint16_t half_from_float(float f) {
    uint32_t x;
    std::memcpy(&x, &f, sizeof x);
    const uint32_t sign = (x >> 16) & 0x8000u;
    const uint32_t e32 = (x >> 23) & 0xFFu;
    uint32_t man = x & 0x7FFFFFu;
    if (e32 == 0xFFu) return static_cast<uint16_t>(sign | 0x7C00u | (man ? 0x200u : 0u));
    const int exp = static_cast<int>(e32) - 127 + 15;
    if (exp >= 0x1F) return static_cast<uint16_t>(sign | 0x7C00u);
    if (exp <= 0) {
        if (exp < -10) return static_cast<uint16_t>(sign);
        man |= 0x800000u;
        const int shift = 14 - exp;
        uint32_t h = man >> shift;
        const uint32_t rem = man & ((1u << shift) - 1u), half = 1u << (shift - 1);
        if (rem > half || (rem == half && (h & 1u))) ++h;
        return static_cast<uint16_t>(sign | h);
    }
    uint32_t h = (static_cast<uint32_t>(exp) << 10) | (man >> 13);
    const uint32_t rem = man & 0x1FFFu;
    if (rem > 0x1000u || (rem == 0x1000u && (h & 1u))) ++h;   // a carry into the exponent is the right result
    return static_cast<uint16_t>(sign | h);
}

float float_from_half(uint16_t h) {
    const float sign = (h & 0x8000u) ? -1.0f : 1.0f;
    const int e = (h >> 10) & 0x1F;
    const uint32_t m = h & 0x3FFu;
    if (e == 0) return sign * std::ldexp(static_cast<float>(m), -24);
    return sign * std::ldexp(static_cast<float>(m | 0x400u), e - 25);   // the test values are finite and normal or zero
}

// The value the GPU actually sees for a float that is stored as a half.
float quant_half(float v) { return float_from_half(half_from_float(v)); }

// A w x h RGBA16F image: fill(x, y, rgba) sets the four values of a texel (alpha defaults to 1). Returns the bytes, 8 per texel.
std::vector<uint8_t> pack_half_rgba(uint32_t w, uint32_t h, const std::function<void(uint32_t, uint32_t, float*)>& fill) {
    std::vector<uint8_t> out(static_cast<size_t>(w) * h * 8);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            float v[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            fill(x, y, v);
            for (int c = 0; c < 4; ++c) {
                const uint16_t hb = half_from_float(v[c]);
                uint8_t* p = &out[(static_cast<size_t>(y) * w + x) * 8 + 2 * c];
                p[0] = static_cast<uint8_t>(hb & 0xFFu);
                p[1] = static_cast<uint8_t>(hb >> 8);
            }
        }
    return out;
}

// ---- the colour ramp of cases 1 and 2 -----------------------------------------------------------------------------------------------

// 8x8 source: column x holds v = kRamp[x] as r = g = v and b = 0.5 v (scene-linear BT.709, 1.0 = paper white). Rows are identical.
constexpr float kRamp[8] = {0.0f, 0.25f, 0.5f, 1.0f, 2.0f, 3.0f, 4.9f, 8.0f};
constexpr float kPaper = 203.0f, kPeak = 1000.0f;
constexpr float kPeakRatio = kPeak / kPaper;   // 4.926 paper-white units

std::vector<uint8_t> ramp_source() {
    return pack_half_rgba(8, 8, [](uint32_t x, uint32_t, float* v) {
        v[0] = kRamp[x];
        v[1] = kRamp[x];
        v[2] = 0.5f * kRamp[x];
    });
}

// The input of column x as the GPU sees it (after the half-float rounding of the upload).
void ramp_input(uint32_t x, float in[3]) {
    in[0] = quant_half(kRamp[x]);
    in[1] = in[0];
    in[2] = quant_half(0.5f * kRamp[x]);
}

// CPU reference of the HDR10 path: encode_hdr10, then what decode_hdr_capture does with the code words (PQ inverse, divide by paper white,
// BT.2020 -> BT.709). Ideal values: the 10-bit quantisation is not modelled.
void cpu_hdr10_roundtrip(const float in[3], float out[3]) {
    hdr::HdrOut o;
    o.paper_white_nits = kPaper;
    o.peak_nits = kPeak;
    float pq[3];
    hdr::encode_hdr10(in, o, pq);
    float bt2020[3];
    for (int c = 0; c < 3; ++c) bt2020[c] = hdr::pq_eotf(pq[c]) / kPaper;
    hdr::bt2020_to_bt709(bt2020, out);
}

// ---- device, images and the pass -----------------------------------------------------------------------------------------------------

struct Rig {
    std::unique_ptr<gfx::Device> dev;
    hdr::OutputPass pass;
    gfx::Image src, dst;
    gfx::Buffer rb;                  // readback of the destination
    VkFormat dst_fmt = VK_FORMAT_UNDEFINED;
    uint32_t w = 0, h = 0, bpp = 0;  // destination size and bytes per texel
};

// Frees what the rig made, then checks the validation counters and that the device holds no allocation.
void teardown(Rig& r) {
    if (!r.dev) return;
    r.dev->wait_idle();
    r.pass.shutdown();
    if (r.rb) r.dev->destroy_buffer(r.rb);
    if (r.dst) r.dev->destroy_image(r.dst);
    if (r.src) r.dev->destroy_image(r.src);
    const gfx::ValidationStats v = r.dev->validation_stats();
    std::printf("    validation: %llu errors, %llu warnings\n", static_cast<unsigned long long>(v.errors), static_cast<unsigned long long>(v.warnings));
    if (!v.first_error.empty()) std::printf("      first error: %s\n", v.first_error.c_str());
    CHECK_EQ(v.errors, 0ull);
    CHECK_EQ(r.dev->memory_stats().allocations, 0ull);
    r.dev.reset();
}

// Runs teardown when the case ends, however it ends (a REQUIRE return included).
struct Teardown {
    Rig& r;
    ~Teardown() { teardown(r); }
};

// Headless device with validation on, and the output pass initialised. False when there is no device (the case skips).
bool make_device(Rig& r) {
    gfx::DeviceDesc dd;
    dd.validation = 1;
    dd.headless = 1;
    std::string err;
    r.dev = gfx::Device::create(dd, &err);
    if (!r.dev) {
        std::printf("    device: %s\n", err.c_str());
        return false;
    }
    gfxtest::validation_layer_live(*r.dev);   // the zero counts below mean something only with the layer on
    CHECK(r.pass.init(*r.dev));
    return true;
}

// The source and destination formats must be usable as images here (sampled, colour attachment and transfer source). False: the case skips.
bool formats_supported(gfx::Device& dev, VkFormat src_fmt, VkFormat dst_fmt) {
    gfx::ImageDesc s;
    s.format = src_fmt;
    s.width = s.height = 8;
    s.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    gfx::ImageDesc d = s;
    d.format = dst_fmt;
    d.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    std::string why;
    if (!dev.image_supported(s, &why)) {
        std::printf("    source format %d: %s\n", static_cast<int>(src_fmt), why.c_str());
        return false;
    }
    if (!dev.image_supported(d, &why)) {
        std::printf("    destination format %d: %s\n", static_cast<int>(dst_fmt), why.c_str());
        return false;
    }
    return true;
}

// Sync2 layout transition of one colour image.
void barrier(VkCommandBuffer cmd, VkImage img, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 ss, VkAccessFlags2 sa, VkPipelineStageFlags2 ds,
             VkAccessFlags2 da) {
    VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = ss;
    b.srcAccessMask = sa;
    b.dstStageMask = ds;
    b.dstAccessMask = da;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &di);
}

bool make_image(gfx::Device& dev, const gfx::ImageDesc& d, gfx::Image* out) {
    std::string why;
    if (dev.create_image(d, out, &why) != VK_SUCCESS) {
        std::printf("    create_image %s: %s\n", d.name, why.c_str());
        CHECK_MSG(false, "create_image failed");
        return false;
    }
    return true;
}

// Uploads bytes (tightly packed, the size of fmt for w x h) into a sampled image, left in SHADER_READ_ONLY_OPTIMAL: the pass's source.
bool upload_source(Rig& r, VkFormat fmt, uint32_t w, uint32_t h, const std::vector<uint8_t>& bytes) {
    gfx::ImageDesc d;
    d.format = fmt;
    d.width = w;
    d.height = h;
    d.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    d.name = "output test source";
    if (!make_image(*r.dev, d, &r.src)) return false;
    gfx::Buffer stage;
    if (r.dev->create_buffer(bytes.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, gfx::MemoryUse::Upload, &stage, "output test staging") != VK_SUCCESS) {
        CHECK_MSG(false, "staging buffer failed");
        return false;
    }
    std::memcpy(stage.mapped, bytes.data(), bytes.size());
    r.dev->flush_buffer(stage);
    const bool ok = r.dev->submit_once([&](VkCommandBuffer cmd) {
        barrier(cmd, r.src.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkBufferImageCopy region = {};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {w, h, 1};
        vkCmdCopyBufferToImage(cmd, stage.buffer, r.src.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        barrier(cmd, r.src.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    });
    r.dev->destroy_buffer(stage);
    CHECK_MSG(ok, "upload submit failed");
    return ok;
}

// The destination: a colour attachment that can be copied out, and a readback buffer of the same size.
bool make_dst(Rig& r, VkFormat fmt, uint32_t w, uint32_t h, uint32_t bpp) {
    gfx::ImageDesc d;
    d.format = fmt;
    d.width = w;
    d.height = h;
    d.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    d.name = "output test destination";
    if (!make_image(*r.dev, d, &r.dst)) return false;
    r.dst_fmt = fmt;
    r.w = w;
    r.h = h;
    r.bpp = bpp;
    if (r.dev->create_buffer(static_cast<VkDeviceSize>(w) * h * bpp, VK_BUFFER_USAGE_TRANSFER_DST_BIT, gfx::MemoryUse::Readback, &r.rb,
                             "output test readback") != VK_SUCCESS) {
        CHECK_MSG(false, "readback buffer failed");
        return false;
    }
    return true;
}

// Parameters with the source already set; the case adjusts the rest.
hdr::OutputParams base_params(const Rig& r, uint32_t sw, uint32_t sh) {
    hdr::OutputParams p;
    p.src = r.src.view;
    p.src_extent = {sw, sh};
    p.keep_aspect = false;
    p.paper_white_nits = kPaper;
    p.peak_nits = kPeak;
    return p;
}

// Records OutputPass::record into the destination, copies it out and reads it back. Returns the encode mode the pass reported, or -2 when
// the submit failed. The destination starts undefined each time (the pass draws over all of it when keep_aspect is off, and the letterbox
// clears otherwise).
int run_pass(Rig& r, const hdr::OutputParams& p, std::vector<uint8_t>* out) {
    int mode = -2;
    const bool ok = r.dev->submit_once([&](VkCommandBuffer cmd) {
        barrier(cmd, r.dst.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        mode = r.pass.record(cmd, r.dst.view, r.dst_fmt, VkExtent2D{r.w, r.h}, p);
        barrier(cmd, r.dst.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT);
        VkBufferImageCopy region = {};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {r.w, r.h, 1};
        vkCmdCopyImageToBuffer(cmd, r.dst.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, r.rb.buffer, 1, &region);
    });
    if (!ok) return -2;
    r.dev->invalidate_buffer(r.rb);
    const uint8_t* m = static_cast<const uint8_t*>(r.rb.mapped);
    out->assign(m, m + static_cast<size_t>(r.w) * r.h * r.bpp);
    return mode;
}

// Tolerance of the HDR checks: 2 percent relative or 0.01 absolute. The 10-bit PQ code steps are about 0.7 percent of the value at 200 nits
// and about 1 percent at 1000 nits, so 2 percent leaves margin.
bool close_to(double got, double want, double rel, double abs_tol) { return std::fabs(got - want) <= std::max(rel * std::fabs(want), abs_tol); }

}  // namespace

// ---- 1: HDR10 encode against the CPU reference --------------------------------------------------------------------------------------

NXTEST(hdr10_encode_matches_cpu) {
    Rig r;
    Teardown td{r};
    if (!make_device(r)) {
        nxtest::skip("no headless Vulkan device");
        return;
    }
    if (!formats_supported(*r.dev, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_A2B10G10R10_UNORM_PACK32)) {
        nxtest::skip("needed format not supported");
        return;
    }
    REQUIRE(upload_source(r, VK_FORMAT_R16G16B16A16_SFLOAT, 8, 8, ramp_source()));
    REQUIRE(make_dst(r, VK_FORMAT_A2B10G10R10_UNORM_PACK32, 8, 8, 4));
    hdr::OutputParams p = base_params(r, 8, 8);
    p.out = hdr::OutputKind::Hdr10;
    p.src_linear = true;
    std::vector<uint8_t> got;
    const int mode = run_pass(r, p, &got);
    CHECK_EQ(mode, 2);
    std::vector<float> lin;
    REQUIRE(hdr::decode_hdr_capture(hdr::CaptureFormat::A2B10G10R10, got.data(), 8, 8, kPaper, &lin, nullptr, nullptr, nullptr));

    // Against the CPU reference chain (encode_hdr10, then the decoder's inverse).
    double max_abs = 0.0;
    size_t bad = 0;
    // Against the plan's simple expectation min(input, 1000/203) per channel: differs where the BT.2020 clamp bites (column 7).
    double max_naive = 0.0;
    int naive_col = -1;
    for (uint32_t y = 0; y < 8; ++y)
        for (uint32_t x = 0; x < 8; ++x) {
            float in[3], ref[3];
            ramp_input(x, in);
            cpu_hdr10_roundtrip(in, ref);
            for (int c = 0; c < 3; ++c) {
                const double g = lin[(static_cast<size_t>(y) * 8 + x) * 3 + c];
                if (!close_to(g, ref[c], 0.02, 0.01)) ++bad;
                max_abs = std::max(max_abs, std::fabs(g - ref[c]));
                const double naive = std::min(static_cast<double>(in[c]), static_cast<double>(kPeakRatio));
                if (std::fabs(g - naive) > max_naive) {
                    max_naive = std::fabs(g - naive);
                    naive_col = static_cast<int>(x);
                }
            }
        }
    std::printf("    hdr10: max abs error vs CPU reference %.5f, channels outside tolerance %zu of 192\n", max_abs, bad);
    std::printf("    hdr10: max deviation from min(input, 1000/203) %.4f (column %d; BT.2020 clamp, see test comment)\n", max_naive, naive_col);
    CHECK(bad == 0);
}

// ---- 2: scRGB encode ------------------------------------------------------------------------------------------------------------------

NXTEST(scrgb_encode_matches_cpu) {
    Rig r;
    Teardown td{r};
    if (!make_device(r)) {
        nxtest::skip("no headless Vulkan device");
        return;
    }
    if (!formats_supported(*r.dev, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT)) {
        nxtest::skip("needed format not supported");
        return;
    }
    REQUIRE(upload_source(r, VK_FORMAT_R16G16B16A16_SFLOAT, 8, 8, ramp_source()));
    REQUIRE(make_dst(r, VK_FORMAT_R16G16B16A16_SFLOAT, 8, 8, 8));
    hdr::OutputParams p = base_params(r, 8, 8);
    p.out = hdr::OutputKind::ScRgb;
    p.src_linear = true;
    std::vector<uint8_t> got;
    const int mode = run_pass(r, p, &got);
    CHECK_EQ(mode, 3);
    std::vector<float> lin;
    REQUIRE(hdr::decode_hdr_capture(hdr::CaptureFormat::Rgba16F, got.data(), 8, 8, kPaper, &lin, nullptr, nullptr, nullptr));

    // Expected after the decode: min(input, 1000/203) per channel (scRGB has no BT.2020 step, so only the peak limits it).
    double max_err = 0.0;
    size_t bad = 0;
    double worst_col_err[8] = {};
    for (uint32_t y = 0; y < 8; ++y)
        for (uint32_t x = 0; x < 8; ++x) {
            float in[3];
            ramp_input(x, in);
            for (int c = 0; c < 3; ++c) {
                const double e = std::min(static_cast<double>(in[c]), static_cast<double>(kPeakRatio));
                const double g = lin[(static_cast<size_t>(y) * 8 + x) * 3 + c];
                const double err = std::fabs(g - e);
                if (err > 0.01 * std::fabs(e) + 1e-4) ++bad;
                max_err = std::max(max_err, err);
                worst_col_err[x] = std::max(worst_col_err[x], err);
            }
        }
    std::printf("    scrgb: max abs error vs min(input, 1000/203) %.5f, channels outside 1 percent %zu of 192\n", max_err, bad);
    std::printf("    scrgb: column 7 (input 8.0) decoded to %.4f, expected %.4f\n", static_cast<double>(lin[(7 * 3)]), static_cast<double>(kPeakRatio));
    CHECK(bad == 0);
}

// ---- 3: SDR encode from scene-linear ------------------------------------------------------------------------------------------------

NXTEST(sdr_encode) {
    Rig r;
    Teardown td{r};
    if (!make_device(r)) {
        nxtest::skip("no headless Vulkan device");
        return;
    }
    if (!formats_supported(*r.dev, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R8G8B8A8_UNORM)) {
        nxtest::skip("needed format not supported");
        return;
    }
    const float vals[4] = {0.0f, 0.1f, 0.5f, 1.0f};
    const std::vector<uint8_t> src = pack_half_rgba(8, 8, [&](uint32_t x, uint32_t, float* v) {
        v[0] = v[1] = v[2] = vals[x % 4];
    });
    REQUIRE(upload_source(r, VK_FORMAT_R16G16B16A16_SFLOAT, 8, 8, src));
    REQUIRE(make_dst(r, VK_FORMAT_R8G8B8A8_UNORM, 8, 8, 4));
    hdr::OutputParams p = base_params(r, 8, 8);
    p.out = hdr::OutputKind::Sdr8;
    p.src_linear = true;
    std::vector<uint8_t> got;
    const int mode = run_pass(r, p, &got);
    CHECK_EQ(mode, 1);
    int max_diff = 0;
    int alpha_bad = 0;
    for (uint32_t y = 0; y < 8; ++y)
        for (uint32_t x = 0; x < 8; ++x) {
            const float v = std::clamp(quant_half(vals[x % 4]), 0.0f, 1.0f);
            const int want = static_cast<int>(std::lround(255.0f * hdr::srgb_oetf(v)));
            const uint8_t* px = &got[(static_cast<size_t>(y) * 8 + x) * 4];
            for (int c = 0; c < 3; ++c) max_diff = std::max(max_diff, std::abs(static_cast<int>(px[c]) - want));
            if (px[3] != 255) ++alpha_bad;
        }
    std::printf("    sdr: max byte difference vs round(255 * srgb_oetf(clamp(v))) %d, alpha not 255: %d\n", max_diff, alpha_bad);
    std::printf("    sdr: value 0.5 -> byte %d (expected %ld)\n", got[(0 * 8 + 2) * 4], std::lround(255.0f * hdr::srgb_oetf(quant_half(0.5f))));
    CHECK(max_diff <= 1);
    CHECK_EQ(alpha_bad, 0);
}

// ---- 4: SDR copy is exact -----------------------------------------------------------------------------------------------------------

NXTEST(copy_mode_is_exact) {
    Rig r;
    Teardown td{r};
    if (!make_device(r)) {
        nxtest::skip("no headless Vulkan device");
        return;
    }
    if (!formats_supported(*r.dev, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM)) {
        nxtest::skip("needed format not supported");
        return;
    }
    // Fixed-seed LCG bytes for RGB; alpha 255 so the expected output is the input bytes exactly.
    std::vector<uint8_t> src(8 * 8 * 4);
    uint32_t s = 12345u;
    for (size_t i = 0; i < src.size(); i += 4) {
        for (int c = 0; c < 3; ++c) {
            s = s * 1664525u + 1013904223u;
            src[i + c] = static_cast<uint8_t>(s >> 24);
        }
        src[i + 3] = 255;
    }
    REQUIRE(upload_source(r, VK_FORMAT_R8G8B8A8_UNORM, 8, 8, src));
    REQUIRE(make_dst(r, VK_FORMAT_R8G8B8A8_UNORM, 8, 8, 4));
    hdr::OutputParams p = base_params(r, 8, 8);
    p.out = hdr::OutputKind::Sdr8;
    p.src_linear = false;
    std::vector<uint8_t> got;
    const int mode = run_pass(r, p, &got);
    CHECK_EQ(mode, 0);
    size_t mismatch = 0;
    for (size_t i = 0; i < src.size(); ++i)
        if (got[i] != src[i]) ++mismatch;
    std::printf("    copy: %zu of %zu bytes differ from the input\n", mismatch, src.size());
    CHECK_EQ(mismatch, 0ull);
}

// ---- 5: FSR 1 upscales a gradient ---------------------------------------------------------------------------------------------------

NXTEST(fsr1_upscales_a_gradient) {
    Rig r;
    Teardown td{r};
    if (!make_device(r)) {
        nxtest::skip("no headless Vulkan device");
        return;
    }
    if (!formats_supported(*r.dev, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM)) {
        nxtest::skip("needed format not supported");
        return;
    }
    constexpr uint32_t S = 32, D = 64;
    std::vector<uint8_t> src(S * S * 4);
    for (uint32_t y = 0; y < S; ++y)
        for (uint32_t x = 0; x < S; ++x) {
            uint8_t* px = &src[(static_cast<size_t>(y) * S + x) * 4];
            px[0] = px[1] = px[2] = static_cast<uint8_t>(x * 8);
            px[3] = 255;
        }
    REQUIRE(upload_source(r, VK_FORMAT_R8G8B8A8_UNORM, S, S, src));
    REQUIRE(make_dst(r, VK_FORMAT_R8G8B8A8_UNORM, D, D, 4));
    hdr::OutputParams p = base_params(r, S, S);
    p.keep_aspect = true;
    p.out = hdr::OutputKind::Sdr8;
    p.src_linear = false;
    p.fsr1 = true;
    p.sharpness = 0.8f;
    CHECK_EQ(r.pass.fsr_frames(), 0u);
    std::vector<uint8_t> got;
    const int mode = run_pass(r, p, &got);
    CHECK_EQ(mode, 0);
    CHECK_EQ(r.pass.fsr_frames(), 1u);
    REQUIRE(got.size() == static_cast<size_t>(D) * D * 4);

    // Bilinear reference: pixel centres mapped to source texel space, clamped to the edge texels. Source value at texel x is 8 x.
    double bil[D];
    for (uint32_t x = 0; x < D; ++x) {
        double sx = (x + 0.5) * S / D - 0.5;
        sx = std::clamp(sx, 0.0, S - 1.0);
        const int x0 = static_cast<int>(std::floor(sx));
        const int x1 = std::min(x0 + 1, static_cast<int>(S) - 1);
        const double f = sx - x0;
        bil[x] = (1.0 - f) * (8.0 * x0) + f * (8.0 * x1);
    }
    int worst_drop = 0;
    size_t drop_bad = 0;
    int first_max = 0, last_min = 255;
    double sum_abs = 0.0;
    int max_abs = 0;
    for (uint32_t y = 0; y < D; ++y) {
        const uint8_t* row = &got[static_cast<size_t>(y) * D * 4];
        first_max = std::max(first_max, static_cast<int>(row[0]));
        last_min = std::min(last_min, static_cast<int>(row[(D - 1) * 4]));
        for (uint32_t x = 1; x < D; ++x) {
            const int drop = static_cast<int>(row[(x - 1) * 4]) - static_cast<int>(row[x * 4]);
            if (drop > 0) {
                worst_drop = std::max(worst_drop, drop);
                if (drop > 3) ++drop_bad;
            }
        }
        for (uint32_t x = 0; x < D; ++x) {
            const int d = std::abs(static_cast<int>(row[x * 4 + 0]) - static_cast<int>(std::lround(bil[x])));
            sum_abs += d;
            max_abs = std::max(max_abs, d);
        }
    }
    const double mean_abs = sum_abs / (static_cast<double>(D) * D);
    std::printf("    fsr1 gradient: largest decrease along a row %d levels (%zu steps of more than 3), first column max %d, last column min %d\n",
                worst_drop, drop_bad, first_max, last_min);
    std::printf("    fsr1 gradient: vs CPU bilinear, mean abs difference %.3f levels, max %d\n", mean_abs, max_abs);
    CHECK(drop_bad == 0);
    CHECK(first_max <= 12);
    CHECK(last_min >= 238);
    CHECK(mean_abs < 6.0);
}

// ---- 6: FSR 1 in HDR keeps highlights -----------------------------------------------------------------------------------------------

NXTEST(fsr1_hdr_keeps_highlights) {
    Rig r;
    Teardown td{r};
    if (!make_device(r)) {
        nxtest::skip("no headless Vulkan device");
        return;
    }
    if (!formats_supported(*r.dev, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_A2B10G10R10_UNORM_PACK32)) {
        nxtest::skip("needed format not supported");
        return;
    }
    constexpr uint32_t S = 32, D = 64;
    const std::vector<uint8_t> src = pack_half_rgba(S, S, [](uint32_t x, uint32_t, float* v) {
        v[0] = v[1] = v[2] = (x < S / 2) ? 0.2f : 3.0f;
    });
    REQUIRE(upload_source(r, VK_FORMAT_R16G16B16A16_SFLOAT, S, S, src));
    REQUIRE(make_dst(r, VK_FORMAT_A2B10G10R10_UNORM_PACK32, D, D, 4));
    hdr::OutputParams p = base_params(r, S, S);
    p.keep_aspect = true;
    p.out = hdr::OutputKind::Hdr10;
    p.src_linear = true;
    p.fsr1 = true;
    CHECK_EQ(r.pass.fsr_frames(), 0u);
    std::vector<uint8_t> got;
    const int mode = run_pass(r, p, &got);
    CHECK_EQ(mode, 2);
    CHECK_EQ(r.pass.fsr_frames(), 1u);
    std::vector<float> lin;
    REQUIRE(hdr::decode_hdr_capture(hdr::CaptureFormat::A2B10G10R10, got.data(), D, D, kPaper, &lin, nullptr, nullptr, nullptr));

    double hi_min = 1e9, hi_max = -1e9, lo_min = 1e9, lo_max = -1e9;
    size_t hi_bad = 0, lo_bad = 0;
    for (uint32_t y = 0; y < D; ++y)
        for (uint32_t x = 0; x < D; ++x) {
            const double g = lin[(static_cast<size_t>(y) * D + x) * 3];   // R channel (the three are equal here)
            if (x >= 40 && x <= 58) {
                hi_min = std::min(hi_min, g);
                hi_max = std::max(hi_max, g);
                if (std::fabs(g - 3.0) > 0.05 * 3.0) ++hi_bad;
            }
            if (x >= 5 && x <= 24) {
                lo_min = std::min(lo_min, g);
                lo_max = std::max(lo_max, g);
                if (std::fabs(g - 0.2) > 0.05 * 0.2) ++lo_bad;
            }
        }
    std::printf("    fsr1 hdr: columns 40..58 decoded %.4f..%.4f (want 3.0 +-5%%), %zu pixels outside\n", hi_min, hi_max, hi_bad);
    std::printf("    fsr1 hdr: columns 5..24 decoded %.4f..%.4f (want 0.2 +-5%%), %zu pixels outside\n", lo_min, lo_max, lo_bad);
    CHECK(hi_bad == 0);
    CHECK(lo_bad == 0);
}
