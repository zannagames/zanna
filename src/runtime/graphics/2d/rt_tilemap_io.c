//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/2d/rt_tilemap_io.c
// Purpose: File I/O (JSON save/load, CSV import), duration-aware animation
// persistence, and auto-tiling for tilemaps.
//
// Key invariants:
//   - JSON format version 1 retains layers, imported layout, collision,
//     properties, and animation durations while accepting legacy omissions.
//   - JSON numeric fields that control identity/shape must convert exactly to
//     int64 and files are bounded to 256 MiB.
//   - Saving is transactional through a temporary file and atomic-style replace.
//   - CSV import requires a rectangular nonempty grid of strict integer fields.
//   - Auto-tiling rewrites the base layer from a two-pass 4-bit N/E/S/W mask.
//   - Tile properties are bounded fixed storage indexed by raw tile ID.
//
// Ownership/Lifetime:
//   - LoadFromFile/LoadCSV return newly allocated tilemaps.
//   - Deserialized Pixels are transferred into Tilemap-owned base/layer slots.
//   - Temporary Maps, Seqs, strings, buffers, and failed partial tilemaps are
//     released on every cleanup path. Property/rule storage is embedded.
//
// Links: src/runtime/graphics/2d/rt_tilemap.h,
//   src/runtime/graphics/2d/rt_tilemap_internal.h,
//   docs/adr/0144-complete-tiled-map-import.md
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Implements Tilemap persistence, CSV import, properties, and auto-tiling.
 *
 * @details This module serializes and validates bounded versioned JSON,
 *          transactionally replaces saved files, imports strict rectangular
 *          CSV grids, persists imported layout and animation metadata, manages
 *          fixed tile-property/rule storage, and applies deterministic
 *          two-pass neighbor-mask auto-tiling.
 */

#include "rt_tilemap.h"
#include "rt_tilemap_internal.h"

#include "rt_box.h"
#include "rt_file_path.h"
#include "rt_file_stdio.h"
#include "rt_graphics.h"
#include "rt_heap.h"
#include "rt_internal.h"
#include "rt_json.h"
#include "rt_map.h"
#include "rt_object.h"
#include "rt_pixels.h"
#include "rt_pixels_internal.h"
#include "rt_seq.h"
#include "rt_string.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/// @brief Maximum accepted serialized Tilemap file size in bytes.
#define TMIO_MAX_FILE_BYTES (INT64_C(256) * 1024 * 1024)
/// @brief Largest exactly representable integer magnitude in an IEEE double.
#define TMIO_JSON_SAFE_INTEGER_LIMIT 9007199254740992.0

/// @brief Route Tilemap seeks through the runtime's 64-bit stdio adapter.
/// @param fp Open stream.
/// @param off Signed 64-bit byte offset.
/// @param whence Seek origin constant.
#define tmio_fseek(fp, off, whence) rt_file_stdio_seek64((fp), (off), (whence))
/// @brief Route Tilemap position queries through the 64-bit stdio adapter.
/// @param fp Open stream.
#define tmio_ftell(fp) rt_file_stdio_tell64((fp))

static int tilemap_io_grid_supported(int64_t width, int64_t height);

/// @brief Read exactly @p len bytes from @p f into @p data.
/// @details Tilemap files are read as complete JSON blobs. This helper avoids
///          assuming that one `fread` call must return the full file payload,
///          while preserving the existing all-or-nothing loader contract.
/// @param f Open binary stream.
/// @param data Destination byte buffer.
/// @param len Number of bytes to read.
/// @return 1 when all bytes were read; otherwise 0.
static int tmio_read_exact(FILE *f, void *data, size_t len) {
    uint8_t *dst = (uint8_t *)data;
    size_t done = 0;
    if (!f || (!dst && len > 0))
        return 0;
    while (done < len) {
        size_t n = fread(dst + done, 1, len - done, f);
        if (n == 0)
            return 0;
        done += n;
    }
    return 1;
}

/// @brief Validate-and-return a Tilemap pointer; NULL for NULL or wrong class.
/// @details Soft check used by every public Tilemap I/O entry point.
/// @param tm Candidate opaque Tilemap handle.
/// @return Validated implementation pointer, or `NULL` for invalid input.
static rt_tilemap_impl *tilemap_io_checked(void *tm) {
    if (!tm || !rt_obj_is_instance(tm, RT_TILEMAP_CLASS_ID, sizeof(rt_tilemap_impl)))
        return NULL;
    rt_tilemap_impl *tilemap = (rt_tilemap_impl *)tm;
    if (tilemap->state_magic != RT_TILEMAP_STATE_MAGIC ||
        !tilemap_io_grid_supported(tilemap->width, tilemap->height) || tilemap->tile_width <= 0 ||
        tilemap->tile_height <= 0 || tilemap->source_frame_width <= 0 ||
        tilemap->source_frame_height <= 0 || tilemap->layer_count < 1 ||
        tilemap->layer_count > TM_MAX_LAYERS || tilemap->collision_layer < 0 ||
        tilemap->collision_layer >= tilemap->layer_count || tilemap->autotile_count < 0 ||
        tilemap->autotile_count > MAX_AUTOTILE_RULES || tilemap->tile_anim_count < 0 ||
        tilemap->tile_anim_count > TM_MAX_TILE_ANIMS ||
        tilemap->import_orientation < RT_TILEMAP_IMPORT_ORTHOGONAL ||
        tilemap->import_orientation > RT_TILEMAP_IMPORT_OBLIQUE ||
        tilemap->import_render_order < RT_TILEMAP_IMPORT_RIGHT_DOWN ||
        tilemap->import_render_order > RT_TILEMAP_IMPORT_LEFT_UP ||
        (tilemap->import_stagger_axis != 0 && tilemap->import_stagger_axis != 1) ||
        (tilemap->import_stagger_even != 0 && tilemap->import_stagger_even != 1) ||
        !isfinite(tilemap->import_skew_x) || !isfinite(tilemap->import_skew_y) ||
        !isfinite(tilemap->import_parallax_origin_x) ||
        !isfinite(tilemap->import_parallax_origin_y))
        return NULL;

    int64_t tile_count = tilemap->width * tilemap->height;
    size_t tiles_size = (size_t)tile_count * sizeof(int64_t);
    if (tiles_size > SIZE_MAX - sizeof(*tilemap) ||
        !rt_obj_is_instance(tm, RT_TILEMAP_CLASS_ID, sizeof(*tilemap) + tiles_size) ||
        tilemap->tiles != (int64_t *)((uint8_t *)tilemap + sizeof(*tilemap)) ||
        tilemap->layers[0].tiles != tilemap->tiles || tilemap->layers[0].owns_tiles != 0 ||
        (tilemap->tileset && !rt_pixels_checked_impl_or_null(tilemap->tileset)))
        return NULL;
    for (int32_t i = 0; i < tilemap->layer_count; ++i) {
        tm_layer *layer = &tilemap->layers[i];
        if (!layer->tiles || (layer->visible != 0 && layer->visible != 1) ||
            (layer->owns_tiles != 0 && layer->owns_tiles != 1) ||
            (i > 0 && layer->owns_tiles != 1) || !isfinite(layer->import_offset_x) ||
            !isfinite(layer->import_offset_y) || !isfinite(layer->import_parallax_x) ||
            !isfinite(layer->import_parallax_y) ||
            (layer->tileset && !rt_pixels_checked_impl_or_null(layer->tileset)))
            return NULL;
    }
    for (int32_t i = 0; i < tilemap->tile_anim_count; ++i) {
        tm_tile_anim *anim = &tilemap->tile_anims[i];
        if (anim->base_tile_id <= 0 || anim->frame_count <= 0 ||
            anim->frame_count > TM_MAX_IMPORT_ANIM_FRAMES || !anim->frame_tiles ||
            !anim->frame_durations || anim->current_frame < 0 ||
            anim->current_frame >= anim->frame_count || anim->ms_per_frame < 0 || anim->timer < 0)
            return NULL;
        for (int32_t frame = 0; frame < anim->frame_count; ++frame) {
            if (anim->frame_tiles[frame] <= 0 || anim->frame_durations[frame] <= 0)
                return NULL;
        }
        if (anim->timer >= anim->frame_durations[anim->current_frame])
            return NULL;
    }
    return tilemap;
}

/// @brief Release a retained reference held in @p *slot, free the payload at refcount 0,
///        and clear the slot to NULL.
/// @details The standard ownership-discipline helper used when loading
///          replaces or removes a tile / layer / object reference. Decrements
///          the refcount, frees the payload only when this was the last
///          reference, and writes NULL into @p *slot so a subsequent reload
///          cannot accidentally double-free. NULL @p slot or NULL @c *slot
///          are no-ops.
/// @param slot Address of an owned runtime-object pointer.
static void tilemap_io_release_ref(void **slot) {
    if (!slot || !*slot)
        return;
    if (rt_obj_release_check0(*slot))
        rt_obj_free(*slot);
    *slot = NULL;
}

/// @brief Validate tilemap grid dimensions before allocating layer storage.
/// @details Checks positive dimensions, int64 tile-count overflow, and host
///          size_t byte-count overflow. This mirrors the constructor guard so
///          file and CSV loads can reject oversized inputs before doing any
///          partial object setup.
/// @param width Tile columns.
/// @param height Tile rows.
/// @return 1 when the grid fits all allocation limits; otherwise 0.
static int tilemap_io_grid_supported(int64_t width, int64_t height) {
    if (width <= 0 || height <= 0)
        return 0;
    if (width > INT64_MAX / height)
        return 0;
    int64_t tile_count = width * height;
    if ((uint64_t)tile_count > (uint64_t)(SIZE_MAX / sizeof(int64_t)))
        return 0;
    return 1;
}

/// @brief Validate a bounded length-bearing runtime string with no embedded NUL.
/// @param value Required runtime string.
/// @param capacity Fixed destination capacity, including its trailing NUL.
/// @param bytes_out Required destination for borrowed string bytes.
/// @param length_out Required destination for the exact byte length.
/// @return `1` when the complete string fits and is C-string-compatible.
static int8_t tilemap_io_bounded_runtime_string(rt_string value,
                                                size_t capacity,
                                                const char **bytes_out,
                                                size_t *length_out) {
    if (!value || capacity == 0 || !bytes_out || !length_out)
        return 0;
    int64_t raw_length = rt_str_len(value);
    if (raw_length < 0 || (uint64_t)raw_length >= (uint64_t)capacity)
        return 0;
    const char *bytes = rt_string_cstr(value);
    size_t length = (size_t)raw_length;
    if (!bytes || memchr(bytes, '\0', length) != NULL)
        return 0;
    *bytes_out = bytes;
    *length_out = length;
    return 1;
}

