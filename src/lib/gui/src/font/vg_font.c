//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: lib/gui/src/font/vg_font.c
// Purpose: Main font API implementation — loading, destruction, glyph
//          rasterization (with cache), kerning lookup, UTF-8 utilities,
//          text measurement, cursor hit-testing, and canvas text rendering.
// Key invariants:
//   - vg_font_get_glyph always returns a cache-backed pointer; the caller must
//     not free it.
//   - vg_font_draw_text delegates pixel output to the extern
//     vg_canvas_draw_glyph (implemented in vg_canvas_integration.c).
//   - UTF-8 decode advances *str past the consumed sequence even on error,
//     returning U+FFFD to allow resilient iteration.
// Ownership/Lifetime:
//   - vg_font_load copies the data buffer; the caller may free the original.
//   - vg_font_load_file allocates and frees the read buffer internally.
//   - vg_font_destroy frees the font and all sub-arrays including the cache.
// Links: lib/gui/include/vg_font.h,
//        lib/gui/src/font/vg_ttf_internal.h,
//        lib/gui/src/font/vg_cache.c,
//        lib/gui/src/font/vg_raster.c,
//        lib/gui/src/font/vg_canvas_integration.c
//
//===----------------------------------------------------------------------===//
#include "../../../../runtime/rt_platform.h"
#include "vg_ttf_internal.h"
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/// @file
/// @brief Implements font loading, glyph access, text layout, UTF-8 traversal, and canvas drawing.
/// @details Loaded TrueType faces are registered as live handles, own their parsed tables and
/// raster cache, and may borrow a fallback chain. Measurement paths read horizontal metrics
/// without rasterizing, while drawing optionally applies supported GSUB ligature substitutions.

//=============================================================================
// Live Font Registry
//=============================================================================

#define VG_FONT_MAGIC UINT64_C(0x564750464F4E5431)
#define VG_FONT_DESTROYED_MAGIC UINT64_C(0x564750464F4E5444)

static vg_font_t *g_live_fonts = NULL;

/// @brief Validate a caller-supplied rasterization or logical font size.
/// @param size Candidate size in pixels or logical points.
/// @return `true` only for finite positive values within the implementation's safety bound.
static bool vg_font_valid_size(float size);

/// @brief Add a newly allocated font to the process-local live-handle registry.
/// @details Initializes the font's magic value and links it at the head of the intrusive list.
/// @param font Font object to register; NULL is ignored.
static void vg_font_register_live(vg_font_t *font) {
    if (!font)
        return;
    font->magic = VG_FONT_MAGIC;
    font->live_prev = NULL;
    font->live_next = g_live_fonts;
    if (g_live_fonts)
        g_live_fonts->live_prev = font;
    g_live_fonts = font;
}

/// @brief Remove a font from the process-local live-handle registry.
/// @details Repairs neighboring links and clears the removed object's linkage fields.
/// @param font Registered font to unlink; NULL is ignored.
static void vg_font_unregister_live(vg_font_t *font) {
    if (!font)
        return;
    if (font->live_prev)
        font->live_prev->live_next = font->live_next;
    else if (g_live_fonts == font)
        g_live_fonts = font->live_next;
    if (font->live_next)
        font->live_next->live_prev = font->live_prev;
    font->live_prev = NULL;
    font->live_next = NULL;
}

/// @copydoc vg_font_is_live
bool vg_font_is_live(const vg_font_t *font) {
    if (!font)
        return false;
    for (const vg_font_t *live = g_live_fonts; live; live = live->live_next) {
        if (live == font)
            return live->magic == VG_FONT_MAGIC;
    }
    return false;
}

/// @copydoc vg_font_set_logical_size
void vg_font_set_logical_size(vg_font_t *font, float logical_size) {
    if (!vg_font_is_live(font))
        return;
    font->logical_size = vg_font_valid_size(logical_size) ? logical_size : 0.0f;
}

/// @copydoc vg_font_get_logical_size
float vg_font_get_logical_size(const vg_font_t *font) {
    if (!vg_font_is_live(font))
        return 0.0f;
    return vg_font_valid_size(font->logical_size) ? font->logical_size : 0.0f;
}

//=============================================================================
// Font Loading
//=============================================================================

