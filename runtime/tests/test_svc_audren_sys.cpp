// SPDX-License-Identifier: GPL-3.0-or-later
// AudioRenderSystem / AudioRendererManager / AudioProcessor / HostAudioDevice (S3c, top of the audio renderer).
//
// Every expected value below is derived by hand from the C# sources under refs/ryubing/src/Ryujinx.Audio/Renderer (Server/AudioRenderSystem.cs,
// Server/AudioRendererManager.cs, Dsp/AudioProcessor.cs, Dsp/Command/DeviceSinkCommand.cs, Dsp/DataSourceHelper.cs, Common/WorkBufferAllocator.cs,
// Common/NodeStates.cs, Common/EdgeMatrix.cs, Server/Splitter/SplitterContext.cs, Server/Mix/MixInfo.cs, Server/Voice/VoiceInfo.cs,
// Parameter sizes from audren_types.h). The derivations are in the comments. No expected value is copied from running the port.
//
// Element sizes used by the work buffer arithmetic (C# Unsafe.SizeOf<T>, explicit StructLayout sizes):
//   float 4, int 4, MixInfo 0x940 (MixInfo.cs Size = 0x940), MemoryPoolInfo 0x20 (MemoryPoolInfo.cs Size = 0x20),
//   SplitterState 0x20 (SplitterState.cs Size = 0x20), SplitterDestinationVersion1 0xE0 (SplitterDestinationVersion1.cs Size = 0xE0),
//   BiquadFilterState 0x20. Alignments: MixInfo 0x10, MemoryPoolInfo 0x10, SplitterState 0x10, SplitterDestinationVersion1 0x10, VoiceState 0x10.
//   Not restated: VoiceInfo and VoiceChannelResource (0xD0). The two work buffer configurations below have no voices, so these never enter
//   the hand sums. Voices are covered by the session test, where a too small work buffer would make OpenAudioRenderer fail.
//
// Constants (C# Constants.cs): VoiceChannelCountMax 6, VoiceWaveBufferCount 4, TargetSampleCount 240, BufferAlignment 0x40,
// WorkBufferAlignment 0x1000, TargetSampleRate 48000, AudioRendererSessionCountMax 2.
#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "../src/services/audren/Server/AudioRendererManager.h"
#include "../src/services/audren/Integration/HostAudioDevice.h"
#include "../src/services/audren/audren_types.h"
#include "nxrt/audio_sink.h"
#include "test_framework.h"

NXTEST_GROUP(svc_audren_sys)

namespace {

using namespace nxrt::audren;

// REV1: BaseRevisionMagic + Revision1 = 'R' 'E' 'V' '0'+1 = 0x31564552. REV2 = 0x32564552. REV10 = 0x3A564552.
const int32_t kRevision1 = BehaviourInfo::BaseRevisionMagic + BehaviourInfo::Revision1;
const int32_t kRevision2 = BehaviourInfo::BaseRevisionMagic + BehaviourInfo::Revision2;
const int32_t kRevision10 = BehaviourInfo::BaseRevisionMagic + BehaviourInfo::Revision10;

// Guest memory (host memory here). Page aligned, because PoolMapper requires 0x1000-aligned cpu addresses for memory pools.
alignas(0x1000) uint8_t gGuest[0x3000];

uint64_t GuestBase() { return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(gGuest)); }

// ---- Work buffer size: two hand-computed configurations ----

