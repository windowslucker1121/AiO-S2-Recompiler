// SPDX-License-Identifier: GPL-3.0-or-later
// [nvn] hazard (default off: see the 2026-11-10 measurements in AGENTS.md): range-based hazard tracking for the render thread's command buffers.
//
// The replay used to put a full memory barrier (all commands, all memory) after every dispatch, around every copy, fill and memory write,
// at the end of passes that wrote storage, and for every game barrier: ~900 per frame in the village at 1920x1080, ~3.85 ms of GPU time
// (K1: dropping all of them, as a measurement). Most of them order nothing: the commands on either side touch different memory. Here the
// barrier calls of the replay (memory_barrier) are only counted, and a barrier is recorded where a command reads memory or an image that a
// command wrote since the last barrier, or writes what one read or wrote since then. What each command touches comes from the Vulkan calls
// themselves (hooks in front of the command queue's, nvn_cmdq.cpp), so every site of the replay, the pools and the textures is covered the
// same way:
//   - buffers by range, in one address space per memory: buffers over imported host memory (the game's pools, imported in overlapping
//     chunks) by host address, every other buffer by itself; descriptor sets by what their last vkUpdateDescriptorSets wrote; vertex,
//     index, indirect and count buffers, copies, fills, updates and query result copies by their arguments;
//   - a storage buffer or storage image counts as written when the bound pipeline stores to storage anywhere (its SPIR-V, scanned at
//     vkCreateShaderModule), else as read;
//   - a pipeline that loads through PhysicalStorageBuffer pointers (A17 pointer shaders) reads every buffer that has a device address (and
//     every buffer the hooks never saw made), one that stores through them writes all of those;
//   - images: attachments (written unless their layout is read-only), storage images, sampled images in the GENERAL layout, and the images of
//     copies, blits, resolves and clears. A sampled image in another layout needs no tracking: an image written as an attachment or by a
//     transfer reaches a read-only layout only through a layout transition, and that image barrier orders the write;
//   - image barriers (layout transitions) end the tracking of their image; with [nvn] hazard_piggyback (default on) a full memory barrier is
//     added to such a call whenever anything is unordered, so the transition and the barrier cost one call;
//   - the tracking runs on the recorder thread (nvn_cmdq.cpp: these hooks are behind the command queue's), so it costs the render thread
//     nothing. A render pass cannot hold a barrier, and what its draws touch is known only from its commands: at vkCmdBeginRendering the
//     recorder looks ahead through the queued commands of the pass (cmdq::scan_pass, tracked without driver calls) up to its
//     vkCmdEndRendering; the first one that conflicts with what was unordered before the pass makes it a barrier before the pass, else none.
//     Then the pass is recorded. When the render thread needs the recorder meanwhile (a drain, a full ring), the barrier is made;
//   - at the end of a command buffer, unordered writes get a barrier that includes the host (the CPU reads results after the fence). Reads
//     stay tracked into the next command buffer of the render thread; when anything else was submitted in between, the next command buffer
//     starts with everything unknown (its first access gets a barrier).
// Commands the hooks do not understand get a barrier before them when anything is unordered and leave everything unknown.
// [nvn] hazard = off: every barrier as before. verify: every requested barrier is recorded as before, and the tracker runs beside it and
// logs where it finds a hazard that no requested barrier ordered since the conflicting access (a gap in the old barriers, or one too many
// in the model).
#include "nvn_hazard.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "nvn_cmdq.h"
#include "nvn_gpu_internal.h"
#include "nxrt/config.h"
#include "nxrt/log.h"

namespace nxrt::nvn::be::gpu {
void count_tracked_barrier();   // nvn_gpu.cpp: the full-barrier gauge
}

