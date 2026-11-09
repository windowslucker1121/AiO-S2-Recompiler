// SPDX-License-Identifier: MIT
// Structurizer for reducible control flow graphs (OoT register pressure, 2026-11-09).
//
// Ryujinx's structurizer (StructuredProgramContext + GotoElimination) turns every branch that does not land inside the current
// if into a goto and then removes the gotos with the "Taming Control Flow" moves. For the SASS of OoT's material shaders (a light
// loop with a BRX switch whose cases leave to a common merge from different nesting depths) that produced five do-while loops nested
// in the light loop, each with a flag condition, and many "if (!flag)" guards; NVIDIA's compiler then needed 235-255 registers for
// shaders whose original code uses 94.
//
// This one follows Ramsey, "Beyond Relooper" (ICFP 2022): the code of a node x of the dominator tree is followed by its merge
// children (nodes with two or more forward in-edges, or entered by an edge that leaves a loop), each after a block that encloses the
// code before it; a forward branch to a merge node leaves that block, a branch to a node x dominates alone continues with that node's
// code inline, a back edge continues its loop. Blocks have no construct of their own here: leaving one from the end of the enclosing
// if/else chain needs nothing; leaving it from further inside sets a flag that guards the statements up to the block's end
// ("if (!flag) { ... }", the flag cleared where the block begins), and crossing a loop on the way breaks out of it ("flag = true;
// break;" and "if (flag) break;" after each loop crossed). A continue of an outer loop uses a flag the same way. Loops are
// "do { body; break; } while (true)", or "do { body } while (c)" when the body ends with its only continue under the condition c.
// Flags that no statement reads are removed again.
//
// Works on the blocks after phi removal (copies at the end of the predecessors). Falls back to Ryujinx's structurizer for an
// irreducible graph. Off: NXRT_HARNESS_STRUCTURIZER=ryujinx.
using Ryujinx.Graphics.Shader.IntermediateRepresentation;
using Ryujinx.Graphics.Shader.Translation;
using System;
using System.Collections.Generic;
using static Ryujinx.Graphics.Shader.StructuredIr.AstHelper;

namespace Ryujinx.Graphics.Shader.StructuredIr
{
    static partial class CfgStructurizer
    {
        public static readonly bool Enabled = Environment.GetEnvironmentVariable("NXRT_HARNESS_STRUCTURIZER") != "ryujinx";

        // ---- the structured tree ----

        private abstract class S { }

        private sealed class SCode : S { public int Block; }

        private sealed class SIf : S
        {
            public int Block;          // the block whose terminator this is
            public Operation Op;       // conditional branch
            public List<S> Then;       // runs when the branch condition (source 0) is true
            public List<S> Else;
        }

        private sealed class SLoop : S { public int Header; public List<S> Body; }

        private sealed class SBlock : S { public int Follow; public List<S> Body; }

        private sealed class SBr : S { public int Target; public bool Continue; }

        private sealed class Graph
        {
            public BasicBlock[] Blocks;
            public int N;
            public int[] Rpo;
            public int[] RpoNum;
            public int[] Idom;
            public List<int>[] Preds;
            public List<int>[] DomChildren;
            public bool[] IsHeader;
            public HashSet<int>[] LoopBody;  // by header
            public bool[] IsMerge;
        }

        public static bool TryEmit(BasicBlock[] blocks, StructuredProgramContext context, Action<Operation> addOperation)
        {
            if (!Enabled || blocks.Length == 0)
            {
                return false;
            }

            Graph g = Analyse(blocks);

            if (g == null)
            {
                return false;
            }

            List<S> tree;

            try
            {
                tree = DoTree(g, 0);
                Validate(g, tree, []);
            }
            catch (InvalidOperationException e)
            {
                if (Environment.GetEnvironmentVariable("NXRT_HARNESS_STRUCTURIZER") == "log")
                {
                    Console.Error.WriteLine($"structurizer: fallback ({e.Message})");
                }

                return false;
            }

            Lowering lowering = new(g, context, addOperation);
            lowering.Run(tree, context.CurrentFunction.MainBlock);

            return true;
        }

        private static IEnumerable<int> Succ(BasicBlock b)
        {
            if (b.Next != null)
            {
                yield return b.Next.Index;
            }

            if (b.Branch != null && b.Branch != b.Next)
            {
                yield return b.Branch.Index;
            }
        }

