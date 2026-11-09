// SPDX-License-Identifier: MIT
// HMMA on VK_KHR_cooperative_matrix: one OpCooperativeMatrixMulAddKHR per HMMA instead of the subgroup emulation in
// AmpereEmit.Tensor.cs (32 shuffles and 64 fused multiply-adds per lane and instruction).
//
// The SASS registers stay the program's state: every HMMA builds its A, B and C cooperative matrices from the lane's own register
// halves with OpCompositeInsert and takes D apart with OpCompositeExtract. That is only right when the cooperative matrix gives
// each invocation the same elements the SASS fragment gives its lane (AmpereEmit.Tensor.cs, "Fragment layouts"). Vulkan leaves the
// per-invocation element order to the implementation, so:
//   - the order is a table here (the PTX fragment element each cooperative-matrix component index holds; the default is the one
//     measured on an RTX 4080, driver 610.88, with `hwcheck --coopmat`: tools/shaders/hwcheck/CoopMat.cs);
//   - the runtime runs the same probe on its device before it uses a module made this way (runtime/src/gfx/vk_coopmat_probe.cpp) and falls back to
//     the scalar module when the device's order differs or the device lacks 16x8x16 float16 subgroup matrices (or 32-lane
//     subgroups).
// The arithmetic is the tensor core's own (the driver lowers the multiply-add to HMMA), so the result is the hardware's, which the
// scalar emulation matches only within the HMMA tolerance (hwcheck README, "Tolerances").
//
// A cooperative-matrix operation must be reached by the whole subgroup, as HMMA needs the whole warp: the game's code already obeys
// that (OoT's network passes run their HMMAs in straight-line code between barriers). Compute stage only (32-lane subgroups).
//
// Used when the harness translates a block in its cooperative-matrix variant (Active, per thread; NXRT_HARNESS_COOPMAT=1 turns
// it on for every translation): the harness writes it as <hash>.cm.spv next to <hash>.spv (Program.cs TranslateVariants), and the
// runtime loads it on a device that passed the probe. Shapes: HMMA.16816.F16 and .F32 (no .BF16; 1688 keeps the emulation).
using Nx.Shaders.Ampere;
using Ryujinx.Graphics.Shader.IntermediateRepresentation;
using Ryujinx.Graphics.Shader.StructuredIr;
using Ryujinx.Graphics.Shader.Translation;
using System;
using System.Collections.Generic;
using System.Linq;
using System.Reflection;
using static Spv.Specification;
using IrOperand = Ryujinx.Graphics.Shader.IntermediateRepresentation.Operand;
using SpvInstruction = Spv.Generator.Instruction;

namespace Ryujinx.Graphics.Shader.CodeGen.Spirv
{
    partial class CodeGenContext
    {
        internal AmpereCoopMat.Types CoopMatTypes;
        internal SpvInstruction HalfType;   // float16, declared by the first Float16Arithmetic (AmpereCoopMat.HalfTypeOf)
    }

    public static class AmpereCoopMat
    {
        const Op OpTypeCooperativeMatrixKHR = (Op)4456;
        const Op OpCooperativeMatrixMulAddKHR = (Op)4459;
        const Capability CooperativeMatrixKHR = (Capability)6022;
        const int UseA = 0, UseB = 1, UseAccumulator = 2, ScopeSubgroup = 3;

        /// The translation of the current thread uses cooperative matrices for HMMA.
        [ThreadStatic] static bool _active;
        /// NXRT_HARNESS_COOPMAT=1: every translation takes the cooperative-matrix path (the harness then makes no separate variant).
        public static readonly bool EnvActive = Environment.GetEnvironmentVariable("NXRT_HARNESS_COOPMAT") == "1";
        public static bool Active { get => _active || EnvActive; set => _active = value; }

        /// Set by a translation that emitted at least one cooperative-matrix HMMA.
        [ThreadStatic] public static bool Used;
        /// Set by a translation (scalar or not) with an HMMA this path can take: the harness then makes the variant too.
        [ThreadStatic] public static bool Eligible;

