// SPDX-License-Identifier: GPL-3.0-or-later
// Frame capture and replay (nvn_capture.cpp, runtime/docs/CAPTURE.md): the hooks the back end calls.
#pragma once
#include <memory>
#include <string>
#include <vector>

#include "nvn_cmd.h"
#include "nvn_core.h"

namespace nxrt::nvn::be::capture {

// gpu::submit, game thread, before the lists are queued. True when this is the first submission of a frame being captured: the
// submission then takes a snapshot of the replay state on the render thread before replaying (state_taken).
// recorded: set when the submission belongs to the frame being captured (render thread: frame_submit_begin / frame_submit_end around it).
bool on_submit(const std::vector<Ref<CmdList>>& lists, bool* recorded = nullptr);
void state_taken(std::shared_ptr<void> snap);      // render thread
// Render thread, around the replay of a submission of the frame being captured (first: its first submission, which waits for the GPU
// first). Meanwhile note_gpu_write saves the bytes of each range before the frame's first GPU write to it, so the capture holds the memory
// the frame starts from, not what it left behind (accumulators the game clears on the CPU, results a later pass of the frame overwrites).
void frame_submit_begin(bool first);
void frame_submit_end();
// gpu::present, game thread, before the present is queued: counts frames, polls capture requests, and at the end of a recorded frame
// waits for the GPU and writes the capture (the presenting thread is held for a few seconds).
void on_present(Window* w, int index);
void request(const std::string& name);             // any thread: capture the next whole frame
bool replaying();                                  // this process replays a capture (no game)
// Render thread: a command wrote pool memory [addr, +size). While a replay records its first loop, the ranges are collected: later loops
// put their captured bytes back first, so every loop starts from the captured memory. While a frame is captured, see frame_submit_begin.
void note_gpu_write(uint64_t addr, uint64_t size);
int run_replay(const std::string& folder);

}  // namespace nxrt::nvn::be::capture
