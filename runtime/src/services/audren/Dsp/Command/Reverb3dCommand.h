// SPDX-License-Identifier: GPL-3.0-or-later
// Port of Ryujinx.Audio/Renderer/Dsp/Command/Reverb3dCommand.cs (Ryubing fork, MIT). Scalar path (no SIMD in the C# file).
// The C# static int[] index tables are kept verbatim as static constexpr arrays.
#pragma once
#include <vector>

#include "../../Constants.h"
#include "../../audren_common.h"
#include "../../audren_types.h"
#include "../DataSourceHelper.h"
#include "../Effect/DecayDelay.h"
#include "../Effect/IDelayLine.h"
#include "../State/Reverb3dState.h"
#include "CommandList.h"
#include "ICommand.h"

namespace nxrt::audren {

class Reverb3dCommand : public ICommand {
  private:
    static constexpr int32_t _outputEarlyIndicesTableMono[20] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    static constexpr int32_t _targetEarlyDelayLineIndicesTableMono[20] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19};
    static constexpr int32_t _targetOutputFeedbackIndicesTableMono[1] = {0};

    static constexpr int32_t _outputEarlyIndicesTableStereo[20] = {0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1};
    static constexpr int32_t _targetEarlyDelayLineIndicesTableStereo[20] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19};
    static constexpr int32_t _targetOutputFeedbackIndicesTableStereo[2] = {0, 1};

    static constexpr int32_t _outputEarlyIndicesTableQuadraphonic[20] = {0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 1, 1, 1, 0, 0, 0, 0, 3, 3, 3};
    static constexpr int32_t _targetEarlyDelayLineIndicesTableQuadraphonic[20] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19};
    static constexpr int32_t _targetOutputFeedbackIndicesTableQuadraphonic[4] = {0, 1, 2, 3};

    static constexpr int32_t _outputEarlyIndicesTableSurround[40] = {4, 5, 0, 5, 0, 5, 1, 5, 1, 5, 1, 5, 1, 5, 2, 5, 2, 5, 2, 5,
                                                                     1, 5, 1, 5, 1, 5, 0, 5, 0, 5, 0, 5, 0, 5, 3, 5, 3, 5, 3, 5};
    static constexpr int32_t _targetEarlyDelayLineIndicesTableSurround[40] = {0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9,
                                                                              10, 10, 11, 11, 12, 12, 13, 13, 14, 14, 15, 15, 16, 16, 17, 17, 18, 18, 19, 19};
    static constexpr int32_t _targetOutputFeedbackIndicesTableSurround[6] = {0, 1, 2, 3, -1, 3};

  public:
    int32_t NodeId = 0;
    uint32_t EstimatedProcessingTime = 0;

    uint16_t InputBufferIndex = 0;
    uint16_t OutputBufferIndex = 0;

    Span<Reverb3dState> State;
    CpuAddress WorkBuffer = 0;
    std::vector<uint16_t> OutputBufferIndices;
    std::vector<uint16_t> InputBufferIndices;

    bool IsEffectEnabled = false;

  private:
    Reverb3dParameter _parameter{};

  public:
    Reverb3dCommand()
        : OutputBufferIndices(static_cast<size_t>(Constants::VoiceChannelCountMax), 0), InputBufferIndices(static_cast<size_t>(Constants::VoiceChannelCountMax), 0) {}

    // C# `public Reverb3dParameter Parameter => _parameter;`
    Reverb3dParameter Parameter() const { return _parameter; }

    Reverb3dCommand& Initialize(uint32_t bufferOffset, Reverb3dParameter parameter, Span<Reverb3dState> state, bool isEnabled, CpuAddress workBuffer,
                                int32_t nodeId, bool newEffectChannelMappingSupported) {
        Enabled = true;
        IsEffectEnabled = isEnabled;
        NodeId = nodeId;
        _parameter = parameter;
        State = state;
        WorkBuffer = workBuffer;

        // C# Array6 indexing: a channel count above 6 throws in the C#.
        if (static_cast<size_t>(_parameter.channel_count) > sizeof(_parameter.input)) {
            NXAUDREN_FATAL("Reverb3dCommand::Initialize: channel count above 6");
        }

        for (size_t i = 0; i < _parameter.channel_count; i++) {
            InputBufferIndices[i] = static_cast<uint16_t>(bufferOffset + _parameter.input[i]);
            OutputBufferIndices[i] = static_cast<uint16_t>(bufferOffset + _parameter.output[i]);
        }

        // NOTE: We do the opposite as the platform here for now to restore previous behaviour
        // TODO: Update reverb 3d processing and remove this to use RemapLegacyChannelEffectMappingToChannelResourceMapping.
        DataSourceHelper::RemapChannelResourceMappingToLegacy(newEffectChannelMappingSupported, InputBufferIndices, _parameter.channel_count);
        DataSourceHelper::RemapChannelResourceMappingToLegacy(newEffectChannelMappingSupported, OutputBufferIndices, _parameter.channel_count);

        return *this;
    }

    int32_t GetNodeId() const override { return NodeId; }
    CommandType GetCommandType() const override { return CommandType::Reverb3d; }
    uint32_t GetEstimatedProcessingTime() const override { return EstimatedProcessingTime; }

    void Process(CommandList& context) override {
        Reverb3dState& state = State[0];

        if (IsEffectEnabled) {
            if (_parameter.parameter_status == UsageState::Invalid) {
                state = Reverb3dState(_parameter, WorkBuffer);
            } else if (_parameter.parameter_status == UsageState::New) {
                state.UpdateParameter(_parameter);
            }
        }

        ProcessReverb3d(context, state);
    }

    void ProcessReverb3d(CommandList& context, Reverb3dState& state) {
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
                    ProcessReverb3dMono(state, outputBuffers, inputBuffers, context.SampleCount);
                    break;
                case 2:
                    ProcessReverb3dStereo(state, outputBuffers, inputBuffers, context.SampleCount);
                    break;
                case 4:
                    ProcessReverb3dQuadraphonic(state, outputBuffers, inputBuffers, context.SampleCount);
                    break;
                case 6:
                    ProcessReverb3dSurround(state, outputBuffers, inputBuffers, context.SampleCount);
                    break;
                default:
                    NXAUDREN_FATAL("Reverb3dCommand::ProcessReverb3d: NotImplementedException (channel count)");
            }
        } else {
            for (size_t i = 0; i < _parameter.channel_count; i++) {
                if (InputBufferIndices[i] != OutputBufferIndices[i]) {
                    context.CopyBuffer(OutputBufferIndices[i], InputBufferIndices[i]);
                }
            }
        }
    }

  private:
    static bool IsChannelCountValid(uint16_t channelCount) { return channelCount == 1 || channelCount == 2 || channelCount == 4 || channelCount == 6; }

    void ProcessReverb3dMono(Reverb3dState& state, float* const* outputBuffers, float* const* inputBuffers, uint32_t sampleCount) {
        ProcessReverb3dGeneric(state, outputBuffers, inputBuffers, sampleCount, Span<const int32_t>(_outputEarlyIndicesTableMono, 20),
                               Span<const int32_t>(_targetEarlyDelayLineIndicesTableMono, 20), Span<const int32_t>(_targetOutputFeedbackIndicesTableMono, 1));
    }

    void ProcessReverb3dStereo(Reverb3dState& state, float* const* outputBuffers, float* const* inputBuffers, uint32_t sampleCount) {
        ProcessReverb3dGeneric(state, outputBuffers, inputBuffers, sampleCount, Span<const int32_t>(_outputEarlyIndicesTableStereo, 20),
                               Span<const int32_t>(_targetEarlyDelayLineIndicesTableStereo, 20),
                               Span<const int32_t>(_targetOutputFeedbackIndicesTableStereo, 2));
    }

    void ProcessReverb3dQuadraphonic(Reverb3dState& state, float* const* outputBuffers, float* const* inputBuffers, uint32_t sampleCount) {
        ProcessReverb3dGeneric(state, outputBuffers, inputBuffers, sampleCount, Span<const int32_t>(_outputEarlyIndicesTableQuadraphonic, 20),
                               Span<const int32_t>(_targetEarlyDelayLineIndicesTableQuadraphonic, 20),
                               Span<const int32_t>(_targetOutputFeedbackIndicesTableQuadraphonic, 4));
    }

    void ProcessReverb3dSurround(Reverb3dState& state, float* const* outputBuffers, float* const* inputBuffers, uint32_t sampleCount) {
        ProcessReverb3dGeneric(state, outputBuffers, inputBuffers, sampleCount, Span<const int32_t>(_outputEarlyIndicesTableSurround, 40),
                               Span<const int32_t>(_targetEarlyDelayLineIndicesTableSurround, 40),
                               Span<const int32_t>(_targetOutputFeedbackIndicesTableSurround, 6));
    }

    void ProcessReverb3dGeneric(Reverb3dState& state, float* const* outputBuffers, float* const* inputBuffers, uint32_t sampleCount,
                                Span<const int32_t> outputEarlyIndicesTable, Span<const int32_t> targetEarlyDelayLineIndicesTable,
                                Span<const int32_t> targetOutputFeedbackIndicesTable) {
        const int32_t DelayLineSampleIndexOffset = 1;

        bool isMono = _parameter.channel_count == 1;
        bool isSurround = _parameter.channel_count == 6;

        const size_t channelCount = _parameter.channel_count;

        float outputValues[Constants::ChannelCountMax] = {};
        float channelInput[Constants::ChannelCountMax] = {};
        float feedbackValues[4] = {};
        float feedbackOutputValues[4] = {};
        float values[4] = {};

        for (uint32_t sampleIndex = 0; sampleIndex < sampleCount; sampleIndex++) {
            for (float& value : outputValues) {
                value = 0.0f;
            }

            float tapOut = state.PreDelayLine->TapUnsafe(state.ReflectionDelayTime, DelayLineSampleIndexOffset);

            for (size_t i = 0; i < targetEarlyDelayLineIndicesTable.Length; i++) {
                int32_t earlyDelayIndex = targetEarlyDelayLineIndicesTable[i];
                int32_t outputIndex = outputEarlyIndicesTable[static_cast<size_t>(earlyDelayIndex)];

                float tempTapOut = state.PreDelayLine->TapUnsafe(state.EarlyDelayTime[earlyDelayIndex], DelayLineSampleIndexOffset);

                outputValues[outputIndex] += tempTapOut * state.EarlyGain[earlyDelayIndex];
            }

            float targetPreDelayValue = 0;

            for (size_t channelIndex = 0; channelIndex < channelCount; channelIndex++) {
                channelInput[channelIndex] = inputBuffers[channelIndex][sampleIndex];
                targetPreDelayValue += channelInput[channelIndex];
            }

            for (size_t i = 0; i < channelCount; i++) {
                outputValues[i] *= state.EarlyReflectionsGain;
            }

            state.PreviousPreDelayValue = (targetPreDelayValue * state.TargetPreDelayGain) + (state.PreviousPreDelayValue * state.PreviousPreDelayGain);

            state.PreDelayLine->Update(state.PreviousPreDelayValue);

            for (size_t i = 0; i < state.FdnDelayLines.size(); i++) {
                float fdnValue = state.FdnDelayLines[i]->Read();

                float feedbackOutputValue = fdnValue * state.DecayDirectFdnGain[i] + state.PreviousFeedbackOutputDecayed[i];

                state.PreviousFeedbackOutputDecayed[i] = (fdnValue * state.DecayCurrentFdnGain[i]) + (feedbackOutputValue * state.DecayCurrentOutputGain[i]);

                feedbackOutputValues[i] = feedbackOutputValue;
            }

            feedbackValues[0] = feedbackOutputValues[2] + feedbackOutputValues[1];
            feedbackValues[1] = -feedbackOutputValues[0] - feedbackOutputValues[3];
            feedbackValues[2] = feedbackOutputValues[0] - feedbackOutputValues[3];
            feedbackValues[3] = feedbackOutputValues[1] - feedbackOutputValues[2];

            for (size_t i = 0; i < state.DecayDelays1.size(); i++) {
                float temp = state.DecayDelays1[i].Update(tapOut * state.LateReverbGain + feedbackValues[i]);

                values[i] = state.DecayDelays2[i].Update(temp);

                state.FdnDelayLines[i]->Update(values[i]);
            }

            for (size_t channelIndex = 0; channelIndex < targetOutputFeedbackIndicesTable.Length; channelIndex++) {
                int32_t targetOutputFeedbackIndex = targetOutputFeedbackIndicesTable[channelIndex];

                if (targetOutputFeedbackIndex >= 0) {
                    outputBuffers[channelIndex][sampleIndex] =
                        (outputValues[channelIndex] + values[targetOutputFeedbackIndex] + channelInput[channelIndex] * state.DryGain);
                }
            }

            if (isMono) {
                outputBuffers[0][sampleIndex] += values[1];
            }

            if (isSurround) {
                outputBuffers[4][sampleIndex] +=
                    (outputValues[4] + state.FrontCenterDelayLine->Update((values[2] - values[3]) * 0.5f) + channelInput[4] * state.DryGain);
            }
        }
    }
};

}  // namespace nxrt::audren
