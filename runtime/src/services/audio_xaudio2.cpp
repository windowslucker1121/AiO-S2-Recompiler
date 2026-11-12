// SPDX-License-Identifier: GPL-3.0-or-later
// XAudio2 host sink (S3c): an AudioSink that plays the guest's interleaved int16 PCM on the output device chosen by
// [audio] device (the default device unless it names another one).
// The engine, a mastering voice and one source voice are created in open(); write() copies the caller's frames into a small pool
// of buffers and submits them; the voice callback (OnBufferEnd, on XAudio2's audio thread) returns the buffer to the pool. When the
// device already holds kMaxQueued buffers, or the pool is empty, write() drops the rest and reports how many frames it accepted, so
// the caller never blocks. The voice callback also counts underruns (OnVoiceProcessingPassStart with nothing queued). The source
// voice carries the process master volume ([audio] volume), re-applied whenever it changes.
#include <windows.h>
#include <objbase.h>
#include <mmreg.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <xaudio2.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "nxrt/audio_sink.h"
#include "nxrt/config.h"
#include "nxrt/game.h"
#include "nxrt/instance.h"
#include "nxrt/log.h"

#pragma comment(lib, "xaudio2.lib")

namespace nxrt::audio {
namespace {

// audout submits each guest buffer as its own voice buffer (often 5-10 ms each) and keeps the guest's queue plus [audio] out_latency_ms in
// the voice, so the pool holds far more buffers than the queue normally needs.
constexpr uint32_t kSlots = 64;          // buffers in the pool
constexpr uint32_t kSlotFrames = 1024;   // frames per buffer (about 21 ms at 48 kHz)
constexpr UINT32 kMaxQueued = 60;        // beyond this many queued buffers write() drops the new frames (below XAUDIO2_MAX_QUEUED_BUFFERS)

// KSDATAFORMAT_SUBTYPE_PCM (ksmedia.h), spelled out so the link needs no ksguid.lib.
constexpr GUID kSubtypePcm = {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
// SPEAKER_5POINT1: FRONT_LEFT | FRONT_RIGHT | FRONT_CENTER | LOW_FREQUENCY | BACK_LEFT | BACK_RIGHT.
constexpr DWORD kChannelMask51 = 0x3F;

std::string to_utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string out(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out.data(), n, nullptr, nullptr);
    return out;
}

std::string lower_ascii(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

void log_devices(const std::vector<OutputDevice>& devices) {
    if (devices.empty()) NXLOG_I("audio", "xaudio2: no active output endpoints");
    for (const OutputDevice& d : devices) {
        NXLOG_I("audio", "xaudio2: output device%s: %s [%s]", d.is_default ? " (default)" : "", d.name.c_str(),
                to_utf8(d.id).c_str());
    }
}

// The endpoint [audio] device asks for. "default" (or empty) is the default endpoint (empty id). Otherwise an exact endpoint id
// wins, then the first endpoint whose friendly name contains the text (ASCII case-insensitive). No match: a warning and the default.
std::wstring pick_device(const std::vector<OutputDevice>& devices, const std::string& want) {
    if (want.empty() || lower_ascii(want) == "default") return {};
    for (const OutputDevice& d : devices) {
        if (to_utf8(d.id) == want) return d.id;
    }
    const std::string needle = lower_ascii(want);
    for (const OutputDevice& d : devices) {
        if (lower_ascii(d.name).find(needle) != std::string::npos) return d.id;
    }
    NXLOG_W("audio", "xaudio2: [audio] device = %s matches no active output device; using the default device", want.c_str());
    return {};
}

// The Windows volume mixer lists this process' audio session. Without a name it shows the exe's file description and a generic icon, so
// the session gets the game window's title and the exe's own icon. XAudio2 plays through the process' default session (GUID_NULL) of the
// endpoint it opened: the one chosen by [audio] device, or the default render endpoint (console and multimedia roles, usually the same).
// Called after the mastering voice exists, on the thread that opened the sink (COM is initialised there). Failures only log.
// The name the mixer shows: [audio] session_name, else the game window's title (with " [instance]" for a named instance, as the window).
std::string session_display_name() {
    std::string s = config_get_str("audio", "session_name", "");
    if (!s.empty()) return s;
    s = nxrt::game::window_title();
    if (nxrt::instance_active()) s += " [" + nxrt::instance_name() + "]";
    return s;
}

template <class T>
void release_com(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

void name_session_on(IMMDevice* dev, const std::wstring& name, const std::wstring& icon) {
    IAudioSessionManager2* mgr = nullptr;
    IAudioSessionControl* ctl = nullptr;
    HRESULT hr = dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&mgr));
    if (SUCCEEDED(hr)) hr = mgr->GetAudioSessionControl(nullptr, 0, &ctl);
    if (SUCCEEDED(hr)) hr = ctl->SetDisplayName(name.c_str(), nullptr);
    if (SUCCEEDED(hr) && !icon.empty()) hr = ctl->SetIconPath(icon.c_str(), nullptr);
    if (FAILED(hr)) NXLOG_W("audio", "xaudio2: naming the audio session failed (0x%08lx)", static_cast<unsigned long>(hr));
    release_com(ctl);
    release_com(mgr);
}

void name_audio_session(const std::wstring& device_id) {
    const std::string title = session_display_name();
    if (title.empty()) return;
    const int n = MultiByteToWideChar(CP_UTF8, 0, title.c_str(), -1, nullptr, 0);
    std::wstring name(n > 0 ? static_cast<size_t>(n - 1) : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, title.c_str(), -1, name.data(), n);
    // "<exe path>,0": the first icon group of the exe. An exe without one (tests) keeps the mixer's default icon.
    std::wstring icon;
    wchar_t exe[MAX_PATH * 4] = {};
    const DWORD len = GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
    if (len > 0 && len < std::size(exe) && FindResourceW(nullptr, MAKEINTRESOURCEW(1), MAKEINTRESOURCEW(14) /* RT_GROUP_ICON */))
        icon = std::wstring(exe, len) + L",0";

    IMMDeviceEnumerator* en = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) return;
    std::vector<std::wstring> done;
    auto name_device = [&](IMMDevice* dev) {
        if (!dev) return;
        LPWSTR id = nullptr;
        if (SUCCEEDED(dev->GetId(&id)) && id) {
            const std::wstring sid(id);
            CoTaskMemFree(id);
            if (std::find(done.begin(), done.end(), sid) == done.end()) {
                done.push_back(sid);
                name_session_on(dev, name, icon);
            }
        }
        dev->Release();
    };
    if (!device_id.empty()) {
        IMMDevice* dev = nullptr;
        if (SUCCEEDED(en->GetDevice(device_id.c_str(), &dev))) name_device(dev);
    } else {
        for (ERole role : {eConsole, eMultimedia}) {
            IMMDevice* dev = nullptr;
            if (SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, role, &dev))) name_device(dev);
        }
    }
    en->Release();
    if (!done.empty()) NXLOG_I("audio", "xaudio2: audio session named \"%s\"%s", title.c_str(), icon.empty() ? " (no exe icon)" : ", exe icon");
}