        // ---- native float16 for packed-half arithmetic (any stage) ---------------------------------------------------------
        // The float16 variant of a block (<hash>.f16.spv, harness Program.cs TranslateVariants): HADD2 / HMUL2 / HFMA2 with a half
        // result computed in float16 (TryHalf), every stage. The module declares Float16 and the float-controls execution modes
        // DenormPreserve 16 and SignedZeroInfNanPreserve 16, so the runtime loads it only on a device with shaderFloat16 and both
        // float16 properties (runtime nvn_shaders.cpp read_spv); the plain <hash>.spv (fp32 arithmetic, HFMA2 rounded to odd in fp64:
        // AmpereEmit.cs HalfFmaOdd) stays the fallback. The cooperative-matrix variant always computes them in float16.

        [ThreadStatic] static bool _half;
        /// NXRT_HARNESS_F16=1: every translation computes packed-half arithmetic in float16 (the harness then makes no separate variant).
        public static readonly bool EnvHalf = Environment.GetEnvironmentVariable("NXRT_HARNESS_F16") == "1";
        public static bool HalfActive { get => _half || EnvHalf || Active; set => _half = value; }
        /// Set by a translation with packed-half arithmetic that TryHalf can take: the harness then makes the float16 variant too.
        [ThreadStatic] public static bool HalfEligible;

        // Element order: component index i of the cooperative matrix holds PTX fragment element Order[i] of the lane
        // (element e = register e >> 1, half e & 1; AmpereEmit.Tensor.cs). A (16 x 16): 8, B (16 x 8): 4, C/D F16 (16 x 8): 4,
        // C/D F32 (16 x 8): 4 (one per register). NXRT_HARNESS_COOPMAT_ORDER="A=..;B=..;C=..;F=.." overrides (probing).
        public static int[] OrderA = { 0, 1, 4, 5, 2, 3, 6, 7 };   // NVIDIA: A registers 1 and 2 swapped
        public static int[] OrderB = { 0, 1, 2, 3 };
        public static int[] OrderC = { 0, 1, 2, 3 };
        public static int[] OrderC32 = { 0, 1, 2, 3 };

        static AmpereCoopMat()
        {
            string o = Environment.GetEnvironmentVariable("NXRT_HARNESS_COOPMAT_ORDER");
            if (string.IsNullOrEmpty(o)) return;
            foreach (string part in o.Split(';', StringSplitOptions.RemoveEmptyEntries))
            {
                string[] kv = part.Split('=');
                int[] v = Array.ConvertAll(kv[1].Split(','), int.Parse);
                switch (kv[0].Trim())
                {
                    case "A": OrderA = v; break;
                    case "B": OrderB = v; break;
                    case "C": OrderC = v; break;
                    case "F": OrderC32 = v; break;
                }
            }
        }

        /// The orders as one text (the runtime probe and the harness report print it).
        public static string OrderText() =>
            $"A={string.Join(",", OrderA)};B={string.Join(",", OrderB)};C={string.Join(",", OrderC)};F={string.Join(",", OrderC32)}";

        // ---- front end ---------------------------------------------------------------------------------------------------

        /// HMMA.16816.{F16|F32} Rd, Ra, Rb, Rc as one cooperative-matrix instruction. False: the caller emulates it.
        internal static bool TryEmit(EmitterContext ctx, SassInst i, Func<Nx.Shaders.Ampere.Operand, int, IrOperand> reg,
                                     Action<int, IrOperand> dst)
        {
            bool k16 = i.Mods.Contains("16816"), f32 = i.Mods.Contains("F32"), bf16 = i.Mods.Contains("BF16");
            if (!k16 || bf16) return false;
            if (ctx.TranslatorContext.Definitions.Stage != ShaderStage.Compute) return false;   // subgroups of 32 known only there
            Eligible = true;
            if (!Active) return false;
            IReadOnlyList<Nx.Shaders.Ampere.Operand> o = i.Operands;
            int cRegs = f32 ? 4 : 2;
            var src = new List<IrOperand>(10);
            for (int q = 0; q < 4; q++) src.Add(reg(o[1], q));
            for (int q = 0; q < 2; q++) src.Add(reg(o[2], q));
            for (int q = 0; q < cRegs; q++) src.Add(reg(o[3], q));
            var dests = new IrOperand[cRegs];
            for (int q = 0; q < cRegs; q++) dests[q] = OperandHelper.Local();
            ctx.Add(new Operation(Instruction.CooperativeMatrixMulAdd, (1 << cRegs) - 1, dests, src.ToArray()));
            for (int q = 0; q < cRegs; q++) dst(q, dests[q]);
            Used = true;
            return true;
        }

