// SPDX-License-Identifier: MIT
// Ampere (Volta-family SASS) instructions into Ryujinx's IR. A partial of Ryujinx's InstEmit, so it reuses the
// translator's private helpers (comparisons, LOP3 truth tables, texture binding, constant/local/shared/global memory)
// and, for attribute I/O and S2R, Ryujinx's own Maxwell emitters on a transcoded 64-bit Maxwell opcode.
// Semantics notes marked "inferred" are read from the compiler's output, not from documentation.
using Nx.Shaders.Ampere;
using Ryujinx.Graphics.Shader.Decoders;
using Ryujinx.Graphics.Shader.IntermediateRepresentation;
using Ryujinx.Graphics.Shader.Translation;
using System;
using System.Collections.Generic;
using System.Linq;
using System.Numerics;
using static Ryujinx.Graphics.Shader.IntermediateRepresentation.OperandHelper;
using static Ryujinx.Graphics.Shader.Instructions.InstEmitHelper;
using static Ryujinx.Graphics.Shader.Instructions.InstEmitAluHelper;
using Op = Nx.Shaders.Ampere.Operand;
using IrOperand = Ryujinx.Graphics.Shader.IntermediateRepresentation.Operand;

namespace Ryujinx.Graphics.Shader.Instructions
{
    sealed class AmpereFrame
    {
        public Dictionary<int, IrOperand> Labels;   // instruction address -> label inside an inlined subroutine
        public IrOperand ReturnLabel;
        public int Depth;
    }

    static partial class InstEmit
    {
        sealed class AmpereSkip : Exception
        {
            public AmpereSkip(string why) : base(why ?? "not handled") { }
        }

        const int UGprBase = 4, UPredBase = 4 + 64;
        // NVN keeps graphics texture and image handles in c[2] (the bank a bound TEX names); Ryujinx calls that the default bank.
        const int AmpereTextureBank = 2;

        static Exception Unsup(string why = null) => new AmpereSkip(why);

        // ---- entry point ------------------------------------------------------------------------------------------

        internal static void EmitAmpere(EmitterContext ctx, AmpereProgram prog, SassInst i, AmpereFrame frame)
        {
            if (i.Pred == 7 && i.PredNeg) return;   // @!PT: never executes
            try
            {
                IrOperand guard = i.Pred == 7 ? null : Guard(ctx, i);
                // Control flow with a condition predicate ("@!P3 BRA P5, target", "BRA.U !UP0, target"): taken when the guard
                // and every condition hold (inferred: the condition is ANDed with the guard).
                if (i.Opcode is "BRA" or "CALL" or "BRX" or "BRXU" or "RET" or "EXIT" or "KILL")
                {
                    foreach (Op c in i.Operands.TakeWhile(IsPredKind))
                    {
                        if (c.Reg == 7 && !c.Not) continue;
                        IrOperand cv = PredSrc(ctx, c);
                        guard = guard == null ? cv : ctx.Add(Instruction.LogicalAnd, Local(), guard, cv);
                    }
                }
                switch (i.Opcode)
                {
                    case "BRA": EmitBranch(ctx, guard, TargetLabel(ctx, BranchTarget(i), frame)); return;
                    case "CALL": EmitCall(ctx, prog, i, guard, frame); return;
                    case "BRX": case "BRXU": EmitBrx(ctx, prog, i, guard, frame); return;
                    case "RET":
                        if (frame == null) throw Unsup("RET outside an inlined subroutine");
                        EmitBranch(ctx, guard, frame.ReturnLabel);
                        return;
                }
                IrOperand skip = null;
                if (guard != null)
                {
                    skip = Label();
                    ctx.BranchIfFalse(skip, guard);
                }
                EmitAmpereOp(ctx, prog, i);
                if (skip != null) ctx.MarkLabel(skip);
                if (prog.Mesh != null) AmpereMesh.AfterInstruction(ctx, i.Opcode, Has(i, "ATTR"), Has(i, "32"));
            }
            catch (AmpereSkip e)
            {
                prog.Fail(i, e.Message);
            }
            catch (Exception e) when (e is ArgumentException or InvalidOperationException or IndexOutOfRangeException or NullReferenceException or FormatException)
            {
                prog.Fail(i, $"{e.GetType().Name} {e.Message}");
            }
        }

        static IrOperand Guard(EmitterContext ctx, SassInst i)
        {
            IrOperand p = i.PredIsUniform ? Register(UPredBase + i.Pred, RegisterType.Flag) : Register(i.Pred, RegisterType.Predicate);
            return i.PredNeg ? ctx.Add(Instruction.LogicalNot, Local(), p) : p;
        }

        static int BranchTarget(SassInst i)
        {
            Op l = i.Operands.FirstOrDefault(o => o.Kind == OperandKind.Label) ?? throw Unsup("no branch target");
            return (int)l.Offset;
        }

        static IrOperand TargetLabel(EmitterContext ctx, int address, AmpereFrame frame)
        {
            if (frame != null)
            {
                if (frame.Labels.TryGetValue(address, out IrOperand l)) return l;
                throw Unsup("branch out of an inlined subroutine");
            }
            return ctx.GetLabel((ulong)address);
        }

        static void EmitBranch(EmitterContext ctx, IrOperand guard, IrOperand label)
        {
            if (guard == null) ctx.Branch(label);
            else ctx.BranchIfTrue(label, guard);
        }

        static void EmitCall(EmitterContext ctx, AmpereProgram prog, SassInst i, IrOperand guard, AmpereFrame frame)
        {
            if (!prog.Subroutines.TryGetValue(i.Address, out var region))
            {
                // CALL.REL.NOINC to code without RET: for one invocation that is a jump (the pushed return is never popped).
                EmitBranch(ctx, guard, TargetLabel(ctx, BranchTarget(i), frame));
                return;
            }
            int depth = frame?.Depth ?? 0;
            if (depth >= 4) throw Unsup("subroutine nesting deeper than 4");
            // The callee returns through RET Rn; the caller loads the return address with "MOV Rx, <CALL + 0x10>".
            int idx = prog.IndexOf[i.Address];
            bool movFound = false;
            for (int k = idx - 1; k >= 0 && k >= idx - 12 && !movFound; k--)
            {
                SassInst x = prog.Insts[k];
                movFound = x.Opcode == "MOV" && x.Operands.Count >= 2 && x.Operands[1].Kind == OperandKind.Imm && (int)x.Operands[1].ImmBits == i.Address + 0x10;
            }
            if (!movFound) throw Unsup("subroutine call without a return address to this call site");

            List<SassInst> body = prog.Insts.Where(x => x.Address >= region.Start && x.Address <= region.End).ToList();
            AmpereFrame inner = new() { Labels = new Dictionary<int, IrOperand>(), ReturnLabel = Label(), Depth = depth + 1 };
            foreach (SassInst x in body) inner.Labels[x.Address] = Label();

            IrOperand skip = null;
            if (guard != null)
            {
                skip = Label();
                ctx.BranchIfFalse(skip, guard);
            }
            foreach (SassInst x in body)
            {
                ctx.MarkLabel(inner.Labels[x.Address]);
                EmitAmpere(ctx, prog, x, inner);
            }
            ctx.MarkLabel(inner.ReturnLabel);
            if (skip != null) ctx.MarkLabel(skip);
        }

        static void EmitBrx(EmitterContext ctx, AmpereProgram prog, SassInst i, IrOperand guard, AmpereFrame frame)
        {
            int[] targets = AmpereTranslator.BrxTargets(prog, i);
            if (targets.Length == 0) throw Unsup("no jump table");
            IrOperand skip = null;
            if (guard != null)
            {
                skip = Label();
                ctx.BranchIfFalse(skip, guard);
            }
            (Op sel, long rel) = AmpereTranslator.IndirectOperand(i);
            int baseAddr = i.Address + 0x10 + (int)rel;
            IrOperand value = ctx.IAdd(Src(ctx, sel), Const(baseAddr));
            for (int k = 0; k < targets.Length - 1; k++)
            {
                ctx.BranchIfTrue(TargetLabel(ctx, targets[k], frame), ctx.ICompareEqual(value, Const(targets[k])));
            }
            ctx.Branch(TargetLabel(ctx, targets[^1], frame));
            if (skip != null) ctx.MarkLabel(skip);
        }

        // ---- operands ---------------------------------------------------------------------------------------------

        static IrOperand Gpr(int r) => r >= 255 ? Const(0) : Register(r, RegisterType.Gpr);
        static IrOperand UGpr(int r) => r >= 63 ? Const(0) : Register(UGprBase + r, RegisterType.Flag);

        static bool HasSuffix(Op o, string s) => (o.Suffixes != null && o.Suffixes.Contains(s)) || (o.Kind == OperandKind.Mem && o.Text != null && System.Text.RegularExpressions.Regex.IsMatch(o.Text, @"R(\d+|Z)" + System.Text.RegularExpressions.Regex.Escape(s) + @"(?![0-9A-Z])"));

        /// Raw 32-bit value of a source operand (no modifiers), `word` = 0/1 for the low/high register of a pair.
        static IrOperand Raw(EmitterContext ctx, Op o, int word = 0)
        {
            switch (o.Kind)
            {
                case OperandKind.Reg: return o.Reg >= 255 ? Const(0) : Gpr(o.Reg + word);
                case OperandKind.UReg: return o.Reg >= 63 ? Const(0) : UGpr(o.Reg + word);
                case OperandKind.Imm:
                case OperandKind.FImm:
                    return word == 0 ? Const((int)(uint)o.ImmBits) : Const(o.Kind == OperandKind.Imm && (long)o.ImmBits < 0 ? -1 : 0);
                case OperandKind.CBuf: return LoadCbuf(ctx, o, word);
                case OperandKind.Pred:
                case OperandKind.UPred:
                    return PredSrc(ctx, o);
            }
            throw Unsup($"operand {o.Kind} {o.Text}");
        }

        static IrOperand LoadCbuf(EmitterContext ctx, Op o, int word)
        {
            long off = o.Offset + word * 4;
            if (!o.BankIsUReg && o.IndexReg < 0)
            {
                if ((off & 3) != 0) throw Unsup("unaligned constant buffer offset");
                return Cbuf(o.Bank, (int)(off >> 2));
            }
            IrOperand slot = o.BankIsUReg ? UGpr(o.Bank) : Const(o.Bank);
            IrOperand addr = Const((int)off);
            if (o.IndexReg >= 0) addr = ctx.IAdd(o.IndexIsUReg ? UGpr(o.IndexReg) : Gpr(o.IndexReg), addr);
            return EmitLoadConstant(ctx, slot, ctx.ShiftRightU32(addr, Const(2)));
        }

        static IrOperand PredSrc(EmitterContext ctx, Op o)
        {
            IrOperand p = o.Reg == 7 ? Const(IrConsts.True)
                : o.Kind == OperandKind.UPred ? Register(UPredBase + o.Reg, RegisterType.Flag) : Register(o.Reg, RegisterType.Predicate);
            return o.Not ? ctx.Add(Instruction.LogicalNot, Local(), p) : p;
        }

        static IrOperand Src(EmitterContext ctx, Op o)
        {
            if (o.Kind is OperandKind.Pred or OperandKind.UPred) return PredSrc(ctx, o);
            IrOperand v = Raw(ctx, o);
            if (o.Kind == OperandKind.Imm || o.Kind == OperandKind.FImm) return v;   // sign already folded in
            if (o.Abs) v = ctx.IAbsolute(v);
            if (o.Neg) v = ctx.INegate(v);
            if (o.Not) v = ctx.BitwiseNot(v);
            return v;
        }

        /// fp32 source of an instruction that may carry .FTZ: a denormal input reads as zero of the same sign. The host flushes
        /// denormals in arithmetic but not in compares or rounding (hwcheck, RTX 3080: FSETP.EQ.FTZ of 0x807fffff and 0 is true,
        /// FRND.FTZ.CEIL / F2I.FTZ.FLOOR of a denormal give 0 / 0 on the GPU and 1 / -1 translated), so those flush explicitly.
        static IrOperand FtzSrc(EmitterContext ctx, SassInst i, Op o)
        {
            IrOperand v = FSrc(ctx, o);
            if (!Has(i, "FTZ") || o.Kind == OperandKind.FImm || (o.Kind == OperandKind.Reg && o.Reg == 255)) return v;
            IrOperand denormal = ctx.FPCompareLess(ctx.FPAbsolute(v), ConstF(1.17549435e-38f));
            return ctx.ConditionalSelect(denormal, ctx.BitwiseAnd(v, Const(unchecked((int)0x80000000))), v);
        }

        static IrOperand FSrc(EmitterContext ctx, Op o)
        {
            if (o.Kind == OperandKind.Imm) throw Unsup("integer immediate in a float operand");
            IrOperand v = Raw(ctx, o);
            if (o.Kind == OperandKind.FImm) return v;
            return ctx.FPAbsNeg(v, o.Abs, o.Neg);
        }

        static void Dst(EmitterContext ctx, Op o, IrOperand v)
        {
            switch (o.Kind)
            {
                case OperandKind.Reg: if (o.Reg < 255) ctx.Copy(Gpr(o.Reg), v); return;
                case OperandKind.UReg: if (o.Reg < 63) ctx.Copy(UGpr(o.Reg), v); return;
                case OperandKind.Pred: if (o.Reg < 7) ctx.Copy(Register(o.Reg, RegisterType.Predicate), v); return;
                case OperandKind.UPred: if (o.Reg < 7) ctx.Copy(Register(UPredBase + o.Reg, RegisterType.Flag), v); return;
            }
            throw Unsup($"destination {o.Kind}");
        }

        static void DstWord(EmitterContext ctx, Op o, int word, IrOperand v)
        {
            if (o.Kind == OperandKind.Reg && o.Reg + word < 255) ctx.Copy(Gpr(o.Reg + word), v);
            else if (o.Kind == OperandKind.UReg && o.Reg + word < 63) ctx.Copy(UGpr(o.Reg + word), v);
            else if (o.Kind is not (OperandKind.Reg or OperandKind.UReg)) throw Unsup($"destination {o.Kind}");
        }

        static bool IsPredKind(Op o) => o.Kind is OperandKind.Pred or OperandKind.UPred;
        static IrOperand Bool(EmitterContext ctx, IrOperand b) => ctx.ConditionalSelect(b, Const(1), Const(0));
        static bool Has(SassInst i, string m) => i.Mods.Contains(m);
        static int IndexOfMod(SassInst i, string m) { for (int k = 0; k < i.Mods.Count; k++) if (i.Mods[k] == m) return k; return -1; }

        // ---- dispatch ---------------------------------------------------------------------------------------------

