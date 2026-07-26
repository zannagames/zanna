//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/localization/rt_plural_rules.c
// Purpose: Implementation of Zanna.Localization.PluralRules. Walks the
//          cardinal/ordinal rule AST chains stored on rt_locale_data_t and
//          returns the matching CLDR plural category. The AST is a compact
//          recursive-descent structure; evaluation is a single pass across
//          the chain that short-circuits on the first true rule.
//
// Key invariants:
//   - Locale rule chains conventionally end in an RT_PRN_TRUE node. Empty,
//     malformed, or non-matching chains still return RT_PLURAL_OTHER as a
//     conservative evaluator fallback.
//   - AST evaluation is pure (no allocation) and thread-safe.
//   - Operand variables (n, i, v, f, t) are computed according to CLDR
//     Unicode Technical Standard #35 Section 5 conventions: n is the
//     absolute value; i is the integer part; v / f / t encode visible
//     fraction digits. For real-valued inputs we render a C-locale decimal
//     with 15 significant digits before deriving the fractional operands.
//
// Ownership/Lifetime:
//   - PluralRules handles are rt_obj_new_i64-allocated; GC manages them.
//   - Each handle retains its Locale and captured locale-data record until its
//     finalizer runs. Category strings and category lists are returned owned.
//
// Links: src/runtime/localization/rt_plural_rules.h (interface),
//        src/runtime/localization/rt_locale_data.h (AST node shape),
//        docs/zannalib/localization/messages.md (user documentation).
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Implements CLDR cardinal and ordinal plural-category evaluation.
 * @details Captures locale rule chains, derives exact integer or bounded
 * real-number operands, evaluates AST expressions and ranges with short
 * circuiting, and returns canonical category strings and distinct category
 * lists under explicit managed ownership.
 */

#include "rt_plural_rules.h"

#include "rt_heap.h"
#include "rt_internal.h"
#include "rt_list.h"
#include "rt_locale.h"
#include "rt_locale_data.h"
#include "rt_locale_manager.h"
#include "rt_object.h"
#include "rt_string.h"
#include "rt_trap.h"

#include <locale.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

//===----------------------------------------------------------------------===//
// PluralRules instance struct
//===----------------------------------------------------------------------===//

/// @brief Retained locale context for one PluralRules instance.
typedef struct rt_plural_rules_inst {
    void *locale;                 ///< strong Locale handle ref
    const rt_locale_data_t *data; ///< non-owning
} rt_plural_rules_inst_t;

