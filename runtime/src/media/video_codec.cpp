// SPDX-License-Identifier: GPL-3.0-or-later
// nn::codec hardware video decoders (VP9, VP8, AV1) + their fences on the host, decoding with libvpx and dav1d (AV1: the game's
// movies). See include/nxrt/video_codec.h. The AV1 calls take the VP9 shapes except: GetAv1DecoderSubmissionReadyEvent(dec) returns the event,
// InitializeAv1DecoderByRemoteMemory has no extra size_t, and UnregisterAv1DecoderBuffer(dec, index) exists.
//
// Calling conventions, read off title A's subsdk0 (movie::detail::*, which forwards its registers unchanged to nn::codec) and off the
// mangled names (title C's variants differ where noted):
//   MakeDecodeImagePreference(u16 w, u16 h, OutputFormat, CodecMode) -> 64-byte VideoDecoderImagePreference returned through x8
//   CalculateDecoderImagePoolBuffer(size_t* size, size_t* align, const Preference*, u32 count)
//   InitializeDecoderImagePool(Pool*, void* mem, size_t, const Preference*, u32 count, CodecMode)    Pool = 32 bytes
//   ReadDecoderOutputImageNv12(const Pool*, u32 image, u16 x, u16 y, u16 w, u16 h, void* y, size_t y_stride, [sp] size_t y_size,
//                              [sp+8] void* uv, [sp+16] size_t uv_stride, [sp+24] size_t uv_size)
//   ReadDecoderOutputImageRgba(const Pool*, u32 image, u16 x, u16 y, u16 w, u16 h, void* dst, size_t stride, [sp] size_t size)
//   CalculateVp9DecoderWorkBuffer(size_t* size, size_t* align, size_t, u16 w, u16 h)
//   InitializeVp9Decoder(Vp9DecoderType*, void* work, size_t, size_t, u16, u16, u32, InputFormat, [sp] OutputFormat)   (transfer memory)
//   InitializeVp9Decoder(Vp9DecoderType*, size_t, u16, u16, u32, InputFormat, OutputFormat)   (remote memory; title C: an extra size_t)
//   RegisterVp9DecoderBuffer(Vp9DecoderType*, u32 index, Pool*, u32)
//   GetVp9DecoderSubmissionReadyEvent(os::SystemEventType** out, Vp9DecoderType*)
//   SubmitVp9DecoderInputBuffer(u64* out, Vp9DecoderType*, const void* data, size_t size, TimeSpan pts, bool end_of_stream)
//   AcquireVp9DecoderOutputBuffer(u32* index, gs::NativeSyncHandle* fence, Vp9DecoderType*)
//   ReleaseVp9DecoderOutputBuffer(Vp9DecoderType*, u32 index, gs::NativeSyncHandle pre_fence (x2, x3))
//   GetVp9DecoderOutputBufferProperties(u16* w, u16* h, u16*, u16*, TimeSpan* pts, bool* eos, Vp9DecoderType*, u32 index)
//     (title C: a movie::ColorSpace* after the four u16*, so the decoder is x7 and the index on the stack)
//   nn::syncpt::sdk::{Create,Destroy,Cancel}SyncPointEvent(SyncPointEventType*), RequestSyncPointEvent(evt, const SyncPointHandle&),
//     InitializeMultiWaitHolder(os::MultiWaitHolderType*, evt), TryWaitSyncPointEvent(const evt*); nn::gs::TryWaitNativeSync(const handle&)
// Every nn::codec call the player makes is checked (a non-zero nn::Result aborts), so these answer 0 unless noted.
//
// Guest objects keep {host pointer, marker} in their first 16 bytes. A SyncPointEventType (128 bytes in the player's event manager) holds a
// guest os::EventType at +0 (40 bytes) that the guest's own MultiWait waits on; a fence handle is {kSyncMarker | decoder id, sequence}.
// Fences of a decoder complete in sequence order, so "done" is sequence <= the decoder's last completed sequence; any handle that is not
// one of ours counts as reached (nxrt has no GPU syncpoints: NVN runs on the host).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "nxrt/abi.h"
#include "nxrt/config.h"
#include "nxrt/dispatch.h"
#include "nxrt/game.h"
#include "nxrt/log.h"
#include "nxrt/module.h"
#include "nxrt/video_codec.h"

#if NXRT_HAVE_LIBVPX
#include <vpx/vp8dx.h>
#include <vpx/vpx_decoder.h>
#endif
#if NXRT_HAVE_LIBVPX && NXRT_HAVE_DAV1D
#include <dav1d/dav1d.h>
#endif

