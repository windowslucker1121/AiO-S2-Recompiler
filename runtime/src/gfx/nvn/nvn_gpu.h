// SPDX-License-Identifier: GPL-3.0-or-later
// The boundary between the NVN entry points (host side, api_*.cpp) and the GPU back end (render thread, nvn_gpu*.cpp,
// nvn_replay.cpp). The api files only use the functions below; they never include Vulkan. nvn_gpu_stub.cpp implements the same
// functions without a GPU (a serial counter and nothing else) so the api files can be unit-tested on their own.
#pragma once
#include <cstdint>
#include <vector>

#include "nvn_cmd.h"
#include "nvn_core.h"

namespace nxrt::nvn::be::gpu {

// True when a Vulkan back end is running (install_backend was called with a device). The stub reports false.
bool active();
// True when the Vulkan back end can show debug labels (VK_EXT_debug_utils): the debug group commands are recorded only then. The stub reports false.
bool debug_labels();

// ---- submission --------------------------------------------------------------------------------------------------------
// Replays the lists on the render thread, in order, and returns when the commands are recorded into Vulkan (not when the GPU
// has run them). Returns the serial of the submission: the GPU timeline value that is reached when it has completed.
uint64_t submit(const std::vector<Ref<CmdList>>& lists);
// Submits whatever is pending (nothing is pending in this design: submit() always submits) and returns the last serial.
uint64_t flush();
// Waits until every submission so far has completed on the GPU and its write-backs are visible to the CPU.
void finish();
// The same, called on the render thread itself (inside a submission's work).
void finish_on_render_thread();
uint64_t last_serial();
// Waits for `serial` to complete (timeout_ns: 0 poll, UINT64_MAX forever). True when it did.
bool wait_serial(uint64_t serial, uint64_t timeout_ns);
bool serial_done(uint64_t serial);
// The game's nvnSyncWait on a queue fence: wait_serial, or with [nvn] sync_lead the frame throttle relaxed by one frame (nvn_gpu.cpp).
bool wait_serial_game(uint64_t serial, uint64_t timeout_ns);
void sync_lead_enable(bool on);   // [nvn] cpuperf_ab_what sync_lead: off in the off windows

// Window: texture `index` of the window is shown (after the submissions already made). The call returns when the frame has
// been handed to the swapchain; it honours the shell's frame limiter.
void present(Window* window, int index);

// ---- resources (called from destructors and finalizers, any thread) --------------------------------------------------------
void destroy_texture_gpu(TexGpu* t);          // the Vulkan image of a finalized texture; deferred until the GPU is done with it
void destroy_pool_gpu(PoolGpu* p);
// The game finalized t (or initialized its NVNtexture again): its image goes now, deferred until the GPU is done with it, although a texture
// pool entry may keep the object alive until the game registers another texture at that id. The console's pool entry is a descriptor that
// does not own the texture; ACNH finalizes ~300 textures a second and leaves most of them in its 8192-entry pool. Should the object be used
// again, the image is made again from its storage. Queued behind the submissions already made (they are replayed in order).
void release_texture_gpu(Texture* t);
// [nvn] tex_park: the GPU-written image of a finalized texture is kept a few presents (any thread; false: not kept, destroy it as before). A
// texture initialized again over the same storage with the same layout takes it back (tex_or_null) instead of a new image and an upload of
// the storage: the game makes its upscaler chain's 1920x1080 targets anew every frame (8.6 MB CPU untiles each). parked_cpu_wrote: the CPU wrote
// [lo, hi): parked images there are stale (uploaded again when taken back).
bool tex_park(const Texture& t, TexGpu* g);
bool tex_release_shared(TexGpu* g);   // render thread: drops a finalized object's share of an image others still use (true: done, nothing to destroy)
void parked_cpu_wrote(uint64_t lo, uint64_t hi);
// A pool was created: import it into Vulkan on a background thread so the first draw that uses it does not wait for the driver (the import
// pins the pages; it takes 10 to 80 ms). A no-op without a back end and for virtual pools.
void prefetch_pool(MemoryPool* p);
// A program got its shaders: queue the recorded pipelines that use exactly these shaders (nvn_prewarm.cpp). A no-op without a back end.
void prewarm_program_shaders(const ShaderInfo* const stages[6]);
// A command buffer was recorded with this program bound (nvn_gpl.cpp): the shader parts of its pipelines are compiled first. Any thread.
void program_bound(const ShaderInfo* const stages[6]);
// The CPU wrote texture memory (nvnMemoryPoolFlushMappedRange, nvnTextureWriteTexels ...): upload again before the next use.
void mark_texture_cpu_dirty(Texture* t);
// A texture is about to be sampled: it was put into a texture pool (nvnTexturePoolRegisterTexture / Image) or its handle was bound while a command
// list was recorded (nvnCommandBufferBindTexture*). Its upload data is prepared in the background (nvn_texprep.h) if the CPU wrote it. Any thread.
void texture_bound(Texture* t);
void texture_registered(Texture* t);   // the registration half of the above: only big textures are acted on

// The GPU image of a texture is copied back into the texture's storage in the game's memory in its block-linear layout (for
// nvnTextureReadTexels / CopyTextureToBuffer users that read render targets on the CPU). Waits for the GPU. False when the
// texture has no GPU image yet or cannot be read back.
bool download_texture(Texture* t);

// nvnMemoryPoolInvalidateMappedRange / nvnBufferInvalidateMappedRange: the CPU is about to read [addr, +size), which the GPU may have
// written. Compute results the back end holds in video memory there are copied back first (waits for the GPU). Any thread.
void cpu_will_read(uint64_t addr, uint64_t size);
void flush_copy_cpu_wrote(uint64_t addr, uint64_t n);   // [nvn] flush_copy: a CPU flush of pool memory (any thread)
void flush_copy_enable(bool on);                        // cpuperf_ab (flush_copy)
void pt_copy_enable(bool on);                           // cpuperf_ab (pt_copy)
void pt_note_pointer(uint64_t v);                       // [nvn] pt_copy: a pointer a pointer shader may follow

// nvnDeviceSetWindowOriginMode / nvnDeviceSetDepthMode: the replay needs them to place viewports and the depth range.
void set_window_modes(int origin_mode, int depth_mode);

// ---- device facts for the api files ------------------------------------------------------------------------------------
uint64_t timestamp_ns();                       // nvnDeviceGetCurrentTimestampInNanoseconds

}  // namespace nxrt::nvn::be::gpu
