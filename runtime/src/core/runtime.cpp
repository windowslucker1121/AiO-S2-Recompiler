// SPDX-License-Identifier: GPL-3.0-or-later
#include "nxrt/runtime.h"

#include <cstdlib>
#include <mutex>

#include "nxrt/audio_sink.h"
#include "nxrt/bugcapture.h"
#include "nxrt/config.h"
#include "nxrt/diag.h"
#include "nxrt/dispatch.h"
#include "nxrt/eh.h"
#include "nxrt/ftrace.h"
#include "nxrt/gauge.h"
#include "nxrt/hooks.h"
#include "nxrt/ipc.h"
#include "nxrt/kernel.h"
#include "nxrt/log.h"
#include "nxrt/memguard.h"
#include "nxrt/memory.h"
#include "nxrt/module.h"
#include "nxrt/movie.h"
#include "nxrt/video_codec.h"
#include "nxrt/nxmodule.h"
#include "nxrt/services.h"
#include "nxrt/svc.h"
#include "nxrt/symbols.h"
#include "nxrt/timestep.h"
#include "nxrt/tt.h"

namespace nxrt {
void install_switch2_hle();   // src/core/switch2.cpp
void trace_sites_install();   // src/diag/trace_sites.cpp
namespace {
std::mutex g_lock;
bool g_up = false;
}  // namespace

bool init(const InitOptions& opt) {
    std::lock_guard<std::mutex> lk(g_lock);
    if (g_up) return true;
    const char* ini = opt.ini_path ? opt.ini_path : std::getenv("NXRT_INI");
    config_load_file(ini && *ini ? ini : "nxrt.ini");
    LogOptions lo;
    lo.path = opt.log_path;
    log_init(lo);
    NXLOG_I("runtime", "nxrt starting (ABI %s)", abi_source());
    if (opt.load_symbols) {
        size_t n = symbols_load_map();
        NXLOG_I("runtime", "host symbols: %zu from the linker map%s%s", n, n ? " " : " (no map next to the exe or in its PDB's folder: host frames show module+offset only)", n ? symbols_map_path().c_str() : "");
    }
    if (opt.crash_handler) crash_install();
    bool ok = true;
    if (opt.memory_init && !mem::init()) {
        NXLOG_E("runtime", "address-space layout could not be reserved");
        ok = false;
    }
    svc_init();
    kernel::install_spin_hint();
    kernel::starvation_guard_start();
    if (opt.services) {
        services::register_all();
        kernel::install_busy_mutex_hle();   // the SDK's spin lock yields under the console core model
        install_libc_hle();           // the SDK's string and memory functions as host code ([hle] libc)
        media::install_movie_hle();   // movie:: on Media Foundation ([media] movie = off keeps the library in subsdk0); Switch 1 only
        media::install_video_codec_hle();   // Switch 2: nn::codec VP9/VP8 decoders + fences on libvpx (src/media/video_codec.cpp)
        install_switch2_hle();        // Switch 2: the GPU driver boot answers on the host (src/core/switch2.cpp)
        trace_sites_install();        // [diag] trace_sites: bring-up register probes at mid hook sites (src/diag/trace_sites.cpp)
    }
    if (opt.memguard) memguard_start();
    if (opt.watchdog) watchdog_start();
    bug::start_requests();      // bug capture request files <exe dir>\capture_bug[.<pid>] ([bug] requests; nxrt/bugcapture.h)
    config_log_effective();
    hitch_start();              // frame summary every 10 s and a line per frame over 50 ms ([diag] hitch_log / hitch_summary_s / hitch_ms); on by default
    soak_start();               // gauge sampler ([diag] soak_csv); a no-op when unset
    prof_sampler_start();       // sampling profiler ([prof] sample_hz); a no-op when unset
    ft::start_from_config();    // frame trace ([prof] trace); a no-op when unset
    tt::init();                 // test tooling (deterministic mode, scripted runs, traces); a no-op unless a [test] key asks for it
    timestep::init();           // real-time frame step for the game ([time] section; the hooks do nothing until the lifted game calls them)
    g_up = ok;
    return ok;
}

std::string summary() {
    std::string s;
    s += svc_summary() + "\n";
    s += ipc::ipc_summary();
    s += "audio: " + audio::stats_report() + "\n";
    size_t stubs = 0, called_stubs = 0;
    for (auto& h : hle_list()) {
        stubs += h.stub;
        if (h.stub && h.calls) {
            called_stubs++;
            s += "hle stub called " + std::to_string(h.calls) + "x: " + h.name + "\n";
        }
    }
    s += "hle: " + std::to_string(hle_count()) + " thunks, " + std::to_string(stubs) + " logging stubs (" + std::to_string(called_stubs) + " called)\n";
    {
        eh::Stats e = eh::stats();
        s += "lifted modules: " + std::to_string(nxmodules().size()) + ", imports through the registry: " + std::to_string(import_calls()) + ", site hooks: " +
             std::to_string(hook_count()) + " (calls: inline " + std::to_string(hook_site_calls(HookKind::Inline)) + ", mid " + std::to_string(hook_site_calls(HookKind::Mid)) +
             ", entry " + std::to_string(hook_site_calls(HookKind::Entry)) + ")\n";
        s += "c++ exceptions: " + std::to_string(e.raised) + " raised, " + std::to_string(e.resumed) + " resumed, " + std::to_string(e.caught) + " caught, " +
             std::to_string(e.cleanups) + " cleanup landings, " + std::to_string(e.uncaught) + " uncaught, " + std::to_string(e.frames_walked) + " frames walked\n";
    }
    s += "lifted functions: " + std::to_string(fn_count()) + ", modules: " + std::to_string(modules().size()) + ", live threads: " +
         std::to_string(kernel::thread_count_running()) + ", handles open: " + std::to_string(kernel::handle_count()) + "\n";
    return s;
}

void shutdown() {
    std::lock_guard<std::mutex> lk(g_lock);
    if (!g_up) return;
    NXLOG_I("runtime", "end-of-run summary:\n%s", summary().c_str());
    timestep::shutdown();
    tt::shutdown();
    prof_sampler_stop();
    soak_stop();
    hitch_stop();
    watchdog_stop();
    bug::stop_requests();
    kernel::starvation_guard_stop();
    memguard_stop();
    log_flush();
    g_up = false;
}

bool initialised() {
    std::lock_guard<std::mutex> lk(g_lock);
    return g_up;
}

}  // namespace nxrt