/// @brief Return the length of a fixed stored string only when NUL-terminated.
/// @param bytes Fixed storage to inspect.
/// @param capacity Storage size in bytes.
/// @param length_out Required destination for the length before the terminator.
/// @return `1` when a terminator exists within the slot.
static int8_t tilemap_io_stored_string_length(const char *bytes,
                                              size_t capacity,
                                              size_t *length_out) {
    if (!bytes || capacity == 0 || !length_out)
        return 0;
    const char *terminator = (const char *)memchr(bytes, '\0', capacity);
    if (!terminator)
        return 0;
    *length_out = (size_t)(terminator - bytes);
    return 1;
}

/// @brief Set the tile property of the tilemap.
/// @details Keys that exceed the fixed on-object storage slot are rejected so
///          two distinct long keys cannot collapse to the same truncated name.
///          Existing keys are updated; new keys are ignored when the per-tile
///          table is full.
/// @param tm Candidate Tilemap handle.
/// @param tile_index Index in the fixed tile-property table.
/// @param key Borrowed case-sensitive runtime key.
/// @param value Signed value to store.
void rt_tilemap_set_tile_property(void *tm, int64_t tile_index, rt_string key, int64_t value) {
    rt_tilemap_impl *tilemap = tilemap_io_checked(tm);
    if (!tilemap || tile_index < 0 || tile_index >= MAX_TILE_PROPS || !key)
        return;
    const char *ckey = NULL;
    size_t klen = 0;
    if (!tilemap_io_bounded_runtime_string(key, MAX_PROP_KEY_LEN, &ckey, &klen))
        return;

    tile_props *p = &tilemap->tile_props[tile_index];
    if (p->count < 0 || p->count > MAX_PROP_KEYS)
        return;
    // Check if key exists
    for (int32_t i = 0; i < p->count; i++) {
        size_t stored_length = 0;
        if (!tilemap_io_stored_string_length(
                p->entries[i].key, sizeof(p->entries[i].key), &stored_length))
            return;
        if (stored_length == klen && memcmp(p->entries[i].key, ckey, klen) == 0) {
            p->entries[i].value = value;
            return;
        }
    }
    // Add new
    if (p->count >= MAX_PROP_KEYS)
        return;
    memcpy(p->entries[p->count].key, ckey, klen);
    p->entries[p->count].key[klen] = '\0';
    p->entries[p->count].value = value;
    p->count++;
}

/// @brief Look up a custom integer property attached to tile `tile_index` (e.g., "damage",
/// "speed_modifier"). Returns `default_val` if the tile has no such property or inputs are
/// invalid. Properties are stored per-tile-type (not per-cell), max 8 keys per tile.
/// @param tm Candidate Tilemap handle.
/// @param tile_index Index in the fixed tile-property table.
/// @param key Borrowed case-sensitive runtime key.
/// @param default_val Fallback for invalid input or an absent key.
/// @return Stored value when present, otherwise @p default_val.
int64_t rt_tilemap_get_tile_property(void *tm,
                                     int64_t tile_index,
                                     rt_string key,
                                     int64_t default_val) {
    rt_tilemap_impl *tilemap = tilemap_io_checked(tm);
    if (!tilemap || tile_index < 0 || tile_index >= MAX_TILE_PROPS || !key)
        return default_val;
    const char *ckey = NULL;
    size_t key_length = 0;
    if (!tilemap_io_bounded_runtime_string(key, MAX_PROP_KEY_LEN, &ckey, &key_length))
        return default_val;

    tile_props *p = &tilemap->tile_props[tile_index];
    if (p->count < 0 || p->count > MAX_PROP_KEYS)
        return default_val;
    for (int32_t i = 0; i < p->count; i++) {
        size_t stored_length = 0;
        if (!tilemap_io_stored_string_length(
                p->entries[i].key, sizeof(p->entries[i].key), &stored_length))
            return default_val;
        if (stored_length == key_length && memcmp(p->entries[i].key, ckey, key_length) == 0)
            return p->entries[i].value;
    }
    return default_val;
}

/// @brief Test whether a tile type has a case-sensitive integer property.
/// @param tm Candidate Tilemap handle.
/// @param tile_index Index in the fixed tile-property table.
/// @param key Borrowed runtime key.
/// @return `1` when present, otherwise `0`.
int8_t rt_tilemap_has_tile_property(void *tm, int64_t tile_index, rt_string key) {
    rt_tilemap_impl *tilemap = tilemap_io_checked(tm);
    if (!tilemap || tile_index < 0 || tile_index >= MAX_TILE_PROPS || !key)
        return 0;
    const char *ckey = NULL;
    size_t key_length = 0;
    if (!tilemap_io_bounded_runtime_string(key, MAX_PROP_KEY_LEN, &ckey, &key_length))
        return 0;

    tile_props *p = &tilemap->tile_props[tile_index];
    if (p->count < 0 || p->count > MAX_PROP_KEYS)
        return 0;
    for (int32_t i = 0; i < p->count; i++) {
        size_t stored_length = 0;
        if (!tilemap_io_stored_string_length(
                p->entries[i].key, sizeof(p->entries[i].key), &stored_length))
            return 0;
        if (stored_length == key_length && memcmp(p->entries[i].key, ckey, key_length) == 0)
            return 1;
    }
    return 0;
}

//=============================================================================
// Auto-Tiling
//=============================================================================

/// @brief Find an existing auto-tile rule or allocate a default active slot.
/// @details New rules initially map all 16 masks back to @p base_tile.
/// @param tilemap Valid Tilemap implementation.
/// @param base_tile Raw tile identifier used as the rule key.
/// @return Borrowed rule pointer, or `NULL` when the fixed table is full.
static autotile_rule *find_or_create_rule(rt_tilemap_impl *tilemap, int64_t base_tile) {
    if (!tilemap || base_tile <= 0)
        return NULL;
    for (int32_t i = 0; i < tilemap->autotile_count; i++) {
        if (tilemap->autotile_rules[i].base_tile == base_tile)
            return &tilemap->autotile_rules[i];
    }
    if (tilemap->autotile_count >= MAX_AUTOTILE_RULES)
        return NULL;
    autotile_rule *r = &tilemap->autotile_rules[tilemap->autotile_count++];
    memset(r, 0, sizeof(autotile_rule));
    r->base_tile = base_tile;
    for (int i = 0; i < 16; i++)
        r->variants[i] = base_tile;
    r->active = 1;
    return r;
}

/// @brief Register the *low half* (variants 0..7) of an autotile rule for `base_tile`. Each
/// `vN` is the tile index to use for one of the 16 neighbor-bitmask cases. Pair with
/// `_set_autotile_hi` to cover variants 8..15. Splits across two calls because the runtime ABI
/// caps function args at 8.
/// @param tm Candidate Tilemap handle.
/// @param base_tile Rule key and default for newly allocated variants.
/// @param v0 Replacement for mask 0.
/// @param v1 Replacement for mask 1.
/// @param v2 Replacement for mask 2.
/// @param v3 Replacement for mask 3.
/// @param v4 Replacement for mask 4.
/// @param v5 Replacement for mask 5.
/// @param v6 Replacement for mask 6.
/// @param v7 Replacement for mask 7.
void rt_tilemap_set_autotile_lo(void *tm,
                                int64_t base_tile,
                                int64_t v0,
                                int64_t v1,
                                int64_t v2,
                                int64_t v3,
                                int64_t v4,
                                int64_t v5,
                                int64_t v6,
                                int64_t v7) {
    rt_tilemap_impl *tilemap = tilemap_io_checked(tm);
    if (!tilemap || base_tile <= 0 || v0 < 0 || v1 < 0 || v2 < 0 || v3 < 0 || v4 < 0 || v5 < 0 ||
        v6 < 0 || v7 < 0)
        return;
    autotile_rule *r = find_or_create_rule(tilemap, base_tile);
    if (!r)
        return;
    r->variants[0] = v0;
    r->variants[1] = v1;
    r->variants[2] = v2;
    r->variants[3] = v3;
    r->variants[4] = v4;
    r->variants[5] = v5;
    r->variants[6] = v6;
    r->variants[7] = v7;
    r->active = 1;
}

/// @brief Register the *high half* (variants 8..15) of an autotile rule for `base_tile`. The
/// 16 entries jointly cover every neighbor pattern (4-bit bitmask of N/E/S/W or NW/NE/SW/SE
/// adjacency). Marks the rule active so `_apply_autotile_region` will start substituting tiles.
/// @param tm Candidate Tilemap handle.
/// @param base_tile Rule key and default for newly allocated variants.
/// @param v8 Replacement for mask 8.
/// @param v9 Replacement for mask 9.
/// @param v10 Replacement for mask 10.
/// @param v11 Replacement for mask 11.
/// @param v12 Replacement for mask 12.
/// @param v13 Replacement for mask 13.
/// @param v14 Replacement for mask 14.
/// @param v15 Replacement for mask 15.
void rt_tilemap_set_autotile_hi(void *tm,
                                int64_t base_tile,
                                int64_t v8,
                                int64_t v9,
                                int64_t v10,
                                int64_t v11,
                                int64_t v12,
                                int64_t v13,
                                int64_t v14,
                                int64_t v15) {
    rt_tilemap_impl *tilemap = tilemap_io_checked(tm);
    if (!tilemap || base_tile <= 0 || v8 < 0 || v9 < 0 || v10 < 0 || v11 < 0 || v12 < 0 ||
        v13 < 0 || v14 < 0 || v15 < 0)
        return;
    autotile_rule *r = find_or_create_rule(tilemap, base_tile);
    if (!r)
        return;
    r->variants[8] = v8;
    r->variants[9] = v9;
    r->variants[10] = v10;
    r->variants[11] = v11;
    r->variants[12] = v12;
    r->variants[13] = v13;
    r->variants[14] = v14;
    r->variants[15] = v15;
    r->active = 1;
}

