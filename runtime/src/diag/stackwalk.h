// SPDX-License-Identifier: GPL-3.0-or-later
// Internal: host stack capture without allocation (safe to call while another thread is suspended).
#pragma once
#include <windows.h>

#include <cstddef>
#include <cstdint>

namespace nxrt::detail {

// Walks the host stack described by *ctx with RtlLookupFunctionEntry/RtlVirtualUnwind. Writes up to `max` return
// addresses (the first is ctx->Rip). Does not modify *ctx. Never allocates.
size_t capture_stack(const CONTEXT* ctx, uint64_t* frames, size_t max);
size_t capture_stack_guarded(const CONTEXT* ctx, uint64_t* frames, size_t max);   // a fault while walking: 0 frames

}  // namespace nxrt::detail
