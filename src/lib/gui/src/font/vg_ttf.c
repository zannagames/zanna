//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: lib/gui/src/font/vg_ttf.c
// Purpose: TrueType font parser — reads binary TTF tables (head, hhea, maxp,
//          cmap, glyf, loca, hmtx, kern, name) and extracts glyph outlines,
//          horizontal metrics, kerning pairs, and naming strings.
// Key invariants:
//   - All multi-byte reads use the ttf_read_u*/ttf_read_i* helpers that handle
//     big-endian conversion; never read raw fields directly.
//   - Bounds checks (ttf_range_fits, ttf_cursor_can_read) guard every read
//     that could be influenced by attacker-controlled font data.
//   - Composite glyph recursion is capped at TTF_MAX_COMPOSITE_DEPTH (16).
//   - CMAP format 12 is preferred over format 4; both may coexist in the font.
// Ownership/Lifetime:
//   - All arrays allocated by parse functions are owned by the vg_font and
//     freed in vg_font_destroy (via vg_font_destroy's direct free calls).
//   - ttf_get_glyph_outline returns heap-allocated arrays; caller frees them.
// Links: lib/gui/src/font/vg_ttf_internal.h,
//        lib/gui/src/font/vg_font.c,
//        lib/gui/src/font/vg_raster.c
//
//===----------------------------------------------------------------------===//
#include "vg_ttf_internal.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/// @file
/// @brief Implements bounded TrueType table parsing, glyph lookup, metrics, and outlines.
/// @details The parser reads big-endian SFNT data through explicit range checks, supports the first
/// face of a TrueType Collection, retains selected table offsets for lazy glyph access, and expands
/// simple or recursively composed `glyf` records into caller-owned contour arrays.

#define TTF_MAX_COMPOSITE_DEPTH 16

/// @brief Test whether a byte range lies entirely within a buffer.
/// @param total Total buffer length in bytes.
/// @param offset Proposed range start.
/// @param length Proposed range length.
/// @return `true` when `[offset, offset + length)` is contained in `[0, total)`.
static bool ttf_range_fits(size_t total, uint32_t offset, uint32_t length) {
    return (size_t)offset <= total && (size_t)length <= total - (size_t)offset;
}

/// @brief Test whether a cursor can consume a requested number of bytes.
/// @param p Current read cursor.
/// @param end One-past-the-end pointer for the bounded source.
/// @param n Number of bytes requested.
/// @return `true` when @p p is not past @p end and at least @p n bytes remain.
static bool ttf_cursor_can_read(const uint8_t *p, const uint8_t *end, size_t n) {
    return p <= end && (size_t)(end - p) >= n;
}

/// @brief Read one byte from a bounded cursor.
/// @param[in,out] p Read cursor advanced by one byte on success.
/// @param end One-past-the-end pointer for the source.
/// @param[out] out Receives the decoded byte.
/// @return `true` on success, or `false` without advancing @p p when insufficient data remains.
static bool ttf_read_u8_checked(const uint8_t **p, const uint8_t *end, uint8_t *out) {
    if (!ttf_cursor_can_read(*p, end, 1))
        return false;
    *out = **p;
    (*p)++;
    return true;
}

/// @brief Read one big-endian unsigned 16-bit value from a bounded cursor.
/// @param[in,out] p Read cursor advanced by two bytes on success.
/// @param end One-past-the-end pointer for the source.
/// @param[out] out Receives the decoded value.
/// @return `true` on success, or `false` without advancing @p p when insufficient data remains.
static bool ttf_read_u16_checked(const uint8_t **p, const uint8_t *end, uint16_t *out) {
    if (!ttf_cursor_can_read(*p, end, 2))
        return false;
    *out = ttf_read_u16(*p);
    *p += 2;
    return true;
}

/// @brief Read one big-endian signed 16-bit value from a bounded cursor.
/// @param[in,out] p Read cursor advanced by two bytes on success.
/// @param end One-past-the-end pointer for the source.
/// @param[out] out Receives the decoded value.
/// @return `true` on success, or `false` without advancing @p p when insufficient data remains.
static bool ttf_read_i16_checked(const uint8_t **p, const uint8_t *end, int16_t *out) {
    uint16_t value;
    if (!ttf_read_u16_checked(p, end, &value))
        return false;
    *out = (int16_t)value;
    return true;
}

/// @brief Test whether multiplying two allocation dimensions would overflow.
/// @param a First factor.
/// @param b Second factor.
/// @return `true` when `a * b` is not representable as `size_t`.
static bool ttf_mul_size_overflows(size_t a, size_t b) {
    return a != 0 && b > SIZE_MAX / a;
}

//=============================================================================
// Table Finding
//=============================================================================

/// @brief Locate a table in the active SFNT directory.
/// @details Table offsets remain absolute to the full font buffer, including when the directory
/// belongs to the first member of a TrueType Collection.
/// @param font Font containing the active directory and backing data.
/// @param tag Four-byte table tag in big-endian packed form.
/// @param[out] out_len Optional destination for the table byte length; cleared before searching.
/// @return Borrowed pointer into `font->data`, or NULL when the directory/table range is invalid or
/// the tag is absent.
static const uint8_t *ttf_find_table(vg_font_t *font, uint32_t tag, uint32_t *out_len) {
    const uint8_t *data = font->data + font->sfnt_base;
    if (out_len)
        *out_len = 0;

    /* Minimum header size: 12 bytes (sfVersion + numTables + searchRange + ...) */
    if (font->data_size < font->sfnt_base + 12)
        return NULL;

    uint16_t num_tables = ttf_read_u16(data + 4);

    /* Validate that the entire table directory fits within the file */
    size_t directory_bytes = (size_t)font->sfnt_base + 12u + (size_t)num_tables * 16u;
    if (directory_bytes > font->data_size)
        return NULL;

    for (int i = 0; i < num_tables; i++) {
        const uint8_t *entry = data + 12 + i * 16;
        uint32_t entry_tag = ttf_read_u32(entry);
        if (entry_tag == tag) {
            uint32_t offset = ttf_read_u32(entry + 8);
            uint32_t length = ttf_read_u32(entry + 12);
            if (ttf_range_fits(font->data_size, offset, length)) {
                if (out_len)
                    *out_len = length;
                // Table offsets are file-absolute (also inside .ttc members).
                return font->data + offset;
            }
            return NULL;
        }
    }
    return NULL;
}

//=============================================================================
// Parse 'head' Table
//=============================================================================

/// @copydoc ttf_parse_head
bool ttf_parse_head(vg_font_t *font, const uint8_t *data, uint32_t len) {
    if (len < 54)
        return false;

    font->head.units_per_em = ttf_read_u16(data + 18);
    font->head.x_min = ttf_read_i16(data + 36);
    font->head.y_min = ttf_read_i16(data + 38);
    font->head.x_max = ttf_read_i16(data + 40);
    font->head.y_max = ttf_read_i16(data + 42);
    font->head.index_to_loc_format = ttf_read_i16(data + 50);

    return font->head.units_per_em > 0 &&
           (font->head.index_to_loc_format == 0 || font->head.index_to_loc_format == 1);
}

