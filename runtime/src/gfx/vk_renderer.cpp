// SPDX-License-Identifier: GPL-3.0-or-later
// Renderer (declarations in include/nxrt/gfx.h): swapchain or offscreen image ring, present modes, frames in flight, GPU
// timestamps, resize and minimise handling, screenshots, overlay hook.
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_surface.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <thread>

#include "nxrt/config.h"
#include "nxrt/ftrace.h"
#include "nxrt/gfx.h"
#include "nxrt/instance.h"
#include "nxrt/log.h"
#include "nxrt/lowlat.h"
#include "nxrt/platform.h"
#include "extras/hdr_capture.h"
#include "extras/hdr_math.h"
#include "extras/win_display.h"
#include "vk_gauges.h"

namespace nxrt::tt { extern void (*wait_shot_writes)(); }   // tt_core.cpp

namespace nxrt::gfx {
namespace {
constexpr const char* kTag = "gfx";

struct ImageState {   // a barrier helper description
    VkImageLayout layout;
    VkPipelineStageFlags2 stage;
    VkAccessFlags2 access;
};

void barrier(VkCommandBuffer cmd, VkImage img, const ImageState& from, const ImageState& to) {
    VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = from.stage;
    b.srcAccessMask = from.access;
    b.dstStageMask = to.stage;
    b.dstAccessMask = to.access;
    b.oldLayout = from.layout;
    b.newLayout = to.layout;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &di);
}

const ImageState kUndefined = {VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0};
const ImageState kColorAtt = {VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                              VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT};
const ImageState kTransferSrc = {VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT};
const ImageState kPresent = {VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_NONE, 0};

}  // namespace

// ---- names ------------------------------------------------------------------------------------------------------------
const char* present_mode_name(PresentMode m) {
    switch (m) {
        case PresentMode::Fifo: return "fifo";
        case PresentMode::FifoRelaxed: return "fifo_relaxed";
        case PresentMode::Mailbox: return "mailbox";
        case PresentMode::Immediate: return "immediate";
    }
    return "?";
}

bool parse_present_mode(const char* s, PresentMode* out) {
    if (!s) return false;
    std::string v;
    for (const char* p = s; *p; ++p) v += static_cast<char>(std::tolower(static_cast<unsigned char>(*p)));
    for (PresentMode m : {PresentMode::Fifo, PresentMode::FifoRelaxed, PresentMode::Mailbox, PresentMode::Immediate})
        if (v == present_mode_name(m)) { *out = m; return true; }
    return false;
}

VkPresentModeKHR to_vk(PresentMode m) {
    switch (m) {
        case PresentMode::Fifo: return VK_PRESENT_MODE_FIFO_KHR;
        case PresentMode::FifoRelaxed: return VK_PRESENT_MODE_FIFO_RELAXED_KHR;
        case PresentMode::Mailbox: return VK_PRESENT_MODE_MAILBOX_KHR;
        case PresentMode::Immediate: return VK_PRESENT_MODE_IMMEDIATE_KHR;
    }
    return VK_PRESENT_MODE_FIFO_KHR;
}

// ---- PNG helpers ------------------------------------------------------------------------------------------------------
std::atomic<int> g_png_writes{0};   // screenshots requested and not yet written to disk

