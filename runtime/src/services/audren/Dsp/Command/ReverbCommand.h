// SPDX-License-Identifier: GPL-3.0-or-later
// Port of Ryujinx.Audio/Renderer/Dsp/Command/ReverbCommand.cs (Ryubing fork, MIT). Scalar path (no SIMD in the C# file).
// The C# static int[] index tables are kept verbatim as static constexpr arrays; the C# Span<T> parameters become Span<const int32_t>.
#pragma once
#include <vector>

#include "../../Constants.h"
#include "../../audren_common.h"
#include "../../audren_types.h"
#include "../DataSourceHelper.h"
#include "../Effect/DecayDelay.h"
#include "../Effect/DelayLine.h"
#include "../FixedPointHelper.h"
#include "../State/ReverbState.h"
#include "CommandList.h"
#include "ICommand.h"

namespace nxrt::audren {

class ReverbCommand : public ICommand {
  private:
    static constexpr int32_t _outputEarlyIndicesTableMono[10] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    static constexpr int32_t _targetEarlyDelayLineIndicesTableMono[10] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    static constexpr int32_t _outputIndicesTableMono[4] = {0, 0, 0, 0};
    static constexpr int32_t _targetOutputFeedbackIndicesTableMono[4] = {0, 1, 2, 3};

    static constexpr int32_t _outputEarlyIndicesTableStereo[10] = {0, 0, 1, 1, 0, 1, 0, 0, 1, 1};
    static constexpr int32_t _targetEarlyDelayLineIndicesTableStereo[10] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    static constexpr int32_t _outputIndicesTableStereo[4] = {0, 0, 1, 1};
    static constexpr int32_t _targetOutputFeedbackIndicesTableStereo[4] = {2, 0, 3, 1};

    static constexpr int32_t _outputEarlyIndicesTableQuadraphonic[10] = {0, 0, 1, 1, 0, 1, 2, 2, 3, 3};
    static constexpr int32_t _targetEarlyDelayLineIndicesTableQuadraphonic[10] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    static constexpr int32_t _outputIndicesTableQuadraphonic[4] = {0, 1, 2, 3};
    static constexpr int32_t _targetOutputFeedbackIndicesTableQuadraphonic[4] = {0, 1, 2, 3};

    static constexpr int32_t _outputEarlyIndicesTableSurround[20] = {0, 5, 0, 5, 1, 5, 1, 5, 4, 5, 4, 5, 2, 5, 2, 5, 3, 5, 3, 5};
    static constexpr int32_t _targetEarlyDelayLineIndicesTableSurround[20] = {0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9};
    static constexpr int32_t _outputIndicesTableSurround[6] = {0, 1, 2, 3, 4, 5};
    static constexpr int32_t _targetOutputFeedbackIndicesTableSurround[6] = {0, 1, 2, 3, -1, 3};

  private:
    static constexpr int32_t FixedPointPrecision = 14;

  public:
    int32_t NodeId = 0;
    uint32_t EstimatedProcessingTime = 0;

    Span<ReverbState> State;
    CpuAddress WorkBuffer = 0;
    std::vector<uint16_t> OutputBufferIndices;
    std::vector<uint16_t> InputBufferIndices;
    bool IsLongSizePreDelaySupported = false;

    bool IsEffectEnabled = false;

  private:
    ReverbParameter _parameter{};

  public:
    ReverbCommand()
        : OutputBufferIndices(static_cast<size_t>(Constants::VoiceChannelCountMax), 0), InputBufferIndices(static_cast<size_t>(Constants::VoiceChannelCountMax), 0) {}

    // C# `public ReverbParameter Parameter => _parameter;`
    ReverbParameter Parameter() const { return _parameter; }

    ReverbCommand& Initialize(uint32_t bufferOffset, ReverbParameter parameter, Span<ReverbState> state, bool isEnabled, CpuAddress workBuffer,
                              int32_t nodeId, bool isLongSizePreDelaySupported, bool newEffectChannelMappingSupported) {
        Enabled = true;
        IsEffectEnabled = isEnabled;
        NodeId = nodeId;
        _parameter = parameter;
        State = state;
        WorkBuffer = workBuffer;

        // C# Array6 indexing: a channel count above 6 throws in the C#.
        if (static_cast<size_t>(_parameter.channel_count) > sizeof(_parameter.input)) {
            NXAUDREN_FATAL("ReverbCommand::Initialize: channel count above 6");
        }

        for (size_t i = 0; i < _parameter.channel_count; i++) {
            InputBufferIndices[i] = static_cast<uint16_t>(bufferOffset + _parameter.input[i]);
            OutputBufferIndices[i] = static_cast<uint16_t>(bufferOffset + _parameter.output[i]);
        }

        IsLongSizePreDelaySupported = isLongSizePreDelaySupported;

        // NOTE: We do the opposite as Nintendo here for now to restore previous behaviour
        // TODO: Update reverb processing and remove this to use RemapLegacyChannelEffectMappingToChannelResourceMapping.
        DataSourceHelper::RemapChannelResourceMappingToLegacy(newEffectChannelMappingSupported, InputBufferIndices, _parameter.channel_count);
        DataSourceHelper::RemapChannelResourceMappingToLegacy(newEffectChannelMappingSupported, OutputBufferIndices, _parameter.channel_count);

        return *this;
    }

