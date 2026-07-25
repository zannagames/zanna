//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/assets/rt_gltf.h
// Purpose: glTF 2.0 (.gltf/.glb) asset loader.
// Key invariants:
//   - Supports .gltf (JSON + external files) and .glb (binary container)
//   - PBR metallic-roughness materials are preserved on Material3D's native PBR surface
//   - Embedded base64 buffers/images and GLB bufferView images are supported
//   - Extracts meshes, materials, node-attached lights, skeletons, animations,
//     and the active-scene node hierarchy
// Ownership/Lifetime:
//   - Caller owns the returned asset and all objects within it
// Links: rt_mesh3d.c, rt_material3d.c, rt_skeleton3d.c
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_gltf.h
 * @brief Declares glTF 2.0/GLB loading, asynchronous preload staging, import options,
 *        and typed asset queries.
 *
 * Load functions return a GC-managed aggregate asset. Resource accessors borrow meshes,
 * materials, skeletons, animations, cameras, and scene roots from that asset; those
 * handles must not outlive it. String accessors return runtime string handles following
 * normal runtime-string reference ownership. Preload APIs make ownership transfer of
 * malloc-backed root data and bundles explicit.
 */

#pragma once

#include "rt_string.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Optional glTF import behavior knobs (Plan 09). Zero-initialized = defaults
///   (current behavior: tangents generated only when a normal map is bound at load).
typedef struct rt_gltf_load_options {
    /// Generate tangents for every UV0-mapped primitive even when its material has
    /// no normal map yet (for materials that gain normal maps after import).
    int8_t force_tangents;
    /// Keep up to 8 bone influences per vertex: the strongest 4 stay in the vertex
    /// record and influences 5-8 ride a per-mesh side stream applied by the CPU
    /// skinning path (GPU fast path is bypassed for such meshes).
    int8_t eight_bone_influences;
    /// Tolerance-based keyframe reduction for imported animation clips.
    int8_t compress_animations;
    /// Reserved bytes; callers must leave these zero for forward compatibility.
    int8_t reserved[5];
} rt_gltf_load_options;

/// @brief Default-initialized import options (all zero — current loader behavior).
/// @return Zero-initialized option value.
rt_gltf_load_options rt_gltf_load_options_default(void);
/// @brief Borrow the calling thread's active import options (never NULL).
/// @details Load entry points scope options thread-locally so the option state reaches
///   the mesh decode gates without threading a parameter through every loader layer;
///   worker-decoded preload bundles apply the gates on the committing thread.
/// @return Borrowed pointer to calling-thread storage.
const rt_gltf_load_options *rt_gltf_active_load_options(void);
/// @brief Install @p opts as the calling thread's import options; returns the previous
///   value so callers can save/restore around a load (NULL restores defaults).
/// @param opts Options to copy, or NULL to restore defaults.
/// @return Previous option value.
rt_gltf_load_options rt_gltf_set_thread_load_options(const rt_gltf_load_options *opts);

/// @brief Load a glTF/GLB asset directly from a filesystem path.
/// @param path Runtime string containing the native path.
/// @return New asset handle on success, or NULL on failure.
void *rt_gltf_load(rt_string path);
/// @brief Load a glTF/GLB asset through the runtime asset manager.
/// @details Supports mounted/embedded assets plus dev filesystem fallback. External
///          .gltf buffers/images resolve relative to the parent model asset.
/// @param path Runtime string containing the logical asset path.
/// @return New asset handle on success, or NULL on failure.
void *rt_gltf_load_asset(rt_string path);
/// @brief Internal async path: load glTF/GLB from preloaded root bytes.
/// @details Takes ownership of @p preloaded_data; callers must not reuse it.
/// @param path Runtime source path for diagnostics and relative dependencies.
/// @param[in,out] preloaded_data Malloc-owned root-document bytes transferred to the loader.
/// @param preloaded_size Number of readable root bytes.
/// @param load_assets Non-zero to resolve dependencies through the asset manager.
/// @return New asset handle on success, or NULL on failure.
void *rt_gltf_load_preloaded(rt_string path,
                             uint8_t *preloaded_data,
                             size_t preloaded_size,
                             int load_assets);
/// @brief Opaque malloc-owned staged root/dependency bundle.
typedef struct rt_gltf_preload_bundle rt_gltf_preload_bundle;
/// @brief Internal async path: stage a glTF/GLB root plus dependency bytes.
/// @details Takes ownership of @p root_data on both success and failure. The returned
///          bundle contains only malloc-owned POD bytes and may be built on a worker.
/// @param path Runtime source path.
/// @param[in,out] root_data Malloc-owned root bytes transferred to this call.
/// @param root_size Number of readable root bytes.
/// @param load_assets Non-zero to stage through asset-manager resolution.
/// @param[out] error Optional diagnostic buffer.
/// @param error_cap Capacity of @p error in bytes.
/// @return New preload bundle on success, or NULL with @p error populated when possible.
rt_gltf_preload_bundle *rt_gltf_preload_bundle_create(rt_string path,
                                                      uint8_t *root_data,
                                                      size_t root_size,
                                                      int load_assets,
                                                      char *error,
                                                      size_t error_cap);
