// SPDX-License-Identifier: GPL-3.0-or-later
// vi:u, vi:s, vi:m (the display services and the binder driver of a layer's buffer queue) and nvdrv, nvdrv:a, nvdrv:s, nvdrv:t (the
// nvhost devices), as far as the SDK uses them before NVN is handed to our graphics bootstrap (S3c).
//
// Adapted from ScarletNX src/nx/hle/services/vi.swipc, vi.c, nvdrv.swipc and nvdrv.c (GPL-3.0-or-later; vi.c takes its parcel and
// buffer-queue layouts from Eden and yuzu, GPL-2.0-or-later) and from Ryujinx src/Ryujinx.HLE/HOS/Services/{Vi,Nv,SurfaceFlinger}
// (MIT). Command ids and layouts were also checked against https://switchbrew.org/wiki/NV_services and Display_services.
//
// Not here, left to the GPU back end: nvhost channel submission (SUBMIT_GPFIFO only answers success), fences beyond success, presentation.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "nxrt/gauge.h"
#include "nxrt/log.h"
#include "nxrt/services_api.h"
#include "services_internal.h"

namespace nxrt::services {
namespace {

constexpr const char* kTagVi = "vi";
constexpr const char* kTagNv = "nvdrv";

// Horizon results of the vi module (114): operation failed 0x272, not found 0xE72 (ScarletNX vi.c RC(114,1), RC(114,7)).
constexpr uint32_t kViOperationFailed = 0x272;
constexpr uint32_t kViNotFound = 0xE72;

// NvError values the nvdrv replies carry (Ryujinx Nv/Types/NvResult.cs; Switchbrew NV_services uses the same numbers).
constexpr uint32_t kNvOk = 0;
constexpr uint32_t kNvNotImplemented = 1;
constexpr uint32_t kNvNotSupported = 2;
constexpr uint32_t kNvNotInitialized = 3;
constexpr uint32_t kNvInvalidParameter = 4;
constexpr uint32_t kNvInsufficientMemory = 6;
constexpr uint32_t kNvInvalidSize = 10;
constexpr uint32_t kNvInvalidValue = 11;
constexpr uint32_t kNvFileOperationFailed = 0x30003;
constexpr uint32_t kNvNotAvailableInProduction = 0x30006;
constexpr uint32_t kSyncptCount = 192;   // the Tegra X1's hardware syncpoints

// android::status_t values the buffer queue answers with (as uint32 words in the reply parcel)
constexpr uint32_t kBqOk = 0;
constexpr uint32_t kBqEBusy = static_cast<uint32_t>(-16);
constexpr uint32_t kBqNoInit = static_cast<uint32_t>(-19);
constexpr uint32_t kBqBadValue = static_cast<uint32_t>(-22);

constexpr uint32_t kLayerSlots = 8;        // a layer's binder id is its slot + 1
constexpr uint64_t kStrayLayerBase = 0x10000;
constexpr uint32_t kNativeWindowSize = 0x3c;

// ============================================================ shared helpers

// A byte span the nvdrv ioctl code reads and writes (a guest buffer, or a copy of one)
struct Arg {
    uint8_t* p = nullptr;
    uint64_t n = 0;
    uint32_t u32(uint64_t off) const {
        uint32_t v = 0;
        if (p && off + 4 <= n) std::memcpy(&v, p + off, 4);
        return v;
    }
    uint64_t u64(uint64_t off) const {
        uint64_t v = 0;
        if (p && off + 8 <= n) std::memcpy(&v, p + off, 8);
        return v;
    }
    void put32(uint64_t off, uint32_t v) const {
        if (p && off + 4 <= n) std::memcpy(p + off, &v, 4);
    }
    void put64(uint64_t off, uint64_t v) const {
        if (p && off + 8 <= n) std::memcpy(p + off, &v, 8);
    }
};

Arg arg_of(const ipc::Buffer& b) { return Arg{buf_mut(b), b.size}; }

// The NUL-terminated string a guest buffer holds (at most the buffer's size).
std::string c_string(const ipc::Buffer& b) {
    std::string s;
    if (!b.addr) return s;
    const char* p = reinterpret_cast<const char*>(static_cast<uintptr_t>(b.addr));
    s.assign(p, strnlen(p, static_cast<size_t>(b.size)));
    return s;
}

// Logs a binder or nvdrv condition once per code (a game may call DequeueBuffer in a loop).
void warn_once(uint32_t code, const char* what) {
    static std::atomic<uint32_t> seen{0};
    const uint32_t bit = 1u << (code & 31);
    if (!(seen.fetch_or(bit) & bit)) NXLOG_W(kTagVi, "binder transaction %u %s", code, what);
}

// Registers a command that is accepted and does nothing (logged at Debug).
void accept_cmd(ipc::Service& s, uint32_t id, const char* name, const char* tag) {
    s.add_cmd(id, name, [name, tag](ipc::Request&, ipc::Response&) -> uint32_t {
        NXLOG_D(tag, "%s accepted, nothing done", name);
        return 0;
    });
}

// ============================================================ vi: displays and layers

// nn::vi::DisplayName ids: the names Switchbrew lists, in its order ("Default" is display 0).
bool display_id_of(const char* name40, uint64_t* id) {
    static const char* const names[] = {"Default", "External", "Edid", "Internal", "Null"};
    for (uint64_t i = 0; i < 5; ++i) {
        if (std::strncmp(name40, names[i], 0x40) == 0) {
            *id = i;
            return true;
        }
    }
    return false;
}

// The DisplayName a command carries as its first 0x40 raw bytes.
bool display_from_raw(const ipc::Request& r, uint64_t* id) {
    char nm[0x41] = {};
    if (r.in) std::memcpy(nm, r.in, std::min<size_t>(0x40, r.in_size));
    return display_id_of(nm, id);
}

// The docked and handheld resolutions (amctl::docked). The "Default" display always reports 1080p (Switchbrew Display_services).
void display_size(uint32_t* w, uint32_t* h) {
    *w = amctl::docked() ? 1920 : 1280;
    *h = amctl::docked() ? 1080 : 720;
}

struct Layer {
    bool used = false;
    uint64_t layer_id = 0;
    bool connected = false;
    uint32_t buffer_count = 2;
    uint32_t width = 0, height = 0;
    ServiceEvent event;   // signalled when a buffer comes free: never, since nothing is ever queued (see DequeueBuffer)
};

struct ViState {
    std::mutex mu;
    Layer layers[kLayerSlots];
    uint64_t next_stray = kStrayLayerBase;
    ServiceEvent vsync;
    bool vsync_on = false;
    bool vsync_stop = false;
    std::condition_variable cv;
    std::thread th;
};
ViState& vi() {
    static ViState s;
    return s;
}

// The display's vsync: a host thread signals the event at 60 Hz. Started by the first GetDisplayVsyncEvent, stopped by vi_nv_reset().
// The vsync runs on an absolute 60 Hz schedule (start + n / 60 s). A relative wait_for(16667 us) became a 16 ms (or 15.6 ms) wait on
// Windows, so the event came at 62-64 Hz: OoT, which waits two vsyncs per frame, ran at 32 fps, and its movies and their voice lines
// 6 % fast (A11). After a stall it skips to the next slot instead of firing the missed ones in a burst.
void vsync_main() {
    ViState& s = vi();
    using clk = std::chrono::steady_clock;
    constexpr auto kPeriod = std::chrono::duration_cast<clk::duration>(std::chrono::duration<double>(1.0 / 60.0));
#ifdef _WIN32
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
#endif
    auto next = clk::now() + kPeriod;
    std::unique_lock<std::mutex> lk(s.mu);
    while (!s.vsync_stop) {
        auto now = clk::now();
        if (now < next) {
#ifdef _WIN32
            lk.unlock();
            LARGE_INTEGER due;
            due.QuadPart = -std::max<int64_t>(1, std::chrono::duration_cast<std::chrono::nanoseconds>(next - now).count() / 100);
            if (!timer || !SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE) || WaitForSingleObject(timer, 100) != WAIT_OBJECT_0)
                std::this_thread::sleep_until(next);
            lk.lock();
#else
            s.cv.wait_until(lk, next, [&] { return s.vsync_stop; });
#endif
            continue;   // re-check the stop flag and the time (a wait can end a little early)
        }
        s.vsync.signal();
        next += kPeriod;
        if (now - next > kPeriod) next = now + kPeriod;   // stalled: resume on the next slot, no burst of missed vsyncs
    }
#ifdef _WIN32
    if (timer) CloseHandle(timer);
#endif
}

void vsync_stop_and_join() {
    ViState& s = vi();
    std::thread t;
    {
        std::lock_guard<std::mutex> g(s.mu);
        if (!s.vsync_on) return;
        s.vsync_stop = true;
        s.cv.notify_all();
        t = std::move(s.th);
    }
    if (t.joinable()) t.join();
    std::lock_guard<std::mutex> g(s.mu);
    s.vsync_on = false;
    s.vsync_stop = false;
}

// Caller holds vi().mu. Layer lookups: a binder id is the slot + 1; nullptr when no layer is open there.
Layer* layer_by_binder(int32_t binder) {
    if (binder < 1 || binder > static_cast<int32_t>(kLayerSlots)) return nullptr;
    Layer& l = vi().layers[binder - 1];
    return l.used ? &l : nullptr;
}

// Caller holds vi().mu. Opens (or finds) the layer; returns its binder id, 0 when every slot is taken.
uint32_t layer_open_locked(uint64_t layer_id) {
    ViState& s = vi();
    int free_slot = -1;
    for (uint32_t i = 0; i < kLayerSlots; ++i) {
        if (s.layers[i].used && s.layers[i].layer_id == layer_id) return i + 1;
        if (!s.layers[i].used && free_slot < 0) free_slot = static_cast<int>(i);
    }
    if (free_slot < 0) return 0;
    Layer& l = s.layers[free_slot];
    display_size(&l.width, &l.height);
    l.used = true;
    l.layer_id = layer_id;
    l.connected = false;
    l.buffer_count = 2;
    l.event = ServiceEvent();
    return static_cast<uint32_t>(free_slot) + 1;
}

// Caller holds vi().mu. CloseLayer and DestroyStrayLayer.
void layer_close_locked(uint64_t layer_id) {
    ViState& s = vi();
    for (uint32_t i = 0; i < kLayerSlots; ++i) {
        Layer& l = s.layers[i];
        if (!l.used || l.layer_id != layer_id) continue;
        l.used = false;
        l.connected = false;
        l.event = ServiceEvent();
        return;
    }
    NXLOG_W(kTagVi, "closing layer %llx, which isn't open", static_cast<unsigned long long>(layer_id));
}

// The native window a layer hands back: a parcel of one flattened binder (Switchbrew Display_services "NativeWindow"): header
// (data size 0x28 at 0, data offset 0x10, objects size 4 at 8, objects offset 0x38 at 0xC), data (0x2 at 0x10, the process word 1
// at 0x14, the binder id as u64 at 0x18, "dispdrv\0" at 0x28), and an empty object list. Returns the size the parcel needs.
uint32_t native_window(const ipc::Buffer& out, uint32_t binder, int64_t* size) {
    if (!binder) return kViOperationFailed;
    uint8_t p[kNativeWindowSize] = {};
    const uint32_t words[] = {0x28, 0x10, 4, 0x38, 2, 1};
    std::memcpy(p, words, sizeof words);
    const uint64_t id = binder;
    std::memcpy(p + 0x18, &id, 8);
    std::memcpy(p + 0x28, "dispdrv", 8);
    *size = static_cast<int64_t>(sizeof p);
    if (out.size < sizeof p) {
        NXLOG_W(kTagVi, "native window buffer of 0x%llx bytes, 0x%x are needed", static_cast<unsigned long long>(out.size),
                static_cast<unsigned>(sizeof p));
        return 0;
    }
    buf_write(out, p, sizeof p);
    return 0;
}

// ---- the binder driver of a layer's buffer queue (IHOSBinderDriver) ----

// A parcel's reader: the header, the interface token (strict policy, then the descriptor as a length and UTF-16 units with a NUL,
// padded to 4), then the call's words.
struct ParcelIn {
    const uint8_t* d = nullptr;
    uint64_t n = 0, pos = 0;
    void open(const uint8_t* data, uint64_t size) {
        d = data;
        n = size;
        uint32_t off = 0;
        if (n >= 8) std::memcpy(&off, d + 4, 4);
        pos = off < n ? off : n;
        u32();                                       // strict policy
        const uint32_t len = u32();
        if (len < 0x100) pos += (((len + 1) * 2) + 3) & ~3u;
        if (pos > n) pos = n;
    }
    uint32_t u32() {
        uint32_t v = 0;
        if (pos + 4 <= n) std::memcpy(&v, d + pos, 4);
        pos += 4;
        return v;
    }
};

struct ParcelOut {
    std::vector<uint8_t> d;
    void u32(uint32_t v) {
        const size_t o = d.size();
        d.resize(o + 4);
        std::memcpy(d.data() + o, &v, 4);
    }
};

// The reply: the header (data size, data offset 16, no objects, objects offset after the data) and the data.
void reply_parcel(const ipc::Buffer& reply, const std::vector<uint8_t>& data) {
    const uint32_t hdr[4] = {static_cast<uint32_t>(data.size()), 16, 0, static_cast<uint32_t>(16 + data.size())};
    std::vector<uint8_t> out(16 + data.size());
    std::memcpy(out.data(), hdr, 16);
    if (!data.empty()) std::memcpy(out.data() + 16, data.data(), data.size());
    if (out.size() > reply.size)
        NXLOG_W(kTagVi, "binder reply of %llu bytes doesn't fit the game's %llu", static_cast<unsigned long long>(out.size()),
                static_cast<unsigned long long>(reply.size));
    buf_write(reply, out.data(), out.size());
}

// One TransactParcel: the layer's buffer queue answers each IGraphicBufferProducer code. The queue never holds a buffer (the game's
// buffers are not presented here), so DequeueBuffer answers -EBUSY and RequestBuffer / QueueBuffer answer NO_INIT. Outputs come
// first and the status word last, except GetBufferHistory, which answers the status and then a count of 0 (Ryujinx).
uint32_t binder_call(int32_t binder, uint32_t code, const ipc::Buffer& parcel, const ipc::Buffer& reply) {
    if (parcel.size < 16) return kViOperationFailed;
    ParcelIn in;
    in.open(buf_ptr(parcel), parcel.size < 0x10000 ? parcel.size : 0x10000);
    ParcelOut out;
    bool history = false;
    uint32_t status = kBqOk;
    {
        ViState& s = vi();
        std::lock_guard<std::mutex> g(s.mu);
        Layer* l = layer_by_binder(binder);
        if (!l) return kViNotFound;
        switch (code) {
        case 1:   // RequestBuffer: a null graphic buffer
            out.u32(0);
            status = kBqNoInit;
            warn_once(code, "RequestBuffer: no buffer in the queue, NO_INIT");
            break;
        case 2: {   // SetBufferCount(i32)
            const int32_t count = static_cast<int32_t>(in.u32());
            if (count < 1) status = kBqBadValue;
            else l->buffer_count = static_cast<uint32_t>(count);
            break;
        }
        case 3:   // DequeueBuffer: no free slot (-EBUSY) and no slot number
            out.u32(static_cast<uint32_t>(-1));
            status = kBqEBusy;
            warn_once(code, "DequeueBuffer: no free buffer, -EBUSY");
            break;
        case 4:
        case 5:
        case 6:
        case 8:
        case 13:
        case 14: break;   // DetachBuffer, DetachNextBuffer, AttachBuffer, CancelBuffer, AllocateBuffers, SetPreallocatedBuffer: accepted
        case 7:   // QueueBuffer: nothing was dequeued
            status = kBqNoInit;
            warn_once(code, "QueueBuffer: nothing dequeued, NO_INIT");
            break;
        case 9: {   // Query(u32 what) -> i32 value
            const uint32_t what = in.u32();
            uint32_t v = 0;
            bool known = true;
            switch (what) {
            case 0: v = l->width; break;                                    // Width
            case 1: v = l->height; break;                                   // Height
            case 2: v = 1; break;                                           // Format: RGBA8888
            case 3: v = 1; break;                                           // MinUnqueuedBuffers
            case 6: v = l->width; break;                                    // DefaultWidth
            case 7: v = l->height; break;                                   // DefaultHeight
            case 9:
            case 10:
            case 11: v = 0; break;                                          // ConsumerRunningBehind, ConsumerUsageBits, StickyTransform
            case 12: v = l->buffer_count; break;                            // MaxBufferCountAsync
            default: known = false; break;
            }
            out.u32(v);
            if (!known) status = kBqBadValue;
            break;
        }
        case 10:   // Connect(bool has_listener, u32 api, bool controlled): QueueBufferOutput {width, height, transform hint, pending}
            l->connected = true;
            out.u32(l->width);
            out.u32(l->height);
            out.u32(0x10);   // NoVSyncCapability, as Ryujinx answers
            out.u32(0);
            break;
        case 11:   // Disconnect(u32 api)
            if (!l->connected) status = kBqNoInit;
            l->connected = false;
            break;
        case 17:   // GetBufferHistory: status, then a count of 0
            history = true;
            break;
        default:
            warn_once(code, "is not implemented: answered with success");
            break;
        }
    }
    if (history) {
        out.u32(status);
        out.u32(0);
    } else {
        out.u32(status);
    }
    reply_parcel(reply, out.d);
    return 0;
}

// vsync: the first request starts the 60 Hz thread; each request is a new handle onto the one event.
uint32_t vsync_new_handle() {
    ViState& s = vi();
    std::lock_guard<std::mutex> g(s.mu);
    if (!s.vsync_on) {
        s.vsync_on = true;
        s.vsync_stop = false;
        s.th = std::thread(vsync_main);
        NXLOG_I(kTagVi, "display vsync: 60 Hz event started");
    }
    return s.vsync.new_handle();
}

uint32_t vsync_handle(ipc::Response& rsp) {
    const uint32_t h = vsync_new_handle();
    if (!h) return kViOperationFailed;
    rsp.out_copy.push_back(h);
    return 0;
}

// An unsignalled event handle (hotplug, mode changed, texture presenting): nothing here raises them.
uint32_t fresh_event_handle(ipc::Response& rsp) {
    ServiceEvent ev;
    const uint32_t h = ev.new_handle();
    if (!h) return kViOperationFailed;
    rsp.out_copy.push_back(h);
    return 0;
}

// The Default display's DisplayInfo (0x60 bytes): name at 0, has-limited-layers at 0x40, layer count max at 0x48, the 1080p size at
// 0x50 and 0x58 (Switchbrew Display_services; the layer count of 1 is ScarletNX's value).
void write_display_info(const ipc::Buffer& out) {
    uint8_t info[0x60] = {};
    std::memcpy(info, "Default", 8);
    info[0x40] = 1;
    const uint64_t one = 1, w = 1920, h = 1080;
    std::memcpy(info + 0x48, &one, 8);
    std::memcpy(info + 0x50, &w, 8);
    std::memcpy(info + 0x58, &h, 8);
    buf_write(out, info, sizeof info);
}

std::shared_ptr<ipc::Service> make_binder_driver();   // forward: the IHOSBinderDriver interface

void setup_binder_driver(ipc::Service& s) {
    s.add_cmd(0, "TransactParcel", [](ipc::Request& r, ipc::Response&) -> uint32_t {
        return binder_call(r.arg<int32_t>(0), r.arg<uint32_t>(4), r.in_buf(0), r.out_buf(0));
    });
    accept_cmd(s, 1, "AdjustRefcount", kTagVi);
    s.add_cmd(2, "GetNativeHandle", [](ipc::Request& r, ipc::Response& rsp) -> uint32_t {
        std::lock_guard<std::mutex> g(vi().mu);
        Layer* l = layer_by_binder(r.arg<int32_t>(0));
        if (!l) return kViNotFound;
        const uint32_t h = l->event.new_handle();
        if (!h) return kViOperationFailed;
        rsp.out_copy.push_back(h);
        return 0;
    });
    s.add_cmd(3, "TransactParcelAuto", [](ipc::Request& r, ipc::Response&) -> uint32_t {
        return binder_call(r.arg<int32_t>(0), r.arg<uint32_t>(4), r.in_buf(0), r.out_buf(0));
    });
}

std::shared_ptr<ipc::Service> make_binder_driver() {
    return make_iface("nns::hosbinder::IHOSBinderDriver", setup_binder_driver);
}

std::shared_ptr<ipc::Service> make_system_display();    // forward
std::shared_ptr<ipc::Service> make_manager_display();   // forward

void setup_system_display(ipc::Service& s) {
    s.add_cmd(1200, "GetZOrderCountMin", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        rsp.push<int64_t>(1);
        return 0;
    });
    s.add_cmd(1202, "GetZOrderCountMax", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        rsp.push<int64_t>(255);
        return 0;
    });
    s.add_cmd(1203, "GetDisplayLogicalResolution", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        uint32_t w = 0, h = 0;
        display_size(&w, &h);
        rsp.push<int32_t>(static_cast<int32_t>(w));
        rsp.push<int32_t>(static_cast<int32_t>(h));
        return 0;
    });
    // DisplayModeInfo: width, height, refresh rate (f32), unknown (u32)
    s.add_cmd(3200, "GetDisplayMode", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        uint32_t w = 0, h = 0;
        display_size(&w, &h);
        rsp.push<uint32_t>(w);
        rsp.push<uint32_t>(h);
        rsp.push<float>(60.0f);
        rsp.push<uint32_t>(0);
        return 0;
    });
}

