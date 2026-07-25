//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/ast/DeclNodes.hpp
// Purpose: Defines the parsed BASIC program root that partitions procedure
//          declarations from executable top-level statements.
// Key invariants:
//   - Procedure and main-statement vectors preserve parser source order within
//     their respective partitions.
//   - Every non-null node is uniquely owned through its AST smart pointer.
// Ownership/Lifetime:
//   - Program owns all procedure and main statement subtrees.
//   - SourceLoc is retained by value for diagnostics and later phases.
// Links: src/frontends/basic/ast/StmtNodesAll.hpp,
//        src/frontends/basic/Parser.hpp,
//        src/frontends/basic/AST.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "frontends/basic/ast/StmtNodesAll.hpp"

#include <vector>

/// @file
/// @brief Defines the ownership root of a parsed BASIC abstract syntax tree.

namespace il::frontends::basic {

/// @brief Root node partitioning procedure declarations from main statements.
/// @details The parser places FUNCTION/SUB declarations in @ref procs and
///          executable program-entry statements in @ref main. Later semantic
///          and lowering phases borrow this root while traversing its owned
///          subtrees.
struct Program {
    /// FUNCTION/SUB declaration nodes in their procedure partition order.
    std::vector<ProcDecl> procs;

    /// Executable top-level statements forming the program entry body.
    std::vector<StmtPtr> main;

    /// Source location of the first token represented by the program.
    il::support::SourceLoc loc{};
};

} // namespace il::frontends::basic
