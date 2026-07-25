//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/assets/rt_fbx_loader.h
// Purpose: Typed binary/ASCII FBX loader — extracts Mesh3D, Skeleton3D,
//   Animation3D, Material3D, morph, and scene graph data from .fbx files.
//   Supports binary v<7500/v>=7500 records plus brace-scoped ASCII documents.
//
// Key invariants:
//   - Zero external dependencies (uses existing rt_compress for zlib).
//   - Returns an FBX asset container with arrays of extracted objects.
//   - All extracted objects (meshes, skeleton, etc.) are GC-managed.
//   - Handles Blender Z-up → Y-up coordinate system conversion.
//   - One checked per-load budget covers source, typed graph, indexes, expanded
//     properties, and generated animation samples before allocation.
//
// Ownership/Lifetime:
//   - A returned FBX asset owns retained runtime references to all extracted objects.
//   - Test telemetry is thread-local and owns no asset storage.
//
// Links: plans/3d/15-fbx-loader.md, rt_skeleton3d.h
//
//===----------------------------------------------------------------------===//
/**
 * @file rt_fbx_loader.h
 * @brief Declares strict and recoverable FBX loading and asset accessors.
 *
 * The loader accepts supported binary or brace-scoped ASCII FBX documents and
 * publishes a GC-managed container of retained scene resources. Object
 * accessors return borrowed handles owned by that container; animation-name
 * accessors return runtime strings. Recoverable entry points report ordinary
 * content failures through the thread-local asset-error API.
 */
#pragma once

