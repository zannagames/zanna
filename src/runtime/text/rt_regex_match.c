//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/text/rt_regex_match.c
// Purpose: Backtracking matching engine for the Zanna regex AST. Runs a
//          compiled re_node tree against a subject string, with and without
//          capture-group tracking. Split out of rt_regex.c; shares AST types
//          and class primitives via rt_regex_internal.h.
//
// Key invariants:
//   - Matching is bounded by RE_MAX_STEPS (S-11 ReDoS guard); the engine
//     aborts a match attempt rather than backtracking unboundedly.
//   - re_find_match / re_find_match_with_groups are the engine entry points
//     consumed by the core's public rt_pattern_* API.
//   - The capture matcher uses continuations so groups and quantified groups
//     participate in backtracking; a repeated group reports its final
//     participating iteration.
//
// Ownership/Lifetime:
//   - Operates on caller-owned compiled patterns and text buffers; allocates
//     only transient position scratch arrays, freed before returning.
//
// Links: src/runtime/text/rt_regex.c (core/cache/public API),
//        src/runtime/text/rt_regex_parse.c (parser),
//        src/runtime/text/rt_regex_internal.h (shared engine types)
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_regex_match.c
 * @brief Implements bounded backtracking over compiled regex ASTs.
 * @details The engine evaluates literals, anchors, classes, concatenation,
 *          alternation, groups, and greedy or lazy quantifiers against borrowed
 *          byte strings. A shared step budget limits pathological
 *          backtracking, and capture-aware continuations restore ranges as
 *          alternative paths are explored.
 */

#include "rt_regex.h"
#include "rt_regex_internal.h"

#include "rt_internal.h"
#include "rt_trap.h"

#include <ctype.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

//=============================================================================
// Pattern Matching Engine (Backtracking)
//=============================================================================

/* S-11: Maximum backtracking steps before aborting (ReDoS guard) */
#define RE_MAX_STEPS 1000000

/// State shared by one non-capturing search, including its global step budget.
typedef struct {
    const char *text;
    int text_len;
    int start_pos; // Start position for this match attempt
    int steps;     // Backtracking step counter
    int max_steps; // Step limit (0 = unlimited)
} match_context;

// Forward declarations
static bool match_node(match_context *ctx, re_node *n, int pos, int *end_pos);
static bool match_concat_from(
    match_context *ctx, re_node **children, int count, int index, int pos, int *end_pos);
static int collect_node_positions(
    match_context *ctx, re_node *n, int pos, int *positions, int max_positions);

/// @brief Enumerate the end positions reachable by repeating a quantifier.
///
/// For `<child>{0..max}` (or `{1..max}` for `+`, or `{0..1}` for `?`),
/// greedily steps the cursor forward, recording each successful end
/// position. Returns the count. Detects zero-width matches and bails
/// (otherwise `()*` would loop forever). Used by both `match_quant`
/// (standalone) and `match_concat_from` (with backtracking).
///
/// @param ctx           Match context (text, length, step counter).
/// @param n             Quantifier node.
/// @param pos           Starting cursor.
/// @param positions     Out: array of reachable end positions.
/// @param max_positions Capacity of `positions`.
/// @return Number of end positions written.
static int collect_quant_positions(
    match_context *ctx, re_node *n, int pos, int *positions, int max_positions) {
    if (!positions || max_positions <= 0)
        return 0;

    re_node *child = n->data.quant.child;
    re_quant_type qtype = n->data.quant.qtype;

    int min_count = (qtype == QUANT_PLUS) ? 1 : 0;
    int max_count = (qtype == QUANT_QUEST) ? 1 : INT32_MAX;

    int num = 0;
    int cur_pos = pos;

    // Position for 0 matches (if allowed)
    if (min_count == 0 && num < max_positions)
        positions[num++] = pos;

    // Greedily collect match positions
    int count = 0;
    while (count < max_count && num < max_positions) {
        int child_end;
        if (match_node(ctx, child, cur_pos, &child_end)) {
            if (child_end == cur_pos)
                break; // Zero-width match; only count once
            cur_pos = child_end;
            count++;
            if (count >= min_count)
                positions[num++] = cur_pos;
        } else {
            break;
        }
    }

    return num;
}

