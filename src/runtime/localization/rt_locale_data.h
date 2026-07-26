//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/localization/rt_locale_data.h
// Purpose: Internal record for per-locale data (number separators, currency
//          symbols, date names and patterns, relative-time strings, plural
//          rule trees, list-join templates, collation tailorings). Shared
//          between the C-baked en-US table (rt_locale_data_en_us.c) and the
//          (Phase 2+) JSON/ZPAK loader so both paths produce structurally
//          identical records consumable by NumberFormat, DateFormat, etc.
//
// Key invariants:
//   - Every pointer field is either a static immortal string literal (baked
//     locale) or an arena-allocated copy whose lifetime matches the struct's
//     `arena` field. Never mix ownership models within a single record.
//   - `arena == NULL` is the sentinel for a statically baked record and
//     signals "do not free on Unload".
//   - months_wide/months_abbr arrays carry exactly 12 entries. days_wide /
//     days_abbr carry exactly 7 using the runtime's Sunday-first index.
//   - Each plural pointer/count pair must agree; every entry within a
//     non-empty rule chain has a non-NULL rule head.
//
// Ownership/Lifetime:
//   - Records are owned by LocaleManager's registry once registered.
//   - The registry never frees baked records (arena == NULL); JSON-loaded
//     records free the arena on Unload().
//
// Links: src/runtime/localization/rt_locale_data_en_us.c (baked seed),
//        src/runtime/localization/rt_locale_manager.c (registry owner).
//
//===----------------------------------------------------------------------===//
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Categories for plural rule evaluation.
/// @details Matches the six CLDR plural categories. A locale only populates
///          the categories it uses; queries against missing categories fall
///          through to RT_PLURAL_OTHER.
typedef enum {
    RT_PLURAL_OTHER = 0,
    RT_PLURAL_ZERO,
    RT_PLURAL_ONE,
    RT_PLURAL_TWO,
    RT_PLURAL_FEW,
    RT_PLURAL_MANY,
} rt_plural_category_t;

/// @brief AST node kind for the plural rule mini-language.
/// @details Documented grammar in docs/zannalib/localization/data-files.md.
///          Kept as a small enum so the evaluator switches on it without
///          touching branch predictors beyond the 8-node working set.
typedef enum {
    RT_PRN_TRUE = 0,   ///< always-matches literal
    RT_PRN_OR,         ///< logical OR of two children
    RT_PRN_AND,        ///< logical AND of two children
    RT_PRN_EQ,         ///< equality comparison between two expressions
    RT_PRN_NE,         ///< inequality comparison
    RT_PRN_IN,         ///< expression is in an integer range/list
    RT_PRN_NOT_IN,     ///< expression is not in an integer range/list
    RT_PRN_WITHIN,     ///< expression is within a numeric range/list
    RT_PRN_NOT_WITHIN, ///< expression is not within a numeric range/list
    RT_PRN_VAR,        ///< variable expression (possibly with a `mod N` tail)
    RT_PRN_INT,        ///< integer literal
} rt_plural_rule_kind_t;

/// @brief Variable identifiers used by the plural-rule mini-language.
/// @details Computed once from the numeric input before rule evaluation.
typedef enum {
    RT_PVAR_N = 0, ///< absolute value
    RT_PVAR_I,     ///< integer part
    RT_PVAR_V,     ///< visible fraction digit count with trailing zeros
    RT_PVAR_F,     ///< visible fraction digits with trailing zeros (as int)
    RT_PVAR_T,     ///< visible fraction digits without trailing zeros (as int)
} rt_plural_var_t;

/// @brief Inclusive integer range used by CLDR plural-rule list predicates.
typedef struct rt_plural_rule_range {
    /// Inclusive lower bound.
    int64_t start;
    /// Inclusive upper bound.
    int64_t end;
} rt_plural_rule_range_t;

/// @brief Plural rule AST node.
typedef struct rt_plural_rule_node {
    /// Discriminator selecting the active union member.
    rt_plural_rule_kind_t kind;

    union {
        /// Children for logical/comparison binary nodes.
        struct {
            /// Left operand.
            struct rt_plural_rule_node *l;
            /// Right operand.
            struct rt_plural_rule_node *r;
        } bin;

        /// Expression plus accepted ranges for in/within predicates.
        struct {
            /// Numeric expression being tested.
            struct rt_plural_rule_node *expr;
            /// Inclusive range array.
            rt_plural_rule_range_t *ranges;
            /// Number of elements in @c ranges.
            size_t range_count;
        } range;

        /// Variable expression with optional modulus.
        struct {
            /// CLDR plural operand to read.
            rt_plural_var_t var;
            int32_t mod; ///< 0 means no `mod` operator
        } var;

        /// Value for @ref RT_PRN_INT.
        int64_t int_val;
    } u;
} rt_plural_rule_node_t;

