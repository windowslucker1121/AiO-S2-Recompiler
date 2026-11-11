// SPDX-License-Identifier: GPL-3.0-or-later
// [tweaks] in controls.ini: optional looks the player can change, all off by default (the game's own look). Header-only, so the library and
// the game read and write the section with the same code.
//
//   [tweaks]
//   glow_color = off | R,G,B               one colour for every glow the game module can recolour (S44; for example the
//                                           suit's glowing parts, the morph ball's glow, trail and light, the arm cannon's glow and its charge)
//   suit_glow  = auto | off | R,G,B        auto follows glow_color; off keeps the game's colour even when glow_color is set
//   ball_glow  = auto | off | R,G,B
//   gun_glow   = auto | off | R,G,B
//   suit_color = off | R,G,B               the suit's paint, the morph ball and the spider ball with it (S45; texture recolour that keeps the
//                                           detail and shading of every texel; title E: the game module)
//   gun_color  = off | R,G,B               the arm cannon's shell (first person, every suit and beam)
//
// R, G and B are 0..255 (sRGB, as a colour picker shows them). (#RRGGBB is accepted by parse_glow_rgb, but the settings file reads a '#' after
// a space as a comment, so the file uses R,G,B.)
#pragma once
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <string>

namespace nxrt::play {

struct GlowRgb {
    uint8_t r = 255, g = 255, b = 255;
    bool operator==(const GlowRgb& o) const { return r == o.r && g == o.g && b == o.b; }
    bool operator!=(const GlowRgb& o) const { return !(*this == o); }
};

// One part's setting: follow the shared colour (auto), the game's own colour (off), or its own colour.
enum class GlowMode : uint8_t { Auto = 0, Off, Color };
struct GlowPart {
    GlowMode mode = GlowMode::Auto;
    GlowRgb rgb;
    bool operator==(const GlowPart& o) const { return mode == o.mode && (mode != GlowMode::Color || rgb == o.rgb); }
    bool operator!=(const GlowPart& o) const { return !(*this == o); }
};

struct TweakSettings {
    bool glow_on = false;   // [tweaks] glow_color: false = off (the game's colours)
    GlowRgb glow;           // the shared colour while glow_on
    GlowPart suit;          // [tweaks] suit_glow
    GlowPart ball;          // [tweaks] ball_glow
    GlowPart gun;           // [tweaks] gun_glow
    bool suit_color_on = false;   // [tweaks] suit_color: false = off (the game's paint)
    GlowRgb suit_color{40, 90, 255};
    bool gun_color_on = false;    // [tweaks] gun_color
    GlowRgb gun_color{40, 90, 255};
};

// The colour a part is drawn with: true and *out set when it is recoloured, false for the game's own colour.
inline bool glow_effective(const TweakSettings& t, const GlowPart& p, GlowRgb* out) {
    if (p.mode == GlowMode::Off) return false;
    if (p.mode == GlowMode::Color) {
        *out = p.rgb;
        return true;
    }
    if (!t.glow_on) return false;
    *out = t.glow;
    return true;
}

namespace tweaks_detail {
inline std::string lower_trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    std::string o = s.substr(a, b - a);
    for (char& c : o) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return o;
}
inline int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}
}  // namespace tweaks_detail

// "R,G,B" (0..255 each, spaces allowed) or "#RRGGBB". False for anything else.
inline bool parse_glow_rgb(const std::string& text, GlowRgb* out) {
    const std::string s = tweaks_detail::lower_trim(text);
    if (s.size() == 7 && s[0] == '#') {
        int v[6];
        for (int i = 0; i < 6; ++i)
            if ((v[i] = tweaks_detail::hexval(s[1 + i])) < 0) return false;
        out->r = static_cast<uint8_t>(v[0] * 16 + v[1]);
        out->g = static_cast<uint8_t>(v[2] * 16 + v[3]);
        out->b = static_cast<uint8_t>(v[4] * 16 + v[5]);
        return true;
    }
    int c[3] = {0, 0, 0};
    size_t pos = 0;
    for (int i = 0; i < 3; ++i) {
        while (pos < s.size() && s[pos] == ' ') ++pos;
        if (pos >= s.size() || !std::isdigit(static_cast<unsigned char>(s[pos]))) return false;
        int v = 0, digits = 0;
        while (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos]))) {
            v = v * 10 + (s[pos++] - '0');
            if (++digits > 3) return false;
        }
        if (v > 255) return false;
        c[i] = v;
        while (pos < s.size() && s[pos] == ' ') ++pos;
        if (i < 2) {
            if (pos >= s.size() || s[pos] != ',') return false;
            ++pos;
        }
    }
    if (pos != s.size()) return false;
    out->r = static_cast<uint8_t>(c[0]);
    out->g = static_cast<uint8_t>(c[1]);
    out->b = static_cast<uint8_t>(c[2]);
    return true;
}

