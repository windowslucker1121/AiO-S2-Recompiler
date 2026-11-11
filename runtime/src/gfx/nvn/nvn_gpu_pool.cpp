// SPDX-License-Identifier: GPL-3.0-or-later
// Memory pools as Vulkan memory. A pool's storage is the game's own memory (a view of the runtime's pagefile-backed heap); it is
// imported with VK_EXT_external_memory_host and wrapped in one VkBuffer, so a GPU address (= host address) resolves to
// (buffer, offset) with no copy, and anything the GPU writes (compute, copies, queries, events) is visible to the CPU through
// the same pages. Measured on the RTX 3080 and the UHD 770 with MapViewOfFile views: imports work with 4096-byte alignment,
// and GPU writes are visible through every view of the section.
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <cstring>
#include <map>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <shared_mutex>
#include <memory>
#include <string>
#include <thread>
#include <tuple>
#include <vector>
#include <new>
#include <unordered_set>

#include "nxrt/config.h"
#include "nxrt/diag.h"
#include "nxrt/dispatch.h"
#include "nxrt/ftrace.h"
#include "nxrt/log.h"
#include "nxrt/gauge.h"
#include "nxrt/memory.h"
#include "nxrt/write_watch.h"
#include "vk_gauges.h"
#include "nvn_gpu_internal.h"
#include "nvn_mirror_cap.h"
#include "nvn_arch_exp.h"
#include "nvn_capture.h"

namespace nxrt::nvn::be {

namespace gpu { struct Mirror; }

// One Vulkan import of a byte range of a pool: its own private view, VkDeviceMemory and VkBuffer.
struct PoolChunk {
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    void* view = nullptr;             // private small view of the heap section the import goes through (nullptr: imported at its own address)
    uint64_t off = 0;                 // pool offset (from PoolGpu::base) of byte 0 of the import
    VkDeviceSize size = 0;
    uint64_t dev_addr = 0;            // its buffer's device address (bufferDeviceAddress; 0 without): the pool address table
    bool tried = false;               // import attempted
    bool failed = false;
};

// A pool is imported whole (one chunk; the background importer does it) or, when it is larger than one chunk ([nvn] pool_import_chunk_mb), in
// chunks imported on the render thread the first time an address in them is resolved. Chunk k covers pool bytes [k*stride, k*stride + clen):
// neighbouring chunks overlap by clen - stride bytes, so any range that short lies wholly in one chunk. Each chunk goes through its own private
// view of the section (never the same host pointer twice: the spec lets a driver refuse importing the same pointer again). A longer range
// that crosses a stride boundary gets a span: one more import over the strides it touches (rare; logged).
struct PoolGpu {
    uint8_t* base = nullptr;          // aligned start of the pool's imports
    VkDeviceSize size = 0;            // aligned size
    bool failed = false;
    bool viewed = false;              // imports go through private views of the heap section (the mirror needs that)
    int flags = 0;
    uint64_t stride = 0;              // chunk k starts at k * stride (whole import: stride = size)
    uint64_t clen = 0;                // bytes per chunk (the last one may be shorter)
    std::vector<PoolChunk> chunks;
    std::deque<PoolChunk> spans;      // imports of ranges longer than the overlap that cross a stride boundary (deque: stable addresses)
    gpu::Mirror* mirror = nullptr;  // device-local copy of the pages draws read (S38, [nvn] pool_mirror); render thread
    bool mirror_tried = false;
    void* fcopy = nullptr;            // device-local copy kept current by the game's flushes ([nvn] flush_copy); render thread
    bool fcopy_tried = false;
    bool ptr_all = false;             // import_pools_for_pointers tried every chunk already (render thread)
    // Background chunk import ([nvn] pool_prefetch_chunks): while `bg` is set the importer may still import chunks, each under chunk_mu,
    // and ready[k] says (release) that chunk k was tried; once `bg` is clear every chunk field is the render thread's alone again.
    std::atomic<bool> bg{false};
    std::unique_ptr<std::atomic<bool>[]> ready;
    std::mutex chunk_mu;
};

MemoryPool::~MemoryPool() {
    if (gpu) gpu::destroy_pool_gpu(gpu);
    gpu = nullptr;
    if (PoolGpu* pf = prefetched.exchange(nullptr)) gpu::destroy_pool_gpu(pf);   // imported in the background and never used
}

namespace gpu {
namespace {

constexpr const char* kTag = "nvn.pool";
constexpr uint64_t kMirrorChunkBytes = 64ull << 20;   // the mirror's chunk (kChunkBytes below): import strides are a multiple of it

VkBuffer g_dummy = VK_NULL_HANDLE;
gfx::Buffer g_dummy_buf;
std::once_flag g_dummy_once;
std::mutex g_log_mu;
std::unordered_set<uint64_t> g_logged;
std::mutex g_live_mu;
std::unordered_set<PoolGpu*> g_live;
nxrt::GaugeCounter g_gauge_pools_live("nvn.pools_live");   // pools imported and not yet destroyed (g_live)

// [nvn] pool_import_chunk_mb (default 64, rounded up to a multiple of 64; 0 = import every pool whole). A pool larger than one chunk is imported
// in chunks of that many MiB plus a quarter of it overlapping the next chunk, each on the render thread the first time a draw, copy or texture
// touches it. vkAllocateMemory of an import pins every page of it (about a quarter of a millisecond per MiB) and a vkQueueSubmit2 running beside
// it waits: title A's 3500 MiB graphics heap took ~0.9 s as one import, while draws read only a few MiB of it.
uint64_t import_stride() {
    static const uint64_t s = [] {
        const int64_t mb = std::max<int64_t>(0, config_get_int("nvn", "pool_import_chunk_mb", 64));
        const uint64_t b = static_cast<uint64_t>(mb) << 20;
        return b ? (b + kMirrorChunkBytes - 1) / kMirrorChunkBytes * kMirrorChunkBytes : uint64_t{0};
    }();
    return s;
}
uint64_t import_overlap() { return import_stride() / 4; }

// The pool address table (A17): every imported range as {guest address, size, device address of its buffer}. Translated shaders whose global
// memory accesses no bound storage buffer explains look their address up in it (AmpereGlobal.cs) and access the memory through a
// PhysicalStorageBuffer pointer. Game memory is the host memory these ranges import, so a guest address is the host address.
std::mutex g_addr_mu;
std::map<uint64_t, std::pair<uint64_t, uint64_t>> g_addr_by_dev;   // device address -> (guest address, size)
std::atomic<uint64_t> g_addr_gen{0};

// The guest page table (A17; harness side AmpereGlobal.EmitPageWalk): the same imports as one device address per guest page, so a shader's
// pointer access costs a walk of two or three dependent loads instead of a 16-step binary search over the sorted imports. Three levels:
// the root has an entry per 1 GiB (2^17 entries: 47-bit addresses), a node below it an entry per 2 MiB, a leaf node one per 4 KiB page.
// A 2 MiB span that one import holds whole is a leaf in the middle level. Entries: 0 = not mapped, bit 0 = leaf (device address of the
// span's first byte | 1), else the byte offset of the next node from the table's start. The shaders read the layout from c[0]
// (page_table_words), so it can change here without re-translating them.
// The table lives in host-visible memory and is written in place (under g_addr_mu, by the render thread and the background importer), so a
// mapping added while a submission is recorded is seen when it runs. Entries only go from 0 to a value or back to 0 (a range two imports
// hold keeps the first one's address), nodes are never freed, and a table that runs out of room is copied into a larger buffer (the old one
// stays alive: submissions in flight still read it).
namespace pt {
constexpr int kShift[3] = {30, 21, 12};
constexpr uint32_t kRootEntries = 1u << 17;
constexpr uint64_t kNodeEntries = 512;
constexpr uint64_t kRootBytes = uint64_t{kRootEntries} * 8;
constexpr uint64_t kNodeBytes = kNodeEntries * 8;
constexpr uint64_t kSpan1 = uint64_t{1} << kShift[1], kPage = uint64_t{1} << kShift[2];
gfx::Buffer g_buf;
std::vector<gfx::Buffer> g_retired;
uint64_t g_bytes = 0, g_used = 0, g_dev = 0;
bool g_failed = false;
std::atomic<uint64_t> g_dev_pub{0};
std::atomic<uint64_t> g_pages{0}, g_spans{0};   // statistics: page and 2 MiB leaves set

uint64_t* words() { return static_cast<uint64_t*>(g_buf.mapped); }
void put(uint64_t entry_off, uint64_t v) { reinterpret_cast<std::atomic<uint64_t>*>(words())[entry_off / 8].store(v, std::memory_order_release); }

bool grow(uint64_t need) {
    if (g_failed) return false;
    gfx::Device& d = device();
    if (!d.features().buffer_device_address) { g_failed = true; return false; }
    uint64_t bytes = g_bytes ? g_bytes * 2 : (32ull << 20);
    while (bytes < need) bytes *= 2;
    gfx::Buffer nb;
    if (d.create_buffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, gfx::MemoryUse::Upload, &nb, "nvn guest page table") != VK_SUCCESS ||
        !nb.mapped) {
        g_failed = true;
        NXLOG_W(kTag, "guest page table: no buffer of %llu MiB: global memory accesses past the current table read 0", static_cast<unsigned long long>(bytes >> 20));
        return false;
    }
    std::memset(nb.mapped, 0, bytes);
    if (g_buf.buffer) {
        std::memcpy(nb.mapped, g_buf.mapped, g_used);
        g_retired.push_back(g_buf);   // submissions in flight read it (a few MiB; kept until exit)
    }
    g_buf = nb;
    g_bytes = bytes;
    if (!g_used) g_used = kRootBytes;
    g_dev = d.buffer_address(g_buf.buffer);
    g_dev_pub.store(g_dev, std::memory_order_release);
    NXLOG_I(kTag, "guest page table: %llu MiB at device address 0x%llx (%llu KiB used)", static_cast<unsigned long long>(bytes >> 20),
            static_cast<unsigned long long>(g_dev), static_cast<unsigned long long>(g_used >> 10));
    return g_dev != 0;
}

// The node the entry at byte offset entry_off points at, made when `make` and the entry is 0. 0: none (or the entry is a leaf).
uint64_t child(uint64_t entry_off, bool make) {
    const uint64_t e = words()[entry_off / 8];
    if (e & 1) return 0;
    if (e || !make) return e;
    if (g_used + kNodeBytes > g_bytes && !grow(g_used + kNodeBytes)) return 0;
    const uint64_t n = g_used;
    g_used += kNodeBytes;
    put(entry_off, n);
    return n;
}

// Maps the pages of [guest, guest + size) that lie inside [lo, hi) and are not mapped yet to dev + (page - guest).
void map(uint64_t guest, uint64_t size, uint64_t dev, uint64_t lo, uint64_t hi) {
    if (!g_buf.buffer && !grow(0)) return;
    uint64_t cur = (std::max(guest, lo) + kPage - 1) & ~(kPage - 1);
    const uint64_t end = std::min(guest + size, hi) & ~(kPage - 1);
    while (cur < end) {
        const uint64_t ri = cur >> kShift[0];
        if (ri >= kRootEntries) return;
        const uint64_t n1 = child(ri * 8, true);
        if (!n1) return;
        const uint64_t e1 = n1 + ((cur >> kShift[1]) & (kNodeEntries - 1)) * 8;
        const uint64_t span_end = (cur | (kSpan1 - 1)) + 1;
        const uint64_t v1 = words()[e1 / 8];
        if (v1 & 1) { cur = span_end; continue; }          // a 2 MiB leaf of another import holds it all
        if (!v1 && (cur & (kSpan1 - 1)) == 0 && cur + kSpan1 <= end) {
            put(e1, (dev + (cur - guest)) | 1);
            g_spans.fetch_add(1, std::memory_order_relaxed);
            cur = span_end;
            continue;
        }
        const uint64_t n2 = child(e1, true);
        if (!n2) return;
        const uint64_t stop = std::min(span_end, end);
        for (; cur < stop; cur += kPage) {
            const uint64_t e2 = n2 + ((cur >> kShift[2]) & (kNodeEntries - 1)) * 8;
            if (!words()[e2 / 8]) { put(e2, (dev + (cur - guest)) | 1); g_pages.fetch_add(1, std::memory_order_relaxed); }
        }
    }
}

// [nvn] pt_copy: page entries pointed at a device-local copy, with what they held before (by entry offset).
std::unordered_map<uint64_t, uint64_t> g_saved;
uint64_t g_split = 0;
// The entry offset of the 4 KiB leaf of `guest` (a 2 MiB leaf is split into a node of 512 page leaves first: the node is filled before the
// middle entry points at it, so a walk on the GPU meanwhile sees one or the other, both right). 0: not mapped.
uint64_t page_entry(uint64_t guest) {
    if (!g_buf.buffer) return 0;
    const uint64_t ri = guest >> kShift[0];
    if (ri >= kRootEntries) return 0;
    const uint64_t n1 = words()[ri];
    if (!n1 || (n1 & 1)) return 0;
    const uint64_t e1 = n1 + ((guest >> kShift[1]) & (kNodeEntries - 1)) * 8;
    uint64_t v1 = words()[e1 / 8];
    if (!v1) return 0;
    if (v1 & 1) {
        if (g_used + kNodeBytes > g_bytes && !grow(g_used + kNodeBytes)) return 0;
        const uint64_t n2 = g_used;
        g_used += kNodeBytes;
        const uint64_t base = v1 & ~uint64_t{1};
        for (uint64_t k = 0; k < kNodeEntries; ++k) put(n2 + k * 8, (base + k * kPage) | 1);
        put(e1, n2);
        v1 = n2;
        ++g_split;
    }
    return v1 + ((guest >> kShift[2]) & (kNodeEntries - 1)) * 8;
}
// The middle (2 MiB) entry offset of `guest`; 0: none.
uint64_t mid_entry(uint64_t guest) {
    if (!g_buf.buffer) return 0;
    const uint64_t ri = guest >> kShift[0];
    if (ri >= kRootEntries) return 0;
    const uint64_t n1 = words()[ri];
    if (!n1 || (n1 & 1)) return 0;
    return n1 + ((guest >> kShift[1]) & (kNodeEntries - 1)) * 8;
}
void restore_saved(uint64_t e2) {
    if (g_saved.empty()) return;
    const auto it = g_saved.find(e2);
    if (it == g_saved.end()) return;
    put(e2, it->second);
    g_saved.erase(it);
}

// Clears the entries of [guest, guest + size) that point into [dev, dev + size).
void unmap(uint64_t guest, uint64_t size, uint64_t dev) {
    if (!g_buf.buffer) return;
    auto mine = [&](uint64_t v) { const uint64_t a = v & ~uint64_t{1}; return (v & 1) && a >= dev && a < dev + size; };
    uint64_t cur = guest & ~(kPage - 1);
    const uint64_t end = guest + size;
    while (cur < end) {
        const uint64_t ri = cur >> kShift[0];
        if (ri >= kRootEntries) return;
        const uint64_t n1 = child(ri * 8, false);
        if (!n1) { cur = (ri + 1) << kShift[0]; continue; }
        const uint64_t e1 = n1 + ((cur >> kShift[1]) & (kNodeEntries - 1)) * 8;
        const uint64_t span_end = (cur | (kSpan1 - 1)) + 1;
        restore_saved(e1);   // [nvn] pt_copy: a 2 MiB leaf at the copy goes back to its page node first
        const uint64_t v1 = words()[e1 / 8];
        if (v1 & 1) { if (mine(v1)) put(e1, 0); cur = span_end; continue; }
        if (!v1) { cur = span_end; continue; }
        const uint64_t stop = std::min(span_end, end);
        for (; cur < stop; cur += kPage) {
            const uint64_t e2 = v1 + ((cur >> kShift[2]) & (kNodeEntries - 1)) * 8;
            restore_saved(e2);   // [nvn] pt_copy: the import's own address first
            if (mine(words()[e2 / 8])) put(e2, 0);
        }
    }
}
}  // namespace pt

void address_table_add(uint64_t guest, uint64_t size, uint64_t dev) {
    if (!dev) return;
    std::lock_guard<std::mutex> lk(g_addr_mu);
    g_addr_by_dev[dev] = {guest, size};
    pt::map(guest, size, dev, 0, UINT64_MAX);
    g_addr_gen.fetch_add(1);
}
void address_table_remove(uint64_t dev) {
    std::lock_guard<std::mutex> lk(g_addr_mu);
    auto it = g_addr_by_dev.find(dev);
    if (it != g_addr_by_dev.end()) {
        const uint64_t guest = it->second.first, size = it->second.second;
        g_addr_by_dev.erase(it);
        pt::unmap(guest, size, dev);
        // what other imports hold of the range (overlapping chunks of a pool, spans) is theirs again
        for (const auto& [d2, gs] : g_addr_by_dev)
            if (gs.first < guest + size && guest < gs.first + gs.second) pt::map(gs.first, gs.second, d2, guest, guest + size);
    }
    g_addr_gen.fetch_add(1);
}

// Imports pool bytes [off, off + size) of g into c. False (c.failed) when it cannot.
bool import_range(PoolGpu* g, PoolChunk& c, uint64_t off, uint64_t size, const char* what, size_t index, size_t count) {
    c.tried = true;
    c.off = off;
    c.size = size;
    ft::Scope zs(ft::zone_id("pool.import"), 0, static_cast<uint32_t>(g->flags) << 16 | static_cast<uint32_t>(std::min<uint64_t>(size >> 20, 0xffff)));
    nxrt::HitchTimer ht(nxrt::kHpPoolImport);      // the hitch line: an import running beside a slow vkQueueSubmit2 shows here
    gfx::Device& dev = device();
    const uint64_t start = reinterpret_cast<uint64_t>(g->base) + off;

    // Import through a small private view of the same section pages when the range lies in the heap's primary view (see mem::heap_private_view).
    uint8_t* import_ptr = reinterpret_cast<uint8_t*>(start);
    if (g->viewed) {
        const uint64_t soff = start - mem::kHeapBase;
        const uint64_t voff = soff & ~(mem::kAllocGranularity - 1);
        const uint64_t delta = soff - voff;
        const uint64_t vsize = (delta + size + mem::kAllocGranularity - 1) & ~(mem::kAllocGranularity - 1);
        if (void* v = mem::heap_private_view(voff, std::min<uint64_t>(vsize, mem::heap_reserved() - voff))) {
            c.view = v;
            import_ptr = static_cast<uint8_t*>(v) + delta;
        }
    }

    VkMemoryHostPointerPropertiesEXT hp = {VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    VkResult r;
    const auto tm0 = std::chrono::steady_clock::now();
    { NXFT("pool.hostprops"); r = vkGetMemoryHostPointerPropertiesEXT(dev.device(), VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, import_ptr, &hp); }
    if (r != VK_SUCCESS || !hp.memoryTypeBits) {
        NXLOG_E(kTag, "vkGetMemoryHostPointerPropertiesEXT failed (%d) for %p (%s %zu of %zu)", static_cast<int>(r), reinterpret_cast<void*>(start), what, index, count);
        c.failed = true;
        return false;
    }
    uint32_t ty = 0;
    while (!(hp.memoryTypeBits & (1u << ty))) ++ty;
    VkImportMemoryHostPointerInfoEXT imp = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT};
    imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    imp.pHostPointer = import_ptr;
    VkMemoryAllocateFlagsInfo maf = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, nullptr, VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT, 0};
    if (dev.features().buffer_device_address) { maf.pNext = imp.pNext; imp.pNext = &maf; }
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp};
    mai.allocationSize = size;
    mai.memoryTypeIndex = ty;
    const auto tm1 = std::chrono::steady_clock::now();
    { NXFT("pool.alloc"); r = vkAllocateMemory(dev.device(), &mai, nullptr, &c.mem); }
    const auto tm2 = std::chrono::steady_clock::now();
    if (r != VK_SUCCESS) {
        NXLOG_E(kTag, "importing pool %p %s %zu of %zu (offset 0x%llx, 0x%llx bytes) failed (%d)", static_cast<void*>(g->base), what, index, count,
                static_cast<unsigned long long>(off), static_cast<unsigned long long>(size), static_cast<int>(r));
        c.mem = VK_NULL_HANDLE;
        c.failed = true;
        return false;
    }
    vk_gauge_memory_allocs().add(1);
    vk_gauge_memory_bytes().add(static_cast<int64_t>(size));
    VkExternalMemoryBufferCreateInfo ebi = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
    ebi.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &ebi};
    bci.size = size;
    bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT |
                (device().features().transform_feedback ? VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT : 0u) |
                (dev.features().buffer_device_address ? VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT : 0u);
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    r = vkCreateBuffer(dev.device(), &bci, nullptr, &c.buffer);
    if (r == VK_SUCCESS) vk_gauge_buffers().add(1);
    else c.buffer = VK_NULL_HANDLE;
    if (r == VK_SUCCESS) r = vkBindBufferMemory(dev.device(), c.buffer, c.mem, 0);
    if (r != VK_SUCCESS) {
        NXLOG_E(kTag, "buffer over pool %p %s %zu of %zu failed (%d)", static_cast<void*>(g->base), what, index, count, static_cast<int>(r));
        c.failed = true;
        return false;
    }
    counters().pools_imported.fetch_add(1);
    c.dev_addr = dev.buffer_address(c.buffer);
    address_table_add(start, size, c.dev_addr);
    {   // a slow import stalls a vkQueueSubmit2 running beside it (the driver): size and where the time went, to tell page pinning from the rest
        const auto tm3 = std::chrono::steady_clock::now();
        auto msd = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
        const double total = msd(tm0, tm3);
        if (total >= 20.0)
            NXLOG_I(kTag, "slow pool import: %p, %llu MiB (flags 0x%x, %s %zu of %zu, %s): %.1f ms (host pointer properties %.1f, vkAllocateMemory %.1f, buffer and bind %.1f) on %s",
                    reinterpret_cast<void*>(start), static_cast<unsigned long long>(size >> 20), static_cast<unsigned>(g->flags), what, index, count,
                    c.view ? "through a private view" : "at its own address", total, msd(tm0, tm1), msd(tm1, tm2), msd(tm2, tm3),
                    nxrt::hitch_on_render_thread() ? "the render thread" : "the import worker");
    }
    NXLOG_D(kTag, "pool %p %s %zu of %zu imported: offset 0x%llx, 0x%llx bytes, memory type %u", static_cast<void*>(g->base), what, index, count,
            static_cast<unsigned long long>(off), static_cast<unsigned long long>(size), ty);
    return true;
}

void destroy_chunk(PoolChunk& c) {
    gfx::Device& dev = device();
    if (c.dev_addr) address_table_remove(c.dev_addr);
    if (c.buffer) buffer_views_forget(c.buffer);
    if (c.buffer) { vkDestroyBuffer(dev.device(), c.buffer, nullptr); vk_gauge_buffers().sub(1); }
    if (c.mem) {
        vkFreeMemory(dev.device(), c.mem, nullptr);
        vk_gauge_memory_allocs().sub(1);
        vk_gauge_memory_bytes().sub(static_cast<int64_t>(c.size));   // the size the allocation was made with
    }
    if (c.view) mem::heap_private_view_unmap(c.view);
    c = PoolChunk{};
}

// The Vulkan side of a pool. A pool no larger than one chunk (every pool, with [nvn] pool_import_chunk_mb = 0) is imported here, whole; a
// larger one only gets its chunk table here, and its chunks are imported as they are touched (pool_chunk).
PoolGpu* import_pool(MemoryPool* p) {
    auto* g = new PoolGpu();
    g->flags = p->flags;
    gfx::Device& dev = device();
    if (!dev.features().external_memory_host || !p->base || !p->size) {
        g->failed = true;
        NXLOG_E(kTag, "pool at %p size 0x%llx cannot be imported (%s)", static_cast<void*>(p->base), static_cast<unsigned long long>(p->size),
                !dev.features().external_memory_host ? "VK_EXT_external_memory_host is missing" : "no storage");
        return g;
    }
    const uint64_t align = std::max<uint64_t>(dev.limits_ext().min_imported_host_alignment, 4096);
    const uint64_t start = reinterpret_cast<uint64_t>(p->base) & ~(align - 1);
    const uint64_t end = (reinterpret_cast<uint64_t>(p->base) + p->size + align - 1) & ~(align - 1);
    VkDeviceSize size = end - start;
    const uint64_t stride = import_stride();
    const bool chunked = stride && size > stride + import_overlap();
    if (!chunked && size > dev.limits_ext().max_buffer_size && dev.limits_ext().max_buffer_size) {
        NXLOG_W(kTag, "pool of 0x%llx bytes is larger than maxBufferSize 0x%llx: the tail is not reachable by the GPU",
                static_cast<unsigned long long>(size), static_cast<unsigned long long>(dev.limits_ext().max_buffer_size));
        size = dev.limits_ext().max_buffer_size & ~(align - 1);
    }
    g->base = reinterpret_cast<uint8_t*>(start);
    g->size = size;
    static const bool use_view = config_get_bool("nvn", "pool_view", true);      // [nvn] pool_view = 0: import the pool's own address (slow)
    g->viewed = use_view && mem::heap_exists() && start >= mem::kHeapBase && start + size <= mem::kHeapBase + mem::heap_reserved();
    if (chunked) {
        g->stride = stride;
        g->clen = stride + import_overlap();
        g->chunks.resize(static_cast<size_t>((size + stride - 1) / stride));
        NXLOG_I(kTag, "pool %p, %llu MiB (flags 0x%x): imported in %zu chunks of %llu MiB (%llu MiB overlapping the next) as they are first used",
                static_cast<void*>(g->base), static_cast<unsigned long long>(size >> 20), static_cast<unsigned>(p->flags), g->chunks.size(),
                static_cast<unsigned long long>(g->clen >> 20), static_cast<unsigned long long>(import_overlap() >> 20));
    } else {
        g->stride = size;
        g->clen = size;
        g->chunks.resize(1);
        if (!import_range(g, g->chunks[0], 0, size, "whole", 0, 1)) {
            g->failed = true;
            return g;
        }
    }
    { std::lock_guard<std::mutex> lk(g_live_mu); g_live.insert(g); }
    g_gauge_pools_live.add(1);
    return g;
}

