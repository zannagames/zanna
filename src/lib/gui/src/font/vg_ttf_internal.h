//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: lib/gui/src/font/vg_ttf_internal.h
// Purpose: Internal TrueType font parsing structures, big-endian byte-reading
//          helpers, glyph cache, and rasterization declarations. Private to the
//          font subsystem — must not be included outside lib/gui/src/font/.
// Key invariants:
//   - All multi-byte integers in TTF data are big-endian; the ttf_read_*
//     family handles byte-order conversion.
//   - The glyph cache evicts entries automatically when VG_CACHE_MAX_MEMORY
//     is exceeded.
//   - vg_font owns its data buffer when owns_data is true; freed in destroy.
//   - CMAP format 4 and format 12 arrays are heap-allocated and freed on destroy.
// Ownership/Lifetime:
//   - struct vg_font owns all heap-allocated sub-arrays and the glyph cache.
//   - vg_cache_entry_t nodes and their bitmaps are owned by the cache.
// Links: lib/gui/include/vg_font.h,
//        lib/gui/src/font/vg_font.c,
//        lib/gui/src/font/vg_ttf.c,
//        lib/gui/src/font/vg_cache.c,
//        lib/gui/src/font/vg_raster.c
//
//===----------------------------------------------------------------------===//
#pragma once

#include "../../include/vg_font.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/// @file
/// @brief Defines the private TrueType parser, rasterizer, and glyph-cache interface.
/// @details This header supplies the concrete opaque-font representation shared by the font
/// implementation units, parsed table records, endian-safe scalar readers, table tags, and
/// ownership-bearing declarations for decoded outlines and cached bitmaps.

//=============================================================================
// TTF Table Directory
//=============================================================================

/// @brief A single entry from the TTF table directory, identifying one font
///        table within the file.
///
/// @details Every TrueType font begins with a table directory that lists all
///          tables present in the file. Each entry records the table's 4-byte
///          tag, a checksum for integrity verification, the byte offset from
///          the start of the file, and the table's length in bytes.
typedef struct ttf_table {
    uint32_t tag; ///< 4-byte table identifier (e.g. 'head', 'cmap') packed as a big-endian uint32.
    uint32_t checksum; ///< Checksum of the table data for integrity verification.
    uint32_t
        offset; ///< Byte offset from the beginning of the font file to the start of this table.
    uint32_t length; ///< Length of the table data in bytes.
} ttf_table_t;

//=============================================================================
// TTF 'head' Table - Font header
//=============================================================================

/// @brief Parsed contents of the TrueType 'head' (font header) table.
///
/// @details The 'head' table contains global information about the font such
///          as the em-square size (units_per_em), the bounding box that
///          encloses all glyphs, and the format used by the 'loca' table to
///          index glyph data. Only the fields needed by the rasterizer are
///          stored here; the full 54-byte table is not retained.
typedef struct ttf_head {
    uint16_t units_per_em; ///< Number of font design units per em square (typically 1000 or 2048).
    int16_t x_min;         ///< Minimum x-coordinate across all glyph bounding boxes, in font units.
    int16_t y_min;         ///< Minimum y-coordinate across all glyph bounding boxes, in font units.
    int16_t x_max;         ///< Maximum x-coordinate across all glyph bounding boxes, in font units.
    int16_t y_max;         ///< Maximum y-coordinate across all glyph bounding boxes, in font units.
    int16_t index_to_loc_format; ///< Format of the 'loca' table: 0 = short (16-bit offsets / 2), 1
                                 ///< = long (32-bit offsets).
} ttf_head_t;

//=============================================================================
// TTF 'hhea' Table - Horizontal header
//=============================================================================

/// @brief Parsed contents of the TrueType 'hhea' (horizontal header) table.
///
/// @details The 'hhea' table provides global horizontal layout metrics:
///          the typographic ascent and descent (used to compute line height),
///          the line gap (additional inter-line spacing), and the number of
///          horizontal metric entries in the 'hmtx' table. These values are
///          in font design units and must be scaled to pixel size by the
///          caller.
typedef struct ttf_hhea {
    int16_t ascent;  ///< Typographic ascent in font units (distance from baseline to top of tallest
                     ///< glyph).
    int16_t descent; ///< Typographic descent in font units (negative; distance from baseline to
                     ///< bottom of lowest glyph).
    int16_t line_gap; ///< Additional line spacing in font units, added between descent of one line
                      ///< and ascent of the next.
    uint16_t
        num_h_metrics; ///< Number of advance-width + left-side-bearing pairs in the 'hmtx' table.
} ttf_hhea_t;

