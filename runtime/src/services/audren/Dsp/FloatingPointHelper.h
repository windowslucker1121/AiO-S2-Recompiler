// SPDX-License-Identifier: GPL-3.0-or-later
// Port of Ryujinx.Audio/Renderer/Dsp/FloatingPointHelper.cs (Ryubing fork, MIT). Static class -> struct with static members.
// MathF.Round (C#) rounds half to even by default, which is std::nearbyint under the default rounding mode.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

#include "../audren_common.h"

namespace nxrt::audren {

struct FloatingPointHelper {
    static float MultiplyRoundDown(float a, float b) { return RoundDown(a * b); }

    static float RoundDown(float a) { return std::nearbyint(a); }

    static float RoundUp(float a) { return std::nearbyint(a); }

    static float MultiplyRoundUp(float a, float b) { return RoundUp(a * b); }

    static float Pow10(float x) {
        // NOTE: the platform implementation uses Q15 and a LUT for this, we don't.
        // As such, we support the same ranges as the platform to avoid unexpected behaviours.
        if (x >= 0.0f) return 1.0f;
        if (x <= -5.3f) return 0.0f;
        return std::pow(10.0f, x);
    }

    static float Log10(float x) {
        // NOTE: the platform uses an approximation of log10, we don't.
        // As such, we support the same ranges as the platform to avoid unexpected behaviours.
        return std::log10(std::max(x, 1.0e-10f));
    }

    static float MeanSquare(Span<const float> inputs) {
        float res = 0.0f;
        for (float input : inputs) {
            float normInput = input * (1.0f / 32768.0f);
            res += normInput * normInput;
        }
        res /= static_cast<float>(inputs.Length);
        return res;
    }

    // Map decibel to linear.
    static float DecibelToLinear(float db) { return std::pow(10.0f, db / 20.0f); }

    static float DegreesToRadians(float degrees) { return degrees * 3.14159265358979323846f / 180.0f; }

    static float Cos(float value) { return std::cos(DegreesToRadians(value)); }

    static float Sin(float value) { return std::sin(DegreesToRadians(value)); }
};

}  // namespace nxrt::audren
