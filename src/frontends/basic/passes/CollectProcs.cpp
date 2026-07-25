//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/passes/CollectProcs.cpp
// Purpose: Post-parse pass to collect procedures declared inside NAMESPACE blocks
//          and assign fully-qualified names to FunctionDecl/SubDecl.
// Key invariants:
//   - Namespace segments are canonicalized to lowercase ASCII.
//   - Qualified names join segments with '.' and include the procedure name.
//   - AST structure is not flattened; only annotations are added to nodes.
// Ownership/Lifetime: Mutates AST nodes in-place; no allocations beyond strings.
// Links: docs/internals/codemap.md
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implementation of CollectProcedures pass for BASIC frontend.

#include "frontends/basic/passes/CollectProcs.hpp"

#include "frontends/basic/IdentifierUtil.hpp"
#include "frontends/basic/ast/StmtDecl.hpp"

#include <functional>

namespace il::frontends::basic {

/// @brief Assign namespace paths and canonical qualified names to procedures in a program.
/// @param prog The parsed program (mutated in place; only annotations are added).
/// @details Walks the statement tree depth-first, maintaining a stack of canonicalized
///          (lowercased) namespace segments. Each FunctionDecl/SubDecl records its namespace
///          path and a qualified name formed by joining the namespace segments with '.' and
///          appending the suffix-stripped, canonicalized procedure name. AST structure is left
///          intact; class qualification is handled elsewhere.
/// @post Every nested FUNCTION/SUB reached through namespace bodies carries
///       canonical identity annotations.
void CollectProcedures(Program &prog) {
    std::vector<std::string> nsStack;

    // Helper to set fields on a procedure declaration.
    auto stripSuffix = [](std::string_view name) -> std::string_view {
        if (name.empty())
            return name;
        char last = name.back();
        switch (last) {
            case '$':
            case '#':
            case '!':
            case '&':
            case '%':
                return name.substr(0, name.size() - 1);
            default:
                return name;
        }
    };

    auto assignProcIdentity = [&](auto &decl) {
        decl.namespacePath = nsStack; // copy canonical segments

        // Build canonical qualified name: nsStack.join('.') + optional '.' + proc ident
        std::string nsQual = CanonicalizeQualified(nsStack);
        std::string procCanon = CanonicalizeIdent(stripSuffix(decl.name));

        std::string qn;
        if (!nsQual.empty()) {
            qn = nsQual;
            if (!procCanon.empty()) {
                qn.push_back('.');
                qn += procCanon;
            }
        } else {
            qn = procCanon;
        }
        decl.qualifiedName = std::move(qn);
    };

    // Recursive DFS over statement lists.
    std::function<void(std::vector<StmtPtr> &)> scan;
    scan = [&](std::vector<StmtPtr> &stmts) {
        for (auto &stmtPtr : stmts) {
            if (!stmtPtr)
                continue;
            switch (stmtPtr->stmtKind()) {
                case Stmt::Kind::NamespaceDecl: {
                    auto &ns = static_cast<NamespaceDecl &>(*stmtPtr);
                    // Push canonicalized segments
                    for (const auto &seg : ns.path) {
                        std::string canon = CanonicalizeIdent(seg);
                        if (!canon.empty())
                            nsStack.push_back(std::move(canon));
                        else
                            nsStack.push_back(seg); // fallback: preserve as-is
                    }
                    // Recurse into body
                    scan(ns.body);
                    // Pop
                    if (nsStack.size() >= ns.path.size())
                        nsStack.resize(nsStack.size() - ns.path.size());
                    else
                        nsStack.clear();
                    break;
                }
                case Stmt::Kind::FunctionDecl: {
                    auto &fn = static_cast<FunctionDecl &>(*stmtPtr);
                    assignProcIdentity(fn);
                    break;
                }
                case Stmt::Kind::SubDecl: {
                    auto &sub = static_cast<SubDecl &>(*stmtPtr);
                    assignProcIdentity(sub);
                    break;
                }
                case Stmt::Kind::ClassDecl:
                    // Class/type qualification handled elsewhere; no-op here.
                    break;
                default:
                    break;
            }
        }
    };

    scan(prog.main);
}

} // namespace il::frontends::basic
