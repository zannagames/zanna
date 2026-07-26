//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/text/rt_bitmapfont.c
// Purpose: Custom bitmap font loading (BDF/PSF formats) and Canvas rendering.
//   Parses BDF (text-based) and PSF v1/v2 (binary) font files into an internal
//   glyph table, then draws text to a Canvas using the loaded glyphs.
//
// Key invariants:
//   - BDF parser: line-by-line text parsing, hex bitmap decoding.
//   - PSF parser: binary header + sequential raw glyph bitmaps + optional Unicode tables.
//   - Glyph bitmaps are packed 1-bit (MSB-left, row-major), same as rt_font.c.
//   - Rendering uses vgfx_pset / vgfx_fill_rect, matching existing Canvas text.
//
// Ownership/Lifetime:
//   - BitmapFont objects allocated via rt_obj_new_i64 (GC-managed).
//   - Per-glyph bitmap arrays are malloc'd; freed in rt_bitmapfont_destroy.
//
// Links: src/runtime/graphics/text/rt_bitmapfont.h,
//        src/runtime/graphics/text/rt_font.h,
//        src/runtime/graphics/2d/rt_drawing.c
//
//===----------------------------------------------------------------------===//

#include "rt_bitmapfont.h"
#include "rt_error.h"
#include "rt_file_stdio.h"
#include "rt_object.h"
#include "rt_pixels_internal.h"
#include "rt_string.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

//=============================================================================
// Internal Data Structures
//=============================================================================

/// @brief A single glyph in a bitmap font.
typedef struct {
    uint8_t *bitmap;  ///< Packed 1-bit bitmap (row-major, MSB-left). NULL if no glyph.
    int16_t width;    ///< Glyph width in pixels.
    int16_t height;   ///< Glyph height in pixels.
    int16_t x_offset; ///< Horizontal bearing (BDF BBX x-offset).
    int16_t y_offset; ///< Vertical bearing from baseline (BDF BBX y-offset).
    int16_t advance;  ///< Horizontal advance after glyph.
} rt_glyph;

/// @brief Maximum codepoints in the glyph table (full BMP / UTF-16 plane 0).
#define BF_MAX_GLYPHS 65536

/// @brief Internal bitmap font structure.
typedef struct {
    rt_glyph glyphs[BF_MAX_GLYPHS]; ///< Glyph table indexed by codepoint.
    int16_t line_height;            ///< Line height in pixels (ascent + descent).
    int16_t max_width;              ///< Widest glyph advance.
    int16_t ascent;                 ///< Distance from baseline to top.
    int8_t monospace;               ///< 1 if all loaded glyphs have same advance.
    int64_t glyph_count;            ///< Number of valid (non-NULL bitmap) glyphs.
} rt_bitmapfont_impl;

/// @brief Validate and unwrap a public BitmapFont or SpriteFont handle.
/// @param font_ptr Candidate runtime object handle.
/// @return The internal font payload when @p font_ptr has either supported
/// class ID and the expected object size; otherwise `NULL`.
static rt_bitmapfont_impl *bitmapfont_checked(void *font_ptr) {
    if (rt_obj_is_instance(font_ptr, RT_BITMAPFONT_CLASS_ID, sizeof(rt_bitmapfont_impl)) ||
        rt_obj_is_instance(font_ptr, RT_SPRITEFONT_CLASS_ID, sizeof(rt_bitmapfont_impl)))
        return (rt_bitmapfont_impl *)font_ptr;
    return NULL;
}

//=============================================================================
// Fallback Glyph
//=============================================================================

/// @brief Compute the number of packed bitmap bytes needed for one row.
/// @param width Glyph width in pixels; callers supply a non-negative value.
/// @return `ceil(width / 8)` bytes.
static inline int bf_row_bytes(int width) {
    return (width + 7) / 8;
}

/// @brief Parse one signed integer token from a BDF metadata field.
/// @details Uses `strtol` with full-token validation and explicit int-range checks so malformed
///          fields such as `12px` or overflowing values cannot be partially accepted.
/// @param text Input cursor; leading spaces and tabs are skipped.
/// @param out_value Receives the parsed integer on success.
/// @param out_end Receives the cursor after the parsed token when non-NULL.
/// @return 1 on success; 0 on malformed input or overflow.
static int bf_parse_int_token(const char *text, int *out_value, const char **out_end) {
    if (!text || !out_value)
        return 0;
    while (*text == ' ' || *text == '\t')
        text++;
    if (*text == '\0')
        return 0;
    errno = 0;
    char *end = NULL;
    long parsed = strtol(text, &end, 10);
    if (end == text || errno == ERANGE || parsed < INT_MIN || parsed > INT_MAX)
        return 0;
    *out_value = (int)parsed;
    if (out_end)
        *out_end = end;
    return 1;
}

/// @brief Parse exactly @p count integer fields from a BDF directive payload.
/// @details After the expected values, only horizontal whitespace is allowed.
///          This catches truncated directives and extra garbage that `sscanf`
///          would otherwise ignore.
/// @param text Directive payload immediately after the keyword.
/// @param values Output array of at least @p count integers.
/// @param count Number of integer tokens required.
/// @return 1 when exactly @p count integers were consumed; otherwise 0.
static int bf_parse_int_fields(const char *text, int *values, int count) {
    const char *cursor = text;
    if (!values || count <= 0)
        return 0;
    for (int i = 0; i < count; i++) {
        if (!bf_parse_int_token(cursor, &values[i], &cursor))
            return 0;
    }
    while (*cursor == ' ' || *cursor == '\t')
        cursor++;
    return *cursor == '\0';
}

static int bf_next_codepoint(const char *str, size_t byte_len, size_t *index, int *codepoint_out);

/// @brief Add two signed coordinates without overflowing.
/// @param a First addend.
/// @param b Second addend.
/// @return The mathematical sum clamped to the `int64_t` range.
static int64_t bf_add_sat64(int64_t a, int64_t b) {
    if (b > 0 && a > INT64_MAX - b)
        return INT64_MAX;
    if (b < 0 && a < INT64_MIN - b)
        return INT64_MIN;
    return a + b;
}

/// @brief Resolve a codepoint to a loaded glyph or the configured fallback.
/// @param font Valid internal font payload.
/// @param codepoint Unicode scalar value requested by the renderer.
/// @return The matching glyph, then `'?'`, then space, or `NULL` if none of
/// those slots contains bitmap data.
static const rt_glyph *bf_get_glyph(const rt_bitmapfont_impl *font, int codepoint) {
    if (codepoint >= 0 && codepoint < BF_MAX_GLYPHS && font->glyphs[codepoint].bitmap)
        return &font->glyphs[codepoint];

    // Fallback: try '?', then space
    if (font->glyphs['?'].bitmap)
        return &font->glyphs['?'];
    if (font->glyphs[' '].bitmap)
        return &font->glyphs[' '];

    return NULL;
}

/// @brief Grow a `(min_x, max_x)` interval to also cover `[left, right)`.
/// @details Initializes the bounds on the first call (`*has_bounds` flips
///          from 0 to 1) so callers don't need a separate initialization
///          pass. Empty spans (`right <= left`) are silently skipped so the
///          caller can pass through every glyph regardless of whether it
///          has visible pixels.
/// @param left Inclusive left edge of the candidate interval.
/// @param right Exclusive right edge of the candidate interval.
/// @param min_x In/out minimum bound.
/// @param max_x In/out maximum bound.
/// @param has_bounds In/out flag indicating whether the bound pair has been
/// initialized.
static void bf_extend_bounds(
    int64_t left, int64_t right, int64_t *min_x, int64_t *max_x, int8_t *has_bounds) {
    if (!min_x || !max_x || !has_bounds || right <= left)
        return;

    if (!*has_bounds) {
        *min_x = left;
        *max_x = right;
        *has_bounds = 1;
        return;
    }

    if (left < *min_x)
        *min_x = left;
    if (right > *max_x)
        *max_x = right;
}

