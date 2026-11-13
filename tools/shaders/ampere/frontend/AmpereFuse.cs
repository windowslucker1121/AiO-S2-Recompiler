// SPDX-License-Identifier: GPL-3.0-or-later
// Peephole analyses over the decoded SASS that let the emitter produce what the host compiler turns into the original
// instruction sequence, instead of an exact emulation of each instruction on its own.
//
// Sine and cosine (NXRT_HARNESS_SIN_FUSE=1, opt-in: measured no GPU-time gain, G4): Volta+ compilers emit "FMUL.FTZ.RZ t, x, 1/(2*pi)" and then
// "MUFU.SIN/COS r, t" (MUFU takes turns). Translated one by one that is the exact round-toward-zero product (a fused multiply-add,
// compares and selects: ~15 host instructions) and MUFU as sin(2*pi * (t - round(t))), which the host compiler then compiles
// back into its own FMUL.RZ by 1/(2*pi) and MUFU.SIN. When the MUFU reads the product of such an FMUL in the same straight-line
// run (no label, no control flow, neither the product nor the guard predicate rewritten in between, the same guard), the
// MUFU is emitted as sin(x) / cos(x) of the FMUL's own source (copied at the FMUL: the compiler often reuses x's register): the host compiler makes exactly the console's pair of it. The FMUL
// itself is still emitted exactly; when nothing else reads its product, dead code elimination drops it.
using Nx.Shaders.Ampere;
using System;
using System.Collections.Generic;
using System.Linq;

namespace Ryujinx.Graphics.Shader.Translation
{
    internal static class AmpereFuse
    {
        internal static readonly bool SinFuse = Environment.GetEnvironmentVariable("NXRT_HARNESS_SIN_FUSE") == "1";
        const ulong InvTwoPiBits = 0x3e22f983;   // 0.15915494f
        const int Window = 96;
        static readonly bool Debug = Environment.GetEnvironmentVariable("NXRT_HARNESS_FUSE_DEBUG") == "1";

        internal static void Analyse(AmpereProgram p)
        {
            if (!SinFuse) return;
            HashSet<int> targets = new();
            foreach (SassInst i in p.Insts)
                foreach (Operand o in i.Operands)
                    if (o.Kind == OperandKind.Label) targets.Add((int)o.Offset);
            foreach (int[] t in p.IndirectTargets.Values) foreach (int a in t) targets.Add(a);
            foreach ((int start, _) in p.Subroutines.Values) targets.Add(start);

            for (int k = 0; k < p.Insts.Count; k++)
            {
                SassInst f = p.Insts[k];
                if (Debug && f.Opcode == "FMUL" && f.Mods.Contains("RZ"))
                    Console.Error.WriteLine($"fuse: {f.Address:x} {string.Join(",", f.Mods)} {string.Join(" | ", f.Operands.Select(o => $"{o.Kind} r{o.Reg} {o.ImmBits:x} {o.Text}"))}");
                if (!IsTurnsProduct(f, out Operand x)) continue;
                int d = f.Operands[0].Reg;
                for (int j = k + 1; j < p.Insts.Count && j <= k + Window; j++)
                {
                    SassInst m = p.Insts[j];
                    if (targets.Contains(m.Address)) break;
                    if (m.Opcode is "BRA" or "CALL" or "BRX" or "BRXU" or "EXIT" or "RET" or "KILL" or "JMP" or "JMX") break;
                    if (m.Opcode == "MUFU" && (m.Mods.Contains("SIN") || m.Mods.Contains("COS")) && m.Mods.Count == 1 && m.Operands.Count >= 2
                        && m.Operands[1] is { Kind: OperandKind.Reg, Neg: false, Abs: false } src && src.Reg == d && (src.Suffixes == null || src.Suffixes.Count == 0 || OnlyReuse(src))
                        && m.Pred == f.Pred && m.PredNeg == f.PredNeg && m.PredIsUniform == f.PredIsUniform)
                    {
                        p.SinFrom[m.Address] = f;
                        p.SinSources.Add(f.Address);
                    }
                    else if (Debug && m.Operands.Skip(m.DstCount).Any(o => o.Kind == OperandKind.Reg && o.Reg == d))
                        Console.Error.WriteLine($"fuse: {f.Address:x} R{d} also read by {m.Address:x} {m.ToSassText()}");
                    if (Writes(m, OperandKind.Reg, d)) break;   // x may change: the FMUL keeps a copy of it (SinArg)
                    if (f.Pred != 7 && Writes(m, f.PredIsUniform ? OperandKind.UPred : OperandKind.Pred, f.Pred)) break;
                }
            }
            if (Debug) Console.Error.WriteLine($"fuse: {p.SinFrom.Count} sin/cos fused");
        }

