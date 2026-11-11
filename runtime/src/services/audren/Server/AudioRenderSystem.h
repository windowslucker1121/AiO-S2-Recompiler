// SPDX-License-Identifier: GPL-3.0-or-later
// Port of Ryujinx.Audio/Renderer/Server/AudioRenderSystem.cs (Ryubing fork, MIT): one audio renderer session. It owns the work buffer
// and the server objects built in it, applies the user's update (Update), generates the DSP command list of a frame (SendCommands) and
// hands it to the AudioProcessor.
//
// Deviations from the C#, all forced by the port conventions or by C++ ownership:
//   * IVirtualMemoryManager / processHandle-based mapping: guest memory is host memory (audren_common.h). memoryManager parameters and
//     the MemoryManager property are dropped; processHandle stays where a value is stored (PoolMapper, StateUpdater).
//   * The work buffer backing store is a std::vector<uint8_t> that the renderer owns (C# GC.AllocateArray + Memory pin). Initialize takes
//     it by value and moves it in, so the Span<> views into it stay valid for the renderer's lifetime.
//   * C# `_manager` (AudioRendererManager) is a reference. The members that use the manager (Processor, TickSource, Unregister) are
//     declared here and defined inline in AudioRendererManager.h. The two headers include each other at their ends, so including
//     either one gives a complete API.
//   * C# `lock (_lock)` is a std::mutex; `Interlocked.CompareExchange(ref _disposeState)` is a std::atomic.
//   * C# `ref AudioRendererConfiguration` parameters are non-const references. The C# CommandList / IWritableEvent / IVirtualMemoryManager
//     out or ref parameters become pointers or unique_ptr references as noted at each method.
//   * C# `ReadOnlySequence<byte> input` is one contiguous Span<const uint8_t> (see StateUpdater.h).
//   * C# `Debug.Assert` -> NXAUDREN_ASSERT; C# `throw new NotImplementedException` -> NXAUDREN_FATAL.
#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "../Common/EdgeMatrix.h"
#include "../Common/NodeIdHelper.h"
#include "../Common/NodeStates.h"
#include "../Common/WorkBufferAllocator.h"
#include "../Constants.h"
#include "../Dsp/AudioProcessor.h"
#include "../Dsp/Command/CommandList.h"
#include "../Integration/IWritableEvent.h"
#include "../Utils/BitUtils.h"
#include "../audren_common.h"
#include "../audren_types.h"
#include "BehaviourInfo.h"
#include "CommandBuffer.h"
#include "CommandGenerator.h"
#include "CommandProcessingTimeEstimatorVersion1.h"
#include "CommandProcessingTimeEstimatorVersion2.h"
#include "CommandProcessingTimeEstimatorVersion3.h"
#include "CommandProcessingTimeEstimatorVersion4.h"
#include "CommandProcessingTimeEstimatorVersion5.h"
#include "ICommandProcessingTimeEstimator.h"
#include "Effect/EffectContext.h"
#include "MemoryPool/MemoryPoolInfo.h"
#include "MemoryPool/PoolMapper.h"
#include "Mix/MixContext.h"
#include "Mix/MixInfo.h"
#include "Performance/PerformanceManager.h"
#include "RendererSystemContext.h"
#include "Sink/SinkContext.h"
#include "Splitter/SplitterContext.h"
#include "StateUpdater.h"
#include "Upsampler/UpsamplerManager.h"
#include "Voice/VoiceChannelResource.h"
#include "Voice/VoiceContext.h"
#include "Voice/VoiceInfo.h"
#include "nxrt/log.h"

namespace nxrt::audren {

// Gauges (audio.cpp registers them): totals over every open renderer. Each renderer adds its change at each update (gauge_set) and gives
// its share back when it is destroyed. Read from the sampler thread: relaxed atomics only.
inline std::atomic<int64_t> g_gauge_voices_in_use{0};
inline std::atomic<int64_t> g_gauge_wavebuffers_queued{0};
inline std::atomic<int64_t> g_gauge_memory_pools{0};
inline std::atomic<uint64_t> g_gauge_frames_rendered{0};
inline void gauge_set(std::atomic<int64_t>& total, int64_t& mine, int64_t now) {
    total.fetch_add(now - mine, std::memory_order_relaxed);
    mine = now;
}

class AudioRendererManager;

class AudioRenderSystem {
  private:
    std::mutex _lock;

    server::AudioRendererRenderingDevice _renderingDevice = server::AudioRendererRenderingDevice::Dsp;
    server::AudioRendererExecutionMode _executionMode = server::AudioRendererExecutionMode::Auto;
    IWritableEvent* _systemEvent = nullptr;
    MemoryPoolInfo _dspMemoryPoolInfo = MemoryPoolInfo::Create(MemoryPoolInfo::LocationType::Dsp);
    VoiceContext _voiceContext;
    MixContext _mixContext;
    SinkContext _sinkContext;
    SplitterContext _splitterContext;
    EffectContext _effectContext;
    std::unique_ptr<PerformanceManager> _performanceManager;
    std::unique_ptr<UpsamplerManager> _upsamplerManager;
    bool _isActive = false;
    BehaviourInfo _behaviourInfo;
    uint64_t _totalElapsedTicksUpdating = 0;
    uint64_t _totalElapsedTicks = 0;
    int32_t _sessionId = 0;
    Span<MemoryPoolInfo> _memoryPools;

