// SPDX-License-Identifier: MIT
// The cooperative-matrix HMMA probe (vk_coopmat_probe.h).
#include "vk_coopmat_probe.h"
#include <atomic>
#include <cstring>
#include <vector>
#include "nxrt/gfx.h"
#include "nxrt/log.h"
#include "vk_coopmat_probe_spv.h"

namespace nxrt::gfx {

namespace {
std::atomic<bool> g_enabled{false};
std::atomic<bool> g_float16{false};
}

bool coopmat_hmma_enabled() { return g_enabled.load(std::memory_order_relaxed); }
void set_coopmat_hmma_enabled(bool on) { g_enabled.store(on, std::memory_order_relaxed); }
bool float16_shaders_enabled() { return g_float16.load(std::memory_order_relaxed); }
void set_float16_shaders_enabled(bool on) { g_float16.store(on, std::memory_order_relaxed); }

bool coopmat_hmma_probe(Device& dev, std::string* why) {
    auto fail = [&](const std::string& w) { if (why) *why = w; return false; };
    if (!vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR) return fail("no vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR");
    uint32_t n = 0;
    vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR(dev.physical(), &n, nullptr);
    std::vector<VkCooperativeMatrixPropertiesKHR> props(n, {VK_STRUCTURE_TYPE_COOPERATIVE_MATRIX_PROPERTIES_KHR});
    if (n) vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR(dev.physical(), &n, props.data());
    auto has = [&](VkComponentTypeKHR acc) {
        for (auto& p : props)
            if (p.MSize == 16 && p.NSize == 8 && p.KSize == 16 && p.AType == VK_COMPONENT_TYPE_FLOAT16_KHR && p.BType == VK_COMPONENT_TYPE_FLOAT16_KHR &&
                p.CType == acc && p.ResultType == acc && p.scope == VK_SCOPE_SUBGROUP_KHR && !p.saturatingAccumulation)
                return true;
        return false;
    };
    if (!has(VK_COMPONENT_TYPE_FLOAT16_KHR) || !has(VK_COMPONENT_TYPE_FLOAT32_KHR)) return fail("no 16x8x16 float16 subgroup matrices");

    VkDevice d = dev.device();
    const VkDeviceSize data_bytes = sizeof(kCoopMatProbeInput), ubo_bytes = 0x10000;
    Buffer data, ubo;
    VkDescriptorSetLayout layouts[2] = {};
    VkPipelineLayout pl = VK_NULL_HANDLE;
    VkShaderModule sm = VK_NULL_HANDLE;
    VkPipeline pipe = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    std::string err;
    bool ok = false;
    do {
        if (dev.create_buffer(data_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, MemoryUse::Readback, &data, "coopmat probe") != VK_SUCCESS ||
            dev.create_buffer(ubo_bytes, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, MemoryUse::Upload, &ubo, "coopmat probe ubo") != VK_SUCCESS) {
            err = "buffer";
            break;
        }
        std::memcpy(data.mapped, kCoopMatProbeInput, sizeof(kCoopMatProbeInput));
        std::memset(ubo.mapped, 0, ubo_bytes);   // the support buffer every translated module declares: zero
        dev.flush_buffer(data);
        dev.flush_buffer(ubo);
        // the hardware-check module's layout: set 0 binding 0 uniform buffer, set 1 binding 0 storage buffer
        const VkDescriptorType types[2] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
        for (int s = 0; s < 2; ++s) {
            VkDescriptorSetLayoutBinding b = {0, types[s], 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
            VkDescriptorSetLayoutCreateInfo ci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            ci.bindingCount = 1;
            ci.pBindings = &b;
            if (vkCreateDescriptorSetLayout(d, &ci, nullptr, &layouts[s]) != VK_SUCCESS) err = "set layout";
        }
        if (!err.empty()) break;
        VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        plci.setLayoutCount = 2;
        plci.pSetLayouts = layouts;
        if (vkCreatePipelineLayout(d, &plci, nullptr, &pl) != VK_SUCCESS) { err = "pipeline layout"; break; }
        VkShaderModuleCreateInfo smci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        smci.codeSize = sizeof(kCoopMatProbeSpirv);
        smci.pCode = kCoopMatProbeSpirv;
        VkResult r = vkCreateShaderModule(d, &smci, nullptr, &sm);
        if (r != VK_SUCCESS) { err = "vkCreateShaderModule " + std::to_string(r); break; }
        VkComputePipelineCreateInfo cpci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        cpci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, sm, "main", nullptr};
        cpci.layout = pl;
        r = vkCreateComputePipelines(d, VK_NULL_HANDLE, 1, &cpci, nullptr, &pipe);
        if (r != VK_SUCCESS) { err = "vkCreateComputePipelines " + std::to_string(r); break; }
        VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}};
        VkDescriptorPoolCreateInfo dpci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dpci.maxSets = 2;
        dpci.poolSizeCount = 2;
        dpci.pPoolSizes = sizes;
        if (vkCreateDescriptorPool(d, &dpci, nullptr, &pool) != VK_SUCCESS) { err = "descriptor pool"; break; }
        VkDescriptorSet sets[2] = {};
        VkDescriptorSetAllocateInfo dsai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dsai.descriptorPool = pool;
        dsai.descriptorSetCount = 2;
        dsai.pSetLayouts = layouts;
        if (vkAllocateDescriptorSets(d, &dsai, sets) != VK_SUCCESS) { err = "descriptor sets"; break; }
        VkDescriptorBufferInfo bi[2] = {{ubo.buffer, 0, VK_WHOLE_SIZE}, {data.buffer, 0, VK_WHOLE_SIZE}};
        VkWriteDescriptorSet w[2] = {};
        for (int s = 0; s < 2; ++s) {
            w[s] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w[s].dstSet = sets[s];
            w[s].descriptorCount = 1;
            w[s].descriptorType = types[s];
            w[s].pBufferInfo = &bi[s];
        }
        vkUpdateDescriptorSets(d, 2, w, 0, nullptr);
        if (!dev.submit_once([&](VkCommandBuffer cb) {
                vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
                vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 2, sets, 0, nullptr);
                vkCmdDispatch(cb, 1, 1, 1);
                VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
                vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
            })) {
            err = "dispatch";
            break;
        }
        dev.invalidate_buffer(data);
        const uint32_t* out = static_cast<const uint32_t*>(data.mapped);
        int bad = 0;
        for (uint32_t lane = 0; lane < 32; ++lane)
            for (uint32_t q = 0; q < 2; ++q) {
                const uint32_t i = lane * kCoopMatProbeWords + kCoopMatProbeDst + q;
                if (out[i] != kCoopMatProbeExpected[i] && bad++ < 4)
                    NXLOG_W("gfx", "cooperative-matrix probe: lane %u D%u = %08x, expected %08x", lane, q, out[i], kCoopMatProbeExpected[i]);
            }
        if (bad) { err = std::to_string(bad) + " of 64 result words differ (element order)"; break; }
        ok = true;
    } while (false);
    if (pool) vkDestroyDescriptorPool(d, pool, nullptr);
    if (pipe) vkDestroyPipeline(d, pipe, nullptr);
    if (sm) vkDestroyShaderModule(d, sm, nullptr);
    if (pl) vkDestroyPipelineLayout(d, pl, nullptr);
    for (auto l : layouts) if (l) vkDestroyDescriptorSetLayout(d, l, nullptr);
    if (data) dev.destroy_buffer(data);
    if (ubo) dev.destroy_buffer(ubo);
    if (!ok) return fail("probe: " + err);
    if (why) *why = " (16x8x16 float16, probe dispatch matched)";
    return true;
}

}  // namespace nxrt::gfx
