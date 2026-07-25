//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/game/rt_scene_editor.h
// Purpose: Scene-owned editable level document primitives for IDE scene tools,
//   including dependency-aware Tiled JSON/TMX import.
//
// Key invariants:
//   - SceneDocument loads never publish partially parsed external documents.
//   - Tiled filesystem and asset-package dependency resolution are separate,
//     explicit entry points with Result companions for recoverable failures.
//   - Tile-behavior (collision/tileProperties/animations/autotiles) and
//     camera/lighting sections load into typed state, serialize canonically,
//     and retain unrecognized members verbatim.
//
// Ownership/Lifetime:
//   - Returned SceneDocument and Result handles are owned runtime objects.
//   - Input runtime strings are borrowed for the duration of each call.
//
// Links: rt_scene_editor.cpp, rt_tiled_import.cpp,
//   docs/adr/0140-tiled-map-and-scene-import.md,
//   docs/adr/0174-scene-object-authoring-metadata-and-duplication.md,
//   docs/adr/0164-backward-compatible-2d-scene-object-hierarchy.md,
//   docs/adr/0171-bounded-scene-flood-fill-and-studio-tile-tools.md,
//   docs/adr/0176-typed-tile-behavior-sections.md,
//   docs/adr/0177-scene-camera-and-lighting-sections.md
//
//===----------------------------------------------------------------------===//

#pragma once

#include "rt_string.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RT_GAME_SCENE_CLASS_ID INT64_C(-0x510301)

void *rt_game_scene_new(int64_t width, int64_t height, int64_t tile_width, int64_t tile_height);
void *rt_game_scene_load_json(rt_string text);
/// @brief Load a SceneDocument from JSON text and return a Zanna.Result.
/// @details Returns `Ok(SceneDocument)` when the document has no retained error
///          diagnostics and `Err(message)` when JSON/schema loading records an
///          error. The compatibility rt_game_scene_load_json() API still
///          returns diagnostic documents for callers that need full records.
/// @param text Scene JSON text.
/// @return Opaque Zanna.Result object containing a SceneDocument or error string.
void *rt_game_scene_load_json_result(rt_string text);
void *rt_game_scene_load_file(rt_string path);
/// @brief Load a SceneDocument from a file and return a Zanna.Result.
/// @details Returns `Ok(SceneDocument)` when the file loads without retained
///          error diagnostics and `Err(message)` when the file is missing,
///          oversized, malformed, or schema-invalid.
/// @param path Scene file path.
/// @return Opaque Zanna.Result object containing a SceneDocument or error string.
void *rt_game_scene_load_file_result(rt_string path);
/// @brief Import a filesystem Tiled JSON/TMX map as a SceneDocument, or NULL.
void *rt_game_scene_import_tiled(rt_string path);
/// @brief Result-returning filesystem companion to @ref rt_game_scene_import_tiled.
void *rt_game_scene_import_tiled_result(rt_string path);
/// @brief Import a Tiled JSON/TMX map and dependencies through the asset manager.
void *rt_game_scene_import_tiled_asset(rt_string path);
/// @brief Result-returning asset companion to @ref rt_game_scene_import_tiled_asset.
void *rt_game_scene_import_tiled_asset_result(rt_string path);
rt_string rt_game_scene_to_json(void *scene);
int8_t rt_game_scene_save_file(void *scene, rt_string path);
rt_string rt_game_scene_last_error(void *scene);
void *rt_game_scene_diagnostics(void *scene);
void *rt_game_scene_diagnostic_records(void *scene);
int8_t rt_game_scene_has_errors(void *scene);
void rt_game_scene_clear_diagnostics(void *scene);

int64_t rt_game_scene_get_width(void *scene);
int64_t rt_game_scene_get_height(void *scene);
int64_t rt_game_scene_get_tile_width(void *scene);
int64_t rt_game_scene_get_tile_height(void *scene);

int64_t rt_game_scene_add_layer(void *scene, rt_string name);
int64_t rt_game_scene_layer_count(void *scene);
rt_string rt_game_scene_layer_name(void *scene, int64_t layer);
void rt_game_scene_set_layer_name(void *scene, int64_t layer, rt_string name);
int8_t rt_game_scene_layer_visible(void *scene, int64_t layer);
void rt_game_scene_set_layer_visible(void *scene, int64_t layer, int8_t visible);
void rt_game_scene_move_layer(void *scene, int64_t from, int64_t to);
void rt_game_scene_remove_layer(void *scene, int64_t layer);
int64_t rt_game_scene_get_tile(void *scene, int64_t layer, int64_t x, int64_t y);
void rt_game_scene_set_tile(void *scene, int64_t layer, int64_t x, int64_t y, int64_t tile);
void rt_game_scene_fill_tiles(
    void *scene, int64_t layer, int64_t x, int64_t y, int64_t w, int64_t h, int64_t tile);