/// @brief Return whether a font size is finite, positive, and safely bounded.
/// @param size Candidate size in pixels or logical points.
/// @return `true` for values in `(0, 1000000]`, otherwise `false`.
static bool vg_font_valid_size(float size) {
    return isfinite(size) && size > 0.0f && size <= 1000000.0f;
}

/// @brief Convert a floating-point font metric to a bounded integer.
/// @param value Metric value to convert.
/// @return The truncated integer value, zero for non-finite input, or the nearest `int` endpoint
/// when @p value lies outside the representable range.
static int vg_font_metric_to_int(double value) {
    if (!isfinite(value))
        return 0;
    if (value > (double)INT_MAX)
        return INT_MAX;
    if (value < (double)INT_MIN)
        return INT_MIN;
    return (int)value;
}

/// @brief Open a font file path using the platform's Unicode-aware API.
/// @details POSIX platforms pass UTF-8 paths directly to fopen.  Windows uses
///          `_wfopen` after converting the UTF-8 path and mode to UTF-16 so
///          fonts outside the process ANSI code page can be loaded.
/// @param path UTF-8 path to open.
/// @param mode Standard fopen mode string.
/// @return Open FILE handle, or NULL on conversion/open failure.
static FILE *vg_font_fopen_utf8(const char *path, const char *mode) {
#if RT_PLATFORM_WINDOWS
    if (!path || !mode)
        return NULL;
    int path_len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
    int mode_len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, mode, -1, NULL, 0);
    if (path_len <= 0 || mode_len <= 0)
        return NULL;
    wchar_t *wide_path = (wchar_t *)malloc((size_t)path_len * sizeof(wchar_t));
    wchar_t *wide_mode = (wchar_t *)malloc((size_t)mode_len * sizeof(wchar_t));
    if (!wide_path || !wide_mode) {
        free(wide_path);
        free(wide_mode);
        return NULL;
    }
    FILE *file = NULL;
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide_path, path_len) ==
            path_len &&
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, mode, -1, wide_mode, mode_len) ==
            mode_len) {
        file = _wfopen(wide_path, wide_mode);
    }
    free(wide_path);
    free(wide_mode);
    return file;
#else
    return fopen(path, mode);
#endif
}

/// @brief Build a font around an owned TTF data buffer.
/// @details Ownership of @p data transfers to this helper on entry. On success,
///          the returned font frees it from vg_font_destroy(); on failure, the
///          buffer is released before returning NULL.
/// @param data Heap-allocated TTF data buffer.
/// @param size Length of @p data in bytes.
/// @return A newly allocated vg_font_t, or NULL on allocation or parse failure.
static vg_font_t *vg_font_load_owned(uint8_t *data, size_t size) {
    if (!data || size < 12) {
        free(data);
        return NULL;
    }

    vg_font_t *font = calloc(1, sizeof(vg_font_t));
    if (!font) {
        free(data);
        return NULL;
    }

    font->data = data;
    font->data_size = size;
    font->owns_data = true;
    vg_font_register_live(font);

    if (!ttf_parse_tables(font)) {
        vg_font_destroy(font);
        return NULL;
    }

    font->cache = vg_cache_create();
    if (!font->cache) {
        vg_font_destroy(font);
        return NULL;
    }

    if (font->family_name[0] == '\0')
        strcpy(font->family_name, "Unknown");

    return font;
}

/// @brief Load a font from an in-memory TTF buffer.
///
/// @details Copies the buffer, parses all required TTF tables, and creates a
///          glyph cache. Returns NULL on allocation failure, parse error, or if
///          any required table is missing.
///
/// @param data Pointer to the raw TTF font data.
/// @param size Length of the data buffer in bytes (must be >= 12).
/// @return A newly allocated vg_font_t, or NULL on failure.
vg_font_t *vg_font_load(const uint8_t *data, size_t size) {
    if (!data || size < 12)
        return NULL;

    uint8_t *copy = malloc(size);
    if (!copy)
        return NULL;
    memcpy(copy, data, size);
    return vg_font_load_owned(copy, size);
}

