//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/localization/rt_locale_info.h
// Purpose: Static utility surface for Zanna.Localization.LocaleInfo — queries
//          about a Locale's display name, text direction, first day of week,
//          measurement system, and default currency. All queries read from
//          the Locale's bound rt_locale_data_t; NULL locales and locales
//          without bound data fall through to the invariant locale's values.
//
// Key invariants:
//   - Every method tolerates NULL locale inputs without trapping; only
//     internal corruption would cause a trap here.
//   - Display-name queries support an optional `inLocale` parameter so
//     future implementations can localize labels for the caller. Phase 1
//     ignores it and always emits the target locale's native stored name.
//
// Ownership/Lifetime:
//   - Returned rt_string values are fresh allocations owned by the caller.
//
// Links: src/runtime/localization/rt_locale_info.c (implementation),
//        src/runtime/localization/rt_locale.h (Locale handle type).
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Declares stateless LocaleInfo metadata queries.
 * @details Exposes display-name, direction, calendar, measurement-system, and
 * default-currency lookups with invariant fallback and fresh-string ownership.
 */

#pragma once

#include "rt.hpp"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Combined display name for the locale (e.g. "English (United States)").
/// @details @p in_locale is reserved for future label localization and is currently ignored.
/// @param locale Locale to describe; NULL selects invariant locale data.
/// @param in_locale Intended display-language Locale; currently unused.
/// @return Fresh display-name string, or a fresh empty string when missing.
rt_string rt_locale_info_display_name(void *locale, void *in_locale);

/// @brief Native-language display name (e.g. "English", "français").
/// @details @p in_locale is reserved for future label localization and is currently ignored.
/// @param locale Locale whose language name is requested; NULL selects invariant data.
/// @param in_locale Intended display-language Locale; currently unused.
/// @return Fresh language-name string, or a fresh empty string when missing.
rt_string rt_locale_info_language_name(void *locale, void *in_locale);

/// @brief Native region name (e.g. "United States", "France").
/// @details @p in_locale is reserved for future label localization and is currently ignored.
/// @param locale Locale whose region name is requested; NULL selects invariant data.
/// @param in_locale Intended display-language Locale; currently unused.
/// @return Fresh region-name string, or a fresh empty string when missing.
rt_string rt_locale_info_region_name(void *locale, void *in_locale);

/// @brief Dominant text direction: "ltr" or "rtl".
/// @param locale Locale to query; NULL selects invariant data.
/// @return Fresh direction string, or a fresh empty string when missing.
rt_string rt_locale_info_text_direction(void *locale);

/// @brief First day of the week (0 = Sunday .. 6 = Saturday).
/// @param locale Locale to query; NULL selects invariant data.
/// @return Sunday-first weekday index from the locale-data record.
int64_t rt_locale_info_first_day_of_week(void *locale);

/// @brief Test whether the locale-data direction token is exactly "rtl".
/// @param locale Locale to query; NULL selects invariant data.
/// @return 1 for RTL, otherwise 0.
int8_t rt_locale_info_is_rtl(void *locale);

/// @brief Measurement system code: "metric", "us", or "uk".
/// @param locale Locale to query; NULL selects invariant data.
/// @return Fresh measurement-code string, or a fresh empty string when missing.
rt_string rt_locale_info_measurement(void *locale);

/// @brief Default ISO-4217 currency code for the region (e.g. "USD").
/// @param locale Locale to query; NULL selects invariant data.
/// @return Fresh currency-code string, or a fresh empty string when missing.
rt_string rt_locale_info_currency(void *locale);

#ifdef __cplusplus
}
#endif