/// @brief Replace one four-connected region and return its exact changed-cell count.
/// @details Work storage is fully allocated before mutation, so allocation
///          failure and invalid/already-equal requests return zero atomically.
int64_t rt_game_scene_flood_fill_tiles(
    void *scene, int64_t layer, int64_t x, int64_t y, int64_t tile);
void rt_game_scene_set_layer_asset(void *scene, int64_t layer, rt_string asset_path);
rt_string rt_game_scene_layer_asset(void *scene, int64_t layer);

int64_t rt_game_scene_add_object(void *scene, rt_string type, rt_string id, int64_t x, int64_t y);
int64_t rt_game_scene_object_count(void *scene);
void rt_game_scene_remove_object(void *scene, int64_t index);
rt_string rt_game_scene_object_type(void *scene, int64_t index);
rt_string rt_game_scene_object_id(void *scene, int64_t index);
int64_t rt_game_scene_object_x(void *scene, int64_t index);
int64_t rt_game_scene_object_y(void *scene, int64_t index);
/// @brief Return an object's organizational parent index, or -1 for a root.
/// @details Invalid object indices also return -1. Positions remain absolute
///          scene-space coordinates and do not inherit from this parent.
int64_t rt_game_scene_object_parent(void *scene, int64_t index);
/// @brief Set an object's organizational parent when the hierarchy stays valid.
/// @details Accepts -1 for a root. Invalid indices, self-parenting, and cycles
///          return false without mutation. Parenting also returns false when
///          all 256 serialized property slots are already public properties.
int8_t rt_game_scene_try_set_object_parent(void *scene, int64_t index, int64_t parent);
void rt_game_scene_set_object_metadata(void *scene, int64_t index, rt_string type, rt_string id);
void rt_game_scene_set_object_position(void *scene, int64_t index, int64_t x, int64_t y);
int64_t rt_game_scene_duplicate_object(void *scene, int64_t index, rt_string id);
void rt_game_scene_set_object_property(void *scene, int64_t index, rt_string key, rt_string value);
rt_string rt_game_scene_get_object_property(void *scene, int64_t index, rt_string key);
void rt_game_scene_delete_object_property(void *scene, int64_t index, rt_string key);
int64_t rt_game_scene_object_get_int(void *scene, int64_t index, rt_string key, int64_t def);
rt_string rt_game_scene_object_get_str(void *scene, int64_t index, rt_string key, rt_string def);
double rt_game_scene_object_get_float(void *scene, int64_t index, rt_string key, double def);
int8_t rt_game_scene_object_get_bool(void *scene, int64_t index, rt_string key, int8_t def);
int8_t rt_game_scene_object_has(void *scene, int64_t index, rt_string key);
rt_string rt_game_scene_object_property_kind(void *scene, int64_t index, rt_string key);
void *rt_game_scene_object_keys(void *scene, int64_t index);
void rt_game_scene_object_set_null(void *scene, int64_t index, rt_string key);
void rt_game_scene_object_set_int(void *scene, int64_t index, rt_string key, int64_t value);
void rt_game_scene_object_set_str(void *scene, int64_t index, rt_string key, rt_string value);
void rt_game_scene_object_set_float(void *scene, int64_t index, rt_string key, double value);
void rt_game_scene_object_set_bool(void *scene, int64_t index, rt_string key, int8_t value);
void rt_game_scene_object_remove(void *scene, int64_t index, rt_string key);
int64_t rt_game_scene_count_of_type(void *scene, rt_string type);
int64_t rt_game_scene_object_of_type(void *scene, rt_string type, int64_t n);
int64_t rt_game_scene_find_object(void *scene, rt_string id);
/// @brief Find an object by id and return its index as an Option.
/// @details Returns `SomeI64(index)` when an object with @p id exists and
///          `None` when absent, avoiding the legacy `-1` sentinel.
/// @param scene SceneDocument handle.
/// @param id Object id to search for.
/// @return Opaque Zanna.Option containing the object index, or None.
void *rt_game_scene_find_object_option(void *scene, rt_string id);
void rt_game_scene_move_object(void *scene, int64_t from, int64_t to);

/// @brief Return one tile's authored collision kind (0 none, 1 solid, 2 one-way-up).
int64_t rt_game_scene_tile_collision(void *scene, int64_t tile);
/// @brief Author one tile's collision kind; kind 0 removes the record.
/// @details Tile IDs outside 1..4095 and kinds outside 0..2 are edit-time
///          warning no-ops, matching Tilemap's behavior-table addressability.
void rt_game_scene_set_tile_collision(void *scene, int64_t tile, int64_t kind);
/// @brief Return ascending tile IDs whose authored collision kind is not none.
void *rt_game_scene_collision_tiles(void *scene);
int64_t rt_game_scene_collision_layer(void *scene);
void rt_game_scene_set_collision_layer(void *scene, int64_t layer);

