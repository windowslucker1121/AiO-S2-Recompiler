// SPDX-License-Identifier: GPL-3.0-or-later
// acc:u0 / acc:u1 and their IProfile, IManagerForApplication and IAsyncContext (S3c). Request bytes and expected replies are
// written out from Switchbrew's acc tables and error list (0x287C = 124-20 ResultInvalidUserId, 0x407C = 124-32, 0xC87C = 124-100)
// and from the layouts in docs/S3c.md; the single user is the default [account] uid of the registered title (services::TitleProfile
// user_uid_hi / user_uid_lo; oot: hi 1, lo "OOTRecmp"), whose bytes are laid out here by hand (hi first, each little-endian; see the
// comment at the top of src/services/acc.cpp).
#include <cstring>
#include <string>
#include <vector>

#include "nxrt/config.h"
#include "svc_test_util.h"

NXTEST_GROUP(svc_acc)

using namespace nxrt;
using nxsvc::Args;

namespace {

// The local user's Uid as it sits in guest memory: hi then lo, each u64 little-endian, byte by byte (not a memcpy of the struct).
const uint8_t* local_uid() {
    static uint8_t b[16];
    const services::TitleProfile& p = services::profile();
    for (int i = 0; i < 8; ++i) {
        b[i] = static_cast<uint8_t>(p.user_uid_hi >> (8 * i));
        b[8 + i] = static_cast<uint8_t>(p.user_uid_lo >> (8 * i));
    }
    return b;
}
// hi 3, lo 3: not a user of the console.
const uint8_t kOtherUid[16] = {0x03, 0, 0, 0, 0, 0, 0, 0, 0x03, 0, 0, 0, 0, 0, 0, 0};
const uint8_t kNullUid[16] = {};
// 1546300800 = 2019-01-01 00:00 UTC, the ProfileBase timestamp (ScarletNX).
constexpr uint64_t kLastEdit = 1546300800ull;

// A fresh start: the default profile, every service reset (acc_reset takes the profile's user).
void fresh_profile() {
    config_clear();
    services::profile_reload();
    services::reset_all();
}

// The 0x38-byte ProfileBase the default user has: uid, timestamp, the title's default nickname (oot "Player") NUL padded to 0x20.
void check_profile_base(const nxsvc::Reply& r) {
    // the raw reply is padded to 16 bytes (hipc), so the layout is checked by offset, not by size
    CHECK(r.out.size() >= 0x38);
    CHECK(r.out.size() >= 0x38 && std::memcmp(r.out.data(), local_uid(), 16) == 0);
    CHECK_EQ(r.get<uint64_t>(0x10), kLastEdit);
    const std::string& nick = services::profile().user_nickname;
    CHECK(!nick.empty() && nick.size() < 0x20);
    CHECK(r.str(0x18, 0x20) == nick);
    for (size_t i = 0x18 + nick.size(); i < 0x38 && i < r.out.size(); ++i) CHECK(r.out[i] == 0);
}

}  // namespace

NXTEST(acc_user_count_and_existence) {
    nxsvc::Env env;
    fresh_profile();
    nxsvc::Session s = nxsvc::connect("acc:u0");
    REQUIRE(s.valid());
    // GetUserCount (0): no input, i32 output = 1
    nxsvc::Reply r = s.call(0);
    CHECK(r.ok());
    CHECK(r.out.size() >= 4);
    CHECK_EQ(r.get<int32_t>(0), 1);
    // GetUserExistence (1): Uid input; the local user yes, another uid no, the null uid no (not an error)
    r = s.call(1, Args().bytes(local_uid(), 16));
    CHECK(r.ok());
    CHECK_EQ(r.get<uint8_t>(0), 1u);
    r = s.call(1, Args().bytes(kOtherUid, 16));
    CHECK(r.ok());
    CHECK_EQ(r.get<uint8_t>(0), 0u);
    r = s.call(1, Args().bytes(kNullUid, 16));
    CHECK(r.ok());
    CHECK_EQ(r.get<uint8_t>(0), 0u);
}