// Configuration A (REV1: no splitter, no node states): sample_count 240, mix_buffer_count 4, sub_mix_buffer_count 1 (mixesCount 2),
// sink_count 1, effect_count 0, voice_count 0 (memoryPoolCount 0), splitter 0/0, no performance buffer.
//   Term by term, GetTargetSize<T>(size, count, align) = AlignUp(size, align) + sizeof(T) * count:
//   1. Mix buffers   <float>(0, 240 * (6 + 4) = 2400, 0x10)         = 0 + 4 * 2400                     = 9600
//   2. Upsampler     <float>(9600, 240 * (6 + 4) * (1 + 1) = 4800, 0x10) = 9600 + 4 * 4800              = 28800
//   3. Depop         <float>(28800, AlignUp(4, 0x40) = 64, 0x40)     = AlignUp(28800, 64) + 4 * 64      = 28800 + 256 = 29056
//                    (28800 = 64 * 450, so the alignment adds nothing)
//   4. Voice         <VoiceInfo>, <int>, <VoiceChannelResource>, <VoiceState>, all with count 0:
//                    each adds only AlignUp(29056, 16) = 29056 (29056 = 16 * 1816)
//   5. Mix           <MixInfo>(29056, 2, 0x10)                       = 29056 + 2 * 0x940 = 29056 + 4736 = 33792
//   6. Effect order  <int>(33792, 0 * 2, 0x10)                       = 33792
//   7. Sorted mixes  <int>(33792, 2, 0x10)                           = 33792 + 2 * 4 = 33800
//   8. Splitter: REV1 has no splitter, so no node-state term and SplitterContext.GetWorkBufferSize returns 33800 unchanged.
//   9. Memory pool   <MemoryPoolInfo>(33800, 0, 0x10)                = AlignUp(33800, 16) = 33808 (33800 = 16 * 2112 + 8)
//  10. DSP voice     <VoiceState>(33808, 0, 0x10)                    = 33808
//  11. Performance: none.
//  Result: AlignUp(33808, 0x1000) = 9 * 0x1000 = 36864 = 0x9000.
const uint64_t kExpectedWorkBufferRev1NoVoices = 36864;

// Configuration B (REV2: splitter supported, version 1 destinations, no biquad splitter, no splitter bug fix): sample_count 240,
// mix_buffer_count 6, sub_mix_buffer_count 2 (mixesCount 3), sink_count 2, effect_count 2 (memoryPoolCount 2), voice_count 0,
// splitter_count 1, splitter_destination_count 2, no performance buffer.
//   1. Mix buffers   <float>(0, 240 * (6 + 6) = 2880, 0x10)          = 11520
//   2. Upsampler     <float>(11520, 240 * 12 * (2 + 2) = 11520, 0x10) = 11520 + 46080 = 57600
//   3. Depop         <float>(57600, AlignUp(6, 64) = 64, 0x40)        = 57600 + 256 = 57856 (57600 = 64 * 900)
//   4. Voice         four zero-count terms                             = 57856 (57856 = 16 * 3616)
//   5. Mix           <MixInfo>(57856, 3, 0x10)                         = 57856 + 3 * 0x940 = 57856 + 7104 = 64960
//   6. Effect order  <int>(64960, 2 * 3 = 6, 0x10)                     = 64960 + 24 = 64984
//   7. Sorted mixes  <int>(64984, 3, 0x10)                             = AlignUp(64984, 16) + 12 = 64992 + 12 = 65004
//   8. Node states (splitter supported): NodeStates.GetWorkBufferSize(3) = CalcBufferSize(9) + 0xC * 3 + 2 * EdgeMatrix(3)
//        = 9 * 4 + 36 + 2 * AlignUp(9, 64) = 36 + 36 + 128 = 200
//      EdgeMatrix.GetWorkBufferSize(3) = AlignUp(9, 64) = 64. Sum 264. size += AlignUp(264, 16) = 272 (no pre-alignment of size): 65004 + 272 = 65276
//   9. Memory pool   <MemoryPoolInfo>(65276, 2, 0x10)                  = AlignUp(65276, 16) + 2 * 0x20 = 65280 + 64 = 65344
//  10. Splitter      <SplitterState>(65344, 1, 0x10)                    = 65344 + 0x20 = 65376
//                    <SplitterDestinationVersion1>(65376, 2, 0x10)      = 65376 + 2 * 0xE0 = 65376 + 448 = 65824
//                    (no splitter bug fix below REV5, so no int term; no biquad states below REV12)
//  11. DSP voice     <VoiceState>(65824, 0, 0x10)                       = 65824
//  12. Performance: none.
//  Result: AlignUp(65824, 0x1000) = 17 * 0x1000 = 69632 = 0x11000.
const uint64_t kExpectedWorkBufferRev2Splitter = 69632;