/// @brief Compute the horizontal pixel bounds of a rendered string.
/// @details Walks the string codepoint by codepoint, tracking both the
///          per-glyph advance (the cursor's nominal step) and the actual
///          ink extent (which may overhang on either side via positive
///          `x_offset` reaching past the advance, or negative `x_offset`
///          starting before the cursor). The returned `(min_x, max_x)`
///          interval covers the union of advance and ink extents — used by
///          centering / right-alignment / width-measurement APIs to size
///          text accurately even with overhanging glyphs (italics,
///          decorative scripts).
/// @param font Font used to resolve glyph metrics and fallbacks.
/// @param text Runtime UTF-8 string to measure.
/// @param min_x Receives the inclusive left extent relative to pen position
/// zero.
/// @param max_x Receives the exclusive right extent relative to pen position
/// zero.
/// @param has_bounds Receives 1 when at least one codepoint contributed an
/// interval, or 0 for invalid/empty input.
static void bf_text_bounds(
    rt_bitmapfont_impl *font, rt_string text, int64_t *min_x, int64_t *max_x, int8_t *has_bounds) {
    if (min_x)
        *min_x = 0;
    if (max_x)
        *max_x = 0;
    if (has_bounds)
        *has_bounds = 0;
    if (!font || !text || !min_x || !max_x || !has_bounds)
        return;

    const char *str = rt_string_cstr(text);
    if (!str)
        return;

    int64_t pen_x = 0;
    size_t len = rt_str_len(text);
    size_t index = 0;
    int codepoint = 0;
    while (bf_next_codepoint(str, len, &index, &codepoint)) {
        const rt_glyph *g = bf_get_glyph(font, codepoint);
        int64_t span_left = pen_x;
        int64_t span_right = bf_add_sat64(pen_x, g ? g->advance : font->max_width);

        if (g && g->bitmap && g->width > 0 && g->height > 0) {
            int64_t glyph_left = bf_add_sat64(pen_x, g->x_offset);
            int64_t glyph_right = bf_add_sat64(glyph_left, g->width);
            if (glyph_left < span_left)
                span_left = glyph_left;
            if (glyph_right > span_right)
                span_right = glyph_right;
        }

        bf_extend_bounds(span_left, span_right, min_x, max_x, has_bounds);
        pen_x = bf_add_sat64(pen_x, g ? g->advance : font->max_width);
    }
}

/// @brief Decode the UTF-8 codepoint at `*index` and advance `*index` past it.
/// @details Handles 1- through 4-byte UTF-8 sequences with the standard
///          continuation-byte (`0b10xxxxxx`) verification on every trailing
///          byte. Several invalid-input cases collapse to the substitution
///          character `'?'` rather than failing:
///          - **Overlong encoding** (e.g., a 3-byte form for a value
///            < 0x800): rejected. Allowing overlongs is a known
///            security-hole pattern — a string filter that only checks
///            the canonical encoding can be bypassed via overlong
///            re-encoding.
///          - **Surrogate pair half** (`0xD800-0xDFFF`): rejected.
///            UTF-8 must not encode UTF-16 surrogate halves; their sole
///            purpose is in UTF-16 pair encoding.
///          - **Out-of-range codepoint** (above `U+10FFFF`): rejected.
///          - **Truncated trailing byte** (sequence runs past `byte_len`):
///            falls back to a single-byte read.
///          - **Bad continuation byte**: falls back to a single-byte read.
///
///          The "fall back to single byte" cases produce `'?'` and only
///          consume one byte, so the caller resyncs naturally to the next
///          codepoint boundary on the following call.
/// @param str Byte buffer containing UTF-8 input.
/// @param byte_len Number of accessible bytes in @p str.
/// @param index In/out byte offset of the next sequence.
/// @param codepoint_out Receives the decoded scalar value or `'?'` for an
/// invalid sequence.
/// @return 1 if a codepoint was decoded and `*index` advanced; 0 if
///         `*index` is already at or past `byte_len` (end of string).
static int bf_next_codepoint(const char *str, size_t byte_len, size_t *index, int *codepoint_out) {
    if (!str || !index || !codepoint_out || *index >= byte_len)
        return 0;

    size_t i = *index;
    unsigned char c0 = (unsigned char)str[i];
    uint32_t cp = '?';
    size_t advance = 1;

    if (c0 < 0x80) {
        cp = c0;
    } else if ((c0 & 0xE0u) == 0xC0u && i + 1 < byte_len) {
        unsigned char c1 = (unsigned char)str[i + 1];
        if ((c1 & 0xC0u) == 0x80u) {
            cp = ((uint32_t)(c0 & 0x1Fu) << 6) | (uint32_t)(c1 & 0x3Fu);
            advance = 2;
            if (cp < 0x80u)
                cp = '?';
        }
    } else if ((c0 & 0xF0u) == 0xE0u && i + 2 < byte_len) {
        unsigned char c1 = (unsigned char)str[i + 1];
        unsigned char c2 = (unsigned char)str[i + 2];
        if ((c1 & 0xC0u) == 0x80u && (c2 & 0xC0u) == 0x80u) {
            cp = ((uint32_t)(c0 & 0x0Fu) << 12) | ((uint32_t)(c1 & 0x3Fu) << 6) |
                 (uint32_t)(c2 & 0x3Fu);
            advance = 3;
            if (cp < 0x800u || (cp >= 0xD800u && cp <= 0xDFFFu))
                cp = '?';
        }
    } else if ((c0 & 0xF8u) == 0xF0u && i + 3 < byte_len) {
        unsigned char c1 = (unsigned char)str[i + 1];
        unsigned char c2 = (unsigned char)str[i + 2];
        unsigned char c3 = (unsigned char)str[i + 3];
        if ((c1 & 0xC0u) == 0x80u && (c2 & 0xC0u) == 0x80u && (c3 & 0xC0u) == 0x80u) {
            cp = ((uint32_t)(c0 & 0x07u) << 18) | ((uint32_t)(c1 & 0x3Fu) << 12) |
                 ((uint32_t)(c2 & 0x3Fu) << 6) | (uint32_t)(c3 & 0x3Fu);
            advance = 4;
            if (cp < 0x10000u || cp > 0x10FFFFu)
                cp = '?';
        }
    }

    *index = i + advance;
    *codepoint_out = (int)cp;
    return 1;
}

/// @brief Copy a glyph bitmap from one slot to another codepoint slot in the same font.
/// @details Used by the PSF Unicode-table appliers (apply_psf1/2_unicode_table)
///          to alias multiple Unicode codepoints onto the same physical glyph
///          (e.g. mapping U+00A0 NO-BREAK SPACE to the existing U+0020 glyph).
///          Refuses to overwrite an already-populated destination, refuses
///          out-of-range source/dest indices, and caps per-glyph allocation
///          at 1 MiB to bound the memory cost of malformed PSF Unicode tables.
/// @param font Font containing the source and destination glyph slots.
/// @param source_index Existing physical-glyph slot to copy.
/// @param codepoint BMP destination slot to populate.
/// @return 1 if the copy succeeded, 0 if any precondition failed.
static int bf_copy_glyph_to_codepoint(rt_bitmapfont_impl *font, int source_index, int codepoint) {
    if (!font || source_index < 0 || source_index >= BF_MAX_GLYPHS || codepoint < 0 ||
        codepoint >= BF_MAX_GLYPHS)
        return 0;
    rt_glyph *src = &font->glyphs[source_index];
    rt_glyph *dst = &font->glyphs[codepoint];
    if (!src->bitmap || dst->bitmap)
        return 0;
    int rb = bf_row_bytes(src->width);
    int64_t byte_count = (int64_t)rb * src->height;
    if (byte_count <= 0 || byte_count > 1024 * 1024)
        return 0;
    uint8_t *copy = (uint8_t *)malloc((size_t)byte_count);
    if (!copy)
        return 0;
    memcpy(copy, src->bitmap, (size_t)byte_count);
    *dst = *src;
    dst->bitmap = copy;
    font->glyph_count++;
    return 1;
}

