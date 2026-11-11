// SPDX-License-Identifier: MIT
// --layout: field statistics and invariants of the NVN2 control section and the shader program header (SPH v4) over the corpora, the evidence behind
// tools/shaders/docs/nvn2_layout_xcheck.md. Writes layout_words.tsv, layout_invariants.tsv, layout_crosstabs.tsv, sph_words.tsv and sph_io.tsv.
using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using Nx.Shaders.Ampere;

namespace Nx.Shaders.XCheck
{
    static class Layout
    {
        static readonly string[] StageNames = { "vs", "ps", "gs", "tcs", "tes", "cs" };
        static readonly int[] SphShaderType = { 1, 5, 4, 2, 3, -1 };   // by control stage: SPH word 0 bits 10..13 (NAK SPHV3_T1_SHADER_TYPE_*)

        sealed class Inv { public long Tested, Pass; public string Example = ""; }
        sealed class Dist { public readonly Dictionary<uint, long> V = new(); public long N; }

        static string SName(int s) => s >= 0 && s < StageNames.Length ? StageNames[s] : "stage" + s;

        public static void Run(string[] dirs, string[] names, string outDir, int jobs)
        {
            var inv = new ConcurrentDictionary<(string name, string corpus, string stage), Inv>();
            var words = new ConcurrentDictionary<(string corpus, string stage, int off), Dist>();
            var cross = new ConcurrentDictionary<(string name, string corpus, string stage, string key), long>();
            var sphw = new ConcurrentDictionary<(string corpus, string stage, string field, string value), long>();
            var io = new ConcurrentDictionary<(string corpus, string check), long>();

            void Check(string name, string corpus, int stage, bool? ok, string hash)
            {
                if (ok == null) return;
                var a = inv.GetOrAdd((name, corpus, SName(stage)), _ => new Inv());
                lock (a) { a.Tested++; if (ok.Value) a.Pass++; else if (a.Example.Length == 0) a.Example = hash; }
            }
            void Cross(string name, string corpus, int stage, string key) => cross.AddOrUpdate((name, corpus, SName(stage), key), 1, (_, v) => v + 1);
            void Sph(string corpus, int stage, string field, object v) => sphw.AddOrUpdate((corpus, SName(stage), field, v.ToString()!), 1, (_, c) => c + 1);
            void IO(string corpus, string check, long n = 1) => io.AddOrUpdate((corpus, check), n, (_, v) => v + n);

            for (int c = 0; c < dirs.Length; c++)
            {
                if (!Directory.Exists(dirs[c])) continue;
                string cn = names[c];
                var rows = Corpus.Rows(dirs[c]);
                int ps = 0;
                Parallel.ForEach(rows, new ParallelOptions { MaxDegreeOfParallelism = jobs }, b =>
                {
                    byte[] ctrl, code;
                    try { ctrl = File.ReadAllBytes(b.CtrlPath); code = File.ReadAllBytes(b.CodePath); } catch (IOException) { return; }
                    if (ctrl.Length < 0x58) { IO(cn, "ctrl shorter than 0x58"); return; }
                    uint U(int o) => o + 4 <= ctrl.Length ? BitConverter.ToUInt32(ctrl, o) : 0;
                    uint U16(int o) => o + 2 <= ctrl.Length ? BitConverter.ToUInt16(ctrl, o) : 0u;
                    int stage = (int)U16(0x56);
                    string sn = SName(stage);
                    string h = b.Hash;
                    bool cs = stage == 5;

                    // per-word value distributions
                    for (int off = 0; off < Math.Min(ctrl.Length, 0x100); off += 4)
                    {
                        var d = words.GetOrAdd((cn, sn, off), _ => new Dist());
                        lock (d) { d.N++; uint v = U(off); d.V[v] = d.V.GetValueOrDefault(v) + 1; }
                    }

                    // ---- control-section invariants ----
                    uint ctl = U(0x30), prog = U(0x38), clen = U(0x3c), coff = U(0x40), area = U(0x44);
                    Check("magic == 0x98761234 and +4 == 0xc9", cn, stage, U(0) == 0x98761234 && U(4) == 0xc9, h);
                    Check("+0x0c == 0x000e0170", cn, stage, U(0x0c) == 0x000e0170, h);
                    Check("+0x30 == 0", cn, stage, ctl == 0, h);
                    Check("+0x30 (or 0xd8 when 0) == control file length", cn, stage, (ctl == 0 ? 0xd8u : ctl) == ctrl.Length, h);
                    Check("+0x98 == +0x30 (when +0x30 != 0)", cn, stage, ctl == 0 ? null : U(0x98) == ctl, h);
                    uint cl = ctl > 0xd8 ? 0xe0u : ctl;
                    Check("+0xbc == +0xc4 == +0xc8 == (+0x30 > 0xd8 ? 0xe0 : +0x30)", cn, stage, ctl == 0 ? null : (U(0xbc) == cl && U(0xc4) == cl && U(0xc8) == cl), h);
                    Check("+0x38 multiple of 0x80", cn, stage, prog % 0x80 == 0, h);
                    Check("+0x38 <= +0x40 (program precedes the constant data)", cn, stage, prog <= coff || clen == 0, h);
                    Check("+0x38 <= code file length", cn, stage, prog <= code.Length, h);
                    Check("+0x44 == code file length", cn, stage, area == code.Length, h);
                    Check("+0x40 + +0x3c <= +0x44", cn, stage, (ulong)coff + clen <= area, h);
                    Check("+0x44 - +0x40 == +0x3c (constants run to the end of the code area; b: c1 = [+0x40, +0x44))", cn, stage, clen == 0 ? null : area - coff == clen, h);
                    Check("+0x40 multiple of 0x100 (when +0x3c != 0)", cn, stage, clen == 0 ? null : coff % 0x100 == 0, h);
                    Check("+0x4c in 0x24..0x48 (b: register count)", cn, stage, U(0x4c) >= 0x24 && U(0x4c) <= 0x48, h);
                    Check("+0x4c in 6..0xff (ours: register count)", cn, stage, U(0x4c) >= 6 && U(0x4c) <= 0xff, h);
                    Check("+0x54 multiple of 0x1000 (b)", cn, stage, U(0x54) % 0x1000 == 0, h);
                    Check("+0x60 multiple of 0x1000 (b)", cn, stage, U(0x60) % 0x1000 == 0, h);
                    Check("+0x68 == +0x50 / 32", cn, stage, U(0x68) == U(0x50) / 32, h);
                    Check("+0xa4 == 0x01000000", cn, stage, U(0xa4) == 0x01000000, h);
                    Cross("+0x08 by stage", cn, stage, "0x" + U(0x08).ToString("x"));
                    if (cs)
                    {
                        ulong wg = (ulong)U(0x58) * U(0x5c) * U(0x60);
                        Check("compute: x*y*z of +0x58/+0x5c/+0x60 in 1..1024", cn, stage, wg >= 1 && wg <= 1024, h);
                        Check("compute: +0x50 == +0x64 (b: shared memory at +0x64)", cn, stage, U(0x50) == U(0x64), h);
                        Check("compute: +0x64 multiple of 0x1000 (cross-check of b's claim)", cn, stage, U(0x64) % 0x1000 == 0, h);
                        uint tab = U16(0xba), want = 0x10 + 0x10 * (uint)(ctrl.Length > 0xb2 ? ctrl[0xb1] + ctrl[0xb2] : 0) + 8 * (uint)(ctrl.Length > 0xb6 ? ctrl[0xb3] + ctrl[0xb6] : 0);
                        Check("compute: u16 +0xba == 0x10 + 0x10*(+0xb1 + +0xb2) + 8*(+0xb3 + +0xb6)", cn, stage, tab == want, h);
                        Cross("compute (+0x50, +0x64, +0x68)", cn, stage, $"{U(0x50):x},{U(0x64):x},{U(0x68):x}");
                        Cross("compute (+0xb1 ubo, +0xb2 ssbo, +0xb3 tex, +0xb6 img)", cn, stage, ctrl.Length > 0xb6 ? $"{ctrl[0xb1]},{ctrl[0xb2]},{ctrl[0xb3]},{ctrl[0xb6]}" : "short");
                        Cross("compute (+0xa9..+0xb0 root table word counts)", cn, stage, ctrl.Length > 0xb0 ? string.Join(",", Enumerable.Range(0xa9, 8).Select(i => ctrl[i].ToString())) : "short");
                    }

                    // ---- SPH ----
                    uint w0 = 0; int sphType = 0, ver = 0, shType = 0;
                    if (cs) Check("compute: code does not start with an SPH (word 0 type 1/2, version 4)", cn, stage, !(((code[0] & 31) is 1 or 2) && ((BitConverter.ToUInt32(code, 0) >> 5) & 31) == 4), h);
                    else
                    {
                        if (code.Length < 0x80) { IO(cn, "graphics code shorter than the SPH"); return; }
                        w0 = BitConverter.ToUInt32(code, 0);
                        sphType = (int)(w0 & 31); ver = (int)((w0 >> 5) & 31); shType = (int)((w0 >> 10) & 15);
                        Sph(cn, stage, "sph_type", sphType); Sph(cn, stage, "version", ver); Sph(cn, stage, "shader_type", shType);
                        Check("SPH word 0: version == 4", cn, stage, ver == 4, h);
                        Check("SPH word 0: sph_type == (ps ? 2 : 1)", cn, stage, sphType == (stage == 1 ? 2 : 1), h);
                        Check("SPH word 0: shader_type matches control +0x56 (vs 1, tcs 2, tes 3, gs 4, ps 5)", cn, stage, stage >= 0 && stage < 5 && shType == SphShaderType[stage], h);
                        foreach (var (nm, lo, w) in new[] { ("mrt_enable", 14, 1), ("kills_pixels", 15, 1), ("does_global_store", 16, 1), ("sass_version", 17, 4), ("bit24 (gs passthrough)", 24, 1),
                                                           ("bit25 (isbe_space_sharing, vs)", 25, 1), ("does_load_or_store", 26, 1), ("does_fp64", 27, 1), ("stream_out_mask", 28, 4), ("reserved 21..23", 21, 3) })
                            Sph(cn, stage, nm, (w0 >> lo) & ((1u << w) - 1));
                        Sph(cn, stage, "SPH bytes 0x50..0x7f all zero", code.AsSpan(0x50, 0x30).IndexOfAnyExcept((byte)0) < 0);
                        Sph(cn, stage, "word 1 bits 24..31 per_patch_attribute_count", BitConverter.ToUInt32(code, 4) >> 24);
                        Sph(cn, stage, "word 2 bits 24..31 threads_per_input_primitive", BitConverter.ToUInt32(code, 8) >> 24);
                        Sph(cn, stage, "word 3 bits 24..27 output_topology", (BitConverter.ToUInt32(code, 12) >> 24) & 15);
                        Sph(cn, stage, "word 4 bits 0..11 max_output_vertices", BitConverter.ToUInt32(code, 16) & 0xfff);
                        Sph(cn, stage, "words 1,2,3 shader_local_memory_low/high/crs_size != 0", $"{(BitConverter.ToUInt32(code, 4) & 0xffffff) != 0},{(BitConverter.ToUInt32(code, 8) & 0xffffff) != 0},{(BitConverter.ToUInt32(code, 12) & 0xffffff) != 0}");
                    }

                    // attribute usage and memory spaces vs the SPH maps and the control section: every 20th pixel block of title A, all others
                    bool sample = cn != "a" || stage != 1 || (System.Threading.Interlocked.Increment(ref ps) % 20 == 0);
                    if (!sample) return;
                    int start = cs ? 0 : 0x80, end = (int)Math.Min(prog, (uint)code.Length);
                    while (end - 16 >= start && code.AsSpan(end - 16, 16).IndexOfAnyExcept((byte)0) < 0) end -= 16;
                    if (end <= start) return;
                    ulong B(int bit, int len) { ulong v = 0; for (int i = 0; i < len; i++) if ((code[(bit + i) >> 3] >> ((bit + i) & 7) & 1) != 0) v |= 1UL << i; return v; }
                    bool usesLocal = false, usesShared = false; long maxShared = -1;
                    foreach (var inst in SassDecoder.DecodeBlock(code, start, end))
                    {
                        string op = inst.Opcode;
                        if (op is "LDL" or "STL") usesLocal = true;
                        if (op is "LDS" or "STS" or "ATOMS" or "LDSM" or "REDUX") usesShared = true;
                        if (op is "LDS" or "STS" or "ATOMS")
                        {
                            var m = inst.Operands.FirstOrDefault(o => o.Kind == OperandKind.Mem);
                            if (m != null && m.Reg == 255 && m.IndexReg < 0) maxShared = Math.Max(maxShared, m.Offset + 4);
                        }
                        if (!cs && stage == 1 && op == "IPA")
                        {
                            var at = inst.Operands.FirstOrDefault(o => o.Kind == OperandKind.Attr);
                            if (at == null || at.Offset < 0x80 || at.Offset >= 0x280) { IO(cn, "ps IPA outside generic attributes"); continue; }
                            int vec = (int)((at.Offset - 0x80) / 16), comp = (int)((at.Offset & 0xf) / 4);
                            int imap = (int)B(192 + vec * 8 + comp * 2, 2);
                            string mode = inst.Mods.Contains("CONSTANT") ? "CONSTANT" : inst.Mods.Contains("SC") ? "SC" : "PERSPECTIVE";
                            int want = mode == "CONSTANT" ? 1 : mode == "SC" ? 3 : 2;
                            IO(cn, "ps IPA reads");
                            if (imap == 0) IO(cn, "ps IPA of a component imap (SPH bits 192..447) says unused");
                            else if (imap != want) { IO(cn, $"ps IPA mode {mode} but imap says {imap}"); }
                            else IO(cn, "ps IPA mode agrees with imap");
                        }
                        else if (!cs && stage == 0 && op == "AST")
                        {
                            var at = inst.Operands.FirstOrDefault(o => o.Kind == OperandKind.Attr);
                            if (at == null || at.Offset < 0x80 || at.Offset >= 0x280) continue;
                            int n = inst.Mods.Contains("128") ? 4 : inst.Mods.Contains("96") ? 3 : inst.Mods.Contains("64") ? 2 : 1;
                            for (int k = 0; k < n; k++)
                            {
                                long o = at.Offset + 4 * k;
                                int vec = (int)((o - 0x80) / 16), comp = (int)((o & 0xf) / 4);
                                IO(cn, "vs AST generic components");
                                if (B(432 + vec * 4 + comp, 1) == 0) IO(cn, "vs AST component not in omap (SPH bits 432..559)");
                            }
                        }
                    }
                    Cross("memory spaces: (uses LDL/STL, uses LDS/STS/ATOMS, +0x50 != 0, +0x64 != 0)", cn, stage, $"{usesLocal},{usesShared},{U(0x50) != 0},{U(0x64) != 0}");
                    if (!cs) Cross("local memory: (uses LDL/STL, +0x50 != 0, SPH low local size != 0)", cn, stage, $"{usesLocal},{U(0x50) != 0},{(BitConverter.ToUInt32(code, 4) & 0xffffff) != 0}");
                    if (cs && maxShared >= 0)
                    {
                        Check("compute: static shared access end <= +0x64", cn, stage, maxShared <= U(0x64), h);
                        Check("compute: static shared access end <= +0x50", cn, stage, maxShared <= U(0x50), h);
                    }
                    IO(cn, stage == 1 ? "ps blocks attribute-checked" : "vs blocks attribute-checked");
                });
            }

            // ---- write ----
            string Top(Dist d) => string.Join("; ", d.V.OrderByDescending(kv => kv.Value).ThenBy(kv => kv.Key).Take(6).Select(kv => $"0x{kv.Key:x}:{kv.Value}"));
            using (var w = new StreamWriter(Path.Combine(outDir, "layout_words.tsv"), false, new UTF8Encoding(false)) { NewLine = "\n" })
            {
                w.WriteLine("corpus\tstage\toffset\tblocks\tdistinct\ttop_values");
                foreach (var kv in words.OrderBy(k => k.Key.corpus).ThenBy(k => k.Key.stage).ThenBy(k => k.Key.off))
                    w.WriteLine($"{kv.Key.corpus}\t{kv.Key.stage}\t0x{kv.Key.off:x2}\t{kv.Value.N}\t{kv.Value.V.Count}\t{Top(kv.Value)}");
            }
            using (var w = new StreamWriter(Path.Combine(outDir, "layout_invariants.tsv"), false, new UTF8Encoding(false)) { NewLine = "\n" })
            {
                w.WriteLine("invariant\tcorpus\tstage\ttested\tpassed\tfailed\texample_failing_block");
                foreach (var kv in inv.OrderBy(k => k.Key.name, StringComparer.Ordinal).ThenBy(k => k.Key.corpus).ThenBy(k => k.Key.stage))
                    w.WriteLine($"{kv.Key.name}\t{kv.Key.corpus}\t{kv.Key.stage}\t{kv.Value.Tested}\t{kv.Value.Pass}\t{kv.Value.Tested - kv.Value.Pass}\t{kv.Value.Example}");
            }
            using (var w = new StreamWriter(Path.Combine(outDir, "layout_crosstabs.tsv"), false, new UTF8Encoding(false)) { NewLine = "\n" })
            {
                w.WriteLine("table\tcorpus\tstage\tvalue\tblocks");
                foreach (var kv in cross.OrderBy(k => k.Key.name).ThenBy(k => k.Key.corpus).ThenBy(k => k.Key.stage).ThenByDescending(k => k.Value))
                    w.WriteLine($"{kv.Key.name}\t{kv.Key.corpus}\t{kv.Key.stage}\t{kv.Key.key}\t{kv.Value}");
            }
            using (var w = new StreamWriter(Path.Combine(outDir, "sph_words.tsv"), false, new UTF8Encoding(false)) { NewLine = "\n" })
            {
                w.WriteLine("corpus\tstage\tfield\tvalue\tblocks");
                foreach (var kv in sphw.OrderBy(k => k.Key.corpus).ThenBy(k => k.Key.stage).ThenBy(k => k.Key.field, StringComparer.Ordinal).ThenByDescending(k => k.Value))
                    w.WriteLine($"{kv.Key.corpus}\t{kv.Key.stage}\t{kv.Key.field}\t{kv.Key.value}\t{kv.Value}");
            }
            using (var w = new StreamWriter(Path.Combine(outDir, "sph_io.tsv"), false, new UTF8Encoding(false)) { NewLine = "\n" })
            {
                w.WriteLine("corpus\tcheck\tcount");
                foreach (var kv in io.OrderBy(k => k.Key.corpus).ThenBy(k => k.Key.check, StringComparer.Ordinal)) w.WriteLine($"{kv.Key.corpus}\t{kv.Key.check}\t{kv.Value}");
            }
            Console.WriteLine($"layout: {inv.Count} invariant rows, {words.Count} word rows -> {outDir}");
        }
    }
}
