// SPDX-License-Identifier: GPL-3.0-or-later
// Settings services set and set:sys (S3c). Request bytes and expected replies are written out by hand from Switchbrew's
// "Settings services" and "System Version Title" pages (layouts and result codes as libnx and ScarletNX lay them out), never read
// from the implementation.
#include "svc_test_util.h"

#include <cstring>

#include "nxrt/config.h"

NXTEST_GROUP(svc_settings)

using namespace nxrt;

namespace {

// LanguageCode strings in nn::settings::Language order (Switchbrew "Settings services", LanguageCode table; pt-BR from 10.1.0).
const char* const kCodes[18] = {"ja",    "en-US", "fr",    "de",    "it",    "es",    "zh-CN", "ko",    "nl",
                                "pt",    "ru",    "zh-TW", "en-GB", "fr-CA", "es-419", "zh-Hans", "zh-Hant", "pt-BR"};

// The 8 bytes of a LanguageCode: the name NUL terminated, zero padded.
void code_bytes(uint8_t out[8], const char* name) {
    std::memset(out, 0, 8);
    std::memcpy(out, name, std::strlen(name));
}

// True when the reply's output is exactly the LanguageCode `name`.
bool out_is_code(const nxsvc::Reply& r, const char* name) {
    uint8_t want[8];
    code_bytes(want, name);
    return r.out.size() >= 8 && std::memcmp(r.out.data(), want, 8) == 0;
}

// No configuration overrides: the registered title's defaults.
void default_profile() {
    config_clear();
    services::profile_reload();
}

// The title's defaults with two console facts pinned to what the hand-written expectations below are for: the language (with none set the
// profile follows the player's pick or the Windows display language, which differs between machines) and firmware 12.1.0 (the one version
// whose hash the service knows; the test game may name another, oot 22.0.0: set_sys_firmware_version_without_a_known_hash).
void pinned_profile() {
    config_clear();
    config_set("system", "language", "AmericanEnglish");
    config_set("system", "firmware", "12.1.0");
    services::profile_reload();
}

// The 0x100-byte SystemVersion of firmware 12.1.0-1.0, written out from Switchbrew "System Version Title": Format table and the
// 12.1.0 row of the Known Versions table.
void expected_firmware(uint8_t want[0x100]) {
    std::memset(want, 0, 0x100);
    want[0] = 12;      // major
    want[1] = 1;       // minor
    want[2] = 0;       // micro
    want[4] = 1;       // revision major
    want[5] = 0;       // revision minor
    want[8] = 'N';     // platform "NX"
    want[9] = 'X';
    const char hash[] = "76b10c2dab7d3aa73fc162f8dff1655e6a21caf4";   // 0x28: version hash of 12.1.0-1.0
    std::memcpy(&want[0x28], hash, 40);
    const char display[] = "12.1.0";                                   // 0x68: display version
    std::memcpy(&want[0x68], display, sizeof display - 1);
    const char title[] = "NintendoSDK Firmware for NX 12.1.0-1.0";     // 0x80: display title
    std::memcpy(&want[0x80], title, sizeof title - 1);
}

}  // namespace

// set: the language code of the console (Japanese..BrazilianPortuguese), the 8-byte LanguageCode of a Language.
NXTEST(set_get_language_code) {
    nxsvc::Env env;
    pinned_profile();
    nxsvc::Session s = nxsvc::connect("set");
    REQUIRE(s.valid());
    nxsvc::Reply r = s.call(0);
    CHECK_EQ(r.result, 0u);
    CHECK(out_is_code(r, "en-US"));   // the profile's language, AmericanEnglish
    // another language the title supports reaches the reply the same way
    if (services::profile().supported_languages & (1u << static_cast<uint32_t>(services::Language::French))) {
        config_set("system", "language", "French");
        services::profile_reload();
        r = s.call(0);
        CHECK_EQ(r.result, 0u);
        CHECK(out_is_code(r, "fr"));
    }
    default_profile();
}

// MakeLanguageCode(Language): index 0 is Japanese, 17 BrazilianPortuguese (10.1.0+); 18 is out of range.
NXTEST(set_make_language_code) {
    nxsvc::Env env;
    nxsvc::Session s = nxsvc::connect("set");
    REQUIRE(s.valid());
    nxsvc::Reply r = s.call(2, nxsvc::Args().u32(0));
    CHECK_EQ(r.result, 0u);
    CHECK(out_is_code(r, "ja"));
    r = s.call(2, nxsvc::Args().u32(17));
    CHECK_EQ(r.result, 0u);
    CHECK(out_is_code(r, "pt-BR"));
    // Module 105 (set), description 625: 105 | (625 << 9) = 0x4E269 (Switchbrew error codes; ScarletNX RC(105, 625)).
    r = s.call(2, nxsvc::Args().u32(18));
    CHECK_EQ(r.result, 0x0004E269u);
}

