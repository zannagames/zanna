//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/ast/StmtNodesAll.hpp
// Purpose: Aggregates the BASIC statement base, control-flow, declaration, and
//          expression/IO node families behind one include.
// Key invariants:
//   - Every concrete Stmt::Kind family is reachable through this header.
//   - This header defines no parallel node types or ownership behavior.
// Ownership/Lifetime:
//   - Individual family headers define unique ownership of nested expressions
//     and statements.
// Links: src/frontends/basic/ast/StmtBase.hpp,
//        src/frontends/basic/ast/StmtControl.hpp,
//        src/frontends/basic/ast/StmtDecl.hpp,
//        src/frontends/basic/ast/StmtExpr.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

/// @file
/// @brief Includes every BASIC statement AST node family.

#include "frontends/basic/ast/StmtBase.hpp"
#include "frontends/basic/ast/StmtControl.hpp"
#include "frontends/basic/ast/StmtDecl.hpp"
#include "frontends/basic/ast/StmtExpr.hpp"
