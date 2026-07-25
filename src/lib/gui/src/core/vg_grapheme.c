//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: lib/gui/src/core/vg_grapheme.c
// Purpose: Implement Unicode extended grapheme boundary scanning and bounded
//          UTF-8 byte/codepoint/grapheme offset conversion for ZannaGUI.
// Key invariants:
//   - Boundary decisions implement Unicode 17.0 UAX #29 rules GB3 through GB13.
//   - A malformed UTF-8 byte decodes as one Other scalar and consumes one byte.
//   - Property lookup tables are sorted and searched without heap allocation.
// Ownership/Lifetime:
//   - Scanners borrow caller bytes and never retain them after a public call.
//   - Generated property tables have immutable process lifetime.
// Links: lib/gui/include/vg_grapheme.h,
//        lib/gui/src/core/vg_grapheme_data.inc,
//        https://www.unicode.org/reports/tr29/tr29-47.html
//
//===----------------------------------------------------------------------===//

#include "../../include/vg_grapheme.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/// @file
/// @brief Implements allocation-free Unicode 17 extended-grapheme boundary scanning.
/// @details Generated property ranges feed a compact UAX #29 state machine covering Hangul,
/// Indic conjuncts, emoji ZWJ sequences, and regional-indicator pairs. Bounded UTF-8 decoding
/// consumes malformed bytes individually so every public conversion remains deterministic.

/// @brief Grapheme_Cluster_Break values needed by the UAX #29 rule machine.
typedef enum vg_grapheme_gcb {
    VG_GRAPHEME_GCB_OTHER = 0,
    VG_GRAPHEME_GCB_CR,
    VG_GRAPHEME_GCB_LF,
    VG_GRAPHEME_GCB_CONTROL,
    VG_GRAPHEME_GCB_EXTEND,
    VG_GRAPHEME_GCB_REGIONAL_INDICATOR,
    VG_GRAPHEME_GCB_PREPEND,
    VG_GRAPHEME_GCB_SPACING_MARK,
    VG_GRAPHEME_GCB_L,
    VG_GRAPHEME_GCB_V,
    VG_GRAPHEME_GCB_T,
    VG_GRAPHEME_GCB_LV,
    VG_GRAPHEME_GCB_LVT,
    VG_GRAPHEME_GCB_ZWJ
} vg_grapheme_gcb_t;

/// @brief Indic_Conjunct_Break values needed by UAX #29 rule GB9c.
typedef enum vg_grapheme_incb {
    VG_GRAPHEME_INCB_NONE = 0,
    VG_GRAPHEME_INCB_LINKER,
    VG_GRAPHEME_INCB_CONSONANT,
    VG_GRAPHEME_INCB_EXTEND
} vg_grapheme_incb_t;

/// @brief Inclusive Unicode scalar range carrying one generated property value.
typedef struct vg_grapheme_property_range {
    uint32_t first; ///< First scalar in the inclusive range.
    uint32_t last;  ///< Last scalar in the inclusive range.
    uint8_t value;  ///< Table-specific property enum or Boolean value.
} vg_grapheme_property_range_t;

#include "vg_grapheme_data.inc"

/// @brief Stateful suffix categories needed to recognize Indic rule GB9c.
typedef enum vg_grapheme_incb_suffix {
    VG_GRAPHEME_INCB_SUFFIX_NONE = 0,
    VG_GRAPHEME_INCB_SUFFIX_CONSONANT,
    VG_GRAPHEME_INCB_SUFFIX_LINKED
} vg_grapheme_incb_suffix_t;

/// @brief One allocation-free forward scanner positioned at a cluster boundary.
typedef struct vg_grapheme_scanner {
    const unsigned char *text; ///< Borrowed bytes supplied by the caller.
    size_t byte_length;        ///< Exact readable length of @ref text.
    size_t byte_offset;        ///< Current extended-grapheme byte boundary.
    size_t codepoint_offset;   ///< Current boundary in decoded codepoints.
} vg_grapheme_scanner_t;