/// @brief Remove an auto-tile rule and reclaim its fixed slot.
/// @param tm Candidate Tilemap handle.
/// @param base_tile Rule key to deactivate.
void rt_tilemap_clear_autotile(void *tm, int64_t base_tile) {
    rt_tilemap_impl *tilemap = tilemap_io_checked(tm);
    if (!tilemap)
        return;
    for (int32_t i = 0; i < tilemap->autotile_count; i++) {
        if (tilemap->autotile_rules[i].base_tile == base_tile) {
            int32_t remaining = tilemap->autotile_count - i - 1;
            if (remaining > 0) {
                memmove(&tilemap->autotile_rules[i],
                        &tilemap->autotile_rules[i + 1],
                        (size_t)remaining * sizeof(tilemap->autotile_rules[0]));
            }
            tilemap->autotile_count--;
            memset(&tilemap->autotile_rules[tilemap->autotile_count],
                   0,
                   sizeof(tilemap->autotile_rules[0]));
            return;
        }
    }
}

/// @brief Find the active autotile rule whose `base_tile` exactly matches `tile`.
/// @param tilemap Valid Tilemap implementation.
/// @param tile Raw base tile identifier.
/// @return Pointer to the matching rule, or NULL if no rule is registered for this base tile.
static autotile_rule *find_rule(rt_tilemap_impl *tilemap, int64_t tile) {
    for (int32_t i = 0; i < tilemap->autotile_count; i++) {
        if (tilemap->autotile_rules[i].active && tilemap->autotile_rules[i].base_tile == tile)
            return &tilemap->autotile_rules[i];
    }
    return NULL;
}

/// @brief Test whether @p tile belongs to @p rule's base or variant set.
/// @details Used by the autotile neighbor scan to decide whether adjacent tiles should
///   be counted as connected. Without variant checking, placing any variant (e.g., the
///   corners or edge variants) next to another tile of the same type would break the
///   connectivity mask because the neighbour would no longer equal the base exactly.
/// @param rule Already-resolved active autotile rule.
/// @param tile Candidate neighboring identifier.
/// @return 1 if the tiles are considered the same autotile type, 0 otherwise.
static int8_t is_same_rule(const autotile_rule *rule, int64_t tile) {
    if (!rule)
        return 0;
    if (tile == rule->base_tile)
        return 1;
    for (int i = 0; i < 16; i++) {
        if (rule->variants[i] == tile)
            return 1;
    }
    return 0;
}

/// @brief Find the autotile rule for `tile`, checking both base tiles and all variant indices.
/// @details First tries an exact base_tile match via find_rule; on failure scans all active
///          rules' 16-slot variant arrays. Used when placing any variant of an autotile set,
///          not just the canonical base tile.
/// @param tilemap Tilemap containing active rules.
/// @param tile Base or variant identifier to classify.
/// @return Pointer to the governing autotile rule, or NULL if tile belongs to no active set.
static autotile_rule *find_rule_for_tile(rt_tilemap_impl *tilemap, int64_t tile) {
    autotile_rule *rule = find_rule(tilemap, tile);
    if (rule)
        return rule;
    for (int32_t r = 0; r < tilemap->autotile_count; r++) {
        if (!tilemap->autotile_rules[r].active)
            continue;
        for (int v = 0; v < 16; v++) {
            if (tilemap->autotile_rules[r].variants[v] == tile)
                return &tilemap->autotile_rules[r];
        }
    }
    return NULL;
}

/// @brief Apply active four-neighbor auto-tile rules to a clipped base-layer region.
/// @details Reads all outcomes into temporary storage before writing so traversal
///          order cannot affect connectivity. Mask bits are up=1, right=2,
///          down=4, and left=8.
/// @param tm Candidate Tilemap handle.
/// @param rx Requested first logical column.
/// @param ry Requested first logical row.
/// @param rw Requested width.
/// @param rh Requested height.
void rt_tilemap_apply_autotile_region(void *tm, int64_t rx, int64_t ry, int64_t rw, int64_t rh) {
    rt_tilemap_impl *tilemap = tilemap_io_checked(tm);
    if (!tilemap)
        return;
    if (tilemap->autotile_count == 0 || rw <= 0 || rh <= 0)
        return;

    int64_t map_w = rt_tilemap_get_width(tm);
    int64_t map_h = rt_tilemap_get_height(tm);
    int64_t rx_end = rt_pixels_add_sat64(rx, rw);
    int64_t ry_end = rt_pixels_add_sat64(ry, rh);
    int64_t start_x = rx < 0 ? 0 : rx;
    int64_t start_y = ry < 0 ? 0 : ry;
    int64_t end_x = rx_end > map_w ? map_w : rx_end;
    int64_t end_y = ry_end > map_h ? map_h : ry_end;
    if (end_x <= start_x || end_y <= start_y)
        return;

    int64_t region_w = end_x - start_x;
    int64_t region_h = end_y - start_y;
    if (region_w > INT64_MAX / region_h)
        return;
    int64_t count = region_w * region_h;
    if ((uint64_t)count > (uint64_t)(SIZE_MAX / sizeof(int64_t)))
        return;
    int64_t *resolved = (int64_t *)malloc((size_t)count * sizeof(int64_t));
    if (!resolved)
        return;

    for (int64_t y = start_y; y < end_y; y++) {
        for (int64_t x = start_x; x < end_x; x++) {
            int64_t tile = rt_tilemap_get_tile(tm, x, y);
            autotile_rule *rule = find_rule_for_tile(tilemap, tile);
            if (!rule) {
                resolved[(y - start_y) * region_w + (x - start_x)] = tile;
                continue;
            }

            // Compute 4-bit neighbor mask
            int32_t mask = 0;
            // Up
            if (y > 0 && is_same_rule(rule, rt_tilemap_get_tile(tm, x, y - 1)))
                mask |= 1;
            // Right
            if (x < map_w - 1 && is_same_rule(rule, rt_tilemap_get_tile(tm, x + 1, y)))
                mask |= 2;
            // Down
            if (y < map_h - 1 && is_same_rule(rule, rt_tilemap_get_tile(tm, x, y + 1)))
                mask |= 4;
            // Left
            if (x > 0 && is_same_rule(rule, rt_tilemap_get_tile(tm, x - 1, y)))
                mask |= 8;

            resolved[(y - start_y) * region_w + (x - start_x)] = rule->variants[mask];
        }
    }

    for (int64_t y = start_y; y < end_y; y++) {
        for (int64_t x = start_x; x < end_x; x++)
            rt_tilemap_set_tile(tm, x, y, resolved[(y - start_y) * region_w + (x - start_x)]);
    }
    free(resolved);
}

/// @brief Apply active auto-tile rules across the complete base layer.
/// @param tm Candidate Tilemap handle.
void rt_tilemap_apply_autotile(void *tm) {
    if (!tm)
        return;
    rt_tilemap_apply_autotile_region(tm, 0, 0, rt_tilemap_get_width(tm), rt_tilemap_get_height(tm));
}

//=============================================================================
// JSON Save/Load
//=============================================================================

/// @brief Convert a boxed JSON number to an exact int64_t without trapping.
/// @details Accepts boxed i64 values directly and boxed f64 values only when they are
///          finite, integral, and within JSON's exact integer range. This prevents
///          dimensions, tile IDs, and animation indices from being silently rounded.
/// @param boxed Candidate runtime numeric box.
/// @param out Required destination for the exact integer.
/// @return `1` on exact conversion, otherwise `0`.
static int8_t boxed_to_i64_exact(void *boxed, int64_t *out) {
    int64_t i64_value;
    double f64_value;
    if (!out)
        return 0;
    if (rt_box_try_to_i64(boxed, &i64_value)) {
        *out = i64_value;
        return 1;
    }
    if (!rt_box_try_to_f64(boxed, &f64_value) || !isfinite(f64_value))
        return 0;
    if (f64_value < -TMIO_JSON_SAFE_INTEGER_LIMIT || f64_value > TMIO_JSON_SAFE_INTEGER_LIMIT)
        return 0;
    double integral = trunc(f64_value);
    if (integral != f64_value)
        return 0;
    *out = (int64_t)integral;
    return 1;
}

/// @brief Read a required integral numeric value from a JSON map.
/// @details Returns 1 only for exact boxed integers or finite integral doubles in
///          JSON's safe integer range. This is the strict path used for dimensions
///          and tile IDs where silent coercion would corrupt a tilemap.
/// @param map Runtime Map to query.
/// @param key Required NUL-terminated key.
/// @param out Required exact-integer destination.
/// @return `1` when the key exists as an exactly convertible number.
static int8_t map_get_i64_checked(void *map, const char *key, int64_t *out) {
    if (!map || !key || !out)
        return 0;
    return boxed_to_i64_exact(rt_map_get(map, rt_const_cstr(key)), out);
}

/// @brief Read a required finite numeric value from a JSON map.
/// @details Both JSON integer and floating-point boxes are accepted. Non-numeric,
///          non-finite, and missing values are rejected so imported projection
///          metadata cannot inject undefined coordinate arithmetic.
/// @param map Runtime Map to query.
/// @param key Required NUL-terminated key.
/// @param out Required finite-double destination.
/// @return `1` for a present finite numeric value, otherwise `0`.
static int8_t map_get_f64_checked(void *map, const char *key, double *out) {
    if (!map || !key || !out)
        return 0;
    double value = 0.0;
    if (!rt_box_try_to_f64(rt_map_get(map, rt_const_cstr(key)), &value) || !isfinite(value))
        return 0;
    *out = value;
    return 1;
}

/// @brief Create a Seq that owns its elements so they are released when the Seq
///        is collected. Used for the per-serialized-object pixel data arrays so the
///        boxed integer elements are freed along with the containing sequence.
/// @return A caller-owned Seq configured to own elements, or `NULL` on allocation failure.
static void *seq_new_owned(void) {
    void *seq = rt_seq_new();
    if (seq)
        rt_seq_set_owns_elements(seq, 1);
    return seq;
}

/// @brief Store @p value into @p map under @p key and drop the caller's local reference.
/// @details The classic "transfer ownership" idiom: rt_map_set retains its own
///          ref to @p value, so the caller's local ref must be released to
///          avoid a leak. tilemap_io_release_ref handles refcount 0 by freeing.
///          NULL @p value is treated as "skip" so the map never contains NULL
///          entries.
/// @param map Destination runtime Map.
/// @param key NUL-terminated destination key.
/// @param value Caller-owned runtime object whose local reference is consumed.
static void map_set_owned(void *map, const char *key, void *value) {
    if (!map || !value)
        return;
    rt_map_set(map, rt_const_cstr(key), value);
    tilemap_io_release_ref(&value);
}

/// @brief Append @p value to @p seq and drop the caller's local reference.
/// @details Mirrors map_set_owned but for sequence appends. Used during JSON
///          tilemap load when each parsed tile/layer/object is appended into
///          its parent sequence and the loader's temporary reference must be
///          released afterward.
/// @param seq Destination runtime Seq.
/// @param value Caller-owned runtime object whose local reference is consumed.
static void seq_push_owned(void *seq, void *value) {
    if (!seq || !value)
        return;
    rt_seq_push(seq, value);
    tilemap_io_release_ref(&value);
}

