// SPDX-License-Identifier: MIT
// NVN2 task and mesh programs (stages 6 and 7) as VK_EXT_mesh_shader stages.
//
// The hardware runs them like compute work groups that talk to the fixed-function units through the internal stage buffer (ISBE):
//   task  ISBERD.ATTR.SKEW [RZ]          its work-group index
//         ISBEWR.O.ATTR.32 [a], Rb       the payload for the mesh work groups it launches; word 1 (byte 4) is how many it launches
//   mesh  ISBERD.ATTR[.SKEW] [RZ]        its work-group index (among the groups its task launched)
//         ISBERD.ATTR.32 [a]             word a/4 of the task's payload
//         ISBEWR.O.ATTR.SKEW.32 [a], Rb  output attribute word a/4: vertex v, line k at word (v >> 5) * lines * 32 + k * 32 + (v & 31)
//                                        (the shader computes that address itself; .SKEW adds nothing)
//         ISBEWR.O.32 [3], Rb            the primitive count
//         ISBEWR.O [a], Rb (8-bit)       byte a of the index map: triangle p's vertices at bytes 4 + 3p .. 6 + 3p
// The output lines are the components the SPH's OMAP enables, in order: position x, y, z, w (system values B bits 4..7), then the
// enabled components of generic vectors 0..31. Work-group size, the vertex and primitive limits and the task payload size come from
// the NVN2 control section (+0x58 threads, +0x5a task payload bytes, +0x60 maximum vertices, +0x62 maximum primitives; read from the
// game's blocks, every one 32 threads). Same semantics as the Lichii emulator's nx2 notes (zortos293/Lichii, row 89).
//
// Translation: the block becomes a compute program for Ryujinx (work-group size from the control section), with the ISBE regions as
// extra shared memories: "isbe_payload" (declared TaskPayloadWorkgroupEXT instead of Workgroup), "isbe_attr" and "isbe_map". Every EXIT
// jumps to one epilogue block (barrier, return); at that return the SPIR-V back end emits what the hardware does after the program:
// mesh: SetMeshOutputsEXT(max vertices, min(count, max primitives)), every vertex's lines copied into gl_Position and the generic
// outputs, every triangle's indices from the map; task: EmitMeshTasksEXT(payload word 1, 1, 1, payload). The module becomes SPIR-V 1.4
// (VK_EXT_mesh_shader needs it), whose entry point lists every global variable.
// Hooks: translator/spirv-else-if.patch (SpirvGenerator.cs), translator/ampere-mesh-instructions.patch (Instructions.cs: returns,
// payload pointers), translator/spirv-pixld-declarations-spirv.patch (Declarations.cs: payload storage class).
using Ryujinx.Graphics.Shader.CodeGen.Spirv;
using Ryujinx.Graphics.Shader.IntermediateRepresentation;
using Ryujinx.Graphics.Shader.StructuredIr;
using Spv.Generator;
using static Spv.Specification;
using System;
using System.Collections.Generic;
using System.Linq;
using System.Reflection;
using SpvInstruction = Spv.Generator.Instruction;
using IrInst = Ryujinx.Graphics.Shader.IntermediateRepresentation.Instruction;

namespace Ryujinx.Graphics.Shader.Translation
{
    /// What a task or mesh block declares about itself (control section and SPH).
    public sealed class AmpereMeshInfo
    {
        public bool IsTask;
        public int Threads = 32;
        public int MaxVertices, MaxPrimitives;
        public int PayloadBytes;
        /// Output lines of a mesh block: Location -1 = position, else the generic vector; Component 0..3.
        public List<(int Location, int Component)> Lines = new();

        public const int PayloadWords = 1024;   // the declared task payload (4 KiB; the game's largest is 0xc80 bytes); task and mesh agree

        public int AttrWords => Math.Max(1, (MaxVertices + 31) / 32 * Math.Max(1, Lines.Count) * 32);
        public int MapWords => 2 + (3 * MaxPrimitives + 3) / 4;

