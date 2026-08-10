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

/// @file
/// @brief Declares imported Model3D loading, enumeration, instantiation, and persistence APIs.
/// @details Model3D owns immutable imported templates and retained asset catalogs. Enumeration
///          accessors return borrowed resources, while load and instantiation routines return
///          caller-owned runtime objects.

#pragma once

#include "rt_string.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Load a 3D model asset (mesh/material/skeleton/animation graph) from
///        a file.
/// @param path Filesystem path with a supported model extension.
/// @return New Model3D handle, or NULL on failure.
void *rt_model3d_load(rt_string path);
/// @brief Load a 3D model asset from a file as `Result.Ok(SceneAsset)` or `Result.Err(String)`.
/// @details This side-channel-free companion to @ref rt_model3d_load preserves the same loader
/// behavior while returning recoverable failure diagnostics directly in the Result.
/// @param path Filesystem path with a supported model extension.
/// @return Owned Result containing a Model3D or diagnostic string.
void *rt_model3d_load_result(rt_string path);
/// @brief Load VSCN text as `Result.Ok(SceneAsset)` or `Result.Err(String)` without reading
///        the document from disk (ADR 0190). @p path must carry a .scene3d/.vscn extension
///        and names the relative prefab base directory plus diagnostics.
/// @param path Logical VSCN path used for extension validation, dependency bases, and diagnostics.
/// @param text Complete VSCN source text.
/// @return Owned Result containing a Model3D or diagnostic string.
void *rt_model3d_load_text_result(rt_string path, rt_string text);
/// @brief Load with explicit import options: `force_tangents` generates tangents for
///        every UV0-mapped glTF primitive even without a normal map bound at load.
/// @param path Filesystem model path.
/// @param force_tangents Nonzero to force tangent generation on eligible glTF primitives.
/// @return New Model3D handle, or NULL on failure.
void *rt_model3d_load_with_options(rt_string path, int8_t force_tangents);
/// @brief Options-string variant: comma-separated flags "forceTangents",
///   "eightInfluences", "compressAnimations"; unknown flags are ignored.
/// @param path Filesystem model path.
/// @param options Nullable comma-separated importer option string.
/// @return New Model3D handle, or NULL on failure.
void *rt_model3d_load_with_options_ex(rt_string path, rt_string options);
/// @brief Options-aware Result variant of @ref rt_model3d_load_with_options.
/// @param path Filesystem model path.
/// @param force_tangents Nonzero to force tangent generation on eligible glTF primitives.
/// @return Owned Result containing a Model3D or diagnostic string.
void *rt_model3d_load_result_with_options(rt_string path, int8_t force_tangents);
/// @brief Load a 3D model asset through the runtime asset manager.
/// @details glTF/GLB assets resolve external dependencies relative to the model
///          asset and search mounted/embedded packages before dev filesystem files.
/// @param path Logical asset path or URI.
/// @return New Model3D handle, or NULL on failure.
void *rt_model3d_load_asset(rt_string path);
/// @brief Load a 3D model asset through the asset manager as `Result.Ok` or `Result.Err`.
/// @details Failure diagnostics are returned in the Result instead of requiring
/// `AssetDiagnostics3D.LastLoadError`.
/// @param path Logical asset path or URI.
/// @return Owned Result containing a Model3D or diagnostic string.
void *rt_model3d_load_asset_result(rt_string path);
/// @brief Save the complete imported scene asset as VSCN v5, or v6 when node metadata exists.
/// @details Unlike SceneGraph.Save, this preserves every immutable scene, camera association,
/// animation class, material variant, morph target, enumerable shared resource, and exact
/// supported texture source container. Typed SceneNode metadata is preserved exactly.
/// @param obj Model3D to serialize.
/// @param path Destination filesystem path.
/// @return One after atomic publication, otherwise zero.
int64_t rt_model3d_save(void *obj, rt_string path);
/// @brief Retain only the listed animation clips, releasing every other clip in place.
/// @details Tool-facing subset helper for per-clip bakes (`zanna asset bake --clips`) — not a
///          registered scripting-surface method. Both index lists must be strictly ascending and
///          in range; on any invalid input the model is left completely unchanged. Kept clips
///          preserve their relative order. Scene templates hold no serialized references into the
///          clip arrays (VSCN scenes own no clips), so a filtered model saves and reloads cleanly.
/// @param obj Model3D whose clip inventory is filtered.
/// @param animation_indices Strictly ascending kept skeletal-animation indices, or NULL when
///        @p animation_index_count is zero (drops every skeletal clip).
/// @param animation_index_count Number of kept skeletal-animation indices.
/// @param node_animation_indices Strictly ascending kept node-animation indices, or NULL when
///        @p node_animation_index_count is zero (drops every node clip).
/// @param node_animation_index_count Number of kept node-animation indices.
/// @return One after filtering, zero on invalid model or index lists (model unchanged).
int64_t rt_model3d_keep_animation_subset(void *obj,
                                         const int64_t *animation_indices,
                                         int32_t animation_index_count,
                                         const int64_t *node_animation_indices,
                                         int32_t node_animation_index_count);