// Chunk k of g, imported now if it was not tried yet (render thread). Null when it cannot be imported.
PoolChunk* pool_chunk(PoolGpu* g, uint64_t k) {
    if (k >= g->chunks.size()) return nullptr;
    PoolChunk& c = g->chunks[static_cast<size_t>(k)];
    if (g->bg.load(std::memory_order_acquire)) {
        if (!g->ready[static_cast<size_t>(k)].load(std::memory_order_acquire)) {   // the importer has not reached it: import it here (or wait for its import)
            NXFT("pool.chunk_wait");
            std::lock_guard<std::mutex> lk(g->chunk_mu);
            if (!c.tried) {
                const uint64_t off = k * g->stride;
                import_range(g, c, off, std::min<uint64_t>(g->clen, g->size - off), "chunk", static_cast<size_t>(k), g->chunks.size());
            }
            g->ready[static_cast<size_t>(k)].store(true, std::memory_order_release);
        }
    } else if (!c.tried) {
        const uint64_t off = k * g->stride;
        import_range(g, c, off, std::min<uint64_t>(g->clen, g->size - off), "chunk", static_cast<size_t>(k), g->chunks.size());
    }
    return c.failed || !c.buffer ? nullptr : &c;
}
bool chunk_tried(PoolGpu* g, uint64_t k) {
    if (g->bg.load(std::memory_order_acquire)) return g->ready[static_cast<size_t>(k)].load(std::memory_order_acquire);
    return g->chunks[static_cast<size_t>(k)].tried;
}

// An import holding pool bytes [off, off + size) when no chunk does: an existing span, or a new one over the strides the range touches.
PoolChunk* pool_span(PoolGpu* g, uint64_t off, uint64_t size) {
    for (PoolChunk& s : g->spans)
        if (off >= s.off && off + size <= s.off + s.size) return s.failed || !s.buffer ? nullptr : &s;   // a failed span is not tried again
    const uint64_t s0 = off / g->stride * g->stride;
    const uint64_t s1 = std::min<uint64_t>((off + size + g->stride - 1) / g->stride * g->stride, g->size);
    const uint64_t maxb = device().limits_ext().max_buffer_size;
    if (s1 <= s0 || (maxb && s1 - s0 > maxb)) return nullptr;
    NXLOG_I(kTag, "pool %p: a range of %llu KiB at offset 0x%llx crosses a chunk boundary: span import of %llu MiB at offset 0x%llx",
            static_cast<void*>(g->base), static_cast<unsigned long long>(size >> 10), static_cast<unsigned long long>(off),
            static_cast<unsigned long long>((s1 - s0) >> 20), static_cast<unsigned long long>(s0));
    g->spans.emplace_back();
    PoolChunk& s = g->spans.back();
    if (!import_range(g, s, s0, s1 - s0, "span", g->spans.size() - 1, g->spans.size())) {
        destroy_chunk(s);              // keeps nothing but the range, so the same range is not tried again
        s.off = s0;
        s.size = s1 - s0;
        s.tried = s.failed = true;
        return nullptr;
    }
    return &s;
}

// ---- background import --------------------------------------------------------------------------------------------------
// One worker thread imports pools as they are created. The pool is kept alive by a reference while it is queued or running; the render
// thread, when it needs a pool that is still in flight, waits for it instead of importing a second time.
std::mutex g_pf_mu;
std::condition_variable g_pf_cv;
std::deque<Ref<MemoryPool>> g_pf_queue;
// [nvn] pool_prefetch_chunks: pools larger than one chunk whose chunks the importer imports one at a time when no whole pool waits. The
// pool's chunk table is handed to the render thread first (prefetch state 2, PoolGpu::bg set), so a chunk the render thread needs before
// the importer reached it is imported there (pool_chunk). Pointer shaders need every chunk of every pool (import_pools_for_pointers):
// at an area's or the first scene's first frame that was 1.4-2.9 s of imports on the render thread.
struct ChunkJob { Ref<MemoryPool> pool; PoolGpu* g; size_t next; };
std::deque<ChunkJob> g_pf_chunks;
std::thread g_pf_thread;
bool g_pf_stop = false;
std::atomic<uint64_t> g_pf_chunks_done{0}, g_pf_chunk_bytes{0};   // statistics: chunks the importer imported

bool prefetch_chunks_on() {
    static const bool on = config_get_bool("nvn", "pool_prefetch_chunks", true);
    return on;
}

// One chunk of the front chunk job (importer thread, g_pf_mu not held).
void prefetch_one_chunk(ChunkJob& j) {
    PoolGpu* g = j.g;
    const size_t k = j.next++;
    if (j.pool->dead.load()) return;
    std::lock_guard<std::mutex> lk(g->chunk_mu);
    PoolChunk& c = g->chunks[k];
    if (!c.tried) {
        const uint64_t off = k * g->stride;
        if (import_range(g, c, off, std::min<uint64_t>(g->clen, g->size - off), "chunk", k, g->chunks.size()) && c.buffer) {
            g_pf_chunks_done.fetch_add(1, std::memory_order_relaxed);
            g_pf_chunk_bytes.fetch_add(c.size, std::memory_order_relaxed);
        }
    }
    g->ready[k].store(true, std::memory_order_release);
}

void prefetch_loop() {
    log_set_thread_name("nvn-pool-import");
    for (;;) {
        Ref<MemoryPool> p;
        ChunkJob job{};
        bool chunk = false;
        {
            std::unique_lock<std::mutex> lk(g_pf_mu);
            g_pf_cv.wait(lk, [] { return g_pf_stop || !g_pf_queue.empty() || !g_pf_chunks.empty(); });
            if (g_pf_stop) return;
            if (!g_pf_queue.empty()) {
                p = std::move(g_pf_queue.front());
                g_pf_queue.pop_front();
            } else {
                job = std::move(g_pf_chunks.front());
                g_pf_chunks.pop_front();
                chunk = true;
            }
        }
        if (chunk) {   // whole pools first; between them one chunk at a time
            if (job.next < job.g->chunks.size()) prefetch_one_chunk(job);
            if (job.next >= job.g->chunks.size() || job.pool->dead.load()) {
                job.g->bg.store(false, std::memory_order_release);   // the render thread's alone from here on
                NXLOG_I(kTag, "pool %p, %llu MiB: chunks imported in the background (%llu chunks, %llu MiB by the importer so far)",
                        static_cast<void*>(job.g->base), static_cast<unsigned long long>(job.g->size >> 20),
                        static_cast<unsigned long long>(g_pf_chunks_done.load()), static_cast<unsigned long long>(g_pf_chunk_bytes.load() >> 20));
            } else {
                std::lock_guard<std::mutex> lk(g_pf_mu);
                if (g_pf_stop) job.g->bg.store(false, std::memory_order_release);
                else g_pf_chunks.push_front(std::move(job));
            }
            continue;
        }
        int expect = 1;
        if (!p->prefetch.compare_exchange_strong(expect, 3)) continue;   // the render thread needed it first and imports it itself
        PoolGpu* g = nullptr;
        if (!p->dead.load()) g = import_pool(p.get());
        const bool chunks = g && !g->failed && g->chunks.size() > 1 && prefetch_chunks_on();
        if (chunks) {
            g->ready.reset(new std::atomic<bool>[g->chunks.size()]);
            for (size_t k = 0; k < g->chunks.size(); ++k) g->ready[k].store(false, std::memory_order_relaxed);
            g->bg.store(true, std::memory_order_release);
        }
        {
            std::lock_guard<std::mutex> lk(g_pf_mu);
            p->prefetched.store(g);
            p->prefetch.store(2);
            if (chunks) g_pf_chunks.push_back(ChunkJob{p, g, 0});
        }
        g_pf_cv.notify_all();
    }
}

void make_dummy() {
    gfx::Device& dev = device();
    const VkBufferUsageFlags usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                                     VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT;
    if (dev.create_buffer(65536, usage, gfx::MemoryUse::Upload, &g_dummy_buf, "nvn dummy buffer") == VK_SUCCESS) {
        std::memset(g_dummy_buf.mapped, 0, 65536);
        // What the console delivers for a vertex attribute the game did not set up: (0, 0, 0, 1) in the shader's own type.
        const float f1[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        const uint32_t i1[4] = {0, 0, 0, 1};
        std::memcpy(static_cast<uint8_t*>(g_dummy_buf.mapped) + kDummyAttrFloatOffset, f1, 16);
        std::memcpy(static_cast<uint8_t*>(g_dummy_buf.mapped) + kDummyAttrIntOffset, i1, 16);
        g_dummy = g_dummy_buf.buffer;
    }
}

}  // namespace

void prefetch_pool(MemoryPool* p) {
    static const bool enabled = config_get_bool("nvn", "pool_prefetch", true);   // [nvn] pool_prefetch = false: import on first use only
    if (!enabled || !p || p->is_virtual || !p->base || !p->size || !active()) return;
    // a pool larger than one chunk is imported chunk by chunk: by the importer in its idle time ([nvn] pool_prefetch_chunks), or as the
    // render thread touches it (pool_chunk), never whole
    if (!prefetch_chunks_on() && import_stride() && p->size > import_stride() + import_overlap()) return;
    std::lock_guard<std::mutex> lk(g_pf_mu);
    if (g_pf_stop) return;
    int expect = 0;
    if (!p->prefetch.compare_exchange_strong(expect, 1)) return;
    if (!g_pf_thread.joinable()) g_pf_thread = std::thread(prefetch_loop);
    g_pf_queue.push_back(Ref<MemoryPool>::share(p));
    g_pf_cv.notify_all();
}

uint64_t pool_address_table_generation() { return g_addr_gen.load(); }
bool page_table_words(uint32_t* shifts, uint32_t* root_entries, uint64_t* address) {
    const uint64_t a = pt::g_dev_pub.load(std::memory_order_acquire);
    *shifts = static_cast<uint32_t>(pt::kShift[0]) | static_cast<uint32_t>(pt::kShift[1]) << 8 | static_cast<uint32_t>(pt::kShift[2]) << 16;
    *root_entries = a ? pt::kRootEntries : 0;
    *address = a;
    return a != 0;
}
void page_table_stats(uint64_t* spans, uint64_t* pages) { *spans = pt::g_spans.load(); *pages = pt::g_pages.load(); }
namespace {
BufRef resolve_in_pool(uint64_t gpu_address, uint64_t size, MemoryPool** pool_out, PoolGpu** gpu_out, uint64_t* pool_off_out);
}

void import_pools_for_pointers() {
    static uint64_t done_gen = ~uint64_t{0};
    const uint64_t gen = pool_registry_generation();
    if (gen == done_gen) return;
    done_gen = gen;
    std::vector<Ref<MemoryPool>> pools;
    pool_collect(pools);
    const auto t0 = std::chrono::steady_clock::now();
    uint64_t chunks = 0, bytes = 0;
    for (const Ref<MemoryPool>& r : pools) {
        MemoryPool* mp = r.get();
        if (!mp || mp->is_virtual || !mp->base || !mp->size) continue;
        if (mp->gpu && mp->gpu->ptr_all) continue;   // every chunk tried in an earlier call: nothing new in this pool
        MemoryPool* pool = nullptr;
        PoolGpu* pg = nullptr;
        resolve_in_pool(reinterpret_cast<uint64_t>(mp->base), 1, &pool, &pg, nullptr);   // the pool's import (or chunk table) and its first chunk
        if (pg && (pg->failed || pg->chunks.size() < 2)) { pg->ptr_all = true; continue; }
        if (!pg) continue;
        for (uint64_t k = 0; k < pg->chunks.size(); ++k) {
            const bool had = chunk_tried(pg, k);
            const PoolChunk* c = pool_chunk(pg, k);
            if (!had && c && c->buffer) { ++chunks; bytes += c->size; }
        }
        pg->ptr_all = true;
    }
    if (chunks)
        NXLOG_I(kTag, "pointer shaders: %llu more pool chunks imported (%llu MiB) so the page table maps every pool: %.1f ms", static_cast<unsigned long long>(chunks),
                static_cast<unsigned long long>(bytes >> 20), std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
}

uint64_t page_table_lookup(uint64_t guest) {
    std::lock_guard<std::mutex> lk(g_addr_mu);
    if (!pt::g_buf.buffer) return 0;
    uint64_t node = 0;   // byte offset of the root
    for (int level = 0; level < 3; ++level) {
        const uint64_t mask = level == 0 ? pt::kRootEntries - 1 : pt::kNodeEntries - 1;
        const uint64_t e = pt::words()[(node + ((guest >> pt::kShift[level]) & mask) * 8) / 8];
        if (!e) return 0;
        if (e & 1) return (e & ~uint64_t{1}) + (guest & ((uint64_t{1} << pt::kShift[level]) - 1));
        node = e;
    }
    return 0;
}
size_t pool_address_table(std::vector<PoolAddress>& out) {
    out.clear();
    {
        std::lock_guard<std::mutex> lk(g_addr_mu);
        for (const auto& [dev, gs] : g_addr_by_dev) out.push_back({gs.first, gs.second, dev});
    }
    std::sort(out.begin(), out.end(), [](const PoolAddress& a, const PoolAddress& b) { return a.guest < b.guest; });
    return out.size();
}

VkBuffer dummy_buffer() {
    std::call_once(g_dummy_once, make_dummy);
    return g_dummy;
}

namespace {
BufRef resolve_in_pool(uint64_t gpu_address, uint64_t size, MemoryPool** pool_out, PoolGpu** gpu_out, uint64_t* pool_off_out = nullptr);
}

BufRef resolve_buffer(uint64_t gpu_address, uint64_t size) { return resolve_in_pool(gpu_address, size, nullptr, nullptr); }

namespace {
bool virtual_to_physical(MemoryPool& vp, uint64_t off, uint64_t size, uint64_t* phys_address);
uint64_t physical_buffer_address_uncached(uint64_t gpu_address, uint64_t size);
}

std::atomic<uint64_t> g_vmap_gen{0};
void virtual_maps_changed() { g_vmap_gen.fetch_add(1, std::memory_order_release); }
uint64_t physical_buffer_address(uint64_t gpu_address, uint64_t size) {
    // Per-thread memo (the render thread asks for every uniform and storage binding: ~10k per frame in the village scene, a few hundred
    // distinct ranges). The answer depends only on the pool registry and the virtual pools' mappings: both counters are in the entry.
    struct Memo { uint64_t addr = 0, size = 0, gen = ~0ull, phys = 0; };
    thread_local Memo memo[256];
    // and the last ordinary (not virtual) pool the thread found: per-frame uniform ring addresses are new every frame but lie in a few pools
    struct Last { uint64_t lo = 0, hi = 0, gen = ~0ull; };
    thread_local Last last;
    const uint64_t pgen = pool_registry_generation();
    if (last.gen == pgen && gpu_address >= last.lo && gpu_address < last.hi) return gpu_address;
    const uint64_t gen = pgen * 0x9e3779b97f4a7c15ull ^ g_vmap_gen.load(std::memory_order_acquire);
    Memo& mm = memo[(gpu_address >> 8 ^ gpu_address >> 17 ^ size) & 255];
    if (mm.gen == gen && mm.addr == gpu_address && mm.size == size) return mm.phys;
    const PoolHit hit = pool_lookup(gpu_address);
    if (hit.pool && !hit.pool->is_virtual) {   // an ordinary pool: the address is its own answer
        last = {hit.pool->gpu_base, hit.pool->gpu_base + hit.pool->size, pgen};
        return gpu_address;
    }
    const uint64_t r = physical_buffer_address_uncached(gpu_address, size);
    mm = {gpu_address, size, gen, r};
    return r;
}
namespace {
uint64_t physical_buffer_address_uncached(uint64_t gpu_address, uint64_t size) {
    const PoolHit hit = pool_lookup(gpu_address);
    uint64_t phys = 0;
    if (!hit.pool || !hit.pool->is_virtual || !virtual_to_physical(*hit.pool, hit.offset, size, &phys)) return gpu_address;
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1) < 4)
        NXLOG_I(kTag, "buffer at virtual pool address 0x%llx (+0x%llx) bound through its physical memory 0x%llx", static_cast<unsigned long long>(gpu_address),
                static_cast<unsigned long long>(size), static_cast<unsigned long long>(phys));
    return phys;
}
}  // namespace

namespace {
// A range of a virtual (sparse) pool as the GPU address of its physical backing: the pool's nvnMemoryPoolMapVirtual mappings must cover all of
// it with one physical pool at consecutive offsets (several adjacent mappings are fine). the game keeps a 200 KB storage buffer of its post-process
// chain there (0x200000382000: written by compute f05596b9, read by 54ecc4b4); bound as the dummy buffer, the passes could not talk.
bool virtual_to_physical(MemoryPool& vp, uint64_t off, uint64_t size, uint64_t* phys_address) {
    std::lock_guard<std::mutex> lk(vp.vmu);
    const MemoryPool* phys = nullptr;
    int64_t want_poff = 0;
    uint64_t cur = off;
    const uint64_t end = off + std::max<uint64_t>(size, 1);
    while (cur < end) {
        const MemoryPool::VirtualMap* m = nullptr;
        for (const MemoryPool::VirtualMap& x : vp.vmaps)
            if (x.voff <= cur && cur < x.voff + x.size) { m = &x; break; }
        if (!m || !m->phys || m->phys->is_virtual || !m->phys->base) return false;
        const int64_t poff = m->poff + static_cast<int64_t>(cur - m->voff);
        if (poff < 0) return false;
        if (!phys) {
            phys = m->phys.get();
            *phys_address = phys->gpu_base + static_cast<uint64_t>(poff);
        } else if (m->phys.get() != phys || poff != want_poff) {
            return false;
        }
        want_poff = poff + static_cast<int64_t>(m->voff + m->size - cur);
        cur = m->voff + m->size;
    }
    return phys && static_cast<uint64_t>(want_poff) <= phys->size;   // a physical pool's GPU address is its host address
}

// out.buffer / out.offset name the chunk (or span) import holding the range; *pool_off_out receives the offset from the pool's aligned base
// (what the mirror's pages count from). out.avail is what is left of that import, at least the chunk overlap for a chunked pool.
BufRef resolve_in_pool(uint64_t gpu_address, uint64_t size, MemoryPool** pool_out, PoolGpu** gpu_out, uint64_t* pool_off_out) {
    BufRef out;
    const PoolHit hit = pool_lookup(gpu_address);
    PoolGpu* pg = nullptr;
    if (hit.pool) {
        if (!hit.pool->gpu) {
            MemoryPool* mp = hit.pool;
            int expect = 1;
            if (mp->prefetch.load() == 1 && mp->prefetch.compare_exchange_strong(expect, 4)) {
                // still queued: import it here and now rather than behind the pools queued before it
            } else if (mp->prefetch.load() == 3) {
                NXFT("pool.wait");
                nxrt::HitchTimer ht(nxrt::kHpRenderLockWait);
                std::unique_lock<std::mutex> lk(g_pf_mu);
                g_pf_cv.wait(lk, [mp] { return mp->prefetch.load() == 2 || g_pf_stop; });
            }
            if (mp->prefetch.load() == 2) mp->gpu = mp->prefetched.exchange(nullptr);
            if (!mp->gpu) mp->gpu = import_pool(mp);
            ft::mark("pool.used", static_cast<uint32_t>(mp->flags) << 16 | static_cast<uint32_t>(std::min<uint64_t>(mp->size >> 20, 0xffff)));
        }
        pg = hit.pool->gpu;
    }
    // the import starts at the aligned base, which may be below the pool's own base
    const uint64_t off = pg ? gpu_address - reinterpret_cast<uint64_t>(pg->base) : 0;
    const PoolChunk* c = nullptr;
    if (pg && !pg->failed && off < pg->size) {
        c = pool_chunk(pg, off / pg->stride);
        // a range longer than the overlap that runs past its chunk (and not past the pool): a span import holds all of it
        if (c && size && off + size > c->off + c->size && off + size <= pg->size)
            if (const PoolChunk* sp = pool_span(pg, off, size)) c = sp;
    }
    if (!pg || pg->failed || (off < pg->size && !c)) {
        counters().unresolved_addresses.fetch_add(1);
        std::lock_guard<std::mutex> lk(g_log_mu);
        if (g_logged.insert(gpu_address >> 12).second)
            NXLOG_W(kTag, "GPU address 0x%llx (+0x%llx) is not in an importable memory pool: the dummy buffer is bound", static_cast<unsigned long long>(gpu_address),
                    static_cast<unsigned long long>(size));
        out.buffer = dummy_buffer();
        out.avail = 65536;
        return out;
    }
    if (off >= pg->size) {
        counters().unresolved_addresses.fetch_add(1);
        out.buffer = dummy_buffer();
        out.avail = 65536;
        return out;
    }
    out.buffer = c->buffer;
    out.offset = off - c->off;
    out.avail = c->off + c->size - off;
    out.ok = size <= out.avail;
    if (pool_out) *pool_out = hit.pool;
    if (gpu_out) *gpu_out = pg;
    if (pool_off_out) *pool_off_out = off;
    return out;
}
}  // namespace

void pool_shutdown() {
    {   // stop the importer first: a pool it is working on finishes, queued ones are dropped
        std::unique_lock<std::mutex> lk(g_pf_mu);
        g_pf_stop = true;
        g_pf_queue.clear();
        for (ChunkJob& j : g_pf_chunks) j.g->bg.store(false, std::memory_order_release);
        g_pf_chunks.clear();
    }
    g_pf_cv.notify_all();
    if (g_pf_thread.joinable()) g_pf_thread.join();
    {
        std::lock_guard<std::mutex> lk(g_pf_mu);
        g_pf_stop = false;           // a later back end (tests bring it up and down) starts clean
    }
    std::vector<PoolGpu*> left;
    {
        std::lock_guard<std::mutex> lk(g_live_mu);
        left.assign(g_live.begin(), g_live.end());
    }
    for (PoolGpu* p : left) pool_gpu_destroy(p);
    if (g_dummy_buf.buffer) buffer_views_forget(g_dummy_buf.buffer);
    if (g_dummy_buf.buffer) device().destroy_buffer(g_dummy_buf);
    g_dummy = VK_NULL_HANDLE;
    g_dummy_buf = gfx::Buffer();
    new (&g_dummy_once) std::once_flag();
}

uint64_t pool_imports() { return counters().pools_imported.load(); }

void fc_destroy(PoolGpu* pg);   // flush_copy (below)
void pool_gpu_destroy(PoolGpu* p) {
    if (!p) return;
    bool was_live = false;
    {
        std::lock_guard<std::mutex> lk(g_live_mu);
        was_live = g_live.erase(p) != 0;
        if (!was_live && !p->failed) return;   // already destroyed by pool_shutdown
    }
    if (was_live) g_gauge_pools_live.sub(1);
    if (p->mirror) mirror_destroy(p);
    if (p->fcopy) fc_destroy(p);
    for (PoolChunk& c : p->chunks) destroy_chunk(c);
    for (PoolChunk& c : p->spans) destroy_chunk(c);
    delete p;
}


