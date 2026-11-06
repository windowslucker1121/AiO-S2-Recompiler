// SPDX-License-Identifier: GPL-3.0-or-later
// Pipeline layouts, shader modules and pipelines (see nvn_pipeline.h). Pipeline creation is synchronous on the render thread; a
// VkPipelineCache persists between runs ([nvn] pipeline_cache, default "pipeline_cache.bin").
#include "nvn_pipeline.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "nxrt/config.h"
#include "nxrt/ftrace.h"
#include "nxrt/gauge.h"
#include "nxrt/log.h"
#include "nvn_gpu_internal.h"
#include "nvn_spirv_patch.h"
#include "extras/hdr_nvn.h"
#include "nxrt/extras.h"

namespace nxrt::nvn::be::gpu {
namespace {

constexpr const char* kTag = "nvn.pipe";

struct LayoutKey {
    const ShaderInfo* s[kProgStages];
    bool operator<(const LayoutKey& o) const { return std::memcmp(s, o.s, sizeof s) < 0; }
};

// Shader modules and layouts are also made by the prewarm workers (nvn_prewarm.cpp), so they have locks; the pipeline buckets below stay
// render thread only.
std::mutex g_layout_mu;
std::recursive_mutex g_module_mu;   // every module cache below; the variant makers call shader_module
std::map<LayoutKey, ProgLayout> g_layouts;
// Descriptor set layouts and the pipeline layout built from them, shared by every program whose descriptors are the same (S24): two programs with
// the same signature get the same VkPipelineLayout handle, so the shader parts compiled for one are found again for the other.
struct SharedLayout { VkDescriptorSetLayout sets[4] = {}; VkPipelineLayout layout = VK_NULL_HANDLE; };
std::map<std::string, SharedLayout> g_shared_layouts;
std::unordered_map<const ShaderInfo*, VkShaderModule> g_modules;
std::mutex g_module_id_mu;
std::unordered_map<VkShaderModule, uint64_t> g_module_ids;   // every module create_module made: the hash of its words (module_content_id)
std::atomic<uint64_t> g_built_at_draw{0};                    // whole pipelines built by the render thread at a draw
std::atomic<bool> g_cache_dirty{false};
std::atomic<uint32_t> g_new_since_save{0};      // pipelines and parts created since the cache file was last written
bool g_cache_big = false;                       // the file loaded at start was large (a shader preparation made it): rewritten rarely, see pipeline_cache_dirty
// Producer modules with extra outputs (nvn_spirv_patch.h): (shader, slots as location * 4 + scalar type).
std::map<std::pair<const ShaderInfo*, std::vector<uint32_t>>, VkShaderModule> g_variants;
std::unordered_map<const ShaderInfo*, VkShaderModule> g_dual_src;
std::unordered_map<const ShaderInfo*, std::pair<VkShaderModule, bool>> g_no_out;   // (module, owned by this map)      // fragment shaders with private outputs (passes without colour attachments)
std::unordered_map<const ShaderInfo*, std::pair<VkShaderModule, uint32_t>> g_xfb_mod;   // vertex shaders with the captured outputs, their stride       // fragment shaders with the second blend source at Location 0, Index 1
// One pipeline per key. `up`: a linked pipeline (nvn_gpl.cpp) that a background thread is replacing with an optimised one; the lookup swaps it in.
struct GfxEntry {
    GraphicsKey first;
    VkPipeline second = VK_NULL_HANDLE;
    VkPipeline alt = VK_NULL_HANDLE;                 // measurement only ([nvn] gpl_ab): the optimised pipeline, kept next to the linked one
    std::shared_ptr<PipeUpgrade> up;
};
std::unordered_map<uint64_t, std::vector<GfxEntry>> g_gfx;
std::map<std::pair<const ShaderInfo*, VkPipelineLayout>, VkPipeline> g_compute;
VkPipelineCache g_cache = VK_NULL_HANDLE;
bool g_cache_tried = false;
std::string g_cache_path;
nxrt::GaugeCounter g_pipeline_count{"nvn.pipelines"};   // live VkPipelines (graphics and compute, linked ones included); render thread
std::unordered_map<const ShaderInfo*, std::pair<gfx::Buffer, VkDeviceSize>> g_const_bufs;
gfx::Buffer g_driver_const;
bool g_driver_const_made = false;
// Counters mirrored at the sites that change the maps above (they are render thread only, so the gauge reads these instead).
nxrt::GaugeCounter g_pipes_created{"nvn.pipelines_created"};     // ever; same events as counters().pipelines_created (shader parts included)
nxrt::GaugeCounter g_gfx_keys{"nvn.pipeline_keys"};              // entries in g_gfx: one per graphics key tried (a failed build keeps its entry)
nxrt::GaugeCounter g_compute_keys{"nvn.compute_pipeline_keys"};  // entries in g_compute
nxrt::GaugeCounter g_const_n{"nvn.const_buffers"};               // entries in g_const_bufs
nxrt::GaugeCounter g_const_bytes{"nvn.const_buffers.bytes"};     // bytes of the constant buffers in g_const_bufs

VkShaderStageFlagBits stage_bit(ShaderStage s) {
    switch (s) {
        case ShaderStage::Vertex: return VK_SHADER_STAGE_VERTEX_BIT;
        case ShaderStage::Fragment: return VK_SHADER_STAGE_FRAGMENT_BIT;
        case ShaderStage::Geometry: return VK_SHADER_STAGE_GEOMETRY_BIT;
        case ShaderStage::TessControl: return VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
        case ShaderStage::TessEval: return VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
        case ShaderStage::Compute: return VK_SHADER_STAGE_COMPUTE_BIT;
        case ShaderStage::Task: return VK_SHADER_STAGE_TASK_BIT_EXT;
        case ShaderStage::Mesh: return VK_SHADER_STAGE_MESH_BIT_EXT;
        default: return VK_SHADER_STAGE_VERTEX_BIT;
    }
}

std::mutex g_cache_mu;
std::atomic<bool> g_cache_ready{false};
std::mutex g_save_mu;                   // one save at a time (the cache-save thread, the preparation's end, the shutdown flush)
uint64_t g_cache_base = 0;              // bytes the driver held right after the file was loaded: a save never writes less (see save_cache)
bool g_cache_unreadable = false;        // the file exists but could not be read: never overwritten this session

double ms_from(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

// The cache file's bytes. A shader preparation leaves gigabytes on NVIDIA, so sizes are 64-bit: the C library's ftell answers -1 from 2 GiB on
// (a prepared cache was then not loaded at all, and the session's next save replaced it with the few hundred MB it had compiled itself).
bool read_cache_file(const std::string& path, std::vector<char>* out, uint64_t* file_bytes) {
    std::error_code ec;
    const std::filesystem::path p(std::u8string(reinterpret_cast<const char8_t*>(path.data()), path.size()));
    *file_bytes = 0;
    if (!std::filesystem::is_regular_file(p, ec)) return true;          // no file: nothing to load, nothing at stake
    const uintmax_t len = std::filesystem::file_size(p, ec);
    if (ec) return false;
    *file_bytes = static_cast<uint64_t>(len);
    if (len == 0) return true;
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out->resize(static_cast<size_t>(len));
    constexpr size_t kChunk = size_t(256) << 20;                          // in pieces: no single read call of gigabytes
    for (size_t at = 0; at < out->size();) {
        const size_t n = std::min(kChunk, out->size() - at);
        f.read(out->data() + at, static_cast<std::streamsize>(n));
        if (static_cast<size_t>(f.gcount()) != n) { out->clear(); return false; }
        at += n;
    }
    return true;
}

// Why a driver would ignore the file: its header names another device or driver build (VkPipelineCacheHeaderVersionOne).
std::string cache_header_mismatch(const std::vector<char>& data) {
    if (data.size() < 32) return "the file is shorter than a pipeline cache header";
    uint32_t w[4];
    std::memcpy(w, data.data(), sizeof w);
    const VkPhysicalDeviceProperties& p = device().properties();
    if (w[1] != VK_PIPELINE_CACHE_HEADER_VERSION_ONE) return "unknown header version";
    if (w[2] != p.vendorID || w[3] != p.deviceID) return "it was made on another GPU";
    if (std::memcmp(data.data() + 16, p.pipelineCacheUUID, VK_UUID_SIZE) != 0) return "it was made by another driver version (pipelineCacheUUID)";
    return {};
}

void ensure_cache() {
    if (g_cache_ready.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> lk(g_cache_mu);       // the prewarm workers and the render thread may get here together
    if (g_cache_tried) return;
    g_cache_tried = true;
    // Default: next to the shader cache, where the shader preparation's programs.bin and prepare.state are (a path relative to the working
    // folder put the prepared cache wherever the preparation was started); a file in the working folder is still used when that one is missing.
    std::string def = "pipeline_cache.bin";
    std::error_code ec;
    if (shader_cache().is_open()) {
        const std::filesystem::path p = std::filesystem::path(shader_cache().dir()).parent_path() / "pipeline_cache.bin";
        if (std::filesystem::exists(p, ec) || !std::filesystem::exists(def, ec)) def = p.string();
    }
    g_cache_path = config_get_str("nvn", "pipeline_cache", def.c_str());
    std::vector<char> data;
    uint64_t file_bytes = 0;
    const auto t0 = std::chrono::steady_clock::now();
    if (!g_cache_path.empty() && !read_cache_file(g_cache_path, &data, &file_bytes)) {
        g_cache_unreadable = true;
        NXLOG_W(kTag, "pipeline cache %s (%llu bytes) could not be read: the session starts with an empty cache and leaves the file alone",
                g_cache_path.c_str(), static_cast<unsigned long long>(file_bytes));
    }
    const double read_ms = ms_from(t0);
    g_cache_big = file_bytes >= (64u << 20);
    VkPipelineCacheCreateInfo ci = {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    ci.initialDataSize = data.size();
    ci.pInitialData = data.empty() ? nullptr : data.data();
    const auto t1 = std::chrono::steady_clock::now();
    if (vkCreatePipelineCache(vkdev(), &ci, nullptr, &g_cache) != VK_SUCCESS) {
        NXLOG_W(kTag, "the driver refused the pipeline cache %s (%zu bytes): starting empty", g_cache_path.c_str(), data.size());
        ci.initialDataSize = 0;
        ci.pInitialData = nullptr;
        vkCreatePipelineCache(vkdev(), &ci, nullptr, &g_cache);
    } else if (!data.empty()) {
        const double create_ms = ms_from(t1);
        // What the driver kept of it: the floor for every later save. A driver ignores a file made by another build without an error.
        size_t held = 0;
        const auto t2 = std::chrono::steady_clock::now();
        vkGetPipelineCacheData(vkdev(), g_cache, &held, nullptr);
        g_cache_base = held;
        const std::string why = cache_header_mismatch(data);
        NXLOG_I(kTag, "pipeline cache loaded: %zu bytes from %s (read %.0f ms, driver %.0f ms; the driver holds %zu bytes, size query %.0f ms)%s%s",
                data.size(), g_cache_path.c_str(), read_ms, create_ms, held, ms_from(t2), why.empty() ? "" : "; ignored: ", why.c_str());
    }
    g_cache_ready.store(true, std::memory_order_release);
}

// Returns the size of the file written (0: nothing written).
uint64_t save_cache() {
    if (!g_cache || g_cache_path.empty()) return 0;
    std::lock_guard<std::mutex> lk(g_save_mu);
    NXFT("pipe.cache.save");
    g_cache_dirty.store(false);
    g_new_since_save.store(0);
    if (g_cache_unreadable) return 0;
    const auto t0 = std::chrono::steady_clock::now();
    size_t n = 0;
    if (vkGetPipelineCacheData(vkdev(), g_cache, &n, nullptr) != VK_SUCCESS || !n) return 0;
    // Never fewer bytes than the driver held after loading the file: a driver that dropped loaded entries (or a load that went wrong) would
    // otherwise replace a prepared cache of gigabytes with what this session compiled.
    if (n < g_cache_base) {
        static std::atomic<bool> warned{false};
        if (!warned.exchange(true))
            NXLOG_W(kTag, "pipeline cache not written: the driver now returns %zu bytes, fewer than the %llu it held after loading %s", n,
                    static_cast<unsigned long long>(g_cache_base), g_cache_path.c_str());
        return 0;
    }
    std::vector<char> data(n);
    if (vkGetPipelineCacheData(vkdev(), g_cache, &n, data.data()) != VK_SUCCESS) return 0;
    const double get_ms = ms_from(t0);
    // written to a temporary file and renamed, so a process that ends in the middle of it leaves the old cache intact
    const std::filesystem::path path(std::u8string(reinterpret_cast<const char8_t*>(g_cache_path.data()), g_cache_path.size()));
    std::filesystem::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return 0;
        f.write(data.data(), static_cast<std::streamsize>(n));
        if (!f) return 0;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::filesystem::remove(path, ec);
        std::filesystem::rename(tmp, path, ec);
    }
    if (ec) return 0;
    if (n > g_cache_base) g_cache_base = n;
    if (n >= (64u << 20)) g_cache_big = true;       // from now on rewritten as rarely as a loaded big one
    if (g_cache_big || n >= (64u << 20)) NXLOG_I(kTag, "pipeline cache saved: %zu bytes to %s (driver %.0f ms, %.0f ms in all)", n, g_cache_path.c_str(), get_ms, ms_from(t0));
    return n;
}

}  // namespace

// A cache of hundreds of MB (the shader preparation's) is rewritten only after thousands of new pipelines: the new ones are few next to it and
// the write takes seconds. The ordinary small cache is written whenever something new appeared, as before.
bool pipeline_cache_dirty() {
    if (!g_cache_dirty.load(std::memory_order_relaxed)) return false;
    return !g_cache_big || g_new_since_save.load(std::memory_order_relaxed) >= 3000;
}
uint64_t pipeline_cache_save() { return save_cache(); }
// The write at the end of a session: skipped when nothing changed since the last save (the preparation has just written its gigabytes),
// and when the cache is a big one and little was added.
void pipeline_cache_flush() {
    if (!g_cache_dirty.load(std::memory_order_relaxed)) return;
    if (!g_cache_big || g_new_since_save.load(std::memory_order_relaxed) >= 300) save_cache();
}
std::string pipeline_cache_path() {
    ensure_cache();
    return g_cache_path;
}

bool GraphicsKey::operator==(const GraphicsKey& o) const { return std::memcmp(this, &o, sizeof *this) == 0; }
uint64_t GraphicsKey::hash() const {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(this);
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < sizeof *this; ++i) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

// A module for SPIR-V words. The translator declares OpCapability Float64 in every module although no module uses a 64-bit float;
// devices without shaderFloat64 (Intel iGPUs) reject that, so the capability is dropped when no OpTypeFloat 64 exists.
static VkShaderModule create_module(const std::vector<uint32_t>& words, const char* what) {
    std::vector<uint32_t> fixed;
    const std::vector<uint32_t>* code = &words;
    {
        const auto& w = words;
        size_t cap_at = 0;
        bool uses64 = false;
        for (size_t i = 5; i < w.size();) {
            const uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
            if (len == 0) break;
            if (op == 17 && w[i + 1] == 10) cap_at = i;                       // OpCapability Float64
            if (op == 22 && i + 2 < w.size() && w[i + 2] == 64) uses64 = true;  // OpTypeFloat 64
            i += len;
        }
        if (cap_at && !uses64) {
            fixed = w;
            fixed.erase(fixed.begin() + static_cast<std::ptrdiff_t>(cap_at), fixed.begin() + static_cast<std::ptrdiff_t>(cap_at) + 2);
            code = &fixed;
        }
    }
    // [nvn] spirv_salt (measurement only): an OpSourceExtension with the salt in every module gives the driver a new shader hash, so a run
    // compiles every shader from scratch without touching the driver's own cache on disk.
    static const std::string salt = config_get_str("nvn", "spirv_salt", "");
    std::vector<uint32_t> salted;
    if (!salt.empty() && code->size() > 5) {
        salted = *code;
        size_t at = 5;
        while (at < salted.size()) {
            const uint32_t op = salted[at] & 0xffff, len = salted[at] >> 16;
            if (len == 0) break;
            if (op != 17 && op != 10 && op != 11 && op != 14 && op != 15 && op != 16 && op != 331) break;   // capability .. execution mode
            at += len;
        }
        std::vector<uint32_t> ins((salt.size() + 1 + 3) / 4 + 1, 0);
        ins[0] = (static_cast<uint32_t>(ins.size()) << 16) | 4u;                 // OpSourceExtension
        std::memcpy(&ins[1], salt.c_str(), salt.size());
        salted.insert(salted.begin() + static_cast<std::ptrdiff_t>(at), ins.begin(), ins.end());
        code = &salted;
    }
    VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = code->size() * 4;
    ci.pCode = code->data();
    VkShaderModule m = VK_NULL_HANDLE;
    const VkResult r = vkCreateShaderModule(vkdev(), &ci, nullptr, &m);
    if (r != VK_SUCCESS) {
        NXLOG_E(kTag, "vkCreateShaderModule failed (%d) for %s", static_cast<int>(r), what);
        return m;
    }
    uint64_t id = 1469598103934665603ull ^ (static_cast<uint64_t>(code->size()) * 0x9e3779b97f4a7c15ull);
    for (uint32_t w : *code) id = (id ^ w) * 1099511628211ull;
    {
        std::lock_guard<std::mutex> lk(g_module_id_mu);
        g_module_ids[m] = id;
    }
    return m;
}

VkShaderModule shader_module(const ShaderInfo* s) {
    if (!s) return VK_NULL_HANDLE;
    std::lock_guard<std::recursive_mutex> lk(g_module_mu);
    auto it = g_modules.find(s);
    if (it != g_modules.end()) return it->second;
    VkShaderModule m = create_module(s->spirv, s->hash.c_str());
    g_modules[s] = m;
    return m;
}

static const std::pair<VkShaderModule, uint32_t>& xfb_variant(const ShaderInfo* vs) {
    std::lock_guard<std::recursive_mutex> lk(g_module_mu);
    auto it = g_xfb_mod.find(vs);
    if (it != g_xfb_mod.end()) return it->second;
    std::vector<uint32_t> patched;
    std::vector<SpvXfbOutput> lay;
    uint32_t stride = 0;
    VkShaderModule m = VK_NULL_HANDLE;
    if (spirv_add_xfb(vs->spirv, patched, &lay, &stride)) {
        m = create_module(patched, vs->hash.c_str());
        std::string desc;
        for (const SpvXfbOutput& o : lay) desc += " loc" + std::to_string(o.location) + "x" + std::to_string(o.components) + "@" + std::to_string(o.offset);
        NXLOG_I(kTag, "%s: transform feedback variant, %u bytes per vertex:%s", vs->hash.c_str(), stride, desc.c_str());
    } else {
        NXLOG_W(kTag, "%s: no transform feedback variant could be made (no Location outputs?)", vs->hash.c_str());
    }
    return g_xfb_mod.emplace(vs, std::make_pair(m, stride)).first->second;
}

uint32_t xfb_stride(const ShaderInfo* vs) { return vs ? xfb_variant(vs).second : 0; }

static VkShaderModule no_output_module(const ShaderInfo* fs) {
    std::lock_guard<std::recursive_mutex> lk(g_module_mu);
    auto it = g_no_out.find(fs);
    if (it != g_no_out.end()) return it->second.first;
    std::vector<uint32_t> patched;
    std::pair<VkShaderModule, bool> e = {VK_NULL_HANDLE, false};
    if (spirv_outputs_to_private(fs->spirv, patched)) e = {create_module(patched, fs->hash.c_str()), true};
    else e = {shader_module(fs), false};
    g_no_out[fs] = e;
    return e.first;
}

// The tone-map fragment shader runs as its HDR variant while the HDR scene is active (extras/hdr_nvn.h); every other fragment shader is the translated one.
static VkShaderModule hdr_fragment_module(const ShaderInfo* fs) {
    if (!extras::hdr_scene_active() || fs->spirv.size() > 65536) return shader_module(fs);   // Pokopia's composite tone map is about 24.5k words
    std::lock_guard<std::recursive_mutex> lk(g_module_mu);
    static std::unordered_map<const ShaderInfo*, VkShaderModule> cache;
    auto it = cache.find(fs);
    if (it != cache.end()) return it->second;
    std::vector<uint32_t> patched;
    VkShaderModule m = extras::hdr_tonemap_variant(fs->spirv, fs->hash, patched) ? create_module(patched, fs->hash.c_str()) : shader_module(fs);
    cache[fs] = m;
    return m;
}

static VkShaderModule dual_source_module(const ShaderInfo* fs) {
    std::lock_guard<std::recursive_mutex> lk(g_module_mu);
    auto it = g_dual_src.find(fs);
    if (it != g_dual_src.end()) return it->second;
    VkShaderModule m = VK_NULL_HANDLE;
    std::vector<uint32_t> patched;
    if (spirv_dual_source_output(fs->spirv, patched)) {
        m = create_module(patched, fs->hash.c_str());
    } else {
        NXLOG_W(kTag, "%s: dual-source blending is on but the fragment shader has no second output; the unpatched module is used", fs->hash.c_str());
        m = shader_module(fs);
    }
    g_dual_src[fs] = m;
    return m;
}

// The slots `consumer` reads that `producer` does not write: the console gives them undefined values, Vulkan needs an output.
static std::vector<SpvOutputSlot> missing_outputs(const ShaderInfo* producer, const ShaderInfo* consumer) {
    std::vector<SpvOutputSlot> missing;
    if (!producer || !consumer) return missing;
    bool covered[64] = {};
    for (const ShaderIo& o : producer->refl.outputs)
        for (uint32_t e = 0; e < (o.array ? o.array : 1); ++e)
            if (o.location + e < 64) covered[o.location + e] = true;
    for (const ShaderIo& in : consumer->refl.inputs) {
        for (uint32_t e = 0; e < (in.array ? in.array : 1); ++e) {
            const uint32_t loc = in.location + e;
            if (loc >= 64 || covered[loc]) continue;
            covered[loc] = true;
            SpvOutputSlot slot;
            slot.location = loc;
            slot.type = in.type == ScalarType::Sint ? SpvScalar::Sint : in.type == ScalarType::Uint ? SpvScalar::Uint : SpvScalar::Float;
            missing.push_back(slot);
        }
    }
    return missing;
}

// The module of `producer` to use in a pipeline whose next stage is `consumer`: the translated one, or a copy with the outputs the
// consumer reads and the producer lacks. Null on a Vulkan error.
static VkShaderModule producer_module(const ShaderInfo* producer, const ShaderInfo* consumer, const std::vector<SpvIntSlot>& ints = {}) {
    const std::vector<SpvOutputSlot> missing = missing_outputs(producer, consumer);
    if (missing.empty() && ints.empty()) return shader_module(producer);
    std::vector<uint32_t> id;
    for (const SpvOutputSlot& m : missing) id.push_back(m.location * 4 + static_cast<uint32_t>(m.type));
    for (const SpvIntSlot& n : ints) id.push_back(0x80000000u | (n.location * 2 + (n.is_signed ? 1u : 0u)));
    std::lock_guard<std::recursive_mutex> lk(g_module_mu);
    auto it = g_variants.find({producer, id});
    if (it != g_variants.end()) return it->second;
    VkShaderModule m = VK_NULL_HANDLE;
    std::vector<uint32_t> base_words;
    const std::vector<uint32_t>* words = &producer->spirv;
    if (!ints.empty()) {
        // integer vertex attributes: the shader gets the raw bits the console delivers (nvn_spirv_patch.h)
        if (!spirv_int_attributes(producer->spirv, ints, base_words)) {
            NXLOG_E(kTag, "%s: the integer vertex attributes could not be patched in; draws with this shader are skipped", producer->hash.c_str());
            g_variants[{producer, id}] = VK_NULL_HANDLE;
            return VK_NULL_HANDLE;
        }
        words = &base_words;
        if (missing.empty()) {
            m = create_module(base_words, producer->hash.c_str());
            g_variants[{producer, id}] = m;
            return m;
        }
    }
    std::vector<uint32_t> patched;
    // a passthrough geometry shader: the hardware forwards the attributes of the input vertices, the translation has no code for it
    const bool pass = producer->stage == ShaderStage::Geometry && spirv_gs_passthrough(*words, missing, patched);
    if (pass || spirv_add_outputs(*words, missing, patched)) {
        m = create_module(patched, producer->hash.c_str());
        NXLOG_D(kTag, "%s: %zu outputs added for the next stage%s", producer->hash.c_str(), missing.size(), pass ? " (copied from the input vertices)" : "");
    } else {
        NXLOG_W(kTag, "%s: the %zu varyings the next stage reads could not be added; the unpatched module is used", producer->hash.c_str(), missing.size());
        m = shader_module(producer);
    }
    g_variants[{producer, id}] = m;
    return m;
}

const ProgLayout* prog_layout(const ShaderInfo* const stages[kProgStages]) {
    LayoutKey key;
    for (int i = 0; i < kProgStages; ++i) key.s[i] = stages[i];
    std::lock_guard<std::mutex> lk(g_layout_mu);
    auto it = g_layouts.find(key);
    if (it != g_layouts.end()) return &it->second;

    ProgLayout pl;
    std::map<std::pair<uint32_t, uint32_t>, size_t> index;
    for (int st = 0; st < kProgStages; ++st) {
        const ShaderInfo* s = stages[st];
        if (!s) continue;
        for (size_t i = 0; i < s->refl.bindings.size(); ++i) {
            const DescriptorBinding& b = s->refl.bindings[i];
            if (!b.used || b.set > 3) continue;
            const auto k = std::make_pair(b.set, b.binding);
            auto f = index.find(k);
            if (f == index.end()) {
                BindingDesc d;
                d.set = b.set;
                d.binding = b.binding;
                d.type = static_cast<VkDescriptorType>(b.type);
                d.stages = stage_bit(s->stage);
                d.shader = s;
                d.index = static_cast<int>(i);
                d.nvn_stage = st;
                index[k] = pl.bindings.size();
                pl.bindings.push_back(d);
            } else {
                pl.bindings[f->second].stages |= stage_bit(s->stage);   // shared binding (the support buffer)
            }
        }
    }
    std::vector<VkDescriptorSetLayoutBinding> lbs[4];
    std::string sig;                                   // the descriptors of the program, as bytes: the key of the shared layout
    for (int set = 0; set < 4; ++set) {
        for (const BindingDesc& d : pl.bindings) {
            if (d.set != static_cast<uint32_t>(set)) continue;
            VkDescriptorSetLayoutBinding b = {};
            b.binding = d.binding;
            b.descriptorType = d.type;
            b.descriptorCount = std::max<uint32_t>(1, d.shader->refl.bindings[d.index].count);
            b.stageFlags = d.stages;
            lbs[set].push_back(b);
            pl.has_set[set] = true;
            const uint32_t w[4] = {b.binding, static_cast<uint32_t>(b.descriptorType), b.descriptorCount, b.stageFlags};
            sig.append(reinterpret_cast<const char*>(w), sizeof w);
        }
        sig.append("|", 1);
    }
    // push constants of generated stages (the pass-through tessellation control shader): one range from offset 0 over those stages
    VkPushConstantRange pcr = {0, 0, 0};
    for (int st = 0; st < kProgStages; ++st)
        if (stages[st] && stages[st]->push_bytes) {
            pcr.stageFlags |= stage_bit(stages[st]->stage);
            pcr.size = std::max(pcr.size, stages[st]->push_bytes);
        }
    if (pcr.size) {
        const uint32_t w[2] = {pcr.stageFlags, pcr.size};
        sig.append("P", 1);
        sig.append(reinterpret_cast<const char*>(w), sizeof w);
    }
    auto sh = g_shared_layouts.find(sig);
    if (sh == g_shared_layouts.end()) {
        SharedLayout nl;
        for (int set = 0; set < 4; ++set) {
            VkDescriptorSetLayoutCreateInfo ci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            ci.bindingCount = static_cast<uint32_t>(lbs[set].size());
            ci.pBindings = lbs[set].data();
            if (vkCreateDescriptorSetLayout(vkdev(), &ci, nullptr, &nl.sets[set]) != VK_SUCCESS) {
                NXLOG_E(kTag, "descriptor set layout %d failed", set);
                return nullptr;
            }
        }
        VkPipelineLayoutCreateInfo pci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pci.setLayoutCount = 4;
        pci.pSetLayouts = nl.sets;
        pci.pushConstantRangeCount = pcr.size ? 1 : 0;
        pci.pPushConstantRanges = pcr.size ? &pcr : nullptr;
        if (vkCreatePipelineLayout(vkdev(), &pci, nullptr, &nl.layout) != VK_SUCCESS) {
            NXLOG_E(kTag, "pipeline layout failed");
            return nullptr;
        }
        sh = g_shared_layouts.emplace(sig, nl).first;
    }
    for (int set = 0; set < 4; ++set) pl.sets[set] = sh->second.sets[set];
    pl.layout = sh->second.layout;
    pl.push_stages = pcr.stageFlags;
    pl.push_bytes = pcr.size;
    return &g_layouts.emplace(key, std::move(pl)).first->second;
}

std::vector<SpvIntSlot> key_int_slots(const GraphicsKey& key) {
    // vertex inputs the shader declares as floats but the game feeds with integer attributes (key formats UINT / SINT)
    std::vector<SpvIntSlot> int_slots;
    if (key.stage[0] && !key.xfb)
        for (uint32_t k = 0; k < key.attr_count && k < 16; ++k) {
            const VkFormat f = static_cast<VkFormat>(key.attr[k].format);
            if (!vk_format_is_integer(f)) continue;
            for (const ShaderIo& in : key.stage[0]->refl.inputs)
                if (in.type == ScalarType::Float && key.attr[k].location >= in.location && key.attr[k].location < in.location + (in.array ? in.array : 1)) {
                    SpvIntSlot sl;
                    sl.location = key.attr[k].location;
                    sl.is_signed = vk_format_is_signed_int(f);
                    int_slots.push_back(sl);
                    break;
                }
        }
    return int_slots;
}

bool resolve_stage_modules(const GraphicsKey& key, StageModules* out) {
    *out = StageModules{};
    // the stage that feeds the fragment shader gets outputs for the varyings the fragment shader reads and it does not write
    // (geometry, else tessellation evaluation, else vertex)
    const int feeder = key.stage[2] ? 2 : key.tess[1] ? 4 : 0;
    const std::vector<SpvIntSlot> int_slots = key_int_slots(key);
    for (int i = 0; i < 3; ++i) {
        if (!key.stage[i]) continue;
        if (key.xfb && i != 0) continue;     // stream-out draws run the vertex stage only
        if (key.xfb) {
            VkShaderModule m = xfb_variant(key.stage[0]).first;
            if (!m) return false;
            out->m[0] = m;
            continue;
        }
        VkShaderModule m = (i == feeder && key.stage[1]) ? producer_module(key.stage[i], key.stage[1], i == 0 ? int_slots : std::vector<SpvIntSlot>())
                           : (i == 0 && !int_slots.empty()) ? producer_module(key.stage[i], nullptr, int_slots)
                           : (i == 1 && key.no_out)      ? no_output_module(key.stage[i])
                           : (i == 1 && key.dual_src)    ? dual_source_module(key.stage[i])
                           : (i == 1)                    ? hdr_fragment_module(key.stage[i])
                                                         : shader_module(key.stage[i]);
        if (!m) return false;
        out->m[i] = m;
    }
    // tessellation stages (never under stream-out: the replay does not build such keys)
    for (int t = 0; t < 2 && !key.xfb; ++t) {
        const ShaderInfo* s = key.tess[t];
        if (!s) continue;
        VkShaderModule m = (t == 1 && feeder == 4 && key.stage[1]) ? producer_module(s, key.stage[1], std::vector<SpvIntSlot>()) : shader_module(s);
        if (!m) return false;
        out->m[3 + t] = m;
    }
    return true;
}

bool small_dynamic_conservative() {
    static const bool on = config_get_bool("nvn", "small_dynamic", true) && device().features().conservative_raster_dynamic;
    return on;
}
bool small_dynamic_fsr() {
    static const bool on = config_get_bool("nvn", "small_dynamic", true) && device().features().fragment_shading_rate;
    return on;
}

// The dynamic states of every graphics pipeline. The NVN2 smaller features add two groups when the device has them (nvn_small.h): the
// conservative raster mode / overestimation size (VK_EXT_conservative_rasterization with extended dynamic state 3) and the fragment shading
// rate (VK_KHR_fragment_shading_rate). Once a pipeline lists them, the replay sets them before the first draw of every pass
// (set_dynamic_state). [nvn] small_dynamic = false leaves both out.
const VkPipelineDynamicStateCreateInfo* pipeline_dynamic_state() {
    static const struct Dyn {
        VkDynamicState s[32];
        VkPipelineDynamicStateCreateInfo ci;
        Dyn() {
            const VkDynamicState base[] = {
                VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT, VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT, VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE,
                VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
                VK_DYNAMIC_STATE_DEPTH_COMPARE_OP, VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE,
                VK_DYNAMIC_STATE_STENCIL_OP, VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE, VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE,
                VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE, VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE, VK_DYNAMIC_STATE_DEPTH_BIAS,
                VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_DEPTH_BOUNDS, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
                VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_LINE_WIDTH};
            uint32_t n = 0;
            for (VkDynamicState d : base) s[n++] = d;
            if (small_dynamic_conservative()) {
                s[n++] = VK_DYNAMIC_STATE_CONSERVATIVE_RASTERIZATION_MODE_EXT;
                s[n++] = VK_DYNAMIC_STATE_EXTRA_PRIMITIVE_OVERESTIMATION_SIZE_EXT;
            }
            if (small_dynamic_fsr()) s[n++] = VK_DYNAMIC_STATE_FRAGMENT_SHADING_RATE_KHR;
            ci = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, nullptr, 0, n, s};
        }
    } dyn;
    return &dyn.ci;
}

VkPipelineCache pipeline_cache_handle() {
    ensure_cache();
    return g_cache;
}

void note_pipeline_created(double ms, const char* what) {
    counters().pipelines_created.fetch_add(1);
    g_pipes_created.add(1);
    g_cache_dirty.store(true, std::memory_order_relaxed);
    if (ms >= 2.0) g_new_since_save.fetch_add(1, std::memory_order_relaxed);      // a part the cache already had comes back in well under that
    NXLOG_D(kTag, "%s created in %.1f ms", what, ms);
}

// The dynamic states of a mesh pipeline: the graphics list without the vertex input and input assembly ones (VK_EXT_mesh_shader forbids them).
const VkPipelineDynamicStateCreateInfo* mesh_dynamic_state() {
    static const struct Dyn {
        std::vector<VkDynamicState> s;
        VkPipelineDynamicStateCreateInfo ci;
        Dyn() {
            const VkPipelineDynamicStateCreateInfo* g = pipeline_dynamic_state();
            for (uint32_t i = 0; i < g->dynamicStateCount; ++i) {
                const VkDynamicState d = g->pDynamicStates[i];
                if (d == VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY || d == VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE || d == VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE) continue;
                s.push_back(d);
            }
            ci = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, nullptr, 0, static_cast<uint32_t>(s.size()), s.data()};
        }
    } dyn;
    return &dyn.ci;
}

VkPipeline build_graphics_pipeline(const GraphicsKey& key, const ProgLayout& layout) {
    ensure_cache();
    const auto t0 = std::chrono::steady_clock::now();
    const bool mesh = key.mesh[1] != nullptr;

    VkPipelineShaderStageCreateInfo stages[5] = {};
    uint32_t ns = 0;
    const VkShaderStageFlagBits bits[5] = {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT, VK_SHADER_STAGE_GEOMETRY_BIT,
                                           VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT, VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT};
    StageModules sm;
    if (!resolve_stage_modules(key, &sm)) return VK_NULL_HANDLE;
    for (int i = 0; i < 5; ++i) {
        if (!sm.m[i]) continue;
        stages[ns] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stages[ns].stage = bits[i];
        stages[ns].module = sm.m[i];
        stages[ns].pName = "main";
        ++ns;
    }
    for (int i = 0; mesh && i < 2; ++i) {   // task, mesh
        if (!key.mesh[i]) continue;
        VkShaderModule m = shader_module(key.mesh[i]);
        if (!m) return VK_NULL_HANDLE;
        stages[ns] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stages[ns].stage = i == 0 ? VK_SHADER_STAGE_TASK_BIT_EXT : VK_SHADER_STAGE_MESH_BIT_EXT;
        stages[ns].module = m;
        stages[ns].pName = "main";
        ++ns;
    }

    // vertex input
    VkVertexInputBindingDescription vb[16];
    VkVertexInputAttributeDescription va[16];
    uint32_t nvb = 0, nva = 0;
    bool binding_seen[16] = {};
    for (uint32_t i = 0; i < key.attr_count && i < 16; ++i) {
        const KeyAttr& a = key.attr[i];
        va[nva++] = {a.location, a.binding, static_cast<VkFormat>(a.format), a.offset};
        if (!binding_seen[a.binding]) {
            binding_seen[a.binding] = true;
            vb[nvb++] = {a.binding, 0, key.binding_instance[a.binding] ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX};
        }
    }
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = nvb;
    vi.pVertexBindingDescriptions = vb;
    vi.vertexAttributeDescriptionCount = nva;
    vi.pVertexAttributeDescriptions = va;

    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = static_cast<VkPrimitiveTopology>(key.topology);
    VkPipelineTessellationStateCreateInfo ts = {VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO};
    ts.patchControlPoints = key.patch_control_points;

    VkPipelineViewportDepthClipControlCreateInfoEXT dcc = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT};
    dcc.negativeOneToOne = key.depth_neg_one ? VK_TRUE : VK_FALSE;
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    if (device().features().depth_clip_control) vp.pNext = &dcc;   // viewport and scissor counts are dynamic

    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.depthClampEnable = key.depth_clamp ? VK_TRUE : VK_FALSE;
    rs.polygonMode = static_cast<VkPolygonMode>(key.polygon_mode);
    rs.lineWidth = 1.0f;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = static_cast<VkSampleCountFlagBits>(key.samples ? key.samples : 1);
    ms.alphaToCoverageEnable = key.alpha_to_coverage ? VK_TRUE : VK_FALSE;

    VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.maxDepthBounds = 1.0f;

    VkPipelineColorBlendAttachmentState cba[8];
    for (uint32_t i = 0; i < key.color_count && i < 8; ++i) {
        cba[i] = {};
        cba[i].blendEnable = key.blend_enable[i] ? VK_TRUE : VK_FALSE;
        cba[i].srcColorBlendFactor = static_cast<VkBlendFactor>(key.blend_src_rgb[i]);
        cba[i].dstColorBlendFactor = static_cast<VkBlendFactor>(key.blend_dst_rgb[i]);
        cba[i].colorBlendOp = static_cast<VkBlendOp>(key.blend_op_rgb[i]);
        cba[i].srcAlphaBlendFactor = static_cast<VkBlendFactor>(key.blend_src_a[i]);
        cba[i].dstAlphaBlendFactor = static_cast<VkBlendFactor>(key.blend_dst_a[i]);
        cba[i].alphaBlendOp = static_cast<VkBlendOp>(key.blend_op_a[i]);
        cba[i].colorWriteMask = key.write_mask[i];
    }
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.logicOpEnable = key.logic_op_enable ? VK_TRUE : VK_FALSE;
    cb.logicOp = static_cast<VkLogicOp>(key.logic_op);
    cb.attachmentCount = key.color_count;
    cb.pAttachments = cba;

