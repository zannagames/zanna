//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/text/rt_fuzzy_match.h
// Purpose: C ABI for reusable fuzzy-subsequence matching and quick-open
//          ranking metadata.
//
// Key invariants:
//   - Matching is a greedy, ASCII case-insensitive subsequence comparison.
//   - Successful scores are non-negative; -1 is the ordinary miss sentinel.
//   - Highlight ranges use half-open byte offsets into the candidate.
//   - C++ exceptions are contained inside the implementation's C ABI.
//
// Ownership/Lifetime:
//   - Input runtime strings are borrowed and never retained.
//   - Detailed match Maps and all nested values are fresh runtime objects;
//     callers own the returned Map reference.
//
// Links: src/runtime/text/rt_fuzzy_match.cpp (implementation),
//        src/runtime/rt_map.h (result Maps),
//        src/runtime/rt_seq.h (highlight-range sequence)
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_fuzzy_match.h
 * @brief Declares fuzzy-subsequence scoring and detailed match results.
 * @details The C ABI compares borrowed runtime Strings with greedy ASCII
 *          case-insensitive semantics, returns a nonnegative rank or miss
 *          sentinel, and can build a caller-owned Map containing source text,
 *          match state, score, and half-open highlight ranges.
 */

#pragma once

#include "rt_string.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Rank a candidate using greedy case-insensitive subsequence matching.
/// @details Boundary, consecutive, and exact-case matches receive bonuses;
///          gaps and excess candidate text receive penalties. Successful
///          scores are clamped to zero or above. Null strings are treated as
///          empty, and an empty query matches with score zero.
/// @param query Borrowed query string.
/// @param candidate Borrowed candidate string.
/// @return Non-negative score for a match, `-1` for a miss, or zero when an
///         internal C++ exception is contained.
int64_t rt_fuzzy_match_score(rt_string query, rt_string candidate);

/// @brief Produce detailed match state, score, source strings, and highlights.
/// @details The returned Map contains `"matched"`, `"score"`, `"query"`,
///          `"candidate"`, and `"ranges"`. Each range is a Map with half-open
///          `"start"` and `"end"` byte offsets into the candidate. A contained
///          exception yields an unmatched, empty fallback result.
/// @param query Borrowed query string; null is treated as empty.
/// @param candidate Borrowed candidate string; null is treated as empty.
/// @return Newly allocated runtime Map owned by the caller.
void *rt_fuzzy_match_match(rt_string query, rt_string candidate);

#ifdef __cplusplus
}
#endif
