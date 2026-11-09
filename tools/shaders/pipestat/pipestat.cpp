// SPDX-License-Identifier: GPL-3.0-or-later
// pipestat: compile translated shader modules (.spv from the shader cache) into Vulkan pipelines on the local GPU and print the driver's
// statistics per executable (VK_KHR_pipeline_executable_properties: registers, binary size, spills ...). No game and no runtime: the
// descriptor set layouts come from the modules' own decorations. Vertex and mesh (+ optional task) modules become a graphics pipeline with
// no fragment stage and no attachments (dynamic rendering); compute modules a compute pipeline. For comparing translations of one block
// (e.g. the A17 pointer-access code) without a game run. Usage:
//   pipestat [--frag <fs.spv>] <a.spv> [<b.spv> ...]       (task modules: pass "task.spv+mesh.spv" as one argument)
// Build: tools\shaders\pipestat\build.cmd (clang-cl, volk and Vulkan-Headers from the game build's FetchContent folders).
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include "volk.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

std::vector<uint32_t> read_spv(const std::string& path) {
    std::vector<uint32_t> w;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return w;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    w.resize(static_cast<size_t>(n) / 4);
    if (std::fread(w.data(), 4, w.size(), f) != w.size()) w.clear();
    std::fclose(f);
    return w;
}

struct Binding { VkDescriptorType type; uint32_t count; };
struct Module {
    std::vector<uint32_t> words;
    VkShaderStageFlagBits stage = VK_SHADER_STAGE_ALL;
    std::string entry = "main";
    std::map<uint32_t, std::map<uint32_t, Binding>> sets;   // set -> binding -> type
    uint32_t push_bytes = 0;
};

// Descriptor bindings from the module's decorations and variable types.
bool reflect(Module& m) {
    const auto& w = m.words;
    if (w.size() < 5 || w[0] != 0x07230203) return false;
    std::map<uint32_t, uint32_t> set_of, binding_of;
    std::set<uint32_t> block, buffer_block;
    std::map<uint32_t, std::vector<uint32_t>> types;   // id -> instruction words
    struct Var { uint32_t type, sc; };
    std::map<uint32_t, Var> vars;
    for (size_t i = 5; i < w.size();) {
        uint32_t n = w[i] >> 16, op = w[i] & 0xffff;
        if (n == 0) return false;
        const uint32_t* a = &w[i + 1];
        switch (op) {
        case 15: {   // OpEntryPoint
            uint32_t model = a[0];
            m.stage = model == 0 ? VK_SHADER_STAGE_VERTEX_BIT : model == 4 ? VK_SHADER_STAGE_FRAGMENT_BIT : model == 5 ? VK_SHADER_STAGE_COMPUTE_BIT
                    : model == 5364 ? VK_SHADER_STAGE_TASK_BIT_EXT : model == 5365 ? VK_SHADER_STAGE_MESH_BIT_EXT
                    : model == 1 ? VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT : model == 2 ? VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT
                    : VK_SHADER_STAGE_GEOMETRY_BIT;
            m.entry = reinterpret_cast<const char*>(&a[2]);
            break;
        }
        case 71:   // OpDecorate
            if (a[1] == 34) set_of[a[0]] = a[2];
            else if (a[1] == 33) binding_of[a[0]] = a[2];
            else if (a[1] == 2) block.insert(a[0]);
            else if (a[1] == 3) buffer_block.insert(a[0]);
            break;
        case 59: vars[a[1]] = {a[0], a[2]}; break;   // OpVariable
        default:
            if ((op >= 19 && op <= 39) || op == 4472 || op == 5341) types[a[0]] = std::vector<uint32_t>(a, a + n - 1);
            break;
        }
        i += n;
    }
    for (auto& [id, v] : vars) {
        if (v.sc == 9) {   // PushConstant: size not needed exactly; 256 bytes covers Ryujinx's push data
            m.push_bytes = 256;
            continue;
        }
        if (!set_of.count(id) || !binding_of.count(id)) continue;
        auto pt = types.find(v.type);   // OpTypePointer: {result, storage class, type}
        if (pt == types.end() || pt->second.size() < 3) continue;
        uint32_t t = pt->second[2];
        uint32_t count = 1;
        // unwrap arrays (OpTypeArray 28, OpTypeRuntimeArray 29)
        for (int guard = 0; guard < 4; guard++) {
            bool unwrapped = false;
            for (size_t i = 5; i < w.size();) {
                uint32_t n = w[i] >> 16, op = w[i] & 0xffff;
                if ((op == 28 || op == 29) && w[i + 1] == t) {
                    t = w[i + 2];
                    count = op == 29 ? 4096 : 0;
                    if (op == 28) {   // find the length constant
                        uint32_t len_id = w[i + 3];
                        for (size_t j = 5; j < w.size();) {
                            uint32_t nj = w[j] >> 16, oj = w[j] & 0xffff;
                            if (oj == 43 && w[j + 2] == len_id) { count = w[j + 3]; break; }
                            j += nj;
                        }
                    }
                    unwrapped = true;
                    break;
                }
                i += n;
            }
            if (!unwrapped) break;
        }
        VkDescriptorType dt;
        auto ti = types.find(t);
        uint32_t top = 0;
        for (size_t i = 5; i < w.size();) {   // opcode of the type
            uint32_t n = w[i] >> 16, op = w[i] & 0xffff;
            if (n >= 2 && ((op >= 19 && op <= 39)) && w[i + 1] == t) { top = op; break; }
            i += n;
        }
        if (v.sc == 12 || (v.sc == 2 && buffer_block.count(t))) dt = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        else if (v.sc == 2) dt = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        else if (top == 27) dt = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        else if (top == 26) dt = VK_DESCRIPTOR_TYPE_SAMPLER;
        else if (top == 25 && ti != types.end() && ti->second.size() >= 8) {
            uint32_t sampled = ti->second[6], dim = ti->second[2];
            if (dim == 5) dt = sampled == 2 ? VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
            else dt = sampled == 2 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        } else if (top == 5341) dt = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        else continue;
        m.sets[set_of[id]][binding_of[id]] = {dt, count ? count : 1};
    }
    return true;
}