namespace nxrt::media {

#if NXRT_HAVE_LIBVPX
namespace {

#define A(i) (c->x[i])
#define RET(v) (c->x[0] = static_cast<uint64_t>(v))

constexpr uint64_t kDecMarker = 0x4e58524356444543ull;    // "NXRCVDEC"
constexpr uint64_t kPoolMarker = 0x4e585243504f4f4cull;   // "NXRCPOOL"
constexpr uint64_t kPrefMarker = 0x4e58524350524546ull;   // "NXRCPREF"
constexpr uint64_t kEvtMarker = 0x4e58524353594e43ull;    // "NXRCSYNC" at +0x60 of a SyncPointEventType
constexpr uint64_t kSyncMarker = 0x4e58435600000000ull;   // "NXCV" in the high half of a fence handle's first word
constexpr uint32_t kManualClear = 0;                      // nn::os::EventClearMode_ManualClear
constexpr size_t kBacklog = 3;                            // decoded pictures waiting for a buffer before input stops being "ready"
constexpr size_t kPacketBacklog = 32;                     // packets not decoded yet (decoding waits for an output buffer) before the same

bool g_log = false;
uint64_t g_max_pictures = 0;   // [media] video_max_pictures (tests): a stream ends after this many pictures (0 = never)
int g_threads = 4;
size_t g_ahead = 8;           // [media] video_decode_ahead: pictures a decoder's own thread decodes ahead of the game (0 = decode on demand)
int g_frame_delay = 0;         // [media] video_frame_delay: dav1d's max_frame_delay (0 = dav1d's choice from the thread count)
std::atomic<bool> g_enabled{false};
std::atomic<uint64_t> s_created{0}, s_destroyed{0}, s_packets{0}, s_decoded{0}, s_delivered{0}, s_read{0}, s_errors{0};
std::mutex g_err_mu;
std::string g_first_error;   // the first decode error, kept for the stats line

// Records an error: the first one is logged at Error level and kept, later ones only counted (the stats line shows the total).
void note_error(const char* fmt, ...) {
    s_errors++;
    char buf[320];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> lk(g_err_mu);
    if (!g_first_error.empty()) return;
    g_first_error = buf;
    NXLOG_E("vcodec", "first error: %s", buf);
}

// Decode stats, at most every 5 s and only when something moved: packets in, pictures out of libvpx, pictures placed in the player's
// output buffers (fences signalled), pictures the game read back (ReadDecoderOutputImage*), errors.
void stats_tick() {
    static std::atomic<int64_t> last_ns{0};
    static std::atomic<uint64_t> last_sum{0};
    const int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    int64_t prev = last_ns.load(std::memory_order_relaxed);
    if (prev && now - prev < 5'000'000'000ll) return;
    const uint64_t sum = s_packets + s_decoded + s_delivered + s_read + s_errors;
    if (sum == last_sum.load(std::memory_order_relaxed)) return;
    if (!last_ns.compare_exchange_strong(prev, now)) return;
    last_sum = sum;
    std::string first;
    {
        std::lock_guard<std::mutex> lk(g_err_mu);
        first = g_first_error;
    }
    NXLOG_I("vcodec", "stats: decoders %llu/%llu live, packets %llu, decoded %llu, placed in output buffers %llu, read by the game %llu, errors %llu%s%s",
            (unsigned long long)(s_created - s_destroyed), (unsigned long long)s_created.load(), (unsigned long long)s_packets.load(),
            (unsigned long long)s_decoded.load(), (unsigned long long)s_delivered.load(), (unsigned long long)s_read.load(),
            (unsigned long long)s_errors.load(), first.empty() ? "" : " (first: ", first.empty() ? "" : (first + ")").c_str());
}

inline uint64_t rd64(uint64_t a) { uint64_t v; std::memcpy(&v, reinterpret_cast<const void*>(a), 8); return v; }
inline void wr64(uint64_t a, uint64_t v) { std::memcpy(reinterpret_cast<void*>(a), &v, 8); }
inline void wr32(uint64_t a, uint32_t v) { std::memcpy(reinterpret_cast<void*>(a), &v, 4); }
inline void wr16(uint64_t a, uint16_t v) { std::memcpy(reinterpret_cast<void*>(a), &v, 2); }

// ---- guest calls ------------------------------------------------------------------------------------------------------------------
// Calls a guest function from inside a handler (on the guest thread that called the import); returns x0.
uint64_t gcall(::Ctx* c, uint64_t fn, uint64_t a0, uint64_t a1 = 0, uint64_t a2 = 0, uint64_t a3 = 0) {
    if (!fn) return 0;
    uint64_t save[9];
    std::memcpy(save, c->x, sizeof save);
    const uint64_t lr = c->x[30], f = c->fn;
    c->x[0] = a0; c->x[1] = a1; c->x[2] = a2; c->x[3] = a3;
    dispatch_call(c, fn);
    const uint64_t r = c->x[0];
    std::memcpy(c->x, save, sizeof save);
    c->x[30] = lr;
    c->fn = f;
    return r;
}
struct OsFns {
    uint64_t init_event, signal_event, clear_event, finalize_event, trywait_event, init_holder_event;
    uint64_t create_sysevent, signal_sysevent, destroy_sysevent;
};
const OsFns& os() {
    static const OsFns f = [] {
        OsFns o{};
        o.init_event = module_find_symbol("_ZN2nn2os15InitializeEventEPNS0_9EventTypeEbNS0_14EventClearModeE");
        o.signal_event = module_find_symbol("_ZN2nn2os11SignalEventEPNS0_9EventTypeE");
        o.clear_event = module_find_symbol("_ZN2nn2os10ClearEventEPNS0_9EventTypeE");
        o.finalize_event = module_find_symbol("_ZN2nn2os13FinalizeEventEPNS0_9EventTypeE");
        o.trywait_event = module_find_symbol("_ZN2nn2os12TryWaitEventEPNS0_9EventTypeE");
        o.init_holder_event = module_find_symbol("_ZN2nn2os25InitializeMultiWaitHolderEPNS0_19MultiWaitHolderTypeEPNS0_9EventTypeE");
        o.create_sysevent = module_find_symbol("_ZN2nn2os17CreateSystemEventEPNS0_15SystemEventTypeENS0_14EventClearModeEb");
        o.signal_sysevent = module_find_symbol("_ZN2nn2os17SignalSystemEventEPNS0_15SystemEventTypeE");
        o.destroy_sysevent = module_find_symbol("_ZN2nn2os18DestroySystemEventEPNS0_15SystemEventTypeE");
        if (!o.init_event || !o.signal_event || !o.clear_event || !o.init_holder_event || !o.create_sysevent || !o.signal_sysevent)
            NXLOG_E("vcodec", "guest nn::os event functions missing: fences and input events will not work");
        return o;
    }();
    return f;
}

// ---- image pools --------------------------------------------------------------------------------------------------------------------
struct Pool {
    uint16_t w = 0, h = 0;      // the image size the pool was made for (even)
    uint32_t format = 0;        // nn::codec VideoDecoderOutputFormat (2 RGBA, 3/6 NV12 as the player reads it)
    std::vector<uint8_t> nv12;  // the picture last placed in it, w*h luma + w*h/2 interleaved chroma
};
Pool* pool_of(uint64_t g) { return g && rd64(g + 8) == kPoolMarker ? reinterpret_cast<Pool*>(rd64(g)) : nullptr; }

// ---- decoders -----------------------------------------------------------------------------------------------------------------------
struct Picture {
    std::vector<uint8_t> nv12;
    uint16_t w = 0, h = 0;
    int64_t pts = 0;
    bool eos = false;
};
struct BufInfo {
    uint16_t w = 0, h = 0;
    int64_t pts = 0;
    bool eos = false;
};
struct Packet {   // submitted, decoded once an output buffer waits for a picture (so decoded pictures never pile up)
    std::vector<uint8_t> data;
    int64_t pts = 0;
    bool eos = false;
};
struct Decoder {
    uint32_t id = 0;
    int codec = 9;                                        // 9 VP9, 8 VP8, 1 AV1
    std::mutex mu;
    vpx_codec_ctx_t vpx{};
#if NXRT_HAVE_DAV1D
    Dav1dContext* av1 = nullptr;
#endif
    bool vpx_ok = false;                                  // the codec library's decoder is open (libvpx or dav1d)
    std::map<uint32_t, uint64_t> pools;                   // buffer index -> guest pool
    std::map<uint32_t, BufInfo> info;                     // buffer index -> picture properties
    std::deque<uint32_t> free;                            // registered or released, not acquired
    std::deque<std::pair<uint32_t, uint64_t>> waiting;    // acquired buffers without a picture yet (index, fence sequence)
    std::deque<Picture> ready;                            // decoded pictures without a buffer yet
    std::deque<Packet> packets;                           // submitted, not decoded yet
    uint64_t next_seq = 1;
    std::atomic<uint64_t> done_seq{0};
    alignas(16) uint8_t submit_event[0x100]{};            // the guest os::SystemEventType handed out as "submission ready"
    bool submit_event_made = false;
    bool want_submit_signal = false;
    bool started = false;                                 // between Start*Decoder and Stop*Decoder: only then is input "ready"
    std::vector<uint8_t> last;                            // last decoded picture (NV12) for the end-of-stream buffer
    uint16_t last_w = 0, last_h = 0;
    uint64_t pictures = 0;
    // [media] video_decode_ahead: a host thread owns the codec and keeps up to g_ahead pictures decoded before the game asks; codec_mu
    // guards the codec library state, gen drops pictures of packets taken before a flush.
    std::mutex codec_mu;
    std::condition_variable cv;
    std::thread worker;
    bool quit = false;
    uint64_t gen = 0;
    std::deque<Picture>* sink = nullptr;                  // the worker's output while it decodes (codec_mu held)
    ~Decoder() {   // a decoder the game never finalized (process exit): let its worker go
        if (worker.joinable()) {
            { std::lock_guard<std::mutex> lk(mu); quit = true; }
            cv.notify_all();
            worker.detach();
        }
    }
};

std::mutex g_reg_mu;
std::map<uint32_t, std::shared_ptr<Decoder>> g_decoders;
uint32_t g_next_id = 1;

std::shared_ptr<Decoder> dec_of(uint64_t g) {
    if (!g || rd64(g + 8) != kDecMarker) return nullptr;
    const uint32_t id = static_cast<uint32_t>(rd64(g));
    std::lock_guard<std::mutex> lk(g_reg_mu);
    auto it = g_decoders.find(id);
    return it == g_decoders.end() ? nullptr : it->second;
}
bool fence_done(uint64_t w0, uint64_t seq) {
    if ((w0 & 0xffffffff00000000ull) != kSyncMarker) return true;   // not a decoder fence: reached
    std::lock_guard<std::mutex> lk(g_reg_mu);
    auto it = g_decoders.find(static_cast<uint32_t>(w0));
    return it == g_decoders.end() || it->second->done_seq.load() >= seq;
}

bool vpx_open(Decoder& d) {
#if NXRT_HAVE_DAV1D
    if (d.codec == 1) {
        Dav1dSettings s;
        dav1d_default_settings(&s);
        s.n_threads = g_threads;
        s.max_frame_delay = g_frame_delay;
        // max_frame_delay stays 0 (dav1d picks it from the thread count): frame threading needs several pictures in flight, and the
        // player submits packets well ahead of the pictures it reads (kPacketBacklog), so pump() always has the next ones. With 1, dav1d
        // only had tile threads and made ~21 pictures/s of the game's 1080p intro on the Core Ultra 5.
        const int e = dav1d_open(&d.av1, &s);
        d.vpx_ok = e == 0;
        if (!d.vpx_ok) note_error("decoder %u: dav1d_open failed (%d)", d.id, e);
        return d.vpx_ok;
    }
#endif
    vpx_codec_dec_cfg_t cfg{};
    cfg.threads = static_cast<unsigned>(g_threads);
    const vpx_codec_err_t e = vpx_codec_dec_init(&d.vpx, d.codec == 8 ? vpx_codec_vp8_dx() : vpx_codec_vp9_dx(), &cfg, 0);
    d.vpx_ok = e == VPX_CODEC_OK;
    if (!d.vpx_ok) note_error("decoder %u: vpx_codec_dec_init failed: %s", d.id, vpx_codec_err_to_string(e));
    return d.vpx_ok;
}
void vpx_close(Decoder& d) {
#if NXRT_HAVE_DAV1D
    if (d.codec == 1) {
        if (d.av1) dav1d_close(&d.av1);
        d.vpx_ok = false;
        return;
    }
#endif
    if (d.vpx_ok) vpx_codec_destroy(&d.vpx);
    d.vpx_ok = false;
}

// ---- sync point events ----------------------------------------------------------------------------------------------------------
struct Armed {
    uint32_t dec = 0;
    uint64_t seq = 0;
};
std::mutex g_evt_mu;
std::map<uint64_t, Armed> g_armed;   // guest SyncPointEventType -> the fence it waits for

// After a decoder completed fences: the armed events whose fence is now reached (disarmed; the caller signals them).
std::vector<uint64_t> take_reached(uint32_t dec, uint64_t done) {
    std::vector<uint64_t> out;
    std::lock_guard<std::mutex> lk(g_evt_mu);
    for (auto it = g_armed.begin(); it != g_armed.end();) {
        if (it->second.dec == dec && it->second.seq <= done) {
            out.push_back(it->first);
            it = g_armed.erase(it);
        } else {
            ++it;
        }
    }
    return out;
}

// ---- picture conversion -----------------------------------------------------------------------------------------------------------
Picture to_nv12(const vpx_image_t* img, int64_t pts) {
    Picture p;
    p.w = static_cast<uint16_t>((img->d_w + 1) & ~1u);
    p.h = static_cast<uint16_t>((img->d_h + 1) & ~1u);
    p.pts = pts;
    const size_t w = p.w, h = p.h;
    p.nv12.assign(w * h + w * h / 2, 0);
    for (size_t y = 0; y < img->d_h; y++) std::memcpy(&p.nv12[y * w], img->planes[0] + y * img->stride[0], img->d_w);
    uint8_t* uv = p.nv12.data() + w * h;
    const size_t cw = (img->d_w + 1) / 2, ch = (img->d_h + 1) / 2;
    for (size_t y = 0; y < ch; y++) {
        const uint8_t* u = img->planes[1] + y * img->stride[1];
        const uint8_t* v = img->planes[2] + y * img->stride[2];
        uint8_t* o = uv + y * w;
        for (size_t x = 0; x < cw; x++) {
            o[2 * x] = u[x];
            o[2 * x + 1] = v[x];
        }
    }
    return p;
}

#if NXRT_HAVE_DAV1D
// An 8- or 10-bit 4:2:0 dav1d picture as NV12 (10-bit is cut to 8).
bool av1_to_nv12(const Dav1dPicture& pic, Picture& p) {
    if (pic.p.layout != DAV1D_PIXEL_LAYOUT_I420 || (pic.p.bpc != 8 && pic.p.bpc != 10)) return false;
    const int dw = pic.p.w, dh = pic.p.h;
    p.w = static_cast<uint16_t>((dw + 1) & ~1);
    p.h = static_cast<uint16_t>((dh + 1) & ~1);
    p.pts = pic.m.timestamp;
    const size_t w = p.w, h = p.h;
    p.nv12.assign(w * h + w * h / 2, 0);
    const bool hi = pic.p.bpc == 10;
    auto sample = [hi](const uint8_t* row, int x) -> uint8_t {
        if (!hi) return row[x];
        uint16_t v;
        std::memcpy(&v, row + 2 * x, 2);
        return static_cast<uint8_t>(v >> 2);
    };
    for (int y = 0; y < dh; y++) {
        const uint8_t* r = static_cast<const uint8_t*>(pic.data[0]) + static_cast<ptrdiff_t>(y) * pic.stride[0];
        if (!hi) std::memcpy(&p.nv12[y * w], r, static_cast<size_t>(dw));
        else for (int x = 0; x < dw; x++) p.nv12[y * w + x] = sample(r, x);
    }
    uint8_t* uv = p.nv12.data() + w * h;
    const int cw = (dw + 1) / 2, ch = (dh + 1) / 2;
    for (int y = 0; y < ch; y++) {
        const uint8_t* u = static_cast<const uint8_t*>(pic.data[1]) + static_cast<ptrdiff_t>(y) * pic.stride[1];
        const uint8_t* v = static_cast<const uint8_t*>(pic.data[2]) + static_cast<ptrdiff_t>(y) * pic.stride[1];
        uint8_t* o = uv + y * w;
        for (int x = 0; x < cw; x++) {
            o[2 * x] = sample(u, x);
            o[2 * x + 1] = sample(v, x);
        }
    }
    return true;
}
#endif

// Copies `p` into the pool's image (at the pool's size; padding black when the picture is smaller).
void place(Pool& pool, const Picture& p) {
    const size_t pw = pool.w ? pool.w : p.w, ph = pool.h ? pool.h : p.h;
    pool.nv12.assign(pw * ph + pw * ph / 2, 0);
    std::memset(pool.nv12.data(), 16, pw * ph);
    std::memset(pool.nv12.data() + pw * ph, 128, pw * ph / 2);
    if (p.nv12.empty()) return;
    const size_t cw = std::min<size_t>(pw, p.w), chh = std::min<size_t>(ph, p.h);
    for (size_t y = 0; y < chh; y++) std::memcpy(&pool.nv12[y * pw], &p.nv12[y * p.w], cw);
    for (size_t y = 0; y < chh / 2; y++) std::memcpy(&pool.nv12[pw * ph + y * pw], &p.nv12[size_t(p.w) * p.h + y * p.w], cw);
}

// Pairs decoded pictures with acquired buffers (both first in first out). Caller holds d.mu. Returns true when a fence completed.
bool assign(Decoder& d) {
    bool any = false;
    while (!d.waiting.empty() && !d.ready.empty()) {
        const auto [index, seq] = d.waiting.front();
        d.waiting.pop_front();
        Picture p = std::move(d.ready.front());
        d.ready.pop_front();
        auto it = d.pools.find(index);
        if (Pool* pool = it == d.pools.end() ? nullptr : pool_of(it->second)) place(*pool, p);
        else note_error("decoder %u: buffer %u has no image pool", d.id, index);
        d.info[index] = BufInfo{p.w, p.h, p.pts, p.eos};
        d.done_seq.store(seq);
        any = true;
        s_delivered++;
    }
    return any;
}

// Signals what a decoder state change made due: reached fences, and "submission ready" when the backlog went down. No locks held.
void after_change(::Ctx* c, Decoder& d, bool fences) {
    stats_tick();
    if (fences)
        for (uint64_t evt : take_reached(d.id, d.done_seq.load())) gcall(c, os().signal_event, evt);
    bool signal = false;
    {
        std::lock_guard<std::mutex> lk(d.mu);
        if (d.submit_event_made && d.started && d.ready.size() < kBacklog + g_ahead && d.packets.size() < kPacketBacklog) {
            signal = d.want_submit_signal;
            d.want_submit_signal = false;
        }
    }
    if (signal) gcall(c, os().signal_sysevent, reinterpret_cast<uint64_t>(d.submit_event));
}

void keep_picture(Decoder& d, Picture&& p, unsigned w, unsigned h) {
    if (d.sink) {   // decode-ahead worker: d.mu is not held, the worker files the picture under it
        d.sink->push_back(std::move(p));
        return;
    }
    if (d.pictures++ == 0)
        NXLOG_I("vcodec", "decoder %u: first picture %ux%u (%s, %d threads, frame delay %d)", d.id, w, h, d.codec == 1 ? "AV1" : d.codec == 8 ? "VP8" : "VP9", g_threads, g_frame_delay);
    d.last = p.nv12;
    d.last_w = p.w;
    d.last_h = p.h;
    d.ready.push_back(std::move(p));
    s_decoded++;
}

#if NXRT_HAVE_DAV1D
// One packet into dav1d (null data: drain), every picture it gives back into d.ready.
void av1_decode(Decoder& d, const uint8_t* data, size_t size, int64_t pts) {
    Dav1dData dd{};
    if (data && size) {
        uint8_t* buf = dav1d_data_create(&dd, size);
        if (!buf) {
            note_error("decoder %u: dav1d_data_create(%zu) failed", d.id, size);
            return;
        }
        std::memcpy(buf, data, size);
        dd.m.timestamp = pts;
    }
    for (int round = 0; round < 64; round++) {
        if (dd.sz) {
            const int r = dav1d_send_data(d.av1, &dd);
            if (r < 0 && r != DAV1D_ERR(EAGAIN)) {
                note_error("decoder %u: dav1d_send_data(%zu bytes, pts %lld) failed (%d)", d.id, size, (long long)pts, r);
                dav1d_data_unref(&dd);
                return;
            }
        }
        for (;;) {
            Dav1dPicture pic{};
            const int g = dav1d_get_picture(d.av1, &pic);
            if (g == DAV1D_ERR(EAGAIN)) break;
            if (g < 0) {
                note_error("decoder %u: dav1d_get_picture failed (%d)", d.id, g);
                break;
            }
            Picture p;
            if (av1_to_nv12(pic, p)) keep_picture(d, std::move(p), static_cast<unsigned>(pic.p.w), static_cast<unsigned>(pic.p.h));
            else note_error("decoder %u: AV1 picture layout %d, %d bits not handled (4:2:0, 8 or 10 bits only)", d.id, static_cast<int>(pic.p.layout), pic.p.bpc);
            dav1d_picture_unref(&pic);
        }
        if (!dd.sz) break;
    }
    if (dd.sz) dav1d_data_unref(&dd);
}
#endif

void decode_into(Decoder& d, const uint8_t* data, size_t size, int64_t pts) {
    if (!d.vpx_ok && !vpx_open(d)) return;
#if NXRT_HAVE_DAV1D
    if (d.codec == 1) {
        av1_decode(d, data, size, pts);
        return;
    }
#endif
    const vpx_codec_err_t e = vpx_codec_decode(&d.vpx, data, static_cast<unsigned>(size), nullptr, 0);
    if (e != VPX_CODEC_OK)
        note_error("decoder %u: vpx_codec_decode(%zu bytes, pts %lld) failed: %s (%s)", d.id, size, (long long)pts, vpx_codec_err_to_string(e),
                   vpx_codec_error_detail(&d.vpx) ? vpx_codec_error_detail(&d.vpx) : "");
    vpx_codec_iter_t it = nullptr;
    while (const vpx_image_t* img = vpx_codec_get_frame(&d.vpx, &it)) {
        if (img->fmt != VPX_IMG_FMT_I420) {
            note_error("decoder %u: picture format 0x%x not handled (8-bit 4:2:0 only)", d.id, img->fmt);
            continue;
        }
        keep_picture(d, to_nv12(img, pts), img->d_w, img->d_h);
    }
}

// Decodes queued packets while an output buffer waits and no picture is ready, then pairs pictures with buffers. Caller holds d.mu.
// Returns true when a fence completed.
bool pump(Decoder& d) {
    if (d.worker.joinable()) {   // decode ahead: the worker decodes, the guest calls only pair pictures with buffers
        d.cv.notify_one();
        return assign(d);
    }
    while (!d.waiting.empty() && d.ready.empty() && !d.packets.empty()) {
        Packet p = std::move(d.packets.front());
        d.packets.pop_front();
        if (g_max_pictures && d.pictures >= g_max_pictures) {   // the stream ends here (tests skip the game's 5-minute intro this way)
            d.packets.clear();
            p.data.clear();
            p.eos = true;
        }
        if (!p.data.empty()) decode_into(d, p.data.data(), p.data.size(), p.pts);
        if (p.eos) {
            if (d.vpx_ok) decode_into(d, nullptr, 0, p.pts);   // drain
            Picture e;
            e.nv12 = d.last;
            e.w = d.last_w;
            e.h = d.last_h;
            e.pts = p.pts;
            e.eos = true;
            d.ready.push_back(std::move(e));
            NXLOG_I("vcodec", "decoder %u: end of stream after %llu pictures", d.id, (unsigned long long)d.pictures);
        }
    }
    return assign(d);
}

// [media] video_decode_ahead: the decoder's own thread. It takes the next packet while fewer than g_ahead pictures wait, decodes it with
// d.mu free (codec_mu held), then files its pictures under d.mu, so a picture is usually ready when the game acquires a buffer instead of
// being decoded then (the game shows the previous picture again when it is not ready in time). The guest calls pair pictures with buffers
// (assign) and signal the fences; polls of a fence pair too (poll_decoder).
void ahead_worker(Decoder* d) {
    for (;;) {
        Packet p;
        uint64_t gen = 0;
        {
            std::unique_lock<std::mutex> lk(d->mu);
            d->cv.wait(lk, [d] { return d->quit || (!d->packets.empty() && d->ready.size() < g_ahead); });
            if (d->quit) return;
            p = std::move(d->packets.front());
            d->packets.pop_front();
            if (g_max_pictures && d->pictures >= g_max_pictures) {
                d->packets.clear();
                p.data.clear();
                p.eos = true;
            }
            gen = d->gen;
        }
        std::deque<Picture> out;
        {
            std::lock_guard<std::mutex> ck(d->codec_mu);
            d->sink = &out;
            if (!p.data.empty()) decode_into(*d, p.data.data(), p.data.size(), p.pts);
            if (p.eos && d->vpx_ok) decode_into(*d, nullptr, 0, p.pts);
            d->sink = nullptr;
        }
        std::lock_guard<std::mutex> lk(d->mu);
        if (gen != d->gen) continue;   // flushed meanwhile
        for (Picture& q : out) {
            const unsigned w = q.w, h = q.h;
            keep_picture(*d, std::move(q), w, h);
        }
        if (p.eos) {
            Picture e;
            e.nv12 = d->last;
            e.w = d->last_w;
            e.h = d->last_h;
            e.pts = p.pts;
            e.eos = true;
            d->ready.push_back(std::move(e));
            NXLOG_I("vcodec", "decoder %u: end of stream after %llu pictures", d->id, (unsigned long long)d->pictures);
        }
    }
}

// A poll of a decoder fence (TryWait / RequestSyncPointEvent): pictures the worker decoded since the last guest call go to the buffers
// waiting for them now.
void poll_decoder(::Ctx* c, uint64_t w0) {
    if ((w0 & 0xffffffff00000000ull) != kSyncMarker) return;
    std::shared_ptr<Decoder> d;
    {
        std::lock_guard<std::mutex> lk(g_reg_mu);
        auto it = g_decoders.find(static_cast<uint32_t>(w0));
        if (it != g_decoders.end()) d = it->second;
    }
    if (!d || !d->worker.joinable()) return;
    bool fences;
    {
        std::lock_guard<std::mutex> lk(d->mu);
        fences = assign(*d);
    }
    if (fences) after_change(c, *d, true);
}

// ---- nn::codec handlers -----------------------------------------------------------------------------------------------------------
void make_pref(::Ctx* c) {   // MakeDecodeImagePreference(w, h, format, mode) -> Preference via x8
    const uint64_t out = c->x[8];
    std::memset(reinterpret_cast<void*>(out), 0, 64);
    wr16(out + 0, static_cast<uint16_t>(A(0)));
    wr16(out + 2, static_cast<uint16_t>(A(1)));
    wr32(out + 4, static_cast<uint32_t>(A(2)));
    wr32(out + 8, static_cast<uint32_t>(A(3)));
    wr64(out + 16, kPrefMarker);
    RET(out);
}
void calc_pool(::Ctx* c) {   // CalculateDecoderImagePoolBuffer(size*, align*, pref*, count): the pictures live on the host
    const uint32_t n = std::max<uint32_t>(1, static_cast<uint32_t>(A(3)));
    if (A(0)) wr64(A(0), 0x1000ull * n);
    if (A(1)) wr64(A(1), 0x1000);
    RET(0);
}
void init_pool(::Ctx* c) {   // InitializeDecoderImagePool(pool, mem, size, pref*, count, mode)
    const uint64_t g = A(0), pref = A(3);
    Pool* p = new Pool();
    if (pref && rd64(pref + 16) == kPrefMarker) {
        std::memcpy(&p->w, reinterpret_cast<const void*>(pref), 2);
        std::memcpy(&p->h, reinterpret_cast<const void*>(pref + 2), 2);
        std::memcpy(&p->format, reinterpret_cast<const void*>(pref + 4), 4);
    }
    p->w = static_cast<uint16_t>((p->w + 1) & ~1);
    p->h = static_cast<uint16_t>((p->h + 1) & ~1);
    wr64(g, reinterpret_cast<uint64_t>(p));
    wr64(g + 8, kPoolMarker);
    if (g_log) NXLOG_I("vcodec", "InitializeDecoderImagePool %llx: %ux%u format %u", (unsigned long long)g, p->w, p->h, p->format);
    RET(0);
}
void fini_pool(::Ctx* c) {
    if (Pool* p = pool_of(A(0))) delete p;
    if (A(0)) { wr64(A(0), 0); wr64(A(0) + 8, 0); }
    RET(0);
}
void read_nv12(::Ctx* c) {
    Pool* p = pool_of(A(0));
    const uint64_t sp = c->x[31];
    const uint32_t x0 = static_cast<uint16_t>(A(2)), y0 = static_cast<uint16_t>(A(3));
    const uint32_t w = static_cast<uint16_t>(A(4)), h = static_cast<uint16_t>(A(5));
    uint8_t* ydst = reinterpret_cast<uint8_t*>(A(6));
    const size_t ystride = A(7), ysize = rd64(sp);
    uint8_t* uvdst = reinterpret_cast<uint8_t*>(rd64(sp + 8));
    const size_t uvstride = rd64(sp + 16), uvsize = rd64(sp + 24);
    s_read++;
    stats_tick();
    if (!p || p->nv12.empty() || !ydst) { RET(0); return; }
    const size_t pw = p->w, ph = p->h;
    const size_t cw = x0 < pw ? std::min<size_t>(w, pw - x0) : 0;
    for (size_t y = 0; y < h && y0 + y < ph; y++) {
        if ((y + 1) * ystride > ysize && y * ystride + cw > ysize) break;
        std::memcpy(ydst + y * ystride, &p->nv12[(y0 + y) * pw + x0], cw);
    }
    if (uvdst)
        for (size_t y = 0; y < h / 2 && y0 / 2 + y < ph / 2; y++) {
            if (y * uvstride + cw > uvsize) break;
            std::memcpy(uvdst + y * uvstride, &p->nv12[pw * ph + (y0 / 2 + y) * pw + (x0 & ~1u)], cw);
        }
    RET(0);
}
inline uint8_t clamp8(int v) { return static_cast<uint8_t>(v < 0 ? 0 : v > 255 ? 255 : v); }
void read_rgba(::Ctx* c) {   // BT.709 limited range
    Pool* p = pool_of(A(0));
    const uint32_t x0 = static_cast<uint16_t>(A(2)), y0 = static_cast<uint16_t>(A(3));
    const uint32_t w = static_cast<uint16_t>(A(4)), h = static_cast<uint16_t>(A(5));
    uint8_t* dst = reinterpret_cast<uint8_t*>(A(6));
    const size_t stride = A(7), size = rd64(c->x[31]);
    s_read++;
    if (!p || p->nv12.empty() || !dst) { RET(0); return; }
    const size_t pw = p->w, ph = p->h;
    for (size_t y = 0; y < h && y0 + y < ph; y++) {
        if ((y + 1) * stride > size) break;
        uint8_t* o = dst + y * stride;
        const uint8_t* yr = &p->nv12[(y0 + y) * pw];
        const uint8_t* uvr = &p->nv12[pw * ph + ((y0 + y) / 2) * pw];
        for (size_t x = 0; x < w && x0 + x < pw; x++) {
            const size_t sx = x0 + x;
            const int Y = (yr[sx] - 16) * 298, U = uvr[sx & ~size_t(1)] - 128, V = uvr[(sx & ~size_t(1)) + 1] - 128;
            o[4 * x + 0] = clamp8((Y + 459 * V + 128) >> 8);
            o[4 * x + 1] = clamp8((Y - 55 * U - 136 * V + 128) >> 8);
            o[4 * x + 2] = clamp8((Y + 541 * U + 128) >> 8);
            o[4 * x + 3] = 255;
        }
    }
    RET(0);
}
void native_image_handle(::Ctx* c) {
    static bool once = false;
    if (!once) {
        once = true;
        NXLOG_W("vcodec", "GetDecoderImagePoolNativeImageHandle: GPU (native texture) output is not implemented; only CPU reads work");
    }
    RET(0);
}

void calc_work(::Ctx* c) {   // Calculate*DecoderWorkBuffer(size*, align*, ...)
    if (A(0)) wr64(A(0), 0x1000);
    if (A(1)) wr64(A(1), 0x1000);
    RET(0);
}
void create_decoder(::Ctx* c, int codec, uint32_t w, uint32_t h) {
    const uint64_t g = A(0);
    auto d = std::make_shared<Decoder>();
    d->codec = codec;
    {
        std::lock_guard<std::mutex> lk(g_reg_mu);
        d->id = g_next_id++;
        g_decoders[d->id] = d;
    }
    vpx_open(*d);
    if (g_ahead) d->worker = std::thread(ahead_worker, d.get());
    wr64(g, d->id);
    wr64(g + 8, kDecMarker);
    s_created++;
#if NXRT_HAVE_DAV1D
    if (codec == 1) NXLOG_I("vcodec", "AV1 decoder %u created (%ux%u max) on dav1d %s", d->id, w, h, dav1d_version());
    else
#endif
    NXLOG_I("vcodec", "VP%d decoder %u created (%ux%u max) on libvpx %s", codec, d->id, w, h, vpx_codec_version_str());
    RET(0);
}
void init_vp9_tm(::Ctx* c) { create_decoder(c, 9, static_cast<uint16_t>(A(4)), static_cast<uint16_t>(A(5))); }
void init_vp9_rm(::Ctx* c) { create_decoder(c, 9, static_cast<uint16_t>(A(2)), static_cast<uint16_t>(A(3))); }
void init_vp9_rm2(::Ctx* c) { create_decoder(c, 9, static_cast<uint16_t>(A(3)), static_cast<uint16_t>(A(4))); }
void init_vp8_tm(::Ctx* c) { create_decoder(c, 8, static_cast<uint16_t>(A(4)), static_cast<uint16_t>(A(5))); }
void init_vp8_rm(::Ctx* c) { create_decoder(c, 8, static_cast<uint16_t>(A(2)), static_cast<uint16_t>(A(3))); }
void init_vp8_rm2(::Ctx* c) { create_decoder(c, 8, static_cast<uint16_t>(A(3)), static_cast<uint16_t>(A(4))); }
void init_av1_tm(::Ctx* c) { create_decoder(c, 1, static_cast<uint16_t>(A(4)), static_cast<uint16_t>(A(5))); }
void init_av1_rm(::Ctx* c) { create_decoder(c, 1, static_cast<uint16_t>(A(2)), static_cast<uint16_t>(A(3))); }

void finalize_decoder(::Ctx* c) {
    const uint64_t g = A(0);
    auto d = dec_of(g);
    if (d) {
        {
            std::lock_guard<std::mutex> lk(g_reg_mu);
            g_decoders.erase(d->id);
        }
        // fences of a finalized decoder count as reached: wake whoever still waits
        for (uint64_t evt : take_reached(d->id, ~0ull)) gcall(c, os().signal_event, evt);
        if (d->submit_event_made && os().destroy_sysevent) gcall(c, os().destroy_sysevent, reinterpret_cast<uint64_t>(d->submit_event));
        if (d->worker.joinable()) {
            {
                std::lock_guard<std::mutex> lk(d->mu);
                d->quit = true;
            }
            d->cv.notify_all();
            d->worker.join();
        }
        {
            std::lock_guard<std::mutex> lk(d->mu);
            vpx_close(*d);
        }
        NXLOG_I("vcodec", "decoder %u finalized after %llu pictures", d->id, (unsigned long long)d->pictures);
        s_destroyed++;
        wr64(g, 0);
        wr64(g + 8, 0);
    }
    RET(0);
}
void register_buffer(::Ctx* c) {   // (dec, index, pool, u32)
    auto d = dec_of(A(0));
    if (d) {
        std::lock_guard<std::mutex> lk(d->mu);
        const uint32_t index = static_cast<uint32_t>(A(1));
        d->pools[index] = A(2);
        d->free.push_back(index);
        if (g_log) NXLOG_I("vcodec", "decoder %u: buffer %u registered (pool %llx)", d->id, index, (unsigned long long)A(2));
    }
    RET(0);
}
// Start*Decoder: from here on input is accepted, so the submission-ready event is signalled. It must not be signalled before: subsdk0's
// decoder adapter thread wakes on it, forwards "input available" to the player only while the adapter is started, and clears the event
// either way, so a signal that comes before Start is lost and the player never sends its first packet.
void start_decoder(::Ctx* c) {
    auto d = dec_of(A(0));
    if (d) {
        {
            std::lock_guard<std::mutex> lk(d->mu);
            d->started = true;
            d->want_submit_signal = true;
        }
        NXLOG_I("vcodec", "decoder %u started", d->id);
        after_change(c, *d, false);
    }
    RET(0);
}
void stop_decoder(::Ctx* c) {
    if (auto d = dec_of(A(0))) {
        std::lock_guard<std::mutex> lk(d->mu);
        d->started = false;
    }
    RET(0);
}
void flush_decoder(::Ctx* c) {
    auto d = dec_of(A(0));
    if (!d) { RET(0); return; }
    {
        std::lock_guard<std::mutex> lk(d->mu);
        // the buffers waiting for a picture complete empty, decoded pictures and the decoder's references are dropped
        for (const auto& [index, seq] : d->waiting) d->info[index] = BufInfo{d->last_w, d->last_h, 0, false};
        if (!d->waiting.empty()) d->done_seq.store(d->waiting.back().second);
        d->waiting.clear();
        d->ready.clear();
        d->packets.clear();
        d->gen++;
        {
            std::lock_guard<std::mutex> ck(d->codec_mu);   // a picture the worker is decoding now is dropped (gen)
            vpx_close(*d);
        }
        d->want_submit_signal = true;
    }
    if (g_log) NXLOG_I("vcodec", "decoder %u flushed", d->id);
    after_change(c, *d, true);
    RET(0);
}
// The decoder's "submission ready" system event, made on first request.
uint64_t submit_event_of(::Ctx* c, const std::shared_ptr<Decoder>& d) {
    bool make = false;
    {
        std::lock_guard<std::mutex> lk(d->mu);
        make = !d->submit_event_made;
        d->submit_event_made = true;
    }
    const uint64_t ev = reinterpret_cast<uint64_t>(d->submit_event);
    if (make) {
        gcall(c, os().create_sysevent, ev, kManualClear, 0);
        bool started;
        {
            std::lock_guard<std::mutex> lk(d->mu);
            started = d->started;
        }
        if (started) gcall(c, os().signal_sysevent, ev);   // asked for after Start: input is accepted now (else Start signals it)
    }
    return ev;
}
void submission_event(::Ctx* c) {   // (SystemEventType** out, dec)
    auto d = dec_of(A(1));
    const uint64_t ev = d ? submit_event_of(c, d) : 0;
    if (A(0)) wr64(A(0), ev);
    RET(0);
}
void submission_event_av1(::Ctx* c) {   // (dec) -> SystemEventType*
    auto d = dec_of(A(0));
    RET(d ? submit_event_of(c, d) : 0);
}
void unregister_buffer(::Ctx* c) {   // (dec, index)
    if (auto d = dec_of(A(0))) {
        std::lock_guard<std::mutex> lk(d->mu);
        const uint32_t index = static_cast<uint32_t>(A(1));
        d->pools.erase(index);
        d->free.erase(std::remove(d->free.begin(), d->free.end(), index), d->free.end());
    }
    RET(0);
}
void submit(::Ctx* c) {   // (u64* out, dec, data, size, pts, eos)
    auto d = dec_of(A(1));
    const uint8_t* data = reinterpret_cast<const uint8_t*>(A(2));
    const size_t size = A(3);
    const int64_t pts = static_cast<int64_t>(A(4));
    const bool eos = (A(5) & 1) != 0;
    if (A(0)) wr64(A(0), 0);
    if (!d) { RET(0); return; }
    s_packets++;
    bool fences = false;
    {
        std::lock_guard<std::mutex> lk(d->mu);
        Packet p;
        if (data && size) p.data.assign(data, data + size);
        p.pts = pts;
        p.eos = eos;
        d->packets.push_back(std::move(p));
        fences = pump(*d);
        d->want_submit_signal = true;
    }
    static std::atomic<uint32_t> first_submits{0};
    if (g_log || first_submits.fetch_add(1) < 3)
        NXLOG_I("vcodec", "decoder %u: submit %zu bytes pts %lld%s (%llu pictures so far)", d->id, size, (long long)pts, eos ? " eos" : "",
                (unsigned long long)d->pictures);
    after_change(c, *d, fences);
    RET(0);
}
void acquire_output(::Ctx* c) {   // (u32* index, NativeSyncHandle* fence, dec)
    auto d = dec_of(A(2));
    uint32_t index = 0;
    uint64_t seq = 0;
    bool fences = false;
    if (d) {
        std::lock_guard<std::mutex> lk(d->mu);
        if (d->free.empty()) {
            note_error("decoder %u: AcquireOutputBuffer with no free buffer", d->id);
        } else {
            index = d->free.front();
            d->free.pop_front();
            seq = d->next_seq++;
            d->waiting.emplace_back(index, seq);
            fences = pump(*d);
        }
    }
    if (A(0)) wr32(A(0), index);
    if (A(1)) {
        wr64(A(1), d && seq ? (kSyncMarker | d->id) : 0);
        wr64(A(1) + 8, seq);
    }
    if (d) after_change(c, *d, fences);
    RET(0);
}
void release_output(::Ctx* c) {   // (dec, index, pre-fence)
    if (auto d = dec_of(A(0))) {
        std::lock_guard<std::mutex> lk(d->mu);
        d->free.push_back(static_cast<uint32_t>(A(1)));
    }
    RET(0);
}
void props_write(uint64_t pw, uint64_t ph, uint64_t pa, uint64_t pb, uint64_t ppts, uint64_t peos, const BufInfo& b) {
    if (pw) wr16(pw, b.w);
    if (ph) wr16(ph, b.h);
    if (pa) wr16(pa, b.w);
    if (pb) wr16(pb, b.h);
    if (ppts) wr64(ppts, static_cast<uint64_t>(b.pts));
    if (peos) *reinterpret_cast<uint8_t*>(peos) = b.eos ? 1 : 0;
}
BufInfo props_of(uint64_t dec, uint32_t index) {
    BufInfo b;
    if (auto d = dec_of(dec)) {
        std::lock_guard<std::mutex> lk(d->mu);
        auto it = d->info.find(index);
        if (it != d->info.end()) b = it->second;
    }
    return b;
}
void props(::Ctx* c) {   // (u16*, u16*, u16*, u16*, TimeSpan*, bool*, dec, index)
    props_write(A(0), A(1), A(2), A(3), A(4), A(5), props_of(A(6), static_cast<uint32_t>(A(7))));
    RET(0);
}
void props_cs(::Ctx* c) {   // title C: (u16*, u16*, u16*, u16*, ColorSpace*, TimeSpan*, bool*, dec, [sp] index)
    if (A(4)) wr32(A(4), 0);
    props_write(A(0), A(1), A(2), A(3), A(5), A(6), props_of(A(7), static_cast<uint32_t>(rd64(c->x[31]))));
    RET(0);
}
void unsupported_codec(::Ctx* c) {
    static bool once = false;
    if (!once) {
        once = true;
        NXLOG_E("vcodec", "%s: H.264 / AV1 hardware decoding is not implemented on the host", c->in_import ? c->in_import : "nn::codec");
    }
    RET(0x1a6f);   // a stand-in non-zero nn::Result (codec module)
}

// ---- nn::syncpt::sdk / nn::gs ---------------------------------------------------------------------------------------------------
void sp_create(::Ctx* c) {
    const uint64_t evt = A(0);
    std::memset(reinterpret_cast<void*>(evt), 0, 0x60);
    gcall(c, os().init_event, evt, 0, kManualClear);
    wr64(evt + 0x60, kEvtMarker);
    RET(0);
}
void sp_destroy(::Ctx* c) {
    const uint64_t evt = A(0);
    {
        std::lock_guard<std::mutex> lk(g_evt_mu);
        g_armed.erase(evt);
    }
    if (rd64(evt + 0x60) == kEvtMarker) {
        gcall(c, os().finalize_event, evt);
        wr64(evt + 0x60, 0);
    }
    RET(0);
}
void sp_cancel(::Ctx* c) {
    const uint64_t evt = A(0);
    {
        std::lock_guard<std::mutex> lk(g_evt_mu);
        g_armed.erase(evt);
    }
    gcall(c, os().clear_event, evt);
    RET(0);
}
void sp_request(::Ctx* c) {   // (evt, const SyncPointHandle&)
    const uint64_t evt = A(0), h = A(1);
    const uint64_t w0 = h ? rd64(h) : 0, seq = h ? rd64(h + 8) : 0;
    poll_decoder(c, w0);
    gcall(c, os().clear_event, evt);
    bool reached;
    {
        std::lock_guard<std::mutex> lk(g_evt_mu);   // held across the check and the arm (see take_reached's caller)
        reached = fence_done(w0, seq);
        if (!reached) g_armed[evt] = Armed{static_cast<uint32_t>(w0), seq};
        else g_armed.erase(evt);
    }
    if (reached) gcall(c, os().signal_event, evt);
    RET(0);
}
void sp_holder(::Ctx* c) {   // (MultiWaitHolderType*, evt)
    gcall(c, os().init_holder_event, A(0), A(1));
    RET(0);
}
void sp_trywait(::Ctx* c) { RET(gcall(c, os().trywait_event, A(0)) & 1); }
void gs_trywait(::Ctx* c) {   // (const NativeSyncHandle&) -> bool
    const uint64_t h = A(0);
    if (h) poll_decoder(c, rd64(h));
    RET(h ? fence_done(rd64(h), rd64(h + 8)) : 1);
}

// ---- nn::movie extractor trace ([media] movie_trace) ------------------------------------------------------------------------------
// title A's movie::Extractor (subsdk0) runs the SDK's nn::movie::MediaExtractor in media source mode: its ExtractorSourceModeThread
// reads the file through the game's ClientStreamReader (8 KiB at a time) and appends it with AppendMediaSourceData; the extractor
// (nandroid MatroskaExtractor for WebM) parses from that. movie::Extractor::SetDataSource then checks the track count and the
// "container-mime" of track 0 against the ContainerType (3 = WebM) and answers 0x4000 when they disagree, which the game's player
// treats as "cannot open". These wrappers log what the SDK answered on that path (rate-limited) and then return its answer unchanged.
uint64_t call_original(::Ctx* c, uint64_t fn) {
    const uint64_t lr = c->x[30], f = c->fn;
    dispatch_call(c, fn);
    c->x[30] = lr;
    c->fn = f;
    return c->x[0];
}
struct Traced {
    const char* name;
    std::atomic<uint64_t> orig{0};
    std::atomic<uint32_t> calls{0};
};
Traced t_init{"_ZN2nn5movie14MediaExtractor10InitializeEPvmm"};
Traced t_msm{"_ZN2nn5movie14MediaExtractor20SetUpMediaSourceModeEv"};
Traced t_size{"_ZN2nn5movie6detail18ExtractorUnwrapper19StreamSourceSetSizeEl"};
Traced t_append{"_ZN2nn5movie14MediaExtractor21AppendMediaSourceDataEPKvm"};
Traced t_seekpos{"_ZN2nn5movie14MediaExtractor30GetMediaSourceModeSeekPositionEPl"};
Traced t_count{"_ZN2nn5movie14MediaExtractor13GetTrackCountEPi"};
Traced t_props{"_ZN2nn5movie14MediaExtractor18GetTrackPropertiesEPNS0_13MediaPropertyEi"};
Traced t_getstr{"_ZNK2nn5movie13MediaProperty9GetStringEPciPKc"};
std::atomic<uint64_t> t_appended{0};

// Calls the SDK's own function; the count of calls so far comes back in *n.
uint64_t traced(::Ctx* c, Traced& t, uint32_t* n) {
    uint64_t fn = t.orig.load(std::memory_order_relaxed);
    if (!fn) {
        fn = module_find_symbol(t.name);   // the defining module's address (the override only redirects the imports)
        t.orig = fn;
    }
    *n = t.calls.fetch_add(1) + 1;
    if (!fn) { RET(0); return 0; }
    return call_original(c, fn);
}
void tr_init(::Ctx* c) {
    const uint64_t cache = A(3);
    uint32_t n;
    const uint32_t r = static_cast<uint32_t>(traced(c, t_init, &n));
    if (n <= 8 || r) NXLOG_I("movie.trace", "MediaExtractor::Initialize(cache %llu) -> 0x%x", (unsigned long long)cache, r);
}
void tr_msm(::Ctx* c) {
    uint32_t n;
    const uint32_t r = static_cast<uint32_t>(traced(c, t_msm, &n));
    if (n <= 8 || r) NXLOG_I("movie.trace", "MediaExtractor::SetUpMediaSourceMode -> 0x%x", r);
}
void tr_size(::Ctx* c) {
    const int64_t size = static_cast<int64_t>(A(1));
    uint32_t n;
    const uint32_t r = static_cast<uint32_t>(traced(c, t_size, &n));
    if (n <= 8 || r) NXLOG_I("movie.trace", "StreamSourceSetSize(%lld) -> 0x%x", (long long)size, r);
}
void tr_append(::Ctx* c) {
    const uint64_t data = A(1), size = A(2);
    uint32_t n;
    const uint32_t r = static_cast<uint32_t>(traced(c, t_append, &n));
    const uint64_t total = t_appended.fetch_add(size) + size;
    static std::atomic<uint32_t> logged{0};
    if ((n <= 3 || size == 0 || r) && logged.fetch_add(1) < 24)
        NXLOG_I("movie.trace", "AppendMediaSourceData #%u: %llu bytes%s (%llu in all) -> 0x%x", n, (unsigned long long)size,
                data ? "" : " (no data: end of stream)", (unsigned long long)total, r);
}
void tr_seekpos(::Ctx* c) {
    const uint64_t out = A(1);
    uint32_t n;
    const uint32_t r = static_cast<uint32_t>(traced(c, t_seekpos, &n));
    if (n <= 16 || r) NXLOG_I("movie.trace", "GetMediaSourceModeSeekPosition -> %lld, 0x%x", out ? (long long)rd64(out) : -1ll, r);
}
void tr_count(::Ctx* c) {
    const uint64_t out = A(1);
    uint32_t n;
    const uint32_t r = static_cast<uint32_t>(traced(c, t_count, &n));
    int32_t k = -1;
    if (out) std::memcpy(&k, reinterpret_cast<const void*>(out), 4);
    if (n <= 8 || r) NXLOG_I("movie.trace", "MediaExtractor::GetTrackCount -> %d tracks, 0x%x", k, r);
}
void tr_props(::Ctx* c) {
    const int32_t track = static_cast<int32_t>(A(2));
    uint32_t n;
    const uint32_t r = static_cast<uint32_t>(traced(c, t_props, &n));
    if (n <= 8 || r) NXLOG_I("movie.trace", "MediaExtractor::GetTrackProperties(track %d) -> 0x%x", track, r);
}
void tr_getstr(::Ctx* c) {   // (const MediaProperty*, char* out, int size, const char* key) -> bool
    const uint64_t out = A(1), key = A(3);
    uint32_t n;
    const uint64_t r = traced(c, t_getstr, &n) & 1;
    const char* k = key ? reinterpret_cast<const char*>(key) : "";
    static std::atomic<uint32_t> logged{0};
    if (std::strstr(k, "mime") && logged.fetch_add(1) < 16)
        NXLOG_I("movie.trace", "MediaProperty::GetString(\"%s\") -> %s \"%.40s\"", k, r ? "found" : "missing",
                r && out ? reinterpret_cast<const char*>(out) : "");
}
struct TraceImport {
    Traced* t;
    HleFn fn;
};
const TraceImport kTraceImports[] = {
    {&t_init, tr_init}, {&t_msm, tr_msm}, {&t_size, tr_size}, {&t_append, tr_append},
    {&t_seekpos, tr_seekpos}, {&t_count, tr_count}, {&t_props, tr_props}, {&t_getstr, tr_getstr},
};

struct Import {
    const char* name;
    HleFn fn;
};
const Import kImports[] = {
    {"_ZN2nn5codec25MakeDecodeImagePreferenceEttNS0_24VideoDecoderOutputFormatENS0_9CodecModeE", make_pref},
    {"_ZN2nn5codec3sdk34MakeDecodeImagePreferenceForCompatEttNS0_24VideoDecoderOutputFormatENS0_9CodecModeE", make_pref},
    {"_ZN2nn5codec31CalculateDecoderImagePoolBufferEPmS1_RKNS0_27VideoDecoderImagePreferenceEj", calc_pool},
    {"_ZN2nn5codec26InitializeDecoderImagePoolEPNS0_25VideoDecoderImagePoolTypeEPvmRKNS0_27VideoDecoderImagePreferenceEjNS0_9CodecModeE", init_pool},
    {"_ZN2nn5codec24FinalizeDecoderImagePoolEPNS0_25VideoDecoderImagePoolTypeE", fini_pool},
    {"_ZN2nn5codec26ReadDecoderOutputImageNv12EPKNS0_25VideoDecoderImagePoolTypeEjttttPvmmS4_mm", read_nv12},
    {"_ZN2nn5codec26ReadDecoderOutputImageRgbaEPKNS0_25VideoDecoderImagePoolTypeEjttttPvmm", read_rgba},
    {"_ZN2nn5codec36GetDecoderImagePoolNativeImageHandleEPKNS0_25VideoDecoderImagePoolTypeEj", native_image_handle},
    // VP9
    {"_ZN2nn5codec29CalculateVp9DecoderWorkBufferEPmS1_mtt", calc_work},
    {"_ZN2nn5codec20InitializeVp9DecoderEPNS0_14Vp9DecoderTypeEPvmmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatE", init_vp9_tm},
    {"_ZN2nn5codec20InitializeVp9DecoderEPNS0_14Vp9DecoderTypeEmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatE", init_vp9_rm},
    {"_ZN2nn5codec20InitializeVp9DecoderEPNS0_14Vp9DecoderTypeEmmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatE", init_vp9_rm2},
    {"_ZN2nn5codec18FinalizeVp9DecoderEPNS0_14Vp9DecoderTypeE", finalize_decoder},
    {"_ZN2nn5codec24RegisterVp9DecoderBufferEPNS0_14Vp9DecoderTypeEjPNS0_25VideoDecoderImagePoolTypeEj", register_buffer},
    {"_ZN2nn5codec15StartVp9DecoderEPNS0_14Vp9DecoderTypeE", start_decoder},
    {"_ZN2nn5codec14StopVp9DecoderEPNS0_14Vp9DecoderTypeE", stop_decoder},
    {"_ZN2nn5codec15FlushVp9DecoderEPNS0_14Vp9DecoderTypeE", flush_decoder},
    {"_ZN2nn5codec33GetVp9DecoderSubmissionReadyEventEPPNS_2os15SystemEventTypeEPNS0_14Vp9DecoderTypeE", submission_event},
    {"_ZN2nn5codec27SubmitVp9DecoderInputBufferEPmPNS0_14Vp9DecoderTypeEPKvmNS_8TimeSpanEb", submit},
    {"_ZN2nn5codec29AcquireVp9DecoderOutputBufferEPjPNS_2gs16NativeSyncHandleEPNS0_14Vp9DecoderTypeE", acquire_output},
    {"_ZN2nn5codec29ReleaseVp9DecoderOutputBufferEPNS0_14Vp9DecoderTypeEjNS_2gs16NativeSyncHandleE", release_output},
    {"_ZN2nn5codec35GetVp9DecoderOutputBufferPropertiesEPtS1_S1_S1_PNS_8TimeSpanEPbPNS0_14Vp9DecoderTypeEj", props},
    {"_ZN2nn5codec35GetVp9DecoderOutputBufferPropertiesEPtS1_S1_S1_PNS_5movie10ColorSpaceEPNS_8TimeSpanEPbPNS0_14Vp9DecoderTypeEj", props_cs},
    // VP8 (same decoder on libvpx's VP8 interface)
    {"_ZN2nn5codec29CalculateVp8DecoderWorkBufferEPmS1_mtt", calc_work},
    {"_ZN2nn5codec20InitializeVp8DecoderEPNS0_14Vp8DecoderTypeEPvmmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatE", init_vp8_tm},
    {"_ZN2nn5codec20InitializeVp8DecoderEPNS0_14Vp8DecoderTypeEmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatE", init_vp8_rm},
    {"_ZN2nn5codec20InitializeVp8DecoderEPNS0_14Vp8DecoderTypeEmmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatE", init_vp8_rm2},
    {"_ZN2nn5codec18FinalizeVp8DecoderEPNS0_14Vp8DecoderTypeE", finalize_decoder},
    {"_ZN2nn5codec24RegisterVp8DecoderBufferEPNS0_14Vp8DecoderTypeEjPNS0_25VideoDecoderImagePoolTypeEj", register_buffer},
    {"_ZN2nn5codec15StartVp8DecoderEPNS0_14Vp8DecoderTypeE", start_decoder},
    {"_ZN2nn5codec14StopVp8DecoderEPNS0_14Vp8DecoderTypeE", stop_decoder},
    {"_ZN2nn5codec15FlushVp8DecoderEPNS0_14Vp8DecoderTypeE", flush_decoder},
    {"_ZN2nn5codec33GetVp8DecoderSubmissionReadyEventEPPNS_2os15SystemEventTypeEPNS0_14Vp8DecoderTypeE", submission_event},
    {"_ZN2nn5codec27SubmitVp8DecoderInputBufferEPmPNS0_14Vp8DecoderTypeEPKvmNS_8TimeSpanEb", submit},
    {"_ZN2nn5codec29AcquireVp8DecoderOutputBufferEPjPNS_2gs16NativeSyncHandleEPNS0_14Vp8DecoderTypeE", acquire_output},
    {"_ZN2nn5codec29ReleaseVp8DecoderOutputBufferEPNS0_14Vp8DecoderTypeEjNS_2gs16NativeSyncHandleE", release_output},
    {"_ZN2nn5codec35GetVp8DecoderOutputBufferPropertiesEPtS1_S1_S1_PNS_8TimeSpanEPbPNS0_14Vp8DecoderTypeEj", props},
    {"_ZN2nn5codec35GetVp8DecoderOutputBufferPropertiesEPtS1_S1_S1_PNS_5movie10ColorSpaceEPNS_8TimeSpanEPbPNS0_14Vp8DecoderTypeEj", props_cs},
    // H.264 / AV1: not decoded here (a clear error instead of the console driver path)
    {"_ZN2nn5codec21InitializeH264DecoderEPNS0_15H264DecoderTypeEPvmmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatEPKvm", unsupported_codec},
    {"_ZN2nn5codec21InitializeH264DecoderEPNS0_15H264DecoderTypeEmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatEPKvm", unsupported_codec},
    // SDK 22 (the game): the image calls without CodecMode, the decoders "By{Transfer,Remote}Memory" with an output mode, and the
    // submission-ready event returned instead of written through a pointer
    {"_ZN2nn5codec25MakeDecodeImagePreferenceEttNS0_24VideoDecoderOutputFormatENS0_15ImageTilingModeE", make_pref},
    {"_ZN2nn5codec26InitializeDecoderImagePoolEPNS0_25VideoDecoderImagePoolTypeEPvmRKNS0_27VideoDecoderImagePreferenceEj", init_pool},
    {"_ZN2nn5codec36InitializeVp9DecoderByTransferMemoryEPNS0_14Vp9DecoderTypeEPvmmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatENS0_22VideoDecoderOutputModeE", init_vp9_tm},
    {"_ZN2nn5codec34InitializeVp9DecoderByRemoteMemoryEPNS0_14Vp9DecoderTypeEmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatENS0_22VideoDecoderOutputModeE", init_vp9_rm},
    {"_ZN2nn5codec33GetVp9DecoderSubmissionReadyEventEPNS0_14Vp9DecoderTypeE", submission_event_av1},
    {"_ZN2nn5codec36InitializeVp8DecoderByTransferMemoryEPNS0_14Vp8DecoderTypeEPvmmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatENS0_22VideoDecoderOutputModeE", init_vp8_tm},
    {"_ZN2nn5codec34InitializeVp8DecoderByRemoteMemoryEPNS0_14Vp8DecoderTypeEmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatENS0_22VideoDecoderOutputModeE", init_vp8_rm},
    {"_ZN2nn5codec33GetVp8DecoderSubmissionReadyEventEPNS0_14Vp8DecoderTypeE", submission_event_av1},
#if NXRT_HAVE_DAV1D
    {"_ZN2nn5codec36InitializeAv1DecoderByTransferMemoryEPNS0_14Av1DecoderTypeEPvmmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatENS0_22VideoDecoderOutputModeE", init_av1_tm},
    {"_ZN2nn5codec34InitializeAv1DecoderByRemoteMemoryEPNS0_14Av1DecoderTypeEmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatENS0_22VideoDecoderOutputModeE", init_av1_rm},
    {"_ZN2nn5codec18FinalizeAv1DecoderEPNS0_14Av1DecoderTypeE", finalize_decoder},
    {"_ZN2nn5codec24RegisterAv1DecoderBufferEPNS0_14Av1DecoderTypeEjPNS0_25VideoDecoderImagePoolTypeEj", register_buffer},
    {"_ZN2nn5codec26UnregisterAv1DecoderBufferEPNS0_14Av1DecoderTypeEj", unregister_buffer},
    {"_ZN2nn5codec15StartAv1DecoderEPNS0_14Av1DecoderTypeE", start_decoder},
    {"_ZN2nn5codec14StopAv1DecoderEPNS0_14Av1DecoderTypeE", stop_decoder},
    {"_ZN2nn5codec15FlushAv1DecoderEPNS0_14Av1DecoderTypeE", flush_decoder},
    {"_ZN2nn5codec33GetAv1DecoderSubmissionReadyEventEPNS0_14Av1DecoderTypeE", submission_event_av1},
    {"_ZN2nn5codec27SubmitAv1DecoderInputBufferEPmPNS0_14Av1DecoderTypeEPKvmNS_8TimeSpanEb", submit},
    {"_ZN2nn5codec29AcquireAv1DecoderOutputBufferEPjPNS_2gs16NativeSyncHandleEPNS0_14Av1DecoderTypeE", acquire_output},
    {"_ZN2nn5codec29ReleaseAv1DecoderOutputBufferEPNS0_14Av1DecoderTypeEjNS_2gs16NativeSyncHandleE", release_output},
    {"_ZN2nn5codec35GetAv1DecoderOutputBufferPropertiesEPtS1_S1_S1_PNS_8TimeSpanEPbPNS0_14Av1DecoderTypeEj", props},
#endif
    {"_ZN2nn5codec20InitializeAv1DecoderEPNS0_14Av1DecoderTypeEPvmmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatE", unsupported_codec},
    {"_ZN2nn5codec20InitializeAv1DecoderEPNS0_14Av1DecoderTypeEmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatE", unsupported_codec},
    // fences
    {"_ZN2nn6syncpt3sdk20CreateSyncPointEventEPNS1_18SyncPointEventTypeE", sp_create},
    {"_ZN2nn6syncpt3sdk21DestroySyncPointEventEPNS1_18SyncPointEventTypeE", sp_destroy},
    {"_ZN2nn6syncpt3sdk20CancelSyncPointEventEPNS1_18SyncPointEventTypeE", sp_cancel},
    {"_ZN2nn6syncpt3sdk21RequestSyncPointEventEPNS1_18SyncPointEventTypeERKNS1_15SyncPointHandleE", sp_request},
    {"_ZN2nn6syncpt3sdk25InitializeMultiWaitHolderEPNS_2os19MultiWaitHolderTypeEPNS1_18SyncPointEventTypeE", sp_holder},
    {"_ZN2nn6syncpt3sdk21TryWaitSyncPointEventEPKNS1_18SyncPointEventTypeE", sp_trywait},
    {"_ZN2nn2gs17TryWaitNativeSyncERKNS0_16NativeSyncHandleE", gs_trywait},
};

}  // namespace

bool install_video_codec_hle() {
    static std::once_flag once;
    std::call_once(once, [] {
        if (game::info().platform != game::Platform::Switch2) return;
        const std::string mode = config_get_str("media", "video_decoder", "host");
        if (mode == "off") {
            NXLOG_I("vcodec", "[media] video_decoder = off: nn::codec video decoders are not on the host (movies are left out)");
            return;
        }
        g_log = config_get_bool("media", "video_decoder_log", false);
        g_max_pictures = static_cast<uint64_t>(std::max<int64_t>(0, config_get_int("media", "video_max_pictures", 0)));
        const int64_t hw = std::max(1u, std::thread::hardware_concurrency());
        g_threads = static_cast<int>(std::clamp<int64_t>(config_get_int("media", "video_decoder_threads", std::clamp<int64_t>(hw / 2, 4, 8)), 1, 16));
        g_ahead = static_cast<size_t>(std::clamp<int64_t>(config_get_int("media", "video_decode_ahead", 8), 0, 32));
        g_frame_delay = static_cast<int>(std::clamp<int64_t>(config_get_int("media", "video_frame_delay", 0), 0, 8));
        for (const Import& im : kImports) {
            hle_register(im.name, im.fn);
            module_override_symbol(im.name);
        }
        g_enabled = true;
        NXLOG_I("vcodec", "nn::codec VP9/VP8%s decoders on the host (libvpx %s, %d threads): %zu imports routed", NXRT_HAVE_DAV1D ? "/AV1 (dav1d)" : "",
                vpx_codec_version_str(), g_threads, sizeof kImports / sizeof kImports[0]);
        if (config_get_bool("media", "movie_trace", true)) {
            for (const TraceImport& ti : kTraceImports) {
                hle_register(ti.t->name, ti.fn);
                module_override_symbol(ti.t->name);
            }
            NXLOG_I("vcodec", "[media] movie_trace: %zu nn::movie extractor calls logged (rate-limited)", sizeof kTraceImports / sizeof kTraceImports[0]);
        }
    });
    return g_enabled;
}

VideoCodecStats video_codec_stats() {
    VideoCodecStats s;
    s.decoders_created = s_created;
    s.decoders_destroyed = s_destroyed;
    s.packets_submitted = s_packets;
    s.frames_decoded = s_decoded;
    s.frames_delivered = s_delivered;
    s.frames_read = s_read;
    s.decode_errors = s_errors;
    return s;
}

#else   // built without libvpx

bool install_video_codec_hle() {
    static bool logged = false;
    if (!logged && game::info().platform == game::Platform::Switch2) {
        logged = true;
        NXLOG_W("vcodec", "built without libvpx (NXRT_HAVE_LIBVPX): movies are left out");
    }
    return false;
}
VideoCodecStats video_codec_stats() { return {}; }
namespace {
std::atomic<bool> g_enabled{false};
}

#endif

bool video_codec_enabled() { return g_enabled; }

}  // namespace nxrt::media