        /// From a block's code (SPH at 0) and NVN2 control section; null with `error` when the block uses something not handled.
        public static AmpereMeshInfo FromNvn2(byte[] code, byte[] ctrl, int nvnStage, out string error)
        {
            error = null;
            if (ctrl == null || ctrl.Length < 0x64 || code.Length < 0x80) { error = "control section or SPH too short"; return null; }
            AmpereMeshInfo m = new()
            {
                IsTask = nvnStage == 6,
                Threads = BitConverter.ToUInt16(ctrl, 0x58),
                PayloadBytes = BitConverter.ToUInt16(ctrl, 0x5a),
                MaxVertices = BitConverter.ToUInt16(ctrl, 0x60),
                MaxPrimitives = BitConverter.ToUInt16(ctrl, 0x62),
            };
            if (m.Threads is < 1 or > 128) { error = $"work group of {m.Threads} threads"; return null; }
            if (m.IsTask)
            {
                if (m.PayloadBytes > AmpereMeshInfo.PayloadWords * 4) { error = $"task payload of {m.PayloadBytes} bytes"; return null; }
                return m;
            }
            if (m.MaxVertices is < 1 or > 256 || m.MaxPrimitives is < 1 or > 512) { error = $"mesh limits {m.MaxVertices} vertices, {m.MaxPrimitives} primitives"; return null; }
            // OMAP (SPH v3 layout for VTG): system values A at 0x32..0x34, B at 0x35, generic vectors 0x36..0x45 (4 bits each, x = bit 0),
            // colour / system values C / fixed-function textures after it.
            for (int b = 0x32; b < 0x35; b++) if (code[b] != 0) { error = "mesh outputs system values A (OMAP 0x32..0x34)"; return null; }
            byte svb = code[0x35];
            if ((svb & 0x0f) != 0) { error = $"mesh outputs primitive id / layer / viewport / point size (OMAP system values B 0x{svb:x2})"; return null; }
            for (int c = 0; c < 4; c++) if ((svb & (0x10 << c)) != 0) m.Lines.Add((-1, c));
            for (int v = 0; v < 32; v++)
            {
                int nib = (code[0x36 + v / 2] >> (v % 2 * 4)) & 0xf;
                for (int c = 0; c < 4; c++) if ((nib & (1 << c)) != 0) m.Lines.Add((v, c));
            }
            for (int b = 0x46; b < 0x50; b++) if (code[b] != 0) { error = $"mesh outputs colours / clip distances / fixed-function attributes (OMAP byte 0x{b:x})"; return null; }
            if ((svb & 0xf0) != 0xf0) { error = "mesh output without a full position"; return null; }
            return m;
        }
    }

    /// The task/mesh state of the block being translated on this thread (set by AmpereTranslator.CreateContext, read by the back end).
    public static class AmpereMesh
    {
        sealed class State
        {
            public AmpereMeshInfo Info;
            public int PayloadId = -1, AttrId = -1, MapId = -1;
            // direct outputs: one 1-word dummy shared memory per output line (its id names the line) and one for dynamic addresses
            public readonly Dictionary<int, int> OutIds = new();
            public int OutDynId = -1;
            // back end
            public SpvInstruction PosVar, IndexVar, ThreadIdVar, LoopVar;
            public readonly Dictionary<int, SpvInstruction> GenericVars = new();
            public SpvInstruction DynStoreFunc;   // void (u32 byte address, f32 value), body emitted in FinishModule
            // early primitive count (EarlyCount)
            public int SmoId = -1, CountWrites;
            public bool AttrSeen, EarlyOk;
            public bool AttrBeforeCount() => CountWrites != 1;
        }

        /// Mesh output attributes (ISBEWR.O.ATTR) are written straight into the mesh outputs (default since 2026-11-12, R8: with the real
        /// primitive count below, the village scene's GPU-bound frame got +2.3 fps; NXRT_HARNESS_MESH_DIRECT=0: staged in a shared-memory copy
        /// of the ISBE and copied out at the end of the program, the form before).
        public static readonly bool Direct = Environment.GetEnvironmentVariable("NXRT_HARNESS_MESH_DIRECT") != "0";
        public const string OutName = "isbe_out", OutDynName = "isbe_outd";
        /// NXRT_HARNESS_MESH_SLOW=call: an attribute address of another form goes through an out-of-line store function (default: decoded
        /// inline; a call costs the whole program registers: NVIDIA saves live values around it).
        static readonly bool SlowInline = Environment.GetEnvironmentVariable("NXRT_HARNESS_MESH_SLOW") != "call";

        [ThreadStatic] static State _cur;

        public const string PayloadName = "isbe_payload", AttrName = "isbe_attr", MapName = "isbe_map";
        const StorageClass TaskPayloadWorkgroup = (StorageClass)5402;
        const ExecutionModel TaskModel = (ExecutionModel)5364, MeshModel = (ExecutionModel)5365;
        const Capability MeshShading = (Capability)5283;
        const ExecutionMode OutputPrimitives = (ExecutionMode)5270, OutputTriangles = (ExecutionMode)5298;
        const BuiltIn PrimitiveTriangleIndices = (BuiltIn)5296;
        const Op SetMeshOutputs = (Op)5295, EmitMeshTasks = (Op)5294;

        internal static void Begin(AmpereMeshInfo info)
        {
            _cur = info == null ? null : new State { Info = info };
            ProvenCount = UnprovenCount = 0;
        }
        public static bool Active => _cur != null;
        internal static AmpereMeshInfo Info => _cur?.Info;

        // ---- front end: the ISBE regions as shared memories ----------------------------------------------------------------------

        internal static int PayloadId(ResourceManager rm) => _cur.PayloadId >= 0 ? _cur.PayloadId
            : _cur.PayloadId = rm.Properties.AddSharedMemory(new MemoryDefinition(PayloadName, AggregateType.Array | AggregateType.U32, AmpereMeshInfo.PayloadWords));
        internal static int AttrId(ResourceManager rm) => _cur.AttrId >= 0 ? _cur.AttrId
            : _cur.AttrId = rm.Properties.AddSharedMemory(new MemoryDefinition(AttrName, AggregateType.Array | AggregateType.U32, _cur.Info.AttrWords));
        internal static int MapId(ResourceManager rm) => _cur.MapId >= 0 ? _cur.MapId
            : _cur.MapId = rm.Properties.AddSharedMemory(new MemoryDefinition(MapName, AggregateType.Array | AggregateType.U32, _cur.Info.MapWords));