namespace nxrt::nvn::be::hazard {
namespace {

constexpr const char* kTag = "nvn.hazard";
inline uint64_t H(const void* p) { return reinterpret_cast<uint64_t>(p); }

int mode() {   // 0 off, 1 on, 2 verify
    static const int m = [] {
        const std::string v = config_get_str("nvn", "hazard", "off");
        return v == "off" || v == "0" || v == "false" ? 0 : v == "verify" ? 2 : 1;
    }();
    return m;
}
bool g_piggy_ab = true;   // cpuperf_ab_what hazard_piggyback
// The recorder thread's mode while it looks ahead through a render pass (cmdq::scan_pass): 0 normal (track, then the driver call), 1 dry (track,
// no driver call: the pass's commands are looked at before its barrier is decided), 2 pass-through (the driver call only: those commands were
// tracked in the dry run).
thread_local int tl_mode = 0;
inline bool dry() { return tl_mode == 1; }
bool piggyback_on() {
    static const bool on = config_get_bool("nvn", "hazard_piggyback", true);
    return on && g_piggy_ab;
}

// ---- what objects touch (any thread) -------------------------------------------------------------------------------------------
enum : uint32_t { kFBdaRead = 1, kFBdaWrite = 2, kFStore = 4, kFUnknown = 8 };
constexpr uint64_t kHostSpace = 1;   // buffers over imported host memory: addresses are host addresses
constexpr uint64_t kEnd = ~0ull;

struct Rng { uint64_t space = 0, lo = 0, hi = 0; bool bda = true; };
struct BufInfo { uint64_t space = 0, base = 0, size = kEnd; bool bda = true; };
struct BufView { VkBuffer buf; VkDeviceSize off, range; };
struct Res { uint8_t kind = 0; bool storage = false; Rng r; uint64_t img = 0; };   // kind 0 buffer, 1 image, 2 unknown (anything)
struct Sum {
    uint64_t ver = 0;
    std::vector<std::pair<uint32_t, Res>> e;   // key: binding << 16 | array element
    uint64_t seen_ep = 0, seen_ver = 0;   // registered in barrier interval seen_ep at version seen_ver (render thread)
    uint64_t seen_wm = 0;                 // ... with these bindings' storage descriptors as written (~0: all)
    uint64_t gb = 0;                      // granule bloom of its buffer ranges (gbits)
    bool has_img = false, has_bda = false, has_unknown = false;
};
// A 64-bit bloom of the 2 MiB granules a range touches (all bits for a range over 8 granules).
inline uint64_t gbits(uint64_t space, uint64_t lo, uint64_t hi) {
    const uint64_t g0 = lo >> 21, g1 = (hi - 1) >> 21;
    if (g1 - g0 >= 8) return ~0ull;
    uint64_t b = 0;
    for (uint64_t g = g0; g <= g1; ++g) b |= uint64_t{1} << (((space ^ (g * 0x9e3779b97f4a7c15ull)) * 0xff51afd7ed558ccdull) >> 58);
    return b;
}
void sum_refresh(Sum& s) {
    s.gb = 0;
    s.has_img = s.has_bda = s.has_unknown = false;
    for (const auto& [k, x] : s.e) {
        if (x.kind == 0) { s.gb |= gbits(x.r.space, x.r.lo, x.r.hi); s.has_bda |= x.r.bda; }
        else if (x.kind == 1) s.has_img = true;
        else s.has_unknown = true;
    }
}
// What a module or pipeline writes: flags, and per descriptor set the bindings its stores reach (bit b: binding b; kFStore in flags:
// a store whose target is not known, so every storage descriptor counts as written).
constexpr int kSets = 8;
struct PInfo { uint32_t f = 0; uint64_t wm[kSets] = {}; };
PInfo unknown_info() { PInfo i; i.f = 8; return i; }

std::mutex g_mu;     // buffers, memory, views, descriptor set summaries
std::mutex g_pmu;    // modules and pipelines
std::unordered_map<uint64_t, PInfo> g_mod, g_pipe;
std::unordered_map<uint64_t, BufInfo> g_buf;
std::unordered_map<uint64_t, uint64_t> g_hostmem, g_view;
std::unordered_map<uint64_t, BufView> g_bview;
std::unordered_map<uint64_t, Sum> g_sums;
uint64_t g_ver = 0;
uint64_t g_mods_n = 0, g_mods_f[4] = {};   // statistics: modules scanned, with each flag

// SPIR-V: which storage bindings does a module store to (or write as images), does it load or store through PhysicalStorageBuffer pointers?
PInfo scan_spirv(const uint32_t* w, size_t n) {
    PInfo info;
    if (n < 5 || w[0] != 0x07230203u) return unknown_info();
    const uint32_t bound = w[3];
    if (bound > (1u << 24)) return unknown_info();
    std::vector<uint32_t> cls(bound, ~0u), ty(bound, 0), base(bound, ~0u), dset(bound, ~0u), dbind(bound, ~0u);
    uint32_t& f = info.f;
    for (size_t i = 5; i < n;) {
        const uint32_t op = w[i] & 0xffff, wc = w[i] >> 16;
        if (!wc || i + wc > n) return unknown_info();
        switch (op) {
            case 32: if (wc >= 3 && w[i + 1] < bound) cls[w[i + 1]] = w[i + 2]; break;        // OpTypePointer
            case 39: if (wc >= 3 && w[i + 1] < bound) cls[w[i + 1]] = w[i + 2]; break;        // OpTypeForwardPointer
            case 71:                                                                           // OpDecorate DescriptorSet / Binding
                if (wc >= 4 && w[i + 1] < bound) { if (w[i + 2] == 34) dset[w[i + 1]] = w[i + 3]; else if (w[i + 2] == 33) dbind[w[i + 1]] = w[i + 3]; }
                break;
            case 1: case 55: case 57: case 59: case 61: case 65: case 66: case 67: case 70: case 81: case 83: case 120: case 124: case 169: case 245:
                if (wc >= 3 && w[i + 2] < bound) ty[w[i + 2]] = w[i + 1];
                // the variable a pointer (or a loaded image) comes from: variables, access chains, copies, loads
                if (op == 59 && w[i + 2] < bound) base[w[i + 2]] = w[i + 2];
                else if ((op == 65 || op == 66 || op == 67 || op == 70 || op == 83 || op == 124 || op == 61) && wc >= 4 && w[i + 2] < bound && w[i + 3] < bound)
                    base[w[i + 2]] = base[w[i + 3]];
                break;
            default: break;
        }
        i += wc;
    }
    // A store through `id`: the binding of its variable, else (no known variable) every storage binding.
    auto write_of = [&](uint32_t id) {
        const uint32_t v = id < bound ? base[id] : ~0u;
        if (v < bound && dset[v] < static_cast<uint32_t>(kSets) && dbind[v] < 64) info.wm[dset[v]] |= uint64_t{1} << dbind[v];
        else if (v < bound && dset[v] < static_cast<uint32_t>(kSets) && dbind[v] != ~0u) info.wm[dset[v]] = ~0ull;
        else f |= kFStore;
    };
    auto klass = [&](uint32_t id) -> uint32_t {
        if (id >= bound) return ~0u;
        const uint32_t t = ty[id];
        return t < bound ? cls[t] : ~0u;
    };
    auto store_to = [&](uint32_t ptr) {
        const uint32_t c = klass(ptr);
        if (c == 12 || c == 2 || c == 11) write_of(ptr);          // StorageBuffer, Uniform (BufferBlock), Image (texel pointer)
        else if (c == 5349) f |= kFBdaWrite | kFBdaRead;          // PhysicalStorageBuffer
        else if (c == ~0u) f |= kFStore;                          // a pointer of unknown origin
    };
    for (size_t i = 5; i < n;) {
        const uint32_t op = w[i] & 0xffff, wc = w[i] >> 16;
        switch (op) {
            case 62: case 228: case 63: case 64: if (wc >= 2) store_to(w[i + 1]); break;   // OpStore, OpAtomicStore, OpCopyMemory(Sized)
            case 229: case 230: case 231: case 232: case 233: case 234: case 235: case 236: case 237: case 238: case 239: case 240: case 241: case 242:
            case 6035: case 5614: case 5615:
                if (wc >= 4) store_to(w[i + 3]);
                break;
            case 99: if (wc >= 2) write_of(w[i + 1]); break;                                // OpImageWrite: the loaded image's variable
            case 60: if (wc >= 4) write_of(w[i + 3]); break;                                // OpImageTexelPointer (image atomics)
            case 61: if (wc >= 4 && klass(w[i + 3]) == 5349) f |= kFBdaRead; break;     // OpLoad through a pointer
            case 120: if (wc >= 3 && w[i + 1] < bound && cls[w[i + 1]] == 5349) f |= kFBdaRead; break;   // OpConvertUToPtr
            default: break;
        }
        i += wc;
    }
    return info;
}

void or_into(PInfo& a, const PInfo& b) {
    a.f |= b.f;
    for (int k = 0; k < kSets; ++k) a.wm[k] |= b.wm[k];
}
PInfo module_info_locked(VkShaderModule m) {
    const auto it = g_mod.find(H(m));
    return it == g_mod.end() ? unknown_info() : it->second;
}

BufInfo buf_info_locked(VkBuffer b) {
    const auto it = g_buf.find(H(b));
    if (it != g_buf.end()) return it->second;
    BufInfo i;
    i.space = H(b);
    return i;
}
Rng buf_rng_locked(VkBuffer b, VkDeviceSize off, VkDeviceSize size) {
    const BufInfo i = buf_info_locked(b);
    Rng r;
    r.space = i.space;
    r.bda = i.bda;
    r.lo = i.base + off;
    if (size == VK_WHOLE_SIZE || size == kEnd) r.hi = i.size == kEnd ? kEnd : i.base + i.size;
    else r.hi = r.lo + size;
    if (r.hi <= r.lo) r.hi = r.lo + 1;
    return r;
}

// ---- create hooks ------------------------------------------------------------------------------------------------------------
PFN_vkCreateShaderModule o_csm = nullptr;
PFN_vkCreateGraphicsPipelines o_cgp = nullptr;
PFN_vkCreateComputePipelines o_ccp = nullptr;
PFN_vkCreateBuffer o_cb = nullptr;
PFN_vkDestroyBuffer o_db = nullptr;
PFN_vkAllocateMemory o_am = nullptr;
PFN_vkFreeMemory o_fm = nullptr;
PFN_vkBindBufferMemory o_bbm = nullptr;
PFN_vkBindBufferMemory2 o_bbm2 = nullptr;
PFN_vkCreateImageView o_civ = nullptr;
PFN_vkCreateBufferView o_cbv = nullptr;
PFN_vkAllocateDescriptorSets o_ads = nullptr;
PFN_vkUpdateDescriptorSets o_uds = nullptr;

const void* find_chain(const void* p, VkStructureType t) {
    for (auto* s = static_cast<const VkBaseInStructure*>(p); s; s = s->pNext) if (s->sType == t) return s;
    return nullptr;
}

VkResult VKAPI_PTR h_csm(VkDevice d, const VkShaderModuleCreateInfo* ci, const VkAllocationCallbacks* a, VkShaderModule* out) {
    const VkResult r = o_csm(d, ci, a, out);
    if (r == VK_SUCCESS) {
        const PInfo pi = scan_spirv(ci->pCode, ci->codeSize / 4);
        std::lock_guard<std::mutex> lk(g_pmu);
        g_mod[H(*out)] = pi;
        ++g_mods_n;
        for (int b = 0; b < 4; ++b) if (pi.f & (1u << b)) ++g_mods_f[b];
    }
    return r;
}

PInfo stages_info_locked(const VkPipelineShaderStageCreateInfo* st, uint32_t n) {
    PInfo f;
    for (uint32_t i = 0; i < n; ++i) {
        if (st[i].module) or_into(f, module_info_locked(st[i].module));
        else if (auto* mi = static_cast<const VkShaderModuleCreateInfo*>(find_chain(st[i].pNext, VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO)))
            or_into(f, scan_spirv(mi->pCode, mi->codeSize / 4));
        else f.f |= kFUnknown;
    }
    return f;
}

VkResult VKAPI_PTR h_cgp(VkDevice d, VkPipelineCache c, uint32_t n, const VkGraphicsPipelineCreateInfo* ci, const VkAllocationCallbacks* a, VkPipeline* out) {
    const VkResult r = o_cgp(d, c, n, ci, a, out);
    std::lock_guard<std::mutex> lk(g_pmu);
    for (uint32_t i = 0; i < n; ++i) {
        if (!out[i]) continue;
        PInfo f = stages_info_locked(ci[i].pStages, ci[i].stageCount);
        if (auto* li = static_cast<const VkPipelineLibraryCreateInfoKHR*>(find_chain(ci[i].pNext, VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR)))
            for (uint32_t k = 0; k < li->libraryCount; ++k) {
                const auto it = g_pipe.find(H(li->pLibraries[k]));
                or_into(f, it == g_pipe.end() ? unknown_info() : it->second);
            }
        g_pipe[H(out[i])] = f;
    }
    return r;
}
VkResult VKAPI_PTR h_ccp(VkDevice d, VkPipelineCache c, uint32_t n, const VkComputePipelineCreateInfo* ci, const VkAllocationCallbacks* a, VkPipeline* out) {
    const VkResult r = o_ccp(d, c, n, ci, a, out);
    std::lock_guard<std::mutex> lk(g_pmu);
    for (uint32_t i = 0; i < n; ++i)
        if (out[i]) g_pipe[H(out[i])] = stages_info_locked(&ci[i].stage, 1);
    return r;
}

VkResult VKAPI_PTR h_cb(VkDevice d, const VkBufferCreateInfo* ci, const VkAllocationCallbacks* a, VkBuffer* out) {
    const VkResult r = o_cb(d, ci, a, out);
    if (r == VK_SUCCESS) note_buffer(*out, ci->usage, ci->size);
    return r;
}
void VKAPI_PTR h_db(VkDevice d, VkBuffer b, const VkAllocationCallbacks* a) {
    if (b) { std::lock_guard<std::mutex> lk(g_mu); g_buf.erase(H(b)); }
    o_db(d, b, a);
}
VkResult VKAPI_PTR h_am(VkDevice d, const VkMemoryAllocateInfo* ai, const VkAllocationCallbacks* a, VkDeviceMemory* out) {
    const VkResult r = o_am(d, ai, a, out);
    if (r == VK_SUCCESS)
        if (auto* imp = static_cast<const VkImportMemoryHostPointerInfoEXT*>(find_chain(ai->pNext, VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT))) {
            std::lock_guard<std::mutex> lk(g_mu);
            g_hostmem[H(*out)] = H(imp->pHostPointer);
        }
    return r;
}
void VKAPI_PTR h_fm(VkDevice d, VkDeviceMemory m, const VkAllocationCallbacks* a) {
    if (m) { std::lock_guard<std::mutex> lk(g_mu); g_hostmem.erase(H(m)); }
    o_fm(d, m, a);
}
void bound_locked(VkBuffer b, VkDeviceMemory m, VkDeviceSize off) {
    const auto it = g_hostmem.find(H(m));
    BufInfo& i = g_buf[H(b)];
    if (it != g_hostmem.end()) { i.space = kHostSpace; i.base = it->second + off; i.bda = true; }
    else if (!i.space) i.space = H(b);
}
VkResult VKAPI_PTR h_bbm(VkDevice d, VkBuffer b, VkDeviceMemory m, VkDeviceSize off) {
    const VkResult r = o_bbm(d, b, m, off);
    if (r == VK_SUCCESS) { std::lock_guard<std::mutex> lk(g_mu); bound_locked(b, m, off); }
    return r;
}
VkResult VKAPI_PTR h_bbm2(VkDevice d, uint32_t n, const VkBindBufferMemoryInfo* bi) {
    const VkResult r = o_bbm2(d, n, bi);
    if (r == VK_SUCCESS) { std::lock_guard<std::mutex> lk(g_mu); for (uint32_t i = 0; i < n; ++i) bound_locked(bi[i].buffer, bi[i].memory, bi[i].memoryOffset); }
    return r;
}
VkResult VKAPI_PTR h_civ(VkDevice d, const VkImageViewCreateInfo* ci, const VkAllocationCallbacks* a, VkImageView* out) {
    const VkResult r = o_civ(d, ci, a, out);
    if (r == VK_SUCCESS) { std::lock_guard<std::mutex> lk(g_mu); g_view[H(*out)] = H(ci->image); }
    return r;
}
VkResult VKAPI_PTR h_cbv(VkDevice d, const VkBufferViewCreateInfo* ci, const VkAllocationCallbacks* a, VkBufferView* out) {
    const VkResult r = o_cbv(d, ci, a, out);
    if (r == VK_SUCCESS) { std::lock_guard<std::mutex> lk(g_mu); g_bview[H(*out)] = {ci->buffer, ci->offset, ci->range}; }
    return r;
}
void sums_forget(const uint64_t* h, uint32_t n) {   // handles allocated again: their old contents go (a new one has none)
    std::lock_guard<std::mutex> lk(g_mu);
    for (uint32_t i = 0; i < n; ++i) {
        const auto it = g_sums.find(h[i]);
        if (it == g_sums.end() || it->second.e.empty()) continue;
        it->second.e.clear();
        it->second.ver = ++g_ver;
        sum_refresh(it->second);
    }
}
VkResult VKAPI_PTR h_ads(VkDevice d, const VkDescriptorSetAllocateInfo* ai, VkDescriptorSet* out) {
    const VkResult r = o_ads(d, ai, out);
    if (r == VK_SUCCESS) {
        static_assert(sizeof(VkDescriptorSet) == 8);
        // on the recorder, in order with the queued updates of these sets (the summaries are its own); else now
        if (!cmdq::post_words(&sums_forget, reinterpret_cast<const uint64_t*>(out), ai->descriptorSetCount))
            sums_forget(reinterpret_cast<const uint64_t*>(out), ai->descriptorSetCount);
    }
    return r;
}

uint64_t view_image_locked(VkImageView v) {
    const auto it = g_view.find(H(v));
    return it == g_view.end() ? H(v) : it->second;   // unknown view: tracked as an image of its own
}

void put(Sum& s, uint32_t key, const Res* r) {   // r null: the descriptor holds nothing tracked
    for (size_t i = 0; i < s.e.size(); ++i)
        if (s.e[i].first == key) {
            if (r) s.e[i].second = *r;
            else { s.e[i] = s.e.back(); s.e.pop_back(); }
            return;
        }
    if (r) s.e.emplace_back(key, *r);
}

void VKAPI_PTR h_uds(VkDevice d, uint32_t n, const VkWriteDescriptorSet* w, uint32_t nc, const VkCopyDescriptorSet* c) {
    if (tl_mode != 1) o_uds(d, n, w, nc, c);
    if (tl_mode == 2) return;
    std::lock_guard<std::mutex> lk(g_mu);
    for (uint32_t i = 0; i < n; ++i) {
        Sum& s = g_sums[H(w[i].dstSet)];
        s.ver = ++g_ver;
        for (uint32_t k = 0; k < w[i].descriptorCount; ++k) {
            const uint32_t key = (w[i].dstBinding << 16) + w[i].dstArrayElement + k;
            Res r;
            bool has = false;
            switch (w[i].descriptorType) {
                case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER: case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
                case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC: case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC: {
                    const VkDescriptorBufferInfo& b = w[i].pBufferInfo[k];
                    if (!b.buffer) break;
                    r.kind = 0;
                    r.storage = w[i].descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER || w[i].descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
                    if (w[i].descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC || w[i].descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC)
                        r.r = buf_rng_locked(b.buffer, b.offset, kEnd);   // a dynamic offset moves it: to the end
                    else r.r = buf_rng_locked(b.buffer, b.offset, b.range);
                    has = true;
                    break;
                }
                case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER: case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER: {
                    const VkBufferView v = w[i].pTexelBufferView[k];
                    if (!v) break;
                    const auto it = g_bview.find(H(v));
                    r.kind = 0;
                    r.storage = w[i].descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
                    if (it != g_bview.end()) r.r = buf_rng_locked(it->second.buf, it->second.off, it->second.range);
                    else { r.kind = 2; }
                    has = true;
                    break;
                }
                case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE: {
                    const VkDescriptorImageInfo& ii = w[i].pImageInfo[k];
                    if (!ii.imageView) break;
                    r.kind = 1;
                    r.storage = true;
                    r.img = view_image_locked(ii.imageView);
                    has = true;
                    break;
                }
                case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER: case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE: case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT: {
                    const VkDescriptorImageInfo& ii = w[i].pImageInfo[k];
                    if (!ii.imageView || ii.imageLayout != VK_IMAGE_LAYOUT_GENERAL) break;   // another layout: ordered by its transition
                    r.kind = 1;
                    r.img = view_image_locked(ii.imageView);
                    has = true;
                    break;
                }
                default: break;   // samplers, inline uniforms
            }
            if (has || !s.e.empty()) put(s, key, has ? &r : nullptr);
        }
        sum_refresh(s);
    }
    for (uint32_t i = 0; i < nc; ++i) {   // copies: what the source holds is not followed per descriptor
        Sum& s = g_sums[H(c[i].dstSet)];
        s.ver = ++g_ver;
        Res r;
        r.kind = 2;
        put(s, 0xffffffffu, &r);
        sum_refresh(s);
    }
}

// ---- the render thread's tracking --------------------------------------------------------------------------------------------
struct Acc {
    std::vector<Rng> r;
    std::vector<uint64_t> img;
    bool all = false;     // unknown: anything
    bool bda = false;     // every buffer with a device address
    uint32_t nbda = 0;    // ranges in r over such buffers
    size_t cap = 512;
    // Coarse (the read sets R / Rp): ranges as bits of a hashed bitmap of 64 KiB granules (a false hit only costs a barrier), ranges over
    // kCoarseMax granules in `r` as they are. Adding a draw's reads is then a few bit sets instead of keeping a sorted interval list.
    bool coarse = false;
    std::vector<uint64_t> bits;   // kBitWords words when coarse
    uint32_t bits_lo = ~0u, bits_hi = 0;   // words touched since the last clear
    static constexpr uint32_t kBitWords = 1024;
    static constexpr uint64_t kCoarseMax = 16;
    static uint32_t gbit(uint64_t space, uint64_t g) { return static_cast<uint32_t>(((space ^ (g * 0x9e3779b97f4a7c15ull)) * 0xff51afd7ed558ccdull) >> 48) & (kBitWords * 64 - 1); }
    size_t sorted_n = 0;  // r[0, sorted_n) is sorted by (space, lo) and has no overlaps (compact); the rest is in the order added
    // Descriptor sets read as a whole (R, Rp, cr): their summaries at a version; a write is checked against them only when its granule bloom
    // meets `sgb` (the OR of theirs), then entry by entry. A summary updated since counts as a hit.
    struct SumRef { const Sum* s; uint64_t ver; };
    std::vector<SumRef> sums;
    uint64_t sgb = 0;
    bool s_img = false, s_bda = false, s_unknown = false;
    uint64_t gb = 0;      // granule bloom of the ranges in r
    uint64_t bloom = 0;   // bit (space hash & 63) of every range in r
    static uint64_t bit(uint64_t space) { return uint64_t{1} << ((space * 0x9e3779b97f4a7c15ull) >> 58); }
    bool empty() const { return !all && !bda && r.empty() && img.empty() && !(coarse && bits_lo <= bits_hi) && sums.empty(); }
    void add_sum(const Sum& s) {
        sums.push_back({&s, s.ver});
        sgb |= s.gb;
        s_img |= s.has_img;
        s_bda |= s.has_bda;
        s_unknown |= s.has_unknown;
    }
    void clear() {
        r.clear(); img.clear(); all = bda = false; nbda = 0; cap = 512; bloom = 0; sorted_n = 0;
        sums.clear(); sgb = 0; s_img = s_bda = s_unknown = false; gb = 0;
        if (bits_lo <= bits_hi) std::fill(bits.begin() + bits_lo, bits.begin() + bits_hi + 1, 0);
        bits_lo = ~0u; bits_hi = 0;
    }
    bool coarse_add(const Rng& x) {   // true: taken by the bitmap
        const uint64_t g0 = x.lo >> 16, g1 = (x.hi - 1) >> 16;
        if (g1 - g0 >= kCoarseMax) return false;
        if (bits.empty()) bits.assign(kBitWords, 0);
        for (uint64_t g = g0; g <= g1; ++g) {
            const uint32_t b = gbit(x.space, g), w = b >> 6;
            bits[w] |= uint64_t{1} << (b & 63);
            bits_lo = std::min(bits_lo, w);
            bits_hi = std::max(bits_hi, w);
        }
        nbda += x.bda;
        any_bits = true;
        return true;
    }
    bool any_bits = false;
    void compact() {
        std::sort(r.begin(), r.end(), [](const Rng& a, const Rng& b) { return a.space != b.space ? a.space < b.space : a.lo < b.lo; });
        size_t o = 0;
        for (size_t i = 0; i < r.size(); ++i) {
            if (o && r[o - 1].space == r[i].space && r[i].lo <= r[o - 1].hi) {
                r[o - 1].hi = std::max(r[o - 1].hi, r[i].hi);
                r[o - 1].bda |= r[i].bda;
            } else r[o++] = r[i];
        }
        r.resize(o);
        nbda = 0;
        for (const Rng& x : r) nbda += x.bda;
        sorted_n = r.size();
    }
    void add(const Rng& x) {
        if (coarse && coarse_add(x)) return;
        gb |= gbits(x.space, x.lo, x.hi);
        r.push_back(x);
        bloom |= bit(x.space);
        nbda += x.bda;
        if (r.size() - sorted_n > std::max<size_t>(32, sorted_n / 4)) compact();
    }
    void add_img(uint64_t i) { if (std::find(img.begin(), img.end(), i) == img.end()) img.push_back(i); }
    void merge(const Acc& o) {
        all |= o.all;
        bda |= o.bda;
        for (const Rng& x : o.r) add(x);
        for (uint64_t i : o.img) add_img(i);
        if (!o.sums.empty()) {
            sums.insert(sums.end(), o.sums.begin(), o.sums.end());
            sgb |= o.sgb;
            s_img |= o.s_img;
            s_bda |= o.s_bda;
            s_unknown |= o.s_unknown;
        }
        if (o.bits_lo <= o.bits_hi) {   // both coarse (R / Rp)
            if (bits.empty()) bits.assign(kBitWords, 0);
            for (uint32_t w = o.bits_lo; w <= o.bits_hi; ++w) bits[w] |= o.bits[w];
            bits_lo = std::min(bits_lo, o.bits_lo);
            bits_hi = std::max(bits_hi, o.bits_hi);
            nbda += o.nbda;
        }
    }
    bool sums_hit(const Rng& x) const {
        if (sums.empty()) return false;
        if (s_unknown) return true;
        const uint64_t xb = gbits(x.space, x.lo, x.hi);
        if (!(xb & sgb)) return false;
        std::lock_guard<std::mutex> lk(g_mu);   // the summaries (other threads may update sets)
        for (const SumRef& sr : sums) {
            if (sr.s->ver != sr.ver) return true;
            if (!(sr.s->gb & xb)) continue;
            for (const auto& [k, e] : sr.s->e)
                if (e.kind == 0 && e.r.space == x.space && e.r.lo < x.hi && x.lo < e.r.hi) return true;
        }
        return false;
    }
    bool sums_hit_img(uint64_t i) const {
        if (sums.empty() || !(s_img || s_unknown)) return false;
        if (s_unknown) return true;
        std::lock_guard<std::mutex> lk(g_mu);
        for (const SumRef& sr : sums) {
            if (sr.s->ver != sr.ver) return true;
            if (!sr.s->has_img) continue;
            for (const auto& [k, e] : sr.s->e) if (e.kind == 1 && e.img == i) return true;
        }
        return false;
    }
    bool hit(const Rng& x) const {
        if (all || (bda && x.bda)) return true;
        if (sums_hit(x)) return true;
        if (bits_lo <= bits_hi) {
            const uint64_t g0 = x.lo >> 16, g1 = (x.hi - 1) >> 16;
            if (g1 - g0 > 4096) return true;   // a huge range: assume it meets something read
            for (uint64_t g = g0; g <= g1; ++g) { const uint32_t b = gbit(x.space, g); if ((bits[b >> 6] >> (b & 63)) & 1) return true; }
        }
        if (!(bloom & bit(x.space))) return false;
        if (sorted_n) {   // the merged prefix: the range starting at or before x.lo, and the one after it
            auto it = std::upper_bound(r.begin(), r.begin() + static_cast<std::ptrdiff_t>(sorted_n), x,
                                       [](const Rng& a, const Rng& b) { return a.space != b.space ? a.space < b.space : a.lo < b.lo; });
            if (it != r.begin()) { const Rng& a = *(it - 1); if (a.space == x.space && a.hi > x.lo) return true; }
            if (it != r.begin() + static_cast<std::ptrdiff_t>(sorted_n) && it->space == x.space && it->lo < x.hi) return true;
        }
        for (size_t i = sorted_n; i < r.size(); ++i) { const Rng& a = r[i]; if (a.space == x.space && a.lo < x.hi && x.lo < a.hi) return true; }
        return false;
    }
    bool hit_bda() const { return all || bda || nbda || s_bda || s_unknown; }
    bool hit_img(uint64_t i) const { return all || std::find(img.begin(), img.end(), i) != img.end() || sums_hit_img(i); }
};

// What one command reads (cr) and writes (cw) before it is checked.
Acc cr, cw;
// Since the last barrier: written (D) and read (R). While a pass waits on its placeholder: Dp / Rp hold what came before the pass.
Acc D, R, Dp, Rp;
struct CoarseInit { CoarseInit() { R.coarse = Rp.coarse = true; } } g_coarse_init;

enum Why : int { kWDraw, kWDispatch, kWCopy, kWWrite, kWImage, kWAttach, kWUnknown, kWEnd, kWBegin, kWOff, kWN };
const char* const kWhyName[kWN] = {"draw", "dispatch", "copy", "fill/update", "image", "attachments", "unknown", "cb end", "cb start", "switch"};
struct Stats {
    uint64_t requests = 0, emitted = 0, by[kWN] = {}, ph_in = 0, ph_skip = 0, ph_need = 0, ph_forced = 0, piggy = 0, img_bar = 0, verify_gaps = 0;
    uint64_t verify_gap_by[kWN] = {};
};
Stats g_st;

std::atomic<VkCommandBuffer> g_cb{VK_NULL_HANDLE};   // the tracked command buffer (recorder side), null: none
VkCommandBuffer g_rt_cb = VK_NULL_HANDLE;             // the same as the render thread queues it (takes; a chunk switch is queued)
bool g_on = false;              // this submission is tracked
bool g_in_pass = false, g_ph = false;
bool g_pre = false;             // Dp / Rp hold what was unordered when the open pass began
std::atomic<uint64_t> g_foreign{0};   // submissions of anything but the tracked command buffers
uint64_t g_foreign_seen = 0;
VkCommandBuffer g_last_ended = VK_NULL_HANDLE;
bool g_req_since = false;       // verify: a barrier was requested since the last access
bool g_ph_bar = false;          // the open pass's placeholder was decided as a barrier (recorder look-ahead)
// The pass's barrier: decided while the recorder looks ahead (g_ph), recorded before vkCmdBeginRendering by h_br.
void ph_decide(bool barrier) { g_ph_bar = barrier; }

// Bound state per bind point (0 graphics, 1 compute).
struct BindPt {
    VkDescriptorSet sets[16] = {};
    uint32_t pend = 0;            // sets not yet registered since the last barrier
    PInfo pi;
    uint32_t push_set = 0;
    bool push_pend = false;
    std::vector<std::pair<uint32_t, Res>> push;   // binding, descriptor
};
BindPt g_bp[2];
Rng g_vb[32];
uint32_t g_vb_set = 0, g_vb_pend = 0;
Rng g_ib;
bool g_ib_set = false, g_ib_pend = false;
uint64_t g_ep = 1;   // barrier interval: a set registered in an older one is registered again
constexpr int kScBits = 12;
struct SumSlot { uint64_t h = 0; Sum* s = nullptr; };
SumSlot g_sc[1 << kScBits];   // render thread: set handle -> its summary

// next hooks (the command queue's, or the driver's)
PFN_vkCmdPipelineBarrier2 n_pb2 = nullptr;

void reset_bound() {
    for (BindPt& b : g_bp) { b = BindPt(); }
    g_vb_set = g_vb_pend = 0;
    g_ib_set = g_ib_pend = false;
}
void all_pending() {
    for (BindPt& b : g_bp) {
        b.pend = 0;
        for (uint32_t i = 0; i < 16; ++i) if (b.sets[i]) b.pend |= 1u << i;
        b.push_pend = !b.push.empty();
    }
    g_vb_pend = g_vb_set;
    g_ib_pend = g_ib_set;
    ++g_ep;
}

void record_barrier(VkCommandBuffer cb, bool host) {
    VkMemoryBarrier2 b = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT | (host ? VK_PIPELINE_STAGE_2_HOST_BIT : 0);
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT | (host ? VK_ACCESS_2_HOST_READ_BIT : 0);
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.memoryBarrierCount = 1;
    di.pMemoryBarriers = &b;
    n_pb2(cb, &di);
    gpu::count_tracked_barrier();
}

// A full barrier now (outside any pass): everything before is ordered.
void emit(VkCommandBuffer cb, Why why, bool host = false) {
    if (mode() == 2) {   // verify: the requested barriers are recorded; a hazard without one since the access is a gap
        if (!g_req_since && why < kWEnd) { ++g_st.verify_gaps; ++g_st.verify_gap_by[why]; }
        record_barrier(cb, host);
    } else record_barrier(cb, host);
    ++g_st.emitted;
    ++g_st.by[why];
    D.clear();
    R.clear();
    all_pending();
    g_req_since = false;
}

// Why a command conflicted (statistics): what of the command against what since the last barrier.
enum Cause : int { kCUnknown, kCPtrRead, kCPtrWrite, kCReadW, kCWriteW, kCWriteR, kCImgRead, kCImgWrite, kCN };
const char* const kCauseName[kCN] = {"unknown", "pointer read after a write", "pointer write", "read after write", "write after write", "write after read",
                                     "image read after write", "image write"};
uint64_t g_cause[kWN][kCN] = {};
// The side of the conflict found first: 0 none, else Cause + 1; who: D or R hit with all / bda / a range.
int conflict_c(const Acc& d, const Acc& r) {
    if (d.empty() && r.empty()) return 0;
    if (cr.all && !d.empty()) return kCUnknown + 1;
    if (cw.all && (!d.empty() || !r.empty())) return kCUnknown + 1;
    if (cr.bda && d.hit_bda()) return kCPtrRead + 1;
    if (cw.bda && (d.hit_bda() || r.hit_bda())) return kCPtrWrite + 1;
    if (d.bloom || d.bda || d.all) for (const Rng& x : cr.r) if (d.hit(x)) return kCReadW + 1;
    for (const Rng& x : cw.r) {
        if (d.hit(x)) return kCWriteW + 1;
        if (r.hit(x)) return kCWriteR + 1;
    }
    if (!d.img.empty() || d.all) for (uint64_t i : cr.img) if (d.hit_img(i)) return kCImgRead + 1;
    for (uint64_t i : cw.img) if (d.hit_img(i) || r.hit_img(i)) return kCImgWrite + 1;
    return 0;
}
Why g_why_now = kWDraw;
bool conflict(const Acc& d, const Acc& r) {
    const int c = conflict_c(d, r);
    if (c) ++g_cause[g_why_now][c - 1];
    return c != 0;
}

// The look-ahead stopped inside the pass (the render thread needed the recorder: a drain or a full ring): a barrier at the pass start.
void ph_force() {
    if (!g_ph) return;
    g_ph = false;
    g_ph_bar = true;
    g_pre = false;
    ++g_st.ph_forced;
    ++g_st.emitted;
    gpu::count_tracked_barrier();
    Dp.clear();
    Rp.clear();
    all_pending();
}
void ph_check_forced() {}

// The accesses in cr / cw (built by `build`, which may be called again after a barrier: what was already registered since the last
// barrier is left out of it) against what is unordered; a barrier when they conflict; then they are registered.
template <typename F>
void access(VkCommandBuffer cb, Why why, F&& build) {
    cr.clear();
    cw.clear();
    build();
    g_why_now = why;
    if (g_in_pass) {
        ph_check_forced();
        if (!g_ph && g_pre && conflict(Dp, Rp)) {   // verify: a hazard inside a pass that no requested barrier ordered
            if (!g_req_since) { ++g_st.verify_gaps; ++g_st.verify_gap_by[why]; }
            Dp.clear();
            Rp.clear();
            g_pre = false;
        }
        if (g_ph && conflict(Dp, Rp)) {
            ph_decide(true);
            g_ph = false;
            ++g_st.ph_need;
            ++g_st.emitted;
            ++g_st.by[why];
            gpu::count_tracked_barrier();
            if (mode() == 2 && !g_req_since) { ++g_st.verify_gaps; ++g_st.verify_gap_by[why]; }
            Dp.clear();
            Rp.clear();
            g_pre = false;
            all_pending();
            cr.clear();
            cw.clear();
            build();
        }
    } else if (conflict(D, R)) {
        emit(cb, why);
        cr.clear();
        cw.clear();
        build();
    }
    D.merge(cw);
    R.merge(cr);
}

inline bool tracking(VkCommandBuffer cb) { return tl_mode != 2 && g_on && cb == g_cb.load(std::memory_order_relaxed) && cb; }

// The bound sets / push descriptors / pipeline flags of a bind point into cr / cw.
// The bindings of set slot `k` a pipeline writes (~0: all).
inline uint64_t wmask(const PInfo& pi, uint32_t k) { return (pi.f & (kFStore | kFUnknown)) ? ~0ull : k < kSets ? pi.wm[k] : 0; }
inline bool written(uint64_t wm, uint32_t key, const Res& x) { const uint32_t b = key >> 16; return x.storage && (b < 64 ? (wm >> b) & 1 : wm == ~0ull); }
void add_res(const Res& x, bool write) {
    if (x.kind == 2) { cr.all = true; if (write) cw.all = true; return; }
    if (x.kind == 1) { (write ? cw : cr).add_img(x.img); if (write) cr.add_img(x.img); return; }
    (write ? cw : cr).add(x.r);
}
void build_bp(int bp) {
    BindPt& b = g_bp[bp];
    const uint32_t f = b.pi.f;
    if (f & kFUnknown) { cr.all = cw.all = true; }
    if (f & kFBdaRead) cr.bda = true;
    if (f & kFBdaWrite) cw.bda = true;
    if (b.pend) {
        std::lock_guard<std::mutex> lk(g_mu);
        for (uint32_t m = b.pend; m; m &= m - 1) {
            const uint32_t i = static_cast<uint32_t>(__builtin_ctz(m));
            const uint64_t h = H(b.sets[i]);
            SumSlot& c = g_sc[(h * 0x9e3779b97f4a7c15ull) >> (64 - kScBits)];
            if (c.h != h) {   // map nodes stay where they are: the pointer is good for as long as the handle has a summary
                const auto it = g_sums.find(h);
                if (it == g_sums.end()) continue;   // never written: holds nothing
                c.h = h;
                c.s = &it->second;
            }
            Sum& sm = *c.s;
            const uint64_t wm = wmask(b.pi, i);
            if (sm.seen_ep == g_ep && sm.seen_ver == sm.ver && (sm.seen_wm & wm) == wm) continue;
            sm.seen_ep = g_ep;
            sm.seen_ver = sm.ver;
            sm.seen_wm = wm;
            // reads: as ranges only when what is unordered (D, or Dp before an open pass) may meet them; else the set as a whole
            const bool meets = sm.has_unknown || D.all || D.bda || Dp.all || Dp.bda || (sm.gb & (D.gb | Dp.gb)) ||
                               (sm.has_img && !(D.img.empty() && Dp.img.empty()));
            bool any_w = false;
            for (const auto& [k, x] : sm.e) {
                const bool wr = written(wm, k, x);
                if (wr || meets) add_res(x, wr);
                any_w |= wr;
            }
            if (!meets) cr.add_sum(sm);
            (void)any_w;
        }
        b.pend = 0;
    }
    if (b.push_pend) {
        const uint64_t wm = wmask(b.pi, b.push_set);
        for (const auto& [k, x] : b.push) add_res(x, written(wm, k << 16, x));
        b.push_pend = false;
    }
}
void build_vertex() {
    for (uint32_t m = g_vb_pend; m; m &= m - 1) cr.add(g_vb[__builtin_ctz(m)]);
    g_vb_pend = 0;
    if (g_ib_pend) { cr.add(g_ib); g_ib_pend = false; }
}

Rng brng(VkBuffer b, VkDeviceSize off, VkDeviceSize size) {
    std::lock_guard<std::mutex> lk(g_mu);
    return buf_rng_locked(b, off, size);
}

// ---- command hooks -------------------------------------------------------------------------------------------------------------
PFN_vkCmdBindPipeline n_bp = nullptr;
PFN_vkCmdBindDescriptorSets n_bds = nullptr;
PFN_vkCmdPushDescriptorSetKHR n_pds = nullptr;
PFN_vkCmdBindVertexBuffers n_bvb = nullptr;
PFN_vkCmdBindVertexBuffers2 n_bvb2 = nullptr;
PFN_vkCmdBindIndexBuffer n_bib = nullptr;
PFN_vkCmdDraw n_d = nullptr;
PFN_vkCmdDrawIndexed n_di = nullptr;
PFN_vkCmdDrawIndirect n_dind = nullptr;
PFN_vkCmdDrawIndexedIndirect n_diind = nullptr;
PFN_vkCmdDrawIndirectCount n_dic = nullptr;
PFN_vkCmdDrawIndexedIndirectCount n_diic = nullptr;
PFN_vkCmdDrawMeshTasksEXT n_dm = nullptr;
PFN_vkCmdDrawMeshTasksIndirectEXT n_dmi = nullptr;
PFN_vkCmdDrawMeshTasksIndirectCountEXT n_dmic = nullptr;
PFN_vkCmdDrawIndirectByteCountEXT n_dibc = nullptr;
PFN_vkCmdDispatch n_disp = nullptr;
PFN_vkCmdDispatchIndirect n_dispi = nullptr;
PFN_vkCmdCopyBuffer n_cpb = nullptr;
PFN_vkCmdCopyBufferToImage n_cbti = nullptr;
PFN_vkCmdCopyImageToBuffer n_citb = nullptr;
PFN_vkCmdCopyImage n_ci = nullptr;
PFN_vkCmdBlitImage n_bi = nullptr;
PFN_vkCmdResolveImage n_ri = nullptr;
PFN_vkCmdClearColorImage n_cci = nullptr;
PFN_vkCmdClearDepthStencilImage n_cdsi = nullptr;
PFN_vkCmdFillBuffer n_fb = nullptr;
PFN_vkCmdUpdateBuffer n_ub = nullptr;
PFN_vkCmdCopyQueryPoolResults n_cqpr = nullptr;
PFN_vkCmdBeginRendering n_br = nullptr;
PFN_vkCmdEndRendering n_er = nullptr;
PFN_vkEndCommandBuffer n_ecb = nullptr;
PFN_vkQueueSubmit n_qs = nullptr;
PFN_vkQueueSubmit2 n_qs2 = nullptr;
PFN_vkCmdPipelineBarrier n_pb1 = nullptr;
PFN_vkCmdExecuteCommands n_ec = nullptr;
PFN_vkCmdCopyBufferToImage2 n_cbti2 = nullptr;
PFN_vkCmdCopyImageToBuffer2 n_citb2 = nullptr;
PFN_vkCmdBeginTransformFeedbackEXT n_btf = nullptr;
PFN_vkCmdEndTransformFeedbackEXT n_etf = nullptr;
PFN_vkCmdBindTransformFeedbackBuffersEXT n_btfb = nullptr;

Rng g_xfb[4];
uint32_t g_xfb_set = 0;
bool g_xfb_on = false;

void VKAPI_PTR h_bp(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline p) {
    if (tracking(cb) && (bp == VK_PIPELINE_BIND_POINT_GRAPHICS || bp == VK_PIPELINE_BIND_POINT_COMPUTE)) {
        BindPt& b = g_bp[bp == VK_PIPELINE_BIND_POINT_COMPUTE ? 1 : 0];
        PInfo f;
        {
            std::lock_guard<std::mutex> lk(g_pmu);
            const auto it = g_pipe.find(H(p));
            f = it == g_pipe.end() ? unknown_info() : it->second;
        }
        for (uint32_t i = 0; i < 16; ++i)   // storage descriptors that are written now and were not: registered again
            if (b.sets[i] && (wmask(f, i) & ~wmask(b.pi, i))) b.pend |= 1u << i;
        if (!b.push.empty() && (wmask(f, b.push_set) & ~wmask(b.pi, b.push_set))) b.push_pend = true;
        b.pi = f;
    }
    if (!dry()) n_bp(cb, bp, p);
}
void VKAPI_PTR h_bds(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout l, uint32_t first, uint32_t n, const VkDescriptorSet* sets, uint32_t nd, const uint32_t* dyn) {
    if (tracking(cb)) {
        BindPt& b = g_bp[bp == VK_PIPELINE_BIND_POINT_COMPUTE ? 1 : 0];
        for (uint32_t i = 0; i < n && first + i < 16; ++i) {
            b.sets[first + i] = sets[i];
            b.pend |= 1u << (first + i);
        }
    }
    if (!dry()) n_bds(cb, bp, l, first, n, sets, nd, dyn);
}
void VKAPI_PTR h_pds(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout l, uint32_t set, uint32_t n, const VkWriteDescriptorSet* w) {
    if (tracking(cb)) {
        BindPt& b = g_bp[bp == VK_PIPELINE_BIND_POINT_COMPUTE ? 1 : 0];
        b.push.clear();
        b.push_set = set;
        std::lock_guard<std::mutex> lk(g_mu);
        for (uint32_t i = 0; i < n; ++i)
            for (uint32_t k = 0; k < w[i].descriptorCount; ++k) {
                Res r;
                const VkDescriptorType t = w[i].descriptorType;
                if (t == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || t == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
                    r.storage = t == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                    r.r = buf_rng_locked(w[i].pBufferInfo[k].buffer, w[i].pBufferInfo[k].offset, w[i].pBufferInfo[k].range);
                } else if (t == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE || (w[i].pImageInfo && w[i].pImageInfo[k].imageLayout == VK_IMAGE_LAYOUT_GENERAL)) {
                    r.kind = 1;
                    r.storage = t == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                    r.img = view_image_locked(w[i].pImageInfo[k].imageView);
                } else if (t == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER || t == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) {
                    r.kind = 2;
                } else continue;
                b.push.emplace_back(w[i].dstBinding, r);
            }
        b.push_pend = true;
    }
    if (!dry()) n_pds(cb, bp, l, set, n, w);
}
void VKAPI_PTR h_bvb(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkBuffer* bufs, const VkDeviceSize* off) {
    if (tracking(cb))
        for (uint32_t i = 0; i < n && first + i < 32; ++i) {
            const uint32_t s = first + i;
            if (bufs[i]) { g_vb[s] = brng(bufs[i], off[i], kEnd); g_vb_set |= 1u << s; g_vb_pend |= 1u << s; }
            else { g_vb_set &= ~(1u << s); g_vb_pend &= ~(1u << s); }
        }
    if (!dry()) n_bvb(cb, first, n, bufs, off);
}
void VKAPI_PTR h_bvb2(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkBuffer* bufs, const VkDeviceSize* off, const VkDeviceSize* sz, const VkDeviceSize* st) {
    if (tracking(cb))
        for (uint32_t i = 0; i < n && first + i < 32; ++i) {
            const uint32_t s = first + i;
            if (bufs[i]) { g_vb[s] = brng(bufs[i], off[i], sz ? sz[i] : kEnd); g_vb_set |= 1u << s; g_vb_pend |= 1u << s; }
            else { g_vb_set &= ~(1u << s); g_vb_pend &= ~(1u << s); }
        }
    if (!dry()) n_bvb2(cb, first, n, bufs, off, sz, st);
}
void VKAPI_PTR h_bib(VkCommandBuffer cb, VkBuffer b, VkDeviceSize off, VkIndexType t) {
    if (tracking(cb)) { g_ib = brng(b, off, kEnd); g_ib_set = g_ib_pend = true; }
    if (!dry()) n_bib(cb, b, off, t);
}

void draw_access(VkCommandBuffer cb, VkBuffer ind = VK_NULL_HANDLE, VkDeviceSize ioff = 0, VkDeviceSize isize = 0, VkBuffer cnt = VK_NULL_HANDLE,
                 VkDeviceSize coff = 0) {
    Rng ir, crg;
    if (ind) ir = brng(ind, ioff, isize);
    if (cnt) crg = brng(cnt, coff, 4);
    access(cb, kWDraw, [&] {
        build_bp(0);
        build_vertex();
        if (ind) cr.add(ir);
        if (cnt) cr.add(crg);
        if (g_xfb_on) for (uint32_t m = g_xfb_set; m; m &= m - 1) cw.add(g_xfb[__builtin_ctz(m)]);
    });
}
void VKAPI_PTR h_d(VkCommandBuffer cb, uint32_t a, uint32_t b, uint32_t c, uint32_t d) { if (tracking(cb)) draw_access(cb); if (!dry()) n_d(cb, a, b, c, d); }
void VKAPI_PTR h_di(VkCommandBuffer cb, uint32_t a, uint32_t b, uint32_t c, int32_t d, uint32_t e) { if (tracking(cb)) draw_access(cb); if (!dry()) n_di(cb, a, b, c, d, e); }
void VKAPI_PTR h_dind(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, uint32_t n, uint32_t s) {
    if (tracking(cb)) draw_access(cb, b, o, n ? static_cast<VkDeviceSize>(s) * (n - 1) + 16 : 16);
    if (!dry()) n_dind(cb, b, o, n, s);
}
void VKAPI_PTR h_diind(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, uint32_t n, uint32_t s) {
    if (tracking(cb)) draw_access(cb, b, o, n ? static_cast<VkDeviceSize>(s) * (n - 1) + 20 : 20);
    if (!dry()) n_diind(cb, b, o, n, s);
}
void VKAPI_PTR h_dic(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, VkBuffer cbuf, VkDeviceSize co, uint32_t n, uint32_t s) {
    if (tracking(cb)) draw_access(cb, b, o, n ? static_cast<VkDeviceSize>(s) * (n - 1) + 16 : 16, cbuf, co);
    if (!dry()) n_dic(cb, b, o, cbuf, co, n, s);
}
void VKAPI_PTR h_diic(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, VkBuffer cbuf, VkDeviceSize co, uint32_t n, uint32_t s) {
    if (tracking(cb)) draw_access(cb, b, o, n ? static_cast<VkDeviceSize>(s) * (n - 1) + 20 : 20, cbuf, co);
    if (!dry()) n_diic(cb, b, o, cbuf, co, n, s);
}
void VKAPI_PTR h_dm(VkCommandBuffer cb, uint32_t x, uint32_t y, uint32_t z) { if (tracking(cb)) draw_access(cb); if (!dry()) n_dm(cb, x, y, z); }
void VKAPI_PTR h_dmi(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, uint32_t n, uint32_t s) {
    if (tracking(cb)) draw_access(cb, b, o, n ? static_cast<VkDeviceSize>(s) * (n - 1) + 12 : 12);
    if (!dry()) n_dmi(cb, b, o, n, s);
}
void VKAPI_PTR h_dmic(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, VkBuffer cbuf, VkDeviceSize co, uint32_t n, uint32_t s) {
    if (tracking(cb)) draw_access(cb, b, o, n ? static_cast<VkDeviceSize>(s) * (n - 1) + 12 : 12, cbuf, co);
    if (!dry()) n_dmic(cb, b, o, cbuf, co, n, s);
}
void VKAPI_PTR h_dibc(VkCommandBuffer cb, uint32_t ic, uint32_t fi, VkBuffer cbuf, VkDeviceSize co, uint32_t cof, uint32_t vs) {
    if (tracking(cb)) draw_access(cb, cbuf, co, 4);
    if (!dry()) n_dibc(cb, ic, fi, cbuf, co, cof, vs);
}
void VKAPI_PTR h_disp(VkCommandBuffer cb, uint32_t x, uint32_t y, uint32_t z) {
    if (tracking(cb)) access(cb, kWDispatch, [] { build_bp(1); });
    if (!dry()) n_disp(cb, x, y, z);
}
void VKAPI_PTR h_dispi(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o) {
    if (tracking(cb)) {
        const Rng ir = brng(b, o, 12);
        access(cb, kWDispatch, [&] { build_bp(1); cr.add(ir); });
    }
    if (!dry()) n_dispi(cb, b, o);
}

void VKAPI_PTR h_cpb(VkCommandBuffer cb, VkBuffer s, VkBuffer d, uint32_t n, const VkBufferCopy* r) {
    if (tracking(cb)) {
        std::vector<Rng> rs, ws;
        {
            std::lock_guard<std::mutex> lk(g_mu);
            for (uint32_t i = 0; i < n; ++i) { rs.push_back(buf_rng_locked(s, r[i].srcOffset, r[i].size)); ws.push_back(buf_rng_locked(d, r[i].dstOffset, r[i].size)); }
        }
        access(cb, kWCopy, [&] { for (const Rng& x : rs) cr.add(x); for (const Rng& x : ws) cw.add(x); });
    }
    if (!dry()) n_cpb(cb, s, d, n, r);
}
// Bytes a buffer-image copy region can touch: 16 bytes per texel (no format here; the widest uncompressed texel), from its buffer offset.
VkDeviceSize region_bytes(const VkBufferImageCopy& c) {
    const uint64_t rl = c.bufferRowLength ? c.bufferRowLength : c.imageExtent.width;
    const uint64_t ih = c.bufferImageHeight ? c.bufferImageHeight : c.imageExtent.height;
    return rl * ih * std::max(1u, c.imageExtent.depth) * std::max(1u, c.imageSubresource.layerCount) * 16;
}
void VKAPI_PTR h_cbti(VkCommandBuffer cb, VkBuffer b, VkImage img, VkImageLayout l, uint32_t n, const VkBufferImageCopy* r) {
    if (tracking(cb)) {
        std::vector<Rng> rs;
        for (uint32_t i = 0; i < n; ++i) rs.push_back(brng(b, r[i].bufferOffset, region_bytes(r[i])));
        access(cb, kWImage, [&] { for (const Rng& x : rs) cr.add(x); cw.add_img(H(img)); });
    }
    if (!dry()) n_cbti(cb, b, img, l, n, r);
}
void VKAPI_PTR h_citb(VkCommandBuffer cb, VkImage img, VkImageLayout l, VkBuffer b, uint32_t n, const VkBufferImageCopy* r) {
    if (tracking(cb)) {
        std::vector<Rng> ws;
        for (uint32_t i = 0; i < n; ++i) ws.push_back(brng(b, r[i].bufferOffset, region_bytes(r[i])));
        access(cb, kWImage, [&] { for (const Rng& x : ws) cw.add(x); cr.add_img(H(img)); });
    }
    if (!dry()) n_citb(cb, img, l, b, n, r);
}
void img_rw(VkCommandBuffer cb, VkImage s, VkImage d) {
    access(cb, kWImage, [&] { if (s) cr.add_img(H(s)); cw.add_img(H(d)); cr.add_img(H(d)); });
}
void VKAPI_PTR h_ci(VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n, const VkImageCopy* r) {
    if (tracking(cb)) img_rw(cb, s, d);
    if (!dry()) n_ci(cb, s, sl, d, dl, n, r);
}
void VKAPI_PTR h_bi(VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n, const VkImageBlit* r, VkFilter f) {
    if (tracking(cb)) img_rw(cb, s, d);
    if (!dry()) n_bi(cb, s, sl, d, dl, n, r, f);
}
void VKAPI_PTR h_ri(VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n, const VkImageResolve* r) {
    if (tracking(cb)) img_rw(cb, s, d);
    if (!dry()) n_ri(cb, s, sl, d, dl, n, r);
}
void VKAPI_PTR h_cci(VkCommandBuffer cb, VkImage i, VkImageLayout l, const VkClearColorValue* c, uint32_t n, const VkImageSubresourceRange* r) {
    if (tracking(cb)) img_rw(cb, VK_NULL_HANDLE, i);
    if (!dry()) n_cci(cb, i, l, c, n, r);
}
void VKAPI_PTR h_cdsi(VkCommandBuffer cb, VkImage i, VkImageLayout l, const VkClearDepthStencilValue* c, uint32_t n, const VkImageSubresourceRange* r) {
    if (tracking(cb)) img_rw(cb, VK_NULL_HANDLE, i);
    if (!dry()) n_cdsi(cb, i, l, c, n, r);
}
void VKAPI_PTR h_fb(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, VkDeviceSize s, uint32_t v) {
    if (tracking(cb)) { const Rng w = brng(b, o, s); access(cb, kWWrite, [&] { cw.add(w); }); }
    if (!dry()) n_fb(cb, b, o, s, v);
}
void VKAPI_PTR h_ub(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, VkDeviceSize s, const void* d) {
    if (tracking(cb)) { const Rng w = brng(b, o, s); access(cb, kWWrite, [&] { cw.add(w); }); }
    if (!dry()) n_ub(cb, b, o, s, d);
}
void VKAPI_PTR h_cqpr(VkCommandBuffer cb, VkQueryPool q, uint32_t first, uint32_t n, VkBuffer b, VkDeviceSize o, VkDeviceSize st, VkQueryResultFlags fl) {
    if (tracking(cb)) {
        const VkDeviceSize one = (fl & VK_QUERY_RESULT_64_BIT ? 8 : 4) * ((fl & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) ? 2 : 1);
        const Rng w = brng(b, o, n ? st * (n - 1) + one : one);
        access(cb, kWWrite, [&] { cw.add(w); });
    }
    if (!dry()) n_cqpr(cb, q, first, n, b, o, st, fl);
}

bool ro_layout(VkImageLayout l) {
    return l == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL || l == VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL ||
           l == VK_IMAGE_LAYOUT_STENCIL_READ_ONLY_OPTIMAL || l == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL || l == VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL;
}
void VKAPI_PTR h_br(VkCommandBuffer cb, const VkRenderingInfo* ri) {
    if (!tracking(cb)) return dry() ? void() : (void)n_br(cb, ri);
    std::vector<std::pair<uint64_t, bool>> att;   // image, written
    {
        std::lock_guard<std::mutex> lk(g_mu);
        auto one = [&](const VkRenderingAttachmentInfo* a) {
            if (!a || !a->imageView) return;
            att.emplace_back(view_image_locked(a->imageView), !ro_layout(a->imageLayout));
            if (a->resolveImageView && a->resolveMode) att.emplace_back(view_image_locked(a->resolveImageView), true);
        };
        for (uint32_t i = 0; i < ri->colorAttachmentCount; ++i) one(&ri->pColorAttachments[i]);
        one(ri->pDepthAttachment);
        one(ri->pStencilAttachment);
    }
    auto build = [&] { for (const auto& [i, wr] : att) { cr.add_img(i); if (wr) cw.add_img(i); } };
    cr.clear();
    cw.clear();
    build();
    g_why_now = kWAttach;
    if (!D.empty() || !R.empty()) {
        if (conflict(D, R)) {
            emit(cb, kWAttach);
        } else if (mode() == 2 || cmdq::on_recorder()) {   // verify: no placeholder, the pre-pass state only for counting
            if (mode() == 1) { ++g_st.ph_in; g_ph = true; g_ph_bar = false; }
            g_pre = true;
            std::swap(D, Dp);
            std::swap(R, Rp);
            D.clear();
            R.clear();
        } else emit(cb, kWAttach);
    }
    D.merge(cw);
    R.merge(cr);
    g_in_pass = true;
    if (g_ph) {
        // The recorder looks at the pass's commands (dry: tracked, not recorded) up to its vkCmdEndRendering, a conflict with what was unordered
        // before the pass decides the barrier; then the barrier (or none), the pass begins and its commands are recorded.
        cmdq::scan_pass();
        if (g_ph && g_in_pass) ph_force();   // stopped early: the render thread waits for the recorder
        if (g_ph_bar) record_barrier(cb, false);
        g_ph_bar = false;
        n_br(cb, ri);
        cmdq::replay_scanned();
        return;
    }
    if (!dry()) n_br(cb, ri);
}
void VKAPI_PTR h_er(VkCommandBuffer cb) {
    if (tracking(cb) && g_in_pass) {
        ph_check_forced();
        if (g_ph) {   // nothing in the pass needed what came before it ordered
            ph_decide(false);
            g_ph = false;
            ++g_st.ph_skip;
        }
        if (g_pre) {
            g_pre = false;
            Dp.merge(D);
            Rp.merge(R);
            std::swap(D, Dp);
            std::swap(R, Rp);
            Dp.clear();
            Rp.clear();
        }
        g_in_pass = false;
    }
    if (!dry()) n_er(cb);
}

// Image barriers (layout transitions): they order their image. Piggyback: a full memory barrier rides along when anything is unordered.
void VKAPI_PTR h_pb2(VkCommandBuffer cb, const VkDependencyInfo* di) {
    if (!tracking(cb) || g_in_pass) return dry() ? void() : (void)n_pb2(cb, di);
    bool full = false;
    for (uint32_t i = 0; i < di->memoryBarrierCount; ++i) {
        const VkMemoryBarrier2& m = di->pMemoryBarriers[i];
        if ((m.srcStageMask & VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT) && (m.dstStageMask & VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT) && (m.srcAccessMask & VK_ACCESS_2_MEMORY_WRITE_BIT))
            full = true;
    }
    if (full) {   // somebody's own full barrier (not memory_barrier): everything is ordered
        D.clear();
        R.clear();
        all_pending();
        return dry() ? void() : (void)n_pb2(cb, di);
    }
    for (uint32_t i = 0; i < di->imageMemoryBarrierCount; ++i) {
        const uint64_t img = H(di->pImageMemoryBarriers[i].image);
        D.img.erase(std::remove(D.img.begin(), D.img.end(), img), D.img.end());
        R.img.erase(std::remove(R.img.begin(), R.img.end(), img), R.img.end());
    }
    ++g_st.img_bar;
    if (di->imageMemoryBarrierCount && piggyback_on() && mode() == 1 && (!D.empty() || !R.empty())) {
        VkMemoryBarrier2 b = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        VkDependencyInfo d2 = *di;   // di has no memory barriers here (a full one returned above; narrower ones are not made)
        if (di->memoryBarrierCount) return dry() ? void() : (void)n_pb2(cb, di);
        d2.memoryBarrierCount = 1;
        d2.pMemoryBarriers = &b;
        ++g_st.piggy;
        D.clear();
        R.clear();
        all_pending();
        return dry() ? void() : (void)n_pb2(cb, &d2);
    }
    if (!dry()) n_pb2(cb, di);
}

// Commands the tracker does not follow: a barrier before when anything is unordered, everything unknown after.
void unknown(VkCommandBuffer cb) {
    if (g_in_pass) {
        ph_check_forced();
        if (g_ph) {
            ph_decide(true);
            g_ph = false;
            ++g_st.ph_need;
            ++g_st.emitted;
            ++g_st.by[kWUnknown];
            gpu::count_tracked_barrier();
            Dp.clear();
            Rp.clear();
            g_pre = false;
            all_pending();
        }
        if (g_pre) { Dp.all = Rp.all = true; }
    } else if (!D.empty() || !R.empty()) emit(cb, kWUnknown);
    D.all = R.all = true;
}
void VKAPI_PTR h_pb1(VkCommandBuffer cb, VkPipelineStageFlags s, VkPipelineStageFlags d, VkDependencyFlags f, uint32_t nm, const VkMemoryBarrier* m, uint32_t nb,
                     const VkBufferMemoryBarrier* b, uint32_t ni, const VkImageMemoryBarrier* im) {
    if (tracking(cb)) unknown(cb);
    if (!dry()) n_pb1(cb, s, d, f, nm, m, nb, b, ni, im);
}
void VKAPI_PTR h_ec(VkCommandBuffer cb, uint32_t n, const VkCommandBuffer* c) { if (tracking(cb)) unknown(cb); if (!dry()) n_ec(cb, n, c); }
void VKAPI_PTR h_cbti2(VkCommandBuffer cb, const VkCopyBufferToImageInfo2* i) { if (tracking(cb)) unknown(cb); if (!dry()) n_cbti2(cb, i); }
void VKAPI_PTR h_citb2(VkCommandBuffer cb, const VkCopyImageToBufferInfo2* i) { if (tracking(cb)) unknown(cb); if (!dry()) n_citb2(cb, i); }
void VKAPI_PTR h_btfb(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkBuffer* b, const VkDeviceSize* o, const VkDeviceSize* s) {
    if (tracking(cb))
        for (uint32_t i = 0; i < n && first + i < 4; ++i) { g_xfb[first + i] = brng(b[i], o[i], s ? s[i] : kEnd); g_xfb_set |= 1u << (first + i); }
    if (!dry()) n_btfb(cb, first, n, b, o, s);
}
void VKAPI_PTR h_btf(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkBuffer* b, const VkDeviceSize* o) {
    if (tracking(cb)) { unknown(cb); g_xfb_on = true; }   // the counter buffers: read and written, not followed
    if (!dry()) n_btf(cb, first, n, b, o);
}
void VKAPI_PTR h_etf(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkBuffer* b, const VkDeviceSize* o) {
    if (tracking(cb)) { unknown(cb); g_xfb_on = false; }
    if (!dry()) n_etf(cb, first, n, b, o);
}

void begin_cb(VkCommandBuffer cb) {
    g_cb.store(cb, std::memory_order_relaxed);
    reset_bound();
    ++g_ep;
    g_in_pass = g_ph = g_pre = false;
    g_xfb_set = 0;
    g_xfb_on = false;
    if (g_foreign.load(std::memory_order_relaxed) != g_foreign_seen) {   // something else ran on the queue since: what it wrote is not known here
        g_foreign_seen = g_foreign.load(std::memory_order_relaxed);
        D.all = true;
    }
}

thread_local int tl_render = -1;
inline bool on_rt() {
    if (tl_render < 0) tl_render = gpu::on_render_thread() ? 1 : 0;
    return tl_render == 1;
}

VkResult VKAPI_PTR h_ecb(VkCommandBuffer cb) {
    if (tracking(cb)) {
        if (g_ph) { ph_decide(true); g_ph = false; }
        if (!D.empty()) emit(cb, kWEnd, true);   // what the CPU reads after the fence, and anything submitted later
        g_last_ended = cb;
    }
    return n_ecb(cb);
}
bool ours(uint32_t n, const VkCommandBuffer* c) {
    for (uint32_t i = 0; i < n; ++i) if (c[i] == g_last_ended && c[i]) return true;
    return false;
}
VkResult VKAPI_PTR h_qs(VkQueue q, uint32_t n, const VkSubmitInfo* s, VkFence f) {
    bool mine = false;
    if (on_rt() || cmdq::on_recorder()) for (uint32_t i = 0; i < n; ++i) mine |= ours(s[i].commandBufferCount, s[i].pCommandBuffers);
    if (!mine) g_foreign.fetch_add(1, std::memory_order_relaxed);
    return n_qs(q, n, s, f);
}
VkResult VKAPI_PTR h_qs2(VkQueue q, uint32_t n, const VkSubmitInfo2* s, VkFence f) {
    bool mine = false;
    if (on_rt() || cmdq::on_recorder())
        for (uint32_t i = 0; i < n; ++i)
            for (uint32_t k = 0; k < s[i].commandBufferInfoCount; ++k) mine |= s[i].pCommandBufferInfos[k].commandBuffer == g_last_ended && g_last_ended;
    if (!mine) g_foreign.fetch_add(1, std::memory_order_relaxed);
    return n_qs2(q, n, s, f);
}

const void* g_hooks[96];
int g_nhooks = 0;
template <typename P> void hk(P& slot, P& next, P fn) {
    if (slot && slot != fn) { next = slot; slot = fn; }
    if (g_nhooks < 96) g_hooks[g_nhooks++] = reinterpret_cast<const void*>(fn);
}

}  // namespace

bool enabled() { return mode() != 0; }
bool is_hook(const void* fn) {
    if (!fn) return false;
    for (int i = 0; i < g_nhooks; ++i) if (g_hooks[i] == fn) return true;
    return false;
}
void set_thread_mode(int m) { tl_mode = m; }
bool scan_open() { return g_ph; }
bool verify() { return mode() == 2; }

void note_buffer(VkBuffer b, VkBufferUsageFlags usage, VkDeviceSize size) {
    if (!b) return;
    std::lock_guard<std::mutex> lk(g_mu);
    BufInfo& i = g_buf[H(b)];
    i = BufInfo();
    i.space = H(b);
    i.size = size;
    // [nvn] hazard_bda_host (default on): pointer shaders reach game memory only (the guest page table maps the pool imports, which
    // bound_locked marks), so the runtime's own buffers with a device address (upload ring, copies) are not "every buffer with a device
    // address" for them.
    static const bool host_only = config_get_bool("nvn", "hazard_bda_host", true);
    i.bda = !host_only && (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) != 0;
}

void install_create_hooks() {
    if (!enabled()) return;
    hk(vkCreateShaderModule, o_csm, &h_csm);
    hk(vkCreateGraphicsPipelines, o_cgp, &h_cgp);
    hk(vkCreateComputePipelines, o_ccp, &h_ccp);
    hk(vkCreateBuffer, o_cb, &h_cb);
    hk(vkDestroyBuffer, o_db, &h_db);
    hk(vkAllocateMemory, o_am, &h_am);
    hk(vkFreeMemory, o_fm, &h_fm);
    hk(vkBindBufferMemory, o_bbm, &h_bbm);
    hk(vkBindBufferMemory2, o_bbm2, &h_bbm2);
    hk(vkCreateImageView, o_civ, &h_civ);
    hk(vkCreateBufferView, o_cbv, &h_cbv);
    hk(vkAllocateDescriptorSets, o_ads, &h_ads);
    hk(vkUpdateDescriptorSets, o_uds, &h_uds);
}

void install_cmd_hooks() {
    if (!enabled() || !o_uds) return;   // without the create hooks nothing is known about sets and pipelines
    hk(vkCmdPipelineBarrier2, n_pb2, &h_pb2);
    if (!n_pb2) return;
    hk(vkCmdBindPipeline, n_bp, &h_bp);
    hk(vkCmdBindDescriptorSets, n_bds, &h_bds);
    hk(vkCmdPushDescriptorSetKHR, n_pds, &h_pds);
    hk(vkCmdBindVertexBuffers, n_bvb, &h_bvb);
    hk(vkCmdBindVertexBuffers2, n_bvb2, &h_bvb2);
    hk(vkCmdBindIndexBuffer, n_bib, &h_bib);
    hk(vkCmdDraw, n_d, &h_d);
    hk(vkCmdDrawIndexed, n_di, &h_di);
    hk(vkCmdDrawIndirect, n_dind, &h_dind);
    hk(vkCmdDrawIndexedIndirect, n_diind, &h_diind);
    hk(vkCmdDrawIndirectCount, n_dic, &h_dic);
    hk(vkCmdDrawIndexedIndirectCount, n_diic, &h_diic);
    hk(vkCmdDrawMeshTasksEXT, n_dm, &h_dm);
    hk(vkCmdDrawMeshTasksIndirectEXT, n_dmi, &h_dmi);
    hk(vkCmdDrawMeshTasksIndirectCountEXT, n_dmic, &h_dmic);
    hk(vkCmdDrawIndirectByteCountEXT, n_dibc, &h_dibc);
    hk(vkCmdDispatch, n_disp, &h_disp);
    hk(vkCmdDispatchIndirect, n_dispi, &h_dispi);
    hk(vkCmdCopyBuffer, n_cpb, &h_cpb);
    hk(vkCmdCopyBufferToImage, n_cbti, &h_cbti);
    hk(vkCmdCopyImageToBuffer, n_citb, &h_citb);
    hk(vkCmdCopyImage, n_ci, &h_ci);
    hk(vkCmdBlitImage, n_bi, &h_bi);
    hk(vkCmdResolveImage, n_ri, &h_ri);
    hk(vkCmdClearColorImage, n_cci, &h_cci);
    hk(vkCmdClearDepthStencilImage, n_cdsi, &h_cdsi);
    hk(vkCmdFillBuffer, n_fb, &h_fb);
    hk(vkCmdUpdateBuffer, n_ub, &h_ub);
    hk(vkCmdCopyQueryPoolResults, n_cqpr, &h_cqpr);
    hk(vkCmdBeginRendering, n_br, &h_br);
    hk(vkCmdEndRendering, n_er, &h_er);
    hk(vkEndCommandBuffer, n_ecb, &h_ecb);
    hk(vkQueueSubmit, n_qs, &h_qs);
    hk(vkQueueSubmit2, n_qs2, &h_qs2);
    hk(vkCmdPipelineBarrier, n_pb1, &h_pb1);
    hk(vkCmdExecuteCommands, n_ec, &h_ec);
    hk(vkCmdCopyBufferToImage2, n_cbti2, &h_cbti2);
    hk(vkCmdCopyImageToBuffer2, n_citb2, &h_citb2);
    hk(vkCmdBeginTransformFeedbackEXT, n_btf, &h_btf);
    hk(vkCmdEndTransformFeedbackEXT, n_etf, &h_etf);
    hk(vkCmdBindTransformFeedbackBuffersEXT, n_btfb, &h_btfb);
    NXLOG_I(kTag, "hazard tracking: %s (barriers where commands touch what an earlier command wrote; piggyback %s)", mode() == 2 ? "verify" : "on",
            piggyback_on() ? "on" : "off");
}

void begin_submission(VkCommandBuffer cb, bool on, bool piggyback) {
    g_piggy_ab = piggyback;
    const bool want = on && enabled() && n_pb2 && cmdq::queuing();
    if (!want) {
        if (g_on && cb && (!D.empty() || !R.empty())) {   // switching off: the old barriers order only what follows them
            record_barrier(cb, true);
            ++g_st.emitted;
            ++g_st.by[kWOff];
        }
        g_on = false;
        g_cb.store(VK_NULL_HANDLE, std::memory_order_relaxed);
        g_rt_cb = VK_NULL_HANDLE;
        D.clear(); R.clear(); Dp.clear(); Rp.clear();
        return;
    }
    if (!g_on) { D.clear(); R.clear(); D.all = true; }   // switching on: nothing is known about what came before
    g_on = true;
    g_rt_cb = cb;
    begin_cb(cb);
}

void switch_cb(VkCommandBuffer from, VkCommandBuffer to) {
    if (on_rt() && from && from == g_rt_cb) g_rt_cb = to;
    if (g_on && (on_rt() || cmdq::on_recorder()) && from && from == g_cb.load(std::memory_order_relaxed)) begin_cb(to);
}
void note_rt_switch(VkCommandBuffer from, VkCommandBuffer to) {   // render thread: the switch is queued (the recorder makes switch_cb)
    if (from && from == g_rt_cb) g_rt_cb = to;
}

void external(VkCommandBuffer cb) {
    if (!(tracking(cb) && on_rt())) return;
    // in order with the queued commands: the recorder makes it (the tracking runs there)
    cmdq::post(+[](VkCommandBuffer c) { if (tracking(c) && !g_in_pass) unknown(c); }, cb);
}

bool takes(VkCommandBuffer cb) { return mode() == 1 && g_on && cb && cb == g_rt_cb && on_rt(); }

void request(uint32_t) {
    ++g_st.requests;
    g_req_since = true;
}

std::string stats_line() {
    uint64_t ph_n = 0, ph_f = 0;
    cmdq::ph_stats(&ph_n, &ph_f);
    const uint64_t presents = gpu::counters_if() ? gpu::counters_if()->presents.load() : 0;
    char b[2400];
    int o = std::snprintf(b, sizeof b, "hazard: %s, %llu presents: %llu barriers requested, %llu recorded (", mode() == 2 ? "verify" : "on",
                          (unsigned long long)presents, (unsigned long long)g_st.requests, (unsigned long long)g_st.emitted);
    for (int i = 0; i < kWN && o < (int)sizeof b; ++i)
        if (g_st.by[i]) o += std::snprintf(b + o, sizeof b - o, "%s %llu, ", kWhyName[i], (unsigned long long)g_st.by[i]);
    if (o < (int)sizeof b)
        o += std::snprintf(b + o, sizeof b - o, "pass starts %llu), placeholders %llu: %llu nothing, %llu barrier, %llu forced; %llu image barriers, %llu with a full barrier added",
                           (unsigned long long)g_st.by[kWAttach], (unsigned long long)g_st.ph_in, (unsigned long long)g_st.ph_skip,
                           (unsigned long long)g_st.ph_need, (unsigned long long)g_st.ph_forced, (unsigned long long)g_st.img_bar, (unsigned long long)g_st.piggy);
    if (o < (int)sizeof b) {
        uint64_t sc = 0, ss = 0, sw = 0;
        cmdq::scan_stats(&sc, &ss, &sw);
        o += std::snprintf(b + o, sizeof b - o, "; recorder look-aheads %llu (%llu stopped early, %llu waits for queued commands)", (unsigned long long)sc,
                           (unsigned long long)ss, (unsigned long long)sw);
    }
    if (o < (int)sizeof b) {
        std::lock_guard<std::mutex> lk(g_pmu);
        o += std::snprintf(b + o, sizeof b - o, "; modules %llu: %llu pointer loads, %llu pointer stores, %llu storage stores, %llu unknown", (unsigned long long)g_mods_n,
                           (unsigned long long)g_mods_f[0], (unsigned long long)g_mods_f[1], (unsigned long long)g_mods_f[2], (unsigned long long)g_mods_f[3]);
    }
    for (int w = 0; w < kWN && o < (int)sizeof b; ++w) {
        uint64_t t = 0;
        for (int c = 0; c < kCN; ++c) t += g_cause[w][c];
        if (!t) continue;
        o += std::snprintf(b + o, sizeof b - o, "; %s:", kWhyName[w]);
        for (int c = 0; c < kCN && o < (int)sizeof b; ++c)
            if (g_cause[w][c]) o += std::snprintf(b + o, sizeof b - o, " %s %llu", kCauseName[c], (unsigned long long)g_cause[w][c]);
    }
    if (mode() == 2 && o < (int)sizeof b) {
        o += std::snprintf(b + o, sizeof b - o, "; verify: %llu hazards with no requested barrier since (", (unsigned long long)g_st.verify_gaps);
        for (int i = 0; i < kWN && o < (int)sizeof b; ++i)
            if (g_st.verify_gap_by[i]) o += std::snprintf(b + o, sizeof b - o, " %s %llu", kWhyName[i], (unsigned long long)g_st.verify_gap_by[i]);
        if (o < (int)sizeof b) std::snprintf(b + o, sizeof b - o, " )");
    }
    return b;
}

}  // namespace nxrt::nvn::be::hazard
