//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/il/runtime/HelperEffects.hpp
// Purpose: Provide shared classification of runtime helper side-effect flags.
// Key invariants: Effect tables remain aligned with runtime helper semantics and
//                 are reused across debug registries and runtime descriptor
//                 builders to ensure consistent optimisation metadata.
// Ownership/Lifetime: Header-only utilities containing constexpr tables.
// Links: docs/il/il-guide.md#reference, docs/internals/architecture.md#runtime-signatures
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Defines shared side-effect classifications for runtime helpers.

#pragma once

#include <array>
#include <string_view>

namespace il::runtime {

/// @brief Describe behavioural flags associated with a runtime helper.
struct HelperEffects {
    /// @brief Construct an effect classification from individual flags.
    /// @param nothrowIn Whether the helper cannot throw or trap.
    /// @param readonlyIn Whether the helper may read but cannot write memory.
    /// @param pureIn Whether the helper has no observable side effects.
    /// @param knownIn Whether the helper is explicitly classified even when all
    ///                effect flags are false.
    constexpr HelperEffects(bool nothrowIn = false,
                            bool readonlyIn = false,
                            bool pureIn = false,
                            bool knownIn = false)
        : nothrow(nothrowIn), readonly(readonlyIn), pure(pureIn),
          known(knownIn || nothrowIn || readonlyIn || pureIn) {}

    bool nothrow = false;  ///< Helper cannot throw or trap under defined behaviour.
    bool readonly = false; ///< Helper may read memory but performs no writes.
    bool pure = false;     ///< Helper has no observable side effects.
    bool known = false;    ///< Helper was found in the table, even if all effects are false.
};

/// @brief Lookup helper side-effect metadata by symbol name.
/// @param name Runtime helper symbol (e.g., "rt_str_len").
/// @return Effect classification; default-initialised when unknown.
/// @details This table provides fast constexpr lookup for common runtime helpers.
///          For comprehensive metadata, also consult the runtime signature registry.
///          Effects are: {nothrow, readonly, pure}.
///          - pure: No observable side effects; can eliminate if result unused
///          - readonly: May read memory but no writes; can reorder with stores
///          - nothrow: Cannot throw or trap; can hoist across exception boundaries
inline HelperEffects classifyHelperEffects(std::string_view name) {
    /// @brief One symbol-to-effects row in the local classification table.
    struct Entry {
        /// Runtime C symbol spelling.
        std::string_view name;
        /// Known effects for @ref name.
        HelperEffects effects;
    };

    // Pure math helpers: nothrow=true, readonly=false, pure=true
    // These perform pure computation with no memory access.
    constexpr std::array<Entry, 33> kEntries{{
        // Math: pure computation, no memory access
        Entry{"rt_cdbl_from_any", HelperEffects{true, false, true}},
        Entry{"rt_int_floor", HelperEffects{true, false, true}},
        Entry{"rt_fix_trunc", HelperEffects{true, false, true}},
        Entry{"rt_round_even", HelperEffects{true, false, true}},
        Entry{"rt_sqrt", HelperEffects{true, false, true}},
        Entry{"rt_abs_f64", HelperEffects{true, false, true}},
        Entry{"rt_abs_i64", HelperEffects{false, false, true, true}},
        Entry{"rt_floor", HelperEffects{true, false, true}},
        Entry{"rt_ceil", HelperEffects{true, false, true}},
        Entry{"rt_sin", HelperEffects{true, false, true}},
        Entry{"rt_cos", HelperEffects{true, false, true}},
        Entry{"rt_tan", HelperEffects{true, false, true}},
        Entry{"rt_atan", HelperEffects{true, false, true}},
        Entry{"rt_exp", HelperEffects{true, false, true}},
        Entry{"rt_log", HelperEffects{true, false, true}},
        Entry{"rt_sgn_i64", HelperEffects{true, false, true}},
        Entry{"rt_sgn_f64", HelperEffects{true, false, true}},

        // String inspection: readonly (reads string memory), not pure.
        // These may trap on invalid handles, so they are not marked nothrow.
        Entry{"rt_str_len", HelperEffects{false, true, false}},
        Entry{"rt_str_byte_at", HelperEffects{false, true, false}},
        Entry{"rt_str_index_of", HelperEffects{false, true, false}},
        Entry{"rt_str_instr3", HelperEffects{false, true, false}},
        Entry{"rt_str_eq", HelperEffects{false, true, false}},
        Entry{"rt_str_lt", HelperEffects{false, true, false}},
        Entry{"rt_str_le", HelperEffects{false, true, false}},
        Entry{"rt_str_gt", HelperEffects{false, true, false}},
        Entry{"rt_str_ge", HelperEffects{false, true, false}},
        Entry{"rt_str_asc", HelperEffects{false, true, false}},

        // Array length queries: readonly (reads array header)
        Entry{"rt_arr_i32_len", HelperEffects{true, true, false}},
        Entry{"rt_arr_str_len", HelperEffects{true, true, false}},

        // Conversion helpers.
        Entry{"rt_str_chr", HelperEffects{true, false, false}},
        Entry{"rt_to_int", HelperEffects{false, false, false, true}},
        Entry{"rt_to_double", HelperEffects{false, false, false, true}},
        Entry{"rt_val", HelperEffects{true, false, false}},
    }};

    for (const auto &entry : kEntries)
        if (entry.name == name)
            return entry.effects;
    return {};
}

} // namespace il::runtime
