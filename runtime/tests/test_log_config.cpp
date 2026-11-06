// SPDX-License-Identifier: GPL-3.0-or-later
// Unit tests for nxrt/log.h (src/diag/log.cpp) and nxrt/config.h (src/diag/config.cpp).
#include "test_framework.h"

#include "nxrt/config.h"
#include "nxrt/log.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

NXTEST_GROUP(log_config)

using namespace nxrt;

namespace {

struct Capture {
    std::vector<std::string> lines;
    std::vector<LogLevel> levels;
};

void capture_sink(LogLevel l, const char* line, void* user) {
    auto* c = static_cast<Capture*>(user);
    c->lines.emplace_back(line);
    c->levels.push_back(l);
}

// Removes the sink when the test leaves scope, even on an early return.
struct SinkGuard {
    explicit SinkGuard(Capture* c) { log_set_sink(capture_sink, c); }
    ~SinkGuard() { log_set_sink(nullptr, nullptr); }
};

// Environment variables the module reads. An empty value removes the variable on Windows.
void clear_env() {
    _putenv_s("NXRT_LOG", "");
    _putenv_s("NXRT_LOG_LEVEL", "");
    _putenv_s("NXRT_LOG_PATH", "");
    _putenv_s("NXRT_DIAG_WATCHDOG_SECONDS", "");
}

// Log to nothing but the sink: no file, no stderr, empty config.
void quiet_init(LogLevel lvl = LogLevel::Info) {
    clear_env();
    config_clear();
    LogOptions o;
    o.path = "";
    o.level = lvl;
    o.to_stderr = false;
    o.keep_previous = false;
    log_init(o);
}

bool contains(const std::string& s, const char* needle) { return s.find(needle) != std::string::npos; }

int count_containing(const std::vector<std::string>& lines, const char* needle) {
    int n = 0;
    for (const auto& l : lines)
        if (contains(l, needle)) n++;
    return n;
}

std::string temp_path(const char* name) {
    char dir[MAX_PATH];
    DWORD n = GetTempPathA(MAX_PATH, dir);
    if (n == 0 || n >= MAX_PATH) return std::string();
    char buf[MAX_PATH + 128];
    std::snprintf(buf, sizeof buf, "%snxrt_test_%s_%lu.log", dir, name,
                  static_cast<unsigned long>(GetCurrentProcessId()));
    return buf;
}

void remove_log_files(const std::string& p) {
    std::remove(p.c_str());
    std::string prev = p + ".prev";
    std::remove(prev.c_str());
}

std::string read_file(const std::string& p) {
    std::string out;
    std::FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) return out;
    char buf[1024];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    std::fclose(f);
    return out;
}

bool file_exists(const std::string& p) {
    std::FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) return false;
    std::fclose(f);
    return true;
}

}  // namespace

// ---------------------------------------------------------------- log

NXTEST(log_parse_level_names) {
    CHECK(log_parse_level("error", LogLevel::Info) == LogLevel::Error);
    CHECK(log_parse_level("WARN", LogLevel::Info) == LogLevel::Warn);
    CHECK(log_parse_level("Info", LogLevel::Error) == LogLevel::Info);
    CHECK(log_parse_level("debug", LogLevel::Info) == LogLevel::Debug);
    CHECK(log_parse_level("TRACE", LogLevel::Info) == LogLevel::Trace);
    CHECK(log_parse_level("0", LogLevel::Info) == LogLevel::Error);
    CHECK(log_parse_level("4", LogLevel::Info) == LogLevel::Trace);
    CHECK(log_parse_level("9", LogLevel::Debug) == LogLevel::Debug);
    CHECK(log_parse_level("bogus", LogLevel::Warn) == LogLevel::Warn);
    CHECK(log_parse_level("", LogLevel::Warn) == LogLevel::Warn);
    CHECK(log_parse_level(nullptr, LogLevel::Trace) == LogLevel::Trace);
}

NXTEST(log_level_names) {
    CHECK(std::strcmp(log_level_name(LogLevel::Error), "error") == 0);
    CHECK(std::strcmp(log_level_name(LogLevel::Warn), "warn") == 0);
    CHECK(std::strcmp(log_level_name(LogLevel::Info), "info") == 0);
    CHECK(std::strcmp(log_level_name(LogLevel::Debug), "debug") == 0);
    CHECK(std::strcmp(log_level_name(LogLevel::Trace), "trace") == 0);
}