    uint32_t _sampleRate = 0;
    uint32_t _sampleCount = 0;
    uint32_t _mixBufferCount = 0;
    uint32_t _voiceChannelCountMax = 0;
    uint32_t _upsamplerCount = 0;
    uint32_t _memoryPoolCount = 0;
    uint32_t _processHandle = 0;
    uint64_t _appletResourceId = 0;
    // What this renderer last added to the gauges above.
    int64_t _gaugeVoices = 0, _gaugeWaves = 0, _gaugePools = 0;

    // The work buffer backing store, owned by the renderer (C# Memory<byte> workBufferMemory, pinned).
    std::vector<uint8_t> _workBufferMemory;

    Span<float> _mixBuffer;
    Span<float> _depopBuffer;

    uint32_t _renderingTimeLimitPercent = 100;
    bool _voiceDropEnabled = false;
    uint32_t _voiceDropCount = 0;
    float _voiceDropParameter = 1.0f;
    bool _isDspRunningBehind = false;

    std::unique_ptr<ICommandProcessingTimeEstimator> _commandProcessingTimeEstimator;

    Span<uint8_t> _performanceBuffer;

    uint64_t _elapsedFrameCount = 0;
    uint64_t _renderingStartTick = 0;

    AudioRendererManager& _manager;

    std::atomic<int32_t> _disposeState{0};

    // Ticks of the system clock (C# GetSystemTicks: TickSource.ElapsedSeconds * TargetTimerFrequency). Defined in AudioRendererManager.h.
    uint64_t GetSystemTicks();

    // C# ComputeVoiceDrop. Returns the count of voices dropped for this frame.
    uint32_t ComputeVoiceDrop(CommandBuffer& commandBuffer, uint32_t voicesEstimatedTime, int64_t deltaTimeDsp) {
        CommandList& commandList = commandBuffer.CommandList;
        const size_t count = commandList.Commands.size();
        size_t i = 0;

        for (; i < count; i++) {
            CommandType commandType = commandList.Commands[i]->GetCommandType();

            if (commandType == CommandType::AdpcmDataSourceVersion1 || commandType == CommandType::AdpcmDataSourceVersion2 ||
                commandType == CommandType::PcmInt16DataSourceVersion1 || commandType == CommandType::PcmInt16DataSourceVersion2 ||
                commandType == CommandType::PcmFloatDataSourceVersion1 || commandType == CommandType::PcmFloatDataSourceVersion2 ||
                commandType == CommandType::Performance) {
                break;
            }
        }

        uint32_t voiceDropped = 0;

        // NOTE: as in the C#, the outer loop's i++ after an inner loop that stopped on a different node skips that command.
        for (; i < count; i++) {
            ICommand& targetCommand = *commandList.Commands[i];

            int32_t targetNodeId = targetCommand.GetNodeId();

            if (static_cast<int64_t>(voicesEstimatedTime) <= deltaTimeDsp || NodeIdHelper::GetType(targetNodeId) != NodeIdType::Voice) {
                break;
            }

            VoiceInfo& voice = _voiceContext.GetState(NodeIdHelper::GetBase(targetNodeId));

            if (voice.Priority == static_cast<uint32_t>(Constants::VoiceHighestPriority)) {
                break;
            }

            // We can safely drop this voice, disable all associated commands while activating depop preparation commands.
            voiceDropped++;
            voice.VoiceDropFlag = true;

            NXLOG_W("AudioRenderer", "Dropping voice %d", voice.NodeId);

            for (; i < count; i++) {
                ICommand& command = *commandList.Commands[i];

                if (command.GetNodeId() != targetNodeId) {
                    break;
                }

                if (command.GetCommandType() == CommandType::DepopPrepare) {
                    command.Enabled = true;
                } else if (command.GetCommandType() == CommandType::Performance || !command.Enabled) {
                    continue;
                } else {
                    command.Enabled = false;

                    voicesEstimatedTime -= static_cast<uint32_t>(_voiceDropParameter * static_cast<float>(command.GetEstimatedProcessingTime()));
                }
            }
        }

        return voiceDropped;
    }