//=============================================================================
// Parse 'hhea' Table
//=============================================================================

/// @copydoc ttf_parse_hhea
bool ttf_parse_hhea(vg_font_t *font, const uint8_t *data, uint32_t len) {
    if (len < 36)
        return false;

    font->hhea.ascent = ttf_read_i16(data + 4);
    font->hhea.descent = ttf_read_i16(data + 6);
    font->hhea.line_gap = ttf_read_i16(data + 8);
    font->hhea.num_h_metrics = ttf_read_u16(data + 34);

    return true;
}

//=============================================================================
// Parse 'maxp' Table
//=============================================================================

/// @copydoc ttf_parse_maxp
bool ttf_parse_maxp(vg_font_t *font, const uint8_t *data, uint32_t len) {
    if (len < 6)
        return false;

    font->maxp.num_glyphs = ttf_read_u16(data + 4);
    return font->maxp.num_glyphs > 0;
}

//=============================================================================
// Parse 'cmap' Table
//=============================================================================

/// @brief Release every stored CMAP format 4 array and reset its metadata.
/// @param font Font whose owned format 4 mapping data is cleared.
static void ttf_free_cmap4(vg_font_t *font) {
    free(font->cmap4_end_codes);
    free(font->cmap4_start_codes);
    free(font->cmap4_id_deltas);
    free(font->cmap4_id_range_offsets);
    free(font->cmap4_glyph_ids);
    font->cmap4_end_codes = NULL;
    font->cmap4_start_codes = NULL;
    font->cmap4_id_deltas = NULL;
    font->cmap4_id_range_offsets = NULL;
    font->cmap4_glyph_ids = NULL;
    font->cmap4_seg_count = 0;
    font->cmap4_glyph_ids_count = 0;
}

/// @brief Release every stored CMAP format 12 array and reset its metadata.
/// @param font Font whose owned format 12 mapping data is cleared.
static void ttf_free_cmap12(vg_font_t *font) {
    free(font->cmap12_start_codes);
    free(font->cmap12_end_codes);
    free(font->cmap12_start_glyph_ids);
    font->cmap12_start_codes = NULL;
    font->cmap12_end_codes = NULL;
    font->cmap12_start_glyph_ids = NULL;
    font->cmap12_num_groups = 0;
}

/// @brief Parse a CMAP format-4 subtable (BMP segment mapping); allocates all
///        cmap4_* arrays in the font struct.
/// @param font Font that takes ownership of the decoded mapping arrays.
/// @param subtable First byte of the candidate format 4 subtable.
/// @param available_len Number of readable bytes beginning at @p subtable.
/// @return `true` when a structurally valid mapping is stored, otherwise `false`.
static bool ttf_parse_cmap_format4(vg_font_t *font,
                                   const uint8_t *subtable,
                                   uint32_t available_len) {
    if (available_len < 16)
        return false;

    uint16_t length = ttf_read_u16(subtable + 2);
    uint16_t seg_count_x2 = ttf_read_u16(subtable + 6);
    if (length > available_len || length < 16 || (seg_count_x2 & 1u) != 0)
        return false;

    uint16_t seg_count = seg_count_x2 / 2;

    if (seg_count == 0)
        return false;

    size_t minimum_len = 16u + (size_t)seg_count * 8u;
    if (minimum_len > length)
        return false;

    // Allocate arrays
    if (ttf_mul_size_overflows(seg_count, sizeof(uint16_t)) ||
        ttf_mul_size_overflows(seg_count, sizeof(int16_t)))
        return false;

    font->cmap4_end_codes = malloc(seg_count * sizeof(uint16_t));
    font->cmap4_start_codes = malloc(seg_count * sizeof(uint16_t));
    font->cmap4_id_deltas = malloc(seg_count * sizeof(int16_t));
    font->cmap4_id_range_offsets = malloc(seg_count * sizeof(uint16_t));

    if (!font->cmap4_end_codes || !font->cmap4_start_codes || !font->cmap4_id_deltas ||
        !font->cmap4_id_range_offsets) {
        ttf_free_cmap4(font);
        return false;
    }

    const uint8_t *p = subtable + 14;
    const uint8_t *end = subtable + length;

    // End codes
    for (int i = 0; i < seg_count; i++) {
        if (!ttf_cursor_can_read(p, end, 2)) {
            ttf_free_cmap4(font);
            return false;
        }
        font->cmap4_end_codes[i] = ttf_read_u16(p);
        p += 2;
    }

    if (!ttf_cursor_can_read(p, end, 2)) {
        ttf_free_cmap4(font);
        return false;
    }
    p += 2; // Skip reserved pad

    // Start codes
    for (int i = 0; i < seg_count; i++) {
        if (!ttf_cursor_can_read(p, end, 2)) {
            ttf_free_cmap4(font);
            return false;
        }
        font->cmap4_start_codes[i] = ttf_read_u16(p);
        p += 2;
        if (font->cmap4_start_codes[i] > font->cmap4_end_codes[i]) {
            ttf_free_cmap4(font);
            return false;
        }
    }

    // ID deltas
    for (int i = 0; i < seg_count; i++) {
        if (!ttf_cursor_can_read(p, end, 2)) {
            ttf_free_cmap4(font);
            return false;
        }
        font->cmap4_id_deltas[i] = ttf_read_i16(p);
        p += 2;
    }

    // ID range offsets
    for (int i = 0; i < seg_count; i++) {
        if (!ttf_cursor_can_read(p, end, 2)) {
            ttf_free_cmap4(font);
            return false;
        }
        font->cmap4_id_range_offsets[i] = ttf_read_u16(p);
        p += 2;
    }

    // Glyph ID array follows (remaining bytes)
    size_t glyph_ids_bytes = (size_t)(end - p);
    font->cmap4_glyph_ids_count = (uint32_t)(glyph_ids_bytes / 2u);
    if (font->cmap4_glyph_ids_count > 0) {
        font->cmap4_glyph_ids = malloc(font->cmap4_glyph_ids_count * sizeof(uint16_t));
        if (!font->cmap4_glyph_ids) {
            ttf_free_cmap4(font);
            return false;
        }
        for (uint32_t i = 0; i < font->cmap4_glyph_ids_count; i++) {
            if (!ttf_cursor_can_read(p, end, 2)) {
                ttf_free_cmap4(font);
                return false;
            }
            font->cmap4_glyph_ids[i] = ttf_read_u16(p);
            p += 2;
        }
    }

    font->cmap4_seg_count = seg_count;
    return true;
}

