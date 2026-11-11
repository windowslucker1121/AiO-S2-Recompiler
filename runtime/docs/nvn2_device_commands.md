# NVN2 device commands

Goal 19 of the platform 2 plan. NVN2 has a device command buffer: memory the GPU fills with command words, which a command buffer then runs
(`nvnCommandBufferSubmitDeviceCommands`). This page is what the sdk code (title A, SDK_NVN-20_5_25, `work/ida/sdk`) says about it, how the
back end replays it, and what is still open. Addresses are sdk-module offsets (IDA image base 0x7500000000).

No game we have uses these functions: title A and title I only fetch them in `nvnLoadCProcs` (no call site), so nothing here has been compared with a
real stream. The facts marked *sdk* are read off the sdk's functions; *inferred* means a reading that fits the code but is not proven.

## Entry points

| Function | sdk | Signature (checked) | Back end |
|---|---|---|---|
| `nvnDeviceCommandBufferSetDefaults` | 0x5DC1A0 | `(dcb)` | clears +8..+0x30, keeps the device |
| `nvnDeviceCommandBufferSetDevice` | 0x5DC1C0 | `(dcb, device*)` | store at +0 |
| `nvnDeviceCommandBufferSetStorage` | 0x5DC1D0 | `(dcb, pool*, offset, size)` | +8, +0x10, +0x18 |
| `nvnDeviceCommandBufferSetCommandMemorySize` | 0x5DC1E0 | `(dcb, size)` | +0x20 |
| `nvnDeviceCommandBufferSetComputeMemorySize` | 0x5DC1F0 | `(dcb, size)` | +0x28 |
| `nvnDeviceCommandBufferGetHandle` | 0x5DC200 | `(dcb) -> u64` | pool GPU address + offset; 0 without a pool |
| `nvnCommandBufferBeginDeviceCommands` | 0x5DC220 | `(cmd, dcb)` | pairing check only |
| `nvnCommandBufferEndDeviceCommands` | 0x5DC320 | `(cmd, dcb)` | pairing check only |
| `nvnCommandBufferSubmitDeviceCommands` | 0x5DC680 | `(cmd, dcb)` | records `Op::SubmitDeviceCommands` |

`nvn2_procs.tsv` had End as `pl`, SetDevice as `pl` and SetStorage as `plll` (Hex-Rays guesses). The sdk bodies treat the second argument of
End and Submit as a pointer to the device command buffer (it reads +8/+0x10/+0x20/+0x28 from it), SetDevice stores a pointer and SetStorage's second
argument is the pool pointer (the sdk reads the pool's GPU address at pool+0x18). The TSV now says `pp`, `pp` and `ppll` with `sdk-verified` for these
and the other six device-command rows, and `nvn2_table.inc` and `nvn2_abi.md` follow. The device message buffer family (`nvnDeviceMessageBuffer*`, `Bind/ResetDeviceMessageBuffer`) is
GPU-to-CPU messaging, not commands, and is not part of this.

## The object

`NVNdeviceCommandBuffer`, 0x30 bytes, as the sdk keeps it (*sdk*):

| Offset | Field |
|---|---|
| 0x00 | device |
| 0x08 | memory pool |
| 0x10 | offset in the pool |
| 0x18 | storage size |
| 0x20 | command memory size (`SetCommandMemorySize`) |
| 0x28 | compute memory size (`SetComputeMemorySize`) |

The handle is the GPU address of byte 0 of the storage. The storage is laid out (*sdk*, from what Begin and Submit write):

```
storage + 0                         256-byte header (written by the console's GPU; the back end ignores it)
storage + 256                       command words, command-memory-size bytes
storage + ((cmd size + 511) & ~255) compute memory, compute-memory-size bytes (inline compute launch records)
```

## What the sdk writes for Begin, End and Submit (*sdk*)

All three put packets in the command buffer's own command memory, as every NVN2 command does.

* **Begin**: one packet `0xA0080E9C` (`0xA0082E9C` on a compute-mode buffer; the mode is the `SetRecordingMode` value 1 or 4), 8 words: storage
  address lo, hi, 256, `(cmd size + 511) & ~255`, `cmd size + 256`, end of the compute memory, number of segments `(cmd size + 0x7FFFFB) / 0x7FFFFC`,
  and 0x7FFFFC.
* **End**: a mode marker word, `0x80000002`, then `0xA0050EAA` with the storage address and `cmd size + 256`, and a control entry that calls the first
  8 bytes of the storage.
* **Submit**: a mode marker word, `0x80000002`, then control entries that call `[storage + 256, + cmd size)` in pieces of at most 0x7FFFFC bytes
  (the longest push-buffer segment: a 21-bit dword count), then a closing marker.

The segment limit in Begin's words is why the stream is a plain push-buffer stream: the writer has to know where segments end.

*Inferred*: Begin opens and End closes the GPU's work on the storage (it initialises and finalises the header: write cursors for the command and compute
areas), and the words between are produced by GPU work in between (a compute shader), not by the CPU. The sdk keeps no "inside device commands"
state on the command buffer (Begin writes nothing but its packet), so commands recorded between Begin and End are ordinary commands. Reading the
other way (the GPU captures the CPU-recorded commands between Begin and End) fits the sdk code too; the back end's decoder works for either as long as
the storage holds the words by the time Submit runs.