#define CK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { std::fprintf(stderr, "%s failed: %d\n", #x, r_); return false; } } while (0)

struct Ctx {
    VkInstance inst = VK_NULL_HANDLE;
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE;
};

bool init(Ctx& c) {
    if (volkInitialize() != VK_SUCCESS) { std::fprintf(stderr, "no vulkan-1.dll\n"); return false; }
    VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    ai.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &ai;
    CK(vkCreateInstance(&ici, nullptr, &c.inst));
    volkLoadInstance(c.inst);
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(c.inst, &n, nullptr);
    std::vector<VkPhysicalDevice> pds(n);
    vkEnumeratePhysicalDevices(c.inst, &n, pds.data());
    for (auto p : pds) {
        VkPhysicalDeviceProperties pr;
        vkGetPhysicalDeviceProperties(p, &pr);
        if (pr.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU || c.pd == VK_NULL_HANDLE) c.pd = p;
    }
    if (!c.pd) return false;
    VkPhysicalDeviceProperties pr;
    vkGetPhysicalDeviceProperties(c.pd, &pr);
    std::printf("device: %s, driver 0x%x\n", pr.deviceName, pr.driverVersion);

    // the features the runtime turns on that change code generation of these modules
    VkPhysicalDeviceMeshShaderFeaturesEXT mesh{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT};
    VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR pep{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR, &mesh};
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, &pep};
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f13};
    VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &f12};
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f11};
    vkGetPhysicalDeviceFeatures2(c.pd, &f2);
    mesh.multiviewMeshShader = VK_FALSE;
    mesh.primitiveFragmentShadingRateMeshShader = VK_FALSE;
    const char* exts[] = {VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME, VK_EXT_MESH_SHADER_EXTENSION_NAME};
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f2};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 2;
    dci.ppEnabledExtensionNames = exts;
    CK(vkCreateDevice(c.pd, &dci, nullptr, &c.dev));
    volkLoadDevice(c.dev);
    return true;
}

bool module_of(Ctx& c, const Module& m, VkShaderModule* out) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = m.words.size() * 4;
    ci.pCode = m.words.data();
    CK(vkCreateShaderModule(c.dev, &ci, nullptr, out));
    return true;
}