/// @brief Parse a CMAP format-12 subtable (full Unicode group mapping); allocates
///        all cmap12_* arrays in the font struct.
/// @param font Font that takes ownership of the decoded group arrays.
/// @param subtable First byte of the candidate format 12 subtable.
/// @param available_len Number of readable bytes beginning at @p subtable.
/// @return `true` when a structurally valid group mapping is stored, otherwise `false`.
static bool ttf_parse_cmap_format12(vg_font_t *font,
                                    const uint8_t *subtable,
                                    uint32_t available_len) {
    if (available_len < 16)
        return false;

    uint32_t length = ttf_read_u32(subtable + 4);
    uint32_t num_groups = ttf_read_u32(subtable + 12);

    if (num_groups == 0)
        return false;
    if (length < 16 || length > available_len)
        return false;
    if (num_groups > (length - 16u) / 12u)
        return false;
    if (ttf_mul_size_overflows(num_groups, sizeof(uint32_t)))
        return false;

    font->cmap12_start_codes = malloc(num_groups * sizeof(uint32_t));
    font->cmap12_end_codes = malloc(num_groups * sizeof(uint32_t));
    font->cmap12_start_glyph_ids = malloc(num_groups * sizeof(uint32_t));

    if (!font->cmap12_start_codes || !font->cmap12_end_codes || !font->cmap12_start_glyph_ids) {
        ttf_free_cmap12(font);
        return false;
    }

    const uint8_t *p = subtable + 16;
    const uint8_t *end = subtable + length;
    for (uint32_t i = 0; i < num_groups; i++) {
        if (!ttf_cursor_can_read(p, end, 12)) {
            ttf_free_cmap12(font);
            return false;
        }
        font->cmap12_start_codes[i] = ttf_read_u32(p);
        font->cmap12_end_codes[i] = ttf_read_u32(p + 4);
        font->cmap12_start_glyph_ids[i] = ttf_read_u32(p + 8);
        if (font->cmap12_start_codes[i] > font->cmap12_end_codes[i] ||
            font->cmap12_end_codes[i] > 0x10FFFFu) {
            ttf_free_cmap12(font);
            return false;
        }
        p += 12;
    }

    font->cmap12_num_groups = num_groups;
    return true;
}

/// @copydoc ttf_parse_cmap
bool ttf_parse_cmap(vg_font_t *font, const uint8_t *data, uint32_t len) {
    if (len < 4)
        return false;

    uint16_t num_tables = ttf_read_u16(data + 2);
    if (4u + (size_t)num_tables * 8u > len)
        return false;

    // Look for format 12 (full Unicode) first, then format 4 (BMP)
    const uint8_t *format4_subtable = NULL;
    const uint8_t *format12_subtable = NULL;
    uint32_t format4_len = 0;
    uint32_t format12_len = 0;

    for (int i = 0; i < num_tables; i++) {
        const uint8_t *record = data + 4 + i * 8;
        uint16_t platform_id = ttf_read_u16(record);
        uint32_t offset = ttf_read_u32(record + 4);

        if (offset > len || len - offset < 2)
            continue;

        const uint8_t *subtable = data + offset;
        uint32_t subtable_len = len - offset;
        uint16_t format = ttf_read_u16(subtable);

        // Prefer Unicode platform (0) or Windows (3)
        if (platform_id == 0 || platform_id == 3) {
            if (format == 4 && !format4_subtable) {
                format4_subtable = subtable;
                format4_len = subtable_len;
            } else if (format == 12 && !format12_subtable) {
                format12_subtable = subtable;
                format12_len = subtable_len;
            }
        }
    }

    // Parse format 12 if available (full Unicode support)
    if (format12_subtable) {
        ttf_parse_cmap_format12(font, format12_subtable, format12_len);
    }

    // Always try to parse format 4 for BMP characters
    if (format4_subtable) {
        if (!ttf_parse_cmap_format4(font, format4_subtable, format4_len)) {
            return font->cmap12_num_groups > 0; // OK if we have parsed format 12
        }
    }

    return font->cmap4_seg_count > 0 || font->cmap12_num_groups > 0;
}

//=============================================================================
// Parse 'kern' Table
//=============================================================================

/// @copydoc ttf_parse_kern
bool ttf_parse_kern(vg_font_t *font, const uint8_t *data, uint32_t len) {
    if (len < 4)
        return false;

    uint16_t num_tables = ttf_read_u16(data + 2);

    const uint8_t *p = data + 4;
    const uint8_t *end = data + len;

    for (int t = 0; t < num_tables; t++) {
        if (!ttf_cursor_can_read(p, end, 6))
            break;

        uint16_t subtable_length = ttf_read_u16(p + 2);
        uint16_t coverage = ttf_read_u16(p + 4);
        if (subtable_length < 6 || !ttf_cursor_can_read(p, end, subtable_length))
            break;

        // Only support format 0 (ordered list of kerning pairs)
        uint8_t format = coverage >> 8;
        if (format == 0 && subtable_length >= 14) {
            uint16_t num_pairs = ttf_read_u16(p + 6);
            size_t pair_bytes = (size_t)num_pairs * 6u;
            if (pair_bytes > (size_t)subtable_length - 14u)
                break;

            if (num_pairs > 0) {
                if (ttf_mul_size_overflows(num_pairs, sizeof(ttf_kern_pair_t)))
                    break;
                font->kern_pairs = malloc(num_pairs * sizeof(ttf_kern_pair_t));
                if (font->kern_pairs) {
                    font->kern_pair_count = num_pairs;

                    const uint8_t *pair_data = p + 14;
                    for (uint16_t i = 0; i < num_pairs; i++) {
                        font->kern_pairs[i].left = ttf_read_u16(pair_data);
                        font->kern_pairs[i].right = ttf_read_u16(pair_data + 2);
                        font->kern_pairs[i].value = ttf_read_i16(pair_data + 4);
                        pair_data += 6;
                    }
                }
            }
            break; // Only use first subtable
        }

        p += subtable_length;
    }

    return true;
}

//=============================================================================
// Parse 'name' Table
//=============================================================================

