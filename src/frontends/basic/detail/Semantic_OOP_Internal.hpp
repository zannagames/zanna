//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE in the project root for license information.
//
// File: src/frontends/basic/detail/Semantic_OOP_Internal.hpp
// Purpose: Declares internal OOP semantic-analysis helpers and the phased
//          builder that populates OopIndex from a parsed BASIC program.
// Key invariants:
//   - OopIndexBuilder phases execute in their documented order.
//   - Declaration and lookup keys use canonical qualified names.
//   - A null DiagnosticEmitter suppresses diagnostics without changing index
//     construction.
// Ownership/Lifetime:
//   - OopIndexBuilder borrows its index and optional diagnostic emitter.
//   - AST declarations and statement bodies are borrowed during analysis.
// Links: src/frontends/basic/Semantic_OOP_Builder.cpp,
//        src/frontends/basic/Semantic_OOP_Helpers.cpp,
//        src/frontends/basic/OopIndex.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "frontends/basic/AST.hpp"
#include "frontends/basic/DiagnosticEmitter.hpp"
#include "frontends/basic/OopIndex.hpp"

#include "support/diagnostics.hpp"

#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

/// @file
/// @brief Declares implementation-only OOP semantic-analysis facilities.
/// @details This header is not part of the BASIC frontend's public API. It is
///          shared by the Semantic_OOP translation units so their validation,
///          name-resolution, and index-building phases use one contract.

namespace il::frontends::basic::detail {

//===----------------------------------------------------------------------===//
// Helper Validation Functions
//===----------------------------------------------------------------------===//

/// @brief Check for local variables that shadow class fields.
/// @param body Statement body to scan for shadowing locals.
/// @param klass The class declaration providing field context.
/// @param fieldNames Set of field names to check against.
/// @param emitter Diagnostics interface for emitting warnings.
void checkMemberShadowing(const std::vector<StmtPtr> &body,
                          const ClassDecl &klass,
                          const std::unordered_set<std::string> &fieldNames,
                          DiagnosticEmitter *emitter);

/// @brief Check for use of 'ME' in a static context.
/// @param body Statement body to scan for ME references.
/// @param emitter Diagnostics interface for emitting errors.
/// @param errorCode Error code to emit.
/// @param message Error message to emit.
void checkMeInStaticContext(const std::vector<StmtPtr> &body,
                            DiagnosticEmitter *emitter,
                            const char *errorCode,
                            const char *message);

/// @brief Check if a method body must return a value.
/// @param stmts Statement list forming the method body.
/// @return True if the body definitively returns a value.
[[nodiscard]] bool methodBodyMustReturn(const std::vector<StmtPtr> &stmts);

/// @brief Detect VB-style implicit returns via assignment to the function name.
/// @param method Method declaration to analyze.
/// @return True if the method has an implicit return.
[[nodiscard]] bool methodHasImplicitReturn(const MethodDecl &method);

/// @brief Emit a diagnostic for missing return statement in a function.
/// @param klass Class containing the method.
/// @param method Method declaration to check.
/// @param emitter Diagnostics interface for emitting errors.
void emitMissingReturn(const ClassDecl &klass,
                       const MethodDecl &method,
                       DiagnosticEmitter *emitter);

/// @brief Join qualified name segments with dots.
/// @param segs Vector of name segments.
/// @return Dot-separated qualified name.
[[nodiscard]] std::string joinQualified(const std::vector<std::string> &segs);

//===----------------------------------------------------------------------===//
// OOP Index Builder
//===----------------------------------------------------------------------===//

/// @brief Context for building the OOP index, holding shared state across phases.
/// @details This class encapsulates all the logic for scanning the AST,
///          resolving base classes, building vtables, and checking conformance.
class OopIndexBuilder {
  public:
    /// @brief Creates a phased builder for a caller-owned OOP index.
    /// @param index Index populated by @ref build.
    /// @param emitter Optional diagnostic sink; may be nullptr.
    /// @pre @p index remains alive for the lifetime of this builder.
    OopIndexBuilder(OopIndex &index, DiagnosticEmitter *emitter)
        : index_(index), emitter_(emitter) {}