/// @brief Allocate scratch storage for every possible end position.
/// @details Reserves one slot per remaining subject byte plus two boundary
///          slots. Invalid positions, arithmetic overflow, and allocation
///          failure trap.
/// @param text_len Total subject length in bytes.
/// @param pos Current byte position in `[0, text_len]`.
/// @param capacity_out Optional destination for element capacity; reset first.
/// @return Newly allocated integer array, or `NULL` after a trap.
static int *alloc_match_positions(int text_len, int pos, int *capacity_out) {
    if (capacity_out)
        *capacity_out = 0;
    if (pos < 0 || pos > text_len) {
        rt_trap("Pattern: invalid match position");
        return NULL;
    }
    size_t capacity = (size_t)(text_len - pos) + 2;
    if (capacity > (size_t)INT_MAX || capacity > SIZE_MAX / sizeof(int)) {
        rt_trap("Pattern: position allocation overflow");
        return NULL;
    }
    int *positions = (int *)malloc(sizeof(int) * capacity);
    if (!positions) {
        rt_trap("Pattern: memory allocation failed");
        return NULL;
    }
    if (capacity_out)
        *capacity_out = (int)capacity;
    return positions;
}

/// @brief Enumerate every end position reachable by matching `n` at `pos`.
///
/// Generalizes `collect_quant_positions` so concat-level backtracking can
/// see through groups (VDOC-056): a quantifier inside `(...)` must be able
/// to give bytes back to the syntax that follows the group. Alternations
/// and nested concats are enumerated with a dedup bitmap and emitted in
/// ascending order, matching the quantifier enumerator's convention
/// (greedy callers walk the list backwards). Falls back to a single
/// `match_node` attempt for simple nodes.
/// @param ctx Active non-capturing match context.
/// @param n AST node to evaluate.
/// @param pos Starting subject byte position.
/// @param positions Destination array for exclusive end positions.
/// @param max_positions Capacity of @p positions.
/// @return Number of end positions written in ascending order.
static int collect_node_positions(
    match_context *ctx, re_node *n, int pos, int *positions, int max_positions) {
    if (!positions || max_positions <= 0)
        return 0;

    switch (n->type) {
        case RE_QUANT:
            return collect_quant_positions(ctx, n, pos, positions, max_positions);

        case RE_GROUP:
            if (n->data.children.count > 0)
                return collect_node_positions(
                    ctx, n->data.children.children[0], pos, positions, max_positions);
            positions[0] = pos;
            return 1;

        case RE_ALT: {
            unsigned char *seen = (unsigned char *)calloc((size_t)ctx->text_len + 1, 1);
            if (!seen) {
                rt_trap("Pattern: memory allocation failed");
                return 0;
            }
            for (int i = 0; i < n->data.children.count; i++) {
                int cap = 0;
                int *tmp = alloc_match_positions(ctx->text_len, pos, &cap);
                if (!tmp) {
                    free(seen);
                    return 0;
                }
                int cnt =
                    collect_node_positions(ctx, n->data.children.children[i], pos, tmp, cap);
                for (int j = 0; j < cnt; j++)
                    if (tmp[j] >= pos && tmp[j] <= ctx->text_len)
                        seen[tmp[j]] = 1;
                free(tmp);
            }
            int num = 0;
            for (int e = pos; e <= ctx->text_len && num < max_positions; e++)
                if (seen[e])
                    positions[num++] = e;
            free(seen);
            return num;
        }

        case RE_CONCAT: {
            size_t set_len = (size_t)ctx->text_len + 1;
            unsigned char *cur = (unsigned char *)calloc(set_len, 1);
            unsigned char *nxt = (unsigned char *)calloc(set_len, 1);
            if (!cur || !nxt) {
                free(cur);
                free(nxt);
                rt_trap("Pattern: memory allocation failed");
                return 0;
            }
            cur[pos] = 1;
            bool any = true;
            for (int i = 0; i < n->data.children.count && any; i++) {
                memset(nxt, 0, set_len);
                any = false;
                for (int e = pos; e <= ctx->text_len; e++) {
                    if (!cur[e])
                        continue;
                    int cap = 0;
                    int *tmp = alloc_match_positions(ctx->text_len, e, &cap);
                    if (!tmp) {
                        free(cur);
                        free(nxt);
                        return 0;
                    }
                    int cnt =
                        collect_node_positions(ctx, n->data.children.children[i], e, tmp, cap);
                    for (int j = 0; j < cnt; j++) {
                        if (tmp[j] >= pos && tmp[j] <= ctx->text_len) {
                            nxt[tmp[j]] = 1;
                            any = true;
                        }
                    }
                    free(tmp);
                }
                unsigned char *swap = cur;
                cur = nxt;
                nxt = swap;
            }
            int num = 0;
            if (any) {
                for (int e = pos; e <= ctx->text_len && num < max_positions; e++)
                    if (cur[e])
                        positions[num++] = e;
            }
            free(cur);
            free(nxt);
            return num;
        }

        default: {
            int end;
            if (match_node(ctx, n, pos, &end)) {
                positions[0] = end;
                return 1;
            }
            return 0;
        }
    }
}

