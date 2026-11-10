// SPDX-License-Identifier: GPL-3.0-or-later
// Scripted-run DSL: parser, region table and GUID helpers. The grammar is documented in include/nxrt/tt_script.h.
// Standard C++ only (no windows.h, no nxrt headers besides tt_script.h). Errors are returned, never thrown.

#include "nxrt/tt_script.h"

#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace nxrt::tt {

namespace {

constexpr std::size_t kNpos = std::string::npos;
const char kUtf8Bom[] = "\xEF\xBB\xBF";

void set_err(std::string* err, const std::string& message) {
    if (err) *err = message;
}

bool is_ws(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
}

std::string trim(const std::string& s) {
    std::size_t b = 0, e = s.size();
    while (b < e && is_ws(s[b])) b++;
    while (e > b && is_ws(s[e - 1])) e--;
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

std::string first_word(const std::string& s) {
    std::size_t e = 0;
    while (e < s.size() && !is_ws(s[e])) e++;
    return s.substr(0, e);
}

// The text after the first word, trimmed (used by @hash, @log, @fail).
std::string rest_after_first_word(const std::string& body) {
    return trim(body.substr(first_word(body).size()));
}

std::vector<std::string> split_ws(const std::string& s) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && is_ws(s[i])) i++;
        std::size_t b = i;
        while (i < s.size() && !is_ws(s[i])) i++;
        if (i > b) out.push_back(s.substr(b, i - b));
    }
    return out;
}

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Non-negative integer: decimal, or 0x hex. The whole token must be consumed.
bool parse_count(const std::string& tok, int64_t* v) {
    if (tok.empty()) return false;
    std::size_t i = 0;
    uint64_t base = 10;
    if (tok.size() > 2 && tok[0] == '0' && (tok[1] == 'x' || tok[1] == 'X')) {
        base = 16;
        i = 2;
    }
    const uint64_t max = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
    uint64_t acc = 0;
    for (; i < tok.size(); i++) {
        int d = hex_value(tok[i]);
        if (d < 0 || (base == 10 && d > 9)) return false;
        uint64_t du = static_cast<uint64_t>(d);
        if (acc > (max - du) / base) return false;  // overflow
        acc = acc * base + du;
    }
    *v = static_cast<int64_t>(acc);
    return true;
}

// Floating-point number via strtod. The whole token must be consumed and the value must be finite.
bool parse_real(const std::string& tok, double* v) {
    if (tok.empty()) return false;
    char* end = nullptr;
    double d = std::strtod(tok.c_str(), &end);
    if (end != tok.c_str() + tok.size()) return false;
    if (!std::isfinite(d)) return false;
    *v = d;
    return true;
}

bool count_arg(const std::string& tok, int64_t* v, std::string& msg) {
    if (parse_count(tok, v)) return true;
    msg = "bad number '" + tok + "'";
    return false;
}

bool real_arg(const std::string& tok, double* v, std::string& msg) {
    if (parse_real(tok, v)) return true;
    msg = "bad number '" + tok + "'";
    return false;
}

bool usage(std::string& msg, const char* form) {
    msg = std::string("wrong number of arguments, expected: ") + form;
    return false;
}

bool fail_with(std::string& msg, const std::string& text) {
    msg = text;
    return false;
}

struct ButtonName {
    const char* name;
    int bit;
};

// Same bit numbers as nn::hid::NpadButton.
constexpr ButtonName kButtons[] = {
    {"A", 0},      {"B", 1},       {"X", 2},      {"Y", 3},      {"LSTICK", 4}, {"RSTICK", 5}, {"L", 6},
    {"R", 7},      {"ZL", 8},      {"ZR", 9},     {"PLUS", 10},  {"START", 10}, {"MINUS", 11},  {"SELECT", 11},
    {"LEFT", 12},  {"UP", 13},     {"RIGHT", 14}, {"DOWN", 15},
};

// "A+B+X" -> bit mask. Unknown or empty names are errors.
bool keys_mask(const std::string& keys, uint64_t* mask, std::string& msg) {
    uint64_t m = 0;
    std::size_t i = 0;
    while (true) {
        std::size_t p = keys.find('+', i);
        std::string name = keys.substr(i, p == kNpos ? kNpos : p - i);
        if (name.empty()) return fail_with(msg, "empty button name in '" + keys + "'");
        std::string up = upper(name);
        int bit = -1;
        for (const ButtonName& b : kButtons) {
            if (up == b.name) {
                bit = b.bit;
                break;
            }
        }
        if (bit < 0) return fail_with(msg, "unknown button '" + name + "'");
        m |= 1ull << bit;
        if (p == kNpos) break;
        i = p + 1;
    }
    *mask = m;
    return true;
}

