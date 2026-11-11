// What Ryujinx's shader translator asks of "the GPU", answered from the shader file alone.
//
// In the emulator these queries read live Maxwell state (bound constant buffers, the texture pool, the 3D engine
// registers). Offline we only have the code block and its NVN control section, so every answer is either read from
// those or a fixed default. The defaults that stand in for draw-time state are the ones our runtime will have to key
// a variant cache on; docs/nvn/TRANSLATOR.md lists them.

using Ryujinx.Graphics.Shader;
using Ryujinx.Graphics.Shader.CodeGen;
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

namespace SpirvHarness
{
    sealed class StaticGpuAccessor : IGpuAccessor
    {
        // Vulkan descriptor layout, identical to Ryujinx's Vulkan backend (PipelineBase / Constants.cs there):
        // one set per resource kind, and a fixed binding range per stage inside each set.
        public const int UniformSet = 0, StorageSet = 1, TextureSet = 2, ImageSet = 3;
        public const int MaxUniformBuffersPerStage = 18;   // c[0]..c[17]
        public const int MaxStorageBuffersPerStage = 16;
        public const int MaxTexturesPerStage = 64;
        public const int MaxImagesPerStage = 16;
        const int ShaderStages = 5;                         // vertex, tess ctrl, tess eval, geometry, fragment
        const int ReservedUniformBuffers = 1;               // binding 0 of each stage range: the translator's support buffer

        readonly byte[] _code;        // the whole code block (0x30-byte NVN header, SPH, instructions, constant data)
        readonly int _constOff, _constSize;
        readonly int _stageSlot;      // 0 vertex/compute, 1 fragment (Ryujinx's GetStageIndex remap)
        readonly int _localX, _localY, _localZ, _sharedSize;
        int _staticTextures, _staticImages, _arrayTextures, _arrayImages;
        // Tessellation evaluation input layout (domain, spacing, winding) from the NVN2 control section; see TessFromNvn2Control.
        readonly TessPatchType _tessPatchType = TessPatchType.Triangles;
        readonly TessSpacing _tessSpacing = TessSpacing.EqualSpacing;
        readonly bool _tessCw;

        public readonly List<string> Messages = new();
        // Draw-time queries the translator made for this shader (what a runtime variant would have to key on).
        public readonly SortedSet<string> DrawTimeQueries = new();

