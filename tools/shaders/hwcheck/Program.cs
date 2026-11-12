// SPDX-License-Identifier: MIT
// hwcheck: per-instruction hardware oracle for the Ampere front end. See README.md.
//
//   hwcheck [--corpus <oracle dir> | --no-corpus] [--out <dir>] [--per-shape 2] [--threads 1024] [--only <regex>] [--seed 1]
//
// For every test instruction: run it on the GPU (Cuda.cs: patched cubin) and its translation (spirv_harness --hwcheck, then
// Vk.cs) on the same register-file inputs, compare (Compare.cs) and report per opcode/modifier. Exit 0 = no mismatch.
using Nx.Shaders.Ampere;
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.RegularExpressions;

namespace Hwcheck
{
    static class Program
    {
        static string Opt(string[] a, string n, string d) { int i = Array.IndexOf(a, n); return i >= 0 && i + 1 < a.Length ? a[i + 1] : d; }

        static int Main(string[] args)
        {
            try { Process.GetCurrentProcess().PriorityClass = ProcessPriorityClass.BelowNormal; } catch { }
            if (args.Length == 4 && args[0] == "--cuda-worker") return CudaWorker(args[1], int.Parse(args[2]), int.Parse(args[3]));
            string bin = AppContext.BaseDirectory;
            string shaders = Path.GetFullPath(Path.Combine(bin, "..", "..", "..", ".."));
            string outDir = Path.GetFullPath(Opt(args, "--out", Path.Combine(shaders, "build", "hwcheck")));
            int perShape = int.Parse(Opt(args, "--per-shape", "2"));
            int threads = int.Parse(Opt(args, "--threads", "1024"));
            int seed = int.Parse(Opt(args, "--seed", "1"));
            string only = Opt(args, "--only", null);
            string corpus = args.Contains("--no-corpus") ? null : Opt(args, "--corpus", null) ?? FindCorpus(bin);
            Directory.CreateDirectory(outDir);
            if (args.Contains("--coopmat")) return CoopMat.Run(args, bin, outDir);
            if (args.Length > 0 && args[0] == "--pipeline-stats") return CoopMat.Stats(args);   // driver statistics of compute modules (CoopMat.cs)   // cooperative-matrix HMMA order probe (CoopMat.cs)
            var sw = Stopwatch.StartNew();

            // ---- test instructions
            var raw = new List<(ulong, ulong, string, string)>();
            byte[] opsCubin = Compile(Path.Combine(bin, "ops.ptx"), Path.Combine(outDir, "ops.cubin"));
            string opsSass = Tools.Run(Tools.Cuda("cuobjdump"), $"-sass \"{Path.Combine(outDir, "ops.cubin")}\"");
            raw.AddRange(Cases.FromCuobjdump(opsSass).Select(x => (x.Item1, x.Item2, x.Item3, "ops")));
            int opsCount = raw.Count;
            if (corpus != null && Directory.Exists(corpus))
                raw.AddRange(Cases.FromCorpus(corpus, Path.Combine(outDir, "corpus_samples.tsv"), 64).Select(x => (x.Item1, x.Item2, x.Item3, "corpus")));
            else corpus = null;
            int fromOracle = raw.Count - opsCount;
            string game = Opt(args, "--game-corpus", null);   // the game's shader corpus itself (Cases.FromGameCorpus)
            if (game != null)
                raw.AddRange(Cases.FromGameCorpus(game, only, perShape).Select(x => (x.Item1, x.Item2, x.Item3, "game")));
            Console.WriteLine($"sources: {opsCount} words from ops.ptx, {fromOracle} from {(corpus ?? "no corpus")}" +
                              (game != null ? $", {raw.Count - opsCount - fromOracle} from {game}" : ""));

            var cases = new List<Case>();
            var perShapeCount = new Dictionary<string, int>();
            var seenWords = new HashSet<(ulong, ulong)>();
            var refused = new SortedDictionary<string, string>();
            foreach (var (lo, hi, text, src) in raw)
            {
                var words = Cases.Rewrite(lo, hi, out SassInst inst, out string why);
                if (words == null)
                {
                    string tok = SassDecoder.Decode(lo, hi, 0) is var d && Cases.InScope.Contains(d.Opcode) ? d.Opcode + (d.Mods.Count > 0 ? "." + string.Join('.', d.Mods) : "") : null;
                    if (tok != null) refused.TryAdd(tok, $"{why} ({text})");
                    continue;
                }
                MemSpec mem = null;
                if (MemSpec.Opcodes.Contains(inst.Opcode) && (mem = MemSpec.Of(inst, out why)) == null)
                {
                    refused.TryAdd(inst.Opcode + (inst.Mods.Count > 0 ? "." + string.Join('.', inst.Mods) : ""), $"{why} ({text})");
                    continue;
                }
                if (!seenWords.Add(words[0])) continue;
                string shape = Cases.Shape(inst);
                if (only != null && !Regex.IsMatch(shape, only)) continue;
                perShapeCount.TryGetValue(shape, out int n);
                if (n >= perShape) continue;
                perShapeCount[shape] = n + 1;
                cases.Add(new Case
                {
                    Id = $"c{cases.Count:d4}", Source = src, Key = inst.Opcode + (inst.Mods.Count > 0 ? "." + string.Join('.', inst.Mods) : ""),
                    Text = inst.ToSassText(), Inst = inst, Words = words, Mem = mem,
                });
            }
            if (args.Contains("--sin-pairs")) cases = SinPairs(cases);   // FMUL.RZ x, 1/(2pi) then MUFU.SIN/COS of it, as one case (AmpereFuse)
            Console.WriteLine($"{cases.Count} test instructions, {perShapeCount.Count} shapes");

            // ---- nvdisasm agrees with our decoder on every rewritten word
            Verify(cases, outDir);
            VerifyKernel(outDir, cases.FirstOrDefault(c => c.Status == "")?.Words ?? new List<(ulong, ulong)>());

            // ---- translate
            var run = cases.Where(c => c.Status == "").ToList();
            string casesTsv = Path.Combine(outDir, "cases.tsv");
            File.WriteAllLines(casesTsv, run.Select(c => c.Id + "\t" + string.Join(' ', c.Words.SelectMany(w => new[] { w.lo.ToString("x16"), w.hi.ToString("x16") })) + (c.Mem != null ? "\t" + c.Mem.Tsv : "")));
            string spvDir = Path.Combine(outDir, "spv");
            if (Directory.Exists(spvDir)) Directory.Delete(spvDir, true);
            Console.Write(Tools.Run("dotnet", $"\"{Path.Combine(bin, "spirv_harness.dll")}\" --hwcheck \"{casesTsv}\" --out \"{spvDir}\" --local-x 32"));
            var byId = run.ToDictionary(c => c.Id);
            foreach (string line in File.ReadAllLines(Path.Combine(spvDir, "hwcheck.tsv")))
            {
                string[] f = line.Split('\t');
                if (f[1] != "ok") { byId[f[0]].Status = f[1]; byId[f[0]].Detail = f.Length > 3 ? f[3] : ""; }
            }

            // ---- run both sides: the GPU side in worker processes (a faulting launch poisons the CUDA context of its process)
            Cubin template = new(Compile(Path.Combine(bin, "template.ptx"), Path.Combine(outDir, "template.cubin")));
            var todo = run.Where(c => c.Status == "").ToList();
            string cudaDir = Path.Combine(outDir, "cuda");
            if (Directory.Exists(cudaDir)) Directory.Delete(cudaDir, true);
            Directory.CreateDirectory(cudaDir);
            var inputs = new Dictionary<string, uint[]>();
            foreach (Case c in todo)
            {
                c.Threads = threads;
                inputs[c.Id] = Inputs.Make(threads, seed * 7919 + StableHash(c.Text));
                c.Mem?.Apply(inputs[c.Id], threads, seed * 7919 + StableHash(c.Text));
                var kernel = Sass.Kernel(c.Words, c.Mem, out int exitOff);
                File.WriteAllBytes(Path.Combine(cudaDir, c.Id + ".cubin"), template.Build(kernel, exitOff));
                File.WriteAllBytes(Path.Combine(cudaDir, c.Id + ".in"), System.Runtime.InteropServices.MemoryMarshal.AsBytes(inputs[c.Id].AsSpan()).ToArray());
            }
            File.WriteAllLines(Path.Combine(cudaDir, "list.txt"), todo.Select(c => c.Id));
            for (int next = 0; next < todo.Count;)
            {
                string o = Tools.Run("dotnet", $"\"{Path.Combine(bin, "hwcheck.dll")}\" --cuda-worker \"{cudaDir}\" {next} {threads}", check: false);
                if (next == 0) Console.Write(o);
                int k = next;
                while (k < todo.Count && (File.Exists(Path.Combine(cudaDir, todo[k].Id + ".out")) || File.Exists(Path.Combine(cudaDir, todo[k].Id + ".err")))) k++;
                if (k < todo.Count && k == next) File.WriteAllText(Path.Combine(cudaDir, todo[k].Id + ".err"), "worker died: " + o.Trim()); // no progress
                next = k;
            }
            using var vk = new VkRunner(threads * Inputs.Words * 4);
            Console.WriteLine($"Vulkan: {vk.DeviceName} ({vk.Float16Info})");
            foreach (Case c in todo)
            {
                string errPath = Path.Combine(cudaDir, c.Id + ".err");
                if (File.Exists(errPath)) { c.Status = "hw-error"; c.Detail = File.ReadAllText(errPath); continue; }
                uint[] hw = System.Runtime.InteropServices.MemoryMarshal.Cast<byte, uint>(File.ReadAllBytes(Path.Combine(cudaDir, c.Id + ".out"))).ToArray();
                uint[] ours = (uint[])inputs[c.Id].Clone();
                string e = vk.Run(File.ReadAllBytes(Path.Combine(spvDir, c.Id + ".spv")), ours, threads / 32);
                if (e != null) { c.Status = "vk-error"; c.Detail = e; continue; }
                CompareCase(c, inputs[c.Id], hw, ours);
            }

            Report(cases, refused, outDir, corpus, sw.Elapsed);
            return cases.Any(c => c.Status is "mismatch" or "hw-error" or "vk-error" or "decoder-disagrees" or "exception") ? 1 : 0;
        }

