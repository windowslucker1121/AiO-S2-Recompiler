// SPDX-License-Identifier: GPL-3.0-or-later
// [nvn] cmd_thread (nvn_cmdq.cpp): the render thread's vkCmd* calls are made by a recorder thread, in order, from a ring of copied calls.
#pragma once
#include <cstdint>
#include <mutex>
#include <string>

#include <volk.h>

namespace nxrt::nvn::be::cmdq {

bool enabled();                       // [nvn] cmd_thread (read once)
// At the start of a submission (render thread, nothing recorded yet): installs the hooks the first time, waits for the recorder, and
// switches queuing on or off for what follows (paired windows).
void begin_submission(bool on);
void set_pds(bool on);                // [nvn] cmd_thread_pds under cpuperf_ab (pds_q): push descriptor sets queued or drained
// [nvn] chunk_async (pds_q's sibling, cpuperf_ab_what chunk_async): ends `old`, submits it (no fence, no signal) and begins `next` on the
// recorder thread, in order. False: not queuing now; the caller does it itself.
bool queue_chunk(VkCommandBuffer old, VkCommandBuffer next, VkQueue q, std::mutex* mu);
void set_chunk_async(bool on);
void set_copies(bool on);             // [nvn] cmd_thread_copies under cpuperf_ab (cmd_copies): image copies and clears queued or drained
void apply_render_priority(bool on);   // [nvn] render_priority: the calling (render) thread and the recorder; on false: NORMAL
void set_publish_batch(bool on);       // [nvn] cmd_publish_batch under cpuperf_ab (cmd_batch)
void set_desc(bool on);               // [nvn] cmd_thread_desc under cpuperf_ab (cmd_desc): descriptor set updates queued or made at once
void desc_stats(uint64_t* queued, uint64_t* direct);
void drain();                        // render thread: waits until every queued call has been made
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
