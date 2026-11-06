// SPDX-License-Identifier: MIT
// Diagnosis for Ryujinx's structurizer (StructuredProgram + GotoElimination), off unless NXRT_HARNESS_CFG_CHECK=N is set:
// after goto elimination, N random walks through the structured code of each function, every one checked step by step
// against the basic block graph the code was made from. Compiled into the translator assembly
// (translator/Ryujinx.Graphics.Shader.csproj); the hooks are in structured-program-cfg-check.patch and
// structured-context-node-blocks.patch.
//
// What a walk does: it runs the AST (if/else, do-while, break, continue, return) with a value for every boolean local.
// Branch conditions of the original code (a comparison, a predicate) get a random value each time their assignment
// runs; the flags goto elimination adds are evaluated exactly. Each basic block's first AST statement marks "entering
// that block"; at every such mark the walk checks that the block is the one the graph names after the previous block,
// following the previous block's branch with the value its condition has now (blocks without statements, such as a
// lone "BRA" that became a loop condition, are followed through). A statement that runs outside its own block, a step
// to the wrong block or a walk that does not end within StepLimit statements is an error (InvalidOperationException,
// so the harness reports the block as "exception").
using Ryujinx.Graphics.Shader.IntermediateRepresentation;
using System;
using System.Collections.Generic;
using System.Linq;

namespace Ryujinx.Graphics.Shader.StructuredIr
{
    static class StructuredCfgCheck
    {
        private static readonly int Trials = int.TryParse(Environment.GetEnvironmentVariable("NXRT_HARNESS_CFG_CHECK"), out int trials) ? trials : 0;

        private const int StepLimit = 2_000_000;

        public static bool Enabled => Trials > 0;

        public static void Run(BasicBlock[] blocks, StructuredFunction function, List<(IAstNode Node, int Block)> nodeBlocks, Func<Operand, AstOperand> getOperand)
        {
            // A branch on an undefined value (code the front end could not give a predicate) has no value both sides can share.
            if (blocks.Any(b => b.GetLastOp() is Operation { Inst: Instruction.BranchIfTrue or Instruction.BranchIfFalse } op && op.GetSource(0).Type == OperandType.Undefined))
            {
                Console.Error.WriteLine($"cfg check {function.Name}: not checked (a branch on an undefined value)");
                return;
            }

            Walker walker = new(blocks, function, nodeBlocks, getOperand);

            for (int trial = 0; trial < Trials; trial++)
            {
                walker.Walk(trial);
            }

            string unseenBlocks = string.Join(",", blocks.Select(b => b.Index).Where(b => !walker.SeenBlocks.Contains(b)).Take(8));
            string unseenEdges = string.Join(",", blocks
                .SelectMany(b => new[] { b.Next, b.Branch }.Where(s => s != null).Distinct().Select(s => (b.Index, s.Index)))
                .Where(e => !walker.SeenEdges.Contains(e))
                .Take(8)
                .Select(e => $"{e.Item1}->{e.Item2}"));

            Console.Error.WriteLine($"cfg check {function.Name}: {Trials} walks ok, blocks {walker.SeenBlocks.Count}/{blocks.Length} " +
                                    $"({walker.VisibleCount} with statements), edges {walker.SeenEdges.Count}/{walker.EdgeCount}, " +
                                    $"longest walk {walker.MaxSteps} statements" +
                                    (unseenBlocks.Length > 0 ? $"; not reached: blocks {unseenBlocks}" : "") +
                                    (unseenEdges.Length > 0 ? $"; edges {unseenEdges}" : ""));
        }

        private enum Flow
        {
            Normal,
            Break,
            Continue,
            End,
        }

        private sealed class Walker
        {
            private readonly BasicBlock[] _blocks;
            private readonly StructuredFunction _function;
            private readonly Dictionary<IAstNode, int> _blockOf = new();
            private readonly Dictionary<IAstNode, int> _entryOf = new();
            private readonly bool[] _visible;
            private readonly AstOperand[] _cond;
            private readonly Dictionary<AstOperand, bool> _values = new();

            private Random _random;
            private int _trial;
            private int _last;
            private long _steps;

            public HashSet<int> SeenBlocks { get; } = [];
            public HashSet<(int, int)> SeenEdges { get; } = [];
            public int EdgeCount { get; }
            public int VisibleCount { get; }
            public long MaxSteps { get; private set; }

