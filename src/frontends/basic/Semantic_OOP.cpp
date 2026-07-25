//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE in the project root for license information.
//
// File: src/frontends/basic/Semantic_OOP.cpp
// Purpose: Implements the public facade for the phased BASIC OOP index builder.
// Key invariants:
//   - All construction logic runs through one detail::OopIndexBuilder instance.
//   - Optional diagnostic suppression does not alter builder phase ordering.
// Ownership/Lifetime:
//   - The local builder borrows the caller-owned index and optional emitter and
//     does not escape this function.
// Links: src/frontends/basic/Semantic_OOP.hpp,
//        src/frontends/basic/Semantic_OOP_Builder.cpp,
//        src/frontends/basic/Semantic_OOP_Helpers.cpp,
//        src/frontends/basic/detail/Semantic_OOP_Internal.hpp
//
//===----------------------------------------------------------------------===//

#include "frontends/basic/Semantic_OOP.hpp"
#include "frontends/basic/detail/Semantic_OOP_Internal.hpp"

/// @file
/// @brief Implements the public BASIC object-model indexing entry point.

namespace il::frontends::basic {

//===----------------------------------------------------------------------===//
// Public API
//===----------------------------------------------------------------------===//

/// @brief Rebuilds @p index from the object-oriented declarations in @p program.
/// @details Creates the internal phased builder and runs declaration scanning,
///          relationship resolution, cycle detection, virtual layout, and
///          interface-conformance analysis.
/// @param program Parsed program borrowed while the builder traverses its AST.
/// @param index Destination cleared and populated by the builder.
/// @param emitter Optional borrowed sink for semantic errors and warnings.
/// @post @p index contains the builder's complete post-validation result.
void buildOopIndex(const Program &program, OopIndex &index, DiagnosticEmitter *emitter) {
    detail::OopIndexBuilder builder(index, emitter);
    builder.build(program);
}

} // namespace il::frontends::basic
