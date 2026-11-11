// SPDX-License-Identifier: GPL-3.0-or-later
// acc:u0 (nn::account::IAccountServiceForApplication) and acc:u1 (the same answers for the system side), with the interfaces they
// return: IProfile (nn::account::profile), IManagerForApplication (nn::account::baas) and IAsyncContext (nn::account::detail).
// The console has one user, the [account] user of the profile (services::profile()), which is open from the start and has no
// network service account.
//
// Adapted from ScarletNX src/nx/hle/services/acc.c (GPL-3.0-or-later): the command table (acc.swipc), the one-local-user answers,
// the profile timestamp (1546300800, 2019-01-01 UTC), the IManagerForApplication GetAccountId formula (uid[0] ^ uid[1]) and the
// IAsyncContext that is done at once and fails with the nifm result (110-300) because there is no network.
// Adapted from Ryujinx src/Ryujinx.HLE/HOS/Services/Account/Acc (MIT): IsUserRegistrationRequestPermitted per service (false on
// acc:u0, true on acc:u1), the IProfile Get/GetBase layout, and the NullArgument check of user ids.
//
// Uid bytes (nn::account::Uid is 0x10 bytes, two u64; Switchbrew names no order): the first u64 in memory is user_uid_hi, the
// second is user_uid_lo, both little-endian. Evidence: ScarletNX writes uid[0] first and prints it as the first 16 hex digits;
// Ryujinx's UserId writes High first and prints High then Low; libnx's AccountUid is uid[2] with uid[0] first. The hex form of
// [account] uid is hi then lo (services/profile), so the bytes follow it. services_api.h's "low word first in memory" note is not
// what these sources say; it is left as it is (not this file).
//
// Result codes (module 124 = acc, Switchbrew "Error codes"): 124-20 ResultInvalidUserId (0x287C), 124-32 ResultInvalidArrayLength
// (0x407C), 124-100 ResultUserNotExist (0xC87C). Module 110 (nifm) 300 comes from ScarletNX's comment; Switchbrew's page does not
// list it (checked), so that code is unconfirmed.
#include <algorithm>
#include <cstring>
#include <mutex>
#include <vector>

#include "nxrt/log.h"
#include "nxrt/services.h"
#include "nxrt/services_api.h"
#include "services_internal.h"

