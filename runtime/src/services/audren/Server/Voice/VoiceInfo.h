// SPDX-License-Identifier: GPL-3.0-or-later
// Port of Ryujinx.Audio/Renderer/Server/Voice/VoiceInfo.cs (Ryubing fork, MIT).
//
// Conventions specific to this file:
//   * The C# class names `PlayState`, `SampleFormat` and `DecodingBehaviour` are also the names of the public fields, which C#
//     allows and C++ does not inside the class. The fields keep the C# name and their types are always written qualified
//     (`::nxrt::audren::SampleFormat`, `server::PlayState`, ...).
//   * PlayState here is the server one (nxrt::audren::server::PlayState); the parameter's PlayState is nxrt::audren::PlayState.
//   * WaveBuffer here is the server one (server::WaveBuffer, see WaveBuffer.h).
//   * C# `out ErrorInfo[]` becomes an out reference to std::vector<BehaviourParameter::ErrorInfo>.
//   * C# `ReadOnlySpan<Memory<VoiceState>>` / `Memory<VoiceState>[]` become Span<const Span<VoiceState>> / Span<Span<VoiceState>>.
//   * The C# static ObjectPool of voice-state arrays is not ported; UpdateForCommandGeneration uses a local array.
//   * The C# extension WaveBufferInternal.IsSampleOffsetValid (defined in Renderer/Parameter/VoiceInParameter1.cs) is ported as the
//     free function IsSampleOffsetValid below, because audren_types.h carries no methods.
//   * UpdateForCommandGeneration and ResetResources need a complete VoiceContext, so their bodies are at the end of
//     VoiceContext.h. This header ends by including it; both include orders work.
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <vector>

#include "../../Constants.h"
#include "../../Dsp/AdpcmHelper.h"
#include "../../Dsp/BiquadFilterHelper.h"
#include "../../audren_common.h"
#include "../../audren_types.h"
#include "../BehaviourInfo.h"
#include "../MemoryPool/AddressInfo.h"
#include "../MemoryPool/PoolMapper.h"
#include "WaveBuffer.h"

namespace nxrt::timestep {
bool av_trace_on();
void av_note_sound(const char* kind, int64_t id, int64_t a, int64_t b);
}  // namespace nxrt::timestep

namespace nxrt::audren {

class VoiceContext;

// C# WaveBufferInternal.IsSampleOffsetValid(SampleFormat) (Renderer/Parameter/VoiceInParameter1.cs).
inline bool IsSampleOffsetValid(const WaveBufferInternal& waveBuffer, ::nxrt::audren::SampleFormat format) {
    auto inRangeForPcm = [&waveBuffer](uint32_t dataTypeSize) {
        return static_cast<uint64_t>(waveBuffer.start_sample_offset) * dataTypeSize <= waveBuffer.size &&
               static_cast<uint64_t>(waveBuffer.end_sample_offset) * dataTypeSize <= waveBuffer.size;
    };

    switch (format) {
        case ::nxrt::audren::SampleFormat::PcmInt16:
            return inRangeForPcm(sizeof(uint16_t));
        case ::nxrt::audren::SampleFormat::PcmFloat:
            return inRangeForPcm(sizeof(float));
        case ::nxrt::audren::SampleFormat::Adpcm:
            return AdpcmHelper::GetAdpcmDataSize(static_cast<int32_t>(waveBuffer.start_sample_offset)) <= waveBuffer.size &&
                   AdpcmHelper::GetAdpcmDataSize(static_cast<int32_t>(waveBuffer.end_sample_offset)) <= waveBuffer.size;
        default:
            NXAUDREN_FATAL("IsSampleOffsetValid: sample format not implemented");
    }
}

class VoiceInfo {
  public:
    static constexpr int32_t Alignment = 0x10;

    // Set to true if the voice is used.
    bool InUse = false;

    // Set to true if the voice is new.
    bool IsNew = false;

    bool WasPlaying = false;

    // The SampleFormat of the voice.
    ::nxrt::audren::SampleFormat SampleFormat = ::nxrt::audren::SampleFormat::Invalid;

    // The sample rate of the voice.
    uint32_t SampleRate = 0;

    // The total channel count used.
    uint32_t ChannelsCount = 0;

    // Id of the voice.
    int32_t Id = 0;

