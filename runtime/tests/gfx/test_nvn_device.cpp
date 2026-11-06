// SPDX-License-Identifier: GPL-3.0-or-later
// Device, queue, sync, event and window entry points (api_device.cpp) through the typed thunks, called the way a game calls them.
// The command buffer procs (api_cmdbuf.cpp) record the lists that the queue submits; the builders (api_builders.cpp) fill the
// builder objects. The GPU is the stub (nvn_gpu_stub.cpp): submit advances the serial, every wait is already satisfied.
#include <cstring>

#include "nvn_cmd.h"
#include "nvn_core.h"
#include "nvn_gpu.h"
#include "nvn_impl.h"
#include "test_framework.h"
#include "test_platform.h"
NXTEST_GROUP(gfx_nvn_device)

using namespace nxrt;
using namespace nxrt::nvn;

namespace {

// Every name that register_api_device registers.
const char* const kNames[] = {
    // device
    "nvnDeviceInitialize", "nvnDeviceFinalize", "nvnDeviceSetDebugLabel", "nvnDeviceGetInteger",
    "nvnDeviceGetCurrentTimestampInNanoseconds", "nvnDeviceSetIntermediateShaderCache", "nvnDeviceGetTextureHandle",
    "nvnDeviceGetTexelFetchHandle", "nvnDeviceGetImageHandle", "nvnDeviceInstallDebugCallback",
    "nvnDeviceGenerateDebugDomainId", "nvnDeviceSetWindowOriginMode", "nvnDeviceSetDepthMode",
    "nvnDeviceRegisterFastClearColor", "nvnDeviceRegisterFastClearColori", "nvnDeviceRegisterFastClearColorui",
    "nvnDeviceRegisterFastClearDepth", "nvnDeviceGetWindowOriginMode", "nvnDeviceGetDepthMode",
    "nvnDeviceGetTimestampInNanoseconds", "nvnDeviceApplyDeferredFinalizes", "nvnDeviceFinalizeCommandHandle",
    "nvnDeviceWalkDebugDatabase", "nvnDeviceGetSeparateTextureHandle", "nvnDeviceGetSeparateSamplerHandle",
    "nvnDeviceIsExternalDebuggerAttached",
    // queue
    "nvnQueueInitialize", "nvnQueueFinalize", "nvnQueueSetDebugLabel", "nvnQueueSubmitCommands", "nvnQueueFlush",
    "nvnQueueFinish", "nvnQueuePresentTexture", "nvnQueueAcquireTexture", "nvnQueueGetError",
    "nvnQueueGetTotalCommandMemoryUsed", "nvnQueueGetTotalControlMemoryUsed", "nvnQueueGetTotalComputeMemoryUsed",
    "nvnQueueResetMemoryUsageCounts", "nvnQueueFenceSync", "nvnQueueWaitSync",
    // sync
    "nvnSyncInitialize", "nvnSyncFinalize", "nvnSyncSetDebugLabel", "nvnSyncWait",
    // event
    "nvnEventInitialize", "nvnEventFinalize", "nvnEventGetValue", "nvnEventSignal", "nvnEventGetMemoryOffset",
    // window
    "nvnWindowInitialize", "nvnWindowFinalize", "nvnWindowSetDebugLabel", "nvnWindowAcquireTexture",
    "nvnWindowGetNativeWindow", "nvnWindowGetPresentInterval", "nvnWindowSetPresentInterval", "nvnWindowSetCrop",
    "nvnWindowGetCrop", "nvnWindowGetNumTextures", "nvnWindowGetNumActiveTextures", "nvnWindowSetNumActiveTextures",
    // loader hooks
    "nvnLoadCProcs", "nvnSetExternalDebuggerEnabledNVX",
};

void setup() {
    install();
    abi::reset_mismatches();
    register_api_builders();
    register_api_cmdbuf();
    register_api_device();
    CHECK(abi::mismatches() == 0u);
}

// A typed call through abi::proc: P<R(A...)>("name") is the guest entry point of that name.
template <class Sig>
auto P(const char* name) {
    return abi::proc<Sig*>(name);
}

void init_device(NVNdevice* dev, int flags) {
    NVNdeviceBuilder b{};
    P<void(NVNdeviceBuilder*)>("nvnDeviceBuilderSetDefaults")(&b);
    P<void(NVNdeviceBuilder*, int)>("nvnDeviceBuilderSetFlags")(&b, flags);
    const NVNboolean ok = P<NVNboolean(NVNdevice*, const NVNdeviceBuilder*)>("nvnDeviceInitialize")(dev, &b);
    CHECK(ok == 1);
}

void init_queue(NVNqueue* q, NVNdevice* dev) {
    NVNqueueBuilder qb{};
    P<void(NVNqueueBuilder*)>("nvnQueueBuilderSetDefaults")(&qb);
    P<void(NVNqueueBuilder*, NVNdevice*)>("nvnQueueBuilderSetDevice")(&qb, dev);
    const NVNboolean ok = P<NVNboolean(NVNqueue*, const NVNqueueBuilder*)>("nvnQueueInitialize")(q, &qb);
    CHECK(ok == 1);
}

// Records an empty command list through the command buffer procs and returns its handle.
NVNcommandHandle record_empty_list(NVNcommandBuffer* cb, NVNdevice* dev) {
    P<NVNboolean(NVNcommandBuffer*, NVNdevice*)>("nvnCommandBufferInitialize")(cb, dev);
    P<void(NVNcommandBuffer*)>("nvnCommandBufferBeginRecording")(cb);
    return P<NVNcommandHandle(NVNcommandBuffer*)>("nvnCommandBufferEndRecording")(cb);
}

uint32_t peek32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}
void poke32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, 4); }

}  // namespace

