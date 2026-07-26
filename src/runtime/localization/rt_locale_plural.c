//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/localization/rt_locale_plural.c
// Purpose: Recursive-descent parser for CLDR plural-rule expressions (operand
//   comparisons, ranges, and/or), producing an arena-allocated AST.
//
// Key invariants:
//   - Parsing is bounded to rule strings of at most 256 bytes and range lists
//     of at most 64 entries.
//   - `and` binds more tightly than `or`; both operators build left-associative
//     binary trees.
//   - A parse succeeds only when the complete input has been consumed.
//   - Decimal accumulation detects signed-integer overflow before mutation.
//
// Ownership/Lifetime:
//   - The parser borrows the input string for the duration of `parse_rule`.
//   - Every AST node and range array is allocated from the caller's locale
//     arena. Intermediate allocations made before a parse error remain owned
//     by that arena and are reclaimed with the rest of the locale data.
//
// Links: src/runtime/localization/rt_locale_manager.h (loader API),
//        src/runtime/localization/rt_locale_manager_internal.h (arena boundary),
//        src/runtime/localization/rt_locale_manager.c (arena implementation),
//        src/runtime/localization/rt_locale_data.h (AST node schema).
//
//===----------------------------------------------------------------------===//

#include "rt_locale_manager.h"
#include "rt_asset.h"
#include "rt_box.h"
#include "rt_bytes.h"
#include "rt_file_ext.h"
#include "rt_heap.h"
#include "rt_internal.h"
#include "rt_json.h"
#include "rt_list.h"
#include "rt_locale.h"
#include "rt_locale_data.h"
#include "rt_locale_platform.h"
#include "rt_map.h"
#include "rt_object.h"
#include "rt_platform.h"
#include "rt_seq.h"
#include "rt_string.h"
#include "rt_threads.h"
#include "rt_trap.h"
#include <ctype.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include "rt_locale_manager_internal.h"

/// @brief Mutable cursor and failure state for one plural-rule parse.
typedef struct rule_parser {
    /// Borrowed NUL-terminated rule text.
    const char *s;
    /// Current byte offset into @ref s.
    size_t pos;
    /// Valid byte length of @ref s, excluding its terminator.
    size_t len;
    /// Borrowed arena that owns all nodes produced by this parse.
    loc_arena_t *arena;
    /// Sticky flag set after the first syntax, bounds, or allocation failure.
    int failed;
} rule_parser_t;

/// @brief Advance the rule parser past any whitespace (space, tab, CR, LF).
/// @param p Active rule parser; must not be NULL.
static void rp_skip(rule_parser_t *p) {
    while (p->pos < p->len) {
        char c = p->s[p->pos];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
            break;
        ++p->pos;
    }
}

/// @brief Attempt to consume the keyword @p word from the rule parser.
/// @details Skips leading whitespace, then checks for an exact prefix match that
///          is not followed by an alphanumeric or underscore character (word boundary).
/// @param p    Active rule parser.
/// @param word Keyword to match (e.g., "and", "or", "in", "within").
/// @return 1 if the keyword was consumed; 0 if it was not present.
static int rp_word(rule_parser_t *p, const char *word) {
    rp_skip(p);
    size_t wl = strlen(word);
    if (p->pos + wl > p->len || memcmp(p->s + p->pos, word, wl) != 0)
        return 0;
    if (p->pos + wl < p->len) {
        char next = p->s[p->pos + wl];
        if ((next >= 'a' && next <= 'z') || (next >= 'A' && next <= 'Z') ||
            (next >= '0' && next <= '9') || next == '_')
            return 0;
    }
    p->pos += wl;
    return 1;
}

