// SPDX-License-Identifier: GPL-3.0-or-later
// NVNdevice, NVNqueue, NVNsync, NVNevent and NVNwindow (S4d). These are heavy objects: Initialize creates the host
// object (nvn_core.h) and binds it to the guest object (GuestHeader); Finalize unbinds it. Every function checks that the
// guest object is ours (host_of) and answers 0 / null / NATIVE_ERROR when it is not. Anything that touches the GPU goes
// through nvn_gpu.h only.
//
// Not registered here, on purpose:
//   nvnDeviceGetProcAddress        already implemented by nvn_loader.cpp (device_get_proc_address).
//   nvnEventWait                   not in nvn_table.inc.
//   nvnEventSetDebugLabel          not in nvn_table.inc.
//
// Signatures without an entry in sigs.txt (the table has '?'), assumed here and not checked by NVN_REG:
//   ptrdiff_t nvnEventGetMemoryOffset(const NVNevent*)          (mirrors nvnEventBuilderGetMemoryOffset in api_builders.cpp)
//   int nvnWindowGetNumTextures(const NVNwindow*)
//   int nvnWindowGetNumActiveTextures(const NVNwindow*)
//   void nvnWindowSetNumActiveTextures(NVNwindow*, int)
//   void nvnSetExternalDebuggerEnabledNVX(NVNboolean)            (no-op)
// NVN2 entry points with no NVN1 signature, answered as the NVN2 driver does (sdk addresses of its functions):
//   const NVNmemoryPool* nvnEventGetMemoryPool(const NVNevent*)                                    (0x5E0DA0)
//   int nvnSyncCreateGLSync(const NVNsync*, uint64_t* gl_sync)                                     (0x5EDDE0)
//   NVNboolean nvnSyncInitializeFromFencedGLSync(NVNsync*, NVNdevice*, int gl_sync, int* status)   (0x5EDD00)
// The GL sync pair is GL interop: no GL context exists behind this back end, so both give the driver's answer for a missing GL.
//
// Layouts and enum values are those of the NVN header (third_party/nvn/nvn.h). Event signal mode WRITE is the header's
// NVN_EVENT_SIGNAL_MODE_WRITE. Constants used by the device info table: nvn_device_info.inc.
#include <atomic>
#include <chrono>
#include <mutex>
#include <unordered_map>
#include <cstring>
#include <vector>

#include "nxrt/extras.h"
#include "nxrt/game.h"
#include "nvn_cmd.h"
#include "nvn_gpu.h"
#include "nvn_impl.h"
#include "nxrt/gauge.h"
#include "nxrt/log.h"

