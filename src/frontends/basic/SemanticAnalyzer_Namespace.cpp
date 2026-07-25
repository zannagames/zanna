//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/SemanticAnalyzer_Namespace.cpp
// Purpose: Implements namespace semantic checking including USING placement
//          and reserved-root validation.
// Key invariants:
//   - USING applies at file scope or inside NAMESPACE blocks; not inside procedures.
//   - "Zanna" root namespace is reserved.
//   - Alias names cannot duplicate existing aliases or namespace names.
// Ownership/Lifetime: Borrows DiagnosticEmitter; no AST ownership.
// Links: docs/internals/codemap.md
//
//===----------------------------------------------------------------------===//

/// @file SemanticAnalyzer_Namespace.cpp
/// @brief Implements BASIC namespace, USING, and declaration-scope analysis.
/// @details Maintains namespace/import stacks, enforces reserved-root and USING
///          placement rules, resolves class base/interface names, analyzes
///          class member bodies in rollback procedure scopes, and adapts
///          TypeResolver failures into BASIC namespace diagnostics.

#include "frontends/basic/ASTUtils.hpp"
#include "frontends/basic/IdentifierUtil.hpp"
#include "frontends/basic/Options.hpp"
#include "frontends/basic/SemanticAnalyzer_Internal.hpp"
#include "frontends/basic/StringUtils.hpp"
#include <algorithm>
#include <cctype>

namespace il::frontends::basic {

/// @brief Compares two byte strings case-insensitively.
/// @details Requires equal length and lowercases each byte through
///          @c std::tolower after unsigned-char conversion.
/// @param a First string.
/// @param b Second string.
/// @return @c true when every corresponding byte compares equal.
static bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size())
        return false;
    /// Compares one byte pair after safe ctype conversion.
    return std::equal(a.begin(), a.end(), b.begin(), [](char ca, char cb) {
        return std::tolower(static_cast<unsigned char>(ca)) ==
               std::tolower(static_cast<unsigned char>(cb));
    });
}

/// @brief Analyzes one namespace declaration and its nested statements.
/// @details Rejects any declaration rooted case-insensitively at reserved
///          `Zanna` before mutating stacks. Otherwise marks a declaration seen,
///          pushes all path segments, creates a child USING scope that inherits
///          imports but not aliases, applies local USING declarations in a
///          first pass, and visits every other statement in a second pass.
///          Namespace and USING stacks are restored after traversal.
/// @param decl Mutable declaration whose nested statements may be annotated.
void SemanticAnalyzer::analyzeNamespaceDecl(NamespaceDecl &decl) {
    // Check for reserved "Zanna" root namespace: declarations are never allowed
    // under the reserved root. Emit a dedicated diagnostic.
    if (!decl.path.empty() && iequals(decl.path[0], "Zanna")) {
        // Use the established E_NS_009 diagnostic for reserved root violations.
        de.emit(diag::BasicDiag::NsReservedZanna, decl.loc, 1, {});
        return;
    }

    sawDecl_ = true;

    // Push namespace segments onto stack.
    for (const auto &seg : decl.path)
        nsStack_.push_back(seg);

    // Inherit USING context from parent scope and then apply local USINGs.
    UsingScope child;
    if (!usingStack_.empty()) {
        // Inherit imports; aliases can be shadowed so keep local alias map empty.
        child.imports = usingStack_.back().imports;
    }
    usingStack_.push_back(std::move(child));

    // First pass: apply USING directives in this namespace to the current using scope.
    for (const auto &stmt : decl.body) {
        if (!stmt)
            continue;
        if (stmt->stmtKind() == Stmt::Kind::UsingDecl) {
            analyzeUsingDecl(static_cast<UsingDecl &>(*stmt));
        }
    }

    // Second pass: analyze remaining statements (children see combined parent + local using).
    for (const auto &stmt : decl.body) {
        if (!stmt)
            continue;
        if (stmt->stmtKind() != Stmt::Kind::UsingDecl)
            visitStmt(*stmt);
    }

    // Pop segments.
    size_t popCount = decl.path.size();
    if (nsStack_.size() >= popCount)
        nsStack_.resize(nsStack_.size() - popCount);

    // Pop USING scope.
    if (!usingStack_.empty())
        usingStack_.pop_back();
}