        /// Direct outputs: the dummy shared memory whose stores the back end turns into writes of output line k (index: the ISBE byte
        /// address of line 0 of the vertex), or into a decode at run time (OutDynId, index: the byte address).
        internal static int OutId(ResourceManager rm, int k)
        {
            if (_cur.OutIds.TryGetValue(k, out int id)) return id;
            id = rm.Properties.AddSharedMemory(new MemoryDefinition(OutName + k, AggregateType.Array | AggregateType.U32, 1));
            _cur.OutIds[k] = id;
            return id;
        }
        internal static int OutDynId(ResourceManager rm) => _cur.OutDynId >= 0 ? _cur.OutDynId
            : _cur.OutDynId = rm.Properties.AddSharedMemory(new MemoryDefinition(OutDynName, AggregateType.Array | AggregateType.U32, 1));

        /// Storage class of a shared memory definition (Declarations / Instructions hooks).
        public static StorageClass StorageClassOf(string memoryName) => Active && memoryName == PayloadName ? TaskPayloadWorkgroup : StorageClass.Workgroup;

        // ---- back end ------------------------------------------------------------------------------------------------------------

        public static ExecutionModel Model(ExecutionModel normal) => !Active ? normal : _cur.Info.IsTask ? TaskModel : MeshModel;

        static readonly FieldInfo VersionField = typeof(Spv.Generator.Module).GetField("_version", BindingFlags.NonPublic | BindingFlags.Instance);
        static readonly FieldInfo GlobalsField = typeof(Spv.Generator.Module).GetField("_globals", BindingFlags.NonPublic | BindingFlags.Instance);
        static readonly FieldInfo EntryPointsField = typeof(Spv.Generator.Module).GetField("_entrypoints", BindingFlags.NonPublic | BindingFlags.Instance);
        static readonly MethodInfo AddToFunction = typeof(Spv.Generator.Module).GetMethod("AddToFunctionDefinitions", BindingFlags.NonPublic | BindingFlags.Instance);

        /// After Declarations.DeclareAll: capability, extension, SPIR-V 1.4, the mesh outputs and the helper variables.
        internal static void Declare(CodeGenContext ctx)
        {
            if (!Active) return;
            ctx.AddCapability(MeshShading);
            ctx.AddExtension("SPV_EXT_mesh_shader");
            VersionField.SetValue(ctx, (1u << 16) | (4u << 8));
            State s = _cur;
            AmpereMeshInfo m = s.Info;
            SpvInstruction u32 = ctx.TypeU32();
            SpvInstruction f32 = ctx.TypeFP32();
            // gl_LocalInvocationID: Ryujinx's own when the program reads it, else one of ours (a built-in may be declared once)
            if (!ctx.Inputs.TryGetValue(new IoDefinition(StorageKind.Input, IoVariable.ThreadId), out s.ThreadIdVar))
            {
                s.ThreadIdVar = ctx.Variable(ctx.TypePointer(StorageClass.Input, ctx.TypeVector(u32, 3)), StorageClass.Input);
                ctx.Decorate(s.ThreadIdVar, Decoration.BuiltIn, (LiteralInteger)(int)BuiltIn.LocalInvocationId);
                ctx.AddGlobalVariable(s.ThreadIdVar);
            }
            s.LoopVar = ctx.Variable(ctx.TypePointer(StorageClass.Private, u32), StorageClass.Private);
            ctx.AddGlobalVariable(s.LoopVar);
            if (m.IsTask) return;

            SpvInstruction vec4 = ctx.TypeVector(f32, 4);
            SpvInstruction nv = ctx.Constant(u32, m.MaxVertices);
            SpvInstruction perVertex = ctx.TypeStruct(true, vec4);
            ctx.Decorate(perVertex, Decoration.Block);
            ctx.MemberDecorate(perVertex, (LiteralInteger)0, Decoration.BuiltIn, (LiteralInteger)(int)BuiltIn.Position);
            s.PosVar = ctx.Variable(ctx.TypePointer(StorageClass.Output, ctx.TypeArray(perVertex, nv, true)), StorageClass.Output);
            ctx.AddGlobalVariable(s.PosVar);
            foreach ((int loc, int _) in m.Lines)
            {
                if (loc < 0 || s.GenericVars.ContainsKey(loc)) continue;
                SpvInstruction v = ctx.Variable(ctx.TypePointer(StorageClass.Output, ctx.TypeArray(vec4, nv)), StorageClass.Output);
                ctx.Decorate(v, Decoration.Location, (LiteralInteger)loc);
                ctx.AddGlobalVariable(v);
                s.GenericVars[loc] = v;
            }
            s.IndexVar = ctx.Variable(ctx.TypePointer(StorageClass.Output, ctx.TypeArray(ctx.TypeVector(u32, 3), ctx.Constant(u32, m.MaxPrimitives))), StorageClass.Output);
            ctx.Decorate(s.IndexVar, Decoration.BuiltIn, (LiteralInteger)(int)PrimitiveTriangleIndices);
            ctx.AddGlobalVariable(s.IndexVar);
        }

