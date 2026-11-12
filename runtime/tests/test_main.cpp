// SPDX-License-Identifier: GPL-3.0-or-later
#include "nxrt/diag.h"
#include "test_framework.h"
#include "project_root.h"

#include <cstdlib>
#include <cstring>
#include <set>

#ifdef NXRT_TEST_GAME_REGISTER
void NXRT_TEST_GAME_REGISTER();     // the test game (CMakeLists.txt NXRT_TEST_GAME): the tests run with its tables registered (docs/GAME_BOUNDARY.md)
#endif

namespace nxtest {
std::vector<Case>& cases() {
    static std::vector<Case> v;
    return v;
}
std::vector<Child>& children() {
    static std::vector<Child> v;
    return v;
}
std::vector<GroupFixture>& group_fixtures() {
    static std::vector<GroupFixture> v;
    return v;
}
static thread_local int t_failures = 0;
static thread_local bool t_skipped = false;
void fail(const char* file, int line, const char* expr, const std::string& detail) {
    std::printf("    FAILED %s:%d: %s %s\n", file, line, expr, detail.c_str());
    std::fflush(stdout);
    t_failures++;
}
void skip(const char* why) {
    std::printf("    SKIP: %s\n", why);
    t_skipped = true;
}
}  // namespace nxtest

int main(int argc, char** argv) {
    using namespace nxtest;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    // NXRT_TEST_CRASH_REPORT=<file>: a fault in a case writes the runtime's crash report there (host stack, threads) before the process ends.
    if (const char* cr = std::getenv("NXRT_TEST_CRASH_REPORT")) {
        static nxrt::CrashOptions co;
        co.report_path = cr;
        nxrt::crash_install(co);
    }
#ifdef NXRT_TEST_GAME_REGISTER
    NXRT_TEST_GAME_REGISTER();
#endif
    // The runtime opens its shader cache only from [nvn] shader_cache / NXRT_NVN_SHADER_CACHE (no work-folder fallback),
    // so the test process points it at the game's cache unless the environment already sets it.
    if (!std::getenv("NXRT_NVN_SHADER_CACHE") && !project_root().empty()) _putenv_s("NXRT_NVN_SHADER_CACHE", project_path("work/shader_cache/spirv").c_str());
    if (argc >= 2 && !std::strcmp(argv[1], "--list")) {
        std::set<std::string> groups;
        for (auto& c : cases()) groups.insert(c.group);
        for (auto& g : groups) std::printf("%s\n", g.c_str());
        return 0;
    }
    if (argc >= 3 && !std::strcmp(argv[1], "--child")) {
        for (auto& c : children())
            if (!std::strcmp(c.name, argv[2])) return c.fn(argc - 3, argv + 3);
        std::fprintf(stderr, "no child mode %s\n", argv[2]);
        return 90;
    }
    const char* want = argc >= 2 ? argv[1] : "--all";
    bool all = !std::strcmp(want, "--all");
    int passed = 0, failed = 0, skipped = 0, ran = 0;
    for (auto& c : cases()) {
        std::string full = std::string(c.group) + "." + c.name;
        if (!all && full != want && std::strcmp(c.group, want) != 0) continue;
        ran++;
        t_failures = 0;
        t_skipped = false;
        std::printf("[ RUN  ] %s\n", full.c_str());
        for (const GroupFixture& f : group_fixtures())
            if (f.before && !std::strcmp(f.group, c.group)) f.before();
        c.fn();
        for (auto it = group_fixtures().rbegin(); it != group_fixtures().rend(); ++it)
            if (it->after && !std::strcmp(it->group, c.group)) it->after();
        if (t_failures) {
            failed++;
            std::printf("[ FAIL ] %s (%d check(s) failed)\n", full.c_str(), t_failures);
        } else if (t_skipped) {
            skipped++;
            std::printf("[ SKIP ] %s\n", full.c_str());
        } else {
            passed++;
            std::printf("[  OK  ] %s\n", full.c_str());
        }
    }
    if (!ran) {
        std::printf("no test matches '%s'\n", want);
        return 2;
    }
    std::printf("SUMMARY %s: %d passed, %d failed, %d skipped (of %d)\n", want, passed, failed, skipped, ran);
    return failed ? 1 : 0;
}