void CheckWorkBufferSizes(AudioRendererConfiguration& config, uint64_t expected) {
    CHECK_EQ(AudioRenderSystem::GetWorkBufferSize(config), expected);
    CHECK_EQ(AudioRendererManager::GetWorkBufferSize(config), expected);
}

// ---- Session test: a PCM16 voice through the whole chain ----

// A sink that keeps what the host receives (NullSink counts frames; this one also keeps the samples).
class CaptureSink : public nxrt::audio::NullSink {
  public:
    std::mutex mutex;
    std::vector<int16_t> samples;  // interleaved, `channels` per frame

    uint32_t write(const int16_t* pcm, uint32_t frames) override {
        {
            std::lock_guard<std::mutex> guard(mutex);
            samples.insert(samples.end(), pcm, pcm + static_cast<size_t>(frames) * channels);
        }
        return NullSink::write(pcm, frames);
    }
};

// A session event that counts Signal and Clear calls.
class CountingEvent : public IWritableEvent {
  public:
    std::atomic<int> signals{0};
    std::atomic<int> clears{0};

    void Signal() override { signals++; }
    void Clear() override { clears++; }
};

// Counts of the test session. Sizes follow the C# structs (see the header of the file for the names).
const uint32_t kPoolCount = 8;      // effect_count 0 + voice_count 2 * VoiceWaveBufferCount 4
const uint32_t kVoiceCount = 2;
const uint32_t kMixCount = 2;       // sub_mix_buffer_count 1 + 1
const uint32_t kSinkCount = 1;

// Output layout of the first Update (see the derivation in BuildInput): header 0x40, pools 8 * 0x10, voices 2 * 0x10, effects 0,
// sink 0x20, performance 0x10, behaviour 0xB0, render info 0x10.
//   0x40 + 0x80 + 0x20 + 0x20 + 0x10 + 0xB0 + 0x10 = 0x1D0
const uint64_t kExpectedOutputSize = 0x1D0;

// Input layout (C# call order). Section sizes:
//   UpdateDataHeader                 0x40
//   BehaviourParameter               0x10
//   MemoryPoolInParameter x 8        8 * 0x20 = 0x100
//   VoiceChannelResourceInParameter x 2   2 * 0x70 = 0xE0
//   VoiceInParameter1 x 2            2 * 0x170 = 0x2E0
//   effects                          0 (effect_count 0)
//   MixInParameterDirtyOnlyUpdate    0x20, then MixParameter x 2 = 2 * 0x930 = 0x1260: 0x1280
//   splitter                         0 (splitter_count 0; the splitter update consumes nothing)
//   SinkInParameter x 1              0x140
//   PerformanceInParameter           0x10
//   total: 0x40 + 0x10 + 0x100 + 0xE0 + 0x2E0 + 0x1280 + 0x140 + 0x10 = 0x18E0
const uint64_t kExpectedInputSize = 0x18E0;

// Samples per update (sample_count) and the constant value of the PCM16 voice.
const int16_t kPcmValue = 1000;

