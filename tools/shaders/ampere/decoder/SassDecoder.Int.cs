// SPDX-License-Identifier: MIT
// Integer ALU. Opcodes and fields: [NAK] OpIAdd3/OpIAdd3X/OpIMad/OpIMad64/OpISetP/OpLea/OpLeaX/OpLop3/OpPLop3/OpShf/OpPrmt/
// OpSel/OpIMnMx/OpIAbs/OpFlo/OpPopC/OpBRev/OpBMsk/OpSgxt/OpMov. Printed operand order (optional predicate destinations,
// carry-in predicates), IMAD aliases (MOV/SHL/IADD/U32), immediate signedness, P2R/R2P: [oracle].
namespace Nx.Shaders.Ampere
{
    public static partial class SassDecoder
    {
        static readonly string?[] PrmtMode = { null, "F4E", "B4E", "RC8", "ECL", "ECR", "RC16", "?7" };

        static void IntSrcMods(Ctx c, bool negA = true, bool negB = true, bool negC = true)
        {
            c.AAbs = -1; c.BAbs = -1; c.CAbs = -1;
            c.ANeg = negA ? 72 : -1; c.BNeg = negB ? 63 : -1; c.CNeg = negC ? 75 : -1;
        }

        static void RegisterInt()
        {
            // MOV [NAK] 0x002: source in the 32..64 slot, quad-lane mask 72..76 printed when not 0xf
            Op9(0x002, c =>
            {
                c.Op = "MOV"; IntSrcMods(c, false, false, false); c.Dst = 1;
                c.Add(c.D(), c.Src2());
                if (!c.U && c.I(72, 4) != 0xf) c.Add(c.Hex(72, 4));
            });
            // SEL [NAK] 0x007
            Op9(0x007, c => { c.Op = "SEL"; IntSrcMods(c, false, false, false); c.ImmK = ImmKind.Hex; c.Dst = 1; c.Add(c.D(), c.SrcA(), c.Src2(), c.Pr(87, 90)); });
            // IADD3 [NAK] 0x010: .X bit 74, carry-out predicates 81/84, carry-in 87/77 (printed with .X)
            Op9(0x010, c =>
            {
                c.Op = "IADD3"; bool x = c.B(74); c.Mod(x, "X"); IntSrcMods(c); c.ImmK = ImmKind.SHex;
                c.Add(c.D()); c.Dst = 1;
                if (!IsPT(c, 81)) { c.Add(c.Pr(81)); c.Dst++; }
                if (!IsPT(c, 84)) { c.Add(c.Pr(84)); c.Dst++; }
                c.Add(c.SrcA()); var (b, d) = c.SrcBC(); c.Add(b, d);
                if (x) c.Add(c.Pr(87, 90), c.Pr(77, 80));
            });
            // LEA [NAK] 0x011: .HI 80, .X 74, shift 75..80, neg A 72, carry-out 81, carry-in 87 (.X). .HI with bit 73 is .SX32 (sign-extended 32-bit high
            // part): no third source is printed [xcheck probe, title A LEA.HI.SX32 R12, R5, R5, 0x19]
            Op9(0x011, c =>
            {
                c.Op = "LEA"; bool hi = c.B(80), x = c.B(74), sx = hi && c.B(73); c.Mod(hi, "HI"); c.Mod(x, "X"); c.Mod(sx, "SX32"); IntSrcMods(c, true, true, false); c.ImmK = ImmKind.Hex;
                c.Add(c.D()); c.Dst = 1;
                if (!IsPT(c, 81)) { c.Add(c.Pr(81)); c.Dst++; }
                c.Add(c.SrcA());
                var (b, d) = c.SrcBC(); c.Add(b); if (hi && !sx) c.Add(d);
                c.Add(c.Hex(75, 5));
                if (x) c.Add(c.Pr(87, 90));
            });
            // LOP3 [NAK] 0x012: lut 72..80, predicate destination 81 (printed first when not PT), predicate 87/90
            Op9(0x012, c =>
            {
                c.Op = "LOP3"; c.Mod(c.B(80) ? "PAND" : "LUT"); IntSrcMods(c, false, false, false);
                c.Dst = 1;
                if (!IsPT(c, 81)) { c.Add(c.Pr(81)); c.Dst++; }
                c.Add(c.D(), c.SrcA()); var (b, d) = c.SrcBC(); c.Add(b, d);
                c.Add(c.Hex(72, 8), c.Pr(87, 90));
            });
            // IABS [NAK] 0x013, FLO 0x100, BREV 0x101, POPC 0x109
            Op9(0x013, c => { c.Op = "IABS"; IntSrcMods(c, false, false, false); c.Dst = 1; c.Add(c.D(), c.Src2()); });
            Op9(0x100, c =>
            {
                c.Op = "FLO"; c.Mod(!c.B(73), "U32"); c.Mod(c.B(74), "SH"); IntSrcMods(c, false, false, false); c.BNeg = -1;
                c.Dst = 1; c.Add(c.D());
                if (!IsPT(c, 81)) { c.Add(c.Pr(81)); c.Dst++; }
                var s = c.Src2(); if (c.B(63)) s = s with { Not = true }; c.Add(s);
            });
            Op9(0x101, c => { c.Op = "BREV"; IntSrcMods(c, false, false, false); c.Dst = 1; c.Add(c.D(), c.Src2()); });
            Op9(0x109, c =>
            {
                c.Op = "POPC"; IntSrcMods(c, false, false, false); c.BNeg = -1; c.Dst = 1;
                var s = c.Src2(); if (c.B(63)) s = s with { Not = true }; c.Add(c.D(), s);
            });
            // PRMT [NAK] 0x016: mode 72..75
            Op9(0x016, c => { c.Op = "PRMT"; c.Mod(72, 3, PrmtMode); IntSrcMods(c, false, false, false); c.Dst = 1; c.Add(c.D(), c.SrcA()); var (b, d) = c.SrcBC(); c.Add(b, d); });
            // IMNMX [NAK] 0x017: signed bit 73
            Op9(0x017, c => { c.Op = "IMNMX"; c.Mod(!c.B(73), "U32"); IntSrcMods(c, false, false, false); c.ImmK = ImmKind.SHex; c.Dst = 1; c.Add(c.D(), c.SrcA(), c.Src2(), c.Pr(87, 90)); });
            // SHF [NAK] 0x019: right 76, wrap 75, type 73..75 (S64 U64 S32 U32), hi 80
            Op9(0x019, c =>
            {
                c.Op = "SHF"; c.Mod(c.B(76) ? "R" : "L"); c.Mod(c.B(75), "W"); c.Mod(73, 2, "S64", "U64", "S32", "U32"); c.Mod(c.B(80), "HI");
                IntSrcMods(c, false, false, false); c.Dst = 1; c.Add(c.D(), c.SrcA()); var (b, d) = c.SrcBC(); c.Add(b, d);
            });
            // SGXT [NAK] 0x01a, BMSK 0x01b
            Op9(0x01a, c => { c.Op = "SGXT"; c.Mod(c.B(75), "W"); c.Mod(!c.B(73), "U32"); IntSrcMods(c, false, false, false); c.Dst = 1; c.Add(c.D(), c.SrcA(), c.Src2()); });
            Op9(0x01b, c => { c.Op = "BMSK"; c.Mod(c.B(75), "W"); IntSrcMods(c, false, false, false); c.Dst = 1; c.Add(c.D(), c.SrcA(), c.Src2()); });
            // ISETP [NAK] 0x00c: cmp 76..79, signed 73, .EX 72 (adds 68/71 predicate, printed after the set op [probe]), set op 74..76
            Op9(0x00c, c =>
            {
                c.Op = "ISETP"; c.Mod(76, 3, ICmp); c.Mod(!c.B(73), "U32"); bool ex = c.B(72); c.Mod(74, 2, PSet); c.Mod(ex, "EX");
                IntSrcMods(c, false, false, false); c.ImmK = ImmKind.SHex; c.Dst = 2;
                c.Add(c.Pr(81), c.Pr(84), c.SrcA(), c.Src2(), c.Pr(87, 90));
                if (ex) c.Add(c.Pr(68, 71));
            });
            // IMAD [NAK] 0x024 (and .WIDE 0x025, .HI 0x027): signed 73, .X 74 with carry-in 87
            Op9(0x024, c => Imad(c, ""));
            Op9(0x025, c => Imad(c, "WIDE"));
            Op9(0x027, c => Imad(c, "HI"));
            // PLOP3 [NAK] 0x81c: luts 64..67+72..77 and 16..24, sources 87, 77, 68 (67 = UP source)
            Op12(0x81c, c =>
            {
                c.Op = "PLOP3"; c.Mod("LUT"); c.Dst = 2;
                c.Add(c.Pr(81), c.Pr(84), c.Pr(87, 90), c.Pr(77, 80));
                c.Add(c.B(67) ? c.UP(68, 71) : c.Pr(68, 71));
                long lut = (long)(c.F(64, 3) | (c.F(72, 5) << 3));
                c.Add(c.Imm(lut, SassText.Hex(lut)), c.Hex(16, 8));
            });
            // I2I 0x038 [probe]: destination type 76..77 (U8 S8 U16 S16), source in the b slot; S32 and SAT always printed
            Op9(0x038, c =>
            {
                c.Op = "I2I"; c.Mod(76, 2, "U8", "S8", "U16", "S16"); c.Mod("S32"); c.Mod("SAT");
                IntSrcMods(c, false, false, false); c.Dst = 1; c.Add(c.D(), c.Src2());
            });
            // IDP 0x026 [probe]: 2A 76 (.LO, .HI 77), a signed 73, b signed 74, c negated 75
            Op9(0x026, c =>
            {
                c.Op = "IDP"; bool two = c.B(76);
                if (two) { c.Mod("2A"); c.Mod(c.B(77) ? "HI" : "LO"); c.Mod(c.B(73) ? "S16" : "U16"); }
                else { c.Mod("4A"); c.Mod(c.B(73) ? "S8" : "U8"); }
                c.Mod(c.B(74) ? "S8" : "U8");
                IntSrcMods(c, false, false, true); c.Dst = 1;
                c.Add(c.D(), c.SrcA()); var (b, d) = c.SrcBC(); c.Add(b, d);
            });
            // VABSDIFF 0x014 [probe]: unsigned when 73 clear; predicate destination 81 printed after Rd when not PT
            Op9(0x014, c =>
            {
                c.Op = "VABSDIFF"; c.Mod(!c.B(73), "U32"); IntSrcMods(c, false, false, false);
                c.Add(c.D()); c.Dst = 1;
                if (!IsPT(c, 81)) { c.Add(c.Pr(81)); c.Dst++; }
                c.Add(c.SrcA()); var (b, d) = c.SrcBC(); c.Add(b, d);
            });
            // P2R 0x803 / R2P 0x804 [oracle]: "P2R Rd, PR, Ra, mask", "R2P PR, Ra, mask"
            Op9(0x003, c => { c.Op = "P2R"; c.Mod(c.I(76, 2) is var s && s != 0, "B" + c.I(76, 2)); IntSrcMods(c, false, false, false); c.Dst = 1; c.Add(c.D(), c.Text(c.U ? "UPR" : "PR", OperandKind.Special), c.SrcA(), c.Src2()); });
            Op9(0x004, c =>
            {
                // byte select 76..77: P2R.B1 Rd, PR, Ra, mask / R2P PR, Ra.B1, mask [xcheck probe on P2R/R2P words; was read from 74..75]
                c.Op = "R2P"; IntSrcMods(c, false, false, false); c.Dst = 1;
                var a = c.SrcA(); int sel = c.I(76, 2);
                // nvdisasm puts .reuse before the byte select here: R6.reuse.B1
                if (sel != 0) a = a with { Suffixes = new[] { (a.Suffixes != null && System.Linq.Enumerable.Contains(a.Suffixes, ".reuse")) ? ".reuse.B" + sel : ".B" + sel } };
                c.Add(c.Text("PR", OperandKind.Special), a, c.Src2());
            });
        }