NXTEST(all_registered_names_are_implemented) {
    setup();
    for (const char* name : kNames) CHECK(is_implemented(name));
    CHECK(abi::mismatches() == 0u);
}

NXTEST(device_initialize_binds_and_finalize_unbinds) {
    setup();
    NVNdevice dev{};
    init_device(&dev, 0x5);
    be::Device* d = be::host_of<be::Device>(&dev);
    CHECK(d != nullptr);
    CHECK(d->flags == 0x5);
    CHECK(d->window_origin == NVN_WINDOW_ORIGIN_MODE_LOWER_LEFT);
    CHECK(d->depth_mode == NVN_DEPTH_MODE_NEAR_IS_MINUS_W);
    P<void(NVNdevice*)>("nvnDeviceFinalize")(&dev);
    CHECK(be::host_of<be::Device>(&dev) == nullptr);
}

// NVN1 (Switch 1): API 55.15 and the NVN1 table. The test game may be a Switch 2 title (oot), so the console is named; NVN2 is checked below.
NXTEST(device_get_integer_known_and_unknown_values) {
    nxtest::ScopedPlatform sw1(nxrt::game::Platform::Switch1);
    setup();
    NVNdevice dev{};
    init_device(&dev, 0);
    auto get = P<void(const NVNdevice*, NVNdeviceInfo, int*)>("nvnDeviceGetInteger");
    int v = -1;
    get(&dev, NVN_DEVICE_INFO_UNIFORM_BUFFER_ALIGNMENT, &v);
    CHECK(v == 256);
    get(&dev, NVN_DEVICE_INFO_MAX_TEXTURE_SIZE, &v);
    CHECK(v == 16384);
    get(&dev, NVN_DEVICE_INFO_API_MAJOR_VERSION, &v);
    CHECK(v == 55);
    get(&dev, NVN_DEVICE_INFO_API_MINOR_VERSION, &v);
    CHECK(v == 15);
    get(&dev, NVN_DEVICE_INFO_TEXTURE_BINDINGS_PER_STAGE, &v);
    CHECK(v == 32);
    get(&dev, NVN_DEVICE_INFO_MAX_TEXTURE_LAYERS, &v);
    CHECK(v == 2048);
    // a value past the end of the table is written as 0
    v = 77;
    get(&dev, static_cast<NVNdeviceInfo>(0x62), &v);
    CHECK(v == 0);
    // a null out pointer is tolerated
    get(&dev, NVN_DEVICE_INFO_UNIFORM_BUFFER_ALIGNMENT, nullptr);
    P<void(NVNdevice*)>("nvnDeviceFinalize")(&dev);
}

// NVN2 (Switch 2): API 231.40 (Ocarina of Time, SDK 22, needs minor >= 40; Pokopia >= 30), the NVN2-only values 0x62..0xA9 from the SDK 22
// constant table (0x67 = 256: the movie enhancer aborts with 0), 0 past the table; the values NVN2 shares with NVN1 keep their numbering.
NXTEST(device_get_integer_nvn2_values) {
    nxtest::ScopedPlatform sw2(nxrt::game::Platform::Switch2);
    setup();
    NVNdevice dev{};
    init_device(&dev, 0);
    auto get = P<void(const NVNdevice*, NVNdeviceInfo, int*)>("nvnDeviceGetInteger");
    int v = -1;
    get(&dev, NVN_DEVICE_INFO_API_MAJOR_VERSION, &v);
    CHECK_EQ(v, 231);
    get(&dev, NVN_DEVICE_INFO_API_MINOR_VERSION, &v);
    CHECK_EQ(v, 40);
    get(&dev, NVN_DEVICE_INFO_UNIFORM_BUFFER_ALIGNMENT, &v);
    CHECK_EQ(v, 256);
    get(&dev, static_cast<NVNdeviceInfo>(0x62), &v);
    CHECK_EQ(v, 32);
    get(&dev, static_cast<NVNdeviceInfo>(0x67), &v);
    CHECK_EQ(v, 256);
    get(&dev, static_cast<NVNdeviceInfo>(0xA9), &v);
    CHECK_EQ(v, 1);
    v = 77;
    get(&dev, static_cast<NVNdeviceInfo>(0xAA), &v);
    CHECK_EQ(v, 0);
    P<void(NVNdevice*)>("nvnDeviceFinalize")(&dev);
}