/// @brief Match a quantifier node when it has no following continuation.
///
/// Picks the longest (greedy) or shortest (lazy) reachable end without
/// any backtracking — there's no "what comes after" to consult. Used
/// when the quantifier is the entire pattern or the last child of a
/// concat. The full backtracking version lives in `match_concat_from`.
/// @param ctx Active non-capturing match context.
/// @param n Quantifier AST node.
/// @param pos Starting subject byte position.
/// @param end_pos Destination for the chosen exclusive end position.
/// @return `true` when the quantifier's minimum can be satisfied.
static bool match_quant(match_context *ctx, re_node *n, int pos, int *end_pos) {
    bool greedy = n->data.quant.greedy;

    int capacity = 0;
    int *positions = alloc_match_positions(ctx->text_len, pos, &capacity);
    if (!positions)
        return false;
    int num = collect_quant_positions(ctx, n, pos, positions, capacity);

    bool found = false;
    if (greedy) {
        // Try longest first
        if (num > 0) {
            *end_pos = positions[num - 1];
            found = true;
        }
    } else {
        // Try shortest first
        if (num > 0) {
            *end_pos = positions[0];
            found = true;
        }
    }

    free(positions);
    return found;
}

/// @brief Match a node against `ctx->text` starting at `pos`.
///
/// Returns true on success and writes the post-match cursor to
/// `*end_pos`. Implements the per-node-kind match logic via switch.
/// Increments the global step counter on each call and bails (returns
/// false) when the S-11 ReDoS cap is exceeded — this is what protects
/// the engine from catastrophic backtracking inputs.
/// @param ctx Active non-capturing match context.
/// @param n AST node to match; null is a successful empty expression.
/// @param pos Starting subject byte position.
/// @param end_pos Destination for the exclusive end position.
/// @return `true` on a successful node match, otherwise `false`.
static bool match_node(match_context *ctx, re_node *n, int pos, int *end_pos) {
    if (!ctx || !end_pos || pos < 0 || pos > ctx->text_len)
        return false;
    /* S-11: ReDoS guard — abort if step limit exceeded */
    if (ctx->max_steps > 0 && ++ctx->steps > ctx->max_steps) {
        *end_pos = pos;
        return false;
    }

    if (!n) {
        *end_pos = pos;
        return true;
    }

    switch (n->type) {
        case RE_LITERAL:
            if (pos < ctx->text_len && ctx->text[pos] == n->data.literal) {
                *end_pos = pos + 1;
                return true;
            }
            return false;

        case RE_DOT:
            if (pos < ctx->text_len && ctx->text[pos] != '\n') {
                *end_pos = pos + 1;
                return true;
            }
            return false;

        case RE_ANCHOR_START:
            if (pos == 0) {
                *end_pos = pos;
                return true;
            }
            return false;

        case RE_ANCHOR_END:
            if (pos == ctx->text_len) {
                *end_pos = pos;
                return true;
            }
            return false;

        case RE_CLASS:
            if (pos < ctx->text_len &&
                class_test(&n->data.char_class, (unsigned char)ctx->text[pos])) {
                *end_pos = pos + 1;
                return true;
            }
            return false;

        case RE_CONCAT:
            return match_concat_from(
                ctx, n->data.children.children, n->data.children.count, 0, pos, end_pos);

        case RE_ALT:
            for (int i = 0; i < n->data.children.count; i++) {
                int child_end;
                if (match_node(ctx, n->data.children.children[i], pos, &child_end)) {
                    *end_pos = child_end;
                    return true;
                }
            }
            return false;

        case RE_GROUP:
            if (n->data.children.count > 0) {
                return match_node(ctx, n->data.children.children[0], pos, end_pos);
            }
            *end_pos = pos;
            return true;

        case RE_QUANT:
            return match_quant(ctx, n, pos, end_pos);
    }

    return false;
}

