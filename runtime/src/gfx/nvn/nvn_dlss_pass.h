// SPDX-License-Identifier: GPL-3.0-or-later
// [dlss_pass]: replace a game's own upscaler / TAA compute pass with the temporal backend (DLSS / FSR 3), config only (docs/CONFIG.md,
// docs/DLSS_PLAN.md WP2). Pure parsing and the skip decision live here so tests can reach them; the binding lookup is in nvn_replay.cpp.
#pragma once
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace nxrt::nvn {

// A binding name: texN / imgN (a compute binding), depth_rt (the latest 3D draw's colour target as colour, its depth target as depth), none.
struct DlssPassBind {
    enum Kind { None, Tex, Img, DepthRt } kind = None;
    int n = 0;
};

struct DlssPassStrings {   // the raw values of the [dlss_pass] keys (empty: key absent)
    std::string match, color, depth, motion, output, jitter, mv_scale, reset, depth_reversed;
};

struct DlssPassCfg {
    std::vector<std::string> match;   // shader hash prefixes (comma / space list); the first evaluates, the others are skipped after a success
    DlssPassBind color, depth, motion, output;
    int jitter_slot = -1, jitter_word[2] = {0, 0};   // ubo:<slot>:<word>,<word>: two floats (pixels) in compute uniform block <slot>; slot < 0: none
    float mv_scale[2] = {1.0f, 1.0f};
    int reset_slot = -1, reset_word = 0;             // ubo:<slot>:<word>: non-zero = history reset
    bool depth_reversed = true;
    bool valid() const { return !match.empty() && color.kind != DlssPassBind::None && output.kind != DlssPassBind::None; }
};

inline DlssPassBind parse_dlss_bind(const std::string& s) {
    DlssPassBind b;
    if (s == "depth_rt") { b.kind = DlssPassBind::DepthRt; return b; }
    const bool tex = s.rfind("tex", 0) == 0, img = s.rfind("img", 0) == 0;
    if ((tex || img) && s.size() > 3 && s.find_first_not_of("0123456789", 3) == std::string::npos) {
        b.kind = tex ? DlssPassBind::Tex : DlssPassBind::Img;
        b.n = std::atoi(s.c_str() + 3);
    }
    return b;   // none and anything unknown (fs:<hash>:texN is not supported here)
}

// "ubo:<slot>:<w>[,<w>]" -> slot and words; false when it is not that form.
inline bool parse_dlss_ubo(const std::string& s, int* slot, int* w0, int* w1) {
    if (s.rfind("ubo:", 0) != 0) return false;
    const size_t c = s.find(':', 4);
    if (c == std::string::npos) return false;
    const int sl = std::atoi(s.c_str() + 4);
    const size_t comma = s.find(',', c + 1);
    *slot = sl;
    *w0 = std::atoi(s.c_str() + c + 1);
    if (w1) *w1 = comma == std::string::npos ? *w0 + 1 : std::atoi(s.c_str() + comma + 1);
    return sl >= 0 && sl < 16;
}

inline DlssPassCfg parse_dlss_pass(const DlssPassStrings& k) {
    DlssPassCfg c;
    for (size_t at = 0; at < k.match.size();) {
        size_t end = k.match.find_first_of(", ", at);
        if (end == std::string::npos) end = k.match.size();
        if (end > at) c.match.push_back(k.match.substr(at, end - at));
        at = end + 1;
    }
    c.color = parse_dlss_bind(k.color);
    c.depth = parse_dlss_bind(k.depth);
    c.motion = parse_dlss_bind(k.motion);
    c.output = parse_dlss_bind(k.output);
    int w1 = 0;
    if (!parse_dlss_ubo(k.jitter, &c.jitter_slot, &c.jitter_word[0], &w1)) c.jitter_slot = -1;
    else c.jitter_word[1] = w1;
    if (const size_t comma = k.mv_scale.find(','); comma != std::string::npos) {
        c.mv_scale[0] = static_cast<float>(std::atof(k.mv_scale.c_str()));
        c.mv_scale[1] = static_cast<float>(std::atof(k.mv_scale.c_str() + comma + 1));
    }
    if (!parse_dlss_ubo(k.reset, &c.reset_slot, &c.reset_word, nullptr)) c.reset_slot = -1;
    if (!k.depth_reversed.empty()) c.depth_reversed = k.depth_reversed != "0";
    return c;
}

enum class DlssPassAction { RunGamePass, SkipPass, Replaced };

// One compute dispatch with shader `hash` in present `present`. `evaluate` runs the backend and returns true when it produced the output.
// The first match prefix evaluates; when it cannot (mode off, DLLs missing, missing inputs) the game's pass runs unchanged. The other prefixes
// are skipped only in a present where the evaluation succeeded.
struct DlssPassState { uint64_t ok_present = ~0ull; };

template <class Evaluate>
DlssPassAction dlss_pass_decide(const DlssPassCfg& c, DlssPassState& st, const std::string& hash, uint64_t present, Evaluate evaluate) {
    if (!c.valid() || hash.empty()) return DlssPassAction::RunGamePass;
    auto is = [&](const std::string& p) { return hash.compare(0, p.size(), p) == 0; };
    if (is(c.match[0])) {
        if (!evaluate()) return DlssPassAction::RunGamePass;
        st.ok_present = present;
        return DlssPassAction::Replaced;
    }
    for (size_t i = 1; i < c.match.size(); ++i)
        if (is(c.match[i])) return st.ok_present == present ? DlssPassAction::SkipPass : DlssPassAction::RunGamePass;
    return DlssPassAction::RunGamePass;
}

}  // namespace nxrt::nvn