        static bool OnlyReuse(Operand o)
        {
            foreach (string s in o.Suffixes) if (s != ".reuse") return false;
            return true;
        }

        /// "FMUL[.FTZ].RZ Rd, x, 1/(2*pi)" (either source order); x a plain or negated/absolute register.
        static bool IsTurnsProduct(SassInst f, out Operand x)
        {
            x = null;
            if (f.Opcode != "FMUL" || !f.Mods.Contains("RZ")) return false;
            foreach (string m in f.Mods) if (m is not ("RZ" or "FTZ")) return false;
            if (f.Operands.Count < 3 || f.Operands[0].Kind != OperandKind.Reg || f.Operands[0].Reg >= 255) return false;
            Operand a = f.Operands[1], b = f.Operands[2];
            if (IsInvTwoPi(b) && a.Kind is OperandKind.Reg or OperandKind.UReg) x = a;
            else if (IsInvTwoPi(a) && b.Kind is OperandKind.Reg or OperandKind.UReg) x = b;
            return x != null && x.Reg < (x.Kind == OperandKind.Reg ? 255 : 63);
        }

        static bool IsInvTwoPi(Operand o) => o.Kind == OperandKind.FImm && !o.Neg && !o.Abs && (o.ImmBits & 0xffffffffUL) == InvTwoPiBits;

        /// Registers a destination operand covers: four for 128-bit, texture, tensor and unknown-width forms, two for 64-bit and
        /// .WIDE, else one.
        static int Width(SassInst m)
        {
            if (m.Opcode is "TEX" or "TLD" or "TLD4" or "TXD" or "TXQ" or "TMML" or "SULD" or "HMMA" or "IMMA" or "LDSM" or "MOVM" or "LD" or "LDGSTS") return 4;
            foreach (string x in m.Mods)
            {
                if (x.Contains("128")) return 4;
                if (x.Contains("64") || x == "WIDE") return 2;
            }
            return 1;
        }

        // ---- predicated instructions run unconditionally where the old value is dead ----------------------------------------------------
        // NXRT_HARNESS_SPECULATE=1 (opt-in, G4: proves few instructions in the hot meshes, no measured gain). A predicated SASS instruction ("@P0 FFMA R3, ...") becomes "if (P0) R3 = ..." in the
        // IR, and the host compiler keeps the old R3 alive and merges it with a select: one more instruction and one more live value per
        // predicated instruction (f3054ac2: ~620 of its ~3,400 host instructions). When the instruction is plain arithmetic (no memory,
        // nothing that can fault or has a side effect) and on every path on which the guard is false each register it writes is
        // overwritten before anything may read it, running it unconditionally changes no value that is ever read: the guard only kept
        // the old value for readers that do not exist. The forward search knows that the guard predicate keeps its value until something
        // writes it, so "@P0 X" readers are not reachable readers on the guard-false paths, "@!P0 Y" writers are definite writes there,
        // and "@P0 BRA" is not taken there.

        internal static readonly bool Speculate = Environment.GetEnvironmentVariable("NXRT_HARNESS_SPECULATE") == "1";
        const int SpeculateBudget = 4096;   // instructions visited per candidate before giving up (then: keep the guard)

