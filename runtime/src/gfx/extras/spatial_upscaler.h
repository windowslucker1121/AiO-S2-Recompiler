// SPDX-License-Identifier: GPL-3.0-or-later
// The common face of the spatial upscalers (AMD FSR 1 in fsr1_spatial.h, NVIDIA Image Scaling in nis_spatial.h), so the output pass and the
// scene upscaler can run either one. Temporal upscalers (FSR 3, DLSS, XeSS) need per-pixel motion vectors and a jittered projection, which
// title A does not render (no velocity target, no TAA pass in the shaders it uses), so they are not offered through this interface.
#pragma once
#include <volk.h>

#include <cstdint>

namespace nxrt::extras {

class SpatialUpscaler {
public:
    virtual ~SpatialUpscaler() = default;
    // Records into cb (outside any render pass). src: a view of an image in SHADER_READ_ONLY_OPTIMAL; in_view: the part of it to upscale (from texel 0, 0);
    // in_tex: the whole image; out: the size of the result. linear_hdr: scene-linear float input, compressed with pre-scale k around the filter.
    // sharpness 0..1. Returns a view of the result (GENERAL layout, readable by compute and fragment shaders after the recorded barrier), or null.
    virtual VkImageView upscale(VkCommandBuffer cb, VkImageView src, VkExtent2D in_view, VkExtent2D in_tex, VkExtent2D out, bool linear_hdr, float prescale,
                                float sharpness) = 0;
    // True when this upscaler can scale in_view to out (NIS: 1x to 2x per side).
    virtual bool supports(VkExtent2D in_view, VkExtent2D out) const { (void)in_view; (void)out; return true; }
};

}  // namespace nxrt::extras
