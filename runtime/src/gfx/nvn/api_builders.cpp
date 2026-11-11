// SPDX-License-Identifier: GPL-3.0-or-later
// NVN builders: NVNdeviceBuilder, NVNqueueBuilder, NVNwindowBuilder, NVNmemoryPoolBuilder, NVNbufferBuilder and NVNeventBuilder.
// Plain data in the guest object; the layouts below are private to this file (all fields are read and written with memcpy, so
// an object at any alignment works). Each object is at most the size of its NVN struct, checked by the static_asserts.
//
// Layouts (byte offsets inside the guest object):
//   NVNdeviceBuilder (0x40):       +0 flags (i32)
//   NVNqueueBuilder (0x40):        +0 queue memory (u64), +8 its size (u64), +0x10 command memory size, +0x18 compute memory
//                                  size, +0x20 control memory size, +0x28 command flush threshold (all u64), +0x30 flags (i32),
//                                  +0x34 queue priority (i32), +0x38 device (u64)
//   NVNwindowBuilder (0x40):       +0 device (u64), +8 native window (u64), +0x10 four texture pointers (u64 each), +0x30 texture
//                                  count as given (i32), +0x34 present interval (i32), +0x38 active textures (i32)
//   NVNmemoryPoolBuilder (0x40):   +0 device (u64), +8 storage (u64), +0x10 size (u64), +0x18 flags (i32)
//   NVNbufferBuilder (0x40):       +0 device (u64), +8 memory pool (u64), +0x10 offset (i64), +0x18 size (u64)
//   NVNeventBuilder (0x20):        +0 memory pool (u64), +8 offset (i64)
//
// Defaults and bit layout: refs/smo-recomp/runtime/gfx/nvn_gl.c (GPL-3.0), queue, window and event builder layouts and defaults
// checked, no code copied. refs/smo-recomp/runtime/gfx/nvn_stub.c (GPL-3.0), memory pool and buffer builder layouts checked,
// no code copied.
//
// nvnEventBuilderGetStorage and nvnWindowBuilderGetNumActiveTextures have no NVN1 signature; they follow the NVN2 driver (see each).
#include <atomic>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include "nvn_impl.h"
#include "nxrt/log.h"

