// SPDX-License-Identifier: GPL-3.0-or-later
// Masked colour clear (src/gfx/nvn/nvn_mask_clear.cpp), headless. For four colour formats (RGBA8_UNORM, RGBA16F, R32_UINT, R32_SINT),
// six channel masks, a sub-rectangle and the full image, on attachment 0 of one and attachment 1 of two, the targets are cleared to a
// known colour (loadOp CLEAR inside the dynamic-rendering pass), the pass records the masked clear, and every channel is read back:
// the masked channels inside the rectangle hold the value, all other channels and pixels keep the clear colour. Validation must
// report nothing.
//
// Tolerance: RGBA8 is compared with one 8-bit step (a float converted to UNORM8 may round either way); RGBA16F with 0.002 (0.2 is
// 0.19995 in half precision); the 32-bit formats exactly. R32 formats have one channel: only mask bit 0 changes them.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "nvn_mask_clear.h"
#include "nxrt/gfx.h"
#include "test_framework.h"
#include "validation_layer.h"
NXTEST_GROUP(gfx_nvn_mask_clear)

using namespace nxrt;
using namespace nxrt::gfx;
using nxrt::nvn::be::MaskClear;

namespace {

constexpr uint32_t kW = 32, kH = 24;
constexpr VkRect2D kFull = {{0, 0}, {kW, kH}};
constexpr VkRect2D kSub = {{5, 3}, {17, 11}};   // x 5..21, y 3..13

enum FmtIdx { kRGBA8 = 0, kRGBA16F = 1, kR32UINT = 2, kR32SINT = 3, kFmtCount = 4 };

struct FmtInfo {
    const char* name;
    VkFormat format;
    uint32_t bpp, channels, kind;   // kind: 0 float, 1 int32, 2 uint32 (MaskClear::record)
};
constexpr FmtInfo kFormats[kFmtCount] = {
    {"RGBA8_UNORM", VK_FORMAT_R8G8B8A8_UNORM, 4, 4, 0},
    {"RGBA16F", VK_FORMAT_R16G16B16A16_SFLOAT, 8, 4, 0},
    {"R32_UINT", VK_FORMAT_R32_UINT, 4, 1, 2},
    {"R32_SINT", VK_FORMAT_R32_SINT, 4, 1, 1},
};

// The colour the targets start with (prev) and the colour the pass writes (value), as 32-bit words for each format's kind.
void values_for(int f, uint32_t prev[4], uint32_t value[4]) {
    if (f == kRGBA8 || f == kRGBA16F) {
        const float p[4] = {0.0f, 1.0f, 0.6f, 0.4f}, v[4] = {1.0f, 0.0f, 0.2f, 0.75f};
        std::memcpy(prev, p, sizeof p);
        std::memcpy(value, v, sizeof v);
    } else if (f == kR32UINT) {
        const uint32_t p[4] = {0x12345678u, 0, 0, 0}, v[4] = {0xDEADBEEFu, 0, 0, 0};
        std::memcpy(prev, p, sizeof p);
        std::memcpy(value, v, sizeof v);
    } else {
        const int32_t p[4] = {1000, 0, 0, 0}, v[4] = {-7, 0, 0, 0};
        std::memcpy(prev, p, sizeof p);
        std::memcpy(value, v, sizeof v);
    }
}

// IEEE half precision to double (finite values and infinities; the test values are finite).
double half_to_double(uint16_t h) {
    const int s = h >> 15, e = (h >> 10) & 31, m = h & 1023;
    double v;
    if (e == 0)
        v = std::ldexp(static_cast<double>(m), -24);
    else if (e == 31)
        v = m ? NAN : INFINITY;
    else
        v = std::ldexp(static_cast<double>(1024 + m), e - 25);
    return s ? -v : v;
}

// One channel of a stored pixel, in the units the expectation uses (RGBA8: 0..1; RGBA16F: float; 32-bit: the integer).
double stored(int f, const uint8_t* px, uint32_t c) {
    switch (f) {
        case kRGBA8: return px[c] / 255.0;
        case kRGBA16F: {
            uint16_t h;
            std::memcpy(&h, px + 2 * c, 2);
            return half_to_double(h);
        }
        case kR32UINT: {
            uint32_t u;
            std::memcpy(&u, px, 4);
            return static_cast<double>(u);
        }
        default: {
            int32_t s;
            std::memcpy(&s, px, 4);
            return static_cast<double>(s);
        }
    }
}

// The expected value of channel c for a clear/write word, in the same units as stored().
double expected(int f, const uint32_t words[4], uint32_t c) {
    if (f == kRGBA8 || f == kRGBA16F) {
        float x;
        std::memcpy(&x, &words[c], 4);
        return x;
    }
    if (f == kR32UINT) return static_cast<double>(words[c]);
    int32_t s;
    std::memcpy(&s, &words[c], 4);
    return static_cast<double>(s);
}

double tolerance(int f) {
    if (f == kRGBA8) return 1.01 / 255.0;
    if (f == kRGBA16F) return 0.002;
    return 0.0;
}

struct Rig {
    std::unique_ptr<Device> dev;   // declared first: destroyed after mc
    MaskClear mc;
};

// Headless device with validation on, and the masked clear initialised on it.
bool make_rig(Rig& r) {
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
    return r.mc.init(*r.dev);
}

// Shuts the pass down, then checks the validation counters and that the device holds no allocation.
void teardown(Rig& r) {
    if (!r.dev) return;
    r.dev->wait_idle();
    r.mc.shutdown();
    const ValidationStats v = r.dev->validation_stats();
    std::printf("    validation: %llu errors, %llu warnings\n", static_cast<unsigned long long>(v.errors), static_cast<unsigned long long>(v.warnings));
    if (!v.first_error.empty()) std::printf("      first error: %s\n", v.first_error.c_str());
    CHECK_EQ(v.errors, 0ull);
    CHECK_EQ(v.warnings, 0ull);
    CHECK_EQ(r.dev->memory_stats().allocations, 0ull);
    r.dev.reset();
}

struct CaseResult {
    uint64_t channels = 0, wrong = 0;
    double worst = 0;
    std::string first;   // the first wrong channel, for the report
};

void barriers(VkCommandBuffer cb, const std::vector<VkImageMemoryBarrier2>& bars) {
    if (bars.empty()) return;
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = static_cast<uint32_t>(bars.size());
    di.pImageMemoryBarriers = bars.data();
    vkCmdPipelineBarrier2(cb, &di);
}

// Clears `total` targets to prev, records the masked clear on `attachment` inside rect, reads every target back and compares each
// channel with the expectation. Returns false on a Vulkan or recording error (reported).
bool run_case(Device& dev, MaskClear& mc, int f, uint32_t total, uint32_t attachment, uint32_t mask, VkRect2D rect, CaseResult* res) {
    const FmtInfo& fi = kFormats[f];
    uint32_t prev[4] = {}, value[4] = {};
    values_for(f, prev, value);
    const size_t one = static_cast<size_t>(kW) * kH * fi.bpp;

    std::vector<Image> imgs(total);
    for (uint32_t i = 0; i < total; ++i) {
        ImageDesc id;
        id.format = fi.format;
        id.width = kW;
        id.height = kH;
        id.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        id.name = "mask clear target";
        std::string why;
        if (dev.create_image(id, &imgs[i], &why) != VK_SUCCESS) {
            std::printf("    target image (%s): %s\n", fi.name, why.c_str());
            for (auto& im : imgs)
                if (im) dev.destroy_image(im);
            return false;
        }
    }
    Buffer rb;
    if (dev.create_buffer(one * total, VK_BUFFER_USAGE_TRANSFER_DST_BIT, MemoryUse::Readback, &rb, "mask clear readback") != VK_SUCCESS) {
        for (auto& im : imgs) dev.destroy_image(im);
        return false;
    }

    auto barrier = [&](uint32_t i, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 src_st, VkAccessFlags2 src_ac,
                       VkPipelineStageFlags2 dst_st, VkAccessFlags2 dst_ac) {
        VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = src_st;
        b.srcAccessMask = src_ac;
        b.dstStageMask = dst_st;
        b.dstAccessMask = dst_ac;
        b.oldLayout = from;
        b.newLayout = to;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = imgs[i].image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        return b;
    };

    bool recorded = false;
    const bool ok = dev.submit_once([&](VkCommandBuffer cb) {
        std::vector<VkImageMemoryBarrier2> bars;
        for (uint32_t i = 0; i < total; ++i)
            bars.push_back(barrier(i, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0,
                                   VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT));
        barriers(cb, bars);

        // Every target is cleared to prev by loadOp CLEAR; the masked clear is recorded inside the same pass.
        VkRenderingAttachmentInfo atts[MaskClear::kMaxAttachments] = {};
        for (uint32_t i = 0; i < total; ++i) {
            atts[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            atts[i].imageView = imgs[i].view;
            atts[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            atts[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            atts[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            std::memcpy(atts[i].clearValue.color.uint32, prev, 16);
        }
        VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, {kW, kH}};
        ri.layerCount = 1;
        ri.colorAttachmentCount = total;
        ri.pColorAttachments = atts;
        vkCmdBeginRendering(cb, &ri);
        recorded = mc.record(cb, fi.format, total, attachment, mask, fi.kind, value, rect, 1);
        vkCmdEndRendering(cb);

        bars.clear();
        for (uint32_t i = 0; i < total; ++i)
            bars.push_back(barrier(i, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                                   VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT));
        barriers(cb, bars);
        for (uint32_t i = 0; i < total; ++i) {
            VkBufferImageCopy region = {};
            region.bufferOffset = i * one;   // rows tightly packed (row length 0)
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {kW, kH, 1};
            vkCmdCopyImageToBuffer(cb, imgs[i].image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.buffer, 1, &region);
        }
    });

    bool good = ok && recorded;
    if (good) {
        dev.invalidate_buffer(rb);
        const uint8_t* base = static_cast<const uint8_t*>(rb.mapped);
        const int32_t rx0 = rect.offset.x, ry0 = rect.offset.y;
        const int32_t rx1 = rx0 + static_cast<int32_t>(rect.extent.width), ry1 = ry0 + static_cast<int32_t>(rect.extent.height);
        for (uint32_t i = 0; i < total; ++i)
            for (uint32_t y = 0; y < kH; ++y)
                for (uint32_t x = 0; x < kW; ++x) {
                    const uint8_t* px = base + i * one + (static_cast<size_t>(y) * kW + x) * fi.bpp;
                    const bool inside = static_cast<int32_t>(x) >= rx0 && static_cast<int32_t>(x) < rx1 && static_cast<int32_t>(y) >= ry0 &&
                                        static_cast<int32_t>(y) < ry1;
                    for (uint32_t c = 0; c < fi.channels; ++c) {
                        const bool written = i == attachment && inside && ((mask >> c) & 1u);
                        const double want = expected(f, written ? value : prev, c);
                        const double got = stored(f, px, c);
                        const double d = std::fabs(got - want);
                        res->channels++;
                        if (d > res->worst) res->worst = d;
                        if (d > tolerance(f)) {
                            if (res->wrong == 0) {
                                char buf[256];
                                std::snprintf(buf, sizeof buf, "target %u pixel (%u,%u) channel %u: got %.6g, want %.6g", i, x, y, c, got, want);
                                res->first = buf;
                            }
                            res->wrong++;
                        }
                    }
                }
    }
    dev.destroy_buffer(rb);
    for (auto& im : imgs) dev.destroy_image(im);
    return good;
}

}  // namespace

NXTEST(init_and_shutdown_release_everything) {
    Rig r;
    REQUIRE(make_rig(r));
    CHECK(r.mc.initialised());
    CHECK(r.mc.init(*r.dev));   // a second init is a no-op
    r.mc.shutdown();
    CHECK(!r.mc.initialised());
    r.mc.shutdown();            // and so is a second shutdown
    teardown(r);
}

NXTEST(masked_clear_changes_only_masked_channels_inside_rect_on_one_attachment) {
    Rig r;
    REQUIRE(make_rig(r));
    const uint32_t masks[6] = {0x1, 0x2, 0x4, 0x8, 0x5, 0xA};
    const struct {
        uint32_t total, attachment;
    } layouts[2] = {{1, 0}, {2, 1}};
    const VkRect2D rects[2] = {kFull, kSub};
    for (int f = 0; f < kFmtCount; ++f) {
        CaseResult sum;
        int cases = 0;
        for (const auto& L : layouts)
            for (uint32_t m : masks)
                for (const VkRect2D& rect : rects) {
                    CaseResult res;
                    REQUIRE(run_case(*r.dev, r.mc, f, L.total, L.attachment, m, rect, &res));
                    ++cases;
                    sum.channels += res.channels;
                    sum.wrong += res.wrong;
                    if (res.worst > sum.worst) sum.worst = res.worst;
                    if (res.wrong) {
                        std::printf("    %s total %u attachment %u mask 0x%X rect %dx%d@%d,%d: %llu wrong, first: %s\n", kFormats[f].name, L.total,
                                    L.attachment, m, static_cast<int>(rect.extent.width), static_cast<int>(rect.extent.height), rect.offset.x,
                                    rect.offset.y, static_cast<unsigned long long>(res.wrong), res.first.c_str());
                    }
                }
        std::printf("    %s: %d cases, %llu channels compared, %llu wrong, worst error %.6g\n", kFormats[f].name, cases,
                    static_cast<unsigned long long>(sum.channels), static_cast<unsigned long long>(sum.wrong), sum.worst);
        CHECK_EQ(sum.wrong, 0ull);
    }
    teardown(r);
}

NXTEST(rejects_bad_arguments_and_draws_nothing_for_empty_masks_or_rects) {
    // No command buffer is needed: these calls return before anything is recorded.
    Rig r;
    REQUIRE(make_rig(r));
    const uint32_t v[4] = {1, 2, 3, 4};
    const VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;
    CHECK(!r.mc.record(VK_NULL_HANDLE, fmt, 0, 0, 0xF, 0, v, kFull, 1));          // no attachments
    CHECK(!r.mc.record(VK_NULL_HANDLE, fmt, 9, 0, 0xF, 0, v, kFull, 1));          // more than eight
    CHECK(!r.mc.record(VK_NULL_HANDLE, fmt, 1, 1, 0xF, 0, v, kFull, 1));          // attachment out of range
    CHECK(!r.mc.record(VK_NULL_HANDLE, fmt, 1, 0, 0xF, 3, v, kFull, 1));          // unknown kind
    CHECK(!r.mc.record(VK_NULL_HANDLE, fmt, 1, 0, 0xF, 0, v, kFull, 3));          // sample count not a power of two
    CHECK(r.mc.record(VK_NULL_HANDLE, fmt, 1, 0, 0, 0, v, kFull, 1));             // mask 0: nothing to draw
    CHECK(r.mc.record(VK_NULL_HANDLE, fmt, 1, 0, 0xF, 0, v, VkRect2D{{0, 0}, {0, 5}}, 1));   // empty rectangle
    teardown(r);
}