/// @brief Load a font from a file path.
///
/// @details Reads the file in chunks into the owned font data buffer. Rejects
///          files larger than 100 MB.
///
/// @param path Null-terminated path to a TrueType font file.
/// @return A newly allocated vg_font_t, or NULL on I/O or parse failure.
vg_font_t *vg_font_load_file(const char *path) {
    if (!path)
        return NULL;

    FILE *f = vg_font_fopen_utf8(path, "rb");
    if (!f)
        return NULL;

    const size_t max_font_bytes = 100u * 1024u * 1024u;
    uint8_t *data = NULL;
    size_t size = 0;
    size_t capacity = 0;
    uint8_t chunk[8192];
    for (;;) {
        size_t n = fread(chunk, 1, sizeof(chunk), f);
        if (n > 0) {
            if (size > max_font_bytes - n) {
                free(data);
                fclose(f);
                return NULL;
            }
            size_t needed = size + n;
            if (needed > capacity) {
                size_t new_capacity = capacity ? capacity * 2u : sizeof(chunk);
                while (new_capacity < needed) {
                    if (new_capacity > max_font_bytes / 2u) {
                        new_capacity = max_font_bytes;
                        break;
                    }
                    new_capacity *= 2u;
                }
                uint8_t *new_data = realloc(data, new_capacity);
                if (!new_data) {
                    free(data);
                    fclose(f);
                    return NULL;
                }
                data = new_data;
                capacity = new_capacity;
            }
            memcpy(data + size, chunk, n);
            size = needed;
        }
        if (n < sizeof(chunk)) {
            if (ferror(f)) {
                free(data);
                fclose(f);
                return NULL;
            }
            break;
        }
    }

    fclose(f);
    return vg_font_load_owned(data, size);
}

/// @brief Destroy a font and free all associated resources.
///
/// @details Frees the glyph cache, all CMAP arrays, kerning pairs, and (when
///          owns_data is true) the font data buffer. Safe to call with NULL.
///
/// @param font The font to destroy (may be NULL).
void vg_font_destroy(vg_font_t *font) {
    if (!vg_font_is_live(font))
        return;
    vg_font_unregister_live(font);
    font->magic = VG_FONT_DESTROYED_MAGIC;

    // Free cache
    if (font->cache) {
        vg_cache_destroy(font->cache);
    }

    // Free CMAP data
    free(font->cmap4_end_codes);
    free(font->cmap4_start_codes);
    free(font->cmap4_id_deltas);
    free(font->cmap4_id_range_offsets);
    free(font->cmap4_glyph_ids);
    free(font->cmap12_start_codes);
    free(font->cmap12_end_codes);
    free(font->cmap12_start_glyph_ids);

    // Free kerning data
    free(font->kern_pairs);
    free(font->gsub_feature_lookups);

    // Free font data
    if (font->owns_data) {
        free(font->data);
    }

    free(font);
}

//=============================================================================
// Font Information
//=============================================================================

/// @brief Query typographic metrics for a font at a given pixel size.
///
/// @param font    The font to query.
/// @param size    Target font size in pixels.
/// @param metrics Output struct populated with ascent, descent, line_height,
///               and units_per_em. No-op if font or metrics is NULL.
void vg_font_get_metrics(vg_font_t *font, float size, vg_font_metrics_t *metrics) {
    if (!font || !metrics)
        return;
    metrics->ascent = 0;
    metrics->descent = 0;
    metrics->line_height = 0;
    metrics->units_per_em = font->head.units_per_em;
    if (!vg_font_valid_size(size) || font->head.units_per_em == 0)
        return;

    float scale = size / (float)font->head.units_per_em;

    metrics->ascent = vg_font_metric_to_int((double)font->hhea.ascent * scale + 0.5);
    metrics->descent =
        vg_font_metric_to_int((double)font->hhea.descent * scale - 0.5); // Usually negative
    metrics->line_height = vg_font_metric_to_int(
        (double)(font->hhea.ascent - font->hhea.descent + font->hhea.line_gap) * scale + 0.5);
    metrics->units_per_em = font->head.units_per_em;
}

/// @brief Return the font family name string parsed from the 'name' table.
///
/// @param font The font to query.
/// @return Null-terminated family name, or "Unknown" if font is NULL.
const char *vg_font_get_family(vg_font_t *font) {
    if (!font)
        return "Unknown";
    return font->family_name;
}