std::shared_ptr<ipc::Service> make_system_display() {
    return make_iface("nn::visrv::sf::ISystemDisplayService", setup_system_display);
}

void setup_manager_display(ipc::Service& s) {
    s.add_cmd(2010, "CreateManagedLayer", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        uint64_t layer = 0;
        {
            std::lock_guard<std::mutex> g(vi().mu);
            layer = vi().next_stray++;
        }
        rsp.push<uint64_t>(layer);
        return 0;
    });
    s.add_cmd(2300, "AcquireLayerTexturePresentingEvent", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        return fresh_event_handle(rsp);
    });
    s.add_cmd(2302, "GetDisplayHotplugEvent", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        return fresh_event_handle(rsp);
    });
    s.add_cmd(2303, "GetDisplayModeChangedEvent", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        return fresh_event_handle(rsp);
    });
}

std::shared_ptr<ipc::Service> make_manager_display() {
    return make_iface("nn::visrv::sf::IManagerDisplayService", setup_manager_display);
}

void setup_app_display(ipc::Service& s) {
    s.add_cmd(100, "GetRelayService", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        rsp.push_object(make_binder_driver());
        return 0;
    });
    s.add_cmd(101, "GetSystemDisplayService", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        rsp.push_object(make_system_display());
        return 0;
    });
    s.add_cmd(102, "GetManagerDisplayService", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        rsp.push_object(make_manager_display());
        return 0;
    });
    s.add_cmd(103, "GetIndirectDisplayTransactionService", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        rsp.push_object(make_binder_driver());
        return 0;
    });
    s.add_cmd(1000, "ListDisplays", [](ipc::Request& r, ipc::Response& rsp) -> uint32_t {
        write_display_info(r.out_buf(0));
        rsp.push<uint64_t>(1);
        return 0;
    });
    s.add_cmd(1010, "OpenDisplay", [](ipc::Request& r, ipc::Response& rsp) -> uint32_t {
        uint64_t id = 0;
        if (!display_from_raw(r, &id)) return kViOperationFailed;
        rsp.push<uint64_t>(id);
        return 0;
    });
    s.add_cmd(1011, "OpenDefaultDisplay", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        rsp.push<uint64_t>(0);
        return 0;
    });
    accept_cmd(s, 1020, "CloseDisplay", kTagVi);
    accept_cmd(s, 1101, "SetDisplayEnabled", kTagVi);
    s.add_cmd(1102, "GetDisplayResolution", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        uint32_t w = 0, h = 0;
        display_size(&w, &h);
        rsp.push<int64_t>(w);
        rsp.push<int64_t>(h);
        return 0;
    });
    s.add_cmd(2020, "OpenLayer", [](ipc::Request& r, ipc::Response& rsp) -> uint32_t {
        uint64_t display = 0;
        if (!display_from_raw(r, &display)) return kViOperationFailed;
        const uint64_t layer = r.arg<uint64_t>(0x40);   // the AppletResourceUserId that follows is not needed here
        uint32_t binder = 0;
        {
            std::lock_guard<std::mutex> g(vi().mu);
            binder = layer_open_locked(layer);
        }
        int64_t size = 0;
        const uint32_t err = native_window(r.out_buf(0), binder, &size);
        if (err) return err;
        rsp.push<int64_t>(size);
        return 0;
    });
    s.add_cmd(2021, "CloseLayer", [](ipc::Request& r, ipc::Response&) -> uint32_t {
        std::lock_guard<std::mutex> g(vi().mu);
        layer_close_locked(r.arg<uint64_t>(0));
        return 0;
    });
    s.add_cmd(2030, "CreateStrayLayer", [](ipc::Request& r, ipc::Response& rsp) -> uint32_t {
        (void)r.arg<uint32_t>(0);   // layer flags, and the display at 8 (only one display exists)
        uint64_t layer = 0;
        uint32_t binder = 0;
        {
            std::lock_guard<std::mutex> g(vi().mu);
            layer = vi().next_stray++;
            binder = layer_open_locked(layer);
        }
        int64_t size = 0;
        const uint32_t err = native_window(r.out_buf(0), binder, &size);
        if (err) return err;
        rsp.push<uint64_t>(layer);
        rsp.push<int64_t>(size);
        return 0;
    });
    s.add_cmd(2031, "DestroyStrayLayer", [](ipc::Request& r, ipc::Response&) -> uint32_t {
        std::lock_guard<std::mutex> g(vi().mu);
        layer_close_locked(r.arg<uint64_t>(0));
        return 0;
    });
    accept_cmd(s, 2101, "SetLayerScalingMode", kTagVi);
    s.add_cmd(2102, "ConvertScalingMode", [](ipc::Request& r, ipc::Response& rsp) -> uint32_t {
        // nn::vi::ScalingMode (None, Freeze, ScaleToWindow, ScaleAndCrop, PreserveAspectRatio) to Android's values
        static const uint64_t android[] = {3, 0, 1, 2, 4};
        const uint32_t mode = r.arg<uint32_t>(0);
        if (mode >= sizeof android / sizeof android[0]) return kViOperationFailed;
        rsp.push<uint64_t>(android[mode]);
        return 0;
    });
    s.add_cmd(2460, "GetIndirectLayerImageRequiredMemoryInfo", [](ipc::Request& r, ipc::Response& rsp) -> uint32_t {
        const int64_t w = r.arg<int64_t>(0), h = r.arg<int64_t>(8);
        const int64_t block = 0x20000;
        rsp.push<int64_t>((w * h * 4 + block - 1) / block * block);
        rsp.push<int64_t>(0x1000);
        return 0;
    });
    s.add_cmd(5202, "GetDisplayVsyncEvent", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        return vsync_handle(rsp);
    });
    s.add_cmd(5203, "GetDisplayVsyncEventForDebug", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        return vsync_handle(rsp);
    });
}

