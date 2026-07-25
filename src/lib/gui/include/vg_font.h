//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: lib/gui/include/vg_font.h
// Purpose: Public API for the Zanna GUI font subsystem — TTF loading, glyph
//          rasterization, text measurement, hit-testing, and rendering.
// Key invariants:
//   - Font handles are opaque and must be destroyed with vg_font_destroy.
//   - Glyph pointers from vg_font_get_glyph are valid only while the font
//     handle is alive and the cache entry has not been evicted.
//   - All text parameters are UTF-8 encoded, null-terminated C strings.
// Ownership/Lifetime:
//   - vg_font_load copies the data buffer; the caller may free it afterwards.
//   - vg_font_load_file reads and owns the file data internally.
//   - vg_font_destroy frees the font handle and all cached glyph bitmaps.
// Links: lib/gui/src/font/vg_font.c,
//        lib/gui/src/font/vg_ttf_internal.h,
//        lib/gui/include/vg_theme.h
//
//===----------------------------------------------------------------------===//
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/// @file
/// @brief Declares TrueType loading, shaping, rasterization, measurement, and UTF-8 utilities.
/// @details Font handles own copied face data and cached coverage bitmaps. Callers can query
/// metrics, shape optional ligatures, select fallback faces, measure or hit-test UTF-8 text,
/// and render glyphs onto an opaque canvas.