        private static Graph Analyse(BasicBlock[] blocks)
        {
            int n = blocks.Length;

            for (int i = 0; i < n; i++)
            {
                if (blocks[i].Index != i)
                {
                    return null;
                }
            }

            Graph g = new() { Blocks = blocks, N = n };

            // Reverse post order.
            bool[] visited = new bool[n];
            List<int> post = new(n);
            Stack<(int, IEnumerator<int>)> stack = new();
            visited[0] = true;
            stack.Push((0, Succ(blocks[0]).GetEnumerator()));

            while (stack.Count != 0)
            {
                var (node, it) = stack.Peek();

                if (it.MoveNext())
                {
                    int s = it.Current;

                    if (!visited[s])
                    {
                        visited[s] = true;
                        stack.Push((s, Succ(blocks[s]).GetEnumerator()));
                    }
                }
                else
                {
                    stack.Pop();
                    post.Add(node);
                }
            }

            if (post.Count != n)
            {
                return null;
            }

            g.Rpo = new int[n];
            g.RpoNum = new int[n];

            for (int i = 0; i < n; i++)
            {
                g.Rpo[i] = post[n - 1 - i];
                g.RpoNum[g.Rpo[i]] = i;
            }

            g.Preds = new List<int>[n];

            for (int i = 0; i < n; i++)
            {
                g.Preds[i] = [];
            }

            for (int i = 0; i < n; i++)
            {
                foreach (int s in Succ(blocks[i]))
                {
                    g.Preds[s].Add(i);
                }
            }

            // Dominators.
            int[] idom = new int[n];
            Array.Fill(idom, -1);
            idom[0] = 0;

            for (bool changed = true; changed;)
            {
                changed = false;

                for (int k = 1; k < n; k++)
                {
                    int b = g.Rpo[k];
                    int d = -1;

                    foreach (int p in g.Preds[b])
                    {
                        if (idom[p] != -1)
                        {
                            d = d == -1 ? p : Intersect(idom, g.RpoNum, p, d);
                        }
                    }

                    if (d != -1 && idom[b] != d)
                    {
                        idom[b] = d;
                        changed = true;
                    }
                }
            }

            g.Idom = idom;
            g.DomChildren = new List<int>[n];

            for (int i = 0; i < n; i++)
            {
                g.DomChildren[i] = [];
            }

            for (int i = 1; i < n; i++)
            {
                g.DomChildren[idom[i]].Add(i);
            }

            // Back edges (must target a dominator: reducible), natural loops.
            g.IsHeader = new bool[n];
            g.LoopBody = new HashSet<int>[n];

            for (int u = 0; u < n; u++)
            {
                foreach (int v in Succ(blocks[u]))
                {
                    if (g.RpoNum[v] > g.RpoNum[u])
                    {
                        continue;
                    }

                    if (!Dominates(g, v, u))
                    {
                        return null; // irreducible
                    }

                    g.IsHeader[v] = true;
                    HashSet<int> body = g.LoopBody[v] ??= [v];
                    Stack<int> work = new();

                    if (body.Add(u))
                    {
                        work.Push(u);
                    }

                    while (work.Count != 0)
                    {
                        int x = work.Pop();

                        foreach (int p in g.Preds[x])
                        {
                            if (body.Add(p))
                            {
                                work.Push(p);
                            }
                        }
                    }
                }
            }

            // Merge nodes: two or more forward in-edges, or an in-edge that leaves a loop.
            g.IsMerge = new bool[n];

            for (int v = 0; v < n; v++)
            {
                int forward = 0;

                foreach (int p in g.Preds[v])
                {
                    if (IsBackEdge(g, p, v))
                    {
                        continue;
                    }

                    forward++;

                    for (int h = 0; h < n; h++)
                    {
                        if (g.IsHeader[h] && g.LoopBody[h].Contains(p) && !g.LoopBody[h].Contains(v))
                        {
                            g.IsMerge[v] = true;
                        }
                    }
                }

                if (forward >= 2)
                {
                    g.IsMerge[v] = true;
                }
            }

            return g;
        }

        private static int Intersect(int[] idom, int[] rpoNum, int a, int b)
        {
            while (a != b)
            {
                while (rpoNum[a] > rpoNum[b])
                {
                    a = idom[a];
                }

                while (rpoNum[b] > rpoNum[a])
                {
                    b = idom[b];
                }
            }

            return a;
        }

        private static bool Dominates(Graph g, int a, int b)
        {
            while (true)
            {
                if (a == b)
                {
                    return true;
                }

                if (b == 0)
                {
                    return false;
                }

                b = g.Idom[b];
            }
        }

        private static bool IsBackEdge(Graph g, int u, int v)
        {
            return g.RpoNum[v] <= g.RpoNum[u];
        }

        // ---- Ramsey's translation ----

