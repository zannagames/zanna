//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/Lowerer_Program.cpp
// Purpose: Hosts the program-level helpers that bridge the BASIC AST to the IL
//          builder. The lowering pipeline coordinates scanning, declaration
//          emission, runtime discovery, and final IL generation. Concentrating
//          the orchestration logic in this file keeps the main `Lowerer`
//          interface focused while documenting the lifecycle of program
//          compilation.
// Key invariants: Shared lowering state is reset before each run and builders
//                 are released once emission finishes to avoid dangling
//                 pointers.
// Ownership/Lifetime: ProgramLowering borrows the Lowerer; the caller owns the
//                     destination module and the run owns its temporary builder.
// Links: docs/internals/codemap.md, docs/internals/architecture.md#cpp-overview
//
//===----------------------------------------------------------------------===//

/// @file Lowerer_Program.cpp
/// @brief Implements program-level helpers for the BASIC-to-IL lowering pipeline.
/// @details These utilities reset shared lowering state, construct IR builders,
///          and drive the staged emission sequence used by the BASIC front end.

#include "frontends/basic/ILTypeUtils.hpp"
#include "frontends/basic/Lowerer.hpp"
#include "frontends/basic/LoweringPipeline.hpp"

#include "zanna/il/IRBuilder.hpp"

namespace il::frontends::basic {

namespace pipeline_detail {
/// @brief Translate a BASIC AST type enumeration into an IL core type handle.
///
/// @details Lowering frequently needs to turn semantic types expressed by the
///          BASIC AST (`Type`) into the concrete IL type descriptor understood by
///          the builder.  The mapping is intentionally narrow: each BASIC type
///          collapses to a single IL `Type::Kind`.  Should the language evolve
///          new cases can be added here without touching call sites.
///
///          This function now forwards to the consolidated implementation in
///          type_conv::astToIlType() to maintain the single source of truth
///          while preserving the existing API for backward compatibility.
///
/// @param ty BASIC type enumeration value.
/// @return Concrete IL type used during lowering. Defaults to `I64` for
///         robustness when the caller passes an unrecognised type.
il::core::Type coreTypeForAstType(::il::frontends::basic::Type ty) {
    return type_conv::astToIlType(ty);
}
} // namespace pipeline_detail

/// @brief Create a program-lowering helper bound to a borrowed @c Lowerer.
///
/// @details The helper is a thin façade around the shared `Lowerer` instance.
///          Storing a reference keeps the orchestration code decoupled from the
///          parser and IR builder while still having access to shared caches,
///          manglers, and runtime trackers the front end maintains.
///
/// @param lowerer Borrowed lowering pipeline that manages shared state.
ProgramLowering::ProgramLowering(Lowerer &lowerer) : lowerer(lowerer) {}

/// @brief Lower a parsed BASIC program into IL.
///
/// @details The orchestration proceeds as follows:
///          1. Bind the destination module and initialise a fresh IR builder.
///          2. Reset the procedure context, symbol/string/signature stores,
///             mangler, runtime tracker, manual requirements, and module type caches.
///          3. Run scanning passes that gather type and runtime requirements
///             prior to emission.
///          4. Emit OOP bodies, procedures, and synthetic main with one builder.
///          5. Declare the runtime externs requested by scanning and emission.
///          6. Release borrowed references to ensure no dangling pointers remain
///             once the module is fully populated.
///
/// @param prog AST representing the BASIC program.
/// @param module IL module receiving the lowered output.
void ProgramLowering::run(const Program &prog, il::core::Module &module) {
    lowerer.mod = &module;
    build::IRBuilder builder(module);
    lowerer.builder = &builder;

    /// @brief Clears run-scoped builder/module bindings on every scope exit.
    struct LowererBindingGuard {
        /// Borrowed lowerer whose transient bindings are guarded.
        Lowerer &lowerer;

        /// @brief Clear borrowed module and builder pointers when lowering exits.
        ~LowererBindingGuard() {
            lowerer.builder = nullptr;
            lowerer.mod = nullptr;
        }
    } bindingGuard{lowerer};

    lowerer.mangler = NameMangler();
    auto &ctx = lowerer.context();
    ctx.reset();
    lowerer.symbolTable_.clear();
    lowerer.stringTable_.clear();
    lowerer.procSignatures.clear();

    lowerer.runtimeTracker.reset();
    lowerer.resetManualHelpers();

    // Clear any cached module-level array typing from previous runs (BUG-097)
    lowerer.clearModuleObjectArrayCache();

    lowerer.scanOOP(prog);     // Must scan OOP first to populate classLayouts_
    lowerer.scanProgram(prog); // Then scan program (needs classLayouts_ for field assignments)

    // BUG-097 fix: Cache module-level object arrays BEFORE emitting OOP bodies.
    // Class methods may reference global arrays (e.g., g_widgets(i).Update()),
    // and resolveObjectClass needs the element class info during method lowering.
    // Previously this was only called in emitProgram, which runs AFTER emitOopDeclsAndBodies.
    lowerer.cacheModuleObjectArraysFromAST(prog.main);

    // Ensure procedure signature/alias table is populated before emitting OOP bodies
    // so method calls to module-level procedures resolve correctly.
    lowerer.collectProcedureSignatures(prog);
    lowerer.emitOopDeclsAndBodies(prog);
    // Emit program bodies (may toggle manual helpers during lowering).
    lowerer.emitProgram(prog);
    // Declare all helpers requested via scan and lowering in a single pass to avoid duplicates.
    lowerer.declareRequiredRuntime(builder);
}

} // namespace il::frontends::basic