    VkFormat cf[8];
    for (uint32_t i = 0; i < key.color_count && i < 8; ++i) cf[i] = static_cast<VkFormat>(key.color_format[i]);
    VkPipelineRenderingCreateInfo ri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ri.colorAttachmentCount = key.color_count;
    ri.pColorAttachmentFormats = cf;
    ri.depthAttachmentFormat = static_cast<VkFormat>(key.depth_format);
    ri.stencilAttachmentFormat = static_cast<VkFormat>(key.stencil_format);

    VkGraphicsPipelineCreateInfo gi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &ri};
    gi.stageCount = ns;
    gi.pStages = stages;
    gi.pVertexInputState = mesh ? nullptr : &vi;
    gi.pInputAssemblyState = mesh ? nullptr : &ia;
    gi.pTessellationState = key.patch_control_points && !mesh ? &ts : nullptr;
    gi.pViewportState = &vp;
    gi.pRasterizationState = &rs;
    gi.pMultisampleState = &ms;
    gi.pDepthStencilState = &ds;
    gi.pColorBlendState = &cb;
    gi.pDynamicState = mesh ? mesh_dynamic_state() : pipeline_dynamic_state();
    gi.layout = layout.layout;
    VkPipeline pipe = VK_NULL_HANDLE;
    const VkResult r = create_graphics_pipeline_tracked(vkdev(), g_cache, &gi, &pipe);
    const double ms_taken = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (r != VK_SUCCESS) {
        NXLOG_E(kTag, "graphics pipeline failed (%d): %s %s fs %s, %u colour targets", static_cast<int>(r), mesh ? "ms" : "vs",
                mesh ? key.mesh[1]->hash.c_str() : key.stage[0] ? key.stage[0]->hash.c_str() : "-", key.stage[1] ? key.stage[1]->hash.c_str() : "-", key.color_count);
        pipe = VK_NULL_HANDLE;
    } else {
        counters().pipelines_created.fetch_add(1);
        g_cache_dirty.store(true, std::memory_order_relaxed);
    g_new_since_save.fetch_add(1, std::memory_order_relaxed);
        g_pipes_created.add(1);
        NXLOG_D(kTag, "graphics pipeline created in %.1f ms (vs %s fs %s)", ms_taken, key.stage[0] ? key.stage[0]->hash.c_str() : "-",
                key.stage[1] ? key.stage[1]->hash.c_str() : "-");
    }
    return pipe;
}

