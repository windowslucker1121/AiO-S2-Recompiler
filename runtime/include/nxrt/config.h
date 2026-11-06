// SPDX-License-Identifier: GPL-3.0-or-later
// Configuration: an ini file plus environment-variable switches.
//   [section]
//   key = value        ; comment    # comment
// Lookup order for config_get_*("diag", "watchdog_seconds", def):
//   1. environment variable NXRT_DIAG_WATCHDOG_SECONDS   (NXRT_ + upper-case section + '_' + upper-case key)
//   2. the ini value
//   3. the default
// Keys outside any section use the section name "" and the env name NXRT_<KEY>.
#pragma once
#include <cstdint>
#include <string>

namespace nxrt {

// Loads (replaces) the ini from a file. Returns false when the file cannot be read (the config stays empty).
bool config_load_file(const char* path);
// Loads from a string (tests). Replaces the current ini content.
void config_load_string(const char* text);
void config_clear();
// Programmatic override, as if it were in the ini (tests, command-line options). Env still wins over it.
void config_set(const char* section, const char* key, const char* value);
// A default set in code (a game module's own defaults, game::Info::config_defaults): read like an ini value, but config_user_set() says false
// for it, so a later choice of the player (controls.ini [config]) may replace it with config_set. Env wins over it.
void config_set_default(const char* section, const char* key, const char* value);
// The environment, the ini file or config_set gives the key (a config_set_default value or no value: false).
bool config_user_set(const char* section, const char* key);

std::string config_get_str(const char* section, const char* key, const char* def = "");
int64_t config_get_int(const char* section, const char* key, int64_t def = 0);      // decimal, 0x hex, optional k/m/g suffix not supported
double config_get_double(const char* section, const char* key, double def = 0.0);
bool config_get_bool(const char* section, const char* key, bool def = false);       // 1/0 true/false yes/no on/off, case-insensitive
bool config_has(const char* section, const char* key);                              // set by env or ini

// Where a value came from: "env", "ini" or "default". For the startup dump.
const char* config_source(const char* section, const char* key);
// Logs (Info, tag "config") every key read so far with its effective value and source.
void config_log_effective();
// The environment variable name used for a key, e.g. NXRT_DIAG_WATCHDOG_SECONDS.
std::string config_env_name(const char* section, const char* key);

}  // namespace nxrt
