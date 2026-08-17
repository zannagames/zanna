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
//   - This header is a private in-process bridge for the runtime text layer and
//     ZannaGUI editor search; it is not part of the installed runtime C ABI.
//   - Compiled patterns own a parsed AST and source-pattern copy.
//   - re_compile is the trapping runtime compilation entry point.
//   - re_compile_diagnostic reports malformed interactive input without trapping.
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
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Opaque declaration used by the internal compile and match API.
typedef struct re_compiled_pattern re_compiled_pattern;

/// @brief Compile and matching options for the shared in-process engine.
typedef enum re_compile_flags {
    RE_COMPILE_DEFAULT = 0,
    RE_COMPILE_CASE_INSENSITIVE = 1u << 0,
} re_compile_flags;

/// @brief Process-wide fatal engine failure callback.
/// @details Syntax diagnostics never use this callback. Runtime integration
///          installs `rt_trap`; standalone GUI consumers may leave it null and
///          receive ordinary failure returns on resource exhaustion.
typedef void (*re_failure_handler)(const char *message);

/// @brief Install the shared engine's resource/invariant failure callback.
/// @param handler Callback to invoke, or null to restore return-only behavior.
void re_set_failure_handler(re_failure_handler handler);

/// @brief Report a non-syntax engine failure through the installed callback.
/// @param message Borrowed stable diagnostic string.
void re_report_failure(const char *message);

/// @brief Compile a pattern string into an internal representation.
/// @details Parses the complete null-terminated source and counts explicit
///          capture groups. Empty input compiles as a zero-width expression.
/// @param pattern Required null-terminated regex source.
/// @return Newly allocated compiled pattern, or `NULL` after a syntax or
///         allocation trap.
re_compiled_pattern *re_compile(const char *pattern);

/// @brief Compile malformed user input without raising a runtime trap.
/// @details Syntax errors return `NULL` with a bounded diagnostic. Resource
///          exhaustion and internal invariant failures keep their trapping
///          behavior so this API does not hide runtime faults.
/// @param pattern Required null-terminated regex source.
/// @param flags Bitwise @ref re_compile_flags values.
/// @param error Writable diagnostic buffer, or null when capacity is zero.
/// @param error_capacity Capacity of @p error including its terminator.
/// @return Newly allocated pattern, or `NULL` for malformed syntax.
re_compiled_pattern *re_compile_diagnostic(const char *pattern,
                                           unsigned int flags,
                                           char *error,
                                           size_t error_capacity);

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

/// @brief Expand capture references for the match beginning at one byte offset.
/// @details Supports `$$` (literal dollar), `$&`/`$0` (full match), `$1` and
///          `${1}` style numbered groups. Unknown references remain literal;
///          nonparticipating valid groups expand to empty bytes.
/// @param cp Borrowed compiled pattern.
/// @param text Borrowed subject bytes.
/// @param text_len Subject byte length.
/// @param match_start Expected full-match starting byte offset.
/// @param replacement Borrowed replacement bytes.
/// @param replacement_len Replacement byte length.
/// @param output Receives a newly allocated NUL-terminated byte buffer.
/// @param output_len Receives expanded byte length, excluding the terminator.
/// @return `true` after exact-match expansion, otherwise `false`.
bool re_expand_replacement(re_compiled_pattern *cp,
                           const char *text,
                           int text_len,
                           int match_start,
                           const char *replacement,
                           size_t replacement_len,
                           char **output,
                           size_t *output_len);

/// @brief Get number of capture groups in pattern.
/// @param cp Borrowed compiled pattern, or null.
/// @return Number of explicit capture groups, excluding the full match.
int re_group_count(re_compiled_pattern *cp);

/// @brief Classify one Unicode scalar for whole-word search boundaries.
/// @details Uses a deterministic Unicode 16.0 table covering letters, marks,
///          numbers, connector punctuation, and join-control characters.
/// @param codepoint Unicode scalar value.
/// @return `true` when the scalar continues a searchable word.
bool re_is_word_codepoint(uint32_t codepoint);

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
    unsigned int flags;  // Bitwise re_compile_flags used by the matcher
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
    int group_counter;         // next lexical capture-group index
    const char *error_message; // first syntax error, or NULL
    int error_position;        // byte offset captured with error_message
} parser_state;

/// @brief Allocate a zero-initialized AST node.
/// @param type Node discriminator.
/// @return Newly allocated node, or null after reporting allocation failure.
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

/// @brief Record a pattern error annotated by the current byte position.
/// @param p Parser state supplying the position.
/// @param msg Null-terminated diagnostic detail.
void parse_error(parser_state *p, const char *msg);

#ifdef __cplusplus
}
#endif