//=============================================================================
// TTF 'maxp' Table - Maximum profile
//=============================================================================

/// @brief Parsed contents of the TrueType 'maxp' (maximum profile) table.
///
/// @details The 'maxp' table declares the total number of glyphs in the font.
///          This value is used to validate glyph indices returned by the CMAP
///          lookup and to bounds-check accesses into the 'loca' and 'hmtx'
///          tables. Only the glyph count is retained here; the full table
///          contains additional maximums for composite glyphs, instruction
///          storage, etc., which are not needed by this implementation.
typedef struct ttf_maxp {
    uint16_t num_glyphs; ///< Total number of glyphs in the font (including the .notdef glyph at
                         ///< index 0).
} ttf_maxp_t;

//=============================================================================
// TTF 'hmtx' Entry - Horizontal metrics
//=============================================================================

/// @brief A single entry from the TrueType 'hmtx' (horizontal metrics) table.
///
/// @details Each entry pairs an advance width with a left-side bearing for one
///          glyph. The advance width is the total horizontal distance the pen
///          moves after drawing the glyph; the left-side bearing is the offset
///          from the pen position to the left edge of the glyph's bounding box.
///          Both values are in font design units and must be scaled to the
///          target pixel size.
typedef struct ttf_hmtx_entry {
    uint16_t advance_width;    ///< Horizontal advance width in font units.
    int16_t left_side_bearing; ///< Left-side bearing (horizontal offset from pen to glyph bbox left
                               ///< edge) in font units.
} ttf_hmtx_entry_t;

//=============================================================================
// TTF 'kern' Pair - Kerning
//=============================================================================

/// @brief A single kerning pair from the TrueType 'kern' table.
///
/// @details Kerning pairs specify horizontal spacing adjustments between
///          specific pairs of adjacent glyphs to improve visual appearance.
///          For example, the pair "AV" typically has a negative kerning value
///          to bring the glyphs closer together. The left and right fields are
///          glyph indices (not codepoints), and the value is a signed offset
///          in font design units.
typedef struct ttf_kern_pair {
    uint16_t left;  ///< Glyph index of the left (preceding) glyph in the pair.
    uint16_t right; ///< Glyph index of the right (following) glyph in the pair.
    int16_t value;  ///< Kerning adjustment in font units (negative = move glyphs closer together).
} ttf_kern_pair_t;

//=============================================================================
// Glyph Cache Entry
//=============================================================================

/// @brief A single entry in the glyph cache hash map, storing one rasterised
///        glyph keyed by (size, codepoint).
///
/// @details The cache uses a hash map with separate chaining for collision
///          resolution. Each entry stores a composite 64-bit key formed by
///          packing a 1/64-pixel quantized size into the upper 32 bits and the
///          Unicode codepoint into the lower 32 bits. This separates practical
///          raster sizes while coalescing insignificant floating-point noise.
///          The @c next pointer forms a singly-linked collision chain.
typedef struct vg_cache_entry {
    uint64_t key; ///< Composite key: (size_bits << 32) | codepoint, where size_bits is the IEEE 754
                  ///< representation of the float size.
    struct vg_glyph glyph; ///< The rasterised glyph data (including the alpha-coverage bitmap).
    struct vg_cache_entry *
        next; ///< Next entry in the collision chain (NULL if this is the last entry in the bucket).
    uint64_t access_tick; ///< Monotonic counter value at last cache hit; 0 = never accessed (LRU).
} vg_cache_entry_t;

//=============================================================================
// Glyph Cache
//=============================================================================

/// @brief Initial number of hash buckets allocated when a new glyph cache is
///        created.
///
/// @details The cache starts with this many buckets and doubles in size as
///          the load factor increases, up to VG_CACHE_MAX_SIZE.
#define VG_CACHE_INITIAL_SIZE 256

