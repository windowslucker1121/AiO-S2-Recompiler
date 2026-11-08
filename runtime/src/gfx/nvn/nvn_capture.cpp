// SPDX-License-Identifier: GPL-3.0-or-later
// Frame capture and replay (docs: runtime/docs/CAPTURE.md).
//
// Capture: one whole presented frame of the running game is written to a folder: the command lists the game submitted between two presents
// (and the lists they call), every back-end object they reach (memory pools, textures, texture and sampler pools, programs, syncs, events,
// the window), the replay's persistent GPU state at the frame's start (GState) and the bytes of the guest memory the GPU reads (every
// non-virtual memory pool, the shader code blocks and control sections). GPU addresses are host addresses (identity mapped guest memory,
// nxrt/memory.h), so the replay puts the memory back at the same addresses and the recorded commands work unchanged; only the host object
// pointers in the payloads are written as object ids and patched on load.
//
// Replay (`<game>.exe --replay-capture <folder>`): no game code runs. The runtime starts the window and the NVN back end as for the game,
// restores the memory and the objects, and then submits the frame's lists and presents it again and again, starting each loop from the
// captured GPU state. Everything the back end offers for debugging works on it: [nvn] skip_fs_file (re-read live), trace_frame,
// trace_snap*, the F1 overlay, screenshots; shaders come from the shader cache as in the game, so a re-translated shader shows on the next start.
//
// Limits: the memory is read when the frame's last submission has completed, so CPU data the game rewrote for the next frame while the
// capture was being written (a uniform ring buffer with fewer slots than frames in flight) can differ; GPU-only contents of render targets
// from earlier frames (history buffers, shadow maps reused across frames) are not in the capture and build up again over the first loops.
// A capture is tied to the runtime build that wrote it (the size of the GPU state is checked).
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "nxrt/bugcapture.h"
#include "nxrt/config.h"
#include "nxrt/instance.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"
#include "nxrt/nvn_backend.h"
#include "nxrt/platform.h"
#include "nvn_capture.h"
#include "nvn_cmd.h"
#include "nvn_core.h"
#include "nvn_gpu_internal.h"
#include "nvn_shaders.h"
#include "nvn_texture_layout.h"

namespace stdfs = std::filesystem;

namespace {
stdfs::path u8p(const std::string& s) { return stdfs::path(std::u8string(s.begin(), s.end())); }
std::string u8s(const stdfs::path& p) {
    const std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}
}  // namespace

