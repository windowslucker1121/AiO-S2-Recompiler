// SPDX-License-Identifier: MIT
// sassdecode: decode Ampere SM86 code blocks with SassDecoder.
//   --verify  <oracle dir> <corpus dir> [--every N] [--show OPC] [--examples K] [--jobs 4]
//             decodes every instruction of every block listed in <oracle>/index.tsv from the corpus code and compares
//             ToSassText() with the nvdisasm oracle line (whitespace normalised only). Prints match %, weighted by occurrence,
//             per-mnemonic gaps and example mismatches.
//   --corpus  <corpus dir> [--jobs 4]   decode the whole corpus (every unique block's program range), counts only.
//   --dump    <code.bin> <ctrl.bin>      print one block.
//   --census  <corpus dir> --out <census.tsv> [--blocks <blocks.tsv>] [--examples K] [--all-blocks] [--jobs 4]
//             instruction census of a corpus (title A layout: unique.tsv + code/ + ctrl/; runtime dump layout: manifest.tsv + code_<h>.bin
//             + ctrl_<h>.bin, duplicate rows folded): one row per opcode + modifier combination with its count, the number of distinct
//             blocks containing it, blocks per stage, example block hashes (K smallest, default 5; --all-blocks lists every one),
//             a decode class (ok | partial = a modifier or form the decoder flagged as unknown | undecoded = unknown opcode) and, for
//             undecoded and partial rows, the instruction word of the smallest-hash block that holds it.
using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using System.Threading.Tasks;
using Nx.Shaders.Ampere;

static class Program
{
    static readonly Regex Ws = new(@"\s+", RegexOptions.Compiled);
    static readonly Regex Line = new(@"^/\*([0-9a-f]+)\*/ (.*) ; (0x[0-9a-f]{16}) (0x[0-9a-f]{16})$", RegexOptions.Compiled);

    static int Main(string[] args)
    {
        try { Process.GetCurrentProcess().PriorityClass = ProcessPriorityClass.BelowNormal; } catch { }
        if (args.Length >= 3 && args[0] == "--verify") return Verify(args[1], args[2], args.Skip(3).ToArray());
        if (args.Length >= 2 && args[0] == "--corpus") return WholeCorpus(args[1], args.Skip(2).ToArray());
        if (args.Length >= 3 && args[0] == "--dump") return Dump(args[1], args[2]);
        if (args.Length >= 4 && args[0] == "--census") return Census(args[1], args.Skip(2).ToArray());
        Console.Error.WriteLine("usage: sassdecode --verify <oracle dir> <corpus dir> [--every N] [--show OPC] [--examples K] [--jobs N]\n" +
                                "       sassdecode --census <corpus dir> --out <census.tsv> [--blocks <blocks.tsv>] [--examples K] [--all-blocks] [--jobs N]\n" +
                                "       sassdecode --corpus <corpus dir> [--jobs N]\n       sassdecode --dump <code.bin> <ctrl.bin>");
        return 2;
    }

    static string Opt(string[] a, string name, string def)
    {
        int i = Array.IndexOf(a, name);
        return i >= 0 && i + 1 < a.Length ? a[i + 1] : def;
    }

    /// program range per docs/AMPERE.md: stage u16 ctrl+0x56 (5 = compute, no SPH), end u32 ctrl+0x38, trailing zero words dropped.
    static (int start, int end) Program_(byte[] code, byte[] ctrl)
    {
        int stage = BitConverter.ToUInt16(ctrl, 0x56);
        int end = Math.Min((int)BitConverter.ToUInt32(ctrl, 0x38), code.Length);
        int start = stage == 5 ? 0 : 0x80;
        while (end - 16 >= start && code.AsSpan(end - 16, 16).IndexOfAnyExcept((byte)0) < 0) end -= 16;
        return (start, end);
    }

    static string Norm(string s) => Ws.Replace(s, " ").Trim();

    static string BaseOp(string text)
    {
        string t = text;
        if (t.StartsWith("@")) { int sp = t.IndexOf(' '); t = sp >= 0 ? t[(sp + 1)..] : t; }
        int e = t.IndexOfAny(new[] { ' ', '.', ';' });
        return e < 0 ? t : t[..e];
    }

