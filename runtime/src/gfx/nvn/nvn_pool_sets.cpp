// SPDX-License-Identifier: MIT
// Pool arrays (A7): descriptor set layouts for the texture and sampler arrays the size of an NVN pool.
//
// NVN shaders sample through 32-bit handles: texture id (bits 0..19) into the bound texture pool, sampler id (bits 20..31) into the bound
// sampler pool. Most handles are fixed per draw (a binding slot, a uniform word, or an expression the runtime evaluates: HandleExpr), and
// those keep their plain per-draw binding. A handle computed per pixel (OoT's clustered-light shadow maps: TEX.SCR.B.DC with the light
// index from a storage loop, 4,336 fragment shaders) cannot be bound that way: the harness turns such a sample into Ryujinx's
// GenerateBindlessAccess (an index into a sampled-image array plus one into a sampler array, in extra descriptor sets), and the runtime
// binds those to persistent sets that mirror the bound pools (bind_pool_sets in nvn_replay.cpp fills and binds them).
//
// One set layout per descriptor type, shared by every pipeline layout: binding 0, the whole array, partially bound, updated after bind,
// and updatable while unused entries are pending (descriptors the GPU work in flight does not read can change).
#include "nvn_pipeline.h"

#include <mutex>

#include "nxrt/log.h"
#include "nvn_gpu_internal.h"

namespace nxrt::nvn::be::gpu {
namespace {
constexpr const char* kTag = "nvn.pool";
std::mutex g_mu;
VkDescriptorSetLayout g_layout[2] = {};   // [0] sampled images, [1] samplers
VkDescriptorSetLayout g_empty = VK_NULL_HANDLE;
bool g_tried[2] = {}, g_empty_tried = false;
}  // namespace

VkImageViewType pool_view_type(const ImageShape& s) {
    switch (s.dim) {
        case 0: return s.arrayed ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D;
        case 2: return VK_IMAGE_VIEW_TYPE_3D;
        case 3: return s.arrayed ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : VK_IMAGE_VIEW_TYPE_CUBE;
        default: return s.arrayed ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    }
}

VkDescriptorSetLayout pool_set_layout(bool sampler) {
    std::lock_guard<std::mutex> lk(g_mu);
    const int i = sampler ? 1 : 0;
    if (g_tried[i]) return g_layout[i];
    g_tried[i] = true;
    if (!device().features().descriptor_indexing) {
        NXLOG_W(kTag, "this GPU has no descriptor indexing with update-after-bind: shaders with pool arrays (per-pixel texture handles) cannot run");
        return VK_NULL_HANDLE;
    }
    VkDescriptorSetLayoutBinding b = {};
    b.binding = 0;
    b.descriptorType = sampler ? VK_DESCRIPTOR_TYPE_SAMPLER : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    b.descriptorCount = sampler ? kPoolSamplers : kPoolTextures;
    b.stageFlags = VK_SHADER_STAGE_ALL;
    const VkDescriptorBindingFlags bf = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
                                        VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
    VkDescriptorSetLayoutBindingFlagsCreateInfo fi = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    fi.bindingCount = 1;
    fi.pBindingFlags = &bf;
    VkDescriptorSetLayoutCreateInfo ci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, &fi};
    ci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    ci.bindingCount = 1;
    ci.pBindings = &b;
    if (vkCreateDescriptorSetLayout(vkdev(), &ci, nullptr, &g_layout[i]) != VK_SUCCESS) {
        NXLOG_E(kTag, "%s array set layout failed", sampler ? "sampler" : "texture");
        g_layout[i] = VK_NULL_HANDLE;
    }
    return g_layout[i];
}

VkDescriptorSetLayout pool_empty_set_layout() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_empty_tried) return g_empty;
    g_empty_tried = true;
    VkDescriptorSetLayoutCreateInfo ci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    if (vkCreateDescriptorSetLayout(vkdev(), &ci, nullptr, &g_empty) != VK_SUCCESS) g_empty = VK_NULL_HANDLE;
    return g_empty;
}

void pool_layouts_shutdown() {
    std::lock_guard<std::mutex> lk(g_mu);
    for (int i = 0; i < 2; ++i) {
        if (g_layout[i]) vkDestroyDescriptorSetLayout(vkdev(), g_layout[i], nullptr);
        g_layout[i] = VK_NULL_HANDLE;
        g_tried[i] = false;
    }
    if (g_empty) vkDestroyDescriptorSetLayout(vkdev(), g_empty, nullptr);
    g_empty = VK_NULL_HANDLE;
    g_empty_tried = false;
}

}  // namespace nxrt::nvn::be::gpu
