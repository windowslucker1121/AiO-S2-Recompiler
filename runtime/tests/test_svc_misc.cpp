// SPDX-License-Identifier: GPL-3.0-or-later
// Small services of the title's Service Access Control list (S3c): csrng, ldr:ro, mm:u, hwopus, aoc:u, caps:u, mii:u, irs,
// nfc:user, nfp:user, ectx:aw, banana, htc, htcs. Request bytes and expected results are written out from Switchbrew (command tables
// and the Error_codes page), not read from the implementation.
#include <filesystem>
#include <vector>
#include <windows.h>

#include "svc_test_util.h"

#include "nxrt/config.h"

NXTEST_GROUP(svc_misc)

using namespace nxrt;

// Switchbrew Error_codes: ro module 22, description 1028 (NotLoaded) = (1028 << 9) | 22.
static constexpr uint32_t kRoNotLoaded = 0x80816u;
// Switchbrew Error_codes: Sf_Hipc module 11, description 1 (NotSupported) = (1 << 9) | 11.
static constexpr uint32_t kNotSupported = 0x20Bu;

NXTEST(misc_csrng_generates_bytes) {
    nxsvc::Env env;
    nxsvc::Session s = nxsvc::connect("csrng");
    REQUIRE(s.valid());
    uint8_t a[32] = {};
    uint8_t b[32] = {};
    // IRandomInterface::GenerateRandomBytes (cmd 0) takes one output buffer.
    nxsvc::Reply r1 = s.call(0, nxsvc::Args().buf_b(a, sizeof a));
    CHECK(r1.ok());
    nxsvc::Reply r2 = s.call(0, nxsvc::Args().buf_b(b, sizeof b));
    CHECK(r2.ok());
    bool any = false;
    for (uint8_t v : a) any = any || v != 0;
    CHECK(any);                                    // 32 zero bytes from a CSPRNG would be a 2^-256 event
    CHECK(std::memcmp(a, b, sizeof a) != 0);
}

NXTEST(misc_ro_refuses_loading) {
    nxsvc::Env env;
    nxsvc::Session s = nxsvc::connect("ldr:ro");
    REQUIRE(s.valid());
    // RegisterProcessHandle (cmd 4): PID-descriptor, a process handle, u64 reserved. Any valid handle will do: the service ignores it.
    services::ServiceEvent ev;
    const uint32_t h = ev.new_handle();
    REQUIRE(h != 0);
    nxsvc::Reply reg = s.call(4, nxsvc::Args().with_pid().hcopy(h).u64(0));
    CHECK_EQ(reg.result, 0u);
    // MapManualLoadModuleMemory (cmd 0): PID-descriptor, u64 reserved, 4 x u64 (nro, nro size, bss, bss size).
    nxsvc::Reply map = s.call(0, nxsvc::Args().with_pid().u64(0).u64(0x1000).u64(0x1000).u64(0).u64(0));
    CHECK_EQ(map.result, kRoNotLoaded);
    kernel::handle_close(h);
}

NXTEST(misc_mm_request_keeps_rate) {
    nxsvc::Env env;
    nxsvc::Session s = nxsvc::connect("mm:u");
    REQUIRE(s.valid());
    // Initialize (cmd 4): module, priority, event clear mode -> request id.
    nxsvc::Reply init = s.call(4, nxsvc::Args().u32(1).u32(0).u32(0));
    REQUIRE(init.ok());
    const uint32_t id = init.get<uint32_t>(0);
    CHECK(id != 0);
    // SetAndWait (cmd 6): request id, min, max. Get (cmd 7): request id -> current.
    CHECK(s.call(6, nxsvc::Args().u32(id).u32(1000).u32(2000)).ok());
    nxsvc::Reply got = s.call(7, nxsvc::Args().u32(id));
    REQUIRE(got.ok());
    CHECK_EQ(got.get<uint32_t>(0), 1000u);
    // The old forms key by module: InitializeOld (cmd 0) module, priority, event clear mode; SetAndWaitOld (cmd 2) module, min, max;
    // GetOld (cmd 3) -> current.
    CHECK(s.call(0, nxsvc::Args().u32(3).u32(0).u32(0)).ok());
    CHECK(s.call(2, nxsvc::Args().u32(3).u32(500).u32(600)).ok());
    nxsvc::Reply old = s.call(3, nxsvc::Args().u32(3));
    REQUIRE(old.ok());
    CHECK_EQ(old.get<uint32_t>(0), 500u);
    CHECK(s.call(5, nxsvc::Args().u32(id)).ok());   // Finalize
}

