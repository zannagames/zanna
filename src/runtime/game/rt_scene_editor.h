//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/game/rt_scene_editor.h
/// @file
/// @brief Declares the C ABI for editable scene documents, typed Tilemap
///        behavior, Tiled import, diagnostics, and persistence.
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

/// @brief Runtime class identifier used by SceneDocument handles.
#define RT_GAME_SCENE_CLASS_ID INT64_C(-0x510301)

/// @brief Create a scene with one empty base layer.
/// @param width Width in tile cells.
/// @param height Height in tile cells.
/// @param tile_width Tile width in pixels; nonpositive values default to 16.
/// @param tile_height Tile height in pixels; nonpositive values default to 16.
/// @return Owned SceneDocument handle, or `NULL` on an exceptional failure.
void *rt_game_scene_new(int64_t width, int64_t height, int64_t tile_width, int64_t tile_height);

/// @brief Parse scene JSON into a diagnostic-bearing document.
/// @details Schema and parse errors produce an editable fallback document;
///          inspect the diagnostic APIs to distinguish it from a valid load.
/// @param text Borrowed scene JSON text.
/// @return Owned SceneDocument handle, or `NULL` on an exceptional failure.
void *rt_game_scene_load_json(rt_string text);

/// @brief Load a SceneDocument from JSON text and return a Zanna.Result.
/// @details Returns `Ok(SceneDocument)` when the document has no retained error
///          diagnostics and `Err(message)` when JSON/schema loading records an
///          error. The compatibility rt_game_scene_load_json() API still
///          returns diagnostic documents for callers that need full records.
/// @param text Scene JSON text.
/// @return Opaque Zanna.Result object containing a SceneDocument or error string.
void *rt_game_scene_load_json_result(rt_string text);

/// @brief Load a diagnostic-bearing scene document from a filesystem path.
/// @details Missing, oversized, malformed, and schema-invalid inputs return an
///          editable fallback document carrying error diagnostics.
/// @param path Borrowed runtime string containing the scene file path.
/// @return Owned SceneDocument handle, or `NULL` on an exceptional failure.
void *rt_game_scene_load_file(rt_string path);

/// @brief Load a SceneDocument from a file and return a Zanna.Result.
/// @details Returns `Ok(SceneDocument)` when the file loads without retained
///          error diagnostics and `Err(message)` when the file is missing,
///          oversized, malformed, or schema-invalid.
/// @param path Scene file path.
/// @return Opaque Zanna.Result object containing a SceneDocument or error string.
void *rt_game_scene_load_file_result(rt_string path);

/// @brief Import a filesystem Tiled JSON/TMX map as a SceneDocument.
/// @param path Borrowed filesystem path to the root Tiled map.
/// @return Owned imported SceneDocument, or `NULL` when import fails.
void *rt_game_scene_import_tiled(rt_string path);

/// @brief Import a filesystem Tiled map with recoverable Result semantics.
/// @param path Borrowed filesystem path to the root Tiled map.
/// @return Owned Result containing a SceneDocument or an error string.
void *rt_game_scene_import_tiled_result(rt_string path);

/// @brief Import a Tiled JSON/TMX map through the asset manager.
/// @details Resolves the root map and external dependencies in asset-package
///          space instead of directly against the host filesystem.
/// @param path Borrowed asset path to the root Tiled map.
/// @return Owned imported SceneDocument, or `NULL` when import fails.
void *rt_game_scene_import_tiled_asset(rt_string path);

/// @brief Import an asset-backed Tiled map with Result semantics.
/// @param path Borrowed asset path to the root Tiled map.
/// @return Owned Result containing a SceneDocument or an error string.
void *rt_game_scene_import_tiled_asset_result(rt_string path);

/// @brief Serialize a scene document to deterministic canonical JSON.
/// @param scene Borrowed SceneDocument handle.
/// @return Owned runtime string, empty if serialization fails.
rt_string rt_game_scene_to_json(void *scene);

/// @brief Atomically save canonical scene JSON to a filesystem path.
/// @param scene Borrowed SceneDocument handle.
/// @param path Borrowed destination path.
/// @return `1` after successful replacement; otherwise `0` with a diagnostic.
int8_t rt_game_scene_save_file(void *scene, rt_string path);

/// @brief Copy the most recently recorded diagnostic message.
/// @param scene Borrowed SceneDocument handle.
/// @return Owned runtime string, empty when no diagnostic is available.
rt_string rt_game_scene_last_error(void *scene);