namespace nxrt::services {

namespace {

constexpr uint64_t kProfileLastEdit = 1546300800ull;   // ProfileBase.last_edit_timestamp: 2019-01-01 00:00 UTC (ScarletNX)
constexpr uint64_t kThumbnailSize = 0x24000;           // StoreSaveDataThumbnail's image: 256x144 RGBA (ScarletNX ACC_THUMBNAIL_SIZE)
constexpr uint64_t kUserListBytes = 0xA0;              // the user array of the list commands: array<Uid, 0xa> (Switchbrew)
const uint32_t kNsaRegistrationRequired = res(124, 200);   // ResultNetworkServiceAccountRegistrationRequired (0x1907C, Switchbrew)

struct Uid {
    uint64_t hi = 0, lo = 0;
    bool null() const { return (hi | lo) == 0; }
    bool operator==(const Uid& o) const { return hi == o.hi && lo == o.lo; }
};

Uid local_uid() {
    const TitleProfile& p = profile();
    return Uid{p.user_uid_hi, p.user_uid_lo};
}

// The users this process has open. The console opens its user at boot (Ryujinx opens the default user in its AccountManager
// constructor). No command of the acc:u0 / acc:u1 table below changes the set, so it stays at that one user until acc_reset().
struct AccState {
    std::mutex mu;
    std::vector<Uid> opened;
};
AccState& state() {
    static AccState s;
    return s;
}

// Only one user exists: the local one.
bool user_exists(const Uid& u) { return !u.null() && u == local_uid(); }

// Writes a Uid as 16 bytes: hi then lo, each little-endian (the host and the guest are little-endian x64).
void put_uid(uint8_t* p, const Uid& u) {
    std::memcpy(p, &u.hi, 8);
    std::memcpy(p + 8, &u.lo, 8);
}
void push_uid(ipc::Response& rsp, const Uid& u) {
    rsp.push<uint64_t>(u.hi);
    rsp.push<uint64_t>(u.lo);
}
// A Uid argument at `off` of the raw request data.
Uid arg_uid(const ipc::Request& req, size_t off) { return Uid{req.arg<uint64_t>(off), req.arg<uint64_t>(off + 8)}; }

// Fills the user array the guest gave (zeros after the users) with `users`, up to what the buffer holds.
void write_user_list(const ipc::Request& req, const std::vector<Uid>& users) {
    const ipc::Buffer out = req.out_buf(0);
    if (!out.addr) return;
    const uint64_t n = std::min<uint64_t>(out.size, kUserListBytes);
    std::vector<uint8_t> bytes(static_cast<size_t>(n), 0);
    for (size_t i = 0; i < users.size() && (i + 1) * 16 <= bytes.size(); ++i) put_uid(bytes.data() + i * 16, users[i]);
    buf_write(out, bytes.data(), bytes.size());
}

// nn::account::profile::ProfileBase (0x38 bytes): uid (0x10), last edit timestamp (u64 at 0x10), nickname (0x20 at 0x18, NUL padded).
void push_profile_base(ipc::Response& rsp, const Uid& u) {
    uint8_t b[0x38] = {};
    put_uid(b, u);
    std::memcpy(b + 0x10, &kProfileLastEdit, 8);
    const std::string& nick = profile().user_nickname;
    std::memcpy(b + 0x18, nick.data(), std::min<size_t>(nick.size(), 0x20));
    rsp.out.insert(rsp.out.end(), b, b + sizeof b);
}

// IProfile: Get (0), GetBase (1), GetImageSize (10), LoadImage (11). The picture is empty (size 0): Ryujinx embeds a stock JPEG and
// ScarletNX draws one, this file has neither, so a game that reserves GetImageSize bytes gets none.
std::shared_ptr<ipc::Service> make_profile(const Uid& u) {
    return make_iface("IProfile", [u](ipc::Service& s) {
        s.add_cmd(0, "Get", [u](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            // UserData (0x80 bytes) into the guest's buffer: all zeros, which is the default icon with no Mii.
            const std::vector<uint8_t> user_data(0x80, 0);
            buf_write(req.out_buf(0), user_data.data(), user_data.size());
            push_profile_base(rsp, u);
            return 0;
        });
        s.add_cmd(1, "GetBase", [u](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            push_profile_base(rsp, u);
            return 0;
        });
        s.add_cmd(10, "GetImageSize", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push<uint32_t>(0);
            return 0;
        });
        s.add_cmd(11, "LoadImage", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push<uint32_t>(0);
            return 0;
        });
    });
}

// IAsyncContext: an operation that needs the network. It is done at once and failed, as nifm fails with no network (ScarletNX).
std::shared_ptr<ipc::Service> make_async_context() {
    auto done = std::make_shared<ServiceEvent>(true);
    return make_iface("IAsyncContext", [done](ipc::Service& s) {
        s.add_cmd(0, "GetSystemEvent", [done](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.out_copy.push_back(done->new_handle());
            return 0;
        });
        s.add_cmd(1, "Cancel", [](ipc::Request&, ipc::Response&) -> uint32_t { return 0; });
        s.add_cmd(2, "HasDone", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push<uint8_t>(1);
            return 0;
        });
        s.add_cmd(3, "GetResult", [](ipc::Request&, ipc::Response&) -> uint32_t { return res(110, 300); });
    });
}

