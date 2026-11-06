// SPDX-License-Identifier: GPL-3.0-or-later
// Tiny test framework: groups of named cases in one executable. Usage in a test source (one group per file):
//   NXTEST_GROUP(mymodule)            // first line, no leading space: CMake reads it to create the CTest entry
//   NXTEST(does_thing) { CHECK_EQ(1 + 1, 2); }
// Child-process modes (for crash/watchdog self-tests): NXTEST_CHILD(name) { ... } runs on `nxrt_tests --child name`.
// A group fixture (after NXTEST_GROUP): NXTEST_GROUP_FIXTURE(before, after) runs before() ahead of every case of the group and after() once
// the case has returned (a failed REQUIRE returns too), e.g. to register data the group's checks are written against.
#pragma once
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace nxtest {
struct Case {
    const char* group;
    const char* name;
    void (*fn)();
};
struct Child {
    const char* name;
    int (*fn)(int argc, char** argv);
};
std::vector<Case>& cases();
std::vector<Child>& children();
void fail(const char* file, int line, const char* expr, const std::string& detail);
void skip(const char* why);  // mark the running case as skipped (counts as pass, printed as SKIP)
struct Registrar {
    Registrar(const char* g, const char* n, void (*f)()) { cases().push_back({g, n, f}); }
};
struct ChildRegistrar {
    ChildRegistrar(const char* n, int (*f)(int, char**)) { children().push_back({n, f}); }
};
struct GroupFixture {
    const char* group;
    void (*before)();
    void (*after)();
};
std::vector<GroupFixture>& group_fixtures();
struct GroupFixtureRegistrar {
    GroupFixtureRegistrar(const char* g, void (*b)(), void (*a)()) { group_fixtures().push_back({g, b, a}); }
};
template <class T>
std::string to_s(const T& v) {
    if constexpr (std::is_pointer_v<T>) {
        char b[32];
        std::snprintf(b, sizeof b, "%p", (const void*)v);
        return b;
    } else if constexpr (std::is_integral_v<T> || std::is_enum_v<T>) {
        char b[48];
        std::snprintf(b, sizeof b, "%lld (0x%llx)", (long long)v, (unsigned long long)v);
        return b;
    } else if constexpr (std::is_floating_point_v<T>) {
        return std::to_string(v);
    } else {
        return std::string(v);
    }
}
}  // namespace nxtest

#define NXTEST_GROUP(name) static constexpr const char* kNxGroup = #name;
#define NXTEST_GROUP_FIXTURE(before, after) static ::nxtest::GroupFixtureRegistrar nxtest_group_fixture_reg(kNxGroup, (before), (after));
#define NXTEST(name)                                                                  \
    static void nxtest_##name();                                                      \
    static ::nxtest::Registrar nxtest_reg_##name(kNxGroup, #name, &nxtest_##name);    \
    static void nxtest_##name()
#define NXTEST_CHILD(name)                                                            \
    static int nxchild_##name(int argc, char** argv);                                 \
    static ::nxtest::ChildRegistrar nxchild_reg_##name(#name, &nxchild_##name);       \
    static int nxchild_##name(int argc, char** argv)

#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) ::nxtest::fail(__FILE__, __LINE__, #cond, "");                   \
    } while (0)
#define CHECK_MSG(cond, msg)                                                          \
    do {                                                                              \
        if (!(cond)) ::nxtest::fail(__FILE__, __LINE__, #cond, (msg));                \
    } while (0)
#define CHECK_EQ(a, b)                                                                \
    do {                                                                              \
        auto&& nx_a = (a);                                                            \
        auto&& nx_b = (b);                                                            \
        if (!(nx_a == nx_b))                                                          \
            ::nxtest::fail(__FILE__, __LINE__, #a " == " #b,                          \
                           "left " + ::nxtest::to_s(nx_a) + ", right " + ::nxtest::to_s(nx_b)); \
    } while (0)
#define REQUIRE(cond)                                                                 \
    do {                                                                              \
        if (!(cond)) {                                                                \
            ::nxtest::fail(__FILE__, __LINE__, #cond, "(required)");                  \
            return;                                                                   \
        }                                                                             \
    } while (0)