struct FieldName {
    const char* name;
    AssertField field;
};

constexpr FieldName kFields[] = {
    {"area", AssertField::Area},   {"world", AssertField::World},  {"frame", AssertField::Frame},
    {"igt", AssertField::Igt},     {"health", AssertField::Health}, {"px", AssertField::PosX},
    {"py", AssertField::PosY},     {"pz", AssertField::PosZ},      {"vx", AssertField::VelX},
    {"vy", AssertField::VelY},     {"vz", AssertField::VelZ},      {"speed", AssertField::Speed},
    {"state", AssertField::State},   {"tod", AssertField::Tod},
    {"prog", AssertField::Prog},
};

struct OpName {
    const char* text;
    AssertOp op;
};

constexpr OpName kOps[] = {
    {"==", AssertOp::Eq}, {"!=", AssertOp::Ne}, {"<", AssertOp::Lt},
    {"<=", AssertOp::Le}, {">", AssertOp::Gt},  {">=", AssertOp::Ge},
};

// An id token is a GUID text, or a region name (for @area: the entry's area; for @world and @warp's world token: its world).
bool resolve_id(const std::string& tok, const RegionTable* regions, bool want_area, uint8_t out[16], std::string& msg) {
    if (tt_guid_to_bytes(tok, out)) return true;
    const RegionTable::Entry* e = regions ? regions->find(tok) : nullptr;
    if (!e) return fail_with(msg, "'" + tok + "' is not a GUID or a known region name");
    if (want_area) {
        if (!e->has_area) return fail_with(msg, "region '" + tok + "' has no area");
        std::memcpy(out, e->area, 16);
    } else {
        std::memcpy(out, e->world, 16);
    }
    return true;
}

// Splits trailing "timeout=N" tokens off a positional argument list.
bool take_timeout(const std::vector<std::string>& t, std::size_t from, std::vector<std::string>& positional,
                  int64_t* timeout, std::string& msg) {
    bool seen = false;
    for (std::size_t i = from; i < t.size(); i++) {
        if (lower(t[i]).rfind("timeout=", 0) == 0) {
            if (seen) return fail_with(msg, "timeout given twice");
            if (!count_arg(t[i].substr(8), timeout, msg)) return false;
            seen = true;
        } else {
            positional.push_back(t[i]);
        }
    }
    return true;
}

// EItemType (CPlayerStateMP1::EItemType): name (case and underscores ignored) or number -> id; -1 when unknown.
int64_t item_id(const std::string& tok) {
    static const char* const names[] = {"powerbeam", "icebeam", "wavebeam", "plasmabeam", "missiles", "scanvisor", "morphballbombs", "powerbombs",
                                        "flamethrower", "thermalvisor", "chargebeam", "supermissile", "grapplebeam", "xrayvisor", "icespreader",
                                        "spacejumpboots", "morphball", "combatvisor", "boostball", "spiderball", "powersuit", "gravitysuit",
                                        "variasuit", "phazonsuit", "energytanks"};
    std::string k;
    for (char c : tok) if (c != '_') k.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    for (size_t i = 0; i < sizeof names / sizeof *names; i++) if (k == names[i]) return static_cast<int64_t>(i);
    if (k == "wavebuster") return 0x1c;
    if (k == "bombs") return 6;
    if (k == "spacejump") return 0xf;
    if (!k.empty() && std::isdigit(static_cast<unsigned char>(k[0]))) {
        char* end = nullptr;
        const long v = std::strtol(k.c_str(), &end, 0);
        if (end && *end == 0 && v >= 0 && v < 0x29) return v;
    }
    return -1;
}