// ---- device-local copies of pool pages (S38) ---------------------------------------------------------------------
// The game's vertex, index and uniform data live in its memory pools, which are host memory: every draw read them over PCIe (S34). A pool
// that draws read from gets a device-local buffer of the same size (the mirror). A page is copied into it, in a transfer at the start of a
// submission, after some draw wanted it; from then on draws whose whole range is in valid pages read the mirror. A page stops being valid
// when the CPU writes it (the write tracker made it read-only when it was copied, nxrt/write_watch.h) or when the GPU writes it (copies,
// UpdateUniformBuffer, reports, storage buffers: the replay calls note_gpu_write). Until it is copied again, draws that touch it read the
// pool itself, as before. A page written again soon after every copy (a ring buffer the game refills each frame) is "hot": it is left in
// host memory for a while instead of faulting and being copied every frame.
//
// The copy is made of 64 MiB chunks, each created the first time a draw wants one of its pages, so a pool of gigabytes (title A's 3.5 GiB
// graphics heap) costs video memory only for the parts draws read. A bind that spans two chunks reads the pool.
//
// [nvn] pool_mirror = on (default) | off.
constexpr uint64_t kChunkBytes = 64ull << 20;
struct Mirror {
    std::vector<gfx::Buffer> chunks;    // the copy, kChunkBytes each (the last one may be shorter); empty until a draw wants a page in it
    PoolGpu* pg = nullptr;              // the pool (the copies read its chunk imports, mirror_src)
    mem::ww::Region* ww = nullptr;      // the CPU's writes to the pool's pages
    uint64_t npages = 0;
    std::vector<uint64_t> want;         // bitmap: a draw wanted the page while it was not valid
    std::vector<uint32_t> copied_at;    // present count of the last copy (0: never)
    std::vector<uint64_t> used;         // bitmap: a draw read the page from the copy since it was copied
    std::vector<uint8_t> score;         // copies in a row that the CPU overwrote before any draw read them
    std::vector<uint8_t> backoff;       // how often the page has been hot in a row (the hold doubles each time)
    std::vector<uint32_t> hot_until;    // present count until which the page is not copied
    bool any_want = false;
    int pool_flags = 0;
    int64_t st_written = 0, st_copied = 0;   // pages the CPU wrote after a copy, pages copied (summary line)
    uint64_t pool_bytes = 0;
    // compute out (below): pages where a compute result in the copy is newer than the pool, and pages the CPU has read compute results
    // from (any thread sets them; atomics)
    std::unique_ptr<std::atomic<uint64_t>[]> gpu_new, cpu_read;
    // pages mirror_fill copied in this submission without tracking them (hot): current in the copy until the submission ends (render thread)
    std::unique_ptr<std::atomic<uint64_t>[]> fresh;
    // pages whose pool content is a copy-back of the copy (pool == copy there) and that neither the CPU nor a GPU write changed since
    std::unique_ptr<std::atomic<uint64_t>[]> back;
    bool any_fresh = false;
    std::vector<gfx::Buffer> snap_chunks;   // out_delta (below): the snapshots of results before their dispatch, per chunk of the copy
    // [nvn] pt_copy: the guest page table points pages of this pool at the copy (pointer shaders read video memory there)
    bool pt_target = false;
    std::vector<uint64_t> redir;          // bitmap: the page's table entry points at the copy
    uint64_t redir_n = 0;
    std::vector<uint64_t> chunk_dev;      // device address of each chunk of the copy (0: not asked yet)
    std::vector<uint64_t> pt_pending;     // pages copied in (or found valid) that may point at the copy once settled
    bool pt_scan = false;                 // look at every page once (just made a target, or pt_copy switched on again)
};

// Bits [first, last] of Mirror::back (any thread).
void back_put(Mirror& m, uint64_t first, uint64_t last, bool on) {
    if (!m.back || last < first) return;
    last = std::min(last, m.npages - 1);
    for (uint64_t p = first; p <= last;) {
        const uint64_t lo = p & 63, hi = std::min<uint64_t>(63, lo + (last - p));
        const uint64_t mask = (hi - lo == 63) ? ~uint64_t{0} : (((uint64_t{1} << (hi - lo + 1)) - 1) << lo);
        if (on) m.back[static_cast<size_t>(p >> 6)].fetch_or(mask, std::memory_order_acq_rel);
        else m.back[static_cast<size_t>(p >> 6)].fetch_and(~mask, std::memory_order_acq_rel);
        p += hi - lo + 1;
    }
}
bool back_get(const Mirror& m, uint64_t p) { return m.back && ((m.back[static_cast<size_t>(p >> 6)].load(std::memory_order_acquire) >> (p & 63)) & 1); }
std::atomic<uint64_t> g_st_back_pages{0}, g_st_copyin_pages{0};   // copy-ins (pool -> copy) of pages the copy already holds (back) / all

namespace {

enum class MirrorMode { Off, On };
constexpr uint64_t kPageBytes = 4096;
constexpr uint64_t kMaxBindPages = (64ull << 20) / kPageBytes;      // a binding over more than 64 MiB stays in the pool
constexpr uint64_t kCopyBudget = 64ull << 20;                        // bytes copied per submission at most (an area load can want hundreds of MiB)
constexpr uint8_t kHotHits = 2;                                      // two copies in a row overwritten before any draw read them: the page is hot ...
constexpr uint32_t kHotHold = 600;                                   // ... and is not copied for 600 presents, twice that the next time, ...
constexpr uint8_t kHotMaxShift = 4;                                  // ... up to 16 times
constexpr uint32_t kHotWindow = 4;                                   // a write this many presents or fewer after the copy counts even when draws read it

// [nvn] mirror_hot_used (default on): a page the CPU rewrites within kHotWindow presents of its copy, kHotHits times in a row, goes hot even
// when draws read it (per-frame vertices, uniforms and texels). Each such rewrite was a write fault on the game's thread (a vectored exception
// and a VirtualProtect, tens of microseconds) and a copy; hot, the page stays writable and draws read it from the pool, like data the console
// streams, while geometry the game writes once stays in the device-local copy. It is tried again after the hold, like any hot page.
bool hot_used_on() {
    static const bool on = config_get_bool("nvn", "mirror_hot_used", true);
    return on;
}

std::vector<Mirror*> g_mirrors;      // render thread
uint64_t g_mirror_bytes = 0;         // video memory of the chunks made
bool g_mirror_on = false;            // this submission reads mirrors (mode on and, with [nvn] gpuperf_ab, an "on" window)
std::vector<uint64_t> g_written_tmp;
std::vector<VkBufferCopy> g_copy_tmp;

// Arming budget of one submission ([nvn] mirror_arm_ms, default 3; 0 = no limit). Each arm is a VirtualQuery walk and a VirtualProtect; while
// the game streams an area they took the render thread up to 490 ms a frame (hitch line "write-watch arming"). Past the budget no page is
// armed or copied in this submission: draws read those pages from the pool itself (always current), and the copies wait for the next
// submissions. Render thread.
int64_t g_arm_spent_ns = 0;
std::atomic<uint64_t> g_st_arm_deferred{0};
std::atomic<uint64_t> g_st_chunk_bytes_made{0};   // video memory of the copy chunks made, ever (the hitch line: new chunks during a slow frame)

int64_t arm_budget_ns() {
    static const int64_t ns = std::max<int64_t>(0, config_get_int("nvn", "mirror_arm_ms", 3)) * 1000000;
    return ns;
}

bool arm_in_budget() {
    const int64_t b = arm_budget_ns();
    if (!b || g_arm_spent_ns < b) return true;
    g_st_arm_deferred.fetch_add(1, std::memory_order_relaxed);
    return false;
}

bool arm_timed(Mirror& m, uint64_t first, uint64_t count) {
    mem::ww::Region* r = m.ww;
    const auto t0 = std::chrono::steady_clock::now();
    const bool ok = mem::ww::arm(r, first, count);
    g_arm_spent_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
    return ok;
}

std::vector<Mirror*> g_fresh_mirrors;   // mirrors with fresh pages in this submission
std::atomic<int64_t> g_st_hot_fill{0};   // hot pages copied into the copy for one compute use (mirror_fill)
std::atomic<int64_t> g_st_dev_binds{0}, g_st_host_binds{0}, g_st_copied{0}, g_st_hot{0}, g_st_live_bytes{0};
// statistics for the summary line (render thread): binds by use (to the copy / to the pool / not eligible), pages made stale by GPU writes by use
int64_t g_use_dev[3] = {}, g_use_host[3] = {}, g_use_skip[3] = {};
int64_t g_skip_why[5] = {};          // binds not eligible: outside a pool (or past its end), over 64 MiB, pool without a copy, two chunks, no chunk
int64_t g_wr_pages[6] = {}, g_wr_calls[6] = {};
int64_t g_fill_fail[3] = {};         // mirror_fill refused: banned pages, arming over budget or refused, not servable after the fill
std::atomic<uint64_t> g_st_refill_written{0};
std::atomic<uint64_t> g_st_pending_written{0};   // the same, covered by a result of the current submission: current in the copy (fresh)   // pages newer in the copy but written by the CPU, copied in by mirror_fill

MirrorMode mirror_mode() {
    static const MirrorMode m = [] {
        const std::string v = config_get_str("nvn", "pool_mirror", "on");
        if (v == "on" || v == "1" || v == "true") return MirrorMode::On;
        return MirrorMode::Off;
    }();
    return m;
}

// The cap of the copies ([nvn] pool_mirror_mb, or from the card's video memory when it is not set; nvn_mirror_cap.h), logged once.
uint64_t mirror_budget() {
    uint64_t vram = 0;
    bool shared = false;
    if (VkPhysicalDevice pd = device().physical()) {
        VkPhysicalDeviceProperties pp{};
        vkGetPhysicalDeviceProperties(pd, &pp);
        shared = pp.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU || pp.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
        VkPhysicalDeviceMemoryProperties mp{};
        vkGetPhysicalDeviceMemoryProperties(pd, &mp);
        for (uint32_t h = 0; h < mp.memoryHeapCount; ++h)
            if (mp.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) vram = std::max<uint64_t>(vram, mp.memoryHeaps[h].size);
    }
    const int64_t set_mb = config_has("nvn", "pool_mirror_mb") ? std::max<int64_t>(0, config_get_int("nvn", "pool_mirror_mb", 0)) : -1;
    const MirrorCap c = mirror_cap(vram, set_mb, shared);
    NXLOG_I(kTag, "pool copies: cap %llu MiB (%s; largest device-local heap %llu MiB)%s", static_cast<unsigned long long>(c.bytes >> 20), c.why,
            static_cast<unsigned long long>(vram >> 20), c.bytes ? "" : ": no device-local copies");
    return c.bytes;
}

uint64_t mirror_cap_bytes() {
    static const uint64_t b = mirror_budget();
    return b;
}

uint32_t presents_now() { return static_cast<uint32_t>(counters().presents.load(std::memory_order_relaxed)); }

bool bit_get(const std::vector<uint64_t>& v, uint64_t i) { return (v[static_cast<size_t>(i >> 6)] >> (i & 63)) & 1; }
void bit_put(std::vector<uint64_t>& v, uint64_t i, bool on) {
    if (on) v[static_cast<size_t>(i >> 6)] |= uint64_t{1} << (i & 63);
    else v[static_cast<size_t>(i >> 6)] &= ~(uint64_t{1} << (i & 63));
}

bool page_valid(const Mirror& m, uint64_t p) { return mem::ww::page_valid(m.ww, p); }
// Sets or clears bits [first, last] of v a word at a time.
void bits_put(std::vector<uint64_t>& v, uint64_t first, uint64_t last, bool on) {
    for (uint64_t p = first; p <= last;) {
        const uint64_t lo = p & 63, hi = std::min<uint64_t>(63, lo + (last - p));
        const uint64_t mask = (hi - lo == 63) ? ~uint64_t{0} : (((uint64_t{1} << (hi - lo + 1)) - 1) << lo);
        if (on) v[static_cast<size_t>(p >> 6)] |= mask;
        else v[static_cast<size_t>(p >> 6)] &= ~mask;
        p += hi - lo + 1;
    }
}
void out_register(Mirror* m);
bool new_servable(const Mirror& m, uint64_t first, uint64_t last);

bool range_valid(const Mirror& m, uint64_t first, uint64_t count) { return mem::ww::all_valid(m.ww, first, count); }

Mirror* mirror_for(PoolGpu* pg, MemoryPool* mp) {
    if (pg->mirror || pg->mirror_tried) return pg->mirror;
    pg->mirror_tried = true;
    if (!mp || mp->is_virtual || mp->is_shader_code || mp->dead.load() || pg->failed) return nullptr;
    // Tracked copies change the protection of the CPU's view of the pages. With [nvn] pool_view = false the GPU import is that same view;
    // that combination is not tried: no copy.
    if (!pg->viewed) return nullptr;
    const uint64_t budget = mirror_cap_bytes();
    if (budget == 0) return nullptr;

    auto m = std::make_unique<Mirror>();
    // Without tracking of the CPU's writes there is no copy: a copy that misses a write serves old vertices and indices, and after an area
    // load those can point anywhere (an untracked experiment lost the Vulkan device at the first load).
    m->ww = mem::ww::region_create(reinterpret_cast<uint64_t>(pg->base), pg->size);
    if (!m->ww) {
        NXLOG_I(kTag, "pool %p (%llu KiB) gets no device-local copy: its writes cannot be tracked (not in the heap's primary view)",
                static_cast<void*>(pg->base), static_cast<unsigned long long>(pg->size >> 10));
        return nullptr;
    }
    m->chunks.resize(static_cast<size_t>((pg->size + kChunkBytes - 1) / kChunkBytes));
    m->pg = pg;
    m->pool_flags = mp->flags;
    m->npages = pg->size / kPageBytes;
    const size_t words = static_cast<size_t>((m->npages + 63) / 64);
    m->want.assign(words, 0);
    m->copied_at.assign(static_cast<size_t>(m->npages), 0);
    m->used.assign(words, 0);
    m->score.assign(static_cast<size_t>(m->npages), 0);
    m->backoff.assign(static_cast<size_t>(m->npages), 0);
    m->hot_until.assign(static_cast<size_t>(m->npages), 0);
    m->pool_bytes = pg->size;
    m->gpu_new.reset(new std::atomic<uint64_t>[words]);
    m->cpu_read.reset(new std::atomic<uint64_t>[words]);
    m->fresh.reset(new std::atomic<uint64_t>[words]);
    m->back.reset(new std::atomic<uint64_t>[words]);
    for (size_t i = 0; i < words; ++i) {
        m->gpu_new[i].store(0, std::memory_order_relaxed);
        m->cpu_read[i].store(0, std::memory_order_relaxed);
        m->fresh[i].store(0, std::memory_order_relaxed);
        m->back[i].store(0, std::memory_order_relaxed);
    }
    out_register(m.get());
    NXLOG_I(kTag, "pool %p: device-local copy (%llu KiB in %zu chunks made on demand, pool flags 0x%x, %s)", static_cast<void*>(pg->base),
            static_cast<unsigned long long>(pg->size >> 10), m->chunks.size(), static_cast<unsigned>(mp->flags), "writes tracked");
    pg->mirror = m.release();
    g_mirrors.push_back(pg->mirror);
    return pg->mirror;
}

// ---- [nvn] pt_copy (default on; cpuperf_ab_what pt_copy): the guest page table points pointer shaders at the pool copy -----------------
// Pointer shaders (A17 "gpt") read game memory through the page table, which maps the pools' host imports: every such load crosses the bus.
// For pools whose pointers the replay sees in a pointer shader's bound storage buffers (pt_note_pointer), the whole pool is copied (the
// mirror's own copy-in and write tracking) and the table entries of pages that are valid in the copy, hold no newer result and were copied
// at least kPtSettle presents ago (the copy-in has run on the GPU) point at the copy. A GPU write into the pool (note_gpu_write) points its
// pages back at once (before the write is submitted); pages the CPU wrote go back at the next submission start (absorb_writes).
constexpr uint32_t kPtSettle = 4;
int g_pt_on = -1;
uint64_t g_pt_redirects = 0, g_pt_restores = 0, g_pt_targets = 0, g_pt_spans = 0;
bool pt_span_on() {
    static const bool on = config_get_bool("nvn", "pt_copy_spans", true);
    return on;
}
bool pt_mode() {
    static const bool on = config_get_bool("nvn", "pt_copy", true) && device().features().buffer_device_address;
    return on;
}
bool pt_active() { return pt_mode() && g_pt_on != 0; }
void pt_restore_page(Mirror& m, uint64_t p) {
    if (!bit_get(m.redir, p)) return;
    bit_put(m.redir, p, false);
    --m.redir_n;
    ++g_pt_restores;
    std::lock_guard<std::mutex> lk(g_addr_mu);
    const uint64_t g = reinterpret_cast<uint64_t>(m.pg->base) + p * kPageBytes;
    if (const uint64_t e1 = pt::mid_entry(g); e1 && !pt::g_saved.empty()) pt::restore_saved(e1);   // a 2 MiB leaf at the copy: its page node again
    const uint64_t e2 = pt::page_entry(g);
    if (e2) pt::restore_saved(e2);
}
void pt_restore_range(Mirror& m, uint64_t first, uint64_t last) {
    if (m.redir.empty()) return;
    last = std::min(last, m.npages - 1);
    for (uint64_t p = first; p <= last && m.redir_n;) {
        if (!m.redir[static_cast<size_t>(p >> 6)]) { p = (p | 63) + 1; continue; }
        pt_restore_page(m, p);
        ++p;
    }
}
void pt_restore_pages(Mirror& m, const std::vector<uint64_t>& pages) {
    if (m.redir.empty()) return;
    for (uint64_t p : pages)
        if (p < m.npages && bit_get(m.redir, p)) {
            pt_restore_page(m, p);
            bit_put(m.want, p, true);   // copied in again (unless hot), then pointed at the copy again
            m.any_want = true;
        }
}
bool new_get(const Mirror& m, uint64_t p);
bool page_valid(const Mirror& m, uint64_t p);
// Once per present, at the first submission: pages that may point at the copy do, the others go back.
void pt_pass(uint32_t now) {
    static uint32_t last = ~0u;
    if (now == last) return;
    last = now;
    const bool on = pt_active();
    for (Mirror* mp : g_mirrors) {
        Mirror& m = *mp;
        if (!m.pt_target) continue;
        if (m.redir.empty()) m.redir.assign(static_cast<size_t>((m.npages + 63) / 64), 0);
        if (m.chunk_dev.size() != m.chunks.size()) m.chunk_dev.assign(m.chunks.size(), 0);
        if (!on) {
            if (m.redir_n) pt_restore_range(m, 0, m.npages - 1);
            m.pt_scan = true;
            continue;
        }
        static std::vector<uint64_t> todo, keep;
        todo.clear();
        keep.clear();
        if (m.pt_scan) {   // every page once
            m.pt_scan = false;
            for (uint64_t p = 0; p < m.npages; ++p) todo.push_back(p);
            m.pt_pending.clear();
        } else {
            todo.swap(m.pt_pending);
        }
        std::lock_guard<std::mutex> lk(g_addr_mu);
        const uint64_t base = reinterpret_cast<uint64_t>(m.pg->base);
        const uint64_t chunk_pages = kChunkBytes / kPageBytes;
        for (uint64_t p : todo) {
            if (p >= m.npages) continue;
            const bool valid = page_valid(m, p);
            const uint32_t at = m.copied_at[static_cast<size_t>(p)];
            const bool settled = at && now - at >= kPtSettle;
            const bool ok = valid && settled && m.chunks[static_cast<size_t>(p / chunk_pages)].buffer;
            const bool r = bit_get(m.redir, p);
            if (valid && !settled && !r) keep.push_back(p);   // copied lately: next present
            if (ok && !r) {
                const size_t ci = static_cast<size_t>(p / chunk_pages);
                if (!m.chunk_dev[ci]) m.chunk_dev[ci] = device().buffer_address(m.chunks[ci].buffer);
                if (!m.chunk_dev[ci]) continue;
                const uint64_t e2 = pt::page_entry(base + p * kPageBytes);
                if (!e2) continue;
                const uint64_t old = pt::words()[e2 / 8];
                if (!(old & 1)) continue;
                pt::g_saved.emplace(e2, old);
                pt::put(e2, (m.chunk_dev[ci] + (p - ci * chunk_pages) * kPageBytes) | 1);
                bit_put(m.redir, p, true);
                ++m.redir_n;
                ++g_pt_redirects;
            } else if (!ok && r) {
                bit_put(m.redir, p, false);
                --m.redir_n;
                ++g_pt_restores;
                if (const uint64_t e1 = pt::mid_entry(base + p * kPageBytes); e1) pt::restore_saved(e1);
                const uint64_t e2 = pt::page_entry(base + p * kPageBytes);
                if (e2) pt::restore_saved(e2);
            }
            if (!valid && !new_get(m, p)) { bit_put(m.want, p, true); m.any_want = true; }
        }
        m.pt_pending.insert(m.pt_pending.end(), keep.begin(), keep.end());
        // 2 MiB spans whose 512 pages all point at the copy (in one chunk) become one 2 MiB leaf again: a pointer shader's page cache then
        // covers the whole span (one walk instead of one per 4 KiB page)
        if (pt_span_on() && m.redir_n >= 512) {
            for (uint64_t g = (base + (uint64_t{1} << 21) - 1) & ~((uint64_t{1} << 21) - 1); g + (uint64_t{1} << 21) <= base + m.npages * kPageBytes;
                 g += uint64_t{1} << 21) {
                const uint64_t p0 = (g - base) / kPageBytes;
                if (p0 / chunk_pages != (p0 + 511) / chunk_pages || (p0 & 63)) continue;
                bool all = true;
                for (uint64_t w = 0; w < 8 && all; ++w) all = m.redir[static_cast<size_t>((p0 >> 6) + w)] == ~uint64_t{0};
                if (!all) continue;
                const uint64_t e1 = pt::mid_entry(g);
                if (!e1) continue;
                const uint64_t v1 = pt::words()[e1 / 8];
                if ((v1 & 1) || !v1 || pt::g_saved.count(e1)) continue;   // already a leaf (ours or an import's)
                const size_t ci = static_cast<size_t>(p0 / chunk_pages);
                if (!m.chunk_dev[ci]) continue;
                pt::g_saved.emplace(e1, v1);
                pt::put(e1, (m.chunk_dev[ci] + (p0 - ci * chunk_pages) * kPageBytes) | 1);
                ++g_pt_spans;
            }
        }
    }
    static uint32_t last_log = 0;
    if (now - last_log >= 1200) {
        last_log = now;
        uint64_t n = 0, pages = 0;
        for (Mirror* mp : g_mirrors) if (mp->pt_target) { ++n; pages += mp->redir_n; }
        NXLOG_I(kTag, "pt_copy: %llu pools, %llu MiB of pages read from the copy now; %llu redirects, %llu back, %llu spans split, %llu 2 MiB leaves at the copy",
                (unsigned long long)n, (unsigned long long)(pages * kPageBytes >> 20), (unsigned long long)g_pt_redirects,
                (unsigned long long)g_pt_restores, (unsigned long long)pt::g_split, (unsigned long long)g_pt_spans);
    }
}

// The CPU wrote these pages since their copy: they wait for the next draw that wants them. A copy the CPU overwrote before any draw read it
// was wasted (data the game writes for one frame, a ring buffer): after kHotHits of those in a row the page is hot and stays in the pool for
// a hold that doubles every time it comes back.
void absorb_writes(Mirror& m, uint32_t now) {
    g_written_tmp.clear();
    mem::ww::take_written(m.ww, g_written_tmp);
    m.st_written += static_cast<int64_t>(g_written_tmp.size());
    if (m.redir_n) pt_restore_pages(m, g_written_tmp);   // [nvn] pt_copy: the CPU wrote them
    for (uint64_t p : g_written_tmp) {
        const size_t i = static_cast<size_t>(p);
        back_put(m, p, p, false);
        bit_put(m.want, p, false);
        if (m.copied_at[i] == 0) continue;                     // never copied: nothing was wasted
        const bool soon = hot_used_on() && now - m.copied_at[i] <= kHotWindow;   // rewritten right after its copy: per-frame data
        if (bit_get(m.used, p) && !soon) {
            m.score[i] = 0;
            m.backoff[i] = 0;
            continue;
        }
        if (++m.score[i] >= kHotHits) {
            if (m.hot_until[i] <= now) g_st_hot.fetch_add(1, std::memory_order_relaxed);
            m.hot_until[i] = now + (kHotHold << m.backoff[i]);
            if (m.backoff[i] < kHotMaxShift) ++m.backoff[i];
            m.score[i] = 0;
        }
    }
}

bool fresh_get(const Mirror& m, uint64_t p) { return (m.fresh[static_cast<size_t>(p >> 6)].load(std::memory_order_acquire) >> (p & 63)) & 1; }
bool new_get(const Mirror& m, uint64_t p) { return (m.gpu_new[static_cast<size_t>(p >> 6)].load(std::memory_order_acquire) >> (p & 63)) & 1; }

bool copyable(const Mirror& m, uint64_t i, uint32_t now) {
    return bit_get(m.want, i) && !page_valid(m, i) && !new_get(m, i) && m.hot_until[static_cast<size_t>(i)] <= now && !mem::ww::page_banned(m.ww, i);
}

void log_summary(uint32_t now) {
    static uint32_t last = 0;
    if (now - last < 1200) return;
    last = now;
    uint64_t valid = 0, hot = 0;
    for (const Mirror* m : g_mirrors)
        for (uint64_t p = 0; p < m->npages; ++p) {
            valid += page_valid(*m, p) ? 1 : 0;
            hot += m->hot_until[static_cast<size_t>(p)] > now ? 1 : 0;
        }
    NXLOG_I(kTag, "pool copy-ins: %llu pages, %llu of them copied back from the copy before and unchanged since (re-uploads of the GPU's own writes)",
            (unsigned long long)g_st_copyin_pages.load(), (unsigned long long)g_st_back_pages.load());
    NXLOG_I(kTag, "pool mirrors: %zu, %llu MiB, valid %llu MiB, hot %llu KiB; binds to mirrors %lld, to pools %lld; copied %lld MiB; write faults %llu",
            g_mirrors.size(), static_cast<unsigned long long>(g_mirror_bytes >> 20), static_cast<unsigned long long>(valid * kPageBytes >> 20),
            static_cast<unsigned long long>(hot * kPageBytes >> 10), static_cast<long long>(g_st_dev_binds.load()), static_cast<long long>(g_st_host_binds.load()),
            static_cast<long long>(g_st_copied.load() >> 20), static_cast<unsigned long long>(mem::ww::faults()));
    NXLOG_I(kTag, "pool mirror binds (copy/pool/not eligible): vertex %lld/%lld/%lld index %lld/%lld/%lld uniform %lld/%lld/%lld; GPU writes (calls/pages): storage %lld/%lld "
            "stream-out %lld/%lld image copy %lld/%lld update %lld/%lld fill %lld/%lld copy %lld/%lld",
            (long long)g_use_dev[0], (long long)g_use_host[0], (long long)g_use_skip[0], (long long)g_use_dev[1], (long long)g_use_host[1], (long long)g_use_skip[1],
            (long long)g_use_dev[2], (long long)g_use_host[2], (long long)g_use_skip[2], (long long)g_wr_calls[0], (long long)g_wr_pages[0], (long long)g_wr_calls[1],
            (long long)g_wr_pages[1], (long long)g_wr_calls[2], (long long)g_wr_pages[2], (long long)g_wr_calls[3], (long long)g_wr_pages[3], (long long)g_wr_calls[4],
            (long long)g_wr_pages[4], (long long)g_wr_calls[5], (long long)g_wr_pages[5]);
    NXLOG_I(kTag, "pool mirror binds not eligible: outside a pool %lld, over 64 MiB %lld, pool without a copy %lld, across two chunks %lld, no chunk %lld; "
            "fills refused: banned %lld, arming %lld (deferred arms %llu), not servable after %lld; "
            "pages newer in the copy but written by the CPU copied in %llu, current in the copy (this submission's results) %llu",
            (long long)g_skip_why[0], (long long)g_skip_why[1], (long long)g_skip_why[2], (long long)g_skip_why[3], (long long)g_skip_why[4],
            (long long)g_fill_fail[0], (long long)g_fill_fail[1], (unsigned long long)g_st_arm_deferred.load(), (long long)g_fill_fail[2],
            (unsigned long long)g_st_refill_written.load(), (unsigned long long)g_st_pending_written.load());
    std::vector<const Mirror*> top(g_mirrors.begin(), g_mirrors.end());
    std::sort(top.begin(), top.end(), [](const Mirror* a, const Mirror* b) { return a->st_copied > b->st_copied; });
    for (size_t i = 0; i < top.size() && i < 6; ++i) {
        uint64_t v = 0;
        for (uint64_t p = 0; p < top[i]->npages; ++p) v += page_valid(*top[i], p) ? 1 : 0;
        NXLOG_I(kTag, "  mirror of %llu KiB (flags 0x%x): valid %llu KiB, copied %lld KiB, written by the CPU after a copy %lld KiB", (unsigned long long)(top[i]->npages * 4),
                top[i]->pool_flags, (unsigned long long)(v * 4), (long long)(top[i]->st_copied * 4), (long long)(top[i]->st_written * 4));
    }
}

struct MirrorGauges {
    MirrorGauges() {
        nxrt::gauge_register("nvn.mirror.bytes", [] { return g_st_live_bytes.load(std::memory_order_relaxed); });
        nxrt::gauge_register("nvn.mirror.copied_mb", [] { return g_st_copied.load(std::memory_order_relaxed) >> 20; });
        nxrt::gauge_register("nvn.mirror.dev_binds", [] { return g_st_dev_binds.load(std::memory_order_relaxed); });
        nxrt::gauge_register("nvn.mirror.host_binds", [] { return g_st_host_binds.load(std::memory_order_relaxed); });
        nxrt::gauge_register("nvn.mirror.hot_pages", [] { return g_st_hot.load(std::memory_order_relaxed); });
        // the hitch log (nxrt/gauge.h): growth during a slow frame
        nxrt::hitch_add_counter("pool_mirror_copied_kb", []() -> uint64_t { return static_cast<uint64_t>(g_st_copied.load(std::memory_order_relaxed)) >> 10; });
        nxrt::hitch_add_counter("write_faults", []() -> uint64_t { return mem::ww::faults(); });
        nxrt::hitch_add_counter("mirror_arms_deferred", []() -> uint64_t { return g_st_arm_deferred.load(std::memory_order_relaxed); });
        nxrt::hitch_add_counter("pool_mirror_chunks_mb", []() -> uint64_t { return g_st_chunk_bytes_made.load(std::memory_order_relaxed) >> 20; });
        nxrt::hitch_add_counter("pools_imported", []() -> uint64_t { Counters* c = counters_if(); return c ? c->pools_imported.load(std::memory_order_relaxed) : 0; });
    }
    ~MirrorGauges() {
        for (const char* n : {"nvn.mirror.bytes", "nvn.mirror.copied_mb", "nvn.mirror.dev_binds", "nvn.mirror.host_binds", "nvn.mirror.hot_pages"}) nxrt::gauge_unregister(n);
    }
};
MirrorGauges g_mirror_gauges;

}  // namespace