std::shared_ptr<ipc::Service> make_app_display() {
    return make_iface("nn::visrv::sf::IApplicationDisplayService", setup_app_display);
}

// vi:u, vi:s and vi:m. GetDisplayService takes a policy word the display ignores; the proxy-name variants take an 8-byte name first.
void setup_vi_u(ipc::Service& s) {
    auto open = [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        rsp.push_object(make_app_display());
        return 0;
    };
    s.add_cmd(0, "GetDisplayService", open);
    s.add_cmd(1, "GetDisplayServiceWithProxyNameExchange", open);
}

void setup_vi_s(ipc::Service& s) {
    auto open = [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        rsp.push_object(make_app_display());
        return 0;
    };
    s.add_cmd(1, "GetDisplayService", open);
    s.add_cmd(3, "GetDisplayServiceWithProxyNameExchange", open);
}

void setup_vi_m(ipc::Service& s) {
    auto open = [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        rsp.push_object(make_app_display());
        return 0;
    };
    s.add_cmd(2, "GetDisplayService", open);
    s.add_cmd(3, "GetDisplayServiceWithProxyNameExchange", open);
}

// ============================================================ nvdrv: devices, handles, address spaces

enum class Dev { NvMap, Ctrl, CtrlGpu, AsGpu, Gpu, Other };

