// SPDX-License-Identifier: GPL-3.0-or-later
// hid service (S3c): IPC replies and the shared-memory contents after activation and sampling.
// Every request byte and every shared-memory offset in this file is written out by hand from the Ryujinx struct layouts
// (refs ryubing Types/SharedMemory: SharedMemory.cs, Common/RingLifo.cs, Npad/NpadInternalState.cs, Npad/NpadCommonState.cs,
// TouchScreen/*.cs) and from the Switchbrew "HID services" argument tables. None of them is read from the service's own code.
#include <cstring>
#include <memory>

#include "../src/services/hid_shm.h"
#include "nxrt/config.h"
#include "nxrt/input_source.h"
#include "svc_test_util.h"

NXTEST_GROUP(svc_hid)

using namespace nxrt;

namespace {

// ---- layout, written by hand --------------------------------------------------------------------------------------------------------
constexpr uint64_t kShmSize = 0x40000;               // Switchbrew HID Shared Memory size; Ryujinx SharedMemory StructLayout Size
constexpr uint64_t kTouchScreen = 0x400;             // Ryujinx SharedMemory.TouchScreen FieldOffset
constexpr uint64_t kNpads = 0x9A00;                  // Ryujinx SharedMemory.Npads FieldOffset (player 1 entry)
constexpr uint64_t kNpadStride = 0x5000;             // Ryujinx NpadState Size
constexpr uint64_t kHandheldEntry = 8;               // Ryujinx PlayerIndex.Handheld
constexpr uint64_t kFullKeyLifo = 0x28;              // Ryujinx NpadInternalState.FullKey (after StyleSet, JoyAssignmentMode, colours)
constexpr uint64_t kHandheldLifo = 0x378;            // Ryujinx NpadInternalState.Handheld (FullKey + 0x350)
constexpr uint64_t kStyleSet = 0x0;                  // Ryujinx NpadInternalState.StyleSet (u32)
constexpr uint64_t kRingIndex = 0x10;                // RingLifo: {unused, bufferCount, index, count}, u64 each
constexpr uint64_t kRingCount = 0x18;
constexpr uint64_t kRingEntries = 0x20;              // first AtomicStorage<T>
constexpr uint64_t kCommonStorage = 0x30;            // AtomicStorage<NpadCommonState>: u64 sampling + 0x28 state
constexpr uint64_t kTouchStorage = 0x298;            // AtomicStorage<TouchScreenState>: u64 sampling + 0x290 state
constexpr uint64_t kEntrySampling = 0x8;             // the state's sampling number (AtomicStorage's own is a sequence lock: 2n)
constexpr uint64_t kEntrySeqLock = 0x0;              // AtomicStorage.SamplingNumber: even when stable
constexpr uint64_t kEntryButtons = 0x10;             // NpadCommonState.Buttons at state + 0x8
constexpr uint64_t kEntryStickL = 0x18;              // NpadCommonState.AnalogStickL at state + 0x10 (int32 x, int32 y)
constexpr uint64_t kEntryAttributes = 0x28;          // NpadCommonState.Attributes at state + 0x20
constexpr uint64_t kTouchCount = 0x10;               // TouchScreenState.TouchesCount at state + 0x8
constexpr uint64_t kTouch0Finger = 0x24;             // TouchState.FingerId at touch + 0xC; touch 0 starts at state + 0x10
constexpr uint64_t kTouch0X = 0x28;                  // TouchState.X at touch + 0x10
constexpr uint64_t kTouch0Y = 0x2C;                  // TouchState.Y at touch + 0x14
// NpadStyleTag and NpadButton bits (Ryujinx NpadStyleTag.cs, NpadButton.cs; Switchbrew NpadStyleTag / NpadButton).
constexpr uint32_t kTagFullKey = 1u << 0;
constexpr uint32_t kTagHandheld = 1u << 1;
constexpr uint32_t kTagJoyDual = 1u << 2;
constexpr uint64_t kButtonA = 1ull << 0;
constexpr uint64_t kButtonB = 1ull << 1;
constexpr uint64_t kButtonStickLDown = 1ull << 19;
// NpadAttribute: IsConnected 1 << 0, IsWired 1 << 1.
constexpr uint32_t kAttrConnected = 1u << 0;
constexpr uint32_t kAttrWired = 1u << 1;
// Argument layouts (Switchbrew HID services): SetSupportedNpadStyleSet takes a u32 style and a u64 AppletResourceUserId (at 8).
constexpr uint64_t kAruid = 0x1234;

uint64_t rd64(const uint8_t* p, uint64_t off) {
    uint64_t v = 0;
    std::memcpy(&v, p + off, 8);
    return v;
}
uint32_t rd32(const uint8_t* p, uint64_t off) {
    uint32_t v = 0;
    std::memcpy(&v, p + off, 4);
    return v;
}
int32_t rd32s(const uint8_t* p, uint64_t off) {
    int32_t v = 0;
    std::memcpy(&v, p + off, 4);
    return v;
}

// Address of the latest entry of a ring whose entries are `stride` bytes apart.
const uint8_t* latest_entry(const uint8_t* ring, uint64_t stride) {
    return ring + kRingEntries + rd64(ring, kRingIndex) * stride;
}
// Address of the entry `back` samples before the latest one.
const uint8_t* entry_back(const uint8_t* ring, uint64_t stride, uint64_t back) {
    const uint64_t idx = (rd64(ring, kRingIndex) + 17 - back) % 17;
    return ring + kRingEntries + idx * stride;
}

// Ties a test to one input source and clears it afterwards, also when a REQUIRE returns early.
struct SourceGuard {
    ~SourceGuard() { hid::set_input_source(nullptr); }
};

struct Hid {
    nxsvc::Session service;   // nn::hid::IHidServer
    nxsvc::Session resource;  // nn::hid::IAppletResource
    uint32_t shm_handle = 0;
    uint8_t* shm = nullptr;   // host view of the shared memory
};

// CreateAppletResource (cmd 0, PID + u64 aruid), then GetSharedMemoryHandle (IAppletResource cmd 0).
bool open_hid(Hid& out) {
    out.service = nxsvc::connect("hid");
    if (!out.service.valid()) return false;
    nxsvc::Reply r = out.service.call(0, nxsvc::Args().with_pid().u64(kAruid));
    if (!r.ok() || r.move.size() != 1) return false;
    out.resource = out.service.sub(r);
    nxsvc::Reply m = out.resource.call(0);
    if (!m.ok() || m.copy.size() != 1) return false;
    out.shm_handle = m.copy[0];
    auto obj = kernel::handle_as<kernel::KSharedMemory>(out.shm_handle);
    if (!obj) return false;
    out.shm = reinterpret_cast<uint8_t*>(obj->host);
    return out.shm != nullptr;
}

// ActivateNpad (cmd 103) and SetSupportedNpadStyleSet (cmd 100, u32 style, aruid at 8).
bool activate_npad(Hid& h, uint32_t styles) {
    if (!h.service.call(103, nxsvc::Args().with_pid().u64(kAruid)).ok()) return false;
    return h.service.call(100, nxsvc::Args().with_pid().u32(styles).align(8).u64(kAruid)).ok();
}

}  // namespace

