// SPDX-License-Identifier: GPL-3.0-or-later
// Gap counters (nxrt/gaps.h): what the game asked for that the runtime only stands in for, written to gaps.tsv next to the log.
#include "nxrt/gaps.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <tuple>
#include <unordered_map>

#include "nxrt/log.h"

namespace nxrt {
namespace {

using Clock = std::chrono::steady_clock;
const Clock::time_point g_start = Clock::now();

struct Entry {
    std::string area, what, detail;
    uint64_t count = 0;
    double first_s = 0, last_s = 0;
};

struct Store {
    std::mutex m;
    std::unordered_map<std::string, Entry> map;
    double last_write_s = 0;
    size_t written_size = 0;          // entries at the last write: a new kind of gap writes again soon
};
Store& store() {
    static Store* s = new Store;      // never destroyed: gaps may be noted from other statics' destructors at exit
    return *s;
}

double now_s() { return std::chrono::duration<double>(Clock::now() - g_start).count(); }

std::string default_path() {
    const char* lp = log_path();
    if (!lp || !*lp) return {};
    const std::string p = lp;   // UTF-8, as the log opened it
    const size_t slash = p.find_last_of("\\/");
    return (slash == std::string::npos ? std::string() : p.substr(0, slash + 1)) + "gaps.tsv";
}

std::string clean(std::string s) {    // one TSV field: no tabs or line breaks
    for (char& ch : s)
        if (ch == '\t' || ch == '\n' || ch == '\r') ch = ' ';
    return s;
}

std::vector<GapRecord> snapshot_locked(Store& s) {
    std::vector<GapRecord> v;
    v.reserve(s.map.size());
    for (const auto& [k, e] : s.map) v.push_back(GapRecord{e.area, e.what, e.detail, e.count, e.first_s, e.last_s});
    std::sort(v.begin(), v.end(), [](const GapRecord& a, const GapRecord& b) {
        return a.count != b.count ? a.count > b.count : std::tie(a.area, a.what, a.detail) < std::tie(b.area, b.what, b.detail);
    });
    return v;
}

bool write_records(const std::vector<GapRecord>& v, const std::string& path) {
    if (path.empty()) return false;
    const std::string tmp = path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return false;
    std::fprintf(f, "count\tarea\twhat\tdetail\tfirst_s\tlast_s\n");
    for (const GapRecord& r : v)
        std::fprintf(f, "%llu\t%s\t%s\t%s\t%.1f\t%.1f\n", static_cast<unsigned long long>(r.count), clean(r.area).c_str(), clean(r.what).c_str(),
                     clean(r.detail).c_str(), r.first_s, r.last_s);
    std::fclose(f);
    std::error_code ec;
    std::filesystem::rename(std::filesystem::u8path(tmp), std::filesystem::u8path(path), ec);
    return !ec;
}

}  // namespace

void gap(const char* area, std::string_view what, std::string_view detail) { gap_n(area, what, detail, 1); }

namespace {
std::mutex& counters_mu() {
    static std::mutex* m = new std::mutex;   // never destroyed, as the store
    return *m;
}
GapCounter* g_counters = nullptr;            // registry, under counters_mu
}  // namespace

void gap_n(const char* area, std::string_view what, std::string_view detail, uint64_t n) {
    if (!n) return;
    Store& s = store();
    const double t = now_s();
    std::string key;
    key.reserve(std::char_traits<char>::length(area) + what.size() + detail.size() + 2);
    key.append(area).append(1, '\x1f').append(what).append(1, '\x1f').append(detail);
    std::vector<GapRecord> to_write;
    {
        std::lock_guard<std::mutex> lk(s.m);
        auto [it, fresh] = s.map.try_emplace(std::move(key));
        Entry& e = it->second;
        if (fresh) {
            e.area = area;
            e.what = std::string(what);
            e.detail = std::string(detail);
            e.first_s = t;
        }
        e.count += n;
        e.last_s = t;
        // Rewrite the file every 30 s, and within 5 s of a kind of gap not in the last file, so a run that dies still leaves its list.
        if (t - s.last_write_s >= 30.0 || (s.map.size() != s.written_size && t - s.last_write_s >= 5.0)) {
            s.last_write_s = t;
            s.written_size = s.map.size();
            to_write = snapshot_locked(s);
        }
    }
    if (!to_write.empty()) write_records(to_write, default_path());
}

GapCounter::GapCounter(const char* area, std::string_view what, std::string_view detail) : area_(area), what_(what), detail_(detail) {
    std::lock_guard<std::mutex> lk(counters_mu());
    next_ = g_counters;
    g_counters = this;
}

void GapCounter::flush() {
    uint64_t n;
    {
        std::lock_guard<std::mutex> lk(counters_mu());
        const uint64_t t = total_.load(std::memory_order_relaxed);
        n = t - flushed_;
        flushed_ = t;
    }
    gap_n(area_, what_, detail_, n);
}

void gap_counters_flush() {
    std::vector<GapCounter*> v;
    {
        std::lock_guard<std::mutex> lk(counters_mu());
        for (GapCounter* c = g_counters; c; c = c->next_) v.push_back(c);
    }
    for (GapCounter* c : v) c->flush();
}

std::vector<GapRecord> gaps_snapshot() {
    gap_counters_flush();
    Store& s = store();
    std::lock_guard<std::mutex> lk(s.m);
    return snapshot_locked(s);
}

bool gaps_write(const std::string& path) {
    std::vector<GapRecord> v = gaps_snapshot();
    const std::string p = path.empty() ? default_path() : path;
    return write_records(v, p);
}

std::string gaps_report(size_t n) {
    std::vector<GapRecord> v = gaps_snapshot();
    std::string s;
    char line[512];
    for (size_t i = 0; i < v.size() && i < n; ++i) {
        std::snprintf(line, sizeof line, "%10llu  %-9s %s%s%s\n", static_cast<unsigned long long>(v[i].count), v[i].area.c_str(), v[i].what.c_str(),
                      v[i].detail.empty() ? "" : ": ", v[i].detail.c_str());
        s += line;
    }
    if (v.size() > n) s += "(" + std::to_string(v.size() - n) + " more in gaps.tsv)\n";
    return s;
}

void gaps_log(size_t n) {
    gaps_write();
    std::vector<GapRecord> v = gaps_snapshot();
    NXLOG_I("gaps", "%zu kinds of requests the runtime only stands in for (all in gaps.tsv next to the log; tools/ai/gaps.py lists them)%s", v.size(),
            v.empty() ? "" : ", the most frequent:");
    for (size_t i = 0; i < v.size() && i < n; ++i)
        NXLOG_I("gaps", "%10llu  %-9s %s%s%s", static_cast<unsigned long long>(v[i].count), v[i].area.c_str(), v[i].what.c_str(),
                v[i].detail.empty() ? "" : ": ", v[i].detail.c_str());
}

}  // namespace nxrt