        /// GPU side of the cases in <dir>/list.txt from index `start`: <id>.cubin + <id>.in -> <id>.out, or <id>.err and exit 3.
        static int CudaWorker(string dir, int start, int threads)
        {
            string[] ids = File.ReadAllLines(Path.Combine(dir, "list.txt"));
            using var cuda = new CudaRunner(threads * Inputs.Words * 4);
            if (start == 0) Console.WriteLine($"CUDA: {cuda.DeviceName}");
            for (int k = start; k < ids.Length; k++)
            {
                uint[] data = System.Runtime.InteropServices.MemoryMarshal.Cast<byte, uint>(File.ReadAllBytes(Path.Combine(dir, ids[k] + ".in"))).ToArray();
                string e = cuda.Run(File.ReadAllBytes(Path.Combine(dir, ids[k] + ".cubin")), data, threads);
                if (e != null) { File.WriteAllText(Path.Combine(dir, ids[k] + ".err"), e); return 3; }
                File.WriteAllBytes(Path.Combine(dir, ids[k] + ".out"), System.Runtime.InteropServices.MemoryMarshal.AsBytes(data.AsSpan()).ToArray());
            }
            return 0;
        }

        static string FindCorpus(string from)
        {
            for (DirectoryInfo d = new(from); d != null; d = d.Parent)
            {
                string p = Path.Combine(d.FullName, "work", "shaders", "oracle", "SM86");
                if (Directory.Exists(p)) return p;
            }
            return null;
        }

