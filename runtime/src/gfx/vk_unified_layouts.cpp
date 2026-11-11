// SPDX-License-Identifier: GPL-3.0-or-later
// [gfx] unified_layouts (VK_KHR_unified_image_layouts where the driver has it; default off: in the game's village outside at 1080 it changed
// no GPU time, see AGENTS.md P5): every image layout the runtime names is handed to the driver as VK_IMAGE_LAYOUT_GENERAL.
//
// With the extension, GENERAL is as fast as the specific layouts for every use, so an image never needs a layout change, only the
// dependency a barrier carries. The runtime's own bookkeeping keeps its layouts (they say how an image was used last, and a change of them
// is where it records its barriers); only what reaches the driver is mapped. The mapping is a function of the layout alone, applied to every
// call that takes one (barriers, rendering attachments, copies, blits, resolves, clears, descriptor writes), so each image's layout as the
// driver sees it is always consistent: UNDEFINED before its first use, then GENERAL (PRESENT_SRC for swapchain images when presented). The
// hooks sit under every other hook on the volk pointers (installed right after volkLoadDevice): with [nvn] cmd_thread the mapping runs on the
// recorder thread. With this on, the NVN replay may re-tag an image as sampled without a barrier where a full barrier already followed its
// last write (nvn_replay.cpp prepare_textures, [nvn] uil_retag): no pass break for a layout change alone.
#include <volk.h>

#include <cstring>
#include <vector>