namespace nxrt::nvn {
namespace {

// ---- raw access to the guest object ----------------------------------------------------------------------------------------
template <class T>
inline T rd(const void* base, size_t off) {
    T v;
    std::memcpy(&v, static_cast<const uint8_t*>(base) + off, sizeof v);
    return v;
}
template <class T>
inline void wr(void* base, size_t off, T v) {
    std::memcpy(static_cast<uint8_t*>(base) + off, &v, sizeof v);
}
inline uint64_t bits_of(const void* p) { return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(p)); }
template <class T>
inline T* ptr_of(uint64_t v) { return reinterpret_cast<T*>(static_cast<uintptr_t>(v)); }
template <class T>
inline void zero_object(T* o) { std::memset(static_cast<void*>(o), 0, sizeof(T)); }

// ---- layout offsets -------------------------------------------------------------------------------------------------------
constexpr size_t kDevFlags = 0;

constexpr size_t kQMemory = 0x00, kQMemorySize = 0x08, kQCommand = 0x10, kQCompute = 0x18, kQControl = 0x20, kQFlush = 0x28,
                 kQFlags = 0x30, kQPriority = 0x34, kQDevice = 0x38;

constexpr size_t kWDevice = 0x00, kWNative = 0x08, kWTextures = 0x10, kWCount = 0x30, kWInterval = 0x34, kWActive = 0x38;

constexpr size_t kPDevice = 0x00, kPStorage = 0x08, kPSize = 0x10, kPFlags = 0x18;

constexpr size_t kBDevice = 0x00, kBPool = 0x08, kBOffset = 0x10, kBSize = 0x18;

constexpr size_t kEPool = 0x00, kEOffset = 0x08;

// Defaults (smo-recomp runtime/gfx/nvn_gl.c): command memory 0x10000, compute 0x40000, control 0x4000, flush threshold 0,
// priority 100 (normal). Window present interval 1.
constexpr uint64_t kDefaultCommandMemory = 0x10000, kDefaultComputeMemory = 0x40000, kDefaultControlMemory = 0x4000;
constexpr int32_t kDefaultQueuePriority = 100;
constexpr int32_t kDefaultPresentInterval = 1;

constexpr int kMaxWindowTextures = 4;

static_assert(kQDevice + 8 <= sizeof(NVNqueueBuilder), "queue builder layout fits the NVN object");
static_assert(kWActive + 4 <= sizeof(NVNwindowBuilder), "window builder layout fits the NVN object");
static_assert(kPFlags + 4 <= sizeof(NVNmemoryPoolBuilder), "memory pool builder layout fits the NVN object");
static_assert(kBSize + 8 <= sizeof(NVNbufferBuilder), "buffer builder layout fits the NVN object");
static_assert(kEOffset + 8 <= sizeof(NVNeventBuilder), "event builder layout fits the NVN object");
static_assert(kDevFlags + 4 <= sizeof(NVNdeviceBuilder), "device builder layout fits the NVN object");

// Rounds a size up to the next multiple of 4096.
inline size_t round_up_4k(size_t v) { return (v + 0xfffu) & ~static_cast<size_t>(0xfffu); }

// ---- NVNdeviceBuilder ------------------------------------------------------------------------------------------------------
void nvnDeviceBuilderSetDefaults(NVNdeviceBuilder* b) { zero_object(b); }
void nvnDeviceBuilderSetFlags(NVNdeviceBuilder* b, int flags) { wr<int32_t>(b, kDevFlags, flags); }
int nvnDeviceBuilderGetFlags(const NVNdeviceBuilder* b) { return rd<int32_t>(b, kDevFlags); }

// ---- NVNqueueBuilder -------------------------------------------------------------------------------------------------------
void nvnQueueBuilderSetDevice(NVNqueueBuilder* b, NVNdevice* d) { wr<uint64_t>(b, kQDevice, bits_of(d)); }
NVNdevice* nvnQueueBuilderGetDevice(const NVNqueueBuilder* b) { return ptr_of<NVNdevice>(rd<uint64_t>(b, kQDevice)); }

void nvnQueueBuilderSetDefaults(NVNqueueBuilder* b) {
    zero_object(b);
    wr<uint64_t>(b, kQCommand, kDefaultCommandMemory);
    wr<uint64_t>(b, kQCompute, kDefaultComputeMemory);
    wr<uint64_t>(b, kQControl, kDefaultControlMemory);
    wr<uint64_t>(b, kQFlush, 0);
    wr<int32_t>(b, kQPriority, kDefaultQueuePriority);
}
void nvnQueueBuilderSetFlags(NVNqueueBuilder* b, int flags) { wr<int32_t>(b, kQFlags, flags); }
int nvnQueueBuilderGetFlags(const NVNqueueBuilder* b) { return rd<int32_t>(b, kQFlags); }

void nvnQueueBuilderSetCommandMemorySize(NVNqueueBuilder* b, size_t size) { wr<uint64_t>(b, kQCommand, size); }
size_t nvnQueueBuilderGetCommandMemorySize(const NVNqueueBuilder* b) { return rd<uint64_t>(b, kQCommand); }
void nvnQueueBuilderSetComputeMemorySize(NVNqueueBuilder* b, size_t size) { wr<uint64_t>(b, kQCompute, size); }
size_t nvnQueueBuilderGetComputeMemorySize(const NVNqueueBuilder* b) { return rd<uint64_t>(b, kQCompute); }
void nvnQueueBuilderSetControlMemorySize(NVNqueueBuilder* b, size_t size) { wr<uint64_t>(b, kQControl, size); }
size_t nvnQueueBuilderGetControlMemorySize(const NVNqueueBuilder* b) { return rd<uint64_t>(b, kQControl); }

// The command, compute and control sizes, each rounded up to 4096, added together.
size_t nvnQueueBuilderGetQueueMemorySize(const NVNqueueBuilder* b) {
    return round_up_4k(rd<uint64_t>(b, kQCommand)) + round_up_4k(rd<uint64_t>(b, kQCompute)) +
           round_up_4k(rd<uint64_t>(b, kQControl));
}

void nvnQueueBuilderSetQueueMemory(NVNqueueBuilder* b, void* memory, size_t size) {
    wr<uint64_t>(b, kQMemory, bits_of(memory));
    wr<uint64_t>(b, kQMemorySize, size);
}
void* nvnQueueBuilderGetMemory(const NVNqueueBuilder* b) { return ptr_of<void>(rd<uint64_t>(b, kQMemory)); }
size_t nvnQueueBuilderGetMemorySize(const NVNqueueBuilder* b) { return rd<uint64_t>(b, kQMemorySize); }

void nvnQueueBuilderSetCommandFlushThreshold(NVNqueueBuilder* b, size_t threshold) { wr<uint64_t>(b, kQFlush, threshold); }
size_t nvnQueueBuilderGetCommandFlushThreshold(const NVNqueueBuilder* b) { return rd<uint64_t>(b, kQFlush); }

// NVN2 only (not in the NVN1 table): nvnQueueBuilderSetPrimaryQueue(builder, queue). title A calls it once at start-up. It names an
// existing queue as the primary one, which the new queue is created next to (NVN2 lets a title have a second queue on the same
// engine). This back end has one Vulkan queue and replays every submission in order, so there is nothing to select: accepted, not stored
// (the builder has no spare room for the pointer) and logged the first time.
void nvnQueueBuilderSetPrimaryQueue(NVNqueueBuilder*, NVNqueue*) {
    static std::atomic<bool> seen{false};
    if (!seen.exchange(true)) NXLOG_I("nvn", "nvnQueueBuilderSetPrimaryQueue: recorded as a no-op, one queue replays everything in order (first)");
}

void nvnQueueBuilderSetQueuePriority(NVNqueueBuilder* b, int priority) { wr<int32_t>(b, kQPriority, priority); }
int nvnQueueBuilderGetQueuePriority(const NVNqueueBuilder* b) { return rd<int32_t>(b, kQPriority); }

// ---- NVNwindowBuilder ------------------------------------------------------------------------------------------------------
void nvnWindowBuilderSetDevice(NVNwindowBuilder* b, NVNdevice* d) { wr<uint64_t>(b, kWDevice, bits_of(d)); }
NVNdevice* nvnWindowBuilderGetDevice(const NVNwindowBuilder* b) { return ptr_of<NVNdevice>(rd<uint64_t>(b, kWDevice)); }

// NVN2 HDR fields of a window builder: kept here by builder address (SetDefaults forgets them), read by decode().
std::mutex g_whdr_mu;
std::unordered_map<const void*, WindowHdrInfo> g_whdr;
WindowHdrInfo builder_hdr(const void* b) {
    std::lock_guard<std::mutex> lk(g_whdr_mu);
    auto it = g_whdr.find(b);
    return it == g_whdr.end() ? WindowHdrInfo{} : it->second;
}
template <class F>
void edit_builder_hdr(const void* b, F f) {
    if (!b) return;
    std::lock_guard<std::mutex> lk(g_whdr_mu);
    f(g_whdr[b]);
}

void nvnWindowBuilderSetDefaults(NVNwindowBuilder* b) {
    zero_object(b);
    wr<int32_t>(b, kWInterval, kDefaultPresentInterval);
    std::lock_guard<std::mutex> lk(g_whdr_mu);
    g_whdr.erase(b);
}

void nvnWindowBuilderSetColorSpace(NVNwindowBuilder* b, int color_space) {
    edit_builder_hdr(b, [&](WindowHdrInfo& h) { h.color_space = color_space; });
}
int nvnWindowBuilderGetColorSpace(const NVNwindowBuilder* b) {
    const int cs = builder_hdr(b).color_space;
    return cs < 0 ? 0 : cs;
}
void nvnWindowBuilderSetHdrDisplayMetadata(NVNwindowBuilder* b, float rx, float ry, float gx, float gy, float bx, float by, float wx, float wy,
                                           float max_lum, float min_lum) {
    edit_builder_hdr(b, [&](WindowHdrInfo& h) {
        const float v[10] = {rx, ry, gx, gy, bx, by, wx, wy, max_lum, min_lum};
        std::memcpy(h.display, v, sizeof v);
        h.has_display = true;
    });
}
void nvnWindowBuilderGetHdrDisplayMetadata(const NVNwindowBuilder* b, float* rx, float* ry, float* gx, float* gy, float* bx, float* by, float* wx,
                                           float* wy, float* max_lum, float* min_lum) {
    const WindowHdrInfo h = builder_hdr(b);
    float* out[10] = {rx, ry, gx, gy, bx, by, wx, wy, max_lum, min_lum};
    for (int i = 0; i < 10; ++i)
        if (out[i]) *out[i] = h.display[i];
}
void nvnWindowBuilderSetHdrContentMetadata(NVNwindowBuilder* b, float max_cll, float max_fall) {
    edit_builder_hdr(b, [&](WindowHdrInfo& h) {
        h.content[0] = max_cll;
        h.content[1] = max_fall;
        h.has_content = true;
    });
}
void nvnWindowBuilderGetHdrContentMetadata(const NVNwindowBuilder* b, float* max_cll, float* max_fall) {
    const WindowHdrInfo h = builder_hdr(b);
    if (max_cll) *max_cll = h.content[0];
    if (max_fall) *max_fall = h.content[1];
}
void nvnWindowBuilderSetNativeWindow(NVNwindowBuilder* b, NVNnativeWindow w) { wr<uint64_t>(b, kWNative, bits_of(w)); }
NVNnativeWindow nvnWindowBuilderGetNativeWindow(const NVNwindowBuilder* b) {
    return ptr_of<void>(rd<uint64_t>(b, kWNative));
}

// Keeps up to four texture pointers (the caller's array may be gone later). The count is stored as given.
void nvnWindowBuilderSetTextures(NVNwindowBuilder* b, int count, NVNtexture* const* textures) {
    const int copied = count < 0 ? 0 : (count > kMaxWindowTextures ? kMaxWindowTextures : count);
    for (int i = 0; i < kMaxWindowTextures; ++i) {
        const uint64_t v = (i < copied && textures) ? bits_of(textures[i]) : 0;
        wr<uint64_t>(b, kWTextures + 8 * static_cast<size_t>(i), v);
    }
    wr<int32_t>(b, kWCount, count);
}
int nvnWindowBuilderGetNumTextures(const NVNwindowBuilder* b) { return rd<int32_t>(b, kWCount); }
NVNtexture* nvnWindowBuilderGetTexture(const NVNwindowBuilder* b, int index) {
    const int count = rd<int32_t>(b, kWCount);
    if (index < 0 || index >= count || index >= kMaxWindowTextures) return nullptr;
    return ptr_of<NVNtexture>(rd<uint64_t>(b, kWTextures + 8 * static_cast<size_t>(index)));
}

void nvnWindowBuilderSetPresentInterval(NVNwindowBuilder* b, int interval) { wr<int32_t>(b, kWInterval, interval); }
int nvnWindowBuilderGetPresentInterval(const NVNwindowBuilder* b) { return rd<int32_t>(b, kWInterval); }

void nvnWindowBuilderSetNumActiveTextures(NVNwindowBuilder* b, int count) { wr<int32_t>(b, kWActive, count); }
// NVN2 (sdk 0x5F1300): int (const NVNwindowBuilder*), the count SetNumActiveTextures stored.
int nvnWindowBuilderGetNumActiveTextures(const NVNwindowBuilder* b) { return rd<int32_t>(b, kWActive); }

// ---- NVNmemoryPoolBuilder --------------------------------------------------------------------------------------------------
void nvnMemoryPoolBuilderSetDevice(NVNmemoryPoolBuilder* b, NVNdevice* d) { wr<uint64_t>(b, kPDevice, bits_of(d)); }
NVNdevice* nvnMemoryPoolBuilderGetDevice(const NVNmemoryPoolBuilder* b) {
    return ptr_of<NVNdevice>(rd<uint64_t>(b, kPDevice));
}
void nvnMemoryPoolBuilderSetDefaults(NVNmemoryPoolBuilder* b) { zero_object(b); }
void nvnMemoryPoolBuilderSetStorage(NVNmemoryPoolBuilder* b, void* memory, size_t size) {
    wr<uint64_t>(b, kPStorage, bits_of(memory));
    wr<uint64_t>(b, kPSize, size);
}
// Returns the storage pointer given to SetStorage (the original header typedef said void; the driver returns void*).
void* nvnMemoryPoolBuilderGetMemory(const NVNmemoryPoolBuilder* b) { return ptr_of<void>(rd<uint64_t>(b, kPStorage)); }
size_t nvnMemoryPoolBuilderGetSize(const NVNmemoryPoolBuilder* b) { return rd<uint64_t>(b, kPSize); }
void nvnMemoryPoolBuilderSetFlags(NVNmemoryPoolBuilder* b, int flags) { wr<int32_t>(b, kPFlags, flags); }
NVNmemoryPoolFlags nvnMemoryPoolBuilderGetFlags(const NVNmemoryPoolBuilder* b) {
    return static_cast<NVNmemoryPoolFlags>(rd<int32_t>(b, kPFlags));
}

// ---- NVNbufferBuilder ------------------------------------------------------------------------------------------------------
void nvnBufferBuilderSetDevice(NVNbufferBuilder* b, NVNdevice* d) { wr<uint64_t>(b, kBDevice, bits_of(d)); }
NVNdevice* nvnBufferBuilderGetDevice(const NVNbufferBuilder* b) { return ptr_of<NVNdevice>(rd<uint64_t>(b, kBDevice)); }
void nvnBufferBuilderSetDefaults(NVNbufferBuilder* b) { zero_object(b); }
void nvnBufferBuilderSetStorage(NVNbufferBuilder* b, NVNmemoryPool* pool, ptrdiff_t offset, size_t size) {
    wr<uint64_t>(b, kBPool, bits_of(pool));
    wr<int64_t>(b, kBOffset, static_cast<int64_t>(offset));
    wr<uint64_t>(b, kBSize, size);
}
const NVNmemoryPool* nvnBufferBuilderGetMemoryPool(const NVNbufferBuilder* b) {
    return reinterpret_cast<const NVNmemoryPool*>(static_cast<uintptr_t>(rd<uint64_t>(b, kBPool)));
}
ptrdiff_t nvnBufferBuilderGetMemoryOffset(const NVNbufferBuilder* b) {
    return static_cast<ptrdiff_t>(rd<int64_t>(b, kBOffset));
}
size_t nvnBufferBuilderGetSize(const NVNbufferBuilder* b) { return rd<uint64_t>(b, kBSize); }

// ---- NVNeventBuilder -------------------------------------------------------------------------------------------------------
void nvnEventBuilderSetDefaults(NVNeventBuilder* b) { zero_object(b); }
void nvnEventBuilderSetStorage(NVNeventBuilder* b, const NVNmemoryPool* pool, int64_t offset) {
    wr<uint64_t>(b, kEPool, bits_of(pool));
    wr<int64_t>(b, kEOffset, offset);
}
const NVNmemoryPool* nvnEventBuilderGetMemoryPool(const NVNeventBuilder* b) {
    return ptr_of<const NVNmemoryPool>(rd<uint64_t>(b, kEPool));
}
ptrdiff_t nvnEventBuilderGetMemoryOffset(const NVNeventBuilder* b) {
    return static_cast<ptrdiff_t>(rd<int64_t>(b, kEOffset));
}
// NVN2 (sdk 0x5E0D00): const NVNmemoryPool* (const NVNeventBuilder*, int64_t* offset): the pool and offset given to SetStorage.
const NVNmemoryPool* nvnEventBuilderGetStorage(const NVNeventBuilder* b, int64_t* offset) {
    if (offset) *offset = rd<int64_t>(b, kEOffset);
    return ptr_of<const NVNmemoryPool>(rd<uint64_t>(b, kEPool));
}

}  // namespace

