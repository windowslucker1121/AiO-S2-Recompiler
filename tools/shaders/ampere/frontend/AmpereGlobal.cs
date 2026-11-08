// SPDX-License-Identifier: MIT
// Global memory accesses whose address the translator cannot trace to one storage descriptor (A17, AGENTS.md). Ryujinx's GlobalToStorage
// dropped them (loads read 0, stores and atomics vanished); 3,485 of OoT's blocks had such accesses, among them the counters of its GPU
// culling, so every GPU-culled draw got zero work. Two steps now resolve them at run time:
//  1. Like the Lichii emulator (zortos293/Lichii, global_memory_to_storage_buffer_pass.cpp and DefineGlobalMemoryFunctions): the address is
//     checked against every storage descriptor slot of the stage's c[0] ({address, size} words); the first range that holds it is used.
//     The harness sets the candidates per block (StaticGpuAccessor.GlobalFallbackWords) and adds the resource token "gmem" when a block
//     uses them: the runtime then fills that stage's c[0] descriptors per draw (graphics_c0) or per dispatch (fill_compute_c0). Since
//     2026-11-08 only the slots the runtime marks are searched (SlotMaskC0: bindings that may differ from the pool); the others skip the
//     search, which cost more than the walk.
//  2. Our own (Lichii has nothing for it): when no bound buffer holds the address (pointers read from memory or uniform tables, e.g. the
//     culling root 4c1e8564's counters), it is translated through the runtime's guest page table (EmitResolve: a radix walk whose levels,
//     shifts and table address are data in c[0] 0xfe0..0xfef, so the table layout can change without a re-translation; the runtime keeps
//     one device address per 4 KiB guest page, or per 2 MiB where one import holds it all: nvn_gpu_pool.cpp page table), and the access
//     goes through a PhysicalStorageBuffer pointer. An unmapped address still reads 0. Blocks that use it carry the token "gpt". (Until
//     2026-11-07 a 16-step binary search over a sorted table of imports, c[0] 0xff0/0xff8; the runtime still fills it for older
//     translations without "gpt".)
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
        [ThreadStatic] static bool _used, _usedDeviceAddress, _usedWrite;
        [ThreadStatic] static HashSet<Operation> _deviceOps;
        [ThreadStatic] static BasicBlock _mainEntry;
        [ThreadStatic] static int _cacheId;
        [ThreadStatic] static bool _compute;

        // Which bound storage descriptors the access checks before the page table (c[0] SlotMaskC0 + 4 * index, written by the runtime per
        // draw or dispatch: nvn_replay.cpp gmem_slot_masks): bit k = candidate k. The runtime sets the slots whose binding is not the pool
        // itself where it matters: a result held only in the device-local copy, a compute shadow, a buffer in a virtual pool (which the page
        // table cannot map). With the mask 0 (most draws) the access goes straight to the page table and skips the 16-slot search, which
        // cost more than the walk (OoT's fern mesh draw: 2.4 ms with the search for every word). Index: compute 0, a graphics stage its
        // descriptors' c[0] base >> 8 (vertex 1 ... fragment 5, task 6, mesh 7).
        public const int SlotMaskC0 = 0xfc0;
        static int SlotMaskWord(int[] w) => (SlotMaskC0 + 4 * (_compute ? 0 : ((w[0] * 4) >> 8) & 7)) / 4;

        /// c[0] word offsets of the storage descriptors to check (null or empty: the access is dropped as before). Resets the flags.
        public static void Begin(int[] candidateWords) => Begin(candidateWords, false);

        public static void Begin(int[] candidateWords, bool compute)
        {
            _candidates = candidateWords;
            _compute = compute;
            _used = false;
            _usedDeviceAddress = false;
            _usedWrite = false;
            _deviceOps = null;
            _mainEntry = null;
            _cacheId = -1;
        }

        /// GlobalToStorage.RunPass of each function (main first): remembers main's entry block, where the page cache is cleared.
        internal static void NoteFunction(BasicBlock[] blocks)
        {
            if (_mainEntry == null && blocks.Length > 0) _mainEntry = blocks[0];
        }

        // The last page walk's result, per invocation (a Private array, CacheWords words): {tag low, tag high, offset mask, device address
        // low, high} of the span the walk ended in. Consecutive accesses into the same 4 KiB page or 2 MiB span (the four words of an
        // LDG.128, the fields of one record) then skip the walk. Cleared at main's entry; only made for shaders that walk the table.
        const int CacheWords = 5;
        static int CacheMemory(ResourceManager rm)
        {
            if (_cacheId >= 0 || rm == null || _mainEntry == null || NoCache) return _cacheId;
            _cacheId = rm.Properties.AddLocalMemory(new MemoryDefinition("gmem_page_cache", AggregateType.Array | AggregateType.U32, CacheWords));
            int[] init = { 1, -1, 0, 0, 0 };   // tag 0xffffffff_00000001 with mask 0 matches no canonical address
            for (int i = CacheWords - 1; i >= 0; i--)
                _mainEntry.Operations.AddFirst(new Operation(IrInstruction.Store, StorageKind.LocalMemory, null, Const(_cacheId), Const(i), Const(init[i])));
            return _cacheId;
        }

        /// NXRT_HARNESS_GMEM_NO_CACHE=1: no page cache (for measuring).
        static readonly bool NoCache = Environment.GetEnvironmentVariable("NXRT_HARNESS_GMEM_NO_CACHE") == "1";

        public static int[] Candidates => _candidates;

        /// Whether the block being translated on this thread used the fallback.
        public static bool Used { get => _used; internal set => _used = value; }
        public static bool UsedDeviceAddress => _usedDeviceAddress;
        /// A store or atomic went through the page table (the runtime then treats the ranges its uniform pointers name as written: token "gmemw").
        public static bool UsedWrite => _usedWrite;

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
            Operand slots = Cbuf(0, SlotMaskWord(w));
            c.Copy(k, Const(0));
            c.Copy(hit, Const(-1));
            Operand loop = Label(), none = Label();
            c.BranchIfFalse(none, c.ICompareNotEqual(slots, Const(0)));
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
                Operand wanted = c.ICompareNotEqual(c.BitwiseAnd(c.ShiftRightU32(slots, k), Const(1)), Const(0));
                Operand first = c.BitwiseAnd(c.BitwiseAnd(inRange, wanted), c.ICompareEqual(hit, Const(-1)));
                c.Copy(hit, c.ConditionalSelect(first, k, hit));
                c.Copy(k, c.IAdd(k, Const(1)));
                c.BranchIfTrue(loop, c.ICompareLessUnsigned(k, Const(w.Length)));
            }
            c.MarkLabel(none);
            return hit;
        }

        // The guest page table walk (c[0] PageTableC0..+0xf, written by the runtime: nvn_replay.cpp page_table_words). The shader knows only
        // the generic radix walk; how many levels there are, their shifts and where the table lives are data in c[0], so the runtime can
        // change the table's layout without a re-translation:
        //   +0x0 u32: the shift of each level, one byte per level from the root (0 ends the list; at most MaxLevels)
        //   +0x4 u32: number of root entries (an address whose root index is not below it is not mapped)
        //   +0x8 u64: device address of the table (the root node at byte 0)
        // An entry is a u64: 0 = not mapped; bit 0 set = leaf: (entry & ~1) is the device address of the first byte of the span that
        // entry covers (2^shift bytes); otherwise the byte offset of the next level's node from the table's start (2^(shift above - shift)
        // entries; offsets below 4 GiB). Not mapped jumps to `miss`. Returns the device address of (aLo, aHi).
        public const int PageTableC0 = 0xfe0;
        public const int MaxLevels = 4;
        /// NXRT_HARNESS_GMEM_WINDOW=1 (experiment E3): the flat window check before the walk; its words are c[0] WindowC0..+0xf (the runtime's
        /// [nvn] exp_gmem_window). Without the runtime key the words are 0 and every access walks as before.
        public const int WindowC0 = 0xff0;
        static readonly bool Window = Environment.GetEnvironmentVariable("NXRT_HARNESS_GMEM_WINDOW") == "1";

        // The device address of (aLo, aHi): the page cache (CacheMemory) when it holds the address's span, else a walk of the page table
        // (layout above). One loop without early exits and a single branch to `miss` at its end: NVIDIA's compiler (driver 610.88) crashed
        // on the earlier form, a cache test jumping over a walk loop whose exits branch to the miss label.
        static (Operand, Operand) EmitResolve(EmitterContext c, ResourceManager rm, Operand aLo, Operand aHi, Operand miss)
        {
            Operand shifts = Cbuf(0, PageTableC0 / 4), rootCount = Cbuf(0, PageTableC0 / 4 + 1);
            Operand tLo = Cbuf(0, PageTableC0 / 4 + 2), tHi = Cbuf(0, PageTableC0 / 4 + 3);
            Operand pLo = Local(), pHi = Local(), found = Local();
            int cache = CacheMemory(rm);
            Operand hit = c.ICompareNotEqual(tLo, tLo);   // false, as a bool (no cache)
            if (cache >= 0)
            {
                Operand tagLo = c.Load(StorageKind.LocalMemory, cache, Const(0));
                Operand tagHi = c.Load(StorageKind.LocalMemory, cache, Const(1));
                Operand cm = c.Load(StorageKind.LocalMemory, cache, Const(2));
                Operand dLo = c.Load(StorageKind.LocalMemory, cache, Const(3));
                Operand dHi = c.Load(StorageKind.LocalMemory, cache, Const(4));
                hit = c.BitwiseAnd(c.ICompareEqual(c.BitwiseAnd(aLo, c.BitwiseNot(cm)), tagLo), c.ICompareEqual(aHi, tagHi));
                Operand lo = c.IAdd(dLo, c.BitwiseAnd(aLo, cm));
                c.Copy(pLo, lo);
                c.Copy(pHi, c.IAdd(dHi, Carry(c, lo, dLo)));
            }
            else
            {
                c.Copy(pLo, Const(0));
                c.Copy(pHi, Const(0));
            }
            if (Window)
            {
                // E3 / bet 2 experiment (NXRT_HARNESS_GMEM_WINDOW=1): a flat window (c[0] WindowC0: guest base >> 16, size >> 16, device
                // address) where device address = window + (guest - base), one add and no walk; addresses outside it walk as before.
                Operand wBase = Cbuf(0, WindowC0 / 4), wSize = Cbuf(0, WindowC0 / 4 + 1);
                Operand wLo = Cbuf(0, WindowC0 / 4 + 2), wHi = Cbuf(0, WindowC0 / 4 + 3);
                // offset = address - (base << 16), as 64 bits: in the window when its high part is 0 and the low part below size << 16
                Operand bLo = c.ShiftLeft(wBase, Const(16)), bHi = c.ShiftRightU32(wBase, Const(16));
                Operand oLo = c.ISubtract(aLo, bLo);
                Operand oHi = c.ISubtract(c.ISubtract(aHi, bHi), c.ConditionalSelect(c.ICompareLessUnsigned(aLo, bLo), Const(1), Const(0)));
                Operand inWin = c.BitwiseAnd(c.BitwiseAnd(c.ICompareEqual(oHi, Const(0)), c.ICompareLessUnsigned(c.ShiftRightU32(oLo, Const(16)), wSize)),
                                             c.BitwiseNot(hit));
                Operand rLo = c.IAdd(wLo, oLo);
                c.Copy(pLo, c.ConditionalSelect(inWin, rLo, pLo));
                c.Copy(pHi, c.ConditionalSelect(inWin, c.IAdd(wHi, Carry(c, rLo, wLo)), pHi));
                hit = c.BitwiseOr(hit, inWin);
            }
            c.Copy(found, hit);
            Operand tableOk = c.ICompareNotEqual(c.BitwiseOr(tLo, tHi), Const(0));
            Operand level = Local(), nodeOff = Local(), prev = Local(), go = Local();
            c.Copy(level, Const(0));
            c.Copy(nodeOff, Const(0));
            c.Copy(prev, Const(64));
            c.Copy(go, c.BitwiseAnd(c.BitwiseNot(hit), tableOk));
            Operand loop = Label(), after = Label();
            c.BranchIfFalse(after, go);
            c.MarkLabel(loop);
            {
                Operand s = c.BitwiseAnd(c.ShiftRightU32(shifts, c.ShiftLeft(level, Const(3))), Const(0xff));
                Operand s5 = c.BitwiseAnd(s, Const(31));
                Operand big = c.ICompareGreaterOrEqualUnsigned(s, Const(32));
                // index = address >> s (low 32 bits) and whether bits above them are set
                Operand lowSmall = c.BitwiseOr(c.ShiftRightU32(aLo, s5), c.ShiftLeft(aHi, c.BitwiseAnd(c.ISubtract(Const(32), s5), Const(31))));
                Operand idxFull = c.ConditionalSelect(big, c.ShiftRightU32(aHi, s5), lowSmall);
                Operand above = c.ConditionalSelect(big, Const(0), c.ShiftRightU32(aHi, s5));
                Operand isRoot = c.ICompareEqual(level, Const(0));
                Operand width = c.ISubtract(prev, s);   // bits of a node below the root (< 32)
                Operand wmask = c.ISubtract(c.ShiftLeft(Const(1), c.BitwiseAnd(width, Const(31))), Const(1));
                Operand idx = c.ConditionalSelect(isRoot, idxFull, c.BitwiseAnd(idxFull, wmask));
                Operand rootOk = c.BitwiseAnd(c.ICompareEqual(above, Const(0)), c.ICompareLessUnsigned(idxFull, rootCount));
                Operand valid = c.BitwiseAnd(c.ICompareNotEqual(s, Const(0)), c.BitwiseOr(c.BitwiseNot(isRoot), rootOk));
                // entry address = table + node offset + idx * 8; an invalid step reads the root's first entry (inside the table) and ends the walk
                Operand off = c.ConditionalSelect(valid, c.IAdd(nodeOff, c.ShiftLeft(idx, Const(3))), Const(0));
                Operand lo = c.IAdd(tLo, off);
                Operand hi = c.IAdd(tHi, Carry(c, lo, tLo));
                Operand eLo = LoadDevice(c, lo, hi);
                Operand lo4 = c.IAdd(lo, Const(4));
                Operand eHi = LoadDevice(c, lo4, c.IAdd(hi, Carry(c, lo4, lo)));
                Operand used = c.BitwiseAnd(valid, c.ICompareNotEqual(c.BitwiseOr(eLo, eHi), Const(0)));
                Operand isLeaf = c.BitwiseAnd(used, c.ICompareNotEqual(c.BitwiseAnd(eLo, Const(1)), Const(0)));
                // a node: its offset must fit in 32 bits (a torn or bad entry ends the walk, never a wild load)
                Operand isNode = c.BitwiseAnd(c.BitwiseAnd(used, c.BitwiseNot(isLeaf)), c.ICompareEqual(eHi, Const(0)));
                // a leaf: device address = (entry & ~1) + (address & (2^s - 1))
                Operand m = c.ISubtract(c.ShiftLeft(Const(1), s5), Const(1));
                Operand oLo = c.ConditionalSelect(big, aLo, c.BitwiseAnd(aLo, m));
                Operand oHi = c.ConditionalSelect(big, c.BitwiseAnd(aHi, m), Const(0));
                Operand bLo = c.BitwiseAnd(eLo, Const(~1));
                Operand rLo = c.IAdd(bLo, oLo);
                Operand rHi = c.IAdd(c.IAdd(eHi, oHi), Carry(c, rLo, bLo));
                c.Copy(pLo, c.ConditionalSelect(isLeaf, rLo, pLo));
                c.Copy(pHi, c.ConditionalSelect(isLeaf, rHi, pHi));
                c.Copy(found, c.BitwiseOr(found, isLeaf));
                if (cache >= 0)
                {
                    // spans of 2^32 bytes or more are not cached (the tag compares the high word whole)
                    Operand keep = c.BitwiseOr(c.BitwiseNot(isLeaf), big);
                    Operand[] now = { c.BitwiseAnd(aLo, c.BitwiseNot(m)), aHi, m, bLo, eHi };
                    for (int i = 0; i < CacheWords; i++)
                        c.Store(StorageKind.LocalMemory, cache, Const(i), c.ConditionalSelect(keep, c.Load(StorageKind.LocalMemory, cache, Const(i)), now[i]));
                }
                c.Copy(nodeOff, c.ConditionalSelect(isNode, eLo, nodeOff));
                c.Copy(prev, c.ConditionalSelect(isNode, s, prev));
                c.Copy(level, c.IAdd(level, Const(1)));
                c.Copy(go, c.BitwiseAnd(isNode, c.ICompareLessUnsigned(level, Const(MaxLevels))));
                c.BranchIfTrue(loop, go);
            }
            c.MarkLabel(after);
            c.BranchIfFalse(miss, found);
            return (pLo, pHi);
        }

        internal static void EmitDeviceAddressAccess(EmitterContext c, ResourceManager rm, IrInstruction inst, StorageKind kind, int inArguments, bool returnsValue)
        {
            if (kind != StorageKind.GlobalMemory) return;   // 8/16-bit accesses: not handled (read 0)
            IrInstruction baseInst = inst & IrInstruction.Mask;
            if (baseInst is not (IrInstruction.Load or IrInstruction.Store) && !inst.IsAtomic()) return;
            _usedDeviceAddress = true;
            if (baseInst == IrInstruction.Store || inst.IsAtomic()) _usedWrite = true;
            Operand aLo = Argument(0), aHi = Argument(1);
            Operand skip = Label();
            (Operand pLo, Operand pHi) = EmitResolve(c, rm, aLo, aHi, skip);

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