        static int StableHash(string s) { unchecked { int h = (int)2166136261; foreach (char ch in s) h = (h ^ ch) * 16777619; return h & 0x7fffffff; } }

        static byte[] Compile(string ptx, string cubin)
        {
            if (!File.Exists(cubin) || File.GetLastWriteTimeUtc(cubin) < File.GetLastWriteTimeUtc(ptx))
                Tools.Run(Tools.Cuda("ptxas"), $"-arch=sm_86 -o \"{cubin}\" \"{ptx}\"");
            return File.ReadAllBytes(cubin);
        }

        static readonly Regex DisLine = new(@"^\s*/\*([0-9a-f]{4,})\*/\s*(.*?)\s*;\s*/\*\s*(0x[0-9a-f]{16})\s*\*/\s*$");

        static string Norm(string s) => Regex.Replace(s.Trim().TrimEnd(';').Trim(), @"\s+", " ");

        /// Two-instruction cases "FMUL.FTZ.RZ R1, R0, 1/(2pi); MUFU.SIN|COS R2, R1" from the encodings found (needs --game-corpus with an
        /// --only that keeps FMUL and MUFU): the translator emits the MUFU as sin(R0) / cos(R0) (AmpereFuse); compared with the hardware pair.
        static List<Case> SinPairs(List<Case> cases)
        {
            static ulong SetField(ulong w, int bit, int v) => (w & ~(0xffUL << bit)) | ((ulong)(uint)v << bit);
            var fm = cases.OrderBy(c => c.Inst.Mods.Contains("FTZ") ? 0 : 1).FirstOrDefault(c => c.Inst.Opcode == "FMUL" && c.Inst.Mods.Contains("RZ") && c.Inst.Operands.Any(o => o.Kind == OperandKind.FImm && (o.ImmBits & 0xffffffff) == 0x3e22f983));
            var res = new List<Case>();
            if (fm == null) { Console.WriteLine("sin pairs: no FMUL.RZ by 1/(2pi) found"); return res; }
            var (flo, fhi) = fm.Words[0];
            flo = SetField(SetField(flo, 16, 1), 24, 0);
            foreach (var mu in cases.Where(c => c.Inst.Opcode == "MUFU" && (c.Inst.Mods.Contains("SIN") || c.Inst.Mods.Contains("COS")) && c.Inst.Mods.Count == 1))
            {
                var (mlo, mhi) = mu.Words[0];
                mlo = SetField(SetField(mlo, 16, 2), 32, 1);
                var a = SassDecoder.Decode(flo, fhi, 0); var b = SassDecoder.Decode(mlo, mhi, 0x10);
                res.Add(new Case { Id = $"c{res.Count:d4}", Source = "pair", Key = "SINPAIR." + string.Join('.', b.Mods), Text = a.ToSassText(), Detail = b.ToSassText(),
                                   Inst = b, Words = new List<(ulong, ulong)> { (flo, fhi), (mlo, mhi) } });
                Console.WriteLine($"sin pair: {a.ToSassText()} {b.ToSassText()}");
            }
            return res;
        }