/// @brief Internal async path: build a glTF/GLB model from preloaded root bytes.
/// @details Takes ownership of @p preloaded_data; callers must not reuse it.
/// @param path Logical source path for format and dependency resolution.
/// @param preloaded_data Owned root-file byte buffer.
/// @param preloaded_size Byte count of @p preloaded_data.
/// @param load_assets Nonzero to resolve dependencies through the asset manager.
/// @return New Model3D handle, or NULL on failure; input bytes are consumed in either case.
void *rt_model3d_load_preloaded_gltf(rt_string path,
                                     uint8_t *preloaded_data,
                                     size_t preloaded_size,
                                     int load_assets);
/// @brief Internal async path: build an FBX model from preloaded root bytes.
/// @details Takes ownership of @p preloaded_data; callers must not reuse it.
/// @param path Logical source path used for format context and diagnostics.
/// @param preloaded_data Owned root-file byte buffer.
/// @param preloaded_size Byte count of @p preloaded_data.
/// @param load_assets Nonzero to use asset-manager dependency behavior.
/// @return New Model3D handle, or NULL on failure; input bytes are consumed in either case.
void *rt_model3d_load_preloaded_fbx(rt_string path,
                                    uint8_t *preloaded_data,
                                    size_t preloaded_size,
                                    int load_assets);
struct rt_gltf_preload_bundle;
/// @brief Internal async path: load a glTF/GLB Model3D from staged root/dependency bytes.
/// @details Takes ownership of @p bundle.
/// @param path Logical glTF/GLB source path.
/// @param bundle Owned staged root/dependency bundle.
/// @param load_assets Nonzero to retain asset-manager resolution semantics.
/// @return New Model3D handle, or NULL on failure; @p bundle is consumed in either case.
void *rt_model3d_load_preloaded_gltf_bundle(rt_string path,
                                            struct rt_gltf_preload_bundle *bundle,
                                            int load_assets);

