// SPDX-License-Identifier: GPL-3.0-or-later
// [nvn] hazard (nvn_hazard.cpp): full memory barriers only where a command touches memory or an image that an earlier command wrote (or
// writes what an earlier one read) since the last barrier, decided from the Vulkan commands the render thread records.
#pragma once
#include <cstdint>
#include <string>

#include <volk.h>

namespace nxrt::nvn::be::hazard {

// Right after volkLoadDevice: hooks on object creation (shader modules, pipelines, buffers, memory imports, views, descriptor sets), so the
// tracker knows what a bound pipeline or descriptor set touches. Cheap; always installed unless [nvn] hazard = off at that time.
void install_create_hooks();
// From the command queue's install (nvn_cmdq.cpp): the hooks on the vkCmd* calls, in front of the queue's own.
void install_cmd_hooks();
// The replay at the start of each submission (render thread, after cmdq::begin_submission): `on` from [nvn] hazard and cpuperf_ab.
void begin_submission(VkCommandBuffer cb, bool on, bool piggyback = true);
// memory_barrier (nvn_gpu.cpp): true when the tracker decides the barriers of this command buffer; the call is then only counted.
bool takes(VkCommandBuffer cb);
void request(uint32_t line);
// frame_chunk / frame_split (nvn_gpu.cpp): the submission goes on in another command buffer.
void switch_cb(VkCommandBuffer from, VkCommandBuffer to);
// Commands recorded without the hooks follow (an upscaler SDK): a barrier now if anything is unordered, everything unknown after.
void external(VkCommandBuffer cb);
// Buffers made through VMA (vk_device.cpp create_buffer), whose vkCreateBuffer the hooks do not see.
void note_buffer(VkBuffer b, VkBufferUsageFlags usage, VkDeviceSize size);
bool enabled();               // [nvn] hazard != off
bool verify();                // [nvn] hazard = verify
std::string stats_line();     // the 10 s statistics line

}  // namespace nxrt::nvn::be::hazard