/// @brief Test whether the font has a glyph for the given Unicode codepoint.
///
/// @param font      The font to query.
/// @param codepoint Unicode codepoint to test.
/// @return true if a non-zero glyph index exists for the codepoint; false otherwise.
bool vg_font_has_glyph(vg_font_t *font, uint32_t codepoint) {
    if (!font)
        return false;
    if (ttf_get_glyph_index(font, codepoint) != 0)
        return true;
    return font->fallback && vg_font_has_glyph(font->fallback, codepoint);
}

//=============================================================================
// Glyph Rasterization (with caching)
//=============================================================================

/// @brief Retrieve a rasterised glyph, populating the cache on first access.
///
/// @details Checks the glyph cache first; on a miss, rasterises and inserts.
///          The returned pointer is owned by the cache and must not be freed.
///
/// @param font      The font to rasterise from.
/// @param size      Target font size in pixels (must be finite and > 0).
/// @param codepoint Unicode codepoint to look up.
/// @return Pointer to a cache-owned vg_glyph_t, or NULL on error.
const vg_glyph_t *vg_font_get_glyph(vg_font_t *font, float size, uint32_t codepoint) {
    if (!font || !vg_font_valid_size(size))
        return NULL;

    // Check cache first
    const vg_glyph_t *cached = vg_cache_get(font->cache, size, codepoint);
    if (cached)
        return cached;

    // Get glyph index
    uint16_t glyph_id = ttf_get_glyph_index(font, codepoint);

    // Per-glyph fallback: codepoints this face cannot map render from the
    // fallback chain instead of .notdef (plan 06).
    if (glyph_id == 0 && codepoint != 0 && font->fallback)
        return vg_font_get_glyph(font->fallback, size, codepoint);

    // Rasterize
    vg_glyph_t *glyph = vg_rasterize_glyph(font, glyph_id, size);
    if (!glyph)
        return NULL;

    glyph->codepoint = codepoint;

    // Add to cache
    vg_cache_put(font->cache, size, codepoint, glyph);

    // Free the temporary glyph (cache made a copy)
    free(glyph->bitmap);
    free(glyph);

    // Return cached version
    return vg_cache_get(font->cache, size, codepoint);
}

//=============================================================================
// Ligature shaping support (Zanna Studio plan 06)
//=============================================================================

static bool g_font_ligatures_enabled = true;

/// @copydoc vg_font_set_ligatures_enabled
void vg_font_set_ligatures_enabled(bool enabled) {
    g_font_ligatures_enabled = enabled;
}

/// @copydoc vg_font_ligatures_enabled
bool vg_font_ligatures_enabled(void) {
    return g_font_ligatures_enabled;
}

/// @copydoc vg_font_set_fallback
void vg_font_set_fallback(vg_font_t *font, vg_font_t *fallback) {
    if (!font || font == fallback)
        return;
    font->fallback = fallback;
}

/// @copydoc vg_font_get_fallback
vg_font_t *vg_font_get_fallback(vg_font_t *font) {
    return font ? font->fallback : NULL;
}

// Cache namespace for glyph-id entries: above the Unicode range so id keys
// can never collide with codepoint keys.
#define VG_FONT_GLYPH_ID_CACHE_BASE 0x40000000u

/// @copydoc vg_font_get_glyph_by_id
const vg_glyph_t *vg_font_get_glyph_by_id(vg_font_t *font, float size, uint16_t glyph_id) {
    if (!font || !vg_font_valid_size(size))
        return NULL;
    uint32_t key = VG_FONT_GLYPH_ID_CACHE_BASE + glyph_id;
    const vg_glyph_t *cached = vg_cache_get(font->cache, size, key);
    if (cached)
        return cached;
    vg_glyph_t *glyph = vg_rasterize_glyph(font, glyph_id, size);
    if (!glyph)
        return NULL;
    glyph->codepoint = key;
    vg_cache_put(font->cache, size, key, glyph);
    free(glyph->bitmap);
    free(glyph);
    return vg_cache_get(font->cache, size, key);
}