    // C# GenerateCommandList(out CommandList). The caller owns the returned list.
    std::unique_ptr<CommandList> GenerateCommandList() {
        NXAUDREN_ASSERT(_executionMode == server::AudioRendererExecutionMode::Auto);

        PoolMapper::ClearUsageState(_memoryPools);

        uint64_t startTicks = GetSystemTicks();

        auto commandList = std::make_unique<CommandList>(_mixBuffer, _sampleCount, _sampleRate, _mixBufferCount, _voiceChannelCountMax);

        if (_performanceManager != nullptr) {
            _performanceManager->TapFrame(_isDspRunningBehind, _voiceDropCount, _renderingStartTick);

            _isDspRunningBehind = false;
            _voiceDropCount = 0;
            _renderingStartTick = 0;
        }

        CommandBuffer commandBuffer(*commandList, *_commandProcessingTimeEstimator);

        RendererSystemContext context = GetContext();

        CommandGenerator commandGenerator(commandBuffer, context, _voiceContext, _mixContext, _effectContext, _sinkContext, _splitterContext,
                                          _performanceManager.get());

        _voiceContext.Sort();
        commandGenerator.GenerateVoices();

        uint32_t voicesEstimatedTime = static_cast<uint32_t>(_voiceDropParameter * static_cast<float>(commandBuffer.EstimatedProcessingTime));

        commandGenerator.GenerateSubMixes();
        commandGenerator.GenerateFinalMixes();
        commandGenerator.GenerateSinks();

        uint32_t totalEstimatedTime = static_cast<uint32_t>(_voiceDropParameter * static_cast<float>(commandBuffer.EstimatedProcessingTime));

        if (_voiceDropEnabled) {
            int64_t maxDspTime = GetMaxAllocatedTimeForDsp();

            // C# computes this difference in uint (it wraps) and widens it to long afterwards.
            uint32_t restEstimateTimeUnsigned = totalEstimatedTime - voicesEstimatedTime;
            int64_t restEstimateTime = static_cast<int64_t>(restEstimateTimeUnsigned);

            int64_t deltaTimeDsp = std::max<int64_t>(maxDspTime - restEstimateTime, 0);

            _voiceDropCount = ComputeVoiceDrop(commandBuffer, voicesEstimatedTime, deltaTimeDsp);
        }

        _voiceContext.UpdateForCommandGeneration();

        if (_behaviourInfo.IsEffectInfoVersion2Supported()) {
            _effectContext.UpdateResultStateForCommandGeneration();
        }

        uint64_t endTicks = GetSystemTicks();

        _totalElapsedTicks = endTicks - startTicks;

        _renderingStartTick = GetSystemTicks();
        _elapsedFrameCount++;
        {   // gauges: voices in use and their queued wave buffers, and the frame count (relaxed atomics, read by the sampler)
            int64_t voices = 0, waves = 0;
            for (uint32_t i = 0; i < _voiceContext.GetCount(); i++) {
                const VoiceInfo& v = _voiceContext.GetState(static_cast<int32_t>(i));
                if (v.InUse) {
                    voices++;
                    waves += static_cast<int64_t>(v.WaveBuffersCount);
                }
            }
            gauge_set(g_gauge_voices_in_use, _gaugeVoices, voices);
            gauge_set(g_gauge_wavebuffers_queued, _gaugeWaves, waves);
            g_gauge_frames_rendered.fetch_add(1, std::memory_order_relaxed);
        }

        return commandList;
    }

    // C# GetMaxAllocatedTimeForDsp.
    int32_t GetMaxAllocatedTimeForDsp() {
        return static_cast<int32_t>(static_cast<float>(Constants::AudioProcessorMaxUpdateTimePerSessions) *
                                    _behaviourInfo.GetAudioRendererProcessingTimeLimit() * (static_cast<float>(GetRenderingTimeLimit()) / 100.0f));
    }

    // C# GetContext(): the lite context the command generator reads. Defined in AudioRendererManager.h (needs the output device).
    RendererSystemContext GetContext();

    // The work of Dispose(true). Defined in AudioRendererManager.h (unregisters from the manager).
    void DisposeImpl();

  public:
    AudioRenderSystem(AudioRendererManager& manager, IWritableEvent* systemEvent)
        : _systemEvent(systemEvent), _manager(manager) {
        _dspMemoryPoolInfo = MemoryPoolInfo::Create(MemoryPoolInfo::LocationType::Dsp);
        _commandProcessingTimeEstimator = nullptr;
        _behaviourInfo = BehaviourInfo();
        _totalElapsedTicksUpdating = 0;
        _sessionId = 0;
        _voiceDropParameter = 1.0f;
    }

    // Defined in AudioRendererManager.h: Dispose() then the manager is told that the session is gone.
    ~AudioRenderSystem();

    AudioRenderSystem(const AudioRenderSystem&) = delete;
    AudioRenderSystem& operator=(const AudioRenderSystem&) = delete;