#ifdef __cplusplus
extern "C" {
#endif

//=============================================================================
// Forward Declarations
//=============================================================================

/// @brief Opaque loaded font face and glyph-cache handle.
typedef struct vg_font vg_font_t;

/// @brief Opaque runtime canvas accepted by font rendering integrations.
typedef struct rt_canvas rt_canvas_t;

//=============================================================================
// Glyph Information
//=============================================================================

/// @brief Rasterised glyph data for a single character at a specific size.
///
/// @details Contains the alpha-coverage bitmap, its dimensions, horizontal
///          bearing offsets (for correct placement relative to the baseline),
///          and the advance width (how far the pen moves after this glyph).
typedef struct vg_glyph {
    uint32_t codepoint; ///< Unicode codepoint this glyph represents.
    int width;          ///< Bitmap width in pixels.
    int height;         ///< Bitmap height in pixels.
    int bearing_x;      ///< Horizontal bearing: offset from the pen to the left edge of the bitmap.
    int bearing_y;      ///< Vertical bearing: offset from the baseline to the top edge of the
                        ///< bitmap.
    int advance;        ///< Horizontal advance width in pixels (pen movement after this glyph).
    uint8_t *bitmap;    ///< 8-bit alpha-coverage bitmap (owned by the glyph cache; do not free).
} vg_glyph_t;

//=============================================================================
// Font Metrics
//=============================================================================

/// @brief Global metrics for a font at a given pixel size.
typedef struct vg_font_metrics {
    int ascent;       ///< Distance from the baseline to the top of the tallest glyph (positive).
    int descent;      ///< Distance from the baseline to the bottom of the lowest glyph (negative).
    int line_height;  ///< Recommended line spacing (ascent - descent + line gap).
    int units_per_em; ///< Number of design units per em square.
} vg_font_metrics_t;

//=============================================================================
// Text Measurement
//=============================================================================

/// @brief Aggregate metrics for a measured text string.
typedef struct vg_text_metrics {
    float width;     ///< Total horizontal extent of the text in pixels.
    float height;    ///< Height of the text (typically one line_height).
    int glyph_count; ///< Number of glyphs (codepoints) in the string.
} vg_text_metrics_t;

//=============================================================================
// Font Loading
//=============================================================================

/// @brief Load a TrueType font from an in-memory buffer.
///
/// @details The data is copied internally; the caller may free the buffer
///          after this call returns. Only the first font face in the file is
///          loaded if the file contains a TrueType Collection.
///
/// @param data Pointer to the TTF file data.
/// @param size Size of the data buffer in bytes.
/// @return An opaque font handle, or NULL if parsing fails.
vg_font_t *vg_font_load(const uint8_t *data, size_t size);

/// @brief Load a TrueType font from a file on disk.
///
/// @details Reads the entire file into memory, parses the TTF tables, and
///          returns a font handle. The file is not kept open.
///
/// @param path Filesystem path to the .ttf file (UTF-8 encoded).
/// @return An opaque font handle, or NULL if the file cannot be read or parsed.
vg_font_t *vg_font_load_file(const char *path);

/// @brief Destroy a font and release all associated resources.
///
/// @details Frees the internal data buffer, the glyph cache and all cached
///          bitmaps, and the font structure itself. After this call the
///          pointer is invalid.
///
/// @param font The font to destroy (may be NULL, in which case this is a no-op).
void vg_font_destroy(vg_font_t *font);

/// @brief Return true when @p font is a currently-live font handle.
///
/// @details This is intended for runtime bindings that receive opaque user
///          handles and must reject stale or non-font pointers before storing
///          them in widgets.
/// @param font Candidate handle to validate; may be NULL or unrelated.
/// @return true only when @p font is registered as a live font object.
bool vg_font_is_live(const vg_font_t *font);

/// @brief Associate a logical-point size with a live font handle.
/// @details The font rasterizer remains size-agnostic and accepts an effective pixel size on every
///          draw call. This metadata preserves the unscaled size requested by higher-level GUI
///          APIs so DPI changes can recompute effective pixels without compounding scale. Invalid,
///          stale, non-finite, non-positive, or greater-than-one-million values clear the metadata
///          to zero rather than affecting rasterization.
/// @param font Live font handle whose metadata should change; NULL and stale handles are ignored.
/// @param logical_size Size in logical points, or an invalid value to clear the metadata.
void vg_font_set_logical_size(vg_font_t *font, float logical_size);

/// @brief Return the logical-point size associated with a live font handle.
/// @details File-loaded fonts have zero until explicitly annotated. The value is metadata only;
///          callers still pass an effective pixel size to measurement and drawing operations.
/// @param font Font to query; may be NULL, stale, or unrelated.
/// @return Finite positive logical size, or zero when unset/invalid.
float vg_font_get_logical_size(const vg_font_t *font);

//=============================================================================
// Font Information
//=============================================================================

/// @brief Retrieve global metrics for a font at a specific pixel size.
///
/// @param font    The font to query.
/// @param size    Desired finite, positive font size in pixels.
/// @param metrics Pointer to the output structure that receives the metrics.
///                Invalid sizes produce zeroed metrics with units_per_em
///                preserved when a font is supplied.
void vg_font_get_metrics(vg_font_t *font, float size, vg_font_metrics_t *metrics);

//=============================================================================
// Ligature shaping (GSUB; Zanna Studio plan 06 / ADR 0137)
//=============================================================================

/// @brief One shaped glyph and the source-character span it covers.
/// @details Plain glyphs cover one character; ligatures cover several. The
///          span keeps per-character columns/selection intact: callers
///          advance by the SOURCE characters' widths, not the ligature's.
typedef struct vg_shaped_glyph {
    uint16_t glyph_id;     ///< Font glyph index to rasterize.
    uint16_t source_start; ///< Index of the first source character covered.
    uint16_t source_len;   ///< Number of source characters merged (1 = plain).
} vg_shaped_glyph_t;

/// @brief Shape a codepoint run through the font's liga/calt GSUB lookups.
/// @param font Font to shape with; fonts without GSUB return identity runs.
/// @param codepoints Source characters (no newlines).
/// @param count Number of source characters.
/// @param out Receives shaped glyphs; capacity must be at least @p count.
/// @param out_capacity Capacity of @p out.
/// @return Number of shaped glyphs written (never exceeds @p count).
int32_t vg_font_shape(vg_font_t *font,
                      const uint32_t *codepoints,
                      int32_t count,
                      vg_shaped_glyph_t *out,
                      int32_t out_capacity);

/// @brief Return whether the font resolved any liga/calt GSUB lookups.
/// @param font Font face to inspect.
/// @return true when at least one supported ligature or contextual-alternate lookup was loaded.
bool vg_font_has_ligatures(vg_font_t *font);

/// @brief Enable or disable ligature shaping in vg_font_draw_text process-wide.
/// @details Default on; per-widget overrides push/restore around their draws.
/// @param enabled true to apply supported ligature shaping during text drawing.
void vg_font_set_ligatures_enabled(bool enabled);

/// @brief Return the process-wide ligature shaping flag.
/// @return true when text drawing currently applies supported ligature shaping.
bool vg_font_ligatures_enabled(void);

/// @brief Rasterize (and cache) a glyph by font glyph index instead of codepoint.
/// @details Backs ligature rendering; the cache keys ids in a private range
///          above Unicode so codepoint entries never collide.
/// @param font Font owning the glyph.
/// @param size Pixel size.
/// @param glyph_id Font glyph index.
/// @return Cached glyph, or NULL when rasterization fails.
const vg_glyph_t *vg_font_get_glyph_by_id(vg_font_t *font, float size, uint16_t glyph_id);

/// @brief Attach a per-glyph fallback face consulted on cmap misses (plan 06).
/// @details Borrowed reference: the caller keeps @p fallback alive for the
///          primary font's lifetime. Fallbacks may chain (fallback's own
///          fallback is consulted next). NULL clears the link.
/// @param font Primary font whose missing glyphs should consult a fallback.
/// @param fallback Borrowed fallback font, or NULL to remove the current link.
void vg_font_set_fallback(vg_font_t *font, vg_font_t *fallback);

/// @brief Return the font's per-glyph fallback face, or NULL.
/// @param font Primary font to query.
/// @return Borrowed fallback handle, or NULL when none is attached.
vg_font_t *vg_font_get_fallback(vg_font_t *font);

/// @brief Retrieve the font's family name (e.g. "Noto Sans", "Fira Code").
///
/// @param font The font to query.
/// @return A null-terminated string (read-only, owned by the font). Returns
///         an empty string if the name could not be determined.
const char *vg_font_get_family(vg_font_t *font);

/// @brief Check whether the font contains a glyph for a specific codepoint.
///
/// @param font      The font to query.
/// @param codepoint The Unicode codepoint to look up.
/// @return true if the font has a glyph for the codepoint.
bool vg_font_has_glyph(vg_font_t *font, uint32_t codepoint);

//=============================================================================
// Glyph Rasterization
//=============================================================================

/// @brief Obtain the rasterised glyph for a codepoint at a given size.
///
/// @details Looks up the glyph in the cache; if not found the glyph is
///          rasterised from the font's outline data and added to the cache.
///          The returned pointer remains valid until the font is destroyed
///          or the cache is evicted under memory pressure.
///
/// @param font      The font handle.
/// @param size      Finite, positive font size in pixels.
/// @param codepoint Unicode codepoint.
/// @return Pointer to the cached glyph data, or NULL if the glyph is missing.
const vg_glyph_t *vg_font_get_glyph(vg_font_t *font, float size, uint32_t codepoint);

/// @brief Query the kerning adjustment between two consecutive glyphs.
///
/// @details If the font includes a kern table, this returns the horizontal
///          offset that should be added between the two glyphs for proper
///          spacing. A positive value moves glyphs further apart; negative
///          brings them closer.
///
/// @param font  The font handle.
/// @param size  Font size in pixels.
/// @param left  Unicode codepoint of the left (preceding) glyph.
/// @param right Unicode codepoint of the right (following) glyph.
/// @return Kerning adjustment in pixels (may be negative).
float vg_font_get_kerning(vg_font_t *font, float size, uint32_t left, uint32_t right);

//=============================================================================
// Text Measurement
//=============================================================================

/// @brief Measure the dimensions of a UTF-8 text string at a given font size.
///
/// @param font    The font handle.
/// @param size    Font size in pixels.
/// @param text    Null-terminated UTF-8 text string.
/// @param metrics Pointer to the output structure that receives the measurements.
void vg_font_measure_text(vg_font_t *font,
                          float size,
                          const char *text,
                          vg_text_metrics_t *metrics);

/// @brief Determine which character in a string lies at a given x-pixel offset.
///
/// @details Useful for mapping a mouse click position to a cursor position in
///          a text field. Returns the zero-based character index whose bounding
///          box contains @p x, or -1 if @p x is past the end of the string.
///
/// @param font The font handle.
/// @param size Font size in pixels.
/// @param text Null-terminated UTF-8 text string.
/// @param x    Horizontal offset in pixels from the start of the string.
/// @return Zero-based character index, or -1 if @p x is beyond the last character.
int vg_font_hit_test(vg_font_t *font, float size, const char *text, float x);

/// @brief Compute the x-pixel offset of a specific character index in a string.
///
/// @details The inverse of vg_font_hit_test. Given a character index, returns
///          the x position of the left edge of that character's glyph.
///
/// @param font  The font handle.
/// @param size  Font size in pixels.
/// @param text  Null-terminated UTF-8 text string.
/// @param index Zero-based character index.
/// @return X position in pixels from the start of the string.
float vg_font_get_cursor_x(vg_font_t *font, float size, const char *text, int index);

//=============================================================================
// Text Rendering
//=============================================================================

/// @brief Render a UTF-8 text string onto a canvas at a specified position.
///
/// @details Iterates over codepoints in the string, rasterises each glyph
///          (or retrieves it from cache), applies kerning, and composites
///          the glyph bitmaps onto the canvas with the given colour.
///
/// @param canvas Opaque canvas handle (platform-specific rendering surface).
/// @param font   The font handle.
/// @param size   Font size in pixels.
/// @param x      X position for the start of the text (left edge of first glyph).
/// @param y      Y position of the text baseline.
/// @param text   Null-terminated UTF-8 text string.
/// @param color  Text colour in packed ARGB format (0xAARRGGBB).
void vg_font_draw_text(
    void *canvas, vg_font_t *font, float size, float x, float y, const char *text, uint32_t color);

//=============================================================================
// UTF-8 Utilities
//=============================================================================

/// @brief Decode the next UTF-8 codepoint from a string and advance the pointer.
///
/// @details On each call, decodes one codepoint and moves *str forward by
///          the appropriate number of bytes (1-4). Returns 0 when the string
///          terminator is reached or if a decoding error occurs.
///
/// @param[in,out] str Pointer to the current position in the string; updated
///                    to point past the decoded codepoint on return.
/// @return The decoded Unicode codepoint, or 0 on end-of-string / error.
uint32_t vg_utf8_decode(const char **str);

/// @brief Count the number of Unicode codepoints in a UTF-8 string.
///
/// @param str Null-terminated UTF-8 string.
/// @return Number of codepoints (not bytes).
int vg_utf8_strlen(const char *str);

/// @brief Convert a character index (codepoint offset) to a byte offset.
///
/// @details Walks through the UTF-8 string, counting codepoints, and returns
///          the byte offset of the codepoint at position @p index.
///
/// @param str   Null-terminated UTF-8 string.
/// @param index Zero-based character (codepoint) index.
/// @return Byte offset into the string.
int vg_utf8_offset(const char *str, int index);

#ifdef __cplusplus
}
#endif
