// SPDX-License-Identifier: MIT
// hwcheck --coopmat: the element-order probe of the cooperative-matrix HMMA path (ampere/frontend/AmpereCoopMat.cs), on the real
// GPU through Vulkan only (no CUDA toolkit needed).
//
//   hwcheck --coopmat [--out <dir>] [--warps 64] [--seed 1] [--header <file.h>]
//
// 1. Lists the device's VK_KHR_cooperative_matrix properties and its subgroup size; the path needs 16x8x16 float16 subgroup
//    matrices (float16 and float32 accumulators) and 32-lane subgroups.
// 2. Translates HMMA.16816.F16 and HMMA.16816.F32 three ways with spirv_harness --hwcheck: the scalar emulation (checked against
//    the tensor core on an RTX 3080, README), the cooperative-matrix form with the identity order (component index i = PTX
//    fragment element i), and the cooperative-matrix form with the translator's order (AmpereCoopMat.Order*).
// 3. Runs them on random fp16 inputs. The identity run is a black box of the device's real order: every candidate order (A by
//    register with optional half swaps, B and C/D by element) predicts what that run must output (the CPU reference of the PTX
//    layout with the inputs placed where the candidate says the hardware takes them); the candidates that predict it within the
//    HMMA tolerance are the device's order. No candidate = the device does not give each lane the PTX fragment's elements.
// 4. Checks the translator's order: its run against the CPU reference and against the scalar emulation (max difference in f16
//    ulps / f32 ulps).
// 5. --header: writes the F16 module with the translator's order as a C++ array plus its test inputs and expected outputs, for
//    the runtime's probe (runtime/src/gfx/vk_coopmat_probe.cpp).
using Nx.Shaders.Ampere;
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;

namespace Hwcheck
{
    static class CoopMat
    {
        const uint F16 = 0, F32 = 1, ScopeSubgroup = 3;
        // HMMA.16816.F16 R0, R2, R6, R8 (D R0..R1, A R2..R5, B R6..R7, C R8..R9) and HMMA.16816.F32 R0, R4, R8, R10
        // (D R0..R3, A R4..R7, B R8..R9, C R10..R13), encoded by the decoder's field layout (SassDecoder.Tensor.cs; scheduling bits
        // as ptxas sets them).
        static readonly (string id, ulong lo, ulong hi, bool f32, int rd, int ra, int rb, int rc)[] Shapes =
        {
            ("f16", 0x000000060200723cUL, 0x010fe20000000808UL, false, 0, 2, 6, 8),
            ("f32", 0x000000080400723cUL, 0x010fe2000000180aUL, true, 0, 4, 8, 10),
        };

        static string Opt(string[] a, string n, string d) { int i = Array.IndexOf(a, n); return i >= 0 && i + 1 < a.Length ? a[i + 1] : d; }