// The language lists. Count and list of GetAvailableLanguageCodes(1) answer 15 entries (max_entries 0xF); the 2 variants answer 18.
NXTEST(set_available_language_codes) {
    nxsvc::Env env;
    nxsvc::Session s = nxsvc::connect("set");
    REQUIRE(s.valid());
    nxsvc::Reply r = s.call(3);
    CHECK_EQ(r.result, 0u);
    CHECK_EQ(r.get<int32_t>(0), 15);
    r = s.call(6);
    CHECK_EQ(r.result, 0u);
    CHECK_EQ(r.get<int32_t>(0), 18);

    // cmd 1 into a 160-byte C buffer: 15 codes, the 16th slot stays untouched (zero).
    uint8_t buf[160];
    std::memset(buf, 0, sizeof buf);
    r = s.call(1, nxsvc::Args().buf_c(buf, sizeof buf));
    CHECK_EQ(r.result, 0u);
    CHECK_EQ(r.get<int32_t>(0), 15);
    for (int i = 0; i < 15; ++i) {
        uint8_t want[8];
        code_bytes(want, kCodes[i]);
        CHECK(std::memcmp(buf + 8 * i, want, 8) == 0);
    }
    for (int i = 120; i < 160; ++i) CHECK_EQ(buf[i], 0u);

    // cmd 5 into a 160-byte B buffer: all 18 codes in order, the spare 16 bytes untouched.
    std::memset(buf, 0, sizeof buf);
    r = s.call(5, nxsvc::Args().buf_b(buf, sizeof buf));
    CHECK_EQ(r.result, 0u);
    CHECK_EQ(r.get<int32_t>(0), 18);
    for (int i = 0; i < 18; ++i) {
        uint8_t want[8];
        code_bytes(want, kCodes[i]);
        CHECK(std::memcmp(buf + 8 * i, want, 8) == 0);
    }
    for (int i = 144; i < 160; ++i) CHECK_EQ(buf[i], 0u);

    // cmd 5 into a 64-byte buffer: the count is capped by the buffer (8 codes).
    std::memset(buf, 0, sizeof buf);
    r = s.call(5, nxsvc::Args().buf_b(buf, 64));
    CHECK_EQ(r.get<int32_t>(0), 8);
    uint8_t want_last[8];
    code_bytes(want_last, kCodes[7]);
    CHECK(std::memcmp(buf + 56, want_last, 8) == 0);

    // cmd 1 without an output buffer: no codes, count 0.
    r = s.call(1);
    CHECK_EQ(r.result, 0u);
    CHECK_EQ(r.get<int32_t>(0), 0);
}

// GetRegionCode: RegionCode 1 (Usa) by default; the profile's region names map to Switchbrew's RegionCode values.
NXTEST(set_get_region_code) {
    nxsvc::Env env;
    default_profile();
    nxsvc::Session s = nxsvc::connect("set");
    REQUIRE(s.valid());
    nxsvc::Reply r = s.call(4);
    CHECK_EQ(r.result, 0u);
    CHECK_EQ(r.get<int32_t>(0), 1);

    config_set("system", "region", "Europe");
    services::profile_reload();
    r = s.call(4);
    CHECK_EQ(r.get<int32_t>(0), 2);

    // Korea is HongKongTaiwanKorea (4) and China is China (5) in Switchbrew's RegionCode table.
    config_set("system", "region", "Korea");
    services::profile_reload();
    r = s.call(4);
    CHECK_EQ(r.get<int32_t>(0), 4);

    config_set("system", "region", "China");
    services::profile_reload();
    r = s.call(4);
    CHECK_EQ(r.get<int32_t>(0), 5);
    default_profile();
}

// GetQuestFlag (5.0.0+): false for an ordinary console.
NXTEST(set_get_quest_flag) {
    nxsvc::Env env;
    nxsvc::Session s = nxsvc::connect("set");
    REQUIRE(s.valid());
    nxsvc::Reply r = s.call(8);
    CHECK_EQ(r.result, 0u);
    REQUIRE(r.out.size() >= 1);
    CHECK_EQ(r.out[0], 0u);
}

