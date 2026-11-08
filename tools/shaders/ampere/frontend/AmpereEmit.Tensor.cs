// SPDX-License-Identifier: MIT
// Tensor-core instructions (HMMA, LDSM, MOVM) as subgroup emulation: every lane gathers the matrix elements it needs from the
// lanes that hold them (IDX shuffles over the 32-lane warp) and does the arithmetic itself.
//
// This is the portable form. A block with HMMA also gets a cooperative-matrix variant (AmpereCoopMat.cs, <hash>.cm.spv): a SASS
// warp matrix lives in ordinary registers with a fixed per-lane layout (below), and a cooperative matrix's per-invocation layout is
// implementation-defined, so that variant relies on the device's order measured by a probe (hwcheck --coopmat; the runtime checks
// it again at start) and the runtime falls back to this form elsewhere.
//
// Fragment layouts (PTX ISA, "Matrix fragments for mma.m16n8k16 / m16n8k8 with floating point type", ldmatrix, movmatrix;
// g = lane >> 2, t = lane & 3; a 32-bit register holds two 16-bit elements, the lower-numbered column/k in the low half):
//   A (16 x K, row major)  reg q: row g + 8 (q & 1), k = 8 (q >> 1) + 2t + {0,1}      (K = 16: 4 regs, K = 8: 2 regs)
//   B (K x 8)              reg q: k = 8 q + 2t + {0,1}, column g                     (K = 16: 2 regs, K = 8: 1 reg)
//   C/D F16 (16 x 8)       reg q: row g + 8 q, columns 2t + {0,1}
//   C/D F32                reg q: row g + 8 (q >> 1), column 2t + (q & 1)
//   LDSM.M88 matrix m      lane m * 8 + r gives the byte address of row r (8 x 16-bit); the lane gets row g, columns 2t + {0,1}
//   LDSM.MT88 / MOVM.MT88  the lane gets the transposed fragment: rows 2t + {0,1}, column g
// The SASS register order equals PTX's fragment order (ptxas: mma {d0,d1}, {a0..a3}, {b0,b1}, {c0,c1} is HMMA Rd, Ra, Rb, Rc
// on consecutive registers); the hardware check (tools/shaders/hwcheck) confirms the layouts on an RTX 3080.
// Arithmetic: products of fp16 inputs are exact in fp32; they are summed in k order with fp32 fused multiply-adds, then
// the accumulator C is added and the result is rounded once to the destination type (round to nearest). The tensor core's
// own summation (alignment to the largest exponent, truncation) is not bit-reproduced; see the HMMA tolerance in hwcheck.
using Nx.Shaders.Ampere;
using Ryujinx.Graphics.Shader.IntermediateRepresentation;
using Ryujinx.Graphics.Shader.Translation;
using System.Collections.Generic;
using static Ryujinx.Graphics.Shader.IntermediateRepresentation.OperandHelper;
using Op = Nx.Shaders.Ampere.Operand;
using IrOperand = Ryujinx.Graphics.Shader.IntermediateRepresentation.Operand;

namespace Ryujinx.Graphics.Shader.Instructions
{
    static partial class InstEmit
    {
        static IrOperand LaneIdAmpere(EmitterContext ctx) =>
            ctx.BitwiseAnd(ctx.Load(StorageKind.Input, IoVariable.SubgroupLaneId), Const(0x1f));

        /// The two 16-bit elements of a register as fp32 (low, high).
        static (IrOperand lo, IrOperand hi) UnpackPair(EmitterContext ctx, IrOperand w, bool bf16) =>
            bf16 ? (ctx.ShiftLeft(w, Const(16)), ctx.BitwiseAnd(w, Const(unchecked((int)0xffff0000))))
                 : (ctx.UnpackHalf2x16Low(w), ctx.UnpackHalf2x16High(w));

