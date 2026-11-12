// SPDX-License-Identifier: MIT
// Comparison rules. Integer results exact. Float results exact except that any NaN equals any NaN (per lane of the result
// type: f32, f16 halves, bf16 halves, f64). MUFU results are hardware approximations; the translated code runs the
// driver's GLSL.std.450 functions, so a per-function tolerance applies (documented in README.md, "Tolerances"):
//   RCP, RSQ, SQRT, EX2, LG2 (f32)  2 ulp
//   SIN, COS (f32)                  absolute 2^-20 (results lie in [-1, 1])
//   TANH (f32)                      absolute 2^-10 (hardware tanh.approx: maximum relative error 2^-11 per the PTX ISA)
//   any .F16 form                   1 ulp of the f16 result (counted through zero of the same sign)
// Special results (zero of either sign excluded: -0 vs +0 is a mismatch outside tolerance ops), inf and NaN must match
// exactly (NaN as a class) under every rule.
using Nx.Shaders.Ampere;
using System;
using System.Linq;

namespace Hwcheck
{
    enum Lane { Int, F32, F16x2, Bf16x2, F64 }

    static class Compare
    {
        /// How the destination registers of `i` are typed.
        public static Lane LaneOf(SassInst i)
        {
            bool has(string m) => i.Mods.Contains(m);
            switch (i.Opcode)
            {
                case "FADD": case "FMUL": case "FFMA": case "FMNMX": case "FRND": case "FSEL": return has("F64") ? Lane.F64 : Lane.F32;
                case "MUFU": return has("F16") ? Lane.F16x2 : Lane.F32;
                case "I2F": case "I2FP": return has("F64") ? Lane.F64 : has("F16") ? Lane.F16x2 : Lane.F32;
                case "F2F":
                    return i.Mods.FirstOrDefault(m => m is "F16" or "F32" or "F64" or "BF16") switch
                    {
                        "F16" => Lane.F16x2, "F64" => Lane.F64, "BF16" => Lane.Bf16x2, _ => Lane.F32,
                    };
                case "F2FP": return has("BF16") ? Lane.Bf16x2 : Lane.F16x2;
                case "HADD2": case "HMUL2": case "HFMA2": case "HMNMX2":
                    return has("F32") ? Lane.F32 : has("BF16") || has("BF16_V2") ? Lane.Bf16x2 : Lane.F16x2;
                case "DADD": case "DMUL": case "DFMA": return Lane.F64;
                case "HMMA": return has("F32") ? Lane.F32 : Lane.F16x2;
                default: return Lane.Int;
            }
        }

        static bool NaN32(uint v) => (v & 0x7fffffff) > 0x7f800000;
        static bool NaN16(uint v) => (v & 0x7fff) > 0x7c00;
        static bool NaNBf16(uint v) => (v & 0x7fff) > 0x7f80;

        /// Tolerance of a MUFU form: (ulps, absolute) or null = exact.
        public static (int ulp, double abs)? Tolerance(SassInst i)
        {
            if (i.Opcode != "MUFU" || Environment.GetEnvironmentVariable("HWCHECK_EXACT") == "1") return null;
            if (i.Mods.Contains("F16")) return (1, 0);
            if (i.Mods.Contains("SIN") || i.Mods.Contains("COS")) return (0, Math.Pow(2, -20));
            if (i.Mods.Contains("TANH")) return (0, Math.Pow(2, -10));
            return (2, 0);
        }

        /// Equal under the rules (whole-word compare for a 32-bit destination word; `lane` from LaneOf).
        public static bool Same(uint hw, uint ours, Lane lane, (int ulp, double abs)? tol)
        {
            if (hw == ours) return true;
            switch (lane)
            {
                case Lane.F32:
                    if (NaN32(hw) && NaN32(ours)) return true;
                    return tol is { } t32 && Close32(hw, ours, t32);
                case Lane.F16x2:
                    for (int s = 0; s < 32; s += 16)
                    {
                        uint a = (hw >> s) & 0xffff, b = (ours >> s) & 0xffff;
                        if (a == b || (NaN16(a) && NaN16(b))) continue;
                        if (tol is { } t && Close16(a, b, t.ulp)) continue;
                        return false;
                    }
                    return true;
                case Lane.Bf16x2:
                    for (int s = 0; s < 32; s += 16)
                    {
                        uint a = (hw >> s) & 0xffff, b = (ours >> s) & 0xffff;
                        if (a != b && !(NaNBf16(a) && NaNBf16(b))) return false;
                    }
                    return true;
                default:
                    return false;
            }
        }

        static bool Special32(uint v) => (v & 0x7f800000) == 0x7f800000 || (v & 0x7fffffff) == 0;

        static bool Close32(uint a, uint b, (int ulp, double abs) t)
        {
            if (Special32(a) || Special32(b))
                return t.abs > 0 && (a & 0x7fffffff) == 0 && (b & 0x7f800000) != 0x7f800000 && Math.Abs(BitConverter.UInt32BitsToSingle(b)) <= t.abs
                    || t.abs > 0 && (b & 0x7fffffff) == 0 && (a & 0x7f800000) != 0x7f800000 && Math.Abs(BitConverter.UInt32BitsToSingle(a)) <= t.abs;
            if (t.abs > 0) return Math.Abs((double)BitConverter.UInt32BitsToSingle(a) - BitConverter.UInt32BitsToSingle(b)) <= t.abs;
            if ((a ^ b) >> 31 != 0) return false;
            return Math.Abs((long)(a & 0x7fffffff) - (b & 0x7fffffff)) <= t.ulp;
        }