struct NvMapObj {
    uint32_t size = 0, align = 0, heap = 0, flags = 0, kind = 0;
    uint64_t addr = 0;
    bool allocated = false;
    uint32_t refs = 1;
};

struct NvFile {
    Dev dev = Dev::Other;
    std::map<uint32_t, std::shared_ptr<ServiceEvent>> events;   // QueryEvent: one event per slot or id
    uint64_t user_data = 0;                                     // gpu channel: SET_USER_DATA
    uint32_t syncpt = 0;                                        // gpu channel: its syncpoint (ALLOC_GPFIFO_EX2)
    uint64_t big_page = 0x20000;                                // as-gpu: INITIALIZE_EX
    std::map<uint64_t, uint64_t> ranges;                        // as-gpu: start -> size of each reservation and mapping
};

struct NvState {
    std::mutex mu;
    bool initialized = false;
    uint32_t next_fd = 1;
    uint32_t next_handle = 1;
    uint32_t next_syncpt = 1;
    std::map<uint32_t, std::shared_ptr<NvFile>> files;
    std::map<uint32_t, NvMapObj> maps;
    std::set<uint64_t> seen;     // ioctls logged once at Info
    std::set<uint64_t> warned;   // unhandled ioctls warned once
    bool submit_logged = false;
};
NvState& nv() {
    static NvState s;
    return s;
}

