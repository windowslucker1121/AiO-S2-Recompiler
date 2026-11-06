// SPDX-License-Identifier: MIT
// Task and mesh programs: the internal stage buffer instructions (ISBERD / ISBEWR) and the end-of-program block. The semantics and
// the translation scheme are described in AmpereMesh.cs.
using Nx.Shaders.Ampere;
using Ryujinx.Graphics.Shader.IntermediateRepresentation;
using Ryujinx.Graphics.Shader.Translation;
using static Ryujinx.Graphics.Shader.IntermediateRepresentation.OperandHelper;
using Op = Nx.Shaders.Ampere.Operand;
using IrOperand = Ryujinx.Graphics.Shader.IntermediateRepresentation.Operand;

namespace Ryujinx.Graphics.Shader.Instructions
{
    static partial class InstEmit
    {
        /// The block every EXIT of a task/mesh program jumps to: all threads' ISBE writes become visible, then the program returns (the
        /// back end puts the output copies or the task launch before that return).
        internal static void EmitAmpereMeshEnd(EmitterContext ctx, AmpereProgram prog)
        {
            ResourceManager rm = ctx.ResourceManager;
            if (prog.Mesh.IsTask) AmpereMesh.PayloadId(rm);
            else { AmpereMesh.AttrId(rm); AmpereMesh.MapId(rm); }
            ctx.Barrier();
            ctx.Return();
        }

        static bool IsRz(Op m) => m.Kind == OperandKind.Mem && (m.Reg < 0 || m.Reg >= 255) && m.IndexReg < 0 && m.Offset == 0;

        static IrOperand WorkGroupIndex(EmitterContext ctx) => ctx.Load(StorageKind.Input, IoVariable.CtaId, null, Const(0));

        static IrOperand WordIndex(EmitterContext ctx, Op mem, int words) =>
            ctx.IMinimumU32(ctx.ShiftRightU32(MemAddress(ctx, mem), Const(2)), Const(words - 1));   // out of range: the last word, not undefined

        // ISBERD[.ATTR][.SKEW][.32] Rd, [Ra+imm]
        static void EmitIsberdMesh(EmitterContext ctx, AmpereProgram prog, SassInst i)
        {
            Op d = i.Operands[0], mem = i.Operands[1];
            if (Has(i, "O") || Has(i, "PATCH") || Has(i, "PRIM")) throw Unsup("ISBERD of the output or patch / primitive space");
            if (!Has(i, "32")) throw Unsup("ISBERD narrower than 32 bits");
            // Address 0 is the work-group index (task: its own; mesh: among the groups its task launched, which Vulkan counts the same way).
            if (IsRz(mem)) { Dst(ctx, d, WorkGroupIndex(ctx)); return; }
            if (prog.Mesh.IsTask) throw Unsup("task ISBERD past the work-group index");
            int payload = AmpereMesh.PayloadId(ctx.ResourceManager);
            IrOperand v = ctx.Load(StorageKind.SharedMemory, payload, WordIndex(ctx, mem, AmpereMeshInfo.PayloadWords));
            // A dynamic address may be 0 as well.
            IrOperand zero = ctx.ICompareEqual(MemAddress(ctx, mem), Const(0));
            Dst(ctx, d, ctx.ConditionalSelect(zero, WorkGroupIndex(ctx), v));
        }

        // ISBEWR.O[.ATTR][.SKEW][.U16|.32] [Ra+imm], Rb
        static void EmitIsbewrMesh(EmitterContext ctx, AmpereProgram prog, SassInst i)
        {
            if (prog.Mesh == null) throw Unsup("ISBEWR outside task/mesh programs");
            Op mem = i.Operands[0], data = i.Operands[1];
            if (!Has(i, "O")) throw Unsup("ISBEWR without .O");
            IrOperand value = Src(ctx, data);
            ResourceManager rm = ctx.ResourceManager;
            bool attr = Has(i, "ATTR"), w32 = Has(i, "32"), w16 = Has(i, "U16");
            if (prog.Mesh.IsTask)
            {
                if (!attr || !w32) throw Unsup("task ISBEWR other than .ATTR.32 (the payload)");
                ctx.Store(StorageKind.SharedMemory, AmpereMesh.PayloadId(rm), WordIndex(ctx, mem, AmpereMeshInfo.PayloadWords), value);
                return;
            }
            if (attr)
            {
                if (!w32) throw Unsup("mesh ISBEWR.ATTR narrower than 32 bits");
                ctx.Store(StorageKind.SharedMemory, AmpereMesh.AttrId(rm), WordIndex(ctx, mem, prog.Mesh.AttrWords), value);
                return;
            }
            // The map: the primitive count (a 32-bit write at byte 3, i.e. word 0) and the triangles' 8-bit vertex indices from byte 4.
            int map = AmpereMesh.MapId(rm);
            if (w32) { ctx.Store(StorageKind.SharedMemory, map, WordIndex(ctx, mem, prog.Mesh.MapWords), value); return; }
            IrOperand b = ctx.IMinimumU32(MemAddress(ctx, mem), Const(prog.Mesh.MapWords * 4 - 1));
            ctx.Store(w16 ? StorageKind.SharedMemory16 : StorageKind.SharedMemory8, map, b, value);
        }
    }
}
