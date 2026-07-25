//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// This file declares the modular lowering helper components that compose the
// BASIC lowering pipeline, transforming AST nodes into IL instructions.
//
// Lowering Pipeline Architecture:
// The BASIC lowering process is decomposed into specialized helper components,
// each responsible for a specific category of AST nodes or operations:
//
// - ProgramLowering: Top-level program structure, global initialization
// - ProcedureLowering: SUB/FUNCTION declarations and procedure bodies
// - StatementLowering: Statement-level lowering dispatch
// - LowerExprNumeric: Arithmetic and numeric expressions
// - LowerExprLogical: Boolean and comparison expressions
// - LowerExprBuiltin: Built-in function calls
// - LowerStmt_Core: Assignment and basic statements
// - LowerStmt_Control: Control flow (IF, FOR, WHILE, SELECT)
// - LowerStmt_IO: I/O operations (PRINT, INPUT)
// - LowerRuntime: Runtime library function declarations
//
// Design Philosophy:
// Rather than a monolithic Lowerer class, the pipeline is decomposed into
// focused helper components that:
// - Encapsulate specific lowering concerns
// - Share common Lowerer state (IRBuilder, NameMangler, symbol tables)
// - Can be tested independently
// - Are easier to understand and maintain
//
// Helper Coordination:
// All helpers:
// - Borrow a Lowerer instance for state access
// - Do not take ownership of AST or IL module
// - Use IRBuilder for IL instruction emission
// - Use NameMangler for deterministic symbol naming
// - Return IL values or register names as appropriate
//
// Integration:
// - Composed by: Lowerer class to handle different AST node types
// - Shared state: All helpers access Lowerer state
// - Modular testing: Each helper can be tested in isolation
//
// Design Notes:
// - Helpers are structs with operator() or named methods
// - No ownership transfer; all state is borrowed
// - Consistent interface across helper types
//
//===----------------------------------------------------------------------===//
/// @file LoweringPipeline.hpp
/// @brief Declares the orchestration helpers for BASIC-to-IL lowering.
/// @details ProgramLowering manages one complete module run,
///          ProcedureLowering coordinates the phases for individual procedures,
///          and StatementLowering sequences pre-scheduled statement blocks.
///          Each facade borrows the parent Lowerer and shares its builder,
///          emitter, symbol, runtime, and control-flow state.

#pragma once

#include "frontends/basic/ast/DeclNodes.hpp"
#include "zanna/il/Module.hpp"
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace il::build {
class IRBuilder;
} // namespace il::build

namespace il::frontends::basic {

namespace lower {
class Emitter;
} // namespace lower

class Lowerer;

namespace pipeline_detail {
/// @brief Translate a BASIC AST scalar type into the IL core representation.
/// @param ty BASIC semantic type sourced from the front-end AST.
/// @return Corresponding IL type used for stack slots and temporaries.
il::core::Type coreTypeForAstType(::il::frontends::basic::Type ty);

} // namespace pipeline_detail

/// @brief Infer variable type from semantic analyzer, then suffix, then fallback.
/// @details BUG-001 FIX: Queries semantic analyzer for value-based type inference
///          before falling back to suffix-based naming convention.
/// @param lowerer Lowerer instance providing access to semantic analyzer.
/// @param name Variable name to query.
/// @return Best-effort type derived from semantic analysis or naming convention.
Type inferVariableTypeForLowering(const Lowerer &lowerer, std::string_view name);

/// @brief Coordinates program-level lowering by seeding module state and driving emission.
struct ProgramLowering {
    /// @brief Construct a program-level facade over @p lowerer.
    /// @param lowerer Parent lowering coordinator borrowed by this facade.
    /// @pre @p lowerer outlives the constructed facade.
    explicit ProgramLowering(Lowerer &lowerer);

    /// @brief Lower the full program @p prog into @p module.
    /// @details Resets run-scoped state, scans requirements, emits OOP and
    ///          program bodies, declares requested runtime helpers, and clears
    ///          the parent's temporary builder/module bindings on exit.
    /// @param prog Parsed BASIC program to scan and emit.
    /// @param module Destination module populated by the lowering run.
    void run(const Program &prog, il::core::Module &module);

  private:
    /// Borrowed parent that owns all shared lowering services and caches.
    Lowerer &lowerer;
};

/// @brief Handles procedure signature caching, variable collection, and body emission.
struct ProcedureLowering {
    /// @brief Aggregates borrowed services and derived state for one procedure.
    /// @details The procedure name and derived containers are owned by the
    ///          context. AST vectors, configuration callbacks, and pipeline
    ///          services are borrowed and must remain valid through emission.
    struct LoweringContext {
        /// @brief Construct the phase state for one procedure.
        /// @param lowerer Parent coordinator providing shared lowering state.
        /// @param symbols Mutable symbol map owned by the parent lowerer.
        /// @param builder Active builder used to create the IL function.
        /// @param emitter Low-level instruction emitter used during lowering.
        /// @param name Emitted procedure name copied into this context.
        /// @param params Borrowed formal parameters in declaration order.
        /// @param body Borrowed owning pointers for the procedure body.
        /// @param config Borrowed procedure-specific callbacks and return type.
        /// @pre All referenced services, vectors, and @p config outlive this context.
        LoweringContext(Lowerer &lowerer,
                        SymbolTable::SymbolMap &symbols,
                        il::build::IRBuilder &builder,
                        lower::Emitter &emitter,
                        std::string name,
                        const std::vector<Param> &params,
                        const std::vector<StmtPtr> &body,
                        const Lowerer::ProcedureConfig &config);

