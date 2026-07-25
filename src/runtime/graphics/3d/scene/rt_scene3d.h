//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/3d/scene/rt_scene3d.h
// Purpose: Zanna.Graphics3D.Scene3D and SceneNode3D — hierarchical scene graph
//   with parent-child transform propagation. Nodes hold local TRS (translate,
//   rotate, scale) and compute world matrices by multiplying up the hierarchy.
//
// Key invariants:
//   - Scene3D always has a root node (created in constructor).
//   - SceneNode3D world_matrix is recomputed lazily when dirty.
//   - Descendants notice parent transform changes through cached world revisions.
//   - Children are stored as a dynamic array (not GC-managed).
//   - Gameplay metadata is typed, bounded, and serialized only through VSCN.
//   - Nodes with both mesh and material are drawn; imported node lights contribute
//     to scene draw snapshots after world-transform evaluation.
//
// Links: rt_canvas3d.h, rt_quat.h, rt_mat4.h, plans/3d/12-scene-graph.md,
//   docs/adr/0159-typed-scenenode-metadata-and-vscn-v6.md,
//   docs/adr/0161-stable-scenenode-sibling-reordering.md,
//   docs/adr/0162-exact-preserve-world-scenenode-reparenting.md,
//   docs/adr/0166-exact-scenenode-world-matrix-assignment.md,
//   docs/adr/0172-public-scenenode-light-authoring-and-studio-light-inspector.md
//
//===----------------------------------------------------------------------===//
#pragma once

#include "rt_string.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Direction a SceneNode3D synchronizes transforms with its bound physics body:
///   physics→node, node→physics, animation-root-motion→node, or bidirectional kinematic.
enum {
    RT_SCENE_NODE3D_SYNC_NODE_FROM_BODY = 0,
    RT_SCENE_NODE3D_SYNC_BODY_FROM_NODE = 1,
    RT_SCENE_NODE3D_SYNC_NODE_FROM_ANIMATOR_ROOT_MOTION = 2,
    RT_SCENE_NODE3D_SYNC_TWO_WAY_KINEMATIC = 3,
};

/// @brief Stable limits for gameplay-facing SceneNode metadata.
#define RT_SCENE_NODE3D_MAX_METADATA_ENTRIES 256
#define RT_SCENE_NODE3D_MAX_METADATA_KEY_BYTES 128
#define RT_SCENE_NODE3D_MAX_METADATA_STRING_BYTES (64 * 1024)

/* Scene3D */
/// @brief Create an empty 3D scene with a single root node.
void *rt_scene3d_new(void);
/// @brief Get the implicit root node so callers can attach children directly.
void *rt_scene3d_get_root(void *scene);
/// @brief Convenience: attach @p node beneath the scene root.
void rt_scene3d_add(void *scene, void *node);
/// @brief Convenience: attach @p node beneath the scene root; returns 1 on success.
int8_t rt_scene3d_try_add(void *scene, void *node);
/// @brief Convenience: detach @p node from the scene root.
void rt_scene3d_remove(void *scene, void *node);
/// @brief Recursively search the scene for a node whose name matches @p name (NULL if none).
void *rt_scene3d_find(void *scene, rt_string name);
/// @brief Recursively search the scene and return Some(SceneNode3D) on match, None on absence.
void *rt_scene3d_find_option(void *scene, rt_string name);
/// @brief Return visible mesh nodes whose world-space AABB intersects @p min/@p max.
void *rt_scene3d_query_aabb(void *scene, void *min, void *max);
/// @brief Return visible mesh nodes whose world-space AABB intersects @p center/@p radius.
void *rt_scene3d_query_sphere(void *scene, void *center, double radius);
/// @brief Return the closest visible mesh node hit by the world-space ray, or NULL.
void *rt_scene3d_raycast_nodes(void *scene, void *origin, void *direction, double max_distance);
/// @brief Add an authored interior visibility zone and return its zero-based index.
int64_t rt_scene3d_add_visibility_zone(void *scene, rt_string name, void *min, void *max);
/// @brief Add a directed or bidirectional portal between visibility zones.
int64_t rt_scene3d_add_visibility_portal(void *scene,
                                         int64_t from_zone,
                                         int64_t to_zone,
                                         int8_t bidirectional);