VkPipeline graphics_pipeline(const GraphicsKey& key, const ProgLayout& layout) {
    ensure_cache();
    const uint64_t h = key.hash();
    auto& bucket = g_gfx[h];
    for (auto& e : bucket)
        if (e.first == key) {
            counters().pipeline_cache_hits.fetch_add(1, std::memory_order_relaxed);
            if (e.up && e.up->state.load(std::memory_order_acquire) != 0) gpl_take_upgrade(e.second, e.alt, e.up);
            if (e.alt && gpl_ab_whole()) return e.alt;
            return e.second;
        }
    prewarm_init();
    VkPipeline pipe = VK_NULL_HANDLE;
    std::shared_ptr<PipeUpgrade> later;          // the key file's whole pipeline, still being built: it replaces the linked one
    // compiled ahead on a worker; one being compiled is linked from its parts meanwhile when they are built (else waited for)
    if (key.mesh[1]) {   // mesh pipelines: built whole at the first draw (no pipeline library parts, not in the prewarm key file)
        NXFT("pipe.create");
        pipe = build_graphics_pipeline(key, layout);
        if (pipe) { ++g_pipeline_count; g_built_at_draw.fetch_add(1, std::memory_order_relaxed); }
        g_gfx_keys.add(1);
        bucket.push_back({key, pipe, VK_NULL_HANDLE, nullptr});
        return pipe;
    }
    if (prewarm_take(key, &pipe, [&] { return gpl_ready(key, layout); }, &later)) {
        ++g_pipeline_count;
        g_gfx_keys.add(1);
        bucket.push_back({key, pipe, VK_NULL_HANDLE, nullptr});
        return pipe;
    }
    std::shared_ptr<PipeUpgrade> up;
    bool pending = false;
    pipe = gpl_link(key, layout, later ? nullptr : &up, &pending); // linked from shader parts compiled ahead (null: not eligible or a part failed)
    if (later) up = later;
    prewarm_record(key);                         // every key a draw asked for: the next run builds it from boot
    if (pending && !later) return VK_NULL_HANDLE;   // its parts are compiling in the background: the draw is skipped, the next one asks again
    g_gfx_keys.add(1);                           // every path below adds one entry to the bucket
    if (pipe) {
        ++g_pipeline_count;
        bucket.push_back({key, pipe, VK_NULL_HANDLE, up});
        return pipe;
    }
    if (pending) {                               // (rare) parts gone missing between the check and the link: skipped until the whole pipeline is in
        bucket.push_back({key, VK_NULL_HANDLE, VK_NULL_HANDLE, up});
        return VK_NULL_HANDLE;
    }
    NXFT("pipe.create");
    pipe = build_graphics_pipeline(key, layout);
    if (pipe) {
        ++g_pipeline_count;
        g_built_at_draw.fetch_add(1, std::memory_order_relaxed);
    }
    bucket.push_back({key, pipe, VK_NULL_HANDLE, up});   // up: the key file's build, when one is under way, replaces this one (same pipeline)
    return pipe;
}