        static void EmitHmmaAmpere(EmitterContext ctx, SassInst i)
        {
            // HMMA.{1688|16816}.{F16|F32}[.BF16] Rd, Ra, Rb, Rc: D (16 x 8) = A (16 x K) * B (K x 8) + C over the warp
            IReadOnlyList<Op> o = i.Operands;
            bool k16 = Has(i, "16816");
            if (!k16 && !Has(i, "1688")) throw Unsup("HMMA shape");
            bool f32 = Has(i, "F32"), bf16 = Has(i, "BF16");
            // BF16: a denormal bf16 input is an fp32 denormal here, which the host flushes (the RTX 3080 driver has no fp32
            // DenormPreserve) while the tensor core keeps it (hwcheck: HMMA.16816.F32.BF16 with bf16 0x0040 inputs). No corpus uses it.
            if (bf16) throw Unsup("HMMA .BF16 (denormal inputs not emulated)");
            foreach (Op x in o) if (x.Kind != OperandKind.Reg) throw Unsup("HMMA operand");
            int K = k16 ? 16 : 8, aRegs = k16 ? 4 : 2, bRegs = k16 ? 2 : 1;
            IrOperand Reg(Op x, int q) => x.Reg >= 255 ? Const(0) : Gpr(x.Reg + q);
            // the cooperative-matrix variant of a block (AmpereCoopMat.cs): one tensor-core multiply-add
            if (Ryujinx.Graphics.Shader.CodeGen.Spirv.AmpereCoopMat.TryEmit(ctx, i, Reg, (q, v) => { if (o[0].Reg < 255) DstWord(ctx, o[0], q, v); }))
                return;
            IrOperand lane = LaneIdAmpere(ctx);
            IrOperand g = ctx.ShiftRightU32(lane, Const(2)), t = ctx.BitwiseAnd(lane, Const(3));

            // A rows g and g + 8, B columns 2t and 2t + 1, each over k = 0..K-1
            var a = new IrOperand[2, K];
            var b = new IrOperand[2, K];
            for (int tp = 0; tp < 4; tp++)
            {
                IrOperand srcA = ctx.IAdd(ctx.ShiftLeft(g, Const(2)), Const(tp));
                for (int q = 0; q < aRegs; q++)
                {
                    (IrOperand lo, IrOperand hi) = UnpackPair(ctx, ReadLane(ctx, Reg(o[1], q), srcA), bf16);
                    int k = 8 * (q >> 1) + 2 * tp;
                    a[q & 1, k] = lo; a[q & 1, k + 1] = hi;
                }
                for (int nn = 0; nn < 2; nn++)
                {
                    IrOperand srcB = ctx.IAdd(ctx.ShiftLeft(ctx.IAdd(ctx.ShiftLeft(t, Const(1)), Const(nn)), Const(2)), Const(tp));
                    for (int q = 0; q < bRegs; q++)
                    {
                        (IrOperand lo, IrOperand hi) = UnpackPair(ctx, ReadLane(ctx, Reg(o[2], q), srcB), bf16);
                        int k = 8 * q + 2 * tp;
                        b[nn, k] = lo; b[nn, k + 1] = hi;
                    }
                }
            }

            // C, as fp32 per element [row half, column]
            var c = new IrOperand[2, 2];
            for (int rh = 0; rh < 2; rh++)
            {
                if (f32) { c[rh, 0] = Reg(o[3], 2 * rh); c[rh, 1] = Reg(o[3], 2 * rh + 1); }
                else { (c[rh, 0], c[rh, 1]) = UnpackPair(ctx, Reg(o[3], rh), false); }
            }

            var d = new IrOperand[2, 2];
            for (int rh = 0; rh < 2; rh++)
                for (int nn = 0; nn < 2; nn++)
                {
                    IrOperand acc = ctx.FPMultiply(a[rh, 0], b[nn, 0]);
                    for (int k = 1; k < K; k++) acc = ctx.FPFusedMultiplyAdd(a[rh, k], b[nn, k], acc);
                    d[rh, nn] = ctx.FPAdd(acc, c[rh, nn]);
                }

            Op rd = o[0];
            if (rd.Reg >= 255) return;
            if (f32)
            {
                for (int q = 0; q < 4; q++) DstWord(ctx, rd, q, d[q >> 1, q & 1]);
            }
            else
            {
                for (int q = 0; q < 2; q++) DstWord(ctx, rd, q, ctx.PackHalf2x16(d[q, 0], d[q, 1]));
            }
        }

        static IrOperand LoadShared32(EmitterContext ctx, IrOperand byteAddr) =>
            ctx.Load(StorageKind.SharedMemory, ctx.ResourceManager.SharedMemoryId, ctx.ShiftRightU32(byteAddr, Const(2)));

