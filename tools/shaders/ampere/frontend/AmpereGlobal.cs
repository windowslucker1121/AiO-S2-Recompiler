// SPDX-License-Identifier: MIT
// Global memory accesses whose address the translator cannot trace to one storage descriptor (A17, AGENTS.md). Ryujinx's GlobalToStorage
// dropped them (loads read 0, stores and atomics vanished); 3,485 of OoT's blocks had such accesses, among them the counters of its GPU
// culling, so every GPU-culled draw got zero work. Two steps now resolve them at run time:
//  1. Like the Lichii emulator (zortos293/Lichii, global_memory_to_storage_buffer_pass.cpp and DefineGlobalMemoryFunctions): the address is
//     checked against every storage descriptor slot of the stage's c[0] ({address, size} words); the first range that holds it is used.
//     The harness sets the candidates per block (StaticGpuAccessor.GlobalFallbackWords) and adds the resource token "gmem" when a block
//     uses them: the runtime then fills that stage's c[0] descriptors per draw (graphics_c0) or per dispatch (fill_compute_c0).
//  2. Our own (Lichii has nothing for it): when no bound buffer holds the address (pointers read from memory or uniform tables, e.g. the
//     culling root 4c1e8564's counters), it is looked up in the runtime's pool address table: every imported game memory range as
//     {guest address, size, device address} (32-byte entries sorted by guest address; c[0] 0xff8 holds the table's device address, 0xff0
//     the entry count; nvn_replay.cpp pool_table_words). A 12-step binary search (no loop: 4096 entries at most) finds the range, and the
//     access goes through a PhysicalStorageBuffer pointer to device address + (address - guest base). Nothing found still reads 0.
//     Such accesses stay StorageKind.GlobalMemory operations; the SPIR-V back end turns them into pointer accesses (DevicePointer) and the
//     module gets PhysicalStorageBufferAddresses / PhysicalStorageBuffer64 (Declare). Only 32-bit accesses (64/128-bit ones are split into
//     32-bit words by the front end; 8/16-bit ones keep reading 0).
// Hooks: translator/spirv-g2s-c0-base.patch (GlobalToStorage), translator/ampere-mesh-instructions.patch (GetStoragePointer, aligned
// loads/stores), translator/spirv-else-if.patch (Declare).
using Ryujinx.Graphics.Shader.CodeGen.Spirv;
using Ryujinx.Graphics.Shader.IntermediateRepresentation;
using Ryujinx.Graphics.Shader.StructuredIr;
using Spv.Generator;
using System;
using System.Collections.Generic;
using static Ryujinx.Graphics.Shader.IntermediateRepresentation.OperandHelper;
using static Spv.Specification;
using SpvInstruction = Spv.Generator.Instruction;
using IrInstruction = Ryujinx.Graphics.Shader.IntermediateRepresentation.Instruction;

namespace Ryujinx.Graphics.Shader.Translation
{
    public static class AmpereGlobal
    {
        [ThreadStatic] static int[] _candidates;
        [ThreadStatic] static bool _used, _usedDeviceAddress;
        [ThreadStatic] static HashSet<Operation> _deviceOps;

        public const int PoolTableC0 = 0xff8;   // u64 device address of the pool address table; the u32 entry count at PoolTableC0 - 8
        const int SearchSteps = 12;             // 4096 entries at most

        /// c[0] word offsets of the storage descriptors to check (null or empty: the access is dropped as before). Resets the flags.
        public static void Begin(int[] candidateWords)
        {
            _candidates = candidateWords;
            _used = false;
            _usedDeviceAddress = false;
            _deviceOps = null;
        }

        public static int[] Candidates => _candidates;

        /// Whether the block being translated on this thread used the fallback.
        public static bool Used { get => _used; internal set => _used = value; }
        public static bool UsedDeviceAddress => _usedDeviceAddress;

        internal static bool IsDeviceAddressOp(Operation op) => _deviceOps != null && _deviceOps.Contains(op);