    static int Verify(string oracle, string corpus, string[] a)
    {
        int every = int.Parse(Opt(a, "--every", "1"));
        string show = Opt(a, "--show", "");
        int examples = int.Parse(Opt(a, "--examples", "3"));
        int jobs = int.Parse(Opt(a, "--jobs", "4"));
        var firstCtrl = new Dictionary<string, string>();
        foreach (var ln in File.ReadLines(Path.Combine(corpus, "unique.tsv")).Skip(1))
        {
            var f = ln.Split('\t');
            firstCtrl[f[0]] = f[9];
        }
        var hashes = File.ReadLines(Path.Combine(oracle, "index.tsv")).Skip(1).Select(l => l.Split('\t')[0]).Where((h, i) => i % every == 0).ToList();
        var total = new ConcurrentDictionary<string, long>();
        var bad = new ConcurrentDictionary<string, long>();
        var ex = new ConcurrentDictionary<string, ConcurrentQueue<string>>();
        long nInst = 0, nOk = 0, nUnknown = 0;
        var sw = Stopwatch.StartNew();
        Parallel.ForEach(hashes, new ParallelOptions { MaxDegreeOfParallelism = jobs }, h =>
        {
            var code = File.ReadAllBytes(Path.Combine(corpus, "code", h + ".bin"));
            var ctrl = File.ReadAllBytes(Path.Combine(corpus, "ctrl", firstCtrl[h] + ".bin"));
            var (start, end) = Program_(code, ctrl);
            var insts = SassDecoder.DecodeBlock(code, start, end);
            var lines = File.ReadAllLines(Path.Combine(oracle, h + ".sass"));
            long ok = 0;
            var tl = new Dictionary<string, long>(); var bl = new Dictionary<string, long>();
            for (int k = 0; k < lines.Length; k++)
            {
                var m = Line.Match(lines[k]);
                if (!m.Success) continue;
                string want = Norm(m.Groups[2].Value + " ;");
                string op = BaseOp(want);
                tl[op] = tl.GetValueOrDefault(op) + 1;
                string got = k < insts.Count ? Norm(insts[k].ToSassText()) : "<missing>";
                bool addrOk = k < insts.Count && insts[k].Address == Convert.ToInt32(m.Groups[1].Value, 16);
                if (got == want && addrOk) { ok++; continue; }
                bl[op] = bl.GetValueOrDefault(op) + 1;
                if (k < insts.Count && insts[k].Opcode.StartsWith(".UNKNOWN")) Interlocked.Increment(ref nUnknown);
                if (show.Length == 0 || show == op)
                {
                    var q = ex.GetOrAdd(op, _ => new ConcurrentQueue<string>());
                    if (q.Count < examples) q.Enqueue($"  want {want}\n  got  {got}\n       {m.Groups[3].Value} {m.Groups[4].Value}  {h}@{m.Groups[1].Value}");
                }
            }
            Interlocked.Add(ref nInst, lines.Length);
            Interlocked.Add(ref nOk, ok);
            foreach (var kv in tl) total.AddOrUpdate(kv.Key, kv.Value, (_, v) => v + kv.Value);
            foreach (var kv in bl) bad.AddOrUpdate(kv.Key, kv.Value, (_, v) => v + kv.Value);
        });
        Console.WriteLine($"blocks {hashes.Count}  instructions {nInst}  exact {nOk}  ({100.0 * nOk / Math.Max(1, nInst):F4}%)  unknown-opcode {nUnknown}  [{sw.Elapsed.TotalSeconds:F1}s]");
        Console.WriteLine($"mnemonics {total.Count}; with >=100 occurrences: {total.Count(kv => kv.Value >= 100)}, of those exact: {total.Count(kv => kv.Value >= 100 && !bad.ContainsKey(kv.Key))}");
        Console.WriteLine("unmatched by mnemonic (count / occurrences):");
        foreach (var kv in bad.OrderByDescending(kv => kv.Value).Take(40))
            Console.WriteLine($"  {kv.Key,-10} {kv.Value,9} / {total[kv.Key],9}");
        foreach (var kv in ex.OrderByDescending(kv => bad.GetValueOrDefault(kv.Key)).Take(show.Length > 0 ? 1 : 25))
        {
            Console.WriteLine($"-- {kv.Key}");
            foreach (var s in kv.Value) Console.WriteLine(s);
        }
        return nOk == nInst ? 0 : 1;
    }