/// @brief Mutable rule context for codepoints consumed within one cluster.
typedef struct vg_grapheme_context {
    vg_grapheme_gcb_t previous_gcb;        ///< GCB property immediately before the candidate.
    size_t regional_indicator_run;         ///< Consecutive RI suffix length for GB12/GB13.
    vg_grapheme_incb_suffix_t incb_suffix; ///< Indic linker-pattern suffix for GB9c.
    bool ep_extend_suffix;                 ///< Suffix matches Extended_Pictographic Extend*.
    bool ep_zwj_suffix;                    ///< Suffix matches Extended_Pictographic Extend* ZWJ.
} vg_grapheme_context_t;

/// @brief Look up one scalar in a sorted, disjoint generated range table.
/// @param ranges Sorted inclusive range table.
/// @param count Number of readable entries in @p ranges.
/// @param scalar Unicode scalar to locate.
/// @return Stored property value, or zero when no range contains @p scalar.
static uint8_t grapheme_lookup_range(const vg_grapheme_property_range_t *ranges,
                                     size_t count,
                                     uint32_t scalar) {
    size_t low = 0;
    size_t high = count;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        const vg_grapheme_property_range_t *range = &ranges[middle];
        if (scalar < range->first) {
            high = middle;
        } else if (scalar > range->last) {
            low = middle + 1;
        } else {
            return range->value;
        }
    }
    return 0;
}

/// @brief Resolve one Unicode scalar's Grapheme_Cluster_Break property.
/// @details Hangul LV/LVT syllables are derived arithmetically, avoiding 399
///          alternating generated ranges and reducing binary-search depth.
/// @param scalar Unicode scalar value or replacement value for malformed input.
/// @return Unicode 17 Grapheme_Cluster_Break property.
static vg_grapheme_gcb_t grapheme_gcb(uint32_t scalar) {
    if (scalar >= 0xAC00u && scalar <= 0xD7A3u)
        return ((scalar - 0xAC00u) % 28u) == 0u ? VG_GRAPHEME_GCB_LV : VG_GRAPHEME_GCB_LVT;
    return (vg_grapheme_gcb_t)grapheme_lookup_range(
        g_grapheme_gcb_ranges, g_grapheme_gcb_ranges_count, scalar);
}

/// @brief Resolve one Unicode scalar's Indic_Conjunct_Break property.
/// @param scalar Unicode scalar value or replacement value for malformed input.
/// @return Unicode 17 Indic_Conjunct_Break value, or None when omitted.
static vg_grapheme_incb_t grapheme_incb(uint32_t scalar) {
    return (vg_grapheme_incb_t)grapheme_lookup_range(
        g_grapheme_incb_ranges, g_grapheme_incb_ranges_count, scalar);
}

/// @brief Test the Unicode Extended_Pictographic property for UAX #29 GB11.
/// @param scalar Unicode scalar value or replacement value for malformed input.
/// @return true when the pinned emoji property table contains @p scalar.
static bool grapheme_is_extended_pictographic(uint32_t scalar) {
    return grapheme_lookup_range(g_grapheme_extended_pictographic_ranges,
                                 g_grapheme_extended_pictographic_ranges_count,
                                 scalar) != 0;
}

