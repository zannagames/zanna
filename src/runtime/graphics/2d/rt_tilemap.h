//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/graphics/2d/rt_tilemap.h
// Purpose: Public C ABI for layered logical tile grids, cloned tilesets,
// imported map projections, native/scaled Canvas rendering and hit testing,
// collision, persistence, auto-tiling, properties, and tile animation.
//
// Key invariants:
//   - Tile IDs are stored as int64 values without setter range validation.
//     Rendering treats IDs <= 0 or beyond the selected tileset as empty.
//   - Layer zero is the permanent inline base grid. Added layers own equal-size
//     grids and may override the cloned base tileset.
//   - Logical tile dimensions remain fixed after creation even when imported
//     source-frame dimensions and projection metadata differ.
//   - Drawing honors layer visibility, imported visual order, animation, and
//     conservative viewport culling.
//
// Ownership/Lifetime:
//   - Constructors/loaders return caller-owned runtime reference-counted objects.
//   - Tileset setters clone their Pixels input. The finalizer releases those
//     clones plus owned layer grids and animation arrays.
//
// Links: src/runtime/graphics/2d/rt_tilemap.c (implementation),
//   src/runtime/graphics/2d/rt_tilemap_io.c (persistence/metadata),
//   docs/adr/0144-complete-tiled-map-import.md
//
//===----------------------------------------------------------------------===//
#pragma once

#include <stdint.h>

#include "rt_string.h"