namespace nxrt::gfx {

bool g_unified_layouts = false;

namespace {

inline VkImageLayout m(VkImageLayout l) {
    switch (l) {
        case VK_IMAGE_LAYOUT_UNDEFINED: case VK_IMAGE_LAYOUT_PREINITIALIZED: case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR: case VK_IMAGE_LAYOUT_SHARED_PRESENT_KHR:
        case VK_IMAGE_LAYOUT_GENERAL:
            return l;
        default:
            return VK_IMAGE_LAYOUT_GENERAL;
    }
}
inline bool odd(VkImageLayout l) { return m(l) != l; }

// A small array on the stack, more on the heap.
template <typename T, size_t N = 16> struct Tmp {
    T buf[N];
    std::vector<T> big;
    T* p;
    Tmp(const T* src, uint32_t n) {
        if (n <= N) p = buf; else { big.resize(n); p = big.data(); }
        if (n) std::memcpy(p, src, sizeof(T) * n);
    }
};

PFN_vkCmdPipelineBarrier2 o_pb2 = nullptr;
void VKAPI_PTR h_pb2(VkCommandBuffer cb, const VkDependencyInfo* di) {
    bool need = false;
    for (uint32_t i = 0; i < di->imageMemoryBarrierCount && !need; ++i) need = odd(di->pImageMemoryBarriers[i].oldLayout) || odd(di->pImageMemoryBarriers[i].newLayout);
    if (!need) return o_pb2(cb, di);
    Tmp<VkImageMemoryBarrier2> b(di->pImageMemoryBarriers, di->imageMemoryBarrierCount);
    for (uint32_t i = 0; i < di->imageMemoryBarrierCount; ++i) { b.p[i].oldLayout = m(b.p[i].oldLayout); b.p[i].newLayout = m(b.p[i].newLayout); }
    VkDependencyInfo d = *di;
    d.pImageMemoryBarriers = b.p;
    o_pb2(cb, &d);
}

PFN_vkCmdPipelineBarrier o_pb = nullptr;
void VKAPI_PTR h_pb(VkCommandBuffer cb, VkPipelineStageFlags s, VkPipelineStageFlags d, VkDependencyFlags f, uint32_t nm, const VkMemoryBarrier* mb,
                    uint32_t nb, const VkBufferMemoryBarrier* bb, uint32_t ni, const VkImageMemoryBarrier* ib) {
    bool need = false;
    for (uint32_t i = 0; i < ni && !need; ++i) need = odd(ib[i].oldLayout) || odd(ib[i].newLayout);
    if (!need) return o_pb(cb, s, d, f, nm, mb, nb, bb, ni, ib);
    Tmp<VkImageMemoryBarrier> b(ib, ni);
    for (uint32_t i = 0; i < ni; ++i) { b.p[i].oldLayout = m(b.p[i].oldLayout); b.p[i].newLayout = m(b.p[i].newLayout); }
    o_pb(cb, s, d, f, nm, mb, nb, bb, ni, b.p);
}

PFN_vkCmdBeginRendering o_br = nullptr;
void VKAPI_PTR h_br(VkCommandBuffer cb, const VkRenderingInfo* ri) {
    VkRenderingAttachmentInfo c[8];
    VkRenderingAttachmentInfo dep, ste;
    VkRenderingInfo r = *ri;
    const uint32_t n = ri->colorAttachmentCount <= 8 ? ri->colorAttachmentCount : 8;
    if (ri->colorAttachmentCount <= 8) {
        for (uint32_t i = 0; i < n; ++i) {
            c[i] = ri->pColorAttachments[i];
            c[i].imageLayout = m(c[i].imageLayout);
            c[i].resolveImageLayout = m(c[i].resolveImageLayout);
        }
        r.pColorAttachments = n ? c : ri->pColorAttachments;
    }
    if (ri->pDepthAttachment) { dep = *ri->pDepthAttachment; dep.imageLayout = m(dep.imageLayout); dep.resolveImageLayout = m(dep.resolveImageLayout); r.pDepthAttachment = &dep; }
    if (ri->pStencilAttachment) { ste = *ri->pStencilAttachment; ste.imageLayout = m(ste.imageLayout); ste.resolveImageLayout = m(ste.resolveImageLayout); r.pStencilAttachment = &ste; }
    o_br(cb, &r);
}

PFN_vkCmdCopyBufferToImage o_cbti = nullptr;
void VKAPI_PTR h_cbti(VkCommandBuffer cb, VkBuffer b, VkImage i, VkImageLayout l, uint32_t n, const VkBufferImageCopy* r) { o_cbti(cb, b, i, m(l), n, r); }
PFN_vkCmdCopyImageToBuffer o_citb = nullptr;
void VKAPI_PTR h_citb(VkCommandBuffer cb, VkImage i, VkImageLayout l, VkBuffer b, uint32_t n, const VkBufferImageCopy* r) { o_citb(cb, i, m(l), b, n, r); }
PFN_vkCmdCopyImage o_ci = nullptr;
void VKAPI_PTR h_ci(VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n, const VkImageCopy* r) { o_ci(cb, s, m(sl), d, m(dl), n, r); }
PFN_vkCmdBlitImage o_bi = nullptr;
void VKAPI_PTR h_bi(VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n, const VkImageBlit* r, VkFilter f) { o_bi(cb, s, m(sl), d, m(dl), n, r, f); }
PFN_vkCmdResolveImage o_ri = nullptr;
void VKAPI_PTR h_ri(VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n, const VkImageResolve* r) { o_ri(cb, s, m(sl), d, m(dl), n, r); }
PFN_vkCmdClearColorImage o_cci = nullptr;
void VKAPI_PTR h_cci(VkCommandBuffer cb, VkImage i, VkImageLayout l, const VkClearColorValue* c, uint32_t n, const VkImageSubresourceRange* r) { o_cci(cb, i, m(l), c, n, r); }
PFN_vkCmdClearDepthStencilImage o_cdsi = nullptr;
void VKAPI_PTR h_cdsi(VkCommandBuffer cb, VkImage i, VkImageLayout l, const VkClearDepthStencilValue* c, uint32_t n, const VkImageSubresourceRange* r) { o_cdsi(cb, i, m(l), c, n, r); }
PFN_vkCmdCopyBufferToImage2 o_cbti2 = nullptr;
void VKAPI_PTR h_cbti2(VkCommandBuffer cb, const VkCopyBufferToImageInfo2* ci) { VkCopyBufferToImageInfo2 c = *ci; c.dstImageLayout = m(c.dstImageLayout); o_cbti2(cb, &c); }
PFN_vkCmdCopyImageToBuffer2 o_citb2 = nullptr;
void VKAPI_PTR h_citb2(VkCommandBuffer cb, const VkCopyImageToBufferInfo2* ci) { VkCopyImageToBufferInfo2 c = *ci; c.srcImageLayout = m(c.srcImageLayout); o_citb2(cb, &c); }
PFN_vkCmdCopyImage2 o_ci2 = nullptr;
void VKAPI_PTR h_ci2(VkCommandBuffer cb, const VkCopyImageInfo2* ci) { VkCopyImageInfo2 c = *ci; c.srcImageLayout = m(c.srcImageLayout); c.dstImageLayout = m(c.dstImageLayout); o_ci2(cb, &c); }
PFN_vkCmdBlitImage2 o_bi2 = nullptr;
void VKAPI_PTR h_bi2(VkCommandBuffer cb, const VkBlitImageInfo2* bi) { VkBlitImageInfo2 c = *bi; c.srcImageLayout = m(c.srcImageLayout); c.dstImageLayout = m(c.dstImageLayout); o_bi2(cb, &c); }
PFN_vkCmdResolveImage2 o_ri2 = nullptr;
void VKAPI_PTR h_ri2(VkCommandBuffer cb, const VkResolveImageInfo2* ri) { VkResolveImageInfo2 c = *ri; c.srcImageLayout = m(c.srcImageLayout); c.dstImageLayout = m(c.dstImageLayout); o_ri2(cb, &c); }

bool image_type(VkDescriptorType t) {
    return t == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER || t == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || t == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ||
           t == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
}
// Descriptor writes whose image layouts need mapping: copied (writes and their image arrays), else null.
struct WriteCopy {
    std::vector<VkWriteDescriptorSet> w;
    std::vector<VkDescriptorImageInfo> ii;
    bool make(uint32_t n, const VkWriteDescriptorSet* src) {
        size_t imgs = 0;
        bool need = false;
        for (uint32_t i = 0; i < n; ++i) {
            if (!image_type(src[i].descriptorType) || !src[i].pImageInfo) continue;
            imgs += src[i].descriptorCount;
            for (uint32_t k = 0; k < src[i].descriptorCount && !need; ++k) need = odd(src[i].pImageInfo[k].imageLayout);
        }
        if (!need) return false;
        w.assign(src, src + n);
        ii.resize(imgs);
        size_t at = 0;
        for (uint32_t i = 0; i < n; ++i) {
            if (!image_type(w[i].descriptorType) || !w[i].pImageInfo) continue;
            VkDescriptorImageInfo* d = ii.data() + at;
            for (uint32_t k = 0; k < w[i].descriptorCount; ++k) { d[k] = w[i].pImageInfo[k]; d[k].imageLayout = m(d[k].imageLayout); }
            w[i].pImageInfo = d;
            at += w[i].descriptorCount;
        }
        return true;
    }
};
PFN_vkUpdateDescriptorSets o_uds = nullptr;
void VKAPI_PTR h_uds(VkDevice dev, uint32_t n, const VkWriteDescriptorSet* w, uint32_t nc, const VkCopyDescriptorSet* c) {
    WriteCopy wc;
    if (!wc.make(n, w)) return o_uds(dev, n, w, nc, c);
    o_uds(dev, n, wc.w.data(), nc, c);
}
PFN_vkCmdPushDescriptorSetKHR o_pds = nullptr;
void VKAPI_PTR h_pds(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout layout, uint32_t set, uint32_t n, const VkWriteDescriptorSet* w) {
    WriteCopy wc;
    if (!wc.make(n, w)) return o_pds(cb, bp, layout, set, n, w);
    o_pds(cb, bp, layout, set, n, wc.w.data());
}

// Again after a later volkLoadDevice (tests bring devices up and down): the slot then holds the new device's pointer.
template <typename P> void hook(P& slot, P& orig, P fn) { if (slot && slot != fn) { orig = slot; slot = fn; } }

}  // namespace

void unified_layouts_install(bool on) {
    g_unified_layouts = on;
    if (!on) return;
    hook(vkCmdPipelineBarrier2, o_pb2, &h_pb2);
    hook(vkCmdPipelineBarrier, o_pb, &h_pb);
    hook(vkCmdBeginRendering, o_br, &h_br);
    hook(vkCmdCopyBufferToImage, o_cbti, &h_cbti);
    hook(vkCmdCopyImageToBuffer, o_citb, &h_citb);
    hook(vkCmdCopyImage, o_ci, &h_ci);
    hook(vkCmdBlitImage, o_bi, &h_bi);
    hook(vkCmdResolveImage, o_ri, &h_ri);
    hook(vkCmdClearColorImage, o_cci, &h_cci);
    hook(vkCmdClearDepthStencilImage, o_cdsi, &h_cdsi);
    hook(vkCmdCopyBufferToImage2, o_cbti2, &h_cbti2);
    hook(vkCmdCopyImageToBuffer2, o_citb2, &h_citb2);
    hook(vkCmdCopyImage2, o_ci2, &h_ci2);
    hook(vkCmdBlitImage2, o_bi2, &h_bi2);
    hook(vkCmdResolveImage2, o_ri2, &h_ri2);
    hook(vkUpdateDescriptorSets, o_uds, &h_uds);
    hook(vkCmdPushDescriptorSetKHR, o_pds, &h_pds);
}

}  // namespace nxrt::gfx
