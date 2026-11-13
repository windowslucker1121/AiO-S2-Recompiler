// SPDX-License-Identifier: MIT
// Ampere (SM86, NVN2) front end for Ryujinx's shader translator: builds a TranslatorContext from decoded SASS
// (Nx.Shaders.Ampere.SassInst) instead of Maxwell code. Compiled into the Ryujinx.Graphics.Shader assembly (see
// tools/shaders/harness/translator/Ryujinx.Graphics.Shader.csproj), so it uses the translator's internals directly:
// the instructions are emitted into Ryujinx's IR by AmpereEmit.cs, then Ryujinx's own CFG, SSA, optimiser and SPIR-V
// back end run unchanged.
//
// Design:
//   - Each reachable SASS instruction becomes one InstOp whose emitter is a closure over the SassInst; basic blocks
//     are split at branch targets and after control flow, so Translator.EmitShader's EnterBlock(address) marks the
//     label every branch refers to. Raw opcodes carry PT as guard: guards are emitted by AmpereEmit itself.
//   - Uniform registers UR0-62 and UP0-6 live in the IR as extra Flag registers (index 4+n and 68+n); the
//     ampere-uniform-registers.patch widens RegisterConsts.TotalCount so SSA tracks them.
//   - Volta control flow: BSSY/BSYNC/BREAK/WARPSYNC only steer reconvergence and are no-ops for one invocation;
//     BRA/EXIT/KILL as usual; BRX through its constant-buffer jump table; CALL to a block ending in RET (the
//     compiler's subroutines, return address passed in a register) is inlined at each call site; other CALLs are jumps.
//   - Attribute usage, used features and the SPH-derived definitions are computed here the way Decoder.Decode does
//     for Maxwell (same logic, Volta operand forms).
//   - Anything the emitter cannot translate is recorded in AmpereProgram.Unsupported; the caller must treat a
//     non-empty list as a failed block (never ship that SPIR-V).
using Nx.Shaders.Ampere;
using Ryujinx.Graphics.Shader.Decoders;
using Ryujinx.Graphics.Shader.Instructions;
using Ryujinx.Graphics.Shader.IntermediateRepresentation;
using Operand = Nx.Shaders.Ampere.Operand;
using System;
using System.Collections.Generic;
using System.Linq;

namespace Ryujinx.Graphics.Shader.Translation
{
    public sealed class AmpereUnsupportedException : Exception
    {
        public AmpereUnsupportedException(string message) : base(message) { }
    }

    /// Everything the emitter needs to know about the whole program.
    public sealed class AmpereProgram
    {
        internal readonly List<SassInst> Insts;
        internal readonly Dictionary<int, int> IndexOf = new();
        internal readonly Dictionary<int, int[]> IndirectTargets = new();   // BRX/BRXU address -> possible targets
        internal readonly Dictionary<int, (int Start, int End)> Subroutines = new(); // CALL address -> [start, RET address]
        internal readonly IGpuAccessor Gpu;
        internal ShaderStage Stage;
        internal AmpereMeshInfo Mesh;          // task/mesh block (AmpereMesh.cs), else null
        internal int EpilogueAddress = -1;     // task/mesh: every EXIT jumps here (the end-of-program block)
        internal readonly Dictionary<int, SassInst> SinFrom = new();   // MUFU.SIN/COS address -> the FMUL.RZ x, 1/(2pi) it reads (AmpereFuse)
        internal readonly HashSet<int> SinSources = new();
        internal readonly Dictionary<int, (int Counter, uint Max)> BatchBounds = new();   // ISETP address -> extra exit test (AmpereFuse.AnalyseBatchLoops)
        internal readonly HashSet<int> Speculated = new();   // predicated instructions emitted unconditionally (AmpereFuse.AnalyseSpeculation)               // addresses of those FMULs
        internal readonly Dictionary<int, IntermediateRepresentation.Operand> SinArg = new();   // FMUL address -> copy of its x, while emitting

        /// Reasons this block cannot be translated (instruction forms the front end does not handle). Non-empty = failed.
        public List<string> Unsupported { get; } = new();