namespace {
// One vkCreateComputePipelines call, timed; VK_NULL_HANDLE on failure (logged).
VkPipeline create_compute(const ShaderInfo* cs, VkShaderModule m, VkPipelineLayout layout) {
    VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = m;
    ci.stage.pName = "main";
    ci.layout = layout;
    VkPipeline pipe = VK_NULL_HANDLE;
    const uint64_t tp0 = nxrt::hitch_ticks();
    const auto t0 = std::chrono::steady_clock::now();
    const VkResult cr = vkCreateComputePipelines(vkdev(), g_cache, 1, &ci, nullptr, &pipe);
    if (nxrt::hitch_on_render_thread()) nxrt::hitch_phase_add(nxrt::kHpRenderPipeline, nxrt::hitch_ticks() - tp0);
    const double ms = ms_from(t0);
    if (ms >= 250.0)
        NXLOG_W(kTag, "compute pipeline %s took %.0f ms to create (%zu bytes of SPIR-V)", cs->hash.c_str(), ms, cs->spirv.size() * 4);
    if (cr != VK_SUCCESS) {
        NXLOG_E(kTag, "compute pipeline failed for %s", cs->hash.c_str());
        return VK_NULL_HANDLE;
    }
    counters().pipelines_created.fetch_add(1);
    ++g_pipeline_count;
    g_pipes_created.add(1);
    g_cache_dirty.store(true, std::memory_order_relaxed);
    g_new_since_save.fetch_add(1, std::memory_order_relaxed);
    return pipe;
}

// Compute programs with this much SPIR-V are built on a thread of their own, and their dispatches are left out until the pipeline is there.
// OoT's tensor-core upscaler programs (fea98fe7, f90d726d: 4-5 MB of SPIR-V each) took 116 s in the NVIDIA driver the first time, all of it
// on the render thread (no frame for two minutes). The pipeline cache keeps them afterwards. [nvn] compute_background_spirv_kb (0 = never).
struct ComputeBuild {
    std::thread thread;
    std::atomic<bool> done{false};
    VkPipeline pipe = VK_NULL_HANDLE;
};
std::map<std::pair<const ShaderInfo*, VkPipelineLayout>, std::unique_ptr<ComputeBuild>> g_compute_builds;
}  // namespace

