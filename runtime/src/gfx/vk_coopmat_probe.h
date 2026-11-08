// SPDX-License-Identifier: MIT
#pragma once
#include <string>

namespace nxrt::gfx {
class Device;

// Whether this device runs the shader translator's cooperative-matrix HMMA (tools/shaders/ampere/frontend/AmpereCoopMat.cs) as the
// tensor core would: the device lists 16x8x16 float16 subgroup matrices with float16 and float32 accumulators, and one dispatch of
// the translator's own HMMA.16816.F16 module (vk_coopmat_probe_spv.h, made by `hwcheck --coopmat --header`) gives the expected
// result bit for bit. That module builds its matrices from each lane's SASS fragment registers in the translator's element order,
// so a device that hands out the matrix elements differently fails here, and the shader cache keeps the scalar modules.
// Needs the device's command pool (Device::submit_once). False with the reason in *why.
bool coopmat_hmma_probe(Device& dev, std::string* why);

// Process-wide: the last device made passed the probe. The shader cache then loads <hash>.cm.spv in place of <hash>.spv where the
// harness wrote one (nvn_shaders.cpp, spv_file).
bool coopmat_hmma_enabled();
void set_coopmat_hmma_enabled(bool on);

// Process-wide: the last device made has Features::float16_shaders. The shader cache then loads <hash>.f16.spv (packed-half
// arithmetic in float16, AmpereCoopMat.cs) in place of <hash>.spv where the harness wrote one.
bool float16_shaders_enabled();
void set_float16_shaders_enabled(bool on);
}  // namespace nxrt::gfx