bool layout_of(Ctx& c, const std::vector<Module*>& ms, VkPipelineLayout* out) {
    std::map<uint32_t, std::map<uint32_t, VkDescriptorSetLayoutBinding>> all;
    uint32_t push = 0;
    VkShaderStageFlags stages = 0;
    for (auto* m : ms) stages |= m->stage;
    for (auto* m : ms) {
        push = std::max(push, m->push_bytes);
        for (auto& [s, bs] : m->sets)
            for (auto& [b, t] : bs) {
                auto& e = all[s][b];
                e.binding = b;
                e.descriptorType = t.type;
                e.descriptorCount = std::max(e.descriptorCount, t.count);
                e.stageFlags = stages;
            }
    }
    uint32_t nsets = all.empty() ? 0 : all.rbegin()->first + 1;
    std::vector<VkDescriptorSetLayout> sls(nsets, VK_NULL_HANDLE);
    for (uint32_t s = 0; s < nsets; s++) {
        std::vector<VkDescriptorSetLayoutBinding> v;
        std::vector<VkDescriptorBindingFlags> fl;
        if (all.count(s))
            for (auto& [b, e] : all[s]) { v.push_back(e); fl.push_back(e.descriptorCount > 1 ? VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT : 0); }
        VkDescriptorSetLayoutBindingFlagsCreateInfo bf{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
        bf.bindingCount = static_cast<uint32_t>(fl.size());
        bf.pBindingFlags = fl.data();
        VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, &bf};
        ci.bindingCount = static_cast<uint32_t>(v.size());
        ci.pBindings = v.data();
        CK(vkCreateDescriptorSetLayout(c.dev, &ci, nullptr, &sls[s]));
    }
    VkPushConstantRange pr{stages, 0, push};
    VkPipelineLayoutCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    ci.setLayoutCount = nsets;
    ci.pSetLayouts = sls.data();
    ci.pushConstantRangeCount = push ? 1 : 0;
    ci.pPushConstantRanges = &pr;
    CK(vkCreatePipelineLayout(c.dev, &ci, nullptr, out));
    return true;
}

void print_stats(Ctx& c, VkPipeline p, const std::string& label, double ms) {
    VkPipelineInfoKHR pi{VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR};
    pi.pipeline = p;
    uint32_t n = 0;
    vkGetPipelineExecutablePropertiesKHR(c.dev, &pi, &n, nullptr);
    std::vector<VkPipelineExecutablePropertiesKHR> ex(n, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_PROPERTIES_KHR});
    vkGetPipelineExecutablePropertiesKHR(c.dev, &pi, &n, ex.data());
    for (uint32_t e = 0; e < n; e++) {
        VkPipelineExecutableInfoKHR ei{VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR};
        ei.pipeline = p;
        ei.executableIndex = e;
        uint32_t ns = 0;
        vkGetPipelineExecutableStatisticsKHR(c.dev, &ei, &ns, nullptr);
        std::vector<VkPipelineExecutableStatisticKHR> st(ns, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR});
        vkGetPipelineExecutableStatisticsKHR(c.dev, &ei, &ns, st.data());
        std::printf("%s\t%.0f ms\t%s", label.c_str(), ms, ex[e].name);
        for (auto& s : st) {
            std::printf("\t%s=", s.name);
            switch (s.format) {
            case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_BOOL32_KHR: std::printf("%u", s.value.b32); break;
            case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_INT64_KHR: std::printf("%lld", static_cast<long long>(s.value.i64)); break;
            case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR: std::printf("%llu", static_cast<unsigned long long>(s.value.u64)); break;
            case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_FLOAT64_KHR: std::printf("%g", s.value.f64); break;
            default: break;
            }
        }
        std::printf("\n");
        // PIPESTAT_IR=<folder>: the driver's internal representations (if it offers any) as text files
        if (const char* dir = std::getenv("PIPESTAT_IR")) {
            uint32_t nr = 0;
            vkGetPipelineExecutableInternalRepresentationsKHR(c.dev, &ei, &nr, nullptr);
            std::vector<VkPipelineExecutableInternalRepresentationKHR> ir(nr, {VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INTERNAL_REPRESENTATION_KHR});
            vkGetPipelineExecutableInternalRepresentationsKHR(c.dev, &ei, &nr, ir.data());
            std::vector<std::vector<char>> buf(nr);
            for (uint32_t r = 0; r < nr; r++) { buf[r].resize(ir[r].dataSize); ir[r].pData = buf[r].data(); }
            vkGetPipelineExecutableInternalRepresentationsKHR(c.dev, &ei, &nr, ir.data());
            for (uint32_t r = 0; r < nr; r++) {
                std::string fn = std::string(dir) + "/" + ex[e].name + "_" + std::to_string(r) + (ir[r].isText ? ".txt" : ".bin");
                for (auto& ch : fn) if (ch == ' ') ch = '_';
                if (FILE* f = std::fopen(fn.c_str(), "wb")) { std::fwrite(buf[r].data(), 1, buf[r].size(), f); std::fclose(f); }
                std::printf("  ir %s: %s (%zu bytes)\n", ir[r].name, fn.c_str(), buf[r].size());
            }
        }
    }
    std::fflush(stdout);
}