/// @brief Maximum number of hash buckets the glyph cache will grow to.
///
/// @details Once the bucket count reaches this limit, no further resizing
///          occurs. Additional entries are still inserted, but the average
///          chain length may increase.
#define VG_CACHE_MAX_SIZE 4096

/// @brief Maximum total memory (in bytes) that the glyph cache may consume
///        for glyph bitmap data before triggering eviction.
///
/// @details When bitmap memory usage exceeds this threshold (32 MB), the
///          cache evicts least-recently-used entries in bounded batches. This
///          prevents unbounded growth while retaining frequently used glyphs.
#define VG_CACHE_MAX_MEMORY (32 * 1024 * 1024) // 32MB

/// @brief Hash-map-based glyph cache for storing rasterised glyph bitmaps.
///
/// @details The cache is a power-of-two-sized hash map using separate chaining
///          (linked lists per bucket). It tracks both the number of stored
///          entries and the total memory consumed by glyph bitmaps to enforce
///          the VG_CACHE_MAX_MEMORY limit. When the limit is exceeded, the
///          oldest quarter of entries is evicted and pressure checks repeat as
///          needed. The cache is owned by its parent vg_font and is destroyed
///          when the font is destroyed.
typedef struct vg_glyph_cache {
    vg_cache_entry_t **buckets; ///< Array of bucket head pointers (each bucket is a singly-linked
                                ///< list of cache entries).
    size_t bucket_count;        ///< Current number of hash buckets (always a power of two).
    size_t entry_count;         ///< Total number of cached glyph entries across all buckets.
    size_t memory_used;         ///< Total bytes of bitmap memory currently held by cached glyphs.
    uint64_t access_tick;       ///< Per-cache monotonic access counter for LRU ordering.
} vg_glyph_cache_t;

//=============================================================================
// Internal Font Structure
//=============================================================================

/// @brief Complete internal representation of a loaded TrueType font.
///
/// @details This is the concrete definition of the opaque @c vg_font_t type
///          declared in vg_font.h. It holds the raw TTF file data, parsed
///          copies of the most-used tables, byte offsets to tables that are
///          read on demand, the decoded CMAP character-to-glyph mapping arrays
///          (both format 4 for BMP and format 12 for full Unicode), the sorted
///          kerning pair array, the glyph cache, and the font's human-readable
///          family and style names.
///
///          All heap-allocated members (data, cmap arrays, kern_pairs, cache)
///          are freed by vg_font_destroy.
struct vg_font {
    uint64_t magic;            ///< Live-font sentinel used by runtime handle validation.
    struct vg_font *live_prev; ///< Private: previous entry in the live-font registry.
    struct vg_font *live_next; ///< Private: next entry in the live-font registry.
    float logical_size;        ///< Optional caller-facing size in logical points; zero if unset.

    //-- Raw TTF data ----------------------------------------------------------

    uint8_t *data;    ///< Pointer to the raw TTF file data buffer.
    size_t data_size; ///< Size of the raw data buffer in bytes.
    bool owns_data;   ///< If true, the font owns the data buffer and will free it on destruction.

    //-- Parsed tables ---------------------------------------------------------

    ttf_head_t head; ///< Parsed 'head' table: em-square size, bounding box, loca format.
    ttf_hhea_t hhea; ///< Parsed 'hhea' table: ascent, descent, line gap, hmtx entry count.
    ttf_maxp_t maxp; ///< Parsed 'maxp' table: total glyph count.

    //-- Table byte offsets (from start of file) -------------------------------

    uint32_t cmap_offset; ///< Byte offset to the 'cmap' (character mapping) table.
    uint32_t glyf_offset; ///< Byte offset to the 'glyf' (glyph outline data) table.
    uint32_t loca_offset; ///< Byte offset to the 'loca' (glyph location index) table.
    uint32_t hmtx_offset; ///< Byte offset to the 'hmtx' (horizontal metrics) table.
    uint32_t kern_offset; ///< Byte offset to the 'kern' (kerning) table (0 if absent).
    uint32_t gsub_offset; ///< Byte offset to the 'GSUB' table (0 if absent).
    uint32_t sfnt_base;   ///< Offset of the sfnt table directory (non-zero for .ttc).
    uint32_t name_offset; ///< Byte offset to the 'name' (naming) table.

