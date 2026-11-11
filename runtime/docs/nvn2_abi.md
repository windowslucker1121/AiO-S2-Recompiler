# NVN2 ABI as used by title A

Source: the game's own `sdk` NSO (tag `SDK_NVN-20_5_25-Release`, build id `cc7da00d…`) and `main` (build id `76a489f3…`), converted with nx2elf and read with IDA 9.4 (Hex-Rays) plus capstone scripts. Release SDK: no debug layer, no enum validation, so evidence comes from what the code reads and writes. Analysis scratch lives in `work/nvn2/` (not in git).

Generated files: `src/gfx/nvn/nvn2_procs.tsv` (one row per proc, the source of truth), `src/gfx/nvn/nvn2_table.inc` and `src/gfx/nvn/nvn2_sizes.inc` (written by `scripts/gen_nvn2_table.py`, which also checks the TSV against the NVN1 table and, with `--sdk`, against the sdk's own tables).

Marks used below: **checked** = read directly in code; *inferred* = a reasoned guess.

## 1. API version

`nvnDeviceGetInteger` (sdk 0x5D6840 → 0x5DAF60) is a switch over 170 `NVNdeviceInfo` values (0x00–0xA9; NVN1 had 98, 0x00–0x61):

| query | NVN2 | NVN1 (runtime's `nvn_device_info.inc`) |
|---|---|---|
| API_MAJOR_VERSION (0) | **231 (0xE7)** | 55 |
| API_MINOR_VERSION (1) | **30 (0x1E)** | 15 |

**Checked.** main calls a version-check stub (0x114A628, empty in release) with exactly `(0xE7, 0x1E)` right after `nvnLoadCProcs` (0x114C718), so the game was built against 231.30.

Other DeviceInfo values that differ from the NVN1 values the runtime reports (constants unless "device field", which the sdk reads from the device at runtime):

| value | name (NVN1) | NVN2 | NVN1 |
|---|---|---|---|
| 0x06 | VERTEX_BUFFER_BINDINGS | 32 | 16 |
| 0x0E | VERTEX_ATTRIBUTES | 32 | 16 |
| 0x1A | SHADER_SCRATCH_MEMORY_GRANULARITY | 0x4000 | 0x20000 |
| 0x1C / 0x1F | MAX_COMPUTE_WORK_GROUP_SIZE_X / _THREADS | 1024 | 1536 |
| 0x20 | MAX_COMPUTE_DISPATCH_WORK_GROUPS_X | 0x7FFFFFFF | 65535 |
| 0x2C | MAX_TEXTURE_LEVELS | 16 | 15 |
| 0x2E / 0x2F | GLSLC_MAX / MIN_SUPPORTED_GPU_CODE_MAJOR_VERSION | **201** | 1 |
| 0x30 / 0x31 | GLSLC_MAX / MIN_SUPPORTED_GPU_CODE_MINOR_VERSION | **17 / 0** | 15 / 5 |
| 0x4E | SHADER_CODE_MEMORY_POOL_PADDING_SIZE | 2048 | 1024 |
| 0x5A | SUPPORTS_DEBUG_LAYER | 0 | 1 |
| 0x17, 0x18, 0x2B, 0x2D, 0x37, 0x38, 0x3A, 0x3D–0x41 | scratch factors, L2 size, max layers, linear stride align, pool page size, max texture sizes | device field | constants |

The game queries 0x10, 0x11/0x12, 0x24/0x25, 0x2A, 0x5A; their numbering matches the NVN1 names (descriptor pool code asks 0x11/0x12 and 0x24/0x25 for texture versus sampler pools). **Checked.**

## 2. Procs

`nvnDeviceGetProcAddress` (sdk 0x5DBD00) binary-searches 726 names (stored without the `nvn` prefix, offsets at 0xBA1D88 from 0xB9CA26) and returns `table[i]` from 0xE056E0 (a second table at 0xDD1650 holds interception stubs). Unknown names fall back to 0x5DB2E0 (the `*NVX`/interop names). main's 729 names = these 726 + `nvnBootstrapLoader`, `nvnInstallGlobalDebugCallback`, `nvnLoadCProcs`. main fetches all 726 in `nvnLoadCProcs` (0x11456A0–0x114A628), each into a global reached through a GOT slot.

**Used / unused** = a code reference to that proc's global (through its GOT slot or directly) anywhere in main's `.text` outside `nvnLoadCProcs`, found by register dataflow from every ADRP (callee-saved registers survive calls). 215 used, 511 unused. A name only present in the loader counts as unused.

| class | count | used by main |
|---|---|---|
| same as the NX NVN1 table (signature kept) | 468 | 209 |
| changed signature | 2 | 0 |
| not in the NX NVN1 table | 256 | 6 |

Of the 256, 222 are NVN2 feature families by name (ray tracing, mesh tasks, DLSS, device command/message buffers, HDR/colour space, push constants, ProgramBuilder, dirty tile maps, shading rate, checkpoints, debug groups per engine…); 34 look like NVN1-era API the NX table never had (`*Deferred` binds, `*Builder*GetDevice`, `QueueBuilderGet*`, `EventGet*`…) — *inferred* from names. NVN1 table names missing from NVN2: `CommandBufferInsertDebugMarkerStatic/Dynamic`, `PushDebugGroupStatic/Dynamic`, `PopDebugGroupId`, `QueueAcquireTexture`, `DeviceApplyDeferredFinalizes` (plus the two loader names).

**Changed signatures (checked in assembly):**
- `nvnCommandBufferPopDebugGroup(cmdbuf, uint32 domainId)` — NVN1 took only `cmdbuf` (NVN1's `PopDebugGroupId` folded in).
- `nvnCommandBufferInsertDebugMarker(cmdbuf, uint32 domainId, const char*)` — NVN1 `(cmdbuf, const char*)`; x2 goes to `strlen`.

**Not-in-NX-table procs the game calls** (signatures checked in assembly): `CommandBufferPushDebugGroupGraphics/Compute(cmdbuf, uint32, const char*)`, `CommandBufferPopDebugGroupGraphics/Compute(cmdbuf, uint32)`, `CommandBufferSetRecordingMode(cmdbuf, int)` (stores to cmdbuf+0x18; values 0–4 are used by the sdk, the game passes 0/1/2), `QueueBuilderSetPrimaryQueue(builder, NVNqueue*)` (stores to +0x18).

How signatures were settled: for the 468 'same' rows the NVN1 signature stands, because the number of argument registers the sdk function reads (Hex-Rays) is never more than NVN1's, except 12 rows where Hex-Rays over-counted; all 12 were read by hand and only the two above changed. Argument types (int versus pointer) of 'same' rows are not proven, only their count and float/int placement. For the other 'new' rows the signature is Hex-Rays' guess (`sig_source=hexrays`); 12 are empty no-ops in the release sdk (`noop-stub`, signature unknown) and 9 have implausible Hex-Rays argument lists (`hexrays-unreliable`: HDR metadata get/set, `DeviceMessageBufferGetNextMessage`, `DispatchRays`, `ComputeDirtyTileMap`, `DlssNetworkBuilderGetOptimalSettings`, `MultisampleStateGetSampleLocations`). Return classes of 'new' rows are a name heuristic.

The full per-proc table is the appendix; machine-readable: `src/gfx/nvn/nvn2_procs.tsv`.

## 3. Struct sizes

min = bytes the sdk itself touches or clears (safe for the back end to use); max = room the game leaves (its next field, stack frame or array stride). The game's own NVN wrapper (main 0x114A640–0x114C800) lays objects out as `{u8 flags, …, T* self @+8, T @+0x10, …}`, which gives exact bounds.

| struct | NVN1 | NVN2 min | NVN2 max | evidence |
|---|---|---|---|---|
| NVNdevice | 0x3000 | 0x22D8 | **0x3000** | wrapper: device at +8, own field at +0x3008, object memset 0x3018 (0x114C694, 0x114C6A4) |
| NVNdeviceBuilder | 0x40 | 0x18 | **0x40** | 0x40-byte stack slot value-initialised, frame record right after (0x114C728) |
| NVNqueue | 0x2000 | 0x2CC8 | **0x3000** (grew) | queue at wrapper+0x20, flag at +0x3020 (0x17AAAA0); second wrapper memset 0x3028 with flag at +0x3010 |
| NVNqueueBuilder | 0x40 | 0x48 | 0x50 (grew) | SetDefaults writes to +0x48; stack slot sp..sp+0x50 (0x17AA9B4) |
| NVNwindow | 0x180 | 0x218 | 0x280 (grew) | WindowInitialize memsets 0x218; window at wrapper+0x10, wrapper field at +0x290 |
| NVNwindowBuilder | 0x40 | 0x60 | **0x80** (grew) | 0x80 bytes value-initialised between two other stack variables (0x114B298) |
| NVNmemoryPool | 0x100 | 0x100 | **0x100** | Initialize clears 0x100; pool at wrapper+0x10, flag at +0x110 |
| NVNmemoryPoolBuilder | 0x40 | 0x38 | **0x40** | 0x40 zeroed stack slot, frame record after (0x114AFB4) |
| NVNbuffer | 0x30 | 0x30 | 0x38 | Initialize writes to +0x30; wrapper ctor clears 0x48 with buffer at +0x10 |
| NVNbufferBuilder | 0x40 | 0x28 | **0x40** | zeroed 0x40 stack slot (0x114BF3C) |
| NVNtexture | 0x80 (game) | 0x80 | **0xC0** (grew) | wrapper `{flags, self@8, texture@0x10, field@0xD0}` is 0xD8 (swapchain array stride, ctor 0x114B99C clears 0xD8); outer class 0x17A9080 has texture at +0x20 and writes the wrapper field at +0xE0 |
| NVNtextureBuilder | 0x80 | 0x68 | **0x80** | 0x80 zeroed stack slot, frame record after (0x114B8B0) |
| NVNtextureView | 0x10 (game) | 0x10 | **0x10** | adjacent stack views 0x10 apart (0x1144E34/0x1144E6C); RegisterTexture reads 0x10 |
| NVNsampler | 0x40 | 0x40 | 0x68 | Initialize writes to +0x40; wrapper ctor clears 0x78 with sampler at +0x10 |
| NVNsamplerBuilder | 0x40 | 0x30 | **0x40** | zeroed 0x40 stack slot (0x114C170) |
| NVNtexturePool / NVNsamplerPool | 0x20 | 0x20 | 0x28 | pool at +0x18 of a 0x40 wrapper (ctor 0x114C37C) |
| NVNprogram | 0x80 (NX game) | 0xA0 | 0xC0 (grew) | InitializeFromBuilder writes +0x98; program at +0x48, next field at +0x108 (0x17A9768) |
| NVNsync | 0x40 | 0x20 | **0x40** | wrapper: sync at +0x10, flag at +0x50; arrays stride 0x40 (0x17B1F9C) |
| NVNcommandBuffer | 0xA0 | 0xB0 | 0xC0 (grew) | Initialize clears exactly 0xB0; wrapper stores its pointer at +0xD0 with the command buffer at +0x10 (0x11448B0) |
| NVNevent / NVNeventBuilder | 0x40 / 0x20 | 0x28 / 0x10 | – | sdk only (game does not use events) |
| state objects (blend 8, channel mask 4, colour 4, depth-stencil 8, multisample 0x18, polygon 4, vertex stream 8) | same | same | – | sdk getters/setters; vertex attrib state is **4** (game arrays stride 4) |
| NVNcopyRegion | 0x18 | 0x18 | – | copy procs read six u32 |
| NVNshaderData | 0x10 | **0x10** | | `{u64 code GPU address, const void* control}` |
| NVNshaderDataExt (new) | – | **0x18** | | `{code, control, u64 extra}` (SetShadersExt) |
| NVNprogramBuilder (new) | – | 0x64 | – | SetDefaults writes to +0x64: shaders count/ptr +0x28/+0x30, RT modules +0x38/+0x40, subroutine linkage +0x48/+0x50 |
| NVNdeviceCommandBuffer (new) | – | 0x30 | – | SetDefaults clears +8..+0x30 |
| NVNdeviceMessageBuffer (new) | – | 0x28 | – | SetDefaults clears +8..+0x28 |
| NVNdirtyTileMapParameters (new) | – | 0x74 | – | getters to +0x74 |
| NVNdlssNetwork (new) | – | **0x220** | – | Initialize memsets exactly 0x220 (*inferred* = sizeof) |
| NVNdlssNetworkBuilder (new) | – | 0xB8 | – | texture IDs at +0x38, 128 bytes |
| NVNdlssParameters (new) | – | 0xC0 | – | SetDefaults writes to +0xC0 |

The three DLSS objects are fully mapped since goal 18 (field offsets in `src/gfx/nvn/api_dlss.cpp`, from the setters/getters and the packet of `nvnCommandBufferEvaluateDlss` 0x5DEBC0; the network is `memset` 0x220 by Initialize and carries an embedded memory pool at +200). Signature corrections to the Hex-Rays rows: `nvnDlssNetworkBuilderGetStorageSize(builder)` has one argument (the second is a scratch vector register), `nvnDlssNetworkResize(network, int w, int h)` takes the new output size, `nvnDlssNetworkBuilderGetOptimalSettings(builder, 6 x int*)` forwards six pointers to the driver, the texture arguments of the six `SetInputColor`-style calls are `(const NVNtexture*, const NVNtextureView* or null)`.

Acceleration structures are not objects: they are memory the game sizes with `DeviceCalculateAccelerationStructureMemoryRequirements` and passes as addresses.

Note for the back end: texture (0x80→0xC0), queue (0x2000→0x3000), command buffer, window, program and the queue/window builders grew. min is a lower bound from sdk code only; where max is bold the game's layout settles the size.

## 4. Enums

- **Formats: not renumbered** for the 98 the game uses. main's nn::gfx table (0x28499C4, 0x62 pairs `ImageFormat → NVNformat`) maps every gfx channel/type to the NVN1 value of the same name (R8=1 … RGBA8=0x25, RGBA8_SRGB=0x38, BGRA8=0x9A, DXT1=0x42, BPTC, all ASTC 0x79–0x94, depth formats 0x32–0x36, A1BGR5=0x98, BGR5A1=0x97). **Checked.**
- **Texture targets: same as NVN1** (game table 0x2849CD4 maps nn::gfx dimensions 1D/2D/3D/Cube/1DArray/2DArray/MS/MSArray/CubeArray to 0/1/2/8/3/4/5/6/9). **Checked.**
- **Memory pool flags: same bits as NVN1** (`MemoryPoolInitialize`: CPU_NO_ACCESS 1, CPU_UNCACHED 2, CPU_CACHED 4, GPU_NO_ACCESS 8, GPU_UNCACHED 0x10, GPU_CACHED 0x20, bits ≥0x40 passed through, PHYSICAL 0x100, VIRTUAL 0x200). Builder default flags 0x21. The game tests flag 0x4 (CPU_CACHED) to decide on flushes. **Checked.**
- **Device info: same numbering** for 0x00–0x61 as far as the game uses it; extended to 0xA9.
- **Command buffer recording mode**: new; a 32-bit mode at cmdbuf+0x18 read by every command emitter (0, 1, 2, 3, 4 seen; 1 and 4 select the same path in several emitters). Meanings not settled.
- Sync wait results: game maps 0..2 like NVN1 (ALREADY_SIGNALED, CONDITION_SATISFIED, TIMEOUT_EXPIRED; *inferred* from the table at 0x2849D70).

## 5. Shaders

- `nvnProgramSetShaders(program, int count, const NVNshaderData*)` → core 0x5E4EA0(program, count, data, NULL). `NVNshaderData` = `{u64 code GPU address; const void* control}` (16 bytes). **Checked.**
- `nvnProgramSetShadersExt(program, int count, const NVNshaderDataExt*)`: 24-byte entries `{code, control, u64 extra}`, split into NVNshaderData + a parallel `extra[]` passed as the core's 4th argument (stored per stage at +0x6A8 of the core's stage record). Local buffers hold up to 9 entries.
- `nvnProgramBuilderSetShaders(builder, int count, const NVNshaderData*)` just stores count/pointer (+0x28/+0x30); `nvnProgramInitializeFromBuilder` consumes them. Also `SetRayTracingModules(count, ptr)` and `SetSubroutineLinkage(count, ptr)`.
- Core checks per entry: control non-null and 8-byte aligned, code address non-zero, stage unique. 9 stage slots (mask 0x1FF); stage 8 = invalid. *Inferred:* 0–5 keep NVN1 order (stage 5 has compute-only checks), the extra slots are task/mesh-class stages.
- **Control section header** (accessor getters, offsets from the control pointer): `+0 u32` (first field copied out; magic or kind, unconfirmed), `+4 u32 major = 201` (required), `+8 u32 minor` (≤ 17), `+0xC u16`, `+0xE u16`, stage `u8 @+0x56`, flag bytes `+0x70–0x76`, `+0x92–0x94`, `+0xA7`, embedded blob `offset u32 @+0xC8, size u32 @+0xCC` (copied verbatim into program memory), and for newer minors `+0xD1/+0xD2 u8`, `+0xD8/+0xDC u32`. The minor picks one of 7 accessor classes (minors 0–1, 2, 3–12, 13–14, 15, 16, 17); newer ones add fields, older ones return 0. Per-stage hardware state is 1564 bytes copied out of the parsed control.
- **The sdk patches the shader code in place**: it maps the code GPU address back to CPU memory through the device's memory-pool list, then walks 16-byte instructions backwards from the end of the code looking at the low 9 opcode bits: 0x118 (NOP) is skipped, and a 0x147 (BRA) preceded by at least 8 NOPs gets a block of MOV-immediate instructions (`…0xFF7802`, scheduling words `0x000FC00000000F00`) written with the code size, a control value and the stage number. This is Volta-and-later 128-bit SASS. **Checked**; what the patched values mean is *inferred*.
- **GPU class**: the device stores a compute class at device+0x14; QMD/compute code picks layouts for **0xC5C0 (Turing), 0xC7C0 (Ampere B) and 0xC9C0 (Ada)**, and several paths test `== 0xC9C0`. No 3D class constant (xx97) appears as an immediate. Which class the console reports is a runtime value, not settled here.

## How the tables were made

All in `work/nvn2/py/` (not committed): `mainload.py` (loader → slot per name), `mainxref2.py` (code references), `footprint.py` (offsets each proc touches through its arguments, following calls, memset/memcpy sizes), `build_tsv.py` (TSV). Hex-Rays decompilation of all 726 procs: `ida_decomp.py`. Linear-sweep footprints can over-report (one false 0xD0 access in `CommandBufferBarrier` was found and rejected); every min above that matters was read in the decompiled code.

## Open questions

1. Exact sizes where min ≠ max (queue builder, window, buffer, sampler, pools, program, command buffer) and for the new types the game does not use.
2. Which control-section minor title A's shaders use (read the romfs shader archives) and what `+0` and the 1564-byte per-stage state hold.
3. Meaning of the recording modes 0–4 and the meaning of the patched shader-tail constants.
4. The GPU class the console reports (0xC7C0 versus 0xC9C0), and the 3D class.
5. Signatures of the 9 `hexrays-unreliable` and 12 `noop-stub` procs (none is used by the game).

## Appendix: all procs

| # | proc | sdk addr | sig (ret: args) | NVN1 sig | class | note | main refs |
|---|---|---|---|---|---|---|---|
| 0 | nvnBlendStateGetAdvancedMode | 0x5EC370 | i: p | i: p | same |  | unused |
| 1 | nvnBlendStateGetAdvancedNormalizedDst | 0x5EC3A0 | i: p | i: p | same |  | unused |
| 2 | nvnBlendStateGetAdvancedOverlap | 0x5EC380 | i: p | i: p | same |  | unused |
| 3 | nvnBlendStateGetAdvancedPremultipliedSrc | 0x5EC390 | i: p | i: p | same |  | unused |
| 4 | nvnBlendStateGetBlendEquation | 0x5EC350 | v: ppp | v: ppp | same |  | unused |
| 5 | nvnBlendStateGetBlendFunc | 0x5EC310 | v: ppppp | v: ppppp | same |  | unused |
| 6 | nvnBlendStateGetBlendTarget | 0x5EC300 | i: p | i: p | same |  | unused |
| 7 | nvnBlendStateSetAdvancedMode | 0x5EC280 | v: pi | v: pi | same |  | unused |
| 8 | nvnBlendStateSetAdvancedNormalizedDst | 0x5EC2E0 | v: pi | v: pi | same |  | unused |
| 9 | nvnBlendStateSetAdvancedOverlap | 0x5EC2A0 | v: pi | v: pi | same |  | unused |
| 10 | nvnBlendStateSetAdvancedPremultipliedSrc | 0x5EC2C0 | v: pi | v: pi | same |  | unused |
| 11 | nvnBlendStateSetBlendEquation | 0x5EC250 | v: pii | v: pii | same |  | 1 |
| 12 | nvnBlendStateSetBlendFunc | 0x5EC210 | v: piiii | v: piiii | same |  | 1 |
| 13 | nvnBlendStateSetBlendTarget | 0x5EC1F0 | v: pi | v: pi | same |  | 1 |
| 14 | nvnBlendStateSetDefaults | 0x5EC1D0 | v: p | v: p | same |  | 1 |
| 15 | nvnBufferBuilderGetDevice | 0x5C3500 | l: p (hexrays) |  | new | not-in-nx-table | unused |
| 16 | nvnBufferBuilderGetMemoryOffset | 0x5C3520 | l: p | l: p | same |  | unused |
| 17 | nvnBufferBuilderGetMemoryPool | 0x5C3510 | S: p | S: p | same |  | unused |
| 18 | nvnBufferBuilderGetSize | 0x5C3530 | l: p | l: p | same |  | unused |
| 19 | nvnBufferBuilderSetDefaults | 0x5C34E0 | v: p | v: p | same |  | 4 |
| 20 | nvnBufferBuilderSetDevice | 0x5C34D0 | v: pp | v: pp | same |  | 4 |
| 21 | nvnBufferBuilderSetStorage | 0x5C34F0 | v: ppll | v: ppll | same |  | 4 |
| 22 | nvnBufferFinalize | 0x5C3590 | v: p | v: p | same |  | 6 |
| 23 | nvnBufferFlushMappedRange | 0x5C3610 | v: pll | v: pll | same |  | 1 |
| 24 | nvnBufferGetAddress | 0x5C3600 | l: p | l: p | same |  | 6 |
| 25 | nvnBufferGetDebugID | 0x5C3660 | l: p | l: p | same |  | unused |
| 26 | nvnBufferGetMemoryOffset | 0x5C3640 | l: p | l: p | same |  | unused |
| 27 | nvnBufferGetMemoryPool | 0x5C3630 | p: p | p: p | same |  | 1 |
| 28 | nvnBufferGetSize | 0x5C3650 | l: p | l: p | same |  | 4 |
| 29 | nvnBufferInitialize | 0x5C3540 | i: pp | i: pp | same |  | 4 |
| 30 | nvnBufferInvalidateMappedRange | 0x5C3620 | v: pll | v: pll | same |  | 1 |
| 31 | nvnBufferMap | 0x5C35D0 | p: p | p: p | same |  | 2 |
| 32 | nvnBufferSetDebugLabel | 0x5D63B0 | v: pp | v: pp | same |  | unused |
| 33 | nvnChannelMaskStateGetChannelMask | 0x5EC410 | v: pipppp | v: pipppp | same |  | unused |
| 34 | nvnChannelMaskStateSetChannelMask | 0x5EC3C0 | v: piiiii | v: piiiii | same |  | 1 |
| 35 | nvnChannelMaskStateSetDefaults | 0x5EC3B0 | v: p | v: p | same |  | 1 |
| 36 | nvnColorStateGetAlphaTest | 0x5EC4F0 | i: p | i: p | same |  | unused |
| 37 | nvnColorStateGetBlendEnable | 0x5EC4D0 | i: pi | i: pi | same |  | unused |
| 38 | nvnColorStateGetLogicOp | 0x5EC4E0 | i: p | i: p | same |  | unused |
| 39 | nvnColorStateSetAlphaTest | 0x5EC4B0 | v: pi | v: pi | same |  | unused |
| 40 | nvnColorStateSetBlendEnable | 0x5EC450 | v: pii | v: pii | same |  | 1 |
| 41 | nvnColorStateSetDefaults | 0x5EC440 | v: p | v: p | same |  | 1 |
| 42 | nvnColorStateSetLogicOp | 0x5EC490 | v: pi | v: pi | same |  | unused |
| 43 | nvnCommandBufferAddCommandMemory | 0x5C49C0 | v: ppll | v: ppll | same |  | 1 |
| 44 | nvnCommandBufferAddControlMemory | 0x5C4BB0 | v: ppl | v: ppl | same |  | 1 |
| 45 | nvnCommandBufferBarrier | 0x61CB70 | v: pi | v: pi | same |  | 13 |
| 46 | nvnCommandBufferBeginDeviceCommands | 0x5DC220 | v: pp (sdk-verified) |  | new | nvn2 | unused |
| 47 | nvnCommandBufferBeginRecording | 0x5C4D20 | v: p | v: p | same |  | 1 |
| 48 | nvnCommandBufferBeginTransformFeedback | 0x608E40 | v: pl | v: pl | same |  | 1 |
| 49 | nvnCommandBufferBindAccelerationStructure | 0x600CE0 | v: piil (hexrays) |  | new | nvn2 | unused |
| 50 | nvnCommandBufferBindAccelerationStructures | 0x601120 | v: piiip (hexrays) |  | new | nvn2 | unused |
| 51 | nvnCommandBufferBindAccelerationStructuresDeferred | 0x6011A0 | v: piiil (hexrays) |  | new | nvn2 | unused |
| 52 | nvnCommandBufferBindBlendState | 0x61F420 | v: pp | v: pp | same |  | 1 |
| 53 | nvnCommandBufferBindChannelMaskState | 0x5FF1C0 | v: pp | v: pp | same |  | 1 |
| 54 | nvnCommandBufferBindColorState | 0x5FF150 | v: pp | v: pp | same |  | 1 |
| 55 | nvnCommandBufferBindCoverageModulationTable | 0x60A930 | v: pp | v: pp | same |  | unused |
| 56 | nvnCommandBufferBindDepthStencilState | 0x5FF230 | v: pp | v: pp | same |  | 1 |
| 57 | nvnCommandBufferBindDeviceMessageBuffer | 0x5DD2D0 | v: piil (hexrays) |  | new | nvn2 | unused |
| 58 | nvnCommandBufferBindDeviceMessageBufferDeferred | 0x5DD610 | v: piil (hexrays) |  | new | nvn2 | unused |
| 59 | nvnCommandBufferBindImage | 0x604C20 | v: piil | v: piil | same |  | 2 |
| 60 | nvnCommandBufferBindImages | 0x605060 | v: piiip | v: piiip | same |  | unused |
| 61 | nvnCommandBufferBindImagesDeferred | 0x605450 | v: piiil (hexrays) |  | new | not-in-nx-table | unused |
| 62 | nvnCommandBufferBindMultisampleState | 0x5FF310 | v: pp | v: pp | same |  | 1 |
| 63 | nvnCommandBufferBindPolygonState | 0x5FF2A0 | v: pp | v: pp | same |  | 1 |
| 64 | nvnCommandBufferBindProgram | 0x5FF6E0 | v: ppi | v: ppi | same |  | 2 |
| 65 | nvnCommandBufferBindSeparateSampler | 0x6034C0 | v: piiS | v: piiS | same |  | unused |
| 66 | nvnCommandBufferBindSeparateSamplers | 0x6038E0 | v: piiip | v: piiip | same |  | unused |
| 67 | nvnCommandBufferBindSeparateSamplersDeferred | 0x603CD0 | v: piiil (hexrays) |  | new | not-in-nx-table | unused |
| 68 | nvnCommandBufferBindSeparateTexture | 0x602700 | v: piiS | v: piiS | same |  | unused |
| 69 | nvnCommandBufferBindSeparateTextures | 0x602B20 | v: piiip | v: piiip | same |  | unused |
| 70 | nvnCommandBufferBindSeparateTexturesDeferred | 0x602F10 | v: piiil (hexrays) |  | new | not-in-nx-table | unused |
| 71 | nvnCommandBufferBindShadingRateImage | 0x621200 | v: pll (hexrays) |  | new | nvn2 | unused |
| 72 | nvnCommandBufferBindStorageBuffer | 0x6015D0 | v: piill | v: piill | same |  | 1 |
| 73 | nvnCommandBufferBindStorageBuffers | 0x601A00 | v: piiip | v: piiip | same |  | unused |
| 74 | nvnCommandBufferBindStorageBuffersDeferred | 0x601DA0 | v: piiil (hexrays) |  | new | not-in-nx-table | unused |
| 75 | nvnCommandBufferBindTexture | 0x6022E0 | v: piil | v: piil | same |  | 1 |
| 76 | nvnCommandBufferBindTextures | 0x604280 | v: piiip | v: piiip | same |  | unused |
| 77 | nvnCommandBufferBindTexturesDeferred | 0x604670 | v: piiil (hexrays) |  | new | not-in-nx-table | unused |
| 78 | nvnCommandBufferBindTransformFeedbackBuffer | 0x608C90 | v: pill | v: pill | same |  | 2 |
| 79 | nvnCommandBufferBindTransformFeedbackBuffers | 0x608D60 | v: piip | v: piip | same |  | unused |
| 80 | nvnCommandBufferBindUniformBuffer | 0x5FFF60 | v: piill | v: piill | same |  | 1 |
| 81 | nvnCommandBufferBindUniformBuffers | 0x6003A0 | v: piiip | v: piiip | same |  | unused |
| 82 | nvnCommandBufferBindUniformBuffersDeferred | 0x600760 | v: piiil (hexrays) |  | new | not-in-nx-table | unused |
| 83 | nvnCommandBufferBindVertexAttribState | 0x61B260 | v: pip | v: pip | same |  | 1 |
| 84 | nvnCommandBufferBindVertexBuffer | 0x5FFD60 | v: pill | v: pill | same |  | 1 |
| 85 | nvnCommandBufferBindVertexBuffers | 0x5FFE00 | v: piip | v: piip | same |  | unused |
| 86 | nvnCommandBufferBindVertexStreamState | 0x61B380 | v: pip | v: pip | same |  | 1 |
| 87 | nvnCommandBufferBuildAccelerationStructures | 0x5F5370 | v: pllll (hexrays) |  | new | nvn2 | unused |
| 88 | nvnCommandBufferCallCommands | 0x5C62A0 | v: pip | v: pip | same |  | unused |
| 89 | nvnCommandBufferClearBuffer | 0x60AD20 | v: plli | v: plli | same |  | unused |
| 90 | nvnCommandBufferClearColor | 0x607810 | v: pipi | v: pipi | same |  | 1 |
| 91 | nvnCommandBufferClearColori | 0x607970 | v: pipi | v: pipi | same |  | unused |
| 92 | nvnCommandBufferClearColorui | 0x607AD0 | v: pipi | v: pipi | same |  | unused |
| 93 | nvnCommandBufferClearDepthStencil | 0x607E80 | v: pfiii | v: pfiii | same |  | 2 |
| 94 | nvnCommandBufferClearTexture | 0x61E240 | v: pppppi | v: pppppi | same |  | unused |
| 95 | nvnCommandBufferClearTexturei | 0x61E6A0 | v: pppppi | v: pppppi | same |  | unused |
| 96 | nvnCommandBufferClearTextureui | 0x61EAF0 | v: pppppi | v: pppppi | same |  | unused |
| 97 | nvnCommandBufferCompactAccelerationStructure | 0x5F5C90 | l: pll (hexrays) |  | new | nvn2 | unused |
| 98 | nvnCommandBufferComputeDirtyTileMap | 0x5D4C30 | v: pllpilii (hexrays-unreliable) |  | new | nvn2 | unused |
| 99 | nvnCommandBufferComputeDirtyTileMapEx | 0x5D4CA0 | v: pl (hexrays) |  | new | nvn2 | unused |
| 100 | nvnCommandBufferCopyAccelerationStructure | 0x5F5960 | v: pll (hexrays) |  | new | nvn2 | unused |
| 101 | nvnCommandBufferCopyBufferToBuffer | 0x5D4920 | v: pllli | v: pllli | same |  | 1 |
| 102 | nvnCommandBufferCopyBufferToTexture | 0x5D3E40 | v: plpppi | v: plpppi | same |  | 1 |
| 103 | nvnCommandBufferCopyCommands | 0x5C6300 | v: pip | v: pip | same |  | unused |
| 104 | nvnCommandBufferCopyTextureToBuffer | 0x5D4580 | v: ppppli | v: ppppli | same |  | unused |
| 105 | nvnCommandBufferCopyTextureToTexture | 0x5D5340 | v: pppppppi | v: pppppppi | same |  | 3 |
| 106 | nvnCommandBufferDiscardColor | 0x61CA90 | v: pi | v: pi | same |  | unused |
| 107 | nvnCommandBufferDiscardDepthStencil | 0x61CB00 | v: p | v: p | same |  | unused |
| 108 | nvnCommandBufferDispatchCompute | 0x60D9C0 | v: piii | v: piii | same |  | 1 |
| 109 | nvnCommandBufferDispatchComputeIndirect | 0x60DB80 | v: pl | v: pl | same |  | unused |
| 110 | nvnCommandBufferDispatchRays | 0x5F6750 | v: plppliii (hexrays-unreliable) |  | new | nvn2 | unused |
| 111 | nvnCommandBufferDispatchRaysIndirect | 0x5F6F40 | v: plppll (hexrays) |  | new | nvn2 | unused |
| 112 | nvnCommandBufferDownsample | 0x5D5FD0 | v: ppp | v: ppp | same |  | unused |
| 113 | nvnCommandBufferDownsampleTextureView | 0x5D5FE0 | v: ppppp | v: ppppp | same |  | unused |
| 114 | nvnCommandBufferDrawArrays | 0x605A00 | v: piii | v: piii | same |  | 3 |
| 115 | nvnCommandBufferDrawArraysIndirect | 0x605DA0 | v: pil | v: pil | same |  | 1 |
| 116 | nvnCommandBufferDrawArraysInstanced | 0x605C20 | v: piiiii | v: piiiii | same |  | 1 |
| 117 | nvnCommandBufferDrawElements | 0x605A80 | v: piiil | v: piiil | same |  | unused |
| 118 | nvnCommandBufferDrawElementsBaseVertex | 0x605B10 | v: piiili | v: piiili | same |  | 3 |
| 119 | nvnCommandBufferDrawElementsIndirect | 0x605FB0 | v: piill | v: piill | same |  | 1 |
| 120 | nvnCommandBufferDrawElementsInstanced | 0x605CC0 | v: piiiliii | v: piiiliii | same |  | 2 |
| 121 | nvnCommandBufferDrawMeshTasks | 0x606970 | v: pii (hexrays) |  | new | nvn2 | unused |
| 122 | nvnCommandBufferDrawMeshTasks3D | 0x606E00 | v: piii (hexrays) |  | new | nvn2 | unused |
| 123 | nvnCommandBufferDrawMeshTasks3DIndirect | 0x606E80 | v: pl (hexrays) |  | new | nvn2 | unused |
| 124 | nvnCommandBufferDrawMeshTasksIndirect | 0x6069E0 | v: pl (hexrays) |  | new | nvn2 | unused |
| 125 | nvnCommandBufferDrawTexture | 0x60A6D0 | v: plpp | v: plpp | same |  | unused |
| 126 | nvnCommandBufferDrawTransformFeedback | 0x609460 | v: pil | v: pil | same |  | 1 |
| 127 | nvnCommandBufferEnableExclusiveScissor | 0x620EA0 | v: pi (hexrays) |  | new | nvn2 | unused |
| 128 | nvnCommandBufferEndDeviceCommands | 0x5DC320 | v: pp (sdk-verified) |  | new | nvn2 | unused |
| 129 | nvnCommandBufferEndRecording | 0x5C4DE0 | l: p | l: p | same |  | 1 |
| 130 | nvnCommandBufferEndTransformFeedback | 0x608EB0 | v: pl | v: pl | same |  | 8 |
| 131 | nvnCommandBufferEvaluateDlss | 0x5DEBC0 | v: pll (hexrays) |  | new | nvn2 | unused |
| 132 | nvnCommandBufferFenceSync | 0x5C6360 | v: ppii | v: ppii | same |  | 5 |
| 133 | nvnCommandBufferFinalize | 0x5C4930 | v: p | v: p | same |  | 1 |
| 134 | nvnCommandBufferGetAccelerationStructureVisualizationData | 0x5F62E0 | l: pll (hexrays) |  | new | nvn2 | unused |
| 135 | nvnCommandBufferGetCommandMemoryFree | 0x5C4CC0 | l: p | l: p | same |  | unused |
| 136 | nvnCommandBufferGetCommandMemorySize | 0x5C4CA0 | l: p | l: p | same |  | unused |
| 137 | nvnCommandBufferGetCommandMemoryUsed | 0x5C4CB0 | l: p | l: p | same |  | 5 |
| 138 | nvnCommandBufferGetControlMemoryFree | 0x5C4D00 | l: p | l: p | same |  | unused |
| 139 | nvnCommandBufferGetControlMemorySize | 0x5C4CD0 | l: p | l: p | same |  | unused |
| 140 | nvnCommandBufferGetControlMemoryUsed | 0x5C4CF0 | l: p | l: p | same |  | 5 |
| 141 | nvnCommandBufferGetCopyImageStride | 0x5D6020 | l: p | l: p | same |  | unused |
| 142 | nvnCommandBufferGetCopyRowStride | 0x5D6010 | l: p | l: p | same |  | unused |
| 143 | nvnCommandBufferGetMemoryCallback | 0x5C4990 | p: p | p: p | same |  | unused |
| 144 | nvnCommandBufferGetMemoryCallbackData | 0x5C49A0 | v: p | v: p | same |  | unused |
| 145 | nvnCommandBufferInitialize | 0x5C48E0 | i: pp | i: pp | same |  | 1 |
| 146 | nvnCommandBufferInsertCheckpointDynamic | 0x5C4870 | v:  (noop-stub) |  | new | nvn2 | unused |
| 147 | nvnCommandBufferInsertCheckpointStatic | 0x5C4860 | v:  (noop-stub) |  | new | nvn2 | unused |
| 148 | nvnCommandBufferInsertDebugMarker | 0x60BDE0 | v: pip (sdk-verified) | v: pp | changed-sig |  | unused |
| 149 | nvnCommandBufferInsertDebugMarkerCompute | 0x60BB80 | v: pil (hexrays) |  | new | nvn2 | unused |
| 150 | nvnCommandBufferInsertDebugMarkerGraphics | 0x60B920 | v: pil (hexrays) |  | new | nvn2 | unused |
| 151 | nvnCommandBufferIsRecording | 0x5C6CE0 | i: p | i: p | same |  | unused |
| 152 | nvnCommandBufferMultiDrawArraysIndirectCount | 0x6061E0 | v: pillil | v: pillil | same |  | unused |
| 153 | nvnCommandBufferMultiDrawElementsIndirectCount | 0x606790 | v: piilllil | v: piilllil | same |  | unused |
| 154 | nvnCommandBufferMultiDrawMeshTasks3DIndirect | 0x6070D0 | v: plii (hexrays) |  | new | nvn2 | unused |
| 155 | nvnCommandBufferMultiDrawMeshTasks3DIndirectCount | 0x6071D0 | v: pllii (hexrays) |  | new | nvn2 | unused |
| 156 | nvnCommandBufferMultiDrawMeshTasksIndirect | 0x606BF0 | v: plii (hexrays) |  | new | nvn2 | unused |
| 157 | nvnCommandBufferMultiDrawMeshTasksIndirectCount | 0x606CF0 | v: pllii (hexrays) |  | new | nvn2 | unused |
| 158 | nvnCommandBufferOverrideSubtileSize | 0x60C560 | v: pii (hexrays) |  | new | nvn2 | unused |
| 159 | nvnCommandBufferPauseTransformFeedback | 0x608F90 | v: pl | v: pl | same |  | unused |
| 160 | nvnCommandBufferPopDebugGroup | 0x60C300 | v: pi (sdk-verified) | v: p | changed-sig |  | unused |
| 161 | nvnCommandBufferPopDebugGroupCompute | 0x60C290 | v: pi (sdk-verified) |  | new | nvn2 | 1 |
| 162 | nvnCommandBufferPopDebugGroupGraphics | 0x60C220 | v: pi (sdk-verified) |  | new | nvn2 | 1 |
| 163 | nvnCommandBufferPushComputeConstants | 0x5F8AA0 | v: piiip (hexrays) |  | new | nvn2 | unused |
| 164 | nvnCommandBufferPushComputeConstantsDeferred | 0x5F8E10 | v: piiil (hexrays) |  | new | nvn2 | unused |
| 165 | nvnCommandBufferPushDebugGroup | 0x60B4E0 | v: pip | v: pip | same |  | unused |
| 166 | nvnCommandBufferPushDebugGroupCompute | 0x60B280 | v: pip (sdk-verified) |  | new | nvn2 | 1 |
| 167 | nvnCommandBufferPushDebugGroupGraphics | 0x60B020 | v: pip (sdk-verified) |  | new | nvn2 | 1 |
| 168 | nvnCommandBufferPushGraphicsConstants | 0x5F85E0 | v: piiip (hexrays) |  | new | nvn2 | unused |
| 169 | nvnCommandBufferPushGraphicsConstantsDeferred | 0x5F8740 | v: piiil (hexrays) |  | new | nvn2 | unused |
| 170 | nvnCommandBufferRelocateAccelerationStructure | 0x5F5FC0 | v: pl (hexrays) |  | new | nvn2 | unused |
| 171 | nvnCommandBufferReportCounter | 0x61CF90 | v: pil | v: pil | same |  | 1 |
| 172 | nvnCommandBufferReportValue | 0x61D4F0 | v: pil | v: pil | same |  | unused |
| 173 | nvnCommandBufferResetCounter | 0x61D680 | v: pi | v: pi | same |  | unused |
| 174 | nvnCommandBufferResetDeviceMessageBuffer | 0x5DD9E0 | v: pl (hexrays) |  | new | nvn2 | unused |
| 175 | nvnCommandBufferResetDeviceMessageBufferDeferred | 0x5DDB60 | v: pl (hexrays) |  | new | nvn2 | unused |
| 176 | nvnCommandBufferResolveDepthBuffer | 0x60ACC0 | v: p | v: p | same |  | unused |
| 177 | nvnCommandBufferRestoreZCullData | 0x60A310 | v: pll | v: pll | same |  | unused |
| 178 | nvnCommandBufferResumeTransformFeedback | 0x6090A0 | v: pl | v: pl | same |  | unused |
| 179 | nvnCommandBufferSaveZCullData | 0x60A160 | v: pll | v: pll | same |  | unused |
| 180 | nvnCommandBufferSetAlphaRef | 0x607730 | v: pf | v: pf | same |  | unused |
| 181 | nvnCommandBufferSetBlendColor | 0x607520 | v: pp | v: pp | same |  | unused |
| 182 | nvnCommandBufferSetColorReductionEnable | 0x60C3C0 | v: pi (hexrays) |  | new | nvn2 | unused |
| 183 | nvnCommandBufferSetColorReductionThresholds | 0x60C430 | v: piii (hexrays) |  | new | nvn2 | unused |
| 184 | nvnCommandBufferSetCommandMemoryCallbackEnabled | 0x5C49B0 | v: pi (hexrays) |  | new | nvn2 | unused |
| 185 | nvnCommandBufferSetComputeEnable | 0x610560 | v: pi (hexrays) |  | new | nvn2 | unused |
| 186 | nvnCommandBufferSetComputeEnableConditional | 0x6105D0 | v: pil (hexrays) |  | new | nvn2 | unused |
| 187 | nvnCommandBufferSetComputeOverlapMode | 0x60CA40 | v: pi (hexrays) |  | new | nvn2 | unused |
| 188 | nvnCommandBufferSetComputeWorkloadPriority | 0x60CDF0 | v: pi (hexrays) |  | new | nvn2 | unused |
| 189 | nvnCommandBufferSetConservativeRasterDilate | 0x609FA0 | v: pf | v: pf | same |  | unused |
| 190 | nvnCommandBufferSetConservativeRasterEnable | 0x609F30 | v: pi | v: pi | same |  | unused |
| 191 | nvnCommandBufferSetConservativeRasterMode | 0x60A030 | v: pii (hexrays) |  | new | nvn2 | unused |
| 192 | nvnCommandBufferSetControlMemoryCallbackEnabled | 0x5C4BA0 | v: pi (hexrays) |  | new | nvn2 | unused |
| 193 | nvnCommandBufferSetCopyEnable | 0x5D61B0 | v: pi (hexrays) |  | new | nvn2 | unused |
| 194 | nvnCommandBufferSetCopyEnableConditional | 0x5D6240 | v: pil (hexrays) |  | new | nvn2 | unused |
| 195 | nvnCommandBufferSetCopyImageStride | 0x5D6000 | v: pl | v: pl | same |  | 2 |
| 196 | nvnCommandBufferSetCopyRowStride | 0x5D5FF0 | v: pl | v: pl | same |  | 2 |
| 197 | nvnCommandBufferSetDebugLabel | 0x5D63C0 | v: pp | v: pp | same |  | unused |
| 198 | nvnCommandBufferSetDepthBounds | 0x6087D0 | v: piff | v: piff | same |  | 2 |
| 199 | nvnCommandBufferSetDepthClamp | 0x6097E0 | v: pi | v: pi | same |  | unused |
| 200 | nvnCommandBufferSetDepthRange | 0x608610 | v: pff | v: pff | same |  | unused |
| 201 | nvnCommandBufferSetDepthRanges | 0x608500 | v: piip | v: piip | same |  | 1 |
| 202 | nvnCommandBufferSetExclusiveScissor | 0x620F40 | v: piiii (hexrays) |  | new | nvn2 | unused |
| 203 | nvnCommandBufferSetExclusiveScissors | 0x620F80 | v: piip (hexrays) |  | new | nvn2 | unused |
| 204 | nvnCommandBufferSetFloatBlendClamp | 0x60D2D0 | v: pi (hexrays) |  | new | nvn2 | unused |
| 205 | nvnCommandBufferSetGeometryProcessingMode | 0x606900 | v: pi (hexrays) |  | new | nvn2 | unused |
| 206 | nvnCommandBufferSetGraphicsWorkloadPriority | 0x60CBE0 | v: pi (hexrays) |  | new | nvn2 | unused |
| 207 | nvnCommandBufferSetInnerTessellationLevels | 0x608BA0 | v: pp | v: pp | same |  | unused |
| 208 | nvnCommandBufferSetLineWidth | 0x607620 | v: pf | v: pf | same |  | unused |
| 209 | nvnCommandBufferSetMemoryCallback | 0x5C4970 | v: pp | v: pp | same |  | 1 |
| 210 | nvnCommandBufferSetMemoryCallbackData | 0x5C4980 | v: pp | v: pp | same |  | 1 |
| 211 | nvnCommandBufferSetOuterTessellationLevels | 0x608C10 | v: pp | v: pp | same |  | unused |
| 212 | nvnCommandBufferSetPatchSize | 0x608B40 | v: pi | v: pi | same |  | 1 |
| 213 | nvnCommandBufferSetPointSize | 0x6075A0 | v: pf | v: pf | same |  | unused |
| 214 | nvnCommandBufferSetPolygonOffsetClamp | 0x607690 | v: pfff | v: pfff | same |  | 1 |
| 215 | nvnCommandBufferSetPrimitiveRestart | 0x6072E0 | v: pii | v: pii | same |  | 1 |
| 216 | nvnCommandBufferSetProgramSubroutines | 0x5C6A70 | v: ppiiip | v: ppiiip | same |  | unused |
| 217 | nvnCommandBufferSetProvokingVertexMode | 0x60C890 | v: pi (hexrays) |  | new | nvn2 | unused |
| 218 | nvnCommandBufferSetRasterizerDiscard | 0x609770 | v: pi | v: pi | same |  | 9 |
| 219 | nvnCommandBufferSetRecordingMode | 0x5C4D10 | v: pi (sdk-verified) |  | new | not-in-nx-table | 2 |
| 220 | nvnCommandBufferSetRenderEnable | 0x61D6F0 | v: pi | v: pi | same |  | unused |
| 221 | nvnCommandBufferSetRenderEnableConditional | 0x61D760 | v: pil | v: pil | same |  | unused |
| 222 | nvnCommandBufferSetRenderTargets | 0x61C000 | v: pipppp | v: pipppp | same |  | 3 |
| 223 | nvnCommandBufferSetRepresentativeFragmentTest | 0x61B5A0 | v: pi (hexrays) |  | new | nvn2 | unused |
| 224 | nvnCommandBufferSetResourceOccupancy | 0x60D0F0 | v: pif (hexrays) |  | new | nvn2 | unused |
| 225 | nvnCommandBufferSetSampleMask | 0x6077A0 | v: pi | v: pi | same |  | unused |
| 226 | nvnCommandBufferSetSamplerPool | 0x609D90 | v: pp | v: pp | same |  | 1 |
| 227 | nvnCommandBufferSetScissor | 0x608B00 | v: piiii | v: piiii | same |  | 2 |
| 228 | nvnCommandBufferSetScissors | 0x608880 | v: piip | v: piip | same |  | 1 |
| 229 | nvnCommandBufferSetShaderScratchMemory | 0x5C6820 | v: ppll | v: ppll | same |  | 3 |
| 230 | nvnCommandBufferSetShadingRatePaletteMode | 0x621340 | v: pi (hexrays) |  | new | nvn2 | unused |
| 231 | nvnCommandBufferSetStencilCullCriteria | 0x60C4E0 | v: piii (hexrays) |  | new | nvn2 | unused |
| 232 | nvnCommandBufferSetStencilMask | 0x607400 | v: pii | v: pii | same |  | 1 |
| 233 | nvnCommandBufferSetStencilRef | 0x607490 | v: pii | v: pii | same |  | 1 |
| 234 | nvnCommandBufferSetStencilValueMask | 0x607370 | v: pii | v: pii | same |  | 1 |
| 235 | nvnCommandBufferSetSubpixelPrecisionBias | 0x60A0B0 | v: pii | v: pii | same |  | unused |
| 236 | nvnCommandBufferSetTexturePool | 0x609B20 | v: pp | v: pp | same |  | 1 |
| 237 | nvnCommandBufferSetTiledCacheAction | 0x609850 | v: pi | v: pi | same |  | 2 |
| 238 | nvnCommandBufferSetTiledCacheTileSize | 0x609920 | v: pii | v: pii | same |  | 1 |
| 239 | nvnCommandBufferSetViewport | 0x6084A0 | v: piiii | v: piiii | same |  | unused |
| 240 | nvnCommandBufferSetViewportSwizzles | 0x608640 | v: piip | v: piip | same |  | unused |
| 241 | nvnCommandBufferSetViewports | 0x608060 | v: piip | v: piip | same |  | 1 |
| 242 | nvnCommandBufferSetZCullZF32CompressionEnable | 0x60C6E0 | v: pi (hexrays) |  | new | nvn2 | unused |
| 243 | nvnCommandBufferShadingRateImagePalette | 0x620D80 | v: piil (hexrays) |  | new | nvn2 | unused |
| 244 | nvnCommandBufferSignalEvent | 0x5E1320 | v: ppiiii | v: ppiiii | same |  | unused |
| 245 | nvnCommandBufferSubmitDeviceCommands | 0x5DC680 | v: pp (sdk-verified) |  | new | nvn2 | unused |
| 246 | nvnCommandBufferTiledDownsample | 0x61E220 | v: ppp | v: ppp | same |  | unused |
| 247 | nvnCommandBufferTiledDownsampleTextureView | 0x61E230 | v: ppppp | v: ppppp | same |  | unused |
| 248 | nvnCommandBufferUpdateUniformBuffer | 0x60A4E0 | v: pllllp | v: pllllp | same |  | unused |
| 249 | nvnCommandBufferWaitEvent | 0x5E0DE0 | v: ppii | v: ppii | same |  | unused |
| 250 | nvnCommandBufferWaitSync | 0x5C65E0 | v: pp | v: pp | same |  | 4 |
| 251 | nvnDepthStencilStateGetDepthFunc | 0x5EC720 | i: p | i: p | same |  | unused |
| 252 | nvnDepthStencilStateGetDepthTestEnable | 0x5EC700 | i: p | i: p | same |  | unused |
| 253 | nvnDepthStencilStateGetDepthWriteEnable | 0x5EC710 | i: p | i: p | same |  | unused |
| 254 | nvnDepthStencilStateGetStencilFunc | 0x5EC740 | i: pi | i: pi | same |  | unused |
| 255 | nvnDepthStencilStateGetStencilOp | 0x5EC760 | v: pippp | v: pippp | same |  | unused |
| 256 | nvnDepthStencilStateGetStencilTestEnable | 0x5EC730 | i: p | i: p | same |  | unused |
| 257 | nvnDepthStencilStateSetDefaults | 0x5EC5D0 | v: p | v: p | same |  | 1 |
| 258 | nvnDepthStencilStateSetDepthFunc | 0x5EC630 | v: pi | v: pi | same |  | 1 |
| 259 | nvnDepthStencilStateSetDepthTestEnable | 0x5EC5F0 | v: pi | v: pi | same |  | 1 |
| 260 | nvnDepthStencilStateSetDepthWriteEnable | 0x5EC610 | v: pi | v: pi | same |  | 1 |
| 261 | nvnDepthStencilStateSetStencilFunc | 0x5EC670 | v: pii | v: pii | same |  | 1 |
| 262 | nvnDepthStencilStateSetStencilOp | 0x5EC6B0 | v: piiii | v: piiii | same |  | 2 |
| 263 | nvnDepthStencilStateSetStencilTestEnable | 0x5EC650 | v: pi | v: pi | same |  | 1 |
| 264 | nvnDeviceBuilderGetFlags | 0x5D64E0 | l: p (hexrays) |  | new | not-in-nx-table | unused |
| 265 | nvnDeviceBuilderSetDefaults | 0x5D64F0 | v: p | v: p | same |  | 1 |
| 266 | nvnDeviceBuilderSetFlags | 0x5D6500 | v: pi | v: pi | same |  | 1 |
| 267 | nvnDeviceCalculateAccelerationStructureMemoryRequirements | 0x5F5040 | l: plppp (hexrays) |  | new | nvn2 | unused |
| 268 | nvnDeviceCommandBufferGetHandle | 0x5DC200 | l: p (sdk-verified) |  | new | nvn2 | unused |
| 269 | nvnDeviceCommandBufferSetCommandMemorySize | 0x5DC1E0 | v: pl (sdk-verified) |  | new | nvn2 | unused |
| 270 | nvnDeviceCommandBufferSetComputeMemorySize | 0x5DC1F0 | v: pl (sdk-verified) |  | new | nvn2 | unused |
| 271 | nvnDeviceCommandBufferSetDefaults | 0x5DC1A0 | v: p (sdk-verified) |  | new | nvn2 | unused |
| 272 | nvnDeviceCommandBufferSetDevice | 0x5DC1C0 | v: pp (sdk-verified) |  | new | nvn2 | unused |
| 273 | nvnDeviceCommandBufferSetStorage | 0x5DC1D0 | v: ppll (sdk-verified) |  | new | nvn2 | unused |
| 274 | nvnDeviceCompactAccelerationStructure | 0x5F7A50 | l: plll (hexrays) |  | new | nvn2 | unused |
| 275 | nvnDeviceCopyAccelerationStructure | 0x5F7A30 | v: plll (hexrays) |  | new | nvn2 | unused |
| 276 | nvnDeviceFinalize | 0x5D6510 | v: p | v: p | same |  | 1 |
| 277 | nvnDeviceFinalizeCommandHandle | 0x5C6CF0 | v: pl | v: pl | same |  | 2 |
| 278 | nvnDeviceGenerateDebugDomainId | 0x5D6790 | i: pp | i: pp | same |  | unused |
| 279 | nvnDeviceGetCommandHandleMemorySize | 0x5DC9F0 | l: pl (hexrays) |  | new | nvn2 | unused |
| 280 | nvnDeviceGetCurrentTimestampInNanoseconds | 0x5D67B0 | l: p | l: p | same |  | unused |
| 281 | nvnDeviceGetDepthMode | 0x5D6810 | i: p | i: p | same |  | unused |
| 282 | nvnDeviceGetFlags | 0x5D6820 | l: p (hexrays) |  | new | nvn2 | unused |
| 283 | nvnDeviceGetImageHandle | 0x5D6830 | l: pi | l: pi | same |  | 2 |
| 284 | nvnDeviceGetInteger | 0x5D6840 | v: pip | v: pip | same |  | 21 |
| 285 | nvnDeviceGetProcAddress | 0x5D6880 | p: pp | p: pp | same |  | unused |
| 286 | nvnDeviceGetSeparateSamplerHandle | 0x5D6890 | S: pi | S: pi | same |  | unused |
| 287 | nvnDeviceGetSeparateTextureHandle | 0x5D68A0 | S: pi | S: pi | same |  | unused |
| 288 | nvnDeviceGetTexelFetchHandle | 0x5D68B0 | l: pi | l: pi | same |  | unused |
| 289 | nvnDeviceGetTextureHandle | 0x5D68C0 | l: pii | l: pii | same |  | 1 |
| 290 | nvnDeviceGetTimestampInNanoseconds | 0x5D68D0 | l: pp | l: pp | same |  | unused |
| 291 | nvnDeviceGetUniformBufferHandle | 0x5F85B0 | l: pll (hexrays) |  | new | nvn2 | unused |
| 292 | nvnDeviceGetWindowOriginMode | 0x5D68F0 | i: p | i: p | same |  | unused |
| 293 | nvnDeviceInitialize | 0x5D6900 | i: pp | i: pp | same |  | 1 |
| 294 | nvnDeviceInstallDebugCallback | 0x5D6390 | v: pppi | v: pppi | same |  | unused |
| 295 | nvnDeviceIsExternalDebuggerAttached | 0x5D6910 | i: p | i: p | same |  | unused |
| 296 | nvnDeviceMessageBufferBeginMessages | 0x5DCA80 | v: p (hexrays) |  | new | nvn2 | unused |
| 297 | nvnDeviceMessageBufferEndMessages | 0x5DCAD0 | v: p (hexrays) |  | new | nvn2 | unused |
| 298 | nvnDeviceMessageBufferGetNextMessage | 0x5DCAE0 | l: ppllllllllllllllllllllllllll (hexrays-unreliable) |  | new | nvn2 | unused |
| 299 | nvnDeviceMessageBufferReset | 0x5DCA40 | v: p (hexrays) |  | new | nvn2 | unused |
| 300 | nvnDeviceMessageBufferSetDefaults | 0x5DCA10 | v: p (hexrays) |  | new | nvn2 | unused |
| 301 | nvnDeviceMessageBufferSetDevice | 0x5DCA20 | v: pl (hexrays) |  | new | nvn2 | unused |
| 302 | nvnDeviceMessageBufferSetStorage | 0x5DCA30 | v: plii (hexrays) |  | new | nvn2 | unused |
| 303 | nvnDeviceRegisterFastClearColor | 0x5D6920 | i: ppi | i: ppi | same |  | unused |
| 304 | nvnDeviceRegisterFastClearColorIndex | 0x5D6930 | v:  (noop-stub) |  | new | nvn2 | unused |
| 305 | nvnDeviceRegisterFastClearColori | 0x5D6940 | i: ppi | i: ppi | same |  | unused |
| 306 | nvnDeviceRegisterFastClearColoriIndex | 0x5D6950 | v:  (noop-stub) |  | new | nvn2 | unused |
| 307 | nvnDeviceRegisterFastClearColorui | 0x5D6960 | i: ppi | i: ppi | same |  | unused |
| 308 | nvnDeviceRegisterFastClearColoruiIndex | 0x5D6970 | v:  (noop-stub) |  | new | nvn2 | unused |
| 309 | nvnDeviceRegisterFastClearDepth | 0x5D6980 | i: pf | i: pf | same |  | unused |
| 310 | nvnDeviceRegisterFastClearDepthIndex | 0x5D69A0 | v:  (noop-stub) |  | new | nvn2 | unused |
| 311 | nvnDeviceRegisterFastClearStencil | 0x5D69B0 | v: pl (hexrays) |  | new | nvn2 | unused |
| 312 | nvnDeviceRegisterFastClearStencilIndex | 0x5D69E0 | v: pll (hexrays) |  | new | nvn2 | unused |
| 313 | nvnDeviceRelocateAccelerationStructure | 0x5F7A70 | v: pll (hexrays) |  | new | nvn2 | unused |
| 314 | nvnDeviceSetAutomaticCheckpointsByDebugGroup | 0x5C48B0 | v:  (noop-stub) |  | new | nvn2 | unused |
| 315 | nvnDeviceSetAutomaticCheckpointsEnable | 0x5C4880 | v:  (noop-stub) |  | new | nvn2 | unused |
| 316 | nvnDeviceSetAutomaticCheckpointsMask | 0x5C4890 | v:  (noop-stub) |  | new | nvn2 | unused |
| 317 | nvnDeviceSetAutomaticCheckpointsSamplingInterval | 0x5C48A0 | v:  (noop-stub) |  | new | nvn2 | unused |
| 318 | nvnDeviceSetCheckpointCallStacksEnable | 0x5C48C0 | v:  (noop-stub) |  | new | nvn2 | unused |
| 319 | nvnDeviceSetCheckpointMaxCallStacksDepth | 0x5C48D0 | v:  (noop-stub) |  | new | nvn2 | unused |
| 320 | nvnDeviceSetDebugLabel | 0x5D63D0 | v: pp | v: pp | same |  | unused |
| 321 | nvnDeviceSetDepthMode | 0x5D69F0 | v: pi | v: pi | same |  | 1 |
| 322 | nvnDeviceSetIntermediateShaderCache | 0x5D6A00 | v: pi | v: pi | same |  | unused |
| 323 | nvnDeviceSetWindowOriginMode | 0x5D6A10 | v: pi | v: pi | same |  | 1 |
| 324 | nvnDeviceWalkDebugDatabase | 0x5D6A20 | v: pipp | v: pipp | same |  | unused |
| 325 | nvnDirtyTileMapParametersGetConstantColorTable | 0x5D52A0 | l: ppppp (hexrays) |  | new | nvn2 | unused |
| 326 | nvnDirtyTileMapParametersGetDepthStencilMask | 0x5D52E0 | l: ppp (hexrays) |  | new | nvn2 | unused |
| 327 | nvnDirtyTileMapParametersGetExpandFootprint | 0x5D5300 | p: ppppp (hexrays) |  | new | nvn2 | unused |
| 328 | nvnDirtyTileMapParametersGetInputs | 0x5D5220 | p: ppppp (hexrays) |  | new | nvn2 | unused |
| 329 | nvnDirtyTileMapParametersGetOutputs | 0x5D5260 | l: ppppp (hexrays) |  | new | nvn2 | unused |
| 330 | nvnDirtyTileMapParametersGetTexture | 0x5D51F0 | l: pppl (hexrays) |  | new | nvn2 | unused |
| 331 | nvnDirtyTileMapParametersSetConstantColorTable | 0x5D51B0 | v: pliil (hexrays) |  | new | nvn2 | unused |
| 332 | nvnDirtyTileMapParametersSetDefaults | 0x5D5130 | v: p (hexrays) |  | new | nvn2 | unused |
| 333 | nvnDirtyTileMapParametersSetDepthStencilMask | 0x5D51C0 | v: pii (hexrays) |  | new | nvn2 | unused |
| 334 | nvnDirtyTileMapParametersSetExpandFootprint | 0x5D51D0 | v: piiii (hexrays) |  | new | nvn2 | unused |
| 335 | nvnDirtyTileMapParametersSetInputs | 0x5D5190 | v: piiii (hexrays) |  | new | nvn2 | unused |
| 336 | nvnDirtyTileMapParametersSetOutputs | 0x5D51A0 | v: pliil (hexrays) |  | new | nvn2 | unused |
| 337 | nvnDirtyTileMapParametersSetTexture | 0x5D5170 | v: plll (hexrays) |  | new | nvn2 | unused |
| 338 | nvnDlssNetworkBuilderGetDevice | 0x5DF2E0 | l: p (hexrays) |  | new | nvn2 | unused |
| 339 | nvnDlssNetworkBuilderGetFlags | 0x5DF300 | l: p (hexrays) |  | new | nvn2 | unused |
| 340 | nvnDlssNetworkBuilderGetMaxEvaluationEntries | 0x5DF430 | l: p (hexrays) |  | new | nvn2 | unused |
| 341 | nvnDlssNetworkBuilderGetMemory | 0x5DF390 | l: p (hexrays) |  | new | nvn2 | unused |
| 342 | nvnDlssNetworkBuilderGetMemorySize | 0x5DF3A0 | l: p (hexrays) |  | new | nvn2 | unused |
| 343 | nvnDlssNetworkBuilderGetMethod | 0x5DF350 | l: p (hexrays) |  | new | nvn2 | unused |
| 344 | nvnDlssNetworkBuilderGetOptimalSettings | 0x5DE330 | l: pllllll (hexrays-unreliable) |  | new | nvn2 | unused |
| 345 | nvnDlssNetworkBuilderGetOutputResolution | 0x5DF320 | l: ppp (hexrays) |  | new | nvn2 | unused |
| 346 | nvnDlssNetworkBuilderGetPreset | 0x5DF370 | l: p (hexrays) |  | new | nvn2 | unused |
| 347 | nvnDlssNetworkBuilderGetStorageSize | 0x5DE620 | l: pi (hexrays) |  | new | nvn2 | unused |
| 348 | nvnDlssNetworkBuilderGetTextureIDs | 0x5DF400 | l: pl (hexrays) |  | new | nvn2 | unused |
| 349 | nvnDlssNetworkBuilderGetTexturePool | 0x5DF3F0 | l: p (hexrays) |  | new | nvn2 | unused |
| 350 | nvnDlssNetworkBuilderSetDefaults | 0x5DF290 | v: p (hexrays) |  | new | nvn2 | unused |
| 351 | nvnDlssNetworkBuilderSetDevice | 0x5DF2D0 | v: pl (hexrays) |  | new | nvn2 | unused |
| 352 | nvnDlssNetworkBuilderSetFlags | 0x5DF2F0 | v: pi (hexrays) |  | new | nvn2 | unused |
| 353 | nvnDlssNetworkBuilderSetMaxEvaluationEntries | 0x5DF420 | v: pi (hexrays) |  | new | nvn2 | unused |
| 354 | nvnDlssNetworkBuilderSetMethod | 0x5DF340 | v: pi (hexrays) |  | new | nvn2 | unused |
| 355 | nvnDlssNetworkBuilderSetOutputResolution | 0x5DF310 | v: pii (hexrays) |  | new | nvn2 | unused |
| 356 | nvnDlssNetworkBuilderSetPreset | 0x5DF360 | v: pi (hexrays) |  | new | nvn2 | unused |
| 357 | nvnDlssNetworkBuilderSetStorage | 0x5DF380 | v: pll (hexrays) |  | new | nvn2 | unused |
| 358 | nvnDlssNetworkBuilderSetTextureIDs | 0x5DF3B0 | v: pll (hexrays) |  | new | nvn2 | unused |
| 359 | nvnDlssNetworkFinalize | 0x5DE590 | v: p (hexrays) |  | new | nvn2 | unused |
| 360 | nvnDlssNetworkInitialize | 0x5DE390 | i: pl (hexrays) |  | new | nvn2 | unused |
| 361 | nvnDlssNetworkResize | 0x5DE230 | l: pli (hexrays) |  | new | nvn2 | unused |
| 362 | nvnDlssParametersGetBiasCurrentColorSubrectangleBase | 0x5DF6B0 | l: ppp (hexrays) |  | new | nvn2 | unused |
| 363 | nvnDlssParametersGetDepthSubrectangleBase | 0x5DF650 | l: ppp (hexrays) |  | new | nvn2 | unused |
| 364 | nvnDlssParametersGetEvaluationIndex | 0x5DF5D0 | l: p (hexrays) |  | new | nvn2 | unused |
| 365 | nvnDlssParametersGetHistoryReset | 0x5DF580 | l: p (hexrays) |  | new | nvn2 | unused |
| 366 | nvnDlssParametersGetInputColorSubrectangleBase | 0x5DF620 | l: ppp (hexrays) |  | new | nvn2 | unused |
| 367 | nvnDlssParametersGetInputSubrectangleSize | 0x5DF5F0 | l: ppp (hexrays) |  | new | nvn2 | unused |
| 368 | nvnDlssParametersGetJitterOffset | 0x5DF520 | f: ppp (hexrays) |  | new | nvn2 | unused |
| 369 | nvnDlssParametersGetMotionVectorScale | 0x5DF5A0 | f: ppp (hexrays) |  | new | nvn2 | unused |
| 370 | nvnDlssParametersGetMotionVectorSubrectangleBase | 0x5DF680 | l: ppp (hexrays) |  | new | nvn2 | unused |
| 371 | nvnDlssParametersGetOutputSubrectangleBase | 0x5DF6E0 | l: ppp (hexrays) |  | new | nvn2 | unused |
| 372 | nvnDlssParametersGetPreExposure | 0x5DF550 | f: p (hexrays) |  | new | nvn2 | unused |
| 373 | nvnDlssParametersSetBiasCurrentColorMask | 0x5DF500 | v: pll (hexrays) |  | new | nvn2 | unused |
| 374 | nvnDlssParametersSetBiasCurrentColorSubrectangleBase | 0x5DF6A0 | v: pii (hexrays) |  | new | nvn2 | unused |
| 375 | nvnDlssParametersSetDefaults | 0x5DF440 | v: p (hexrays) |  | new | nvn2 | unused |
| 376 | nvnDlssParametersSetDepth | 0x5DF4F0 | v: pll (hexrays) |  | new | nvn2 | unused |
| 377 | nvnDlssParametersSetDepthSubrectangleBase | 0x5DF640 | v: pii (hexrays) |  | new | nvn2 | unused |
| 378 | nvnDlssParametersSetEvaluationIndex | 0x5DF5C0 | v: pi (hexrays) |  | new | nvn2 | unused |
| 379 | nvnDlssParametersSetExposureTexture | 0x5DF560 | v: pll (hexrays) |  | new | nvn2 | unused |
| 380 | nvnDlssParametersSetHistoryReset | 0x5DF570 | v: pi (hexrays) |  | new | nvn2 | unused |
| 381 | nvnDlssParametersSetInputColor | 0x5DF480 | v: pll (hexrays) |  | new | nvn2 | unused |
| 382 | nvnDlssParametersSetInputColorSubrectangleBase | 0x5DF610 | v: pii (hexrays) |  | new | nvn2 | unused |
| 383 | nvnDlssParametersSetInputSubrectangleSize | 0x5DF5E0 | v: pii (hexrays) |  | new | nvn2 | unused |
| 384 | nvnDlssParametersSetJitterOffset | 0x5DF510 | v: pff (hexrays) |  | new | nvn2 | unused |
| 385 | nvnDlssParametersSetMotionVectorScale | 0x5DF590 | v: pff (hexrays) |  | new | nvn2 | unused |
| 386 | nvnDlssParametersSetMotionVectorSubrectangleBase | 0x5DF670 | v: pii (hexrays) |  | new | nvn2 | unused |
| 387 | nvnDlssParametersSetMotionVectors | 0x5DF4E0 | v: pll (hexrays) |  | new | nvn2 | unused |
| 388 | nvnDlssParametersSetOutputColor | 0x5DF490 | v: pll (hexrays) |  | new | nvn2 | unused |
| 389 | nvnDlssParametersSetOutputSubrectangleBase | 0x5DF6D0 | v: pii (hexrays) |  | new | nvn2 | unused |
| 390 | nvnDlssParametersSetOverlayDebugLayer | 0x5DF4A0 | v: pi (hexrays) |  | new | nvn2 | unused |
| 391 | nvnDlssParametersSetOverlayFullScreen | 0x5DF4B0 | v: pi (hexrays) |  | new | nvn2 | unused |
| 392 | nvnDlssParametersSetOverlayJitterDebug | 0x5DF4D0 | v: pi (hexrays) |  | new | nvn2 | unused |
| 393 | nvnDlssParametersSetOverlayShowNans | 0x5DF4C0 | v: pi (hexrays) |  | new | nvn2 | unused |
| 394 | nvnDlssParametersSetPreExposure | 0x5DF540 | v: pf (hexrays) |  | new | nvn2 | unused |
| 395 | nvnEventBuilderGetMemoryOffset | 0x5E0D20 | l: p (hexrays) |  | new | not-in-nx-table | unused |
| 396 | nvnEventBuilderGetMemoryPool | 0x5E0D10 | l: p (hexrays) |  | new | not-in-nx-table | unused |
| 397 | nvnEventBuilderGetStorage | 0x5E0D00 | l: pp (hexrays) |  | new | not-in-nx-table | unused |
| 398 | nvnEventBuilderSetDefaults | 0x5E0CE0 | v: p | v: p | same |  | unused |
| 399 | nvnEventBuilderSetStorage | 0x5E0CF0 | v: ppl | v: ppl | same |  | unused |
| 400 | nvnEventFinalize | 0x5E0D80 | v: p | v: p | same |  | unused |
| 401 | nvnEventGetMemoryOffset | 0x5E0DB0 | l: p (hexrays) |  | new | not-in-nx-table | unused |
| 402 | nvnEventGetMemoryPool | 0x5E0DA0 | l: p (hexrays) |  | new | not-in-nx-table | unused |
| 403 | nvnEventGetValue | 0x5E0D90 | i: p | i: p | same |  | unused |
| 404 | nvnEventInitialize | 0x5E0D30 | i: pp | i: pp | same |  | unused |
| 405 | nvnEventSignal | 0x5E0DC0 | v: pii | v: pii | same |  | unused |
| 406 | nvnMemoryPoolBuilderGetDevice | 0x5E2460 | l: p (hexrays) |  | new | not-in-nx-table | unused |
| 407 | nvnMemoryPoolBuilderGetFlags | 0x5E2490 | i: p | i: p | same |  | unused |
| 408 | nvnMemoryPoolBuilderGetMemory | 0x5E2470 | v: p | v: p | same |  | unused |
| 409 | nvnMemoryPoolBuilderGetSize | 0x5E2480 | l: p | l: p | same |  | unused |
| 410 | nvnMemoryPoolBuilderSetDefaults | 0x5E2360 | v: p | v: p | same |  | 1 |
| 411 | nvnMemoryPoolBuilderSetDevice | 0x5E2350 | v: pp | v: pp | same |  | 1 |
| 412 | nvnMemoryPoolBuilderSetFlags | 0x5E2450 | v: pi | v: pi | same |  | 1 |
| 413 | nvnMemoryPoolBuilderSetStorage | 0x5E2380 | v: ppl | v: ppl | same |  | 1 |
| 414 | nvnMemoryPoolFinalize | 0x5E2680 | v: p | v: p | same |  | 1 |
| 415 | nvnMemoryPoolFlushMappedRange | 0x5E2760 | v: pll | v: pll | same |  | 6 |
| 416 | nvnMemoryPoolGetBufferAddress | 0x5E2780 | l: p | l: p | same |  | 1 |
| 417 | nvnMemoryPoolGetFlags | 0x5E2800 | i: p | i: p | same |  | 3 |
| 418 | nvnMemoryPoolGetSize | 0x5E27F0 | l: p | l: p | same |  | unused |
| 419 | nvnMemoryPoolInitialize | 0x5E24A0 | i: pp | i: pp | same |  | 1 |
| 420 | nvnMemoryPoolInvalidateMappedRange | 0x5E2770 | v: pll | v: pll | same |  | 2 |
| 421 | nvnMemoryPoolMap | 0x5E2740 | p: p | p: p | same |  | 1 |
| 422 | nvnMemoryPoolMapVirtual | 0x5E2790 | i: pip | i: pip | same |  | unused |
| 423 | nvnMemoryPoolSetDebugLabel | 0x5D63E0 | v: pp | v: pp | same |  | unused |
| 424 | nvnMultisampleStateGetAlphaToCoverageDither | 0x5EC950 | i: p | i: p | same |  | unused |
| 425 | nvnMultisampleStateGetAlphaToCoverageEnable | 0x5EC940 | i: p | i: p | same |  | unused |
| 426 | nvnMultisampleStateGetCoverageModulationMode | 0x5EC9B0 | i: p | i: p | same |  | unused |
| 427 | nvnMultisampleStateGetCoverageToColorEnable | 0x5ECA10 | i: p | i: p | same |  | unused |
| 428 | nvnMultisampleStateGetCoverageToColorOutput | 0x5ECA40 | i: p | i: p | same |  | unused |
| 429 | nvnMultisampleStateGetCoverageTruncationEnable | 0x5EC9E0 | i: p (hexrays) |  | new | nvn2 | unused |
| 430 | nvnMultisampleStateGetMultisampleEnable | 0x5EC920 | i: p | i: p | same |  | unused |
| 431 | nvnMultisampleStateGetRasterSamples | 0x5EC980 | i: p | i: p | same |  | unused |
| 432 | nvnMultisampleStateGetSampleLocations | 0x5ECEE0 | l: piilddii (hexrays-unreliable) |  | new | not-in-nx-table | unused |
| 433 | nvnMultisampleStateGetSampleLocationsEnable | 0x5ECA70 | i: p | i: p | same |  | unused |
| 434 | nvnMultisampleStateGetSampleLocationsGrid | 0x5ECA80 | v: ppp | v: ppp | same |  | unused |
| 435 | nvnMultisampleStateGetSampleLocationsGridEnable | 0x5ECB10 | i: p | i: p | same |  | unused |
| 436 | nvnMultisampleStateGetSamples | 0x5EC930 | i: p | i: p | same |  | unused |
| 437 | nvnMultisampleStateSetAlphaToCoverageDither | 0x5EC8E0 | v: pi | v: pi | same |  | unused |
| 438 | nvnMultisampleStateSetAlphaToCoverageEnable | 0x5EC8C0 | v: pi | v: pi | same |  | 1 |
| 439 | nvnMultisampleStateSetCoverageModulationMode | 0x5EC990 | v: pi | v: pi | same |  | unused |
| 440 | nvnMultisampleStateSetCoverageToColorEnable | 0x5EC9F0 | v: pi | v: pi | same |  | unused |
| 441 | nvnMultisampleStateSetCoverageToColorOutput | 0x5ECA20 | v: pi | v: pi | same |  | unused |
| 442 | nvnMultisampleStateSetCoverageTruncationEnable | 0x5EC9C0 | v: pi (hexrays) |  | new | nvn2 | unused |
| 443 | nvnMultisampleStateSetDefaults | 0x5EC880 | v: p | v: p | same |  | 1 |
| 444 | nvnMultisampleStateSetMultisampleEnable | 0x5EC8A0 | v: pi | v: pi | same |  | 1 |
| 445 | nvnMultisampleStateSetRasterSamples | 0x5EC960 | v: pi | v: pi | same |  | unused |
| 446 | nvnMultisampleStateSetSampleLocations | 0x5ECB20 | v: piip | v: piip | same |  | unused |
| 447 | nvnMultisampleStateSetSampleLocationsEnable | 0x5ECA50 | v: pi | v: pi | same |  | unused |
| 448 | nvnMultisampleStateSetSampleLocationsGridEnable | 0x5ECAF0 | v: pi | v: pi | same |  | unused |
| 449 | nvnMultisampleStateSetSamples | 0x5EC900 | v: pi | v: pi | same |  | 1 |
| 450 | nvnPolygonStateGetCullFace | 0x5EC840 | i: p | i: p | same |  | unused |
| 451 | nvnPolygonStateGetFrontFace | 0x5EC850 | i: p | i: p | same |  | unused |
| 452 | nvnPolygonStateGetPolygonMode | 0x5EC860 | i: p | i: p | same |  | unused |
| 453 | nvnPolygonStateGetPolygonOffsetEnables | 0x5EC870 | i: p | i: p | same |  | unused |
| 454 | nvnPolygonStateSetCullFace | 0x5EC7C0 | v: pi | v: pi | same |  | 1 |
| 455 | nvnPolygonStateSetDefaults | 0x5EC7B0 | v: p | v: p | same |  | 1 |
| 456 | nvnPolygonStateSetFrontFace | 0x5EC7E0 | v: pi | v: pi | same |  | unused |
| 457 | nvnPolygonStateSetPolygonMode | 0x5EC800 | v: pi | v: pi | same |  | 1 |
| 458 | nvnPolygonStateSetPolygonOffsetEnables | 0x5EC820 | v: pi | v: pi | same |  | 1 |
| 459 | nvnProgramBuilderGetDebugDataHashes | 0x5E5E60 | l: p (hexrays) |  | new | nvn2 | unused |
| 460 | nvnProgramBuilderGetDevice | 0x5E5E20 | l: p (hexrays) |  | new | nvn2 | unused |
| 461 | nvnProgramBuilderGetMaxCallableRecursionDepth | 0x5F79F0 | l: p (hexrays) |  | new | nvn2 | unused |
| 462 | nvnProgramBuilderGetMaxTraceRecursionDepth | 0x5F79D0 | l: p (hexrays) |  | new | nvn2 | unused |
| 463 | nvnProgramBuilderGetProgramRayTracingFlags | 0x5F7A10 | l: p (hexrays) |  | new | nvn2 | unused |
| 464 | nvnProgramBuilderGetRayTracingModuleCount | 0x5E5ED0 | l: p (hexrays) |  | new | nvn2 | unused |
| 465 | nvnProgramBuilderGetRayTracingModuleData | 0x5E5EE0 | l: p (hexrays) |  | new | nvn2 | unused |
| 466 | nvnProgramBuilderGetSampleShading | 0x5E5E40 | l: p (hexrays) |  | new | nvn2 | unused |
| 467 | nvnProgramBuilderGetShaderCount | 0x5E5EB0 | l: p (hexrays) |  | new | nvn2 | unused |
| 468 | nvnProgramBuilderGetShaders | 0x5E5EA0 | l: p (hexrays) |  | new | nvn2 | unused |
| 469 | nvnProgramBuilderGetSubroutineLinkage | 0x5E5F00 | l: p (hexrays) |  | new | nvn2 | unused |
| 470 | nvnProgramBuilderGetSubroutineLinkageCount | 0x5E5F10 | l: p (hexrays) |  | new | nvn2 | unused |
| 471 | nvnProgramBuilderSetDebugDataHashes | 0x5E5E50 | v: pil (hexrays) |  | new | nvn2 | unused |
| 472 | nvnProgramBuilderSetDefaults | 0x5E5DE0 | v: p (hexrays) |  | new | nvn2 | unused |
| 473 | nvnProgramBuilderSetDevice | 0x5E5E10 | v: pl (hexrays) |  | new | nvn2 | unused |
| 474 | nvnProgramBuilderSetMaxCallableRecursionDepth | 0x5F7A00 | v: pi (hexrays) |  | new | nvn2 | unused |
| 475 | nvnProgramBuilderSetMaxTraceRecursionDepth | 0x5F79E0 | v: pi (hexrays) |  | new | nvn2 | unused |
| 476 | nvnProgramBuilderSetProgramRayTracingFlags | 0x5F7A20 | v: pi (hexrays) |  | new | nvn2 | unused |
| 477 | nvnProgramBuilderSetRayTracingModules | 0x5E5EC0 | v: pil (hexrays) |  | new | nvn2 | unused |
| 478 | nvnProgramBuilderSetSampleShading | 0x5E5E30 | v: pi (hexrays) |  | new | nvn2 | unused |
| 479 | nvnProgramBuilderSetShaders | 0x5E5E90 | v: pil (hexrays) |  | new | nvn2 | unused |
| 480 | nvnProgramBuilderSetSubroutineLinkage | 0x5E5EF0 | v: pil (hexrays) |  | new | nvn2 | unused |
| 481 | nvnProgramFinalize | 0x5E4B10 | v: p | v: p | same |  | 1 |
| 482 | nvnProgramGetDeviceComputeMemorySize | 0x5E6D90 | l: p (hexrays) |  | new | nvn2 | unused |
| 483 | nvnProgramGetScratchBytesPerWarp | 0x5F6640 | l: p (hexrays) |  | new | nvn2 | unused |
| 484 | nvnProgramGetShaderGroupHandle | 0x5F6520 | l: plip (hexrays) |  | new | nvn2 | unused |
| 485 | nvnProgramGetSubtileSize | 0x5E5DD0 | l: p (hexrays) |  | new | nvn2 | unused |
| 486 | nvnProgramInitialize | 0x5E4A70 | i: pp | i: pp | same |  | 1 |
| 487 | nvnProgramInitializeFromBuilder | 0x5E6CB0 | i: pl (hexrays) |  | new | nvn2 | unused |
| 488 | nvnProgramSetDebugLabel | 0x5D63F0 | v: pp | v: pp | same |  | unused |
| 489 | nvnProgramSetSampleShading | 0x5E5DB0 | v: pi (hexrays) |  | new | nvn2 | unused |
| 490 | nvnProgramSetShaders | 0x5E4E90 | i: pip | i: pip | same |  | 1 |
| 491 | nvnProgramSetShadersExt | 0x5E58A0 | v: plp (hexrays) |  | new | nvn2 | unused |
| 492 | nvnProgramSetSubroutineLinkage | 0x5E59A0 | i: pip | i: pip | same |  | unused |
| 493 | nvnQueueBuilderGetCommandFlushThreshold | 0x5E8690 | l: pp (hexrays) |  | new | not-in-nx-table | unused |
| 494 | nvnQueueBuilderGetCommandMemorySize | 0x5E8650 | l: pp (hexrays) |  | new | not-in-nx-table | unused |
| 495 | nvnQueueBuilderGetComputeMemorySize | 0x5E8660 | l: pp (hexrays) |  | new | not-in-nx-table | unused |
| 496 | nvnQueueBuilderGetControlMemorySize | 0x5E8670 | l: pp (hexrays) |  | new | not-in-nx-table | unused |
| 497 | nvnQueueBuilderGetDevice | 0x5E8630 | l: p (hexrays) |  | new | not-in-nx-table | unused |
| 498 | nvnQueueBuilderGetFlags | 0x5E8640 | l: p (hexrays) |  | new | not-in-nx-table | unused |
| 499 | nvnQueueBuilderGetMemory | 0x5E86B0 | l: p (hexrays) |  | new | not-in-nx-table | unused |
| 500 | nvnQueueBuilderGetMemorySize | 0x5E86A0 | l: p (hexrays) |  | new | not-in-nx-table | unused |
| 501 | nvnQueueBuilderGetPrimaryQueue | 0x5E8710 | l: p (hexrays) |  | new | not-in-nx-table | unused |
| 502 | nvnQueueBuilderGetQueueMemorySize | 0x5E85A0 | l: p | l: p | same |  | unused |
| 503 | nvnQueueBuilderGetQueuePriority | 0x5E86F0 | l: p (hexrays) |  | new | nvn2 | unused |
| 504 | nvnQueueBuilderGetRayTracingMemorySize | 0x5E8680 | l: pp (hexrays) |  | new | nvn2 | unused |
| 505 | nvnQueueBuilderSetCommandFlushThreshold | 0x5E8360 | v: pl | v: pl | same |  | 1 |
| 506 | nvnQueueBuilderSetCommandMemorySize | 0x5E8320 | v: pl | v: pl | same |  | 1 |
| 507 | nvnQueueBuilderSetComputeMemorySize | 0x5E8330 | v: pl | v: pl | same |  | 1 |
| 508 | nvnQueueBuilderSetControlMemorySize | 0x5E8340 | v: pl | v: pl | same |  | 1 |
| 509 | nvnQueueBuilderSetDefaults | 0x5E82E0 | v: p | v: p | same |  | 1 |
| 510 | nvnQueueBuilderSetDevice | 0x5E82D0 | v: pp | v: pp | same |  | 1 |
| 511 | nvnQueueBuilderSetFlags | 0x5E8310 | v: pi | v: pi | same |  | 2 |
| 512 | nvnQueueBuilderSetPrimaryQueue | 0x5E8700 | v: pp (sdk-verified) |  | new | not-in-nx-table | 1 |
| 513 | nvnQueueBuilderSetQueueMemory | 0x5E8370 | v: ppl | v: ppl | same |  | unused |
| 514 | nvnQueueBuilderSetQueuePriority | 0x5E86C0 | v: pi (hexrays) |  | new | nvn2 | unused |
| 515 | nvnQueueBuilderSetRayTracingMemorySize | 0x5E8350 | v: pi (hexrays) |  | new | nvn2 | unused |
| 516 | nvnQueueFenceSync | 0x5ED770 | v: ppii | v: ppii | same |  | 2 |
| 517 | nvnQueueFinalize | 0x5E9AD0 | v: p | v: p | same |  | 1 |
| 518 | nvnQueueFinish | 0x5E9830 | v: p | v: p | same |  | 1 |
| 519 | nvnQueueFlush | 0x5EB4B0 | v: p | v: p | same |  | 4 |
| 520 | nvnQueueGetError | 0x5E9CC0 | i: pp | i: pp | same |  | unused |
| 521 | nvnQueueGetTotalCommandMemoryUsed | 0x5EB4C0 | l: p | l: p | same |  | unused |
| 522 | nvnQueueGetTotalComputeMemoryUsed | 0x5EB540 | l: p | l: p | same |  | unused |
| 523 | nvnQueueGetTotalControlMemoryUsed | 0x5EB500 | l: p | l: p | same |  | unused |
| 524 | nvnQueueGetTotalRayTracingMemoryUsed | 0x5EB550 | l: p (hexrays) |  | new | nvn2 | unused |
| 525 | nvnQueueInitialize | 0x5E98C0 | i: pp | i: pp | same |  | 1 |
| 526 | nvnQueuePresentTexture | 0x5F1990 | v: ppi | v: ppi | same |  | 1 |
| 527 | nvnQueueResetMemoryUsageCounts | 0x5EB560 | v: p | v: p | same |  | unused |
| 528 | nvnQueueSetDebugLabel | 0x5D6400 | v: pp | v: pp | same |  | unused |
| 529 | nvnQueueSubmitCommands | 0x5EB340 | v: pip | v: pip | same |  | 2 |
| 530 | nvnQueueWaitSync | 0x5EDCF0 | i: pp | i: pp | same |  | 2 |
| 531 | nvnSamplerBuilderGetBorderColor | 0x5EBC80 | v: pp | v: pp | same |  | unused |
| 532 | nvnSamplerBuilderGetBorderColori | 0x5EBCB0 | v: pp | v: pp | same |  | unused |
| 533 | nvnSamplerBuilderGetBorderColorui | 0x5EBCE0 | v: pp | v: pp | same |  | unused |
| 534 | nvnSamplerBuilderGetCompare | 0x5EBC60 | v: ppp | v: ppp | same |  | unused |
| 535 | nvnSamplerBuilderGetDevice | 0x5EBBD0 | l: p (hexrays) |  | new | not-in-nx-table | unused |
| 536 | nvnSamplerBuilderGetLodBias | 0x5EBC50 | f: p | f: p | same |  | unused |
| 537 | nvnSamplerBuilderGetLodClamp | 0x5EBC30 | v: ppp | v: ppp | same |  | unused |
| 538 | nvnSamplerBuilderGetLodSnap | 0x5EBD30 | f: p | f: p | same |  | unused |
| 539 | nvnSamplerBuilderGetMaxAnisotropy | 0x5EBD10 | f: p | f: p | same |  | unused |
| 540 | nvnSamplerBuilderGetMinMagFilter | 0x5EBBE0 | v: ppp | v: ppp | same |  | unused |
| 541 | nvnSamplerBuilderGetReductionFilter | 0x5EBD20 | i: p | i: p | same |  | unused |
| 542 | nvnSamplerBuilderGetWrapMode | 0x5EBC00 | v: pppp | v: pppp | same |  | unused |
| 543 | nvnSamplerBuilderSetBorderColor | 0x5EBB10 | v: pp | v: pp | same |  | 1 |
| 544 | nvnSamplerBuilderSetBorderColori | 0x5EBB40 | v: pp | v: pp | same |  | unused |
| 545 | nvnSamplerBuilderSetBorderColorui | 0x5EBB70 | v: pp | v: pp | same |  | unused |
| 546 | nvnSamplerBuilderSetCompare | 0x5EBAF0 | v: pii | v: pii | same |  | 1 |
| 547 | nvnSamplerBuilderSetDefaults | 0x5EBA60 | v: p | v: p | same |  | 1 |
| 548 | nvnSamplerBuilderSetDevice | 0x5EBA50 | v: pp | v: pp | same |  | 1 |
| 549 | nvnSamplerBuilderSetLodBias | 0x5EBAE0 | v: pf | v: pf | same |  | 1 |
| 550 | nvnSamplerBuilderSetLodClamp | 0x5EBAD0 | v: pff | v: pff | same |  | 1 |
| 551 | nvnSamplerBuilderSetLodSnap | 0x5EBBC0 | v: pf | v: pf | same |  | unused |
| 552 | nvnSamplerBuilderSetMaxAnisotropy | 0x5EBBA0 | v: pf | v: pf | same |  | 1 |
| 553 | nvnSamplerBuilderSetMinMagFilter | 0x5EBA90 | v: pii | v: pii | same |  | 1 |
| 554 | nvnSamplerBuilderSetReductionFilter | 0x5EBBB0 | v: pi | v: pi | same |  | 1 |
| 555 | nvnSamplerBuilderSetWrapMode | 0x5EBAB0 | v: piii | v: piii | same |  | 1 |
| 556 | nvnSamplerCompare | 0x5EC110 | i: pp | i: pp | same |  | unused |
| 557 | nvnSamplerFinalize | 0x5EBD70 | v: p | v: p | same |  | 1 |
| 558 | nvnSamplerGetBorderColor | 0x5EBE20 | v: pp | v: pp | same |  | unused |
| 559 | nvnSamplerGetBorderColori | 0x5EBE50 | v: pp | v: pp | same |  | unused |
| 560 | nvnSamplerGetBorderColorui | 0x5EBE80 | v: pp | v: pp | same |  | unused |
| 561 | nvnSamplerGetCompare | 0x5EBE00 | v: ppp | v: ppp | same |  | unused |
| 562 | nvnSamplerGetDebugID | 0x5EC1C0 | l: p | l: p | same |  | unused |
| 563 | nvnSamplerGetLodBias | 0x5EBDF0 | f: p | f: p | same |  | unused |
| 564 | nvnSamplerGetLodClamp | 0x5EBDD0 | v: ppp | v: ppp | same |  | unused |
| 565 | nvnSamplerGetMaxAnisotropy | 0x5EBEB0 | f: p | f: p | same |  | unused |
| 566 | nvnSamplerGetMinMagFilter | 0x5EBD80 | v: ppp | v: ppp | same |  | unused |
| 567 | nvnSamplerGetReductionFilter | 0x5EBEC0 | i: p | i: p | same |  | unused |
| 568 | nvnSamplerGetWrapMode | 0x5EBDA0 | v: pppp | v: pppp | same |  | unused |
| 569 | nvnSamplerInitialize | 0x5EBD40 | i: pp | i: pp | same |  | 1 |
| 570 | nvnSamplerPoolFinalize | 0x5EBFE0 | v: p | v: p | same |  | unused |
| 571 | nvnSamplerPoolGetMemoryOffset | 0x5EC0F0 | l: p | l: p | same |  | unused |
| 572 | nvnSamplerPoolGetMemoryPool | 0x5EC0E0 | p: p | p: p | same |  | unused |
| 573 | nvnSamplerPoolGetSize | 0x5EC100 | i: p | i: p | same |  | unused |
| 574 | nvnSamplerPoolInitialize | 0x5EBED0 | i: ppli | i: ppli | same |  | unused |
| 575 | nvnSamplerPoolRegisterSampler | 0x5EBFD0 | v: pip | v: pip | same |  | 1 |
| 576 | nvnSamplerPoolRegisterSamplerBuilder | 0x5EBFF0 | v: pip | v: pip | same |  | unused |
| 577 | nvnSamplerPoolSetDebugLabel | 0x5D6460 | v: pp | v: pp | same |  | unused |
| 578 | nvnSamplerSetDebugLabel | 0x5D6410 | v: pp | v: pp | same |  | unused |
| 579 | nvnSyncCreateGLSync | 0x5EDDE0 | l: pp (hexrays) |  | new | nvn2 | unused |
| 580 | nvnSyncFinalize | 0x5ED170 | v: p | v: p | same |  | 8 |
| 581 | nvnSyncInitialize | 0x5ED110 | i: pp | i: pp | same |  | 7 |
| 582 | nvnSyncInitializeFromFencedGLSync | 0x5EDD00 | l: plip (hexrays) |  | new | nvn2 | unused |
| 583 | nvnSyncSetDebugLabel | 0x5D6420 | v: pp | v: pp | same |  | unused |
| 584 | nvnSyncWait | 0x5ED780 | i: pl | i: pl | same |  | 2 |
| 585 | nvnTextureBuilderGetDepth | 0x5EE2F0 | i: p | i: p | same |  | unused |
| 586 | nvnTextureBuilderGetDepthStencilMode | 0x5EE360 | i: p | i: p | same |  | unused |
| 587 | nvnTextureBuilderGetDevice | 0x5EE2A0 | l: p (hexrays) |  | new | not-in-nx-table | unused |
| 588 | nvnTextureBuilderGetFlags | 0x5EE2B0 | i: p | i: p | same |  | unused |
| 589 | nvnTextureBuilderGetFormat | 0x5EE310 | i: p | i: p | same |  | unused |
| 590 | nvnTextureBuilderGetGLTextureName | 0x5EE980 | i: p | i: p | same |  | unused |
| 591 | nvnTextureBuilderGetHeight | 0x5EE2E0 | i: p | i: p | same |  | 1 |
| 592 | nvnTextureBuilderGetLevels | 0x5EE300 | i: p | i: p | same |  | unused |
| 593 | nvnTextureBuilderGetMemoryOffset | 0x5EE400 | l: p | l: p | same |  | unused |
| 594 | nvnTextureBuilderGetMemoryPool | 0x5EE3F0 | S: p | S: p | same |  | unused |
| 595 | nvnTextureBuilderGetPackagedTextureData | 0x5EE370 | p: p | p: p | same |  | unused |
| 596 | nvnTextureBuilderGetPackagedTextureLayout | 0x5EE380 | l: pp (hexrays) |  | new | not-in-nx-table | unused |
| 597 | nvnTextureBuilderGetRawStorageClass | 0x5EE9A0 | l: p (hexrays) |  | new | nvn2 | unused |
| 598 | nvnTextureBuilderGetSamples | 0x5EE320 | i: p | i: p | same |  | unused |
| 599 | nvnTextureBuilderGetSparseTileLayout | 0x5F05D0 | v: pp | v: pp | same |  | unused |
| 600 | nvnTextureBuilderGetStorageAlignment | 0x5EE410 | l: p | l: p | same |  | 4 |
| 601 | nvnTextureBuilderGetStorageClass | 0x5EE860 | i: p | i: p | same |  | unused |
| 602 | nvnTextureBuilderGetStorageSize | 0x5EE7D0 | l: p | l: p | same |  | 4 |
| 603 | nvnTextureBuilderGetStride | 0x5EE3E0 | l: p | l: p | same |  | unused |
| 604 | nvnTextureBuilderGetSwizzle | 0x5EE330 | v: ppppp | v: ppppp | same |  | unused |
| 605 | nvnTextureBuilderGetTarget | 0x5EE2C0 | i: p | i: p | same |  | unused |
| 606 | nvnTextureBuilderGetViewOffset | 0x5EE5A0 | l: pp (hexrays) |  | new | not-in-nx-table | unused |
| 607 | nvnTextureBuilderGetWidth | 0x5EE2D0 | i: p | i: p | same |  | 1 |
| 608 | nvnTextureBuilderGetZCullStorageSize | 0x5EFEE0 | l: p | l: p | same |  | unused |
| 609 | nvnTextureBuilderSetDefaults | 0x5EE160 | v: p | v: p | same |  | 4 |
| 610 | nvnTextureBuilderSetDepth | 0x5EE1E0 | v: pi | v: pi | same |  | 2 |
| 611 | nvnTextureBuilderSetDepthStencilMode | 0x5EE260 | v: pi | v: pi | same |  | unused |
| 612 | nvnTextureBuilderSetDevice | 0x5EE150 | v: pp | v: pp | same |  | 6 |
| 613 | nvnTextureBuilderSetFlags | 0x5EE1A0 | v: pi | v: pi | same |  | 2 |
| 614 | nvnTextureBuilderSetFormat | 0x5EE230 | v: pi | v: pi | same |  | 4 |
| 615 | nvnTextureBuilderSetGLTextureName | 0x5EE280 | v: pi | v: pi | same |  | unused |
| 616 | nvnTextureBuilderSetHeight | 0x5EE1D0 | v: pi | v: pi | same |  | 2 |
| 617 | nvnTextureBuilderSetLevels | 0x5EE220 | v: pi | v: pi | same |  | 2 |
| 618 | nvnTextureBuilderSetPackagedTextureData | 0x5EE990 | v: pp | v: pp | same |  | unused |
| 619 | nvnTextureBuilderSetPackagedTextureLayout | 0x5EEA00 | v: pp | v: pp | same |  | 2 |
| 620 | nvnTextureBuilderSetSamples | 0x5EE240 | v: pi | v: pi | same |  | 2 |
| 621 | nvnTextureBuilderSetSize1D | 0x5EE1F0 | v: pi | v: pi | same |  | 2 |
| 622 | nvnTextureBuilderSetSize2D | 0x5EE200 | v: pii | v: pii | same |  | unused |
| 623 | nvnTextureBuilderSetSize3D | 0x5EE210 | v: piii | v: piii | same |  | unused |
| 624 | nvnTextureBuilderSetStorage | 0x5EE270 | v: ppl | v: ppl | same |  | 4 |
| 625 | nvnTextureBuilderSetStride | 0x5EE290 | v: pl | v: pl | same |  | 2 |
| 626 | nvnTextureBuilderSetSwizzle | 0x5EE250 | v: piiii | v: piiii | same |  | unused |
| 627 | nvnTextureBuilderSetTarget | 0x5EE1B0 | v: pi | v: pi | same |  | 4 |
| 628 | nvnTextureBuilderSetWidth | 0x5EE1C0 | v: pi | v: pi | same |  | 2 |
| 629 | nvnTextureCompare | 0x5F0E80 | i: pp | i: pp | same |  | unused |
| 630 | nvnTextureFinalize | 0x5EFE70 | v: p | v: p | same |  | 1 |
| 631 | nvnTextureFlushTexels | 0x5F0D30 | v: ppp | v: ppp | same |  | 2 |
| 632 | nvnTextureGetDebugID | 0x5F1040 | l: p | l: p | same |  | unused |
| 633 | nvnTextureGetDepth | 0x5F00D0 | i: p | i: p | same |  | unused |
| 634 | nvnTextureGetDepthStencilMode | 0x5F0190 | i: p | i: p | same |  | unused |
| 635 | nvnTextureGetFlags | 0x5F0060 | i: p | i: p | same |  | 2 |
| 636 | nvnTextureGetFormat | 0x5F0100 | i: p | i: p | same |  | 3 |
| 637 | nvnTextureGetHeight | 0x5F00C0 | i: p | i: p | same |  | 1 |
| 638 | nvnTextureGetLevels | 0x5F00E0 | i: p | i: p | same |  | unused |
| 639 | nvnTextureGetMemoryOffset | 0x5F0E60 | l: p | l: p | same |  | unused |
| 640 | nvnTextureGetMemoryPool | 0x5F0E50 | S: p | S: p | same |  | unused |
| 641 | nvnTextureGetRawStorageClass | 0x5F1050 | l: p (hexrays) |  | new | nvn2 | unused |
| 642 | nvnTextureGetSamples | 0x5F0130 | i: p | i: p | same |  | unused |
| 643 | nvnTextureGetSparseTileLayout | 0x5F0680 | v: pp | v: pp | same |  | unused |
| 644 | nvnTextureGetStorageClass | 0x5EFFB0 | i: p | i: p | same |  | unused |
| 645 | nvnTextureGetStorageSize | 0x5F0E70 | i: p | i: p | same |  | 4 |
| 646 | nvnTextureGetStride | 0x5F01C0 | l: p | l: p | same |  | unused |
| 647 | nvnTextureGetSwizzle | 0x5F0150 | v: ppppp | v: ppppp | same |  | unused |
| 648 | nvnTextureGetTarget | 0x5F0080 | i: p | i: p | same |  | 6 |
| 649 | nvnTextureGetTextureAddress | 0x5F01D0 | l: p | l: p | same |  | unused |
| 650 | nvnTextureGetViewOffset | 0x5F0020 | l: pp | l: pp | same |  | unused |
| 651 | nvnTextureGetWidth | 0x5F00B0 | i: p | i: p | same |  | 1 |
| 652 | nvnTextureGetZCullStorageSize | 0x5EFF30 | l: p | l: p | same |  | unused |
| 653 | nvnTextureInitialize | 0x5EF720 | i: pp | i: pp | same |  | 4 |
| 654 | nvnTextureInvalidateTexels | 0x5F0DC0 | v: ppp | v: ppp | same |  | 4 |
| 655 | nvnTexturePoolFinalize | 0x5F0270 | v: p | v: p | same |  | 1 |
| 656 | nvnTexturePoolGetMemoryOffset | 0x5F05B0 | l: p | l: p | same |  | unused |
| 657 | nvnTexturePoolGetMemoryPool | 0x5F05A0 | p: p | p: p | same |  | unused |
| 658 | nvnTexturePoolGetSize | 0x5F05C0 | i: p | i: p | same |  | unused |
| 659 | nvnTexturePoolInitialize | 0x5F01E0 | i: ppli | i: ppli | same |  | unused |
| 660 | nvnTexturePoolRegisterImage | 0x5F0410 | v: pipp | v: pipp | same |  | 1 |
| 661 | nvnTexturePoolRegisterTexture | 0x5F0280 | v: pipp | v: pipp | same |  | 1 |
| 662 | nvnTexturePoolSetDebugLabel | 0x5D6450 | v: pp | v: pp | same |  | unused |
| 663 | nvnTextureReadTexels | 0x5F0D20 | v: pppp | v: pppp | same |  | 3 |
| 664 | nvnTextureReadTexelsStrided | 0x5F0A60 | v: ppppll | v: ppppll | same |  | unused |
| 665 | nvnTextureSetDebugLabel | 0x5D6430 | v: pp | v: pp | same |  | unused |
| 666 | nvnTextureViewCompare | 0x5EEBF0 | i: pp | i: pp | same |  | unused |
| 667 | nvnTextureViewGetDepthStencilMode | 0x5EEBB0 | i: pp | i: pp | same |  | unused |
| 668 | nvnTextureViewGetFormat | 0x5EEB60 | i: pp | i: pp | same |  | unused |
| 669 | nvnTextureViewGetLayers | 0x5EEB40 | i: ppp | i: ppp | same |  | unused |
| 670 | nvnTextureViewGetLevels | 0x5EEB20 | i: ppp | i: ppp | same |  | unused |
| 671 | nvnTextureViewGetSwizzle | 0x5EEB80 | i: ppppp | i: ppppp | same |  | unused |
| 672 | nvnTextureViewGetTarget | 0x5EEBD0 | i: pp | i: pp | same |  | unused |
| 673 | nvnTextureViewSetDefaults | 0x5EEA60 | v: p | v: p | same |  | 21 |
| 674 | nvnTextureViewSetDepthStencilMode | 0x5EEB00 | v: pi | v: pi | same |  | 1 |
| 675 | nvnTextureViewSetFormat | 0x5EEAC0 | v: pi | v: pi | same |  | 6 |
| 676 | nvnTextureViewSetLayers | 0x5EEAA0 | v: pii | v: pii | same |  | 5 |
| 677 | nvnTextureViewSetLevels | 0x5EEA70 | v: pii | v: pii | same |  | 20 |
| 678 | nvnTextureViewSetSwizzle | 0x5EEAE0 | v: piiii | v: piiii | same |  | 1 |
| 679 | nvnTextureViewSetTarget | 0x5EE7B0 | v: pi | v: pi | same |  | 4 |
| 680 | nvnTextureWriteTexels | 0x5F0A40 | v: pppp | v: pppp | same |  | 6 |
| 681 | nvnTextureWriteTexelsStrided | 0x5F0780 | v: ppppll | v: ppppll | same |  | unused |
| 682 | nvnVertexAttribStateGetFormat | 0x5EC550 | v: ppp | v: ppp | same |  | unused |
| 683 | nvnVertexAttribStateGetStreamIndex | 0x5EC570 | i: p | i: p | same |  | unused |
| 684 | nvnVertexAttribStateSetDefaults | 0x5EC500 | v: p | v: p | same |  | 32 |
| 685 | nvnVertexAttribStateSetFormat | 0x5EC510 | v: pil | v: pil | same |  | 1 |
| 686 | nvnVertexAttribStateSetStreamIndex | 0x5EC530 | v: pi | v: pi | same |  | unused |
| 687 | nvnVertexStreamStateGetDivisor | 0x5EC5C0 | i: p | i: p | same |  | unused |
| 688 | nvnVertexStreamStateGetStride | 0x5EC5B0 | l: p | l: p | same |  | unused |
| 689 | nvnVertexStreamStateSetDefaults | 0x5EC580 | v: p | v: p | same |  | 16 |
| 690 | nvnVertexStreamStateSetDivisor | 0x5EC5A0 | v: pi | v: pi | same |  | 16 |
| 691 | nvnVertexStreamStateSetStride | 0x5EC590 | v: pl | v: pl | same |  | 16 |
| 692 | nvnWindowAcquireTexture | 0x5F1920 | i: ppp | i: ppp | same |  | 2 |
| 693 | nvnWindowBuilderGetColorSpace | 0x5F1320 | l: p (hexrays) |  | new | nvn2 | unused |
| 694 | nvnWindowBuilderGetDevice | 0x5F12B0 | l: p (hexrays) |  | new | not-in-nx-table | unused |
| 695 | nvnWindowBuilderGetHdrContentMetadata | 0x5F1390 | f: ppp (hexrays) |  | new | nvn2 | unused |
| 696 | nvnWindowBuilderGetHdrDisplayMetadata | 0x5F1330 | f: ppppppppppp (hexrays-unreliable) |  | new | nvn2 | unused |
| 697 | nvnWindowBuilderGetNativeWindow | 0x5F1310 | p: p | p: p | same |  | unused |
| 698 | nvnWindowBuilderGetNumActiveTextures | 0x5F1300 | l: p (hexrays) |  | new | nvn2 | unused |
| 699 | nvnWindowBuilderGetNumTextures | 0x5F12C0 | l: p (hexrays) |  | new | not-in-nx-table | unused |
| 700 | nvnWindowBuilderGetPresentInterval | 0x5F12F0 | i: p | i: p | same |  | unused |
| 701 | nvnWindowBuilderGetTexture | 0x5F12D0 | l: pi (hexrays) |  | new | not-in-nx-table | unused |
| 702 | nvnWindowBuilderSetColorSpace | 0x5F1250 | v: pi (hexrays) |  | new | nvn2 | unused |
| 703 | nvnWindowBuilderSetDefaults | 0x5F11E0 | v: p | v: p | same |  | 1 |
| 704 | nvnWindowBuilderSetDevice | 0x5F11D0 | v: pp | v: pp | same |  | 1 |
| 705 | nvnWindowBuilderSetHdrContentMetadata | 0x5F12A0 | v: pff (hexrays) |  | new | nvn2 | unused |
| 706 | nvnWindowBuilderSetHdrDisplayMetadata | 0x5F1260 | v: pfffffffflllllllif (hexrays-unreliable) |  | new | nvn2 | unused |
| 707 | nvnWindowBuilderSetNativeWindow | 0x5F1210 | v: pp | v: pp | same |  | 1 |
| 708 | nvnWindowBuilderSetNumActiveTextures | 0x5F1240 | v: pi (hexrays) |  | new | nvn2 | unused |
| 709 | nvnWindowBuilderSetPresentInterval | 0x5F1230 | v: pi | v: pi | same |  | 1 |
| 710 | nvnWindowBuilderSetTextures | 0x5F1220 | v: pip | v: pip | same |  | 1 |
| 711 | nvnWindowFinalize | 0x5F15E0 | v: p | v: p | same |  | 1 |
| 712 | nvnWindowGetCrop | 0x5F17D0 | v: pp | v: pp | same |  | unused |
| 713 | nvnWindowGetHdrContentMetadata | 0x5F1780 | f: ppp (hexrays) |  | new | nvn2 | unused |
| 714 | nvnWindowGetHdrDisplayMetadata | 0x5F1720 | f: ppppppppppp (hexrays-unreliable) |  | new | nvn2 | unused |
| 715 | nvnWindowGetNativeWindow | 0x5F1670 | p: p | p: p | same |  | unused |
| 716 | nvnWindowGetNumActiveTextures | 0x5F1810 | l: p (hexrays) |  | new | nvn2 | unused |
| 717 | nvnWindowGetNumTextures | 0x5F1800 | l: p (hexrays) |  | new | not-in-nx-table | unused |
| 718 | nvnWindowGetPresentInterval | 0x5F1680 | i: p | i: p | same |  | 1 |
| 719 | nvnWindowInitialize | 0x5F13C0 | i: pp | i: pp | same |  | 1 |
| 720 | nvnWindowSetCrop | 0x5F17A0 | v: piiii | v: piiii | same |  | 2 |
| 721 | nvnWindowSetDebugLabel | 0x5D6440 | v: pp | v: pp | same |  | unused |
| 722 | nvnWindowSetHdrContentMetadata | 0x5F16F0 | v: pff (hexrays) |  | new | nvn2 | unused |
| 723 | nvnWindowSetHdrDisplayMetadata | 0x5F1690 | v: pfffffffflllllllii (hexrays-unreliable) |  | new | nvn2 | unused |
| 724 | nvnWindowSetNumActiveTextures | 0x5F1560 | v: pi (hexrays) |  | new | nvn2 | unused |
| 725 | nvnWindowSetPresentInterval | 0x5F1570 | v: pi | v: pi | same |  | 1 |
