// SPDX-License-Identifier: GPL-3.0-or-later
// audren:u (S3c): the audio renderer service. The renderer itself (session state, the update protocol, command generation and the
// DSP) is the port of Ryujinx.Audio in src/services/audren; this file is the IPC face of it:
//   IAudioRendererManager (audren:u)  0 OpenAudioRenderer, 1 GetWorkBufferSize, 2 GetAudioDeviceService, 3 OpenAudioRendererForManualExecution,
//                                     4 GetAudioDeviceServiceWithRevisionInfo
//   IAudioRenderer                    0 GetSampleRate .. 13 GetVoiceDropParameter (RequestUpdate / RequestUpdateAuto run the update protocol)
//   IAudioDevice                      0 ListAudioDeviceName .. 17 AcquireAudioInputDeviceNotification (virtual output devices)
// Adapted from Ryujinx src/Ryujinx.Horizon/Sdk/Audio/Detail/{AudioRendererManager,AudioRenderer,AudioDevice}.cs (MIT).
// The renderer DSP runs on its own thread paced by the host clock (5 ms frames) and writes PCM to nxrt::audio::sink().
#include <atomic>
#include <array>
#include <functional>
#include <memory>
#include <mutex>

#include "audren/Integration/HostAudioDevice.h"
#include "audren/Integration/IWritableEvent.h"
#include "audren/Integration/ThreadBoost.h"
#include "audren/Server/AudioRenderSystem.h"
#include "audren/Server/AudioRendererManager.h"
#include "audren/Server/BehaviourInfo.h"
#include "audren/audren_types.h"
#include "nxrt/config.h"
#include "nxrt/gauge.h"
#include "nxrt/kernel.h"
#include "nxrt/log.h"
#include "services_internal.h"

namespace nxrt::services {

namespace {

using ipc::Request;
using ipc::Response;
using ipc::Service;

constexpr uint32_t kAudioModule = 153;
// Ryujinx AudioResult (module 153): DeviceNotFound 1, UnsupportedRevision 2, NotImplemented 513.
const uint32_t kResultUnsupportedRevision = res(kAudioModule, 2);
const uint32_t kResultNotImplemented = res(kAudioModule, 513);

// An IWritableEvent over a service event: the renderer signals it at the end of every rendered frame.
class SessionEvent : public audren::IWritableEvent {
public:
    void Signal() override { ev.signal(); }
    void Clear() override { ev.clear(); }
    ServiceEvent ev;
};

struct DeviceInfo {
    const char* name;
    uint32_t channels;   // 0: the configured channel count
};

// Ryujinx VirtualDeviceSessionRegistry devices, in its order.
constexpr DeviceInfo kDevices[] = {
    {"AudioStereoJackOutput", 2},
    {"AudioBuiltInSpeakerOutput", 2},
    {"AudioTvOutput", 0},
    {"AudioUsbDeviceOutput", 2},
};

uint32_t configured_channels() { return static_cast<uint32_t>(config_get_int("audio", "channels", 2)) == 6 ? 6u : 2u; }

const DeviceInfo& active_device() { return profile().docked ? kDevices[2] : kDevices[1]; }

struct AudrenState {
    std::mutex mu;
    std::unique_ptr<audren::HostAudioDeviceDriver> driver;
    std::array<std::unique_ptr<SessionEvent>, 4> events;
    std::shared_ptr<audren::AudioRendererManager> manager;
    std::array<std::shared_ptr<ServiceEvent>, 3> device_events;   // QueryAudioDeviceSystemEvent / Input / Output (never fire)
    float device_volume[4] = {1.0f, 1.0f, 1.0f, 1.0f};