        internal AmpereProgram(List<SassInst> insts, IGpuAccessor gpu)
        {
            Insts = insts;
            Gpu = gpu;
            for (int i = 0; i < insts.Count; i++) IndexOf[insts[i].Address] = i;
        }

        internal void Fail(SassInst i, string why)
        {
            string key = i.Opcode + (i.Mods.Count > 0 ? "." + string.Join('.', i.Mods) : "");
            string msg = why == null ? key : $"{key}: {why}";
            lock (Unsupported) if (!Unsupported.Contains(msg)) Unsupported.Add(msg);
        }

        internal SassInst At(int address) => IndexOf.TryGetValue(address, out int i) ? Insts[i] : null;
    }

    public static partial class AmpereTranslator
    {
        private const int ThreadsPerWarp = 32;

        /// Builds the translation context. `insts` are the program's instructions (addresses from 0 = first
        /// instruction after the SPH); the SPH itself is read through gpuAccessor at address 0 (graphics only).
        public static TranslatorContext CreateContext(IGpuAccessor gpuAccessor, TranslationOptions options, IReadOnlyList<SassInst> insts, out AmpereProgram program,
                                                      AmpereMeshInfo mesh = null)
        {
            List<SassInst> list = insts.ToList();
            // measurement only (G4): NXRT_HARNESS_MESH_EXP=empty: a mesh program is only its first EXIT (no work, no primitive)
            // measurement only (G-g1): NXRT_HARNESS_TASK_EXP=empty: a task program is only its first EXIT (launches no mesh work)
            if (((mesh != null && !mesh.IsTask && Environment.GetEnvironmentVariable("NXRT_HARNESS_MESH_EXP") == "empty") ||
                 (mesh != null && mesh.IsTask && AmpereMesh.TaskExp == "empty") || (mesh == null && Environment.GetEnvironmentVariable("NXRT_HARNESS_EXP_EMPTY") == "1")) && list.FirstOrDefault(x => x.Opcode == "EXIT" && x.Pred == 7) is SassInst ex)
                list = new List<SassInst> { ex with { Address = 0 } };
            program = new AmpereProgram(list, gpuAccessor);
            bool compute = options.Flags.HasFlag(TranslationFlags.Compute);
            AmpereMesh.Begin(mesh);   // this thread's task/mesh state for the emitters and the back end (null: none)
            program.Mesh = mesh;

            ShaderDefinitions definitions;
            int localMemorySize;
            if (mesh != null)
            {
                // Task/mesh: a compute work group for Ryujinx (AmpereMesh.cs); the code has a graphics SPH (local memory sizes).
                definitions = new ShaderDefinitions(ShaderStage.Compute, mesh.Threads, 1, 1);
                ShaderHeader header = new(gpuAccessor, 0);
                localMemorySize = header.ShaderLocalMemoryLowSize + header.ShaderLocalMemoryHighSize + (header.ShaderLocalMemoryCrsSize / ThreadsPerWarp);
            }
            else if (compute)
            {
                definitions = new ShaderDefinitions(ShaderStage.Compute, gpuAccessor.QueryComputeLocalSizeX(), gpuAccessor.QueryComputeLocalSizeY(), gpuAccessor.QueryComputeLocalSizeZ());
                localMemorySize = gpuAccessor.QueryComputeLocalMemorySize();
            }
            else
            {
                // SPH v4 (Ampere, clc797sph.h): the first 0x50 bytes have the SPH v3 layout Ryujinx reads (common words,
                // IMAP/OMAP for VTG; generic IMAP, OMAP targets, sample mask and depth for PS). The v4 additions (VMAP,
                // *_EXT fields) are after 0x50 and only matter for features we do not use.
                ShaderHeader header = new(gpuAccessor, 0);
                TransformFeedbackOutput[] tfo = Translator.GetTransformFeedbackOutputs(gpuAccessor, out ulong tfVecMap);
                definitions = new ShaderDefinitions(
                    header.Stage, gpuAccessor.QueryGraphicsState(), header.Stage == ShaderStage.Geometry && header.GpPassthrough,
                    header.ThreadsPerInputPrimitive, header.OutputTopology, header.MaxOutputVertexCount, header.ImapTypes,
                    header.OmapTargets, header.OmapSampleMask, header.OmapDepth, gpuAccessor.QueryHostSupportsScaledVertexFormats(),
                    tfVecMap, tfo);
                localMemorySize = header.ShaderLocalMemoryLowSize + header.ShaderLocalMemoryHighSize + (header.ShaderLocalMemoryCrsSize / ThreadsPerWarp);
            }
            program.Stage = definitions.Stage;

            AmpereProgram prog = program;
            FindSubroutines(prog);
            HashSet<int> reachable = new();
            SortedSet<int> leaders = new() { 0 };
            Walk(prog, 0, reachable, leaders, null);
            AmpereFuse.Analyse(prog);
            AmpereFuse.AnalyseSpeculation(prog);
            AmpereFuse.AnalyseBatchLoops(prog);

            // Features and attribute usage, over everything that will be emitted (main code and inlined subroutines).
            AttributeUsage attributeUsage = new(gpuAccessor);
            FeatureFlags features = FeatureFlags.None;
            byte clipDistances = 0;
            IEnumerable<SassInst> emitted = reachable.Select(a => prog.At(a))
                .Concat(prog.Subroutines.Values.Distinct().SelectMany(r => prog.Insts.Where(x => x.Address >= r.Start && x.Address <= r.End)));
            foreach (SassInst i in emitted.Distinct())
            {
                ScanFeatures(definitions, attributeUsage, ref features, ref clipDistances, i);
            }

            // Blocks: runs of reachable instructions split at leaders.
            DecodedFunction main = new(0);
            List<Block> blocks = new();
            Block current = null;
            int prevAddr = int.MinValue;
            foreach (SassInst i in prog.Insts)
            {
                if (!reachable.Contains(i.Address)) { current = null; continue; }
                if (current == null || leaders.Contains(i.Address) || i.Address != prevAddr + 0x10)
                {
                    current = new Block((ulong)i.Address);
                    blocks.Add(current);
                }
                SassInst captured = i;
                InstEmitter emitter = ctx => InstEmit.EmitAmpere(ctx, prog, captured, null);
                current.OpCodes.Add(new InstOp((ulong)i.Address, 7UL << 16, InstName.Nop, emitter, InstProps.None));
                current.EndAddress = (ulong)i.Address + 0x10;
                prevAddr = i.Address;
            }
            if (mesh != null && prog.Insts.Count > 0)
            {
                // The end of a task/mesh program: one block after all code that every EXIT jumps to (barrier, return; the back end adds
                // the output copies or the task launch at that return, AmpereMesh.EmitEpilogue).
                prog.EpilogueAddress = prog.Insts[^1].Address + 0x10;
                Block epi = new((ulong)prog.EpilogueAddress);
                epi.OpCodes.Add(new InstOp((ulong)prog.EpilogueAddress, 7UL << 16, InstName.Nop, ctx => InstEmit.EmitAmpereMeshEnd(ctx, prog), InstProps.None));
                epi.EndAddress = (ulong)prog.EpilogueAddress + 0x10;
                blocks.Add(epi);
            }
            main.SetBlocks(blocks.ToArray());

            DecodedProgram decoded = new(main, new Dictionary<ulong, DecodedFunction> { [0] = main }, attributeUsage, features, clipDistances, 0);
            int size = prog.Insts.Count == 0 ? 0 : prog.Insts[^1].Address + 0x10 + (compute ? 0 : 0x80);
            return new TranslatorContext(0, size, localMemorySize, definitions, gpuAccessor, options, decoded);
        }

