// SPDX-License-Identifier: GPL-3.0-or-later
// Picture presets and limits (include/nxrt/extras.h): pure functions, also linked into the Win32 library app.
#include <algorithm>
#include <string>

#include "nxrt/extras.h"

namespace nxrt::extras {

bool picture_preset(const std::string& name, PictureSettings* out) {
    if (name == "neutral" || name == "off" || name.empty()) {
        *out = PictureSettings{};
        return true;
    }
    if (name == "vivid") {   // a modest push against the game's hazy look
        *out = PictureSettings{};
        out->contrast = 1.10;
        out->saturation = 1.15;
        out->black = -0.03;
        return true;
    }
    return false;
}

PictureSettings picture_clamped(PictureSettings p) {
    p.contrast = std::clamp(p.contrast, 0.5, 1.5);
    p.saturation = std::clamp(p.saturation, 0.0, 2.0);
    p.black = std::clamp(p.black, -0.5, 0.25);
    p.gamma = std::clamp(p.gamma, 0.5, 2.0);
    return p;
}

}  // namespace nxrt::extras
