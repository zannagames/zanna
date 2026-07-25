//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: lib/gui/include/vg_grapheme.h
// Purpose: Dependency-free Unicode extended grapheme segmentation and offset
//          conversion for GUI editing, selection, and accessibility APIs.
// Key invariants:
//   - Public grapheme indices follow Unicode 17.0 / UAX #29 revision 47.
//   - Every operation is bounded by an explicit byte length and makes progress
//     across malformed UTF-8 without reading beyond the supplied buffer.
// Ownership/Lifetime:
//   - All functions borrow input bytes only for the duration of the call.
//   - The module allocates no memory and returns only scalar offsets.
// Links: lib/gui/src/core/vg_grapheme.c,
//        lib/gui/tools/generate_grapheme_data.py,
//        https://www.unicode.org/reports/tr29/tr29-47.html
//
//===----------------------------------------------------------------------===//
#pragma once

#include <stddef.h>

/// @file
/// @brief Declares bounded Unicode extended-grapheme segmentation and offset conversion.
/// @details The dependency-free API implements Unicode 17.0 default extended grapheme rules,
/// treats malformed UTF-8 deterministically, and converts among byte, codepoint, and
/// user-perceived-character boundaries without allocating.

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Major Unicode version implemented by the grapheme property tables.
#define VG_GRAPHEME_UNICODE_VERSION_MAJOR 17

/// @brief Minor Unicode version implemented by the grapheme property tables.
#define VG_GRAPHEME_UNICODE_VERSION_MINOR 0

/// @brief Patch Unicode version implemented by the grapheme property tables.
#define VG_GRAPHEME_UNICODE_VERSION_PATCH 0

/// @brief Return the Unicode data version used for extended grapheme boundaries.
///
/// @details The returned static string currently names Unicode `17.0.0`. It is
///          suitable for diagnostics, generated documentation, and test logs.
/// @return A process-lifetime, NUL-terminated version string owned by the module.
const char *vg_grapheme_unicode_version(void);

/// @brief Count extended grapheme clusters in a bounded UTF-8 byte sequence.
///
/// @details Implements the default extended grapheme boundary rules from UAX
///          #29, including Hangul syllables, Indic conjuncts, regional-indicator
///          pairs, and extended-pictographic ZWJ sequences. Embedded NUL bytes
///          are treated as U+0000 Control characters rather than terminators.
///          Each malformed UTF-8 byte is treated as one independent Other
///          scalar so the scan remains deterministic and always advances.
/// @param text Borrowed UTF-8 bytes, or NULL only when @p byte_length is zero.
/// @param byte_length Exact number of readable bytes in @p text.
/// @return Number of extended grapheme clusters; zero for an empty or invalid
///         `(NULL, nonzero)` input pair.
size_t vg_grapheme_count(const char *text, size_t byte_length);

/// @brief Convert a grapheme index to its UTF-8 byte boundary.
///
/// @details Index zero maps to byte zero. An index at or beyond the grapheme
///          count maps to @p byte_length, making the operation safe for clamped
///          cursor APIs. Embedded NUL and malformed bytes follow the same
///          rules as vg_grapheme_count().
/// @param text Borrowed UTF-8 bytes, or NULL only when @p byte_length is zero.
/// @param byte_length Exact number of readable bytes in @p text.
/// @param grapheme_index Zero-based extended grapheme boundary index.
/// @return Byte offset in the inclusive range `[0, byte_length]`.
size_t vg_grapheme_byte_offset(const char *text, size_t byte_length, size_t grapheme_index);

/// @brief Convert a grapheme index to its Unicode codepoint boundary.
///
/// @details This bridges the public user-perceived-character contract to legacy
///          editor storage that still records cursor positions in codepoints.
///          An index beyond the grapheme count maps to the total decoded
///          codepoint count. Malformed bytes each contribute one codepoint.
/// @param text Borrowed UTF-8 bytes, or NULL only when @p byte_length is zero.
/// @param byte_length Exact number of readable bytes in @p text.
/// @param grapheme_index Zero-based extended grapheme boundary index.
/// @return Codepoint offset in the inclusive range `[0, decoded_count]`.
size_t vg_grapheme_codepoint_offset(const char *text, size_t byte_length, size_t grapheme_index);

/// @brief Convert a codepoint offset to a clamped extended-grapheme index.
///
/// @details Exact grapheme boundaries round-trip. A codepoint offset inside a
///          multi-codepoint cluster maps down to the index of that cluster;
///          an offset beyond the text maps to the grapheme count.
/// @param text Borrowed UTF-8 bytes, or NULL only when @p byte_length is zero.
/// @param byte_length Exact number of readable bytes in @p text.
/// @param codepoint_offset Zero-based codepoint boundary to convert.
/// @return Zero-based grapheme index containing or beginning at the requested
///         codepoint offset.
size_t vg_grapheme_index_from_codepoint(const char *text,
                                        size_t byte_length,
                                        size_t codepoint_offset);

/// @brief Find the preceding extended-grapheme boundary in codepoint units.
///
/// @details At an exact boundary this returns the previous boundary; inside a
///          cluster it returns that cluster's start. Zero remains zero and
///          offsets beyond the text are clamped to the decoded codepoint count
///          before moving backward.
/// @param text Borrowed UTF-8 bytes, or NULL only when @p byte_length is zero.
/// @param byte_length Exact number of readable bytes in @p text.
/// @param codepoint_offset Current legacy codepoint cursor offset.
/// @return Previous grapheme boundary expressed as a codepoint offset.
size_t vg_grapheme_previous_codepoint_boundary(const char *text,
                                               size_t byte_length,
                                               size_t codepoint_offset);

/// @brief Find the following extended-grapheme boundary in codepoint units.
///
/// @details At an exact boundary this returns the next boundary; inside a
///          cluster it returns that cluster's end. End-of-text remains fixed
///          and offsets beyond the text are clamped.
/// @param text Borrowed UTF-8 bytes, or NULL only when @p byte_length is zero.
/// @param byte_length Exact number of readable bytes in @p text.
/// @param codepoint_offset Current legacy codepoint cursor offset.
/// @return Next grapheme boundary expressed as a codepoint offset.
size_t vg_grapheme_next_codepoint_boundary(const char *text,
                                           size_t byte_length,
                                           size_t codepoint_offset);

#ifdef __cplusplus
}
#endif