        // ---- control flow ----------------------------------------------------------------------------------------

        internal static bool IsUnconditional(SassInst i) => i.Pred == 7 && !i.PredNeg;

        static int? LabelOf(SassInst i) => i.Operands.FirstOrDefault(o => o.Kind == OperandKind.Label) is Operand o ? (int)o.Offset : null;

        /// CALLs whose target block ends in RET before any EXIT: the compiler's subroutines (return address passed in a register).
        static void FindSubroutines(AmpereProgram p)
        {
            foreach (SassInst c in p.Insts)
            {
                if (c.Opcode != "CALL" || LabelOf(c) is not int target || p.At(target) == null) continue;
                int idx = p.IndexOf[target];
                for (int k = idx; k < p.Insts.Count && k < idx + 4096; k++)
                {
                    SassInst x = p.Insts[k];
                    if (x.Opcode == "RET") { p.Subroutines[c.Address] = (target, x.Address); break; }
                    if (x.Opcode == "EXIT" && IsUnconditional(x)) break;
                }
            }
        }

        /// Marks every instruction reachable from `start` in main code; records block leaders and indirect targets.
        static void Walk(AmpereProgram p, int start, HashSet<int> reachable, SortedSet<int> leaders, (int, int)? region)
        {
            Stack<int> work = new();
            work.Push(start);
            leaders.Add(start);
            while (work.Count > 0)
            {
                int a = work.Pop();
                while (true)
                {
                    SassInst i = p.At(a);
                    if (i == null || !reachable.Add(a)) break;
                    bool stop = false;
                    switch (i.Opcode)
                    {
                        case "BRA":
                            if (LabelOf(i) is int t) { work.Push(t); leaders.Add(t); }
                            stop = IsUnconditional(i);
                            break;
                        case "CALL":
                            if (p.Subroutines.ContainsKey(i.Address)) break;   // inlined; execution continues after it
                            if (LabelOf(i) is int ct) { work.Push(ct); leaders.Add(ct); }
                            stop = IsUnconditional(i);
                            break;
                        case "BRX":
                        case "BRXU":
                            foreach (int bt in BrxTargets(p, i)) { work.Push(bt); leaders.Add(bt); }
                            stop = IsUnconditional(i);
                            break;
                        case "EXIT":
                        case "RET":
                            stop = IsUnconditional(i);
                            break;
                    }
                    if (i.Opcode is "BRA" or "CALL" or "BRX" or "BRXU" or "EXIT" or "RET" or "KILL") leaders.Add(a + 0x10);
                    if (stop) break;
                    a += 0x10;
                }
            }
        }