NXTEST(acc_list_users_writes_the_array) {
    nxsvc::Env env;
    fresh_profile();
    nxsvc::Session s = nxsvc::connect("acc:u0");
    REQUIRE(s.valid());
    // ListAllUsers (2) and ListOpenUsers (3): the array of 0xa Uids in the receive buffer; the first entry is the user, the rest zero.
    for (uint32_t cmd : {2u, 3u}) {
        uint8_t buf[0xA0];
        std::memset(buf, 0xEE, sizeof buf);
        nxsvc::Reply r = s.call(cmd, Args().buf_c(buf, 0xA0));
        CHECK(r.ok());
        CHECK(std::memcmp(buf, local_uid(), 16) == 0);
        bool rest_zero = true;
        for (size_t i = 16; i < sizeof buf; ++i) rest_zero = rest_zero && buf[i] == 0;
        CHECK(rest_zero);
    }
}

NXTEST(acc_last_opened_user) {
    nxsvc::Env env;
    fresh_profile();
    nxsvc::Session s = nxsvc::connect("acc:u0");
    REQUIRE(s.valid());
    // GetLastOpenedUser (4): the Uid, 16 bytes
    nxsvc::Reply r = s.call(4);
    CHECK(r.ok());
    CHECK(r.out.size() >= 16 && std::memcmp(r.out.data(), local_uid(), 16) == 0);
}

NXTEST(acc_get_profile_and_base) {
    nxsvc::Env env;
    fresh_profile();
    nxsvc::Session s = nxsvc::connect("acc:u0");
    REQUIRE(s.valid());
    // GetProfile (5) of the local user: an IProfile as a move handle
    nxsvc::Reply r = s.call(5, Args().bytes(local_uid(), 16));
    CHECK(r.ok());
    REQUIRE(r.move.size() == 1);
    nxsvc::Session prof = s.sub(r);
    // GetBase (1): ProfileBase
    r = prof.call(1);
    CHECK(r.ok());
    check_profile_base(r);
    // Get (0): ProfileBase in the reply, UserData (0x80 bytes) in the buffer: all zeros
    uint8_t user_data[0x80];
    std::memset(user_data, 0xEE, sizeof user_data);
    r = prof.call(0, Args().buf_b(user_data, sizeof user_data));
    CHECK(r.ok());
    check_profile_base(r);
    bool zero = true;
    for (uint8_t b : user_data) zero = zero && b == 0;
    CHECK(zero);
}

NXTEST(acc_profile_image_is_empty) {
    nxsvc::Env env;
    fresh_profile();
    nxsvc::Session s = nxsvc::connect("acc:u0");
    REQUIRE(s.valid());
    nxsvc::Reply r = s.call(5, Args().bytes(local_uid(), 16));
    REQUIRE(r.ok() && r.move.size() == 1);
    nxsvc::Session prof = s.sub(r);
    // GetImageSize (10): 0 (no picture; see acc.cpp)
    r = prof.call(10);
    CHECK(r.ok());
    CHECK_EQ(r.get<uint32_t>(0), 0u);
    // LoadImage (11): reports 0 and writes nothing into the buffer
    uint8_t image[0x100];
    std::memset(image, 0xEE, sizeof image);
    r = prof.call(11, Args().buf_b(image, sizeof image));
    CHECK(r.ok());
    CHECK_EQ(r.get<uint32_t>(0), 0u);
    bool untouched = true;
    for (uint8_t b : image) untouched = untouched && b == 0xEE;
    CHECK(untouched);
}

NXTEST(acc_get_profile_errors) {
    nxsvc::Env env;
    fresh_profile();
    nxsvc::Session s = nxsvc::connect("acc:u0");
    REQUIRE(s.valid());
    // null uid: ResultInvalidUserId 124-20 = 0x287C (Switchbrew)
    nxsvc::Reply r = s.call(5, Args().bytes(kNullUid, 16));
    CHECK_EQ(r.result, 0x287Cu);
    CHECK(r.move.empty());
    // a uid that is not a user: ResultUserNotExist 124-100 = 0xC87C (Switchbrew)
    r = s.call(5, Args().bytes(kOtherUid, 16));
    CHECK_EQ(r.result, 0xC87Cu);
    CHECK(r.move.empty());
}