        /// nvdisasm text of raw words, by instruction index (null = refused).
        static string[] Disasm(IReadOnlyList<(ulong lo, ulong hi)> words, string dir)
        {
            string p = Path.Combine(dir, "dis.bin");
            using (var f = new BinaryWriter(File.Create(p))) foreach (var (lo, hi) in words) { f.Write(lo); f.Write(hi); }
            string o = Tools.Run(Tools.Cuda("nvdisasm"), $"-b SM86 -hex \"{p}\"", check: false);
            var r = new string[words.Count];
            foreach (string line in o.Split('\n'))
            {
                Match m = DisLine.Match(line);
                if (m.Success) { int idx = Convert.ToInt32(m.Groups[1].Value, 16) / 16; if (idx < r.Length) r[idx] = Norm(m.Groups[2].Value); }
            }
            return r;
        }

        static void Verify(List<Case> cases, string dir)
        {
            var words = cases.Select(c => c.Words[0]).ToList();
            string[] t = Disasm(words, dir);
            for (int k = 0; k < cases.Count; k++)
            {
                string nv = t[k] ?? Disasm(new[] { words[k] }, dir)[0];
                string ours = Norm(cases[k].Text);
                if (nv == null) { cases[k].Status = "nvdisasm-refused"; cases[k].Detail = ours; }
                else if (nv != ours) { cases[k].Status = "decoder-disagrees"; cases[k].Detail = $"nvdisasm: {nv} | decoder: {ours}"; }
            }
        }