        /// Borrowed parent coordinator for procedure-level operations.
        Lowerer &lowerer;
        /// Borrowed mutable alias of the parent's current symbol map.
        SymbolTable::SymbolMap &symbols;
        /// Borrowed builder that creates the procedure function and blocks.
        il::build::IRBuilder &builder;
        /// Borrowed emitter attached to the parent lowering context.
        lower::Emitter &emitter;
        /// Owned emitted name of the procedure being lowered.
        std::string name;
        /// Borrowed formal parameter declarations.
        const std::vector<Param> &params;
        /// Borrowed procedure body containing the owning statement pointers.
        const std::vector<StmtPtr> &body;
        /// Borrowed hooks and return-type configuration for this procedure.
        const Lowerer::ProcedureConfig &config;
        /// Non-owning statement pointers flattened from @ref body.
        std::vector<const Stmt *> bodyStmts;
        /// Canonical parameter names excluded from local allocation or cleanup.
        std::unordered_set<std::string> paramNames;
        /// IL parameter descriptors derived from @ref params.
        std::vector<il::core::Param> irParams;
        /// Number of formal parameters recorded in @ref metadata.
        size_t paramCount{0};
        /// Function created during block scheduling; owned by the destination module.
        il::core::Function *function{nullptr};
        /// Shared ownership of metadata consumed by skeleton and emission phases.
        std::shared_ptr<Lowerer::ProcedureMetadata> metadata;
    };

    /// @brief Construct a procedure-level facade over @p lowerer.
    /// @param lowerer Parent lowering coordinator borrowed by this facade.
    /// @pre @p lowerer outlives the constructed facade.
    explicit ProcedureLowering(Lowerer &lowerer);

    /// @brief Cache declared signatures for all user-defined procedures.
    /// @details Replaces the parent's signature and alias tables with entries
    ///          from top-level and namespace-nested FUNCTION/SUB declarations.
    /// @param prog Program whose declarations are scanned.
    void collectProcedureSignatures(const Program &prog);

    /// @brief Discover variable usage within a statement list.
    /// @param stmts Non-owning statement pointers to traverse; null entries are skipped.
    void collectVars(const std::vector<const Stmt *> &stmts);

    /// @brief Collect variables from every procedure and main body statement.
    /// @param prog Program whose procedure and main statements are traversed.
    void collectVars(const Program &prog);

    /// @brief Build a lowering context describing a BASIC procedure.
    /// @param name Emitted procedure name copied into the returned context.
    /// @param params Borrowed formal parameter declarations.
    /// @param body Borrowed procedure body.
    /// @param config Borrowed procedure callbacks and return configuration.
    /// @return Context bound to the parent's active builder, symbol map, and emitter.
    /// @pre The parent lowerer has an active IR builder.
    LoweringContext makeContext(const std::string &name,
                                const std::vector<Param> &params,
                                const std::vector<StmtPtr> &body,
                                const Lowerer::ProcedureConfig &config);

    /// @brief Reset per-procedure state prior to emission.
    /// @param ctx Active procedure context; currently retained for phase symmetry.
    void resetContext(LoweringContext &ctx);

    /// @brief Collect metadata required for lowering the procedure body.
    /// @details Populates the flattened body, parameter-name set, IL parameters,
    ///          parameter count, and shared metadata record in @p ctx.
    /// @param ctx Context receiving derived procedure metadata.
    void collectProcedureInfo(LoweringContext &ctx);

    /// @brief Construct the IL function skeleton and allocate locals.
    /// @details Starts the function, schedules source-line blocks, materializes
    ///          parameters, and allocates non-parameter local slots.
    /// @param ctx Context whose function and active block state are initialized.
    /// @pre @p ctx has metadata and both required return callbacks.
    void scheduleBlocks(LoweringContext &ctx);

    /// @brief Emit IL instructions for the lowered BASIC procedure.
    /// @details Handles empty bodies or lowers the flattened statement sequence,
    ///          patches unused scheduled blocks, emits exit cleanup, and resets
    ///          the block namer.
    /// @param ctx Context with a scheduled function.
    void emitProcedureIL(LoweringContext &ctx);

  private:
    /// @brief Patch empty line blocks with explicit branch to exit.
    /// @param ctx Context whose scheduled line blocks are inspected.
    void patchEmptyLineBlocks(LoweringContext &ctx);

    /// @brief Emit cleanup code in the procedure's exit block.
    /// @param ctx Context providing the function, parameters, and return callback.
    /// @pre @p ctx has a scheduled function and valid exit-block index.
    void emitProcedureCleanup(LoweringContext &ctx);

    /// Borrowed parent that owns the procedure pipeline's shared state.
    Lowerer &lowerer;
};

/// @brief Emits control flow for sequential statement lowering within a procedure.
struct StatementLowering {
    /// @brief Construct a sequence-lowering facade over @p lowerer.
    /// @param lowerer Parent lowering coordinator borrowed by this facade.
    /// @pre @p lowerer outlives the constructed facade.
    explicit StatementLowering(Lowerer &lowerer);

    /// @brief Lower a sequence of statements, branching between numbered blocks.
    /// @details Registers GOSUB continuations, enters the first scheduled line
    ///          block, lowers statements in order, and emits normal fallthrough
    ///          or error-handler termination as required.
    /// @param stmts Non-null statement pointers in execution order.
    /// @param stopOnTerminated Stop after the first statement that terminates its block.
    /// @param beforeBranch Optional callback invoked before normal fallthrough edges.
    /// @pre An active function exists and every statement line has a scheduled block.
    void lowerSequence(const std::vector<const Stmt *> &stmts,
                       bool stopOnTerminated,
                       const std::function<void(const Stmt &)> &beforeBranch = {});

  private:
    /// Borrowed parent providing statement dispatch and current procedure state.
    Lowerer &lowerer;
};

} // namespace il::frontends::basic