/// @brief Decode one bounded UTF-8 scalar with deterministic malformed-byte recovery.
/// @details Valid sequences consume their full width. An invalid lead, truncated
///          sequence, overlong form, surrogate, or out-of-range scalar consumes
///          exactly one byte and returns U+FFFD so a scanner always progresses.
/// @param text Readable byte buffer.
/// @param byte_length Exact readable buffer length.
/// @param byte_offset Offset of the candidate sequence.
/// @param width Receives bytes consumed; zero only at or beyond end-of-buffer.
/// @return Decoded Unicode scalar or U+FFFD for one malformed byte.
static uint32_t grapheme_decode(const unsigned char *text,
                                size_t byte_length,
                                size_t byte_offset,
                                size_t *width) {
    *width = 0;
    if (!text || byte_offset >= byte_length)
        return 0;

    const unsigned char first = text[byte_offset];
    if (first < 0x80u) {
        *width = 1;
        return first;
    }

    size_t expected = 0;
    uint32_t scalar = 0;
    uint32_t minimum = 0;
    if (first >= 0xC2u && first <= 0xDFu) {
        expected = 2;
        scalar = first & 0x1Fu;
        minimum = 0x80u;
    } else if (first >= 0xE0u && first <= 0xEFu) {
        expected = 3;
        scalar = first & 0x0Fu;
        minimum = 0x800u;
    } else if (first >= 0xF0u && first <= 0xF4u) {
        expected = 4;
        scalar = first & 0x07u;
        minimum = 0x10000u;
    } else {
        *width = 1;
        return 0xFFFDu;
    }

    if (expected > byte_length - byte_offset) {
        *width = 1;
        return 0xFFFDu;
    }
    for (size_t index = 1; index < expected; index++) {
        unsigned char continuation = text[byte_offset + index];
        if ((continuation & 0xC0u) != 0x80u) {
            *width = 1;
            return 0xFFFDu;
        }
        scalar = (scalar << 6) | (uint32_t)(continuation & 0x3Fu);
    }
    if (scalar < minimum || scalar > 0x10FFFFu || (scalar >= 0xD800u && scalar <= 0xDFFFu)) {
        *width = 1;
        return 0xFFFDu;
    }
    *width = expected;
    return scalar;
}

/// @brief Return whether a GCB value invokes mandatory breaks in GB4/GB5.
/// @param property Grapheme_Cluster_Break value to classify.
/// @return true for CR, LF, or Control.
static bool grapheme_is_control(vg_grapheme_gcb_t property) {
    return property == VG_GRAPHEME_GCB_CR || property == VG_GRAPHEME_GCB_LF ||
           property == VG_GRAPHEME_GCB_CONTROL;
}

/// @brief Initialize suffix context after consuming the first cluster scalar.
/// @param context Context storage to overwrite.
/// @param scalar First decoded scalar in a non-empty cluster.
static void grapheme_context_init(vg_grapheme_context_t *context, uint32_t scalar) {
    vg_grapheme_gcb_t gcb = grapheme_gcb(scalar);
    vg_grapheme_incb_t incb = grapheme_incb(scalar);
    context->previous_gcb = gcb;
    context->regional_indicator_run = gcb == VG_GRAPHEME_GCB_REGIONAL_INDICATOR ? 1u : 0u;
    context->incb_suffix = incb == VG_GRAPHEME_INCB_CONSONANT ? VG_GRAPHEME_INCB_SUFFIX_CONSONANT
                                                              : VG_GRAPHEME_INCB_SUFFIX_NONE;
    context->ep_extend_suffix = grapheme_is_extended_pictographic(scalar);
    context->ep_zwj_suffix = false;
}

