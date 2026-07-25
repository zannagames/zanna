//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: include/zanna/vm/OpcodeNames.hpp
// Purpose: Provide a stable opcode->name helper for VM diagnostics.
// Key invariants: Forwards to the canonical IL opcode name table to avoid
//                 duplication and drift.
// Ownership/Lifetime: Header-only convenience; no state or allocation.
// Links: docs/il/il-guide.md#reference, src/il/core/OpcodeNames.cpp
//
//===----------------------------------------------------------------------===//

/**
 * @file include/zanna/vm/OpcodeNames.hpp
 * @brief Provides the VM-facing view of canonical IL opcode mnemonics.
 *
 * @details
 * VM diagnostics and embedding clients historically use `il::vm::opcodeName`.
 * This header preserves that convenience API while delegating directly to the
 * IL core's single opcode-name table. Returned views therefore remain stable
 * for the process lifetime and cannot drift from serialization spellings.
 */

#pragma once

#include "il/core/Opcode.hpp"

#include <string_view>

namespace il::vm
{

/**
 * @brief Return the canonical textual mnemonic for an IL opcode.
 *
 * @param op Opcode enumeration value to translate.
 * @return A non-owning view of static, null-terminated mnemonic storage. The
 *         view is empty when `op` is outside the valid opcode range, including
 *         the `Opcode::Count` sentinel.
 *
 * @note The function performs no allocation and is safe to call concurrently.
 */
inline std::string_view opcodeName(il::core::Opcode op) noexcept
{
    return il::core::toString(op);
}

} // namespace il::vm