/// @brief Allocate a new rule AST node of the given @p kind from the arena.
/// @details Sets `p->failed = 1` and returns NULL on OOM.
/// @param p    Active rule parser.
/// @param kind The node kind (`RT_PRN_*`).
/// @return Pointer to a fresh, uninitialised node; NULL on OOM.
static rt_plural_rule_node_t *rp_node(rule_parser_t *p, rt_plural_rule_kind_t kind) {
    rt_plural_rule_node_t *n = (rt_plural_rule_node_t *)loc_arena_alloc(p->arena, sizeof(*n));
    if (!n) {
        p->failed = 1;
        return NULL;
    }
    n->kind = kind;
    return n;
}

/// @brief Parse a primary expression: an integer literal or an operand variable.
/// @details Handles numeric literals (RT_PRN_INT), variable names (`n`, `i`, `v`,
///          `f`, `t`) optionally followed by `mod N` (RT_PRN_VAR). Sets
///          `p->failed` on unrecognised input and returns NULL.
/// @param p Active rule parser.
/// @return A new literal or variable AST node; NULL on parse failure.
static rt_plural_rule_node_t *rp_expr(rule_parser_t *p) {
    rp_skip(p);
    if (p->pos >= p->len) {
        p->failed = 1;
        return NULL;
    }
    char c = p->s[p->pos];
    if (c >= '0' && c <= '9') {
        int64_t value = 0;
        while (p->pos < p->len && p->s[p->pos] >= '0' && p->s[p->pos] <= '9') {
            int d = p->s[p->pos++] - '0';
            if (value > (INT64_MAX - d) / 10) {
                p->failed = 1;
                return NULL;
            }
            value = value * 10 + d;
        }
        rt_plural_rule_node_t *n = rp_node(p, RT_PRN_INT);
        if (n)
            n->u.int_val = value;
        return n;
    }
    rt_plural_var_t var;
    if (c == 'n')
        var = RT_PVAR_N;
    else if (c == 'i')
        var = RT_PVAR_I;
    else if (c == 'v')
        var = RT_PVAR_V;
    else if (c == 'f')
        var = RT_PVAR_F;
    else if (c == 't')
        var = RT_PVAR_T;
    else {
        p->failed = 1;
        return NULL;
    }
    ++p->pos;
    int32_t mod = 0;
    if (rp_word(p, "mod")) {
        rp_skip(p);
        int64_t value = 0;
        int saw = 0;
        while (p->pos < p->len && p->s[p->pos] >= '0' && p->s[p->pos] <= '9') {
            saw = 1;
            int d = p->s[p->pos++] - '0';
            if (value > (INT32_MAX - d) / 10) {
                p->failed = 1;
                return NULL;
            }
            value = value * 10 + d;
        }
        if (!saw || value <= 0) {
            p->failed = 1;
            return NULL;
        }
        mod = (int32_t)value;
    }
    rt_plural_rule_node_t *n = rp_node(p, RT_PRN_VAR);
    if (n) {
        n->u.var.var = var;
        n->u.var.mod = mod;
    }
    return n;
}

/// @brief Consume an unsigned decimal integer from the rule parser into @p *out.
/// @details Overflow is detected and sets `p->failed`. Returns 0 (not consuming)
///          when the next character is not a digit.
/// @param p   Active rule parser.
/// @param out Written with the parsed value on success; may be NULL.
/// @return 1 if an integer was consumed; 0 if none was available.
static int rp_integer(rule_parser_t *p, int64_t *out) {
    rp_skip(p);
    int saw = 0;
    int64_t value = 0;
    while (p->pos < p->len && p->s[p->pos] >= '0' && p->s[p->pos] <= '9') {
        saw = 1;
        int d = p->s[p->pos++] - '0';
        if (value > (INT64_MAX - d) / 10) {
            p->failed = 1;
            return 0;
        }
        value = value * 10 + d;
    }
    if (!saw)
        return 0;
    if (out)
        *out = value;
    return 1;
}

