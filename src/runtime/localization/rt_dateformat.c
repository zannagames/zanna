//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/localization/rt_dateformat.c
// Purpose: Implementation of Zanna.Localization.DateFormat. Delegates the
//          heavy lifting to rt_dateformat_patterns.c's emit engine; this
//          file wires the class lifecycle, resolves style-name patterns
//          from the bound Locale's table, and exposes the MonthName/
//          DayName/AmPm convenience queries.
//
// Key invariants:
//   - Each method captures the bound rt_locale_data_t pointer from the
//     Locale at construction; hot-path formatting never takes a lock.
//   - Style-name lookup (Short/Medium/Long/Full) pulls patterns from the
//     locale data's dates.patterns structure. Unknown style names trap.
//
// Ownership/Lifetime:
//   - Instances are rt_obj_new_i64-allocated; GC-managed.
//   - Each instance retains both its Locale handle and the immutable locale-
//     data snapshot captured at construction.
//
// Links: src/runtime/localization/rt_dateformat.h (interface),
//        src/runtime/localization/rt_dateformat_patterns.c (emitter).
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Implements the managed DateFormat facade and locale style selection.
 * @details Captures an immutable locale-data snapshot, resolves canonical and
 * combined patterns, delegates checked pattern emission, formats DateOnly
 * values, and exposes localized month, weekday, and day-period names.
 */

#include "rt_dateformat.h"

#include "rt_datetime.h"
#include "rt_heap.h"
#include "rt_internal.h"
#include "rt_locale.h"
#include "rt_locale_data.h"
#include "rt_locale_manager.h"
#include "rt_object.h"
#include "rt_string.h"
#include "rt_string_builder.h"
#include "rt_trap.h"

#include <stdint.h>
#include <string.h>

//===----------------------------------------------------------------------===//
// Forward declaration for the pattern emitter (implemented in
// rt_dateformat_patterns.c).
//===----------------------------------------------------------------------===//
/// @copydoc rt_dateformat_emit_pattern()
void rt_dateformat_emit_pattern(rt_string_builder *sb,
                                int64_t timestamp,
                                const char *pattern,
                                size_t pattern_len,
                                const rt_locale_data_t *data);
/// @copydoc rt_dateformat_emit_pattern_checked()
int rt_dateformat_emit_pattern_checked(rt_string_builder *sb,
                                       int64_t timestamp,
                                       const char *pattern,
                                       size_t pattern_len,
                                       const rt_locale_data_t *data);

//===----------------------------------------------------------------------===//
// Instance struct
//===----------------------------------------------------------------------===//

/** GC payload retaining the Locale and data snapshot for one DateFormat. */
typedef struct rt_dateformat_inst {
    void *locale;                 ///< Retained Locale handle, if supplied.
    const rt_locale_data_t *data; ///< Retained immutable formatting data.
} rt_dateformat_inst_t;

/// @brief Unchecked cast of an opaque handle to the DateFormat instance.
/// @param obj Valid DateFormat payload.
/// @return The same pointer interpreted as @ref rt_dateformat_inst_t.
static rt_dateformat_inst_t *as_fmt(void *obj) {
    return (rt_dateformat_inst_t *)obj;
}

