// spirv_harness --serve: a long-lived translator. One process translates blocks as they are asked for, straight into a shader cache
// folder (the one the runtime opens: <hash>.spv + results.tsv), so .NET's start-up and the translator's JIT are paid once. Used by
//   * the library's setup: every block of the corpus, in the order it sends them (recorded boot order first);
//   * the runtime (nvn_shader_worker.cpp): blocks the game asks for that the cache does not have yet.
//
//   spirv_harness --serve --cache <spirv dir> [--corpus <corpus dir>] [--jobs N] [--priority below|idle|normal]
//
// Protocol: UTF-8 text lines both ways, '\n' terminated.
//   stdin   C <hash>                       translate corpus block <hash> (code/<hash>.bin, its first ctrl and GLSLC output)
//           T <hash> <code b64> <ctrl b64>  translate these bytes. When <hash> is also in the corpus, the corpus's control section and
//                                           GLSLC reflection are used (the same result the setup would write); else the bytes alone.
//           P below|idle|normal            set this process's priority class
//           E                              end of requests: finish everything queued, then exit 0
//           (end of input without E: finish the blocks being translated, drop the rest, exit 0. So closing the pipe pauses.)
//   stdout  I <text>                       information (first line: "I ready jobs=<n> corpus=<blocks>")
//           R\t<results.tsv row>           <hash>.spv (if the status produced one) is in the cache and this row is appended to results.tsv
//           X <hash> <reason>              request refused (not in the corpus, bad base64, hash mismatch); nothing was written
// T requests go ahead of C requests. A hash asked for twice in one process is translated once.
//
// Writes are safe against the other writer (library setup and runtime worker can run at the same time): <hash>.spv is written to a
// temporary name and moved into place (an existing file is kept: translation is deterministic), and each row is appended under an
// exclusive-write open of results.tsv (FileShare.Read: readers are never blocked; the library opens it the same way). An empty
// results.tsv gets the header first.

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Security.Cryptography;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

namespace SpirvHarness
{
    static partial class Program
    {
        sealed record CorpusEntry(string Group, int Occurrences, string CodePath, string CtrlPath, string GlslcPath);
        sealed record Request(string Hash, byte[] Code, byte[] Ctrl);