// Waits (at most 15 s) for screenshot files still being written; called before the test runner exits.
void wait_screenshot_writes() {
    for (int i = 0; i < 1500 && g_png_writes.load() > 0; i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
}
const bool g_wait_hook_set = (nxrt::tt::wait_shot_writes = wait_screenshot_writes, true);

bool save_png_rgba(const std::string& path, const uint8_t* rgba, uint32_t w, uint32_t h) {
    SDL_Surface* s = SDL_CreateSurfaceFrom(static_cast<int>(w), static_cast<int>(h), SDL_PIXELFORMAT_RGBA32, const_cast<uint8_t*>(rgba),
                                           static_cast<int>(w * 4));
    if (!s) return false;
    const bool ok = SDL_SavePNG(s, path.c_str());
    SDL_DestroySurface(s);
    if (!ok) NXLOG_E(kTag, "could not write %s: %s", path.c_str(), SDL_GetError());
    return ok;
}

bool load_png_rgba(const std::string& path, std::vector<uint8_t>* rgba, uint32_t* w, uint32_t* h) {
    SDL_Surface* s = SDL_LoadPNG(path.c_str());
    if (!s) return false;
    SDL_Surface* c = SDL_ConvertSurface(s, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(s);
    if (!c) return false;
    *w = static_cast<uint32_t>(c->w);
    *h = static_cast<uint32_t>(c->h);
    rgba->resize(static_cast<size_t>(c->w) * c->h * 4);
    for (int y = 0; y < c->h; ++y) std::memcpy(rgba->data() + static_cast<size_t>(y) * c->w * 4, static_cast<uint8_t*>(c->pixels) + static_cast<size_t>(y) * c->pitch, static_cast<size_t>(c->w) * 4);
    SDL_DestroySurface(c);
    return true;
}

ImageDiff compare_rgba(const uint8_t* a, uint32_t aw, uint32_t ah, const uint8_t* b, uint32_t bw, uint32_t bh) {
    ImageDiff d;
    d.width = aw;
    d.height = ah;
    d.same_size = aw == bw && ah == bh;
    if (!d.same_size) return d;
    const size_t px = static_cast<size_t>(aw) * ah;
    double sum = 0;
    size_t over = 0;
    for (size_t i = 0; i < px; ++i) {
        int worst = 0;
        for (int c = 0; c < 3; ++c) {
            const int df = std::abs(static_cast<int>(a[i * 4 + c]) - static_cast<int>(b[i * 4 + c]));
            sum += df;
            worst = std::max(worst, df);
        }
        d.max_abs = std::max(d.max_abs, worst);
        if (worst > 8) ++over;
    }
    d.mean_abs = px ? sum / (3.0 * static_cast<double>(px)) : 0;
    d.fraction_over_8 = px ? static_cast<double>(over) / static_cast<double>(px) : 0;
    return d;
}

// ---- renderer ---------------------------------------------------------------------------------------------------------
struct Renderer::Impl {
    Device* dev = nullptr;
    RendererDesc desc;
    bool headless = false;

    PresentMode requested = PresentMode::Fifo, active = PresentMode::Fifo;
    std::vector<VkPresentModeKHR> supported_modes;
    bool capture_supported = true;

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_B8G8R8A8_UNORM;
    // display extras (S14)
    extras::OutputKind wanted = extras::OutputKind::Sdr8;     // what the settings and the surface asked for at creation
    extras::OutputKind kind = extras::OutputKind::Sdr8;       // what the swapchain in use carries
    VkColorSpaceKHR color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    uint32_t format_serial = 0;
    std::unique_ptr<extras::LowLatency> latency;
    VkExtent2D extent = {0, 0};
    uint32_t headless_w = 0, headless_h = 0;
    struct Target {
        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkSemaphore render_done = VK_NULL_HANDLE;
        Image owned;    // headless images
    };
    std::vector<Target> targets;
    bool dirty = true;
    int last_px_w = -1, last_px_h = -1;

    struct Slot {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VkSemaphore acquire = VK_NULL_HANDLE;
        VkQueryPool queries = VK_NULL_HANDLE;
        bool ts_pending = false;
    };
    std::vector<Slot> slots;
    uint32_t cur_slot = 0, cur_image = 0;
    uint64_t frame_number = 0;
    bool in_frame = false;
    FrameInfo info;

    std::function<void(const FrameInfo&)> overlay;

    // pending capture
    // set by the thread that asks for a screenshot, read by end_frame and by capture_pending() (overlay, presenting thread)
    std::atomic<bool> cap_pending{false};
    std::string cap_path;
    std::vector<uint8_t>* cap_out = nullptr;
    uint32_t* cap_w = nullptr;
    uint32_t* cap_h = nullptr;

    RendererStats st;

    // latency probe (S18): with the frame trace on, the present submission also signals a timeline semaphore and a watcher thread
    // marks "lat.gpudone" (arg = frame id) when the GPU has finished that frame's present command buffer (and so, in queue order, the frame).
    VkSemaphore probe_sem = VK_NULL_HANDLE;
    uint64_t probe_next = 0;
    uint64_t probe_ids[256] = {};
    std::thread probe_th;
    std::atomic<bool> probe_stop{false};
    std::atomic<uint64_t> probe_published{0};

    void probe_init() {
        if (probe_sem || headless || !ft::on()) return;
        VkSemaphoreTypeCreateInfo tci = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        tci.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        VkSemaphoreCreateInfo ci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &tci};
        if (vkCreateSemaphore(vk(), &ci, nullptr, &probe_sem) != VK_SUCCESS) { probe_sem = VK_NULL_HANDLE; return; }
        vk_gauge_semaphores().add(1);
        probe_th = std::thread([this] {
            log_set_thread_name("lat-watch");
            static const uint16_t z = ft::zone_id("lat.gpudone");
            uint64_t v = 1;
            while (!probe_stop.load()) {
                if (probe_published.load() < v) { std::this_thread::sleep_for(std::chrono::microseconds(200)); continue; }
                VkSemaphoreWaitInfo wi = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
                wi.semaphoreCount = 1;
                wi.pSemaphores = &probe_sem;
                wi.pValues = &v;
                if (vkWaitSemaphores(vk(), &wi, 50ull * 1000 * 1000) != VK_SUCCESS) continue;
                const uint64_t t = ft::now_ns();
                ft::event(z, t, t, static_cast<uint32_t>(probe_ids[v % 256]));
                ++v;
            }
        });
    }
    void probe_stop_join() {
        if (probe_th.joinable()) { probe_stop.store(true); probe_th.join(); }
        if (probe_sem) { vkDestroySemaphore(vk(), probe_sem, nullptr); vk_gauge_semaphores().sub(1); probe_sem = VK_NULL_HANDLE; }
    }

    // [gfx] screenshot_async (default on): a PNG screenshot no longer waits on the render thread for the GPU to finish the frame (a full
    // queue drain, 10-30 ms frames in the benchmark scenes' @shot frames). The present submission also signals a timeline semaphore and a
    // worker waits for it, reads the copy back and writes the file.
    VkSemaphore shot_sem = VK_NULL_HANDLE;
    uint64_t shot_next = 0;
    std::atomic<int> shot_inflight{0};
    bool shot_sem_ready() {
        if (shot_sem) return true;
        VkSemaphoreTypeCreateInfo tci = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        tci.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        VkSemaphoreCreateInfo ci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &tci};
        if (vkCreateSemaphore(vk(), &ci, nullptr, &shot_sem) != VK_SUCCESS) { shot_sem = VK_NULL_HANDLE; return false; }
        vk_gauge_semaphores().add(1);
        return true;
    }
    void shot_stop() {
        for (int i = 0; i < 1000 && shot_inflight.load() > 0; i++) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (shot_sem && shot_inflight.load() == 0) { vkDestroySemaphore(vk(), shot_sem, nullptr); vk_gauge_semaphores().sub(1); shot_sem = VK_NULL_HANDLE; }
    }

    VkDevice vk() const { return dev->device(); }

    // -- present mode resolution
    PresentMode resolve(PresentMode want) const {
        if (headless) return want;
        auto has = [&](PresentMode m) { return std::find(supported_modes.begin(), supported_modes.end(), to_vk(m)) != supported_modes.end(); };
        if (has(want)) return want;
        switch (want) {
            case PresentMode::Mailbox: if (has(PresentMode::Immediate)) return PresentMode::Immediate; break;
            case PresentMode::Immediate: if (has(PresentMode::Mailbox)) return PresentMode::Mailbox; break;
            default: break;
        }
        return PresentMode::Fifo;
    }

    // Records the swapchain flavour in use; publishes the change.
    void set_kind(extras::OutputKind k, VkColorSpaceKHR cs, VkFormat f) {
        const bool changed = k != kind || f != format || cs != color_space;
        kind = k;
        color_space = cs;
        format = f;
        if (changed) {
            ++format_serial;
            extras::HdrState hs = extras::hdr_state();
            hs.output = k;
            if (k == extras::OutputKind::Sdr8) hs.peak_ratio = 1.0f;
            extras::hdr_set_state(hs);
        }
    }

    // The swapchain's HDR metadata (VK_EXT_hdr_metadata): the display's peak; in [hdr] mode = native the content levels are the game's own
    // declarations (NVN window content metadata, else the nn::vi extended content range). Sent again when HdrState::metadata_serial moves.
    uint32_t md_serial = 0;
    void apply_hdr_metadata() {
        const extras::HdrState hs = extras::hdr_state();
        md_serial = hs.metadata_serial;
        if (headless || !swapchain || kind == extras::OutputKind::Sdr8 || !dev->features().hdr_metadata) return;
        const bool native = extras::settings().hdr.mode == extras::HdrMode::Native;
        VkHdrMetadataEXT md = {VK_STRUCTURE_TYPE_HDR_METADATA_EXT};
        md.displayPrimaryRed = {0.708f, 0.292f};       // BT.2020
        md.displayPrimaryGreen = {0.170f, 0.797f};
        md.displayPrimaryBlue = {0.131f, 0.046f};
        md.whitePoint = {0.3127f, 0.3290f};
        md.maxLuminance = hs.peak_nits;
        md.minLuminance = 0.0f;
        md.maxContentLightLevel = hs.peak_nits;
        md.maxFrameAverageLightLevel = std::max(hs.paper_white_nits, hs.peak_nits * 0.25f);
        if (native) {
            const float cll = hs.game_max_cll > 0 ? hs.game_max_cll : hs.game_extended_nits;
            if (cll > 0) md.maxContentLightLevel = std::min(std::max(cll, hs.paper_white_nits), hs.peak_nits);
            if (hs.game_max_fall > 0) md.maxFrameAverageLightLevel = std::min(hs.game_max_fall, md.maxContentLightLevel);
            else md.maxFrameAverageLightLevel = std::min(md.maxFrameAverageLightLevel, md.maxContentLightLevel);
        }
        vkSetHdrMetadataEXT(vk(), 1, &swapchain, &md);
    }

    // HDR decision (S14): from [hdr] and what the surface offers. Publishes the HdrState (scene_hdr, levels, peak ratio).
    void decide_output() {
        const extras::Settings es = extras::settings();
        const extras::HdrState prev = extras::hdr_state();
        extras::HdrState hs;
        // what the game declared so far (nn::vi layer ranges, NVN window HDR) survives a new decision
        hs.game_standard_nits = prev.game_standard_nits;
        hs.game_extended_nits = prev.game_extended_nits;
        hs.game_color_space = prev.game_color_space;
        hs.game_max_cll = prev.game_max_cll;
        hs.game_max_fall = prev.game_max_fall;
        hs.game_display_max_nits = prev.game_display_max_nits;
        hs.metadata_serial = prev.metadata_serial + 1;
        const bool forced = es.hdr.mode == extras::HdrMode::On || es.hdr.mode == extras::HdrMode::Native;   // Native: the game's HDR TV path, forced like On
        wanted = extras::OutputKind::Sdr8;
        if (es.hdr.mode != extras::HdrMode::Off) {
            bool o10 = false, osc = false;
            extras::DisplayHdrInfo di;
            if (headless) {
                o10 = osc = forced;         // an offscreen image can be anything; only a forced mode asks for HDR
            } else {
                uint32_t n = 0;
                vkGetPhysicalDeviceSurfaceFormatsKHR(dev->physical(), dev->surface(), &n, nullptr);
                std::vector<VkSurfaceFormatKHR> fmts(n);
                vkGetPhysicalDeviceSurfaceFormatsKHR(dev->physical(), dev->surface(), &n, fmts.data());
                for (auto& f : fmts) {
                    if (f.colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT && (f.format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 || f.format == VK_FORMAT_A2R10G10B10_UNORM_PACK32)) o10 = true;
                    if (f.colorSpace == VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT && f.format == VK_FORMAT_R16G16B16A16_SFLOAT) osc = true;
                }
                extras::query_display_hdr(platform::native_hwnd(), &di);
                NXLOG_I(kTag, "HDR: surface offers HDR10 %s, scRGB %s; display %s: hdr %s, peak %.0f nits, SDR white %.0f nits (%s)", o10 ? "yes" : "no", osc ? "yes" : "no",
                        di.monitor, di.hdr_enabled ? "on" : "off", di.max_nits, di.sdr_white_nits, di.how);
            }
            if (!headless && es.hdr.mode == extras::HdrMode::Auto && !(di.valid && di.hdr_enabled)) {
                // the NVIDIA driver lists HDR10 and scRGB surface formats whether or not Windows HDR is on for the monitor, so "auto" needs Windows' own answer
                if (o10 || osc) NXLOG_I(kTag, "HDR: the surface lists HDR formats but Windows does not report HDR on for this monitor (%s): SDR output (set [hdr] mode = on to force HDR)", di.how);
                o10 = osc = false;
            }
            const bool pick10 = es.hdr.format == extras::HdrFormat::Hdr10 ? o10 : es.hdr.format == extras::HdrFormat::ScRgb ? false : o10;
            const bool pickSc = es.hdr.format == extras::HdrFormat::ScRgb ? osc : es.hdr.format == extras::HdrFormat::Hdr10 ? false : (!o10 && osc);
            if (pick10) wanted = extras::OutputKind::Hdr10;
            else if (pickSc) wanted = extras::OutputKind::ScRgb;
            hs.scene_hdr = wanted != extras::OutputKind::Sdr8 || forced;
            hs.display_hdr = di.hdr_enabled;
            hs.display_peak_nits = di.valid ? di.max_nits : 0.0f;
            hs.display_sdr_white_nits = di.valid ? di.sdr_white_nits : 80.0f;
            extras::hdr_resolve_levels(es.hdr, di.valid && di.hdr_enabled, di.valid ? di.max_nits : 0.0f, di.sdr_white_nits, &hs.paper_white_nits, &hs.peak_nits);
            if (es.hdr.mode == extras::HdrMode::Native && es.hdr.paper_white_nits <= 0 && hs.game_standard_nits >= 80.0f) {
                hs.paper_white_nits = std::clamp(hs.game_standard_nits, 80.0f, 500.0f);
                hs.peak_nits = std::max(hs.peak_nits, hs.paper_white_nits);
            }
            hs.peak_ratio = wanted == extras::OutputKind::Sdr8 ? 1.0f : extras::peak_ratio(hs.peak_nits, hs.paper_white_nits, static_cast<float>(es.hdr.highlight_boost));
        }
        hs.output = extras::OutputKind::Sdr8;     // the swapchain sets the real one
        extras::hdr_set_state(hs);
        NXLOG_I(kTag, "HDR: mode %d, scene %s, wanted output %s, paper white %.0f nits, peak %.0f nits, peak ratio %.2f", static_cast<int>(es.hdr.mode), hs.scene_hdr ? "HDR" : "SDR",
                extras::output_kind_name(wanted), hs.paper_white_nits, hs.peak_nits, hs.peak_ratio);
    }

    void destroy_targets() {
        for (auto& t : targets) {
            if (t.render_done) { vkDestroySemaphore(vk(), t.render_done, nullptr); vk_gauge_semaphores().sub(1); }
            if (headless) {
                dev->destroy_image(t.owned);
            } else if (t.view) {
                vkDestroyImageView(vk(), t.view, nullptr);
                vk_gauge_image_views().sub(1);
            }
        }
        targets.clear();
    }

    bool create_semaphore(VkSemaphore* s) {
        VkSemaphoreCreateInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        const bool ok = vkCreateSemaphore(vk(), &si, nullptr, s) == VK_SUCCESS;
        if (ok) vk_gauge_semaphores().add(1);
        return ok;
    }

    bool recreate(uint32_t want_w, uint32_t want_h) {
        dev->wait_idle();
        if (headless) {
            destroy_targets();
            extent = {want_w, want_h};
            VkFormat want_fmt = VK_FORMAT_B8G8R8A8_UNORM;
            extras::OutputKind got = extras::OutputKind::Sdr8;
            if (wanted != extras::OutputKind::Sdr8) {       // HDR output into an offscreen image (tests, scripted runs, screenshots of the HDR path)
                ImageDesc probe;
                probe.format = wanted == extras::OutputKind::Hdr10 ? VK_FORMAT_A2B10G10R10_UNORM_PACK32 : VK_FORMAT_R16G16B16A16_SFLOAT;
                probe.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
                if (dev->image_supported(probe)) { want_fmt = probe.format; got = wanted; }
            }
            set_kind(got, got == extras::OutputKind::Hdr10 ? VK_COLOR_SPACE_HDR10_ST2084_EXT : got == extras::OutputKind::ScRgb ? VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT : VK_COLOR_SPACE_SRGB_NONLINEAR_KHR, want_fmt);
            const uint32_t count = desc.frames_in_flight + 1;
            for (uint32_t i = 0; i < count; ++i) {
                ImageDesc id;
                id.format = format;
                id.width = want_w;
                id.height = want_h;
                id.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
                id.name = "headless color";
                Target t;
                std::string why;
                if (dev->create_image(id, &t.owned, &why) != VK_SUCCESS) {
                    NXLOG_E(kTag, "offscreen image: %s", why.c_str());
                    return false;
                }
                t.image = t.owned.image;
                t.view = t.owned.view;
                targets.push_back(t);
            }
            active = requested;
            dirty = false;
            st.swapchain_creates++;
            NXLOG_I(kTag, "offscreen targets: %u x %ux%u", count, want_w, want_h);
            return true;
        }

        VkPhysicalDevice pd = dev->physical();
        VkSurfaceKHR surf = dev->surface();
        VkSurfaceCapabilitiesKHR caps;
        if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surf, &caps) != VK_SUCCESS) return false;
        uint32_t n = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(pd, surf, &n, nullptr);
        supported_modes.assign(n, VK_PRESENT_MODE_FIFO_KHR);
        vkGetPhysicalDeviceSurfacePresentModesKHR(pd, surf, &n, supported_modes.data());
        active = resolve(requested);

        vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surf, &n, nullptr);
        std::vector<VkSurfaceFormatKHR> fmts(n);
        vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surf, &n, fmts.data());
        VkSurfaceFormatKHR chosen = fmts.empty() ? VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR} : fmts[0];
        extras::OutputKind got = extras::OutputKind::Sdr8;
        if (wanted != extras::OutputKind::Sdr8) {     // HDR: the first of the wanted flavour, then the other one, then SDR
            auto find = [&](VkFormat f, VkColorSpaceKHR cs) {
                for (auto& x : fmts) if (x.format == f && x.colorSpace == cs) { chosen = x; return true; }
                return false;
            };
            const auto hdr10 = [&] { return find(VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_COLOR_SPACE_HDR10_ST2084_EXT) || find(VK_FORMAT_A2R10G10B10_UNORM_PACK32, VK_COLOR_SPACE_HDR10_ST2084_EXT); };
            const auto scrgb = [&] { return find(VK_FORMAT_R16G16B16A16_SFLOAT, VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT); };
            if (wanted == extras::OutputKind::Hdr10) got = hdr10() ? extras::OutputKind::Hdr10 : scrgb() ? extras::OutputKind::ScRgb : extras::OutputKind::Sdr8;
            else got = scrgb() ? extras::OutputKind::ScRgb : hdr10() ? extras::OutputKind::Hdr10 : extras::OutputKind::Sdr8;
            if (got == extras::OutputKind::Sdr8)
                NXLOG_W(kTag, "HDR output was chosen but the surface does not offer it any more (Windows HDR off?): the picture is shown as SDR");
        }
        if (got == extras::OutputKind::Sdr8) {
            for (VkFormat want : {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM}) {
                bool found = false;
                for (auto& f : fmts) if (f.format == want && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { chosen = f; found = true; break; }
                if (found) break;
            }
        }
        VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        capture_supported = (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
        if (capture_supported) usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        {   // the swapchain format goes through the same check as every image
            ImageDesc id;
            id.format = chosen.format;
            id.usage = usage;
            std::string why;
            if (!dev->image_supported(id, &why)) {
                NXLOG_E(kTag, "swapchain format refused: %s", why.c_str());
                return false;
            }
        }

        VkExtent2D ext = caps.currentExtent;
        if (ext.width == 0xFFFFFFFFu) {
            ext.width = std::clamp(want_w, caps.minImageExtent.width, caps.maxImageExtent.width);
            ext.height = std::clamp(want_h, caps.minImageExtent.height, caps.maxImageExtent.height);
        }
        if (ext.width == 0 || ext.height == 0) { extent = {0, 0}; return false; }

        uint32_t images = std::max(caps.minImageCount + (active == PresentMode::Mailbox ? 1u : 0u), desc.frames_in_flight + 1);
        if (caps.maxImageCount) images = std::min(images, caps.maxImageCount);

        VkSwapchainKHR old = swapchain;
        VkSwapchainCreateInfoKHR ci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        ci.surface = surf;
        ci.minImageCount = images;
        ci.imageFormat = chosen.format;
        ci.imageColorSpace = chosen.colorSpace;
        ci.imageExtent = ext;
        ci.imageArrayLayers = 1;
        ci.imageUsage = usage;
        ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ci.preTransform = caps.currentTransform;
        ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        ci.presentMode = to_vk(active);
        ci.clipped = VK_TRUE;
        ci.oldSwapchain = old;
        if (latency) latency->prepare_swapchain(&ci);
        VkSwapchainKHR fresh = VK_NULL_HANDLE;
        VkResult r = vkCreateSwapchainKHR(vk(), &ci, nullptr, &fresh);
        if (r == VK_SUCCESS) vk_gauge_swapchains().add(1);
        destroy_targets();
        if (old) {
            if (latency) latency->swapchain_destroyed(old);
            vkDestroySwapchainKHR(vk(), old, nullptr);
            vk_gauge_swapchains().sub(1);
        }
        swapchain = VK_NULL_HANDLE;
        if (r != VK_SUCCESS) {
            NXLOG_E(kTag, "vkCreateSwapchainKHR failed (%d)", static_cast<int>(r));
            return false;
        }
        swapchain = fresh;
        set_kind(got, chosen.colorSpace, chosen.format);
        extent = ext;
        if (latency) latency->swapchain_created(swapchain);
        apply_hdr_metadata();
        uint32_t count = 0;
        vkGetSwapchainImagesKHR(vk(), swapchain, &count, nullptr);
        std::vector<VkImage> imgs(count);
        vkGetSwapchainImagesKHR(vk(), swapchain, &count, imgs.data());
        for (VkImage im : imgs) {
            Target t;
            t.image = im;
            VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            vi.image = im;
            vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vi.format = format;
            vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            if (vkCreateImageView(vk(), &vi, nullptr, &t.view) != VK_SUCCESS) return false;
            vk_gauge_image_views().add(1);
            if (!create_semaphore(&t.render_done)) return false;
            targets.push_back(t);
        }
        dirty = false;
        st.swapchain_creates++;
        NXLOG_I(kTag, "swapchain %ux%u, %u images, format %d, color space %d (%s), present mode %s%s", ext.width, ext.height, count, static_cast<int>(format),
                static_cast<int>(color_space), extras::output_kind_name(kind), present_mode_name(active), active != requested ? " (requested mode not available)" : "");
        return true;
    }

    void read_timestamps(Slot& s) {
        if (!s.ts_pending) return;
        s.ts_pending = false;
        uint64_t ts[2] = {};
        if (vkGetQueryPoolResults(vk(), s.queries, 0, 2, sizeof ts, ts, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) return;
        const uint32_t bits = 64;   // timestampValidBits is queried by the device; assume a full counter and mask below
        (void)bits;
        const double ms = static_cast<double>(ts[1] - ts[0]) * static_cast<double>(dev->timestamp_period_ns()) * 1e-6;
        if (ms < 0 || ms > 1000.0) return;
        st.gpu_ms_last = ms;
        st.gpu_ms_avg = st.gpu_ms_avg == 0 ? ms : st.gpu_ms_avg * 0.95 + ms * 0.05;
    }

    bool create_slots() {
        for (uint32_t i = 0; i < desc.frames_in_flight; ++i) {
            Slot s;
            VkCommandPoolCreateInfo pi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            pi.queueFamilyIndex = dev->queue_family();
            if (vkCreateCommandPool(vk(), &pi, nullptr, &s.pool) != VK_SUCCESS) return false;
            vk_gauge_command_pools().add(1);
            VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            ai.commandPool = s.pool;
            ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            ai.commandBufferCount = 1;
            if (vkAllocateCommandBuffers(vk(), &ai, &s.cmd) != VK_SUCCESS) return false;
            vk_gauge_command_buffers().add(1);
            VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            if (vkCreateFence(vk(), &fi, nullptr, &s.fence) != VK_SUCCESS) return false;
            vk_gauge_fences().add(1);
            if (!create_semaphore(&s.acquire)) return false;
            if (dev->timestamps_supported()) {
                VkQueryPoolCreateInfo qi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
                qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
                qi.queryCount = 2;
                if (vkCreateQueryPool(vk(), &qi, nullptr, &s.queries) != VK_SUCCESS) return false;
                vk_gauge_query_pools().add(1);
            }
            slots.push_back(s);
        }
        return true;
    }

    // [gfx] flash_watch (diagnostic, 0 = off): every presented picture is read back (one buffer per slot) and compared with the
    // pictures before and after it; a frame in which more than this share of a sample grid is much brighter than both neighbours
    // (a short flash) is logged and written as PNGs with its neighbours (<flash_dir>/flash_<frame>_{a,b,c}.png).
    struct FlashSlot { Buffer buf; uint64_t frame = 0; bool valid = false; };
    std::vector<FlashSlot> flash;
    struct FlashFrame { std::vector<uint8_t> px; std::vector<uint8_t> luma; uint64_t frame = ~0ull; };
    FlashFrame flash_ring[4];
    uint32_t flash_n = 0;        // frames in the ring (consecutive)
    uint32_t flash_w = 0, flash_h = 0;
    uint32_t flash_saved = 0;
    void flash_reset() { flash_n = 0; for (auto& f : flash_ring) f.frame = ~0ull; }
    double flash_max = 0.0;
    uint64_t flash_max_frame = 0, flash_window = 0;
    // the read-back picture at a quarter of its width and height as 8-bit RGBA (HDR10 formats keep their encoded top 8 bits: enough
    // to look at), plus its luma
    void flash_small(const uint8_t* src, FlashFrame& f) const {
        const uint32_t gw = flash_w / 4, gh = flash_h / 4;
        f.px.resize(static_cast<size_t>(gw) * gh * 4);
        f.luma.resize(static_cast<size_t>(gw) * gh);
        for (uint32_t y = 0; y < gh; ++y)
            for (uint32_t x = 0; x < gw; ++x) {
                const uint8_t* q = src + (static_cast<size_t>(y) * 4 * flash_w + x * 4) * 4;
                uint32_t v;
                std::memcpy(&v, q, 4);
                uint8_t r, g, b;
                if (format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB) { b = q[0]; g = q[1]; r = q[2]; }
                else if (format == VK_FORMAT_A2B10G10R10_UNORM_PACK32) { r = (v >> 2) & 0xff; g = (v >> 12) & 0xff; b = (v >> 22) & 0xff; }
                else if (format == VK_FORMAT_A2R10G10B10_UNORM_PACK32) { b = (v >> 2) & 0xff; g = (v >> 12) & 0xff; r = (v >> 22) & 0xff; }
                else { r = q[0]; g = q[1]; b = q[2]; }
                const size_t i = static_cast<size_t>(y) * gw + x;
                f.px[i * 4] = r; f.px[i * 4 + 1] = g; f.px[i * 4 + 2] = b; f.px[i * 4 + 3] = 255;
                f.luma[i] = static_cast<uint8_t>((r * 2 + g * 5 + b) >> 3);
            }
    }
    // A flash: a sample at least 30 levels brighter than in both neighbours, which agree with each other (within 12): camera motion
    // and cuts change the neighbours too, a frame that misses or adds one pass does not.
    void flash_take(FlashSlot& fs) {
        static const double th = config_get_double("gfx", "flash_watch", 0.0);
        static const std::string dir = config_get_str("gfx", "flash_dir", "flashes");
        dev->invalidate_buffer(fs.buf);
        const uint64_t fr = fs.frame;
        FlashFrame& f = flash_ring[fr & 3];
        flash_small(static_cast<const uint8_t*>(fs.buf.mapped), f);
        f.frame = fr;
        auto have = [&](uint64_t k) { return fr >= k && flash_ring[(fr - k) & 3].frame == fr - k; };
        if (!have(1) || !have(2)) return;
        const FlashFrame& c = f;
        const FlashFrame& b = flash_ring[(fr - 1) & 3];
        const FlashFrame& a = flash_ring[(fr - 2) & 3];
        const size_t n = c.luma.size();
        size_t s1 = 0, s2 = 0;
        const bool z_ok = have(3);
        const FlashFrame& z = flash_ring[(fr - 3) & 3];
        for (size_t i = 0; i < n; ++i) {
            const int lb = b.luma[i], la = a.luma[i], lc = c.luma[i];
            if (lb - std::max(la, lc) > 30 && std::abs(la - lc) < 12) ++s1;
            if (z_ok) {
                const int lz = z.luma[i];
                const int lo = std::min(la, lb), hi = std::max(lz, lc);
                if (lo - hi > 30 && std::abs(lz - lc) < 12) ++s2;
            }
        }
        const double sh1 = static_cast<double>(s1) / static_cast<double>(n), sh2 = static_cast<double>(s2) / static_cast<double>(n);
        const double sh = std::max(sh1, sh2);
        if (sh > flash_max) { flash_max = sh; flash_max_frame = sh1 >= sh2 ? fr - 1 : fr - 2; }
        if (++flash_window >= 600) {
            NXLOG_I(kTag, "flash watch: largest flash share over the last %llu frames %.5f (frame %llu)", static_cast<unsigned long long>(flash_window),
                    flash_max, static_cast<unsigned long long>(flash_max_frame));
            flash_window = 0;
            flash_max = 0.0;
            flash_max_frame = 0;
        }
        if (sh <= th) return;
        const uint64_t at = sh1 >= sh2 ? fr - 1 : fr - 2;
        NXLOG_W(kTag, "flash watch: frame %llu %s brighter than the frames around it in %.3f%% of the picture", static_cast<unsigned long long>(at),
                sh1 >= sh2 ? "is" : "and the next are", sh * 100.0);
        if (flash_saved >= 300) return;
        ++flash_saved;
        std::vector<std::vector<uint8_t>> pics;
        std::vector<std::string> names;
        const uint64_t first = sh1 >= sh2 ? fr - 2 : fr - 3;
        for (uint64_t k = first; k <= fr; ++k) {
            char name[96];
            std::snprintf(name, sizeof name, "/flash_%06llu_%llu.png", static_cast<unsigned long long>(at), static_cast<unsigned long long>(k));
            pics.push_back(flash_ring[k & 3].px);
            names.push_back(instance_file(dir + name));
        }
        g_png_writes.fetch_add(1);
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(names[0]).parent_path(), ec);
        std::thread([pics = std::move(pics), names = std::move(names), w = flash_w / 4, h = flash_h / 4]() {
            for (size_t i = 0; i < pics.size(); ++i) save_png_rgba(names[i], pics[i].data(), w, h);
            g_png_writes.fetch_sub(1);
        }).detach();
    }    void flash_free() {
        for (auto& f : flash) if (f.buf) dev->destroy_buffer(f.buf);
        flash.clear();
        flash_n = 0;
    }

    void destroy_all() {
        dev->wait_idle();
        probe_stop_join();
        shot_stop();
        flash_free();
        for (auto& s : slots) {
            if (s.queries) { vkDestroyQueryPool(vk(), s.queries, nullptr); vk_gauge_query_pools().sub(1); }
            if (s.acquire) { vkDestroySemaphore(vk(), s.acquire, nullptr); vk_gauge_semaphores().sub(1); }
            if (s.fence) { vkDestroyFence(vk(), s.fence, nullptr); vk_gauge_fences().sub(1); }
            if (s.pool) {   // frees its command buffer
                vkDestroyCommandPool(vk(), s.pool, nullptr);
                vk_gauge_command_pools().sub(1);
                if (s.cmd) vk_gauge_command_buffers().sub(1);
            }
        }
        slots.clear();
        destroy_targets();
        if (swapchain) { vkDestroySwapchainKHR(vk(), swapchain, nullptr); vk_gauge_swapchains().sub(1); }
        swapchain = VK_NULL_HANDLE;
    }
};