            public Walker(BasicBlock[] blocks, StructuredFunction function, List<(IAstNode Node, int Block)> nodeBlocks, Func<Operand, AstOperand> getOperand)
            {
                _blocks = blocks;
                _function = function;
                _visible = new bool[blocks.Length];
                _cond = new AstOperand[blocks.Length];

                foreach ((IAstNode node, int block) in nodeBlocks)
                {
                    _blockOf[node] = block;

                    // Statements only: an if or do-while made for a block is also entered by gotos from elsewhere.
                    if (node is not AstBlock && !_visible[block])
                    {
                        _visible[block] = true;
                        _entryOf[node] = block;
                    }
                }

                VisibleCount = _visible.Count(v => v);

                foreach (BasicBlock block in blocks)
                {
                    if (block.GetLastOp() is Operation op && op.Inst is Instruction.BranchIfTrue or Instruction.BranchIfFalse)
                    {
                        Operand cond = op.GetSource(0);

                        if (cond.Type != OperandType.Constant && cond.Type != OperandType.LocalVariable)
                        {
                            throw new InvalidOperationException($"CFG check ({function.Name}): block {block.Index} branches on a {cond.Type}");
                        }

                        _cond[block.Index] = getOperand(cond);
                    }

                    EdgeCount += (block.Next != null ? 1 : 0) + (block.Branch != null && block.Branch != block.Next ? 1 : 0);
                }
            }

            public void Walk(int trial)
            {
                _trial = trial;
                _random = new Random(0x5eed + trial);
                _values.Clear();
                _last = -1;
                _steps = 0;

                Flow flow = Exec(_function.MainBlock);

                if (flow is Flow.Break or Flow.Continue)
                {
                    Fail($"{flow} outside a loop");
                }

                if (flow == Flow.Normal)
                {
                    // Fell off the end of the function: the graph must end there too (a function may have no statement at all).
                    int expected = _last == -1 ? FollowFrom(0) : Follow(_last);

                    if (expected != -1)
                    {
                        Fail($"the structured code ends after block {_last}, the graph continues with block {expected}");
                    }
                }

                MaxSteps = Math.Max(MaxSteps, _steps);
            }

            private Flow Exec(AstBlock block)
            {
                bool ifTaken = false;

                foreach (IAstNode node in block)
                {
                    if (++_steps > StepLimit)
                    {
                        Fail($"no end after {StepLimit} statements (an endless loop?), last block {_last}");
                    }

                    if (node is AstBlock child)
                    {
                        Flow flow = Flow.Normal;

                        switch (child.Type)
                        {
                            case AstBlockType.If:
                                ifTaken = Eval(child.Condition);
                                if (ifTaken)
                                {
                                    flow = Exec(child);
                                }
                                break;

                            case AstBlockType.Else:
                                if (AstHelper.Previous(child) is not AstBlock { Type: AstBlockType.If })
                                {
                                    Fail("else without an if before it");
                                }
                                if (!ifTaken)
                                {
                                    flow = Exec(child);
                                }
                                break;

                            case AstBlockType.DoWhile:
                                while (true)
                                {
                                    flow = Exec(child);

                                    if (flow == Flow.Break)
                                    {
                                        flow = Flow.Normal;
                                        break;
                                    }

                                    if (flow == Flow.End)
                                    {
                                        break;
                                    }

                                    flow = Flow.Normal;

                                    if (!Eval(child.Condition))
                                    {
                                        break;
                                    }

                                    if (++_steps > StepLimit)
                                    {
                                        Fail($"no end after {StepLimit} statements (an endless loop?), last block {_last}");
                                    }
                                }
                                break;

                            default:
                                Fail($"unexpected {child.Type} block before the AST optimizer");
                                break;
                        }

                        if (flow != Flow.Normal)
                        {
                            return flow;
                        }

                        continue;
                    }

                    if (_entryOf.TryGetValue(node, out int entered))
                    {
                        Enter(entered);
                    }

                    if (_blockOf.TryGetValue(node, out int owner) && owner != _last)
                    {
                        Fail($"a statement of block {owner} runs while the walk is in block {_last}");
                    }

                    if (node is AstAssignment assignment)
                    {
                        if (assignment.Destination is AstOperand { Type: OperandType.LocalVariable } dest)
                        {
                            _values[dest] = Eval(assignment.Source);
                        }
                    }
                    else if (node is AstOperation operation)
                    {
                        switch (operation.Inst)
                        {
                            case Instruction.LoopBreak:
                                return Flow.Break;
                            case Instruction.LoopContinue:
                                return Flow.Continue;
                            case Instruction.Return:
                            case Instruction.Discard:
                                return Flow.End;
                            case Instruction.Branch:
                            case Instruction.BranchIfTrue:
                            case Instruction.BranchIfFalse:
                                Fail("a goto is left after goto elimination");
                                break;
                        }

                        // Results written through extra operands (out arguments): unknown values.
                        for (int index = 0; index < operation.SourcesCount; index++)
                        {
                            if (operation.GetSource(index) is AstOperand { Type: OperandType.LocalVariable } outDest && outDest.Defs.Contains(operation))
                            {
                                _values[outDest] = _random.Next(2) != 0;
                            }
                        }
                    }
                }

                return Flow.Normal;
            }