/// @brief Render every visible drawable node in the scene from @p camera onto @p canvas3d.
void rt_scene3d_draw(void *scene, void *canvas3d, void *camera);
/// @brief Detach all children of the root (does not delete the root itself).
void rt_scene3d_clear(void *scene);
/// @brief Total number of nodes in the scene (counts root + every descendant).
int64_t rt_scene3d_get_node_count(void *scene);
/// @brief Serialize the scene to a .vscn file. Returns 1 on success, 0 on failure.
int64_t rt_scene3d_save(void *scene, rt_string path);
/// @brief Serialize the scene to canonical VSCN text in memory (ADR 0190).
///   Byte-identical to rt_scene3d_save output; empty string on failure.
rt_string rt_scene3d_save_text(void *scene);
/// @brief Deserialize a scene from a `.vscn` (JSON) file. NULL on failure.
///   (glTF/FBX scenes load through Zanna.Graphics3D.GLTF.Load / FBX.Load.)
void *rt_scene3d_load(rt_string path);
/// @brief Deserialize a scene from already-read `.vscn` JSON text (internal streaming path).
///   @p path is diagnostics-only; the caller keeps ownership of @p text. NULL on failure.
void *rt_scene3d_load_from_memory(rt_string path, const char *text, size_t len);
/// @brief Push physics body, animator root-motion, and other bindings into node transforms.
/// Call once per frame after physics step but before draw.
void rt_scene3d_sync_bindings(void *scene, double dt);
/// @brief Shift every root-level scene subtree by -delta for floating-origin rebasing.
void rt_scene3d_rebase_origin(void *scene, double dx, double dy, double dz);

/* SceneNode3D */
/// @brief Create a SceneNode3D at the origin with identity rotation, unit scale, no mesh.
void *rt_scene_node3d_new(void);
/// @brief Set the node's local-space position (relative to its parent).
void rt_scene_node3d_set_position(void *node, double x, double y, double z);
/// @brief Get the node's local-space position as a Vec3.
void *rt_scene_node3d_get_position(void *node);
/// @brief Set the node's local-space orientation from a Quaternion.
void rt_scene_node3d_set_rotation(void *node, void *quat);
/// @brief Get the node's local-space orientation as a Quaternion.
void *rt_scene_node3d_get_rotation(void *node);
/// @brief Set the node's local-space scale (independent x/y/z factors).
void rt_scene_node3d_set_scale(void *node, double x, double y, double z);
/// @brief Get the node's local-space scale as a Vec3.
void *rt_scene_node3d_get_scale(void *node);
/// @brief Get the node's world-space transform as a Mat4 (lazily recomputed when dirty).
void *rt_scene_node3d_get_world_matrix(void *node);
/// @brief Assign a complete world matrix only when it has an exact parent-relative TRS.
/// @return 1 on success; 0 without mutation for invalid, singular, sheared, or lossy requests.
int8_t rt_scene_node3d_try_set_world_matrix(void *node, void *world_matrix);
/// @brief Get the node's world-space position as a Vec3.
void *rt_scene_node3d_get_world_position(void *node);
/// @brief Get the node's world-space position as raw components (1 on success, 0 on null node).
int8_t rt_scene_node3d_get_world_position_components(void *node, double *x, double *y, double *z);
/// @brief Get the node's world-space orientation as a Quaternion.
void *rt_scene_node3d_get_world_rotation(void *node);
/// @brief Get the node's world-space scale magnitudes as a Vec3.
void *rt_scene_node3d_get_world_scale(void *node);
/// @brief Get the local position as raw components (1 on success, 0 on invalid node).
int8_t rt_scene_node3d_get_position_components(void *node, double *x, double *y, double *z);
/// @brief Get the local rotation quaternion as raw components (1 on success, 0 on invalid node).
int8_t rt_scene_node3d_get_rotation_components(
    void *node, double *x, double *y, double *z, double *w);