        /// The execution modes besides LocalSize (added by the compute path).
        internal static void AddModes(CodeGenContext ctx, SpvInstruction func)
        {
            if (!Active || _cur.Info.IsTask) return;
            ctx.AddExecutionMode(func, ExecutionMode.OutputVertices, (LiteralInteger)_cur.Info.MaxVertices);
            ctx.AddExecutionMode(func, OutputPrimitives, (LiteralInteger)_cur.Info.MaxPrimitives);
            ctx.AddExecutionMode(func, OutputTriangles);
        }

        /// Before Module.Generate: the entry point lists every global variable (SPIR-V 1.4).
        internal static void FinishModule(CodeGenContext ctx)
        {
            if (!Active) return;
            EmitDynStore(ctx);
            if (Environment.GetEnvironmentVariable("NXRT_HARNESS_MESH_LOG") == "1" && !_cur.Info.IsTask)
                Console.Error.WriteLine($"mesh direct outputs: {ProvenCount} stores with a proven address form, {UnprovenCount} checked at run time");
            var globals = (List<SpvInstruction>)GlobalsField.GetValue(ctx);
            var entries = (List<SpvInstruction>)EntryPointsField.GetValue(ctx);
            if (entries.Count == 0) return;
            SpvInstruction ep = entries[0];
            // The entry point was added with no interface for task/mesh (see the SpirvGenerator hook); every global goes in.
            foreach (SpvInstruction g in globals) ep.AddOperand(g);
        }

        static void Emit(CodeGenContext ctx, SpvInstruction inst) => AddToFunction.Invoke(ctx, new object[] { inst });

        static bool DirectMesh => Active && !_cur.Info.IsTask && Direct;

        // ---- direct outputs: the real primitive count where the program writes it (F2; default since 2026-11-12) -----------------------
        /// NXRT_HARNESS_MESH_EARLY_COUNT (default on with MESH_DIRECT; =0 off): when the program writes its primitive count (ISBEWR.O.32 [3]) once and before
        /// any output attribute, SetMeshOutputsEXT(max vertices, the count) goes right after that instruction (after its predicate: the
        /// count is lane 0's, and the instruction takes the first invocation's arguments) instead of the maximum at the start; the
        /// epilogue then writes only the real primitives (no degenerate ones up to the maximum).
        static readonly bool EarlyCount = Environment.GetEnvironmentVariable("NXRT_HARNESS_MESH_EARLY_COUNT") != "0";
        public const string SmoName = "isbe_smo";

        /// Front end, after each instruction (outside its predicate): notes attribute writes and the count write; after the count write the
        /// count is read back from the map (lane 0 wrote it) into a store to the dummy memory SmoName, which the back end turns into
        /// SetMeshOutputsEXT.
        internal static void AfterInstruction(EmitterContext ctx, string opcode, bool attr, bool w32)
        {
            if (!DirectMesh || !EarlyCount || opcode != "ISBEWR") return;
            State s = _cur;
            if (attr) { s.AttrSeen = true; return; }
            if (!w32) return;
            s.CountWrites++;
            if (s.AttrSeen || s.CountWrites > 1) { s.EarlyOk = false; return; }
            ResourceManager rm = ctx.ResourceManager;
            if (s.SmoId < 0) s.SmoId = rm.Properties.AddSharedMemory(new MemoryDefinition(SmoName, AggregateType.Array | AggregateType.U32, 1));
            var count = ctx.Load(StorageKind.SharedMemory, MapId(rm), OperandHelper.Const(0));
            ctx.Store(StorageKind.SharedMemory, s.SmoId, OperandHelper.Const(0), count);
            s.EarlyOk = true;
        }

        static bool EarlyOk => DirectMesh && EarlyCount && _cur.EarlyOk && !_cur.AttrBeforeCount();

        /// At the start of main (after its locals): direct outputs need the output counts before the first output write, so the mesh
        /// declares its maximum primitive count here; the epilogue gives the primitives past the real count degenerate indices.
        internal static void BeginMain(CodeGenContext ctx)
        {
            if (!DirectMesh || EarlyOk) return;
            SpvInstruction u32 = ctx.TypeU32();
            SpvInstruction smo = ctx.NewInstruction(SetMeshOutputs);
            smo.AddOperand(ctx.Constant(u32, _cur.Info.MaxVertices)); smo.AddOperand(ctx.Constant(u32, _cur.Info.MaxPrimitives));
            Emit(ctx, smo);
        }

