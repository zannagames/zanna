//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/graphics/2d/rt_tilemap_internal.h
// Purpose: Shared private Tilemap storage for projection, imported artwork,
//   animation, rendering, metadata, and serialization.
//
// Key invariants:
//   - Logical cell dimensions remain independent from imported source frames.
//   - Dynamic animation arrays are owned by the containing Tilemap.
//
// Ownership/Lifetime:
//   - Layer tiles, cloned tilesets, and animation arrays release in the finalizer.
//   - The base tile array remains inline after rt_tilemap_impl.
//
// Links: rt_tilemap.h, rt_tilemap.c, rt_tilemap_io.c,
//   docs/adr/0144-complete-tiled-map-import.md
//
//===----------------------------------------------------------------------===//
#pragma once

#include <stdint.h>

/// @file
/// @brief Defines the shared private storage layout used by Tilemap runtime modules.

/// Number of raw tile identifiers addressable by the collision-type table.
#define MAX_TILE_COLLISION_IDS 4096
/// Maximum number of layers, including the permanent base layer.
#define TM_MAX_LAYERS 16
/// Maximum number of registered tile-animation keys.
#define TM_MAX_TILE_ANIMS 64
/// Maximum frames accepted by the uniform-duration public animation helper.
#define TM_MAX_ANIM_FRAMES 8
/// Maximum frames accepted from imported variable-duration animation data.
#define TM_MAX_IMPORT_ANIM_FRAMES 4096
/// Number of tile identifiers addressable by the fixed property table.
#define MAX_TILE_PROPS 256
/// Maximum custom integer properties stored for one tile identifier.
#define MAX_PROP_KEYS 8
/// Bytes reserved for each NUL-terminated property key.
#define MAX_PROP_KEY_LEN 32
/// Maximum number of allocated auto-tile rule slots.
#define MAX_AUTOTILE_RULES 64

/// @brief One fixed-storage string-to-integer tile-property entry.
typedef struct {
    /// NUL-terminated case-sensitive key.
    char key[MAX_PROP_KEY_LEN];
    /// Associated signed integer value.
    int64_t value;
} tile_prop_entry;

/// @brief Bounded property set for one raw tile identifier.
typedef struct {
    /// Dense entries below @c count.
    tile_prop_entry entries[MAX_PROP_KEYS];
    /// Number of initialized entries.
    int32_t count;
} tile_props;

/// @brief Four-neighbor bitmask substitutions for one auto-tile family.
typedef struct {
    /// Canonical tile identifier used as the rule key.
    int64_t base_tile;
    /// Replacement for N/E/S/W connectivity masks 0–15.
    int64_t variants[16];
    /// Nonzero when application and persistence use the rule.
    int8_t active;
} autotile_rule;

/// @brief Storage and imported placement metadata for one logical Tilemap layer.
/// @details Layer zero aliases the inline base tile grid and does not own it.
///          Added layers own separate grids. A nonnull @c tileset is a cloned
///          override; null falls back to `rt_tilemap_impl::tileset`.
typedef struct {
    /// Dense row-major logical tile identifiers.
    int64_t *tiles;
    /// Owned Pixels override, or `NULL` for base fallback.
    void *tileset;
    /// Complete source-frame columns in the override.
    int64_t tileset_cols;
    /// Complete source-frame rows in the override.
    int64_t tileset_rows;
    /// Valid identifier count for the override.
    int64_t tile_count;
    /// NUL-terminated layer name of at most 31 bytes.
    char name[32];
    /// Normalized drawing/counting visibility flag.
    int8_t visible;
    /// Nonzero when @c tiles must be freed separately.
    int8_t owns_tiles;
    /// Authored source-space horizontal offset.
    double import_offset_x;
    /// Authored source-space vertical offset.
    double import_offset_y;
    /// Horizontal camera parallax factor.
    double import_parallax_x;
    /// Vertical camera parallax factor.
    double import_parallax_y;
} tm_layer;

/// @brief Owned playback state for one raw base-tile animation.
typedef struct {
    /// Raw map identifier that triggers resolution.
    int64_t base_tile_id;
    /// Owned array of resolved tile identifiers.
    int64_t *frame_tiles;
    /// Owned positive per-frame milliseconds.
    int64_t *frame_durations;
    /// Length of both owned arrays.
    int32_t frame_count;
    /// Uniform-duration metadata, or zero for imported timing.
    int64_t ms_per_frame;
    /// Elapsed milliseconds within @c current_frame.
    int64_t timer;
    /// Active zero-based array index.
    int32_t current_frame;
} tm_tile_anim;

/// @brief Complete private state of a runtime-managed layered Tilemap.
/// @details The base tile array is allocated immediately after this structure;
///          @c tiles points into that trailing storage. Other owned allocations
///          are released by the Tilemap finalizer.
typedef struct rt_tilemap_impl {
    /// Logical grid width in cells.
    int64_t width;
    /// Logical grid height in cells.
    int64_t height;
    /// Logical collision-cell width in pixels.
    int64_t tile_width;
    /// Logical collision-cell height in pixels.
    int64_t tile_height;
    /// Rendered tileset-frame width.
    int64_t source_frame_width;
    /// Rendered tileset-frame height.
    int64_t source_frame_height;
    /// Map-level authored X offset.
    int64_t import_draw_offset_x;
    /// Map-level authored Y offset.
    int64_t import_draw_offset_y;
    /// Imported source X represented by logical zero.
    int64_t import_origin_tile_x;
    /// Imported source Y represented by logical zero.
    int64_t import_origin_tile_y;
    /// Height term used by isometric projection.
    int64_t import_projection_height;
    /// Hex side length along the stagger axis.
    int64_t import_hex_side_length;
    /// `rt_tilemap_import_orientation_t` value.
    int32_t import_orientation;
    /// `rt_tilemap_import_render_order_t` value.
    int32_t import_render_order;
    /// Zero for columns; one for rows.
    int32_t import_stagger_axis;
    /// Nonzero when even coordinates are staggered.
    int8_t import_stagger_even;
    /// Oblique horizontal skew.
    double import_skew_x;
    /// Oblique vertical skew.
    double import_skew_y;
    /// Imported parallax-origin X.
    double import_parallax_origin_x;
    /// Imported parallax-origin Y.
    double import_parallax_origin_y;
    /// Complete source-frame columns in base tileset.
    int64_t tileset_cols;
    /// Complete source-frame rows in base tileset.
    int64_t tileset_rows;
    /// Valid base tileset identifiers.
    int64_t tile_count;
    /// Owned cloned base Pixels, or `NULL`.
    void *tileset;
    /// Pointer to inline base row-major tile grid.
    int64_t *tiles;
    /// Collision enum by raw tile identifier.
    int8_t collision[MAX_TILE_COLLISION_IDS];
    /// Dense layer records below @c layer_count.
    tm_layer layers[TM_MAX_LAYERS];
    /// Active layers, including base layer.
    int32_t layer_count;
    /// Layer index used by collision queries.
    int32_t collision_layer;
    /// Dense registered animations.
    tm_tile_anim tile_anims[TM_MAX_TILE_ANIMS];
    /// Number of animation entries.
    int32_t tile_anim_count;
    /// Fixed per-tile property sets.
    tile_props tile_props[MAX_TILE_PROPS];
    /// Allocated rule slots.
    autotile_rule autotile_rules[MAX_AUTOTILE_RULES];
    /// Slots ever allocated.
    int32_t autotile_count;
} rt_tilemap_impl;
