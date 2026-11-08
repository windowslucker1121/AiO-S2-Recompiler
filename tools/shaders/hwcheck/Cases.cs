// SPDX-License-Identifier: MIT
// Test instructions: where the encodings come from, how they are moved into the register window, and the inputs.
//
// Sources (both real NVIDIA encodings, never hand-assembled):
//   ops   ops.ptx (our own PTX, every ALU form we want) compiled by ptxas -arch=sm_86; words read with cuobjdump -sass.
//   corpus  the nvdisasm oracle of the game's shaders (work/shaders/oracle/SM86/*.sass, see docs/AMPERE.md section 5): the
//           opcode/modifier combinations the game actually uses. Read at run time only; nothing from it is committed.
// Each word is rewritten so that every operand lives in the window the oracle loads and stores (R0..R15, P0..P6):
//   - constant-bank and uniform-register source forms (ALU form 3/5/6/7) become the register form (1);
//   - register fields Rd 16..23, Ra 24..31, Rb 32..39 (register form only), Rc 64..71 become r mod 16 (RZ stays);
//   - a uniform destination (REDUX) becomes UR8 and is followed by IMAD.U32 R15, RZ, RZ, UR8;
//   - SHFL loses its guard (a shuffle from a lane the guard switched off is undefined on both sides).
// The rewritten word must decode to the same opcode and modifiers, use only window operands, and nvdisasm must print it
// exactly as our decoder does (checked in Program.Verify).
using Nx.Shaders.Ampere;
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;

namespace Hwcheck
{
    sealed class Case
    {
        public string Id, Source, Key, Text;
        public List<(ulong lo, ulong hi)> Words = new();
        public SassInst Inst;
        public MemSpec Mem;
        public string Status = "", Detail = "";
        public int Threads, Mismatches;
        public string Example = "";
    }

    static class Cases
    {
        /// Opcodes in scope: ALU work with a single-instruction meaning, and the memory classes that need no texture or
        /// descriptor (Mem.cs). Texture, constant, attribute, barrier, control flow and special-register reads are out of scope.
        public static readonly HashSet<string> InScope = new()
        {
            "FADD", "FMUL", "FFMA", "FMNMX", "FSET", "FSETP", "FSEL", "FRND", "MUFU", "F2I", "I2F", "I2FP", "F2F", "F2FP", "I2I",
            "HADD2", "HMUL2", "HFMA2", "HMNMX2", "HSET2", "HSETP2", "DADD", "DMUL", "DFMA", "DSETP",
            "IADD3", "IMAD", "LEA", "SHF", "LOP3", "PLOP3", "ISETP", "IMNMX", "IABS", "POPC", "BREV", "FLO", "SGXT", "BMSK", "PRMT",
            "IDP", "VABSDIFF", "MOV", "SEL", "P2R", "R2P", "SHFL", "VOTE", "REDUX", "MATCH",
            "LDS", "STS", "ATOMS", "LDL", "STL", "LDG", "STG", "ATOMG", "RED",
            "HMMA", "LDSM", "MOVM",
        };

        static readonly Regex OracleLine = new(@"^/\*[0-9a-f]+\*/\s+(.*?)\s*;\s*(0x[0-9a-f]{16})\s+(0x[0-9a-f]{16})\s*$");
        static readonly Regex CuobjLine = new(@"^\s*/\*[0-9a-f]{4}\*/\s+(.*?)\s*;\s*/\*\s*(0x[0-9a-f]{16})\s*\*/\s*$");
        static readonly Regex CuobjCont = new(@"^\s*/\*\s*(0x[0-9a-f]{16})\s*\*/\s*$");

        static string OpToken(string text)
        {
            string t = text.StartsWith("@") ? text[(text.IndexOf(' ') + 1)..] : text;
            int sp = t.IndexOf(' ');
            return sp < 0 ? t : t[..sp];
        }