        static int Serve(string[] args)
        {
            string cache = Opt(args, "--cache", null) ?? throw new ArgumentException("--cache is required");
            string corpus = Opt(args, "--corpus", null);
            int jobs = Math.Max(1, int.Parse(Opt(args, "--jobs", Environment.ProcessorCount.ToString())));
            SetPriority(Opt(args, "--priority", "below"));
            Directory.CreateDirectory(cache);
            ThreadPool.GetMinThreads(out int minW, out int minIo);
            ThreadPool.SetMinThreads(Math.Max(minW, jobs * 2 + 4), minIo);   // each worker waits on a pool task (the timeout guard)

            Dictionary<string, CorpusEntry> index = LoadCorpusIndex(corpus);
            object outLock = new(), qLock = new();
            void Say(string line) { lock (outLock) { Console.Out.Write(line + "\n"); Console.Out.Flush(); } }

            LinkedList<Request> fast = new(), slow = new();   // T, C
            HashSet<string> asked = new();
            bool end = false, closed = false;

            Thread[] workers = new Thread[jobs];
            for (int i = 0; i < jobs; i++)
            {
                workers[i] = new Thread(() =>
                {
                    for (;;)
                    {
                        Request req;
                        lock (qLock)
                        {
                            while (fast.Count == 0 && slow.Count == 0 && !end && !closed) Monitor.Wait(qLock);
                            if (closed || (fast.Count == 0 && slow.Count == 0)) return;
                            LinkedList<Request> q = fast.Count > 0 ? fast : slow;
                            req = q.First.Value;
                            q.RemoveFirst();
                        }
                        try { ServeOne(req, index, cache, Say); }
                        catch (Exception e) { Say($"X {req.Hash} {e.GetType().Name}: {e.Message.Replace('\n', ' ')}"); }
                    }
                }) { IsBackground = true, Name = $"serve{i}" };
                workers[i].Start();
            }
            Say($"I ready jobs={jobs} corpus={index.Count}");

            // Input on its own thread: P lines keep working after E, and the process ends when the workers do.
            Thread reader = new(() =>
            {
                using StreamReader rd = new(Console.OpenStandardInput(), new UTF8Encoding(false), false, 1 << 16);
                string line;
                while ((line = rd.ReadLine()) != null)
                {
                    if (line.Length == 0) continue;
                    string[] f = line.Split(' ');
                    switch (f[0])
                    {
                        case "E":
                            lock (qLock) { end = true; Monitor.PulseAll(qLock); }
                            break;
                        case "P":
                            SetPriority(f.Length > 1 ? f[1] : "below");
                            break;
                        case "C" when f.Length >= 2:
                            lock (qLock) { if (asked.Add(f[1])) { slow.AddLast(new Request(f[1], null, null)); Monitor.Pulse(qLock); } }
                            break;
                        case "T" when f.Length >= 4:
                            byte[] code, ctrl;
                            try { code = Convert.FromBase64String(f[2]); ctrl = Convert.FromBase64String(f[3]); }
                            catch (FormatException) { Say($"X {f[1]} bad base64"); break; }
                            lock (qLock) { if (asked.Add(f[1])) { fast.AddLast(new Request(f[1], code, ctrl)); Monitor.Pulse(qLock); } }
                            break;
                        default:
                            Say($"I unknown request {f[0]}");
                            break;
                    }
                }
                lock (qLock)
                {
                    if (!end) { closed = true; fast.Clear(); slow.Clear(); }   // input closed without E: pause
                    end = true;
                    Monitor.PulseAll(qLock);
                }
            }) { IsBackground = true, Name = "stdin" };
            reader.Start();
            foreach (Thread t in workers) t.Join();
            return 0;
        }

        static void SetPriority(string p)
        {
            try
            {
                Process.GetCurrentProcess().PriorityClass = p switch
                {
                    "idle" => ProcessPriorityClass.Idle,
                    "normal" => ProcessPriorityClass.Normal,
                    _ => ProcessPriorityClass.BelowNormal,
                };
            }
            catch (Exception) { }
        }

        // unique_all.tsv (the library's full list) or unique.tsv: hash -> files of its first occurrence.
        static Dictionary<string, CorpusEntry> LoadCorpusIndex(string root)
        {
            Dictionary<string, CorpusEntry> index = new();
            if (string.IsNullOrEmpty(root)) return index;
            string list = Path.Combine(root, "unique_all.tsv");
            if (!File.Exists(list)) list = Path.Combine(root, "unique.tsv");
            if (!File.Exists(list)) return index;
            foreach (var row in ReadTsv(list))
            {
                string h = row["code_sha1_16"];
                index[h] = new CorpusEntry(row["stage"], int.TryParse(row.GetValueOrDefault("occurrences"), out int n) ? n : 1,
                                           Path.Combine(root, "code", h + ".bin"), Path.Combine(root, "ctrl", row["first_ctrl"] + ".bin"),
                                           Path.Combine(root, "glslc", row["first_glslc"] + ".bin"));
            }
            return index;
        }