/// @brief Drop one GC reference to @p obj and free it if the count hit zero.
/// @param obj Owned runtime-managed handle reference; NULL is a no-op.
static void df_release_handle(void *obj) {
    if (obj && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

/// @brief GC finalizer: release the format's locale data and locale handle.
/// @details The data snapshot and Locale were retained independently during
///          construction and are each released exactly once.
/// @param obj DateFormat payload being finalized; NULL is a no-op.
static void df_finalizer(void *obj) {
    rt_dateformat_inst_t *fmt = (rt_dateformat_inst_t *)obj;
    if (!fmt)
        return;
    rt_locale_manager_release_data(fmt->data);
    df_release_handle(fmt->locale);
    fmt->locale = NULL;
    fmt->data = NULL;
}

//===----------------------------------------------------------------------===//
// Constructors
//===----------------------------------------------------------------------===//

/// @brief Allocate and initialize a GC-managed DateFormat for @p locale.
/// @details Retains the locale handle + its data. Traps on allocation failure;
///          installs @ref df_finalizer.
/// @param locale Optional Locale handle to retain; NULL selects invariant fallback data.
/// @return Fresh GC-managed DateFormat, or NULL after an allocation trap.
static void *df_alloc(void *locale) {
    rt_dateformat_inst_t *fmt =
        (rt_dateformat_inst_t *)rt_obj_new_i64(0, (int64_t)sizeof(rt_dateformat_inst_t));
    if (!fmt) {
        rt_trap("Zanna.Localization.DateFormat: allocation failed");
        return NULL;
    }
    fmt->locale = locale;
    if (fmt->locale)
        rt_heap_retain(fmt->locale);
    fmt->data = rt_locale_get_data(locale);
    rt_locale_manager_retain_data(fmt->data);
    rt_obj_set_finalizer(fmt, df_finalizer);
    return fmt;
}

/// @brief Create a DateFormat bound to the process's current Locale snapshot.
/// @details The locale manager's temporary reference is released after the
///          formatter acquires its independent Locale and data references.
/// @return Fresh GC-managed DateFormat, or NULL after an allocation trap.
void *rt_dateformat_new(void) {
    void *current = rt_locale_manager_current();
    void *fmt = df_alloc(current);
    df_release_handle(current);
    return fmt;
}

/// @brief Create a DateFormat bound to a specified Locale.
/// @param locale Locale handle retained by the result; may be NULL for invariant data.
/// @return Fresh GC-managed DateFormat, or NULL after an allocation trap.
void *rt_dateformat_for_locale(void *locale) {
    return df_alloc(locale);
}

/// @brief Get the Locale handle retained by a DateFormat.
/// @param self Valid DateFormat handle; may be NULL.
/// @return Borrowed Locale handle, or NULL when absent.
void *rt_dateformat_get_locale(void *self) {
    return self ? as_fmt(self)->locale : NULL;
}

//===----------------------------------------------------------------------===//
// Shared style renderer
//===----------------------------------------------------------------------===//

/// @brief Render @p timestamp through a CLDR @p pattern using the format's
///        locale data. Shared backend for the named-style entry points.
/// @details The emitter extracts local-time components and applies localized
///          names, AM/PM tokens, and digit glyphs. Emission failure discards
///          the builder and returns an empty result.
/// @param self Valid DateFormat handle.
/// @param timestamp Unix seconds interpreted through local civil-time accessors.
/// @param pattern NUL-terminated supported CLDR-like pattern.
/// @return Fresh formatted runtime string, or a fresh empty string on failure.
static rt_string render_with_pattern(void *self, int64_t timestamp, const char *pattern) {
    if (!self || !pattern)
        return rt_string_from_bytes("", 0);
    rt_dateformat_inst_t *fmt = as_fmt(self);
    rt_string_builder sb;
    rt_sb_init(&sb);
    if (!rt_dateformat_emit_pattern_checked(&sb, timestamp, pattern, strlen(pattern), fmt->data)) {
        rt_sb_free(&sb);
        return rt_string_from_bytes("", 0);
    }
    rt_string r = rt_string_from_bytes(sb.data, sb.len);
    rt_sb_free(&sb);
    return r;
}

//===----------------------------------------------------------------------===//
// Canonical style methods
//===----------------------------------------------------------------------===//

/// @brief Format a timestamp with the Locale's short date pattern.
/// @param self Valid DateFormat handle.
/// @param ts Unix timestamp in seconds, rendered in local civil time.
/// @return Fresh formatted string, or a fresh empty string on failure.
rt_string rt_dateformat_short(void *self, int64_t ts) {
    if (!self)
        return rt_string_from_bytes("", 0);
    return render_with_pattern(self, ts, as_fmt(self)->data->dates.patterns.short_p);
}

/// @brief Format a timestamp with the Locale's medium date pattern.
/// @param self Valid DateFormat handle.
/// @param ts Unix timestamp in seconds, rendered in local civil time.
/// @return Fresh formatted string, or a fresh empty string on failure.
rt_string rt_dateformat_medium(void *self, int64_t ts) {
    if (!self)
        return rt_string_from_bytes("", 0);
    return render_with_pattern(self, ts, as_fmt(self)->data->dates.patterns.medium_p);
}

/// @brief Format a timestamp with the Locale's long date pattern.
/// @param self Valid DateFormat handle.
/// @param ts Unix timestamp in seconds, rendered in local civil time.
/// @return Fresh formatted string, or a fresh empty string on failure.
rt_string rt_dateformat_long(void *self, int64_t ts) {
    if (!self)
        return rt_string_from_bytes("", 0);
    return render_with_pattern(self, ts, as_fmt(self)->data->dates.patterns.long_p);
}

/// @brief Format a timestamp with the Locale's full date pattern.
/// @param self Valid DateFormat handle.
/// @param ts Unix timestamp in seconds, rendered in local civil time.
/// @return Fresh formatted string, or a fresh empty string on failure.
rt_string rt_dateformat_full(void *self, int64_t ts) {
    if (!self)
        return rt_string_from_bytes("", 0);
    return render_with_pattern(self, ts, as_fmt(self)->data->dates.patterns.full_p);
}

/// @brief Format a timestamp with the Locale's short time pattern.
/// @param self Valid DateFormat handle.
/// @param ts Unix timestamp in seconds, rendered in local civil time.
/// @return Fresh formatted string, or a fresh empty string on failure.
rt_string rt_dateformat_time_short(void *self, int64_t ts) {
    if (!self)
        return rt_string_from_bytes("", 0);
    return render_with_pattern(self, ts, as_fmt(self)->data->dates.patterns.time_short);
}

/// @brief Format a timestamp with the Locale's medium time pattern.
/// @param self Valid DateFormat handle.
/// @param ts Unix timestamp in seconds, rendered in local civil time.
/// @return Fresh formatted string, or a fresh empty string on failure.
rt_string rt_dateformat_time_medium(void *self, int64_t ts) {
    if (!self)
        return rt_string_from_bytes("", 0);
    return render_with_pattern(self, ts, as_fmt(self)->data->dates.patterns.time_medium);
}

/// @brief Format a timestamp with the Locale's short combined date/time style.
/// @details Uses an explicit combined pattern when present; otherwise emits
///          the short date pattern, one ASCII space, and the short time pattern.
/// @param self Valid DateFormat handle.
/// @param ts Unix timestamp in seconds, rendered in local civil time.
/// @return Fresh formatted string, or a fresh empty string on failure.
rt_string rt_dateformat_datetime_short(void *self, int64_t ts) {
    if (!self)
        return rt_string_from_bytes("", 0);
    rt_dateformat_inst_t *fmt = as_fmt(self);
    if (fmt->data->dates.patterns.datetime_short && *fmt->data->dates.patterns.datetime_short)
        return render_with_pattern(self, ts, fmt->data->dates.patterns.datetime_short);
    rt_string_builder sb;
    rt_sb_init(&sb);
    if (!rt_dateformat_emit_pattern_checked(&sb,
                                            ts,
                                            fmt->data->dates.patterns.short_p,
                                            strlen(fmt->data->dates.patterns.short_p),
                                            fmt->data) ||
        rt_sb_append_cstr(&sb, " ") != RT_SB_OK ||
        !rt_dateformat_emit_pattern_checked(&sb,
                                            ts,
                                            fmt->data->dates.patterns.time_short,
                                            strlen(fmt->data->dates.patterns.time_short),
                                            fmt->data)) {
        rt_sb_free(&sb);
        return rt_string_from_bytes("", 0);
    }
    rt_string r = rt_string_from_bytes(sb.data, sb.len);
    rt_sb_free(&sb);
    return r;
}

/// @brief Format a timestamp with the Locale's medium combined date/time style.
/// @details Uses an explicit combined pattern when present; otherwise emits
///          the medium date pattern, one ASCII space, and the medium time pattern.
/// @param self Valid DateFormat handle.
/// @param ts Unix timestamp in seconds, rendered in local civil time.
/// @return Fresh formatted string, or a fresh empty string on failure.
rt_string rt_dateformat_datetime_medium(void *self, int64_t ts) {
    if (!self)
        return rt_string_from_bytes("", 0);
    rt_dateformat_inst_t *fmt = as_fmt(self);
    if (fmt->data->dates.patterns.datetime_medium && *fmt->data->dates.patterns.datetime_medium)
        return render_with_pattern(self, ts, fmt->data->dates.patterns.datetime_medium);
    rt_string_builder sb;
    rt_sb_init(&sb);
    if (!rt_dateformat_emit_pattern_checked(&sb,
                                            ts,
                                            fmt->data->dates.patterns.medium_p,
                                            strlen(fmt->data->dates.patterns.medium_p),
                                            fmt->data) ||
        rt_sb_append_cstr(&sb, " ") != RT_SB_OK ||
        !rt_dateformat_emit_pattern_checked(&sb,
                                            ts,
                                            fmt->data->dates.patterns.time_medium,
                                            strlen(fmt->data->dates.patterns.time_medium),
                                            fmt->data)) {
        rt_sb_free(&sb);
        return rt_string_from_bytes("", 0);
    }
    rt_string r = rt_string_from_bytes(sb.data, sb.len);
    rt_sb_free(&sb);
    return r;
}

/// @brief Format a timestamp with a caller-provided CLDR-like pattern.
/// @details Patterns longer than 256 bytes, unsupported letters, unterminated
///          quotes, and builder failures trap through the emitter and return
///          a fresh empty string. NULL self or pattern traps immediately.
/// @param self Valid DateFormat handle.
/// @param ts Unix timestamp in seconds, rendered in local civil time.
/// @param pattern Valid runtime string containing the complete bounded pattern.
/// @return Fresh formatted string, or a fresh empty string on failure.
rt_string rt_dateformat_custom(void *self, int64_t ts, rt_string pattern) {
    if (!self || !pattern) {
        rt_trap("Zanna.Localization.DateFormat: Custom requires non-null pattern");
        return rt_string_from_bytes("", 0);
    }
    rt_dateformat_inst_t *fmt = as_fmt(self);
    const char *cs = rt_string_cstr(pattern);
    int64_t len = rt_str_len(pattern);
    if (!cs || len < 0)
        return rt_string_from_bytes("", 0);
    rt_string_builder sb;
    rt_sb_init(&sb);
    if (!rt_dateformat_emit_pattern_checked(&sb, ts, cs, (size_t)len, fmt->data)) {
        rt_sb_free(&sb);
        return rt_string_from_bytes("", 0);
    }
    rt_string r = rt_string_from_bytes(sb.data, sb.len);
    rt_sb_free(&sb);
    return r;
}

//===----------------------------------------------------------------------===//
// DateOnly variant
//===----------------------------------------------------------------------===//

/// Forward-declare the accessors we need from rt_dateonly (which exposes
/// Year/Month/Day getters on an rt_dateonly_t handle). We don't include
/// rt_dateonly.h to avoid pulling the full runtime class in this TU;
/// extern declarations match its public signatures.
extern int64_t rt_dateonly_year(void *handle);
extern int64_t rt_dateonly_month(void *handle);
extern int64_t rt_dateonly_day(void *handle);

/// @brief Format a DateOnly handle through a named date style.
/// @details A NULL style selects medium; otherwise the exact lowercase names
///          `short`, `medium`, `long`, and `full` are accepted. Components are
///          converted through @ref rt_datetime_create at local midnight before
///          pattern emission. Unknown styles and NULL required handles trap.
/// @param self Valid DateFormat handle.
/// @param dateonly Valid DateOnly handle supplying year, month, and day.
/// @param style Optional lowercase style name; NULL selects medium.
/// @return Fresh formatted string, or a fresh empty string on failure.
rt_string rt_dateformat_date_only(void *self, void *dateonly, rt_string style) {
    if (!self || !dateonly) {
        rt_trap("Zanna.Localization.DateFormat: DateOnly requires non-null arguments");
        return rt_string_from_bytes("", 0);
    }
    rt_dateformat_inst_t *fmt = as_fmt(self);

    // Pick the pattern by style name, defaulting to medium.
    const char *pattern = fmt->data->dates.patterns.medium_p;
    const char *style_cs = style ? rt_string_cstr(style) : NULL;
    if (style_cs) {
        if (strcmp(style_cs, "medium") == 0) {
            /* Keep the default medium pattern. */
        } else if (strcmp(style_cs, "short") == 0)
            pattern = fmt->data->dates.patterns.short_p;
        else if (strcmp(style_cs, "long") == 0)
            pattern = fmt->data->dates.patterns.long_p;
        else if (strcmp(style_cs, "full") == 0)
            pattern = fmt->data->dates.patterns.full_p;
        else {
            rt_trap(
                "Zanna.Localization.DateFormat: unknown style (expected short/medium/long/full)");
            return rt_string_from_bytes("", 0);
        }
    }

    // Synthesize a Unix timestamp from DateOnly components at local midnight.
    // Convert date components to a midnight timestamp for pattern emission.
    int64_t y = rt_dateonly_year(dateonly);
    int64_t m = rt_dateonly_month(dateonly);
    int64_t d = rt_dateonly_day(dateonly);
    int64_t ts = rt_datetime_create(y, m, d, 0, 0, 0);

    rt_string_builder sb;
    rt_sb_init(&sb);
    if (!rt_dateformat_emit_pattern_checked(&sb, ts, pattern, strlen(pattern), fmt->data)) {
        rt_sb_free(&sb);
        return rt_string_from_bytes("", 0);
    }
    rt_string r = rt_string_from_bytes(sb.data, sb.len);
    rt_sb_free(&sb);
    return r;
}

//===----------------------------------------------------------------------===//
// Name queries
//===----------------------------------------------------------------------===//

/// @brief Copy a localized month name from the captured Locale data.
/// @param self Valid DateFormat handle.
/// @param month One-based month number from 1 through 12.
/// @param abbreviated Nonzero for abbreviated names, zero for wide names.
/// @return Fresh localized runtime string; missing table entries produce an
///         empty string, while invalid arguments trap and return empty.
rt_string rt_dateformat_month_name(void *self, int64_t month, int8_t abbreviated) {
    if (!self || month < 1 || month > 12) {
        rt_trap("Zanna.Localization.DateFormat: month out of range (1..12)");
        return rt_string_from_bytes("", 0);
    }
    const rt_locale_data_t *data = as_fmt(self)->data;
    const char *const *arr = abbreviated ? data->dates.months_abbr : data->dates.months_wide;
    const char *s = arr ? arr[month - 1] : NULL;
    if (!s)
        s = "";
    return rt_string_from_bytes(s, strlen(s));
}

/// @brief Copy a localized weekday name from the captured Locale data.
/// @param self Valid DateFormat handle.
/// @param dow Weekday index from 0 (Sunday) through 6 (Saturday).
/// @param abbreviated Nonzero for abbreviated names, zero for wide names.
/// @return Fresh localized runtime string; missing table entries produce an
///         empty string, while invalid arguments trap and return empty.
rt_string rt_dateformat_day_name(void *self, int64_t dow, int8_t abbreviated) {
    if (!self || dow < 0 || dow > 6) {
        rt_trap("Zanna.Localization.DateFormat: weekday out of range (0..6)");
        return rt_string_from_bytes("", 0);
    }
    const rt_locale_data_t *data = as_fmt(self)->data;
    const char *const *arr = abbreviated ? data->dates.days_abbr : data->dates.days_wide;
    const char *s = arr ? arr[dow] : NULL;
    if (!s)
        s = "";
    return rt_string_from_bytes(s, strlen(s));
}

/// @brief Copy the Locale's AM or PM day-period token.
/// @details Missing locale tokens fall back to the ASCII literals `AM` and `PM`.
/// @param self Valid DateFormat handle.
/// @param is_pm Nonzero to select PM, zero to select AM.
/// @return Fresh runtime string containing the selected token, or a fresh
///         empty string for NULL self.
rt_string rt_dateformat_am_pm(void *self, int8_t is_pm) {
    if (!self)
        return rt_string_from_bytes("", 0);
    const rt_locale_data_t *data = as_fmt(self)->data;
    const char *s = is_pm ? data->dates.pm : data->dates.am;
    if (!s)
        s = is_pm ? "PM" : "AM";
    return rt_string_from_bytes(s, strlen(s));
}
