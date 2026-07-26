//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/text/rt_pluralize.h
// Purpose: Declares lightweight English noun pluralization, singularization,
//          and count-label formatting based on common rules and lookup tables.
//
// Key invariants:
//   - Handles common English pluralization rules (e.g., -s, -es, -ies, -ves).
//   - Irregular forms (e.g., 'child'/'children', 'person'/'people') use a lookup table.
//   - Listed uncountable nouns (e.g., 'information', 'sheep') are unchanged.
//   - Not a full NLP engine; uncommon edge cases may be incorrect.
//   - Table matching and case handling are byte-oriented rather than Unicode
//     linguistic case folding.
//
// Ownership/Lifetime:
//   - Returned strings are newly allocated; caller must release.
//   - Input strings are borrowed; callers retain ownership.
//
// Links: src/runtime/text/rt_pluralize.c (implementation), src/runtime/core/rt_string.h
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_pluralize.h
 * @brief Declares English noun pluralization, singularization, and count labels.
 * @details Borrowed noun Strings are matched against common irregular and
 *          uncountable vocabularies before byte-oriented suffix rules are
 *          applied. Every operation returns a newly allocated managed String
 *          and intentionally does not provide general linguistic analysis.
 */

#pragma once

#include <stdint.h>

#include "rt_string.h"

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Pluralize an English noun.
/// @details Checks uncountable and irregular tables before applying ordered
///          suffix heuristics for sibilants, consonant-plus-y, f/fe, and
///          consonant-plus-o endings. Case is coarsely preserved.
/// @param word Borrowed singular noun.
/// @return Newly allocated plural form such as `"cats"` or `"children"`.
rt_string rt_pluralize(rt_string word);

/// @brief Singularize an English noun.
/// @details Checks uncountable and reverse irregular tables before heuristically
///          removing `ves`, `ies`, `es`, or `s` endings. Ambiguous English
///          forms may not round-trip.
/// @param word Borrowed plural noun.
/// @return Newly allocated singular form such as `"cat"` or `"child"`.
rt_string rt_singularize(rt_string word);

/// @brief Format a count with the correct singular/plural noun.
/// @details Uses the supplied noun unchanged for counts `1` and `-1`;
///          every other count passes it through `rt_pluralize`.
/// @param count Signed number of items.
/// @param word Borrowed singular noun.
/// @return Newly allocated string such as `"1 item"`, `"5 items"`, or
///         `"0 items"`.
rt_string rt_pluralize_count(int64_t count, rt_string word);

#ifdef __cplusplus
}
#endif