/// @brief Get the world rotation quaternion as raw components (1 on success, 0 on invalid node).
int8_t rt_scene_node3d_get_world_rotation_components(
    void *node, double *x, double *y, double *z, double *w);
/// @brief Get the local scale as raw components (1 on success, 0 on invalid node).
int8_t rt_scene_node3d_get_scale_components(void *node, double *x, double *y, double *z);
/// @brief Get the world scale magnitudes as raw components (1 on success, 0 on invalid node).
int8_t rt_scene_node3d_get_world_scale_components(void *node, double *x, double *y, double *z);
/// @brief Copy the world matrix into @p out (row-major 16 doubles); 1 on success.
int8_t rt_scene_node3d_get_world_matrix_components(void *node, double out[16]);
/// @brief Set position, rotation (quaternion components), and scale in one call.
void rt_scene_node3d_set_transform(void *node,
                                   double px,
                                   double py,
                                   double pz,
                                   double qx,
                                   double qy,
                                   double qz,
                                   double qw,
                                   double sx,
                                   double sy,
                                   double sz);
/// @brief Apply packed TRS values (10 floats per node) to a list of nodes in one call.
void rt_scene_node3d_set_transform_batch(void *nodes, void *values);
/// @brief SceneGraph.SetNodeTransforms: batch-apply packed TRS values through a scene handle.
void rt_scene3d_set_node_transforms(void *scene, void *nodes, void *values);
/// @brief Attach @p child as a child of @p node (detaches from any prior parent).
void rt_scene_node3d_add_child(void *node, void *child);
/// @brief Attach @p child as a child of @p node, returning 1 when the child is parented there.
int8_t rt_scene_node3d_try_add_child(void *node, void *child);
/// @brief Attach @p child while preserving its complete world matrix, or reject without mutation.
int8_t rt_scene_node3d_try_add_child_preserve_world(void *node, void *child);
/// @brief Move an existing direct child to @p index without changing its parent.
int8_t rt_scene_node3d_try_move_child(void *node, void *child, int64_t index);
/// @brief Detach @p child from @p node (no-op if not actually a child).
void rt_scene_node3d_remove_child(void *node, void *child);
/// @brief Number of direct children of @p node.
int64_t rt_scene_node3d_child_count(void *node);
/// @brief Get the @p index-th child (NULL if out of range).
void *rt_scene_node3d_get_child(void *node, int64_t index);
/// @brief Get the node's parent (NULL if root or detached).
void *rt_scene_node3d_get_parent(void *node);
/// @brief Recursively search this subtree for a node whose name matches @p name.
void *rt_scene_node3d_find(void *node, rt_string name);
/// @brief Recursively search this subtree and return Some(SceneNode3D) on match, None on absence.
void *rt_scene_node3d_find_option(void *node, rt_string name);
/// @brief Bind a Mesh3D for rendering at this node's world transform.
void rt_scene_node3d_set_mesh(void *node, void *mesh);
/// @brief Get the bound Mesh3D (NULL if this is a transform-only node).
void *rt_scene_node3d_get_mesh(void *node);
/// @brief Bind a Material3D for the node's mesh draw.
void rt_scene_node3d_set_material(void *node, void *material);
/// @brief Get the bound Material3D (NULL if none).
void *rt_scene_node3d_get_material(void *node);
/// @brief Bind a Light3D transformed and submitted with this node; NULL clears.
void rt_scene_node3d_set_light(void *node, void *light);
/// @brief Get the node-attached Light3D, or NULL.
void *rt_scene_node3d_get_light(void *node);
/// @brief Bind a Camera3D whose view pose follows this node during SyncBindings.
void rt_scene_node3d_set_camera(void *node, void *camera);
/// @brief Get the node-attached Camera3D, or NULL.
void *rt_scene_node3d_get_camera(void *node);
/// @brief Toggle visibility (hidden nodes and their descendants are skipped during draw).
void rt_scene_node3d_set_visible(void *node, int8_t visible);
/// @brief Get the visibility flag.
int8_t rt_scene_node3d_get_visible(void *node);
/// @brief Set the node's name (used by `_find` lookups).
void rt_scene_node3d_set_name(void *node, rt_string name);
/// @brief Get the node's name as an owned string (empty if unset).
rt_string rt_scene_node3d_get_name(void *node);
rt_string rt_scene_node3d_get_prefab_path(void *node);
int8_t rt_scene_node3d_get_is_instance_content(void *node);
int8_t rt_scene_node3d_set_prefab_reference(void *node, rt_string path);
int8_t rt_scene_node3d_clear_prefab_reference(void *node);
/// @brief Return every gameplay-metadata key in deterministic lexicographic order.
void *rt_scene_node3d_metadata_keys(void *node);
/// @brief Return `null`, `bool`, `int`, `float`, or `string`; empty when @p key is absent.
rt_string rt_scene_node3d_metadata_kind(void *node, rt_string key);
/// @brief Return whether the node has an explicit metadata value for @p key.
int8_t rt_scene_node3d_metadata_has(void *node, rt_string key);
/// @brief Read exact integer metadata, or @p def when absent or differently typed.
int64_t rt_scene_node3d_metadata_get_int(void *node, rt_string key, int64_t def);
/// @brief Read float metadata (or widen an integer), or @p def on mismatch.
double rt_scene_node3d_metadata_get_float(void *node, rt_string key, double def);
/// @brief Read boolean metadata, or @p def when absent or differently typed.
int8_t rt_scene_node3d_metadata_get_bool(void *node, rt_string key, int8_t def);
/// @brief Read string metadata, or a retained copy of @p def on mismatch.
rt_string rt_scene_node3d_metadata_get_string(void *node, rt_string key, rt_string def);
/// @brief Set explicit null metadata; return 0 when the key or bounded table is invalid.
int8_t rt_scene_node3d_metadata_set_null(void *node, rt_string key);
/// @brief Set integer metadata; return 0 when the key or bounded table is invalid.
int8_t rt_scene_node3d_metadata_set_int(void *node, rt_string key, int64_t value);
/// @brief Set finite float metadata; return 0 for invalid input or a full bounded table.
int8_t rt_scene_node3d_metadata_set_float(void *node, rt_string key, double value);
/// @brief Set boolean metadata; return 0 when the key or bounded table is invalid.
int8_t rt_scene_node3d_metadata_set_bool(void *node, rt_string key, int8_t value);
/// @brief Set bounded string metadata; return 0 for invalid input or a full table.
int8_t rt_scene_node3d_metadata_set_string(void *node, rt_string key, rt_string value);
/// @brief Remove metadata and report whether the key existed.
int8_t rt_scene_node3d_metadata_remove(void *node, rt_string key);
/// @brief Get the AABB minimum corner for the node subtree in this node's local space.
void *rt_scene_node3d_get_aabb_min(void *node);
/// @brief Get the AABB maximum corner for the node subtree in this node's local space.
void *rt_scene_node3d_get_aabb_max(void *node);
/// @brief Bind a physics Body3D so its transform syncs with this node's transform.
void rt_scene_node3d_bind_body(void *node, void *body);
/// @brief Remove the body binding (transform updates stop affecting the body).
void rt_scene_node3d_clear_body_binding(void *node);
/// @brief Attach this node to a skeletal bone: SyncBindings drives its world
///   transform from parentWorld x bonePose x positional offset each pass.
void rt_scene_node3d_attach_to_bone(void *node,
                                    void *animator,
                                    int64_t bone_index,
                                    double offset_x,
                                    double offset_y,
                                    double offset_z);