        static bool Close16(uint a, uint b, int ulp)
        {
            bool spec(uint v) => (v & 0x7c00) == 0x7c00;
            if (spec(a) || spec(b) || ((a ^ b) & 0x8000) != 0) return false;   // zero vs the smallest denormal counts as 1 ulp
            return Math.Abs((int)(a & 0x7fff) - (int)(b & 0x7fff)) <= ulp;
        }

        // ---- HMMA -------------------------------------------------------------------------------------------------------
        // The tensor core does not round like a chain of IEEE operations: its summation drops low bits of the aligned terms
        // (RTX 3080: HMMA.1688.F16 matched round-to-nearest of the exact sum on every element, HMMA.16816.F16 differed by up to
        // 2^-10.0 * B, the F32 forms by up to 2^-16.0 * B, where B = sum |a_k b_k| + |c|). The translation computes the products
        // exactly and sums them in fp32. A destination element therefore passes when hardware and translation differ by at most
        // 2^-9 * B (F16 destination) or 2^-15 * B (F32) plus one ulp of the result; when B reaches the destination's largest
        // finite value (intermediate overflow decides between inf and a finite or NaN result); or when both are NaN. An inf or
        // NaN input element requires the same result class (NaN = NaN, inf of the same sign).

        static double H2D(uint h) => (double)BitConverter.UInt16BitsToHalf((ushort)h);
        static double Bf2D(uint h) => BitConverter.UInt32BitsToSingle(h << 16);

        /// B and "an input element is inf or NaN" for destination element (row half rh, column nn) of `lane`; `rec(l, r)` reads
        /// register r of lane l of the warp (RZ reads 0).
        public static (double bound, bool special) HmmaInputs(SassInst i, Func<int, int, uint> rec, int lane, int rh, int nn)
        {
            bool k16 = i.Mods.Contains("16816"), f32 = i.Mods.Contains("F32"), bf16 = i.Mods.Contains("BF16");
            int ra = i.Operands[1].Reg, rb = i.Operands[2].Reg, rc = i.Operands[3].Reg;
            uint R(int l, int r, int q) => r >= 255 ? 0 : rec(l, r + q);
            Func<uint, double> cvt = bf16 ? Bf2D : H2D;
            int g = lane >> 2, t = lane & 3;
            double bound = 0; bool special = false;
            void Term(double x) { if (double.IsFinite(x)) bound += Math.Abs(x); else special = true; }
            var a = new double[16]; var b = new double[16];
            for (int tp = 0; tp < 4; tp++)
            {
                for (int q = 0; q < (k16 ? 4 : 2); q++)
                {
                    if ((q & 1) != rh) continue;
                    uint w = R(g * 4 + tp, ra, q);
                    int k = 8 * (q >> 1) + 2 * tp;
                    a[k] = cvt(w & 0xffff); a[k + 1] = cvt(w >> 16);
                }
                for (int q = 0; q < (k16 ? 2 : 1); q++)
                {
                    uint w = R((2 * t + nn) * 4 + tp, rb, q);
                    int k = 8 * q + 2 * tp;
                    b[k] = cvt(w & 0xffff); b[k + 1] = cvt(w >> 16);
                }
            }
            for (int k = 0; k < (k16 ? 16 : 8); k++)
            {
                if (!double.IsFinite(a[k]) || !double.IsFinite(b[k])) special = true;
                else Term(a[k] * b[k]);
            }
            Term(f32 ? BitConverter.UInt32BitsToSingle(R(lane, rc, 2 * rh + nn)) : H2D(R(lane, rc, rh) >> (16 * nn)));
            return (bound, special);
        }

        /// One HMMA destination element (f32 bits, or an f16 in the low 16 bits) under the rule above.
        public static bool HmmaClose(uint hw, uint ours, bool f32, double bound, bool special)
        {
            if (hw == ours) return true;
            double h = f32 ? BitConverter.UInt32BitsToSingle(hw) : H2D(hw), o = f32 ? BitConverter.UInt32BitsToSingle(ours) : H2D(ours);
            if (double.IsNaN(h) && double.IsNaN(o)) return true;
            double max = f32 ? float.MaxValue : 65504.0;
            if (bound >= max) return true;   // also with an inf input: hardware 16816.F16 gave NaN for inf + an overflowing sum
            if (special) return false;
            if (double.IsNaN(h) || double.IsNaN(o)) return false;
            double tol = bound * Math.Pow(2, f32 ? -15 : -9);
            if (double.IsInfinity(h) || double.IsInfinity(o))
            {
                double fin = double.IsInfinity(h) ? o : h, inf = double.IsInfinity(h) ? h : o;
                return !double.IsInfinity(fin) && Math.Sign(fin) == Math.Sign(inf) && Math.Abs(fin) + tol >= max;
            }
            double m = Math.Max(Math.Abs(h), Math.Abs(o));
            double ulp = m == 0 ? 0 : Math.Max(Math.Pow(2, Math.Floor(Math.Log2(m)) - (f32 ? 23 : 10)), f32 ? Math.Pow(2, -149) : Math.Pow(2, -24));
            return Math.Abs(h - o) <= tol + ulp;
        }

        public static bool Same64(ulong hw, ulong ours)
        {
            if (hw == ours) return true;
            bool nan(ulong v) => (v & 0x7fffffffffffffff) > 0x7ff0000000000000;
            return nan(hw) && nan(ours);
        }
    }
}
