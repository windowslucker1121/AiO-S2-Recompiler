// SPDX-License-Identifier: GPL-3.0-or-later
// Port of Ryujinx.Audio/Renderer/Dsp/Command/DelayCommand.cs (Ryubing fork, MIT). Scalar bodies of the mono, stereo, quadraphonic and
// surround paths (the C# System.Numerics vector code is written out as scalar float arithmetic in the same operation order).
// Ryujinx's `Vector2/Vector4/Matrix4x4` come from Utils/Math/MatrixHelper.h (Numerics::).
#pragma once
#include <vector>

#include "../../Constants.h"
#include "../../Utils/Math/MatrixHelper.h"
#include "../../audren_common.h"
#include "../../audren_types.h"
#include "../DataSourceHelper.h"
#include "../FixedPointHelper.h"
#include "../State/DelayState.h"
#include "CommandList.h"
#include "ICommand.h"

namespace nxrt::audren {

class DelayCommand : public ICommand {
  private:
    static constexpr int32_t FixedPointPrecision = 14;

  public:
    int32_t NodeId = 0;
    uint32_t EstimatedProcessingTime = 0;

    Span<DelayState> State;
    CpuAddress WorkBuffer = 0;
    std::vector<uint16_t> OutputBufferIndices;
    std::vector<uint16_t> InputBufferIndices;
    bool IsEffectEnabled = false;

  private:
    DelayParameter _parameter{};

  public:
    DelayCommand()
        : OutputBufferIndices(static_cast<size_t>(Constants::VoiceChannelCountMax), 0), InputBufferIndices(static_cast<size_t>(Constants::VoiceChannelCountMax), 0) {}

    // C# `public DelayParameter Parameter => _parameter;`
    DelayParameter Parameter() const { return _parameter; }

    DelayCommand& Initialize(uint32_t bufferOffset, DelayParameter parameter, Span<DelayState> state, bool isEnabled, CpuAddress workBuffer,
                             int32_t nodeId, bool newEffectChannelMappingSupported) {
        Enabled = true;
        NodeId = nodeId;
        _parameter = parameter;
        State = state;
        WorkBuffer = workBuffer;

        IsEffectEnabled = isEnabled;

        // C# Array6 indexing: a channel count above 6 throws in the C#.
        if (static_cast<size_t>(_parameter.channel_count) > sizeof(_parameter.input)) {
            NXAUDREN_FATAL("DelayCommand::Initialize: channel count above 6");
        }

        for (size_t i = 0; i < _parameter.channel_count; i++) {
            InputBufferIndices[i] = static_cast<uint16_t>(bufferOffset + _parameter.input[i]);
            OutputBufferIndices[i] = static_cast<uint16_t>(bufferOffset + _parameter.output[i]);
        }

        DataSourceHelper::RemapLegacyChannelEffectMappingToChannelResourceMapping(newEffectChannelMappingSupported, InputBufferIndices,
                                                                                  _parameter.channel_count);
        DataSourceHelper::RemapLegacyChannelEffectMappingToChannelResourceMapping(newEffectChannelMappingSupported, OutputBufferIndices,
                                                                                  _parameter.channel_count);

        return *this;
    }

    int32_t GetNodeId() const override { return NodeId; }
    CommandType GetCommandType() const override { return CommandType::Delay; }
    uint32_t GetEstimatedProcessingTime() const override { return EstimatedProcessingTime; }

    void Process(CommandList& context) override {
        DelayState& state = State[0];

        if (IsEffectEnabled) {
            // also when never built (enabled later without passing through Invalid again; see ReverbCommand)
            if (_parameter.status == UsageState::Invalid || state.DelayLines.empty()) {
                state = DelayState(_parameter, WorkBuffer);
            } else if (_parameter.status == UsageState::New) {
                state.UpdateParameter(_parameter);
            }
        }

        ProcessDelay(context, state);
    }

  private:
    static bool IsChannelCountValid(uint16_t channelCount) { return channelCount == 1 || channelCount == 2 || channelCount == 4 || channelCount == 6; }

    // C# `new Matrix4x4(16 values)`: row-major.
    static Numerics::Matrix4x4 Matrix4x4FromRowMajor(const float (&v)[16]) {
        Numerics::Matrix4x4 m;
        m.M11 = v[0];
        m.M12 = v[1];
        m.M13 = v[2];
        m.M14 = v[3];
        m.M21 = v[4];
        m.M22 = v[5];
        m.M23 = v[6];
        m.M24 = v[7];
        m.M31 = v[8];
        m.M32 = v[9];
        m.M33 = v[10];
        m.M34 = v[11];
        m.M41 = v[12];
        m.M42 = v[13];
        m.M43 = v[14];
        m.M44 = v[15];
        return m;
    }