    std::shared_ptr<audren::AudioRendererManager> get_manager() {
        if (!manager) {
            driver = std::make_unique<audren::HostAudioDeviceDriver>();
            std::vector<audren::IWritableEvent*> evs;
            for (auto& e : events) {
                e = std::make_unique<SessionEvent>();
                evs.push_back(e.get());
            }
            manager = std::make_shared<audren::AudioRendererManager>();
            manager->Initialize(std::move(evs), driver.get());
            NXLOG_I("audren", "audio renderer manager up (host sink, %u channels)", configured_channels());
        }
        return manager;
    }
};

AudrenState& st() {
    static AudrenState s;
    return s;
}

audren::AudioRendererConfiguration read_config(const Request& req) {
    audren::AudioRendererConfiguration c{};
    if (req.in_size >= sizeof c) std::memcpy(&c, req.in, sizeof c);
    return c;
}

// ---- gauges (include/nxrt/gauge.h): audren.* (the renderer totals are kept by AudioRenderSystem, see audren/Server/AudioRenderSystem.h)
GaugeCounter g_audren_renderers("audren.renderers");   // open IAudioRenderer objects
// Removes its gauge when this file's statics are torn down.
struct GaugeHook {
    const char* name;
    GaugeHook(const char* n, std::function<int64_t()> f) : name(n) { nxrt::gauge_register(n, std::move(f)); }
    ~GaugeHook() { nxrt::gauge_unregister(name); }
};
GaugeHook gauge_voices("audren.voices_in_use", [] { return audren::g_gauge_voices_in_use.load(std::memory_order_relaxed); });
GaugeHook gauge_waves("audren.wavebuffers_queued", [] { return audren::g_gauge_wavebuffers_queued.load(std::memory_order_relaxed); });
GaugeHook gauge_pools("audren.memory_pools", [] { return audren::g_gauge_memory_pools.load(std::memory_order_relaxed); });
GaugeHook gauge_frames("audren.frames_rendered", [] { return static_cast<int64_t>(audren::g_gauge_frames_rendered.load(std::memory_order_relaxed)); });

// ---- IAudioRenderer -----------------------------------------------------------------------------------------------------------------

class AudioRendererObj : public Service {
public:
    AudioRendererObj(std::unique_ptr<audren::AudioRenderSystem> sys, std::shared_ptr<audren::AudioRendererManager> mgr)
        : Service("nn::audio::detail::IAudioRenderer"), mgr_(std::move(mgr)), sys_(std::move(sys)) {
        g_audren_renderers.add(1);
        audren::AudioRenderSystem* s = sys_.get();
        add_cmd(0, "GetSampleRate", [s](Request&, Response& rsp) -> uint32_t {
            rsp.push<int32_t>(static_cast<int32_t>(s->GetSampleRate()));
            return 0;
        });
        add_cmd(1, "GetSampleCount", [s](Request&, Response& rsp) -> uint32_t {
            rsp.push<int32_t>(static_cast<int32_t>(s->GetSampleCount()));
            return 0;
        });
        add_cmd(2, "GetMixBufferCount", [s](Request&, Response& rsp) -> uint32_t {
            rsp.push<int32_t>(static_cast<int32_t>(s->GetMixBufferCount()));
            return 0;
        });
        add_cmd(3, "GetState", [s](Request&, Response& rsp) -> uint32_t {
            rsp.push<int32_t>(s->IsActive() ? 0 : 1);
            return 0;
        });
        auto update = [s](Request& req, Response&) -> uint32_t {
            // The game updates the renderer from its main thread (CAudioManager::PostScriptingUpdate), and the update holds the renderer's lock.
            // The renderer's own frame (SendCommands) waits for that lock; if the system deschedules the game thread inside the update, the
            // frame is late and a sound card starves. The thread runs at higher priority while it holds the lock, as a priority-inheriting
            // mutex would do.
            audren::ScopedAudioLockBoost lock_boost;
            const ipc::Buffer out = req.out_buf(0), perf = req.out_buf(1), in = req.in_buf(0);
            if (!out.addr || !in.addr) return kResultNotImplemented;
            audren::Span<uint8_t> o(buf_mut(out), static_cast<size_t>(out.size));
            audren::Span<uint8_t> p(perf.addr ? buf_mut(perf) : nullptr, static_cast<size_t>(perf.addr ? perf.size : 0));
            audren::Span<const uint8_t> i(buf_ptr(in), static_cast<size_t>(in.size));
            return static_cast<uint32_t>(s->Update(o, p, i));
        };
        add_cmd(4, "RequestUpdate", update);
        add_cmd(5, "Start", [s](Request&, Response&) -> uint32_t {
            s->Start();
            return 0;
        });
        add_cmd(6, "Stop", [s](Request&, Response&) -> uint32_t {
            s->Stop();
            return 0;
        });
        add_cmd(7, "QuerySystemEvent", [s](Request&, Response& rsp) -> uint32_t {
            audren::IWritableEvent* ev = nullptr;
            const audren::ResultCode rc = s->QuerySystemEvent(ev);
            if (rc != audren::ResultCode::Success) return static_cast<uint32_t>(rc);
            rsp.out_copy.push_back(static_cast<SessionEvent*>(ev)->ev.new_handle());
            return 0;
        });
        add_cmd(8, "SetRenderingTimeLimit", [s](Request& req, Response&) -> uint32_t {
            s->SetRenderingTimeLimitPercent(req.arg<uint32_t>(0));
            return 0;
        });
        add_cmd(9, "GetRenderingTimeLimit", [s](Request&, Response& rsp) -> uint32_t {
            rsp.push<int32_t>(static_cast<int32_t>(s->GetRenderingTimeLimit()));
            return 0;
        });
        add_cmd(10, "RequestUpdateAuto", update);
        add_cmd(11, "ExecuteAudioRendererRendering", [s](Request&, Response&) -> uint32_t { return static_cast<uint32_t>(s->ExecuteAudioRendererRendering()); });
        add_cmd(12, "SetVoiceDropParameter", [s](Request& req, Response&) -> uint32_t {
            s->SetVoiceDropParameter(req.arg<float>(0));
            return 0;
        });
        add_cmd(13, "GetVoiceDropParameter", [s](Request&, Response& rsp) -> uint32_t {
            rsp.push<float>(s->GetVoiceDropParameter());
            return 0;
        });
    }
    ~AudioRendererObj() override { g_audren_renderers.sub(1); }

