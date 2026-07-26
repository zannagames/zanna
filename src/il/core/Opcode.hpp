//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: il/core/Opcode.hpp
// Purpose: Declares the Opcode enum class -- all instruction operation codes
//          supported by Zanna IL. Generated from Opcode.def via X-macros,
//          covering arithmetic, comparisons, memory, control flow, calls,
//          casts, exception handling, and bitwise operations.
// Key invariants:
//   - Opcode::Count is a sentinel past the last valid enumerator.
//   - toString() returns a spec-compliant lowercase mnemonic for every opcode.
//   - Opcode values are contiguous starting from 0.
// Ownership/Lifetime: Enum is a value type with no dynamic resources.
// Links: docs/il/il-guide.md#reference, il/core/Opcode.def
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Declares the canonical enumeration of all Zanna IL opcodes.
 *
 * @details The opcode list is generated from `Opcode.def` so parsing,
 *          verification, interpretation, and code generation share one
 *          contiguous identity space. `Opcode::Count` is a non-instruction
 *          sentinel used to size indexed metadata tables.
 */

#pragma once

#include <cstddef>

namespace il::core {

/// @brief All instruction opcodes defined by the IL.
/// @see docs/il/il-guide.md#reference §3 for opcode descriptions.
enum class Opcode {
/// @def IL_OPCODE
/// @brief Expands one opcode-definition record into its enumeration name.
/// @param NAME C++ enumerator identifying the opcode.
/// @param ... Remaining canonical opcode metadata, unused by this expansion.
#define IL_OPCODE(NAME, ...) NAME,
#include "il/core/Opcode.def"
#undef IL_OPCODE
    Count
};

/// @brief Total number of opcodes defined by the IL.
constexpr size_t kNumOpcodes = static_cast<size_t>(Opcode::Count);

/// @brief Convert opcode @p op to its mnemonic string.
/// @param op Opcode to stringify.
/// @return Lowercase mnemonic defined by the IL spec.
const char *toString(Opcode op);

} // namespace il::core