std::vector<uint8_t> BuildInput(bool voiceIsNew, bool sentToServer) {
    std::vector<uint8_t> input;

    UpdateDataHeader header{};
    header.revision = kRevision10;
    header.behaviour_size = static_cast<uint32_t>(sizeof(BehaviourParameter));
    header.memory_pools_size = kPoolCount * static_cast<uint32_t>(sizeof(MemoryPoolInParameter));
    header.voices_size = kVoiceCount * static_cast<uint32_t>(sizeof(VoiceInParameter1));
    header.voice_resources_size = kVoiceCount * static_cast<uint32_t>(sizeof(VoiceChannelResourceInParameter));
    header.effects_size = 0;
    header.mixes_size = static_cast<uint32_t>(sizeof(MixInParameterDirtyOnlyUpdate) + kMixCount * sizeof(MixParameter));
    header.sinks_size = kSinkCount * static_cast<uint32_t>(sizeof(SinkInParameter));
    header.performance_buffer_size = static_cast<uint32_t>(sizeof(PerformanceInParameter));
    header.splitter_size = 0;
    header.render_info_size = 0;
    header.total_size = static_cast<uint32_t>(kExpectedInputSize);

    auto append = [&input](const auto& value) {
        size_t offset = input.size();
        input.resize(offset + sizeof(value));
        std::memcpy(input.data() + offset, &value, sizeof(value));
    };

    append(header);

    BehaviourParameter behaviour{};
    behaviour.user_revision = kRevision10;
    behaviour.flags = 0;
    append(behaviour);

    // Pool 0 asks for an attach of the first 0x2000 bytes of guest memory (the samples live inside it). Pools 1..7 are Invalid: no change.
    MemoryPoolInParameter pool0{};
    pool0.cpu_address = GuestBase();
    pool0.size = 0x2000;
    pool0.state = MemoryPoolUserState::RequestAttach;
    append(pool0);
    for (uint32_t i = 1; i < kPoolCount; i++) {
        MemoryPoolInParameter none{};
        none.state = MemoryPoolUserState::Invalid;
        append(none);
    }

    // Channel resources 0 and 1, both used. Resource 0 sends the voice to mix buffers 0 and 1 (the two channels of the stereo device) at 1.0.
    for (uint32_t id = 0; id < kVoiceCount; id++) {
        VoiceChannelResourceInParameter resource{};
        resource.id = id;
        resource.is_used = true;
        if (id == 0) {
            resource.mix[0] = 1.0f;
            resource.mix[1] = 1.0f;
        }
        append(resource);
    }

    // Voice 0: PCM16, mono, 48000 Hz, Start, unit volume and pitch, one wave buffer at guest +0x1000 (0x1000 bytes), samples [0, 240),
    // looping. The decoding flag SkipPitchAndSampleRateConversion copies the decoded samples straight to the output: DataSourceHelper.cs
    // `outputBuffer[j] = tempBuffer[j]`. Without it the default-quality resampler runs, whose first phase taps sum to 32635 / 32768, so
    // a constant 1000 would come out near 995. Voice 1 is not in use (all zero).
    VoiceInParameter1 voice0{};
    voice0.id = 0;
    voice0.node_id = 5;
    voice0.is_new = voiceIsNew;
    voice0.in_use = true;
    voice0.play_state = PlayState::Start;
    voice0.sample_format = SampleFormat::PcmInt16;
    voice0.sample_rate = 48000;
    voice0.priority = 8;
    voice0.sorting_order = 1;
    voice0.channel_count = 1;
    voice0.pitch = 1.0f;
    voice0.volume = 1.0f;
    voice0.wave_buffers_count = 1;
    voice0.wave_buffers_index = 0;
    voice0.data_source_state_address = 0;
    voice0.data_source_state_size = 0;
    voice0.mix_id = 0;
    voice0.splitter_id = Constants::UnusedSplitterId;
    voice0.wave_buffers[0].address = GuestBase() + 0x1000;
    voice0.wave_buffers[0].size = 0x1000;
    voice0.wave_buffers[0].start_sample_offset = 0;
    voice0.wave_buffers[0].end_sample_offset = 240;
    voice0.wave_buffers[0].should_loop = true;
    voice0.wave_buffers[0].loop_count = -1;
    voice0.wave_buffers[0].loop_first_sample_offset = 0;
    voice0.wave_buffers[0].loop_last_sample_offset = 240;
    voice0.wave_buffers[0].sent_to_server = sentToServer;
    for (int k = 1; k < 4; k++) {
        voice0.wave_buffers[k].sent_to_server = true;
    }
    voice0.channel_resource_ids[0] = 0;
    voice0.decoding_behaviour_flags = DecodingBehaviour::SkipPitchAndSampleRateConversion;
    voice0.src_quality = SampleRateConversionQuality::Default;
    append(voice0);

    VoiceInParameter1 voice1{};
    append(voice1);

    // Mixes (dirty-only form, mix_count 2). Mix 0 is the final mix: unit volume, 2 buffers, no destination. Mix 1 is not used.
    MixInParameterDirtyOnlyUpdate dirty{};
    dirty.magic = 0;
    dirty.mix_count = kMixCount;
    append(dirty);

    MixParameter mix0{};
    mix0.volume = 1.0f;
    mix0.sample_rate = 48000;
    mix0.buffer_count = 2;
    mix0.is_used = true;
    mix0.mix_id = 0;
    mix0.effect_count = 0;
    mix0.node_id = 0;
    mix0.destination_mix_id = Constants::UnusedMixId;
    mix0.destination_splitter_id = Constants::UnusedSplitterId;
    append(mix0);

    MixParameter mix1{};
    mix1.mix_id = 1;
    mix1.node_id = 1;
    mix1.destination_mix_id = Constants::UnusedMixId;
    mix1.destination_splitter_id = Constants::UnusedSplitterId;
    append(mix1);

    // Sink 0: device, used, two input channels read from final mix buffers 0 and 1, no down-mix.
    DeviceParameter device{};
    device.input_count = 2;
    device.input[0] = 0;
    device.input[1] = 1;
    device.down_mix_parameter_enabled = false;
    SinkInParameter sink{};
    sink.type = SinkType::Device;
    sink.is_used = true;
    sink.magic = 0xCAFE;
    sink.node_id = 7;
    std::memcpy(sink.specific_data, &device, sizeof(device));
    append(sink);

    // Performance: no manager is attached, so the history is 0. The 0x10-byte section must still be present.
    PerformanceInParameter performance{};
    performance.target_node_id = 0;
    append(performance);

    return input;
}