        static bool TokenInScope(string tok) { int d = tok.IndexOf('.'); return InScope.Contains(d < 0 ? tok : tok[..d]); }

        /// SASS of a cubin (cuobjdump -sass): (lo, hi, text).
        public static IEnumerable<(ulong, ulong, string)> FromCuobjdump(string sass)
        {
            string[] lines = sass.Split('\n');
            for (int k = 0; k + 1 < lines.Length; k++)
            {
                Match m = CuobjLine.Match(lines[k]);
                if (!m.Success) continue;
                Match c = CuobjCont.Match(lines[k + 1]);
                if (!c.Success) continue;
                yield return (Convert.ToUInt64(m.Groups[2].Value, 16), Convert.ToUInt64(c.Groups[1].Value, 16), m.Groups[1].Value);
            }
        }

        /// Corpus oracle: up to `perToken` distinct words per printed opcode token, in-scope tokens only. Cached in `cache`.
        public static List<(ulong, ulong, string)> FromCorpus(string dir, string cache, int perToken)
        {
            if (File.Exists(cache) && File.GetLastWriteTimeUtc(cache) > Directory.GetLastWriteTimeUtc(dir))
                return File.ReadAllLines(cache).Select(l => l.Split('\t')).Select(f => (Convert.ToUInt64(f[0], 16), Convert.ToUInt64(f[1], 16), f[2])).ToList();
            var byTok = new Dictionary<string, Dictionary<(ulong, ulong), string>>();
            foreach (string file in Directory.EnumerateFiles(dir, "*.sass").OrderBy(f => f, StringComparer.Ordinal))
            {
                foreach (string line in File.ReadLines(file))
                {
                    int star = line.IndexOf("*/ ", StringComparison.Ordinal);
                    if (star < 0) continue;
                    string rest = line[(star + 3)..];
                    string tok = OpToken(rest);
                    if (!TokenInScope(tok)) continue;
                    if (!byTok.TryGetValue(tok, out var set)) byTok[tok] = set = new();
                    if (set.Count >= perToken) continue;
                    Match m = OracleLine.Match(line);
                    if (!m.Success) continue;
                    set.TryAdd((Convert.ToUInt64(m.Groups[2].Value, 16), Convert.ToUInt64(m.Groups[3].Value, 16)), m.Groups[1].Value);
                }
            }
            var list = byTok.SelectMany(t => t.Value.Select(w => (w.Key.Item1, w.Key.Item2, w.Value))).ToList();
            Directory.CreateDirectory(Path.GetDirectoryName(cache));
            File.WriteAllLines(cache, list.Select(x => $"{x.Item1:x16}\t{x.Item2:x16}\t{x.Item3}"));
            return list;
        }