    // C# Initialize. `workBufferMemory` is the backing store (at least workBufferSize bytes), taken over by the renderer.
    // `workBuffer` is the guest address of the work buffer (the DSP pool's CPU address).
    ResultCode Initialize(AudioRendererConfiguration& parameter, uint32_t processHandle, std::vector<uint8_t> workBufferMemory, CpuAddress workBuffer,
                          uint64_t workBufferSize, int32_t sessionId, uint64_t appletResourceId) {
        if (!BehaviourInfo::CheckValidRevision(parameter.revision)) {
            return ResultCode::OperationFailed;
        }

        if (GetWorkBufferSize(parameter) > workBufferSize) {
            return ResultCode::WorkBufferTooSmall;
        }

        NXAUDREN_ASSERT(parameter.rendering_device == server::AudioRendererRenderingDevice::Dsp &&
                        parameter.execution_mode == server::AudioRendererExecutionMode::Auto);

        NXLOG_I("AudioRenderer", "Initializing with REV%d", BehaviourInfo::GetRevisionNumber(parameter.revision));

        _behaviourInfo.SetUserRevision(parameter.revision);

        _sampleRate = parameter.sample_rate;
        _sampleCount = parameter.sample_count;
        _mixBufferCount = parameter.mix_buffer_count;
        _voiceChannelCountMax = static_cast<uint32_t>(Constants::VoiceChannelCountMax);
        _upsamplerCount = parameter.sink_count + parameter.sub_mix_buffer_count;
        _appletResourceId = appletResourceId;
        _memoryPoolCount = parameter.effect_count + parameter.voice_count * static_cast<uint32_t>(Constants::VoiceWaveBufferCount);
        gauge_set(g_gauge_memory_pools, _gaugePools, static_cast<int64_t>(_memoryPoolCount));
        _renderingDevice = parameter.rendering_device;
        _executionMode = parameter.execution_mode;
        _sessionId = sessionId;

        _workBufferMemory = std::move(workBufferMemory);

        Span<uint8_t>(_workBufferMemory).Clear();

        WorkBufferAllocator workBufferAllocator{Span<uint8_t>(_workBufferMemory)};

        PoolMapper poolMapper(processHandle, false);
        poolMapper.InitializeSystemPool(_dspMemoryPoolInfo, workBuffer, workBufferSize);

        _mixBuffer = workBufferAllocator.Allocate<float>(static_cast<uint64_t>(_sampleCount * (_voiceChannelCountMax + _mixBufferCount)), 0x10);

        if (_mixBuffer.IsEmpty()) {
            return ResultCode::WorkBufferTooSmall;
        }

        Span<float> upSamplerWorkBuffer = workBufferAllocator.Allocate<float>(
            static_cast<uint64_t>(Constants::TargetSampleCount) * (_voiceChannelCountMax + _mixBufferCount) * _upsamplerCount, 0x10);

        if (upSamplerWorkBuffer.IsEmpty()) {
            return ResultCode::WorkBufferTooSmall;
        }

        _depopBuffer = workBufferAllocator.Allocate<float>(BitUtils::AlignUp<uint64_t>(parameter.mix_buffer_count, Constants::BufferAlignment),
                                                           Constants::BufferAlignment);

        if (_depopBuffer.IsEmpty()) {
            return ResultCode::WorkBufferTooSmall;
        }

        Span<BiquadFilterState> splitterBqfStates;

        if (_behaviourInfo.IsBiquadFilterParameterForSplitterEnabled() && parameter.splitter_count > 0 && parameter.splitter_destination_count > 0) {
            splitterBqfStates = workBufferAllocator.Allocate<BiquadFilterState>(
                static_cast<uint64_t>(parameter.splitter_destination_count) * SplitterContext::BqfStatesPerDestination, 0x10);

            if (splitterBqfStates.IsEmpty()) {
                return ResultCode::WorkBufferTooSmall;
            }

            splitterBqfStates.Clear();
        }

        // C# AudioProcessorMemoryManager.InvalidateDspCache: no DSP cache in this port (guest memory is host memory).

        NXAUDREN_ASSERT((workBufferAllocator.Offset % Constants::BufferAlignment) == 0);

        Span<VoiceInfo> voices = workBufferAllocator.Allocate<VoiceInfo>(parameter.voice_count, VoiceInfo::Alignment);

        if (voices.IsEmpty()) {
            return ResultCode::WorkBufferTooSmall;
        }

        for (VoiceInfo& voice : voices) {
            voice.Initialize();
        }

        // A pain to handle as we can't have VoiceInfo*, use indices to be a bit more safe
        Span<int32_t> sortedVoices = workBufferAllocator.Allocate<int32_t>(parameter.voice_count, 0x10);

        if (sortedVoices.IsEmpty()) {
            return ResultCode::WorkBufferTooSmall;
        }

        // Clear memory (use -1 as it's an invalid index)
        sortedVoices.Fill(-1);

        Span<VoiceChannelResource> voiceChannelResources =
            workBufferAllocator.Allocate<VoiceChannelResource>(parameter.voice_count, VoiceChannelResource::Alignment);

        if (voiceChannelResources.IsEmpty()) {
            return ResultCode::WorkBufferTooSmall;
        }

        for (uint32_t id = 0; id < voiceChannelResources.Length; id++) {
            VoiceChannelResource& voiceChannelResource = voiceChannelResources[id];

            voiceChannelResource.Id = id;
            voiceChannelResource.IsUsed = false;
        }

        Span<VoiceState> voiceStates = workBufferAllocator.Allocate<VoiceState>(parameter.voice_count, VoiceState::Align);

        if (voiceStates.IsEmpty()) {
            return ResultCode::WorkBufferTooSmall;
        }

        uint32_t mixesCount = parameter.sub_mix_buffer_count + 1;

        Span<MixInfo> mixes = workBufferAllocator.Allocate<MixInfo>(mixesCount, MixInfo::Alignment);

        if (mixes.IsEmpty()) {
            return ResultCode::WorkBufferTooSmall;
        }

        if (parameter.effect_count == 0) {
            for (MixInfo& mix : mixes) {
                mix = MixInfo(Span<int32_t>(), _behaviourInfo);
            }
        } else {
            Span<int32_t> effectProcessingOrderArray = workBufferAllocator.Allocate<int32_t>(parameter.effect_count * mixesCount, 0x10);

            for (MixInfo& mix : mixes) {
                mix = MixInfo(effectProcessingOrderArray.Slice(0, parameter.effect_count), _behaviourInfo);

                effectProcessingOrderArray = effectProcessingOrderArray.Slice(parameter.effect_count);
            }
        }

        // Initialize the final mix id
        mixes[0].MixId = Constants::FinalMixId;

        Span<int32_t> sortedMixesInfo = workBufferAllocator.Allocate<int32_t>(mixesCount, 0x10);

        if (sortedMixesInfo.IsEmpty()) {
            return ResultCode::WorkBufferTooSmall;
        }

        // Clear memory (use -1 as it's an invalid index)
        sortedMixesInfo.Fill(-1);

        Span<uint8_t> nodeStatesWorkBuffer;
        Span<uint8_t> edgeMatrixWorkBuffer;

        if (_behaviourInfo.IsSplitterSupported()) {
            nodeStatesWorkBuffer = workBufferAllocator.Allocate(static_cast<uint64_t>(NodeStates::GetWorkBufferSize(static_cast<int32_t>(mixesCount))), 1);
            edgeMatrixWorkBuffer = workBufferAllocator.Allocate(static_cast<uint64_t>(EdgeMatrix::GetWorkBufferSize(static_cast<int32_t>(mixesCount))), 1);

            if (nodeStatesWorkBuffer.IsEmpty() || edgeMatrixWorkBuffer.IsEmpty()) {
                return ResultCode::WorkBufferTooSmall;
            }
        }

        _mixContext.Initialize(sortedMixesInfo, mixes, nodeStatesWorkBuffer, edgeMatrixWorkBuffer);

        _memoryPools = workBufferAllocator.Allocate<MemoryPoolInfo>(_memoryPoolCount, MemoryPoolInfo::Alignment);

        if (_memoryPools.IsEmpty()) {
            return ResultCode::WorkBufferTooSmall;
        }

        for (MemoryPoolInfo& info : _memoryPools) {
            info = MemoryPoolInfo::Create(MemoryPoolInfo::LocationType::Cpu);
        }

        if (!_splitterContext.Initialize(_behaviourInfo, parameter, workBufferAllocator, splitterBqfStates)) {
            return ResultCode::WorkBufferTooSmall;
        }

        _processHandle = processHandle;

        _upsamplerManager = std::make_unique<UpsamplerManager>(upSamplerWorkBuffer, _upsamplerCount);

        _effectContext.Initialize(parameter.effect_count, _behaviourInfo.IsEffectInfoVersion2Supported() ? parameter.effect_count : 0);
        _sinkContext.Initialize(parameter.sink_count);

        Span<VoiceState> voiceStatesDsp = workBufferAllocator.Allocate<VoiceState>(parameter.voice_count, VoiceState::Align);

        if (voiceStatesDsp.IsEmpty()) {
            return ResultCode::WorkBufferTooSmall;
        }

        _voiceContext.Initialize(sortedVoices, voices, voiceChannelResources, voiceStates, voiceStatesDsp, parameter.voice_count);

        if (parameter.performance_metric_frames_count > 0) {
            uint64_t performanceBufferSize =
                PerformanceManager::GetRequiredBufferSizeForPerformanceMetricsPerFrame(parameter, _behaviourInfo) *
                    (static_cast<uint64_t>(parameter.performance_metric_frames_count) + 1) +
                0xC;

            _performanceBuffer = workBufferAllocator.Allocate(performanceBufferSize, Constants::BufferAlignment);

            if (_performanceBuffer.IsEmpty()) {
                return ResultCode::WorkBufferTooSmall;
            }

            _performanceManager = PerformanceManager::Create(_performanceBuffer, parameter, _behaviourInfo);
        } else {
            _performanceManager = nullptr;
        }

        _totalElapsedTicksUpdating = 0;
        _totalElapsedTicks = 0;
        _renderingTimeLimitPercent = 100;
        _voiceDropEnabled = parameter.voice_drop_enabled && _executionMode == server::AudioRendererExecutionMode::Auto;

        // C# AudioProcessorMemoryManager.InvalidateDataCache: no DSP cache in this port.

        _processHandle = processHandle;
        _elapsedFrameCount = 0;
        _voiceDropParameter = 1.0f;

        switch (_behaviourInfo.GetCommandProcessingTimeEstimatorVersion()) {
            case 1:
                _commandProcessingTimeEstimator = std::make_unique<CommandProcessingTimeEstimatorVersion1>(_sampleCount, _mixBufferCount);
                break;
            case 2:
                _commandProcessingTimeEstimator = std::make_unique<CommandProcessingTimeEstimatorVersion2>(_sampleCount, _mixBufferCount);
                break;
            case 3:
                _commandProcessingTimeEstimator = std::make_unique<CommandProcessingTimeEstimatorVersion3>(_sampleCount, _mixBufferCount);
                break;
            case 4:
                _commandProcessingTimeEstimator = std::make_unique<CommandProcessingTimeEstimatorVersion4>(_sampleCount, _mixBufferCount);
                break;
            case 5:
                _commandProcessingTimeEstimator = std::make_unique<CommandProcessingTimeEstimatorVersion5>(_sampleCount, _mixBufferCount);
                break;
            default:
                NXAUDREN_FATAL("Unsupported processing time estimator version.");
        }

        return ResultCode::Success;
    }