// Read a struct at a byte offset of an output buffer.
template <class T>
const T* At(const std::vector<uint8_t>& buffer, size_t offset) {
    return reinterpret_cast<const T*>(buffer.data() + offset);
}

// Runs fn on a helper thread and waits at most `limit`. Returns false on a timeout (the helper is then detached).
bool RunWithWatchdog(std::function<void()> fn, std::chrono::seconds limit) {
    auto done = std::make_shared<std::promise<void>>();
    std::future<void> finished = done->get_future();

    std::thread helper([fn, done] {
        fn();
        done->set_value();
    });

    if (finished.wait_for(limit) == std::future_status::ready) {
        helper.join();
        return true;
    }

    helper.detach();
    return false;
}

}  // namespace

NXTEST(work_buffer_size_rev1_without_voices) {
    AudioRendererConfiguration config{};
    config.sample_rate = 48000;
    config.sample_count = 240;
    config.mix_buffer_count = 4;
    config.sub_mix_buffer_count = 1;
    config.voice_count = 0;
    config.sink_count = 1;
    config.effect_count = 0;
    config.splitter_count = 0;
    config.splitter_destination_count = 0;
    config.performance_metric_frames_count = 0;
    config.rendering_device = server::AudioRendererRenderingDevice::Dsp;
    config.execution_mode = server::AudioRendererExecutionMode::Auto;
    config.revision = kRevision1;

    CheckWorkBufferSizes(config, kExpectedWorkBufferRev1NoVoices);
}