/// @brief Copy diagnostic messages without their structured metadata.
/// @param scene Borrowed SceneDocument handle.
/// @return Owned sequence of owned strings in diagnostic order.
void *rt_game_scene_diagnostics(void *scene);

/// @brief Copy the document's complete structured diagnostic records.
/// @details Each owned map contains code, severity, message, path, line,
///          column, and source fields.
/// @param scene Borrowed SceneDocument handle.
/// @return Owned sequence of owned maps in diagnostic order.
void *rt_game_scene_diagnostic_records(void *scene);

/// @brief Test for structural invalidity or retained error diagnostics.
/// @param scene Borrowed SceneDocument handle.
/// @return `1` when errors remain; otherwise `0`.
int8_t rt_game_scene_has_errors(void *scene);

/// @brief Remove queued diagnostics without resetting structural validity.
/// @param scene Borrowed SceneDocument handle.
void rt_game_scene_clear_diagnostics(void *scene);

/// @brief Return the scene width in tile cells.
/// @param scene Borrowed SceneDocument handle.
/// @return Positive width.
int64_t rt_game_scene_get_width(void *scene);

/// @brief Return the scene height in tile cells.
/// @param scene Borrowed SceneDocument handle.
/// @return Positive height.
int64_t rt_game_scene_get_height(void *scene);

/// @brief Return the width of one tile in pixels.
/// @param scene Borrowed SceneDocument handle.
/// @return Positive tile width.
int64_t rt_game_scene_get_tile_width(void *scene);

/// @brief Return the height of one tile in pixels.
/// @param scene Borrowed SceneDocument handle.
/// @return Positive tile height.
int64_t rt_game_scene_get_tile_height(void *scene);

/// @brief Append an empty layer within layer and aggregate-cell budgets.
/// @param scene Borrowed SceneDocument handle.
/// @param name Borrowed Tilemap-compatible layer name.
/// @return New zero-based layer index, or `-1` when rejected.
int64_t rt_game_scene_add_layer(void *scene, rt_string name);

/// @brief Return the current number of tile layers.
/// @param scene Borrowed SceneDocument handle.
/// @return Layer count.
int64_t rt_game_scene_layer_count(void *scene);

/// @brief Copy a tile layer's name.
/// @param scene Borrowed SceneDocument handle.
/// @param layer Zero-based layer index.
/// @return Owned name string, empty for an invalid index.
rt_string rt_game_scene_layer_name(void *scene, int64_t layer);

/// @brief Rename a layer when the name fits Tilemap's byte limit.
/// @param scene Borrowed SceneDocument handle.
/// @param layer Zero-based layer index.
/// @param name Borrowed replacement name.
void rt_game_scene_set_layer_name(void *scene, int64_t layer, rt_string name);

/// @brief Query a layer's visibility.
/// @param scene Borrowed SceneDocument handle.
/// @param layer Zero-based layer index.
/// @return `1` for a visible valid layer; otherwise `0`.
int8_t rt_game_scene_layer_visible(void *scene, int64_t layer);

/// @brief Set a layer's visibility.
/// @param scene Borrowed SceneDocument handle.
/// @param layer Zero-based layer index.
/// @param visible Zero to hide, nonzero to show.
void rt_game_scene_set_layer_visible(void *scene, int64_t layer, int8_t visible);

/// @brief Move a layer to another stack index.
/// @param scene Borrowed SceneDocument handle.
/// @param from Current zero-based index.
/// @param to Destination zero-based index.
void rt_game_scene_move_layer(void *scene, int64_t from, int64_t to);

/// @brief Remove a layer unless it is invalid or the sole remaining layer.
/// @param scene Borrowed SceneDocument handle.
/// @param layer Zero-based layer index.
void rt_game_scene_remove_layer(void *scene, int64_t layer);

/// @brief Read one layer cell.
/// @param scene Borrowed SceneDocument handle.
/// @param layer Zero-based layer index.
/// @param x Horizontal tile coordinate.
/// @param y Vertical tile coordinate.
/// @return Tile identifier, or `0` for an invalid cell.
int64_t rt_game_scene_get_tile(void *scene, int64_t layer, int64_t x, int64_t y);

/// @brief Store one layer cell.
/// @param scene Borrowed SceneDocument handle.
/// @param layer Zero-based layer index.
/// @param x Horizontal tile coordinate.
/// @param y Vertical tile coordinate.
/// @param tile Tile identifier to store.
void rt_game_scene_set_tile(void *scene, int64_t layer, int64_t x, int64_t y, int64_t tile);