/// @brief Apply the optional PSF v2 Unicode table to alias codepoints onto loaded glyphs.
/// @details The PSF v2 Unicode section is a sequence of UTF-8 encoded
///          codepoints separated by 0xFF (end of glyph) and 0xFE (start of
///          combining-character sequence, which we skip — Zanna's bitmap font
///          renderer does not compose). Each codepoint between separators is
///          aliased to the current glyph index via bf_copy_glyph_to_codepoint.
/// @param font Font whose glyph table receives copied Unicode aliases.
/// @param glyph_count Number of sequential physical glyphs described by the
/// table.
/// @param table Raw PSF v2 Unicode-table bytes.
/// @param table_len Number of accessible bytes in @p table.
static void bf_apply_psf2_unicode_table(rt_bitmapfont_impl *font,
                                        int glyph_count,
                                        const uint8_t *table,
                                        size_t table_len) {
    if (!font || !table || table_len == 0)
        return;
    int glyph_index = 0;
    int skipping_sequence = 0;
    size_t index = 0;
    while (glyph_index < glyph_count && index < table_len) {
        uint8_t byte = table[index];
        if (byte == 0xFFu) {
            glyph_index++;
            index++;
            skipping_sequence = 0;
            continue;
        }
        if (byte == 0xFEu) {
            skipping_sequence = 1;
            index++;
            continue;
        }
        if (skipping_sequence) {
            index++;
            continue;
        }

        int codepoint = 0;
        size_t before = index;
        if (!bf_next_codepoint((const char *)table, table_len, &index, &codepoint) ||
            index == before) {
            index++;
            continue;
        }
        if (codepoint >= 0 && codepoint < BF_MAX_GLYPHS)
            bf_copy_glyph_to_codepoint(font, glyph_index, codepoint);
    }
}

/// @brief Apply the optional PSF v1 Unicode table to alias codepoints onto loaded glyphs.
/// @details PSF v1 stores codepoints as little-endian uint16 (UCS-2) with
///          0xFFFF as the end-of-glyph separator and 0xFFFE marking the
///          start of a skipped combining sequence. Otherwise mirrors the
///          PSF v2 table semantics implemented by bf_apply_psf2_unicode_table.
/// @param font Font whose glyph table receives copied Unicode aliases.
/// @param glyph_count Number of sequential physical glyphs described by the
/// table.
/// @param table Raw little-endian PSF v1 Unicode-table bytes.
/// @param table_len Number of accessible bytes in @p table.
static void bf_apply_psf1_unicode_table(rt_bitmapfont_impl *font,
                                        int glyph_count,
                                        const uint8_t *table,
                                        size_t table_len) {
    if (!font || !table || table_len < 2)
        return;
    int glyph_index = 0;
    int skipping_sequence = 0;
    size_t index = 0;
    while (glyph_index < glyph_count && index + 1 < table_len) {
        uint16_t value = (uint16_t)table[index] | ((uint16_t)table[index + 1] << 8);
        index += 2;
        if (value == 0xFFFFu) {
            glyph_index++;
            skipping_sequence = 0;
            continue;
        }
        if (value == 0xFFFEu) {
            skipping_sequence = 1;
            continue;
        }
        if (!skipping_sequence)
            bf_copy_glyph_to_codepoint(font, glyph_index, (int)value);
    }
}

/// @brief Release a partially constructed or caller-owned bitmap-font object.
/// @details The runtime finalizer frees each glyph bitmap before the object
/// storage is reclaimed when its reference count reaches zero.
/// @param font Font object to release; `NULL` is accepted.
static void bf_release_font(rt_bitmapfont_impl *font) {
    if (!font)
        return;
    if (rt_obj_release_check0(font))
        rt_obj_free(font);
}

//=============================================================================
// BDF Parser
//=============================================================================