NXTEST(work_buffer_size_rev2_with_splitter) {
    AudioRendererConfiguration config{};
    config.sample_rate = 48000;
    config.sample_count = 240;
    config.mix_buffer_count = 6;
    config.sub_mix_buffer_count = 2;
    config.voice_count = 0;
    config.sink_count = 2;
    config.effect_count = 2;
    config.splitter_count = 1;
    config.splitter_destination_count = 2;
    config.performance_metric_frames_count = 0;
    config.rendering_device = server::AudioRendererRenderingDevice::Dsp;
    config.execution_mode = server::AudioRendererExecutionMode::Auto;
    config.revision = kRevision2;

    CheckWorkBufferSizes(config, kExpectedWorkBufferRev2Splitter);
}

NXTEST(session_plays_pcm16_voice_to_device_sink) {
    // The guest samples: 240 PCM16 values of 1000 at guest +0x1000.
    int16_t* samples = reinterpret_cast<int16_t*>(gGuest + 0x1000);
    for (int i = 0; i < 240; i++) {
        samples[i] = kPcmValue;
    }

    auto capture = std::make_shared<CaptureSink>();
    nxrt::audio::set_sink(capture);

    CountingEvent events[4];
    std::vector<IWritableEvent*> eventPointers = {&events[0], &events[1], &events[2], &events[3]};

    HostAudioDeviceDriver driver;
    AudioRendererManager manager;
    manager.Initialize(eventPointers, &driver);

    AudioRendererConfiguration config{};
    config.sample_rate = 48000;
    config.sample_count = 240;
    config.mix_buffer_count = 4;
    config.sub_mix_buffer_count = 1;
    config.voice_count = kVoiceCount;
    config.sink_count = kSinkCount;
    config.effect_count = 0;
    config.performance_metric_frames_count = 0;
    config.voice_drop_enabled = false;
    config.rendering_device = server::AudioRendererRenderingDevice::Dsp;
    config.execution_mode = server::AudioRendererExecutionMode::Auto;
    config.splitter_count = 0;
    config.splitter_destination_count = 0;
    config.revision = kRevision10;

    std::unique_ptr<AudioRenderSystem> renderer;
    const uint64_t workBufferSize = AudioRendererManager::GetWorkBufferSize(config);
    REQUIRE(workBufferSize == AudioRenderSystem::GetWorkBufferSize(config));

    ResultCode open = manager.OpenAudioRenderer(renderer, config, 0, GuestBase(), workBufferSize, 0x1234);
    REQUIRE(open == ResultCode::Success);
    REQUIRE(renderer != nullptr);

    // First update, renderer not started: the voice is new and in use. The voice has not been played yet, so played_sample_count is 0.
    std::vector<uint8_t> input = BuildInput(true, false);
    CHECK_EQ(input.size(), kExpectedInputSize);

    std::vector<uint8_t> output(0x1000, 0xAA);
    std::vector<uint8_t> performanceOutput(0x10, 0xAA);
    ResultCode update = renderer->Update(Span<uint8_t>(output.data(), output.size()), Span<uint8_t>(performanceOutput.data(), performanceOutput.size()),
                                         Span<const uint8_t>(input.data(), input.size()));
    REQUIRE(update == ResultCode::Success);

    const UpdateDataHeader* outHeader = At<UpdateDataHeader>(output, 0);
    CHECK_EQ(outHeader->total_size, static_cast<uint32_t>(kExpectedOutputSize));
    CHECK_EQ(outHeader->revision, kRevision10);
    CHECK_EQ(outHeader->memory_pools_size, 0x80u);
    CHECK_EQ(outHeader->voices_size, 0x20u);
    CHECK_EQ(outHeader->sinks_size, 0x20u);
    CHECK_EQ(outHeader->performance_buffer_size, 0x10u);
    CHECK_EQ(outHeader->behaviour_size, 0xB0u);
    CHECK_EQ(outHeader->render_info_size, 0x10u);
    // Voice 0's status is at 0x40 + 0x80 = 0xC0.
    CHECK_EQ(At<VoiceOutStatus>(output, 0xC0)->played_sample_count, 0ull);

    // Start the renderer: frames are generated and processed every 5 ms.
    renderer->Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // The system event is signalled once per generated frame.
    CHECK(events[0].signals.load() >= 10);

    // Stop sending new frames.
    renderer->Stop();
    CHECK(!renderer->IsActive());

    // Every sample the device received: stereo, 1000 in both channels, with unit volumes. The expected count is at least 10 frames of 240
    // samples per channel (the frames are generated while the renderer is active).
    std::vector<int16_t> received;
    {
        std::lock_guard<std::mutex> guard(capture->mutex);
        received = capture->samples;
    }
    CHECK_EQ(capture->channels, 2u);
    CHECK_EQ(capture->rate, 48000u);
    CHECK(received.size() >= static_cast<size_t>(10) * 240 * 2);
    CHECK_EQ(received.size() % 2, static_cast<size_t>(0));
    size_t mismatches = 0;
    for (int16_t sample : received) {
        if (sample != kPcmValue) {
            mismatches++;
        }
    }
    CHECK_MSG(mismatches == 0, "some device samples are not 1000");

    // Second update, renderer stopped: the voice is not new any more. The played-sample counter has advanced: the output status is
    // read from the CPU copy of the voice state, which is refreshed each frame, so it lags at most one frame. It counts 240 samples per
    // processed frame.
    std::vector<uint8_t> input2 = BuildInput(false, true);
    ResultCode update2 = renderer->Update(Span<uint8_t>(output.data(), output.size()), Span<uint8_t>(performanceOutput.data(), performanceOutput.size()),
                                          Span<const uint8_t>(input2.data(), input2.size()));
    REQUIRE(update2 == ResultCode::Success);
    uint64_t played = At<VoiceOutStatus>(output, 0xC0)->played_sample_count;
    CHECK(played >= static_cast<uint64_t>(9) * 240);
    CHECK_EQ(played % 240, 0ull);

    // Close the renderer. Its destruction disposes it, unregisters it, and stops the worker and the processor: the join must not hang.
    bool closed = RunWithWatchdog([&renderer] { renderer.reset(); }, std::chrono::seconds(5));
    CHECK_MSG(closed, "closing the renderer did not finish within 5 s (worker thread join hang)");

    // Restore the default sink for later tests in the same process.
    nxrt::audio::set_sink(std::make_shared<nxrt::audio::NullSink>());
}