        /// `stageSlot`: Ryujinx's binding slot when it is not the stage's own (task 0, mesh 4: AmpereMesh.cs); -1 = the stage's.
        public StaticGpuAccessor(byte[] code, int constOff, int constSize, ShaderStage stage, byte[] ctrl, int stageSlot = -1)
        {
            // Pad so that the decoder may read past the program end (it asks for minimum sizes) without bounds errors.
            _code = new byte[(code.Length + 0x1000 + 7) & ~7];
            code.CopyTo(_code, 0);
            _constOff = constOff;
            _constSize = constSize;
            // Ryujinx's Vulkan stage slots: vertex/compute 0, fragment 1, geometry 2, tessellation control 3, evaluation 4.
            _stageSlot = stage switch
            {
                ShaderStage.Fragment => 1,
                ShaderStage.Geometry => 2,
                ShaderStage.TessellationControl => 3,
                ShaderStage.TessellationEvaluation => 4,
                _ => 0,
            };
            if (stageSlot >= 0) _stageSlot = stageSlot;
            // Compute work-group size: control section +0x748..+0x750 (verified against BNSH reflection, SHADERS.md).
            _localX = _localY = _localZ = 1;
            if (Program.IsNvn2(ctrl))
            {
                // NVN2 (platform 2) control section, docs/AMPERE.md section 3: work group +0x58..+0x60, shared memory +0x64 (+0x50 is the local
                // memory size, non-zero exactly when the SASS has LDL/STL; docs/nvn2_layout_xcheck.md).
                if (stage == ShaderStage.Compute)
                {
                    _localX = Math.Max(1, BitConverter.ToInt32(ctrl, 0x58));
                    _localY = Math.Max(1, BitConverter.ToInt32(ctrl, 0x5c));
                    _localZ = Math.Max(1, BitConverter.ToInt32(ctrl, 0x60));
                    _sharedSize = BitConverter.ToInt32(ctrl, 0x64);
                }
                if (stage == ShaderStage.TessellationEvaluation)
                    (_tessPatchType, _tessSpacing, _tessCw) = TessFromNvn2Control(ctrl, Messages);
            }
            else if (ctrl != null && ctrl.Length >= 0x754)
            {
                _localX = Math.Max(1, BitConverter.ToInt32(ctrl, 0x748));
                _localY = Math.Max(1, BitConverter.ToInt32(ctrl, 0x74c));
                _localZ = Math.Max(1, BitConverter.ToInt32(ctrl, 0x750));
            }
            // Shared memory bytes: control +0x754 (0 exactly for the compute shaders without shared memory). Without
            // it the translator declares its default, 0xc000, past the 32 KB Metal allows (docs/nvn/DEBUG.md #23).
            if (!Program.IsNvn2(ctrl) && ctrl != null && ctrl.Length >= 0x758) _sharedSize = BitConverter.ToInt32(ctrl, 0x754);
            // Task/mesh (stageSlot set): no shared memory of their own (no LDS/STS in the game's blocks); their ISBE regions are separate
            // definitions (AmpereMesh.cs). 4 bytes keeps Ryujinx's default 4 KiB array out of the mesh stage's 28 KiB.
            if (stageSlot >= 0) _sharedSize = 4;
        }

        // NVN2 tessellation evaluation blocks keep their GLSL input layout in the control section as GL enums: +0x58 domain
        // (GL_TRIANGLES 4, GL_QUADS 7, GL_ISOLINES 0x8e7a), +0x5c spacing (GL_EQUAL 0x202, GL_FRACTIONAL_ODD 0x8e7b,
        // GL_FRACTIONAL_EVEN 0x8e7c), +0x64 winding (GL_CW 0x900, GL_CCW 0x901). All 527 tes blocks of the corpus have
        // 4 / 0x8e7b / 0x901 = layout(triangles, fractional_odd_spacing, ccw); their SASS confirms the triangle domain (reads
        // gl_TessCoord.xy, computes z = 1 - x - y). (Geometry blocks keep their input primitive at +0x58 the same way.)
        // Ryujinx gets the same three values from the Maxwell register SetTessellationParameters, which the driver fills from
        // these. The winding goes in as GL states it: Ryujinx's SPIR-V back end inverts it for Vulkan's upper-left domain origin.
        static (TessPatchType, TessSpacing, bool) TessFromNvn2Control(byte[] ctrl, List<string> messages)
        {
            uint prim = BitConverter.ToUInt32(ctrl, 0x58), spacing = BitConverter.ToUInt32(ctrl, 0x5c), order = BitConverter.ToUInt32(ctrl, 0x64);
            TessPatchType pt = prim switch
            {
                0x0007 => TessPatchType.Quads,
                0x8e7a => TessPatchType.Isolines,
                _ => TessPatchType.Triangles,
            };
            TessSpacing sp = spacing switch
            {
                0x8e7b => TessSpacing.FractionalOddSpacing,
                0x8e7c => TessSpacing.FractionalEventSpacing,
                _ => TessSpacing.EqualSpacing,
            };
            if (prim is not (0x4 or 0x7 or 0x8e7a) || spacing is not (0x202 or 0x8e7b or 0x8e7c) || order is not (0x900 or 0x901))
                lock (messages) messages.Add($"tessellation layout not recognised: control +0x58 0x{prim:x} +0x5c 0x{spacing:x} +0x64 0x{order:x}");
            return (pt, sp, order == 0x900);
        }

