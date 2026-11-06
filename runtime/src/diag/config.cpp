// SPDX-License-Identifier: GPL-3.0-or-later
#include "nxrt/config.h"

#include "nxrt/log.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace nxrt {
namespace {

// Lock discipline: g_mu is only held for plain data access. Nothing here calls logf while holding it;
// config_log_effective copies the snapshot first and logs after the lock is released.

struct Seen {
    std::string section;   // as first queried
    std::string key;       // as first queried
    std::string value;     // effective value, as text
    std::string source;    // "env", "ini" or "default"
};

std::mutex g_mu;
// canonical id ("section" 0x1F "key", lower-case) -> value. config_set values live here too (source "ini").
std::unordered_map<std::string, std::string> g_ini;
// ids whose g_ini value came from config_set_default (a game module's default), not from the ini or config_set.
std::unordered_set<std::string> g_soft;
// every key read so far, keyed by canonical id, ordered for a stable dump.
std::map<std::string, Seen> g_seen;
// the file the last config_load_file read ("" when none was read)
std::string g_file;

std::string S(const char* p) { return p ? std::string(p) : std::string(); }

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f'; }

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && is_space(s[b])) ++b;
    while (e > b && is_space(s[e - 1])) --e;
    return s.substr(b, e - b);
}

std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

std::string canon_id(const std::string& section, const std::string& key) {
    std::string id = lower(trim(section));
    id += '\x1f';
    id += lower(trim(key));
    return id;
}

struct Found {
    bool found;
    std::string value;
    const char* source;
};

// env > ini > nothing. Does not touch g_seen.
Found lookup(const std::string& section, const std::string& key) {
    std::string env = config_env_name(section.c_str(), key.c_str());
    if (const char* e = std::getenv(env.c_str())) return {true, e, "env"};
    std::lock_guard<std::mutex> g(g_mu);
    auto it = g_ini.find(canon_id(section, key));
    if (it != g_ini.end()) return {true, it->second, "ini"};
    return {false, std::string(), "default"};
}

void record(const std::string& section, const std::string& key, const std::string& value, const char* source) {
    std::lock_guard<std::mutex> g(g_mu);
    Seen& s = g_seen[canon_id(section, key)];
    s.section = section;
    s.key = key;
    s.value = value;
    s.source = source;
}

bool parse_int(const std::string& raw, int64_t& out) {
    std::string s = trim(raw);
    size_t i = 0;
    bool neg = false;
    if (i < s.size() && s[i] == '-') {
        neg = true;
        ++i;
    }
    uint64_t base = 10;
    if (s.size() >= i + 2 && s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X')) {
        base = 16;
        i += 2;
    }
    if (i >= s.size()) return false;
    uint64_t acc = 0;
    for (; i < s.size(); ++i) {
        char c = s[i];
        uint64_t d;
        if (c >= '0' && c <= '9')
            d = static_cast<uint64_t>(c - '0');
        else if (base == 16 && c >= 'a' && c <= 'f')
            d = static_cast<uint64_t>(c - 'a' + 10);
        else if (base == 16 && c >= 'A' && c <= 'F')
            d = static_cast<uint64_t>(c - 'A' + 10);
        else
            return false;
        if (acc > (UINT64_MAX - d) / base) return false;  // overflow
        acc = acc * base + d;
    }
    const uint64_t limit = neg ? static_cast<uint64_t>(INT64_MAX) + 1 : static_cast<uint64_t>(INT64_MAX);
    if (acc > limit) return false;
    out = neg ? static_cast<int64_t>(0 - acc) : static_cast<int64_t>(acc);
    return true;
}

bool parse_bool(const std::string& raw, bool& out) {
    std::string s = lower(trim(raw));
    if (s == "1" || s == "true" || s == "yes" || s == "on") {
        out = true;
        return true;
    }
    if (s == "0" || s == "false" || s == "no" || s == "off") {
        out = false;
        return true;
    }
    return false;
}

bool parse_double(const std::string& raw, double& out) {
    std::string s = trim(raw);
    if (s.empty()) return false;
    char* end = nullptr;
    double v = std::strtod(s.c_str(), &end);
    if (end != s.c_str() + s.size()) return false;
    out = v;
    return true;
}

// Parses one ini line into the current section and stores key/value pairs in `out`.
void parse_line(const std::string& line, std::string& section, std::unordered_map<std::string, std::string>& out) {
    std::string s = trim(line);
    if (s.empty()) return;
    char c0 = s[0];
    if (c0 == ';' || c0 == '#') return;  // whole-line comment
    if (c0 == '[') {
        size_t close = s.find(']');
        if (close == std::string::npos) return;  // garbage
        section = lower(trim(s.substr(1, close - 1)));
        return;
    }
    size_t eq = s.find('=');
    if (eq == std::string::npos) return;  // garbage
    std::string key = lower(trim(s.substr(0, eq)));
    if (key.empty()) return;
    std::string rest = s.substr(eq + 1);
    std::string value;
    size_t first = rest.find_first_not_of(" \t");
    if (first != std::string::npos && rest[first] == '"') {
        // quoted: take the text between the quotes verbatim; anything after the closing quote is ignored
        size_t close = rest.find('"', first + 1);
        if (close != std::string::npos)
            value = rest.substr(first + 1, close - first - 1);
        else
            value = trim(rest.substr(first + 1));
    } else {
        // unquoted: a ';' or '#' that follows whitespace starts a comment
        size_t cut = rest.size();
        for (size_t i = 1; i < rest.size(); ++i) {
            if ((rest[i] == ';' || rest[i] == '#') && is_space(rest[i - 1])) {
                cut = i;
                break;
            }
        }
        value = trim(rest.substr(0, cut));
    }
    out[canon_id(section, key)] = value;
}

