//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/localization/rt_locale_data_en_us.c
// Purpose: Statically baked rt_locale_data_t record for en-US. Provides the
//          out-of-the-box default locale for Zanna.Localization.*, used both
//          as the invariant-fallback target when system detection fails and
//          as the reference fixture that validates the JSON-loader path in
//          tests (baked and JSON-loaded en-US should be structurally equal).
//
// Key invariants:
//   - Every field pointer is a string literal with static storage duration;
//     semantic locale content is immutable and shareable across threads.
//   - `formatter_refs` is the sole mutable field and is updated atomically by
//     LocaleManager when formatters capture/release this record.
//   - arena is NULL; LocaleManager treats this as "do not free on Unload".
//   - Plural rules match CLDR 44 cardinal/ordinal for en: cardinal (one:
//     i=1 and v=0, other), ordinal (one: n mod 10 = 1 and n mod 100 != 11,
//     two: n mod 10 = 2 and n mod 100 != 12, few: n mod 10 = 3 and
//     n mod 100 != 13, other).
//
// Ownership/Lifetime:
//   - Process-lifetime; never freed. Address-stable across queries.
//
// Links: src/runtime/localization/rt_locale_data.h (struct layout).
//
//===----------------------------------------------------------------------===//

#include "rt_locale_data.h"

#include <stddef.h>

//===----------------------------------------------------------------------===//
// Calendar name tables
//===----------------------------------------------------------------------===//

static const char *const g_en_us_months_wide[12] = {
    "January",
    "February",
    "March",
    "April",
    "May",
    "June",
    "July",
    "August",
    "September",
    "October",
    "November",
    "December",
};

static const char *const g_en_us_months_abbr[12] = {
    "Jan",
    "Feb",
    "Mar",
    "Apr",
    "May",
    "Jun",
    "Jul",
    "Aug",
    "Sep",
    "Oct",
    "Nov",
    "Dec",
};

static const char *const g_en_us_days_wide[7] = {
    "Sunday",
    "Monday",
    "Tuesday",
    "Wednesday",
    "Thursday",
    "Friday",
    "Saturday",
};

static const char *const g_en_us_days_abbr[7] = {
    "Sun",
    "Mon",
    "Tue",
    "Wed",
    "Thu",
    "Fri",
    "Sat",
};

//===----------------------------------------------------------------------===//
// Plural rule tree helpers — built as a small DAG of static structs. These
// structures are read-only after program start so concurrent evaluation is
// safe without locking.
//===----------------------------------------------------------------------===//

// Cardinal: one = { i = 1 and v = 0 }; other = true.
// We encode `v = 0` (count of visible fraction digits with trailing zeros,
// which for a pure integer is 0) and `i = 1` as two equality leaves AND'd
// together.

// Leaves
static rt_plural_rule_node_t g_card_var_i = {.kind = RT_PRN_VAR,
                                             .u = {.var = {.var = RT_PVAR_I, .mod = 0}}};
static rt_plural_rule_node_t g_card_int_1 = {.kind = RT_PRN_INT, .u = {.int_val = 1}};
static rt_plural_rule_node_t g_card_var_v = {.kind = RT_PRN_VAR,
                                             .u = {.var = {.var = RT_PVAR_V, .mod = 0}}};
static rt_plural_rule_node_t g_card_int_0 = {.kind = RT_PRN_INT, .u = {.int_val = 0}};

// i = 1
static rt_plural_rule_node_t g_card_eq_i_1 = {
    .kind = RT_PRN_EQ, .u = {.bin = {.l = &g_card_var_i, .r = &g_card_int_1}}};
// v = 0
static rt_plural_rule_node_t g_card_eq_v_0 = {
    .kind = RT_PRN_EQ, .u = {.bin = {.l = &g_card_var_v, .r = &g_card_int_0}}};
// (i = 1) and (v = 0)
static rt_plural_rule_node_t g_card_and = {
    .kind = RT_PRN_AND, .u = {.bin = {.l = &g_card_eq_i_1, .r = &g_card_eq_v_0}}};
// other / catch-all
static rt_plural_rule_node_t g_rule_true = {.kind = RT_PRN_TRUE, .u = {.int_val = 0}};