namespace nxrt::nvn {
namespace {

constexpr const char* kTag = "nvn";

// Live counts (nxrt/gauge.h): an object counts from its Initialize to its Finalize (or to an Initialize over it). A window also holds up to 4
// swapchain textures, counted in window_textures.
nxrt::GaugeCounter g_syncs("nvn.syncs");
nxrt::GaugeCounter g_windows("nvn.windows");
nxrt::GaugeCounter g_window_textures("nvn.window_textures");
void window_gone(const be::Window* w) {
    g_windows.sub(1);
    g_window_textures.sub(static_cast<int64_t>(w->textures.size()));
}

// Unknown command handles seen by nvnQueueSubmitCommands; a warning is logged for the first and then every 100th.
std::atomic<uint64_t> g_unknown_handles{0};

void note_unknown_handle(NVNcommandHandle handle, int count) {
    const uint64_t n = g_unknown_handles.fetch_add(1);
    if (n % 100 == 0)
        NXLOG_W(kTag, "nvnQueueSubmitCommands: unknown handle 0x%llx (count %d) skipped; %llu unknown so far",
                static_cast<unsigned long long>(handle), count, static_cast<unsigned long long>(n + 1));
}

// ---- device -------------------------------------------------------------------------------------------------------------
int device_info_value(int info) {
    switch (info) {
#define NVN_DEVICE_INFO_VALUE(name, value) case name: return value;
#include "nvn_device_info.inc"
#undef NVN_DEVICE_INFO_VALUE
        default: return 0;
    }
}

NVNboolean nvnDeviceInitialize(NVNdevice* device, const NVNdeviceBuilder* builder) {
    if (!device) return 0;
    const DeviceBuilderInfo info = builder ? decode(builder) : DeviceBuilderInfo{};
    auto* dev = new be::Device();
    dev->flags = info.flags;
    be::bind_guest(device, dev);
    return 1;
}

void nvnDeviceFinalize(NVNdevice* device) {
    if (!be::host_of<be::Device>(device)) return;
    be::unbind_guest(device);
}

void nvnDeviceSetDebugLabel(NVNdevice*, const char*) {}

// NVN 55 debugging calls the game asks for by name and never makes (signatures assumed from their names): accepted, nothing to do.
void nvnDeviceSetAutomaticCheckpointsEnable(NVNdevice*, NVNboolean) {}
void nvnDeviceSetAutomaticCheckpointsMask(NVNdevice*, uint32_t) {}
void nvnDeviceSetAutomaticCheckpointsSamplingInterval(NVNdevice*, uint32_t) {}
void nvnDeviceSetAutomaticCheckpointsByDebugGroup(NVNdevice*, NVNboolean) {}
void nvnDeviceSetCheckpointCallStacksEnable(NVNdevice*, NVNboolean) {}
void nvnDeviceSetCheckpointMaxCallStacksDepth(NVNdevice*, int) {}

void nvnDeviceGetInteger(const NVNdevice* device, NVNdeviceInfo info, int* out) {
    if (!out) return;
    (void)device;   // the values do not depend on the device (the game asks before and after nvnDeviceInitialize)
    if (game::info().platform == game::Platform::Switch2) {
        // NVN2 (Switch 2 SDK 20.5, docs/nvn2_abi.md section 1): the values that differ from NVN1; the rest keep NVN1's numbering.
        switch (static_cast<int>(info)) {
            case 0x00: *out = 231; return;          // API_MAJOR_VERSION
            case 0x01: *out = 40; return;           // API_MINOR_VERSION: Ocarina of Time (SDK 22) needs >= 40, Pokopia (20.5) >= 30
            case 0x06: *out = 32; return;           // VERTEX_BUFFER_BINDINGS
            case 0x0E: *out = 32; return;           // VERTEX_ATTRIBUTES
            case 0x1A: *out = 0x4000; return;       // SHADER_SCRATCH_MEMORY_GRANULARITY
            case 0x1C: case 0x1F: *out = 1024; return;
            case 0x20: *out = 0x7FFFFFFF; return;   // MAX_COMPUTE_DISPATCH_WORK_GROUPS_X
            case 0x2C: *out = 16; return;           // MAX_TEXTURE_LEVELS
            case 0x2E: case 0x2F: *out = 201; return;   // GLSLC max/min supported GPU code major
            case 0x30: *out = 17; return;           // GLSLC max supported GPU code minor
            case 0x31: *out = 0; return;            // GLSLC min supported GPU code minor
            case 0x4E: *out = 2048; return;         // SHADER_CODE_MEMORY_POOL_PADDING_SIZE
            case 0x5A: *out = 0; return;            // SUPPORTS_DEBUG_LAYER
            default: break;
        }
        // The NVN2-only values 0x62..0xA9, read from Ocarina of Time's sdk (SDK 22): nvnDeviceGetInteger (sdk+0x656210) answers a few itself and
        // takes the rest from its constant table (sdk+0x648380, byte map at sdk+0xC7A318). 0 = not a constant there (device fields; unused so far).
        // Its movie enhancer (NDVE in main) needs 0x67 = 256 (an alignment): with 0 it aborts the title movie.
        static constexpr int kNvn2[0xAA - 0x62] = {
            32, 256, 16, 65535, 65535, 256, 1, 16, 16, 16, 1, 1, 1, 256,                       // 0x62..0x6F
            0, 16, 8, 16, 32, 31, 536870912, 16777216, 16777216, 1073741824, 32, 1, 64, 8, 1, 0,   // 0x70..0x7F
            32, 32, 0, 512, 256, 32, 16383, 1, 8, 0, 0, 4, 256, 16, 0, 1,                       // 0x80..0x8F
            31, 0, 64, 1, 58368, 0, 0, 256, 262144, 2, 1, 0, 201, 14, 201, 20,                  // 0x90..0x9F
            0, 8, 4, 256, 256, 4, 65536, 201, 5, 1,                                             // 0xA0..0xA9
        };
        if (static_cast<int>(info) >= 0x62 && static_cast<int>(info) <= 0xA9) {
            *out = kNvn2[static_cast<int>(info) - 0x62];
            return;
        }
    }
    *out = device_info_value(static_cast<int>(info));
}

uint64_t nvnDeviceGetCurrentTimestampInNanoseconds(const NVNdevice* device) {
    return be::host_of<be::Device>(device) ? be::gpu::timestamp_ns() : 0;
}

void nvnDeviceSetIntermediateShaderCache(NVNdevice*, int) {}

NVNtextureHandle nvnDeviceGetTextureHandle(const NVNdevice* device, int texture_id, int sampler_id) {
    if (!be::host_of<be::Device>(device)) return 0;
    return be::make_texture_handle(static_cast<uint32_t>(texture_id), static_cast<uint32_t>(sampler_id));
}

NVNtextureHandle nvnDeviceGetTexelFetchHandle(const NVNdevice* device, int texture_id) {
    if (!be::host_of<be::Device>(device)) return 0;
    return be::make_texture_handle(static_cast<uint32_t>(texture_id), 0);
}

NVNimageHandle nvnDeviceGetImageHandle(const NVNdevice* device, int texture_id) {
    if (!be::host_of<be::Device>(device)) return 0;
    return static_cast<uint64_t>(static_cast<uint32_t>(texture_id));
}

void nvnDeviceInstallDebugCallback(NVNdevice*, PFNNVNDEBUGCALLBACKPROC, void*, NVNboolean) {}

NVNdebugDomainId nvnDeviceGenerateDebugDomainId(const NVNdevice* device, const char*) {
    be::Device* dev = be::host_of<be::Device>(device);
    return dev ? static_cast<NVNdebugDomainId>(dev->debug_domain.fetch_add(1)) : 0;
}

void nvnDeviceSetWindowOriginMode(NVNdevice* device, NVNwindowOriginMode mode) {
    if (be::Device* dev = be::host_of<be::Device>(device)) {
        dev->window_origin = static_cast<int>(mode);
        be::gpu::set_window_modes(dev->window_origin, dev->depth_mode);
    }
}

void nvnDeviceSetDepthMode(NVNdevice* device, NVNdepthMode mode) {
    if (be::Device* dev = be::host_of<be::Device>(device)) {
        dev->depth_mode = static_cast<int>(mode);
        be::gpu::set_window_modes(dev->window_origin, dev->depth_mode);
    }
}

// Fast-clear registration (sdk 0x5D6920..0x5D69E0; the table is FastClearTables in nvn_small.h). Vulkan has no fast-clear table, so this
// keeps the registry only: it accepts and refuses the values the console does and hands out a stable index. A null value is refused.
int register_color(NVNdevice* device, uint32_t kind, const void* color, NVNformat format) {
    be::Device* dev = be::host_of<be::Device>(device);
    if (!dev || !color) return -1;
    uint32_t v[4];
    std::memcpy(v, color, sizeof v);
    return dev->fast_clear.add_color(kind, v, static_cast<uint32_t>(format));
}
int register_depth(NVNdevice* device, float value) {
    be::Device* dev = be::host_of<be::Device>(device);
    return dev ? dev->fast_clear.add_depth(value) : -1;
}
int register_stencil(NVNdevice* device, int value, int format) {
    be::Device* dev = be::host_of<be::Device>(device);
    return dev ? dev->fast_clear.add_stencil(static_cast<uint32_t>(value), static_cast<uint32_t>(format)) : -1;
}

NVNboolean nvnDeviceRegisterFastClearColor(NVNdevice* device, const float* color, NVNformat format) { return register_color(device, 0, color, format) >= 0; }
NVNboolean nvnDeviceRegisterFastClearColori(NVNdevice* device, const int* color, NVNformat format) { return register_color(device, 1, color, format) >= 0; }
NVNboolean nvnDeviceRegisterFastClearColorui(NVNdevice* device, const uint32_t* color, NVNformat format) { return register_color(device, 2, color, format) >= 0; }
NVNboolean nvnDeviceRegisterFastClearDepth(NVNdevice* device, float value) { return register_depth(device, value) >= 0; }
// NVN2 only (not in the NVN1 table). The *Index calls return the registry index (-1: refused); signatures read from the sdk (0x5D6930 and the
// other thunks into 0x6179F0 / 0x617D00 / 0x617D20 / 0x617E40): (device, value, format) and (device, value); stencil takes the value, and the
// index form also the depth-stencil format.
int nvnDeviceRegisterFastClearColorIndex(NVNdevice* device, const float* color, NVNformat format) { return register_color(device, 0, color, format); }
int nvnDeviceRegisterFastClearColoriIndex(NVNdevice* device, const int* color, NVNformat format) { return register_color(device, 1, color, format); }
int nvnDeviceRegisterFastClearColoruiIndex(NVNdevice* device, const uint32_t* color, NVNformat format) { return register_color(device, 2, color, format); }
int nvnDeviceRegisterFastClearDepthIndex(NVNdevice* device, float value) { return register_depth(device, value); }
NVNboolean nvnDeviceRegisterFastClearStencil(NVNdevice* device, int value) { return register_stencil(device, value, 0) >= 0; }
int nvnDeviceRegisterFastClearStencilIndex(NVNdevice* device, int value, NVNformat format) { return register_stencil(device, value, static_cast<int>(format)); }

NVNwindowOriginMode nvnDeviceGetWindowOriginMode(const NVNdevice* device) {
    const be::Device* dev = be::host_of<be::Device>(device);
    return static_cast<NVNwindowOriginMode>(dev ? dev->window_origin : NVN_WINDOW_ORIGIN_MODE_LOWER_LEFT);
}

NVNdepthMode nvnDeviceGetDepthMode(const NVNdevice* device) {
    const be::Device* dev = be::host_of<be::Device>(device);
    return static_cast<NVNdepthMode>(dev ? dev->depth_mode : NVN_DEPTH_MODE_NEAR_IS_MINUS_W);
}

uint64_t nvnDeviceGetTimestampInNanoseconds(const NVNdevice* device, const NVNcounterData* counter) {
    if (!be::host_of<be::Device>(device) || !counter) return 0;
    return counter->timestamp;
}

void nvnDeviceApplyDeferredFinalizes(NVNdevice*, int) {}

// The handle table is global, so the device is not needed (and is not checked: a handle must not outlive the game's
// finalize because the device pointer it passed did not resolve).
void nvnDeviceFinalizeCommandHandle(NVNdevice*, NVNcommandHandle handle) {
    if (handle) be::handle_release(handle);
}

void nvnDeviceWalkDebugDatabase(const NVNdevice*, NVNdebugObjectType, PFNNVNWALKDEBUGDATABASECALLBACKPROC, void*) {}

NVNseparateTextureHandle nvnDeviceGetSeparateTextureHandle(const NVNdevice* device, int texture_id) {
    NVNseparateTextureHandle h{};
    if (be::host_of<be::Device>(device)) h.value = static_cast<uint64_t>(static_cast<uint32_t>(texture_id));
    return h;
}

NVNseparateSamplerHandle nvnDeviceGetSeparateSamplerHandle(const NVNdevice* device, int sampler_id) {
    NVNseparateSamplerHandle h{};
    if (be::host_of<be::Device>(device)) h.value = static_cast<uint64_t>(static_cast<uint32_t>(sampler_id));
    return h;
}

NVNboolean nvnDeviceIsExternalDebuggerAttached(const NVNdevice*) { return 0; }

// ---- queue --------------------------------------------------------------------------------------------------------------
NVNboolean nvnQueueInitialize(NVNqueue* queue, const NVNqueueBuilder* builder) {
    if (!queue) return 0;
    const QueueBuilderInfo info = builder ? decode(builder) : QueueBuilderInfo{};
    auto* q = new be::Queue();
    q->device = be::host_of<be::Device>(info.device);
    q->flags = info.flags;
    q->command_memory = info.command_memory;
    q->compute_memory = info.compute_memory;
    q->control_memory = info.control_memory;
    be::bind_guest(queue, q);
    return 1;
}

void nvnQueueFinalize(NVNqueue* queue) {
    if (!be::host_of<be::Queue>(queue)) return;
    be::gpu::finish();
    be::unbind_guest(queue);
}

void nvnQueueSetDebugLabel(NVNqueue*, const char*) {}

void nvnQueueSubmitCommands(NVNqueue* queue, int count, const NVNcommandHandle* handles) {
    if (!be::host_of<be::Queue>(queue)) return;
    std::vector<be::Ref<be::CmdList>> lists;
    if (handles && count > 0) {
        lists.reserve(static_cast<size_t>(count));
        for (int i = 0; i < count; ++i) {
            be::Ref<be::CmdList> list = be::handle_lookup(handles[i]);
            if (list) lists.push_back(std::move(list));
            else note_unknown_handle(handles[i], count);
        }
    }
    be::gpu::submit(lists);
}

void nvnQueueFlush(NVNqueue* queue) {
    if (!be::host_of<be::Queue>(queue)) return;
    be::gpu::flush();
}

void nvnQueueFinish(NVNqueue* queue) {
    if (!be::host_of<be::Queue>(queue)) return;
    be::gpu::finish();
}

// Diagnostics: which syncs the game waits on (nvnSyncWait), by how they were armed: the window's acquire sync (armed with the newest queued
// serial, so a wait on it waits for every submitted frame) or a queue fence (nvnQueueFenceSync). One line every 10 s.
enum SyncKind { kSyncNone = 0, kSyncAcquire, kSyncFence, kSyncKinds };
std::mutex g_sync_kind_mu;
std::unordered_map<const be::Sync*, int> g_sync_kind;
struct SyncWaitStat { uint64_t calls = 0, signaled = 0, waits = 0, wait_ns = 0, lag = 0, max_wait_ns = 0; };
SyncWaitStat g_sync_stat[kSyncKinds];
void sync_note_kind(const be::Sync* s, int kind) {
    std::lock_guard<std::mutex> lk(g_sync_kind_mu);
    g_sync_kind[s] = kind;
}
void sync_wait_note(const be::Sync* s, bool waited, uint64_t ns, uint64_t lag) {
    std::lock_guard<std::mutex> lk(g_sync_kind_mu);
    auto it = g_sync_kind.find(s);
    SyncWaitStat& st = g_sync_stat[it == g_sync_kind.end() ? kSyncNone : it->second];
    st.calls++;
    if (waited) { st.waits++; st.wait_ns += ns; st.lag += lag; st.max_wait_ns = std::max(st.max_wait_ns, ns); }
    else st.signaled++;
    static auto last = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    if (now - last < std::chrono::seconds(10)) return;
    const double secs = std::chrono::duration<double>(now - last).count();
    last = now;
    static const char* const kName[kSyncKinds] = {"unarmed/other", "window acquire", "queue fence"};
    for (int k = 0; k < kSyncKinds; ++k) {
        SyncWaitStat& w = g_sync_stat[k];
        if (!w.calls) continue;
        NXLOG_I(kTag, "nvnSyncWait over %.1f s on %s syncs: %llu calls, %llu already signaled, %llu waited %.1f ms in all (max %.1f), mean %.2f submissions behind the newest",
                secs, kName[k], (unsigned long long)w.calls, (unsigned long long)w.signaled, (unsigned long long)w.waits, w.wait_ns / 1e6, w.max_wait_ns / 1e6,
                w.waits ? static_cast<double>(w.lag) / static_cast<double>(w.waits) : 0.0);
        w = SyncWaitStat{};
    }
}

// Shared by nvnWindowAcquireTexture and nvnQueueAcquireTexture (the queue variant passes no sync object).
NVNwindowAcquireTextureResult acquire_texture(be::Window* w, NVNsync* sync, int* index) {
    if (!w) return NVN_WINDOW_ACQUIRE_TEXTURE_RESULT_NATIVE_ERROR;
    std::lock_guard<std::mutex> lk(w->lock);
    const int total = static_cast<int>(w->textures.size());
    const int n = (w->active > 0 && w->active < total) ? w->active : total;
    if (n <= 0) return NVN_WINDOW_ACQUIRE_TEXTURE_RESULT_NATIVE_ERROR;
    const int slot = w->next_acquire % n;
    w->next_acquire = slot + 1;
    if (index) *index = slot;
    if (be::Sync* s = be::host_of<be::Sync>(sync)) {
        s->serial.store(be::gpu::last_serial());
        s->armed.store(true);
        sync_note_kind(s, kSyncAcquire);
    }
    return NVN_WINDOW_ACQUIRE_TEXTURE_RESULT_SUCCESS;
}

void nvnQueuePresentTexture(NVNqueue* queue, NVNwindow* window, int index) {
    if (!be::host_of<be::Queue>(queue)) return;
    be::Window* w = be::host_of<be::Window>(window);
    if (!w) return;
    if (index < 0 || index >= static_cast<int>(w->textures.size())) {
        NXLOG_W(kTag, "nvnQueuePresentTexture: index %d is not one of the %zu window textures; not presented", index, w->textures.size());
        return;
    }
    be::gpu::present(w, index);
}

NVNqueueAcquireTextureResult nvnQueueAcquireTexture(NVNqueue* queue, NVNwindow* window, int* index) {
    if (!be::host_of<be::Queue>(queue)) return NVN_QUEUE_ACQUIRE_TEXTURE_RESULT_NATIVE_ERROR;
    return static_cast<NVNqueueAcquireTextureResult>(acquire_texture(be::host_of<be::Window>(window), nullptr, index));
}

NVNqueueGetErrorResult nvnQueueGetError(NVNqueue*, NVNqueueErrorInfo*) {
    return NVN_QUEUE_GET_ERROR_RESULT_GPU_NO_ERROR;
}

size_t nvnQueueGetTotalCommandMemoryUsed(NVNqueue*) { return 0; }
size_t nvnQueueGetTotalControlMemoryUsed(NVNqueue*) { return 0; }
size_t nvnQueueGetTotalComputeMemoryUsed(NVNqueue*) { return 0; }
void nvnQueueResetMemoryUsageCounts(NVNqueue*) {}

void nvnQueueFenceSync(NVNqueue* queue, NVNsync* sync, NVNsyncCondition, int) {
    if (!be::host_of<be::Queue>(queue)) return;
    be::Sync* s = be::host_of<be::Sync>(sync);
    if (!s) return;
    s->serial.store(be::gpu::last_serial());   // a serial of 0 reads as already signaled (see nvnSyncWait)
    s->armed.store(true);
    sync_note_kind(s, kSyncFence);
}

// One GPU queue, in order: a later command already waits for an earlier one, so there is nothing to do.
NVNboolean nvnQueueWaitSync(NVNqueue* queue, const NVNsync* sync) {
    return be::host_of<be::Queue>(queue) && be::host_of<be::Sync>(sync) ? 1 : 0;
}

// ---- sync ---------------------------------------------------------------------------------------------------------------
NVNboolean nvnSyncInitialize(NVNsync* sync, NVNdevice*) {
    if (!sync) return 0;
    const bool was_live = be::host_of<be::Sync>(sync) != nullptr;   // initialised over a live sync: replaced, not added
    be::bind_guest(sync, new be::Sync());
    if (!was_live) g_syncs.add(1);
    return 1;
}

// GL interop (NVN2). The driver answers 4 (leaving *gl_sync alone) when it cannot export the sync, and false with *status = 4 when its
// GL interop interface is missing; here there is never a GL context.
constexpr int kGlSyncFailed = 4;
int nvnSyncCreateGLSync(const NVNsync*, uint64_t*) { return kGlSyncFailed; }
NVNboolean nvnSyncInitializeFromFencedGLSync(NVNsync*, NVNdevice*, int, int* status) {
    if (status) *status = kGlSyncFailed;
    return 0;
}

void nvnSyncFinalize(NVNsync* sync) {
    if (!be::host_of<be::Sync>(sync)) return;
    {
        std::lock_guard<std::mutex> lk(g_sync_kind_mu);
        g_sync_kind.erase(be::host_of<be::Sync>(sync));
    }
    g_syncs.sub(1);
    be::unbind_guest(sync);
}

void nvnSyncSetDebugLabel(NVNsync*, const char*) {}

NVNsyncWaitResult nvnSyncWait(const NVNsync* sync, uint64_t timeout_ns) {
    const be::Sync* s = be::host_of<be::Sync>(sync);
    if (!s || !s->armed.load()) return NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED;
    const uint64_t serial = s->serial.load();
    if (serial == 0 || be::gpu::serial_done(serial)) { sync_wait_note(s, false, 0, 0); return NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED; }
    const uint64_t newest = be::gpu::last_serial();
    const auto t0 = std::chrono::steady_clock::now();
    const bool ok = be::gpu::wait_serial(serial, timeout_ns);
    sync_wait_note(s, true, static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count()),
                   newest > serial ? newest - serial : 0);
    return ok ? NVN_SYNC_WAIT_RESULT_CONDITION_SATISFIED : NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED;
}

// ---- event --------------------------------------------------------------------------------------------------------------
// The 32-bit value of an event: a 4-byte aligned word inside the storage of its pool; null when there is none.
uint32_t* event_word(const be::Event* e) {
    if (!e || !e->pool || !e->pool->base || e->offset < 0 || e->offset % 4 != 0) return nullptr;
    if (static_cast<uint64_t>(e->offset) + 4 > e->pool->size) return nullptr;
    return reinterpret_cast<uint32_t*>(e->pool->base + e->offset);
}

NVNboolean nvnEventInitialize(NVNevent* event, const NVNeventBuilder* builder) {
    if (!event) return 0;
    const EventBuilderInfo info = builder ? decode(builder) : EventBuilderInfo{};
    auto* e = new be::Event();
    e->pool = be::host_of<be::MemoryPool>(info.pool);
    e->guest_pool = info.pool;
    e->offset = info.offset;
    be::bind_guest(event, e);
    return 1;
}

const NVNmemoryPool* nvnEventGetMemoryPool(const NVNevent* event) {
    const be::Event* e = be::host_of<be::Event>(event);
    return e ? e->guest_pool : nullptr;
}

void nvnEventFinalize(NVNevent* event) {
    if (!be::host_of<be::Event>(event)) return;
    be::unbind_guest(event);
}

uint32_t nvnEventGetValue(const NVNevent* event) {
    uint32_t* w = event_word(be::host_of<be::Event>(event));
    if (!w) return 0;
    return std::atomic_ref<uint32_t>(*w).load(std::memory_order_acquire);
}

void nvnEventSignal(NVNevent* event, NVNeventSignalMode mode, uint32_t value) {
    uint32_t* w = event_word(be::host_of<be::Event>(event));
    if (!w) return;
    std::atomic_ref<uint32_t> a(*w);
    if (mode == NVN_EVENT_SIGNAL_MODE_ADD) a.fetch_add(value, std::memory_order_release);
    else a.store(value, std::memory_order_release);
}

ptrdiff_t nvnEventGetMemoryOffset(const NVNevent* event) {
    const be::Event* e = be::host_of<be::Event>(event);
    return e ? static_cast<ptrdiff_t>(e->offset) : 0;
}

// ---- window -------------------------------------------------------------------------------------------------------------
NVNboolean nvnWindowInitialize(NVNwindow* window, const NVNwindowBuilder* builder) {
    if (!window) return 0;
    const WindowBuilderInfo info = builder ? decode(builder) : WindowBuilderInfo{};
    if (be::Window* old = be::host_of<be::Window>(window)) window_gone(old);   // initialised over a live window: the old one is replaced
    auto* w = new be::Window();
    for (int i = 0; i < info.texture_count && i < 4; ++i) {
        if (!info.textures[i]) continue;
        be::Texture* t = be::host_of<be::Texture>(info.textures[i]);
        if (t) w->textures.push_back(be::Ref<be::Texture>::share(t));
    }
    w->present_interval = info.present_interval;
    NXLOG_I(kTag, "nvnWindowInitialize: present interval %d", info.present_interval);
    w->native_window = info.native_window;
    w->hdr = info.hdr;
    if (w->hdr.color_space >= 0 || w->hdr.has_display || w->hdr.has_content)   // the game asks for an HDR window: tell the HDR output
        extras::hdr_note_window_hdr(w->hdr.color_space, w->hdr.has_display ? w->hdr.display : nullptr, w->hdr.has_content ? w->hdr.content : nullptr);
    be::bind_guest(window, w);
    g_windows.add(1);
    g_window_textures.add(static_cast<int64_t>(w->textures.size()));
    return 1;
}

void nvnWindowFinalize(NVNwindow* window) {
    const be::Window* w = be::host_of<be::Window>(window);
    if (!w) return;
    window_gone(w);
    be::unbind_guest(window);
}

void nvnWindowSetDebugLabel(NVNwindow*, const char*) {}

NVNwindowAcquireTextureResult nvnWindowAcquireTexture(NVNwindow* window, NVNsync* sync, int* index) {
    return acquire_texture(be::host_of<be::Window>(window), sync, index);
}

void* nvnWindowGetNativeWindow(const NVNwindow* window) {
    const be::Window* w = be::host_of<be::Window>(window);
    return w ? w->native_window : nullptr;
}

int nvnWindowGetPresentInterval(const NVNwindow* window) {
    const be::Window* w = be::host_of<be::Window>(window);
    return w ? w->present_interval : 0;
}

void nvnWindowSetPresentInterval(NVNwindow* window, int interval) {
    be::Window* w = be::host_of<be::Window>(window);
    if (!w) return;
    std::lock_guard<std::mutex> lk(w->lock);
    if (w->present_interval != interval) {
        static std::atomic<int> logged{0};
        if (logged.fetch_add(1) < 64) NXLOG_I(kTag, "nvnWindowSetPresentInterval: %d -> %d", w->present_interval, interval);
    }
    w->present_interval = interval;
}

void nvnWindowSetCrop(NVNwindow* window, int x, int y, int width, int height) {
    be::Window* w = be::host_of<be::Window>(window);
    if (!w) return;
    std::lock_guard<std::mutex> lk(w->lock);
    w->crop[0] = x;
    w->crop[1] = y;
    w->crop[2] = width;
    w->crop[3] = height;
}

void nvnWindowGetCrop(const NVNwindow* window, NVNrectangle* out) {
    const be::Window* w = be::host_of<be::Window>(window);
    if (!w || !out) return;
    out->x = w->crop[0];
    out->y = w->crop[1];
    out->width = w->crop[2];
    out->height = w->crop[3];
}

int nvnWindowGetNumTextures(const NVNwindow* window) {
    const be::Window* w = be::host_of<be::Window>(window);
    return w ? static_cast<int>(w->textures.size()) : 0;
}

// The count set by nvnWindowSetNumActiveTextures; when none was set (0), every texture is active.
int nvnWindowGetNumActiveTextures(const NVNwindow* window) {
    const be::Window* w = be::host_of<be::Window>(window);
    if (!w) return 0;
    return w->active > 0 ? w->active : static_cast<int>(w->textures.size());
}

void nvnWindowSetNumActiveTextures(NVNwindow* window, int count) {
    be::Window* w = be::host_of<be::Window>(window);
    if (!w) return;
    std::lock_guard<std::mutex> lk(w->lock);
    w->active = count;
}

// NVN2 HDR metadata of a live window (the sdk driver forwards it to the display; here it goes to the HDR output, src/gfx/extras).
void nvnWindowSetHdrDisplayMetadata(NVNwindow* window, float rx, float ry, float gx, float gy, float bx, float by, float wx, float wy, float max_lum,
                                    float min_lum) {
    be::Window* w = be::host_of<be::Window>(window);
    if (!w) return;
    const float v[10] = {rx, ry, gx, gy, bx, by, wx, wy, max_lum, min_lum};
    {
        std::lock_guard<std::mutex> lk(w->lock);
        std::memcpy(w->hdr.display, v, sizeof v);
        w->hdr.has_display = true;
    }
    extras::hdr_note_window_hdr(-1, v, nullptr);
}

void nvnWindowGetHdrDisplayMetadata(const NVNwindow* window, float* rx, float* ry, float* gx, float* gy, float* bx, float* by, float* wx, float* wy,
                                    float* max_lum, float* min_lum) {
    const be::Window* w = be::host_of<be::Window>(window);
    float* out[10] = {rx, ry, gx, gy, bx, by, wx, wy, max_lum, min_lum};
    for (int i = 0; i < 10; ++i)
        if (out[i]) *out[i] = w ? w->hdr.display[i] : 0.0f;
}

void nvnWindowSetHdrContentMetadata(NVNwindow* window, float max_cll, float max_fall) {
    be::Window* w = be::host_of<be::Window>(window);
    if (!w) return;
    {
        std::lock_guard<std::mutex> lk(w->lock);
        w->hdr.content[0] = max_cll;
        w->hdr.content[1] = max_fall;
        w->hdr.has_content = true;
    }
    const float c[2] = {max_cll, max_fall};
    extras::hdr_note_window_hdr(-1, nullptr, c);
}

void nvnWindowGetHdrContentMetadata(const NVNwindow* window, float* max_cll, float* max_fall) {
    const be::Window* w = be::host_of<be::Window>(window);
    if (max_cll) *max_cll = w ? w->hdr.content[0] : 0.0f;
    if (max_fall) *max_fall = w ? w->hdr.content[1] : 0.0f;
}

// ---- loader hooks (no-ops) ----------------------------------------------------------------------------------------------
void nvnLoadCProcs(NVNdevice*, void*) {}
void nvnSetExternalDebuggerEnabledNVX(NVNboolean) {}

}  // namespace