        static void VerifyKernel(string dir, List<(ulong, ulong)> sample)
        {
            var k = Sass.Kernel(sample, null, out _);
            string[] t = Disasm(k, dir);
            string[] want = { "S2R R16, SR_TID.X", "IMAD.WIDE.U32 R18, R16, R17, c[0x0][0x160]", "LDG.E.128 R12, [R18.64+0x30]", "R2P PR, R20, 0x7f",
                              "P2R R20, PR, RZ, 0x7f", "STG.E [R18.64+0x40], R20", "EXIT" };
            foreach (string w in want)
                if (!t.Any(x => x == w)) throw new Exception($"kernel text check failed: '{w}' not found in nvdisasm output:\n{string.Join('\n', t)}");
        }

        static void CompareCase(Case c, uint[] input, uint[] hw, uint[] ours)
        {
            Lane lane = Compare.LaneOf(c.Inst);
            var tol = Compare.Tolerance(c.Inst);
            int bad = 0;
            for (int t = 0; t < c.Threads; t++)
            {
                int b = t * Inputs.Words;
                var diff = new List<int>();
                for (int k = 0; k < 16; k++)
                {
                    if (hw[b + k] == ours[b + k]) continue;
                    bool same;
                    bool hmmaF32 = c.Inst.Opcode == "HMMA" && c.Inst.Mods.Contains("F32");
                    int hmmaDst = c.Inst.Opcode == "HMMA" ? c.Inst.Operands[0].Reg : -1;
                    if (hmmaDst >= 0 && hmmaDst < 255 && k >= hmmaDst && k < hmmaDst + (hmmaF32 ? 4 : 2))
                    {
                        // tensor-core rounding: element-wise bound from the warp's inputs (Compare.HmmaClose)
                        int q = k - hmmaDst, w0 = t - t % 32;
                        uint Rec(int l, int r) => input[(w0 + l) * Inputs.Words + r];
                        same = true;
                        for (int e = 0; e < (hmmaF32 ? 1 : 2); e++)
                        {
                            int rh = hmmaF32 ? q >> 1 : q, nn = hmmaF32 ? q & 1 : e;
                            var (bound, special) = Compare.HmmaInputs(c.Inst, Rec, t % 32, rh, nn);
                            uint hv = hmmaF32 ? hw[b + k] : (hw[b + k] >> (16 * e)) & 0xffff, ov = hmmaF32 ? ours[b + k] : (ours[b + k] >> (16 * e)) & 0xffff;
                            same &= Compare.HmmaClose(hv, ov, hmmaF32, bound, special);
                        }
                    }
                    else if (lane == Lane.F64)
                    {
                        int e = k & ~1;
                        same = Compare.Same64((ulong)hw[b + e + 1] << 32 | hw[b + e], (ulong)ours[b + e + 1] << 32 | ours[b + e]);
                    }
                    else same = Compare.Same(hw[b + k], ours[b + k], lane, tol);
                    if (!same) diff.Add(k);
                }
                if (hw[b + Inputs.PredWord] != ours[b + Inputs.PredWord]) diff.Add(Inputs.PredWord);
                if (c.Mem != null)   // the memory window, bit-exact
                    for (int k = MemSpec.Word; k < MemSpec.Word + MemSpec.Words; k++) if (hw[b + k] != ours[b + k]) diff.Add(k);
                if (diff.Count == 0) continue;
                if (bad++ == 0)
                {
                    var used = c.Inst.Operands.Where(o => o.Kind == OperandKind.Reg && o.Reg < 16).Select(o => o.Reg)
                        .SelectMany(r => new[] { r, r + 1 }).Where(r => r < 16).Distinct().OrderBy(r => r);
                    var sb = new StringBuilder();
                    sb.Append($"thread {t} (lane {t % 32}); in: ");
                    sb.Append(string.Join(' ', used.Select(r => $"R{r}={input[b + r]:x8}"))).Append($" P={input[b + Inputs.PredWord]:x2}; ");
                    sb.Append(string.Join("; ", diff.Select(k => k == Inputs.PredWord ? $"P hw={hw[b + k]:x2} ours={ours[b + k]:x2}" : k >= MemSpec.Word ? $"M{k - MemSpec.Word} hw={hw[b + k]:x8} ours={ours[b + k]:x8}" : $"R{k} hw={hw[b + k]:x8} ours={ours[b + k]:x8}")));
                    c.Example = sb.ToString();
                }
            }
            c.Mismatches = bad;
            c.Status = bad == 0 ? "pass" : "mismatch";
        }

