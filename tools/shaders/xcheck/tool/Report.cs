// SPDX-License-Identifier: MIT
// Turns the scan into findings, asks nvdisasm about each one, and writes disagreements.tsv + summary.txt.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using Nx.Shaders.Ampere;

namespace Nx.Shaders.XCheck
{
    sealed class Finding
    {
        public string Key = "", Level = "", Kind = "", Ours = "", NxEmitter = "", Detail = "";
        public int Op12, Bit = -1;
        public long[] N = new long[3];
        public long Total;                      // L2: instructions of the opcode over all corpora
        public bool Live => Level != "L2-bit" || (N.Sum() > 0 && N.Sum() < Total);   // the bit takes both values in the corpora          // instructions of the form (L1) / instructions with the bit set (L2)
        public string Hash = "";
        public ulong Lo, Hi;                    // example word (the flipped twin for level 2 is Lo2/Hi2)
        public ulong Lo2, Hi2;
        public string OursText = "", NxText = "", Nv = "", NvFlip = "";
        public string Auto = "";                // automatic verdict from nvdisasm
        public string Status = "open", Note = "";
    }

    static class Report
    {
        static string W(ulong lo, ulong hi) => $"0x{lo:x16} 0x{hi:x16}";
        static string Clip(string s) => s.Length > 240 ? s[..240] + "..." : s;
        static string Text(ulong lo, ulong hi, int addr) => Nvdis.Norm(SassDecoder.Decode(lo, hi, addr).ToSassText());

        static string ToolDir()
        {
            for (var d = new DirectoryInfo(AppContext.BaseDirectory); d != null; d = d.Parent)
                if (File.Exists(Path.Combine(d.FullName, "xcheck", "verdicts.tsv"))) return Path.Combine(d.FullName, "xcheck");
            return "";
        }