        // ---- IR: the pool table lookup and the access (inside GlobalToStorage's generated function; arguments 0/1 = address low/high) ----

        static Operand Add64Lo(EmitterContext c, Operand aLo, Operand bLo) => c.IAdd(aLo, bLo);
        static Operand Carry(EmitterContext c, Operand sumLo, Operand aLo) => c.ConditionalSelect(c.ICompareLessUnsigned(sumLo, aLo), Const(1), Const(0));

        static Operand LoadDevice(EmitterContext c, Operand lo, Operand hi)
        {
            Operand dest = Local();
            Operation op = new(IrInstruction.Load, StorageKind.GlobalMemory, dest, lo, hi);
            (_deviceOps ??= new HashSet<Operation>()).Add(op);
            c.Add(op);
            return dest;
        }

        /// The candidate storage descriptors (Candidates) as a loop: the index of the first {address, size} in c[0] that holds the address
        /// (arguments 0/1), or -1. Null when the candidates are not one run of 16-byte slots (then the caller checks each one inline).
        internal static Operand EmitCandidateSearch(EmitterContext c, int cbBinding)
        {
            int[] w = _candidates;
            if (w == null || w.Length < 2) return null;
            for (int i = 1; i < w.Length; i++) if (w[i] != w[0] + 4 * i) return null;
            Operand aLo = Argument(0), aHi = Argument(1);
            Operand k = Local(), hit = Local();
            c.Copy(k, Const(0));
            c.Copy(hit, Const(-1));
            Operand loop = Label();
            c.MarkLabel(loop);
            {
                Operand vec = c.IAdd(Const(w[0] / 4), k);   // 16-byte vectors of c[0]; every candidate starts one (w[0] % 4 == 0)
                Operand bLo = c.Load(StorageKind.ConstantBuffer, cbBinding, Const(0), vec, Const(w[0] & 3));
                Operand bHi = c.Load(StorageKind.ConstantBuffer, cbBinding, Const(0), vec, Const((w[0] & 3) + 1));
                Operand size = c.Load(StorageKind.ConstantBuffer, cbBinding, Const(0), vec, Const((w[0] & 3) + 2));
                // as GlobalToStorage's inline check: low word in [base, base + size), high word equal after the borrow
                Operand offset = c.ISubtract(aLo, bLo);
                Operand borrow = c.ICompareLessUnsigned(aLo, bLo);
                Operand inRange = c.BitwiseAnd(c.ICompareLessUnsigned(offset, size), c.ICompareEqual(c.IAdd(aHi, borrow), bHi));
                Operand first = c.BitwiseAnd(inRange, c.ICompareEqual(hit, Const(-1)));
                c.Copy(hit, c.ConditionalSelect(first, k, hit));
                c.Copy(k, c.IAdd(k, Const(1)));
                c.BranchIfTrue(loop, c.ICompareLessUnsigned(k, Const(w.Length)));
            }
            return hit;
        }