/// @copydoc ttf_parse_name
bool ttf_parse_name(vg_font_t *font, const uint8_t *data, uint32_t len) {
    if (len < 6)
        return false;

    uint16_t count = ttf_read_u16(data + 2);
    uint16_t string_offset = ttf_read_u16(data + 4);
    if ((size_t)string_offset > len)
        return false;
    if (6u + (size_t)count * 12u > len)
        count = (uint16_t)((len - 6u) / 12u);

    const uint8_t *string_storage = data + string_offset;

    for (int i = 0; i < count; i++) {
        const uint8_t *record = data + 6 + i * 12;
        if (record + 12 > data + len)
            break;

        uint16_t platform_id = ttf_read_u16(record);
        uint16_t encoding_id = ttf_read_u16(record + 2);
        uint16_t name_id = ttf_read_u16(record + 6);
        uint16_t length = ttf_read_u16(record + 8);
        uint16_t offset = ttf_read_u16(record + 10);

        if ((size_t)offset > (size_t)len - string_offset ||
            (size_t)length > (size_t)len - string_offset - offset)
            continue;

        const uint8_t *str = string_storage + offset;

        // Name ID 1 = Font Family, Name ID 2 = Font Subfamily
        char *dest = NULL;
        size_t dest_size = 0;

        if (name_id == 1 && font->family_name[0] == '\0') {
            dest = font->family_name;
            dest_size = sizeof(font->family_name);
        } else if (name_id == 2 && font->style_name[0] == '\0') {
            dest = font->style_name;
            dest_size = sizeof(font->style_name);
        }

        if (dest) {
            // Platform 3 (Windows) uses UTF-16BE
            if (platform_id == 3 && encoding_id == 1) {
                int j = 0;
                for (int k = 0; k + 1 < length && j < (int)dest_size - 1; k += 2) {
                    uint16_t ch = ttf_read_u16(str + k);
                    if (ch < 128) {
                        dest[j++] = (char)ch;
                    }
                }
                dest[j] = '\0';
            }
            // Platform 1 (Mac) uses Mac Roman (treat as ASCII for basic chars)
            else if (platform_id == 1) {
                int copy_len = length < (int)dest_size - 1 ? length : (int)dest_size - 1;
                memcpy(dest, str, copy_len);
                dest[copy_len] = '\0';
            }
        }
    }

    return true;
}

//=============================================================================
// Parse All Tables
//=============================================================================

/// @brief Compare kerning pairs by their packed `(left, right)` glyph key.
/// @param a Address of the first @ref ttf_kern_pair_t.
/// @param b Address of the second @ref ttf_kern_pair_t.
/// @return A negative, zero, or positive value according to ascending packed-key order.
static int ttf_kern_pair_cmp(const void *a, const void *b) {
    const ttf_kern_pair_t *pa = (const ttf_kern_pair_t *)a;
    const ttf_kern_pair_t *pb = (const ttf_kern_pair_t *)b;
    uint32_t ka = ((uint32_t)pa->left << 16) | pa->right;
    uint32_t kb = ((uint32_t)pb->left << 16) | pb->right;
    return (ka > kb) - (ka < kb);
}

/// @copydoc ttf_parse_tables
bool ttf_parse_tables(vg_font_t *font) {
    const uint8_t *data = font->data;

    // TrueType Collections (.ttc): use the first face's table directory.
    // Table offsets inside the directory stay file-absolute, so only the
    // directory location is re-based.
    uint32_t sfnt_version = ttf_read_u32(data);
    if (sfnt_version == TTF_TAG('t', 't', 'c', 'f')) {
        if (font->data_size < 16)
            return false;
        uint32_t num_fonts = ttf_read_u32(data + 8);
        if (num_fonts == 0)
            return false;
        uint32_t first_offset = ttf_read_u32(data + 12);
        if ((size_t)first_offset + 12 > font->data_size)
            return false;
        font->sfnt_base = first_offset;
        sfnt_version = ttf_read_u32(data + first_offset);
    }

    // Validate sfnt version
    if (sfnt_version != 0x00010000 && sfnt_version != TTF_TAG('t', 'r', 'u', 'e')) {
        // Not a TrueType font
        return false;
    }

    uint32_t len;
    const uint8_t *table;

    // Required tables
    table = ttf_find_table(font, TTF_TAG_HEAD, &len);
    if (!table || !ttf_parse_head(font, table, len))
        return false;

    table = ttf_find_table(font, TTF_TAG_HHEA, &len);
    if (!table || !ttf_parse_hhea(font, table, len))
        return false;

    table = ttf_find_table(font, TTF_TAG_MAXP, &len);
    if (!table || !ttf_parse_maxp(font, table, len))
        return false;

    table = ttf_find_table(font, TTF_TAG_CMAP, &len);
    if (!table || !ttf_parse_cmap(font, table, len))
        return false;
    font->cmap_offset = (uint32_t)(table - font->data);
    font->cmap_len = len;

    // Store offsets for tables we'll need later
    table = ttf_find_table(font, TTF_TAG_GLYF, &len);
    if (table) {
        font->glyf_offset = (uint32_t)(table - font->data);
        font->glyf_len = len;
    }

    table = ttf_find_table(font, TTF_TAG_LOCA, &len);
    if (table) {
        font->loca_offset = (uint32_t)(table - font->data);
        font->loca_len = len;
    }

    table = ttf_find_table(font, TTF_TAG_HMTX, &len);
    if (table) {
        font->hmtx_offset = (uint32_t)(table - font->data);
        font->hmtx_len = len;
    }

    // Optional tables
    table = ttf_find_table(font, TTF_TAG_KERN, &len);
    if (table) {
        font->kern_offset = (uint32_t)(table - font->data);
        font->kern_len = len;
        ttf_parse_kern(font, table, len);
        // Sort kern pairs by (left<<16)|right so binary search works correctly
        if (font->kern_pairs && font->kern_pair_count > 1) {
            qsort(font->kern_pairs,
                  font->kern_pair_count,
                  sizeof(ttf_kern_pair_t),
                  ttf_kern_pair_cmp);
        }
    }

    table = ttf_find_table(font, TTF_TAG_GSUB, &len);
    if (table) {
        font->gsub_offset = (uint32_t)(table - font->data);
        font->gsub_len = len;
        vg_gsub_init(font);
    }

    table = ttf_find_table(font, TTF_TAG_NAME, &len);
    if (table) {
        font->name_offset = (uint32_t)(table - font->data);
        font->name_len = len;
        ttf_parse_name(font, table, len);
    }

    return font->glyf_offset > 0 && font->loca_offset > 0 && font->loca_len > 0;
}

//=============================================================================
// Glyph Index Lookup
//=============================================================================

/// @copydoc ttf_get_glyph_index
uint16_t ttf_get_glyph_index(vg_font_t *font, uint32_t codepoint) {
    // Try format 12 first (full Unicode)
    if (font->cmap12_num_groups > 0) {
        for (uint32_t i = 0; i < font->cmap12_num_groups; i++) {
            if (codepoint >= font->cmap12_start_codes[i] &&
                codepoint <= font->cmap12_end_codes[i]) {
                uint32_t delta = codepoint - font->cmap12_start_codes[i];
                if (delta > UINT32_MAX - font->cmap12_start_glyph_ids[i])
                    return 0;
                uint32_t glyph_id = font->cmap12_start_glyph_ids[i] + delta;
                if (glyph_id <= UINT16_MAX && glyph_id < font->maxp.num_glyphs)
                    return (uint16_t)glyph_id;
                return 0;
            }
        }
    }

    // Try format 4 (BMP only)
    if (font->cmap4_seg_count > 0 && codepoint <= 0xFFFF) {
        for (int i = 0; i < font->cmap4_seg_count; i++) {
            if (codepoint <= font->cmap4_end_codes[i]) {
                if (codepoint >= font->cmap4_start_codes[i]) {
                    if (font->cmap4_id_range_offsets[i] == 0) {
                        uint16_t glyph_id =
                            (uint16_t)((codepoint + font->cmap4_id_deltas[i]) & 0xFFFF);
                        return (glyph_id < font->maxp.num_glyphs) ? glyph_id : 0;
                    } else {
                        // Calculate glyph ID from range offset
                        int64_t idx = (int64_t)(font->cmap4_id_range_offsets[i] / 2u) +
                                      (int64_t)(codepoint - font->cmap4_start_codes[i]) -
                                      (int64_t)(font->cmap4_seg_count - i);
                        if (idx >= 0 && (uint64_t)idx < font->cmap4_glyph_ids_count) {
                            uint16_t glyph_id = font->cmap4_glyph_ids[(size_t)idx];
                            if (glyph_id != 0) {
                                glyph_id =
                                    (uint16_t)((glyph_id + font->cmap4_id_deltas[i]) & 0xFFFF);
                                return (glyph_id < font->maxp.num_glyphs) ? glyph_id : 0;
                            }
                        }
                    }
                }
                break;
            }
        }
    }

    return 0; // .notdef glyph
}

