// SPDX-License-Identifier: GPL-3.0-or-later
// NVN front-end shell (declarations in include/nxrt/nvn.h): nvnBootstrapLoader / nvnDeviceGetProcAddress and a logging stub for
// every entry point of the generated table. The stub/logging layout follows the idea in smo-recomp runtime/gfx/nvn_stub.c
// (log the first calls with x0..x3 and the caller, count the rest); the argument decoding by signature class is new.
#include "nxrt/nvn.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

#include "nxrt/config.h"
#include "nxrt/dispatch.h"
#include "nxrt/log.h"
#include "nxrt/gaps.h"

namespace nxrt::nvn {
namespace {

constexpr const char* kTag = "nvn";

#define NVN_ENTRY(name, ret, args, src) {name, ret, args, src},
const Entry g_entries[] = {
#include "nvn/nvn_table.inc"
};
#undef NVN_ENTRY
constexpr size_t kEntryCount = sizeof(g_entries) / sizeof(g_entries[0]);

#define NVN_STRUCT(name, size, src) {name, size, src},
const StructSize g_structs[] = {
#include "nvn/nvn_sizes.inc"
};
#undef NVN_STRUCT
constexpr size_t kStructCount = sizeof(g_structs) / sizeof(g_structs[0]);

std::atomic<uint64_t> g_calls[kEntryCount];
std::atomic<nxrt::GapCounter*> g_stub_gaps[kEntryCount];   // made on an entry's first stub call (per call: one atomic add)
std::atomic<bool> g_impl[kEntryCount];
std::atomic<uint64_t> g_stub_calls{0};
std::atomic<uint64_t> g_unknown_calls{0};
// Calls of each unknown proc by name: the first log_first_calls calls of every name are logged, not of all unknown procs together.
std::mutex g_unknown_mu;
std::unordered_map<std::string, uint64_t> g_unknown_by_name;
// NVN2-only procs the back end implements (names that are not in the NVN1 table): proc_address hands these out instead of the logging stub.
std::mutex g_extra_mu;
std::unordered_map<std::string, HleFn> g_extra_impl;
std::atomic<int> g_unknown_policy{-1};   // -1: not read from the config yet
std::atomic<bool> g_installed{false};
std::mutex g_install_mu;

const std::unordered_map<std::string, int>& index_map() {
    static const std::unordered_map<std::string, int> m = [] {
        std::unordered_map<std::string, int> r;
        r.reserve(kEntryCount * 2);
        for (size_t i = 0; i < kEntryCount; ++i) r.emplace(g_entries[i].name, static_cast<int>(i));
        return r;
    }();
    return m;
}

int64_t log_first_calls() {
    static const int64_t n = config_get_int("nvn", "log_first_calls", 3);
    return n;
}

UnknownPolicy unknown_policy() {
    int p = g_unknown_policy.load();
    if (p < 0) {
        const std::string v = config_get_str("nvn", "unknown", "stub");
        p = (v == "null" || v == "NULL") ? static_cast<int>(UnknownPolicy::Null) : static_cast<int>(UnknownPolicy::Stub);
        g_unknown_policy.store(p);
    }
    return static_cast<UnknownPolicy>(p);
}

unsigned long long u64(uint64_t v) { return static_cast<unsigned long long>(v); }

// The logging stub, shared by every entry point: the thunk's name says which one was called.
void stub_handler(::Ctx* c) {
    const char* name = hle_current_name();
    const int idx = name ? entry_index(name) : -1;
    uint64_t n = 0;
    if (idx >= 0) {
        n = g_calls[idx].fetch_add(1, std::memory_order_relaxed) + 1;
        g_stub_calls.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_unknown_calls.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lk(g_unknown_mu);
        n = ++g_unknown_by_name[name ? name : "?"];
    }
    if (idx >= 0) {
        nxrt::GapCounter* gc = g_stub_gaps[idx].load(std::memory_order_acquire);
        if (!gc) {
            auto* made = new nxrt::GapCounter("nvn-proc", "logging stub called", g_entries[idx].name);   // never freed (registered)
            gc = g_stub_gaps[idx].compare_exchange_strong(gc, made, std::memory_order_acq_rel) ? made : gc;
            // a lost race leaks one registered counter that never counts: harmless
        }
        gc->hit();
    }
    if (idx < 0) nxrt::gap("nvn-proc", "entry point without an implementation called", name ? name : "?");
    const uint64_t caller = lr(c) - 4;   // lr is the return address: the BLR is the instruction before it
    if (static_cast<int64_t>(n) <= log_first_calls()) {
        if (idx >= 0) {
            const Entry& e = g_entries[idx];
            NXLOG_I(kTag, "STUB %s(%s) returns 0, caller=0x%llx, call #%llu", e.name, describe_args(e, c).c_str(), u64(caller), u64(n));
        } else {
            NXLOG_W(kTag, "UNKNOWN NVN PROC %s(x0=0x%llx x1=0x%llx x2=0x%llx x3=0x%llx) returns 0, caller=0x%llx, call #%llu", name ? name : "?",
                    u64(reg(c, 0)), u64(reg(c, 1)), u64(reg(c, 2)), u64(reg(c, 3)), u64(caller), u64(n));
        }
    } else if (idx >= 0) {
        NXLOG_T(kTag, "STUB %s(%s) caller=0x%llx call #%llu", g_entries[idx].name, describe_args(g_entries[idx], c).c_str(), u64(caller), u64(n));
    }
    reg(c, 0) = 0;
    c->v[0].d[0] = 0;
    c->v[0].d[1] = 0;
}

// void* nvnBootstrapLoader(const char* name)
void bootstrap_loader(::Ctx* c) {
    const char* name = reinterpret_cast<const char*>(static_cast<uintptr_t>(reg(c, 0)));
    const uint64_t a = proc_address(name);
    NXLOG_D(kTag, "nvnBootstrapLoader(\"%s\") -> 0x%llx, caller=0x%llx", name ? name : "(null)", u64(a), u64(lr(c) - 4));
    reg(c, 0) = a;
}

// void* nvnDeviceGetProcAddress(const NVNdevice* device, const char* name)
void device_get_proc_address(::Ctx* c) {
    const char* name = reinterpret_cast<const char*>(static_cast<uintptr_t>(reg(c, 1)));
    const uint64_t a = proc_address(name);
    NXLOG_D(kTag, "nvnDeviceGetProcAddress(\"%s\") -> 0x%llx, caller=0x%llx", name ? name : "(null)", u64(a), u64(lr(c) - 4));
    reg(c, 0) = a;
}

}  // namespace

// ---- table ------------------------------------------------------------------------------------------------------------
size_t entry_count() { return kEntryCount; }

const Entry& entry_at(size_t index) { return g_entries[index < kEntryCount ? index : 0]; }

int entry_index(const char* name) {
    if (!name) return -1;
    const auto& m = index_map();
    auto it = m.find(name);
    return it == m.end() ? -1 : it->second;
}

const StructSize* struct_sizes(size_t* count) {
    if (count) *count = kStructCount;
    return g_structs;
}

uint32_t struct_size(const char* name) {
    if (!name) return 0;
    for (const StructSize& s : g_structs)
        if (!std::strcmp(s.name, name)) return s.size;
    return 0;
}

// ---- installation -----------------------------------------------------------------------------------------------------
void install() {
    std::lock_guard<std::mutex> lk(g_install_mu);
    if (g_installed.load()) return;
    for (size_t i = 0; i < kEntryCount; ++i) {
        const char* name = g_entries[i].name;
        if (!std::strcmp(name, "nvnBootstrapLoader")) hle_register(name, bootstrap_loader);
        else if (!std::strcmp(name, "nvnDeviceGetProcAddress")) hle_register(name, device_get_proc_address);
        else hle_register(name, stub_handler);
    }
    // These two are real implementations from the start.
    g_impl[entry_index("nvnBootstrapLoader")] = true;
    g_impl[entry_index("nvnDeviceGetProcAddress")] = true;
    g_installed.store(true);
    NXLOG_I(kTag, "NVN shell installed: %zu entry points (2 implemented, %zu logging stubs), %zu object sizes", kEntryCount, kEntryCount - 2, kStructCount);
}

void set_unknown_policy(UnknownPolicy p) { g_unknown_policy.store(static_cast<int>(p)); }

uint64_t proc_address(const char* name) {
    if (!name || !*name) return 0;
    if (!g_installed.load()) install();
    if (entry_index(name) >= 0) return hle_address(name);
    {
        HleFn fn = nullptr;
        {
            std::lock_guard<std::mutex> lk(g_extra_mu);
            auto it = g_extra_impl.find(name);
            if (it != g_extra_impl.end()) fn = it->second;
        }
        if (fn) return hle_register(name, fn);
    }
    if (std::strncmp(name, "nvn", 3) == 0 && unknown_policy() == UnknownPolicy::Stub) {
        NXLOG_W(kTag, "UNKNOWN NVN PROC requested by name: %s (not in the table): handing out a logging stub", name);
        return hle_register(name, stub_handler);
    }
    NXLOG_W(kTag, "NVN proc %s requested: not in the table, answering null", name);
    return 0;
}

bool register_impl(const char* name, HleFn fn) {
    const int idx = entry_index(name);
    if (idx < 0 || !fn) return false;
    if (!g_installed.load()) install();
    hle_register(name, fn);
    g_impl[idx] = true;
    return true;
}

bool register_extra_impl(const char* name, HleFn fn) {
    if (!name || !fn || entry_index(name) >= 0) return false;
    std::lock_guard<std::mutex> lk(g_extra_mu);
    g_extra_impl[name] = fn;
    return true;
}

bool is_implemented(const char* name) {
    const int idx = entry_index(name);
    return idx >= 0 && g_impl[idx].load();
}

size_t implemented_count() {
    size_t n = 0;
    for (size_t i = 0; i < kEntryCount; ++i) n += g_impl[i].load() ? 1 : 0;
    return n;
}

// ---- reporting --------------------------------------------------------------------------------------------------------
uint64_t entry_calls(size_t index) { return index < kEntryCount ? g_calls[index].load() : 0; }
uint64_t stub_calls_total() { return g_stub_calls.load(); }

size_t stubs_called_distinct() {
    size_t n = 0;
    for (size_t i = 0; i < kEntryCount; ++i) n += (g_calls[i].load() > 0 && !g_impl[i].load()) ? 1 : 0;
    return n;
}

std::vector<CallStat> called_entries() {
    std::vector<CallStat> v;
    for (size_t i = 0; i < kEntryCount; ++i)
        if (g_calls[i].load() > 0) v.push_back({&g_entries[i], g_calls[i].load(), g_impl[i].load()});
    std::sort(v.begin(), v.end(), [](const CallStat& a, const CallStat& b) { return a.calls > b.calls; });
    return v;
}

std::string describe_args(const Entry& e, const ::Ctx* c) {
    std::string out;
    char buf[64];
    if (e.args[0] == '?') {
        for (int i = 0; i < 8; ++i) {
            std::snprintf(buf, sizeof buf, "%sx%d=0x%llx", i ? " " : "", i, u64(c->x[i]));
            out += buf;
        }
        return out;
    }
    int ni = 0, nf = 0;
    for (const char* p = e.args; *p; ++p) {
        if (!out.empty()) out += ' ';
        switch (*p) {
            case 'f':
            case 'd': {
                if (nf >= 8) { out += "stack"; break; }
                if (*p == 'f') std::snprintf(buf, sizeof buf, "f=%g", static_cast<double>(c->v[nf].f32[0]));
                else std::snprintf(buf, sizeof buf, "d=%g", c->v[nf].f64[0]);
                ++nf;
                out += buf;
                break;
            }
            default: {
                if (ni >= 8) { out += "stack"; break; }
                const uint64_t x = c->x[ni++];
                if (*p == 'i') std::snprintf(buf, sizeof buf, "i=%d", static_cast<int32_t>(static_cast<uint32_t>(x)));
                else if (*p == 'S') std::snprintf(buf, sizeof buf, "S=0x%llx", u64(x));
                else std::snprintf(buf, sizeof buf, "%c=0x%llx", *p, u64(x));
                out += buf;
            }
        }
    }
    return out;
}

void reset_counters() {
    for (size_t i = 0; i < kEntryCount; ++i) g_calls[i] = 0;
    g_stub_calls = 0;
    g_unknown_calls = 0;
    std::lock_guard<std::mutex> lk(g_unknown_mu);
    g_unknown_by_name.clear();
}

}  // namespace nxrt::nvn