VkPipeline compute_pipeline(const ShaderInfo* cs, const ProgLayout& layout) {
    ensure_cache();
    const auto k = std::make_pair(cs, layout.layout);
    auto it = g_compute.find(k);
    if (it != g_compute.end()) return it->second;
    if (auto b = g_compute_builds.find(k); b != g_compute_builds.end()) {   // built in the background: taken once done
        if (!b->second->done.load(std::memory_order_acquire)) return VK_NULL_HANDLE;
        b->second->thread.join();
        const VkPipeline pipe = b->second->pipe;
        g_compute_builds.erase(b);
        g_compute[k] = pipe;
        g_compute_keys.add(1);
        NXLOG_I(kTag, "compute pipeline %s: background build done, its dispatches run from now on", cs->hash.c_str());
        return pipe;
    }
    VkShaderModule m = shader_module(cs);
    VkPipeline pipe = VK_NULL_HANDLE;
    static const uint64_t background_kb = static_cast<uint64_t>(std::max<int64_t>(0, config_get_int("nvn", "compute_background_spirv_kb", 1024)));
    if (m && background_kb && cs->spirv.size() * 4 >= background_kb * 1024) {
        auto b = std::make_unique<ComputeBuild>();
        ComputeBuild* bp = b.get();
        const VkPipelineLayout pl = layout.layout;
        NXLOG_I(kTag, "compute pipeline %s (%zu bytes of SPIR-V): built in the background; its dispatches are left out until it is ready",
                cs->hash.c_str(), cs->spirv.size() * 4);
        bp->thread = std::thread([bp, cs, m, pl] {
            bp->pipe = create_compute(cs, m, pl);
            bp->done.store(true, std::memory_order_release);
        });
        g_compute_builds.emplace(k, std::move(b));
        return VK_NULL_HANDLE;
    }
    if (m) pipe = create_compute(cs, m, layout.layout);
    g_compute[k] = pipe;                         // new key: the lookup above missed
    g_compute_keys.add(1);
    return pipe;
}