/// @brief Parse a comma-separated list of integer ranges (e.g., "1, 3..5, 7").
/// @details Each range is either a single integer or `start..end`. The range list
///          starts with capacity 4 and is doubled as needed, capped at 64 entries.
///          Sets `p->failed` on any parse error.
/// @param p         Active rule parser.
/// @param out_count Written with the number of parsed ranges on success.
/// @return Arena-allocated array of `rt_plural_rule_range_t`; NULL on failure.
static rt_plural_rule_range_t *rp_range_list(rule_parser_t *p, size_t *out_count) {
    if (out_count)
        *out_count = 0;
    size_t cap = 4;
    size_t count = 0;
    rt_plural_rule_range_t *ranges =
        (rt_plural_rule_range_t *)loc_arena_alloc(p->arena, cap * sizeof(*ranges));
    if (!ranges) {
        p->failed = 1;
        return NULL;
    }

    while (!p->failed) {
        int64_t start = 0;
        if (!rp_integer(p, &start)) {
            p->failed = 1;
            return NULL;
        }
        int64_t end = start;
        rp_skip(p);
        if (p->pos + 1 < p->len && p->s[p->pos] == '.' && p->s[p->pos + 1] == '.') {
            p->pos += 2;
            if (!rp_integer(p, &end)) {
                p->failed = 1;
                return NULL;
            }
            if (end < start) {
                p->failed = 1;
                return NULL;
            }
        }
        if (count == cap) {
            size_t new_cap = cap * 2;
            if (new_cap > 64) {
                p->failed = 1;
                return NULL;
            }
            rt_plural_rule_range_t *grown =
                (rt_plural_rule_range_t *)loc_arena_alloc(p->arena, new_cap * sizeof(*grown));
            if (!grown) {
                p->failed = 1;
                return NULL;
            }
            memcpy(grown, ranges, count * sizeof(*grown));
            ranges = grown;
            cap = new_cap;
        }
        ranges[count].start = start;
        ranges[count].end = end;
        ++count;

        rp_skip(p);
        if (p->pos < p->len && p->s[p->pos] == ',') {
            ++p->pos;
            continue;
        }
        break;
    }

    if (out_count)
        *out_count = count;
    return ranges;
}

/// @brief Parse a comparison expression: `true`, or `<expr> <op> <rhs>`.
/// @details Handles `in`, `not in`, `within`, `not within`, `=`, and `!=`.
///          Range-based operators consume a range list; scalar operators consume
///          either a range list (collapsed to a WITHIN/NOT_WITHIN node) or a
///          single expression.
/// @param p Active rule parser.
/// @return A comparison AST node, or a literal RT_PRN_TRUE; NULL on failure.
static rt_plural_rule_node_t *rp_comparison(rule_parser_t *p) {
    if (rp_word(p, "true"))
        return rp_node(p, RT_PRN_TRUE);
    rt_plural_rule_node_t *left = rp_expr(p);
    rp_skip(p);
    rt_plural_rule_kind_t kind;
    int range_op = 0;
    if (rp_word(p, "not")) {
        if (rp_word(p, "within")) {
            kind = RT_PRN_NOT_WITHIN;
            range_op = 1;
        } else if (rp_word(p, "in")) {
            kind = RT_PRN_NOT_IN;
            range_op = 1;
        } else {
            p->failed = 1;
            return NULL;
        }
    } else if (rp_word(p, "within")) {
        kind = RT_PRN_WITHIN;
        range_op = 1;
    } else if (rp_word(p, "in")) {
        kind = RT_PRN_IN;
        range_op = 1;
    } else if (p->pos + 1 < p->len && p->s[p->pos] == '!' && p->s[p->pos + 1] == '=') {
        kind = RT_PRN_NE;
        p->pos += 2;
    } else if (p->pos < p->len && p->s[p->pos] == '=') {
        kind = RT_PRN_EQ;
        p->pos += 1;
    } else {
        p->failed = 1;
        return NULL;
    }

    if (range_op) {
        size_t range_count = 0;
        rt_plural_rule_range_t *ranges = rp_range_list(p, &range_count);
        if (!ranges || range_count == 0) {
            p->failed = 1;
            return NULL;
        }
        rt_plural_rule_node_t *n = rp_node(p, kind);
        if (n) {
            n->u.range.expr = left;
            n->u.range.ranges = ranges;
            n->u.range.range_count = range_count;
        }
        return n;
    }

    rp_skip(p);
    if (p->pos < p->len && p->s[p->pos] >= '0' && p->s[p->pos] <= '9') {
        size_t range_count = 0;
        rt_plural_rule_range_t *ranges = rp_range_list(p, &range_count);
        if (!ranges || range_count == 0) {
            p->failed = 1;
            return NULL;
        }
        rt_plural_rule_node_t *n =
            rp_node(p, kind == RT_PRN_NE ? RT_PRN_NOT_WITHIN : RT_PRN_WITHIN);
        if (n) {
            n->u.range.expr = left;
            n->u.range.ranges = ranges;
            n->u.range.range_count = range_count;
        }
        return n;
    }

    rt_plural_rule_node_t *right = rp_expr(p);
    rt_plural_rule_node_t *n = rp_node(p, kind);
    if (n) {
        n->u.bin.l = left;
        n->u.bin.r = right;
    }
    return n;
}