        static void ServeOne(Request req, Dictionary<string, CorpusEntry> index, string cache, Action<string> say)
        {
            index.TryGetValue(req.Hash, out CorpusEntry ce);
            if (ce != null && !File.Exists(ce.CtrlPath)) ce = null;   // the setup removed the corpus meanwhile
            byte[] code = req.Code, ctrl = req.Ctrl;
            string glslc = null;
            if (ce != null)
            {
                code ??= File.ReadAllBytes(ce.CodePath);
                ctrl = File.ReadAllBytes(ce.CtrlPath);
                glslc = File.Exists(ce.GlslcPath) ? ce.GlslcPath : null;
            }
            if (code == null || ctrl == null) { say($"X {req.Hash} not in the corpus"); return; }
            if (req.Code != null)
            {
                string h = Convert.ToHexStringLower(SHA1.HashData(code)).Substring(0, 16);
                if (h != req.Hash) { say($"X {req.Hash} code hashes to {h}"); return; }
            }

            Result r;
            byte[] spv = null;
            byte[] spvCm = null, spvF16 = null;
            Task<(Result, byte[], byte[], byte[])> t = Task.Run(() =>
            {
                Result x = TranslateVariants(code, ctrl, out byte[] s, out byte[] cm, out byte[] f16, false, glslc);
                return (x, s, cm, f16);
            });
            if (t.Wait(Timeout)) (r, spv, spvCm, spvF16) = t.Result;
            else r = new Result { Status = "timeout", Error = $"no result after {Timeout.TotalSeconds} s" };
            r.Hash = req.Hash;
            r.Group = ce?.Group ?? "runtime";
            r.Stages = ce?.Occurrences ?? 1;
            string row = FormatRow(r);
            if (spv != null) PutSpv(cache, req.Hash, spv, spvCm, spvF16);
            AppendRow(Path.Combine(cache, "results.tsv"), row);
            say("R\t" + row);
        }

        static void PutSpv(string cache, string hash, byte[] spv, byte[] spvCm = null, byte[] spvF16 = null)
        {
            // the variants first (cooperative-matrix and float16, AmpereCoopMat.cs): a stale one must not outlive the words it was
            // made with
            foreach (var (ext, bytes) in new[] { (".cm.spv", spvCm), (".f16.spv", spvF16) })
            {
                string dst = Path.Combine(cache, hash + ext);
                if (bytes != null) PutFile(cache, dst, bytes);
                else
                {
                    try { if (File.Exists(dst)) File.Delete(dst); }
                    catch (IOException) { }
                    catch (UnauthorizedAccessException) { }
                }
            }
            PutFile(cache, Path.Combine(cache, hash + ".spv"), spv);
        }

        static void PutFile(string cache, string dst, byte[] bytes)
        {
            // A request comes only for a block without a results.tsv row, so a .spv already there is stale (its row was removed to have
            // the block translated again, or it is from another translator build) and is replaced: keeping it left a new row next to the
            // old words, and the runtime rejected the pair ("resources record does not match the SPIR-V": five of the game's culling-chain
            // compute shaders never ran, run game_1). The other writer writes the same words, so the last replace wins harmlessly.
            string tmp = $"{dst}.{Environment.ProcessId}.tmp";
            File.WriteAllBytes(tmp, bytes);
            try { File.Move(tmp, dst, true); }
            catch (IOException) { File.Delete(tmp); }                   // open in a reader right now: the old file stays for this run
            catch (UnauthorizedAccessException) { File.Delete(tmp); }
        }

        static readonly object RowLock = new();

        // Exclusive write, shared read: the library and the runtime's worker never interleave rows, readers are not blocked.
        static void AppendRow(string path, string row)
        {
            byte[] bytes = Encoding.UTF8.GetBytes(row + "\n");
            lock (RowLock)
            {
                for (int attempt = 0; ; attempt++)
                {
                    try
                    {
                        using FileStream fs = new(path, FileMode.OpenOrCreate, FileAccess.Write, FileShare.Read);
                        if (fs.Length == 0)
                        {
                            byte[] h = Encoding.UTF8.GetBytes(ResultsHeader + "\n");
                            fs.Write(h, 0, h.Length);
                        }
                        else
                        {
                            fs.Seek(0, SeekOrigin.End);
                        }
                        fs.Write(bytes, 0, bytes.Length);
                        return;
                    }
                    catch (IOException) when (attempt < 400)
                    {
                        Thread.Sleep(25);   // the other writer has it
                    }
                }
            }
        }
    }
}