NXTEST(device_window_origin_and_depth_mode_round_trip) {
    setup();
    NVNdevice dev{};
    init_device(&dev, 0);
    auto set_origin = P<void(NVNdevice*, NVNwindowOriginMode)>("nvnDeviceSetWindowOriginMode");
    auto get_origin = P<NVNwindowOriginMode(const NVNdevice*)>("nvnDeviceGetWindowOriginMode");
    auto set_depth = P<void(NVNdevice*, NVNdepthMode)>("nvnDeviceSetDepthMode");
    auto get_depth = P<NVNdepthMode(const NVNdevice*)>("nvnDeviceGetDepthMode");

    CHECK(static_cast<int>(get_origin(&dev)) == NVN_WINDOW_ORIGIN_MODE_LOWER_LEFT);
    CHECK(static_cast<int>(get_depth(&dev)) == NVN_DEPTH_MODE_NEAR_IS_MINUS_W);
    set_origin(&dev, NVN_WINDOW_ORIGIN_MODE_UPPER_LEFT);
    CHECK(static_cast<int>(get_origin(&dev)) == NVN_WINDOW_ORIGIN_MODE_UPPER_LEFT);
    CHECK(static_cast<int>(get_depth(&dev)) == NVN_DEPTH_MODE_NEAR_IS_MINUS_W);   // untouched
    set_depth(&dev, NVN_DEPTH_MODE_NEAR_IS_ZERO);
    CHECK(static_cast<int>(get_depth(&dev)) == NVN_DEPTH_MODE_NEAR_IS_ZERO);
    CHECK(static_cast<int>(get_origin(&dev)) == NVN_WINDOW_ORIGIN_MODE_UPPER_LEFT);   // untouched
    set_origin(&dev, NVN_WINDOW_ORIGIN_MODE_LOWER_LEFT);
    CHECK(static_cast<int>(get_origin(&dev)) == NVN_WINDOW_ORIGIN_MODE_LOWER_LEFT);
    set_depth(&dev, NVN_DEPTH_MODE_NEAR_IS_MINUS_W);
    CHECK(static_cast<int>(get_depth(&dev)) == NVN_DEPTH_MODE_NEAR_IS_MINUS_W);
    P<void(NVNdevice*)>("nvnDeviceFinalize")(&dev);
}

NXTEST(device_handles_timestamps_and_debug_domains) {
    setup();
    NVNdevice dev{};
    init_device(&dev, 0);

    const NVNtextureHandle th = P<NVNtextureHandle(const NVNdevice*, int, int)>("nvnDeviceGetTextureHandle")(&dev, 5, 7);
    CHECK(th == be::make_texture_handle(5, 7));
    CHECK(be::handle_texture_id(th) == 5u);
    CHECK(be::handle_sampler_id(th) == 7u);

    const NVNtextureHandle tf = P<NVNtextureHandle(const NVNdevice*, int)>("nvnDeviceGetTexelFetchHandle")(&dev, 9);
    CHECK(be::handle_texture_id(tf) == 9u);
    CHECK(be::handle_sampler_id(tf) == 0u);

    CHECK(P<NVNimageHandle(const NVNdevice*, int)>("nvnDeviceGetImageHandle")(&dev, 9) == 9u);

    const NVNseparateTextureHandle st = P<NVNseparateTextureHandle(const NVNdevice*, int)>("nvnDeviceGetSeparateTextureHandle")(&dev, 11);
    CHECK(st.value == 11u);
    const NVNseparateSamplerHandle ss = P<NVNseparateSamplerHandle(const NVNdevice*, int)>("nvnDeviceGetSeparateSamplerHandle")(&dev, 13);
    CHECK(ss.value == 13u);

    const uint64_t t0 = P<uint64_t(const NVNdevice*)>("nvnDeviceGetCurrentTimestampInNanoseconds")(&dev);
    const uint64_t t1 = P<uint64_t(const NVNdevice*)>("nvnDeviceGetCurrentTimestampInNanoseconds")(&dev);
    CHECK(t0 > 0u);
    CHECK(t1 >= t0);

    NVNcounterData counter{};
    counter.counter = 3;
    counter.timestamp = 12345;
    CHECK(P<uint64_t(const NVNdevice*, const NVNcounterData*)>("nvnDeviceGetTimestampInNanoseconds")(&dev, &counter) == 12345u);
    CHECK(P<uint64_t(const NVNdevice*, const NVNcounterData*)>("nvnDeviceGetTimestampInNanoseconds")(&dev, nullptr) == 0u);

    auto domain = P<NVNdebugDomainId(const NVNdevice*, const char*)>("nvnDeviceGenerateDebugDomainId");
    const NVNdebugDomainId d1 = domain(&dev, "a");
    const NVNdebugDomainId d2 = domain(&dev, "b");
    CHECK(d1 != 0);
    CHECK(d2 == d1 + 1);

    CHECK(P<NVNboolean(const NVNdevice*)>("nvnDeviceIsExternalDebuggerAttached")(&dev) == 0);
    P<NVNboolean(NVNdevice*, float)>("nvnDeviceRegisterFastClearDepth")(&dev, 1.0f);
    // fast-clear registry (nvn_small.h; the details are in test_nvn_small.cpp): real values are accepted, a null value is refused
    const float fc[4] = {0, 0, 0, 1};
    const int fci[4] = {1, 2, 3, 4};
    const uint32_t fcu[4] = {1, 2, 3, 4};
    CHECK(P<NVNboolean(NVNdevice*, const float*, NVNformat)>("nvnDeviceRegisterFastClearColor")(&dev, fc, NVN_FORMAT_RGBA8) == 1);
    CHECK(P<NVNboolean(NVNdevice*, const int*, NVNformat)>("nvnDeviceRegisterFastClearColori")(&dev, fci, NVN_FORMAT_RGBA8I) == 1);
    CHECK(P<NVNboolean(NVNdevice*, const uint32_t*, NVNformat)>("nvnDeviceRegisterFastClearColorui")(&dev, fcu, NVN_FORMAT_RGBA8I) == 1);
    CHECK(P<NVNboolean(NVNdevice*, const float*, NVNformat)>("nvnDeviceRegisterFastClearColor")(&dev, nullptr, NVN_FORMAT_RGBA8) == 0);

    P<void(NVNdevice*, const char*)>("nvnDeviceSetDebugLabel")(&dev, "device under test");
    P<void(NVNdevice*, int)>("nvnDeviceSetIntermediateShaderCache")(&dev, 1);
    P<void(NVNdevice*, PFNNVNDEBUGCALLBACKPROC, void*, NVNboolean)>("nvnDeviceInstallDebugCallback")(&dev, nullptr, nullptr, 0);
    P<void(const NVNdevice*, NVNdebugObjectType, PFNNVNWALKDEBUGDATABASECALLBACKPROC, void*)>("nvnDeviceWalkDebugDatabase")(&dev, static_cast<NVNdebugObjectType>(0), nullptr, nullptr);
    P<void(NVNdevice*, int)>("nvnDeviceApplyDeferredFinalizes")(&dev, 0);
    P<void(NVNdevice*)>("nvnDeviceFinalize")(&dev);
}