void out_unregister(Mirror* m);
void out_retire();
void out_log();

bool mirror_enabled() { return mirror_mode() != MirrorMode::Off; }

// The chunk of the copy that holds page `page`, made now if it is not yet (null: over the budget or out of video memory).
VkBuffer mirror_chunk(Mirror& m, uint64_t page) {
    const size_t ci = static_cast<size_t>(page * kPageBytes / kChunkBytes);
    if (ci >= m.chunks.size()) return VK_NULL_HANDLE;
    gfx::Buffer& c = m.chunks[ci];
    if (c.buffer) return c.buffer;
    const uint64_t budget = mirror_cap_bytes();
    const uint64_t bytes = std::min<uint64_t>(kChunkBytes, m.pool_bytes - ci * kChunkBytes);
    if (g_mirror_bytes + bytes > budget) {
        static bool logged = false;
        if (!logged) { logged = true; NXLOG_I(kTag, "pool copies: the budget is used up (%llu MiB): further chunks stay in the pools", static_cast<unsigned long long>(g_mirror_bytes >> 20)); }
        return VK_NULL_HANDLE;
    }
    // texel buffer and transfer source usage: compute dispatches bind the copy directly as storage and texel buffers, and their results go
    // back to the pool from it (nvn_replay.cpp, compute shadows)
    const VkBufferUsageFlags usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                     VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT |
                                     (device().features().buffer_device_address ? VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT : 0u);   // [nvn] pt_copy
    if (device().create_buffer(bytes, usage, gfx::MemoryUse::GpuOnly, &c, "nvn pool mirror") != VK_SUCCESS) {
        c = gfx::Buffer{};
        static bool logged = false;
        if (!logged) { logged = true; NXLOG_W(kTag, "a device-local chunk of %llu MiB could not be made: those pages stay in the pool", static_cast<unsigned long long>(bytes >> 20)); }
        return VK_NULL_HANDLE;
    }
    g_mirror_bytes += bytes;
    g_st_chunk_bytes_made.fetch_add(bytes, std::memory_order_relaxed);
    g_st_live_bytes.store(static_cast<int64_t>(g_mirror_bytes), std::memory_order_relaxed);
    return c.buffer;
}

// The pool import the copies of mirror chunk `page` belongs to read from (imported now if it is not yet), and in *base the pool offset of its
// byte 0. A mirror chunk lies wholly in one import chunk: import strides are multiples of kChunkBytes. Null: the pool part cannot be imported.
VkBuffer mirror_src(Mirror& m, uint64_t page, uint64_t* base) {
    const PoolChunk* c = pool_chunk(m.pg, page * kPageBytes / m.pg->stride);
    if (!c) return VK_NULL_HANDLE;
    *base = c->off;
    return c->buffer;
}

