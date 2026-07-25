//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/game/rt_leveldata.c
/// @file
/// @brief Implements JSON level loading into a TileMap and copied spawn metadata.
//
// Purpose: JSON level loader. Parses level file with tilemap data + objects.
//
// Level JSON format:
//   { "width": N, "height": N, "tileWidth": N, "tileHeight": N,
//     "properties": { "theme": "...", "playerStartX": N, "playerStartY": N },
//     "layers": [{ "name": "terrain", "type": "tiles", "data": [0,1,...] }],
//     "objects": [{ "type": "enemy", "id": "slime", "x": N, "y": N }, ...] }
//
//===----------------------------------------------------------------------===//

#include "rt_leveldata.h"
#include "rt_numeric.h"
#include "rt_box.h"
#include "rt_file_ext.h"
#include "rt_json.h"
#include "rt_jsonpath.h"
#include "rt_map.h"
#include "rt_object.h"
#include "rt_seq.h"
#include "rt_string.h"
#include "rt_tilemap.h"
#include "rt_trap.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

/// @brief Convert a boxed JSON numeric value to an integer.
/// @param val Candidate boxed value.
/// @return Unboxed integer, saturating conversion of a boxed double, or zero
///         for null/unsupported box tags.
static int64_t json_val_to_i64(void *val) {
    if (!val)
        return 0;
    int64_t tag = rt_box_type(val);
    if (tag == 0)
        return rt_unbox_i64(val); // RT_BOX_I64 = 0
    if (tag == 1)
        return (int64_t)rt_f64_to_i64(rt_unbox_f64(val)); // RT_BOX_F64 = 1 (saturating, VDOC-037)
    return 0;
}

/// @brief True if @p o is a JSON array (rt_seq). Used to guard rt_seq_len /
///        rt_seq_get, which trap on a non-seq object — so a malformed level
///        file (e.g. "layers": 5) degrades to "skip" instead of crashing.
/// @param o Candidate parsed JSON node.
/// @return Nonzero only for a non-null RT_SEQ_CLASS_ID object.
static int level_is_array(void *o) {
    return o && rt_obj_class_id(o) == RT_SEQ_CLASS_ID;
}

/// @brief Maximum copied spawn-object records per loaded level.
#define LEVEL_MAX_OBJECTS 512

/// @brief One fixed-capacity copied object/spawn record.
typedef struct {
    char type[32];
    char id[32];
    int64_t x, y;
} level_object_t;

/// @brief Private TileMap, spawn metadata, and properties of LevelData.
typedef struct {
    void *tilemap;
    level_object_t objects[LEVEL_MAX_OBJECTS];
    int32_t object_count;
    int64_t player_start_x;
    int64_t player_start_y;
    char theme[32];
} leveldata_impl;

/// @brief Validate and cast an opaque LevelData handle.
/// @param level Candidate handle; `NULL` is accepted.
/// @param api Trap message used for a non-null class mismatch.
/// @return The LevelData payload when valid; otherwise `NULL`.
/// @details A mismatched handle raises a runtime trap.
static leveldata_impl *checked_leveldata(void *level, const char *api) {
    if (!level)
        return NULL;
    if (rt_obj_class_id(level) != RT_LEVELDATA_CLASS_ID) {
        rt_trap(api);
        return NULL;
    }
    return (leveldata_impl *)level;
}

