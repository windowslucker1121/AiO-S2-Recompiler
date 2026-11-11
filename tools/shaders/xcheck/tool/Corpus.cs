// SPDX-License-Identifier: MIT
// Corpus access shared with sassdecode's census: title A layout (unique.tsv + code/ + ctrl/) or runtime dump layout
// (manifest.tsv + code_<h>.bin + ctrl_<h>.bin).
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;

namespace Nx.Shaders.XCheck
{
    sealed record CorpusBlock(string Hash, string CodePath, string CtrlPath);

    static class Corpus
    {
        public static List<CorpusBlock> Rows(string dir)
        {
            var rows = new List<CorpusBlock>();
            string uniq = Path.Combine(dir, "unique.tsv"), man = Path.Combine(dir, "manifest.tsv");
            if (File.Exists(uniq))
            {
                foreach (var l in File.ReadLines(uniq).Skip(1))
                {
                    var f = l.Split('\t');
                    rows.Add(new CorpusBlock(f[0], Path.Combine(dir, "code", f[0] + ".bin"), Path.Combine(dir, "ctrl", f[9] + ".bin")));
                }
            }
            else if (File.Exists(man))
            {
                var seen = new HashSet<string>();
                foreach (var l in File.ReadLines(man).Skip(1))
                {
                    var f = l.Split('\t');
                    if (f.Length < 13 || !seen.Add(f[10])) continue;
                    rows.Add(new CorpusBlock(f[10], Path.Combine(dir, f[11]), Path.Combine(dir, f[12])));
                }
            }
            else throw new FileNotFoundException("no unique.tsv or manifest.tsv in " + dir);
            return rows;
        }

        /// Program range of a block per docs/AMPERE.md: stage u16 ctrl+0x56 (5 = compute, no SPH), end u32 ctrl+0x38, trailing zero words dropped.
        public static bool TryLoad(CorpusBlock b, out byte[] code, out int start, out int end)
        {
            code = Array.Empty<byte>(); start = end = 0;
            byte[] ctrl;
            try { code = File.ReadAllBytes(b.CodePath); ctrl = File.ReadAllBytes(b.CtrlPath); }
            catch (IOException) { return false; }
            if (ctrl.Length < 0x58 || BitConverter.ToUInt32(ctrl, 0) != 0x98761234) return false;
            int stage = BitConverter.ToUInt16(ctrl, 0x56);
            end = Math.Min((int)BitConverter.ToUInt32(ctrl, 0x38), code.Length);
            start = stage == 5 ? 0 : 0x80;
            while (end - 16 >= start && code.AsSpan(end - 16, 16).IndexOfAnyExcept((byte)0) < 0) end -= 16;
            return end > start;
        }
    }
}