        public static int Run(string[] args, string bin, string outDir)
        {
            int warps = int.Parse(Opt(args, "--warps", "64"));
            int seed = int.Parse(Opt(args, "--seed", "1"));
            string header = Opt(args, "--header", null);
            Directory.CreateDirectory(outDir);
            int threads = warps * 32;
            using var vk = new VkRunner(threads * Inputs.Words * 4, true);
            Console.WriteLine($"Vulkan: {vk.DeviceName}, subgroup size {vk.SubgroupSize}");
            string[] ct = { "f16", "f32", "f64", "s8", "s16", "s32", "s64", "u8", "u16", "u32", "u64" };
            string T(uint t) => t < ct.Length ? ct[t] : t.ToString();
            foreach (var p in vk.CoopMatProps)
                Console.WriteLine($"  coopmat {p.m}x{p.n}x{p.k} A {T(p.a)} B {T(p.b)} C {T(p.c)} R {T(p.r)} sat {p.sat} scope {p.scope}");
            bool Has(uint acc) => vk.CoopMatProps.Any(p => p.m == 16 && p.n == 8 && p.k == 16 && p.a == F16 && p.b == F16 && p.c == acc && p.r == acc && p.scope == ScopeSubgroup);
            bool ok = true;
            if (vk.SubgroupSize != 32) { Console.WriteLine("FAIL: subgroup size is not 32"); ok = false; }
            foreach (uint acc in new[] { F16, F32 })
                if (!Has(acc)) { Console.WriteLine($"FAIL: no 16x8x16 f16 x f16 + {T(acc)} subgroup matrix"); ok = false; }
            if (!ok) return 1;

            string casesTsv = Path.Combine(outDir, "coopmat_cases.tsv");
            File.WriteAllLines(casesTsv, Shapes.Select(s => $"{s.id}\t{s.lo:x16} {s.hi:x16}"));
            string identity = "A=0,1,2,3,4,5,6,7;B=0,1,2,3;C=0,1,2,3;F=0,1,2,3";
            string Translate(string name, string coop, string order)
            {
                string dir = Path.Combine(outDir, "coopmat_" + name);
                if (Directory.Exists(dir)) Directory.Delete(dir, true);
                Environment.SetEnvironmentVariable("NXRT_HARNESS_COOPMAT", coop);
                Environment.SetEnvironmentVariable("NXRT_HARNESS_COOPMAT_ORDER", order);
                Tools.Run("dotnet", $"\"{Path.Combine(bin, "spirv_harness.dll")}\" --hwcheck \"{casesTsv}\" --out \"{dir}\" --local-x 32");
                foreach (string line in File.ReadAllLines(Path.Combine(dir, "hwcheck.tsv")))
                {
                    string[] f = line.Split('\t');
                    if (f[1] != "ok") throw new Exception($"{name} {f[0]}: {f[1]} {(f.Length > 3 ? f[3] : "")}");
                }
                return dir;
            }
            string scalarDir = Translate("scalar", "", "");
            string identDir = Translate("identity", "1", identity);
            string orderDir = Translate("order", "1", "");
            Environment.SetEnvironmentVariable("NXRT_HARNESS_COOPMAT", null);
            Environment.SetEnvironmentVariable("NXRT_HARNESS_COOPMAT_ORDER", null);

            int failures = 0;
            var rnd = new Random(seed);
            uint[] probeInput = null, probeExpected = null;
            foreach (var s in Shapes)
            {
                uint[] input = MakeInput(rnd, threads, s.f32, s.rc, small: false);
                uint[] RunMod(string dir)
                {
                    uint[] d = (uint[])input.Clone();
                    string e = vk.Run(File.ReadAllBytes(Path.Combine(dir, s.id + ".spv")), d, warps);
                    if (e != null) throw new Exception($"{dir} {s.id}: {e}");
                    return d;
                }
                uint[] scalar = RunMod(scalarDir), ident = RunMod(identDir), ours = RunMod(orderDir);
                Console.WriteLine($"HMMA.16816.{(s.f32 ? "F32" : "F16")}:");

                // the device's order from the identity run (first warp), checked on all warps
                var found = Search(s, input, ident, s.f32);
                if (found.Count == 0) { Console.WriteLine("  FAIL: no candidate order explains the identity run (lane ownership differs)"); failures++; }
                foreach (var (pa, pb, pc) in found.Take(4))
                {
                    int bad = 0;
                    for (int w = 0; w < warps; w++) bad += Mismatches(s, input, ident, w, pa, pb, pc);
                    Console.WriteLine($"  device order: A={string.Join(",", pa)};B={string.Join(",", pb)};{(s.f32 ? "F" : "C")}={string.Join(",", pc)}" +
                                      $"  ({bad} mismatching elements over {warps} warps)");
                }
                if (found.Count > 4) Console.WriteLine($"  ... {found.Count} candidates");

                // the translator's order against the CPU reference and the scalar emulation
                int[] idA = Enumerable.Range(0, 8).ToArray(), id4 = Enumerable.Range(0, 4).ToArray();
                int badRef = 0;
                for (int w = 0; w < warps; w++) badRef += Mismatches(s, input, ours, w, idA, id4, id4, alreadyOrdered: true);
                (double maxUlpRef, double maxUlpScalar) = MaxUlps(s, input, ours, scalar, warps);
                int badScalar = 0;
                for (int w = 0; w < warps; w++) badScalar += Mismatches(s, input, scalar, w, idA, id4, id4, alreadyOrdered: true);
                Console.WriteLine($"  translator order: {badRef} elements outside the HMMA tolerance of the CPU reference (scalar emulation: {badScalar});" +
                                  $" max difference {maxUlpRef:0.##} ulp to the reference, {maxUlpScalar:0.##} ulp to the scalar emulation");
                if (badRef != 0) failures++;

                if (!s.f32 && header != null)
                {
                    // small integers: every product and sum is exact in fp16, so the probe compares bit for bit
                    probeInput = MakeInput(new Random(12345), 32, false, s.rc, small: true);
                    probeExpected = (uint[])probeInput.Clone();
                    uint[] gpu = (uint[])probeInput.Clone();
                    string e = vk.Run(File.ReadAllBytes(Path.Combine(orderDir, s.id + ".spv")), gpu, 1);
                    if (e != null) throw new Exception(e);
                    var d = Reference(s, probeInput, 0, Elem(s, probeInput, 0, 'A'), Elem(s, probeInput, 0, 'B'), Elem(s, probeInput, 0, 'C'), s.f32);
                    for (int l = 0; l < 32; l++)
                        for (int q = 0; q < 2; q++)
                            probeExpected[l * Inputs.Words + s.rd + q] = (uint)(H(d.val[l, 2 * q]) | H(d.val[l, 2 * q + 1]) << 16);
                    for (int l = 0; l < 32; l++)
                        for (int q = 0; q < 2; q++)
                            if (gpu[l * Inputs.Words + s.rd + q] != probeExpected[l * Inputs.Words + s.rd + q])
                            { Console.WriteLine($"  FAIL: probe data lane {l} D{q}: gpu {gpu[l * Inputs.Words + s.rd + q]:x8} expected {probeExpected[l * Inputs.Words + s.rd + q]:x8}"); failures++; }
                    WriteHeader(header, File.ReadAllBytes(Path.Combine(orderDir, s.id + ".spv")), probeInput, probeExpected, s);
                    Console.WriteLine($"  wrote {header}");
                }
            }
            Console.WriteLine(failures == 0 ? "coopmat: OK" : $"coopmat: {failures} failure(s)");
            return failures == 0 ? 0 : 1;
        }