void register_api_device() {
    // device
    NVN_REG(nvnDeviceInitialize);
    NVN_REG(nvnDeviceFinalize);
    NVN_REG(nvnDeviceSetDebugLabel);
    NVN_REG(nvnDeviceSetAutomaticCheckpointsEnable);
    NVN_REG(nvnDeviceSetAutomaticCheckpointsMask);
    NVN_REG(nvnDeviceSetAutomaticCheckpointsSamplingInterval);
    NVN_REG(nvnDeviceSetAutomaticCheckpointsByDebugGroup);
    NVN_REG(nvnDeviceSetCheckpointCallStacksEnable);
    NVN_REG(nvnDeviceSetCheckpointMaxCallStacksDepth);
    NVN_REG(nvnDeviceGetInteger);
    NVN_REG(nvnDeviceGetCurrentTimestampInNanoseconds);
    NVN_REG(nvnDeviceSetIntermediateShaderCache);
    NVN_REG(nvnDeviceGetTextureHandle);
    NVN_REG(nvnDeviceGetTexelFetchHandle);
    NVN_REG(nvnDeviceGetImageHandle);
    NVN_REG(nvnDeviceInstallDebugCallback);
    NVN_REG(nvnDeviceGenerateDebugDomainId);
    NVN_REG(nvnDeviceSetWindowOriginMode);
    NVN_REG(nvnDeviceSetDepthMode);
    NVN_REG(nvnDeviceRegisterFastClearColor);
    NVN_REG(nvnDeviceRegisterFastClearColori);
    NVN_REG(nvnDeviceRegisterFastClearColorui);
    NVN_REG(nvnDeviceRegisterFastClearDepth);
    NVN2_REG(nvnDeviceRegisterFastClearColorIndex);
    NVN2_REG(nvnDeviceRegisterFastClearColoriIndex);
    NVN2_REG(nvnDeviceRegisterFastClearColoruiIndex);
    NVN2_REG(nvnDeviceRegisterFastClearDepthIndex);
    NVN2_REG(nvnDeviceRegisterFastClearStencil);
    NVN2_REG(nvnDeviceRegisterFastClearStencilIndex);
    NVN_REG(nvnDeviceGetWindowOriginMode);
    NVN_REG(nvnDeviceGetDepthMode);
    NVN_REG(nvnDeviceGetTimestampInNanoseconds);
    NVN_REG(nvnDeviceApplyDeferredFinalizes);
    NVN_REG(nvnDeviceFinalizeCommandHandle);
    NVN_REG(nvnDeviceWalkDebugDatabase);
    NVN_REG(nvnDeviceGetSeparateTextureHandle);
    NVN_REG(nvnDeviceGetSeparateSamplerHandle);
    NVN_REG(nvnDeviceIsExternalDebuggerAttached);
    // queue
    NVN_REG(nvnQueueInitialize);
    NVN_REG(nvnQueueFinalize);
    NVN_REG(nvnQueueSetDebugLabel);
    NVN_REG(nvnQueueSubmitCommands);
    NVN_REG(nvnQueueFlush);
    NVN_REG(nvnQueueFinish);
    NVN_REG(nvnQueuePresentTexture);
    NVN_REG(nvnQueueAcquireTexture);
    NVN_REG(nvnQueueGetError);
    NVN_REG(nvnQueueGetTotalCommandMemoryUsed);
    NVN_REG(nvnQueueGetTotalControlMemoryUsed);
    NVN_REG(nvnQueueGetTotalComputeMemoryUsed);
    NVN_REG(nvnQueueResetMemoryUsageCounts);
    NVN_REG(nvnQueueFenceSync);
    NVN_REG(nvnQueueWaitSync);
    // sync
    NVN_REG(nvnSyncInitialize);
    NVN_REG(nvnSyncFinalize);
    NVN_REG(nvnSyncCreateGLSync);
    NVN_REG(nvnSyncInitializeFromFencedGLSync);
    NVN_REG(nvnSyncSetDebugLabel);
    NVN_REG(nvnSyncWait);
    // event
    NVN_REG(nvnEventInitialize);
    NVN_REG(nvnEventFinalize);
    NVN_REG(nvnEventGetValue);
    NVN_REG(nvnEventSignal);
    NVN_REG(nvnEventGetMemoryOffset);
    NVN_REG(nvnEventGetMemoryPool);
    // window
    NVN_REG(nvnWindowInitialize);
    NVN_REG(nvnWindowFinalize);
    NVN_REG(nvnWindowSetDebugLabel);
    NVN_REG(nvnWindowAcquireTexture);
    NVN_REG(nvnWindowGetNativeWindow);
    NVN_REG(nvnWindowGetPresentInterval);
    NVN_REG(nvnWindowSetPresentInterval);
    NVN_REG(nvnWindowSetCrop);
    NVN_REG(nvnWindowGetCrop);
    NVN_REG(nvnWindowGetNumTextures);
    NVN_REG(nvnWindowGetNumActiveTextures);
    NVN_REG(nvnWindowSetNumActiveTextures);
    NVN_REG(nvnWindowSetHdrDisplayMetadata);
    NVN_REG(nvnWindowGetHdrDisplayMetadata);
    NVN_REG(nvnWindowSetHdrContentMetadata);
    NVN_REG(nvnWindowGetHdrContentMetadata);
    // loader hooks
    NVN_REG(nvnLoadCProcs);
    NVN_REG(nvnSetExternalDebuggerEnabledNVX);
}

}  // namespace nxrt::nvn