        /// The c[0] word offsets of every storage descriptor an NVN2 block's stage can have bound: the candidates of the global memory fallback
        /// (tools/shaders/ampere/frontend/AmpereGlobal.cs). Graphics: 16 slots at 0x110 + 0x100 * hardware stage (vertex 0, tessellation control 1,
        /// evaluation 2, geometry 3, fragment 4), task 0x650 and mesh 0x770 (measured, nvn_replay.cpp storage_slot_for); compute: the storage entries
        /// of the program's c[0] table (header, push area, uniform block entries, then storage entries; as apply_nvn2_compute_table). Null: none.
        public static int[] GlobalFallbackWords(byte[] ctrl)
        {
            if (!Program.IsNvn2(ctrl) || ctrl.Length < 0xbc) return null;
            int stage = BitConverter.ToUInt16(ctrl, 0x56);
            int baseByte, count = 16;
            switch (stage)
            {
                case 0: baseByte = 0x110; break;
                case 1: baseByte = 0x510; break;
                case 2: baseByte = 0x410; break;
                case 3: baseByte = 0x210; break;
                case 4: baseByte = 0x310; break;
                case 6: baseByte = 0x650; break;
                case 7: baseByte = 0x770; break;
                case 5:
                    {
                        int ubos = ctrl[0xb1], ssbos = ctrl[0xb2], textures = ctrl[0xb3], images = ctrl[0xb6];
                        int table = ctrl[0xba] | ctrl[0xbb] << 8;
                        int push = table - (0x10 + 0x10 * (ubos + ssbos) + 8 * (textures + images));
                        if (push < 0 || push % 0x10 != 0 || push > 0x100) push = 0;
                        if (ssbos == 0) return null;
                        baseByte = 0x10 + push + 0x10 * ubos;
                        count = ssbos;
                        break;
                    }
                default: return null;
            }
            int[] words = new int[count];
            for (int k = 0; k < count; k++) words[k] = (baseByte + 0x10 * k) / 4;
            return words;
        }

        public void Log(string message)
        {
            // "handle-expr n rpn" from the bindless patch (translator/ampere-bindless-handle-expr.patch): goes to the resource list
            if (message.StartsWith("handle-expr ", StringComparison.Ordinal))
            {
                string[] f = message.Split(' ', 3);
                lock (HandleExprs) HandleExprs[int.Parse(f[1])] = f[2];
                return;
            }
            lock (Messages) Messages.Add(message);
        }

        /// The handle expressions of textures bound with constant buffer slot HandleExprSlot (0x20000), by handle number.
        public readonly Dictionary<int, string> HandleExprs = new();

        // Pool-sized texture and sampler arrays (A7): a handle the draw does not determine (per-pixel handles, e.g. the clustered-light
        // shadow maps: TEX.SCR.B.DC with the light index from a storage loop) becomes Ryujinx's GenerateBindlessAccess: texture id =
        // handle & 0xfffff into a sampled-image array, sampler id = handle >> 20 into a sampler array. It runs only when
        // TryConvertBindless (and the handle expressions of ampere-bindless-handle-expr.patch) found no per-draw binding, so every block
        // whose handle resolves per draw keeps its plain binding. Each array gets an extra descriptor set (CreateExtraSet) that the runtime
        // binds to its persistent pool set for that array kind (nvn_pool_sets.cpp). Off (NXRT_HARNESS_POOL_BINDLESS=0 / --no-pool-bindless):
        // the sample is left out as before (it reads 0, a depth comparison 1).
        public static bool PoolBindless = Environment.GetEnvironmentVariable("NXRT_HARNESS_POOL_BINDLESS") != "0";
        public bool QueryHostSupportsSeparateSampler() => PoolBindless;