        /// Jump table of a BRX: "IMNMX.U32 Ri, Ri, n" (bound; or "SEL/USEL Ri, Ri, n, P" after a compare with n),
        /// "SHF.L.U32 / IMAD.SHL.U32 Rj, Ri, 2" (byte index), "LDC Rk, c[0x1][Rj+off]" (table in the constant data),
        /// "BRX Rk -base". Target = entry + (address of BRX + 0x10 + encoded offset). The compiler schedules other work in
        /// between (up to ~60 instructions seen), so the chain is followed register by register over a wide window.
        internal static int[] BrxTargets(AmpereProgram p, SassInst brx)
        {
            if (p.IndirectTargets.TryGetValue(brx.Address, out int[] known)) return known;
            int[] result = Array.Empty<int>();
            p.IndirectTargets[brx.Address] = result;
            if (brx.Operands.Count < 1) return result;
            (Operand sel, long rel) = IndirectOperand(brx);
            int idx = p.IndexOf[brx.Address];
            int baseAddr = brx.Address + 0x10 + (int)rel;
            const int Window = 256;
            SassInst ldc = null;
            int bound = -1;
            int ldcAt = -1;
            for (int k = idx - 1; k >= 0 && k >= idx - Window; k--)
            {
                SassInst x = p.Insts[k];
                bool writesSel = x.DstCount > 0 && x.Operands.Count > 0 && x.Operands[0].Kind == sel.Kind && x.Operands[0].Reg == sel.Reg;
                if (!writesSel) continue;
                if ((x.Opcode == "LDC" || x.Opcode == "ULDC") && x.Operands.Count > 1 && x.Operands[1].Kind == OperandKind.CBuf && x.Operands[1].Bank == 1 && !x.Operands[1].BankIsUReg)
                {
                    ldc = x;
                    ldcAt = k;
                    break;
                }
                if (x.Opcode == "MOV" || x.Opcode == "R2UR" || x.Opcode == "UMOV") { sel = x.Operands[1]; continue; }
                break;
            }
            if (ldc != null && ldc.Operands[1].IndexReg >= 0 && ldc.Operands[1].IndexReg < 255)
            {
                // Follow the index register back: shift by 2 (or multiply by 4), then the clamp.
                OperandKind ik = ldc.Operands[1].IndexIsUReg ? OperandKind.UReg : OperandKind.Reg;
                int reg = ldc.Operands[1].IndexReg;
                for (int k = ldcAt - 1; k >= 0 && k >= ldcAt - Window && bound < 0; k--)
                {
                    SassInst x = p.Insts[k];
                    if (!(x.DstCount > 0 && x.Operands.Count > 0 && x.Operands[0].Kind == ik && x.Operands[0].Reg == reg)) continue;
                    bool imm2(int at, ulong v) => x.Operands.Count > at && x.Operands[at].Kind == OperandKind.Imm && x.Operands[at].ImmBits == v;
                    if (x.Opcode is "SHF" or "USHF" && x.Mods.Contains("L") && imm2(2, 2) && x.Operands[1].Kind == ik) { reg = x.Operands[1].Reg; continue; }
                    if (x.Opcode == "IMAD" && x.Mods.Contains("SHL") && imm2(2, 4) && x.Operands[1].Kind == ik) { reg = x.Operands[1].Reg; continue; }
                    if (x.Opcode is "IMNMX" or "UIMNMX" && x.Mods.Contains("U32") && x.Operands.Count >= 3 && x.Operands[2].Kind == OperandKind.Imm) { bound = (int)x.Operands[2].ImmBits; break; }
                    if (x.Opcode is "SEL" or "USEL" && x.Operands.Count >= 3 && x.Operands[2].Kind == OperandKind.Imm) { bound = (int)x.Operands[2].ImmBits; break; }
                    if (x.Opcode is "MOV" or "UMOV" && x.Operands[1].Kind == ik) { reg = x.Operands[1].Reg; continue; }
                    break;
                }
            }
            if (ldc == null) { p.Fail(brx, "jump table not found"); return result; }
            int tableOff = (int)ldc.Operands[1].Offset;
            int count = bound >= 0 ? bound + 1 : 64;
            List<int> targets = new();
            for (int e = 0; e < count; e++)
            {
                int t = (int)p.Gpu.ConstantBuffer1Read(tableOff + e * 4) + baseAddr;
                if (p.At(t) == null) { if (bound >= 0) { p.Fail(brx, $"jump table entry {e} is not an instruction"); return result; } break; }
                targets.Add(t);
            }
            if (targets.Count == 0) { p.Fail(brx, "empty jump table"); return result; }
            result = targets.ToArray();
            p.IndirectTargets[brx.Address] = result;
            return result;
        }