std::unique_ptr<Renderer> Renderer::create(Device& dev, const RendererDesc& desc_in, std::string* error) {
    std::unique_ptr<Renderer> r(new Renderer());
    r->impl_ = new Impl();
    Impl& s = *r->impl_;
    s.dev = &dev;
    s.desc = desc_in;
    s.desc.frames_in_flight = static_cast<uint32_t>(std::clamp<int64_t>(config_get_int("gfx", "frames_in_flight", desc_in.frames_in_flight), 1, 3));
    PresentMode pm = desc_in.present_mode;
    const std::string cfg_pm = config_get_str("gfx", "present_mode", "");
    if (!cfg_pm.empty() && !parse_present_mode(cfg_pm.c_str(), &pm)) NXLOG_W(kTag, "unknown [gfx] present_mode '%s'", cfg_pm.c_str());
    s.requested = pm;
    s.headless = dev.headless();
    s.headless_w = desc_in.headless_width;
    s.headless_h = desc_in.headless_height;
    s.decide_output();
    s.latency = std::make_unique<extras::LowLatency>(dev);
    extras::lowlat_set(s.latency.get());
    if (!s.create_slots()) {
        if (error) *error = "could not create the frame slots";
        return nullptr;
    }
    int pw = static_cast<int>(s.headless_w), ph = static_cast<int>(s.headless_h);
    if (!s.headless) platform::pixel_size(&pw, &ph);
    if (!s.headless && (pw == 0 || ph == 0)) {
        s.dirty = true;   // created while minimised: the first begin_frame builds the swapchain
    } else if (!s.recreate(static_cast<uint32_t>(pw), static_cast<uint32_t>(ph))) {
        if (error) *error = "could not create the swapchain";
        return nullptr;
    }
    s.last_px_w = pw;
    s.last_px_h = ph;
    return r;
}