namespace nxrt::nvn::be::capture {
namespace {

constexpr const char* kTag = "nvn.capture";
constexpr char kMagic[8] = {'N', 'X', 'C', 'A', 'P', '0', '0', '1'};
constexpr uint32_t kVersion = 3;     // 2: 8 shader stages per program and in the GPU state (task, mesh); 3: blocks still being translated
constexpr int kCapStages = 8;
uint32_t g_read_version = kVersion;   // of the capture being loaded
constexpr uint64_t kChunk = 1ull << 20;      // memory is written in chunks of this size; all-zero chunks are left out
constexpr uint64_t kControlBytes = 0x800;    // a shader control section (the fields read_control uses end before 0x760)

// ---- the frame being recorded ---------------------------------------------------------------------------------------------------
struct FrameEvent {
    int kind = 0;                            // 0 submit, 1 present
    std::vector<Ref<CmdList>> lists;         // submit
    Ref<Window> window;                      // present
    int index = 0;
};

enum class Phase { Idle, Armed, Recording };

struct Recorder {
    std::mutex mu;
    Phase phase = Phase::Idle;
    std::string name;                        // folder name of the capture being made
    std::vector<FrameEvent> events;
    std::unordered_map<uint64_t, Ref<CmdList>> called;   // lists the recorded ones call (CallCommands), resolved at submit time
    bool state_requested = false;
    std::shared_ptr<void> state;             // GState at the start of the first recorded submission (render thread)
    uint64_t presents = 0;
    uint64_t request_polls = 0;
};
Recorder& rec() {
    static Recorder r;
    return r;
}

std::atomic<bool> g_replaying{false};
std::atomic<bool> g_collect_writes{false};
// Frame start contents of the memory the captured frame writes (frame_submit_begin): saved on the render thread at the first GPU write of
// each range, written into memory.bin instead of what the range held at the end of the frame. Non-overlapping, by address.
std::mutex g_pre_mu;
std::map<uint64_t, std::vector<uint8_t>> g_pre;
uint64_t g_pre_bytes = 0;
bool g_pre_on = false;                 // render thread: a submission of the frame being captured is being recorded
constexpr uint64_t kPreMaxBytes = 1ull << 30;
// Frame start contents of the images the GPU had rendered (read when the capture is armed, before the frame's first submission is queued):
// images the frame updates in place (OoT's volumetric fog volumes accumulate into themselves) must start from what the frame started from,
// not from what it left behind. Images rendered for the first time in the frame are read at its end, as before.
std::mutex g_pre_img_mu;
std::unordered_map<const Texture*, std::pair<Ref<Texture>, gpu::TexImageDump>> g_pre_img;
std::mutex g_writes_mu;
std::vector<std::pair<uint64_t, uint64_t>> g_writes;   // [lo, hi) the GPU wrote during the first loop

std::string capture_root() {
    const std::string d = config_get_str("capture", "dir", "");
    if (!d.empty()) return d;
    wchar_t buf[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return u8s(stdfs::path(std::wstring(buf, n)).parent_path() / "captures");
}

std::string stamp() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    char s[32];
    std::snprintf(s, sizeof s, "%04u%02u%02u-%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return s;
}

std::string sanitize(const std::string& in) {
    std::string o;
    for (char c : in) o += (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') ? c : '_';
    return o;
}

// ---- binary writer / reader -------------------------------------------------------------------------------------------------------
class Out {
public:
    explicit Out(const stdfs::path& p) { f_ = _wfopen(p.c_str(), L"wb"); if (f_) setvbuf(f_, nullptr, _IOFBF, 8 << 20); }
    ~Out() { if (f_) std::fclose(f_); }
    bool ok() const { return f_ && !err_; }
    void raw(const void* p, size_t n) { if (f_ && n && std::fwrite(p, 1, n, f_) != n) err_ = true; bytes_ += n; }
    template <class T> void put(const T& v) { static_assert(std::is_trivially_copyable_v<T>); raw(&v, sizeof v); }
    void str(const std::string& s) { put(static_cast<uint32_t>(s.size())); raw(s.data(), s.size()); }
    void vec(const std::vector<uint8_t>& v) { put(static_cast<uint64_t>(v.size())); raw(v.data(), v.size()); }
    uint64_t bytes() const { return bytes_; }
    bool close() { bool r = ok(); if (f_ && std::fclose(f_) != 0) r = false; f_ = nullptr; return r; }
private:
    FILE* f_ = nullptr;
    bool err_ = false;
    uint64_t bytes_ = 0;
};

class In {
public:
    explicit In(std::vector<uint8_t> d) : d_(std::move(d)) {}
    bool ok() const { return !err_; }
    bool more() const { return !err_ && at_ < d_.size(); }
    void raw(void* p, size_t n) {
        if (err_ || at_ + n > d_.size()) { err_ = true; if (n) std::memset(p, 0, n); return; }
        if (n) std::memcpy(p, d_.data() + at_, n);
        at_ += n;
    }
    template <class T> T get() { T v{}; raw(&v, sizeof v); return v; }
    std::string str() { const uint32_t n = get<uint32_t>(); if (n > (1u << 24)) { err_ = true; return {}; } std::string s(n, '\0'); raw(s.data(), n); return s; }
    std::vector<uint8_t> vec() { const uint64_t n = get<uint64_t>(); if (n > d_.size()) { err_ = true; return {}; } std::vector<uint8_t> v(n); raw(v.data(), n); return v; }
private:
    std::vector<uint8_t> d_;
    size_t at_ = 0;
    bool err_ = false;
};

bool read_whole(const stdfs::path& p, std::vector<uint8_t>& out) {
    FILE* f = _wfopen(p.c_str(), L"rb");
    if (!f) return false;
    _fseeki64(f, 0, SEEK_END);
    const int64_t n = _ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);
    out.resize(static_cast<size_t>(std::max<int64_t>(0, n)));
    const bool ok = n >= 0 && std::fread(out.data(), 1, out.size(), f) == out.size();
    std::fclose(f);
    return ok;
}

// ---- object pointers in payloads ----------------------------------------------------------------------------------------------------
// Calls fn on every host object pointer of a record (the slot holds an Obj-derived pointer, or after the writer an id).
template <class Fn>
void for_each_ptr(Op op, uint8_t* payload, Fn&& fn) {
    auto slot = [&](void* p) { fn(reinterpret_cast<uint64_t*>(p)); };
    switch (op) {
        case Op::BindProgram: slot(&reinterpret_cast<P_BindProgram*>(payload)->program); break;
        case Op::SetTexturePool: case Op::SetSamplerPool: slot(&reinterpret_cast<P_Pool*>(payload)->pool); break;
        case Op::SetRenderTargets: {
            auto* p = reinterpret_cast<P_RenderTargets*>(payload);
            for (auto& c : p->color) slot(&c.tex);
            slot(&p->depth.tex);
            break;
        }
        case Op::ClearTexture: slot(&reinterpret_cast<P_ClearTexture*>(payload)->tex); break;
        case Op::CopyBufferToTexture: case Op::CopyTextureToBuffer: slot(&reinterpret_cast<P_CopyBufTex*>(payload)->tex); break;
        case Op::CopyTextureToTexture: {
            auto* p = reinterpret_cast<P_CopyTexTex*>(payload);
            slot(&p->src);
            slot(&p->dst);
            break;
        }
        case Op::Downsample: {
            auto* p = reinterpret_cast<P_Downsample*>(payload);
            slot(&p->src);
            slot(&p->dst);
            break;
        }
        case Op::EvaluateDlss: {
            auto* p = reinterpret_cast<P_EvaluateDlss*>(payload);
            for (DlssTex* t : {&p->color, &p->depth, &p->motion, &p->output, &p->bias, &p->exposure}) slot(&t->tex);
            break;
        }
        case Op::FenceSync: case Op::WaitSync: slot(&reinterpret_cast<P_Sync*>(payload)->sync); break;
        case Op::SignalEvent: case Op::WaitEvent: slot(&reinterpret_cast<P_Event*>(payload)->event); break;
        default: break;
    }
}

// Iterates the records of a byte buffer (a list's bytes or a copy of them).
template <class Fn>
void for_each_record(std::vector<uint8_t>& bytes, Fn&& fn) {
    size_t at = 0;
    while (at + sizeof(RecHeader) <= bytes.size()) {
        RecHeader h;
        std::memcpy(&h, &bytes[at], sizeof h);
        if (h.size < sizeof(RecHeader) || at + h.size > bytes.size()) break;
        fn(static_cast<Op>(h.op), &bytes[at + sizeof(RecHeader)], h.size - static_cast<uint32_t>(sizeof(RecHeader)));
        at += h.size;
    }
}

uint32_t payload_size_of_call() { return (static_cast<uint32_t>(sizeof(P_CallCommands)) + 7u) & ~7u; }

// ---- the writer -------------------------------------------------------------------------------------------------------------------
// Object kinds in the file, in the order they are written (an object only refers to kinds written before it, except the lists).
enum K : uint32_t { kPool = 0, kTex, kTpool, kSpool, kProg, kSync, kEvent, kWin, kList, kKinds };

struct Writer {
    std::unordered_map<const Obj*, uint64_t> ids[kKinds];   // id + 1 (0 = null)
    std::vector<const Obj*> objs[kKinds];

    uint64_t id_of(K k, const Obj* o) {
        if (!o) return 0;
        auto it = ids[k].find(o);
        if (it != ids[k].end()) return it->second;
        objs[k].push_back(o);
        return ids[k][o] = objs[k].size();
    }
    static K kind_of(const Obj* o) {
        switch (o->kind) {
            case Kind::MemoryPool: return kPool;
            case Kind::Texture: return kTex;
            case Kind::TexturePool: return kTpool;
            case Kind::SamplerPool: return kSpool;
            case Kind::Program: return kProg;
            case Kind::Sync: return kSync;
            case Kind::Event: return kEvent;
            case Kind::Window: return kWin;
            case Kind::CmdList: return kList;
            default: return kKinds;
        }
    }
    uint64_t id_any(const Obj* o) {
        if (!o) return 0;
        const K k = kind_of(o);
        if (k == kKinds) return 0;
        return (static_cast<uint64_t>(k) << 56) | id_of(k, o);
    }
};

// The bytes of a list with every object pointer turned into a tagged id and every called handle into a list id.
std::vector<uint8_t> list_image(Writer& w, const CmdList& l, const std::unordered_map<uint64_t, Ref<CmdList>>& called) {
    std::vector<uint8_t> b = l.bytes;
    for_each_record(b, [&](Op op, uint8_t* payload, uint32_t size) {
        if (op == Op::CallCommands) {
            const auto* p = reinterpret_cast<const P_CallCommands*>(payload);
            uint64_t* h = reinterpret_cast<uint64_t*>(payload + payload_size_of_call());
            if (payload_size_of_call() + static_cast<uint64_t>(p->count) * 8 > size) return;
            for (uint32_t i = 0; i < p->count; ++i) {
                auto it = called.find(h[i]);
                h[i] = it != called.end() ? w.id_of(kList, it->second.get()) : 0;
            }
            return;
        }
        for_each_ptr(op, payload, [&](uint64_t* slot) { *slot = w.id_any(reinterpret_cast<const Obj*>(static_cast<uintptr_t>(*slot))); });
    });
    return b;
}

struct MemRange { uint64_t a, n; };

void add_range(std::vector<MemRange>& v, uint64_t a, uint64_t n) {
    if (!a || !n) return;
    const uint64_t s = a & ~(mem::kPage - 1), e = (a + n + mem::kPage - 1) & ~(mem::kPage - 1);
    v.push_back({s, e - s});
}

std::vector<MemRange> merge_ranges(std::vector<MemRange> v) {
    std::sort(v.begin(), v.end(), [](const MemRange& x, const MemRange& y) { return x.a < y.a; });
    std::vector<MemRange> o;
    for (const MemRange& r : v) {
        if (!o.empty() && r.a <= o.back().a + o.back().n) o.back().n = std::max(o.back().n, r.a + r.n - o.back().a);
        else o.push_back(r);
    }
    return o;
}

bool all_zero(const uint8_t* p, size_t n) {
    const uint64_t* q = reinterpret_cast<const uint64_t*>(p);
    for (size_t i = 0; i < n / 8; ++i) if (q[i]) return false;
    for (size_t i = n & ~size_t(7); i < n; ++i) if (p[i]) return false;
    return true;
}

// Copies [a, a + n) of guest memory into dst; pages that are not readable read as zeros.
void read_guest(uint64_t a, uint64_t n, uint8_t* dst) {
    if (mem::is_mapped(a, n)) { std::memcpy(dst, reinterpret_cast<const void*>(a), n); return; }
    for (uint64_t o = 0; o < n; o += mem::kPage) {
        const uint64_t k = std::min<uint64_t>(mem::kPage, n - o);
        if (mem::is_mapped(a + o, k)) std::memcpy(dst + o, reinterpret_cast<const void*>(a + o), k);
        else std::memset(dst + o, 0, k);
    }
}

template <class T>
std::vector<uint8_t> pod_bytes(const T& v) {
    std::vector<uint8_t> b(sizeof(T));
    std::memcpy(b.data(), &v, sizeof(T));
    return b;
}

// Writes the recorded frame. Game thread, after the frame's submissions completed on the GPU.
bool write_capture(const std::string& name, std::vector<FrameEvent>& events, const std::unordered_map<uint64_t, Ref<CmdList>>& called,
                   const std::shared_ptr<void>& state) {
    const auto t0 = std::chrono::steady_clock::now();
    const stdfs::path dir = u8p(capture_root()) / u8p(name);
    std::error_code ec;
    stdfs::create_directories(dir, ec);
    Writer w;

    // 1. events and the lists they reach (the list table grows while the called lists are imaged)
    std::vector<std::vector<uint8_t>> list_bytes;
    struct EvImg { int kind; std::vector<uint64_t> lists; uint64_t win; int index; };
    std::vector<EvImg> evs;
    for (FrameEvent& e : events) {
        EvImg x{e.kind, {}, 0, e.index};
        if (e.kind == 0) for (auto& l : e.lists) x.lists.push_back(w.id_of(kList, l.get()));
        else x.win = w.id_of(kWin, e.window.get());
        evs.push_back(std::move(x));
    }
    for (size_t i = 0; i < w.objs[kList].size(); ++i)   // the table grows while called lists are met
        list_bytes.push_back(list_image(w, *static_cast<const CmdList*>(w.objs[kList][i]), called));

    // 2. the GPU state at the frame's start
    std::vector<uint8_t> gpod;
    gpu::ReplayStateRefs grefs;
    if (state) gpu::replay_state_image(state, gpod, grefs);
    uint64_t g_tpool = w.id_of(kTpool, grefs.tpool), g_spool = w.id_of(kSpool, grefs.spool), g_depth = w.id_of(kTex, grefs.depth);
    uint64_t g_color[8];
    for (int i = 0; i < 8; ++i) g_color[i] = w.id_of(kTex, grefs.color[i]);

    // 3. what the objects reach: windows -> textures; texture pools -> textures; textures -> pools; events -> pools
    for (size_t i = 0; i < w.objs[kWin].size(); ++i)
        for (auto& t : static_cast<const Window*>(w.objs[kWin][i])->textures) w.id_of(kTex, t.get());
    for (size_t i = 0; i < w.objs[kTpool].size(); ++i) {
        auto* tp = const_cast<TexturePool*>(static_cast<const TexturePool*>(w.objs[kTpool][i]));
        std::shared_lock lk(tp->lock);
        w.id_of(kPool, tp->mem);
        for (auto& e : tp->entries) if (e.used && e.tex) w.id_of(kTex, e.tex.get());
    }
    for (size_t i = 0; i < w.objs[kSpool].size(); ++i) w.id_of(kPool, static_cast<const SamplerPool*>(w.objs[kSpool][i])->mem);
    for (size_t i = 0; i < w.objs[kEvent].size(); ++i) w.id_of(kPool, static_cast<const Event*>(w.objs[kEvent][i])->pool);
    for (size_t i = 0; i < w.objs[kTex].size(); ++i) w.id_of(kPool, static_cast<const Texture*>(w.objs[kTex][i])->vpool.get());
    // every live pool: buffers, vertex and index data are addressed by GPU address, not by object
    std::vector<Ref<MemoryPool>> pools;
    pool_collect(pools);
    for (auto& p : pools) w.id_of(kPool, p.get());
    for (size_t i = 0; i < w.objs[kPool].size(); ++i) {
        auto* p = const_cast<MemoryPool*>(static_cast<const MemoryPool*>(w.objs[kPool][i]));
        std::lock_guard lk(p->vmu);
        for (auto& m : p->vmaps) w.id_of(kPool, m.phys.get());
    }

    // GPU results the game's memory does not hold: compute output kept in device-local copies ([nvn] compute_out lazy) is copied back into
    // the pools; the images the GPU rendered (shadow and probe maps made once at load, history buffers, the previous frame's targets) are
    // saved as the GPU holds them (images.bin) and put back into the same images by the replay.
    for (const Obj* ob : w.objs[kPool]) {
        const MemoryPool* p = static_cast<const MemoryPool*>(ob);
        if (!p->is_virtual && p->base) gpu::cpu_will_read(p->gpu_base, p->size);
    }
    struct ImgEntry { uint64_t tex; gpu::TexImageDump d; uint64_t off; uint64_t size; };
    std::vector<ImgEntry> images;
    uint64_t img_bytes = 0, img_fail = 0, img_start = 0;
    {
        Out im(dir / "images.bin");
        for (size_t i = 0; i < w.objs[kTex].size(); ++i) {
            Texture* t = const_cast<Texture*>(static_cast<const Texture*>(w.objs[kTex][i]));
            if (!gpu::texture_gpu_written(t)) continue;
            ImgEntry e{i + 1, {}, im.bytes(), 0};
            bool from_start = false;
            {
                std::lock_guard lk(g_pre_img_mu);
                auto it = g_pre_img.find(t);
                if (it != g_pre_img.end()) { e.d = std::move(it->second.second); from_start = true; }
            }
            if (from_start) ++img_start;
            else if (!gpu::tex_image_read(t, e.d)) {   // sparse or multisampled (Limits in runtime/docs/CAPTURE.md): the replay starts it from its storage
                ++img_fail;
                NXLOG_W(kTag, "rendered image t%u (%s %dx%dx%d, %u level(s), %u layer(s)%s%s) could not be read: not saved", t->id, t->fmt ? t->fmt->name : "?",
                        t->b.width, t->b.height, t->b.depth, t->levels, t->layers, t->b.samples > 1 ? ", multisampled" : "",
                        t->b.target == NVN_TEXTURE_TARGET_3D ? ", 3D" : "");
                continue;
            }
            im.raw(e.d.bytes.data(), e.d.bytes.size());
            img_bytes += e.d.bytes.size();
            e.size = e.d.bytes.size();
            e.d.bytes.clear();
            e.d.bytes.shrink_to_fit();
            images.push_back(std::move(e));
        }
        im.close();
    }
    NXLOG_I(kTag, "rendered images saved: %zu (%.0f MiB; %llu as the frame started), %llu could not be read", images.size(), img_bytes / 1048576.0,
            static_cast<unsigned long long>(img_start), static_cast<unsigned long long>(img_fail));
    {
        std::lock_guard lk(g_pre_img_mu);
        g_pre_img.clear();
    }

    Out o(dir / "capture.bin");
    o.raw(kMagic, 8);
    o.put(kVersion);
    o.put(gpu::replay_state_size());
    o.str(config_get_str("process", "program_id", ""));
    o.put(static_cast<uint32_t>(sizeof(TextureBuilderInfo)));
    o.put(static_cast<uint32_t>(sizeof(TextureViewInfo)));
    o.put(static_cast<uint32_t>(sizeof(SamplerInfo)));
    o.put(static_cast<uint32_t>(sizeof(WindowHdrInfo)));

    // pools
    std::vector<MemRange> ranges;
    uint64_t pool_bytes = 0;
    o.put(static_cast<uint64_t>(w.objs[kPool].size()));
    for (const Obj* ob : w.objs[kPool]) {
        auto* p = const_cast<MemoryPool*>(static_cast<const MemoryPool*>(ob));
        o.put(reinterpret_cast<uint64_t>(p->base));
        o.put(p->size);
        o.put(static_cast<int32_t>(p->flags));
        o.put(p->gpu_base);
        o.put(static_cast<uint8_t>(p->is_virtual));
        o.put(static_cast<uint8_t>(p->is_shader_code));
        std::lock_guard lk(p->vmu);
        o.put(static_cast<uint32_t>(p->vmaps.size()));
        for (auto& m : p->vmaps) {
            o.put(m.voff);
            o.put(m.size);
            o.put(w.ids[kPool].count(m.phys.get()) ? w.ids[kPool][m.phys.get()] : uint64_t(0));
            o.put(m.poff);
        }
        if (!p->is_virtual && p->base) { add_range(ranges, reinterpret_cast<uint64_t>(p->base), p->size); pool_bytes += p->size; }
    }
    // textures
    o.put(static_cast<uint64_t>(w.objs[kTex].size()));
    for (const Obj* ob : w.objs[kTex]) {
        const Texture* t = static_cast<const Texture*>(ob);
        TextureBuilderInfo b = t->b;
        b.device = nullptr;
        b.pool = nullptr;
        b.packaged_data = nullptr;
        o.raw(&b, sizeof b);
        o.put(reinterpret_cast<uint64_t>(t->mem));
        o.put(w.ids[kPool].count(t->vpool.get()) ? w.ids[kPool][t->vpool.get()] : uint64_t(0));
        o.put(t->voffset);
        o.put(t->id);
        o.put(static_cast<uint8_t>(t->cpu_coherent));
        o.put(static_cast<uint8_t>(t->destroyed.load()));
    }
    // texture pools
    o.put(static_cast<uint64_t>(w.objs[kTpool].size()));
    for (const Obj* ob : w.objs[kTpool]) {
        auto* tp = const_cast<TexturePool*>(static_cast<const TexturePool*>(ob));
        std::shared_lock lk(tp->lock);
        o.put(w.ids[kPool].count(tp->mem) ? w.ids[kPool][tp->mem] : uint64_t(0));
        o.put(tp->offset);
        o.put(static_cast<int32_t>(tp->count));
        uint32_t used = 0;
        for (auto& e : tp->entries) used += e.used ? 1 : 0;
        o.put(used);
        for (size_t i = 0; i < tp->entries.size(); ++i) {
            const auto& e = tp->entries[i];
            if (!e.used) continue;
            o.put(static_cast<uint32_t>(i));
            o.put(e.tex ? w.ids[kTex][e.tex.get()] : uint64_t(0));
            o.raw(&e.view, sizeof e.view);
            o.put(static_cast<uint8_t>(e.has_view));
        }
    }
    // sampler pools
    o.put(static_cast<uint64_t>(w.objs[kSpool].size()));
    for (const Obj* ob : w.objs[kSpool]) {
        auto* sp = const_cast<SamplerPool*>(static_cast<const SamplerPool*>(ob));
        std::shared_lock lk(sp->lock);
        o.put(w.ids[kPool].count(sp->mem) ? w.ids[kPool][sp->mem] : uint64_t(0));
        o.put(sp->offset);
        o.put(static_cast<int32_t>(sp->count));
        uint32_t used = 0;
        for (size_t i = 0; i < sp->entries.size(); ++i) used += (i < sp->used.size() && sp->used[i]) ? 1 : 0;
        o.put(used);
        for (size_t i = 0; i < sp->entries.size(); ++i) {
            if (!(i < sp->used.size() && sp->used[i])) continue;
            o.put(static_cast<uint32_t>(i));
            o.raw(&sp->entries[i], sizeof(SamplerInfo));
        }
    }
    // programs: the code block and control section of each stage (their bytes go into the memory ranges)
    o.put(static_cast<uint64_t>(w.objs[kProg].size()));
    for (const Obj* ob : w.objs[kProg]) {
        const Program* p = static_cast<const Program*>(ob);
        o.str(p->label);
        for (int s = 0; s < kCapStages; ++s) {
            o.put(p->code_addr[s]);
            o.put(p->control_addr[s]);
            o.str(p->stage[s] ? p->stage[s]->hash : std::string());
            if (p->code_addr[s] && p->control_addr[s]) {
                add_range(ranges, p->control_addr[s], kControlBytes);
                const ShaderControl c = p->stage[s] ? p->stage[s]->control : ShaderControl{};
                add_range(ranges, p->code_addr[s], c.code_size ? c.code_size : 0x1000);
            }
        }
        o.put(static_cast<uint32_t>(p->sample_shading));
        // Blocks the on-demand translator had not finished when the frame was captured: SetShaders knows no stage for them yet, so
        // without these the replay ran such programs without the stage (a capture taken while a run translates, run cap_1: five
        // dispatches of OoT's culling chain "cs -" on every replay). The replay resolves them like a running game does.
        std::vector<std::pair<uint64_t, uint64_t>> pend;
        {
            std::unique_lock lk(const_cast<Program*>(p)->pending_mu, std::try_to_lock);
            if (lk.owns_lock())
                for (const Program::PendingBlock& b : p->pending)
                    pend.emplace_back(reinterpret_cast<uint64_t>(b.code), reinterpret_cast<uint64_t>(b.control));
        }
        o.put(static_cast<uint32_t>(pend.size()));
        for (const auto& [code, control] : pend) {
            o.put(code);
            o.put(control);
            add_range(ranges, control, kControlBytes);
            const ShaderControl c = read_control(reinterpret_cast<const uint8_t*>(control));
            add_range(ranges, code, c.valid && c.code_size ? c.code_size : 0x1000);
        }
    }
    // syncs, events
    o.put(static_cast<uint64_t>(w.objs[kSync].size()));
    o.put(static_cast<uint64_t>(w.objs[kEvent].size()));
    for (const Obj* ob : w.objs[kEvent]) {
        const Event* e = static_cast<const Event*>(ob);
        o.put(w.ids[kPool].count(e->pool) ? w.ids[kPool][e->pool] : uint64_t(0));
        o.put(e->offset);
    }
    // windows
    o.put(static_cast<uint64_t>(w.objs[kWin].size()));
    for (const Obj* ob : w.objs[kWin]) {
        auto* wn = const_cast<Window*>(static_cast<const Window*>(ob));
        std::lock_guard lk(wn->lock);
        o.put(static_cast<uint32_t>(wn->textures.size()));
        for (auto& t : wn->textures) o.put(w.ids[kTex][t.get()]);
        o.put(static_cast<int32_t>(wn->present_interval));
        o.put(static_cast<int32_t>(wn->active));
        o.raw(wn->crop, sizeof wn->crop);
        o.raw(&wn->hdr, sizeof wn->hdr);
    }
    // lists (their object pointers are tagged ids now)
    o.put(static_cast<uint64_t>(list_bytes.size()));
    uint64_t draws = 0;
    for (size_t i = 0; i < list_bytes.size(); ++i) {
        const CmdList* l = static_cast<const CmdList*>(w.objs[kList][i]);
        o.put(l->record_count);
        o.put(l->draw_count);
        o.vec(list_bytes[i]);
        draws += l->draw_count;
    }
    // events
    o.put(static_cast<uint64_t>(evs.size()));
    for (const EvImg& e : evs) {
        o.put(static_cast<int32_t>(e.kind));
        o.put(static_cast<uint32_t>(e.lists.size()));
        for (uint64_t id : e.lists) o.put(id);
        o.put(e.win);
        o.put(static_cast<int32_t>(e.index));
    }
    // GPU state
    o.vec(gpod);
    o.put(g_tpool);
    o.put(g_spool);
    o.put(g_depth);
    for (uint64_t c : g_color) o.put(c);
    for (int s = 0; s < kCapStages; ++s) o.str(grefs.stage[s] ? grefs.stage[s]->hash : std::string());

    // memory: the table here, the bytes in memory.bin
    const std::vector<MemRange> merged = merge_ranges(ranges);
    Out m(dir / "memory.bin");
    std::vector<uint8_t> buf(kChunk);
    uint64_t written = 0, total = 0;
    o.put(static_cast<uint64_t>(merged.size()));
    for (const MemRange& r : merged) {
        uint64_t off = 0;
        o.put(r.a);
        o.put(r.n);
        const uint64_t chunks = (r.n + kChunk - 1) / kChunk;
        std::vector<uint64_t> where(chunks, ~0ull);   // offset in memory.bin, ~0: all zeros
        for (uint64_t c = 0; c < chunks; ++c) {
            const uint64_t k = std::min(kChunk, r.n - off);
            read_guest(r.a + off, k, buf.data());
            {   // ranges the frame writes: their contents at the frame's start (frame_submit_begin)
                std::lock_guard lk(g_pre_mu);
                const uint64_t lo = r.a + off, hi = lo + k;
                auto it = g_pre.upper_bound(lo);
                if (it != g_pre.begin()) --it;
                for (; it != g_pre.end() && it->first < hi; ++it) {
                    const uint64_t a = std::max(lo, it->first), b = std::min(hi, it->first + it->second.size());
                    if (a < b) std::memcpy(buf.data() + (a - lo), it->second.data() + (a - it->first), static_cast<size_t>(b - a));
                }
            }
            if (!all_zero(buf.data(), k)) { where[c] = m.bytes(); m.raw(buf.data(), k); written += k; }
            off += k;
        }
        total += r.n;
        for (uint64_t x : where) o.put(x);
    }
    {
        std::lock_guard lk(g_pre_mu);
        NXLOG_I(kTag, "memory the frame writes: %zu ranges (%.1f MiB) saved as they were when the frame started", g_pre.size(), g_pre_bytes / 1048576.0);
        g_pre.clear();
        g_pre_bytes = 0;
    }
    // device modes (nvnDeviceSetWindowOriginMode / SetDepthMode): appended, a capture without them replays with the defaults
    o.put(static_cast<int32_t>(gpu::window_origin_mode()));
    o.put(static_cast<int32_t>(gpu::depth_mode()));
    // rendered images (images.bin)
    o.put(static_cast<uint64_t>(images.size()));
    for (const ImgEntry& e : images) {
        o.put(e.tex);
        o.put(static_cast<int32_t>(e.d.format));
        o.put(e.d.base);
        o.put(e.d.levels);
        o.put(e.d.layers);
        o.put(static_cast<uint32_t>(e.d.aspect));
        o.put(e.d.scale);
        o.put(e.off);
        o.put(e.size);
    }
    const bool ok = o.close() && m.close();
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    NXLOG_I(kTag, "%s %s: %zu submissions/presents, %zu lists, %llu draws, %zu pools (%.1f GiB), %zu textures, %zu programs; memory %.2f GiB in %zu "
                  "ranges, %.2f GiB written; %.1f s",
            ok ? "written" : "FAILED", u8s(dir).c_str(), evs.size(), list_bytes.size(), static_cast<unsigned long long>(draws), w.objs[kPool].size(),
            pool_bytes / 1073741824.0, w.objs[kTex].size(), w.objs[kProg].size(), total / 1073741824.0, merged.size(), written / 1073741824.0, secs);
    return ok;
}

// ---- request polling (game thread, at presents) ---------------------------------------------------------------------------------------
// A file named capture_frame (any process) or capture_frame.<pid> in the request folder (the instance folder, or the exe's folder) asks for a
// capture; its first line, if any, names it. [capture] at_present = N captures the frame after the N-th present once.
void poll_request(Recorder& r) {
    if (r.phase != Phase::Idle) return;
    static const int64_t at = config_get_int("capture", "at_present", 0);
    if (at > 0 && r.presents == static_cast<uint64_t>(at)) {
        r.phase = Phase::Armed;
        r.name = stamp() + "-present" + std::to_string(at);
        return;
    }
    if (++r.request_polls % 15) return;   // a file check every 15 presents
    const stdfs::path dir = u8p(instance_request_dir());
    const stdfs::path own = dir / ("capture_frame." + std::to_string(GetCurrentProcessId())), any = dir / "capture_frame";
    const stdfs::path taken = dir / ("capture_frame.taken." + std::to_string(GetCurrentProcessId()));
    for (const stdfs::path* p : {&own, &any}) {
        if (!MoveFileExW(p->c_str(), taken.c_str(), MOVEFILE_REPLACE_EXISTING)) continue;
        std::vector<uint8_t> text;
        read_whole(taken, text);
        DeleteFileW(taken.c_str());
        std::string label(text.begin(), text.end());
        if (const size_t nl = label.find_first_of("\r\n"); nl != std::string::npos) label.resize(nl);
        r.phase = Phase::Armed;
        r.name = stamp();
        if (instance_active()) r.name += "-" + sanitize(instance_name());
        if (!sanitize(label).empty()) r.name += "-" + sanitize(label);
        NXLOG_I(kTag, "capture requested: %s (the next whole frame)", r.name.c_str());
        return;
    }
}

}  // namespace

// ---- hooks (nvn_gpu.cpp) -------------------------------------------------------------------------------------------------------------
bool on_submit(const std::vector<Ref<CmdList>>& lists, bool* recorded) {
    if (recorded) *recorded = false;
    if (g_replaying.load(std::memory_order_relaxed)) return false;
    Recorder& r = rec();
    std::lock_guard lk(r.mu);
    if (r.phase != Phase::Recording) return false;
    if (recorded) *recorded = true;
    FrameEvent e;
    e.kind = 0;
    e.lists = lists;
    std::vector<const CmdList*> todo;
    for (auto& l : lists) todo.push_back(l.get());
    while (!todo.empty()) {
        const CmdList* l = todo.back();
        todo.pop_back();
        std::vector<uint8_t> b = l->bytes;
        for_each_record(b, [&](Op op, uint8_t* payload, uint32_t size) {
            if (op != Op::CallCommands) return;
            const auto* p = reinterpret_cast<const P_CallCommands*>(payload);
            const uint64_t* h = reinterpret_cast<const uint64_t*>(payload + payload_size_of_call());
            if (payload_size_of_call() + static_cast<uint64_t>(p->count) * 8 > size) return;
            for (uint32_t i = 0; i < p->count; ++i) {
                if (r.called.count(h[i])) continue;
                Ref<CmdList> c = handle_lookup(h[i]);
                if (!c) continue;
                todo.push_back(c.get());
                r.called.emplace(h[i], std::move(c));
            }
        });
    }
    r.events.push_back(std::move(e));
    if (r.state_requested) return false;
    r.state_requested = true;
    return true;
}

namespace {
// Game thread, between the present before the captured frame and its first submission: every image with storage in a pool that the GPU
// rendered, as it is now (queued behind the previous frame's work, so after it).
void read_images_at_frame_start() {
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<Ref<Texture>> list = texture_registry_collect([](const Texture& t, void*) { return gpu::texture_gpu_written(&t); }, nullptr);
    uint64_t bytes = 0;
    size_t n = 0;
    std::lock_guard lk(g_pre_img_mu);
    g_pre_img.clear();
    for (Ref<Texture>& t : list) {
        gpu::TexImageDump d;
        if (!gpu::tex_image_read(t.get(), d)) continue;
        bytes += d.bytes.size();
        ++n;
        const Texture* key = t.get();
        g_pre_img.emplace(key, std::make_pair(std::move(t), std::move(d)));
    }
    NXLOG_I(kTag, "frame start: %zu rendered images read (%.0f MiB) in %.0f ms", n, bytes / 1048576.0,
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
}
}  // namespace

void state_taken(std::shared_ptr<void> snap) {
    Recorder& r = rec();
    std::lock_guard lk(r.mu);
    r.state = std::move(snap);
}

void on_present(Window* w, int index) {
    if (g_replaying.load(std::memory_order_relaxed)) return;
    Recorder& r = rec();
    std::unique_lock lk(r.mu);
    ++r.presents;
    if (r.phase == Phase::Recording) {
        FrameEvent e;
        e.kind = 1;
        e.window = Ref<Window>::share(w);
        e.index = index;
        r.events.push_back(std::move(e));
        std::vector<FrameEvent> events = std::move(r.events);
        std::unordered_map<uint64_t, Ref<CmdList>> called = std::move(r.called);
        const std::string name = r.name;
        r.events.clear();
        r.called.clear();
        r.phase = Phase::Idle;
        r.state_requested = false;
        lk.unlock();
        NXLOG_I(kTag, "frame recorded (%zu events); waiting for the GPU, then writing the capture", events.size());
        gpu::finish();   // the state snapshot is taken on the render thread with the first submission; finish() also runs it
        std::shared_ptr<void> st;
        {
            std::lock_guard l2(r.mu);
            st = std::move(r.state);
        }
        if (!st) NXLOG_W(kTag, "no GPU state snapshot was taken: the capture starts from the replay's default state");
        write_capture(name, events, called, st);
        return;
    }
    if (r.phase == Phase::Armed) {
        r.phase = Phase::Recording;   // the frame after this present
        r.events.clear();
        r.called.clear();
        lk.unlock();
        read_images_at_frame_start();
        lk.lock();
        // the game's own picture of the recorded frame (the renderer writes the next presented frame): the reference a replay is compared with
        {
            std::error_code ec;
            const stdfs::path dir = u8p(capture_root()) / u8p(r.name);
            stdfs::create_directories(dir, ec);
            bug::screenshot_next_frame(u8s(dir / "reference.png").c_str());
        }
        r.state.reset();
        r.state_requested = false;
        return;
    }
    poll_request(r);
}

void request(const std::string& name) {
    Recorder& r = rec();
    std::lock_guard lk(r.mu);
    if (r.phase != Phase::Idle) return;
    r.phase = Phase::Armed;
    r.name = stamp() + (name.empty() ? std::string() : "-" + sanitize(name));
}

bool replaying() { return g_replaying.load(std::memory_order_relaxed); }

void frame_submit_begin(bool first) {
    if (first) {
        gpu::finish_on_render_thread();   // earlier frames' GPU writes have landed before anything is saved
        std::lock_guard lk(g_pre_mu);
        g_pre.clear();
        g_pre_bytes = 0;
    }
    g_pre_on = true;
}
void frame_submit_end() { g_pre_on = false; }

namespace {
// The parts of [lo, hi) not saved yet: their bytes now (results held in device-local copies go back to the pool first).
void save_frame_start(uint64_t lo, uint64_t hi) {
    std::vector<std::pair<uint64_t, uint64_t>> gaps;
    {
        std::lock_guard lk(g_pre_mu);
        if (g_pre_bytes >= kPreMaxBytes) return;
        uint64_t cur = lo;
        auto it = g_pre.upper_bound(lo);
        if (it != g_pre.begin()) --it;
        for (; it != g_pre.end() && it->first < hi && cur < hi; ++it) {
            const uint64_t a = it->first, b = a + it->second.size();
            if (b <= cur) continue;
            if (a > cur) gaps.emplace_back(cur, a);
            cur = std::max(cur, b);
        }
        if (cur < hi) gaps.emplace_back(cur, hi);
    }
    for (auto [a, b] : gaps) {
        const PoolHit h = pool_lookup(a);
        if (!h.pool || h.pool->is_virtual || !h.pool->base) continue;
        b = std::min<uint64_t>(b, h.pool->gpu_base + h.pool->size);
        if (b <= a) continue;
        gpu::cpu_will_read(a, b - a);
        std::vector<uint8_t> v(static_cast<size_t>(b - a));
        read_guest(a, b - a, v.data());
        std::lock_guard lk(g_pre_mu);
        g_pre_bytes += v.size();
        g_pre.emplace(a, std::move(v));
    }
}
}  // namespace

void note_gpu_write(uint64_t addr, uint64_t size) {
    if (g_pre_on && size) save_frame_start(addr, addr + size);
    if (!g_collect_writes.load(std::memory_order_relaxed) || !size) return;
    std::lock_guard lk(g_writes_mu);
    if (!g_writes.empty() && g_writes.back().first == addr && g_writes.back().second == addr + size) return;
    g_writes.emplace_back(addr, addr + size);
}

// ---- the replay ---------------------------------------------------------------------------------------------------------------------
namespace {

struct Loaded {
    std::vector<Ref<MemoryPool>> pools;
    std::vector<Ref<Texture>> texs;
    std::vector<Ref<TexturePool>> tpools;
    std::vector<Ref<SamplerPool>> spools;
    std::vector<Ref<Program>> progs;
    std::vector<Ref<Sync>> syncs;
    std::vector<Ref<Event>> evs;
    std::vector<Ref<Window>> wins;
    std::vector<Ref<CmdList>> lists;
    struct Ev { int kind; std::vector<Ref<CmdList>> lists; Ref<Window> win; int index; };
    std::vector<Ev> events;
    std::vector<uint8_t> gpod;
    gpu::ReplayStateRefs grefs;
    uint64_t draws = 0;
    struct Img { Texture* tex; gpu::TexImageDump d; uint64_t off, size; };
    std::vector<Img> images;
    struct Rng { uint64_t a, n; std::vector<uint64_t> where; };
    std::vector<Rng> rngs;          // the memory table (memory.bin chunks)
};

template <class T>
T* at(const std::vector<Ref<T>>& v, uint64_t id) { return (id && id <= v.size()) ? v[id - 1].get() : nullptr; }

// Puts [a, a + n) back with the bytes in data. The heap view is the game's heap section; anything else becomes private memory at the address.
bool restore_range(uint64_t a, uint64_t n) {
    if (a >= mem::kHeapBase && a + n <= mem::kHeapBase + mem::kHeapMax) {
        if (!mem::heap_exists() && !mem::heap_create(0)) return false;
        return mem::heap_commit(a - mem::kHeapBase, n);
    }
    const uint64_t s = a & ~(mem::kAllocGranularity - 1), e = (a + n + mem::kAllocGranularity - 1) & ~(mem::kAllocGranularity - 1);
    if (VirtualAlloc(reinterpret_cast<void*>(s), static_cast<SIZE_T>(e - s), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) return true;
    if (a >= mem::kStackBase && a + n <= mem::kKernelBase + mem::kKernelSize && mem::commit(a, n, mem::Prot::RW)) return true;   // reserved by mem::init
    // part of the span is reserved already (a neighbouring range): commit page by page, reserving free 64 KiB granules on the way
    for (uint64_t g = s; g < e; g += mem::kAllocGranularity) {
        MEMORY_BASIC_INFORMATION mi{};
        VirtualQuery(reinterpret_cast<void*>(g), &mi, sizeof mi);
        if (mi.State == MEM_FREE && !VirtualAlloc(reinterpret_cast<void*>(g), static_cast<SIZE_T>(mem::kAllocGranularity), MEM_RESERVE, PAGE_READWRITE)) return false;
    }
    // one commit per granule: a commit cannot span two reservations
    for (uint64_t g = s; g < e; g += mem::kAllocGranularity) {
        const uint64_t lo = std::max(g, a), hi = std::min(g + mem::kAllocGranularity, a + n);
        if (lo >= hi) continue;
        if (!VirtualAlloc(reinterpret_cast<void*>(lo), static_cast<SIZE_T>(hi - lo), MEM_COMMIT, PAGE_READWRITE)) return false;
    }
    return true;
}

// Puts the rendered images back as the GPU held them at capture time (images.bin).
void restore_images(const stdfs::path& dir, Loaded& L, bool log) {
    if (L.images.empty()) return;
    FILE* f = _wfopen((dir / "images.bin").c_str(), L"rb");
    if (!f) { NXLOG_W(kTag, "cannot read images.bin"); return; }
    uint64_t ok = 0, bytes = 0;
    std::map<std::string, int> fails;
    for (Loaded::Img& m : L.images) {
        m.d.bytes.resize(static_cast<size_t>(m.size));
        _fseeki64(f, static_cast<int64_t>(m.off), SEEK_SET);
        if (std::fread(m.d.bytes.data(), 1, m.d.bytes.size(), f) != m.d.bytes.size()) { fails["images.bin is truncated"]++; continue; }
        std::string why;
        if (gpu::tex_image_write(m.tex, m.d, &why)) { ++ok; bytes += m.size; }
        else fails[why]++;
        m.d.bytes.clear();
        m.d.bytes.shrink_to_fit();
    }
    std::fclose(f);
    if (!log) return;
    NXLOG_I(kTag, "rendered images restored: %llu of %zu (%.0f MiB)", static_cast<unsigned long long>(ok), L.images.size(), bytes / 1048576.0);
    for (auto& [why, n] : fails) NXLOG_W(kTag, "%d image(s) not restored: %s", n, why.c_str());
}

// Puts the captured bytes of [lo, hi) back (memory.bin); bytes outside the captured ranges are left alone. Returns the bytes written.
uint64_t restore_memory(FILE* f, const Loaded& L, uint64_t lo, uint64_t hi, std::vector<uint8_t>& buf) {
    uint64_t done = 0;
    auto it = std::upper_bound(L.rngs.begin(), L.rngs.end(), lo, [](uint64_t v, const Loaded::Rng& r) { return v < r.a; });
    if (it != L.rngs.begin()) --it;
    for (; it != L.rngs.end() && it->a < hi; ++it) {
        const uint64_t a = std::max(lo, it->a), b = std::min(hi, it->a + it->n);
        for (uint64_t x = a; x < b;) {
            const uint64_t c = (x - it->a) / kChunk, c0 = it->a + c * kChunk, k = std::min(b, std::min(c0 + kChunk, it->a + it->n)) - x;
            uint8_t* dst = reinterpret_cast<uint8_t*>(x);
            if (it->where[c] == ~0ull) {
                std::memset(dst, 0, k);
            } else {
                buf.resize(static_cast<size_t>(k));
                _fseeki64(f, static_cast<int64_t>(it->where[c] + (x - c0)), SEEK_SET);
                if (std::fread(buf.data(), 1, k, f) == k) std::memcpy(dst, buf.data(), k);
            }
            done += k;
            x += k;
        }
    }
    return done;
}

bool load_capture(const stdfs::path& dir, Loaded& L, std::string* err) {
    std::vector<uint8_t> data;
    if (!read_whole(dir / "capture.bin", data)) { *err = "cannot read " + u8s(dir / "capture.bin"); return false; }
    In in(std::move(data));
    char magic[8];
    in.raw(magic, 8);
    if (std::memcmp(magic, kMagic, 8) != 0) { *err = "not a frame capture (magic)"; return false; }
    const uint32_t ver = in.get<uint32_t>(), gsize = in.get<uint32_t>();
    if (ver != kVersion && ver != 2) { *err = "capture version " + std::to_string(ver) + ", this build reads 2 and " + std::to_string(kVersion); return false; }
    g_read_version = ver;
    if (gsize != gpu::replay_state_size()) {
        *err = "the capture was written by another runtime build (GPU state " + std::to_string(gsize) + " bytes, this build " +
               std::to_string(gpu::replay_state_size()) + "): capture the frame again";
        return false;
    }
    const std::string program_id = in.str();
    NXLOG_I(kTag, "capture of program %s", program_id.c_str());
    if (in.get<uint32_t>() != sizeof(TextureBuilderInfo) || in.get<uint32_t>() != sizeof(TextureViewInfo) || in.get<uint32_t>() != sizeof(SamplerInfo) ||
        in.get<uint32_t>() != sizeof(WindowHdrInfo)) {
        *err = "the capture was written by another runtime build (object layouts differ): capture the frame again";
        return false;
    }

    // pools (virtual maps after all pools exist)
    struct VM { size_t pool; uint64_t voff, size, phys; int64_t poff; };
    std::vector<VM> vms;
    const uint64_t npools = in.get<uint64_t>();
    for (uint64_t i = 0; i < npools && in.ok(); ++i) {
        auto* p = new MemoryPool();
        p->base = reinterpret_cast<uint8_t*>(in.get<uint64_t>());
        p->size = in.get<uint64_t>();
        p->flags = in.get<int32_t>();
        p->gpu_base = in.get<uint64_t>();
        p->is_virtual = in.get<uint8_t>() != 0;
        p->is_shader_code = in.get<uint8_t>() != 0;
        const uint32_t nv = in.get<uint32_t>();
        for (uint32_t k = 0; k < nv && in.ok(); ++k) {
            VM v{static_cast<size_t>(i), in.get<uint64_t>(), in.get<uint64_t>(), in.get<uint64_t>(), in.get<int64_t>()};
            vms.push_back(v);
        }
        L.pools.emplace_back(p);
    }
    for (const VM& v : vms) {
        MemoryPool* phys = at(L.pools, v.phys);
        if (!phys) continue;
        MemoryPool::VirtualMap m;
        m.voff = v.voff;
        m.size = v.size;
        m.phys = Ref<MemoryPool>::share(phys);
        m.poff = v.poff;
        L.pools[v.pool]->vmaps.push_back(std::move(m));
    }
    // textures
    const uint64_t ntex = in.get<uint64_t>();
    uint64_t bad_tex = 0;
    for (uint64_t i = 0; i < ntex && in.ok(); ++i) {
        TextureBuilderInfo b;
        in.raw(&b, sizeof b);
        const uint64_t memp = in.get<uint64_t>(), vpool = in.get<uint64_t>(), voff = in.get<uint64_t>();
        const uint32_t id = in.get<uint32_t>();
        const bool coherent = in.get<uint8_t>() != 0;
        (void)in.get<uint8_t>();   // destroyed by the game at capture time (still referenced): made all the same
        const TexLayout lay = compute_texture_layout(b);
        auto* t = new Texture();
        t->b = b;
        if (lay.valid) {
            t->fmt = lay.fmt;
            t->storage_size = lay.storage_size;
            t->block_linear = lay.block_linear;
            t->block_height_gobs = lay.block_linear ? lay.layout.levels[0].gob_blocks_y : 0;
            t->levels = lay.levels;
            t->layers = lay.layers;
        } else {
            ++bad_tex;
        }
        t->mem = reinterpret_cast<uint8_t*>(memp);
        if (MemoryPool* vp = at(L.pools, vpool)) t->vpool = Ref<MemoryPool>::share(vp);
        t->voffset = voff;
        t->id = id;
        t->is_display = (b.flags & NVN_TEXTURE_FLAGS_DISPLAY) != 0;
        t->cpu_coherent = coherent;
        L.texs.emplace_back(t);
    }
    if (bad_tex) NXLOG_W(kTag, "%llu textures have no layout", static_cast<unsigned long long>(bad_tex));
    // texture pools
    const uint64_t ntp = in.get<uint64_t>();
    for (uint64_t i = 0; i < ntp && in.ok(); ++i) {
        auto* tp = new TexturePool();
        tp->mem = at(L.pools, in.get<uint64_t>());
        if (tp->mem) tp->mem->ref();   // as nvnTexturePoolInitialize (never released here)
        tp->offset = in.get<int64_t>();
        tp->count = in.get<int32_t>();
        tp->entries.resize(static_cast<size_t>(std::max(0, tp->count)));
        const uint32_t used = in.get<uint32_t>();
        for (uint32_t k = 0; k < used && in.ok(); ++k) {
            const uint32_t idx = in.get<uint32_t>();
            Texture* t = at(L.texs, in.get<uint64_t>());
            TextureViewInfo v;
            in.raw(&v, sizeof v);
            const bool hv = in.get<uint8_t>() != 0;
            if (idx >= tp->entries.size()) continue;
            auto& e = tp->entries[idx];
            e.tex = Ref<Texture>::share(t);
            e.view = v;
            e.has_view = hv;
            e.used = true;
        }
        L.tpools.emplace_back(tp);
    }
    // sampler pools
    const uint64_t nsp = in.get<uint64_t>();
    for (uint64_t i = 0; i < nsp && in.ok(); ++i) {
        auto* sp = new SamplerPool();
        sp->mem = at(L.pools, in.get<uint64_t>());
        if (sp->mem) sp->mem->ref();
        sp->offset = in.get<int64_t>();
        sp->count = in.get<int32_t>();
        sp->entries.resize(static_cast<size_t>(std::max(0, sp->count)));
        sp->used.resize(static_cast<size_t>(std::max(0, sp->count)), 0);
        const uint32_t used = in.get<uint32_t>();
        for (uint32_t k = 0; k < used && in.ok(); ++k) {
            const uint32_t idx = in.get<uint32_t>();
            SamplerInfo s;
            in.raw(&s, sizeof s);
            if (idx >= sp->entries.size()) continue;
            sp->entries[idx] = s;
            sp->used[idx] = 1;
        }
        L.spools.emplace_back(sp);
    }
    // programs (their shaders are looked up after the memory is back)
    struct ProgImg { std::string label; uint64_t code[kCapStages] = {}, control[kCapStages] = {}; std::string hash[kCapStages]; uint32_t sample_shading; std::vector<std::pair<uint64_t, uint64_t>> pending; };
    std::vector<ProgImg> pimgs;
    const uint64_t nprog = in.get<uint64_t>();
    for (uint64_t i = 0; i < nprog && in.ok(); ++i) {
        ProgImg pi;
        pi.label = in.str();
        for (int s = 0; s < kCapStages; ++s) {
            pi.code[s] = in.get<uint64_t>();
            pi.control[s] = in.get<uint64_t>();
            pi.hash[s] = in.str();
        }
        pi.sample_shading = in.get<uint32_t>();
        const uint32_t npend = g_read_version >= 3 ? in.get<uint32_t>() : 0;   // version 2 has no pending blocks
        for (uint32_t k = 0; k < npend && in.ok(); ++k) {
            const uint64_t code = in.get<uint64_t>();
            pi.pending.emplace_back(code, in.get<uint64_t>());
        }
        pimgs.push_back(std::move(pi));
    }
    const uint64_t nsync = in.get<uint64_t>();
    for (uint64_t i = 0; i < nsync; ++i) L.syncs.emplace_back(new Sync());
    const uint64_t nev = in.get<uint64_t>();
    for (uint64_t i = 0; i < nev && in.ok(); ++i) {
        auto* e = new Event();
        e->pool = at(L.pools, in.get<uint64_t>());
        e->offset = in.get<int64_t>();
        L.evs.emplace_back(e);
    }
    const uint64_t nwin = in.get<uint64_t>();
    for (uint64_t i = 0; i < nwin && in.ok(); ++i) {
        auto* wn = new Window();
        const uint32_t nt = in.get<uint32_t>();
        for (uint32_t k = 0; k < nt; ++k) wn->textures.push_back(Ref<Texture>::share(at(L.texs, in.get<uint64_t>())));
        wn->present_interval = in.get<int32_t>();
        wn->active = in.get<int32_t>();
        in.raw(wn->crop, sizeof wn->crop);
        in.raw(&wn->hdr, sizeof wn->hdr);
        L.wins.emplace_back(wn);
    }
    // lists: bytes first (they refer to each other through CallCommands)
    const uint64_t nlist = in.get<uint64_t>();
    std::vector<std::vector<uint8_t>> lbytes;
    for (uint64_t i = 0; i < nlist && in.ok(); ++i) {
        auto* l = new CmdList();
        l->record_count = in.get<uint32_t>();
        l->draw_count = in.get<uint32_t>();
        lbytes.push_back(in.vec());
        L.draws += l->draw_count;
        L.lists.emplace_back(l);
    }
    const uint64_t nevents = in.get<uint64_t>();
    for (uint64_t i = 0; i < nevents && in.ok(); ++i) {
        Loaded::Ev e;
        e.kind = in.get<int32_t>();
        const uint32_t n = in.get<uint32_t>();
        for (uint32_t k = 0; k < n; ++k) if (CmdList* l = at(L.lists, in.get<uint64_t>())) e.lists.push_back(Ref<CmdList>::share(l));
        e.win = Ref<Window>::share(at(L.wins, in.get<uint64_t>()));
        e.index = in.get<int32_t>();
        L.events.push_back(std::move(e));
    }
    L.gpod = in.vec();
    const uint64_t g_tpool = in.get<uint64_t>(), g_spool = in.get<uint64_t>(), g_depth = in.get<uint64_t>();
    uint64_t g_color[8];
    for (auto& c : g_color) c = in.get<uint64_t>();
    std::string g_stage[kCapStages];
    for (auto& h : g_stage) h = in.str();

    // memory
    using Rng = Loaded::Rng;
    std::vector<Rng>& rngs = L.rngs;
    const uint64_t nr = in.get<uint64_t>();
    for (uint64_t i = 0; i < nr && in.ok(); ++i) {
        Rng r{in.get<uint64_t>(), in.get<uint64_t>(), {}};
        r.where.resize((r.n + kChunk - 1) / kChunk);
        for (auto& x : r.where) x = in.get<uint64_t>();
        rngs.push_back(std::move(r));
    }
    if (!in.ok()) { *err = "capture.bin is truncated"; return false; }
    if (in.more()) {
        const int32_t origin = in.get<int32_t>(), depth = in.get<int32_t>();
        gpu::set_window_modes(origin, depth);
        NXLOG_I(kTag, "device modes: window origin %d, depth mode %d", origin, depth);
        const uint64_t nimg = in.more() ? in.get<uint64_t>() : 0;
        for (uint64_t i = 0; i < nimg && in.ok(); ++i) {
            Loaded::Img m;
            m.tex = at(L.texs, in.get<uint64_t>());
            m.d.format = static_cast<VkFormat>(in.get<int32_t>());
            m.d.base = in.get<VkExtent3D>();
            m.d.levels = in.get<uint32_t>();
            m.d.layers = in.get<uint32_t>();
            m.d.aspect = in.get<uint32_t>();
            m.d.scale = in.get<float>();
            m.off = in.get<uint64_t>();
            m.size = in.get<uint64_t>();
            if (m.tex) L.images.push_back(std::move(m));
        }
    } else {
        NXLOG_W(kTag, "the capture has no device modes (an older capture): the defaults are used, the picture may be upside down");
    }

    {
        FILE* f = _wfopen((dir / "memory.bin").c_str(), L"rb");
        if (!f) { *err = "cannot read memory.bin"; return false; }
        uint64_t total = 0;
        for (const Rng& r : rngs) {
            if (!restore_range(r.a, r.n)) {
                std::fclose(f);
                char b[160];
                std::snprintf(b, sizeof b, "cannot map guest memory %llx (+%llx) at its address (error %lu)", static_cast<unsigned long long>(r.a),
                              static_cast<unsigned long long>(r.n), static_cast<unsigned long>(GetLastError()));
                *err = b;
                return false;
            }
            for (size_t c = 0; c < r.where.size(); ++c) {
                const uint64_t off = c * kChunk, k = std::min(kChunk, r.n - off);
                uint8_t* dst = reinterpret_cast<uint8_t*>(r.a + off);
                if (r.where[c] == ~0ull) { std::memset(dst, 0, k); continue; }
                _fseeki64(f, static_cast<int64_t>(r.where[c]), SEEK_SET);
                if (std::fread(dst, 1, k, f) != k) { std::fclose(f); *err = "memory.bin is truncated"; return false; }
            }
            total += r.n;
        }
        std::fclose(f);
        NXLOG_I(kTag, "memory restored: %zu ranges, %.2f GiB", rngs.size(), total / 1073741824.0);
    }

    // objects that need the memory: pools are registered, textures enter the registry, programs find their shaders
    for (auto& p : L.pools) pool_register(p.get());
    for (auto& p : L.pools) if (!p->is_virtual && p->base) gpu::prefetch_pool(p.get());
    for (auto& t : L.texs) {
        if (!t->fmt) continue;
        texture_registry_add(t.get());
        texture_registry_add_virtual(t.get());
    }
    ShaderCache& cache = shader_cache();
    uint64_t pending = 0, missing = 0;
    for (const ProgImg& pi : pimgs) {
        auto* p = new Program();
        p->label = pi.label;
        p->sample_shading = pi.sample_shading;
        bool all = true;
        for (int s = 0; s < kCapStages; ++s) {
            if (!pi.code[s] || !pi.control[s]) continue;
            const uint8_t* code = reinterpret_cast<const uint8_t*>(pi.code[s]);
            const uint8_t* control = reinterpret_cast<const uint8_t*>(pi.control[s]);
            ShaderCache::FindMiss miss;
            const ShaderInfo* si = cache.find(code, control, &miss);
            if (!si && miss.pending) {
                p->pending.push_back(Program::PendingBlock{code, control, miss.hash});
                ++pending;
                all = false;
                continue;
            }
            if (!si) { ++missing; p->missing_hard = true; all = false; continue; }
            p->stage[s] = si;
            p->code_hash[s] = std::strtoull(si->hash.c_str(), nullptr, 16);
            p->code_addr[s] = pi.code[s];
            p->control_addr[s] = pi.control[s];
        }
        for (const auto& [code_a, control_a] : pi.pending) {   // blocks still being translated at capture time (format 3)
            const uint8_t* code = reinterpret_cast<const uint8_t*>(code_a);
            const uint8_t* control = reinterpret_cast<const uint8_t*>(control_a);
            ShaderCache::FindMiss miss;
            const ShaderInfo* si = cache.find(code, control, &miss);
            if (!si && !miss.pending) { ++missing; p->missing_hard = true; all = false; continue; }
            p->pending.push_back(Program::PendingBlock{code, control, si ? si->hash : miss.hash});
            if (!si) ++pending;
            all = false;
        }
        p->has_pending.store(!p->pending.empty());
        p->linked = all;
        if (!p->pending.empty()) program_resolve_pending(p);   // the ones the cache has by now get their stage at once
        if (p->linked) gpu::prewarm_program_shaders(p->stage);
        L.progs.emplace_back(p);
    }
    NXLOG_I(kTag, "%zu programs: %llu stages being translated, %llu missing", L.progs.size(), static_cast<unsigned long long>(pending),
            static_cast<unsigned long long>(missing));

    // patch the lists: tagged ids -> objects, list ids -> handles of the called lists
    std::vector<uint64_t> handle_of(L.lists.size(), 0);
    auto handle = [&](uint64_t id) -> uint64_t {
        if (!id || id > L.lists.size()) return 0;
        if (!handle_of[id - 1]) handle_of[id - 1] = handle_register(Ref<CmdList>::share(L.lists[id - 1].get()));
        return handle_of[id - 1];
    };
    for (size_t i = 0; i < L.lists.size(); ++i) {
        CmdList* l = L.lists[i].get();
        std::vector<uint8_t>& b = lbytes[i];
        for_each_record(b, [&](Op op, uint8_t* payload, uint32_t size) {
            if (op == Op::CallCommands) {
                const auto* p = reinterpret_cast<const P_CallCommands*>(payload);
                uint64_t* h = reinterpret_cast<uint64_t*>(payload + payload_size_of_call());
                if (payload_size_of_call() + static_cast<uint64_t>(p->count) * 8 > size) return;
                for (uint32_t j = 0; j < p->count; ++j) h[j] = handle(h[j]);
                return;
            }
            for_each_ptr(op, payload, [&](uint64_t* slot) {
                const uint64_t tag = *slot, id = tag & ((1ull << 56) - 1);
                Obj* o = nullptr;
                switch (static_cast<K>(tag >> 56)) {
                    case kPool: o = at(L.pools, id); break;
                    case kTex: o = at(L.texs, id); break;
                    case kTpool: o = at(L.tpools, id); break;
                    case kSpool: o = at(L.spools, id); break;
                    case kProg: o = at(L.progs, id); break;
                    case kSync: o = at(L.syncs, id); break;
                    case kEvent: o = at(L.evs, id); break;
                    case kWin: o = at(L.wins, id); break;
                    case kList: o = at(L.lists, id); break;
                    default: break;
                }
                *slot = reinterpret_cast<uint64_t>(o);
                l->hold(o);
            });
        });
        l->bytes = std::move(b);
    }

    restore_images(dir, L, true);

    // GPU state
    L.grefs.tpool = at(L.tpools, g_tpool);
    L.grefs.spool = at(L.spools, g_spool);
    L.grefs.depth = at(L.texs, g_depth);
    for (int i = 0; i < 8; ++i) L.grefs.color[i] = at(L.texs, g_color[i]);
    for (int s = 0; s < kCapStages; ++s) L.grefs.stage[s] = g_stage[s].empty() ? nullptr : cache.find_by_hash(g_stage[s]);
    return true;
}

}  // namespace

int run_replay(const std::string& folder) {
    g_replaying.store(true);
    const stdfs::path dir = u8p(folder);
    Loaded L;
    std::string err;
    const auto t0 = std::chrono::steady_clock::now();
    if (!load_capture(dir, L, &err)) {
        NXLOG_E(kTag, "replay of %s: %s", folder.c_str(), err.c_str());
        return 1;
    }
    size_t presents = 0, submits = 0;
    for (auto& e : L.events) (e.kind ? presents : submits)++;
    NXLOG_I(kTag, "replaying %s: %zu submissions, %zu present(s), %zu lists, %llu draws (loaded in %.1f s)", folder.c_str(), submits, presents,
            L.lists.size(), static_cast<unsigned long long>(L.draws), std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    if (!presents) NXLOG_W(kTag, "the capture has no present: nothing will be shown");

    // [capture] reset (default on): every loop starts from the captured memory and images, so each shows the same frame; off: a loop builds on
    // what the previous one left, as frames do in the game (effects that accumulate, such as exposure adaptation, then drift without the game).
    const bool reset = config_get_bool("capture", "reset", true);
    std::vector<std::pair<uint64_t, uint64_t>> writes;
    if (reset) g_collect_writes.store(true);
    const int64_t max_loops = config_get_int("capture", "loops", 0);
    const int64_t max_seconds = config_get_int("boot", "max_seconds", 0);
    const auto start = std::chrono::steady_clock::now();
    uint64_t loop = 0;
    for (;;) {
        if (reset && loop >= 1) {
            // The ranges the GPU writes, as far as the loops so far have shown them. Collected in every loop, not only the first: a dispatch
            // or draw left out of the first loops (its pipeline still building on a background thread, its shader being translated) writes
            // its ranges only later, and a range missing here keeps what the previous loop left (the culling counters of the mesh draws
            // then added up loop after loop).
            gpu::finish();
            std::vector<std::pair<uint64_t, uint64_t>> got;
            {
                std::lock_guard lk(g_writes_mu);
                got.swap(g_writes);
            }
            if (!got.empty()) {
                got.insert(got.end(), writes.begin(), writes.end());
                std::sort(got.begin(), got.end());
                const size_t before = writes.size();
                writes.clear();
                for (auto& r : got) {
                    if (!writes.empty() && r.first <= writes.back().second) writes.back().second = std::max(writes.back().second, r.second);
                    else writes.push_back(r);
                }
                uint64_t total = 0;
                for (auto& r : writes) total += r.second - r.first;
                if (loop == 1 || writes.size() != before)
                    NXLOG_I(kTag, "the frame writes %zu ranges of pool memory (%.1f MiB): put back before every loop", writes.size(), total / 1048576.0);
            }
        }
        // [capture] reset_full = 1 (diagnosis, A18): every loop puts back all captured memory, not only the ranges seen written (a
        // determinism check: a frame that still differs from loop to loop is not deterministic on the GPU)
        static const bool reset_full = config_get_bool("capture", "reset_full", false);
        if (reset && loop >= 1) {
            gpu::finish();
            if (FILE* f = _wfopen((dir / "memory.bin").c_str(), L"rb")) {
                std::vector<uint8_t> buf;
                if (reset_full)
                    for (const auto& r : L.rngs) restore_memory(f, L, r.a, r.a + r.n, buf);
                else
                    for (auto& r : writes) restore_memory(f, L, r.first, r.second, buf);
                std::fclose(f);
            }
            restore_images(dir, L, false);
        }
        // every loop starts from the captured GPU state
        gpu::run_on_render_thread([&] { gpu::replay_state_restore(L.gpod, L.grefs); });
        for (auto& e : L.events) {
            if (e.kind == 0) gpu::submit(e.lists);
            else if (e.win) gpu::present(e.win.get(), e.index);
            if (platform::initialised() && platform::has_window()) platform::pump_events();
        }
        ++loop;
        if (loop == 1 || loop % 300 == 0) NXLOG_I(kTag, "loop %llu", static_cast<unsigned long long>(loop));
        if (max_loops > 0 && static_cast<int64_t>(loop) >= max_loops) break;
        if (max_seconds > 0 && std::chrono::steady_clock::now() - start > std::chrono::seconds(max_seconds)) break;
        if (platform::initialised() && platform::quit_requested()) break;
    }
    gpu::finish();
    NXLOG_I(kTag, "replay ended after %llu loops", static_cast<unsigned long long>(loop));
    return 0;
}

}  // namespace nxrt::nvn::be::capture

namespace nxrt::nvn {
void request_frame_capture(const std::string& name) { be::capture::request(name); }
int run_capture_replay(const std::string& folder) { return be::capture::run_replay(folder); }
}  // namespace nxrt::nvn