        public static void Write(string dir, ScanData d, List<(FormAcc f, Ex e)> exs, Dictionary<(int op12, int bit), Cell> cells, bool nvdisasm, int jobs)
        {
            var fs = new List<Finding>();
            var opTotal = new long[4096];
            foreach (var kv in d.L1) opTotal[kv.Key.op12] += kv.Value.N.Sum();

            // ---- level 1: opcodes ----
            foreach (var kv in d.L1.OrderBy(k => k.Key.op12).ThenBy(k => k.Key.ours, StringComparer.Ordinal))
            {
                var (op, ours) = kv.Key; var a = kv.Value;
                string em = Nx.Emitter[op];
                bool unk = ours == ".UNKNOWN";
                string st = unk ? (em == null ? "both-unknown" : "ours-unknown") : em == null ? "nx-unsupported" : Names.Match(ours, em) ? "agree" : "name-differs";
                if (st == "agree") continue;
                fs.Add(new Finding
                {
                    Key = $"L1|{op:x3}|{ours}|{st}", Level = "L1-opcode", Kind = st, Op12 = op, Ours = ours, NxEmitter = em ?? "-",
                    Detail = $"nx InstName {Nx.Name[op] ?? "-"}", N = a.N, Hash = a.First.Hash, Lo = a.First.Lo, Hi = a.First.Hi,
                });
            }
            foreach (var kv in d.Pred.OrderBy(k => k.Key.op12).ThenBy(k => k.Key.kind, StringComparer.Ordinal))
                fs.Add(new Finding
                {
                    Key = $"L1|{kv.Key.op12:x3}|{kv.Key.kind}", Level = "L1-predicate", Kind = kv.Key.kind, Op12 = kv.Key.op12, NxEmitter = Nx.Emitter[kv.Key.op12],
                    N = kv.Value.N, Hash = kv.Value.First.Hash, Lo = kv.Value.First.Lo, Hi = kv.Value.First.Hi,
                });

            // ---- level 2: bits ----
            foreach (var kv in cells.OrderBy(k => k.Key.op12).ThenBy(k => k.Key.bit))
            {
                var c = kv.Value;
                int op = kv.Key.op12, bit = kv.Key.bit;
                foreach (var (kind, list, cnt) in new[] { ("ours-only", c.OursOnlyEx, c.OursOnly), ("nx-only", c.NxOnlyEx, c.NxOnly) })
                {
                    if (list.Count == 0) continue;
                    var cs = list[0];
                    ulong lo2 = cs.E.Lo, hi2 = cs.E.Hi;
                    if (bit < 64) lo2 ^= 1UL << bit; else hi2 ^= 1UL << (bit - 64);
                    var n = new long[3];
                    for (int i = 0; i < 3; i++) n[i] = d.BitCount[i][op * 128 + bit];
                    long setAll = n.Sum();
                    fs.Add(new Finding
                    {
                        Key = $"L2|{op:x3}|{bit}|{kind}", Level = "L2-bit", Kind = kind, Op12 = op, Bit = bit, Ours = cs.OursBase.Split(' ')[0].Split('|')[0], NxEmitter = Nx.Emitter[op],
                        Detail = $"exemplars {c.Exemplars}, ours-sensitive {c.OursSens}, nx-sensitive {c.NxSens}, {kind} in {cnt}; ours {Clip(cs.OursBase)} => {Clip(cs.OursFlip)}",
                        N = n, Total = opTotal[op], Hash = cs.E.Hash, Lo = cs.E.Lo, Hi = cs.E.Hi, Lo2 = lo2, Hi2 = hi2,
                        OursText = Clip(cs.OursFlip), NxText = Clip(cs.NxBase) + " => " + Clip(cs.NxFlip),
                    });
                }
            }

            // ---- level 0: every exemplar word against nvdisasm ----
            var baseWords = exs.Select(x => (x.e.Lo, x.e.Hi)).ToList();
            if (nvdisasm)
            {
                Console.WriteLine($"nvdisasm: {baseWords.Count} exemplar words");
                var nvb = Nvdis.Run(baseWords, jobs);
                var bad = new Dictionary<(int, string), (int n, Finding f)>();
                int mism = 0;
                for (int i = 0; i < baseWords.Count; i++)
                {
                    var (lo, hi) = baseWords[i];
                    string o = Text(lo, hi, i * 16);
                    if (o == nvb[i]) continue;
                    mism++;
                    var key = ((int)(lo & 0xfff), o.Split(' ')[0]);
                    var e = exs[i].e;
                    if (bad.TryGetValue(key, out var b)) bad[key] = (b.n + 1, b.f);
                    else bad[key] = (1, new Finding
                    {
                        Key = $"L0|{key.Item1:x3}|{key.Item2}", Level = "L0-text", Kind = "text-differs", Op12 = key.Item1, Ours = key.Item2, NxEmitter = Nx.Emitter[key.Item1] ?? "-",
                        Hash = e.Hash, Lo = lo, Hi = hi, Nv = nvb[i], OursText = o, Auto = "OURS:text differs from nvdisasm on a corpus word",
                    });
                }
                foreach (var kv in bad.OrderBy(k => k.Key.Item1))
                {
                    kv.Value.f.Detail = $"{kv.Value.n} of the exemplar words";
                    kv.Value.f.N[0] = kv.Value.n;
                    fs.Add(kv.Value.f);
                }
                Console.WriteLine($"level 0: {baseWords.Count} exemplar words, {mism} differ from nvdisasm, {bad.Count} (opcode, mnemonic) rows");
            }

            // ---- nvdisasm adjudication ----
            if (nvdisasm)
            {
                var wa = new List<(ulong, ulong)>(); var wb = new List<(ulong, ulong)>();
                foreach (var f in fs.Where(f => f.Level != "L0-text"))
                {
                    wa.Add((f.Lo, f.Hi));
                    wb.Add(f.Level == "L2-bit" ? (f.Lo2, f.Hi2) : (f.Lo, f.Hi));
                }
                Console.WriteLine($"nvdisasm: {wa.Count} words (+{wa.Count} twins)");
                var ra = Nvdis.Run(wa, jobs);
                var rb = Nvdis.Run(wb, jobs);
                var adj = fs.Where(f => f.Level != "L0-text").ToList();
                for (int i = 0; i < adj.Count; i++)
                {
                    var f = adj[i];
                    f.Nv = ra[i]; f.NvFlip = rb[i];
                    string ob = Text(f.Lo, f.Hi, i * 16), of = f.Level == "L2-bit" ? Text(f.Lo2, f.Hi2, i * 16) : ob;
                    f.OursText = f.Level == "L2-bit" ? ob + "  =>  " + of : ob;
                    f.Auto = Adjudicate(f, ob, of);
                }
            }

            // ---- manual verdicts ----
            var manual = new Dictionary<string, (string st, string note)>();
            string td = ToolDir();
            if (td.Length > 0)
                foreach (var l in File.ReadLines(Path.Combine(td, "verdicts.tsv")))
                {
                    if (l.Length == 0 || l[0] == '#') continue;
                    var p = l.Split('\t');
                    if (p.Length >= 3) manual[p[0]] = (p[1], p[2]);
                }
            foreach (var f in fs)
            {
                if (manual.TryGetValue(f.Key, out var m)) { f.Status = m.st; f.Note = m.note; }
                else if (f.Auto.StartsWith("justified")) { f.Status = "justified"; f.Note = f.Auto.Substring(f.Auto.IndexOf(':') + 1); }
                else f.Status = "open";
            }

            var nl = "\n";
            using (var w = new StreamWriter(Path.Combine(dir, "disagreements.tsv"), false, new UTF8Encoding(false)) { NewLine = nl })
            {
                w.WriteLine("key\tlevel\tkind\top12\tbit\tours\tnx_emitter\tn_a\tn_b\tn_c\texample_hash\texample_word\ttwin_word\tnvdisasm\tnvdisasm_twin\tours_text\tnx_detail\tauto\tstatus\tnote");
                foreach (var f in fs)
                    w.WriteLine(string.Join("\t", f.Key, f.Level, f.Kind, f.Op12.ToString("x3"), f.Bit < 0 ? "" : f.Bit.ToString(), f.Ours, f.NxEmitter, f.N[0], f.N[1], f.N[2], f.Hash, W(f.Lo, f.Hi),
                        f.Level == "L2-bit" ? W(f.Lo2, f.Hi2) : "", f.Nv, f.Level == "L2-bit" ? f.NvFlip : "", f.OursText, f.Level == "L2-bit" ? f.NxText : f.Detail, f.Auto, f.Status, f.Note));
            }
            var sb = new StringBuilder();
            var s = new StringWriter(sb) { NewLine = nl };
            s.WriteLine($"instructions: a {d.Instructions[0]}  b {d.Instructions[1]}  c {d.Instructions[2]}   blocks: {d.Blocks[0]} / {d.Blocks[1]} / {d.Blocks[2]}");
            int forms = d.L1.Count, agree = forms - fs.Count(f => f.Level == "L1-opcode");
            s.WriteLine($"L1 opcode rows (opcode x our mnemonic): {forms}, agreeing {agree}");
            s.WriteLine($"level 2: {cells.Count} (opcode, bit) cells compared over {d.Forms.Count} forms");
            s.WriteLine("disagreements by level/kind/status:");
            foreach (var g in fs.GroupBy(f => (f.Level, f.Kind, f.Status)).OrderBy(g => g.Key))
                s.WriteLine($"  {g.Key.Level,-13} {g.Key.Kind,-16} {g.Key.Status,-10} {g.Count(),5} findings, {g.Sum(f => f.N.Sum())} instructions/set-bits");
            s.WriteLine($"total findings {fs.Count}: open {fs.Count(f => f.Status == "open")}, justified {fs.Count(f => f.Status == "justified")}, fixed {fs.Count(f => f.Status == "fixed")}");
            File.WriteAllText(Path.Combine(dir, "summary.txt"), sb.ToString());
            Console.Write(sb.ToString());
        }