NXTEST(device_finalize_command_handle_releases_the_list) {
    setup();
    NVNdevice dev{};
    init_device(&dev, 0);
    NVNcommandBuffer cb{};
    const NVNcommandHandle h = record_empty_list(&cb, &dev);
    CHECK(h != 0);
    {
        be::Ref<be::CmdList> list = be::handle_lookup(h);
        CHECK(static_cast<bool>(list));
    }
    P<void(NVNdevice*, NVNcommandHandle)>("nvnDeviceFinalizeCommandHandle")(&dev, h);
    {
        be::Ref<be::CmdList> list = be::handle_lookup(h);
        CHECK(!static_cast<bool>(list));
    }
    P<void(NVNcommandBuffer*)>("nvnCommandBufferFinalize")(&cb);
    P<void(NVNdevice*)>("nvnDeviceFinalize")(&dev);
}

NXTEST(queue_initialize_submit_flush_and_counters) {
    setup();
    NVNdevice dev{};
    init_device(&dev, 0);
    NVNqueueBuilder qb{};
    P<void(NVNqueueBuilder*)>("nvnQueueBuilderSetDefaults")(&qb);
    P<void(NVNqueueBuilder*, NVNdevice*)>("nvnQueueBuilderSetDevice")(&qb, &dev);
    P<void(NVNqueueBuilder*, int)>("nvnQueueBuilderSetFlags")(&qb, 3);
    P<void(NVNqueueBuilder*, size_t)>("nvnQueueBuilderSetCommandMemorySize")(&qb, 65536);
    NVNqueue q{};
    CHECK(P<NVNboolean(NVNqueue*, const NVNqueueBuilder*)>("nvnQueueInitialize")(&q, &qb) == 1);
    be::Queue* hq = be::host_of<be::Queue>(&q);
    CHECK(hq != nullptr);
    CHECK(hq->device == be::host_of<be::Device>(&dev));
    CHECK(hq->flags == 3);
    CHECK(hq->command_memory == 65536u);

    NVNcommandBuffer cb{};
    const NVNcommandHandle h = record_empty_list(&cb, &dev);
    CHECK(h != 0);
    const uint64_t before = be::gpu::last_serial();
    P<void(NVNqueue*, int, const NVNcommandHandle*)>("nvnQueueSubmitCommands")(&q, 1, &h);
    CHECK(be::gpu::last_serial() > before);

    // an unknown handle is skipped and counted as missing; the submit itself still goes to the GPU layer
    const uint64_t missing = be::handles_missing();
    const NVNcommandHandle bogus = 0xdeadbeef0ull;
    P<void(NVNqueue*, int, const NVNcommandHandle*)>("nvnQueueSubmitCommands")(&q, 1, &bogus);
    CHECK(be::handles_missing() == missing + 1);

    P<void(NVNqueue*)>("nvnQueueFlush")(&q);
    P<void(NVNqueue*)>("nvnQueueFinish")(&q);
    P<void(NVNqueue*, const char*)>("nvnQueueSetDebugLabel")(&q, "queue under test");

    CHECK(static_cast<int>(P<NVNqueueGetErrorResult(NVNqueue*, NVNqueueErrorInfo*)>("nvnQueueGetError")(&q, nullptr)) ==
          NVN_QUEUE_GET_ERROR_RESULT_GPU_NO_ERROR);
    CHECK(P<size_t(NVNqueue*)>("nvnQueueGetTotalCommandMemoryUsed")(&q) == 0u);
    CHECK(P<size_t(NVNqueue*)>("nvnQueueGetTotalControlMemoryUsed")(&q) == 0u);
    CHECK(P<size_t(NVNqueue*)>("nvnQueueGetTotalComputeMemoryUsed")(&q) == 0u);
    P<void(NVNqueue*)>("nvnQueueResetMemoryUsageCounts")(&q);

    // a queue that is not ours submits nothing
    NVNqueue zero{};
    const uint64_t serial = be::gpu::last_serial();
    P<void(NVNqueue*, int, const NVNcommandHandle*)>("nvnQueueSubmitCommands")(&zero, 1, &h);
    CHECK(be::gpu::last_serial() == serial);

    P<void(NVNqueue*)>("nvnQueueFinalize")(&q);
    CHECK(be::host_of<be::Queue>(&q) == nullptr);
    P<void(NVNcommandBuffer*)>("nvnCommandBufferFinalize")(&cb);
    P<void(NVNdevice*)>("nvnDeviceFinalize")(&dev);
}