/// @brief Remove any bone-socket binding; the node keeps its last transform.
void rt_scene_node3d_detach_bone_socket(void *node);
/// @brief Get the bound Body3D (NULL if none).
void *rt_scene_node3d_get_body(void *node);
/// @brief Choose how node↔body sync flows (one of RT_SCENE_NODE3D_SYNC_*).
void rt_scene_node3d_set_sync_mode(void *node, int64_t sync_mode);
/// @brief Get the current sync-mode constant.
int64_t rt_scene_node3d_get_sync_mode(void *node);
/// @brief Bind an AnimController3D so its evaluated bones drive this node's child meshes.
void rt_scene_node3d_bind_animator(void *node, void *controller);
/// @brief Remove the animator binding.
void rt_scene_node3d_clear_animator_binding(void *node);
/// @brief Get the bound AnimController3D (NULL if none).
void *rt_scene_node3d_get_animator(void *node);
/// @brief Bind a NodeAnimator3D so imported node, morph-weight, object, or camera clips drive
/// this node's subtree during Scene3D.SyncBindings().
void rt_scene_node3d_bind_node_animator(void *node, void *animator);
/// @brief Remove only the bound NodeAnimator3D, leaving any skeletal AnimController3D intact.
void rt_scene_node3d_clear_node_animator_binding(void *node);
/// @brief Get the bound NodeAnimator3D (NULL if no node-animation playback is attached).
void *rt_scene_node3d_get_node_animator(void *node);