#ifdef __cplusplus
extern "C" {
#endif

/// @file
/// @brief Declares layered Tilemap storage, rendering, collision, I/O, and animation APIs.

/// Runtime class tag used to validate opaque Tilemap handles.
#define RT_TILEMAP_CLASS_ID 0x56504754494c4553LL /* "VPGTILES" */

/// Tile collision type constants.
typedef enum {
    RT_TILE_COLLISION_NONE = 0,       ///< No collision (passable).
    RT_TILE_COLLISION_SOLID = 1,      ///< Fully solid (blocks movement).
    RT_TILE_COLLISION_ONE_WAY_UP = 2, ///< One-way platform (passable from below).
} rt_tilemap_collision_t;

/// Private import-layout orientation values used by SceneDocument adapters.
typedef enum {
    RT_TILEMAP_IMPORT_ORTHOGONAL = 0,
    RT_TILEMAP_IMPORT_ISOMETRIC = 1,
    RT_TILEMAP_IMPORT_STAGGERED = 2,
    RT_TILEMAP_IMPORT_HEXAGONAL = 3,
    RT_TILEMAP_IMPORT_OBLIQUE = 4,
} rt_tilemap_import_orientation_t;

/// Private imported-map render-order values, matching Tiled's four orders.
typedef enum {
    RT_TILEMAP_IMPORT_RIGHT_DOWN = 0,
    RT_TILEMAP_IMPORT_RIGHT_UP = 1,
    RT_TILEMAP_IMPORT_LEFT_DOWN = 2,
    RT_TILEMAP_IMPORT_LEFT_UP = 3,
} rt_tilemap_import_render_order_t;

//=========================================================================
// Tilemap Creation
//=========================================================================

/// @brief Create a new tilemap.
/// @param width Logical width; nonpositive values become 1.
/// @param height Logical height; nonpositive values become 1.
/// @param tile_width Logical cell width; nonpositive values become 16.
/// @param tile_height Logical cell height; nonpositive values become 16.
/// @return A caller-owned Tilemap, or `NULL` after overflow/allocation failure.
void *rt_tilemap_new(int64_t width, int64_t height, int64_t tile_width, int64_t tile_height);

//=========================================================================
// Tilemap Properties
//=========================================================================

/// @brief Get tilemap width in tiles.
/// @param tilemap Candidate Tilemap.
/// @return Logical grid width, or `0` for invalid input.
int64_t rt_tilemap_get_width(void *tilemap);

/// @brief Get tilemap height in tiles.
/// @param tilemap Candidate Tilemap.
/// @return Logical grid height, or `0` for invalid input.
int64_t rt_tilemap_get_height(void *tilemap);

/// @brief Get tile width in pixels.
/// @param tilemap Candidate Tilemap.
/// @return Logical tile width, or `0` for invalid input.
int64_t rt_tilemap_get_tile_width(void *tilemap);

/// @brief Get tile height in pixels.
/// @param tilemap Candidate Tilemap.
/// @return Logical tile height, or `0` for invalid input.
int64_t rt_tilemap_get_tile_height(void *tilemap);

//=========================================================================
// Tileset Management
//=========================================================================

/// @brief Set the tileset image (sprite sheet).
/// @details Clones @p pixels and derives columns/rows from imported source-frame
///          dimensions. The previous clone survives if cloning fails.
/// @param tilemap Tilemap object.
/// @param pixels Pixels object containing tile graphics arranged in a grid.
/// @note Tileset cells are numbered left-to-right, top-to-bottom starting at 1;
///       tile index 0 means "empty" (transparent, nothing drawn).
void rt_tilemap_set_tileset(void *tilemap, void *pixels);

/// @brief Configure source-frame and projected-layout state for an imported map.
/// @param tilemap Candidate Tilemap.
/// @param orientation One `RT_TILEMAP_IMPORT_*` orientation.
/// @param origin_tile_x Imported source-grid X represented by logical zero.
/// @param origin_tile_y Imported source-grid Y represented by logical zero.
/// @param source_frame_width Positive rendered frame width.
/// @param source_frame_height Positive rendered frame height.
/// @param draw_offset_x Authored map X offset.
/// @param draw_offset_y Authored map Y offset.
/// @param render_order One imported render-order enum.
/// @param stagger_axis Zero for columns or one for rows.
/// @param stagger_even Nonzero to stagger even coordinates.
/// @param hex_side_length Nonnegative side length along the stagger axis.
/// @param skew_x Finite oblique horizontal skew.
/// @param skew_y Finite oblique vertical skew.
/// @param parallax_origin_x Finite parallax-origin X.
/// @param parallax_origin_y Finite parallax-origin Y.
/// @param projection_height Nonnegative isometric projection height.
/// @return `1` when all metadata is valid and stored, otherwise `0`.
/// @note This is an adapter bridge. Ordinary Tilemap callers retain orthogonal defaults.
int8_t rt_tilemap_configure_import_layout(void *tilemap,
                                          int64_t orientation,
                                          int64_t origin_tile_x,
                                          int64_t origin_tile_y,
                                          int64_t source_frame_width,
                                          int64_t source_frame_height,
                                          int64_t draw_offset_x,
                                          int64_t draw_offset_y,
                                          int64_t render_order,
                                          int64_t stagger_axis,
                                          int8_t stagger_even,
                                          int64_t hex_side_length,
                                          double skew_x,
                                          double skew_y,
                                          double parallax_origin_x,
                                          double parallax_origin_y,
                                          int64_t projection_height);

/// @brief Configure effective imported placement/parallax for one Tilemap layer.
/// @param tilemap Candidate Tilemap.
/// @param layer Valid layer index.
/// @param offset_x Finite authored X offset.
/// @param offset_y Finite authored Y offset.
/// @param parallax_x Finite horizontal parallax factor.
/// @param parallax_y Finite vertical parallax factor.
/// @return `1` when stored, otherwise `0`.
int8_t rt_tilemap_configure_import_layer(void *tilemap,
                                         int64_t layer,
                                         double offset_x,
                                         double offset_y,
                                         double parallax_x,
                                         double parallax_y);

/// @brief Restrict a composed imported atlas to its non-padding tile count.
/// @param tilemap Candidate Tilemap with a configured base tileset.
/// @param tile_count Positive count within the derived tileset grid.
/// @return `1` when stored, otherwise `0`.
int8_t rt_tilemap_set_import_tile_count(void *tilemap, int64_t tile_count);

/// @brief Get number of tiles in the tileset.
/// @param tilemap Candidate Tilemap.
/// @return Valid base tileset entry count, or `0` for invalid/unconfigured input.
int64_t rt_tilemap_get_tile_count(void *tilemap);

//=========================================================================
// Tile Access
//=========================================================================

/// @brief Set a tile at the given position.
/// @param tilemap Tilemap object.
/// @param x Tile X coordinate.
/// @param y Tile Y coordinate.
/// @param tile_index Index of the tile in the tileset (0 = empty/transparent).
void rt_tilemap_set_tile(void *tilemap, int64_t x, int64_t y, int64_t tile_index);

/// @brief Get the tile index at the given position.
/// @param tilemap Tilemap object.
/// @param x Tile X coordinate.
/// @param y Tile Y coordinate.
/// @return Tile index (0 = empty).
int64_t rt_tilemap_get_tile(void *tilemap, int64_t x, int64_t y);

/// @brief Fill the entire tilemap with a single tile.
/// @param tilemap Tilemap object.
/// @param tile_index Tile index to fill with.
void rt_tilemap_fill(void *tilemap, int64_t tile_index);

/// @brief Clear the tilemap (set all tiles to 0).
/// @param tilemap Candidate Tilemap.
void rt_tilemap_clear(void *tilemap);

/// @brief Fill a rectangular region with a tile.
/// @param tilemap Tilemap object.
/// @param x Start X coordinate.
/// @param y Start Y coordinate.
/// @param w Width in tiles.
/// @param h Height in tiles.
/// @param tile_index Tile index to fill with.
void rt_tilemap_fill_rect(
    void *tilemap, int64_t x, int64_t y, int64_t w, int64_t h, int64_t tile_index);

//=========================================================================
// Rendering
//=========================================================================

/// @brief Draw the tilemap to a canvas.
/// @param tilemap Tilemap object.
/// @param canvas Canvas to draw on.
/// @param offset_x X offset for scrolling.
/// @param offset_y Y offset for scrolling.
/// @note Draws every visible layer in layer order.
void rt_tilemap_draw(void *tilemap, void *canvas, int64_t offset_x, int64_t offset_y);

/// @brief Draw a portion of the tilemap (for culling).
/// @param tilemap Tilemap object.
/// @param canvas Canvas to draw on.
/// @param offset_x X offset for scrolling.
/// @param offset_y Y offset for scrolling.
/// @param view_x View start X in tiles.
/// @param view_y View start Y in tiles.
/// @param view_w View width in tiles.
/// @param view_h View height in tiles.
/// @note Draws the tile-coordinate sub-region across every visible layer.
void rt_tilemap_draw_region(void *tilemap,
                            void *canvas,
                            int64_t offset_x,
                            int64_t offset_y,
                            int64_t view_x,
                            int64_t view_y,
                            int64_t view_w,
                            int64_t view_h);

/// @brief Draw the tilemap with nearest-neighbor scaled tile cells.
/// @param tilemap Candidate Tilemap.
/// @param canvas Destination Canvas.
/// @param offset_x Unscaled destination-pixel camera X.
/// @param offset_y Unscaled destination-pixel camera Y.
/// @param scale_percent Scale percentage; 100 draws at native tile size.
void rt_tilemap_draw_scaled(
    void *tilemap, void *canvas, int64_t offset_x, int64_t offset_y, int64_t scale_percent);

/// @brief Count non-empty, drawable tiles in a tile-coordinate sub-region.
/// @param tilemap Candidate Tilemap.
/// @param view_x Requested first logical column.
/// @param view_y Requested first logical row.
/// @param view_w Requested logical width.
/// @param view_h Requested logical height.
/// @return Saturating count across visible layers after clipping and animation resolution.
int64_t rt_tilemap_count_drawn_region(
    void *tilemap, int64_t view_x, int64_t view_y, int64_t view_w, int64_t view_h);

/// @brief Count non-empty, drawable tiles visible in the canvas viewport.
/// @param tilemap Candidate Tilemap.
/// @param canvas Canvas supplying viewport dimensions.
/// @param offset_x Camera/destination X offset.
/// @param offset_y Camera/destination Y offset.
/// @return Saturating visible drawable-cell count.
int64_t rt_tilemap_count_drawn_visible(void *tilemap,
                                       void *canvas,
                                       int64_t offset_x,
                                       int64_t offset_y);

/// @brief Count drawable tiles visible in a scaled canvas viewport.
/// @param tilemap Candidate Tilemap.
/// @param canvas Canvas supplying viewport dimensions.
/// @param offset_x Unscaled destination-pixel camera X.
/// @param offset_y Unscaled destination-pixel camera Y.
/// @param scale_percent Positive zoom percentage.
/// @return Saturating visible drawable-cell count, or `0` for invalid input.
int64_t rt_tilemap_count_drawn_visible_scaled(
    void *tilemap, void *canvas, int64_t offset_x, int64_t offset_y, int64_t scale_percent);

/// @brief Convert scaled screen coordinates to tile coordinates and return a result map.
/// @details The new Map contains `tileX`, `tileY`, `tile`, `scalePercent`, and
///          `inBounds`, using base-layer imported projection/offset metadata.
/// @param tilemap Candidate Tilemap.
/// @param screen_x Destination-space X.
/// @param screen_y Destination-space Y.
/// @param offset_x Unscaled camera X.
/// @param offset_y Unscaled camera Y.
/// @param scale_percent Requested zoom percentage.
/// @return A caller-owned result Map; invalid inputs produce an out-of-bounds result.
void *rt_tilemap_hit_test_scaled(void *tilemap,
                                 int64_t screen_x,
                                 int64_t screen_y,
                                 int64_t offset_x,
                                 int64_t offset_y,
                                 int64_t scale_percent);

//=========================================================================
// Utility
//=========================================================================

/// @brief Convert pixel coordinates to tile coordinates.
/// @param tilemap Tilemap object.
/// @param pixel_x Pixel X coordinate.
/// @param pixel_y Pixel Y coordinate.
/// @param tile_x Output: Tile X coordinate.
/// @param tile_y Output: Tile Y coordinate.
void rt_tilemap_pixel_to_tile(
    void *tilemap, int64_t pixel_x, int64_t pixel_y, int64_t *tile_x, int64_t *tile_y);

/// @brief Get tile X from pixel X.
/// @param tilemap Candidate Tilemap.
/// @param pixel_x Projected source-space X, evaluated with Y fixed at zero.
/// @return Logical tile X, or `0` for invalid input.
int64_t rt_tilemap_to_tile_x(void *tilemap, int64_t pixel_x);

/// @brief Get tile Y from pixel Y.
/// @param tilemap Candidate Tilemap.
/// @param pixel_y Projected source-space Y, evaluated with X fixed at zero.
/// @return Logical tile Y, or `0` for invalid input.
int64_t rt_tilemap_to_tile_y(void *tilemap, int64_t pixel_y);

/// @brief Convert tile coordinates to pixel coordinates.
/// @param tilemap Tilemap object.
/// @param tile_x Tile X coordinate.
/// @param tile_y Tile Y coordinate.
/// @param pixel_x Output: Pixel X coordinate.
/// @param pixel_y Output: Pixel Y coordinate.
void rt_tilemap_tile_to_pixel(
    void *tilemap, int64_t tile_x, int64_t tile_y, int64_t *pixel_x, int64_t *pixel_y);

/// @brief Get pixel X from tile X.
/// @param tilemap Candidate Tilemap.
/// @param tile_x Logical tile X, projected with tile Y fixed at zero.
/// @return Rounded projected X, or `0` for invalid input.
int64_t rt_tilemap_to_pixel_x(void *tilemap, int64_t tile_x);

/// @brief Get pixel Y from tile Y.
/// @param tilemap Candidate Tilemap.
/// @param tile_y Logical tile Y, projected with tile X fixed at zero.
/// @return Rounded projected Y, or `0` for invalid input.
int64_t rt_tilemap_to_pixel_y(void *tilemap, int64_t tile_y);

//=========================================================================
// Tile Collision
//=========================================================================

/// @brief Set collision type for a tile ID.
/// @param tilemap Tilemap object.
/// @param tile_id Tile index (1-4095; 0 = empty and is rejected by collision setters).
/// @param coll_type Collision type (RT_TILE_COLLISION_NONE, _SOLID, or _ONE_WAY_UP).
void rt_tilemap_set_collision(void *tilemap, int64_t tile_id, int64_t coll_type);

/// @brief Get collision type for a tile ID.
/// @param tilemap Candidate Tilemap.
/// @param tile_id Raw tile identifier.
/// @return Stored collision enum, or `RT_TILE_COLLISION_NONE` when invalid/unset.
int64_t rt_tilemap_get_collision(void *tilemap, int64_t tile_id);

/// @brief Check if a pixel position is on a solid tile.
/// @param tilemap Candidate Tilemap.
/// @param pixel_x Logical orthogonal world-pixel X.
/// @param pixel_y Logical orthogonal world-pixel Y.
/// @return `1` only when the designated collision-layer cell is tagged solid.
int8_t rt_tilemap_is_solid_at(void *tilemap, int64_t pixel_x, int64_t pixel_y);

/// @brief Resolve an AABB or circle Physics2D.Body against solid/one-way tiles.
/// @details Uses previous-to-current swept motion to prevent crossing a tile
///          between samples, then resolves any remaining penetration.
/// @param tilemap Tilemap object.
/// @param body Physics2D.Body object.
/// @return 1 if any collision occurred, 0 otherwise.
int8_t rt_tilemap_collide_body(void *tilemap, void *body);

//=========================================================================
// Layer Management
//=========================================================================

/// @brief Add a named layer to the tilemap.
/// @param tilemap Tilemap object.
/// @param name Optional layer name of at most 31 bytes; longer names are rejected.
/// @return Layer ID (1–15) on success, or `-1` for invalid input, capacity,
///         name-length, dimension, or allocation failure.
int64_t rt_tilemap_add_layer(void *tilemap, rt_string name);

/// @brief Get the number of layers.
/// @param tilemap Candidate Tilemap.
/// @return Layer count, or `0` for invalid input.
int64_t rt_tilemap_get_layer_count(void *tilemap);

/// @brief Find a layer by name.
/// @param tilemap Candidate Tilemap.
/// @param name Borrowed case-sensitive runtime name.
/// @return Layer ID, or -1 if not found.
int64_t rt_tilemap_get_layer_by_name(void *tilemap, rt_string name);

/// @brief Remove a layer. Layer 0 (base) cannot be removed.
/// @param tilemap Candidate Tilemap.
/// @param layer Nonzero valid layer index.
void rt_tilemap_remove_layer(void *tilemap, int64_t layer);

/// @brief Set layer visibility.
/// @param tilemap Candidate Tilemap.
/// @param layer Valid layer index.
/// @param visible Zero to hide; nonzero to show.
void rt_tilemap_set_layer_visible(void *tilemap, int64_t layer, int8_t visible);

/// @brief Get layer visibility.
/// @param tilemap Candidate Tilemap.
/// @param layer Layer index to query.
/// @return Normalized visibility, or `0` for invalid input.
int8_t rt_tilemap_get_layer_visible(void *tilemap, int64_t layer);

//=========================================================================
// Per-Layer Tile Access
//=========================================================================

/// @brief Set a tile on a specific layer.
/// @param tilemap Candidate Tilemap.
/// @param layer Layer index to modify.
/// @param x Logical column.
/// @param y Logical row.
/// @param tile Tile identifier to store.
void rt_tilemap_set_tile_layer(void *tilemap, int64_t layer, int64_t x, int64_t y, int64_t tile);

/// @brief Get a tile from a specific layer.
/// @param tilemap Candidate Tilemap.
/// @param layer Layer index to inspect.
/// @param x Logical column.
/// @param y Logical row.
/// @return Stored identifier, or `0` for invalid/out-of-bounds access.
int64_t rt_tilemap_get_tile_layer(void *tilemap, int64_t layer, int64_t x, int64_t y);

/// @brief Fill an entire layer with a single tile.
/// @param tilemap Candidate Tilemap.
/// @param layer Layer index to fill.
/// @param tile Tile identifier to store.
void rt_tilemap_fill_layer(void *tilemap, int64_t layer, int64_t tile);

/// @brief Clear a layer (set all tiles to 0).
/// @param tilemap Candidate Tilemap.
/// @param layer Layer index to clear.
void rt_tilemap_clear_layer(void *tilemap, int64_t layer);

//=========================================================================
// Per-Layer Tileset
//=========================================================================

/// @brief Set a per-layer tileset. NULL = use base layer tileset.
/// @details Nonnull Pixels are cloned before replacing the previous override.
/// @param tilemap Candidate Tilemap.
/// @param layer Valid layer index.
/// @param pixels Pixels to clone, or `NULL` to restore base fallback.
void rt_tilemap_set_layer_tileset(void *tilemap, int64_t layer, void *pixels);

//=========================================================================
// Per-Layer Rendering
//=========================================================================

/// @brief Draw a single layer to a canvas.
/// @param tilemap Candidate Tilemap.
/// @param canvas Destination Canvas.
/// @param layer Layer index to render.
/// @param cam_x Camera/destination X offset.
/// @param cam_y Camera/destination Y offset.
void rt_tilemap_draw_layer(
    void *tilemap, void *canvas, int64_t layer, int64_t cam_x, int64_t cam_y);

//=========================================================================
// Collision Layer
//=========================================================================

/// @brief Set which layer is used for collision queries.
/// @param tilemap Candidate Tilemap.
/// @param layer Valid layer index.
void rt_tilemap_set_collision_layer(void *tilemap, int64_t layer);

/// @brief Get the current collision layer index.
/// @param tilemap Candidate Tilemap.
/// @return Designated layer index, or `0` for invalid input.
int64_t rt_tilemap_get_collision_layer(void *tilemap);

//=========================================================================
// File I/O
//=========================================================================

/// @brief Save tilemap to JSON file.
/// @details Serializes layout, cloned tilesets, layers, collision, properties,
///          active auto-tile rules, and animation state. Writes a temporary file
///          and replaces the destination only after a complete successful write.
/// @param tm Candidate Tilemap.
/// @param path Borrowed runtime string containing a UTF-8 destination path.
/// @return `1` on successful replacement, otherwise `0`.
int8_t rt_tilemap_save_to_file(void *tm, rt_string path);

/// @brief Load tilemap from JSON file.
/// @details Requires supported versioned, bounded, exact-integer JSON and
///          reconstructs owned tilesets, layers, metadata, and animation state.
/// @param path Borrowed runtime string containing a UTF-8 source path.
/// @return A caller-owned Tilemap, or `NULL` for I/O, size, parse, schema,
///         validation, or allocation failure.
void *rt_tilemap_load_from_file(rt_string path);

/// @brief Load tilemap from CSV file (Tiled export format).
/// @details Requires a nonempty rectangular comma-separated grid. Empty lines
///          are skipped, whitespace around fields is allowed, malformed fields
///          fail, and overlong lines are rejected.
/// @param path Borrowed runtime string containing a UTF-8 CSV path.
/// @param tile_w Logical tile width passed to the constructor.
/// @param tile_h Logical tile height passed to the constructor.
/// @return A caller-owned one-layer Tilemap, or `NULL` on invalid/I/O/parse/
///         allocation failure.
void *rt_tilemap_load_csv(rt_string path, int64_t tile_w, int64_t tile_h);

//=========================================================================
// Auto-Tiling
//=========================================================================

/// @brief Set auto-tile rule variants 0-7 (lower half of 4-bit bitmask).
/// @param tm Candidate Tilemap.
/// @param base_tile Raw tile identifier that keys the rule.
/// @param v0 Variant for mask 0.
/// @param v1 Variant for mask 1.
/// @param v2 Variant for mask 2.
/// @param v3 Variant for mask 3.
/// @param v4 Variant for mask 4.
/// @param v5 Variant for mask 5.
/// @param v6 Variant for mask 6.
/// @param v7 Variant for mask 7.
void rt_tilemap_set_autotile_lo(void *tm,
                                int64_t base_tile,
                                int64_t v0,
                                int64_t v1,
                                int64_t v2,
                                int64_t v3,
                                int64_t v4,
                                int64_t v5,
                                int64_t v6,
                                int64_t v7);

/// @brief Set auto-tile rule variants 8-15 (upper half of 4-bit bitmask).
/// @param tm Candidate Tilemap.
/// @param base_tile Raw tile identifier that keys the rule.
/// @param v8 Variant for mask 8.
/// @param v9 Variant for mask 9.
/// @param v10 Variant for mask 10.
/// @param v11 Variant for mask 11.
/// @param v12 Variant for mask 12.
/// @param v13 Variant for mask 13.
/// @param v14 Variant for mask 14.
/// @param v15 Variant for mask 15.
void rt_tilemap_set_autotile_hi(void *tm,
                                int64_t base_tile,
                                int64_t v8,
                                int64_t v9,
                                int64_t v10,
                                int64_t v11,
                                int64_t v12,
                                int64_t v13,
                                int64_t v14,
                                int64_t v15);

/// @brief Clear auto-tile rule for a base tile.
/// @details Deactivates the stored rule without reclaiming its fixed table slot.
/// @param tm Candidate Tilemap.
/// @param base_tile Rule key to deactivate.
void rt_tilemap_clear_autotile(void *tm, int64_t base_tile);

/// @brief Apply auto-tiling to entire tilemap.
/// @param tm Candidate Tilemap whose base layer should be rewritten.
void rt_tilemap_apply_autotile(void *tm);

/// @brief Apply auto-tiling to a region.
/// @details Clips to the base grid and resolves into temporary storage before
///          writing, so earlier substitutions do not affect later neighbor masks.
/// @param tm Candidate Tilemap.
/// @param x Requested first logical column.
/// @param y Requested first logical row.
/// @param w Requested width.
/// @param h Requested height.
void rt_tilemap_apply_autotile_region(void *tm, int64_t x, int64_t y, int64_t w, int64_t h);

//=========================================================================
// Tile Properties
//=========================================================================

/// @brief Set a property on a tile ID.
/// @details Properties are per tile type, not per map cell. Existing
///          case-sensitive keys are replaced; invalid/overlong keys and full
///          fixed property tables are silent no-ops.
/// @param tm Candidate Tilemap.
/// @param tile_index Tile-property table index.
/// @param key Borrowed runtime property key.
/// @param value Signed integer value.
void rt_tilemap_set_tile_property(void *tm, int64_t tile_index, rt_string key, int64_t value);

/// @brief Get a property from a tile ID (returns default_val if not found).
/// @param tm Candidate Tilemap.
/// @param tile_index Tile-property table index.
/// @param key Borrowed case-sensitive runtime key.
/// @param default_val Value returned for invalid input or an absent key.
/// @return Stored value when present, otherwise @p default_val.
int64_t rt_tilemap_get_tile_property(void *tm,
                                     int64_t tile_index,
                                     rt_string key,
                                     int64_t default_val);

/// @brief Check if a tile ID has a property.
/// @param tm Candidate Tilemap.
/// @param tile_index Tile-property table index.
/// @param key Borrowed case-sensitive runtime key.
/// @return `1` when present, otherwise `0`.
int8_t rt_tilemap_has_tile_property(void *tm, int64_t tile_index, rt_string key);

//=========================================================================
// Tile Animation
//=========================================================================

/// @brief Register an animated tile. Frames default to base_id, base_id+1, ...
/// @param tm Candidate Tilemap.
/// @param base_tile_id Positive raw tile identifier used as the animation key.
/// @param frame_count Positive count within the uniform-animation limit.
/// @param ms_per_frame Positive uniform duration in milliseconds.
void rt_tilemap_set_tile_anim(void *tm,
                              int64_t base_tile_id,
                              int64_t frame_count,
                              int64_t ms_per_frame);

/// @brief Override a specific frame's tile ID for a registered animation.
/// @param tm Candidate Tilemap.
/// @param base_tile_id Animation key to find.
/// @param frame_idx Zero-based frame index.
/// @param tile_id Replacement identifier, stored without range validation.
void rt_tilemap_set_tile_anim_frame(void *tm,
                                    int64_t base_tile_id,
                                    int64_t frame_idx,
                                    int64_t tile_id);

/// @brief Configure an imported animation with authored per-frame durations.
/// @param tm Candidate Tilemap.
/// @param base_tile_id Positive raw tile identifier used as the animation key.
/// @param frame_count Positive number of entries within the imported limit.
/// @param frame_tiles Array of positive tile identifiers; copied on success.
/// @param frame_durations Array of positive milliseconds; copied on success.
/// @return `1` when transactionally registered, otherwise `0`.
/// @note Frame and duration arrays are copied; each duration must be positive.
int8_t rt_tilemap_set_import_tile_anim(void *tm,
                                       int64_t base_tile_id,
                                       int64_t frame_count,
                                       const int64_t *frame_tiles,
                                       const int64_t *frame_durations);

/// @brief Advance all tile animations by dt milliseconds.
/// @param tm Candidate Tilemap.
/// @param dt_ms Elapsed milliseconds; negative values become zero.
void rt_tilemap_update_anims(void *tm, int64_t dt_ms);

/// @brief Resolve a tile ID through animation (returns current frame's tile ID).
/// @param tm Candidate Tilemap.
/// @param tile_id Raw tile identifier to resolve.
/// @return Current frame identifier, or @p tile_id when unregistered/invalid.
int64_t rt_tilemap_resolve_anim_tile(void *tm, int64_t tile_id);

#ifdef __cplusplus
}
#endif
