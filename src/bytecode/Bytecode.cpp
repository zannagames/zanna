//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/bytecode/Bytecode.cpp
// Purpose: Map bytecode opcode values to stable diagnostic mnemonics.
// Key invariants:
//   - Every opcode defined in Bytecode.def maps to its enumerator spelling.
//   - Unknown numeric values map to the non-null string "UNKNOWN".
// Ownership/Lifetime:
//   - Returned names are string literals with static storage duration.
// Links: src/bytecode/Bytecode.hpp, src/bytecode/Bytecode.def
//
//===----------------------------------------------------------------------===//

/**
 * @file src/bytecode/Bytecode.cpp
 * @brief Implements the canonical bytecode opcode-name lookup.
 *
 * @details
 * The switch is generated from the same X-macro registry as `BCOpcode`, keeping
 * disassembly, trace, and diagnostic spellings synchronized with the executable
 * opcode set. No dynamic storage or mutable lookup table is required.
 */

#include "bytecode/Bytecode.hpp"

namespace zanna::bytecode {

/// @brief Return a stable, human-readable mnemonic for a bytecode opcode.
/// @param op The opcode to name.
/// @return A static string literal (never NULL); "UNKNOWN" for any value
///         outside the defined BCOpcode set. Used by the disassembler and
///         trace/diagnostic output, so the names are part of the debug ABI.
/// @note The function performs no allocation and is safe for concurrent calls.
const char *opcodeName(BCOpcode op) {
    switch (op) {
#define BC_OPCODE(name, value)                                                                     \
    case BCOpcode::name:                                                                           \
        return #name;
#include "bytecode/Bytecode.def"
#undef BC_OPCODE
    }
    return "UNKNOWN";
}

} // namespace zanna::bytecode