/// @brief Drop one GC reference to @p obj and free it if the count hit zero.
/// @param obj Runtime handle to release, or NULL for a no-op.
static void plural_release_handle(void *obj) {
    if (obj && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

/// @brief GC finalizer: release the rules' locale data and locale handle.
/// @param obj PluralRules allocation being finalized. NULL is accepted.
static void plural_finalizer(void *obj) {
    rt_plural_rules_inst_t *self = (rt_plural_rules_inst_t *)obj;
    if (!self)
        return;
    rt_locale_manager_release_data(self->data);
    plural_release_handle(self->locale);
    self->locale = NULL;
    self->data = NULL;
}

/// @brief snprintf forced through the "C" LC_NUMERIC locale so the operand
///        digits ("%.Nf" of the value) are always '.'-separated regardless of
///        the ambient locale — the CLDR plural operands must be locale-neutral.
/// @details Creates and releases a platform C numeric-locale object for this
///          call, falling back to ordinary `vsnprintf` if creation fails.
/// @param out Destination buffer for formatted bytes.
/// @param cap Capacity of @p out in bytes.
/// @param fmt `printf`-style format string followed by its arguments.
/// @return Required byte count excluding the terminator, or a negative value
///         on encoding/format failure.
static int plural_snprintf_c(char *out, size_t cap, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wformat-nonliteral"
#endif
#if defined(_WIN32)
    _locale_t c_locale = _create_locale(LC_NUMERIC, "C");
    int n = c_locale ? _vsnprintf_l(out, cap, fmt, c_locale, args) : vsnprintf(out, cap, fmt, args);
    if (c_locale)
        _free_locale(c_locale);
#else
    locale_t c_locale = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);
    locale_t old = c_locale ? uselocale(c_locale) : (locale_t)0;
    int n = vsnprintf(out, cap, fmt, args);
    if (old)
        uselocale(old);
    if (c_locale)
        freelocale(c_locale);
#endif
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
    va_end(args);
    return n;
}

//===----------------------------------------------------------------------===//
// CLDR operand computation
//===----------------------------------------------------------------------===//

/// @brief CLDR operands packed for rule evaluation.
typedef struct {
    double n;       ///< absolute value
    double i_d;     ///< integer part as double, preserving INT64_MIN magnitude
    int64_t i;      ///< integer part
    int64_t v;      ///< visible fraction digit count (with trailing zeros)
    int64_t f;      ///< visible fraction digits as integer (with trailing zeros)
    int64_t t;      ///< visible fraction digits as integer (without trailing zeros)
    int int_exact;  ///< 1 when the input was an integer; `mag` is exact
    uint64_t mag;   ///< exact magnitude for integer inputs (VDOC-083)
} plural_operands_t;

/// @brief Compute plural operands from an integer input (all-integer path).
/// @details Uses unsigned magnitude arithmetic so `INT64_MIN` remains exact.
///          Visible-fraction operands are all zero.
/// @param n Signed integer whose absolute value supplies `n` and `i`.
/// @return Fully initialized operand record with exact-magnitude metadata.
static plural_operands_t operands_from_int(int64_t n) {
    plural_operands_t op;
    uint64_t mag = n < 0 ? (uint64_t)(-(n + 1)) + 1u : (uint64_t)n;
    op.n = (double)mag;
    op.i_d = (double)mag;
    op.i = mag > (uint64_t)INT64_MAX ? INT64_MAX : (int64_t)mag;
    op.v = 0;
    op.f = 0;
    op.t = 0;
    op.int_exact = 1;
    op.mag = mag;
    return op;
}

/// @brief Compute plural operands from a real-valued input.
/// @details Formats via C-locale `snprintf("%.15g", n)`, then extracts the
///          integer and fractional parts for operand derivation. This avoids
///          the default `%g` six-significant-digit truncation while still
///          stripping insignificant trailing zeros for common user inputs.
/// @param n Real input whose absolute value supplies the operands.
/// @return Fully initialized operand record. Non-finite input maps to zero
///         operands, although public real selection rejects it beforehand.
static plural_operands_t operands_from_double(double n) {
    double abs_n = n < 0 ? -n : n;
    plural_operands_t op;
    op.n = abs_n;
    op.i_d = 0.0;
    op.int_exact = 0;
    op.mag = 0;

    if (!isfinite(abs_n)) {
        op.n = 0.0;
        op.i = 0;
        op.v = 0;
        op.f = 0;
        op.t = 0;
        return op;
    }

    // Integer-valued double: skip the string parse entirely.
    if (abs_n > (double)INT64_MAX || floor(abs_n) == abs_n) {
        op.i_d = floor(abs_n);
        op.i = abs_n > (double)INT64_MAX ? INT64_MAX : (int64_t)abs_n;
        op.v = 0;
        op.f = 0;
        op.t = 0;
        return op;
    }

    // Render via %g which strips trailing zeros by default; %.15g provides
    // bounded working precision without exposing most binary representation noise.
    char buf[64];
    int len = plural_snprintf_c(buf, sizeof(buf), "%.15g", abs_n);
    if (len < 0 || len >= (int)sizeof(buf)) {
        op.i = (int64_t)abs_n;
        op.v = 0;
        op.f = 0;
        op.t = 0;
        return op;
    }

    // Parse mantissa digits, dot position, and any exponent so scientific
    // spellings like "1e-07" derive the same operands as their fixed-point
    // form "0.0000001" (VDOC-076).
    char digits[32];
    int nd = 0;
    int dot_pos = -1; // mantissa digits before the '.'
    const char *p = buf;
    for (; *p && *p != 'e' && *p != 'E'; ++p) {
        if (*p == '.') {
            dot_pos = nd;
            continue;
        }
        if (*p >= '0' && *p <= '9' && nd < (int)sizeof(digits))
            digits[nd++] = *p;
    }
    if (dot_pos < 0)
        dot_pos = nd;
    long expo = 0;
    if (*p == 'e' || *p == 'E')
        expo = strtol(p + 1, NULL, 10);

    op.i_d = floor(abs_n);
    op.i = (int64_t)floor(abs_n);

    // Number of mantissa digits left of the decimal point in fixed notation.
    long int_digits = dot_pos + expo;
    if (int_digits >= nd) {
        // Integer-valued spelling (possibly with trailing zeros).
        op.v = 0;
        op.f = 0;
        op.t = 0;
        return op;
    }

    // Fixed-notation fraction: leading zeros (negative int_digits) followed
    // by the mantissa digits after the decimal point. %g strips trailing
    // zeros, so the fraction string is already minimal.
    long lead_zeros = int_digits < 0 ? -int_digits : 0;
    long frac_start_idx = int_digits > 0 ? int_digits : 0;
    int64_t frac_val = 0;
    for (int k = (int)frac_start_idx; k < nd; ++k) {
        int digit = digits[k] - '0';
        if (frac_val <= (INT64_MAX - digit) / 10)
            frac_val = frac_val * 10 + digit;
    }

    op.v = lead_zeros + (nd - frac_start_idx);
    op.f = frac_val;

    // t: strip trailing zeros from f.
    int64_t t = frac_val;
    while (t != 0 && t % 10 == 0)
        t /= 10;
    op.t = t;

    return op;
}

//===----------------------------------------------------------------------===//
// AST evaluator
//===----------------------------------------------------------------------===//

/// @brief Resolve a VAR or INT node to its numeric value.
/// @details Applies a variable node's positive modulus with floating `fmod`.
///          Unsupported or NULL nodes resolve to zero.
/// @param node Expression node to evaluate.
/// @param op Precomputed CLDR operands.
/// @return Numeric expression value as a double.
static double eval_expr(const rt_plural_rule_node_t *node, const plural_operands_t *op) {
    if (!node)
        return 0.0;
    switch (node->kind) {
        case RT_PRN_INT:
            return (double)node->u.int_val;
        case RT_PRN_VAR: {
            double v;
            switch (node->u.var.var) {
                case RT_PVAR_N:
                    v = op->n;
                    break;
                case RT_PVAR_I:
                    v = op->i_d;
                    break;
                case RT_PVAR_V:
                    v = (double)op->v;
                    break;
                case RT_PVAR_F:
                    v = (double)op->f;
                    break;
                case RT_PVAR_T:
                    v = (double)op->t;
                    break;
                default:
                    v = 0.0;
                    break;
            }
            if (node->u.var.mod > 0)
                return fmod(v, (double)node->u.var.mod);
            return v;
        }
        default:
            return 0.0;
    }
}

/// @brief True iff @p x is finite and has no fractional part.
/// @param x Floating-point value to inspect.
/// @return Non-zero only for finite integral values.
static int plural_is_integral(double x) {
    return isfinite(x) && floor(x) == x;
}

/// @brief Exact unsigned-64 expression evaluation (VDOC-083).
/// @details Succeeds when the expression's value is exactly representable:
///          rule integer literals, the always-integral v/f/t operands, and
///          n/i for integer inputs (tracked via `mag`, which also preserves
///          the INT64_MIN magnitude). Modulo is applied in integer space.
/// @param node Expression node to evaluate.
/// @param op Precomputed CLDR operands and exact-integer metadata.
/// @param out Receives the exact unsigned value on success.
/// @return 1 with @p *out set when exact; 0 to fall back to the double path.
static int eval_expr_u64(const rt_plural_rule_node_t *node,
                         const plural_operands_t *op,
                         uint64_t *out) {
    if (!node)
        return 0;
    switch (node->kind) {
        case RT_PRN_INT:
            if (node->u.int_val < 0)
                return 0;
            *out = (uint64_t)node->u.int_val;
            return 1;
        case RT_PRN_VAR: {
            uint64_t v;
            switch (node->u.var.var) {
                case RT_PVAR_N:
                case RT_PVAR_I:
                    if (!op->int_exact)
                        return 0;
                    v = op->mag;
                    break;
                case RT_PVAR_V:
                    v = (uint64_t)op->v;
                    break;
                case RT_PVAR_F:
                    v = (uint64_t)op->f;
                    break;
                case RT_PVAR_T:
                    v = (uint64_t)op->t;
                    break;
                default:
                    return 0;
            }
            if (node->u.var.mod > 0)
                v %= (uint64_t)node->u.var.mod;
            *out = v;
            return 1;
        }
        default:
            return 0;
    }
}

/// @brief Evaluate a CLDR range predicate (`n in/within a..b, c..d`).
/// @param node Range-predicate AST node.
/// @param op Precomputed CLDR operands.
/// @param allow_fraction 0 for "in" (integers only), 1 for "within".
/// @return 1 if the operand expression's value lands in any listed range.
static int eval_range_pred(const rt_plural_rule_node_t *node,
                           const plural_operands_t *op,
                           int allow_fraction) {
    // Exact-integer path: full 64-bit precision for integer inputs
    // (VDOC-083).
    uint64_t exact;
    if (eval_expr_u64(node->u.range.expr, op, &exact)) {
        for (size_t i = 0; i < node->u.range.range_count; ++i) {
            int64_t start = node->u.range.ranges[i].start;
            int64_t end = node->u.range.ranges[i].end;
            if (start < 0)
                start = 0;
            if (end < 0)
                continue;
            if (exact >= (uint64_t)start && exact <= (uint64_t)end)
                return 1;
        }
        return 0;
    }

    double value = eval_expr(node->u.range.expr, op);
    if (!isfinite(value))
        return 0;
    if (!allow_fraction && !plural_is_integral(value))
        return 0;
    for (size_t i = 0; i < node->u.range.range_count; ++i) {
        double start = (double)node->u.range.ranges[i].start;
        double end = (double)node->u.range.ranges[i].end;
        if (value >= start && value <= end)
            return 1;
    }
    return 0;
}

/// @brief Evaluate a rule AST node as a boolean predicate.
/// @details Recursively short-circuits `and` and `or`, uses exact unsigned
///          comparison when available, and delegates range operators to
///          @ref eval_range_pred.
/// @param node Predicate node to evaluate.
/// @param op Precomputed CLDR operands.
/// @return 1 when the predicate matches; 0 for false, NULL, or unsupported nodes.
static int eval_pred(const rt_plural_rule_node_t *node, const plural_operands_t *op) {
    if (!node)
        return 0;
    switch (node->kind) {
        case RT_PRN_TRUE:
            return 1;
        case RT_PRN_OR:
            return eval_pred(node->u.bin.l, op) || eval_pred(node->u.bin.r, op);
        case RT_PRN_AND:
            return eval_pred(node->u.bin.l, op) && eval_pred(node->u.bin.r, op);
        case RT_PRN_EQ: {
            uint64_t le, re;
            if (eval_expr_u64(node->u.bin.l, op, &le) && eval_expr_u64(node->u.bin.r, op, &re))
                return le == re;
            return eval_expr(node->u.bin.l, op) == eval_expr(node->u.bin.r, op);
        }
        case RT_PRN_NE: {
            uint64_t le, re;
            if (eval_expr_u64(node->u.bin.l, op, &le) && eval_expr_u64(node->u.bin.r, op, &re))
                return le != re;
            return eval_expr(node->u.bin.l, op) != eval_expr(node->u.bin.r, op);
        }
        case RT_PRN_IN:
            return eval_range_pred(node, op, 0);
        case RT_PRN_NOT_IN:
            return !eval_range_pred(node, op, 0);
        case RT_PRN_WITHIN:
            return eval_range_pred(node, op, 1);
        case RT_PRN_NOT_WITHIN:
            return !eval_range_pred(node, op, 1);
        default:
            return 0;
    }
}

/// @brief Return the first rule entry whose predicate matches @p op,
///        defaulting to RT_PLURAL_OTHER when none do (CLDR fallback).
/// @param entries Ordered plural-rule entries, or NULL.
/// @param count Number of entries in @p entries.
/// @param op Precomputed CLDR operands.
/// @return First matching category, or @ref RT_PLURAL_OTHER.
static rt_plural_category_t walk_chain(const rt_plural_rule_entry_t *entries,
                                       size_t count,
                                       const plural_operands_t *op) {
    if (!entries || count == 0)
        return RT_PLURAL_OTHER;
    for (size_t i = 0; i < count; ++i) {
        if (eval_pred(entries[i].head, op))
            return entries[i].category;
    }
    return RT_PLURAL_OTHER;
}

//===----------------------------------------------------------------------===//
// Internal entry points (shared with other Localization modules)
//===----------------------------------------------------------------------===//

/// @brief Select a cardinal category directly from locale data for a real input.
/// @param data Borrowed locale-data record, or NULL.
/// @param n Real value whose absolute-value operands will be evaluated.
/// @return First matching category, or `other` for NULL data, non-finite input,
///         or a non-matching chain.
rt_plural_category_t rt_plural_rules_select_cardinal(const rt_locale_data_t *data, double n) {
    if (!data || !isfinite(n))
        return RT_PLURAL_OTHER;
    plural_operands_t op = operands_from_double(n);
    return walk_chain(data->plural_cardinal, data->cardinal_count, &op);
}

/// @brief Select a cardinal category directly from locale data for an integer.
/// @param data Borrowed locale-data record, or NULL.
/// @param n Signed integer evaluated with exact unsigned-magnitude operands.
/// @return First matching category, or `other` for NULL data or no match.
rt_plural_category_t rt_plural_rules_select_cardinal_int(const rt_locale_data_t *data, int64_t n) {
    if (!data)
        return RT_PLURAL_OTHER;
    plural_operands_t op = operands_from_int(n);
    return walk_chain(data->plural_cardinal, data->cardinal_count, &op);
}

/// @brief Select an ordinal category directly from locale data for an integer.
/// @param data Borrowed locale-data record, or NULL.
/// @param n Signed integer evaluated with exact unsigned-magnitude operands.
/// @return First matching ordinal category, or `other` for NULL data or no match.
rt_plural_category_t rt_plural_rules_select_ordinal(const rt_locale_data_t *data, int64_t n) {
    if (!data)
        return RT_PLURAL_OTHER;
    plural_operands_t op = operands_from_int(n);
    return walk_chain(data->plural_ordinal, data->ordinal_count, &op);
}

/// @brief Convert a plural-category enum to its canonical CLDR keyword.
/// @param cat Category to name.
/// @return Process-lifetime string literal; unknown values map to `"other"`.
const char *rt_plural_rules_category_name(rt_plural_category_t cat) {
    switch (cat) {
        case RT_PLURAL_ZERO:
            return "zero";
        case RT_PLURAL_ONE:
            return "one";
        case RT_PLURAL_TWO:
            return "two";
        case RT_PLURAL_FEW:
            return "few";
        case RT_PLURAL_MANY:
            return "many";
        case RT_PLURAL_OTHER:
        default:
            return "other";
    }
}

//===----------------------------------------------------------------------===//
// Public class API
//===----------------------------------------------------------------------===//

/// @brief Wrap a plural category's canonical CLDR keyword
///        ("zero"/"one"/"two"/"few"/"many"/"other") as a runtime string.
/// @param cat Category to convert.
/// @return Newly allocated runtime string containing the canonical keyword.
static rt_string category_to_string(rt_plural_category_t cat) {
    const char *s = rt_plural_rules_category_name(cat);
    return rt_string_from_bytes(s, strlen(s));
}

/// @brief Create a PluralRules instance bound to a locale.
/// @details Retains both @p locale and its resolved locale-data record; NULL
///          locale resolves to the baked invariant data.
/// @param locale Locale handle to retain, or NULL for invariant rules.
/// @return Newly allocated PluralRules handle, or NULL after allocation failure
///         if the runtime trap hook returns.
void *rt_plural_rules_for_locale(void *locale) {
    rt_plural_rules_inst_t *self =
        (rt_plural_rules_inst_t *)rt_obj_new_i64(0, (int64_t)sizeof(rt_plural_rules_inst_t));
    if (!self) {
        rt_trap("Zanna.Localization.PluralRules: allocation failed");
        return NULL;
    }
    self->locale = locale;
    if (self->locale)
        rt_heap_retain(self->locale);
    self->data = rt_locale_get_data(locale);
    rt_locale_manager_retain_data(self->data);
    rt_obj_set_finalizer(self, plural_finalizer);
    return self;
}

/// @brief Select the bound locale's cardinal category for a real value.
/// @param self_obj PluralRules handle, or NULL.
/// @param n Real value to classify.
/// @return Newly allocated canonical category string; NULL @p self_obj and
///         non-finite values select `"other"`.
rt_string rt_plural_rules_cardinal(void *self_obj, double n) {
    if (!self_obj)
        return rt_string_from_bytes("other", 5);
    rt_plural_rules_inst_t *self = (rt_plural_rules_inst_t *)self_obj;
    return category_to_string(rt_plural_rules_select_cardinal(self->data, n));
}

/// @brief Select the bound locale's cardinal category for an integer.
/// @param self_obj PluralRules handle, or NULL.
/// @param n Signed integer to classify exactly.
/// @return Newly allocated canonical category string; NULL @p self_obj selects
///         `"other"`.
rt_string rt_plural_rules_cardinal_int(void *self_obj, int64_t n) {
    if (!self_obj)
        return rt_string_from_bytes("other", 5);
    rt_plural_rules_inst_t *self = (rt_plural_rules_inst_t *)self_obj;
    return category_to_string(rt_plural_rules_select_cardinal_int(self->data, n));
}

/// @brief Select the bound locale's ordinal category for an integer.
/// @param self_obj PluralRules handle, or NULL.
/// @param n Signed integer to classify exactly.
/// @return Newly allocated canonical category string; NULL @p self_obj selects
///         `"other"`.
rt_string rt_plural_rules_ordinal(void *self_obj, int64_t n) {
    if (!self_obj)
        return rt_string_from_bytes("other", 5);
    rt_plural_rules_inst_t *self = (rt_plural_rules_inst_t *)self_obj;
    return category_to_string(rt_plural_rules_select_ordinal(self->data, n));
}

/// @brief Enumerate distinct categories used by the cardinal and ordinal chains.
/// @details Preserves first appearance across the cardinal chain followed by
///          the ordinal chain, skips invalid enum values, and stores owned
///          category strings in a fresh List.
/// @param self_obj PluralRules handle, or NULL.
/// @return Newly allocated List, empty for NULL/missing locale data; possibly
///         NULL if list allocation fails.
void *rt_plural_rules_categories(void *self_obj) {
    void *list = rt_list_new();
    if (!list || !self_obj)
        return list;

    rt_plural_rules_inst_t *self = (rt_plural_rules_inst_t *)self_obj;
    if (!self->data)
        return list;

    // Accumulate seen categories across both chains, preserving insertion order.
    int seen[6] = {0}; // index by rt_plural_category_t
    rt_plural_category_t order[6];
    size_t order_len = 0;

    for (size_t i = 0; i < self->data->cardinal_count; ++i) {
        rt_plural_category_t c = self->data->plural_cardinal[i].category;
        if ((int)c >= 0 && (int)c < 6 && !seen[(int)c]) {
            seen[(int)c] = 1;
            order[order_len++] = c;
        }
    }
    for (size_t i = 0; i < self->data->ordinal_count; ++i) {
        rt_plural_category_t c = self->data->plural_ordinal[i].category;
        if ((int)c >= 0 && (int)c < 6 && !seen[(int)c]) {
            seen[(int)c] = 1;
            order[order_len++] = c;
        }
    }

    for (size_t i = 0; i < order_len; ++i) {
        const char *name = rt_plural_rules_category_name(order[i]);
        // rt_list_push retains; release the fresh string's initial reference
        // so the list holds the only one (VDOC-077).
        rt_string entry = rt_string_from_bytes(name, strlen(name));
        rt_list_push(list, entry);
        rt_string_unref(entry);
    }
    return list;
}
