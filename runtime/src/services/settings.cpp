// SPDX-License-Identifier: GPL-3.0-or-later
// Settings services set (nn::settings::ISettingsServer) and set:sys (nn::settings::ISystemSettingsServer), IPC level (S3c).
// Adapted from ScarletNX src/nx/hle/services/set.c and set.swipc (GPL-3.0-or-later): the language order, the region mapping, the
// firmware version layout and the settings-item lookup.
// Adapted from Ryujinx src/Ryujinx.HLE/HOS/Services/Settings/ISettingsServer.cs, ISystemSettingsServer.cs, ResultCode.cs and
// NxSettings.cs (MIT): the not-found and buffer-size handling, the settings table, result module 105.
// Command ids, layouts and enum values follow https://switchbrew.org/wiki/Settings_services and
// https://switchbrew.org/wiki/System_Version_Title (SDK 12.x era).
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cstring>
#include <string>
#include <system_error>
#include <vector>

#include "nxrt/log.h"
#include "services_internal.h"

namespace nxrt::services {
namespace {

// ---- constants ----------------------------------------------------------------------------------------------------------------
constexpr uint32_t kSetModule = 105;                  // set, set:sys (ScarletNX RC(105, ...), Ryujinx ResultCode ModuleId 105)
constexpr uint32_t kLanguageOutOfRange = 625;         // MakeLanguageCode with an invalid Language (ScarletNX, Ryujinx)
constexpr uint32_t kNullDeviceNicknameBuffer = 808;   // GetDeviceNickName without an output buffer (Ryujinx)

// Language::Count, the nn::settings::Language order of Switchbrew: 18 entries, the last one BrazilianPortuguese (10.1.0+).
constexpr uint32_t kLanguageCount = static_cast<uint32_t>(Language::Count);
// GetAvailableLanguageCodes and its count answer at most 0xF entries (Switchbrew: max_entries 0xF; ScarletNX and Ryujinx cap at 15).
constexpr uint32_t kLanguageCountV1 = 15;
constexpr size_t kNickNameSize = 0x80;                // nn::settings::system::DeviceNickName
constexpr size_t kFirmwareSize = 0x100;               // nn::settings::system::FirmwareVersion (user processes: hard-coded 0x100)
constexpr size_t kSerialSize = 0x18;                  // nn::settings::factory::SerialNumber (Switchbrew: 0x18-byte string)
constexpr size_t kSettingsNameSize = 0x48;            // nn::settings::SettingsName and SettingsItemKey
constexpr int32_t kProductModelNx = 1;                // nn::settings::system::ProductModel: 1 Nx ("HAC", platform 1)

// ---- state -----------------------------------------------------------------------------------------------------------------------
// The state set:sys keeps for the run and that its getters report: the theme (ColorSetId: 0 Basic White, 1 Basic Black, Switchbrew)
// and the lock screen flag (false by default). Process-wide; every session object shares it. settings_reset() puts it back.
struct State {
    std::atomic<int32_t> color_set{0};
    std::atomic<uint8_t> lock_screen{0};
};
State& state() {
    static State s;
    return s;
}

// ---- helpers ----------------------------------------------------------------------------------------------------------------------
// The 8-byte LanguageCode of a language: its NUL-terminated name, zero padded (Switchbrew "LanguageCode").
void put_code(uint8_t* dst, Language l) {
    std::memset(dst, 0, 8);
    const char* c = language_code(l);
    std::memcpy(dst, c, std::min<size_t>(std::strlen(c), 7));
}

// GetAvailableLanguageCodes(2): as many codes as the out buffer holds, at most `max`; the count goes in the response.
uint32_t put_codes(ipc::Request& req, ipc::Response& rsp, uint32_t max) {
    const ipc::Buffer b = req.out_buf(0);
    const uint64_t n = std::min<uint64_t>(b.size / 8, max);
    std::vector<uint8_t> v(n * 8, 0);
    for (uint64_t i = 0; i < n; ++i) put_code(&v[i * 8], static_cast<Language>(i));
    buf_write(b, v.data(), v.size());
    rsp.push<int32_t>(static_cast<int32_t>(n));
    return 0;
}

// nn::settings::system::RegionCode: 0 Japan, 1 Usa, 2 Europe, 3 Australia, 4 HongKongTaiwanKorea, 5 China (Switchbrew, ScarletNX); the profile
// uses the same numbering (services.h). Out-of-range answers Usa (Ryujinx).
int32_t region_code() {
    const int r = profile().region;
    return r >= 0 && r <= 5 ? r : 1;
}

// "major.minor.micro" into out[0..2]; missing trailing parts stay 0. False when even the major part is not a number.
bool parse_version(const std::string& s, uint8_t out[3]) {
    const char* p = s.data();
    const char* end = p + s.size();
    for (int k = 0; k < 3; ++k) {
        unsigned v = 0;
        const auto r = std::from_chars(p, end, v);
        if (r.ec != std::errc{}) return k > 0;
        out[k] = static_cast<uint8_t>(v);
        p = r.ptr;
        if (p == end || *p != '.') break;
        ++p;
    }
    return true;
}

// The 0x100-byte SystemVersion of the System Version Title (Switchbrew "System Version Title", Format table): major, minor, micro
// at 0..2; revision major and minor at 4 and 5; platform "NX" at 0x08; version hash at 0x28 (40 hex characters); display version at
// 0x68 (0x18 bytes); display title at 0x80 (0x80 bytes). The version is profile().firmware ("12.1.0" by default), the title is
// "SDK Firmware for NX <version>-1.0" and the revision is 1.0 to match it. The hash is Switchbrew's for 12.1.0-1.0; other
// versions get an empty hash because no source for it was checked.
std::array<uint8_t, kFirmwareSize> firmware_version() {
    constexpr char kHash121[] = "76b10c2dab7d3aa73fc162f8dff1655e6a21caf4";
    uint8_t ver[3] = {0, 0, 0};
    const bool parsed = parse_version(profile().firmware, ver);
    const std::string fw = parsed ? profile().firmware : std::string("12.1.0");
    if (!parsed) {
        ver[0] = 12;
        ver[1] = 1;
        ver[2] = 0;
    }
    std::array<uint8_t, kFirmwareSize> v{};
    v[0] = ver[0];
    v[1] = ver[1];
    v[2] = ver[2];
    v[4] = 1;   // revision major
    v[5] = 0;   // revision minor
    std::memcpy(&v[0x08], "NX", 2);
    if (fw == "12.1.0") std::memcpy(&v[0x28], kHash121, 40);
    std::memcpy(&v[0x68], fw.data(), std::min<size_t>(fw.size(), 0x17));
    const std::string title = "NintendoSDK Firmware for NX " + fw + "-1.0";
    std::memcpy(&v[0x80], title.data(), std::min<size_t>(title.size(), 0x7F));
    return v;
}

// The device nickname into a DeviceNickName buffer: NUL terminated, the rest zero (Switchbrew; ScarletNX zeroes the buffer too).
uint32_t put_nickname(ipc::Request& req) {
    const ipc::Buffer b = req.out_buf(0);
    if (!b.addr) return res(kSetModule, kNullDeviceNicknameBuffer);
    std::vector<uint8_t> v(std::min<uint64_t>(b.size, kNickNameSize), 0);
    const std::string& nick = profile().device_nickname;
    if (!v.empty()) std::memcpy(v.data(), nick.data(), std::min(nick.size(), v.size() - 1));
    buf_write(b, v.data(), v.size());
    return 0;
}

// ---- settings items (set:sys GetSettingsItemValue) ----------------------------------------------------------------------------
// The name!key pairs a title may ask for. Values are the ones Ryujinx's NxSettings.cs holds (a Switch 3.0 config dump, not checked
// against 12.x). Encodings follow Switchbrew: a bool is one byte, an int four bytes little endian, a string with its NUL.
// Anything else answers size 0 with a zeroed buffer (ScarletNX: "no such setting here"; Ryujinx logs "not found").
enum class SettingKind : uint8_t { Bool, Int, Str };
struct SettingEntry {
    const char* name;
    const char* key;
    SettingKind kind;
    int32_t i;            // Bool (0 or 1) and Int values
    const char* s;        // Str value
};
constexpr SettingEntry kSettings[] = {
    {"account", "na_required_for_network_service", SettingKind::Bool, 1, nullptr},
    {"audio", "uac_speaker_enabled", SettingKind::Bool, 0, nullptr},
    {"idle", "dim_level_percent_lcd", SettingKind::Int, 10, nullptr},
    {"capsrv", "screenshot_layerstack", SettingKind::Str, 0, "screenshot"},
};

const SettingEntry* find_setting(const std::string& name, const std::string& key) {
    for (const SettingEntry& e : kSettings)
        if (name == e.name && key == e.key) return &e;
    return nullptr;
}

std::vector<uint8_t> setting_bytes(const SettingEntry& e) {
    switch (e.kind) {
        case SettingKind::Bool: return {static_cast<uint8_t>(e.i ? 1 : 0)};
        case SettingKind::Int: {
            std::vector<uint8_t> v(4);
            std::memcpy(v.data(), &e.i, 4);
            return v;
        }
        case SettingKind::Str: {
            const std::string s = e.s;
            std::vector<uint8_t> v(s.begin(), s.end());
            v.push_back(0);
            return v;
        }
    }
    return {};
}

// A SettingsName or SettingsItemKey buffer as text: the bytes up to the first NUL, at most 0x48 of them.
std::string read_setting_text(const ipc::Buffer& b) {
    char tmp[kSettingsNameSize] = {};
    buf_read(b, tmp, sizeof tmp - 1);
    return std::string(tmp);
}

// ---- set (ISettingsServer) ------------------------------------------------------------------------------------------------------
void setup_set(ipc::Service& s) {
    s.add_cmd(0, "GetLanguageCode", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        rsp.push<uint64_t>(language_code_u64(profile().language));
        return 0;
    });
    s.add_cmd(1, "GetAvailableLanguageCodes", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
        return put_codes(req, rsp, kLanguageCountV1);
    });
    s.add_cmd(2, "MakeLanguageCode", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
        const uint32_t l = req.arg<uint32_t>(0);
        if (l >= kLanguageCount) {
            NXLOG_W("set", "MakeLanguageCode(%u): out of range", l);
            return res(kSetModule, kLanguageOutOfRange);
        }
        rsp.push<uint64_t>(language_code_u64(static_cast<Language>(l)));
        return 0;
    });
    s.add_cmd(3, "GetAvailableLanguageCodeCount", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        rsp.push<int32_t>(static_cast<int32_t>(kLanguageCountV1));
        return 0;
    });
    s.add_cmd(4, "GetRegionCode", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        rsp.push<int32_t>(region_code());
        return 0;
    });
    s.add_cmd(5, "GetAvailableLanguageCodes2", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
        return put_codes(req, rsp, kLanguageCount);
    });
    s.add_cmd(6, "GetAvailableLanguageCodeCount2", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        rsp.push<int32_t>(static_cast<int32_t>(kLanguageCount));
        return 0;
    });
    s.add_cmd(8, "GetQuestFlag", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        rsp.push<uint8_t>(0);   // not a kiosk unit (Ryujinx answers false too)
        return 0;
    });
    // GetFirmwareVersionForDebug (9.0.0+): the last 0x80 bytes of the GetFirmwareVersion2 data. Switchbrew gates it on the debug
    // mode flag and says an error follows when that flag is 0; the flag's own fallback when its config is missing is 1, so this
    // never errors.
    s.add_cmd(10, "GetFirmwareVersionForDebug", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        const auto fw = firmware_version();
        buf_write(req.out_buf(0), fw.data() + 0x80, 0x80);
        return 0;
    });
    s.add_cmd(11, "GetDeviceNickName", [](ipc::Request& req, ipc::Response&) -> uint32_t { return put_nickname(req); });
}