NXTEST(hid_disconnect_npad_takes_back_an_assigned_pad) {
    nxsvc::Env env;
    services::hid_set_background_sampling(false);
    Hid h;
    REQUIRE(open_hid(h));
    // Players 2 and 3 have pads 21 and 22 (the controller applet's assignment). DisconnectNpad(2) (player 3) takes pad 22 back; (1) then
    // pad 21, which empties the list. Player 1 (id 0) is never taken.
    hid::set_player_pads({21, 22});
    CHECK_EQ(h.service.call(107, nxsvc::Args().u32(2).align(8).u64(kAruid)).result, 0u);
    REQUIRE(hid::player_pads().size() == 1u);
    CHECK_EQ(hid::player_pads()[0], 21u);
    CHECK_EQ(h.service.call(107, nxsvc::Args().u32(0).align(8).u64(kAruid)).result, 0u);
    CHECK_EQ(hid::player_pads().size(), 1u);
    CHECK_EQ(h.service.call(107, nxsvc::Args().u32(1).align(8).u64(kAruid)).result, 0u);
    CHECK(hid::player_pads().empty());
}

NXTEST(hid_shared_memory_handle) {
    nxsvc::Env env;
    services::hid_set_background_sampling(false);
    Hid h;
    REQUIRE(open_hid(h));
    auto obj = kernel::handle_as<kernel::KSharedMemory>(h.shm_handle);
    REQUIRE(obj != nullptr);
    CHECK_EQ(obj->size, kShmSize);
}