// ---- decode(): what the back end reads ------------------------------------------------------------------------------------
DeviceBuilderInfo decode(const NVNdeviceBuilder* b) {
    DeviceBuilderInfo i;
    i.flags = rd<int32_t>(b, kDevFlags);
    return i;
}

QueueBuilderInfo decode(const NVNqueueBuilder* b) {
    QueueBuilderInfo i;
    i.device = ptr_of<NVNdevice>(rd<uint64_t>(b, kQDevice));
    i.flags = rd<int32_t>(b, kQFlags);
    i.command_memory = rd<uint64_t>(b, kQCommand);
    i.compute_memory = rd<uint64_t>(b, kQCompute);
    i.control_memory = rd<uint64_t>(b, kQControl);
    i.flush_threshold = rd<uint64_t>(b, kQFlush);
    i.queue_memory = ptr_of<void>(rd<uint64_t>(b, kQMemory));
    i.queue_memory_size = rd<uint64_t>(b, kQMemorySize);
    return i;
}

MemoryPoolBuilderInfo decode(const NVNmemoryPoolBuilder* b) {
    MemoryPoolBuilderInfo i;
    i.device = ptr_of<NVNdevice>(rd<uint64_t>(b, kPDevice));
    i.storage = ptr_of<void>(rd<uint64_t>(b, kPStorage));
    i.size = rd<uint64_t>(b, kPSize);
    i.flags = rd<int32_t>(b, kPFlags);
    return i;
}