// ---- set:sys (ISystemSettingsServer) --------------------------------------------------------------------------------------------
void setup_set_sys(ipc::Service& s) {
    auto firmware = [](ipc::Request& req, ipc::Response&) -> uint32_t {
        const auto fw = firmware_version();
        buf_write(req.out_buf(0), fw.data(), kFirmwareSize);
        return 0;
    };
    s.add_cmd(3, "GetFirmwareVersion", firmware);
    s.add_cmd(4, "GetFirmwareVersion2", firmware);
    s.add_cmd(7, "GetLockScreenFlag", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        rsp.push<uint8_t>(state().lock_screen.load());
        return 0;
    });
    s.add_cmd(8, "SetLockScreenFlag", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        state().lock_screen = req.arg<uint8_t>(0) ? 1 : 0;
        return 0;
    });
    s.add_cmd(23, "GetColorSetId", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        rsp.push<int32_t>(state().color_set.load());
        return 0;
    });
    s.add_cmd(24, "SetColorSetId", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        state().color_set = req.arg<int32_t>(0);
        return 0;
    });
    // GetSettingsItemValueSize(name, key) -> u64: the value's size, 0 when the setting is not in the table.
    s.add_cmd(37, "GetSettingsItemValueSize", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
        const SettingEntry* e = find_setting(read_setting_text(req.in_buf(0)), read_setting_text(req.in_buf(1)));
        uint64_t size = 0;
        if (e) size = setting_bytes(*e).size();
        rsp.push<uint64_t>(size);
        return 0;
    });
    // GetSettingsItemValue(name, key, out buffer) -> u64: min(value size, out buffer size) bytes are written and that count returned.
    s.add_cmd(38, "GetSettingsItemValue", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
        const ipc::Buffer out = req.out_buf(0);
        const std::string name = read_setting_text(req.in_buf(0));
        const std::string key = read_setting_text(req.in_buf(1));
        const SettingEntry* e = find_setting(name, key);
        uint64_t size = 0;
        if (e) {
            const std::vector<uint8_t> v = setting_bytes(*e);
            size = std::min<uint64_t>(v.size(), out.size);
            buf_write(out, v.data(), size);
        } else {
            NXLOG_I("set:sys", "GetSettingsItemValue(%s!%s): no such setting in the table, answering size 0", name.c_str(), key.c_str());
            const std::vector<uint8_t> zero(out.size, 0);
            buf_write(out, zero.data(), zero.size());
        }
        rsp.push<uint64_t>(size);
        return 0;
    });
    // GetSerialNumber -> 0x18-byte SerialNumber. A fixed fake (14 characters, zero padded), not a real console's serial.
    s.add_cmd(68, "GetSerialNumber", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        uint8_t field[kSerialSize] = {};
        std::memcpy(field, "NXRT0000000000", 14);
        rsp.out.insert(rsp.out.end(), field, field + kSerialSize);
        return 0;
    });
    s.add_cmd(77, "GetDeviceNickName", [](ipc::Request& req, ipc::Response&) -> uint32_t { return put_nickname(req); });
    s.add_cmd(79, "GetProductModel", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        rsp.push<int32_t>(kProductModelNx);
        return 0;
    });
}

}  // namespace

void register_settings() {
    add("set", [] { return make_iface("set", setup_set); });
    add("set:sys", [] { return make_iface("set:sys", setup_set_sys); });
}

void settings_reset() {
    state().color_set = 0;
    state().lock_screen = 0;
}

}  // namespace nxrt::services