        /// A store of the front end's direct-output form (EmitIsbewrMesh): the write of one output line of one vertex. False: not one.
        internal static bool TryGenerateOutStore(CodeGenContext ctx, AstOperation op)
        {
            if (!DirectMesh || op.StorageKind != StorageKind.SharedMemory || op.SourcesCount != 3 ||
                op.GetSource(0) is not AstOperand { Type: Ryujinx.Graphics.Shader.IntermediateRepresentation.OperandType.Constant } id) return false;
            string name = ctx.Properties.SharedMemories[id.Value].Name;
            if (name == SmoName)
            {
                // the early primitive count (AfterInstruction); dropped when the count cannot go early (BeginMain declared the maximum)
                if (EarlyOk)
                {
                    SpvInstruction u = ctx.TypeU32();
                    SpvInstruction cnt = ctx.Get(AggregateType.U32, op.GetSource(2));
                    cnt = ctx.Select(u, ctx.ULessThan(ctx.TypeBool(), cnt, ctx.Constant(u, _cur.Info.MaxPrimitives)), cnt, ctx.Constant(u, _cur.Info.MaxPrimitives));
                    SpvInstruction smo = ctx.NewInstruction(SetMeshOutputs);
                    smo.AddOperand(ctx.Constant(u, _cur.Info.MaxVertices)); smo.AddOperand(cnt);
                    Emit(ctx, smo);
                }
                return true;
            }
            if (!name.StartsWith(OutName, StringComparison.Ordinal)) return false;
            State s = _cur;
            AmpereMeshInfo m = s.Info;
            SpvInstruction u32 = ctx.TypeU32(), f32 = ctx.TypeFP32(), bt = ctx.TypeBool();
            SpvInstruction C(int v) => ctx.Constant(u32, v);
            SpvInstruction index = ctx.Get(AggregateType.U32, op.GetSource(1));
            SpvInstruction value = ctx.Bitcast(f32, ctx.Get(AggregateType.U32, op.GetSource(2)));
            void Slow(SpvInstruction addr)
            {
                if (SlowInline) { EmitDecodedStore(ctx, addr, value); return; }
                s.DynStoreFunc ??= ctx.Function(ctx.TypeVoid(), FunctionControlMask.DontInline, ctx.TypeFunction(ctx.TypeVoid(), false, u32, f32));
                ctx.FunctionCall(ctx.TypeVoid(), s.DynStoreFunc, addr, value);
            }
            if (name == OutDynName)
            {
                Slow(index);
                return true;
            }
            int k = int.Parse(name.AsSpan(OutName.Length));
            // index = byte address of line 0 of vertex v: (v >> 5) * lines * 128 + (v & 31) * 4
            int lineBytes = m.Lines.Count * 128;
            SpvInstruction rel = ctx.UMod(u32, index, C(lineBytes));
            SpvInstruction v = ctx.IAdd(u32, ctx.IMul(u32, ctx.UDiv(u32, index, C(lineBytes)), C(32)), ctx.ShiftRightLogical(u32, rel, C(2)));
            // When the address's form is proven from its definition (LineZeroForm) only the vertex bound is checked, and a vertex past it
            // is dropped (no other line can be meant); otherwise the form is checked here and another form is decoded (Slow).
            bool proven = !NoProof && LineZeroForm(op.GetSource(1), lineBytes, 0) is { Form: true } f && f.Max < 128 && f.Align >= 2;
            SpvInstruction inRange = ctx.ULessThan(bt, v, C(m.MaxVertices));
            if (proven)
            {
                SpvInstruction lStore = ctx.Label(), lEnd = ctx.Label();
                ctx.SelectionMerge(lEnd, SelectionControlMask.MaskNone);
                ctx.BranchConditional(inRange, lStore, lEnd);
                ctx.AddLabel(lStore);
                StoreLine(ctx, v, k, value);
                ctx.Branch(lEnd);
                ctx.AddLabel(lEnd);
                ProvenCount++;
                return true;
            }
            UnprovenCount++;
            SpvInstruction ok = ctx.LogicalAnd(bt, ctx.ULessThan(bt, rel, C(128)),
                                ctx.LogicalAnd(bt, ctx.IEqual(bt, ctx.BitwiseAnd(u32, index, C(3)), C(0)), inRange));
            SpvInstruction lFast = ctx.Label(), lSlow = ctx.Label(), lMerge = ctx.Label();
            ctx.SelectionMerge(lMerge, SelectionControlMask.MaskNone);
            ctx.BranchConditional(ok, lFast, lSlow);
            ctx.AddLabel(lFast);
            StoreLine(ctx, v, k, value);
            ctx.Branch(lMerge);
            ctx.AddLabel(lSlow);
            if (Environment.GetEnvironmentVariable("NXRT_HARNESS_MESH_EXP") != "nofallback") Slow(ctx.IAdd(u32, index, C(k * 128)));
            ctx.Branch(lMerge);
            ctx.AddLabel(lMerge);
            return true;
        }

        [ThreadStatic] static int ProvenCount, UnprovenCount;
        /// NXRT_HARNESS_MESH_PROOF=0: every direct attribute store checks its address form at run time.
        static readonly bool NoProof = Environment.GetEnvironmentVariable("NXRT_HARNESS_MESH_PROOF") == "0";

