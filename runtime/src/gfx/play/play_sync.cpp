// SPDX-License-Identifier: GPL-3.0-or-later
// The play settings file shared with other programs: see play_sync.h.
#include "play_sync.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <system_error>

namespace nxrt::play {
namespace {

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
    return s.substr(a, b - a);
}
std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
bool is_bindings(const std::string& sec) { return sec.compare(0, 9, "bindings.") == 0; }

// The text as lines (without their line ends); `crlf` is set when the text uses CRLF line ends.
std::vector<std::string> split(const std::string& text, bool* crlf) {
    std::vector<std::string> out;
    size_t pos = 0;
    if (crlf) *crlf = text.find("\r\n") != std::string::npos;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        std::string l = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        if (!l.empty() && l.back() == '\r') l.pop_back();
        out.push_back(std::move(l));
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return out;
}

// What one line is: a section header (sets *sec), a key line (sets *key and *value) or neither.
enum class Line { Other, Header, Key };
Line classify(const std::string& raw, const std::string& cur_sec, std::string* sec, std::string* key, std::string* value) {
    std::string l = trim(raw);
    if (l.empty()) return Line::Other;
    if (l[0] == '[') {
        const size_t close = l.find(']');
        if (close == std::string::npos) return Line::Other;
        *sec = lower(trim(l.substr(1, close - 1)));
        return Line::Header;
    }
    const bool bind = is_bindings(cur_sec);
    if (l[0] == '#') return Line::Other;
    if (l[0] == ';' && !(bind && trim(l.substr(1)).compare(0, 1, "=") == 0)) return Line::Other;
    if (!bind) {   // a trailing comment: '#' or ';' after a space or tab
        for (size_t i = 1; i < l.size(); ++i)
            if ((l[i] == '#' || l[i] == ';') && (l[i - 1] == ' ' || l[i - 1] == '\t')) { l = trim(l.substr(0, i)); break; }
    }
    const size_t eq = bind ? l.rfind('=') : l.find('=');
    if (eq == std::string::npos) return Line::Other;
    *key = trim(l.substr(0, eq));
    *value = trim(l.substr(eq + 1));
    return Line::Key;
}

struct Parsed {
    std::vector<std::string> order;                                    // sections in their first order
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> keys;   // section -> (key as written, value), last wins on lookup
    std::map<std::string, std::vector<std::string>> lines;             // section -> normalised key lines ("key = value")
};
Parsed parse(const std::string& text) {
    Parsed p;
    std::string sec;
    for (const std::string& raw : split(text, nullptr)) {
        std::string s, k, v;
        switch (classify(raw, sec, &s, &k, &v)) {
            case Line::Header:
                sec = s;
                if (!p.keys.count(sec)) { p.order.push_back(sec); p.keys[sec]; p.lines[sec]; }
                break;
            case Line::Key:
                if (sec.empty()) break;
                p.keys[sec].emplace_back(k, v);
                p.lines[sec].push_back(k + " = " + v);
                break;
            default: break;
        }
    }
    return p;
}
const std::string* find_value(const std::vector<std::pair<std::string, std::string>>& kv, const std::string& key) {
    const std::string lk = lower(key);
    const std::string* r = nullptr;
    for (const auto& e : kv) if (lower(e.first) == lk) r = &e.second;
    return r;
}

}  // namespace

std::vector<IniChange> ini_diff(const std::string& base, const std::string& other) {
    const Parsed a = parse(base), b = parse(other);
    std::vector<IniChange> out;
    std::vector<std::string> secs = b.order;
    for (const std::string& s : a.order) if (!b.keys.count(s)) secs.push_back(s);
    for (const std::string& sec : secs) {
        const auto ai = a.keys.find(sec), bi = b.keys.find(sec);
        if (is_bindings(sec)) {
            const std::vector<std::string> none;
            const auto& al = a.lines.count(sec) ? a.lines.at(sec) : none;
            const auto& bl = b.lines.count(sec) ? b.lines.at(sec) : none;
            const bool a_has = ai != a.keys.end(), b_has = bi != b.keys.end();
            if (al != bl || a_has != b_has) {
                IniChange c;
                c.kind = IniChange::Kind::Section;
                c.section = sec;
                c.lines = bl;
                if (!b_has) c.lines.clear();
                out.push_back(std::move(c));
            }
            continue;
        }
        static const std::vector<std::pair<std::string, std::string>> empty;
        const auto& akv = ai != a.keys.end() ? ai->second : empty;
        const auto& bkv = bi != b.keys.end() ? bi->second : empty;
        std::vector<std::string> seen;
        for (const auto& e : bkv) {
            const std::string lk = lower(e.first);
            if (std::find(seen.begin(), seen.end(), lk) != seen.end()) continue;
            seen.push_back(lk);
            const std::string* bv = find_value(bkv, e.first);   // the last one counts
            const std::string* av = find_value(akv, e.first);
            if (!av || *av != *bv) {
                IniChange c;
                c.kind = IniChange::Kind::Set;
                c.section = sec;
                c.key = e.first;
                c.value = *bv;
                out.push_back(std::move(c));
            }
        }
        for (const auto& e : akv) {
            const std::string lk = lower(e.first);
            if (std::find(seen.begin(), seen.end(), lk) != seen.end()) continue;
            seen.push_back(lk);
            IniChange c;
            c.kind = IniChange::Kind::Remove;
            c.section = sec;
            c.key = e.first;
            out.push_back(std::move(c));
        }
    }
    return out;
}

