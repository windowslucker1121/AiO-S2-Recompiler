// SPDX-License-Identifier: GPL-3.0-or-later
// Vulkan 1.3 device layer of the graphics shell (S4b). Game independent: the NVN back end (later work) builds on it.
//   Device    instance (+ validation in debug), GPU choice (discrete first, override), logical device, one graphics+present
//             queue, VMA allocator, image-format checks before every image is created, buffers, one-shot submits.
//   Renderer  swapchain (or an offscreen image ring in headless mode), present-mode choice, frames in flight, GPU timestamps,
//             resize / minimise handling, screenshots, an overlay hook. Frame limiting is separate (frame_limiter.h): call
//             FrameLimiter::begin_frame() first, then platform::pump_events(), then Renderer::begin_frame().
// Configuration (config.h; env wins): [gfx] gpu (NXRT_GFX_GPU), validation (NXRT_GFX_VALIDATION), headless (NXRT_GFX_HEADLESS),
// present_mode (NXRT_GFX_PRESENT_MODE), frames_in_flight (NXRT_GFX_FRAMES_IN_FLIGHT); see docs/S4b.md.
#pragma once
#include <volk.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "nxrt/extras.h"

// VMA handles (the allocator header is only included by the .cpp files).
struct VmaAllocator_T;
struct VmaAllocation_T;
typedef struct VmaAllocator_T* VmaAllocator;
typedef struct VmaAllocation_T* VmaAllocation;

namespace nxrt::extras { class LowLatency; }

namespace nxrt::gfx {

// ---- GPU choice -------------------------------------------------------------------------------------------------------
// The rule alone (testable without a GPU). type = VkPhysicalDeviceType (0 other, 1 integrated, 2 discrete, 3 virtual, 4 cpu).
struct GpuCandidate {
    std::string name;
    int type = 0;
    uint64_t vram_bytes = 0;     // device-local memory, breaks ties between devices of the same type
};
// want: "" = default (discrete GPU with the most video memory, else integrated, virtual, other, cpu), "discrete",
// "integrated", an index (1-2 digits), or a case-insensitive part of the device name. An override that matches nothing falls
// back to the default and sets *matched = false. Returns an index into gpus (0 for an empty list).
size_t pick_gpu(const std::vector<GpuCandidate>& gpus, const char* want, bool* matched = nullptr);

struct GpuInfo {
    uint32_t index = 0;
    std::string name;
    uint32_t vendor_id = 0, device_id = 0;
    int type = 0;
    uint32_t api_version = 0, driver_version = 0;
    uint64_t vram_bytes = 0;
    bool can_present = false;    // has a queue family that can draw and present to the window surface (true when headless)
};

// ---- device -----------------------------------------------------------------------------------------------------------
struct DeviceDesc {
    std::string app_name = "nxrt";
    // Unset values are read from the configuration (see the top of this file).
    int validation = -1;                  // 1 on, 0 off, -1: [gfx] validation, default on in debug builds only
    std::string gpu;                      // override (see pick_gpu); empty: [gfx] gpu
    int headless = -1;                    // 1 no window/surface, 0 use the platform window, -1: [gfx] headless (env NXRT_GFX_HEADLESS)
};

struct ValidationStats {
    uint64_t errors = 0, warnings = 0, infos = 0, verbose = 0;
    std::string first_error;              // text of the first error since the last reset
};

enum class MemoryUse { GpuOnly, Upload, Readback };   // host-visible memory is mapped persistently

struct ImageDesc {
    VkImageType type = VK_IMAGE_TYPE_2D;
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    uint32_t width = 1, height = 1, depth = 1;
    uint32_t mip_levels = 1, array_layers = 1;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkImageCreateFlags flags = 0;
    VkImageTiling tiling = VK_IMAGE_TILING_OPTIMAL;
    bool create_view = true;              // a view of the whole image (2D / 2D array / 3D / cube by flags and layer count)
    const char* name = nullptr;           // debug name
};

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent3D extent = {};
    uint32_t mip_levels = 1, array_layers = 1;
    explicit operator bool() const { return image != VK_NULL_HANDLE; }
};

struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    VkDeviceSize size = 0;
    void* mapped = nullptr;               // set for Upload and Readback buffers
    explicit operator bool() const { return buffer != VK_NULL_HANDLE; }
};

struct FormatCheckStats {
    uint64_t checks = 0;                  // vkGetPhysicalDeviceImageFormatProperties2 calls made
    uint64_t cache_hits = 0;
    uint64_t rejected = 0;                // image creations refused before vkCreateImage
};

struct DeviceFeatures {
    bool bc = false, astc_ldr = false, etc2 = false;          // textureCompression*: enabled on the device
    bool sampler_anisotropy = false, independent_blend = false, depth_clamp = false, fill_mode_non_solid = false;
    bool timeline_semaphore = false, dynamic_rendering = false, synchronization2 = false;
    // Enabled for the NVN back end (everything the GPU offers is enabled; these flags say what is there).
    bool geometry_shader = false, shader_float64 = false, shader_int64 = false, dual_src_blend = false, logic_op = false;
    bool multi_viewport = false, depth_bounds = false, depth_bias_clamp = false, wide_lines = false, large_points = false;
    bool image_cube_array = false, shader_draw_parameters = false, sampler_mirror_clamp_to_edge = false;
    bool descriptor_indexing = false, host_query_reset = false;
    bool fragment_shader_interlock = false, depth_clip_control = false, depth_clip_enable = false, custom_border_color = false;
    bool extended_dynamic_state3 = false, vertex_input_dynamic_state = false;
    bool graphics_pipeline_library = false;   // VK_EXT_graphics_pipeline_library (pipelines linked from separately compiled parts, S24)
    bool external_memory_host = false, push_descriptor = false;
    bool dynamic_rendering_unused_attachments = false;
    bool buffer_device_address = false;   // Vulkan 1.2 bufferDeviceAddress: raw guest pointers in translated shaders (A17, the pool address table)
    bool mesh_shader = false, task_shader = false;   // VK_EXT_mesh_shader (NVN2 task/mesh programs); [gfx] mesh_shader = false leaves it off
    // VK_KHR_cooperative_matrix with 16x8x16 float16 subgroup matrices (float16 and float32 accumulators), 32-lane subgroups, and the
    // translator's element order confirmed on this device by a probe dispatch (vk_coopmat_probe.cpp): the shader cache may then use
    // the cooperative-matrix variant of a block with HMMA (<hash>.cm.spv). [gfx] cooperative_matrix = false leaves it off.
    bool cooperative_matrix_hmma = false;
    bool transform_feedback = false, transform_feedback_queries = false;   // VK_EXT_transform_feedback (the feedback queries: transformFeedbackQueries)
    bool fragment_shading_rate = false;   // VK_KHR_fragment_shading_rate with pipelineFragmentShadingRate (NVN2 shading rate image, nvn_small.h)
    bool conservative_raster_dynamic = false;   // VK_EXT_conservative_rasterization + the extended-dynamic-state-3 mode / overestimation size (NVN2 conservative raster)
    bool index_type_uint8 = false;        // VK_EXT/KHR_index_type_uint8 (8-bit index buffers, indexTypeUint8)
    bool debug_labels = false;            // VK_EXT_debug_utils is enabled on the instance (command buffer labels)
    bool occlusion_query = false, occlusion_query_precise = false, pipeline_statistics_query = false;
    bool sparse_binding = false, sparse_residency_image2d = false, sparse_residency_image3d = false, sparse_residency_aliased = false;
    // Display extras (S14): HDR swapchains and low-latency markers. Enabled whenever the GPU / driver offers them; used only when the settings ask.
    bool swapchain_colorspace = false;    // VK_EXT_swapchain_colorspace (instance): HDR10 and scRGB colour spaces in the surface format list
    bool hdr_metadata = false;            // VK_EXT_hdr_metadata
    bool present_id = false;              // VK_KHR_present_id (feature presentId)
    bool low_latency2 = false;            // VK_NV_low_latency2 (NVIDIA Reflex)
    bool anti_lag = false;                // VK_AMD_anti_lag (AMD Anti-Lag 2)
    // Device-loss diagnostics: what the driver says after VK_ERROR_DEVICE_LOST (Device::report_device_lost).
    bool device_fault = false, device_fault_vendor_binary = false;   // VK_EXT_device_fault ([gfx] device_fault, default on)
    bool diagnostic_checkpoints = false;  // VK_NV_device_diagnostic_checkpoints ([gfx] device_diagnostics, default off: a marker per draw)
};