//=============================================================================
// Horizontal Metrics
//=============================================================================

/// @copydoc ttf_get_h_metrics
void ttf_get_h_metrics(vg_font_t *font,
                       uint16_t glyph_id,
                       int *advance_width,
                       int *left_side_bearing) {
    if (!advance_width || !left_side_bearing)
        return;

    if (font->hmtx_offset == 0 || font->hmtx_len == 0 || font->hhea.num_h_metrics == 0) {
        *advance_width = font->head.units_per_em;
        *left_side_bearing = 0;
        return;
    }

    const uint8_t *hmtx = font->data + font->hmtx_offset;

    if (glyph_id < font->hhea.num_h_metrics) {
        size_t offset = (size_t)glyph_id * 4u;
        if (offset > font->hmtx_len || font->hmtx_len - offset < 4u) {
            *advance_width = font->head.units_per_em;
            *left_side_bearing = 0;
            return;
        }
        *advance_width = ttf_read_u16(hmtx + glyph_id * 4);
        *left_side_bearing = ttf_read_i16(hmtx + glyph_id * 4 + 2);
    } else {
        // Use last advance width for glyphs beyond num_h_metrics
        size_t last_metric_offset = (size_t)(font->hhea.num_h_metrics - 1u) * 4u;
        size_t lsb_offset = (size_t)font->hhea.num_h_metrics * 4u +
                            (size_t)(glyph_id - font->hhea.num_h_metrics) * 2u;
        if (last_metric_offset > font->hmtx_len || font->hmtx_len - last_metric_offset < 2u ||
            lsb_offset > font->hmtx_len || font->hmtx_len - lsb_offset < 2u) {
            *advance_width = font->head.units_per_em;
            *left_side_bearing = 0;
            return;
        }
        *advance_width = ttf_read_u16(hmtx + last_metric_offset);
        // Left side bearing from array after long metrics
        *left_side_bearing = ttf_read_i16(hmtx + lsb_offset);
    }
}

//=============================================================================
// Glyph Outline
//=============================================================================

/// @brief Read one glyph boundary from the `loca` index.
/// @details Supports both short offsets, which are stored divided by two, and long 32-bit offsets.
/// The special glyph identifier equal to `num_glyphs` retrieves the final table boundary.
/// @param font Font containing validated `loca` and `glyf` metadata.
/// @param glyph_id Glyph boundary index in `[0, num_glyphs]`.
/// @param[out] out_offset Receives a byte offset relative to the `glyf` table.
/// @return `true` when the index entry exists and its value is within the `glyf` table.
static bool ttf_get_glyph_offset(vg_font_t *font, uint16_t glyph_id, uint32_t *out_offset) {
    if (!font || !out_offset || glyph_id > font->maxp.num_glyphs || font->loca_len == 0)
        return false;

    const uint8_t *loca = font->data + font->loca_offset;

    if (font->head.index_to_loc_format == 0) {
        // Short format (16-bit offsets, multiply by 2)
        size_t offset_pos = (size_t)glyph_id * 2u;
        if (offset_pos > font->loca_len || font->loca_len - offset_pos < 2u)
            return false;
        *out_offset = (uint32_t)ttf_read_u16(loca + offset_pos) * 2u;
    } else if (font->head.index_to_loc_format == 1) {
        // Long format (32-bit offsets)
        size_t offset_pos = (size_t)glyph_id * 4u;
        if (offset_pos > font->loca_len || font->loca_len - offset_pos < 4u)
            return false;
        *out_offset = ttf_read_u32(loca + offset_pos);
    } else {
        return false;
    }

    return *out_offset <= font->glyf_len;
}

/// @brief Resolve the half-open raw-data range for one glyph.
/// @param font Font containing validated `loca` and `glyf` metadata.
/// @param glyph_id Glyph identifier strictly below `num_glyphs`.
/// @param[out] out_glyph Receives the first byte of the glyph record.
/// @param[out] out_end Receives the one-past-the-end byte of the glyph record.
/// @return `true` when both consecutive `loca` boundaries are valid and ordered.
static bool ttf_get_glyph_range(vg_font_t *font,
                                uint16_t glyph_id,
                                const uint8_t **out_glyph,
                                const uint8_t **out_end) {
    if (!font || !out_glyph || !out_end || glyph_id >= font->maxp.num_glyphs)
        return false;

    uint32_t offset = 0;
    uint32_t next_offset = 0;
    if (!ttf_get_glyph_offset(font, glyph_id, &offset) ||
        !ttf_get_glyph_offset(font, (uint16_t)(glyph_id + 1u), &next_offset))
        return false;
    if (offset > next_offset || next_offset > font->glyf_len)
        return false;

    const uint8_t *glyf = font->data + font->glyf_offset;
    *out_glyph = glyf + offset;
    *out_end = glyf + next_offset;
    return true;
}

/// @brief Extract a simple or composite glyph outline with bounded recursion.
/// @param font Font containing the requested glyph.
/// @param glyph_id Font-local glyph identifier.
/// @param depth Current composite-recursion depth.
/// @param[out] out_points_x Receives an owned x-coordinate array.
/// @param[out] out_points_y Receives an owned y-coordinate array.
/// @param[out] out_flags Receives an owned on-curve flag array.
/// @param[out] out_contour_ends Receives an owned contour-end index array.
/// @param[out] out_num_points Receives the point count.
/// @param[out] out_num_contours Receives the contour count.
/// @return `true` for a decoded outline or a valid empty glyph, otherwise `false`.
static bool ttf_get_glyph_outline_internal(vg_font_t *font,
                                           uint16_t glyph_id,
                                           int depth,
                                           float **out_points_x,
                                           float **out_points_y,
                                           uint8_t **out_flags,
                                           int **out_contour_ends,
                                           int *out_num_points,
                                           int *out_num_contours);