        /// BRX/BRXU/RET operand "Rn offset" (printed as one Other operand by the decoder, or as Reg + Imm).
        internal static (Operand reg, long offset) IndirectOperand(SassInst i)
        {
            Operand o = i.Operands.Last(x => x.Kind is not (OperandKind.Pred or OperandKind.UPred));
            if (o.Kind is OperandKind.Reg or OperandKind.UReg)
            {
                Operand imm = i.Operands.LastOrDefault(x => x.Kind == OperandKind.Imm);
                return (o, imm == null ? 0 : (long)imm.ImmBits);
            }
            string[] parts = o.Text.Split(' ', StringSplitOptions.RemoveEmptyEntries);
            if (parts.Length != 2) throw new FormatException($"indirect branch operand '{o.Text}'");
            string r = parts[0];
            Operand reg = r.StartsWith("UR") ? new Operand(OperandKind.UReg, r == "URZ" ? 63 : int.Parse(r[2..]))
                        : new Operand(OperandKind.Reg, r == "RZ" ? 255 : int.Parse(r[1..]));
            string v = parts[1];
            bool neg = v.StartsWith('-');
            long off = Convert.ToInt64(v.TrimStart('-')[2..], 16);
            return (reg, neg ? -off : off);
        }

        // ---- features and attributes (Decoder.FillBlock / SetUserAttributeUses for Volta forms) -------------------