        static void Report(List<Case> cases, SortedDictionary<string, string> refused, string dir, string corpus, TimeSpan took)
        {
            var sb = new StringBuilder();
            sb.AppendLine($"hwcheck report  ({cases.Count} instructions, corpus: {corpus ?? "none"}, {took.TotalSeconds:F0} s)");
            foreach (var g in cases.GroupBy(c => c.Status).OrderBy(g => g.Key)) sb.AppendLine($"  {g.Key,-18} {g.Count()}");
            sb.AppendLine();
            sb.AppendLine("By opcode and modifiers (pass/total, other states):");
            foreach (var g in cases.GroupBy(c => c.Key).OrderBy(g => g.Key, StringComparer.Ordinal))
            {
                int pass = g.Count(c => c.Status == "pass");
                string other = string.Join(", ", g.Where(c => c.Status != "pass").GroupBy(c => c.Status).Select(x => $"{x.Key} {x.Count()}"));
                sb.AppendLine($"  {g.Key,-34} {pass}/{g.Count()} {other}");
            }
            sb.AppendLine();
            foreach (string st in new[] { "mismatch", "hw-error", "vk-error", "unsupported", "exception", "decoder-disagrees", "nvdisasm-refused" })
            {
                var list = cases.Where(c => c.Status == st).ToList();
                if (list.Count == 0) continue;
                sb.AppendLine($"== {st} ({list.Count})");
                foreach (Case c in list)
                {
                    sb.AppendLine($"  {c.Id} [{c.Source}] {c.Text}   {string.Join(' ', c.Words.Select(w => $"{w.lo:x16}:{w.hi:x16}"))}");
                    if (st == "mismatch") sb.AppendLine($"      {c.Mismatches}/{c.Threads} threads; e.g. {c.Example}");
                    else if (c.Detail != "") sb.AppendLine($"      {c.Detail}");
                }
            }
            var covered = cases.Where(c => c.Status is "pass" or "mismatch").Select(c => c.Inst.Opcode).ToHashSet();
            sb.AppendLine();
            sb.AppendLine("In-scope opcodes without a compared instruction: " + string.Join(' ', Cases.InScope.Where(o => !covered.Contains(o)).OrderBy(o => o)));
            if (refused.Count > 0)
            {
                sb.AppendLine("Forms that could not be moved into the register window (first reason):");
                foreach (var kv in refused) sb.AppendLine($"  {kv.Key,-34} {kv.Value}");
            }
            File.WriteAllText(Path.Combine(dir, "report.txt"), sb.ToString());
            File.WriteAllLines(Path.Combine(dir, "results.tsv"), new[] { "id\tsource\tkey\tstatus\tmismatched_threads\ttext\twords\tdetail" }
                .Concat(cases.Select(c => $"{c.Id}\t{c.Source}\t{c.Key}\t{c.Status}\t{c.Mismatches}\t{c.Text}\t{string.Join(' ', c.Words.Select(w => $"{w.lo:x16}:{w.hi:x16}"))}\t{(c.Status == "mismatch" ? c.Example : c.Detail)}")));
            Console.WriteLine();
            foreach (var g in cases.GroupBy(c => c.Status).OrderBy(g => g.Key)) Console.WriteLine($"  {g.Key,-18} {g.Count()}");
            foreach (var g in cases.Where(c => c.Status == "mismatch").GroupBy(c => c.Key).OrderBy(g => g.Key, StringComparer.Ordinal))
                Console.WriteLine($"  MISMATCH {g.Key}: {g.Count()} instruction(s), e.g. {g.First().Text}  {g.First().Example}");
            Console.WriteLine($"report: {Path.Combine(dir, "report.txt")}");
        }
    }
}