        // ---- inputs -------------------------------------------------------------------------------------------------------

        static ushort H(double v) => BitConverter.HalfToUInt16Bits((Half)v);
        static double HD(uint bits) => (double)BitConverter.UInt16BitsToHalf((ushort)bits);

        /// R0..R15 per thread: fp16 pairs, multiples of 1/64 in [-2, 2) (or small integers in [-3, 3] for the bit-exact probe);
        /// an F32 accumulator's registers hold fp32 values of the same kind.
        static uint[] MakeInput(Random rnd, int threads, bool f32, int rc, bool small)
        {
            uint[] d = new uint[threads * Inputs.Words];
            double V() => small ? rnd.Next(-3, 4) : rnd.Next(-128, 128) / 64.0;
            for (int t = 0; t < threads; t++)
                for (int r = 0; r < 16; r++)
                    d[t * Inputs.Words + r] = f32 && r >= rc && r < rc + 4 ? BitConverter.SingleToUInt32Bits((float)V()) : (uint)(H(V()) | H(V()) << 16);
            return d;
        }

        // ---- PTX fragment layout reference --------------------------------------------------------------------------------

        /// The fragment elements of warp w as doubles: A [lane, 8], B [lane, 4], C [lane, 4] (element e = register e >> 1, half e & 1;
        /// F32 C: element = register).
        static double[,] Elem((string id, ulong lo, ulong hi, bool f32, int rd, int ra, int rb, int rc) s, uint[] data, int w, char which)
        {
            int n = which == 'A' ? 8 : 4, r0 = which == 'A' ? s.ra : which == 'B' ? s.rb : s.rc;
            var e = new double[32, n];
            for (int l = 0; l < 32; l++)
            {
                int b = (w * 32 + l) * Inputs.Words;
                for (int i = 0; i < n; i++)
                    e[l, i] = which == 'C' && s.f32 ? BitConverter.UInt32BitsToSingle(data[b + r0 + i]) : HD(data[b + r0 + (i >> 1)] >> (16 * (i & 1)));
            }
            return e;
        }