NXTEST(sync_arm_by_queue_fence_and_wait) {
    setup();
    NVNdevice dev{};
    init_device(&dev, 0);
    NVNqueue q{};
    init_queue(&q, &dev);

    // a submit gives the GPU timeline a serial to fence on
    NVNcommandBuffer cb{};
    const NVNcommandHandle h = record_empty_list(&cb, &dev);
    P<void(NVNqueue*, int, const NVNcommandHandle*)>("nvnQueueSubmitCommands")(&q, 1, &h);

    NVNsync s{};
    CHECK(P<NVNboolean(NVNsync*, NVNdevice*)>("nvnSyncInitialize")(&s, &dev) == 1);
    auto wait = P<NVNsyncWaitResult(const NVNsync*, uint64_t)>("nvnSyncWait");
    CHECK(static_cast<int>(wait(&s, 0)) == NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED);   // unarmed

    CHECK(P<NVNboolean(NVNqueue*, const NVNsync*)>("nvnQueueWaitSync")(&q, &s) == 1);

    P<void(NVNqueue*, NVNsync*, NVNsyncCondition, int)>("nvnQueueFenceSync")(&q, &s, NVN_SYNC_CONDITION_ALL_GPU_COMMANDS_COMPLETE, 0);
    be::Sync* hs = be::host_of<be::Sync>(&s);
    CHECK(hs != nullptr);
    CHECK(hs->armed.load());
    CHECK(hs->serial.load() == be::gpu::last_serial());
    // the stub reports every serial as done, so the armed sync is already signaled
    CHECK(static_cast<int>(wait(&s, 1000)) == NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED);

    P<void(NVNsync*, const char*)>("nvnSyncSetDebugLabel")(&s, "sync under test");
    P<void(NVNsync*)>("nvnSyncFinalize")(&s);
    CHECK(be::host_of<be::Sync>(&s) == nullptr);

    // not ours: wait is already satisfied, queue wait answers 0
    NVNsync z{};
    CHECK(static_cast<int>(wait(&z, 0)) == NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED);
    CHECK(P<NVNboolean(NVNqueue*, const NVNsync*)>("nvnQueueWaitSync")(&q, &z) == 0);
    P<void(NVNqueue*, NVNsync*, NVNsyncCondition, int)>("nvnQueueFenceSync")(&q, &z, NVN_SYNC_CONDITION_ALL_GPU_COMMANDS_COMPLETE, 0);
    CHECK(be::host_of<be::Sync>(&z) == nullptr);

    P<void(NVNqueue*)>("nvnQueueFinalize")(&q);
    P<void(NVNcommandBuffer*)>("nvnCommandBufferFinalize")(&cb);
    P<void(NVNdevice*)>("nvnDeviceFinalize")(&dev);
}

NXTEST(event_value_signal_and_offset) {
    setup();
    alignas(8) uint8_t mem[64] = {};
    NVNmemoryPool pool{};
    auto* mp = new be::MemoryPool();
    mp->base = mem;
    mp->size = sizeof mem;
    be::bind_guest(&pool, mp);

    NVNeventBuilder b{};
    P<void(NVNeventBuilder*)>("nvnEventBuilderSetDefaults")(&b);
    P<void(NVNeventBuilder*, const NVNmemoryPool*, int64_t)>("nvnEventBuilderSetStorage")(&b, &pool, 8);
    NVNevent ev{};
    CHECK(P<NVNboolean(NVNevent*, const NVNeventBuilder*)>("nvnEventInitialize")(&ev, &b) == 1);
    be::Event* e = be::host_of<be::Event>(&ev);
    CHECK(e != nullptr);
    CHECK(e->pool == mp);
    CHECK(e->offset == 8);

    auto get = P<uint32_t(const NVNevent*)>("nvnEventGetValue");
    auto sig = P<void(NVNevent*, NVNeventSignalMode, uint32_t)>("nvnEventSignal");
    CHECK(get(&ev) == 0u);
    poke32(mem + 8, 5);
    CHECK(get(&ev) == 5u);
    sig(&ev, NVN_EVENT_SIGNAL_MODE_WRITE, 9);
    CHECK(get(&ev) == 9u);
    CHECK(peek32(mem + 8) == 9u);
    sig(&ev, NVN_EVENT_SIGNAL_MODE_ADD, 3);
    CHECK(get(&ev) == 12u);
    sig(&ev, NVN_EVENT_SIGNAL_MODE_ADD, 0xFFFFFFFFu);      // wraps
    CHECK(get(&ev) == 11u);
    sig(&ev, NVN_EVENT_SIGNAL_MODE_WRITE, 0xFFFFFFFFu);
    CHECK(get(&ev) == 0xFFFFFFFFu);
    sig(&ev, NVN_EVENT_SIGNAL_MODE_ADD, 1);
    CHECK(get(&ev) == 0u);
    CHECK(P<ptrdiff_t(const NVNevent*)>("nvnEventGetMemoryOffset")(&ev) == 8);

    // an offset whose word lies past the end of the pool reads 0 and signals nothing
    NVNeventBuilder b2{};
    P<void(NVNeventBuilder*)>("nvnEventBuilderSetDefaults")(&b2);
    P<void(NVNeventBuilder*, const NVNmemoryPool*, int64_t)>("nvnEventBuilderSetStorage")(&b2, &pool, 64);
    NVNevent ev2{};
    CHECK(P<NVNboolean(NVNevent*, const NVNeventBuilder*)>("nvnEventInitialize")(&ev2, &b2) == 1);
    sig(&ev2, NVN_EVENT_SIGNAL_MODE_WRITE, 5);
    CHECK(get(&ev2) == 0u);

    // not ours
    NVNevent z{};
    CHECK(get(&z) == 0u);
    sig(&z, NVN_EVENT_SIGNAL_MODE_WRITE, 3);
    CHECK(P<ptrdiff_t(const NVNevent*)>("nvnEventGetMemoryOffset")(&z) == 0);

    P<void(NVNevent*)>("nvnEventFinalize")(&ev);
    P<void(NVNevent*)>("nvnEventFinalize")(&ev2);
    CHECK(be::host_of<be::Event>(&ev) == nullptr);
    be::unbind_guest(&pool);
}