class XAudio2Sink final : public AudioSink {
public:
    ~XAudio2Sink() override { close(); }

    bool open(uint32_t sample_rate, uint32_t channels) override;
    void close() override;
    uint32_t write(const int16_t* pcm, uint32_t frames) override;
    uint32_t queued_frames() override;
    bool has_device_clock() const override { return true; }   // SamplesPlayed is the device's own clock
    const char* name() const override { return "xaudio2"; }
    SinkStats stats() override;

private:
    struct Slot {
        std::vector<int16_t> pcm;   // kSlotFrames * channels values
        uint32_t frames = 0;
        bool busy = false;          // submitted and not yet finished by the voice
    };

    // The voice callback. XAudio2 calls it on its own thread. OnBufferEnd returns a buffer to the pool; OnVoiceProcessingPassStart
    // checks for underruns.
    struct Callback final : IXAudio2VoiceCallback {
        XAudio2Sink* owner = nullptr;
        void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32 bytes_required) noexcept override {
            owner->on_pass_start(bytes_required);
        }
        void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() noexcept override {}
        void STDMETHODCALLTYPE OnStreamEnd() noexcept override {}
        void STDMETHODCALLTYPE OnBufferStart(void*) noexcept override {}
        void STDMETHODCALLTYPE OnBufferEnd(void* ctx) noexcept override {
            owner->release_slot(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(ctx)));
        }
        void STDMETHODCALLTYPE OnLoopEnd(void*) noexcept override {}
        void STDMETHODCALLTYPE OnVoiceError(void*, HRESULT error) noexcept override {
            NXLOG_W("audio", "xaudio2: voice error 0x%08lx", static_cast<unsigned long>(error));
        }
    };

    void release_slot(uint32_t idx);
    void on_pass_start(UINT32 bytes_required) noexcept;
    void note_queued(uint32_t buffers) noexcept;
    uint32_t queued_now() noexcept;                  // submitted frames minus the frames the device has played
    void note_queue_frames(bool playing) noexcept;   // peak and low water marks of queued_now()
    uint32_t submit(const int16_t* pcm, uint32_t frames);
    void teardown();   // destroys the voices and the engine and undoes our COM initialisation; safe when nothing is open

    IXAudio2* engine_ = nullptr;
    IXAudio2MasteringVoice* master_ = nullptr;
    IXAudio2SourceVoice* source_ = nullptr;
    Callback callback_;
    bool com_owned_ = false;          // this sink called CoInitializeEx successfully and must balance it
    DWORD com_thread_ = 0;            // the thread that did, so CoUninitialize runs there
    uint32_t channels_ = 0;
    uint32_t rate_ = 48000;
    float applied_volume_ = 1.0f;     // the volume last set on the source voice (write thread)
    bool session_named_ = false;      // the mixer name and icon are set (once per sink: the session outlives the voices)

    std::mutex mu_;                   // guards slots_ (write, queued_frames and the callback)
    std::mutex voice_mu_;             // keeps source_ alive while queued_frames() reads it from another thread (the stats logger);
                                      // never taken on XAudio2's own thread, since DestroyVoice waits for the callbacks
    std::vector<Slot> slots_ = std::vector<Slot>(kSlots);

    // Counters for stats(). They are not reset by open().
    std::atomic<uint64_t> frames_written_{0};
    std::atomic<uint64_t> frames_accepted_{0};
    std::atomic<uint64_t> frames_dropped_{0};
    std::atomic<uint64_t> frames_audible_{0};
    std::atomic<uint64_t> underruns_{0};
    std::atomic<uint32_t> queued_peak_{0};
    std::atomic<uint64_t> submitted_frames_{0};          // frames handed to the voice since open()
    std::atomic<uint32_t> queue_peak_frames_{0};
    std::atomic<uint32_t> queue_low_frames_{0xffffffffu};
    std::atomic<bool> started_{false};   // a buffer has been submitted since open()
    std::atomic<bool> starved_{false};   // the device is out of buffers now; an underrun is counted once per such stretch
};

