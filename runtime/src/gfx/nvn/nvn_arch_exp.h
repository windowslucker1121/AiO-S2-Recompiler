// SPDX-License-Identifier: GPL-3.0-or-later
// Architecture experiments (measurement only, all off by default; nothing here changes what the replay does):
//   E1  [nvn] exp_lists = 1        how many draws come from command lists whose content repeats a list of the last 8 frames
//                                  (exact bytes / address fields masked / structure only), logged every 10 s.
//   E2a [nvn] exp_coherence = N    does the game keep NVN's Flush/Invalidate contract? Every N-th frame the pages the submitted lists
//                                  read through buffer bindings are hashed at nvnQueueSubmitCommands; a page whose bytes changed since
//                                  it was last seen without a FlushMappedRange over it (and that no GPU command wrote) counts as an
//                                  unflushed CPU write. Guard faults on held compute results are split by a preceding InvalidateMappedRange.
//                                  Per pool type (CPU_CACHED 0x24 / CPU_UNCACHED 0x22 / other), logged every 10 s.
//   E4  [nvn] exp_emit = 1         the render thread's CPU time split into vkCmd* calls (emit), descriptor allocation/update, queue
//                                  submission and the rest (decide), per presented frame, logged every 10 s.
#pragma once
#include <cstdint>
#include <vector>

#include "nvn_cmd.h"

namespace nxrt::nvn::be::archexp {

// E1
bool lists_on();
void list_recorded(const CmdList& l);                 // nvnCommandBufferEndRecording (any thread)
void game_present();                                   // nvnQueuePresentTexture (game thread): the frame boundary of E1 and E2a

// E2a
bool coherence_on();
void note_flush(uint64_t addr, uint64_t size);         // nvnMemoryPool/BufferFlushMappedRange
void note_invalidate(uint64_t addr, uint64_t size);    // nvnMemoryPool/BufferInvalidateMappedRange
void note_gpu_write(uint64_t addr, uint64_t size);     // a command may write pool memory (render thread)
void submit_sample(const std::vector<Ref<CmdList>>& lists);   // nvnQueueSubmitCommands (game thread)
void note_guard_fault(uint64_t addr, int access, int pool_flags);   // a CPU access of a held compute result (any thread)

// E4
bool emit_on();
void render_frame_tick();                              // render thread, once per present

}  // namespace nxrt::nvn::be::archexp
