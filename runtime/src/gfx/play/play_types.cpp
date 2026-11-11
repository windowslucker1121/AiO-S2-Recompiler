// SPDX-License-Identifier: GPL-3.0-or-later
// Play mode: names and parsers for contexts, triggers, actions, console outputs and inputs. The contract is play_types.h.
#include "play_types.h"

#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_scancode.h>

#include <cctype>
#include <string>

#include "nxrt/input_source.h"

namespace nxrt::play {
namespace {

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// Names compare without case, and '-' counts as '_' ("double-tap", "dpad-up").
std::string name_key(const std::string& s) {
    std::string r = lower(trim(s));
    for (char& c : r) {
        if (c == '-') c = '_';
    }
    return r;
}

// ---- contexts and triggers ---------------------------------------------------------------------------------------------------

// The generic names; the registered game names the contexts itself (GameVocabulary::ctx_names).
const char* const kCtxNames[] = {"all", "play", "alt", "map", "menu", "inspect"};
static_assert(sizeof(kCtxNames) / sizeof(kCtxNames[0]) == kCtxCount, "one name per Ctx");
const char* const* ctx_names() {
    const GameVocabulary* v = game_vocabulary();
    return v && v->ctx_names ? v->ctx_names : kCtxNames;
}

const char* const kTrigNames[] = {"press", "tap", "hold", "toggle", "doubletap"};

}  // namespace

const char* ctx_name(Ctx c) { return ctx_names()[static_cast<int>(c)]; }

bool parse_ctx(const std::string& s, Ctx* out) {
    const std::string key = name_key(s);
    for (int i = 0; i < kCtxCount; ++i) {
        if (key == ctx_names()[i]) {
            if (out) *out = static_cast<Ctx>(i);
            return true;
        }
    }
    return false;
}

const char* trig_name(Trig t) { return kTrigNames[static_cast<int>(t)]; }

bool parse_trig(const std::string& s, Trig* out) {
    const std::string key = name_key(s);
    for (int i = 0; i < 5; ++i) {
        if (key == kTrigNames[i]) {
            if (out) *out = static_cast<Trig>(i);
            return true;
        }
    }
    if (key == "double_tap") {   // the spelling with an underscore is accepted too
        if (out) *out = Trig::DoubleTap;
        return true;
    }
    return false;
}

// ---- button prompts ---------------------------------------------------------------------------------------------------------------

namespace {
const char* const kPromptNames[] = {"console", "xbox", "playstation", "keyboard", "auto"};
static_assert(sizeof(kPromptNames) / sizeof(kPromptNames[0]) == kPromptStyleCount, "one name per PromptStyle");

// Every spelling parse_prompt_style accepts (the canonical names are in the table too).
struct PromptAlias {
    const char* name;
    PromptStyle style;
};
const PromptAlias kPromptAliases[] = {
    {"console", PromptStyle::Console},        {"game", PromptStyle::Console},        {"own", PromptStyle::Console},
    {"switch", PromptStyle::Console},         {"xbox", PromptStyle::Xbox},           {"playstation", PromptStyle::PlayStation},
    {"ps", PromptStyle::PlayStation},         {"ps4", PromptStyle::PlayStation},     {"ps5", PromptStyle::PlayStation},
    {"dualsense", PromptStyle::PlayStation},  {"keyboard", PromptStyle::Keyboard},   {"kbm", PromptStyle::Keyboard},
    {"keyboard_mouse", PromptStyle::Keyboard}, {"mouse", PromptStyle::Keyboard},     {"auto", PromptStyle::Auto},
};
}  // namespace

const char* prompt_style_name(PromptStyle s) { return kPromptNames[static_cast<int>(s)]; }

bool parse_prompt_style(const std::string& s, PromptStyle* out) {
    const std::string key = name_key(s);
    for (const PromptAlias& a : kPromptAliases) {
        if (key == a.name) {
            if (out) *out = a.style;
            return true;
        }
    }
    return false;
}

// ---- pad sources and the counter corner ---------------------------------------------------------------------------------------------

namespace {
const char* const kPadSrcNames[kPadSrcCount] = {"south",         "east",           "west",       "north",      "left_shoulder", "right_shoulder",
                                                "left_trigger", "right_trigger",  "start",      "back",       "left_stick",    "right_stick",
                                                "dpad_up",      "dpad_down",      "dpad_left",  "dpad_right"};
const char* const kCornerNames[] = {"top_left", "top_right", "bottom_left", "bottom_right"};
}  // namespace

const char* pad_src_name(PadSrc s) { return kPadSrcNames[static_cast<int>(s)]; }

bool parse_pad_src(const std::string& name, PadSrc* out) {
    const std::string key = name_key(name);
    for (int i = 0; i < kPadSrcCount; ++i) {
        if (key == kPadSrcNames[i]) {
            if (out) *out = static_cast<PadSrc>(i);
            return true;
        }
    }
    return false;
}

const char* corner_name(Corner c) { return kCornerNames[static_cast<int>(c)]; }

bool parse_corner(const std::string& name, Corner* out) {
    const std::string key = name_key(name);
    for (int i = 0; i < 4; ++i) {
        if (key == kCornerNames[i]) {
            if (out) *out = static_cast<Corner>(i);
            return true;
        }
    }
    return false;
}

// ---- actions -----------------------------------------------------------------------------------------------------------------

// One row per Act, in enum order (checked by the static_assert below and by the unit tests). The generic names and outputs: the game's
// action slots have no factory output here; the registered game gives its own table (GameVocabulary::acts).
namespace {
const ActInfo kActs[] = {
    {Act::None, "none", ""},
    {Act::MoveForward, "move_forward", "LS_UP"},
    {Act::MoveBack, "move_back", "LS_DOWN"},
    {Act::MoveLeft, "move_left", "LS_LEFT"},
    {Act::MoveRight, "move_right", "LS_RIGHT"},
    {Act::LookUp, "look_up", "RS_UP"},
    {Act::LookDown, "look_down", "RS_DOWN"},
    {Act::LookLeft, "look_left", "RS_LEFT"},
    {Act::LookRight, "look_right", "RS_RIGHT"},
    {Act::Primary, "primary", ""},
    {Act::Target, "target", ""},
    {Act::Secondary, "secondary", ""},
    {Act::Jump, "jump", ""},
    {Act::AltForm, "alt_form", ""},
    {Act::LookHold, "look_hold", ""},
    {Act::Map, "map", ""},
    {Act::Pause, "pause", ""},
    {Act::Confirm, "confirm", "A"},
    {Act::Back, "back", "B"},
    {Act::DpadUp, "dpad_up", "UP"},
    {Act::DpadDown, "dpad_down", "DOWN"},
    {Act::DpadLeft, "dpad_left", "LEFT"},
    {Act::DpadRight, "dpad_right", "RIGHT"},
    {Act::SetA1, "set_a_1", ""},
    {Act::SetA2, "set_a_2", ""},
    {Act::SetA3, "set_a_3", ""},
    {Act::SetA4, "set_a_4", ""},
    {Act::SetB1, "set_b_1", ""},
    {Act::SetB2, "set_b_2", ""},
    {Act::SetB3, "set_b_3", ""},
    {Act::SetB4, "set_b_4", ""},
    {Act::SetANext, "set_a_next", ""},
    {Act::SetAPrev, "set_a_prev", ""},
    {Act::BtnA, "btn_a", "A"},
    {Act::BtnB, "btn_b", "B"},
    {Act::BtnX, "btn_x", "X"},
    {Act::BtnY, "btn_y", "Y"},
    {Act::BtnL, "btn_l", "L"},
    {Act::BtnR, "btn_r", "R"},
    {Act::BtnZL, "btn_zl", "ZL"},
    {Act::BtnZR, "btn_zr", "ZR"},
    {Act::BtnPlus, "btn_plus", "PLUS"},
    {Act::BtnMinus, "btn_minus", "MINUS"},
    {Act::BtnLStick, "btn_lstick", "LSTICK"},
    {Act::BtnRStick, "btn_rstick", "RSTICK"},
    {Act::MouseMode, "mouse_mode", ""},
};
static_assert(sizeof(kActs) / sizeof(kActs[0]) == kActCount, "one row per Act");
const ActInfo* acts() {
    const GameVocabulary* v = game_vocabulary();
    return v && v->acts ? v->acts : kActs;
}
const char* key_or(const char* GameVocabulary::* field, const char* generic) {
    const GameVocabulary* v = game_vocabulary();
    return v && v->*field ? v->*field : generic;
}
}  // namespace

const GameVocabulary* game_vocabulary() {
    const GameDefaults* d = game_defaults();
    return d ? d->vocabulary : nullptr;
}
const char* key_alt_yaw() { return key_or(&GameVocabulary::key_alt_yaw, "alt_yaw"); }
const char* key_alt_pitch() { return key_or(&GameVocabulary::key_alt_pitch, "alt_pitch"); }
const char* key_wheel_cycles() { return key_or(&GameVocabulary::key_wheel_cycles, "wheel_cycles"); }
const char* key_alt_orbit() { return key_or(&GameVocabulary::key_alt_orbit, "alt_orbit"); }
const char* key_alt_camera() { return key_or(&GameVocabulary::key_alt_camera, "alt_camera"); }
const char* key_alt_sens() { return key_or(&GameVocabulary::key_alt_sens, "alt_sensitivity"); }

const ActInfo& act_info(Act a) { return acts()[static_cast<int>(a)]; }

bool parse_act(const std::string& name, Act* out) {
    const std::string key = name_key(name);
    const ActInfo* table = acts();
    for (int i = 0; i < kActCount; ++i) {
        const ActInfo& info = table[i];
        if (key == info.name) {
            if (out) *out = info.act;
            return true;
        }
    }
    return false;
}

// ---- console outputs ----------------------------------------------------------------------------------------------------------

namespace {
using nxrt::hid::NpadButtonBit;
using nxrt::hid::btn;

struct ButtonTerm {
    const char* name;
    NpadButtonBit bit;
};
// This order is also the order of output_text().
const ButtonTerm kButtonTerms[] = {
    {"A", nxrt::hid::kBtnA},         {"B", nxrt::hid::kBtnB},         {"X", nxrt::hid::kBtnX},
    {"Y", nxrt::hid::kBtnY},         {"L", nxrt::hid::kBtnL},         {"R", nxrt::hid::kBtnR},
    {"ZL", nxrt::hid::kBtnZL},       {"ZR", nxrt::hid::kBtnZR},       {"PLUS", nxrt::hid::kBtnPlus},
    {"MINUS", nxrt::hid::kBtnMinus}, {"UP", nxrt::hid::kBtnUp},       {"DOWN", nxrt::hid::kBtnDown},
    {"LEFT", nxrt::hid::kBtnLeft},   {"RIGHT", nxrt::hid::kBtnRight}, {"LSTICK", nxrt::hid::kBtnStickL},
    {"RSTICK", nxrt::hid::kBtnStickR},
};
}  // namespace

bool parse_output(const std::string& text, Output* out, std::string* err) {
    const std::string all = trim(text);
    if (all.empty() || lower(all) == "none") {
        if (out) *out = Output{};
        return true;
    }

    Output o;
    // Stick directions are flags first, so "LS_UP+LS_DOWN" cancels to 0 and "LS_UP+LS_UP" stays 1.
    bool ls_up = false, ls_down = false, ls_left = false, ls_right = false;
    bool rs_up = false, rs_down = false, rs_left = false, rs_right = false;
    size_t pos = 0;
    for (;;) {
        const size_t plus = all.find('+', pos);
        const std::string raw = trim(all.substr(pos, plus == std::string::npos ? std::string::npos : plus - pos));
        const std::string t = upper(raw);
        if (t.empty()) {
            if (err) *err = "empty term in output '" + all + "'";
            return false;
        }
        bool found = false;
        for (const ButtonTerm& b : kButtonTerms) {
            if (t == b.name) {
                o.buttons |= btn(b.bit);
                found = true;
                break;
            }
        }
        if (!found) {
            if (t == "LS_UP") ls_up = true;
            else if (t == "LS_DOWN") ls_down = true;
            else if (t == "LS_LEFT") ls_left = true;
            else if (t == "LS_RIGHT") ls_right = true;
            else if (t == "RS_UP") rs_up = true;
            else if (t == "RS_DOWN") rs_down = true;
            else if (t == "RS_LEFT") rs_left = true;
            else if (t == "RS_RIGHT") rs_right = true;
            else {
                if (err) *err = "unknown output term '" + raw + "'";
                return false;
            }
        }
        if (plus == std::string::npos) break;
        pos = plus + 1;
    }
    o.ly = (ls_up ? 1.0f : 0.0f) - (ls_down ? 1.0f : 0.0f);
    o.lx = (ls_right ? 1.0f : 0.0f) - (ls_left ? 1.0f : 0.0f);
    o.ry = (rs_up ? 1.0f : 0.0f) - (rs_down ? 1.0f : 0.0f);
    o.rx = (rs_right ? 1.0f : 0.0f) - (rs_left ? 1.0f : 0.0f);
    if (out) *out = o;
    return true;
}

std::string output_text(const Output& o) {
    std::string r;
    auto add = [&r](const char* term) {
        if (!r.empty()) r += '+';
        r += term;
    };
    for (const ButtonTerm& b : kButtonTerms) {
        if (o.buttons & btn(b.bit)) add(b.name);
    }
    if (o.ly > 0) add("LS_UP");
    if (o.ly < 0) add("LS_DOWN");
    if (o.lx < 0) add("LS_LEFT");
    if (o.lx > 0) add("LS_RIGHT");
    if (o.ry > 0) add("RS_UP");
    if (o.ry < 0) add("RS_DOWN");
    if (o.rx < 0) add("RS_LEFT");
    if (o.rx > 0) add("RS_RIGHT");
    return r.empty() ? "none" : r;
}

// ---- inputs ------------------------------------------------------------------------------------------------------------------

std::string input_name(const InputId& id) {
    switch (id.kind) {
        case InKind::Key: {
            const char* n = SDL_GetScancodeName(static_cast<SDL_Scancode>(id.code));
            return n ? n : "";
        }
        case InKind::Mouse:
            return "Mouse" + std::to_string(id.code);
        case InKind::Wheel:
            return id.code > 0 ? "WheelUp" : "WheelDown";
        default:
            return "";
    }
}

namespace {
// Lower case with the spaces removed: "Page Up" and "PageUp" both become "pageup".
std::string squash(const std::string& s) {
    std::string r;
    for (char c : lower(s)) {
        if (c != ' ' && c != '\t') r += c;
    }
    return r;
}

// Fallback for a name SDL's lookup does not match: compares all scancode names ignoring case and spaces.
// SDL3 calls the key "PageUp", the factory bindings say "Page Up"; both must work.
SDL_Scancode find_scancode_ci(const std::string& name) {
    const std::string key = squash(name);
    if (key.empty()) return SDL_SCANCODE_UNKNOWN;
    for (int i = 1; i < SDL_SCANCODE_COUNT; ++i) {
        const char* n = SDL_GetScancodeName(static_cast<SDL_Scancode>(i));
        if (n && *n && squash(n) == key) return static_cast<SDL_Scancode>(i);
    }
    return SDL_SCANCODE_UNKNOWN;
}
}  // namespace

bool parse_input(const std::string& name_in, InputId* out) {
    const std::string name = trim(name_in);
    const std::string low = lower(name);
    InputId id;
    if (low.size() == 6 && low.compare(0, 5, "mouse") == 0 && low[5] >= '1' && low[5] <= '5') {
        id.kind = InKind::Mouse;
        id.code = low[5] - '0';
    } else if (low == "wheelup") {
        id.kind = InKind::Wheel;
        id.code = 1;
    } else if (low == "wheeldown") {
        id.kind = InKind::Wheel;
        id.code = -1;
    } else {
        SDL_Scancode sc = name.empty() ? SDL_SCANCODE_UNKNOWN : SDL_GetScancodeFromName(name.c_str());
        if (sc == SDL_SCANCODE_UNKNOWN) sc = find_scancode_ci(name);   // SDL's lookup is exact; this one ignores case and spaces
        if (sc == SDL_SCANCODE_UNKNOWN) return false;
        id.kind = InKind::Key;
        id.code = static_cast<int32_t>(sc);
    }
    if (out) *out = id;
    return true;
}

}  // namespace nxrt::play