/* NodeAnimation3D / NodeAnimator3D */
/// @brief Create a NodeAnimation3D clip that targets scene-node TRS or morph-weight channels.
/// @details Node animations mirror glTF node animation semantics and are separate from
/// skeletal Animation3D clips. They are useful for object motion, camera paths, and morph target
/// weight animation.
void *rt_node_animation3d_new(rt_string name, double duration);
/// @brief Get the authored NodeAnimation3D clip name, or an empty string for invalid handles.
rt_string rt_node_animation3d_get_name(void *animation);
/// @brief Get the NodeAnimation3D duration in seconds.
double rt_node_animation3d_get_duration(void *animation);
/// @brief Get the number of channels stored on a NodeAnimation3D clip.
int64_t rt_node_animation3d_get_channel_count(void *animation);
/// @brief Create a NodeAnimator3D that owns and plays a single NodeAnimation3D clip.
void *rt_node_animator3d_new(void *clip);
/// @brief Get the number of clips retained by a NodeAnimator3D.
int64_t rt_node_animator3d_get_clip_count(void *animator);
/// @brief Borrow a clip from a NodeAnimator3D by index, or NULL when out of range.
void *rt_node_animator3d_get_clip(void *animator, int64_t index);
/// @brief Get the clip name at @p index, or an empty string when out of range.
rt_string rt_node_animator3d_get_clip_name(void *animator, int64_t index);
/// @brief Get the currently active node-animation clip name, or an empty string when none.
rt_string rt_node_animator3d_get_current_clip(void *animator);
/// @brief Play the named node-animation clip immediately and reset its playback time.
int8_t rt_node_animator3d_play(void *animator, rt_string name);
/// @brief Stop a NodeAnimator3D without clearing its clip selection or current time.
void rt_node_animator3d_stop(void *animator);
/// @brief Set the global playback speed multiplier for a NodeAnimator3D.
void rt_node_animator3d_set_speed(void *animator, double speed);
/// @brief Get the global playback speed multiplier for a NodeAnimator3D.
double rt_node_animator3d_get_speed(void *animator);
/// @brief Set the current clip time in seconds; the next update clamps or wraps it to the clip.
void rt_node_animator3d_set_time(void *animator, double time);
/// @brief Get the current clip time in seconds.
double rt_node_animator3d_get_time(void *animator);
/// @brief Return whether the NodeAnimator3D is currently advancing playback.
int8_t rt_node_animator3d_get_playing(void *animator);
/// @brief Advance and apply the current node-animation clip by @p dt seconds.
void rt_node_animator3d_update(void *animator, double dt);