/// @brief Fill the clipped in-bounds portion of a rectangular tile region.
/// @param scene Borrowed SceneDocument handle.
/// @param layer Zero-based layer index.
/// @param x Left tile coordinate.
/// @param y Top tile coordinate.
/// @param w Rectangle width in cells.
/// @param h Rectangle height in cells.
/// @param tile Tile identifier to store.
void rt_game_scene_fill_tiles(
    void *scene, int64_t layer, int64_t x, int64_t y, int64_t w, int64_t h, int64_t tile);
/// @brief Replace one four-connected region and return its exact changed-cell count.
/// @details Work storage is fully allocated before mutation, so allocation
///          failure and invalid/already-equal requests return zero atomically.
/// @param scene Borrowed SceneDocument handle.
/// @param layer Zero-based layer index.
/// @param x Horizontal seed coordinate.
/// @param y Vertical seed coordinate.
/// @param tile Replacement tile identifier.
/// @return Number of changed cells.
int64_t rt_game_scene_flood_fill_tiles(
    void *scene, int64_t layer, int64_t x, int64_t y, int64_t tile);

/// @brief Associate an asset path with a layer.
/// @param scene Borrowed SceneDocument handle.
/// @param layer Zero-based layer index.
/// @param asset_path Borrowed asset path copied into the document.
void rt_game_scene_set_layer_asset(void *scene, int64_t layer, rt_string asset_path);

/// @brief Copy the asset path associated with a layer.
/// @param scene Borrowed SceneDocument handle.
/// @param layer Zero-based layer index.
/// @return Owned path string, empty for an invalid layer.
rt_string rt_game_scene_layer_asset(void *scene, int64_t layer);

/// @brief Append a root object at an absolute scene position.
/// @param scene Borrowed SceneDocument handle.
/// @param type Borrowed object type.
/// @param id Borrowed object identifier.
/// @param x Absolute horizontal coordinate.
/// @param y Absolute vertical coordinate.
/// @return New zero-based object index, or `-1` when rejected.
int64_t rt_game_scene_add_object(void *scene, rt_string type, rt_string id, int64_t x, int64_t y);

/// @brief Return the number of objects in document order.
/// @param scene Borrowed SceneDocument handle.
/// @return Object count.
int64_t rt_game_scene_object_count(void *scene);

/// @brief Remove an object and repair all organizational parent indices.
/// @param scene Borrowed SceneDocument handle.
/// @param index Zero-based object index.
void rt_game_scene_remove_object(void *scene, int64_t index);

/// @brief Copy an object's type string.
/// @param scene Borrowed SceneDocument handle.
/// @param index Zero-based object index.
/// @return Owned type string, empty for an invalid index.
rt_string rt_game_scene_object_type(void *scene, int64_t index);

/// @brief Copy an object's identifier string.
/// @param scene Borrowed SceneDocument handle.
/// @param index Zero-based object index.
/// @return Owned identifier string, empty for an invalid index.
rt_string rt_game_scene_object_id(void *scene, int64_t index);

/// @brief Return an object's absolute horizontal coordinate.
/// @param scene Borrowed SceneDocument handle.
/// @param index Zero-based object index.
/// @return Stored x coordinate, or `0` for an invalid index.
int64_t rt_game_scene_object_x(void *scene, int64_t index);

/// @brief Return an object's absolute vertical coordinate.
/// @param scene Borrowed SceneDocument handle.
/// @param index Zero-based object index.
/// @return Stored y coordinate, or `0` for an invalid index.
int64_t rt_game_scene_object_y(void *scene, int64_t index);

/// @brief Return an object's organizational parent index, or -1 for a root.
/// @details Invalid object indices also return -1. Positions remain absolute
///          scene-space coordinates and do not inherit from this parent.
/// @param scene Borrowed SceneDocument handle.
/// @param index Zero-based object index.
/// @return Parent index, or `-1` for a root or invalid object.
int64_t rt_game_scene_object_parent(void *scene, int64_t index);

/// @brief Set an object's organizational parent when the hierarchy stays valid.
/// @details Accepts -1 for a root. Invalid indices, self-parenting, and cycles
///          return false without mutation. Parenting also returns false when
///          all 256 serialized property slots are already public properties.
/// @param scene Borrowed SceneDocument handle.
/// @param index Zero-based child object index.
/// @param parent Parent object index, or `-1` to make a root.
/// @return `1` when applied; otherwise `0`.
int8_t rt_game_scene_try_set_object_parent(void *scene, int64_t index, int64_t parent);

/// @brief Replace an object's type and identifier.
/// @param scene Borrowed SceneDocument handle.
/// @param index Zero-based object index.
/// @param type Borrowed replacement type.
/// @param id Borrowed replacement identifier.
void rt_game_scene_set_object_metadata(void *scene, int64_t index, rt_string type, rt_string id);

