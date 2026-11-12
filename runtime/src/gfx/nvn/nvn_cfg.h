// SPDX-License-Identifier: GPL-3.0-or-later
// Config values read once, without a guard on the hot path.
//
// `static const bool on = config_get_bool(...)` in a function is a thread-safe local static: every call checks its guard against a
// thread-local epoch (several loads, one through TLS). The render thread asks such switches hundreds of thousands of times per frame; the
// cpuperf_ab "off" windows showed what it costs when one more guarded static sits in a hot check (ab_mask(): ~0.6 ms per frame in the
// village scene). These keep the value in a constant-initialized atomic (no guard): the first call reads the config, a race only reads it
// twice (the same answer).
#pragma once
#include <atomic>
#include <cstdint>

#include "nxrt/config.h"

namespace nxrt::nvn::be {

inline bool cfg_cached_bool(std::atomic<int8_t>& v, const char* s, const char* k, bool d) {
    int8_t x = v.load(std::memory_order_relaxed);
    if (x < 0) [[unlikely]] {
        x = nxrt::config_get_bool(s, k, d) ? 1 : 0;
        v.store(x, std::memory_order_relaxed);
    }
    return x != 0;
}
constexpr int64_t kCfgUnread = INT64_MIN + 12345;
inline int64_t cfg_cached_int(std::atomic<int64_t>& v, const char* s, const char* k, int64_t d) {
    int64_t x = v.load(std::memory_order_relaxed);
    if (x == kCfgUnread) [[unlikely]] {
        x = nxrt::config_get_int(s, k, d);
        v.store(x, std::memory_order_relaxed);
    }
    return x;
}

}  // namespace nxrt::nvn::be

// The value of [s] k (default d), read once per call site.
#define NX_CFG_BOOL(s, k, d) ([]() -> bool { static std::atomic<int8_t> v_{-1}; return ::nxrt::nvn::be::cfg_cached_bool(v_, s, k, d); }())
#define NX_CFG_INT(s, k, d) ([]() -> int64_t { static std::atomic<int64_t> v_{::nxrt::nvn::be::kCfgUnread}; return ::nxrt::nvn::be::cfg_cached_int(v_, s, k, d); }())
namespace nxrt::nvn::be {
template <typename T> struct OnceVal { std::atomic<bool> ready{false}; T v{}; };
}
// Any trivially copyable value computed once from the config (the expression may only use the config and constants: no locals).
#define NX_ONCE(T, ...) ([]() -> T { static ::nxrt::nvn::be::OnceVal<T> c_; if (!c_.ready.load(std::memory_order_acquire)) [[unlikely]] { c_.v = (__VA_ARGS__); c_.ready.store(true, std::memory_order_release); } return c_.v; }())
