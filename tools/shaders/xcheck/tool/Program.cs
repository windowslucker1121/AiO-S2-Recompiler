// SPDX-License-Identifier: MIT
// xcheck: decode a shader corpus with our Ampere decoder and with nxnative's Ampere front end and list where they disagree. See ../README.md.
//   dotnet run --project tools\shaders\xcheck\tool -c Release [-- --out <dir>] [--corpus pkp=<dir>] [--corpus dkb=<dir>] [--corpus mkw=<dir>] [--jobs N] [--no-nvdisasm] | --layout | --words <file> | --ops <hex,hex,..>
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Text;

namespace Nx.Shaders.XCheck
{
    static class Program
    {
        static readonly string[] CorpusNames = { "pkp", "dkb", "mkw" };

        static int Main(string[] args)
        {
            try { Process.GetCurrentProcess().PriorityClass = ProcessPriorityClass.BelowNormal; } catch { }
            string root = FindRoot();
            var dirs = new string[3];
            dirs[0] = Path.Combine(root, "work", "shaders", "corpus");
            dirs[1] = Path.Combine(root, "work", "shaders_dkb", "corpus");
            dirs[2] = Path.Combine(root, "work", "mkw", "shader_dump");
            string outDir = Path.Combine(root, "work", "xcheck");
            int jobs = 6;
            bool nvdisasm = true, layout = false;
            string opsArg = "";
            string only = "";
            for (int i = 0; i < args.Length; i++)
            {
                switch (args[i])
                {
                    case "--out": outDir = args[++i]; break;
                    case "--jobs": jobs = int.Parse(args[++i]); break;
                    case "--no-nvdisasm": nvdisasm = false; break;
                    case "--layout": layout = true; break;
                    case "--words": return Report.CheckWords(args[++i], jobs);
                    case "--ops": opsArg = args[++i]; break;
                    case "--only": only = args[++i]; break;
                    case "--corpus":
                        var kv = args[++i].Split('=', 2);
                        int ci = Array.IndexOf(CorpusNames, kv[0]);
                        if (ci < 0) { Console.Error.WriteLine("corpus name must be pkp, dkb or mkw"); return 2; }
                        dirs[ci] = kv[1];
                        break;
                    default: Console.Error.WriteLine("unknown option " + args[i]); return 2;
                }
            }
            Directory.CreateDirectory(outDir);
            var sw = Stopwatch.StartNew();
            if (opsArg.Length > 0) return OpsCheck.Run(dirs, CorpusNames, opsArg, jobs);
            if (layout)
            {
                Layout.Run(dirs, CorpusNames, outDir, jobs);
                return 0;
            }
            var data = new ScanData();
            for (int c = 0; c < 3; c++)
            {
                if (only.Length > 0 && !only.Contains(CorpusNames[c])) continue;
                if (!Directory.Exists(dirs[c])) { Console.WriteLine($"{CorpusNames[c]}: {dirs[c]} missing, skipped"); continue; }
                Scan.Run(data, c, dirs[c], jobs);
                Console.WriteLine($"{CorpusNames[c]}: blocks {data.Blocks[c]} instructions {data.Instructions[c]}  [{sw.Elapsed.TotalSeconds:F0}s]");
            }
            var exs = Perturb.Exemplars(data);
            var cells = Perturb.Run(exs, jobs);
            Console.WriteLine($"[{sw.Elapsed.TotalSeconds:F0}s]");
            Report.Write(outDir, data, exs, cells, nvdisasm, jobs);
            Console.WriteLine($"done [{sw.Elapsed.TotalSeconds:F0}s]");
            return 0;
        }

        /// the repository folder: the nearest parent of the working directory or of the tool that holds work\shaders.
        static string FindRoot()
        {
            foreach (var start in new[] { Directory.GetCurrentDirectory(), AppContext.BaseDirectory })
                for (var d = new DirectoryInfo(start); d != null; d = d.Parent)
                    if (Directory.Exists(Path.Combine(d.FullName, "work", "shaders"))) return d.FullName;
            // a worktree lives in <root>\nx-wt\<name>
            return Path.GetFullPath(Path.Combine(Directory.GetCurrentDirectory(), "..", "..", ".."));
        }
    }
}
