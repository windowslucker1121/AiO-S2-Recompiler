// SPDX-License-Identifier: GPL-3.0-or-later
// NVIDIA DLSS Super Resolution / DLAA through Streamline (temporal.h). Streamline (MIT headers in third_party/nvidia_streamline) is loaded at
// run time from sl.interposer.dll; its plugins (sl.common.dll, sl.dlss.dll) and nvngx_dlss.dll (NVIDIA RTX SDK licence, not bundled: the library downloads it) sit next to it
// (release package: upscalers\streamline\).
// Nothing is linked. Streamline has to see vkCreateInstance / vkCreateDevice / the swapchain calls, so with DLSS or DLAA selected at start-up
// the Vulkan loader functions come from its vkGetInstanceProcAddr (streamline_preinit, called by vk_device.cpp before volk is initialised).
// Choosing DLSS later in the F1 page takes effect at the next start.
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>

#include "extras/temporal.h"
#include "nxrt/extras.h"
#include "nxrt/gfx.h"
#include "nxrt/log.h"

#include "../../../third_party/nvidia_streamline/include/sl.h"
#include "../../../third_party/nvidia_streamline/include/sl_consts.h"
#include "../../../third_party/nvidia_streamline/include/sl_dlss.h"
#include "../../../third_party/nvidia_streamline/include/sl_security.h"

#pragma comment(lib, "wintrust.lib")
#pragma comment(lib, "crypt32.lib")

namespace nxrt::extras {
namespace {

constexpr const char* kTag = "dlss";

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
std::string narrow(const std::wstring& w) {
    char buf[1024];
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, buf, sizeof buf, nullptr, nullptr);
    return buf;
}
bool exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

// The folder holding sl.interposer.dll: [upscale] dlss_dll (a folder, or the path of sl.interposer.dll / any file in it); unset: the release
// package's upscalers\streamline\ next to the executable when it holds sl.interposer.dll, else the executable's own folder.
std::wstring sl_dir() {
    std::wstring p = widen(settings().upscale.dlss_dll);
    if (p.empty()) {
        wchar_t buf[MAX_PATH];
        const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
        p.assign(buf, n);
        const size_t s = p.find_last_of(L"\\/");
        const std::wstring exe = s == std::wstring::npos ? std::wstring() : p.substr(0, s + 1);
        const std::wstring pkg = exe + L"upscalers\\streamline\\";
        return exists(pkg + L"sl.interposer.dll") ? pkg : exe;
    }
    const DWORD a = GetFileAttributesW(p.c_str());
    if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) return p.back() == L'\\' || p.back() == L'/' ? p : p + L"\\";
    const size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? std::wstring() : p.substr(0, s + 1);
}

struct Sl {
    HMODULE mod = nullptr;
    bool started = false;
    PFun_slInit* init = nullptr;
    PFun_slIsFeatureSupported* supported = nullptr;
    PFun_slGetNewFrameToken* token = nullptr;
    PFun_slSetConstants* constants = nullptr;
    PFun_slSetTagForFrame* tag = nullptr;
    PFun_slEvaluateFeature* evaluate = nullptr;
    PFun_slGetFeatureFunction* feature_fn = nullptr;
    PFun_slDLSSSetOptions* set_options = nullptr;
    PFun_slDLSSGetOptimalSettings* optimal = nullptr;
};
Sl& sl_state() {
    static Sl s;
    return s;
}

void on_log(sl::LogType type, const char* msg) {
    std::string m = msg ? msg : "";
    while (!m.empty() && (m.back() == '\n' || m.back() == '\r')) m.pop_back();
    if (type == sl::LogType::eError) NXLOG_E(kTag, "streamline: %s", m.c_str());
    else if (type == sl::LogType::eWarn) NXLOG_W(kTag, "streamline: %s", m.c_str());
}

sl::Resource resource(const TemporalImage& t, VkImageLayout layout = VK_IMAGE_LAYOUT_GENERAL) {
    sl::Resource r(sl::ResourceType::eTex2d, reinterpret_cast<void*>(t.image), nullptr, reinterpret_cast<void*>(t.view), static_cast<uint32_t>(layout));
    r.width = t.width;
    r.height = t.height;
    r.nativeFormat = static_cast<uint32_t>(t.format);
    r.mipLevels = 1;
    r.arrayLayers = 1;
    r.flags = 0;
    r.usage = t.usage;
    return r;
}

sl::float4x4 mat(const float* cm) {   // column-major column-vector matrix -> Streamline's row-major row-vector matrix (the same 16 floats)
    sl::float4x4 m;
    for (int i = 0; i < 4; ++i) m.row[i] = sl::float4(cm[i * 4 + 0], cm[i * 4 + 1], cm[i * 4 + 2], cm[i * 4 + 3]);
    return m;
}