NXTEST(hid_fullkey_pad_sample) {
    nxsvc::Env env;
    services::hid_set_background_sampling(false);
    SourceGuard guard;
    auto src = std::make_shared<hid::ManualInputSource>();
    hid::set_input_source(src);
    Hid h;
    REQUIRE(open_hid(h));
    REQUIRE(activate_npad(h, kTagFullKey | kTagHandheld | kTagJoyDual));
    nxsvc::Reply g = h.service.call(101, nxsvc::Args().with_pid().u64(kAruid));
    REQUIRE(g.ok());
    CHECK_EQ(g.get<uint32_t>(0), kTagFullKey | kTagHandheld | kTagJoyDual);

    // Player 1 as a FullKey pad, left stick pushed hard down (y = -20000 is past half range), A pressed.
    src->connect(0, hid::NpadStyle::FullKey);
    hid::InputSnapshot s = src->get();
    s.npad[0].left.x = 1000;
    s.npad[0].left.y = -20000;
    src->set(s);
    src->press(0, hid::btn(hid::kBtnA));

    services::hid_sample_once();
    services::hid_sample_once();

    const uint8_t* player1 = h.shm + kNpads;
    CHECK_EQ(rd32(player1, kStyleSet), kTagFullKey);
    const uint8_t* lifo = player1 + kFullKeyLifo;
    CHECK_EQ(rd64(lifo, kRingCount), 2u);
    const uint8_t* last = latest_entry(lifo, kCommonStorage);
    CHECK_EQ(rd64(last, kEntrySampling), 2u);
    CHECK_EQ(rd64(last, kEntrySeqLock), 4u);
    CHECK((rd64(last, kEntryButtons) & kButtonA) != 0);
    CHECK((rd64(last, kEntryButtons) & kButtonStickLDown) != 0);
    CHECK_EQ(rd32s(last, kEntryStickL), 1000);
    CHECK_EQ(rd32s(last, kEntryStickL + 4), -20000);
    CHECK_EQ(rd32(last, kEntryAttributes) & (kAttrConnected | kAttrWired), kAttrConnected | kAttrWired);
    // The sample before it carries sampling number 1 and the same press.
    const uint8_t* before = entry_back(lifo, kCommonStorage, 1);
    CHECK_EQ(rd64(before, kEntrySampling), 1u);
    CHECK((rd64(before, kEntryButtons) & kButtonA) != 0);
}

NXTEST(hid_disconnect_clears_attributes) {
    nxsvc::Env env;
    services::hid_set_background_sampling(false);
    SourceGuard guard;
    auto src = std::make_shared<hid::ManualInputSource>();
    hid::set_input_source(src);
    Hid h;
    REQUIRE(open_hid(h));
    REQUIRE(activate_npad(h, kTagFullKey));
    src->connect(0, hid::NpadStyle::FullKey);
    services::hid_sample_once();

    hid::InputSnapshot s = src->get();
    s.npad[0].connected = false;
    src->set(s);
    services::hid_sample_once();
    const uint8_t* player1 = h.shm + kNpads;
    CHECK_EQ(rd32(player1, kStyleSet), 0u);
    const uint8_t* last = latest_entry(player1 + kFullKeyLifo, kCommonStorage);
    CHECK_EQ(rd32(last, kEntryAttributes), 0u);
    CHECK_EQ(rd64(last, kEntrySampling), 2u);
}

NXTEST(hid_default_pad_without_source) {
    nxsvc::Env env;
    services::hid_set_background_sampling(false);
    SourceGuard guard;
    config_clear();
    services::profile_reload();   // docked by default: no handheld pad
    Hid h;
    REQUIRE(open_hid(h));
    REQUIRE(activate_npad(h, kTagFullKey));
    services::hid_sample_once();
    const uint8_t* player1 = h.shm + kNpads;
    CHECK_EQ(rd32(player1, kStyleSet), kTagFullKey);
    CHECK_EQ(rd64(latest_entry(player1 + kFullKeyLifo, kCommonStorage), kEntrySampling), 1u);
    CHECK_EQ(rd32(h.shm + kNpads + kHandheldEntry * kNpadStride, kStyleSet), 0u);
}

