//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
// File: src/frontends/basic/lower/Lowerer_Errors.cpp
// Purpose: Centralises diagnostic handling and error-related lowering helpers
//          for the BASIC front-end lowerer.
// Key invariants: Diagnostic emitters are optional; runtime error helpers only
//                 operate when a procedure context is active.
// Ownership/Lifetime: Borrowed DiagnosticEmitter and Lowerer state; no AST
//                     ownership.
// Links: docs/internals/codemap.md, docs/tutorials/basic-tutorial.md
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements BASIC lowerer diagnostics, source-location access, and
///        runtime error-control-flow helpers.
/// @details The methods attach optional semantic/diagnostic services, normalize
///          channel values, and build failure/continuation blocks while
///          preserving the active procedure context and source metadata.

#include "frontends/basic/DiagnosticEmitter.hpp"
#include "frontends/basic/Lowerer.hpp"

#include "zanna/il/Module.hpp"

#include <functional>
#include <string_view>
#include <utility>

namespace il::frontends::basic {

// NOTE (TRY/CATCH interop):
// The label-driven ON ERROR/RESUME lowering manipulates ErrorHandlerState
// (active/line/index) in the ProcedureContext so later RESUME statements can
// find the appropriate handler and token. In contrast, TRY/CATCH lowering
// intentionally uses only eh.push/eh.pop without mutating the active state.
// This allows a TRY to nest over an existing ON ERROR GOTO handler and ensures
// that leaving TRY restores the prior handler automatically with a single pop.
// See Lower_TryCatch.cpp for details.

/// @brief Attach a diagnostic emitter to the lowering pipeline.
/// @details Stores the supplied @p emitter for later use and wires its reporting
///          callbacks into @ref TypeRules so that type-check errors surface
///          through the same sink as lowering diagnostics.  Passing `nullptr`
///          detaches the sink and restores the default behaviour.
/// @param emitter Optional diagnostic sink to receive error reports.
void Lowerer::setDiagnosticEmitter(DiagnosticEmitter *emitter) noexcept {
    diagnosticEmitter_ = emitter;
    if (emitter) {
        TypeRules::setTypeErrorSink([emitter](const TypeRules::TypeError &error) {
            emitter->emit(il::support::Severity::Error,
                          error.code,
                          il::support::SourceLoc{},
                          0,
                          error.message);
        });
    } else {
        TypeRules::setTypeErrorSink({});
    }
}

/// @brief Retrieve the diagnostic emitter associated with the lowering pass.
/// @details Returns the previously installed emitter without transferring
///          ownership.  A null result indicates that diagnostics should be
///          suppressed or routed elsewhere by the caller.
/// @return Pointer to the active diagnostic emitter or `nullptr`.
DiagnosticEmitter *Lowerer::diagnosticEmitter() const noexcept {
    return diagnosticEmitter_;
}

/// @brief Attach semantic analyzer to provide variable type information.
/// @details Stores the supplied @p analyzer for later use during lowering to
///          query variable types determined during semantic analysis. This allows
///          the lowerer to use value-based type inference instead of only
///          suffix-based inference. Passing `nullptr` detaches the analyzer.
/// @param analyzer Optional semantic analyzer providing type information.
void Lowerer::setSemanticAnalyzer(const SemanticAnalyzer *analyzer) noexcept {
    semanticAnalyzer_ = analyzer;
}

/// @brief Retrieve the semantic analyzer associated with the lowering pass.
/// @details Returns the previously installed analyzer without transferring
///          ownership.  A null result indicates that semantic type information
///          is not available.
/// @return Pointer to the active semantic analyzer or `nullptr`.
const SemanticAnalyzer *Lowerer::semanticAnalyzer() const noexcept {
    return semanticAnalyzer_;
}

// =============================================================================
// Source Location Accessors
// =============================================================================

/// @brief Access the mutable source location for IR emission.
/// @details Provides controlled access to the current location so RAII helpers
///          and emission utilities can manage location state without friendship.
/// @return Reference to the mutable source location field.
il::support::SourceLoc &Lowerer::sourceLocation() noexcept {
    return curLoc;
}

/// @brief Access the immutable source location for IR emission.
/// @details Const overload for read-only location queries.
/// @return Reference to the immutable source location field.
const il::support::SourceLoc &Lowerer::sourceLocation() const noexcept {
    return curLoc;
}

/// @brief Set the current source location for IR emission.
/// @details Convenience method for setting the location in a single call.
/// @param loc New source location to use for subsequent emissions.
void Lowerer::setSourceLocation(il::support::SourceLoc loc) noexcept {
    curLoc = loc;
}

/// @brief Coerce a BASIC I/O channel value to the 32-bit integer domain.
/// @details Accepts either 32-bit or 64-bit integer expressions.  When a
///          64-bit value is supplied, it inserts a narrowing conversion into the
///          current basic block using @ref emitCommon.  The resulting value is
///          tagged with the 32-bit type so later stages observe the canonical
///          representation.
/// @param channel Lowered r-value representing the channel operand.
/// @param loc Source location used for any generated instructions.
/// @return Normalised r-value guaranteed to carry the 32-bit integer type.
Lowerer::RVal Lowerer::normalizeChannelToI32(RVal channel, il::support::SourceLoc loc) {
    if (channel.type.kind == Type::Kind::I32)
        return channel;

    channel = ensureI64(std::move(channel), loc);
    channel.value = emitCommon(loc).narrow_to(channel.value, 64, 32);
    channel.type = Type(Type::Kind::I32);
    return channel;
}

/// @brief Emit a branch that checks a runtime error flag and handles failures.
/// @details Spills the @p err value to stack so it can be safely reloaded as a
///          64-bit operand, compares it against zero, and materialises a pair of
///          continuation/failure blocks named using @p labelStem.  When the flag
///          is non-zero, control transfers to a new failure block where
///          @p onFailure is invoked to generate diagnostics or cleanup code.  On
///          success, control resumes in the continuation block, preserving the
///          original block ordering.
/// @param err Value representing the runtime error flag to inspect.
/// @param loc Source location applied to generated instructions.
/// @param labelStem Stem used to derive unique failure/continuation block labels.
/// @param onFailure Callback invoked within the failure block to emit handling code.
void Lowerer::emitRuntimeErrCheck(Value err,
                                  il::support::SourceLoc loc,
                                  std::string_view labelStem,
                                  const std::function<void(Value)> &onFailure) {
    ProcedureContext &ctx = context();
    Function *func = ctx.function();
    BasicBlock *original = ctx.current();
    if (!func || !original)
        return;

    size_t curIdx = ctx.blockIndex(original);
    BlockNamer *blockNamer = ctx.blockNames().namer();
    std::string stem(labelStem);
    std::string failLbl =
        blockNamer ? blockNamer->generic(stem + "_fail") : mangler.block(stem + "_fail");
    std::string contLbl =
        blockNamer ? blockNamer->generic(stem + "_cont") : mangler.block(stem + "_cont");

    size_t failIdx = func->blocks.size();
    builder->addBlock(*func, failLbl);
    size_t contIdx = func->blocks.size();
    builder->addBlock(*func, contLbl);

    BasicBlock *failBlk = &func->blocks[failIdx];
    BasicBlock *contBlk = &func->blocks[contIdx];

    ctx.setCurrent(&func->blocks[curIdx]);
    curLoc = loc;

    Value err64 = err;
    {
        Value scratch = emitAlloca(sizeof(int64_t));
        emitStore(Type(Type::Kind::I64), scratch, Value::constInt(0));
        emitStore(Type(Type::Kind::I32), scratch, err);
        err64 = emitLoad(Type(Type::Kind::I64), scratch);
    }

    Value isFail = emitBinary(Opcode::ICmpNe, ilBoolTy(), err64, Value::constInt(0));
    emitCBr(isFail, failBlk, contBlk);

    ctx.setCurrent(failBlk);
    curLoc = loc;
    onFailure(err);

    ctx.setCurrent(contBlk);
}

} // namespace il::frontends::basic
