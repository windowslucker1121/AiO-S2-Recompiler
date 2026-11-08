// spirv_harness: run Ryujinx's Maxwell shader translator offline on blocks extracted by tools/maxwell/containers.py.
//
//   spirv_harness <code.bin> <ctrl.bin> [-o out.spv] [--glsl]
//       one stage: code block + NVN control section as written by containers.py --extract
//       (--glsl: Vulkan GLSL text instead of SPIR-V, to read what the translator made or compare backends)
//   spirv_harness --batch <extracted dir> --out <dir> [--jobs 2] [--limit N] [--no-val] [--no-retry] [--debug-mode] [--target-env vulkan1.3] [--host desktop|moltenvk]
//       every distinct code block named in any manifest.tsv under <extracted dir>; writes <hash>.spv, results.tsv
//       (one line per block) and summary.txt (counts, exceptions and translator messages grouped by text)
//
// Everything this program learns about the block comes from the control section (see docs/nvn/SHADERS.md):
// +0x708 program start (0x30 graphics, 0x100 compute), +0x6fc/+0x700 constant data size/offset. The stage comes
// from the SPH inside the code (graphics) or from the NVN header magic (0x12345679 = compute).

using Nx.Shaders.Ampere;
using Ryujinx.Graphics.Shader;
using Ryujinx.Graphics.Shader.CodeGen.Spirv;
using Ryujinx.Graphics.Shader.Translation;
using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using System.Threading.Tasks;

namespace SpirvHarness
{
    sealed record Job(string Hash, string Group, string CodePath, string CtrlPath, int Stages, string GlslcPath = null);

    sealed class Result
    {
        public string Hash, Group, Stage = "?", Status = "?", Error = "", ValError = "";
        public int Stages, SpvBytes;
        public double Ms;
        public List<string> Messages = new();
        public string Resources = "";
        public string Queries = "";
        public string Reflection = "";
        public List<string> Unsupported = new();
    }

    static partial class Program
    {
        const uint ComputeMagic = 0x12345679;
        static readonly TimeSpan Timeout = TimeSpan.FromSeconds(60);
        static bool NoRetry;   // --no-retry: report attempt 1 only (the translator as Ryujinx runs it)
        // --no-global-fallback or NXRT_HARNESS_NO_GLOBAL_FALLBACK=1: untraceable global memory accesses are dropped as before (A17, AmpereGlobal.cs)
        static bool NoGlobalFallback = Environment.GetEnvironmentVariable("NXRT_HARNESS_NO_GLOBAL_FALLBACK") == "1";
        static bool Debug;     // --debug-mode: DebugMode from attempt 1 (also reports undecodable opcodes)

        static readonly Dictionary<string, string> GlslcByHash = new();   // --reflect <corpus>: code hash -> GLSLC output

        /// NVN2 (Switch 2) control section: magic 0x98761234, version 201 (docs/AMPERE.md section 3).
        public static bool IsNvn2(byte[] ctrl) => ctrl != null && ctrl.Length >= 0xd8 && BitConverter.ToUInt32(ctrl, 0) == 0x98761234 && BitConverter.ToUInt32(ctrl, 4) == 201;

        static int Main(string[] args)
        {
            NoRetry = args.Contains("--no-retry");
            NoGlobalFallback |= args.Contains("--no-global-fallback");
            if (args.Contains("--no-pool-bindless")) StaticGpuAccessor.PoolBindless = false;   // A7: no pool arrays (StaticGpuAccessor)
            StaticGpuAccessor.DesktopHost = Opt(args, "--host", "desktop") != "moltenvk";
            Debug = args.Contains("--debug-mode");
            try
            {
                if (args.Length >= 1 && args[0] == "--serve") return Serve(args);
                if (args.Length >= 2 && args[0] == "--batch") return Batch(args);
                if (args.Length >= 2 && args[0] == "--hwcheck") return Hwcheck(args);
                if (args.Length >= 2 && !args[0].StartsWith("-")) return Single(args);
            }
            catch (ArgumentException e)
            {
                Console.Error.WriteLine(e.Message);
            }
            Console.Error.WriteLine("usage: spirv_harness <code.bin> <ctrl.bin> [-o out.spv] [--glsl] [--glslc <GLSLC output, NVN2>]\n" +
                                    "       spirv_harness --serve --cache <spirv dir> [--corpus <dir>] [--jobs N] [--priority below|idle|normal]   (protocol: Serve.cs)\n" +
                                    "       spirv_harness --batch <extracted dir> --out <dir> [--jobs N] [--limit N] [--no-val] [--no-retry] [--debug-mode] [--target-env E] [--host desktop|moltenvk] [--spirv-val PATH] [--reflect <Ampere corpus>]");
            return 2;
        }