VkBuffer constant_data_buffer(const ShaderInfo* s, VkDeviceSize* range) {
    auto it = g_const_bufs.find(s);
    if (it == g_const_bufs.end()) {
        const VkDeviceSize size = std::max<VkDeviceSize>(256, (s->constants.size() + 15) & ~15ull);
        gfx::Buffer b;
        if (device().create_buffer(size, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, gfx::MemoryUse::Upload, &b, "nvn c1") != VK_SUCCESS) {
            *range = 0;
            return VK_NULL_HANDLE;
        }
        std::memset(b.mapped, 0, size);
        if (!s->constants.empty()) std::memcpy(b.mapped, s->constants.data(), s->constants.size());
        it = g_const_bufs.emplace(s, std::make_pair(b, size)).first;
        g_const_n.add(1);
        g_const_bytes.add(static_cast<int64_t>(size));
    }
    *range = it->second.second;
    return it->second.first.buffer;
}

VkBuffer driver_constants_buffer(VkDeviceSize* range) {
    if (!g_driver_const_made) {
        g_driver_const_made = true;
        if (device().create_buffer(65536, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, gfx::MemoryUse::Upload, &g_driver_const, "nvn c0") == VK_SUCCESS) {
            std::memset(g_driver_const.mapped, 0, 65536);
            // c[0]+0x610: maximum point size read by the agl vertex shaders (gl_PointSize = min(size, c0[0x610]))
            const float max_point = 64.0f;
            std::memcpy(static_cast<uint8_t*>(g_driver_const.mapped) + 0x610, &max_point, 4);
        }
    }
    *range = 65536;
    return g_driver_const.buffer;
}