    int32_t GetNodeId() const override { return NodeId; }
    CommandType GetCommandType() const override { return CommandType::Reverb; }
    uint32_t GetEstimatedProcessingTime() const override { return EstimatedProcessingTime; }

    void Process(CommandList& context) override {
        ReverbState& state = State[0];

        if (IsEffectEnabled) {
            // Also when the state was never built: an effect first processed while disabled and enabled later without passing through
            // Invalid again had empty delay lines (OoT's first scene: null DecayDelays read at 0x20 on the audio thread).
            if (_parameter.status == UsageState::Invalid || state.FdnDelayLines.empty()) {
                state = ReverbState(_parameter, WorkBuffer, IsLongSizePreDelaySupported);
            } else if (_parameter.status == UsageState::New) {
                state.UpdateParameter(_parameter);
            }
        }

        ProcessReverb(context, state);
    }

  private:
    static bool IsChannelCountValid(uint16_t channelCount) { return channelCount == 1 || channelCount == 2 || channelCount == 4 || channelCount == 6; }

    void ProcessReverbMono(ReverbState& state, float* const* outputBuffers, float* const* inputBuffers, uint32_t sampleCount) {
        ProcessReverbGeneric(state, outputBuffers, inputBuffers, sampleCount, Span<const int32_t>(_outputEarlyIndicesTableMono, 10),
                             Span<const int32_t>(_targetEarlyDelayLineIndicesTableMono, 10), Span<const int32_t>(_targetOutputFeedbackIndicesTableMono, 4),
                             Span<const int32_t>(_outputIndicesTableMono, 4));
    }

    void ProcessReverbStereo(ReverbState& state, float* const* outputBuffers, float* const* inputBuffers, uint32_t sampleCount) {
        ProcessReverbGeneric(state, outputBuffers, inputBuffers, sampleCount, Span<const int32_t>(_outputEarlyIndicesTableStereo, 10),
                             Span<const int32_t>(_targetEarlyDelayLineIndicesTableStereo, 10),
                             Span<const int32_t>(_targetOutputFeedbackIndicesTableStereo, 4), Span<const int32_t>(_outputIndicesTableStereo, 4));
    }

    void ProcessReverbQuadraphonic(ReverbState& state, float* const* outputBuffers, float* const* inputBuffers, uint32_t sampleCount) {
        ProcessReverbGeneric(state, outputBuffers, inputBuffers, sampleCount, Span<const int32_t>(_outputEarlyIndicesTableQuadraphonic, 10),
                             Span<const int32_t>(_targetEarlyDelayLineIndicesTableQuadraphonic, 10),
                             Span<const int32_t>(_targetOutputFeedbackIndicesTableQuadraphonic, 4),
                             Span<const int32_t>(_outputIndicesTableQuadraphonic, 4));
    }

    void ProcessReverbSurround(ReverbState& state, float* const* outputBuffers, float* const* inputBuffers, uint32_t sampleCount) {
        ProcessReverbGeneric(state, outputBuffers, inputBuffers, sampleCount, Span<const int32_t>(_outputEarlyIndicesTableSurround, 20),
                             Span<const int32_t>(_targetEarlyDelayLineIndicesTableSurround, 20),
                             Span<const int32_t>(_targetOutputFeedbackIndicesTableSurround, 6), Span<const int32_t>(_outputIndicesTableSurround, 6));
    }

