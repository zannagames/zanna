//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/localization/rt_locale_manager_internal.h
// Purpose: Shared arena allocator type + the CLDR plural-rule parser boundary
//   between rt_locale_manager.c (locale loader) and rt_locale_plural.c (parser).
//
// Key invariants:
//   - Every successful arena allocation is zero-initialized and represented by
//     exactly one node in the arena's allocation list.
//   - Plural-rule AST nodes and their range arrays are owned by the same arena
//     as the locale data that references them.
//   - Parser failure never publishes a partial tree to the locale manager.
//
// Ownership/Lifetime:
//   - The locale manager owns each `loc_arena_t` and releases its complete
//     allocation list when the associated locale data is destroyed.
//   - Pointers returned by `loc_arena_alloc` and `parse_rule` remain valid until
//     that owning arena is destroyed; callers must not free them individually.
//
// Links: src/runtime/localization/rt_locale_manager.c (allocator and owner),
//        src/runtime/localization/rt_locale_plural.c (plural-rule parser),
//        src/runtime/localization/rt_locale_data.h (plural AST schema).
//
//===----------------------------------------------------------------------===//

#pragma once

#include <stddef.h>

/// @brief List node recording one allocation owned by a locale arena.
typedef struct loc_arena_alloc {
    /// Allocation returned to an arena client.
    void *ptr;
    /// Next tracked allocation, or NULL at the end of the list.
    struct loc_arena_alloc *next;
} loc_arena_alloc_t;

/// @brief Aggregate owner for dynamically loaded locale data and plural ASTs.
typedef struct loc_arena {
    /// Head of the singly linked allocation list; NULL for an empty arena.
    loc_arena_alloc_t *allocs;
} loc_arena_t;

/// @brief Allocate zero-initialized storage owned by @p arena.
/// @details Adds the allocation to @p arena's tracking list so the locale
///          manager can release all related data in one pass. Allocation
///          failure raises the runtime out-of-memory trap.
/// @param arena Arena that will own the allocation. NULL is rejected.
/// @param size Number of bytes to allocate. Zero is rejected.
/// @return Pointer to @p size zeroed bytes, or NULL when @p arena is NULL or
///         @p size is zero.
void *loc_arena_alloc(loc_arena_t *arena, size_t size);

/// @brief Parse a bounded CLDR plural-rule expression into an arena-owned AST.
/// @details Supports the locale runtime's operand, modulus, comparison, range,
///          `and`, and `or` grammar. A NULL rule is interpreted as `true`.
///          Input longer than 256 bytes, malformed syntax, trailing input, or
///          an invalid arena causes parsing to fail.
/// @param arena Arena that will own every node and range array in the result.
/// @param rule NUL-terminated rule expression, or NULL for an unconditional rule.
/// @return Root of the parsed AST, or NULL if the expression is invalid.
rt_plural_rule_node_t *parse_rule(loc_arena_t *arena, const char *rule);
