//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: il/io/ParserLimits.hpp
// Purpose: Define configurable resource budgets for textual IL parsing.
// Key invariants: Defaults are large enough for generated compiler output while
//                 bounding allocation from untrusted source text.
// Ownership/Lifetime: Plain value configuration copied into parser state.
// Links: docs/adr/0111-il-text-resource-limits.md
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Declares configurable resource budgets for textual IL parsing.
 *
 * @details The limits bound physical input size and the amount of IR one parse
 *          may create, protecting callers from unbounded allocation or work on
 *          untrusted text. Defaults accommodate generated compiler output;
 *          callers can copy the value and tighten individual dimensions.
 */

#pragma once

#include <cstddef>

namespace il::io {

/// @brief Resource budgets enforced while parsing textual IL.
/// @details Every limit is inclusive: input or resulting IR may contain exactly
///          the configured amount, but exceeding it produces a resource-limit
///          diagnostic. Callers may copy and adjust this value to tighten
///          budgets for untrusted input.
struct ParserLimits {
    /// @brief Maximum bytes retained from a single physical input line.
    std::size_t maxLineBytes{1U << 20};
    /// @brief Maximum number of physical lines consumed from the stream.
    std::size_t maxLines{1'000'000};
    /// @brief Maximum total function definitions in the destination module.
    std::size_t maxFunctions{100'000};
    /// @brief Maximum total external declarations in the destination module.
    std::size_t maxExterns{100'000};
    /// @brief Maximum total global declarations in the destination module.
    std::size_t maxGlobals{100'000};
    /// @brief Maximum total basic blocks across all functions.
    std::size_t maxBlocks{1'000'000};
    /// @brief Maximum total instructions across all basic blocks.
    std::size_t maxInstructions{10'000'000};
    /// @brief Maximum comma-separated value operands accepted by one instruction.
    std::size_t maxValuesPerInstruction{65'535};
    /// @brief Maximum distinct temporary identifiers permitted in one function.
    std::size_t maxTempsPerFunction{10'000'000};
};

} // namespace il::io
