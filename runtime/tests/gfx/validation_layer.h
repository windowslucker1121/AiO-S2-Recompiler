// SPDX-License-Identifier: GPL-3.0-or-later
// The Khronos validation layer (VK_LAYER_KHRONOS_validation, part of the Vulkan SDK) behind the "zero validation errors" checks of the GPU
// tests. Without it the device runs unvalidated: the tests still check their pictures, results and allocations, and the zero error counts
// mean nothing, so a test says that once instead of failing (development machines of this repository need not have the SDK).
//   if (!validation_layer_live(*dev)) ...      true when the layer is on; prints the note once per process otherwise
#pragma once
#include <cstdio>

#include "nxrt/gfx.h"

namespace gfxtest {

inline bool validation_layer_live(const nxrt::gfx::Device& dev) {
    if (dev.validation_enabled()) return true;
    static bool said = false;
    if (!said) {
        std::printf("    note: the Khronos validation layer is not installed (Vulkan SDK): the zero-validation-error checks prove nothing here\n");
        said = true;
    }
    return false;
}

}  // namespace gfxtest