bool XAudio2Sink::open(uint32_t sample_rate, uint32_t channels) {
    close();
    if (channels != 2 && channels != 6) {
        NXLOG_W("audio", "xaudio2: %u channels not supported (2 or 6)", channels);
        return false;
    }
    if (sample_rate < XAUDIO2_MIN_SAMPLE_RATE || sample_rate > XAUDIO2_MAX_SAMPLE_RATE) {
        NXLOG_W("audio", "xaudio2: sample rate %u out of range", sample_rate);
        return false;
    }

    // XAudio2 wants COM. Initialise it only when this thread has no mode yet; an existing mode (RPC_E_CHANGED_MODE) is kept.
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (com == S_OK || com == S_FALSE) {
        com_owned_ = true;
        com_thread_ = GetCurrentThreadId();
    } else if (com != RPC_E_CHANGED_MODE) {
        NXLOG_W("audio", "xaudio2: CoInitializeEx failed (0x%08lx)", static_cast<unsigned long>(com));
        return false;
    }

    HRESULT hr = XAudio2Create(&engine_, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr)) {
        NXLOG_W("audio", "xaudio2: XAudio2Create failed (0x%08lx)", static_cast<unsigned long>(hr));
        teardown();
        return false;
    }

    const std::vector<OutputDevice> devices = list_output_devices();
    log_devices(devices);
    const std::wstring device_id = pick_device(devices, config_get_str("audio", "device", "default"));
    auto create_master = [this](const wchar_t* id) {
        return engine_->CreateMasteringVoice(&master_, XAUDIO2_DEFAULT_CHANNELS, XAUDIO2_DEFAULT_SAMPLERATE, 0, id, nullptr,
                                             AudioCategory_GameEffects);
    };
    hr = create_master(device_id.empty() ? nullptr : device_id.c_str());
    bool used_default = device_id.empty();
    if (FAILED(hr) && !device_id.empty()) {
        NXLOG_W("audio", "xaudio2: the chosen output device failed (CreateMasteringVoice 0x%08lx); using the default device",
                static_cast<unsigned long>(hr));
        hr = create_master(nullptr);
        used_default = true;
    }
    if (FAILED(hr)) {
        NXLOG_W("audio", "xaudio2: no audio output device (CreateMasteringVoice 0x%08lx)", static_cast<unsigned long>(hr));
        teardown();
        return false;
    }
    if (!session_named_) {
        name_audio_session(used_default ? std::wstring() : device_id);
        session_named_ = true;
    }

    // Stereo is plain PCM; 5.1 needs WAVEFORMATEXTENSIBLE with the channel mask.
    WAVEFORMATEXTENSIBLE wfx{};
    WAVEFORMATEX& f = wfx.Format;
    f.wFormatTag = channels == 2 ? WAVE_FORMAT_PCM : WAVE_FORMAT_EXTENSIBLE;
    f.nChannels = static_cast<WORD>(channels);
    f.nSamplesPerSec = sample_rate;
    f.wBitsPerSample = 16;
    f.nBlockAlign = static_cast<WORD>(channels * 2);
    f.nAvgBytesPerSec = sample_rate * f.nBlockAlign;
    if (channels == 6) {
        f.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        wfx.Samples.wValidBitsPerSample = 16;
        wfx.dwChannelMask = kChannelMask51;
        wfx.SubFormat = kSubtypePcm;
    }

    started_.store(false);
    starved_.store(false);
    submitted_frames_.store(0);
    callback_.owner = this;
    hr = engine_->CreateSourceVoice(&source_, &wfx.Format, 0, XAUDIO2_DEFAULT_FREQ_RATIO, &callback_, nullptr, nullptr);
    if (FAILED(hr)) {
        NXLOG_W("audio", "xaudio2: CreateSourceVoice(%u Hz, %u ch) failed (0x%08lx)", sample_rate, channels,
                static_cast<unsigned long>(hr));
        teardown();
        return false;
    }
    hr = source_->Start(0);
    if (FAILED(hr)) {
        NXLOG_W("audio", "xaudio2: source voice Start failed (0x%08lx)", static_cast<unsigned long>(hr));
        teardown();
        return false;
    }
    applied_volume_ = effective_volume();
    source_->SetVolume(applied_volume_);

    {
        std::lock_guard<std::mutex> lk(mu_);
        for (Slot& s : slots_) {
            s.pcm.assign(static_cast<size_t>(kSlotFrames) * channels, 0);
            s.frames = 0;
            s.busy = false;
        }
    }
    channels_ = channels;
    rate_ = sample_rate;
    NXLOG_I("audio", "xaudio2: opened %u Hz, %u channels, volume %.2f", sample_rate, channels, applied_volume_);
    return true;
}