        /// Packed-half arithmetic of the variant (HADD2 / HMUL2 / HFMA2 with a half result): one lane's add (kind 0), multiply (1) or
        /// fused multiply-add (2) of fp32 operands holding fp16 values, done in float16. The hardware's own rounding (once, to nearest)
        /// instead of fp32 arithmetic rounded twice (and, for HFMA2, the fp64 round-to-odd step that avoids that: AmpereEmit.cs
        /// HalfFmaOdd), which made most of the network passes' cost after HMMA (and up to ~100 fp64 operations in each of OoT's hot
        /// material fragment shaders). Null outside the cooperative-matrix and float16 variants: the caller emits fp32.
        internal static IrOperand TryHalf(EmitterContext ctx, int kind, IrOperand a, IrOperand b, IrOperand c)
        {
            HalfEligible = true;
            if (!HalfActive) return null;
            IrOperand d = OperandHelper.Local();
            ctx.Add(new Operation(Instruction.Float16Arithmetic, kind, d, kind == 2 ? new[] { a, b, c } : new[] { a, b }));
            return d;
        }

        internal static OperationResult GenerateFloat16(CodeGenContext c, AstOperation op)
        {
            SpvInstruction f16 = HalfTypeOf(c);
            SpvInstruction f32 = c.TypeFP32();
            SpvInstruction H(int s) => c.FConvert(f16, c.GetFP32(op.GetSource(s)));   // exact: the operands are fp16 values
            SpvInstruction r = op.Index switch
            {
                0 => c.FAdd(f16, H(0), H(1)),
                1 => c.FMul(f16, H(0), H(1)),
                2 => c.GlslFma(f16, H(0), H(1), H(2)),
                _ => throw new InvalidOperationException($"Float16Arithmetic kind {op.Index}"),
            };
            if (op.Index != 2) c.Decorate(r, Decoration.NoContraction);   // HMUL2 then HADD2 are two roundings, never one fma
            return new OperationResult(AggregateType.FP32, c.FConvert(f32, r));
        }

        /// float16, plus what float16 arithmetic needs to round as the hardware does: fp16 denormals kept (the conversions of fp16
        /// denormal operands and results included) and no fast-math on signed zeros, infinities and NaN. Vulkan leaves both to the
        /// implementation otherwise.
        static SpvInstruction HalfTypeOf(CodeGenContext c)
        {
            if (c.HalfType != null) return c.HalfType;
            c.AddCapability(Capability.Float16);
            c.AddCapability(Capability.DenormPreserve);
            c.AddCapability(Capability.SignedZeroInfNanPreserve);
            c.AddExtension("SPV_KHR_float_controls");
            SpvInstruction main = c.GetFunction(0).Item2;
            c.AddExecutionMode(main, ExecutionMode.DenormPreserve, (Spv.Generator.LiteralInteger)16);
            c.AddExecutionMode(main, ExecutionMode.SignedZeroInfNanPreserve, (Spv.Generator.LiteralInteger)16);
            c.HalfType = c.TypeFloat(16);
            return c.HalfType;
        }

        // ---- SPIR-V back end ---------------------------------------------------------------------------------------------

        internal sealed class Types
        {
            public SpvInstruction F16, V2F16, A, B, C16, C32;
        }

        static readonly MethodInfo AddTypeDeclaration =
            typeof(Spv.Generator.Module).GetMethod("AddTypeDeclaration", BindingFlags.NonPublic | BindingFlags.Instance)
            ?? throw new MissingMethodException("Spv.Generator.Module.AddTypeDeclaration");

        static SpvInstruction MatrixType(CodeGenContext c, SpvInstruction component, int rows, int cols, int use)
        {
            // operands are ids: the constants exist before the type (declarations are written in id order)
            SpvInstruction scope = c.Constant(c.TypeU32(), ScopeSubgroup);
            SpvInstruction r = c.Constant(c.TypeU32(), rows), k = c.Constant(c.TypeU32(), cols), u = c.Constant(c.TypeU32(), use);
            SpvInstruction t = c.NewInstruction(OpTypeCooperativeMatrixKHR);
            t.AddOperand(component);
            t.AddOperand(scope);
            t.AddOperand(r);
            t.AddOperand(k);
            t.AddOperand(u);
            AddTypeDeclaration.Invoke(c, new object[] { t, false });
            return t;
        }