/// @brief Replace an object's absolute scene position.
/// @param scene Borrowed SceneDocument handle.
/// @param index Zero-based object index.
/// @param x Replacement horizontal coordinate.
/// @param y Replacement vertical coordinate.
void rt_game_scene_set_object_position(void *scene, int64_t index, int64_t x, int64_t y);

/// @brief Duplicate an object immediately after its source.
/// @details Copies metadata, position, hierarchy, and scalar properties while
///          remapping all parent indices around the insertion.
/// @param scene Borrowed SceneDocument handle.
/// @param index Zero-based source index.
/// @param id Borrowed identifier for the duplicate.
/// @return Inserted object index, or `-1` when rejected.
int64_t rt_game_scene_duplicate_object(void *scene, int64_t index, rt_string id);

/// @brief Store a legacy string-valued object property.
/// @param scene Borrowed SceneDocument handle.
/// @param index Zero-based object index.
/// @param key Borrowed property key.
/// @param value Borrowed string value.
void rt_game_scene_set_object_property(void *scene, int64_t index, rt_string key, rt_string value);

/// @brief Read any object scalar as canonical text.
/// @param scene Borrowed SceneDocument handle.
/// @param index Zero-based object index.
/// @param key Borrowed property key.
/// @return Owned textual value, empty when absent or invalid.
rt_string rt_game_scene_get_object_property(void *scene, int64_t index, rt_string key);

/// @brief Remove a public object property through the legacy interface.
/// @param scene Borrowed SceneDocument handle.
/// @param index Zero-based object index.
/// @param key Borrowed property key.
void rt_game_scene_delete_object_property(void *scene, int64_t index, rt_string key);

/// @brief Read an exact integer object property.
/// @param scene Borrowed SceneDocument handle.
/// @param index Zero-based object index.
/// @param key Borrowed property key.
/// @param def Fallback for absence, mismatch, or invalid input.
/// @return Stored integer or @p def.
int64_t rt_game_scene_object_get_int(void *scene, int64_t index, rt_string key, int64_t def);

/// @brief Read an exact string object property.
/// @param scene Borrowed SceneDocument handle.
/// @param index Zero-based object index.
/// @param key Borrowed property key.
/// @param def Borrowed fallback string.
/// @return Owned stored or fallback string.
rt_string rt_game_scene_object_get_str(void *scene, int64_t index, rt_string key, rt_string def);

/// @brief Read a floating-point object property, widening integers.
/// @param scene Borrowed SceneDocument handle.
/// @param index Zero-based object index.
/// @param key Borrowed property key.
/// @param def Fallback for absence, mismatch, or invalid input.
/// @return Stored numeric value or @p def.
double rt_game_scene_object_get_float(void *scene, int64_t index, rt_string key, double def);

/// @brief Read an exact Boolean object property.
/// @param scene Borrowed SceneDocument handle.
/// @param index Zero-based object index.
/// @param key Borrowed property key.
/// @param def Fallback for absence, mismatch, or invalid input.
/// @return Stored `0`/`1`, or @p def unchanged.
int8_t rt_game_scene_object_get_bool(void *scene, int64_t index, rt_string key, int8_t def);

/// @brief Test whether an object property exists.
/// @param scene Borrowed SceneDocument handle.
/// @param index Zero-based object index.
/// @param key Borrowed property key.
/// @return `1` when present; otherwise `0`.
int8_t rt_game_scene_object_has(void *scene, int64_t index, rt_string key);

/// @brief Copy the exact scalar-kind token of an object property.
/// @param scene Borrowed SceneDocument handle.
/// @param index Zero-based object index.
/// @param key Borrowed property key.
/// @return Owned `null`, `bool`, `int`, `float`, or `string` token; empty when
///         absent or invalid.
rt_string rt_game_scene_object_property_kind(void *scene, int64_t index, rt_string key);

/// @brief Enumerate an object's public property keys in lexical order.
/// @param scene Borrowed SceneDocument handle.
/// @param index Zero-based object index.
/// @return Owned sequence of owned key strings.
void *rt_game_scene_object_keys(void *scene, int64_t index);

/// @brief Create or replace an object property with explicit null.
/// @param scene Borrowed SceneDocument handle.
/// @param index Zero-based object index.
/// @param key Borrowed non-reserved property key.
void rt_game_scene_object_set_null(void *scene, int64_t index, rt_string key);

