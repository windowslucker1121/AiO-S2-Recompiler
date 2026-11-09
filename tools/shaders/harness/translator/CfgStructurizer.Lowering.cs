// SPDX-License-Identifier: MIT
// CfgStructurizer, second half: the structured tree to Ryujinx's AST (if/else, do-while, break, continue, flags). See CfgStructurizer.cs.
using Ryujinx.Graphics.Shader.IntermediateRepresentation;
using Ryujinx.Graphics.Shader.Translation;
using System;
using System.Collections.Generic;
using static Ryujinx.Graphics.Shader.StructuredIr.AstHelper;

namespace Ryujinx.Graphics.Shader.StructuredIr
{
    static partial class CfgStructurizer
    {
        private sealed class Frame
        {
            public bool Loop;
            public int Target;          // loop header or block follow
            public AstOperand Flag;     // made on first use
            public bool FlagRead;
            public List<AstAssignment> FlagWrites = [];
            public AstBlock InitBlock;  // where the flag is cleared
            public IAstNode InitAfter;  // ... after this node (null: first)
            public int Continues;       // continues of this loop
        }

        // A branch that left by its flag: control is now at loop depth Depth, on its way to the frame.
        private sealed class Esc
        {
            public Frame F;
            public int Depth;
        }

        private sealed class Lowering
        {
            private readonly Graph _g;
            private readonly StructuredProgramContext _context;
            private readonly Action<Operation> _addOperation;
            private readonly List<Frame> _frames = [];
            private readonly List<Frame> _allFrames = [];
            private int _loopDepth;
            private static readonly bool _flatElse = Environment.GetEnvironmentVariable("NXRT_HARNESS_STRUCT_FLAT_ELSE") != "0";

            public Lowering(Graph g, StructuredProgramContext context, Action<Operation> addOperation)
            {
                _g = g;
                _context = context;
                _addOperation = addOperation;
            }

            public void Run(List<S> tree, AstBlock main)
            {
                List<Esc> esc = LowerSeq(tree, main);

                if (esc.Count != 0)
                {
                    throw new InvalidOperationException("structurizer: a branch escaped the function");
                }

                foreach (Frame f in _allFrames)
                {
                    if (f.Flag == null)
                    {
                        continue;
                    }

                    if (!f.FlagRead)
                    {
                        // nobody reads it: drop the writes
                        foreach (AstAssignment a in f.FlagWrites)
                        {
                            if (a.Parent != null)
                            {
                                a.Parent.Remove(a);
                                RemoveDef(a.Destination, a);
                            }
                        }

                        continue;
                    }

                    AstAssignment init = Assign(f.Flag, Const(IrConsts.False));

                    if (f.InitAfter == null)
                    {
                        f.InitBlock.AddFirst(init);
                    }
                    else
                    {
                        f.InitBlock.AddAfter(f.InitAfter, init);
                    }
                }
            }

            private Frame Find(int target, bool loop, out int loopsCrossed)
            {
                loopsCrossed = 0;

                for (int i = _frames.Count - 1; i >= 0; i--)
                {
                    Frame f = _frames[i];

                    if (f.Loop == loop && f.Target == target)
                    {
                        return f;
                    }

                    if (f.Loop)
                    {
                        loopsCrossed++;
                    }
                }

                throw new InvalidOperationException($"structurizer: no construct for branch to {target}");
            }

            // Loops between the current position and the frame (the frame itself excluded).
            private int LoopsBetween(Frame target)
            {
                int n = 0;

                for (int i = _frames.Count - 1; i >= 0 && _frames[i] != target; i--)
                {
                    if (_frames[i].Loop)
                    {
                        n++;
                    }
                }

                return n;
            }

            private AstOperand FlagOf(Frame f)
            {
                return f.Flag ??= _context.NewTemp(AggregateType.Bool);
            }

            private AstAssignment WriteFlag(Frame f, bool value)
            {
                AstAssignment a = Assign(FlagOf(f), Const(value ? IrConsts.True : IrConsts.False));
                f.FlagWrites.Add(a);

                return a;
            }

            private static void Merge(List<Esc> list, Esc e)
            {
                foreach (Esc x in list)
                {
                    if (x.F == e.F && x.Depth == e.Depth)
                    {
                        return;
                    }
                }

                list.Add(e);
            }

            // Lowers the statements into dst; returns the branches that left by a flag and are still on their way out.
            private List<Esc> LowerSeq(List<S> seq, AstBlock dst)
            {
                List<Esc> pending = [];

                for (int i = 0; i < seq.Count; i++)
                {
                    List<Esc> esc = LowerItem(seq[i], dst);

                    if (esc.Count == 0)
                    {
                        continue;
                    }

                    List<Frame> guard = [];

                    foreach (Esc e in esc)
                    {
                        if (e.Depth < _loopDepth)
                        {
                            // control already left the loop this sequence is in
                            Merge(pending, e);
                        }
                        else if (LoopsBetween(e.F) > 0)
                        {
                            // a loop between here and the target: leave it
                            e.F.FlagRead = true;
                            AstBlock brk = new(AstBlockType.If, FlagOf(e.F));
                            brk.Add(new AstOperation(Instruction.LoopBreak));
                            dst.Add(brk);
                            Merge(pending, new Esc { F = e.F, Depth = _loopDepth - 1 });
                        }
                        else if (e.F.Loop)
                        {
                            // continue of the innermost loop
                            e.F.FlagRead = true;
                            AstBlock cont = new(AstBlockType.If, FlagOf(e.F));
                            cont.Add(WriteFlag(e.F, false));
                            cont.Add(new AstOperation(Instruction.LoopContinue));
                            e.F.Continues++;
                            dst.Add(cont);
                        }
                        else
                        {
                            if (!guard.Contains(e.F))
                            {
                                guard.Add(e.F);
                            }

                            Merge(pending, e);
                        }
                    }

                    if (guard.Count != 0 && i + 1 < seq.Count)
                    {
                        IAstNode cond = null;

                        foreach (Frame f in guard)
                        {
                            f.FlagRead = true;
                            IAstNode notF = new AstOperation(Instruction.LogicalNot, FlagOf(f));
                            cond = cond == null ? notF : new AstOperation(Instruction.LogicalAnd, cond, notF);
                        }

                        AstBlock g = new(AstBlockType.If, cond);
                        dst.Add(g);
                        dst = g;
                    }
                }

                return pending;
            }