/// @brief Resolves and semantically analyzes a class declaration.
/// @details Determines the declared qualified class spelling from the OOP
///          index, resolves/mutates base and implemented-interface names, then
///          analyzes constructor, destructor, and method bodies in isolated
///          procedure scopes. Static members do not expose `ME`; method return
///          presence selects FUNCTION versus SUB EXIT context.
/// @param decl Mutable class declaration receiving resolved type references.
void SemanticAnalyzer::analyzeClassDecl(ClassDecl &decl) {
    sawDecl_ = true;

    std::string classQName = decl.qualifiedName;
    if (classQName.empty()) {
        std::vector<std::string> parts = nsStack_;
        parts.push_back(decl.name);
        classQName = JoinDots(parts);
    }
    if (const ClassInfo *info = oopIndex_.findClass(classQName))
        classQName = info->qualifiedName;

    /// Analyzes one class-member body with procedure-local rollback.
    /// The helper saves/restores the active class/receiver state, registers
    /// parameters and body line labels, visits statements, and brackets an
    /// optional SUB/FUNCTION EXIT context.
    auto analyzeMemberBody = [&](const std::vector<Param> &params,
                                 const std::vector<StmtPtr> &body,
                                 bool hasMe,
                                 std::optional<LoopKind> loopKind) {
        ProcedureScope procScope(*this);

        const std::string previousClass = activeClassQName_;
        const bool previousHasMe = activeMemberHasMe_;
        activeClassQName_ = classQName;
        activeMemberHasMe_ = hasMe;

        if (loopKind)
            loopStack_.push_back(*loopKind);

        for (const auto &p : params)
            registerProcedureParam(p);

        for (const auto &st : body) {
            if (!st)
                continue;
            auto insertResult = labels_.insert(st->line);
            if (insertResult.second && activeProcScope_)
                activeProcScope_->noteLabelInserted(st->line);
        }

        for (const auto &st : body)
            if (st)
                visitStmt(*st);

        if (loopKind)
            loopStack_.pop_back();

        activeClassQName_ = previousClass;
        activeMemberHasMe_ = previousHasMe;
    };

    // Resolve base class if present.
    if (decl.baseName) {
        std::string resolvedBase = resolveTypeRef(
            *decl.baseName, nsStack_, decl.loc, static_cast<uint32_t>(decl.baseName->size()));
        if (!resolvedBase.empty())
            decl.baseName = std::move(resolvedBase);
    }

    // Resolve implemented interfaces.
    for (auto &ifaceQN : decl.implementsQualifiedNames) {
        // Build dotted name from qualified segments.
        std::string ifaceName;
        for (size_t i = 0; i < ifaceQN.size(); ++i) {
            if (i > 0)
                ifaceName.push_back('.');
            ifaceName += ifaceQN[i];
        }

        if (!ifaceName.empty()) {
            std::string resolvedIface = resolveTypeRef(
                ifaceName, nsStack_, decl.loc, static_cast<uint32_t>(ifaceName.size()));
            if (!resolvedIface.empty())
                ifaceQN = SplitDots(resolvedIface);
        }
    }

    for (const auto &member : decl.members) {
        if (!member)
            continue;
        switch (member->stmtKind()) {
            case Stmt::Kind::ConstructorDecl: {
                auto &ctor = static_cast<ConstructorDecl &>(*member);
                analyzeMemberBody(ctor.params, ctor.body, !ctor.isStatic, LoopKind::Sub);
                break;
            }
            case Stmt::Kind::DestructorDecl: {
                static const std::vector<Param> kNoParams;
                auto &dtor = static_cast<DestructorDecl &>(*member);
                analyzeMemberBody(kNoParams, dtor.body, true, LoopKind::Sub);
                break;
            }
            case Stmt::Kind::MethodDecl: {
                auto &method = static_cast<MethodDecl &>(*member);
                analyzeMemberBody(method.params,
                                  method.body,
                                  !method.isStatic,
                                  method.ret ? LoopKind::Function : LoopKind::Sub);
                break;
            }
            default:
                break;
        }
    }
}

/// @brief Marks an interface declaration for subsequent USING placement rules.
/// @param decl Declaration already indexed by the OOP/declaration pass; its
///             contents are not inspected here.
void SemanticAnalyzer::analyzeInterfaceDecl(InterfaceDecl &decl) {
    sawDecl_ = true;
}

