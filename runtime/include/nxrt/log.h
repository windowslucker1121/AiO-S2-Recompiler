// SPDX-License-Identifier: GPL-3.0-or-later
// Leveled logging to a file (and optionally stderr). Thread safe; never allocates unbounded memory.
// Line format:  [  12.345678 I t1234 name] tag: message
//   seconds since log_init, level letter (E W I D T), host thread id, guest/host thread name (log_set_thread_name).
#pragma once
#include <cstdarg>
#include <cstdint>
#include <string>
#include <vector>

namespace nxrt {

enum class LogLevel : int { Error = 0, Warn = 1, Info = 2, Debug = 3, Trace = 4 };

struct LogOptions {
    const char* path = nullptr;      // null: "nxrt.log" in the current directory ("" = no file)
    LogLevel level = LogLevel::Info;
    bool to_stderr = true;           // mirror lines at Warn and above to stderr (everything with stderr_all)
    bool stderr_all = false;
    bool keep_previous = true;       // rename an existing log to <path>.prev first
};

// Initialise (or re-initialise: closes the old file). Reads NXRT_LOG (path) and NXRT_LOG_LEVEL (error|warn|info|debug|trace
// or 0..4) from the environment over `opt`, then the [log] level/path keys of the config (see config.h) if loaded before.
// Calling logf before log_init initialises with defaults.
void log_init(const LogOptions& opt = {});
void log_shutdown();                   // flush and close the file
void log_flush();
void log_set_level(LogLevel l);
LogLevel log_level();
bool log_enabled(LogLevel l);          // l <= current level
const char* log_path();                // path of the file actually open ("" when none)
void log_set_thread_name(const char* name);   // copied; shown in the line prefix of this thread
const char* log_thread_name();
LogLevel log_parse_level(const char* s, LogLevel fallback);
const char* log_level_name(LogLevel l);

// Counters since log_init, for tests and the end-of-run summary.
uint64_t log_count(LogLevel l);        // lines written at exactly this level
// The last lines written (every level that passed the filter), kept in memory for the F1 menu's log view: at most kLogTailLines lines of at most
// 2000 bytes each. log_tail copies up to `max` of the newest, oldest first, and returns the number of lines written since log_init (a line's
// sequence number is that count minus its distance from the end), so a caller can tell whether anything new arrived.
constexpr unsigned kLogTailLines = 512;
struct LogTailLine {
    LogLevel level = LogLevel::Info;
    std::string text;
};
uint64_t log_tail(std::vector<LogTailLine>* out, size_t max);

// Test hook: every formatted line (without the trailing newline) is also passed to the sink. Null removes it.
using LogSink = void (*)(LogLevel level, const char* line, void* user);
void log_set_sink(LogSink sink, void* user);

void logv(LogLevel l, const char* tag, const char* fmt, va_list ap);
void logf(LogLevel l, const char* tag, const char* fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;

#define NXLOG_E(tag, ...) ::nxrt::logf(::nxrt::LogLevel::Error, tag, __VA_ARGS__)
#define NXLOG_W(tag, ...) ::nxrt::logf(::nxrt::LogLevel::Warn, tag, __VA_ARGS__)
#define NXLOG_I(tag, ...) ::nxrt::logf(::nxrt::LogLevel::Info, tag, __VA_ARGS__)
#define NXLOG_D(tag, ...) do { if (::nxrt::log_enabled(::nxrt::LogLevel::Debug)) ::nxrt::logf(::nxrt::LogLevel::Debug, tag, __VA_ARGS__); } while (0)
#define NXLOG_T(tag, ...) do { if (::nxrt::log_enabled(::nxrt::LogLevel::Trace)) ::nxrt::logf(::nxrt::LogLevel::Trace, tag, __VA_ARGS__); } while (0)

}  // namespace nxrt