// ---- [nvn] flush_copy: device-local copies of CPU_CACHED pools outside the heap's primary view --------------------------------------
// The pools the game maps with MapPhysicalMemory (outside the heap's primary view: ~300 pools of flags 0x24 = CPU_CACHED | GPU_CACHED in
// the village outside) get no mirror, because their CPU writes cannot be tracked (mem::ww regions need the primary view). 72% of the
// vertex and 88% of the index binds of that scene come from them and read host memory over PCIe (G1: ~1.1 ms of GPU per frame). The NVN
// contract for CPU_CACHED pools is that the CPU flushes what it wrote before the GPU may see it (nvnMemoryPoolFlushMappedRange; the E2a
// measurement found every changed page of such pools flushed), so a copy here is kept current by the flushes instead of write tracking:
//   - a flush (any thread) clears the valid bits of its pages; GPU writes into the pool (note_gpu_write) clear them too;
//   - vertex and index reads whose pages are all valid bind the copy; otherwise the pool, and the pages are wanted;
//   - at the start of the next submission the wanted invalid pages are copied from the pool on the GPU and become valid (set before the
//     copy is recorded, so a flush after that point clears them again).
// Only pools imported whole (one chunk) and up to [nvn] flush_copy_max_mb (64) each, at most [nvn] flush_copy_mb (1024) in all.
namespace {
struct FCopy {
    PoolGpu* pg = nullptr;
    uint64_t npages = 0;
    gfx::Buffer buf;
    std::unique_ptr<std::atomic<uint64_t>[]> valid;
    std::vector<uint64_t> want;
    bool any_want = false;
};
std::shared_mutex g_fc_mu;                        // g_fc_by_base (flush threads read it)
std::map<uint64_t, FCopy*> g_fc_by_base;          // pool base address -> copy
std::vector<FCopy*> g_fc_list;                    // render thread
uint64_t g_fc_bytes = 0;
std::atomic<uint64_t> g_fc_flush_pages{0}, g_fc_gpu_pages{0}, g_fc_copied{0}, g_fc_dev{0}, g_fc_host{0};
int g_fc_on = -1;                                 // cpuperf_ab can switch it per window (flush_copy_enable)

bool fc_mode() {
    // a capture replay rewrites the captured memory every loop without flushes: no flush copies there
    static const bool on = config_get_bool("nvn", "flush_copy", !capture::replaying());
    return on;
}
bool fc_active() { return fc_mode() && g_fc_on != 0; }

uint64_t fc_mask(uint64_t lo, uint64_t hi) { return (hi - lo == 63) ? ~uint64_t{0} : (((uint64_t{1} << (hi - lo + 1)) - 1) << lo); }
bool fc_clear(FCopy& f, uint64_t first, uint64_t last) {   // true: a valid page was cleared
    if (first > last || first >= f.npages) return false;
    last = std::min<uint64_t>(last, f.npages - 1);
    bool any = false;
    for (uint64_t p = first; p <= last;) {
        const uint64_t lo = p & 63, hi = std::min<uint64_t>(63, lo + (last - p));
        const uint64_t m = fc_mask(lo, hi);
        any |= (f.valid[static_cast<size_t>(p >> 6)].fetch_and(~m, std::memory_order_acq_rel) & m) != 0;
        p += hi - lo + 1;
    }
    return any;
}
bool fc_all_valid(const FCopy& f, uint64_t first, uint64_t last) {
    for (uint64_t p = first; p <= last;) {
        const uint64_t lo = p & 63, hi = std::min<uint64_t>(63, lo + (last - p));
        const uint64_t mask = fc_mask(lo, hi);
        if ((f.valid[static_cast<size_t>(p >> 6)].load(std::memory_order_acquire) & mask) != mask) return false;
        p += hi - lo + 1;
    }
    return true;
}
FCopy* fc_find_locked(uint64_t addr) {   // g_fc_mu held (shared)
    auto it = g_fc_by_base.upper_bound(addr);
    if (it == g_fc_by_base.begin()) return nullptr;
    --it;
    FCopy* f = it->second;
    return addr < it->first + f->npages * kPageBytes ? f : nullptr;
}
bool fc_gpu_wrote(uint64_t addr, uint64_t size) {   // true: pages a binding may use went stale (the next draw binds again)
    std::shared_lock<std::shared_mutex> lk(g_fc_mu);   // fc_destroy cannot free it meanwhile
    FCopy* f = fc_find_locked(addr);
    if (!f) return false;
    const uint64_t base = reinterpret_cast<uint64_t>(f->pg->base);
    const uint64_t first = (addr - base) / kPageBytes, last = (addr + size - 1 - base) / kPageBytes;
    g_fc_gpu_pages.fetch_add(last - first + 1, std::memory_order_relaxed);
    return fc_clear(*f, first, last);
}
FCopy* fc_for(PoolGpu* pg, MemoryPool* mp) {
    if (pg->fcopy || pg->fcopy_tried) return static_cast<FCopy*>(pg->fcopy);
    pg->fcopy_tried = true;
    if (!mp || mp->is_virtual || mp->is_shader_code || mp->dead.load() || pg->failed || pg->viewed) return nullptr;
    if ((mp->flags & 0x7) != 0x4) return nullptr;   // CPU_CACHED only (the game flushes what it writes)
    if (pg->chunks.size() != 1) return nullptr;
    static const uint64_t max_one = static_cast<uint64_t>(std::max<int64_t>(1, config_get_int("nvn", "flush_copy_max_mb", 64))) << 20;
    static const uint64_t max_all = static_cast<uint64_t>(std::max<int64_t>(0, config_get_int("nvn", "flush_copy_mb", 1024))) << 20;
    if (pg->size > max_one || g_fc_bytes + pg->size > max_all) return nullptr;
    auto f = std::make_unique<FCopy>();
    const VkBufferUsageFlags usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (device().create_buffer(pg->size, usage, gfx::MemoryUse::GpuOnly, &f->buf, "nvn pool flush copy") != VK_SUCCESS) return nullptr;
    f->pg = pg;
    f->npages = pg->size / kPageBytes;
    const size_t words = static_cast<size_t>((f->npages + 63) / 64);
    f->valid.reset(new std::atomic<uint64_t>[words]);
    for (size_t i = 0; i < words; ++i) f->valid[i].store(0, std::memory_order_relaxed);
    f->want.assign(words, 0);
    g_fc_bytes += pg->size;
    FCopy* raw = f.release();
    { std::unique_lock<std::shared_mutex> lk(g_fc_mu); g_fc_by_base[reinterpret_cast<uint64_t>(pg->base)] = raw; }
    g_fc_list.push_back(raw);
    pg->fcopy = raw;
    static int logged = 0;
    if (logged++ < 4) NXLOG_I(kTag, "pool %p (%llu KiB, flags 0x%x): device-local copy kept current by flushes (flush_copy)", static_cast<void*>(pg->base),
                              static_cast<unsigned long long>(pg->size >> 10), static_cast<unsigned>(mp->flags));
    return raw;
}
// Vertex / index reads of a pool without a mirror: the copy when every page is valid (true, out rewritten), else the pages are wanted.
bool fc_serve(PoolGpu* pg, MemoryPool* mp, uint64_t poff, uint64_t size, BufRef& out) {
    if (!fc_active()) return false;
    FCopy* f = fc_for(pg, mp);
    if (!f) return false;
    const uint64_t first = poff / kPageBytes, last = (poff + size - 1) / kPageBytes;
    if (last >= f->npages) return false;
    if (fc_all_valid(*f, first, last)) {
        out.buffer = f->buf.buffer;
        out.offset = poff;
        out.avail = f->npages * kPageBytes - poff;
        g_fc_dev.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    bits_put(f->want, first, last, true);
    f->any_want = true;
    g_fc_host.fetch_add(1, std::memory_order_relaxed);
    return false;
}
// At the start of a submission: the wanted pages that are not valid are copied from the pool (render thread).
void fc_begin_submit(Frame& fr) {
    if (!fc_active()) return;
    static std::vector<VkBufferCopy> runs;
    bool barrier_done = false;
    for (FCopy* fp : g_fc_list) {
        FCopy& f = *fp;
        if (!f.any_want) continue;
        f.any_want = false;
        runs.clear();
        const PoolChunk& c = f.pg->chunks[0];
        if (!c.buffer || c.failed) { std::fill(f.want.begin(), f.want.end(), 0); continue; }
        for (uint64_t p = 0; p < f.npages;) {
            if (!f.want[static_cast<size_t>(p >> 6)]) { p = (p | 63) + 1; continue; }
            if (!bit_get(f.want, p) || fc_all_valid(f, p, p)) { bit_put(f.want, p, false); ++p; continue; }
            uint64_t q = p + 1;
            while (q < f.npages && bit_get(f.want, q) && !fc_all_valid(f, q, q)) ++q;
            for (uint64_t i = p; i < q; ++i) {
                bit_put(f.want, i, false);
                f.valid[static_cast<size_t>(i >> 6)].fetch_or(uint64_t{1} << (i & 63), std::memory_order_acq_rel);   // before the copy: a later flush clears it
            }
            if (p * kPageBytes >= c.off && q * kPageBytes <= c.off + c.size) {
                runs.push_back(VkBufferCopy{p * kPageBytes - c.off, p * kPageBytes, (q - p) * kPageBytes});
                g_fc_copied.fetch_add((q - p) * kPageBytes, std::memory_order_relaxed);
            } else {
                fc_clear(f, p, q - 1);
            }
            p = q;
        }
        if (runs.empty()) continue;
        if (!barrier_done) { memory_barrier(fr.cb); barrier_done = true; }   // earlier GPU writes into the pools and reads of the copies come first
        vkCmdCopyBuffer(fr.cb, c.buffer, f.buf.buffer, static_cast<uint32_t>(runs.size()), runs.data());
    }
    if (barrier_done) {
        VkMemoryBarrier2 b = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        b.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT;
        VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        di.memoryBarrierCount = 1;
        di.pMemoryBarriers = &b;
        vkCmdPipelineBarrier2(fr.cb, &di);
    }
    static uint32_t last_log = 0;
    const uint32_t now = presents_now();
    if (now - last_log >= 1200) {
        last_log = now;
        NXLOG_I(kTag, "flush copies: %zu pools, %llu MiB; vertex/index binds to the copy %llu, to the pool %llu; copied %llu MiB; pages cleared by flushes %llu, by GPU writes %llu",
                g_fc_list.size(), static_cast<unsigned long long>(g_fc_bytes >> 20), static_cast<unsigned long long>(g_fc_dev.load()),
                static_cast<unsigned long long>(g_fc_host.load()), static_cast<unsigned long long>(g_fc_copied.load() >> 20),
                static_cast<unsigned long long>(g_fc_flush_pages.load()), static_cast<unsigned long long>(g_fc_gpu_pages.load()));
    }
}
}  // namespace

void fc_destroy(PoolGpu* pg) {   // pool_gpu_destroy (deferred until the GPU is done with the pool and its copy)
    FCopy* f = static_cast<FCopy*>(pg->fcopy);
    if (!f) return;
    pg->fcopy = nullptr;
    { std::unique_lock<std::shared_mutex> lk(g_fc_mu); g_fc_by_base.erase(reinterpret_cast<uint64_t>(pg->base)); }
    g_fc_list.erase(std::remove(g_fc_list.begin(), g_fc_list.end(), f), g_fc_list.end());
    g_fc_bytes -= std::min<uint64_t>(g_fc_bytes, pg->size);
    if (f->buf.buffer) device().destroy_buffer(f->buf);
    delete f;
}
void flush_copy_cpu_wrote(uint64_t addr, uint64_t n) {   // any thread: nvnMemoryPool/BufferFlushMappedRange
    if (!n || !fc_mode()) return;
    std::shared_lock<std::shared_mutex> lk(g_fc_mu);   // fc_destroy cannot free it meanwhile
    FCopy* f = fc_find_locked(addr);
    if (!f) return;
    const uint64_t base = reinterpret_cast<uint64_t>(f->pg->base);
    const uint64_t first = (addr - base) / kPageBytes, last = (addr + n - 1 - base) / kPageBytes;
    fc_clear(*f, first, last);
    g_fc_flush_pages.fetch_add(last - first + 1, std::memory_order_relaxed);
}
void flush_copy_enable(bool on) { g_fc_on = on ? 1 : 0; }
void pt_copy_enable(bool on) { g_pt_on = on ? 1 : 0; }
// A pointer a pointer shader may follow (render thread): its pool becomes a pt_copy target (copied whole, its pages pointed at the copy).
void pt_note_pointer(uint64_t v) {
    if (!pt_mode() || v < (uint64_t{1} << 32) || v >= (uint64_t{1} << 47)) return;
    const PoolHit hit = pool_lookup(v);
    if (!hit.pool || !hit.pool->gpu) return;
    Mirror* m = mirror_for(hit.pool->gpu, hit.pool);
    if (!m || m->pt_target) return;
    m->pt_target = true;
    m->pt_scan = true;
    ++g_pt_targets;
    bits_put(m->want, 0, m->npages - 1, true);
    m->any_want = true;
    NXLOG_I(kTag, "pt_copy: pool %p (%llu KiB) is read through pointers: copied whole, the page table points at the copy",
            static_cast<void*>(hit.pool->gpu->base), static_cast<unsigned long long>(hit.pool->gpu->size >> 10));
}

BufRef resolve_read(uint64_t gpu_address, uint64_t size, int use) { return resolve_read_dep(gpu_address, size, use, nullptr); }

bool read_dep_check(const ReadDep& d, int use) {
    if (d.mirror_on != g_mirror_on) return false;
    if (d.state == 0) { g_use_skip[use % 3]++; return true; }
    Mirror& m = *d.m;
    const bool serve = range_valid(m, d.first, d.last - d.first + 1) || (out_lazy() && new_servable(m, d.first, d.last));
    if (serve != (d.state == 1)) return false;
    if (serve) {
        bits_put(m.used, d.first, d.last, true);
        g_st_dev_binds.fetch_add(1, std::memory_order_relaxed);
        g_use_dev[use % 3]++;
    } else {
        g_use_host[use % 3]++;
        bits_put(m.want, d.first, d.last, true);
        m.any_want = true;
        g_st_host_binds.fetch_add(1, std::memory_order_relaxed);
    }
    return true;
}

BufRef resolve_read_dep(uint64_t gpu_address, uint64_t size, int use, ReadDep* dep) {
    MemoryPool* mp = nullptr;
    PoolGpu* pg = nullptr;
    uint64_t poff = 0;
    ReadDep scratch;
    ReadDep& d = dep ? *dep : scratch;
    d = ReadDep();
    d.mirror_on = g_mirror_on;
    BufRef out = resolve_in_pool(gpu_address, size, &mp, &pg, &poff);
    d.pool = mp;
    if (!g_mirror_on) return out;
    if (!out.ok || !pg || !size) { g_use_skip[use % 3]++; g_skip_why[0]++; return out; }
    const uint64_t first = poff / kPageBytes, last = (poff + size - 1) / kPageBytes;
    if (last - first + 1 > kMaxBindPages) { g_use_skip[use % 3]++; g_skip_why[1]++; return out; }
    Mirror* m = mirror_for(pg, mp);
    if (!m || last >= m->npages) {
        g_use_skip[use % 3]++; g_skip_why[2]++;
        if (use != kReadUniform && fc_serve(pg, mp, poff, size, out)) d.keep = false;   // flush_copy (vertex and index only)
        return out;
    }
    const uint64_t chunk_pages = kChunkBytes / kPageBytes;
    if (first / chunk_pages != last / chunk_pages) { g_use_skip[use % 3]++; g_skip_why[3]++; return out; }
    const VkBuffer cb = mirror_chunk(*m, first);
    if (!cb) { g_use_skip[use % 3]++; g_skip_why[4]++; d.keep = false; return out; }
    d.m = m;
    d.first = first;
    d.last = last;
    if (range_valid(*m, first, last - first + 1) || (out_lazy() && new_servable(*m, first, last))) {
        d.state = 1;
        out.buffer = cb;
        out.offset = poff - (first / chunk_pages) * kChunkBytes;
        out.avail = std::min<uint64_t>(kChunkBytes, m->pool_bytes - (first / chunk_pages) * kChunkBytes) - out.offset;
        bits_put(m->used, first, last, true);
        g_st_dev_binds.fetch_add(1, std::memory_order_relaxed);
        g_use_dev[use % 3]++;
        return out;
    }
    d.state = 2;
    g_use_host[use % 3]++;
    bits_put(m->want, first, last, true);
    m->any_want = true;
    g_st_host_binds.fetch_add(1, std::memory_order_relaxed);
    return out;
}

MirrorSpan mirror_span(uint64_t gpu_address, uint64_t size) {
    MirrorSpan s;
    if (!g_mirror_on || !size) return s;
    MemoryPool* mp = nullptr;
    PoolGpu* pg = nullptr;
    uint64_t poff = 0;
    const BufRef out = resolve_in_pool(gpu_address, size, &mp, &pg, &poff);
    if (!out.ok || !pg) return s;
    const uint64_t first = poff / kPageBytes, last = (poff + size - 1) / kPageBytes;
    if (last - first + 1 > kMaxBindPages) return s;
    Mirror* m = mirror_for(pg, mp);
    if (!m || last >= m->npages) return s;
    const uint64_t chunk_pages = kChunkBytes / kPageBytes;
    if (first / chunk_pages != last / chunk_pages) return s;
    const VkBuffer cb = mirror_chunk(*m, first);
    if (!cb) return s;
    s.buf = cb;
    s.offset = poff - (first / chunk_pages) * kChunkBytes;
    s.valid = range_valid(*m, first, last - first + 1) || (out_lazy() && new_servable(*m, first, last));
    s.avail = std::min<uint64_t>(kChunkBytes, m->pool_bytes - (first / chunk_pages) * kChunkBytes) - s.offset;
    return s;
}

void mirror_span_written(uint64_t gpu_address, uint64_t size) {
    if (!g_mirror_on || !size) return;
    MemoryPool* mp = nullptr;
    PoolGpu* pg = nullptr;
    uint64_t poff = 0;
    const BufRef out = resolve_in_pool(gpu_address, size, &mp, &pg, &poff);
    if (!out.ok || !pg || !pg->mirror) return;
    Mirror& m = *pg->mirror;
    const uint64_t first = (poff + kPageBytes - 1) / kPageBytes, end = (poff + size) / kPageBytes;   // pages wholly inside
    if (first >= end || end > m.npages) return;
    const uint32_t now = presents_now();
    // per-frame CPU data (hot) and pages with another view (banned) stay untracked; the runs of other pages are tracked again
    auto skip = [&](uint64_t p) { return m.hot_until[static_cast<size_t>(p)] > now || mem::ww::page_banned(m.ww, p); };
    for (uint64_t p = first; p < end;) {
        if (skip(p)) { ++p; continue; }
        uint64_t q = p + 1;
        while (q < end && !skip(q)) ++q;
        if (!arm_in_budget() || !arm_timed(m, p, q - p)) return;   // not armed: the pages stay invalid and are read from the pool
        for (uint64_t i = p; i < q; ++i) {
            m.copied_at[static_cast<size_t>(i)] = now ? now : 1;
            bit_put(m.want, i, false);
        }
        p = q;
    }
}

bool out_pending_on_page(const Mirror* m, uint64_t page);   // below

// [nvn] mirror_refill_written (default off): see stale_new in mirror_fill. Measured 2026-11-08 (first scene): the draws' flushes for GPU
// readers fell from ~800 to ~150 MiB/s, but the results then stayed held longer, the CPU's reads of their first page waited for the GPU
// (write-fault stalls 6 -> 20 per 160 s) and fps fell from ~34 to ~27.
bool refill_written_on() {
    static const bool on = config_get_bool("nvn", "mirror_refill_written", false);
    return on;
}

bool mirror_fill(VkCommandBuffer cb, uint64_t gpu_address, uint64_t size, uint64_t* bytes, bool refill_written) {
    if (bytes) *bytes = 0;
    if (!g_mirror_on || !size) return false;
    MemoryPool* mp = nullptr;
    PoolGpu* pg = nullptr;
    uint64_t poff = 0;
    const BufRef out = resolve_in_pool(gpu_address, size, &mp, &pg, &poff);
    if (!out.ok || !pg || !pg->mirror) return false;
    Mirror& m = *pg->mirror;
    const uint64_t first = poff / kPageBytes, last = (poff + size - 1) / kPageBytes;
    const uint64_t chunk_pages = kChunkBytes / kPageBytes;
    if (last >= m.npages || last - first + 1 > kMaxBindPages || first / chunk_pages != last / chunk_pages) return false;
    const VkBuffer chunk = mirror_chunk(m, first);
    if (!chunk) return false;
    uint64_t src_base = 0;
    const VkBuffer src = mirror_src(m, first, &src_base);
    if (!src) return false;
    const uint32_t now = presents_now();
    for (uint64_t p = first; p <= last; ++p)      // pages with another view (banned) cannot be tracked: the range stays in the pool
        if (!page_valid(m, p) && mem::ww::page_banned(m.ww, p)) { g_fill_fail[0]++; return false; }
    static std::vector<VkBufferCopy> copies;
    copies.clear();
    const uint64_t base = (first / chunk_pages) * kChunkBytes;
    bool ok = true;
    auto hot = [&](uint64_t p) { return m.hot_until[static_cast<size_t>(p)] > now; };
    // A page newer in the copy whose tracking the CPU dropped (it wrote the page) and that no result of this submission covers: the result's
    // copy-back is recorded already (it runs before this submission), and the CPU wrote the pool page after that result, so the pool is the
    // current one. It is copied in for this submission like a hot page (fresh). Before, the whole range went back to the pool instead: the game
    // clears the counters in the first page of its GPU culling buffers (2-15 MB each), and every draw reading one flushed all of it.
    // refill_written (the compute bindings, [nvn] refill_compute): the same for this caller only. the game resets the counters in the first page of
    // its GPU culling buffers every frame; the dispatch that writes them next found that page "newer in the copy" but unarmed, the fill
    // failed, and the range was bound as a shadow read over PCIe from the pool (2-6 MB per buffer), then copied back whole before the
    // indirect dispatch that reads its arguments there (compute out "indirect"). For every caller the refill cost a lot (the draws that
    // read these buffers each broke their pass for it); compute dispatches run outside passes.
    const bool refill = refill_written || refill_written_on();
    auto stale_new = [&](uint64_t x) {
        return new_get(m, x) && !(m.any_fresh && fresh_get(m, x)) && !mem::ww::all_armed(m.ww, x, 1) && refill && !out_pending_on_page(&m, x);
    };
    for (uint64_t p = first; p <= last;) {
        auto have = [&](uint64_t x) { return page_valid(m, x) || (new_get(m, x) && !stale_new(x)) || (m.any_fresh && fresh_get(m, x)); };
        if (have(p)) {
            // The same kind of page, but a result of this submission covers it: the GPU writes it after the CPU did, so the copy is the
            // current one already. It counts as current for this submission (fresh) without a copy.
            if (refill_written_on() && new_get(m, p) && !(m.any_fresh && fresh_get(m, p)) && !page_valid(m, p) && !mem::ww::all_armed(m.ww, p, 1)) {
                m.fresh[static_cast<size_t>(p >> 6)].fetch_or(uint64_t{1} << (p & 63), std::memory_order_acq_rel);
                if (!m.any_fresh) { m.any_fresh = true; g_fresh_mirrors.push_back(&m); }
                g_st_pending_written.fetch_add(1, std::memory_order_relaxed);
            }
            ++p;
            continue;
        }
        // a run of pages to copy in, all hot or all not (pages newer in the copy but written by the CPU count as hot: they are not armed)
        const bool h = hot(p) || stale_new(p);
        uint64_t q = p + 1;
        while (q <= last && !have(q) && (hot(q) || stale_new(q)) == h) ++q;
        if (h && stale_new(p)) g_st_refill_written.fetch_add(q - p, std::memory_order_relaxed);
        bool copy = h;
        if (h) {
            // per-frame CPU data: copied without tracking (arming pages the CPU rewrites every frame costs a fault each), current in the copy
            // for the rest of this submission (fresh); the rest of the range stays tracked, so only these pages cross PCIe again next time
            for (uint64_t i = p; i < q; ++i) m.fresh[static_cast<size_t>(i >> 6)].fetch_or(uint64_t{1} << (i & 63), std::memory_order_acq_rel);
            if (!m.any_fresh) { m.any_fresh = true; g_fresh_mirrors.push_back(&m); }
            g_st_hot_fill.fetch_add(static_cast<int64_t>(q - p), std::memory_order_relaxed);
        } else if (arm_in_budget() && arm_timed(m, p, q - p)) {   // valid from now until the CPU writes a page; the copy below reads the pool when it runs
            for (uint64_t i = p; i < q; ++i) {
                m.copied_at[static_cast<size_t>(i)] = now ? now : 1;
                bit_put(m.used, i, true);
                bit_put(m.want, i, false);
            }
            copy = true;
        } else {
            ok = false;
            g_fill_fail[1]++;
        }
        if (copy) {
            copies.push_back(VkBufferCopy{p * kPageBytes - src_base, p * kPageBytes - base, (q - p) * kPageBytes});
            for (uint64_t i = p; i < q; ++i) g_st_back_pages.fetch_add(back_get(m, i) ? 1 : 0, std::memory_order_relaxed);
            g_st_copyin_pages.fetch_add(q - p, std::memory_order_relaxed);
            m.st_copied += static_cast<int64_t>(q - p);
            g_st_copied.fetch_add(static_cast<int64_t>((q - p) * kPageBytes), std::memory_order_relaxed);
            if (bytes) *bytes += (q - p) * kPageBytes;
        }
        p = q;
    }
    if (!copies.empty()) vkCmdCopyBuffer(cb, src, chunk, static_cast<uint32_t>(copies.size()), copies.data());
    if (!ok) return false;
    if (range_valid(m, first, last - first + 1) || new_servable(m, first, last)) return true;
    g_fill_fail[2]++;
    {   // why (the first few): pages newer in the copy whose tracking the CPU dropped (written since) and that are not fresh
        static int logged = 0;
        if (logged < 40) {
            ++logged;
            uint64_t n_new_unarmed = 0, n_other = 0, first_bad = UINT64_MAX;
            for (uint64_t p = first; p <= last; ++p) {
                if (page_valid(m, p) || (m.any_fresh && fresh_get(m, p))) continue;
                const bool armed = mem::ww::all_armed(m.ww, p, 1);
                if (new_get(m, p) && armed) continue;
                if (new_get(m, p)) ++n_new_unarmed; else ++n_other;
                first_bad = std::min(first_bad, p);
            }
            NXLOG_I(kTag, "pool copy fill: %llx +%llx not servable after the fill: %llu pages newer in the copy but written by the CPU, %llu other, first at page +%llx "
                    "(hot %d, cpu_read %d)", (unsigned long long)gpu_address, (unsigned long long)size, (unsigned long long)n_new_unarmed,
                    (unsigned long long)n_other, (unsigned long long)(first_bad == UINT64_MAX ? 0 : (first_bad - first) * kPageBytes),
                    first_bad != UINT64_MAX && m.hot_until[static_cast<size_t>(first_bad)] > now ? 1 : 0,
                    first_bad != UINT64_MAX ? static_cast<int>((m.cpu_read[static_cast<size_t>(first_bad >> 6)].load() >> (first_bad & 63)) & 1) : 0);
        }
    }
    return false;
}

bool note_gpu_write(uint64_t gpu_address, uint64_t size, int use) {
    if (archexp::coherence_on()) archexp::note_gpu_write(gpu_address, size);   // E2a (measurement only)
    const bool fc_stale = !g_fc_list.empty() && size && fc_gpu_wrote(gpu_address, size);   // flush_copy: what the GPU writes into the pool is not in the copy
    if (g_mirrors.empty() || !size) return fc_stale;
    const PoolHit hit = pool_lookup(gpu_address);
    if (!hit.pool || !hit.pool->gpu || !hit.pool->gpu->mirror) return fc_stale;
    PoolGpu* pg = hit.pool->gpu;
    Mirror& m = *pg->mirror;
    const uint64_t off = gpu_address - reinterpret_cast<uint64_t>(pg->base);
    if (off >= pg->size) return fc_stale;
    const uint64_t first = off / kPageBytes, last = std::min<uint64_t>((off + size - 1) / kPageBytes, m.npages - 1);
    g_wr_calls[use % 6]++;
    g_wr_pages[use % 6] += static_cast<int64_t>(last - first + 1);
    back_put(m, first, last, false);
    if (m.redir_n) pt_restore_range(m, first, last);   // [nvn] pt_copy: the pool gets the GPU's write, pointer reads go there again
    bool stale = mem::ww::any_valid(m.ww, first, last - first + 1);
    mem::ww::invalidate(m.ww, first, last - first + 1);
    if (m.any_fresh) {             // the pool there is written: what fill copied is old now
        for (uint64_t p = first; p <= last; ++p) stale |= ((m.fresh[static_cast<size_t>(p >> 6)].fetch_and(~(uint64_t{1} << (p & 63)), std::memory_order_acq_rel) >> (p & 63)) & 1) != 0;
    }
    bits_put(m.want, first, last, false);
    return stale || fc_stale;
}

void srv_note_submit();
void srv_note_mirror_gone();
void srv_log();
void mirror_begin_submit(Frame& f, bool on) {
    srv_note_submit();   // fresh bits are per submission (servable_cache)
    srv_log();
    fc_begin_submit(f);
    for (Mirror* m : g_fresh_mirrors) {           // fresh pages are current for one submission
        for (size_t i = 0; i < static_cast<size_t>((m->npages + 63) / 64); ++i) m->fresh[i].store(0, std::memory_order_relaxed);
        m->any_fresh = false;
    }
    g_fresh_mirrors.clear();
    g_mirror_on = on && mirror_mode() != MirrorMode::Off;
    out_retire();
    out_log();
    if (!g_mirror_on) {
        if (out_any()) out_flush(f.cb, 0, UINT64_MAX, kOutMirrorOff);   // draws of this submission read the pools: results go back first
        return;
    }
    const uint32_t now = presents_now();
    log_summary(now);
    pt_pass(now);   // [nvn] pt_copy
    uint64_t budget = kCopyBudget;
    bool barrier_done = false;
    g_arm_spent_ns = 0;          // the arming budget is per submission (mirror_fill and mirror_span_written during recording count too)
    for (Mirror* mp : g_mirrors) {
        Mirror& m = *mp;
        absorb_writes(m, now);
        if (!m.any_want) continue;
        if (!arm_in_budget()) continue;   // the wanted pages wait (any_want stays set)
        m.any_want = false;
        g_copy_tmp.clear();
        uint64_t p = 0;
        while (p < m.npages) {
            if (!m.want[static_cast<size_t>(p >> 6)]) { p = (p | 63) + 1; continue; }     // a word with nothing wanted
            if (!bit_get(m.want, p)) { ++p; continue; }
            if (!copyable(m, p, now)) { bit_put(m.want, p, false); ++p; continue; }     // valid already, hot or banned: the next draw asks again
            if (budget < kPageBytes) { m.any_want = true; break; }                      // the rest waits for the next submission
            if (!arm_in_budget()) { m.any_want = true; break; }                         // arming took its time for this submission: the same
            const uint64_t chunk_pages = kChunkBytes / kPageBytes;
            const uint64_t chunk_end = (p / chunk_pages + 1) * chunk_pages;
            uint64_t src_base = 0;
            if (!mirror_chunk(m, p) || !mirror_src(m, p, &src_base)) { bit_put(m.want, p, false); ++p; continue; }
            uint64_t q = p + 1;
            while (q < m.npages && q < chunk_end && copyable(m, q, now) && (q - p + 1) * kPageBytes <= budget) ++q;
            const bool armed = arm_timed(m, p, q - p);
            for (uint64_t i = p; i < q; ++i) {
                bit_put(m.want, i, false);
                if (!armed) continue;
                m.copied_at[static_cast<size_t>(i)] = now ? now : 1;
                bit_put(m.used, i, false);
                if (m.pt_target) m.pt_pending.push_back(i);   // [nvn] pt_copy: points at the copy once settled
            }
            if (armed) {
                for (uint64_t i = p; i < q; ++i) g_st_back_pages.fetch_add(back_get(m, i) ? 1 : 0, std::memory_order_relaxed);
                g_st_copyin_pages.fetch_add(q - p, std::memory_order_relaxed);
                const uint64_t base = (p / chunk_pages) * kChunkBytes;
                g_copy_tmp.push_back(VkBufferCopy{p * kPageBytes, p * kPageBytes - base, (q - p) * kPageBytes});
                budget -= (q - p) * kPageBytes;
                m.st_copied += static_cast<int64_t>(q - p);
                g_st_copied.fetch_add(static_cast<int64_t>((q - p) * kPageBytes), std::memory_order_relaxed);
            }
            p = q;
        }
        if (g_copy_tmp.empty()) continue;
        if (!barrier_done) {
            memory_barrier(f.cb);      // earlier GPU writes into the pools, and draws still reading the mirrors, come first
            barrier_done = true;
        }
        // the source is the pool's own buffer: the copy reads what the CPU and earlier GPU work left in the pages. The runs are in pool
        // order and never span two chunks: one copy command per chunk.
        for (size_t i = 0; i < g_copy_tmp.size();) {
            const size_t ci = static_cast<size_t>(g_copy_tmp[i].srcOffset / kChunkBytes);
            size_t j = i + 1;
            while (j < g_copy_tmp.size() && g_copy_tmp[j].srcOffset / kChunkBytes == ci) ++j;
            uint64_t src_base = 0;
            const VkBuffer src = mirror_src(m, g_copy_tmp[i].srcOffset / kPageBytes, &src_base);   // imported when the run was made
            for (size_t k = i; k < j; ++k) g_copy_tmp[k].srcOffset -= src_base;
            if (src) vkCmdCopyBuffer(f.cb, src, m.chunks[ci].buffer, static_cast<uint32_t>(j - i), g_copy_tmp.data() + i);
            i = j;
        }
    }
    if (barrier_done) {
        VkMemoryBarrier2 b = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        b.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT;
        VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        di.memoryBarrierCount = 1;
        di.pMemoryBarriers = &b;
        vkCmdPipelineBarrier2(f.cb, &di);
        if (gpass_on()) gpass_stamp(f, "mirror", 0);      // GPU pass profiler: the copies of this submission
    }
}

void snaps_forget_locked(Mirror* m);   // below

void mirror_destroy(PoolGpu* p) {
    Mirror* m = p->mirror;
    if (!m) return;
    srv_note_mirror_gone();
    if (m->redir_n) pt_restore_range(*m, 0, m->npages - 1);   // [nvn] pt_copy
    p->mirror = nullptr;
    snaps_forget_locked(m);
    g_fresh_mirrors.erase(std::remove(g_fresh_mirrors.begin(), g_fresh_mirrors.end(), m), g_fresh_mirrors.end());
    out_unregister(m);
    g_mirrors.erase(std::remove(g_mirrors.begin(), g_mirrors.end(), m), g_mirrors.end());
    if (m->ww) mem::ww::region_destroy(m->ww);
    for (gfx::Buffer& c : m->chunks)
        if (c.buffer) {
            forget_buffer_views(c.buffer);     // compute texel buffers bound to the copy
            g_mirror_bytes -= std::min<uint64_t>(g_mirror_bytes, c.size);
            device().destroy_buffer(c);
        }
    g_st_live_bytes.store(static_cast<int64_t>(g_mirror_bytes), std::memory_order_relaxed);
    delete m;
}


// ---- compute out: results of direct compute bindings held in the copy (nvn_gpu_internal.h) ---------------------------------------
// The copy-back of compute results to the pool streamed ~0.8 GB/s over PCIe, while most results are only read by later GPU work. With
// [nvn] compute_out_lazy (default on) a result stays in the copy after its submission: its pages are marked newer-in-the-copy (gpu_new)
// and guarded (PAGE_NOACCESS, mem::ww::guard). Draws bind the copy for them (resolve_read); a GPU reader of the pool gets them copied back
// first, in queue order (out_flush, called by the replay before such a command); the CPU's first touch of a guarded page faults, and the
// handler waits for the submission that wrote the result and copies it back on its own small submission (out_fetch, also called for
// nvnMemoryPool/BufferInvalidateMappedRange). A page the CPU read once (cpu_read) has its later results copied back at the end of their
// submission, as before, so a result the game reads every frame costs one wait, not one per frame.
namespace {

struct OutEntry {
    uint64_t addr = 0;
    VkDeviceSize size = 0;
    Mirror* m = nullptr;
    uint64_t first = 0, last = 0;       // pool pages it touches
    VkBuffer mbuf = VK_NULL_HANDLE;     // its place in the copy
    VkDeviceSize moff = 0;
    VkBuffer hbuf = VK_NULL_HANDLE;     // its place in the pool's own buffer
    VkDeviceSize hoff = 0;
    bool pending = false;               // written by the submission being recorded
    bool guarded = false;               // its pages are guarded
    uint64_t serial = 0;                // the submission whose result the copy holds (0: only the one being recorded)
    uint64_t flushed = 0;               // a copy-back is recorded in this submission: the guard goes when it completes
    bool partial = false;               // some of its pages went back already (out_split): copy-backs take only the pages still newer (gpu_new)
    uint64_t id = 0;                    // unique, so out_fetch finds it again after the lock was free
    uint64_t gen = 0;                   // moves when a later write takes it over again (out_add): the result out_fetch copied is no longer the newest
    bool fetching = false;              // out_fetch copies it back with the lock free: other fetches wait (g_out_cv), retire and unregister leave it
    bool snapped = false;               // out_delta: the snapshot chunk holds the range as it was before its first write (delta copy-back)
};

std::mutex g_out_mu;                    // g_out, g_out_mirrors; never held while touching guest memory
std::vector<OutEntry> g_out;
std::vector<Mirror*> g_out_mirrors;
std::atomic<size_t> g_out_n{0};
std::atomic<uint64_t> g_out_lo{UINT64_MAX}, g_out_hi{0};   // address bounds of the entries (render thread; a quick filter)
std::condition_variable g_out_cv;       // an entry stopped fetching
std::atomic<uint64_t> g_fetch_busy_waits{0}, g_fetch_overtaken{0};   // fetches that waited for another one; entries written again meanwhile
bool fetch_unlocked_on() {
    static const bool on = config_get_bool("nvn", "out_fetch_unlocked", true);
    static const int64_t ab = config_get_int("nvn", "out_fetch_ab", 0);
    if (!on) return false;
    if (ab <= 0) return true;
    return ((counters().presents.load(std::memory_order_relaxed) / static_cast<uint64_t>(ab)) & 1) != 0;
}
uint64_t g_out_next_id = 1;             // g_out_mu held

// Render-thread waits for g_out_mu (the 10 s line): the guard handler used to hold it across a whole submission's fence wait.
std::atomic<uint64_t> g_lock_wait_ns{0}, g_lock_waits{0}, g_lock_wait_max_ns{0};
struct OutLock {
    std::unique_lock<std::mutex> lk;
    OutLock() : lk(g_out_mu, std::defer_lock) {
        if (lk.try_lock()) return;
        const auto t0 = std::chrono::steady_clock::now();
        lk.lock();
        if (!on_render_thread()) return;
        const uint64_t ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
        g_lock_wait_ns.fetch_add(ns, std::memory_order_relaxed);
        g_lock_waits.fetch_add(1, std::memory_order_relaxed);
        uint64_t m = g_lock_wait_max_ns.load(std::memory_order_relaxed);
        while (ns > m && !g_lock_wait_max_ns.compare_exchange_weak(m, ns, std::memory_order_relaxed)) {}
    }
};

// ---- the interval index of the held results (render thread) ---------------------------------------------------------------------------
// out_overlaps and its kin ran a linear scan of g_out under g_out_mu, thousands of times per frame (~15% of the render thread). Now every
// change of an entry's set membership moves g_out_ver (g_out_mu held), and the render thread keeps sorted, merged page intervals of the
// entries per question, rebuilt (under the lock) only when it finds the version moved. A query is then a binary search without the lock.
// Changes on other threads (out_fetch, out_unregister) only take entries out or make them less live, so a snapshot a moment old answers
// "overlaps" where it no longer does, never the other way round: the callers then find nothing to flush. [nvn] out_index = false (or the
// off windows of [nvn] out_index_ab = N presents) scans as before.
std::atomic<uint64_t> g_out_ver{1};
void out_changed() { g_out_ver.fetch_add(1, std::memory_order_acq_rel); }
// Moves only when the live set may gain pages (out_add: a new entry, or one copied back in this submission written again). Every other
// change takes entries out or makes them less live, so a "no overlap with a live result" answer stays true until this moves
// ([nvn] overlap_grow, out_overlaps_window). Changed under g_out_mu on the render thread.
std::atomic<uint64_t> g_out_grow{1};
using Spans = std::vector<std::pair<uint64_t, uint64_t>>;    // [lo, hi) page-granular, sorted, merged
struct OutIndex {
    uint64_t ver = 0;
    Spans all, live, shadow, pending;     // every entry; not yet copied back (!flushed || pending); the same of shadows (no copy); pending results in a copy
};
OutIndex g_idx;                          // render thread only
std::atomic<uint64_t> g_idx_rebuilds{0}, g_idx_rebuild_ns{0};
uint64_t g_idx_q = 0;                    // queries answered by the index (render thread; read racily by the 10 s line)
bool spans_any(const Spans& v, uint64_t lo, uint64_t hi) {
    auto it = std::upper_bound(v.begin(), v.end(), lo, [](uint64_t x, const std::pair<uint64_t, uint64_t>& s) { return x < s.second; });   // first span ending after lo
    return it != v.end() && it->first < hi;
}
bool out_index_on() {
    static const bool on = config_get_bool("nvn", "out_index", true);
    static const int64_t ab = config_get_int("nvn", "out_index_ab", 0);
    if (!on) return false;
    if (ab <= 0) return true;
    static int last = -1;
    const bool w = ((counters().presents.load(std::memory_order_relaxed) / static_cast<uint64_t>(ab)) & 1) != 0;
    if (static_cast<int>(w) != last) { last = w ? 1 : 0; ft::mark(w ? "outidx.on" : "outidx.off", static_cast<uint32_t>(counters().presents.load())); }
    return w;
}

// statistics (bytes and counts; the 10 s line)
std::atomic<uint64_t> g_ob_end{0}, g_ob_gpu{0}, g_ob_fault{0}, g_ob_inval{0};
std::atomic<uint64_t> g_why_bytes[kOutWhyCount], g_why_n[kOutWhyCount];   // GPU-reader copy-backs by reason
std::atomic<uint64_t> g_on_split{0};   // entries split at the end of their submission (out_split)
struct EagerDiag { uint64_t addr = 0, size = 0, bytes = 0, n = 0; bool cpu_read = false; uint64_t pages = 0, read_pages = 0, unarmed_pages = 0, was_guarded = 0; };
std::unordered_map<uint64_t, EagerDiag> g_eager_diag;   // g_out_mu held
struct FlushDiag { uint64_t addr = 0, size = 0, bytes = 0, n[kOutWhyCount] = {}; };
std::unordered_map<uint64_t, FlushDiag> g_flush_diag;   // GPU-reader copy-backs by result (g_out_mu held)
std::atomic<uint64_t> g_on_inval_skipped{0};   // invalidates left to the guards (out_invalidate_by_guard)
std::atomic<uint64_t> g_on_fault{0}, g_on_inval{0}, g_on_deferred{0}, g_on_eager_read{0}, g_on_eager_noguard{0}, g_on_gpu_flush{0};

// Bits [first, last] of a page bitmap, a 64-bit word at a time (one atomic per word, not per page: held results span thousands of pages).
inline uint64_t word_mask(uint64_t w, uint64_t first, uint64_t last) {
    const uint64_t lo = w == (first >> 6) ? (first & 63) : 0, hi = w == (last >> 6) ? (last & 63) : 63;
    return (hi == 63 ? ~uint64_t{0} : ((uint64_t{1} << (hi + 1)) - 1)) & (~uint64_t{0} << lo);
}
void bits_set(std::atomic<uint64_t>* bm, uint64_t first, uint64_t last, bool on) {
    if (last < first) return;
    for (uint64_t w = first >> 6; w <= (last >> 6); ++w) {
        const uint64_t m = word_mask(w, first, last);
        if (on) bm[w].fetch_or(m, std::memory_order_acq_rel);
        else bm[w].fetch_and(~m, std::memory_order_acq_rel);
    }
}
bool bits_any(const std::atomic<uint64_t>* bm, uint64_t first, uint64_t last) {
    if (last < first) return false;
    for (uint64_t w = first >> 6; w <= (last >> 6); ++w)
        if (bm[w].load(std::memory_order_acquire) & word_mask(w, first, last)) return true;
    return false;
}

// ---- CPU fetches of held results: who, how, how long (the 10 s line; [nvn] out_fetch_trace = N logs the first N fetches one by one) ----
// Source of a fetch: a guard fault (read or write: the game's own code, host_pc in lifted code), an OS call on guarded pages, an invalidate
// of the game (nvnMemoryPool/BufferInvalidateMappedRange: lr of the guest thread is the call site), or the render thread (cpu_view).
enum FetchSrc { kFsRead = 0, kFsWrite, kFsOsRead, kFsOsWrite, kFsInvalidate, kFsRender, kFsCount };
const char* const kFetchSrcName[kFsCount] = {"read fault", "write fault", "OS read", "OS write", "invalidate", "render thread"};
struct FetchKey {
    int src = 0;
    uint64_t pc = 0, lr = 0, entry = 0;     // host pc of the access, guest lr, the largest held result it touched
    bool operator<(const FetchKey& o) const { return std::tie(src, pc, lr, entry) < std::tie(o.src, o.pc, o.lr, o.entry); }
};
struct FetchStat { uint64_t n = 0, new_pages = 0, waits = 0, wait_ns = 0, max_wait_ns = 0, bytes = 0, entry_size = 0; };
std::mutex g_fd_mu;
std::map<FetchKey, FetchStat> g_fd;
std::atomic<int64_t> g_fd_lines{-1};

int64_t fetch_trace_lines() {
    int64_t v = g_fd_lines.load(std::memory_order_relaxed);
    if (v < 0) {
        v = std::max<int64_t>(0, config_get_int("nvn", "out_fetch_trace", 0));
        g_fd_lines.store(v, std::memory_order_relaxed);
    }
    return v;
}

void fetch_note(int src, uint64_t addr, uint64_t size, uint64_t host_pc, uint64_t entry, uint64_t entry_size, uint64_t new_pages, uint64_t bytes,
                uint64_t wait_ns, bool waited, uint64_t need, uint64_t done_before) {
    FetchKey k;
    k.src = src;
    k.pc = host_pc;
    if (::Ctx* c = diag_thread_ctx()) k.lr = c->x[30];
    k.entry = entry;
    {
        std::lock_guard<std::mutex> lk(g_fd_mu);
        FetchStat& s = g_fd[k];
        s.n++;
        s.new_pages += new_pages;
        s.bytes += bytes;
        s.entry_size = entry_size;
        if (waited) { s.waits++; s.wait_ns += wait_ns; s.max_wait_ns = std::max(s.max_wait_ns, wait_ns); }
    }
    // the first N fetches, every fetch that marked pages CPU-read for the first time (rare: the mark is sticky), and slow ones
    static std::atomic<int64_t> traced{0}, marked{0}, slow{0};
    const bool tr = traced.load(std::memory_order_relaxed) < fetch_trace_lines() && traced.fetch_add(1) < fetch_trace_lines();
    const bool mk = new_pages && marked.load(std::memory_order_relaxed) < 200 && marked.fetch_add(1) < 200;
    const bool sl = wait_ns > 2000000 && slow.load(std::memory_order_relaxed) < 100 && slow.fetch_add(1) < 100;
    if (!tr && !mk && !sl)
        return;
    NXLOG_I(kTag, "compute out fetch: %s %llx +%llx tid %lu pc %llx (guest f_%llx) lr %llx: result %llx +%llx (page +%llx), %llu pages newly CPU-read, %llu KiB copied, "
            "waited %.2f ms for submission %llu (done %llu)%s",
            kFetchSrcName[src], (unsigned long long)addr, (unsigned long long)size, (unsigned long)GetCurrentThreadId(), (unsigned long long)host_pc,
            (unsigned long long)(host_pc ? guest_function_of(host_pc) : 0),
            (unsigned long long)k.lr, (unsigned long long)entry, (unsigned long long)entry_size,
            (unsigned long long)(entry && addr >= entry ? (addr - entry) & ~(kPageBytes - 1) : 0), (unsigned long long)new_pages,
            (unsigned long long)(bytes >> 10), wait_ns / 1e6, (unsigned long long)need, (unsigned long long)done_before, sl ? " (slow)" : "");
}

void out_register(Mirror* m) {
    OutLock ol;
    g_out_mirrors.push_back(m);
    static std::once_flag once;
    std::call_once(once, [] {
        mem::ww::set_guard_handler([](uint64_t addr, int access, uint64_t host_pc) { out_fetch(addr, kPageBytes, true, access, host_pc); });
    });
}

bool new_servable(const Mirror& m, uint64_t first, uint64_t last) {   // every page valid, newer in the copy and armed (the CPU has not written it
    return mem::ww::all_valid_or_armed(m.ww, first, last - first + 1, m.gpu_new.get(), m.any_fresh ? m.fresh.get() : nullptr);   // since), or fresh
}

bool overlaps(const OutEntry& e, uint64_t lo, uint64_t hi) {      // page granular: [lo, hi) of addresses
    const uint64_t a0 = e.addr & ~(kPageBytes - 1), a1 = (e.addr + e.size + kPageBytes - 1) & ~(kPageBytes - 1);
    return a0 < hi && lo < a1;
}

bool live(const OutEntry& e) { return !e.flushed || e.pending; }   // not copied back in this submission, or written again since

// The render thread's index, current with g_out_ver (rebuilt under the lock when it moved). Render thread only.
const OutIndex& index_fresh() {
    const uint64_t v = g_out_ver.load(std::memory_order_acquire);
    if (g_idx.ver == v) return g_idx;
    const auto t0 = std::chrono::steady_clock::now();
    OutLock ol;
    g_idx.ver = g_out_ver.load(std::memory_order_acquire);   // under the lock: every change before it is in what follows
    // one sort for all four: (lo, hi, kind bits) by lo, then each set merged in order
    struct Iv { uint64_t lo, hi; uint32_t k; };
    static std::vector<Iv> iv;
    iv.clear();
    for (const OutEntry& e : g_out) {
        const uint64_t a0 = e.addr & ~(kPageBytes - 1), a1 = (e.addr + e.size + kPageBytes - 1) & ~(kPageBytes - 1);
        iv.push_back({a0, a1, 1u | (live(e) ? 2u : 0u) | (live(e) && !e.m ? 4u : 0u)});
        if (e.m && e.pending) {
            const uint64_t base = reinterpret_cast<uint64_t>(e.m->pg->base);
            iv.push_back({base + e.first * kPageBytes, base + (e.last + 1) * kPageBytes, 8u});
        }
    }
    ol.lk.unlock();
    std::sort(iv.begin(), iv.end(), [](const Iv& a, const Iv& b) { return a.lo < b.lo; });
    Spans* sets[4] = {&g_idx.all, &g_idx.live, &g_idx.shadow, &g_idx.pending};
    for (Spans* s : sets) s->clear();
    for (const Iv& x : iv)
        for (int k = 0; k < 4; ++k) {
            if (!(x.k & (1u << k))) continue;
            Spans& s = *sets[k];
            if (!s.empty() && x.lo <= s.back().second) s.back().second = std::max(s.back().second, x.hi);
            else s.emplace_back(x.lo, x.hi);
        }
    g_idx_rebuilds.fetch_add(1, std::memory_order_relaxed);
    g_idx_rebuild_ns.fetch_add(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count()),
                               std::memory_order_relaxed);
    return g_idx;
}
bool use_index() { return on_render_thread() && out_index_on(); }

// [nvn] out_index_inc (default on): a result added on the render thread (out_add) goes into the index in place when the index was current,
// instead of making the next query rebuild it from every entry (each dispatch adds results, and the next dispatch's storage checks rebuilt
// the index: ~100 rebuilds per frame in village outside).
bool index_inc_on() {
    static const bool on = config_get_bool("nvn", "out_index_inc", true);
    return on && ab_index_inc_on();
}
void add_span(Spans& v, uint64_t lo, uint64_t hi) {   // merged like the rebuild: touching spans become one
    auto it = std::lower_bound(v.begin(), v.end(), lo, [](const std::pair<uint64_t, uint64_t>& s, uint64_t x) { return s.second < x; });
    if (it == v.end() || it->first > hi) { v.insert(it, {lo, hi}); return; }
    it->first = std::min(it->first, lo);
    it->second = std::max(it->second, hi);
    auto j = it + 1;
    while (j != v.end() && j->first <= it->second) { it->second = std::max(it->second, j->second); ++j; }
    v.erase(it + 1, j);
}
std::atomic<uint64_t> g_idx_inc{0};   // statistics: index updates in place
// After out_add changed e (lock held; ver_before: g_out_ver before its out_changed). was_*: e's state before (a new entry: all false).
void index_note_add(uint64_t ver_before, const OutEntry& e, bool is_new, bool was_live, bool was_pending) {
    if (!index_inc_on() || !on_render_thread() || !out_index_on() || g_idx.ver != ver_before) return;
    if (g_out_ver.load(std::memory_order_acquire) != ver_before + 1) return;   // another change in between: rebuild as before
    const uint64_t a0 = e.addr & ~(kPageBytes - 1), a1 = (e.addr + e.size + kPageBytes - 1) & ~(kPageBytes - 1);
    if (is_new) add_span(g_idx.all, a0, a1);
    if (live(e) && !was_live) {
        add_span(g_idx.live, a0, a1);
        if (!e.m) add_span(g_idx.shadow, a0, a1);
    }
    if (e.m && e.pending && !was_pending) {
        const uint64_t base = reinterpret_cast<uint64_t>(e.m->pg->base);
        add_span(g_idx.pending, base + e.first * kPageBytes, base + (e.last + 1) * kPageBytes);
    }
    g_idx.ver = ver_before + 1;
    g_idx_inc.fetch_add(1, std::memory_order_relaxed);
}

// After entries of m covering [first, last] were removed or changed (lock held): pages no entry covers any more are not newer in the
// copy; pages no guarded entry covers lose their guard.
void settle(Mirror* m, uint64_t first, uint64_t last) {
    // the version moves before gpu_new bits go: a page no longer newer in the copy (and not valid) resolves to the pool from now on, and the
    // set memo (bind_sets, out_version in its epoch) on the render thread must see that before it can see the bit gone. out_fetch on a game
    // thread (a guard fault) settles while the render thread replays; its out_changed came only after settle and remove_entries.
    if (memo_exact_on()) out_changed();
    const uint64_t n = last - first + 1;
    std::vector<uint8_t> cov(static_cast<size_t>(n), 0);     // 1: some entry, 2: some guarded entry
    for (const OutEntry& e : g_out) {
        if (e.m != m || e.last < first || e.first > last) continue;
        const uint64_t a = std::max(e.first, first), b = std::min(e.last, last);
        for (uint64_t p = a; p <= b; ++p) cov[static_cast<size_t>(p - first)] |= static_cast<uint8_t>(1 | (e.guarded ? 2 : 0));
    }
    for (uint64_t p = first; p <= last;) {
        const uint8_t c = cov[static_cast<size_t>(p - first)];
        if (c & 2) { ++p; continue; }
        if (!(c & 1)) bits_set(m->gpu_new.get(), p, p, false);
        uint64_t q = p + 1;
        while (q <= last && !(cov[static_cast<size_t>(q - first)] & 2)) {
            if (!(cov[static_cast<size_t>(q - first)] & 1)) bits_set(m->gpu_new.get(), q, q, false);
            ++q;
        }
        mem::ww::unguard(m->ww, p, q - p);
        p = q;
    }
}

void recompute_bounds() {
    uint64_t lo = UINT64_MAX, hi = 0;
    for (const OutEntry& e : g_out) { lo = std::min(lo, e.addr); hi = std::max(hi, e.addr + e.size); }
    g_out_lo.store(lo, std::memory_order_relaxed);
    g_out_hi.store(hi, std::memory_order_relaxed);
    g_out_n.store(g_out.size(), std::memory_order_release);
}

void copy_barrier_in(VkCommandBuffer cb) { memory_barrier(cb); }   // the writes of the results (and anything earlier) before the copies

void copy_barrier_host(VkCommandBuffer cb) {                         // the copies before everything after, the host's reads included
    VkMemoryBarrier2 b = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_2_HOST_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_HOST_READ_BIT;
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.memoryBarrierCount = 1;
    di.pMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cb, &di);
}