Renderer::~Renderer() {
    if (impl_) {
        impl_->destroy_all();
        delete impl_;
    }
}

bool Renderer::begin_frame(FrameInfo* out) {
    Impl& s = *impl_;
    if (s.in_frame) end_frame();   // a forgotten end_frame must not wedge the slot
    int pw = static_cast<int>(s.headless_w), ph = static_cast<int>(s.headless_h);
    if (!s.headless) platform::pixel_size(&pw, &ph);
    if (!s.headless && (pw == 0 || ph == 0)) {
        s.st.skipped++;
        return false;
    }
    if (pw != s.last_px_w || ph != s.last_px_h) {
        s.dirty = true;
        s.last_px_w = pw;
        s.last_px_h = ph;
    }
    if (s.dirty && !s.recreate(static_cast<uint32_t>(pw), static_cast<uint32_t>(ph))) {
        s.st.skipped++;
        return false;
    }
    if (s.extent.width == 0 || s.extent.height == 0) {
        s.st.skipped++;
        return false;
    }

    if (s.kind != extras::OutputKind::Sdr8 && extras::hdr_state().metadata_serial != s.md_serial) s.apply_hdr_metadata();   // the game declared new levels

    Impl::Slot& slot = s.slots[s.cur_slot];
    vkWaitForFences(s.vk(), 1, &slot.fence, VK_TRUE, UINT64_MAX);
    s.read_timestamps(slot);

    uint32_t idx = 0;
    if (s.headless) {
        idx = static_cast<uint32_t>(s.frame_number % s.targets.size());
    } else {
        VkResult r = vkAcquireNextImageKHR(s.vk(), s.swapchain, UINT64_MAX, slot.acquire, VK_NULL_HANDLE, &idx);
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_ERROR_SURFACE_LOST_KHR) {
            s.dirty = true;
            s.st.out_of_date++;
            s.st.skipped++;
            return false;
        }
        if (r == VK_SUBOPTIMAL_KHR) {
            s.dirty = true;     // finish this frame, rebuild before the next
            s.st.out_of_date++;
        } else if (r != VK_SUCCESS) {
            NXLOG_E(kTag, "vkAcquireNextImageKHR failed (%d)", static_cast<int>(r));
            s.st.skipped++;
            return false;
        }
    }
    vkResetFences(s.vk(), 1, &slot.fence);
    vkResetCommandPool(s.vk(), slot.pool, 0);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(slot.cmd, &bi);
    if (slot.queries) {
        vkCmdResetQueryPool(slot.cmd, slot.queries, 0, 2);
        vkCmdWriteTimestamp2(slot.cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, slot.queries, 0);
    }
    Impl::Target& t = s.targets[idx];
    barrier(slot.cmd, t.image, kUndefined, kColorAtt);

    s.cur_image = idx;
    s.info = FrameInfo{slot.cmd, t.image, t.view, s.format, s.extent, s.cur_slot, idx, s.frame_number, s.kind != extras::OutputKind::Sdr8};
    s.in_frame = true;
    if (out) *out = s.info;
    return true;
}

