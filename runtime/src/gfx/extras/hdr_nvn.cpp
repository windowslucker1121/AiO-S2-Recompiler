// SPDX-License-Identifier: GPL-3.0-or-later
#include "extras/hdr_nvn.h"

#include <mutex>
#include <map>
#include <set>

#include "nxrt/extras.h"
#include "nxrt/log.h"
#include "extras/fsr1_spatial.h"
#include "extras/nis_spatial.h"
#include "extras/hdr_tonemap_patch.h"

namespace nxrt::extras {
namespace {
constexpr int kFlagDisplay = 0x1, kFlagCompressible = 0x8;   // NVN_TEXTURE_FLAGS_DISPLAY, NVN_TEXTURE_FLAGS_COMPRESSIBLE
}

VkFormat hdr_promote_target(VkFormat chosen, int flags, bool is_2d, uint32_t levels, uint32_t layers, uint32_t samples, bool nvn_srgb) {
    if (!nvn_srgb || chosen != VK_FORMAT_R8G8B8A8_SRGB || !is_2d || levels != 1 || layers != 1 || samples > 1) return VK_FORMAT_UNDEFINED;
    if (!(flags & (kFlagDisplay | kFlagCompressible))) return VK_FORMAT_UNDEFINED;
    if (!hdr_scene_active()) return VK_FORMAT_UNDEFINED;
    return VK_FORMAT_R16G16B16A16_SFLOAT;
}

bool hdr_tonemap_variant(const std::vector<uint32_t>& words, const std::string& hash, std::vector<uint32_t>& out) {
    if (!hdr_scene_active()) return false;
    static std::mutex mu;
    static std::set<std::string> tried;
    // the curve tone map (small shaders), else the LUT tone map (Pokopia's composite pass)
    bool lut = false;
    bool ok = words.size() <= 16384 && spirv_hdr_tonemap(words, out);
    if (!ok) ok = lut = spirv_hdr_tonemap_lut(words, hdr_state().peak_ratio, out);
    std::lock_guard<std::mutex> lk(mu);
    if (tried.insert(hash).second && ok)
        NXLOG_I("hdr", "tone-map shader %s: HDR variant made (%s, peak ratio %.2f)", hash.c_str(), lut ? "LUT highlights extended" : "curve",
                static_cast<double>(hdr_state().peak_ratio));
    return ok;
}

bool hdr_is_tonemap(const std::vector<uint32_t>& words, const std::string& hash) {
    static std::mutex mu;
    static std::map<std::string, bool> cache;
    {
        std::lock_guard<std::mutex> lk(mu);
        auto it = cache.find(hash);
        if (it != cache.end()) return it->second;
    }
    std::vector<uint32_t> scratch;
    // the curve tone map or the LUT composite (Pokopia); the patch result is only used as a match test here
    bool ok = words.size() <= 16384 && spirv_hdr_tonemap(words, scratch);
    if (!ok) {   // no size limit: Pokopia's composite shader is large (hdr_tonemap_variant matches it the same way)
        scratch.clear();
        ok = spirv_hdr_tonemap_lut(words, 2.0f, scratch);
    }
    std::lock_guard<std::mutex> lk(mu);
    cache[hash] = ok;
    return ok;
}

bool scene_upscale_wanted() {
    const UpscaleMode m = upscale_mode();
    return m == UpscaleMode::Fsr1 || m == UpscaleMode::Nis;
}

SpatialUpscaler* scene_upscaler(void* gfx_device, VkExtent2D in, VkExtent2D out) {
    static Fsr1Spatial* fsr = nullptr;
    static NisSpatial* nis = nullptr;
    static bool tried_fsr = false, tried_nis = false, told_range = false;
    if (!gfx_device) return nullptr;
    gfx::Device& dev = *static_cast<gfx::Device*>(gfx_device);
    if (upscale_mode() == UpscaleMode::Nis) {
        if (!tried_nis) {
            tried_nis = true;
            auto* n = new NisSpatial();
            if (n->init(dev)) nis = n;
            else delete n;
        }
        if (nis && nis->supports(in, out)) return nis;
        if (nis && !told_range) {
            told_range = true;
            NXLOG_I("hdr", "scene upscale %ux%u -> %ux%u is beyond NIS's 2x: FSR 1 is used for it", in.width, in.height, out.width, out.height);
        }
    }
    if (!tried_fsr) {
        tried_fsr = true;
        auto* f = new Fsr1Spatial();
        if (f->init(dev)) fsr = f;
        else delete f;
    }
    return fsr;
}

float scene_upscale_prescale() { return 0.1f; }

}  // namespace nxrt::extras