        // --hwcheck <cases.tsv> --out <dir> [--local-x 32]: hardware-check modules (tools/shaders/hwcheck). Each line of cases.tsv is
        // "<id>	<lo hi lo hi ...>[	<memory test>]" (16-hex-digit instruction words; memory test: AmpereHwcheck.cs); writes <dir>/<id>.spv (a compute module that runs the decoded
        // instructions on the register file in storage buffer set 1 binding 0, see ampere/frontend/AmpereHwcheck.cs) and
        // <dir>/hwcheck.tsv (id, status, sass text, error). Status: ok, unsupported (the front end refuses a form), exception.
        static int Hwcheck(string[] args)
        {
            string outDir = Opt(args, "--out", null) ?? throw new ArgumentException("--out required");
            int localX = int.Parse(Opt(args, "--local-x", "32"));
            Directory.CreateDirectory(outDir);
            byte[] ctrl = new byte[0xd8];
            BitConverter.GetBytes(0x98761234u).CopyTo(ctrl, 0);
            BitConverter.GetBytes(201u).CopyTo(ctrl, 4);
            BitConverter.GetBytes((ushort)5).CopyTo(ctrl, 0x56);
            BitConverter.GetBytes(localX).CopyTo(ctrl, 0x58);
            BitConverter.GetBytes(1).CopyTo(ctrl, 0x5c);
            BitConverter.GetBytes(1).CopyTo(ctrl, 0x60);
            BitConverter.GetBytes(AmpereTranslator.HwcheckSharedBytes).CopyTo(ctrl, 0x64);   // shared memory size (+0x64; +0x50 is local memory), declared only when a test uses it
            var lines = File.ReadAllLines(args[1]).Where(l => l.Length > 0 && l[0] != '#').ToList();
            var rows = new string[lines.Count];
            Parallel.For(0, lines.Count, new ParallelOptions { MaxDegreeOfParallelism = 4 }, k =>
            {
                string[] f = lines[k].Split('\t');
                string id = f[0];
                ulong[] w = f[1].Split(' ', StringSplitOptions.RemoveEmptyEntries).Select(x => Convert.ToUInt64(x, 16)).ToArray();
                var insts = new List<SassInst>();
                for (int n = 0; n + 1 < w.Length; n += 2) insts.Add(SassDecoder.Decode(w[n], w[n + 1], n * 8));
                string text = string.Join(" | ", insts.Select(x => x.ToSassText()));
                string status = "ok", err = "";
                try
                {
                    StaticGpuAccessor gpu = new(Array.Empty<byte>(), 0, 0, ShaderStage.Compute, ctrl);
                    TranslationOptions options = new(TargetLanguage.Spirv, TargetApi.Vulkan, TranslationFlags.Compute);
                    TranslatorContext tctx = AmpereTranslator.CreateHwcheckContext(gpu, options, insts, out AmpereProgram prog, f.Length > 2 ? f[2] : null);
                    ShaderProgram sp = tctx.Translate();
                    if (prog.Unsupported.Count > 0) { status = "unsupported"; err = string.Join("; ", prog.Unsupported); }
                    else File.WriteAllBytes(Path.Combine(outDir, id + ".spv"), sp.BinaryCode);
                }
                catch (Exception e)
                {
                    status = "exception";
                    err = $"{e.GetType().Name}: {e.Message}";
                }
                rows[k] = $"{id}\t{status}\t{text}\t{err.Replace('\t', ' ').Replace('\n', ' ')}";
            });
            File.WriteAllLines(Path.Combine(outDir, "hwcheck.tsv"), rows);
            Console.WriteLine($"hwcheck: {rows.Count(r => r.Split('\t')[1] == "ok")} of {rows.Length} translated -> {outDir}");
            return 0;
        }

        // spirv-val: $VULKAN_SDK\Bin, else the PATH.
        static string FindSpirvVal()
        {
            string exe = OperatingSystem.IsWindows() ? "spirv-val.exe" : "spirv-val";
            string sdk = Environment.GetEnvironmentVariable("VULKAN_SDK");
            if (!string.IsNullOrEmpty(sdk) && File.Exists(Path.Combine(sdk, "Bin", exe))) return Path.Combine(sdk, "Bin", exe);
            return exe;
        }

        static string Opt(string[] args, string name, string def)
        {
            int i = Array.IndexOf(args, name);
            return i >= 0 && i + 1 < args.Length ? args[i + 1] : def;
        }

        static int Single(string[] args)
        {
            bool glsl = args.Contains("--glsl");
            string output = Opt(args, "-o", Path.ChangeExtension(args[0], glsl ? ".glsl" : ".spv"));
            string glslcPath = Opt(args, "--glslc", null);
            Result r = TranslateVariants(File.ReadAllBytes(args[0]), File.ReadAllBytes(args[1]), out byte[] spv, out byte[] spvCm, glsl, glslcPath);
            if (spv != null) File.WriteAllBytes(output, spv);
            Console.WriteLine($"{r.Stage} {r.Status} {r.SpvBytes} bytes {r.Ms:F1} ms -> {(spv != null ? output : "-")}");
            if (spvCm != null)   // the cooperative-matrix variant next to it (AmpereCoopMat.cs)
            {
                string cm = Path.ChangeExtension(output, ".cm.spv");
                File.WriteAllBytes(cm, spvCm);
                Console.WriteLine($"cooperative-matrix variant {spvCm.Length} bytes -> {cm}");
            }
            Console.WriteLine($"resources: {r.Resources}");
            foreach (string m in r.Messages) Console.WriteLine($"translator: {m}");
            foreach (string m in r.Unsupported) Console.WriteLine($"unsupported: {m}");
            if (r.Reflection != "") Console.WriteLine($"glslc reflection: {r.Reflection}");
            if (r.Error != "") Console.WriteLine($"error: {r.Error}");
            return spv != null ? 0 : 1;
        }