static const rt_plural_rule_entry_t g_en_us_cardinal[] = {
    {RT_PLURAL_ONE, &g_card_and},
    {RT_PLURAL_OTHER, &g_rule_true},
};

// Ordinal: one = { n mod 10 = 1 and n mod 100 != 11 }; two = { n mod 10 = 2 and
// n mod 100 != 12 }; few = { n mod 10 = 3 and n mod 100 != 13 }; other.

static rt_plural_rule_node_t g_ord_var_nmod10 = {.kind = RT_PRN_VAR,
                                                 .u = {.var = {.var = RT_PVAR_N, .mod = 10}}};
static rt_plural_rule_node_t g_ord_var_nmod100 = {.kind = RT_PRN_VAR,
                                                  .u = {.var = {.var = RT_PVAR_N, .mod = 100}}};

// Literal constants
static rt_plural_rule_node_t g_int_1 = {.kind = RT_PRN_INT, .u = {.int_val = 1}};
static rt_plural_rule_node_t g_int_2 = {.kind = RT_PRN_INT, .u = {.int_val = 2}};
static rt_plural_rule_node_t g_int_3 = {.kind = RT_PRN_INT, .u = {.int_val = 3}};
static rt_plural_rule_node_t g_int_11 = {.kind = RT_PRN_INT, .u = {.int_val = 11}};
static rt_plural_rule_node_t g_int_12 = {.kind = RT_PRN_INT, .u = {.int_val = 12}};
static rt_plural_rule_node_t g_int_13 = {.kind = RT_PRN_INT, .u = {.int_val = 13}};

// ordinal "one" clauses
static rt_plural_rule_node_t g_ord_eq_nmod10_1 = {
    .kind = RT_PRN_EQ, .u = {.bin = {.l = &g_ord_var_nmod10, .r = &g_int_1}}};
static rt_plural_rule_node_t g_ord_ne_nmod100_11 = {
    .kind = RT_PRN_NE, .u = {.bin = {.l = &g_ord_var_nmod100, .r = &g_int_11}}};
static rt_plural_rule_node_t g_ord_one = {
    .kind = RT_PRN_AND, .u = {.bin = {.l = &g_ord_eq_nmod10_1, .r = &g_ord_ne_nmod100_11}}};

// ordinal "two" clauses
static rt_plural_rule_node_t g_ord_eq_nmod10_2 = {
    .kind = RT_PRN_EQ, .u = {.bin = {.l = &g_ord_var_nmod10, .r = &g_int_2}}};
static rt_plural_rule_node_t g_ord_ne_nmod100_12 = {
    .kind = RT_PRN_NE, .u = {.bin = {.l = &g_ord_var_nmod100, .r = &g_int_12}}};
static rt_plural_rule_node_t g_ord_two = {
    .kind = RT_PRN_AND, .u = {.bin = {.l = &g_ord_eq_nmod10_2, .r = &g_ord_ne_nmod100_12}}};

// ordinal "few" clauses
static rt_plural_rule_node_t g_ord_eq_nmod10_3 = {
    .kind = RT_PRN_EQ, .u = {.bin = {.l = &g_ord_var_nmod10, .r = &g_int_3}}};
static rt_plural_rule_node_t g_ord_ne_nmod100_13 = {
    .kind = RT_PRN_NE, .u = {.bin = {.l = &g_ord_var_nmod100, .r = &g_int_13}}};
static rt_plural_rule_node_t g_ord_few = {
    .kind = RT_PRN_AND, .u = {.bin = {.l = &g_ord_eq_nmod10_3, .r = &g_ord_ne_nmod100_13}}};

static const rt_plural_rule_entry_t g_en_us_ordinal[] = {
    {RT_PLURAL_ONE, &g_ord_one},
    {RT_PLURAL_TWO, &g_ord_two},
    {RT_PLURAL_FEW, &g_ord_few},
    {RT_PLURAL_OTHER, &g_rule_true},
};

//===----------------------------------------------------------------------===//
// Top-level record
//===----------------------------------------------------------------------===//

