//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/text/rt_template.h
// Purpose: Declares lightweight byte-string templating with keyed Map,
//          positional Seq, custom-delimiter, inspection, and escaping APIs.
//
// Key invariants:
//   - Placeholders have the form {{key}}; whitespace around key is trimmed.
//   - Missing or non-string values leave placeholders as literal text.
//   - Empty placeholders {{}} are left as-is.
//   - Custom delimiters can be configured via rt_template_render_with.
//   - Only raw or boxed runtime strings are substituted.
//   - Doubling an opening or closing delimiter emits it literally.
//
// Ownership/Lifetime:
//   - Returned nonempty strings are newly allocated; empty paths may return an
//     immutable empty-string sentinel.
//   - Input template and value strings are borrowed for the duration of rendering.
//
// Links: src/runtime/text/rt_template.c (implementation), src/runtime/core/rt_string.h
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_template.h
 * @brief Declares keyed and positional lightweight template operations.
 * @details Borrowed templates render Map or sequence String values through
 *          default or caller-selected delimiters, preserve missing and invalid
 *          substitutions, inspect placeholder membership, extract keys, and
 *          escape literal delimiters into caller-owned output.
 */

#pragma once

#include "rt_string.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Render template with Map values.
/// @details Trims placeholder whitespace, substitutes raw or boxed strings, and
///          preserves missing, null, non-string, and empty-key placeholders.
/// @param tmpl Borrowed template with `{{key}}` placeholders.
/// @param values Borrowed Map with runtime-string keys.
/// @return Caller-owned rendered string.
/// @note Null @p tmpl or @p values traps.
rt_string rt_template_render(rt_string tmpl, void *values);

/// @brief Render template with Seq values (positional).
/// @details Placeholder keys must be nonnegative decimal indexes. Invalid,
///          overflowing, out-of-range, or non-string elements remain literal.
/// @param tmpl Borrowed template with `{{0}}`, `{{1}}`, and similar placeholders.
/// @param values Borrowed Seq of substitution values.
/// @return Caller-owned rendered string.
/// @note Null @p tmpl or @p values traps.
rt_string rt_template_render_seq(rt_string tmpl, void *values);

/// @brief Render template with custom delimiters.
/// @details Delimiters are arbitrary nonempty byte strings; lookup otherwise
///          follows `rt_template_render`.
/// @param tmpl Borrowed template string.
/// @param values Borrowed Map with runtime-string keys.
/// @param prefix Borrowed nonempty placeholder prefix.
/// @param suffix Borrowed nonempty placeholder suffix.
/// @return Caller-owned rendered string.
/// @note Null required arguments or empty delimiters trap.
rt_string rt_template_render_with(rt_string tmpl, void *values, rt_string prefix, rt_string suffix);

/// @brief Check if template contains a placeholder for key.
/// @details Uses default delimiters, skips doubled opening delimiters, and
///          compares the trimmed placeholder key byte-for-byte.
/// @param tmpl Borrowed template string.
/// @param key Borrowed nonempty key.
/// @return `1` if @p tmpl contains `{{key}}`, otherwise `0`.
int8_t rt_template_has(rt_string tmpl, rt_string key);

/// @brief Extract all placeholder keys from template.
/// @details Uses default delimiters and omits escaped openings and empty keys.
/// @param tmpl Borrowed template string; null produces an empty Bag.
/// @return Caller-owned runtime-managed Bag of unique trimmed keys.
void *rt_template_keys(rt_string tmpl);

/// @brief Escape {{ and }} in text for literal output.
/// @details Doubles each non-overlapping delimiter pair; this is not HTML escaping.
/// @param text Borrowed byte string; null is treated as empty.
/// @return Caller-owned string with `{{` changed to `{{{{` and `}}` to `}}}}`.
rt_string rt_template_escape(rt_string text);

#ifdef __cplusplus
}
#endif
