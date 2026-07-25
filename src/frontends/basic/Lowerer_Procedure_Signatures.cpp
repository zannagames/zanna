//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/Lowerer_Procedure_Signatures.cpp
// Purpose: Procedure signature collection and lookup for BASIC lowering.
//
// Phase: Signature Collection (runs during program scanning)
//
// Key Invariants:
// - Each signature has one emitted-name key plus a canonical unqualified alias
// - Canonical aliases support case-insensitive unqualified lookup
// - Parameter types include array/object/byref classification
//
// Ownership/Lifetime: Operates on borrowed Lowerer instance.
//
//===----------------------------------------------------------------------===//

/**
 * @file Lowerer_Procedure_Signatures.cpp
 * @brief Implements procedure signature indexing, aliasing, and lookup.
 *
 * Collection clears the preceding run, stores each signature under its emitted
 * qualified-or-unqualified name, and records one canonical unqualified alias.
 * Cached signature pointers remain valid only until the maps are mutated.
 */

#include "frontends/basic/AST.hpp"
#include "frontends/basic/ASTUtils.hpp"
#include "frontends/basic/IdentifierUtil.hpp"
#include "frontends/basic/Lowerer.hpp"
#include "frontends/basic/LoweringPipeline.hpp"

using namespace il::core;

namespace il::frontends::basic {

using pipeline_detail::coreTypeForAstType;

// =============================================================================
// Signature Collection
// =============================================================================

/// @brief Scan a BASIC program and cache signatures for all declared procedures.
/// @details Visits each function and subroutine declaration, converting the AST
///          parameter and return types into IL types stored in the owning
///          @ref Lowerer.  Array parameters are normalised to pointer types so
///          later lowering logic can allocate the appropriate slots without
///          inspecting the AST again. Both the dedicated procedure list and
///          namespace declarations nested in the main statement tree are scanned.
/// @param prog Program whose declarations should be indexed.
void ProcedureLowering::collectProcedureSignatures(const Program &prog) {
    lowerer.procSignatures.clear();
    lowerer.procNameAliases.clear();

    /// Build an IL signature from one return type and AST parameter sequence.
    auto buildSig = [&](il::core::Type ret, const auto &params) {
        Lowerer::ProcedureSignature sig;
        sig.retType = ret;
        sig.paramTypes.reserve(params.size());
        sig.byRefFlags.reserve(params.size());
        for (const auto &p : params) {
            // BUG-060 fix: Handle object-typed parameters
            il::core::Type ty;
            if (p.is_array) {
                ty = il::core::Type(il::core::Type::Kind::Ptr);
            } else if (!p.objectClass.empty()) {
                // Object parameter - use Ptr type
                ty = il::core::Type(il::core::Type::Kind::Ptr);
            } else if (p.isByRef) {
                // BYREF scalar/string/bool pass pointer to storage
                ty = il::core::Type(il::core::Type::Kind::Ptr);
            } else {
                ty = coreTypeForAstType(p.type);
            }
            sig.paramTypes.push_back(ty);
            sig.byRefFlags.push_back(p.isByRef);
        }
        return sig;
    };

    /// Register under the emitted name and add a canonical unqualified alias.
    auto registerSig =
        [&](const std::string &unqual, const std::string &qual, Lowerer::ProcedureSignature sig) {
            const bool hasQual = !qual.empty();
            const std::string &key = hasQual ? qual : unqual;
            lowerer.procSignatures.emplace(key, std::move(sig));
            // Map canonical unqualified name to the resolved key used for emission
            // BUG-BAS-001 fix: Strip type suffix before canonicalizing
            std::string canon = CanonicalizeIdent(StripTypeSuffix(unqual));
            if (!canon.empty())
                lowerer.procNameAliases.emplace(canon, key);
        };

    // Process top-level procedure declarations
    for (const auto &decl : prog.procs) {
        if (auto *fn = as<const FunctionDecl>(*decl)) {
            il::core::Type retTy =
                (!fn->explicitClassRetQname.empty())
                    ? il::core::Type(il::core::Type::Kind::Ptr)
                    : lowerer.functionRetTypeFromHint(fn->name, fn->explicitRetType);
            auto sig = buildSig(retTy, fn->params);
            registerSig(fn->name, fn->qualifiedName, std::move(sig));
        } else if (auto *sub = as<const SubDecl>(*decl)) {
            auto sig = buildSig(il::core::Type(il::core::Type::Kind::Void), sub->params);
            registerSig(sub->name, sub->qualifiedName, std::move(sig));
        }
    }

    // Also scan namespace blocks in main for nested procedures.
    /// Recursively collect declarations nested in namespace statement bodies.
    std::function<void(const std::vector<StmtPtr> &)> scan;
    scan = [&](const std::vector<StmtPtr> &stmts) {
        for (const auto &stmtPtr : stmts) {
            if (!stmtPtr)
                continue;
            switch (stmtPtr->stmtKind()) {
                case Stmt::Kind::NamespaceDecl:
                    scan(static_cast<const NamespaceDecl &>(*stmtPtr).body);
                    break;
                case Stmt::Kind::FunctionDecl: {
                    const auto &fn = static_cast<const FunctionDecl &>(*stmtPtr);
                    il::core::Type retTy =
                        (!fn.explicitClassRetQname.empty())
                            ? il::core::Type(il::core::Type::Kind::Ptr)
                            : lowerer.functionRetTypeFromHint(fn.name, fn.explicitRetType);
                    auto sig = buildSig(retTy, fn.params);
                    registerSig(fn.name, fn.qualifiedName, std::move(sig));
                    break;
                }
                case Stmt::Kind::SubDecl: {
                    const auto &sub = static_cast<const SubDecl &>(*stmtPtr);
                    auto sig = buildSig(il::core::Type(il::core::Type::Kind::Void), sub.params);
                    registerSig(sub.name, sub.qualifiedName, std::move(sig));
                    break;
                }
                default:
                    break;
            }
        }
    };
    scan(prog.main);
}

// =============================================================================
// Signature Lookup
// =============================================================================

/// @brief Canonicalize a qualified procedure name (handles dots and type suffixes).
/// @details Strips trailing type suffix, splits by dots, canonicalizes each segment,
///          and joins back together. This matches how CollectProcedures builds
///          qualified names for namespace functions.
/// @param name Raw procedure name like "MyModule.Helper$"
/// @return Canonical qualified name like "mymodule.helper"
static std::string CanonicalizeQualifiedName(std::string_view name) {
    if (name.empty())
        return std::string{};

    // Strip trailing type suffix if present
    char last = name.back();
    if (last == '$' || last == '%' || last == '#' || last == '!' || last == '&')
        name = name.substr(0, name.size() - 1);

    // Split by dots and canonicalize each segment
    std::vector<std::string> segments = SplitDots(name);
    return CanonicalizeQualified(segments);
}

/// @brief Retrieve a cached procedure signature when available.
/// @details Looks up metadata gathered during @ref collectProcedureSignatures so
///          later lowering stages can inspect parameter and return types without
///          re-traversing the AST.
/// @param name Name of the procedure whose signature is requested.
/// @return Pointer to the cached signature or @c nullptr when unknown.
const Lowerer::ProcedureSignature *Lowerer::findProcSignature(const std::string &name) const {
    auto it = procSignatures.find(name);
    if (it == procSignatures.end()) {
        auto aliasIt = procNameAliases.find(name);
        if (aliasIt != procNameAliases.end()) {
            auto it2 = procSignatures.find(aliasIt->second);
            if (it2 != procSignatures.end())
                return &it2->second;
        }
        // Try case-insensitive alias: canonicalize key (simple identifiers)
        std::string canon = CanonicalizeIdent(name);
        if (!canon.empty()) {
            auto itAlias2 = procNameAliases.find(canon);
            if (itAlias2 != procNameAliases.end()) {
                auto it3 = procSignatures.find(itAlias2->second);
                if (it3 != procSignatures.end())
                    return &it3->second;
            }
        }
        // BUG-BAS-001 fix: Try canonicalizing as qualified name (handles dots and suffixes)
        std::string qualCanon = CanonicalizeQualifiedName(name);
        if (!qualCanon.empty()) {
            auto it4 = procSignatures.find(qualCanon);
            if (it4 != procSignatures.end())
                return &it4->second;
        }
        return nullptr;
    }
    return &it->second;
}

/// @brief Resolve a procedure call name to its canonical IL function name.
/// @details First probes the alias map using @p name exactly. If absent, it
///          canonicalizes a possibly dotted/suffixed qualified name and returns
///          that spelling only when a signature key exists. Otherwise the input
///          is preserved.
/// @param name Procedure name as written in the source.
/// @return Canonical IL name or the original name if no alias exists.
std::string Lowerer::resolveCalleeName(const std::string &name) const {
    auto it = procNameAliases.find(name);
    if (it != procNameAliases.end())
        return it->second;
    // BUG-BAS-001 fix: Try canonicalizing as qualified name
    std::string qualCanon = CanonicalizeQualifiedName(name);
    if (!qualCanon.empty() && procSignatures.find(qualCanon) != procSignatures.end())
        return qualCanon;
    return name;
}

/// @brief Forward signature collection to the procedure lowering helper.
/// @param prog Program containing declarations to index.
void Lowerer::collectProcedureSignatures(const Program &prog) {
    procedureLowering->collectProcedureSignatures(prog);
}

} // namespace il::frontends::basic
