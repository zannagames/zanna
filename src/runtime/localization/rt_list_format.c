//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/localization/rt_list_format.c
// Purpose: Implementation of Zanna.Localization.ListFormat. Joins a list of
//          rt_string items using the bound locale's list_format templates
//          (pair / start / middle / end) per CLDR conventions.
//
// Key invariants:
//   - Joining algorithm (for n >= 3): begin with end(items[n-2], items[n-1]),
//     then wrap leftwards: result = middle(items[k], result) for k in
//     n-3 .. 1, then start(items[0], result).
//   - Templates are simple {0}/{1} substitutions. Missing templates fall
//     back to ", " concatenation to avoid producing empty output on
//     misconfigured locale data.
//
// Ownership/Lifetime:
//   - Handles are rt_obj_new_i64-allocated; GC-managed.
//   - Each instance retains its Locale handle and immutable locale-data snapshot.
//   - Every format operation returns a fresh runtime string reference.
//
// Links: src/runtime/localization/rt_list_format.h (interface),
//        src/runtime/localization/rt_locale_data.h (template storage).
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Implements locale-aware CLDR-style list formatting.
 * @details Retains a locale-data snapshot, expands pair placeholders with
 * checked string-builder operations, right-folds lists through pair, end,
 * middle, and start templates, and preserves element-reference ownership.
 */

#include "rt_list_format.h"

#include "rt_heap.h"
#include "rt_internal.h"
#include "rt_list.h"
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
// Instance struct
//===----------------------------------------------------------------------===//

/** GC payload retaining one ListFormat's Locale and template snapshot. */
typedef struct rt_list_format_inst {
    void *locale;                 ///< Retained Locale handle, if supplied.
    const rt_locale_data_t *data; ///< Retained immutable list-pattern data.
} rt_list_format_inst_t;

/// @brief Unchecked cast of an opaque handle to the ListFormat instance.
/// @param obj Valid ListFormat payload.
/// @return The same pointer interpreted as @ref rt_list_format_inst_t.
static rt_list_format_inst_t *as_fmt(void *obj) {
    return (rt_list_format_inst_t *)obj;
}