    /// @brief Build the complete OOP index from a parsed program.
    /// @param program The parsed BASIC program.
    /// @post @ref index_ contains registered declarations, resolved
    ///       inheritance, vtables, and interface-conformance information for
    ///       the declarations reachable from @p program.
    void build(const Program &program);

  private:
    OopIndex &index_;            ///< Index being populated (borrowed).
    DiagnosticEmitter *emitter_; ///< Diagnostics sink (may be null).

    /// Namespace path of the declaration currently being scanned.
    std::vector<std::string> nsStack_;

    /// Unresolved raw base-class names per class, with the site for diagnostics.
    std::unordered_map<std::string, std::pair<std::string, il::support::SourceLoc>> rawBases_;

    /// @brief Active USING imports/aliases used to resolve unqualified names.
    struct UsingContext {
        std::unordered_set<std::string> imports;              ///< Imported namespace prefixes.
        std::unordered_map<std::string, std::string> aliases; ///< alias -> qualified target.
    } usingCtx_;

    // Helper methods

    /// @brief Join @ref nsStack_ into a dot-qualified namespace prefix.
    /// @return Current namespace path, or an empty string at file scope.
    [[nodiscard]] std::string joinNamespace() const;

    // Phase methods (run in this declared order by build()).

    /// @brief Phase 1: register every class/interface/enum declaration.
    void scanDeclarations(const std::vector<StmtPtr> &stmts);
    /// @brief Phase 2: collect USING directives into @ref usingCtx_.
    void collectUsingDirectives(const std::vector<StmtPtr> &stmts);
    /// @brief Phase 3: resolve raw base/implements names to qualified types.
    void resolveBasesAndImplements();
    /// @brief Phase 4: report classes that inherit in a cycle.
    void detectInheritanceCycles();
    /// @brief Phase 5: lay out vtables once inheritance is resolved.
    void buildVtables();
    /// @brief Phase 6: verify each class implements its interfaces' members.
    void checkInterfaceConformance();

    // Declaration processing (invoked from scanDeclarations).

    /// @brief Register a class, its fields, and members into the index.
    void processClassDecl(const ClassDecl &classDecl);
    /// @brief Register an interface and its abstract member set.
    void processInterfaceDecl(const InterfaceDecl &interfaceDecl);
    /// @brief Register an enum and its constant variants.
    void processEnumDecl(const EnumDecl &enumDecl);
    /// @brief Add a property's accessor methods to @p info.
    void processPropertyDecl(const PropertyDecl &prop, ClassInfo &info);
    /// @brief Register a constructor, checking field-shadowing locals.
    void processConstructorDecl(const ConstructorDecl &ctor,
                                ClassInfo &info,
                                const ClassDecl &classDecl,
                                const std::unordered_set<std::string> &fieldNames);
    /// @brief Register a method, checking returns and field shadowing.
    void processMethodDecl(const MethodDecl &method,
                           ClassInfo &info,
                           const ClassDecl &classDecl,
                           const std::unordered_set<std::string> &fieldNames);
    /// @brief Diagnose names used as both a field and a method on a class.
    void checkFieldMethodCollisions(ClassInfo &info,
                                    const ClassDecl &classDecl,
                                    const std::unordered_set<std::string> &fieldNames);

    // Resolution helpers

    /// @brief Resolve a raw base name relative to @p classQ's scope/USINGs.
    /// @param classQ Canonical qualified name of the derived class.
    /// @param raw Source spelling of the requested base type.
    /// @return Canonical qualified base name, or an empty string when no
    ///         declaration can be resolved.
    [[nodiscard]] std::string resolveBase(const std::string &classQ, const std::string &raw) const;
    /// @brief Expand a leading alias segment of @p q to its qualified target.
    /// @param q Candidate qualified name.
    /// @return Expanded qualified name, or @p q unchanged when its first
    ///         segment is not an active alias.
    [[nodiscard]] std::string expandAlias(const std::string &q) const;
    /// @brief Resolve a raw interface name relative to @p classQ's scope.
    /// @param classQ Canonical qualified name of the implementing class.
    /// @param raw Source spelling of the requested interface.
    /// @return Canonical qualified interface name, or an empty string when no
    ///         declaration can be resolved.
    [[nodiscard]] std::string resolveInterface(const std::string &classQ,
                                               const std::string &raw) const;
};

} // namespace il::frontends::basic::detail
