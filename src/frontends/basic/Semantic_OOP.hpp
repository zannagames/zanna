//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/Semantic_OOP.hpp
// Purpose: Declares the public entry point that derives BASIC object-model
//          metadata from a parsed program.
// Key invariants:
//   - Each invocation replaces the destination index with metadata from the
//     supplied program.
//   - Declaration collection precedes name resolution, inheritance analysis,
//     virtual layout, and interface-conformance checks.
// Ownership/Lifetime:
//   - The builder borrows the program, destination index, and optional emitter
//     only for the duration of the call.
// Links: src/frontends/basic/Semantic_OOP.cpp,
//        src/frontends/basic/Semantic_OOP_Builder.cpp,
//        src/frontends/basic/Semantic_OOP_Helpers.cpp,
//        src/frontends/basic/OopIndex.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "frontends/basic/OopIndex.hpp"
#include "frontends/basic/ast/NodeFwd.hpp"

/// @file
/// @brief Declares construction of the BASIC semantic object-model index.

namespace il::frontends::basic {

/// @brief Diagnostic sink used optionally while constructing the index.
class DiagnosticEmitter;

/// @brief Populate @p index with class metadata extracted from @p program.
/// @details This is the main entry point that builds the OopIndex from a parsed
///          BASIC program. It clears previous index state, walks class,
///          interface, enum, namespace, and USING declarations, resolves
///          inheritance, rejects cycles, assigns virtual slots, and records
///          interface implementations for later compiler phases.
/// @param program Parsed BASIC program borrowed for the duration of the build.
/// @param index Destination whose previous contents are cleared and replaced.
/// @param emitter Optional borrowed diagnostic sink; null suppresses reporting
///                without disabling index construction.
/// @post @p index contains the best valid metadata derivable from @p program;
///       diagnosed invalid relationships may be omitted or downgraded to safe
///       empty state.
void buildOopIndex(const Program &program, OopIndex &index, DiagnosticEmitter *emitter);

} // namespace il::frontends::basic