    void ProcessDelayMono(DelayState& state, float* outputBuffer, float* inputBuffer, uint32_t sampleCount) {
        const uint16_t ChannelCount = 1;

        float feedbackGain = FixedPointHelper::ToFloat(static_cast<int64_t>(_parameter.feedback_gain), FixedPointPrecision);
        float inGain = FixedPointHelper::ToFloat(static_cast<int64_t>(_parameter.in_gain), FixedPointPrecision);
        float dryGain = FixedPointHelper::ToFloat(static_cast<int64_t>(_parameter.dry_gain), FixedPointPrecision);
        float outGain = FixedPointHelper::ToFloat(static_cast<int64_t>(_parameter.out_gain), FixedPointPrecision);

        for (uint32_t i = 0; i < sampleCount; i++) {
            float input = inputBuffer[i] * 64;
            float delayLineValue = state.DelayLines[0].Read();

            float temp = input * inGain + delayLineValue * feedbackGain;

            state.UpdateLowPassFilter(temp, ChannelCount);

            outputBuffer[i] = (input * dryGain + delayLineValue * outGain) / 64;
        }
    }

    void ProcessDelayStereo(DelayState& state, float* const* outputBuffers, float* const* inputBuffers, uint32_t sampleCount) {
        const uint16_t ChannelCount = 2;

        float delayFeedbackBaseGain = state.DelayFeedbackBaseGain;
        float delayFeedbackCrossGain = state.DelayFeedbackCrossGain;
        float inGain = FixedPointHelper::ToFloat(static_cast<int64_t>(_parameter.in_gain), FixedPointPrecision);
        float dryGain = FixedPointHelper::ToFloat(static_cast<int64_t>(_parameter.dry_gain), FixedPointPrecision);
        float outGain = FixedPointHelper::ToFloat(static_cast<int64_t>(_parameter.out_gain), FixedPointPrecision);

        Matrix2x2 delayFeedback(delayFeedbackBaseGain, delayFeedbackCrossGain, delayFeedbackCrossGain, delayFeedbackBaseGain);

        for (uint32_t i = 0; i < sampleCount; i++) {
            Numerics::Vector2 channelInput(inputBuffers[0][i] * 64, inputBuffers[1][i] * 64);

            Numerics::Vector2 delayLineValues(state.DelayLines[0].Read(), state.DelayLines[1].Read());

            Numerics::Vector2 transformed = MatrixHelper::Transform(delayLineValues, delayFeedback);
            float temp[ChannelCount] = {transformed.X + channelInput.X * inGain, transformed.Y + channelInput.Y * inGain};

            state.UpdateLowPassFilter(temp[0], ChannelCount);

            outputBuffers[0][i] = (channelInput.X * dryGain + delayLineValues.X * outGain) / 64;
            outputBuffers[1][i] = (channelInput.Y * dryGain + delayLineValues.Y * outGain) / 64;
        }
    }

    void ProcessDelayQuadraphonic(DelayState& state, float* const* outputBuffers, float* const* inputBuffers, uint32_t sampleCount) {
        const uint16_t ChannelCount = 4;

        float delayFeedbackBaseGain = state.DelayFeedbackBaseGain;
        float delayFeedbackCrossGain = state.DelayFeedbackCrossGain;
        float inGain = FixedPointHelper::ToFloat(static_cast<int64_t>(_parameter.in_gain), FixedPointPrecision);
        float dryGain = FixedPointHelper::ToFloat(static_cast<int64_t>(_parameter.dry_gain), FixedPointPrecision);
        float outGain = FixedPointHelper::ToFloat(static_cast<int64_t>(_parameter.out_gain), FixedPointPrecision);

        const float feedbackMatrix[16] = {
            delayFeedbackBaseGain, delayFeedbackCrossGain, delayFeedbackCrossGain, 0.0f,
            delayFeedbackCrossGain, delayFeedbackBaseGain, 0.0f, delayFeedbackCrossGain,
            delayFeedbackCrossGain, 0.0f, delayFeedbackBaseGain, delayFeedbackCrossGain,
            0.0f, delayFeedbackCrossGain, delayFeedbackCrossGain, delayFeedbackBaseGain};
        Numerics::Matrix4x4 delayFeedback = Matrix4x4FromRowMajor(feedbackMatrix);

        for (uint32_t i = 0; i < sampleCount; i++) {
            Numerics::Vector4 channelInput(inputBuffers[0][i] * 64, inputBuffers[1][i] * 64, inputBuffers[2][i] * 64, inputBuffers[3][i] * 64);

            Numerics::Vector4 delayLineValues(state.DelayLines[0].Read(), state.DelayLines[1].Read(), state.DelayLines[2].Read(),
                                              state.DelayLines[3].Read());

            Numerics::Vector4 transformed = MatrixHelper::Transform(delayLineValues, delayFeedback);
            float temp[ChannelCount] = {transformed.X + channelInput.X * inGain, transformed.Y + channelInput.Y * inGain,
                                        transformed.Z + channelInput.Z * inGain, transformed.W + channelInput.W * inGain};

            state.UpdateLowPassFilter(temp[0], ChannelCount);

            outputBuffers[0][i] = (channelInput.X * dryGain + delayLineValues.X * outGain) / 64;
            outputBuffers[1][i] = (channelInput.Y * dryGain + delayLineValues.Y * outGain) / 64;
            outputBuffers[2][i] = (channelInput.Z * dryGain + delayLineValues.Z * outGain) / 64;
            outputBuffers[3][i] = (channelInput.W * dryGain + delayLineValues.W * outGain) / 64;
        }
    }

