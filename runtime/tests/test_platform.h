// SPDX-License-Identifier: GPL-3.0-or-later
// Running test code under one platform profile. The test executables register the test game (CMakeLists.txt NXRT_TEST_GAME), so
// game::info().platform is that game's: platform 1 for title E, platform 2 for oot. A test whose expectations belong to one console
// says so with ScopedPlatform (Switch1 or Switch2) instead of relying on the registered game.
//   ScopedPlatform p(Platform::Switch2);   in-process, restored at scope end. Covers what reads game::info().platform on every call
//                                          (core count, GetInfo CoreMask, RandomEntropy subtypes ...).
//   use_platform_for_process(Platform::Switch2);   first thing in an NXTEST_CHILD body, for what caches the platform at its first use
//                                          (kernel::tick_hz is a function-local static, so the parent process cannot switch it).
//   run_child(name)                        runs `<this exe> --child <name>` and returns its exit code.
#pragma once
#include <windows.h>

#include <cstdlib>
#include <string>

#include "nxrt/game.h"

namespace nxtest {

class ScopedPlatform {
public:
    explicit ScopedPlatform(nxrt::game::Platform p) : prev_(nxrt::game::current()), copy_(nxrt::game::info()) {
        copy_.platform = p;
        nxrt::game::set_current(&copy_);
    }
    ~ScopedPlatform() { nxrt::game::set_current(prev_); }
    ScopedPlatform(const ScopedPlatform&) = delete;
    ScopedPlatform& operator=(const ScopedPlatform&) = delete;

private:
    const nxrt::game::Info* prev_;
    nxrt::game::Info copy_;
};

// Switches the registered game's platform for the rest of the process (the tables stay the test game's).
inline void use_platform_for_process(nxrt::game::Platform p) {
    static nxrt::game::Info info;
    info = nxrt::game::info();
    info.platform = p;
    nxrt::game::set_current(&info);
}

inline int run_child(const char* name) {
    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    // cmd /c strips the outer pair of quotes, so the whole command is wrapped in one more pair.
    const std::string cmd = std::string("\"\"") + exe + "\" --child " + name + "\"";
    return std::system(cmd.c_str());
}

}  // namespace nxtest