std::string ini_apply(const std::string& text, const std::vector<IniChange>& changes) {
    bool crlf = false;
    std::vector<std::string> lines = split(text, &crlf);
    const char* nl = crlf ? "\r\n" : "\n";
    for (const IniChange& c : changes) {
        // where the section's headers and key lines are now
        std::string sec;
        std::vector<size_t> headers, keys;      // keys: lines of this key (Set / Remove) or all key lines (Section)
        size_t last_key = std::string::npos, last_header = std::string::npos;
        for (size_t i = 0; i < lines.size(); ++i) {
            std::string s, k, v;
            const Line t = classify(lines[i], sec, &s, &k, &v);
            if (t == Line::Header) {
                sec = s;
                if (sec == c.section) { headers.push_back(i); last_header = i; }
                continue;
            }
            if (t != Line::Key || sec != c.section) continue;
            last_key = i;
            if (c.kind == IniChange::Kind::Section || lower(k) == lower(c.key)) keys.push_back(i);
        }
        if (c.kind == IniChange::Kind::Section) {
            // drop every key line of the section, then put the new ones after the first header (or a new section at the end)
            for (size_t j = keys.size(); j-- > 0;) lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(keys[j]));
            if (c.lines.empty()) continue;
            if (headers.empty()) {
                if (!lines.empty() && !trim(lines.back()).empty()) lines.push_back("");
                lines.push_back("[" + c.section + "]");
                for (const std::string& l : c.lines) lines.push_back(l);
            } else {
                size_t at = headers[0] + 1;
                for (const std::string& l : c.lines) lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(at++), l);
            }
            continue;
        }
        if (c.kind == IniChange::Kind::Remove) {
            for (size_t j = keys.size(); j-- > 0;) lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(keys[j]));
            continue;
        }
        if (!keys.empty()) {
            // the last occurrence counts when the file is read: set it there, drop the earlier ones
            const size_t at = keys.back();
            const std::string& old = lines[at];
            const size_t eq = old.find('=');
            std::string lead = old.substr(0, eq);           // the key as written, with its spacing
            while (!lead.empty() && (lead.back() == ' ' || lead.back() == '\t')) lead.pop_back();
            lines[at] = lead + " = " + c.value;
            for (size_t j = keys.size() - 1; j-- > 0;) lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(keys[j]));
        } else if (last_key != std::string::npos) {
            lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(last_key + 1), c.key + " = " + c.value);
        } else if (last_header != std::string::npos) {
            lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(last_header + 1), c.key + " = " + c.value);
        } else {
            if (!lines.empty() && !trim(lines.back()).empty()) lines.push_back("");
            lines.push_back("[" + c.section + "]");
            lines.push_back(c.key + " = " + c.value);
        }
    }
    std::string out;
    for (size_t i = 0; i < lines.size(); ++i) {
        out += lines[i];
        if (i + 1 < lines.size() || (!text.empty() && text.back() == '\n')) out += nl;
    }
    return out;
}

bool merge_settings(const Settings& base, const Settings& other, const Settings& current, Settings* out) {
    const std::vector<IniChange> ch = ini_diff(format_settings(base), format_settings(other));
    if (ch.empty()) {
        *out = current;
        return true;
    }
    Settings r;
    std::string err;
    if (!parse_settings(ini_apply(format_settings(current), ch), &r, &err)) {
        *out = current;
        return false;
    }
    *out = r;
    return true;
}

