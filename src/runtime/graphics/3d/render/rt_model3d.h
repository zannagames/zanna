//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/render/rt_model3d.h
// Purpose: High-level imported 3D asset container for meshes, materials,
//   skeletons, animations, and an instantiable template node hierarchy.
//
// Key invariants:
//   - Model3D.Load routes by file extension: .vscn, .fbx, .gltf, .glb, .obj, .stl.
//   - Imported resources are shared, except morph-enabled meshes are cloned per
//     instantiation so mutable blend-shape weights do not leak across instances.
//   - Typed SceneNode metadata is deep-copied for every mutable instance.
//   - InstantiateScene() creates a fresh Scene3D and attaches cloned top-level
//     nodes below the scene root.
//
// Ownership/Lifetime:
//   - SceneAsset retains imported resources and immutable scene templates until finalization.
//   - Instantiation returns independent scene/node graphs with shared immutable resources and
//     instance-local mutable morph state.
//
// Links: rt_scene3d.h, rt_fbx_loader.h, rt_gltf.h,
//   docs/adr/0159-typed-scenenode-metadata-and-vscn-v6.md
//
//===----------------------------------------------------------------------===//

#pragma once

#include "rt_string.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Load a 3D model asset (mesh/material/skeleton/animation graph) from
///        a file. @return New Model3D handle, or NULL on failure.
void *rt_model3d_load(rt_string path);
/// @brief Load a 3D model asset from a file as `Result.Ok(SceneAsset)` or `Result.Err(String)`.
/// @details This side-channel-free companion to @ref rt_model3d_load preserves the same loader
/// behavior while returning recoverable failure diagnostics directly in the Result.
void *rt_model3d_load_result(rt_string path);
/// @brief Load VSCN text as `Result.Ok(SceneAsset)` or `Result.Err(String)` without reading
///        the document from disk (ADR 0190). @p path must carry a .scene3d/.vscn extension
///        and names the relative prefab base directory plus diagnostics.
void *rt_model3d_load_text_result(rt_string path, rt_string text);
/// @brief Load with explicit import options: `force_tangents` generates tangents for
///        every UV0-mapped glTF primitive even without a normal map bound at load.
void *rt_model3d_load_with_options(rt_string path, int8_t force_tangents);
/// @brief Options-string variant: comma-separated flags "forceTangents",
///   "eightInfluences", "compressAnimations"; unknown flags are ignored.
void *rt_model3d_load_with_options_ex(rt_string path, rt_string options);
/// @brief Options-aware Result variant of @ref rt_model3d_load_with_options.
void *rt_model3d_load_result_with_options(rt_string path, int8_t force_tangents);
/// @brief Load a 3D model asset through the runtime asset manager.
/// @details glTF/GLB assets resolve external dependencies relative to the model
///          asset and search mounted/embedded packages before dev filesystem files.
void *rt_model3d_load_asset(rt_string path);
/// @brief Load a 3D model asset through the asset manager as `Result.Ok` or `Result.Err`.
/// @details Failure diagnostics are returned in the Result instead of requiring
/// `AssetDiagnostics3D.LastLoadError`.
void *rt_model3d_load_asset_result(rt_string path);
/// @brief Save the complete imported scene asset as VSCN v5, or v6 when node metadata exists.
/// @details Unlike SceneGraph.Save, this preserves every immutable scene, camera association,
/// animation class, material variant, morph target, enumerable shared resource, and exact
/// supported texture source container. Typed SceneNode metadata is preserved exactly.
/// @return One after atomic publication, otherwise zero.
int64_t rt_model3d_save(void *obj, rt_string path);
/// @brief Internal async path: build a glTF/GLB model from preloaded root bytes.
/// @details Takes ownership of @p preloaded_data; callers must not reuse it.
void *rt_model3d_load_preloaded_gltf(rt_string path,
                                     uint8_t *preloaded_data,
                                     size_t preloaded_size,
                                     int load_assets);
/// @brief Internal async path: build an FBX model from preloaded root bytes.
/// @details Takes ownership of @p preloaded_data; callers must not reuse it.
void *rt_model3d_load_preloaded_fbx(rt_string path,
                                    uint8_t *preloaded_data,
                                    size_t preloaded_size,
                                    int load_assets);
struct rt_gltf_preload_bundle;
/// @brief Internal async path: load a glTF/GLB Model3D from staged root/dependency bytes.
/// @details Takes ownership of @p bundle.
void *rt_model3d_load_preloaded_gltf_bundle(rt_string path,
                                            struct rt_gltf_preload_bundle *bundle,
                                            int load_assets);