        static void EmitAmpereOp(EmitterContext ctx, AmpereProgram prog, SassInst i)
        {
            IReadOnlyList<Op> o = i.Operands;
            string op = i.Opcode;
            // Uniform-datapath forms share the semantics of their vector counterparts; operands carry the register file.
            if (op.Length > 1 && op[0] == 'U' && op is "UMOV" or "UIADD3" or "UIMAD" or "USHF" or "ULEA" or "ULOP3" or "USEL" or "UPRMT" or "UPOPC" or "UFLO" or "UISETP" or "UPLOP3" or "UBREV" or "USGXT" or "UIMNMX")
                op = op[1..];
            switch (op)
            {
                // no-ops for a single invocation: scheduling, reconvergence and error barriers
                case "NOP": case "DEPBAR": case "BSSY": case "BSYNC": case "BREAK": case "WARPSYNC": case "ERRBAR": case "YIELD": case "CCTL":
                    return;
                case "EXIT":
                    if (prog.Mesh != null) { ctx.Branch(ctx.GetLabel((ulong)prog.EpilogueAddress)); return; }   // task/mesh: to the end-of-program block
                    if (ctx.PrepareForReturn()) ctx.Return();
                    return;
                case "KILL":
                    ctx.Discard();
                    return;
                case "BAR":
                    if (!Has(i, "SYNC") && !(i.Mods.Count == 0)) throw Unsup();
                    ctx.Barrier();
                    return;
                case "MEMBAR":
                    if (Has(i, "CTA")) ctx.GroupMemoryBarrier(); else ctx.MemoryBarrier();
                    return;

                // ---- moves and selects
                case "MOV": case "R2UR":
                    Dst(ctx, o[0], Src(ctx, o[1]));
                    return;
                case "SEL":
                    Dst(ctx, o[0], ctx.ConditionalSelect(Src(ctx, o[3]), Src(ctx, o[1]), Src(ctx, o[2])));
                    return;
                case "FSEL":
                    Dst(ctx, o[0], ctx.ConditionalSelect(Src(ctx, o[3]), FSrc(ctx, o[1]), FSrc(ctx, o[2])));
                    return;
                case "CS2R":
                    if (o[1].Text != "SRZ" && !o[1].Text.StartsWith("SR_CLOCK")) throw Unsup(o[1].Text);
                    DstWord(ctx, o[0], 0, Const(0));
                    if (!Has(i, "32")) DstWord(ctx, o[0], 1, Const(0));
                    return;
                case "ISBERD" when prog.Mesh != null: EmitIsberdMesh(ctx, prog, i); return;
                case "ISBEWR": EmitIsbewrMesh(ctx, prog, i); return;
                case "ISBERD":
                    // ISBERD Rd, [Ra]: read of the internal stage buffer; Ryujinx (Maxwell Isberd) copies Ra to Rd
                    Dst(ctx, i.Operands[0], i.Operands[1].Kind == OperandKind.Mem ? Gpr(i.Operands[1].Reg) : Src(ctx, i.Operands[1]));
                    return;
                case "LEPC":
                    // The PC for an ATEXIT_PC handler ("LEPC R0; IADD3 R0, R0, off; BMOV.64 ATEXIT_PC, R0"): the handler (OUT.FINAL;
                    // BMOV.64 ATEXIT_PC, 0x0; EXIT) is the end-of-shader work the host does itself, so the address is never used.
                    Dst(ctx, i.Operands[0], Const(0));
                    return;
                case "BMOV" when Has(i, "64") && i.Operands.Count > 0 && i.Operands[0].Text == "ATEXIT_PC":
                    return;   // the exit handler is not run (see LEPC)
                case "S2R": case "S2UR":
                    EmitS2rAmpere(ctx, i);
                    return;
                case "P2R": case "UP2UR": EmitP2rAmpere(ctx, i); return;
                case "R2P": EmitR2pAmpere(ctx, i); return;

                // ---- float
                case "FADD":
                    {
                        IrOperand fa = FSrc(ctx, o[1]), fb = FSrc(ctx, o[2]);
                        IrOperand r = ctx.FPAdd(fa, fb);
                        if (Directed(i))
                        {
                            // The sum in fp64 is exact, or (operand exponents more than 29 apart) a correct rounding of it, so it
                            // orders against r like the exact sum. When it equals r the smaller operand may have been lost
                            // entirely; then the exact error is that operand.
                            IrOperand dr = F64(ctx, r), sum = ctx.FPAdd(F64(ctx, fa), F64(ctx, fb), Instruction.FP64);
                            IrOperand aBig = ctx.FPCompareLess(ctx.FPAbsolute(fb), ctx.FPAbsolute(fa));
                            IrOperand small = ctx.ConditionalSelect(aBig, fb, fa), big = ctx.ConditionalSelect(aBig, fa, fb);
                            IrOperand lost = ctx.BitwiseAnd(ctx.FPCompareEqual(sum, dr, Instruction.FP64), ctx.FPCompareEqual(sum, F64(ctx, big), Instruction.FP64));
                            IrOperand below = ctx.BitwiseOr(ctx.FPCompareLess(sum, dr, Instruction.FP64), ctx.BitwiseAnd(lost, ctx.FPCompareLess(small, ConstF(0))));
                            IrOperand above = ctx.BitwiseOr(ctx.FPCompareLess(dr, sum, Instruction.FP64), ctx.BitwiseAnd(lost, ctx.FPCompareLess(ConstF(0), small)));
                            r = DirectedRound(ctx, i, r, below, above, AllFinite(ctx, Raw(ctx, o[1]), Raw(ctx, o[2])));
                            // by sign bit: denormal operands count as zeros here (hwcheck: FADD.FTZ.RM of two positive denormals is +0)
                            IrOperand plusZeros = ctx.BitwiseAnd(ctx.ICompareLessOrEqual(Const(0), fa), ctx.ICompareLessOrEqual(Const(0), fb));
                            r = RmZero(ctx, i, r, ctx.FPCompareEqual(sum, ConstD(ctx), Instruction.FP64), plusZeros);
                        }
                        if (!Has(i, "FTZ"))
                        {
                            IrOperand[] bits = [FBits(ctx, o[1]), FBits(ctx, o[2])];
                            DstOrSlow(ctx, o[0], FinishF(ctx, i, r), NeedsDenormPath(ctx, r, bits), () => DenormArith(ctx, i, bits, 0));
                            return;
                        }
                        Dst(ctx, o[0], FinishF(ctx, i, r));
                        return;
                    }
                case "FMUL":
                    {
                        IrOperand fa = FSrc(ctx, o[1]), fb = FSrc(ctx, o[2]);
                        float scale = Has(i, "M2") ? 2 : Has(i, "M4") ? 4 : Has(i, "M8") ? 8 : Has(i, "D2") ? 0.5f : Has(i, "D4") ? 0.25f : Has(i, "D8") ? 0.125f : 1;
                        if (scale != 1)
                        {
                            // The scale is part of the one rounding (hwcheck, RTX 3080: FMUL.D2 of a product past FLT_MAX is finite,
                            // FMUL.M2 of a product below FLT_MIN is normal). Scaling an operand by 2^k is exact, so scale the one that
                            // moves toward 1: the smaller magnitude for M2/M4/M8, the larger for D2/D4/D8.
                            IrOperand aSmaller = ctx.FPCompareLess(ctx.FPAbsolute(fa), ctx.FPAbsolute(fb));
                            IrOperand pickA = scale > 1 ? aSmaller : ctx.Add(Instruction.LogicalNot, Local(), aSmaller);
                            IrOperand s = ctx.ConditionalSelect(pickA, fa, fb), t = ctx.ConditionalSelect(pickA, fb, fa);
                            fa = ctx.FPMultiply(s, ConstF(scale));
                            fb = t;
                        }
                        IrOperand r = ctx.FPMultiply(fa, fb);
                        if (Directed(i))
                        {
                            // Which side of r the exact product lies on: the sign of e = fma(a, b, -r) = a*b - r, which is exact in
                            // fp32 (the error of a rounded product is representable) as long as it does not fall below FLT_MIN.
                            // That needs |r| >= 2^-79 (|e| >= |r| * 2^-47); below 2^-78 both operands are scaled by 2^32 and r by 2^64
                            // first, exactly: an operand above 2^96 there means the other is below FLT_MIN, so r is 0 (left alone) and
                            // an infinite e changes nothing. No fp64: FMUL.FTZ.RZ is in 25,000 of OoT's fragment shaders (the x / 2pi
                            // before MUFU.SIN/COS, about 20 in each hot material shader), and the earlier fp64 compare cost five fp64
                            // operations each (1/64 rate on GeForce). hwcheck checks it.
                            IrOperand tiny = ctx.FPCompareLess(ctx.FPAbsolute(r), ConstF(MathF.ScaleB(1f, -78)));
                            IrOperand k = ctx.ConditionalSelect(tiny, ConstF(MathF.ScaleB(1f, 32)), ConstF(1));
                            IrOperand k2 = ctx.ConditionalSelect(tiny, ConstF(MathF.ScaleB(1f, 64)), ConstF(1));
                            IrOperand e = ctx.FPFusedMultiplyAdd(ctx.FPMultiply(fa, k), ctx.FPMultiply(fb, k), ctx.FPNegate(ctx.FPMultiply(r, k2)));
                            r = DirectedRound(ctx, i, r, ctx.FPCompareLess(e, ConstF(0)), ctx.FPCompareLess(ConstF(0), e), AllFinite(ctx, Raw(ctx, o[1]), Raw(ctx, o[2])));
                        }
                        if (!Has(i, "FTZ"))
                        {
                            IrOperand[] bits = [FBits(ctx, o[1]), FBits(ctx, o[2])];
                            DstOrSlow(ctx, o[0], FinishF(ctx, i, r), NeedsDenormPath(ctx, r, bits), () => DenormArith(ctx, i, bits, 1, scale));
                            return;
                        }
                        Dst(ctx, o[0], FinishF(ctx, i, r));
                        return;
                    }
                case "FFMA":
                    {
                        IrOperand fa = FSrc(ctx, o[1]), fb = FSrc(ctx, o[2]), fc = FSrc(ctx, o[3]);
                        IrOperand r = ctx.FPFusedMultiplyAdd(fa, fb, fc);
                        if (Directed(i))
                        {
                            // P = a*b is exact in fp64; P + c in fp64 is a correct rounding of the exact value, so it orders
                            // against r like the exact value. When it equals r, the residual is (P - r) + c for |c| < |r|
                            // (P - r exact) or P + (c - r) otherwise (c - r exact unless c and r are 2^29 apart, where the
                            // fp64 sum is exact and the residual is zero up to that rounding).
                            IrOperand dr = F64(ctx, r), dc = F64(ctx, fc);
                            IrOperand prod = ctx.FPMultiply(F64(ctx, fa), F64(ctx, fb), Instruction.FP64);
                            IrOperand sum = ctx.FPAdd(prod, dc, Instruction.FP64);
                            IrOperand eq = ctx.FPCompareEqual(sum, dr, Instruction.FP64);
                            IrOperand cSmall = ctx.FPCompareLess(ctx.FPAbsolute(fc), ctx.FPAbsolute(r));
                            IrOperand res1 = ctx.FPAdd(ctx.FPSubtract(prod, dr, Instruction.FP64), dc, Instruction.FP64);
                            IrOperand res2 = ctx.FPAdd(prod, ctx.FPSubtract(dc, dr, Instruction.FP64), Instruction.FP64);
                            IrOperand notSmall = ctx.Add(Instruction.LogicalNot, Local(), cSmall);
                            IrOperand resNeg = ctx.BitwiseOr(ctx.BitwiseAnd(cSmall, ctx.FPCompareLess(res1, ConstD(ctx), Instruction.FP64)), ctx.BitwiseAnd(notSmall, ctx.FPCompareLess(res2, ConstD(ctx), Instruction.FP64)));
                            IrOperand resPos = ctx.BitwiseOr(ctx.BitwiseAnd(cSmall, ctx.FPCompareLess(ConstD(ctx), res1, Instruction.FP64)), ctx.BitwiseAnd(notSmall, ctx.FPCompareLess(ConstD(ctx), res2, Instruction.FP64)));
                            IrOperand below = ctx.BitwiseOr(ctx.FPCompareLess(sum, dr, Instruction.FP64), ctx.BitwiseAnd(eq, resNeg));
                            IrOperand above = ctx.BitwiseOr(ctx.FPCompareLess(dr, sum, Instruction.FP64), ctx.BitwiseAnd(eq, resPos));
                            r = DirectedRound(ctx, i, r, below, above, AllFinite(ctx, Raw(ctx, o[1]), Raw(ctx, o[2]), Raw(ctx, o[3])));
                            // exact zero: +0 only when a*b is +0 and c is +0
                            IrOperand prodPlusZero = ctx.Add(Instruction.LogicalNot, Local(), ctx.ICompareLess(ctx.BitwiseExclusiveOr(fa, fb), Const(0)));
                            r = RmZero(ctx, i, r, ctx.FPCompareEqual(sum, ConstD(ctx), Instruction.FP64), ctx.BitwiseAnd(prodPlusZero, ctx.ICompareLessOrEqual(Const(0), fc)));
                        }
                        if (!Has(i, "FTZ"))
                        {
                            IrOperand[] bits = [FBits(ctx, o[1]), FBits(ctx, o[2]), FBits(ctx, o[3])];
                            DstOrSlow(ctx, o[0], FinishF(ctx, i, r), NeedsDenormPath(ctx, r, bits), () => DenormArith(ctx, i, bits, 2));
                            return;
                        }
                        Dst(ctx, o[0], FinishF(ctx, i, r));
                        return;
                    }
                case "FMNMX":
                    {
                        IrOperand a = FSrc(ctx, o[1]), b = FSrc(ctx, o[2]), p = Src(ctx, o[3]);
                        IrOperand r = ctx.ConditionalSelect(p, ctx.FPMinimum(a, b), ctx.FPMaximum(a, b));
                        if (!Has(i, "FTZ"))
                        {
                            IrOperand ba = FBits(ctx, o[1]), bb = FBits(ctx, o[2]);
                            r = ctx.ConditionalSelect(ctx.BitwiseOr(IsDenormBits(ctx, ba), IsDenormBits(ctx, bb)), DenormMinMax(ctx, ba, bb, p), r);
                        }
                        // .NAN: a NaN input gives NaN (canonical 0x7fffffff); .XORSIGN: the result's sign is the XOR of the sources'
                        // signs before |x| (max.xorsign.abs)
                        if (Has(i, "NAN"))
                            r = ctx.ConditionalSelect(ctx.BitwiseOr(ctx.IsNan(a), ctx.IsNan(b)), Const(0x7fffffff), r);
                        if (Has(i, "XORSIGN"))
                        {
                            IrOperand sgn = ctx.BitwiseAnd(ctx.BitwiseExclusiveOr(Raw(ctx, o[1]), Raw(ctx, o[2])), Const(unchecked((int)0x80000000)));
                            r = ctx.BitwiseOr(ctx.BitwiseAnd(r, Const(0x7fffffff)), sgn);
                        }
                        Dst(ctx, o[0], r);
                        return;
                    }
                case "FSETP": EmitFsetpAmpere(ctx, i, setp: true); return;
                case "FSET": EmitFsetpAmpere(ctx, i, setp: false); return;
                case "FRND":
                    {
                        if (Has(i, "F64")) throw Unsup();
                        IrOperand a = FtzSrc(ctx, i, o[1]);
                        Dst(ctx, o[0], Has(i, "FLOOR") ? ctx.FPFloor(a) : Has(i, "CEIL") ? ctx.FPCeiling(a) : Has(i, "TRUNC") ? ctx.FPTruncate(a) : ctx.FPRound(a));
                        return;
                    }
                case "MUFU": EmitMufuAmpere(ctx, i); return;
                case "DADD": case "DMUL": case "DFMA": EmitDArithAmpere(ctx, i); return;
                case "DSETP": EmitDsetpAmpere(ctx, i); return;
                case "F2I": EmitF2iAmpere(ctx, i); return;
                case "I2F": EmitI2fAmpere(ctx, i); return;
                case "F2IP":
                    {
                        // F2IP.U8|S8.F32[.TRUNC][.NTZ][.RELU] Rd, Ra, Rb, Rc: Ra and Rb rounded (RN or toward zero), saturated to 8 bits and packed
                        // like PTX cvt.pack: Rb in byte 0, Ra in byte 1, Rc's low half in bits 16..31 (NaN converts to 0, as F2I's .NTZ).
                        if (Has(i, "H1") || i.Mods.Any(m => m.StartsWith("INVALID"))) throw Unsup("F2IP form");
                        bool s8 = Has(i, "S8");
                        IrOperand Conv(IrOperand v)
                        {
                            v = ctx.ConditionalSelect(ctx.FPCompareEqual(v, v), v, ConstF(0));   // NaN -> 0
                            if (Has(i, "RELU")) v = ctx.FPMaximum(v, ConstF(0));
                            v = Has(i, "TRUNC") ? ctx.FPTruncate(v) : ctx.FPRound(v);
                            v = ctx.FPMinimum(ctx.FPMaximum(v, ConstF(s8 ? -128f : 0f)), ConstF(s8 ? 127f : 255f));
                            return ctx.BitwiseAnd(ctx.FP32ConvertToS32(v), Const(0xff));
                        }
                        IrOperand lo8 = Conv(FSrc(ctx, o[2])), hi8 = Conv(FSrc(ctx, o[1]));
                        IrOperand c16 = ctx.ShiftLeft(Raw(ctx, o[3]), Const(16));
                        Dst(ctx, o[0], ctx.BitwiseOr(ctx.BitwiseOr(lo8, ctx.ShiftLeft(hi8, Const(8))), c16));
                        return;
                    }
                case "I2FP":
                    if (!Has(i, "F32")) throw Unsup();
                    Dst(ctx, o[0], IntToF32(ctx, i, Src(ctx, o[1]), Has(i, "U32")));
                    return;
                case "F2F": EmitF2fAmpere(ctx, i); return;
                case "F2FP": EmitF2fpAmpere(ctx, i); return;
                case "HADD2": case "HMUL2": case "HFMA2": case "HMNMX2": EmitHalfAmpere(ctx, i); return;
                case "HSETP2": case "HSET2": EmitHsetAmpere(ctx, i); return;
                case "BMOV" when Has(i, "32") && i.Operands.Any(x => x.Text == "MACTIVE"):
                    // "BMOV.32 B15, MACTIVE; BMOV.32.PQUAD MACTIVE, B15 ... BMOV.32 MACTIVE, B15": saves the active mask, makes
                    // the whole quad active around SHFL/FSWZADD derivative code, then restores it. The host runs quad helpers
                    // for derivatives itself, so the save and restore are dropped.
                    return;

                // ---- integer
                case "IADD3": EmitIadd3Ampere(ctx, i); return;
                case "IMAD": EmitImadAmpere(ctx, i); return;
                case "LEA": EmitLeaAmpere(ctx, i); return;
                case "SHF": EmitShfAmpere(ctx, i); return;
                case "LOP3": EmitLop3Ampere(ctx, i); return;
                case "PLOP3": EmitPlop3Ampere(ctx, i); return;
                case "ISETP": EmitIsetpAmpere(ctx, i); return;
                case "IMNMX":
                    {
                        IrOperand a = Src(ctx, o[1]), b = Src(ctx, o[2]);
                        bool u = Has(i, "U32");
                        IrOperand min = u ? ctx.IMinimumU32(a, b) : ctx.IMinimumS32(a, b);
                        IrOperand max = u ? ctx.IMaximumU32(a, b) : ctx.IMaximumS32(a, b);
                        Dst(ctx, o[0], ctx.ConditionalSelect(Src(ctx, o[3]), min, max));
                        return;
                    }
                case "IABS": Dst(ctx, o[0], ctx.IAbsolute(Src(ctx, o[1]))); return;
                case "POPC": Dst(ctx, o[0], ctx.BitCount(Src(ctx, o[1]))); return;
                case "BREV": Dst(ctx, o[0], ctx.BitfieldReverse(Src(ctx, o[1]))); return;
                case "FLO":
                    {
                        IrOperand a = Src(ctx, o[1]);
                        // .SH: the shift that brings the found bit to bit 31 (31 - msb), all ones when none: Ryujinx's Maxwell
                        // FLO.SH, FindLSB of the reversed value.
                        if (Has(i, "SH")) Dst(ctx, o[0], ctx.FindLSB(ctx.BitfieldReverse(a)));
                        else Dst(ctx, o[0], Has(i, "U32") ? ctx.FindMSBU32(a) : ctx.FindMSBS32(a));
                        return;
                    }
                case "SGXT":
                    {
                        // SGXT[.W][.U32] Rd, a, n: sign/zero-extend the low n bits of a. n is masked to 0..31 with .W, otherwise
                        // clamped to 32 (unsigned); n = 32 leaves a unchanged (hwcheck, RTX 3080).
                        IrOperand a = Src(ctx, o[1]), bits = Src(ctx, o[2]);
                        bits = Has(i, "W") ? ctx.BitwiseAnd(bits, Const(31)) : ctx.IMinimumU32(bits, Const(32));
                        IrOperand n = ctx.IMinimumU32(bits, Const(31));
                        IrOperand x = Has(i, "U32") ? ctx.BitfieldExtractU32(a, Const(0), n) : ctx.BitfieldExtractS32(a, Const(0), n);
                        Dst(ctx, o[0], ctx.ConditionalSelect(ctx.ICompareEqual(bits, Const(32)), a, x));
                        return;
                    }
                case "PRMT": EmitPrmtAmpere(ctx, i); return;
                case "BMSK": EmitBmskAmpere(ctx, i); return;
                case "I2I": EmitI2iAmpere(ctx, i); return;
                case "IDP": EmitIdpAmpere(ctx, i); return;
                case "VABSDIFF": EmitVabsdiffAmpere(ctx, i); return;

                // ---- constants and memory
                case "LDC": case "ULDC": EmitLdcAmpere(ctx, i); return;
                case "LDS": EmitLdStAmpere(ctx, i, StorageKind.SharedMemory, load: true); return;
                case "STS": EmitLdStAmpere(ctx, i, StorageKind.SharedMemory, load: false); return;
                case "LDL": EmitLdStAmpere(ctx, i, StorageKind.LocalMemory, load: true); return;
                case "STL": EmitLdStAmpere(ctx, i, StorageKind.LocalMemory, load: false); return;
                case "LDG": EmitGlobalAmpere(ctx, i, load: true); return;
                case "STG": EmitGlobalAmpere(ctx, i, load: false); return;
                case "ATOMS": EmitAtomsAmpere(ctx, i); return;
                case "ATOMG": EmitAtomgAmpere(ctx, i); return;
                case "RED": EmitRedAmpere(ctx, i); return;

                // ---- attributes (Ryujinx's Maxwell emitters on transcoded opcodes)
                case "ALD": EmitAldAmpere(ctx, i); return;
                case "AST": EmitAstAmpere(ctx, i); return;
                case "IPA": EmitIpaAmpere(ctx, i); return;
                case "OUT": EmitOutAmpere(ctx, i); return;

                // ---- textures and images
                case "TEX": EmitTexAmpere(ctx, i, fetch: false); return;
                case "TLD": EmitTexAmpere(ctx, i, fetch: true); return;
                case "TLD4": EmitTld4Ampere(ctx, i); return;
                case "TXQ": EmitTxqAmpere(ctx, i); return;
                case "TXD": EmitTxdAmpere(ctx, i); return;
                case "TMML": EmitTmmlAmpere(ctx, i); return;
                case "FSWZADD": EmitFswzaddAmpere(ctx, i); return;
                case "SUST": EmitSustAmpere(ctx, i); return;
                case "SULD": EmitSuldAmpere(ctx, i); return;
                case "SURED": EmitSuredAmpere(ctx, i); return;

                // ---- warp
                case "SHFL": EmitShflAmpere(ctx, i); return;
                case "VOTE": case "VOTEU": EmitVoteAmpere(ctx, i); return;
                case "REDUX": EmitReduxAmpere(ctx, i); return;
                case "MATCH": EmitMatchAmpere(ctx, i); return;

                // ---- tensor cores (AmpereEmit.Tensor.cs)
                case "HMMA": EmitHmmaAmpere(ctx, i); return;
                case "LDSM": EmitLdsmAmpere(ctx, i); return;
                case "MOVM": EmitMovmAmpere(ctx, i); return;
            }
            throw Unsup();
        }

        static IrOperand FinishF(EmitterContext ctx, SassInst i, IrOperand r) => Has(i, "SAT") ? ctx.FPSaturate(r) : r;

        // ---- directed rounding (.RZ/.RM/.RP) ----------------------------------------------------------------------
        // The host rounds to nearest only. The result is the round-to-nearest one moved by one ulp when the exact value lies on
        // the wrong side of it; `below`/`above` say exact < r / exact > r, found with fp64 compares (an fp32 TwoSum
        // did not survive the host compiler: hwcheck still saw round-to-nearest results), except for FMUL, whose error is one
        // fused multiply-add in fp32 (hwcheck, RTX 4080: bit-identical). Overflow to infinity from finite
        // inputs becomes FLT_MAX where the mode says so. Results at zero are left alone (the directed step would give a
        // denormal, which .FTZ flushes anyway); an exact zero sum is -0 under .RM unless both terms are +0. hwcheck, RTX 3080.

        static bool Directed(SassInst i) => Has(i, "RZ") || Has(i, "RM") || Has(i, "RP");

        static IrOperand Finite(EmitterContext ctx, IrOperand v) =>
            ctx.ICompareNotEqual(ctx.BitwiseAnd(v, Const(0x7f800000)), Const(0x7f800000));

        // on the raw source bits (|x| and -x keep the exponent)
        static IrOperand AllFinite(EmitterContext ctx, params IrOperand[] v)
        {
            IrOperand r = Finite(ctx, v[0]);
            for (int k = 1; k < v.Length; k++) r = ctx.BitwiseAnd(r, Finite(ctx, v[k]));
            return r;
        }

        static IrOperand F64(EmitterContext ctx, IrOperand f) => ctx.FP32ConvertToFP64(f);

        static IrOperand ConstD(EmitterContext ctx) => ctx.FP32ConvertToFP64(ConstF(0));

        static IrOperand RmZero(EmitterContext ctx, SassInst i, IrOperand r, IrOperand exactZero, IrOperand plusZeros) =>
            Has(i, "RM") ? ctx.ConditionalSelect(exactZero, ctx.ConditionalSelect(plusZeros, Const(0), Const(unchecked((int)0x80000000))), r) : r;

        static IrOperand DirectedRound(EmitterContext ctx, SassInst i, IrOperand r, IrOperand below, IrOperand above, IrOperand inputsFinite)
        {
            IrOperand Not(IrOperand b) => ctx.Add(Instruction.LogicalNot, Local(), b);
            IrOperand neg = ctx.ICompareLess(r, Const(0));                                   // sign bit
            IrOperand mag = ctx.BitwiseAnd(r, Const(0x7fffffff));
            IrOperand nonzero = ctx.ICompareNotEqual(mag, Const(0));
            IrOperand rFinite = Finite(ctx, r);                                               // inf/NaN results stay
            below = ctx.BitwiseAnd(below, rFinite);
            above = ctx.BitwiseAnd(above, rFinite);
            IrOperand overflow = ctx.BitwiseAnd(ctx.ICompareEqual(mag, Const(0x7f800000)), inputsFinite);
            IrOperand towardZero = ctx.BitwiseOr(ctx.BitwiseAnd(Not(neg), below), ctx.BitwiseAnd(neg, above));
            IrOperand dec, inc;   // magnitude one ulp down / up
            if (Has(i, "RZ"))
            {
                dec = ctx.BitwiseOr(ctx.BitwiseAnd(towardZero, nonzero), overflow);
                inc = Const(IrConsts.False);
            }
            else if (Has(i, "RM"))
            {
                dec = ctx.BitwiseOr(ctx.BitwiseAnd(ctx.BitwiseAnd(Not(neg), below), nonzero), ctx.BitwiseAnd(overflow, Not(neg)));
                inc = ctx.BitwiseAnd(ctx.BitwiseAnd(neg, below), nonzero);
            }
            else
            {
                dec = ctx.BitwiseOr(ctx.BitwiseAnd(ctx.BitwiseAnd(neg, above), nonzero), ctx.BitwiseAnd(overflow, neg));
                inc = ctx.BitwiseAnd(ctx.BitwiseAnd(Not(neg), above), nonzero);
            }
            return ctx.ConditionalSelect(dec, ctx.ISubtract(r, Const(1)), ctx.ConditionalSelect(inc, ctx.IAdd(r, Const(1)), r));
        }

