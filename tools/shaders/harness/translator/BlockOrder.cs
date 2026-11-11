// SPDX-License-Identifier: MIT
// Block order for the structurizer (the game register pressure, 2026-11-09).
//
// Ryujinx's structurizer (StructuredProgramContext) takes the blocks in their index order, which is the order the IR was emitted in,
// i.e. the address order of the SASS, and treats every branch to a lower index as a loop: the target becomes the head of a do-while
// that runs up to the branching block. SASS puts the cases of a BRX jump table before and after the BRX, so each case that lies before
// the BRX became one more do-while around the switch (the game's material shaders: the light loop held five such loops nested in each
// other, each with a goto-elimination flag, and NVIDIA's compiler kept the light record and the shading inputs alive through all of
// them: 235-255 registers against 94 in the original code).
//
// This pass renumbers the blocks of a function so that every edge that is not a back edge of a natural loop goes forward and every
// loop's blocks are contiguous, starting at its header and ending at a latch: a topological order of the loop-collapsed graph, ties
// broken by the original index (code that is already in order stays in order). Fall-through edges (Next) must lead to the following
// block, so where the new order separates a block from its Next, the block gets an explicit branch: a block without a branch op gets
// an unconditional Branch; a conditional branch whose taken target follows it is inverted; otherwise a new block holding an
// unconditional Branch to the old Next is inserted right after it. Runs before SSA, so there are no phis to fix.
// An irreducible graph (a retreating edge whose target does not dominate its source) keeps the original order.
// Off: NXRT_HARNESS_BLOCK_ORDER=0.
using Ryujinx.Graphics.Shader.IntermediateRepresentation;
using System;
using System.Collections.Generic;
using static Ryujinx.Graphics.Shader.IntermediateRepresentation.OperandHelper;

namespace Ryujinx.Graphics.Shader.Translation
{
    static class BlockOrder
    {
        private static readonly bool _enabled = Environment.GetEnvironmentVariable("NXRT_HARNESS_BLOCK_ORDER") != "0";
        private static readonly bool _log = Environment.GetEnvironmentVariable("NXRT_HARNESS_BLOCK_ORDER") == "log";

        public static BasicBlock[] Run(BasicBlock[] blocks)
        {
            if (!_enabled || blocks.Length < 3)
            {
                return blocks;
            }

            int[] order;

            try
            {
                order = ComputeOrder(blocks);
            }
            catch (InvalidOperationException)
            {
                return blocks;
            }

            if (_log)
            {
                Console.Error.WriteLine($"block order: {blocks.Length} blocks, {(order == null ? "kept (irreducible or unreachable)" : string.Join(",", order))}");

                foreach (BasicBlock b in blocks)
                {
                    string last = b.GetLastOp() is Operation lo ? lo.Inst.ToString() : "-";
                    Console.Error.WriteLine($"  b{b.Index}: {b.Operations.Count} ops, next {(b.Next?.Index.ToString() ?? "-")}, branch {(b.Branch?.Index.ToString() ?? "-")}, last {last}");
                }
            }

            if (order == null)
            {
                return blocks;
            }

            bool same = true;

            for (int i = 0; i < order.Length; i++)
            {
                if (order[i] != i)
                {
                    same = false;
                    break;
                }
            }

            if (same)
            {
                return blocks;
            }

            return Rebuild(blocks, order);
        }

        private static IEnumerable<int> Succ(BasicBlock[] blocks, int n)
        {
            BasicBlock b = blocks[n];

            if (b.Next != null)
            {
                yield return b.Next.Index;
            }

            if (b.Branch != null && b.Branch != b.Next)
            {
                yield return b.Branch.Index;
            }
        }