BufferBuilderInfo decode(const NVNbufferBuilder* b) {
    BufferBuilderInfo i;
    i.device = ptr_of<NVNdevice>(rd<uint64_t>(b, kBDevice));
    i.pool = ptr_of<NVNmemoryPool>(rd<uint64_t>(b, kBPool));
    i.offset = static_cast<ptrdiff_t>(rd<int64_t>(b, kBOffset));
    i.size = rd<uint64_t>(b, kBSize);
    return i;
}

WindowBuilderInfo decode(const NVNwindowBuilder* b) {
    WindowBuilderInfo i;
    i.hdr = builder_hdr(b);
    i.device = ptr_of<NVNdevice>(rd<uint64_t>(b, kWDevice));
    i.native_window = ptr_of<void>(rd<uint64_t>(b, kWNative));
    i.present_interval = rd<int32_t>(b, kWInterval);
    const int count = rd<int32_t>(b, kWCount);
    i.texture_count = count < 0 ? 0 : (count > kMaxWindowTextures ? kMaxWindowTextures : count);
    for (int n = 0; n < kMaxWindowTextures; ++n) {
        i.textures[n] = n < i.texture_count ? ptr_of<NVNtexture>(rd<uint64_t>(b, kWTextures + 8 * static_cast<size_t>(n))) : nullptr;
    }
    return i;
}