    //-- Table byte lengths ------------------------------------------------------

    uint32_t cmap_len; ///< Length of the 'cmap' table in bytes.
    uint32_t glyf_len; ///< Length of the 'glyf' table in bytes.
    uint32_t loca_len; ///< Length of the 'loca' table in bytes.
    uint32_t hmtx_len; ///< Length of the 'hmtx' table in bytes.
    uint32_t kern_len; ///< Length of the 'kern' table in bytes.
    uint32_t gsub_len; ///< Length of the 'GSUB' table in bytes.
    uint32_t name_len; ///< Length of the 'name' table in bytes.

    //-- CMAP format 4 data (Basic Multilingual Plane, U+0000..U+FFFF) ---------

    uint16_t cmap4_seg_count; ///< Number of segments in the format 4 CMAP subtable.
    uint16_t
        *cmap4_end_codes; ///< Array of segment end character codes (inclusive), length = seg_count.
    uint16_t *cmap4_start_codes; ///< Array of segment start character codes, length = seg_count.
    int16_t *cmap4_id_deltas; ///< Array of signed deltas added to character codes to produce glyph
                              ///< indices, length = seg_count.
    uint16_t *cmap4_id_range_offsets; ///< Array of offsets into the glyph ID array (0 means use
                                      ///< delta), length = seg_count.
    uint16_t *cmap4_glyph_ids;        ///< Glyph ID array referenced by non-zero id_range_offsets.
    uint32_t cmap4_glyph_ids_count;   ///< Number of entries in the cmap4_glyph_ids array.

    //-- CMAP format 12 data (full Unicode, U+0000..U+10FFFF) ------------------

    uint32_t
        cmap12_num_groups; ///< Number of sequential mapping groups in the format 12 CMAP subtable.
    uint32_t *cmap12_start_codes; ///< Array of group start character codes, length = num_groups.
    uint32_t
        *cmap12_end_codes; ///< Array of group end character codes (inclusive), length = num_groups.
    uint32_t *cmap12_start_glyph_ids; ///< Array of glyph indices for the first character in each
                                      ///< group, length = num_groups.

    //-- Kerning data ----------------------------------------------------------

    ttf_kern_pair_t *kern_pairs; ///< Sorted array of kerning pairs (sorted by left then right glyph
                                 ///< index for binary search).
    uint32_t kern_pair_count;    ///< Number of kerning pairs in the kern_pairs array.

    //-- GSUB ligature shaping (Zanna Studio plan 06) --------------------------

    struct vg_font *fallback; ///< Borrowed per-glyph fallback face (may chain); not owned.
    uint16_t *gsub_feature_lookups; ///< Lookup indices for liga/calt, load-resolved.
    uint16_t gsub_feature_lookup_count; ///< Number of entries in gsub_feature_lookups.

    //-- Glyph cache -----------------------------------------------------------

    vg_glyph_cache_t
        *cache; ///< Hash-map glyph cache storing rasterised bitmaps keyed by (size, codepoint).

    //-- Font names ------------------------------------------------------------

    char family_name[128]; ///< Human-readable font family name (e.g. "Noto Sans", "Fira Code"),
                           ///< null-terminated.
    char style_name[64];   ///< Human-readable style name (e.g. "Regular", "Bold Italic"),
                           ///< null-terminated.
};

//=============================================================================
// Byte Reading Utilities (Big-Endian)
//=============================================================================

/// @brief Read an unsigned 8-bit integer from a byte buffer.
///
/// @param p Pointer to the byte to read.
/// @return The unsigned 8-bit value at the given position.
static inline uint8_t ttf_read_u8(const uint8_t *p) {
    return p[0];
}

/// @brief Read a signed 8-bit integer from a byte buffer.
///
/// @param p Pointer to the byte to read.
/// @return The signed 8-bit value at the given position.
static inline int8_t ttf_read_i8(const uint8_t *p) {
    return (int8_t)p[0];
}