/// @brief Return the rounded horizontal advance for a codepoint without rasterizing it.
/// @details Text measurement and cursor hit-testing only need glyph advance.  Reading
///          the `hmtx` metrics directly avoids populating the raster glyph cache
///          during query-only operations.
/// @param font Font to query.
/// @param size Font size in pixels.
/// @param codepoint Unicode codepoint whose glyph advance should be measured.
/// @return Pixel advance, rounded to match `vg_rasterize_glyph` behavior.
static float vg_font_get_codepoint_advance(vg_font_t *font, float size, uint32_t codepoint) {
    if (!font || !vg_font_valid_size(size) || font->head.units_per_em == 0)
        return 0.0f;
    uint16_t glyph_id = ttf_get_glyph_index(font, codepoint);
    if (glyph_id == 0 && codepoint != 0 && font->fallback)
        return vg_font_get_codepoint_advance(font->fallback, size, codepoint);
    int advance_width = 0;
    int left_side_bearing = 0;
    ttf_get_h_metrics(font, glyph_id, &advance_width, &left_side_bearing);
    (void)left_side_bearing;
    double scaled = (double)advance_width * (double)size / (double)font->head.units_per_em;
    if (!isfinite(scaled))
        return 0.0f;
    if (scaled > (double)INT_MAX)
        return (float)INT_MAX;
    if (scaled < (double)INT_MIN)
        return (float)INT_MIN;
    return (float)(int)(scaled + (scaled >= 0.0 ? 0.5 : -0.5));
}

//=============================================================================
// Kerning
//=============================================================================

/// @brief Return the kerning adjustment in pixels between two adjacent codepoints.
///
/// @details Converts both codepoints to glyph indices, then performs a binary
///          search over the sorted kern_pairs array. Returns 0.0 if no pair
///          is found or if the font has no kerning data.
///
/// @param font  The font to query.
/// @param size  Font size in pixels (used to scale design-unit values).
/// @param left  Unicode codepoint of the left (preceding) character.
/// @param right Unicode codepoint of the right (following) character.
/// @return Kerning adjustment in pixels (may be negative to tighten pairs).
float vg_font_get_kerning(vg_font_t *font, float size, uint32_t left, uint32_t right) {
    if (!font || font->kern_pair_count == 0)
        return 0;

    uint16_t left_id = ttf_get_glyph_index(font, left);
    uint16_t right_id = ttf_get_glyph_index(font, right);

    // Binary search in kerning pairs (they should be sorted)
    int lo = 0;
    int hi = font->kern_pair_count - 1;

    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        ttf_kern_pair_t *pair = &font->kern_pairs[mid];

        if (pair->left == left_id && pair->right == right_id) {
            float scale = size / (float)font->head.units_per_em;
            return pair->value * scale;
        }

        // Compare as 32-bit key
        uint32_t pair_key = ((uint32_t)pair->left << 16) | pair->right;
        uint32_t search_key = ((uint32_t)left_id << 16) | right_id;

        if (pair_key < search_key) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }

    return 0;
}

//=============================================================================
// UTF-8 Utilities
//=============================================================================