NXTEST(misc_hwopus_size_and_refusal) {
    nxsvc::Env env;
    nxsvc::Session s = nxsvc::connect("hwopus");
    REQUIRE(s.valid());
    // GetWorkBufferSize (cmd 1): SampleRate (s32) and ChannelCount (s32) packed as one u64.
    const uint64_t packed = (uint64_t(2) << 32) | 48000u;
    nxsvc::Reply sz = s.call(1, nxsvc::Args().u64(packed));
    REQUIRE(sz.ok());
    const uint32_t size = sz.get<uint32_t>(0);
    CHECK(size != 0);
    CHECK_EQ(size % 0x1000u, 0u);                   // official user processes page-align this size
    // OpenHardwareOpusDecoder (cmd 0): packed rate/channels, u32 work buffer size, transfer memory handle. There is no Opus decoder on
    // the host, but the manager opens a decoder that decodes silence (title C aborts on a refusal); it comes back as a session handle.
    services::ServiceEvent ev;
    const uint32_t h = ev.new_handle();
    REQUIRE(h != 0);
    nxsvc::Reply open = s.call(0, nxsvc::Args().u64(packed).u32(size).hcopy(h));
    CHECK(open.ok());
    CHECK_EQ(open.move.size(), 1u);
    for (uint32_t m : open.move) kernel::handle_close(m);
    // The multistream decoders are still refused.
    nxsvc::Reply ms = s.call(2, nxsvc::Args().u64(packed).u32(size).hcopy(h));
    CHECK_EQ(ms.result, kNotSupported);
    kernel::handle_close(h);
}

NXTEST(misc_aoc_base_id_and_empty_list) {
    nxsvc::Env env;
    config_clear();
    services::profile_reload();
    nxsvc::Session s = nxsvc::connect("aoc:u");
    REQUIRE(s.valid());
    // GetAddOnContentBaseId (cmd 5): PID-descriptor -> u64. The title's add-on base id (control.nacp): its program id + 0x1000
    // (oot 0400486024DED000).
    nxsvc::Reply base = s.call(5, nxsvc::Args().with_pid());
    REQUIRE(base.ok());
    CHECK(services::profile().program_id != 0);
    CHECK_EQ(base.get<uint64_t>(0), services::profile().program_id + 0x1000ull);
    // CountAddOnContent (cmd 2): PID-descriptor -> s32 count = 0.
    nxsvc::Reply cnt = s.call(2, nxsvc::Args().with_pid());
    REQUIRE(cnt.ok());
    CHECK_EQ(cnt.get<int32_t>(0), 0);
    // GetAddOnContentListChangedEvent (cmd 8) returns an event handle.
    nxsvc::Reply ev = s.call(8);
    CHECK(ev.ok());
    CHECK_EQ(ev.copy.size(), 1u);
    config_clear();
    services::profile_reload();
}

NXTEST(misc_caps_album_is_empty) {
    nxsvc::Env env;
    nxsvc::Session s = nxsvc::connect("caps:u");
    REQUIRE(s.valid());
    // SetShimLibraryVersion (cmd 32): u64 version, u64 AppletResourceUserId, PID-descriptor.
    CHECK(s.call(32, nxsvc::Args().with_pid().u64(1).u64(0)).ok());
    // GetAlbumFileList3AaeAruid (cmd 142): PID-descriptor, u16 content type, u64 start, u64 end, u64 aruid; output buffer, count u64.
    uint8_t out[0x60];
    std::memset(out, 0xEE, sizeof out);
    nxsvc::Reply list = s.call(142, nxsvc::Args().with_pid().u16(0).align(8).u64(0).u64(0).u64(0).buf_b(out, sizeof out));
    REQUIRE(list.ok());
    CHECK_EQ(list.get<uint64_t>(0), 0u);
}

NXTEST(misc_caps_su_saves_png) {
    nxsvc::Env env;
    char tmp[MAX_PATH];
    GetTempPathA(MAX_PATH, tmp);
    const std::filesystem::path dir = std::filesystem::path(tmp) / "nxrt_album_test";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    config_set("album", "dir", dir.string().c_str());
    nxsvc::Session s = nxsvc::connect("caps:su");
    REQUIRE(s.valid());
    // SaveScreenShotEx1 (cmd 205) as the platform 2 sdk sends it: ScreenShotAttribute (0x40 bytes, ImageSize 0 = 1280x720), u32 report
    // option, u64 aruid; buffer 0 the 0x404-byte ApplicationData, buffer 1 the RGBA8 image.
    std::vector<uint8_t> img(1280 * 720 * 4, 0x80);
    std::vector<uint8_t> app(0x404, 0);
    nxsvc::Reply r = s.call(205, nxsvc::Args().u32(0).pad(0x3C).u32(0).align(8).u64(0).buf_a(app.data(), app.size()).buf_a(img.data(), img.size()));
    REQUIRE(r.ok());
    // ApplicationAlbumEntry: u64 file size first, the last byte 1.
    CHECK(r.get<uint64_t>(0) > 0);
    CHECK_EQ(r.get<uint8_t>(0x1F), 1u);
    size_t pngs = 0;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec))
        if (e.path().extension() == ".png") ++pngs;
    CHECK_EQ(pngs, 1u);
    std::filesystem::remove_all(dir, ec);
    config_clear();
}