        static void ScanFeatures(ShaderDefinitions definitions, AttributeUsage usage, ref FeatureFlags features, ref byte clip, SassInst i)
        {
            switch (i.Opcode)
            {
                case "LDL": case "STL": features |= FeatureFlags.LocalMemory; break;
                case "LDS": case "STS": case "ATOMS": features |= FeatureFlags.SharedMemory; break;
                case "ISBEWR": features |= FeatureFlags.SharedMemory; break;   // task/mesh: the ISBE regions are shared memories (8-bit map stores need the CAS lowering)
                case "LDSM": features |= FeatureFlags.SharedMemory | FeatureFlags.Shuffle; break;
                case "STG": case "ATOMG": case "ATOM": case "RED": case "SUST": case "SUATOM": case "SURED": features |= FeatureFlags.Store; break;
                case "SHFL": case "REDUX": case "MATCH": case "FSWZADD": case "HMMA": case "MOVM": features |= FeatureFlags.Shuffle; break;
                case "ALD": case "AST": case "IPA":
                    AttributeUses(definitions, usage, ref features, ref clip, i);
                    break;
            }
        }

        static void AttributeUses(ShaderDefinitions definitions, AttributeUsage usage, ref FeatureFlags features, ref byte clip, SassInst i)
        {
            Operand attr = i.Operands.FirstOrDefault(o => o.Kind == OperandKind.Attr);
            if (attr == null) return;
            int count = 1;
            bool isStore = i.Opcode == "AST", perPatch = i.Mods.Contains("P");
            if (i.Opcode != "IPA")
            {
                count = i.Mods.Contains("128") ? 4 : i.Mods.Contains("96") ? 3 : i.Mods.Contains("64") ? 2 : 1;
                if (i.Opcode == "ALD" && i.Mods.Contains("O")) isStore = true;
            }
            int offset = (int)attr.Offset;
            bool indexed = attr.IndexReg >= 0;
            if (indexed)
            {
                if (isStore) { usage.SetAllOutputUserAttributes(); definitions.EnableOutputIndexing(); }
                else { usage.SetAllInputUserAttributes(); definitions.EnableInputIndexing(); }
                return;
            }
            for (int elem = 0; elem < count; elem++)
            {
                int a = offset + elem * 4;
                if (perPatch)
                {
                    if (a is >= AttributeConsts.UserAttributePerPatchBase and < AttributeConsts.UserAttributePerPatchEnd)
                    {
                        int index = (a - AttributeConsts.UserAttributePerPatchBase) / 16;
                        if (isStore) usage.SetOutputUserAttributePerPatch(index); else usage.SetInputUserAttributePerPatch(index);
                    }
                }
                else if (a is >= AttributeConsts.UserAttributeBase and < AttributeConsts.UserAttributeEnd)
                {
                    int user = a - AttributeConsts.UserAttributeBase;
                    if (isStore) usage.SetOutputUserAttribute(user / 16); else usage.SetInputUserAttribute(user / 16, (user >> 2) & 3);
                }

                if (!isStore && (a == AttributeConsts.FogCoord || (a >= AttributeConsts.FrontColorDiffuseR && a < AttributeConsts.ClipDistance0) ||
                                 (a >= AttributeConsts.TexCoordBase && a < AttributeConsts.TexCoordEnd)))
                {
                    features |= FeatureFlags.FixedFuncAttr;
                }
                else if (isStore)
                {
                    switch (a)
                    {
                        case AttributeConsts.Layer:
                            if (definitions.Stage is not ShaderStage.Compute and not ShaderStage.Fragment) features |= FeatureFlags.RtLayer;
                            break;
                        case AttributeConsts.ViewportIndex:
                            if (definitions.Stage != ShaderStage.Fragment) features |= FeatureFlags.ViewportIndex;
                            break;
                        case >= AttributeConsts.ClipDistance0 and <= AttributeConsts.ClipDistance7:
                            if (definitions.Stage.IsVtg) clip |= (byte)(1 << ((a - AttributeConsts.ClipDistance0) / 4));
                            break;
                        case AttributeConsts.ViewportMask:
                            if (definitions.Stage != ShaderStage.Fragment) features |= FeatureFlags.ViewportMask;
                            break;
                    }
                }
                else
                {
                    if (a is AttributeConsts.PositionX or AttributeConsts.PositionY && definitions.Stage == ShaderStage.Fragment) features |= FeatureFlags.FragCoordXY;
                    if (a == AttributeConsts.InstanceId && definitions.Stage == ShaderStage.Vertex) features |= FeatureFlags.InstanceId;
                }
            }
        }
    }
}