// GetDeviceNickName (set cmd 11, 10.1.0+): a 0x80-byte NUL-terminated name in a C buffer; default "nxrt", config [system] nickname.
NXTEST(set_get_device_nickname) {
    nxsvc::Env env;
    default_profile();
    nxsvc::Session s = nxsvc::connect("set");
    REQUIRE(s.valid());
    uint8_t buf[0x80];
    std::memset(buf, 0xEE, sizeof buf);
    nxsvc::Reply r = s.call(11, nxsvc::Args().buf_c(buf, sizeof buf));
    CHECK_EQ(r.result, 0u);
    CHECK(std::memcmp(buf, "nxrt\0", 5) == 0);
    CHECK_EQ(buf[5], 0u);
    CHECK_EQ(buf[0x7F], 0u);

    config_set("system", "nickname", "Sol");
    services::profile_reload();
    std::memset(buf, 0xEE, sizeof buf);
    r = s.call(11, nxsvc::Args().buf_c(buf, sizeof buf));
    CHECK_EQ(r.result, 0u);
    CHECK(std::memcmp(buf, "Sol\0", 4) == 0);
    CHECK_EQ(buf[4], 0u);
    default_profile();

    // Without an output buffer: the Ryujinx NullDeviceNicknameBuffer result, 105 | (808 << 9) = 0x65069.
    r = s.call(11);
    CHECK_EQ(r.result, 0x00065069u);
}

// GetFirmwareVersionForDebug (set cmd 10, 9.0.0+): the last 0x80 bytes of the firmware data (the display title half).
NXTEST(set_get_firmware_version_for_debug) {
    nxsvc::Env env;
    pinned_profile();
    nxsvc::Session s = nxsvc::connect("set");
    REQUIRE(s.valid());
    uint8_t want[0x100];
    expected_firmware(want);
    uint8_t buf[0x80];
    std::memset(buf, 0xEE, sizeof buf);
    nxsvc::Reply r = s.call(10, nxsvc::Args().buf_c(buf, sizeof buf));
    CHECK_EQ(r.result, 0u);
    CHECK(std::memcmp(buf, &want[0x80], 0x80) == 0);
}

// set:sys GetFirmwareVersion (3) and GetFirmwareVersion2 (4): the 0x100-byte SystemVersion of 12.1.0-1.0 into a C buffer.
NXTEST(set_sys_firmware_version) {
    nxsvc::Env env;
    pinned_profile();
    nxsvc::Session s = nxsvc::connect("set:sys");
    REQUIRE(s.valid());
    uint8_t want[0x100];
    expected_firmware(want);
    for (uint32_t cmd : {3u, 4u}) {
        uint8_t buf[0x100];
        std::memset(buf, 0xEE, sizeof buf);
        nxsvc::Reply r = s.call(cmd, nxsvc::Args().buf_c(buf, sizeof buf));
        CHECK_EQ(r.result, 0u);
        CHECK(std::memcmp(buf, want, 0x100) == 0);
    }
}

// set:sys GetColorSetId (23) and SetColorSetId (24): Basic White (0) by default; a value set on one session is seen on another.
// Another firmware (22.0.0, what oot names: the lower bound its ZBIC-compressed main needs): the version bytes, display version and title
// follow it; the hash stays empty (no source for it was checked, see settings.cpp).
NXTEST(set_sys_firmware_version_without_a_known_hash) {
    nxsvc::Env env;
    config_clear();
    config_set("system", "firmware", "22.0.0");
    services::profile_reload();
    nxsvc::Session s = nxsvc::connect("set:sys");
    REQUIRE(s.valid());
    uint8_t want[0x100];
    std::memset(want, 0, sizeof want);
    want[0] = 22;
    want[4] = 1;   // revision 1.0
    want[8] = 'N';
    want[9] = 'X';
    std::memcpy(&want[0x68], "22.0.0", 6);
    const char title[] = "NintendoSDK Firmware for NX 22.0.0-1.0";
    std::memcpy(&want[0x80], title, sizeof title - 1);
    uint8_t buf[0x100];
    std::memset(buf, 0xEE, sizeof buf);
    nxsvc::Reply r = s.call(3, nxsvc::Args().buf_c(buf, sizeof buf));
    CHECK_EQ(r.result, 0u);
    CHECK(std::memcmp(buf, want, 0x100) == 0);
    default_profile();
}

