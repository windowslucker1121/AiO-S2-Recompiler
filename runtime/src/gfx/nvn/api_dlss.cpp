// SPDX-License-Identifier: GPL-3.0-or-later
// NVN2 DLSS (family dlss): the 57 nvnDlss* functions and nvnCommandBufferEvaluateDlss, which a platform 2 title calls to upscale with the console's own
// DLSS. None of them is in the NVN1 table, so the game fetches them by name and NVN2_REG hands the thunks out (nvn_loader.cpp).
//
// What each one does here:
//   - the three objects (NVNdlssNetworkBuilder 0xB8 bytes, NVNdlssNetwork 0x220, NVNdlssParameters 0xC0) are plain data in the game's memory, laid out
//     exactly as the sdk lays them out, so every Set/Get pair round-trips and nothing needs a host object;
//   - nvnDlssNetworkInitialize / Resize / Finalize and the optimal-settings query answer for the console's DLSS network: the render size comes
//     from [upscale] (docs/S14.md, "Native DLSS calls"), there is no network to build, the storage the game hands over is not used;
//   - nvnCommandBufferEvaluateDlss is recorded (Op::EvaluateDlss) and replayed by nvn_replay.cpp: DLSS or FSR 3 on the game's colour, depth and
//     motion vectors when [upscale] mode asks for one and its SDK is there, else a plain bilinear scale of the colour rectangle into the output.
//
// Evidence for the layouts (SDK SDK_NVN-20_5_25, the game's own `sdk` module read in IDA/Hex-Rays, docs/nvn2_abi.md section 3; addresses are
// the sdk's): builder = 0x5DF290-0x5DF430 (SetDefaults clears +8..+0xB8 then max evaluation entries +0x34 = 1) and the storage-size function 0x5DE620;
// network = 0x5DE390 (Initialize: memset 0x220, copies the builder into it); parameters = 0x5DF440-0x5DF6E0 (SetDefaults) and the record
// nvnCommandBufferEvaluateDlss 0x5DEBC0 reads. Offsets are listed beside the accessors below. Not proven by code: the argument order of the six
// size pointers of GetOptimalSettings (taken from NGX's NGX_DLSS_GET_OPTIMAL_SETTINGS, the same order Fire Emblem Fortune's Weave's caller uses
// per refs/nxnative hle_nvn.c) and the meaning of the builder method values (0: up to 2x, 1: 3x, 2: 1x, from the same source).
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>

#include "extras/temporal.h"
#include "nvn_cmd.h"
#include "nvn_impl.h"
#include "nxrt/extras.h"
#include "nxrt/log.h"

