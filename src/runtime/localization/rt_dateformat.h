//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/localization/rt_dateformat.h
// Purpose: Public C API for Zanna.Localization.DateFormat — locale-aware
//          date and time formatting via CLDR pattern letters. The bound
//          Locale's rt_locale_data_t supplies month/day name tables,
//          AM/PM tokens, and the short/medium/long/full pattern templates.
//
// Key invariants:
//   - DateTime inputs are Unix timestamps (int64 seconds since epoch), the
//     same representation used by rt_datetime_* component accessors; rendered
//     fields use the host's local civil time.
//   - Custom patterns use CLDR letter conventions (y/M/d/E/H/h/m/s/a) plus
//     quoted literals. Unsupported letters trap with a clear diagnostic.
//   - MonthName / DayName methods accept a 1-based month (1-12) or a
//     0-based weekday (0=Sunday..6=Saturday) per the rest of the runtime's
//     calendar conventions.
//
// Ownership/Lifetime:
//   - Handles are rt_obj_new_i64-allocated; GC-managed.
//   - Each formatter retains its Locale handle and immutable data snapshot.
//   - GetLocale is borrowed; every formatting/name result is a fresh string.
//
// Links: src/runtime/localization/rt_dateformat.c (class methods),
//        src/runtime/localization/rt_dateformat_patterns.c (emit engine),
//        src/runtime/core/rt_datetime.h (component accessors),
//        docs/zannalib/localization/formatting.md (user documentation).
//
//===----------------------------------------------------------------------===//
#pragma once

#include "rt.hpp"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//===----------------------------------------------------------------------===//
// Constructors
//===----------------------------------------------------------------------===//

/// @brief Create a date formatter bound to the process's current locale.
/// @return Fresh GC-managed DateFormat, or NULL after an allocation trap.
void *rt_dateformat_new(void);
/// @brief Create a date formatter bound to the given @p locale handle.
/// @param locale Locale retained by the result; may be NULL for invariant data.
/// @return Fresh GC-managed DateFormat, or NULL after an allocation trap.
void *rt_dateformat_for_locale(void *locale);

//===----------------------------------------------------------------------===//
// Properties
//===----------------------------------------------------------------------===//

/// @brief Return the Locale handle this formatter was built with (borrowed).
/// @param self Valid DateFormat handle; may be NULL.
/// @return Borrowed Locale handle, or NULL when absent.
void *rt_dateformat_get_locale(void *self);

//===----------------------------------------------------------------------===//
// Canonical style methods (take a Unix timestamp i64)
//===----------------------------------------------------------------------===//

/// @brief Format the date of @p timestamp in the locale's short style.
/// @param self Valid DateFormat handle.
/// @param timestamp Unix seconds rendered in local civil time.
/// @return Fresh formatted string, or a fresh empty string on failure.
rt_string rt_dateformat_short(void *self, int64_t timestamp);
/// @brief Format the date of @p timestamp in the locale's medium style.
/// @param self Valid DateFormat handle.
/// @param timestamp Unix seconds rendered in local civil time.
/// @return Fresh formatted string, or a fresh empty string on failure.
rt_string rt_dateformat_medium(void *self, int64_t timestamp);
/// @brief Format the date of @p timestamp in the locale's long style.
/// @param self Valid DateFormat handle.
/// @param timestamp Unix seconds rendered in local civil time.
/// @return Fresh formatted string, or a fresh empty string on failure.
rt_string rt_dateformat_long(void *self, int64_t timestamp);
/// @brief Format the date of @p timestamp in the locale's full style.
/// @param self Valid DateFormat handle.
/// @param timestamp Unix seconds rendered in local civil time.
/// @return Fresh formatted string, or a fresh empty string on failure.
rt_string rt_dateformat_full(void *self, int64_t timestamp);

/// @brief Format the time-of-day of @p timestamp in the short style.
/// @param self Valid DateFormat handle.
/// @param timestamp Unix seconds rendered in local civil time.
/// @return Fresh formatted string, or a fresh empty string on failure.
rt_string rt_dateformat_time_short(void *self, int64_t timestamp);
/// @brief Format the time-of-day of @p timestamp in the medium style.
/// @param self Valid DateFormat handle.
/// @param timestamp Unix seconds rendered in local civil time.
/// @return Fresh formatted string, or a fresh empty string on failure.
rt_string rt_dateformat_time_medium(void *self, int64_t timestamp);

/// @brief Format both date and time of @p timestamp in the short style.
/// @details Falls back to short date, one space, and short time when no
///          explicit combined pattern is configured.
/// @param self Valid DateFormat handle.
/// @param timestamp Unix seconds rendered in local civil time.
/// @return Fresh formatted string, or a fresh empty string on failure.
rt_string rt_dateformat_datetime_short(void *self, int64_t timestamp);
/// @brief Format both date and time of @p timestamp in the medium style.
/// @details Falls back to medium date, one space, and medium time when no
///          explicit combined pattern is configured.
/// @param self Valid DateFormat handle.
/// @param timestamp Unix seconds rendered in local civil time.
/// @return Fresh formatted string, or a fresh empty string on failure.
rt_string rt_dateformat_datetime_medium(void *self, int64_t timestamp);

/// @brief Format @p timestamp using a custom CLDR pattern.
/// @details Supported letters: y M d E H h m s a. Quoted literals via '.
///          Unsupported letters trap. Pattern length is capped at 256 bytes.
/// @param self Valid DateFormat handle.
/// @param timestamp Unix seconds rendered in local civil time.
/// @param pattern Runtime string containing the complete pattern.
/// @return Fresh formatted string, or a fresh empty string on failure.
rt_string rt_dateformat_custom(void *self, int64_t timestamp, rt_string pattern);

/// @brief Format a DateOnly (@p dateonly is an rt_dateonly_t handle) using
///        a named style ("short"/"medium"/"long"/"full").
/// @details A NULL style selects medium. Date components are interpreted at
///          local midnight before formatting.
/// @param self Valid DateFormat handle.
/// @param dateonly Valid DateOnly handle.
/// @param style Optional lowercase style name.
/// @return Fresh formatted string, or a fresh empty string on failure.
rt_string rt_dateformat_date_only(void *self, void *dateonly, rt_string style);

/// @brief Get the month name for month @p month (1-12). @p abbreviated picks
///        between wide (false) and abbreviated (true) forms.
/// @param self Valid DateFormat handle.
/// @param month One-based month number.
/// @param abbreviated Nonzero for abbreviated form, zero for wide form.
/// @return Fresh localized name, or a fresh empty string on missing data/failure.
rt_string rt_dateformat_month_name(void *self, int64_t month, int8_t abbreviated);

/// @brief Get the weekday name for @p dow (0=Sunday..6=Saturday).
/// @param self Valid DateFormat handle.
/// @param dow Zero-based Sunday-first weekday index.
/// @param abbreviated Nonzero for abbreviated form, zero for wide form.
/// @return Fresh localized name, or a fresh empty string on missing data/failure.
rt_string rt_dateformat_day_name(void *self, int64_t dow, int8_t abbreviated);

/// @brief Get the AM/PM token for the given boolean (1 = PM, 0 = AM).
/// @param self Valid DateFormat handle.
/// @param is_pm Nonzero for PM, zero for AM.
/// @return Fresh localized token, falling back to ASCII AM/PM; empty for NULL self.
rt_string rt_dateformat_am_pm(void *self, int8_t is_pm);

#ifdef __cplusplus
}
#endif