        /// Integer to fp32 with the rounding the instruction names (I2FP, I2F): exact below 2^24; above, the value is cut to
        /// 24 significant bits (exact conversion) and moved up one ulp when the mode rounds the magnitude up.
        static IrOperand IntToF32(EmitterContext ctx, SassInst i, IrOperand v, bool unsigned)
        {
            if (!Directed(i)) return unsigned ? ctx.IConvertU32ToFP32(v) : ctx.IConvertS32ToFP32(v);
            IrOperand neg = unsigned ? Const(IrConsts.False) : ctx.ICompareLess(v, Const(0));
            IrOperand m = unsigned ? v : ctx.IAbsolute(v);                                    // |INT_MIN| = 0x80000000 as u32
            IrOperand msb = ctx.FindMSBU32(m);
            IrOperand shift = ctx.IMaximumS32(ctx.ISubtract(msb, Const(23)), Const(0));
            IrOperand mt = ctx.ShiftLeft(ctx.ShiftRightU32(m, shift), shift);
            IrOperand f = ctx.IConvertU32ToFP32(mt);                                          // exact
            IrOperand inexact = ctx.ICompareNotEqual(m, mt);
            IrOperand up = Has(i, "RP") ? ctx.BitwiseAnd(inexact, ctx.Add(Instruction.LogicalNot, Local(), neg))
                         : Has(i, "RM") ? ctx.BitwiseAnd(inexact, neg) : Const(IrConsts.False);
            f = ctx.ConditionalSelect(up, ctx.IAdd(f, Const(1)), f);
            return ctx.ConditionalSelect(neg, ctx.FPNegate(f), f);
        }

        // ---- comparisons ------------------------------------------------------------------------------------------

        static readonly string[] FCompNames = ["F", "LT", "EQ", "LE", "GT", "NE", "GE", "NUM", "NAN", "LTU", "EQU", "LEU", "GTU", "NEU", "GEU", "T"];
        static readonly string[] ICompNames = ["F", "LT", "EQ", "LE", "GT", "NE", "GE", "T"];

        static BoolOp BopOf(SassInst i) => Has(i, "OR") ? BoolOp.Or : Has(i, "XOR") ? BoolOp.Xor : Has(i, "AND") ? BoolOp.And : throw Unsup("no boolean op");

        static void EmitFsetpAmpere(EmitterContext ctx, SassInst i, bool setp)
        {
            int c = Array.FindIndex(FCompNames, n => i.Mods.Contains(n));
            if (c < 0) throw Unsup("no comparison");
            BoolOp bop = BopOf(i);
            IReadOnlyList<Op> o = i.Operands;
            if (setp)
            {
                // FSETP Pd, Pd2, a, b, Pc
                IrOperand cmp = FCompare(ctx, i, (FComp)c, o[2], o[3]);
                IrOperand pc = Src(ctx, o[4]);
                Dst(ctx, o[0], GetPredLogicalOp(ctx, bop, cmp, pc));
                if (!(o[1].Kind == OperandKind.Pred && o[1].Reg == 7))
                    Dst(ctx, o[1], GetPredLogicalOp(ctx, bop, ctx.Add(Instruction.LogicalNot, Local(), cmp), pc));
            }
            else
            {
                // FSET.BF Rd, a, b, Pc
                IrOperand cmp = FCompare(ctx, i, (FComp)c, o[1], o[2]);
                IrOperand res = GetPredLogicalOp(ctx, bop, cmp, Src(ctx, o[3]));
                Dst(ctx, o[0], Has(i, "BF") ? ctx.ConditionalSelect(res, ConstF(1), ConstF(0)) : ctx.ConditionalSelect(res, Const(-1), Const(0)));
            }
        }

        /// fp32 compare of FSETP/FSET: .FTZ flushes its inputs; without it a denormal keeps its value, so compare in fp64.
        static IrOperand FCompare(EmitterContext ctx, SassInst i, FComp c, Op a, Op b) =>
            Has(i, "FTZ") ? GetFPComparison(ctx, c, FtzSrc(ctx, i, a), FtzSrc(ctx, i, b))
                          : GetFPComparison(ctx, c, ExactF64(ctx, FBits(ctx, a)), ExactF64(ctx, FBits(ctx, b)), Instruction.FP64);

        static void EmitIsetpAmpere(EmitterContext ctx, SassInst i)
        {
            int c = Array.FindIndex(ICompNames, n => i.Mods.Contains(n));
            if (c < 0) throw Unsup("no comparison");
            IReadOnlyList<Op> o = i.Operands;
            IrOperand cmp;
            if (Has(i, "EX"))
            {
                // .EX: the high words of a 64-bit compare; the last operand is the low words' result (same comparison, unsigned):
                // EQ: eq & low, NE: ne | low, LT/LE: lt | (eq & low), GT/GE: gt | (eq & low)
                IrOperand ha = Src(ctx, o[2]), hb = Src(ctx, o[3]), low = Src(ctx, o[5]);
                bool sgn = !Has(i, "U32");
                IrOperand eq = ctx.ICompareEqual(ha, hb);
                cmp = (IComp)c switch
                {
                    IComp.Eq => ctx.BitwiseAnd(eq, low),
                    IComp.Ne => ctx.BitwiseOr(ctx.ICompareNotEqual(ha, hb), low),
                    IComp.Lt or IComp.Le => ctx.BitwiseOr(sgn ? ctx.ICompareLess(ha, hb) : ctx.ICompareLessUnsigned(ha, hb), ctx.BitwiseAnd(eq, low)),
                    IComp.Gt or IComp.Ge => ctx.BitwiseOr(sgn ? ctx.ICompareGreater(ha, hb) : ctx.ICompareGreaterUnsigned(ha, hb), ctx.BitwiseAnd(eq, low)),
                    IComp.T => Const(IrConsts.True),
                    _ => Const(IrConsts.False),
                };
            }
            else cmp = GetIntComparison(ctx, (IComp)c, Src(ctx, o[2]), Src(ctx, o[3]), !Has(i, "U32"));
            BoolOp bop = BopOf(i);
            IrOperand pc = Src(ctx, o[4]);
            Dst(ctx, o[0], GetPredLogicalOp(ctx, bop, cmp, pc));
            if (!(IsPredKind(o[1]) && o[1].Reg == 7))
                Dst(ctx, o[1], GetPredLogicalOp(ctx, bop, ctx.Add(Instruction.LogicalNot, Local(), cmp), pc));
        }

        static void EmitPlop3Ampere(EmitterContext ctx, SassInst i)
        {
            // PLOP3.LUT Pd, Pd2, a, b, c, lut, lut2
            IReadOnlyList<Op> o = i.Operands;
            IrOperand a = ctx.ConditionalSelect(Src(ctx, o[2]), Const(-1), Const(0));
            IrOperand b = ctx.ConditionalSelect(Src(ctx, o[3]), Const(-1), Const(0));
            IrOperand c = ctx.ConditionalSelect(Src(ctx, o[4]), Const(-1), Const(0));
            int lut = (int)o[5].ImmBits & 0xff, lut2 = o.Count > 6 ? (int)o[6].ImmBits & 0xff : 0;
            Dst(ctx, o[0], ctx.ICompareNotEqual(Lop3Expression.GetFromTruthTable(ctx, a, b, c, lut), Const(0)));
            if (!(IsPredKind(o[1]) && o[1].Reg == 7))
                Dst(ctx, o[1], ctx.ICompareNotEqual(Lop3Expression.GetFromTruthTable(ctx, a, b, c, lut2), Const(0)));
        }

        static void EmitLop3Ampere(EmitterContext ctx, SassInst i)
        {
            // LOP3.LUT [Pd,] Rd, a, b, c, lut, Pin: the predicate destination is printed first (decoder: bit 81 when not PT) and is
            // set when the result is non-zero (hwcheck, RTX 3080). Only Pin = !PT is seen (the compiler's and NAK's encoding).
            if (!Has(i, "LUT")) throw Unsup();
            IReadOnlyList<Op> o = i.Operands;
            bool pd = IsPredKind(o[0]);
            int s = pd ? 2 : 1;
            Op pin = o[s + 4];
            if (pd && !(IsPredKind(pin) && pin.Reg == 7 && pin.Not)) throw Unsup("predicate input");
            IrOperand r = Lop3Expression.GetFromTruthTable(ctx, Src(ctx, o[s]), Src(ctx, o[s + 1]), Src(ctx, o[s + 2]), (int)o[s + 3].ImmBits & 0xff);
            Dst(ctx, o[s - 1], r);
            if (pd) Dst(ctx, o[0], ctx.ICompareNotEqual(r, Const(0)));
        }

        // ---- integer arithmetic -----------------------------------------------------------------------------------

        static void EmitIadd3Ampere(EmitterContext ctx, SassInst i)
        {
            // IADD3[.X] Rd, [Pco0, [Pco1,]] a, b, c [, Pci0, Pci1]
            IReadOnlyList<Op> o = i.Operands;
            int k = 1;
            List<Op> carryOut = new();
            while (k < o.Count && IsPredKind(o[k])) carryOut.Add(o[k++]);
            IrOperand a = Src(ctx, o[k]), b = Src(ctx, o[k + 1]), c = Src(ctx, o[k + 2]);
            IrOperand s1 = ctx.IAdd(a, b);
            IrOperand carries = Bool(ctx, ctx.ICompareLessUnsigned(s1, a));
            IrOperand s2 = ctx.IAdd(s1, c);
            carries = ctx.IAdd(carries, Bool(ctx, ctx.ICompareLessUnsigned(s2, s1)));
            if (Has(i, "X"))
            {
                for (int n = k + 3; n < o.Count; n++)
                {
                    if (!IsPredKind(o[n])) continue;
                    IrOperand prev = s2;
                    s2 = ctx.IAdd(s2, Bool(ctx, Src(ctx, o[n])));
                    carries = ctx.IAdd(carries, Bool(ctx, ctx.ICompareLessUnsigned(s2, prev)));
                }
            }
            Dst(ctx, o[0], s2);
            if (carryOut.Count > 0) Dst(ctx, carryOut[0], ctx.ICompareNotEqual(ctx.BitwiseAnd(carries, Const(1)), Const(0)));
            if (carryOut.Count > 1) Dst(ctx, carryOut[1], ctx.ICompareNotEqual(ctx.BitwiseAnd(carries, Const(2)), Const(0)));
        }

        static void EmitImadAmpere(EmitterContext ctx, SassInst i)
        {
            // IMAD[.MOV|.SHL|.IADD|.HI|.WIDE|.X][.U32] Rd, [Pco,] a, b, c [, Pci]
            IReadOnlyList<Op> o = i.Operands;
            int k = 1;
            Op co = null;
            if (IsPredKind(o[1])) { co = o[1]; k = 2; }
            IrOperand a = Src(ctx, o[k]), b = Src(ctx, o[k + 1]);
            bool unsigned = Has(i, "U32");
            if (Has(i, "WIDE"))
            {
                Op cop = o[k + 2];
                IrOperand lo = ctx.IMultiply(a, b);
                IrOperand hi = unsigned ? ctx.MultiplyHighU32(a, b) : ctx.MultiplyHighS32(a, b);
                IrOperand clo = Raw(ctx, cop, 0), chi = Raw(ctx, cop, 1);
                if (cop.Neg || cop.Not || cop.Abs) throw Unsup("modified 64-bit addend");
                IrOperand rlo = ctx.IAdd(lo, clo);
                IrOperand carry = Bool(ctx, ctx.ICompareLessUnsigned(rlo, lo));
                IrOperand rhi = ctx.IAdd(ctx.IAdd(hi, chi), carry);
                DstWord(ctx, o[0], 0, rlo);
                DstWord(ctx, o[0], 1, rhi);
                if (co != null) throw Unsup("carry out");
                return;
            }
            IrOperand c, prod;
            if (Has(i, "HI"))
            {
                // IMAD.HI: high word of a*b + {Rc+1:Rc} (the addend is a 64-bit register pair; hwcheck, RTX 3080)
                Op cop = o[k + 2];
                if (!(cop.Kind == OperandKind.Reg && !cop.Neg && !cop.Not && !cop.Abs)) throw Unsup("IMAD.HI addend not a register pair");
                IrOperand clo = Raw(ctx, cop, 0);
                c = Raw(ctx, cop, 1);
                IrOperand plo = ctx.IMultiply(a, b);
                IrOperand carry = Bool(ctx, ctx.ICompareLessUnsigned(ctx.IAdd(plo, clo), plo));
                prod = ctx.IAdd(unsigned ? ctx.MultiplyHighU32(a, b) : ctx.MultiplyHighS32(a, b), carry);
            }
            else
            {
                c = Src(ctx, o[k + 2]);
                prod = ctx.IMultiply(a, b);
            }
            IrOperand r = ctx.IAdd(prod, c);
            IrOperand carries = co != null ? Bool(ctx, ctx.ICompareLessUnsigned(r, prod)) : null;
            if (Has(i, "X"))
            {
                for (int n = k + 3; n < o.Count; n++)
                {
                    if (!IsPredKind(o[n])) continue;
                    IrOperand prev = r;
                    r = ctx.IAdd(r, Bool(ctx, Src(ctx, o[n])));
                    if (carries != null) carries = ctx.IAdd(carries, Bool(ctx, ctx.ICompareLessUnsigned(r, prev)));
                }
            }
            Dst(ctx, o[0], r);
            if (co != null) Dst(ctx, co, ctx.ICompareNotEqual(carries, Const(0)));
        }

        static void EmitLeaAmpere(EmitterContext ctx, SassInst i)
        {
            // LEA Rd, [Pco,] a, b, shift [, Pci]       = (a << shift) + b
            // LEA.HI Rd, [Pco,] a, b, c, shift [, Pci] = high word of ({c:a} << shift) + b
            // LEA.HI.SX32 Rd, [Pco,] a, b, shift [, Pci] = the same with c = the sign of a (a sign-extended to 64 bits), no c operand
            IReadOnlyList<Op> o = i.Operands;
            int k = 1;
            Op co = null;
            if (IsPredKind(o[1])) { co = o[1]; k = 2; }
            IrOperand a = Src(ctx, o[k]), b = Src(ctx, o[k + 1]);
            bool hi = Has(i, "HI"), sx = hi && Has(i, "SX32"), hasC = hi && !sx;
            Op shOp = o[hasC ? k + 3 : k + 2];
            if (shOp.Kind != OperandKind.Imm) throw Unsup("register shift");
            int sh = (int)shOp.ImmBits & 31;
            IrOperand shifted;
            if (hi)
            {
                IrOperand c = sx ? ctx.ShiftRightS32(a, Const(31)) : Src(ctx, o[k + 2]);
                shifted = sh == 0 ? c : ctx.BitwiseOr(ctx.ShiftLeft(c, Const(sh)), ctx.ShiftRightU32(a, Const(32 - sh)));
            }
            else
            {
                shifted = ctx.ShiftLeft(a, Const(sh));
            }
            IrOperand r = ctx.IAdd(shifted, b);
            IrOperand carries = co != null ? Bool(ctx, ctx.ICompareLessUnsigned(r, b)) : null;
            if (Has(i, "X"))
            {
                for (int n = (hasC ? k + 4 : k + 3); n < o.Count; n++)
                {
                    if (!IsPredKind(o[n])) continue;
                    IrOperand prev = r;
                    r = ctx.IAdd(r, Bool(ctx, Src(ctx, o[n])));
                    if (carries != null) carries = ctx.IAdd(carries, Bool(ctx, ctx.ICompareLessUnsigned(r, prev)));
                }
            }
            Dst(ctx, o[0], r);
            if (co != null) Dst(ctx, co, ctx.ICompareNotEqual(carries, Const(0)));
        }

        static void EmitShfAmpere(EmitterContext ctx, SassInst i)
        {
            // SHF.L|R.U32|S32[.HI][.W] Rd, lo, shift, hi: funnel shift of the 64-bit {hi:lo}.
            IReadOnlyList<Op> o = i.Operands;
            if (Has(i, "U64") || Has(i, "S64")) { EmitShf64Ampere(ctx, i); return; }
            IrOperand lo = Src(ctx, o[1]), hiw = Src(ctx, o[3]);
            bool left = Has(i, "L"), high = Has(i, "HI"), signed = Has(i, "S32");
            Op shOp = o[2];
            IrOperand r;
            if (shOp.Kind == OperandKind.Imm)
            {
                int s = (int)shOp.ImmBits;
                s = Has(i, "W") ? s & 31 : Math.Min(s, 32);
                if (left)
                {
                    r = high ? (s == 0 ? hiw : s == 32 ? lo : ctx.BitwiseOr(ctx.ShiftLeft(hiw, Const(s)), ctx.ShiftRightU32(lo, Const(32 - s))))
                             : (s == 32 ? Const(0) : ctx.ShiftLeft(lo, Const(s)));
                }
                else if (high)
                {
                    r = s == 32 ? (signed ? ctx.ShiftRightS32(hiw, Const(31)) : Const(0)) : signed ? ctx.ShiftRightS32(hiw, Const(s)) : ctx.ShiftRightU32(hiw, Const(s));
                }
                else
                {
                    r = s == 0 ? lo : s == 32 ? hiw : ctx.BitwiseOr(ctx.ShiftRightU32(lo, Const(s)), ctx.ShiftLeft(hiw, Const(32 - s)));
                }
            }
            else
            {
                // Register shift: .W wraps the amount (& 31); otherwise it is clamped to 32 as an unsigned value, like the
                // immediate form (hwcheck, RTX 3080: amounts >= 32, including ones with bit 31 set, shift by exactly 32).
                IrOperand s = Has(i, "W") ? ctx.BitwiseAnd(Src(ctx, shOp), Const(31)) : ctx.IMinimumU32(Src(ctx, shOp), Const(32));
                IrOperand zero = ctx.ICompareEqual(s, Const(0));
                IrOperand full = ctx.ICompareEqual(s, Const(32));
                IrOperand s31 = ctx.BitwiseAnd(s, Const(31));
                IrOperand inv = ctx.BitwiseAnd(ctx.ISubtract(Const(32), s), Const(31));
                if (left)
                {
                    r = high ? ctx.ConditionalSelect(zero, hiw, ctx.ConditionalSelect(full, lo, ctx.BitwiseOr(ctx.ShiftLeft(hiw, s31), ctx.ShiftRightU32(lo, inv))))
                             : ctx.ConditionalSelect(full, Const(0), ctx.ShiftLeft(lo, s31));
                }
                else if (high)
                {
                    r = signed ? ctx.ShiftRightS32(hiw, ctx.IMinimumU32(s, Const(31)))
                               : ctx.ConditionalSelect(full, Const(0), ctx.ShiftRightU32(hiw, s31));
                }
                else
                {
                    r = ctx.ConditionalSelect(zero, lo, ctx.ConditionalSelect(full, hiw, ctx.BitwiseOr(ctx.ShiftRightU32(lo, s31), ctx.ShiftLeft(hiw, inv))));
                }
            }
            Dst(ctx, o[0], r);
        }

        static void EmitShf64Ampere(EmitterContext ctx, SassInst i)
        {
            // SHF.L|R.U64|S64[.HI][.W] Rd, lo, shift, hi: a 64-bit shift of {hi:lo}; Rd = its low word, or with .HI its high word.
            // The amount is masked to 0..63 with .W, otherwise clamped to 64 (inferred from the 32-bit forms: & 31 / clamp 32).
            IReadOnlyList<Op> o = i.Operands;
            IrOperand lo = Src(ctx, o[1]), hi = Src(ctx, o[3]);
            bool left = Has(i, "L"), high = Has(i, "HI"), signed = Has(i, "S64");
            IrOperand s = Has(i, "W") ? ctx.BitwiseAnd(Src(ctx, o[2]), Const(63)) : ctx.IMinimumU32(Src(ctx, o[2]), Const(64));
            IrOperand s31 = ctx.BitwiseAnd(s, Const(31)), inv = ctx.BitwiseAnd(ctx.ISubtract(Const(32), s31), Const(31));
            IrOperand zero = ctx.ICompareEqual(s31, Const(0));
            IrOperand ge32 = ctx.ICompareGreaterOrEqualUnsigned(s, Const(32)), ge64 = ctx.ICompareGreaterOrEqualUnsigned(s, Const(64));
            IrOperand rlo, rhi;
            if (left)
            {
                // s < 32: hi' = hi << s | lo >> (32 - s), lo' = lo << s; 32 <= s < 64: hi' = lo << (s - 32), lo' = 0
                IrOperand h1 = ctx.ConditionalSelect(zero, hi, ctx.BitwiseOr(ctx.ShiftLeft(hi, s31), ctx.ShiftRightU32(lo, inv)));
                IrOperand l1 = ctx.ShiftLeft(lo, s31);
                rhi = ctx.ConditionalSelect(ge64, Const(0), ctx.ConditionalSelect(ge32, ctx.ShiftLeft(lo, s31), h1));
                rlo = ctx.ConditionalSelect(ge32, Const(0), l1);
            }
            else
            {
                IrOperand fill = signed ? ctx.ShiftRightS32(hi, Const(31)) : Const(0);
                IrOperand l1 = ctx.ConditionalSelect(zero, lo, ctx.BitwiseOr(ctx.ShiftRightU32(lo, s31), ctx.ShiftLeft(hi, inv)));
                IrOperand h1 = signed ? ctx.ShiftRightS32(hi, s31) : ctx.ShiftRightU32(hi, s31);
                rlo = ctx.ConditionalSelect(ge64, fill, ctx.ConditionalSelect(ge32, h1, l1));
                rhi = ctx.ConditionalSelect(ge32, fill, h1);
            }
            Dst(ctx, o[0], high ? rhi : rlo);
        }