NXTEST(hid_handheld_entry_in_handheld_mode) {
    nxsvc::Env env;
    services::hid_set_background_sampling(false);
    SourceGuard guard;
    config_clear();
    config_set("am", "operation_mode", "handheld");
    services::profile_reload();
    auto src = std::make_shared<hid::ManualInputSource>();
    hid::set_input_source(src);
    Hid h;
    REQUIRE(open_hid(h));
    REQUIRE(activate_npad(h, kTagFullKey | kTagHandheld));
    src->connect(hid::kNpadSlotHandheld, hid::NpadStyle::Handheld);
    src->press(hid::kNpadSlotHandheld, hid::btn(hid::kBtnB));
    services::hid_sample_once();

    const uint8_t* handheld = h.shm + kNpads + kHandheldEntry * kNpadStride;
    CHECK_EQ(rd32(handheld, kStyleSet), kTagHandheld);
    const uint8_t* last = latest_entry(handheld + kHandheldLifo, kCommonStorage);
    CHECK_EQ(rd64(last, kEntrySampling), 1u);
    CHECK((rd64(last, kEntryButtons) & kButtonB) != 0);

    config_clear();
    services::profile_reload();
}

NXTEST(hid_touch_screen_entry) {
    nxsvc::Env env;
    services::hid_set_background_sampling(false);
    SourceGuard guard;
    auto src = std::make_shared<hid::ManualInputSource>();
    hid::set_input_source(src);
    Hid h;
    REQUIRE(open_hid(h));
    // ActivateTouchScreen (cmd 11): PID and the aruid.
    REQUIRE(h.service.call(11, nxsvc::Args().with_pid().u64(kAruid)).ok());

    hid::InputSnapshot s = src->get();
    s.touch.count = 1;
    s.touch.fingers[0].id = 3;
    s.touch.fingers[0].x = 640;
    s.touch.fingers[0].y = 360;
    src->set(s);
    services::hid_sample_once();

    const uint8_t* touch = h.shm + kTouchScreen;
    CHECK_EQ(rd64(touch, kRingCount), 1u);
    const uint8_t* last = latest_entry(touch, kTouchStorage);
    CHECK_EQ(rd64(last, kEntrySampling), 1u);
    CHECK_EQ(rd32(last, kTouchCount), 1u);
    CHECK_EQ(rd32(last, kTouch0Finger), 3u);
    CHECK_EQ(rd32(last, kTouch0X), 640u);
    CHECK_EQ(rd32(last, kTouch0Y), 360u);
}

// ---- vibration sink: SendVibrationValue (cmd 201) forwards the value to nxrt::hid::set_vibration_sink ---------------------------------
namespace {

struct VibrationSeen {
    int calls = 0;
    uint32_t slot = 0;
    int hand = -1;
    hid::VibrationValue v;
};
void record_vibration(uint32_t slot, int hand, const hid::VibrationValue& v, void* user) {
    auto* seen = static_cast<VibrationSeen*>(user);
    seen->calls++;
    seen->slot = slot;
    seen->hand = hand;
    seen->v = v;
}
// Removes the vibration sink when a REQUIRE returns early.
struct VibrationSinkGuard {
    ~VibrationSinkGuard() { hid::set_vibration_sink(nullptr); }
};

}  // namespace

NXTEST(hid_send_vibration_value_reaches_sink) {
    nxsvc::Env env;
    services::hid_set_background_sampling(false);
    VibrationSinkGuard guard;
    VibrationSeen seen;
    hid::set_vibration_sink(&record_vibration, &seen);
    Hid h;
    REQUIRE(open_hid(h));
    // Handle (u32): device type 3 (FullKey / LRA), npad id 0 (player 1), position 0 (left). Then the VibrationValue (amp_low, freq_low,
    // amp_high, freq_high) and the aruid, which the service does not use.
    constexpr uint32_t kHandleP1Left = 0x3;
    auto send = [&]() {
        return h.service.call(201, nxsvc::Args().with_pid().u32(kHandleP1Left).f32(0.5f).f32(160.0f).f32(0.25f).f32(320.0f).align(8).u64(kAruid));
    };
    REQUIRE(send().ok());
    CHECK_EQ(seen.calls, 1);
    CHECK_EQ(seen.slot, 0u);
    CHECK_EQ(seen.hand, 0);
    CHECK_EQ(seen.v.amp_low, 0.5f);
    CHECK_EQ(seen.v.freq_low, 160.0f);
    CHECK_EQ(seen.v.amp_high, 0.25f);
    CHECK_EQ(seen.v.freq_high, 320.0f);

    // Sink removed: the command is still accepted and nothing is delivered.
    hid::set_vibration_sink(nullptr);
    REQUIRE(send().ok());
    CHECK_EQ(seen.calls, 1);
}