EventBuilderInfo decode(const NVNeventBuilder* b) {
    EventBuilderInfo i;
    i.device = nullptr;   // an event builder has no device
    i.pool = ptr_of<NVNmemoryPool>(rd<uint64_t>(b, kEPool));
    i.offset = static_cast<ptrdiff_t>(rd<int64_t>(b, kEOffset));
    return i;
}

void register_api_builders() {
    NVN_REG(nvnDeviceBuilderSetDefaults);
    NVN_REG(nvnDeviceBuilderSetFlags);
    NVN_REG(nvnDeviceBuilderGetFlags);

    NVN_REG(nvnQueueBuilderSetDevice);
    NVN_REG(nvnQueueBuilderGetDevice);
    NVN_REG(nvnQueueBuilderSetDefaults);
    NVN_REG(nvnQueueBuilderSetFlags);
    NVN_REG(nvnQueueBuilderGetFlags);
    NVN_REG(nvnQueueBuilderSetCommandMemorySize);
    NVN_REG(nvnQueueBuilderGetCommandMemorySize);
    NVN_REG(nvnQueueBuilderSetComputeMemorySize);
    NVN_REG(nvnQueueBuilderGetComputeMemorySize);
    NVN_REG(nvnQueueBuilderSetControlMemorySize);
    NVN_REG(nvnQueueBuilderGetControlMemorySize);
    NVN_REG(nvnQueueBuilderGetQueueMemorySize);
    NVN_REG(nvnQueueBuilderSetQueueMemory);
    NVN_REG(nvnQueueBuilderGetMemory);
    NVN_REG(nvnQueueBuilderGetMemorySize);
    NVN_REG(nvnQueueBuilderSetCommandFlushThreshold);
    NVN_REG(nvnQueueBuilderGetCommandFlushThreshold);
    NVN_REG(nvnQueueBuilderSetQueuePriority);
    NVN2_REG(nvnQueueBuilderSetPrimaryQueue);
    NVN_REG(nvnQueueBuilderGetQueuePriority);

    NVN_REG(nvnWindowBuilderSetDevice);
    NVN_REG(nvnWindowBuilderGetDevice);
    NVN_REG(nvnWindowBuilderSetDefaults);
    NVN_REG(nvnWindowBuilderSetNativeWindow);
    NVN_REG(nvnWindowBuilderGetNativeWindow);
    NVN_REG(nvnWindowBuilderSetTextures);
    NVN_REG(nvnWindowBuilderGetNumTextures);
    NVN_REG(nvnWindowBuilderGetTexture);
    NVN_REG(nvnWindowBuilderSetPresentInterval);
    NVN_REG(nvnWindowBuilderGetPresentInterval);
    NVN_REG(nvnWindowBuilderSetNumActiveTextures);
    NVN_REG(nvnWindowBuilderGetNumActiveTextures);
    NVN_REG(nvnWindowBuilderSetColorSpace);
    NVN_REG(nvnWindowBuilderGetColorSpace);
    NVN_REG(nvnWindowBuilderSetHdrDisplayMetadata);
    NVN_REG(nvnWindowBuilderGetHdrDisplayMetadata);
    NVN_REG(nvnWindowBuilderSetHdrContentMetadata);
    NVN_REG(nvnWindowBuilderGetHdrContentMetadata);

    NVN_REG(nvnMemoryPoolBuilderSetDevice);
    NVN_REG(nvnMemoryPoolBuilderGetDevice);
    NVN_REG(nvnMemoryPoolBuilderSetDefaults);
    NVN_REG(nvnMemoryPoolBuilderSetStorage);
    NVN_REG(nvnMemoryPoolBuilderGetMemory);
    NVN_REG(nvnMemoryPoolBuilderGetSize);
    NVN_REG(nvnMemoryPoolBuilderSetFlags);
    NVN_REG(nvnMemoryPoolBuilderGetFlags);

    NVN_REG(nvnBufferBuilderSetDevice);
    NVN_REG(nvnBufferBuilderGetDevice);
    NVN_REG(nvnBufferBuilderSetDefaults);
    NVN_REG(nvnBufferBuilderSetStorage);
    NVN_REG(nvnBufferBuilderGetMemoryPool);
    NVN_REG(nvnBufferBuilderGetMemoryOffset);
    NVN_REG(nvnBufferBuilderGetSize);

    NVN_REG(nvnEventBuilderSetDefaults);
    NVN_REG(nvnEventBuilderSetStorage);
    NVN_REG(nvnEventBuilderGetMemoryPool);
    NVN_REG(nvnEventBuilderGetMemoryOffset);
    NVN_REG(nvnEventBuilderGetStorage);
}

}  // namespace nxrt::nvn