        static bool IsRZ(Operand o) => o.Kind == OperandKind.Reg && o.Reg == 255 && !o.Neg;

        static void Imad(Ctx c, string kind)
        {
            c.Op = "IMAD"; IntSrcMods(c); c.ImmK = ImmKind.SHex;
            bool signed = c.B(73), x = c.B(74);
            var a = c.SrcA(); var (b, d) = c.SrcBC();
            if (kind.Length > 0) { c.Mod(kind); c.Mod(!signed, "U32"); c.Mod(x, "X"); }
            else if (x) { c.Mod(!signed, "U32"); c.Mod("X"); } // IMAD.U32.X [oracle]
            else if (IsRZ(a) && IsRZ(b) && c.Form != 7) { c.Mod("MOV"); c.Mod(!signed, "U32"); }
            // [probe] register form: either multiplicand RZ prints the MOV alias ("IMAD.MOV.U32 R10, RZ, R6, R3")
            // and a negated b after an RZ a is printed without its sign ("IMAD.MOV R7, RZ, R6, RZ"; the product is 0 either way)
            else if (c.Form == 1 && (IsRZ(a) || IsRZ(b))) { c.Mod("MOV"); c.Mod(!signed, "U32"); if (IsRZ(a)) b = b with { Neg = false }; }
            else if (!signed && b.Kind == OperandKind.Imm && IsRZ(d) && (b.Offset == 0 || b.Offset == 1)) { c.Mod("MOV"); c.Mod("U32"); }
            // [probe] findings/imad_shl_pow2.tsv: SHL for 2^1..2^30 except 2^16
            else if (!signed && b.Kind == OperandKind.Imm && IsRZ(d) && b.Offset > 1 && b.Offset < 0x80000000L && b.Offset != 0x10000 && (b.Offset & (b.Offset - 1)) == 0) { c.Mod("SHL"); c.Mod("U32"); }
            else if (b.Kind == OperandKind.Imm && b.Offset == 1) { c.Mod("IADD"); c.Mod(!signed, "U32"); }
            else c.Mod(!signed, "U32");
            c.Add(c.D()); c.Dst = kind == "WIDE" ? 1 : 1;
            if (!IsPT(c, 81)) { c.Add(c.Pr(81)); c.Dst++; }
            c.Add(a, b, d);
            if (x) c.Add(c.Pr(87, 90));
        }
    }
}