        /// --words <file>: lines "lo hi" (hex, 0x optional); prints the words whose text differs between our decoder and nvdisasm (word i sits at address i*16).
        public static int CheckWords(string path, int jobs)
        {
            var w = new List<(ulong, ulong)>();
            foreach (var l in File.ReadLines(path))
            {
                var p = l.Split(new[] { ' ', '	' }, StringSplitOptions.RemoveEmptyEntries);
                if (p.Length < 2 || p[0][0] == '#') continue;
                w.Add((Convert.ToUInt64(p[0].Replace("0x", ""), 16), Convert.ToUInt64(p[1].Replace("0x", ""), 16)));
            }
            var nv = Nvdis.Run(w, jobs);
            int bad = 0, illegal = 0;
            for (int i = 0; i < w.Count; i++)
            {
                if (nv[i] == Nvdis.Illegal) { illegal++; continue; }
                string o = Text(w[i].Item1, w[i].Item2, i * 16);
                if (o != nv[i]) { bad++; Console.WriteLine($"0x{w[i].Item1:x16} 0x{w[i].Item2:x16}"); Console.WriteLine("  ours " + o); Console.WriteLine("  nv   " + nv[i]); }
            }
            Console.WriteLine($"{w.Count} words, {illegal} rejected by nvdisasm, {bad} differ");
            return bad == 0 ? 0 : 1;
        }

        /// The automatic verdict: "justified:<why>" when nvdisasm backs our decoder, "OURS:<what>" when it does not, "review" otherwise.
        static string Adjudicate(Finding f, string ours, string oursFlip)
        {
            string nv = f.Nv, nvf = f.NvFlip;
            if (f.Level == "L2-bit")
            {
                if (!f.Live) return "justified:unexercised, the bit has one value in every corpus instruction of this opcode";
                if (nvf == Nvdis.Illegal) return "justified:nvdisasm rejects the flipped word (reserved value)";
                if (nv == Nvdis.Illegal) return "review";
                bool changed = nv != nvf;
                if (f.Kind == "ours-only")
                {
                    if (oursFlip != nvf) return "OURS:text differs from nvdisasm on the flipped word";
                    if (ours != nv) return "OURS:text differs from nvdisasm on the base word";
                    return changed ? "justified:nvdisasm prints the bit, nxnative's emitter ignores it"
                                   : "review";
                }
                // nx-only
                if (!changed) return "justified:nvdisasm ignores the bit (nxnative reads it, a reserved or don't-care field)";
                return "OURS:nvdisasm changes with the bit, our text does not";
            }
            if (nv == Nvdis.Illegal)
                return f.Kind is "ours-unknown" or "both-unknown" ? "justified:nvdisasm rejects the word (illegal)" : "review";
            switch (f.Kind)
            {
                case "nx-unsupported": return ours == nv ? "justified:no emitter in nxnative; our text equals nvdisasm" : "OURS:text differs from nvdisasm";
                case "ours-unknown": case "both-unknown": return "OURS:nvdisasm decodes the word, we do not";
                case "name-differs": return ours == nv ? "review" : "OURS:text differs from nvdisasm";
                default: return "review";   // predicate mismatches
            }
        }
    }
}