        static readonly HashSet<string> PureAlu = new()
        {
            "FADD", "FMUL", "FFMA", "FSEL", "SEL", "MOV", "IADD3", "IMAD", "LOP3", "SHF", "LEA", "FMNMX", "IMNMX", "MUFU", "FRND",
            "F2I", "I2F", "I2FP", "PRMT", "FSETP", "ISETP", "PLOP3", "IABS", "POPC", "FLO", "BREV", "SGXT", "BMSK", "F2FP", "HADD2", "HMUL2",
            "HFMA2", "HMNMX2", "HSETP2", "FSET", "CS2R",
        };

        static bool Is32BitAlu(SassInst m) => PureAlu.Contains(m.Opcode) && !m.Mods.Any(x => x.Contains("64") || x.Contains("128") || x == "WIDE");

        /// Registers a source operand may read (over-approximated widths).
        static void SourceRegs(SassInst m, List<int> gpr, List<int> pred, out bool allGpr, out bool allPred)
        {
            allGpr = allPred = false;
            if (m.Pred != 7 && !m.PredIsUniform) pred.Add(m.Pred);
            int w = Is32BitAlu(m) || m.Opcode is "ISBEWR" ? 1 : 4;
            for (int k = Math.Min(m.DstCount, m.Operands.Count); k < m.Operands.Count; k++)
            {
                Operand o = m.Operands[k];
                switch (o.Kind)
                {
                    case OperandKind.Reg when o.Reg < 255: for (int q = 0; q < w; q++) gpr.Add(o.Reg + q); break;
                    case OperandKind.Pred when o.Reg != 7: pred.Add(o.Reg); break;
                    case OperandKind.Mem or OperandKind.Attr or OperandKind.CBuf:
                        if (o.Kind == OperandKind.Mem && o.Reg < 255) { gpr.Add(o.Reg); gpr.Add(o.Reg + 1); }
                        if (o.IndexReg >= 0 && !o.IndexIsUReg && o.IndexReg < 255) gpr.Add(o.IndexReg);
                        foreach (System.Text.RegularExpressions.Match r in System.Text.RegularExpressions.Regex.Matches(o.Text ?? "", @"(?<![U\w])R(\d+)"))
                        { int n = int.Parse(r.Groups[1].Value); if (n < 255) { gpr.Add(n); gpr.Add(n + 1); } }
                        break;
                    case OperandKind.Special or OperandKind.Other:
                        if (o.Text == "PR") allPred = true;
                        break;
                }
            }
            // destinations of these are also read (accumulators, read-modify-write)
            if (m.Opcode is "HMMA" or "IMMA" or "R2P") allGpr = true;
        }

        /// Registers m certainly writes when it executes (exact first register only).
        // ---- batch loops with a clamped count (G1) -------------------------------------------------------------------------------------
        // The game's GPU culling tasks (fd8debac, c6f3f11e, ... : one work group per cell) process a cell's items in batches of 32:
        //     IMNMX.U32 Rn, Rcount, K, PT        n = min(count, K)          (K = 256)
        //     IADD3 Rt, Rn, -0x1, RZ
        //     SHF.R.U32.HI Rb, RZ, s, Rt         last = (n - 1) >> s        (s = 5)
        //     loop: ... IADD3 Rc, Rc, 0x1 ... ISETP.GT.U32.AND Pd, PT, Rc, Rb, PT ... (exit when Pd)
        // For count >= 1, last <= M = (K - 1) >> s, so "Rc > M" implies "Rc > last" and adding it to Pd changes nothing. For count 0
        // last wraps to 2^(32-s) - 1: the console would run ~2^27 empty batches (it never meets such a cell), our translation ran into the
        // loop cap (4096 back edges per invocation) - 1.15 ms of GPU per grass draw on the balcony. With the extra test such a loop ends
        // after M + 1 batches, which do nothing when every item index is past the count.
        // NXRT_HARNESS_BATCH_BOUND=0 turns it off.
        internal static readonly bool BatchBound = Environment.GetEnvironmentVariable("NXRT_HARNESS_BATCH_BOUND") != "0";