// One copy of a held result back to the pool (collect_copies), recorded later (record_jobs), maybe with the lock free.
struct CopyJob { VkBuffer src, dst; VkBufferCopy c; };

// The copies record_copies would make for the entries at idx (lock held).
void collect_copies(const std::vector<size_t>& idx, std::vector<CopyJob>& out) {
    for (size_t i : idx) {
        const OutEntry& e = g_out[i];
        if (e.partial && e.m) {                 // only the pages still newer in the copy
            const uint64_t base = reinterpret_cast<uint64_t>(e.m->pg->base);
            auto newer = [&](uint64_t p) { return ((e.m->gpu_new[static_cast<size_t>(p >> 6)].load(std::memory_order_relaxed) >> (p & 63)) & 1) != 0; };
            for (uint64_t p = e.first; p <= e.last;) {
                if (!newer(p)) { ++p; continue; }
                uint64_t q = p + 1;
                while (q <= e.last && newer(q)) ++q;
                const uint64_t lo = std::max(e.addr, base + p * kPageBytes), hi = std::min(e.addr + e.size, base + q * kPageBytes);
                out.push_back({e.mbuf, e.hbuf, VkBufferCopy{e.moff + (lo - e.addr), e.hoff + (lo - e.addr), hi - lo}});
                p = q;
            }
            continue;
        }
        out.push_back({e.mbuf, e.hbuf, VkBufferCopy{e.moff, e.hoff, e.size}});
    }
}

uint64_t record_jobs(VkCommandBuffer cb, const std::vector<CopyJob>& jobs) {
    if (jobs.empty()) return 0;
    copy_barrier_in(cb);
    uint64_t bytes = 0;
    for (const CopyJob& j : jobs) {
        vkCmdCopyBuffer(cb, j.src, j.dst, 1, &j.c);
        bytes += j.c.size;
    }
    copy_barrier_host(cb);
    return bytes;
}

// ---- delta copy-backs ([nvn] out_delta, default on) -----------------------------------------------------------------------------------
// The copy-backs of held results sent ~1.9 GB/s over PCIe at 1920x1080 (first scene): the game's GPU culling buffers (2-15 MB each) go back
// whole every frame because the CPU reads and clears the counters in their first page, while the dispatches change a small part of them.
// Before a dispatch writes a range in the pool copy that holds no unsent result (the copy is what the pool holds there), the range is
// copied into a device-local snapshot (out_presnap, per 64 MiB chunk of the copy, at the same offset). The copy-back of that result then
// runs a compute shader (nvn_delta_shader.inc) that compares the copy with the snapshot 16 bytes at a time and writes the pool only where
// the GPU changed something: the reads stay in video memory, only changed bytes cross PCIe, and what the CPU wrote into the pool meanwhile
// (the counter resets) survives wherever the GPU did not write. A result without a snapshot (written first by a draw, retaken while an
// unsent result was in the copy, a shadow buffer) goes back whole as before.
#include "nvn_delta_shader.inc"
uint64_t g_snap_bytes = 0, g_snap_n = 0;   // snapshot chunks (Mirror::snap_chunks), g_out_mu held
struct DeltaPipe { bool tried = false, ok = false; VkDescriptorSetLayout dsl = VK_NULL_HANDLE; VkPipelineLayout layout = VK_NULL_HANDLE; VkPipeline pipe = VK_NULL_HANDLE; VkDeviceSize align = 256; };
DeltaPipe g_delta;
bool g_delta_blocked = false;                   // render thread: a dispatch's compute state is bound (the delta shader would replace it)
std::atomic<uint64_t> g_dl_n{0}, g_dl_scanned{0}, g_dl_plain{0}, g_dl_snaps{0}, g_dl_snap_bytes{0};   // statistics (the 10 s line)
struct PreSnap { uint64_t addr, size; };
std::vector<PreSnap> g_presnap;                 // snapshots recorded for the dispatch being set up (g_out_mu held), taken by out_add

bool delta_on() {
    static const bool on = config_get_bool("nvn", "out_delta", true);
    return on && ab_out_delta_on();
}
uint64_t delta_cap() {
    static const uint64_t b = static_cast<uint64_t>(std::max<int64_t>(0, config_get_int("nvn", "out_delta_mb", 512))) << 20;
    return b;
}

bool delta_init() {
    DeltaPipe& d = g_delta;
    if (d.tried) return d.ok;
    d.tried = true;
    if (!device().features().push_descriptor || !vkCmdPushDescriptorSetKHR) {
        NXLOG_I(kTag, "delta copy-backs: off (no VK_KHR_push_descriptor)");
        return false;
    }
    VkDevice dv = vkdev();
    VkPhysicalDeviceProperties props = {};
    vkGetPhysicalDeviceProperties(device().physical(), &props);
    d.align = std::max<VkDeviceSize>(16, props.limits.minStorageBufferOffsetAlignment);
    VkDescriptorSetLayoutBinding b[3] = {};
    for (uint32_t i = 0; i < 3; ++i) b[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo sli = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    sli.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    sli.bindingCount = 3;
    sli.pBindings = b;
    if (vkCreateDescriptorSetLayout(dv, &sli, nullptr, &d.dsl) != VK_SUCCESS) return false;
    VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &d.dsl;
    if (vkCreatePipelineLayout(dv, &pli, nullptr, &d.layout) != VK_SUCCESS) return false;
    VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = sizeof kDeltaComp;
    mi.pCode = kDeltaComp;
    VkShaderModule sm = VK_NULL_HANDLE;
    if (vkCreateShaderModule(dv, &mi, nullptr, &sm) != VK_SUCCESS) return false;
    VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, sm, "main", nullptr};
    ci.layout = d.layout;
    const VkResult r = vkCreateComputePipelines(dv, VK_NULL_HANDLE, 1, &ci, nullptr, &d.pipe);
    vkDestroyShaderModule(dv, sm, nullptr);
    if (r != VK_SUCCESS) {
        NXLOG_W(kTag, "delta copy-backs: off (the compute pipeline failed: %d)", static_cast<int>(r));
        return false;
    }
    d.ok = true;
    NXLOG_I(kTag, "delta copy-backs: on (snapshots up to %llu MiB)", static_cast<unsigned long long>(delta_cap() >> 20));
    return true;
}

// The snapshot chunk of the copy chunk that holds [addr, +size) at moff of mbuf (null: not eligible). Lock held.
VkBuffer snap_chunk_for(Mirror& m, uint64_t addr, uint64_t size, VkBuffer mbuf, VkDeviceSize moff) {
    if (!delta_init()) return VK_NULL_HANDLE;
    const DeltaPipe& d = g_delta;
    if (size < (64u << 10) || (size % 16) || (moff % d.align) || (addr % d.align)) return VK_NULL_HANDLE;
    const uint64_t base = reinterpret_cast<uint64_t>(m.pg->base);
    const size_t ci = static_cast<size_t>((addr - base) / kChunkBytes);
    if (ci >= m.chunks.size() || m.chunks[ci].buffer != mbuf) return VK_NULL_HANDLE;
    if (m.snap_chunks.size() < m.chunks.size()) m.snap_chunks.resize(m.chunks.size());
    gfx::Buffer& sb = m.snap_chunks[ci];
    if (!sb.buffer) {
        const uint64_t sz = m.chunks[ci].size;
        if (g_snap_bytes + sz > delta_cap()) return VK_NULL_HANDLE;
        if (device().create_buffer(sz, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                   gfx::MemoryUse::GpuOnly, &sb, "nvn delta snapshot") != VK_SUCCESS) {
            sb = gfx::Buffer{};
            return VK_NULL_HANDLE;
        }
        g_snap_bytes += sz;
        ++g_snap_n;
    }
    return sb.buffer;
}

// Lock held. Records the copy-back of e as a delta copy-back; false: not eligible (the caller copies it whole).
bool record_delta(VkCommandBuffer cb, const OutEntry& e, uint64_t* bytes, bool* dispatched) {
    if (!e.snapped || !e.m || g_delta_blocked || !delta_on()) return false;
    const VkBuffer sb = snap_chunk_for(*e.m, e.addr, e.size, e.mbuf, e.moff);
    if (!sb || (e.hoff % g_delta.align)) return false;
    Mirror& m = *e.m;
    const uint64_t base = reinterpret_cast<uint64_t>(m.pg->base);
    auto newer = [&](uint64_t p) { return ((m.gpu_new[static_cast<size_t>(p >> 6)].load(std::memory_order_relaxed) >> (p & 63)) & 1) != 0; };
    for (uint64_t p = e.first; p <= e.last;) {   // a split result: only the runs of pages still newer in the copy
        const bool take = !e.partial || newer(p);
        uint64_t q = p + 1;
        while (q <= e.last && (!e.partial || newer(q)) == take) ++q;
        if (take) {
            const uint64_t lo = std::max(e.addr, base + p * kPageBytes), hi = std::min(e.addr + e.size, base + q * kPageBytes);
            const uint64_t off = lo - e.addr, len = hi - lo;
            if (!*dispatched) vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_delta.pipe);
            *dispatched = true;
            const VkDescriptorBufferInfo bi[3] = {{e.mbuf, e.moff + off, len}, {e.hbuf, e.hoff + off, len}, {sb, e.moff + off, len}};
            VkWriteDescriptorSet w[3] = {};
            for (uint32_t k = 0; k < 3; ++k) {
                w[k] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                w[k].dstBinding = k;
                w[k].descriptorCount = 1;
                w[k].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                w[k].pBufferInfo = &bi[k];
            }
            vkCmdPushDescriptorSetKHR(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_delta.layout, 0, 3, w);
            vkCmdDispatch(cb, static_cast<uint32_t>((len / 16 + 255) / 256), 1, 1);
            g_dl_scanned.fetch_add(len, std::memory_order_relaxed);
        }
        p = q;
    }
    g_dl_n.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void snaps_forget(Mirror* m) {   // lock held
    for (gfx::Buffer& b : m->snap_chunks) {
        if (!b.buffer) continue;
        gfx::Buffer old = b;
        g_snap_bytes -= std::min<uint64_t>(g_snap_bytes, b.size);
        --g_snap_n;
        defer_destroy([old]() mutable { device().destroy_buffer(old); });
        b = gfx::Buffer{};
    }
}

bool presnap_take(uint64_t addr, uint64_t size) {   // lock held
    for (size_t i = 0; i < g_presnap.size(); ++i)
        if (g_presnap[i].addr == addr && g_presnap[i].size == size) {
            g_presnap.erase(g_presnap.begin() + static_cast<std::ptrdiff_t>(i));
            return true;
        }
    return false;
}

void delta_log(double secs) {
    static uint64_t p[5] = {};
    const uint64_t v[5] = {g_dl_n.load(), g_dl_scanned.load(), g_dl_plain.load(), g_dl_snaps.load(), g_dl_snap_bytes.load()};
    if (v[0] == p[0] && v[2] == p[2] && v[3] == p[3]) return;
    NXLOG_I(kTag, "delta copy-backs over %.1f s: %llu results, %llu MiB compared on the GPU; %llu MiB copied back whole (no snapshot); %llu snapshots "
                  "before dispatches (%llu MiB); snapshot chunks %llu, %llu MiB",
            secs, (unsigned long long)(v[0] - p[0]), (unsigned long long)((v[1] - p[1]) >> 20), (unsigned long long)((v[2] - p[2]) >> 20),
            (unsigned long long)(v[3] - p[3]), (unsigned long long)((v[4] - p[4]) >> 20), (unsigned long long)g_snap_n, (unsigned long long)(g_snap_bytes >> 20));
    std::copy(v, v + 5, p);
}