// Parses "<field> <op> <value>" at t[i..i+2] into s.field / s.op / s.x / s.id / s.text (shared by @assert and @mash).
bool parse_condition(const std::vector<std::string>& t, std::size_t i, const RegionTable* rt, Step* sp, std::string& msg) {
    Step& s = *sp;
    std::string field_name = lower(t[i]);
    const FieldName* field = nullptr;
    for (const FieldName& f : kFields) {
        if (field_name == f.name) {
            field = &f;
            break;
        }
    }
    if (!field) return fail_with(msg, "unknown assert field '" + t[i] + "'");
    const OpName* op = nullptr;
    for (const OpName& o : kOps) {
        if (t[i + 1] == o.text) {
            op = &o;
            break;
        }
    }
    if (!op) return fail_with(msg, "unknown operator '" + t[i + 1] + "'");
    s.field = field->field;
    s.op = op->op;
    s.text = t[i + 2];
    if (s.field == AssertField::Area || s.field == AssertField::World) {
        if (s.op != AssertOp::Eq && s.op != AssertOp::Ne)
            return fail_with(msg, "@assert " + t[i] + " allows only == and !=");
        if (!resolve_id(t[i + 2], rt, s.field == AssertField::Area, s.id, msg)) return false;
        s.name1 = t[i + 2];
    } else if (s.field == AssertField::Frame) {
        int64_t frame = 0;
        if (!count_arg(t[i + 2], &frame, msg)) return false;
        s.x = static_cast<double>(frame);
    } else {
        if (!real_arg(t[i + 2], &s.x, msg)) return false;
    }
    return true;
}