/// @brief Drop one GC reference to @p obj and free it if the count hit zero.
/// @param obj Owned runtime-managed handle reference; NULL is a no-op.
static void lf_release_handle(void *obj) {
    if (obj && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

/// @brief GC finalizer: release the formatter's locale data and locale handle.
/// @details The Locale and data snapshot were retained independently at
///          construction and are each released exactly once.
/// @param obj ListFormat payload being finalized; NULL is a no-op.
static void lf_finalizer(void *obj) {
    rt_list_format_inst_t *f = (rt_list_format_inst_t *)obj;
    if (!f)
        return;
    rt_locale_manager_release_data(f->data);
    lf_release_handle(f->locale);
    f->locale = NULL;
    f->data = NULL;
}

//===----------------------------------------------------------------------===//
// Constructors / properties
//===----------------------------------------------------------------------===//

/// @brief Allocate and initialize a GC-managed ListFormat for @p locale.
/// @details Retains the locale handle + its data. Traps on allocation failure;
///          installs @ref lf_finalizer.
/// @param locale Optional Locale handle to retain; NULL selects invariant fallback data.
/// @return Fresh GC-managed ListFormat, or NULL after an allocation trap.
static void *lf_alloc(void *locale) {
    rt_list_format_inst_t *f =
        (rt_list_format_inst_t *)rt_obj_new_i64(0, (int64_t)sizeof(rt_list_format_inst_t));
    if (!f) {
        rt_trap("Zanna.Localization.ListFormat: allocation failed");
        return NULL;
    }
    f->locale = locale;
    if (f->locale)
        rt_heap_retain(f->locale);
    f->data = rt_locale_get_data(locale);
    rt_locale_manager_retain_data(f->data);
    rt_obj_set_finalizer(f, lf_finalizer);
    return f;
}

/// @brief Create a ListFormat bound to the process's current Locale snapshot.
/// @details The locale manager's temporary reference is released after the
///          formatter acquires its own Locale and data references.
/// @return Fresh GC-managed ListFormat, or NULL after an allocation trap.
void *rt_list_format_new(void) {
    void *current = rt_locale_manager_current();
    void *fmt = lf_alloc(current);
    lf_release_handle(current);
    return fmt;
}

/// @brief Create a ListFormat bound to a specified Locale.
/// @param locale Locale retained by the result; may be NULL for invariant data.
/// @return Fresh GC-managed ListFormat, or NULL after an allocation trap.
void *rt_list_format_for_locale(void *locale) {
    return lf_alloc(locale);
}

/// @brief Get the Locale retained by a ListFormat.
/// @param self Valid ListFormat handle; may be NULL.
/// @return Borrowed Locale handle, or NULL when absent.
void *rt_list_format_get_locale(void *self) {
    return self ? as_fmt(self)->locale : NULL;
}

//===----------------------------------------------------------------------===//
// Template expansion: {0} and {1} positional placeholders
//===----------------------------------------------------------------------===//

/// @brief Append bytes to a ListFormat expansion and report builder failure.
/// @details CLDR list expansion builds nested intermediate strings. If any
///          builder append fails, callers must stop the current expansion to
///          avoid returning a partially formatted list.
/// @param sb Destination builder.
/// @param bytes Bytes to append.
/// @param len Number of bytes in @p bytes.
/// @return 1 on success, 0 when the append failed.
static int lf_append_bytes_checked(rt_string_builder *sb, const char *bytes, size_t len) {
    return rt_sb_append_bytes(sb, bytes, len) == RT_SB_OK;
}

/// @brief Substitute {0}→@p a and {1}→@p b into @p tmpl (default "{0}, {1}").
/// @details Placeholders may appear repeatedly and any other template byte is
///          copied literally. NULL operands contribute empty text. Builder or
///          invalid-string failure discards partial output and returns empty.
/// @param tmpl NUL-terminated template, or NULL for the comma-separated fallback.
/// @param a First runtime string operand; may be NULL.
/// @param b Second runtime string operand; may be NULL.
/// @return Fresh expanded runtime string.
static rt_string expand_pair(const char *tmpl, rt_string a, rt_string b) {
    if (!tmpl)
        tmpl = "{0}, {1}";
    rt_string_builder sb;
    rt_sb_init(&sb);
    const char *p = tmpl;
    const char *a_cs = a ? rt_string_cstr(a) : "";
    const char *b_cs = b ? rt_string_cstr(b) : "";
    int64_t a_len = a ? rt_str_len(a) : 0;
    int64_t b_len = b ? rt_str_len(b) : 0;
    if (a_len < 0 || b_len < 0)
        goto expand_error;
    while (*p) {
        if (p[0] == '{' && p[1] == '0' && p[2] == '}') {
            if (a_cs && a_len > 0 && !lf_append_bytes_checked(&sb, a_cs, (size_t)a_len))
                goto expand_error;
            p += 3;
        } else if (p[0] == '{' && p[1] == '1' && p[2] == '}') {
            if (b_cs && b_len > 0 && !lf_append_bytes_checked(&sb, b_cs, (size_t)b_len))
                goto expand_error;
            p += 3;
        } else {
            if (!lf_append_bytes_checked(&sb, p, 1))
                goto expand_error;
            ++p;
        }
    }
    rt_string r = rt_string_from_bytes(sb.data, sb.len);
    rt_sb_free(&sb);
    return r;

expand_error:
    rt_sb_free(&sb);
    return rt_string_from_bytes("", 0);
}

//===----------------------------------------------------------------------===//
// Join algorithm
//===----------------------------------------------------------------------===//

/// @brief Join @p items per a CLDR list @p style (start/middle/end/pair).
/// @details 0/1 items are trivial; 2 use the "pair" template; 3+ fold from the
///          right ("end", then "middle"s, then "start") so nested expansions
///          place separators exactly as CLDR specifies. Returns a new string.
///          Each retained reference returned by List.Get is either transferred
///          for the single-item result or released after expansion.
/// @param items Runtime List containing strings or NULL elements; may be NULL.
/// @param style Locale template set containing pair/start/middle/end patterns.
/// @return Fresh formatted runtime string; empty for NULL/empty input.
static rt_string join_with_style(void *items, const rt_locdata_list_style_t *style) {
    if (!items)
        return rt_string_from_bytes("", 0);
    int64_t n = rt_list_len(items);
    if (n <= 0)
        return rt_string_from_bytes("", 0);
    // rt_list_get returns a retained reference; every element fetched here
    // must be released once consumed (VDOC-075).
    if (n == 1) {
        rt_string a = (rt_string)rt_list_get(items, 0);
        if (!a)
            return rt_string_from_bytes("", 0);
        return a; // transfer the retained reference to the caller
    }
    if (n == 2) {
        rt_string a = (rt_string)rt_list_get(items, 0);
        rt_string b = (rt_string)rt_list_get(items, 1);
        rt_string out = expand_pair(style->pair, a, b);
        rt_string_unref(a);
        rt_string_unref(b);
        return out;
    }
    // n >= 3: build from the right.
    rt_string last_a = (rt_string)rt_list_get(items, n - 2);
    rt_string last_b = (rt_string)rt_list_get(items, n - 1);
    rt_string result = expand_pair(style->end, last_a, last_b);
    rt_string_unref(last_a);
    rt_string_unref(last_b);

    for (int64_t k = n - 3; k > 0; --k) {
        rt_string item = (rt_string)rt_list_get(items, k);
        rt_string next = expand_pair(style->middle, item, result);
        rt_string_unref(item);
        rt_string_unref(result);
        result = next;
    }

    rt_string first = (rt_string)rt_list_get(items, 0);
    rt_string final = expand_pair(style->start, first, result);
    rt_string_unref(first);
    rt_string_unref(result);
    return final;
}

//===----------------------------------------------------------------------===//
// Public API
//===----------------------------------------------------------------------===//

/// @brief Join a string List with the Locale's conjunction templates.
/// @param self Valid ListFormat handle.
/// @param items Runtime List of strings; may be NULL.
/// @return Fresh formatted runtime string, or a fresh empty string for NULL self/list.
rt_string rt_list_format_and(void *self, void *items) {
    if (!self)
        return rt_string_from_bytes("", 0);
    return join_with_style(items, &as_fmt(self)->data->list_format.and_p);
}

/// @brief Join a string List with the Locale's disjunction templates.
/// @param self Valid ListFormat handle.
/// @param items Runtime List of strings; may be NULL.
/// @return Fresh formatted runtime string, or a fresh empty string for NULL self/list.
rt_string rt_list_format_or(void *self, void *items) {
    if (!self)
        return rt_string_from_bytes("", 0);
    return join_with_style(items, &as_fmt(self)->data->list_format.or_p);
}

/// @brief Join a string List with the Locale's unit-list templates.
/// @param self Valid ListFormat handle.
/// @param items Runtime List of strings; may be NULL.
/// @return Fresh formatted runtime string, or a fresh empty string for NULL self/list.
rt_string rt_list_format_unit(void *self, void *items) {
    if (!self)
        return rt_string_from_bytes("", 0);
    return join_with_style(items, &as_fmt(self)->data->list_format.unit_p);
}

/// @brief Join a string List using the current short-form behavior.
/// @details The present locale schema has no separate short template set, so
///          this delegates exactly to @ref rt_list_format_and.
/// @param self Valid ListFormat handle.
/// @param items Runtime List of strings; may be NULL.
/// @return Fresh formatted runtime string.
rt_string rt_list_format_short(void *self, void *items) {
    // Phase 4: baked en-US has a single set of and_p templates used for both
    // the default and the short form. A future phase will introduce
    // ampersand-style short templates as a separate list_format sub-record.
    return rt_list_format_and(self, items);
}