    static int WholeCorpus(string corpus, string[] a)
    {
        int jobs = int.Parse(Opt(a, "--jobs", "4"));
        var rows = File.ReadLines(Path.Combine(corpus, "unique.tsv")).Skip(1).Select(l => l.Split('\t')).ToList();
        long nInst = 0, nUnknown = 0, bytes = 0;
        var sw = Stopwatch.StartNew();
        Parallel.ForEach(rows, new ParallelOptions { MaxDegreeOfParallelism = jobs }, f =>
        {
            var code = File.ReadAllBytes(Path.Combine(corpus, "code", f[0] + ".bin"));
            var ctrl = File.ReadAllBytes(Path.Combine(corpus, "ctrl", f[9] + ".bin"));
            var (start, end) = Program_(code, ctrl);
            var insts = SassDecoder.DecodeBlock(code, start, end);
            long u = insts.Count(i => i.Opcode.StartsWith(".UNKNOWN"));
            Interlocked.Add(ref nInst, insts.Count);
            Interlocked.Add(ref nUnknown, u);
            Interlocked.Add(ref bytes, end - start);
        });
        Console.WriteLine($"blocks {rows.Count}  program bytes {bytes}  instructions {nInst}  unknown-opcode {nUnknown}  [{sw.Elapsed.TotalSeconds:F1}s]");
        return 0;
    }

    sealed class Agg
    {
        public long Count;
        public int Blocks;
        public readonly int[] Stage = new int[8];            // distinct blocks per control stage value (0 vs .. 5 cs, 6 and 7+ = above)
        public readonly SortedSet<string> Examples = new(StringComparer.Ordinal);
        public string Decode = "ok";
        public string ExHash = "";                            // smallest-hash block that holds the row (undecoded / partial rows)
        public int ExAddr;
        public ulong ExLo, ExHi;
    }

    /// Corpus rows as (code hash, code path, ctrl path): title A layout (unique.tsv) or runtime dump layout (manifest.tsv, duplicates folded).
    static List<(string hash, string code, string ctrl)> CorpusRows(string dir)
    {
        var rows = new List<(string, string, string)>();
        string uniq = Path.Combine(dir, "unique.tsv"), man = Path.Combine(dir, "manifest.tsv");
        if (File.Exists(uniq))
        {
            foreach (var l in File.ReadLines(uniq).Skip(1))
            {
                var f = l.Split('\t');
                rows.Add((f[0], Path.Combine(dir, "code", f[0] + ".bin"), Path.Combine(dir, "ctrl", f[9] + ".bin")));
            }
        }
        else if (File.Exists(man))
        {
            var seen = new HashSet<string>();
            foreach (var l in File.ReadLines(man).Skip(1))
            {
                var f = l.Split('\t');
                if (f.Length < 13 || !seen.Add(f[10])) continue;
                rows.Add((f[10], Path.Combine(dir, f[11]), Path.Combine(dir, f[12])));
            }
        }
        else throw new FileNotFoundException("no unique.tsv or manifest.tsv in " + dir);
        return rows;
    }

    static bool Partial(SassInst i)
    {
        foreach (var m in i.Mods) if (m.StartsWith("?")) return true;
        foreach (var o in i.Operands) if (o.Text.StartsWith("?")) return true;
        return false;
    }