// ---- gauges (include/nxrt/gauge.h): the nvdrv tables, counted under the state lock ----
// Removes its gauge when this file's statics are torn down.
struct GaugeHook {
    const char* name;
    GaugeHook(const char* n, std::function<int64_t()> f) : name(n) { nxrt::gauge_register(n, std::move(f)); }
    ~GaugeHook() { nxrt::gauge_unregister(name); }
};
GaugeHook gauge_nv_files("nv.files", [] {   // open nvdrv device files (nvmap, gpu channels, as-gpu, ...)
    NvState& s = nv();
    std::lock_guard<std::mutex> lk(s.mu);
    return static_cast<int64_t>(s.files.size());
});
GaugeHook gauge_nv_maps("nv.nvmap_objects", [] {   // nvmap allocations alive (removed when the last reference is freed)
    NvState& s = nv();
    std::lock_guard<std::mutex> lk(s.mu);
    return static_cast<int64_t>(s.maps.size());
});

struct DevEntry {
    const char* path;
    Dev dev;
    bool debug_only;   // the debug and profiler devices answer NotSupported (debug mode is off on a retail console)
};
constexpr DevEntry kDevices[] = {
    {"/dev/nvmap", Dev::NvMap, false},
    {"/dev/nvhost-ctrl", Dev::Ctrl, false},
    {"/dev/nvhost-ctrl-gpu", Dev::CtrlGpu, false},
    {"/dev/nvhost-as-gpu", Dev::AsGpu, false},
    {"/dev/nvhost-gpu", Dev::Gpu, false},
    {"/dev/nvhost-nvdec", Dev::Other, false},
    {"/dev/nvhost-nvjpg", Dev::Other, false},
    {"/dev/nvhost-vic", Dev::Other, false},
    {"/dev/nvdisp-ctrl", Dev::Other, false},
    {"/dev/nvsched-ctrl-gpu", Dev::Other, false},
    {"/dev/nvhost-dbg-gpu", Dev::Other, true},
    {"/dev/nvhost-prof-gpu", Dev::Other, true},
};

const char* dev_name(Dev d) {
    switch (d) {
    case Dev::NvMap: return "nvmap";
    case Dev::Ctrl: return "nvhost-ctrl";
    case Dev::CtrlGpu: return "nvhost-ctrl-gpu";
    case Dev::AsGpu: return "nvhost-as-gpu";
    case Dev::Gpu: return "nvhost-gpu";
    default: return "other";
    }
}

// The as-gpu regions (Ryujinx AddressSpaceContext): small pages below 16 GiB, big pages above it.
struct Region {
    uint64_t start, limit, page;
};
constexpr Region kRegions[2] = {{0x100000000ull, 0x400000000ull, 0x1000}, {0x400000000ull, 0x2000000000ull, 0x10000}};

const Region& region_for(uint64_t page) { return page == 0x10000 ? kRegions[1] : kRegions[0]; }

// The first run of `size` bytes in [lo, hi) that no range of `ranges` covers, aligned to `align`; 0 when there is none.
uint64_t find_gap(const std::map<uint64_t, uint64_t>& ranges, uint64_t lo, uint64_t hi, uint64_t size, uint64_t align) {
    if (align == 0) align = 1;
    uint64_t cand = (lo + align - 1) / align * align;
    for (const auto& [start, len] : ranges) {
        if (start + len <= cand) continue;
        if (cand + size <= start) break;
        cand = (start + len + align - 1) / align * align;
    }
    return cand + size <= hi ? cand : 0;
}

bool range_free(const std::map<uint64_t, uint64_t>& ranges, uint64_t start, uint64_t size) {
    for (const auto& [s, len] : ranges)
        if (s < start + size && start < s + len) return false;
    return true;
}

// The ioctl-number fields (Linux _IOC layout, Switchbrew NV_services): direction (bit 30 copies in, bit 31 copies out), size, type, nr.
void log_ioctl(const NvFile& f, uint32_t cmd, uint32_t size, uint32_t dir, bool first) {
    if (first)
        NXLOG_I(kTagNv, "ioctl %s cmd %08x (type %02x nr %02x) size %u dir %u", dev_name(f.dev), cmd, (cmd >> 8) & 0xFF, cmd & 0xFF,
                size, dir);
    NXLOG_T(kTagNv, "ioctl %s cmd %08x size %u", dev_name(f.dev), cmd, size);
}

// ---- nvmap (type 0x01)

uint32_t nvmap_ioctl(uint32_t nr, Arg a, bool& handled) {
    NvState& s = nv();
    switch (nr) {
    case 0x01: {   // CREATE {u32 size; out u32 handle}: size rounded up to a page
        const uint32_t size = a.u32(0);
        if (!size) return kNvInvalidValue;
        NvMapObj m;
        m.size = static_cast<uint32_t>((static_cast<uint64_t>(size) + 0xFFF) & ~static_cast<uint64_t>(0xFFF));
        const uint32_t h = s.next_handle++;
        if (s.next_handle == 0) s.next_handle = 1;
        s.maps[h] = m;
        a.put32(4, h);
        return kNvOk;
    }
    case 0x03: {   // FROM_ID {u32 id; out u32 handle}: the id is the handle
        auto it = s.maps.find(a.u32(0));
        if (it == s.maps.end()) return kNvInvalidValue;
        it->second.refs++;
        a.put32(4, it->first);
        return kNvOk;
    }
    case 0x04: {   // ALLOC {u32 handle, heap, flags; u32 align (inout); u8 kind at 16; u64 addr at 24}
        auto it = s.maps.find(a.u32(0));
        if (it == s.maps.end()) return kNvInvalidValue;
        NvMapObj& m = it->second;
        uint32_t align = a.u32(12);
        if (align & (align - 1)) return kNvInvalidValue;
        if (align < 0x1000) align = 0x1000;
        if (!m.allocated) {
            m.allocated = true;
            m.heap = a.u32(4);
            m.flags = a.u32(8);
            m.kind = a.u32(16) & 0xFF;
            m.addr = a.u64(24);
        }
        m.align = align;
        a.put32(12, align);
        return kNvOk;
    }
    case 0x05: {   // FREE {u32 handle; pad; out u64 address at 8, u32 size at 16, u32 flags at 20}: flags 1 = not freed yet
        auto it = s.maps.find(a.u32(0));
        if (it == s.maps.end()) return kNvInvalidValue;
        NvMapObj& m = it->second;
        if (m.refs > 0) m.refs--;
        const bool freed = m.refs == 0;
        a.put64(8, freed ? m.addr : 0);
        a.put32(16, m.size);
        a.put32(20, freed ? 0 : 1);
        if (freed) s.maps.erase(it);
        return kNvOk;
    }
    case 0x09: {   // PARAM {u32 handle, param; out u32 result}: 1 size, 2 align, 4 heap, 5 kind, 6 compr
        auto it = s.maps.find(a.u32(0));
        if (it == s.maps.end()) return kNvInvalidValue;
        const NvMapObj& m = it->second;
        uint32_t v = 0;
        switch (a.u32(4)) {
        case 1: v = m.size; break;
        case 2: v = m.align; break;
        case 4: v = 0x40000000; break;
        case 5: v = m.kind; break;
        case 6: v = 0; break;
        default: return kNvInvalidValue;   // Base (3) and anything else
        }
        a.put32(8, v);
        return kNvOk;
    }
    case 0x0e: {   // GET_ID {out u32 id; u32 handle}
        const uint32_t h = a.u32(4);
        if (s.maps.find(h) == s.maps.end()) return kNvInvalidValue;
        a.put32(0, h);
        return kNvOk;
    }
    case 0x02:
    case 0x06:
    case 0x07:
    case 0x08:
    case 0x0a:
    case 0x0b:
    case 0x0c:
    case 0x0d:
    case 0x0f:
    case 0x10:
    case 0x11: return kNvNotSupported;   // CLAIM, MMAP, WRITE, READ, PIN, UNPIN, CACHE, IVC, RESERVE: NotSupported on the console
    default:
        handled = false;
        return kNvOk;
    }
}