        // Extra sets for the pool arrays: 4 + 3 * stage slot + n (at most three arrays per stage: the sampler array and two texture
        // kinds), so the stages of one program never share a set number. A fourth array goes into the texture set as before.
        public const int PoolSetBase = 4, PoolSetsPerStage = 3;
        int _extraSets;
        public int CreateExtraSet()
        {
            if (!PoolBindless || _extraSets >= PoolSetsPerStage) return -1;
            return PoolSetBase + PoolSetsPerStage * _stageSlot + _extraSets++;
        }

        // "GPU addresses" are offsets in the code block: the harness passes the program start (0x30 or 0x100).
        public ReadOnlySpan<ulong> GetCode(ulong address, int minimumSize)
        {
            return MemoryMarshal.Cast<byte, ulong>(new ReadOnlySpan<byte>(_code, (int)address, _code.Length - (int)address));
        }

        // Hypothesis tested by this harness: the constant data the compiler appended to the block (control +0x700)
        // is what NVN binds as c[1]. The translator reads c[1] to follow indirect branch (BRX) jump tables.
        public uint ConstantBuffer1Read(int offset)
        {
            return offset >= 0 && offset + 4 <= _constSize ? BitConverter.ToUInt32(_code, _constOff + offset) : 0;
        }

        public SetBindingPair CreateConstantBufferBinding(int index)
        {
            return new(UniformSet, _stageSlot * MaxUniformBuffersPerStage + index + ReservedUniformBuffers);
        }

        public SetBindingPair CreateStorageBufferBinding(int index)
        {
            return new(StorageSet, _stageSlot * MaxStorageBuffersPerStage + index);
        }

        // Single textures: per stage, 2 x max slots (plain textures first, buffer textures in the upper half).
        // Arrays (bindless turned into an array of textures): after every stage's range, counted from there.
        public SetBindingPair CreateTextureBinding(int count, bool isBuffer)
        {
            int binding = count == 1
                ? _stageSlot * MaxTexturesPerStage * 2 + (_staticTextures++) + (isBuffer ? MaxTexturesPerStage : 0)
                : MaxTexturesPerStage * ShaderStages * 2 + _arrayTextures++;
            return new(TextureSet, binding);
        }

        public SetBindingPair CreateImageBinding(int count, bool isBuffer)
        {
            int binding = count == 1
                ? _stageSlot * MaxImagesPerStage * 2 + (_staticImages++) + (isBuffer ? MaxImagesPerStage : 0)
                : MaxImagesPerStage * ShaderStages * 2 + _arrayImages++;
            return new(ImageSet, binding);
        }

        // Per-texture state that lives in the texture/sampler descriptors at draw time. Defaults: 2D, normalized
        // coordinates, RGBA8 (the last one only matters for typed image access).
        public SamplerType QuerySamplerType(int handle, int cbufSlot = -1)
        {
            DrawTimeQueries.Add("samplerType");
            return BufferTextures.Contains((cbufSlot, handle)) ? SamplerType.TextureBuffer : SamplerType.Texture2D;
        }

        // Texture handles (constant bank slot as the translator passes it, handle word) that the GLSLC reflection declares as buffer
        // samplers (samplerBuffer / isamplerBuffer / usamplerBuffer): their 1D fetches become texelFetch on a texel buffer.
        public readonly HashSet<(int Slot, int Handle)> BufferTextures = new();

        public bool QueryTextureCoordNormalized(int handle, int cbufSlot = -1)
        {
            DrawTimeQueries.Add("coordNormalized");
            return true;
        }

        public TextureFormat QueryTextureFormat(int handle, int cbufSlot = -1)
        {
            DrawTimeQueries.Add("textureFormat");
            return TextureFormat.R8G8B8A8Unorm;
        }

        public bool QueryHasUnalignedStorageBuffer()
        {
            DrawTimeQueries.Add("unalignedStorage");
            return false;
        }