NXTEST(acc_try_select_without_interaction) {
    nxsvc::Env env;
    fresh_profile();
    nxsvc::Session s = nxsvc::connect("acc:u0");
    REQUIRE(s.valid());
    // TrySelectUserWithoutInteraction (52), input u8 bool 0 -> the one user
    nxsvc::Reply r = s.call(52, Args().u8(0));
    CHECK(r.ok());
    CHECK(r.out.size() >= 16 && std::memcmp(r.out.data(), local_uid(), 16) == 0);
    // the deprecated id 51 with the network flag set: still the one user (ScarletNX and Ryujinx only log the flag)
    r = s.call(51, Args().u8(1));
    CHECK(r.ok());
    CHECK(r.out.size() >= 16 && std::memcmp(r.out.data(), local_uid(), 16) == 0);
}

NXTEST(acc_registration_permitted_differs_by_service) {
    nxsvc::Env env;
    fresh_profile();
    // IsUserRegistrationRequestPermitted (50): input u64 pid_reserved, output u8. acc:u0 may not register users (0), acc:u1 may (1).
    nxsvc::Session app = nxsvc::connect("acc:u0");
    nxsvc::Session sys = nxsvc::connect("acc:u1");
    REQUIRE(app.valid() && sys.valid());
    nxsvc::Reply r = app.call(50, Args().u64(0));
    CHECK(r.ok());
    CHECK_EQ(r.get<uint8_t>(0), 0u);
    r = sys.call(50, Args().u64(0));
    CHECK(r.ok());
    CHECK_EQ(r.get<uint8_t>(0), 1u);
}

NXTEST(acc_u1_list_qualified_users_is_140) {
    nxsvc::Env env;
    fresh_profile();
    // acc:u1 command 140 is ListQualifiedUsers (Switchbrew's acc:u1 table): the array with the local user first
    nxsvc::Session sys = nxsvc::connect("acc:u1");
    REQUIRE(sys.valid());
    uint8_t buf[0xA0];
    std::memset(buf, 0xEE, sizeof buf);
    nxsvc::Reply r = sys.call(140, Args().buf_c(buf, 0xA0));
    CHECK(r.ok());
    CHECK(std::memcmp(buf, local_uid(), 16) == 0);
    // acc:u0 command 140 is InitializeApplicationInfo (pid + u64 placeholder, no output): accepted
    nxsvc::Session app = nxsvc::connect("acc:u0");
    REQUIRE(app.valid());
    r = app.call(140, Args().u64(0).with_pid());
    CHECK(r.ok());
}

NXTEST(acc_store_and_clear_thumbnail) {
    nxsvc::Env env;
    fresh_profile();
    nxsvc::Session s = nxsvc::connect("acc:u0");
    REQUIRE(s.valid());
    std::vector<uint8_t> thumb(0x24000, 0x55);   // 256x144 RGBA, Switchbrew's size for StoreSaveDataThumbnail
    nxsvc::Reply r = s.call(110, Args().bytes(local_uid(), 16).buf_a(thumb.data(), thumb.size()));
    CHECK(r.ok());
    // a picture of another size: ResultInvalidArrayLength 124-32 = 0x407C
    std::vector<uint8_t> small(0x1000, 0x55);
    r = s.call(110, Args().bytes(local_uid(), 16).buf_a(small.data(), small.size()));
    CHECK_EQ(r.result, 0x407Cu);
    // null uid: ResultInvalidUserId 0x287C
    r = s.call(110, Args().bytes(kNullUid, 16).buf_a(thumb.data(), thumb.size()));
    CHECK_EQ(r.result, 0x287Cu);
    // ClearSaveDataThumbnail (111): the local user yes, null no
    r = s.call(111, Args().bytes(local_uid(), 16));
    CHECK(r.ok());
    r = s.call(111, Args().bytes(kNullUid, 16));
    CHECK_EQ(r.result, 0x287Cu);
}