/// @brief One entry in a locale's plural rule chain.
/// @details The chain is walked in array order; first rule whose `head`
///          evaluates truthy wins. A catch-all RT_PRN_TRUE terminator is
///          required for every chain.
typedef struct rt_plural_rule_entry {
    /// Category returned when @c head evaluates true.
    rt_plural_category_t category;
    rt_plural_rule_node_t *head; ///< NULL is illegal
} rt_plural_rule_entry_t;

/// @brief Locale's numeric formatting conventions.
typedef struct rt_locdata_numbers {
    const char *decimal_sep;      ///< e.g. "." (en-US) or "," (fr-FR)
    const char *group_sep;        ///< e.g. "," (en-US) or U+00A0 (fr-FR)
    int32_t group_size;           ///< rightmost group size, typically 3
    int32_t secondary_group_size; ///< repeated group size to the left; 0 => group_size
    /// Locale minus-sign token.
    const char *minus;
    /// Locale plus-sign token.
    const char *plus;
    /// Locale percent token.
    const char *percent;
    /// Locale infinity spelling.
    const char *infinity;
    /// Locale not-a-number spelling.
    const char *nan;
    /// Scientific-notation exponent separator.
    const char *exponent;
    const char *digits; ///< 10-codepoint digit set; ASCII 0-9 for Latin locales
} rt_locdata_numbers_t;

/// @brief Locale's currency defaults.
typedef struct rt_locdata_currency {
    const char *default_code;     ///< ISO-4217 three-letter code (e.g. "USD")
    const char *symbol;           ///< currency symbol (e.g. "$", "€")
    const char *pattern_positive; ///< layout template with {n} (number) and {s} (symbol)
    /// Negative layout template with `{n}` and `{s}` placeholders.
    const char *pattern_negative;
    /// Default minor-unit decimal places.
    int32_t fraction_digits;
} rt_locdata_currency_t;

/// @brief CLDR-style date / time pattern bundle for a locale.
typedef struct rt_locdata_dates_patterns {
    /// Short date pattern.
    const char *short_p;
    /// Medium date pattern.
    const char *medium_p;
    /// Long date pattern.
    const char *long_p;
    /// Full date pattern.
    const char *full_p;
    /// Short time-of-day pattern.
    const char *time_short;
    /// Medium time-of-day pattern.
    const char *time_medium;
    /// Optional short combined date/time pattern.
    const char *datetime_short;
    /// Optional medium combined date/time pattern.
    const char *datetime_medium;
} rt_locdata_dates_patterns_t;

/// @brief Locale's calendar and date-formatting data.
typedef struct rt_locdata_dates {
    const char *const *months_wide; ///< pointer to 12-entry array
    /// Pointer to a 12-entry abbreviated month-name array.
    const char *const *months_abbr;
    const char *const *days_wide; ///< pointer to 7-entry array (index 0 = Sunday)
    /// Pointer to a 7-entry abbreviated weekday array (index 0 = Sunday).
    const char *const *days_abbr;
    /// Ante-meridiem token.
    const char *am;
    /// Post-meridiem token.
    const char *pm;
    /// Named date/time patterns.
    rt_locdata_dates_patterns_t patterns;
} rt_locdata_dates_t;

/// @brief Per-unit plural forms for relative-time formatting.
/// @details Not every category is populated for every locale; the formatter
///          falls through to RT_PLURAL_OTHER when a specific category is NULL.
typedef struct rt_locdata_reltime_unit {
    /// Required/default plural form.
    const char *other;
    /// Zero-category form when defined.
    const char *zero;
    /// One-category form when defined.
    const char *one;
    /// Two-category form when defined.
    const char *two;
    /// Few-category form when defined.
    const char *few;
    /// Many-category form when defined.
    const char *many;
} rt_locdata_reltime_unit_t;