// ---- nvhost-ctrl (type 0x00)

uint32_t ctrl_ioctl(uint32_t nr, Arg a, bool& handled) {
    switch (nr) {
    case 0x14:   // SYNCPT_READ {u32 id; out u32 value}: zeros
        if (a.u32(0) >= kSyncptCount) return kNvInvalidValue;
        a.put32(4, 0);
        return kNvOk;
    case 0x15:   // SYNCPT_INCR {u32 id}
        if (a.u32(0) >= kSyncptCount) return kNvInvalidValue;
        return kNvOk;
    case 0x16:   // SYNCPT_WAIT {u32 id, thresh; i32 timeout}: the threshold counts as reached
        if (a.u32(0) >= kSyncptCount) return kNvInvalidValue;
        return kNvOk;
    case 0x19:   // SYNCPT_WAITEX {u32 id, thresh; i32 timeout; out u32 value at 12}
        if (a.u32(0) >= kSyncptCount) return kNvInvalidValue;
        a.put32(12, 0);
        return kNvOk;
    case 0x1a:   // SYNCPT_READ_MAX {u32 id; out u32 max}
        if (a.u32(0) >= kSyncptCount) return kNvInvalidValue;
        a.put32(4, 0);
        return kNvOk;
    case 0x1b:   // GET_CONFIG: a retail console answers NotAvailableInProduction (Switchbrew; Ryujinx's note on the same ioctl)
        return kNvNotAvailableInProduction;
    case 0x1c:   // SYNCPT_CLEAR_EVENT_WAIT {u32 slot}
    case 0x1f:   // SYNCPT_ALLOC_EVENT {u32 slot}
    case 0x20:   // SYNCPT_FREE_EVENT {u32 slot}
    case 0x21:   // SYNCPT_FREE_EVENT_BATCH {u64 mask}
        return kNvOk;
    case 0x1d:   // SYNCPT_WAIT_EVENT {u32 id, thresh; i32 timeout; out u32 value at 12}
    case 0x1e:   // SYNCPT_WAIT_EVENT_EX, the same layout
        a.put32(12, 0);
        return kNvOk;
    default:
        handled = false;
        return kNvOk;
    }
}

// ---- nvhost-ctrl-gpu (type 0x47): the values of a Tegra X1 (GM20B), the same words Ryujinx and ScarletNX report

void write_characteristics(uint8_t* c) {   // 0xA0 bytes: GpuCharacteristics, 64-bit fields as low word then high word
    static const uint32_t words[40] = {
        0x120, 0xB, 0xA1, 1,                      // arch, impl, rev, GPC count
        0x40000, 0, 0, 0,                         // L2 cache size, on-board video memory (u64s)
        2, 0x20, 0x20000, 0x20000,                // TPCs per GPC, bus type, big page size, compression page size
        0x1B, 0x30000, 1, 0x503,                  // PDE coverage bits, available big page sizes, GPC mask, SM version
        0x503, 0x80, 0x28, 0,                     // SPA version, warp count, GPU VA bits, reserved
        0x55, 0,                                  // flags (u64)
        0x902D, 0xB197, 0xB1C0, 0xB06F,           // 2D, 3D, compute, GPFIFO classes
        0xA140, 0xB0B5,                           // inline-to-memory, DMA copy classes
        1, 0, 2, 1,                               // FBPs, FBP enable mask, LTCs per FBP, LTSs per LTC
        0, 1, 0x21D70, 0,                         // textures per TPC, GPCs, ROP L2 enable masks
        0x30326D67, 0x62,                         // chip name "gm20b" (u64)
        0, 0,                                     // compression bit store base (u64)
    };
    std::memcpy(c, words, sizeof words);
}

uint32_t ctrlgpu_ioctl(uint32_t nr, Arg a, Arg xout, bool& handled) {
    switch (nr) {
    case 0x01:   // ZCULL_GET_CTX_SIZE {out u32 size}
        a.put32(0, 1);
        return kNvOk;
    case 0x02: {   // ZCULL_GET_INFO: ten u32s
        static const uint32_t info[10] = {0x20, 0x20, 0x400, 0x800, 0x20, 0x20, 0xC0, 0x20, 0x40, 0x10};
        for (uint32_t i = 0; i < 10; ++i) a.put32(i * 4, info[i]);
        return kNvOk;
    }
    case 0x03:   // ZBC_SET_TABLE: accepted
        return kNvOk;
    case 0x05: {   // GET_CHARACTERISTICS {u64 buf_size at 0, u64 buf_addr at 8, GpuCharacteristics at 16}; also copied to an inline out
        uint8_t c[0xA0];
        write_characteristics(c);
        a.put64(0, sizeof c);
        if (a.p && a.n >= 16 + sizeof c) std::memcpy(a.p + 16, c, sizeof c);
        if (xout.p && xout.n >= sizeof c) std::memcpy(xout.p, c, sizeof c);
        return kNvOk;
    }
    case 0x06: {   // GET_TPC_MASKS {u32 mask_buf_size; pad; u64 mask_buf_addr; out u32 mask at 16}
        if (a.u32(0) != 0) {
            const uint32_t mask = 3;   // TPC 0 and 1 present
            a.put32(16, mask);
            if (xout.p && xout.n >= 4) std::memcpy(xout.p, &mask, 4);
        }
        return kNvOk;
    }
    case 0x1c: {   // GET_GPU_TIME {out u64 timestamp; reserved}: nanoseconds since the first call, 56 bits as the device has them
        static const auto t0 = std::chrono::steady_clock::now();
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
        a.put64(0, static_cast<uint64_t>(ns) & 0xFFFFFFFFFFFFFFull);
        return kNvOk;
    }
    default:
        handled = false;
        return kNvOk;
    }
}

// ---- nvhost-as-gpu (type 0x41): a simple address space, first fit in the regions, reservations and mappings in one map