/// @brief Match the tail of a concat sequence starting at `children[index]`.
///
/// The interesting case is a quantifier child: we enumerate every
/// reachable end position and try each one in greedy/lazy order,
/// recursing for the remaining children. This is the backtracking
/// loop — most patterns terminate quickly, but the S-11 step cap in
/// `match_node` ensures even pathological cases bail eventually.
/// Non-quantifier children are matched once; if they succeed we tail
/// into the next child.
/// @param ctx Active non-capturing match context.
/// @param children Ordered AST child array.
/// @param count Number of children.
/// @param index Index of the next child to match.
/// @param pos Current subject byte position.
/// @param end_pos Destination for the final exclusive end position.
/// @return `true` when the remaining sequence matches.
static bool match_concat_from(
    match_context *ctx, re_node **children, int count, int index, int pos, int *end_pos) {
    if (index >= count) {
        *end_pos = pos;
        return true;
    }

    re_node *child = children[index];

    if (child->type == RE_QUANT || child->type == RE_GROUP) {
        // Groups are enumerated like quantifiers so a quantifier inside
        // parentheses can backtrack against the following syntax (VDOC-056).
        bool greedy = child->type == RE_QUANT ? child->data.quant.greedy : true;

    int capacity = 0;
    int *positions = alloc_match_positions(ctx->text_len, pos, &capacity);
    if (!positions)
        return false;
    int num = collect_node_positions(ctx, child, pos, positions, capacity);

        bool found = false;
        if (greedy) {
            // Try longest match first, backtrack to shorter
            for (int i = num - 1; i >= 0; i--) {
                if (match_concat_from(ctx, children, count, index + 1, positions[i], end_pos)) {
                    found = true;
                    break;
                }
            }
        } else {
            // Try shortest match first
            for (int i = 0; i < num; i++) {
                if (match_concat_from(ctx, children, count, index + 1, positions[i], end_pos)) {
                    found = true;
                    break;
                }
            }
        }

        free(positions);
        return found;
    } else {
        // Non-quantifier child: single match attempt
        int child_end;
        if (match_node(ctx, child, pos, &child_end))
            return match_concat_from(ctx, children, count, index + 1, child_end, end_pos);
        return false;
    }
}

/// @brief Scan the text for the first match starting at or after `start_from`.
///
/// Walks the cursor forward one byte at a time, attempting `match_node`
/// at each position. Caps the total search step count via `RE_MAX_STEPS`
/// (S-11). Returns true on first hit with start/end set.
/// @param cp Borrowed compiled pattern.
/// @param text Borrowed subject byte buffer.
/// @param text_len Number of subject bytes.
/// @param start_from First candidate byte position.
/// @param match_start Destination for the inclusive match start.
/// @param match_end Destination for the exclusive match end.
/// @return `true` for the first successful match, otherwise `false`.
static bool find_match(compiled_pattern *cp,
                       const char *text,
                       int text_len,
                       int start_from,
                       int *match_start,
                       int *match_end) {
    match_context ctx = {text, text_len, 0, 0, RE_MAX_STEPS};

    for (int i = start_from; i <= text_len; i++) {
        ctx.start_pos = i;
        int end_pos;
        if (match_node(&ctx, cp->root, i, &end_pos)) {
            *match_start = i;
            *match_end = end_pos;
            return true;
        }
    }
    return false;
}