/// @brief Number of meshes contained in the model.
/// @param obj Model3D receiver.
/// @return Safe enumerable mesh count, or zero for invalid input.
int64_t rt_model3d_get_mesh_count(void *obj);
/// @brief Number of materials contained in the model.
/// @param obj Model3D receiver.
/// @return Safe enumerable material count, or zero for invalid input.
int64_t rt_model3d_get_material_count(void *obj);
/// @brief Number of skeletons contained in the model.
/// @param obj Model3D receiver.
/// @return Safe enumerable skeleton count, or zero for invalid input.
int64_t rt_model3d_get_skeleton_count(void *obj);
/// @brief Number of animations contained in the model.
/// @param obj Model3D receiver.
/// @return Safe skeletal-animation count, or zero for invalid input.
int64_t rt_model3d_get_animation_count(void *obj);
/// @brief Number of node/object/morph/camera animations contained in the model.
/// @details NodeAnimation3D clips are imported from glTF node animation channels and are
/// separate from skeletal Animation3D clips.
/// @param obj Model3D receiver.
/// @return Safe node-animation count, or zero for invalid input.
int64_t rt_model3d_get_node_animation_count(void *obj);
/// @brief Number of enumerable meshes carrying an attached MorphTarget3D container.
/// @param obj Model3D receiver.
/// @return Count of valid mesh-attached morph-target containers.
int64_t rt_model3d_get_morph_target_count(void *obj);
/// @brief Total number of morph shapes across every enumerable mesh.
/// @param obj Model3D receiver.
/// @return Total safe shape count, saturated at `INT64_MAX`.
int64_t rt_model3d_get_morph_shape_count(void *obj);
/// @brief Number of scene-graph nodes contained in the model.
/// @param obj Model3D receiver.
/// @return Template subtree node count excluding the synthetic root, or zero.
int64_t rt_model3d_get_node_count(void *obj);
/// @brief Number of immutable scenes contained in the model.
/// @param obj Model3D receiver.
/// @return Addressable immutable-scene count, or zero for invalid input.
int64_t rt_model3d_get_scene_count(void *obj);
/// @brief Number of imported cameras in @p scene_index.
/// @param obj Model3D receiver.
/// @param scene_index Zero-based immutable-scene index.
/// @return Safe camera count, or zero for an invalid model or scene index.
int64_t rt_model3d_get_camera_count(void *obj, int64_t scene_index);

/// @brief Get the mesh at @p index (NULL if out of range).
/// @param obj Model3D receiver.
/// @param index Zero-based mesh index.
/// @return Borrowed Mesh3D owned by the model, or NULL; callers must not release it.
void *rt_model3d_get_mesh(void *obj, int64_t index);
/// @brief Get the material at @p index (NULL if out of range).
/// @param obj Model3D receiver.
/// @param index Zero-based material index.
/// @return Borrowed Material3D owned by the model, or NULL.
void *rt_model3d_get_material(void *obj, int64_t index);
/// @brief Get the skeleton at @p index (NULL if out of range).
/// @param obj Model3D receiver.
/// @param index Zero-based skeleton index.
/// @return Borrowed Skeleton3D owned by the model, or NULL.
void *rt_model3d_get_skeleton(void *obj, int64_t index);
/// @brief Get the animation at @p index (NULL if out of range).
/// @param obj Model3D receiver.
/// @param index Zero-based skeletal-animation index.
/// @return Borrowed Animation3D owned by the model, or NULL.
void *rt_model3d_get_animation(void *obj, int64_t index);
/// @brief Get the node animation at @p index (NULL if out of range).
/// @param obj Model3D receiver.
/// @param index Zero-based node-animation index.
/// @return Borrowed NodeAnimation3D owned by the model, or NULL.
void *rt_model3d_get_node_animation(void *obj, int64_t index);
/// @brief Get the MorphTarget3D attached to mesh @p mesh_index, or NULL.
/// @param obj Model3D receiver.
/// @param mesh_index Zero-based enumerable mesh index.
/// @return Borrowed MorphTarget3D owned through the mesh, or NULL.
void *rt_model3d_get_morph_target(void *obj, int64_t mesh_index);
/// @brief Get the node animation name at @p index (empty string if out of range).
/// @param obj Model3D receiver.
/// @param index Zero-based node-animation index.
/// @return Borrowed runtime string name, or the empty string for invalid input.
rt_string rt_model3d_get_node_animation_name(void *obj, int64_t index);
/// @brief Get the imported camera at @p index in @p scene_index (NULL if out of range).
/// @param obj Model3D receiver.
/// @param scene_index Zero-based immutable-scene index.
/// @param index Zero-based camera index within the scene.
/// @return Borrowed Camera3D retained by the scene entry, or NULL.
void *rt_model3d_get_camera(void *obj, int64_t scene_index, int64_t index);
/// @brief Get the immutable scene name at @p index (empty string if out of range).
/// @param obj Model3D receiver.
/// @param index Zero-based immutable-scene index.
/// @return Borrowed runtime string name, or the empty string for invalid input.
rt_string rt_model3d_get_scene_name(void *obj, int64_t index);