void XAudio2Sink::close() {
    if (!engine_ && !com_owned_) return;
    if (source_) {
        NXLOG_I("audio", "%s", format_stats(name(), stats()).c_str());
        started_.store(false);
        source_->Stop(0);
        source_->FlushSourceBuffers();
        // DestroyVoice waits for the audio thread to leave the callbacks.
        std::lock_guard<std::mutex> vl(voice_mu_);
        source_->DestroyVoice();
        source_ = nullptr;
    }
    teardown();
    std::lock_guard<std::mutex> lk(mu_);
    for (Slot& s : slots_) {
        s.frames = 0;
        s.busy = false;
    }
}

void XAudio2Sink::teardown() {
    if (master_) {
        master_->DestroyVoice();
        master_ = nullptr;
    }
    if (engine_) {
        engine_->Release();
        engine_ = nullptr;
    }
    if (com_owned_) {
        com_owned_ = false;
        if (GetCurrentThreadId() == com_thread_) CoUninitialize();
    }
    channels_ = 0;
}

uint32_t XAudio2Sink::write(const int16_t* pcm, uint32_t frames) {
    const uint32_t accepted = (pcm && frames) ? submit(pcm, frames) : 0;
    if (accepted) {
        // Frames that are not silence (any sample of magnitude 64 or more): the periodic stats line tells silent stretches from playing ones.
        const size_t ch = channels_ ? channels_ : 2;
        uint64_t audible = 0;
        for (uint32_t f = 0; f < accepted; f++) {
            const int16_t* s = pcm + f * ch;
            for (size_t c = 0; c < ch; c++) {
                if (s[c] >= 64 || s[c] <= -64) {
                    audible++;
                    break;
                }
            }
        }
        frames_audible_.fetch_add(audible, std::memory_order_relaxed);
    }
    frames_written_.fetch_add(frames, std::memory_order_relaxed);
    frames_accepted_.fetch_add(accepted, std::memory_order_relaxed);
    frames_dropped_.fetch_add(frames - accepted, std::memory_order_relaxed);
    return accepted;
}