namespace nxrt::nvn {
using namespace be;
namespace {

constexpr const char* kTag = "dlss";

// ---- plain data access ---------------------------------------------------------------------------------------------------------
template <class T>
T get(const void* base, size_t off) {
    T v;
    std::memcpy(&v, static_cast<const uint8_t*>(base) + off, sizeof v);
    return v;
}
template <class T>
void put(void* base, size_t off, T v) {
    std::memcpy(static_cast<uint8_t*>(base) + off, &v, sizeof v);
}

// NVNdlssNetworkBuilder (sdk 0x5DF290..0x5DF430)
constexpr size_t kBuilderSize = 0xB8;
enum : size_t {
    kBDevice = 0,        // NVNdevice*
    kBTexturePool = 8,   // NVNtexturePool* the texture ids are slots of
    kBStorage = 16,      // storage the game hands over (nvnDlssNetworkBuilderSetStorage)
    kBStorageSize = 24,
    kBFlags = 32,        // NGX feature flags, see below
    kBMethod = 36,
    kBPreset = 40,
    kBOutW = 44,
    kBOutH = 48,
    kBMaxEval = 52,      // frames in flight the network keeps state for (default 1)
    kBTextureIds = 56,   // 32 x int32: the texture pool slots the network binds its inputs to (128 bytes)
};
// The flags are NGX's NVSDK_NGX_DLSS_Feature_Flags: nvnDlssNetworkInitialize masks them with 0x4E and forces bit 0 before it hands them to the
// driver (sdk 0x5DE390), and bits 0x10000000 / 0x20000000 steer the storage size. Bit 0 is HDR colour, 2 motion vectors at the render size
// (else at the output size), 4 motion vectors that include the jitter, 8 depth reversed (1 is near), 0x40 auto exposure.
enum : uint32_t { kFlagHdr = 1, kFlagMvLowRes = 2, kFlagMvJittered = 4, kFlagDepthInverted = 8, kFlagAutoExposure = 0x40 };

// NVNdlssNetwork (0x220 bytes, sdk 0x5DE390): the fields Initialize fills. Offsets are the sdk's; +0x1D0 is ours (a counter, unused by the sdk).
constexpr size_t kNetworkSize = 0x220;
enum : size_t {
    kNDevice = 8,
    kNTexturePool = 16,
    kNTextureIds = 24,        // 32 x int32 copied from the builder; EvaluateDlss writes the descriptors of its six inputs to ids [0..5]:
                              // input colour, depth, motion vectors, bias-current-colour mask, output colour, exposure (sdk 0x5DEBC0)
    kNMask = 152,             // 63 (six inputs)
    kNHandle = 160,           // the driver's network handle: non-zero while the network is made
    kNStorage = 176,
    kNStorageSize = 184,
    kNMaxEval = 456,
    kNSerial = 464,           // ours: bumped by every Initialize, so a network made again starts a new history
    kNMethod = 468,
    kNFeatureFlags = 472,     // builder flags & 0x4E, bit 0 forced
    kNPreset = 480,
    kNOptW = 484, kNOptH = 488,
    kNOutW = 492, kNOutH = 496,
    kNMaxW = 504, kNMaxH = 508, kNMinW = 512, kNMinH = 516,   // the largest and smallest input size
    kNCurOutW = 520, kNCurOutH = 524,                           // nvnDlssNetworkResize changes these
    kNLimitOutW = 528, kNLimitOutH = 532,                       // the size the network was made for: Resize cannot go beyond it
    kNBuilderFlags = 536,
};

// NVNdlssParameters (0xC0 bytes, sdk 0x5DF440..0x5DF6E0). A texture is a pair (const NVNtexture*, const NVNtextureView* or null).
constexpr size_t kParamsSize = 0xC0;
enum : size_t {
    kPInputColor = 0, kPOutputColor = 16, kPMotion = 32, kPDepth = 48, kPBiasMask = 64,
    kPJitter = 80,            // float x, y
    kPMvScale = 88,           // float x, y (default 1, 1)
    kPPreExposure = 96,       // float (default 1)
    kPExposure = 104,         // texture pair
    kPHistoryReset = 120,     // u8
    kPEvalIndex = 124,
    kPInputSize = 128,        // int32 w, h (0, 0: the whole input texture)
    kPColorBase = 136, kPDepthBase = 144, kPMvBase = 152, kPBiasBase = 160, kPOutBase = 168,   // int32 x, y
    kPOverlayDebug = 176, kPOverlayFull = 180, kPOverlayNans = 184, kPOverlayJitter = 188,     // int32, default -1
};

std::atomic<uint32_t> g_serial{0};

// ---- the optimal render size ---------------------------------------------------------------------------------------------------
// The console's DLSS renders at a ratio of the output that depends on the method; here the ratio follows [upscale] (docs/S14.md): with fsr3, dlss
// or dlaa selected, the quality preset (method 1, "ultra performance", asks for a third at most; method 2 and dlaa keep the output size); with
// no temporal mode the render size is the output size, so the plain scale has nothing to enlarge.
void optimal_size(int out_w, int out_h, int method, int32_t* w, int32_t* h) {
    int pct = 100;
    if (extras::upscale_temporal() && method != 2) {
        pct = extras::upscale_scene_scale();
        if (method == 1) pct = std::min(pct, 33);
    }
    pct = std::clamp(pct, 20, 100);
    uint32_t sw = 0, sh = 0;   // Streamline's own answer for the mode nearest to the quality, when DLSS runs
    if (pct < 100 && extras::temporal_dlss_optimal_size(static_cast<uint32_t>(out_w), static_cast<uint32_t>(out_h), pct, &sw, &sh) && sw && sh) {
        *w = static_cast<int32_t>(sw);
        *h = static_cast<int32_t>(sh);
        return;
    }
    *w = std::max(1, static_cast<int32_t>(std::ceil(out_w * pct / 100.0)));
    *h = std::max(1, static_cast<int32_t>(std::ceil(out_h * pct / 100.0)));
}

// ---- NVNdlssNetworkBuilder -----------------------------------------------------------------------------------------------------
void nvnDlssNetworkBuilderSetDefaults(void* b) {
    if (!b) return;
    std::memset(static_cast<uint8_t*>(b) + kBTexturePool, 0, kBuilderSize - kBTexturePool);   // the device (+0) is kept, as the sdk does
    put<int32_t>(b, kBMaxEval, 1);
}
void nvnDlssNetworkBuilderSetDevice(void* b, void* device) { if (b) put<uint64_t>(b, kBDevice, reinterpret_cast<uint64_t>(device)); }
uint64_t nvnDlssNetworkBuilderGetDevice(const void* b) { return b ? get<uint64_t>(b, kBDevice) : 0; }
void nvnDlssNetworkBuilderSetFlags(void* b, uint32_t flags) { if (b) put<uint32_t>(b, kBFlags, flags); }
uint32_t nvnDlssNetworkBuilderGetFlags(const void* b) { return b ? get<uint32_t>(b, kBFlags) : 0; }
void nvnDlssNetworkBuilderSetMethod(void* b, int method) { if (b) put<int32_t>(b, kBMethod, method); }
uint32_t nvnDlssNetworkBuilderGetMethod(const void* b) { return b ? get<uint32_t>(b, kBMethod) : 0; }
void nvnDlssNetworkBuilderSetPreset(void* b, int preset) { if (b) put<int32_t>(b, kBPreset, preset); }
uint32_t nvnDlssNetworkBuilderGetPreset(const void* b) { return b ? get<uint32_t>(b, kBPreset) : 0; }
void nvnDlssNetworkBuilderSetOutputResolution(void* b, int w, int h) {
    if (!b) return;
    put<int32_t>(b, kBOutW, w);
    put<int32_t>(b, kBOutH, h);
}
void nvnDlssNetworkBuilderGetOutputResolution(const void* b, int* w, int* h) {
    if (!b) return;
    if (w) *w = get<int32_t>(b, kBOutW);
    if (h) *h = get<int32_t>(b, kBOutH);
}
void nvnDlssNetworkBuilderSetStorage(void* b, void* memory, size_t size) {
    if (!b) return;
    put<uint64_t>(b, kBStorage, reinterpret_cast<uint64_t>(memory));
    put<uint64_t>(b, kBStorageSize, size);
}
uint64_t nvnDlssNetworkBuilderGetMemory(const void* b) { return b ? get<uint64_t>(b, kBStorage) : 0; }
uint64_t nvnDlssNetworkBuilderGetMemorySize(const void* b) { return b ? get<uint64_t>(b, kBStorageSize) : 0; }
void nvnDlssNetworkBuilderSetMaxEvaluationEntries(void* b, int n) { if (b) put<int32_t>(b, kBMaxEval, n); }
uint32_t nvnDlssNetworkBuilderGetMaxEvaluationEntries(const void* b) { return b ? get<uint32_t>(b, kBMaxEval) : 0; }
void nvnDlssNetworkBuilderSetTextureIDs(void* b, void* pool, const int* ids) {
    if (!b) return;
    put<uint64_t>(b, kBTexturePool, reinterpret_cast<uint64_t>(pool));
    if (ids) std::memcpy(static_cast<uint8_t*>(b) + kBTextureIds, ids, 32 * sizeof(int32_t));
}
void nvnDlssNetworkBuilderGetTextureIDs(const void* b, int* ids) {
    if (b && ids) std::memcpy(ids, static_cast<const uint8_t*>(b) + kBTextureIds, 32 * sizeof(int32_t));
}
uint64_t nvnDlssNetworkBuilderGetTexturePool(const void* b) { return b ? get<uint64_t>(b, kBTexturePool) : 0; }

// The storage the game must hand over (nvnDlssNetworkBuilderSetStorage). The console's network keeps its weights and intermediate images there; this
// back end uses none of it, so one page is enough: the sdk's own answer is also a multiple of 4 KiB.
uint64_t nvnDlssNetworkBuilderGetStorageSize(const void* b) {
    (void)b;
    return 0x1000;
}

// (builder, int* optimal w, h, int* largest w, h, int* smallest w, h): the order of NGX's NGX_DLSS_GET_OPTIMAL_SETTINGS. The sdk passes six pointers on
// (0x5DE330 calls the driver with the output size and method from the builder, then x1..x6); a game that passes a seventh (sharpness) is not read.
bool nvnDlssNetworkBuilderGetOptimalSettings(const void* b, int* opt_w, int* opt_h, int* max_w, int* max_h, int* min_w, int* min_h) {
    if (!b) return false;
    const int out_w = get<int32_t>(b, kBOutW), out_h = get<int32_t>(b, kBOutH);
    if (out_w <= 0 || out_h <= 0) return false;
    int32_t w = 0, h = 0;
    optimal_size(out_w, out_h, get<int32_t>(b, kBMethod), &w, &h);
    // one size per query: the game may not choose a smaller or larger input than the preset gives
    for (int* p : {opt_w, max_w, min_w}) if (p) *p = w;
    for (int* p : {opt_h, max_h, min_h}) if (p) *p = h;
    return true;
}

// ---- NVNdlssNetwork ------------------------------------------------------------------------------------------------------------
void network_fill_sizes(void* net, int out_w, int out_h) {
    int32_t w = 0, h = 0;
    optimal_size(out_w, out_h, get<int32_t>(net, kNMethod), &w, &h);
    put<int32_t>(net, kNOptW, w);
    put<int32_t>(net, kNOptH, h);
    put<int32_t>(net, kNMaxW, w);
    put<int32_t>(net, kNMaxH, h);
    put<int32_t>(net, kNMinW, w);
    put<int32_t>(net, kNMinH, h);
}

bool nvnDlssNetworkInitialize(void* net, const void* b) {
    if (!net || !b) return false;
    const int out_w = get<int32_t>(b, kBOutW), out_h = get<int32_t>(b, kBOutH);
    if (out_w <= 0 || out_h <= 0) {
        NXLOG_W(kTag, "nvnDlssNetworkInitialize: the builder has no output resolution (%d x %d)", out_w, out_h);
        return false;
    }
    std::memset(net, 0, kNetworkSize);
    put<uint64_t>(net, kNDevice, get<uint64_t>(b, kBDevice));
    put<uint64_t>(net, kNTexturePool, get<uint64_t>(b, kBTexturePool));
    std::memcpy(static_cast<uint8_t*>(net) + kNTextureIds, static_cast<const uint8_t*>(b) + kBTextureIds, 32 * sizeof(int32_t));
    put<uint32_t>(net, kNMask, 63);
    put<uint64_t>(net, kNStorage, get<uint64_t>(b, kBStorage));
    put<uint64_t>(net, kNStorageSize, get<uint64_t>(b, kBStorageSize));
    put<int32_t>(net, kNMaxEval, get<int32_t>(b, kBMaxEval));
    const uint32_t serial = g_serial.fetch_add(1, std::memory_order_relaxed) + 1;
    put<uint32_t>(net, kNSerial, serial);
    put<uint64_t>(net, kNHandle, 0x444C535300000000ull | serial);   // "DLSS" and the serial: only its being non-zero is ever looked at
    put<int32_t>(net, kNMethod, get<int32_t>(b, kBMethod));
    put<int32_t>(net, kNPreset, get<int32_t>(b, kBPreset));
    const uint32_t flags = get<uint32_t>(b, kBFlags);
    put<uint32_t>(net, kNFeatureFlags, (flags & 0x4E) | 1);
    put<uint32_t>(net, kNBuilderFlags, flags);
    put<int32_t>(net, kNOutW, out_w);
    put<int32_t>(net, kNOutH, out_h);
    put<int32_t>(net, kNCurOutW, out_w);
    put<int32_t>(net, kNCurOutH, out_h);
    put<int32_t>(net, kNLimitOutW, out_w);
    put<int32_t>(net, kNLimitOutH, out_h);
    network_fill_sizes(net, out_w, out_h);
    static std::atomic<int> told{0};
    if (told.fetch_add(1) < 4)
        NXLOG_I(kTag, "DLSS network %d x %d, method %d, preset %d, flags 0x%x: render size %d x %d (mode %d)", out_w, out_h, get<int32_t>(b, kBMethod),
                get<int32_t>(b, kBPreset), flags, get<int32_t>(net, kNOptW), get<int32_t>(net, kNOptH), static_cast<int>(extras::upscale_mode()));
    return true;
}

void nvnDlssNetworkFinalize(void* net) {
    if (net) std::memset(net, 0, kNetworkSize);
}

// (network, output w, h): the output size changes up to the size the network was made for (the sdk compares with +528/+532, then asks the driver for
// the new render size and resizes the network).
bool nvnDlssNetworkResize(void* net, int w, int h) {
    if (!net || !get<uint64_t>(net, kNHandle)) return false;
    if (w <= 0 || h <= 0 || get<int32_t>(net, kNLimitOutW) < w || get<int32_t>(net, kNLimitOutH) < h) return false;
    put<int32_t>(net, kNCurOutW, w);
    put<int32_t>(net, kNCurOutH, h);
    put<int32_t>(net, kNOutW, w);
    put<int32_t>(net, kNOutH, h);
    network_fill_sizes(net, w, h);
    return true;
}

// ---- NVNdlssParameters ---------------------------------------------------------------------------------------------------------
void nvnDlssParametersSetDefaults(void* p) {
    if (!p) return;
    std::memset(p, 0, kParamsSize);
    put<float>(p, kPMvScale, 1.0f);
    put<float>(p, kPMvScale + 4, 1.0f);
    put<float>(p, kPPreExposure, 1.0f);
    for (size_t off : {kPOverlayDebug, kPOverlayFull, kPOverlayNans, kPOverlayJitter}) put<int32_t>(p, off, -1);
}

void set_texture(void* p, size_t off, const void* tex, const void* view) {
    if (!p) return;
    put<uint64_t>(p, off, reinterpret_cast<uint64_t>(tex));
    put<uint64_t>(p, off + 8, reinterpret_cast<uint64_t>(view));
}
void nvnDlssParametersSetInputColor(void* p, const void* tex, const void* view) { set_texture(p, kPInputColor, tex, view); }
void nvnDlssParametersSetOutputColor(void* p, const void* tex, const void* view) { set_texture(p, kPOutputColor, tex, view); }
void nvnDlssParametersSetMotionVectors(void* p, const void* tex, const void* view) { set_texture(p, kPMotion, tex, view); }
void nvnDlssParametersSetDepth(void* p, const void* tex, const void* view) { set_texture(p, kPDepth, tex, view); }
void nvnDlssParametersSetBiasCurrentColorMask(void* p, const void* tex, const void* view) { set_texture(p, kPBiasMask, tex, view); }
void nvnDlssParametersSetExposureTexture(void* p, const void* tex, const void* view) { set_texture(p, kPExposure, tex, view); }

void set_pair(void* p, size_t off, int a, int b) {
    if (!p) return;
    put<int32_t>(p, off, a);
    put<int32_t>(p, off + 4, b);
}
void get_pair(const void* p, size_t off, int* a, int* b) {
    if (!p) return;
    if (a) *a = get<int32_t>(p, off);
    if (b) *b = get<int32_t>(p, off + 4);
}
void nvnDlssParametersSetInputSubrectangleSize(void* p, int w, int h) { set_pair(p, kPInputSize, w, h); }
void nvnDlssParametersGetInputSubrectangleSize(const void* p, int* w, int* h) { get_pair(p, kPInputSize, w, h); }
void nvnDlssParametersSetInputColorSubrectangleBase(void* p, int x, int y) { set_pair(p, kPColorBase, x, y); }
void nvnDlssParametersGetInputColorSubrectangleBase(const void* p, int* x, int* y) { get_pair(p, kPColorBase, x, y); }
void nvnDlssParametersSetDepthSubrectangleBase(void* p, int x, int y) { set_pair(p, kPDepthBase, x, y); }
void nvnDlssParametersGetDepthSubrectangleBase(const void* p, int* x, int* y) { get_pair(p, kPDepthBase, x, y); }
void nvnDlssParametersSetMotionVectorSubrectangleBase(void* p, int x, int y) { set_pair(p, kPMvBase, x, y); }
void nvnDlssParametersGetMotionVectorSubrectangleBase(const void* p, int* x, int* y) { get_pair(p, kPMvBase, x, y); }
void nvnDlssParametersSetBiasCurrentColorSubrectangleBase(void* p, int x, int y) { set_pair(p, kPBiasBase, x, y); }
void nvnDlssParametersGetBiasCurrentColorSubrectangleBase(const void* p, int* x, int* y) { get_pair(p, kPBiasBase, x, y); }
void nvnDlssParametersSetOutputSubrectangleBase(void* p, int x, int y) { set_pair(p, kPOutBase, x, y); }
void nvnDlssParametersGetOutputSubrectangleBase(const void* p, int* x, int* y) { get_pair(p, kPOutBase, x, y); }

void nvnDlssParametersSetJitterOffset(void* p, float x, float y) {
    if (!p) return;
    put<float>(p, kPJitter, x);
    put<float>(p, kPJitter + 4, y);
}
float nvnDlssParametersGetJitterOffset(const void* p, float* x, float* y) {   // the sdk returns y as well
    if (!p) return 0.0f;
    const float fx = get<float>(p, kPJitter), fy = get<float>(p, kPJitter + 4);
    if (x) *x = fx;
    if (y) *y = fy;
    return fy;
}
void nvnDlssParametersSetMotionVectorScale(void* p, float x, float y) {
    if (!p) return;
    put<float>(p, kPMvScale, x);
    put<float>(p, kPMvScale + 4, y);
}
float nvnDlssParametersGetMotionVectorScale(const void* p, float* x, float* y) {
    if (!p) return 0.0f;
    const float fx = get<float>(p, kPMvScale), fy = get<float>(p, kPMvScale + 4);
    if (x) *x = fx;
    if (y) *y = fy;
    return fy;
}
void nvnDlssParametersSetPreExposure(void* p, float e) { if (p) put<float>(p, kPPreExposure, e); }
float nvnDlssParametersGetPreExposure(const void* p) { return p ? get<float>(p, kPPreExposure) : 0.0f; }
void nvnDlssParametersSetHistoryReset(void* p, int on) { if (p) put<uint8_t>(p, kPHistoryReset, static_cast<uint8_t>(on)); }
uint32_t nvnDlssParametersGetHistoryReset(const void* p) { return p ? get<uint8_t>(p, kPHistoryReset) : 0; }
void nvnDlssParametersSetEvaluationIndex(void* p, int i) { if (p) put<int32_t>(p, kPEvalIndex, i); }
uint32_t nvnDlssParametersGetEvaluationIndex(const void* p) { return p ? get<uint32_t>(p, kPEvalIndex) : 0; }

// The overlay switches draw debug layers on top of the console's DLSS output (debug layer, full screen, NaN highlight, jitter pattern). They are stored (the
// sdk's evaluate packet carries them) and never drawn: a write-only debug aid with nothing to show here.
void nvnDlssParametersSetOverlayDebugLayer(void* p, int v) { if (p) put<int32_t>(p, kPOverlayDebug, v); }
void nvnDlssParametersSetOverlayFullScreen(void* p, int v) { if (p) put<int32_t>(p, kPOverlayFull, v); }
void nvnDlssParametersSetOverlayShowNans(void* p, int v) { if (p) put<int32_t>(p, kPOverlayNans, v); }
void nvnDlssParametersSetOverlayJitterDebug(void* p, int v) { if (p) put<int32_t>(p, kPOverlayJitter, v); }

// ---- the command ---------------------------------------------------------------------------------------------------------------
CommandBuffer* begin_record(NVNcommandBuffer* cmd, std::unique_lock<std::mutex>& lk) {
    CommandBuffer* cb = host_of<CommandBuffer>(cmd);
    if (!cb) return nullptr;
    lk = std::unique_lock<std::mutex>(cb->lock);
    return (cb->recording && cb->rec) ? cb : nullptr;
}

// One input or output of the evaluation: the texture's host object (held by the list) and the view the game gave.
void note_texture(CmdList* list, const void* params, size_t pair_off, size_t base_off, bool has_base, DlssTex& out) {
    out = DlssTex{};
    const void* tex = reinterpret_cast<const void*>(static_cast<uintptr_t>(get<uint64_t>(params, pair_off)));
    const NVNtextureView* view = reinterpret_cast<const NVNtextureView*>(static_cast<uintptr_t>(get<uint64_t>(params, pair_off + 8)));
    out.tex = host_of<Texture>(tex);
    if (!out.tex) return;
    list->hold(out.tex);
    if (view) {
        out.view = decode(view);
        out.has_view = 1;
    }
    if (has_base) {
        out.base_x = get<int32_t>(params, base_off);
        out.base_y = get<int32_t>(params, base_off + 4);
    }
}

void nvnCommandBufferEvaluateDlss(NVNcommandBuffer* cmd, const void* net, const void* params) {
    if (!net || !params || !get<uint64_t>(net, kNHandle)) return;   // the sdk reads both; a network that was not made has nothing to evaluate
    std::unique_lock<std::mutex> lk;
    CommandBuffer* cb = begin_record(cmd, lk);
    if (!cb) return;
    P_EvaluateDlss p;
    std::memset(static_cast<void*>(&p), 0, sizeof p);
    CmdList* list = cb->rec.get();
    note_texture(list, params, kPInputColor, kPColorBase, true, p.color);
    note_texture(list, params, kPDepth, kPDepthBase, true, p.depth);
    note_texture(list, params, kPMotion, kPMvBase, true, p.motion);
    note_texture(list, params, kPOutputColor, kPOutBase, true, p.output);
    note_texture(list, params, kPBiasMask, kPBiasBase, true, p.bias);
    note_texture(list, params, kPExposure, 0, false, p.exposure);
    p.in_w = get<int32_t>(params, kPInputSize);
    p.in_h = get<int32_t>(params, kPInputSize + 4);
    p.out_w = get<int32_t>(net, kNCurOutW);
    p.out_h = get<int32_t>(net, kNCurOutH);
    p.jitter[0] = get<float>(params, kPJitter);
    p.jitter[1] = get<float>(params, kPJitter + 4);
    p.mv_scale[0] = get<float>(params, kPMvScale);
    p.mv_scale[1] = get<float>(params, kPMvScale + 4);
    p.pre_exposure = get<float>(params, kPPreExposure);
    p.reset = get<uint8_t>(params, kPHistoryReset);
    p.eval_index = get<uint32_t>(params, kPEvalIndex);
    p.method = get<uint32_t>(net, kNMethod);
    p.preset = get<uint32_t>(net, kNPreset);
    p.flags = get<uint32_t>(net, kNBuilderFlags);
    p.serial = get<uint32_t>(net, kNSerial);
    p.net = reinterpret_cast<uint64_t>(net);
    list->emit(Op::EvaluateDlss, p);
}

}  // namespace

void register_api_dlss() {
    NVN2_REG(nvnCommandBufferEvaluateDlss);
    NVN2_REG(nvnDlssNetworkBuilderGetDevice);
    NVN2_REG(nvnDlssNetworkBuilderGetFlags);
    NVN2_REG(nvnDlssNetworkBuilderGetMaxEvaluationEntries);
    NVN2_REG(nvnDlssNetworkBuilderGetMemory);
    NVN2_REG(nvnDlssNetworkBuilderGetMemorySize);
    NVN2_REG(nvnDlssNetworkBuilderGetMethod);
    NVN2_REG(nvnDlssNetworkBuilderGetOptimalSettings);
    NVN2_REG(nvnDlssNetworkBuilderGetOutputResolution);
    NVN2_REG(nvnDlssNetworkBuilderGetPreset);
    NVN2_REG(nvnDlssNetworkBuilderGetStorageSize);
    NVN2_REG(nvnDlssNetworkBuilderGetTextureIDs);
    NVN2_REG(nvnDlssNetworkBuilderGetTexturePool);
    NVN2_REG(nvnDlssNetworkBuilderSetDefaults);
    NVN2_REG(nvnDlssNetworkBuilderSetDevice);
    NVN2_REG(nvnDlssNetworkBuilderSetFlags);
    NVN2_REG(nvnDlssNetworkBuilderSetMaxEvaluationEntries);
    NVN2_REG(nvnDlssNetworkBuilderSetMethod);
    NVN2_REG(nvnDlssNetworkBuilderSetOutputResolution);
    NVN2_REG(nvnDlssNetworkBuilderSetPreset);
    NVN2_REG(nvnDlssNetworkBuilderSetStorage);
    NVN2_REG(nvnDlssNetworkBuilderSetTextureIDs);
    NVN2_REG(nvnDlssNetworkFinalize);
    NVN2_REG(nvnDlssNetworkInitialize);
    NVN2_REG(nvnDlssNetworkResize);
    NVN2_REG(nvnDlssParametersGetBiasCurrentColorSubrectangleBase);
    NVN2_REG(nvnDlssParametersGetDepthSubrectangleBase);
    NVN2_REG(nvnDlssParametersGetEvaluationIndex);
    NVN2_REG(nvnDlssParametersGetHistoryReset);
    NVN2_REG(nvnDlssParametersGetInputColorSubrectangleBase);
    NVN2_REG(nvnDlssParametersGetInputSubrectangleSize);
    NVN2_REG(nvnDlssParametersGetJitterOffset);
    NVN2_REG(nvnDlssParametersGetMotionVectorScale);
    NVN2_REG(nvnDlssParametersGetMotionVectorSubrectangleBase);
    NVN2_REG(nvnDlssParametersGetOutputSubrectangleBase);
    NVN2_REG(nvnDlssParametersGetPreExposure);
    NVN2_REG(nvnDlssParametersSetBiasCurrentColorMask);
    NVN2_REG(nvnDlssParametersSetBiasCurrentColorSubrectangleBase);
    NVN2_REG(nvnDlssParametersSetDefaults);
    NVN2_REG(nvnDlssParametersSetDepth);
    NVN2_REG(nvnDlssParametersSetDepthSubrectangleBase);
    NVN2_REG(nvnDlssParametersSetEvaluationIndex);
    NVN2_REG(nvnDlssParametersSetExposureTexture);
    NVN2_REG(nvnDlssParametersSetHistoryReset);
    NVN2_REG(nvnDlssParametersSetInputColor);
    NVN2_REG(nvnDlssParametersSetInputColorSubrectangleBase);
    NVN2_REG(nvnDlssParametersSetInputSubrectangleSize);
    NVN2_REG(nvnDlssParametersSetJitterOffset);
    NVN2_REG(nvnDlssParametersSetMotionVectorScale);
    NVN2_REG(nvnDlssParametersSetMotionVectorSubrectangleBase);
    NVN2_REG(nvnDlssParametersSetMotionVectors);
    NVN2_REG(nvnDlssParametersSetOutputColor);
    NVN2_REG(nvnDlssParametersSetOutputSubrectangleBase);
    NVN2_REG(nvnDlssParametersSetOverlayDebugLayer);
    NVN2_REG(nvnDlssParametersSetOverlayFullScreen);
    NVN2_REG(nvnDlssParametersSetOverlayJitterDebug);
    NVN2_REG(nvnDlssParametersSetOverlayShowNans);
    NVN2_REG(nvnDlssParametersSetPreExposure);
}

}  // namespace nxrt::nvn