// IAsyncNetworkServiceLicenseKindContext (IAsyncContextForLoginForOnlinePlay): the IAsyncContext above plus cmd 100, the licence kind.
// nn::account::AsyncNetworkServiceLicenseInfoContext::GetNetworkServiceLicenseKind aborts when cmd 100 fails (sdk 0x750018c634), so it
// answers 0 (no licence) even though GetResult has already failed.
std::shared_ptr<ipc::Service> make_license_async_context() {
    std::shared_ptr<ipc::Service> s = make_async_context();
    s->add_cmd(100, "GetNetworkServiceLicenseKind", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        rsp.push<uint32_t>(0);
        return 0;
    });
    return s;
}

// nn::account::baas::IManagerForApplication: CheckAvailability (0), GetAccountId (1), EnsureIdTokenCacheAsync (2).
// The local user has no network service account. GetAccountId answers the id ScarletNX and Ryujinx make for a user (uid[0] ^ uid[1]);
// the network calls behind it fail (EnsureIdTokenCacheAsync's context).
std::shared_ptr<ipc::Service> make_manager(const Uid& u) {
    return make_iface("IManagerForApplication", [u](ipc::Service& s) {
        s.add_cmd(0, "CheckAvailability", [](ipc::Request&, ipc::Response&) -> uint32_t { return 0; });
        s.add_cmd(1, "GetAccountId", [u](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push<uint64_t>(u.hi ^ u.lo);
            return 0;
        });
        s.add_cmd(2, "EnsureIdTokenCacheAsync", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push_object(make_async_context());
            return 0;
        });
        // LoadIdTokenCache (3: the deprecated one on 19.0.0+, 4: the current one; nn::account::LoadNetworkServiceAccountIdTokenCache uses
        // vtable slot 0x40 = cmd 4). There is no token: 124-200 NetworkServiceAccountRegistrationRequired, which the SDK wrapper hands
        // back to the game (sdk 0x7500189ed0, no abort).
        for (uint32_t id : {3u, 4u}) {
            s.add_cmd(id, id == 3 ? "LoadIdTokenCacheDeprecated" : "LoadIdTokenCache",
                      [](ipc::Request&, ipc::Response&) -> uint32_t { return kNsaRegistrationRequired; });
        }
        // 170 LoadNetworkServiceLicenseKindAsync / EnsureIdTokenCacheForOnlinePlayAsync (nn::account::LoadNetworkServiceLicenseInfoAsync,
        // slot 0x60): an async context that is done and failed like the others, so the game reads "no online-service licence".
        s.add_cmd(170, "LoadNetworkServiceLicenseKindAsync", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push_object(make_license_async_context());
            return 0;
        });
    });
}

// Commands both acc:u0 and acc:u1 answer with the same ids and meaning in Switchbrew's tables.
void add_shared(ipc::Service& s, bool system_side) {
    s.add_cmd(0, "GetUserCount", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        rsp.push<int32_t>(1);
        return 0;
    });
    s.add_cmd(1, "GetUserExistence", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
        rsp.push<uint8_t>(user_exists(arg_uid(req, 0)) ? 1 : 0);
        return 0;
    });
    s.add_cmd(2, "ListAllUsers", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        write_user_list(req, {local_uid()});
        return 0;
    });
    s.add_cmd(3, "ListOpenUsers", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        std::vector<Uid> open;
        {
            std::lock_guard<std::mutex> lk(state().mu);
            open = state().opened;
        }
        write_user_list(req, open);
        return 0;
    });
    s.add_cmd(4, "GetLastOpenedUser", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        push_uid(rsp, local_uid());
        return 0;
    });
    s.add_cmd(5, "GetProfile", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
        const Uid u = arg_uid(req, 0);
        if (u.null()) return res(124, 20);
        if (!user_exists(u)) return res(124, 100);
        rsp.push_object(make_profile(u));
        return 0;
    });
    s.add_cmd(50, "IsUserRegistrationRequestPermitted", [system_side](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        rsp.push<uint8_t>(system_side ? 1 : 0);   // the application may not register users; the system service may (Ryujinx)
        return 0;
    });
    // TrySelectUserWithoutInteraction: with one user, that user. The network-account flag is only logged (ScarletNX and Ryujinx).
    auto try_select = [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
        if (req.arg<uint8_t>(0)) NXLOG_D("acc", "TrySelectUserWithoutInteraction: network service account required (not checked)");
        push_uid(rsp, local_uid());
        return 0;
    };
    s.add_cmd(51, "TrySelectUserWithoutInteractionDeprecated", try_select);
    s.add_cmd(52, "TrySelectUserWithoutInteraction", try_select);
    s.add_cmd(110, "StoreSaveDataThumbnail", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        if (arg_uid(req, 0).null()) return res(124, 20);
        if (req.in_buf(0).size != kThumbnailSize) return res(124, 32);
        return 0;   // the thumbnail is not kept (ScarletNX)
    });
    s.add_cmd(111, "ClearSaveDataThumbnail", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        if (arg_uid(req, 0).null()) return res(124, 20);
        return 0;
    });
}

