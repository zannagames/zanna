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

#include <cstddef>
#include <optional>
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

/// @brief Number of integer-class arguments a no-return helper reads.
/// @details Every helper in the set takes only integer or pointer
///          parameters, so this is exactly the count of argument registers a
///          call reads. The counts mirror the C prototypes in
///          src/runtime/core/rt_trap.h, src/runtime/core/rt_error.h, and
///          src/runtime/arrays/rt_array.h.
/// @param symbol Canonical runtime symbol spelling (see isNoReturnRuntimeSymbol).
/// @return The arity, or `std::nullopt` when @p symbol is not a no-return helper.
[[nodiscard]] inline std::optional<std::size_t> noReturnRuntimeSymbolIntArgCount(
    std::string_view symbol) noexcept {
    if (symbol == "rt_trap_ovf" || symbol == "rt_trap_div0" || symbol == "rt_trap_null")
        return 0;
    if (symbol == "rt_trap_raise_error" || symbol == "rt_trap_string" || symbol == "rt_trap")
        return 1;
    if (symbol == "rt_arr_oob_panic")
        return 2;
    return std::nullopt;
}

/// @brief Integer-argument arity of a direct call target, when it is a
///        no-return helper.
/// @param callee Callee spelling as written on the call; aliases are
///        canonicalized first.
/// @return See noReturnRuntimeSymbolIntArgCount().
[[nodiscard]] inline std::optional<std::size_t> noReturnRuntimeCalleeIntArgCount(
    std::string_view callee) {
    if (auto mapped = il::runtime::mapCanonicalRuntimeName(callee))
        return noReturnRuntimeSymbolIntArgCount(*mapped);
    return noReturnRuntimeSymbolIntArgCount(callee);
}

} // namespace zanna::codegen::common