        private static int[] ComputeOrder(BasicBlock[] blocks)
        {
            int n = blocks.Length;

            for (int i = 0; i < n; i++)
            {
                if (blocks[i].Index != i)
                {
                    return null;
                }
            }

            // Reverse post order (iterative DFS, Next before Branch as ControlFlowGraph does).
            int[] post = new int[n];
            int[] rpoNum = new int[n];
            Array.Fill(rpoNum, -1);
            bool[] visited = new bool[n];
            List<int> postOrder = new(n);
            Stack<(int Node, IEnumerator<int> It)> stack = new();
            visited[0] = true;
            stack.Push((0, Succ(blocks, 0).GetEnumerator()));

            while (stack.Count != 0)
            {
                var (node, it) = stack.Peek();

                if (it.MoveNext())
                {
                    int s = it.Current;

                    if (!visited[s])
                    {
                        visited[s] = true;
                        stack.Push((s, Succ(blocks, s).GetEnumerator()));
                    }
                }
                else
                {
                    stack.Pop();
                    postOrder.Add(node);
                }
            }

            if (postOrder.Count != n)
            {
                return null; // unreachable blocks (Create removes them; be safe)
            }

            int[] rpo = new int[n];

            for (int i = 0; i < n; i++)
            {
                rpo[i] = postOrder[n - 1 - i];
                rpoNum[rpo[i]] = i;
            }

            // Predecessors by index.
            List<int>[] preds = new List<int>[n];

            for (int i = 0; i < n; i++)
            {
                preds[i] = [];
            }

            for (int i = 0; i < n; i++)
            {
                foreach (int s in Succ(blocks, i))
                {
                    preds[s].Add(i);
                }
            }

            // Dominators (Cooper, Harvey, Kennedy).
            int[] idom = new int[n];
            Array.Fill(idom, -1);
            idom[0] = 0;
            bool changed = true;

            while (changed)
            {
                changed = false;

                for (int k = 1; k < n; k++)
                {
                    int b = rpo[k];
                    int newIdom = -1;

                    foreach (int p in preds[b])
                    {
                        if (idom[p] == -1)
                        {
                            continue;
                        }

                        newIdom = newIdom == -1 ? p : Intersect(idom, rpoNum, p, newIdom);
                    }

                    if (newIdom != -1 && idom[b] != newIdom)
                    {
                        idom[b] = newIdom;
                        changed = true;
                    }
                }
            }

            bool Dominates(int a, int b)
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

                    b = idom[b];
                }
            }

            // Back edges and natural loops (merged per header).
            Dictionary<int, HashSet<int>> loops = new();
            List<(int From, int To)> backEdges = [];

            for (int u = 0; u < n; u++)
            {
                foreach (int v in Succ(blocks, u))
                {
                    if (rpoNum[v] <= rpoNum[u])
                    {
                        if (!Dominates(v, u))
                        {
                            return null; // irreducible
                        }

                        backEdges.Add((u, v));
                    }
                }
            }

            foreach (var (u, h) in backEdges)
            {
                if (!loops.TryGetValue(h, out HashSet<int> body))
                {
                    body = [h];
                    loops.Add(h, body);
                }

                Stack<int> work = new();

                if (body.Add(u))
                {
                    work.Push(u);
                }

                while (work.Count != 0)
                {
                    int x = work.Pop();

                    foreach (int p in preds[x])
                    {
                        if (body.Add(p))
                        {
                            work.Push(p);
                        }
                    }
                }
            }

            HashSet<int> all = [];

            for (int i = 0; i < n; i++)
            {
                all.Add(i);
            }

            List<int> result = new(n);
            OrderRegion(blocks, all, -1, loops, result);

            if (result.Count != n)
            {
                return null;
            }

            return result.ToArray();
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