        /// What the address expression is known to be: Form = lineBytes * q + r with r in [0, Max], r a multiple of 2^Align (q any);
        /// Pure: q is 0 (the value itself is in [0, Max]).
        record struct AddrForm(bool Form, bool Pure, long Max, int Align);

        static AddrForm Unknown => new(false, false, 0, 0);

        static int TrailingZeros(long v) => v == 0 ? 31 : System.Numerics.BitOperations.TrailingZeroCount((ulong)v);

        /// Follows the address back through single-definition locals and integer adds, multiplies, ands and shifts (the compiler's
        /// "(v >> 5) * lines * 128 + ((v << 2) & 0x7c)" in its IMAD / LEA / LOP3 forms).
        static AddrForm LineZeroForm(IAstNode node, int lineBytes, int depth)
        {
            if (depth > 24) return Unknown;
            if (node is AstOperand o)
            {
                if (o.Type == Ryujinx.Graphics.Shader.IntermediateRepresentation.OperandType.Constant)
                {
                    long c = (uint)o.Value;
                    long r = c % lineBytes;
                    return new AddrForm(true, r == c, r, TrailingZeros(r));
                }
                if (o.Type == Ryujinx.Graphics.Shader.IntermediateRepresentation.OperandType.LocalVariable && o.Defs.Count == 1 &&
                    o.Defs.First() is AstAssignment a) return LineZeroForm(a.Source, lineBytes, depth + 1);
                return Unknown;
            }
            if (node is not AstOperation op) return Unknown;
            AddrForm S(int i) => LineZeroForm(op.GetSource(i), lineBytes, depth + 1);
            long? Cst(int i) => op.GetSource(i) is AstOperand { Type: Ryujinx.Graphics.Shader.IntermediateRepresentation.OperandType.Constant } c ? (uint)c.Value : null;
            switch (op.Inst)
            {
                case IrInst.Copy:
                    return S(0);
                case IrInst.Add:
                {
                    AddrForm a = S(0), b = S(1);
                    if (!a.Form || !b.Form) return Unknown;
                    long max = a.Max + b.Max;
                    if (max >= lineBytes) return Unknown;
                    return new AddrForm(true, a.Pure && b.Pure, max, Math.Min(a.Align, b.Align));
                }
                case IrInst.Multiply:
                {
                    for (int i = 0; i < 2; i++)
                    {
                        long? c = Cst(i);
                        if (c == null) continue;
                        if (c.Value % lineBytes == 0) return new AddrForm(true, false, 0, 31);
                        AddrForm x = S(1 - i);
                        if (x.Form && x.Max == 0) return new AddrForm(true, false, 0, 31);   // a multiple of lineBytes, times anything
                        if (x.Pure && x.Max * c.Value < lineBytes)
                            return new AddrForm(true, true, x.Max * c.Value, Math.Min(31, x.Align + TrailingZeros(c.Value)));
                    }
                    return Unknown;
                }
                case IrInst.ShiftLeft:
                {
                    long? s = Cst(1);
                    if (s == null || s.Value > 31) return Unknown;
                    AddrForm x = S(0);
                    if (!x.Form) return Unknown;
                    if (x.Max == 0) return new AddrForm(true, x.Pure, 0, 31);
                    long max = x.Max << (int)s.Value;
                    if (!x.Pure || max >= lineBytes) return Unknown;
                    return new AddrForm(true, true, max, Math.Min(31, x.Align + (int)s.Value));
                }
                case IrInst.BitwiseAnd:
                    for (int i = 0; i < 2; i++)
                    {
                        long? m = Cst(i);
                        if (m != null && m.Value < lineBytes) return new AddrForm(true, true, m.Value, TrailingZeros(m.Value));
                    }
                    return Unknown;
                default:
                    return Unknown;
            }
        }

        static void StoreLine(CodeGenContext ctx, SpvInstruction v, int k, SpvInstruction value)
        {
            State s = _cur;
            SpvInstruction u32 = ctx.TypeU32();
            SpvInstruction ptrOutF = ctx.TypePointer(StorageClass.Output, ctx.TypeFP32());
            (int loc, int comp) = s.Info.Lines[k];
            SpvInstruction dst = loc < 0
                ? ctx.AccessChain(ptrOutF, s.PosVar, v, ctx.Constant(u32, 0), ctx.Constant(u32, comp))
                : ctx.AccessChain(ptrOutF, s.GenericVars[loc], v, ctx.Constant(u32, comp));
            ctx.Store(dst, value);
        }

        /// The out-of-line store for attribute addresses of another form: decodes vertex and line at run time (an address past the
        /// vertices is dropped; the staged form wrote such a store into the last word).
        static void EmitDynStore(CodeGenContext ctx)
        {
            State s = _cur;
            if (s.DynStoreFunc == null) return;
            AmpereMeshInfo m = s.Info;
            SpvInstruction u32 = ctx.TypeU32(), f32 = ctx.TypeFP32(), bt = ctx.TypeBool();
            SpvInstruction C(int v) => ctx.Constant(u32, v);
            ctx.AddFunction(s.DynStoreFunc);
            SpvInstruction addr = ctx.FunctionParameter(u32), value = ctx.FunctionParameter(f32);
            ctx.AddLabel(ctx.Label());
            EmitDecodedStore(ctx, addr, value);
            ctx.Return();
            ctx.FunctionEnd();
        }

