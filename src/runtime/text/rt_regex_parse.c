//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/text/rt_regex_parse.c
// Purpose: Recursive-descent parser for the Zanna regex engine. Turns a
//          pattern source string into an AST of re_node values (literals,
//          classes, groups, quantifiers, alternation). Split out of
//          rt_regex.c; shares AST types and node/class primitives via
//          rt_regex_internal.h.
//
// Key invariants:
//   - Malformed syntax is recorded on parser_state; owned partial subtrees are
//     released and the caller chooses trapping or diagnostic-return behavior.
//   - Node allocation/teardown is owned by the core (node_new/node_free);
//     this file only assembles nodes.
//   - parse_alternation is the grammar entry point used by compile_pattern.
//
// Ownership/Lifetime:
//   - Returned re_node trees are owned by the enclosing compiled_pattern.
//
// Links: src/runtime/text/rt_regex.c (core/compile/cache),
//        src/runtime/text/rt_regex_match.c (matching engine),
//        src/runtime/text/rt_regex_internal.h (shared engine types)
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_regex_parse.c
 * @brief Implements recursive-descent parsing for the in-tree regex syntax.
 * @details Pattern bytes are assembled into owned AST nodes for literals,
 *          anchors, character classes, groups, quantifiers, concatenation, and
 *          alternation. Syntax failures report their cursor position and
 *          release any partially constructed subtrees before returning.
 */

#include "rt_regex_internal.h"

#include <ctype.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

//=============================================================================
// Pattern Parser
//=============================================================================

/// @brief Return the next byte without advancing; '\\0' at EOF.
/// @param p Parser state to inspect.
/// @return Current pattern byte, or the null sentinel at end-of-input.
static char peek(parser_state *p) {
    return p->pos < p->len ? p->src[p->pos] : '\0';
}

/// @brief Consume and return the next byte; '\\0' at EOF.
/// @param p Parser state to advance.
/// @return Consumed pattern byte, or the null sentinel at end-of-input.
static char advance(parser_state *p) {
    return p->pos < p->len ? p->src[p->pos++] : '\0';
}

/// @brief True if the parser cursor is past the end of the pattern.
/// @param p Parser state to inspect.
/// @return `true` when @p p has no source bytes remaining.
bool at_end(parser_state *p) {
    return p->pos >= p->len;
}

/// @brief Record the first contextual parse error without trapping.
/// @details Compilation has both a runtime path that traps and an interactive
///          path that reports malformed input. Keeping parsing non-trapping
///          prevents editor validation from needing setjmp-based recovery.
/// @param p Parser state supplying the current byte position.
/// @param msg Null-terminated diagnostic detail.
void parse_error(parser_state *p, const char *msg) {
    if (!p || p->error_message)
        return;
    p->error_message = msg ? msg : "invalid syntax";
    p->error_position = p->pos;
}