// ---- the file ------------------------------------------------------------------------------------------------------------------------------
SettingsFile::Stamp SettingsFile::stamp_of(const std::string& path) {
    Stamp s;
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &d)) return s;
    s.exists = true;
    s.size = (static_cast<uint64_t>(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
    s.time = (static_cast<uint64_t>(d.ftLastWriteTime.dwHighDateTime) << 32) | d.ftLastWriteTime.dwLowDateTime;
    return s;
}

bool SettingsFile::read_text(const std::string& path, std::string* out) {
    // shared read and write: the other program may be writing it right now (its rename then simply comes after)
    HANDLE h = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    std::string t;
    char buf[8192];
    DWORD n = 0;
    bool ok = true;
    while ((ok = ReadFile(h, buf, sizeof buf, &n, nullptr) != 0) && n > 0) t.append(buf, n);
    CloseHandle(h);
    if (!ok) return false;
    if (t.compare(0, 3, "\xEF\xBB\xBF") == 0) t.erase(0, 3);
    *out = std::move(t);
    return true;
}

void SettingsFile::set_path(const std::string& path) {
    std::lock_guard<std::mutex> lk(mu_);
    path_ = path;
}

void SettingsFile::note_current() {
    std::lock_guard<std::mutex> lk(mu_);
    stamp_ = stamp_of(path_);
    std::string t;
    if (!read_text(path_, &t)) return;
    Settings s;
    std::string err;
    if (!parse_settings(t, &s, &err)) return;
    text_ = std::move(t);
    base_ = s;
    have_base_ = true;
}

Settings SettingsFile::base() const {
    std::lock_guard<std::mutex> lk(mu_);
    return have_base_ ? base_ : default_settings();
}

SettingsFile::Poll SettingsFile::poll(Settings* disk, std::string* err) {
    std::lock_guard<std::mutex> lk(mu_);
    const Stamp now = stamp_of(path_);
    if (now == stamp_ || !now.exists) return Poll::Same;    // a deleted file is written again at the next save
    std::string t;
    if (!read_text(path_, &t)) return Poll::Same;            // locked for a moment: the next poll tries again
    if (t == text_) {                                        // our own write, or a save without a change
        stamp_ = now;
        return Poll::Same;
    }
    Settings s;
    std::string perr;
    if (!parse_settings(t, &s, &perr)) {
        stamp_ = now;                                        // say it once; the next change is read again
        if (err) *err = perr;
        return Poll::Unreadable;
    }
    stamp_ = now;
    text_ = std::move(t);
    *disk = s;
    return Poll::Changed;
}

void SettingsFile::accept(const Settings& disk) {
    std::lock_guard<std::mutex> lk(mu_);
    base_ = disk;
    have_base_ = true;
}

bool SettingsFile::save(const Settings& mine, std::string* err, bool* external, Settings* merged) {
    std::lock_guard<std::mutex> lk(mu_);
    if (external) *external = false;
    std::string cur;
    const bool exists = read_text(path_, &cur);
    if (!exists || !have_base_) {      // no file (or none the game could read): the whole text
        if (!save_settings(path_, mine, err)) return false;
        text_.clear();
        read_text(path_, &text_);
        stamp_ = stamp_of(path_);
        base_ = mine;
        have_base_ = true;
        return true;
    }
    const bool outside = cur != text_;     // someone else wrote the file since the game last read or wrote it: their keys stay
    const std::vector<IniChange> ch = ini_diff(format_settings(base_), format_settings(mine));
    const std::string next = ch.empty() ? cur : ini_apply(cur, ch);
    if (next != cur) {
        namespace fs = std::filesystem;
        const std::string tmp = path_ + ".game.tmp";   // not the library's temporary name
        {
            std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
            if (!o) {
                if (err) *err = "cannot write " + tmp;
                return false;
            }
            o << next;
            o.flush();
            if (!o) {
                if (err) *err = "cannot write " + tmp;
                std::error_code ec;
                fs::remove(tmp, ec);
                return false;
            }
        }
        std::error_code ec;
        fs::rename(tmp, path_, ec);
        if (ec) {
            std::error_code rm;
            fs::remove(tmp, rm);
            if (err) *err = "cannot replace " + path_ + ": " + ec.message();
            return false;
        }
    }
    text_ = next;
    stamp_ = stamp_of(path_);
    Settings now;
    std::string perr;
    base_ = parse_settings(next, &now, &perr) ? now : mine;
    have_base_ = true;
    if (outside) {
        if (external) *external = true;
        if (merged) *merged = base_;
    }
    return true;
}

}  // namespace nxrt::play