            private void Enter(int block)
            {
                int expected = _last == -1 ? FollowFrom(0) : Follow(_last);

                if (block != expected)
                {
                    Fail(expected == -1
                        ? $"the structured code enters block {block} after block {_last}, where the graph ends"
                        : $"the structured code enters block {block} after block {_last}, the graph goes to block {expected}");
                }

                _last = block;
                SeenBlocks.Add(block);
            }

            // The next block with statements after block `from`, following branches with the current condition values.
            private int Follow(int from)
            {
                return FollowFrom(Successor(from));
            }

            private int FollowFrom(int block)
            {
                int guard = 0;

                while (block != -1 && !_visible[block])
                {
                    SeenBlocks.Add(block);

                    block = Successor(block);

                    if (++guard > _blocks.Length)
                    {
                        Fail("a cycle of blocks without statements");
                    }
                }

                return block;
            }

            private int Successor(int index)
            {
                BasicBlock block = _blocks[index];

                int next = block.GetLastOp() is Operation op
                    ? op.Inst switch
                    {
                        Instruction.Branch => block.Branch.Index,
                        Instruction.BranchIfTrue => Eval(_cond[index]) ? block.Branch.Index : block.Next.Index,
                        Instruction.BranchIfFalse => Eval(_cond[index]) ? block.Next.Index : block.Branch.Index,
                        Instruction.Return or Instruction.Discard => -1,
                        _ => block.Next?.Index ?? -1,
                    }
                    : block.Next?.Index ?? -1;

                if (next != -1)
                {
                    SeenEdges.Add((index, next));
                }

                return next;
            }

            private bool Eval(IAstNode node)
            {
                switch (node)
                {
                    case AstOperand { Type: OperandType.Constant } constant:
                        return constant.Value != 0;

                    case AstOperand { Type: OperandType.LocalVariable } local:
                        if (!_values.TryGetValue(local, out bool value))
                        {
                            _values[local] = value = _random.Next(2) != 0;
                        }
                        return value;

                    // Predicates are also combined with the bitwise forms (on integers the result is as random as the inputs).
                    case AstOperation { Inst: Instruction.LogicalNot or Instruction.BitwiseNot } not:
                        return !Eval(not.GetSource(0));

                    case AstOperation { Inst: Instruction.LogicalAnd or Instruction.BitwiseAnd } and:
                        return Eval(and.GetSource(0)) & Eval(and.GetSource(1));

                    case AstOperation { Inst: Instruction.LogicalOr or Instruction.BitwiseOr } or:
                        return Eval(or.GetSource(0)) | Eval(or.GetSource(1));

                    case AstOperation { Inst: Instruction.LogicalExclusiveOr or Instruction.BitwiseExclusiveOr } xor:
                        return Eval(xor.GetSource(0)) ^ Eval(xor.GetSource(1));

                    case AstOperation { Inst: Instruction.Copy } copy:
                        return Eval(copy.GetSource(0));

                    default:
                        // A comparison or anything else the original code computes: a random outcome.
                        return _random.Next(2) != 0;
                }
            }

            private void Fail(string what)
            {
                throw new InvalidOperationException($"CFG check ({_function.Name}, walk {_trial}): {what}");
            }
        }
    }
}