uint64_t record_copies(VkCommandBuffer cb, const std::vector<size_t>& idx) {
    if (idx.empty()) return 0;
    copy_barrier_in(cb);
    uint64_t bytes = 0;
    bool dispatched = false;
    for (size_t i : idx) {
        const OutEntry& e = g_out[i];
        if (e.m) back_put(*e.m, e.first, e.last, true);   // the pool holds what the copy holds there once the copy-back has run
        if (record_delta(cb, e, &bytes, &dispatched)) continue;
        if (e.m) g_dl_plain.fetch_add(e.size, std::memory_order_relaxed);
        if (e.partial && e.m) {                 // only the pages still newer in the copy
            const uint64_t base = reinterpret_cast<uint64_t>(e.m->pg->base);
            auto newer = [&](uint64_t p) { return ((e.m->gpu_new[static_cast<size_t>(p >> 6)].load(std::memory_order_relaxed) >> (p & 63)) & 1) != 0; };
            for (uint64_t p = e.first; p <= e.last;) {
                if (!newer(p)) { ++p; continue; }
                uint64_t q = p + 1;
                while (q <= e.last && newer(q)) ++q;
                const uint64_t lo = std::max(e.addr, base + p * kPageBytes), hi = std::min(e.addr + e.size, base + q * kPageBytes);
                VkBufferCopy c = {e.moff + (lo - e.addr), e.hoff + (lo - e.addr), hi - lo};
                vkCmdCopyBuffer(cb, e.mbuf, e.hbuf, 1, &c);
                bytes += hi - lo;
                p = q;
            }
            continue;
        }
        VkBufferCopy c = {e.moff, e.hoff, e.size};
        vkCmdCopyBuffer(cb, e.mbuf, e.hbuf, 1, &c);
        bytes += e.size;
    }
    if (dispatched) {   // the delta shader's writes too, and the next dispatch binds its own pipeline and sets anyway
        VkMemoryBarrier2 b = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        b.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_2_HOST_BIT;
        b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_HOST_READ_BIT;
        VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        di.memoryBarrierCount = 1;
        di.pMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cb, &di);
    } else {
        copy_barrier_host(cb);
    }
    return bytes;
}

// Removes the entries at idx (ascending or not) and settles their pages. Lock held.
void remove_entries(std::vector<size_t> idx) {
    if (idx.empty()) return;
    std::sort(idx.begin(), idx.end());
    struct Span { Mirror* m; uint64_t first, last; };
    std::vector<Span> spans;
    for (size_t k = idx.size(); k-- > 0;) {
        const OutEntry& e = g_out[idx[k]];
        spans.push_back({e.m, e.first, e.last});
        g_out.erase(g_out.begin() + static_cast<std::ptrdiff_t>(idx[k]));
    }
    for (const Span& s : spans)
        if (s.m) settle(s.m, s.first, s.last);
    recompute_bounds();
    out_changed();
}

uint64_t page_size_kb(uint64_t bytes) { return bytes >> 10; }

}  // namespace

void snaps_forget_locked(Mirror* m) {
    OutLock ol;
    snaps_forget(m);
}
void out_delta_block(bool on) { g_delta_blocked = on; }

void out_presnap_end() { OutLock ol; g_presnap.clear(); }

bool out_presnap(uint64_t addr, uint64_t size, VkBuffer mbuf, VkDeviceSize moff, VkCommandBuffer cb) {
    if (!size || !out_lazy() || !delta_on()) return false;
    const PoolHit hit = pool_lookup(addr);
    Mirror* m = hit.pool && hit.pool->gpu ? hit.pool->gpu->mirror : nullptr;
    if (!m) return false;
    OutLock ol;
    for (const OutEntry& e : g_out)   // an unsent result there: the copy is not what the pool holds (the entry keeps its snapshot, if any)
        if (live(e) && e.addr < addr + size && addr < e.addr + e.size) return false;
    const VkBuffer sb = snap_chunk_for(*m, addr, size, mbuf, moff);
    if (!sb) return false;
    memory_barrier(cb);                  // the fills of the copy before the snapshot reads it (the dispatch waits for the snapshot: g_sh_copied)
    const VkBufferCopy c = {moff, moff, size};
    vkCmdCopyBuffer(cb, mbuf, sb, 1, &c);
    if (g_presnap.size() > 64) g_presnap.erase(g_presnap.begin());
    g_presnap.push_back({addr, size});
    g_dl_snaps.fetch_add(1, std::memory_order_relaxed);
    g_dl_snap_bytes.fetch_add(size, std::memory_order_relaxed);
    return true;
}

bool out_lazy() {
    static const bool on = config_get_bool("nvn", "compute_out_lazy", true);
    return on;
}

bool out_any() { return g_out_n.load(std::memory_order_acquire) != 0; }
uint64_t out_version() { return g_out_ver.load(std::memory_order_acquire); }
uint64_t out_grow_version() { return g_out_grow.load(std::memory_order_acquire); }

uint64_t out_bytes_total() {
    return g_ob_end.load(std::memory_order_relaxed) + g_ob_gpu.load(std::memory_order_relaxed) + g_ob_fault.load(std::memory_order_relaxed) +
           g_ob_inval.load(std::memory_order_relaxed);
}

bool out_add(uint64_t addr, VkDeviceSize size, VkBuffer mbuf, VkDeviceSize moff, const BufRef& host, bool in_mirror) {
    if (!size || !host.ok) return false;
    Mirror* m = nullptr;
    if (in_mirror) {
        const PoolHit hit = pool_lookup(addr);
        m = hit.pool && hit.pool->gpu ? hit.pool->gpu->mirror : nullptr;
    }
    OutLock ol;
    for (OutEntry& e : g_out)
        if (e.addr == addr && e.size == size && e.m == m) {
            const uint64_t v0 = g_out_ver.load(std::memory_order_acquire);
            const bool was_live = live(e), was_pending = e.pending;
            const bool snap = presnap_take(addr, size);
            if (!live(e)) e.snapped = snap;         // sent back already: the copy was the pool again when the new snapshot was taken
            if (!e.pending) out_changed();          // live and pending again
            if (e.flushed && !e.pending) g_out_grow.fetch_add(1, std::memory_order_acq_rel);   // was not live: its pages are live again
            ++e.gen;                                // a newer result than one a fetch may be copying back
            e.pending = true;
            e.partial = false;
            e.mbuf = mbuf; e.moff = moff; e.hbuf = host.buffer; e.hoff = host.offset;
            if (m) bits_set(m->gpu_new.get(), e.first, e.last, true);    // a copy-back recorded earlier in this submission cleared them
            if (!was_pending) index_note_add(v0, e, false, was_live, was_pending);
            return true;
        }
    OutEntry e;
    e.addr = addr; e.size = size; e.m = m;
    e.mbuf = mbuf; e.moff = moff; e.hbuf = host.buffer; e.hoff = host.offset;
    e.pending = true;
    e.snapped = presnap_take(addr, size);
    e.id = g_out_next_id++;
    if (m) {
        const uint64_t base = reinterpret_cast<uint64_t>(m->pg->base);
        e.first = (addr - base) / kPageBytes;
        e.last = std::min<uint64_t>((addr + size - 1 - base) / kPageBytes, m->npages - 1);
        bits_set(m->gpu_new.get(), e.first, e.last, true);
    }
    const uint64_t v0 = g_out_ver.load(std::memory_order_acquire);
    g_out.push_back(e);
    out_changed();
    g_out_grow.fetch_add(1, std::memory_order_acq_rel);
    index_note_add(v0, e, true, false, false);
    g_out_lo.store(std::min(g_out_lo.load(std::memory_order_relaxed), addr), std::memory_order_relaxed);
    g_out_hi.store(std::max(g_out_hi.load(std::memory_order_relaxed), addr + size), std::memory_order_relaxed);
    g_out_n.store(g_out.size(), std::memory_order_release);
    return false;
}

bool out_exact(uint64_t addr, VkDeviceSize size, VkBuffer* buf, VkDeviceSize* off, bool* in_mirror) {
    if (!out_any()) return false;
    OutLock ol;
    for (const OutEntry& e : g_out)
        if (e.addr == addr && e.size == size && e.mbuf) {
            // a copy-back recorded in this submission (flushed) leaves the copy as current as the pool: still the right place
            *buf = e.mbuf;
            *off = e.moff;
            if (in_mirror) *in_mirror = e.m != nullptr;
            return true;
        }
    return false;
}

bool out_touches(uint64_t addr, uint64_t size) {
    if (!out_any() || !size) return false;
    const uint64_t lo = addr & ~(kPageBytes - 1), hi = size >= UINT64_MAX - addr ? UINT64_MAX : (addr + size + kPageBytes - 1) & ~(kPageBytes - 1);
    if (hi <= (g_out_lo.load(std::memory_order_relaxed) & ~(kPageBytes - 1)) || lo >= g_out_hi.load(std::memory_order_relaxed) + kPageBytes) return false;
    if (use_index()) { ++g_idx_q; return spans_any(index_fresh().all, lo, hi); }
    OutLock ol;
    for (const OutEntry& e : g_out)
        if (overlaps(e, lo, hi)) return true;
    return false;
}

bool out_overlaps_shadow(uint64_t addr, uint64_t size) {
    if (!out_any() || !size) return false;
    const uint64_t lo = addr & ~(kPageBytes - 1), hi = size >= UINT64_MAX - addr ? UINT64_MAX : (addr + size + kPageBytes - 1) & ~(kPageBytes - 1);
    if (hi <= (g_out_lo.load(std::memory_order_relaxed) & ~(kPageBytes - 1)) || lo >= g_out_hi.load(std::memory_order_relaxed) + kPageBytes) return false;
    if (use_index()) { ++g_idx_q; return spans_any(index_fresh().shadow, lo, hi); }
    OutLock ol;
    for (const OutEntry& e : g_out)
        if (!e.m && live(e) && overlaps(e, lo, hi)) return true;
    return false;
}

bool out_overlaps(uint64_t addr, uint64_t size) {
    if (!out_any() || !size) return false;
    const uint64_t lo = addr & ~(kPageBytes - 1), hi = size >= UINT64_MAX - addr ? UINT64_MAX : (addr + size + kPageBytes - 1) & ~(kPageBytes - 1);
    if (hi <= (g_out_lo.load(std::memory_order_relaxed) & ~(kPageBytes - 1)) || lo >= g_out_hi.load(std::memory_order_relaxed) + kPageBytes) return false;
    if (use_index()) { ++g_idx_q; return spans_any(index_fresh().live, lo, hi); }
    OutLock ol;
    for (const OutEntry& e : g_out)
        if (live(e) && overlaps(e, lo, hi)) return true;
    return false;
}

// out_overlaps(page, size) for the pointer windows of gmem_before_op (render thread): the same pages are asked about in draw after draw and
// almost never overlap, so a "no overlap" answer is kept per (page, size, index version) in a small direct-mapped table.
// [nvn] overlap_grow (default on): the answers are kept per g_out_grow instead, which moves only when live results may cover more pages
// (g_out_ver moves on every copy-back, split and fetch too: ~140 times per frame in village outside, and every move emptied the table).
bool overlap_grow_on() {
    static const bool on = config_get_bool("nvn", "overlap_grow", true);
    return on && ab_overlap_grow_on();
}
uint64_t g_win_hits = 0, g_win_asks = 0;   // render thread
// [nvn] overlap_pos (default on; cpuperf_ab_what overlap_pos): out_overlaps_window keeps overlap answers too (per g_out_ver)
std::atomic<bool> g_overlap_pos_ab{true};
void overlap_pos_enable(bool on) { g_overlap_pos_ab.store(on, std::memory_order_relaxed); }
bool overlap_pos_on() {
    static const bool on = config_get_bool("nvn", "overlap_pos", true);
    return on && g_overlap_pos_ab.load(std::memory_order_relaxed);
}
bool out_overlaps_window(uint64_t page, uint64_t size) {
    if (!out_any() || !size) return false;
    if (!use_index()) return out_overlaps(page, size);
    // pos: an overlap answer, kept per g_out_ver (out_overlaps is a function of the index, which moves with g_out_ver): the draw bindings over
    // held results are asked about at every draw (~4,000 times per frame in village outside)
    struct Slot { uint64_t page = ~0ull, size = 0, ver = 0; bool pos = false; };
    static Slot cache[4096];
    const uint64_t ov = g_out_ver.load(std::memory_order_acquire);
    const uint64_t v = overlap_grow_on() ? (g_out_grow.load(std::memory_order_acquire) | (1ull << 63)) : ov;
    Slot& s = cache[((page >> 12) ^ (page >> 24)) & 4095];
    ++g_win_asks;
    if (s.page == page && s.size == size) {
        if (!s.pos && s.ver == v) { ++g_win_hits; return false; }
        if (s.pos && s.ver == ov && overlap_pos_on()) { ++g_win_hits; return true; }
    }
    const bool r = out_overlaps(page, size);
    s.page = page; s.size = size; s.ver = r ? ov : v; s.pos = r;
    return r;
}

// [nvn] servable_cache (default on; verify: both, mismatches counted): out_servable's true answers are remembered per (address, size) while
// nothing that clears a page's valid, armed, newer-in-the-copy or fresh bit has happened since (mem::ww::changes / owner_clears, the
// held-result version, the submission, the pool registry, mirror destruction). The draws of the village outside ask it ~4,000 times per
// frame for the same few storage ranges over held results, each a walk over the range's pages (a 9.9 MB binding: 2,400 pages).
uint64_t g_srv_submits = 0, g_srv_mirror_gone = 0;
void srv_note_submit() { ++g_srv_submits; }
void srv_note_mirror_gone() { ++g_srv_mirror_gone; }
uint64_t g_srv_hits = 0, g_srv_bad = 0, g_srv_raced = 0;
bool out_servable_uncached(uint64_t addr, uint64_t size);
int servable_cache_mode() {
    static const int m = [] { const std::string v = config_get_str("nvn", "servable_cache", "on"); return v == "verify" ? 2 : (v == "off" || v == "0" || v == "false") ? 0 : 1; }();
    return m;
}
void srv_log() {
    static uint32_t last = 0;
    const uint32_t now = presents_now();
    if (servable_cache_mode() && now - last >= 1200) { last = now; NXLOG_I(kTag, "servable_cache: %llu hits, %llu verify differences (%llu more raced with a page release)", (unsigned long long)g_srv_hits, (unsigned long long)g_srv_bad, (unsigned long long)g_srv_raced); }
}
std::atomic<bool> g_srv_ab{true};
void servable_cache_enable(bool on) { g_srv_ab.store(on, std::memory_order_relaxed); }
bool out_servable(uint64_t addr, uint64_t size) {
    if (!out_lazy() || !g_mirror_on || !size) return false;
    const int mode = servable_cache_mode();
    if (!mode || !g_srv_ab.load(std::memory_order_relaxed) || !on_render_thread()) return out_servable_uncached(addr, size);
    struct Srv { uint64_t addr = 0, size = 0, k = 0; };
    static Srv memo[1024];
    const uint64_t changes0 = mem::ww::changes();
    const uint64_t k = ((changes0 * 0x9e3779b97f4a7c15ull ^ mem::ww::owner_clears()) * 1099511628211ull ^ out_version() * 31 ^ g_srv_submits * 131) * 1099511628211ull ^
                       pool_registry_generation() * 0x51ed27ull ^ g_srv_mirror_gone * 0x2545f491ull;
    Srv& e = memo[((addr >> 4) ^ (addr >> 20) ^ size * 7) & 1023];
    if (e.addr == addr && e.size == size && e.k == k) {
        ++g_srv_hits;
        if (mode == 2 && !out_servable_uncached(addr, size)) {
            if (mem::ww::changes() != changes0) { ++g_srv_raced; return false; }   // a page release began on another thread meanwhile: not stale
            if (g_srv_bad++ < 16) NXLOG_W(kTag, "servable_cache verify: %llx +%llx was servable, is not now", static_cast<unsigned long long>(addr), static_cast<unsigned long long>(size));
            return false;
        }
        return true;
    }
    const bool r = out_servable_uncached(addr, size);
    if (r) e = Srv{addr, size, k};
    return r;
}
bool out_servable_uncached(uint64_t addr, uint64_t size) {
    MemoryPool* mp = nullptr;
    PoolGpu* pg = nullptr;
    uint64_t poff = 0;
    const BufRef r = resolve_in_pool(addr, size, &mp, &pg, &poff);
    if (!r.ok || !pg || !pg->mirror) return false;
    const Mirror& m = *pg->mirror;
    const uint64_t first = poff / kPageBytes, last = (poff + size - 1) / kPageBytes;
    const uint64_t chunk_pages = kChunkBytes / kPageBytes;
    if (last >= m.npages || last - first + 1 > kMaxBindPages || first / chunk_pages != last / chunk_pages) return false;
    if (!m.chunks[static_cast<size_t>(first / chunk_pages)].buffer) return false;
    return range_valid(m, first, last - first + 1) || new_servable(m, first, last);
}

// A result written by the submission being recorded (pending) covers page `page` of m.
bool out_pending_on_page(const Mirror* m, uint64_t page) {
    if (!out_any()) return false;
    if (use_index()) {
        const uint64_t a = reinterpret_cast<uint64_t>(m->pg->base) + page * kPageBytes;
        return spans_any(index_fresh().pending, a, a + kPageBytes);
    }
    OutLock ol;
    for (const OutEntry& e : g_out)
        if (e.m == m && e.pending && page >= e.first && page <= e.last) return true;
    return false;
}

uint64_t out_flush(VkCommandBuffer cb, uint64_t addr, uint64_t size, int why) {
    if (!out_any()) return 0;
    const uint64_t lo = size == UINT64_MAX ? 0 : addr & ~(kPageBytes - 1);
    const uint64_t hi = size == UINT64_MAX || size >= UINT64_MAX - addr ? UINT64_MAX : (addr + size + kPageBytes - 1) & ~(kPageBytes - 1);
    const uint64_t cur = current_serial();
    std::vector<std::pair<uint64_t, uint64_t>> rearm;     // results copied back in queue order: tracked again (mirror_span_written)
    uint64_t bytes = 0;
    {
        OutLock ol;
        std::vector<size_t> idx, gone;
        for (size_t i = 0; i < g_out.size(); ++i) {
            const OutEntry& e = g_out[i];
            if (e.flushed && !e.pending) continue;          // copied back already in this submission, nothing newer since
            if (overlaps(e, lo, hi)) idx.push_back(i);
        }
        if (idx.empty()) return 0;
        out_changed();
        bytes = record_copies(cb, idx);
        if (why != kOutSubmitEnd)
            for (size_t i : idx) {
                const OutEntry& e = g_out[i];
                FlushDiag& d = g_flush_diag[e.addr ^ (e.size << 40)];
                d.addr = e.addr; d.size = e.size; d.bytes += e.size;
                d.n[why >= 0 && why < kOutWhyCount ? why : kOutGpuReader]++;
            }
        for (size_t i : idx) {
            OutEntry& e = g_out[i];
            if (e.guarded || e.fetching) {                  // the CPU may still read old bytes until the copy has run: guarded until then;
                e.pending = false;                          // the copy stays as current as the pool for it (gpu_new kept until retired)
                e.flushed = cur;
            } else {
                if (e.m) rearm.emplace_back(e.addr, e.size);   // a shadow's result (no e.m) left the copy's pages invalid: they stay so
                gone.push_back(i);
            }
        }
        remove_entries(gone);
    }
    for (const auto& r : rearm) mirror_span_written(r.first, r.second);
    (why == kOutSubmitEnd ? g_ob_end : g_ob_gpu).fetch_add(bytes, std::memory_order_relaxed);
    if (why != kOutSubmitEnd) g_on_gpu_flush.fetch_add(1, std::memory_order_relaxed);
    const int w = why >= 0 && why < kOutWhyCount ? why : kOutGpuReader;
    g_why_bytes[w].fetch_add(bytes, std::memory_order_relaxed);
    g_why_n[w].fetch_add(1, std::memory_order_relaxed);
    return bytes;
}

// [nvn] compute_out_split (default on): a result whose range holds pages the CPU has read (cpu_read) or that cannot be guarded (hot pages,
// never tracked) used to go back whole at the end of every submission. the game's big compute buffers (up to 16 MB) have such pages, and ~50 MB
// per frame went back over PCIe. Now only those pages go back; the entry stays held and guarded for the others (OutEntry::partial).
bool out_split_on() {
    static const bool on = config_get_bool("nvn", "compute_out_split", true);
    return on;
}

// [nvn] compute_out_split_min_kb (default 1024): smaller results are not split; one with a CPU-read page goes back whole at the end of its
// submission. the game's job workers read small GPU results (16-192 KiB) a page at a time, a new page every second or so: split, each new page was
// a guard fault that waited for the submission still on the GPU (20-50 ms, the periodic hitches of the first scene). Whole, the copy-back
// costs microseconds and the CPU finds the pool current.
uint64_t out_split_min() {
    static const uint64_t b = static_cast<uint64_t>(std::max<int64_t>(0, config_get_int("nvn", "compute_out_split_min_kb", 1024))) << 10;
    return b;
}

// Lock held. Pending entry i (lazy, not guarded yet) whose pages are of both kinds: the pages that must go back now (the CPU read them, or they
// are not tracked) are copied (parts, their ranges into rearm) and are no longer newer in the copy; the runs of other pages are guarded and the
// entry is held for them (partial). 0: one kind only, the caller handles the entry whole; 1: held; 2: every page went back (remove it).
int out_split(size_t i, uint64_t serial, std::vector<VkBufferCopy>& parts, std::vector<std::pair<VkBuffer, VkBuffer>>& bufs,
              std::vector<std::pair<uint64_t, uint64_t>>& rearm) {
    OutEntry& e = g_out[i];
    Mirror* m = e.m;
    const uint64_t base = reinterpret_cast<uint64_t>(m->pg->base);
    auto back = [&](uint64_t p) { return ((m->cpu_read[static_cast<size_t>(p >> 6)].load(std::memory_order_relaxed) >> (p & 63)) & 1) || !mem::ww::all_armed(m->ww, p, 1); };
    bool any_back = false, any_hold = false;
    for (uint64_t p = e.first; p <= e.last && !(any_back && any_hold); ++p) (back(p) ? any_back : any_hold) = true;
    if (!any_back || !any_hold) return 0;
    bool held = false;
    for (uint64_t p = e.first; p <= e.last;) {
        const bool b = back(p);
        uint64_t q = p + 1;
        while (q <= e.last && back(q) == b) ++q;
        if (!b && mem::ww::guard(m->ww, p, q - p)) {
            held = true;
        } else {
            const uint64_t lo = std::max(e.addr, base + p * kPageBytes), hi = std::min(e.addr + e.size, base + q * kPageBytes);
            parts.push_back(VkBufferCopy{e.moff + (lo - e.addr), e.hoff + (lo - e.addr), hi - lo});
            bufs.emplace_back(e.mbuf, e.hbuf);
            rearm.emplace_back(lo, hi - lo);
            bits_set(m->gpu_new.get(), p, q - 1, false);   // current in the pool once the copy has run: a later copy-back leaves them alone
        }
        p = q;
    }
    g_on_split.fetch_add(1, std::memory_order_relaxed);
    if (!held) return 2;
    e.partial = true;
    e.guarded = true;
    e.pending = false;
    e.serial = serial;
    e.flushed = 0;
    return 1;
}

void out_end_submit(VkCommandBuffer cb, uint64_t serial) {
    if (!out_any()) return;
    std::vector<std::pair<uint64_t, uint64_t>> rearm;
    {
        OutLock ol;
        out_changed();                         // pending entries become held or go back
        std::vector<size_t> eager, gone;
        std::vector<VkBufferCopy> part_copies;          // split entries: the parts that go back now
        std::vector<std::pair<VkBuffer, VkBuffer>> part_bufs;
        const size_t n0 = g_out.size();
        for (size_t i = 0; i < n0; ++i) {
            OutEntry& e = g_out[i];
            if (!e.pending) continue;
            bool hold = out_lazy() && e.m && e.m->ww;
            // A result still held from an earlier submission is not split again (it goes back whole when the CPU has read some of it). Splitting
            // it too (2026-11-08: the game's eight GPU culling buffers, the CPU reads and clears only their first page) cut the end-of-submission
            // copies from ~850 to ~150 MiB/s but took the first scene from ~31 to ~22 fps.
            if (hold && !e.guarded && e.last > e.first && out_split_on() && e.size >= out_split_min()) {
                const int sp = out_split(i, serial, part_copies, part_bufs, rearm);
                if (sp == 1) { g_on_deferred.fetch_add(1, std::memory_order_relaxed); continue; }   // held for its guarded pages
                if (sp == 2) { gone.push_back(i); continue; }                                       // every page went back (part_copies)
            }
            if (hold && bits_any(e.m->cpu_read.get(), e.first, e.last)) { hold = false; g_on_eager_read.fetch_add(1, std::memory_order_relaxed); }
            else if (hold && !e.guarded && !mem::ww::guard(e.m->ww, e.first, e.last - e.first + 1)) { hold = false; g_on_eager_noguard.fetch_add(1, std::memory_order_relaxed); }
            if (hold) {
                e.guarded = true;
                e.pending = false;
                e.serial = serial;
                e.flushed = 0;
                g_on_deferred.fetch_add(1, std::memory_order_relaxed);
            } else {
                eager.push_back(i);
            }
        }
        for (size_t i : eager) {               // diagnosis (the 10 s line): which results go back at the end of their submission
            const OutEntry& e = g_out[i];
            auto& d = g_eager_diag[e.addr ^ (e.size << 40)];
            d.addr = e.addr; d.size = e.size; d.bytes += e.size; d.n++;
            d.cpu_read = e.m && bits_any(e.m->cpu_read.get(), e.first, e.last);
            if (e.m) {   // why it went back whole: pages the CPU read, pages not armed (cannot be guarded), held from before
                d.pages = e.last - e.first + 1;
                uint64_t rd = 0, ua = 0;
                for (uint64_t p = e.first; p <= e.last; ++p) {
                    rd += (e.m->cpu_read[static_cast<size_t>(p >> 6)].load(std::memory_order_relaxed) >> (p & 63)) & 1;
                    ua += mem::ww::all_armed(e.m->ww, p, 1) ? 0 : 1;
                }
                d.read_pages = rd;
                d.unarmed_pages = ua;
                d.was_guarded += e.guarded ? 1 : 0;
            }
        }
        uint64_t bytes = record_copies(cb, eager);
        if (!part_copies.empty()) {
            copy_barrier_in(cb);
            for (size_t k = 0; k < part_copies.size(); ++k) {
                vkCmdCopyBuffer(cb, part_bufs[k].first, part_bufs[k].second, 1, &part_copies[k]);
                bytes += part_copies[k].size;
            }
            copy_barrier_host(cb);
        }
        g_ob_end.fetch_add(bytes, std::memory_order_relaxed);
        for (size_t i : eager) {
            OutEntry& e = g_out[i];
            e.pending = false;
            if (e.guarded || e.fetching) { e.flushed = serial; e.serial = serial; }   // guarded from before: the guard goes when this submission is done
            else { if (e.m) rearm.emplace_back(e.addr, e.size); gone.push_back(i); }
        }
        remove_entries(gone);
    }
    for (const auto& r : rearm) mirror_span_written(r.first, r.second);
}