NXTEST(log_line_format_via_sink) {
    Capture cap;
    SinkGuard sg(&cap);
    quiet_init(LogLevel::Info);
    log_set_thread_name("main");
    logf(LogLevel::Info, "kernel", "hello %d", 5);
    REQUIRE(cap.lines.size() == 1);
    const std::string& line = cap.lines[0];
    CHECK(!line.empty() && line[0] == '[');
    CHECK(contains(line, " I t"));
    char tid[32];
    std::snprintf(tid, sizeof tid, " t%lu ", static_cast<unsigned long>(GetCurrentThreadId()));
    CHECK(contains(line, tid));
    CHECK(contains(line, " main] kernel: hello 5"));
    CHECK(cap.levels[0] == LogLevel::Info);
    CHECK(line.find('.') < line.find(" I t"));  // seconds with six decimals come first
    log_set_thread_name("");
}

NXTEST(log_unnamed_thread_shows_dash) {
    Capture cap;
    SinkGuard sg(&cap);
    quiet_init(LogLevel::Info);
    std::thread t([] { logf(LogLevel::Info, "thr", "unnamed"); });
    t.join();
    REQUIRE(cap.lines.size() == 1);
    CHECK(contains(cap.lines[0], " -] thr: unnamed"));
}

NXTEST(log_thread_names_from_two_threads) {
    Capture cap;
    SinkGuard sg(&cap);
    quiet_init(LogLevel::Info);
    std::thread a([] {
        log_set_thread_name("alpha");
        logf(LogLevel::Info, "thr", "from a");
    });
    std::thread b([] {
        log_set_thread_name("beta");
        logf(LogLevel::Info, "thr", "from b");
    });
    a.join();
    b.join();
    CHECK(count_containing(cap.lines, " alpha] thr: from a") == 1);
    CHECK(count_containing(cap.lines, " beta] thr: from b") == 1);
    CHECK(log_thread_name()[0] == '\0');  // this thread never named itself
}

NXTEST(log_level_filter_and_counts) {
    Capture cap;
    SinkGuard sg(&cap);
    quiet_init(LogLevel::Warn);
    logf(LogLevel::Info, "lv", "i");
    logf(LogLevel::Debug, "lv", "d");
    logf(LogLevel::Warn, "lv", "w");
    logf(LogLevel::Error, "lv", "e");
    CHECK(log_count(LogLevel::Info) == 0);
    CHECK(log_count(LogLevel::Debug) == 0);
    CHECK(log_count(LogLevel::Warn) == 1);
    CHECK(log_count(LogLevel::Error) == 1);
    CHECK(cap.lines.size() == 2);  // the sink only sees lines that pass the filter
    CHECK(count_containing(cap.lines, "lv: i") == 0);
}

NXTEST(log_enabled_and_macro_skips_args) {
    Capture cap;
    SinkGuard sg(&cap);
    quiet_init(LogLevel::Info);
    CHECK(log_enabled(LogLevel::Error));
    CHECK(log_enabled(LogLevel::Info));
    CHECK(!log_enabled(LogLevel::Debug));
    CHECK(log_level() == LogLevel::Info);
    int evaluated = 0;
    auto bump = [&] { return ++evaluated; };
    NXLOG_T("t", "x %d", bump());
    NXLOG_D("t", "x %d", bump());
    CHECK(evaluated == 0);  // the disabled macros do not evaluate their arguments
    NXLOG_I("t", "x %d", bump());
    CHECK(evaluated == 1);
    CHECK(count_containing(cap.lines, "t: x 1") == 1);
}

NXTEST(log_long_message_truncated) {
    Capture cap;
    SinkGuard sg(&cap);
    quiet_init(LogLevel::Info);
    std::string big(3000, 'x');
    logf(LogLevel::Info, "big", "%s", big.c_str());
    std::string huge(5000, 'y');  // longer than the format buffer as well
    logf(LogLevel::Info, "big", "%s", huge.c_str());
    REQUIRE(cap.lines.size() == 2);
    for (const auto& line : cap.lines) {
        size_t pos = line.find("big: ");
        REQUIRE(pos != std::string::npos);
        std::string body = line.substr(pos + 5);
        CHECK(body.size() == 2003);  // 2000 characters plus "..."
        CHECK(body.compare(body.size() - 3, 3, "...") == 0);
    }
}