        /// D = A B + C over the warp in the PTX layout, exactly (double), with the HMMA error bound per element.
        static (double[,] val, double[,] bound) Reference((string id, ulong lo, ulong hi, bool f32, int rd, int ra, int rb, int rc) s, uint[] data, int w,
                                                         double[,] a, double[,] b, double[,] c, bool f32)
        {
            var val = new double[32, 4];
            var bound = new double[32, 4];
            for (int l = 0; l < 32; l++)
            {
                int g = l >> 2, t = l & 3;
                for (int i = 0; i < 4; i++)
                {
                    // D element i of lane l: F16 register i >> 1 half i & 1 -> row g + 8 (i >> 1), column 2t + (i & 1); F32 register i -> same
                    int row = g + 8 * (i >> 1), col = 2 * t + (i & 1);
                    double sum = c[l, i], bnd = Math.Abs(c[l, i]);
                    for (int k = 0; k < 16; k++)
                    {
                        // A(row, k): lane 4 (row & 7) + ((k & 7) >> 1), register 2 (k >> 3) + (row >> 3), half k & 1
                        double av = a[4 * (row & 7) + ((k & 7) >> 1), 2 * (2 * (k >> 3) + (row >> 3)) + (k & 1)];
                        // B(k, col): lane 4 col + ((k & 7) >> 1), register k >> 3, half k & 1
                        double bv = b[4 * col + ((k & 7) >> 1), 2 * (k >> 3) + (k & 1)];
                        sum += av * bv;
                        bnd += Math.Abs(av * bv);
                    }
                    val[l, i] = sum;
                    bound[l, i] = bnd;
                }
            }
            return (val, bound);
        }

        static uint OutBits(uint[] data, int w, int l, int i, int rd, bool f32)
        {
            int b = (w * 32 + l) * Inputs.Words;
            return f32 ? data[b + rd + i] : (data[b + rd + (i >> 1)] >> (16 * (i & 1))) & 0xffff;
        }

        static uint Round(double v, bool f32) => f32 ? BitConverter.SingleToUInt32Bits((float)v) : H(v);

        /// Elements of warp w whose output differs (beyond the HMMA tolerance) from what the device would give if its component
        /// index i meant PTX element pa[i] / pb[i] / pc[i] and the module inserted element i at index i (the identity run).
        /// alreadyOrdered: the module already maps (the translator's order or the scalar emulation): compare with the reference.
        static int Mismatches((string id, ulong lo, ulong hi, bool f32, int rd, int ra, int rb, int rc) s, uint[] input, uint[] output, int w,
                              int[] pa, int[] pb, int[] pc, bool alreadyOrdered = false, bool stopAtFirst = false)
        {
            double[,] ai = Elem(s, input, w, 'A'), bi = Elem(s, input, w, 'B'), ci = Elem(s, input, w, 'C');
            var a = new double[32, 8]; var b = new double[32, 4]; var c = new double[32, 4];
            for (int l = 0; l < 32; l++)
            {
                for (int i = 0; i < 8; i++) a[l, pa[i]] = ai[l, i];
                for (int i = 0; i < 4; i++) { b[l, pb[i]] = bi[l, i]; c[l, pc[i]] = ci[l, i]; }
            }
            var (val, bound) = Reference(s, input, w, a, b, c, s.f32);
            int bad = 0;
            for (int l = 0; l < 32; l++)
                for (int i = 0; i < 4; i++)
                {
                    int e = alreadyOrdered ? i : pc[i];
                    if (!Compare.HmmaClose(OutBits(output, w, l, i, s.rd, s.f32), Round(val[l, e], s.f32), s.f32, bound[l, e], false))
                    {
                        bad++;
                        if (stopAtFirst) return bad;
                    }
                }
            return bad;
        }

        static IEnumerable<int[]> Perms(int n)
        {
            var p = Enumerable.Range(0, n).ToArray();
            IEnumerable<int[]> Rec(int k)
            {
                if (k == n) { yield return (int[])p.Clone(); yield break; }
                for (int i = k; i < n; i++)
                {
                    (p[k], p[i]) = (p[i], p[k]);
                    foreach (var r in Rec(k + 1)) yield return r;
                    (p[k], p[i]) = (p[i], p[k]);
                }
            }
            return Rec(0);
        }

        static List<(int[] pa, int[] pb, int[] pc)> Search((string id, ulong lo, ulong hi, bool f32, int rd, int ra, int rb, int rc) s, uint[] input, uint[] ident, bool f32)
        {
            var aCands = new List<int[]>();
            foreach (int[] rp in Perms(4))
                for (int swap = 0; swap < 16; swap++)
                {
                    var pa = new int[8];
                    for (int q = 0; q < 4; q++)
                        for (int h = 0; h < 2; h++) pa[2 * q + h] = 2 * rp[q] + (h ^ ((swap >> q) & 1));
                    aCands.Add(pa);
                }
            var four = Perms(4).ToList();
            var found = new List<(int[], int[], int[])>();
            foreach (int[] pa in aCands)
                foreach (int[] pb in four)
                    foreach (int[] pc in four)
                        if (Mismatches(s, input, ident, 0, pa, pb, pc, stopAtFirst: true) == 0) found.Add((pa, pb, pc));
            return found;
        }

