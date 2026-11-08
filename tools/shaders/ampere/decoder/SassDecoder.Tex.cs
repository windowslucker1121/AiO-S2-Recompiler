// SPDX-License-Identifier: MIT
// Texture and surface ops. Opcodes (bound 0xb6x, bindless 0x36x), handle c-slot 40..59 (offset/4 in 40..54, index 54..59),
// scalar 60, dim 61..64, channel mask 72..76, AOFFI 76, NDV 77, DC 78, LOD mode 87..90, NODEP 90, TLD4 component 87..89,
// TXQ query 62..64, surface dim/mask: [NAK] OpTex/OpTld/OpTld4/OpTmml/OpTxd/OpTxq/OpSuLd/OpSuSt.
// Printed operand order (second destination R64 first), mod spellings (SCR, LZ/LL, dims, TEX_HEADER_*), bound SULD/SUST
// opcodes 0xb97/0xb9b: [oracle].
using System;

namespace Nx.Shaders.Ampere
{
    public static partial class SassDecoder
    {
        static readonly string[] TexDim = { "1D", "2D", "3D", "CUBE", "ARRAY_1D", "ARRAY_2D", "INVALID6", "ARRAY_CUBE" };
        // TLD and TXD have no cube, TXD no 3D [xcheck probe]
        static readonly string[] TexDimTld = { "1D", "2D", "3D", "INVALID3", "ARRAY_1D", "ARRAY_2D", "INVALID6", "INVALID7" };
        static readonly string[] TexDimTxd = { "1D", "2D", "INVALID2", "INVALID3", "ARRAY_1D", "ARRAY_2D", "INVALID6", "INVALID7" };
        static readonly string?[] TexLod = { null, "LZ", "LB", "LL", "LC", "LB.LC", "?lod6", "?lod7" };
        static readonly string[] ImgDim = { "1D", "1D_BUFFER", "1D_ARRAY", "2D", "2D_ARRAY", "3D", "INVALID6", "INVALID7" };

        static void TexHandle(Ctx c, bool bindless)
        {
            if (bindless) return;
            if ((c.Lo & 0xfff) == 0xf60)
            {
                // TEX with the handle in a uniform register pair [probe, nvdisasm 13.3]: URn at 40..45, then an 8-bit immediate at 46..53
                // ("UR10, 0x0"); 54..59 do nothing. NVN2 shaders load URn = handle, URn+1 = handle >> 20 (the sampler index).
                c.Add(c.UR(40), c.Hex(46, 8));
                return;
            }
            c.Add(c.Hex(54, 5), c.Imm((long)c.F(40, 14), SassText.Hex((long)c.F(40, 14))));
        }

        static void TexMask(Ctx c)
        {
            if (c.I(72, 4) != 0xf) c.Add(c.Hex(72, 4));
        }

        /// TEX / TLD / TLD4 / TMML / TXD common shape: R64, R16, R24, [R32], handle, dim, [mask]
        static void TexCommon(Ctx c, string op, bool bindless, Action<Ctx> mods, string[]? dims = null)
        {
            c.Op = op;
            c.Mod(c.B(60), "SCR");
            c.Mod(bindless, "B");
            mods(c);
            c.Mod(84, 3, Evict);
            c.Mod(c.B(90), "NODEP");
            c.Dst = 2;
            c.Add(c.R(64, reuse: false), c.R(16, reuse: false));
            if (!IsPT(c, 81)) { c.Add(c.P(81)); c.Dst++; }
            c.Add(c.R(24, reuse: false));
            if (c.I(32, 8) != 255) c.Add(c.R(32, reuse: false));
            TexHandle(c, bindless);
            c.Add(c.Text((dims ?? TexDim)[c.I(61, 3)]));
            TexMask(c);
        }

        static void Tmml(Ctx c, bool bindless)
        {
            c.Op = "TMML"; c.Mod(bindless, "B"); c.Mod("LOD"); c.Mod(c.B(77), "NDV"); c.Mod(c.B(90), "NODEP");
            c.Dst = 2;
            c.Add(c.R(64, reuse: false), c.R(16, reuse: false), c.R(24, reuse: false));
            if (c.I(32, 8) != 255) c.Add(c.R(32, reuse: false));
            TexHandle(c, bindless);
            c.Add(c.Text(TexDim[c.I(61, 3)]));
            TexMask(c);
        }