/// @brief Decide whether UAX #29 places a boundary before one candidate scalar.
/// @param context Rule suffix state for all scalars already consumed in the cluster.
/// @param scalar Candidate scalar immediately following the consumed suffix.
/// @return true when the candidate begins a new extended grapheme cluster.
static bool grapheme_should_break(const vg_grapheme_context_t *context, uint32_t scalar) {
    vg_grapheme_gcb_t current = grapheme_gcb(scalar);
    vg_grapheme_incb_t current_incb = grapheme_incb(scalar);
    vg_grapheme_gcb_t previous = context->previous_gcb;

    if (previous == VG_GRAPHEME_GCB_CR && current == VG_GRAPHEME_GCB_LF)
        return false; // GB3
    if (grapheme_is_control(previous) || grapheme_is_control(current))
        return true; // GB4, GB5
    if (previous == VG_GRAPHEME_GCB_L &&
        (current == VG_GRAPHEME_GCB_L || current == VG_GRAPHEME_GCB_V ||
         current == VG_GRAPHEME_GCB_LV || current == VG_GRAPHEME_GCB_LVT))
        return false; // GB6
    if ((previous == VG_GRAPHEME_GCB_LV || previous == VG_GRAPHEME_GCB_V) &&
        (current == VG_GRAPHEME_GCB_V || current == VG_GRAPHEME_GCB_T))
        return false; // GB7
    if ((previous == VG_GRAPHEME_GCB_LVT || previous == VG_GRAPHEME_GCB_T) &&
        current == VG_GRAPHEME_GCB_T)
        return false; // GB8
    if (current == VG_GRAPHEME_GCB_EXTEND || current == VG_GRAPHEME_GCB_ZWJ)
        return false; // GB9
    if (current == VG_GRAPHEME_GCB_SPACING_MARK)
        return false; // GB9a
    if (previous == VG_GRAPHEME_GCB_PREPEND)
        return false; // GB9b
    if (current_incb == VG_GRAPHEME_INCB_CONSONANT &&
        context->incb_suffix == VG_GRAPHEME_INCB_SUFFIX_LINKED)
        return false; // GB9c
    if (context->ep_zwj_suffix && grapheme_is_extended_pictographic(scalar))
        return false; // GB11
    if (previous == VG_GRAPHEME_GCB_REGIONAL_INDICATOR &&
        current == VG_GRAPHEME_GCB_REGIONAL_INDICATOR &&
        (context->regional_indicator_run & 1u) != 0u)
        return false; // GB12, GB13
    return true;      // GB999
}

/// @brief Update suffix context after a candidate was joined to the cluster.
/// @param context Mutable context to advance.
/// @param scalar Newly consumed Unicode scalar.
static void grapheme_context_consume(vg_grapheme_context_t *context, uint32_t scalar) {
    vg_grapheme_gcb_t gcb = grapheme_gcb(scalar);
    vg_grapheme_incb_t incb = grapheme_incb(scalar);
    bool prior_ep_extend = context->ep_extend_suffix;

    context->ep_zwj_suffix = gcb == VG_GRAPHEME_GCB_ZWJ && prior_ep_extend;
    if (grapheme_is_extended_pictographic(scalar)) {
        context->ep_extend_suffix = true;
    } else if (gcb != VG_GRAPHEME_GCB_EXTEND) {
        context->ep_extend_suffix = false;
    }

    if (gcb == VG_GRAPHEME_GCB_REGIONAL_INDICATOR) {
        context->regional_indicator_run++;
    } else {
        context->regional_indicator_run = 0;
    }

    if (incb == VG_GRAPHEME_INCB_CONSONANT) {
        context->incb_suffix = VG_GRAPHEME_INCB_SUFFIX_CONSONANT;
    } else if (incb == VG_GRAPHEME_INCB_LINKER) {
        if (context->incb_suffix == VG_GRAPHEME_INCB_SUFFIX_CONSONANT ||
            context->incb_suffix == VG_GRAPHEME_INCB_SUFFIX_LINKED)
            context->incb_suffix = VG_GRAPHEME_INCB_SUFFIX_LINKED;
    } else if (incb != VG_GRAPHEME_INCB_EXTEND) {
        context->incb_suffix = VG_GRAPHEME_INCB_SUFFIX_NONE;
    }
    context->previous_gcb = gcb;
}

/// @brief Initialize a scanner at start-of-text with validated borrowed input.
/// @param scanner Scanner storage to overwrite.
/// @param text Borrowed input bytes.
/// @param byte_length Exact readable byte length.
/// @return true for a valid input pair, false for `(NULL, nonzero)`.
static bool grapheme_scanner_init(vg_grapheme_scanner_t *scanner,
                                  const char *text,
                                  size_t byte_length) {
    scanner->text = (const unsigned char *)text;
    scanner->byte_length = byte_length;
    scanner->byte_offset = 0;
    scanner->codepoint_offset = 0;
    return text != NULL || byte_length == 0;
}