    void Start() {
        NXLOG_I("AudioRenderer", "Starting renderer id %d", _sessionId);

        std::lock_guard<std::mutex> guard(_lock);
        _elapsedFrameCount = 0;
        _isActive = true;
    }

    void Stop() {
        NXLOG_I("AudioRenderer", "Stopping renderer id %d", _sessionId);

        {
            std::lock_guard<std::mutex> guard(_lock);
            _isActive = false;
        }

        NXLOG_I("AudioRenderer", "Stopped renderer id %d", _sessionId);
    }

    void Disable() {
        std::lock_guard<std::mutex> guard(_lock);
        _isActive = false;
    }

    // What the guest actually asks of the renderer (not in the C#): the most voices playing at once, the sample formats, rates and
    // channel counts of the voices, the effect types in use. One log line per ~30 s of updates and one when the renderer is destroyed;
    // it tells which parts of the port the game really exercises (docs/S9.md).
    struct UsageStats {
        uint64_t updates = 0;
        uint32_t max_active_voices = 0, max_active_effects = 0;
        uint64_t format_voice_updates[8] = {};            // by SampleFormat value: Invalid, PcmInt8, PcmInt16, PcmInt24, PcmInt32, PcmFloat, Adpcm
        uint64_t rate_counts[9] = {};                      // 8000, 11025, 16000, 22050, 24000, 32000, 44100, 48000, other
        uint32_t channel_mask = 0;                         // bit n set: a voice with n channels was playing
        uint32_t effect_type_mask = 0;                     // bit n set: an enabled effect of EffectType n
        uint64_t drop_flag_voices = 0;
    } _usage;