        static void RegisterTex()
        {
            // TEX bits 79..80: .F16.RN / .F16.RZ (fp16 results packed in pairs), 3 is reserved [xcheck probe; 2 Pokopia TEX.SCR.F16.RN words]
            Action<Ctx> texMods = c => { c.Mod(79, 2, null, "F16.RN", "F16.RZ", "INVALID3"); c.Mod(87, 3, TexLod); c.Mod(c.B(76), "AOFFI"); c.Mod(c.B(78), "DC"); c.Mod(c.B(77), "NDV"); };
            // TLD, TLD4 and TXD have the same .F16.RN / .F16.RZ field at 79..80 [nvdisasm 13.3 probe, 2026-11-08]: printed after SCR
            // (TLD: before the LOD mode; TLD4: after the component, before PTP/AOFFI; TXD: first). It was missing here, so OoT's
            // upscaler f90d726d / 02dcb888 ("TLD4.SCR.R.F16.RN.PTP R13, R12, R10, R0, 0x0, 0x30, 2D") were translated as fp32 gathers
            // that wrote four registers (Rd, Rd+1, Rd2, Rd2+1) instead of two half pairs and clobbered live registers.
            Action<Ctx> f16Mod = c => c.Mod(79, 2, null, "F16.RN", "F16.RZ", "INVALID3");
            Action<Ctx> tldMods = c => { f16Mod(c); c.Mod(87, 3, TexLod); c.Mod(c.B(76), "AOFFI"); c.Mod(c.B(78), "MS"); c.Mod(c.B(77), "CL"); };
            Action<Ctx> tld4Mods = c => { c.Mod(87, 2, "R", "G", "B", "A"); f16Mod(c); c.Mod(76, 2, null, "AOFFI", "PTP", "?3"); c.Mod(c.B(78), "DC"); };
            Action<Ctx> txdMods = c => { f16Mod(c); c.Mod(c.B(76), "AOFFI"); };
            Op12(0xb60, c => TexCommon(c, "TEX", false, texMods));
            Op12(0x361, c => TexCommon(c, "TEX", true, texMods));
            Op12(0xf60, c => TexCommon(c, "TEX", false, texMods));   // handle in URn (TexHandle); bit 91 set in every word seen
            Op12(0xb66, c => TexCommon(c, "TLD", false, tldMods, TexDimTld));
            Op12(0x367, c => TexCommon(c, "TLD", true, tldMods, TexDimTld));
            Op12(0xb63, c => TexCommon(c, "TLD4", false, tld4Mods));
            Op12(0x364, c => TexCommon(c, "TLD4", true, tld4Mods));
            // TMML [probe]: always .LOD, no SCR, no eviction field and no residency predicate (bits 60, 81, 84..86 do nothing); .NDV 77, .NODEP 90.
            // Printed as "TMML[.B].LOD[.NDV][.NODEP] Rd2, Rd, Ra, [Rb,] bank, handle, dim, mask"; it used to be decoded like TEX, which made bits 81 and 84..86
            // a P0 destination and an eviction hint.
            Op12(0xb69, c => Tmml(c, false));
            Op12(0x36a, c => Tmml(c, true));
            Op12(0xb6c, c => TexCommon(c, "TXD", false, txdMods, TexDimTxd));
            Op12(0x36d, c => TexCommon(c, "TXD", true, txdMods, TexDimTxd));
            // TXQ: R64, R16, R24, query, handle, mask
            Action<Ctx> txq = c =>
            {
                bool bl = (c.Lo & 0xfff) == 0x370;
                c.Op = "TXQ"; c.Mod(bl, "B"); c.Mod(c.B(90), "NODEP");
                c.Dst = 2; c.Add(c.R(64, reuse: false), c.R(16, reuse: false), c.R(24, reuse: false));
                c.Add(c.Text(c.I(62, 2) switch { 0 => "TEX_HEADER_DIMENSION", 1 => "TEX_HEADER_TEXTURE_TYPE", 2 => "TEX_HEADER_SAMPLER_POS", _ => "?q3" }));
                TexHandle(c, bl);
                TexMask(c);
            };
            Op12(0xb6f, txq); Op12(0x370, txq);

            // surfaces: bound SULD 0xb97 / SUST 0xb9b [oracle], bindless 0x998/0x99a, 0x99c/0x99e [NAK]
            Op12(0xb97, c => Surface(c, true, false));
            Op12(0x998, c => Surface(c, true, true));
            Op12(0xb9b, c => Surface(c, false, false));
            // SURED 0x59f (surface reduction; one Mario Kart World instruction): SURED.D[.BA].<dim>.<op>[.<type>][.<evict>].<order>[.IGN|.TRAP] [Ra], Rb, slot, handle
            // op 87..89 as ATOM, type 73..75, 59..60 = 0 IGN, 2 TRAP [xcheck probe]
            Op12(0x59f, c =>
            {
                c.Op = "SURED"; c.Mod("D"); c.Mod(c.B(72), "BA"); c.Mod(ImgDim[c.I(61, 3)]);
                c.Mod(87, 3, AtomOp); c.Mod(73, 3, null, "S32", "U64", "?3", "F16x2.RN", "?5", "?6", "?7");
                c.Mod(84, 3, Evict); MemOrder(c); c.Mod(59, 2, "IGN", null, "TRAP", "?3");
                c.Dst = 0; c.Add(MemOp(c, c.I(24, 8), null, -1, 0), c.R(32)); TexHandle(c, false);
            });
            Op12(0x99c, c => Surface(c, false, true));
            // SURED 0x59f [probe] (Mario Kart World ee145ba8782ff91f@0xc0: "SURED.D.2D.ADD.STRONG.GPU.IGN [R4], R0, 0x0, 0xc"): BA 72, shape
            // 61..63, op 87..89 (atomic op table), type 73..75 (SuatomSize order), evict 84..86, memory order 77..80, clamp 59..60
            // (0 IGN, 1 none, 2 TRAP); operands [Ra] (coordinates), Rb (data), bank 54..58, handle 40..53
            Op12(0x59f, c =>
            {
                c.Op = "SURED"; c.Mod("D"); c.Mod(c.B(72), "BA"); c.Mod(ImgDim[c.I(61, 3)]);
                c.Mod(87, 3, AtomOp); c.Mod(73, 3, SuredType); c.Mod(84, 3, Evict); MemOrder(c);
                c.Mod(59, 2, "IGN", null, "TRAP", "INVALID3");
                c.Dst = 0; c.Add(MemOp(c, c.I(24, 8), null, -1, 0), c.R(32));
                TexHandle(c, false);
            });
        }