/// @brief Parse a left-associative `and`-chain of comparison expressions.
/// @details Builds a left-leaning tree of RT_PRN_AND nodes. Stops when no
///          `and` keyword follows the last comparison.
/// @param p Active rule parser.
/// @return Root of the AND sub-tree; NULL on failure.
static rt_plural_rule_node_t *rp_and(rule_parser_t *p) {
    rt_plural_rule_node_t *left = rp_comparison(p);
    while (!p->failed && rp_word(p, "and")) {
        rt_plural_rule_node_t *right = rp_comparison(p);
        rt_plural_rule_node_t *n = rp_node(p, RT_PRN_AND);
        if (!n)
            return left;
        n->u.bin.l = left;
        n->u.bin.r = right;
        left = n;
    }
    return left;
}

/// @brief Parse a left-associative `or`-chain of `and`-chains.
/// @details Builds a left-leaning tree of RT_PRN_OR nodes (lowest precedence level).
///          The CLDR grammar is: rule ::= or_chain; or_chain ::= and_chain ('or' and_chain)*.
/// @param p Active rule parser.
/// @return Root of the OR sub-tree; NULL on failure.
static rt_plural_rule_node_t *rp_or(rule_parser_t *p) {
    rt_plural_rule_node_t *left = rp_and(p);
    while (!p->failed && rp_word(p, "or")) {
        rt_plural_rule_node_t *right = rp_and(p);
        rt_plural_rule_node_t *n = rp_node(p, RT_PRN_OR);
        if (!n)
            return left;
        n->u.bin.l = left;
        n->u.bin.r = right;
        left = n;
    }
    return left;
}

/// @brief Parse a CLDR plural rule expression string into an AST.
/// @details Entry point for the recursive-descent parser. NULL @p rule is treated
///          as "true". Rules longer than 256 bytes or that fail to consume all input
///          return NULL. The returned AST is fully arena-owned.
/// @param arena Arena for all node allocations.
/// @param rule  NUL-terminated CLDR plural rule expression; NULL → "true".
/// @return Root AST node, or NULL if the expression is invalid or too long.
rt_plural_rule_node_t *parse_rule(loc_arena_t *arena, const char *rule) {
    if (!rule)
        rule = "true";
    size_t len = strlen(rule);
    if (len > 256)
        return NULL;
    rule_parser_t p = {rule, 0, len, arena, 0};
    rt_plural_rule_node_t *head = rp_or(&p);
    rp_skip(&p);
    if (p.failed || p.pos != p.len)
        return NULL;
    return head;
}
