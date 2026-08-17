//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/text/rt_regex_engine.c
// Purpose: Own the platform-neutral compiled-regex representation shared by
//          runtime Pattern APIs and ZannaGUI editor search.
//
// Key invariants:
//   - Syntax errors are recoverable diagnostics; they never invoke the fatal
//     failure callback.
//   - Allocation and invariant failures use the installed process callback.
//   - Compiled patterns own their source copy and complete AST.
//
// Ownership/Lifetime:
//   - re_compile* returns caller-owned patterns released with re_free.
//   - The failure callback is borrowed and process-wide.
//
// Links: src/runtime/text/rt_regex_internal.h,
//        src/runtime/text/rt_regex_parse.c,
//        src/runtime/text/rt_regex_match.c
//
//===----------------------------------------------------------------------===//

#include "rt_regex_internal.h"

#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static _Atomic(re_failure_handler) g_re_failure_handler = NULL;

/// Inclusive Unicode scalar interval used by the word classifier.
typedef struct re_codepoint_range {
    uint32_t first;
    uint32_t last;
} re_codepoint_range;

/// Unicode 16.0 L*, M*, N*, Pc, and Join_Control intervals.
static const re_codepoint_range g_re_word_ranges[] = {
#include "rt_unicode_word_ranges.inc"
};

/// @copydoc re_set_failure_handler
void re_set_failure_handler(re_failure_handler handler) {
    atomic_store_explicit(&g_re_failure_handler, handler, memory_order_release);
}

/// @copydoc re_report_failure
void re_report_failure(const char *message) {
    re_failure_handler handler = atomic_load_explicit(&g_re_failure_handler, memory_order_acquire);
    if (handler)
        handler(message ? message : "Pattern: regex engine failure");
}

/// @copydoc re_is_word_codepoint
bool re_is_word_codepoint(uint32_t codepoint) {
    if (codepoint > UINT32_C(0x10FFFF) ||
        (codepoint >= UINT32_C(0xD800) && codepoint <= UINT32_C(0xDFFF)))
        return false;

    size_t low = 0;
    size_t high = sizeof(g_re_word_ranges) / sizeof(g_re_word_ranges[0]);
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        re_codepoint_range range = g_re_word_ranges[middle];
        if (codepoint < range.first) {
            high = middle;
        } else if (codepoint > range.last) {
            low = middle + 1;
        } else {
            return true;
        }
    }
    return false;
}

/// @copydoc node_new
re_node *node_new(re_node_type type) {
    re_node *node = (re_node *)calloc(1, sizeof(re_node));
    if (!node) {
        re_report_failure("Pattern: memory allocation failed");
        return NULL;
    }
    node->type = type;
    node->group_index = -1;
    return node;
}

/// @copydoc node_free
void node_free(re_node *node) {
    if (!node)
        return;
    switch (node->type) {
        case RE_CONCAT:
        case RE_ALT:
        case RE_GROUP:
            for (int i = 0; i < node->data.children.count; i++)
                node_free(node->data.children.children[i]);
            free(node->data.children.children);
            break;
        case RE_QUANT:
            node_free(node->data.quant.child);
            break;
        default:
            break;
    }
    free(node);
}

/// @copydoc children_add
void children_add(re_node *node, re_node *child) {
    if (!node || !child) {
        re_report_failure("Pattern: invalid child node");
        return;
    }
    if (node->data.children.count >= node->data.children.capacity) {
        if (node->data.children.capacity > INT_MAX / 2) {
            re_report_failure("Pattern: too many child nodes");
            return;
        }
        int next_capacity =
            node->data.children.capacity == 0 ? 4 : node->data.children.capacity * 2;
        if ((size_t)next_capacity > SIZE_MAX / sizeof(re_node *)) {
            re_report_failure("Pattern: child node allocation overflow");
            return;
        }
        re_node **next = (re_node **)realloc(node->data.children.children,
                                             (size_t)next_capacity * sizeof(re_node *));
        if (!next) {
            re_report_failure("Pattern: memory allocation failed");
            return;
        }
        node->data.children.children = next;
        node->data.children.capacity = next_capacity;
    }
    node->data.children.children[node->data.children.count++] = child;
}

/// @copydoc class_set
void class_set(re_class *cls, int ch) {
    if (cls && ch >= 0 && ch < 256)
        cls->bits[ch / 8] |= (uint8_t)(1u << (ch % 8));
}

/// @copydoc class_test
bool class_test(const re_class *cls, int ch) {
    if (!cls)
        return false;
    if (ch < 0 || ch >= 256)
        return cls->negated;
    bool member = (cls->bits[ch / 8] & (uint8_t)(1u << (ch % 8))) != 0;
    return cls->negated ? !member : member;
}

