// SPDX-License-Identifier: GPL-3.0-or-later
// Port of Ryujinx.Audio/Renderer/Server/BehaviourInfo.cs (Ryubing fork, MIT). Feature gating by the user revision, the
// behaviour flags, and the bounded list of ErrorInfo reported back to the user.
#pragma once
#include <array>

#include "../Constants.h"
#include "../audren_common.h"
#include "../audren_types.h"

namespace nxrt::audren {

class BehaviourInfo {
  public:
    // C# const int members (revision magic is 'R' 'E' 'V' '0'+N, the revision number in the top byte).
    static constexpr int32_t BaseRevisionMagic = ('R' << 0) | ('E' << 8) | ('V' << 16) | ('0' << 24);
    static constexpr int32_t Revision1 = 1 << 24;
    static constexpr int32_t Revision2 = 2 << 24;
    static constexpr int32_t Revision3 = 3 << 24;
    static constexpr int32_t Revision4 = 4 << 24;
    static constexpr int32_t Revision5 = 5 << 24;
    static constexpr int32_t Revision6 = 6 << 24;
    static constexpr int32_t Revision7 = 7 << 24;
    static constexpr int32_t Revision8 = 8 << 24;
    static constexpr int32_t Revision9 = 9 << 24;
    static constexpr int32_t Revision10 = 10 << 24;
    static constexpr int32_t Revision11 = 11 << 24;
    static constexpr int32_t Revision12 = 12 << 24;
    static constexpr int32_t Revision13 = 13 << 24;
    static constexpr int32_t Revision14 = 14 << 24;
    static constexpr int32_t Revision15 = 15 << 24;
    // Last revision supported by the implementation.
    static constexpr int32_t LastRevision = Revision15;
    // Target revision magic supported by the implementation.
    static constexpr int32_t ProcessRevision = BaseRevisionMagic + LastRevision;

    // Get the revision number from the revision magic.
    static constexpr int32_t GetRevisionNumber(int32_t revision) { return (revision - BaseRevisionMagic) >> 24; }

    // platform 2 SDKs tag their native renderer with 'REv' (lower-case v) + the revision digit instead of 'REV', and the native revision goes
    // one past ours (NativeLastRevision, 16; Eglantine NX2_NOTES rows 38-39). GetRevisionNumber already reads such a tag (the case bit sits
    // below the shifted-out bytes); only the validity check and the update-header comparison need to know about it.
    static constexpr int32_t NativeCaseBit = 0x20 << 16;   // 'v' - 'V'
    static constexpr int32_t NativeLastRevision = 16;
    static constexpr bool IsNativeRevision(int32_t revision) { return (revision & 0x00FFFFFF) == (BaseRevisionMagic & 0x00FFFFFF) + NativeCaseBit; }
    // True when the two revision tags differ at most in the case of the 'V' (the open call and later update headers may disagree on it).
    static constexpr bool SameRevision(int32_t a, int32_t b) { return (a ^ b) == 0 || (((a ^ b) & ~NativeCaseBit) == 0 && GetRevisionNumber(a) == GetRevisionNumber(b)); }

    // Current active revision (C# `{ get; private set; }`, so it is a plain member set through SetUserRevision).
    int32_t UserRevision = 0;

  private:
    // Current flags of the BehaviourInfo.
    uint64_t _flags = 0;
    // Error storage (C# Array10<ErrorInfo>, zero-initialised).
    std::array<BehaviourParameter::ErrorInfo, static_cast<size_t>(Constants::MaxErrorInfos)> _errorInfos{};
    // Current position in _errorInfos.
    uint32_t _errorIndex = 0;

  public:
    BehaviourInfo() = default;

    // Set the active revision.
    void SetUserRevision(int32_t userRevision) { UserRevision = userRevision; }

    // Update flags of the BehaviourInfo.
    void UpdateFlags(uint64_t flags) { _flags = flags; }

    // Check if a given revision is valid/supported.
    static bool CheckValidRevision(int32_t revision) {
        const int32_t number = GetRevisionNumber(revision);
        return number <= GetRevisionNumber(ProcessRevision) || (IsNativeRevision(revision) && number <= NativeLastRevision);
    }

    // Check if the given revision is greater than or equal the supported revision.
    static bool CheckFeatureSupported(int32_t revision, int32_t supportedRevision) {
        int32_t revA = GetRevisionNumber(revision);
        int32_t revB = GetRevisionNumber(supportedRevision);

        if (revA > LastRevision) {
            revA = 1;
        }

        if (revB > LastRevision) {
            revB = 1;
        }

        return revA >= revB;
    }

    // Check if the memory pool mapping bypass flag is active.
    bool IsMemoryPoolForceMappingEnabled() const { return (_flags & 1) != 0; }

    // Check if the audio renderer should fix the GC-ADPCM context not being provided to the DSP.
    bool IsAdpcmLoopContextBugFixed() const { return CheckFeatureSupported(UserRevision, BaseRevisionMagic + Revision2); }

    // Check if the audio renderer should accept splitters.
    bool IsSplitterSupported() const { return CheckFeatureSupported(UserRevision, BaseRevisionMagic + Revision2); }

    // Check if the audio renderer should use a max pre-delay of 350 instead of 150.
    bool IsLongSizePreDelaySupported() const { return CheckFeatureSupported(UserRevision, BaseRevisionMagic + Revision3); }

    // Check if the audio renderer should expose USB audio device.
    bool IsAudioUsbDeviceOutputSupported() const { return CheckFeatureSupported(UserRevision, BaseRevisionMagic + Revision4); }