std::mutex g_sl_mu;   // slDLSSSetOptions / slDLSSGetOptimalSettings are not thread safe: the game thread asks for sizes, the render thread evaluates
bool g_disabled = false;

sl::DLSSMode mode_for_ratio(double r) {   // output / render
    return r < 1.01 ? sl::DLSSMode::eDLAA : r < 1.55 ? sl::DLSSMode::eMaxQuality : r < 1.8 ? sl::DLSSMode::eBalanced
           : r < 2.2 ? sl::DLSSMode::eMaxPerformance : sl::DLSSMode::eUltraPerformance;
}

class Dlss final : public TemporalBackend {
public:
    explicit Dlss(gfx::Device& dev) : dev_(&dev) {}
    const char* name() const override { return dlaa_ ? "DLAA" : "DLSS"; }
    void set_dlaa(bool on) { dlaa_ = on; }

    bool evaluate(VkCommandBuffer cb, const TemporalFrame& f) override {
        Sl& s = sl_state();
        if (failed_) return false;
        if (g_disabled) return false;
        // one Streamline viewport (history) per network: the camera path (key 0) and each game network have their own
        auto ins = viewports_.emplace(f.history_key, Vp{static_cast<uint32_t>(viewports_.size())});
        Vp& v = ins.first->second;
        const sl::ViewportHandle vp(v.id);
        sl::FrameToken* frame = nullptr;
        if (s.token(frame, nullptr) != sl::Result::eOk || !frame) return fail("slGetNewFrameToken");
        // the mode from the size ratio (DLAA at the same size)
        const double r = static_cast<double>(f.out.width) / static_cast<double>(f.render.width);
        sl::DLSSOptions o;
        o.mode = dlaa_ ? sl::DLSSMode::eDLAA : mode_for_ratio(r);
        o.outputWidth = f.out.width;
        o.outputHeight = f.out.height;
        o.colorBuffersHDR = sl::Boolean::eTrue;
        o.preExposure = f.pre_exposure;
        o.useAutoExposure = f.exposure.image ? sl::Boolean::eFalse : sl::Boolean::eTrue;   // the game's exposure texture, or DLSS's own
        if (o.mode != v.mode || f.out.width != v.out.width || f.out.height != v.out.height || o.preExposure != v.pre_exposure || o.useAutoExposure != v.auto_exp) {
            std::lock_guard<std::mutex> lk(g_sl_mu);
            if (s.set_options(vp, o) != sl::Result::eOk) return fail("slDLSSSetOptions");
            v.mode = o.mode;
            v.out = f.out;
            v.pre_exposure = o.preExposure;
            v.auto_exp = o.useAutoExposure;
            NXLOG_I(kTag, "%s viewport %u: mode %u, %ux%u -> %ux%u, pre-exposure %.3f, %s exposure", name(), v.id, static_cast<unsigned>(o.mode), f.render.width,
                    f.render.height, f.out.width, f.out.height, static_cast<double>(o.preExposure), f.exposure.image ? "the game's" : "automatic");
        }
        sl::Constants c;
        c.cameraViewToClip = mat(f.view_to_clip);
        c.clipToCameraView = mat(f.clip_to_view);
        c.clipToLensClip = mat(kIdentity);
        c.clipToPrevClip = mat(f.clip_to_prev_clip);
        c.prevClipToClip = mat(f.prev_clip_to_clip);
        c.jitterOffset = sl::float2(f.jitter_px[0], f.jitter_px[1]);
        c.mvecScale = sl::float2(1.0f, 1.0f);   // the motion vectors are already in UV units ([-1, 1] of the render size)
        c.cameraPinholeOffset = sl::float2(0.0f, 0.0f);
        c.cameraPos = sl::float3(f.cam_pos[0], f.cam_pos[1], f.cam_pos[2]);
        c.cameraUp = sl::float3(f.cam_up[0], f.cam_up[1], f.cam_up[2]);
        c.cameraRight = sl::float3(f.cam_right[0], f.cam_right[1], f.cam_right[2]);
        c.cameraFwd = sl::float3(f.cam_fwd[0], f.cam_fwd[1], f.cam_fwd[2]);
        c.cameraNear = f.near_z;
        c.cameraFar = f.far_z;
        c.cameraFOV = f.fov_y;
        c.cameraAspectRatio = f.aspect;
        c.depthInverted = sl::Boolean::eTrue;
        c.cameraMotionIncluded = sl::Boolean::eTrue;
        c.motionVectors3D = sl::Boolean::eFalse;
        c.reset = f.reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
        c.motionVectorsJittered = f.mv_jittered ? sl::Boolean::eTrue : sl::Boolean::eFalse;
        if (s.constants(c, *frame, vp) != sl::Result::eOk) return fail("slSetConstants");
        sl::Resource col = resource(f.color), dep = resource(f.depth), mv = resource(f.motion), out = resource(f.output);
        const sl::Extent in_ext{0, 0, f.render.width, f.render.height}, out_ext{0, 0, f.out.width, f.out.height};
        sl::Resource exp_r = resource(f.exposure, f.aux_layout), bias_r = resource(f.bias_mask, f.aux_layout);
        const sl::Extent one{0, 0, 1, 1};
        sl::ResourceTag tags[6] = {sl::ResourceTag(&col, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilPresent, &in_ext),
                                   sl::ResourceTag(&dep, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &in_ext),
                                   sl::ResourceTag(&mv, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &in_ext),
                                   sl::ResourceTag(&out, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilPresent, &out_ext)};
        uint32_t nt = 4;
        if (f.exposure.image) tags[nt++] = sl::ResourceTag(&exp_r, sl::kBufferTypeExposure, sl::ResourceLifecycle::eValidUntilPresent, &one);
        if (f.bias_mask.image) tags[nt++] = sl::ResourceTag(&bias_r, sl::kBufferTypeBiasCurrentColorHint, sl::ResourceLifecycle::eValidUntilPresent, &in_ext);
        auto* slcb = reinterpret_cast<sl::CommandBuffer*>(cb);
        if (s.tag(*frame, vp, tags, nt, slcb) != sl::Result::eOk) return fail("slSetTagForFrame");
        const sl::BaseStructure* inputs[] = {&vp};
        const sl::Result er = s.evaluate(sl::kFeatureDLSS, *frame, inputs, 1, slcb);
        if (er != sl::Result::eOk) return fail("slEvaluateFeature", static_cast<int>(er));
        return true;
    }

private:
    static constexpr float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    bool fail(const char* what, int code = -1) {
        NXLOG_E(kTag, "%s failed (%d): DLSS off", what, code);
        failed_ = true;
        return false;
    }
    gfx::Device* dev_;
    bool dlaa_ = false, failed_ = false;
    struct Vp {
        uint32_t id;
        sl::DLSSMode mode = sl::DLSSMode::eCount;
        VkExtent2D out = {};
        float pre_exposure = -1.0f;
        sl::Boolean auto_exp = sl::Boolean::eFalse;
    };
    std::unordered_map<uint64_t, Vp> viewports_;
};

}  // namespace