uint32_t as_ioctl(NvFile& f, uint32_t nr, Arg a, bool& handled) {
    NvState& s = nv();
    switch (nr) {
    case 0x01:   // BIND_CHANNEL {i32 fd}
        return kNvOk;
    case 0x02: {   // ALLOC_SPACE {u32 pages, page_size, flags; u32 pad; u64 offset at 16 (the alignment when not fixed)}
        const uint64_t pages = a.u32(0), page = a.u32(4), size = pages * page;
        const uint32_t flags = a.u32(8);
        const uint64_t want = a.u64(16);
        if (!size) return kNvInvalidValue;
        const Region& rg = region_for(page);
        uint64_t got = 0;
        if (flags & 1) {   // FIXED_OFFSET
            if (want >= rg.start && want + size <= rg.limit && range_free(f.ranges, want, size)) got = want;
        } else {
            got = find_gap(f.ranges, rg.start, rg.limit, size, want ? want : page);
        }
        if (!got) {
            a.put64(16, 0);
            NXLOG_W(kTagNv, "ALLOC_SPACE of 0x%llx bytes has no room", static_cast<unsigned long long>(size));
            return kNvInsufficientMemory;
        }
        f.ranges[got] = size;
        a.put64(16, got);
        return kNvOk;
    }
    case 0x03: {   // FREE_SPACE {u64 offset; u32 pages, page_size}
        const uint64_t off = a.u64(0);
        auto it = f.ranges.find(off);
        if (it == f.ranges.end()) return kNvInvalidValue;
        f.ranges.erase(it);
        return kNvOk;
    }
    case 0x05:   // UNMAP_BUFFER {u64 offset}: unknown offsets are accepted, as Ryujinx does
        f.ranges.erase(a.u64(0));
        return kNvOk;
    case 0x06: {   // MAP_BUFFER_EX {u32 flags; i32 kind; u32 nvmap; i32 page_size; u64 buffer_off; u64 map_size; u64 offset at 32}
        const uint32_t flags = a.u32(0), handle = a.u32(8), page_arg = a.u32(12);
        const uint64_t map_size_arg = a.u64(24), want = a.u64(32);
        auto it = s.maps.find(handle);
        if (it == s.maps.end()) return kNvInvalidValue;
        const NvMapObj& m = it->second;
        const uint64_t size = map_size_arg ? map_size_arg : m.size;
        const uint64_t page = page_arg ? page_arg : (m.align ? m.align : 0x1000);
        const Region& rg = region_for(page);
        uint64_t va = 0;
        if (flags & 1) {
            if (want >= rg.start && want + size <= rg.limit && range_free(f.ranges, want, size)) va = want;
        } else {
            va = find_gap(f.ranges, rg.start, rg.limit, size, page);
        }
        if (!va) {
            a.put64(32, 0);
            return kNvInvalidValue;
        }
        f.ranges[va] = size;
        a.put64(32, va);
        return kNvOk;
    }
    case 0x08: {   // GET_VA_REGIONS {u64; u32 buffer_size at 8; pad; {u64 offset, u32 page, u32 pad, u64 pages} x2 at 16}
        const uint32_t entries = std::min<uint32_t>(a.u32(8) / 24, 2);
        for (uint32_t i = 0; i < entries; ++i) {
            const uint64_t base = 16 + 24 * static_cast<uint64_t>(i);
            const Region& rg = kRegions[i];
            a.put64(base, rg.start);
            a.put32(base + 8, static_cast<uint32_t>(rg.page));
            a.put32(base + 12, 0);
            a.put64(base + 16, (rg.limit - rg.start) / rg.page);
        }
        a.put32(8, 48);
        return kNvOk;
    }
    case 0x09: {   // INITIALIZE_EX {u32 flags, i32 as_fd, u32 big_page_size at 8, ...}
        const uint32_t big = a.u32(8);
        f.big_page = big ? big : 0x20000;
        return kNvOk;
    }
    default:
        handled = false;
        return kNvOk;
    }
}

// ---- nvhost-gpu (type 0x48 the channel, 0x47 SET_USER_DATA): the channel's bookkeeping; submission is the GPU back end's

uint32_t gpu_ioctl(NvFile& f, uint32_t nr, Arg a, bool& handled) {
    NvState& s = nv();
    switch (nr) {
    case 0x01:   // SET_NVMAP_FD {i32}
        return kNvOk;
    case 0x08:   // SUBMIT_GPFIFO
    case 0x1b:   // SUBMIT_GPFIFO through Ioctl2: answered, never run here
        if (!s.submit_logged) {
            s.submit_logged = true;
            NXLOG_W(kTagNv, "SUBMIT_GPFIFO accepted and not run: the GPU back end owns command submission");
        }
        return kNvOk;
    case 0x09:   // ALLOC_OBJ_CTX {u32 class, flags; out u64 object id at 8}: the id is the class number, as ScarletNX answers
        a.put64(8, a.u32(0));
        return kNvOk;
    case 0x16:   // GET_ERROR_NOTIFIER: nothing has gone wrong, so the notifier is all zeros
        if (a.p) std::memset(a.p, 0, static_cast<size_t>(a.n));
        return kNvOk;
    case 0x1a:   // ALLOC_GPFIFO_EX2 {u32 entries, jobs, flags; NvFence {u32 id at 12, u32 value at 16}; reserved}
        if (!f.syncpt) f.syncpt = s.next_syncpt++;
        a.put32(12, f.syncpt);
        a.put32(16, 0);
        return kNvOk;
    default:
        handled = false;
        return kNvOk;
    }
}

// Dispatch on the device, then on the command word's type and number. xout is the inline output of Ioctl3 (empty otherwise).
uint32_t dispatch(NvFile& f, uint32_t cmd, Arg a, Arg xout, bool& handled) {
    const uint32_t type = (cmd >> 8) & 0xFF, nr = cmd & 0xFF;
    handled = true;
    switch (f.dev) {
    case Dev::NvMap:
        if (type == 0x01) return nvmap_ioctl(nr, a, handled);
        break;
    case Dev::Ctrl:
        if (type == 0x00) return ctrl_ioctl(nr, a, handled);
        break;
    case Dev::CtrlGpu:
        if (type == 0x47) return ctrlgpu_ioctl(nr, a, xout, handled);
        break;
    case Dev::AsGpu:
        if (type == 0x41) return as_ioctl(f, nr, a, handled);
        break;
    case Dev::Gpu:
        if (type == 0x48) return gpu_ioctl(f, nr, a, handled);
        if (type == 0x47 && nr == 0x14) {   // SET_USER_DATA {u64}
            f.user_data = a.u64(0);
            return kNvOk;
        }
        break;
    default: break;
    }
    handled = false;
    return kNvOk;
}

// Ioctl, Ioctl2 and Ioctl3 share this: the command's direction and size decide what is copied in and out (as Ryujinx's
// GetIoctlArgument checks it). xout is Ioctl3's inline output buffer, read and written in place; Ioctl2's inline input is unused.
uint32_t ioctl_call(uint32_t fd, uint32_t cmd, const ipc::Buffer& in, const ipc::Buffer& out, const ipc::Buffer& xout) {
    NvState& s = nv();
    std::lock_guard<std::mutex> g(s.mu);
    if (!s.initialized) return kNvNotInitialized;
    if (static_cast<int32_t>(fd) < 0) return kNvInvalidParameter;
    auto it = s.files.find(fd);
    if (it == s.files.end()) return kNvNotImplemented;
    NvFile& f = *it->second;

    const uint32_t dir = cmd >> 30, size = (cmd >> 16) & 0x3FFF;
    const bool rd = dir & 1, wr = dir & 2;
    const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(f.dev)) << 32) | cmd;
    log_ioctl(f, cmd, size, dir, s.seen.insert(key).second);
    if ((wr && size > out.size) || (rd && size > in.size)) {
        NXLOG_W(kTagNv, "ioctl size inconsistency on cmd %08x", cmd);
        return kNvInvalidSize;
    }
    std::vector<uint8_t> v(size, 0);
    if (rd) buf_read(in, v.data(), size);
    bool handled = false;
    uint32_t err = dispatch(f, cmd, Arg{v.data(), size}, arg_of(xout), handled);
    if (!handled) {
        if (s.warned.insert(key).second)
            NXLOG_W(kTagNv, "UNHANDLED IOCTL on %s: cmd %08x (type %02x nr %02x size %u), answered with zeros", dev_name(f.dev), cmd,
                    (cmd >> 8) & 0xFF, cmd & 0xFF, size);
        std::fill(v.begin(), v.end(), 0);
        err = kNvOk;
    }
    if (wr) buf_write(out, v.data(), size);
    return err;
}

