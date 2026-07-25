//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/core/rt_string_intern.h
// Purpose: Declares process-global byte-string canonicalization and the
// pointer-identity comparison available for successfully interned handles.
//
// Key invariants:
//   - Each unique byte sequence maps to exactly one canonical rt_string pointer.
//   - Interned strings are retained by the table and treated as immortal during normal operation.
//   - rt_string_interned_eq is literal pointer comparison; it represents
//     content equality only when both operands were successfully interned.
//   - Table uses open addressing with 5/8 load factor and power-of-two capacity.
//   - Intern calls are mutually serialized. Drain requires external quiescence.
//
// Ownership/Lifetime:
//   - Successful table hits/inserts return a new retained pointer that the
//     caller releases. Initial table-allocation failure returns the original
//     input claim unchanged and unretained.
//   - The intern table retains its own reference to canonical strings; they are freed only at
//     shutdown/reset when rt_string_intern_drain releases that claim.
//
// Links: src/runtime/core/rt_string_intern.c (implementation), src/runtime/core/rt_string.h
//
//===----------------------------------------------------------------------===//
/// @file
/// @brief Global canonical runtime byte-string table and identity comparison.
#pragma once

#include "rt_string.h"

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Intern @p s, returning the canonical rt_string for its byte content.
/// @details Hashes and compares complete stored byte spans, including embedded
///          NULs. On a hit, increments the canonical string's refcount. On a
///          miss, inserts @p s itself with one table retain and produces a
///          second retain for the caller.
///
///          After interning, two strings with equal content will share the same
///          pointer, enabling O(1) equality via @ref rt_string_interned_eq instead
///          of O(n) `memcmp`. Retain traps are re-raised only after lock cleanup.
///
///          If initial table allocation fails, @p s is returned unchanged,
///          without a new retain and without canonicalization. Null returns
///          NULL without accessing the table.
/// @param s Borrowed live string to intern; may be NULL.
/// @return Owned canonical reference on table success, unchanged input claim
///         after initial table-allocation failure, or `NULL` for null input or
///         after a returning retain trap.
rt_string rt_string_intern(rt_string s);

/// @brief Test pointer equality for two interned strings.
/// @details Both @p a and @p b must have been obtained from @ref rt_string_intern.
///          Successfully canonicalized equal byte strings share a pointer, so
///          this O(1) predicate then represents content equality. For arbitrary
///          non-interned handles it tests identity only; two nulls compare equal.
/// @param a First canonical handle; may be NULL.
/// @param b Second canonical handle; may be NULL.
/// @return One when the pointers are identical; otherwise zero.
static inline int rt_string_interned_eq(rt_string a, rt_string b) {
    return a == b;
}

/// @brief Release all interned strings and reset the table.
/// @details Decrements the refcount of every string held by the table and frees
///          slot storage. On Windows, also destroys and resets the lazily
///          initialized critical section so later interning can recreate it.
///          Canonical strings remain live if other owners still retain them.
/// @warning No thread may call, enter, or already be executing
///          @ref rt_string_intern concurrently with this shutdown/reset call.
void rt_string_intern_drain(void);

#ifdef __cplusplus
}
#endif