/// @brief Parse a `[...]` character class (cursor positioned after the `[`).
///
/// Handles:
///   - Leading `^` for negation.
///   - Escape sequences (`\\n`, `\\d`, etc.).
///   - Ranges (`a-z`).
///   - Literal `-` when at end (`[abc-]`).
/// Traps via `parse_error` if `]` is missing.
/// @param p Parser cursor positioned immediately after the opening bracket.
/// @return Newly allocated character-class node.
static re_node *parse_class(parser_state *p) {
    re_node *n = node_new(RE_CLASS);
    if (!n)
        return NULL;
    memset(n->data.char_class.bits, 0, sizeof(n->data.char_class.bits));
    n->data.char_class.negated = false;

    // Check for negation
    if (peek(p) == '^') {
        n->data.char_class.negated = true;
        advance(p);
    }

    bool first = true;
    while (!at_end(p) && (first || peek(p) != ']')) {
        first = false;
        char c = advance(p);

        if (c == '\\' && !at_end(p)) {
            char esc = advance(p);
            switch (esc) {
                case 'd':
                case 'D':
                case 'w':
                case 'W':
                case 's':
                case 'S':
                    class_add_shorthand(&n->data.char_class, esc);
                    break;
                case 'n':
                    class_set(&n->data.char_class, '\n');
                    break;
                case 'r':
                    class_set(&n->data.char_class, '\r');
                    break;
                case 't':
                    class_set(&n->data.char_class, '\t');
                    break;
                default:
                    class_set(&n->data.char_class, (unsigned char)esc);
                    break;
            }
        } else if (peek(p) == '-' && p->pos + 1 < p->len && p->src[p->pos + 1] != ']') {
            // Range: a-z
            advance(p); // consume -
            char end = advance(p);
            if (end == '\\' && !at_end(p)) {
                end = advance(p);
                // Handle escape in range end
                switch (end) {
                    case 'n':
                        end = '\n';
                        break;
                    case 'r':
                        end = '\r';
                        break;
                    case 't':
                        end = '\t';
                        break;
                }
            }
            class_add_range(&n->data.char_class, (unsigned char)c, (unsigned char)end);
        } else {
            class_set(&n->data.char_class, (unsigned char)c);
        }
    }

    if (peek(p) != ']') {
        node_free(n);
        parse_error(p, "unclosed character class");
        return NULL;
    }
    advance(p); // consume ]

    return n;
}

/// @brief Parse a single atom: literal char, escape, `.`, anchor, class, or group.
///
/// Returns NULL when the cursor is at a quantifier or alternation
/// boundary (`* + ? | )` or EOF) — the caller treats that as
/// "no more atoms in this concat". Group atoms recursively invoke
/// `parse_alternation` for the body.
/// @param p Parser state to consume and update.
/// @return Newly allocated atom node, or `NULL` at an expression boundary.
static re_node *parse_atom(parser_state *p) {
    char c = peek(p);

    if (c == '\\') {
        advance(p);
        if (at_end(p)) {
            parse_error(p, "trailing backslash");
            return NULL;
        }

        char esc = advance(p);
        switch (esc) {
            case 'd':
            case 'D':
            case 'w':
            case 'W':
            case 's':
            case 'S': {
                re_node *n = node_new(RE_CLASS);
                memset(n->data.char_class.bits, 0, sizeof(n->data.char_class.bits));
                n->data.char_class.negated = false;
                class_add_shorthand(&n->data.char_class, esc);
                return n;
            }
            case 'n': {
                re_node *n = node_new(RE_LITERAL);
                n->data.literal = '\n';
                return n;
            }
            case 'r': {
                re_node *n = node_new(RE_LITERAL);
                n->data.literal = '\r';
                return n;
            }
            case 't': {
                re_node *n = node_new(RE_LITERAL);
                n->data.literal = '\t';
                return n;
            }
            default: {
                // Escaped special char or literal
                re_node *n = node_new(RE_LITERAL);
                n->data.literal = esc;
                return n;
            }
        }
    } else if (c == '.') {
        advance(p);
        return node_new(RE_DOT);
    } else if (c == '^') {
        advance(p);
        return node_new(RE_ANCHOR_START);
    } else if (c == '$') {
        advance(p);
        return node_new(RE_ANCHOR_END);
    } else if (c == '[') {
        advance(p);
        return parse_class(p);
    } else if (c == '(') {
        advance(p);
        // Claim the capture index before parsing the body so nested groups
        // number by opening parenthesis (PCRE convention).
        int group_index = p->group_counter++;
        re_node *inner = parse_alternation(p);
        if (p->error_message) {
            node_free(inner);
            return NULL;
        }
        if (peek(p) != ')') {
            node_free(inner);
            parse_error(p, "unclosed group");
            return NULL;
        }
        advance(p);
        re_node *group = node_new(RE_GROUP);
        if (!group) {
            node_free(inner);
            return NULL;
        }
        group->group_index = group_index;
        if (!inner)
            inner = node_new(RE_CONCAT);
        if (!inner) {
            node_free(group);
            return NULL;
        }
        children_add(group, inner);
        return group;
    } else if (c == ')' || c == '|' || c == '*' || c == '+' || c == '?' || c == '\0') {
        // These end an atom
        return NULL;
    } else {
        advance(p);
        re_node *n = node_new(RE_LITERAL);
        n->data.literal = c;
        return n;
    }
}

