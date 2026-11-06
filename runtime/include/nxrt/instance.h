// SPDX-License-Identifier: GPL-3.0-or-later
// Named instances: several runs of one game on one machine at the same time (the game exe's --instance NAME [--instance-dir DIR],
// or NXRT_INSTANCE / NXRT_INSTANCE_DIR). An instance writes everything of its own into its folder: the log, the crash / freeze / fatal
// reports, save data and temporary storage, controls.ini, the pipeline cache and pipeline key file, the shader order, screenshots, test
// output and the outputs of diagnostic keys given as relative paths. What stays shared: the game files, the shader cache and the BC7 cache
// (read by every run; new entries are written under a temporary name and moved into place, so concurrent runs never see a partial file).
// The default run (no instance) keeps the old places (the game folder, next to the exe, %APPDATA%). The rules are in AGENTS.md.
#pragma once
#include <string>

namespace nxrt {

// Before nxrt::init: this run is the instance `name` with its own files in `dir` (an absolute path; the folder is created).
// An empty name is the default run.
void instance_set(const std::string& name, const std::string& dir);
const std::string& instance_name();     // "" = the default run
const std::string& instance_dir();      // absolute, "" = the default run
bool instance_active();

// The default file of a per-run report: "<exe path without its extension><suffix>" for the default run (next to the exe, as before),
// "<instance dir>\<exe file stem><suffix>" for an instance. suffix e.g. "_crash.txt".
std::string instance_report_path(const char* suffix);
// The folder of per-run request files such as dump_threads: the exe's folder, or the instance folder.
std::string instance_request_dir();

// A file or folder the run writes, named relative to the working folder (the game folder): unchanged for the default run, an empty path
// or an absolute path; inside the instance folder for an instance.
std::string instance_file(const std::string& path);

// nxrt::init calls it right after the configuration is loaded, before anything opens an output file. For an instance: every key of a file
// the run writes defaults to the instance folder (a key given as a relative path is moved there; an absolute one is kept), and the
// instance gets its own copies of the shared read-and-write files (controls.ini, pipeline_keys.bin, shader_order.txt) the first time.
// The shader setup window is off unless [nvn] setup_on_start is set (it writes the shared shader data). A no-op for the default run.
void instance_apply_config();

// After [nvn] shader_cache is known (the game exe calls it once its defaults are set): the instance's pipeline cache file in its folder,
// a hard link to (or a copy of) the shared one when the instance has none or the shared one is newer; [nvn] pipeline_cache then names
// it. A no-op for the default run and when [nvn] pipeline_cache is set to an absolute path or to nothing.
void instance_link_pipeline_cache();

}  // namespace nxrt
