//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/ast/StmtNodes.hpp
// Purpose: Preserves the legacy umbrella include for every BASIC statement node.
// Key invariants:
//   - This header contributes no declarations beyond StmtNodesAll.hpp.
//   - Legacy and direct aggregate includes expose the same statement families.
// Ownership/Lifetime:
//   - Ownership rules are defined by the included node-family headers.
// Links: src/frontends/basic/ast/StmtNodesAll.hpp,
//        src/frontends/basic/AST.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

/// @file
/// @brief Compatibility include for the complete BASIC statement AST.

#include "frontends/basic/ast/StmtNodesAll.hpp"
