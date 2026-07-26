//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/localization/rt_locale_info.c
// Purpose: Implementation of Zanna.Localization.LocaleInfo. Each query pulls
//          the bound rt_locale_data_t via rt_locale_get_data (which falls back
//          to the invariant record when the Locale has no bound data) and
//          returns a fresh rt_string with the appropriate field.
//
// Key invariants:
//   - Every return path yields a non-NULL rt_string; empty strings stand in
//     for missing fields (no null-vs-empty-string ambiguity).
//   - Matches the plan's "static utility class (LocaleInfo)" shape: no
//     instance state, no construction, just pure queries.
//
// Ownership/Lifetime:
//   - Returned strings are fresh allocations owned by the caller.
//
// Links: src/runtime/localization/rt_locale_info.h (interface),
//        src/runtime/localization/rt_locale.h (Locale handle accessor).
//
//===----------------------------------------------------------------------===//

#include "rt_locale_info.h"

#include "rt_internal.h"
#include "rt_locale.h"
#include "rt_locale_data.h"
#include "rt_string.h"

#include <stdint.h>
#include <string.h>

/// @brief Convert a C string to an rt_string, returning an empty string for NULL or "".
/// @details Centralises the null/empty guard so every accessor below has a
///          single, trivial return path.
/// @param s Pointer to a NUL-terminated C string, or NULL.
/// @return A fresh rt_string copy of @p s, or an empty rt_string when @p s is NULL or empty.
static rt_string loc_info_str(const char *s) {
    if (!s || !*s)
        return rt_string_from_bytes("", 0);
    return rt_string_from_bytes(s, strlen(s));
}

/// @brief Return the display name of @p locale, optionally formatted in @p in_locale.
/// @details Phase-1 implementation: @p in_locale is ignored and the locale's own
///          native display name is always used. Falls back to an empty string when
///          no bound data record provides `names.display`.
/// @param locale  Opaque Locale handle; may be NULL (falls back to invariant data).
/// @param in_locale Intended target language for the name string (currently unused).
/// @return A freshly allocated rt_string with the display name, or empty on failure.
rt_string rt_locale_info_display_name(void *locale, void *in_locale) {
    (void)in_locale; // Phase 1: only en-US is baked, so we always emit the
                     // native display name of the target locale.
    const rt_locale_data_t *d = rt_locale_get_data(locale);
    return loc_info_str(d->names.display);
}

/// @brief Return the language component name of @p locale (e.g., "English").
/// @details The language name is the locale's own native-language label for its
///          language subtag. @p in_locale is reserved for future localisation of
///          the label and is currently ignored.
/// @param locale    Opaque Locale handle; may be NULL.
/// @param in_locale Target language for the returned name (currently unused).
/// @return A freshly allocated rt_string with the language name, or empty on failure.
rt_string rt_locale_info_language_name(void *locale, void *in_locale) {
    (void)in_locale;
    const rt_locale_data_t *d = rt_locale_get_data(locale);
    return loc_info_str(d->names.language);
}

/// @brief Return the region/country component name of @p locale (e.g., "United States").
/// @details Maps to the `names.region` field of the bound locale data record.
///          @p in_locale is reserved for future localisation and is currently ignored.
/// @param locale    Opaque Locale handle; may be NULL.
/// @param in_locale Target language for the returned name (currently unused).
/// @return A freshly allocated rt_string with the region name, or empty on failure.
rt_string rt_locale_info_region_name(void *locale, void *in_locale) {
    (void)in_locale;
    const rt_locale_data_t *d = rt_locale_get_data(locale);
    return loc_info_str(d->names.region);
}

/// @brief Return the text-direction string for @p locale ("ltr" or "rtl").
/// @details Reads `text_direction` from the bound locale data record. The canonical
///          values are the CLDR strings "ltr" and "rtl". Callers requiring a boolean
///          should prefer `rt_locale_info_is_rtl`.
/// @param locale Opaque Locale handle; may be NULL.
/// @return A freshly allocated rt_string ("ltr" / "rtl"), or empty when unknown.
rt_string rt_locale_info_text_direction(void *locale) {
    const rt_locale_data_t *d = rt_locale_get_data(locale);
    return loc_info_str(d->text_direction);
}

/// @brief Return the first day of the week for @p locale as a 0-based index.
/// @details 0 = Sunday … 6 = Saturday, matching the locale-data schema and the
///          months/days array convention (en-US default is 0/Sunday).
///          The value is read from `first_day_of_week` in the bound locale data.
/// @param locale Opaque Locale handle; may be NULL (invariant data is used).
/// @return First day index (0–6).
int64_t rt_locale_info_first_day_of_week(void *locale) {
    const rt_locale_data_t *d = rt_locale_get_data(locale);
    return (int64_t)d->first_day_of_week;
}

/// @brief Return 1 if @p locale uses right-to-left text layout, 0 otherwise.
/// @details Compares `text_direction` to the literal string "rtl".  This is a
///          convenience predicate that avoids string comparisons in Zia callers.
/// @param locale Opaque Locale handle; may be NULL.
/// @return 1 when the locale's text direction is RTL; 0 for LTR or unknown.
int8_t rt_locale_info_is_rtl(void *locale) {
    const rt_locale_data_t *d = rt_locale_get_data(locale);
    return (int8_t)(strcmp(d->text_direction, "rtl") == 0 ? 1 : 0);
}

/// @brief Return the measurement system code for @p locale (e.g., "metric" or "us").
/// @details Maps to the `measurement` field of the bound locale data. Useful for
///          selecting unit-formatting rules (km vs. mi, kg vs. lb).
/// @param locale Opaque Locale handle; may be NULL.
/// @return A freshly allocated rt_string with the measurement system, or empty when unknown.
rt_string rt_locale_info_measurement(void *locale) {
    const rt_locale_data_t *d = rt_locale_get_data(locale);
    return loc_info_str(d->measurement);
}

/// @brief Return the default ISO 4217 currency code for @p locale (e.g., "USD").
/// @details Reads `currency.default_code` from the bound locale data. The code is
///          a 3-letter uppercase string per the ISO 4217 standard.
/// @param locale Opaque Locale handle; may be NULL.
/// @return A freshly allocated rt_string with the 3-letter currency code, or empty when unknown.
rt_string rt_locale_info_currency(void *locale) {
    const rt_locale_data_t *d = rt_locale_get_data(locale);
    return loc_info_str(d->currency.default_code);
}