/// @brief Read a big-endian unsigned 16-bit integer from a byte buffer.
///
/// @details TrueType files store all multi-byte integers in big-endian (network)
///          byte order. This function reads two consecutive bytes and assembles
///          them into a native-endian uint16_t with the first byte as the most
///          significant.
///
/// @param p Pointer to the first of two bytes to read.
/// @return The unsigned 16-bit value decoded from big-endian byte order.
static inline uint16_t ttf_read_u16(const uint8_t *p) {
    return ((uint16_t)p[0] << 8) | p[1];
}

/// @brief Read a big-endian signed 16-bit integer from a byte buffer.
///
/// @details Reads two bytes in big-endian order and reinterprets the result as
///          a signed 16-bit integer via a cast from the unsigned representation.
///
/// @param p Pointer to the first of two bytes to read.
/// @return The signed 16-bit value decoded from big-endian byte order.
static inline int16_t ttf_read_i16(const uint8_t *p) {
    return (int16_t)(((uint16_t)p[0] << 8) | p[1]);
}

/// @brief Read a big-endian unsigned 32-bit integer from a byte buffer.
///
/// @details Reads four consecutive bytes in big-endian order and assembles
///          them into a native-endian uint32_t. Used extensively to read table
///          tags, offsets, and lengths from the TTF table directory.
///
/// @param p Pointer to the first of four bytes to read.
/// @return The unsigned 32-bit value decoded from big-endian byte order.
static inline uint32_t ttf_read_u32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/// @brief Read a big-endian signed 32-bit integer from a byte buffer.
///
/// @details Delegates to ttf_read_u32 and reinterprets the result as signed.
///
/// @param p Pointer to the first of four bytes to read.
/// @return The signed 32-bit value decoded from big-endian byte order.
static inline int32_t ttf_read_i32(const uint8_t *p) {
    return (int32_t)ttf_read_u32(p);
}

/// @brief Read a big-endian 2.14 fixed-point number and convert it to float.
///
/// @details The TrueType 2.14 format stores a fixed-point number as a signed
///          16-bit integer where the upper 2 bits represent the integer part
///          and the lower 14 bits represent the fractional part. Division by
///          16384 (2^14) converts to a floating-point value in the range
///          [-2.0, +2.0). This format is used for component glyph transform
///          matrices.
///
/// @param p Pointer to the first of two bytes to read.
/// @return The decoded floating-point value.
static inline float ttf_read_f2dot14(const uint8_t *p) {
    int16_t val = ttf_read_i16(p);
    return (float)val / 16384.0f;
}

/// @brief Read a big-endian 16.16 fixed-point number and convert it to float.
///
/// @details The TrueType 16.16 (Fixed) format stores a fixed-point number as
///          a signed 32-bit integer where the upper 16 bits represent the
///          integer part and the lower 16 bits represent the fractional part.
///          Division by 65536 (2^16) produces the floating-point equivalent.
///          This format is used for the font's version number and various
///          table revision fields.
///
/// @param p Pointer to the first of four bytes to read.
/// @return The decoded floating-point value.
static inline float ttf_read_fixed(const uint8_t *p) {
    int32_t val = ttf_read_i32(p);
    return (float)val / 65536.0f;
}

//=============================================================================
// Tag Macros
//=============================================================================

/// @brief Construct a 4-byte TTF table tag from four ASCII characters.
///
/// @details Packs four single-byte ASCII characters into a single uint32_t in
///          big-endian order, matching the encoding used in the TTF table
///          directory. For example, TTF_TAG('h','e','a','d') produces the same
///          value that ttf_read_u32 would return from the 'head' tag bytes in
///          a font file.
///
/// @param a First (most significant) ASCII character.
/// @param b Second ASCII character.
/// @param c Third ASCII character.
/// @param d Fourth (least significant) ASCII character.
#define TTF_TAG(a, b, c, d)                                                                        \
    (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))

/// @brief Tag for the 'head' table (font header -- em-square, bounding box, loca format).
#define TTF_TAG_HEAD TTF_TAG('h', 'e', 'a', 'd')

/// @brief Tag for the 'hhea' table (horizontal header -- ascent, descent, line gap).
#define TTF_TAG_HHEA TTF_TAG('h', 'h', 'e', 'a')

/// @brief Tag for the 'maxp' table (maximum profile -- glyph count).
#define TTF_TAG_MAXP TTF_TAG('m', 'a', 'x', 'p')