/// @brief Return "int", "bool", or "" for one authored tile property.
rt_string rt_game_scene_tile_property_kind(void *scene, int64_t tile, rt_string key);
int64_t rt_game_scene_tile_property_int(void *scene, int64_t tile, rt_string key, int64_t def);
int8_t rt_game_scene_tile_property_bool(void *scene, int64_t tile, rt_string key, int8_t def);
void rt_game_scene_set_tile_property_int(void *scene, int64_t tile, rt_string key, int64_t value);
void rt_game_scene_set_tile_property_bool(void *scene, int64_t tile, rt_string key, int8_t value);
void rt_game_scene_remove_tile_property(void *scene, int64_t tile, rt_string key);
/// @brief Return one tile's authored property keys in lexicographic order.
void *rt_game_scene_tile_property_keys(void *scene, int64_t tile);
/// @brief Return ascending tile IDs carrying at least one typed property.
void *rt_game_scene_tile_property_tiles(void *scene);

/// @brief Author one tile animation from equal-length frame/duration sequences.
/// @details 1..4096 frames, every duration positive; invalid input is an
///          edit-time warning no-op that leaves any existing rule unchanged.
void rt_game_scene_set_tile_anim(void *scene, int64_t base_tile, void *frames, void *durations_ms);
void *rt_game_scene_tile_anim_frames(void *scene, int64_t base_tile);
void *rt_game_scene_tile_anim_durations(void *scene, int64_t base_tile);
void rt_game_scene_remove_tile_anim(void *scene, int64_t base_tile);
/// @brief Return ascending base tile IDs carrying an authored animation.
void *rt_game_scene_tile_anim_bases(void *scene);

/// @brief Author one autotile rule from exactly 16 variant tile IDs.
void rt_game_scene_set_autotile_rule(void *scene, int64_t base_tile, void *variants);
void *rt_game_scene_autotile_variants(void *scene, int64_t base_tile);
void rt_game_scene_remove_autotile_rule(void *scene, int64_t base_tile);
/// @brief Return ascending base tile IDs carrying an authored autotile rule.
void *rt_game_scene_autotile_bases(void *scene);

/// @brief Return "int", "string", or "" for one camera-section field.
rt_string rt_game_scene_camera_field_kind(void *scene, rt_string key);
int64_t rt_game_scene_camera_get_int(void *scene, rt_string key, int64_t def);
rt_string rt_game_scene_camera_get_str(void *scene, rt_string key, rt_string def);
void rt_game_scene_camera_set_int(void *scene, rt_string key, int64_t value);
void rt_game_scene_camera_set_str(void *scene, rt_string key, rt_string value);
void rt_game_scene_camera_remove(void *scene, rt_string key);
void *rt_game_scene_camera_keys(void *scene);
rt_string rt_game_scene_lighting_field_kind(void *scene, rt_string key);
int64_t rt_game_scene_lighting_get_int(void *scene, rt_string key, int64_t def);
void rt_game_scene_lighting_set_int(void *scene, rt_string key, int64_t value);
void rt_game_scene_lighting_remove(void *scene, rt_string key);
void *rt_game_scene_lighting_keys(void *scene);

void rt_game_scene_set_property(void *scene, rt_string key, rt_string value);
rt_string rt_game_scene_get_property(void *scene, rt_string key);
void rt_game_scene_delete_property(void *scene, rt_string key);
int64_t rt_game_scene_get_int(void *scene, rt_string key, int64_t def);
rt_string rt_game_scene_get_str(void *scene, rt_string key, rt_string def);
double rt_game_scene_get_float(void *scene, rt_string key, double def);
int8_t rt_game_scene_get_bool(void *scene, rt_string key, int8_t def);
int8_t rt_game_scene_has(void *scene, rt_string key);
/// @brief Return the exact scalar-kind token for one scene-level property.
/// @return Owned string containing null, bool, int, float, string, or empty.
rt_string rt_game_scene_property_kind(void *scene, rt_string key);
/// @brief Return scene-level property keys in lexicographic order.
/// @return Caller-owned Zanna.Collections.Seq of owned string values.
void *rt_game_scene_keys(void *scene);
/// @brief Create or replace one scene-level property with the null kind.
void rt_game_scene_set_null(void *scene, rt_string key);
void rt_game_scene_set_int(void *scene, rt_string key, int64_t value);
void rt_game_scene_set_str(void *scene, rt_string key, rt_string value);
void rt_game_scene_set_float(void *scene, rt_string key, double value);
void rt_game_scene_set_bool(void *scene, rt_string key, int8_t value);
void rt_game_scene_remove(void *scene, rt_string key);
void *rt_game_scene_asset_paths(void *scene);
void *rt_game_scene_asset_descriptors(void *scene);
void *rt_game_scene_build_tilemap(void *scene);

#ifdef __cplusplus
}
#endif