// Parses one logical line (comment already removed, trimmed) into zero or more steps.
// `body` is the trimmed line, `t` its whitespace-separated tokens (t is never empty).
bool parse_stmt(const std::string& body, const std::vector<std::string>& t, const RegionTable* rt,
                std::vector<Step>& out, std::string& msg) {
    const std::string kw = lower(t[0]);
    const std::size_t n = t.size();
    auto make = [](StepKind k) {
        Step s;
        s.kind = k;
        return s;
    };

    // Bare "<n>" (wait) or "<n>:<keys>[/<h>]" (wait n, then press keys for h frames, default 4).
    if (kw[0] >= '0' && kw[0] <= '9') {
        std::size_t colon = t[0].find(':');
        if (colon == kNpos) {
            if (n != 1) return usage(msg, "<n>");
            Step w = make(StepKind::Wait);
            if (!count_arg(t[0], &w.count, msg)) return false;
            out.push_back(w);
            return true;
        }
        if (n != 1) return usage(msg, "<n>:<keys>[/<h>]");
        int64_t wait = 0;
        int64_t hold = 4;
        if (!count_arg(t[0].substr(0, colon), &wait, msg)) return false;
        std::string keys = t[0].substr(colon + 1);
        std::size_t slash = keys.find('/');
        if (slash != kNpos) {
            std::string hold_text = keys.substr(slash + 1);
            keys = keys.substr(0, slash);
            if (!count_arg(hold_text, &hold, msg)) return false;
        }
        uint64_t mask = 0;
        if (!keys_mask(keys, &mask, msg)) return false;
        Step w = make(StepKind::Wait);
        w.count = wait;
        out.push_back(w);
        Step p = make(StepKind::Press);
        p.buttons = mask;
        p.count = hold;
        out.push_back(p);
        return true;
    }

    if (kw == "wait") {
        if (n != 2) return usage(msg, "wait <n>");
        Step s = make(StepKind::Wait);
        if (!count_arg(t[1], &s.count, msg)) return false;
        out.push_back(s);
        return true;
    }

    if (kw == "press") {
        if (!(n == 2 || (n == 4 && lower(t[2]) == "for"))) return usage(msg, "press <keys> [for <n>]");
        Step s = make(StepKind::Press);
        if (!keys_mask(t[1], &s.buttons, msg)) return false;
        s.count = -1;
        if (n == 4 && !count_arg(t[3], &s.count, msg)) return false;
        out.push_back(s);
        return true;
    }

    if (kw == "hold") {
        if (n != 2) return usage(msg, "hold <keys>");
        Step s = make(StepKind::Hold);
        if (!keys_mask(t[1], &s.buttons, msg)) return false;
        out.push_back(s);
        return true;
    }

    if (kw == "release") {
        if (n != 2) return usage(msg, "release <keys>|all");
        Step s = make(StepKind::Release);
        if (lower(t[1]) == "all") {
            s.buttons = ~0ull;
        } else if (!keys_mask(t[1], &s.buttons, msg)) {
            return false;
        }
        out.push_back(s);
        return true;
    }

    // key <name> [for <n>] / keydown <name> / keyup <name>: the name is the rest of the line; key takes an optional trailing "for <n>".
    if (kw == "key" || kw == "keydown" || kw == "keyup") {
        const bool takes_for = kw == "key";
        const char* form = kw == "key" ? "key <name> [for <n>]" : kw == "keydown" ? "keydown <name>" : "keyup <name>";
        std::string name = rest_after_first_word(body);
        Step s = make(kw == "key" ? StepKind::Key : kw == "keydown" ? StepKind::KeyDown : StepKind::KeyUp);
        s.count = -1;
        if (takes_for) {
            std::vector<std::string> tk = split_ws(name);
            if (tk.size() >= 2 && lower(tk[tk.size() - 2]) == "for") {
                // name is trimmed, so the last token ends the string; skip the blanks before it to reach the "for" token
                std::size_t p = name.size() - tk.back().size();
                while (p > 0 && is_ws(name[p - 1])) p--;
                const std::size_t for_at = p - tk[tk.size() - 2].size();
                if (!count_arg(tk.back(), &s.count, msg)) return false;
                name = trim(name.substr(0, for_at));
            }
        }
        if (name.empty()) return usage(msg, form);
        s.text = name;
        out.push_back(s);
        return true;
    }

    if (kw == "stick") {
        if (!(n == 4 || (n == 6 && lower(t[4]) == "for"))) return usage(msg, "stick left|right|l|r <x> <y> [for <n>]");
        Step s = make(StepKind::Stick);
        std::string side = lower(t[1]);
        if (side == "left" || side == "l") {
            s.which = 0;
        } else if (side == "right" || side == "r") {
            s.which = 1;
        } else {
            return fail_with(msg, "stick side must be left, right, l or r, got '" + t[1] + "'");
        }
        if (!real_arg(t[2], &s.x, msg) || !real_arg(t[3], &s.y, msg)) return false;
        if (s.x < -1.0 || s.x > 1.0) return fail_with(msg, "stick x " + t[2] + " out of range -1.0..1.0");
        if (s.y < -1.0 || s.y > 1.0) return fail_with(msg, "stick y " + t[3] + " out of range -1.0..1.0");
        s.count = 0;
        if (n == 6 && !count_arg(t[5], &s.count, msg)) return false;
        out.push_back(s);
        return true;
    }

    if (kw == "mouse") {
        if (!(n == 3 || (n == 5 && lower(t[3]) == "for"))) return usage(msg, "mouse <dx> <dy> [for <n>]");
        Step s = make(StepKind::Mouse);
        if (!real_arg(t[1], &s.x, msg) || !real_arg(t[2], &s.y, msg)) return false;
        s.count = -1;
        if (n == 5 && !count_arg(t[4], &s.count, msg)) return false;
        out.push_back(s);
        return true;
    }

    if (kw == "@frame") {
        if (n != 2) return usage(msg, "@frame <n>");
        Step s = make(StepKind::WaitFrame);
        if (!count_arg(t[1], &s.count, msg)) return false;
        out.push_back(s);
        return true;
    }

    if (kw == "@area" || kw == "@world") {
        const bool area = kw == "@area";
        std::vector<std::string> pos;
        int64_t timeout = -1;
        if (!take_timeout(t, 1, pos, &timeout, msg)) return false;
        if (pos.size() != 1) return usage(msg, area ? "@area <id> [timeout=<n>]" : "@world <id> [timeout=<n>]");
        Step s = make(area ? StepKind::WaitArea : StepKind::WaitWorld);
        if (!resolve_id(pos[0], rt, area, s.id, msg)) return false;
        s.name1 = pos[0];
        s.timeout = timeout;
        out.push_back(s);
        return true;
    }

    if (kw == "@pos") {
        std::vector<std::string> pos;
        int64_t timeout = -1;
        if (!take_timeout(t, 1, pos, &timeout, msg)) return false;
        if (pos.size() != 3 && pos.size() != 4) return usage(msg, "@pos <x> <y> <z> [<r>] [timeout=<n>]");
        Step s = make(StepKind::WaitPos);
        if (!real_arg(pos[0], &s.x, msg) || !real_arg(pos[1], &s.y, msg) || !real_arg(pos[2], &s.z, msg)) return false;
        s.r = 1.0;
        if (pos.size() == 4) {
            if (!real_arg(pos[3], &s.r, msg)) return false;
            if (s.r < 0.0) return fail_with(msg, "@pos radius must be >= 0, got '" + pos[3] + "'");
        }
        s.timeout = timeout;
        out.push_back(s);
        return true;
    }

    if (kw == "@igt") {
        std::vector<std::string> pos;
        int64_t timeout = -1;
        if (!take_timeout(t, 1, pos, &timeout, msg)) return false;
        if (pos.size() != 1) return usage(msg, "@igt <seconds> [timeout=<n>]");
        Step s = make(StepKind::WaitIgt);
        if (!real_arg(pos[0], &s.x, msg)) return false;
        if (s.x < 0.0) return fail_with(msg, "@igt seconds must be >= 0, got '" + pos[0] + "'");
        s.timeout = timeout;
        out.push_back(s);
        return true;
    }

    if (kw == "@grant") {
        if (n < 2 || n > 4) return usage(msg, "@grant <item|all> [<amount> [<capacity>]]");
        Step s = make(StepKind::Grant);
        s.text = t[1];
        s.count = -1;
        if (lower(t[1]) != "all") {
            s.count = item_id(t[1]);
            if (s.count < 0) return fail_with(msg, "unknown item '" + t[1] + "' (an EItemType name or a number)");
        }
        s.x = 1;
        if (n >= 3 && !real_arg(t[2], &s.x, msg)) return false;
        s.y = s.x;
        if (n == 4 && !real_arg(t[3], &s.y, msg)) return false;
        out.push_back(s);
        return true;
    }

    if (kw == "@call") {
        if (n < 2 || n > 8) return usage(msg, "@call <offset> [<arg>...] (up to 6 arguments)");
        Step s = make(StepKind::Call);
        if (!count_arg(t[1], &s.count, msg)) return false;
        s.text = rest_after_first_word(rest_after_first_word(body));
        out.push_back(s);
        return true;
    }

    if (kw == "@objs") {
        if (n < 2) return usage(msg, "@objs <class>... (door dock pickup special actor platform trigger player or a vtable offset)");
        Step s = make(StepKind::Objs);
        s.text = rest_after_first_word(body);
        out.push_back(s);
        return true;
    }

    if (kw == "@probe") {
        if (n != 3) return usage(msg, "@probe <in-file> <out-file>");
        Step s = make(StepKind::Probe);
        s.text = rest_after_first_word(body);
        out.push_back(s);
        return true;
    }

    if (kw == "@poke") {
        if (n != 2) return usage(msg, "@poke <in-file>");
        Step s = make(StepKind::Poke);
        s.text = rest_after_first_word(body);
        out.push_back(s);
        return true;
    }

    if (kw == "@peek") {
        if (n != 3) return usage(msg, "@peek <in-file> <out-file>");
        Step s = make(StepKind::Peek);
        s.text = rest_after_first_word(body);
        out.push_back(s);
        return true;
    }

    if (kw == "@rays") {
        if (n != 3) return usage(msg, "@rays <in-file> <out-file>");
        Step s = make(StepKind::Rays);
        s.text = rest_after_first_word(body);
        out.push_back(s);
        return true;
    }

    if (kw == "@watch") {
        if (n < 2 || n > 3) return usage(msg, "@watch on [vely|posy|both] | @watch off");
        Step s = make(StepKind::Watch);
        s.text = lower(t[1]);
        if (s.text != "on" && s.text != "off") return fail_with(msg, "@watch: expected on or off, got '" + t[1] + "'");
        s.y = 3;
        if (n == 3) {
            const std::string w = lower(t[2]);
            if (w == "vely") s.y = 1; else if (w == "posy") s.y = 2; else if (w == "both") s.y = 3;
            else return fail_with(msg, "@watch on: expected vely, posy or both, got '" + t[2] + "'");
        }
        out.push_back(s);
        return true;
    }

    if (kw == "@run") {
        std::vector<std::string> pos;
        int64_t timeout = -1;
        if (!take_timeout(t, 1, pos, &timeout, msg)) return false;
        bool soft = false;
        if (pos.size() == 2 && lower(pos[1]) == "soft") { soft = true; pos.pop_back(); }
        if (pos.size() != 1) return usage(msg, "@run <seconds> [timeout=<n>] [soft]");
        Step s = make(StepKind::WaitRun);
        s.soft = soft;
        if (!real_arg(pos[0], &s.x, msg)) return false;
        if (s.x < 0.0) return fail_with(msg, "@run seconds must be >= 0, got '" + pos[0] + "'");
        s.timeout = timeout;
        out.push_back(s);
        return true;
    }

    if (kw == "@mash") {
        std::vector<std::string> pos;
        int64_t timeout = -1;
        if (!take_timeout(t, 1, pos, &timeout, msg)) return false;
        if (pos.size() != 4) return usage(msg, "@mash <keys> <period_ms> area|world <id> [timeout=<ms>]");
        Step s = make(StepKind::Mash);
        if (!keys_mask(pos[0], &s.buttons, msg)) return false;
        if (!real_arg(pos[1], &s.x, msg)) return false;
        const std::string what = lower(pos[2]);
        if (what != "area" && what != "world") return usage(msg, "@mash <keys> <period_ms> area|world <id> [timeout=<ms>]");
        s.text = what;
        if (!resolve_id(pos[3], rt, what == "area", s.id, msg)) return false;
        s.name1 = pos[3];
        s.timeout = timeout;
        out.push_back(s);
        return true;
    }

    if (kw == "@rate") {
        if (n != 2) return usage(msg, "@rate <hz>");
        Step s = make(StepKind::Rate);
        if (!real_arg(t[1], &s.x, msg)) return false;
        if (s.x < 0.0 || s.x > 10000.0) return fail_with(msg, "@rate hz must be 0 (real clock) or 1..10000, got '" + t[1] + "'");
        out.push_back(s);
        return true;
    }

    if (kw == "@give") {
        if (n != 3) return usage(msg, "@give <item> <amount>");
        static const char* const names[] = {"powerbeam", "icebeam", "wavebeam", "plasmabeam", "missiles", "scanvisor", "morphballbombs", "powerbombs", "flamethrower",
            "thermalvisor", "chargebeam", "supermissile", "grapplebeam", "xrayvisor", "icespreader", "spacejumpboots", "morphball", "combatvisor", "boostball", "spiderball",
            "powersuit", "gravitysuit", "variasuit", "phazonsuit", "energytanks"};
        std::string nm;
        for (char ch : t[1]) if (ch != '_' && ch != ' ') nm += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        Step s = make(StepKind::Give);
        s.x = -1;
        for (int i = 0; i < 25; i++) if (nm == names[i]) s.x = i;
        if (s.x < 0 && std::isdigit(static_cast<unsigned char>(t[1][0])) && !real_arg(t[1], &s.x, msg)) return false;
        if (s.x < 0 || s.x > 40) return fail_with(msg, "@give: unknown item '" + t[1] + "'");
        if (!real_arg(t[2], &s.y, msg)) return false;
        out.push_back(s);
        return true;
    }

    if (kw == "@sleep") {
        if (n != 2) return usage(msg, "@sleep <ms>");
        Step s = make(StepKind::WaitMs);
        if (!real_arg(t[1], &s.x, msg)) return false;
        if (s.x < 0.0) return fail_with(msg, "@sleep ms must be >= 0, got '" + t[1] + "'");
        out.push_back(s);
        return true;
    }

    if (kw == "@warp") {
        if (n != 3) return usage(msg, "@warp <world> <area>");
        Step s = make(StepKind::Warp);
        if (!resolve_id(t[1], rt, false, s.id2, msg)) return false;
        if (!resolve_id(t[2], rt, true, s.id, msg)) return false;
        s.name1 = t[1];
        s.name2 = t[2];
        out.push_back(s);
        return true;
    }

    if (kw == "@shot") {
        if (n != 2) return usage(msg, "@shot <name>");
        Step s = make(StepKind::Shot);
        s.text = t[1];
        out.push_back(s);
        return true;
    }

    if (kw == "@hash") {
        Step s = make(StepKind::Hash);
        s.text = rest_after_first_word(body);
        out.push_back(s);
        return true;
    }

    if (kw == "@log") {
        Step s = make(StepKind::Log);
        s.text = rest_after_first_word(body);
        if (s.text.empty()) return fail_with(msg, "@log needs text");
        out.push_back(s);
        return true;
    }

    if (kw == "@timeout") {
        if (n != 2) return usage(msg, "@timeout <n>");
        Step s = make(StepKind::Timeout);
        if (!count_arg(t[1], &s.count, msg)) return false;
        out.push_back(s);
        return true;
    }

    if (kw == "@pass" || kw == "@end") {
        if (n != 1) return usage(msg, kw.c_str());
        out.push_back(make(StepKind::Pass));
        return true;
    }

    if (kw == "@fail") {
        Step s = make(StepKind::Fail);
        s.text = rest_after_first_word(body);
        out.push_back(s);
        return true;
    }

    if (kw == "@assert") {
        if (n != 4) return usage(msg, "@assert <field> <op> <value>");
        Step s = make(StepKind::Assert);
        if (!parse_condition(t, 1, rt, &s, msg)) return false;
        out.push_back(s);
        return true;
    }

    if (kw == "@goto") {
        std::vector<std::string> pos;
        int64_t timeout = -1;
        if (!take_timeout(t, 1, pos, &timeout, msg)) return false;
        Step s = make(StepKind::GoTo);
        if (!pos.empty() && lower(pos.back()) == "soft") { s.soft = true; pos.pop_back(); }
        if (pos.size() != 2 && pos.size() != 3) return usage(msg, "@goto <x> <z> [<r>] [soft] [timeout=<n>]");
        if (!real_arg(pos[0], &s.x, msg) || !real_arg(pos[1], &s.z, msg)) return false;
        s.r = 60.0;
        if (pos.size() == 3 && (!real_arg(pos[2], &s.r, msg) || s.r <= 0.0)) return fail_with(msg, "@goto radius must be > 0");
        s.timeout = timeout;
        out.push_back(s);
        return true;
    }

    if (kw == "@mashuntil") {
        // @mashuntil <keys> every <n> until <field> <op> <value> [timeout=<n>]: press the keys for 4 frames every n frames until the condition holds
        std::vector<std::string> pos;
        int64_t timeout = -1;
        if (!take_timeout(t, 1, pos, &timeout, msg)) return false;
        if (pos.size() != 7 || lower(pos[1]) != "every" || lower(pos[3]) != "until")
            return usage(msg, "@mashuntil <keys> every <n> until <field> <op> <value> [timeout=<n>]");
        Step s = make(StepKind::PressUntil);
        if (!keys_mask(pos[0], &s.buttons, msg)) return false;
        if (!count_arg(pos[2], &s.count, msg)) return false;
        if (s.count < 5) return fail_with(msg, "@mashuntil interval must be at least 5 frames");
        if (!parse_condition(pos, 4, rt, &s, msg)) return false;
        s.timeout = timeout;
        out.push_back(s);
        return true;
    }

    return fail_with(msg, "unknown keyword '" + t[0] + "'");
}

}  // namespace

