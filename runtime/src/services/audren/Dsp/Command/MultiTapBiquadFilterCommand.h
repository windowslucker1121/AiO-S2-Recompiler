// SPDX-License-Identifier: GPL-3.0-or-later
// Port of Ryujinx.Audio/Renderer/Dsp/Command/MultiTapBiquadFilterCommand.cs (Ryubing fork, MIT).
// C# `ToArray()` copies of the parameter and flag spans become std::vector members. The C# bool[] becomes std::vector<bool>.
#pragma once
#include <vector>

#include "../../audren_common.h"
#include "../../audren_types.h"
#include "../BiquadFilterHelper.h"
#include "CommandList.h"
#include "ICommand.h"

namespace nxrt::audren {

class MultiTapBiquadFilterCommand : public ICommand {
  public:
    int32_t NodeId = 0;
    uint32_t EstimatedProcessingTime = 0;

    std::vector<BiquadFilterParameter2> Parameters;
    Span<::nxrt::audren::BiquadFilterState> BiquadFilterStates;
    int32_t InputBufferIndex = 0;
    int32_t OutputBufferIndex = 0;
    std::vector<bool> IsInitialized;

    MultiTapBiquadFilterCommand() = default;

    MultiTapBiquadFilterCommand& Initialize(int32_t baseIndex, Span<const BiquadFilterParameter2> filters,
                                            Span<::nxrt::audren::BiquadFilterState> biquadFilterStateMemory, int32_t inputBufferOffset,
                                            int32_t outputBufferOffset, Span<const bool> isInitialized, int32_t nodeId) {
        Parameters.assign(filters.begin(), filters.end());
        BiquadFilterStates = biquadFilterStateMemory;
        InputBufferIndex = baseIndex + inputBufferOffset;
        OutputBufferIndex = baseIndex + outputBufferOffset;
        IsInitialized.assign(isInitialized.begin(), isInitialized.end());

        Enabled = true;
        NodeId = nodeId;

        return *this;
    }

    int32_t GetNodeId() const override { return NodeId; }
    CommandType GetCommandType() const override { return CommandType::MultiTapBiquadFilter; }
    uint32_t GetEstimatedProcessingTime() const override { return EstimatedProcessingTime; }

    void Process(CommandList& context) override {
        Span<::nxrt::audren::BiquadFilterState> states = BiquadFilterStates;

        Span<const float> inputBuffer = context.GetBuffer(InputBufferIndex);
        Span<float> outputBuffer = context.GetBuffer(OutputBufferIndex);

        for (size_t i = 0; i < Parameters.size(); i++) {
            if (!IsInitialized[i]) {
                states[i] = ::nxrt::audren::BiquadFilterState{};
            }
        }

        // NOTE: the platform only implement single and double biquad filters but no generic path when the command definition suggests it could be done.
        // As such the generic path is only used for the double case here, as in the C#.
        if (Parameters.size() == 1) {
            BiquadFilterHelper::ProcessBiquadFilter(Parameters[0], states[0], outputBuffer, inputBuffer, context.SampleCount);
        } else {
            BiquadFilterHelper::ProcessBiquadFilter(Span<const BiquadFilterParameter2>(Parameters), states, outputBuffer, inputBuffer, context.SampleCount);
        }
    }
};

}  // namespace nxrt::audren