/// @brief Create or replace an integer object property.
/// @param scene Borrowed SceneDocument handle.
/// @param index Zero-based object index.
/// @param key Borrowed non-reserved property key.
/// @param value Integer value.
void rt_game_scene_object_set_int(void *scene, int64_t index, rt_string key, int64_t value);

/// @brief Create or replace a string object property.
/// @param scene Borrowed SceneDocument handle.
/// @param index Zero-based object index.
/// @param key Borrowed non-reserved property key.
/// @param value Borrowed value subject to the scalar string-size limit.
void rt_game_scene_object_set_str(void *scene, int64_t index, rt_string key, rt_string value);

/// @brief Create or replace a finite floating-point object property.
/// @param scene Borrowed SceneDocument handle.
/// @param index Zero-based object index.
/// @param key Borrowed non-reserved property key.
/// @param value Finite floating-point value.
void rt_game_scene_object_set_float(void *scene, int64_t index, rt_string key, double value);

/// @brief Create or replace a Boolean object property.
/// @param scene Borrowed SceneDocument handle.
/// @param index Zero-based object index.
/// @param key Borrowed non-reserved property key.
/// @param value Zero for false, nonzero for true.
void rt_game_scene_object_set_bool(void *scene, int64_t index, rt_string key, int8_t value);

/// @brief Remove a public object property if present.
/// @param scene Borrowed SceneDocument handle.
/// @param index Zero-based object index.
/// @param key Borrowed non-reserved property key.
void rt_game_scene_object_remove(void *scene, int64_t index, rt_string key);

/// @brief Count objects with an exact type match.
/// @param scene Borrowed SceneDocument handle.
/// @param type Borrowed case-sensitive type.
/// @return Number of matches.
int64_t rt_game_scene_count_of_type(void *scene, rt_string type);

/// @brief Return the nth object of a type in document order.
/// @param scene Borrowed SceneDocument handle.
/// @param type Borrowed case-sensitive type.
/// @param n Zero-based match ordinal.
/// @return Object index, or `-1` when absent.
int64_t rt_game_scene_object_of_type(void *scene, rt_string type, int64_t n);

/// @brief Find the first object with an exact identifier match.
/// @param scene Borrowed SceneDocument handle.
/// @param id Borrowed case-sensitive identifier.
/// @return Object index, or `-1` when absent.
int64_t rt_game_scene_find_object(void *scene, rt_string id);

/// @brief Find an object by id and return its index as an Option.
/// @details Returns `SomeI64(index)` when an object with @p id exists and
///          `None` when absent, avoiding the legacy `-1` sentinel.
/// @param scene SceneDocument handle.
/// @param id Object id to search for.
/// @return Opaque Zanna.Option containing the object index, or None.
void *rt_game_scene_find_object_option(void *scene, rt_string id);

/// @brief Move an object while preserving logical parent references.
/// @param scene Borrowed SceneDocument handle.
/// @param from Current zero-based object index.
/// @param to Destination zero-based object index.
void rt_game_scene_move_object(void *scene, int64_t from, int64_t to);

/// @brief Return one tile's authored collision kind (0 none, 1 solid, 2 one-way-up).
/// @param scene Borrowed SceneDocument handle.
/// @param tile Tile identifier to inspect.
/// @return Authored collision kind, defaulting to none.
int64_t rt_game_scene_tile_collision(void *scene, int64_t tile);

/// @brief Author one tile's collision kind; kind 0 removes the record.
/// @details Tile IDs outside 1..4095 and kinds outside 0..2 are edit-time
///          warning no-ops, matching Tilemap's behavior-table addressability.
/// @param scene Borrowed SceneDocument handle.
/// @param tile Editable tile identifier.
/// @param kind Collision kind in the inclusive range 0..2.
void rt_game_scene_set_tile_collision(void *scene, int64_t tile, int64_t kind);

/// @brief Return ascending tile IDs whose authored collision kind is not none.
/// @param scene Borrowed SceneDocument handle.
/// @return Owned sequence of boxed integer tile IDs.
void *rt_game_scene_collision_tiles(void *scene);

/// @brief Return the configured Tilemap collision-layer slot.
/// @param scene Borrowed SceneDocument handle.
/// @return Layer slot in the range 0..15.
int64_t rt_game_scene_collision_layer(void *scene);

/// @brief Configure the Tilemap collision-layer slot.
/// @param scene Borrowed SceneDocument handle.
/// @param layer Layer slot in the inclusive range 0..15.
void rt_game_scene_set_collision_layer(void *scene, int64_t layer);