/// @brief Public `find_match` exposed via `rt_regex_internal.h`.
///
/// Wraps the static helper for the cached-pattern API and any other
/// in-process consumer that holds a compiled pattern directly.
/// @param cp Borrowed compiled pattern.
/// @param text Borrowed subject byte buffer.
/// @param text_len Number of subject bytes.
/// @param start_from First candidate byte position.
/// @param match_start Destination for the inclusive match start.
/// @param match_end Destination for the exclusive match end.
/// @return `true` if a match is found within the shared step budget.
bool re_find_match(re_compiled_pattern *cp,
                   const char *text,
                   int text_len,
                   int start_from,
                   int *match_start,
                   int *match_end) {
    return find_match(cp, text, text_len, start_from, match_start, match_end);
}

//-----------------------------------------------------------------------------
// Capture Group Support
//-----------------------------------------------------------------------------

/* Depth cap for the recursive capture matcher (bounds C stack usage). */
#define RE_MAX_CAPTURE_DEPTH 4096

/// @brief Match context augmented with capture-group tracking arrays.
///
/// Pairs the standard text/length/start with caller-provided arrays for
/// group start/end positions (indexed by the lexical `group_index`
/// assigned at parse time). `accept_end` receives the overall match end
/// when the accept continuation fires.
typedef struct {
    const char *text;
    int text_len;
    int start_pos;
    int *group_starts;
    int *group_ends;
    int max_groups;
    int steps;
    int max_steps;
    int depth;
    int accept_end;
} match_context_groups;

/// @brief Continuation frame for the capture matcher.
///
/// The capture matcher is continuation-passing so every construct —
/// including groups — backtracks against the syntax that follows it,
/// giving it the same match language as the plain matcher (VDOC-056/057).
/// One frame type serves all constructs; the meaning of the int fields
/// depends on `fn` (seq: children/count/index; quant: count=iterations,
/// index=previous position; group: aux=group start).
typedef struct gcont gcont;
struct gcont {
    /// @brief Resume one capture-aware matcher continuation frame.
    /// @param ctx Mutable capture-matching context.
    /// @param pos Current input position.
    /// @param self Current continuation frame.
    /// @return `true` when this frame and its successor match.
    bool (*fn)(match_context_groups *ctx, int pos, const gcont *self);
    const gcont *next;
    re_node **children;
    int count;
    int index;
    re_node *node;
    int aux;
};

static bool match_node_g(match_context_groups *ctx, re_node *n, int pos, const gcont *k);
static bool match_quant_g(
    match_context_groups *ctx, re_node *n, int matched, int pos, const gcont *k);

/// @brief Invoke a capture matcher's continuation.
/// @param ctx Active capture-tracking context.
/// @param pos Current subject byte position.
/// @param k Continuation frame to invoke.
/// @return Result reported by the continuation.
static bool gapply(match_context_groups *ctx, int pos, const gcont *k) {
    return k->fn(ctx, pos, k);
}

/// @brief Terminal continuation: the whole pattern matched ending at `pos`.
/// @param ctx Capture context whose accepted end is updated.
/// @param pos Exclusive full-match end position.
/// @param self Terminal continuation frame; unused.
/// @return Always `true`.
static bool gc_accept(match_context_groups *ctx, int pos, const gcont *self) {
    (void)self;
    ctx->accept_end = pos;
    return true;
}

static bool match_seq_g(
    match_context_groups *ctx, re_node **children, int count, int index, int pos, const gcont *k);

