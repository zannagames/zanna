//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/Lowerer_Procedure_Context.cpp
// Purpose: LoweringContext construction and symbol table management helpers.
//
// Phase: Context Setup (runs before metadata collection)
//
// Key Invariants:
// - Symbol table entries are lazily created on first reference
// - Each procedure starts with a fresh symbol state (resetSymbolState)
// - Field scopes are managed via push/pop for class method lowering
//
// Ownership/Lifetime: Operates on borrowed Lowerer instance.
//
//===----------------------------------------------------------------------===//

/**
 * @file Lowerer_Procedure_Context.cpp
 * @brief Implements procedure context construction and symbol-scope adapters.
 *
 * The functions here mutate the Lowerer's shared SymbolTable and field-scope
 * stack. LoweringContext owns only its procedure name; all pipeline services,
 * parameter/body vectors, and configuration callbacks are borrowed.
 */

#include "frontends/basic/Lowerer.hpp"
#include "frontends/basic/LoweringPipeline.hpp"
#include "frontends/basic/lower/Emitter.hpp"

#include "zanna/il/IRBuilder.hpp"

#include <utility>

using namespace il::core;

namespace il::frontends::basic {

// =============================================================================
// LoweringContext Construction
// =============================================================================

/// @brief Aggregate lowering state for a single procedure invocation.
/// @details The context bundles references to the shared @ref Lowerer state,
///          symbol table, and emitter while capturing procedure-specific
///          parameters such as the body statements, configuration hooks, and the
///          IR builder used to materialise code. The name is copied, while the
///          parameters, body, configuration, and service objects are borrowed
///          and must remain alive through all procedure phases.
/// @param lowerer Parent lowering coordinator.
/// @param symbols Mutable symbol-map alias owned by @p lowerer.
/// @param builder Active IR builder.
/// @param emitter Shared low-level emitter.
/// @param name Procedure emission name, moved into the context.
/// @param params Borrowed formal-parameter vector.
/// @param body Borrowed procedure-body vector.
/// @param config Borrowed return and callback configuration.
ProcedureLowering::LoweringContext::LoweringContext(Lowerer &lowerer,
                                                    SymbolTable::SymbolMap &symbols,
                                                    il::build::IRBuilder &builder,
                                                    lower::Emitter &emitter,
                                                    std::string name,
                                                    const std::vector<Param> &params,
                                                    const std::vector<StmtPtr> &body,
                                                    const Lowerer::ProcedureConfig &config)
    : lowerer(lowerer), symbols(symbols), builder(builder), emitter(emitter), name(std::move(name)),
      params(params), body(body), config(config) {}

// =============================================================================
// Symbol Table Accessors
// =============================================================================

/// @brief Ensure a symbol table entry exists for the given name.
/// @details Inserts a default @ref SymbolInfo when the symbol is first observed
///          so subsequent metadata updates can assume the entry exists.  Newly
///          created records start with inferred integer type information and no
///          recorded usage, mirroring BASIC's default variable semantics.
/// @param name BASIC identifier whose symbol information is requested.
/// @return Reference to the mutable symbol record.
Lowerer::SymbolInfo &Lowerer::ensureSymbol(std::string_view name) {
    return symbolTable_.define(name);
}

/// @brief Look up a symbol record, creating no new entries.
/// @details Returns @c nullptr when the identifier has not been encountered,
///          allowing callers to treat unknown symbols as implicitly typed.
/// @param name Identifier to probe.
/// @return Mutable symbol metadata or @c nullptr when absent.
Lowerer::SymbolInfo *Lowerer::findSymbol(std::string_view name) {
    return symbolTable_.lookup(name);
}

/// @brief Const-qualified symbol lookup helper.
/// @details Mirrors @ref findSymbol(std::string_view) while preserving const
///          correctness for call sites that only need to inspect metadata.
/// @param name Identifier to probe.
/// @return Const symbol metadata or @c nullptr when absent.
const Lowerer::SymbolInfo *Lowerer::findSymbol(std::string_view name) const {
    return symbolTable_.lookup(name);
}

/// @brief Record the declared primitive type for a symbol.
/// @details Updates the symbol information with the explicit AST type and, when
///          applicable, notes that the identifier represents a boolean scalar.
///          Symbols that are later used as arrays have their boolean flag
///          cleared when @ref markArray executes. Object flags and class text
///          are cleared, but the referenced flag is left unchanged.
/// @param name Identifier whose type is being fixed.
/// @param type AST-declared type for the symbol.
void Lowerer::setSymbolType(std::string_view name, AstType type) {
    auto &info = ensureSymbol(name);
    info.type = type;
    info.hasType = true;
    info.isBoolean = !info.isArray && type == AstType::Bool;
    // BUG-BAS-002 fix: Clear object flags when setting primitive type
    info.isObject = false;
    info.objectClass.clear();
}

/// @brief Record that a symbol denotes an object reference of a specific class.
/// @details Marks the symbol as typed, toggles the object flag so later slot
///          allocation emits pointer storage, and captures the class name for
///          runtime dispatch.
/// @param name Identifier being classified as an object.
/// @param className BASIC class identifier stored without additional qualification.
void Lowerer::setSymbolObjectType(std::string_view name, std::string className) {
    if (name.empty())
        return;
    auto &info = ensureSymbol(name);
    info.isObject = true;
    info.objectClass = std::move(className);
    info.hasType = true;
}

/// @brief Mark that a symbol has been referenced somewhere in the procedure.
/// @details Lazily infers the type from semantic analysis or name suffix when
///          absent, ensuring later slot allocation chooses the appropriate
///          storage width.  Empty names are ignored because they arise from
///          parse errors handled elsewhere.
/// @param name Identifier encountered during AST traversal.
void Lowerer::markSymbolReferenced(std::string_view name) {
    if (name.empty())
        return;
    auto &info = ensureSymbol(name);
    if (!info.hasType) {
        info.type = inferVariableTypeForLowering(*this, name);
        info.hasType = true;
        info.isBoolean = !info.isArray && info.type == AstType::Bool;
    }
    info.referenced = true;
}

/// @brief Flag that a symbol is used with array semantics.
/// @details Records the array bit on the symbol metadata and clears the boolean
///          flag because arrays are always pointer typed regardless of element
///          suffixes.
/// @param name Identifier representing an array value.
void Lowerer::markArray(std::string_view name) {
    if (name.empty())
        return;
    auto &info = ensureSymbol(name);
    info.isArray = true;
    if (info.isBoolean)
        info.isBoolean = false;
}

/// @brief Flag that a symbol has STATIC storage duration.
/// @details STATIC variables persist across procedure calls using module-level
///          runtime storage with procedure-qualified names.
/// @param name Identifier representing a static variable.
void Lowerer::markStatic(std::string_view name) {
    if (name.empty())
        return;
    auto &info = ensureSymbol(name);
    info.isStatic = true;
}

// =============================================================================
// Field Scope Management
// =============================================================================

/// @brief Push a field scope for class method lowering.
/// @details Enables implicit field access within class methods by establishing
///          the class layout context.
/// @param className Fully qualified class name whose fields become accessible.
void Lowerer::pushFieldScope(const std::string &className) {
    const ClassLayout *layout = nullptr;
    if (auto it = classLayouts_.find(className); it != classLayouts_.end())
        layout = &it->second;
    symbolTable_.pushFieldScope(layout);
}

/// @brief Pop the current field scope after class method lowering completes.
void Lowerer::popFieldScope() {
    symbolTable_.popFieldScope();
}

/// @brief Query the active field scope for implicit field resolution.
/// @return Pointer to the active field scope or nullptr if none.
const Lowerer::FieldScope *Lowerer::activeFieldScope() const {
    return symbolTable_.activeFieldScope();
}

/// @brief Check whether a name refers to a field in the current scope.
/// @param name Identifier to check.
/// @return True if the name matches a field in the active class layout.
bool Lowerer::isFieldInScope(std::string_view name) const {
    return symbolTable_.isFieldInScope(name);
}

/// @brief Reset symbol metadata between procedure lowering runs.
/// @details Clears transient fields (slot identifiers, reference flags, type
///          overrides) for persistent string literals and removes all other
///          symbols entirely.  This prevents leakage of declaration information
///          from one procedure into the next without discarding the shared pool
///          of literal strings.
void Lowerer::resetSymbolState() {
    symbolTable_.resetForNewProcedure();
}

} // namespace il::frontends::basic
