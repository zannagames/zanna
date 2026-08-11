//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/analysis/PostDominatorsTests.cpp
// Purpose: Validate post-dominator tree construction and queries.
// Key invariants:
//   - Exit blocks have ipostdom == nullptr (virtual exit).
//   - A post-dominates B iff A is on every path from B to any exit.
//   - The post-dominator tree is dual to the dominator tree on the reversed CFG.
// Ownership/Lifetime: Builds local modules via IRBuilder.
// Links: il/analysis/Dominators.hpp
//
//===----------------------------------------------------------------------===//

#include "il/analysis/CFG.hpp"
#include "il/analysis/Dominators.hpp"
#include "il/build/IRBuilder.hpp"
#include <cassert>

int main() {
    using namespace il::core;
    using namespace zanna::analysis;

    Module m;
    il::build::IRBuilder b(m);

    // -------------------------------------------------------------------------
    // Test 1: Linear chain — entry -> A -> B -> exit (ret)
    //
    //   entry → A → B → exit
    //
    // Expected post-dominator tree (virtual exit = nullptr):
    //   exit  .ipostdom = nullptr   (exit block)
    //   B     .ipostdom = exit      (B post-dominates exit)
    //   A     .ipostdom = B         (A must pass through B to reach exit)
    //   entry .ipostdom = A
    //
    // Post-domination queries:
    //   exit pdom B, exit pdom A, exit pdom entry ✓
    //   B    pdom A, B pdom entry                 ✓
    //   A    pdom entry                            ✓
    //   A does NOT pdom B (B can reach exit without passing A)
    // -------------------------------------------------------------------------
    {
        Function &fn = b.startFunction("chain", Type(Type::Kind::Void), {});
        b.createBlock(fn, "entry");
        b.createBlock(fn, "A");
        b.createBlock(fn, "B");
        b.createBlock(fn, "exit");
        Block &bEntry = fn.blocks[0];
        Block &bA = fn.blocks[1];
        Block &bB = fn.blocks[2];
        Block &bExit = fn.blocks[3];

        b.setInsertPoint(bEntry);
        b.br(bA, {});
        b.setInsertPoint(bA);
        b.br(bB, {});
        b.setInsertPoint(bB);
        b.br(bExit, {});
        b.setInsertPoint(bExit);
        b.emitRet(std::nullopt, {});

        CFGContext ctx(m);
        PostDomTree pdt = computePostDominatorTree(ctx, fn);

        assert(pdt.immediatePostDominator(&bExit) == nullptr &&
               "exit block has virtual exit as ipostdom");
        assert(pdt.immediatePostDominator(&bB) == &bExit && "B ipostdom is exit");
        assert(pdt.immediatePostDominator(&bA) == &bB && "A ipostdom is B");
        assert(pdt.immediatePostDominator(&bEntry) == &bA && "entry ipostdom is A");

        assert(pdt.postDominates(&bExit, &bB) && "exit pdoms B");
        assert(pdt.postDominates(&bExit, &bA) && "exit pdoms A");
        assert(pdt.postDominates(&bExit, &bEntry) && "exit pdoms entry");
        assert(pdt.postDominates(&bB, &bA) && "B pdoms A");
        assert(pdt.postDominates(&bB, &bEntry) && "B pdoms entry");
        assert(pdt.postDominates(&bA, &bEntry) && "A pdoms entry");

        assert(!pdt.postDominates(&bA, &bB) && "A does not pdom B");
        assert(!pdt.postDominates(&bEntry, &bA) && "entry does not pdom A");
    }

    // -------------------------------------------------------------------------
    // Test 2: Diamond — entry -> {left, right} -> merge -> exit
    //
    //   entry
    //   +-- left  --+
    //   +-- right --+
    //               +-- merge
    //                   +-- exit (ret)
    //
    // Expected post-dominator tree:
    //   exit  .ipostdom = nullptr
    //   merge .ipostdom = exit
    //   left  .ipostdom = merge
    //   right .ipostdom = merge
    //   entry .ipostdom = merge   (intersection of left/right paths = merge)
    //
    // merge post-dominates entry because every path entry->...->exit goes through merge.
    // left and right do NOT post-dominate each other (sibling paths).
    // -------------------------------------------------------------------------
    {
        Function &fn = b.startFunction("diamond", Type(Type::Kind::Void), {});
        b.createBlock(fn, "entry");
        b.createBlock(fn, "left");
        b.createBlock(fn, "right");
        b.createBlock(fn, "merge");
        b.createBlock(fn, "exit");
        Block &bEntry = fn.blocks[0];
        Block &bLeft = fn.blocks[1];
        Block &bRight = fn.blocks[2];
        Block &bMerge = fn.blocks[3];
        Block &bExit = fn.blocks[4];

        b.setInsertPoint(bEntry);
        b.cbr(Value::constBool(true), bLeft, {}, bRight, {});
        b.setInsertPoint(bLeft);
        b.br(bMerge, {});
        b.setInsertPoint(bRight);
        b.br(bMerge, {});
        b.setInsertPoint(bMerge);
        b.br(bExit, {});
        b.setInsertPoint(bExit);
        b.emitRet(std::nullopt, {});

        CFGContext ctx(m);
        PostDomTree pdt = computePostDominatorTree(ctx, fn);

        assert(pdt.immediatePostDominator(&bExit) == nullptr &&
               "exit block ipostdom is virtual exit");
        assert(pdt.immediatePostDominator(&bMerge) == &bExit && "merge ipostdom is exit");
        assert(pdt.immediatePostDominator(&bLeft) == &bMerge && "left ipostdom is merge");
        assert(pdt.immediatePostDominator(&bRight) == &bMerge && "right ipostdom is merge");
        assert(pdt.immediatePostDominator(&bEntry) == &bMerge && "entry ipostdom is merge");

        assert(pdt.postDominates(&bMerge, &bEntry) && "merge pdoms entry");
        assert(pdt.postDominates(&bExit, &bEntry) && "exit pdoms entry");
        assert(pdt.postDominates(&bMerge, &bLeft) && "merge pdoms left");
        assert(pdt.postDominates(&bMerge, &bRight) && "merge pdoms right");

        assert(!pdt.postDominates(&bLeft, &bEntry) && "left does not pdom entry");
        assert(!pdt.postDominates(&bRight, &bEntry) && "right does not pdom entry");
        assert(!pdt.postDominates(&bLeft, &bRight) && "left does not pdom right");
        assert(!pdt.postDominates(&bRight, &bLeft) && "right does not pdom left");
    }

    // -------------------------------------------------------------------------
    // Test 3: Multiple exits — two independent paths, no common block before exit
    //
    //   entry
    //   +-- left  --+-- exit1
    //   +-- right --+-- exit2
    //
    // Expected post-dominator tree:
    //   exit1 .ipostdom = nullptr      (exit block)
    //   exit2 .ipostdom = nullptr      (exit block)
    //   left  .ipostdom = exit1
    //   right .ipostdom = exit2
    //   entry .ipostdom = nullptr      (virtual exit — no concrete block pdoms entry)
    //
    // Since entry has two paths to different exits, the only common post-dominator
    // is the virtual exit.  Neither left nor right post-dominates entry.
    // -------------------------------------------------------------------------
    {
        Function &fn = b.startFunction("multi_exit", Type(Type::Kind::Void), {});
        b.createBlock(fn, "entry");
        b.createBlock(fn, "left");
        b.createBlock(fn, "right");
        b.createBlock(fn, "exit1");
        b.createBlock(fn, "exit2");
        Block &bEntry = fn.blocks[0];
        Block &bLeft = fn.blocks[1];
        Block &bRight = fn.blocks[2];
        Block &bExit1 = fn.blocks[3];
        Block &bExit2 = fn.blocks[4];

        b.setInsertPoint(bEntry);
        b.cbr(Value::constBool(true), bLeft, {}, bRight, {});
        b.setInsertPoint(bLeft);
        b.br(bExit1, {});
        b.setInsertPoint(bRight);
        b.br(bExit2, {});
        b.setInsertPoint(bExit1);
        b.emitRet(std::nullopt, {});
        b.setInsertPoint(bExit2);
        b.emitRet(std::nullopt, {});

        CFGContext ctx(m);
        PostDomTree pdt = computePostDominatorTree(ctx, fn);

        assert(pdt.immediatePostDominator(&bExit1) == nullptr && "exit1 ipostdom is virtual exit");
        assert(pdt.immediatePostDominator(&bExit2) == nullptr && "exit2 ipostdom is virtual exit");
        assert(pdt.immediatePostDominator(&bLeft) == &bExit1 && "left ipostdom is exit1");
        assert(pdt.immediatePostDominator(&bRight) == &bExit2 && "right ipostdom is exit2");
        assert(pdt.immediatePostDominator(&bEntry) == nullptr && "entry ipostdom is virtual exit");

        // No concrete block post-dominates entry (two separate exit paths)
        assert(!pdt.postDominates(&bLeft, &bEntry) && "left does not pdom entry");
        assert(!pdt.postDominates(&bRight, &bEntry) && "right does not pdom entry");
        assert(!pdt.postDominates(&bExit1, &bEntry) && "exit1 does not pdom entry");
        assert(!pdt.postDominates(&bExit2, &bEntry) && "exit2 does not pdom entry");

        // left pdoms itself; exit1 pdoms left
        assert(pdt.postDominates(&bLeft, &bLeft) && "left pdoms itself");
        assert(pdt.postDominates(&bExit1, &bLeft) && "exit1 pdoms left");
        assert(pdt.postDominates(&bExit2, &bRight) && "exit2 pdoms right");
    }

    return 0;
}