/// @brief Internal async path: stage a glTF/GLB root plus dependencies from a C path.
/// @details Takes ownership of @p root_data on both success and failure. This variant avoids
///          touching runtime strings from worker threads; use it only with filesystem paths unless
///          the caller can guarantee any asset-system access is thread-safe for its use case.
/// @param path NUL-terminated source path.
/// @param[in,out] root_data Malloc-owned root bytes transferred to this call.
/// @param root_size Number of readable root bytes.
/// @param load_assets Non-zero to stage through asset-manager resolution.
/// @param[out] error Optional diagnostic buffer.
/// @param error_cap Capacity of @p error in bytes.
/// @return New preload bundle on success, or NULL with @p error populated when possible.
rt_gltf_preload_bundle *rt_gltf_preload_bundle_create_cstr(const char *path,
                                                           uint8_t *root_data,
                                                           size_t root_size,
                                                           int load_assets,
                                                           char *error,
                                                           size_t error_cap);
/// @brief Free a bundle returned by rt_gltf_preload_bundle_create.
/// @param[in,out] bundle Bundle and all unconsumed payloads to free; NULL is accepted.
void rt_gltf_preload_bundle_free(rt_gltf_preload_bundle *bundle);
/// @brief Number of dependency payloads staged into @p bundle.
/// @param bundle Preload bundle.
/// @return Staged dependency count, or zero for NULL.
size_t rt_gltf_preload_bundle_dependency_count(const rt_gltf_preload_bundle *bundle);
/// @brief Number of staged image payloads that were worker-decoded to raw RGBA POD.
/// @param bundle Preload bundle.
/// @return Worker-decoded image count, or zero for NULL.
size_t rt_gltf_preload_bundle_decoded_image_count(const rt_gltf_preload_bundle *bundle);
/// @brief Estimated decoded RGBA bytes staged in @p bundle for upload budgeting.
/// @param bundle Preload bundle.
/// @return Total staged decoded-image byte estimate, or zero for NULL.
size_t rt_gltf_preload_bundle_decoded_image_bytes(const rt_gltf_preload_bundle *bundle);
/// @brief Estimated bytes in the next decoded image slice that still needs main-thread prep.
/// @param bundle Preload bundle.
/// @param max_bytes Maximum preferred slice budget; zero permits the implementation default.
/// @return Estimated bytes for the next preparable slice, or zero when none remains.
size_t rt_gltf_preload_bundle_next_decoded_image_slice_bytes(const rt_gltf_preload_bundle *bundle,
                                                             size_t max_bytes);
/// @brief Convert one decoded RGBA image slice into its commit-side Pixels object.
/// @param[in,out] bundle Preload bundle whose next decoded images are prepared.
/// @param max_bytes Maximum preferred preparation budget.
/// @return Number of decoded source bytes prepared during this call.
size_t rt_gltf_preload_bundle_prepare_decoded_image_slice(rt_gltf_preload_bundle *bundle,
                                                          size_t max_bytes);
/// @brief Number of static mesh primitives worker-decoded to raw Mesh3D POD.
/// @param bundle Preload bundle.
/// @return Worker-decoded mesh primitive count, or zero for NULL.
size_t rt_gltf_preload_bundle_decoded_mesh_count(const rt_gltf_preload_bundle *bundle);
/// @brief Internal async path: load glTF/GLB from a staged preload bundle.
/// @details Takes ownership of @p bundle.
/// @param path Runtime source path used for diagnostics.
/// @param[in,out] bundle Bundle transferred to and freed by this call.
/// @param load_assets Non-zero to select asset-manager fallback when @p bundle is NULL.
/// @return New asset handle on success, or NULL on failure.
void *rt_gltf_load_preloaded_bundle(rt_string path,
                                    rt_gltf_preload_bundle *bundle,
                                    int load_assets);
