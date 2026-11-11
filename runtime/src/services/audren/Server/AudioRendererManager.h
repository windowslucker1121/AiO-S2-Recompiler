// SPDX-License-Identifier: GPL-3.0-or-later
// Port of Ryujinx.Audio/Renderer/Server/AudioRendererManager.cs (Ryubing fork, MIT): the session table of the audio renderer service and
// the worker thread that drives every open AudioRenderSystem through the AudioProcessor.
//
// Deviations from the C#, all forced by the port conventions or by C++ ownership:
//   * ITickSource is the struct TickSource below (steady_clock scaled to the 19.2 MHz guest tick).
//   * OpenAudioRenderer hands the renderer out as std::unique_ptr; the manager keeps raw pointers only (non-owning) in its table, as
//     the C# keeps references. The renderer unregisters itself when it is destroyed or disposed.
//   * IVirtualMemoryManager is dropped (guest memory is host memory). The work buffer is a zeroed std::vector of workBufferSize bytes.
//   * C# `Lock` (reentrant) is a std::mutex, so the C# nested `ReleaseSessionId` inside `lock (_sessionLock)` is a separate unlocked helper.
//   * C# `_isRunning` is read by the worker without the lock, so it is a std::atomic<bool>.
//   * Including this header first is fine: it includes AudioRenderSystem.h, which includes this header back at its end.
#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "../Constants.h"
#include "../Dsp/AudioProcessor.h"
#include "../Integration/IHardwareDeviceDriver.h"
#include "../Integration/IWritableEvent.h"
#include "../audren_types.h"
#include "AudioRenderSystem.h"
#include "nxrt/kernel.h"
#include "nxrt/log.h"

namespace nxrt::audren {

// C# ITickSource: a monotonic clock in guest ticks. Counter() is steady_clock nanoseconds scaled to 19.2 MHz: ns * 192 / 10000.
struct TickSource {
    uint64_t Counter() const {
        uint64_t ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());

        return ns * 192 / 10000;
    }

    uint64_t Frequency() const { return nxrt::kernel::tick_hz(); }   // the console tick (Constants::TargetTimerFrequency on platform 1)
};

class AudioRendererManager {
  private:
    // Lock used for session allocation (C# _sessionLock). Not reentrant: the *Locked helpers below expect it to be held.
    std::mutex _sessionLock;

    // Lock used to control the AudioProcessor running state (C# _audioProcessorLock).
    std::mutex _audioProcessorLock;

    // The session ids allocation table (C# int[] _sessionIds).
    std::vector<int32_t> _sessionIds;

    // The events linked to each session (C# IWritableEvent[] _sessionsSystemEvent). Owned by the caller.
    std::vector<IWritableEvent*> _sessionsSystemEvent;

    // The AudioRenderSystem sessions instances (C# AudioRenderSystem[]). Non-owning.
    std::vector<AudioRenderSystem*> _sessions;

    // The count of active sessions.
    int32_t _activeSessionCount = 0;

    // The worker thread used to run SendCommands.
    std::thread _workerThread;

    // Indicate if the worker thread and AudioProcessor are running.
    std::atomic<bool> _isRunning{false};

    // The audio device driver to create audio outputs.
    IHardwareDeviceDriver* _deviceDriver = nullptr;

    // The dispose state (C# _disposeState).
    std::atomic<int32_t> _disposeState{0};

  public:
    // Tick source used to measure elapsed time (C# TickSource { get; }).
    nxrt::audren::TickSource TickSource;

    // The AudioProcessor instance associated to this manager (C# Processor { get; }).
    AudioProcessor Processor;

    explicit AudioRendererManager(nxrt::audren::TickSource tickSource = {}) : TickSource(tickSource) {
        _sessionIds.resize(static_cast<size_t>(Constants::AudioRendererSessionCountMax));
        _sessions.assign(static_cast<size_t>(Constants::AudioRendererSessionCountMax), nullptr);
        _activeSessionCount = 0;

        for (size_t i = 0; i < _sessionIds.size(); i++) {
            _sessionIds[i] = static_cast<int32_t>(i);
        }
    }

    AudioRendererManager(const AudioRendererManager&) = delete;
    AudioRendererManager& operator=(const AudioRendererManager&) = delete;

    ~AudioRendererManager() { Dispose(); }

    // Initialize the manager (C# Initialize). `sessionSystemEvents` has one event per session id; the events are owned by the caller.
    void Initialize(std::vector<IWritableEvent*> sessionSystemEvents, IHardwareDeviceDriver* deviceDriver) {
        _sessionsSystemEvent = std::move(sessionSystemEvents);
        _deviceDriver = deviceDriver;
    }

    // The work buffer size required by a session (C# static GetWorkBufferSize).
    static uint64_t GetWorkBufferSize(AudioRendererConfiguration& parameter) { return AudioRenderSystem::GetWorkBufferSize(parameter); }