        // --no-coopmat or NXRT_HARNESS_COOPMAT=0: no cooperative-matrix variants (<hash>.cm.spv, ampere/frontend/AmpereCoopMat.cs)
        static bool NoCoopMat = Environment.GetEnvironmentVariable("NXRT_HARNESS_COOPMAT") == "0";

        // Translate, plus the cooperative-matrix variant of a compute block with HMMA (AmpereCoopMat.cs): the same block again with
        // HMMA as OpCooperativeMatrixMulAddKHR, kept only when it translates with the same status and the same resources (the
        // runtime applies the one results.tsv record to either module). spvCm is null otherwise.
        static Result TranslateVariants(byte[] code, byte[] ctrl, out byte[] spv, out byte[] spvCm, bool glsl = false, string glslcPath = null)
        {
            spvCm = null;
            AmpereCoopMat.Eligible = false;
            Result r = Translate(code, ctrl, out spv, glsl, glslcPath);
            if (glsl || spv == null || NoCoopMat || AmpereCoopMat.EnvActive || !AmpereCoopMat.Eligible) return r;
            AmpereCoopMat.Active = true;
            try
            {
                Result c = Translate(code, ctrl, out byte[] s, false, glslcPath);
                if (s != null && c.Status == r.Status && c.Resources == r.Resources) spvCm = s;
                else r.Messages.Add($"cooperative-matrix variant dropped: {c.Status} {c.Error} [{c.Resources}]");
            }
            finally { AmpereCoopMat.Active = false; }
            return r;
        }

        // One code block through the translator. Never throws: failures are recorded in the result.
        static Result Translate(byte[] code, byte[] ctrl, out byte[] spv, bool glsl = false, string glslcPath = null)
        {
            if (IsNvn2(ctrl)) return TranslateNvn2(code, ctrl, out spv, glsl, glslcPath);
            Result r = new();
            spv = null;
            int start = BitConverter.ToInt32(ctrl, 0x708);
            int constSize = BitConverter.ToInt32(ctrl, 0x6fc);
            int constOff = BitConverter.ToInt32(ctrl, 0x700);
            bool compute = BitConverter.ToUInt32(code, 0) == ComputeMagic ||
                           (ctrl.Length >= 0x718 && BitConverter.ToUInt32(ctrl, 0x714) == 5);   // NVN stage 5 (also set by dksh_to_corpus.py)
            // SPH word 0 bits 10..13. Used here for binding ranges only; the translator reads the SPH itself.
            int sphType = compute ? 0 : (BitConverter.ToInt32(code, start) >> 10) & 0xf;
            // SPH types: 0/1 vertex (A / B), 2 tessellation control, 3 tessellation evaluation, 4 geometry, 5 pixel.
            ShaderStage stage = compute ? ShaderStage.Compute : sphType switch
            {
                5 => ShaderStage.Fragment,
                4 => ShaderStage.Geometry,
                2 => ShaderStage.TessellationControl,
                3 => ShaderStage.TessellationEvaluation,
                _ => ShaderStage.Vertex,
            };
            r.Stage = stage.ToString().ToLowerInvariant();

            Stopwatch sw = Stopwatch.StartNew();
            // Attempt 1: normal translation. Attempt 2 (only after an exception, SPIR-V only, unless NoRetry):
            // DebugMode, which turns off the translator's expression propagation. That pass folds the temporaries
            // Ryujinx creates for SPIR-V call arguments back into the call, and the SPIR-V backend then throws in
            // GenerateCall (see docs/nvn/TRANSLATOR.md, "Failures"). Status "ok-retry" = translated by attempt 2.
            for (int attempt = 1; attempt <= (glsl || NoRetry ? 1 : 2); attempt++)
            {
                StaticGpuAccessor gpu = new(code, constOff, constSize, stage, ctrl);
                TranslationFlags flags = (compute ? TranslationFlags.Compute : TranslationFlags.None) |
                                         (attempt == 2 || Debug ? TranslationFlags.DebugMode : TranslationFlags.None);
                TranslationOptions options = new(glsl ? TargetLanguage.Glsl : TargetLanguage.Spirv, TargetApi.Vulkan, flags);
                try
                {
                    ShaderProgram program = Translator.CreateContext((ulong)start, gpu, options).Translate();
                    spv = glsl ? Encoding.UTF8.GetBytes(program.Code) : program.BinaryCode;
                    if (!glsl && spv != null && program.Info.Images.Count > 0) spv = AddImageWithoutFormatCapabilities(spv);
                    if (!glsl && spv != null) spv = AddMissingBuiltInCapabilities(spv);
                    r.SpvBytes = spv?.Length ?? 0;
                    r.Status = spv == null ? "no-output" : attempt == 1 ? "ok" : "ok-retry";
                    r.Resources = DescribeResources(program.Info, gpu);
                }
                catch (Exception e)
                {
                    r.Status = "exception";
                    r.Error = $"{e.GetType().Name}: {e.Message}";
                    string frame = e.StackTrace?.Split('\n').FirstOrDefault(l => l.Contains("Ryujinx.Graphics.Shader"))?.Trim();
                    if (frame != null) r.Error += $" @ {Regex.Replace(frame, @" in (/|[A-Za-z]:\\).*", "")}";
                }
                lock (gpu.Messages) r.Messages = gpu.Messages.Distinct().ToList();
                r.Queries = string.Join(',', gpu.DrawTimeQueries);
                if (r.Status != "exception") break;
            }
            r.Ms = sw.Elapsed.TotalMilliseconds;
            return r;
        }