bool temporal_dlss_available() {
    const std::wstring dir = sl_dir();
    return exists(dir + L"sl.interposer.dll") && exists(dir + L"sl.dlss.dll") && exists(dir + L"nvngx_dlss.dll");
}
bool temporal_dlss_started() { return sl_state().started && !g_disabled; }

void temporal_dlss_disable(const char* why) {
    if (!g_disabled) NXLOG_W(kTag, "DLSS off for this run: %s", why);
    g_disabled = true;
}

bool temporal_dlss_optimal_size(uint32_t out_w, uint32_t out_h, int scale_pct, uint32_t* rw, uint32_t* rh) {
    Sl& s = sl_state();
    if (!s.started || g_disabled || settings().upscale.mode != UpscaleMode::Dlss) return false;
    std::lock_guard<std::mutex> lk(g_sl_mu);
    if (!s.optimal && (!s.feature_fn || s.feature_fn(sl::kFeatureDLSS, "slDLSSGetOptimalSettings", reinterpret_cast<void*&>(s.optimal)) != sl::Result::eOk || !s.optimal))
        return false;
    sl::DLSSOptions o;
    o.mode = mode_for_ratio(100.0 / std::max(1, scale_pct));
    o.outputWidth = out_w;
    o.outputHeight = out_h;
    sl::DLSSOptimalSettings os;
    if (s.optimal(o, os) != sl::Result::eOk || !os.optimalRenderWidth || !os.optimalRenderHeight) return false;   // ponytail: falls back to the percentage
    *rw = os.optimalRenderWidth;
    *rh = os.optimalRenderHeight;
    return true;
}