/// @brief Locale's relative-time formatting strings.
/// @details units index: 0=second, 1=minute, 2=hour, 3=day, 4=week, 5=month,
///          6=year. `past` and `future` are the enclosing templates with
///          {n} (number) and {unit} (selected unit string) placeholders.
typedef struct rt_locdata_reltime {
    /// Token for the present instant.
    const char *now;
    /// Long-form past enclosing template.
    const char *past;
    /// Long-form future enclosing template.
    const char *future;
    /// Long-form plural unit tables.
    rt_locdata_reltime_unit_t units[7];
    /// Short-form past enclosing template.
    const char *short_past;
    /// Short-form future enclosing template.
    const char *short_future;
    /// Short-form plural unit tables.
    rt_locdata_reltime_unit_t short_units[7];
} rt_locdata_reltime_t;

/// @brief List-joining templates for one style (and/or/unit).
/// @details Each template is a two-placeholder {0} and {1} string.
typedef struct rt_locdata_list_style {
    const char *pair;   ///< exactly 2 items
    const char *start;  ///< first item + rest
    const char *middle; ///< middle combinations in 3+ lists
    const char *end;    ///< last item
} rt_locdata_list_style_t;

/// @brief Locale's list-formatting templates across styles.
typedef struct rt_locdata_list {
    rt_locdata_list_style_t and_p;  ///< "A, B, and C"
    rt_locdata_list_style_t or_p;   ///< "A, B, or C"
    rt_locdata_list_style_t unit_p; ///< "A B C" (no conjunction)
} rt_locdata_list_t;

/// @brief Collation tailoring metadata.
/// @details v1 stores strength and a reorder array placeholder; actual
///          collation weight overrides are applied inside rt_collator_table.c
///          via locale-specific patch tables keyed by `tag`.
typedef struct rt_locdata_collation {
    int32_t strength;       ///< 1-3 valid; 4 warned and clamped to 3
    const int32_t *reorder; ///< optional array of codepoints; may be NULL
    /// Number of entries in @c reorder.
    size_t reorder_len;
} rt_locdata_collation_t;

/// @brief Top-level locale-data record referenced by Locale handles.
typedef struct rt_locale_data {
    const char *tag; ///< canonical BCP-47 identifier

    /// Localized/native locale display names.
    struct {
        const char *language; ///< display name in native language
        /// Region display name.
        const char *region;
        const char *display; ///< combined display name
    } names;

    char text_direction[4];    ///< "ltr" or "rtl"
    int32_t first_day_of_week; ///< 0=Sun..6=Sat
    char measurement[8];       ///< "metric" / "us" / "uk"

    /// Numeric formatting conventions.
    rt_locdata_numbers_t numbers;
    /// Currency defaults and templates.
    rt_locdata_currency_t currency;
    /// Calendar names and patterns.
    rt_locdata_dates_t dates;
    /// Relative-time templates.
    rt_locdata_reltime_t reltime;

    const rt_plural_rule_entry_t
        *plural_cardinal; ///< NULL-terminator-free; `cardinal_count` is authoritative
    /// Number of cardinal rule entries.
    size_t cardinal_count;
    /// Ordinal plural rule chain.
    const rt_plural_rule_entry_t *plural_ordinal;
    /// Number of ordinal rule entries.
    size_t ordinal_count;

    /// Conjunction, disjunction, and unit-list templates.
    rt_locdata_list_t list_format;
    /// Default collation configuration.
    rt_locdata_collation_t collation;

    /// @brief Opaque arena pointer for JSON-loaded records; NULL for baked.
    /// @details When non-NULL, LocaleManager.Unload frees this pointer via
    ///          free() after removing the registry entry.
    void *arena;

    /// @brief Live formatter/collator count holding this data pointer.
    /// @details Incremented by every NumberFormat/DateFormat/etc. constructor
    ///          that captures this record; decremented on instance finalize.
    ///          Atomic in rt_locale_manager.c. Non-zero at Unload() time
    ///          traps with a "locale in use" diagnostic.
    int64_t formatter_refs;
} rt_locale_data_t;

/// @brief Retrieve the statically baked en-US data record.
/// @details Returns a pointer to a process-lifetime constant; the pointer
///          is valid for the entire program run. Used by LocaleManager
///          bootstrap and by `LoadBuiltin("en-US")`.
/// @return Borrowed process-lifetime record; never NULL.
const rt_locale_data_t *rt_locale_data_en_us(void);

#ifdef __cplusplus
}
#endif
