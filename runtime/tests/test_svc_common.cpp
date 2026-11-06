// SPDX-License-Identifier: GPL-3.0-or-later
// Service library framework: registration, events, shared memory, the title profile, the test client.
#include "svc_test_util.h"

#include <algorithm>

#include "nxrt/config.h"
#include "nxrt/game.h"

NXTEST_GROUP(svc_common)

using namespace nxrt;

NXTEST(common_connect_through_sm) {
    nxsvc::Env env;
    // A service nobody implements is still reachable as a stub (the old behaviour), a registered one is not a stub.
    nxsvc::Session s = nxsvc::connect("no:such");
    CHECK(s.valid());
    nxsvc::Reply r = s.call(0);
    CHECK_EQ(r.result, kernel::kStubService);
}

NXTEST(common_event_wait) {
    nxsvc::Env env;
    services::ServiceEvent ev;
    const uint32_t h = ev.new_handle();
    REQUIRE(h != 0);
    int32_t idx = -1;
    CHECK_EQ(kernel::wait_synchronization(&h, 1, 0, &idx), kernel::kTimedOut);
    ev.signal();
    CHECK_EQ(kernel::wait_synchronization(&h, 1, 0, &idx), 0u);
    CHECK_EQ(idx, 0);
    ev.clear();
    CHECK_EQ(kernel::wait_synchronization(&h, 1, 0, &idx), kernel::kTimedOut);
    // A second handle sees the same state.
    const uint32_t h2 = ev.new_handle();
    ev.signal();
    CHECK_EQ(kernel::wait_synchronization(&h2, 1, 0, &idx), 0u);
    kernel::handle_close(h);
    kernel::handle_close(h2);
}

NXTEST(common_shared_memory_map) {
    nxsvc::Env env;
    services::ServiceSharedMemory sm(0x3000, "test");
    REQUIRE(sm.ok());
    CHECK_EQ(sm.size(), 0x3000u);
    std::memset(reinterpret_cast<void*>(sm.host()), 0xAB, 16);
    const uint32_t h = sm.new_handle();
    REQUIRE(h != 0);
    auto obj = kernel::handle_as<kernel::KSharedMemory>(h);
    REQUIRE(obj != nullptr);
    CHECK_EQ(obj->size, 0x3000u);
    CHECK_EQ(reinterpret_cast<uint8_t*>(sm.host())[3], 0xAB);
    kernel::handle_close(h);
}

// With nothing in the ini the profile is the registered title's facts (nxrt/game.h Info and its title_defaults: main.npdm, control.nacp).
NXTEST(common_profile_defaults) {
    nxsvc::Env env;
    config_clear();
    services::profile_reload();
    const services::TitleProfile& p = services::profile();
    const game::Info& g = game::info();
    if (!g.title_defaults) {
        nxtest::skip("no test game registered (NXRT_TEST_GAME): no title facts to compare");
        return;
    }
    services::TitleProfile want;
    g.title_defaults(want);
    CHECK(g.program_id != 0);
    CHECK_EQ(p.program_id, g.program_id);
    CHECK_EQ(want.program_id, g.program_id);
    CHECK_EQ(p.account_save_size, want.account_save_size);
    CHECK_EQ(p.account_save_journal, want.account_save_journal);
    CHECK_EQ(p.main_thread_stack, want.main_thread_stack);
    CHECK_EQ(p.system_resource_size, want.system_resource_size);
    CHECK_EQ(p.firmware, want.firmware);
    CHECK(p.docked);
    // the supported languages (control.nacp; American English alone when the title names none)
    const uint32_t langs = want.supported_languages ? want.supported_languages : 1u << static_cast<uint32_t>(services::Language::AmericanEnglish);
    CHECK_EQ(p.supported_languages, langs);
    // the language with none set is the Windows display language when the title has it, else American English: one the title supports
    CHECK((p.supported_languages >> static_cast<uint32_t>(p.language)) & 1u);
    CHECK_EQ(services::language_code_u64(services::Language::AmericanEnglish), 0x53552d6e65ull);   // "en-US"
}

// A title that supports only American English answers it whatever the machine's display language is.
NXTEST(common_profile_language_falls_back_to_american_english) {
    nxsvc::Env env;
    config_clear();
    config_set("title", "supported_languages", "AmericanEnglish");
    services::profile_reload();
    CHECK_EQ(static_cast<uint32_t>(services::profile().language), static_cast<uint32_t>(services::Language::AmericanEnglish));
    CHECK_EQ(services::profile().supported_languages, 1u << static_cast<uint32_t>(services::Language::AmericanEnglish));
    config_clear();
    services::profile_reload();
}

NXTEST(common_profile_overrides) {
    nxsvc::Env env;
    config_clear();
    config_set("system", "language", "Japanese");
    config_set("system", "region", "Europe");
    config_set("am", "operation_mode", "handheld");
    config_set("account", "uid", "00000000000000010000000000000002");
    services::profile_reload();
    const services::TitleProfile& p = services::profile();
    CHECK_EQ(static_cast<uint32_t>(p.language), static_cast<uint32_t>(services::Language::Japanese));
    CHECK_EQ(p.region, 2);
    CHECK(!p.docked);
    CHECK_EQ(p.user_uid_hi, 1ull);
    CHECK_EQ(p.user_uid_lo, 2ull);
    config_clear();
    services::profile_reload();
}

NXTEST(common_program_id_default) {
    config_clear();
    CHECK_EQ(services::program_id_now(), game::info().program_id);   // the registered title's id (oot 0x0400486024DEC000)
    config_set("process", "program_id", "0x1234");
    CHECK_EQ(services::program_id_now(), 0x1234ull);
    config_clear();
}