        internal static void EmitDeviceAddressAccess(EmitterContext c, IrInstruction inst, StorageKind kind, int inArguments, bool returnsValue)
        {
            if (kind != StorageKind.GlobalMemory) return;   // 8/16-bit accesses: not handled (read 0)
            IrInstruction baseInst = inst & IrInstruction.Mask;
            if (baseInst is not (IrInstruction.Load or IrInstruction.Store) && !inst.IsAtomic()) return;
            _usedDeviceAddress = true;
            Operand aLo = Argument(0), aHi = Argument(1);
            Operand tLo = Cbuf(0, PoolTableC0 / 4), tHi = Cbuf(0, PoolTableC0 / 4 + 1), count = Cbuf(0, (PoolTableC0 - 8) / 4);

            // Entry e's word w: table + e * 32 + w * 4.
            (Operand lo, Operand hi) Entry(Operand e, int word)
            {
                Operand off = c.IAdd(c.ShiftLeft(e, Const(5)), Const(word * 4));
                Operand lo = c.IAdd(tLo, off);
                return (lo, c.IAdd(tHi, Carry(c, lo, tLo)));
            }
            // a <= b for 64-bit (lo, hi) pairs
            Operand Le64(Operand xLo, Operand xHi, Operand yLo, Operand yHi) =>
                c.BitwiseOr(c.ICompareLessUnsigned(xHi, yHi), c.BitwiseAnd(c.ICompareEqual(xHi, yHi), c.ICompareLessOrEqualUnsigned(xLo, yLo)));

            // No table (count 0, address 0: the runtime gave none): nothing to look up, no load from a null pointer.
            Operand skip = Label();
            c.BranchIfFalse(skip, c.ICompareNotEqual(count, Const(0)));
            Operand last = c.ISubtract(count, Const(1));
            // the last entry whose guest address is <= the address (entry 0 when none is); every load stays inside the table. A loop, not
            // unrolled: the drivers inline this function at every access (253 calls in one compute shader), and the unrolled search made
            // pipelines of 20-90 s in the NVIDIA driver (run da_7).
            Operand idx = Local(), step = Local();
            c.Copy(idx, Const(0));
            c.Copy(step, Const(1 << (SearchSteps - 1)));
            Operand loop = Label();
            c.MarkLabel(loop);
            {
                Operand probe = c.IAdd(idx, step);
                Operand safe = c.IMinimumU32(probe, last);
                (Operand sLoA, Operand sHiA) = Entry(safe, 0);
                Operand sLo = LoadDevice(c, sLoA, sHiA);
                (Operand hLoA, Operand hHiA) = Entry(safe, 1);
                Operand sHi = LoadDevice(c, hLoA, hHiA);
                Operand ok = c.BitwiseAnd(c.ICompareLessUnsigned(probe, count), Le64(sLo, sHi, aLo, aHi));
                c.Copy(idx, c.ConditionalSelect(ok, probe, idx));
                c.Copy(step, c.ShiftRightU32(step, Const(1)));
                c.BranchIfTrue(loop, c.ICompareNotEqual(step, Const(0)));
            }
            (Operand a0, Operand a1) = Entry(idx, 0);
            Operand gLo = LoadDevice(c, a0, a1);
            (a0, a1) = Entry(idx, 1);
            Operand gHi = LoadDevice(c, a0, a1);
            (a0, a1) = Entry(idx, 2);
            Operand sizeLo = LoadDevice(c, a0, a1);
            (a0, a1) = Entry(idx, 4);
            Operand dLo = LoadDevice(c, a0, a1);
            (a0, a1) = Entry(idx, 5);
            Operand dHi = LoadDevice(c, a0, a1);
            // offset = address - guest base (64-bit); in range when the high word is 0 and the low word < size (ranges are < 4 GiB)
            Operand offLo = c.ISubtract(aLo, gLo);
            Operand borrow = c.ConditionalSelect(c.ICompareLessUnsigned(aLo, gLo), Const(1), Const(0));
            Operand offHi = c.ISubtract(c.ISubtract(aHi, gHi), borrow);
            Operand inRange = c.BitwiseAnd(c.BitwiseAnd(c.ICompareLessUnsigned(idx, count), Le64(gLo, gHi, aLo, aHi)),
                                           c.BitwiseAnd(c.ICompareEqual(offHi, Const(0)), c.ICompareLessUnsigned(offLo, sizeLo)));
            c.BranchIfFalse(skip, inRange);
            Operand pLo = c.IAdd(dLo, offLo);
            Operand pHi = c.IAdd(dHi, Carry(c, pLo, dLo));

            // Atomics other than add, exchange and compare-and-swap as a compare-and-swap loop. The device address points into a game memory
            // pool, which is host memory imported into Vulkan, and a GPU atomic on system memory is a PCIe atomic operation: PCIe has only
            // fetch-add, swap and compare-and-swap, so min/max/and/or/xor there did nothing. Seen in OoT's culling 4c1e8564: its RED.E.MAX
            // of each indirect dispatch record's work-group count left x at 0 (every GPU-culled dispatch and mesh draw empty) while the
            // counts' atomic adds next to it landed; as a CAS loop the records get 65, 66, 1, 20 (capture deku150).
            Func<Operand, Operand, Operand> combine = baseInst switch
            {
                IrInstruction.AtomicMaxU32 => c.IMaximumU32,
                IrInstruction.AtomicMaxS32 => c.IMaximumS32,
                IrInstruction.AtomicMinU32 => c.IMinimumU32,
                IrInstruction.AtomicMinS32 => c.IMinimumS32,
                IrInstruction.AtomicAnd => c.BitwiseAnd,
                IrInstruction.AtomicOr => c.BitwiseOr,
                IrInstruction.AtomicXor => c.BitwiseExclusiveOr,
                _ => null,
            };
            if (combine != null && inArguments == 3)
            {
                Operand casLoop = Label();
                c.MarkLabel(casLoop);
                Operand before = LoadDevice(c, pLo, pHi);   // loaded again in every round: IR locals are single-assignment
                Operand want = combine(before, Argument(2));
                Operand seen = Local();
                Operation cas = new(IrInstruction.AtomicCompareAndSwap, StorageKind.GlobalMemory, seen, pLo, pHi, before, want);
                (_deviceOps ??= new HashSet<Operation>()).Add(cas);
                c.Add(cas);
                c.BranchIfTrue(casLoop, c.ICompareNotEqual(seen, before));
                if (returnsValue) c.Return(before); else c.Return();
                c.MarkLabel(skip);
                return;
            }

            Operand dest = returnsValue ? Local() : null;
            Operand[] sources = inArguments switch
            {
                4 => new[] { pLo, pHi, Argument(2), Argument(3) },
                3 => new[] { pLo, pHi, Argument(2) },
                _ => new[] { pLo, pHi },
            };
            Operation access = new(inst, StorageKind.GlobalMemory, dest, sources);
            (_deviceOps ??= new HashSet<Operation>()).Add(access);
            c.Add(access);
            if (returnsValue) c.Return(dest); else c.Return();
            c.MarkLabel(skip);
        }