## The stream format

The command words are push-buffer packets, the same ones the sdk's command writers put in command memory (*sdk*, every writer decompiled):

```
header word   bits 31:29 sec_op   28:16 count (or the immediate for IMMD)   15:13 subchannel   11:0 method (in dwords)
sec_op 1 INC_METHOD      count words to method, method+1, ...
sec_op 3 NON_INC_METHOD  count words to the same method
sec_op 4 IMMD_DATA       one write, value = bits 28:16
sec_op 5 ONE_INC         first word to method, the other count-1 words to method+1 (macro call: the first word starts the macro, the rest are its arguments)
sec_op 7 END_PB_SEGMENT  ends the segment
```

Examples straight from the sdk: `0xA0030E4A` (DrawArrays), `0x81900E36` (IMMD 0xE36, value 0x190), `0x91100002` (a marker IMMD the sdk puts before indirect draws).

The methods 0xE00 and up are macro calls (each macro is a 2-method pair, call at the even method). Macros the decoder reads, with the argument order the
sdk writes (draw parameters are NVN enums stored as given):

| Method | NVN command | Arguments |
|---|---|---|
| 0xE4A | DrawArrays | mode, first, count |
| 0xE4E | DrawArraysInstanced (base instance 0) | mode, count, instance count, first, base instance |
| 0xE9E | DrawArraysInstanced (base instance != 0, after IMMD 0xE36 = 0x190) | same |
| 0xE04 | DrawElements | mode, index address hi, lo, index type, count |
| 0xE52 | DrawElementsInstanced (base vertex and base instance 0) | mode, count, instance count, first index (0), base vertex, base instance |
| 0xEA0 | DrawElementsInstanced (otherwise, after IMMD 0xE36) | same |
| 0xE4C | DrawArraysIndirect | mode, then NVNdrawArraysIndirectData: count, instance count, first, base instance |
| 0xE50 | DrawElementsIndirect | mode, then NVNdrawElementsIndirectData: count, instance count, first index, base vertex, base instance |

Registers the elements macros read: index type is the IMMD 0x5F6 (value = NVNindexType), index address the two-word INC at 0x5F2 (hi, lo; used by 0xE52,
0xEA0, 0xE50). DrawElementsBaseVertex writes the base vertex to registers 0x446 and 0x50D (INC), runs 0xE04, then writes 0 to both (IMMD); 0xE04 uses the
last value written to 0x50D.

For the indirect draws the console's control stream splices the indirect buffer's words into the packet (the sdk's writer puts a control entry that calls
the 16 or 20 bytes of the indirect-data struct right after the macro word, and the header's count already includes them). In a device stream those words
are inline, which is what the decoder reads; the back end turns them into a direct draw.

Bind and state packets the decoder reads as well (the replay has an equivalent op for each; all verified against the sdk writers):

| Packet | NVN command | Decoded as |
|---|---|---|
| IMMD 0x4E5 / 0x3D5 (value in the immediate) | SetStencilRef, front / back | `SetStencil` what 0 |
| IMMD 0x4E6 / 0x3D7 | SetStencilValueMask, front / back | what 1 |
| IMMD 0x4E7 / 0x3D6 | SetStencilMask, front / back | what 2 |
| IMMD 0x591 (0 / 1), then INC 0x592 with the index when enabling | SetPrimitiveRestart | `SetPrimitiveRestart` (one op at the IMMD, one at the index) |
| INC, 2 words at 0x381 + 4 * index: x min \| x max << 16, y min \| y max << 16 | SetScissor(s) | `SetScissors` (width = max - min; clamped to 0..65535; empty = 0) |
| INC, 2 words at 0x701 + 4 * i (address hi, lo), then INC 2 words at 0x180 + 2 * i (size hi, lo) | BindVertexBuffer | `BindBuffers` vertex |
| 0xE98: (count - 1) << 16 \| first, then count * {address lo, hi, size lo, hi} | BindVertexBuffers | `BindBuffers` vertex |
| 0xE02: stage value, index, address lo, (size / 256 rounded up) << 20 \| address bits 32..39 | BindUniformBuffer, graphics stages | `BindBuffers` uniform (the size is a multiple of 256) |
| 0xE00: stage value, index | BindUniformBuffer with size 0 | `BindBuffers` uniform, range 0 |

The stage values are the sdk's table at 0xBD9B4C: vertex 8, tess control 4, tess eval 3, geometry 1, fragment 10, task 9, mesh 11 (compute and
unused slots -1).

Not mapped (the sdk writes them, the decoder counts them as unhandled and skips them):