        static void EmitBmskAmpere(EmitterContext ctx, SassInst i)
        {
            // BMSK[.W] Rd, pos, width: ((1 << width) - 1) << pos, bits past 31 dropped. Without .W both are clamped to 32 (unsigned),
            // with .W both are taken mod 32 (PTX bmsk.clamp / bmsk.wrap).
            IReadOnlyList<Op> o = i.Operands;
            IrOperand pos = Src(ctx, o[1]), w = Src(ctx, o[2]);
            if (Has(i, "W")) { pos = ctx.BitwiseAnd(pos, Const(31)); w = ctx.BitwiseAnd(w, Const(31)); }
            else { pos = ctx.IMinimumU32(pos, Const(32)); w = ctx.IMinimumU32(w, Const(32)); }
            IrOperand ones = ctx.ConditionalSelect(ctx.ICompareEqual(w, Const(32)), Const(-1), ctx.ISubtract(ctx.ShiftLeft(Const(1), ctx.BitwiseAnd(w, Const(31))), Const(1)));
            Dst(ctx, o[0], ctx.ConditionalSelect(ctx.ICompareEqual(pos, Const(32)), Const(0), ctx.ShiftLeft(ones, ctx.BitwiseAnd(pos, Const(31)))));
        }

        static void EmitI2iAmpere(EmitterContext ctx, SassInst i)
        {
            // I2I.{U8,S8,U16,S16}.S32.SAT Rd, b: the signed source clamped to the destination range (signed results sign-extended)
            IReadOnlyList<Op> o = i.Operands;
            (int lo, int hi) = Has(i, "U8") ? (0, 255) : Has(i, "S8") ? (-128, 127) : Has(i, "U16") ? (0, 65535) : (-32768, 32767);
            Dst(ctx, o[0], ctx.IMinimumS32(ctx.IMaximumS32(Src(ctx, o[1]), Const(lo)), Const(hi)));
        }

        static void EmitIdpAmpere(EmitterContext ctx, SassInst i)
        {
            // IDP.4A.{U8,S8}.{U8,S8} Rd, a, b, c: c + sum of a.byte[k] * b.byte[k]; IDP.2A.{LO,HI}.{U16,S16}.{U8,S8}: c + a.half[k] *
            // b.byte[k] (bytes 0..1 for LO, 2..3 for HI). The second type is b's, the first a's (PTX dp4a / dp2a).
            IReadOnlyList<Op> o = i.Operands;
            IrOperand a = Src(ctx, o[1]), b = Src(ctx, o[2]), acc = Src(ctx, o[3]);
            bool two = Has(i, "2A");
            bool aSigned = i.Mods.Count > 2 && i.Mods[two ? 2 : 1].StartsWith("S"), bSigned = i.Mods[^1] == "S8";
            int bBase = two && Has(i, "HI") ? 2 : 0;
            for (int k = 0; k < (two ? 2 : 4); k++)
            {
                int aw = two ? 16 : 8;
                IrOperand ea = aSigned ? ctx.BitfieldExtractS32(a, Const(k * aw), Const(aw)) : ctx.BitfieldExtractU32(a, Const(k * aw), Const(aw));
                IrOperand eb = bSigned ? ctx.BitfieldExtractS32(b, Const((bBase + k) * 8), Const(8)) : ctx.BitfieldExtractU32(b, Const((bBase + k) * 8), Const(8));
                acc = ctx.IAdd(acc, ctx.IMultiply(ea, eb));
            }
            Dst(ctx, o[0], acc);
        }

        static void EmitVabsdiffAmpere(EmitterContext ctx, SassInst i)
        {
            // VABSDIFF[.U32] Rd, a, b, c: |a - b| + c (PTX sad). The predicate-destination form is not known.
            IReadOnlyList<Op> o = i.Operands;
            if (i.DstCount != 1) throw Unsup("VABSDIFF with a predicate destination");
            IrOperand a = Src(ctx, o[1]), b = Src(ctx, o[2]);
            bool u = Has(i, "U32");
            IrOperand max = u ? ctx.IMaximumU32(a, b) : ctx.IMaximumS32(a, b), min = u ? ctx.IMinimumU32(a, b) : ctx.IMinimumS32(a, b);
            Dst(ctx, o[0], ctx.IAdd(ctx.ISubtract(max, min), Src(ctx, o[3])));
        }

        static void EmitPrmtAmpere(EmitterContext ctx, SassInst i)
        {
            // PRMT[.mode] Rd, a, sel, b: byte k of the result = byte n_k of {b:a}. Default mode: n_k = sel nibble k & 7, nibble bit 3
            // replicates the byte's sign. Modes (PTX prmt, s = sel & 3): F4E n_k = s + k, B4E s - k (mod 8), RC8 s, ECL max(k, s),
            // ECR min(k, s), RC16 2 (s & 1) + (k & 1).
            IReadOnlyList<Op> o = i.Operands;
            string mode = i.Mods.Count == 0 ? null : i.Mods[0];
            if (mode is not (null or "F4E" or "B4E" or "RC8" or "ECL" or "ECR" or "RC16")) throw Unsup("PRMT mode");
            if (mode != null || o[2].Kind != OperandKind.Imm)
            {
                IrOperand ra = Src(ctx, o[1]), rb = Src(ctx, o[3]), selv = Src(ctx, o[2]);
                IrOperand s = ctx.BitwiseAnd(selv, Const(3));
                IrOperand res = Const(0);
                for (int k = 0; k < 4; k++)
                {
                    IrOperand n, signRep = null;
                    switch (mode)
                    {
                        case null:
                            IrOperand nib = ctx.BitwiseAnd(ctx.ShiftRightU32(selv, Const(k * 4)), Const(0xf));
                            n = ctx.BitwiseAnd(nib, Const(7));
                            signRep = ctx.ICompareNotEqual(ctx.BitwiseAnd(nib, Const(8)), Const(0));
                            break;
                        case "F4E": n = ctx.BitwiseAnd(ctx.IAdd(s, Const(k)), Const(7)); break;
                        case "B4E": n = ctx.BitwiseAnd(ctx.ISubtract(s, Const(k)), Const(7)); break;
                        case "RC8": n = s; break;
                        case "ECL": n = ctx.IMaximumU32(s, Const(k)); break;
                        case "ECR": n = ctx.IMinimumU32(s, Const(k)); break;
                        default: n = ctx.IAdd(ctx.ShiftLeft(ctx.BitwiseAnd(s, Const(1)), Const(1)), Const(k & 1)); break;
                    }
                    IrOperand word = ctx.ConditionalSelect(ctx.ICompareNotEqual(ctx.BitwiseAnd(n, Const(4)), Const(0)), rb, ra);
                    IrOperand v = ctx.BitfieldExtractU32(word, ctx.ShiftLeft(ctx.BitwiseAnd(n, Const(3)), Const(3)), Const(8));
                    if (signRep != null)
                        v = ctx.ConditionalSelect(signRep, ctx.BitwiseAnd(ctx.ShiftRightS32(ctx.ShiftLeft(v, Const(24)), Const(31)), Const(0xff)), v);
                    res = ctx.BitwiseOr(res, k == 0 ? v : ctx.ShiftLeft(v, Const(k * 8)));
                }
                Dst(ctx, o[0], res);
                return;
            }
            int sel = (int)o[2].ImmBits;
            IrOperand a = Src(ctx, o[1]), b = Src(ctx, o[3]);
            IrOperand r = Const(0);
            for (int k = 0; k < 4; k++)
            {
                int nib = (sel >> (k * 4)) & 0xf;
                IrOperand src = (nib & 4) == 0 ? a : b;
                IrOperand v = ctx.BitfieldExtractU32(src, Const((nib & 3) * 8), Const(8));
                if ((nib & 8) != 0) v = ctx.BitwiseAnd(ctx.ShiftRightS32(ctx.ShiftLeft(v, Const(24)), Const(31)), Const(0xff));
                r = ctx.BitwiseOr(r, k == 0 ? v : ctx.ShiftLeft(v, Const(k * 8)));
            }
            Dst(ctx, o[0], r);
        }

        static void EmitP2rAmpere(EmitterContext ctx, SassInst i)
        {
            // P2R Rd, PR, a, mask: Rd = (a & ~mask) | (predicate bits & mask); .B1-.B3 select a byte.
            IReadOnlyList<Op> o = i.Operands;
            bool uni = o[1].Text == "UPR";   // UP2UR: the uniform predicates
            if (o[1].Text is not ("PR" or "UPR") || o[3].Kind != OperandKind.Imm) throw Unsup();
            int shift = Has(i, "B1") ? 8 : Has(i, "B2") ? 16 : Has(i, "B3") ? 24 : 0;
            int mask = ((int)o[3].ImmBits & 0x7f) << shift;
            IrOperand r = ctx.BitwiseAnd(Src(ctx, o[2]), Const(~mask));
            for (int p = 0; p < 7; p++)
            {
                if ((mask & (1 << (p + shift))) == 0) continue;
                IrOperand pr = uni ? Register(UPredBase + p, RegisterType.Flag) : Register(p, RegisterType.Predicate);
                r = ctx.BitwiseOr(r, ctx.ConditionalSelect(pr, Const(1 << (p + shift)), Const(0)));
            }
            Dst(ctx, o[0], r);
        }

        static void EmitR2pAmpere(EmitterContext ctx, SassInst i)
        {
            // R2P PR, a[.B1-.B3], mask: the byte select is a suffix of the source (R2P PR, R6.reuse.B1, 0x70), not an instruction modifier
            IReadOnlyList<Op> o = i.Operands;
            if (o[0].Text != "PR" || o[2].Kind != OperandKind.Imm) throw Unsup();
            int shift = 0;
            if (o[1].Suffixes != null)
                foreach (string s in o[1].Suffixes)
                    shift = s.EndsWith(".B1") ? 8 : s.EndsWith(".B2") ? 16 : s.EndsWith(".B3") ? 24 : shift;
            int mask = (int)o[2].ImmBits & 0x7f;
            IrOperand a = Src(ctx, o[1]);
            for (int p = 0; p < 7; p++)
            {
                if ((mask & (1 << p)) == 0) continue;
                ctx.Copy(Register(p, RegisterType.Predicate), ctx.ICompareNotEqual(ctx.BitwiseAnd(a, Const(1 << (p + shift))), Const(0)));
            }
        }

        // ---- conversions and special functions --------------------------------------------------------------------

        /// One lane of a 16-bit source: .H1 = high half, otherwise low.
        static IrOperand HalfLane(EmitterContext ctx, Op o)
        {
            if (o.Kind == OperandKind.FImm) return Raw(ctx, o);
            IrOperand v = Raw(ctx, o);
            v = HasSuffix(o, ".H1") ? ctx.UnpackHalf2x16High(v) : ctx.UnpackHalf2x16Low(v);
            return ctx.FPAbsNeg(v, o.Abs, o.Neg);
        }

        const float TwoPi = 6.28318530717958647692f;

        static void EmitMufuAmpere(EmitterContext ctx, SassInst i)
        {
            IReadOnlyList<Op> o = i.Operands;
            bool f16 = Has(i, "F16");
            IrOperand a = f16 ? HalfLane(ctx, o[1]) : FSrc(ctx, o[1]);
            IrOperand r;
            if (Has(i, "RCP")) r = ctx.FPReciprocal(a);
            else if (Has(i, "RSQ")) r = ctx.FPReciprocalSquareRoot(a);
            else if (Has(i, "SQRT")) r = ctx.FPSquareRoot(a);
            else if (Has(i, "EX2")) r = ctx.FPExponentB2(a);
            else if (Has(i, "LG2")) r = ctx.FPLogarithmB2(a);
            // Volta+ MUFU.SIN/COS take the angle in turns (sin(2*pi*x)): there is no RRO, the compiler emits
            // FMUL.RZ x, 1/(2*pi) before them (Mesa NAK builder.rs fsin/fcos, sm >= 70). Maxwell's MUFU took radians after RRO.
            // The hardware reduces the angle in turns: whole turns are dropped first (hwcheck, RTX 3080: MUFU.COS of -808768.375 is
            // cos(0.375 turns)); x - round(x) is exact in fp32, so scale only the fraction.
            else if (Has(i, "SIN")) r = ctx.FPSine(ctx.FPMultiply(ctx.FPAdd(a, ctx.FPNegate(ctx.FPRound(a))), ConstF(TwoPi)));
            else if (Has(i, "COS")) r = ctx.FPCosine(ctx.FPMultiply(ctx.FPAdd(a, ctx.FPNegate(ctx.FPRound(a))), ConstF(TwoPi)));
            else if (Has(i, "TANH"))
            {
                // tanh x = 1 - 2 / (e^2x + 1); below |x| = 1/16 the series x - x^3/3 + 2x^5/15 (no cancellation, keeps -0)
                IrOperand e = ctx.FPExponentB2(ctx.FPMultiply(a, ConstF(2.88539008177792681f)));   // 2 / ln 2
                IrOperand big = ctx.FPSubtract(ConstF(1), ctx.FPDivide(ConstF(2), ctx.FPAdd(e, ConstF(1))));
                IrOperand x2 = ctx.FPMultiply(a, a);
                IrOperand poly = ctx.FPMultiply(a, ctx.FPFusedMultiplyAdd(x2, ctx.FPFusedMultiplyAdd(x2, ConstF(2f / 15), ConstF(-1f / 3)), ConstF(1)));
                r = ctx.ConditionalSelect(ctx.FPCompareLess(ctx.FPAbsolute(a), ConstF(0.0625f)), poly, big);
                r = ctx.ConditionalSelect(ctx.IsNan(a), a, r);
            }
            else throw Unsup();
            if (f16) r = ctx.PackHalf2x16(r, ConstF(0));   // inferred: the F16 result is in the low half
            Dst(ctx, o[0], r);
        }

        static void EmitF2iAmpere(EmitterContext ctx, SassInst i)
        {
            if (Has(i, "F64") || Has(i, "U64") || Has(i, "S64")) { EmitF2i64Ampere(ctx, i); return; }
            IReadOnlyList<Op> o = i.Operands;
            IrOperand src = Has(i, "F16") ? HalfLane(ctx, o[1]) : FtzSrc(ctx, i, o[1]);
            src = Has(i, "TRUNC") ? ctx.FPTruncate(src) : Has(i, "FLOOR") ? ctx.FPFloor(src) : Has(i, "CEIL") ? ctx.FPCeiling(src) : ctx.FPRound(src);
            bool unsigned = Has(i, "U32") || Has(i, "U16") || Has(i, "U8");
            if (unsigned) src = ctx.FPMaximum(src, ConstF(0));
            IrOperand r = unsigned ? ctx.FP32ConvertToU32(src) : ctx.FP32ConvertToS32(src);
            if (Has(i, "U16")) r = ctx.IClampU32(r, Const(0), Const(0xffff));
            else if (Has(i, "U8")) r = ctx.IClampU32(r, Const(0), Const(0xff));
            else if (Has(i, "S16")) r = ctx.IClampS32(r, Const(short.MinValue), Const(short.MaxValue));
            else if (Has(i, "S8")) r = ctx.IClampS32(r, Const(sbyte.MinValue), Const(sbyte.MaxValue));
            Dst(ctx, o[0], r);
        }

        static void EmitI2fAmpere(EmitterContext ctx, SassInst i)
        {
            if (Has(i, "F64") || Has(i, "U64") || Has(i, "S64")) { EmitI2f64Ampere(ctx, i); return; }
            if (Has(i, "F16")) throw Unsup();
            IReadOnlyList<Op> o = i.Operands;
            IrOperand v = Raw(ctx, o[1]);
            bool unsigned = Has(i, "U8") || Has(i, "U16") || Has(i, "U32");
            int bits = Has(i, "U8") || Has(i, "S8") ? 8 : Has(i, "U16") || Has(i, "S16") ? 16 : 32;
            int byteSel = HasSuffix(o[1], ".B1") ? 1 : HasSuffix(o[1], ".B2") ? 2 : HasSuffix(o[1], ".B3") ? 3 : HasSuffix(o[1], ".H1") ? 2 : 0;
            if (bits < 32 || byteSel != 0)
                v = unsigned ? ctx.BitfieldExtractU32(v, Const(byteSel * 8), Const(bits)) : ctx.BitfieldExtractS32(v, Const(byteSel * 8), Const(bits));
            if (o[1].Abs) v = ctx.IAbsolute(v);
            if (o[1].Neg) v = ctx.INegate(v);
            Dst(ctx, o[0], unsigned ? ctx.IConvertU32ToFP32(v) : ctx.IConvertS32ToFP32(v));
        }

        static void EmitF2fAmpere(EmitterContext ctx, SassInst i)
        {
            IReadOnlyList<Op> o = i.Operands;
            int f16 = IndexOfMod(i, "F16"), f32 = IndexOfMod(i, "F32"), f64 = IndexOfMod(i, "F64");
            if (f64 >= 0 && f32 >= 0) { EmitF2f64Ampere(ctx, i, toF64: f64 < f32); return; }
            if (f16 < 0 || f32 < 0 || f64 >= 0) throw Unsup();
            if (f16 < f32)
            {
                // F2F.F16.F32: f32 -> f16 in the low half
                Dst(ctx, o[0], ctx.PackHalf2x16(FSrc(ctx, o[1]), ConstF(0)));
            }
            else
            {
                Dst(ctx, o[0], HalfLane(ctx, o[1]));
            }
        }

        static void EmitF2fpAmpere(EmitterContext ctx, SassInst i)
        {
            IReadOnlyList<Op> o = i.Operands;
            if (Has(i, "SATFINITE")) throw Unsup("SATFINITE");
            if (Has(i, "PACK_AB"))
            {
                // {a -> high half, b -> low half}
                if (Has(i, "RM") || Has(i, "RP")) throw Unsup();
                IrOperand hi = FSrc(ctx, o[1]), lo = FSrc(ctx, o[2]);
                // .RELU: negative values (-0 too) become +0 before the conversion, NaN stays NaN
                if (Has(i, "RELU")) { hi = ReluBits(ctx, hi); lo = ReluBits(ctx, lo); }
                if (Has(i, "BF16"))
                {
                    if (Has(i, "RZ")) throw Unsup("BF16 rounding mode");
                    Dst(ctx, o[0], ctx.BitwiseOr(Bf16Rn(ctx, lo), ctx.ShiftLeft(Bf16Rn(ctx, hi), Const(16))));
                    return;
                }
                if (!Has(i, "RZ"))
                {
                    Dst(ctx, o[0], ctx.PackHalf2x16(lo, hi));
                    return;
                }
                // .RZ: the round-to-nearest half, one step toward zero when it came out larger in magnitude than the input
                // (finite overflow to infinity gives 0x7bff the same way). hwcheck, RTX 3080.
                IrOperand Half(IrOperand x)
                {
                    IrOperand h = ctx.BitwiseAnd(ctx.PackHalf2x16(x, ConstF(0)), Const(0xffff));
                    IrOperand back = ctx.UnpackHalf2x16Low(h);
                    IrOperand over = ctx.BitwiseAnd(ctx.FPCompareLess(ctx.FPAbsolute(x), ctx.FPAbsolute(back)), Finite(ctx, x));
                    return ctx.ConditionalSelect(over, ctx.ISubtract(h, Const(1)), h);
                }
                Dst(ctx, o[0], ctx.BitwiseOr(Half(lo), ctx.ShiftLeft(Half(hi), Const(16))));
                return;
            }
            if (Has(i, "MERGE_C"))
            {
                if (Has(i, "RELU") || Has(i, "BF16") || Directed(i)) throw Unsup("MERGE_C form");
                // inferred from the compiler's use: Rd = {low half of c -> high half, f16(a) -> low half}
                IrOperand lo = ctx.BitwiseAnd(ctx.PackHalf2x16(FSrc(ctx, o[1]), ConstF(0)), Const(0xffff));
                Dst(ctx, o[0], ctx.BitwiseOr(lo, ctx.ShiftLeft(Src(ctx, o[2]), Const(16))));
                return;
            }
            throw Unsup();
        }

        /// fp32 bits with negative values (-0 included) replaced by +0; NaN kept.
        static IrOperand ReluBits(EmitterContext ctx, IrOperand x)
        {
            IrOperand nan = ctx.ICompareGreaterUnsigned(ctx.BitwiseAnd(x, Const(0x7fffffff)), Const(0x7f800000));
            return ctx.ConditionalSelect(ctx.BitwiseAnd(ctx.ICompareLess(x, Const(0)), ctx.Add(Instruction.LogicalNot, Local(), nan)), Const(0), x);
        }

        /// fp32 bits to bf16 (low 16 bits), round to nearest even; NaN gives 0x7fff.
        static IrOperand Bf16Rn(EmitterContext ctx, IrOperand x)
        {
            IrOperand nan = ctx.ICompareGreaterUnsigned(ctx.BitwiseAnd(x, Const(0x7fffffff)), Const(0x7f800000));
            IrOperand r = ctx.ShiftRightU32(ctx.IAdd(ctx.IAdd(x, Const(0x7fff)), ctx.BitwiseAnd(ctx.ShiftRightU32(x, Const(16)), Const(1))), Const(16));
            return ctx.ConditionalSelect(nan, Const(0x7fff), r);
        }