/// @brief Validates and records one namespace USING directive.
/// @details Procedure-local USING is always rejected. Namespace-local USING is
///          accepted only when runtime namespaces are enabled; file-level USING
///          must precede declarations. Paths are canonicalized and must exist.
///          Bare/reserved `Zanna` imports are rejected unless runtime namespaces
///          are enabled and at least one child segment is present. Aliases must
///          be single identifiers, cannot conflict with another target in the
///          same scope, and cannot exactly shadow a namespace. Repeating the
///          same alias-to-target mapping is an accepted no-op.
/// @param decl Mutable declaration supplying path, optional alias, and location.
/// @post On success, the active USING scope gains either an alias/location
///       mapping or a canonical import. Validation failures leave it unchanged.
void SemanticAnalyzer::analyzeUsingDecl(UsingDecl &decl) {
    // Placement rules (per docs/tutorials/basic-tutorial.md):
    // - USING must appear at file scope (not inside namespace blocks)
    // - USING must appear before any namespace/class/interface declarations
    // - USING cannot appear inside procedures

    // Disallow inside procedures (parser should already prevent this, but keep as guard).
    if (activeProcScope_ != nullptr) {
        de.emit(diag::BasicDiag::NsUsingNotFileScope, decl.loc, 1, {});
        return;
    }

    // Reject USING inside namespace blocks (E_NS_008) unless runtime namespaces
    // are enabled (Phase 2 semantics permit scoped USING).
    if (!nsStack_.empty()) {
        if (!FrontendOptions::enableRuntimeNamespaces()) {
            de.emit(diag::BasicDiag::NsUsingNotFileScope, decl.loc, 1, {});
            return;
        }
        // Otherwise, allow scoped USING and proceed.
    }

    // Enforce USING-before-decls at file scope only: E_NS_005.
    // Inside a namespace block, analyzeNamespaceDecl processes USING directives
    // in a first pass before other declarations, so placement within the block
    // is handled there.
    if (sawDecl_ && nsStack_.empty()) {
        de.emit(diag::BasicDiag::NsUsingAfterDecl, decl.loc, 1, {});
        return;
    }

    // Build canonical lowercase namespace path from segments.
    std::string nsPath;
    if (!decl.namespacePath.empty())
        nsPath = CanonJoin(decl.namespacePath);

    if (nsPath.empty())
        return;

    // Reserved root "Zanna": by default, reject imports under the reserved root.
    // When runtime namespaces are enabled, permit USING for any Zanna.* subtree
    // (Console, Strings, Convert, Parse, Diagnostics, Math, IO, ...). Bare
    // "USING Zanna" (root only) remains rejected to avoid ambiguous imports.
    if (!decl.namespacePath.empty() && iequals(decl.namespacePath[0], "Zanna")) {
        const bool allow = FrontendOptions::enableRuntimeNamespaces() &&
                           decl.namespacePath.size() >= 2; // allow any Zanna.<child>
        if (!allow) {
            de.emit(diag::BasicDiag::NsReservedZanna, decl.loc, 1, {});
            return;
        }
        // Accepted: USING Zanna.<any> (and sub-names). Proceed with normal checks.
    }

    // E_NS_001: Namespace must exist in registry (error severity).
    if (!nsPath.empty() && !ns_.namespaceExists(nsPath)) {
        // Print identifier in canonical BASIC uppercase form in diagnostics.
        std::string nsUpper = string_utils::to_upper(nsPath);
        de.emit(
            diag::BasicDiag::NsUnknownNamespace, decl.loc, 1, {{diag::Replacement{"ns", nsUpper}}});
        // Continue after emitting the error to allow follow-on diagnostics but do not
        // record the USING in the scoped context.
        return;
    }

    // Validate alias if present.
    if (!usingStack_.empty()) {
        UsingScope &cur = usingStack_.back();

        if (!decl.alias.empty()) {
            // Canonicalize alias to lowercase.
            std::string aliasLower = Canon(decl.alias);
            if (aliasLower.find('.') != std::string::npos) {
                // Should not happen (parser enforces single identifier).
                de.emit(diag::BasicDiag::NsDuplicateAlias,
                        decl.loc,
                        1,
                        {{diag::Replacement{"alias", decl.alias}}});
                return;
            }

            // E_NS_004: Duplicate alias in the same scope is not allowed unless
            // it refers to the same target namespace (idempotent seeding).
            if (auto itDup = cur.aliases.find(aliasLower); itDup != cur.aliases.end()) {
                if (itDup->second == nsPath) {
                    // Ignore duplicate that maps to the same target.
                    return;
                }
                de.emit(diag::BasicDiag::NsDuplicateAlias,
                        decl.loc,
                        1,
                        {{diag::Replacement{"alias", decl.alias}}});
                return;
            }

            // E_NS_007: Alias must not shadow a namespace name (only check exact alias).
            if (ns_.namespaceExists(aliasLower)) {
                de.emit(diag::BasicDiag::NsAliasShadowsNs,
                        decl.loc,
                        1,
                        {{diag::Replacement{"alias", decl.alias}}});
                return;
            }

            cur.aliases.emplace(aliasLower, nsPath);
            cur.aliasLoc.emplace(aliasLower, decl.loc);
        } else {
            if (!nsPath.empty())
                cur.imports.insert(nsPath);
        }
    }
}