        static IrOperand LoadShared16(EmitterContext ctx, IrOperand byteAddr) =>
            ctx.BitwiseAnd(ctx.ShiftRightU32(LoadShared32(ctx, byteAddr), ctx.ShiftLeft(ctx.BitwiseAnd(byteAddr, Const(2)), Const(3))), Const(0xffff));

        static void EmitLdsmAmpere(EmitterContext ctx, SassInst i)
        {
            // LDSM.16.{M88|MT88}[.2|.4] Rd, [Ra+URb+imm]: 1, 2 or 4 8x8 16-bit matrices from shared memory, row addresses per lane
            IReadOnlyList<Op> o = i.Operands;
            if (o[0].Kind != OperandKind.Reg) throw Unsup("LDSM destination");
            if (ctx.TranslatorContext.Definitions.Stage != ShaderStage.Compute) throw Unsup("shared memory outside compute");
            bool trans = Has(i, "MT88");
            if (!trans && !Has(i, "M88")) throw Unsup("LDSM shape");
            int count = Has(i, "4") ? 4 : Has(i, "2") ? 2 : 1;
            IrOperand lane = LaneIdAmpere(ctx);
            IrOperand g = ctx.ShiftRightU32(lane, Const(2)), t = ctx.BitwiseAnd(lane, Const(3));
            IrOperand addr = MemAddress(ctx, o[1]);
            var res = new IrOperand[count];
            for (int m = 0; m < count; m++)
            {
                if (!trans)
                {
                    IrOperand row = ReadLane(ctx, addr, ctx.IAdd(g, Const(m * 8)));
                    res[m] = LoadShared32(ctx, ctx.IAdd(row, ctx.ShiftLeft(t, Const(2))));
                }
                else
                {
                    IrOperand r0 = ctx.IAdd(ctx.ShiftLeft(t, Const(1)), Const(m * 8));
                    IrOperand col = ctx.ShiftLeft(g, Const(1));
                    IrOperand lo = LoadShared16(ctx, ctx.IAdd(ReadLane(ctx, addr, r0), col));
                    IrOperand hi = LoadShared16(ctx, ctx.IAdd(ReadLane(ctx, addr, ctx.IAdd(r0, Const(1))), col));
                    res[m] = ctx.BitwiseOr(lo, ctx.ShiftLeft(hi, Const(16)));
                }
            }
            for (int m = 0; m < count; m++) DstWord(ctx, o[0], m, res[m]);
        }

        static void EmitMovmAmpere(EmitterContext ctx, SassInst i)
        {
            // MOVM.16.MT88 Rd, Ra: transpose of the 8x8 16-bit matrix the warp holds in Ra (row g, columns 2t + {0,1} per lane).
            // Element (R, C) is in lane 4R + C/2, half C & 1; the lane gets (2t + j, g) for j = 0, 1.
            IReadOnlyList<Op> o = i.Operands;
            if (!Has(i, "MT88") || o[0].Kind != OperandKind.Reg || o[1].Kind != OperandKind.Reg) throw Unsup("MOVM form");
            IrOperand lane = LaneIdAmpere(ctx);
            IrOperand g = ctx.ShiftRightU32(lane, Const(2)), t = ctx.BitwiseAnd(lane, Const(3));
            IrOperand v = o[1].Reg >= 255 ? Const(0) : Gpr(o[1].Reg);
            IrOperand shift = ctx.ShiftLeft(ctx.BitwiseAnd(g, Const(1)), Const(4));
            IrOperand baseLane = ctx.IAdd(ctx.ShiftLeft(t, Const(3)), ctx.ShiftRightU32(g, Const(1)));   // 4 (2t) + g/2
            IrOperand h0 = ctx.BitwiseAnd(ctx.ShiftRightU32(ReadLane(ctx, v, baseLane), shift), Const(0xffff));
            IrOperand h1 = ctx.BitwiseAnd(ctx.ShiftRightU32(ReadLane(ctx, v, ctx.IAdd(baseLane, Const(4))), shift), Const(0xffff));
            Dst(ctx, o[0], ctx.BitwiseOr(h0, ctx.ShiftLeft(h1, Const(16))));
        }
    }
}