/// @brief Number of meshes contained in the model.
int64_t rt_model3d_get_mesh_count(void *obj);
/// @brief Number of materials contained in the model.
int64_t rt_model3d_get_material_count(void *obj);
/// @brief Number of skeletons contained in the model.
int64_t rt_model3d_get_skeleton_count(void *obj);
/// @brief Number of animations contained in the model.
int64_t rt_model3d_get_animation_count(void *obj);
/// @brief Number of node/object/morph/camera animations contained in the model.
/// @details NodeAnimation3D clips are imported from glTF node animation channels and are
/// separate from skeletal Animation3D clips.
int64_t rt_model3d_get_node_animation_count(void *obj);
/// @brief Number of enumerable meshes carrying an attached MorphTarget3D container.
int64_t rt_model3d_get_morph_target_count(void *obj);
/// @brief Total number of morph shapes across every enumerable mesh.
int64_t rt_model3d_get_morph_shape_count(void *obj);
/// @brief Number of scene-graph nodes contained in the model.
int64_t rt_model3d_get_node_count(void *obj);
/// @brief Number of immutable scenes contained in the model.
int64_t rt_model3d_get_scene_count(void *obj);
/// @brief Number of imported cameras in @p scene_index.
int64_t rt_model3d_get_camera_count(void *obj, int64_t scene_index);

/// @brief Get the mesh at @p index (NULL if out of range).
void *rt_model3d_get_mesh(void *obj, int64_t index);
/// @brief Get the material at @p index (NULL if out of range).
void *rt_model3d_get_material(void *obj, int64_t index);
/// @brief Get the skeleton at @p index (NULL if out of range).
void *rt_model3d_get_skeleton(void *obj, int64_t index);
/// @brief Get the animation at @p index (NULL if out of range).
void *rt_model3d_get_animation(void *obj, int64_t index);
/// @brief Get the node animation at @p index (NULL if out of range).
void *rt_model3d_get_node_animation(void *obj, int64_t index);
/// @brief Get the MorphTarget3D attached to mesh @p mesh_index, or NULL.
void *rt_model3d_get_morph_target(void *obj, int64_t mesh_index);
/// @brief Get the node animation name at @p index (empty string if out of range).
rt_string rt_model3d_get_node_animation_name(void *obj, int64_t index);
/// @brief Get the imported camera at @p index in @p scene_index (NULL if out of range).
void *rt_model3d_get_camera(void *obj, int64_t scene_index, int64_t index);
/// @brief Get the immutable scene name at @p index (empty string if out of range).
rt_string rt_model3d_get_scene_name(void *obj, int64_t index);

/// @brief Number of KHR_materials_variants names imported with the asset (0 when absent).
int64_t rt_model3d_get_variant_count(void *obj);
/// @brief Get the material-variant name at @p index (empty string if out of range).
rt_string rt_model3d_get_variant_name(void *obj, int64_t index);
/// @brief Apply material variant @p variant_index to every mapped node under @p target.
/// @details @p target is a SceneNode3D (e.g. an Instantiate() result) or a SceneGraph.
///          Returns the number of nodes whose material was set.
int64_t rt_model3d_apply_variant(void *obj, void *target, int64_t variant_index);
/// @brief Generate LOD chains (1..4 levels of ~ratio^k triangles) for every mesh node in
///        the asset's template and scene hierarchies, and enable auto screen-error
///        selection. Each unique mesh is decimated once; nodes that already carry LOD
///        chains are skipped. Instantiate() clones inherit the chains.
/// @return Number of nodes that received a LOD chain.
int64_t rt_model3d_generate_lods(void *obj, int64_t levels, double ratio);

/// @brief Find a scene-graph node by name (NULL if not found).
void *rt_model3d_find_node(void *obj, rt_string name);
/// @brief Find a scene-graph node by name as Some(SceneNode3D), or None when absent.
void *rt_model3d_find_node_option(void *obj, rt_string name);
/// @brief Instantiate the model's default node hierarchy as a Scene3D node.
void *rt_model3d_instantiate(void *obj);
/// @brief Instantiate the model as a complete standalone Scene3D.
void *rt_model3d_instantiate_scene(void *obj);
/// @brief Instantiate the immutable scene at @p index as a complete standalone Scene3D.
void *rt_model3d_instantiate_scene_at(void *obj, int64_t index);
/// @brief Load a model file and return a retained skeletal Animation3D clip by index.
/// @details This is a convenience for external animation-file workflows. The temporary
/// Model3D is released before returning, so the caller receives a standalone retained clip.
void *rt_model3d_load_animation(rt_string path, int64_t index);
/// @brief Load a model file and return a skeletal Animation3D clip as `Result.Ok` or `Err`.
void *rt_model3d_load_animation_result(rt_string path, int64_t index);
/// @brief Load a packed model asset and return a retained skeletal Animation3D clip by index.
void *rt_model3d_load_animation_asset(rt_string path, int64_t index);
/// @brief Load a packed model asset and return a skeletal Animation3D clip as a Result.
void *rt_model3d_load_animation_asset_result(rt_string path, int64_t index);
/// @brief Load a model file and return a retained NodeAnimation3D clip by index.
/// @details Use this for glTF object, camera, and morph-weight animation clips.
void *rt_model3d_load_node_animation(rt_string path, int64_t index);
/// @brief Load a model file and return a NodeAnimation3D clip as a Result.
void *rt_model3d_load_node_animation_result(rt_string path, int64_t index);
/// @brief Load a packed model asset and return a retained NodeAnimation3D clip by index.
void *rt_model3d_load_node_animation_asset(rt_string path, int64_t index);
/// @brief Load a packed model asset and return a NodeAnimation3D clip as a Result.
void *rt_model3d_load_node_animation_asset_result(rt_string path, int64_t index);

#ifdef __cplusplus
}
#endif