namespace {
// The read-back swapchain bytes as RGBA8 for the PNG. HDR: the picture clipped to SDR (paper white = 1.0); next to the PNG (when it has a
// path) a false-colour map of the brightness above paper white and a PFM with the scene-linear values (1.0 = paper white), and the log stats.
std::vector<uint8_t> capture_to_rgba(std::vector<uint8_t> raw, uint32_t w, uint32_t h, VkFormat format, extras::OutputKind kind, const std::string& cap_path) {
    std::vector<uint8_t> px;
    if (kind != extras::OutputKind::Sdr8) {
        const extras::HdrState hs = extras::hdr_state();
        const extras::CaptureFormat cf = format == VK_FORMAT_R16G16B16A16_SFLOAT ? extras::CaptureFormat::Rgba16F
                                       : format == VK_FORMAT_A2R10G10B10_UNORM_PACK32 ? extras::CaptureFormat::A2R10G10B10 : extras::CaptureFormat::A2B10G10R10;
        std::vector<float> lin;
        std::vector<uint8_t> map;
        extras::HdrCaptureStats cs;
        if (extras::decode_hdr_capture(cf, raw.data(), w, h, hs.paper_white_nits, &lin, &px, &map, &cs)) {
            if (!cap_path.empty()) {
                std::string base = cap_path;
                if (base.size() > 4 && base.compare(base.size() - 4, 4, ".png") == 0) base.resize(base.size() - 4);
                save_png_rgba(base + ".hdrmap.png", map.data(), w, h);
                extras::write_pfm(base + ".pfm", lin, w, h, hs.paper_white_nits / 100.0f);
                NXLOG_I(kTag, "HDR screenshot: %s (paper white %.0f nits, peak %.0f)", extras::capture_stats_line(cs).c_str(), hs.paper_white_nits, hs.peak_nits);
            }
        } else {
            px.assign(static_cast<size_t>(w) * h * 4, 0);
        }
    } else {
        px = std::move(raw);
        if (format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB)
            for (size_t i = 0; i < px.size(); i += 4) std::swap(px[i], px[i + 2]);
        for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;
    }
    return px;
}
}  // namespace