/// @brief Decode a simple glyph's compressed flags and coordinate deltas.
/// @param font Font owning the glyph data; unused by the simple decoder.
/// @param glyph_data First byte of the glyph header.
/// @param glyph_end One-past-the-end pointer for the glyph record.
/// @param num_contours Non-negative contour count from the glyph header.
/// @param[out] out_points_x Receives an owned x-coordinate array.
/// @param[out] out_points_y Receives an owned y-coordinate array.
/// @param[out] out_flags Receives an owned on-curve flag array.
/// @param[out] out_contour_ends Receives an owned contour-end index array.
/// @param[out] out_num_points Receives the point count.
/// @param[out] out_num_contours Receives the contour count.
/// @return `true` for a decoded or valid empty outline, otherwise `false`.
static bool ttf_get_simple_glyph_outline(vg_font_t *font,
                                         const uint8_t *glyph_data,
                                         const uint8_t *glyph_end,
                                         int16_t num_contours,
                                         float **out_points_x,
                                         float **out_points_y,
                                         uint8_t **out_flags,
                                         int **out_contour_ends,
                                         int *out_num_points,
                                         int *out_num_contours);

// Composite glyph flags
#define COMP_ARG_1_AND_2_ARE_WORDS 0x0001
#define COMP_ARGS_ARE_XY_VALUES 0x0002
#define COMP_WE_HAVE_A_SCALE 0x0008
#define COMP_MORE_COMPONENTS 0x0020
#define COMP_WE_HAVE_AN_X_AND_Y_SCALE 0x0040
#define COMP_WE_HAVE_A_TWO_BY_TWO 0x0080

/// @brief Decode a composite glyph by transforming and merging component outlines.
/// @details Each referenced glyph is decoded recursively, transformed by its F2Dot14 matrix, and
/// positioned either by an explicit translation or point-to-point alignment. Component arrays are
/// merged into a single owned outline, with contour indices rebased to the accumulated point count.
/// @param font Font containing the composite and all referenced component glyphs.
/// @param glyph_data First byte of the composite glyph header.
/// @param glyph_end One-past-the-end pointer for the glyph record.
/// @param depth Current composite-recursion depth.
/// @param[out] out_points_x Receives the merged owned x-coordinate array.
/// @param[out] out_points_y Receives the merged owned y-coordinate array.
/// @param[out] out_flags Receives the merged owned on-curve flag array.
/// @param[out] out_contour_ends Receives the merged owned contour-end index array.
/// @param[out] out_num_points Receives the total point count.
/// @param[out] out_num_contours Receives the total contour count.
/// @return `true` when every component is decoded and merged, otherwise `false`.
static bool ttf_get_composite_glyph_outline(vg_font_t *font,
                                            const uint8_t *glyph_data,
                                            const uint8_t *glyph_end,
                                            int depth,
                                            float **out_points_x,
                                            float **out_points_y,
                                            uint8_t **out_flags,
                                            int **out_contour_ends,
                                            int *out_num_points,
                                            int *out_num_contours) {
    if (depth >= TTF_MAX_COMPOSITE_DEPTH)
        return false;

    // Start after the 10-byte glyph header
    const uint8_t *p = glyph_data + 10;

    // Collect all component points
    float *all_points_x = NULL;
    float *all_points_y = NULL;
    uint8_t *all_flags = NULL;
    int *all_contour_ends = NULL;
    int total_points = 0;
    int total_contours = 0;

    uint16_t flags;
    do {
        if (!ttf_read_u16_checked(&p, glyph_end, &flags))
            goto fail;
        uint16_t component_glyph_id = 0;
        if (!ttf_read_u16_checked(&p, glyph_end, &component_glyph_id))
            goto fail;

        // Read component arguments. TrueType allows either direct XY offsets
        // or point-to-point alignment between the accumulated outline and the
        // component outline.
        float dx = 0, dy = 0;
        int args_are_xy = (flags & COMP_ARGS_ARE_XY_VALUES) != 0;
        int arg1 = 0;
        int arg2 = 0;
        if (flags & COMP_ARG_1_AND_2_ARE_WORDS) {
            if (args_are_xy) {
                int16_t arg = 0;
                if (!ttf_read_i16_checked(&p, glyph_end, &arg))
                    goto fail;
                dx = (float)arg;
                if (!ttf_read_i16_checked(&p, glyph_end, &arg))
                    goto fail;
                dy = (float)arg;
            } else {
                uint16_t arg = 0;
                if (!ttf_read_u16_checked(&p, glyph_end, &arg))
                    goto fail;
                arg1 = (int)arg;
                if (!ttf_read_u16_checked(&p, glyph_end, &arg))
                    goto fail;
                arg2 = (int)arg;
            }
        } else if (args_are_xy) {
            uint8_t arg = 0;
            if (!ttf_read_u8_checked(&p, glyph_end, &arg))
                goto fail;
            dx = (float)(int8_t)arg;
            if (!ttf_read_u8_checked(&p, glyph_end, &arg))
                goto fail;
            dy = (float)(int8_t)arg;
        } else {
            uint8_t arg = 0;
            if (!ttf_read_u8_checked(&p, glyph_end, &arg))
                goto fail;
            arg1 = (int)arg;
            if (!ttf_read_u8_checked(&p, glyph_end, &arg))
                goto fail;
            arg2 = (int)arg;
        }

        float m00 = 1.0f, m01 = 0.0f, m10 = 0.0f, m11 = 1.0f;
        if (flags & COMP_WE_HAVE_A_SCALE) {
            int16_t scale = 0;
            if (!ttf_read_i16_checked(&p, glyph_end, &scale))
                goto fail;
            m00 = m11 = (float)scale / 16384.0f;
        } else if (flags & COMP_WE_HAVE_AN_X_AND_Y_SCALE) {
            int16_t scale = 0;
            if (!ttf_read_i16_checked(&p, glyph_end, &scale))
                goto fail;
            m00 = (float)scale / 16384.0f;
            if (!ttf_read_i16_checked(&p, glyph_end, &scale))
                goto fail;
            m11 = (float)scale / 16384.0f;
        } else if (flags & COMP_WE_HAVE_A_TWO_BY_TWO) {
            int16_t value = 0;
            if (!ttf_read_i16_checked(&p, glyph_end, &value))
                goto fail;
            m00 = (float)value / 16384.0f;
            if (!ttf_read_i16_checked(&p, glyph_end, &value))
                goto fail;
            m01 = (float)value / 16384.0f;
            if (!ttf_read_i16_checked(&p, glyph_end, &value))
                goto fail;
            m10 = (float)value / 16384.0f;
            if (!ttf_read_i16_checked(&p, glyph_end, &value))
                goto fail;
            m11 = (float)value / 16384.0f;
        }

        // Get component glyph outline recursively
        float *comp_x = NULL;
        float *comp_y = NULL;
        uint8_t *comp_flags = NULL;
        int *comp_contours = NULL;
        int comp_num_points = 0;
        int comp_num_contours = 0;

        if (component_glyph_id >= font->maxp.num_glyphs)
            goto fail;
        if (!ttf_get_glyph_outline_internal(font,
                                            component_glyph_id,
                                            depth + 1,
                                            &comp_x,
                                            &comp_y,
                                            &comp_flags,
                                            &comp_contours,
                                            &comp_num_points,
                                            &comp_num_contours))
            goto fail;

        if (comp_num_points > 0) {
            // Apply transformation and merge
            for (int i = 0; i < comp_num_points; i++) {
                float src_x = comp_x[i];
                float src_y = comp_y[i];
                comp_x[i] = src_x * m00 + src_y * m01 + dx;
                comp_y[i] = src_x * m10 + src_y * m11 + dy;
            }
            if (!args_are_xy) {
                if (arg1 < 0 || arg1 >= total_points || arg2 < 0 || arg2 >= comp_num_points) {
                    free(comp_x);
                    free(comp_y);
                    free(comp_flags);
                    free(comp_contours);
                    goto fail;
                }
                float align_dx = all_points_x[arg1] - comp_x[arg2];
                float align_dy = all_points_y[arg1] - comp_y[arg2];
                for (int i = 0; i < comp_num_points; i++) {
                    comp_x[i] += align_dx;
                    comp_y[i] += align_dy;
                }
            }

            // Adjust contour end indices
            for (int i = 0; i < comp_num_contours; i++) {
                comp_contours[i] += total_points;
            }

            // Expand arrays — use temporaries so a partial failure doesn't orphan
            // allocations. realloc success frees the old pointer, so checking all four
            // results after the fact can miss already-freed buffers.
            {
                if (comp_num_points > INT_MAX - total_points ||
                    comp_num_contours > INT_MAX - total_contours) {
                    free(comp_x);
                    free(comp_y);
                    free(comp_flags);
                    free(comp_contours);
                    goto fail;
                }
                size_t np = (size_t)(total_points + comp_num_points);
                size_t nc = (size_t)(total_contours + comp_num_contours);
                if (ttf_mul_size_overflows(np, sizeof(float)) ||
                    ttf_mul_size_overflows(np, sizeof(uint8_t)) ||
                    ttf_mul_size_overflows(nc, sizeof(int))) {
                    free(comp_x);
                    free(comp_y);
                    free(comp_flags);
                    free(comp_contours);
                    goto fail;
                }
                float *tmp_x = (float *)realloc(all_points_x, np * sizeof(float));
                float *tmp_y = (float *)realloc(all_points_y, np * sizeof(float));
                uint8_t *tmp_f = (uint8_t *)realloc(all_flags, np * sizeof(uint8_t));
                int *tmp_c = (int *)realloc(all_contour_ends, nc * sizeof(int));

                if (!tmp_x || !tmp_y || !tmp_f || !tmp_c) {
                    // Free whichever allocations succeeded (on success the old pointer is freed
                    // and tmp_* holds the only valid reference; on failure the original is still
                    // valid).
                    free(tmp_x ? tmp_x : all_points_x);
                    free(tmp_y ? tmp_y : all_points_y);
                    free(tmp_f ? tmp_f : all_flags);
                    free(tmp_c ? tmp_c : all_contour_ends);
                    all_points_x = all_points_y = NULL;
                    all_flags = NULL;
                    all_contour_ends = NULL;
                    free(comp_x);
                    free(comp_y);
                    free(comp_flags);
                    free(comp_contours);
                    return false;
                }

                all_points_x = tmp_x;
                all_points_y = tmp_y;
                all_flags = tmp_f;
                all_contour_ends = tmp_c;

                memcpy(all_points_x + total_points, comp_x, comp_num_points * sizeof(float));
                memcpy(all_points_y + total_points, comp_y, comp_num_points * sizeof(float));
                memcpy(all_flags + total_points, comp_flags, comp_num_points * sizeof(uint8_t));
                memcpy(all_contour_ends + total_contours,
                       comp_contours,
                       comp_num_contours * sizeof(int));

                total_points += comp_num_points;
                total_contours += comp_num_contours;
            }

            free(comp_x);
            free(comp_y);
            free(comp_flags);
            free(comp_contours);
        }
    } while (flags & COMP_MORE_COMPONENTS);

    *out_points_x = all_points_x;
    *out_points_y = all_points_y;
    *out_flags = all_flags;
    *out_contour_ends = all_contour_ends;
    *out_num_points = total_points;
    *out_num_contours = total_contours;

    return true;

fail:
    free(all_points_x);
    free(all_points_y);
    free(all_flags);
    free(all_contour_ends);
    return false;
}