/// @brief Tag for the 'cmap' table (character-to-glyph mapping).
#define TTF_TAG_CMAP TTF_TAG('c', 'm', 'a', 'p')

/// @brief Tag for the 'glyf' table (glyph outline data -- contour points and instructions).
#define TTF_TAG_GLYF TTF_TAG('g', 'l', 'y', 'f')

/// @brief Tag for the 'loca' table (glyph location index -- offsets into 'glyf').
#define TTF_TAG_LOCA TTF_TAG('l', 'o', 'c', 'a')

/// @brief Tag for the 'hmtx' table (horizontal metrics -- advance widths and bearings).
#define TTF_TAG_HMTX TTF_TAG('h', 'm', 't', 'x')

/// @brief Tag constant for the 'GSUB' glyph-substitution table (ligatures).
#define TTF_TAG_GSUB TTF_TAG('G', 'S', 'U', 'B')

/// @brief Tag for the 'kern' table (kerning pairs -- inter-glyph spacing adjustments).
#define TTF_TAG_KERN TTF_TAG('k', 'e', 'r', 'n')

/// @brief Tag for the 'name' table (naming -- font family, style, copyright strings).
#define TTF_TAG_NAME TTF_TAG('n', 'a', 'm', 'e')

//=============================================================================
// Internal Functions -- TTF Parsing
//=============================================================================

/// @brief Walk the TTF table directory and parse all required tables.
///
/// @details Reads the table directory at the beginning of the font data,
///          locates each required table (head, hhea, maxp, cmap, loca, glyf,
///          hmtx), and dispatches to the appropriate per-table parser. Optional
///          tables (kern, name) are parsed if present but their absence is not
///          an error. Returns false if any required table is missing or if
///          parsing any required table fails.
///
/// @param font The font whose raw data buffer should be parsed.
/// @return true if all required tables were successfully located and parsed;
///         false on any parse error or missing required table.
bool ttf_parse_tables(struct vg_font *font);

/// @brief Parse the 'head' (font header) table.
///
/// @details Extracts units_per_em, the global bounding box (xMin, yMin, xMax,
///          yMax), and indexToLocFormat from the raw table data and stores them
///          in font->head.
///
/// @param font The font to populate.
/// @param data Pointer to the raw 'head' table data.
/// @param len  Length of the table data in bytes.
/// @return true if the table was parsed successfully; false on malformed data.
bool ttf_parse_head(struct vg_font *font, const uint8_t *data, uint32_t len);

/// @brief Parse the 'hhea' (horizontal header) table.
///
/// @details Extracts the typographic ascent, descent, line gap, and number of
///          horizontal metric entries from the raw table data and stores them
///          in font->hhea.
///
/// @param font The font to populate.
/// @param data Pointer to the raw 'hhea' table data.
/// @param len  Length of the table data in bytes.
/// @return true if the table was parsed successfully; false on malformed data.
bool ttf_parse_hhea(struct vg_font *font, const uint8_t *data, uint32_t len);

/// @brief Parse the 'maxp' (maximum profile) table.
///
/// @details Extracts the total glyph count from the raw table data and stores
///          it in font->maxp. This count is used to validate glyph indices
///          throughout the rest of the parsing and rendering pipeline.
///
/// @param font The font to populate.
/// @param data Pointer to the raw 'maxp' table data.
/// @param len  Length of the table data in bytes.
/// @return true if the table was parsed successfully; false on malformed data.
bool ttf_parse_maxp(struct vg_font *font, const uint8_t *data, uint32_t len);

/// @brief Parse the 'cmap' (character mapping) table.
///
/// @details Searches the CMAP table for a suitable subtable (preferring
///          format 12 for full Unicode coverage, falling back to format 4 for
///          Basic Multilingual Plane coverage). Allocates and populates the
///          corresponding arrays in the font structure (cmap4_* or cmap12_*).
///
/// @param font The font to populate.
/// @param data Pointer to the raw 'cmap' table data.
/// @param len  Length of the table data in bytes.
/// @return true if a usable CMAP subtable was found and parsed; false otherwise.
bool ttf_parse_cmap(struct vg_font *font, const uint8_t *data, uint32_t len);

