//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/localization/rt_list_format.h
// Purpose: Public C API for Zanna.Localization.ListFormat — joins a list of
//          strings using locale-appropriate conjunction styles (And / Or /
//          Unit / Short). Pulls templates (pair/start/middle/end) from the
//          bound locale's list_format data.
//
// Key invariants:
//   - Empty list -> empty string; single-item list -> the item verbatim;
//     two items -> the pair template; three or more items -> start template
//     wrapped around a right-fold of middle/end templates.
//   - Templates use {0} and {1} positional placeholders; any other
//     placeholder syntax is emitted literally.
//
// Ownership/Lifetime:
//   - Handles are rt_obj_new_i64-allocated; GC-managed.
//   - Each formatter retains its Locale handle and immutable data snapshot.
//   - GetLocale is borrowed; each join result is a fresh string reference.
//
// Links: src/runtime/localization/rt_list_format.c (implementation),
//        src/runtime/localization/rt_locale_data.h (list_format tables),
//        docs/zannalib/localization/formatting.md (user documentation).
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Declares locale-aware conjunction, disjunction, and unit list joining.
 * @details Defines formatter construction and Locale ownership together with
 * fresh-result APIs implementing CLDR pair/start/middle/end template folding
 * for empty, singleton, pair, and longer runtime lists.
 */

#pragma once

#include "rt.hpp"

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Create a list formatter bound to the process's current locale.
/// @return Fresh GC-managed ListFormat, or NULL after an allocation trap.
void *rt_list_format_new(void);
/// @brief Create a list formatter bound to the given @p locale handle.
/// @param locale Locale retained by the result; may be NULL for invariant data.
/// @return Fresh GC-managed ListFormat, or NULL after an allocation trap.
void *rt_list_format_for_locale(void *locale);
/// @brief Return the Locale handle this formatter was built with (borrowed).
/// @param self Valid ListFormat handle; may be NULL.
/// @return Borrowed Locale handle, or NULL when absent.
void *rt_list_format_get_locale(void *self);

/// @brief Join with conjunction ("A, B, and C" style).
/// @param self Valid ListFormat handle.
/// @param items Runtime List of strings; may be NULL.
/// @return Fresh formatted string, empty for NULL/empty input.
rt_string rt_list_format_and(void *self, void *items);
/// @brief Join with disjunction ("A, B, or C").
/// @param self Valid ListFormat handle.
/// @param items Runtime List of strings; may be NULL.
/// @return Fresh formatted string, empty for NULL/empty input.
rt_string rt_list_format_or(void *self, void *items);
/// @brief Plain unit-style join (no conjunction, locale's `unit` template).
/// @param self Valid ListFormat handle.
/// @param items Runtime List of strings; may be NULL.
/// @return Fresh formatted string, empty for NULL/empty input.
rt_string rt_list_format_unit(void *self, void *items);
/// @brief Short form — in v1 shares And's templates; future locales may
///        provide distinct ampersand-style short patterns.
/// @param self Valid ListFormat handle.
/// @param items Runtime List of strings; may be NULL.
/// @return Fresh formatted string using the conjunction template set.
rt_string rt_list_format_short(void *self, void *items);

#ifdef __cplusplus
}
#endif