void Renderer::end_frame() {
    Impl& s = *impl_;
    if (!s.in_frame) return;
    s.in_frame = false;
    Impl::Slot& slot = s.slots[s.cur_slot];
    Impl::Target& t = s.targets[s.cur_image];
    if (s.overlay) s.overlay(s.info);

    const bool capturing = s.cap_pending.load(std::memory_order_acquire) && (s.headless || s.capture_supported);
    Buffer cap;
    const uint32_t w = s.extent.width, h = s.extent.height;
    const uint32_t bpp = s.format == VK_FORMAT_R16G16B16A16_SFLOAT ? 8u : 4u;
    if (capturing) {
        if (s.dev->create_buffer(static_cast<VkDeviceSize>(w) * h * bpp, VK_BUFFER_USAGE_TRANSFER_DST_BIT, MemoryUse::Readback, &cap, "screenshot") != VK_SUCCESS) {
            NXLOG_E(kTag, "screenshot buffer allocation failed");
            if (!s.cap_path.empty()) g_png_writes.fetch_sub(1);
        }
    }
    {   // [gfx] exp_present_delay_us (measurement only, default 0): the presenting thread spins this long per frame (reproduces timing
        // that only shows when the present path is slow)
        static const int64_t delay_us = config_get_int("gfx", "exp_present_delay_us", 0);
        if (delay_us > 0) {
            const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(delay_us);
            while (std::chrono::steady_clock::now() < until) {}
        }
    }
    // [gfx] flash_watch: take the slot's previous picture (its fence was waited in begin_frame), then copy this one
    static const double flash_th = config_get_double("gfx", "flash_watch", 0.0);
    bool flash_copy = false;
    if (flash_th > 0.0 && !s.headless && bpp == 4) {
        if (s.flash.size() != s.slots.size()) { s.flash_free(); s.flash.resize(s.slots.size()); }
        Impl::FlashSlot& fs = s.flash[s.cur_slot];
        if (fs.buf && fs.valid) s.flash_take(fs);
        fs.valid = false;
        if (fs.buf && (s.flash_w != w || s.flash_h != h)) { s.dev->destroy_buffer(fs.buf); fs.buf = Buffer{}; }
        if (s.flash_w != w || s.flash_h != h) { s.flash_w = w; s.flash_h = h; s.flash_reset(); }
        if (!fs.buf) s.dev->create_buffer(static_cast<VkDeviceSize>(w) * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, MemoryUse::Readback, &fs.buf, "flash watch");
        if (fs.buf && !cap) { flash_copy = true; fs.valid = true; fs.frame = s.frame_number; }
        if (cap) s.flash_reset();
    }
    if (flash_copy) {
        Impl::FlashSlot& fs = s.flash[s.cur_slot];
        barrier(slot.cmd, t.image, kColorAtt, kTransferSrc);
        VkBufferImageCopy2 region = {VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {w, h, 1};
        VkCopyImageToBufferInfo2 ci = {VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2};
        ci.srcImage = t.image;
        ci.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        ci.dstBuffer = fs.buf.buffer;
        ci.regionCount = 1;
        ci.pRegions = &region;
        vkCmdCopyImageToBuffer2(slot.cmd, &ci);
        VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        mb.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        mb.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        mb.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        di.memoryBarrierCount = 1;
        di.pMemoryBarriers = &mb;
        vkCmdPipelineBarrier2(slot.cmd, &di);
        barrier(slot.cmd, t.image, kTransferSrc, kColorAtt);
    }
    if (cap) {
        barrier(slot.cmd, t.image, kColorAtt, kTransferSrc);
        VkBufferImageCopy2 region = {VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {w, h, 1};
        VkCopyImageToBufferInfo2 ci = {VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2};
        ci.srcImage = t.image;
        ci.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        ci.dstBuffer = cap.buffer;
        ci.regionCount = 1;
        ci.pRegions = &region;
        vkCmdCopyImageToBuffer2(slot.cmd, &ci);
        VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        mb.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        mb.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        mb.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        di.memoryBarrierCount = 1;
        di.pMemoryBarriers = &mb;
        vkCmdPipelineBarrier2(slot.cmd, &di);
        if (!s.headless) barrier(slot.cmd, t.image, kTransferSrc, kPresent);
    } else if (!s.headless) {
        barrier(slot.cmd, t.image, kColorAtt, kPresent);
    }
    if (slot.queries) {
        vkCmdWriteTimestamp2(slot.cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, slot.queries, 1);
        slot.ts_pending = true;
    }
    vkEndCommandBuffer(slot.cmd);

    VkSemaphoreSubmitInfo wait = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    wait.semaphore = slot.acquire;
    wait.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSemaphoreSubmitInfo signal = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    signal.semaphore = t.render_done;
    signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkCommandBufferSubmitInfo cb = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cb.commandBuffer = slot.cmd;
    VkSubmitInfo2 si = {VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cb;
    VkSemaphoreSubmitInfo signals[3] = {signal, {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO}, {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO}};
    if (!s.headless) {
        si.waitSemaphoreInfoCount = 1;
        si.pWaitSemaphoreInfos = &wait;
        si.signalSemaphoreInfoCount = 1;
        si.pSignalSemaphoreInfos = signals;
    }
    uint64_t shot_value = 0;   // [gfx] screenshot_async: a PNG screenshot is read back on a worker once the frame is done on the GPU
    if (cap && !s.headless && !s.cap_out && !s.cap_path.empty() && config_get_bool("gfx", "screenshot_async", true) && s.shot_sem_ready())
        shot_value = ++s.shot_next;
    // low latency markers go outside the queue lock (they take it themselves: the validation layer wants calls on one swapchain from different threads apart)
    uint64_t present_id = 0;
    uint64_t probe_value = 0;
    if (!s.headless && s.latency) {
        const uint64_t id = s.latency->present_begin();
        if (s.latency->enabled() && s.latency->backend() == extras::LowLatency::Backend::Reflex) present_id = id;
        s.probe_init();
        if (s.probe_sem) {
            probe_value = ++s.probe_next;
            s.probe_ids[probe_value % 256] = s.latency->present_sim_id();
            signals[1].semaphore = s.probe_sem;
            signals[1].value = probe_value;
            signals[1].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            si.signalSemaphoreInfoCount = 2;
        }
    }
    if (shot_value) {
        VkSemaphoreSubmitInfo& sv = signals[si.signalSemaphoreInfoCount];
        sv.semaphore = s.shot_sem;
        sv.value = shot_value;
        sv.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        si.signalSemaphoreInfoCount++;
    }
    {
        std::lock_guard<std::mutex> lk(s.dev->queue_mutex());
        VkResult r = vkQueueSubmit2(s.dev->queue(), 1, &si, slot.fence);
        if (r != VK_SUCCESS) NXLOG_E(kTag, "vkQueueSubmit2 failed (%d)", static_cast<int>(r));
        else if (probe_value) s.probe_published.store(probe_value);
        if (r == VK_ERROR_DEVICE_LOST) s.dev->report_device_lost("present submission");   // the NVN back end ends the process
        if (!s.headless) {
            VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
            pi.waitSemaphoreCount = 1;
            pi.pWaitSemaphores = &t.render_done;
            pi.swapchainCount = 1;
            pi.pSwapchains = &s.swapchain;
            pi.pImageIndices = &s.cur_image;
            VkPresentIdKHR pid = {VK_STRUCTURE_TYPE_PRESENT_ID_KHR};
            if (present_id) {
                pid.swapchainCount = 1;
                pid.pPresentIds = &present_id;
                pi.pNext = &pid;
            }
            r = vkQueuePresentKHR(s.dev->queue(), &pi);
            s.st.presents++;
            if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR || r == VK_ERROR_SURFACE_LOST_KHR) {
                s.dirty = true;
                s.st.out_of_date++;
            } else if (r != VK_SUCCESS) {
                NXLOG_E(kTag, "vkQueuePresentKHR failed (%d)", static_cast<int>(r));
            }
        }
    }
    if (!s.headless && s.latency) s.latency->present_end();
    s.st.frames++;
    s.frame_number++;
    {   // [gfx] log_fps = 1: one line per second with the presented frame rate and the GPU time of the present command buffer (output pass, FSR 1)
        static const bool log_fps = config_get_bool("gfx", "log_fps", false);
        static uint64_t last_ns = 0, last_frames = 0;
        if (log_fps) {
            const uint64_t now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
            if (now - last_ns >= 1000000000ull) {
                if (last_ns) NXLOG_I(kTag, "fps %.1f, present pass GPU %.3f ms avg", static_cast<double>(s.st.frames - last_frames) * 1e9 / static_cast<double>(now - last_ns), s.st.gpu_ms_avg);
                last_ns = now;
                last_frames = s.st.frames;
            }
        }
    }
    s.cur_slot = (s.cur_slot + 1) % static_cast<uint32_t>(s.slots.size());

    if (cap && shot_value) {   // [gfx] screenshot_async: a worker waits for the frame on the GPU, reads it back and writes the PNG
        s.shot_inflight.fetch_add(1);
        std::thread([dev = s.dev, sem = s.shot_sem, value = shot_value, buf = cap, w, h, bpp, fmt = s.format, kind = s.kind, path = s.cap_path,
                     inflight = &s.shot_inflight]() mutable {
            VkSemaphoreWaitInfo wi = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
            wi.semaphoreCount = 1;
            wi.pSemaphores = &sem;
            wi.pValues = &value;
            const VkResult wr = vkWaitSemaphores(dev->device(), &wi, 5ull * 1000 * 1000 * 1000);
            std::vector<uint8_t> raw;
            if (wr == VK_SUCCESS) {
                raw.resize(static_cast<size_t>(w) * h * bpp);
                dev->invalidate_buffer(buf);
                std::memcpy(raw.data(), buf.mapped, raw.size());
            }
            dev->destroy_buffer(buf);
            inflight->fetch_sub(1);
            if (wr == VK_SUCCESS) {
                std::vector<uint8_t> px = capture_to_rgba(std::move(raw), w, h, fmt, kind, path);
                if (save_png_rgba(path, px.data(), w, h)) NXLOG_I(kTag, "screenshot %ux%u written to %s", w, h, path.c_str());
            } else {
                NXLOG_E(kTag, "screenshot %s: the frame did not finish on the GPU (%d)", path.c_str(), static_cast<int>(wr));
            }
            g_png_writes.fetch_sub(1);
        }).detach();
        s.cap_pending.store(false, std::memory_order_relaxed);
        s.cap_path.clear();
        s.st.screenshots++;
    } else if (cap) {
        vkWaitForFences(s.vk(), 1, &slot.fence, VK_TRUE, UINT64_MAX);
        std::vector<uint8_t> raw(static_cast<size_t>(w) * h * bpp);
        s.dev->invalidate_buffer(cap);
        std::memcpy(raw.data(), cap.mapped, raw.size());
        s.dev->destroy_buffer(cap);
        std::vector<uint8_t> px = capture_to_rgba(std::move(raw), w, h, s.format, s.kind, s.cap_path);
        if (!s.cap_path.empty()) {
            // the PNG encode takes seconds: write it on a worker so the frame is not held up (wait_screenshot_writes before exit)
            std::thread([path = s.cap_path, data = s.cap_out ? px : std::move(px), w, h]() {
                if (save_png_rgba(path, data.data(), w, h)) NXLOG_I(kTag, "screenshot %ux%u written to %s", w, h, path.c_str());
                g_png_writes.fetch_sub(1);
            }).detach();
        }
        if (s.cap_out) *s.cap_out = std::move(px);
        if (s.cap_w) *s.cap_w = w;
        if (s.cap_h) *s.cap_h = h;
        s.cap_pending.store(false, std::memory_order_relaxed);
        s.cap_path.clear();
        s.cap_out = nullptr;
        s.cap_w = s.cap_h = nullptr;
        s.st.screenshots++;
    }
}

void Renderer::set_overlay_hook(std::function<void(const FrameInfo&)> hook) { impl_->overlay = std::move(hook); }

bool Renderer::set_present_mode(PresentMode m) {
    Impl& s = *impl_;
    s.requested = m;
    int pw = static_cast<int>(s.headless_w), ph = static_cast<int>(s.headless_h);
    if (!s.headless) platform::pixel_size(&pw, &ph);
    if (!s.headless && (pw == 0 || ph == 0)) {   // minimised: applied when the window is back
        s.dirty = true;
        return true;
    }
    return s.recreate(static_cast<uint32_t>(pw), static_cast<uint32_t>(ph));
}

PresentMode Renderer::requested_present_mode() const { return impl_->requested; }
PresentMode Renderer::present_mode() const { return impl_->active; }

bool Renderer::present_mode_supported(PresentMode m) const {
    if (impl_->headless) return true;
    return std::find(impl_->supported_modes.begin(), impl_->supported_modes.end(), to_vk(m)) != impl_->supported_modes.end();
}

bool Renderer::resize_headless(uint32_t w, uint32_t h) {
    Impl& s = *impl_;
    if (!s.headless || !w || !h) return false;
    s.headless_w = w;
    s.headless_h = h;
    s.dirty = true;
    return true;
}

extras::OutputKind Renderer::output_kind() const { return impl_->kind; }
VkColorSpaceKHR Renderer::color_space() const { return impl_->color_space; }
uint32_t Renderer::format_serial() const { return impl_->format_serial; }
VkSwapchainKHR Renderer::swapchain() const { return impl_->swapchain; }
extras::LowLatency* Renderer::low_latency() { return impl_->latency.get(); }

VkExtent2D Renderer::extent() const { return impl_->extent; }
VkFormat Renderer::format() const { return impl_->format; }
uint32_t Renderer::image_count() const { return static_cast<uint32_t>(impl_->targets.size()); }
uint32_t Renderer::frames_in_flight() const { return impl_->desc.frames_in_flight; }
bool Renderer::headless() const { return impl_->headless; }

void Renderer::request_screenshot(const std::string& png_path) {
    g_png_writes.fetch_add(1);   // until the worker has written the file (wait_screenshot_writes)
    impl_->cap_path = instance_file(png_path);   // relative: the working folder, or the instance folder (nxrt/instance.h)
    impl_->cap_pending.store(true, std::memory_order_release);   // after the path is set: end_frame reads the path once it sees the flag
}

void Renderer::capture_next_frame(std::vector<uint8_t>* rgba, uint32_t* width, uint32_t* height) {
    impl_->cap_out = rgba;
    impl_->cap_w = width;
    impl_->cap_h = height;
    impl_->cap_pending.store(true, std::memory_order_release);
}

bool Renderer::capture_pending() const { return impl_->cap_pending.load(std::memory_order_relaxed); }

RendererStats Renderer::stats() const { return impl_->st; }
void Renderer::wait_idle() { impl_->dev->wait_idle(); }

}  // namespace nxrt::gfx
