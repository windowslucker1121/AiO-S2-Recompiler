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
// [nvn] hazard on the recorder (nvn_hazard.cpp): on_recorder is true on the recorder thread; scan_pass (recorder, inside a vkCmdBeginRendering
// packet) runs the pass's queued packets dry up to its vkCmdEndRendering (or until the render thread needs the recorder); replay_scanned then
// runs them for real. post: fn(cb) on the recorder in order with the queued calls (now when not queuing).
bool on_recorder();
void scan_pass();
void replay_scanned();
void scan_stats(uint64_t* scans, uint64_t* stopped, uint64_t* waits);
void post(void (*fn)(VkCommandBuffer), VkCommandBuffer cb);
// fn(words, n) on the recorder in order with the queued calls; false (nothing queued) when this thread does not queue now.
bool post_words(void (*fn)(const uint64_t*, uint32_t), const uint64_t* w, uint32_t n);
// [nvn] chunk_on_recorder (default on): frame_chunk's vkEndCommandBuffer(cb), vkQueueSubmit2 (under *mu) and vkBeginCommandBuffer(next), then
// after(cb, next), made by the recorder in order with the queued calls, so the render thread does not wait for it. False: not queuing.
bool chunk_submit(VkCommandBuffer cb, VkCommandBuffer next, VkQueue q, std::mutex* mu, void (*after)(VkCommandBuffer, VkCommandBuffer));
void set_chunk(bool on);   // cpuperf_ab (chunk_rec)
// The driver's vkCmdPipelineBarrier2 behind the queue (for the placeholder and the hazard tracker's own barriers when not queuing).
void raw_barrier(VkCommandBuffer cb, const VkDependencyInfo* di);

}  // namespace nxrt::nvn::be::cmdq