NXTEST(log_newlines_become_spaces) {
    Capture cap;
    SinkGuard sg(&cap);
    quiet_init(LogLevel::Info);
    logf(LogLevel::Info, "nl", "a%sb", "\n");
    REQUIRE(cap.lines.size() == 1);
    CHECK(contains(cap.lines[0], "nl: a b"));
    CHECK(cap.lines[0].find('\n') == std::string::npos);
}

NXTEST(log_file_output_is_written) {
    std::string p = temp_path("file");
    REQUIRE(!p.empty());
    remove_log_files(p);
    clear_env();
    config_clear();
    LogOptions o;
    o.path = p.c_str();
    o.level = LogLevel::Info;
    o.to_stderr = false;
    o.keep_previous = true;
    log_init(o);
    CHECK(std::string(log_path()) == p);
    logf(LogLevel::Info, "io", "first run %d", 1);
    log_flush();
    std::string text = read_file(p);
    CHECK(contains(text, "io: first run 1"));
    CHECK(!text.empty() && text.back() == '\n');  // text mode: the newline is written as CRLF on Windows
    log_shutdown();
    CHECK(std::string(log_path()).empty());
    remove_log_files(p);
}

NXTEST(log_prev_rotation_on_second_init) {
    std::string p = temp_path("rotate");
    REQUIRE(!p.empty());
    remove_log_files(p);
    clear_env();
    config_clear();
    LogOptions o;
    o.path = p.c_str();
    o.level = LogLevel::Info;
    o.to_stderr = false;
    o.keep_previous = true;
    log_init(o);
    logf(LogLevel::Info, "rot", "older text");
    log_init(o);  // second init: the old file must move to <path>.prev
    logf(LogLevel::Info, "rot", "newer text");
    log_shutdown();
    std::string prev = p + ".prev";
    CHECK(file_exists(prev));
    CHECK(contains(read_file(prev), "rot: older text"));
    std::string cur = read_file(p);
    CHECK(contains(cur, "rot: newer text"));
    CHECK(!contains(cur, "older text"));
    remove_log_files(p);
    CHECK(!file_exists(p));
    CHECK(!file_exists(prev));
}

NXTEST(log_env_level_overrides_options) {
    quiet_init(LogLevel::Error);
    CHECK(log_level() == LogLevel::Error);
    _putenv_s("NXRT_LOG_LEVEL", "debug");
    LogOptions o;
    o.path = "";
    o.level = LogLevel::Error;
    o.to_stderr = false;
    log_init(o);
    CHECK(log_level() == LogLevel::Debug);
    _putenv_s("NXRT_LOG_LEVEL", "4");
    log_init(o);
    CHECK(log_level() == LogLevel::Trace);
    _putenv_s("NXRT_LOG_LEVEL", "bogus");  // invalid: the option level is used
    log_init(o);
    CHECK(log_level() == LogLevel::Error);
    clear_env();
}

NXTEST(log_env_path_overrides_options) {
    std::string p = temp_path("envpath");
    REQUIRE(!p.empty());
    remove_log_files(p);
    clear_env();
    config_clear();
    _putenv_s("NXRT_LOG", p.c_str());
    LogOptions o;
    o.path = "";  // no file by option; the environment must still win
    o.to_stderr = false;
    log_init(o);
    CHECK(std::string(log_path()) == p);
    logf(LogLevel::Info, "envp", "via env");
    log_shutdown();
    CHECK(contains(read_file(p), "envp: via env"));
    clear_env();
    remove_log_files(p);
}

// ---------------------------------------------------------------- config

NXTEST(config_sections_keys_case_insensitive) {
    clear_env();
    config_load_string("[Diag]\nWatchdog_Seconds = 10\n[log]\nlevel=debug\n");
    CHECK(config_get_int("diag", "watchdog_seconds", 0) == 10);
    CHECK(config_get_int("DIAG", "WATCHDOG_SECONDS", 0) == 10);
    CHECK(config_get_str("LOG", "Level", "") == "debug");
    CHECK(config_get_str("log", "nope", "d") == "d");
    config_clear();
}