    void ProcessDelaySurround(DelayState& state, float* const* outputBuffers, float* const* inputBuffers, uint32_t sampleCount) {
        const uint16_t ChannelCount = 6;

        float feedbackGain = FixedPointHelper::ToFloat(static_cast<int64_t>(_parameter.feedback_gain), FixedPointPrecision);
        float delayFeedbackBaseGain = state.DelayFeedbackBaseGain;
        float delayFeedbackCrossGain = state.DelayFeedbackCrossGain;
        float inGain = FixedPointHelper::ToFloat(static_cast<int64_t>(_parameter.in_gain), FixedPointPrecision);
        float dryGain = FixedPointHelper::ToFloat(static_cast<int64_t>(_parameter.dry_gain), FixedPointPrecision);
        float outGain = FixedPointHelper::ToFloat(static_cast<int64_t>(_parameter.out_gain), FixedPointPrecision);

        Matrix6x6 delayFeedback(delayFeedbackBaseGain, 0.0f, delayFeedbackCrossGain, 0.0f, delayFeedbackCrossGain, 0.0f,
                                0.0f, delayFeedbackBaseGain, delayFeedbackCrossGain, 0.0f, 0.0f, delayFeedbackCrossGain,
                                delayFeedbackCrossGain, delayFeedbackCrossGain, delayFeedbackBaseGain, 0.0f, 0.0f, 0.0f,
                                0.0f, 0.0f, 0.0f, feedbackGain, 0.0f, 0.0f,
                                delayFeedbackCrossGain, 0.0f, 0.0f, 0.0f, delayFeedbackBaseGain, delayFeedbackCrossGain,
                                0.0f, delayFeedbackCrossGain, 0.0f, 0.0f, delayFeedbackCrossGain, delayFeedbackBaseGain);

        for (uint32_t i = 0; i < sampleCount; i++) {
            Vector6 channelInput(inputBuffers[0][i] * 64, inputBuffers[1][i] * 64, inputBuffers[2][i] * 64, inputBuffers[3][i] * 64,
                                 inputBuffers[4][i] * 64, inputBuffers[5][i] * 64);

            Vector6 delayLineValues(state.DelayLines[0].Read(), state.DelayLines[1].Read(), state.DelayLines[2].Read(), state.DelayLines[3].Read(),
                                    state.DelayLines[4].Read(), state.DelayLines[5].Read());

            Vector6 transformed = MatrixHelper::Transform(delayLineValues, delayFeedback);
            float temp[ChannelCount] = {transformed.X + channelInput.X * inGain, transformed.Y + channelInput.Y * inGain,
                                        transformed.Z + channelInput.Z * inGain, transformed.W + channelInput.W * inGain,
                                        transformed.V + channelInput.V * inGain, transformed.U + channelInput.U * inGain};

            state.UpdateLowPassFilter(temp[0], ChannelCount);

            outputBuffers[0][i] = (channelInput.X * dryGain + delayLineValues.X * outGain) / 64;
            outputBuffers[1][i] = (channelInput.Y * dryGain + delayLineValues.Y * outGain) / 64;
            outputBuffers[2][i] = (channelInput.Z * dryGain + delayLineValues.Z * outGain) / 64;
            outputBuffers[3][i] = (channelInput.W * dryGain + delayLineValues.W * outGain) / 64;
            outputBuffers[4][i] = (channelInput.V * dryGain + delayLineValues.V * outGain) / 64;
            outputBuffers[5][i] = (channelInput.U * dryGain + delayLineValues.U * outGain) / 64;
        }
    }

    void ProcessDelay(CommandList& context, DelayState& state) {
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
                    ProcessDelayMono(state, outputBuffers[0], inputBuffers[0], context.SampleCount);
                    break;
                case 2:
                    ProcessDelayStereo(state, outputBuffers, inputBuffers, context.SampleCount);
                    break;
                case 4:
                    ProcessDelayQuadraphonic(state, outputBuffers, inputBuffers, context.SampleCount);
                    break;
                case 6:
                    ProcessDelaySurround(state, outputBuffers, inputBuffers, context.SampleCount);
                    break;
                default:
                    NXAUDREN_FATAL("DelayCommand::ProcessDelay: NotImplementedException (channel count)");
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