/// @brief Number of KHR_materials_variants names imported with the asset (0 when absent).
/// @param obj Model3D receiver.
/// @return Imported variant-name count, or zero for invalid input or absent metadata.
int64_t rt_model3d_get_variant_count(void *obj);
/// @brief Get the material-variant name at @p index (empty string if out of range).
/// @param obj Model3D receiver.
/// @param index Zero-based material-variant index.
/// @return Borrowed variant name, or the empty string for invalid input.
rt_string rt_model3d_get_variant_name(void *obj, int64_t index);
/// @brief Apply material variant @p variant_index to every mapped node under @p target.
/// @details @p target is a SceneNode3D (e.g. an Instantiate() result) or a SceneGraph.
///          Returns the number of nodes whose material was set.
/// @param obj Model3D containing the imported variant table.
/// @param target SceneNode3D subtree root or Scene3D whose nodes are updated.
/// @param variant_index Zero-based imported variant index.
/// @return Number of nodes assigned a mapped material, or zero for invalid input.
int64_t rt_model3d_apply_variant(void *obj, void *target, int64_t variant_index);
/// @brief Drop all node visuals and release the model's mesh/material inventories.
/// @details Used by animation-only asset bakes; node transforms, names, skeletons, and animation
///          clips remain intact.
/// @param obj Model3D modified in place.
/// @return Number of inventory meshes released, or zero for invalid input/failure.
int64_t rt_model3d_strip_meshes(void *obj);
/// @brief Downscale every material texture above @p max_dim to fit it
///        (tool-facing: `zanna asset bake --max-texture-dim N`). Oversized
///        slots are rewritten to raw downscaled Pixels, so a subsequent
///        VSCN save stores compact pixel payloads instead of the original
///        full-resolution encoded containers (removing their per-load
///        decode cost). Shared textures are resized once.
/// @param obj Model3D whose materials are rewritten in place.
/// @param max_dim Texture dimension ceiling in texels (minimum 64).
/// @return Number of texture slots rewritten, or 0 for invalid input.
int64_t rt_model3d_limit_texture_dim(void *obj, int64_t max_dim);
/// @brief Replace over-budget meshes with simplified copies throughout the model hierarchy.
/// @param obj Model3D modified in place.
/// @param max_tris Maximum triangles retained by each simplified mesh.
/// @return Number of inventory meshes replaced, or zero for invalid input/failure.
int64_t rt_model3d_simplify_meshes(void *obj, int64_t max_tris);
/// @brief Generate LOD chains (1..4 levels of repeated ratio-scaled triangles) for every mesh node
/// in
///        the asset's template and scene hierarchies, and enable auto screen-error
///        selection. Each unique mesh is decimated once; nodes that already carry LOD
///        chains are skipped. Instantiate() clones inherit the chains.
/// @param obj Model3D whose immutable template and scene hierarchies are augmented.
/// @param levels Requested number of generated levels, from one through four.
/// @param ratio Per-level target triangle ratio.
/// @return Number of nodes that received a LOD chain.
int64_t rt_model3d_generate_lods(void *obj, int64_t levels, double ratio);