/// @brief Number of meshes in the asset.
/// @param asset Candidate glTF asset handle.
/// @return Safe mesh count, or zero for an invalid handle.
int64_t rt_gltf_mesh_count(void *asset);
/// @brief Get the mesh at @p index (NULL if out of range).
/// @param asset Candidate glTF asset handle.
/// @param index Zero-based mesh index.
/// @return Borrowed Mesh3D handle, or NULL.
void *rt_gltf_get_mesh(void *asset, int64_t index);
/// @brief Number of materials in the asset.
/// @param asset Candidate glTF asset handle.
/// @return Safe material count, or zero.
int64_t rt_gltf_material_count(void *asset);
/// @brief Get the material at @p index (NULL if out of range).
/// @param asset Candidate glTF asset handle.
/// @param index Zero-based material index.
/// @return Borrowed Material3D handle, or NULL.
void *rt_gltf_get_material(void *asset, int64_t index);
/// @brief Number of skeletons (skins) in the asset.
/// @param asset Candidate glTF asset handle.
/// @return Safe skeleton count, or zero.
int64_t rt_gltf_skeleton_count(void *asset);
/// @brief Get the skeleton at @p index (NULL if out of range).
/// @param asset Candidate glTF asset handle.
/// @param index Zero-based skeleton index.
/// @return Borrowed Skeleton3D handle, or NULL.
void *rt_gltf_get_skeleton(void *asset, int64_t index);
/// @brief Number of skeletal animation clips.
/// @param asset Candidate glTF asset handle.
/// @return Safe skeletal animation count, or zero.
int64_t rt_gltf_animation_count(void *asset);
/// @brief Get the skeletal animation at @p index (NULL if out of range).
/// @param asset Candidate glTF asset handle.
/// @param index Zero-based animation index.
/// @return Borrowed Animation3D handle, or NULL.
void *rt_gltf_get_animation(void *asset, int64_t index);
/// @brief Number of node (transform) animation clips.
/// @param asset Candidate glTF asset handle.
/// @return Safe node-animation count, or zero.
int64_t rt_gltf_node_animation_count(void *asset);
/// @brief Get the node animation at @p index (NULL if out of range).
/// @param asset Candidate glTF asset handle.
/// @param index Zero-based node-animation index.
/// @return Borrowed NodeAnimation3D handle, or NULL.
void *rt_gltf_get_node_animation(void *asset, int64_t index);
/// @brief Number of imported cameras in the active scene.
/// @param asset Candidate glTF asset handle.
/// @return Safe active-scene camera count, or zero.
int64_t rt_gltf_camera_count(void *asset);
/// @brief Get the imported camera at @p index (NULL if out of range).
/// @param asset Candidate glTF asset handle.
/// @param index Zero-based active-scene camera index.
/// @return Borrowed Camera3D handle, or NULL.
void *rt_gltf_get_camera(void *asset, int64_t index);
/// @brief Number of immutable scenes in the asset. Index 0 is the active/default scene.
/// @param asset Candidate glTF asset handle.
/// @return Safe immutable-scene count, or zero.
int64_t rt_gltf_scene_count(void *asset);
/// @brief Get the immutable scene name at @p index (empty string if out of range).
/// @param asset Candidate glTF asset handle.
/// @param index Zero-based immutable-scene index.
/// @return Runtime string containing the name or an empty string.
rt_string rt_gltf_get_scene_name(void *asset, int64_t index);
/// @brief Get the scene-graph root for immutable scene @p index (NULL if out of range).
/// @param asset Candidate glTF asset handle.
/// @param index Zero-based immutable-scene index.
/// @return Borrowed SceneNode3D root, or NULL.
void *rt_gltf_get_scene_root_at(void *asset, int64_t index);
/// @brief Number of imported cameras reachable from immutable scene @p scene_index.
/// @param asset Candidate glTF asset handle.
/// @param scene_index Zero-based immutable-scene index.
/// @return Safe scene-local camera count, or zero.
int64_t rt_gltf_scene_camera_count(void *asset, int64_t scene_index);
/// @brief Get an imported camera from immutable scene @p scene_index (NULL if out of range).
/// @param asset Candidate glTF asset handle.
/// @param scene_index Zero-based immutable-scene index.
/// @param index Zero-based camera index within that scene.
/// @return Borrowed Camera3D handle, or NULL.
void *rt_gltf_get_scene_camera(void *asset, int64_t scene_index, int64_t index);
/// @brief Number of nodes in the asset's scene graph.
/// @param asset Candidate glTF asset handle.
/// @return Active-scene node count, or zero.
int64_t rt_gltf_node_count(void *asset);
/// @brief Get the asset's scene-graph root node (NULL if none).
/// @param asset Candidate glTF asset handle.
/// @return Borrowed active SceneNode3D root, or NULL.
void *rt_gltf_get_scene_root(void *asset);
/// @brief Number of KHR_materials_variants names imported with the asset (0 when absent).
/// @param asset Candidate glTF asset handle.
/// @return Imported variant-name count, or zero.
int64_t rt_gltf_variant_count(void *asset);
/// @brief Get the material-variant name at @p index (empty string if out of range).
/// @param asset Candidate glTF asset handle.
/// @param index Zero-based variant index.
/// @return Runtime string containing the name or an empty string.
rt_string rt_gltf_get_variant_name(void *asset, int64_t index);

/// @brief Fuzz/test hook: decode a raw KHR_draco_mesh_compression payload and free the
///   result; returns nonzero on a successful decode. Not a scripting-surface method — it
///   exercises the Draco decoder directly, which the load_assets=0 glTF preload fuzzer
///   cannot reach. See src/tests/fuzz/fuzz_gltf_draco.cpp.
/// @param data Candidate raw Draco bytes.
/// @param size Number of readable bytes.
/// @return Non-zero when graphics-enabled decoding succeeds; otherwise zero.
int rt_gltf_draco_decode_probe(const unsigned char *data, size_t size);

#ifdef __cplusplus
}
#endif
