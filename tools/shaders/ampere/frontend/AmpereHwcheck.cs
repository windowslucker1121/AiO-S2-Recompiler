// SPDX-License-Identifier: MIT
// Hardware-check entry point of the Ampere front end (tools/shaders/hwcheck): a compute program that runs a few decoded
// SASS instructions on a register file kept in a storage buffer, so the translated instructions can be compared with the
// same instruction words run on a real Ampere GPU.
//
// Per invocation (index = workgroup id * local size x + local id x) the buffer holds HwcheckWords words:
//   word 0..15  R0..R15
//   word 16     predicates: bit n = Pn (n = 0..6)
//   word 17..19 unused (padding, the 16-byte alignment the GPU side's 128-bit loads need)
//   word 20..27 memory window M (32 bytes) for the memory tests, see `mem` below
// The program loads R0..R15 and P0..P6, emits the test instructions with the normal emitter (InstEmit.EmitAmpere, guards
// included), then stores R0..R15 and the predicate word back. Any other register an instruction names reads as undefined, so
// the caller keeps every register operand in R0..R15.
//
// Memory tests (`mem`, a cases.tsv column of tools/shaders/hwcheck): the window M is where the test instruction's access lands.
//   shared       M is copied to shared memory at byte index * 32 before the test and copied back after it (32 KB declared);
//   local:W      M is copied to local memory at byte W before and back after (2 KB declared);
//   global:a:z   the test addresses the storage buffer itself: Ra holds M's byte offset in this invocation's record (minus the
//                instruction's immediate) and becomes storage-buffer base + record offset + Ra, so the front end's normal global
//                path (Ryujinx GlobalToStorage) finds the buffer; Ra+1 becomes 0. The registers in z (comma list) are zeroed after
//                the test, because the GPU side's address is a real pointer and would never compare equal.
using Nx.Shaders.Ampere;
using Ryujinx.Graphics.Shader.Decoders;
using Ryujinx.Graphics.Shader.Instructions;
using Ryujinx.Graphics.Shader.IntermediateRepresentation;
using System.Collections.Generic;
using System.Linq;
using static Ryujinx.Graphics.Shader.IntermediateRepresentation.OperandHelper;

namespace Ryujinx.Graphics.Shader.Translation
{
    public static partial class AmpereTranslator
    {
        public const int HwcheckRegs = 16, HwcheckPredWord = 16, HwcheckWords = 28, HwcheckMemWord = 20, HwcheckMemWords = 8;
        public const int HwcheckSharedBytes = 32768, HwcheckLocalBytes = 2048;