    // Node id of the voice.
    int32_t NodeId = 0;

    // The target mix id of the voice.
    int32_t MixId = 0;

    // The current voice PlayState (server). The zero value is Started, as in the C# default.
    server::PlayState PlayState = server::PlayState::Started;

    // The previous voice PlayState (server).
    server::PlayState PreviousPlayState = server::PlayState::Started;

    // The priority of the voice.
    uint32_t Priority = 0;

    // Target sorting position of the voice. (used to sort voice with the same Priority)
    uint32_t SortingOrder = 0;

    // The pitch used on the voice.
    float Pitch = 0.0f;

    // The output volume of the voice.
    float Volume = 0.0f;

    // The previous output volume of the voice.
    float PreviousVolume = 0.0f;

    // Biquad filters to apply to the output of the voice (C# Array2<BiquadFilterParameter2>).
    std::array<BiquadFilterParameter2, VoiceBiquadFilterCount> BiquadFilters{};

    // Total count of WaveBufferInternal of the voice.
    uint32_t WaveBuffersCount = 0;

    // Current playing WaveBufferInternal of the voice.
    uint32_t WaveBuffersIndex = 0;

    // Change the behaviour of the voice. (REV5)
    ::nxrt::audren::DecodingBehaviour DecodingBehaviour = ::nxrt::audren::DecodingBehaviour::Default;

    // User state AddressInfo required by the data source. Only used for SampleFormat.Adpcm.
    AddressInfo DataSourceStateAddressInfo;

    // The wavebuffers of this voice (C# Array4<WaveBuffer>).
    std::array<server::WaveBuffer, VoiceWaveBufferCount> WaveBuffers{};

    // The channel resource ids associated to the voice (C# Array6<int>).
    std::array<int32_t, VoiceChannelCountMax> ChannelResourceIds{};

    // The target splitter id of the voice.
    uint32_t SplitterId = 0;

    // Change the Sample Rate Conversion (SRC) quality of the voice. (REV8)
    ::nxrt::audren::SampleRateConversionQuality SrcQuality = ::nxrt::audren::SampleRateConversionQuality::Default;

    // If set to true, the voice was dropped.
    bool VoiceDropFlag = false;

    // Set to true if the data source state work buffer wasn't mapped.
    bool DataSourceStateUnmapped = false;

    // Set to true if any of the WaveBuffer.BufferAddressInfo work buffer wasn't mapped.
    bool BufferInfoUnmapped = false;

    // Flush the amount of wavebuffer specified. This will result in the wavebuffer being skipped and marked played. (REV5)
    uint8_t FlushWaveBufferCount = 0;

  private:
    // The biquad filter initialization state storage. Mutable so that the const accessor below can return a writable view,
    // as the C# property does.
    mutable std::array<bool, VoiceBiquadFilterCount> _biquadFilterNeedInitialization{};

    // C# static List<ErrorInfo> _waveBufferUpdaterErrorInfosList (shared by all voices, as in C#).
    inline static std::vector<BehaviourParameter::ErrorInfo> _waveBufferUpdaterErrorInfosList;

  public:
    // The biquad filter initialization state array (C# property with a body).
    Span<bool> BiquadFilterNeedInitialization() const {
        return Span<bool>(_biquadFilterNeedInitialization.data(), _biquadFilterNeedInitialization.size());
    }

    // Initialize the VoiceInfo.
    void Initialize() {
        IsNew = false;
        VoiceDropFlag = false;
        DataSourceStateUnmapped = false;
        BufferInfoUnmapped = false;
        FlushWaveBufferCount = 0;
        PlayState = server::PlayState::Stopped;
        Priority = static_cast<uint32_t>(Constants::VoiceLowestPriority);
        Id = 0;
        NodeId = 0;
        SampleRate = 0;
        SampleFormat = ::nxrt::audren::SampleFormat::Invalid;
        ChannelsCount = 0;
        Pitch = 0.0f;
        Volume = 0.0f;
        PreviousVolume = 0.0f;
        BiquadFilters.fill(BiquadFilterParameter2{});
        WaveBuffersCount = 0;
        WaveBuffersIndex = 0;
        MixId = Constants::UnusedMixId;
        SplitterId = Constants::UnusedSplitterId;
        DataSourceStateAddressInfo.Setup(0, 0);

        InitializeWaveBuffers();
    }