/// @brief Internal recursive entry point for glyph outline extraction; @p depth
///        guards against infinite recursion in malformed composite glyphs.
/// @param font Font containing the requested glyph.
/// @param glyph_id Font-local glyph identifier.
/// @param depth Current composite-recursion depth.
/// @param[out] out_points_x Receives an owned x-coordinate array.
/// @param[out] out_points_y Receives an owned y-coordinate array.
/// @param[out] out_flags Receives an owned on-curve flag array.
/// @param[out] out_contour_ends Receives an owned contour-end index array.
/// @param[out] out_num_points Receives the point count.
/// @param[out] out_num_contours Receives the contour count.
/// @return `true` for a decoded outline or a valid empty glyph, otherwise `false`.
static bool ttf_get_glyph_outline_internal(vg_font_t *font,
                                           uint16_t glyph_id,
                                           int depth,
                                           float **out_points_x,
                                           float **out_points_y,
                                           uint8_t **out_flags,
                                           int **out_contour_ends,
                                           int *out_num_points,
                                           int *out_num_contours) {
    if (!font || !out_points_x || !out_points_y || !out_flags || !out_contour_ends ||
        !out_num_points || !out_num_contours || depth > TTF_MAX_COMPOSITE_DEPTH)
        return false;

    *out_points_x = NULL;
    *out_points_y = NULL;
    *out_flags = NULL;
    *out_contour_ends = NULL;
    *out_num_points = 0;
    *out_num_contours = 0;

    if (glyph_id >= font->maxp.num_glyphs)
        return false;

    const uint8_t *glyph = NULL;
    const uint8_t *glyph_end = NULL;
    if (!ttf_get_glyph_range(font, glyph_id, &glyph, &glyph_end))
        return false;

    // Empty glyph (like space)
    if (glyph == glyph_end)
        return true;
    if (!ttf_cursor_can_read(glyph, glyph_end, 10))
        return false;
    int16_t num_contours = ttf_read_i16(glyph);

    // Composite glyph (num_contours < 0)
    if (num_contours < 0) {
        return ttf_get_composite_glyph_outline(font,
                                               glyph,
                                               glyph_end,
                                               depth,
                                               out_points_x,
                                               out_points_y,
                                               out_flags,
                                               out_contour_ends,
                                               out_num_points,
                                               out_num_contours);
    }

    // Simple glyph - delegate to helper function
    return ttf_get_simple_glyph_outline(font,
                                        glyph,
                                        glyph_end,
                                        num_contours,
                                        out_points_x,
                                        out_points_y,
                                        out_flags,
                                        out_contour_ends,
                                        out_num_points,
                                        out_num_contours);
}