uint32_t XAudio2Sink::submit(const int16_t* pcm, uint32_t frames) {
    if (!source_) return 0;
    const float volume = effective_volume();
    if (volume != applied_volume_) {
        source_->SetVolume(volume);
        applied_volume_ = volume;
    }
    const size_t ch = channels_;
    uint32_t accepted = 0;
    while (accepted < frames) {
        XAUDIO2_VOICE_STATE state{};
        source_->GetState(&state);
        if (state.BuffersQueued >= kMaxQueued) break;

        const uint32_t n = std::min<uint32_t>(frames - accepted, kSlotFrames);
        uint32_t idx = kSlots;
        {
            std::lock_guard<std::mutex> lk(mu_);
            for (uint32_t i = 0; i < kSlots; i++) {
                if (!slots_[i].busy) {
                    idx = i;
                    break;
                }
            }
            if (idx == kSlots) break;   // pool empty: the voice has not returned buffers yet
            Slot& s = slots_[idx];
            s.busy = true;
            s.frames = n;
            std::memcpy(s.pcm.data(), pcm + static_cast<size_t>(accepted) * ch, static_cast<size_t>(n) * ch * sizeof(int16_t));
        }

        XAUDIO2_BUFFER buf{};
        buf.AudioBytes = n * static_cast<UINT32>(ch) * 2;
        buf.pAudioData = reinterpret_cast<const BYTE*>(slots_[idx].pcm.data());
        buf.pContext = reinterpret_cast<void*>(static_cast<uintptr_t>(idx));
        const HRESULT hr = source_->SubmitSourceBuffer(&buf);
        if (FAILED(hr)) {
            release_slot(idx);
            break;
        }
        started_.store(true);
        starved_.store(false);
        submitted_frames_.fetch_add(n, std::memory_order_relaxed);
        note_queued(state.BuffersQueued + 1);
        note_queue_frames(false);
        accepted += n;
    }
    return accepted;
}