    // Check if the voice needs to be skipped. Returns true if the voice needs to be skipped.
    bool ShouldSkip() const { return !InUse || WaveBuffersCount == 0 || DataSourceStateUnmapped || BufferInfoUnmapped || VoiceDropFlag; }

    // Return true if the mix has any destinations.
    bool HasAnyDestination() const { return MixId != Constants::UnusedMixId || SplitterId != Constants::UnusedSplitterId; }

    // Update the internal state from a user parameter (VoiceInParameter2).
    void UpdateParameters2(BehaviourParameter::ErrorInfo& outErrorInfo, const VoiceInParameter2& parameter, PoolMapper& poolMapper,
                           BehaviourInfo& behaviourInfo) {
        InUse = parameter.in_use;
        Id = parameter.id;
        NodeId = parameter.node_id;

        UpdatePlayState(parameter.play_state);

        SrcQuality = parameter.src_quality;

        Priority = parameter.priority;
        SortingOrder = parameter.sorting_order;
        SampleRate = parameter.sample_rate;
        SampleFormat = parameter.sample_format;
        ChannelsCount = parameter.channel_count;
        Pitch = parameter.pitch;
        Volume = parameter.volume;
        for (size_t i = 0; i < BiquadFilters.size(); i++) {
            BiquadFilters[i] = parameter.biquad_filters[i];
        }
        WaveBuffersCount = parameter.wave_buffers_count;
        WaveBuffersIndex = parameter.wave_buffers_index;

        if (behaviourInfo.IsFlushVoiceWaveBuffersSupported()) {
            FlushWaveBufferCount = static_cast<uint8_t>(FlushWaveBufferCount + parameter.flush_wave_buffer_count);
        }

        MixId = parameter.mix_id;

        if (behaviourInfo.IsSplitterSupported()) {
            SplitterId = parameter.splitter_id;
        } else {
            SplitterId = Constants::UnusedSplitterId;
        }

        std::copy(std::begin(parameter.channel_resource_ids), std::end(parameter.channel_resource_ids), ChannelResourceIds.begin());

        ::nxrt::audren::DecodingBehaviour behaviour = ::nxrt::audren::DecodingBehaviour::Default;

        if (behaviourInfo.IsDecodingBehaviourFlagSupported()) {
            behaviour = parameter.decoding_behaviour_flags;
        }

        DecodingBehaviour = behaviour;

        if (parameter.reset_voice_drop_flag) {
            VoiceDropFlag = false;
        }

        if (ShouldUpdateDataSourceState(parameter.data_source_state_address, parameter.data_source_state_size)) {
            DataSourceStateUnmapped = !poolMapper.TryAttachBuffer(outErrorInfo, DataSourceStateAddressInfo, parameter.data_source_state_address,
                                                                  parameter.data_source_state_size);
        } else {
            outErrorInfo = BehaviourParameter::ErrorInfo{};
        }
    }