/// @brief Return "int", "bool", or "" for one authored tile property.
/// @param scene Borrowed SceneDocument handle.
/// @param tile Tile identifier to inspect.
/// @param key Borrowed property key.
/// @return Owned scalar-kind token or empty string.
rt_string rt_game_scene_tile_property_kind(void *scene, int64_t tile, rt_string key);

/// @brief Read an exact integer tile property.
/// @param scene Borrowed SceneDocument handle.
/// @param tile Tile identifier to inspect.
/// @param key Borrowed property key.
/// @param def Fallback for absence or type mismatch.
/// @return Stored integer or @p def.
int64_t rt_game_scene_tile_property_int(void *scene, int64_t tile, rt_string key, int64_t def);

/// @brief Read an exact Boolean tile property.
/// @param scene Borrowed SceneDocument handle.
/// @param tile Tile identifier to inspect.
/// @param key Borrowed property key.
/// @param def Fallback for absence or type mismatch.
/// @return Stored `0`/`1`, or @p def unchanged.
int8_t rt_game_scene_tile_property_bool(void *scene, int64_t tile, rt_string key, int8_t def);

/// @brief Create or replace an integer property for a behavior tile.
/// @param scene Borrowed SceneDocument handle.
/// @param tile Editable tile identifier in the range 1..4095.
/// @param key Borrowed property key.
/// @param value Integer value.
void rt_game_scene_set_tile_property_int(void *scene, int64_t tile, rt_string key, int64_t value);

/// @brief Create or replace a Boolean property for a behavior tile.
/// @param scene Borrowed SceneDocument handle.
/// @param tile Editable tile identifier in the range 1..4095.
/// @param key Borrowed property key.
/// @param value Zero for false, nonzero for true.
void rt_game_scene_set_tile_property_bool(void *scene, int64_t tile, rt_string key, int8_t value);

/// @brief Remove a typed property and prune an empty per-tile record.
/// @param scene Borrowed SceneDocument handle.
/// @param tile Tile identifier to edit.
/// @param key Borrowed property key.
void rt_game_scene_remove_tile_property(void *scene, int64_t tile, rt_string key);

/// @brief Return one tile's authored property keys in lexicographic order.
/// @param scene Borrowed SceneDocument handle.
/// @param tile Tile identifier to inspect.
/// @return Owned sequence of owned key strings.
void *rt_game_scene_tile_property_keys(void *scene, int64_t tile);

/// @brief Return ascending tile IDs carrying at least one typed property.
/// @param scene Borrowed SceneDocument handle.
/// @return Owned sequence of boxed integer tile IDs.
void *rt_game_scene_tile_property_tiles(void *scene);

/// @brief Author one tile animation from equal-length frame/duration sequences.
/// @details 1..4096 frames, every duration positive; invalid input is an
///          edit-time warning no-op that leaves any existing rule unchanged.
/// @param scene Borrowed SceneDocument handle.
/// @param base_tile Editable base tile identifier in the range 1..4095.
/// @param frames Borrowed sequence of integer frame tile IDs.
/// @param durations_ms Borrowed sequence of positive integer durations.
void rt_game_scene_set_tile_anim(void *scene, int64_t base_tile, void *frames, void *durations_ms);

/// @brief Copy an animation rule's frame tile identifiers.
/// @param scene Borrowed SceneDocument handle.
/// @param base_tile Base tile identifier.
/// @return Owned sequence of boxed integers in playback order.
void *rt_game_scene_tile_anim_frames(void *scene, int64_t base_tile);

/// @brief Copy an animation rule's frame durations.
/// @param scene Borrowed SceneDocument handle.
/// @param base_tile Base tile identifier.
/// @return Owned sequence of boxed millisecond durations in playback order.
void *rt_game_scene_tile_anim_durations(void *scene, int64_t base_tile);

/// @brief Remove an authored tile animation rule.
/// @param scene Borrowed SceneDocument handle.
/// @param base_tile Base tile identifier.
void rt_game_scene_remove_tile_anim(void *scene, int64_t base_tile);

/// @brief Return ascending base tile IDs carrying an authored animation.
/// @param scene Borrowed SceneDocument handle.
/// @return Owned sequence of boxed integer tile IDs.
void *rt_game_scene_tile_anim_bases(void *scene);

/// @brief Author one autotile rule from exactly 16 variant tile IDs.
/// @param scene Borrowed SceneDocument handle.
/// @param base_tile Editable base tile identifier in the range 1..4095.
/// @param variants Borrowed 16-element integer sequence in neighbor-mask order.
void rt_game_scene_set_autotile_rule(void *scene, int64_t base_tile, void *variants);