/// @brief Parse an atom plus optional quantifier (`* + ?` with optional `?` for non-greedy).
///
/// Wraps the atom in an `RE_QUANT` node when a quantifier follows;
/// otherwise returns the bare atom. The trailing `?` after a quantifier
/// flips it to non-greedy mode.
/// @param p Parser state to consume and update.
/// @return Newly allocated quantified or bare atom, or `NULL` when no atom
///         begins at the cursor.
static re_node *parse_quantified(parser_state *p) {
    re_node *atom = parse_atom(p);
    if (p->error_message)
        return NULL;
    if (!atom)
        return NULL;

    char c = peek(p);
    if (c == '*' || c == '+' || c == '?') {
        advance(p);
        re_node *q = node_new(RE_QUANT);
        q->data.quant.child = atom;
        q->data.quant.greedy = true;

        switch (c) {
            case '*':
                q->data.quant.qtype = QUANT_STAR;
                break;
            case '+':
                q->data.quant.qtype = QUANT_PLUS;
                break;
            case '?':
                q->data.quant.qtype = QUANT_QUEST;
                break;
        }

        // Check for non-greedy modifier
        if (peek(p) == '?') {
            advance(p);
            q->data.quant.greedy = false;
        }

        return q;
    }

    return atom;
}

/// @brief Parse a sequence of quantified atoms into a concat node.
///
/// Stops at `)`, `|`, or EOF. As an optimization, the result is
/// flattened: empty concat returns NULL, single-child concat returns
/// just the child (avoids one indirection in the matcher).
/// @param p Parser state to consume and update.
/// @return Owned concat subtree, a single owned child, or `NULL` for an empty
///         sequence.
static re_node *parse_concat(parser_state *p) {
    re_node *concat = node_new(RE_CONCAT);
    if (!concat)
        return NULL;

    while (!at_end(p)) {
        char c = peek(p);
        if (c == ')' || c == '|')
            break;

        re_node *child = parse_quantified(p);
        if (p->error_message) {
            node_free(concat);
            return NULL;
        }
        if (!child)
            break;

        children_add(concat, child);
    }

    // Simplify single-child concat
    if (concat->data.children.count == 0) {
        node_free(concat);
        return NULL;
    }
    if (concat->data.children.count == 1) {
        re_node *child = concat->data.children.children[0];
        concat->data.children.children[0] = NULL;
        concat->data.children.count = 0;
        node_free(concat);
        return child;
    }

    return concat;
}

/// @brief Parse an alternation `a|b|c` (top of the recursive-descent grammar).
///
/// Empty alternatives (e.g., `(|x)`) get an explicit empty `RE_CONCAT`
/// branch so they can match the empty string. Single-branch
/// alternations are flattened to just the branch (matcher optimization).
/// @param p Parser state to consume and update.
/// @return Owned alternation subtree, a single owned branch, or `NULL` for an
///         entirely empty expression.
re_node *parse_alternation(parser_state *p) {
    re_node *first = parse_concat(p);
    if (p->error_message)
        return NULL;

    if (peek(p) != '|') {
        return first;
    }

    re_node *alt = node_new(RE_ALT);
    if (first)
        children_add(alt, first);
    else {
        // Empty alternative (matches empty string)
        children_add(alt, node_new(RE_CONCAT));
    }

    while (peek(p) == '|') {
        advance(p); // consume |
        re_node *branch = parse_concat(p);
        if (p->error_message) {
            node_free(alt);
            return NULL;
        }
        if (branch)
            children_add(alt, branch);
        else
            children_add(alt, node_new(RE_CONCAT));
    }

    // Simplify single-branch alternation
    if (alt->data.children.count == 1) {
        re_node *child = alt->data.children.children[0];
        alt->data.children.children[0] = NULL;
        alt->data.children.count = 0;
        node_free(alt);
        return child;
    }

    return alt;
}