    // Update the internal state from a user parameter (VoiceInParameter1).
    void UpdateParameters1(BehaviourParameter::ErrorInfo& outErrorInfo, const VoiceInParameter1& parameter, PoolMapper& poolMapper,
                           BehaviourInfo& behaviourInfo) {
        InUse = parameter.in_use;
        Id = parameter.id;
        NodeId = parameter.node_id;

        UpdatePlayState(parameter.play_state);

        SrcQuality = parameter.src_quality;

        Priority = parameter.priority;
        SortingOrder = parameter.sorting_order;
        SampleRate = parameter.sample_rate;
        SampleFormat = parameter.sample_format;
        ChannelsCount = parameter.channel_count;
        Pitch = parameter.pitch;
        Volume = parameter.volume;
        BiquadFilters[0] = BiquadFilterHelper::ToBiquadFilterParameter2(parameter.biquad_filters[0]);
        BiquadFilters[1] = BiquadFilterHelper::ToBiquadFilterParameter2(parameter.biquad_filters[1]);
        WaveBuffersCount = parameter.wave_buffers_count;
        WaveBuffersIndex = parameter.wave_buffers_index;

        if (behaviourInfo.IsFlushVoiceWaveBuffersSupported()) {
            FlushWaveBufferCount = static_cast<uint8_t>(FlushWaveBufferCount + parameter.flush_wave_buffer_count);
        }

        MixId = parameter.mix_id;

        if (behaviourInfo.IsSplitterSupported()) {
            SplitterId = parameter.splitter_id;
        } else {
            SplitterId = Constants::UnusedSplitterId;
        }

        std::copy(std::begin(parameter.channel_resource_ids), std::end(parameter.channel_resource_ids), ChannelResourceIds.begin());

        ::nxrt::audren::DecodingBehaviour behaviour = ::nxrt::audren::DecodingBehaviour::Default;

        if (behaviourInfo.IsDecodingBehaviourFlagSupported()) {
            behaviour = parameter.decoding_behaviour_flags;
        }

        DecodingBehaviour = behaviour;

        if (parameter.reset_voice_drop_flag) {
            VoiceDropFlag = false;
        }

        if (ShouldUpdateDataSourceState(parameter.data_source_state_address, parameter.data_source_state_size)) {
            DataSourceStateUnmapped = !poolMapper.TryAttachBuffer(outErrorInfo, DataSourceStateAddressInfo, parameter.data_source_state_address,
                                                                  parameter.data_source_state_size);
        } else {
            outErrorInfo = BehaviourParameter::ErrorInfo{};
        }
    }

    // Update the internal play state from user play state (C# Common.PlayState).
    void UpdatePlayState(::nxrt::audren::PlayState userPlayState) {
        server::PlayState oldServerPlayState = PlayState;

        PreviousPlayState = oldServerPlayState;

        server::PlayState newServerPlayState = server::PlayState::Stopped;

        switch (userPlayState) {
            case ::nxrt::audren::PlayState::Start:
                newServerPlayState = server::PlayState::Started;
                break;

            case ::nxrt::audren::PlayState::Stop:
                if (oldServerPlayState == server::PlayState::Stopped) {
                    return;
                }

                newServerPlayState = server::PlayState::Stopping;
                break;

            case ::nxrt::audren::PlayState::Pause:
                newServerPlayState = server::PlayState::Paused;
                break;

            default:
                NXAUDREN_FATAL("Unhandled PlayState");
        }

        if (newServerPlayState == server::PlayState::Started && oldServerPlayState != server::PlayState::Started && ::nxrt::timestep::av_trace_on())
            ::nxrt::timestep::av_note_sound("voice", Id, static_cast<int64_t>(SampleFormat), SampleRate);   // [time] av_trace
        PlayState = newServerPlayState;
    }

    // Write the status of the voice to the given user output (VoiceInParameter2).
    void WriteOutStatus2(VoiceOutStatus& outStatus, const VoiceInParameter2& parameter, Span<const Span<VoiceState>> voiceStates) {
        WriteOutStatusCommon(outStatus, parameter.is_new, voiceStates);
    }

    // Write the status of the voice to the given user output (VoiceInParameter1).
    void WriteOutStatus1(VoiceOutStatus& outStatus, const VoiceInParameter1& parameter, Span<const Span<VoiceState>> voiceStates) {
        WriteOutStatusCommon(outStatus, parameter.is_new, voiceStates);
    }

    // Update the internal state of all the WaveBuffer of the voice (VoiceInParameter2).
    void UpdateWaveBuffers2(std::vector<BehaviourParameter::ErrorInfo>& errorInfos, const VoiceInParameter2& parameter,
                            Span<const Span<VoiceState>> voiceStates, PoolMapper& mapper, BehaviourInfo& behaviourInfo) {
        UpdateWaveBuffersCommon(errorInfos, parameter.is_new, parameter.channel_count, parameter.sample_format, parameter.wave_buffers,
                                voiceStates, mapper, behaviourInfo);
    }

    // Update the internal state of all the WaveBuffer of the voice (VoiceInParameter1).
    void UpdateWaveBuffers1(std::vector<BehaviourParameter::ErrorInfo>& errorInfos, const VoiceInParameter1& parameter,
                            Span<const Span<VoiceState>> voiceStates, PoolMapper& mapper, BehaviourInfo& behaviourInfo) {
        UpdateWaveBuffersCommon(errorInfos, parameter.is_new, parameter.channel_count, parameter.sample_format, parameter.wave_buffers,
                                voiceStates, mapper, behaviourInfo);
    }