/// @brief Find a scene-graph node by name (NULL if not found).
/// @details The returned node belongs to the immutable imported template; mutate an instantiated
///          clone instead.
/// @param obj Model3D receiver.
/// @param name Exact node name to match.
/// @return Borrowed template SceneNode3D, or NULL when absent or invalid.
void *rt_model3d_find_node(void *obj, rt_string name);
/// @brief Find a scene-graph node by name as Some(SceneNode3D), or None when absent.
/// @param obj Model3D receiver.
/// @param name Exact node name to match.
/// @return Owned Option wrapping the borrowed template node when found.
void *rt_model3d_find_node_option(void *obj, rt_string name);
/// @brief Instantiate the model's default node hierarchy as a Scene3D node.
/// @details Deep-clones nodes and mutable typed metadata, shares static resources, and clones
///          morph-enabled meshes so blend-shape state remains instance-local.
/// @param obj Model3D receiver.
/// @return New synthetic-root SceneNode3D subtree, or NULL on invalid input or cloning failure.
void *rt_model3d_instantiate(void *obj);
/// @brief Instantiate the model as a complete standalone Scene3D.
/// @param obj Model3D receiver.
/// @return New Scene3D for the default immutable scene, or NULL on failure.
void *rt_model3d_instantiate_scene(void *obj);
/// @brief Instantiate the immutable scene at @p index as a complete standalone Scene3D.
/// @param obj Model3D receiver.
/// @param index Zero-based immutable-scene index.
/// @return New independently mutable Scene3D, or NULL for invalid input or cloning failure.
void *rt_model3d_instantiate_scene_at(void *obj, int64_t index);
/// @brief Load a model file and return a retained skeletal Animation3D clip by index.
/// @details This is a convenience for external animation-file workflows. The temporary
/// Model3D is released before returning, so the caller receives a standalone retained clip.
/// @param path Filesystem model path.
/// @param index Zero-based skeletal-animation index.
/// @return Retained standalone Animation3D, or NULL on load failure or invalid index.
void *rt_model3d_load_animation(rt_string path, int64_t index);
/// @brief Load a model file and return a skeletal Animation3D clip as `Result.Ok` or `Err`.
/// @param path Filesystem model path.
/// @param index Zero-based skeletal-animation index.
/// @return Owned Result carrying Animation3D or an error string.
void *rt_model3d_load_animation_result(rt_string path, int64_t index);
/// @brief Load a packed model asset and return a retained skeletal Animation3D clip by index.
/// @param path Logical asset path or URI.
/// @param index Zero-based skeletal-animation index.
/// @return Retained standalone Animation3D, or NULL on load failure or invalid index.
void *rt_model3d_load_animation_asset(rt_string path, int64_t index);
/// @brief Load a packed model asset and return a skeletal Animation3D clip as a Result.
/// @param path Logical asset path or URI.
/// @param index Zero-based skeletal-animation index.
/// @return Owned Result carrying Animation3D or an error string.
void *rt_model3d_load_animation_asset_result(rt_string path, int64_t index);
/// @brief Load a model file and return a retained NodeAnimation3D clip by index.
/// @details Use this for glTF object, camera, and morph-weight animation clips.
/// @param path Filesystem model path.
/// @param index Zero-based node-animation index.
/// @return Retained standalone NodeAnimation3D, or NULL on load failure or invalid index.
void *rt_model3d_load_node_animation(rt_string path, int64_t index);
/// @brief Load a model file and return a NodeAnimation3D clip as a Result.
/// @param path Filesystem model path.
/// @param index Zero-based node-animation index.
/// @return Owned Result carrying NodeAnimation3D or an error string.
void *rt_model3d_load_node_animation_result(rt_string path, int64_t index);
/// @brief Load a packed model asset and return a retained NodeAnimation3D clip by index.
/// @param path Logical asset path or URI.
/// @param index Zero-based node-animation index.
/// @return Retained standalone NodeAnimation3D, or NULL on load failure or invalid index.
void *rt_model3d_load_node_animation_asset(rt_string path, int64_t index);
/// @brief Load a packed model asset and return a NodeAnimation3D clip as a Result.
/// @param path Logical asset path or URI.
/// @param index Zero-based node-animation index.
/// @return Owned Result carrying NodeAnimation3D or an error string.
void *rt_model3d_load_node_animation_asset_result(rt_string path, int64_t index);

#ifdef __cplusplus
}
#endif
