//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/text/rt_regex_internal.h
// Purpose: Defines the internal regex AST, parser cursor, compilation API, and
//          backtracking matcher entry points shared by text-runtime modules.
//
// Key invariants:
//   - This header is internal; it must not be included by code outside the text/ directory.
//   - Compiled patterns own a parsed AST and source-pattern copy.
//   - re_compile is the shared compilation entry point.
//   - re_find_match and re_find_match_with_groups run the backtracking matcher.
//
// Ownership/Lifetime:
//   - Compiled regex objects are owned by their enclosing rt_regex or rt_compiled_pattern.
//   - No direct public ownership semantics; accessed only through the wrapper APIs.
//
// Links: src/runtime/text/rt_regex.c, src/runtime/text/rt_compiled_pattern.c (internal users)
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_regex_internal.h
 * @brief Defines the private regex AST, parser, compile, and match interfaces.
 * @details Internal text modules share node and character-class layouts,
 *          parser cursor state, compiled-pattern ownership, recursive-descent
 *          construction hooks, and bounded backtracking entry points with
 *          optional capture-group ranges.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Opaque declaration used by the internal compile and match API.
typedef struct re_compiled_pattern re_compiled_pattern;

/// @brief Compile a pattern string into an internal representation.
/// @details Parses the complete null-terminated source and counts explicit
///          capture groups. Empty input compiles as a zero-width expression.
/// @param pattern Required null-terminated regex source.
/// @return Newly allocated compiled pattern, or `NULL` after a syntax or
///         allocation trap.
re_compiled_pattern *re_compile(const char *pattern);

/// @brief Free a compiled pattern.
/// @details Recursively destroys the AST and duplicated source. Safe on null.
/// @param cp Owned compiled pattern to free.
void re_free(re_compiled_pattern *cp);

/// @brief Get the pattern string from a compiled pattern.
/// @param cp Borrowed compiled pattern, or null.
/// @return Borrowed original pattern string, or a stable empty string for null.
const char *re_get_pattern(re_compiled_pattern *cp);

/// @brief Find a match in text, returning start and end positions.
/// @details Positions are byte offsets. Search is left-to-right unless the
///          compiled expression begins with a start anchor.
/// @param cp Borrowed compiled pattern.
/// @param text Borrowed subject byte buffer.
/// @param text_len Number of subject bytes.
/// @param start_from First candidate byte position.
/// @param match_start Destination for the inclusive match start.
/// @param match_end Destination for the exclusive match end.
/// @return `true` if a match is found.
bool re_find_match(re_compiled_pattern *cp,
                   const char *text,
                   int text_len,
                   int start_from,
                   int *match_start,
                   int *match_end);

/// @brief Find a match and capture groups.
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
/// @return `true` if a match is found.
bool re_find_match_with_groups(re_compiled_pattern *cp,
                               const char *text,
                               int text_len,
                               int start_from,
                               int *match_start,
                               int *match_end,
                               int *group_starts,
                               int *group_ends,
                               int max_groups,
                               int *num_groups);

/// @brief Get number of capture groups in pattern.
/// @param cp Borrowed compiled pattern, or null.
/// @return Number of explicit capture groups, excluding the full match.
int re_group_count(re_compiled_pattern *cp);

//=============================================================================
// Engine internals
//
// AST node types and primitive constructors shared between the core
// (rt_regex.c), the parser (rt_regex_parse.c), and the matcher
// (rt_regex_match.c). These are engine-private; rt_compiled_pattern.c
// includes this header for the re_* API above and simply ignores them.
//=============================================================================

typedef enum {
    RE_LITERAL,      // Single character literal
    RE_DOT,          // . matches any char except newline
    RE_ANCHOR_START, // ^
    RE_ANCHOR_END,   // $
    RE_CLASS,        // Character class [...]
    RE_GROUP,        // Grouping (...)
    RE_CONCAT,       // Sequence of nodes
    RE_ALT,          // Alternation a|b
    RE_QUANT,        // Quantifier applied to child
} re_node_type;

typedef enum {
    QUANT_STAR,  // *
    QUANT_PLUS,  // +
    QUANT_QUEST, // ?
} re_quant_type;

/// Byte-oriented character class with a 256-bit membership map.
typedef struct {
    uint8_t bits[32]; // 256 bits for ASCII chars
    bool negated;
} re_class;

typedef struct re_node re_node;

struct re_node {
    re_node_type type;
    int group_index; // RE_GROUP: lexical capture index (order of '('), else -1

    union {
        char literal;        // RE_LITERAL
        re_class char_class; // RE_CLASS

        struct {
            re_node **children;
            int count;
            int capacity;
        } children; // RE_CONCAT, RE_ALT, RE_GROUP

        struct {
            re_node *child;
            re_quant_type qtype;
            bool greedy;
        } quant; // RE_QUANT
    } data;
};

/// Owned compiled-pattern representation plus cache bookkeeping.
struct re_compiled_pattern {
    char *pattern_str;
    re_node *root;
    bool anchored_start; // Pattern starts with ^
    bool anchored_end;   // Pattern ends with $
    int group_count;     // Number of capture groups (not including group 0)
    unsigned int cache_refs;
    bool cache_linked;
};

/// Compatibility alias used by the implementation files.
typedef struct re_compiled_pattern compiled_pattern;

/// Parser cursor over a pattern source string.
typedef struct {
    const char *src;
    int pos;
    int len;
    int group_counter; // next lexical capture-group index
} parser_state;

/// @brief Allocate a zero-initialized AST node.
/// @param type Node discriminator.
/// @return Newly allocated node, or null after an allocation trap.
re_node *node_new(re_node_type type);

/// @brief Recursively destroy an AST subtree.
/// @param n Owned subtree root; may be null.
void node_free(re_node *n);

/// @brief Append an owned child to a container node.
/// @param n Destination concat, alternation, or group node.
/// @param child Child whose ownership transfers on success.
void children_add(re_node *n, re_node *child);

/// @brief Add a byte value to a character class.
/// @param c Character class to modify.
/// @param ch Byte value; out-of-range values are ignored.
void class_set(re_class *c, int ch);

/// @brief Test effective membership in a possibly negated character class.
/// @param c Character class to inspect.
/// @param ch Candidate byte value.
/// @return Whether @p ch belongs to the class.
bool class_test(const re_class *c, int ch);

/// @brief Add an inclusive byte range to a character class.
/// @param c Character class to modify.
/// @param from Inclusive first value.
/// @param to Inclusive final value.
void class_add_range(re_class *c, int from, int to);

/// @brief Union an ASCII shorthand class or its complement into a class.
/// @param c Character class to modify.
/// @param shorthand One of `d`, `D`, `w`, `W`, `s`, or `S`.
void class_add_shorthand(re_class *c, char shorthand);

/// @brief Parse an alternation expression at the current cursor.
/// @param p Parser state to consume and update.
/// @return Newly allocated AST subtree, or null for an empty expression.
re_node *parse_alternation(parser_state *p);

/// @brief Test whether the parser cursor reached the source length.
/// @param p Parser state to inspect.
/// @return `true` when no pattern bytes remain.
bool at_end(parser_state *p);

/// @brief Trap with a pattern error annotated by the current byte position.
/// @param p Parser state supplying the position.
/// @param msg Null-terminated diagnostic detail.
void parse_error(parser_state *p, const char *msg);

#ifdef __cplusplus
}
#endif