/// @brief Parse the 'kern' (kerning) table.
///
/// @details Reads kerning pairs from the table and stores them as a sorted
///          array in font->kern_pairs. The array is sorted by (left, right)
///          glyph index to enable efficient binary-search lookups at render
///          time. Only format 0 (ordered list of kerning pairs) subtables are
///          supported.
///
/// @param font The font to populate.
/// @param data Pointer to the raw 'kern' table data.
/// @param len  Length of the table data in bytes.
/// @return true if the table was parsed successfully (or empty); false on error.
bool ttf_parse_kern(struct vg_font *font, const uint8_t *data, uint32_t len);

/// @brief Parse the 'name' (naming) table.
///
/// @details Extracts the font family name and style name from the naming
///          table and stores them in font->family_name and font->style_name.
///          Prefers platform-specific name records (Windows Unicode BMP, then
///          Macintosh Roman) and falls back to empty strings if no suitable
///          record is found.
///
/// @param font The font to populate.
/// @param data Pointer to the raw 'name' table data.
/// @param len  Length of the table data in bytes.
/// @return true if the table was parsed successfully; false on malformed data.
bool ttf_parse_name(struct vg_font *font, const uint8_t *data, uint32_t len);

//=============================================================================
// Internal Functions -- Glyph Lookup
//=============================================================================

/// @brief Map a Unicode codepoint to a glyph index using the font's CMAP data.
///
/// @details Searches the CMAP format 12 mapping first (if available) for full
///          Unicode coverage, then falls back to format 4 for BMP-only
///          lookups. Returns 0 (the .notdef glyph) if the codepoint is not
///          covered by any mapping.
///
/// @param font      The font whose CMAP data to search.
/// @param codepoint The Unicode codepoint to look up.
/// @return The glyph index corresponding to the codepoint, or 0 if not found.
uint16_t ttf_get_glyph_index(struct vg_font *font, uint32_t codepoint);

//=============================================================================
// Internal Functions -- Glyph Outline
//=============================================================================

/// @brief Extract the outline (contour data) for a glyph from the 'glyf' table.
///
/// @details Reads the glyph's entry from the 'loca' table to find its offset
///          within 'glyf', then decodes the contour endpoints, point
///          coordinates, and on-/off-curve flags. For composite glyphs the
///          component transforms are applied and the results merged. The
///          caller receives heap-allocated arrays of x/y coordinates, flags,
///          and contour-end indices that must be freed after use.
///
/// @param font             The font containing the glyph data.
/// @param glyph_id         The glyph index to extract (as returned by ttf_get_glyph_index).
/// @param[out] out_points_x     Receives a heap-allocated array of x-coordinates for all outline
/// points.
/// @param[out] out_points_y     Receives a heap-allocated array of y-coordinates for all outline
/// points.
/// @param[out] out_flags        Receives a heap-allocated array of point flags (bit 0 = on-curve).
/// @param[out] out_contour_ends Receives a heap-allocated array of indices marking the last point
/// in each contour.
/// @param[out] out_num_points   Receives the total number of outline points.
/// @param[out] out_num_contours Receives the total number of contours.
/// @return true if the outline was successfully extracted; false on error or
///         if the glyph has no outline (e.g. space character).
bool ttf_get_glyph_outline(struct vg_font *font,
                           uint16_t glyph_id,
                           float **out_points_x,
                           float **out_points_y,
                           uint8_t **out_flags,
                           int **out_contour_ends,
                           int *out_num_points,
                           int *out_num_contours);

//=============================================================================
// Internal Functions -- Horizontal Metrics
//=============================================================================

/// @brief Retrieve the horizontal metrics (advance width and left-side bearing)
///        for a glyph.
///
/// @details Looks up the glyph's entry in the 'hmtx' table. Glyphs beyond the
///          last full entry in the table (i.e. glyph_id >= hhea.num_h_metrics)
///          share the advance width of the last full entry but have their own
///          left-side bearing stored in a trailing array. Values are returned
///          in font design units.
///
/// @param font               The font to query.
/// @param glyph_id           The glyph index to look up.
/// @param[out] advance_width      Receives the advance width in font units.
/// @param[out] left_side_bearing  Receives the left-side bearing in font units.
void ttf_get_h_metrics(struct vg_font *font,
                       uint16_t glyph_id,
                       int *advance_width,
                       int *left_side_bearing);