    // Update the internal parameters for command generation. Returns true if this voice should be played.
    bool UpdateParametersForCommandGeneration(Span<Span<VoiceState>> voiceStates) {
        if (FlushWaveBufferCount != 0) {
            FlushWaveBuffers(FlushWaveBufferCount, voiceStates, ChannelsCount);

            FlushWaveBufferCount = 0;
        }

        switch (PlayState) {
            case server::PlayState::Started: {
                for (size_t i = 0; i < WaveBuffers.size(); i++) {
                    server::WaveBuffer& waveBuffer = WaveBuffers[i];

                    if (!waveBuffer.IsSendToAudioProcessor) {
                        for (uint32_t y = 0; y < ChannelsCount; y++) {
                            NXAUDREN_ASSERT(!voiceStates[y][0].is_wave_buffer_valid[i]);

                            voiceStates[y][0].is_wave_buffer_valid[i] = true;
                        }

                        waveBuffer.IsSendToAudioProcessor = true;
                    }
                }

                WasPlaying = false;

                VoiceState& primaryVoiceState = voiceStates[0][0];

                for (uint32_t i = 0; i < VoiceWaveBufferCount; i++) {
                    if (primaryVoiceState.is_wave_buffer_valid[i]) {
                        return true;
                    }
                }

                return false;
            }

            case server::PlayState::Stopping: {
                for (size_t i = 0; i < WaveBuffers.size(); i++) {
                    server::WaveBuffer& waveBuffer = WaveBuffers[i];

                    waveBuffer.IsSendToAudioProcessor = true;

                    for (uint32_t j = 0; j < ChannelsCount; j++) {
                        VoiceState& voiceState = voiceStates[j][0];

                        if (voiceState.is_wave_buffer_valid[i]) {
                            voiceState.wave_buffer_index = (voiceState.wave_buffer_index + 1) % VoiceWaveBufferCount;
                            voiceState.wave_buffer_consumed++;
                        }

                        voiceState.is_wave_buffer_valid[i] = false;
                    }
                }

                for (uint32_t i = 0; i < ChannelsCount; i++) {
                    VoiceState& voiceState = voiceStates[i][0];

                    voiceState.offset = 0;
                    voiceState.played_sample_count = 0;
                    std::memset(voiceState.pitch, 0, sizeof(voiceState.pitch));
                    voiceState.fraction = 0;
                    voiceState.loop_context = AdpcmLoopContext{};
                }

                PlayState = server::PlayState::Stopped;
                WasPlaying = PreviousPlayState == server::PlayState::Started;

                return WasPlaying;
            }

            case server::PlayState::Stopped:
            case server::PlayState::Paused: {
                for (server::WaveBuffer& wavebuffer : WaveBuffers) {
                    wavebuffer.BufferAddressInfo.GetReference(true);
                    wavebuffer.ContextAddressInfo.GetReference(true);
                }

                if (SampleFormat == ::nxrt::audren::SampleFormat::Adpcm) {
                    if (DataSourceStateAddressInfo.CpuAddress != 0) {
                        DataSourceStateAddressInfo.GetReference(true);
                    }
                }

                WasPlaying = PreviousPlayState == server::PlayState::Started;

                return WasPlaying;
            }

            default:
                NXAUDREN_FATAL("VoiceInfo::UpdateParametersForCommandGeneration: unhandled PlayState");
        }
    }

    // Update the internal state for command generation (defined in VoiceContext.h, needs VoiceContext complete).
    bool UpdateForCommandGeneration(VoiceContext& context);

  private:
    // Reset the resources associated to this voice (defined in VoiceContext.h, needs VoiceContext complete).
    void ResetResources(VoiceContext& context);

    // C# ShouldUpdateParameters1 / ShouldUpdateParameters2 (identical bodies in C#).
    bool ShouldUpdateDataSourceState(uint64_t dataSourceStateAddress, uint64_t dataSourceStateSize) const {
        if (DataSourceStateAddressInfo.CpuAddress == dataSourceStateAddress) {
            return DataSourceStateAddressInfo.Size != dataSourceStateSize;
        }

        return DataSourceStateAddressInfo.CpuAddress != dataSourceStateAddress || DataSourceStateAddressInfo.Size != dataSourceStateSize ||
               DataSourceStateUnmapped;
    }