        private static void OrderRegion(BasicBlock[] blocks, HashSet<int> region, int header, Dictionary<int, HashSet<int>> loops, List<int> result)
        {
            // Maximal loops inside the region (other than the region's own loop).
            List<int> inner = [];

            foreach (var (h, body) in loops)
            {
                if (h == header || !region.Contains(h))
                {
                    continue;
                }

                bool maximal = true;

                foreach (var (h2, body2) in loops)
                {
                    if (h2 != h && h2 != header && region.Contains(h2) && body2.Contains(h) && body2.Count > body.Count)
                    {
                        maximal = false;
                        break;
                    }
                }

                if (maximal)
                {
                    inner.Add(h);
                }
            }

            Dictionary<int, int> rep = new();

            foreach (int x in region)
            {
                rep[x] = x;
            }

            foreach (int h in inner)
            {
                foreach (int x in loops[h])
                {
                    if (!region.Contains(x))
                    {
                        throw new InvalidOperationException("loop leaves its region");
                    }

                    rep[x] = h;
                }
            }

            Dictionary<int, HashSet<int>> edges = new();
            Dictionary<int, int> indeg = new();

            foreach (int x in region)
            {
                if (rep[x] == x)
                {
                    edges[x] = [];
                    indeg[x] = 0;
                }
            }

            foreach (int x in region)
            {
                foreach (int s in Succ(blocks, x))
                {
                    if (!region.Contains(s) || s == header)
                    {
                        continue;
                    }

                    int a = rep[x], b = rep[s];

                    if (a != b && edges[a].Add(b))
                    {
                        indeg[b]++;
                    }
                }
            }

            PriorityQueue<int, int> ready = new();

            foreach (var (x, d) in indeg)
            {
                if (d == 0)
                {
                    ready.Enqueue(x, x == header ? -1 : x);
                }
            }

            int placed = 0;

            while (ready.TryDequeue(out int x, out _))
            {
                placed++;

                if (x != header && loops.ContainsKey(x) && inner.Contains(x))
                {
                    OrderRegion(blocks, loops[x], x, loops, result);
                }
                else
                {
                    result.Add(x);
                }

                foreach (int s in edges[x])
                {
                    if (--indeg[s] == 0)
                    {
                        ready.Enqueue(s, s);
                    }
                }
            }

            if (placed != indeg.Count)
            {
                throw new InvalidOperationException("cycle in the loop-collapsed graph");
            }

            if (header >= 0 && result.Count > 0 && region.Count > 0)
            {
                // the loop's region must start with its header
                int start = result.Count - region.Count;

                if (start < 0 || result[start] != header)
                {
                    throw new InvalidOperationException("loop does not start at its header");
                }
            }
        }

        private static bool IsConditional(Instruction inst)
        {
            return inst is Instruction.BranchIfTrue or Instruction.BranchIfFalse;
        }

        private static BasicBlock[] Rebuild(BasicBlock[] blocks, int[] order)
        {
            // Labels already used for each target block (BranchElimination compares labels).
            Dictionary<BasicBlock, Operand> labels = new();

            foreach (BasicBlock b in blocks)
            {
                if (b.Branch != null && b.GetLastOp() is Operation op && op.Dest != null && op.Dest.Type == OperandType.Label)
                {
                    labels.TryAdd(b.Branch, op.Dest);
                }
            }

            Operand LabelOf(BasicBlock target)
            {
                if (!labels.TryGetValue(target, out Operand label))
                {
                    label = Label();
                    labels.Add(target, label);
                }

                return label;
            }

            List<BasicBlock> result = new(blocks.Length + 8);

            for (int i = 0; i < order.Length; i++)
            {
                BasicBlock b = blocks[order[i]];
                BasicBlock follow = i + 1 < order.Length ? blocks[order[i + 1]] : null;

                result.Add(b);

                if (b.Next == null || b.Next == follow)
                {
                    continue;
                }

                BasicBlock fall = b.Next;
                Operation last = b.GetLastOp() as Operation;

                if (last != null && IsConditional(last.Inst))
                {
                    if (b.Branch == follow)
                    {
                        // Invert: the taken target falls through now.
                        Instruction inv = last.Inst == Instruction.BranchIfTrue ? Instruction.BranchIfFalse : Instruction.BranchIfTrue;
                        Operation newOp = new(inv, LabelOf(fall), last.GetSource(0));
                        b.Operations.RemoveLast();
                        b.Operations.AddLast(newOp);
                        BasicBlock taken = b.Branch;
                        b.Branch = null;
                        b.Next = null;
                        b.Branch = fall;
                        b.Next = taken;
                    }
                    else
                    {
                        // New block with an unconditional branch to the old fall-through target.
                        BasicBlock j = new();
                        j.Operations.AddLast(new Operation(Instruction.Branch, LabelOf(fall)));
                        b.Next = j;
                        j.Branch = fall;
                        result.Add(j);
                    }
                }
                else
                {
                    if (last != null && (last.Inst == Instruction.Branch || last.Inst == Instruction.Return || last.Inst == Instruction.Discard))
                    {
                        throw new InvalidOperationException("unconditional end with a fall-through");
                    }

                    b.Operations.AddLast(new Operation(Instruction.Branch, LabelOf(fall)));
                    b.Next = null;
                    b.Branch = fall;
                }
            }

            for (int i = 0; i < result.Count; i++)
            {
                result[i].Index = i;
            }

            return result.ToArray();
        }
    }
}