        static Types TypesOf(CodeGenContext c)
        {
            if (c.CoopMatTypes != null) return c.CoopMatTypes;
            c.AddCapability(Capability.Float16);
            c.AddCapability(CooperativeMatrixKHR);
            c.AddExtension("SPV_KHR_cooperative_matrix");
            var t = new Types();
            t.F16 = HalfTypeOf(c);
            t.V2F16 = c.TypeVector(t.F16, 2);
            t.A = MatrixType(c, t.F16, 16, 16, UseA);
            t.B = MatrixType(c, t.F16, 16, 8, UseB);
            t.C16 = MatrixType(c, t.F16, 16, 8, UseAccumulator);
            t.C32 = MatrixType(c, c.TypeFP32(), 16, 8, UseAccumulator);
            c.CoopMatTypes = t;
            return t;
        }

        /// Sources: A0..A3, B0..B1, C0..C1 (F16) or C0..C3 (F32); result: the D words as a vector.
        internal static OperationResult GenerateMulAdd(CodeGenContext c, AstOperation op)
        {
            Types t = TypesOf(c);
            bool f32 = op.SourcesCount == 10;
            if (!f32 && op.SourcesCount != 8) throw new InvalidOperationException($"CooperativeMatrixMulAdd with {op.SourcesCount} sources");
            SpvInstruction u32 = c.TypeU32();
            var words = new SpvInstruction[op.SourcesCount];
            for (int s = 0; s < op.SourcesCount; s++) words[s] = c.GetU32(op.GetSource(s));

            // the 16-bit halves of n registers starting at source `first`: element e = register e >> 1, half e & 1
            SpvInstruction[] Halves(int first, int n)
            {
                var h = new SpvInstruction[2 * n];
                for (int q = 0; q < n; q++)
                {
                    SpvInstruction v = c.Bitcast(t.V2F16, words[first + q]);
                    h[2 * q] = c.CompositeExtract(t.F16, v, 0);
                    h[2 * q + 1] = c.CompositeExtract(t.F16, v, 1);
                }
                return h;
            }
            SpvInstruction Build(SpvInstruction type, SpvInstruction[] elems, int[] order)
            {
                SpvInstruction m = c.Undef(type);
                for (int i = 0; i < order.Length; i++) m = c.CompositeInsert(type, elems[order[i]], m, i);
                return m;
            }

            SpvInstruction a = Build(t.A, Halves(0, 4), OrderA);
            SpvInstruction b = Build(t.B, Halves(4, 2), OrderB);
            SpvInstruction cm, ctype;
            if (f32)
            {
                var e = new SpvInstruction[4];
                for (int q = 0; q < 4; q++) e[q] = c.Bitcast(c.TypeFP32(), words[6 + q]);
                ctype = t.C32;
                cm = Build(ctype, e, OrderC32);
            }
            else
            {
                ctype = t.C16;
                cm = Build(ctype, Halves(6, 2), OrderC);
            }

            // Spv.Generator has no KHR cooperative-matrix instructions and keeps id allocation private: an OpSelect of the same
            // operand shape gets the id and its place in the function, then becomes the multiply-add.
            SpvInstruction d = c.Select(ctype, a, b, cm);
            d.Set(OpCooperativeMatrixMulAddKHR, d.Id, ctype);
            d.AddOperand(a);
            d.AddOperand(b);
            d.AddOperand(cm);

            int n = f32 ? 4 : 2;
            var outw = new SpvInstruction[n];
            if (f32)
            {
                // component i holds fragment element OrderC32[i] = register OrderC32[i]
                for (int i = 0; i < 4; i++) outw[OrderC32[i]] = c.Bitcast(u32, c.CompositeExtract(c.TypeFP32(), d, i));
            }
            else
            {
                var h = new SpvInstruction[4];
                for (int i = 0; i < 4; i++) h[OrderC[i]] = c.CompositeExtract(t.F16, d, i);
                for (int q = 0; q < 2; q++) outw[q] = c.Bitcast(u32, c.CompositeConstruct(t.V2F16, h[2 * q], h[2 * q + 1]));
            }
            SpvInstruction vec = c.CompositeConstruct(c.TypeVector(u32, n), outw);
            return new OperationResult(AggregateType.U32 | (n == 4 ? AggregateType.Vector4 : AggregateType.Vector2), vec);
        }
    }
}