/// @brief Convert one hexadecimal digit to its numeric value.
/// @param c Candidate ASCII digit.
/// @return A value from 0 through 15, or -1 when @p c is not hexadecimal.
static int bf_hex_digit(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/// @brief Parse exactly two hexadecimal characters as one byte.
/// @param s Pointer to at least two accessible characters; an early NUL makes
/// the token invalid.
/// @return A value from 0 through 255, or -1 for a short or non-hex token.
static int bf_hex_byte(const char *s) {
    if (!s[0] || !s[1])
        return -1;
    int hi = bf_hex_digit(s[0]);
    int lo = bf_hex_digit(s[1]);
    if (hi < 0 || lo < 0)
        return -1;
    return (hi << 4) | lo;
}

/// @brief Load a BDF font and assign the requested runtime class identity.
/// @details Parses `ENCODING`, `BBX`, `DWIDTH`, and `BITMAP` records into a
/// 65,536-slot BMP glyph table. Parsing fails closed for incomplete glyph
/// rows, invalid dimensions or metrics, missing `ENDFONT`, allocation
/// failures, and files that produce no glyphs.
/// @param path Runtime string containing the platform path to the BDF file.
/// @param class_id Runtime class ID to assign to the allocated font object.
/// @return A GC-managed font handle on success, or `NULL` on path, I/O,
/// allocation, or parse failure.
static void *bitmapfont_load_bdf_as(rt_string path, int64_t class_id) {
    if (!path)
        return NULL;

    const char *cpath = rt_string_cstr(path);
    if (!cpath)
        return NULL;

    FILE *f = rt_file_stdio_open_utf8(cpath, "rb");
    if (!f)
        return NULL;

    rt_bitmapfont_impl *font =
        (rt_bitmapfont_impl *)rt_obj_new_i64(class_id, (int64_t)sizeof(rt_bitmapfont_impl));
    if (!font) {
        fclose(f);
        return NULL;
    }
    memset(font, 0, sizeof(rt_bitmapfont_impl));
    rt_obj_set_finalizer(font, rt_bitmapfont_destroy);

    char line[1024];
    int encoding = -1;
    int bbx_w = 0, bbx_h = 0, bbx_xoff = 0, bbx_yoff = 0;
    int default_bbx_w = 0, default_bbx_h = 0;
    int in_bitmap = 0;
    int bitmap_row = 0;
    uint8_t *cur_bitmap = NULL;
    int font_ascent = 0;
    int first_advance = -1;
    int all_same_advance = 1;
    int dwidth = 0;
    int saw_endfont = 0;
    int parse_failed = 0;

    while (fgets(line, sizeof(line), f)) {
        // Strip trailing whitespace
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r' || line[len - 1] == ' '))
            line[--len] = '\0';

        if (in_bitmap) {
            if (strncmp(line, "ENDCHAR", 7) == 0) {
                // Store glyph
                if (bitmap_row != bbx_h) {
                    parse_failed = 1;
                } else if (encoding >= 0 && encoding < BF_MAX_GLYPHS && cur_bitmap) {
                    rt_glyph *g = &font->glyphs[encoding];
                    g->bitmap = cur_bitmap;
                    g->width = (int16_t)bbx_w;
                    g->height = (int16_t)bbx_h;
                    g->x_offset = (int16_t)bbx_xoff;
                    g->y_offset = (int16_t)bbx_yoff;
                    g->advance = (int16_t)(dwidth > 0 ? dwidth : bbx_w);
                    font->glyph_count++;

                    if (g->advance > font->max_width)
                        font->max_width = g->advance;

                    if (first_advance < 0)
                        first_advance = g->advance;
                    else if (g->advance != first_advance)
                        all_same_advance = 0;
                } else {
                    free(cur_bitmap);
                }

                in_bitmap = 0;
                cur_bitmap = NULL;
                encoding = -1;
                dwidth = 0;
            } else {
                // Parse hex row
                if (cur_bitmap && bitmap_row < bbx_h) {
                    int rb = bf_row_bytes(bbx_w);
                    int b;
                    for (b = 0; b < rb && line[b * 2] != '\0' && line[b * 2 + 1] != '\0'; b++) {
                        int val = bf_hex_byte(&line[b * 2]);
                        if (val >= 0) {
                            cur_bitmap[bitmap_row * rb + b] = (uint8_t)val;
                        } else {
                            parse_failed = 1;
                            break;
                        }
                    }
                    /* A BITMAP row must supply exactly bf_row_bytes(bbx_w) hex bytes; a line that
                     * ends early (NUL before rb bytes) means the glyph data is truncated, which
                     * would otherwise be silently zero-filled into a malformed glyph. */
                    if (!parse_failed && b < rb)
                        parse_failed = 1;
                    bitmap_row++;
                } else {
                    parse_failed = 1;
                }
            }
            if (parse_failed)
                break;
            continue;
        }

        if (strncmp(line, "ENCODING ", 9) == 0) {
            if (!bf_parse_int_token(line + 9, &encoding, NULL)) {
                parse_failed = 1;
                break;
            }
        } else if (strncmp(line, "BBX ", 4) == 0) {
            int values[4];
            if (!bf_parse_int_fields(line + 4, values, 4)) {
                parse_failed = 1;
                break;
            }
            bbx_w = values[0];
            bbx_h = values[1];
            bbx_xoff = values[2];
            bbx_yoff = values[3];
        } else if (strncmp(line, "FONTBOUNDINGBOX ", 16) == 0) {
            int values[4];
            if (!bf_parse_int_fields(line + 16, values, 4)) {
                parse_failed = 1;
                break;
            }
            default_bbx_w = values[0];
            default_bbx_h = values[1];
            if (default_bbx_h < INT16_MIN || default_bbx_h > INT16_MAX) {
                parse_failed = 1;
                break;
            }
            if (font->line_height == 0)
                font->line_height = (int16_t)default_bbx_h;
        } else if (strncmp(line, "FONT_ASCENT ", 12) == 0) {
            if (!bf_parse_int_token(line + 12, &font_ascent, NULL) || font_ascent < INT16_MIN ||
                font_ascent > INT16_MAX) {
                parse_failed = 1;
                break;
            }
            font->ascent = (int16_t)font_ascent;
        } else if (strncmp(line, "FONT_DESCENT ", 13) == 0) {
            int descent = 0;
            if (!bf_parse_int_token(line + 13, &descent, NULL)) {
                parse_failed = 1;
                break;
            }
            long line_height = (long)font_ascent + (long)descent;
            if (line_height < INT16_MIN || line_height > INT16_MAX) {
                parse_failed = 1;
                break;
            }
            font->line_height = (int16_t)line_height;
        } else if (strncmp(line, "DWIDTH ", 7) == 0) {
            if (!bf_parse_int_token(line + 7, &dwidth, NULL)) {
                parse_failed = 1;
                break;
            }
        } else if (strncmp(line, "BITMAP", 6) == 0 && (line[6] == '\0' || line[6] == '\r')) {
            if (bbx_w <= 0)
                bbx_w = default_bbx_w;
            if (bbx_h <= 0)
                bbx_h = default_bbx_h;

            if (bbx_w <= 0 || bbx_h <= 0 || bbx_w > 4096 || bbx_h > 4096) {
                parse_failed = 1;
                break;
            }
            int rb = bf_row_bytes(bbx_w);
            int64_t alloc_size = (int64_t)rb * bbx_h;
            if (alloc_size > 0 && alloc_size <= 1024 * 1024) {
                cur_bitmap = (uint8_t *)calloc(1, (size_t)alloc_size);
            } else {
                parse_failed = 1;
                break;
            }
            bitmap_row = 0;
            in_bitmap = 1;
        } else if (strncmp(line, "ENDFONT", 7) == 0) {
            saw_endfont = 1;
        }
    }

    fclose(f);
    free(cur_bitmap); /* Free any partial glyph from truncated file */

    if (parse_failed || in_bitmap || !saw_endfont || font->glyph_count == 0) {
        // No glyphs loaded — invalid file
        bf_release_font(font);
        return NULL;
    }

    font->monospace = (int8_t)all_same_advance;

    // If line_height wasn't set by FONT_ASCENT/DESCENT, use bounding box
    if (font->line_height <= 0)
        font->line_height = (int16_t)default_bbx_h;

    return font;
}

/// @brief Load a BDF file as a BitmapFont runtime object.
/// @param path Runtime string containing the BDF file path.
/// @return A GC-managed BitmapFont handle, or `NULL` if loading fails.
void *rt_bitmapfont_load_bdf(rt_string path) {
    return bitmapfont_load_bdf_as(path, RT_BITMAPFONT_CLASS_ID);
}

/// @brief Load a BDF file as a SpriteFont-compatible runtime object.
/// @param path Runtime string containing the BDF file path.
/// @return A GC-managed SpriteFont handle, or `NULL` if loading fails.
void *rt_spritefont_load_bdf(rt_string path) {
    return bitmapfont_load_bdf_as(path, RT_SPRITEFONT_CLASS_ID);
}

//=============================================================================
// PSF Parser
//=============================================================================

/// @brief PSF v1 magic bytes.
#define PSF1_MAGIC0 0x36
#define PSF1_MAGIC1 0x04
#define PSF1_MODE512 0x01
#define PSF1_MODEHASTAB 0x02
#define PSF1_MODEHASSEQ 0x04

/// @brief PSF v2 magic bytes.
#define PSF2_MAGIC0 0x72
#define PSF2_MAGIC1 0xB5
#define PSF2_MAGIC2 0x4A
#define PSF2_MAGIC3 0x86

/// @brief Load a PSF v1 or v2 font with the requested runtime class identity.
/// @details Auto-detects the version from its magic bytes, imports packed
/// monospace glyphs, and applies an optional Unicode alias table. PSF v1
/// glyphs are eight pixels wide; PSF v2 dimensions come from its header.
/// @param path Runtime string containing the platform path to the PSF file.
/// @param class_id Runtime class ID to assign to the allocated font object.
/// @return A GC-managed font handle on success, or `NULL` for an unsupported,
/// truncated, malformed, unreadable, or unallocatable input.
static void *bitmapfont_load_psf_as(rt_string path, int64_t class_id) {
    if (!path)
        return NULL;

    const char *cpath = rt_string_cstr(path);
    if (!cpath)
        return NULL;

    FILE *f = rt_file_stdio_open_utf8(cpath, "rb");
    if (!f)
        return NULL;

    // Read first 4 bytes to detect version
    uint8_t magic[4];
    if (fread(magic, 1, 4, f) != 4) {
        fclose(f);
        return NULL;
    }

    int glyph_count = 0;
    int glyph_height = 0;
    int glyph_width = 0;
    int glyph_byte_size = 0;
    int64_t data_offset = 0;
    int psf_version = 0;
    int has_unicode_table = 0;
    int parse_failed = 0;

    if (magic[0] == PSF1_MAGIC0 && magic[1] == PSF1_MAGIC1) {
        // PSF v1: 4-byte header (magic[2] = mode, magic[3] = charsize)
        psf_version = 1;
        glyph_byte_size = magic[3];
        glyph_height = glyph_byte_size; // PSF1: 1 byte per row, rows = charsize
        glyph_width = 8;                // Always 8 pixels wide
        glyph_count = (magic[2] & PSF1_MODE512) ? 512 : 256;
        has_unicode_table = (magic[2] & (PSF1_MODEHASTAB | PSF1_MODEHASSEQ)) != 0;
        data_offset = 4;
    } else if (magic[0] == PSF2_MAGIC0 && magic[1] == PSF2_MAGIC1 && magic[2] == PSF2_MAGIC2 &&
               magic[3] == PSF2_MAGIC3) {
        // PSF v2: 32-byte header
        psf_version = 2;
        uint8_t hdr[28]; // Remaining 28 bytes of header
        if (fread(hdr, 1, 28, f) != 28) {
            fclose(f);
            return NULL;
        }

        // Fields are little-endian uint32:
        // offset 0: version
        uint32_t header_size = (uint32_t)hdr[4] | ((uint32_t)hdr[5] << 8) |
                               ((uint32_t)hdr[6] << 16) | ((uint32_t)hdr[7] << 24);
        // offset 8: flags
        uint32_t flags = (uint32_t)hdr[8] | ((uint32_t)hdr[9] << 8) | ((uint32_t)hdr[10] << 16) |
                         ((uint32_t)hdr[11] << 24);
        uint32_t num_glyphs = (uint32_t)hdr[12] | ((uint32_t)hdr[13] << 8) |
                              ((uint32_t)hdr[14] << 16) | ((uint32_t)hdr[15] << 24);
        uint32_t bytes_per_glyph = (uint32_t)hdr[16] | ((uint32_t)hdr[17] << 8) |
                                   ((uint32_t)hdr[18] << 16) | ((uint32_t)hdr[19] << 24);
        uint32_t height = (uint32_t)hdr[20] | ((uint32_t)hdr[21] << 8) | ((uint32_t)hdr[22] << 16) |
                          ((uint32_t)hdr[23] << 24);
        uint32_t width = (uint32_t)hdr[24] | ((uint32_t)hdr[25] << 8) | ((uint32_t)hdr[26] << 16) |
                         ((uint32_t)hdr[27] << 24);

        glyph_count = (int)num_glyphs;
        glyph_height = (int)height;
        glyph_width = (int)width;
        glyph_byte_size = (int)bytes_per_glyph;
        data_offset = (int64_t)header_size;
        has_unicode_table = (flags & 1u) != 0;
    } else {
        fclose(f);
        return NULL;
    }

    if (glyph_count <= 0 || glyph_height <= 0 || glyph_width <= 0 || glyph_byte_size <= 0) {
        fclose(f);
        return NULL;
    }

    if (glyph_byte_size < bf_row_bytes(glyph_width) * glyph_height) {
        fclose(f);
        return NULL;
    }

    // Clamp to our max
    if (glyph_count > BF_MAX_GLYPHS)
        glyph_count = BF_MAX_GLYPHS;

    rt_bitmapfont_impl *font =
        (rt_bitmapfont_impl *)rt_obj_new_i64(class_id, (int64_t)sizeof(rt_bitmapfont_impl));
    if (!font) {
        fclose(f);
        return NULL;
    }
    memset(font, 0, sizeof(rt_bitmapfont_impl));
    rt_obj_set_finalizer(font, rt_bitmapfont_destroy);

    if (rt_file_stdio_seek64(f, data_offset, SEEK_SET) != 0) {
        fclose(f);
        bf_release_font(font);
        return NULL;
    }

    int rb = bf_row_bytes(glyph_width);

    for (int i = 0; i < glyph_count; i++) {
        uint8_t *raw = (uint8_t *)malloc((size_t)glyph_byte_size);
        if (!raw) {
            parse_failed = 1;
            break;
        }

        if (fread(raw, 1, (size_t)glyph_byte_size, f) != (size_t)glyph_byte_size) {
            free(raw);
            parse_failed = 1;
            break;
        }

        // PSF glyph bitmaps are already packed MSB-left, row-major
        // but we need to copy only the relevant bytes per row
        int64_t alloc_size = (int64_t)rb * glyph_height;
        if (alloc_size <= 0 || alloc_size > 1024 * 1024) {
            free(raw);
            parse_failed = 1;
            break;
        }
        uint8_t *bitmap = (uint8_t *)calloc(1, (size_t)alloc_size);
        if (!bitmap) {
            free(raw);
            parse_failed = 1;
            break;
        }

        // Copy row data (PSF rows may have padding at end)
        int psf_rb = bf_row_bytes(glyph_width);
        for (int row = 0; row < glyph_height && row * psf_rb < glyph_byte_size; row++) {
            int copy = psf_rb < (glyph_byte_size - row * psf_rb) ? psf_rb
                                                                 : (glyph_byte_size - row * psf_rb);
            if (copy > rb)
                copy = rb;
            memcpy(bitmap + row * rb, raw + row * psf_rb, (size_t)copy);
        }
        free(raw);

        rt_glyph *g = &font->glyphs[i];
        g->bitmap = bitmap;
        g->width = (int16_t)glyph_width;
        g->height = (int16_t)glyph_height;
        g->x_offset = 0;
        g->y_offset = 0;
        g->advance = (int16_t)glyph_width;
        font->glyph_count++;
    }

    if (!parse_failed && has_unicode_table) {
        int64_t table_start = rt_file_stdio_tell64(f);
        if (table_start >= 0 && rt_file_stdio_seek64(f, 0, SEEK_END) == 0) {
            int64_t file_end = rt_file_stdio_tell64(f);
            if (file_end >= table_start) {
                size_t table_len = (size_t)(file_end - table_start);
                if (table_len > 0 && table_len <= 16 * 1024 * 1024) {
                    uint8_t *table = (uint8_t *)malloc(table_len);
                    if (table && rt_file_stdio_seek64(f, table_start, SEEK_SET) == 0 &&
                        fread(table, 1, table_len, f) == table_len) {
                        if (psf_version == 2)
                            bf_apply_psf2_unicode_table(font, glyph_count, table, table_len);
                        else if (psf_version == 1)
                            bf_apply_psf1_unicode_table(font, glyph_count, table, table_len);
                    } else if (!table) {
                        parse_failed = 1;
                    }
                    free(table);
                }
            }
        }
    }

    fclose(f);

    if (parse_failed || font->glyph_count < glyph_count) {
        bf_release_font(font);
        return NULL;
    }

    font->line_height = (int16_t)glyph_height;
    font->max_width = (int16_t)glyph_width;
    font->ascent = (int16_t)glyph_height;
    font->monospace = 1; // PSF fonts are always monospace

    return font;
}

/// @brief Load a PSF v1 or v2 file as a BitmapFont runtime object.
/// @param path Runtime string containing the PSF file path.
/// @return A GC-managed BitmapFont handle, or `NULL` if loading fails.
void *rt_bitmapfont_load_psf(rt_string path) {
    return bitmapfont_load_psf_as(path, RT_BITMAPFONT_CLASS_ID);
}

/// @brief Load a PSF v1 or v2 file as a SpriteFont-compatible runtime object.
/// @param path Runtime string containing the PSF file path.
/// @return A GC-managed SpriteFont handle, or `NULL` if loading fails.
void *rt_spritefont_load_psf(rt_string path) {
    return bitmapfont_load_psf_as(path, RT_SPRITEFONT_CLASS_ID);
}

//=============================================================================
// Destructor
//=============================================================================

/// @brief GC finalizer that frees every per-glyph bitmap allocation.
/// @details The font object storage itself remains owned by the runtime object
/// manager. Invalid handles are ignored.
/// @param font_ptr BitmapFont or SpriteFont object being finalized.
void rt_bitmapfont_destroy(void *font_ptr) {
    rt_bitmapfont_impl *font = bitmapfont_checked(font_ptr);
    if (!font)
        return;
    for (int i = 0; i < BF_MAX_GLYPHS; i++) {
        free(font->glyphs[i].bitmap);
        font->glyphs[i].bitmap = NULL;
    }
}

//=============================================================================
// Properties
//=============================================================================

/// @brief Query the fixed advance width of a monospace bitmap font.
/// @param font_ptr BitmapFont or SpriteFont handle.
/// @return The maximum glyph advance when the font is monospace; otherwise 0,
/// including for an invalid handle.
int64_t rt_bitmapfont_char_width(void *font_ptr) {
    rt_bitmapfont_impl *font = bitmapfont_checked(font_ptr);
    if (!font)
        return 0;
    return font->monospace ? font->max_width : 0;
}

/// @brief Query the font's single-line height.
/// @param font_ptr BitmapFont or SpriteFont handle.
/// @return The ascent-plus-descent height in pixels, or 0 for an invalid
/// handle.
int64_t rt_bitmapfont_char_height(void *font_ptr) {
    rt_bitmapfont_impl *font = bitmapfont_checked(font_ptr);
    return font ? font->line_height : 0;
}

/// @brief Query the number of populated glyph-table slots.
/// @param font_ptr BitmapFont or SpriteFont handle.
/// @return The number of glyph slots containing bitmap data, including
/// Unicode aliases copied from PSF tables, or 0 for an invalid handle.
int64_t rt_bitmapfont_glyph_count(void *font_ptr) {
    rt_bitmapfont_impl *font = bitmapfont_checked(font_ptr);
    return font ? font->glyph_count : 0;
}

/// @brief Report whether every loaded glyph has the same advance width.
/// @param font_ptr BitmapFont or SpriteFont handle.
/// @return 1 for a valid monospace font; otherwise 0.
int8_t rt_bitmapfont_is_monospace(void *font_ptr) {
    rt_bitmapfont_impl *font = bitmapfont_checked(font_ptr);
    return font ? font->monospace : 0;
}

//=============================================================================
// Text Measurement
//=============================================================================

/// @brief Compute the rendered width of @p text in pixels for this font.
/// @details Accounts for per-glyph advances, bearings, fallback glyphs, and
/// left or right ink overhangs while decoding the runtime string as UTF-8.
/// @param font_ptr BitmapFont or SpriteFont handle.
/// @param text Runtime UTF-8 string to measure.
/// @return The saturated horizontal extent in pixels, or 0 for invalid,
/// `NULL`, or empty input.
int64_t rt_bitmapfont_text_width(void *font_ptr, rt_string text) {
    rt_bitmapfont_impl *font = bitmapfont_checked(font_ptr);
    if (!font || !text)
        return 0;

    int64_t min_x = 0;
    int64_t max_x = 0;
    int8_t has_bounds = 0;
    bf_text_bounds(font, text, &min_x, &max_x, &has_bounds);
    if (!has_bounds)
        return 0;
    return max_x - min_x;
}

/// @brief Query the height of one line of bitmap-font text.
/// @details This routine does not inspect the string or account for multiple
/// lines; callers performing multiline layout accumulate line heights.
/// @param font_ptr BitmapFont or SpriteFont handle.
/// @return The font line height in pixels, or 0 for an invalid handle.
int64_t rt_bitmapfont_text_height(void *font_ptr) {
    rt_bitmapfont_impl *font = bitmapfont_checked(font_ptr);
    return font ? font->line_height : 0;
}

//=============================================================================
// Canvas Drawing — Glyph Renderer
//=============================================================================

#ifdef ZANNA_ENABLE_GRAPHICS

#include "rt_graphics_internal.h"

/// @brief Convert a runtime Canvas color to the backend's 24-bit RGB value.
/// @param color Runtime packed color accepted by the Pixels/Canvas API.
/// @return The converted backend RGB value with alpha discarded.
static vgfx_color_t bitmapfont_color_to_vgfx_rgb(int64_t color) {
    return (vgfx_color_t)((rt_pixels_color_to_rgba(color) >> 8) & 0x00FFFFFFu);
}

/// @brief Return whether a packed glyph bitmap has a lit texel at @p row/@p col.
/// @details Glyph bitmaps are MSB-first, one row after another. The helper
///          centralizes bounds checks and bit addressing so run extraction can
///          stay focused on scanline traversal.
/// @param g Glyph whose packed bitmap is being inspected.
/// @param row_bytes Number of packed bytes per glyph row.
/// @param row Zero-based glyph row.
/// @param col Zero-based glyph column.
/// @return Non-zero when the glyph pixel is set, otherwise zero.
static int bf_glyph_bit_is_set(const rt_glyph *g, int row_bytes, int row, int col) {
    int byte_idx;
    int bit_idx;
    if (!g || !g->bitmap || row < 0 || col < 0 || row >= g->height || col >= g->width)
        return 0;
    byte_idx = col / 8;
    bit_idx = 7 - (col % 8);
    return (g->bitmap[row * row_bytes + byte_idx] & (uint8_t)(1u << bit_idx)) != 0;
}

/// @brief Draw a glyph bitmap as horizontal runs of lit pixels.
///
/// Bitmap glyphs often contain adjacent lit pixels. Emitting one `fill_rect`
/// per run substantially reduces backend calls compared with one `pset` per
/// texel, and the same helper supports integer scaling by widening each run.
/// For unscaled glyphs, the helper intentionally preserves the historical
/// `pset`-per-lit-pixel behavior because Canvas tests and lightweight backends
/// observe plot coordinates directly.
/// @param win Target ZannaGFX window.
/// @param g Glyph bitmap to render.
/// @param draw_x Baseline-adjusted destination X coordinate.
/// @param draw_y Baseline-adjusted destination Y coordinate.
/// @param scale Integer scale factor; values below 1 are ignored.
/// @param color Backend RGB color.
static void bf_draw_glyph_runs(vgfx_window_t win,
                               const rt_glyph *g,
                               int64_t draw_x,
                               int64_t draw_y,
                               int64_t scale,
                               vgfx_color_t color) {
    int rb;
    if (!g || !g->bitmap || scale < 1)
        return;
    rb = bf_row_bytes(g->width);
    for (int row = 0; row < g->height; row++) {
        int col = 0;
        while (col < g->width) {
            int run_start;
            int run_len;
            while (col < g->width && !bf_glyph_bit_is_set(g, rb, row, col))
                col++;
            if (col >= g->width)
                break;
            run_start = col;
            while (col < g->width && bf_glyph_bit_is_set(g, rb, row, col))
                col++;
            run_len = col - run_start;
            if (scale == 1) {
                for (int dx = 0; dx < run_len; dx++) {
                    vgfx_pset(win,
                              rtg_clamp_i64_to_i32(rtg_add_sat64(draw_x, run_start + dx)),
                              rtg_clamp_i64_to_i32(rtg_add_sat64(draw_y, row)),
                              color);
                }
                continue;
            }
            vgfx_fill_rect(
                win,
                rtg_clamp_i64_to_i32(rtg_add_sat64(draw_x, rtg_mul_sat64(run_start, scale))),
                rtg_clamp_i64_to_i32(rtg_add_sat64(draw_y, rtg_mul_sat64(row, scale))),
                rtg_clamp_i64_to_i32(rtg_mul_sat64(run_len, scale)),
                rtg_clamp_i64_to_i32(scale),
                color);
        }
    }
}

/// @brief Draw a single glyph at `(px, py)` using horizontal runs of lit pixels.
/// @details Resolves the destination as `(px + x_offset, py + (ascent -
///          y_offset - height))` so the BDF baseline / bearing offsets
///          translate correctly to a top-of-line `(px, py)` reference. The
///          glyph bitmap is packed MSB-left, row-major; iterates row × col
///          and emits one fill rectangle per contiguous run of set bits.
/// @param win Target graphics window.
/// @param g Glyph to render; `NULL` or bitmap-less glyphs are ignored.
/// @param px Unadjusted pen X coordinate.
/// @param py Top-of-line Y coordinate.
/// @param ascent Font ascent used to place the glyph relative to its baseline.
/// @param color Backend RGB color.
static void bf_draw_glyph(vgfx_window_t win,
                          const rt_glyph *g,
                          int64_t px,
                          int64_t py,
                          int16_t ascent,
                          vgfx_color_t color) {
    if (!g || !g->bitmap)
        return;

    // BDF y_offset is from baseline; we draw relative to top of line
    int64_t draw_x = rtg_add_sat64(px, g->x_offset);
    int64_t draw_y = rtg_add_sat64(py, (int64_t)ascent - g->y_offset - g->height);
    bf_draw_glyph_runs(win, g, draw_x, draw_y, 1, color);
}

/// @brief Draw a single glyph with an integer pixel scale.
/// @details Scales bearings, baseline placement, bitmap runs, and the caller's
/// subsequent advance consistently. Values below one are ignored.
/// @param win Target graphics window.
/// @param g Glyph to render; `NULL` or bitmap-less glyphs are ignored.
/// @param px Unadjusted scaled pen X coordinate.
/// @param py Top-of-line scaled Y coordinate.
/// @param ascent Unscaled font ascent.
/// @param scale Positive integer enlargement factor.
/// @param color Backend RGB color.
static void bf_draw_glyph_scaled(vgfx_window_t win,
                                 const rt_glyph *g,
                                 int64_t px,
                                 int64_t py,
                                 int16_t ascent,
                                 int64_t scale,
                                 vgfx_color_t color) {
    if (!g || !g->bitmap || scale < 1)
        return;

    int64_t draw_x = rtg_add_sat64(px, rtg_mul_sat64(g->x_offset, scale));
    int64_t draw_y =
        rtg_add_sat64(py, rtg_mul_sat64((int64_t)ascent - g->y_offset - g->height, scale));
    bf_draw_glyph_runs(win, g, draw_x, draw_y, scale, color);
}

/// @brief Draw a glyph and an opaque background covering its horizontal span.
/// @details The background includes both the glyph advance and any ink
/// overhang, then the packed glyph bitmap is rendered in the foreground
/// color.
/// @param win Target graphics window.
/// @param g Glyph whose advance and bitmap are rendered.
/// @param px Current pen X coordinate.
/// @param py Top-of-line Y coordinate.
/// @param ascent Font ascent used for baseline placement.
/// @param line_h Background rectangle height.
/// @param fg Backend foreground RGB color.
/// @param bg Backend background RGB color.
static void bf_draw_glyph_bg(vgfx_window_t win,
                             const rt_glyph *g,
                             int64_t px,
                             int64_t py,
                             int16_t ascent,
                             int16_t line_h,
                             vgfx_color_t fg,
                             vgfx_color_t bg) {
    if (!g)
        return;

    int64_t bg_left = px;
    int64_t bg_right = rtg_add_sat64(px, g->advance);

    if (g->bitmap) {
        int64_t draw_x = rtg_add_sat64(px, g->x_offset);
        int64_t draw_y = rtg_add_sat64(py, (int64_t)ascent - g->y_offset - g->height);
        if (draw_x < bg_left)
            bg_left = draw_x;
        int64_t glyph_right = rtg_add_sat64(draw_x, g->width);
        if (glyph_right > bg_right)
            bg_right = glyph_right;

        if (bg_right > bg_left) {
            vgfx_fill_rect(win,
                           rtg_clamp_i64_to_i32(bg_left),
                           rtg_clamp_i64_to_i32(py),
                           rtg_clamp_i64_to_i32(bg_right - bg_left),
                           rtg_clamp_i64_to_i32(line_h),
                           bg);
        }

        bf_draw_glyph_runs(win, g, draw_x, draw_y, 1, fg);
    } else if (bg_right > bg_left) {
        vgfx_fill_rect(win,
                       rtg_clamp_i64_to_i32(bg_left),
                       rtg_clamp_i64_to_i32(py),
                       rtg_clamp_i64_to_i32(bg_right - bg_left),
                       rtg_clamp_i64_to_i32(line_h),
                       bg);
    }
}

//=============================================================================
// Canvas Drawing — Public API
//=============================================================================

/// @brief Render a string at (x, y) on @p canvas using the bitmap font.
/// @details @p y is the top of the line; per-glyph baseline offsets are
/// applied internally. Invalid handles and `NULL` strings are no-ops.
/// @param canvas_ptr Canvas object receiving the glyph pixels.
/// @param x Initial pen X coordinate in canvas pixels.
/// @param y Top-of-line Y coordinate in canvas pixels.
/// @param text Runtime UTF-8 string to render.
/// @param font_ptr BitmapFont or SpriteFont handle.
/// @param color Runtime Canvas foreground color.
void rt_canvas_text_font(
    void *canvas_ptr, int64_t x, int64_t y, rt_string text, void *font_ptr, int64_t color) {
    rt_bitmapfont_impl *font = bitmapfont_checked(font_ptr);
    if (!canvas_ptr || !font || !text)
        return;

    rt_canvas *canvas = rt_canvas_checked(canvas_ptr);
    if (!canvas || !canvas->gfx_win)
        return;
    rt_canvas_resync_window_state(canvas);

    const char *str = rt_string_cstr(text);
    if (!str)
        return;

    vgfx_color_t col = bitmapfont_color_to_vgfx_rgb(color);
    int64_t cx = x;
    size_t len = rt_str_len(text);
    size_t index = 0;
    int codepoint = 0;

    while (bf_next_codepoint(str, len, &index, &codepoint)) {
        const rt_glyph *g = bf_get_glyph(font, codepoint);
        if (g) {
            bf_draw_glyph(canvas->gfx_win, g, cx, y, font->ascent, col);
            cx = rtg_add_sat64(cx, g->advance);
        } else {
            cx = rtg_add_sat64(cx, font->max_width);
        }
    }
}

/// @brief Like `_text_font` but fills the glyph advance × line_height background first.
/// @details Useful for opaque overlays such as status bars and editors. Each
/// background cell also expands to cover a glyph's right or left ink
/// overhang. Invalid handles and `NULL` strings are no-ops.
/// @param canvas_ptr Canvas object receiving the glyph pixels.
/// @param x Initial pen X coordinate in canvas pixels.
/// @param y Top-of-line Y coordinate in canvas pixels.
/// @param text Runtime UTF-8 string to render.
/// @param font_ptr BitmapFont or SpriteFont handle.
/// @param fg_color Runtime Canvas foreground color.
/// @param bg_color Runtime Canvas background color.
void rt_canvas_text_font_bg(void *canvas_ptr,
                            int64_t x,
                            int64_t y,
                            rt_string text,
                            void *font_ptr,
                            int64_t fg_color,
                            int64_t bg_color) {
    rt_bitmapfont_impl *font = bitmapfont_checked(font_ptr);
    if (!canvas_ptr || !font || !text)
        return;

    rt_canvas *canvas = rt_canvas_checked(canvas_ptr);
    if (!canvas || !canvas->gfx_win)
        return;
    rt_canvas_resync_window_state(canvas);

    const char *str = rt_string_cstr(text);
    if (!str)
        return;

    vgfx_color_t fg = bitmapfont_color_to_vgfx_rgb(fg_color);
    vgfx_color_t bg = bitmapfont_color_to_vgfx_rgb(bg_color);
    int64_t cx = x;
    size_t len = rt_str_len(text);
    size_t index = 0;
    int codepoint = 0;

    while (bf_next_codepoint(str, len, &index, &codepoint)) {
        const rt_glyph *g = bf_get_glyph(font, codepoint);
        if (g) {
            bf_draw_glyph_bg(canvas->gfx_win, g, cx, y, font->ascent, font->line_height, fg, bg);
            cx = rtg_add_sat64(cx, g->advance);
        } else {
            vgfx_fill_rect(canvas->gfx_win,
                           rtg_clamp_i64_to_i32(cx),
                           rtg_clamp_i64_to_i32(y),
                           rtg_clamp_i64_to_i32(font->max_width),
                           rtg_clamp_i64_to_i32(font->line_height),
                           bg);
            cx = rtg_add_sat64(cx, font->max_width);
        }
    }
}

/// @brief Render text at integer scale (each glyph pixel becomes a `scale × scale` rect).
/// @details @p scale must be at least one; smaller values and invalid handles
/// cause a silent no-op.
/// @param canvas_ptr Canvas object receiving the scaled glyph pixels.
/// @param x Initial pen X coordinate in canvas pixels.
/// @param y Top-of-line Y coordinate in canvas pixels.
/// @param text Runtime UTF-8 string to render.
/// @param font_ptr BitmapFont or SpriteFont handle.
/// @param scale Positive integer enlargement factor.
/// @param color Runtime Canvas foreground color.
void rt_canvas_text_font_scaled(void *canvas_ptr,
                                int64_t x,
                                int64_t y,
                                rt_string text,
                                void *font_ptr,
                                int64_t scale,
                                int64_t color) {
    rt_bitmapfont_impl *font = bitmapfont_checked(font_ptr);
    if (!canvas_ptr || !font || !text || scale < 1)
        return;

    rt_canvas *canvas = rt_canvas_checked(canvas_ptr);
    if (!canvas || !canvas->gfx_win)
        return;
    rt_canvas_resync_window_state(canvas);

    const char *str = rt_string_cstr(text);
    if (!str)
        return;

    vgfx_color_t col = bitmapfont_color_to_vgfx_rgb(color);
    int64_t cx = x;
    size_t len = rt_str_len(text);
    size_t index = 0;
    int codepoint = 0;

    while (bf_next_codepoint(str, len, &index, &codepoint)) {
        const rt_glyph *g = bf_get_glyph(font, codepoint);
        if (g) {
            bf_draw_glyph_scaled(canvas->gfx_win, g, cx, y, font->ascent, scale, col);
            cx = rtg_add_sat64(cx, rtg_mul_sat64(g->advance, scale));
        } else {
            cx = rtg_add_sat64(cx, rtg_mul_sat64(font->max_width, scale));
        }
    }
}

/// @brief Render text horizontally centered in the canvas at row @p y.
/// @details Uses the measured ink-and-advance bounds so glyph overhangs are
/// centered, then delegates rendering to rt_canvas_text_font().
/// @param canvas_ptr Canvas whose current window width defines the center.
/// @param y Top-of-line Y coordinate in canvas pixels.
/// @param text Runtime UTF-8 string to render.
/// @param font_ptr BitmapFont or SpriteFont handle.
/// @param color Runtime Canvas foreground color.
void rt_canvas_text_font_centered(
    void *canvas_ptr, int64_t y, rt_string text, void *font_ptr, int64_t color) {
    rt_bitmapfont_impl *font = bitmapfont_checked(font_ptr);
    if (!canvas_ptr || !font || !text)
        return;

    rt_canvas *canvas = rt_canvas_checked(canvas_ptr);
    if (!canvas || !canvas->gfx_win)
        return;

    int32_t win_w = 0, win_h = 0;
    vgfx_get_size(canvas->gfx_win, &win_w, &win_h);

    int64_t min_x = 0;
    int64_t max_x = 0;
    int8_t has_bounds = 0;
    bf_text_bounds(font, text, &min_x, &max_x, &has_bounds);
    int64_t tw = has_bounds ? (max_x - min_x) : 0;
    int64_t cx = (win_w - tw) / 2 - min_x;

    rt_canvas_text_font(canvas_ptr, cx, y, text, font_ptr, color);
}

/// @brief Render text right-aligned with @p margin pixels of padding from the canvas right edge.
/// @details Alignment uses the text's rightmost ink-or-advance extent before
/// delegating to rt_canvas_text_font().
/// @param canvas_ptr Canvas whose current window width defines the right edge.
/// @param margin Requested distance from the text's right extent to the canvas
/// right edge.
/// @param y Top-of-line Y coordinate in canvas pixels.
/// @param text Runtime UTF-8 string to render.
/// @param font_ptr BitmapFont or SpriteFont handle.
/// @param color Runtime Canvas foreground color.
void rt_canvas_text_font_right(
    void *canvas_ptr, int64_t margin, int64_t y, rt_string text, void *font_ptr, int64_t color) {
    rt_bitmapfont_impl *font = bitmapfont_checked(font_ptr);
    if (!canvas_ptr || !font || !text)
        return;

    rt_canvas *canvas = rt_canvas_checked(canvas_ptr);
    if (!canvas || !canvas->gfx_win)
        return;

    int32_t win_w = 0, win_h = 0;
    vgfx_get_size(canvas->gfx_win, &win_w, &win_h);

    int64_t min_x = 0;
    int64_t max_x = 0;
    int8_t has_bounds = 0;
    bf_text_bounds(font, text, &min_x, &max_x, &has_bounds);
    int64_t cx = has_bounds ? (win_w - margin - max_x) : (win_w - margin);

    rt_canvas_text_font(canvas_ptr, cx, y, text, font_ptr, color);
}

#else // !ZANNA_ENABLE_GRAPHICS — stubs

/// @brief Stub used when graphics are not compiled in; raises an InvalidOperation trap with the
/// given message.
/// @param msg Static diagnostic text associated with the unavailable API.
static void rt_bitmapfont_canvas_unavailable_(const char *msg) {
    rt_trap_raise_kind(RT_TRAP_KIND_INVALID_OPERATION, Err_InvalidOperation, 0, msg);
}

/// @brief Stub for Canvas.TextFont when ZANNA_ENABLE_GRAPHICS is undefined; raises an
/// InvalidOperation trap.
/// @param canvas Unused Canvas handle.
/// @param x Unused X coordinate.
/// @param y Unused Y coordinate.
/// @param text Unused runtime string.
/// @param font Unused font handle.
/// @param color Unused foreground color.
void rt_canvas_text_font(
    void *canvas, int64_t x, int64_t y, rt_string text, void *font, int64_t color) {
    (void)canvas;
    (void)x;
    (void)y;
    (void)text;
    (void)font;
    (void)color;
    rt_bitmapfont_canvas_unavailable_("Canvas.TextFont: graphics support not compiled in");
}

/// @brief Stub for Canvas.TextFontBg when ZANNA_ENABLE_GRAPHICS is undefined; raises an
/// InvalidOperation trap.
/// @param canvas Unused Canvas handle.
/// @param x Unused X coordinate.
/// @param y Unused Y coordinate.
/// @param text Unused runtime string.
/// @param font Unused font handle.
/// @param fg Unused foreground color.
/// @param bg Unused background color.
void rt_canvas_text_font_bg(
    void *canvas, int64_t x, int64_t y, rt_string text, void *font, int64_t fg, int64_t bg) {
    (void)canvas;
    (void)x;
    (void)y;
    (void)text;
    (void)font;
    (void)fg;
    (void)bg;
    rt_bitmapfont_canvas_unavailable_("Canvas.TextFontBg: graphics support not compiled in");
}

/// @brief Stub for Canvas.TextFontScaled when ZANNA_ENABLE_GRAPHICS is undefined; raises an
/// InvalidOperation trap.
/// @param canvas Unused Canvas handle.
/// @param x Unused X coordinate.
/// @param y Unused Y coordinate.
/// @param text Unused runtime string.
/// @param font Unused font handle.
/// @param scale Unused scale factor.
/// @param color Unused foreground color.
void rt_canvas_text_font_scaled(
    void *canvas, int64_t x, int64_t y, rt_string text, void *font, int64_t scale, int64_t color) {
    (void)canvas;
    (void)x;
    (void)y;
    (void)text;
    (void)font;
    (void)scale;
    (void)color;
    rt_bitmapfont_canvas_unavailable_("Canvas.TextFontScaled: graphics support not compiled in");
}

/// @brief Stub for Canvas.TextFontCentered when ZANNA_ENABLE_GRAPHICS is undefined; raises an
/// InvalidOperation trap.
/// @param canvas Unused Canvas handle.
/// @param y Unused Y coordinate.
/// @param text Unused runtime string.
/// @param font Unused font handle.
/// @param color Unused foreground color.
void rt_canvas_text_font_centered(
    void *canvas, int64_t y, rt_string text, void *font, int64_t color) {
    (void)canvas;
    (void)y;
    (void)text;
    (void)font;
    (void)color;
    rt_bitmapfont_canvas_unavailable_("Canvas.TextFontCentered: graphics support not compiled in");
}

/// @brief Stub for Canvas.TextFontRight when ZANNA_ENABLE_GRAPHICS is undefined; raises an
/// InvalidOperation trap.
/// @param canvas Unused Canvas handle.
/// @param margin Unused right-edge margin.
/// @param y Unused Y coordinate.
/// @param text Unused runtime string.
/// @param font Unused font handle.
/// @param color Unused foreground color.
void rt_canvas_text_font_right(
    void *canvas, int64_t margin, int64_t y, rt_string text, void *font, int64_t color) {
    (void)canvas;
    (void)margin;
    (void)y;
    (void)text;
    (void)font;
    (void)color;
    rt_bitmapfont_canvas_unavailable_("Canvas.TextFontRight: graphics support not compiled in");
}

#endif // ZANNA_ENABLE_GRAPHICS