        // One NVN2 (Switch 2, Ampere SM86) block: control section +0x38 program end, +0x3c/+0x40 constant data size/offset,
        // +0x56 stage (0 vs, 1 ps, 2 gs, 3 tcs, 4 tes, 5 cs). Graphics code starts with a 0x80-byte SPH v4, compute has
        // none. The SASS is decoded by tools/shaders/ampere/decoder and emitted into Ryujinx's IR by the Ampere front end
        // (tools/shaders/ampere/frontend); from there on it is Ryujinx's translator as for Maxwell. A block with any
        // instruction form the front end does not handle gets status "unsupported" and no SPIR-V.
        static Result TranslateNvn2(byte[] code, byte[] ctrl, out byte[] spv, bool glsl, string glslcPath)
        {
            Result r = new();
            spv = null;
            int nvnStage = BitConverter.ToUInt16(ctrl, 0x56);
            int progEnd = BitConverter.ToInt32(ctrl, 0x38);
            int constSize = BitConverter.ToInt32(ctrl, 0x3c), constOff = BitConverter.ToInt32(ctrl, 0x40);
            ShaderStage stage = nvnStage switch
            {
                0 => ShaderStage.Vertex, 1 => ShaderStage.Fragment, 2 => ShaderStage.Geometry,
                3 => ShaderStage.TessellationControl, 4 => ShaderStage.TessellationEvaluation, _ => ShaderStage.Compute,
            };
            // Task (6) and mesh (7) programs: a graphics SPH, translated as compute work groups into VK_EXT_mesh_shader stages
            // (tools/shaders/ampere/frontend/AmpereMesh.cs). Their resources bind in Ryujinx's vertex slot (task) and tessellation
            // evaluation slot (mesh): a mesh pipeline has neither of those stages.
            AmpereMeshInfo mesh = null;
            if (nvnStage is 6 or 7)
            {
                mesh = AmpereMeshInfo.FromNvn2(code, ctrl, nvnStage, out string meshError);
                if (mesh == null)
                {
                    r.Stage = nvnStage == 6 ? "task" : "mesh";
                    r.Status = "unsupported";
                    r.Error = "unsupported: " + meshError;
                    return r;
                }
            }
            bool compute = stage == ShaderStage.Compute;
            r.Stage = mesh != null ? (mesh.IsTask ? "task" : "mesh") : stage.ToString().ToLowerInvariant();
            int start = compute && mesh == null ? 0 : 0x80;
            int end = Math.Min(progEnd, code.Length) & ~0xf;
            while (end - 16 >= start && code.AsSpan(end - 16, 16).IndexOfAnyExcept((byte)0) < 0) end -= 16;
            if (nvnStage > 7 || end <= start)
            {
                r.Status = "exception";
                r.Error = $"NVN2 block without code (stage {nvnStage}, program end 0x{progEnd:x}, code 0x{code.Length:x} bytes)";
                return r;
            }
            Nvn2Reflection refl = glslcPath != null && File.Exists(glslcPath) ? Nvn2Reflection.Read(glslcPath) : null;
            if (refl != null) r.Reflection = refl.Describe(nvnStage);

            Stopwatch sw = Stopwatch.StartNew();
            IReadOnlyList<SassInst> insts = SassDecoder.DecodeBlock(code, start, end);
            for (int attempt = 1; attempt <= (glsl || NoRetry ? 1 : 2); attempt++)
            {
                // Address 0 of the accessor's code is the SPH (graphics) or the first instruction (compute); the constant
                // data offset (+0x40) is from the start of the code area in both cases.
                StaticGpuAccessor gpu = new(code, constOff, constSize, stage, ctrl, mesh == null ? -1 : mesh.IsTask ? 0 : 4);
                if (refl != null) foreach (var h in refl.BufferTextureHandles(nvnStage, ctrl)) gpu.BufferTextures.Add(h);
                else foreach (var h in BufferFetchHandles(insts, compute)) gpu.BufferTextures.Add(h);
                TranslationFlags flags = (compute ? TranslationFlags.Compute : TranslationFlags.None) |
                                         (attempt == 2 || Debug ? TranslationFlags.DebugMode : TranslationFlags.None);
                TranslationOptions options = new(glsl ? TargetLanguage.Glsl : TargetLanguage.Spirv, TargetApi.Vulkan, flags);
                AmpereProgram prog = null;
                try
                {
                    AmpereGlobal.Begin(NoGlobalFallback ? null : StaticGpuAccessor.GlobalFallbackWords(ctrl), compute);   // A17: untraceable global accesses
                    TranslatorContext tctx = AmpereTranslator.CreateContext(gpu, options, insts, out prog, mesh);
                    ShaderProgram sp = tctx.Translate();
                    if (prog.Unsupported.Count == 0)
                    {
                        spv = glsl ? Encoding.UTF8.GetBytes(sp.Code) : sp.BinaryCode;
                        if (!glsl && spv != null && sp.Info.Images.Count > 0) spv = AddImageWithoutFormatCapabilities(spv);
                        if (!glsl && spv != null) spv = AddMissingBuiltInCapabilities(spv);
                        r.SpvBytes = spv?.Length ?? 0;
                        r.Status = spv == null ? "no-output" : attempt == 1 ? "ok" : "ok-retry";
                        r.Resources = DescribeResources(sp.Info, gpu);
                        if (AmpereGlobal.Used) r.Resources += (r.Resources.Length > 0 ? " " : "") + "gmem";   // the runtime fills this stage's c[0] storage descriptors per draw
                        if (AmpereGlobal.UsedDeviceAddress) r.Resources += " gpt";
                        if (AmpereGlobal.UsedWrite) r.Resources += " gmemw";   // stores/atomics through it (nvn_replay.cpp gmem_before_op)   // and the guest page table words (c[0] 0xfe0, AmpereGlobal.EmitPageWalk)
                        if (refl != null) lock (gpu.Messages) gpu.Messages.AddRange(refl.Check(nvnStage, sp.Info));
                    }
                }
                catch (Exception e)
                {
                    r.Status = "exception";
                    r.Error = $"{e.GetType().Name}: {e.Message}";
                    string frame = e.StackTrace?.Split('\n').FirstOrDefault(l => l.Contains("Ryujinx.Graphics.Shader"))?.Trim();
                    if (frame != null) r.Error += $" @ {Regex.Replace(frame, @" in (/|[A-Za-z]:\\).*", "")}";
                }
                if (prog != null && prog.Unsupported.Count > 0)
                {
                    r.Status = "unsupported";
                    r.Unsupported = prog.Unsupported.ToList();
                    r.Error = "unsupported: " + string.Join("; ", r.Unsupported.Take(8)) + (r.Unsupported.Count > 8 ? $"; ... {r.Unsupported.Count - 8} more" : "");
                    r.SpvBytes = 0;
                    spv = null;
                }
                lock (gpu.Messages) r.Messages = gpu.Messages.Distinct().ToList();
                r.Queries = string.Join(',', gpu.DrawTimeQueries);
                if (r.Status != "exception") break;
            }
            r.Ms = sw.Elapsed.TotalMilliseconds;
            return r;
        }