NXTEST(window_textures_acquire_cycle_and_getters) {
    setup();
    NVNdevice dev{};
    init_device(&dev, 0);

    NVNtexture tex[3]{};
    for (auto& t : tex) be::bind_guest(&t, new be::Texture());
    NVNtexture* ptrs[3] = {&tex[0], &tex[1], &tex[2]};
    int native_marker = 0;

    NVNwindowBuilder wb{};
    P<void(NVNwindowBuilder*)>("nvnWindowBuilderSetDefaults")(&wb);
    P<void(NVNwindowBuilder*, NVNdevice*)>("nvnWindowBuilderSetDevice")(&wb, &dev);
    P<void(NVNwindowBuilder*, NVNnativeWindow)>("nvnWindowBuilderSetNativeWindow")(&wb, &native_marker);
    P<void(NVNwindowBuilder*, int, NVNtexture* const*)>("nvnWindowBuilderSetTextures")(&wb, 3, ptrs);
    P<void(NVNwindowBuilder*, int)>("nvnWindowBuilderSetPresentInterval")(&wb, 2);

    NVNwindow win{};
    CHECK(P<NVNboolean(NVNwindow*, const NVNwindowBuilder*)>("nvnWindowInitialize")(&win, &wb) == 1);
    be::Window* w = be::host_of<be::Window>(&win);
    CHECK(w != nullptr);
    CHECK(w->textures.size() == 3u);
    CHECK(w->present_interval == 2);
    CHECK(P<void*(const NVNwindow*)>("nvnWindowGetNativeWindow")(&win) == static_cast<void*>(&native_marker));
    CHECK(P<int(const NVNwindow*)>("nvnWindowGetPresentInterval")(&win) == 2);
    CHECK(P<int(const NVNwindow*)>("nvnWindowGetNumTextures")(&win) == 3);
    CHECK(P<int(const NVNwindow*)>("nvnWindowGetNumActiveTextures")(&win) == 3);

    auto acquire = P<NVNwindowAcquireTextureResult(NVNwindow*, NVNsync*, int*)>("nvnWindowAcquireTexture");
    NVNsync sync{};
    CHECK(P<NVNboolean(NVNsync*, NVNdevice*)>("nvnSyncInitialize")(&sync, &dev) == 1);
    int seq[4] = {-1, -1, -1, -1};
    for (int i = 0; i < 4; ++i) {
        const NVNwindowAcquireTextureResult r = acquire(&win, &sync, &seq[i]);
        CHECK(static_cast<int>(r) == NVN_WINDOW_ACQUIRE_TEXTURE_RESULT_SUCCESS);
    }
    CHECK(seq[0] == 0 && seq[1] == 1 && seq[2] == 2 && seq[3] == 0);
    be::Sync* hs = be::host_of<be::Sync>(&sync);
    CHECK(hs != nullptr && hs->armed.load());

    // active count 1: every acquire gives texture 0
    P<void(NVNwindow*, int)>("nvnWindowSetNumActiveTextures")(&win, 1);
    CHECK(P<int(const NVNwindow*)>("nvnWindowGetNumActiveTextures")(&win) == 1);
    for (int i = 0; i < 3; ++i) {
        int idx = -1;
        CHECK(static_cast<int>(acquire(&win, nullptr, &idx)) == NVN_WINDOW_ACQUIRE_TEXTURE_RESULT_SUCCESS);
        CHECK(idx == 0);
    }
    // back to 0: all three textures are active again
    P<void(NVNwindow*, int)>("nvnWindowSetNumActiveTextures")(&win, 0);
    CHECK(P<int(const NVNwindow*)>("nvnWindowGetNumActiveTextures")(&win) == 3);
    int idx = -1;
    CHECK(static_cast<int>(acquire(&win, nullptr, &idx)) == NVN_WINDOW_ACQUIRE_TEXTURE_RESULT_SUCCESS);
    CHECK(idx >= 0 && idx < 3);
    // a null index is tolerated
    CHECK(static_cast<int>(acquire(&win, nullptr, nullptr)) == NVN_WINDOW_ACQUIRE_TEXTURE_RESULT_SUCCESS);

    // queue present and queue acquire
    NVNqueue q{};
    init_queue(&q, &dev);
    P<void(NVNqueue*, NVNwindow*, int)>("nvnQueuePresentTexture")(&q, &win, 1);
    P<void(NVNqueue*, NVNwindow*, int)>("nvnQueuePresentTexture")(&q, &win, 7);   // out of range: logged, not presented
    int qidx = -1;
    CHECK(static_cast<int>(P<NVNqueueAcquireTextureResult(NVNqueue*, NVNwindow*, int*)>("nvnQueueAcquireTexture")(&q, &win, &qidx)) ==
          NVN_QUEUE_ACQUIRE_TEXTURE_RESULT_SUCCESS);
    CHECK(qidx >= 0 && qidx < 3);
    P<void(NVNqueue*)>("nvnQueueFinalize")(&q);

    // present interval extremes
    auto set_interval = P<void(NVNwindow*, int)>("nvnWindowSetPresentInterval");
    auto get_interval = P<int(const NVNwindow*)>("nvnWindowGetPresentInterval");
    set_interval(&win, 0);
    CHECK(get_interval(&win) == 0);
    set_interval(&win, 5);
    CHECK(get_interval(&win) == 5);

    // crop
    P<void(NVNwindow*, int, int, int, int)>("nvnWindowSetCrop")(&win, -5, 0, 1280, 720);
    NVNrectangle r{};
    P<void(const NVNwindow*, NVNrectangle*)>("nvnWindowGetCrop")(&win, &r);
    CHECK(r.x == -5 && r.y == 0 && r.width == 1280 && r.height == 720);
    P<void(const NVNwindow*, NVNrectangle*)>("nvnWindowGetCrop")(&win, nullptr);
    P<void(NVNwindow*, const char*)>("nvnWindowSetDebugLabel")(&win, "window under test");

    // a window without textures cannot acquire
    NVNwindow empty{};
    NVNwindowBuilder eb{};
    P<void(NVNwindowBuilder*)>("nvnWindowBuilderSetDefaults")(&eb);
    CHECK(P<NVNboolean(NVNwindow*, const NVNwindowBuilder*)>("nvnWindowInitialize")(&empty, &eb) == 1);
    CHECK(static_cast<int>(acquire(&empty, nullptr, &idx)) == NVN_WINDOW_ACQUIRE_TEXTURE_RESULT_NATIVE_ERROR);
    P<void(NVNwindow*)>("nvnWindowFinalize")(&empty);

    // not ours
    NVNwindow zero{};
    CHECK(static_cast<int>(acquire(&zero, nullptr, &idx)) == NVN_WINDOW_ACQUIRE_TEXTURE_RESULT_NATIVE_ERROR);
    CHECK(P<int(const NVNwindow*)>("nvnWindowGetNumTextures")(&zero) == 0);
    CHECK(P<void*(const NVNwindow*)>("nvnWindowGetNativeWindow")(&zero) == nullptr);
    CHECK(get_interval(&zero) == 0);

    P<void(NVNwindow*)>("nvnWindowFinalize")(&win);
    CHECK(be::host_of<be::Window>(&win) == nullptr);
    P<void(NVNsync*)>("nvnSyncFinalize")(&sync);
    for (auto& t : tex) be::unbind_guest(&t);
    P<void(NVNdevice*)>("nvnDeviceFinalize")(&dev);
}