/// @brief Resolves a type reference and translates failures to diagnostics.
/// @details Returns silently when the resolver has not yet been initialized.
///          A successful resolver result returns its qualified name. Contenders
///          emit the ambiguous-type diagnostic in resolver-provided order.
///          For a qualified miss, an existing namespace produces the
///          type-not-in-namespace diagnostic; every remaining miss produces
///          type-not-found.
/// @param typeName Simple or dotted type spelling.
/// @param currentNsChain Current namespace path supplied to @ref TypeResolver.
/// @param loc Diagnostic source location.
/// @param length Diagnostic highlight length.
/// @return Resolved qualified name, or an empty string after no resolver or any
///         failed resolution.
std::string SemanticAnalyzer::resolveTypeRef(const std::string &typeName,
                                             const std::vector<std::string> &currentNsChain,
                                             il::support::SourceLoc loc,
                                             uint32_t length) {
    if (!resolver_)
        return ""; // TypeResolver not initialized yet

    // Use TypeResolver to resolve the type.
    auto result = resolver_->resolve(typeName, currentNsChain);

    if (result.found) {
        // Successfully resolved.
        return result.qname;
    }

    // Not found - check if it's a qualified name with existing namespace.
    bool isQualified = typeName.find('.') != std::string::npos;

    if (isQualified && !result.contenders.empty()) {
        // E_NS_003: Ambiguous type with multiple contenders.
        std::string candidates;
        for (size_t i = 0; i < result.contenders.size(); ++i) {
            if (i > 0)
                candidates += ", ";
            candidates += result.contenders[i];
        }

        de.emit(
            diag::BasicDiag::NsAmbiguousType,
            loc,
            length,
            {{diag::Replacement{"type", typeName}}, {diag::Replacement{"candidates", candidates}}});
        return "";
    }

    if (isQualified) {
        // Check if namespace exists but type is missing (E_NS_002).
        // Split the qualified name to get namespace and type parts.
        size_t lastDot = typeName.rfind('.');
        if (lastDot != std::string::npos) {
            std::string nsPath = typeName.substr(0, lastDot);
            std::string typeOnly = typeName.substr(lastDot + 1);

            if (ns_.namespaceExists(nsPath)) {
                // E_NS_002: Namespace exists but doesn't contain the type.
                de.emit(diag::BasicDiag::NsTypeNotInNs,
                        loc,
                        length,
                        {{diag::Replacement{"ns", nsPath}}, {diag::Replacement{"type", typeOnly}}});
                return "";
            }
        }
    }

    // Check for ambiguity even with simple names.
    if (!result.contenders.empty()) {
        // E_NS_003: Ambiguous type with multiple contenders.
        std::string candidates;
        for (size_t i = 0; i < result.contenders.size(); ++i) {
            if (i > 0)
                candidates += ", ";
            candidates += result.contenders[i];
        }

        de.emit(
            diag::BasicDiag::NsAmbiguousType,
            loc,
            length,
            {{diag::Replacement{"type", typeName}}, {diag::Replacement{"candidates", candidates}}});
        return "";
    }

    // E_NS_006: Type not found.
    de.emit(diag::BasicDiag::NsTypeNotFound, loc, length, {{diag::Replacement{"type", typeName}}});
    return "";
}

} // namespace il::frontends::basic