        static bool IsGpr(Operand o, out int r) { r = o.Reg; return o.Kind == OperandKind.Reg && o.Reg < 255 && !o.Neg && !o.Not && !o.Abs; }
        static bool IsImm(Operand o, out long v) { v = o.Kind == OperandKind.Imm ? (long)(uint)o.ImmBits : 0; return o.Kind == OperandKind.Imm; }
        static bool IsRz(Operand o) => o.Kind == OperandKind.Reg && o.Reg == 255;
        static bool IsPt(Operand o) => o.Kind == OperandKind.Pred && o.Reg == 7 && !o.Not;
        static bool Writes(SassInst m, int r)
        {
            for (int k = 0; k < Math.Min(m.DstCount, m.Operands.Count); k++)
            {
                Operand o = m.Operands[k];
                if (o.Kind != OperandKind.Reg || o.Reg >= 255) continue;
                // registers written from this operand: vector and texture forms up to four, 64-bit and wide forms two
                int w = m.Opcode is "TEX" or "TLD" or "TLD4" or "TXD" or "TXQ" or "SULD" or "LDSM" or "HMMA" or "IMMA" or "SUATOM" ? 4
                      : m.Mods.Any(x => x.Contains("128")) ? 4
                      : m.Mods.Any(x => x.Contains("64") || x == "WIDE") || m.Opcode is "DADD" or "DMUL" or "DFMA" or "DMNMX" or "DSETP" ||
                        (m.Opcode == "CS2R" && !m.Mods.Contains("32")) ? 2 : 1;
                if (r >= o.Reg && r < o.Reg + w) return true;
            }
            return false;
        }
        static bool IsBranch(SassInst m) => m.Opcode is "BRA" or "BRX" or "BRXU" or "JMP" or "JMX" or "CALL" or "RET" or "BREAK" or "BSYNC" or "WARPSYNC";