        // Blocks without a GLSLC reflection (the ones only the runtime dumped): a plain 1D TLD (no array, LOD, offset or
        // multisample) is taken as a buffer texture fetch, as Ryujinx does for bindless 1D fetches. In the 99,012 reflected
        // blocks every handle read that way is a buffer sampler (the game has no 1D textures).
        static IEnumerable<(int Slot, int Handle)> BufferFetchHandles(IReadOnlyList<SassInst> insts, bool compute)
        {
            foreach (SassInst i in insts)
            {
                if (i.Opcode != "TLD" || i.Mods.Any(m => m is "LL" or "AOFFI" or "MS")) continue;
                var o = i.Operands;
                if (!o.Any(x => x.Kind == OperandKind.Other && x.Text == "1D")) continue;
                var imms = o.Where(x => x.Kind == OperandKind.Imm).ToList();
                if (imms.Count < 2) continue;
                int bank = (int)imms[0].ImmBits, handle = (int)imms[1].ImmBits;
                if (bank == 2) { yield return (-1, handle); yield return (2, handle); }
                else yield return (bank, handle);
            }
        }

        // Ryujinx declares storage images without a format when the host can load/store them that way (MoltenVK can:
        // shaderStorageImageRead/WriteWithoutFormat), but never adds the two capabilities SPIR-V requires for it.
        // Insert "OpCapability StorageImageReadWithoutFormat / WriteWithoutFormat" right after the 5-word header;
        // capabilities come first in a module, so that is a valid position.
        static byte[] AddImageWithoutFormatCapabilities(byte[] spv)
        {
            const uint OpCapability = 17, ReadWithoutFormat = 55, WriteWithoutFormat = 56;
            uint[] extra = [(2u << 16) | OpCapability, ReadWithoutFormat, (2u << 16) | OpCapability, WriteWithoutFormat];
            byte[] result = new byte[spv.Length + extra.Length * 4];
            Array.Copy(spv, result, 20);
            Buffer.BlockCopy(extra, 0, result, 20, extra.Length * 4);
            Array.Copy(spv, 20, result, 20 + extra.Length * 4, spv.Length - 20);
            return result;
        }