// GetBaasAccountManagerForApplication (101) and LoadOpenContext (130): an IManagerForApplication for a known user.
uint32_t get_baas_manager(ipc::Request& req, ipc::Response& rsp) {
    const Uid u = arg_uid(req, 0);
    if (u.null()) return res(124, 20);
    if (!user_exists(u)) return res(124, 100);
    rsp.push_object(make_manager(u));
    return 0;
}

void setup_app(ipc::Service& s) {
    add_shared(s, false);
    // Accepted and ignored: the application's init call (InitializeApplicationInfo / V0 / V2 take the pid, Switchbrew).
    auto accept = [](ipc::Request&, ipc::Response&) -> uint32_t {
        NXLOG_D("acc", "InitializeApplicationInfo: accepted, no state kept");
        return 0;
    };
    s.add_cmd(100, "InitializeApplicationInfoV0", accept);
    s.add_cmd(140, "InitializeApplicationInfo", accept);
    s.add_cmd(160, "InitializeApplicationInfoV2", accept);   // newer SDKs' init call (Switchbrew: same pid input)
    s.add_cmd(101, "GetBaasAccountManagerForApplication", get_baas_manager);
    s.add_cmd(130, "LoadOpenContext", get_baas_manager);
    s.add_cmd(131, "ListOpenContextStoredUsers", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        std::vector<Uid> open;
        {
            std::lock_guard<std::mutex> lk(state().mu);
            open = state().opened;
        }
        write_user_list(req, open);
        return 0;
    });
    s.add_cmd(141, "ListQualifiedUsers", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        write_user_list(req, {local_uid()});
        return 0;
    });
    s.add_cmd(150, "IsUserAccountSwitchLocked", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        rsp.push<uint8_t>(0);   // the NACP's switch lock is not read here (Ryujinx reads it from the title's NACP)
        return 0;
    });
}

void setup_sys(ipc::Service& s) {
    add_shared(s, true);
    // acc:u1 command 140 is ListQualifiedUsers (Switchbrew's acc:u1 table), not the application's InitializeApplicationInfo.
    s.add_cmd(140, "ListQualifiedUsers", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        write_user_list(req, {local_uid()});
        return 0;
    });
}

}  // namespace

void register_acc() {
    add("acc:u0", [] { return make_iface("acc:u0", setup_app); });
    add("acc:u1", [] { return make_iface("acc:u1", setup_sys); });
}

void acc_reset() {
    std::lock_guard<std::mutex> lk(state().mu);
    state().opened.assign(1, local_uid());
}

namespace accapi {
void user_uid(uint64_t* hi, uint64_t* lo) {
    const TitleProfile& p = profile();
    if (hi) *hi = p.user_uid_hi;
    if (lo) *lo = p.user_uid_lo;
}
}  // namespace accapi

}  // namespace nxrt::services