/// @brief Box and append an integer to an owning Seq.
/// @details Centralizes the `rt_box_i64` allocation check so JSON serialization can
///          fail cleanly when trap hooks return after an allocation failure.
/// @param seq Destination owning Seq.
/// @param value Signed integer to box and transfer.
/// @return `1` when boxed/appended, otherwise `0`.
static int8_t seq_push_i64_owned(void *seq, int64_t value) {
    if (!seq)
        return 0;
    void *boxed = rt_box_i64(value);
    if (!boxed)
        return 0;
    seq_push_owned(seq, boxed);
    return 1;
}

/// @brief Write exactly @p len bytes to @p f unless an I/O error occurs.
/// @details Loops around `fwrite` so unusual streams that accept partial writes
///          do not cause a valid JSON buffer to be reported as fully written.
/// @param f Open output stream.
/// @param bytes Source byte buffer.
/// @param len Number of bytes to write.
/// @return `1` when every byte is written, otherwise `0`.
static int8_t tmio_write_all(FILE *f, const char *bytes, size_t len) {
    size_t offset = 0;
    if (!f || (!bytes && len > 0))
        return 0;
    while (offset < len) {
        size_t written = fwrite(bytes + offset, 1, len - offset, f);
        if (written == 0)
            return 0;
        offset += written;
    }
    return 1;
}

/// @brief Store a C string in a runtime map under @p key, releasing the temporary
///        rt_string after the map takes ownership of it.
/// @details `rt_string_from_bytes` allocates a new rt_string; after `rt_map_set` retains
///   it, the caller's reference is released via `release_check0/free`. An empty string
///   is substituted when @p value is NULL so the map always contains a valid entry.
/// @param map Destination runtime Map.
/// @param key NUL-terminated destination key.
/// @param value Bytes to copy; may be NULL only when @p length is zero.
/// @param length Exact byte length to copy.
/// @return `1` when the temporary string was allocated and stored.
static int8_t map_set_string_copy(void *map, const char *key, const char *value, size_t length) {
    if (!map || !key || (!value && length > 0))
        return 0;
    rt_string copy = rt_string_from_bytes(value ? value : "", length);
    if (!copy)
        return 0;
    rt_map_set(map, rt_const_cstr(key), copy);
    if (rt_obj_release_check0(copy))
        rt_obj_free(copy);
    return 1;
}

/// @brief Serialize a Pixels object to a JSON map blob with "width", "height", and a
///        flat "pixels" Seq of uint32_t RGBA values encoded as boxed integers.
/// @details Used during `rt_tilemap_save` to embed tileset image data directly in the
///   JSON save file, making tilemap files self-contained. Each pixel is stored as an
///   int64_t to stay within JSON's safe integer range. Returns NULL for NULL input or
///   zero-dimension images.
/// @param pixels Borrowed Pixels object to encode.
/// @return A caller-owned Map blob, or `NULL` for invalid geometry/data or allocation failure.
static void *serialize_pixels_blob(void *pixels) {
    if (!pixels)
        return NULL;

    int64_t width = rt_pixels_width(pixels);
    int64_t height = rt_pixels_height(pixels);
    const uint32_t *raw = rt_pixels_raw_buffer(pixels);
    if (width <= 0 || height <= 0 || width > INT64_MAX / height)
        return NULL;
    int64_t expected = width * height;
    if (!raw && expected > 0)
        return NULL;

    void *blob = rt_map_new();
    void *data = seq_new_owned();
    if (!blob || !data) {
        tilemap_io_release_ref(&data);
        tilemap_io_release_ref(&blob);
        return NULL;
    }
    rt_map_set_int(blob, rt_const_cstr("width"), width);
    rt_map_set_int(blob, rt_const_cstr("height"), height);
    for (int64_t i = 0; i < expected; i++) {
        if (!seq_push_i64_owned(data, (int64_t)raw[i])) {
            tilemap_io_release_ref(&data);
            tilemap_io_release_ref(&blob);
            return NULL;
        }
    }
    map_set_owned(blob, "pixels", data);
    return blob;
}

/// @brief Reconstruct a Pixels object from a serialized blob map (inverse of
///        `serialize_pixels_blob`).
/// @details Reads "width" and "height" from the blob, allocates a new Pixels, then
///   converts each `"pixels"` element to an exact integer in the `uint32_t` range.
///   If dimensions, sequence length, or any element is invalid, the partially
///   constructed Pixels is released and NULL is returned.
/// @param blob Runtime Map produced by `serialize_pixels_blob()`.
/// @return A caller-owned Pixels object, or `NULL` for corrupt input or allocation failure.
static void *deserialize_pixels_blob(void *blob) {
    if (!blob)
        return NULL;
    int64_t width = 0;
    int64_t height = 0;
    if (!map_get_i64_checked(blob, "width", &width) ||
        !map_get_i64_checked(blob, "height", &height))
        return NULL;
    if (width <= 0 || height <= 0)
        return NULL;
    if (width > INT64_MAX / height)
        return NULL;
    void *pixels = rt_pixels_new(width, height);
    if (!pixels)
        return NULL;
    rt_pixels_impl *impl = (rt_pixels_impl *)pixels;
    uint32_t *dst = impl->data;
    void *data = rt_map_get(blob, rt_const_cstr("pixels"));
    int64_t expected = width * height;
    if (!data || rt_seq_len(data) != expected) {
        tilemap_io_release_ref(&pixels);
        return NULL;
    }
    for (int64_t i = 0; i < expected; i++) {
        void *boxed = rt_seq_get(data, i);
        int64_t pixel_value = 0;
        if (!boxed_to_i64_exact(boxed, &pixel_value) || pixel_value < 0 ||
            pixel_value > UINT32_MAX) {
            tilemap_io_release_ref(&pixels);
            return NULL;
        }
        dst[i] = (uint32_t)pixel_value;
    }
    return pixels;
}

/// @brief Replace the tilemap's default tileset with @p pixels, releasing the old one
///        and recomputing derived metrics (cols, rows, tile_count) and syncing layer 0.
/// @details The tileset metrics (tileset_cols/rows, tile_count) are derived from the
///   image dimensions divided by the tile size, so they must be recalculated whenever
///   the tileset changes. Layer 0 mirrors the base tileset, so its copy is updated too.
/// @param tm Tilemap whose owned base slot should change.
/// @param pixels Owned deserialized Pixels to transfer, or `NULL` to clear.
static void assign_base_tileset(rt_tilemap_impl *tm, void *pixels) {
    if (!tm)
        return;
    if (tm->tileset && tm->tileset != pixels)
        rt_heap_release(tm->tileset);
    tm->tileset = pixels;
    tm->tileset_cols = pixels ? rt_pixels_width(pixels) / tm->source_frame_width : 0;
    tm->tileset_rows = pixels ? rt_pixels_height(pixels) / tm->source_frame_height : 0;
    if (tm->tileset_cols > 0 && tm->tileset_rows > INT64_MAX / tm->tileset_cols) {
        tm->tileset_cols = 0;
        tm->tileset_rows = 0;
    }
    tm->tile_count = tm->tileset_cols * tm->tileset_rows;
    tm->layers[0].tileset_cols = tm->tileset_cols;
    tm->layers[0].tileset_rows = tm->tileset_rows;
    tm->layers[0].tile_count = tm->tile_count;
}

/// @brief Replace a specific layer's tileset override with @p pixels, releasing the old
///        one and recomputing that layer's cols/rows/tile_count.
/// @details Per-layer tilesets allow different layers to use different tile graphics
///   (e.g., background layer on a larger tileset, foreground on a smaller one).
///   A NULL @p pixels clears the per-layer override so the layer reverts to the
///   tilemap's default tileset during rendering.
/// @param tm Tilemap containing the destination layer.
/// @param layer Valid zero-based layer index.
/// @param pixels Owned deserialized Pixels to transfer, or `NULL` to clear.
static void assign_layer_tileset(rt_tilemap_impl *tm, int64_t layer, void *pixels) {
    if (!tm || layer < 0 || layer >= tm->layer_count)
        return;
    tm_layer *lyr = &tm->layers[layer];
    if (lyr->tileset && lyr->tileset != pixels)
        rt_heap_release(lyr->tileset);
    lyr->tileset = pixels;
    lyr->tileset_cols = pixels ? rt_pixels_width(pixels) / tm->source_frame_width : 0;
    lyr->tileset_rows = pixels ? rt_pixels_height(pixels) / tm->source_frame_height : 0;
    if (lyr->tileset_cols > 0 && lyr->tileset_rows > INT64_MAX / lyr->tileset_cols) {
        lyr->tileset_cols = 0;
        lyr->tileset_rows = 0;
    }
    lyr->tile_count = lyr->tileset_cols * lyr->tileset_rows;
}

/// @brief Linear-search @p tm's tile-animation table for the entry whose base tile is @p base_tile.
/// @details Tile animations are keyed by their first-frame tile id. The
///          table is small (typically < 32 entries) so a linear scan is
///          cheaper than maintaining a hash. Returns NULL if no matching
///          animation exists or if @p tm is NULL.
/// @param tm Candidate Tilemap implementation.
/// @param base_tile Raw animation key.
/// @return Borrowed animation entry, or `NULL` when absent.
static tm_tile_anim *find_tile_anim(rt_tilemap_impl *tm, int64_t base_tile) {
    if (!tm)
        return NULL;
    for (int32_t i = 0; i < tm->tile_anim_count; i++) {
        if (tm->tile_anims[i].base_tile_id == base_tile)
            return &tm->tile_anims[i];
    }
    return NULL;
}