NXTEST(acc_manager_account_id_and_async_context) {
    nxsvc::Env env;
    fresh_profile();
    nxsvc::Session s = nxsvc::connect("acc:u0");
    REQUIRE(s.valid());
    // GetBaasAccountManagerForApplication (101) of the local user: an IManagerForApplication
    nxsvc::Reply r = s.call(101, Args().bytes(local_uid(), 16));
    CHECK(r.ok());
    REQUIRE(r.move.size() == 1);
    nxsvc::Session mgr = s.sub(r);
    // unknown uid: 0xC87C; null uid: 0x287C
    r = s.call(101, Args().bytes(kOtherUid, 16));
    CHECK_EQ(r.result, 0xC87Cu);
    r = s.call(101, Args().bytes(kNullUid, 16));
    CHECK_EQ(r.result, 0x287Cu);
    // CheckAvailability (0): ok
    r = mgr.call(0);
    CHECK(r.ok());
    // GetAccountId (1): uid[0] ^ uid[1] (ScarletNX's id for a user; the local user has no network account)
    r = mgr.call(1);
    CHECK(r.ok());
    CHECK_EQ(r.get<uint64_t>(0), services::profile().user_uid_hi ^ services::profile().user_uid_lo);
    // EnsureIdTokenCacheAsync (2): an IAsyncContext that is done at once and failed with nifm's 110-300 (0x2586E), no network
    r = mgr.call(2);
    CHECK(r.ok());
    REQUIRE(r.move.size() == 1);
    nxsvc::Session ctx = mgr.sub(r);
    r = ctx.call(2);
    CHECK(r.ok());
    CHECK_EQ(r.get<uint8_t>(0), 1u);   // HasDone
    r = ctx.call(0);                   // GetSystemEvent: a copied handle
    CHECK(r.ok());
    CHECK_EQ(r.copy.size(), 1u);
    r = ctx.call(1);                   // Cancel
    CHECK(r.ok());
    r = ctx.call(3);                   // GetResult
    CHECK_EQ(r.result, 0x2586Eu);
}

NXTEST(acc_state_follows_profile_on_reset) {
    nxsvc::Env env;
    fresh_profile();
    nxsvc::Session s = nxsvc::connect("acc:u0");
    REQUIRE(s.valid());
    uint8_t old_uid[16];
    std::memcpy(old_uid, local_uid(), 16);   // the title's default user, before the profile changes
    // another profile uid (hi 2, lo 3); the open user is taken from the profile when the services are reset
    config_set("account", "uid", "00000000000000020000000000000003");
    services::profile_reload();
    services::reset_all();
    const uint8_t uid2[16] = {0x02, 0, 0, 0, 0, 0, 0, 0, 0x03, 0, 0, 0, 0, 0, 0, 0};
    nxsvc::Reply r = s.call(4);   // GetLastOpenedUser
    CHECK(r.ok());
    CHECK(r.out.size() >= 16 && std::memcmp(r.out.data(), uid2, 16) == 0);
    r = s.call(1, Args().bytes(uid2, 16));   // GetUserExistence of the new user: yes
    CHECK_EQ(r.get<uint8_t>(0), 1u);
    r = s.call(1, Args().bytes(old_uid, 16));   // the old one: no
    CHECK_EQ(r.get<uint8_t>(0), 0u);
    fresh_profile();
}

NXTEST(acc_unknown_command_is_stub) {
    nxsvc::Env env;
    fresh_profile();
    nxsvc::Session s = nxsvc::connect("acc:u0");
    REQUIRE(s.valid());
    // command 0x7777 is not in the table: the framework's stub answer
    nxsvc::Reply r = s.call(0x7777);
    CHECK_EQ(r.result, kernel::kStubService);
}