    // Stop sending commands to the AudioProcessor without stopping the worker thread. C# StopSendingCommands.
    void StopSendingCommands() {
        {
            std::lock_guard<std::mutex> guard(_sessionLock);

            for (AudioRenderSystem* renderer : _sessions) {
                if (renderer != nullptr) {
                    renderer->Disable();
                }
            }
        }

        {
            std::lock_guard<std::mutex> guard(_audioProcessorLock);

            if (_isRunning.load()) {
                StopLocked();
            }
        }
    }

    // Open a new AudioRenderSystem (C# OpenAudioRenderer). The renderer is returned through `renderer` (null on failure).
    // `workBufferAddress` is the guest address of the work buffer; `workBufferSize` bytes are allocated for the renderer.
    ResultCode OpenAudioRenderer(std::unique_ptr<AudioRenderSystem>& renderer, AudioRendererConfiguration& parameter, uint64_t appletResourceUserId,
                                 uint64_t workBufferAddress, uint64_t workBufferSize, uint32_t processHandle) {
        int32_t sessionId = AcquireSessionId();

        auto audioRenderer = std::make_unique<AudioRenderSystem>(*this, _sessionsSystemEvent[static_cast<size_t>(sessionId)]);

        // TODO (C#): Eventually, we should try to use the guest supplied work buffer instead of allocating our own.
        std::vector<uint8_t> workBufferMemory(static_cast<size_t>(workBufferSize), 0);

        ResultCode result = audioRenderer->Initialize(parameter, processHandle, std::move(workBufferMemory), workBufferAddress, workBufferSize,
                                                      sessionId, appletResourceUserId);

        if (result == ResultCode::Success) {
            renderer = std::move(audioRenderer);

            Register(renderer.get());
        } else {
            // The session id is released by the renderer's own Unregister when it is destroyed here (C# releases it by hand).
            audioRenderer.reset();

            renderer.reset();
        }

        return result;
    }

    // C# Dispose: disposes every open session, stops the worker and the processor.
    void Dispose() {
        if (_disposeState.exchange(1) == 0) {
            DisposeImpl();
        }
    }

  private:
    void DisposeImpl() {
        // Copy the sessions table and dispose the sessions outside the lock (each one unregisters itself).
        std::vector<AudioRenderSystem*> sessions;

        {
            std::lock_guard<std::mutex> guard(_sessionLock);
            sessions = _sessions;
        }

        for (AudioRenderSystem* renderer : sessions) {
            if (renderer != nullptr) {
                renderer->Dispose();
            }
        }

        {
            std::lock_guard<std::mutex> guard(_audioProcessorLock);

            if (_isRunning.load()) {
                StopLocked();
            }
        }
    }

    // C# AcquireSessionId.
    int32_t AcquireSessionId() {
        std::lock_guard<std::mutex> guard(_sessionLock);

        int32_t index = _activeSessionCount;

        NXAUDREN_ASSERT(static_cast<size_t>(index) < _sessionIds.size());

        int32_t sessionId = _sessionIds[static_cast<size_t>(index)];

        _sessionIds[static_cast<size_t>(index)] = -1;

        _activeSessionCount++;

        NXLOG_I("AudioRenderer", "Registered new renderer (%d)", sessionId);

        return sessionId;
    }

    // C# ReleaseSessionId(int) without the lock: the caller holds _sessionLock.
    void ReleaseSessionIdLocked(int32_t sessionId) {
        NXAUDREN_ASSERT(_activeSessionCount > 0);

        int32_t newIndex = --_activeSessionCount;

        _sessionIds[static_cast<size_t>(newIndex)] = sessionId;
    }

    // C# HasAnyActiveRendererLocked: the caller holds _sessionLock.
    bool HasAnyActiveRendererLocked() const {
        for (AudioRenderSystem* renderer : _sessions) {
            if (renderer != nullptr) {
                return true;
            }
        }

        return false;
    }

    // C# StartLocked: the caller holds _audioProcessorLock.
    void StartLocked() {
        _isRunning.store(true);

        // TODO (C#): virtual device mapping (IAudioDevice)
        Processor.Start(_deviceDriver);

        _workerThread = std::thread([this] { SendCommandsWorker(); });
    }

    // C# StopLocked: the caller holds _audioProcessorLock.
    void StopLocked() {
        _isRunning.store(false);

        _workerThread.join();
        Processor.Stop();

        NXLOG_I("AudioRenderer", "Stopped audio renderer");
    }