    void on_close() override {
        // The renderer leaves the manager (its destructor unregisters the session and joins the DSP thread when it was the last one).
        std::lock_guard<std::mutex> lk(st().mu);
        sys_.reset();
        NXLOG_D("audren", "audio renderer closed");
    }

private:
    std::shared_ptr<audren::AudioRendererManager> mgr_;   // keeps the manager alive for as long as the renderer exists
    std::unique_ptr<audren::AudioRenderSystem> sys_;
};

// ---- IAudioDevice -------------------------------------------------------------------------------------------------------------------

struct DeviceName {
    char name[0x100];
};

std::shared_ptr<Service> make_audio_device(uint32_t revision) {
    return make_iface("nn::audio::detail::IAudioDevice", [revision](Service& s) {
        auto list_names = [](Request& req, Response& rsp) -> uint32_t {
            const ipc::Buffer out = req.out_buf(0);
            const size_t cap = static_cast<size_t>(out.size / sizeof(DeviceName));
            int32_t n = 0;
            for (const DeviceInfo& d : kDevices) {
                if (static_cast<size_t>(n) >= cap) break;
                if (std::string(d.name) == "AudioUsbDeviceOutput") continue;   // not offered
                DeviceName dn{};
                std::strncpy(dn.name, d.name, sizeof dn.name - 1);
                std::memcpy(buf_mut(out) + n * sizeof(DeviceName), &dn, sizeof dn);
                n++;
            }
            rsp.push<int32_t>(n);
            return 0;
        };
        auto find_device = [](const Request& req) -> int {
            const ipc::Buffer in = req.in_buf(0);
            if (!in.addr || in.size < 1) return -1;
            const std::string name(reinterpret_cast<const char*>(buf_ptr(in)), strnlen(reinterpret_cast<const char*>(buf_ptr(in)), 0x100));
            for (size_t i = 0; i < std::size(kDevices); ++i)
                if (name == kDevices[i].name) return static_cast<int>(i);
            return -1;
        };
        auto set_volume = [find_device](Request& req, Response&) -> uint32_t {
            const int d = find_device(req);
            if (d >= 0) {
                std::lock_guard<std::mutex> lk(st().mu);
                st().device_volume[d] = req.arg<float>(0);
            }
            return 0;
        };
        auto get_volume = [find_device](Request& req, Response& rsp) -> uint32_t {
            const int d = find_device(req);
            float v = 0.0f;
            if (d >= 0) {
                std::lock_guard<std::mutex> lk(st().mu);
                v = st().device_volume[d];
            }
            rsp.push<float>(v);
            return 0;
        };
        auto active_name = [](Request& req, Response&) -> uint32_t {
            const ipc::Buffer out = req.out_buf(0);
            if (out.addr && out.size >= sizeof(DeviceName)) {
                DeviceName dn{};
                std::strncpy(dn.name, active_device().name, sizeof dn.name - 1);
                std::memcpy(buf_mut(out), &dn, sizeof dn);
            }
            return 0;
        };
        auto event_cmd = [](size_t which) {
            return [which](Request&, Response& rsp) -> uint32_t {
                std::lock_guard<std::mutex> lk(st().mu);
                if (!st().device_events[which]) st().device_events[which] = std::make_shared<ServiceEvent>(false);
                rsp.out_copy.push_back(st().device_events[which]->new_handle());
                return 0;
            };
        };
        s.add_cmd(0, "ListAudioDeviceName", list_names);
        s.add_cmd(1, "SetAudioDeviceOutputVolume", set_volume);
        s.add_cmd(2, "GetAudioDeviceOutputVolume", get_volume);
        s.add_cmd(3, "GetActiveAudioDeviceName", active_name);
        s.add_cmd(4, "QueryAudioDeviceSystemEvent", event_cmd(0));
        s.add_cmd(5, "GetActiveChannelCount", [](Request&, Response& rsp) -> uint32_t {
            rsp.push<int32_t>(static_cast<int32_t>(configured_channels()));
            return 0;
        });
        s.add_cmd(6, "ListAudioDeviceNameAuto", list_names);
        s.add_cmd(7, "SetAudioDeviceOutputVolumeAuto", set_volume);
        s.add_cmd(8, "GetAudioDeviceOutputVolumeAuto", get_volume);
        s.add_cmd(10, "GetActiveAudioDeviceNameAuto", active_name);
        s.add_cmd(11, "QueryAudioDeviceInputEvent", event_cmd(1));
        s.add_cmd(12, "QueryAudioDeviceOutputEvent", event_cmd(2));
        s.add_cmd(13, "GetActiveAudioOutputDeviceName", active_name);
        s.add_cmd(14, "ListAudioOutputDeviceName", list_names);
        auto not_implemented = [](Request&, Response&) -> uint32_t { return kResultNotImplemented; };
        s.add_cmd(15, "AcquireAudioOutputDeviceNotification", not_implemented);
        s.add_cmd(16, "ReleaseAudioOutputDeviceNotification", not_implemented);
        s.add_cmd(17, "AcquireAudioInputDeviceNotification", not_implemented);
        // 19 / 20 (SDK 22, Ocarina of Time): nn::audio::SetDeviceOutputVolumeAutoTuneModeEnabled(bool) (bool @0, aruid @8) and
        // IsDeviceOutputVolumeAutoTuneModeEnabled() (aruid in, bool out); the SDK aborts on an error. The flag is kept and reported back;
        // the PC's volume stays the player's own (off until the game sets it).
        static std::atomic<bool> auto_tune{false};
        s.add_cmd(19, "SetDeviceOutputVolumeAutoTuneModeEnabled", [](Request& req, Response&) -> uint32_t {
            auto_tune = req.arg<uint8_t>(0) != 0;
            return 0;
        });
        s.add_cmd(20, "IsDeviceOutputVolumeAutoTuneModeEnabled", [](Request&, Response& rsp) -> uint32_t {
            rsp.push<uint8_t>(auto_tune ? 1 : 0);
            return 0;
        });
        (void)revision;
    });
}

// ---- audren:u -----------------------------------------------------------------------------------------------------------------------

uint32_t open_renderer(Request& req, Response& rsp, bool manual) {
    if (req.in_size < 0x48) return kResultNotImplemented;
    audren::AudioRendererConfiguration cfg = read_config(req);
    // OpenAudioRenderer: config (0x34), u64 work buffer size at 0x38, u64 aruid at 0x40; copy handles: the work buffer transfer memory, the process.
    // OpenAudioRendererForManualExecution: config, u64 work buffer address at 0x38, u64 size at 0x40, u64 aruid at 0x48 (and the process handle).
    uint64_t work_addr = 0, size = 0, user = 0;
    if (manual) {
        work_addr = req.arg<uint64_t>(0x38);
        size = req.arg<uint64_t>(0x40);
        user = req.arg<uint64_t>(0x48);
    } else {
        size = req.arg<uint64_t>(0x38);
        user = req.arg<uint64_t>(0x40);
        if (!req.in_copy.empty())
            if (auto tm = kernel::handle_as<kernel::KTransferMemory>(req.in_copy[0])) work_addr = tm->addr;
    }
    // The copy handles stay the client's: HIPC copies them, only move handles change owner (debug note 122). The renderer allocates its own
    // work memory (as Ryujinx does), so the transfer memory is not kept.

    std::unique_ptr<audren::AudioRenderSystem> sys;
    audren::ResultCode rc;
    std::shared_ptr<audren::AudioRendererManager> mgr;
    {
        std::lock_guard<std::mutex> lk(st().mu);
        mgr = st().get_manager();
        rc = mgr->OpenAudioRenderer(sys, cfg, user, work_addr, size, 0);
    }
    if (rc != audren::ResultCode::Success) {
        NXLOG_W("audren", "OpenAudioRenderer failed: 0x%x (revision 0x%x, voices %u, mixes %u)", static_cast<uint32_t>(rc), cfg.revision, cfg.voice_count,
                cfg.mix_buffer_count);
        return static_cast<uint32_t>(rc);
    }
    NXLOG_I("audren", "OpenAudioRenderer: rate %u, samples %u, mixbuffers %u, voices %u, sinks %u, effects %u, revision REV%d", cfg.sample_rate,
            cfg.sample_count, cfg.mix_buffer_count, cfg.voice_count, cfg.sink_count, cfg.effect_count, audren::BehaviourInfo::GetRevisionNumber(cfg.revision));
    rsp.push_object(std::make_shared<AudioRendererObj>(std::move(sys), std::move(mgr)));
    return 0;
}

std::shared_ptr<Service> make_audren() {
    auto s = std::make_shared<Service>("audren:u");
    s->add_cmd(0, "OpenAudioRenderer", [](Request& req, Response& rsp) -> uint32_t { return open_renderer(req, rsp, false); });
    s->add_cmd(1, "GetWorkBufferSize", [](Request& req, Response& rsp) -> uint32_t {
        audren::AudioRendererConfiguration cfg = read_config(req);
        if (!audren::BehaviourInfo::CheckValidRevision(cfg.revision)) {
            NXLOG_W("audren", "library revision REV%d is not supported", audren::BehaviourInfo::GetRevisionNumber(cfg.revision));
            return kResultUnsupportedRevision;
        }
        const int64_t size = static_cast<int64_t>(audren::AudioRendererManager::GetWorkBufferSize(cfg));
        NXLOG_D("audren", "GetWorkBufferSize = 0x%llx", static_cast<unsigned long long>(size));
        rsp.push<int64_t>(size);
        return 0;
    });
    s->add_cmd(2, "GetAudioDeviceService", [](Request&, Response& rsp) -> uint32_t {
        rsp.push_object(make_audio_device(('R' << 0) | ('E' << 8) | ('V' << 16) | ('1' << 24)));
        return 0;
    });
    s->add_cmd(3, "OpenAudioRendererForManualExecution", [](Request& req, Response& rsp) -> uint32_t { return open_renderer(req, rsp, true); });
    s->add_cmd(4, "GetAudioDeviceServiceWithRevisionInfo", [](Request& req, Response& rsp) -> uint32_t {
        rsp.push_object(make_audio_device(req.arg<uint32_t>(8)));
        return 0;
    });
    return s;
}

}  // namespace

// aud:u (Switch 2): the SDK's user client session (nn::audio::CreateUserClientSessionByHipc). nn::audio::CreateAudioRendererManager(false)
// asks it for the renderer manager with cmd 2 instead of opening audren:u; the answer is the same IAudioRendererManager object.
std::shared_ptr<Service> make_aud_user() {
    auto s = std::make_shared<Service>("aud:u");
    // Commands from the SDK (sdk 20.5.25, IDA): the proxy's vtable slots +32/+40/+48/+56 send cmds 0-3. ListAudioIns / OpenAudioIn use cmd 0,
    // ListAudioOuts / OpenAudioOut cmd 1, CreateAudioRendererManager(false) cmd 2, SetSpatialDownmixOutputMode cmd 3 (then that object's cmd 0).
    s->add_cmd(0, "CreateAudioInManager", [](Request&, Response& rsp) -> uint32_t {
        rsp.push_object(audio_in_manager());
        return 0;
    });
    s->add_cmd(1, "CreateAudioOutManager", [](Request&, Response& rsp) -> uint32_t {
        rsp.push_object(audio_out_manager());
        return 0;
    });
    s->add_cmd(2, "CreateAudioRendererManager", [](Request&, Response& rsp) -> uint32_t {
        rsp.push_object(make_audren());
        return 0;
    });
    s->add_cmd(3, "CreateAudioOutputControl", [](Request&, Response& rsp) -> uint32_t {   // name ours; the object sets output modes
        auto ctl = std::make_shared<Service>("aud:u output control");
        ctl->add_cmd(0, "SetSpatialDownmixOutputMode", [](Request&, Response&) -> uint32_t { return 0; });   // the PC mixes its own output
        rsp.push_object(ctl);
        return 0;
    });
    return s;
}

// ---- auddmg:u (Switch 2, SDK 22) ----------------------------------------------------------------------------------------------------
// nn::audio::devmgr, the audio device manager: virtual devices (speaker, microphone), capture streams, mix topology. Command 0
// (devmgr::detail::InitializeImpl -> GetImpl: u32 2 @0, sends PID) answers with the session object every other call goes to; the client
// aborts on an error. The session's commands are found as the game uses them (an unimplemented one is logged with its id): Ocarina of
// Time lists the virtual input devices at boot.
std::shared_ptr<Service> make_auddmg() {
    auto s = std::make_shared<Service>("auddmg:u");
    s->add_cmd(0, "OpenSession", [](Request&, Response& rsp) -> uint32_t {
        auto session = std::make_shared<Service>("nn::audio::devmgr::ISession");
        // 0: ListVirtualDeviceNames (devmgr::ListVirtualDeviceNames: functionality, bool, names out, count out); Ocarina of Time asks for the
        // input devices (microphones) at boot. None for now: the count is 0 and the client clears the names. A microphone comes with
        // capture support.
        session->add_cmd(0, "ListVirtualDeviceNames", [](Request&, Response& r) -> uint32_t {
            r.push<int64_t>(0);
            return 0;
        });
        rsp.push_object(session);
        return 0;
    });
    return s;
}

void register_audio() {
    add("audren:u", [] { return make_audren(); });
    add("aud:u", [] { return make_aud_user(); });
    add("auddmg:u", [] { return make_auddmg(); });
    register_audio_out();
}

void audio_reset() {
    {
        std::lock_guard<std::mutex> lk(st().mu);
        st().manager.reset();   // disposes the sessions and joins the DSP thread
        st().driver.reset();
        for (auto& e : st().events) e.reset();
        for (auto& e : st().device_events) e.reset();
        for (float& v : st().device_volume) v = 1.0f;
    }
    audio_out_reset();
}

}  // namespace nxrt::services
