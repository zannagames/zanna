//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/il/io/ParserState.cpp
// Purpose: Define the constructor logic for the shared ParserState object used
//          by the textual IL parser so that the header can remain lightweight.
// Ownership/Lifetime: ParserState keeps a reference to a caller-owned Module.
// Links: docs/il/il-guide.md#reference
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Binds parser state objects to the module they populate.
/// @details The constructor is intentionally out-of-line to centralise the
///          documentation around ownership semantics: parser helpers mutate the
///          caller-provided module directly and therefore rely on the reference
///          stored here remaining valid for the parser's lifetime.

#include "il/internal/io/ParserState.hpp"
#include "il/core/Module.hpp"

namespace il::io::detail {
/// @brief Bind the parser state to a concrete module instance.
///
/// The parser operates over mutable IR, so the constructor captures the module
/// by reference rather than by value.  Storing the reference guarantees that
/// parsed functions, globals, and extern declarations mutate the caller-owned
/// module directly while avoiding copies or lifetime ownership ambiguities.
///
/// @param mod Module that will receive all IR entities materialized by the
///             parser.
/// @param parserLimits Resource budgets copied into the shared parse state.
ParserState::ParserState(il::core::Module &mod, const il::io::ParserLimits &parserLimits)
    : m(mod), limits(parserLimits) {
    // Limits apply to the resulting module, including declarations supplied by
    // callers before this parse operation. This prevents repeated append-style
    // parses from bypassing aggregate block and instruction budgets.
    for (const auto &function : mod.functions) {
        functionNames.insert(function.name);
        totalBlocks += function.blocks.size();
        for (const auto &block : function.blocks)
            totalInstructions += block.instructions.size();
    }
    for (const auto &external : mod.externs)
        externNames.insert(external.name);
    for (const auto &global : mod.globals)
        globalNames.insert(global.name);
}
} // namespace il::io::detail