/// @brief Advance a scanner from its current boundary to the next one.
/// @details The scanner consumes one complete extended grapheme cluster using
///          a compact suffix-state machine for the non-local GB9c, GB11, and
///          GB12/GB13 rules.
/// @param scanner Valid scanner positioned at a cluster boundary.
/// @return true when a cluster was consumed, false at end-of-text.
static bool grapheme_scanner_next(vg_grapheme_scanner_t *scanner) {
    if (!scanner || scanner->byte_offset >= scanner->byte_length)
        return false;

    size_t width = 0;
    uint32_t scalar =
        grapheme_decode(scanner->text, scanner->byte_length, scanner->byte_offset, &width);
    vg_grapheme_context_t context;
    grapheme_context_init(&context, scalar);
    scanner->byte_offset += width;
    scanner->codepoint_offset++;

    while (scanner->byte_offset < scanner->byte_length) {
        scalar = grapheme_decode(scanner->text, scanner->byte_length, scanner->byte_offset, &width);
        if (grapheme_should_break(&context, scalar))
            break;
        grapheme_context_consume(&context, scalar);
        scanner->byte_offset += width;
        scanner->codepoint_offset++;
    }
    return true;
}

/// @brief Scan every cluster and return both grapheme and decoded-codepoint totals.
/// @param text Borrowed input bytes.
/// @param byte_length Exact readable byte length.
/// @param codepoint_count Optional receiver for the decoded codepoint total.
/// @return Extended grapheme cluster count, or zero for an invalid input pair.
static size_t grapheme_scan_totals(const char *text, size_t byte_length, size_t *codepoint_count) {
    if (codepoint_count)
        *codepoint_count = 0;
    vg_grapheme_scanner_t scanner;
    if (!grapheme_scanner_init(&scanner, text, byte_length))
        return 0;
    size_t count = 0;
    while (grapheme_scanner_next(&scanner))
        count++;
    if (codepoint_count)
        *codepoint_count = scanner.codepoint_offset;
    return count;
}

/// @brief Return the pinned Unicode version implemented by this translation unit.
/// @details The returned process-lifetime string matches the property tables generated from the
///          pinned Unicode Character Database release and requires no caller cleanup.
/// @return Borrowed, immutable, NUL-terminated Unicode version string.
const char *vg_grapheme_unicode_version(void) {
    return "17.0.0";
}

/// @brief Count user-perceived characters in an explicitly bounded UTF-8 sequence.
/// @details The scan applies the complete extended-grapheme rules and treats each malformed byte
///          as one deterministic scalar so it cannot stall or read beyond @p byte_length.
/// @param text Borrowed UTF-8 bytes, or NULL only when @p byte_length is zero.
/// @param byte_length Exact readable byte count in @p text.
/// @return Extended grapheme count, or zero for an invalid `(NULL, nonzero)` input pair.
size_t vg_grapheme_count(const char *text, size_t byte_length) {
    return grapheme_scan_totals(text, byte_length, NULL);
}

/// @brief Resolve a clamped grapheme boundary to its byte offset.
/// @param text Borrowed UTF-8 bytes, or NULL only when @p byte_length is zero.
/// @param byte_length Exact readable byte count in @p text.
/// @param grapheme_index Zero-based boundary index; values beyond the count select end-of-text.
/// @return Offset in the inclusive range `[0, byte_length]`.
size_t vg_grapheme_byte_offset(const char *text, size_t byte_length, size_t grapheme_index) {
    vg_grapheme_scanner_t scanner;
    if (!grapheme_scanner_init(&scanner, text, byte_length))
        return 0;
    for (size_t index = 0; index < grapheme_index; index++) {
        if (!grapheme_scanner_next(&scanner))
            break;
    }
    return scanner.byte_offset;
}

