// SPDX-License-Identifier: GPL-3.0-or-later
// Gaps: what the game asked for that the runtime does not implement, counted per run so the missing work can be listed without looking at
// every scene. Each place that answers a request with a stand-in (a logging stub, a skipped draw, an operation left out, a fake IPC reply)
// calls gap() every time it happens; the counts go to `gaps.tsv` next to the run's log (rewritten every 30 s while new gaps come in and at
// the end of the run) and the most frequent ones to the end-of-run log. tools/ai/gaps.py merges runs into a ranked work list.
//
//   nxrt::gap("nvn-proc", "stub called", "nvnCommandBufferDrawMeshTasks");
//   nxrt::gap("draw", "untranslated shader", hash);        // `detail` names the instance (a hash, a name, a command number)
//
// Areas in use: nvn-proc (an NVN entry point with no implementation), nvn (an operation the back end leaves out or answers wrongly),
// draw / dispatch (a draw or dispatch skipped or drawn incompletely, detail = the shader that was missing), import (an unresolved import of
// a lifted module), ipc (an unhandled service command), svc (an unknown supervisor call). Thread-safe; one mutex and a map lookup per call.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace nxrt {

void gap(const char* area, std::string_view what, std::string_view detail = {});

struct GapRecord {
    std::string area, what, detail;
    uint64_t count = 0;
    double first_s = 0, last_s = 0;   // seconds since the process started
};
std::vector<GapRecord> gaps_snapshot();   // sorted by count, largest first
// Writes gaps.tsv now (next to the log, or `path` when given). False when there is nowhere to write.
bool gaps_write(const std::string& path = {});
// The `n` most frequent gaps, one per line, for the end-of-run log.
std::string gaps_report(size_t n = 20);
// gaps_write() plus the `n` most frequent gaps as log lines of their own (end of run; a summary line would cut them off).
void gaps_log(size_t n = 25);

}  // namespace nxrt