        /// Both lanes of a packed-bf16 source as fp32 bits (modifiers applied to the sign bit).
        static IrOperand[] Bf16Pair(EmitterContext ctx, IReadOnlyList<Op> o, ref int k)
        {
            Op x = o[k++];
            if (x.Kind is not (OperandKind.Reg or OperandKind.CBuf or OperandKind.UReg)) throw Unsup("bf16 immediate");
            IrOperand v = Raw(ctx, o[k - 1]);
            IrOperand l = ctx.ShiftLeft(v, Const(16)), h = ctx.BitwiseAnd(v, Const(unchecked((int)0xffff0000)));
            IrOperand[] r = HasSuffix(x, ".H0_H0") ? [l, l] : HasSuffix(x, ".H1_H1") ? [h, h] : HasSuffix(x, ".F32") ? [v, v] : [l, h];
            for (int n = 0; n < 2; n++)
            {
                if (x.Abs) r[n] = ctx.BitwiseAnd(r[n], Const(0x7fffffff));
                if (x.Neg) r[n] = ctx.BitwiseExclusiveOr(r[n], Const(unchecked((int)0x80000000)));
            }
            return r;
        }

        /// HFMA2.BF16_V2: per lane a*b+c exact in fp64 (8-bit significands), rounded to odd at fp32 (truncate, then set the last bit
        /// when inexact), .SAT / .RELU, then to bf16 with round to nearest even.
        static void EmitHfma2Bf16Ampere(EmitterContext ctx, SassInst i)
        {
            if (i.Mods.Any(m => m is not ("BF16_V2" or "SAT" or "RELU"))) throw Unsup("BF16_V2 form");
            IReadOnlyList<Op> o = i.Operands;
            int k = 1;
            IrOperand[] a = Bf16Pair(ctx, o, ref k), b = Bf16Pair(ctx, o, ref k), c = Bf16Pair(ctx, o, ref k);
            SassInst rz = i with { Mods = new[] { "RZ" } };
            IrOperand res = Const(0);
            for (int n = 0; n < 2; n++)
            {
                IrOperand p = ctx.FPMultiply(ExactF64(ctx, a[n]), ExactF64(ctx, b[n]), D);
                (IrOperand s, IrOperand err) = TwoSumD(ctx, p, ExactF64(ctx, c[n]));
                IrOperand t = RoundToF32(ctx, rz, s, err, Const(IrConsts.True));
                IrOperand inexact = ctx.BitwiseOr(ctx.Add(Instruction.LogicalNot, Local(), ctx.FPCompareEqual(err, ctx.PackDouble2x32(0.0), D)),
                                                  ctx.Add(Instruction.LogicalNot, Local(), ctx.FPCompareEqual(ExactF64(ctx, t), s, D)));
                t = ctx.ConditionalSelect(ctx.BitwiseAnd(inexact, Finite(ctx, t)), ctx.BitwiseOr(t, Const(1)), t);
                if (Has(i, "SAT")) t = SatBits(ctx, t);
                if (Has(i, "RELU")) t = ReluBits(ctx, t);
                IrOperand h = Bf16Rn(ctx, t);
                res = n == 0 ? h : ctx.BitwiseOr(res, ctx.ShiftLeft(h, Const(16)));
            }
            Dst(ctx, o[0], res);
        }

        /// Both lanes of a packed-half source. Immediates come as one operand (raw bits) or two FImm operands.
        static IrOperand[] HalfPair(EmitterContext ctx, IReadOnlyList<Op> o, ref int k)
        {
            Op x = o[k++];
            if (x.Kind is OperandKind.FImm or OperandKind.Imm)
            {
                if (k < o.Count && o[k].Kind == OperandKind.FImm && x.Kind == OperandKind.FImm)
                {
                    Op y = o[k++];
                    return [Raw(ctx, x), Raw(ctx, y)];
                }
                return GetHalfSrc(ctx, (int)x.ImmBits);
            }
            IrOperand v = Raw(ctx, x);
            IrOperand[] r;
            if (HasSuffix(x, ".H0_H0")) r = [ctx.UnpackHalf2x16Low(v), ctx.UnpackHalf2x16Low(v)];
            else if (HasSuffix(x, ".H1_H1")) r = [ctx.UnpackHalf2x16High(v), ctx.UnpackHalf2x16High(v)];
            else if (HasSuffix(x, ".F32")) r = [v, v];
            else r = [ctx.UnpackHalf2x16Low(v), ctx.UnpackHalf2x16High(v)];
            return FPAbsNeg(ctx, r, x.Abs, x.Neg);
        }

        static void EmitHsetAmpere(EmitterContext ctx, SassInst i)
        {
            // HSETP2[.H_AND] Pd, Pd2, a, b, Pc / HSET2[.BF] Rd, a, b, Pc: per-half float compare (Ryujinx Maxwell Hsetp2/Hset2).
            int c = Array.FindIndex(FCompNames, n => i.Mods.Contains(n));
            if (c < 0) throw Unsup("no comparison");
            BoolOp bop = BopOf(i);
            IReadOnlyList<Op> o = i.Operands;
            bool setp = i.Opcode == "HSETP2";
            int k = setp ? 2 : 1;
            IrOperand[] a = HalfPair(ctx, o, ref k);
            IrOperand[] b = HalfPair(ctx, o, ref k);
            IrOperand pc = Src(ctx, o[k]);
            IrOperand[] r = [GetFPComparison(ctx, (FComp)c, a[0], b[0]), GetFPComparison(ctx, (FComp)c, a[1], b[1])];
            if (setp)
            {
                IrOperand p0, p1;
                if (Has(i, "H_AND"))
                {
                    IrOperand both = ctx.BitwiseAnd(r[0], r[1]);
                    p0 = GetPredLogicalOp(ctx, bop, both, pc);
                    p1 = GetPredLogicalOp(ctx, bop, ctx.Add(Instruction.LogicalNot, Local(), both), pc);
                }
                else
                {
                    p0 = GetPredLogicalOp(ctx, bop, r[0], pc);
                    p1 = GetPredLogicalOp(ctx, bop, r[1], pc);
                }
                Dst(ctx, o[0], p0);
                if (!(o[1].Kind == OperandKind.Pred && o[1].Reg == 7)) Dst(ctx, o[1], p1);
                return;
            }
            for (int n = 0; n < 2; n++) r[n] = GetPredLogicalOp(ctx, bop, r[n], pc);
            if (Has(i, "BF"))
                Dst(ctx, o[0], ctx.PackHalf2x16(ctx.ConditionalSelect(r[0], ConstF(1), ConstF(0)), ctx.ConditionalSelect(r[1], ConstF(1), ConstF(0))));
            else
                Dst(ctx, o[0], ctx.BitwiseOr(ctx.ConditionalSelect(r[0], Const(0xffff), Const(0)), ctx.ConditionalSelect(r[1], Const(unchecked((int)0xffff0000)), Const(0))));
        }

        static void EmitHalfAmpere(EmitterContext ctx, SassInst i)
        {
            if (Has(i, "BF16_V2")) { if (i.Opcode != "HFMA2") throw Unsup(); EmitHfma2Bf16Ampere(ctx, i); return; }
            IReadOnlyList<Op> o = i.Operands;
            int k = 1;
            bool ftz = Has(i, "FTZ");
            IrOperand[] a = HalfFtz(ctx, ftz, HalfPair(ctx, o, ref k));
            IrOperand[] b = HalfFtz(ctx, ftz, HalfPair(ctx, o, ref k));
            IrOperand[] r = new IrOperand[2];
            // the float16 and cooperative-matrix variants of a block compute half results in float16 itself (AmpereCoopMat.TryHalf;
            // null in the plain module, which computes in fp32)
            bool f16 = !Has(i, "F32");
            switch (i.Opcode)
            {
                case "HADD2": for (int n = 0; n < 2; n++) r[n] = (f16 ? Ryujinx.Graphics.Shader.CodeGen.Spirv.AmpereCoopMat.TryHalf(ctx, 0, a[n], b[n], null) : null) ?? ctx.FPAdd(a[n], b[n]); break;
                case "HMUL2": for (int n = 0; n < 2; n++) r[n] = (f16 ? Ryujinx.Graphics.Shader.CodeGen.Spirv.AmpereCoopMat.TryHalf(ctx, 1, a[n], b[n], null) : null) ?? ctx.FPMultiply(a[n], b[n]); break;
                case "HFMA2":
                    {
                        IrOperand[] c = HalfFtz(ctx, ftz, HalfPair(ctx, o, ref k));
                        for (int n = 0; n < 2; n++)
                        {
                            if (f16 && (r[n] = Ryujinx.Graphics.Shader.CodeGen.Spirv.AmpereCoopMat.TryHalf(ctx, 2, a[n], b[n], c[n])) != null) continue;
                            r[n] = ctx.FPFusedMultiplyAdd(a[n], b[n], c[n]);
                            // fp16 FMA done in fp32 rounds twice (fp32, then fp16 when packed). Rounding the fp32 result to odd
                            // instead (24 >= 11 + 2 bits) makes the second rounding the correct one (hwcheck: HFMA2 was 1 ulp off in
                            // 1 of 1024 threads). a*b is exact in fp64; the exact value is s + err.
                            if (!Has(i, "F32")) r[n] = HalfFmaOdd(ctx, a[n], b[n], c[n], r[n]);
                        }
                        break;
                    }
                case "HMNMX2":
                    {
                        IrOperand p = Src(ctx, o[k]);
                        for (int n = 0; n < 2; n++)
                        {
                            r[n] = ctx.ConditionalSelect(p, ctx.FPMinimum(a[n], b[n]), ctx.FPMaximum(a[n], b[n]));
                            // .NAN: a NaN input gives NaN
                            if (Has(i, "NAN")) r[n] = ctx.ConditionalSelect(ctx.BitwiseOr(ctx.IsNan(a[n]), ctx.IsNan(b[n])), ConstF(float.NaN), r[n]);
                        }
                        break;
                    }
            }
            for (int n = 0; n < 2; n++)
            {
                if (Has(i, "SAT")) r[n] = ctx.FPSaturate(r[n]);
                // .RELU keeps NaN (the hardware returns the canonical NaN 0x7fff; hwcheck, RTX 3080); max(NaN, 0) would give 0
                if (Has(i, "RELU")) r[n] = ctx.ConditionalSelect(ctx.IsNan(r[n]), r[n], ctx.FPMaximum(r[n], ConstF(0)));
            }
            if (Has(i, "F32")) Dst(ctx, o[0], r[0]);
            else if (i.Mods.All(m => m is "SAT" or "RELU" or "FTZ" or "MMA" or "NAN"))
            {
                IrOperand h = ctx.PackHalf2x16(r[0], r[1]);
                if (ftz)
                {
                    // .FTZ: an fp16 denormal result becomes a zero of its sign
                    for (int n = 0; n < 2; n++)
                    {
                        IrOperand half = ctx.BitwiseAnd(ctx.ShiftRightU32(h, Const(16 * n)), Const(0xffff));
                        IrOperand flushed = ctx.ConditionalSelect(ctx.ICompareEqual(ctx.BitwiseAnd(half, Const(0x7c00)), Const(0)), ctx.BitwiseAnd(half, Const(0x8000)), half);
                        h = n == 0 ? ctx.BitwiseOr(ctx.BitwiseAnd(h, Const(unchecked((int)0xffff0000))), flushed) : ctx.BitwiseOr(ctx.BitwiseAnd(h, Const(0xffff)), ctx.ShiftLeft(flushed, Const(16)));
                    }
                }
                Dst(ctx, o[0], h);
            }
            else throw Unsup();
        }

        /// .FTZ on fp16 inputs (as fp32 values): magnitudes below 2^-14 (fp16 denormals) read as zeros of their sign.
        static IrOperand[] HalfFtz(EmitterContext ctx, bool ftz, IrOperand[] v)
        {
            if (!ftz) return v;
            var r = new IrOperand[v.Length];
            for (int n = 0; n < v.Length; n++)
            {
                IrOperand den = ctx.FPCompareLess(ctx.FPAbsolute(v[n]), ConstF(6.103515625e-05f));
                r[n] = ctx.ConditionalSelect(den, ctx.BitwiseAnd(v[n], Const(unchecked((int)0x80000000))), v[n]);
            }
            return r;
        }

        /// fp32 r = a*b+c rounded to odd: an inexact r with an even last bit moves one ulp toward the exact value.
        static IrOperand HalfFmaOdd(EmitterContext ctx, IrOperand a, IrOperand b, IrOperand c, IrOperand r)
        {
            IrOperand p = ctx.FPMultiply(ctx.FP32ConvertToFP64(a), ctx.FP32ConvertToFP64(b), Instruction.FP64);
            (IrOperand s, IrOperand err) = TwoSumD(ctx, p, ctx.FP32ConvertToFP64(c));
            IrOperand dr = ctx.FP32ConvertToFP64(r), zeroD = ctx.PackDouble2x32(0.0);
            IrOperand eq = ctx.FPCompareEqual(s, dr, Instruction.FP64);
            IrOperand above = ctx.BitwiseOr(ctx.FPCompareLess(dr, s, Instruction.FP64), ctx.BitwiseAnd(eq, ctx.FPCompareLess(zeroD, err, Instruction.FP64)));
            IrOperand below = ctx.BitwiseOr(ctx.FPCompareLess(s, dr, Instruction.FP64), ctx.BitwiseAnd(eq, ctx.FPCompareLess(err, zeroD, Instruction.FP64)));
            IrOperand nonzero = ctx.ICompareNotEqual(ctx.BitwiseAnd(r, Const(0x7fffffff)), Const(0));
            IrOperand nudge = ctx.BitwiseAnd(ctx.BitwiseAnd(ctx.BitwiseOr(above, below), ctx.ICompareEqual(ctx.BitwiseAnd(r, Const(1)), Const(0))), ctx.BitwiseAnd(nonzero, Finite(ctx, r)));
            // up in value: +1 on the bits of a positive r, -1 on a negative one
            IrOperand neg = ctx.ICompareLess(r, Const(0));
            IrOperand plus = ctx.ICompareEqual(Bool(ctx, above), Bool(ctx, ctx.Add(Instruction.LogicalNot, Local(), neg)));
            return ctx.ConditionalSelect(nudge, ctx.ConditionalSelect(plus, ctx.IAdd(r, Const(1)), ctx.ISubtract(r, Const(1))), r);
        }

        // ---- system values ----------------------------------------------------------------------------------------

        static readonly Dictionary<string, SReg> SRegs = new()
        {
            ["SR_LANEID"] = SReg.LaneId, ["SR_TID"] = SReg.TId, ["SR_TID.X"] = SReg.TIdX, ["SR_TID.Y"] = SReg.TIdY, ["SR_TID.Z"] = SReg.TIdZ,
            ["SR_CTAID.X"] = SReg.CtaIdX, ["SR_CTAID.Y"] = SReg.CtaIdY, ["SR_CTAID.Z"] = SReg.CtaIdZ,
            ["SR_EQMASK"] = SReg.EqMask, ["SR_LTMASK"] = SReg.LtMask, ["SR_LEMASK"] = SReg.LeMask, ["SR_GTMASK"] = SReg.GtMask, ["SR_GEMASK"] = SReg.GeMask,
            ["SR_INVOCATIONID"] = SReg.InvocationId, ["SR_Y_DIRECTION"] = SReg.YDirection, ["SR_THREAD_KILL"] = SReg.ThreadKill,
            ["SR_INVOCATIONINFO"] = SReg.InvocationInfo, ["SR_CLOCKLO"] = SReg.ClockLo, ["SR_CLOCKHI"] = SReg.ClockHi,
            // nvdisasm spells these two with an underscore
            ["SR_INVOCATION_ID"] = SReg.InvocationId, ["SR_INVOCATION_INFO"] = SReg.InvocationInfo,
        };

        static void EmitS2rAmpere(EmitterContext ctx, SassInst i)
        {
            Op d = i.Operands[0];
            string name = i.Operands[1].Text;
            // the coarse shading rate of the pixel (variable-rate shading, 350 OoT fragment shaders): the host shades every pixel, so the
            // rate is 1x1, 0 (inferred: no game word shows the encoding)
            if (name == "SR_VARIABLE_RATE") { Dst(ctx, d, Const(0)); return; }
            if (!SRegs.TryGetValue(name, out SReg sr)) throw Unsup(name);
            if (sr is SReg.ClockLo or SReg.ClockHi) { Dst(ctx, d, Const(0)); return; }
            // Ryujinx's S2R on a transcoded Maxwell opcode, writing a scratch-free destination: use the Gpr directly when
            // the destination is a vector register, else a temporary Gpr-shaped write through RZ is not possible, so
            // emit into R255's slot via a local copy.
            if (d.Kind == OperandKind.Reg)
            {
                RunMaxwell(ctx, i, "1111000011001", (ulong)(d.Reg & 0xff) | ((ulong)sr << 20));
                return;
            }
            // S2UR: emit into the vector form on a fresh local, then copy.
            IrOperand v = sr switch
            {
                SReg.TIdX => ctx.Load(StorageKind.Input, IoVariable.ThreadId, null, Const(0)),
                SReg.TIdY => ctx.Load(StorageKind.Input, IoVariable.ThreadId, null, Const(1)),
                SReg.TIdZ => ctx.Load(StorageKind.Input, IoVariable.ThreadId, null, Const(2)),
                SReg.CtaIdX => ctx.Load(StorageKind.Input, IoVariable.CtaId, null, Const(0)),
                SReg.CtaIdY => ctx.Load(StorageKind.Input, IoVariable.CtaId, null, Const(1)),
                SReg.CtaIdZ => ctx.Load(StorageKind.Input, IoVariable.CtaId, null, Const(2)),
                _ => throw Unsup(name),
            };
            Dst(ctx, d, v);
        }

        /// Runs Ryujinx's Maxwell emitter for an opcode built from a 13-bit pattern prefix and field bits (guard = PT).
        static void RunMaxwell(EmitterContext ctx, SassInst i, string pattern, ulong fields)
        {
            ulong raw = fields | (7UL << 16);
            for (int b = 0; b < pattern.Length; b++)
            {
                if (pattern[b] == '1') raw |= 1UL << (63 - b);
            }
            InstOp op = InstTable.GetOp((ulong)i.Address, raw);
            if (op.Emitter == null) throw Unsup("transcoding failed");
            InstOp saved = ctx.CurrOp;
            ctx.CurrOp = op;
            op.Emitter(ctx);
            ctx.CurrOp = saved;
        }

        // ---- attributes -------------------------------------------------------------------------------------------

        static int AlSizeOf(SassInst i) => Has(i, "128") ? 3 : Has(i, "96") ? 2 : Has(i, "64") ? 1 : 0;

        static (int reg, int imm) AttrAddress(Op a)
        {
            if (a.Kind != OperandKind.Attr) throw Unsup("attribute operand");
            int reg = a.IndexReg >= 0 ? a.IndexReg : 255;
            if (reg != 255 && a.Offset != 0) throw Unsup("indexed attribute with offset");
            if (a.Offset < 0 || a.Offset > 0x7ff) throw Unsup("attribute offset");
            return (reg, (int)a.Offset);
        }

        static int RegOrRz(Op o) => o == null ? 255 : o.Kind == OperandKind.Reg ? o.Reg : throw Unsup($"operand {o.Kind}");

        static void EmitAldAmpere(EmitterContext ctx, SassInst i)
        {
            // ALD[.size][.O][.P] Rd, a[...] [, Rvertex]
            IReadOnlyList<Op> o = i.Operands;
            if (o[0].Kind != OperandKind.Reg) throw Unsup("destination");
            (int reg, int imm) = AttrAddress(o[1]);
            int vtx = RegOrRz(o.Count > 2 ? o[2] : null);
            ulong f = (ulong)(o[0].Reg & 0xff) | ((ulong)reg << 8) | ((ulong)imm << 20) | ((ulong)vtx << 39) | ((ulong)AlSizeOf(i) << 47);
            if (Has(i, "P")) f |= 1UL << 31;
            if (Has(i, "O")) f |= 1UL << 32;
            RunMaxwell(ctx, i, "1110111111011", f);
        }

        static void EmitAstAmpere(EmitterContext ctx, SassInst i)
        {
            // AST[.size][.P] a[...], Rsrc [, Rvertex]
            IReadOnlyList<Op> o = i.Operands;
            (int reg, int imm) = AttrAddress(o[0]);
            int src = RegOrRz(o[1]);
            int vtx = RegOrRz(o.Count > 2 ? o[2] : null);
            ulong f = (ulong)src | ((ulong)reg << 8) | ((ulong)imm << 20) | ((ulong)vtx << 39) | ((ulong)AlSizeOf(i) << 47);
            if (Has(i, "P")) f |= 1UL << 31;
            RunMaxwell(ctx, i, "1110111111110", f);
        }