NXTEST(set_sys_color_set) {
    nxsvc::Env env;
    nxsvc::Session a = nxsvc::connect("set:sys");
    nxsvc::Session b = nxsvc::connect("set:sys");
    REQUIRE(a.valid());
    REQUIRE(b.valid());
    nxsvc::Reply r = a.call(23);
    CHECK_EQ(r.result, 0u);
    CHECK_EQ(r.get<int32_t>(0), 0);
    r = a.call(24, nxsvc::Args().i32(1));
    CHECK_EQ(r.result, 0u);
    r = b.call(23);
    CHECK_EQ(r.get<int32_t>(0), 1);
    r = b.call(24, nxsvc::Args().i32(0));
    CHECK_EQ(r.result, 0u);
    r = a.call(23);
    CHECK_EQ(r.get<int32_t>(0), 0);
}

// set:sys GetLockScreenFlag (7) and SetLockScreenFlag (8): off by default, the flag is one byte.
NXTEST(set_sys_lock_screen_flag) {
    nxsvc::Env env;
    nxsvc::Session s = nxsvc::connect("set:sys");
    REQUIRE(s.valid());
    nxsvc::Reply r = s.call(7);
    CHECK_EQ(r.result, 0u);
    REQUIRE(r.out.size() >= 1);
    CHECK_EQ(r.out[0], 0u);
    r = s.call(8, nxsvc::Args().u8(1));
    CHECK_EQ(r.result, 0u);
    r = s.call(7);
    REQUIRE(r.out.size() >= 1);
    CHECK_EQ(r.out[0], 1u);
    r = s.call(8, nxsvc::Args().u8(0));
    r = s.call(7);
    REQUIRE(r.out.size() >= 1);
    CHECK_EQ(r.out[0], 0u);
}

// set:sys GetSerialNumber (68): the 0x18-byte SerialNumber field (fixed fake: 14 characters, zero padded).
NXTEST(set_sys_serial_number) {
    nxsvc::Env env;
    nxsvc::Session s = nxsvc::connect("set:sys");
    REQUIRE(s.valid());
    nxsvc::Reply r = s.call(68);
    CHECK_EQ(r.result, 0u);
    // The field is 0x18 bytes; the HIPC framing pads the raw reply to a 16-byte boundary (32 bytes here), and the padding is zero.
    REQUIRE(r.out.size() >= 0x18);
    CHECK(std::memcmp(r.out.data(), "NXRT0000000000", 14) == 0);
    for (size_t i = 14; i < r.out.size(); ++i) CHECK_EQ(r.out[i], 0u);
}

// set:sys GetProductModel (79): ProductModel 1 (Nx, "HAC").
NXTEST(set_sys_product_model) {
    nxsvc::Env env;
    nxsvc::Session s = nxsvc::connect("set:sys");
    REQUIRE(s.valid());
    nxsvc::Reply r = s.call(79);
    CHECK_EQ(r.result, 0u);
    CHECK_EQ(r.get<int32_t>(0), 1);
}

// set:sys GetDeviceNickName (77): the same name as set cmd 11.
NXTEST(set_sys_device_nickname) {
    nxsvc::Env env;
    default_profile();
    nxsvc::Session s = nxsvc::connect("set:sys");
    REQUIRE(s.valid());
    uint8_t buf[0x80];
    std::memset(buf, 0xEE, sizeof buf);
    nxsvc::Reply r = s.call(77, nxsvc::Args().buf_c(buf, sizeof buf));
    CHECK_EQ(r.result, 0u);
    CHECK(std::memcmp(buf, "nxrt\0", 5) == 0);
    CHECK_EQ(buf[5], 0u);
}