/// @brief Drop one GC reference to @p obj and free it if the count hit zero.
/// @param obj Runtime object reference to release; `NULL` is a no-op.
static void leveldata_release_obj(void *obj) {
    if (obj && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

/// @brief GC finalizer: release the level's referenced tilemap.
/// @param obj LevelData payload being finalized; `NULL` is accepted.
/// @details Releases the owned TileMap reference and clears its slot.
static void leveldata_finalizer(void *obj) {
    leveldata_impl *ld = (leveldata_impl *)obj;
    if (!ld || !ld->tilemap)
        return;
    if (rt_obj_release_check0(ld->tilemap))
        rt_obj_free(ld->tilemap);
    ld->tilemap = NULL;
}

/// @brief Copy @p src into a fixed-size field, truncating on a UTF-8 boundary.
/// @param dest Destination byte buffer.
/// @param dest_size Capacity including the NUL terminator.
/// @param src NUL-terminated source, or `NULL` for empty output.
/// @details The level model stores object type/id and the theme in 32-byte fields.
///          A plain byte truncation could split a multi-byte UTF-8 sequence and
///          emit an invalid trailing fragment (VDOC-239); this backs the cut off to
///          the start of any partially-copied character so the result is always
///          valid UTF-8. @p dest_size includes the NUL terminator.
static void level_copy_field_utf8(char *dest, size_t dest_size, const char *src) {
    if (!dest || dest_size == 0)
        return;
    dest[0] = '\0';
    if (!src)
        return;
    size_t limit = dest_size - 1;
    size_t n = 0;
    while (n < limit && src[n] != '\0')
        n++;
    // If truncation landed inside a multi-byte sequence, back off over any
    // continuation bytes (10xxxxxx) so the incomplete character is dropped whole.
    if (src[n] != '\0') {
        while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80)
            n--;
    }
    memcpy(dest, src, n);
    dest[n] = '\0';
}

/// @brief Load a level from a JSON file, creating a tilemap and extracting objects.
/// @param path Runtime string containing the level file path.
/// @return A new LevelData reference, or `NULL` for null/missing/empty input,
///         parse failure, invalid dimensions, size overflow, TileMap/object
///         allocation failure, or other soft loading failure.
/// @details Parses a JSON level file with "width", "height", "tileWidth", "tileHeight",
///          "layers" (tile data), "objects" (entity spawn points), and "properties"
///          (playerStartX/Y, theme). Creates a tilemap from tile layer data and stores
///          up to 512 named objects with type, id, and position. All tile layers
///          currently overwrite the same base Tilemap in input order (VDOC-239).
///          Nonpositive tile dimensions default independently to 32. Malformed
///          non-array layer/object nodes are skipped. Object strings are copied
///          into 32-byte fields and excess objects are ignored. The returned
///          object owns its TileMap; temporary root state is released.
void *rt_leveldata_load(void *path) {
    if (!path)
        return NULL;

    leveldata_impl *ld = NULL;
    void *root = NULL;

    // Load is a nullable factory: a missing file must soft-fail to NULL so callers
    // can implement the documented fallback, rather than trapping inside the
    // hardened read path. Pre-check existence with the non-trapping helper, mirroring
    // Zanna.Game.Config.Load (VDOC-238).
    if (!rt_io_file_exists((rt_string)path))
        return NULL;

    rt_string text = rt_io_file_read_all_text((rt_string)path);
    if (!text)
        return NULL;
    if (rt_str_len(text) == 0) {
        rt_string_unref(text);
        return NULL;
    }

    root = rt_json_parse(text);
    rt_string_unref(text);
    if (!root)
        goto fail;

    // Read dimensions
    int64_t w = rt_jsonpath_get_int(root, rt_const_cstr("width"));
    int64_t h = rt_jsonpath_get_int(root, rt_const_cstr("height"));
    int64_t tw = rt_jsonpath_get_int(root, rt_const_cstr("tileWidth"));
    int64_t th = rt_jsonpath_get_int(root, rt_const_cstr("tileHeight"));
    if (w <= 0 || h <= 0)
        goto fail;
    if (tw <= 0)
        tw = 32;
    if (th <= 0)
        th = 32;

    // Create level data
    ld = (leveldata_impl *)rt_obj_new_i64(RT_LEVELDATA_CLASS_ID, (int64_t)sizeof(leveldata_impl));
    if (!ld)
        goto fail;
    memset(ld, 0, sizeof(leveldata_impl));
    rt_obj_set_finalizer(ld, leveldata_finalizer);

    // Read properties
    void *props = rt_jsonpath_get(root, rt_const_cstr("properties"));
    if (props) {
        rt_string theme = rt_jsonpath_get_str(props, rt_const_cstr("theme"));
        if (theme) {
            const char *ct = rt_string_cstr(theme);
            if (ct)
                level_copy_field_utf8(ld->theme, sizeof(ld->theme), ct);
        }
        ld->player_start_x = rt_jsonpath_get_int(props, rt_const_cstr("playerStartX"));
        ld->player_start_y = rt_jsonpath_get_int(props, rt_const_cstr("playerStartY"));
    }

    // Validate w*h before allocating the tilemap (validate-then-allocate).
    if (w > INT64_MAX / h)
        goto fail;
    int64_t tile_limit = w * h;

    // Create tilemap
    ld->tilemap = rt_tilemap_new(w, h, tw, th);
    if (!ld->tilemap)
        goto fail;

    // Read tile layers
    void *layers = rt_jsonpath_get(root, rt_const_cstr("layers"));
    if (level_is_array(layers)) {
        int64_t layerCount = rt_seq_len(layers);
        for (int64_t li = 0; li < layerCount; li++) {
            void *layer = rt_seq_get(layers, li);
            if (!layer)
                continue;
            rt_string layerType = rt_jsonpath_get_str(layer, rt_const_cstr("type"));
            if (!layerType)
                continue;
            const char *typeStr = rt_string_cstr(layerType);
            if (!typeStr || strcmp(typeStr, "tiles") != 0)
                continue;

            void *data = rt_jsonpath_get(layer, rt_const_cstr("data"));
            if (!level_is_array(data))
                continue;
            int64_t dataLen = rt_seq_len(data);
            for (int64_t i = 0; i < dataLen && i < tile_limit; i++) {
                void *val = rt_seq_get(data, i);
                if (val) {
                    int64_t tile = json_val_to_i64(val);
                    rt_tilemap_set_tile(ld->tilemap, i % w, i / w, tile);
                }
            }
        }
    }

    // Read objects
    void *objects = rt_jsonpath_get(root, rt_const_cstr("objects"));
    if (level_is_array(objects)) {
        int64_t objCount = rt_seq_len(objects);
        for (int64_t i = 0; i < objCount && ld->object_count < LEVEL_MAX_OBJECTS; i++) {
            void *obj = rt_seq_get(objects, i);
            if (!obj)
                continue;

            level_object_t *lo = &ld->objects[ld->object_count];
            memset(lo, 0, sizeof(level_object_t));

            rt_string otype = rt_jsonpath_get_str(obj, rt_const_cstr("type"));
            rt_string oid = rt_jsonpath_get_str(obj, rt_const_cstr("id"));
            if (otype) {
                const char *s = rt_string_cstr(otype);
                if (s)
                    level_copy_field_utf8(lo->type, sizeof(lo->type), s);
            }
            if (oid) {
                const char *s = rt_string_cstr(oid);
                if (s) {
                    level_copy_field_utf8(lo->id, sizeof(lo->id), s);
                }
            }
            lo->x = rt_jsonpath_get_int(obj, rt_const_cstr("x"));
            lo->y = rt_jsonpath_get_int(obj, rt_const_cstr("y"));
            ld->object_count++;
        }
    }

    leveldata_release_obj(root);
    return ld;

fail:
    leveldata_release_obj(root);
    if (ld)
        leveldata_release_obj(ld);
    return NULL;
}

/// @brief Get the tilemap created from the level's tile layers.
/// @param level LevelData to query.
/// @return Borrowed TileMap owned by @p level, or `NULL` for null/invalid.
/// @details A non-null invalid handle raises a runtime trap.
void *rt_leveldata_get_tilemap(void *level) {
    leveldata_impl *ld =
        checked_leveldata(level, "LevelData.GetTilemap: expected Zanna.Game.LevelData");
    return ld ? ld->tilemap : NULL;
}

/// @brief Get the number of objects (entity spawn points) in the level.
/// @param level LevelData to query.
/// @return Copied object count in `[0, 512]`, or zero for null/invalid.
int64_t rt_leveldata_object_count(void *level) {
    leveldata_impl *ld =
        checked_leveldata(level, "LevelData.ObjectCount: expected Zanna.Game.LevelData");
    return ld ? ld->object_count : 0;
}

/// @brief Get the type string of an object at the given index (e.g., "enemy", "item").
/// @param level LevelData to query.
/// @param index Zero-based object index.
/// @return Caller-owned copy of the type, immortal empty singleton for invalid
///         input/empty field, or `NULL` on allocation failure.
rt_string rt_leveldata_object_type(void *level, int64_t index) {
    leveldata_impl *ld =
        checked_leveldata(level, "LevelData.ObjectType: expected Zanna.Game.LevelData");
    if (!ld || index < 0 || index >= ld->object_count)
        return rt_const_cstr("");
    return rt_const_cstr(ld->objects[index].type);
}

/// @brief Get the ID string of an object at the given index.
/// @param level LevelData to query.
/// @param index Zero-based object index.
/// @return Caller-owned copy of the ID, immortal empty singleton for invalid
///         input/empty field, or `NULL` on allocation failure.
rt_string rt_leveldata_object_id(void *level, int64_t index) {
    leveldata_impl *ld =
        checked_leveldata(level, "LevelData.ObjectId: expected Zanna.Game.LevelData");
    if (!ld || index < 0 || index >= ld->object_count)
        return rt_const_cstr("");
    return rt_const_cstr(ld->objects[index].id);
}

/// @brief Get the X position of an object at the given index.
/// @param level LevelData to query.
/// @param index Zero-based object index.
/// @return Stored X coordinate, or zero for invalid input/index.
int64_t rt_leveldata_object_x(void *level, int64_t index) {
    leveldata_impl *ld =
        checked_leveldata(level, "LevelData.ObjectX: expected Zanna.Game.LevelData");
    if (!ld || index < 0 || index >= ld->object_count)
        return 0;
    return ld->objects[index].x;
}

/// @brief Get the Y position of an object at the given index.
/// @param level LevelData to query.
/// @param index Zero-based object index.
/// @return Stored Y coordinate, or zero for invalid input/index.
int64_t rt_leveldata_object_y(void *level, int64_t index) {
    leveldata_impl *ld =
        checked_leveldata(level, "LevelData.ObjectY: expected Zanna.Game.LevelData");
    if (!ld || index < 0 || index >= ld->object_count)
        return 0;
    return ld->objects[index].y;
}

/// @brief Get the player's starting X position from the level properties.
/// @param level LevelData to query.
/// @return Stored property, defaulting to zero when absent/null/invalid.
int64_t rt_leveldata_player_start_x(void *level) {
    leveldata_impl *ld =
        checked_leveldata(level, "LevelData.PlayerStartX: expected Zanna.Game.LevelData");
    return ld ? ld->player_start_x : 0;
}

/// @brief Get the player's starting Y position from the level properties.
/// @param level LevelData to query.
/// @return Stored property, defaulting to zero when absent/null/invalid.
int64_t rt_leveldata_player_start_y(void *level) {
    leveldata_impl *ld =
        checked_leveldata(level, "LevelData.PlayerStartY: expected Zanna.Game.LevelData");
    return ld ? ld->player_start_y : 0;
}

/// @brief Get the theme name from the level properties (e.g., "forest", "cave").
/// @param level LevelData to query.
/// @return Caller-owned copy of the theme, immortal empty singleton when
///         absent/null/invalid, or `NULL` on allocation failure.
/// @details Every accessor above traps on a non-null wrong LevelData class.
rt_string rt_leveldata_get_theme(void *level) {
    leveldata_impl *ld = checked_leveldata(level, "LevelData.Theme: expected Zanna.Game.LevelData");
    if (!ld)
        return rt_const_cstr("");
    return rt_const_cstr(ld->theme);
}