        /// Fills p.BatchBounds: ISETP address -> (counter register, M).
        internal static void AnalyseBatchLoops(AmpereProgram p)
        {
            if (!BatchBound) return;
            List<SassInst> s = p.Insts;
            HashSet<int> targets = new();
            foreach (SassInst i in s)
                foreach (Operand o in i.Operands)
                    if (o.Kind == OperandKind.Label) targets.Add((int)o.Offset);
            foreach (int[] t in p.IndirectTargets.Values) foreach (int a in t) targets.Add(a);
            foreach ((int start, _) in p.Subroutines.Values) targets.Add(start);
            // every control transfer: (source address, target address)
            List<(int From, int To)> edges = new();
            foreach (SassInst i in s)
            {
                foreach (Operand o in i.Operands)
                    if (o.Kind == OperandKind.Label) edges.Add((i.Address, (int)o.Offset));
                if (p.IndirectTargets.TryGetValue(i.Address, out int[] it)) foreach (int a in it) edges.Add((i.Address, a));
            }
            for (int i2 = 0; i2 < s.Count; i2++)
            {
                SassInst shf = s[i2];
                // SHF.R.U32.HI Rb, RZ, s, Rt
                if (shf.Opcode != "SHF" || shf.Pred != 7 || shf.Operands.Count < 4 || !shf.Mods.SequenceEqual(new[] { "R", "U32", "HI" })) continue;
                if (!IsGpr(shf.Operands[0], out int rb) || !IsRz(shf.Operands[1]) || !IsImm(shf.Operands[2], out long sh) || sh < 1 || sh > 31 ||
                    !IsGpr(shf.Operands[3], out int rt)) continue;
                // IADD3 Rt, Rn, -0x1, RZ: the latest write of Rt before i2
                int i1 = i2 - 1;
                while (i1 >= 0 && !Writes(s[i1], rt)) i1--;
                if (i1 < 0) continue;
                SassInst add = s[i1];
                if (add.Opcode != "IADD3" || add.Pred != 7 || add.Mods.Count != 0 || add.Operands.Count != 4 || !IsGpr(add.Operands[0], out int rt2) || rt2 != rt ||
                    !IsGpr(add.Operands[1], out int rn) || !IsImm(add.Operands[2], out long m1) || m1 != 0xffffffff || !IsRz(add.Operands[3])) continue;
                // IMNMX.U32 Rn, Ra, K, PT: the latest write of Rn before i1
                int i0 = i1 - 1;
                while (i0 >= 0 && !Writes(s[i0], rn)) i0--;
                if (i0 < 0) continue;
                SassInst mn = s[i0];
                if (mn.Opcode != "IMNMX" || mn.Pred != 7 || !mn.Mods.SequenceEqual(new[] { "U32" }) || mn.Operands.Count != 4 || !IsGpr(mn.Operands[0], out int rn2) || rn2 != rn ||
                    !IsImm(mn.Operands[2], out long kk) || kk < 1 || !IsPt(mn.Operands[3])) continue;
                // Rb never written again after i2
                bool rewritten = false;
                for (int k = i2 + 1; k < s.Count && !rewritten; k++) rewritten = Writes(s[k], rb);
                if (rewritten) continue;
                int a0 = s[i0].Address, a2 = shf.Address;
                // the chain is straight-line code: no transfer into (a0, a2], none out of [a0, a2]
                if (edges.Any(e => (e.To > a0 && e.To <= a2) || (e.From >= a0 && e.From <= a2)) || s.Skip(i0).Take(i2 - i0 + 1).Any(IsBranch)) continue;
                // no loop around it: no transfer from after the chain back to it or before it
                if (edges.Any(e => e.From > a2 && e.To <= a2)) continue;
                uint m = (uint)((kk - 1) >> (int)sh);
                List<SassInst> found = new();
                for (int k = i2 + 1; k < s.Count; k++)
                {
                    SassInst c = s[k];
                    // ISETP.GT.U32.AND Pd, PT, Rc, Rb, PT (unguarded)
                    if (c.Opcode != "ISETP" || c.Pred != 7 || !c.Mods.SequenceEqual(new[] { "GT", "U32", "AND" }) || c.Operands.Count != 5) continue;
                    if (c.Operands[0].Kind != OperandKind.Pred || c.Operands[0].Reg == 7 || !IsPt(c.Operands[1]) || !IsGpr(c.Operands[2], out int rc) ||
                        !IsGpr(c.Operands[3], out int rb2) || rb2 != rb || !IsPt(c.Operands[4])) continue;
                    found.Add(c);
                }
                if (found.Count == 0) continue;
                // every path to a compare runs the chain: transfers from before the chain go to before it or past the last compare
                int last = found.Max(c => c.Address);
                if (edges.Any(e => e.From < a0 && e.To > a0 && e.To <= last)) continue;
                foreach (SassInst c in found) p.BatchBounds[c.Address] = (c.Operands[2].Reg, m);
            }
            if (Debug && p.BatchBounds.Count > 0)
                Console.Error.WriteLine($"batch bound: {string.Join(", ", p.BatchBounds.Select(kv => $"{kv.Key:x}: R{kv.Value.Counter} > {kv.Value.Max}"))}");
        }

        static void DefiniteDests(SassInst m, List<(OperandKind, int)> d)
        {
            for (int k = 0; k < Math.Min(m.DstCount, m.Operands.Count); k++)
            {
                Operand o = m.Operands[k];
                if (o.Kind == OperandKind.Reg && o.Reg < 255) d.Add((OperandKind.Reg, o.Reg));
                else if (o.Kind == OperandKind.Pred && o.Reg != 7) d.Add((OperandKind.Pred, o.Reg));
            }
        }