    void ProcessReverbGeneric(ReverbState& state, float* const* outputBuffers, float* const* inputBuffers, uint32_t sampleCount,
                              Span<const int32_t> outputEarlyIndicesTable, Span<const int32_t> targetEarlyDelayLineIndicesTable,
                              Span<const int32_t> targetOutputFeedbackIndicesTable, Span<const int32_t> outputIndicesTable) {
        bool isSurround = _parameter.channel_count == 6;

        float reverbGain = FixedPointHelper::ToFloat(static_cast<int64_t>(_parameter.reverb_gain), FixedPointPrecision);
        float lateGain = FixedPointHelper::ToFloat(static_cast<int64_t>(_parameter.late_gain), FixedPointPrecision);
        float outGain = FixedPointHelper::ToFloat(static_cast<int64_t>(_parameter.out_gain), FixedPointPrecision);
        float dryGain = FixedPointHelper::ToFloat(static_cast<int64_t>(_parameter.dry_gain), FixedPointPrecision);

        const size_t channelCount = _parameter.channel_count;

        float outputValues[Constants::ChannelCountMax] = {};
        float feedbackValues[4] = {};
        float feedbackOutputValues[4] = {};
        float channelInput[Constants::ChannelCountMax] = {};

        for (uint32_t sampleIndex = 0; sampleIndex < sampleCount; sampleIndex++) {
            for (float& value : outputValues) {
                value = 0.0f;
            }

            for (size_t i = 0; i < targetEarlyDelayLineIndicesTable.Length; i++) {
                int32_t earlyDelayIndex = targetEarlyDelayLineIndicesTable[i];
                int32_t outputIndex = outputEarlyIndicesTable[i];

                float tapOutput = state.PreDelayLine.TapUnsafe(state.EarlyDelayTime[earlyDelayIndex], 0);

                outputValues[outputIndex] += tapOutput * state.EarlyGain[earlyDelayIndex];
            }

            if (isSurround) {
                outputValues[5] *= 0.2f;
            }

            float targetPreDelayValue = 0;

            for (size_t channelIndex = 0; channelIndex < channelCount; channelIndex++) {
                channelInput[channelIndex] = inputBuffers[channelIndex][sampleIndex] * 64;
                targetPreDelayValue += channelInput[channelIndex] * reverbGain;
            }

            state.PreDelayLine.Update(targetPreDelayValue);

            float lateValue = state.PreDelayLine.Tap(state.PreDelayLineDelayTime) * lateGain;

            for (size_t i = 0; i < state.FdnDelayLines.size(); i++) {
                feedbackOutputValues[i] = state.FdnDelayLines[i].Read() * state.HighFrequencyDecayDirectGain[i] +
                                          state.PreviousFeedbackOutput[i] * state.HighFrequencyDecayPreviousGain[i];
                state.PreviousFeedbackOutput[i] = feedbackOutputValues[i];
            }

            feedbackValues[0] = feedbackOutputValues[2] + feedbackOutputValues[1];
            feedbackValues[1] = -feedbackOutputValues[0] - feedbackOutputValues[3];
            feedbackValues[2] = feedbackOutputValues[0] - feedbackOutputValues[3];
            feedbackValues[3] = feedbackOutputValues[1] - feedbackOutputValues[2];

            for (size_t i = 0; i < state.FdnDelayLines.size(); i++) {
                feedbackOutputValues[i] = state.DecayDelays[i].Update(feedbackValues[i] + lateValue);
                state.FdnDelayLines[i].Update(feedbackOutputValues[i]);
            }

            for (size_t i = 0; i < targetOutputFeedbackIndicesTable.Length; i++) {
                int32_t targetOutputFeedbackIndex = targetOutputFeedbackIndicesTable[i];
                int32_t outputIndex = outputIndicesTable[i];

                if (targetOutputFeedbackIndex >= 0) {
                    outputValues[outputIndex] += feedbackOutputValues[targetOutputFeedbackIndex];
                }
            }

            if (isSurround) {
                outputValues[4] += state.FrontCenterDelayLine.Update((feedbackOutputValues[2] - feedbackOutputValues[3]) * 0.5f);
            }

            for (size_t channelIndex = 0; channelIndex < channelCount; channelIndex++) {
                outputBuffers[channelIndex][sampleIndex] = (outputValues[channelIndex] * outGain + channelInput[channelIndex] * dryGain) / 64;
            }
        }
    }

    void ProcessReverb(CommandList& context, ReverbState& state) {
        NXAUDREN_ASSERT(IsChannelCountValid(_parameter.channel_count));

        if (IsEffectEnabled && IsChannelCountValid(_parameter.channel_count)) {
            float* inputBuffers[Constants::ChannelCountMax] = {};
            float* outputBuffers[Constants::ChannelCountMax] = {};

            for (size_t i = 0; i < _parameter.channel_count; i++) {
                inputBuffers[i] = context.GetBufferPointer(InputBufferIndices[i]);
                outputBuffers[i] = context.GetBufferPointer(OutputBufferIndices[i]);
            }

            switch (_parameter.channel_count) {
                case 1:
                    ProcessReverbMono(state, outputBuffers, inputBuffers, context.SampleCount);
                    break;
                case 2:
                    ProcessReverbStereo(state, outputBuffers, inputBuffers, context.SampleCount);
                    break;
                case 4:
                    ProcessReverbQuadraphonic(state, outputBuffers, inputBuffers, context.SampleCount);
                    break;
                case 6:
                    ProcessReverbSurround(state, outputBuffers, inputBuffers, context.SampleCount);
                    break;
                default:
                    NXAUDREN_FATAL("ReverbCommand::ProcessReverb: NotImplementedException (channel count)");
            }
        } else {
            for (size_t i = 0; i < _parameter.channel_count; i++) {
                if (InputBufferIndices[i] != OutputBufferIndices[i]) {
                    context.CopyBuffer(OutputBufferIndices[i], InputBufferIndices[i]);
                }
            }
        }
    }
};

}  // namespace nxrt::audren
