// SPDX-License-Identifier: GPL-3.0-or-later
// Port of Ryujinx.Audio/Renderer/Server/CommandProcessingTimeEstimatorVersion1.cs (Ryubing fork, MIT). Version 1 (the first estimator).
#pragma once
#include "ICommandProcessingTimeEstimator.h"

namespace nxrt::audren {

class CommandProcessingTimeEstimatorVersion1 : public ICommandProcessingTimeEstimator {
  private:
    uint32_t _sampleCount;
    uint32_t _bufferCount;

  public:
    CommandProcessingTimeEstimatorVersion1(uint32_t sampleCount, uint32_t bufferCount) : _sampleCount(sampleCount), _bufferCount(bufferCount) {}

    uint32_t Estimate(const PerformanceCommand& command) override { return 1454; }

    uint32_t Estimate(const ClearMixBufferCommand& command) override {
        return estimator_detail::FloatToUInt32(_sampleCount * 0.83f * _bufferCount * 1.2f);
    }

    uint32_t Estimate(const BiquadFilterCommand& command) override { return estimator_detail::FloatToUInt32(_sampleCount * 58.0f * 1.2f); }

    uint32_t Estimate(const MixRampGroupedCommand& command) override {
        int volumeCount = 0;

        for (uint32_t i = 0; i < command.MixBufferCount; i++) {
            if (command.Volume0[i] != 0.0f || command.Volume1[i] != 0.0f) {
                volumeCount++;
            }
        }

        return estimator_detail::FloatToUInt32(_sampleCount * 14.4f * 1.2f * volumeCount);
    }

    uint32_t Estimate(const MixRampCommand& command) override { return estimator_detail::FloatToUInt32(_sampleCount * 14.4f * 1.2f); }

    uint32_t Estimate(const DepopPrepareCommand& command) override { return 1080; }

    uint32_t Estimate(const VolumeRampCommand& command) override { return estimator_detail::FloatToUInt32(_sampleCount * 9.8f * 1.2f); }

    uint32_t Estimate(const PcmInt16DataSourceCommandVersion1& command) override {
        return estimator_detail::FloatToUInt32(command.Pitch * 0.25f * 1.2f);
    }

    uint32_t Estimate(const AdpcmDataSourceCommandVersion1& command) override {
        return estimator_detail::FloatToUInt32(command.Pitch * 0.46f * 1.2f);
    }

    uint32_t Estimate(const DepopForMixBuffersCommand& command) override {
        return estimator_detail::FloatToUInt32(_sampleCount * 8.9f * command.MixBufferCount);
    }

    uint32_t Estimate(const CopyMixBufferCommand& command) override {
        // NOTE: the platform returns 0 here for some reasons even if it will generate a command like that on version 1.. maybe a mistake?
        return 0;
    }

    uint32_t Estimate(const MixCommand& command) override { return estimator_detail::FloatToUInt32(_sampleCount * 10.0f * 1.2f); }

    uint32_t Estimate(const DelayCommand& command) override {
        const DelayParameter parameter = command.Parameter();
        return estimator_detail::FloatToUInt32(_sampleCount * parameter.channel_count * 202.5f);
    }

    uint32_t Estimate(const ReverbCommand& command) override {
        const ReverbParameter parameter = command.Parameter();
        NXAUDREN_ASSERT(estimator_detail::IsChannelCountValid(parameter.channel_count));

        if (command.Enabled) {
            return estimator_detail::FloatToUInt32(750u * _sampleCount * parameter.channel_count * 1.2f);
        }

        return 0;
    }

    uint32_t Estimate(const Reverb3dCommand& command) override {
        const Reverb3dParameter parameter = command.Parameter();

        if (command.Enabled) {
            return estimator_detail::FloatToUInt32(530u * _sampleCount * parameter.channel_count * 1.2f);
        }

        return 0;
    }

    uint32_t Estimate(const AuxiliaryBufferCommand& command) override {
        if (command.Enabled) {
            return 15956;
        }

        return 3765;
    }

    uint32_t Estimate(const VolumeCommand& command) override { return estimator_detail::FloatToUInt32(_sampleCount * 8.8f * 1.2f); }

    uint32_t Estimate(const CircularBufferSinkCommand& command) override { return 55; }

    uint32_t Estimate(const DownMixSurroundToStereoCommand& command) override { return 16108; }

    uint32_t Estimate(const UpsampleCommand& command) override { return 357915; }

    uint32_t Estimate(const DeviceSinkCommand& command) override { return 10042; }

    uint32_t Estimate(const PcmFloatDataSourceCommandVersion1& command) override { return 0; }

    uint32_t Estimate(const DataSourceVersion2Command& command) override { return 0; }

    uint32_t Estimate(const LimiterCommandVersion1& command) override { return 0; }

    uint32_t Estimate(const LimiterCommandVersion2& command) override { return 0; }

    uint32_t Estimate(const MultiTapBiquadFilterCommand& command) override { return 0; }

    uint32_t Estimate(const CaptureBufferCommand& command) override { return 0; }

    uint32_t Estimate(const CompressorCommand& command) override { return 0; }

    uint32_t Estimate(const BiquadFilterAndMixCommand& command) override { return 0; }

    uint32_t Estimate(const MultiTapBiquadFilterAndMixCommand& command) override { return 0; }

    uint32_t Estimate(const FillBufferCommand& command) override { return 0; }
};

}  // namespace nxrt::audren