        private static List<int> MergeChildren(Graph g, int x)
        {
            List<int> ys = [];

            foreach (int c in g.DomChildren[x])
            {
                if (g.IsMerge[c])
                {
                    ys.Add(c);
                }
            }

            ys.Sort((a, b) => g.RpoNum[b].CompareTo(g.RpoNum[a])); // highest first = outermost

            return ys;
        }

        private static List<S> DoTree(Graph g, int x)
        {
            List<int> ys = MergeChildren(g, x);

            if (g.IsHeader[x])
            {
                List<int> inside = [];
                List<int> outside = [];

                foreach (int y in ys)
                {
                    (g.LoopBody[x].Contains(y) ? inside : outside).Add(y);
                }

                List<S> loop = [new SLoop { Header = x, Body = NodeWithin(g, x, inside, 0) }];

                return Wrap(g, loop, outside, 0);
            }

            return NodeWithin(g, x, ys, 0);
        }

        private static List<S> Wrap(Graph g, List<S> inner, List<int> ys, int i)
        {
            if (i == ys.Count)
            {
                return inner;
            }

            int y = ys[i];
            List<S> result = [new SBlock { Follow = y, Body = Wrap(g, inner, ys, i + 1) }];
            result.AddRange(DoTree(g, y));

            return result;
        }

        private static List<S> NodeWithin(Graph g, int x, List<int> ys, int i)
        {
            if (i == ys.Count)
            {
                List<S> code = [new SCode { Block = x }];
                code.AddRange(Terminator(g, x));

                return code;
            }

            int y = ys[i];
            List<S> result = [new SBlock { Follow = y, Body = NodeWithin(g, x, ys, i + 1) }];
            result.AddRange(DoTree(g, y));

            return result;
        }

        private static List<S> Terminator(Graph g, int x)
        {
            BasicBlock b = g.Blocks[x];
            Operation last = b.GetLastOp() as Operation;

            if (last != null && last.Inst is Instruction.BranchIfTrue or Instruction.BranchIfFalse && b.Branch != b.Next)
            {
                int whenTrue = last.Inst == Instruction.BranchIfTrue ? b.Branch.Index : b.Next.Index;
                int whenFalse = last.Inst == Instruction.BranchIfTrue ? b.Next.Index : b.Branch.Index;

                return [new SIf { Block = x, Op = last, Then = DoBranch(g, x, whenTrue), Else = DoBranch(g, x, whenFalse) }];
            }

            BasicBlock target = b.Branch ?? b.Next;

            if (last != null && last.Inst is Instruction.Return or Instruction.Discard)
            {
                return [];
            }

            return target == null ? [] : DoBranch(g, x, target.Index);
        }

        private static List<S> DoBranch(Graph g, int x, int t)
        {
            if (IsBackEdge(g, x, t))
            {
                return [new SBr { Target = t, Continue = true }];
            }

            if (g.IsMerge[t])
            {
                return [new SBr { Target = t }];
            }

            if (g.Idom[t] != x)
            {
                throw new InvalidOperationException($"block {t} is not a merge node and not dominated by {x} alone");
            }

            return DoTree(g, t);
        }

        // Every branch finds its block or loop; every block is emitted exactly once.
        private static void Validate(Graph g, List<S> tree, List<(int Follow, bool Loop)> frames)
        {
            HashSet<int> seen = [];
            ValidateSeq(tree, frames, seen);

            if (seen.Count != g.N)
            {
                throw new InvalidOperationException($"{g.N - seen.Count} blocks not emitted");
            }
        }

        private static void ValidateSeq(List<S> seq, List<(int, bool)> frames, HashSet<int> seen)
        {
            foreach (S s in seq)
            {
                switch (s)
                {
                    case SCode c:
                        if (!seen.Add(c.Block))
                        {
                            throw new InvalidOperationException($"block {c.Block} emitted twice");
                        }
                        break;
                    case SIf i:
                        ValidateSeq(i.Then, frames, seen);
                        ValidateSeq(i.Else, frames, seen);
                        break;
                    case SLoop l:
                        frames.Add((l.Header, true));
                        ValidateSeq(l.Body, frames, seen);
                        frames.RemoveAt(frames.Count - 1);
                        break;
                    case SBlock bl:
                        frames.Add((bl.Follow, false));
                        ValidateSeq(bl.Body, frames, seen);
                        frames.RemoveAt(frames.Count - 1);
                        break;
                    case SBr br:
                        if (!frames.Contains((br.Target, br.Continue)))
                        {
                            throw new InvalidOperationException($"branch to {br.Target} ({(br.Continue ? "continue" : "exit")}) outside its construct");
                        }
                        break;
                }
            }
        }

    }
}