    void TrackUsage() {
        _usage.updates++;
        uint32_t active = 0;
        for (uint32_t i = 0; i < _voiceContext.GetCount(); i++) {
            VoiceInfo& v = _voiceContext.GetState(static_cast<int32_t>(i));
            if (!v.InUse || v.PlayState != server::PlayState::Started || v.WaveBuffersCount == 0) {
                continue;
            }
            active++;
            const uint32_t fmt = static_cast<uint32_t>(v.SampleFormat);
            if (fmt < 8) {
                _usage.format_voice_updates[fmt]++;
            }
            static const uint32_t kRates[8] = {8000, 11025, 16000, 22050, 24000, 32000, 44100, 48000};
            size_t ri = 8;
            for (size_t k = 0; k < 8; k++) {
                if (v.SampleRate == kRates[k]) {
                    ri = k;
                    break;
                }
            }
            _usage.rate_counts[ri]++;
            if (v.ChannelsCount < 32) {
                _usage.channel_mask |= 1u << v.ChannelsCount;
            }
            if (v.VoiceDropFlag) {
                _usage.drop_flag_voices++;
            }
        }
        uint32_t effects = 0;
        for (uint32_t i = 0; i < _effectContext.GetCount(); i++) {
            const std::unique_ptr<BaseEffect>& e = _effectContext.GetEffect(static_cast<int>(i));
            if (e != nullptr && e->IsEnabled) {
                effects++;
                _usage.effect_type_mask |= 1u << static_cast<uint32_t>(e->Type);
            }
        }
        _usage.max_active_voices = std::max(_usage.max_active_voices, active);
        _usage.max_active_effects = std::max(_usage.max_active_effects, effects);
        if (_usage.updates % 2000 == 0) {
            LogUsage();
        }
    }

    void LogUsage() const {
        char rates[160];
        int n = 0;
        static const char* kNames[9] = {"8k", "11.025k", "16k", "22.05k", "24k", "32k", "44.1k", "48k", "other"};
        rates[0] = 0;
        for (size_t k = 0; k < 9 && n < 140; k++) {
            if (_usage.rate_counts[k]) {
                n += std::snprintf(rates + n, sizeof rates - static_cast<size_t>(n), " %s=%llu", kNames[k], static_cast<unsigned long long>(_usage.rate_counts[k]));
            }
        }
        NXLOG_I("audren-use",
                "updates %llu: max playing voices %u (of %u), voice-updates by format pcm8=%llu pcm16=%llu pcm24/32=%llu float=%llu adpcm=%llu, by rate:%s, channel counts mask 0x%x, "
                "max enabled effects %u, effect type mask 0x%x (1 BufferMix 2 Aux 3 Delay 4 Reverb 5 Reverb3d 6 Biquad 7 Limiter 8 Capture 9 Compressor), dropped voices %llu",
                static_cast<unsigned long long>(_usage.updates), _usage.max_active_voices, _voiceContext.GetCount(),
                static_cast<unsigned long long>(_usage.format_voice_updates[1]), static_cast<unsigned long long>(_usage.format_voice_updates[2]),
                static_cast<unsigned long long>(_usage.format_voice_updates[3] + _usage.format_voice_updates[4]),
                static_cast<unsigned long long>(_usage.format_voice_updates[5]), static_cast<unsigned long long>(_usage.format_voice_updates[6]), rates, _usage.channel_mask, _usage.max_active_effects,
                _usage.effect_type_mask >> 1, static_cast<unsigned long long>(_usage.drop_flag_voices));
    }