/// @brief Copy an autotile rule's mask-indexed variants.
/// @param scene Borrowed SceneDocument handle.
/// @param base_tile Base tile identifier.
/// @return Owned 16-element sequence of boxed variant tile IDs.
void *rt_game_scene_autotile_variants(void *scene, int64_t base_tile);

/// @brief Remove an authored autotile rule.
/// @param scene Borrowed SceneDocument handle.
/// @param base_tile Base tile identifier.
void rt_game_scene_remove_autotile_rule(void *scene, int64_t base_tile);

/// @brief Return ascending base tile IDs carrying an authored autotile rule.
/// @param scene Borrowed SceneDocument handle.
/// @return Owned sequence of boxed integer tile IDs.
void *rt_game_scene_autotile_bases(void *scene);

/// @brief Return "int", "string", or "" for one camera-section field.
/// @param scene Borrowed SceneDocument handle.
/// @param key Borrowed camera field name.
/// @return Owned scalar-kind token or empty string.
rt_string rt_game_scene_camera_field_kind(void *scene, rt_string key);

/// @brief Read an exact integer camera field.
/// @param scene Borrowed SceneDocument handle.
/// @param key Borrowed camera field name.
/// @param def Fallback for absence or type mismatch.
/// @return Stored integer or @p def.
int64_t rt_game_scene_camera_get_int(void *scene, rt_string key, int64_t def);

/// @brief Read an exact string camera field.
/// @param scene Borrowed SceneDocument handle.
/// @param key Borrowed camera field name.
/// @param def Borrowed fallback string.
/// @return Owned stored or fallback string.
rt_string rt_game_scene_camera_get_str(void *scene, rt_string key, rt_string def);

/// @brief Store an integer camera field with known-key range validation.
/// @param scene Borrowed SceneDocument handle.
/// @param key Borrowed camera field name.
/// @param value Integer value.
void rt_game_scene_camera_set_int(void *scene, rt_string key, int64_t value);

/// @brief Store a string camera field with known-key value validation.
/// @param scene Borrowed SceneDocument handle.
/// @param key Borrowed camera field name.
/// @param value Borrowed string value.
void rt_game_scene_camera_set_str(void *scene, rt_string key, rt_string value);

/// @brief Remove a typed camera field.
/// @param scene Borrowed SceneDocument handle.
/// @param key Borrowed camera field name.
void rt_game_scene_camera_remove(void *scene, rt_string key);

/// @brief Enumerate typed camera field names in lexical order.
/// @param scene Borrowed SceneDocument handle.
/// @return Owned sequence of owned field-name strings.
void *rt_game_scene_camera_keys(void *scene);

/// @brief Return the scalar-kind token for one lighting-section field.
/// @param scene Borrowed SceneDocument handle.
/// @param key Borrowed lighting field name.
/// @return Owned scalar-kind token or empty string.
rt_string rt_game_scene_lighting_field_kind(void *scene, rt_string key);

/// @brief Read an exact integer lighting field.
/// @param scene Borrowed SceneDocument handle.
/// @param key Borrowed lighting field name.
/// @param def Fallback for absence or type mismatch.
/// @return Stored integer or @p def.
int64_t rt_game_scene_lighting_get_int(void *scene, rt_string key, int64_t def);

/// @brief Store an integer lighting field with known-key range validation.
/// @param scene Borrowed SceneDocument handle.
/// @param key Borrowed lighting field name.
/// @param value Integer value.
void rt_game_scene_lighting_set_int(void *scene, rt_string key, int64_t value);

/// @brief Remove a typed lighting field.
/// @param scene Borrowed SceneDocument handle.
/// @param key Borrowed lighting field name.
void rt_game_scene_lighting_remove(void *scene, rt_string key);

/// @brief Enumerate typed lighting field names in lexical order.
/// @param scene Borrowed SceneDocument handle.
/// @return Owned sequence of owned field-name strings.
void *rt_game_scene_lighting_keys(void *scene);

/// @brief Store a legacy string-valued scene property.
/// @param scene Borrowed SceneDocument handle.
/// @param key Borrowed property key.
/// @param value Borrowed string value.
void rt_game_scene_set_property(void *scene, rt_string key, rt_string value);

/// @brief Read any scene scalar as canonical text.
/// @param scene Borrowed SceneDocument handle.
/// @param key Borrowed property key.
/// @return Owned textual value, empty when absent.
rt_string rt_game_scene_get_property(void *scene, rt_string key);

/// @brief Remove a scene property through the legacy interface.
/// @param scene Borrowed SceneDocument handle.
/// @param key Borrowed property key.
void rt_game_scene_delete_property(void *scene, rt_string key);

