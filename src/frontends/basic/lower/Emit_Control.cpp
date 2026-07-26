//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE in the project root for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/lower/Emit_Control.cpp
// Purpose: Provide control-flow lowering primitives used by the BASIC lowerer
//          to construct IL branch and terminator instructions.
// Key invariants: Each helper preserves the Lowerer's notion of the "current"
//                 block and emits at most one terminator per block so CFGs stay
//                 structurally valid.
// Ownership/Lifetime: Procedures borrow the owning @ref Lowerer state and write
//                     into IL blocks managed by the @ref ProcedureContext.
// Links: docs/internals/codemap.md
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements control-flow emission helpers for the BASIC lowerer.
/// @details Routines in this translation unit manipulate the active block,
/// append terminators, and manage exception-handling stacks. Callers must
/// establish the desired current block before invoking these helpers; each
/// helper ensures terminators are emitted exactly once and that the lowerer
/// preserves ownership of temporary values managed by the ProcedureContext.

#include "frontends/basic/ASTUtils.hpp"
#include "frontends/basic/Lowerer.hpp"
#include "frontends/basic/lower/Emitter.hpp"

#include "zanna/il/Module.hpp"

#include <cassert>

using namespace il::core;

namespace il::frontends::basic {

/// @brief Increment a loop induction slot by the given step value.
///
/// @details Loads the current value from @p slot, adds @p step using an
///          overflow-checking opcode, and stores the result back to the same
///          slot.  The routine assumes the caller has set @ref curLoc so any
///          overflow trap reports the correct BASIC source line.
/// @param slot Stack slot holding the loop induction variable.
/// @param step Value representing the stride applied each iteration.
void Lowerer::emitForStep(Value slot, Value step) {
    Value load = emitLoad(Type(Type::Kind::I64), slot);
    Value add = emitCommon(curLoc).add_checked(load, step, OverflowPolicy::Checked);
    emitStore(Type(Type::Kind::I64), slot, add);
}

/// @brief Emit an unconditional branch to @p target.
///
/// @details Delegates to the shared @ref Emitter instance so block bookkeeping
///          and debug metadata remain centralised.  The helper is the common
///          exit path once a block's body has been fully lowered.
/// @param target Destination block that becomes the current fallthrough.
void Lowerer::emitBr(BasicBlock *target) {
    emitter().emitBr(target);
}

/// @brief Emit a conditional branch guarded by @p cond.
///
/// @details Generates an @c br.cond instruction that jumps to @p t when the
///          boolean value stored in @p cond is true or @p f otherwise.
///          Delegating to @ref emitter() ensures phi arguments and metadata are
///          wired consistently across translation units.
/// @param cond SSA value containing the branch condition.
/// @param t Block reached on the true edge.
/// @param f Block reached on the false edge.
void Lowerer::emitCBr(Value cond, BasicBlock *t, BasicBlock *f) {
    emitter().emitCBr(cond, t, f);
}

/// @brief Lower a BASIC boolean expression into branching control flow.
///
/// @details Handles short-circuit expressions by splitting them into auxiliary
///          blocks when necessary; simple expressions fall back to evaluating
///          the expression and emitting a standard conditional branch.  The
///          routine preserves the active block on entry and restores it for the
///          caller once branch emission completes.
/// @param expr AST expression whose truthiness drives control flow.
/// @param trueBlk Block entered when the expression evaluates to true.
/// @param falseBlk Block entered when the expression evaluates to false.
/// @param loc Source location used for diagnostics when coercions fail.
void Lowerer::lowerCondBranch(const Expr &expr,
                              BasicBlock *trueBlk,
                              BasicBlock *falseBlk,
                              il::support::SourceLoc loc) {
    if (const auto *bin = as<const BinaryExpr>(expr)) {
        const bool isAnd = bin->op == BinaryExpr::Op::LogicalAnd;
        const bool isOr = bin->op == BinaryExpr::Op::LogicalOr;
        if (isAnd || isOr) {
            ProcedureContext &ctx = context();
            Function *func = ctx.function();
            assert(func && ctx.current());

            /// @brief Resolves a block pointer to its stable function index.
            /// @param bb Block owned by the active procedure.
            /// @return Index of `bb` in the active function.
            auto indexOf = [&](BasicBlock *bb) {
                assert(bb && "lowerCondBranch requires non-null block");
                return ctx.blockIndex(bb);
            };

            size_t curIdx = indexOf(ctx.current());
            size_t trueIdx = indexOf(trueBlk);
            size_t falseIdx = indexOf(falseBlk);

            BlockNamer *blockNamer = ctx.blockNames().namer();
            std::string hint = isAnd ? "and_rhs" : "or_rhs";
            std::string midLbl = blockNamer ? blockNamer->generic(hint) : mangler.block(hint);

            size_t midIdx = func->blocks.size();
            builder->addBlock(*func, midLbl);

            func = ctx.function();
            BasicBlock *mid = &func->blocks[midIdx];
            BasicBlock *cur = &func->blocks[curIdx];
            BasicBlock *trueTarget = &func->blocks[trueIdx];
            BasicBlock *falseTarget = &func->blocks[falseIdx];
            ctx.setCurrent(cur);

            if (isAnd)
                lowerCondBranch(*bin->lhs, mid, falseTarget, loc);
            else
                lowerCondBranch(*bin->lhs, trueTarget, mid, loc);

            func = ctx.function();
            mid = &func->blocks[midIdx];
            trueTarget = &func->blocks[trueIdx];
            falseTarget = &func->blocks[falseIdx];
            ctx.setCurrent(mid);

            lowerCondBranch(*bin->rhs, trueTarget, falseTarget, loc);
            return;
        }
    }

    // BUG-101 fix: Capture block indices before lowerExpr, which may add blocks
    // (e.g., array bounds checks) and invalidate the trueBlk/falseBlk pointers
    ProcedureContext &ctx = context();
    Function *func = ctx.function();
    /// @brief Resolves a block pointer to its stable function index.
    /// @param bb Block owned by the active procedure.
    /// @return Index of `bb` in the active function.
    auto indexOf = [&](BasicBlock *bb) {
        assert(bb && "lowerCondBranch requires non-null block");
        return ctx.blockIndex(bb);
    };
    size_t trueIdx = indexOf(trueBlk);
    size_t falseIdx = indexOf(falseBlk);

    RVal cond = lowerExpr(expr);
    cond = coerceToBool(std::move(cond), loc);

    // Refresh pointers after potential reallocation
    func = ctx.function();
    BasicBlock *trueTarget = &func->blocks[trueIdx];
    BasicBlock *falseTarget = &func->blocks[falseIdx];
    emitCBr(cond.value, trueTarget, falseTarget);
}

/// @brief Push a new exception handler for the active procedure.
///
/// @details Forwards to the @ref Emitter so the lowerer can install handler
///          metadata that mirrors runtime semantics.  Used when lowering BASIC
///          @c ON @c ERROR constructs.
/// @param handler Entry block for the new handler region.
void Lowerer::emitEhPush(BasicBlock *handler) {
    emitter().emitEhPush(handler);
}

/// @brief Pop the most recently pushed exception handler.
///
/// @details Invoked when leaving protected regions so the runtime does not
///          retain stale handlers.  Delegates to the shared emitter instance.
void Lowerer::emitEhPop() {
    emitter().emitEhPop();
}

/// @brief Pop an exception handler as part of a return path.
///
/// @details Mirrors @ref emitEhPop but signals to the emitter that the pop is
///          happening during a return, allowing it to update bookkeeping that
///          tracks pending handlers.
void Lowerer::emitEhPopForReturn() {
    emitter().emitEhPopForReturn();
}

/// @brief Clear any active error handler metadata from the lowering context.
///
/// @details Invoked when BASIC code disables @c ON @c ERROR or when a handler
///          scope expires.  Ensures subsequent statements observe a clean error
///          state.
void Lowerer::clearActiveErrorHandler() {
    emitter().clearActiveErrorHandler();
}

/// @brief Retrieve (or lazily create) the IL block backing a BASIC error handler.
///
/// @details The emitter owns the cache mapping handler line numbers to IL
///          blocks.  This wrapper ensures all lowering sites request blocks
///          through a single code path so diagnostics remain consistent.
/// @param targetLine Source line number referencing the handler label.
/// @return Pointer to the block that should receive handler code.
BasicBlock *Lowerer::ensureErrorHandlerBlock(int targetLine) {
    return emitter().ensureErrorHandlerBlock(targetLine);
}

/// @brief Emit a return instruction with a value.
///
/// @details Directly forwards to the emitter, which manages handler unwinding
///          and ensures the appropriate terminators are appended exactly once.
/// @param v SSA value to return to the caller.
void Lowerer::emitRet(Value v) {
    emitter().emitRet(v);
}

/// @brief Emit a void return instruction.
///
/// @details Complements @ref emitRet for procedures that do not produce a
///          result, allowing the emitter to reuse its centralised terminator
///          logic.
void Lowerer::emitRetVoid() {
    emitter().emitRetVoid();
}

/// @brief Emit a generic trap instruction signalling a runtime failure.
///
/// @details Useful for lowering constructs that must abort execution (for
///          example, invalid @c EXIT usage).  The emitter handles wiring in the
///          correct opcode and metadata.
void Lowerer::emitTrap() {
    emitter().emitTrap();
}

/// @brief Emit a trap that derives its runtime error from an IL value.
///
/// @details Delegates to the emitter so the generated instruction references
///          @p errCode and participates in the Lowerer's diagnostics tracking.
/// @param errCode Register holding the runtime error code.
void Lowerer::emitTrapFromErr(Value errCode) {
    emitter().emitTrapFromErr(errCode);
}

} // namespace il::frontends::basic