struct DeviceLimitsExt {
    uint64_t min_imported_host_alignment = 0;   // VK_EXT_external_memory_host (0: not available)
    uint64_t max_buffer_size = 0;
    uint32_t subgroup_size = 0, subgroup_ops = 0;
    uint32_t max_memory_allocation_count = 0;
    float cons_max_extra = 0, cons_granularity = 0;   // VK_EXT_conservative_rasterization: maxExtraPrimitiveOverestimationSize, extraPrimitiveOverestimationSizeGranularity
    uint32_t mesh_max_groups = 0, task_max_groups = 0;   // VK_EXT_mesh_shader: maxMeshWorkGroupCount[0], maxTaskWorkGroupCount[0]
    uint32_t fsr_sizes = 0;   // VK_KHR_fragment_shading_rate: bit (log2(w) * 3 + log2(h)) = a fragment size of 1 sample the device supports (w, h in 1, 2, 4)
};

class Device {
public:
    // Creates instance, device and allocator. Returns null and fills *error on failure. Needs platform::init() and a window
    // (platform::create_window) unless headless.
    static std::unique_ptr<Device> create(const DeviceDesc& desc, std::string* error = nullptr);
    ~Device();
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    VkInstance instance() const { return instance_; }
    VkPhysicalDevice physical() const { return physical_; }
    VkDevice device() const { return device_; }
    VkQueue queue() const { return queue_; }
    uint32_t queue_family() const { return queue_family_; }
    std::mutex& queue_mutex() { return queue_mutex_; }              // hold while calling vkQueueSubmit / vkQueuePresentKHR
    VmaAllocator allocator() const { return vma_; }
    uint64_t buffer_address(VkBuffer b) const;   // vkGetBufferDeviceAddress (0 without the feature or the usage bit)
    VkSurfaceKHR surface() const { return surface_; }              // null when headless
    bool headless() const { return surface_ == VK_NULL_HANDLE; }
    const VkPhysicalDeviceProperties& properties() const { return props_; }
    const DeviceFeatures& features() const { return features_; }
    const DeviceLimitsExt& limits_ext() const { return limits_; }
    const std::vector<GpuInfo>& gpus() const { return gpus_; }
    const GpuInfo& gpu() const { return gpus_[gpu_index_]; }       // the one in use
    bool gpu_override_matched() const { return gpu_override_matched_; }
    float timestamp_period_ns() const { return props_.limits.timestampPeriod; }
    bool timestamps_supported() const { return timestamp_valid_bits_ > 0; }

    // Validation (debug builds, or [gfx] validation). The counters cover everything the layer reported since create/reset.
    bool validation_enabled() const { return validation_; }
    ValidationStats validation_stats() const;
    void validation_reset();

    // Image formats: every image is checked with vkGetPhysicalDeviceImageFormatProperties2 (format, type, tiling, usage,
    // flags) and against the enabled compression features BEFORE vkCreateImage. Desktop NVIDIA GPUs have no ASTC and a
    // blind vkCreateImage with an ASTC format took the driver down in Wonder's Windows port. Results are cached.
    bool image_supported(const ImageDesc& d, std::string* why = nullptr);
    VkResult create_image(const ImageDesc& d, Image* out, std::string* why = nullptr);   // VK_ERROR_FORMAT_NOT_SUPPORTED when refused
    void destroy_image(Image& img);
    FormatCheckStats format_check_stats() const;