// Open(buffer path) -> (i32 fd, u32 error)
uint32_t nv_open(ipc::Request& r, ipc::Response& rsp) {
    NvState& s = nv();
    std::lock_guard<std::mutex> g(s.mu);
    if (!s.initialized) {
        rsp.push<int32_t>(-1);
        rsp.push<uint32_t>(kNvNotInitialized);
        return 0;
    }
    const std::string path = c_string(r.in_buf(0));
    for (const DevEntry& e : kDevices) {
        if (path != e.path) continue;
        if (e.debug_only) {
            rsp.push<int32_t>(-1);
            rsp.push<uint32_t>(kNvNotSupported);
            return 0;
        }
        const uint32_t fd = s.next_fd++;
        auto file = std::make_shared<NvFile>();
        file->dev = e.dev;
        s.files[fd] = file;
        rsp.push<uint32_t>(fd);
        rsp.push<uint32_t>(kNvOk);
        return 0;
    }
    NXLOG_W(kTagNv, "cannot find device \"%s\"", path.c_str());
    rsp.push<int32_t>(-1);
    rsp.push<uint32_t>(kNvFileOperationFailed);
    return 0;
}

uint32_t nv_close(ipc::Request& r, ipc::Response& rsp) {
    NvState& s = nv();
    std::lock_guard<std::mutex> g(s.mu);
    uint32_t err = kNvOk;
    const int32_t fd = static_cast<int32_t>(r.arg<uint32_t>(0));
    if (!s.initialized) err = kNvNotInitialized;
    else if (fd < 0) err = kNvInvalidParameter;
    else {
        auto it = s.files.find(static_cast<uint32_t>(fd));
        if (it == s.files.end()) err = kNvNotImplemented;
        else s.files.erase(it);
    }
    rsp.push<uint32_t>(err);
    return 0;
}

// QueryEvent(fd, id) -> (u32 error, copy handle). The ctrl device keys its events by slot, the gpu ones by id.
uint32_t nv_query_event(uint32_t fd, uint32_t id, ipc::Response& rsp) {
    NvState& s = nv();
    std::lock_guard<std::mutex> g(s.mu);
    if (!s.initialized) return kNvNotInitialized;
    auto it = s.files.find(fd);
    if (it == s.files.end()) return kNvNotImplemented;
    NvFile& f = *it->second;
    uint32_t key = 0;
    switch (f.dev) {
    case Dev::Ctrl: {
        const uint32_t slot = (id >> 28) == 1 ? (id & 0xFFFF) : (id & 0xFF);
        if (slot >= 64) return kNvInvalidValue;
        key = slot;
        break;
    }
    case Dev::CtrlGpu:
        if (id != 1 && id != 2) return kNvInvalidValue;   // ErrorEvent, SemaphoreEvent
        key = id;
        break;
    case Dev::Gpu:
        if (id < 1 || id > 3) return kNvInvalidValue;     // BptIntEvent, BptPauseEvent, ErrorNotifierEvent
        key = id;
        break;
    default: return kNvNotSupported;
    }
    auto& ev = f.events[key];
    if (!ev) ev = std::make_shared<ServiceEvent>();
    const uint32_t h = ev->new_handle();
    if (!h) return kNvInsufficientMemory;
    rsp.out_copy.push_back(h);
    return kNvOk;
}

void setup_nvdrv(ipc::Service& s) {
    s.add_cmd(0, "Open", [](ipc::Request& r, ipc::Response& rsp) -> uint32_t { return nv_open(r, rsp); });
    s.add_cmd(1, "Ioctl", [](ipc::Request& r, ipc::Response& rsp) -> uint32_t {
        rsp.push<uint32_t>(ioctl_call(r.arg<uint32_t>(0), r.arg<uint32_t>(4), r.in_buf(0), r.out_buf(0), ipc::Buffer{}));
        return 0;
    });
    s.add_cmd(2, "Close", [](ipc::Request& r, ipc::Response& rsp) -> uint32_t { return nv_close(r, rsp); });
    s.add_cmd(3, "Initialize", [](ipc::Request& r, ipc::Response& rsp) -> uint32_t {
        // (u32 size, process and transfer memory handles): the handles are not used, as Ryujinx does
        (void)r;
        std::lock_guard<std::mutex> g(nv().mu);
        nv().initialized = true;
        rsp.push<uint32_t>(kNvOk);
        return 0;
    });
    s.add_cmd(4, "QueryEvent", [](ipc::Request& r, ipc::Response& rsp) -> uint32_t {
        rsp.push<uint32_t>(nv_query_event(r.arg<uint32_t>(0), r.arg<uint32_t>(4), rsp));
        return 0;
    });
    s.add_cmd(6, "GetStatus", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        std::lock_guard<std::mutex> g(nv().mu);
        if (!nv().initialized) {
            rsp.push<uint32_t>(kNvNotInitialized);
            return 0;
        }
        for (int i = 0; i < 8; ++i) rsp.push<uint32_t>(0);   // NvDrvStatus: four u32s and 0x10 reserved bytes, all zero, then the error
        rsp.push<uint32_t>(kNvOk);
        return 0;
    });
    s.add_cmd(7, "SetAruidWithoutCheck", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        rsp.push<uint32_t>(kNvOk);
        return 0;
    });
    s.add_cmd(8, "SetAruid", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        rsp.push<uint32_t>(kNvOk);
        return 0;
    });
    s.add_cmd(9, "DumpStatus", [](ipc::Request&, ipc::Response&) -> uint32_t {
        NXLOG_D(kTagNv, "DumpStatus");
        return 0;
    });
    s.add_cmd(11, "Ioctl2", [](ipc::Request& r, ipc::Response& rsp) -> uint32_t {
        rsp.push<uint32_t>(ioctl_call(r.arg<uint32_t>(0), r.arg<uint32_t>(4), r.in_buf(0), r.out_buf(0), ipc::Buffer{}));
        return 0;
    });
    s.add_cmd(12, "Ioctl3", [](ipc::Request& r, ipc::Response& rsp) -> uint32_t {
        rsp.push<uint32_t>(ioctl_call(r.arg<uint32_t>(0), r.arg<uint32_t>(4), r.in_buf(0), r.out_buf(0), r.out_buf(1)));
        return 0;
    });
    s.add_cmd(13, "SetGraphicsFirmwareMemoryMarginEnabled", [](ipc::Request&, ipc::Response&) -> uint32_t {
        NXLOG_D(kTagNv, "SetGraphicsFirmwareMemoryMarginEnabled accepted");
        return 0;
    });
}

}  // namespace

// ============================================================ registration

void register_vi_nv() {
    add("vi:u", [] { return make_iface("nn::visrv::sf::IApplicationRootService", setup_vi_u); });
    add("vi:s", [] { return make_iface("nn::visrv::sf::ISystemRootService", setup_vi_s); });
    add("vi:m", [] { return make_iface("nn::visrv::sf::IManagerRootService", setup_vi_m); });
    for (const char* name : {"nvdrv", "nvdrv:a", "nvdrv:s", "nvdrv:t"})
        add(name, [] { return make_iface("nns::nvdrv::INvDrvServices", setup_nvdrv); });
}

void vi_nv_reset() {
    vsync_stop_and_join();
    {
        ViState& s = vi();
        std::lock_guard<std::mutex> g(s.mu);
        for (Layer& l : s.layers) {
            l.used = false;
            l.connected = false;
            l.layer_id = 0;
            l.event = ServiceEvent();
        }
        s.next_stray = kStrayLayerBase;
        s.vsync = ServiceEvent();
    }
    {
        NvState& n = nv();
        std::lock_guard<std::mutex> g(n.mu);
        n.initialized = false;
        n.next_fd = 1;
        n.next_handle = 1;
        n.next_syncpt = 1;
        n.files.clear();
        n.maps.clear();
        n.seen.clear();
        n.warned.clear();
        n.submit_logged = false;
    }
}

namespace viapi {
uint32_t display_vsync_handle() { return vsync_new_handle(); }
}  // namespace viapi

}  // namespace nxrt::services