/// @brief Serialize the tilemap to a JSON file at `path`. Includes version (1), dimensions,
/// tile size, every layer's data + tileset reference, tile properties, and autotile rules.
/// Returns 1 on success, 0 on null inputs / missing path / I/O error.
/// @details Also persists imported layout/layer metadata, collision, and complete
///          variable-duration animation state. The destination is replaced only
///          after a temporary file is fully written and closed.
/// @param tm Candidate Tilemap handle.
/// @param path Borrowed runtime string containing a UTF-8 destination path.
/// @return `1` after successful replacement, otherwise `0`.
int8_t rt_tilemap_save_to_file(void *tm, rt_string path) {
    rt_tilemap_impl *tilemap = tilemap_io_checked(tm);
    if (!tilemap || !path)
        return 0;
    const char *cpath = NULL;
    if (!rt_file_path_from_vstr((const ZannaString *)path, &cpath) || !cpath || cpath[0] == '\0')
        return 0;

    int64_t w = rt_tilemap_get_width(tm);
    int64_t h = rt_tilemap_get_height(tm);
    int64_t tw = rt_tilemap_get_tile_width(tm);
    int64_t th = rt_tilemap_get_tile_height(tm);
    int64_t layer_count = rt_tilemap_get_layer_count(tm);

    int8_t result = 0;
    FILE *f = NULL;
    char *tmp_path = NULL;

    // Build JSON object using Map
    void *root = rt_map_new();
    rt_string json = NULL;
    if (!root)
        return 0;
    rt_map_set_int(root, rt_const_cstr("version"), 1);
    rt_map_set_int(root, rt_const_cstr("width"), w);
    rt_map_set_int(root, rt_const_cstr("height"), h);
    rt_map_set_int(root, rt_const_cstr("tileWidth"), tw);
    rt_map_set_int(root, rt_const_cstr("tileHeight"), th);

    void *layout_obj = rt_map_new();
    if (!layout_obj)
        goto cleanup;
    rt_map_set_int(layout_obj, rt_const_cstr("orientation"), tilemap->import_orientation);
    rt_map_set_int(layout_obj, rt_const_cstr("originTileX"), tilemap->import_origin_tile_x);
    rt_map_set_int(layout_obj, rt_const_cstr("originTileY"), tilemap->import_origin_tile_y);
    rt_map_set_int(
        layout_obj, rt_const_cstr("projectionHeight"), tilemap->import_projection_height);
    rt_map_set_int(layout_obj, rt_const_cstr("sourceFrameWidth"), tilemap->source_frame_width);
    rt_map_set_int(layout_obj, rt_const_cstr("sourceFrameHeight"), tilemap->source_frame_height);
    rt_map_set_int(layout_obj, rt_const_cstr("drawOffsetX"), tilemap->import_draw_offset_x);
    rt_map_set_int(layout_obj, rt_const_cstr("drawOffsetY"), tilemap->import_draw_offset_y);
    rt_map_set_int(layout_obj, rt_const_cstr("renderOrder"), tilemap->import_render_order);
    rt_map_set_int(layout_obj, rt_const_cstr("staggerAxis"), tilemap->import_stagger_axis);
    rt_map_set_int(layout_obj, rt_const_cstr("staggerEven"), tilemap->import_stagger_even);
    rt_map_set_int(layout_obj, rt_const_cstr("hexSideLength"), tilemap->import_hex_side_length);
    rt_map_set_float(layout_obj, rt_const_cstr("skewX"), tilemap->import_skew_x);
    rt_map_set_float(layout_obj, rt_const_cstr("skewY"), tilemap->import_skew_y);
    rt_map_set_float(
        layout_obj, rt_const_cstr("parallaxOriginX"), tilemap->import_parallax_origin_x);
    rt_map_set_float(
        layout_obj, rt_const_cstr("parallaxOriginY"), tilemap->import_parallax_origin_y);
    rt_map_set_int(layout_obj, rt_const_cstr("tileCount"), tilemap->tile_count);
    map_set_owned(root, "importLayout", layout_obj);

    if (tilemap->tileset) {
        void *tileset_obj = serialize_pixels_blob(tilemap->tileset);
        if (!tileset_obj)
            goto cleanup;
        map_set_owned(root, "tileset", tileset_obj);
    }

    // Layers array
    void *layers_arr = seq_new_owned();
    if (!layers_arr)
        goto cleanup;
    for (int64_t li = 0; li < layer_count; li++) {
        if (!tilemap->layers[li].tiles) {
            tilemap_io_release_ref(&layers_arr);
            goto cleanup;
        }
        void *layer_obj = rt_map_new();
        // Tile array
        void *tiles_arr = seq_new_owned();
        if (!layer_obj || !tiles_arr) {
            tilemap_io_release_ref(&tiles_arr);
            tilemap_io_release_ref(&layer_obj);
            tilemap_io_release_ref(&layers_arr);
            goto cleanup;
        }
        for (int64_t y = 0; y < h; y++) {
            for (int64_t x = 0; x < w; x++) {
                int64_t t = rt_tilemap_get_tile_layer(tm, li, x, y);
                if (!seq_push_i64_owned(tiles_arr, t)) {
                    tilemap_io_release_ref(&tiles_arr);
                    tilemap_io_release_ref(&layer_obj);
                    tilemap_io_release_ref(&layers_arr);
                    goto cleanup;
                }
            }
        }
        map_set_owned(layer_obj, "tiles", tiles_arr);
        rt_map_set_int(layer_obj, rt_const_cstr("visible"), rt_tilemap_get_layer_visible(tm, li));
        size_t layer_name_length = 0;
        if (!tilemap_io_stored_string_length(
                tilemap->layers[li].name, sizeof(tilemap->layers[li].name), &layer_name_length) ||
            !map_set_string_copy(layer_obj, "name", tilemap->layers[li].name, layer_name_length)) {
            tilemap_io_release_ref(&layer_obj);
            tilemap_io_release_ref(&layers_arr);
            goto cleanup;
        }
        rt_map_set_float(
            layer_obj, rt_const_cstr("importOffsetX"), tilemap->layers[li].import_offset_x);
        rt_map_set_float(
            layer_obj, rt_const_cstr("importOffsetY"), tilemap->layers[li].import_offset_y);
        rt_map_set_float(
            layer_obj, rt_const_cstr("importParallaxX"), tilemap->layers[li].import_parallax_x);
        rt_map_set_float(
            layer_obj, rt_const_cstr("importParallaxY"), tilemap->layers[li].import_parallax_y);
        if (li > 0 && tilemap->layers[li].tileset) {
            void *tileset_obj = serialize_pixels_blob(tilemap->layers[li].tileset);
            if (!tileset_obj) {
                tilemap_io_release_ref(&layer_obj);
                tilemap_io_release_ref(&layers_arr);
                goto cleanup;
            }
            map_set_owned(layer_obj, "tileset", tileset_obj);
        }
        seq_push_owned(layers_arr, layer_obj);
    }
    map_set_owned(root, "layers", layers_arr);

    // Collision info
    void *coll_obj = rt_map_new();
    if (!coll_obj)
        goto cleanup;
    rt_map_set_int(coll_obj, rt_const_cstr("layer"), rt_tilemap_get_collision_layer(tm));
    void *types_arr = seq_new_owned();
    if (!types_arr) {
        tilemap_io_release_ref(&coll_obj);
        goto cleanup;
    }
    for (int64_t tile_id = 0; tile_id < MAX_TILE_COLLISION_IDS; tile_id++) {
        int64_t coll_type = rt_tilemap_get_collision(tm, tile_id);
        if (coll_type == RT_TILE_COLLISION_NONE)
            continue;
        void *entry = rt_map_new();
        if (!entry) {
            tilemap_io_release_ref(&types_arr);
            tilemap_io_release_ref(&coll_obj);
            goto cleanup;
        }
        rt_map_set_int(entry, rt_const_cstr("tile"), tile_id);
        rt_map_set_int(entry, rt_const_cstr("type"), coll_type);
        seq_push_owned(types_arr, entry);
    }
    map_set_owned(coll_obj, "types", types_arr);
    map_set_owned(root, "collision", coll_obj);

    void *props_arr = seq_new_owned();
    if (!props_arr)
        goto cleanup;
    for (int64_t tile_id = 0; tile_id < MAX_TILE_PROPS; tile_id++) {
        tile_props *props = &tilemap->tile_props[tile_id];
        if (props->count < 0 || props->count > MAX_PROP_KEYS) {
            tilemap_io_release_ref(&props_arr);
            goto cleanup;
        }
        if (props->count <= 0)
            continue;
        void *prop_obj = rt_map_new();
        void *entries = seq_new_owned();
        if (!prop_obj || !entries) {
            tilemap_io_release_ref(&entries);
            tilemap_io_release_ref(&prop_obj);
            tilemap_io_release_ref(&props_arr);
            goto cleanup;
        }
        rt_map_set_int(prop_obj, rt_const_cstr("tile"), tile_id);
        for (int32_t i = 0; i < props->count; i++) {
            void *entry = rt_map_new();
            if (!entry) {
                tilemap_io_release_ref(&entries);
                tilemap_io_release_ref(&prop_obj);
                tilemap_io_release_ref(&props_arr);
                goto cleanup;
            }
            size_t key_length = 0;
            if (!tilemap_io_stored_string_length(
                    props->entries[i].key, sizeof(props->entries[i].key), &key_length) ||
                !map_set_string_copy(entry, "key", props->entries[i].key, key_length)) {
                tilemap_io_release_ref(&entry);
                tilemap_io_release_ref(&entries);
                tilemap_io_release_ref(&prop_obj);
                tilemap_io_release_ref(&props_arr);
                goto cleanup;
            }
            rt_map_set_int(entry, rt_const_cstr("value"), props->entries[i].value);
            seq_push_owned(entries, entry);
        }
        map_set_owned(prop_obj, "entries", entries);
        seq_push_owned(props_arr, prop_obj);
    }
    map_set_owned(root, "tileProperties", props_arr);

    void *autotile_arr = seq_new_owned();
    if (!autotile_arr)
        goto cleanup;
    for (int32_t i = 0; i < tilemap->autotile_count; i++) {
        autotile_rule *rule = &tilemap->autotile_rules[i];
        if (!rule->active)
            continue;
        if (rule->base_tile <= 0) {
            tilemap_io_release_ref(&autotile_arr);
            goto cleanup;
        }
        for (int32_t v = 0; v < 16; ++v) {
            if (rule->variants[v] < 0) {
                tilemap_io_release_ref(&autotile_arr);
                goto cleanup;
            }
        }
        void *rule_obj = rt_map_new();
        void *variants = seq_new_owned();
        if (!rule_obj || !variants) {
            tilemap_io_release_ref(&variants);
            tilemap_io_release_ref(&rule_obj);
            tilemap_io_release_ref(&autotile_arr);
            goto cleanup;
        }
        rt_map_set_int(rule_obj, rt_const_cstr("baseTile"), rule->base_tile);
        for (int32_t v = 0; v < 16; v++) {
            if (!seq_push_i64_owned(variants, rule->variants[v])) {
                tilemap_io_release_ref(&variants);
                tilemap_io_release_ref(&rule_obj);
                tilemap_io_release_ref(&autotile_arr);
                goto cleanup;
            }
        }
        map_set_owned(rule_obj, "variants", variants);
        seq_push_owned(autotile_arr, rule_obj);
    }
    map_set_owned(root, "autotiles", autotile_arr);

    void *anim_arr = seq_new_owned();
    if (!anim_arr)
        goto cleanup;
    for (int32_t i = 0; i < tilemap->tile_anim_count; i++) {
        tm_tile_anim *anim = &tilemap->tile_anims[i];
        if (anim->base_tile_id <= 0 || anim->frame_count <= 0 ||
            anim->frame_count > TM_MAX_IMPORT_ANIM_FRAMES || !anim->frame_tiles ||
            !anim->frame_durations || anim->current_frame < 0 ||
            anim->current_frame >= anim->frame_count || anim->timer < 0) {
            tilemap_io_release_ref(&anim_arr);
            goto cleanup;
        }
        void *anim_obj = rt_map_new();
        void *frames = seq_new_owned();
        void *durations = seq_new_owned();
        if (!anim_obj || !frames || !durations) {
            tilemap_io_release_ref(&frames);
            tilemap_io_release_ref(&durations);
            tilemap_io_release_ref(&anim_obj);
            tilemap_io_release_ref(&anim_arr);
            goto cleanup;
        }
        rt_map_set_int(anim_obj, rt_const_cstr("baseTile"), anim->base_tile_id);
        rt_map_set_int(anim_obj, rt_const_cstr("frameCount"), anim->frame_count);
        rt_map_set_int(anim_obj, rt_const_cstr("msPerFrame"), anim->ms_per_frame);
        rt_map_set_int(anim_obj, rt_const_cstr("timer"), anim->timer);
        rt_map_set_int(anim_obj, rt_const_cstr("currentFrame"), anim->current_frame);
        for (int32_t fidx = 0; fidx < anim->frame_count; fidx++) {
            if (anim->frame_tiles[fidx] <= 0 || anim->frame_durations[fidx] <= 0) {
                tilemap_io_release_ref(&frames);
                tilemap_io_release_ref(&durations);
                tilemap_io_release_ref(&anim_obj);
                tilemap_io_release_ref(&anim_arr);
                goto cleanup;
            }
            if (!seq_push_i64_owned(frames, anim->frame_tiles[fidx])) {
                tilemap_io_release_ref(&frames);
                tilemap_io_release_ref(&durations);
                tilemap_io_release_ref(&anim_obj);
                tilemap_io_release_ref(&anim_arr);
                goto cleanup;
            }
            if (!seq_push_i64_owned(durations, anim->frame_durations[fidx])) {
                tilemap_io_release_ref(&frames);
                tilemap_io_release_ref(&durations);
                tilemap_io_release_ref(&anim_obj);
                tilemap_io_release_ref(&anim_arr);
                goto cleanup;
            }
        }
        map_set_owned(anim_obj, "frames", frames);
        map_set_owned(anim_obj, "durations", durations);
        seq_push_owned(anim_arr, anim_obj);
    }
    map_set_owned(root, "animations", anim_arr);

    // Format as JSON
    json = rt_json_format_pretty(root, 2);
    if (!json)
        goto cleanup;

    const char *json_cstr = rt_string_cstr(json);
    if (!json_cstr)
        goto cleanup;
    int64_t json_length = rt_str_len(json);
    if (json_length <= 0 || json_length > TMIO_MAX_FILE_BYTES ||
        (uint64_t)json_length > (uint64_t)SIZE_MAX ||
        memchr(json_cstr, '\0', (size_t)json_length) != NULL)
        goto cleanup;

    f = rt_file_stdio_open_temp_for_replace_utf8(cpath, &tmp_path);
    if (!f)
        goto cleanup;
    size_t len = (size_t)json_length;
    int8_t wrote_all = tmio_write_all(f, json_cstr, len);
    int write_error = ferror(f) != 0;
    int close_error = !rt_file_stdio_flush_sync_close(f);
    f = NULL;

    result = (wrote_all && !write_error && !close_error) ? 1 : 0;
    if (result)
        result = rt_file_stdio_replace_utf8(tmp_path, cpath) ? 1 : 0;

cleanup:
    if (f) {
        fclose(f);
        f = NULL;
    }
    if (!result && tmp_path)
        (void)rt_file_stdio_unlink_utf8(tmp_path);
    free(tmp_path);
    tilemap_io_release_ref((void **)&json);
    tilemap_io_release_ref(&root);
    return result;
}