        /// The game's shader corpus itself (unique.tsv, code/<h>.bin, ctrl/<h>.bin; read at run time only): every in-scope
        /// instruction decoded with our decoder, up to `perShape` distinct words per shape (Shape) whose shape matches `only` (null:
        /// all). For forms the nvdisasm oracle does not exist for; nvdisasm still checks every rewritten word (Program.Verify).
        public static List<(ulong, ulong, string)> FromGameCorpus(string dir, string only, int perShape)
        {
            var rows = File.ReadLines(Path.Combine(dir, "unique.tsv")).Skip(1).Select(l => l.Split('\t')).Where(f => f.Length > 9)
                           .OrderBy(f => f[0], StringComparer.Ordinal).ToList();
            var re = only == null ? null : new Regex(only);
            var found = new Dictionary<string, Dictionary<(ulong, ulong), (string text, int blocks)>>();
            System.Threading.Tasks.Parallel.ForEach(rows, f =>
            {
                byte[] code, ctrl;
                try { code = File.ReadAllBytes(Path.Combine(dir, "code", f[0] + ".bin")); ctrl = File.ReadAllBytes(Path.Combine(dir, "ctrl", f[9] + ".bin")); }
                catch (IOException) { return; }
                if (ctrl.Length < 0x58) return;
                int stage = BitConverter.ToUInt16(ctrl, 0x56);
                int end = Math.Min((int)BitConverter.ToUInt32(ctrl, 0x38), code.Length), start = stage == 5 ? 0 : 0x80;
                if (end <= start) return;
                var local = new Dictionary<(ulong, ulong), (string, string)>();
                foreach (SassInst i in SassDecoder.DecodeBlock(code, start, end))
                {
                    if (!InScope.Contains(i.Opcode)) continue;
                    string shape = Shape(i);
                    if (re != null && !re.IsMatch(shape)) continue;
                    local.TryAdd((i.Lo, i.Hi), (shape, i.ToSassText()));
                }
                lock (found)
                    foreach (var (w, (shape, text)) in local)
                    {
                        if (!found.TryGetValue(shape, out var set)) found[shape] = set = new();
                        set[w] = set.TryGetValue(w, out var e) ? (e.text, e.blocks + 1) : (text, 1);
                    }
            });
            // per shape the words in the most blocks: real instructions recur, while data the decoder reads past a block's code
            // (constants, names) is mostly unique and decodes to illegal forms; ties by word, so the selection is deterministic
            return found.OrderBy(s => s.Key, StringComparer.Ordinal)
                        .SelectMany(s => s.Value.Where(w => w.Value.blocks >= 2).OrderByDescending(w => w.Value.blocks).ThenBy(w => w.Key)
                                             .Take(perShape).Select(w => (w.Key.Item1, w.Key.Item2, w.Value.text))).ToList();
        }

        static ulong SetField(ulong lo, int start, int len, ulong v) { ulong m = ((1UL << len) - 1) << start; return (lo & ~m) | ((v << start) & m); }

        /// Shape of an instruction: what makes two encodings test different things (opcode, modifiers, guard sense, operand kinds
        /// and modifiers). Register numbers and immediate values are not part of it.
        public static string Shape(SassInst i)
        {
            var sb = new System.Text.StringBuilder();
            sb.Append(i.Pred == 7 ? "" : i.PredNeg ? "@!P " : "@P ");
            sb.Append(i.Opcode);
            foreach (string m in i.Mods) sb.Append('.').Append(m);
            foreach (Operand o in i.Operands)
            {
                sb.Append(' ');
                if (o.Not) sb.Append('!');
                if (o.Neg) sb.Append('-');
                if (o.Abs) sb.Append('|');
                sb.Append(o.Kind switch
                {
                    OperandKind.Reg => o.Reg == 255 ? "RZ" : "R",
                    OperandKind.Pred => o.Reg == 7 ? "PT" : "P",
                    OperandKind.Imm => "i",
                    OperandKind.FImm => "f",
                    OperandKind.Mem => Regex.Replace(Regex.Replace(o.Text ?? "", @"R\d+", "R"), @"-?0x[0-9a-f]+", "i"),
                    _ => o.Kind.ToString(),
                });
                if (o.Suffixes != null) foreach (string s in o.Suffixes) if (s != ".reuse") sb.Append(s);
            }
            return sb.ToString();
        }

