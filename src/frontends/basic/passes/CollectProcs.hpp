//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/passes/CollectProcs.hpp
// Purpose: Declares the post-parse pass that annotates procedures declared
//          inside namespaces with canonical namespace and qualified names.
// Key invariants: Qualified names are lowercase ASCII with dot separators.
// Ownership/Lifetime: Mutates the parsed AST in-place; no ownership transfers.
// Links: docs/internals/codemap.md
//
//===----------------------------------------------------------------------===//

#pragma once

#include "frontends/basic/ast/DeclNodes.hpp"

/// @file
/// @brief Declares BASIC namespace-aware procedure identity collection.

namespace il::frontends::basic {

/// @brief Walk the AST and assign qualified names to procedures inside namespaces.
/// @details DFS traverses `prog.main`, maintaining a namespace stack from
///          `NamespaceDecl` nodes. For each FunctionDecl/SubDecl encountered,
///          sets `namespacePath` and `qualifiedName` using canonicalized
///          lowercase segments. Top-level procedures remain unchanged.
///          Registration into semantic tables happens in later phases.
/// @param prog Parsed program to annotate in-place.
/// @post Nested FUNCTION and SUB declarations in @ref Program::main have
///       canonical namespacePath and qualifiedName annotations.
void CollectProcedures(Program &prog);

} // namespace il::frontends::basic