/// @brief Load a versioned JSON tilemap saved by `rt_tilemap_save_to_file()`.
/// @details Reads at most 256 MiB, requires exact integral shape/identity fields,
///          validates every grid and embedded Pixels blob, then reconstructs
///          imported layout, layers, collision, properties, rules, and animations.
///          Any fatal schema/allocation failure releases the partial object.
/// @param path Borrowed runtime string containing a UTF-8 source path.
/// @return A caller-owned Tilemap, or `NULL` for I/O, size, parse, version,
///         schema, validation, or allocation failure.
void *rt_tilemap_load_from_file(rt_string path) {
    if (!path)
        return NULL;
    const char *cpath = NULL;
    if (!rt_file_path_from_vstr((const ZannaString *)path, &cpath) || !cpath || cpath[0] == '\0')
        return NULL;

    rt_string json_str = NULL;
    void *root = NULL;
    void *tm = NULL;
    void *result = NULL;

    // Read file contents
    FILE *f = rt_file_stdio_open_utf8(cpath, "rb");
    if (!f)
        return NULL;
    if (tmio_fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    int64_t file_size = (int64_t)tmio_ftell(f);
    if (tmio_fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    if (file_size <= 0) {
        fclose(f);
        return NULL;
    }
    if (file_size > TMIO_MAX_FILE_BYTES || (uint64_t)file_size > SIZE_MAX - 1u) {
        fclose(f);
        return NULL;
    }

    char *buf = (char *)malloc((size_t)file_size + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t read = tmio_read_exact(f, buf, (size_t)file_size) ? (size_t)file_size : 0;
    fclose(f);
    if (read != (size_t)file_size) {
        free(buf);
        return NULL;
    }
    if (memchr(buf, '\0', read) != NULL) {
        free(buf);
        return NULL;
    }
    buf[read] = '\0';

    json_str = rt_string_from_bytes(buf, read);
    free(buf);
    if (!json_str)
        goto cleanup;

    root = rt_json_parse(json_str);
    if (!root)
        goto cleanup;

    int64_t version = 0;
    if (!map_get_i64_checked(root, "version", &version) || version != 1)
        goto cleanup;

    // Extract dimensions as exact integers.
    int64_t w = 0;
    int64_t h = 0;
    int64_t tw = 0;
    int64_t th = 0;
    if (!map_get_i64_checked(root, "width", &w) || !map_get_i64_checked(root, "height", &h) ||
        !map_get_i64_checked(root, "tileWidth", &tw) ||
        !map_get_i64_checked(root, "tileHeight", &th))
        goto cleanup;
    if (!tilemap_io_grid_supported(w, h) || tw <= 0 || th <= 0)
        goto cleanup;
    int64_t expected_tiles = w * h;

    tm = rt_tilemap_new(w, h, tw, th);
    if (!tm)
        goto cleanup;
    rt_tilemap_impl *tilemap = (rt_tilemap_impl *)tm;

    int64_t imported_tile_count = 0;
    void *layout_obj = rt_map_get(root, rt_const_cstr("importLayout"));
    if (layout_obj) {
        int64_t orientation = 0;
        int64_t origin_tile_x = 0;
        int64_t origin_tile_y = 0;
        int64_t projection_height = h;
        int64_t source_frame_width = 0;
        int64_t source_frame_height = 0;
        int64_t draw_offset_x = 0;
        int64_t draw_offset_y = 0;
        int64_t render_order = 0;
        int64_t stagger_axis = 0;
        int64_t stagger_even = 0;
        int64_t hex_side_length = 0;
        double skew_x = 0.0;
        double skew_y = 0.0;
        double parallax_origin_x = 0.0;
        double parallax_origin_y = 0.0;
        if (!map_get_i64_checked(layout_obj, "orientation", &orientation) ||
            !map_get_i64_checked(layout_obj, "originTileX", &origin_tile_x) ||
            !map_get_i64_checked(layout_obj, "originTileY", &origin_tile_y) ||
            (rt_map_get(layout_obj, rt_const_cstr("projectionHeight")) &&
             !map_get_i64_checked(layout_obj, "projectionHeight", &projection_height)) ||
            !map_get_i64_checked(layout_obj, "sourceFrameWidth", &source_frame_width) ||
            !map_get_i64_checked(layout_obj, "sourceFrameHeight", &source_frame_height) ||
            !map_get_i64_checked(layout_obj, "drawOffsetX", &draw_offset_x) ||
            !map_get_i64_checked(layout_obj, "drawOffsetY", &draw_offset_y) ||
            !map_get_i64_checked(layout_obj, "renderOrder", &render_order) ||
            !map_get_i64_checked(layout_obj, "staggerAxis", &stagger_axis) ||
            !map_get_i64_checked(layout_obj, "staggerEven", &stagger_even) ||
            !map_get_i64_checked(layout_obj, "hexSideLength", &hex_side_length) ||
            !map_get_f64_checked(layout_obj, "skewX", &skew_x) ||
            !map_get_f64_checked(layout_obj, "skewY", &skew_y) ||
            !map_get_f64_checked(layout_obj, "parallaxOriginX", &parallax_origin_x) ||
            !map_get_f64_checked(layout_obj, "parallaxOriginY", &parallax_origin_y) ||
            !map_get_i64_checked(layout_obj, "tileCount", &imported_tile_count) ||
            imported_tile_count < 0 ||
            !rt_tilemap_configure_import_layout(tm,
                                                orientation,
                                                origin_tile_x,
                                                origin_tile_y,
                                                source_frame_width,
                                                source_frame_height,
                                                draw_offset_x,
                                                draw_offset_y,
                                                render_order,
                                                stagger_axis,
                                                (int8_t)(stagger_even != 0),
                                                hex_side_length,
                                                skew_x,
                                                skew_y,
                                                parallax_origin_x,
                                                parallax_origin_y,
                                                projection_height))
            goto cleanup;
    }

    void *tileset_blob = rt_map_get(root, rt_const_cstr("tileset"));
    if (tileset_blob) {
        void *pixels = deserialize_pixels_blob(tileset_blob);
        if (!pixels) {
            goto cleanup;
        }
        assign_base_tileset(tilemap, pixels);
        if (layout_obj && imported_tile_count > 0 &&
            !rt_tilemap_set_import_tile_count(tm, imported_tile_count))
            goto cleanup;
    } else if (layout_obj && imported_tile_count > 0) {
        goto cleanup;
    }

    // Load layers
    void *layers_arr = rt_map_get(root, rt_const_cstr("layers"));
    if (!layers_arr)
        goto cleanup;
    int64_t lcount = rt_seq_len(layers_arr);
    if (lcount < 1 || lcount > TM_MAX_LAYERS)
        goto cleanup;
    for (int64_t li = 0; li < lcount; li++) {
        void *layer_obj = rt_seq_get(layers_arr, li);
        if (!layer_obj)
            goto cleanup;

        rt_string lname = (rt_string)rt_map_get(layer_obj, rt_const_cstr("name"));
        const char *layer_name_bytes = "";
        size_t layer_name_length = 0;
        if (lname &&
            !tilemap_io_bounded_runtime_string(
                lname, sizeof(tilemap->layers[0].name), &layer_name_bytes, &layer_name_length))
            goto cleanup;

        int64_t layer_index = li;
        if (li > 0) {
            layer_index = rt_tilemap_add_layer(tm, lname ? lname : rt_const_cstr(""));
            if (layer_index != li)
                goto cleanup;
        } else if (lname) {
            memset(tilemap->layers[0].name, 0, sizeof(tilemap->layers[0].name));
            memcpy(tilemap->layers[0].name, layer_name_bytes, layer_name_length);
        }

        void *tiles_arr = rt_map_get(layer_obj, rt_const_cstr("tiles"));
        if (!tiles_arr || rt_seq_len(tiles_arr) != expected_tiles)
            goto cleanup;
        for (int64_t ti = 0; ti < expected_tiles; ti++) {
            int64_t tile = 0;
            if (!boxed_to_i64_exact(rt_seq_get(tiles_arr, ti), &tile))
                goto cleanup;
            int64_t tx = ti % w;
            int64_t ty = ti / w;
            rt_tilemap_set_tile_layer(tm, layer_index, tx, ty, tile);
        }

        /* Default to visible when the key is absent for legacy files. */
        int64_t vis = 1;
        void *visible_value = rt_map_get(layer_obj, rt_const_cstr("visible"));
        if (visible_value && !boxed_to_i64_exact(visible_value, &vis))
            goto cleanup;
        rt_tilemap_set_layer_visible(tm, layer_index, (int8_t)(vis != 0));
        if (layer_index > 0) {
            void *layer_tileset = rt_map_get(layer_obj, rt_const_cstr("tileset"));
            if (layer_tileset) {
                void *pixels = deserialize_pixels_blob(layer_tileset);
                if (!pixels) {
                    goto cleanup;
                }
                assign_layer_tileset(tilemap, layer_index, pixels);
            }
        }
        void *import_offset_x = rt_map_get(layer_obj, rt_const_cstr("importOffsetX"));
        void *import_offset_y = rt_map_get(layer_obj, rt_const_cstr("importOffsetY"));
        void *import_parallax_x = rt_map_get(layer_obj, rt_const_cstr("importParallaxX"));
        void *import_parallax_y = rt_map_get(layer_obj, rt_const_cstr("importParallaxY"));
        if (import_offset_x || import_offset_y || import_parallax_x || import_parallax_y) {
            double offset_x = 0.0;
            double offset_y = 0.0;
            double parallax_x = 0.0;
            double parallax_y = 0.0;
            if (!map_get_f64_checked(layer_obj, "importOffsetX", &offset_x) ||
                !map_get_f64_checked(layer_obj, "importOffsetY", &offset_y) ||
                !map_get_f64_checked(layer_obj, "importParallaxX", &parallax_x) ||
                !map_get_f64_checked(layer_obj, "importParallaxY", &parallax_y) ||
                !rt_tilemap_configure_import_layer(
                    tm, layer_index, offset_x, offset_y, parallax_x, parallax_y))
                goto cleanup;
        }
    }

    // Load collision
    void *coll = rt_map_get(root, rt_const_cstr("collision"));
    if (coll) {
        int64_t cl = 0;
        if (!map_get_i64_checked(coll, "layer", &cl) || cl < 0 || cl >= lcount)
            goto cleanup;
        rt_tilemap_set_collision_layer(tm, cl);
        void *types = rt_map_get(coll, rt_const_cstr("types"));
        if (!types)
            goto cleanup;
        int64_t type_count = rt_seq_len(types);
        if (type_count < 0 || type_count >= MAX_TILE_COLLISION_IDS)
            goto cleanup;
        uint8_t collision_seen[MAX_TILE_COLLISION_IDS] = {0};
        for (int64_t i = 0; i < type_count; i++) {
            void *entry = rt_seq_get(types, i);
            int64_t tile = 0;
            int64_t type = 0;
            if (!entry || !map_get_i64_checked(entry, "tile", &tile) ||
                !map_get_i64_checked(entry, "type", &type) || tile <= 0 ||
                tile >= MAX_TILE_COLLISION_IDS || collision_seen[tile] ||
                (type != RT_TILE_COLLISION_SOLID && type != RT_TILE_COLLISION_ONE_WAY_UP))
                goto cleanup;
            collision_seen[tile] = 1;
            rt_tilemap_set_collision(tm, tile, type);
        }
    }

    void *props_arr = rt_map_get(root, rt_const_cstr("tileProperties"));
    if (props_arr) {
        int64_t property_group_count = rt_seq_len(props_arr);
        if (property_group_count < 0 || property_group_count > MAX_TILE_PROPS)
            goto cleanup;
        uint8_t property_seen[MAX_TILE_PROPS] = {0};
        for (int64_t i = 0; i < property_group_count; i++) {
            void *prop_obj = rt_seq_get(props_arr, i);
            if (!prop_obj)
                goto cleanup;
            int64_t tile_id = 0;
            if (!map_get_i64_checked(prop_obj, "tile", &tile_id) || tile_id < 0 ||
                tile_id >= MAX_TILE_PROPS || property_seen[tile_id])
                goto cleanup;
            property_seen[tile_id] = 1;
            void *entries = rt_map_get(prop_obj, rt_const_cstr("entries"));
            if (!entries)
                goto cleanup;
            int64_t entry_count = rt_seq_len(entries);
            if (entry_count < 1 || entry_count > MAX_PROP_KEYS)
                goto cleanup;
            for (int64_t j = 0; j < entry_count; j++) {
                void *entry = rt_seq_get(entries, j);
                if (!entry)
                    goto cleanup;
                rt_string key = (rt_string)rt_map_get(entry, rt_const_cstr("key"));
                int64_t value = 0;
                const char *key_bytes = NULL;
                size_t key_length = 0;
                if (!map_get_i64_checked(entry, "value", &value) ||
                    !tilemap_io_bounded_runtime_string(
                        key, MAX_PROP_KEY_LEN, &key_bytes, &key_length))
                    goto cleanup;
                (void)key_bytes;
                (void)key_length;
                rt_tilemap_set_tile_property(tm, tile_id, key, value);
                if (tilemap->tile_props[tile_id].count != j + 1)
                    goto cleanup;
            }
        }
    }

    void *autotiles = rt_map_get(root, rt_const_cstr("autotiles"));
    if (autotiles) {
        int64_t autotile_count = rt_seq_len(autotiles);
        if (autotile_count < 0 || autotile_count > MAX_AUTOTILE_RULES)
            goto cleanup;
        for (int64_t i = 0; i < autotile_count; i++) {
            void *rule_obj = rt_seq_get(autotiles, i);
            if (!rule_obj)
                goto cleanup;
            int64_t base_tile = 0;
            if (!map_get_i64_checked(rule_obj, "baseTile", &base_tile) || base_tile <= 0 ||
                find_rule(tilemap, base_tile))
                goto cleanup;
            void *variants = rt_map_get(rule_obj, rt_const_cstr("variants"));
            if (!variants || rt_seq_len(variants) != 16)
                goto cleanup;
            int64_t variant_values[16];
            for (int32_t vi = 0; vi < 16; vi++) {
                if (!boxed_to_i64_exact(rt_seq_get(variants, vi), &variant_values[vi]) ||
                    variant_values[vi] < 0)
                    goto cleanup;
            }
            rt_tilemap_set_autotile_lo(tm,
                                       base_tile,
                                       variant_values[0],
                                       variant_values[1],
                                       variant_values[2],
                                       variant_values[3],
                                       variant_values[4],
                                       variant_values[5],
                                       variant_values[6],
                                       variant_values[7]);
            rt_tilemap_set_autotile_hi(tm,
                                       base_tile,
                                       variant_values[8],
                                       variant_values[9],
                                       variant_values[10],
                                       variant_values[11],
                                       variant_values[12],
                                       variant_values[13],
                                       variant_values[14],
                                       variant_values[15]);
            if (!find_rule(tilemap, base_tile))
                goto cleanup;
        }
    }

    void *animations = rt_map_get(root, rt_const_cstr("animations"));
    if (animations) {
        int64_t animation_count = rt_seq_len(animations);
        if (animation_count < 0 || animation_count > TM_MAX_TILE_ANIMS)
            goto cleanup;
        for (int64_t i = 0; i < animation_count; i++) {
            void *anim_obj = rt_seq_get(animations, i);
            if (!anim_obj)
                goto cleanup;
            int64_t base_tile = 0;
            int64_t frame_count = 0;
            int64_t ms_per_frame = 0;
            if (!map_get_i64_checked(anim_obj, "baseTile", &base_tile) || base_tile <= 0 ||
                !map_get_i64_checked(anim_obj, "frameCount", &frame_count) || frame_count <= 0 ||
                frame_count > TM_MAX_IMPORT_ANIM_FRAMES ||
                !map_get_i64_checked(anim_obj, "msPerFrame", &ms_per_frame))
                goto cleanup;
            void *frames = rt_map_get(anim_obj, rt_const_cstr("frames"));
            void *durations = rt_map_get(anim_obj, rt_const_cstr("durations"));
            if (!frames || rt_seq_len(frames) != frame_count)
                goto cleanup;
            int configured = 0;
            if (durations) {
                if (rt_seq_len(durations) != frame_count ||
                    (uint64_t)frame_count > (uint64_t)(SIZE_MAX / sizeof(int64_t)))
                    goto cleanup;
                size_t bytes = (size_t)frame_count * sizeof(int64_t);
                int64_t *frame_values = (int64_t *)malloc(bytes);
                int64_t *duration_values = (int64_t *)malloc(bytes);
                if (!frame_values || !duration_values) {
                    free(frame_values);
                    free(duration_values);
                    goto cleanup;
                }
                configured = 1;
                for (int64_t fi = 0; fi < frame_count; ++fi) {
                    if (!boxed_to_i64_exact(rt_seq_get(frames, fi), &frame_values[fi]) ||
                        frame_values[fi] <= 0 ||
                        !boxed_to_i64_exact(rt_seq_get(durations, fi), &duration_values[fi]) ||
                        duration_values[fi] <= 0) {
                        configured = 0;
                        break;
                    }
                }
                if (configured)
                    configured = rt_tilemap_set_import_tile_anim(
                        tm, base_tile, frame_count, frame_values, duration_values);
                free(frame_values);
                free(duration_values);
            } else {
                if (frame_count > TM_MAX_ANIM_FRAMES || ms_per_frame <= 0 ||
                    frame_count - 1 > INT64_MAX - base_tile)
                    goto cleanup;
                rt_tilemap_set_tile_anim(tm, base_tile, frame_count, ms_per_frame);
                configured = find_tile_anim(tilemap, base_tile) != NULL;
                for (int64_t fi = 0; configured && fi < frame_count; fi++) {
                    int64_t frame_tile = 0;
                    if (!boxed_to_i64_exact(rt_seq_get(frames, fi), &frame_tile) ||
                        frame_tile <= 0) {
                        configured = 0;
                        break;
                    }
                    rt_tilemap_set_tile_anim_frame(tm, base_tile, fi, frame_tile);
                }
            }
            if (!configured)
                goto cleanup;
            {
                tm_tile_anim *anim = find_tile_anim(tilemap, base_tile);
                if (!anim)
                    goto cleanup;
                int64_t current = 0;
                void *current_value = rt_map_get(anim_obj, rt_const_cstr("currentFrame"));
                if (current_value && !boxed_to_i64_exact(current_value, &current))
                    goto cleanup;
                if (anim->frame_count > 0) {
                    current %= anim->frame_count;
                    if (current < 0)
                        current += anim->frame_count;
                    anim->current_frame = (int32_t)current;
                } else {
                    anim->current_frame = 0;
                }
                anim->timer = 0;
                void *timer_value = rt_map_get(anim_obj, rt_const_cstr("timer"));
                if (timer_value && !boxed_to_i64_exact(timer_value, &anim->timer))
                    goto cleanup;
                if (anim->timer < 0)
                    anim->timer = 0;
                if (anim->frame_durations && anim->frame_count > 0 &&
                    anim->frame_durations[anim->current_frame] > 0)
                    anim->timer %= anim->frame_durations[anim->current_frame];
            }
        }
    }

    result = tm;
    tm = NULL;

cleanup:
    tilemap_io_release_ref(&tm);
    tilemap_io_release_ref(&root);
    tilemap_io_release_ref((void **)&json_str);
    return result;
}

//=============================================================================
// CSV Import
//=============================================================================

/// @brief Read one bounded CSV line while rejecting embedded NUL bytes.
/// @param file Open CSV stream.
/// @param buffer Destination line buffer.
/// @param capacity Buffer capacity including the trailing NUL.
/// @param length_out Required destination for bytes read, including any newline.
/// @return `1` for a line, `0` at clean EOF, and `-1` for I/O, NUL, or overlength errors.
static int csv_read_line(FILE *file, char *buffer, size_t capacity, size_t *length_out) {
    if (!file || !buffer || capacity < 2 || !length_out)
        return -1;
    *length_out = 0;
    for (;;) {
        int ch = fgetc(file);
        if (ch == EOF) {
            if (ferror(file))
                return -1;
            if (*length_out == 0)
                return 0;
            break;
        }
        if (ch == '\0' || *length_out >= capacity - 1)
            return -1;
        buffer[(*length_out)++] = (char)ch;
        if (ch == '\n')
            break;
    }
    buffer[*length_out] = '\0';
    return 1;
}

/// @brief Strip trailing CR/LF bytes and horizontal whitespace from a CSV line.
/// @details Returns a pointer to the first non-space byte and updates @p len_io to
///          the trimmed length from that returned pointer. Interior whitespace is
///          preserved so fields like `" 12 "` still parse as 12.
/// @param line Mutable NUL-terminated line buffer.
/// @param len_io Required destination for trimmed length.
/// @return Pointer within @p line to the first non-space byte.
static char *csv_trim_line(char *line, size_t *len_io) {
    if (!line || !len_io)
        return line;
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
        line[--len] = '\0';
    while (len > 0 && isspace((unsigned char)line[len - 1]))
        line[--len] = '\0';
    char *start = line;
    while (*start && isspace((unsigned char)*start))
        start++;
    *len_io = strlen(start);
    return start;
}

/// @brief Count comma-delimited fields on a non-empty CSV row.
/// @details Empty rows are handled by the caller. A row with no comma has one
///          field; every comma adds one more field.
/// @param line NUL-terminated trimmed row.
/// @return Field count, or `0` for null/empty input.
static int64_t csv_count_columns(const char *line) {
    int64_t cols = 1;
    if (!line || !*line)
        return 0;
    for (const char *p = line; *p; p++) {
        if (*p == ',')
            cols++;
    }
    return cols;
}

/// @brief Parse one CSV tile field as an int64_t tile id.
/// @details Leading/trailing whitespace is allowed. Empty fields and suffix
///          garbage such as `12abc` and numeric overflow are rejected so tile
///          IDs cannot be silently coerced or aliased to an integer limit.
/// @param field Mutable NUL-terminated field text.
/// @param out Required destination for the parsed/clamped identifier.
/// @return `1` for a syntactically valid integer field, otherwise `0`.
static int8_t csv_parse_tile_field(char *field, int64_t *out) {
    if (!field || !out)
        return 0;
    while (*field && isspace((unsigned char)*field))
        field++;
    if (!*field)
        return 0;
    errno = 0;
    char *end = field;
    long long parsed = strtoll(field, &end, 10);
    if (end == field || errno != 0)
        return 0;
    while (*end && isspace((unsigned char)*end))
        end++;
    if (*end != '\0')
        return 0;
    *out = (int64_t)parsed;
    return 1;
}

/// @brief Load a tilemap from a CSV file (`,`-separated tile indices, one row per line). Two-pass
/// reader: first scans for max columns and row count, then allocates a single-layer tilemap of
/// that size and parses values. Empty lines are skipped; lines longer than 16 KiB fail cleanly.
/// Returns NULL on missing path / empty file / allocation failure.
/// @details Every nonempty row must have the same number of fields. Whitespace
///          around strict integer fields is accepted; malformed fields abort and
///          release the partial Tilemap.
/// @param path Borrowed runtime string containing a UTF-8 CSV path.
/// @param tile_w Logical tile width passed through constructor normalization.
/// @param tile_h Logical tile height passed through constructor normalization.
/// @return A caller-owned one-layer Tilemap, or `NULL` for invalid input, I/O,
///         nonrectangular/malformed data, excessive size, or allocation failure.
void *rt_tilemap_load_csv(rt_string path, int64_t tile_w, int64_t tile_h) {
    if (!path || tile_w <= 0 || tile_h <= 0)
        return NULL;
    const char *cpath = NULL;
    if (!rt_file_path_from_vstr((const ZannaString *)path, &cpath) || !cpath || cpath[0] == '\0')
        return NULL;

    FILE *f = rt_file_stdio_open_utf8(cpath, "rb");
    if (!f)
        return NULL;
    if (tmio_fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    int64_t file_size = tmio_ftell(f);
    if (file_size <= 0 || file_size > TMIO_MAX_FILE_BYTES || tmio_fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }

    // First pass: count rows and require a rectangular column layout.
    int64_t expected_cols = 0;
    int64_t rows = 0;
    char line_buf[16384]; /* max CSV line length */

    for (;;) {
        size_t len = 0;
        int line_status = csv_read_line(f, line_buf, sizeof(line_buf), &len);
        if (line_status == 0)
            break;
        if (line_status < 0) {
            fclose(f);
            return NULL;
        }
        char *trimmed = csv_trim_line(line_buf, &len);
        if (len == 0)
            continue;

        int64_t cols = csv_count_columns(trimmed);
        if (cols <= 0) {
            fclose(f);
            return NULL;
        }
        if (expected_cols == 0)
            expected_cols = cols;
        else if (cols != expected_cols) {
            fclose(f);
            return NULL;
        }
        if (rows == INT64_MAX) {
            fclose(f);
            return NULL;
        }
        rows++;
    }

    if (rows == 0 || expected_cols == 0) {
        fclose(f);
        return NULL;
    }

    if (!tilemap_io_grid_supported(expected_cols, rows)) {
        fclose(f);
        return NULL;
    }

    void *tm = rt_tilemap_new(expected_cols, rows, tile_w, tile_h);
    if (!tm) {
        fclose(f);
        return NULL;
    }

    // Second pass: parse tile values
    if (tmio_fseek(f, 0, SEEK_SET) != 0) {
        tilemap_io_release_ref(&tm);
        fclose(f);
        return NULL;
    }
    int64_t y = 0;

    for (;;) {
        size_t len = 0;
        int line_status = csv_read_line(f, line_buf, sizeof(line_buf), &len);
        if (line_status == 0)
            break;
        if (line_status < 0) {
            tilemap_io_release_ref(&tm);
            fclose(f);
            return NULL;
        }
        char *trimmed = csv_trim_line(line_buf, &len);
        if (len == 0)
            continue;
        if (y >= rows) {
            tilemap_io_release_ref(&tm);
            fclose(f);
            return NULL;
        }

        int64_t x = 0;
        char *tok = trimmed;
        while (tok && x < expected_cols) {
            int64_t val = 0;
            char *comma = strchr(tok, ',');
            if (comma)
                *comma = '\0';
            if (!csv_parse_tile_field(tok, &val)) {
                tilemap_io_release_ref(&tm);
                fclose(f);
                return NULL;
            }

            rt_tilemap_set_tile(tm, x, y, val);
            x++;
            tok = comma ? comma + 1 : NULL;
        }
        if (x != expected_cols || (tok && *tok)) {
            tilemap_io_release_ref(&tm);
            fclose(f);
            return NULL;
        }
        y++;
    }

    if (y != rows) {
        tilemap_io_release_ref(&tm);
        fclose(f);
        return NULL;
    }
    fclose(f);
    return tm;
}