    // C# SendCommands (the worker main function): dispatches every session's commands to the AudioProcessor, one frame at a time.
    void SendCommandsWorker() {
        BoostAudioThread();
        TimerResolutionScope::Acquire();
        NXLOG_I("AudioRenderer", "Starting audio renderer");
        Processor.Wait();

        int stallLogs = 0;

        while (_isRunning.load()) {
            const auto frameStart = std::chrono::steady_clock::now();

            {
                std::lock_guard<std::mutex> guard(_sessionLock);

                for (AudioRenderSystem* renderer : _sessions) {
                    if (renderer != nullptr) {
                        renderer->SendCommands();
                    }
                }
            }

            const auto generated = std::chrono::steady_clock::now();

            Processor.Signal();
            Processor.Wait();

            // Diagnostics, not in the C#: a frame that took long to generate (waiting for the renderer's lock, which the guest's update holds)
            // or to render and pace is where a sound card would starve.
            const auto frameEnd = std::chrono::steady_clock::now();
            const double generateMs = std::chrono::duration<double, std::milli>(generated - frameStart).count();
            const double restMs = std::chrono::duration<double, std::milli>(frameEnd - generated).count();

            if ((generateMs > 15.0 || restMs > 60.0) && stallLogs < 40) {
                stallLogs++;
                NXLOG_W("AudioRenderer", "slow frame: generating the commands took %.1f ms, rendering and waiting %.1f ms", generateMs, restMs);
            }
        }

        TimerResolutionScope::Release();
    }

    // C# Register: adds the session to the table and starts the processor if needed.
    void Register(AudioRenderSystem* renderer) {
        {
            std::lock_guard<std::mutex> guard(_sessionLock);

            _sessions[static_cast<size_t>(renderer->GetSessionId())] = renderer;
        }

        {
            std::lock_guard<std::mutex> guard(_audioProcessorLock);

            if (!_isRunning.load()) {
                StartLocked();
            }
        }
    }

    // C# internal Unregister: removes the session, releases its id and stops the processor when no session is left.
    void Unregister(AudioRenderSystem* renderer) {
        {
            std::lock_guard<std::mutex> guard(_sessionLock);

            int32_t sessionId = renderer->GetSessionId();

            _sessions[static_cast<size_t>(renderer->GetSessionId())] = nullptr;

            // The renderer's memory is about to go away: its queued CommandList must not be processed after that.
            Processor.DiscardSession(sessionId);

            ReleaseSessionIdLocked(sessionId);

            NXLOG_I("AudioRenderer", "Unregistered renderer (%d)", sessionId);
        }

        {
            std::lock_guard<std::mutex> guard(_audioProcessorLock);

            if (_isRunning.load() && !HasAnyActiveRendererLocked()) {
                StopLocked();
            }
        }
    }

    // Lets AudioRenderSystem::DisposeImpl and the renderer's destructor reach the private Unregister.
    friend class AudioRenderSystem;
};

// ---- Members of AudioRenderSystem that need the manager (declared in AudioRenderSystem.h) ----

inline uint64_t AudioRenderSystem::GetSystemTicks() { return _manager.TickSource.Counter(); }

inline void AudioRenderSystem::SendCommands() {
    std::lock_guard<std::mutex> guard(_lock);

    if (_isActive) {
        if (!_manager.Processor.HasRemainingCommands(_sessionId)) {
            std::unique_ptr<CommandList> commands = GenerateCommandList();

            _manager.Processor.Send(_sessionId, std::move(commands), GetMaxAllocatedTimeForDsp(), _appletResourceId);

            if (_systemEvent != nullptr) {
                _systemEvent->Signal();
            }
        } else {
            _isDspRunningBehind = true;
        }
    }
}

inline RendererSystemContext AudioRenderSystem::GetContext() {
    RendererSystemContext context;

    context.ChannelCount = _manager.Processor.OutputDevices[static_cast<size_t>(_sessionId)]->GetChannelCount();
    context.BehaviourInfo = &_behaviourInfo;
    context.DepopBuffer = _depopBuffer;
    context.MixBufferCount = GetMixBufferCount();
    context.SessionId = _sessionId;
    context.UpsamplerManager = _upsamplerManager.get();

    return context;
}

inline void AudioRenderSystem::DisposeImpl() {
    if (_isActive) {
        Stop();
    }

    PoolMapper mapper(_processHandle, false);
    mapper.Unmap(_dspMemoryPoolInfo);

    PoolMapper::ClearUsageState(_memoryPools);

    for (MemoryPoolInfo& memoryPool : _memoryPools) {
        if (memoryPool.IsMapped()) {
            mapper.Unmap(memoryPool);
        }
    }

    _manager.Unregister(this);

    // C# _workBufferMemoryPin.Dispose() and MemoryManager release: the work buffer vector is freed with the renderer.
}

inline AudioRenderSystem::~AudioRenderSystem() {
    Dispose();
    g_gauge_voices_in_use.fetch_sub(_gaugeVoices, std::memory_order_relaxed);
    g_gauge_wavebuffers_queued.fetch_sub(_gaugeWaves, std::memory_order_relaxed);
    g_gauge_memory_pools.fetch_sub(_gaugePools, std::memory_order_relaxed);
}

}  // namespace nxrt::audren
