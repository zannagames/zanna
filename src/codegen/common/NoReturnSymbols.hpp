//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/common/NoReturnSymbols.hpp
// Purpose: The one list of runtime helpers that never return to their
//          caller, shared by both native backends so their CFG builders,
//          lowering, and verifiers agree on which calls end a block.
// Key invariants:
//   - The set must stay in sync with the runtime helpers that genuinely trap
//     or terminate; adding a returning symbol here would prune live code.
//   - Callee spellings are canonicalized through mapCanonicalRuntimeName
//     before the fixed set is consulted, so aliased names match.
// Ownership/Lifetime:
//   - Stateless inline predicates.
// Links: src/codegen/aarch64/Noreturn.hpp, src/codegen/x86_64/Noreturn.hpp,
//        src/il/runtime/RuntimeNameMap.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "il/runtime/RuntimeNameMap.hpp"

#include <string_view>

/// @file
/// @brief Declares the backend-neutral no-return runtime symbol predicates.

namespace zanna::codegen::common {

/// @brief Tests a canonical runtime symbol against the no-return helper set.
/// @param symbol Runtime symbol spelling after any desired canonicalization.
/// @return `true` only for a helper known to trap or terminate execution.
[[nodiscard]] inline bool isNoReturnRuntimeSymbol(std::string_view symbol) noexcept {
    return symbol == "rt_trap_ovf" || symbol == "rt_trap_div0" || symbol == "rt_trap_null" ||
           symbol == "rt_trap_raise_error" || symbol == "rt_trap_string" ||
           symbol == "rt_arr_oob_panic" || symbol == "rt_trap";
}

/// @brief Tests a call target as spelled in MIR against the no-return set.
/// @details The label is first offered to `mapCanonicalRuntimeName`; if it has
///          no mapping, the raw spelling is checked.
/// @param callee Direct call target label.
/// @return `true` when @p callee names a helper that never returns.
[[nodiscard]] inline bool isNoReturnRuntimeCallee(std::string_view callee) {
    if (auto mapped = il::runtime::mapCanonicalRuntimeName(callee))
        return isNoReturnRuntimeSymbol(*mapped);
    return isNoReturnRuntimeSymbol(callee);
}

} // namespace zanna::codegen::common