inline std::string glow_rgb_text(const GlowRgb& c) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%u,%u,%u", static_cast<unsigned>(c.r), static_cast<unsigned>(c.g), static_cast<unsigned>(c.b));
    return buf;
}

inline std::string glow_part_text(const GlowPart& p) {
    if (p.mode == GlowMode::Off) return "off";
    if (p.mode == GlowMode::Color) return glow_rgb_text(p.rgb);
    return "auto";
}

// One `key = value` of [tweaks]. Returns false with *msg set for a bad value; *known is false for a key this code does not have (so another
// part of the program may take it).
inline bool apply_tweaks_key(const std::string& key, const std::string& value, TweakSettings* t, bool* known, std::string* msg) {
    *known = true;
    const std::string k = tweaks_detail::lower_trim(key);
    const std::string v = tweaks_detail::lower_trim(value);
    if (k == "glow_color") {
        if (v == "off" || v == "none" || v.empty()) {
            t->glow_on = false;
            return true;
        }
        GlowRgb c;
        if (!parse_glow_rgb(v, &c)) {
            *msg = "glow_color must be off or R,G,B (0..255 each), got '" + value + "'";
            return false;
        }
        t->glow_on = true;
        t->glow = c;
        return true;
    }
    if (k == "suit_glow" || k == "ball_glow" || k == "gun_glow") {
        GlowPart* p = k == "suit_glow" ? &t->suit : k == "ball_glow" ? &t->ball : &t->gun;
        if (v == "auto" || v.empty()) {
            p->mode = GlowMode::Auto;
            return true;
        }
        if (v == "off") {
            p->mode = GlowMode::Off;
            return true;
        }
        GlowRgb c;
        if (!parse_glow_rgb(v, &c)) {
            *msg = k + " must be auto, off or R,G,B (0..255 each), got '" + value + "'";
            return false;
        }
        p->mode = GlowMode::Color;
        p->rgb = c;
        return true;
    }
    if (k == "suit_color" || k == "gun_color") {
        bool* on = k == "suit_color" ? &t->suit_color_on : &t->gun_color_on;
        GlowRgb* c = k == "suit_color" ? &t->suit_color : &t->gun_color;
        if (v == "off" || v == "none" || v.empty()) {
            *on = false;
            return true;
        }
        GlowRgb rgb;
        if (!parse_glow_rgb(v, &rgb)) {
            *msg = k + " must be off or R,G,B (0..255 each), got '" + value + "'";
            return false;
        }
        *on = true;
        *c = rgb;
        return true;
    }
    *known = false;
    return true;
}

// The [tweaks] keys this code owns, one "key = value" line each.
inline std::string format_tweaks(const TweakSettings& t) {
    std::string o;
    o += "glow_color = " + (t.glow_on ? glow_rgb_text(t.glow) : std::string("off")) + "\n";
    o += "suit_glow = " + glow_part_text(t.suit) + "\n";
    o += "ball_glow = " + glow_part_text(t.ball) + "\n";
    o += "gun_glow = " + glow_part_text(t.gun) + "\n";
    o += "suit_color = " + (t.suit_color_on ? glow_rgb_text(t.suit_color) : std::string("off")) + "\n";
    o += "gun_color = " + (t.gun_color_on ? glow_rgb_text(t.gun_color) : std::string("off")) + "\n";
    return o;
}

}  // namespace nxrt::play