    // C# Update(output, performanceOutput, input). Applies the user's request, in the C# order.
    ResultCode Update(Span<uint8_t> output, Span<uint8_t> performanceOutput, Span<const uint8_t> input) {
        std::lock_guard<std::mutex> guard(_lock);

        uint64_t updateStartTicks = GetSystemTicks();

        output.Clear();

        StateUpdater stateUpdater(input, output, _processHandle, &_behaviourInfo);

        ResultCode result;

        result = stateUpdater.UpdateBehaviourInfo();

        if (result != ResultCode::Success) {
            return result;
        }

        result = stateUpdater.UpdateMemoryPools(_memoryPools);

        if (result != ResultCode::Success) {
            return result;
        }

        result = stateUpdater.UpdateVoiceChannelResources(_voiceContext);

        if (result != ResultCode::Success) {
            return result;
        }

        PoolMapper poolMapper(_processHandle, _memoryPools, _behaviourInfo.IsMemoryPoolForceMappingEnabled());

        if (_behaviourInfo.IsBiquadFilterParameterFloatSupported()) {
            result = stateUpdater.UpdateVoices2(_voiceContext, poolMapper);
        } else {
            result = stateUpdater.UpdateVoices1(_voiceContext, poolMapper);
        }

        if (result != ResultCode::Success) {
            return result;
        }

        result = stateUpdater.UpdateEffects(_effectContext, _isActive, poolMapper);

        if (result != ResultCode::Success) {
            return result;
        }

        if (_behaviourInfo.IsSplitterSupported()) {
            result = stateUpdater.UpdateSplitter(_splitterContext);

            if (result != ResultCode::Success) {
                return result;
            }
        }

        result = stateUpdater.UpdateMixes(_mixContext, GetMixBufferCount(), _effectContext, _splitterContext);

        if (result != ResultCode::Success) {
            return result;
        }

        result = stateUpdater.UpdateSinks(_sinkContext, poolMapper);

        if (result != ResultCode::Success) {
            return result;
        }

        result = stateUpdater.UpdatePerformanceBuffer(_performanceManager.get(), performanceOutput);

        if (result != ResultCode::Success) {
            return result;
        }

        result = stateUpdater.UpdateErrorInfo();

        if (result != ResultCode::Success) {
            return result;
        }

        if (_behaviourInfo.IsElapsedFrameCountSupported()) {
            result = stateUpdater.UpdateRendererInfo(_elapsedFrameCount);

            if (result != ResultCode::Success) {
                return result;
            }
        }

        result = stateUpdater.CheckConsumedSize();

        if (result != ResultCode::Success) {
            return result;
        }

        if (_systemEvent != nullptr) {
            _systemEvent->Clear();
        }

        TrackUsage();

        uint64_t updateEndTicks = GetSystemTicks();

        _totalElapsedTicksUpdating += (updateEndTicks - updateStartTicks);

        return result;
    }

    // C# SendCommands: generates the frame's command list and queues it for the AudioProcessor. Defined in AudioRendererManager.h.
    void SendCommands();

    uint32_t GetMixBufferCount() const { return _mixBufferCount; }

    void SetRenderingTimeLimitPercent(uint32_t percent) {
        NXAUDREN_ASSERT(percent <= 100);

        _renderingTimeLimitPercent = percent;
    }

    uint32_t GetRenderingTimeLimit() const { return _renderingTimeLimitPercent; }

    Span<float> GetMixBuffer() const { return _mixBuffer; }

    uint32_t GetSampleCount() const { return _sampleCount; }

    uint32_t GetSampleRate() const { return _sampleRate; }

    uint32_t GetVoiceChannelCountMax() const { return _voiceChannelCountMax; }

    bool IsActive() const { return _isActive; }

    int32_t GetSessionId() const { return _sessionId; }