            private List<Esc> LowerItem(S s, AstBlock dst)
            {
                switch (s)
                {
                    case SCode c:
                        {
                            BasicBlock block = _g.Blocks[c.Block];
                            _context.SetNodeBlock(c.Block);
                            _context.CurrentAstBlock = dst;

                            for (LinkedListNode<INode> node = block.Operations.First; node != null; node = node.Next)
                            {
                                Operation op = (Operation)node.Value;

                                if (op.Inst is Instruction.Branch or Instruction.BranchIfTrue or Instruction.BranchIfFalse)
                                {
                                    continue;
                                }

                                _addOperation(op);
                            }

                            return [];
                        }

                    case SIf i:
                        {
                            IAstNode cond = _context.GetOperand(i.Op.GetSource(0));
                            List<S> then = i.Then, els = i.Else;

                            if (then.Count == 0 && els.Count != 0)
                            {
                                cond = new AstOperation(Instruction.LogicalNot, cond);
                                (then, els) = (els, then);
                            }

                            if (then.Count == 0)
                            {
                                return [];
                            }

                            AstBlock ifBlock = new(AstBlockType.If, cond);
                            dst.Add(ifBlock);
                            List<Esc> esc = LowerSeq(then, ifBlock);

                            if (els.Count != 0 && _flatElse && ifBlock.Last is AstOperation
                                {
                                    Inst: Instruction.Return or Instruction.Discard or Instruction.LoopBreak or Instruction.LoopContinue
                                })
                            {
                                // "if (c) { ...; return; } rest": the else part needs no block of its own
                                foreach (Esc e in LowerSeq(els, dst))
                                {
                                    Merge(esc, e);
                                }
                            }
                            else if (els.Count != 0)
                            {
                                AstBlock elseBlock = new(AstBlockType.Else);
                                dst.Add(elseBlock);

                                foreach (Esc e in LowerSeq(els, elseBlock))
                                {
                                    Merge(esc, e);
                                }

                                if (elseBlock.Count == 0)
                                {
                                    dst.Remove(elseBlock);
                                }
                            }

                            if (ifBlock.Count == 0 && Next(ifBlock) == null)
                            {
                                dst.Remove(ifBlock);
                                ifBlock.Condition = null;
                            }

                            return esc;
                        }

                    case SBlock bl:
                        {
                            Frame f = new() { Loop = false, Target = bl.Follow, InitBlock = dst, InitAfter = dst.Last };
                            _frames.Add(f);
                            _allFrames.Add(f);
                            List<Esc> esc = LowerSeq(bl.Body, dst);
                            _frames.RemoveAt(_frames.Count - 1);
                            esc.RemoveAll(e => e.F == f);

                            return esc;
                        }

                    case SLoop l:
                        {
                            AstBlock loop = new(AstBlockType.DoWhile, Const(IrConsts.True));
                            dst.Add(loop);
                            Frame f = new() { Loop = true, Target = l.Header, InitBlock = loop, InitAfter = null };
                            _frames.Add(f);
                            _allFrames.Add(f);
                            _loopDepth++;
                            List<Esc> esc = LowerSeq(l.Body, loop);
                            _loopDepth--;
                            _frames.RemoveAt(_frames.Count - 1);
                            esc.RemoveAll(e => e.F == f);

                            // do { ...; if (c) continue; } while (true) with no other continue: do { ... } while (c)
                            if (f.Continues == 1 && loop.Last is AstBlock { Type: AstBlockType.If } last && last.Count == 1 &&
                                last.First is AstOperation { Inst: Instruction.LoopContinue })
                            {
                                IAstNode c = last.Condition;
                                last.Condition = null;
                                loop.Remove(last);
                                loop.Condition = c;
                            }
                            else if (f.Continues == 1 && loop.Last is AstOperation { Inst: Instruction.LoopContinue } lastCont)
                            {
                                loop.Remove(lastCont);
                            }
                            else
                            {
                                loop.Add(new AstOperation(Instruction.LoopBreak));
                            }

                            return esc;
                        }

                    case SBr br:
                        {
                            Frame f = Find(br.Target, br.Continue, out int crossed);

                            if (br.Continue && crossed == 0)
                            {
                                f.Continues++;
                                dst.Add(new AstOperation(Instruction.LoopContinue));

                                return [];
                            }

                            dst.Add(WriteFlag(f, true));

                            if (crossed > 0)
                            {
                                dst.Add(new AstOperation(Instruction.LoopBreak));

                                return [new Esc { F = f, Depth = _loopDepth - 1 }];
                            }

                            return [new Esc { F = f, Depth = _loopDepth }];
                        }
                }

                throw new InvalidOperationException("structurizer: unknown statement");
            }
        }
    }
}