        static void EmitIpaAmpere(EmitterContext ctx, SassInst i)
        {
            // IPA.PASS|CONSTANT|SC[.CENTROID|.OFFSET][.SAT] Rd, [Pd,] a[...] [, Roffset]
            // Volta's IPA has no multiply operand: IPA.PASS of a perspective user attribute returns attr/w (screen-linear)
            // and the code corrects it itself, e.g. 2db3327d9fbc5bb3:
            //   IPA.PASS R6, a[0x7c]; IPA.PASS R5, a[0x90]; MUFU.RCP R6, R6; FMUL R4, R6, R5
            // Vulkan's input is already perspective-correct, so IPA.PASS of a user attribute goes through Ryujinx's
            // Maxwell IPA.PASS, which multiplies a Perspective (SPH imap) input by gl_FragCoord.w (= 1/w) to give the
            // same attr/w; linear and flat inputs are not multiplied. IPA.CONSTANT (flat) stays a plain load (ALD path).
            // The rest is Ryujinx's IPA.PASS too (fragment coordinates with render scale and y flip, front facing, ...).
            IReadOnlyList<Op> o = i.Operands;
            if (o[0].Kind != OperandKind.Reg) throw Unsup("destination");
            if (IsPredKind(o[1])) throw Unsup("predicate destination");
            // .CENTROID samples at the covered centroid of a multisampled pixel; Ryujinx's Maxwell IPA decodes the sample mode (Msi)
            // and ignores it, so the input is read as at the pixel centre, which equals centroid without multisampling (Mario Kart
            // World: nearly every fragment shader).
            if (Has(i, "OFFSET") || Has(i, "SAT")) throw Unsup("interpolation mode");
            if (o[1].Kind == OperandKind.Attr && o[1].IndexIsUReg && o[1].IndexReg >= 0)
            {
                // a[URn+imm] (0x526): a user attribute picked at run time, as Ryujinx's indexed Maxwell IPA (InstEmitAttribute.Ipa, op.Idx), with
                // the immediate added. PASS returns attr/w for a perspective input; the indexed form cannot look up the SPH imap per element, so
                // every element is taken as perspective, as Ryujinx does (OoT 471bac84: the six face varyings are perspective).
                if (!Has(i, "PASS")) throw Unsup("indexed attribute interpolation");
                if (o.Count > 2 && !(o[2].Kind == OperandKind.Reg && o[2].Reg == 255)) throw Unsup("offset register");
                IrOperand addr = o[1].IndexReg >= 63 ? Const((int)o[1].Offset) : ctx.IAdd(UGpr(o[1].IndexReg), Const((int)o[1].Offset));
                IrOperand rel = ctx.ISubtract(addr, Const(AttributeConsts.UserAttributeBase));
                IrOperand vec = ctx.ShiftRightU32(rel, Const(4));
                IrOperand elem = ctx.BitwiseAnd(ctx.ShiftRightU32(rel, Const(2)), Const(3));
                IrOperand v = ctx.Load(StorageKind.Input, IoVariable.UserDefined, null, vec, elem);
                v = ctx.FPMultiply(v, ctx.Load(StorageKind.Input, IoVariable.FragmentCoord, null, Const(3)));
                Dst(ctx, o[0], v);
                return;
            }
            (int reg, int imm) = AttrAddress(o[1]);
            if (reg != 255) throw Unsup("indexed attribute");
            if (o.Count > 2 && !(o[2].Kind == OperandKind.Reg && o[2].Reg == 255)) throw Unsup("offset register");
            if (imm is >= AttributeConsts.UserAttributeBase and < AttributeConsts.UserAttributeEnd && Has(i, "CONSTANT"))
            {
                ulong f = (ulong)(o[0].Reg & 0xff) | (255UL << 8) | ((ulong)imm << 20) | (255UL << 39);
                RunMaxwell(ctx, i, "1110111111011", f);
                return;
            }
            if (imm > 0x3ff) throw Unsup("attribute offset");
            ulong g = (ulong)(o[0].Reg & 0xff) | (255UL << 8) | (255UL << 20) | (255UL << 39) | ((ulong)imm << 28) | ((ulong)IpaOp.Pass << 54) | (7UL << 47);
            RunMaxwell(ctx, i, "11100000", g);
        }

        static void EmitOutAmpere(EmitterContext ctx, SassInst i)
        {
            // OUT.EMIT|CUT|EMIT_THEN_CUT Rd, Ra, Rb
            IReadOnlyList<Op> o = i.Operands;
            int type = Has(i, "EMIT_THEN_CUT") ? 3 : Has(i, "CUT") ? 2 : Has(i, "EMIT") ? 1 : throw Unsup();
            if (o.Count < 3) throw Unsup();
            ulong f = (ulong)RegOrRz(o[0]) | ((ulong)RegOrRz(o[1]) << 8) | ((ulong)RegOrRz(o[2]) << 20) | ((ulong)type << 39);
            RunMaxwell(ctx, i, "1111101111100", f);
        }

        // ---- constant buffers and memory --------------------------------------------------------------------------

        static void EmitLdcAmpere(EmitterContext ctx, SassInst i)
        {
            // LDC[.U8|.S8|.U16|.S16|.64] Rd, c[bank][Ri+off]   (ULDC: uniform destination, no index register)
            IReadOnlyList<Op> o = i.Operands;
            Op c = o[1];
            if (c.Kind != OperandKind.CBuf) throw Unsup("source");
            if (Has(i, "IL")) throw Unsup("address mode");
            int words = Has(i, "64") ? 2 : 1;
            LsSize small = Has(i, "U8") ? LsSize.U8 : Has(i, "S8") ? LsSize.S8 : Has(i, "U16") ? LsSize.U16 : Has(i, "S16") ? LsSize.S16 : LsSize.B32;
            if (Has(i, "IS") || Has(i, "ISL"))
            {
                // Indexed segmented (Ryujinx Maxwell Ldc): the index register holds the bank offset in bits 16..31 and the byte
                // offset in bits 0..15, e.g. "LEA R0, R0, 0x10000, 0x3; LDC.64.IS R8, c[0x0][R0]" reads c[1][index * 8].
                if (small != LsSize.B32 || c.BankIsUReg || c.IndexReg < 0 || c.IndexReg >= 255) throw Unsup("address mode");
                IrOperand ri = c.IndexIsUReg ? UGpr(c.IndexReg) : Gpr(c.IndexReg);
                IrOperand slotIs = ctx.IAdd(Const(c.Bank), ctx.ShiftRightU32(ri, Const(16)));
                IrOperand wordIs = ctx.ShiftRightU32(ctx.IAdd(ctx.BitwiseAnd(ri, Const(0xffff)), Const((int)c.Offset)), Const(2));
                for (int w = 0; w < words; w++) DstWord(ctx, o[0], w, EmitLoadConstant(ctx, slotIs, w == 0 ? wordIs : ctx.IAdd(wordIs, Const(w))));
                return;
            }
            if (small != LsSize.B32)
            {
                IrOperand slot = c.BankIsUReg ? UGpr(c.Bank) : Const(c.Bank);
                IrOperand addr = Const((int)c.Offset);
                if (c.IndexReg >= 0) addr = ctx.IAdd(c.IndexIsUReg ? UGpr(c.IndexReg) : Gpr(c.IndexReg), addr);
                IrOperand v = EmitLoadConstant(ctx, slot, ctx.ShiftRightU32(addr, Const(2)));
                Dst(ctx, o[0], ExtractSmallInt(ctx, small, GetBitOffset(ctx, addr), v));
                return;
            }
            // Every word is loaded before the first is written: the destination pair may overlap the index register
            // ("LDC.64 R58, c[0x6][R58+0x28]", OoT's skinning vertex shaders), and the second load must use the old index.
            IrOperand[] vals = new IrOperand[words];
            for (int w = 0; w < words; w++) vals[w] = LoadCbuf(ctx, c, w);
            for (int w = 0; w < words; w++) DstWord(ctx, o[0], w, vals[w]);
        }

        /// Byte address of a [Ra(.X4|.X8|.X16) + URb + imm] operand (shared/local memory).
        static IrOperand MemAddress(EmitterContext ctx, Op m)
        {
            if (m.Kind != OperandKind.Mem) throw Unsup("memory operand");
            IrOperand addr = Const((int)m.Offset);
            if (m.Reg >= 0 && m.Reg < 255)
            {
                IrOperand r = Gpr(m.Reg);
                int scale = HasSuffix(m, ".X4") ? 2 : HasSuffix(m, ".X8") ? 3 : HasSuffix(m, ".X16") ? 4 : 0;
                if (HasSuffix(m, ".64")) throw Unsup("64-bit address");
                if (scale != 0) r = ctx.ShiftLeft(r, Const(scale));
                addr = ctx.IAdd(r, addr);
            }
            if (m.IndexReg >= 0) addr = ctx.IAdd(m.IndexIsUReg ? UGpr(m.IndexReg) : Gpr(m.IndexReg), addr);
            return addr;
        }

        static LsSize2 LsSizeOf(SassInst i) =>
            Has(i, "U8") ? LsSize2.U8 : Has(i, "S8") ? LsSize2.S8 : Has(i, "U16") ? LsSize2.U16 : Has(i, "S16") ? LsSize2.S16 :
            Has(i, "128") ? LsSize2.B128 : Has(i, "64") ? LsSize2.B64 : LsSize2.B32;

        static void EmitLdStAmpere(EmitterContext ctx, SassInst i, StorageKind kind, bool load)
        {
            // LDS/LDL Rd, [addr]   STS/STL [addr], Rs
            IReadOnlyList<Op> o = i.Operands;
            Op data = load ? o[0] : o[1];
            Op mem = load ? o[1] : o[0];
            if (data.Kind != OperandKind.Reg) throw Unsup("data operand");
            if (kind == StorageKind.SharedMemory && ctx.TranslatorContext.Definitions.Stage != ShaderStage.Compute) throw Unsup("shared memory outside compute");
            IrOperand addr = MemAddress(ctx, mem);
            if (load) EmitLoad(ctx, kind, LsSizeOf(i), addr, data.Reg, 0);
            else EmitStore(ctx, kind, LsSizeOf(i), addr, data.Reg, 0);
        }

        /// 64-bit global address of [Ra(.64|.U32) + URb(pair) + imm] (+ extra: the byte offset of a vector element). The immediate and
        /// the element offset are one constant add on top of the register sum: Ryujinx's GlobalToStorage peels a single constant
        /// before it looks for the c[0] base, so "(R + c0) + imm + 4n" as two adds lost the storage buffer (cbabef05, 6 more).
        static (IrOperand lo, IrOperand hi) GlobalAddress(EmitterContext ctx, SassInst i, Op m, long extra = 0)
        {
            if (m.Kind != OperandKind.Mem) throw Unsup("memory operand");
            IrOperand lo = Const(0), hi = Const(0);
            if (m.Reg >= 0 && m.Reg < 255)
            {
                lo = Gpr(m.Reg);
                // .E = 64-bit addresses: the base is a register pair unless printed as Rn.U32 (32-bit base plus a UR pair).
                bool wide = (Has(i, "E") && !HasSuffix(m, ".U32")) || HasSuffix(m, ".64");
                hi = wide ? Gpr(m.Reg + 1) : Const(0);
            }
            if (m.IndexReg >= 0)
            {
                if (!m.IndexIsUReg) throw Unsup("address index");
                IrOperand ulo = UGpr(m.IndexReg), uhi = m.IndexReg < 63 ? UGpr(m.IndexReg + 1) : Const(0);
                IrOperand nlo = ctx.IAdd(lo, ulo);
                hi = ctx.IAdd(ctx.IAdd(hi, uhi), Bool(ctx, ctx.ICompareLessUnsigned(nlo, ulo)));
                lo = nlo;
            }
            long imm = m.Offset + extra;
            if (imm != 0)
            {
                IrOperand off = Const((int)imm);
                IrOperand nlo = ctx.IAdd(lo, off);
                // a negative offset is sign-extended: hi gains carry - 1
                IrOperand carry = Bool(ctx, ctx.ICompareLessUnsigned(nlo, lo));
                hi = ctx.IAdd(hi, imm < 0 ? ctx.ISubtract(carry, Const(1)) : carry);
                lo = nlo;
            }
            return (lo, hi);
        }

        static void EmitGlobalAmpere(EmitterContext ctx, SassInst i, bool load)
        {
            // LDG.E[.size] Rd, [addr]   STG.E[.size] [addr], Rs
            IReadOnlyList<Op> o = i.Operands;
            Op data = load ? o[0] : o[1];
            Op mem = load ? o[1] : o[0];
            LsSize2 size = LsSizeOf(i);
            StorageKind kind = GetStorageKind((LsSize)size);
            int count = GetVectorCount((LsSize)size);
            if (data.Kind != OperandKind.Reg) throw Unsup("data operand");
            // every element's address before the first load writes a register (LDG.E.64 R0, [R0.64] reads R0 for both words)
            var addr = new (IrOperand lo, IrOperand hi)[count];
            for (int n = 0; n < count; n++) addr[n] = GlobalAddress(ctx, i, mem, n * 4);
            // the words of a vector load are tagged: through the pointer fallback's fast path they load as one vector (AmpereGlobal.cs)
            int group = load && count > 1 && data.Reg + count <= 255 ? AmpereGlobal.NewVectorGroup() : 0;
            for (int n = 0; n < count; n++)
            {
                (IrOperand a, IrOperand hi) = addr[n];
                if (load)
                {
                    if (data.Reg + n >= 255) break;
                    IrOperand word = ctx.Load(kind, a, hi);
                    if (group != 0) AmpereGlobal.NoteVectorWord(word.AsgOp as Operation, group, count, n);
                    ctx.Copy(Gpr(data.Reg + n), word);
                }
                else
                {
                    ctx.Store(kind, a, hi, data.Reg >= 255 ? Const(0) : Gpr(data.Reg + n));
                }
            }
        }

        static AtomOp AtomOpOf(SassInst i) =>
            Has(i, "ADD") ? AtomOp.Add : Has(i, "MIN") ? AtomOp.Min : Has(i, "MAX") ? AtomOp.Max : Has(i, "INC") ? AtomOp.Inc :
            Has(i, "DEC") ? AtomOp.Dec : Has(i, "AND") ? AtomOp.And : Has(i, "OR") ? AtomOp.Or : Has(i, "XOR") ? AtomOp.Xor :
            Has(i, "EXCH") ? AtomOp.Exch : throw Unsup("atomic op");

        static void EmitAtomsAmpere(EmitterContext ctx, SassInst i)
        {
            // ATOMS.op[.S32] Rd, [addr], Rb
            IReadOnlyList<Op> o = i.Operands;
            IrOperand offset = ctx.ShiftRightU32(MemAddress(ctx, o[1]), Const(2));
            if (Has(i, "POPC") && Has(i, "INC"))
            {
                // ATOMS.POPC.INC.32 Rd, [addr]: the word gains the number of lanes that address it; one atomic add of 1 per lane does the
                // same (Rd, the old value, is then per lane rather than the warp's; the corpus only has Rd = RZ)
                IrOperand r = EmitAtomicOp(ctx, StorageKind.SharedMemory, AtomOp.Add, AtomSize.U32, Const(ctx.ResourceManager.SharedMemoryId), offset, Const(1));
                Dst(ctx, o[0], r);
                return;
            }
            if (Has(i, "CAS")) throw Unsup();
            string type = AtomType(i);
            if (type == "F32") { Dst(ctx, o[0], EmitFloatAtomicAdd(ctx, i, StorageKind.SharedMemory, Const(ctx.ResourceManager.SharedMemoryId), offset, Src(ctx, o[2]))); return; }
            if (type != null && type != "S32") throw Unsup($"shared atomic of type {type}");
            AtomSize size = type == "S32" ? AtomSize.S32 : AtomSize.U32;
            IrOperand res = EmitAtomicOp(ctx, StorageKind.SharedMemory, AtomOpOf(i), size, Const(ctx.ResourceManager.SharedMemoryId), offset, Src(ctx, o[2]));
            Dst(ctx, o[0], res);
        }

        /// The data type of an atomic as the decoder names it (AtomTypeG / AtomTypeS: "S32", "64", "F32.FTZ.RN", "F16x2.RN", "S64", "F64.RN"),
        /// up to the first dot; null for the default unsigned 32-bit type. Matching the whole modifier ("F32") missed the decorated forms, so
        /// RED.E.ADD.F32.FTZ.RN was translated as an integer add of the float bits (OoT's sky lighting compute 16a42858: garbage sums such as
        /// 1.8e29 in the spherical harmonics every material reads, NaN in the scene, the white frames of A12).
        static string AtomType(SassInst i)
        {
            foreach (string m in i.Mods)
                foreach (string t in new[] { "S32", "64", "F32", "F16x2", "S64", "F64" })
                    if (m == t || m.StartsWith(t + ".", StringComparison.Ordinal)) return t;
            return null;
        }

        /// A 32-bit float atomic add (RED/ATOM/ATOMS .ADD.F32.FTZ.RN) as a compare-and-swap loop on the word: SPIR-V's float atomics need an
        /// extension the IR has no instruction for. Returns the value before the add (ATOM's Rd).
        static IrOperand EmitFloatAtomicAdd(EmitterContext ctx, SassInst i, StorageKind kind, IrOperand e0, IrOperand e1, IrOperand value)
        {
            if (AtomOpOf(i) != AtomOp.Add) throw Unsup("float atomic other than add");
            // The word is loaded again in every round: IR locals are single-assignment values, so a value carried around the back edge
            // (the CAS result as the next round's expected value) stays the first round's and the loop never ends (device lost).
            IrOperand loop = Label();
            ctx.MarkLabel(loop);
            IrOperand before = ctx.Load(kind, e0, e1);
            IrOperand sum = ctx.FPAdd(before, value);
            IrOperand seen = ctx.AtomicCompareAndSwap(kind, e0, e1, before, sum);
            ctx.BranchIfTrue(loop, ctx.ICompareNotEqual(seen, before));
            return before;
        }

        static void EmitAtomgAmpere(EmitterContext ctx, SassInst i)
        {
            // ATOMG.E.op[.size].STRONG.GPU Pd, Rd, [addr], Rb
            string type = AtomType(i);
            if (Has(i, "CAS") || (type != null && type != "S32" && type != "F32")) throw Unsup($"global atomic of type {type}");
            IReadOnlyList<Op> o = i.Operands;
            int k = IsPredKind(o[0]) ? 1 : 0;
            (IrOperand lo, IrOperand hi) = GlobalAddress(ctx, i, o[k + 1]);
            AtomSize size = type == "S32" ? AtomSize.S32 : AtomSize.U32;
            IrOperand res = type == "F32" ? EmitFloatAtomicAdd(ctx, i, StorageKind.GlobalMemory, lo, hi, Src(ctx, o[k + 2]))
                                          : EmitAtomicOp(ctx, StorageKind.GlobalMemory, AtomOpOf(i), size, lo, hi, Src(ctx, o[k + 2]));
            Dst(ctx, o[k], res);
            if (k == 1) Dst(ctx, o[0], Const(IrConsts.True));
        }

        static void EmitRedAmpere(EmitterContext ctx, SassInst i)
        {
            // RED.E.op[.size].STRONG.GPU [addr], Rb: a global atomic whose result is not returned
            string type = AtomType(i);
            if (Has(i, "CAS") || (type != null && type != "S32" && type != "F32")) throw Unsup($"global reduction of type {type}");
            IReadOnlyList<Op> o = i.Operands;
            (IrOperand lo, IrOperand hi) = GlobalAddress(ctx, i, o[0]);
            if (type == "F32") { EmitFloatAtomicAdd(ctx, i, StorageKind.GlobalMemory, lo, hi, Src(ctx, o[1])); return; }
            AtomSize size = type == "S32" ? AtomSize.S32 : AtomSize.U32;
            EmitAtomicOp(ctx, StorageKind.GlobalMemory, AtomOpOf(i), size, lo, hi, Src(ctx, o[1]));
        }

        // ---- textures ---------------------------------------------------------------------------------------------

        static (SamplerType type, bool array) DimOf(string dim) => dim switch
        {
            "1D" => (SamplerType.Texture1D, false),
            "2D" => (SamplerType.Texture2D, false),
            "3D" => (SamplerType.Texture3D, false),
            "CUBE" => (SamplerType.TextureCube, false),
            "ARRAY_1D" => (SamplerType.Texture1D | SamplerType.Array, true),
            "ARRAY_2D" => (SamplerType.Texture2D | SamplerType.Array, true),
            "ARRAY_3D" => (SamplerType.Texture3D | SamplerType.Array, true),
            "ARRAY_CUBE" => (SamplerType.TextureCube | SamplerType.Array, true),
            _ => throw Unsup($"texture shape {dim}"),
        };

        /// Splits the operands of TEX/TLD/TLD4/TXQ: Rd2, Rd, Ra, [Rb], bank, handle, [shape], [mask].
        static (Op rd2, Op rd, Op ra, Op rb, int bank, int handle, string dim, int mask) TexOperands(SassInst i) => TexOperands(i, out _, false, out _, false);

        /// The same, optionally accepting the predicate destination after Rd (bit 81: the residency result of a sparse access).
        static (Op rd2, Op rd, Op ra, Op rb, int bank, int handle, string dim, int mask) TexOperands(SassInst i, out Op pred, bool allowPred)
            => TexOperands(i, out pred, allowPred, out _, false);