    // Get the percentage allocated to the audio renderer on the DSP for processing.
    float GetAudioRendererProcessingTimeLimit() const {
        if (CheckFeatureSupported(UserRevision, BaseRevisionMagic + Revision5)) {
            return 0.80f;
        }

        if (CheckFeatureSupported(UserRevision, BaseRevisionMagic + Revision4)) {
            return 0.75f;
        }

        return 0.70f;
    }

    // Check if the audio render should support voice flushing.
    bool IsFlushVoiceWaveBuffersSupported() const { return CheckFeatureSupported(UserRevision, BaseRevisionMagic + Revision5); }

    // Check if the audio renderer should trust the user destination count in the splitter update.
    bool IsSplitterBugFixed() const { return CheckFeatureSupported(UserRevision, BaseRevisionMagic + Revision5); }

    // Check if the audio renderer should supply the elapsed frame count to the user when updating.
    bool IsElapsedFrameCountSupported() const { return CheckFeatureSupported(UserRevision, BaseRevisionMagic + Revision5); }

    // Get the performance metric data format version.
    uint32_t GetPerformanceMetricsDataFormat() const {
        if (CheckFeatureSupported(UserRevision, BaseRevisionMagic + Revision5)) {
            return 2;
        }

        return 1;
    }

    // Check if the audio renderer should support VoiceInParameter1.DecodingBehaviour.
    bool IsDecodingBehaviourFlagSupported() const { return CheckFeatureSupported(UserRevision, BaseRevisionMagic + Revision5); }

    // Check if the audio renderer should fix the biquad filter command not clearing up its state with the New usage state.
    bool IsBiquadFilterEffectStateClearBugFixed() const { return CheckFeatureSupported(UserRevision, BaseRevisionMagic + Revision6); }

    // Check if the audio renderer should accept partial mix updates.
    bool IsMixInParameterDirtyOnlyUpdateSupported() const { return CheckFeatureSupported(UserRevision, BaseRevisionMagic + Revision7); }

    // Check if the audio renderer should use the new wavebuffer format.
    bool IsWaveBufferVersion2Supported() const { return CheckFeatureSupported(UserRevision, BaseRevisionMagic + Revision8); }

    // Check if the audio renderer should use the new effect info format.
    bool IsEffectInfoVersion2Supported() const { return CheckFeatureSupported(UserRevision, BaseRevisionMagic + Revision9); }

    // Check if the audio renderer should use an optimized Biquad Filter (Direct Form 1) when two biquad filters are defined on a voice.
    bool UseMultiTapBiquadFilterProcessing() const { return CheckFeatureSupported(UserRevision, BaseRevisionMagic + Revision10); }

    // Check if the audio renderer should support new channel resource mapping for 5.1 on Delay, Reverb and Reverb 3D effects.
    bool IsNewEffectChannelMappingSupported() const { return CheckFeatureSupported(UserRevision, BaseRevisionMagic + Revision11); }

    // Check if the audio renderer should support biquad filter on splitter.
    bool IsBiquadFilterParameterForSplitterEnabled() const { return CheckFeatureSupported(UserRevision, BaseRevisionMagic + Revision12); }

    // Check if the audio renderer should support explicit previous mix volume reset on splitter.
    bool IsSplitterPrevVolumeResetSupported() const { return CheckFeatureSupported(UserRevision, BaseRevisionMagic + Revision13); }

    // Check if the audio renderer should support the depop bug fix.
    bool IsSplitterDepopBugFixEnabled() const { return CheckFeatureSupported(UserRevision, BaseRevisionMagic + Revision14); }

    // Check if the audio renderer should support biquad filter with float coefficients.
    bool IsBiquadFilterParameterFloatSupported() const { return CheckFeatureSupported(UserRevision, BaseRevisionMagic + Revision15); }

    // Get the version of the ICommandProcessingTimeEstimator.
    int32_t GetCommandProcessingTimeEstimatorVersion() const {
        if (CheckFeatureSupported(UserRevision, BaseRevisionMagic + Revision11)) {
            return 5;
        }

        if (CheckFeatureSupported(UserRevision, BaseRevisionMagic + Revision10)) {
            return 4;
        }

        if (CheckFeatureSupported(UserRevision, BaseRevisionMagic + Revision8)) {
            return 3;
        }

        if (CheckFeatureSupported(UserRevision, BaseRevisionMagic + Revision5)) {
            return 2;
        }

        return 1;
    }

    // Append a new ErrorInfo to the error array. Entries past MaxErrorInfos are dropped.
    // NOTE: the C# Debug.Assert here checks `ErrorCode == Success`, which looks inverted (callers only append non-success codes).
    // It is kept literally; NXAUDREN_ASSERT compiles to nothing in Release builds.
    void AppendError(BehaviourParameter::ErrorInfo& errorInfo) {
        NXAUDREN_ASSERT(errorInfo.error_code == ResultCode::Success);

        if (_errorIndex <= static_cast<uint32_t>(Constants::MaxErrorInfos) - 1) {
            _errorInfos[_errorIndex++] = errorInfo;
        }
    }

    // Copy the internal ErrorInfo array to the given span and output the count of valid entries.
    void CopyErrorInfo(Span<BehaviourParameter::ErrorInfo> errorInfos, uint32_t& errorCount) const {
        if (errorInfos.Length != static_cast<size_t>(Constants::MaxErrorInfos)) {
            NXAUDREN_FATAL("Invalid size of errorInfos span!");
        }

        errorCount = std::min(_errorIndex, static_cast<uint32_t>(Constants::MaxErrorInfos));

        Span<const BehaviourParameter::ErrorInfo>(_errorInfos.data(), _errorInfos.size()).CopyTo(errorInfos);
    }

    // Clear the ErrorInfo array.
    void ClearError() { _errorIndex = 0; }
};

}  // namespace nxrt::audren