//=============================================================================
// Internal Functions -- Cache Operations
//=============================================================================

/// @brief Create a new, empty glyph cache.
///
/// @details Allocates the cache structure and an initial bucket array of
///          VG_CACHE_INITIAL_SIZE entries, all set to NULL. The cache begins
///          with zero entries and zero memory usage.
///
/// @return A newly allocated glyph cache, or NULL if allocation fails.
vg_glyph_cache_t *vg_cache_create(void);

/// @brief Destroy a glyph cache and free all associated memory.
///
/// @details Iterates over every bucket, frees each cache entry's glyph bitmap
///          and the entry node itself, then frees the bucket array and the
///          cache structure. Safe to call with NULL.
///
/// @param cache The cache to destroy (may be NULL).
void vg_cache_destroy(vg_glyph_cache_t *cache);

/// @brief Look up a cached glyph by font size and Unicode codepoint.
///
/// @details Computes the composite key from the size and codepoint, hashes it
///          to a bucket index, and walks the collision chain looking for a
///          matching entry. Returns a pointer to the cached glyph data if
///          found, or NULL if the glyph is not in the cache.
///
/// @param cache     The glyph cache to search.
/// @param size      The font size in pixels (used as part of the cache key).
/// @param codepoint The Unicode codepoint to look up.
/// @return Pointer to the cached vg_glyph, or NULL if not found.
const struct vg_glyph *vg_cache_get(vg_glyph_cache_t *cache, float size, uint32_t codepoint);

/// @brief Insert a rasterised glyph into the cache.
///
/// @details Creates a new cache entry with a copy of the provided glyph data,
///          inserts it at the head of the appropriate bucket's collision chain,
///          and updates the entry count and memory usage tracking. If the total
///          memory usage exceeds VG_CACHE_MAX_MEMORY after insertion, the cache
///          may be cleared to reclaim memory. The cache may also resize its
///          bucket array if the load factor becomes too high.
///
/// @param cache     The glyph cache to insert into.
/// @param size      The font size in pixels (used as part of the cache key).
/// @param codepoint The Unicode codepoint being cached.
/// @param glyph     Pointer to the glyph data to copy into the cache.
void vg_cache_put(vg_glyph_cache_t *cache,
                  float size,
                  uint32_t codepoint,
                  const struct vg_glyph *glyph);

/// @brief Remove all entries from the glyph cache, freeing all bitmap memory.
///
/// @details Walks every bucket, frees each entry's bitmap and the entry node,
///          and resets all bucket pointers to NULL. The bucket array itself is
///          retained (not reallocated). After this call, entry_count and
///          memory_used are both zero.
///
/// @param cache The cache to clear.
void vg_cache_clear(vg_glyph_cache_t *cache);

//=============================================================================
// Internal Functions -- Rasterization
//=============================================================================

/// @brief Rasterise a glyph outline into an alpha-coverage bitmap at a given
///        pixel size.
///
/// @details Retrieves the glyph outline via ttf_get_glyph_outline, scales the
///          control points from font units to the target pixel size, converts
///          the quadratic Bezier curves into a series of scanline coverage
///          values, and produces an 8-bit alpha bitmap. The horizontal metrics
///          (advance width, left-side bearing) are also scaled and stored in
///          the returned glyph structure.
///
///          The returned vg_glyph and its bitmap are heap-allocated. Ownership
///          is transferred to the caller (typically the glyph cache, which
///          takes responsibility for freeing the bitmap on eviction).
///
/// @param font     The font containing the glyph outlines and metrics.
/// @param glyph_id The glyph index to rasterise (as returned by ttf_get_glyph_index).
/// @param size     The target font size in pixels.
/// @return A heap-allocated vg_glyph with a filled bitmap, or NULL if the
///         glyph has no outline or rasterization fails.
struct vg_glyph *vg_rasterize_glyph(struct vg_font *font, uint16_t glyph_id, float size);

/// @brief Resolve the GSUB liga/calt lookup indices for a freshly parsed font.
/// @details No-op when the font has no GSUB table or no matching features.
/// @param font Parsed font whose owned lookup-index list should be initialized.
void vg_gsub_init(struct vg_font *font);