        /// allowHandles: also the forms without a bound handle, bank and handle -1 then: .B (the handle is the first register of Rb) and
        /// the handle in a uniform register (0xf60, "URn, 0x0": ur = URn).
        /// gatherComponent (TLD4): .B names the blue component when a bound handle follows ("TLD4.SCR.B.F16.RN.PTP R33, R32, R10, R0, 0x0,
        /// 0x30, 2D" in OoT's compute upscaler f90d726d, next to .R and .G gathers of the same texture), not the bindless form.
        static (Op rd2, Op rd, Op ra, Op rb, int bank, int handle, string dim, int mask) TexOperands(SassInst i, out Op pred, bool allowPred, out Op ur, bool allowHandles,
                                                                                                      bool gatherComponent = false)
        {
            IReadOnlyList<Op> o = i.Operands;
            int k = 0;
            pred = null;
            List<Op> regs = new();
            while (k < o.Count && (o[k].Kind == OperandKind.Reg || IsPredKind(o[k])))
            {
                if (IsPredKind(o[k]))
                {
                    if (!allowPred || pred != null) throw Unsup("residency predicate");
                    pred = o[k++];
                    continue;
                }
                regs.Add(o[k++]);
            }
            if (regs.Count < 3) throw Unsup("register operands");
            List<Op> rest = o.Skip(k).ToList();
            List<Op> imms = rest.Where(x => x.Kind == OperandKind.Imm).ToList();
            string dim = rest.FirstOrDefault(x => x.Kind == OperandKind.Other)?.Text;
            ur = rest.FirstOrDefault(x => x.Kind == OperandKind.UReg);
            bool bindlessB = Has(i, "B") && !(gatherComponent && ur == null && imms.Count >= 2);
            if ((ur != null || bindlessB) && !allowHandles) throw Unsup("bindless or unknown handle form");
            if (ur != null)
            {
                if (imms.Count < 1 || imms[0].ImmBits != 0) throw Unsup("uniform handle with an immediate");
                return (regs[0], regs[1], regs[2], regs.Count > 3 ? regs[3] : null, -1, -1, dim, imms.Count > 1 ? (int)imms[1].ImmBits & 0xf : 0xf);
            }
            if (bindlessB)
                return (regs[0], regs[1], regs[2], regs.Count > 3 ? regs[3] : null, -1, -1, dim, imms.Count > 0 ? (int)imms[0].ImmBits & 0xf : 0xf);
            if (imms.Count < 2) throw Unsup("bindless or unknown handle form");
            int mask = imms.Count > 2 ? (int)imms[2].ImmBits & 0xf : 0xf;
            return (regs[0], regs[1], regs[2], regs.Count > 3 ? regs[3] : null, (int)imms[0].ImmBits, (int)imms[1].ImmBits, dim, mask);
        }

        static int CbufSlotOf(EmitterContext ctx, int bank) => bank == AmpereTextureBank ? TextureOperation.DefaultCbufSlot : bank;

        /// Destination registers for a 4-bit component mask: the first two enabled components go to Rd, Rd+1, the next
        /// two to Rd2, Rd2+1 (inferred from the compiler's use of the two destination operands).
        static IrOperand[] TexDests(Op rd, Op rd2, int mask)
        {
            int n = BitOperations.PopCount((uint)mask);
            IrOperand[] d = new IrOperand[n];
            for (int c = 0; c < n; c++)
            {
                Op baseReg = c < 2 ? rd : rd2;
                int r = baseReg.Reg >= 255 ? 255 : baseReg.Reg + (c & 1);
                d[c] = r >= 255 ? Local() : Gpr(r);
            }
            return d;
        }

        static void EmitTexAmpere(EmitterContext ctx, SassInst i, bool fetch)
        {
            (Op rd2, Op rd, Op ra, Op rb, int bank, int handle, string dim, int mask) = TexOperands(i, out _, false, out Op ur, !fetch);
            if (rd.Reg >= 255 && rd2.Reg >= 255) return;
            // Handles that are not in c[2] (NVN2 materials): Ryujinx's bindless sample, which BindlessElimination turns into a texture whose
            // handle the runtime reads at draw time. URn holds the whole handle (the compiler puts handle >> 20, the sampler, in URn+1).
            // .B: the handle is the first register of Rb, the other Rb parameters follow it (2a6233ad: "MOV R64, handle; R65 = depth
            // reference; TEX.SCR.B.DC RZ, R104, R66(x, y), R64").
            bool bindless = bank < 0;
            IrOperand bindlessHandle = null;
            if (ur != null) bindlessHandle = ctx.Copy(UGpr(ur.Reg));
            else if (bindless)
            {
                if (rb == null || rb.Reg >= 255) throw Unsup("bindless handle in RZ");
                bindlessHandle = ctx.Copy(Gpr(rb.Reg));
                rb = rb with { Reg = rb.Reg + 1 };
            }
            foreach (string m in i.Mods)
            {
                if (m == "B" && bindless) continue;
                if (m is not ("SCR" or "LZ" or "LL" or "LB" or "DC" or "AOFFI" or "NODEP" or "MS" or "NDV" or "EF" or "EL" or "LU" or "EU" or "NA" or "F16" or "RN" or "F16.RN")) throw Unsup($"modifier {m}");
            }
            // .F16(.RN): the components come back as FP16, two per register: Rd = (c0, c1), Rd2 = (c2, c3), the first component in the low
            // half, missing ones 0 (Ryujinx's TEXS.F16 rule). Seen in OoT's compute upscaler fea98fe7 ("TEX.SCR.F16.RN.LZ R19, R0, R13, R67,
            // 0x0, 0x30, 2D, 0x7": R0 is read as a half2 and R19 only through R19.H0_H0; the luminance weight 0.2126 goes with R0.H0, red).
            bool f16 = Has(i, "F16") || Has(i, "F16.RN");
            bool scr = Has(i, "SCR");
            (SamplerType type, bool array) = DimOf(dim);
            bool lz = Has(i, "LZ"), ll = Has(i, "LL"), bias = Has(i, "LB"), dc = Has(i, "DC"), aoffi = Has(i, "AOFFI"), ms = Has(i, "MS");
            TextureFlags flags = fetch ? TextureFlags.IntCoords : TextureFlags.None;
            int coords = (type & SamplerType.Mask) switch { SamplerType.Texture1D => 1, SamplerType.Texture2D => 2, _ => 3 };

            // Register parameter lists, in the order the hardware reads them.
            int raN, rbN;
            List<string> raList = new(), rbList = new();
            if (!scr)
            {
                // Same split as Maxwell TEX/TLD: Ra = [array index, coords...], Rb = [lod|bias, offset, dc, ms sample].
                if (array) raList.Add("arr");
                for (int c = 0; c < coords; c++) raList.Add("c" + c);
                if (ll || bias) rbList.Add("lod");
                if (aoffi) rbList.Add("off");
                if (dc) rbList.Add("dc");
                if (ms) rbList.Add("ms");
            }
            else
            {
                // .SCR: the compact Maxwell TEXS/TLDS split (inferred): the parameters [array, coords..., dc, lod,
                // offset, ms] fill Ra then Rb, one each when there are two, else two in Ra and the rest in Rb.
                // Fetch from an array follows the same rule: TLD.SCR ARRAY_2D is Ra = [array index, x], Rb = [y]
                // (0cd3176a: "MOV R34, 0x3; MOV R35, R27(x); TLD.SCR.LZ R35, R2, R34, R20(y), ARRAY_2D"; Rb+1 is never
                // written before it). Reading x from Rb fetched texel (y, 0) for every pixel: one value per screen row.
                List<string> all = new();
                if (array) all.Add("arr");
                for (int c = 0; c < coords; c++) all.Add("c" + c);
                if (dc) all.Add("dc");
                if (ll || bias) all.Add("lod");
                if (aoffi) all.Add("off");
                if (ms) all.Add("ms");
                int split = all.Count <= 2 ? 1 : 2;
                // .B with two parameters: whether both stay in Ra once the handle takes Rb is not known (no such word seen)
                if (bindless && ur == null && all.Count <= 2) throw Unsup("TEX.SCR.B with two parameters");
                raList.AddRange(all.Take(split));
                rbList.AddRange(all.Skip(split));
            }
            raN = raList.Count;
            rbN = rbList.Count;
            if (rbN > 0 && rb == null) throw Unsup("missing Rb");
            Dictionary<string, IrOperand> p = new();
            for (int k = 0; k < raN; k++) p[raList[k]] = ra.Reg >= 255 ? Const(0) : ctx.Copy(Gpr(ra.Reg + k));
            for (int k = 0; k < rbN; k++) p[rbList[k]] = rb.Reg >= 255 ? Const(0) : ctx.Copy(Gpr(rb.Reg + k));

            // 1D fetch from a buffer texture (texelFetch on samplerBuffer).
            if (fetch && type == SamplerType.Texture1D && !array && !ll && !aoffi && !ms &&
                ctx.TranslatorContext.GpuAccessor.QuerySamplerType(handle, CbufSlotOf(ctx, bank)) == SamplerType.TextureBuffer)
            {
                type = SamplerType.TextureBuffer;
            }

            List<IrOperand> sources = new();
            for (int c = 0; c < coords; c++) sources.Add(p["c" + c]);
            bool is1DTo2D = false;
            if (Sample1DAs2D && (type & SamplerType.Mask) == SamplerType.Texture1D)
            {
                sources.Add(fetch ? Const(0) : ConstF(0));
                type = SamplerType.Texture2D | (type & SamplerType.Array);
                is1DTo2D = true;
            }
            if (array) sources.Add(p["arr"]);
            if (dc) { sources.Add(p["dc"]); type |= SamplerType.Shadow; }
            if ((lz || ll) && !ms && type != SamplerType.TextureBuffer)
            {
                sources.Add(ll ? p["lod"] : fetch ? Const(0) : ConstF(0));
                flags |= TextureFlags.LodLevel;
            }
            if (aoffi)
            {
                for (int c = 0; c < coords; c++) sources.Add(ctx.BitfieldExtractS32(p["off"], Const(c * 4), Const(4)));
                if (is1DTo2D) sources.Add(Const(0));
                flags |= TextureFlags.Offset;
            }
            if (bias) { sources.Add(p["lod"]); flags |= TextureFlags.LodBias; }
            if (ms) { sources.Add(p["ms"]); type |= SamplerType.Multisample; }
            if (Has(i, "NDV") && !fetch) { /* no derivatives across the quad: plain sample on the host */ }

            IrOperand[] dests = f16 ? new IrOperand[BitOperations.PopCount((uint)mask)] : TexDests(rd, rd2, mask);
            if (f16) for (int k = 0; k < dests.Length; k++) dests[k] = Local();
            if (bindlessHandle != null)
            {
                sources.Insert(0, bindlessHandle);
                ctx.TextureSample(type, flags | TextureFlags.Bindless, default, mask, dests, sources.ToArray());
            }
            else
            {
                SetBindingPair binding = ctx.ResourceManager.GetTextureOrImageBinding(Instruction.TextureSample, type, TextureFormat.Unknown, flags, CbufSlotOf(ctx, bank), handle);
                ctx.TextureSample(type, flags, binding, mask, dests, sources.ToArray());
            }
            if (f16)
            {
                IrOperand C(int k) => k < dests.Length ? dests[k] : ConstF(0);
                if (rd.Reg < 255) ctx.Copy(Gpr(rd.Reg), ctx.PackHalf2x16(C(0), C(1)));
                if (rd2.Reg < 255 && dests.Length > 2) ctx.Copy(Gpr(rd2.Reg), ctx.PackHalf2x16(C(2), C(3)));
            }
        }

        static void EmitTxdAmpere(EmitterContext ctx, SassInst i)
        {
            // TXD Rd2, Rd, Ra, Rb, bank, handle, dim[, mask]. Ra = [coords..., packed (array index in bits 0..15, AOFFI offsets
            // 4 bits each from bit 16)], as Maxwell TXD (Ryujinx EmitTxd). Rb = the gradients per coordinate, d/dx then d/dy
            // (s: dx dy, t: dx dy, ...): seen in 22f0d8d9, whose Rb is built as [P1.s-P0.s, P2.s-P0.s, P1.t-P0.t, P2.t-P0.t].
            (Op rd2, Op rd, Op ra, Op rb, int bank, int handle, string dim, int mask) = TexOperands(i);
            if (rd.Reg >= 255 && rd2.Reg >= 255) return;
            foreach (string m in i.Mods)
            {
                if (m is not ("AOFFI" or "NODEP" or "EF" or "EL" or "LU" or "EU" or "NA" or "F16.RN")) throw Unsup($"modifier {m}");
            }
            if (rb == null) throw Unsup("missing Rb");
            bool aoffi = Has(i, "AOFFI");
            bool f16 = Has(i, "F16.RN");   // as TEX .F16.RN: Rd = (c0, c1), Rd2 = (c2, c3) as fp16 pairs (no TXD.F16 seen in OoT yet)
            (SamplerType type, bool array) = DimOf(dim);
            int coords = (type & SamplerType.Mask) switch { SamplerType.Texture1D => 1, SamplerType.Texture2D => 2, _ => 3 };
            int a = ra.Reg, bReg = rb.Reg;
            IrOperand RaN() => a >= 255 ? Const(0) : ctx.Copy(Gpr(a++));
            IrOperand RbN() => bReg >= 255 ? ConstF(0) : ctx.Copy(Gpr(bReg++));
            List<IrOperand> sources = new();
            for (int c = 0; c < coords; c++) sources.Add(RaN());
            bool is1DTo2D = false;
            if (Sample1DAs2D && (type & SamplerType.Mask) == SamplerType.Texture1D)
            {
                sources.Add(ConstF(0));
                type = SamplerType.Texture2D | (type & SamplerType.Array);
                is1DTo2D = true;
            }
            IrOperand packed = (array || aoffi) ? RaN() : null;
            if (array) sources.Add(ctx.BitwiseAnd(packed, Const(0xffff)));
            IrOperand[] dx = new IrOperand[coords], dy = new IrOperand[coords];
            for (int c = 0; c < coords; c++) { dx[c] = RbN(); dy[c] = RbN(); }
            sources.AddRange(dx);
            if (is1DTo2D) sources.Add(ConstF(0));
            sources.AddRange(dy);
            if (is1DTo2D) sources.Add(ConstF(0));
            TextureFlags flags = TextureFlags.Derivatives;
            if (aoffi)
            {
                for (int c = 0; c < coords; c++) sources.Add(ctx.BitfieldExtractS32(packed, Const(16 + c * 4), Const(4)));
                if (is1DTo2D) sources.Add(Const(0));
                flags |= TextureFlags.Offset;
            }
            SetBindingPair binding = ctx.ResourceManager.GetTextureOrImageBinding(Instruction.TextureSample, type, TextureFormat.Unknown, flags, CbufSlotOf(ctx, bank), handle);
            if (!f16)
            {
                ctx.TextureSample(type, flags, binding, mask, TexDests(rd, rd2, mask), sources.ToArray());
                return;
            }
            IrOperand[] dests = new IrOperand[BitOperations.PopCount((uint)mask)];
            for (int k = 0; k < dests.Length; k++) dests[k] = Local();
            ctx.TextureSample(type, flags, binding, mask, dests, sources.ToArray());
            IrOperand C(int k) => k < dests.Length ? dests[k] : ConstF(0);
            if (rd.Reg < 255) ctx.Copy(Gpr(rd.Reg), ctx.PackHalf2x16(C(0), C(1)));
            if (rd2.Reg < 255 && dests.Length > 2) ctx.Copy(Gpr(rd2.Reg), ctx.PackHalf2x16(C(2), C(3)));
        }

        static void EmitTmmlAmpere(EmitterContext ctx, SassInst i)
        {
            // TMML[.NDV] Rd2, Rd, Ra, [Rb], bank, handle, dim[, mask]: the LOD the sample would use, as Ryujinx's Maxwell TMML:
            // Ra = [array index, coords...]; components x/y = the computed and the clamped LOD in 8.8 fixed point (GLSL's
            // textureQueryLod order swapped), z/w = 0. Destinations split like TEX (Rd, Rd+1 then Rd2, Rd2+1).
            (Op rd2, Op rd, Op ra, Op rb, int bank, int handle, string dim, int mask) = TexOperands(i, out Op residency, true);
            // Every texture is fully resident on the host (sparse textures are whole images): the residency predicate is true.
            if (residency != null) Dst(ctx, residency, Const(IrConsts.True));
            if (rd.Reg >= 255 && rd2.Reg >= 255) return;
            foreach (string m in i.Mods)
            {
                if (m is not ("LOD" or "NDV" or "NODEP" or "EF" or "EL" or "LU" or "EU" or "NA")) throw Unsup($"modifier {m}");
            }
            (SamplerType type, bool array) = DimOf(dim);
            int coords = (type & SamplerType.Mask) switch { SamplerType.Texture1D => 1, SamplerType.Texture2D => 2, _ => 3 };
            int a = ra.Reg;
            IrOperand RaN() => a >= 255 ? Const(0) : ctx.Copy(Gpr(a++));
            IrOperand arrayIndex = array ? RaN() : null;
            List<IrOperand> sources = new();
            for (int c = 0; c < coords; c++) sources.Add(RaN());
            if (Sample1DAs2D && (type & SamplerType.Mask) == SamplerType.Texture1D)
            {
                sources.Add(ConstF(0));
                type = SamplerType.Texture2D | (type & SamplerType.Array);
            }
            if (array) sources.Add(arrayIndex);
            IrOperand[] src = sources.ToArray();
            SetBindingPair binding = ctx.ResourceManager.GetTextureOrImageBinding(Instruction.Lod, type, TextureFormat.Unknown, TextureFlags.None, CbufSlotOf(ctx, bank), handle);
            IrOperand[] dests = TexDests(rd, rd2, mask);
            int k = 0;
            for (int comp = 0; comp < 4; comp++)
            {
                if ((mask & (1 << comp)) == 0) continue;
                IrOperand d = dests[k++];
                if (comp >= 2) { ctx.Copy(d, Const(0)); continue; }
                IrOperand lod = ctx.Lod(type, TextureFlags.None, binding, comp ^ 1, src);
                ctx.Copy(d, ctx.FP32ConvertToS32(ctx.FPMultiply(lod, ConstF(256.0f))));
            }
        }

        static void EmitFswzaddAmpere(EmitterContext ctx, SassInst i)
        {
            // FSWZADD Rd, Ra, Rb, ops: per quad lane l, Rd = Ra op_l Rb with op_l = bits 32 + 2l..33 + 2l (0 a+b, 1 b-a, 2 a-b,
            // 3 b): Ryujinx's Maxwell SwizzleAdd (same per-lane op table). The neighbour value comes from a SHFL before it.
            IReadOnlyList<Op> o = i.Operands;
            int ops = (int)((i.Lo >> 32) & 0xff);
            Dst(ctx, o[0], ctx.FPSwizzleAdd(Src(ctx, o[1]), Src(ctx, o[2]), ops));
        }

        static void EmitTld4Ampere(EmitterContext ctx, SassInst i)
        {
            (Op rd2, Op rd, Op ra, Op rb, int bank, int handle, string dim, int mask) = TexOperands(i, out _, false, out _, false, gatherComponent: true);
            if (rd.Reg >= 255 && rd2.Reg >= 255) return;
            bool dc = Has(i, "DC"), aoffi = Has(i, "AOFFI"), scr = Has(i, "SCR"), ptp = Has(i, "PTP");
            foreach (string m in i.Mods)
            {
                if (m is not ("SCR" or "R" or "G" or "B" or "A" or "DC" or "AOFFI" or "PTP" or "NODEP" or "EF" or "EL" or "LU" or "EU" or "NA" or "F16" or "RN" or "F16.RN"))
                    throw Unsup($"modifier {m}");
            }
            // .F16(.RN): the four gathered texels come back as FP16 pairs, as TEX.F16: Rd = (t0, t1), Rd2 = (t2, t3) (f90d726d: "TLD4.SCR.R.F16.RN.PTP
            // R13, R12, R10, R0, 0x0, 0x30, 2D", the R, G and B gathers of one position into three register pairs).
            bool f16 = Has(i, "F16") || Has(i, "F16.RN");
            int comp = Has(i, "G") ? 1 : Has(i, "B") ? 2 : Has(i, "A") ? 3 : 0;
            (SamplerType type, bool array) = DimOf(dim ?? "2D");
            if ((type & SamplerType.Mask) != SamplerType.Texture2D || array && (!scr || dc)) throw Unsup("gather shape");
            // .PTP: one offset per gathered texel (textureGatherOffsets), as Maxwell TLD4.PTP (Ryujinx EmitTld4): two Rb registers with
            // eight signed 6-bit fields in 8-bit lanes, x0 y0 x1 y1 | x2 y2 x3 y3. With .SCR the four parameters split as Ra = [x, y],
            // Rb = [offsets 0-1, offsets 2-3] (OoT's compute block 54ecc4b4: "MOV R18, 0xfe00ff; MOV R19, 0xfc00fd; TLD4.SCR.R.PTP R20, R26,
            // R6, R18, 0x0, 0x7c, 2D": taps (-1,0) (-2,0) (-3,0) (-4,0)). Only that shape: no depth compare, no array, no AOFFI with it.
            if (ptp && (dc || array || aoffi)) throw Unsup("TLD4.PTP with DC, array or AOFFI");
            IrOperand Ra(int k) => ra.Reg >= 255 ? Const(0) : ctx.Copy(Gpr(ra.Reg + k));
            IrOperand Rb(int k) => rb == null || rb.Reg >= 255 ? Const(0) : ctx.Copy(Gpr(rb.Reg + k));
            List<IrOperand> sources = new();
            TextureFlags flags = TextureFlags.Gather;
            int rbk = 0;
            if (array)
            {
                // TLD4.SCR on ARRAY_2D: Ra = [array index (U16), x], Rb = [y, (AOFFI offsets)] (b7fef02a: "F2I.U16 R20; TLD4.SCR.R
                // R16, R14, R20, R18, ARRAY_2D" with x in R21 and y in R18, the AOFFI forms with offsets in R19 / R1 / R7)
                IrOperand arr = Ra(0);
                sources.Add(Ra(1)); sources.Add(Rb(rbk++));
                sources.Add(ctx.BitwiseAnd(arr, Const(0xffff)));
                type |= SamplerType.Array;
            }
            else if (scr && !dc && !aoffi && !ptp) { sources.Add(Ra(0)); sources.Add(Rb(rbk++)); }
            else { sources.Add(Ra(0)); sources.Add(Ra(1)); }
            int dcIndex = sources.Count;
            IrOperand dcv = null;
            if (aoffi)
            {
                IrOperand packed = Rb(rbk++);
                for (int c = 0; c < 2; c++) sources.Add(ctx.BitfieldExtractS32(packed, Const(c * 8), Const(6)));
                flags |= TextureFlags.Offset;
            }
            if (ptp)
            {
                IrOperand[] packed = [Rb(rbk++), Rb(rbk++)];
                for (int k = 0; k < 8; k++) sources.Add(ctx.BitfieldExtractS32(packed[k >> 2], Const((k & 3) * 8), Const(6)));
                flags |= TextureFlags.Offsets;
            }
            if (dc) { dcv = Rb(rbk++); sources.Insert(dcIndex, dcv); type |= SamplerType.Shadow; }
            else sources.Add(Const(comp));
            SetBindingPair binding = ctx.ResourceManager.GetTextureOrImageBinding(Instruction.TextureSample, type, TextureFormat.Unknown, flags, CbufSlotOf(ctx, bank), handle);
            if (!f16)
            {
                ctx.TextureSample(type, flags, binding, mask, TexDests(rd, rd2, mask), sources.ToArray());
                return;
            }
            IrOperand[] dests = new IrOperand[BitOperations.PopCount((uint)mask)];
            for (int k = 0; k < dests.Length; k++) dests[k] = Local();
            ctx.TextureSample(type, flags, binding, mask, dests, sources.ToArray());
            IrOperand C(int k) => k < dests.Length ? dests[k] : ConstF(0);
            if (rd.Reg < 255) ctx.Copy(Gpr(rd.Reg), ctx.PackHalf2x16(C(0), C(1)));
            if (rd2.Reg < 255 && dests.Length > 2) ctx.Copy(Gpr(rd2.Reg), ctx.PackHalf2x16(C(2), C(3)));
        }