PFN_vkGetInstanceProcAddr streamline_preinit() {
    const UpscaleMode m = settings().upscale.mode;
    if (m != UpscaleMode::Dlss && m != UpscaleMode::Dlaa) return nullptr;
    Sl& s = sl_state();
    const std::wstring dir = sl_dir();
    const std::wstring dll = dir + L"sl.interposer.dll";
    if (!exists(dll)) {
        NXLOG_W(kTag, "DLSS selected but %s is missing: put the Streamline DLLs (sl.interposer.dll, sl.common.dll, sl.dlss.dll, nvngx_dlss.dll) there (the "
                      "release package uses upscalers\\streamline) or name their folder in [upscale] dlss_dll", narrow(dll).c_str());
        return nullptr;
    }
    if (!sl::security::verifyEmbeddedSignature(dll.c_str())) {
        NXLOG_E(kTag, "%s has no valid NVIDIA signature: not loaded", narrow(dll).c_str());
        return nullptr;
    }
    s.mod = LoadLibraryW(dll.c_str());
    if (!s.mod) {
        NXLOG_E(kTag, "LoadLibrary(%s) failed (error %lu)", narrow(dll).c_str(), GetLastError());
        return nullptr;
    }
    auto get = [&](const char* n) { return reinterpret_cast<void*>(GetProcAddress(s.mod, n)); };
    s.init = reinterpret_cast<PFun_slInit*>(get("slInit"));
    s.supported = reinterpret_cast<PFun_slIsFeatureSupported*>(get("slIsFeatureSupported"));
    s.token = reinterpret_cast<PFun_slGetNewFrameToken*>(get("slGetNewFrameToken"));
    s.constants = reinterpret_cast<PFun_slSetConstants*>(get("slSetConstants"));
    s.tag = reinterpret_cast<PFun_slSetTagForFrame*>(get("slSetTagForFrame"));
    s.evaluate = reinterpret_cast<PFun_slEvaluateFeature*>(get("slEvaluateFeature"));
    s.feature_fn = reinterpret_cast<PFun_slGetFeatureFunction*>(get("slGetFeatureFunction"));
    auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(get("vkGetInstanceProcAddr"));
    if (!s.init || !s.supported || !s.token || !s.constants || !s.tag || !s.evaluate || !s.feature_fn || !gipa) {
        NXLOG_E(kTag, "sl.interposer.dll lacks the Streamline API");
        return nullptr;
    }
    static const wchar_t* paths[1];
    static std::wstring plugin_dir;
    plugin_dir = dir;
    paths[0] = plugin_dir.c_str();
    static const sl::Feature features[] = {sl::kFeatureDLSS};
    sl::Preferences pref;
    pref.showConsole = false;
    pref.logLevel = sl::LogLevel::eDefault;
    pref.pathsToPlugins = paths;
    pref.numPathsToPlugins = 1;
    pref.pathToLogsAndData = nullptr;
    pref.logMessageCallback = &on_log;
    // no over-the-air downloads; the host restores no command buffer state (the replay rebinds everything after the evaluation)
    pref.flags = sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eUseFrameBasedResourceTagging;
    pref.featuresToLoad = features;
    pref.numFeaturesToLoad = 1;
    pref.engine = sl::EngineType::eCustom;
    pref.engineVersion = "1.0";
    pref.projectId = "5a1f0c2e-8f3b-4e61-9d4c-7b2e6a90c3d5";
    pref.renderAPI = sl::RenderAPI::eVulkan;
    const sl::Result r = s.init(pref, sl::kSDKVersion);
    if (r != sl::Result::eOk) {
        NXLOG_E(kTag, "slInit failed (%d): DLSS off", static_cast<int>(r));
        return nullptr;
    }
    s.started = true;
    NXLOG_I(kTag, "Streamline %d.%d.%d started from %s: the Vulkan instance and device are created through it", SL_VERSION_MAJOR, SL_VERSION_MINOR,
            SL_VERSION_PATCH, narrow(dir).c_str());
    return gipa;
}

TemporalBackend* temporal_dlss(void* gfx_device, bool dlaa) {
    static Dlss* inst = nullptr;
    static bool tried = false;
    Sl& s = sl_state();
    if (!tried && gfx_device) {
        tried = true;
        if (!s.started) {
            NXLOG_W(kTag, "DLSS needs Streamline from the start: choose DLSS or DLAA and restart (the DLLs must be in upscalers\\streamline next to the executable or in [upscale] dlss_dll)");
            return nullptr;
        }
        gfx::Device& dev = *static_cast<gfx::Device*>(gfx_device);
        sl::AdapterInfo ai;
        ai.vkPhysicalDevice = reinterpret_cast<void*>(dev.physical());
        const sl::Result sup = s.supported(sl::kFeatureDLSS, ai);
        if (sup != sl::Result::eOk) {
            NXLOG_W(kTag, "DLSS is not supported on this GPU / driver (slIsFeatureSupported %d)", static_cast<int>(sup));
            return nullptr;
        }
        if (s.feature_fn(sl::kFeatureDLSS, "slDLSSSetOptions", reinterpret_cast<void*&>(s.set_options)) != sl::Result::eOk || !s.set_options ||
            s.feature_fn(sl::kFeatureDLSS, "slDLSSGetOptimalSettings", reinterpret_cast<void*&>(s.optimal)) != sl::Result::eOk) {
            NXLOG_E(kTag, "the DLSS plugin functions are missing");
            return nullptr;
        }
        inst = new Dlss(dev);
        NXLOG_I(kTag, "DLSS available");
    }
    if (inst) inst->set_dlaa(dlaa);
    return inst;
}

}  // namespace nxrt::extras