static const rt_locale_data_t g_en_us_data = {
    .tag = "en-US",
    .names =
        {
            .language = "English",
            .region = "United States",
            .display = "English (United States)",
        },
    .text_direction = "ltr",
    .first_day_of_week = 0, // Sunday, per US calendar convention
    .measurement = "us",

    .numbers =
        {
            .decimal_sep = ".",
            .group_sep = ",",
            .group_size = 3,
            .secondary_group_size = 3,
            .minus = "-",
            .plus = "+",
            .percent = "%",
            .infinity = "\xE2\x88\x9E", // U+221E INFINITY
            .nan = "NaN",
            .exponent = "E",
            .digits = "0123456789",
        },

    .currency =
        {
            .default_code = "USD",
            .symbol = "$",
            .pattern_positive = "{s}{n}",
            .pattern_negative = "-{s}{n}",
            .fraction_digits = 2,
        },

    .dates =
        {
            .months_wide = g_en_us_months_wide,
            .months_abbr = g_en_us_months_abbr,
            .days_wide = g_en_us_days_wide,
            .days_abbr = g_en_us_days_abbr,
            .am = "AM",
            .pm = "PM",
            .patterns =
                {
                    .short_p = "M/d/yy",
                    .medium_p = "MMM d, yyyy",
                    .long_p = "MMMM d, yyyy",
                    .full_p = "EEEE, MMMM d, yyyy",
                    .time_short = "h:mm a",
                    .time_medium = "h:mm:ss a",
                    .datetime_short = "M/d/yy h:mm a",
                    .datetime_medium = "MMM d, yyyy h:mm:ss a",
                },
        },

    .reltime =
        {
            .now = "now",
            .past = "{n} {unit} ago",
            .future = "in {n} {unit}",
            .units =
                {
                    // 0: second
                    {.other = "seconds", .one = "second"},
                    // 1: minute
                    {.other = "minutes", .one = "minute"},
                    // 2: hour
                    {.other = "hours", .one = "hour"},
                    // 3: day
                    {.other = "days", .one = "day"},
                    // 4: week
                    {.other = "weeks", .one = "week"},
                    // 5: month
                    {.other = "months", .one = "month"},
                    // 6: year
                    {.other = "years", .one = "year"},
                },
            .short_past = "{n} {unit} ago",
            .short_future = "in {n} {unit}",
            .short_units =
                {
                    {.other = "sec", .one = "sec"},
                    {.other = "min", .one = "min"},
                    {.other = "hr", .one = "hr"},
                    {.other = "d", .one = "d"},
                    {.other = "wk", .one = "wk"},
                    {.other = "mo", .one = "mo"},
                    {.other = "yr", .one = "yr"},
                },
        },

    .plural_cardinal = g_en_us_cardinal,
    .cardinal_count = sizeof(g_en_us_cardinal) / sizeof(g_en_us_cardinal[0]),
    .plural_ordinal = g_en_us_ordinal,
    .ordinal_count = sizeof(g_en_us_ordinal) / sizeof(g_en_us_ordinal[0]),

    .list_format =
        {
            .and_p =
                {
                    .pair = "{0} and {1}",
                    .start = "{0}, {1}",
                    .middle = "{0}, {1}",
                    .end = "{0}, and {1}",
                },
            .or_p =
                {
                    .pair = "{0} or {1}",
                    .start = "{0}, {1}",
                    .middle = "{0}, {1}",
                    .end = "{0}, or {1}",
                },
            .unit_p =
                {
                    .pair = "{0} {1}",
                    .start = "{0} {1}",
                    .middle = "{0} {1}",
                    .end = "{0} {1}",
                },
        },

    .collation =
        {
            .strength = 3,
            .reorder = NULL,
            .reorder_len = 0,
        },

    .arena = NULL,
    .formatter_refs = 0,
};

/// @brief Return the statically-baked en-US locale data record.
/// @details The returned pointer refers to a process-lifetime static object.
///          The `arena` field is NULL (no heap allocation); `formatter_refs`
///          starts at zero and may be incremented by the locale-manager retain
///          path. Callers must not free or mutate the returned record.
/// @return Pointer to the global `g_en_us_data` record; never NULL.
const rt_locale_data_t *rt_locale_data_en_us(void) {
    return &g_en_us_data;
}
