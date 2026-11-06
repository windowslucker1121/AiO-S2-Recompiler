// SPDX-License-Identifier: GPL-3.0-or-later
// Bring-up probe ([diag] trace_sites / NXRT_DIAG_TRACE_SITES = "0x7100ba3bd8,0x7100ba3db0,..."): a mid hook at each listed address
// logs x0-x3, x19-x20, lr and sp the first [diag] trace_limit (default 50) times per site,
// then on every power-of-two hit (with the hit number), so a site that keeps running stays visible. [diag] trace_peek (NXRT_DIAG_TRACE_PEEK)
// = "x1+0x30,x0+0x18,sp+0x10" adds the 64-bit words at those register-relative addresses to every line. The lifter emits a site only where main.sites asks for
// one (`midhook <addr>`, or `entryhook <addr>` at a function start: only that shard recompiles), so list the same addresses there and re-lift. Answers "which branch did the game take" without a debugger.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "nxrt/config.h"
#include "nxrt/hooks.h"
#include "nxrt/log.h"

namespace nxrt {

namespace {

// [diag] trace_peek: register index (31 = sp) and offset of each extra word to log
std::vector<std::pair<int, int64_t>>& peeks() {
    static std::vector<std::pair<int, int64_t>> v = [] {
        std::vector<std::pair<int, int64_t>> out;
        const std::string list = config_get_str("diag", "trace_peek", "");
        for (size_t i = 0; i < list.size();) {
            size_t j = list.find_first_of(", ", i);
            if (j == std::string::npos) j = list.size();
            const std::string it = list.substr(i, j - i);
            i = j + 1;
            if (it.size() < 2) continue;
            int reg = -1;
            size_t k = 0;
            if (it.rfind("sp", 0) == 0) { reg = 31; k = 2; }
            else if (it[0] == 'x') { reg = static_cast<int>(std::strtol(it.c_str() + 1, nullptr, 10)); k = it.find_first_of("+-"); }
            if (reg < 0 || reg > 31) continue;
            const int64_t off = (k != std::string::npos && k < it.size()) ? std::strtoll(it.c_str() + k, nullptr, 0) : 0;
            out.emplace_back(reg, off);
        }
        return out;
    }();
    return v;
}

int trace_site(::Ctx* c, uint64_t addr, void* user) {
    auto* n = static_cast<unsigned*>(user);
    static const unsigned limit = static_cast<unsigned>(config_get_int("diag", "trace_limit", 50));
    const unsigned hit = ++*n;
    // [x0], the first word x0 points at (a vtable when x0 is an object), when x0 looks like a guest address
    const uint64_t x0 = c->x[0];
    const uint64_t m0 = (x0 >= 0x7000000000ull && x0 < 0x8000000000ull && (x0 & 7) == 0) ? *reinterpret_cast<const uint64_t*>(x0) : 0;
    auto peek = [](uint64_t a) -> uint64_t {
        return (a >= 0x7000000000ull && a < 0x8000000000ull && (a & 7) == 0) ? *reinterpret_cast<const uint64_t*>(a) : 0;
    };
    const uint64_t x0_38 = peek(c->x[0] + 0x38);
    const uint64_t x20a = peek(c->x[20]), x20b = peek(c->x[20] + 8), x20c = peek(c->x[20] + 16);
    if (hit <= limit || (hit & (hit - 1)) == 0)
        NXLOG_I("trace", "site %llx #%u: x0 %llx [x0] %llx [x0+38] %llx x1 %llx x2 %llx x3 %llx x19 %llx x20 %llx [x20] %llx %llx %llx lr %llx sp %llx", (unsigned long long)addr, hit,
                (unsigned long long)c->x[0], (unsigned long long)m0, (unsigned long long)x0_38, (unsigned long long)c->x[1], (unsigned long long)c->x[2], (unsigned long long)c->x[3],
                (unsigned long long)c->x[19], (unsigned long long)c->x[20], (unsigned long long)x20a, (unsigned long long)x20b, (unsigned long long)x20c, (unsigned long long)c->x[30], (unsigned long long)c->x[31]);
    if ((hit <= limit || (hit & (hit - 1)) == 0) && !peeks().empty()) {
        std::string line;
        char buf[64];
        for (const auto& [reg, off] : peeks()) {
            std::snprintf(buf, sizeof buf, " %s%d%+lld=%llx", reg == 31 ? "sp" : "x", reg == 31 ? 0 : reg, static_cast<long long>(off),
                          static_cast<unsigned long long>(peek(c->x[reg] + static_cast<uint64_t>(off))));
            line += buf;
        }
        NXLOG_I("trace", "site %llx #%u peek:%s", (unsigned long long)addr, hit, line.c_str());
    }
    return 0;   // not handled: the instruction runs as lifted
}

}  // namespace

void trace_sites_install() {
    const std::string list = config_get_str("diag", "trace_sites", "");
    size_t n = 0;
    for (size_t i = 0; i < list.size();) {
        size_t j = list.find_first_of(", ", i);
        if (j == std::string::npos) j = list.size();
        if (j > i) {
            const uint64_t a = std::strtoull(list.substr(i, j - i).c_str(), nullptr, 0);
            if (a) {   // a midhook site or a function with an entryhook site (main.sites): whichever the lifted code has calls it
                unsigned* hits = new unsigned(0);
                hook_add_mid(a, trace_site, hits);
                hook_add_entry(a, trace_site, hits);
                ++n;
            }
        }
        i = j + 1;
    }
    if (n) NXLOG_I("trace", "%zu trace sites armed ([diag] trace_sites)", n);
}

}  // namespace nxrt
