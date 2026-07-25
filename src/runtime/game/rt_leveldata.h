//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/game/rt_leveldata.h
/// @file
/// @brief Declares the JSON-backed TileMap and spawn-metadata level loader.
//
// Purpose: JSON-based level loader with tilemap + entity spawn objects.
//
// Ownership/Lifetime:
//   - rt_leveldata_load returns a runtime-object reference whose finalizer owns
//     the created TileMap.
//   - The TileMap getter borrows; string getters return runtime-string copies.
//
//===----------------------------------------------------------------------===//

#pragma once

#include <stdint.h>

#include "rt_string.h"

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Runtime class ID used to validate opaque LevelData handles.
#define RT_LEVELDATA_CLASS_ID INT64_C(-0x510214)

/// @brief Load a level from a JSON file.
/// @param path Runtime string containing the path.
/// @return New LevelData reference, or `NULL` for null/missing/empty input,
///         parse failure, invalid dimensions, overflow, or allocation failure.
/// @details Missing files soft-fail through an existence precheck. Nonpositive
///          tile dimensions default to 32. Tile layers overwrite one TileMap in
///          input order; at most 512 object records are copied.
void *rt_leveldata_load(void *path);

/// @brief Get the tilemap from a loaded level.
/// @param level LevelData to query.
/// @return Borrowed TileMap owned by @p level, or `NULL` for null/invalid.
/// @details A non-null invalid handle raises a runtime trap.
void *rt_leveldata_get_tilemap(void *level);

/// @brief Get the number of objects in the level.
/// @param level LevelData to query.
/// @return Count in `[0, 512]`, or zero for null/invalid.
int64_t rt_leveldata_object_count(void *level);

/// @brief Get object type string at index.
/// @param level LevelData to query.
/// @param index Zero-based object index.
/// @return Caller-owned copy, immortal empty singleton for invalid/empty, or
///         `NULL` on allocation failure.
rt_string rt_leveldata_object_type(void *level, int64_t index);

/// @brief Get object id string at index.
/// @param level LevelData to query.
/// @param index Zero-based object index.
/// @return Caller-owned copy, immortal empty singleton for invalid/empty, or
///         `NULL` on allocation failure.
rt_string rt_leveldata_object_id(void *level, int64_t index);

/// @brief Get object X position at index.
/// @param level LevelData to query.
/// @param index Zero-based object index.
/// @return Stored X, or zero for invalid input/index.
int64_t rt_leveldata_object_x(void *level, int64_t index);

/// @brief Get object Y position at index.
/// @param level LevelData to query.
/// @param index Zero-based object index.
/// @return Stored Y, or zero for invalid input/index.
int64_t rt_leveldata_object_y(void *level, int64_t index);

/// @brief Get player start X.
/// @param level LevelData to query.
/// @return Stored property, defaulting to zero.
int64_t rt_leveldata_player_start_x(void *level);

/// @brief Get player start Y.
/// @param level LevelData to query.
/// @return Stored property, defaulting to zero.
int64_t rt_leveldata_player_start_y(void *level);

/// @brief Get level theme string.
/// @param level LevelData to query.
/// @return Caller-owned theme copy, immortal empty singleton when absent/
///         invalid, or `NULL` on allocation failure.
/// @details All accessors trap on a non-null wrong LevelData class.
rt_string rt_leveldata_get_theme(void *level);

#ifdef __cplusplus
}
#endif