        // Ryujinx's SPIR-V back end declares ViewportIndex as a geometry-shader output (or fragment input) without
        // OpCapability MultiViewport, which spirv-val rejects (docs/debug/001.md). Insert it after the header.
        static byte[] AddMissingBuiltInCapabilities(byte[] spv)
        {
            const uint OpCapability = 17, OpDecorate = 71, DecorationBuiltIn = 11, BuiltInViewportIndex = 10;
            const uint MultiViewport = 57, ShaderViewportIndex = 70, ShaderViewportIndexLayerEXT = 5254;
            uint[] w = new uint[spv.Length / 4];
            Buffer.BlockCopy(spv, 0, w, 0, spv.Length);
            bool viewportIndex = false, haveCap = false;
            for (int i = 5; i < w.Length;)
            {
                uint op = w[i] & 0xffff, len = w[i] >> 16;
                if (len == 0) break;
                if (op == OpCapability && (w[i + 1] == MultiViewport || w[i + 1] == ShaderViewportIndex || w[i + 1] == ShaderViewportIndexLayerEXT)) haveCap = true;
                if (op == OpDecorate && len >= 4 && w[i + 2] == DecorationBuiltIn && w[i + 3] == BuiltInViewportIndex) viewportIndex = true;
                i += (int)len;
            }
            if (!viewportIndex || haveCap) return spv;
            uint[] extra = [(2u << 16) | OpCapability, MultiViewport];
            byte[] result = new byte[spv.Length + extra.Length * 4];
            Array.Copy(spv, result, 20);
            Buffer.BlockCopy(extra, 0, result, 20, extra.Length * 4);
            Array.Copy(spv, 20, result, 20 + extra.Length * 4, spv.Length - 20);
            return result;
        }

        // Compact description of what the shader binds, in Maxwell terms and in Vulkan terms:
        //   cb<slot>=s<set>b<binding>            constant buffer c[slot]
        //   sb[c<slot>+0x<off>]=s<set>b<binding> storage buffer whose address the shader loads from c[slot][off]
        //   tex<type>[c<slot>:<handle>]=s<set>b<binding>  texture whose handle is word <handle> of c[slot]
        // A texture with constant buffer slot 0x20000 gets its handle expression after the handle number: tex<type>[c131072:<n>|<rpn>].
        static string DescribeResources(ShaderProgramInfo info, StaticGpuAccessor gpu)
        {
            string Handle(TextureDescriptor t)
            {
                if (t.CbufSlot == 0x20000) lock (gpu.HandleExprs) if (gpu.HandleExprs.TryGetValue(t.HandleIndex, out string e)) return $"{t.HandleIndex}|{e}";
                return t.HandleIndex.ToString();
            }
            List<string> parts = new();
            foreach (BufferDescriptor b in info.CBuffers) parts.Add($"cb{b.Slot}=s{b.Set}b{b.Binding}");
            foreach (BufferDescriptor b in info.SBuffers) parts.Add($"sb[c{b.SbCbSlot}+0x{b.SbCbOffset:x}]=s{b.Set}b{b.Binding}");
            foreach (TextureDescriptor t in info.Textures)
                parts.Add($"tex{t.Type}[c{t.CbufSlot}:{Handle(t)}{(t.ArrayLength > 1 ? $"x{t.ArrayLength}" : "")}]=s{t.Set}b{t.Binding}");
            foreach (TextureDescriptor t in info.Images)
                parts.Add($"img{t.Type}[c{t.CbufSlot}:{t.HandleIndex}{(t.ArrayLength > 1 ? $"x{t.ArrayLength}" : "")}]=s{t.Set}b{t.Binding}");
            return string.Join(' ', parts).Replace(", ", "|");
        }

        // All distinct code blocks under the extraction root (one manifest.tsv per extracted container).
        static List<Job> LoadJobs(string root)
        {
            Dictionary<string, Job> jobs = new();
            // Ampere corpus layout (tools/shaders/ampere/extract_corpus.py): unique.tsv, code/<h>.bin, ctrl/<h>.bin, glslc/<h>.bin.
            string unique = Path.Combine(root, "unique.tsv");
            if (File.Exists(unique) && Directory.Exists(Path.Combine(root, "code")))
            {
                foreach (var row in ReadTsv(unique))
                {
                    string h = row["code_sha1_16"];
                    jobs[h] = new Job(h, row["stage"], Path.Combine(root, "code", h + ".bin"), Path.Combine(root, "ctrl", row["first_ctrl"] + ".bin"),
                                      int.Parse(row["occurrences"]), Path.Combine(root, "glslc", row["first_glslc"] + ".bin"));
                }
                return jobs.Values.ToList();
            }
            foreach (string manifest in Directory.EnumerateFiles(root, "manifest.tsv", SearchOption.AllDirectories)
                                                .OrderBy(x => x.Replace('\\', '/'), StringComparer.Ordinal))  // same order on Windows
            {
                string dir = Path.GetDirectoryName(manifest);
                string group = Path.GetRelativePath(root, dir);
                foreach (string line in File.ReadLines(manifest).Skip(1))
                {
                    string[] f = line.Split('\t');
                    string hash = f[10];
                    if (jobs.TryGetValue(hash, out Job j)) jobs[hash] = j with { Stages = j.Stages + 1 };
                    else jobs[hash] = new Job(hash, group, Path.Combine(dir, f[11]), Path.Combine(dir, f[12]), 1, GlslcByHash.GetValueOrDefault(hash));
                }
            }
            return jobs.Values.ToList();
        }