/// @copydoc ttf_get_glyph_outline
bool ttf_get_glyph_outline(vg_font_t *font,
                           uint16_t glyph_id,
                           float **out_points_x,
                           float **out_points_y,
                           uint8_t **out_flags,
                           int **out_contour_ends,
                           int *out_num_points,
                           int *out_num_contours) {
    return ttf_get_glyph_outline_internal(font,
                                          glyph_id,
                                          0,
                                          out_points_x,
                                          out_points_y,
                                          out_flags,
                                          out_contour_ends,
                                          out_num_points,
                                          out_num_contours);
}

/// @brief Decodes a simple (non-composite) glyph's raw 'glyf' table data into allocated point
/// arrays and contour end-point indices.
/// @param font Font owning the glyph data; unused by the simple decoder.
/// @param glyph_data First byte of the glyph header.
/// @param glyph_end One-past-the-end pointer for the glyph record.
/// @param num_contours Non-negative contour count from the glyph header.
/// @param[out] out_points_x Receives an owned x-coordinate array.
/// @param[out] out_points_y Receives an owned y-coordinate array.
/// @param[out] out_flags Receives an owned on-curve flag array.
/// @param[out] out_contour_ends Receives an owned contour-end index array.
/// @param[out] out_num_points Receives the point count.
/// @param[out] out_num_contours Receives the contour count.
/// @return `true` for a decoded or valid empty outline, otherwise `false`.
static bool ttf_get_simple_glyph_outline(vg_font_t *font,
                                         const uint8_t *glyph_data,
                                         const uint8_t *glyph_end,
                                         int16_t num_contours,
                                         float **out_points_x,
                                         float **out_points_y,
                                         uint8_t **out_flags,
                                         int **out_contour_ends,
                                         int *out_num_points,
                                         int *out_num_contours) {
    (void)font;                         // Unused in simple glyph case
    const uint8_t *p = glyph_data + 10; // Skip header

    if (num_contours < 0)
        return false;
    if (num_contours == 0) {
        *out_points_x = NULL;
        *out_points_y = NULL;
        *out_flags = NULL;
        *out_contour_ends = NULL;
        *out_num_points = 0;
        *out_num_contours = 0;
        return true;
    }

    // Read contour end points
    if (ttf_mul_size_overflows((size_t)num_contours, sizeof(int)))
        return false;
    int *contour_ends = malloc((size_t)num_contours * sizeof(int));
    if (!contour_ends)
        return false;

    int total_points = 0;
    for (int i = 0; i < num_contours; i++) {
        uint16_t contour_end = 0;
        if (!ttf_read_u16_checked(&p, glyph_end, &contour_end)) {
            free(contour_ends);
            return false;
        }
        if (i > 0 && contour_end <= (uint16_t)contour_ends[i - 1]) {
            free(contour_ends);
            return false;
        }
        contour_ends[i] = (int)contour_end;
        if (contour_ends[i] >= total_points) {
            total_points = contour_ends[i] + 1;
        }
    }

    // Skip instructions
    uint16_t instruction_length = 0;
    if (!ttf_read_u16_checked(&p, glyph_end, &instruction_length) ||
        !ttf_cursor_can_read(p, glyph_end, instruction_length)) {
        free(contour_ends);
        return false;
    }
    p += instruction_length;

    if (total_points <= 0 || ttf_mul_size_overflows((size_t)total_points, sizeof(float)) ||
        ttf_mul_size_overflows((size_t)total_points, sizeof(uint8_t))) {
        free(contour_ends);
        return false;
    }

    // Allocate output arrays
    float *points_x = malloc((size_t)total_points * sizeof(float));
    float *points_y = malloc((size_t)total_points * sizeof(float));
    uint8_t *flags = malloc((size_t)total_points * sizeof(uint8_t));

    if (!points_x || !points_y || !flags) {
        free(contour_ends);
        free(points_x);
        free(points_y);
        free(flags);
        return false;
    }

    // Read flags (with repeat handling)
    int flags_read = 0;
    while (flags_read < total_points) {
        uint8_t flag = 0;
        if (!ttf_read_u8_checked(&p, glyph_end, &flag))
            goto fail;
        flags[flags_read++] = flag;

        if (flag & 0x08) { // Repeat flag
            uint8_t repeat_count = 0;
            if (!ttf_read_u8_checked(&p, glyph_end, &repeat_count))
                goto fail;
            if (repeat_count > total_points - flags_read)
                goto fail;
            for (int r = 0; r < repeat_count && flags_read < total_points; r++) {
                flags[flags_read++] = flag;
            }
        }
    }

    // Read x coordinates
    int32_t x = 0;
    for (int i = 0; i < total_points; i++) {
        uint8_t flag = flags[i];
        if (flag & 0x02) { // x is 1 byte
            uint8_t byte = 0;
            if (!ttf_read_u8_checked(&p, glyph_end, &byte))
                goto fail;
            int32_t dx = byte;
            if (!(flag & 0x10))
                dx = -dx; // Sign
            x += dx;
        } else if (!(flag & 0x10)) { // x is 2 bytes
            int16_t dx = 0;
            if (!ttf_read_i16_checked(&p, glyph_end, &dx))
                goto fail;
            x += dx;
        }
        // else: x is same as previous (delta = 0)
        points_x[i] = (float)x;
    }

    // Read y coordinates
    int32_t y = 0;
    for (int i = 0; i < total_points; i++) {
        uint8_t flag = flags[i];
        if (flag & 0x04) { // y is 1 byte
            uint8_t byte = 0;
            if (!ttf_read_u8_checked(&p, glyph_end, &byte))
                goto fail;
            int32_t dy = byte;
            if (!(flag & 0x20))
                dy = -dy; // Sign
            y += dy;
        } else if (!(flag & 0x20)) { // y is 2 bytes
            int16_t dy = 0;
            if (!ttf_read_i16_checked(&p, glyph_end, &dy))
                goto fail;
            y += dy;
        }
        // else: y is same as previous (delta = 0)
        points_y[i] = (float)y;
    }

    // Convert flags to on-curve indicator (bit 0)
    for (int i = 0; i < total_points; i++) {
        flags[i] = flags[i] & 0x01; // Keep only on-curve bit
    }

    *out_points_x = points_x;
    *out_points_y = points_y;
    *out_flags = flags;
    *out_contour_ends = contour_ends;
    *out_num_points = total_points;
    *out_num_contours = num_contours;

    return true;

fail:
    free(contour_ends);
    free(points_x);
    free(points_y);
    free(flags);
    return false;
}