VkPipelineCreateFlags capture_flags() {
    return VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR |
           (std::getenv("PIPESTAT_IR") ? VK_PIPELINE_CREATE_CAPTURE_INTERNAL_REPRESENTATIONS_BIT_KHR : 0);
}

bool run(Ctx& c, const std::string& arg, Module* frag) {
    std::vector<Module> ms;
    size_t start = 0;
    while (start <= arg.size()) {
        size_t plus = arg.find('+', start);
        std::string p = arg.substr(start, plus == std::string::npos ? std::string::npos : plus - start);
        Module m;
        m.words = read_spv(p);
        if (m.words.empty() || !reflect(m)) { std::fprintf(stderr, "%s: not a SPIR-V module\n", p.c_str()); return false; }
        ms.push_back(std::move(m));
        if (plus == std::string::npos) break;
        start = plus + 1;
    }
    std::vector<Module*> all;
    for (auto& m : ms) all.push_back(&m);
    if (frag && ms[0].stage != VK_SHADER_STAGE_COMPUTE_BIT) all.push_back(frag);
    VkPipelineLayout layout;
    if (!layout_of(c, all, &layout)) return false;
    std::vector<VkShaderModule> mods(all.size());
    std::vector<VkPipelineShaderStageCreateInfo> st(all.size());
    for (size_t i = 0; i < all.size(); i++) {
        if (!module_of(c, *all[i], &mods[i])) return false;
        st[i] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        st[i].stage = all[i]->stage;
        st[i].module = mods[i];
        st[i].pName = all[i]->entry.c_str();
    }
    VkPipeline p = VK_NULL_HANDLE;
    std::fprintf(stderr, "%s: creating the pipeline\n", arg.c_str());
    auto t0 = std::chrono::steady_clock::now();
    if (ms[0].stage == VK_SHADER_STAGE_COMPUTE_BIT) {
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.flags = capture_flags();
        ci.stage = st[0];
        ci.layout = layout;
        CK(vkCreateComputePipelines(c.dev, VK_NULL_HANDLE, 1, &ci, nullptr, &p));
    } else {
        bool meshp = false;
        for (auto* m : all) meshp |= m->stage == VK_SHADER_STAGE_MESH_BIT_EXT;
        VkPipelineRenderingCreateInfo rci{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        rci.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vp.viewportCount = 1;
        vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms_{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms_.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        ds.depthTestEnable = VK_TRUE;
        ds.depthWriteEnable = VK_TRUE;
        ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dy.dynamicStateCount = 2;
        dy.pDynamicStates = dyn;
        VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &rci};
        ci.flags = capture_flags();
        ci.stageCount = static_cast<uint32_t>(st.size());
        ci.pStages = st.data();
        ci.pVertexInputState = meshp ? nullptr : &vi;
        ci.pInputAssemblyState = meshp ? nullptr : &ia;
        ci.pViewportState = &vp;
        ci.pRasterizationState = &rs;
        ci.pMultisampleState = &ms_;
        ci.pDepthStencilState = &ds;
        ci.pColorBlendState = &cb;
        ci.pDynamicState = &dy;
        ci.layout = layout;
        CK(vkCreateGraphicsPipelines(c.dev, VK_NULL_HANDLE, 1, &ci, nullptr, &p));
    }
    double took = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    print_stats(c, p, arg, took);
    vkDestroyPipeline(c.dev, p, nullptr);
    for (auto m : mods) vkDestroyShaderModule(c.dev, m, nullptr);
    vkDestroyPipelineLayout(c.dev, layout, nullptr);
    return true;
}

}   // namespace

int main(int argc, char** argv) {
    Ctx c;
    if (!init(c)) return 2;
    Module frag;
    bool have_frag = false;
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--frag" && i + 1 < argc) {
            frag.words = read_spv(argv[++i]);
            have_frag = !frag.words.empty() && reflect(frag);
            continue;
        }
        if (!run(c, a, have_frag ? &frag : nullptr)) rc = 1;
    }
    vkDestroyDevice(c.dev, nullptr);
    vkDestroyInstance(c.inst, nullptr);
    return rc;
}