NXTEST(config_root_section_and_duplicates) {
    clear_env();
    config_load_string("foo = bar\n[s]\nk=1\nk=2\n[ s ]\nm = 3\n");
    CHECK(config_get_str("", "foo", "") == "bar");
    CHECK(config_get_int("s", "k", 0) == 2);  // later duplicate wins
    CHECK(config_get_int("s", "m", 0) == 3);  // padded header name
    config_clear();
}

NXTEST(config_comments_and_quotes) {
    clear_env();
    config_load_string(
        "; full line comment\n"
        "# another comment\n"
        "[c]\n"
        "a = v ; trailing\n"
        "b = x#y\n"
        "c=a;b\n"
        "d = \"q ; not # comment\"   \n"
        "e = \"unclosed\n"
        "f = ;notvalue\n"
        "garbage line without equals\n");
    CHECK(config_get_str("c", "a", "") == "v");
    CHECK(config_get_str("c", "b", "") == "x#y");
    CHECK(config_get_str("c", "c", "") == "a;b");
    CHECK(config_get_str("c", "d", "") == "q ; not # comment");
    CHECK(config_get_str("c", "e", "") == "unclosed");
    CHECK(config_get_str("c", "f", "def") == "");
    config_clear();
}

NXTEST(config_int_parsing) {
    clear_env();
    config_load_string(
        "[n]\n"
        "h = 0x1F\n"
        "neg = -42\n"
        "bad = 12abc\n"
        "empty =\n"
        "big = 9223372036854775807\n"
        "hx = 0XfF\n");
    CHECK(config_get_int("n", "h", 7) == 31);
    CHECK(config_get_int("n", "neg", 7) == -42);
    CHECK(config_get_int("n", "bad", 7) == 7);
    CHECK(config_get_int("n", "empty", 7) == 7);
    CHECK(config_get_int("n", "big", 7) == INT64_MAX);
    CHECK(config_get_int("n", "hx", 0) == 255);
    CHECK(config_get_int("n", "missing", 7) == 7);
    config_clear();
}

NXTEST(config_bool_and_double_parsing) {
    clear_env();
    config_load_string(
        "[b]\n"
        "a = yes\n"
        "b = OFF\n"
        "c = 1\n"
        "d = maybe\n"
        "e = True\n"
        "[f]\n"
        "dv = 2.5\n"
        "dbad = x\n");
    CHECK(config_get_bool("b", "a", false) == true);
    CHECK(config_get_bool("b", "b", true) == false);
    CHECK(config_get_bool("b", "c", false) == true);
    CHECK(config_get_bool("b", "d", true) == true);   // invalid: default
    CHECK(config_get_bool("b", "e", false) == true);
    CHECK(config_get_bool("b", "none", true) == true);
    CHECK(config_get_double("f", "dv", 0.0) == 2.5);
    CHECK(config_get_double("f", "dbad", 1.25) == 1.25);
    config_clear();
}

NXTEST(config_env_beats_ini_beats_default) {
    clear_env();
    config_load_string("[diag]\nwatchdog_seconds = 10\n");
    CHECK(config_get_int("diag", "watchdog_seconds", 99) == 10);
    CHECK(std::string(config_source("diag", "watchdog_seconds")) == "ini");
    _putenv_s("NXRT_DIAG_WATCHDOG_SECONDS", "30");
    CHECK(config_get_int("diag", "watchdog_seconds", 99) == 30);
    CHECK(std::string(config_source("diag", "watchdog_seconds")) == "env");
    CHECK(config_has("diag", "watchdog_seconds"));
    _putenv_s("NXRT_DIAG_WATCHDOG_SECONDS", "");
    CHECK(config_get_int("diag", "watchdog_seconds", 99) == 10);
    CHECK(config_get_int("diag", "missing", 99) == 99);
    CHECK(std::string(config_source("diag", "missing")) == "default");
    CHECK(!config_has("diag", "missing"));
    config_clear();
}

NXTEST(config_set_and_load_replace) {
    clear_env();
    config_clear();
    config_set("x", "k", "5");
    CHECK(config_get_int("x", "k", 0) == 5);
    CHECK(std::string(config_source("x", "k")) == "ini");
    config_load_string("[x]\nk = 7\n");
    CHECK(config_get_int("x", "k", 0) == 7);  // loading replaces config_set values
    config_load_string("");
    CHECK(config_get_int("x", "k", 0) == 0);
    config_set("x", "k", "9");
    config_clear();
    CHECK(config_get_int("x", "k", 0) == 0);  // clear empties everything
}