    static int Census(string corpus, string[] a)
    {
        int jobs = int.Parse(Opt(a, "--jobs", "4"));
        int examples = int.Parse(Opt(a, "--examples", "5"));
        bool all = Array.IndexOf(a, "--all-blocks") >= 0;
        string outPath = Opt(a, "--out", "");
        string blocksPath = Opt(a, "--blocks", "");
        if (outPath.Length == 0) { Console.Error.WriteLine("--census needs --out <census.tsv>"); return 2; }
        var rows = CorpusRows(corpus);
        var global = new Dictionary<string, Agg>();
        var blockRows = new ConcurrentBag<string>();
        long nInst = 0, skipped = 0;
        var sw = Stopwatch.StartNew();
        Parallel.ForEach(rows, new ParallelOptions { MaxDegreeOfParallelism = jobs }, r =>
        {
            byte[] code, ctrl;
            try { code = File.ReadAllBytes(r.code); ctrl = File.ReadAllBytes(r.ctrl); }
            catch (IOException) { Interlocked.Increment(ref skipped); return; }
            if (ctrl.Length < 0x58 || BitConverter.ToUInt32(ctrl, 0) != 0x98761234) { Interlocked.Increment(ref skipped); return; }
            int stage = BitConverter.ToUInt16(ctrl, 0x56);
            var (start, end) = Program_(code, ctrl);
            if (end <= start) { Interlocked.Increment(ref skipped); return; }
            var insts = SassDecoder.DecodeBlock(code, start, end);
            var local = new Dictionary<string, (long n, string cls, SassInst first)>();
            long unk = 0, part = 0;
            foreach (var i in insts)
            {
                bool undec = i.Opcode.StartsWith(".UNKNOWN");
                bool partial = !undec && Partial(i);
                string key = i.Opcode + "\t" + string.Join(".", i.Mods);
                string cls = undec ? "undecoded" : partial ? "partial" : "ok";
                if (undec) unk++; else if (partial) part++;
                if (local.TryGetValue(key, out var e)) local[key] = (e.n + 1, e.cls, e.first);
                else local[key] = (1, cls, i);
            }
            lock (global)
            {
                foreach (var kv in local)
                {
                    if (!global.TryGetValue(kv.Key, out var g)) global[kv.Key] = g = new Agg();
                    g.Count += kv.Value.n;
                    g.Blocks++;
                    g.Stage[Math.Min(stage, 7)]++;
                    if (kv.Value.cls != "ok" && (g.ExHash.Length == 0 || string.CompareOrdinal(r.hash, g.ExHash) < 0))
                    {
                        g.Decode = kv.Value.cls;
                        g.ExHash = r.hash; g.ExAddr = kv.Value.first.Address; g.ExLo = kv.Value.first.Lo; g.ExHi = kv.Value.first.Hi;
                    }
                    g.Examples.Add(r.hash);
                    if (!all && examples > 0 && g.Examples.Count > examples) g.Examples.Remove(g.Examples.Max!);
                }
            }
            Interlocked.Add(ref nInst, insts.Count);
            var ops = string.Join(" ", local.Keys.Select(k => k.TrimEnd('\t').Replace('\t', '.')).OrderBy(k => k, StringComparer.Ordinal));
            blockRows.Add($"{r.hash}\t{stage}\t{insts.Count}\t{unk}\t{part}\t{ops}");
        });
        string[] sn = { "vs", "ps", "gs", "tcs", "tes", "cs", "stage6", "stage7+" };
        using (var w = new StreamWriter(outPath, false, new UTF8Encoding(false)))
        {
            w.NewLine = "\n";
            w.WriteLine("opcode\tmodifiers\tcount\tblocks\tstage_blocks\tdecode\texample_blocks\texample_addr\texample_lo\texample_hi");
            foreach (var kv in global.OrderBy(k => k.Key.Split('\t')[0], StringComparer.Ordinal).ThenByDescending(k => k.Value.Count).ThenBy(k => k.Key, StringComparer.Ordinal))
            {
                var g = kv.Value;
                var st = string.Join(";", Enumerable.Range(0, 8).Where(k => g.Stage[k] > 0).Select(k => sn[k] + "=" + g.Stage[k]));
                bool ex = g.Decode != "ok";
                w.WriteLine($"{kv.Key}\t{g.Count}\t{g.Blocks}\t{st}\t{g.Decode}\t{string.Join(",", g.Examples)}\t" +
                            (ex ? $"0x{g.ExAddr:x}\t0x{g.ExLo:x16}\t0x{g.ExHi:x16}" : "\t\t"));
            }
        }
        if (blocksPath.Length > 0)
            File.WriteAllLines(blocksPath, new[] { "hash\tstage\tinstructions\tundecoded\tpartial\tops" }.Concat(blockRows.OrderBy(s => s, StringComparer.Ordinal)));
        Console.WriteLine($"blocks {rows.Count - skipped} (skipped {skipped})  instructions {nInst}  rows {global.Count}  [{sw.Elapsed.TotalSeconds:F1}s]");
        return 0;
    }

    static int Dump(string codePath, string ctrlPath)
    {
        var code = File.ReadAllBytes(codePath);
        var (start, end) = Program_(code, File.ReadAllBytes(ctrlPath));
        foreach (var i in SassDecoder.DecodeBlock(code, start, end))
            Console.WriteLine($"/*{i.Address:x5}*/ {i.ToSassText()} ; 0x{i.Lo:x16} 0x{i.Hi:x16}  dst={i.DstCount} sched=0x{i.Sched:x6}");
        return 0;
    }
}