        // NVN keeps the draw's base vertex, base instance and draw index in c[0] at 0x640/0x644/0x648 and NVN2 shaders read them (the game's
        // UI glyph pass picks its glyph records with instance + c[0][0x644]); the runtime does not write them, so Ryujinx's
        // DrawParametersReplace reads the Vulkan built-ins (gl_BaseVertex, gl_BaseInstance, gl_DrawID) instead.
        public bool QueryHasConstantBufferDrawParameters()
        {
            DrawTimeQueries.Add("drawParameters");
            return true;
        }

        public int QueryComputeLocalSizeX() => _localX;
        public int QueryComputeLocalSizeY() => _localY;
        public int QueryComputeLocalSizeZ() => _localZ;
        public int QueryComputeSharedMemorySize() => _sharedSize;  // 0: the translator uses 4 KB (never happens here)

        // Sizes of bindless arrays: pool sizes are draw-time state; a fixed bound makes the SPIR-V valid.
        // With pool arrays: the sampler id has 12 bits (4096), the texture array covers 65536 ids (the game's pool: see nvn_pool_sets.cpp; ids past
        // the end are clamped by the translator, the runtime warns when a pool is larger).
        public const int PoolTextureArrayLength = 65536, PoolSamplerArrayLength = 4096;
        public int QuerySamplerArrayLengthFromPool() { DrawTimeQueries.Add("poolSize"); return 4096; }
        public int QueryTextureArrayLengthFromPool() { DrawTimeQueries.Add("poolSize"); return PoolBindless ? PoolTextureArrayLength : 4096; }
        public int QueryTextureArrayLengthFromBuffer(int slot) { DrawTimeQueries.Add("cbufSize"); return 32; }

        // Default graphics state: triangles, no alpha test (emulated in the shader on Vulkan), every vertex attribute
        // and render target float, origin upper left (what Ryujinx passes on Vulkan). All of it is draw-time state.
        public GpuGraphicsState QueryGraphicsState()
        {
            return new GpuGraphicsState(
                earlyZForce: false,
                topology: InputTopology.Triangles,
                tessCw: _tessCw,
                tessPatchType: _tessPatchType,
                tessSpacing: _tessSpacing,
                alphaToCoverageEnable: false,
                alphaToCoverageDitherEnable: false,
                viewportTransformDisable: false,
                depthMode: false,
                programPointSizeEnable: false,
                pointSize: 1f,
                alphaTestCompare: AlphaTestOp.Always,
                alphaTestReference: 0f,
                attributeTypes: default,
                hasConstantBufferDrawParameters: false,
                fragmentOutputTypes: default,
                dualSourceBlendEnable: false,
                yNegateEnabled: false,
                originUpperLeft: true,
                halvePrimitiveId: false);
        }

        // Host = MoltenVK on the Apple M1 (docs/nvn/WONDER.md), with the choices Ryujinx's Vulkan backend makes.
        // DesktopHost (default, --host desktop): a desktop Vulkan 1.3 GPU with the choices Ryujinx's Vulkan backend
        // makes there (full precision, geometry shaders, float64). --host moltenvk reproduces Wonder's original answers.
        public static bool DesktopHost = true;
        public bool QueryHostReducedPrecision() => !DesktopHost;         // Ryujinx does this on MoltenVK
        public bool QueryHostSupportsGeometryShader() => DesktopHost;
        public bool QueryHostSupportsGeometryShaderPassthrough() => false;
        public bool QueryHostSupportsTransformFeedback() => false;
        public bool QueryHostSupportsShaderFloat64() => DesktopHost;
        public bool QueryHostSupportsShaderBallot() => false;
        public bool QueryHostSupportsViewportMask() => false;
        public bool QueryHostSupportsNonConstantTextureOffset() => false;
        public bool QueryHostSupportsTextureShadowLod() => false;
        public bool QueryHostSupportsFragmentShaderInterlock() => false;   // S4d: no SPV_EXT_fragment_shader_interlock dependency (only the rare ordered-fragment shaders lose their ordering)
        public bool QueryHostSupportsShaderNonUniformIndexing() => true;
        public int QueryHostSubgroupSize() => 32;
    }
}