NXTEST(config_env_name_examples) {
    CHECK(config_env_name("diag", "watchdog_seconds") == "NXRT_DIAG_WATCHDOG_SECONDS");
    CHECK(config_env_name("", "foo") == "NXRT_FOO");
    CHECK(config_env_name("my-sec", "a.b") == "NXRT_MY_SEC_A_B");
    CHECK(config_env_name("log", "level") == "NXRT_LOG_LEVEL");
}

NXTEST(config_load_file_roundtrip_and_missing) {
    std::string p = temp_path("cfg");
    REQUIRE(!p.empty());
    std::remove(p.c_str());
    clear_env();
    std::FILE* f = std::fopen(p.c_str(), "wb");
    REQUIRE(f != nullptr);
    std::fputs("[file]\nval = 42\n", f);
    std::fclose(f);
    CHECK(config_load_file(p.c_str()));
    CHECK(config_get_int("file", "val", 0) == 42);
    std::remove(p.c_str());
    CHECK(!config_load_file(p.c_str()));  // missing file: false, and the config is left empty
    CHECK(config_get_int("file", "val", 0) == 0);
    config_clear();
}

NXTEST(config_log_effective_lines) {
    quiet_init(LogLevel::Info);
    config_load_string("[diag]\nwatchdog_seconds = 10\n");
    CHECK(config_get_int("diag", "watchdog_seconds", 3) == 10);
    CHECK(config_get_str("", "probe_name", "x") == "x");
    Capture cap;
    SinkGuard sg(&cap);
    config_log_effective();
    CHECK(count_containing(cap.lines, "config: [diag] watchdog_seconds = 10 (ini)") == 1);
    CHECK(count_containing(cap.lines, "config: probe_name = x (default)") == 1);
    config_clear();
}

NXTEST(log_tail_keeps_the_newest_lines_in_order) {
    quiet_init(LogLevel::Info);
    std::vector<LogTailLine> t;
    CHECK_EQ(log_tail(&t, 10), 0ull);
    CHECK(t.empty());
    for (int i = 0; i < 5; ++i) NXLOG_I("tail", "line %d", i);
    NXLOG_W("tail", "a warning");
    NXLOG_D("tail", "filtered out: not kept");
    CHECK_EQ(log_tail(&t, 3), 6ull);
    REQUIRE(t.size() == 3);
    CHECK(contains(t[0].text, "tail: line 3"));
    CHECK(contains(t[1].text, "tail: line 4"));
    CHECK(contains(t[2].text, "tail: a warning") && t[2].level == LogLevel::Warn);
    // more lines than the ring holds: the oldest go, the count goes on
    for (unsigned i = 0; i < kLogTailLines + 10; ++i) NXLOG_I("tail", "bulk %u", i);
    CHECK_EQ(log_tail(&t, kLogTailLines * 2), 6ull + kLogTailLines + 10);
    REQUIRE(t.size() == kLogTailLines);
    CHECK(contains(t.front().text, "tail: bulk 10"));
    const std::string last = "tail: bulk " + std::to_string(kLogTailLines + 9);
    CHECK(contains(t.back().text, last.c_str()));
    quiet_init(LogLevel::Info);   // a new log starts a new tail
    CHECK_EQ(log_tail(&t, 10), 0ull);
}

NXTEST(config_set_default_is_not_a_user_setting) {
    clear_env();
    config_clear();
    CHECK(!config_user_set("game", "upscaler"));
    config_set_default("game", "upscaler", "abc");
    CHECK(config_has("game", "upscaler"));
    CHECK(config_get_str("game", "upscaler", "") == "abc");
    CHECK(!config_user_set("game", "upscaler"));   // a default: the settings file of the player may replace it
    config_set("game", "upscaler", "");             // the choice of the player (an empty value turns it off)
    CHECK(config_user_set("game", "upscaler"));
    CHECK(config_get_str("game", "upscaler", "x").empty());
    config_load_string("[game]\nother = 1\n");
    CHECK(config_user_set("game", "other"));       // from the ini
    _putenv_s("NXRT_GAME_ENVKEY", "7");
    CHECK(config_user_set("game", "envkey"));      // from the environment
    _putenv_s("NXRT_GAME_ENVKEY", "");
    config_clear();
}