        static IEnumerable<Dictionary<string, string>> ReadTsv(string path)
        {
            string[] header = null;
            foreach (string line in File.ReadLines(path))
            {
                string[] f = line.Split('\t');
                if (header == null) { header = f; continue; }
                Dictionary<string, string> row = new();
                for (int k = 0; k < header.Length && k < f.Length; k++) row[header[k]] = f[k];
                yield return row;
            }
        }

        static int Batch(string[] args)
        {
            // --reflect <Ampere corpus>: find each block's GLSLC output (reflection section) by code hash.
            string reflect = Opt(args, "--reflect", null);
            if (reflect != null)
                foreach (var row in ReadTsv(Path.Combine(reflect, "unique.tsv")))
                    GlslcByHash[row["code_sha1_16"]] = Path.Combine(reflect, "glslc", row["first_glslc"] + ".bin");
            string root = args[1];
            string outDir = Opt(args, "--out", null) ?? throw new ArgumentException("--out is required");
            int jobsN = int.Parse(Opt(args, "--jobs", "2"));
            int limit = int.Parse(Opt(args, "--limit", "0"));
            string targetEnv = Opt(args, "--target-env", "vulkan1.3");
            bool validate = !args.Contains("--no-val");
            string spirvVal = Opt(args, "--spirv-val", FindSpirvVal());
            Directory.CreateDirectory(outDir);

            List<Job> jobs = LoadJobs(root);
            if (limit > 0) jobs = jobs.Take(limit).ToList();
            Console.WriteLine($"{jobs.Count} distinct code blocks, {jobsN} jobs, validation {(validate ? targetEnv : "off")}");

            // Each Parallel.ForEach body waits on a pool task (the timeout guard): without spare pool threads those tasks queue behind the
            // waiting bodies and the pool adds threads slowly, so fewer than --jobs blocks run at once.
            ThreadPool.GetMinThreads(out int minW, out int minIo);
            ThreadPool.SetMinThreads(Math.Max(minW, jobsN * 2 + 4), minIo);
            ConcurrentBag<Result> results = new();
            int done = 0;
            Stopwatch total = Stopwatch.StartNew();
            Parallel.ForEach(jobs, new ParallelOptions { MaxDegreeOfParallelism = jobsN }, job =>
            {
                Result r;
                byte[] spv = null;
                // A translator bug could loop forever; run on a task so a stuck block is reported, not waited on.
                byte[] spvCm = null;
                Task<(Result, byte[], byte[])> t = Task.Run(() =>
                {
                    Result x = TranslateVariants(File.ReadAllBytes(job.CodePath), File.ReadAllBytes(job.CtrlPath), out byte[] s, out byte[] cm, false, job.GlslcPath);
                    return (x, s, cm);
                });
                if (t.Wait(Timeout)) (r, spv, spvCm) = t.Result;
                else r = new Result { Status = "timeout", Error = $"no result after {Timeout.TotalSeconds} s" };
                r.Hash = job.Hash;
                r.Group = job.Group;
                r.Stages = job.Stages;
                if (spv != null)
                {
                    string spvPath = Path.Combine(outDir, job.Hash + ".spv");
                    File.WriteAllBytes(spvPath, spv);
                    string cmPath = Path.Combine(outDir, job.Hash + ".cm.spv");   // the cooperative-matrix variant (AmpereCoopMat.cs)
                    if (spvCm != null) File.WriteAllBytes(cmPath, spvCm);
                    else if (File.Exists(cmPath)) File.Delete(cmPath);
                    if (validate)
                    {
                        r.ValError = RunSpirvVal(spirvVal, targetEnv, spvPath);
                        if (r.ValError != "") r.Status = "invalid";
                    }
                }
                results.Add(r);
                int n = Interlocked.Increment(ref done);
                if (n % 500 == 0) Console.WriteLine($"  {n}/{jobs.Count}  {total.Elapsed.TotalSeconds:F0} s");
            });

            List<Result> all = results.OrderBy(r => r.Group).ThenBy(r => r.Hash).ToList();
            WriteResults(Path.Combine(outDir, "results.tsv"), all);
            string summary = Summarize(all, total.Elapsed, targetEnv);
            File.WriteAllText(Path.Combine(outDir, "summary.txt"), summary);
            Console.Write(summary);
            return 0;
        }