#include "rt_string.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Strictly load and extract an FBX file.
/// @param[in] path Runtime filesystem path.
/// @return A new GC-managed FBX asset handle on success, or `NULL` after a
/// trapped load failure.
/// @details Supports the loader's binary and ASCII variants, enforces file and
/// aggregate allocation budgets, and resolves relative textures beside `path`.
void *rt_fbx_load(rt_string path);
/// @brief Try to load an FBX asset from @p path without trapping for recoverable I/O failure.
/// @details This entry point is intended for higher-level asset APIs such as `Model3D.Load`,
/// where a missing or unsupported file should be represented as a NULL model so script code can
/// fall back cleanly. Direct `FBX.Load` remains strict and traps on the same failures. Resource
/// exhaustion and malformed binary parser invariants may still trap because they indicate runtime
/// integrity problems rather than ordinary asset absence.
/// @param[in] path Filesystem path to the `.fbx` file.
/// @return An FBX handle on success, or NULL if the file cannot be opened/read/classified.
void *rt_fbx_load_recoverable(rt_string path);
/// @brief Recoverably load an FBX temp file while resolving external textures relative to a
/// different original model path.
/// @details Packed and async FBX loads spill root bytes to a temporary file because the parser is
/// path-based. Passing @p texture_base keeps relative texture references anchored beside the
/// source asset rather than beside the temporary file.
/// @param[in] path Runtime path of the temporary FBX file to parse.
/// @param[in] texture_base Original model path used as the relative-texture
/// base; `NULL` uses `path`.
/// @return A new FBX asset handle on success, or `NULL` for a recoverable load
/// failure.
void *rt_fbx_load_recoverable_with_texture_base(rt_string path, rt_string texture_base);
/// @brief CTest hook: lower the aggregate allocation budget of the next load on this thread.
/// @details The override is consumed by one load. Passing zero clears it; values above the
///          production default are clamped and therefore cannot weaken normal resource limits.
///          This symbol is implementation-only and is not a registered runtime method.
/// @param[in] bytes One-shot byte ceiling, or zero for normal budget selection.
void rt_fbx_test_set_load_budget_bytes(uint64_t bytes);
/// @brief CTest hook: report aggregate bytes charged by the latest load on this thread.
/// @return Monotonic charged byte count, including source bytes and retained parser/extraction
///         allocations named by ADR 0173.
uint64_t rt_fbx_test_get_last_budget_used_bytes(void);
/// @brief CTest hook: report hash and adjacency probes from the latest load on this thread.
/// @return Saturating probe count used to verify near-linear numeric graph resolution.
uint64_t rt_fbx_test_get_last_lookup_probe_count(void);
/// @brief Number of meshes in the loaded FBX.
/// @param[in] fbx FBX asset handle.
/// @return The safe readable mesh count, or zero for invalid input.
int64_t rt_fbx_mesh_count(void *fbx);
/// @brief Get the mesh at @p index (NULL if out of range).
/// @param[in] fbx FBX asset handle.
/// @param[in] index Zero-based mesh index.
/// @return A borrowed validated `Mesh3D`, or `NULL`.
void *rt_fbx_get_mesh(void *fbx, int64_t index);
/// @brief Get the FBX's skeleton (NULL if it has none).
/// @param[in] fbx FBX asset handle.
/// @return The borrowed validated `Skeleton3D`, or `NULL`.
void *rt_fbx_get_skeleton(void *fbx);
/// @brief Get the root node of the imported FBX scene graph (NULL if it has none).
/// @param[in] fbx FBX asset handle.
/// @return The borrowed validated `SceneNode3D` root, or `NULL`.
void *rt_fbx_get_scene_root(void *fbx);
/// @brief Number of animation clips in the FBX.
/// @param[in] fbx FBX asset handle.
/// @return The safe readable skeletal-animation count, or zero.
int64_t rt_fbx_animation_count(void *fbx);
/// @brief Get the animation clip at @p index (NULL if out of range).
/// @param[in] fbx FBX asset handle.
/// @param[in] index Zero-based skeletal-animation index.
/// @return A borrowed validated `Animation3D`, or `NULL`.
void *rt_fbx_get_animation(void *fbx, int64_t index);
/// @brief Name of the animation clip at @p index.
/// @param[in] fbx FBX asset handle.
/// @param[in] index Zero-based skeletal-animation index.
/// @return A caller-owned runtime name string, empty for invalid input.
rt_string rt_fbx_get_animation_name(void *fbx, int64_t index);
/// @brief Number of non-skeletal object/morph animation clips in the FBX.
/// @param[in] fbx FBX asset handle.
/// @return The safe readable node-animation count, or zero.
int64_t rt_fbx_node_animation_count(void *fbx);
/// @brief Get the object/morph animation clip at @p index (NULL if out of range).
/// @param[in] fbx FBX asset handle.
/// @param[in] index Zero-based node-animation index.
/// @return A borrowed validated `NodeAnimation3D`, or `NULL`.
void *rt_fbx_get_node_animation(void *fbx, int64_t index);
/// @brief Name of the object/morph animation clip at @p index.
/// @param[in] fbx FBX asset handle.
/// @param[in] index Zero-based node-animation index.
/// @return A caller-owned runtime name string, empty for invalid input.
rt_string rt_fbx_get_node_animation_name(void *fbx, int64_t index);
/// @brief Number of cameras in the loaded FBX scene.
/// @param[in] fbx FBX asset handle.
/// @return The safe readable camera count, or zero.
int64_t rt_fbx_camera_count(void *fbx);
/// @brief Get the camera at @p index (NULL if out of range).
/// @param[in] fbx FBX asset handle.
/// @param[in] index Zero-based camera index.
/// @return A borrowed validated `Camera3D`, or `NULL`.
void *rt_fbx_get_camera(void *fbx, int64_t index);
/// @brief Number of materials in the FBX.
/// @param[in] fbx FBX asset handle.
/// @return The safe readable material count, or zero.
int64_t rt_fbx_material_count(void *fbx);
/// @brief Get the material at @p index (NULL if out of range).
/// @param[in] fbx FBX asset handle.
/// @param[in] index Zero-based material index.
/// @return A borrowed validated `Material3D`, or `NULL`.
void *rt_fbx_get_material(void *fbx, int64_t index);
/// @brief Get the morph-target set for mesh @p mesh_index (NULL if none).
/// @param[in] fbx FBX asset handle.
/// @param[in] mesh_index Zero-based mesh index in the parallel morph table.
/// @return A borrowed validated `MorphTarget3D`, or `NULL`.
void *rt_fbx_get_morph_target(void *fbx, int64_t mesh_index);

#ifdef __cplusplus
}
#endif
