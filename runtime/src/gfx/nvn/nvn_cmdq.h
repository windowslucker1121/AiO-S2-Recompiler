// SPDX-License-Identifier: GPL-3.0-or-later
// [nvn] cmd_thread (nvn_cmdq.cpp): the render thread's vkCmd* calls are made by a recorder thread, in order, from a ring of copied calls.
#pragma once
#include <cstdint>
#include <string>

#include <volk.h>

namespace nxrt::nvn::be::cmdq {

bool enabled();                       // [nvn] cmd_thread (read once)
// At the start of a submission (render thread, nothing recorded yet): installs the hooks the first time, waits for the recorder, and
// switches queuing on or off for what follows (paired windows).
void begin_submission(bool on);
void set_copies(bool on);             // [nvn] cmd_thread_copies under cpuperf_ab (cmd_copies): image copies and clears queued or drained
void drain();                         // render thread: waits until every queued call has been made
void stats(uint64_t* packets, uint64_t* drains, uint64_t* direct, uint64_t* spins);
void shutdown();
std::string direct_top();          // statistics: the calls that waited for the recorder most often

// [nvn] hazard (nvn_hazard.cpp): a barrier decided later. ph_insert queues a packet in `cb` that the recorder holds at until ph_resolve
// says whether it is a full memory barrier or nothing (one at a time). Anything that has to wait for the recorder (a drain, a full ring)
// resolves it as a barrier first; ph_pending then turns false without a ph_resolve.
bool queuing();                       // render thread: the hooks queue this thread's calls now
bool ph_insert(VkCommandBuffer cb);
void ph_resolve(bool barrier);
bool ph_pending();
void ph_stats(uint64_t* inserted, uint64_t* forced);
// The driver's vkCmdPipelineBarrier2 behind the queue (for the placeholder and the hazard tracker's own barriers when not queuing).
void raw_barrier(VkCommandBuffer cb, const VkDependencyInfo* di);

}  // namespace nxrt::nvn::be::cmdq
