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
            _vectorTags = null;
            _vectorGroups = 0;
            _vectorDone = null;
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
        /// Measurement only (wrong results): NXRT_HARNESS_GMEM_IDENTITY=1 uses the guest address as the device address (no walk);
        /// NXRT_HARNESS_GMEM_NO_SLOTS=1 leaves out the bound-slot candidates. Together: the shader's own cost without the lookup.
        internal static readonly bool Identity = Environment.GetEnvironmentVariable("NXRT_HARNESS_GMEM_IDENTITY") == "1";
        /// NXRT_HARNESS_GMEM_FIRSTSLOT=1 (measurement only): every access goes to the first candidate storage buffer at its low address word.
        internal static readonly bool FirstSlot = Environment.GetEnvironmentVariable("NXRT_HARNESS_GMEM_FIRSTSLOT") == "1";
        /// NXRT_HARNESS_GMEM_SCAN_ALL=1: the candidate search visits all 16 slots per access (the form before 2026-11-09).
        static readonly bool ScanAllSlots = Environment.GetEnvironmentVariable("NXRT_HARNESS_GMEM_SCAN_ALL") == "1";
        internal static readonly bool NoSlots = Environment.GetEnvironmentVariable("NXRT_HARNESS_GMEM_NO_SLOTS") == "1";

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
            _cb0Binding = cbBinding;
            int[] w = _candidates;
            if (w == null || w.Length < 2) return null;
            for (int i = 1; i < w.Length; i++) if (w[i] != w[0] + 4 * i) return null;
            Operand aLo = Argument(0), aHi = Argument(1);
            Operand hit = Local();
            Operand slots = Cbuf(0, SlotMaskWord(w));
            c.Copy(hit, Const(-1));
            Operand loop = Label(), none = Label();
            if (!ScanAllSlots)
            {
                // only the marked slots, lowest first, until one holds the address (most masks have one or two bits; the 16-step loop per
                // 32-bit word was ~20% of a pointer draw, E3)
                Operand m = Local();
                c.Copy(m, c.BitwiseAnd(slots, Const(w.Length >= 32 ? -1 : (1 << w.Length) - 1)));
                c.BranchIfFalse(none, c.ICompareNotEqual(m, Const(0)));
                c.MarkLabel(loop);
                Operand kk = c.FindLSB(m);
                Operand v = c.IAdd(Const(w[0] / 4), kk);
                Operand lo0 = c.Load(StorageKind.ConstantBuffer, cbBinding, Const(0), v, Const(w[0] & 3));
                Operand hi0 = c.Load(StorageKind.ConstantBuffer, cbBinding, Const(0), v, Const((w[0] & 3) + 1));
                Operand sz0 = c.Load(StorageKind.ConstantBuffer, cbBinding, Const(0), v, Const((w[0] & 3) + 2));
                Operand in0 = c.BitwiseAnd(c.ICompareLessUnsigned(c.ISubtract(aLo, lo0), sz0), c.ICompareEqual(c.IAdd(aHi, c.ICompareLessUnsigned(aLo, lo0)), hi0));
                c.Copy(hit, c.ConditionalSelect(in0, kk, Const(-1)));
                c.Copy(m, c.BitwiseAnd(m, c.ISubtract(m, Const(1))));
                c.BranchIfTrue(loop, c.BitwiseAnd(c.ICompareNotEqual(m, Const(0)), c.ICompareEqual(hit, Const(-1))));
                c.MarkLabel(none);
                return hit;
            }
            Operand k = Local();
            c.Copy(k, Const(0));
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
        static (Operand, Operand) EmitResolve(EmitterContext c, ResourceManager rm, Operand aLo, Operand aHi, Operand miss, Operand[] leaf = null)
        {
            Operand shifts = Cbuf(0, PageTableC0 / 4), rootCount = Cbuf(0, PageTableC0 / 4 + 1);
            Operand tLo = Cbuf(0, PageTableC0 / 4 + 2), tHi = Cbuf(0, PageTableC0 / 4 + 3);
            Operand pLo = Local(), pHi = Local(), found = Local();
            // with fast-path helpers the page cache belongs to main alone: helpers neither read nor write it (leaf: the span for the caller)
            int cache = Fast ? -1 : CacheMemory(rm);
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
                if (cache >= 0 || leaf != null)
                {
                    // spans of 2^32 bytes or more are not cached (the tag compares the high word whole)
                    Operand keep = c.BitwiseOr(c.BitwiseNot(isLeaf), big);
                    Operand[] now = { c.BitwiseAnd(aLo, c.BitwiseNot(m)), aHi, m, bLo, eHi };
                    for (int i = 0; i < CacheWords; i++)
                    {
                        if (leaf != null) c.Copy(leaf[i], c.ConditionalSelect(keep, leaf[i], now[i]));
                        else c.Store(StorageKind.LocalMemory, cache, Const(i), c.ConditionalSelect(keep, c.Load(StorageKind.LocalMemory, cache, Const(i)), now[i]));
                    }
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

        // ---- Compute: the page cache with marked slots (2026-11-10) ----------------------------------------------------------------------
        // Compute bindings are the pool copy or a shadow, so the runtime marked every bound slot (SlotMaskC0 word 0 never 0) and no compute
        // access ever took the inline fast path: every 32-bit word of OoT's HiZ / culling pointer shaders (submission #3, the one the game's
        // main thread waits on) called its helper, searched the slots and walked the table. Now the helper hands main a walk's span only when
        // no marked slot overlaps it, so a cached span can never hide a marked binding, and compute call sites test the cache alone (no
        // "mask is 0" condition). The runtime marks only the slots whose binding may differ from the pool (nvn_replay.cpp
        // gmem_compute_slots). NXRT_HARNESS_GMEM_CS_SPAN=0: the form before (compute fast path only with mask 0).
        static readonly bool ComputeSpanCheck = Environment.GetEnvironmentVariable("NXRT_HARNESS_GMEM_CS_SPAN") != "0";
        [ThreadStatic] static int _cb0Binding;

        // The span in `leaf` (CacheWords: tag low, tag high, offset mask, ...) becomes "no span" when a marked candidate slot overlaps it.
        static void EmitSpanClip(EmitterContext c, Operand[] leaf)
        {
            int[] w = _candidates;
            if (w == null || w.Length < 1 || NoSlots) return;
            for (int i = 1; i < w.Length; i++) if (w[i] != w[0] + 4 * i) return;   // EmitCandidateSearch's layout only (then no span: below)
            Operand m = Local(), clash = Local();
            c.Copy(m, c.BitwiseAnd(Cbuf(0, SlotMaskWord(w)), Const(w.Length >= 32 ? -1 : (1 << w.Length) - 1)));
            c.Copy(clash, c.ICompareNotEqual(Const(0), Const(0)));
            Operand loop = Label(), done = Label();
            c.BranchIfFalse(done, c.ICompareNotEqual(m, Const(0)));
            c.MarkLabel(loop);
            {
                Operand k = c.FindLSB(m);
                Operand v = c.IAdd(Const(w[0] / 4), k);
                Operand sLo = c.Load(StorageKind.ConstantBuffer, _cb0Binding, Const(0), v, Const(w[0] & 3));
                Operand sHi = c.Load(StorageKind.ConstantBuffer, _cb0Binding, Const(0), v, Const((w[0] & 3) + 1));
                Operand sSz = c.Load(StorageKind.ConstantBuffer, _cb0Binding, Const(0), v, Const((w[0] & 3) + 2));
                // span [L, L + mask], L = (tag high, tag low); slot [S, S + size): overlap when S - L <= mask or L - S < size (64-bit, unsigned)
                Operand d1Lo = c.ISubtract(sLo, leaf[0]);
                Operand d1Hi = c.ISubtract(c.ISubtract(sHi, leaf[1]), c.ConditionalSelect(c.ICompareLessUnsigned(sLo, leaf[0]), Const(1), Const(0)));
                Operand slotInSpan = c.BitwiseAnd(c.ICompareEqual(d1Hi, Const(0)), c.BitwiseNot(c.ICompareLessUnsigned(leaf[2], d1Lo)));
                Operand d2Lo = c.ISubtract(leaf[0], sLo);
                Operand d2Hi = c.ISubtract(c.ISubtract(leaf[1], sHi), c.ConditionalSelect(c.ICompareLessUnsigned(leaf[0], sLo), Const(1), Const(0)));
                Operand spanInSlot = c.BitwiseAnd(c.ICompareEqual(d2Hi, Const(0)), c.ICompareLessUnsigned(d2Lo, sSz));
                c.Copy(clash, c.BitwiseOr(clash, c.BitwiseOr(slotInSpan, spanInSlot)));
                c.Copy(m, c.BitwiseAnd(m, c.ISubtract(m, Const(1))));
                c.BranchIfTrue(loop, c.ICompareNotEqual(m, Const(0)));
            }
            c.MarkLabel(done);
            int[] none = { 1, -1, 0, 0, 0 };
            for (int i = 0; i < CacheWords; i++) c.Copy(leaf[i], c.ConditionalSelect(clash, Const(none[i]), leaf[i]));
        }

        /// Compute call sites test the page cache alone (the helper never caches a span a marked slot overlaps: EmitSpanClip).
        static bool CacheAloneDecides(CodeGenContext ctx) =>
            ComputeSpanCheck && !NoSlots && ctx.Definitions.Stage == ShaderStage.Compute && SpanClipLayout();
        static bool SpanClipLayout()
        {
            int[] w = _candidates;
            if (w == null || w.Length < 1) return false;
            for (int i = 1; i < w.Length; i++) if (w[i] != w[0] + 4 * i) return false;
            return true;
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
            Operand[] leaf = null;
            if (_helperFast && !Identity)
            {
                int[] none = { 1, -1, 0, 0, 0 };
                leaf = new Operand[CacheWords];
                for (int i = 0; i < CacheWords; i++) { leaf[i] = Local(); c.Copy(leaf[i], Const(none[i])); }
            }
            (Operand pLo, Operand pHi) = Identity ? (aLo, aHi) : EmitResolve(c, rm, aLo, aHi, skip, leaf);
            if (leaf != null && _compute && ComputeSpanCheck) EmitSpanClip(c, leaf);
            if (leaf != null)
                for (int i = 0; i < FastOuts; i++) c.Copy(Argument(_helperInArgs + i), leaf[i]);   // the span for main's page cache

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

        // ---- Out-of-line helpers and the inline fast path ---------------------------------------------------------------------------------
        // Ryujinx makes one helper function per access kind and calls it for every 32-bit word; the driver inlined each call, so a pointer
        // shader carried the whole candidate search and page walk once per word (OoT's fern mesh fbb731e2: 253 calls, 1.26 MB of code,
        // 255 registers, 20 s to compile). Now (each can be switched off for measuring):
        //  - the helpers are marked DontInline (NVIDIA's compiler keeps one copy) and take their in arguments by value;
        //  - a plain 32-bit load calls its helper only on the slow path: at the call site the page cache (CacheMemory, main's own) is
        //    checked inline, and when no bound slot is marked (SlotMaskC0 word 0) and the cached span holds the address, the word is loaded
        //    through the cached device address right there (TryGenerateHelperCall); a miss calls the helper, which returns the value and the
        //    span its walk ended in. fbb731e2: 1.26 MB -> 175 KB of code, 20 s -> 1.1 s to build; fps unchanged (the draws' own work and
        //    register use dominate: with the lookup removed altogether, measurement mode IDENTITY, they still take 255 registers).
        /// NXRT_HARNESS_GMEM_NOINLINE=0: the helpers may be inlined again (as before 2026-11-09).
        public static readonly bool NoInline = Environment.GetEnvironmentVariable("NXRT_HARNESS_GMEM_NOINLINE") != "0";
        /// NXRT_HARNESS_GMEM_FAST=0: no inline fast path (every word calls its helper).
        public static readonly bool Fast = Environment.GetEnvironmentVariable("NXRT_HARNESS_GMEM_FAST") != "0";
        public const string HelperSuffix = "_gmem", FastSuffix = "_gmemfast";
        // Fast-path helpers: one extra in argument (the stage's slot mask word, c[0] SlotMaskC0), out arguments {the five cache words of
        // the span the walk ended in}. The page cache is read and written only by main's inline code (so the driver keeps it in registers:
        // a cache the out-of-line helper touched lived in local memory); in SPIR-V the helper returns {value, span} as a struct.
        public const int FastArguments = 2, FastOuts = CacheWords;
        [ThreadStatic] static bool _lastFallback;
        [ThreadStatic] static int _helperInArgs;
        [ThreadStatic] static bool _helperFast;
        /// GlobalToStorage: whether the multi-target function just made or found for an access is a fallback one.
        internal static bool LastFallback { get => _lastFallback; set => _lastFallback = value; }

        /// Whether the access gets the inline fast path (a plain 32-bit load through the fallback).
        internal static bool WantsFast(Operation op, ResourceManager rm) =>
            Fast && _candidates is { Length: > 0 } && op.Inst == IrInstruction.Load && op.StorageKind == StorageKind.GlobalMemory &&
            op.Dest != null && CacheMemory(rm) >= 0;

        /// GlobalToStorage, at the start of a fallback helper's body: a fast helper sets its out arguments to "no span" first (every return
        /// path then leaves them defined). Other helpers do not touch the page cache when fast helpers exist (it stays main's own).
        internal static void BeginHelper(EmitterContext c, bool fast, int inArgs)
        {
            _helperFast = fast;
            _helperInArgs = inArgs;
            if (!fast) return;
            int[] none = { 1, -1, 0, 0, 0 };
            for (int i = 0; i < FastOuts; i++) c.Copy(Argument(inArgs + i), Const(none[i]));
        }

        /// The fast path's extra call arguments: the slot mask word (the cache test itself is SPIR-V at the call, TryGenerateHelperCall) and
        /// the load's vector tag (VectorTag; the helper does not read it).
        internal static Operand[] EmitFastOperands(Operation op) =>
            new[] { Cbuf(0, SlotMaskWord(_candidates)), Const(_vectorTags != null && _vectorTags.TryGetValue(op, out int t) ? t : 0) };

        // ---- Vector loads (LDG.64 / LDG.128) --------------------------------------------------------------------------------------------
        // The front end splits a vector load into 32-bit loads (the IR is scalar). Through the fast path each word then made its own
        // cache test and its own 32-bit load: four load instructions where the SASS has one (OoT's gpt mesh blocks: 66 LDG.128 of light
        // probe records per lane, scattered over memory). The words of one load carry a tag {group, word count, word}; at word 0 the back
        // end loads the whole vector through the cached span (an aligned LDG.128 / .64 never crosses a 4 KiB page) and the other words take
        // their lane from it when word 0 hit; after a miss they go their own way as before.
        [ThreadStatic] static Dictionary<Operation, int> _vectorTags;
        [ThreadStatic] static int _vectorGroups;
        /// NXRT_HARNESS_GMEM_VECTOR=0: every word loads on its own (as before 2026-11-10).
        static readonly bool VectorLoads = Environment.GetEnvironmentVariable("NXRT_HARNESS_GMEM_VECTOR") != "0";

        /// Front end: the n-th 32-bit load of a vector load of `count` words (2 or 4).
        internal static void NoteVectorWord(Operation op, int group, int count, int n)
        {
            if (!VectorLoads || op == null || count is not (2 or 4)) return;
            (_vectorTags ??= new Dictionary<Operation, int>())[op] = group * 64 + count * 8 + n;
        }
        internal static int NewVectorGroup() => ++_vectorGroups;

        // back end: word 0's results per group (hit flag and every word), for the later words of the same load
        [ThreadStatic] static Dictionary<int, (SpvInstruction Hit, SpvInstruction[] Words)> _vectorDone;
        // The helpers' calling convention in SPIR-V. Ryujinx passes every argument as a pointer to a Function variable of the caller; with
        // the helper out of line (DontInline) each call site's argument temporaries are then address-taken and live in local memory (fbb731e2:
        // 13 KB of local memory). The global-memory helpers take their in arguments by value instead, and a fast-path helper returns a
        // struct {value, the five cache words} in place of out arguments: nothing of the caller is address-taken.
        static bool IsHelper(StructuredFunction f) => f.Name != null &&
            (f.Name.EndsWith(HelperSuffix, StringComparison.Ordinal) || f.Name.EndsWith(FastSuffix, StringComparison.Ordinal));
        static bool IsFastHelper(StructuredFunction f) => f.Name != null && f.Name.EndsWith(FastSuffix, StringComparison.Ordinal) &&
            f.OutArguments.Length == FastOuts && f.ReturnType is AggregateType.U32 or AggregateType.S32 or AggregateType.FP32;

        static SpvInstruction HelperReturnType(CodeGenContext ctx, StructuredFunction f)
        {
            if (!IsFastHelper(f)) return ctx.GetType(f.ReturnType);
            SpvInstruction[] members = new SpvInstruction[1 + FastOuts];
            members[0] = ctx.GetType(f.ReturnType);
            for (int i = 0; i < FastOuts; i++) members[1 + i] = ctx.GetType(f.OutArguments[i]);
            return ctx.TypeStruct(false, members);
        }

        /// SpirvGenerator's function declaration: a helper's type with by-value in arguments (null: not a helper, Ryujinx's form).
        internal static SpvInstruction DeclareHelperFunction(CodeGenContext ctx, StructuredFunction f)
        {
            if (!IsHelper(f) || (f.OutArguments.Length != 0 && !IsFastHelper(f))) return null;
            SpvInstruction[] args = new SpvInstruction[f.InArguments.Length];
            for (int i = 0; i < args.Length; i++) args[i] = ctx.GetType(f.InArguments[i]);
            SpvInstruction type = ctx.TypeFunction(HelperReturnType(ctx, f), false, args);
            return ctx.Function(HelperReturnType(ctx, f), FunctionControl(f), type);
        }

        [ThreadStatic] static List<(SpvInstruction Param, SpvInstruction Var)> _params;

        /// In place of Declarations.DeclareParameters for a helper: value parameters; the Function variables the body reads them from are made
        /// in StoreHelperParameters (after the first label). False: not a helper.
        internal static bool DeclareHelperParameters(CodeGenContext ctx, StructuredFunction f)
        {
            _params = null;
            if (!IsHelper(f) || (f.OutArguments.Length != 0 && !IsFastHelper(f))) return false;
            _params = new();
            for (int i = 0; i < f.InArguments.Length; i++) _params.Add((ctx.FunctionParameter(ctx.GetType(f.InArguments[i])), null));
            return true;
        }

        /// After the first label and DeclareLocals: a Function variable per argument (outs too), the in arguments stored into theirs.
        internal static void StoreHelperParameters(CodeGenContext ctx, StructuredFunction f)
        {
            if (_params == null) return;
            int n = f.InArguments.Length + f.OutArguments.Length;
            for (int i = 0; i < n; i++)
            {
                SpvInstruction v = ctx.Variable(ctx.TypePointer(StorageClass.Function, ctx.GetType(f.GetArgumentType(i))), StorageClass.Function);
                ctx.AddLocalVariable(v);
                ctx.DeclareArgument(i, v);
                if (i < _params.Count) _params[i] = (_params[i].Param, v);
            }
            foreach (var (p, v) in _params) ctx.Store(v, p);
            _params = null;
        }

        /// GenerateReturn in a fast-path helper: the struct {value, out arguments}. False: not one.
        internal static bool TryGenerateHelperReturn(CodeGenContext ctx, AstOperation operation)
        {
            StructuredFunction f = ctx.CurrentFunction;
            if (ctx.IsMainFunction || f == null || !IsFastHelper(f) || operation.SourcesCount == 0) return false;
            ctx.MayHaveReturned = true;
            SpvInstruction[] parts = new SpvInstruction[1 + FastOuts];
            parts[0] = ctx.Get(f.ReturnType, operation.GetSource(0));
            int nIn = f.InArguments.Length;
            for (int i = 0; i < FastOuts; i++)
                parts[1 + i] = ctx.Load(ctx.GetType(f.OutArguments[i]), ctx.GetArgumentPointer(new AstOperand(Ryujinx.Graphics.Shader.IntermediateRepresentation.OperandType.Argument, nIn + i)));
            ctx.ReturnValue(ctx.CompositeConstruct(HelperReturnType(ctx, f), parts));
            return true;
        }

        /// SPIR-V GenerateCall of a helper: in arguments by value; a fast-path helper gets the inline fast path (if hit, load through the
        /// cached device address and keep the cache words, else call and take value and cache words from the returned struct).
        internal static bool TryGenerateHelperCall(CodeGenContext ctx, AstOperation operation, StructuredFunction function, SpvInstruction spvFunc,
                                                   out SpvInstruction result)
        {
            result = null;
            if (!IsHelper(function) || (function.OutArguments.Length != 0 && !IsFastHelper(function))) return false;
            int nIn = function.InArguments.Length, nOut = function.OutArguments.Length;
            SpvInstruction[] vals = new SpvInstruction[nIn];
            for (int i = 0; i < nIn; i++) vals[i] = ctx.Get(function.InArguments[i], operation.GetSource(1 + i));
            SpvInstruction retType = ctx.GetType(function.ReturnType);
            if (!IsFastHelper(function))
            {
                result = ctx.FunctionCall(retType, spvFunc, vals);
                return true;
            }
            if (_cacheId < 0 || !ctx.LocalMemories.TryGetValue(_cacheId, out SpvInstruction cacheVar))
            {
                result = ctx.CompositeExtract(retType, ctx.FunctionCall(HelperReturnType(ctx, function), spvFunc, vals), (Spv.Generator.LiteralInteger)0);
                return true;
            }
            SpvInstruction u32 = ctx.TypeU32();
            SpvInstruction u64 = ctx.TypeInt(64, 0);
            SpvInstruction bt = ctx.TypeBool();
            SpvInstruction pw = ctx.TypePointer(StorageClass.Private, u32);
            SpvInstruction C(uint v) => ctx.Constant(u32, v);
            SpvInstruction aLo = ctx.Get(AggregateType.U32, operation.GetSource(1)), aHi = ctx.Get(AggregateType.U32, operation.GetSource(2));
            SpvInstruction slots = ctx.Get(AggregateType.U32, operation.GetSource(3));
            int tag = operation.SourcesCount > 4 ? ConstantOf(operation.GetSource(4)) : 0;
            int vGroup = tag / 64, vCount = tag / 8 % 8, vWord = tag % 8;
            // the scalar fast path: the page cache test, the load through it, else the helper (vector: word 0 loads them all)
            (SpvInstruction, SpvInstruction) Regular(bool vectorHead)
            {
                SpvInstruction[] w = new SpvInstruction[CacheWords];
                for (int i = 0; i < CacheWords; i++) w[i] = ctx.Load(u32, ctx.AccessChain(pw, cacheVar, C((uint)i)));
                // hit: no marked slot, and the address in the cached span: (a & ~mask) == tag low, a.hi == tag high
                SpvInstruction hit = ctx.LogicalAnd(bt, ctx.LogicalAnd(bt, ctx.IEqual(bt, ctx.BitwiseAnd(u32, aLo, ctx.Not(u32, w[2])), w[0]), ctx.IEqual(bt, aHi, w[1])),
                                                    CacheAloneDecides(ctx) ? ctx.ConstantTrue(bt) : ctx.IEqual(bt, slots, C(0)));
                SpvInstruction lFast = ctx.Label(), lSlow = ctx.Label(), lMerge = ctx.Label();
                ctx.SelectionMerge(lMerge, SelectionControlMask.MaskNone);   // right before the branch
                ctx.BranchConditional(hit, lFast, lSlow);
                ctx.AddLabel(lFast);
                SpvInstruction Address(SpvInstruction[] s)
                {
                    SpvInstruction lo = ctx.IAdd(u32, s[3], ctx.BitwiseAnd(u32, aLo, s[2]));
                    SpvInstruction hi = ctx.IAdd(u32, s[4], ctx.Select(u32, ctx.ULessThan(bt, lo, s[3]), C(1), C(0)));
                    return ctx.BitwiseOr(u64, ctx.UConvert(u64, lo), ctx.ShiftLeftLogical(u64, ctx.UConvert(u64, hi), C(32)));
                }
                SpvInstruction[] LoadVector(SpvInstruction[] s)
                {
                    SpvInstruction vt = ctx.TypeVector(u32, vCount);
                    SpvInstruction vec = ctx.Load(vt, ctx.ConvertUToPtr(ctx.TypePointer((StorageClass)5349, vt), Address(s)), MemoryAccessMask.Aligned);
                    vec.AddOperand((Spv.Generator.LiteralInteger)(4 * vCount));
                    SpvInstruction[] ws = new SpvInstruction[vCount];
                    for (int i = 0; i < vCount; i++) ws[i] = ctx.CompositeExtract(u32, vec, (Spv.Generator.LiteralInteger)i);
                    return ws;
                }
                SpvInstruction[] span = new SpvInstruction[CacheWords];
                SpvInstruction addr = Address(w);
                SpvInstruction v;
                SpvInstruction[] fastWords = null;
                if (vectorHead)
                {
                    fastWords = LoadVector(w);
                    v = fastWords[0];
                }
                else
                {
                    v = ctx.Load(u32, ctx.ConvertUToPtr(ctx.TypePointer((StorageClass)5349, u32), addr), MemoryAccessMask.Aligned);
                    v.AddOperand((Spv.Generator.LiteralInteger)4);
                }
                SpvInstruction fastValue = function.ReturnType == AggregateType.U32 ? v : ctx.Bitcast(retType, v);
                ctx.Branch(lMerge);
                ctx.AddLabel(lSlow);
                // the helper: value and the span its walk ended in, which becomes the page cache
                SpvInstruction r = ctx.FunctionCall(HelperReturnType(ctx, function), spvFunc, vals);
                SpvInstruction slowValue = ctx.CompositeExtract(retType, r, (Spv.Generator.LiteralInteger)0);
                for (int i = 0; i < CacheWords; i++)
                {
                    SpvInstruction word = ctx.CompositeExtract(ctx.GetType(function.OutArguments[i]), r, (Spv.Generator.LiteralInteger)(1 + i));
                    ctx.Store(ctx.AccessChain(pw, cacheVar, C((uint)i)), ctx.BitcastIfNeeded(AggregateType.U32, function.OutArguments[i], word));
                    span[i] = ctx.BitcastIfNeeded(AggregateType.U32, function.OutArguments[i], word);
                }
                SpvInstruction slowEnd = lSlow, slowFlag = null;
                SpvInstruction[] slowWords = null;
                if (vectorHead)
                {
                    // the span the walk ended in holds the whole vector unless a bound slot answered (no span): load the other words
                    // through it as on the fast path
                    SpvInstruction covers = ctx.LogicalAnd(bt, ctx.IEqual(bt, ctx.BitwiseAnd(u32, aLo, ctx.Not(u32, span[2])), span[0]), ctx.IEqual(bt, aHi, span[1]));
                    SpvInstruction lVec = ctx.Label(), lNoVec = ctx.Label(), lVecEnd = ctx.Label();
                    ctx.SelectionMerge(lVecEnd, SelectionControlMask.MaskNone);
                    ctx.BranchConditional(covers, lVec, lNoVec);
                    ctx.AddLabel(lVec);
                    SpvInstruction[] got = LoadVector(span);
                    ctx.Branch(lVecEnd);
                    ctx.AddLabel(lNoVec);
                    ctx.Branch(lVecEnd);
                    ctx.AddLabel(lVecEnd);
                    slowWords = new SpvInstruction[vCount];
                    for (int i = 1; i < vCount; i++) slowWords[i] = ctx.Phi(u32, got[i], lVec, C(0), lNoVec);
                    slowFlag = ctx.Phi(bt, ctx.ConstantTrue(bt), lVec, ctx.ConstantFalse(bt), lNoVec);
                    slowEnd = lVecEnd;
                }
                ctx.Branch(lMerge);
                ctx.AddLabel(lMerge);
                SpvInstruction res = ctx.Phi(retType, fastValue, lFast, slowValue, slowEnd);
                if (vectorHead)
                {
                    SpvInstruction[] words = new SpvInstruction[vCount];
                    for (int i = 1; i < vCount; i++) words[i] = ctx.Phi(u32, fastWords[i], lFast, slowWords[i], slowEnd);
                    SpvInstruction hitFlag = ctx.Phi(bt, ctx.ConstantTrue(bt), lFast, slowFlag, slowEnd);
                    (_vectorDone ??= new())[vGroup] = (hitFlag, words);
                }
                return (res, lMerge);
            }

            if (vGroup != 0 && vWord != 0 && _vectorDone != null && _vectorDone.TryGetValue(vGroup, out var done))
            {
                // a later word of a vector load: word 0 loaded the whole vector (through the page cache or the span its walk ended in);
                // only when a bound slot answered word 0 does this word call the helper itself
                SpvInstruction lHave = ctx.Label(), lReg = ctx.Label(), lEnd = ctx.Label();
                ctx.SelectionMerge(lEnd, SelectionControlMask.MaskNone);
                ctx.BranchConditional(done.Hit, lHave, lReg);
                ctx.AddLabel(lHave);
                SpvInstruction have = function.ReturnType == AggregateType.U32 ? done.Words[vWord] : ctx.Bitcast(retType, done.Words[vWord]);
                ctx.Branch(lEnd);
                ctx.AddLabel(lReg);
                SpvInstruction rc = ctx.FunctionCall(HelperReturnType(ctx, function), spvFunc, vals);
                SpvInstruction regValue = ctx.CompositeExtract(retType, rc, (Spv.Generator.LiteralInteger)0), regEnd = lReg;
                ctx.Branch(lEnd);
                ctx.AddLabel(lEnd);
                result = ctx.Phi(retType, have, lHave, regValue, regEnd);
                return true;
            }
            (result, _) = Regular(vGroup != 0 && vWord == 0 && vCount is 2 or 4);
            return true;
        }
        /// The constant an AST source holds: a constant, or a local whose single assignment is one (call arguments stay locals,
        /// spirv-call-args.patch); 0 otherwise.
        static int ConstantOf(IAstNode node)
        {
            for (int depth = 0; depth < 8; depth++)
            {
                if (node is not AstOperand o) return 0;
                if (o.Type == Ryujinx.Graphics.Shader.IntermediateRepresentation.OperandType.Constant) return o.Value;
                if (o.Type != Ryujinx.Graphics.Shader.IntermediateRepresentation.OperandType.LocalVariable || o.Defs.Count != 1) return 0;
                IAstNode def = null;
                foreach (IAstNode d in o.Defs) def = d;
                if (def is not AstAssignment a) return 0;
                node = a.Source is AstOperation { Inst: Ryujinx.Graphics.Shader.IntermediateRepresentation.Instruction.Copy } cp ? cp.GetSource(0) : a.Source;
            }
            return 0;
        }
        /// The function control of a helper: DontInline for the fallback helpers (NoInline).
        internal static FunctionControlMask FunctionControl(StructuredFunction function) =>
            Environment.GetEnvironmentVariable("NXRT_HARNESS_GMEM_FORCEINLINE") == "1" ? FunctionControlMask.Inline :
            NoInline && function.Name != null && (function.Name.EndsWith(HelperSuffix, StringComparison.Ordinal) || function.Name.EndsWith(FastSuffix, StringComparison.Ordinal))
                ? FunctionControlMask.DontInline : FunctionControlMask.MaskNone;

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