        // ---- SPIR-V back end -------------------------------------------------------------------------------------------------------------

        /// After DeclareAll: the capabilities and the addressing model device address pointers need.
        internal static void Declare(CodeGenContext ctx)
        {
            if (!_usedDeviceAddress) return;
            ctx.AddCapability(Capability.Int64);
            ctx.AddCapability((Capability)5347);   // PhysicalStorageBufferAddresses
            ctx.AddExtension("SPV_KHR_physical_storage_buffer");
            ctx.SetMemoryModel((AddressingModel)5348, MemoryModel.GLSL450);   // PhysicalStorageBuffer64
        }

        /// GetStoragePointer for StorageKind.GlobalMemory: sources 0/1 are the device address (low, high); a pointer to a u32.
        internal static SpvInstruction DevicePointer(CodeGenContext ctx, AstOperation op, out AggregateType varType)
        {
            varType = AggregateType.U32;
            SpvInstruction u32 = ctx.TypeU32();
            SpvInstruction u64 = ctx.TypeInt(64, 0);
            SpvInstruction lo = ctx.UConvert(u64, ctx.Get(AggregateType.U32, op.GetSource(0)));
            SpvInstruction hi = ctx.UConvert(u64, ctx.Get(AggregateType.U32, op.GetSource(1)));
            SpvInstruction addr = ctx.BitwiseOr(u64, lo, ctx.ShiftLeftLogical(u64, hi, ctx.Constant(u32, 32)));
            return ctx.ConvertUToPtr(ctx.TypePointer((StorageClass)5349, u32), addr);   // PhysicalStorageBuffer
        }
    }
}
