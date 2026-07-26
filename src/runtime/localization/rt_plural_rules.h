//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/localization/rt_plural_rules.h
// Purpose: Public C API for Zanna.Localization.PluralRules — CLDR-style
//          plural-category selection for cardinal and ordinal numeric forms.
//          Each instance is keyed on a Locale whose rt_locale_data_t carries
//          rule AST chains populated either by the baked invariant record or
//          by LocaleManager's JSON loader.
//
// Key invariants:
//   - A PluralRules instance holds an opaque Locale handle reference
//     (strong, via rt_heap_retain) plus a retained reference to the captured
//     locale-data record. LocaleManager refuses to unload a record while a
//     live PluralRules instance retains it.
//   - Category lookup walks the rule chain in array order; the first
//     matching rule wins. Valid chains conventionally end in RT_PRN_TRUE;
//     empty or non-matching chains conservatively return "other".
//   - Rule evaluation is pure and thread-safe: no shared mutable state.
//
// Ownership/Lifetime:
//   - Instances are heap-allocated via rt_obj_new_i64; GC-managed.
//   - Category methods return owned runtime strings, Categories returns a
//     newly allocated List, and category-name literals have process lifetime.
//
// Links: src/runtime/localization/rt_plural_rules.c (implementation),
//        src/runtime/localization/rt_locale_data.h (rule AST storage),
//        docs/zannalib/localization/messages.md (end-user doc).
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Declares locale-bound CLDR plural-category selection services.
 * @details Exposes managed PluralRules construction, real and exact-integer
 * cardinal selection, ordinal selection, category enumeration, direct
 * locale-data evaluators, and canonical category-name mapping.
 */

#pragma once

#include "rt.hpp"
#include "rt_locale_data.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//===----------------------------------------------------------------------===//
// Public class surface
//===----------------------------------------------------------------------===//

/// @brief Construct PluralRules for the given Locale.
/// @details Captures the locale's data pointer; subsequent category queries
///          touch only the captured record. NULL locale => invariant rules.
/// @param locale Locale handle retained by the instance, or NULL.
/// @return Newly allocated PluralRules handle, or NULL after allocation
///         failure if the runtime trap hook returns.
void *rt_plural_rules_for_locale(void *locale);

/// @brief Select the cardinal category for a real-valued input.
/// @details Computes CLDR operand variables (n, i, v, f, t) from @p n and
///          evaluates the locale's cardinal rule chain. Returns a freshly
///          allocated string from the category set {"zero", "one", "two",
///          "few", "many", "other"}.
/// @param self PluralRules handle, or NULL.
/// @param n Real value to classify; non-finite values select `"other"`.
/// @return Newly allocated canonical category string.
rt_string rt_plural_rules_cardinal(void *self, double n);

/// @brief Select the cardinal category for an integer input.
/// @param self PluralRules handle, or NULL.
/// @param n Signed integer classified with exact magnitude arithmetic.
/// @return Newly allocated canonical category string.
rt_string rt_plural_rules_cardinal_int(void *self, int64_t n);

/// @brief Select the ordinal category for an integer input.
/// @details Ordinal rules depend on positional suffix conventions (1st,
///          2nd, 3rd, 4th in English). Rule AST uses the `n` variable
///          (absolute value) to model these.
/// @param self PluralRules handle, or NULL.
/// @param n Signed integer classified with exact magnitude arithmetic.
/// @return Newly allocated canonical category string.
rt_string rt_plural_rules_ordinal(void *self, int64_t n);

/// @brief Return the full set of distinct categories used by this locale.
/// @details Equivalent to `{ category : rule in (cardinal ∪ ordinal) }`
///          with stable insertion-order preservation.
/// @param self PluralRules handle, or NULL.
/// @return Newly allocated List of owned category strings, empty for NULL
///         @p self; possibly NULL if list allocation fails.
void *rt_plural_rules_categories(void *self); // List<str>

//===----------------------------------------------------------------------===//
// Internal helpers (shared with RelativeTimeFormat, MessageBundle in later
// phases; also used by RTPluralRulesTests to bypass the class construction).
//===----------------------------------------------------------------------===//

/// @brief Evaluate the cardinal chain on a rt_locale_data_t directly.
/// @param data Borrowed locale-data record, or NULL.
/// @param n Real value to classify.
/// @return First matching category, or `other` for NULL data, non-finite
///         input, or no match.
rt_plural_category_t rt_plural_rules_select_cardinal(const rt_locale_data_t *data, double n);

/// @brief Evaluate the cardinal chain with pure-integer operands.
/// @param data Borrowed locale-data record, or NULL.
/// @param n Signed integer to classify exactly.
/// @return First matching category, or `other` for NULL data or no match.
rt_plural_category_t rt_plural_rules_select_cardinal_int(const rt_locale_data_t *data, int64_t n);

/// @brief Evaluate the ordinal chain on a rt_locale_data_t.
/// @param data Borrowed locale-data record, or NULL.
/// @param n Signed integer to classify exactly.
/// @return First matching category, or `other` for NULL data or no match.
rt_plural_category_t rt_plural_rules_select_ordinal(const rt_locale_data_t *data, int64_t n);

/// @brief Convert a category enum to its canonical string name.
/// @param cat Category value to name.
/// @return Process-lifetime canonical keyword; unknown values map to `"other"`.
const char *rt_plural_rules_category_name(rt_plural_category_t cat);

#ifdef __cplusplus
}
#endif