        static readonly string?[] SuredType = { null, "S32", "U64", "F32.FTZ.RN", "F16x2.RN", "S64", "SD32", "SD64" };

        static string Channels(int m)
        {
            string s = "";
            if ((m & 1) != 0) s += "R";
            if ((m & 2) != 0) s += "G";
            if ((m & 4) != 0) s += "B";
            if ((m & 8) != 0) s += "A";
            return s;
        }

        static void Surface(Ctx c, bool load, bool bindless)
        {
            c.Op = load ? "SULD" : "SUST";
            c.Mod("P"); c.Mod(bindless, "B");
            c.Mod(ImgDim[c.I(61, 3)]);
            int m = c.I(72, 4); if (m != 0xf) c.Mod(Channels(m));
            MemOrder(c);
            c.Mod(84, 3, Evict);
            c.Mod("IGN");
            if (load)
            {
                c.Dst = 1; c.Add(c.R(16));
                if (!IsPT(c, 81)) { c.Add(c.P(81)); c.Dst++; }
                c.Add(MemOp(c, c.I(24, 8), null, -1, 0));
            }
            else { c.Dst = 0; c.Add(MemOp(c, c.I(24, 8), null, -1, 0), c.R(32)); }
            if (bindless) c.Add(c.R(64)); else TexHandle(c, false);
        }
    }
}