/// @copydoc class_add_range
void class_add_range(re_class *cls, int from, int to) {
    for (int ch = from; cls && ch <= to && ch < 256; ch++)
        class_set(cls, ch);
}

/// @brief Return whether a byte belongs to one ASCII shorthand base set.
static bool shorthand_member(char shorthand, int ch) {
    switch (shorthand) {
        case 'd':
            return ch >= '0' && ch <= '9';
        case 'w':
            return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                   (ch >= '0' && ch <= '9') || ch == '_';
        case 's':
            return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == '\v';
        default:
            return false;
    }
}

/// @copydoc class_add_shorthand
void class_add_shorthand(re_class *cls, char shorthand) {
    if (!cls)
        return;
    char base = shorthand;
    bool complement = false;
    if (shorthand >= 'A' && shorthand <= 'Z') {
        base = (char)(shorthand - 'A' + 'a');
        complement = true;
    }
    for (int ch = 0; ch < 256; ch++) {
        if (shorthand_member(base, ch) != complement)
            class_set(cls, ch);
    }
}

/// @brief Recursively count lexical capture groups.
static int count_groups(const re_node *node) {
    if (!node)
        return 0;
    int count = node->type == RE_GROUP ? 1 : 0;
    switch (node->type) {
        case RE_GROUP:
        case RE_CONCAT:
        case RE_ALT:
            for (int i = 0; i < node->data.children.count; i++)
                count += count_groups(node->data.children.children[i]);
            break;
        case RE_QUANT:
            count += count_groups(node->data.quant.child);
            break;
        default:
            break;
    }
    return count;
}

/// @brief Release one complete compiled pattern.
static void pattern_free(compiled_pattern *pattern) {
    if (!pattern)
        return;
    free(pattern->pattern_str);
    node_free(pattern->root);
    free(pattern);
}

/// @copydoc re_free
void re_free(re_compiled_pattern *pattern) {
    pattern_free(pattern);
}

/// @brief Write a bounded recoverable syntax diagnostic.
static void write_compile_error(char *error, size_t capacity, int position, const char *detail) {
    if (!error || capacity == 0)
        return;
    if (position >= 0)
        snprintf(error,
                 capacity,
                 "Pattern error at position %d: %s",
                 position,
                 detail ? detail : "invalid syntax");
    else
        snprintf(error, capacity, "Pattern: %s", detail ? detail : "invalid syntax");
}

/// @copydoc re_compile_diagnostic
re_compiled_pattern *re_compile_diagnostic(const char *source,
                                           unsigned int flags,
                                           char *error,
                                           size_t error_capacity) {
    if (error && error_capacity > 0)
        error[0] = '\0';
    if (!source) {
        write_compile_error(error, error_capacity, -1, "null pattern");
        return NULL;
    }
    size_t source_length = strlen(source);
    if (source_length > (size_t)INT_MAX) {
        write_compile_error(error, error_capacity, -1, "pattern is too long");
        return NULL;
    }

    compiled_pattern *pattern = (compiled_pattern *)calloc(1, sizeof(compiled_pattern));
    if (!pattern) {
        re_report_failure("Pattern: memory allocation failed");
        return NULL;
    }
    pattern->pattern_str = (char *)malloc(source_length + 1);
    if (!pattern->pattern_str) {
        pattern_free(pattern);
        re_report_failure("Pattern: memory allocation failed");
        return NULL;
    }
    memcpy(pattern->pattern_str, source, source_length + 1);
    pattern->flags = flags & RE_COMPILE_CASE_INSENSITIVE;

    parser_state parser = {source, 0, (int)source_length, 0, NULL, 0};
    pattern->root = parse_alternation(&parser);
    if (parser.error_message) {
        write_compile_error(error, error_capacity, parser.error_position, parser.error_message);
        pattern_free(pattern);
        return NULL;
    }
    if (!at_end(&parser)) {
        write_compile_error(error, error_capacity, parser.pos, "unexpected character");
        pattern_free(pattern);
        return NULL;
    }
    if (!pattern->root)
        pattern->root = node_new(RE_CONCAT);
    if (!pattern->root) {
        pattern_free(pattern);
        return NULL;
    }
    pattern->group_count = count_groups(pattern->root);
    return pattern;
}

/// @copydoc re_compile
re_compiled_pattern *re_compile(const char *source) {
    char error[256];
    re_compiled_pattern *pattern =
        re_compile_diagnostic(source, RE_COMPILE_DEFAULT, error, sizeof(error));
    if (!pattern)
        re_report_failure(error[0] ? error : "Pattern: regex compilation failed");
    return pattern;
}

/// @copydoc re_get_pattern
const char *re_get_pattern(re_compiled_pattern *pattern) {
    return pattern ? pattern->pattern_str : "";
}

/// @copydoc re_group_count
int re_group_count(re_compiled_pattern *pattern) {
    return pattern ? pattern->group_count : 0;
}