/// @brief Decode one UTF-8 codepoint and advance the string pointer.
///
/// @details Handles 1–4 byte sequences. On any encoding error (overlong,
///          surrogate, out-of-range), the pointer is advanced by one byte
///          and U+FFFD (replacement character) is returned to allow resilient
///          iteration.
///
/// @param str Pointer to the current position in a null-terminated UTF-8 string.
///            Advanced past the consumed sequence (or one byte on error).
/// @return The decoded Unicode codepoint, 0 at end-of-string, or U+FFFD on error.
uint32_t vg_utf8_decode(const char **str) {
    if (!str || !*str)
        return 0;

    const uint8_t *s = (const uint8_t *)*str;
    uint32_t cp = 0;

    if (s[0] == 0) {
        return 0;
    } else if ((s[0] & 0x80) == 0) {
        // 1-byte sequence (ASCII)
        cp = s[0];
        *str += 1;
    } else if ((s[0] & 0xE0) == 0xC0) {
        // 2-byte sequence
        if (s[1] == 0 || (s[1] & 0xC0) != 0x80) {
            *str += 1; // Skip invalid leading byte so callers always advance
            return 0xFFFD;
        }
        cp = ((s[0] & 0x1F) << 6) | (s[1] & 0x3F);
        if (cp < 0x80) {
            *str += 1;
            return 0xFFFD;
        }
        *str += 2;
    } else if ((s[0] & 0xF0) == 0xE0) {
        // 3-byte sequence
        if (s[1] == 0 || s[2] == 0 || (s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80) {
            *str += 1;
            return 0xFFFD;
        }
        cp = ((s[0] & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
        if (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF)) {
            *str += 1;
            return 0xFFFD;
        }
        *str += 3;
    } else if ((s[0] & 0xF8) == 0xF0) {
        // 4-byte sequence
        if (s[1] == 0 || s[2] == 0 || s[3] == 0 || (s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80 ||
            (s[3] & 0xC0) != 0x80) {
            *str += 1;
            return 0xFFFD;
        }
        cp = ((s[0] & 0x07) << 18) | ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F);
        if (cp < 0x10000 || cp > 0x10FFFF) {
            *str += 1;
            return 0xFFFD;
        }
        *str += 4;
    } else {
        // Invalid UTF-8
        *str += 1;
        return 0xFFFD; // Replacement character
    }

    return cp;
}

/// @brief Count the number of Unicode codepoints in a null-terminated UTF-8 string.
///
/// @param str Null-terminated UTF-8 string (may be NULL).
/// @return Number of decoded codepoints, or 0 if str is NULL.
int vg_utf8_strlen(const char *str) {
    if (!str)
        return 0;

    int count = 0;
    while (*str) {
        vg_utf8_decode(&str);
        count++;
    }
    return count;
}

/// @brief Return the byte offset of the codepoint at a given character index.
///
/// @param str   Null-terminated UTF-8 string.
/// @param index Zero-based character (codepoint) index to locate.
/// @return Byte offset from str to the start of the character at index, or 0 if
///         str is NULL or index is past the end.
int vg_utf8_offset(const char *str, int index) {
    if (!str)
        return 0;

    const char *start = str;
    for (int i = 0; i < index && *str; i++) {
        vg_utf8_decode(&str);
    }
    return (int)(str - start);
}

//=============================================================================
// Text Measurement
//=============================================================================

/// @brief Measure the pixel dimensions of a UTF-8 text string.
///
/// @details Iterates codepoints, accumulates advance widths including kerning,
///          and returns the total width and line height. Does not handle
///          multi-line text (newlines are counted as zero-advance glyphs).
///
/// @param font    The font to measure with.
/// @param size    Font size in pixels.
/// @param text    Null-terminated UTF-8 string to measure.
/// @param metrics Output struct receiving width, height, and glyph_count.
///               Zeroed on entry; no-op if metrics is NULL.
void vg_font_measure_text(vg_font_t *font,
                          float size,
                          const char *text,
                          vg_text_metrics_t *metrics) {
    if (!metrics)
        return;
    metrics->width = 0;
    metrics->height = 0;
    metrics->glyph_count = 0;

    if (!font || !text || !vg_font_valid_size(size))
        return;

    vg_font_metrics_t fm;
    vg_font_get_metrics(font, size, &fm);
    metrics->height = (float)fm.line_height;

    float x = 0;
    uint32_t prev_cp = 0;
    const char *p = text;

    while (*p) {
        uint32_t cp = vg_utf8_decode(&p);
        if (cp == 0)
            break;

        // Add kerning
        if (prev_cp) {
            x += vg_font_get_kerning(font, size, prev_cp, cp);
        }

        x += vg_font_get_codepoint_advance(font, size, cp);
        metrics->glyph_count++;

        prev_cp = cp;
    }

    metrics->width = x;
}

//=============================================================================
// Hit Testing
//=============================================================================

/// @brief Map a pixel x-coordinate to the nearest character index in a text string.
///
/// @details Accumulates advance widths with kerning; returns the index of the
///          character whose left-to-right midpoint is closest to target_x.
///          Returns the length of the string when target_x is past the last glyph.
///
/// @param font     The font to measure with.
/// @param size     Font size in pixels.
/// @param text     Null-terminated UTF-8 string.
/// @param target_x Pixel x-coordinate to map (relative to the text origin).
/// @return Zero-based character index, or -1 on invalid input.
int vg_font_hit_test(vg_font_t *font, float size, const char *text, float target_x) {
    if (!font || !text || !vg_font_valid_size(size))
        return -1;

    float x = 0;
    uint32_t prev_cp = 0;
    const char *p = text;
    int index = 0;

    while (*p) {
        uint32_t cp = vg_utf8_decode(&p);
        if (cp == 0)
            break;

        // Add kerning
        if (prev_cp) {
            x += vg_font_get_kerning(font, size, prev_cp, cp);
        }

        float advance = vg_font_get_codepoint_advance(font, size, cp);
        float glyph_center = x + advance * 0.5f;
        if (target_x < glyph_center) {
            return index;
        }
        x += advance;

        prev_cp = cp;
        index++;
    }

    return index; // Past end
}

/// @brief Return the pixel x-position of the cursor before a given character index.
///
/// @details Iterates codepoints accumulating advance widths and kerning until
///          target_index is reached. Used for cursor positioning in text editors.
///
/// @param font         The font to measure with.
/// @param size         Font size in pixels.
/// @param text         Null-terminated UTF-8 string.
/// @param target_index Zero-based index of the character before which the cursor
///                     should appear. Clamped to the end of the string.
/// @return Pixel x-offset from the text origin, or 0 on invalid input.
float vg_font_get_cursor_x(vg_font_t *font, float size, const char *text, int target_index) {
    if (!font || !text || !vg_font_valid_size(size) || target_index < 0)
        return 0;

    float x = 0;
    uint32_t prev_cp = 0;
    const char *p = text;
    int index = 0;

    while (*p && index < target_index) {
        uint32_t cp = vg_utf8_decode(&p);
        if (cp == 0)
            break;

        // Add kerning
        if (prev_cp) {
            x += vg_font_get_kerning(font, size, prev_cp, cp);
        }

        x += vg_font_get_codepoint_advance(font, size, cp);

        prev_cp = cp;
        index++;
    }

    return x;
}

//=============================================================================
// Text Rendering
//=============================================================================

/// @brief Composite one glyph coverage bitmap into the canvas integration layer.
/// @param canvas Opaque canvas handle accepted by the active graphics backend.
/// @param x Left bitmap edge in canvas pixels.
/// @param y Top bitmap edge in canvas pixels.
/// @param bitmap Row-major eight-bit coverage values.
/// @param width Bitmap width in pixels.
/// @param height Bitmap height in pixels.
/// @param color Packed foreground color whose low 24 bits are `0xRRGGBB`.
extern void vg_canvas_draw_glyph(
    void *canvas, int x, int y, const uint8_t *bitmap, int width, int height, uint32_t color);

/// @brief Maximum characters shaped as one ligature run before splitting.
#define VG_FONT_SHAPE_RUN_CAP 256

/// @brief Draw one shaped segment (no newlines) with ligature substitution.
/// @details Advances by the SOURCE characters' widths so text layout is
///          identical to the unshaped path — ligature glyphs render across
///          the columns they replace (plan 06 caret contract).
/// @param canvas Opaque canvas handle passed to @ref vg_canvas_draw_glyph.
/// @param font Font face providing shaping, metrics, and rasterized glyphs.
/// @param size Font size in pixels.
/// @param cursor_x Initial drawing cursor in canvas pixels.
/// @param y Baseline position in canvas pixels.
/// @param codepoints Source Unicode code points for a newline-free segment.
/// @param count Number of readable entries in @p codepoints.
/// @param color Packed foreground color whose low 24 bits are `0xRRGGBB`.
/// @return Cursor position immediately after the rendered segment.
static float vg_font_draw_shaped_segment(void *canvas,
                                         vg_font_t *font,
                                         float size,
                                         float cursor_x,
                                         float y,
                                         const uint32_t *codepoints,
                                         int32_t count,
                                         uint32_t color) {
    vg_shaped_glyph_t shaped[VG_FONT_SHAPE_RUN_CAP];
    int32_t shaped_count = vg_font_shape(font, codepoints, count, shaped, VG_FONT_SHAPE_RUN_CAP);
    for (int32_t i = 0; i < shaped_count; ++i) {
        // Kerning between adjacent single-character glyphs only (merged
        // ligatures already bake their spacing).
        if (i > 0 && shaped[i - 1].source_len == 1 && shaped[i].source_len == 1) {
            cursor_x += vg_font_get_kerning(font,
                                            size,
                                            codepoints[shaped[i - 1].source_start],
                                            codepoints[shaped[i].source_start]);
        }
        float advance = 0.0f;
        for (uint16_t s = 0; s < shaped[i].source_len; ++s)
            advance += vg_font_get_codepoint_advance(font, size, codepoints[shaped[i].source_start + s]);
        // Always rasterize the SHAPED glyph id: fonts substitute in place
        // (contextual alternates keep one glyph per character with new ids —
        // the JetBrains Mono model) or merge (classic LookupType 4).
        const vg_glyph_t *glyph =
            (shaped[i].glyph_id == 0 && font->fallback)
                ? vg_font_get_glyph(font->fallback, size, codepoints[shaped[i].source_start])
                : vg_font_get_glyph_by_id(font, size, shaped[i].glyph_id);
        if (glyph && glyph->bitmap) {
            int draw_x = (int)(cursor_x + glyph->bearing_x + 0.5f);
            int draw_y = (int)(y - glyph->bearing_y + 0.5f);
            vg_canvas_draw_glyph(
                canvas, draw_x, draw_y, glyph->bitmap, glyph->width, glyph->height, color);
        }
        cursor_x += advance;
    }
    return cursor_x;
}

/// @brief Render a UTF-8 string onto a canvas at the given baseline position.
/// @details When ligatures are enabled and supported by the face, newline-delimited runs are
/// shaped in bounded segments before rasterization. Otherwise the function iterates code points
/// directly, applying pair kerning and cached glyph advances. Newlines reset the horizontal
/// cursor and advance by the font's line height.
/// @param canvas Opaque canvas handle accepted by the active graphics backend.
/// @param font Font face used for shaping, metrics, fallback resolution, and rasterization.
/// @param size Finite positive font size in pixels.
/// @param x Left origin for each line in canvas pixels.
/// @param y Baseline of the first line in canvas pixels.
/// @param text NUL-terminated UTF-8 text.
/// @param color Packed foreground color whose low 24 bits are `0xRRGGBB`.
void vg_font_draw_text(
    void *canvas, vg_font_t *font, float size, float x, float y, const char *text, uint32_t color) {
    if (!canvas || !font || !text || size <= 0)
        return;

    if (g_font_ligatures_enabled && font->gsub_feature_lookup_count > 0) {
        // Shaped path: decode into newline-split runs and substitute
        // liga/calt ligatures per run.
        uint32_t run[VG_FONT_SHAPE_RUN_CAP];
        int32_t run_count = 0;
        float cursor_x = x;
        vg_font_metrics_t fm;
        vg_font_get_metrics(font, size, &fm);
        const char *p = text;
        for (;;) {
            uint32_t cp = *p ? vg_utf8_decode(&p) : 0;
            if (cp == 0 || cp == '\n' || run_count == VG_FONT_SHAPE_RUN_CAP) {
                if (run_count > 0)
                    cursor_x = vg_font_draw_shaped_segment(
                        canvas, font, size, cursor_x, y, run, run_count, color);
                run_count = 0;
                if (cp == '\n') {
                    cursor_x = x;
                    y += fm.line_height;
                    continue;
                }
                if (cp == 0)
                    return;
            }
            run[run_count++] = cp;
        }
    }

    float cursor_x = x;
    uint32_t prev_cp = 0;
    const char *p = text;
    vg_font_metrics_t fm;
    vg_font_get_metrics(font, size, &fm);

    while (*p) {
        uint32_t cp = vg_utf8_decode(&p);
        if (cp == 0)
            break;

        // Handle newlines
        if (cp == '\n') {
            cursor_x = x;
            y += fm.line_height;
            prev_cp = 0;
            continue;
        }

        // Add kerning
        if (prev_cp) {
            cursor_x += vg_font_get_kerning(font, size, prev_cp, cp);
        }

        // Get glyph
        const vg_glyph_t *glyph = vg_font_get_glyph(font, size, cp);
        if (glyph && glyph->bitmap) {
            // Calculate draw position
            int draw_x = (int)(cursor_x + glyph->bearing_x + 0.5f);
            int draw_y = (int)(y - glyph->bearing_y + 0.5f);

            // Draw glyph
            vg_canvas_draw_glyph(
                canvas, draw_x, draw_y, glyph->bitmap, glyph->width, glyph->height, color);
        }

        if (glyph) {
            cursor_x += glyph->advance;
        }

        prev_cp = cp;
    }
}