        /// Builds the hardware-check compute program around `test` (straight-line instructions, no control flow).
        /// The storage buffer is the translator's storage buffer slot 0 (sbCbSlot 0, offset 0x10).
        public static TranslatorContext CreateHwcheckContext(IGpuAccessor gpu, TranslationOptions options, IReadOnlyList<SassInst> test, out AmpereProgram program, string mem = null)
        {
            string[] mf = string.IsNullOrEmpty(mem) ? new[] { "" } : mem.Split(':');
            string memKind = mf[0];
            int memLocalBase = memKind == "local" ? int.Parse(mf[1]) : 0;
            int memAddrReg = memKind == "global" ? int.Parse(mf[1]) : -1;
            int[] memZero = memKind == "global" && mf.Length > 2 && mf[2].Length > 0 ? mf[2].Split(',').Select(int.Parse).ToArray() : System.Array.Empty<int>();
            program = new AmpereProgram(test.ToList(), gpu);
            AmpereProgram prog = program;
            AmpereFuse.Analyse(prog);   // a FMUL.RZ + MUFU.SIN pair checks the fused form
            int localX = gpu.QueryComputeLocalSizeX();
            ShaderDefinitions definitions = new(ShaderStage.Compute, localX, gpu.QueryComputeLocalSizeY(), gpu.QueryComputeLocalSizeZ());
            prog.Stage = ShaderStage.Compute;

            AttributeUsage attributeUsage = new(gpu);
            FeatureFlags features = FeatureFlags.Store;
            byte clip = 0;
            foreach (SassInst i in test) ScanFeatures(definitions, attributeUsage, ref features, ref clip, i);

            int binding = -1;
            IntermediateRepresentation.Operand baseWord = null, memIdx = null;

            void Prologue(EmitterContext ctx)
            {
                if (!ctx.ResourceManager.TryGetStorageBufferBinding(0, 0x10, true, out binding))
                    throw new System.InvalidOperationException("no storage buffer slot");
                IntermediateRepresentation.Operand tid = ctx.Load(StorageKind.Input, IoVariable.ThreadId, null, Const(0));
                IntermediateRepresentation.Operand cta = ctx.Load(StorageKind.Input, IoVariable.CtaId, null, Const(0));
                IntermediateRepresentation.Operand idx = ctx.IAdd(ctx.IMultiply(cta, Const(localX)), tid);
                baseWord = ctx.IMultiply(idx, Const(HwcheckWords));
                for (int r = 0; r < HwcheckRegs; r++)
                    ctx.Copy(Register(r, RegisterType.Gpr), ctx.Load(StorageKind.StorageBuffer, binding, Const(0), ctx.IAdd(baseWord, Const(r))));
                IrPreds(ctx, ctx.Load(StorageKind.StorageBuffer, binding, Const(0), ctx.IAdd(baseWord, Const(HwcheckPredWord))));
                if (memKind is "shared" or "local")
                {
                    for (int mw = 0; mw < HwcheckMemWords; mw++)
                        ctx.Store(MemKind(), MemId(ctx), MemWordOffset(ctx, idx, mw), ctx.Load(StorageKind.StorageBuffer, binding, Const(0), ctx.IAdd(baseWord, Const(HwcheckMemWord + mw))));
                }
                else if (memKind == "global")
                {
                    IntermediateRepresentation.Operand rec = ctx.ShiftLeft(baseWord, Const(2));
                    ctx.Copy(Register(memAddrReg, RegisterType.Gpr), ctx.IAdd(Cbuf(0, 0x10), ctx.IAdd(rec, Register(memAddrReg, RegisterType.Gpr))));
                    ctx.Copy(Register(memAddrReg + 1, RegisterType.Gpr), Const(0));
                }
                memIdx = idx;
            }

            StorageKind MemKind() => memKind == "shared" ? StorageKind.SharedMemory : StorageKind.LocalMemory;
            int MemId(EmitterContext ctx) => memKind == "shared" ? ctx.ResourceManager.SharedMemoryId : ctx.ResourceManager.LocalMemoryId;
            IntermediateRepresentation.Operand MemWordOffset(EmitterContext ctx, IntermediateRepresentation.Operand idx, int w) =>
                memKind == "shared" ? ctx.IAdd(ctx.IMultiply(idx, Const(HwcheckMemWords)), Const(w)) : Const(memLocalBase / 4 + w);

            void Epilogue(EmitterContext ctx)
            {
                if (memKind is "shared" or "local")
                {
                    for (int mw = 0; mw < HwcheckMemWords; mw++)
                        ctx.Store(StorageKind.StorageBuffer, binding, Const(0), ctx.IAdd(baseWord, Const(HwcheckMemWord + mw)), ctx.Load(MemKind(), MemId(ctx), MemWordOffset(ctx, memIdx, mw)));
                }
                foreach (int r in memZero) ctx.Copy(Register(r, RegisterType.Gpr), Const(0));
                for (int r = 0; r < HwcheckRegs; r++)
                    ctx.Store(StorageKind.StorageBuffer, binding, Const(0), ctx.IAdd(baseWord, Const(r)), Register(r, RegisterType.Gpr));
                IntermediateRepresentation.Operand w = Const(0);
                for (int p = 0; p < 7; p++)
                    w = ctx.BitwiseOr(w, ctx.ConditionalSelect(Register(p, RegisterType.Predicate), Const(1 << p), Const(0)));
                ctx.Store(StorageKind.StorageBuffer, binding, Const(0), ctx.IAdd(baseWord, Const(HwcheckPredWord)), w);
                ctx.Return();
            }

            Block block = new(0);
            ulong addr = 0;
            block.OpCodes.Add(new InstOp(addr, 7UL << 16, InstName.Nop, Prologue, InstProps.None));
            foreach (SassInst i in test)
            {
                SassInst captured = i;
                addr += 0x10;
                block.OpCodes.Add(new InstOp(addr, 7UL << 16, InstName.Nop, ctx => InstEmit.EmitAmpere(ctx, prog, captured, null), InstProps.None));
            }
            addr += 0x10;
            block.OpCodes.Add(new InstOp(addr, 7UL << 16, InstName.Nop, Epilogue, InstProps.None));
            block.EndAddress = addr + 0x10;

            DecodedFunction main = new(0);
            main.SetBlocks(new[] { block });
            if (memKind == "shared") features |= FeatureFlags.SharedMemory;
            if (memKind == "local") features |= FeatureFlags.LocalMemory;
            DecodedProgram decoded = new(main, new Dictionary<ulong, DecodedFunction> { [0] = main }, attributeUsage, features, clip, 0);
            return new TranslatorContext(0, (int)block.EndAddress, memKind == "local" ? HwcheckLocalBytes : 0, definitions, gpu, options, decoded);
        }

        static void IrPreds(EmitterContext ctx, IntermediateRepresentation.Operand word)
        {
            for (int p = 0; p < 7; p++)
                ctx.Copy(Register(p, RegisterType.Predicate), ctx.ICompareNotEqual(ctx.BitwiseAnd(word, Const(1 << p)), Const(0)));
        }
    }
}