/// @brief Read an exact integer scene property.
/// @param scene Borrowed SceneDocument handle.
/// @param key Borrowed property key.
/// @param def Fallback for absence or type mismatch.
/// @return Stored integer or @p def.
int64_t rt_game_scene_get_int(void *scene, rt_string key, int64_t def);

/// @brief Read an exact string scene property.
/// @param scene Borrowed SceneDocument handle.
/// @param key Borrowed property key.
/// @param def Borrowed fallback string.
/// @return Owned stored or fallback string.
rt_string rt_game_scene_get_str(void *scene, rt_string key, rt_string def);

/// @brief Read a floating-point scene property, widening integers.
/// @param scene Borrowed SceneDocument handle.
/// @param key Borrowed property key.
/// @param def Fallback for absence or type mismatch.
/// @return Stored numeric value or @p def.
double rt_game_scene_get_float(void *scene, rt_string key, double def);

/// @brief Read an exact Boolean scene property.
/// @param scene Borrowed SceneDocument handle.
/// @param key Borrowed property key.
/// @param def Fallback for absence or type mismatch.
/// @return Stored `0`/`1`, or @p def unchanged.
int8_t rt_game_scene_get_bool(void *scene, rt_string key, int8_t def);

/// @brief Test whether a scene property exists.
/// @param scene Borrowed SceneDocument handle.
/// @param key Borrowed property key.
/// @return `1` when present; otherwise `0`.
int8_t rt_game_scene_has(void *scene, rt_string key);

/// @brief Return the exact scalar-kind token for one scene-level property.
/// @param scene Borrowed SceneDocument handle.
/// @param key Borrowed property key.
/// @return Owned string containing null, bool, int, float, string, or empty.
rt_string rt_game_scene_property_kind(void *scene, rt_string key);

/// @brief Return scene-level property keys in lexicographic order.
/// @param scene Borrowed SceneDocument handle.
/// @return Caller-owned Zanna.Collections.Seq of owned string values.
void *rt_game_scene_keys(void *scene);

/// @brief Create or replace one scene-level property with the null kind.
/// @param scene Borrowed SceneDocument handle.
/// @param key Borrowed property key.
void rt_game_scene_set_null(void *scene, rt_string key);

/// @brief Create or replace an integer scene property.
/// @param scene Borrowed SceneDocument handle.
/// @param key Borrowed property key.
/// @param value Integer value.
void rt_game_scene_set_int(void *scene, rt_string key, int64_t value);

/// @brief Create or replace a string scene property.
/// @param scene Borrowed SceneDocument handle.
/// @param key Borrowed property key.
/// @param value Borrowed value subject to the scalar string-size limit.
void rt_game_scene_set_str(void *scene, rt_string key, rt_string value);

/// @brief Create or replace a finite floating-point scene property.
/// @param scene Borrowed SceneDocument handle.
/// @param key Borrowed property key.
/// @param value Finite floating-point value.
void rt_game_scene_set_float(void *scene, rt_string key, double value);

/// @brief Create or replace a Boolean scene property.
/// @param scene Borrowed SceneDocument handle.
/// @param key Borrowed property key.
/// @param value Zero for false, nonzero for true.
void rt_game_scene_set_bool(void *scene, rt_string key, int8_t value);

/// @brief Remove a typed scene property if present.
/// @param scene Borrowed SceneDocument handle.
/// @param key Borrowed property key.
void rt_game_scene_remove(void *scene, rt_string key);

/// @brief Enumerate unique asset paths referenced by the document.
/// @details Paths are discovered from layer metadata and recognized preserved
///          JSON contexts, deduplicated, and returned in lexical order.
/// @param scene Borrowed SceneDocument handle.
/// @return Owned sequence of owned path strings.
void *rt_game_scene_asset_paths(void *scene);

/// @brief Enumerate structured asset-reference descriptors.
/// @details Each descriptor map identifies a path plus its owner, section, and
///          source context where available.
/// @param scene Borrowed SceneDocument handle.
/// @return Owned sequence of owned descriptor maps.
void *rt_game_scene_asset_descriptors(void *scene);

/// @brief Materialize the document as an independent runtime Tilemap.
/// @details Copies layers and cells, then applies typed and compatible
///          preserved behavior sections.
/// @param scene Borrowed SceneDocument handle.
/// @return Owned Tilemap handle, or `NULL` on construction failure.
void *rt_game_scene_build_tilemap(void *scene);

#ifdef __cplusplus
}
#endif