        internal static void AnalyseSpeculation(AmpereProgram p)
        {
            if (!Speculate) return;
            int n = p.Insts.Count;
            var reads = new (List<int> g, List<int> pr, bool ag, bool ap)[n];
            var defs = new List<(OperandKind, int)>[n];
            for (int k = 0; k < n; k++)
            {
                SassInst m = p.Insts[k];
                List<int> g = new(), pr = new();
                SourceRegs(m, g, pr, out bool ag, out bool ap);
                // at EXIT a fragment program's outputs are its registers
                if (m.Opcode == "EXIT" && p.Stage == ShaderStage.Fragment && p.Mesh == null) ag = true;
                if (m.Opcode is "CALL" or "RET" or "BRX" or "BRXU" or "JMX") { ag = true; ap = true; }   // conservative: callee / indirect
                reads[k] = (g, pr, ag, ap);
                defs[k] = new();
                DefiniteDests(m, defs[k]);
            }
            bool inSub(int addr) => p.Subroutines.Values.Any(r => addr >= r.Start && addr <= r.End);
            for (int k = 0; k < n; k++)
            {
                SassInst c = p.Insts[k];
                if (c.Pred == 7 || c.PredIsUniform || !PureAlu.Contains(c.Opcode) || c.DstCount == 0) continue;
                // every destination must be a GPR or predicate we track exactly
                bool ok = true;
                var dests = new List<(OperandKind, int)>();
                for (int q = 0; q < Math.Min(c.DstCount, c.Operands.Count); q++)
                {
                    Operand o = c.Operands[q];
                    if (o.Kind == OperandKind.Reg && o.Reg >= 255) continue;
                    if (o.Kind == OperandKind.Pred && o.Reg == 7) continue;
                    if (o.Kind is not (OperandKind.Reg or OperandKind.Pred)) { ok = false; break; }
                    int wd = Is32BitAlu(c) ? 1 : 2;
                    for (int w = 0; w < wd; w++) dests.Add((o.Kind, o.Reg + w));
                }
                if (!ok || dests.Count == 0 || inSub(c.Address)) continue;
                bool guardWritten = dests.Contains((OperandKind.Pred, c.Pred));
                if (DeadOnGuardFalse(p, k, c.Pred, c.PredNeg, guardWritten, dests, reads, defs)) p.Speculated.Add(c.Address);
            }
            if (Debug) Console.Error.WriteLine($"speculate: {p.Speculated.Count} predicated instructions run unconditionally");
        }