/* LOD — Level of Detail */
/// @brief Add a level-of-detail mesh swap: when camera distance exceeds @p distance, use @p mesh.
void rt_scene_node3d_add_lod(void *node, double distance, void *mesh);
/// @brief Enable screen-size-driven selection across authored LOD meshes.
void rt_scene_node3d_set_auto_lod(void *node, int8_t enabled, double screen_error_px);
/// @brief Bind a generated textured impostor proxy used at or beyond @p distance.
/// @brief Mark a node as static bake input (lightmaps/probes/reflection captures).
void rt_scene_node3d_set_static(void *node, int8_t is_static);
/// @brief True when the node is flagged static for baking.
int8_t rt_scene_node3d_get_static(void *node);
void rt_scene_node3d_set_impostor(void *node, double distance, void *pixels);
/// @brief Bind a yaw-selected multi-frame impostor from an N-frame horizontal strip.
void rt_scene_node3d_set_impostor_frames(void *node, double distance, void *pixels, int64_t frames);
/// @brief Last impostor frame index selected by the draw path (0 for single-frame).
int64_t rt_scene_node3d_get_impostor_frame_index(void *node);
/// @brief Remove all LOD entries (revert to the base mesh at all distances).
void rt_scene_node3d_clear_lod(void *node);
/// @brief Number of LOD entries registered on this node.
int64_t rt_scene_node3d_get_lod_count(void *node);
/// @brief Get the camera distance threshold for the @p index-th LOD entry.
double rt_scene_node3d_get_lod_distance(void *node, int64_t index);
/// @brief Get the mesh used for the @p index-th LOD entry.
void *rt_scene_node3d_get_lod_mesh(void *node, int64_t index);
/// @brief Mark the @p index-th LOD mesh payload resident/nonresident.
void rt_scene_node3d_set_lod_resident(void *node, int64_t index, int8_t resident);
/// @brief Return whether the @p index-th LOD mesh payload is resident.
int8_t rt_scene_node3d_get_lod_resident(void *node, int64_t index);
/// @brief Return estimated resident bytes for the @p index-th LOD mesh.
int64_t rt_scene_node3d_get_lod_resident_bytes(void *node, int64_t index);

/* Scene3D — frustum culling stats */
/// @brief Number of nodes culled by the frustum during the most recent Draw (perf metric).
int64_t rt_scene3d_get_culled_count(void *scene);
/// @brief Number of drawable nodes submitted by the most recent Draw.
int64_t rt_scene3d_get_visible_node_count(void *scene);
/// @brief Number of drawable nodes skipped by the portal/PVS visibility mask in the latest Draw.
int64_t rt_scene3d_get_pvs_culled_count(void *scene);
/// @brief Number of authored visibility zones.
int64_t rt_scene3d_get_visibility_zone_count(void *scene);
/// @brief Number of directed visibility portal links.
int64_t rt_scene3d_get_visibility_portal_count(void *scene);
/// @brief Toggle portal-frustum PVS clipping (default on; off = reachability flood-fill).
void rt_scene3d_set_portal_clipping(void *scene, int8_t enabled);
/// @brief Current portal-clipping mode.
int8_t rt_scene3d_get_portal_clipping(void *scene);
/// @brief Portal expansions evaluated during the most recent PVS build.
int64_t rt_scene3d_get_portal_traversal_count(void *scene);
/// @brief Total number of nodes in the scene graph.
int64_t rt_scene3d_get_node_count(void *scene);

#ifdef __cplusplus
}
#endif