        /// The word moved into the register window, or null (with the reason) when it cannot be.
        public static List<(ulong, ulong)> Rewrite(ulong lo, ulong hi, out SassInst inst, out string why)
        {
            inst = null;
            SassInst orig = SassDecoder.Decode(lo, hi, 0);
            why = null;
            if (orig.Opcode.StartsWith(".") || !InScope.Contains(orig.Opcode)) { why = "out of scope"; return null; }
            if (orig.PredIsUniform) { why = "uniform guard"; return null; }
            ulong nlo = lo, nhi = hi & ((1UL << 41) - 1);
            nhi |= hi & ~((1UL << 41) - 1);
            int form = (int)(lo >> 9) & 7;
            bool alu = orig.Operands.Skip(orig.DstCount).Any(o => o.Kind is OperandKind.CBuf || (o.Kind == OperandKind.UReg && o.Reg != 63));
            if (alu && form is 3 or 5 or 6 or 7)
            {
                nlo = SetField(nlo, 9, 3, 1);
                nlo = SetField(nlo, 32, 30, 0);            // clear the bank/offset or UR field, keep neg/abs at 62/63
                nhi &= ~(1UL << (91 - 64));                 // cx flag
                nlo = SetField(nlo, 32, 8, 6);
            }
            // registers into the window
            int nform = (int)(nlo >> 9) & 7;
            foreach (int f in new[] { 16, 24, 64 }.Concat(nform == 1 ? new[] { 32 } : Array.Empty<int>()))
            {
                ulong v = f >= 64 ? (nhi >> (f - 64)) & 0xff : (nlo >> f) & 0xff;
                if (v == 255) continue;
                if (f >= 64) nhi = SetField(nhi, f - 64, 8, v % 16); else nlo = SetField(nlo, f, 8, v % 16);
            }
            var words = new List<(ulong, ulong)>();
            SassInst ni = SassDecoder.Decode(nlo, nhi, 0);
            // registers in other fields (e.g. SHFL's third source): find the byte that holds the number and move it too
            for (int guard = 0; guard < 4; guard++)
            {
                Operand far = ni.Operands.FirstOrDefault(o => o.Kind == OperandKind.Reg && o.Reg >= 16 && o.Reg != 255);
                if (far == null) break;
                bool moved = false;
                for (int f = 16; f <= 120 && !moved; f += 8)
                {
                    ulong v = f >= 64 ? (nhi >> (f - 64)) & 0xff : (nlo >> f) & 0xff;
                    if ((int)v != far.Reg) continue;
                    ulong tlo = nlo, thi = nhi;
                    if (f >= 64) thi = SetField(thi, f - 64, 8, v % 16); else tlo = SetField(tlo, f, 8, v % 16);
                    SassInst t = SassDecoder.Decode(tlo, thi, 0);
                    if (t.Opcode == ni.Opcode && t.Mods.SequenceEqual(ni.Mods) && t.Operands.Count(o => o.Kind == OperandKind.Reg && o.Reg == far.Reg) < ni.Operands.Count(o => o.Kind == OperandKind.Reg && o.Reg == far.Reg))
                    { nlo = tlo; nhi = thi; ni = t; moved = true; }
                }
                if (!moved) break;
            }
            Operand udst = ni.DstCount > 0 ? ni.Operands.Take(ni.DstCount).FirstOrDefault(o => o.Kind == OperandKind.UReg && o.Reg != 63) : null;
            if (udst != null)
            {
                nlo = SetField(nlo, 16, 6, 8);
                ni = SassDecoder.Decode(nlo, nhi, 0);
            }
            if (ni.Opcode == "SHFL") { nlo = SetField(nlo, 12, 4, 7); ni = SassDecoder.Decode(nlo, nhi, 0); }
            // IMAD.WIDE's addend is a register pair and must be even (an odd pair is CUDA_ERROR_ILLEGAL_INSTRUCTION on the RTX 3080).
            // Turning a constant-bank form into the register form moves the b register into the c field, which can make it odd.
            if (ni.Opcode == "IMAD" && ni.Mods.Contains("WIDE") && (nhi & 0xff) is var rc && rc != 255 && (rc & 1) != 0)
            { nhi = SetField(nhi, 0, 8, rc & ~1UL); ni = SassDecoder.Decode(nlo, nhi, 0); }
            if (ni.Opcode != orig.Opcode || !ni.Mods.SequenceEqual(orig.Mods)) { why = $"rewrite changed it: {ni.ToSassText()}"; return null; }
            foreach (Operand o in ni.Operands)
            {
                bool ok = o.Kind switch
                {
                    OperandKind.Reg => o.Reg < 16 || o.Reg == 255,
                    OperandKind.Pred => true,
                    OperandKind.UReg => o.Reg == 63 || (udst != null && o.Reg == 8),
                    OperandKind.UPred => o.Reg == 7,
                    OperandKind.Mem => (o.Reg < 16 || o.Reg == 255) && (o.IndexReg < 0 || (o.IndexIsUReg && o.IndexReg == 63)),
                    OperandKind.Imm or OperandKind.FImm or OperandKind.Other => true,
                    _ => false,
                };
                if (!ok) { why = $"operand outside the window: {SassText.OperandText(o)}"; return null; }
            }
            inst = ni;
            words.Add((nlo, nhi));
            if (udst != null) words.Add((0x00000008ff0f7e24, 0x000000000f8e00ff));   // IMAD.U32 R15, RZ, RZ, UR8
            return words;
        }
    }