NXTEST(no_op_and_hook_entry_points_are_callable) {
    setup();
    NVNdevice dev{};
    init_device(&dev, 0);
    P<void(NVNdevice*, NVNdepthMode)>("nvnDeviceSetDepthMode")(&dev, NVN_DEPTH_MODE_NEAR_IS_ZERO);
    P<void(NVNdevice*, const char*)>("nvnDeviceSetDebugLabel")(&dev, nullptr);
    CHECK(P<NVNtextureHandle(const NVNdevice*, int)>("nvnDeviceGetTexelFetchHandle")(&dev, 4) == be::make_texture_handle(4, 0));
    CHECK(P<NVNseparateSamplerHandle(const NVNdevice*, int)>("nvnDeviceGetSeparateSamplerHandle")(&dev, 2).value == 2u);
    CHECK(P<NVNboolean(const NVNdevice*)>("nvnDeviceIsExternalDebuggerAttached")(&dev) == 0);
    P<void(NVNdevice*, void*)>("nvnLoadCProcs")(&dev, nullptr);
    P<void(NVNboolean)>("nvnSetExternalDebuggerEnabledNVX")(1);
    P<void(NVNdevice*)>("nvnDeviceFinalize")(&dev);
}

NXTEST(objects_that_are_not_ours_are_tolerated) {
    setup();
    NVNdevice dev{};
    NVNqueue q{};
    NVNsync s{};
    NVNevent ev{};
    NVNwindow w{};
    NVNcommandHandle h = 0x1000;
    int idx = -1;

    CHECK(P<NVNtextureHandle(const NVNdevice*, int, int)>("nvnDeviceGetTextureHandle")(&dev, 1, 2) == 0);
    int v = 77;
    P<void(const NVNdevice*, NVNdeviceInfo, int*)>("nvnDeviceGetInteger")(&dev, NVN_DEVICE_INFO_UNIFORM_BUFFER_ALIGNMENT, &v);
    CHECK(v == 256);   // device info does not depend on the device object (the game asks before nvnDeviceInitialize)
    CHECK(static_cast<int>(P<NVNwindowOriginMode(const NVNdevice*)>("nvnDeviceGetWindowOriginMode")(&dev)) == 0);
    CHECK(static_cast<int>(P<NVNdepthMode(const NVNdevice*)>("nvnDeviceGetDepthMode")(&dev)) == 0);
    CHECK(P<NVNdebugDomainId(const NVNdevice*, const char*)>("nvnDeviceGenerateDebugDomainId")(&dev, "x") == 0);
    CHECK(P<NVNseparateTextureHandle(const NVNdevice*, int)>("nvnDeviceGetSeparateTextureHandle")(&dev, 3).value == 0u);
    NVNcounterData counter{};
    counter.timestamp = 99;
    CHECK(P<uint64_t(const NVNdevice*, const NVNcounterData*)>("nvnDeviceGetTimestampInNanoseconds")(&dev, &counter) == 0u);
    CHECK(P<NVNboolean(NVNdevice*, float)>("nvnDeviceRegisterFastClearDepth")(&dev, 1.0f) == 0);
    P<void(NVNdevice*, NVNwindowOriginMode)>("nvnDeviceSetWindowOriginMode")(&dev, NVN_WINDOW_ORIGIN_MODE_UPPER_LEFT);
    P<void(NVNdevice*)>("nvnDeviceFinalize")(&dev);
    P<void(NVNdevice*, NVNcommandHandle)>("nvnDeviceFinalizeCommandHandle")(&dev, h);

    const uint64_t serial = be::gpu::last_serial();
    P<void(NVNqueue*, int, const NVNcommandHandle*)>("nvnQueueSubmitCommands")(&q, 1, &h);
    P<void(NVNqueue*)>("nvnQueueFlush")(&q);
    P<void(NVNqueue*)>("nvnQueueFinish")(&q);
    P<void(NVNqueue*)>("nvnQueueFinalize")(&q);
    CHECK(be::gpu::last_serial() == serial);
    CHECK(static_cast<int>(P<NVNqueueAcquireTextureResult(NVNqueue*, NVNwindow*, int*)>("nvnQueueAcquireTexture")(&q, &w, &idx)) ==
          NVN_QUEUE_ACQUIRE_TEXTURE_RESULT_NATIVE_ERROR);
    P<void(NVNqueue*, NVNwindow*, int)>("nvnQueuePresentTexture")(&q, &w, 0);
    P<void(NVNqueue*, NVNsync*, NVNsyncCondition, int)>("nvnQueueFenceSync")(&q, &s, NVN_SYNC_CONDITION_ALL_GPU_COMMANDS_COMPLETE, 0);
    CHECK(P<NVNboolean(NVNqueue*, const NVNsync*)>("nvnQueueWaitSync")(&q, &s) == 0);
    CHECK(static_cast<int>(P<NVNqueueGetErrorResult(NVNqueue*, NVNqueueErrorInfo*)>("nvnQueueGetError")(&q, nullptr)) ==
          NVN_QUEUE_GET_ERROR_RESULT_GPU_NO_ERROR);

    CHECK(static_cast<int>(P<NVNsyncWaitResult(const NVNsync*, uint64_t)>("nvnSyncWait")(&s, 0)) ==
          NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED);
    P<void(NVNsync*)>("nvnSyncFinalize")(&s);

    CHECK(P<uint32_t(const NVNevent*)>("nvnEventGetValue")(&ev) == 0u);
    P<void(NVNevent*, NVNeventSignalMode, uint32_t)>("nvnEventSignal")(&ev, NVN_EVENT_SIGNAL_MODE_WRITE, 3);
    CHECK(P<ptrdiff_t(const NVNevent*)>("nvnEventGetMemoryOffset")(&ev) == 0);
    P<void(NVNevent*)>("nvnEventFinalize")(&ev);

    CHECK(static_cast<int>(P<NVNwindowAcquireTextureResult(NVNwindow*, NVNsync*, int*)>("nvnWindowAcquireTexture")(&w, nullptr, &idx)) ==
          NVN_WINDOW_ACQUIRE_TEXTURE_RESULT_NATIVE_ERROR);
    CHECK(P<int(const NVNwindow*)>("nvnWindowGetNumActiveTextures")(&w) == 0);
    CHECK(P<int(const NVNwindow*)>("nvnWindowGetPresentInterval")(&w) == 0);
    P<void(NVNwindow*, int)>("nvnWindowSetPresentInterval")(&w, 4);
    P<void(NVNwindow*, int)>("nvnWindowSetNumActiveTextures")(&w, 2);
    P<void(NVNwindow*, int, int, int, int)>("nvnWindowSetCrop")(&w, 0, 0, 1, 1);
    NVNrectangle r{};
    P<void(const NVNwindow*, NVNrectangle*)>("nvnWindowGetCrop")(&w, &r);
    P<void(NVNwindow*)>("nvnWindowFinalize")(&w);
    CHECK(be::host_of<be::Window>(&w) == nullptr);
}