        /// Forward search from after instruction k on the paths where its guard is false: true when no path reads a destination
        /// before it is certainly written again.
        static bool DeadOnGuardFalse(AmpereProgram p, int k, int gp, bool gneg, bool guardWrittenByK,
                                     List<(OperandKind, int)> dests, (List<int> g, List<int> pr, bool ag, bool ap)[] reads, List<(OperandKind, int)>[] defs)
        {
            // state: instruction index, guard predicate still known (false sense), the destinations still live (bit set)
            int nd = dests.Count;
            if (nd > 30) return false;
            var seen = new HashSet<(int, bool, int)>();
            var work = new Stack<(int, bool, int)>();
            work.Push((k + 1, !guardWrittenByK, (1 << nd) - 1));
            int budget = SpeculateBudget;
            // under "guard false": an instruction guarded by the same predicate in the same sense as k does not execute; one guarded
            // by the opposite sense executes
            while (work.Count > 0)
            {
                (int i, bool known, int live) = work.Pop();
                if (live == 0) continue;
                if (i >= p.Insts.Count) continue;   // fell off the end: nothing reads
                if (!seen.Add((i, known, live))) continue;
                if (--budget < 0) return false;
                SassInst m = p.Insts[i];
                bool runs = true, certain = m.Pred == 7 && !m.PredNeg;
                if (m.Pred != 7 && !m.PredIsUniform && known && m.Pred == gp)
                {
                    bool sameSense = m.PredNeg == gneg;   // executes iff guard of k is true -> not here
                    runs = !sameSense;
                    certain = !sameSense;
                }
                if (m.Pred == 7 && m.PredNeg) runs = false;   // @!PT
                if (runs)
                {
                    var r = reads[i];
                    for (int b = 0; b < nd; b++)
                    {
                        if ((live >> b & 1) == 0) continue;
                        (OperandKind kind, int reg) = dests[b];
                        bool hit = kind == OperandKind.Reg ? r.ag || r.g.Contains(reg) : r.ap || r.pr.Contains(reg);
                        if (hit) return false;
                    }
                    if (certain)
                        foreach ((OperandKind kind, int reg) in defs[i])
                        {
                            int b = dests.IndexOf((kind, reg));
                            if (b >= 0) live &= ~(1 << b);
                        }
                    // the guard predicate changes: no longer known
                    if (defs[i].Contains((OperandKind.Pred, gp)) || m.Opcode == "R2P" || (m.Opcode is "PLOP3" or "FSETP" or "ISETP" or "HSETP2" or "DSETP" or "VOTE" or "SHFL" or "LOP3" or "IADD3" or "LEA" or "UISETP" && m.Operands.Take(Math.Min(m.DstCount, m.Operands.Count)).Any(o => o.Kind == OperandKind.Pred && o.Reg == gp)))
                        known = false;
                    if (!certain && m.Pred != 7) { /* may or may not run: its writes do not kill */ }
                }
                if (live == 0) continue;
                // successors
                string op = m.Opcode;
                if (op is "EXIT" or "KILL")
                {
                    if (runs && certain) continue;   // ends the thread
                    work.Push((i + 1, known, live));
                    continue;
                }
                if (op == "BRA" || (op == "CALL" && !p.Subroutines.ContainsKey(m.Address)))
                {
                    int? t = m.Operands.FirstOrDefault(o => o.Kind == OperandKind.Label) is Operand lo ? (int)lo.Offset : null;
                    // a branch with condition predicates besides the guard: both ways
                    bool extraCond = m.Operands.TakeWhile(o => o.Kind is OperandKind.Pred or OperandKind.UPred).Any(o => !(o.Reg == 7 && !o.Not));
                    bool taken = runs, maybeNot = !certain || extraCond || !runs;
                    if (t is int target && p.IndexOf.TryGetValue(target, out int ti) && runs) work.Push((ti, known, live));
                    else if (runs && t == null) return false;
                    if (maybeNot || !taken) work.Push((i + 1, known, live));
                    continue;
                }
                if (op is "RET" or "BRX" or "BRXU" or "JMX" or "CALL") return false;   // reads everything (above) anyway
                work.Push((i + 1, known, live));
            }
            return true;
        }

        /// Conservatively: may instruction m write register r of this kind?
        static bool Writes(SassInst m, OperandKind kind, int r)
        {
            int n = Math.Min(m.DstCount, m.Operands.Count);
            for (int k = 0; k < n; k++)
            {
                Operand o = m.Operands[k];
                if (o.Kind != kind) continue;
                if (kind is OperandKind.Reg or OperandKind.UReg)
                {
                    int limit = kind == OperandKind.Reg ? 255 : 63;
                    if (o.Reg < limit && r >= o.Reg && r < o.Reg + Width(m)) return true;
                }
                else if (o.Reg == r) return true;
            }
            // instructions whose destination count the decoder may not know: any write of a pair/quad is caught above;
            // be safe with the ones that write more than their first operand
            return m.DstCount == 0 && m.Operands.Count > 0 && m.Operands[0].Kind == kind && m.Operands[0].Reg == r
                && m.Opcode is not ("ST" or "STG" or "STS" or "STL" or "RED" or "ATOMS" or "ISBEWR" or "AST" or "SUST" or "SURED");
        }
    }
}