    /// Register-file inputs. One block of `threads` invocations, 20 words each (R0..R15, predicate word, 3 pad words). Every warp
    /// gets one flavour, so warp operations see consistent data:
    ///   0 fp32 (edge values: zeros, denormals, inf, NaN, rounding boundaries, conversion limits; and random floats)
    ///   1 fp16 pairs (edge halves and random halves)
    ///   2 integers (0, +-1, min/max, shift amounts 0..64, byte patterns, random)
    ///   3 raw random bits
    ///   4 fp64 pairs (even register low word, odd high word)
    ///   5 small values (integers and floats near 0..4, so multiplies, shuffles' lane numbers and selectors hit real cases)
    /// Predicate words are random; the memory window (words 20..27, Mem.cs) holds integer edge values and random bits.
    static class Inputs
    {
        public const int Words = 28, PredWord = 16;

        static readonly uint[] F32Edge =
        {
            0x00000000, 0x80000000, 0x00000001, 0x80000001, 0x007fffff, 0x807fffff, 0x00800000, 0x80800000, 0x7f7fffff, 0xff7fffff,
            0x7f800000, 0xff800000, 0x7fc00000, 0xffc00000, 0x7f800001, 0x7fffffff, 0x3f800000, 0xbf800000, 0x3f000000, 0xbf000000,
            0x3fc00000, 0xbfc00000, 0x40200000, 0xc0200000, 0x3effffff, 0x3f000001, 0x4b000000, 0x4b000001, 0x4affffff, 0x4b7fffff,
            0x4b800000, 0x4b800001, 0x4effffff, 0x4f000000, 0xcf000000, 0xcf000001, 0x4f7fffff, 0x4f800000, 0x5f000000, 0xdf000000,
            0x477fe000, 0x477ff000, 0x47800000, 0x387fc000, 0x33800000, 0x33000000, 0x3e800000, 0x40490fdb, 0x3e22f983, 0x42c80000,
            0xc2c80000, 0x501502f9, 0x3f7fffff, 0x3f800001, 0x40000000, 0xc0000000, 0x41200000, 0x00400000, 0x7f000000, 0x0d800000,
        };
        static readonly ushort[] F16Edge =
        {
            0x0000, 0x8000, 0x0001, 0x8001, 0x03ff, 0x0400, 0x7bff, 0xfbff, 0x7c00, 0xfc00, 0x7e00, 0xfe00, 0x7c01, 0x3c00, 0xbc00,
            0x3800, 0xb800, 0x3e00, 0x4100, 0x4000, 0x3bff, 0x3c01, 0x5bff, 0x6400, 0x0200,
        };
        static readonly uint[] IntEdge =
        {
            0, 1, 0xffffffff, 2, 0xfffffffe, 0x7fffffff, 0x80000000, 0x80000001, 0x7ffffffe, 0xffff, 0x10000, 0x8000, 0x7fff, 0xffff8000,
            0xff, 0x80, 0x7f, 0xffffff80, 0x00ff00ff, 0xff00ff00, 0x01020304, 0x76543210, 0x12345678, 0x89abcdef, 0x55555555, 0xaaaaaaaa,
            31, 32, 33, 63, 64, 65, 0x1f, 0x20, 0x3f, 0x40, 0x100, 0x7, 0x8, 0xf, 0x10, 0x18, 0x1e, 0x21,
        };
        static readonly ulong[] F64Edge =
        {
            0, 0x8000000000000000, 1, 0x000fffffffffffff, 0x0010000000000000, 0x7fefffffffffffff, 0x7ff0000000000000, 0xfff0000000000000,
            0x7ff8000000000000, 0x7ff0000000000001, 0x3ff0000000000000, 0xbff0000000000000, 0x3fe0000000000000, 0x4330000000000000,
            0x4330000000000001, 0x41dfffffffc00000, 0x41e0000000000000, 0xc1e0000000000000, 0x43e0000000000000, 0x36a0000000000000,
            0x3810000000000000, 0x47efffffe0000000, 0x47f0000000000000,
        };