/// @brief Resume a sequence after one child has matched.
/// @param ctx Active capture-tracking context.
/// @param pos Exclusive end of the preceding child.
/// @param self Continuation frame describing the remaining children.
/// @return Whether the remaining sequence and outer continuation match.
static bool gc_seq(match_context_groups *ctx, int pos, const gcont *self) {
    return match_seq_g(ctx, self->children, self->count, self->index, pos, self->next);
}

/// @brief Match a capture-aware sequence from a selected child index.
/// @param ctx Active capture-tracking context.
/// @param children Ordered AST child array.
/// @param count Number of children.
/// @param index Index of the next child.
/// @param pos Current subject byte position.
/// @param k Continuation after the complete sequence.
/// @return Whether the remaining sequence and continuation match.
static bool match_seq_g(
    match_context_groups *ctx, re_node **children, int count, int index, int pos, const gcont *k) {
    if (index >= count)
        return gapply(ctx, pos, k);
    gcont kk = {gc_seq, k, children, count, index + 1, NULL, 0};
    return match_node_g(ctx, children[index], pos, &kk);
}

/// @brief Continuation after one quantifier child match: iterate or stop.
/// @details A zero-width child stops repetition immediately to guarantee
///          progress.
/// @param ctx Active capture-tracking context.
/// @param pos Exclusive end of the repeated child.
/// @param self Frame containing the previous position and repetition count.
/// @return Whether further repetition or the outer continuation succeeds.
static bool gc_quant(match_context_groups *ctx, int pos, const gcont *self) {
    if (pos == self->index) {
        // Zero-width child match: no progress; stop iterating.
        return gapply(ctx, pos, self->next);
    }
    return match_quant_g(ctx, self->node, self->count, pos, self->next);
}

/// @brief True for quantifier children that always consume exactly one byte.
/// @param child AST node to classify.
/// @return `true` for literal, class, or dot nodes.
static bool quant_child_simple(const re_node *child) {
    return child->type == RE_LITERAL || child->type == RE_CLASS || child->type == RE_DOT;
}

/// @brief Test a single-byte node at a subject position without continuations.
/// @param ctx Capture context containing the subject.
/// @param child Literal, dot, or character-class node.
/// @param pos Candidate byte position.
/// @return `true` when @p child consumes the byte at @p pos.
static bool simple_match_at(const match_context_groups *ctx, const re_node *child, int pos) {
    if (pos >= ctx->text_len)
        return false;
    unsigned char ch = (unsigned char)ctx->text[pos];
    switch (child->type) {
        case RE_LITERAL:
            return (char)ch == child->data.literal;
        case RE_DOT:
            return ch != '\n';
        case RE_CLASS:
            return class_test(&child->data.char_class, ch);
        default:
            return false;
    }
}

/// @brief Quantifier matching with full backtracking against the continuation.
///
/// Single-byte children (literal/class/dot — no captures inside) use an
/// iterative run-length fast path so `(\d+)`-style patterns never deepen
/// the C stack per repetition. Complex children recurse through `gc_quant`
/// frames, bounded by RE_MAX_CAPTURE_DEPTH.
/// @param ctx Active capture-tracking context.
/// @param n Quantifier AST node.
/// @param matched Number of child repetitions already completed.
/// @param pos Current subject byte position.
/// @param k Continuation following the quantifier.
/// @return Whether some permitted repetition count satisfies @p k.
static bool match_quant_g(
    match_context_groups *ctx, re_node *n, int matched, int pos, const gcont *k) {
    re_node *child = n->data.quant.child;
    re_quant_type qtype = n->data.quant.qtype;
    bool greedy = n->data.quant.greedy;

    int min_count = (qtype == QUANT_PLUS) ? 1 : 0;
    int max_count = (qtype == QUANT_QUEST) ? 1 : INT32_MAX;

    if (quant_child_simple(child)) {
        int extra = 0;
        while (matched + extra < max_count && simple_match_at(ctx, child, pos + extra))
            extra++;
        if (greedy) {
            for (int e = extra; e >= 0; e--) {
                if (matched + e < min_count)
                    break;
                if (ctx->max_steps > 0 && ++ctx->steps > ctx->max_steps)
                    return false;
                if (gapply(ctx, pos + e, k))
                    return true;
            }
        } else {
            for (int e = 0; e <= extra; e++) {
                if (matched + e < min_count)
                    continue;
                if (ctx->max_steps > 0 && ++ctx->steps > ctx->max_steps)
                    return false;
                if (gapply(ctx, pos + e, k))
                    return true;
            }
        }
        return false;
    }

    if (matched < max_count) {
        gcont kk = {gc_quant, k, NULL, matched + 1, pos, n, 0};
        if (greedy) {
            if (match_node_g(ctx, child, pos, &kk))
                return true;
            return matched >= min_count ? gapply(ctx, pos, k) : false;
        }
        if (matched >= min_count && gapply(ctx, pos, k))
            return true;
        return match_node_g(ctx, child, pos, &kk);
    }
    return matched >= min_count ? gapply(ctx, pos, k) : false;
}

