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
using System.Reflection;
using SpvInstruction = Spv.Generator.Instruction;

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
            // back end
            public SpvInstruction PosVar, IndexVar, ThreadIdVar, LoopVar;
            public readonly Dictionary<int, SpvInstruction> GenericVars = new();
        }

        [ThreadStatic] static State _cur;

        public const string PayloadName = "isbe_payload", AttrName = "isbe_attr", MapName = "isbe_map";
        const StorageClass TaskPayloadWorkgroup = (StorageClass)5402;
        const ExecutionModel TaskModel = (ExecutionModel)5364, MeshModel = (ExecutionModel)5365;
        const Capability MeshShading = (Capability)5283;
        const ExecutionMode OutputPrimitives = (ExecutionMode)5270, OutputTriangles = (ExecutionMode)5298;
        const BuiltIn PrimitiveTriangleIndices = (BuiltIn)5296;
        const Op SetMeshOutputs = (Op)5295, EmitMeshTasks = (Op)5294;

        internal static void Begin(AmpereMeshInfo info) => _cur = info == null ? null : new State { Info = info };
        public static bool Active => _cur != null;
        internal static AmpereMeshInfo Info => _cur?.Info;

        // ---- front end: the ISBE regions as shared memories ----------------------------------------------------------------------

        internal static int PayloadId(ResourceManager rm) => _cur.PayloadId >= 0 ? _cur.PayloadId
            : _cur.PayloadId = rm.Properties.AddSharedMemory(new MemoryDefinition(PayloadName, AggregateType.Array | AggregateType.U32, AmpereMeshInfo.PayloadWords));
        internal static int AttrId(ResourceManager rm) => _cur.AttrId >= 0 ? _cur.AttrId
            : _cur.AttrId = rm.Properties.AddSharedMemory(new MemoryDefinition(AttrName, AggregateType.Array | AggregateType.U32, _cur.Info.AttrWords));
        internal static int MapId(ResourceManager rm) => _cur.MapId >= 0 ? _cur.MapId
            : _cur.MapId = rm.Properties.AddSharedMemory(new MemoryDefinition(MapName, AggregateType.Array | AggregateType.U32, _cur.Info.MapWords));

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
            var globals = (List<SpvInstruction>)GlobalsField.GetValue(ctx);
            var entries = (List<SpvInstruction>)EntryPointsField.GetValue(ctx);
            if (entries.Count == 0) return;
            SpvInstruction ep = entries[0];
            // The entry point was added with no interface for task/mesh (see the SpirvGenerator hook); every global goes in.
            foreach (SpvInstruction g in globals) ep.AddOperand(g);
        }

        static void Emit(CodeGenContext ctx, SpvInstruction inst) => AddToFunction.Invoke(ctx, new object[] { inst });

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

            SpvInstruction attr = s.AttrId >= 0 && ctx.SharedMemories.TryGetValue(s.AttrId, out SpvInstruction a) ? a : null;
            SpvInstruction map = s.MapId >= 0 && ctx.SharedMemories.TryGetValue(s.MapId, out SpvInstruction mp) ? mp : null;
            SpvInstruction boolT = ctx.TypeBool();
            SpvInstruction count = map == null ? C(0) : ctx.Load(u32, ctx.AccessChain(ptrWg, map, C(0)));
            count = ctx.Select(u32, ctx.ULessThan(boolT, count, C(m.MaxPrimitives)), count, C(m.MaxPrimitives));
            SpvInstruction smo = ctx.NewInstruction(SetMeshOutputs);
            smo.AddOperand(C(m.MaxVertices)); smo.AddOperand(count);
            Emit(ctx, smo);
            SpvInstruction tid = ctx.CompositeExtract(u32, ctx.Load(ctx.TypeVector(u32, 3), s.ThreadIdVar), (LiteralInteger)0);
            SpvInstruction f32 = ctx.TypeFP32();
            SpvInstruction ptrOutF = ctx.TypePointer(StorageClass.Output, f32);

            // vertices: v = tid; v < max; v += threads
            StridedLoop(ctx, s.LoopVar, tid, C(m.MaxVertices), C(m.Threads), v =>
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
                    return ctx.Select(u32, ctx.ULessThan(boolT, idx, C(m.MaxVertices)), idx, C(0));
                }
                SpvInstruction tri = ctx.CompositeConstruct(uvec3, Byte(b0), Byte(ctx.IAdd(u32, b0, C(1))), Byte(ctx.IAdd(u32, b0, C(2))));
                ctx.Store(ctx.AccessChain(ctx.TypePointer(StorageClass.Output, uvec3), s.IndexVar, p), tri);
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