// Paired measurement windows of [nvn] out_index_ab / out_fetch_ab (N presents each, the feature off in even windows): one line per window with
// fps, 1% low, the median frame and the render thread's replay / idle time per frame, so a single run gives back-to-back pairs of one scene.
void out_ab_tick() {
    static const int64_t ab_i = config_get_int("nvn", "out_index_ab", 0), ab_f = config_get_int("nvn", "out_fetch_ab", 0), ab_c = config_get_int("nvn", "cpuperf_ab", 0);
    const int64_t ab = ab_i > 0 ? ab_i : ab_f > 0 ? ab_f : ab_c;   // cpuperf_ab (nvn_replay.cpp): the same windows (on when presents / N is odd)
    if (ab <= 0) return;
    const uint64_t pres = counters().presents.load(std::memory_order_relaxed);
    const uint64_t w = pres / static_cast<uint64_t>(ab);
    static uint64_t cur_w = ~0ull, p0 = 0;
    static std::chrono::steady_clock::time_point t0;
    static double r0 = 0, i0 = 0, s0 = 0;
    static uint64_t lw0 = 0, bar0 = 0, ge0 = 0, gb0 = 0;
    if (w == cur_w) return;
    uint64_t ge = 0, gb = 0;
    gpu_time_totals(&ge, &gb);
    const uint64_t bar = barriers_mem_total();
    const auto now = std::chrono::steady_clock::now();
    const double r = nxrt::hitch_phase_seconds(nxrt::kHpRenderReplay), i = nxrt::hitch_phase_seconds(nxrt::kHpRenderIdle),
                 s = nxrt::hitch_phase_seconds(nxrt::kHpGameSyncWait);
    const uint64_t lw = g_lock_wait_ns.load(std::memory_order_relaxed);
    if (cur_w != ~0ull && pres > p0) {
        const double secs = std::chrono::duration<double>(now - t0).count();
        const double fr = static_cast<double>(pres - p0);
        const nxrt::FrameTimeStats st = nxrt::frame_time_stats(secs);
        NXLOG_I(kTag, "ab window %s%s%s (presents %llu-%llu, %.1f s): %.1f fps, 1%% low %.1f, median %.2f ms, worst %.1f ms; per frame: render thread replay %.2f ms, "
                      "idle %.2f ms, game GPU sync waits %.2f ms, waits for the compute-out lock %.3f ms, GPU exec %.2f ms, GPU busy %.2f ms, full barriers %.1f",
                (cur_w & 1) ? "on" : "off", ab_i > 0 ? " [out_index]" : "",
                ab_f > 0 ? " [out_fetch_unlocked]" : (ab_i <= 0 && ab_c > 0) ? " [cpuperf]" : "",
                (unsigned long long)p0, (unsigned long long)pres, secs, fr / secs, st.low1_fps, st.median_ms, st.worst_ms, (r - r0) * 1000.0 / fr,
                (i - i0) * 1000.0 / fr, (s - s0) * 1000.0 / fr, (lw - lw0) / 1e6 / fr, (ge - ge0) / 1e6 / fr, (gb - gb0) / 1e6 / fr, (bar - bar0) / fr);
    }
    cur_w = w; p0 = pres; t0 = now; r0 = r; i0 = i; s0 = s; lw0 = lw; bar0 = bar; ge0 = ge; gb0 = gb;
}

void out_retire() {
    out_ab_tick();
    // [nvn] out_cpu_read_reset_s = N (diagnostic, default 0: off): every N s the CPU-read marks are cleared, so the next CPU touch of each held
    // result faults again and shows up in the fetch lines (who reads, read or write, how long it waits). Costs fps while on.
    static const int64_t reset_s = config_get_int("nvn", "out_cpu_read_reset_s", 0);
    if (reset_s > 0) {
        static auto last = std::chrono::steady_clock::now();
        const auto now = std::chrono::steady_clock::now();
        if (now - last >= std::chrono::seconds(reset_s)) {
            last = now;
            OutLock ol;
            for (Mirror* m : g_out_mirrors)
                for (size_t i = 0; i < static_cast<size_t>((m->npages + 63) / 64); ++i) m->cpu_read[i].store(0, std::memory_order_relaxed);
            NXLOG_I(kTag, "compute out: CPU-read marks cleared (out_cpu_read_reset_s)");
        }
    }
    if (!out_any()) return;
    const uint64_t done = poll_completed();
    std::vector<std::pair<uint64_t, uint64_t>> rearm;
    {
        OutLock ol;
        std::vector<size_t> gone;
        for (size_t i = 0; i < g_out.size(); ++i) {
            OutEntry& e = g_out[i];
            if (!e.flushed || e.pending || e.flushed > done || e.fetching) continue;   // fetching: out_fetch retires it
            e.guarded = false;
            rearm.emplace_back(e.addr, e.size);
            gone.push_back(i);
        }
        remove_entries(gone);
    }
    for (const auto& r : rearm) mirror_span_written(r.first, r.second);
}

// [nvn] compute_out_invalidate (default "guard"): what nvnMemoryPool/BufferInvalidateMappedRange does with held results. "fetch" (before
// 2026-11-08): copies them back at once and marks the whole range CPU-read, so later results there go back at the end of every submission.
// the game invalidates four 4.4 MB GPU culling buffers whole every other frame (generic buffer code, lr main+0x3433cc) and reads little of them:
// that alone sent ~350 MB/s back over PCIe. "guard": nothing; the guard faults of the pages the CPU touches do the work.
bool out_invalidate_by_guard() {
    static const bool on = config_get_str("nvn", "compute_out_invalidate", "guard") != "fetch";
    return on;
}

void out_fetch(uint64_t addr, uint64_t size, bool by_fault, int access, uint64_t host_pc) {
    if (!size || (!by_fault && !out_lazy())) return;
    if (by_fault && archexp::coherence_on()) {   // E2a (measurement only)
        const PoolHit ph = pool_lookup(addr);
        archexp::note_guard_fault(addr, access, ph.pool ? ph.pool->flags : 0);
    }
    nxrt::HitchTimer ht_fetch(nxrt::kHpOutFetch);   // the hitch line / [diag] frame_csv: time in fetches (by guard faults separately)
    struct FaultTimer {
        bool on;
        uint64_t t0;
        ~FaultTimer() { if (on) nxrt::hitch_phase_add(nxrt::kHpOutFetchFault, nxrt::hitch_ticks() - t0); }
    } ht_fault{by_fault, nxrt::hitch_ticks()};
    const uint64_t lo = addr & ~(kPageBytes - 1), hi = (addr + size + kPageBytes - 1) & ~(kPageBytes - 1);
    const int src = access >= 0 && access < 4 ? access : (on_render_thread() ? kFsRender : kFsInvalidate);
    if (src == kFsInvalidate && out_invalidate_by_guard()) {
        // The guards already make every CPU access of a held result current (and a result whose copy-back is recorded stays guarded until
        // it has run), so an invalidate needs no fetch: the pages the CPU really touches fault and are marked, the rest stays held.
        g_on_inval_skipped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    OutLock ol;
    std::unique_lock<std::mutex>& lk = ol.lk;
    // later results in these pages go back at the end of their submission
    uint64_t new_pages = 0;
    for (Mirror* m : g_out_mirrors) {
        const uint64_t base = reinterpret_cast<uint64_t>(m->pg->base), end = base + m->npages * kPageBytes;
        if (hi <= base || lo >= end) continue;
        const uint64_t f = (std::max(lo, base) - base) / kPageBytes, l = (std::min(hi, end) - 1 - base) / kPageBytes;
        for (uint64_t p = f; p <= l; ++p) {
            const uint64_t b = uint64_t{1} << (p & 63);
            if (!(m->cpu_read[p >> 6].fetch_or(b, std::memory_order_acq_rel) & b)) ++new_pages;
        }
    }
    // diagnostics: the largest held result the fetch touches, the time spent waiting for the GPU
    uint64_t d_entry = 0, d_entry_size = 0, d_wait_ns = 0, d_need = 0, d_done = 0;
    bool d_waited = false;
    for (const OutEntry& e : g_out)
        if (overlaps(e, lo, hi) && e.size > d_entry_size) { d_entry = e.addr; d_entry_size = e.size; }
    uint64_t d_bytes = 0;
    auto finish = [&] {          // lock released
        if (d_entry || new_pages) fetch_note(src, addr, size, host_pc, d_entry, d_entry_size, new_pages, d_bytes, d_wait_ns, d_waited, d_need, d_done);
    };
    for (int waits = 0;; ) {
        // rebuilt after every wait: entries may have changed or gone while the lock was free
        std::vector<size_t> idx, copy;
        uint64_t need = 0;
        bool busy = false;
        const bool rt = on_render_thread();
        const uint64_t cur = rt ? current_serial() : 0;
        for (size_t i = 0; i < g_out.size() && !busy; ++i) {
            const OutEntry& e = g_out[i];
            if (!overlaps(e, lo, hi) || !(e.guarded || e.serial)) continue;   // only in the submission being recorded: it goes back at its end
            if (e.fetching) { busy = true; break; }
            idx.push_back(i);
            // a copy-back is recorded already (flushed): once that submission is done the pool is current, and later pool writes there
            // (a clear, a copy, an update) must not be overwritten with the held result. Only when this thread is the one recording it
            // (the render thread) can it not wait: then the result is copied now, before those later writes run.
            if (e.flushed && !e.pending && !(rt && e.flushed >= cur)) need = std::max(need, e.flushed);
            else { copy.push_back(i); need = std::max(need, e.serial); }
        }
        if (busy) {                     // another thread is copying one of them back with the lock free: when it is done, look again
            g_fetch_busy_waits.fetch_add(1, std::memory_order_relaxed);
            g_out_cv.wait(lk);
            continue;
        }
        if (idx.empty()) {
            lk.unlock();
            finish();
            return;
        }
        if (need && !serial_done(need) && !(rt && need >= cur) && waits < 8) {
            lk.unlock();
            const auto t0 = std::chrono::steady_clock::now();
            if (!d_waited) { d_need = need; d_done = completed_serial(); }
            d_waited = true;
            if (!wait_serial(need, 5ull * 1000 * 1000 * 1000)) NXLOG_W(kTag, "compute out: waited 5 s for submission %llu to read its result", static_cast<unsigned long long>(need));
            d_wait_ns += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
            lk.lock();
            ++waits;
            continue;
        }
        // The copies are recorded from the entries now; the submission and its fence wait (behind everything the queue holds: a whole frame
        // or more) run with the lock free, so the render thread is not stopped meanwhile. The entries are marked fetching: another fetch of
        // them waits for this one, out_retire and out_unregister leave them alone. Afterwards they are found again by id. [nvn]
        // out_fetch_unlocked = false (or the off windows of out_fetch_ab = N presents) keeps the lock across the submission as before.
        std::vector<CopyJob> jobs;
        collect_copies(copy, jobs);
        struct Took { uint64_t id, gen; };
        std::vector<Took> took;
        took.reserve(idx.size());
        for (size_t i : idx) { g_out[i].fetching = true; took.push_back({g_out[i].id, g_out[i].gen}); }
        uint64_t bytes = 0;
        const bool unlocked = fetch_unlocked_on();
        if (!jobs.empty()) {
            if (unlocked) lk.unlock();
            device().submit_once([&](VkCommandBuffer cb) { bytes = record_jobs(cb, jobs); });
            if (unlocked) lk.lock();
        }
        (by_fault ? g_ob_fault : g_ob_inval).fetch_add(bytes, std::memory_order_relaxed);
        (by_fault ? g_on_fault : g_on_inval).fetch_add(1, std::memory_order_relaxed);
        std::vector<size_t> gone;
        struct Span { Mirror* m; uint64_t first, last; };
        std::vector<Span> spans;
        for (const Took& t : took) {
            size_t i = 0;
            while (i < g_out.size() && g_out[i].id != t.id) ++i;
            if (i == g_out.size()) continue;               // gone meanwhile (cannot happen while fetching; kept for safety)
            OutEntry& e = g_out[i];
            e.fetching = false;
            if (e.pending) {                               // written again by the submission being recorded: it stays out, unguarded
                e.guarded = false;
                e.flushed = 0;
                e.serial = 0;
                if (e.m) spans.push_back({e.m, e.first, e.last});
            } else if (e.gen != t.gen) {                   // written again and its submission ended meanwhile: that newer result is the
                g_fetch_overtaken.fetch_add(1, std::memory_order_relaxed);   // entry's now (sent back at the end of it: CPU-read pages)
            } else {
                gone.push_back(i);
            }
        }
        for (const Span& s : spans)
            if (s.m) settle(s.m, s.first, s.last);
        remove_entries(gone);
        out_changed();
        d_bytes = bytes;
        lk.unlock();
        g_out_cv.notify_all();
        finish();
        return;
    }
}
void out_unregister(Mirror* m) {
    OutLock ol;
    // a fetch copying from this pool's copy with the lock free: the buffers must outlive its submission
    if (!g_out_cv.wait_for(ol.lk, std::chrono::seconds(10), [m] { for (const OutEntry& e : g_out) if (e.m == m && e.fetching) return false; return true; }))
        NXLOG_W(kTag, "compute out: a fetch from a pool copy being destroyed did not finish in 10 s");
    out_changed();
    g_out_mirrors.erase(std::remove(g_out_mirrors.begin(), g_out_mirrors.end(), m), g_out_mirrors.end());
    // the pool is going away: its held results with it (the region's destruction makes every page writable again)
    g_out.erase(std::remove_if(g_out.begin(), g_out.end(), [m](const OutEntry& e) { return e.m == m; }), g_out.end());
    recompute_bounds();
}

void out_reset() {
    OutLock ol;
    if (!g_out_cv.wait_for(ol.lk, std::chrono::seconds(10), [] { for (const OutEntry& e : g_out) if (e.fetching) return false; return true; }))   // a guest thread
        NXLOG_W(kTag, "compute out: a fetch did not finish in 10 s at shutdown");                                                    // stopped mid-fetch
    std::vector<size_t> all(g_out.size());
    for (size_t i = 0; i < all.size(); ++i) { all[i] = i; g_out[i].guarded = false; }
    remove_entries(all);
}

// One line every 10 s: what went back to game memory and why, and what the copies hold.
void out_log() {
    static auto last = std::chrono::steady_clock::now();
    static uint64_t p_end = 0, p_gpu = 0, p_fault = 0, p_inval = 0, pn_fault = 0, pn_inval = 0, pn_def = 0, pn_read = 0, pn_ng = 0, pn_gf = 0, p_gfaults = 0;
    const auto now = std::chrono::steady_clock::now();
    const double secs = std::chrono::duration<double>(now - last).count();
    if (secs < 10.0) return;
    last = now;
    {
        OutLock ol;
        delta_log(secs);
    }
    const uint64_t end = g_ob_end.load(), gpu = g_ob_gpu.load(), fault = g_ob_fault.load(), inval = g_ob_inval.load();
    const uint64_t nf = g_on_fault.load(), ni = g_on_inval.load(), nd = g_on_deferred.load(), nr = g_on_eager_read.load(), ng = g_on_eager_noguard.load(),
                   ngf = g_on_gpu_flush.load(), gfaults = mem::ww::guard_faults();
    const uint64_t total = (end - p_end) + (gpu - p_gpu) + (fault - p_fault) + (inval - p_inval);
    size_t held = 0;
    uint64_t held_bytes = 0;
    {
        OutLock ol;
        for (const OutEntry& e : g_out)
            if (e.guarded && !e.flushed) { ++held; held_bytes += e.size; }
    }
    NXLOG_I(kTag, "compute out (%s): copied back %.0f KB/s over %.1f s (end of submission %llu KiB, for GPU readers %llu KiB in %llu flushes, on demand: "
            "CPU touches %llu KiB in %llu copies, invalidates %llu KiB in %llu copies; guard faults %llu); held in the copy %zu results, %llu KiB "
            "(held %llu, sent back at once %llu: CPU-read pages, %llu: not guardable; split %llu in all)",
            out_lazy() ? "lazy" : "always", total / 1000.0 / secs, secs, (unsigned long long)page_size_kb(end - p_end), (unsigned long long)page_size_kb(gpu - p_gpu),
            (unsigned long long)(ngf - pn_gf), (unsigned long long)page_size_kb(fault - p_fault), (unsigned long long)(nf - pn_fault),
            (unsigned long long)page_size_kb(inval - p_inval), (unsigned long long)(ni - pn_inval), (unsigned long long)(gfaults - p_gfaults), held,
            (unsigned long long)(held_bytes >> 10), (unsigned long long)(nd - pn_def), (unsigned long long)(nr - pn_read), (unsigned long long)(ng - pn_ng),
            (unsigned long long)g_on_split.load(std::memory_order_relaxed));
    {
        std::vector<EagerDiag> v;
        {
            OutLock ol;
            for (const auto& kv : g_eager_diag) v.push_back(kv.second);
            g_eager_diag.clear();
        }
        std::sort(v.begin(), v.end(), [](const EagerDiag& a, const EagerDiag& b) { return a.bytes > b.bytes; });
        for (size_t i = 0; i < v.size() && i < 8; ++i)
            NXLOG_I(kTag, "  sent back at submission end: 0x%llx +0x%llx, %llu times, %llu KiB%s; last time %llu pages: %llu CPU-read, %llu not armed; %llu times held from before",
                    (unsigned long long)v[i].addr, (unsigned long long)v[i].size, (unsigned long long)v[i].n, (unsigned long long)(v[i].bytes >> 10),
                    v[i].cpu_read ? " (CPU-read pages)" : "", (unsigned long long)v[i].pages, (unsigned long long)v[i].read_pages,
                    (unsigned long long)v[i].unarmed_pages, (unsigned long long)v[i].was_guarded);
    }
    static const char* const kWhyShort[kOutWhyCount] = {"reader", "end", "off", "dispatch", "draw buf", "draw ssbo/xfb", "indirect", "buf write", "buf read",
                                                        "texture", "event", "other", "gmem"};
    {   // GPU-reader copy-backs by result
        std::vector<FlushDiag> v;
        {
            OutLock ol;
            for (const auto& kv : g_flush_diag) v.push_back(kv.second);
            g_flush_diag.clear();
        }
        std::sort(v.begin(), v.end(), [](const FlushDiag& a, const FlushDiag& b) { return a.bytes > b.bytes; });
        for (size_t i = 0; i < v.size() && i < 8; ++i) {
            std::string w;
            for (int k = 0; k < kOutWhyCount; ++k)
                if (v[i].n[k]) { char t[48]; snprintf(t, sizeof t, " %s %llu", kWhyShort[k], (unsigned long long)v[i].n[k]); w += t; }
            NXLOG_I(kTag, "  flushed for GPU readers: 0x%llx +0x%llx, %llu KiB (entry sizes):%s", (unsigned long long)v[i].addr, (unsigned long long)v[i].size,
                    (unsigned long long)(v[i].bytes >> 10), w.c_str());
        }
    }
    {   // CPU fetches of held results by source (who touched them, how long they waited for the GPU)
        std::vector<std::pair<FetchKey, FetchStat>> v;
        {
            std::lock_guard<std::mutex> lk(g_fd_mu);
            v.assign(g_fd.begin(), g_fd.end());
            g_fd.clear();
        }
        std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second.wait_ns + a.second.n > b.second.wait_ns + b.second.n; });
        for (size_t i = 0; i < v.size() && i < 8; ++i) {
            const FetchKey& k = v[i].first;
            const FetchStat& st = v[i].second;
            NXLOG_I(kTag, "  CPU fetch: %s pc %llx (guest f_%llx) lr %llx on result %llx +%llx: %llu times, %llu pages newly CPU-read, %llu KiB, waited %llu times "
                    "%.1f ms (max %.1f)", kFetchSrcName[k.src], (unsigned long long)k.pc, (unsigned long long)(k.pc ? guest_function_of(k.pc) : 0),
                    (unsigned long long)k.lr, (unsigned long long)k.entry, (unsigned long long)st.entry_size, (unsigned long long)st.n,
                    (unsigned long long)st.new_pages, (unsigned long long)(st.bytes >> 10), (unsigned long long)st.waits, st.wait_ns / 1e6, st.max_wait_ns / 1e6);
        }
    }
    static uint64_t pw_b[kOutWhyCount] = {}, pw_n[kOutWhyCount] = {};
    static const char* const kWhyName[kOutWhyCount] = {"reader", "submit end", "copies off", "dispatch", "draw vertex/index/uniform", "draw storage/xfb",
                                                       "indirect", "buffer write", "buffer read", "texture upload", "event", "other",
                                                       "global memory"};
    std::string why;
    for (int w = 0; w < kOutWhyCount; ++w) {
        const uint64_t b = g_why_bytes[w].load(), n = g_why_n[w].load();
        if (b != pw_b[w] || n != pw_n[w]) {
            char t[96];
            snprintf(t, sizeof t, "%s%s %llu KiB/%llu", why.empty() ? "" : ", ", kWhyName[w], (unsigned long long)((b - pw_b[w]) >> 10), (unsigned long long)(n - pw_n[w]));
            why += t;
        }
        pw_b[w] = b; pw_n[w] = n;
    }
    if (!why.empty()) NXLOG_I(kTag, "compute out: copy-backs by reason (KiB/flushes over %.1f s): %s", secs, why.c_str());
    {   // the index and the lock (render thread)
        static uint64_t p_rb = 0, p_rbns = 0, p_q = 0, p_lw = 0, p_lwns = 0, p_bw = 0, p_ov = 0;
        const uint64_t rb = g_idx_rebuilds.load(), rbns = g_idx_rebuild_ns.load(), q = g_idx_q, lw = g_lock_waits.load(), lwns = g_lock_wait_ns.load(),
                       bw = g_fetch_busy_waits.load(), ov = g_fetch_overtaken.load();
        NXLOG_I(kTag, "compute out index over %.1f s: %llu queries, %llu rebuilds (%.2f ms), %zu entries now; render thread waited %llu times %.2f ms in all for the lock "
                      "(max %.2f ms since start); fetches that waited for another %llu, entries written again during a fetch %llu",
                secs, (unsigned long long)(q - p_q), (unsigned long long)(rb - p_rb), (rbns - p_rbns) / 1e6, g_out_n.load(), (unsigned long long)(lw - p_lw),
                (lwns - p_lwns) / 1e6, g_lock_wait_max_ns.load() / 1e6, (unsigned long long)(bw - p_bw), (unsigned long long)(ov - p_ov));
        NXLOG_I(kTag, "compute out index: %llu updates in place since start (out_index_inc)", (unsigned long long)g_idx_inc.load(std::memory_order_relaxed));
        static uint64_t p_wa = 0, p_wh = 0;
        const uint64_t wa = g_win_asks, wh = g_win_hits;
        if (wa != p_wa) NXLOG_I(kTag, "compute out: pointer windows asked %llu times, %llu answered from the no-overlap table", (unsigned long long)(wa - p_wa), (unsigned long long)(wh - p_wh));
        p_wa = wa; p_wh = wh;
        p_rb = rb; p_rbns = rbns; p_q = q; p_lw = lw; p_lwns = lwns; p_bw = bw; p_ov = ov;
    }
    static uint64_t p_skip = 0;
    const uint64_t skip = g_on_inval_skipped.load(std::memory_order_relaxed);
    if (skip != p_skip) NXLOG_I(kTag, "compute out: %llu invalidates left to the guards over %.1f s", (unsigned long long)(skip - p_skip), secs);
    p_skip = skip;
    p_end = end; p_gpu = gpu; p_fault = fault; p_inval = inval; pn_fault = nf; pn_inval = ni; pn_def = nd; pn_read = nr; pn_ng = ng; pn_gf = ngf; p_gfaults = gfaults;
}

}  // namespace gpu
}  // namespace nxrt::nvn::be