        // spirv-val prints "error: line N: <message>" then the offending instruction; keep the first line.
        static string RunSpirvVal(string tool, string targetEnv, string path)
        {
            ProcessStartInfo psi = new(tool, ["--target-env", targetEnv, path])
            {
                RedirectStandardError = true,
                RedirectStandardOutput = true,
            };
            using Process p = Process.Start(psi);
            string err = p.StandardError.ReadToEnd() + p.StandardOutput.ReadToEnd();
            p.WaitForExit();
            if (p.ExitCode == 0) return "";
            string first = err.Split('\n').FirstOrDefault(l => l.StartsWith("error")) ?? err.Split('\n')[0];
            return first.Trim();
        }

        static void WriteResults(string path, List<Result> all)
        {
            using StreamWriter w = new(path) { NewLine = "\n" };  // LF on Windows too: same bytes as on macOS
            w.WriteLine(ResultsHeader);
            foreach (Result r in all) w.WriteLine(FormatRow(r));
        }

        const string ResultsHeader = "hash\tgroup\tstage\tstages_using\tstatus\tms\tspv_bytes\tresources\ttranslator_messages\terror\tdraw_time_queries\tglslc_reflection";

        // One results.tsv line, without CR/LF in it (--serve also sends it as one stdout line).
        static string FormatRow(Result r)
        {
            string err = (r.Error + " " + r.ValError).Trim().Replace('\t', ' ');
            string row = $"{r.Hash}\t{r.Group}\t{r.Stage}\t{r.Stages}\t{r.Status}\t{r.Ms:F1}\t{r.SpvBytes}\t{r.Resources}\t" +
                         $"{string.Join(" | ", r.Messages).Replace('\t', ' ')}\t{err}\t{r.Queries}\t{r.Reflection}";
            return row.Replace('\r', ' ').Replace('\n', ' ');
        }

        // Group messages that differ only in numbers ("Attribute offset 0x3A0 ..." etc.) so the summary stays short.
        static string Normalize(string s) => Regex.Replace(Regex.Replace(s, @"0x[0-9A-Fa-f]+", "0x#"), @"\b\d+\b", "#");

        static string Summarize(List<Result> all, TimeSpan elapsed, string targetEnv)
        {
            StringBuilder sb = new();
            int stagesTotal = all.Sum(r => r.Stages);
            sb.AppendLine($"blocks {all.Count} (stages using them: {stagesTotal}), {elapsed.TotalSeconds:F0} s, spirv-val --target-env {targetEnv}");
            sb.AppendLine();
            sb.AppendLine("status        blocks   stages   (per stage kind)");
            foreach (var g in all.GroupBy(r => r.Status).OrderByDescending(g => g.Count()))
            {
                string kinds = string.Join(", ", g.GroupBy(r => r.Stage).OrderBy(k => k.Key).Select(k => $"{k.Key} {k.Count()}"));
                sb.AppendLine($"{g.Key,-12} {g.Count(),7} {g.Sum(r => r.Stages),8}   ({kinds})");
            }
            sb.AppendLine();
            sb.AppendLine("per group (blocks: ok / ok-retry / invalid / exception / timeout)");
            foreach (var g in all.GroupBy(r => Regex.Replace(r.Group, @"^(effect|layout)_.*", "$1_*")).OrderBy(g => g.Key))
            {
                sb.AppendLine($"  {g.Key,-24} {g.Count(),6}: {g.Count(r => r.Status == "ok")} / {g.Count(r => r.Status == "ok-retry")} / {g.Count(r => r.Status == "invalid")} / " +
                              $"{g.Count(r => r.Status == "exception")} / {g.Count(r => r.Status == "timeout")}");
            }
            void Top(string title, IEnumerable<string> items, int n = 25)
            {
                var groups = items.GroupBy(Normalize).OrderByDescending(g => g.Count()).ToList();
                if (groups.Count == 0) return;
                sb.AppendLine();
                sb.AppendLine(title);
                foreach (var g in groups.Take(n)) sb.AppendLine($"  {g.Count(),6}  {g.First()}");
                if (groups.Count > n) sb.AppendLine($"  ... {groups.Count - n} more kinds");
            }
            Top("exceptions (by message)", all.Where(r => r.Status is "exception" or "timeout").Select(r => r.Error));
            // Ampere front end: blocks per unhandled instruction form, and per mnemonic (a block counts once per entry).
            Top("unsupported instruction forms (blocks)", all.SelectMany(r => r.Unsupported), 40);
            Top("unsupported mnemonics (blocks)", all.SelectMany(r => r.Unsupported.Select(u => u.Split(':', '.')[0]).Distinct()), 40);
            Top("spirv-val errors (first error, by message)", all.Where(r => r.ValError != "").Select(r => r.ValError));
            Top("translator messages (blocks reporting them)", all.SelectMany(r => r.Messages));
            Top("draw-time queries (blocks making them)", all.SelectMany(r => r.Queries.Split(',', StringSplitOptions.RemoveEmptyEntries)));
            Top("resource kinds (blocks using them)", all.SelectMany(r => r.Resources.Split(' ', StringSplitOptions.RemoveEmptyEntries)
                .Select(x => Regex.Replace(x, @"=s\d+b\d+$", "")).Distinct()), 60);
            return sb.ToString();
        }
    }
}
