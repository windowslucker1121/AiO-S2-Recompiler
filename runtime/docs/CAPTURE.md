# Frame capture and replay

Debugging the renderer used to mean booting the game and waiting a minute or more for the first 3D scene, every time. A **frame capture**
saves one whole presented frame of the running game to disk. **`--replay-capture`** shows it again in a few seconds, with no game code
running. Every back-end debug tool works on the replay as on the game.

Code: `src/gfx/nvn/nvn_capture.cpp` (recorder, file format, loader, replay loop), hooks in `nvn_gpu.cpp` (`submit`, `present`) and
`nvn_replay.cpp` (`gpu_wrote`; the GPU state snapshot `replay_state_*`), raw image read/write in `nvn_gpu_texture.cpp`
(`tex_image_read` / `tex_image_write`).

## Making a capture

While the game runs, one of:

- `tools/ai` (MCP `game` server): `capture_frame(name="deku", instance="...")`. It returns the capture folder.
- A request file named `capture_frame` (any process) or `capture_frame.<pid>` in the run's request folder: the exe's folder, or the
  instance folder of a named run. Its first line names the capture.
- `[capture] at_present = N`: the frame after present N, once.
- From code: `nxrt::nvn::request_frame_capture(name)`.

The next whole frame (from one present to the next) is recorded. At its end the presenting thread waits for the GPU, then writes
`<[capture] dir>/<yyyymmdd-hhmmss>[-instance][-name]/`:

| File | What |
|---|---|
| `capture.bin` | The frame's events (submissions and the present), its command lists and every list they call, every object they reach (memory pools with their virtual maps, textures, texture and sampler pools, programs, syncs, events, the window), the replay's GPU state at the frame's start (`GState`), the memory table, the device modes (window origin, depth mode) and the table of rendered images. |
| `memory.bin` | Guest memory the GPU reads: every non-virtual memory pool, plus each program's code block and control section. All-zero MiB chunks are left out. |
| `images.bin` | The images the GPU had rendered before the frame (render targets, shadow and probe maps, history buffers), read back raw as the GPU holds them: scaled, promoted to float16, and depth images included. |
| `reference.png` | The game's own picture of the captured frame, for comparing with the replay. |

The first scene is about 3.2 GiB of memory and 0.9 GiB of images. Writing takes 3 to 10 s. Before writing, compute results the back end
holds in device-local copies are copied back to the pools.

## Replaying

```bat
game-build\build\release\oot.exe --replay-capture work\captures\<folder> [--instance NAME] [--max-seconds N]
```

or `replay_capture(id="latest")` through `tools/ai`. The runtime does the following:

1. Starts the window and the NVN back end as for the game. No module is mapped and no game thread runs.
2. Puts the memory back at its original addresses (GPU addresses are identity-mapped host addresses, so the recorded commands work
   unchanged).
3. Rebuilds the objects, patching the object ids in the lists.
4. Finds each program's shaders in the shader cache, so a block re-translated since the capture shows the new translation.
5. Fills the rendered images.
6. Submits and presents the frame in a loop.

Every loop starts from the captured GPU state. With `[capture] reset` on (the default), it also starts from the captured memory: the first
loop records which pool ranges the GPU writes, and those ranges and the images are put back before each later loop. Without the reset,
effects the game steers from the CPU (exposure adaptation, temporal accumulation) drift from loop to loop.

The replay runs about as fast as the game rendered that frame (about 10 fps for the first scene on the RTX 4080).

What works on a replay, unchanged:

- `[nvn] skip_fs_file`: fragment shader hash prefixes, re-read twice a second. Bisect a draw live.
- `[nvn] trace_frame`, `trace_sec`, `trace_snap*` and `tools/ai/gpu_snapshots.py`. Frame numbers count the loops.
- `screenshot` and `compare` against `reference.png`.
- The F1 overlay, `[gfx]` settings, and `[prof] gpu_passes` for GPU timings of one fixed frame.

## Limits

- A capture belongs to the runtime build that wrote it: the size of `GState` and of the object layouts is checked, and a mismatch asks
  for a new capture. Shader translations are looked up fresh, so translator changes need no new capture.
- Memory is read once the frame's last submission has completed. CPU data the game already rewrote for the next frame (a uniform ring with
  fewer slots than frames in flight) can differ from what the frame used.
- `CallCommands` handles the game had already released when the frame was submitted are skipped in the replay, as in the game (the gaps
  list shows them).
- Sparse (virtual-pool) images and multisampled images are not saved in `images.bin`. They start from their storage, or cleared.
- Only the frame's GPU work is replayed. CPU-side game behaviour, input, audio and loading need the game.