    // C# static GetWorkBufferSize: the arithmetic is written term by term, in the C# order.
    static uint64_t GetWorkBufferSize(AudioRendererConfiguration& parameter) {
        BehaviourInfo behaviourInfo;

        behaviourInfo.SetUserRevision(parameter.revision);

        uint32_t mixesCount = parameter.sub_mix_buffer_count + 1;

        uint32_t memoryPoolCount = parameter.effect_count + parameter.voice_count * static_cast<uint32_t>(Constants::VoiceWaveBufferCount);

        uint64_t size = 0;

        // Mix Buffers
        size = WorkBufferAllocator::GetTargetSize<float>(
            size, static_cast<uint64_t>(parameter.sample_count) * (static_cast<uint64_t>(Constants::VoiceChannelCountMax) + parameter.mix_buffer_count),
            0x10);

        // Upsampler workbuffer
        size = WorkBufferAllocator::GetTargetSize<float>(
            size,
            static_cast<uint64_t>(Constants::TargetSampleCount) * (static_cast<uint64_t>(Constants::VoiceChannelCountMax) + parameter.mix_buffer_count) *
                (static_cast<uint64_t>(parameter.sink_count) + parameter.sub_mix_buffer_count),
            0x10);

        // Depop buffer
        size = WorkBufferAllocator::GetTargetSize<float>(size, BitUtils::AlignUp<uint64_t>(parameter.mix_buffer_count, Constants::BufferAlignment),
                                                         Constants::BufferAlignment);

        // Voice
        size = WorkBufferAllocator::GetTargetSize<VoiceInfo>(size, parameter.voice_count, VoiceInfo::Alignment);
        size = WorkBufferAllocator::GetTargetSize<int32_t>(size, parameter.voice_count, 0x10);
        size = WorkBufferAllocator::GetTargetSize<VoiceChannelResource>(size, parameter.voice_count, VoiceChannelResource::Alignment);
        size = WorkBufferAllocator::GetTargetSize<VoiceState>(size, parameter.voice_count, VoiceState::Align);

        // Mix
        size = WorkBufferAllocator::GetTargetSize<MixInfo>(size, mixesCount, MixInfo::Alignment);
        size = WorkBufferAllocator::GetTargetSize<int32_t>(size, static_cast<uint64_t>(parameter.effect_count) * mixesCount, 0x10);
        size = WorkBufferAllocator::GetTargetSize<int32_t>(size, mixesCount, 0x10);

        if (behaviourInfo.IsSplitterSupported()) {
            size += static_cast<uint64_t>(BitUtils::AlignUp<int32_t>(
                NodeStates::GetWorkBufferSize(static_cast<int32_t>(mixesCount)) + EdgeMatrix::GetWorkBufferSize(static_cast<int32_t>(mixesCount)), 0x10));
        }

        // Memory Pool
        size = WorkBufferAllocator::GetTargetSize<MemoryPoolInfo>(size, memoryPoolCount, MemoryPoolInfo::Alignment);

        // Splitter
        size = SplitterContext::GetWorkBufferSize(size, behaviourInfo, parameter);

        if (behaviourInfo.IsBiquadFilterParameterForSplitterEnabled() && parameter.splitter_count > 0 && parameter.splitter_destination_count > 0) {
            size = WorkBufferAllocator::GetTargetSize<BiquadFilterState>(
                size, static_cast<uint64_t>(parameter.splitter_destination_count) * SplitterContext::BqfStatesPerDestination, 0x10);
        }

        // DSP Voice
        size = WorkBufferAllocator::GetTargetSize<VoiceState>(size, parameter.voice_count, VoiceState::Align);

        // Performance
        if (parameter.performance_metric_frames_count > 0) {
            uint64_t performanceMetricsPerFramesSize =
                PerformanceManager::GetRequiredBufferSizeForPerformanceMetricsPerFrame(parameter, behaviourInfo) *
                    (static_cast<uint64_t>(parameter.performance_metric_frames_count) + 1) +
                0xC;

            size += BitUtils::AlignUp<uint64_t>(performanceMetricsPerFramesSize, Constants::PerformanceMetricsPerFramesSizeAlignment);
        }

        return BitUtils::AlignUp<uint64_t>(size, Constants::WorkBufferAlignment);
    }

    ResultCode QuerySystemEvent(IWritableEvent*& systemEvent) {
        systemEvent = nullptr;

        if (_executionMode == server::AudioRendererExecutionMode::Manual) {
            return ResultCode::UnsupportedOperation;
        }

        systemEvent = _systemEvent;

        return ResultCode::Success;
    }

    // C# Dispose(): idempotent. Stops the session, unmaps its pools and unregisters it from the manager. Defined in AudioRendererManager.h.
    void Dispose() {
        if (_disposeState.exchange(1) == 0) {
            if (_usage.updates > 0) {
                LogUsage();
            }
            DisposeImpl();
        }
    }

    void SetVoiceDropParameter(float voiceDropParameter) { _voiceDropParameter = ClampT(voiceDropParameter, 0.0f, 4.0f); }

    float GetVoiceDropParameter() const { return _voiceDropParameter; }

    ResultCode ExecuteAudioRendererRendering() {
        if (_executionMode == server::AudioRendererExecutionMode::Manual && _renderingDevice == server::AudioRendererRenderingDevice::Cpu) {
            // NOTE: Here the platform aborts with this error code, we don't want that.
            return ResultCode::InvalidExecutionContextOperation;
        }

        return ResultCode::UnsupportedOperation;
    }
};

}  // namespace nxrt::audren

// The members that need the manager are defined in AudioRendererManager.h, which needs this header complete.
#include "AudioRendererManager.h"