bool RegionTable::load(const std::string& path, std::string* err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        set_err(err, "cannot open " + path);
        return false;
    }
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return load_text(text, err);
}

bool RegionTable::load_text(const std::string& text_in, std::string* err) {
    std::string text = text_in;
    if (text.size() >= 3 && text.compare(0, 3, kUtf8Bom) == 0) text.erase(0, 3);

    std::vector<Entry> parsed;
    std::size_t pos = 0;
    int lineno = 0;
    while (pos <= text.size()) {
        std::size_t nl = text.find('\n', pos);
        std::size_t end = (nl == kNpos) ? text.size() : nl;
        std::string raw = text.substr(pos, end - pos);
        pos = (nl == kNpos) ? text.size() + 1 : nl + 1;
        lineno++;
        if (!raw.empty() && raw.back() == '\r') raw.pop_back();

        std::string trimmed = trim(raw);
        if (trimmed.empty() || trimmed[0] == '#') continue;

        const std::string prefix = "line " + std::to_string(lineno) + ": ";
        std::vector<std::string> cols;
        std::size_t c = 0;
        while (true) {
            std::size_t tab = raw.find('\t', c);
            cols.push_back(raw.substr(c, tab == kNpos ? kNpos : tab - c));
            if (tab == kNpos) break;
            c = tab + 1;
        }
        if (cols.size() < 2 || cols.size() > 3) {
            set_err(err, prefix + "expected name<TAB>world-guid<TAB>area-guid");
            return false;
        }

        Entry e{};
        e.name = trim(cols[0]);
        if (e.name.empty()) {
            set_err(err, prefix + "empty region name");
            return false;
        }
        if (!tt_guid_to_bytes(trim(cols[1]), e.world)) {
            set_err(err, prefix + "bad world GUID '" + trim(cols[1]) + "'");
            return false;
        }
        if (cols.size() == 3 && !trim(cols[2]).empty()) {
            if (!tt_guid_to_bytes(trim(cols[2]), e.area)) {
                set_err(err, prefix + "bad area GUID '" + trim(cols[2]) + "'");
                return false;
            }
            e.has_area = true;
        }
        parsed.push_back(std::move(e));
    }
    entries = std::move(parsed);
    return true;
}

