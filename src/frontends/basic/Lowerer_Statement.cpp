//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/Lowerer_Statement.cpp
// Purpose: Sequence a procedure body's numbered BASIC statements into IL blocks,
//          wiring GOSUB continuations, fallthrough branches, and ON ERROR setup.
// Key invariants:
//   - Every statement is lowered into the pre-created block of its virtual line.
//   - A body with ON ERROR GOTO enters through the dispatcher arm, which is
//     completed after the last statement.
// Ownership/Lifetime:
//   - Borrows the owning Lowerer and its procedure context.
//   - Holds no IL objects beyond the duration of a call.
// Links: src/frontends/basic/lower/Lower_TryCatch.cpp, docs/internals/codemap.md
//
//===----------------------------------------------------------------------===//
//
/// @file Lowerer_Statement.cpp
/// @brief Statement lowering utilities for the BASIC front end.
/// @details The @ref StatementLowering helper coordinates between numbered BASIC
///          lines and the IL block graph, wiring up branches, gosub continuations,
///          and fallthrough logic while reusing the owning @ref Lowerer state.

#include "frontends/basic/ASTUtils.hpp"
#include "frontends/basic/Lowerer.hpp"
#include "frontends/basic/LoweringPipeline.hpp"

#include <cassert>
#include <functional>

namespace il::frontends::basic {

/// @brief Construct a lowering helper bound to a borrowed @ref Lowerer.
/// @details Stores a reference to the parent lowerer so helper routines can
///          access shared state such as the current lowering context, gosub
///          stacks, and basic-block tables.
/// @param lowerer Parent lowerer that must outlive this helper.
StatementLowering::StatementLowering(Lowerer &lowerer) : lowerer(lowerer) {}

/// @brief Lower a sequential list of BASIC statements into IL blocks.
/// @details Establishes gosub continuation state, emits an initial branch from
///          the caller into the first numbered block, and then iterates over the
///          statements, lowering each in turn.  After visiting a statement the
///          helper either stops (when @p stopOnTerminated is true and a
///          terminator was emitted) or stitches a branch to the next block while
///          allowing @p beforeBranch to inject custom behaviour.
/// @param stmts Statement pointers in execution order.
/// @param stopOnTerminated When true the loop exits once a terminator is seen.
/// @param beforeBranch Optional hook invoked immediately before emitting a
///        fallthrough branch.
/// @pre @p stmts contains no null pointers, an active function exists, and its
///      line-block map contains every statement's virtual line.
void StatementLowering::lowerSequence(const std::vector<const Stmt *> &stmts,
                                      bool stopOnTerminated,
                                      const std::function<void(const Stmt &)> &beforeBranch) {
    if (stmts.empty())
        return;

    lowerer.curLoc = {};
    auto &ctx = lowerer.context();
    auto *func = ctx.function();
    assert(func && "lowerSequence requires an active function");
    auto &lineBlocks = ctx.blockNames().lineBlocks();

    // Note: clearContinuations() removed - gosub continuations must persist
    // across all sequences in a procedure since RETURN needs visibility of
    // ALL gosub sites, not just those in the current sequence.

    /// Register GOSUB continuations at the current statement level and through
    /// nested StmtList nodes, reusing the enclosing statement's next block.
    std::function<void(const Stmt *, size_t)> scanForGosub;
    scanForGosub = [&](const Stmt *stmt, size_t nextIdx) {
        if (const auto *gosubStmt = as<const GosubStmt>(*stmt)) {
            ctx.gosub().registerContinuation(gosubStmt, nextIdx);
        } else if (const auto *stmtList = as<const StmtList>(*stmt)) {
            // Recursively scan statements in the list
            for (const auto &childStmt : stmtList->stmts)
                scanForGosub(childStmt.get(), nextIdx);
        }
    };

    bool hasGosub = false;
    for (size_t i = 0; i < stmts.size(); ++i) {
        size_t contIdx = ctx.exitIndex();
        if (i + 1 < stmts.size()) {
            int nextLine = lowerer.virtualLine(*stmts[i + 1]);
            contIdx = lineBlocks[nextLine];
        }

        // Check if this statement (or any nested statement) is a GOSUB
        if (as<const GosubStmt>(*stmts[i]) || as<const StmtList>(*stmts[i])) {
            scanForGosub(stmts[i], contIdx);
            hasGosub = true;
        }
    }

    if (hasGosub)
        lowerer.ensureGosubStack();

    // A body with ON ERROR GOTO enters through the dispatcher's arm block instead.
    const size_t firstBlock = lineBlocks[lowerer.virtualLine(*stmts.front())];
    if (!lowerer.prepareErrorHandling(stmts, firstBlock)) {
        func = ctx.function();
        lowerer.emitBr(&func->blocks[firstBlock]);
    }

    for (size_t i = 0; i < stmts.size(); ++i) {
        const Stmt &stmt = *stmts[i];
        int vLine = lowerer.virtualLine(stmt);

        func = ctx.function();
        ctx.setCurrent(&func->blocks[lineBlocks[vLine]]);

        lowerer.lowerStmt(stmt);

        auto *current = ctx.current();
        if (current && current->terminated) {
            if (stopOnTerminated)
                break;
            continue;
        }

        func = ctx.function();
        auto *next = (i + 1 < stmts.size())
                         ? &func->blocks[lineBlocks[lowerer.virtualLine(*stmts[i + 1])]]
                         : &func->blocks[ctx.exitIndex()];
        if (beforeBranch)
            beforeBranch(stmt);
        lowerer.emitBr(next);
    }

    lowerer.finalizeErrorHandling();
}

/// @brief Forward sequence lowering to the owned StatementLowering facade.
/// @param stmts Non-null statement pointers in execution order.
/// @param stopOnTerminated Whether the facade stops at the first terminated block.
/// @param beforeBranch Optional hook before normal fallthrough branches.
void Lowerer::lowerStatementSequence(const std::vector<const Stmt *> &stmts,
                                     bool stopOnTerminated,
                                     const std::function<void(const Stmt &)> &beforeBranch) {
    statementLowering->lowerSequence(stmts, stopOnTerminated, beforeBranch);
}

} // namespace il::frontends::basic