// Runs on XAudio2's audio thread. Cheap: one GetState and a few atomic updates.
void XAudio2Sink::on_pass_start(UINT32 bytes_required) noexcept {
    if (bytes_required == 0 || !started_.load() || !source_) return;
    XAUDIO2_VOICE_STATE state{};
    source_->GetState(&state);
    note_queued(state.BuffersQueued);
    note_queue_frames(true);
    if (state.BuffersQueued == 0) {
        if (!starved_.exchange(true)) underruns_.fetch_add(1, std::memory_order_relaxed);
    } else {
        starved_.store(false);
    }
}

void XAudio2Sink::note_queued(uint32_t buffers) noexcept {
    uint32_t peak = queued_peak_.load(std::memory_order_relaxed);
    while (buffers > peak && !queued_peak_.compare_exchange_weak(peak, buffers, std::memory_order_relaxed)) {
    }
}

// The device's play position is SamplesPlayed of the source voice: it counts the samples the voice has consumed, so submitted minus played
// is exact (not rounded to whole buffers). A voice that starved keeps counting only what it really played.
uint32_t XAudio2Sink::queued_now() noexcept {
    IXAudio2SourceVoice* v = source_;
    if (!v) return 0;
    XAUDIO2_VOICE_STATE state{};
    v->GetState(&state);
    const uint64_t submitted = submitted_frames_.load(std::memory_order_relaxed);
    return submitted > state.SamplesPlayed ? static_cast<uint32_t>(submitted - state.SamplesPlayed) : 0u;
}

void XAudio2Sink::note_queue_frames(bool playing) noexcept {
    const uint32_t q = queued_now();
    uint32_t peak = queue_peak_frames_.load(std::memory_order_relaxed);
    while (q > peak && !queue_peak_frames_.compare_exchange_weak(peak, q, std::memory_order_relaxed)) {
    }
    if (playing && q > 0 && submitted_frames_.load(std::memory_order_relaxed) > rate_ / 2 + q) {   // not the start-up fill: after half a second played
        uint32_t low = queue_low_frames_.load(std::memory_order_relaxed);
        while (q < low && !queue_low_frames_.compare_exchange_weak(low, q, std::memory_order_relaxed)) {
        }
    }
}

uint32_t XAudio2Sink::queued_frames() {
    {
        std::lock_guard<std::mutex> vl(voice_mu_);
        if (source_) return queued_now();
    }
    std::lock_guard<std::mutex> lk(mu_);
    uint32_t total = 0;
    for (const Slot& s : slots_) {
        if (s.busy) total += s.frames;
    }
    return total;
}

SinkStats XAudio2Sink::stats() {
    SinkStats s;
    s.frames_written = frames_written_.load(std::memory_order_relaxed);
    s.frames_accepted = frames_accepted_.load(std::memory_order_relaxed);
    s.frames_dropped = frames_dropped_.load(std::memory_order_relaxed);
    s.frames_audible = frames_audible_.load(std::memory_order_relaxed);
    s.underruns = underruns_.load(std::memory_order_relaxed);
    s.queued_peak = queued_peak_.load(std::memory_order_relaxed);
    s.queued_peak_frames = queue_peak_frames_.load(std::memory_order_relaxed);
    const uint32_t low = queue_low_frames_.load(std::memory_order_relaxed);
    s.queued_low_frames = low == 0xffffffffu ? 0 : low;
    s.sample_rate = rate_;
    return s;
}

void XAudio2Sink::release_slot(uint32_t idx) {
    std::lock_guard<std::mutex> lk(mu_);
    if (idx >= kSlots) return;
    slots_[idx].busy = false;
    slots_[idx].frames = 0;
}

}  // namespace

std::shared_ptr<AudioSink> make_xaudio2_sink() {
    // The sink is only useful when the system XAudio2 DLL is there; the device is opened later in open().
    HMODULE lib = LoadLibraryExW(L"xaudio2_9.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!lib) return nullptr;
    FreeLibrary(lib);
    return std::make_shared<XAudio2Sink>();
}

}  // namespace nxrt::audio