        static uint RandF32(Random r)
        {
            int e = r.Next(100) < 70 ? 127 + r.Next(-20, 21) : r.Next(0, 256);
            return (uint)(r.Next(2) << 31) | (uint)e << 23 | (uint)r.Next(1 << 23);
        }

        static ushort RandF16(Random r)
        {
            int e = r.Next(100) < 70 ? 15 + r.Next(-6, 7) : r.Next(0, 32);
            return (ushort)(r.Next(2) << 15 | e << 10 | r.Next(1 << 10));
        }

        static uint Small(Random r)
        {
            switch (r.Next(4))
            {
                case 0: return (uint)r.Next(0, 40);
                case 1: return (uint)-r.Next(1, 5);
                case 2: return BitConverter.SingleToUInt32Bits(r.Next(-16, 17) / 4f);
                default: return BitConverter.SingleToUInt32Bits((float)(r.NextDouble() * 8 - 4));
            }
        }

        public static uint[] Make(int threads, int seed)
        {
            var r = new Random(seed);
            uint[] d = new uint[threads * Words];
            for (int t = 0; t < threads; t++)
            {
                int flavour = (t / 32) % 6;
                int b = t * Words;
                for (int k = 0; k < 16; k++)
                {
                    uint v = flavour switch
                    {
                        0 => r.Next(2) == 0 ? F32Edge[r.Next(F32Edge.Length)] : RandF32(r),
                        1 => (uint)(r.Next(2) == 0 ? F16Edge[r.Next(F16Edge.Length)] : RandF16(r)) << 16 | (r.Next(2) == 0 ? F16Edge[r.Next(F16Edge.Length)] : RandF16(r)),
                        2 => r.Next(3) != 0 ? IntEdge[r.Next(IntEdge.Length)] : (uint)r.NextInt64(0, 1L << 32),
                        3 => (uint)r.NextInt64(0, 1L << 32),
                        5 => Small(r),
                        _ => 0,
                    };
                    d[b + k] = v;
                }
                if (flavour == 4)
                    for (int k = 0; k < 16; k += 2)
                    {
                        ulong v = r.Next(2) == 0 ? F64Edge[r.Next(F64Edge.Length)]
                            : (ulong)r.Next(2) << 63 | (ulong)(1023 + r.Next(-30, 31)) << 52 | (ulong)r.NextInt64(0, 1L << 52);
                        if (r.Next(8) == 0) v ^= 1UL << 63;
                        d[b + k] = (uint)v; d[b + k + 1] = (uint)(v >> 32);
                    }
                d[b + PredWord] = (uint)r.Next(128);
                for (int k = 0; k < MemSpec.Words; k++) d[b + MemSpec.Word + k] = r.Next(2) == 0 ? IntEdge[r.Next(IntEdge.Length)] : (uint)r.NextInt64(0, 1L << 32);

            }
            return d;
        }
    }
}