        /// The store of an ISBE attribute byte address: vertex and line decoded at run time, the line chosen by a switch (structured;
        /// ends in a fresh block of the current function).
        static void EmitDecodedStore(CodeGenContext ctx, SpvInstruction addr, SpvInstruction value)
        {
            AmpereMeshInfo m = _cur.Info;
            SpvInstruction u32 = ctx.TypeU32(), bt = ctx.TypeBool();
            SpvInstruction C(int v) => ctx.Constant(u32, v);
            SpvInstruction w = ctx.ShiftRightLogical(u32, addr, C(2));
            SpvInstruction groupWords = C(m.Lines.Count * 32);
            SpvInstruction r = ctx.UMod(u32, w, groupWords);
            SpvInstruction v = ctx.IAdd(u32, ctx.IMul(u32, ctx.UDiv(u32, w, groupWords), C(32)), ctx.BitwiseAnd(u32, r, C(31)));
            SpvInstruction k = ctx.ShiftRightLogical(u32, r, C(5));
            SpvInstruction end = ctx.Label();
            SpvInstruction lBody = ctx.Label();
            SpvInstruction inRange = ctx.ULessThan(bt, v, C(m.MaxVertices));
            ctx.SelectionMerge(end, SelectionControlMask.MaskNone);
            ctx.BranchConditional(inRange, lBody, end);
            ctx.AddLabel(lBody);
            SpvInstruction[] cases = new SpvInstruction[m.Lines.Count];
            IOperand[] targets = new IOperand[2 * m.Lines.Count];
            for (int i = 0; i < cases.Length; i++) { cases[i] = ctx.Label(); targets[2 * i] = (LiteralInteger)i; targets[2 * i + 1] = cases[i]; }
            SpvInstruction sEnd = ctx.Label();
            ctx.SelectionMerge(sEnd, SelectionControlMask.MaskNone);
            ctx.Switch(k, sEnd, targets);
            for (int i = 0; i < cases.Length; i++)
            {
                ctx.AddLabel(cases[i]);
                StoreLine(ctx, v, i, value);
                ctx.Branch(sEnd);
            }
            ctx.AddLabel(sEnd);
            ctx.Branch(end);
            ctx.AddLabel(end);
        }

