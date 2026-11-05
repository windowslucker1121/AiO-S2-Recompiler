// SPDX-License-Identifier: GPL-3.0-or-later
// Switch 2 platform pieces that replace guest SDK code (game::Platform::Switch2 only).
//
// Graphics boot: on Switch 2, nv::InitializeGraphics(void*, size_t) runs nn::gs::sdk::BootGraphicsSystem, which always takes the
// remote-memory path (CanBootWithRemoteMemory returns 1): nn::gsv (the GraphicsSupervisor TIPC session), nn::nvsrv and
// nn::gpu::sdk boot the console GPU driver. nxrt implements NVN on the host (nvnBootstrapLoader), so no driver is needed: the
// boot calls answer on the host and return, and the guest's graphics allocator registration still runs as game code.
#include <cstdint>
#include <cstring>

#include "nxrt/abi.h"
#include "nxrt/dispatch.h"
#include "nxrt/extras.h"
#include "nxrt/game.h"
#include "nxrt/log.h"
#include "nxrt/module.h"
#include "nxrt/services_api.h"
#include "nxrt/video_codec.h"

namespace nxrt::extras { void (*layer_luminance_sink)(bool, unsigned, unsigned) = nullptr; }

namespace nxrt {

namespace {

// nn::gs::sdk::CheckGraphicsSystemInitialized starts "adrp x8, flag@page; add x8, x8, flag@pageoff; ldapr w8, [x8]; cmp w8, #1" and
// aborts unless BootGraphicsSystem stored 1 there. The flag address is decoded from those two instructions (no per-build constant).
uint64_t graphics_initialized_flag() {
    const uint64_t f = module_find_symbol("_ZN2nn2gs3sdk30CheckGraphicsSystemInitializedEPKcm");
    if (!f) return 0;
    const uint32_t adrp = *reinterpret_cast<const uint32_t*>(f), add = *reinterpret_cast<const uint32_t*>(f + 4);
    if ((adrp & 0x9f000000u) != 0x90000000u || (add & 0xffc00000u) != 0x91000000u) return 0;
    int64_t imm = static_cast<int64_t>(((adrp >> 29) & 3u) | (((adrp >> 5) & 0x7ffffu) << 2));
    if (imm & (1ll << 20)) imm -= 1ll << 21;
    return (f & ~0xfffull) + static_cast<uint64_t>(imm << 12) + ((add >> 10) & 0xfffu);
}

// nv::InitCheck (called by nn::streamplay and other sdk code) aborts unless the driver pointer that nv::InitializeGraphics stores is non-null:
// "adrp x8, ptr@page; ldr x8, [x8, #ptr@pageoff]; cbz x8, abort". The pointer's address is decoded from the first such pair in the function.
uint64_t nv_driver_pointer() {
    const uint64_t f = module_find_symbol("_ZN2nv9InitCheckEv");
    if (!f) return 0;
    for (int i = 0; i < 24; ++i) {
        const uint32_t adrp = *reinterpret_cast<const uint32_t*>(f + 4 * i), ldr = *reinterpret_cast<const uint32_t*>(f + 4 * i + 4);
        const uint32_t cbz = *reinterpret_cast<const uint32_t*>(f + 4 * i + 8);
        if ((adrp & 0x9f000000u) != 0x90000000u || (ldr & 0xffc00000u) != 0xf9400000u || (cbz & 0xff000000u) != 0xb4000000u) continue;
        int64_t imm = static_cast<int64_t>(((adrp >> 29) & 3u) | (((adrp >> 5) & 0x7ffffu) << 2));
        if (imm & (1ll << 20)) imm -= 1ll << 21;
        return ((f + 4 * i) & ~0xfffull) + static_cast<uint64_t>(imm << 12) + (((ldr >> 10) & 0xfffu) << 3);
    }
    return 0;
}
alignas(64) uint8_t g_nv_driver[0x1000];   // stand-in driver object: nothing reads it so far, InitCheck only tests for null

void initialize_graphics(::Ctx* c) {
    const uint64_t flag = graphics_initialized_flag();
    if (flag) reinterpret_cast<volatile uint32_t*>(flag)[0] = 1;
    const uint64_t drv = nv_driver_pointer();
    if (drv && !*reinterpret_cast<uint64_t*>(drv)) *reinterpret_cast<uint64_t*>(drv) = reinterpret_cast<uint64_t>(g_nv_driver);
    NXLOG_I("switch2", "nv driver pointer at %llx set to a host stand-in", static_cast<unsigned long long>(drv));
    NXLOG_I("switch2", "nv::InitializeGraphics(%llx, %llx): GPU driver boot skipped (NVN runs on the host); gs initialized flag at %llx",
            static_cast<unsigned long long>(c->x[0]), static_cast<unsigned long long>(c->x[1]), static_cast<unsigned long long>(flag));
}

// nv::GetGraphicsMemoryInfo(nv::MemoryInfo*) asks nn::nvsrv (the skipped driver) for GPU memory usage. The SDK itself answers a
// zeroed 0x40-byte MemoryInfo while no driver is up; do the same (Mario Kart World polls it on a worker thread).
// nn::movie (Switch 2's multimedia library) decodes through nn::codec video decoders, which need the GraphicsSupervisor and the NVDEC of
// the skipped GPU boot. With [media] video_decoder = host (src/media/video_codec.cpp) they answer on the host and the SDK's own
// nn::movie::Initialize runs. With it off, nn::movie::Initialize reports failure (a non-zero nn::Result) and the library is never used:
// Mario Kart World's ad-board movie player checks it and leaves the movie out.
void movie_initialize_unavailable(::Ctx* c) {
    static bool logged = false;
    if (!logged) {
        logged = true;
        NXLOG_I("switch2", "nn::movie::Initialize -> unavailable (no host video decoder): movies are left out");
    }
    c->x[0] = 0x1a4cb;   // stand-in non-zero nn::Result
}

// nn::nifm::HandleNetworkRequestErrorResult shows the network connection applet (library applet 0x0F) so the user can fix a failed
// network request. Library applets do not exist here and the SDK aborts when creating one fails, so the call answers as if the user
// backed out of the applet: a non-zero nn::Result (nifm, module 110), and the game carries on offline (Mario Kart World: Time Trials).
void nifm_handle_network_request_error(::Ctx* c) {
    NXLOG_I("switch2", "nn::nifm::HandleNetworkRequestErrorResult -> not resolved (no network applet): the game stays offline");
    c->x[0] = (3400u << 9) | 110u;   // stand-in nifm error result
}

void nv_get_graphics_memory_info(::Ctx* c) {
    if (c->x[0]) std::memset(reinterpret_cast<void*>(c->x[0]), 0, 0x40);
}

// nn::vi on Switch 2 runs over nn::vi2::sdk and the GraphicsSupervisor, which need the skipped GPU boot. The game only needs a display,
// a layer and a native window handle to give NVN; nxrt's NVN presents to its own window, so these answer on the host with stand-in
// objects (static, so they are valid guest addresses) and nn::Result 0.
alignas(16) uint8_t g_display[0x100], g_layer[0x100], g_native_window[0x100];
void vi_ok(::Ctx* c) { c->x[0] = 0; }
void vi_open_default_display(::Ctx* c) {
    if (c->x[0]) *reinterpret_cast<uint64_t*>(c->x[0]) = reinterpret_cast<uint64_t>(g_display);
    NXLOG_I("switch2", "nn::vi::OpenDefaultDisplay -> host display");
    c->x[0] = 0;
}
void vi_create_layer(::Ctx* c) {
    if (c->x[0]) *reinterpret_cast<uint64_t*>(c->x[0]) = reinterpret_cast<uint64_t>(g_layer);
    NXLOG_I("switch2", "nn::vi::CreateLayer -> host layer");
    c->x[0] = 0;
}
void vi_get_native_window(::Ctx* c) {
    if (c->x[0]) *reinterpret_cast<uint64_t*>(c->x[0]) = reinterpret_cast<uint64_t>(g_native_window);
    NXLOG_I("switch2", "nn::vi::GetNativeWindow -> host window handle");
    c->x[0] = 0;
}

// nn::vi::SetLayerNext{Extended,Standard}ContentLuminanceRangeMetadata(Layer*, uint16_t min, uint16_t max): what the game declares about its
// picture for an HDR TV (Pokopia: extended 0..400, standard 0..300 nits, from its graphics-system defaults, every time it makes the layer). The
// values go to the HDR extras ([hdr] mode = native uses them); nn::Result 0.
void vi_layer_extended_luminance(::Ctx* c) {
    if (extras::layer_luminance_sink) extras::layer_luminance_sink(true, static_cast<uint16_t>(c->x[1]), static_cast<uint16_t>(c->x[2]));
    c->x[0] = 0;
}
void vi_layer_standard_luminance(::Ctx* c) {
    if (extras::layer_luminance_sink) extras::layer_luminance_sink(false, static_cast<uint16_t>(c->x[1]), static_cast<uint16_t>(c->x[2]));
    c->x[0] = 0;
}

// nn::vi::GetDisplayVsyncEvent(SystemEventType*, Display*): the host display's 60 Hz vsync event (services/vi_nv.cpp), attached to the
// guest's event by the SDK's own nn::os::AttachReadableHandleToSystemEvent (managed handle, auto clear: each wait takes one vsync).
// Without it the event stays uninitialised and the first WaitSystemEvent aborts (Mario Kart World's frame pacing waits on it).
void vi_get_display_vsync_event(::Ctx* c) {
    static const uint64_t attach = module_find_symbol("_ZN2nn2os33AttachReadableHandleToSystemEventEPNS0_15SystemEventTypeEjbNS0_14EventClearModeE");
    const uint64_t ev = c->x[0];
    const uint32_t h = services::viapi::display_vsync_handle();
    if (!ev || !h || !attach) {
        NXLOG_W("switch2", "nn::vi::GetDisplayVsyncEvent: no event (handle %x, attach %llx)", h, static_cast<unsigned long long>(attach));
        c->x[0] = 0x272;   // vi OperationFailed (services/vi_nv.cpp kViOperationFailed)
        return;
    }
    const uint64_t lr = c->x[30], fn = c->fn;
    c->x[0] = ev;
    c->x[1] = h;
    c->x[2] = 1;   // the handle belongs to the event (DestroySystemEvent closes it)
    c->x[3] = 1;   // EventClearMode_AutoClear
    dispatch_call(c, attach);
    c->x[30] = lr;
    c->fn = fn;
    NXLOG_I("switch2", "nn::vi::GetDisplayVsyncEvent -> host 60 Hz vsync (handle %x)", h);
    c->x[0] = 0;
}

void streamplay_native_window(::Ctx* c) { c->x[0] = reinterpret_cast<uint64_t>(g_native_window); }

}  // namespace

void install_switch2_hle() {
    if (game::info().platform != game::Platform::Switch2) return;
    struct { const char* name; HleFn fn; } const items[] = {
        {"_ZN2nv18InitializeGraphicsEPvm", initialize_graphics},
        {"_ZN2nv18InitializeGraphicsEv", initialize_graphics},
        {"_ZN2nv26InitializeGraphicsDevtoolsEPvm", initialize_graphics},
        {"_ZN2nv21GetGraphicsMemoryInfoEPNS_10MemoryInfoE", nv_get_graphics_memory_info},
        // nn::codec::video (hardware video decode) opens its IPC session through nn::gsv::GetUserSession(), the GraphicsSupervisor of the
        // skipped GPU boot, and locks a null session object. Mario Kart World only calls Initialize/Finalize (void) from its movie player.
        {"_ZN2nn5codec5video10InitializeEv", vi_ok},
        {"_ZN2nn5codec5video8FinalizeEv", vi_ok},
        {"_ZN2nn4nifm31HandleNetworkRequestErrorResultEv", nifm_handle_network_request_error},
        {"_ZN2nn4nifm17NetworkConnection31HandleNetworkRequestErrorResultEv", nifm_handle_network_request_error},
        // nn::streamplay (GameShare): the guest displays go through nn::vi2::sdk and the GPU driver. Nobody streams to a guest here, so
        // creating them succeeds without doing anything and a guest's window handle is the host one.
        {"_ZN2nn10streamplay24CreateIndividualDisplaysEi", vi_ok},
        {"_ZN2nn10streamplay21GetNativeWindowHandleENS0_7GuestIdE", streamplay_native_window},
        {"_ZN2nn2vi10InitializeEv", vi_ok},
        {"_ZN2nn2vi8FinalizeEv", vi_ok},
        {"_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE", vi_open_default_display},
        {"_ZN2nn2vi12CloseDisplayEPNS0_7DisplayE", vi_ok},
        {"_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE", vi_create_layer},
        {"_ZN2nn2vi12DestroyLayerEPNS0_5LayerE", vi_ok},
        {"_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE", vi_get_native_window},
        {"_ZN2nn2vi23SetDisplayVrrCapabilityEPNS0_7DisplayEb", vi_ok},
        // Ocarina of Time (SDK 22). The SDK's layer calls work on the layer's internals, which the host stand-in does not have (an indirect
        // call through null); its display calls answer a display that is not its own with an error, so the queries stay with the SDK.
        {"_ZN2nn2vi36SetDisplay120HzRefreshRateCapabilityEPNS0_7DisplayEb", vi_ok},
        {"_ZN2nn2vi28SetLayerExtendedSwapIntervalEPNS0_5LayerEii", vi_ok},
        {"_ZN2nn2vi30ClearLayerExtendedSwapIntervalEPNS0_5LayerE", vi_ok},
        {"_ZN2nn2vi35SetLayerNextHdrContentGamutMetadataEPNS0_5LayerENS0_15HdrContentGamutE", vi_ok},
        {"_ZN2nn2vi39SetLayerNextHdrToSdrToneMappingMetadataEPNS0_5LayerENS0_14HdrToneMappingE", vi_ok},
        {"_ZN2nn2vi20GetDisplayVsyncEventEPNS_2os15SystemEventTypeEPNS0_7DisplayE", vi_get_display_vsync_event},
        {"_ZN2nn2vi52SetLayerNextHdrExtendedContentLuminanceRangeMetadataEPNS0_5LayerEtt", vi_layer_extended_luminance},
        {"_ZN2nn2vi52SetLayerNextHdrStandardContentLuminanceRangeMetadataEPNS0_5LayerEtt", vi_layer_standard_luminance},
    };
    for (const auto& it : items) {
        hle_register(it.name, it.fn);
        module_override_symbol(it.name);
    }
    if (!media::video_codec_enabled()) {
        hle_register("_ZN2nn5movie10InitializeEv", movie_initialize_unavailable);
        module_override_symbol("_ZN2nn5movie10InitializeEv");
    }
    NXLOG_I("switch2", "Switch 2 platform: graphics system boot answered on the host");
}

}  // namespace nxrt