// set:sys GetSettingsItemValueSize (37) and GetSettingsItemValue (38): the name!key table. Int: 4 bytes LE; bool: 1 byte;
// string: its characters and a NUL (size 11 for "screenshot"). A value is cut to the out buffer size. Other names: size 0.
NXTEST(set_sys_settings_item_value) {
    nxsvc::Env env;
    nxsvc::Session s = nxsvc::connect("set:sys");
    REQUIRE(s.valid());

    char name[0x48] = {}, key[0x48] = {};
    std::strcpy(name, "idle");
    std::strcpy(key, "dim_level_percent_lcd");
    nxsvc::Reply r = s.call(37, nxsvc::Args().buf_x(name, 0x48, 0).buf_x(key, 0x48, 1));
    CHECK_EQ(r.result, 0u);
    CHECK_EQ(r.get<uint64_t>(0), 4ull);
    uint8_t out[16];
    std::memset(out, 0xAA, sizeof out);
    r = s.call(38, nxsvc::Args().buf_x(name, 0x48, 0).buf_x(key, 0x48, 1).buf_b(out, sizeof out));
    CHECK_EQ(r.result, 0u);
    CHECK_EQ(r.get<uint64_t>(0), 4ull);
    CHECK(out[0] == 0x0A && out[1] == 0x00 && out[2] == 0x00 && out[3] == 0x00);
    CHECK_EQ(out[4], 0xAAu);

    std::memset(name, 0, sizeof name);
    std::memset(key, 0, sizeof key);
    std::strcpy(name, "capsrv");
    std::strcpy(key, "screenshot_layerstack");
    r = s.call(37, nxsvc::Args().buf_x(name, 0x48, 0).buf_x(key, 0x48, 1));
    CHECK_EQ(r.get<uint64_t>(0), 11ull);
    std::memset(out, 0xAA, sizeof out);
    r = s.call(38, nxsvc::Args().buf_x(name, 0x48, 0).buf_x(key, 0x48, 1).buf_b(out, 4));
    CHECK_EQ(r.get<uint64_t>(0), 4ull);
    CHECK(std::memcmp(out, "scre", 4) == 0);

    std::memset(name, 0, sizeof name);
    std::memset(key, 0, sizeof key);
    std::strcpy(name, "account");
    std::strcpy(key, "na_required_for_network_service");
    std::memset(out, 0xAA, sizeof out);
    r = s.call(38, nxsvc::Args().buf_x(name, 0x48, 0).buf_x(key, 0x48, 1).buf_b(out, sizeof out));
    CHECK_EQ(r.get<uint64_t>(0), 1ull);
    CHECK_EQ(out[0], 1u);

    // Not in the table: size 0, success, and the out buffer is cleared.
    std::memset(name, 0, sizeof name);
    std::memset(key, 0, sizeof key);
    std::strcpy(name, "no");
    std::strcpy(key, "such");
    r = s.call(37, nxsvc::Args().buf_x(name, 0x48, 0).buf_x(key, 0x48, 1));
    CHECK_EQ(r.result, 0u);
    CHECK_EQ(r.get<uint64_t>(0), 0ull);
    std::memset(out, 0x55, sizeof out);
    r = s.call(38, nxsvc::Args().buf_x(name, 0x48, 0).buf_x(key, 0x48, 1).buf_b(out, sizeof out));
    CHECK_EQ(r.result, 0u);
    CHECK_EQ(r.get<uint64_t>(0), 0ull);
    for (uint8_t v : out) CHECK_EQ(v, 0u);
}

// An unknown command id answers kStubService, and so do the commands that are out of this work's scope.
NXTEST(set_unhandled_commands_answer_stub) {
    nxsvc::Env env;
    nxsvc::Session s = nxsvc::connect("set");
    REQUIRE(s.valid());
    CHECK_EQ(s.call(0x7FFF).result, kernel::kStubService);
    CHECK_EQ(s.call(7).result, kernel::kStubService);   // set GetKeyCodeMap: not implemented
    nxsvc::Session sy = nxsvc::connect("set:sys");
    REQUIRE(sy.valid());
    CHECK_EQ(sy.call(0x7FFF).result, kernel::kStubService);
    CHECK_EQ(sy.call(39).result, kernel::kStubService);   // GetTvSettings: size not verified from two sources
    CHECK_EQ(sy.call(29).result, kernel::kStubService);   // GetNotificationSettings: size not verified
    CHECK_EQ(sy.call(43).result, kernel::kStubService);   // GetAudioOutputMode: signature not in Switchbrew
}

// The state set:sys keeps is process-wide and goes back to its start-up values with reset_all().
NXTEST(set_sys_state_reset) {
    nxsvc::Env env;
    nxsvc::Session s = nxsvc::connect("set:sys");
    REQUIRE(s.valid());
    CHECK_EQ(s.call(24, nxsvc::Args().i32(1)).result, 0u);
    CHECK_EQ(s.call(8, nxsvc::Args().u8(1)).result, 0u);
    nxsvc::Reply r = s.call(23);
    CHECK_EQ(r.get<int32_t>(0), 1);
    services::reset_all();
    nxsvc::Session again = nxsvc::connect("set:sys");
    REQUIRE(again.valid());
    r = again.call(23);
    CHECK_EQ(r.get<int32_t>(0), 0);
    r = again.call(7);
    REQUIRE(r.out.size() >= 1);
    CHECK_EQ(r.out[0], 0u);
}