/// @brief Resolve a clamped grapheme boundary to the legacy decoded-codepoint coordinate space.
/// @param text Borrowed UTF-8 bytes, or NULL only when @p byte_length is zero.
/// @param byte_length Exact readable byte count in @p text.
/// @param grapheme_index Zero-based boundary index; values beyond the count select end-of-text.
/// @return Decoded-codepoint offset at the requested extended-grapheme boundary.
size_t vg_grapheme_codepoint_offset(const char *text, size_t byte_length, size_t grapheme_index) {
    vg_grapheme_scanner_t scanner;
    if (!grapheme_scanner_init(&scanner, text, byte_length))
        return 0;
    for (size_t index = 0; index < grapheme_index; index++) {
        if (!grapheme_scanner_next(&scanner))
            break;
    }
    return scanner.codepoint_offset;
}

/// @brief Map a legacy decoded-codepoint offset to its containing grapheme index.
/// @details An offset inside a multi-codepoint cluster rounds down to that cluster; an offset at
///          or beyond end-of-text maps to the grapheme count.
/// @param text Borrowed UTF-8 bytes, or NULL only when @p byte_length is zero.
/// @param byte_length Exact readable byte count in @p text.
/// @param codepoint_offset Legacy decoded-codepoint coordinate to convert.
/// @return Zero-based containing or exact-boundary extended-grapheme index.
size_t vg_grapheme_index_from_codepoint(const char *text,
                                        size_t byte_length,
                                        size_t codepoint_offset) {
    vg_grapheme_scanner_t scanner;
    if (!grapheme_scanner_init(&scanner, text, byte_length))
        return 0;
    size_t index = 0;
    while (scanner.byte_offset < scanner.byte_length) {
        size_t cluster_start = scanner.codepoint_offset;
        grapheme_scanner_next(&scanner);
        if (codepoint_offset < scanner.codepoint_offset)
            return index;
        index++;
        if (codepoint_offset == scanner.codepoint_offset)
            return index;
        if (scanner.codepoint_offset == cluster_start)
            break;
    }
    return index;
}

/// @brief Find the previous safe editing boundary in decoded-codepoint units.
/// @details Exact boundaries move to the preceding cluster while positions inside a cluster move
///          to that cluster's start. Inputs beyond end-of-text are safely clamped.
/// @param text Borrowed UTF-8 bytes, or NULL only when @p byte_length is zero.
/// @param byte_length Exact readable byte count in @p text.
/// @param codepoint_offset Current legacy decoded-codepoint cursor coordinate.
/// @return Previous extended-grapheme boundary expressed as a codepoint offset.
size_t vg_grapheme_previous_codepoint_boundary(const char *text,
                                               size_t byte_length,
                                               size_t codepoint_offset) {
    vg_grapheme_scanner_t scanner;
    if (!grapheme_scanner_init(&scanner, text, byte_length) || codepoint_offset == 0)
        return 0;
    size_t previous_boundary = 0;
    size_t current_boundary = 0;
    while (grapheme_scanner_next(&scanner)) {
        size_t next_boundary = scanner.codepoint_offset;
        if (codepoint_offset <= next_boundary) {
            return codepoint_offset == current_boundary ? previous_boundary : current_boundary;
        }
        previous_boundary = current_boundary;
        current_boundary = next_boundary;
    }
    return current_boundary;
}

/// @brief Find the next safe editing boundary in decoded-codepoint units.
/// @details Exact and interior positions advance to the current cluster's end; end-of-text and
///          out-of-range inputs remain clamped to the final decoded-codepoint boundary.
/// @param text Borrowed UTF-8 bytes, or NULL only when @p byte_length is zero.
/// @param byte_length Exact readable byte count in @p text.
/// @param codepoint_offset Current legacy decoded-codepoint cursor coordinate.
/// @return Following extended-grapheme boundary expressed as a codepoint offset.
size_t vg_grapheme_next_codepoint_boundary(const char *text,
                                           size_t byte_length,
                                           size_t codepoint_offset) {
    vg_grapheme_scanner_t scanner;
    if (!grapheme_scanner_init(&scanner, text, byte_length))
        return 0;
    while (grapheme_scanner_next(&scanner)) {
        if (codepoint_offset < scanner.codepoint_offset)
            return scanner.codepoint_offset;
    }
    return scanner.codepoint_offset;
}