    // Shared body of C# WriteOutStatus1 / WriteOutStatus2.
    void WriteOutStatusCommon(VoiceOutStatus& outStatus, bool parameterIsNew, Span<const Span<VoiceState>> voiceStates) {
#ifndef NDEBUG
        // Sanity check in debug mode of the internal state.
        if (!parameterIsNew && !IsNew) {
            for (uint32_t i = 1; i < ChannelsCount; i++) {
                const VoiceState& stateA = voiceStates[i - 1][0];
                const VoiceState& stateB = voiceStates[i][0];

                NXAUDREN_ASSERT(stateA.wave_buffer_consumed == stateB.wave_buffer_consumed);
                NXAUDREN_ASSERT(stateA.played_sample_count == stateB.played_sample_count);
                NXAUDREN_ASSERT(stateA.offset == stateB.offset);
                NXAUDREN_ASSERT(stateA.wave_buffer_index == stateB.wave_buffer_index);
                NXAUDREN_ASSERT(stateA.fraction == stateB.fraction);
                NXAUDREN_ASSERT(std::memcmp(stateA.is_wave_buffer_valid, stateB.is_wave_buffer_valid, sizeof(stateA.is_wave_buffer_valid)) == 0);
            }
        }
#endif
        if (parameterIsNew || IsNew) {
            IsNew = true;

            outStatus.voice_drop_flag = false;
            outStatus.played_wave_buffers_count = 0;
            outStatus.played_sample_count = 0;
        } else {
            const VoiceState& state = voiceStates[0][0];

            outStatus.voice_drop_flag = VoiceDropFlag;
            outStatus.played_wave_buffers_count = state.wave_buffer_consumed;
            outStatus.played_sample_count = state.played_sample_count;
        }
    }

    // Shared body of C# UpdateWaveBuffers1 / UpdateWaveBuffers2.
    void UpdateWaveBuffersCommon(std::vector<BehaviourParameter::ErrorInfo>& errorInfos, bool parameterIsNew, uint32_t channelCount,
                                 ::nxrt::audren::SampleFormat sampleFormat, const WaveBufferInternal* inputWaveBuffers,
                                 Span<const Span<VoiceState>> voiceStates, PoolMapper& mapper, BehaviourInfo& behaviourInfo) {
        if (parameterIsNew) {
            InitializeWaveBuffers();

            for (uint32_t i = 0; i < channelCount; i++) {
                std::fill(std::begin(voiceStates[i][0].is_wave_buffer_valid), std::end(voiceStates[i][0].is_wave_buffer_valid), false);
            }
        }

        const VoiceState& voiceState = voiceStates[0][0];

        _waveBufferUpdaterErrorInfosList.clear();

        for (size_t i = 0; i < VoiceWaveBufferCount; i++) {
            UpdateWaveBuffer(_waveBufferUpdaterErrorInfosList, WaveBuffers[i], inputWaveBuffers[i], sampleFormat, voiceState.is_wave_buffer_valid[i],
                             mapper, behaviourInfo);
        }

        errorInfos = _waveBufferUpdaterErrorInfosList;
    }

    // Initialize the WaveBuffer in this voice.
    void InitializeWaveBuffers() {
        for (server::WaveBuffer& waveBuffer : WaveBuffers) {
            waveBuffer.StartSampleOffset = 0;
            waveBuffer.EndSampleOffset = 0;
            waveBuffer.ShouldLoop = false;
            waveBuffer.IsEndOfStream = false;
            waveBuffer.BufferAddressInfo.Setup(0, 0);
            waveBuffer.ContextAddressInfo.Setup(0, 0);
            waveBuffer.IsSendToAudioProcessor = true;
        }
    }