size_t pipeline_count() { return static_cast<size_t>(g_pipeline_count.get()); }

void module_cache_clear() {
    VkDevice d = vkdev();
    std::lock_guard<std::recursive_mutex> lk(g_module_mu);
    for (auto& [k, m] : g_modules) if (m) vkDestroyShaderModule(d, m, nullptr);
    g_modules.clear();
    for (auto& [k, m] : g_variants) if (m) vkDestroyShaderModule(d, m, nullptr);
    g_variants.clear();
    for (auto& [k, m] : g_dual_src) if (m) vkDestroyShaderModule(d, m, nullptr);
    g_dual_src.clear();
    for (auto& [k, e] : g_no_out) if (e.first && e.second) vkDestroyShaderModule(d, e.first, nullptr);
    g_no_out.clear();
    for (auto& [k, v] : g_xfb_mod) if (v.first) vkDestroyShaderModule(d, v.first, nullptr);
    g_xfb_mod.clear();
    std::lock_guard<std::mutex> lk2(g_module_id_mu);
    g_module_ids.clear();
}

uint64_t module_content_id(VkShaderModule m) {
    std::lock_guard<std::mutex> lk(g_module_id_mu);
    auto it = g_module_ids.find(m);
    return it != g_module_ids.end() ? it->second : reinterpret_cast<uint64_t>(m);   // not made by create_module: the handle stands for itself
}

uint64_t pipelines_built_at_draw() { return g_built_at_draw.load(std::memory_order_relaxed); }

void pipeline_shutdown() {
    VkDevice d = vkdev();
    prewarm_shutdown();
    gpl_stop();
    for (auto& [k, b] : g_compute_builds) {      // a background compute build cannot be cancelled: wait for it before the device goes
        if (b->thread.joinable()) b->thread.join();
        if (b->pipe) vkDestroyPipeline(d, b->pipe, nullptr);
    }
    g_compute_builds.clear();
    pipeline_cache_flush();
    for (auto& [h, bucket] : g_gfx)
        for (auto& e : bucket) {
            if (e.second) vkDestroyPipeline(d, e.second, nullptr);
            if (e.alt) vkDestroyPipeline(d, e.alt, nullptr);
            if (e.up && e.up->state.load() == 1 && e.up->pipe) vkDestroyPipeline(d, e.up->pipe, nullptr);     // built and never taken
        }
    g_gfx.clear();
    g_gfx_keys.sub(g_gfx_keys.get());
    gpl_shutdown();                              // the parts the pipelines were linked from
    for (auto& [k, p] : g_compute)
        if (p) vkDestroyPipeline(d, p, nullptr);
    g_compute.clear();
    g_compute_keys.sub(g_compute_keys.get());
    g_layouts.clear();
    for (auto& [k, l] : g_shared_layouts) {          // the handles in g_layouts were copies of these
        if (l.layout) vkDestroyPipelineLayout(d, l.layout, nullptr);
        for (VkDescriptorSetLayout s : l.sets)
            if (s) vkDestroyDescriptorSetLayout(d, s, nullptr);
    }
    g_shared_layouts.clear();
    for (auto& [k, m] : g_modules)
        if (m) vkDestroyShaderModule(d, m, nullptr);
    g_modules.clear();
    for (auto& [k, m] : g_variants)
        if (m) vkDestroyShaderModule(d, m, nullptr);
    g_variants.clear();
    for (auto& [k, m] : g_dual_src)
        if (m) vkDestroyShaderModule(d, m, nullptr);
    g_dual_src.clear();
    for (auto& [k, e] : g_no_out)
        if (e.first && e.second) vkDestroyShaderModule(d, e.first, nullptr);
    g_no_out.clear();
    for (auto& [k, v] : g_xfb_mod)
        if (v.first) vkDestroyShaderModule(d, v.first, nullptr);
    g_xfb_mod.clear();
    for (auto& [k, b] : g_const_bufs) device().destroy_buffer(b.first);
    g_const_bufs.clear();
    g_const_n.sub(g_const_n.get());
    g_const_bytes.sub(g_const_bytes.get());
    if (g_driver_const_made) device().destroy_buffer(g_driver_const);
    g_driver_const_made = false;
    if (g_cache) vkDestroyPipelineCache(d, g_cache, nullptr);
    g_cache = VK_NULL_HANDLE;
    g_cache_tried = false;
    g_cache_ready.store(false);
    g_cache_dirty.store(false);
    g_cache_big = false;
    g_cache_base = 0;
    g_cache_unreadable = false;
    g_pipeline_count.sub(g_pipeline_count.get());
}

// Gauges (nxrt/gauge.h). The callbacks read the layout and module maps under the locks that guard them; they are removed before those maps
// are destroyed.
namespace {
struct PipelineGauges {
    PipelineGauges() {
        nxrt::gauge_register("nvn.pipeline_layouts", [] {
            std::lock_guard<std::mutex> lk(g_layout_mu);
            return static_cast<int64_t>(g_layouts.size());
        });
        nxrt::gauge_register("nvn.shared_layouts", [] {
            std::lock_guard<std::mutex> lk(g_layout_mu);
            return static_cast<int64_t>(g_shared_layouts.size());
        });
        nxrt::gauge_register("nvn.shader_modules", [] {
            std::lock_guard<std::recursive_mutex> lk(g_module_mu);
            return static_cast<int64_t>(g_modules.size());
        });
        nxrt::gauge_register("nvn.shader_modules.variants", [] {
            std::lock_guard<std::recursive_mutex> lk(g_module_mu);
            return static_cast<int64_t>(g_variants.size());
        });
        nxrt::gauge_register("nvn.shader_modules.dual_src", [] {
            std::lock_guard<std::recursive_mutex> lk(g_module_mu);
            return static_cast<int64_t>(g_dual_src.size());
        });
        nxrt::gauge_register("nvn.shader_modules.no_out", [] {
            std::lock_guard<std::recursive_mutex> lk(g_module_mu);
            return static_cast<int64_t>(g_no_out.size());
        });
        nxrt::gauge_register("nvn.shader_modules.xfb", [] {
            std::lock_guard<std::recursive_mutex> lk(g_module_mu);
            return static_cast<int64_t>(g_xfb_mod.size());
        });
    }
    ~PipelineGauges() {
        for (const char* n : {"nvn.pipeline_layouts", "nvn.shared_layouts", "nvn.shader_modules", "nvn.shader_modules.variants",
                              "nvn.shader_modules.dual_src", "nvn.shader_modules.no_out", "nvn.shader_modules.xfb"})
            nxrt::gauge_unregister(n);
    }
};
PipelineGauges g_pipeline_gauges;
}  // namespace

}  // namespace nxrt::nvn::be::gpu