    VkResult create_buffer(VkDeviceSize size, VkBufferUsageFlags usage, MemoryUse use, Buffer* out, const char* name = nullptr);
    void destroy_buffer(Buffer& b);
    void flush_buffer(const Buffer& b);         // after the CPU wrote an Upload buffer (non-coherent memory)
    void invalidate_buffer(const Buffer& b);    // before the CPU reads a Readback buffer the GPU wrote

    // Runs fn on a command buffer of a transient pool, submits it and waits for it (uploads, tests).
    bool submit_once(const std::function<void(VkCommandBuffer)>& fn);
    void wait_idle();

    void set_name(VkObjectType type, uint64_t handle, const char* name);     // VK_EXT_debug_utils, no-op without it

    // After VK_ERROR_DEVICE_LOST: logs what the driver knows (VK_EXT_device_fault: fault description, faulting addresses, vendor codes;
    // VK_NV_device_diagnostic_checkpoints: the last checkpoint each pipeline stage reached, described by the checkpoint describer).
    // Only the first call reports and returns true; later ones (every submission fails from then on) return false at once.
    bool report_device_lost(const char* where);
    bool device_lost() const { return lost_reported_.load(); }
    // Turns a checkpoint marker (vkCmdSetCheckpointNV) back into text: set by the code that records the checkpoints.
    void set_checkpoint_describer(std::function<std::string(uint64_t)> fn) { checkpoint_describer_ = std::move(fn); }

    // Live allocation counters of the allocator (for leak checks).
    struct MemoryStats { uint64_t allocations = 0, blocks = 0, bytes = 0; };
    MemoryStats memory_stats();

    struct Impl;                          // implementation detail (vk_device.cpp)

private:
    Device() = default;
    bool init(const DeviceDesc& desc, std::string* error);

    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t queue_family_ = 0;
    uint32_t timestamp_valid_bits_ = 0;
    std::mutex queue_mutex_;
    std::atomic<bool> lost_reported_{false};
    std::function<std::string(uint64_t)> checkpoint_describer_;
    VmaAllocator vma_ = nullptr;
    VkPhysicalDeviceProperties props_ = {};
    DeviceFeatures features_;
    DeviceLimitsExt limits_;
    std::vector<GpuInfo> gpus_;
    size_t gpu_index_ = 0;
    bool gpu_override_matched_ = true;
    bool validation_ = false;
    bool debug_utils_ = false;
    bool owns_platform_surface_ = false;
    Impl* impl_ = nullptr;
};

// ---- presentation -----------------------------------------------------------------------------------------------------
enum class PresentMode : uint8_t { Fifo, FifoRelaxed, Mailbox, Immediate };
const char* present_mode_name(PresentMode m);
bool parse_present_mode(const char* s, PresentMode* out);          // "fifo", "fifo_relaxed", "mailbox", "immediate"
VkPresentModeKHR to_vk(PresentMode m);

struct RendererDesc {
    PresentMode present_mode = PresentMode::Fifo;   // [gfx] present_mode overrides
    uint32_t frames_in_flight = 3;                  // 1..3, [gfx] frames_in_flight overrides (3: the CPU can record two frames while the GPU draws one)
    uint32_t headless_width = 1280, headless_height = 720;
};

// One frame as the caller records it. The colour image is in COLOR_ATTACHMENT_OPTIMAL: draw with dynamic rendering.
struct FrameInfo {
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent = {};
    uint32_t slot = 0;                    // frame-in-flight index
    uint32_t image_index = 0;
    uint64_t frame_number = 0;            // counts successful begin_frame calls from 0
    bool hdr_output = false;              // the target is an HDR swapchain image (HDR10 / scRGB): colours are encoded, not sRGB
};

struct RendererStats {
    uint64_t frames = 0;                  // frames submitted
    uint64_t presents = 0;                // vkQueuePresentKHR calls (0 when headless)
    uint64_t skipped = 0;                 // begin_frame returned false
    uint64_t swapchain_creates = 0;       // swapchains created (first one included)
    uint64_t out_of_date = 0;             // OUT_OF_DATE / SUBOPTIMAL results seen
    uint64_t screenshots = 0;
    double gpu_ms_last = 0, gpu_ms_avg = 0;
};