/// @brief Continuation closing a capture group: record the span, restore on
///        backtrack so failed branches leave no stale captures.
/// @param ctx Capture context and output arrays to update.
/// @param pos Exclusive group end position.
/// @param self Frame containing the group node, start, and next continuation.
/// @return Whether the continuation after the group succeeds.
static bool gc_group_end(match_context_groups *ctx, int pos, const gcont *self) {
    int idx = self->node->group_index;
    if (idx >= 0 && idx < ctx->max_groups) {
        int saved_start = ctx->group_starts[idx];
        int saved_end = ctx->group_ends[idx];
        ctx->group_starts[idx] = self->aux;
        ctx->group_ends[idx] = pos;
        if (gapply(ctx, pos, self->next))
            return true;
        ctx->group_starts[idx] = saved_start;
        ctx->group_ends[idx] = saved_end;
        return false;
    }
    return gapply(ctx, pos, self->next);
}

/// @brief Dispatch one capture-aware AST node after common guards.
/// @param ctx Active capture-tracking context.
/// @param n Non-null AST node to match.
/// @param pos Starting subject byte position.
/// @param k Continuation to invoke after the node.
/// @return Whether the node and continuation match.
static bool match_node_g_inner(match_context_groups *ctx, re_node *n, int pos, const gcont *k) {
    switch (n->type) {
        case RE_LITERAL:
            if (pos < ctx->text_len && ctx->text[pos] == n->data.literal)
                return gapply(ctx, pos + 1, k);
            return false;

        case RE_DOT:
            if (pos < ctx->text_len && ctx->text[pos] != '\n')
                return gapply(ctx, pos + 1, k);
            return false;

        case RE_ANCHOR_START:
            return pos == 0 && gapply(ctx, pos, k);

        case RE_ANCHOR_END:
            return pos == ctx->text_len && gapply(ctx, pos, k);

        case RE_CLASS:
            if (pos < ctx->text_len &&
                class_test(&n->data.char_class, (unsigned char)ctx->text[pos]))
                return gapply(ctx, pos + 1, k);
            return false;

        case RE_CONCAT:
            return match_seq_g(ctx, n->data.children.children, n->data.children.count, 0, pos, k);

        case RE_ALT:
            for (int i = 0; i < n->data.children.count; i++) {
                if (match_node_g(ctx, n->data.children.children[i], pos, k))
                    return true;
            }
            return false;

        case RE_GROUP: {
            gcont kk = {gc_group_end, k, NULL, 0, 0, n, pos};
            if (n->data.children.count > 0)
                return match_node_g(ctx, n->data.children.children[0], pos, &kk);
            return gc_group_end(ctx, pos, &kk);
        }

        case RE_QUANT:
            return match_quant_g(ctx, n, 0, pos, k);
    }
    return false;
}