        static void EmitTxqAmpere(EmitterContext ctx, SassInst i)
        {
            // TXQ Rd2, Rd, Ra(lod), TEX_HEADER_DIMENSION, bank, handle, mask
            IReadOnlyList<Op> o = i.Operands;
            if (o.Count < 6 || o[3].Text != "TEX_HEADER_DIMENSION") throw Unsup(o.Count > 3 ? o[3].Text : "form");
            Op rd2 = o[0], rd = o[1], ra = o[2];
            int bank = (int)o[4].ImmBits, handle = (int)o[5].ImmBits, mask = o.Count > 6 ? (int)o[6].ImmBits & 0xf : 0xf;
            int slot = CbufSlotOf(ctx, bank);
            SamplerType type = ctx.TranslatorContext.GpuAccessor.QuerySamplerType(handle, slot);
            SetBindingPair binding = ctx.ResourceManager.GetTextureOrImageBinding(Instruction.TextureQuerySize, type, TextureFormat.Unknown, TextureFlags.None, slot, handle);
            IrOperand lod = ra.Reg >= 255 ? Const(0) : ctx.Copy(Gpr(ra.Reg));
            IrOperand[] dests = TexDests(rd, rd2, mask);
            int n = 0;
            for (int c = 0; c < 4; c++)
            {
                if ((mask & (1 << c)) == 0) continue;
                // A component past the sampler type's size vector (depth of a 2D texture) reads as 1 (inferred); index 3 is
                // the mip count. Compute texture types are not known, so the accessor's 2D default meets depth queries.
                int dims = ((type & SamplerType.Mask) == SamplerType.TextureCube ? 2 : type.Dimensions) + (type.HasFlag(SamplerType.Array) ? 1 : 0);
                ctx.Copy(dests[n++], c < 3 && c >= dims ? Const(1) : ctx.TextureQuerySize(type, TextureFlags.None, binding, c, [lod]));
            }
        }

        static void EmitSustAmpere(EmitterContext ctx, SassInst i)
        {
            // SUST.P.dim[.IGN] [Ra], Rdata, bank, handle   (formatted store of RGBA)
            IReadOnlyList<Op> o = i.Operands;
            if (!Has(i, "P") || o.Count < 4 || o[0].Kind != OperandKind.Mem || o[1].Kind != OperandKind.Reg) throw Unsup("form");
            string dim = i.Mods.FirstOrDefault(m => m is "1D" or "2D" or "3D" or "1D_BUFFER" or "1D_ARRAY" or "2D_ARRAY") ?? throw Unsup("shape");
            SamplerType type = dim switch
            {
                "1D" => SamplerType.Texture1D, "2D" => SamplerType.Texture2D, "3D" => SamplerType.Texture3D, "1D_BUFFER" => SamplerType.TextureBuffer,
                "1D_ARRAY" => SamplerType.Texture1D | SamplerType.Array, _ => SamplerType.Texture2D | SamplerType.Array,
            };
            if (i.Mods.Any(m => m.Length <= 4 && m.All(ch => ch is 'R' or 'G' or 'B' or 'A'))) throw Unsup("component subset");
            int ra = o[0].Reg;
            if (ra >= 255 || o[0].IndexReg >= 0 || o[0].Offset != 0) throw Unsup("address");
            int bank = (int)o[2].ImmBits, handle = (int)o[3].ImmBits;
            List<IrOperand> sources = new();
            int k = 0;
            for (int c = 0; c < type.Dimensions; c++) sources.Add(ctx.Copy(Gpr(ra + k++)));
            if (Sample1DAs2D && (type & SamplerType.Mask) == SamplerType.Texture1D)
            {
                sources.Add(Const(0));
                type = SamplerType.Texture2D | (type & SamplerType.Array);
            }
            if (type.HasFlag(SamplerType.Array)) sources.Add(ctx.Copy(Gpr(ra + k++)));
            int data = o[1].Reg;
            for (int c = 0; c < 4; c++) sources.Add(data >= 255 ? Const(0) : ctx.Copy(Gpr(data + c)));
            TextureFormat format = Ryujinx.Graphics.Shader.StructuredIr.ShaderProperties.GetTextureFormat(ctx.TranslatorContext.GpuAccessor, handle);
            SetBindingPair binding = ctx.ResourceManager.GetTextureOrImageBinding(Instruction.ImageStore, type, format, TextureFlags.None, CbufSlotOf(ctx, bank), handle);
            ctx.ImageStore(type, format, TextureFlags.None, binding, sources.ToArray());
        }

        static void EmitSuredAmpere(EmitterContext ctx, SassInst i)
        {
            // SURED.D.dim.op[.type] [Ra], Rb, bank, handle: image atomic without a result (Ryujinx's Maxwell SURED: coordinates from Ra,
            // Ra+1, ..., then the array index; the format from the type, or the bound image's for SD32/SD64)
            IReadOnlyList<Op> o = i.Operands;
            if (Has(i, "BA") || o.Count < 4 || o[0].Kind != OperandKind.Mem || o[1].Kind != OperandKind.Reg) throw Unsup("form");
            string dim = i.Mods.FirstOrDefault(m => m is "1D" or "2D" or "3D" or "1D_BUFFER" or "1D_ARRAY" or "2D_ARRAY") ?? throw Unsup("shape");
            SamplerType type = dim switch
            {
                "1D" => SamplerType.Texture1D, "2D" => SamplerType.Texture2D, "3D" => SamplerType.Texture3D, "1D_BUFFER" => SamplerType.TextureBuffer,
                "1D_ARRAY" => SamplerType.Texture1D | SamplerType.Array, _ => SamplerType.Texture2D | SamplerType.Array,
            };
            SuatomOp op = Has(i, "ADD") ? SuatomOp.Add : Has(i, "MIN") ? SuatomOp.Min : Has(i, "MAX") ? SuatomOp.Max : Has(i, "INC") ? SuatomOp.Inc
                : Has(i, "DEC") ? SuatomOp.Dec : Has(i, "AND") ? SuatomOp.And : Has(i, "OR") ? SuatomOp.Or : Has(i, "XOR") ? SuatomOp.Xor : throw Unsup("op");
            SuatomSize size = Has(i, "S32") ? SuatomSize.S32 : Has(i, "U64") ? SuatomSize.U64 : Has(i, "F32.FTZ.RN") ? SuatomSize.F32FtzRn : Has(i, "F16x2.RN") ? SuatomSize.F16x2FtzRn
                : Has(i, "S64") ? SuatomSize.S64 : Has(i, "SD32") ? SuatomSize.Sd32 : Has(i, "SD64") ? SuatomSize.Sd64 : SuatomSize.U32;
            int ra = o[0].Reg;
            if (ra >= 255 || o[0].IndexReg >= 0 || o[0].Offset != 0) throw Unsup("address");
            int bank = (int)o[2].ImmBits, handle = (int)o[3].ImmBits;
            List<IrOperand> sources = new();
            int k = 0;
            for (int c = 0; c < type.Dimensions; c++) sources.Add(ctx.Copy(Gpr(ra + k++)));
            if (Sample1DAs2D && (type & SamplerType.Mask) == SamplerType.Texture1D)
            {
                sources.Add(Const(0));
                type = SamplerType.Texture2D | (type & SamplerType.Array);
            }
            if (type.HasFlag(SamplerType.Array)) sources.Add(ctx.Copy(Gpr(ra + k++)));
            TextureFormat format = size is SuatomSize.Sd32 or SuatomSize.Sd64
                ? Ryujinx.Graphics.Shader.StructuredIr.ShaderProperties.GetTextureFormatAtomic(ctx.TranslatorContext.GpuAccessor, handle)
                : GetTextureFormat(size);
            sources.Add(o[1].Reg >= 255 ? Const(0) : ctx.Copy(Gpr(o[1].Reg)));
            TextureFlags flags = GetAtomicOpFlags(op);
            SetBindingPair binding = ctx.ResourceManager.GetTextureOrImageBinding(Instruction.ImageAtomic, type, format, flags, CbufSlotOf(ctx, bank), handle);
            ctx.ImageAtomic(type, format, flags, binding, sources.ToArray());
        }

        static void EmitSuldAmpere(EmitterContext ctx, SassInst i)
        {
            // SULD.P.dim[.RGBA subset].IGN Rd, [Ra], bank, handle   (formatted load; the components named, or all four, land in Rd, Rd+1, ...
            // packed as Ryujinx's SULD.P does). Corpus forms: SULD.P.2D.R.IGN, SULD.P.2D_ARRAY.R.IGN, SULD.P.2D_ARRAY.IGN.
            IReadOnlyList<Op> o = i.Operands;
            if (!Has(i, "P") || Has(i, "B") || o.Count < 4 || o[0].Kind != OperandKind.Reg || o[1].Kind != OperandKind.Mem) throw Unsup("form");
            string dim = i.Mods.FirstOrDefault(m => m is "1D" or "2D" or "3D" or "1D_BUFFER" or "1D_ARRAY" or "2D_ARRAY") ?? throw Unsup("shape");
            SamplerType type = dim switch
            {
                "1D" => SamplerType.Texture1D, "2D" => SamplerType.Texture2D, "3D" => SamplerType.Texture3D, "1D_BUFFER" => SamplerType.TextureBuffer,
                "1D_ARRAY" => SamplerType.Texture1D | SamplerType.Array, _ => SamplerType.Texture2D | SamplerType.Array,
            };
            string comps = i.Mods.FirstOrDefault(m => m.Length <= 4 && m.Length > 0 && m.All(ch => ch is 'R' or 'G' or 'B' or 'A'));
            int mask = comps == null ? 0xf : (comps.Contains('R') ? 1 : 0) | (comps.Contains('G') ? 2 : 0) | (comps.Contains('B') ? 4 : 0) | (comps.Contains('A') ? 8 : 0);
            int ra = o[1].Reg;
            if (ra >= 255 || o[1].IndexReg >= 0 || o[1].Offset != 0) throw Unsup("address");
            int bank = (int)o[2].ImmBits, handle = (int)o[3].ImmBits;
            List<IrOperand> sources = new();
            int k = 0;
            for (int c = 0; c < type.Dimensions; c++) sources.Add(ctx.Copy(Gpr(ra + k++)));
            if (Sample1DAs2D && (type & SamplerType.Mask) == SamplerType.Texture1D)
            {
                sources.Add(Const(0));
                type = SamplerType.Texture2D | (type & SamplerType.Array);
            }
            if (type.HasFlag(SamplerType.Array)) sources.Add(ctx.Copy(Gpr(ra + k++)));
            int rd = o[0].Reg;
            List<IrOperand> dests = new();
            for (int c = 0; c < BitOperations.PopCount((uint)mask) && rd + c < 255; c++) dests.Add(Gpr(rd + c));
            if (dests.Count == 0) return;   // RZ destination: nothing observable
            TextureFormat format = Ryujinx.Graphics.Shader.StructuredIr.ShaderProperties.GetTextureFormat(ctx.TranslatorContext.GpuAccessor, handle);
            SetBindingPair binding = ctx.ResourceManager.GetTextureOrImageBinding(Instruction.ImageLoad, type, format, TextureFlags.None, CbufSlotOf(ctx, bank), handle);
            ctx.ImageLoad(type, format, TextureFlags.None, binding, mask, dests.ToArray(), sources.ToArray());
        }

        // ---- warp -------------------------------------------------------------------------------------------------

        static void EmitShflAmpere(EmitterContext ctx, SassInst i)
        {
            // SHFL.IDX|UP|DOWN|BFLY Pd, Rd, a, b, c. Only the low 5 bits of the lane / delta / xor mask b count (hwcheck, RTX 3080:
            // SHFL.DOWN by 0x371e0289 reads lane + 9); Ryujinx's helpers use all 32.
            IReadOnlyList<Op> o = i.Operands;
            IrOperand a = Src(ctx, o[2]), b = ctx.BitwiseAnd(Src(ctx, o[3]), Const(0x1f)), c = Src(ctx, o[4]);
            (IrOperand res, IrOperand valid) = Has(i, "IDX") ? ctx.Shuffle(a, b, c) : Has(i, "UP") ? ctx.ShuffleUp(a, b, c)
                : Has(i, "DOWN") ? ctx.ShuffleDown(a, b, c) : Has(i, "BFLY") ? ctx.ShuffleXor(a, b, c) : throw Unsup();
            Dst(ctx, o[1], res);
            Dst(ctx, o[0], valid);
        }

        // A18 diagnosis switch, off by default: NXRT_HARNESS_VOTE_SELF=1 makes VOTE Rd, PT the lane's own bit and REDUX the lane's own value
        // in compute programs. That was the first stopgap for OoT's culling counts; the cause was the translator folding ballots into
        // branches (structured-subgroup-propagation.patch), and the own bit changes warp-wide results the game relies on (a max over the
        // warp became the lane's own value).
        internal static bool VoteSelf = System.Environment.GetEnvironmentVariable("NXRT_HARNESS_VOTE_SELF") == "1";

        static void EmitVoteAmpere(EmitterContext ctx, SassInst i)
        {
            // VOTE[U].ANY|ALL|EQ [Rd,] Pd, Ppred
            IReadOnlyList<Op> o = i.Operands;
            VoteMode mode = Has(i, "ANY") ? VoteMode.Any : Has(i, "ALL") ? VoteMode.All : Has(i, "EQ") ? VoteMode.Eq : throw Unsup();
            IrOperand pred = Src(ctx, o[^1]);
            // NXRT_HARNESS_VOTE_SELF=1 (diagnosis, compute only): VOTE[U].ANY/ALL Rd, PT as the lane's own bit, so every lane is the leader of
            // a warp-aggregated atomic (leader = FLO(mask), count = POPC(mask), SHFL.IDX of the result) under any active mask.
            if (o.Count == 3 && VoteSelf && ctx.TranslatorContext.Definitions.Stage == ShaderStage.Compute && IsPredKind(o[^1]) && o[^1].Reg == 7 &&
                !o[^1].Not)   // Rd = ballot(PT) in every mode (VOTE.ALL Rd, PT, PT too)
                Dst(ctx, o[0], ctx.ShiftLeft(Const(1), LaneIdAmpere(ctx)));
            else if (o.Count == 3) Dst(ctx, o[0], EmitBallot(ctx, pred));
            Op pd = o[^2];
            if (!(IsPredKind(pd) && pd.Reg == 7)) Dst(ctx, pd, EmitVote(ctx, mode, pred));
        }

        /// Value of `v` in lane `lane` (an IDX shuffle over the whole warp).
        static IrOperand ReadLane(EmitterContext ctx, IrOperand v, IrOperand lane) => ctx.Shuffle(v, lane, Const(0x1f)).Item1;

        static void EmitReduxAmpere(EmitterContext ctx, SassInst i)
        {
            // REDUX.AND|OR|XOR|SUM|MIN|MAX[.S32] URd, Ra: Ra reduced over the active lanes into a uniform register. Every lane reads every
            // lane (32 IDX shuffles) and keeps the active ones (ballot), so it is right under any active mask.
            IReadOnlyList<Op> o = i.Operands;
            IrOperand v = Src(ctx, o[1]);
            if (VoteSelf && ctx.TranslatorContext.Definitions.Stage == ShaderStage.Compute)
            {
                // NXRT_HARNESS_VOTE_SELF=1: every lane is a warp of one, so the reduction is the lane's own value
                Dst(ctx, o[0], v);
                return;
            }
            IrOperand mask = EmitBallot(ctx, Const(IrConsts.True));
            bool s = Has(i, "S32");
            Func<IrOperand, IrOperand, IrOperand> f =
                Has(i, "SUM") ? ctx.IAdd : Has(i, "AND") ? ctx.BitwiseAnd : Has(i, "OR") ? ctx.BitwiseOr : Has(i, "XOR") ? ctx.BitwiseExclusiveOr
                : Has(i, "MIN") ? (s ? ctx.IMinimumS32 : ctx.IMinimumU32) : Has(i, "MAX") ? (s ? ctx.IMaximumS32 : ctx.IMaximumU32) : throw Unsup();
            // start from the first active lane's value, then fold in the other active lanes
            IrOperand first = ctx.FindLSB(mask);
            IrOperand acc = ReadLane(ctx, v, first);
            for (int l = 0; l < 32; l++)
            {
                IrOperand active = ctx.ICompareNotEqual(ctx.BitwiseAnd(mask, Const(1 << l)), Const(0));
                IrOperand take = ctx.BitwiseAnd(active, ctx.ICompareNotEqual(first, Const(l)));
                // Only active lanes are read: an inactive lane's slot reads the first active lane instead (its value is not taken). Reading
                // a lane that is not active is undefined in SPIR-V, and on NVIDIA it waited for lanes that never come: a mesh program's lanes
                // past its count leave at the start and wait at the end-of-program barrier while the others REDUX (OoT mesh f3054ac2,
                // device lost once the GPU culling gave the mesh draws work).
                IrOperand src = ctx.ConditionalSelect(active, Const(l), first);
                acc = ctx.ConditionalSelect(take, f(acc, ReadLane(ctx, v, src)), acc);
            }
            Dst(ctx, o[0], acc);
        }

        static void EmitMatchAmpere(EmitterContext ctx, SassInst i)
        {
            // MATCH.ALL[.U64] [Pd,] Rd, Ra: Pd = every active lane has the same Ra (Ra pair for .U64); Rd = the active mask then, else 0
            IReadOnlyList<Op> o = i.Operands;
            if (Has(i, "ANY"))
            {
                // MATCH.ANY[.U64] Rd, Ra: the active lanes whose Ra (pair) equals this lane's
                Op rs = o[^1];
                if (rs.Kind != OperandKind.Reg) throw Unsup("source");
                IrOperand act = EmitBallot(ctx, Const(IrConsts.True));
                IrOperand vlo = Src(ctx, rs), vhi = Has(i, "U64") ? (rs.Reg < 254 ? Gpr(rs.Reg + 1) : Const(0)) : null;
                IrOperand m = Const(0);
                for (int l = 0; l < 32; l++)
                {
                    IrOperand same = ctx.ICompareEqual(ReadLane(ctx, vlo, Const(l)), vlo);
                    if (vhi != null) same = ctx.BitwiseAnd(same, ctx.ICompareEqual(ReadLane(ctx, vhi, Const(l)), vhi));
                    m = ctx.BitwiseOr(m, ctx.ConditionalSelect(same, Const(1 << l), Const(0)));
                }
                Dst(ctx, o[^2], ctx.BitwiseAnd(m, act));
                return;
            }
            int k = IsPredKind(o[0]) ? 1 : 0;
            Op ra = o[k + 1];
            if (ra.Kind != OperandKind.Reg) throw Unsup("source");
            IrOperand mask = EmitBallot(ctx, Const(IrConsts.True));
            IrOperand first = ctx.FindLSB(mask);
            IrOperand lo = Src(ctx, ra);
            IrOperand eq = ctx.ICompareEqual(lo, ReadLane(ctx, lo, first));
            if (Has(i, "U64"))
            {
                IrOperand hi = ra.Reg < 254 ? Gpr(ra.Reg + 1) : Const(0);
                eq = ctx.BitwiseAnd(eq, ctx.ICompareEqual(hi, ReadLane(ctx, hi, first)));
            }
            IrOperand all = EmitVote(ctx, VoteMode.All, eq);
            Dst(ctx, o[k], ctx.ConditionalSelect(all, mask, Const(0)));
            if (k == 1) Dst(ctx, o[0], all);
        }
    }
}