std::unordered_map<std::string, std::string> parse_ini(const std::string& text) {
    std::unordered_map<std::string, std::string> out;
    std::string section;
    size_t start = 0;
    while (true) {
        size_t nl = text.find('\n', start);
        size_t end = (nl == std::string::npos) ? text.size() : nl;
        parse_line(text.substr(start, end - start), section, out);
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    return out;
}

void replace_ini(const std::string& text) {
    auto parsed = parse_ini(text);  // parse outside the lock
    std::lock_guard<std::mutex> g(g_mu);
    g_ini = std::move(parsed);
    g_soft.clear();
}

std::string fmt_double(double v) {
    char b[64];
    std::snprintf(b, sizeof b, "%g", v);
    return b;
}

}  // namespace

bool config_load_file(const char* path) {
    std::FILE* f = path ? std::fopen(path, "rb") : nullptr;
    if (!f) {
        config_clear();
        std::lock_guard<std::mutex> g(g_mu);
        g_file.clear();
        return false;
    }
    std::string text;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
    std::fclose(f);
    replace_ini(text);
    std::lock_guard<std::mutex> g(g_mu);
    g_file = path;
    return true;
}

std::string config_file_path() {
    std::lock_guard<std::mutex> g(g_mu);
    return g_file;
}

void config_load_string(const char* text) { replace_ini(S(text)); }

void config_clear() {
    std::lock_guard<std::mutex> g(g_mu);
    g_ini.clear();
    g_soft.clear();
}

void config_set(const char* section, const char* key, const char* value) {
    std::lock_guard<std::mutex> g(g_mu);
    const std::string id = canon_id(S(section), S(key));
    g_ini[id] = S(value);
    g_soft.erase(id);
}

void config_set_default(const char* section, const char* key, const char* value) {
    std::lock_guard<std::mutex> g(g_mu);
    const std::string id = canon_id(S(section), S(key));
    g_ini[id] = S(value);
    g_soft.insert(id);
}

bool config_user_set(const char* section, const char* key) {
    if (std::getenv(config_env_name(section, key).c_str())) return true;
    std::lock_guard<std::mutex> g(g_mu);
    const std::string id = canon_id(S(section), S(key));
    return g_ini.count(id) != 0 && g_soft.count(id) == 0;
}

std::string config_get_str(const char* section, const char* key, const char* def) {
    std::string sec = S(section), k = S(key);
    Found f = lookup(sec, k);
    std::string v = f.found ? f.value : S(def);
    record(sec, k, v, f.source);
    return v;
}

int64_t config_get_int(const char* section, const char* key, int64_t def) {
    std::string sec = S(section), k = S(key);
    Found f = lookup(sec, k);
    int64_t v = def;
    if (f.found) {
        int64_t parsed;
        if (parse_int(f.value, parsed)) v = parsed;  // invalid text: keep default
    }
    record(sec, k, std::to_string(v), f.source);
    return v;
}

double config_get_double(const char* section, const char* key, double def) {
    std::string sec = S(section), k = S(key);
    Found f = lookup(sec, k);
    double v = def;
    if (f.found) {
        double parsed;
        if (parse_double(f.value, parsed)) v = parsed;
    }
    record(sec, k, fmt_double(v), f.source);
    return v;
}

bool config_get_bool(const char* section, const char* key, bool def) {
    std::string sec = S(section), k = S(key);
    Found f = lookup(sec, k);
    bool v = def;
    if (f.found) {
        bool parsed;
        if (parse_bool(f.value, parsed)) v = parsed;
    }
    record(sec, k, v ? "true" : "false", f.source);
    return v;
}

bool config_has(const char* section, const char* key) {
    std::string sec = S(section), k = S(key);
    Found f = lookup(sec, k);
    record(sec, k, f.found ? f.value : std::string(), f.source);
    return f.found;
}

const char* config_source(const char* section, const char* key) {
    std::string sec = S(section), k = S(key);
    Found f = lookup(sec, k);
    record(sec, k, f.found ? f.value : std::string(), f.source);
    return f.source;
}

void config_log_effective() {
    std::vector<Seen> snap;
    {
        std::lock_guard<std::mutex> g(g_mu);
        snap.reserve(g_seen.size());
        for (const auto& kv : g_seen) snap.push_back(kv.second);
    }
    // logged outside the config lock (logf may call back into config through the [log] keys)
    for (const Seen& s : snap) {
        if (s.section.empty())
            logf(LogLevel::Info, "config", "%s = %s (%s)", s.key.c_str(), s.value.c_str(), s.source.c_str());
        else
            logf(LogLevel::Info, "config", "[%s] %s = %s (%s)", s.section.c_str(), s.key.c_str(), s.value.c_str(),
                 s.source.c_str());
    }
}

std::string config_effective_text() {
    std::vector<Seen> snap;
    {
        std::lock_guard<std::mutex> g(g_mu);
        snap.reserve(g_seen.size());
        for (const auto& kv : g_seen) snap.push_back(kv.second);
    }
    std::string out;
    for (const Seen& s : snap) {
        if (!s.section.empty()) out += "[" + s.section + "] ";
        out += s.key + " = " + s.value + " (" + s.source + ")\n";
    }
    return out;
}

std::string config_env_name(const char* section, const char* key) {
    std::string n = "NXRT_";
    std::string sec = S(section);
    if (!sec.empty()) {
        n += sec;
        n += '_';
    }
    n += S(key);
    for (char& c : n) {
        if (c >= 'a' && c <= 'z')
            c = static_cast<char>(c - 'a' + 'A');
        else if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')))
            c = '_';
    }
    return n;
}

}  // namespace nxrt