* **DispatchCompute, DispatchComputeIndirect**: the sdk does not put them in the command words. It writes a 16-byte record (byte 15, then x, y, z;
  indirect has its own) into control memory and inline-calls it between two command segments, so the order against the draws lives in the control
  stream. A device buffer has only the "compute memory" for these and nothing known says how its records are ordered against the command words.
* **BindProgram** (IMMD 0xE3E and a shader handle): the replay needs a host `Program` object, and a stream carries only the GPU-side handle.
* **BindStorageBuffer(s), BindUniformBuffer on the compute stage** (0xEA6, subchannel 1, shared by both): the leading words of the two are not told
  apart by anything in the packet we have read.
* **SetViewports, SetDepthRanges, SetDepthBounds**: the sdk writes the hardware's scale / translate values, which depend on the command buffer's
  origin and depth-mode state at record time; the stream does not carry that state, so the NVN arguments cannot be recovered with certainty.
* BindTextures / BindImages / BindSeparate*, BindVertexAttribState / StreamState, the blend / depth / polygon state objects, SetRenderTargets, clears,
  copies, barriers, fences, counters, the *Deferred and push-constant forms, mesh and ray tracing commands: not read from the sdk for streams yet.

Compute launches are not in the command words either way (see the dispatch note above); `P_SubmitDeviceCommands.compute_*` carries the region.

## Replay (nvn_replay.cpp `do_submit_device_commands`)

1. `cpu_view(stream_address, stream_size)` reads the words. This works when the CPU wrote them, or a submission that has completed did.
2. If a submission that is still being recorded writes the range (the usual case for words a compute shader makes in the same frame), `split_for_read`
   splits the submission there: it ends the render pass, the occlusion / counter queries and the debug labels (none can outlive a command buffer),
   `frame_split` (nvn_gpu.cpp) ends the command buffer, submits what is recorded so far without signalling the serial, waits for that part and every
   earlier submission, and goes on in a second command buffer of the same slot (same serial). Everything the replay had bound is bound again afterwards
   (the dirty flags, pipeline and set caches are reset as at the start of a submission) and the GPU-write records of that serial are settled, so the
   words are readable. It costs a GPU wait on the render thread, once per Submit whose words the same submission wrote. The occlusion count of a
   counter open across the split is the count up to the split; labels are closed at the split. If the range is in no pool, or the split fails, the
   submit is skipped and counted as unsupported (`device commands: the command words are not readable by the CPU`).
3. `devcmd::decode` (nvn_devcmd.cpp) walks the packets: commands become `devcmd::Cmd`, register writes update the state the macros read, everything else
   is counted in `Stats::unhandled` and skipped. It stops at the end of the data, an END_PB_SEGMENT, a zero word (memory nobody wrote) or a packet with
   unknown framing (sec_op 0, 2, 6).
4. The decoded commands are turned into the records the API would have recorded, in a temporary `CmdList`, which `run_list` runs one level down. So the
   draws, binds and state go through the same ops, dirty flags and compute-result handling as a recorded list. An indirect draw whose count or
   instance count is 0 draws nothing; `first index` is added to the index address.

The first submit logs a line with the packet and command counts and the first unhandled headers (`replay: device commands (first)`), and the first
split logs `replay: submission split to read device command words`.

## What VK_EXT_device_generated_commands would change

The read-back needs the words in CPU-visible memory, so a stream the same submission writes costs a GPU wait (the split). With VK_EXT_device_generated_commands:

* a small compute pass translates the device stream into a Vulkan sequence buffer on the GPU (one DGC sequence per draw packet: draw or draw-indexed
  token, vertex / index buffer tokens, push-constant tokens for the uniform binds; programs would need an execution set), so nothing is read back and no split is needed;
* `vkCmdPreprocessGeneratedCommandsEXT` + `vkCmdExecuteGeneratedCommandsEXT` replace `do_draw_*` for the whole submit, with the indirect count taken from
  the stream length the translation pass writes.

That also needs the translation pass's input format to be fully known, including the compute memory order, which is why the CPU decoder came first.

## Open

* The 256-byte header's fields are only known through what Begin passes to the GPU; nothing reads them back here.
* The packets listed as not mapped above (dispatch, programs, storage and compute-stage uniform binds, viewports and depth ranges, and the rest).
* Who writes the words (the GPU between Begin and End, or the GPU capturing CPU-recorded commands) is inferred, see above.
* The split stalls the render thread on the GPU; the DGC route above removes it.

## Tests

`tests/gfx/test_nvn_device_cmds.cpp` (group `gfx_nvn_device_cmds`): the framing against sdk header words, streams built in memory with
`devcmd::StreamBuilder` (the words the sdk's writers produce) and the decoded commands compared field by field, stop conditions, the entry points
through the proc table (object layout, handle, one `SubmitDeviceCommands` record with the storage addresses), and a comparison with the records the direct
draw calls produce for the same arguments, and the bind / state packets. The split itself needs a GPU and is not covered by a unit test. Build and run without CMake: `scripts/nvn_unit.ps1` (see its header; it needs `build/release/nxrt.lib`), with `-Src` listing the api files the test needs.