const RegionTable::Entry* RegionTable::find(const std::string& name) const {
    std::string want = lower(name);
    for (const Entry& e : entries) {
        if (lower(e.name) == want) return &e;
    }
    return nullptr;
}

bool tt_guid_to_bytes(const std::string& text, uint8_t out[16]) {
    if (text.size() != 36) return false;
    uint8_t tmp[16];
    std::size_t n = 0;
    int high = -1;
    for (std::size_t i = 0; i < 36; i++) {
        char c = text[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-') return false;
            continue;
        }
        int v = hex_value(c);
        if (v < 0) return false;
        if (high < 0) {
            high = v;
        } else {
            tmp[n++] = static_cast<uint8_t>(high * 16 + v);
            high = -1;
        }
    }
    // The game's CObjectId is a Windows-style GUID struct in memory: the first three groups are little-endian integers (verified against the
    // real game under Eden: the Frigate world 4b5e8635-0f43-4ffb-9427-2e62186e517d reads as 35865e4b 430f fb4f 9427 2e62186e517d).
    std::swap(tmp[0], tmp[3]); std::swap(tmp[1], tmp[2]);
    std::swap(tmp[4], tmp[5]);
    std::swap(tmp[6], tmp[7]);
    std::memcpy(out, tmp, 16);
    return true;
}