        static (double, double) MaxUlps((string id, ulong lo, ulong hi, bool f32, int rd, int ra, int rb, int rc) s, uint[] input, uint[] ours, uint[] scalar, int warps)
        {
            double toRef = 0, toScalar = 0;
            double Ulp(double v) => v == 0 ? (s.f32 ? Math.Pow(2, -149) : Math.Pow(2, -24)) : Math.Pow(2, Math.Floor(Math.Log2(Math.Abs(v))) - (s.f32 ? 23 : 10));
            double Val(uint bits) => s.f32 ? BitConverter.UInt32BitsToSingle(bits) : HD(bits);
            int[] idA = Enumerable.Range(0, 8).ToArray(), id4 = Enumerable.Range(0, 4).ToArray();
            for (int w = 0; w < warps; w++)
            {
                var (val, _) = Reference(s, input, w, Elem(s, input, w, 'A'), Elem(s, input, w, 'B'), Elem(s, input, w, 'C'), s.f32);
                for (int l = 0; l < 32; l++)
                    for (int i = 0; i < 4; i++)
                    {
                        double o = Val(OutBits(ours, w, l, i, s.rd, s.f32)), r = Val(Round(val[l, i], s.f32)), sc = Val(OutBits(scalar, w, l, i, s.rd, s.f32));
                        toRef = Math.Max(toRef, Math.Abs(o - r) / Ulp(r));
                        toScalar = Math.Max(toScalar, Math.Abs(o - sc) / Ulp(sc));
                    }
            }
            return (toRef, toScalar);
        }

        static void WriteHeader(string path, byte[] spv, uint[] input, uint[] expected, (string id, ulong lo, ulong hi, bool f32, int rd, int ra, int rb, int rc) s)
        {
            var sb = new StringBuilder();
            sb.AppendLine("// SPDX-License-Identifier: MIT");
            sb.AppendLine("// Generated by `hwcheck --coopmat --header` (tools/shaders/hwcheck/CoopMat.cs): do not edit.");
            sb.AppendLine("// The translator's cooperative-matrix HMMA.16816.F16 R0, R2, R6, R8 in its hardware-check module (set 0 binding 0: uniform");
            sb.AppendLine("// buffer, set 1 binding 0: storage buffer of kCoopMatProbeWords words per invocation, R0..R15 at word 0..15), local size 32,");
            sb.AppendLine("// one workgroup; the input and the expected output (bit exact: small integers) of the runtime's probe.");
            sb.AppendLine($"// Element order: {AmpereOrderText()}");
            sb.AppendLine("#pragma once");
            sb.AppendLine("#include <cstdint>");
            sb.AppendLine($"inline constexpr uint32_t kCoopMatProbeWords = {Inputs.Words};");
            sb.AppendLine($"inline constexpr uint32_t kCoopMatProbeDst = {s.rd};   // D: words kCoopMatProbeDst, +1 of each invocation");
            void Arr(string name, uint[] w)
            {
                sb.AppendLine($"inline constexpr uint32_t {name}[{w.Length}] = {{");
                for (int i = 0; i < w.Length; i += 8) sb.AppendLine("    " + string.Join(", ", w.Skip(i).Take(8).Select(x => $"0x{x:x8}u")) + ",");
                sb.AppendLine("};");
            }
            uint[] words = new uint[spv.Length / 4];
            Buffer.BlockCopy(spv, 0, words, 0, spv.Length);
            Arr("kCoopMatProbeSpirv", words);
            Arr("kCoopMatProbeInput", input);
            Arr("kCoopMatProbeExpected", expected);
            File.WriteAllText(path, sb.ToString().Replace("\r\n", "\n"));
        }

        static string AmpereOrderText()
        {
            string o = Environment.GetEnvironmentVariable("NXRT_HARNESS_COOPMAT_ORDER");
            return string.IsNullOrEmpty(o) ? "the translator's default (AmpereCoopMat.cs)" : o;
        }
    }
}
