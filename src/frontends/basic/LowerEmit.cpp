//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/frontends/basic/LowerEmit.cpp
// Purpose: Implements program-level emission orchestration for BASIC lowering.
// Key invariants: Block labels are deterministic via BlockNamer or mangler.
// Ownership/Lifetime: Operates on Lowerer state without owning AST or module.
// Links: docs/internals/codemap.md
//
//===----------------------------------------------------------------------===//

/**
 * @file LowerEmit.cpp
 * @brief Implements staged emission of the synthetic BASIC @c main function.
 *
 * This translation unit pre-emits procedure bodies, constructs one block for
 * each distinct virtual main-body line, discovers and allocates symbols, and
 * finally emits the main statements plus resource-release epilogue.
 */

#include "frontends/basic/ASTUtils.hpp"
#include "frontends/basic/LineUtils.hpp"
#include "frontends/basic/Lowerer.hpp"
#include "zanna/il/Module.hpp"

#include <cassert>
#include <cstdlib>
#include <unordered_set>

using namespace il::core;

namespace il::frontends::basic {

/// @brief Predeclare procedures and gather main-body statement handles.
///
/// @details The pass first registers procedure signatures so forward calls
///          resolve correctly, then fully lowers procedure declarations from
///          both the dedicated procedure list and recursively nested namespace
///          bodies. Finally it borrows raw pointers to the top-level main
///          statements for the remaining stages.
///
/// @param prog Parsed BASIC program.
/// @return Emission context containing non-owning main-statement pointers.
/// @pre @p prog and its statement nodes must outlive all consumers of the result.
Lowerer::ProgramEmitContext Lowerer::collectProgramDeclarations(const Program &prog) {
    collectProcedureSignatures(prog);
    for (const auto &s : prog.procs) {
        if (auto *fn = as<const FunctionDecl>(*s))
            lowerFunctionDecl(*fn);
        else if (auto *sub = as<const SubDecl>(*s))
            lowerSubDecl(*sub);
    }

    // Also predeclare and lower procedures declared inside namespace blocks in the main body
    // so fully-qualified calls can resolve at runtime.
    /// @brief Recursively lowers procedure declarations contained in namespace bodies.
    /// @param stmts Statement list to scan.
    std::function<void(const std::vector<StmtPtr> &)> scan;
    scan = [&](const std::vector<StmtPtr> &stmts) {
        for (const auto &stmtPtr : stmts) {
            if (!stmtPtr)
                continue;
            switch (stmtPtr->stmtKind()) {
                case Stmt::Kind::NamespaceDecl:
                    scan(static_cast<const NamespaceDecl &>(*stmtPtr).body);
                    break;
                case Stmt::Kind::FunctionDecl:
                    lowerFunctionDecl(static_cast<const FunctionDecl &>(*stmtPtr));
                    break;
                case Stmt::Kind::SubDecl:
                    lowerSubDecl(static_cast<const SubDecl &>(*stmtPtr));
                    break;
                default:
                    break;
            }
        }
    };
    scan(prog.main);

    ProgramEmitContext state;
    state.mainStmts.reserve(prog.main.size());
    for (const auto &stmt : prog.main)
        state.mainStmts.push_back(stmt.get());
    return state;
}

/// @brief Create the `@main` function shell and associated basic blocks.
///
/// @details Steps performed:
///          1. Reset per-procedure book-keeping (virtual lines, temporary IDs).
///          2. Start a new function returning `I64` with the canonical `entry`
///             block.
///          3. Preallocate per-line blocks so statement lowering can jump
///             deterministically.
///          4. Append a dedicated `exit` block recorded in the context for
///             epilogue emission.
///
/// @param state Mutable emission context storing block references.
/// @pre @p state contains valid main-statement pointers.
/// @post @p state names the new function and its entry block; the procedure
///       context records the synthetic exit block.
void Lowerer::buildMainFunctionSkeleton(ProgramEmitContext &state) {
    // BUG-063 fix: Clear any deferred temps from prior procedures
    clearDeferredTemps();

    build::IRBuilder &b = *builder;
    ProcedureContext &ctx = context();

    stmtVirtualLines_.clear();
    synthSeq_ = 0;
    ctx.blockNames().lineBlocks().clear();

    Function &f = b.startFunction("main", Type(Type::Kind::I64), {});
    state.function = &f;
    ctx.setFunction(&f);
    ctx.setNextTemp(f.valueNames.size());

    b.addBlock(f, "entry");

    auto &lineBlocks = ctx.blockNames().lineBlocks();
    for (const auto *stmt : state.mainStmts) {
        int vLine = virtualLine(*stmt);
        if (lineBlocks.find(vLine) != lineBlocks.end())
            continue;
        size_t blockIdx = f.blocks.size();
        b.addBlock(
            f, mangler.block((hasUserLine(vLine) ? "L" : "UL") + std::to_string(std::abs(vLine))));
        lineBlocks[vLine] = blockIdx;
    }
    ctx.setExitIndex(f.blocks.size());
    b.addBlock(f, mangler.block("exit"));

    state.entry = &f.blocks.front();
}

/// @brief Discover locals referenced by the main statement sequence.
///
/// @details Clears symbol tracking and walks the recorded main statements to
///          mark every variable, array, and object used in the outer program
///          body so storage can be allocated before emission.
///
/// @param state Emission context containing the main statement list.
/// @post Previously collected symbol state has been replaced with symbols
///       reachable from @p state.mainStmts.
void Lowerer::collectMainVariables(ProgramEmitContext &state) {
    resetSymbolState();
    collectVars(state.mainStmts);
}

/// @brief Assign storage slots for main-function locals and parameters.
///
/// @details Asserts that skeleton construction produced an entry block, selects
///          it as the current insertion point, and invokes the normal slot
///          allocator with an empty parameter-name set.
///
/// @param state Emission context seeded by @ref buildMainFunctionSkeleton.
/// @pre @p state.entry is non-null.
void Lowerer::allocateMainLocals(ProgramEmitContext &state) {
    ProcedureContext &ctx = context();
    assert(state.entry && "buildMainFunctionSkeleton must run before allocateMainLocals");
    ctx.setCurrent(state.entry);
    allocateLocalSlots(std::unordered_set<std::string>(), /*includeParams=*/true);
}

/// @brief Lower the main statement list and append a terminating epilogue.
///
/// @details An empty main body receives a return in its entry block. For a
///          non-empty body, each cached statement is lowered with its own source
///          location; unterminated fallthrough and unused preallocated line
///          blocks are explicitly branched to the exit. In both cases the exit
///          block releases deferred values plus object, array, and array-parameter
///          locals before returning zero.
///
/// @param state Emission context describing the main function layout.
/// @pre @p state.function and its entry/exit block bookkeeping were initialized
///      by @ref buildMainFunctionSkeleton.
void Lowerer::emitMainBodyAndEpilogue(ProgramEmitContext &state) {
    ProcedureContext &ctx = context();
    assert(state.function && "buildMainFunctionSkeleton must populate function");

    if (state.mainStmts.empty()) {
        curLoc = {};
        emitRet(Value::constInt(0));
    } else {
        ctx.setCurrent(state.entry);
        /// @brief Updates the lowerer's current source location before a statement.
        /// @param stmt Statement about to be lowered.
        lowerStatementSequence(state.mainStmts,
                               /*stopOnTerminated=*/false,
                               [&](const Stmt &stmt) { curLoc = stmt.loc; });

        // Ensure fallthrough to the exit block is explicit. The verifier
        // requires every basic block to end with a terminator.
        if (ctx.current() && !ctx.current()->terminated)
            emitBr(&state.function->blocks[ctx.exitIndex()]);

        // BUG-052 guard: Some preallocated per-line blocks may remain unused
        // (no instructions emitted). Fill truly empty blocks with an explicit
        // branch to the exit block so the verifier does not report "empty block".
        for (std::size_t i = 0; i < state.function->blocks.size(); ++i) {
            if (i == 0 || i == static_cast<std::size_t>(ctx.exitIndex()))
                continue; // skip entry and exit
            auto &bb = state.function->blocks[i];
            if (bb.instructions.empty()) {
                ctx.setCurrent(&bb);
                emitBr(&state.function->blocks[ctx.exitIndex()]);
            }
        }
    }

    ctx.setCurrent(&state.function->blocks[ctx.exitIndex()]);
    curLoc = {};
    releaseDeferredTemps();
    releaseObjectLocals(std::unordered_set<std::string>{});
    releaseArrayLocals(std::unordered_set<std::string>{});
    releaseArrayParams(std::unordered_set<std::string>{});
    curLoc = {};
    emitRet(Value::constInt(0));
}

/// @brief Execute the full lowering pipeline for a BASIC program.
///
/// @details Caches module-level object-array element types before procedure
///          lowering, then runs declaration collection, skeleton creation, and
///          variable discovery. The OOP module initializer call is emitted at
///          the main entry before local slots, after which local allocation and
///          body/epilogue emission complete the function.
///
/// @param prog BASIC program to lower into IL.
/// @pre The Lowerer is bound to a live module, builder, and procedure context.
void Lowerer::emitProgram(const Program &prog) {
    // BUG-097 fix: Cache module-level object array types BEFORE lowering procedures.
    // Procedure bodies may reference global arrays (e.g., g_widgets(i).Update()),
    // and resolveObjectClass needs the element class info during procedure lowering.
    cacheModuleObjectArraysFromAST(prog.main);

    ProgramEmitContext state = collectProgramDeclarations(prog);
    buildMainFunctionSkeleton(state);
    collectMainVariables(state);
    // Ensure OOP module init runs before main body (interfaces registered, bindings bound).
    // Switch to main entry before invoking the module initializer.
    context().setCurrent(state.entry);
    emitCall("__mod_init$oop", {});
    allocateMainLocals(state);
    emitMainBodyAndEpilogue(state);
}

} // namespace il::frontends::basic