class Renderer {
public:
    static std::unique_ptr<Renderer> create(Device& dev, const RendererDesc& desc, std::string* error = nullptr);
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // Starts a frame: waits for the frame slot, picks up a window resize (recreates the swapchain), acquires an image, starts
    // the command buffer (GPU timestamp 0) and puts the image in COLOR_ATTACHMENT_OPTIMAL. Returns false when there is nothing
    // to draw (window minimised / zero extent, swapchain being recreated): the caller waits for events and tries again.
    bool begin_frame(FrameInfo* out);
    // Runs the overlay hook, finishes and submits the command buffer (GPU timestamp 1), saves a requested screenshot and
    // presents.
    void end_frame();

    // Called from end_frame, after the caller's drawing, with the image still in COLOR_ATTACHMENT_OPTIMAL: record draws that
    // go on top (the ImGui overlay) with a dynamic-rendering pass that loads the image.
    void set_overlay_hook(std::function<void(const FrameInfo&)> hook);

    // Present mode. The request is remembered; the mode in use is the closest the surface supports (Mailbox -> Immediate ->
    // Fifo, Immediate -> Mailbox -> Fifo, FifoRelaxed -> Fifo). Recreates the swapchain; false if recreation failed.
    bool set_present_mode(PresentMode m);
    PresentMode requested_present_mode() const;
    PresentMode present_mode() const;                 // in use
    bool present_mode_supported(PresentMode m) const;
    // Headless mode only: change the size of the offscreen images (the resize path without a window).
    bool resize_headless(uint32_t w, uint32_t h);

    // Display extras (S14). The swapchain flavour in use: SDR 8 bit, HDR10 (ST 2084 on A2B10G10R10) or scRGB (linear float16). It is decided when the
    // renderer is created from [hdr] and what the surface offers; it can fall back to Sdr8 when the display leaves HDR mode. format_serial() changes
    // whenever the swapchain format changed, so passes that built pipelines for the format know to rebuild them.
    extras::OutputKind output_kind() const;
    VkColorSpaceKHR color_space() const;
    uint32_t format_serial() const;
    VkSwapchainKHR swapchain() const;
    extras::LowLatency* low_latency();                 // never null; check enabled()

    VkExtent2D extent() const;
    VkFormat format() const;
    uint32_t image_count() const;
    uint32_t frames_in_flight() const;
    bool headless() const;

    // Screenshot: the image of the next frame that ends after this call is read back and written as a PNG (RGBA8, opaque).
    // The frame waits for the copy, so the file exists when end_frame returns. capture_next_frame returns the pixels instead
    // (tightly packed RGBA8 rows) into *out, filled when end_frame of that frame returns.
    void request_screenshot(const std::string& png_path);
    void capture_next_frame(std::vector<uint8_t>* rgba, uint32_t* width, uint32_t* height);
    // True while a screenshot or a frame capture is requested and has not been taken yet (safe from any thread).
    bool capture_pending() const;

    RendererStats stats() const;
    void wait_idle();

private:
    Renderer() = default;
    struct Impl;
    Impl* impl_ = nullptr;
};

// ---- image helpers (PNG through SDL3, no extra dependency) --------------------------------------------------------------
bool save_png_rgba(const std::string& path, const uint8_t* rgba, uint32_t w, uint32_t h);
bool load_png_rgba(const std::string& path, std::vector<uint8_t>* rgba, uint32_t* w, uint32_t* h);
struct ImageDiff {
    uint32_t width = 0, height = 0;
    bool same_size = false;
    double mean_abs = 0;                  // mean absolute difference per channel (0..255), alpha ignored
    int max_abs = 0;                      // largest per-channel difference
    double fraction_over_8 = 0;           // fraction of pixels with any channel differing by more than 8
};
ImageDiff compare_rgba(const uint8_t* a, uint32_t aw, uint32_t ah, const uint8_t* b, uint32_t bw, uint32_t bh);

}  // namespace nxrt::gfx
