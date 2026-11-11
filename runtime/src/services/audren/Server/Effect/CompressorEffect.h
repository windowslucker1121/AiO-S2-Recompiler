// SPDX-License-Identifier: GPL-3.0-or-later
// Port of Ryujinx.Audio/Renderer/Server/Effect/CompressorEffect.cs (Ryubing fork, MIT). The version 1 Update does nothing
// except initialise the error info (the platform's behaviour, kept); only the version 2 Update applies the parameter.
#pragma once
#include "../../Dsp/State/CompressorState.h"
#include "../../audren_common.h"
#include "../../audren_types.h"
#include "../MemoryPool/PoolMapper.h"
#include "BaseEffect.h"
#include "EffectParameterHelpers.h"

namespace nxrt::audren {

class CompressorEffect : public BaseEffect {
  public:
    // The compressor parameter (C# CompressorParameter Parameter; default-initialised).
    CompressorParameter Parameter{};

    // The compressor state (C# Memory<CompressorState>, one element).
    Span<CompressorState> State;

  private:
    EffectStateSlot<CompressorState> _state;

  public:
    CompressorEffect() {
        Type = TargetEffectType();
        State = _state.View();
    }

    EffectType TargetEffectType() const override { return EffectType::Compressor; }

    DspAddress GetWorkBuffer([[maybe_unused]] int index) override { return GetSingleBuffer(); }

    void Update(BehaviourParameter::ErrorInfo& updateErrorInfo, [[maybe_unused]] const EffectInParameterVersion1& parameter,
                [[maybe_unused]] PoolMapper& mapper) override {
        // the platform doesn't do anything here but we still require updateErrorInfo to be initialised.
        updateErrorInfo = BehaviourParameter::ErrorInfo{};
    }

    void Update(BehaviourParameter::ErrorInfo& updateErrorInfo, const EffectInParameterVersion2& parameter, [[maybe_unused]] PoolMapper& mapper) override {
        NXAUDREN_ASSERT(IsTypeValid(parameter));

        UpdateParameterBase(parameter);

        Parameter = EffectParameterHelpers::SpecificDataAs<CompressorParameter>(parameter);
        IsEnabled = parameter.is_enabled;

        updateErrorInfo = BehaviourParameter::ErrorInfo{};
    }

    void UpdateForCommandGeneration() override {
        UpdateUsageStateForCommandGeneration();

        Parameter.status = ::nxrt::audren::UsageState::Enabled;
        Parameter.statistics_reset = false;
    }

    void InitializeResultState(EffectResultState& state) override {
        CompressorStatistics statistics = EffectParameterHelpers::LoadBytes<CompressorStatistics>(state.specific_data);

        EffectParameterHelpers::Reset(statistics, Parameter.channel_count);

        EffectParameterHelpers::StoreBytes(state.specific_data, statistics);
    }

    void UpdateResultState(EffectResultState& destState, EffectResultState& srcState) override { destState = srcState; }
};

}  // namespace nxrt::audren