    // Update the internal state of one of the WaveBuffer of the voice.
    void UpdateWaveBuffer(std::vector<BehaviourParameter::ErrorInfo>& errorInfos, server::WaveBuffer& waveBuffer,
                          const WaveBufferInternal& inputWaveBuffer, ::nxrt::audren::SampleFormat sampleFormat, bool isValid, PoolMapper& mapper,
                          BehaviourInfo& behaviourInfo) {
        if (!isValid && waveBuffer.IsSendToAudioProcessor && waveBuffer.BufferAddressInfo.CpuAddress != 0) {
            mapper.ForceUnmap(waveBuffer.BufferAddressInfo);
            waveBuffer.BufferAddressInfo.Setup(0, 0);
        }

        if (!inputWaveBuffer.sent_to_server || BufferInfoUnmapped) {
            if (IsSampleOffsetValid(inputWaveBuffer, sampleFormat)) {
                NXAUDREN_ASSERT(waveBuffer.IsSendToAudioProcessor);

                waveBuffer.IsSendToAudioProcessor = false;
                waveBuffer.StartSampleOffset = inputWaveBuffer.start_sample_offset;
                waveBuffer.EndSampleOffset = inputWaveBuffer.end_sample_offset;
                waveBuffer.ShouldLoop = inputWaveBuffer.should_loop;
                waveBuffer.IsEndOfStream = inputWaveBuffer.is_end_of_stream;
                waveBuffer.LoopStartSampleOffset = inputWaveBuffer.loop_first_sample_offset;
                waveBuffer.LoopEndSampleOffset = inputWaveBuffer.loop_last_sample_offset;
                waveBuffer.LoopCount = inputWaveBuffer.loop_count;

                BehaviourParameter::ErrorInfo bufferInfoError{};
                BufferInfoUnmapped = !mapper.TryAttachBuffer(bufferInfoError, waveBuffer.BufferAddressInfo, inputWaveBuffer.address, inputWaveBuffer.size);

                if (bufferInfoError.error_code != ResultCode::Success) {
                    errorInfos.push_back(bufferInfoError);
                }

                if (sampleFormat == ::nxrt::audren::SampleFormat::Adpcm && behaviourInfo.IsAdpcmLoopContextBugFixed() &&
                    inputWaveBuffer.context_address != 0) {
                    BehaviourParameter::ErrorInfo adpcmLoopContextInfoError{};
                    bool adpcmLoopContextMapped = mapper.TryAttachBuffer(adpcmLoopContextInfoError, waveBuffer.ContextAddressInfo,
                                                                         inputWaveBuffer.context_address, inputWaveBuffer.context_size);

                    if (adpcmLoopContextInfoError.error_code != ResultCode::Success) {
                        errorInfos.push_back(adpcmLoopContextInfoError);
                    }

                    if (!adpcmLoopContextMapped || BufferInfoUnmapped) {
                        BufferInfoUnmapped = true;
                    } else {
                        BufferInfoUnmapped = false;
                    }
                } else {
                    waveBuffer.ContextAddressInfo.Setup(0, 0);
                }
            } else {
                BehaviourParameter::ErrorInfo error{};
                error.error_code = ResultCode::InvalidAddressInfo;
                error.extra_error_info = inputWaveBuffer.address;
                errorInfos.push_back(error);
            }
        }
    }

    // Flush a certain amount of WaveBuffer.
    void FlushWaveBuffers(uint32_t waveBufferCount, Span<Span<VoiceState>> voiceStates, uint32_t channelCount) {
        uint32_t waveBufferIndex = WaveBuffersIndex;

        for (uint32_t i = 0; i < waveBufferCount; i++) {
            WaveBuffers[waveBufferIndex].IsSendToAudioProcessor = true;

            for (uint32_t j = 0; j < channelCount; j++) {
                VoiceState& voiceState = voiceStates[j][0];

                if (!WaveBuffers[waveBufferIndex].IsSendToAudioProcessor || voiceState.is_wave_buffer_valid[waveBufferIndex]) {
                    voiceState.wave_buffer_index = (voiceState.wave_buffer_index + 1) % VoiceWaveBufferCount;
                    voiceState.wave_buffer_consumed++;
                    voiceState.is_wave_buffer_valid[waveBufferIndex] = false;
                }
            }

            WaveBuffers[waveBufferIndex].IsSendToAudioProcessor = true;

            waveBufferIndex = (waveBufferIndex + 1) % VoiceWaveBufferCount;
        }
    }
};

}  // namespace nxrt::audren

// Defines VoiceInfo::UpdateForCommandGeneration and VoiceInfo::ResetResources (needs VoiceContext complete).
#include "VoiceContext.h"