std::string tt_guid_hex(const uint8_t in[16]) {
    static const char kDigits[] = "0123456789abcdef";
    std::string s;
    s.reserve(32);
    for (int i = 0; i < 16; i++) {
        s.push_back(kDigits[in[i] >> 4]);
        s.push_back(kDigits[in[i] & 0x0f]);
    }
    return s;
}

std::string tt_guid_to_text(const uint8_t in[16]) {
    uint8_t t[16];
    std::memcpy(t, in, 16);
    std::swap(t[0], t[3]); std::swap(t[1], t[2]);
    std::swap(t[4], t[5]);
    std::swap(t[6], t[7]);
    std::string h = tt_guid_hex(t);
    return h.substr(0, 8) + "-" + h.substr(8, 4) + "-" + h.substr(12, 4) + "-" + h.substr(16, 4) + "-" + h.substr(20, 12);
}

bool parse_script(const std::string& text_in, const std::string& path, const RegionTable* regions, Script* out,
                  std::string* err) {
    std::string text = text_in;
    if (text.size() >= 3 && text.compare(0, 3, kUtf8Bom) == 0) text.erase(0, 3);

    Script script;
    script.path = path;
    std::size_t pos = 0;
    int lineno = 0;
    while (pos <= text.size()) {
        std::size_t nl = text.find('\n', pos);
        std::size_t end = (nl == kNpos) ? text.size() : nl;
        std::string raw = text.substr(pos, end - pos);
        pos = (nl == kNpos) ? text.size() + 1 : nl + 1;
        lineno++;

        std::string trimmed = trim(raw);
        if (trimmed.empty()) continue;

        // '#' starts a comment, except on @log and @fail lines, which keep everything after the keyword.
        const std::string word = lower(first_word(trimmed));
        std::string body = trimmed;
        if (word != "@log" && word != "@fail") {
            std::size_t hash = trimmed.find('#');
            if (hash != kNpos) body = trim(trimmed.substr(0, hash));
        }
        if (body.empty()) continue;

        std::vector<Step> made;
        std::string msg;
        if (!parse_stmt(body, split_ws(body), regions, made, msg)) {
            set_err(err, path + ":" + std::to_string(lineno) + ": " + msg);
            return false;
        }
        for (Step& s : made) {
            s.line = lineno;
            s.src = body;
            script.steps.push_back(std::move(s));
        }
    }
    if (out) *out = std::move(script);
    return true;
}

bool parse_script_file(const std::string& path, const RegionTable* regions, Script* out, std::string* err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        set_err(err, "cannot open " + path);
        return false;
    }
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return parse_script(text, path, regions, out, err);
}

}  // namespace nxrt::tt
