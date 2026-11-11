// Reflection section (type 3) of an NVN2 GLSLC output (platform 2): uniform blocks and opaque uniforms with their per-stage
// bindings. Layout read from the game's own outputs (tools/shaders/docs/AMPERE.md section 2b); it has the shape of platform 1's
// GLSLC reflection (string pool + block, uniform, input, output tables), with record sizes 0x6c (block) and 0xa4 (uniform).
//
//   section header (0x98 bytes at output +0xa8 + 0x98*i, type 3), u32 words: [10] block table offset, [11] block count,
//   [13] uniform count, [14] uniform table offset, [15]/[16] input count/offset, [17]/[18] output count/offset,
//   [25] string pool size, [26] string pool offset (all offsets from the section data).
//   block   (0x6c): [1] name offset, [2] name length (with NUL), [11] size in bytes, [12] active variables,
//                   [13] stage mask (bit = NVN stage: 0 vs, 1 ps, 2 gs, 3 tcs, 4 tes, 5 cs), [14 + stage] binding or -1.
//   uniform (0xa4): [0] name offset, [1] name length, [10] type code, [11] block index (-1: opaque), [12] offset in block,
//                   [13] array size, [14] array stride, [15] matrix stride, [17] stage mask, [18 + stage] binding or -1.
// Binding rules seen in the code: uniform block binding b is c[3 + b]; a sampler with binding n is read by a bound TEX/TLD
// with handle word 8 + 2n of c[2] (graphics stages). Compute shaders read texture handles from c[0] with another layout.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;

namespace SpirvHarness
{
    sealed class Nvn2Reflection
    {
        public sealed record Block(string Name, int Size, int StageMask, int[] Bindings);
        public sealed record Uniform(string Name, int Type, int BlockIndex, int Offset, int StageMask, int[] Bindings);

        public readonly List<Block> Blocks = new();
        public readonly List<Uniform> Opaque = new();

        // Type codes of opaque uniforms, from which instruction shapes read them (graphics, handle word 8 + 2n).
        static readonly Dictionary<int, string> TypeNames = new()
        {
            [0x43] = "2D", [0x44] = "3D", [0x45] = "Cube", [0x47] = "2DShadow", [0x49] = "2DArray", [0x4b] = "2DArrayShadow",
            [0x4f] = "Buffer", [0x64] = "UBuffer", [0x87] = "CubeArray", [0x8a] = "CubeArrayShadow",
        };

        public static Nvn2Reflection Read(string glslcPath)
        {
            byte[] d = File.ReadAllBytes(glslcPath);
            if (d.Length < 0xa8 || BitConverter.ToUInt32(d, 0) != 0x19866891) return null;
            int sections = BitConverter.ToInt32(d, 0x64);
            for (int i = 0; i < sections; i++)
            {
                int h = 0xa8 + 0x98 * i;
                if (h + 0x98 > d.Length) break;
                int size = BitConverter.ToInt32(d, h), off = BitConverter.ToInt32(d, h + 4), type = BitConverter.ToInt32(d, h + 8);
                if (type != 3 || off + size > d.Length) continue;
                int W(int k) => BitConverter.ToInt32(d, h + 4 * k);
                Nvn2Reflection r = new();
                int pool = off + W(26);
                string Name(int o, int len) => len <= 1 ? "" : Encoding.ASCII.GetString(d, pool + o, len - 1);
                for (int b = 0; b < W(11); b++)
                {
                    int e = off + W(10) + b * 0x6c;
                    int E(int k) => BitConverter.ToInt32(d, e + 4 * k);
                    r.Blocks.Add(new Block(Name(E(1), E(2)), E(11), E(13), Enumerable.Range(0, 6).Select(s => E(14 + s)).ToArray()));
                }
                for (int u = 0; u < W(13); u++)
                {
                    int e = off + W(14) + u * 0xa4;
                    int E(int k) => BitConverter.ToInt32(d, e + 4 * k);
                    if (E(11) != -1) continue;
                    r.Opaque.Add(new Uniform(Name(E(0), E(1)), E(10), E(11), E(12), E(17), Enumerable.Range(0, 6).Select(s => E(18 + s)).ToArray()));
                }
                return r;
            }
            return null;
        }

        // Buffer sampler type codes. The codes follow the order of GL's sampler type list (0x43 sampler2D ... 0x4f samplerBuffer,
        // 0x5a isamplerBuffer, 0x64 usamplerBuffer, 0x67 image2D); 0x4f and 0x64 are confirmed by the shaders that read them with
        // 1D TLD, 0x5a is inferred from that order (a permutation table read the same way).
        static readonly HashSet<int> BufferSamplerTypes = new() { 0x4f, 0x5a, 0x64 };

        /// The texture handles (translator cbuf slot, handle word) of the buffer samplers bound in one NVN stage. Graphics: c[2] (the
        /// translator's default slot -1) word 8 + 2n. Compute: c[0] table after the 0x10-byte header, the uniform block and storage
        /// descriptors (0x10 bytes each; counts at control +0xb1/+0xb2), then 8 bytes per texture binding.
        public IEnumerable<(int Slot, int Handle)> BufferTextureHandles(int stage, byte[] ctrl)
        {
            foreach (Uniform u in Opaque)
            {
                int n = u.Bindings[stage];
                if (n < 0 || !BufferSamplerTypes.Contains(u.Type)) continue;
                if (stage == 5)
                {
                    if (ctrl == null || ctrl.Length < 0xb8) continue;
                    int tex0 = (0x10 + 0x10 * (ctrl[0xb1] + ctrl[0xb2])) / 4;
                    yield return (0, tex0 + 2 * n);
                }
                else
                {
                    yield return (-1, 8 + 2 * n);
                    yield return (2, 8 + 2 * n);
                }
            }
        }

        /// "ub:Name=c3 tex:Name=b4(2D) ..." for one NVN stage.
        public string Describe(int stage)
        {
            List<string> parts = new();
            foreach (Block b in Blocks.Where(b => b.Bindings[stage] >= 0))
                parts.Add($"ub:{b.Name}=c{3 + b.Bindings[stage]}");
            foreach (Uniform u in Opaque.Where(u => u.Bindings[stage] >= 0))
                parts.Add($"opaque:{u.Name}=b{u.Bindings[stage]}({(TypeNames.TryGetValue(u.Type, out string t) ? t : $"0x{u.Type:x}")})");
            return string.Join(' ', parts);
        }

        /// Differences between what the translated code binds and what the reflection declares (graphics bank rules).
        public IEnumerable<string> Check(int stage, Ryujinx.Graphics.Shader.ShaderProgramInfo info)
        {
            HashSet<int> blockSlots = Blocks.Where(b => b.Bindings[stage] >= 0).Select(b => 3 + b.Bindings[stage]).ToHashSet();
            foreach (var cb in info.CBuffers)
            {
                if (cb.Slot >= 3 && !blockSlots.Contains(cb.Slot)) yield return $"reflection: c[{cb.Slot}] used, no uniform block bound there";
            }
            if (stage == 5) yield break;   // compute: texture handles in c[0], layout not known
            HashSet<int> samplers = Opaque.Where(u => u.Bindings[stage] >= 0).Select(u => u.Bindings[stage]).ToHashSet();
            foreach (var t in info.Textures)
            {
                if (t.CbufSlot is not (-1 or 2)) continue;
                int n = (t.HandleIndex - 8) / 2;
                if (t.HandleIndex < 8 || (t.HandleIndex & 1) != 0 || !samplers.Contains(n))
                    yield return $"reflection: texture handle word {t.HandleIndex} of c[2] matches no sampler binding";
            }
        }
    }
}