        /// At the return of main. True when the terminator was emitted here (task: OpEmitMeshTasksEXT); false: the caller returns.
        internal static bool EmitEpilogue(CodeGenContext ctx)
        {
            if (!Active) return false;
            State s = _cur;
            AmpereMeshInfo m = s.Info;
            SpvInstruction u32 = ctx.TypeU32();
            SpvInstruction ptrWg = ctx.TypePointer(StorageClass.Workgroup, u32);
            SpvInstruction C(int v) => ctx.Constant(u32, v);
            // workgroup execution and memory barrier (payload included: the task's writes, the count word)
            ctx.ControlBarrier(C(2), C(2), C(0x108));
            if (m.IsTask)
            {
                if (s.PayloadId < 0 || !ctx.SharedMemories.TryGetValue(s.PayloadId, out SpvInstruction payload))
                {
                    // never wrote a payload: launches nothing
                    SpvInstruction none = ctx.NewInstruction(EmitMeshTasks);
                    none.AddOperand(C(0)); none.AddOperand(C(1)); none.AddOperand(C(1));
                    Emit(ctx, none);
                    return true;
                }
                SpvInstruction cnt = ctx.Load(u32, ctx.AccessChain(ctx.TypePointer(TaskPayloadWorkgroup, u32), payload, C(1)));
                SpvInstruction et = ctx.NewInstruction(EmitMeshTasks);
                et.AddOperand(cnt); et.AddOperand(C(1)); et.AddOperand(C(1)); et.AddOperand(payload);
                Emit(ctx, et);
                return true;
            }

            SpvInstruction attr = s.AttrId >= 0 && ctx.SharedMemories.TryGetValue(s.AttrId, out SpvInstruction a) ? a : null;            SpvInstruction map = s.MapId >= 0 && ctx.SharedMemories.TryGetValue(s.MapId, out SpvInstruction mp) ? mp : null;
            SpvInstruction boolT = ctx.TypeBool();
            SpvInstruction count = map == null ? C(0) : ctx.Load(u32, ctx.AccessChain(ptrWg, map, C(0)));
            count = ctx.Select(u32, ctx.ULessThan(boolT, count, C(m.MaxPrimitives)), count, C(m.MaxPrimitives));
            // measurement only (F2): NXRT_HARNESS_MESH_EXP=noprims: every mesh work group runs as before but outputs no primitive (a count
            // the compiler cannot fold: 0 unless the program wrote 0xdeadbeef), so the time left is the shader's own
            if (Environment.GetEnvironmentVariable("NXRT_HARNESS_MESH_EXP") == "noprims")
                count = ctx.Select(u32, ctx.IEqual(boolT, count, C(unchecked((int)0xdeadbeef))), count, C(0));
            SpvInstruction tid = ctx.CompositeExtract(u32, ctx.Load(ctx.TypeVector(u32, 3), s.ThreadIdVar), (LiteralInteger)0);
            SpvInstruction f32 = ctx.TypeFP32();
            SpvInstruction ptrOutF = ctx.TypePointer(StorageClass.Output, f32);
            SpvInstruction realCount = count;
            if (Direct && EarlyOk)
            {
                // SetMeshOutputsEXT ran where the program wrote its count (AfterInstruction): only the real primitives
            }
            else if (Direct)
            {
                // the counts were declared at the start (BeginMain, the maximum primitives); the vertices are written already
                count = C(m.MaxPrimitives);
            }
            else
            {
                SpvInstruction smo = ctx.NewInstruction(SetMeshOutputs);
                smo.AddOperand(C(m.MaxVertices)); smo.AddOperand(count);
                Emit(ctx, smo);
            }

            // vertices: v = tid; v < max; v += threads
            if (!Direct) StridedLoop(ctx, s.LoopVar, tid, C(m.MaxVertices), C(m.Threads), v =>
            {
                SpvInstruction g = ctx.ShiftRightLogical(u32, v, C(5));
                SpvInstruction baseW = ctx.IAdd(u32, ctx.IMul(u32, g, C(m.Lines.Count * 32)), ctx.BitwiseAnd(u32, v, C(31)));
                for (int k = 0; k < m.Lines.Count; k++)
                {
                    SpvInstruction w = attr == null ? C(0) : ctx.Load(u32, ctx.AccessChain(ptrWg, attr, ctx.IAdd(u32, baseW, C(k * 32))));
                    SpvInstruction fv = ctx.Bitcast(f32, w);
                    (int loc, int comp) = m.Lines[k];
                    SpvInstruction dst = loc < 0
                        ? ctx.AccessChain(ptrOutF, s.PosVar, v, C(0), C(comp))
                        : ctx.AccessChain(ptrOutF, s.GenericVars[loc], v, C(comp));
                    ctx.Store(dst, fv);
                }
            });
            // triangles: p = tid; p < count; p += threads
            SpvInstruction uvec3 = ctx.TypeVector(u32, 3);
            StridedLoop(ctx, s.LoopVar, tid, count, C(m.Threads), p =>
            {
                SpvInstruction b0 = ctx.IAdd(u32, C(4), ctx.IMul(u32, p, C(3)));
                SpvInstruction Byte(SpvInstruction b)
                {
                    if (map == null) return C(0);
                    SpvInstruction word = ctx.Load(u32, ctx.AccessChain(ptrWg, map, ctx.ShiftRightLogical(u32, b, C(2))));
                    SpvInstruction sh = ctx.ShiftLeftLogical(u32, ctx.BitwiseAnd(u32, b, C(3)), C(3));
                    SpvInstruction idx = ctx.BitwiseAnd(u32, ctx.ShiftRightLogical(u32, word, sh), C(0xff));
                    // an index past the vertices would be undefined: clamp
                    SpvInstruction keep = ctx.ULessThan(boolT, idx, C(m.MaxVertices));
                    // direct outputs: the primitives past the real count are degenerate (0, 0, 0): no area, nothing rasterized
                    if (Direct) keep = ctx.LogicalAnd(boolT, keep, ctx.ULessThan(boolT, p, realCount));
                    return ctx.Select(u32, keep, idx, C(0));
                }
                SpvInstruction tri = ctx.CompositeConstruct(uvec3, Byte(b0), Byte(ctx.IAdd(u32, b0, C(1))), Byte(ctx.IAdd(u32, b0, C(2))));                ctx.Store(ctx.AccessChain(ctx.TypePointer(StorageClass.Output, uvec3), s.IndexVar, p), tri);
            });
            return false;
        }

        /// for (i = start; i < end; i += step) body(i), with the counter in a Private variable (structured loop).
        static void StridedLoop(CodeGenContext ctx, SpvInstruction counter, SpvInstruction start, SpvInstruction end, SpvInstruction step, Action<SpvInstruction> body)
        {
            SpvInstruction u32 = ctx.TypeU32();
            ctx.Store(counter, start);
            SpvInstruction header = ctx.Label(), cond = ctx.Label(), bodyL = ctx.Label(), cont = ctx.Label(), merge = ctx.Label();
            ctx.Branch(header);
            ctx.AddLabel(header);
            ctx.LoopMerge(merge, cont, LoopControlMask.MaskNone);
            ctx.Branch(cond);
            ctx.AddLabel(cond);
            SpvInstruction i = ctx.Load(u32, counter);
            ctx.BranchConditional(ctx.ULessThan(ctx.TypeBool(), i, end), bodyL, merge);
            ctx.AddLabel(bodyL);
            body(i);
            ctx.Branch(cont);
            ctx.AddLabel(cont);
            ctx.Store(counter, ctx.IAdd(u32, ctx.Load(u32, counter), step));
            ctx.Branch(header);
            ctx.AddLabel(merge);
        }
    }
}
