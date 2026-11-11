// SPDX-License-Identifier: GPL-3.0-or-later
// Port of Ryujinx.Audio/Renderer/Server/Effect/CaptureBufferEffect.cs (Ryubing fork, MIT). Only the send buffer is resolved;
// the return buffer fields of State stay 0. See ReverbEffect.h for the note on the non-generic Update overrides.
#pragma once
#include "../../Dsp/State/AuxiliaryBufferHeader.h"
#include "../../audren_common.h"
#include "../../audren_types.h"
#include "../MemoryPool/PoolMapper.h"
#include "BaseEffect.h"
#include "EffectParameterHelpers.h"

namespace nxrt::audren {

class CaptureBufferEffect : public BaseEffect {
  public:
    // The capture buffer parameter (C# AuxiliaryBufferParameter Parameter; default-initialised).
    AuxiliaryBufferParameter Parameter{};

    // Capture buffer state (C# AuxiliaryBufferAddresses State, a plain value).
    AuxiliaryBufferAddresses State{};

    CaptureBufferEffect() { Type = TargetEffectType(); }

    EffectType TargetEffectType() const override { return EffectType::CaptureBuffer; }

    DspAddress GetWorkBuffer(int index) override {
        NXAUDREN_ASSERT(index >= 0 && static_cast<size_t>(index) < WorkBuffers.size());
        return WorkBuffers[static_cast<size_t>(index)].GetReference(true);
    }

    void Update(BehaviourParameter::ErrorInfo& updateErrorInfo, const EffectInParameterVersion1& parameter, PoolMapper& mapper) override {
        Update<EffectInParameterVersion1>(updateErrorInfo, parameter, mapper);
    }

    void Update(BehaviourParameter::ErrorInfo& updateErrorInfo, const EffectInParameterVersion2& parameter, PoolMapper& mapper) override {
        Update<EffectInParameterVersion2>(updateErrorInfo, parameter, mapper);
    }

    template <IEffectInParameter T>
    void Update(BehaviourParameter::ErrorInfo& updateErrorInfo, const T& parameter, PoolMapper& mapper) {
        NXAUDREN_ASSERT(IsTypeValid(parameter));

        UpdateParameterBase(parameter);

        Parameter = EffectParameterHelpers::SpecificDataAs<AuxiliaryBufferParameter>(parameter);
        IsEnabled = parameter.is_enabled;

        updateErrorInfo = BehaviourParameter::ErrorInfo{};

        if (BufferUnmapped || parameter.is_new) {
            uint64_t bufferSize = static_cast<uint64_t>(sizeof(int32_t)) * Parameter.buffer_storage_size + sizeof(AuxiliaryBufferHeader);

            bool sendBufferUnmapped = !mapper.TryAttachBuffer(updateErrorInfo, WorkBuffers[0], Parameter.send_buffer_info_address, bufferSize);

            BufferUnmapped = sendBufferUnmapped;

            if (!BufferUnmapped) {
                DspAddress sendDspAddress = WorkBuffers[0].GetReference(false);

                // NOTE: the platform directly interact with the CPU side structure in the processing of the DSP command.
                State.send_buffer_info = sendDspAddress;
                State.send_buffer_info_base = sendDspAddress + sizeof(AuxiliaryBufferHeader);
                State.return_buffer_info = 0;
                State.return_buffer_info_base = 0;
            }
        }
    }

    void UpdateForCommandGeneration() override { UpdateUsageStateForCommandGeneration(); }
};

}  // namespace nxrt::audren