NXTEST(misc_mii_empty_database) {
    nxsvc::Env env;
    nxsvc::Session s = nxsvc::connect("mii:u");
    REQUIRE(s.valid());
    // GetDatabaseService (cmd 0): i32 key code -> IDatabaseService (a move handle outside a domain).
    nxsvc::Reply db = s.call(0, nxsvc::Args().i32(0));
    REQUIRE(db.ok());
    nxsvc::Session d = s.sub(db);
    REQUIRE(d.valid());
    // GetCount (cmd 2): i32 source flag -> i32 count = 0.
    nxsvc::Reply cnt = d.call(2, nxsvc::Args().i32(0));
    REQUIRE(cnt.ok());
    CHECK_EQ(cnt.get<int32_t>(0), 0);
    // BuildDefault (cmd 7): u32 index. Index 6 is outside the six defaults and is refused (result value not checked here: it is
    // ScarletNX's, not Switchbrew's). Index 0 succeeds and returns one CharInfo (0x58 bytes).
    CHECK(d.call(7, nxsvc::Args().u32(6)).result != 0u);
    nxsvc::Reply def = d.call(7, nxsvc::Args().u32(0));
    REQUIRE(def.ok());
    CHECK(def.out.size() >= 0x58u);                // the reply is padded to 16 bytes; the CharInfo is the first 0x58
}

NXTEST(misc_nfc_user_no_device) {
    nxsvc::Env env;
    nxsvc::Session s = nxsvc::connect("nfc:user");
    REQUIRE(s.valid());
    // CreateUserInterface (cmd 0): no input, returns an IUser.
    nxsvc::Reply user = s.call(0);
    REQUIRE(user.ok());
    nxsvc::Session u = s.sub(user);
    REQUIRE(u.valid());
    // GetState (cmd 402): no input, u32 state. NFC is off: NonInitialized (0).
    nxsvc::Reply st = u.call(402);
    REQUIRE(st.ok());
    CHECK_EQ(st.get<uint32_t>(0), 0u);
    // ListDevices (cmd 404): output array of device handles, s32 count = 0.
    uint8_t arr[0x40];
    std::memset(arr, 0xEE, sizeof arr);
    nxsvc::Reply list = u.call(404, nxsvc::Args().buf_b(arr, sizeof arr));
    REQUIRE(list.ok());
    CHECK_EQ(list.get<int32_t>(0), 0);
}

NXTEST(misc_nfp_user_no_device) {
    nxsvc::Env env;
    nxsvc::Session s = nxsvc::connect("nfp:user");
    REQUIRE(s.valid());
    nxsvc::Reply user = s.call(0);
    REQUIRE(user.ok());
    nxsvc::Session u = s.sub(user);
    REQUIRE(u.valid());
    uint8_t arr[0x40];
    nxsvc::Reply list = u.call(2, nxsvc::Args().buf_b(arr, sizeof arr));
    REQUIRE(list.ok());
    CHECK_EQ(list.get<int32_t>(0), 0);
    // StartDetection (cmd 3): device handle (8 bytes). No device: an error result (its value is ScarletNX's, not checked here).
    nxsvc::Reply det = u.call(3, nxsvc::Args().u64(0));
    CHECK(det.result != 0u);
}

NXTEST(misc_ectx_accepts_context) {
    nxsvc::Env env;
    nxsvc::Session s = nxsvc::connect("ectx:aw");
    REQUIRE(s.valid());
    // CreateContextRegistrar (cmd 0) -> IContextRegistrar.
    nxsvc::Reply reg = s.call(0);
    REQUIRE(reg.ok());
    nxsvc::Session r = s.sub(reg);
    REQUIRE(r.valid());
    // Complete (cmd 0): s32 result, input buffer of the raw context -> s32 context_descriptor.
    uint8_t raw[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    nxsvc::Reply c = r.call(0, nxsvc::Args().i32(0).buf_a(raw, sizeof raw));
    REQUIRE(c.ok());
    CHECK_EQ(c.get<int32_t>(0), 0);
}

NXTEST(misc_irs_no_camera) {
    nxsvc::Env env;
    nxsvc::Session s = nxsvc::connect("irs");
    REQUIRE(s.valid());
    // ActivateIrsensor (cmd 302): aruid -> accepted.
    CHECK(s.call(302, nxsvc::Args().u64(1)).ok());
    // GetNpadIrCameraHandle (cmd 311): u32 npad id -> IrCameraHandle (4 bytes). No camera: 0.
    nxsvc::Reply h = s.call(311, nxsvc::Args().u32(0));
    REQUIRE(h.ok());
    CHECK_EQ(h.get<uint32_t>(0), 0u);
}

NXTEST(misc_banana_is_a_stub) {
    nxsvc::Env env;
    nxsvc::Session s = nxsvc::connect("banana");
    REQUIRE(s.valid());
    nxsvc::Reply r = s.call(0);
    CHECK_EQ(r.result, kernel::kStubService);
}

NXTEST(misc_htc_tma_refuses) {
    nxsvc::Env env;
    nxsvc::Session h = nxsvc::connect("htc");
    REQUIRE(h.valid());
    // GetEnvironmentVariable (cmd 0): absent on retail, answered with NotSupported.
    CHECK_EQ(h.call(0).result, kNotSupported);
    nxsvc::Session hs = nxsvc::connect("htcs");
    REQUIRE(hs.valid());
    // RegisterProcessId (cmd 100): as above.
    CHECK_EQ(hs.call(100, nxsvc::Args().u64(0)).result, kNotSupported);
}