// platform 2 native renderer tag: 'REv' (lower-case v) + the revision digit, native revisions run one past ours (Eglantine NX2_NOTES rows
// 38-39). The tag is accepted up to revision 16 and an update header may differ from the open call in the case of the 'V' only.
NXTEST(native_revision_tag_is_accepted_and_compared_by_number) {
    constexpr int32_t kCase = 0x20 << 16;
    const int32_t rev15 = BehaviourInfo::BaseRevisionMagic + BehaviourInfo::Revision15;
    const int32_t rev16 = BehaviourInfo::BaseRevisionMagic + (16 << 24);
    CHECK(BehaviourInfo::CheckValidRevision(rev15));
    CHECK(BehaviourInfo::CheckValidRevision(rev15 + kCase));   // 'REv?'
    CHECK(BehaviourInfo::CheckValidRevision(rev16 + kCase));   // native revision 16
    CHECK(!BehaviourInfo::CheckValidRevision(rev16));          // 'REV' + 16 stays invalid
    CHECK_EQ(BehaviourInfo::GetRevisionNumber(rev15 + kCase), 15);
    CHECK(BehaviourInfo::SameRevision(rev15, rev15 + kCase));
    CHECK(BehaviourInfo::SameRevision(rev15 + kCase, rev15));
    CHECK(!BehaviourInfo::SameRevision(rev15, kRevision10 + kCase));
    CHECK(!BehaviourInfo::SameRevision(rev15, rev15 + 1));
}