/// @brief Capture-tracking matcher entry: guards, then dispatch.
/// @details Enforces both the global step budget and recursive depth cap. A
///          null node acts as an empty expression and immediately continues.
/// @param ctx Active capture-tracking context.
/// @param n AST node to match, or null.
/// @param pos Starting subject byte position.
/// @param k Continuation to invoke after the node.
/// @return Whether the node and continuation match within configured bounds.
static bool match_node_g(match_context_groups *ctx, re_node *n, int pos, const gcont *k) {
    if (!ctx || pos < 0 || pos > ctx->text_len)
        return false;
    if (ctx->max_steps > 0 && ++ctx->steps > ctx->max_steps)
        return false;
    if (ctx->depth >= RE_MAX_CAPTURE_DEPTH)
        return false;
    if (!n)
        return gapply(ctx, pos, k);
    ctx->depth++;
    bool ok = match_node_g_inner(ctx, n, pos, k);
    ctx->depth--;
    return ok;
}

/// @brief Group-tracking version of `find_match`.
///
/// Same scan loop as the plain matcher, but runs the CPS capture matcher
/// so groups backtrack exactly like ordinary syntax and capture indexes
/// follow lexical (opening-parenthesis) numbering. On success,
/// `*num_groups` is the pattern's group count (clamped to `max_groups`);
/// groups that did not participate in the match report start/end -1.
/// @param cp Borrowed compiled pattern.
/// @param text Borrowed subject byte buffer.
/// @param text_len Number of subject bytes.
/// @param start_from First candidate byte position.
/// @param match_start Destination for the inclusive full-match start.
/// @param match_end Destination for the exclusive full-match end.
/// @param group_starts Capture-start output array.
/// @param group_ends Capture-end output array.
/// @param max_groups Capacity of each capture array.
/// @param num_groups Destination for the reported capture count.
/// @return `true` for the first match found within step and depth limits.
static bool find_match_groups(compiled_pattern *cp,
                              const char *text,
                              int text_len,
                              int start_from,
                              int *match_start,
                              int *match_end,
                              int *group_starts,
                              int *group_ends,
                              int max_groups,
                              int *num_groups) {
    match_context_groups ctx = {
        text, text_len, 0, group_starts, group_ends, max_groups, 0, RE_MAX_STEPS, 0, 0};
    int report_groups = cp->group_count < max_groups ? cp->group_count : max_groups;

    for (int i = start_from; i <= text_len; i++) {
        for (int g = 0; g < max_groups; g++) {
            group_starts[g] = -1;
            group_ends[g] = -1;
        }
        ctx.start_pos = i;
        gcont accept = {gc_accept, NULL, NULL, 0, 0, NULL, 0};
        if (match_node_g(&ctx, cp->root, i, &accept)) {
            *match_start = i;
            *match_end = ctx.accept_end;
            *num_groups = report_groups;
            return true;
        }
    }
    *num_groups = 0;
    return false;
}

/// @brief Public group-capturing find — exposed via `rt_regex_internal.h`.
///
/// Wraps `find_match_groups` for callers that hold a compiled pattern
/// directly (cached-pattern wrapper, replace-with-references helpers,
/// etc.).
/// @param cp Borrowed compiled pattern.
/// @param text Borrowed subject byte buffer.
/// @param text_len Number of subject bytes.
/// @param start_from First candidate byte position.
/// @param match_start Destination for the inclusive full-match start.
/// @param match_end Destination for the exclusive full-match end.
/// @param group_starts Preallocated capture-start array.
/// @param group_ends Preallocated capture-end array.
/// @param max_groups Capacity of each capture array.
/// @param num_groups Destination for the reported capture count.
/// @return `true` if a match is found within the engine bounds.
bool re_find_match_with_groups(re_compiled_pattern *cp,
                               const char *text,
                               int text_len,
                               int start_from,
                               int *match_start,
                               int *match_end,
                               int *group_starts,
                               int *group_ends,
                               int max_groups,
                               int *num_groups) {
    return find_match_groups(cp,
                             text,
                             text_len,
                             start_from,
                             match_start,
                             match_end,
                             group_starts,
                             group_ends,
                             max_groups,
                             num_groups);
}
